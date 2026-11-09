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

}  // namespace

// MySQL 的 CREATE VIEW -> PostgreSQL 可执行的 CREATE OR REPLACE VIEW
// 只做安全的机械转换，复杂函数仍需人工改写
std::string convert_view_sql(const std::string& mysql_sql) {
  std::string low = mysql_sql;
  for (char& c : low) c = (char)std::tolower((unsigned char)c);

  // 去掉 ALGORITHM=... DEFINER=... SQL SECURITY ...，从 VIEW 开始
  std::string body = mysql_sql;
  const size_t pos = low.find(" view ");
  if (pos != std::string::npos) body = mysql_sql.substr(pos + 1);

  std::string out = "CREATE OR REPLACE " + body;

  // 反引号 -> 双引号
  for (char& c : out) {
    if (c == kBacktick) c = '"';
  }

  // 常见函数映射
  replace_ci(out, "IFNULL(", "COALESCE(");
  replace_ci(out, "NOW()", "now()");
  replace_ci(out, "CURDATE()", "current_date");
  replace_ci(out, "LENGTH(", "length(");

  return out;
}

}  // namespace mig
