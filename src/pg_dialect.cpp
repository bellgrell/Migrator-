#include "pg_dialect.h"

namespace mig {

bool PgDialect::is_open_gauss(const std::string& version) {
  return version.find("openGauss") != std::string::npos ||
         version.find("GaussDB") != std::string::npos;
}

const char* PgDialect::name(bool og) {
  return og ? "openGauss" : "PostgreSQL";
}

std::string PgDialect::snapshot_file(bool og) {
  return og ? "opengauss-snapshot" : "postgres-snapshot";
}

std::string PgDialect::compat_sql(bool og) {
  return og ? "SET behavior_compat_options = 'display_leading_zero'" : "";
}

bool PgDialect::supports_upsert(bool og) { return !og; }

}  // namespace mig
