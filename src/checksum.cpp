#include "migrator.h"
#include "checksum_normalizer.h"

#include <algorithm>
#include <cstddef>
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

}  // namespace

Status compute_checksum(ISource& src, const std::string& table, int batch,
                        Checksum& out, std::size_t max_bytes,
                        std::size_t max_rows, const std::string& order_col) {
  out = Checksum();
  // 限量校验必须按同一顺序取行，否则两端取到的不是同一批数据。
  Status st = (max_rows > 0 && !order_col.empty())
                  ? src.open_cursor_ordered(table, order_col)
                  : src.open_cursor(table);
  if (!st.ok()) return st;

  int n = batch > 0 ? batch : 1000;
  std::vector<Row> rows;
  while (true) {
    rows.clear();
    st = src.fetch_batch(n, rows);
    if (!st.ok()) {
      src.close_cursor();
      return st;
    }
    if (rows.empty()) break;
    for (const auto& r : rows) {
      std::string joined;
      for (size_t i = 0; i < r.size(); ++i) {
        if (i) joined.push_back('\x1f');
        joined += normalize_value(r[i]);
      }
      const uint64_t h = fnv1a(joined, 1469598103934665603ULL);
      out.sum += h;
      out.xr ^= h;
      out.rows += 1;
      if (max_rows > 0 && (std::size_t)out.rows >= max_rows) break;
    }
    if (max_rows > 0 && (std::size_t)out.rows >= max_rows) break;
    if (max_bytes > 0 && !rows.empty()) {
      std::size_t bytes = rows.size() * 8;
      for (const auto& r : rows) {
        for (const auto& v : r) bytes += v.s.size();
      }
      const double avg = (double)bytes / (double)rows.size();
      int next_n = (int)((double)max_bytes / std::max(1.0, avg));
      if (next_n < 1) next_n = 1;
      if (next_n > 2000) next_n = 2000;
      n = next_n;
    }
  }
  src.close_cursor();
  return Status::OK();
}

bool checksum_equal(const Checksum& a, const Checksum& b) {
  return a.rows == b.rows && a.sum == b.sum && a.xr == b.xr;
}

}  // namespace mig
