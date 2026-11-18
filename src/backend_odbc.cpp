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

class OdbcConn {
 protected:
  explicit OdbcConn(const Options& o) : opts_(o) {}
  ~OdbcConn() { disconnect(); }

  bool connect_odbc(const std::string& dsnstr, const std::string& kind) {
    kind_ = kind;
    const std::string dsn = dsn_get(dsnstr, "dsn");
    const std::string host = dsn_get(dsnstr, "host", "127.0.0.1");
    const std::string port = dsn_get(dsnstr, "port", kind == "mssql" ? "1433" : "1521");
    const std::string user = dsn_get(dsnstr, "user", kind == "mssql" ? "sa" : "");
    const std::string pass = resolve_secret(dsn_get(dsnstr, "password", ""));
    const std::string db = dsn_get(dsnstr, "db", "");
    const std::string charset = dsn_get(dsnstr, "charset", "UTF-8");
    const std::string driver = dsn_get(dsnstr, "driver", "FreeTDS");
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
          dsn_get(dsnstr, "driver", "Oracle in instantclient_23_x");
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
    for (int k = 0; k < n; ++k) {
      const SQLRETURN rc = SQLFetch(cur_);
      if (rc == SQL_NO_DATA) break;
      if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        err_ = odbc_error(SQL_HANDLE_STMT, cur_);
        return Status::Err(Code::kDb, "odbc fetch: " + err_);
      }
      Row row;
      for (SQLUSMALLINT i = 1; i <= ncols_; ++i)
        row.push_back(read_value(cur_, i, types_[i - 1]));
      out.push_back(std::move(row));
    }
    return Status::OK();
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
    for (SQLUSMALLINT i = 1; i <= ncols_; ++i) {
      char name[256];
      SQLSMALLINT nlen = 0, dtype = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      SQLDescribeCol(cur_, i, (SQLCHAR*)name, sizeof name, &nlen, &dtype, &size,
                     &digits, &nullable);
      types_[i - 1] = dtype;
    }
    return Status::OK();
  }

  void close_cursor() override {
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
    schemas_[s.name] = s;
    return exec_sql(ddl);
  }

  Status drop_table(const std::string& table) override {
    return exec_sql("DROP TABLE " + qid(table));
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
    auto it = schemas_.find(table);
    if (it == schemas_.end())
      return Status::Err(Code::kDb, "odbc target: schema not loaded for " + table);
    const auto& s = it->second;
    std::string cols;
    for (size_t i = 0; i < s.columns.size(); ++i) {
      if (i) cols += ",";
      cols += qid(s.columns[i].name);
    }
    if (kind_ == "oracle") {
      for (size_t start = 0; start < rows.size(); start += 500) {
        const size_t end = std::min(rows.size(), start + 500);
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
        Status st = exec_sql(sql);
        if (!st.ok()) return st;
      }
    } else {
      for (size_t start = 0; start < rows.size(); start += 500) {
        const size_t end = std::min(rows.size(), start + 500);
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
        Status st = exec_sql(sql);
        if (!st.ok()) return st;
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
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
    if (!dbc_) return Status::Err(Code::kDb, "not connected");
    return SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_COMMIT) == SQL_SUCCESS
               ? Status::OK()
               : Status::Err(Code::kDb, "odbc commit failed");
  }

  Status rollback() override {
    if (!dbc_) return Status::OK();
    return SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_ROLLBACK) == SQL_SUCCESS
               ? Status::OK()
               : Status::Err(Code::kDb, "odbc rollback failed");
  }

  void close() override { disconnect(); }

  int64_t written(const std::string& table) override {
    std::lock_guard<std::mutex> lk(mu_);
    return written_[table];
  }

  bool supports_upsert() const override { return true; }
  std::string index_ddl(const std::string&, const IndexDef&) override { return ";"; }
  std::string analyze_sql(const std::string&) override { return "SELECT 1"; }
  std::string describe() override { return kind_ + " target (ODBC)"; }

 private:
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
