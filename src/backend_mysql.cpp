#include "backends.h"

#include <mysql/mysql.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mig {
namespace {

const char kBt = (char)96;  // MySQL 标识符引号

// MySQL 的 DATETIME(6) 会输出 ".000000" / ".120000"，PG 会省略零小数部分，
// 校验前统一裁掉末尾的 0，避免 checksum 假阳性
std::string trim_fraction(std::string s) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
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

class MySqlSource : public ISource {
 public:
  explicit MySqlSource(const Options& o) : opts_(o) {}
  ~MySqlSource() override { disconnect(); }

  Status connect() override {
    conn_ = mysql_init(nullptr);
    if (!conn_) return Status::Err(Code::kDb, "mysql_init failed");
    unsigned int ct = (unsigned int)(opts_.connect_timeout > 0 ? opts_.connect_timeout : 10);
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &ct);
    mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &ct);
    mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &ct);
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000
    unsigned int sm = SSL_MODE_PREFERRED;
    if (opts_.mysql_ssl_mode == "DISABLED") sm = SSL_MODE_DISABLED;
    else if (opts_.mysql_ssl_mode == "REQUIRED") sm = SSL_MODE_REQUIRED;
    mysql_options(conn_, MYSQL_OPT_SSL_MODE, &sm);
#endif
    const std::string& dsn =
        opts_.mysql_source_dsn.empty() ? opts_.source : opts_.mysql_source_dsn;
    const std::string host = dsn_get(dsn, "host", "127.0.0.1");
    const std::string port = dsn_get(dsn, "port", "3306");
    const std::string user = dsn_get(dsn, "user", "root");
    const std::string pass = dsn_get(dsn, "password", "");
    const std::string db = dsn_get(dsn, "db", "");

    if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), pass.c_str(),
                            db.empty() ? nullptr : db.c_str(),
                            (unsigned int)std::atoi(port.c_str()), nullptr, 0)) {
      const std::string e = mysql_error(conn_);
      mysql_close(conn_);
      conn_ = nullptr;
      return Status::Err(Code::kDb, "mysql connect: " + e);
    }
    mysql_set_character_set(conn_, "utf8mb4");
    return Status::OK();
  }

  Status list_tables(std::vector<std::string>& out) override {
    out.clear();
    if (mysql_query(conn_, "SHOW FULL TABLES WHERE Table_type = 'BASE TABLE'"))
      return err("SHOW TABLES");
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("SHOW TABLES result");
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r)) != nullptr) {
      if (row[0]) out.emplace_back(row[0]);
    }
    mysql_free_result(r);
    return Status::OK();
  }

  Status read_schema(const std::string& table, TableSchema& out) override {
    out = TableSchema();
    out.name = table;

    const std::string q =
        "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA "
        "FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '" + esc(table) + "' "
        "ORDER BY ORDINAL_POSITION";

    if (mysql_query(conn_, q.c_str())) return err("read_schema");
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("read_schema result");

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r)) != nullptr) {
      ColumnDef c;
      c.name = row[0] ? row[0] : "";
      c.src_type = row[1] ? row[1] : "";
      c.nullable = !(row[2] && std::strcmp(row[2], "NO") == 0);
      c.primary = row[3] && std::strcmp(row[3], "PRI") == 0;
      c.auto_increment = row[4] && std::strstr(row[4], "auto_increment") != nullptr;
      if (c.src_type.find("blob") != std::string::npos ||
          c.src_type.find("binary") != std::string::npos) {
        c.binary = true;
      }
      out.columns.push_back(c);
      if (c.primary) out.primary_key.push_back(c.name);
    }
    mysql_free_result(r);
    if (out.columns.empty())
      return Status::Err(Code::kNotFound, "table not found: " + table);

    const std::string qi =
        "SELECT INDEX_NAME, COLUMN_NAME, NON_UNIQUE, SEQ_IN_INDEX "
        "FROM information_schema.STATISTICS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '" + esc(table) + "' "
        "AND INDEX_NAME <> 'PRIMARY' ORDER BY INDEX_NAME, SEQ_IN_INDEX";
    if (mysql_query(conn_, qi.c_str()) == 0) {
      MYSQL_RES* ri = mysql_store_result(conn_);
      if (ri) {
        std::string cur;
        IndexDef idx;
        MYSQL_ROW irow;
        while ((irow = mysql_fetch_row(ri)) != nullptr) {
          const std::string name = irow[0] ? irow[0] : "";
          if (name != cur) {
            if (!cur.empty()) out.indexes.push_back(idx);
            cur = name;
            idx = IndexDef();
            idx.name = name;
            idx.unique = irow[2] && std::strcmp(irow[2], "0") == 0;
          }
          if (irow[1]) idx.columns.push_back(irow[1]);
        }
        if (!cur.empty()) out.indexes.push_back(idx);
        mysql_free_result(ri);
      }
    }
    return Status::OK();
  }

  Status open_cursor(const std::string& table) override {
    close_cursor();
    const std::string q = std::string("SELECT * FROM ") + kBt + table + kBt;
    if (mysql_query(conn_, q.c_str())) return err("SELECT " + table);
    res_ = mysql_use_result(conn_);
    if (!res_) return err("use_result");
    fields_ = mysql_fetch_fields(res_);
    nfields_ = mysql_num_fields(res_);
    return Status::OK();
  }

  Status open_cursor_ordered(const std::string& table,
                             const std::string& column) override {
    close_cursor();
    const std::string q = std::string("SELECT * FROM ") + kBt + table + kBt +
                          " ORDER BY " + kBt + column + kBt;
    if (mysql_query(conn_, q.c_str())) return err("SELECT ordered " + table);
    res_ = mysql_use_result(conn_);
    if (!res_) return err("use_result ordered");
    fields_ = mysql_fetch_fields(res_);
    nfields_ = mysql_num_fields(res_);
    return Status::OK();
  }

  Status open_cursor_from(const std::string& table, const std::string& column,
                          const std::string& after_literal) override {
    close_cursor();
    const std::string q = std::string("SELECT * FROM ") + kBt + table + kBt +
                          " WHERE " + kBt + column + kBt + " > " +
                          after_literal + " ORDER BY " + kBt + column + kBt;
    if (mysql_query(conn_, q.c_str())) return err("SELECT resume " + table);
    res_ = mysql_use_result(conn_);
    if (!res_) return err("use_result resume");
    fields_ = mysql_fetch_fields(res_);
    nfields_ = mysql_num_fields(res_);
    return Status::OK();
  }

  Status begin_snapshot(std::string& file, uint64_t& pos) override {
    if (mysql_query(conn_, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ"))
      return err("set isolation");
    if (mysql_query(conn_, "START TRANSACTION WITH CONSISTENT SNAPSHOT"))
      return err("start snapshot");
    if (mysql_query(conn_, "SHOW BINARY LOG STATUS") &&
        mysql_query(conn_, "SHOW MASTER STATUS"))
      return err("show binlog status");
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("binlog status result");
    MYSQL_ROW row = mysql_fetch_row(r);
    if (!row || !row[0] || !row[1]) {
      mysql_free_result(r);
      return Status::Err(Code::kDb, "empty binlog status");
    }
    file = row[0];
    pos = (uint64_t)std::strtoull(row[1], nullptr, 10);
    mysql_free_result(r);
    return Status::OK();
  }

  void end_snapshot() override {
    if (conn_) mysql_query(conn_, "COMMIT");
  }

  Status pk_bounds(const std::string& table, const std::string& column,
                   int64_t& lo, int64_t& hi) override {
    const std::string q = std::string("SELECT MIN(") + kBt + column + kBt +
                          "), MAX(" + kBt + column + kBt + ") FROM " + kBt +
                          table + kBt;
    if (mysql_query(conn_, q.c_str())) return err("pk_bounds " + table);
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("pk_bounds result");
    MYSQL_ROW row = mysql_fetch_row(r);
    const bool empty = !row || !row[0] || !row[1];
    if (!empty) {
      lo = std::strtoll(row[0], nullptr, 10);
      hi = std::strtoll(row[1], nullptr, 10);
    }
    mysql_free_result(r);
    if (empty) return Status::Err(Code::kNotFound, "empty table: " + table);
    return Status::OK();
  }

  Status open_cursor_range(const std::string& table, const std::string& column,
                           int64_t lo, int64_t hi) override {
    close_cursor();
    const std::string q = std::string("SELECT * FROM ") + kBt + table + kBt +
                          " WHERE " + kBt + column + kBt + " >= " +
                          std::to_string(lo) + " AND " + kBt + column + kBt +
                          " <= " + std::to_string(hi) + " ORDER BY " + kBt +
                          column + kBt;
    if (mysql_query(conn_, q.c_str())) return err("SELECT range " + table);
    res_ = mysql_use_result(conn_);
    if (!res_) return err("use_result range");
    fields_ = mysql_fetch_fields(res_);
    nfields_ = mysql_num_fields(res_);
    return Status::OK();
  }

  Status fetch_batch(int n, std::vector<Row>& out) override {
    out.clear();
    if (!res_) return Status::OK();
    for (int k = 0; k < n; ++k) {
      MYSQL_ROW row = mysql_fetch_row(res_);
      if (!row) break;
      unsigned long* lens = mysql_fetch_lengths(res_);
      Row r;
      r.reserve(nfields_);
      for (unsigned int i = 0; i < nfields_; ++i) {
        if (row[i] == nullptr) { r.push_back(Value::Null()); continue; }
        const char* p = row[i];
        const size_t len = lens ? (size_t)lens[i] : std::strlen(row[i]);
        switch (fields_[i].type) {
          case MYSQL_TYPE_TINY:
          case MYSQL_TYPE_SHORT:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_INT24:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_YEAR:
            r.push_back(Value::Int(std::strtoll(p, nullptr, 10)));
            break;
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE:
            r.push_back(Value::Dbl(std::strtod(p, nullptr)));
            break;
          case MYSQL_TYPE_TINY_BLOB:
          case MYSQL_TYPE_MEDIUM_BLOB:
          case MYSQL_TYPE_LONG_BLOB:
          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_GEOMETRY:
            // TEXT 也是 MYSQL_TYPE_BLOB，只有字符集 63 才是真正的二进制
            if (fields_[i].charsetnr == 63)
              r.push_back(Value::Bytes(std::string(p, len)));
            else
              r.push_back(Value::Str(std::string(p, len)));
            break;
          case MYSQL_TYPE_DATETIME:
          case MYSQL_TYPE_TIMESTAMP:
            r.push_back(Value::Str(trim_fraction(std::string(p, len))));
            break;
          case MYSQL_TYPE_TIME:
            r.push_back(Value::Str(trim_fraction(std::string(p, len))));
            break;
          default:
            r.push_back(Value::Str(std::string(p, len)));
            break;
        }
      }
      out.push_back(std::move(r));
    }
    return Status::OK();
  }

  void close_cursor() override {
    if (res_) { mysql_free_result(res_); res_ = nullptr; }
  }

  int64_t total_rows(const std::string& table) override {
    const std::string q = std::string("SELECT COUNT(*) FROM ") + kBt + table + kBt;
    if (mysql_query(conn_, q.c_str())) return -1;
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return -1;
    MYSQL_ROW row = mysql_fetch_row(r);
    int64_t n = (row && row[0]) ? std::strtoll(row[0], nullptr, 10) : -1;
    mysql_free_result(r);
    return n;
  }

  Status list_views(std::vector<std::string>& out) override {
    out.clear();
    const std::string q =
        "SELECT TABLE_NAME FROM information_schema.VIEWS "
        "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_NAME";
    if (mysql_query(conn_, q.c_str())) return err("list_views");
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("list_views result");
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r)) != nullptr) {
      if (row[0]) out.emplace_back(row[0]);
    }
    mysql_free_result(r);
    return Status::OK();
  }

  Status read_view_sql(const std::string& name, std::string& sql) override {
    const std::string q = std::string("SHOW CREATE VIEW ") + kBt + name + kBt;
    if (mysql_query(conn_, q.c_str())) return err("SHOW CREATE VIEW " + name);
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return err("SHOW CREATE VIEW result");
    MYSQL_ROW row = mysql_fetch_row(r);
    if (!row || !row[1]) {
      mysql_free_result(r);
      return Status::Err(Code::kNotFound, "no definition for view " + name);
    }
    sql = row[1];
    mysql_free_result(r);
    return Status::OK();
  }

  std::string describe() override {
    return "MySQL source (" + dsn_get(opts_.source, "host", "127.0.0.1") + ":" +
           dsn_get(opts_.source, "port", "3306") + "/" +
           dsn_get(opts_.source, "db", "?") + ")";
  }

 private:
  Status err(const std::string& what) {
    return Status::Err(Code::kDb, what + ": " + (conn_ ? mysql_error(conn_) : "no conn"));
  }

  std::string esc(const std::string& s) {
    std::string out(s.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(conn_, &out[0], s.c_str(), (unsigned long)s.size());
    out.resize(n);
    return out;
  }

  void disconnect() {
    close_cursor();
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
  }

  Options opts_;
  MYSQL* conn_ = nullptr;
  MYSQL_RES* res_ = nullptr;
  MYSQL_FIELD* fields_ = nullptr;
  unsigned int nfields_ = 0;
};

}  // namespace

std::shared_ptr<ISource> make_mysql_source(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<MySqlSource>(o);
}

}  // namespace mig
