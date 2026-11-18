#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sql_backend.h"

namespace mmv {

struct ColumnSpecOverride {
  std::string table;
  std::string column;
  bool target = false;
  LogicalType type = LogicalType::kUnknown;
  double epsilon = 1e-9;
  bool case_insensitive = false;
  bool trim_space = false;
  bool accent_insensitive = false;
  bool digest_bytes = false;
};

struct VerifyOptions {
  std::string source_kind = "pg";
  std::string source_dsn;
  std::string target_kind = "mysql";
  std::string target_dsn;
  std::vector<std::string> tables;
  bool all_tables = false;
  std::string key;            // override chunk key column
  int64_t chunk_rows = 100000;
  int64_t chunk_bytes = 64 * 1024 * 1024;  // 0 disables byte-based sizing
  bool allow_seqscan = false;              // relax the key-index preflight
  std::string resume_dir;                  // empty = no resume
  bool resume_reset = false;               // discard the matching checkpoint
  int resume_gc = 0;                       // keep current + N-1 old plan dirs
  bool assume_static = false;              // user guarantees no writes
  bool allow_tz_mismatch = false;          // allow timestamp <-> timestamptz
  bool mysql_lock = false;                 // FLUSH TABLES WITH READ LOCK for MySQL global snapshot
  std::string resume_token;                // external version/snapshot id
  int query_timeout = 0;                   // seconds; 0 = server default
  int64_t max_split_rows = 5000000;        // stop COUNT/OFFSET bisection above this
  bool progress = false;
  bool snapshot = false;                   // per-worker repeatable-read snapshot
  bool snapshot_global = false;            // PG export snapshot; MySQL needs --assume-static
  int retries = 2;                         // connection / digest retry budget
  int retry_backoff_ms = 200;
  int workers = 0;  // 0 = auto (see choose_workers)
  int sample_percent = 100;  // 1..100; <100 verifies an evenly spaced subset
  std::vector<ColumnSpecOverride> column_overrides;
  int64_t max_mismatches = 10;
  bool row_mode = false;
  double epsilon = 1e-9;  // float tolerance; 1e-9 -> round to 9 decimals
  bool case_insensitive = false;
  bool trim_space = false;
  bool accent_insensitive = false;
  bool quiet = false;
  bool allow_extra_rows = false;  // target-only rows are tolerated
  bool partial = false;           // target may hold only a subset of tables/columns
  bool readonly = false;          // force read-only transactions on both sides
  std::vector<std::string> exclude_tables;  // tables to skip entirely
  bool incremental = false;       // watermark-based incremental verification (CDC phase 1)
  std::string watermark_column;   // e.g. updated_at or id
  std::string watermark_state;    // persisted per-table high-water mark
  bool require_tls = false;       // force TLS on both connections
  bool client_normalize = false;  // compare strings with the C++/ICU normalizer
  int64_t blob_chunk_bytes = 0;   // >0: stream BLOBs in chunks instead of digesting
  std::string keys_file;          // TSV: table<TAB>key1[,key2] (CDC changed rows)
  std::string collation_profile = "binary";  // binary | named collation contract
  std::string collation_key_suffix = "__mmv_key";  // precomputed collation key column
  bool fast = false;                        // throughput-oriented defaults
  std::string json_canon_suffix = "__mmv_json";  // precomputed canonical JSON column
  std::string json_precision = "off";       // off | strict
  std::string parallel_unit = "pk";         // pk | partition | auto
  std::string partition_level = "logical";  // logical | leaf
  int partition_subworkers = 1;             // split each partition by PK range
  int64_t partition_index_min_rows = 100000;  // EXPLAIN only above this size
};

enum class MismatchKind { kValue, kMissingRow, kExtraRow, kChunk };

struct Mismatch {
  MismatchKind kind = MismatchKind::kValue;
  std::string key;
  std::string column;
  std::string source_value;
  std::string target_value;
  // Raw canonical key values, in key-column order. Used by repair so keys that
  // contain commas (or separators) are not parsed from the display string.
  std::vector<std::string> key_values;
};

struct TableReport {
  std::string name;
  std::string status = "ok";  // ok | mismatch | error | skipped | cancelled
  std::string error;
  std::string note;  // informational, e.g. partial migration skips
  int64_t rows_source = 0;
  int64_t rows_target = 0;
  int64_t approx_bytes_source = 0;
  int64_t chunks = 0;
  int64_t completed_chunks = 0;
  int64_t failed_chunks = 0;
  int64_t resumed_chunks = 0;
  double elapsed_ms = 0;
  std::vector<Mismatch> mismatches;
};

struct Report {
  bool ok = true;
  bool operational_error = false;  // exit 1: connection/schema/SQL failure
  bool cancelled = false;          // exit 130: SIGINT
  bool partial = false;            // --partial: target is a declared subset
  std::string source_desc;
  std::string target_desc;
  double elapsed_ms = 0;
  int64_t chunks = 0;
  int64_t completed_chunks = 0;
  int64_t failed_chunks = 0;
  int64_t resumed_chunks = 0;
  int64_t mismatches = 0;
  std::vector<TableReport> tables;
  std::string error;
};

Report run_verify(const VerifyOptions& opts);

std::string report_json(const Report& r, const std::string& version);
std::string metrics_json(const Report& r, const std::string& version);
std::string prometheus_metrics(const Report& r, const std::string& version);
std::string report_text(const Report& r);

const char* mismatch_kind_name(MismatchKind k);

}  // namespace mmv
