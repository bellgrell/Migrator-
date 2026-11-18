#include "sql_backend.h"

#include "dsn.h"

#ifdef HAVE_MYSQL
#include <mysql/mysql.h>

#include <cstring>
#include <vector>

namespace mmv {
namespace {

class MySqlBackend : public SqlBackend {
 public:
  ~MySqlBackend() override {
    if (conn_) mysql_close(conn_);
  }

  Status connect(const std::string& dsn) override {
    Dsn d{dsn};
    const std::string port = d.port().empty() ? "3306" : d.port();
    conn_ = mysql_init(nullptr);
    if (!conn_) return Status::Err(Code::kDb, "mysql_init failed");
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000 && !defined(MARIADB_VERSION_ID)
    const std::string ssl_mode = d.get("ssl-mode", "");
    if (!ssl_mode.empty()) {
      std::string m = ssl_mode;
      for (char& c : m) c = (char)std::tolower((unsigned char)c);
      unsigned int mode = SSL_MODE_PREFERRED;
      if (m == "disabled") mode = SSL_MODE_DISABLED;
      else if (m == "required") mode = SSL_MODE_REQUIRED;
      else if (m == "verify_ca" || m == "verify-ca") mode = SSL_MODE_VERIFY_CA;
      else if (m == "verify_identity" || m == "verify-identity")
        mode = SSL_MODE_VERIFY_IDENTITY;
      mysql_options(conn_, MYSQL_OPT_SSL_MODE, &mode);
    }
#endif
    unsigned int p = (unsigned int)std::stoul(port);
    const std::string pass = d.password_resolved();
    if (!mysql_real_connect(conn_, d.host().c_str(), d.user().c_str(), pass.c_str(),
                            d.dbname().c_str(), p, nullptr, 0)) {
      const std::string e = mysql_error(conn_);
      return Status::Err(Code::kDb, "mysql connect: " + e);
    }
    if (mysql_set_character_set(conn_, "utf8mb4")) {
      return Status::Err(Code::kDb, std::string("mysql charset: ") + mysql_error(conn_));
    }
    // Normalize TIMESTAMP values to UTC so they can be compared with
    // PostgreSQL timestamptz; also lift GROUP_CONCAT for JSON digests.
    if (mysql_query(conn_, "SET time_zone = '+00:00'") ||
        mysql_query(conn_, "SET SESSION group_concat_max_len = 67108864")) {
      return Status::Err(Code::kDb, std::string("mysql session: ") + mysql_error(conn_));
    }
    // Deep JSON canonicalization; best-effort for older servers.
    mysql_query(conn_, "SET SESSION cte_max_recursion_depth = 100000");
    ResultSet rs;
    Status st = query("SELECT VERSION()", rs);
    if (st.ok() && !rs.rows.empty()) version_ = rs.rows[0][0].text;
    desc_ = "mysql " + d.host() + ":" + port + "/" + d.dbname();
    return Status::OK();
  }

  Status query(const std::string& sql, ResultSet& out) override {
    out = ResultSet{};
    if (mysql_query(conn_, sql.c_str()))
      return Status::Err(Code::kDb, std::string("mysql query: ") + mysql_error(conn_));
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) {
      if (mysql_field_count(conn_) == 0) return Status::OK();  // statement, no rows
      return Status::Err(Code::kDb, std::string("mysql store: ") + mysql_error(conn_));
    }
    const unsigned nf = mysql_num_fields(r);
    MYSQL_FIELD* fields = mysql_fetch_fields(r);
    for (unsigned i = 0; i < nf; ++i) out.columns.emplace_back(fields[i].name);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r))) {
      unsigned long* lens = mysql_fetch_lengths(r);
      std::vector<Cell> cells;
      cells.reserve(nf);
      for (unsigned i = 0; i < nf; ++i) {
        Cell c;
        c.is_null = row[i] == nullptr;
        if (!c.is_null) c.text.assign(row[i], lens[i]);
        cells.push_back(std::move(c));
      }
      out.rows.push_back(std::move(cells));
    }
    mysql_free_result(r);
    return Status::OK();
  }

  Status query_stream(const std::string& sql,
                      const std::function<bool(const std::vector<Cell>&)>& fn) override {
    if (mysql_real_query(conn_, sql.c_str(), (unsigned long)sql.size()))
      return Status::Err(Code::kDb, std::string("mysql query: ") + mysql_error(conn_));
    MYSQL_RES* r = mysql_use_result(conn_);
    if (!r) {
      if (mysql_field_count(conn_) == 0) return Status::OK();
      return Status::Err(Code::kDb, std::string("mysql use_result: ") + mysql_error(conn_));
    }
    const unsigned nf = mysql_num_fields(r);
    bool keep = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r)) != nullptr) {
      if (!keep) continue;  // drain the remaining rows, then free the result
      unsigned long* lens = mysql_fetch_lengths(r);
      std::vector<Cell> cells;
      cells.reserve(nf);
      for (unsigned i = 0; i < nf; ++i) {
        Cell c;
        c.is_null = row[i] == nullptr;
        if (!c.is_null) c.text.assign(row[i], lens[i]);
        cells.push_back(std::move(c));
      }
      keep = fn(cells);
    }
    mysql_free_result(r);
    return Status::OK();
  }

  Status exec(const std::string& sql) override {
    if (mysql_query(conn_, sql.c_str()))
      return Status::Err(Code::kDb, std::string("mysql exec: ") + mysql_error(conn_));
    MYSQL_RES* r = mysql_store_result(conn_);
    if (r) mysql_free_result(r);
    return Status::OK();
  }

  Dialect dialect() const override { return Dialect::kMySQL; }
  std::string quote_ident(const std::string& n) const override { return mmv::quote_ident(n, Dialect::kMySQL); }
  std::string quote_lit(const std::string& s) const override { return mmv::quote_lit(s, Dialect::kMySQL); }
  std::string version() const override { return version_; }
  std::string describe() const override { return desc_; }

  Status set_timeout(int seconds) override {
    if (seconds <= 0) return Status::OK();
    return exec("SET SESSION max_execution_time = " + std::to_string(seconds * 1000));
  }
  Status begin_readonly() override {
    return exec("START TRANSACTION WITH CONSISTENT SNAPSHOT");
  }
  Status end_readonly() override { return exec("COMMIT"); }

  Status begin_global_read_lock() override {
    return exec("FLUSH TABLES WITH READ LOCK");
  }
  Status end_global_read_lock() override { return exec("UNLOCK TABLES"); }

  Status begin_export_snapshot(std::string& id) override {
    id.clear();
    return Status::Err(Code::kUnsupported,
                       "MySQL cannot export a cross-connection snapshot; use --assume-static");
  }

  Status table_change_token(const std::string& table, std::string& out) override {
    out.clear();
    ResultSet rs;
    Status st = query("SELECT COALESCE(COUNT_WRITE,0), COALESCE(SUM_TIMER_WRITE,0) "
                      "FROM performance_schema.table_io_waits_summary_by_table "
                      "WHERE OBJECT_SCHEMA = DATABASE() AND OBJECT_NAME = " +
                          quote_lit(table),
                      rs);
    if (st.ok() && !rs.rows.empty()) {
      const std::string c = rs.rows[0][0].is_null ? "0" : rs.rows[0][0].text;
      const std::string t = rs.rows[0][1].is_null ? "0" : rs.rows[0][1].text;
      if (c != "0") {
        out = "my-tbl:" + c + ":" + t;
        return Status::OK();
      }
    }
    return change_token(out);
  }

  Status change_token(std::string& out) override {
    out.clear();
    ResultSet rs;
    Status st = query("SHOW BINARY LOG STATUS", rs);
    if ((!st.ok() || rs.rows.empty() || rs.rows[0].size() < 2)) {
      st = query("SHOW MASTER STATUS", rs);
    }
    if (st.ok() && !rs.rows.empty() && rs.rows[0].size() >= 2) {
      const std::string file = rs.rows[0][0].is_null ? "" : rs.rows[0][0].text;
      const std::string pos = rs.rows[0][1].is_null ? "" : rs.rows[0][1].text;
      if (!file.empty() && !pos.empty()) out = "my:" + file + ":" + pos;
    }
    return Status::OK();
  }

 private:
  MYSQL* conn_ = nullptr;
  std::string version_;
  std::string desc_;
};

}  // namespace

std::shared_ptr<SqlBackend> make_mysql_backend(const std::string& dsn, Status& st) {
  auto b = std::make_shared<MySqlBackend>();
  st = b->connect(dsn);
  if (!st.ok()) return nullptr;
  return b;
}

}  // namespace mmv
#endif  // HAVE_MYSQL
