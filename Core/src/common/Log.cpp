#include "common/Log.h"

#include <atomic>

namespace pnx {

namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};

const char* TagString(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Trace: return "TRC";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}
} // namespace

void Log::SetLevel(LogLevel lvl) { g_level.store(lvl, std::memory_order_relaxed); }

LogLevel Log::Level() { return g_level.load(std::memory_order_relaxed); }

void Log::Write(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (static_cast<int>(lvl) < static_cast<int>(g_level.load(std::memory_order_relaxed))) {
        return;
    }
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::fprintf(stderr, "[%s] [%s] %s\n", TagString(lvl), tag, msg);
}

} // namespace pnx
