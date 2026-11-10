#include "ddl_dialect.h"

#include <cctype>
#include <string>

namespace mig {
namespace {

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

std::string strip_db_prefix(const std::string& q, const std::string& db) {
  std::string out = q;
  if (db.empty()) return out;
  const char bt = (char)96;
  replace_ci(out, std::string(1, bt) + db + std::string(1, bt) + ".", "");
  replace_ci(out, db + ".", "");
  return out;
}

std::string convert_mysql_to_pg(const std::string& q, const std::string& db) {
  std::string out = strip_db_prefix(q, db);
  // MySQL binlog DDL may carry a "use \`db\`;" prefix; the target does not need it.
  {
    std::string low = out;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    size_t p = 0;
    while (p < low.size() && std::isspace((unsigned char)low[p])) ++p;
    if (low.compare(p, 4, "use ") == 0) {
      const size_t semi = out.find(';', p);
      if (semi != std::string::npos) out = out.substr(semi + 1);
    }
    size_t s = 0;
    while (s < out.size() && std::isspace((unsigned char)out[s])) ++s;
    out = out.substr(s);
  }
  const char bt = (char)96;
  for (char& c : out) {
    if (c == bt) c = '"';
  }
  const size_t last_paren = out.rfind(')');
  if (last_paren != std::string::npos) {
    std::string tail = out.substr(last_paren + 1);
    std::string low = tail;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.find("engine=") != std::string::npos ||
        low.find("charset=") != std::string::npos ||
        low.find("collate=") != std::string::npos ||
        low.find("auto_increment=") != std::string::npos) {
      out = out.substr(0, last_paren + 1) + ";";
    }
  }
  return out;
}

class MySqlDdlDialect : public IDdlDialect {
 public:
  std::string convert(const std::string& ddl, const std::string&) override {
    return ddl;
  }
};

class PgDdlDialect : public IDdlDialect {
 public:
  std::string convert(const std::string& ddl, const std::string& db) override {
    return convert_mysql_to_pg(ddl, db);
  }
};

class OgDdlDialect : public PgDdlDialect {};

}  // namespace

std::unique_ptr<IDdlDialect> make_ddl_dialect(const std::string& target_kind) {
  if (target_kind == "mysql") return std::make_unique<MySqlDdlDialect>();
  if (target_kind == "og" || target_kind == "opengauss")
    return std::make_unique<OgDdlDialect>();
  return std::make_unique<PgDdlDialect>();
}

}  // namespace mig
