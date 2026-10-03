#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>

// Local timestamp via localtime_r/strftime: std::chrono::zoned_time (used by
// upstream chess-3d) is not available in Apple's libc++.
void log(std::string_view tag, std::string_view message,
         const std::source_location& source) {
  const auto now{std::chrono::system_clock::now()};
  const std::time_t t{std::chrono::system_clock::to_time_t(now)};
  const auto ms{std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) %
                1000};
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char stamp[32]{};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

  char millis[8]{};
  std::snprintf(millis, sizeof(millis), "%03d", static_cast<int>(ms.count()));
  std::cerr << '[' << stamp << '.' << millis << "] [" << tag << "] " << message << " ("
            << std::filesystem::path{source.file_name()}.filename().string() << ':'
            << source.line() << ")\n";
}
