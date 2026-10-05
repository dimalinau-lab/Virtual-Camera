#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>

inline void logDebug(const std::string& msg) {
    static std::mutex s_logMutex;
    std::lock_guard<std::mutex> lock(s_logMutex);
    std::ofstream ofs("crash_debug.log", std::ios::app);
    if (!ofs.is_open()) return;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timer);
    ofs << std::put_time(&tm, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << " " << msg << std::endl;
}
