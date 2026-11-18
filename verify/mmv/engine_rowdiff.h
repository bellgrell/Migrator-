#pragma once

#include <cstdint>
#include <vector>

#include "engine.h"
#include "engine_common.h"

namespace mmv {
namespace vdetail {

Status row_diff(SqlBackend& src, SqlBackend& tgt, const std::string& src_table_q,
                const std::string& tgt_table_q, const TupleRange& range,
                const std::vector<ColPair>& keys, const std::vector<ColPair>& cols,
                const std::string& extra_src, const std::string& extra_tgt,
                bool client_norm, int64_t blob_chunk_bytes, int64_t max_mismatches,
                int retries, int backoff_ms, std::vector<Mismatch>& out);

}  // namespace vdetail
}  // namespace mmv
