// Utils 模块 — spdlog 初始化封装（规范化日志：级别、时间戳、颜色）
#pragma once
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// 初始化默认 logger（控制台彩色输出）
// level: "trace"/"debug"/"info"/"warning"/"error"/"critical"
inline void init_logger(const char* level = "debug") {
    auto logger = spdlog::stdout_color_mt("p2p");
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->set_level(spdlog::level::from_str(level));
    spdlog::set_default_logger(logger);
}
