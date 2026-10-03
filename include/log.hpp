#pragma once

#include <source_location>
#include <sstream>
#include <string>
#include <string_view>

// #define DISABLE_LOGGING

// Minimal "{}" formatter for log lines. std::format is avoided on purpose:
// older Apple toolchains (Xcode 15 / libc++ 16) ship it as experimental-only.
// Each "{...}" placeholder is replaced by the next argument via operator<<
// (format specs inside the braces are ignored).
namespace logfmt {
inline void append(std::ostringstream& out, std::string_view& fmt) { out << fmt; fmt = {}; }

template <typename T, typename... Rest>
void append(std::ostringstream& out, std::string_view& fmt, const T& value, const Rest&... rest) {
  const auto open{fmt.find('{')};
  if (open == std::string_view::npos) {
    out << fmt;
    fmt = {};
    return;
  }
  const auto close{fmt.find('}', open)};
  out << fmt.substr(0, open) << value;
  fmt.remove_prefix(close == std::string_view::npos ? fmt.size() : close + 1);
  append(out, fmt, rest...);
}

template <typename... Args>
std::string format(std::string_view fmt, const Args&... args) {
  std::ostringstream out;
  append(out, fmt, args...);
  return out.str();
}
}  // namespace logfmt

#ifdef DISABLE_LOGGING
#define LOG(tag, message)
#define LOGF(tag, fmt, ...)
#else
#define LOG(tag, message) log(tag, message)
#define LOGF(tag, fmt, ...) log(tag, logfmt::format(fmt, __VA_ARGS__))
#endif

void log(std::string_view tag, std::string_view message,
         const std::source_location& source = std::source_location::current());
