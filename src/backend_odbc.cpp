#include "providers.h"
#include "backends.h"
#include "db_base.h"
#include "checksum_normalizer.h"
#include "dsn_secret.h"

#ifdef _WIN32
// unixODBC 的 sqltypes.h 依赖 Windows 的 INT64/UINT64 定义
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <filesystem>
#include <atomic>
#include <unistd.h>
#include <vector>

namespace mig {
namespace {

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string dsn_get(const std::string& dsn, const std::string& key,
                    const std::string& def = "") {
  const std::string token = key + "=";
  size_t pos = 0;
  while ((pos = dsn.find(token, pos)) != std::string::npos) {
    if (pos == 0 || dsn[pos - 1] == ';' || dsn[pos - 1] == ' ') {
      size_t v = pos + token.size();
      size_t e = dsn.find(';', v);
      return dsn.substr(v, e == std::string::npos ? std::string::npos : e - v);
    }
    pos += token.size();
  }
  return def;
}

std::string odbc_error(SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[6] = {0};
  SQLCHAR msg[1024] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  std::string out;
  for (SQLSMALLINT i = 1; i < 8; ++i) {
    if (SQLGetDiagRec(type, h, i, state, &native, msg, sizeof msg, &len) !=
        SQL_SUCCESS)
      break;
    if (!out.empty()) out += " | ";
    out += (const char*)msg;
  }
  return out.empty() ? "ODBC error" : out;
}

std::string sql_lit(const std::string& s) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'') o += "''";
    else o += c;
  }
  o += "'";
  return o;
}

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o.push_back(d[c >> 4]);
    o.push_back(d[c & 0xF]);
  }
  return o;
}

std::string trim_ts(std::string s) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

bool is_binary_type(SQLSMALLINT t) {
  return t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY;
}

bool numeric_text(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
  bool digit = false, dot = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.') {
      if (dot) return false;
      dot = true;
    } else if (c >= '0' && c <= '9') {
      digit = true;
    } else {
      return false;
    }
  }
  return digit;
}

std::string strip_type(std::string s) {
  s = lower_ascii(trim(s));
  size_t p = s.find('(');
  if (p != std::string::npos) s = s.substr(0, p);
  return trim(s);
}

std::string args_of(const std::string& s) {
  size_t p = s.find('(');
  if (p == std::string::npos) return "";
  size_t q = s.find(')', p);
  if (q == std::string::npos) return "";
  return s.substr(p, q - p + 1);
}

bool col_is_binary(const ColumnDef& c) {
  if (c.binary) return true;
  const std::string t = lower_ascii(c.src_type);
  return t.find("binary") != std::string::npos || t.find("blob") != std::string::npos ||
         t.find("bytea") != std::string::npos || t.find("image") != std::string::npos;
}

class OdbcConn {
 protected:
  explicit OdbcConn(const Options& o) : opts_(o) {}
 public:
  SQLHDBC raw_dbc() const { return dbc_; }
 protected:
  ~OdbcConn() { disconnect(); }

  bool connect_odbc(const std::string& dsnstr, const std::string& kind) {
    // unixODBC's Windows driver manager is not safe for concurrent
    // SQLDriverConnect/SQLAllocHandle calls from range workers.
    static std::mutex connect_mu;
    std::lock_guard<std::mutex> connect_lk(connect_mu);
    kind_ = kind;
    const std::string dsn = dsn_get(dsnstr, "dsn");
    const std::string host = dsn_get(dsnstr, "host", "127.0.0.1");
    const std::string port = dsn_get(dsnstr, "port", kind == "mssql" ? "1433" : "1521");
    const std::string user = dsn_get(dsnstr, "user", kind == "mssql" ? "sa" : "");
    const std::string pass = resolve_secret(dsn_get(dsnstr, "password", ""));
    const std::string db = dsn_get(dsnstr, "db", "");
    const std::string charset = dsn_get(dsnstr, "charset", "UTF-8");
    const std::string driver = dsn_get(dsnstr, "driver", providers().mssql_driver);
    const bool freetds = (driver == "FreeTDS");
    std::string cs;
    if (!dsn.empty()) {
      cs = "DSN=" + dsn + ";";
      if (kind == "mssql") cs += "ClientCharset=" + charset + ";";
      if (!user.empty()) cs += "UID=" + user + ";";
      if (!pass.empty()) cs += "PWD=" + pass + ";";
      if (!db.empty()) cs += "DATABASE=" + db + ";";
    } else if (kind == "mssql") {
      if (freetds) {
        cs = "DRIVER={FreeTDS};SERVER=" + host + ";PORT=" + port +
             ";UID=" + user + ";PWD=" + pass +
             ";ClientCharset=" + charset + ";TDS_Version=7.4;";
      } else {
        cs = "DRIVER={" + driver + "};SERVER=" + host + "," + port +
             ";UID=" + user + ";PWD=" + pass +
             ";Encrypt=no;TrustServerCertificate=Yes;";
      }
      if (!db.empty()) cs += "DATABASE=" + db + ";";
    } else {
      const std::string drv =
          dsn_get(dsnstr, "driver", providers().oracle_driver);
      cs = "DRIVER={" + drv + "};DBQ=" + host + ":" + port + "/" + db +
           ";UID=" + user + ";PWD=" + pass + ";";
    }

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
    SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
    SQLSetConnectAttr(dbc_, SQL_ATTR_LOGIN_TIMEOUT,
                      (SQLPOINTER)(intptr_t)(opts_.connect_timeout > 0
                                                 ? opts_.connect_timeout
                                                 : 10),
                      0);
    SQLCHAR out[1024];
    SQLSMALLINT outlen = 0;
    SQLRETURN rc = SQLDriverConnect(
        dbc_, NULL, (SQLCHAR*)cs.c_str(), SQL_NTS, out, sizeof out, &outlen,
        SQL_DRIVER_NOPROMPT);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err_ = odbc_error(SQL_HANDLE_DBC, dbc_);
      return false;
    }
    SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
    if (kind == "oracle") {
      exec_sql("ALTER SESSION SET NLS_TIMESTAMP_FORMAT='YYYY-MM-DD HH24:MI:SS.FF6'");
      exec_sql("ALTER SESSION SET NLS_DATE_FORMAT='YYYY-MM-DD HH24:MI:SS'");
    }
    return true;
  }

  Status exec_sql(const std::string& sql) {
    if (!dbc_) return Status::Err(Code::kDb, "not connected");
    SQLHSTMT st = SQL_NULL_HANDLE;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
    SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)sql.c_str(), SQL_NTS);
    Status result = Status::OK();
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO &&
        rc != SQL_NO_DATA) {
      result = Status::Err(Code::kDb, "odbc: " + odbc_error(SQL_HANDLE_STMT, st));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return result;
  }

  bool fetch_all(const std::string& sql, std::vector<std::vector<std::string>>& rows,
                 std::string& err) {
    rows.clear();
    SQLHSTMT st = SQL_NULL_HANDLE;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
    SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)sql.c_str(), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err = odbc_error(SQL_HANDLE_STMT, st);
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return false;
    }
    SQLSMALLINT ncols = 0;
    SQLNumResultCols(st, &ncols);
    while ((rc = SQLFetch(st)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
      std::vector<std::string> row;
      for (SQLUSMALLINT i = 1; i <= (SQLUSMALLINT)ncols; ++i) {
        std::string v;
        SQLLEN ind = 0;
        char buf[8192];
        while (true) {
          rc = SQLGetData(st, i, SQL_C_CHAR, buf, sizeof buf, &ind);
          if (rc == SQL_NO_DATA) break;
          if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
          if (ind == SQL_NULL_DATA) {
            v.clear();
            break;
          }
          const size_t n = ind == SQL_NO_TOTAL || ind < 0
                               ? std::strlen(buf)
                               : std::min<size_t>((size_t)ind, sizeof(buf) - 1);
          v.append(buf, n);
          if (rc == SQL_SUCCESS) break;
        }
        row.push_back(v);
      }
      rows.push_back(std::move(row));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return true;
  }

  std::string qid(const std::string& name) const {
    if (kind_ == "mssql") {
      std::string o = "[";
      for (char c : name) {
        if (c == ']') o += "]]";
        else o += c;
      }
      return o + "]";
    }
    std::string o = "\"";
    for (char c : name) {
      if (c == '"') o += "\"\"";
      else o += c;
    }
    return o + "\"";
  }

  std::string lit(const Value& v) const {
    switch (v.type) {
      case ValType::kNull: return "NULL";
      case ValType::kInt: return std::to_string(v.i);
      case ValType::kBool: return v.b ? "1" : "0";
      case ValType::kDouble: {
        std::ostringstream o;
        o << std::setprecision(17) << v.d;
        return o.str();
      }
      case ValType::kBytes:
        return kind_ == "mssql" ? "0x" + hex(v.s)
                                : "HEXTORAW('" + hex(v.s) + "')";
      case ValType::kString:
      default:
        return (kind_ == "mssql" ? "N" : "") + sql_lit(v.s);
    }
  }

  std::string map_type(const std::string& raw) const {
    std::string base = strip_type(raw);
    const std::string args = args_of(raw);
    if (base.size() > 9 && base.substr(base.size() - 9) == " unsigned")
      base = base.substr(0, base.size() - 9);
    if (kind_ == "mssql") {
      if (base == "int" || base == "integer" || base == "mediumint") return "int";
      if (base == "bigint") return "bigint";
      if (base == "smallint") return "smallint";
      if (base == "tinyint") return "tinyint";
      if (base == "bit" || base == "bool" || base == "boolean") return "bit";
      if (base == "decimal" || base == "numeric")
        return "decimal" + (args.empty() ? std::string("(18,4)") : args);
      if (base == "float" || base == "double" || base == "real" ||
          base == "double precision" || base == "float8" || base == "float4")
        return "float";
      if (base == "char" || base == "nchar") return "nchar" + (args.empty() ? "(1)" : args);
      if (base == "varchar" || base == "nvarchar" || base == "text" ||
          base == "character varying" || base == "json" || base == "jsonb" ||
          base == "enum" || base == "set" || base == "uuid")
        return "nvarchar" + (args.empty() || base == "text" || base == "json"
                                 ? "(max)" : args);
      if (base == "binary" || base == "varbinary" || base == "blob" ||
          base == "bytea")
        return "varbinary(max)";
      if (base == "date") return "date";
      if (base == "datetime" || base == "datetime2" || base == "timestamp" ||
          base == "timestamp without time zone" || base == "timestamp with time zone")
        return "datetime2";
      if (base == "time") return "time";
      if (base == "number") return "decimal" + (args.empty() ? "(18,4)" : args);
      if (base == "varchar2" || base == "nvarchar2")
        return "nvarchar" + (args.empty() ? "(max)" : args);
      if (base == "clob" || base == "nclob" || base == "long" || base == "xmltype")
        return "nvarchar(max)";
      if (base == "blob" || base == "raw" || base == "long raw")
        return "varbinary(max)";
      if (base == "binary_double" || base == "binary_float") return "float";
      if (base == "rowid") return "nvarchar(64)";
      return raw;
    }
    if (base == "int" || base == "integer" || base == "bigint" || base == "mediumint")
      return "NUMBER(19)";
    if (base == "smallint") return "NUMBER(5)";
    if (base == "tinyint") return "NUMBER(3)";
    if (base == "bit" || base == "bool" || base == "boolean") return "NUMBER(1)";
    if (base == "decimal" || base == "numeric")
      return "NUMBER" + (args.empty() ? std::string("(18,4)") : args);
    if (base == "float" || base == "double" || base == "double precision" ||
        base == "float8")
      return "BINARY_DOUBLE";
    if (base == "real" || base == "float4") return "BINARY_FLOAT";
    if (base == "char" || base == "nchar") return "CHAR" + (args.empty() ? "(1)" : args);
    if (base == "varchar" || base == "varchar2" || base == "nvarchar" ||
        base == "character varying") {
      if (args.empty()) return "VARCHAR2(4000)";
      return "VARCHAR2" + args;
    }
    if (base == "text" || base == "json" || base == "jsonb" ||
        base == "enum" || base == "set")
      return "CLOB";
    if (base == "blob" || base == "binary" || base == "varbinary" || base == "bytea")
      return "BLOB";
    if (base == "date") return "DATE";
    if (base == "datetime" || base == "timestamp" ||
        base == "timestamp without time zone" || base == "timestamp with time zone")
      return "TIMESTAMP";
    if (base == "time") return "VARCHAR2(32)";
    if (base == "uuid") return "VARCHAR2(36)";
    return raw;
  }

protected:
  std::string kind_;
  std::string err_;
  Options opts_;

protected:
  SQLHENV env_ = SQL_NULL_HANDLE;
  SQLHDBC dbc_ = SQL_NULL_HANDLE;

  void disconnect() {
    if (dbc_) {
      SQLDisconnect(dbc_);
      SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
      dbc_ = SQL_NULL_HANDLE;
    }
    if (env_) {
      SQLFreeHandle(SQL_HANDLE_ENV, env_);
      env_ = SQL_NULL_HANDLE;
    }
  }
};

class OdbcSource : public OdbcConn, public ISource {
 public:
  explicit OdbcSource(const Options& o)
      : OdbcConn(o), opts_(o), kind_(o.source_kind) {}

  Status connect() override {
    if (kind_ != "mssql" && kind_ != "oracle")
      return Status::Err(Code::kUnsupported, "unsupported ODBC source: " + kind_);
    if (!connect_odbc(opts_.source, kind_))
      return Status::Err(Code::kDb, "odbc source: " + err_);
    return Status::OK();
  }

  Status list_tables(std::vector<std::string>& out) override {
    out.clear();
    const std::string sql =
        kind_ == "mssql"
            ? "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
              "WHERE TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME"
            : "SELECT TABLE_NAME FROM USER_TABLES ORDER BY TABLE_NAME";
    std::vector<std::vector<std::string>> rows;
    std::string err;
    if (!fetch_all(sql, rows, err)) return Status::Err(Code::kDb, "odbc: " + err);
    for (auto& r : rows)
      if (!r.empty() && !r[0].empty()) out.push_back(r[0]);
    return Status::OK();
  }

  Status read_schema(const std::string& table, TableSchema& out) override {
    out = TableSchema();
    out.name = table;
    const std::string t = sql_lit(table);
    const std::string sql =
        kind_ == "mssql"
            ? "SELECT COLUMN_NAME, DATA_TYPE, COALESCE(CHARACTER_MAXIMUM_LENGTH,0), "
              "COALESCE(NUMERIC_PRECISION,0), COALESCE(NUMERIC_SCALE,0), IS_NULLABLE "
              "FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_NAME=" + t +
              " ORDER BY ORDINAL_POSITION"
            : "SELECT COLUMN_NAME, DATA_TYPE, COALESCE(DATA_LENGTH,0), "
              "COALESCE(DATA_PRECISION,0), COALESCE(DATA_SCALE,0), NULLABLE "
              "FROM USER_TAB_COLUMNS WHERE TABLE_NAME=" + t +
              " ORDER BY COLUMN_ID";
    std::vector<std::vector<std::string>> rows;
    std::string err;
    if (!fetch_all(sql, rows, err)) return Status::Err(Code::kDb, "odbc schema: " + err);
    if (rows.empty())
      return Status::Err(Code::kNotFound, "table not found: " + table);
    for (auto& r : rows) {
      if (r.size() < 6) continue;
      ColumnDef c;
      c.name = r[0];
      const std::string dt = lower_ascii(r[1]);
      const int len = std::atoi(r[2].c_str());
      const int prec = std::atoi(r[3].c_str());
      const int scale = std::atoi(r[4].c_str());
      c.src_type = dt;
      if ((dt.find("char") != std::string::npos || dt.find("binary") != std::string::npos) &&
          len > 0)
        c.src_type += "(" + std::to_string(len) + ")";
      else if ((dt == "decimal" || dt == "numeric" || dt == "number") && prec > 0)
        c.src_type += "(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
      const std::string nullable = r[5];
      c.nullable = !(nullable == "NO" || nullable == "N");
      c.binary = dt.find("binary") != std::string::npos ||
                 dt.find("blob") != std::string::npos ||
                 dt == "image" || dt == "raw";
      out.columns.push_back(std::move(c));
    }
    const std::string pk_sql =
        kind_ == "mssql"
            ? "SELECT kcu.COLUMN_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
              "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE kcu "
              "ON tc.CONSTRAINT_NAME=kcu.CONSTRAINT_NAME "
              "WHERE tc.CONSTRAINT_TYPE='PRIMARY KEY' AND tc.TABLE_NAME=" + t +
              " ORDER BY kcu.ORDINAL_POSITION"
            : "SELECT cc.COLUMN_NAME FROM USER_CONSTRAINTS c "
              "JOIN USER_CONS_COLUMNS cc ON c.CONSTRAINT_NAME=cc.CONSTRAINT_NAME "
              "WHERE c.CONSTRAINT_TYPE='P' AND c.TABLE_NAME=" + t +
              " ORDER BY cc.POSITION";
    std::vector<std::vector<std::string>> pks;
    if (fetch_all(pk_sql, pks, err)) {
      for (auto& r : pks)
        if (!r.empty()) out.primary_key.push_back(r[0]);
    }
    for (auto& c : out.columns)
      for (const auto& k : out.primary_key)
        if (c.name == k) c.primary = true;
    return Status::OK();
  }

  Status open_cursor(const std::string& table) override {
    return run_cursor("SELECT * FROM " + qid(table));
  }

  Status open_cursor_ordered(const std::string& table,
                             const std::string& column) override {
    return run_cursor("SELECT * FROM " + qid(table) + " ORDER BY " + qid(column));
  }

  // 断点续传：从上次写入的主键字面量之后继续。
  Status open_cursor_from(const std::string& table, const std::string& pk_col,
                          const std::string& after_literal) override {
    if (after_literal.empty()) return open_cursor(table);
    return run_cursor("SELECT * FROM " + qid(table) + " WHERE " +
                      qid(pk_col) + " > " + after_literal + " ORDER BY " +
                      qid(pk_col));
  }

  Status fetch_batch(int n, std::vector<Row>& out) override {
    out.clear();
    if (!cur_) return Status::OK();
    while ((int)pending_.size() < n) {
      if (!fetch_rowset()) break;
    }
    while (!pending_.empty() && (int)out.size() < n) {
      out.push_back(std::move(pending_.front()));
      pending_.pop_front();
    }
    return Status::OK();
  }

  // 返回 false 表示没有更多数据（或出错）
  bool fetch_rowset() {
    if (!block_) return fetch_one_row();
    rows_fetched_ = 0;
    const SQLRETURN rc = SQLFetch(cur_);
    if (rc == SQL_NO_DATA) return false;
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err_ = odbc_error(SQL_HANDLE_STMT, cur_);
      return false;
    }
    const SQLULEN got = rows_fetched_ == 0 ? 1 : rows_fetched_;
    for (SQLULEN r = 0; r < got; ++r) {
      Row row;
      for (SQLUSMALLINT c = 1; c <= ncols_; ++c) {
        const size_t idx = (size_t)(c - 1);
        const SQLLEN ind = bi_[idx][r];
        if (ind == SQL_NULL_DATA) { row.push_back(Value::Null()); continue; }
        const char* base = bb_[idx].data() + r * bsize_[idx];
        size_t len = (ind == SQL_NO_TOTAL || ind < 0) ? std::strlen(base) : (size_t)ind;
        if (len > bsize_[idx] - 1) len = bsize_[idx] - 1;
        row.push_back(value_from_text(base, len, types_[idx], bbin_[idx]));
      }
      pending_.push_back(std::move(row));
    }
    return true;
  }

  bool fetch_one_row() {
    const SQLRETURN rc = SQLFetch(cur_);
    if (rc == SQL_NO_DATA) return false;
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err_ = odbc_error(SQL_HANDLE_STMT, cur_);
      return false;
    }
    Row row;
    for (SQLUSMALLINT i = 1; i <= ncols_; ++i)
      row.push_back(read_value(cur_, i, types_[i - 1]));
    pending_.push_back(std::move(row));
    return true;
  }

  Value value_from_text(const char* s, size_t len, SQLSMALLINT type, bool bin) {
    if (bin) return Value::Bytes(std::string(s, len));
    const std::string out(s, len);
    if (type == SQL_INTEGER || type == SQL_SMALLINT || type == SQL_TINYINT ||
        type == SQL_BIGINT)
      return Value::Int(std::strtoll(out.c_str(), nullptr, 10));
    if (type == SQL_REAL || type == SQL_FLOAT || type == SQL_DOUBLE)
      return Value::Dbl(std::strtod(out.c_str(), nullptr));
    if (type == SQL_DECIMAL || type == SQL_NUMERIC) return Value::Dec(out);
    if (kind_ == "oracle" && (type == SQL_VARCHAR || type == SQL_CHAR) &&
        numeric_text(out))
      return Value::Dec(out);
    if (type == SQL_BIT)
      return Value::Int(out == "1" || out == "true" || out == "TRUE" ? 1 : 0);
    if (type == SQL_TYPE_TIMESTAMP || type == SQL_TIMESTAMP || type == SQL_DATE ||
        type == SQL_TIME)
      return Value::Str(trim_ts(out));
    return Value::Str(out);
  }

  void setup_block() {
    block_ = false;
    if (!std::getenv("MIG_ODBC_BLOCK")) return;
    if (const char* rs = std::getenv("MIG_ODBC_ROWSET")) { int v = std::atoi(rs); if (v >= 50 && v <= 5000) rowset_ = v; }
    if (ncols_ <= 0 || ncols_ > 64) return;
    for (SQLUSMALLINT i = 0; i < (SQLUSMALLINT)ncols_; ++i) {
      const SQLSMALLINT t = types_[i];
      if (t == SQL_LONGVARCHAR || t == SQL_LONGVARBINARY || t == SQL_WLONGVARCHAR)
        return;
    }
    bsize_.assign(ncols_, 0);
    bbin_.assign(ncols_, false);
    bb_.assign(ncols_, {});
    bi_.assign(ncols_, {});
    for (int i = 0; i < ncols_; ++i) {
      const SQLSMALLINT t = types_[i];
      const bool bin = (t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY);
      size_t elem = (size_t)sizes_[i];
      if (elem < 64) elem = 64;
      if (elem > (bin ? 8192u : 1024u)) elem = bin ? 8192u : 1024u;
      elem += 1;
      bsize_[i] = elem;
      bbin_[i] = bin;
      bb_[i].assign((size_t)rowset_ * elem, 0);
      bi_[i].assign((size_t)rowset_, 0);
      if (SQLBindCol(cur_, (SQLUSMALLINT)(i + 1), bin ? SQL_C_BINARY : SQL_C_CHAR,
                     bb_[i].data(), (SQLLEN)elem, bi_[i].data()) != SQL_SUCCESS)
        return;
    }
    SQLSetStmtAttr(cur_, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0);
    SQLSetStmtAttr(cur_, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)(intptr_t)rowset_, 0);
    SQLSetStmtAttr(cur_, SQL_ATTR_ROWS_FETCHED_PTR, (SQLPOINTER)&rows_fetched_, 0);
    block_ = true;
    if (std::getenv("MIG_DEBUG_BLOCK")) std::fprintf(stderr, "[block] enabled cols=%d rowset=%d\n", (int)ncols_, rowset_);
  }

  Status run_cursor(const std::string& sql) {
    close_cursor();
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &cur_);
    if (SQLExecDirect(cur_, (SQLCHAR*)sql.c_str(), SQL_NTS) != SQL_SUCCESS) {
      err_ = odbc_error(SQL_HANDLE_STMT, cur_);
      close_cursor();
      return Status::Err(Code::kDb, "odbc select: " + err_);
    }
    SQLSMALLINT n = 0;
    SQLNumResultCols(cur_, &n);
    ncols_ = n;
    types_.assign(ncols_, SQL_UNKNOWN_TYPE);
    sizes_.assign(ncols_, 0);
    block_ = false;
    pending_.clear();
    for (SQLUSMALLINT i = 1; i <= ncols_; ++i) {
      char name[256];
      SQLSMALLINT nlen = 0, dtype = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      SQLDescribeCol(cur_, i, (SQLCHAR*)name, sizeof name, &nlen, &dtype, &size,
                     &digits, &nullable);
      types_[i - 1] = dtype;
      sizes_[i - 1] = size;
    }
    setup_block();
    return Status::OK();
  }

  void close_cursor() override {
    block_ = false;
    pending_.clear();
    if (cur_) {
      SQLFreeStmt(cur_, SQL_CLOSE);
      SQLFreeHandle(SQL_HANDLE_STMT, cur_);
      cur_ = SQL_NULL_HANDLE;
    }
  }

  int64_t total_rows(const std::string& table) override {
    std::vector<std::vector<std::string>> rows;
    std::string err;
    if (!fetch_all("SELECT COUNT(*) FROM " + qid(table), rows, err) || rows.empty())
      return -1;
    return std::strtoll(rows[0][0].c_str(), nullptr, 10);
  }

  Status pk_bounds(const std::string& table, const std::string& column,
                   int64_t& lo, int64_t& hi) override {
    std::vector<std::vector<std::string>> rows;
    std::string err;
    const std::string q = "SELECT MIN(" + qid(column) + "), MAX(" + qid(column) +
                          ") FROM " + qid(table);
    if (!fetch_all(q, rows, err) || rows.empty() || rows[0].size() < 2)
      return Status::Err(Code::kDb, "odbc bounds: " + err);
    if (rows[0][0].empty()) return Status::Err(Code::kNotFound, "empty table");
    lo = std::strtoll(rows[0][0].c_str(), nullptr, 10);
    hi = std::strtoll(rows[0][1].c_str(), nullptr, 10);
    return Status::OK();
  }

  std::string describe() override {
    return kind_ + " source (ODBC)";
  }

 private:
  Value read_value(SQLHSTMT st, SQLUSMALLINT col, SQLSMALLINT type) {
    SQLLEN ind = 0;
    if (is_binary_type(type)) {
      std::string out;
      unsigned char buf[8192];
      while (true) {
        const SQLRETURN rc = SQLGetData(st, col, SQL_C_BINARY, buf, sizeof buf, &ind);
        if (rc == SQL_NO_DATA) break;
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
        if (ind == SQL_NULL_DATA) return Value::Null();
        const size_t n = ind == SQL_NO_TOTAL || ind < 0
                             ? sizeof(buf)
                             : std::min<size_t>((size_t)ind, sizeof(buf));
        out.append((char*)buf, n);
        if (rc == SQL_SUCCESS) break;
      }
      return Value::Bytes(out);
    }
    std::string out;
    char buf[8192];
    while (true) {
      const SQLRETURN rc = SQLGetData(st, col, SQL_C_CHAR, buf, sizeof buf, &ind);
      if (rc == SQL_NO_DATA) break;
      if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
      if (ind == SQL_NULL_DATA) return Value::Null();
      const size_t n = ind == SQL_NO_TOTAL || ind < 0
                           ? std::strlen(buf)
                           : std::min<size_t>((size_t)ind, sizeof(buf) - 1);
      out.append(buf, n);
      if (rc == SQL_SUCCESS) break;
    }
    if (type == SQL_INTEGER || type == SQL_SMALLINT || type == SQL_TINYINT ||
        type == SQL_BIGINT)
      return Value::Int(std::strtoll(out.c_str(), nullptr, 10));
    if (type == SQL_REAL || type == SQL_FLOAT || type == SQL_DOUBLE)
      return Value::Dbl(std::strtod(out.c_str(), nullptr));
    // Decimals stay textual, matching MySQL's DECIMAL representation in FNV.
    if (type == SQL_DECIMAL || type == SQL_NUMERIC)
      return Value::Dec(out);
    // Oracle's ODBC driver reports some NUMBER columns as text (no DECIMAL
    // metadata). Mark pure numeric text as decimal so it compares with
    // MySQL DECIMAL/PostgreSQL numeric under the same normalizer tag.
    if (kind_ == "oracle" && (type == SQL_VARCHAR || type == SQL_CHAR) &&
        numeric_text(out))
      return Value::Dec(out);
    if (type == SQL_BIT)
      return Value::Int(out == "1" || out == "true" || out == "TRUE" ? 1 : 0);
    if (type == SQL_TYPE_TIMESTAMP || type == SQL_TIMESTAMP ||
        type == SQL_DATE || type == SQL_TIME)
      return Value::Str(trim_ts(out));
    return Value::Str(out);
  }

  Options opts_;
  std::string kind_;
  SQLHSTMT cur_ = SQL_NULL_HANDLE;
  SQLSMALLINT ncols_ = 0;
  std::vector<SQLSMALLINT> types_;
  std::vector<SQLULEN> sizes_;
  bool block_ = false;
  int rowset_ = 500;
  std::vector<std::vector<char>> bb_;
  std::vector<std::vector<SQLLEN>> bi_;
  std::vector<SQLULEN> bsize_;
  std::vector<bool> bbin_;
  SQLULEN rows_fetched_ = 0;
  std::deque<Row> pending_;
};

class OdbcWriter : public OdbcConn {
 public:
  explicit OdbcWriter(const Options& o) : OdbcConn(o), opts_(o) {}
  bool open(const std::string& kind) {
    if (!connect_odbc(opts_.target, kind)) return false;
    SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
    return true;
  }
  Status exec(const std::string& sql) { return exec_sql(sql); }

 private:
  Options opts_;
};

class OdbcTarget : public OdbcConn, public ITarget {
 public:
  explicit OdbcTarget(const Options& o)
      : OdbcConn(o), opts_(o), kind_(o.target_kind) {}

  Status connect() override {
    if (kind_ != "mssql" && kind_ != "oracle")
      return Status::Err(Code::kUnsupported, "unsupported ODBC target: " + kind_);
    if (!connect_odbc(opts_.target, kind_))
      return Status::Err(Code::kDb, "odbc target: " + err_);
    return Status::OK();
  }

  std::string build_ddl(const TableSchema& s) override {
    std::string out = "CREATE TABLE " + qid(s.name) + " (\n";
    for (size_t i = 0; i < s.columns.size(); ++i) {
      const auto& c = s.columns[i];
      out += "  " + qid(c.name) + " " + map_type(c.src_type);
      if (!c.nullable) out += " NOT NULL";
      if (i + 1 < s.columns.size() || !s.primary_key.empty()) out += ",";
      out += "\n";
    }
    if (!s.primary_key.empty()) {
      out += "  PRIMARY KEY (";
      for (size_t i = 0; i < s.primary_key.size(); ++i) {
        if (i) out += ", ";
        out += qid(s.primary_key[i]);
      }
      out += ")\n";
    }
    out += ");";
    return out;
  }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    {
      std::lock_guard<std::mutex> lk(schema_mu_);
      schemas_[s.name] = s;
    }
    Status st = exec_sql(ddl);
    if (!st.ok()) return st;
    if (kind_ == "oracle") exec_sql("ALTER TABLE " + qid(s.name) + " NOLOGGING");
    return commit();
  }

  void register_schema(const TableSchema& s) override {
    std::lock_guard<std::mutex> lk(schema_mu_);
    schemas_[s.name] = s;
  }

  Status drop_table(const std::string& table) override {
    Status st = exec_sql("DROP TABLE " + qid(table));
    if (!st.ok()) return st;
    return commit();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
    TableSchema s;
    {
      std::lock_guard<std::mutex> lk(schema_mu_);
      auto it = schemas_.find(table);
      if (it == schemas_.end())
        return Status::Err(Code::kDb, "odbc target: schema not loaded for " + table);
      s = it->second;
    }
    // MSSQL: bcp 文件通道（无二进制列时启用）
    if ((kind_ == "mssql" || kind_ == "oracle") && !s.columns.empty()) {
      bool has_bin = false;
      for (const auto& c : s.columns) if (col_is_binary(c)) has_bin = true;
      if (kind_ == "oracle" && has_bin) {
        return oracle_stage_write(table, s, rows);
      }
      const bool bulk_ok = (kind_ == "mssql") ? bcp_ready() : sqlldr_ready();
      if (!has_bin && bulk_ok) {
        std::lock_guard<std::mutex> lk(bcp_mu_);
        bcp_schema_[table] = s;
        auto& buf = bcp_buf_[table];
        buf.insert(buf.end(), rows.begin(), rows.end());
        if (buf.size() >= 100000) {
          Status st = flush_pending_locked(table);
          if (!st.ok()) return st;
        }
        return Status::OK();
      }
    }
    OdbcWriter* w = writer();
    if (!w) return Status::Err(Code::kDb, "odbc target: worker connect failed");
    std::string cols;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (i) cols += ",";
      cols += qid(s.columns[i].name);
    }
    // 字面量批量写入（每 2000 行一条语句）
    const size_t kChunk = 1000;
    if (kind_ == "oracle") {
      for (size_t start = 0; start < rows.size(); start += kChunk) {
        const size_t end = std::min(rows.size(), start + kChunk);
        std::string sql = "INSERT ALL ";
        for (size_t k = start; k < end; ++k) {
          sql += "INTO " + qid(table) + " (" + cols + ") VALUES (";
          for (size_t i = 0; i < rows[k].size(); ++i) {
            if (i) sql += ",";
            sql += lit(rows[k][i]);
          }
          sql += ") ";
        }
        sql += "SELECT 1 FROM DUAL";
        Status st = w->exec(sql);
        if (!st.ok()) return st;
      }
    } else {
      for (size_t start = 0; start < rows.size(); start += kChunk) {
        const size_t end = std::min(rows.size(), start + kChunk);
        std::string sql = "INSERT INTO " + qid(table) + " (" + cols + ") VALUES ";
        for (size_t k = start; k < end; ++k) {
          if (k > start) sql += ",";
          sql += "(";
          for (size_t i = 0; i < rows[k].size(); ++i) {
            if (i) sql += ",";
            sql += lit(rows[k][i]);
          }
          sql += ")";
        }
        Status st = w->exec(sql);
        if (!st.ok()) return st;
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  // Oracle 两段式装载：先写 VARCHAR2 暂存表（hex），再 TO_BLOB(HEXTORAW()) 转入目标
  Status oracle_stage_write(const std::string& table, const TableSchema& s,
                            const std::vector<Row>& rows) {
    OdbcWriter* w = writer();
    if (!w) return Status::Err(Code::kDb, "odbc target: worker connect failed");
    const std::string stage = table + "__stg";
    {
      std::lock_guard<std::mutex> lk(stage_mu_);
      if (!stage_ready_) {
        w->exec("DROP TABLE " + qid(stage));
        std::string ddl = "CREATE TABLE " + qid(stage) + " (\n";
        for (size_t i = 0; i < s.columns.size(); ++i) {
          const auto& c = s.columns[i];
          ddl += "  " + qid(c.name) + " " +
                 (col_is_binary(c) ? std::string("VARCHAR2(4000)") : map_type(c.src_type));
          if (i + 1 < s.columns.size()) ddl += ",";
          ddl += "\n";
        }
        ddl += ")";
        Status st = w->exec(ddl);
        if (!st.ok()) return st;
        stage_ready_ = true;
      }
    }
    std::string cols;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (i) cols += ",";
      cols += qid(s.columns[i].name);
    }
    for (size_t start = 0; start < rows.size(); start += 1000) {
      const size_t end = std::min(rows.size(), start + 1000);
      std::string sql = "INSERT ALL ";
      for (size_t k = start; k < end; ++k) {
        sql += "INTO " + qid(stage) + " (" + cols + ") VALUES (";
        for (size_t i = 0; i < rows[k].size(); ++i) {
          if (i) sql += ",";
          if (col_is_binary(s.columns[i]) && rows[k][i].type == ValType::kBytes)
            sql += "'" + hex(rows[k][i].s) + "'";
          else
            sql += lit(rows[k][i]);
        }
        sql += ") ";
      }
      sql += "SELECT 1 FROM DUAL";
      Status st = w->exec(sql);
      if (!st.ok()) return st;
    }
    std::string sel;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (i) sel += ",";
      sel += col_is_binary(s.columns[i])
                 ? ("TO_BLOB(HEXTORAW(" + qid(s.columns[i].name) + "))")
                 : qid(s.columns[i].name);
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      written_[table] += (int64_t)rows.size();
    }
    int64_t pending = 0;
    {
      std::lock_guard<std::mutex> lk(stage_mu_);
      pending = (stage_pending_[table] += (int64_t)rows.size());
    }
    if (pending > 0) return oracle_copy_stage(table, s);
    return Status::OK();
  }

  Status oracle_copy_stage(const std::string& table, const TableSchema& s) {
    OdbcWriter* w = writer();
    if (!w) return Status::Err(Code::kDb, "odbc target: worker connect failed");
    const std::string stage = table + "__stg";
    std::string cols, sel;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (i) { cols += ","; sel += ","; }
      cols += qid(s.columns[i].name);
      sel += col_is_binary(s.columns[i])
                 ? ("TO_BLOB(HEXTORAW(" + qid(s.columns[i].name) + "))")
                 : qid(s.columns[i].name);
    }
    Status st = w->exec("INSERT INTO " + qid(table) + " (" + cols + ") SELECT " + sel +
                        " FROM " + qid(stage));
    if (!st.ok()) return st;
    w->exec("TRUNCATE TABLE " + qid(stage));
    std::lock_guard<std::mutex> lk(stage_mu_);
    stage_pending_[table] = 0;
    return Status::OK();
  }

  Status exec_sql(const std::string& sql) override {
    return OdbcConn::exec_sql(sql);
  }

  Status table_columns(const std::string& table,
                       std::vector<std::string>& out) override {
    out.clear();
    std::vector<std::vector<std::string>> rows;
    std::string err;
    const std::string t = sql_lit(table);
    const std::string sql =
        kind_ == "mssql"
            ? "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
              "WHERE TABLE_NAME=" + t + " ORDER BY ORDINAL_POSITION"
            : "SELECT COLUMN_NAME FROM USER_TAB_COLUMNS WHERE TABLE_NAME=" + t +
                  " ORDER BY COLUMN_ID";
    if (!fetch_all(sql, rows, err))
      return Status::Err(Code::kDb, "table_columns: " + err);
    for (auto& r : rows)
      if (!r.empty() && !r[0].empty()) out.push_back(r[0]);
    return Status::OK();
  }

  // CDC/repair 的行级变更：用 MERGE 实现 upsert（两种方言都支持），
  // 无主键时返回空串，由调用方报错。
  std::string upsert_sql(const TableSchema& s, const Row& row) override {
    if (row.size() != s.columns.size()) return "";
    std::vector<size_t> pk;
    for (size_t i = 0; i < s.columns.size(); ++i)
      if (s.columns[i].primary) pk.push_back(i);
    if (pk.empty()) return "";

    std::string src = "SELECT ";
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) src += ", ";
      src += typed_lit(row[i], s.columns[i]) + " AS c" + std::to_string(i);
    }
    if (kind_ == "oracle") src += " FROM dual";

    std::string on, upd, cols, vals;
    for (size_t k = 0; k < pk.size(); ++k) {
      if (k) on += " AND ";
      on += "tgt." + qid(s.columns[pk[k]].name) + " = src.c" +
            std::to_string(pk[k]);
    }
    for (size_t i = 0; i < row.size(); ++i) {
      bool is_pk = false;
      for (size_t k : pk)
        if (k == i) is_pk = true;
      if (is_pk) continue;
      if (!upd.empty()) upd += ", ";
      upd += "tgt." + qid(s.columns[i].name) + " = src.c" + std::to_string(i);
    }
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) { cols += ","; vals += ","; }
      cols += qid(s.columns[i].name);
      vals += "src.c" + std::to_string(i);
    }

    const bool mssql = kind_ == "mssql";
    std::string sql = "MERGE INTO " + qid(s.name) + (mssql ? " AS tgt" : " tgt") +
                      " USING (" + src + ")" + (mssql ? " AS src" : " src") +
                      " ON (" + on + ")";
    if (!upd.empty()) sql += " WHEN MATCHED THEN UPDATE SET " + upd;
    sql += " WHEN NOT MATCHED THEN INSERT (" + cols + ") VALUES (" + vals + ")";
    if (mssql) sql += ";";
    return sql;
  }

  std::string delete_row_sql(const TableSchema& s, const Row& row) override {
    if (row.size() != s.columns.size()) return "";
    std::string where;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (!s.columns[i].primary) continue;
      if (!where.empty()) where += " AND ";
      where += qid(s.columns[i].name) + " = " + lit(row[i]);
    }
    if (where.empty()) return "";
    return "DELETE FROM " + qid(s.name) + " WHERE " + where;
  }

  Status commit() override {
    {
      std::lock_guard<std::mutex> lk(bcp_mu_);
      for (auto& kv : bcp_buf_) {
        if (!kv.second.empty()) {
          Status st = flush_pending_locked(kv.first);
          if (!st.ok()) return st;
        }
      }
    }
    // Oracle 两段式：提交点必须把暂存表剩余行转入目标，否则尾批会丢
    if (kind_ == "oracle") {
      std::vector<std::string> pending;
      {
        std::lock_guard<std::mutex> lk(stage_mu_);
        for (auto& kv : stage_pending_)
          if (kv.second > 0) pending.push_back(kv.first);
      }
      for (const auto& t : pending) {
        auto sit = bcp_schema_.find(t);
        if (sit != bcp_schema_.end()) {
          Status st = oracle_copy_stage(t, sit->second);
          if (!st.ok()) return st;
        }
      }
    }
    if (kind_ == "oracle") {
      std::vector<std::string> pts;
      {
        std::lock_guard<std::mutex> lk(stage_mu_);
        for (auto& kv : stage_pending_)
          if (kv.second > 0) pts.push_back(kv.first);
      }
      for (const auto& t : pts) {
        auto sit = bcp_schema_.find(t);
        if (sit != bcp_schema_.end()) {
          Status st = oracle_copy_stage(t, sit->second);
          if (!st.ok()) return st;
        }
      }
    }
    std::lock_guard<std::mutex> lk(commit_mu_);
    if (!dbc_) return Status::Err(Code::kDb, "not connected");
    return SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_COMMIT) == SQL_SUCCESS
               ? Status::OK()
               : Status::Err(Code::kDb, "odbc commit failed");
  }

  Status rollback() override {
    std::lock_guard<std::mutex> lk(commit_mu_);
    if (!dbc_) return Status::OK();
    return SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_ROLLBACK) == SQL_SUCCESS
               ? Status::OK()
               : Status::Err(Code::kDb, "odbc rollback failed");
  }

  void close() override {
    {
      std::lock_guard<std::mutex> lk(bcp_mu_);
      for (auto& kv : bcp_buf_)
        if (!kv.second.empty()) flush_pending_locked(kv.first);
    }
    if (kind_ == "oracle") {
      std::vector<std::string> pending;
      {
        std::lock_guard<std::mutex> lk(stage_mu_);
        for (auto& kv : stage_pending_)
          if (kv.second > 0) pending.push_back(kv.first);
      }
      for (const auto& t : pending) {
        auto sit = bcp_schema_.find(t);
        if (sit != bcp_schema_.end()) oracle_copy_stage(t, sit->second);
      }
    }
    disconnect();
  }

  int64_t written(const std::string& table) override {
    std::lock_guard<std::mutex> lk(mu_);
    return written_[table];
  }

  bool supports_upsert() const override { return true; }
  std::string index_ddl(const std::string&, const IndexDef&) override { return ";"; }
  std::string analyze_sql(const std::string&) override { return "SELECT 1"; }
  std::string describe() override { return kind_ + " target (ODBC)"; }

 private:
  OdbcWriter* writer() {
    static thread_local std::map<const OdbcTarget*, std::unique_ptr<OdbcWriter>>
        workers;
    auto it = workers.find(this);
    if (it != workers.end()) return it->second.get();
    // unixODBC's Windows driver manager is not safe for concurrent
    // SQLDriverConnect/SQLAllocHandle calls; serialize connection setup only.
    static std::mutex connect_mu;
    std::lock_guard<std::mutex> lk(connect_mu);
    auto w = std::make_unique<OdbcWriter>(opts_);
    if (!w->open(kind_)) return nullptr;
    OdbcWriter* raw = w.get();
    workers.emplace(this, std::move(w));
    return raw;
  }

  bool bcp_ready() const {
    static const bool ok = [] {
      const char* e = std::getenv("MIG_BCP");
      if (e && *e && access(e, X_OK) == 0) return true;
      std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
      std::string cand = home + "/msodbc/tools/root/opt/mssql-tools18/bin/bcp";
      if (access(cand.c_str(), X_OK) == 0) return true;
      if (system("command -v bcp >/dev/null 2>&1") == 0) return true;
      return false;
    }();
    return ok;
  }

  std::string bcp_bin() const {
    if (!providers().bcp_path.empty() && access(providers().bcp_path.c_str(), X_OK) == 0)
      return providers().bcp_path;
    const char* e = std::getenv("MIG_BCP");
    if (e && *e && access(e, X_OK) == 0) return e;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    std::string cand = home + "/msodbc/tools/root/opt/mssql-tools18/bin/bcp";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    return "bcp";
  }

  static std::string raw_text(const Value& v) {
    switch (v.type) {
      case ValType::kNull: return std::string();
      case ValType::kInt: return std::to_string(v.i);
      case ValType::kBool: return v.b ? "1" : "0";
      case ValType::kDouble: {
        std::ostringstream o;
        o << std::setprecision(17) << v.d;
        return o.str();
      }
      default: return v.s;
    }
  }

  bool sqlldr_ready() const {
    static const bool ok = [] {
      const char* e = std::getenv("MIG_SQLLDR");
      if (e && *e && access(e, X_OK) == 0) return true;
      std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
      std::string cand = home + "/oracle-ic/instantclient_23_26/sqlldr";
      if (access(cand.c_str(), X_OK) == 0) return true;
      if (system("command -v sqlldr >/dev/null 2>&1") == 0) return true;
      return false;
    }();
    return ok;
  }

  std::string sqlldr_bin() const {
    if (!providers().sqlldr_path.empty() && access(providers().sqlldr_path.c_str(), X_OK) == 0)
      return providers().sqlldr_path;
    const char* e = std::getenv("MIG_SQLLDR");
    if (e && *e && access(e, X_OK) == 0) return e;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    std::string cand = home + "/oracle-ic/instantclient_23_26/sqlldr";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    return "sqlldr";
  }

  Status flush_pending_locked(const std::string& table) {
    return kind_ == "mssql" ? flush_bcp_locked(table) : flush_sqlldr_locked(table);
  }

  // 调用方必须持有 bcp_mu_
  Status flush_sqlldr_locked(const std::string& table) {
    auto it = bcp_buf_.find(table);
    if (it == bcp_buf_.end() || it->second.empty()) return Status::OK();
    auto sit = bcp_schema_.find(table);
    if (sit == bcp_schema_.end()) return Status::Err(Code::kDb, "sqlldr: schema missing");
    const TableSchema& s = sit->second;
    const std::string host = dsn_get(opts_.target, "host", "127.0.0.1");
    const std::string port = dsn_get(opts_.target, "port", "1521");
    const std::string user = dsn_get(opts_.target, "user", "");
    const std::string pass = resolve_secret(dsn_get(opts_.target, "password", ""));
    const std::string db = dsn_get(opts_.target, "db", "");
    if (db.empty()) return Status::Err(Code::kInvalid, "sqlldr: target service name is empty");

    static std::atomic<uint64_t> seq{0};
    const std::string base = std::filesystem::temp_directory_path().string() + "/mig_ld_" +
                             std::to_string((long long)getpid()) + "_" + table + "_" +
                             std::to_string((unsigned long long)seq.fetch_add(1));
    const std::string data = base + ".dat";
    const std::string ctl = base + ".ctl";
    const std::string log = base + ".log";
    const std::string bad = base + ".bad";
    const std::string pwfile = base + ".pw";
    {
      std::ofstream pf(pwfile);
      pf << pass << "\n";
    }
    std::filesystem::permissions(pwfile,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);

    {
      std::ofstream f(data, std::ios::binary | std::ios::trunc);
      if (!f) return Status::Err(Code::kIO, "sqlldr: cannot write " + data);
      for (const auto& row : it->second) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) f << '\t';
          if (i < s.columns.size() && col_is_binary(s.columns[i]) && row[i].type == ValType::kBytes)
            f << hex(row[i].s);
          else
            f << raw_text(row[i]);
        }
        f << '\n';
      }
    }
    {
      std::ofstream c(ctl, std::ios::trunc);
      if (!c) return Status::Err(Code::kIO, "sqlldr: cannot write " + ctl);
      c << "LOAD DATA\nINFILE '" << data << "'\nAPPEND INTO TABLE \"" << table << "\"\n";
      c << "FIELDS TERMINATED BY X'09'\nTRAILING NULLCOLS\n(\n";
      for (size_t i = 0; i < s.columns.size(); ++i) {
        const std::string t = lower_ascii(s.columns[i].src_type);
        c << "  \"" << s.columns[i].name << "\"";
        if (col_is_binary(s.columns[i])) { c << " \"TO_BLOB(HEXTORAW(:" << s.columns[i].name << "))\""; if (i + 1 < s.columns.size()) c << ","; c << "\n"; continue; }
        if (t.find("timestamp") != std::string::npos)
          c << " TIMESTAMP \"YYYY-MM-DD HH24:MI:SS.FF\"";
        else if (t.find("date") != std::string::npos)
          c << " DATE \"YYYY-MM-DD HH24:MI:SS\"";
        if (i + 1 < s.columns.size()) c << ",";
        c << "\n";
      }
      c << ")\n";
    }
    std::string qpass;
    for (char ch : pass) {
      if (ch == '\'') qpass += "'\\''";
      else qpass += ch;
    }
    bool any_bin = false;
    for (const auto& c : s.columns) if (col_is_binary(c)) any_bin = true;
    const std::string direct = any_bin ? "false" : "true";
    const std::string cmd = sqlldr_bin() + " \"userid=" + user + "/" + pass + "@//" + host +
                            ":" + port + "/" + db + "\" control=\"" + ctl + "\" log=\"" + log +
                            "\" bad=\"" + bad + "\" direct=" + direct + " errors=0 >/dev/null 2>&1";
    const int rc = system(cmd.c_str());
    const int64_t n = (int64_t)it->second.size();
    it->second.clear();
    if (rc != 0) {
      std::string err;
      std::ifstream lf(log);
      if (lf) {
        std::ostringstream ss;
        ss << lf.rdbuf();
        err = ss.str();
        if (err.size() > 400) err = err.substr(err.size() - 400);
      }
      std::remove(data.c_str());
      std::remove(ctl.c_str());
      std::remove(bad.c_str());
      return Status::Err(Code::kDb, "sqlldr failed (" + std::to_string(rc) + "): " + err);
    }
    std::remove(data.c_str());
    std::remove(ctl.c_str());
    std::remove(bad.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += n;
    return Status::OK();
  }

  // 调用方必须持有 bcp_mu_
  Status flush_bcp_locked(const std::string& table) {
    auto it = bcp_buf_.find(table);
    if (it == bcp_buf_.end() || it->second.empty()) return Status::OK();
    const std::string host = dsn_get(opts_.target, "host", "127.0.0.1");
    const std::string port = dsn_get(opts_.target, "port", "1433");
    const std::string user = dsn_get(opts_.target, "user", "sa");
    const std::string pass = resolve_secret(dsn_get(opts_.target, "password", ""));
    const std::string db = dsn_get(opts_.target, "db", "");
    if (db.empty()) return Status::Err(Code::kInvalid, "bcp: target db is empty");

    static std::atomic<uint64_t> seq{0};
    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/mig_bcp_" + std::to_string((long long)getpid()) + "_" + table + "_" +
                             std::to_string((unsigned long long)seq.fetch_add(1)) + ".tsv";
    {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) return Status::Err(Code::kIO, "bcp: cannot write " + path);
      for (const auto& row : it->second) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) f << '\t';
          f << raw_text(row[i]);
        }
        f << '\n';
      }
    }

    std::string qpass;
    for (char c : pass) {
      if (c == '\'') qpass += "'\\''";
      else qpass += c;
    }
    const std::string errfile = path + ".err";
    const std::string pwfile = path + ".pw";
    {
      std::ofstream pf(pwfile);
      pf << pass << "\n";
    }
    std::filesystem::permissions(pwfile,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    std::string cmd = bcp_bin() + " \"" + "dbo." + table + "\" in \"" + path +
                      "\" -c -t \"\\t\" -r \"\\n\" -S " + host + "," + port +
                      " -U " + user + " -d " + db +
                      " -u -b 100000 -h \"TABLOCK\" -e \"" + errfile +
                      "\" < \"" + pwfile + "\" >/dev/null 2>&1";
    const int rc = system(cmd.c_str());
    const int64_t n = (int64_t)it->second.size();
    it->second.clear();
    if (rc != 0) {
      std::string err;
      std::ifstream ef(errfile);
      if (ef) { std::ostringstream ss; ss << ef.rdbuf(); err = ss.str(); }
      std::remove(path.c_str());
      std::remove(errfile.c_str());
      return Status::Err(Code::kDb, "bcp load failed (" + std::to_string(rc) + "): " + err.substr(0, 300));
    }
    std::remove(path.c_str());
    std::remove(errfile.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += n;
    return Status::OK();
  }

  // NULL 字面量需要显式类型，否则 Oracle 的 SELECT ... FROM dual 会给出
  // CHAR(0)，MERGE 时报 ORA-00932。
  std::string typed_lit(const Value& v, const ColumnDef& c) const {
    if (v.type != ValType::kNull) return lit(v);
    return "CAST(NULL AS " + map_type(c.src_type) + ")";
  }

  Options opts_;
  std::string kind_;
  std::map<std::string, TableSchema> schemas_;
  std::map<std::string, int64_t> written_;
  std::mutex mu_;
  std::map<std::string, std::vector<Row>> bcp_buf_;
  std::map<std::string, TableSchema> bcp_schema_;
  std::mutex bcp_mu_;
  bool stage_ready_ = false;
  std::map<std::string, int64_t> stage_pending_;
  std::mutex stage_mu_;
  std::mutex schema_mu_;
  std::mutex commit_mu_;
};

}  // namespace

std::shared_ptr<ISource> make_odbc_source(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<OdbcSource>(o);
}

std::shared_ptr<ITarget> make_odbc_target(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<OdbcTarget>(o);
}

}  // namespace mig
