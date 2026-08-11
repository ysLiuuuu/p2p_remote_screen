// BLE TUI 模块 — 开发板终端上的 BlueZ 扫描、连接和配网工具
#include <dbus/dbus.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr const char* BLUEZ = "org.bluez";
constexpr const char* OM_IF = "org.freedesktop.DBus.ObjectManager";
constexpr const char* ADAPTER_IF = "org.bluez.Adapter1";
constexpr const char* DEVICE_IF = "org.bluez.Device1";
constexpr const char* CHAR_IF = "org.bluez.GattCharacteristic1";
constexpr const char* SERVICE_UUID = "7b7f0000-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* DEVICE_INFO_UUID = "7b7f0001-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* NETWORK_UUID = "7b7f0002-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* PAIR_UUID = "7b7f0003-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* VIDEO_UUID = "7b7f0004-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* CONTROL_UUID = "7b7f0005-6e7d-4c8d-9f4b-8f57e34a9100";
constexpr const char* STATUS_UUID = "7b7f0006-6e7d-4c8d-9f4b-8f57e34a9100";

struct Device {
    std::string path, address, name;
    int16_t rssi = -127;
    bool connected = false;
    bool target = false;
};

void clear() { std::cout << "\033[2J\033[H"; }
void title(const std::string& text) {
    clear();
    std::cout << "\033[1;36m╔══════════════════════════════════════════════════════╗\n"
              << "║  Wireless Display · BlueZ 配网工具                  ║\n"
              << "╚══════════════════════════════════════════════════════╝\033[0m\n"
              << "\033[1m" << text << "\033[0m\n\n";
}
std::string prompt(const std::string& label, const std::string& current = {}) {
    std::cout << label;
    if (!current.empty()) std::cout << " [" << current << "]";
    std::cout << ": " << std::flush;
    std::string value;
    std::getline(std::cin, value);
    return value.empty() ? current : value;
}
void pause() {
    std::cout << "\n按 Enter 返回..." << std::flush;
    std::string line;
    std::getline(std::cin, line);
}

class Bluez {
   public:
    explicit Bluez(std::string adapter)
        : adapter_("/org/bluez/" + std::move(adapter)) {
        DBusError e;
        dbus_error_init(&e);
        conn_ = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        if (!conn_) {
            error_ = e.message ? e.message : "无法连接 system bus";
            dbus_error_free(&e);
        } else {
            dbus_connection_set_exit_on_disconnect(conn_, false);
        }
    }
    ~Bluez() {
        if (conn_) {
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
        }
    }
    bool ok() const { return conn_; }
    const std::string& error() const { return error_; }

    bool adapter_call(const char* method, std::string& why) {
        return simple_call(adapter_, ADAPTER_IF, method, why);
    }
    bool set_discovery_filter(std::string& why) {
        auto* call = dbus_message_new_method_call(
            BLUEZ, adapter_.c_str(), ADAPTER_IF, "SetDiscoveryFilter");
        DBusMessageIter root, dict, entry, variant;
        dbus_message_iter_init_append(call, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);

        const char* key = "Transport";
        const char* transport = "le";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &transport);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);

        dbus_message_iter_close_container(&root, &dict);

        auto* reply = send(call, why);
        if (!reply) return false;
        dbus_message_unref(reply);
        return true;
    }
    bool device_call(const std::string& path, const char* method, std::string& why) {
        return simple_call(path, DEVICE_IF, method, why);
    }
    bool connect_provisioning(const std::string& path, std::string& why) {
        if (!simple_call(path, DEVICE_IF, "Connect", why)) return false;
        auto* call = dbus_message_new_method_call(
            BLUEZ, path.c_str(), DEVICE_IF, "ConnectProfile");
        DBusMessageIter root;
        dbus_message_iter_init_append(call, &root);
        const char* uuid = SERVICE_UUID;
        dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING, &uuid);
        auto* reply = send(call, why, 30000);
        // 对首次发现的设备，BlueZ 可能在 ConnectProfile 时仍在解析服务；
        // 物理连接已经建立，后续轮询 GetManagedObjects 即可。
        if (!reply) return true;
        dbus_message_unref(reply);
        return true;
    }

    std::vector<Device> devices(std::string& why) {
        auto* reply = managed(why);
        std::vector<Device> result;
        if (!reply) return result;
        DBusMessageIter root, objects;
        dbus_message_iter_init(reply, &root);
        dbus_message_iter_recurse(&root, &objects);
        while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter object, interfaces;
            dbus_message_iter_recurse(&objects, &object);
            const char* path = nullptr;
            dbus_message_iter_get_basic(&object, &path);
            dbus_message_iter_next(&object);
            dbus_message_iter_recurse(&object, &interfaces);
            Device d;
            d.path = path ? path : "";
            bool is_device = false;
            while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter interface, props;
                dbus_message_iter_recurse(&interfaces, &interface);
                const char* iface = nullptr;
                dbus_message_iter_get_basic(&interface, &iface);
                dbus_message_iter_next(&interface);
                if (iface && std::string(iface) == DEVICE_IF) {
                    is_device = true;
                    dbus_message_iter_recurse(&interface, &props);
                    parse_device_props(props, d);
                }
                dbus_message_iter_next(&interfaces);
            }
            if (is_device) result.push_back(std::move(d));
            dbus_message_iter_next(&objects);
        }
        dbus_message_unref(reply);
        std::sort(result.begin(), result.end(), [](const Device& a, const Device& b) {
            if (a.target != b.target) return a.target > b.target;
            return a.rssi > b.rssi;
        });
        return result;
    }

    std::map<std::string, std::string> characteristics(const std::string& device_path,
                                                        std::string& why) {
        auto* reply = managed(why);
        std::map<std::string, std::string> result;
        if (!reply) return result;
        DBusMessageIter root, objects;
        dbus_message_iter_init(reply, &root);
        dbus_message_iter_recurse(&root, &objects);
        while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter object, interfaces;
            dbus_message_iter_recurse(&objects, &object);
            const char* path = nullptr;
            dbus_message_iter_get_basic(&object, &path);
            dbus_message_iter_next(&object);
            if (!path || std::string(path).find(device_path + "/") != 0) {
                dbus_message_iter_next(&objects);
                continue;
            }
            dbus_message_iter_recurse(&object, &interfaces);
            while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter interface, props;
                dbus_message_iter_recurse(&interfaces, &interface);
                const char* iface = nullptr;
                dbus_message_iter_get_basic(&interface, &iface);
                dbus_message_iter_next(&interface);
                if (iface && std::string(iface) == CHAR_IF) {
                    dbus_message_iter_recurse(&interface, &props);
                    std::string uuid = string_prop(props, "UUID");
                    if (!uuid.empty()) result[uuid] = path;
                }
                dbus_message_iter_next(&interfaces);
            }
            dbus_message_iter_next(&objects);
        }
        dbus_message_unref(reply);
        return result;
    }

    bool read(const std::string& path, std::string& value, std::string& why) {
        auto* call = dbus_message_new_method_call(BLUEZ, path.c_str(), CHAR_IF, "ReadValue");
        append_empty_options(call);
        auto* reply = send(call, why);
        if (!reply) return false;
        DBusMessageIter root, array;
        if (!dbus_message_iter_init(reply, &root) ||
            dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
            why = "ReadValue 返回格式错误";
            dbus_message_unref(reply);
            return false;
        }
        dbus_message_iter_recurse(&root, &array);
        value.clear();
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE) {
            unsigned char c = 0;
            dbus_message_iter_get_basic(&array, &c);
            value.push_back(static_cast<char>(c));
            dbus_message_iter_next(&array);
        }
        dbus_message_unref(reply);
        return true;
    }

    bool write(const std::string& path, const std::string& value, std::string& why) {
        auto* call = dbus_message_new_method_call(BLUEZ, path.c_str(), CHAR_IF, "WriteValue");
        DBusMessageIter root, array, options;
        dbus_message_iter_init_append(call, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "y", &array);
        for (unsigned char c : value) {
            unsigned char byte = c;
            dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE, &byte);
        }
        dbus_message_iter_close_container(&root, &array);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &options);
        dbus_message_iter_close_container(&root, &options);
        auto* reply = send(call, why);
        if (!reply) return false;
        dbus_message_unref(reply);
        return true;
    }

   private:
    DBusConnection* conn_ = nullptr;
    std::string adapter_, error_;

    static void parse_device_props(DBusMessageIter props, Device& d) {
        while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, variant;
            dbus_message_iter_recurse(&props, &entry);
            const char* key = nullptr;
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &variant);
            std::string k = key ? key : "";
            if ((k == "Name" || k == "Alias" || k == "Address") &&
                dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                const char* v = nullptr;
                dbus_message_iter_get_basic(&variant, &v);
                if (k == "Address") d.address = v ? v : "";
                else if (k == "Alias" || d.name.empty()) d.name = v ? v : "";
                if (d.name.find("WD-") == 0) d.target = true;
            } else if (k == "RSSI" &&
                       dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_INT16) {
                dbus_message_iter_get_basic(&variant, &d.rssi);
            } else if (k == "Connected" &&
                       dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
                dbus_bool_t v;
                dbus_message_iter_get_basic(&variant, &v);
                d.connected = v;
            } else if (k == "UUIDs" &&
                       dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
                DBusMessageIter a;
                dbus_message_iter_recurse(&variant, &a);
                while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRING) {
                    const char* uuid = nullptr;
                    dbus_message_iter_get_basic(&a, &uuid);
                    if (uuid && std::string(uuid) == SERVICE_UUID) d.target = true;
                    dbus_message_iter_next(&a);
                }
            }
            dbus_message_iter_next(&props);
        }
    }
    static std::string string_prop(DBusMessageIter props, const char* wanted) {
        while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, variant;
            dbus_message_iter_recurse(&props, &entry);
            const char* key = nullptr;
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &variant);
            if (key && std::string(key) == wanted &&
                dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                const char* value = nullptr;
                dbus_message_iter_get_basic(&variant, &value);
                return value ? value : "";
            }
            dbus_message_iter_next(&props);
        }
        return {};
    }
    static void append_empty_options(DBusMessage* msg) {
        DBusMessageIter root, options;
        dbus_message_iter_init_append(msg, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &options);
        dbus_message_iter_close_container(&root, &options);
    }
    DBusMessage* send(DBusMessage* call, std::string& why, int timeout = 15000) {
        DBusError e;
        dbus_error_init(&e);
        auto* reply = dbus_connection_send_with_reply_and_block(conn_, call, timeout, &e);
        dbus_message_unref(call);
        if (!reply) {
            why = e.message ? e.message : "D-Bus 请求失败";
            dbus_error_free(&e);
        }
        return reply;
    }
    bool simple_call(const std::string& path, const char* iface, const char* method,
                     std::string& why) {
        auto* call = dbus_message_new_method_call(BLUEZ, path.c_str(), iface, method);
        auto* reply = send(call, why);
        if (!reply) return false;
        dbus_message_unref(reply);
        return true;
    }
    DBusMessage* managed(std::string& why) {
        auto* call = dbus_message_new_method_call(
            BLUEZ, "/", OM_IF, "GetManagedObjects");
        return send(call, why);
    }
};

bool parse_json(const std::string& text, nlohmann::json& value, std::string& why) {
    try {
        value = nlohmann::json::parse(text);
        if (!value.is_object()) {
            why = "JSON 顶层必须是 object";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
}

std::string pretty(const std::string& raw) {
    try { return nlohmann::json::parse(raw).dump(2); }
    catch (...) { return raw; }
}

void read_screen(Bluez& bluez, const std::map<std::string, std::string>& chars,
                 const std::string& uuid, const std::string& heading) {
    title(heading);
    auto it = chars.find(uuid);
    if (it == chars.end()) {
        std::cout << "\033[31m设备未提供该特征。\033[0m\n";
    } else {
        std::string value, why;
        if (bluez.read(it->second, value, why))
            std::cout << pretty(value) << "\n";
        else
            std::cout << "\033[31m读取失败：" << why << "\033[0m\n";
    }
    pause();
}

void write_json_screen(Bluez& bluez, const std::map<std::string, std::string>& chars,
                       const std::string& uuid, const std::string& heading,
                       const nlohmann::json& initial = nlohmann::json::object()) {
    title(heading);
    auto it = chars.find(uuid);
    if (it == chars.end()) {
        std::cout << "\033[31m设备未提供该特征。\033[0m\n";
        pause();
        return;
    }
    std::cout << "输入单行 JSON";
    if (!initial.empty()) std::cout << "，直接回车使用下方内容\n" << initial.dump(2);
    std::cout << "\n\nJSON: " << std::flush;
    std::string raw;
    std::getline(std::cin, raw);
    if (raw.empty() && !initial.empty()) raw = initial.dump();
    nlohmann::json value;
    std::string why;
    if (!parse_json(raw, value, why)) {
        std::cout << "\033[31mJSON 错误：" << why << "\033[0m\n";
    } else if (bluez.write(it->second, value.dump(), why)) {
        std::cout << "\033[32m写入成功。\033[0m\n";
    } else {
        std::cout << "\033[31m写入失败：" << why << "\033[0m\n";
    }
    pause();
}

void network_form(Bluez& bluez, const std::map<std::string, std::string>& chars) {
    title("网络配网");
    auto it = chars.find(NETWORK_UUID);
    if (it == chars.end()) { std::cout << "NetworkConfig 不存在\n"; pause(); return; }
    nlohmann::json old = nlohmann::json::object();
    std::string raw, why;
    if (bluez.read(it->second, raw, why)) {
        try { old = nlohmann::json::parse(raw); } catch (...) {}
    }
    std::string role = prompt("角色 (go/gc)", old.value("role", "go"));
    std::string name = prompt("设备名称", old.value("device_name", ""));
    nlohmann::json out{{"role", role}, {"device_name", name}};
    if (role == "go") {
        std::string ip = "192.168.49.1/24";
        if (old.contains("go") && old["go"].is_object()) ip = old["go"].value("ip", ip);
        out["go"] = {{"ip", prompt("GO 地址/CIDR", ip)}};
    } else {
        std::string go_name = "WD-OPI-TX", ip = "192.168.49.100/24";
        if (old.contains("gc") && old["gc"].is_object()) {
            go_name = old["gc"].value("go_devname", go_name);
            ip = old["gc"].value("fallback_ip", ip);
        }
        out["gc"] = {{"go_devname", prompt("目标 GO 名称", go_name)},
                     {"fallback_ip", prompt("GC 地址/CIDR", ip)}};
    }
    if (bluez.write(it->second, out.dump(), why))
        std::cout << "\n\033[32m网络配置写入成功。\033[0m\n";
    else
        std::cout << "\n\033[31m写入失败：" << why << "\033[0m\n";
    pause();
}

void connected_menu(Bluez& bluez, const Device& device) {
    std::string why;
    std::map<std::string, std::string> chars;
    // 首次连接时 BlueZ 需要完成远端 GATT service discovery；部分板卡
    // 控制器在无线环境繁忙时可能需要十几秒。
    for (int i = 0; i < 60 && chars.find(NETWORK_UUID) == chars.end(); ++i) {
        chars = bluez.characteristics(device.path, why);
        if (chars.find(NETWORK_UUID) == chars.end())
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    for (;;) {
        auto refreshed = bluez.characteristics(device.path, why);
        if (!refreshed.empty()) chars = std::move(refreshed);
        title("已连接：" + device.name + "  " + device.address);
        std::cout << "  1. 查看设备信息\n"
                  << "  2. 查看网络配置\n"
                  << "  3. 表单式网络配网\n"
                  << "  4. 查看视频配置\n"
                  << "  5. 写入视频配置 JSON\n"
                  << "  6. 查看设备状态\n"
                  << "  7. 配对命令\n"
                  << "  8. 设备控制命令\n"
                  << "  9. 原始 JSON 写入 NetworkConfig\n"
                  << "  d. 断开并返回\n\n选择: " << std::flush;
        std::string cmd;
        std::getline(std::cin, cmd);
        if (cmd == "1") read_screen(bluez, chars, DEVICE_INFO_UUID, "设备信息");
        else if (cmd == "2") read_screen(bluez, chars, NETWORK_UUID, "网络配置");
        else if (cmd == "3") network_form(bluez, chars);
        else if (cmd == "4") read_screen(bluez, chars, VIDEO_UUID, "视频配置");
        else if (cmd == "5") write_json_screen(bluez, chars, VIDEO_UUID, "写入视频配置");
        else if (cmd == "6") read_screen(bluez, chars, STATUS_UUID, "设备状态");
        else if (cmd == "7") {
            std::string action = prompt("命令 (start/cancel/clear)", "start");
            write_json_screen(bluez, chars, PAIR_UUID, "配对命令",
                              {{"command", action}});
        } else if (cmd == "8") {
            std::string action = prompt("命令 (start/stop/restart/factory_reset)", "restart");
            write_json_screen(bluez, chars, CONTROL_UUID, "设备控制",
                              {{"command", action}});
        } else if (cmd == "9")
            write_json_screen(bluez, chars, NETWORK_UUID, "原始网络配置");
        else if (cmd == "d" || cmd == "q") {
            bluez.device_call(device.path, "Disconnect", why);
            return;
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    std::string adapter = argc > 1 ? argv[1] : "hci0";
    Bluez bluez(adapter);
    if (!bluez.ok()) {
        std::cerr << "连接 BlueZ 失败：" << bluez.error() << "\n";
        return 1;
    }
    std::vector<Device> devices;
    std::string message, why;
    for (;;) {
        title("设备扫描  ·  adapter=" + adapter);
        if (!message.empty()) std::cout << message << "\n\n";
        if (devices.empty()) std::cout << "尚未扫描到设备。\n";
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& d = devices[i];
            std::cout << (d.target ? "\033[1;32m" : "")
                      << "  " << (i + 1) << ". "
                      << (d.name.empty() ? "(未命名)" : d.name) << "  "
                      << d.address << "  RSSI " << d.rssi
                      << (d.target ? "  [配网设备]" : "")
                      << (d.connected ? "  [已连接]" : "") << "\033[0m\n";
        }
        std::cout << "\n  s. 扫描 8 秒    r. 刷新列表    q. 退出\n"
                  << "  输入设备编号连接\n\n选择: " << std::flush;
        std::string cmd;
        std::getline(std::cin, cmd);
        if (cmd == "q") break;
        if (cmd == "s") {
            message = "正在扫描……";
            bluez.set_discovery_filter(why);
            bluez.adapter_call("StartDiscovery", why);
            std::this_thread::sleep_for(std::chrono::seconds(8));
            devices = bluez.devices(why);
            bluez.adapter_call("StopDiscovery", why);
            message = "扫描完成，共发现 " + std::to_string(devices.size()) + " 个设备。";
        } else if (cmd == "r") {
            devices = bluez.devices(why);
            message = why.empty() ? "列表已刷新。" : why;
        } else {
            try {
                size_t index = std::stoul(cmd);
                if (index == 0 || index > devices.size()) throw std::out_of_range("index");
                Device selected = devices[index - 1];
                title("连接 " + selected.name);
                std::cout << "正在连接 " << selected.address << " …\n";
                if (bluez.connect_provisioning(selected.path, why)) {
                    connected_menu(bluez, selected);
                    message = "设备已断开。";
                } else {
                    message = "\033[31m连接失败：" + why + "\033[0m";
                    pause();
                }
            } catch (...) {
                message = "\033[31m无效选择。\033[0m";
            }
        }
    }
    clear();
    return 0;
}
