#pragma once

#include <string>

namespace mig {

// PostgreSQL/openGauss differences in one place.  Backends still share the
// libpq implementation; this strategy keeps the dialect decisions out of the
// connection/DDL code.
class PgDialect {
 public:
  static bool is_open_gauss(const std::string& version);
  static const char* name(bool og);
  static std::string snapshot_file(bool og);
  static std::string compat_sql(bool og);
  static bool supports_upsert(bool og);
};

}  // namespace mig
