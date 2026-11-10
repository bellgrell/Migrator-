#pragma once

#include "migrator.h"

namespace mig {

// Shared base for database backends.  Protocol-specific code stays in the
// MySQL/PostgreSQL implementations; timeout policy and error formatting live
// here so every backend behaves the same way.
class DbBase {
 public:
  explicit DbBase(const Options& o) : opts_(o) {}
  virtual ~DbBase() = default;

  int connect_timeout() const {
    return opts_.connect_timeout > 0 ? opts_.connect_timeout : 10;
  }

 protected:
  static Status db_error(const std::string& prefix,
                         const std::string& detail) {
    return Status::Err(Code::kDb, prefix + ": " + detail);
  }
  static Status db_error(const std::string& prefix) {
    return Status::Err(Code::kDb, prefix);
  }

  Options opts_;
};

}  // namespace mig
