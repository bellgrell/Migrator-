// SQL Server / Oracle CDC (ODBC).
//
// SQL Server: reads cdc.<capture_instance>_CT through
// cdc.fn_cdc_get_all_changes_<instance>(from, to, 'all update old').
// Oracle: reads V$LOGMNR_CONTENTS over SCN windows (DBMS_LOGMNR).
//
// Both paths reuse the normal target backends: every change becomes a
// CdcEvent and goes through apply_cdc_event(), so upsert/delete SQL is the
// target's own dialect. Position is persisted only after the target commit,
// which keeps the "no silent loss" invariant of the MySQL/PG paths.

#include "cdc.h"

#ifdef HAVE_ODBC

#ifdef _WIN32
// unixODBC 的 sqltypes.h 依赖 Windows 的 INT64/UINT64 定义
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends.h"
#include "dsn_secret.h"

namespace mig {
namespace {

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string upper_ascii(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
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

std::string sql_lit(const std::string& s) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'') o += "''";
    else o += c;
  }
  o += "'";
  return o;
}

std::string hex_of(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o.push_back(d[c >> 4]);
    o.push_back(d[c & 0xF]);
  }
  return o;
}

bool unhex(const std::string& h, std::string& out) {
  out.clear();
  if (h.size() % 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < h.size(); i += 2) {
    const int a = nib(h[i]), b = nib(h[i + 1]);
    if (a < 0 || b < 0) return false;
    out.push_back((char)((a << 4) | b));
  }
  return true;
}

std::string qid(const std::string& name, const std::string& kind) {
  if (kind == "mssql") {
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

std::string trim_ts(std::string s) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string odbc_diag(SQLSMALLINT type, SQLHANDLE h) {
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

// Minimal ODBC link. It intentionally duplicates the connection string
// building of backend_odbc.cpp: CDC must talk to the same DSN, but the CDC
// reader needs its own session (Oracle's LogMiner state is session-scoped).
class OdbcLink {
 public:
  ~OdbcLink() { close(); }

  bool open(const Options& o, const std::string& dsnstr, const std::string& kind,
            std::string& err, bool autocommit) {
    kind_ = kind;
    const std::string dsn = dsn_get(dsnstr, "dsn");
    const std::string host = dsn_get(dsnstr, "host", "127.0.0.1");
    const std::string port =
        dsn_get(dsnstr, "port", kind == "mssql" ? "1433" : "1521");
    const std::string user = dsn_get(dsnstr, "user", kind == "mssql" ? "sa" : "");
    const std::string pass = resolve_secret(dsn_get(dsnstr, "password", ""));
    const std::string db = dsn_get(dsnstr, "db", "");
    std::string cs;
    if (!dsn.empty()) {
      cs = "DSN=" + dsn + ";";
      if (kind == "mssql") cs += "ClientCharset=UTF-8;";
      if (!user.empty()) cs += "UID=" + user + ";";
      if (!pass.empty()) cs += "PWD=" + pass + ";";
      if (!db.empty()) cs += "DATABASE=" + db + ";";
    } else if (kind == "mssql") {
      cs = "DRIVER={FreeTDS};SERVER=" + host + ";PORT=" + port + ";UID=" + user +
           ";PWD=" + pass + ";ClientCharset=UTF-8;TDS_Version=7.4;";
      if (!db.empty()) cs += "DATABASE=" + db + ";";
    } else {
      const std::string drv =
          dsn_get(dsnstr, "driver", "Oracle in instantclient_23_x");
      cs = "DRIVER={" + drv + "};DBQ=" + host + ":" + port + "/" + db + ";UID=" +
           user + ";PWD=" + pass + ";";
    }

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
    SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
    SQLSetConnectAttr(dbc_, SQL_ATTR_LOGIN_TIMEOUT,
                      (SQLPOINTER)(intptr_t)(o.connect_timeout > 0
                                                 ? o.connect_timeout
                                                 : 10),
                      0);
    SQLCHAR out[1024];
    SQLSMALLINT outlen = 0;
    const SQLRETURN rc = SQLDriverConnect(
        dbc_, NULL, (SQLCHAR*)cs.c_str(), SQL_NTS, out, sizeof out, &outlen,
        SQL_DRIVER_NOPROMPT);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err = odbc_diag(SQL_HANDLE_DBC, dbc_);
      return false;
    }
    SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)(autocommit ? SQL_AUTOCOMMIT_ON
                                              : SQL_AUTOCOMMIT_OFF),
                      0);
    if (kind == "oracle") {
      exec("ALTER SESSION SET NLS_TIMESTAMP_FORMAT='YYYY-MM-DD HH24:MI:SS.FF6'");
      exec("ALTER SESSION SET NLS_DATE_FORMAT='YYYY-MM-DD HH24:MI:SS'");
      exec("ALTER SESSION SET NLS_TIMESTAMP_TZ_FORMAT='YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM'");
    }
    return true;
  }

  void close() {
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

  Status exec(const std::string& sql) {
    if (!dbc_) return Status::Err(Code::kDb, "odbc: not connected");
    SQLHSTMT st = SQL_NULL_HANDLE;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
    const SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)sql.c_str(), SQL_NTS);
    Status out = Status::OK();
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA)
      out = Status::Err(Code::kDb, "odbc: " + odbc_diag(SQL_HANDLE_STMT, st));
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return out;
  }

  bool query(const std::string& sql, std::vector<std::vector<std::string>>& rows,
             std::string& err) {
    rows.clear();
    if (!dbc_) { err = "not connected"; return false; }
    SQLHSTMT st = SQL_NULL_HANDLE;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
    SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)sql.c_str(), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err = odbc_diag(SQL_HANDLE_STMT, st);
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return false;
    }
    SQLSMALLINT n = 0;
    SQLNumResultCols(st, &n);
    while ((rc = SQLFetch(st)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
      std::vector<std::string> row;
      for (SQLUSMALLINT i = 1; i <= (SQLUSMALLINT)n; ++i) {
        std::string v;
        SQLLEN ind = 0;
        char buf[8192];
        while (true) {
          rc = SQLGetData(st, i, SQL_C_CHAR, buf, sizeof buf, &ind);
          if (rc == SQL_NO_DATA) break;
          if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
          if (ind == SQL_NULL_DATA) { v.clear(); break; }
          const size_t k = ind == SQL_NO_TOTAL || ind < 0
                               ? std::strlen(buf)
                               : std::min<size_t>((size_t)ind, sizeof(buf) - 1);
          v.append(buf, k);
          if (rc == SQL_SUCCESS) break;
        }
        row.push_back(v);
      }
      rows.push_back(std::move(row));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return true;
  }

  // Typed variant: binary stays binary, integers/doubles stay numeric.
  bool query_rows(const std::string& sql, std::vector<Row>& rows,
                  std::string& err) {
    rows.clear();
    if (!dbc_) { err = "not connected"; return false; }
    SQLHSTMT st = SQL_NULL_HANDLE;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &st);
    SQLRETURN rc = SQLExecDirect(st, (SQLCHAR*)sql.c_str(), SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      err = odbc_diag(SQL_HANDLE_STMT, st);
      SQLFreeHandle(SQL_HANDLE_STMT, st);
      return false;
    }
    SQLSMALLINT n = 0;
    SQLNumResultCols(st, &n);
    std::vector<SQLSMALLINT> types((size_t)n, SQL_UNKNOWN_TYPE);
    for (SQLUSMALLINT i = 1; i <= (SQLUSMALLINT)n; ++i) {
      char name[256];
      SQLSMALLINT nlen = 0, dtype = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      SQLDescribeCol(st, i, (SQLCHAR*)name, sizeof name, &nlen, &dtype, &size,
                     &digits, &nullable);
      types[i - 1] = dtype;
    }
    while ((rc = SQLFetch(st)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
      Row row;
      for (SQLUSMALLINT i = 1; i <= (SQLUSMALLINT)n; ++i)
        row.push_back(read_value(st, i, types[i - 1]));
      rows.push_back(std::move(row));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return true;
  }

 private:
  static bool is_binary_type(SQLSMALLINT t) {
    return t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY;
  }

  static Value read_value(SQLHSTMT st, SQLUSMALLINT col, SQLSMALLINT type) {
    SQLLEN ind = 0;
    if (is_binary_type(type)) {
      std::string out;
      unsigned char buf[8192];
      while (true) {
        const SQLRETURN rc =
            SQLGetData(st, col, SQL_C_BINARY, buf, sizeof buf, &ind);
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
    if (type == SQL_BIT)
      return Value::Int(out == "1" || out == "true" || out == "TRUE" ? 1 : 0);
    if (type == SQL_TYPE_TIMESTAMP || type == SQL_TIMESTAMP || type == SQL_DATE ||
        type == SQL_TIME)
      return Value::Str(trim_ts(out));
    return Value::Str(out);
  }

  SQLHENV env_ = SQL_NULL_HANDLE;
  SQLHDBC dbc_ = SQL_NULL_HANDLE;
  std::string kind_;
};

// ---------------------------------------------------------------- helpers

int pk_index_of(const TableSchema& s, const std::string& col) {
  for (size_t i = 0; i < s.columns.size(); ++i)
    if (s.columns[i].name == col) return (int)i;
  return -1;
}

std::string position_read(const std::string& path, const std::string& kind) {
  std::ifstream in(path);
  if (!in) return "";
  std::string v;
  std::getline(in, v);
  v = trim(v);
  // Positions carry a "<kind>:" prefix so a SQL Server LSN and an Oracle SCN
  // can never be silently reused across modes in the same out/ directory.
  // A position written by the other backend is ignored, not misparsed.
  const size_t c = v.find(':');
  if (c != std::string::npos && c > 0) {
    const std::string k = lower_ascii(v.substr(0, c));
    if (k != kind) return "";
    v = v.substr(c + 1);
  }
  return trim(v);
}

std::string table_selected(const Options& o, const std::string& t) {
  if (!o.only_tables.empty()) {
    bool hit = false;
    for (const auto& n : o.only_tables)
      if (lower_ascii(n) == lower_ascii(t)) { hit = true; break; }
    if (!hit) return "";
  }
  for (const auto& n : o.exclude_tables)
    if (lower_ascii(n) == lower_ascii(t)) return "";
  return t;
}

// ============================================================== SQL Server

struct MssqlCapture {
  std::string table;
  std::string instance;
};

bool mssql_cdc_db_enabled(OdbcLink& db, bool& enabled, std::string& err) {
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT CONVERT(varchar(10), is_cdc_enabled) FROM sys.databases "
                "WHERE database_id = DB_ID()",
                rows, err))
    return false;
  enabled = !rows.empty() && !rows[0].empty() && rows[0][0] == "1";
  return true;
}

bool mssql_capture_instance(OdbcLink& db, const std::string& table,
                            std::string& inst, std::string& err) {
  std::vector<std::vector<std::string>> rows;
  const std::string sql =
      "SELECT capture_instance FROM cdc.change_tables "
      "WHERE source_object_id = OBJECT_ID(N'dbo." +
      table + "')";
  if (!db.query(sql, rows, err)) return false;
  if (rows.empty() || rows[0].empty()) { inst.clear(); return true; }
  inst = rows[0][0];
  return true;
}

bool mssql_max_lsn(OdbcLink& db, std::string& lsn, std::string& err) {
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT CONVERT(varchar(50), sys.fn_cdc_get_max_lsn(), 1)",
                rows, err))
    return false;
  lsn = (rows.empty() || rows[0].empty()) ? "" : trim(rows[0][0]);
  return true;
}

bool mssql_min_lsn(OdbcLink& db, const std::string& inst, std::string& lsn,
                   std::string& err) {
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT CONVERT(varchar(50), sys.fn_cdc_get_min_lsn(" +
                    sql_lit(inst) + "), 1)",
                rows, err))
    return false;
  lsn = (rows.empty() || rows[0].empty()) ? "" : trim(rows[0][0]);
  return true;
}

Status mssql_setup(const Options& o, OdbcLink& db, Metrics& m) {
  std::string err;
  bool enabled = false;
  if (!mssql_cdc_db_enabled(db, enabled, err))
    return Status::Err(Code::kDb, "cdc setup: " + err);
  if (!enabled) {
    Status st = db.exec("EXEC sys.sp_cdc_enable_db");
    if (!st.ok()) return Status::Err(Code::kDb, "sp_cdc_enable_db: " + st.str());
    m.log("mssql cdc enabled on database");
    enabled = true;
  }
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT status_desc FROM sys.dm_server_services "
                "WHERE servicename LIKE 'SQL Server Agent%'",
                rows, err)) {
    m.log("[warn] cannot read SQL Server Agent status: " + err);
  } else if (!rows.empty() && !rows[0].empty() && rows[0][0] != "Running") {
    m.log("[warn] SQL Server Agent is " + rows[0][0] +
          "; CDC capture will not advance until it is started");
  }

  std::vector<std::string> tables = o.only_tables;
  if (tables.empty()) {
    std::vector<std::vector<std::string>> trs;
    if (!db.query("SELECT name FROM sys.tables WHERE is_ms_shipped=0 "
                  "ORDER BY name",
                  trs, err))
      return Status::Err(Code::kDb, "cdc setup: " + err);
    for (auto& r : trs)
      if (!r.empty() && !table_selected(o, r[0]).empty()) tables.push_back(r[0]);
  }
  for (const auto& t : tables) {
    std::string inst;
    if (!mssql_capture_instance(db, t, inst, err))
      return Status::Err(Code::kDb, "cdc setup: " + err);
    if (!inst.empty()) {
      m.log("mssql cdc already enabled: " + t + " (" + inst + ")");
      continue;
    }
    const std::string sql =
        "EXEC sys.sp_cdc_enable_table @source_schema=N'dbo', @source_name=N'" +
        t + "', @role_name=NULL, @supports_net_changes=1";
    Status st = db.exec(sql);
    if (!st.ok())
      return Status::Err(Code::kDb, "sp_cdc_enable_table(" + t + "): " + st.str());
    m.log("mssql cdc enabled: " + t);
  }
  return Status::OK();
}

bool mssql_fetch_window(OdbcLink& db, const TableSchema& s,
                        const std::string& inst, const std::string& from_lsn,
                        const std::string& to_lsn, std::vector<Row>& rows,
                        std::string& err) {
  std::string cols;
  for (const auto& c : s.columns) cols += ", " + qid(c.name, "mssql");
  const std::string sql =
      "SELECT __$operation, CONVERT(varchar(50), __$seqval, 1)" + cols +
      " FROM cdc.fn_cdc_get_all_changes_" + inst + "(" + from_lsn + ", " +
      to_lsn + ", 'all update old') ORDER BY __$seqval, __$operation";
  return db.query_rows(sql, rows, err);
}

// 开跑前的环境自检：把“要等 30 分钟才发现”的问题提前暴露。
Status mssql_preflight(OdbcLink& db, Metrics& m) {
  std::string err;
  bool enabled = false;
  if (!mssql_cdc_db_enabled(db, enabled, err))
    return Status::Err(Code::kDb, "preflight: " + err);
  if (!enabled)
    return Status::Err(Code::kUnsupported,
                       "preflight: CDC is not enabled on this database; rerun "
                       "with --cdc-setup (needs sysadmin)");

  std::vector<std::vector<std::string>> rows;
  if (db.query("SELECT status_desc FROM sys.dm_server_services "
               "WHERE servicename LIKE 'SQL Server Agent%'",
               rows, err)) {
    if (!rows.empty() && !rows[0].empty() && rows[0][0] != "Running")
      m.log("[warn] preflight: SQL Server Agent is " + rows[0][0] +
            "; the capture job will not advance (start the service or run "
            "EXEC sys.sp_cdc_scan manually)");
  } else {
    m.log("[warn] preflight: cannot read SQL Server Agent status: " + err);
  }

  if (!db.query("SELECT COUNT(*) FROM cdc.change_tables", rows, err))
    return Status::Err(Code::kDb, "preflight: " + err);
  if (rows.empty() || rows[0][0] == "0")
    return Status::Err(Code::kNotFound,
                       "preflight: no CDC capture instances; run --cdc-setup");
  m.log("preflight: mssql cdc ok, " + rows[0][0] + " capture instance(s)");
  return Status::OK();
}

Status run_mssql_cdc(const Options& o, Metrics& m, int max_events,
                     OdbcLink& db, ISource& src, ITarget& tgt) {
  std::string err;
  if (o.cdc_setup) {
    Status st = mssql_setup(o, db, m);
    if (!st.ok()) return st;
  }
  Status pf = mssql_preflight(db, m);
  if (!pf.ok()) return pf;

  std::vector<std::string> tables;
  std::map<std::string, std::string> instances;
  {
    std::vector<std::vector<std::string>> rows;
    const std::string sql =
        "SELECT OBJECT_SCHEMA_NAME(source_object_id) + '.' + "
        "OBJECT_NAME(source_object_id), capture_instance FROM cdc.change_tables";
    if (!db.query(sql, rows, err))
      return Status::Err(Code::kDb, "cdc: " + err);
    for (auto& r : rows) {
      if (r.size() < 2) continue;
      std::string name = r[1 - 1];
      const size_t dot = name.find('.');
      if (dot != std::string::npos) name = name.substr(dot + 1);
      if (table_selected(o, name).empty()) continue;
      if (lower_ascii(r[0].substr(0, 3)) == "dbo") {
        tables.push_back(name);
        instances[name] = r[1];
      }
    }
  }
  if (tables.empty())
    return Status::Err(Code::kUnsupported,
                       "no CDC-enabled tables found; rerun with --cdc-setup");

  std::string from_lsn;
  if (!o.cdc_start.empty()) {
    from_lsn = o.cdc_start;
  } else {
    from_lsn = position_read(o.odbc_cdc_position_path, "mssql");
  }
  if (from_lsn.empty()) {
    // 刚启用 CDC 且还没有任何事务时 fn_cdc_get_max_lsn() 返回 NULL；
    // 等待第一个被捕获的事务，而不是直接失败。
    int waits = 0;
    while (from_lsn.empty()) {
      if (!mssql_max_lsn(db, from_lsn, err))
        return Status::Err(Code::kDb,
                           "cannot read sys.fn_cdc_get_max_lsn: " + err);
      if (!from_lsn.empty()) break;
      if (++waits > 30)
        return Status::Err(
            Code::kDb,
            "CDC is enabled but no transaction has been captured yet; "
            "is SQL Server Agent running? (EXEC sys.sp_cdc_scan forces a scan)");
      std::this_thread::sleep_for(std::chrono::milliseconds(o.cdc_poll_ms));
    }
    m.log("cdc start (now): " + from_lsn);
  } else {
    m.log("cdc start (saved): " + from_lsn);
  }
  const std::string until_lsn = trim(o.cdc_until_lsn);

  int applied = 0;
  int reconnects = 0;
  while (true) {
    std::string to_lsn;
    if (!mssql_max_lsn(db, to_lsn, err)) {
      m.log("[warn] cdc lsn query: " + err);
      if (++reconnects > (o.cdc_retries > 0 ? o.cdc_retries : 0)) return Status::Err(Code::kDb, err);
      std::this_thread::sleep_for(std::chrono::milliseconds(o.cdc_poll_ms));
      continue;
    }
    if (to_lsn.empty() || lower_ascii(to_lsn) <= lower_ascii(from_lsn)) {
      if (!until_lsn.empty() && lower_ascii(from_lsn) >= lower_ascii(until_lsn)) {
        m.log("cdc caught up to " + until_lsn);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(o.cdc_poll_ms));
      continue;
    }

    int batch_events = 0;
    bool batch_ok = true;
    for (const auto& t : tables) {
      TableSchema s;
      Status ss = src.read_schema(t, s);
      if (!ss.ok()) {
        m.errors += 1;
        m.log("[error] cdc schema " + t + ": " + ss.str());
        batch_ok = false;
        break;
      }

      std::string min_lsn;
      if (mssql_min_lsn(db, instances[t], min_lsn, err) && !min_lsn.empty() &&
          lower_ascii(from_lsn) < lower_ascii(min_lsn)) {
        return Status::Err(
            Code::kDb,
            "cdc position " + from_lsn + " is older than the retention window (" +
                min_lsn + ") for " + t);
      }

      std::vector<Row> rows;
      if (!mssql_fetch_window(db, s, instances[t], from_lsn, to_lsn, rows, err)) {
        m.errors += 1;
        m.log("[error] cdc read " + t + ": " + err);
        batch_ok = false;
        break;
      }

      std::map<std::string, Row> pending;
      for (auto& r : rows) {
        if (r.size() != 2 + s.columns.size()) {
          m.errors += 1;
          m.log("[error] cdc " + t +
                ": captured column list mismatch (did @captured_column_list "
                "restrict the capture instance?)");
          batch_ok = false;
          break;
        }
        const int op = (int)r[0].i;
        const std::string seq = r[1].type == ValType::kString ? r[1].s : "";
        Row img(r.begin() + 2, r.end());

        CdcEvent ev;
        ev.table = t;
        bool emit = false;
        if (op == 2) {  // insert
          ev.op = 0;
          ev.after = img;
          emit = true;
        } else if (op == 1) {  // delete
          ev.op = 2;
          ev.before = img;
          emit = true;
        } else if (op == 3) {  // update, before image
          pending[seq] = img;
        } else if (op == 4) {  // update, after image
          auto it = pending.find(seq);
          if (it != pending.end()) {
            bool pk_changed = false;
            for (const auto& pk : s.primary_key) {
              const int idx = pk_index_of(s, pk);
              if (idx < 0) continue;
              if (it->second[(size_t)idx].sql_literal() !=
                  img[(size_t)idx].sql_literal())
                pk_changed = true;
            }
            if (pk_changed) {
              CdcEvent del;
              del.table = t;
              del.op = 2;
              del.before = it->second;
              Status ds = apply_cdc_event(tgt, s, del);
              if (!ds.ok()) {
                m.errors += 1;
                m.log("[error] cdc apply " + t + ": " + ds.str());
                batch_ok = false;
                break;
              }
              ++batch_events;
            }
            pending.erase(it);
          }
          ev.op = 0;
          ev.after = img;
          emit = true;
        }
        if (!batch_ok) break;
        if (!emit) continue;
        if (table_selected(o, ev.table).empty()) continue;

        Status as = apply_cdc_event(tgt, s, ev);
        if (!as.ok()) {
          m.errors += 1;
          m.log("[error] cdc apply " + t + ": " + as.str());
          batch_ok = false;
          break;
        }
        ++batch_events;
        ++applied;
        m.rows_done += 1;
      }
      if (!batch_ok) break;
    }

    if (!batch_ok) {
      tgt.rollback();
      return Status::Err(Code::kInvalid,
                         "cdc batch failed; position not advanced");
    }
    Status cs = tgt.commit();
    if (!cs.ok()) {
      m.errors += 1;
      m.log("[error] cdc commit: " + cs.str());
      return Status::Err(Code::kDb, "cdc commit failed; position not advanced");
    }
    if (!write_text_atomic(o.odbc_cdc_position_path, "mssql:" + to_lsn))
      m.log("[warn] cannot persist cdc position");
    from_lsn = to_lsn;
    reconnects = 0;
    m.log("cdc applied " + std::to_string(batch_events) + " events, lsn=" +
          to_lsn);

    if (!until_lsn.empty() && lower_ascii(from_lsn) >= lower_ascii(until_lsn)) {
      m.log("cdc caught up to " + until_lsn);
      break;
    }
    if (max_events > 0 && applied >= max_events) break;
  }
  return Status::OK();
}

// ================================================================== Oracle

std::string ora_ident(const std::string& quoted) {
  std::string s = trim(quoted);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string o;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '"' && i + 2 < s.size() && s[i + 1] == '"') { o += '"'; ++i; }
      else o += s[i];
    }
    return o;
  }
  if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
    std::string o;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == ']' && i + 2 < s.size() && s[i + 1] == ']') { o += ']'; ++i; }
      else o += s[i];
    }
    return o;
  }
  return s;
}

// Split on a top-level separator, ignoring separators inside quotes/parens.
std::vector<std::string> split_top(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool q = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      cur += c;
      if (c == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') { cur += s[++i]; }
        else q = false;
      }
      continue;
    }
    if (c == '\'') { q = true; cur += c; continue; }
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == sep && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
    cur += c;
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

bool is_word_at(const std::string& s, size_t pos, const std::string& w) {
  if (pos + w.size() > s.size()) return false;
  if (lower_ascii(s.substr(pos, w.size())) != w) return false;
  const bool lb = pos == 0 || !std::isalnum((unsigned char)s[pos - 1]);
  const bool rb = pos + w.size() >= s.size() ||
                  !std::isalnum((unsigned char)s[pos + w.size()]);
  return lb && rb;
}

// Find a keyword outside quotes/parens.
size_t find_word(const std::string& s, const std::string& w, size_t from = 0) {
  int depth = 0;
  bool q = false;
  for (size_t i = from; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      if (c == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') ++i;
        else q = false;
      }
      continue;
    }
    if (c == '\'') { q = true; continue; }
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (depth == 0 && is_word_at(s, i, w)) return i;
  }
  return std::string::npos;
}

std::vector<std::string> split_word(const std::string& s, const std::string& w) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t p = find_word(s, w, start);
    if (p == std::string::npos) { out.push_back(trim(s.substr(start))); break; }
    out.push_back(trim(s.substr(start, p - start)));
    start = p + w.size();
  }
  return out;
}

std::string strip_semicolon(std::string s) {
  s = trim(s);
  while (!s.empty() && (s.back() == ';' || s.back() == '\n' || s.back() == ' '))
    s.pop_back();
  return trim(s);
}

Value parse_ora_literal(const std::string& raw, const ColumnDef& c) {
  std::string v = trim(raw);
  if (v.empty()) return Value::Null();
  if (lower_ascii(v) == "null") return Value::Null();
  if ((v[0] == 'N' || v[0] == 'n') && v.size() > 1 && v[1] == '\'')
    v = v.substr(1);
  if (v.front() == '\'') {
    std::string o;
    for (size_t i = 1; i < v.size(); ++i) {
      if (v[i] == '\'') {
        if (i + 1 < v.size() && v[i + 1] == '\'') { o += '\''; ++i; }
        else break;
      } else o += v[i];
    }
    return Value::Str(o);
  }
  const std::string lv = lower_ascii(v);
  if (lv.rfind("to_date(", 0) == 0 || lv.rfind("to_timestamp(", 0) == 0 ||
      lv.rfind("to_timestamp_tz(", 0) == 0) {
    const size_t q1 = v.find('\'');
    if (q1 != std::string::npos) {
      std::string o;
      for (size_t i = q1 + 1; i < v.size(); ++i) {
        if (v[i] == '\'') {
          if (i + 1 < v.size() && v[i + 1] == '\'') { o += '\''; ++i; }
          else break;
        } else o += v[i];
      }
      return Value::Str(o);
    }
  }
  if (lv.rfind("hextoraw(", 0) == 0) {
    const size_t q1 = v.find('\'');
    const size_t q2 = v.rfind('\'');
    if (q1 != std::string::npos && q2 > q1) {
      std::string bytes;
      if (unhex(v.substr(q1 + 1, q2 - q1 - 1), bytes))
        return Value::Bytes(bytes);
    }
  }
  const std::string base = lower_ascii(c.src_type);
  if (base.find("char") != std::string::npos || base.find("clob") != std::string::npos)
    return Value::Str(v);
  if (base.find("number") != std::string::npos ||
      base.find("int") != std::string::npos) {
    if (v.find('.') == std::string::npos &&
        v.find('e') == std::string::npos && v.find('E') == std::string::npos)
      return Value::Int(std::strtoll(v.c_str(), nullptr, 10));
    return Value::Dbl(std::strtod(v.c_str(), nullptr));
  }
  if (base.find("float") != std::string::npos ||
      base.find("double") != std::string::npos)
    return Value::Dbl(std::strtod(v.c_str(), nullptr));
  return Value::Str(v);
}

size_t matching_paren(const std::string& s, size_t open) {
  int depth = 0;
  bool q = false;
  for (size_t i = open; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      if (c == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') ++i;
        else q = false;
      }
      continue;
    }
    if (c == '\'') { q = true; continue; }
    if (c == '(') ++depth;
    if (c == ')') { if (--depth == 0) return i; }
  }
  return std::string::npos;
}

struct OraChange {
  std::string table;
  int op = 0;  // 0 insert, 1 update, 2 delete
  std::map<std::string, std::string> set;   // update: column -> literal
  std::map<std::string, std::string> where; // column -> literal
  std::map<std::string, std::string> values;  // insert: column -> literal
};

bool parse_ora_redo(const std::string& redo, OraChange& out, std::string& err) {
  std::string s = strip_semicolon(redo);
  const std::string ls = lower_ascii(s);
  if (ls.rfind("insert into", 0) == 0) {
    out.op = 0;
    const size_t p1 = s.find('(');
    const size_t p2 = matching_paren(s, p1);
    const size_t vw = find_word(s, "values", p1);
    if (p1 == std::string::npos || p2 == std::string::npos ||
        vw == std::string::npos) {
      err = "insert redo not understood";
      return false;
    }
    const size_t v1 = s.find('(', vw);
    const size_t v2 = matching_paren(s, v1);
    if (v1 == std::string::npos || v2 == std::string::npos) {
      err = "insert values not understood";
      return false;
    }
    auto cols = split_top(s.substr(p1 + 1, p2 - p1 - 1), ',');
    auto vals = split_top(s.substr(v1 + 1, v2 - v1 - 1), ',');
    if (cols.size() != vals.size()) {
      err = "insert column/value count mismatch";
      return false;
    }
    for (size_t i = 0; i < cols.size(); ++i)
      out.values[ora_ident(cols[i])] = vals[i];
    return true;
  }
  if (ls.rfind("update", 0) == 0) {
    out.op = 1;
    const size_t sw = find_word(s, "set");
    const size_t ww = find_word(s, "where", sw == std::string::npos ? 0 : sw);
    if (sw == std::string::npos) { err = "update without set"; return false; }
    const std::string setpart =
        s.substr(sw + 3, (ww == std::string::npos ? s.size() : ww) - sw - 3);
    for (const auto& a : split_top(setpart, ',')) {
      const size_t eq = a.find('=');
      if (eq == std::string::npos) continue;
      out.set[ora_ident(a.substr(0, eq))] = trim(a.substr(eq + 1));
    }
    if (ww != std::string::npos) {
      for (const auto& a : split_word(s.substr(ww + 5), "and")) {
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        out.where[ora_ident(a.substr(0, eq))] = trim(a.substr(eq + 1));
      }
    }
    return true;
  }
  if (ls.rfind("delete from", 0) == 0) {
    out.op = 2;
    const size_t ww = find_word(s, "where");
    if (ww != std::string::npos) {
      for (const auto& a : split_word(s.substr(ww + 5), "and")) {
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        out.where[ora_ident(a.substr(0, eq))] = trim(a.substr(eq + 1));
      }
    }
    return true;
  }
  err = "unsupported redo: " + s.substr(0, 60);
  return false;
}

std::string ora_where(const TableSchema& s,
                      const std::map<std::string, std::string>& where) {
  std::string w;
  for (const auto& pk : s.primary_key) {
    auto it = where.find(pk);
    if (it == where.end()) {
      // Oracle returns identifiers upper-case; try case-insensitive.
      for (const auto& kv : where)
        if (lower_ascii(kv.first) == lower_ascii(pk)) { it = where.find(kv.first); break; }
    }
    if (it == where.end()) return "";
    if (!w.empty()) w += " AND ";
    w += qid(pk, "oracle") + "=" + it->second;
  }
  return w;
}

bool ora_hydrate(OdbcLink& db, const TableSchema& s, const std::string& where,
                 Row& out, std::string& err) {
  std::vector<Row> rows;
  const std::string sql = "SELECT * FROM " + qid(s.name, "oracle") +
                          " WHERE " + where;
  if (!db.query_rows(sql, rows, err)) return false;
  if (rows.empty()) return false;
  out = rows[0];
  return out.size() == s.columns.size();
}

bool ora_scn(OdbcLink& db, uint64_t& scn, std::string& err) {
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT CURRENT_SCN FROM V$DATABASE", rows, err)) return false;
  if (rows.empty() || rows[0].empty()) { err = "no SCN"; return false; }
  scn = std::strtoull(rows[0][0].c_str(), nullptr, 10);
  return true;
}

Status ora_setup(const Options& o, OdbcLink& db, const std::string& schema,
                 Metrics& m) {
  std::string err;
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT supplemental_log_data_min, supplemental_log_data_all "
                "FROM V$DATABASE",
                rows, err))
    return Status::Err(Code::kDb, "logminer setup: " + err);
  const std::string min = rows.empty() ? "" : rows[0][0];
  const std::string all = rows.size() > 0 && rows[0].size() > 1 ? rows[0][1] : "";
  if (lower_ascii(min) != "yes") {
    Status st = db.exec("ALTER DATABASE ADD SUPPLEMENTAL LOG DATA");
    if (!st.ok())
      return Status::Err(Code::kDb,
                         "ADD SUPPLEMENTAL LOG DATA needs SYSDBA: " + st.str());
    m.log("oracle supplemental log data enabled");
  }
  if (lower_ascii(all) != "yes") {
    Status st = db.exec("ALTER DATABASE ADD SUPPLEMENTAL LOG DATA (ALL) COLUMNS");
    if (!st.ok())
      m.log("[warn] ALL COLUMNS supplemental logging not enabled: " + st.str() +
            " (DELETE/UPDATE redo may lack non-key before images)");
    else
      m.log("oracle supplemental log data (all columns) enabled");
  }

  std::vector<std::string> tables = o.only_tables;
  if (tables.empty()) {
    std::vector<std::vector<std::string>> trs;
    if (!db.query("SELECT TABLE_NAME FROM USER_TABLES ORDER BY TABLE_NAME", trs,
                  err))
      return Status::Err(Code::kDb, "logminer setup: " + err);
    for (auto& r : trs)
      if (!r.empty() && !table_selected(o, r[0]).empty()) tables.push_back(r[0]);
  }
  for (auto t : tables) {
    t = upper_ascii(t);
    Status st = db.exec("ALTER TABLE " + qid(t, "oracle") +
                        " ADD SUPPLEMENTAL LOG DATA (ALL) COLUMNS");
    if (!st.ok()) m.log("[warn] supplemental log for " + t + ": " + st.str());
  }
  (void)schema;
  return Status::OK();
}

Status oracle_preflight(OdbcLink& db, Metrics& m) {
  std::string err;
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT LOG_MODE, OPEN_MODE FROM V$DATABASE", rows, err))
    return Status::Err(Code::kDb, "preflight: " + err);
  if (rows.empty() || rows[0].size() < 2)
    return Status::Err(Code::kDb, "preflight: cannot read V$DATABASE");
  const std::string log_mode = upper_ascii(rows[0][0]);
  const std::string open_mode = upper_ascii(rows[0][1]);
  if (open_mode.find("READ WRITE") == std::string::npos)
    return Status::Err(Code::kUnsupported,
                       "preflight: database is not open read-write (" +
                           rows[0][1] + ")");
  if (log_mode == "NOARCHIVELOG")
    m.log("[warn] preflight: NOARCHIVELOG — only changes still in online redo "
          "can be mined; long downtime will lose history");

  if (db.query("SELECT supplemental_log_data_min, supplemental_log_data_all "
               "FROM V$DATABASE",
               rows, err) &&
      !rows.empty()) {
    const std::string mn = upper_ascii(rows[0][0]);
    const std::string all = rows[0].size() > 1 ? upper_ascii(rows[0][1]) : "";
    if (mn != "YES" && all != "YES")
      m.log("[warn] preflight: database supplemental logging is off; per-table "
            "ALL COLUMNS logging (--cdc-setup) is required for DELETE/UPDATE "
            "before-images");
  } else {
    m.log("[warn] preflight: cannot read supplemental logging flags: " + err);
  }

  if (db.query("SELECT COUNT(*) FROM session_privs WHERE privilege='LOGMINING'",
               rows, err)) {
    if (rows.empty() || rows[0][0] == "0")
      return Status::Err(Code::kUnsupported,
                         "preflight: LogMiner requires the LOGMINING privilege "
                         "(grant it or connect as SYSDBA)");
  } else {
    m.log("[warn] preflight: cannot read SESSION_PRIVS: " + err);
  }
  m.log("preflight: oracle logminer ready");
  return Status::OK();
}

Status run_oracle_cdc(const Options& o, Metrics& m, int max_events,
                      OdbcLink& db, ISource& src, ITarget& tgt) {
  std::string err;
  std::vector<std::vector<std::string>> rows;
  if (!db.query("SELECT USER FROM DUAL", rows, err) || rows.empty())
    return Status::Err(Code::kDb, "oracle cdc: " + err);
  const std::string schema = rows[0][0];

  if (o.cdc_setup) {
    Status st = ora_setup(o, db, schema, m);
    if (!st.ok()) return st;
  }
  Status pf = oracle_preflight(db, m);
  if (!pf.ok()) return pf;

  std::vector<std::string> tables = o.only_tables;
  if (tables.empty()) {
    std::vector<std::vector<std::string>> trs;
    if (!db.query("SELECT TABLE_NAME FROM USER_TABLES ORDER BY TABLE_NAME", trs,
                  err))
      return Status::Err(Code::kDb, "oracle cdc: " + err);
    for (auto& r : trs)
      if (!r.empty() && !table_selected(o, r[0]).empty()) tables.push_back(r[0]);
  }
  for (auto& t : tables) t = upper_ascii(t);
  if (tables.empty())
    return Status::Err(Code::kNotFound, "oracle cdc: no tables selected");

  uint64_t from_scn = 0;
  const std::string saved = o.cdc_start.empty()
                                ? position_read(o.odbc_cdc_position_path, "oracle")
                                : o.cdc_start;
  if (!saved.empty()) {
    from_scn = std::strtoull(saved.c_str(), nullptr, 10);
    m.log("cdc start (saved): scn " + std::to_string(from_scn));
  }
  uint64_t cur_scn = 0;
  if (!ora_scn(db, cur_scn, err))
    return Status::Err(Code::kDb, "oracle cdc: " + err);
  if (from_scn == 0) {
    from_scn = cur_scn;  // like MySQL: start at "now"
    m.log("cdc start (now): scn " + std::to_string(from_scn));
  }
  const uint64_t until_scn =
      o.cdc_until_lsn.empty() ? 0 : std::strtoull(o.cdc_until_lsn.c_str(), nullptr, 10);

  std::string in_list;
  for (const auto& t : tables) {
    if (!in_list.empty()) in_list += ",";
    in_list += sql_lit(t);
  }

  int applied = 0;
  int reconnects = 0;
  while (true) {
    if (!ora_scn(db, cur_scn, err)) {
      m.log("[warn] oracle scn: " + err);
      if (++reconnects > (o.cdc_retries > 0 ? o.cdc_retries : 0))
        return Status::Err(Code::kDb, "oracle cdc: " + err);
      std::this_thread::sleep_for(std::chrono::milliseconds(o.cdc_poll_ms));
      continue;
    }
    if (until_scn > 0 && cur_scn > until_scn) cur_scn = until_scn;
    if (cur_scn <= from_scn) {
      if (until_scn > 0 && from_scn >= until_scn) {
        m.log("cdc caught up to scn " + std::to_string(until_scn));
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(o.cdc_poll_ms));
      continue;
    }

    const std::string start_sql =
        "BEGIN DBMS_LOGMNR.START_LOGMNR(STARTSCN => " +
        std::to_string(from_scn + 1) + ", ENDSCN => " + std::to_string(cur_scn) +
        ", OPTIONS => DBMS_LOGMNR.DICT_FROM_ONLINE_CATALOG + "
        "DBMS_LOGMNR.COMMITTED_DATA_ONLY); END;";
    Status st = db.exec(start_sql);
    if (!st.ok()) {
      m.errors += 1;
      m.log("[error] START_LOGMNR: " + st.str());
      return Status::Err(Code::kDb, "oracle cdc: " + st.str());
    }

    std::vector<std::vector<std::string>> evs;
    const std::string q =
        "SELECT SCN, OPERATION, TABLE_NAME, SQL_REDO FROM V$LOGMNR_CONTENTS "
        "WHERE SEG_OWNER = " +
        sql_lit(schema) + " AND OPERATION IN ('INSERT','UPDATE','DELETE') AND " +
        "TABLE_NAME IN (" + in_list + ") AND CSF = 0 AND COMMIT_SCN IS NOT NULL " +
        "ORDER BY SCN, RS_ID";
    const bool ok = db.query(q, evs, err);
    db.exec("BEGIN DBMS_LOGMNR.END_LOGMNR; END;");
    if (!ok) {
      m.errors += 1;
      m.log("[error] logmnr query: " + err);
      return Status::Err(Code::kDb, "oracle cdc: " + err);
    }

    bool batch_ok = true;
    int batch_events = 0;
    for (auto& r : evs) {
      if (r.size() < 4) continue;
      const std::string table = r[2];
      const std::string redo = r[3];
      if (table_selected(o, table).empty()) continue;

      TableSchema s;
      Status ss = src.read_schema(table, s);
      if (!ss.ok()) {
        m.errors += 1;
        m.log("[error] cdc schema " + table + ": " + ss.str());
        batch_ok = false;
        break;
      }

      OraChange ch;
      ch.table = table;
      if (!parse_ora_redo(redo, ch, err)) {
        m.errors += 1;
        m.log("[error] redo parse " + table + ": " + err);
        batch_ok = false;
        break;
      }

      Row img;
      CdcEvent ev;
      ev.table = table;
      if (ch.op == 2) {
        const std::string w = ora_where(s, ch.where);
        if (w.empty()) {
          m.errors += 1;
          m.log("[error] delete without full primary key: " + redo.substr(0, 120));
          batch_ok = false;
          break;
        }
        img.assign(s.columns.size(), Value::Null());
        for (const auto& pk : s.primary_key) {
          for (const auto& kv : ch.where) {
            if (lower_ascii(kv.first) != lower_ascii(pk)) continue;
            const int idx = pk_index_of(s, pk);
            if (idx >= 0) img[(size_t)idx] = parse_ora_literal(kv.second, s.columns[(size_t)idx]);
          }
        }
        ev.op = 2;
        ev.before = img;
      } else {
        // INSERT redo carries the full column list; UPDATE redo only carries
        // the changed columns. Hydrate from the source by primary key so the
        // target receives a complete after-image; fall back to the redo column
        // list for an INSERT whose row is already gone.
        Row full(s.columns.size(), Value::Null());
        bool have_all = (ch.op == 0);
        for (size_t i = 0; i < s.columns.size(); ++i) {
          auto it = ch.values.end();
          for (auto kv = ch.values.begin(); kv != ch.values.end(); ++kv)
            if (lower_ascii(kv->first) == lower_ascii(s.columns[i].name)) { it = kv; break; }
          if (it == ch.values.end()) { have_all = false; continue; }
          full[i] = parse_ora_literal(it->second, s.columns[i]);
        }
        const std::string w = ora_where(s, ch.op == 0 ? ch.values : ch.where);
        Row hydrated;
        if (!w.empty() && ora_hydrate(db, s, w, hydrated, err)) {
          img = hydrated;
        } else if (ch.op == 0 && have_all) {
          img = full;
        } else {
          m.errors += 1;
          m.log("[error] cannot hydrate " + table + " for " +
                (ch.op == 0 ? "insert" : "update") + ": " + err);
          batch_ok = false;
          break;
        }
        ev.op = 0;
        ev.after = img;
      }

      Status as = apply_cdc_event(tgt, s, ev);
      if (!as.ok()) {
        m.errors += 1;
        m.log("[error] cdc apply " + table + ": " + as.str());
        batch_ok = false;
        break;
      }
      ++batch_events;
      ++applied;
      m.rows_done += 1;
    }

    if (!batch_ok) {
      tgt.rollback();
      return Status::Err(Code::kInvalid,
                         "cdc batch failed; position not advanced");
    }
    Status cs = tgt.commit();
    if (!cs.ok()) {
      m.errors += 1;
      m.log("[error] cdc commit: " + cs.str());
      return Status::Err(Code::kDb, "cdc commit failed; position not advanced");
    }
    if (!write_text_atomic(o.odbc_cdc_position_path,
                           "oracle:" + std::to_string(cur_scn)))
      m.log("[warn] cannot persist cdc position");
    from_scn = cur_scn;
    reconnects = 0;
    m.log("cdc applied " + std::to_string(batch_events) + " events, scn=" +
          std::to_string(from_scn));

    if (until_scn > 0 && from_scn >= until_scn) {
      m.log("cdc caught up to scn " + std::to_string(until_scn));
      break;
    }
    if (max_events > 0 && applied >= max_events) break;
  }
  return Status::OK();
}

}  // namespace

Status run_odbc_cdc(const Options& o, Metrics& m, int max_events) {
  const bool mssql = o.source_kind == "mssql";
  const bool oracle = o.source_kind == "oracle";
  if (!mssql && !oracle)
    return Status::Err(Code::kUnsupported,
                       "ODBC CDC requires a SQL Server or Oracle source");

  OdbcLink db;
  std::string err;
  if (!db.open(o, o.source, o.source_kind, err, true))
    return Status::Err(Code::kDb, "cdc source connect: " + err);

  Status s1;
  auto src = make_source(o, s1);
  if (!src) return Status::Err(Code::kInvalid, "no source backend");
  Status sc = src->connect();
  if (!sc.ok()) return Status::Err(Code::kDb, "cdc source: " + sc.str());

  Status s2;
  auto tgt = make_target(o, s2);
  if (!tgt) return Status::Err(Code::kInvalid, "no target backend");
  Status tc = tgt->connect();
  if (!tc.ok()) return Status::Err(Code::kDb, "cdc target: " + tc.str());

  return mssql ? run_mssql_cdc(o, m, max_events, db, *src, *tgt)
               : run_oracle_cdc(o, m, max_events, db, *src, *tgt);
}

}  // namespace mig

#else  // HAVE_ODBC

namespace mig {
Status run_odbc_cdc(const Options&, Metrics&, int) {
  return Status::Err(Code::kUnsupported, "ODBC CDC not built");
}
}  // namespace mig

#endif  // HAVE_ODBC
