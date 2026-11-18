#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mig {

struct Options;

struct MmvMismatch {
  std::string kind;
  std::string key;
  std::string column;
  std::string source_value;
  std::string target_value;
  std::vector<std::string> key_values;
};

struct MmvTableResult {
  std::string table;
  bool ok = false;
  int64_t mismatches = 0;
  std::string detail;
  std::vector<MmvMismatch> diffs;
};

struct MmvColumnOverride {
  std::string table;
  std::string column;
  bool target = false;
  std::string family;  // int/decimal/float/bool/string/char/bytes/date/time/timestamp/timestamptz/json/uuid
  double epsilon = 1e-9;
  bool case_insensitive = false;
  bool trim_space = false;
  bool accent_insensitive = false;
  bool digest_bytes = false;
};

struct MmvVerifyResult {
  bool ok = false;
  int64_t mismatches = 0;
  std::string detail;
  std::vector<MmvTableResult> tables;
};

// Verify a batch of tables in one engine run (coordinator + worker connections
// are reused across tables).
MmvVerifyResult run_mmv_verify(const Options& o, const std::vector<std::string>& tables);
MmvVerifyResult run_mmv_verify(const Options& o, const std::vector<std::string>& tables,
                               const std::vector<MmvColumnOverride>& overrides);

// Single-table compatibility wrapper.
MmvVerifyResult run_mmv_table_verify(const Options& o, const std::string& table);

}  // namespace mig
