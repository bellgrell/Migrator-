#pragma once

#include <functional>
#include <mutex>
#include <string>

namespace mig {

enum class LogLevel { kDebug, kInfo, kWarn, kError };

// Single logging entry point.  Metrics::log delegates here so future sinks
// (console, file, webhook) do not require touching business code.
class Logger {
 public:
  static Logger& instance();
  void write(LogLevel level, const std::string& msg);
  void set_sink(std::function<void(LogLevel, const std::string&)> sink);

 private:
  Logger() = default;
  std::mutex mu_;
  std::function<void(LogLevel, const std::string&)> sink_;
};

}  // namespace mig
