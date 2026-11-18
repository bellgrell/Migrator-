#include "chunk.h"

#include <algorithm>
#include <limits>

namespace mmv {

std::string key_literal(const KeyValue& v, Dialect d) {
  if (v.kind == KeyKind::kInt) return std::to_string(v.i);
  if (v.kind == KeyKind::kHex) {
    if (d == Dialect::kPg) return "decode('" + v.s + "', 'hex')";
    return "X'" + v.s + "'";
  }
  return quote_lit(v.s, d);
}

std::string table_ref(const std::string& quoted_table, const TupleRange& r, Dialect d,
                      bool target) {
  const std::string& p = target ? r.partition_tgt : r.partition;
  if (d == Dialect::kMySQL && !p.empty())
    return quoted_table + " PARTITION (" + quote_ident(p, d) + ")";
  return quoted_table;
}

std::string TupleRange::where(const std::vector<std::string>& quoted_keys, Dialect d,
                              bool target) const {
  std::vector<std::string> parts;
  auto tuple = [&](const std::vector<KeyValue>& vals) {
    std::string t = "(";
    for (size_t i = 0; i < vals.size(); ++i) {
      if (i) t += ",";
      t += key_literal(vals[i], d);
    }
    return t + ")";
  };
  auto ident_tuple = [&](const std::vector<std::string>& vals) {
    std::string t = "(";
    for (size_t i = 0; i < vals.size(); ++i) {
      if (i) t += ",";
      t += vals[i];
    }
    return t + ")";
  };

  const std::string& part = target ? partition_tgt : partition;
  auto partition_pred = [&]() -> std::string {
    if (part.empty() || d != Dialect::kPg) return "";
    const std::string q = quote_lit(part, d) + "::regclass";
    if (partition_simple) return "tableoid = " + q;
    return "(tableoid = " + q + " OR tableoid IN (SELECT relid FROM pg_partition_tree(" +
           q + ")))";
  };
  const std::string pp = partition_pred();
  if (equality && !quoted_keys.empty()) {
    std::string eq;
    if (quoted_keys.size() == 1) eq = quoted_keys[0] + " = " + key_literal(lo[0], d);
    else eq = ident_tuple(quoted_keys) + " = " + tuple(lo);
    return pp.empty() ? eq : pp + " AND " + eq;
  }
  if (!pp.empty()) parts.push_back(pp);
  if (first_key_only && !quoted_keys.empty()) {
    if (has_lo) parts.push_back(quoted_keys[0] + " >= " + key_literal(lo[0], d));
    if (has_hi) parts.push_back(quoted_keys[0] + " < " + key_literal(hi[0], d));
  } else if (has_lo) {
    if (quoted_keys.size() == 1)
      parts.push_back(quoted_keys[0] + " >= " + key_literal(lo[0], d));
    else
      parts.push_back(ident_tuple(quoted_keys) + " >= " + tuple(lo));
  }
  if (!first_key_only && has_hi) {
    if (quoted_keys.size() == 1)
      parts.push_back(quoted_keys[0] + " < " + key_literal(hi[0], d));
    else
      parts.push_back(ident_tuple(quoted_keys) + " < " + tuple(hi));
  }
  if (parts.empty()) return "TRUE";
  std::string out = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) out += " AND " + parts[i];
  return out;
}

std::string KeyRange::where(const std::string& quoted_pk) const {
  std::vector<std::string> parts;
  if (has_lo) parts.push_back(quoted_pk + " >= " + std::to_string(lo));
  if (has_hi) parts.push_back(quoted_pk + " < " + std::to_string(hi));
  if (parts.empty()) return "TRUE";
  std::string out = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) out += " AND " + parts[i];
  return out;
}

std::vector<KeyRange> plan_ranges(int64_t min, int64_t max, int64_t row_count,
                                  int64_t chunk_rows, int64_t max_chunks) {
  std::vector<KeyRange> out;
  if (chunk_rows <= 0) chunk_rows = 100000;
  if (row_count <= 0) {
    out.push_back(KeyRange{});
    return out;
  }
  if (max < min) {
    out.push_back(KeyRange{});
    return out;
  }

  int64_t chunks = (row_count + chunk_rows - 1) / chunk_rows;
  const long double span = (long double)max - (long double)min + 1.0L;
  const int64_t max_allowed = std::max<int64_t>(1, max_chunks);
  if (chunks > max_allowed) chunks = max_allowed;
  if (chunks <= 1) {
    KeyRange r;
    r.has_lo = r.has_hi = true;
    r.lo = min;
    r.hi = max == std::numeric_limits<int64_t>::max() ? max : max + 1;
    out.push_back(r);
    return out;
  }

  const long double width = span / (long double)chunks;
  int64_t lo = min;
  for (int64_t i = 0; i < chunks; ++i) {
    KeyRange r;
    r.has_lo = true;
    r.has_hi = true;
    r.lo = lo;
    if (i + 1 == chunks) {
      r.hi = max == std::numeric_limits<int64_t>::max() ? max : max + 1;
      if (max == std::numeric_limits<int64_t>::max()) r.has_hi = false;
    } else {
      r.hi = (int64_t)((long double)min + width * (long double)(i + 1));
    }
    if (r.hi > r.lo) out.push_back(r);
    lo = r.hi;
  }
  if (out.empty()) out.push_back(KeyRange{});
  return out;
}

KeyRange cover(const KeyRange& a, const KeyRange& b) {
  if (!a.has_lo && !a.has_hi) return b;
  if (!b.has_lo && !b.has_hi) return a;
  KeyRange r;
  r.has_lo = a.has_lo && b.has_lo;
  r.has_hi = a.has_hi && b.has_hi;
  if (r.has_lo) r.lo = std::min(a.lo, b.lo);
  if (r.has_hi) r.hi = std::max(a.hi, b.hi);
  return r;
}

}  // namespace mmv
