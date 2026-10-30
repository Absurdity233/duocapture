#include "duocapture/common/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace duo {
namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};

const char* level_name(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?????";
}

const char* base_name(const char* path) noexcept {
  const char* base = path;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  return base;
}

}  // namespace

void set_log_level(LogLevel level) noexcept { g_level.store(level, std::memory_order_relaxed); }

LogLevel log_level() noexcept { return g_level.load(std::memory_order_relaxed); }

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) {
  if (static_cast<int>(level) < static_cast<int>(g_level.load(std::memory_order_relaxed))) {
    return;
  }

  char message[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  std::fprintf(stderr, "[%s] %s:%d %s\n", level_name(level), base_name(file), line, message);
  std::fflush(stderr);
}

}  // namespace duo
