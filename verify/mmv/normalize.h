#pragma once

#include <string>
#include <utility>
#include <vector>

#include "value.h"

namespace mmv {

enum class LogicalType {
  kInt, kDecimal, kFloat, kBool, kString, kChar, kBytes,
  kDate, kTime, kTimestamp, kTimestampTz, kJson, kUuid, kUnknown
};

enum class Dialect { kPg, kMySQL };

struct NormSpec {
  LogicalType type = LogicalType::kUnknown;
  int precision = 0;
  int scale = 0;
  int char_len = 0;
  double epsilon = 1e-9;
  bool case_insensitive = false;  // lower() both sides
  bool trim_space = false;        // rtrim spaces (CHAR always trims)
  bool accent_insensitive = false;  // fold Latin accents to ASCII
  bool digest_bytes = false;        // hash kBytes instead of hex-expanding (digest mode)
};

const char* logical_name(LogicalType t);

// Number of decimal digits implied by an absolute tolerance (1e-9 -> 9).
int epsilon_digits(double eps);

// Client-side reference canonicalization. Null becomes the literal "\\N";
// backslash and '|' are escaped so a concatenated row is unambiguous.
std::string canonical(const Value& v, const NormSpec& spec);

// Fold common Latin-1/Latin Extended-A accents and combining marks to ASCII.
// Case is preserved; apply lower() afterwards for case-insensitive matching.
std::string fold_accent(const std::string& s);

std::string quote_ident(const std::string& name, Dialect d);
std::string quote_lit(const std::string& s, Dialect d);

// SQL expression producing canonical text for one non-null column value.
std::string sql_canonical(const std::string& col, const NormSpec& spec, Dialect d);

// Single field: NULL marker + escaping, ready for row concatenation/comparison.
std::string sql_field(const std::string& quoted_col, const NormSpec& spec, Dialect d);

// Raw key text (no NULL marker/escaping), used to build chunk boundaries.
std::string sql_key_text(const std::string& quoted_col, const NormSpec& spec, Dialect d);

// Server-side digest for a binary column. Used by row mode so large BLOBs are
// hashed in the database instead of being materialized on the client.
std::string sql_blob_digest(const std::string& quoted_col, Dialect d);

// SQL expression for the full row text, with NULL marker and escaping.
std::string sql_row_text(const std::vector<std::pair<std::string, NormSpec>>& cols,
                         Dialect d);

// Chunk digest SQL: (n, h1, h2[, json_overflow]) over rows matching `where`.
// check_json_overflow adds a MySQL-only counter for JSON values whose canonical
// form carries the __MMV_JSON_OVF__ marker, so strict mode needs no extra scan.
// Switch JSON/hash SQL to the openGauss-compatible variant for this run.
void set_opengauss_mode(bool on);

std::string sql_digest(const std::string& quoted_table, const std::string& where,
                       const std::vector<std::pair<std::string, NormSpec>>& cols,
                       Dialect d, bool check_json_overflow = false,
                       bool use_md5 = false);

}  // namespace mmv
