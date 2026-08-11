# 第三方库（本地源码编译，不再 FetchContent 下载）

# ---- spdlog v1.15（静态库）----
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(spdlog)

# ---- nlohmann/json v3.11（header-only）----
add_subdirectory(nlohmann_json)
