#include "logger.h"

namespace mig {

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::write(LogLevel level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(mu_);
  if (sink_) sink_(level, msg);
}

void Logger::set_sink(std::function<void(LogLevel, const std::string&)> sink) {
  std::lock_guard<std::mutex> lk(mu_);
  sink_ = std::move(sink);
}

}  // namespace mig
