#pragma once

#include <string>

#include "normalize.h"
#include "sql_backend.h"
#include "status.h"

namespace mmv {

struct CollationProfile {
  std::string name;
  std::string icu_locale;
  int strength = 0;
  std::string mysql_collation;
  bool mysql_sql_key = false;
  bool requires_key_column = false;
};

bool parse_collation_profile(const std::string& name, CollationProfile& out,
                             std::string& err);

// SQL expression producing a hex collation key, or empty when the dialect
// cannot produce one (PostgreSQL has no public strxfrm).
std::string sql_collation_key(const CollationProfile& p, const std::string& quoted_col,
                              Dialect d);

// Client-side ICU key, used by the ETL key generator and conformance checks.
std::string client_collation_key(const CollationProfile& p, const std::string& value);

// Starts up a small equality corpus on both backends. Returns an error if the
// two sides disagree or the target collation cannot be created.
Status check_collation_profile(SqlBackend& a, SqlBackend& b, const CollationProfile& p,
                               std::string& err);

}  // namespace mmv
