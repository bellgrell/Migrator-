#include "migrator.h"
#include "checksum_normalizer.h"

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
        joined += normalize_value(r[i]);
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
