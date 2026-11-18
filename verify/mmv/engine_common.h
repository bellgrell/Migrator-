#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "chunk.h"
#include "normalize.h"
#include "schema.h"
#include "sql_backend.h"
#include "status.h"
#include "value.h"

namespace mmv {
namespace vdetail {

struct ColPair {
  std::string name;
  std::string src_q;
  std::string tgt_q;
  NormSpec src_spec;
  NormSpec tgt_spec;
};

struct Bounds {
  bool has = false;
  int64_t min = 0;
  int64_t max = 0;
  int64_t rows = 0;
};

struct TableStats {
  int64_t rows = 0;
  int64_t row_bytes = 0;
  int64_t change_token = 0;
};

inline std::string cell_at(const ResultSet& rs, size_t r, size_t c) {
  if (r >= rs.rows.size() || c >= rs.rows[r].size() || rs.rows[r][c].is_null) return "";
  return rs.rows[r][c].text;
}

inline void sleep_ms(int ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline bool compatible(LogicalType a, LogicalType b, bool allow_tz) {
  if (a == b) return true;
  auto numeric = [](LogicalType t) {
    return t == LogicalType::kInt || t == LogicalType::kDecimal || t == LogicalType::kFloat;
  };
  if (numeric(a) && numeric(b)) return true;
  if ((a == LogicalType::kString && b == LogicalType::kChar) ||
      (a == LogicalType::kChar && b == LogicalType::kString))
    return true;
  if (allow_tz && ((a == LogicalType::kTimestamp && b == LogicalType::kTimestampTz) ||
                   (a == LogicalType::kTimestampTz && b == LogicalType::kTimestamp)))
    return true;
  return false;
}

inline bool find_col(const TableSchema& s, const std::string& n, const ColumnInfo** out) {
  for (const auto& c : s.columns) {
    if (lower_ascii(c.name) == lower_ascii(n)) {
      *out = &c;
      return true;
    }
  }
  return false;
}

// Choose the worker count for one table. `requested > 0` is an explicit cap;
// 0 means auto: use available cores, keep small tables single-threaded, and
// never open more connections than there are chunks.
inline int choose_workers(int requested, size_t chunks, int64_t rows, int cores) {
  if (chunks == 0) chunks = 1;
  const int cap = (int)std::min<size_t>(chunks, 1000000);
  if (requested > 0) return std::max(1, std::min(requested, cap));
  if (cores <= 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    cores = hw ? (int)hw : 2;
  }
  int w = std::max(1, std::min(cores, 8));
  w = std::min<int>(w, (int)std::min<size_t>(chunks, 64));
  if (rows > 0 && rows < 200000) w = 1;
  else if (rows > 0 && rows < 1000000) w = std::min(w, 2);
  else if (rows > 0 && rows < 5000000) w = std::min(w, 4);
  return std::max(1, w);
}

inline std::vector<std::string> split_key_list(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace vdetail
}  // namespace mmv
