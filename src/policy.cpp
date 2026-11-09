#include "migrator.h"

#include <fstream>
#include <sstream>
#include <string>

namespace mig {
namespace {

std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TablePolicy parse_value(const std::string& v) {
  TablePolicy p;
  if (v == "skip") {
    p.strategy = "skip";
  } else if (v.rfind("rebuild", 0) == 0) {
    p.strategy = "rebuild";
    const size_t colon = v.find(':');
    if (colon != std::string::npos) p.rebuild_sql = v.substr(colon + 1);
  } else {
    p.strategy = "copy";
  }
  return p;
}

}  // namespace

Status load_table_policies(const std::string& path,
                           std::unordered_map<std::string, TablePolicy>& out) {
  out.clear();
  const std::string s = read_all(path);
  if (s.empty()) return Status::OK();  // 没有配置文件 = 全部 copy

  const size_t tables_pos = s.find("\"tables\"");
  if (tables_pos == std::string::npos) return Status::OK();

  const size_t open = s.find('{', tables_pos);
  const size_t close = s.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close <= open) {
    return Status::Err(Code::kInvalid, "bad table policy file: " + path);
  }

  const std::string body = s.substr(open + 1, close - open - 1);

  size_t i = 0;
  while (i < body.size()) {
    const size_t q1 = body.find('"', i);
    if (q1 == std::string::npos) break;
    const size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    const std::string name = body.substr(q1 + 1, q2 - q1 - 1);

    const size_t colon = body.find(':', q2 + 1);
    if (colon == std::string::npos) break;
    const size_t v1 = body.find('"', colon + 1);
    if (v1 == std::string::npos) break;
    const size_t v2 = body.find('"', v1 + 1);
    if (v2 == std::string::npos) break;
    const std::string value = body.substr(v1 + 1, v2 - v1 - 1);

    out[name] = parse_value(value);
    i = v2 + 1;
  }
  return Status::OK();
}

std::string build_index_ddl(const std::string& table, const IndexDef& idx) {
  std::ostringstream o;
  o << "CREATE " << (idx.unique ? "UNIQUE " : "") << "INDEX IF NOT EXISTS \""
    << idx.name << "\" ON \"" << table << "\" (";
  for (size_t i = 0; i < idx.columns.size(); ++i) {
    if (i) o << ", ";
    o << "\"" << idx.columns[i] << "\"";
  }
  o << ");";
  return o.str();
}

}  // namespace mig
