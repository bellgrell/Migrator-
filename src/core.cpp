#include "migrator.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace mig {

std::string Status::str() const {
  if (code == Code::kOk) return "OK";
  const char* n = "Unknown";
  switch (code) {
    case Code::kOk: n = "Ok"; break;
    case Code::kNotFound: n = "NotFound"; break;
    case Code::kIO: n = "IOError"; break;
    case Code::kInvalid: n = "Invalid"; break;
    case Code::kUnsupported: n = "Unsupported"; break;
    case Code::kDb: n = "DbError"; break;
  }
  return msg.empty() ? std::string(n) : (std::string(n) + ": " + msg);
}

Value Value::Null() { return Value(); }
Value Value::Int(int64_t v) { Value x; x.type = ValType::kInt; x.i = v; return x; }
Value Value::Dbl(double v) { Value x; x.type = ValType::kDouble; x.d = v; return x; }
Value Value::Str(std::string v) { Value x; x.type = ValType::kString; x.s = std::move(v); return x; }
Value Value::Bool(bool v) { Value x; x.type = ValType::kBool; x.b = v; return x; }
Value Value::Bytes(std::string raw) { Value x; x.type = ValType::kBytes; x.s = std::move(raw); return x; }

std::string Value::sql_literal() const {
  switch (type) {
    case ValType::kNull: return "NULL";
    case ValType::kInt: return std::to_string(i);
    case ValType::kDouble: { std::ostringstream o; o << std::setprecision(10) << d; return o.str(); }
    case ValType::kBool: return b ? "true" : "false";
    case ValType::kBytes: {
      static const char* hex = "0123456789abcdef";
      std::string out;
      out += "'";
      out += '\\';
      out += "x";
      for (unsigned char c : s) { out += hex[c >> 4]; out += hex[c & 0xF]; }
      out += "'";
      return out;
    }
    case ValType::kString: {
      std::string out = "'";
      for (char c : s) { if (c == '\'') out += "''"; else out += c; }
      out += "'";
      return out;
    }
  }
  return "NULL";
}

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

std::string Value::json() const {
  switch (type) {
    case ValType::kNull: return "null";
    case ValType::kInt: return std::to_string(i);
    case ValType::kDouble: { std::ostringstream o; o << std::setprecision(10) << d; return o.str(); }
    case ValType::kBool: return b ? "true" : "false";
    case ValType::kBytes: {
      static const char* hex = "0123456789abcdef";
      std::string out = "0x";
      for (unsigned char c : s) { out += hex[c >> 4]; out += hex[c & 0xF]; }
      return out;
    }
    case ValType::kString: return "\"" + json_escape(s) + "\"";
  }
  return "null";
}

double now_sec() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// ---------- 极简 JSON 读取（只支持 {"k":{"k":"v"}}）----------
namespace {

struct MiniJson {
  const std::string& s;
  size_t p = 0;
  explicit MiniJson(const std::string& str) : s(str) {}

  void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
  bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }

  bool str(std::string& out) {
    ws();
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size()) {
      char c = s[p++];
      if (c == '"') return true;
      if (c == '\\' && p < s.size()) {
        char e = s[p++];
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          default: out.push_back(e);
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool parse(std::map<std::string, std::map<std::string, std::string>>& out) {
    if (!eat('{')) return false;
    ws();
    if (eat('}')) return true;
    while (true) {
      std::string key;
      if (!str(key)) return false;
      if (!eat(':')) return false;
      if (!eat('{')) return false;
      std::map<std::string, std::string> inner;
      ws();
      if (!eat('}')) {
        while (true) {
          std::string k, v;
          if (!str(k)) return false;
          if (!eat(':')) return false;
          if (!str(v)) return false;
          inner[k] = v;
          ws();
          if (eat(',')) continue;
          if (eat('}')) break;
          return false;
        }
      }
      out[key] = inner;
      ws();
      if (eat(',')) continue;
      if (eat('}')) break;
      return false;
    }
    return true;
  }
};

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

}  // namespace

Status TypeMap::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::Err(Code::kIO, "cannot open type map: " + path);

  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  std::map<std::string, std::map<std::string, std::string>> root;
  MiniJson j(text);
  if (!j.parse(root)) return Status::Err(Code::kInvalid, "bad JSON in " + path);

  auto g = root.find("global");
  if (g != root.end()) {
    for (auto& kv : g->second) global_map[lower(kv.first)] = kv.second;
  }
  auto c = root.find("columns");
  if (c != root.end()) {
    for (auto& kv : c->second) column_map[lower(kv.first)] = kv.second;
  }
  return Status::OK();
}

std::string TypeMap::map_type(const std::string& src_type) const {
  auto it = global_map.find(lower(src_type));
  if (it != global_map.end()) return it->second;

  // 去掉参数再试一次：varchar(64) -> varchar
  std::string base = lower(src_type);
  auto pos = base.find('(');
  if (pos != std::string::npos) {
    std::string head = base.substr(0, pos);
    std::string args = base.substr(pos);
    auto it2 = global_map.find(head);
    if (it2 != global_map.end()) {
      // 目标类型里带 {args} 就替换
      std::string dst = it2->second;
      auto at = dst.find("{args}");
      if (at != std::string::npos) dst.replace(at, 6, args);
      return dst;
    }
    return head;
  }
  return src_type;
}

std::string TypeMap::map_column(const std::string& table, const std::string& col,
                                const std::string& src_type) const {
  std::string key = lower(table + "." + col);
  auto it = column_map.find(key);
  if (it != column_map.end()) return it->second;
  return map_type(src_type);
}

std::string TypeMap::to_json() const {
  std::ostringstream o;
  o << "{\"global\":{";
  bool first = true;
  for (auto& kv : global_map) {
    if (!first) o << ",";
    first = false;
    o << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
  }
  o << "},\"columns\":{";
  first = true;
  for (auto& kv : column_map) {
    if (!first) o << ",";
    first = false;
    o << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
  }
  o << "}}";
  return o.str();
}

std::string build_ddl(const TableSchema& s) {
  std::vector<std::string> parts;
  parts.reserve(s.columns.size() + 1);

  for (const auto& c : s.columns) {
    std::string line = "  \"" + c.name + "\" " +
                       (c.dst_type.empty() ? c.src_type : c.dst_type);
    if (!c.nullable) line += " NOT NULL";
    parts.push_back(line);
  }

  if (!s.primary_key.empty()) {
    std::string pk = "  PRIMARY KEY (";
    for (size_t i = 0; i < s.primary_key.size(); ++i) {
      if (i) pk += ", ";
      pk += "\"" + s.primary_key[i] + "\"";
    }
    pk += ")";
    parts.push_back(pk);
  }

  std::string out = "CREATE TABLE IF NOT EXISTS \"" + s.name + "\" (\n";
  for (size_t i = 0; i < parts.size(); ++i) {
    out += parts[i];
    if (i + 1 < parts.size()) out += ",";
    out += "\n";
  }
  out += ");\n";
  return out;
}
}  // namespace mig
