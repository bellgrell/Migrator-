#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "normalize.h"

namespace mmv {

enum class KeyKind { kInt, kStr, kHex };

struct KeyValue {
  KeyKind kind = KeyKind::kInt;
  int64_t i = 0;
  std::string s;

  static KeyValue Int(int64_t v) { KeyValue k; k.kind = KeyKind::kInt; k.i = v; return k; }
  static KeyValue Str(std::string v) { KeyValue k; k.kind = KeyKind::kStr; k.s = std::move(v); return k; }
  static KeyValue Hex(std::string v) { KeyValue k; k.kind = KeyKind::kHex; k.s = std::move(v); return k; }
};

std::string key_literal(const KeyValue& v, Dialect d);

// MySQL partition selection belongs in the FROM clause; PostgreSQL uses a
// tableoid predicate in WHERE.
std::string table_ref(const std::string& quoted_table, const struct TupleRange& r,
                      Dialect d, bool target = false);

// Lexicographic tuple range against a composite key; missing bound = unbounded.
struct TupleRange {
  bool has_lo = false;
  bool has_hi = false;
  // Composite key fast path: bounds apply only to the first key column.
  bool first_key_only = false;
  bool equality = false;  // lo is the exact key tuple (CDC changed-row mode)
  std::string partition;      // source-side physical partition
  std::string partition_tgt;  // target-side physical partition
  bool partition_simple = false;  // no pg_partition_tree: tableoid equality only
  std::vector<KeyValue> lo;
  std::vector<KeyValue> hi;

  std::string where(const std::vector<std::string>& quoted_keys, Dialect d,
                    bool target = false) const;
};

// Half-open primary-key range [lo, hi). Missing bound means unbounded.
struct KeyRange {
  bool has_lo = false;
  bool has_hi = false;
  int64_t lo = 0;
  int64_t hi = 0;

  bool empty() const { return has_lo && has_hi && hi <= lo; }
  std::string where(const std::string& quoted_pk) const;
};

// Split [min,max] into roughly `chunk_rows`-sized chunks. Returns at least one
// range; capped at `max_chunks` to keep planning bounded.
std::vector<KeyRange> plan_ranges(int64_t min, int64_t max, int64_t row_count,
                                  int64_t chunk_rows, int64_t max_chunks = 1000000);

// Merge two planning windows (source and target may disagree by extra rows).
KeyRange cover(const KeyRange& a, const KeyRange& b);

}  // namespace mmv
