#pragma once

#include <string>
#include <vector>

#include "normalize.h"
#include "sql_backend.h"

namespace mmv {

struct ColumnInfo {
  std::string name;
  std::string db_type;
  NormSpec spec;
  bool generated = false;  // GENERATED ALWAYS AS / STORED|VIRTUAL
};

struct TableSchema {
  std::string name;
  std::vector<ColumnInfo> columns;
  std::vector<std::string> pk;
};

NormSpec spec_from_type(const std::string& db_type, const std::string& full_type,
                        int precision, int scale, int char_len);

Status load_schema(SqlBackend& db, const std::string& table, TableSchema& out);
Status list_tables(SqlBackend& db, std::vector<std::string>& out);

// Normalize a table name for matching (keeps case; quoted identifiers are
// compared verbatim).
std::string lower_ascii(std::string s);

}  // namespace mmv
