#include "migrator.h"

#include <cstdio>
#include <string>
#include <vector>

namespace mig {
namespace {

uint64_t fnv1a(const std::string& s, uint64_t h) {
  for (unsigned char c : s) {
    h ^= (uint64_t)c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string canonical(const Value& v) {
  switch (v.type) {
    case ValType::kNull: return std::string("\x01", 1);
    case ValType::kInt: return std::string("\x02", 1) + std::to_string(v.i);
    case ValType::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.10f", v.d);
      std::string s(buf);
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      if (s == "-0") s = "0";
      return std::string("\x03", 1) + s;
    }
    // bool 与 0/1 整数归一到同一形态：PG bool 和 MySQL tinyint(1) 才能比对
    case ValType::kBool: return std::string("\x02", 1) + (v.b ? "1" : "0");
    case ValType::kBytes: {
      static const char* hex = "0123456789abcdef";
      std::string out("\x05", 1);
      for (unsigned char c : v.s) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
      }
      return out;
    }
    case ValType::kString:
    default:
      return std::string("\x06", 1) + v.s;
  }
}

}  // namespace

Status compute_checksum(ISource& src, const std::string& table, int batch,
                        Checksum& out) {
  out = Checksum();
  Status st = src.open_cursor(table);
  if (!st.ok()) return st;

  std::vector<Row> rows;
  while (true) {
    rows.clear();
    st = src.fetch_batch(batch > 0 ? batch : 1000, rows);
    if (!st.ok()) {
      src.close_cursor();
      return st;
    }
    if (rows.empty()) break;
    for (const auto& r : rows) {
      std::string joined;
      for (size_t i = 0; i < r.size(); ++i) {
        if (i) joined.push_back('\x1f');
        joined += canonical(r[i]);
      }
      const uint64_t h = fnv1a(joined, 1469598103934665603ULL);
      out.sum += h;
      out.xr ^= h;
      out.rows += 1;
    }
  }
  src.close_cursor();
  return Status::OK();
}

bool checksum_equal(const Checksum& a, const Checksum& b) {
  return a.rows == b.rows && a.sum == b.sum && a.xr == b.xr;
}

}  // namespace mig
