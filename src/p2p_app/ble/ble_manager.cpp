#include "ble/ble_manager.h"

#include <dbus/dbus.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr const char* BLUEZ = "org.bluez";
constexpr const char* ROOT = "/com/wireless_display";
constexpr const char* SERVICE = "/com/wireless_display/service0";
constexpr const char* ADV = "/com/wireless_display/advertisement0";
constexpr const char* SERVICE_IF = "org.bluez.GattService1";
constexpr const char* CHAR_IF = "org.bluez.GattCharacteristic1";
constexpr const char* ADV_IF = "org.bluez.LEAdvertisement1";
constexpr const char* PROPS_IF = "org.freedesktop.DBus.Properties";
constexpr const char* OBJMGR_IF = "org.freedesktop.DBus.ObjectManager";

struct CharDef {
    const char* name;
    const char* uuid;
    const char* path;
    bool read, write, notify;
};
constexpr std::array<CharDef, 6> CHARS{{
    {"DeviceInfo", "7b7f0001-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char0", true, false, false},
    {"NetworkConfig", "7b7f0002-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char1", true, true, false},
    {"PairCommand", "7b7f0003-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char2", false, true, false},
    {"VideoConfig", "7b7f0004-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char3", true, true, false},
    {"DeviceControl", "7b7f0005-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char4", false, true, false},
    {"Status", "7b7f0006-6e7d-4c8d-9f4b-8f57e34a9100",
     "/com/wireless_display/service0/char5", true, false, true},
}};

const CharDef* find_char(const char* path) {
    if (!path) return nullptr;
    for (const auto& c : CHARS)
        if (std::string(c.path) == path) return &c;
    return nullptr;
}
void str(DBusMessageIter* it, const char* v) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &v);
}
void obj(DBusMessageIter* it, const char* v) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &v);
}
void boolean(DBusMessageIter* it, bool v) {
    dbus_bool_t b = v;
    dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &b);
}
template <class F>
void variant(DBusMessageIter* it, const char* sig, F fn) {
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v);
    fn(&v);
    dbus_message_iter_close_container(it, &v);
}
template <class F>
void property(DBusMessageIter* dict, const char* key, const char* sig, F fn) {
    DBusMessageIter e;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    str(&e, key);
    variant(&e, sig, fn);
    dbus_message_iter_close_container(dict, &e);
}
void strings(DBusMessageIter* it, const std::vector<std::string>& values) {
    DBusMessageIter a;
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "s", &a);
    for (const auto& v : values) str(&a, v.c_str());
    dbus_message_iter_close_container(it, &a);
}
void bytes(DBusMessageIter* it, const std::string& value) {
    DBusMessageIter a;
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "y", &a);
    for (unsigned char c : value) {
        unsigned char v = c;
        dbus_message_iter_append_basic(&a, DBUS_TYPE_BYTE, &v);
    }
    dbus_message_iter_close_container(it, &a);
}
std::string get_bytes(DBusMessageIter* it) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(it) != DBUS_TYPE_BYTE)
        throw std::runtime_error("Value 必须是 byte array");
    DBusMessageIter a;
    dbus_message_iter_recurse(it, &a);
    std::string out;
    while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_BYTE) {
        unsigned char c;
        dbus_message_iter_get_basic(&a, &c);
        out.push_back(static_cast<char>(c));
        dbus_message_iter_next(&a);
    }
    return out;
}
DBusMessage* error(DBusMessage* msg, const char* name, const std::string& text) {
    return dbus_message_new_error(msg, name, text.c_str());
}
bool save_atomic(const std::string& path, const nlohmann::json& j) {
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    std::string data = j.dump(2) + "\n";
    bool ok = write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()) &&
              fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) unlink(tmp.c_str());
    return ok;
}
}  // namespace

struct BleManager::Impl {
    DBusConnection* conn = nullptr;
    std::string adapter_path, local_name, config_path;
    WriteCallback callback;
    std::mutex mutex;
    nlohmann::json config;
    nlohmann::json status = {{"network", "idle"}, {"media", "idle"}, {"error", nullptr}};
    std::array<bool, CHARS.size()> notifying{};

    ~Impl() {
        if (conn) dbus_connection_unref(conn);
    }

    std::string value(const CharDef& c) {
        std::lock_guard<std::mutex> lock(mutex);
        std::string name = c.name;
        if (name == "DeviceInfo")
            return nlohmann::json{{"name", config.value("device_name", local_name)},
                                  {"role", config.value("role", "go")},
                                  {"model", "wireless-display"},
                                  {"version", "1.0.0"}}.dump();
        if (name == "NetworkConfig") {
            nlohmann::json j{{"device_name", config.value("device_name", "")},
                             {"role", config.value("role", "go")}};
            if (config.contains("go")) j["go"] = config["go"];
            if (config.contains("gc")) j["gc"] = config["gc"];
            return j.dump();
        }
        if (name == "VideoConfig")
            return config.value("video", nlohmann::json::object()).dump();
        return status.dump();
    }

    void char_props(DBusMessageIter* d, const CharDef& c) {
        property(d, "UUID", "s", [&](auto* v) { str(v, c.uuid); });
        property(d, "Service", "o", [&](auto* v) { obj(v, SERVICE); });
        std::vector<std::string> flags;
        if (c.read) flags.emplace_back("read");
        if (c.write) flags.emplace_back("write");
        if (c.notify) flags.emplace_back("notify");
        property(d, "Flags", "as", [&](auto* v) { strings(v, flags); });
        if (c.notify)
            property(d, "Notifying", "b", [&](auto* v) {
                boolean(v, notifying[static_cast<size_t>(&c - CHARS.data())]);
            });
    }
    void props(DBusMessageIter* d, const char* path, const char* iface) {
        if (std::string(iface) == SERVICE_IF && std::string(path) == SERVICE) {
            property(d, "UUID", "s", [&](auto* v) { str(v, BleManager::service_uuid()); });
            property(d, "Primary", "b", [&](auto* v) { boolean(v, true); });
        } else if (std::string(iface) == CHAR_IF) {
            if (const auto* c = find_char(path)) char_props(d, *c);
        } else if (std::string(iface) == ADV_IF && std::string(path) == ADV) {
            property(d, "Type", "s", [&](auto* v) { str(v, "peripheral"); });
            property(d, "Discoverable", "b", [&](auto* v) { boolean(v, true); });
            property(d, "ServiceUUIDs", "as",
                     [&](auto* v) { strings(v, {BleManager::service_uuid()}); });
            property(d, "LocalName", "s", [&](auto* v) { str(v, local_name.c_str()); });
        }
    }
    void interface(DBusMessageIter* a, const char* path, const char* iface) {
        DBusMessageIter e, d;
        dbus_message_iter_open_container(a, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        str(&e, iface);
        dbus_message_iter_open_container(&e, DBUS_TYPE_ARRAY, "{sv}", &d);
        props(&d, path, iface);
        dbus_message_iter_close_container(&e, &d);
        dbus_message_iter_close_container(a, &e);
    }
    void object(DBusMessageIter* a, const char* path, const char* iface) {
        DBusMessageIter e, interfaces;
        dbus_message_iter_open_container(a, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        obj(&e, path);
        dbus_message_iter_open_container(&e, DBUS_TYPE_ARRAY, "{sa{sv}}", &interfaces);
        interface(&interfaces, path, iface);
        dbus_message_iter_close_container(&e, &interfaces);
        dbus_message_iter_close_container(a, &e);
    }
    DBusMessage* managed(DBusMessage* msg) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter root, a;
        dbus_message_iter_init_append(reply, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &a);
        object(&a, SERVICE, SERVICE_IF);
        for (const auto& c : CHARS) object(&a, c.path, CHAR_IF);
        dbus_message_iter_close_container(&root, &a);
        return reply;
    }
    DBusMessage* get_all(DBusMessage* msg, const char* path) {
        const char* iface = nullptr;
        if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID))
            return error(msg, DBUS_ERROR_INVALID_ARGS, "缺少 interface");
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter root, d;
        dbus_message_iter_init_append(reply, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &d);
        props(&d, path, iface);
        dbus_message_iter_close_container(&root, &d);
        return reply;
    }
    DBusMessage* get(DBusMessage* msg, const char* path) {
        const char *iface = nullptr, *key = nullptr;
        if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &key,
                                   DBUS_TYPE_INVALID))
            return error(msg, DBUS_ERROR_INVALID_ARGS, "缺少 interface/property");
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter root;
        dbus_message_iter_init_append(reply, &root);
        bool found = false;
        auto emit = [&](const char* sig, auto fn) { variant(&root, sig, fn); found = true; };
        if (std::string(iface) == SERVICE_IF && std::string(key) == "UUID")
            emit("s", [&](auto* v) { str(v, BleManager::service_uuid()); });
        else if (std::string(iface) == SERVICE_IF && std::string(key) == "Primary")
            emit("b", [&](auto* v) { boolean(v, true); });
        else if (const auto* c = find_char(path); c && std::string(iface) == CHAR_IF) {
            if (std::string(key) == "UUID") emit("s", [&](auto* v) { str(v, c->uuid); });
            else if (std::string(key) == "Service") emit("o", [&](auto* v) { obj(v, SERVICE); });
            else if (std::string(key) == "Flags") {
                std::vector<std::string> flags;
                if (c->read) flags.emplace_back("read");
                if (c->write) flags.emplace_back("write");
                if (c->notify) flags.emplace_back("notify");
                emit("as", [&](auto* v) { strings(v, flags); });
            } else if (std::string(key) == "Notifying")
                emit("b", [&](auto* v) {
                    boolean(v, notifying[static_cast<size_t>(c - CHARS.data())]);
                });
        } else if (std::string(iface) == ADV_IF) {
            if (std::string(key) == "Type") emit("s", [&](auto* v) { str(v, "peripheral"); });
            else if (std::string(key) == "Discoverable")
                emit("b", [&](auto* v) { boolean(v, true); });
            else if (std::string(key) == "ServiceUUIDs")
                emit("as", [&](auto* v) { strings(v, {BleManager::service_uuid()}); });
            else if (std::string(key) == "LocalName")
                emit("s", [&](auto* v) { str(v, local_name.c_str()); });
        }
        if (!found) {
            dbus_message_unref(reply);
            return error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, key);
        }
        return reply;
    }
    bool valid(const CharDef& c, const nlohmann::json& j, std::string& why) {
        if (!j.is_object()) { why = "JSON 顶层必须是 object"; return false; }
        std::string name = c.name;
        if (name == "NetworkConfig") {
            if (j.contains("role") && (!j["role"].is_string() ||
                (j["role"] != "go" && j["role"] != "gc"))) {
                why = "role 必须为 go 或 gc"; return false;
            }
            if (j.contains("device_name") && (!j["device_name"].is_string() ||
                j["device_name"].get<std::string>().size() > 64)) {
                why = "device_name 非法"; return false;
            }
        } else if (name == "PairCommand") {
            auto cmd = j.value("command", "");
            if (cmd != "start" && cmd != "cancel" && cmd != "clear") {
                why = "command 必须为 start/cancel/clear"; return false;
            }
        } else if (name == "DeviceControl") {
            auto cmd = j.value("command", "");
            if (cmd != "start" && cmd != "stop" && cmd != "restart" &&
                cmd != "factory_reset") {
                why = "command 必须为 start/stop/restart/factory_reset"; return false;
            }
        }
        return true;
    }
    DBusMessage* read_value(DBusMessage* msg, const CharDef& c) {
        if (!c.read) return error(msg, "org.bluez.Error.NotPermitted", "该特征不可读");
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter root;
        dbus_message_iter_init_append(reply, &root);
        bytes(&root, value(c));
        return reply;
    }
    DBusMessage* write_value(DBusMessage* msg, const CharDef& c) {
        if (!c.write) return error(msg, "org.bluez.Error.NotPermitted", "该特征不可写");
        DBusMessageIter args;
        if (!dbus_message_iter_init(msg, &args))
            return error(msg, "org.bluez.Error.InvalidValueLength", "缺少 Value");
        try {
            std::string raw = get_bytes(&args);
            if (raw.size() > 4096)
                return error(msg, "org.bluez.Error.InvalidValueLength", "JSON 超过 4096 字节");
            auto j = nlohmann::json::parse(raw);
            std::string why;
            if (!valid(c, j, why)) return error(msg, "org.bluez.Error.InvalidArguments", why);
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::string name = c.name;
                if (name == "NetworkConfig") {
                    for (auto key : {"role", "device_name", "go", "gc"})
                        if (j.contains(key)) config[key] = j[key];
                } else if (name == "VideoConfig") config["video"] = j;
                if ((name == "NetworkConfig" || name == "VideoConfig") &&
                    !save_atomic(config_path, config))
                    return error(msg, "org.bluez.Error.Failed", "配置保存失败");
            }
            spdlog::info("[ble] {} 写入: {}", c.name, j.dump());
            if (callback) callback(c.name, j);
            return dbus_message_new_method_return(msg);
        } catch (const std::exception& e) {
            return error(msg, "org.bluez.Error.InvalidArguments", e.what());
        }
    }
    void emit_status() {
        if (!conn || !notifying.back()) return;
        auto* signal = dbus_message_new_signal(CHARS.back().path, PROPS_IF, "PropertiesChanged");
        DBusMessageIter root, changed, invalid;
        dbus_message_iter_init_append(signal, &root);
        str(&root, CHAR_IF);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &changed);
        std::string v = value(CHARS.back());
        property(&changed, "Value", "ay", [&](auto* i) { bytes(i, v); });
        dbus_message_iter_close_container(&root, &changed);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "s", &invalid);
        dbus_message_iter_close_container(&root, &invalid);
        dbus_connection_send(conn, signal, nullptr);
        dbus_message_unref(signal);
    }
    DBusHandlerResult handle(DBusMessage* msg) {
        const char* path = dbus_message_get_path(msg);
        DBusMessage* reply = nullptr;
        if (dbus_message_is_method_call(msg, OBJMGR_IF, "GetManagedObjects") &&
            std::string(path ? path : "") == ROOT) reply = managed(msg);
        else if (dbus_message_is_method_call(msg, PROPS_IF, "GetAll")) reply = get_all(msg, path);
        else if (dbus_message_is_method_call(msg, PROPS_IF, "Get")) reply = get(msg, path);
        else if (const auto* c = find_char(path); c) {
            if (dbus_message_is_method_call(msg, CHAR_IF, "ReadValue"))
                reply = read_value(msg, *c);
            else if (dbus_message_is_method_call(msg, CHAR_IF, "WriteValue"))
                reply = write_value(msg, *c);
            else if (dbus_message_is_method_call(msg, CHAR_IF, "StartNotify") && c->notify) {
                notifying[static_cast<size_t>(c - CHARS.data())] = true;
                reply = dbus_message_new_method_return(msg);
            } else if (dbus_message_is_method_call(msg, CHAR_IF, "StopNotify") && c->notify) {
                notifying[static_cast<size_t>(c - CHARS.data())] = false;
                reply = dbus_message_new_method_return(msg);
            }
        } else if (dbus_message_is_method_call(msg, ADV_IF, "Release"))
            reply = dbus_message_new_method_return(msg);
        if (!reply) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    static DBusHandlerResult dispatch(DBusConnection*, DBusMessage* msg, void* data) {
        return static_cast<Impl*>(data)->handle(msg);
    }
    bool path(const char* p) {
        static DBusObjectPathVTable table = {
            nullptr, &Impl::dispatch, nullptr, nullptr, nullptr, nullptr};
        return dbus_connection_register_object_path(conn, p, &table, this);
    }
    bool reg(const char* iface, const char* method, const char* path) {
        auto* call = dbus_message_new_method_call(BLUEZ, adapter_path.c_str(), iface, method);
        DBusMessageIter root, options;
        dbus_message_iter_init_append(call, &root);
        obj(&root, path);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &options);
        dbus_message_iter_close_container(&root, &options);
        // 注册请求必须异步发送。BlueZ 会在回复前反向调用本连接导出的
        // GetManagedObjects/GetAll，等待回复会与 libdbus connection 锁互锁。
        bool sent = dbus_connection_send(conn, call, nullptr);
        dbus_message_unref(call);
        if (!sent) {
            spdlog::error("[ble] {} 请求发送失败", method);
            return false;
        }
        dbus_connection_flush(conn);
        return true;
    }
    void unreg(const char* iface, const char* method, const char* path) {
        auto* call = dbus_message_new_method_call(BLUEZ, adapter_path.c_str(), iface, method);
        DBusMessageIter root;
        dbus_message_iter_init_append(call, &root);
        obj(&root, path);
        dbus_connection_send(conn, call, nullptr);
        dbus_message_unref(call);
        dbus_connection_flush(conn);
    }
};

BleManager::BleManager() : impl_(std::make_unique<Impl>()) {}
BleManager::~BleManager() { stop(); }

bool BleManager::start(const std::string& adapter, const std::string& name,
                       const std::string& config_path, WriteCallback callback) {
    if (running_) return true;
    impl_->adapter_path = "/org/bluez/" + adapter;
    impl_->local_name = name.substr(0, 26);
    impl_->config_path = config_path;
    impl_->callback = std::move(callback);
    std::ifstream in(config_path);
    try { if (in) in >> impl_->config; }
    catch (const std::exception& e) {
        spdlog::error("[ble] 配置读取失败: {}", e.what()); return false;
    }
    dbus_threads_init_default();
    DBusError e;
    dbus_error_init(&e);
    impl_->conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!impl_->conn) {
        spdlog::error("[ble] 连接 system bus 失败: {}", e.message ? e.message : "unknown");
        dbus_error_free(&e); return false;
    }
    dbus_connection_set_exit_on_disconnect(impl_->conn, false);
    if (!impl_->path(ROOT) || !impl_->path(SERVICE) || !impl_->path(ADV)) return false;
    for (const auto& c : CHARS) if (!impl_->path(c.path)) return false;

    // RegisterApplication 期间 BlueZ 会同步回调 GetManagedObjects；必须先有
    // 独立线程分发入站消息，否则双方会互相等待直至超时。
    running_ = true;
    thread_ = std::thread([this] {
        while (running_ && dbus_connection_get_is_connected(impl_->conn))
            dbus_connection_read_write_dispatch(impl_->conn, 500);
        if (running_) { running_ = false; spdlog::error("[ble] D-Bus 连接断开"); }
    });
    if (!impl_->reg("org.bluez.GattManager1", "RegisterApplication", ROOT) ||
        !impl_->reg("org.bluez.LEAdvertisingManager1", "RegisterAdvertisement", ADV)) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return false;
    }
    spdlog::info("[ble] GATT 配网服务启动: name={} uuid={}", impl_->local_name,
                 service_uuid());
    return true;
}
void BleManager::stop() {
    if (!impl_) return;
    bool active = running_.exchange(false);
    if (active && impl_->conn) {
        impl_->unreg("org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement", ADV);
        impl_->unreg("org.bluez.GattManager1", "UnregisterApplication", ROOT);
    }
    if (thread_.joinable()) thread_.join();
    if (impl_->conn) {
        dbus_connection_close(impl_->conn);
        dbus_connection_unref(impl_->conn);
        impl_->conn = nullptr;
    }
}
void BleManager::update_status(const nlohmann::json& status) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->status = status;
    }
    impl_->emit_status();
}
