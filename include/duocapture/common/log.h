#pragma once

namespace duo {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

void set_log_level(LogLevel level) noexcept;
LogLevel log_level() noexcept;

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

}  // namespace duo

#define DUO_LOG(level, ...) ::duo::log_write((level), __FILE__, __LINE__, __VA_ARGS__)
#define DUO_TRACE(...) DUO_LOG(::duo::LogLevel::Trace, __VA_ARGS__)
#define DUO_DEBUG(...) DUO_LOG(::duo::LogLevel::Debug, __VA_ARGS__)
#define DUO_INFO(...) DUO_LOG(::duo::LogLevel::Info, __VA_ARGS__)
#define DUO_WARN(...) DUO_LOG(::duo::LogLevel::Warn, __VA_ARGS__)
#define DUO_ERROR(...) DUO_LOG(::duo::LogLevel::Error, __VA_ARGS__)
