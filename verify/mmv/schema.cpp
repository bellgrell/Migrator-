#include "schema.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mmv {
namespace {

int to_int(const std::string& s) {
  if (s.empty()) return 0;
  return std::atoi(s.c_str());
}

std::string cell(const ResultSet& rs, size_t r, size_t c) {
  if (r >= rs.rows.size() || c >= rs.rows[r].size() || rs.rows[r][c].is_null) return "";
  return rs.rows[r][c].text;
}

}  // namespace

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

NormSpec spec_from_type(const std::string& db_type, const std::string& full_type,
                        int precision, int scale, int char_len) {
  NormSpec sp;
  const std::string t = lower_ascii(db_type);
  sp.precision = precision;
  sp.scale = scale;
  sp.char_len = char_len;
  if (t == "smallint" || t == "integer" || t == "int" || t == "bigint" ||
      t == "tinyint" || t == "mediumint" || t == "int2" || t == "int4" || t == "int8" ||
      t == "serial" || t == "bigserial" || t == "smallserial") {
    if (lower_ascii(full_type) == "tinyint(1)" || lower_ascii(full_type).rfind("tinyint(1)", 0) == 0)
      sp.type = LogicalType::kBool;
    else
      sp.type = LogicalType::kInt;
  } else if (t == "numeric" || t == "decimal" || t == "dec" || t == "money") {
    sp.type = LogicalType::kDecimal;
  } else if (t == "real" || t == "float" || t == "double" || t == "double precision" ||
             t == "float4" || t == "float8") {
    sp.type = LogicalType::kFloat;
  } else if (t == "boolean" || t == "bool") {
    sp.type = LogicalType::kBool;
  } else if (t == "character" || t == "char" || t == "bpchar" || t == "nchar") {
    sp.type = LogicalType::kChar;
  } else if (t == "bytea" || t == "binary" || t == "varbinary" || t == "blob" ||
             t == "tinyblob" || t == "mediumblob" || t == "longblob") {
    sp.type = LogicalType::kBytes;
  } else if (t == "date") {
    sp.type = LogicalType::kDate;
  } else if (t == "time" || t == "timetz" || t == "time without time zone" ||
             t == "time with time zone") {
    sp.type = LogicalType::kTime;
  } else if (t == "datetime" || t == "timestamp without time zone") {
    sp.type = LogicalType::kTimestamp;
  } else if (t == "timestamp" || t == "timestamptz" || t == "timestamp with time zone") {
    sp.type = LogicalType::kTimestampTz;
  } else if (t == "inet" || t == "cidr" || t == "macaddr" || t == "macaddr8" ||
             t == "interval" || t == "xml" || t == "tsvector" || t == "tsquery") {
    sp.type = LogicalType::kString;
  } else if (t == "bit" || t == "varbit" || t == "bit varying" || t == "geometry" ||
             t == "geography") {
    sp.type = LogicalType::kBytes;
  } else if (t == "year" || t == "oid") {
    sp.type = LogicalType::kInt;
  } else if (t == "uniqueidentifier") {
    sp.type = LogicalType::kUuid;
  } else if (t == "json" || t == "jsonb") {
    sp.type = LogicalType::kJson;
  } else if (t == "uuid") {
    sp.type = LogicalType::kUuid;
  } else if (t == "character varying" || t == "varchar" || t == "nvarchar" ||
             t == "text" || t == "tinytext" || t == "mediumtext" || t == "longtext" ||
             t == "enum" || t == "set" || t == "name") {
    sp.type = LogicalType::kString;
  } else {
    sp.type = LogicalType::kUnknown;
  }
  return sp;
}

Status load_schema(SqlBackend& db, const std::string& table, TableSchema& out) {
  out = TableSchema{};
  out.name = table;
  const Dialect d = db.dialect();
  std::string sql;
  if (d == Dialect::kPg) {
    sql =
        "SELECT column_name, data_type, udt_name, "
        "COALESCE(numeric_precision,0), COALESCE(numeric_scale,0), "
        "COALESCE(character_maximum_length,0), COALESCE(is_generated,'NEVER') "
        "FROM information_schema.columns WHERE table_schema = current_schema() "
        "AND table_name = " + db.quote_lit(table) + " ORDER BY ordinal_position";
  } else {
    sql =
        "SELECT COLUMN_NAME, DATA_TYPE, COLUMN_TYPE, "
        "COALESCE(NUMERIC_PRECISION,0), COALESCE(NUMERIC_SCALE,0), "
        "COALESCE(CHARACTER_MAXIMUM_LENGTH,0), COALESCE(EXTRA,'') "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = " + db.quote_lit(table) + " ORDER BY ORDINAL_POSITION";
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty())
    return Status::Err(Code::kInvalid, "table not found or no columns: " + table);
  for (size_t i = 0; i < rs.rows.size(); ++i) {
    ColumnInfo c;
    c.name = cell(rs, i, 0);
    std::string dt = cell(rs, i, 1);
    std::string udt = cell(rs, i, 2);
    const int prec = to_int(cell(rs, i, 3));
    const int scale = to_int(cell(rs, i, 4));
    const int clen = to_int(cell(rs, i, 5));
    const std::string extra = lower_ascii(cell(rs, i, 6));
    c.generated = extra.find("always") != std::string::npos ||
                  extra.find("generated") != std::string::npos;
    if (d == Dialect::kPg) {
      // udt_name is more precise for e.g. int8/bpchar; prefer the SQL name.
      c.db_type = dt;
      if (dt == "USER-DEFINED" || dt == "ARRAY") c.db_type = udt;
      c.spec = spec_from_type(dt, udt, prec, scale, clen);
      if (c.spec.type == LogicalType::kUnknown)
        c.spec = spec_from_type(udt, udt, prec, scale, clen);
    } else {
      c.db_type = cell(rs, i, 2);  // COLUMN_TYPE includes unsigned/zerofill
      c.spec = spec_from_type(dt, c.db_type, prec, scale, clen);
    }
    out.columns.push_back(std::move(c));
  }

  if (d == Dialect::kPg) {
    sql =
        "SELECT kcu.column_name FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
        "WHERE tc.constraint_type = 'PRIMARY KEY' AND tc.table_schema = current_schema() "
        "AND tc.table_name = " + db.quote_lit(table) + " ORDER BY kcu.ordinal_position";
  } else {
    sql =
        "SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " + db.quote_lit(table) +
        " AND CONSTRAINT_NAME = 'PRIMARY' ORDER BY ORDINAL_POSITION";
  }
  ResultSet pk;
  st = db.query(sql, pk);
  if (!st.ok()) return st;
  for (size_t i = 0; i < pk.rows.size(); ++i) out.pk.push_back(cell(pk, i, 0));
  return Status::OK();
}

Status list_tables(SqlBackend& db, std::vector<std::string>& out) {
  out.clear();
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql =
        "SELECT t.table_name FROM information_schema.tables t "
        "WHERE t.table_schema = current_schema() AND t.table_type = 'BASE TABLE' ";
    // openGauss 6.0/8.0 predates pg_class.relispartition.
    if (!db.is_opengauss()) {
      sql +=
          "AND NOT EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
          "WHERE c.relname = t.table_name AND n.nspname = current_schema() AND c.relispartition) ";
    }
    sql += "ORDER BY t.table_name";
  } else {
    sql =
        "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME";
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  for (size_t i = 0; i < rs.rows.size(); ++i) out.push_back(cell(rs, i, 0));
  return Status::OK();
}

}  // namespace mmv
