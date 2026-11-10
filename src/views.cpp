#include "migrator.h"

#include <cctype>
#include <string>

namespace mig {
namespace {

const char kBacktick = (char)96;

void replace_ci(std::string& s, const std::string& from, const std::string& to) {
  std::string low = s;
  for (char& c : low) c = (char)std::tolower((unsigned char)c);
  std::string f = from;
  for (char& c : f) c = (char)std::tolower((unsigned char)c);

  size_t pos = 0;
  while ((pos = low.find(f, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    low.replace(pos, from.size(), to);
    pos += to.size();
  }
}

void backticks_to_quotes(std::string& s) {
  for (char& c : s)
    if (c == kBacktick) c = '"';
}

void quotes_to_backticks(std::string& s) {
  for (char& c : s)
    if (c == '"') c = kBacktick;
}

}  // namespace

// Mechanical view conversion. Complex expressions still need manual review.
std::string convert_view_sql(const std::string& sql,
                             const std::string& source_kind,
                             const std::string& target_kind) {
  if (source_kind == target_kind) return sql;

  const bool src_mysql = source_kind == "mysql";
  const bool dst_mysql = target_kind == "mysql";

  if (src_mysql && !dst_mysql) {
    // MySQL CREATE VIEW -> PostgreSQL/openGauss CREATE OR REPLACE VIEW
    std::string low = sql;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);

    std::string body = sql;
    const size_t pos = low.find(" view ");
    if (pos != std::string::npos) body = sql.substr(pos + 1);

    std::string out = "CREATE OR REPLACE " + body;
    backticks_to_quotes(out);

    replace_ci(out, "IFNULL(", "COALESCE(");
    replace_ci(out, "NOW()", "now()");
    replace_ci(out, "CURDATE()", "current_date");
    replace_ci(out, "LENGTH(", "length(");
    return out;
  }

  if (!src_mysql && dst_mysql) {
    // PostgreSQL/openGauss view -> MySQL
    std::string out = sql;
    quotes_to_backticks(out);

    replace_ci(out, "now()", "NOW()");
    replace_ci(out, "current_date", "CURDATE()");
    replace_ci(out, "COALESCE(", "IFNULL(");

    // Drop simple PostgreSQL casts that MySQL does not understand.
    for (const char* t : {"::integer", "::bigint", "::smallint", "::numeric",
                          "::text", "::boolean", "::double precision",
                          "::real"}) {
      replace_ci(out, t, "");
    }
    return out;
  }

  // PG <-> openGauss: both use the same view dialect.
  return sql;
}

}  // namespace mig
