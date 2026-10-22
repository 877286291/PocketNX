#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace pnx {

// Lightweight leveled logger. Backend is swappable (stderr now, OSLog/file later).
enum class LogLevel { Trace, Debug, Info, Warn, Error };

struct Log {
    static void SetLevel(LogLevel lvl);
    static LogLevel Level();

    // Core entry. Format like printf.
    static void Write(LogLevel lvl, const char* tag, const char* fmt, ...);
};

#define PNX_LOG_INFO(tag, ...)  ::pnx::Log::Write(::pnx::LogLevel::Info,  tag, __VA_ARGS__)
#define PNX_LOG_WARN(tag, ...)  ::pnx::Log::Write(::pnx::LogLevel::Warn,  tag, __VA_ARGS__)
#define PNX_LOG_ERROR(tag, ...) ::pnx::Log::Write(::pnx::LogLevel::Error, tag, __VA_ARGS__)
#define PNX_LOG_DEBUG(tag, ...) ::pnx::Log::Write(::pnx::LogLevel::Debug, tag, __VA_ARGS__)

} // namespace pnx
