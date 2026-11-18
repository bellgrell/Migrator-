#pragma once

#include "engine_common.h"

namespace mmv {
namespace vdetail {

struct Digest {
  int64_t n = 0;
  // Canonical decimal text returned by the database. Never store the 128-bit
  // checksum in long double: on x86-64 Linux it has a 64-bit mantissa and
  // silently rounds sums above 2^64, which can hide real differences.
  std::string h1;
  std::string h2;
  int64_t max_row_bytes = 0;
  // Strict JSON mode: rows whose MySQL JSON canonical form hit the overflow
  // marker in this chunk. Not part of equality (only the target sets it).
  int64_t json_overflow = 0;
  bool truncated = false;
  bool operator==(const Digest& o) const {
    return n == o.n && h1 == o.h1 && h2 == o.h2;
  }
};

Status digest_range(SqlBackend& db, const std::string& table_q, const std::string& where,
                    const std::vector<ColPair>& cols, bool source, Digest& out,
                    int retries, int backoff_ms, bool check_json_overflow = false,
                    bool use_md5 = false);

}  // namespace vdetail
}  // namespace mmv
