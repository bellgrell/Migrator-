#include "backends.h"

#include <mysql/mysql.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace mig {
namespace {

std::string dsn_get(const std::string& dsn, const std::string& key,
                    const std::string& def = "") {
  const std::string token = key + "=";
  size_t pos = 0;
  while ((pos = dsn.find(token, pos)) != std::string::npos) {
    if (pos == 0 || dsn[pos - 1] == ';' || dsn[pos - 1] == ' ') {
      const size_t v = pos + token.size();
      const size_t e = dsn.find(';', v);
      return dsn.substr(v, e == std::string::npos ? std::string::npos : e - v);
    }
    pos += token.size();
  }
  return def;
}

std::string safe_name(const std::string& s) {
  std::string out;
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')
      out.push_back(c);
    else
      out.push_back('_');
  }
  return out;
}

// LOAD DATA 文本格式转义：\ \t \n \r \0
std::string load_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\t': o += "\\t"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case 0: o += "\\0"; break;
      default: o.push_back((char)c);
    }
  }
  return o;
}

class MySqlTarget : public ITarget {
 public:
  explicit MySqlTarget(const Options& o) : opts_(o) {}
  ~MySqlTarget() override { close(); }

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
    unsigned int li = 1;
    mysql_options(conn_, MYSQL_OPT_LOCAL_INFILE, &li);

    const std::string host = dsn_get(opts_.target, "host", "127.0.0.1");
    const std::string port = dsn_get(opts_.target, "port", "3306");
    const std::string user = dsn_get(opts_.target, "user", "root");
    const std::string pass = dsn_get(opts_.target, "password", "");
    const std::string db = dsn_get(opts_.target, "db", "");

    if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), pass.c_str(),
                            db.empty() ? nullptr : db.c_str(),
                            (unsigned)std::atoi(port.c_str()), nullptr, 0)) {
      const std::string e = mysql_error(conn_);
      mysql_close(conn_);
      conn_ = nullptr;
      return Status::Err(Code::kDb, "mysql target connect: " + e);
    }
    mysql_set_character_set(conn_, "utf8mb4");
    mysql_query(conn_, "SET NAMES utf8mb4");
    mysql_autocommit(conn_, 0);
    load_enabled_ = server_local_infile();
    return Status::OK();
  }

  Status drop_table(const std::string& table) override {
    return exec_sql("DROP TABLE IF EXISTS `" + table + "`;");
  }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    if (mysql_query(conn_, ddl.c_str())) return err("create_table " + s.name);
    auto& st = state(s.name);
    st.cols.clear();
    st.binary = false;
    for (const auto& c : s.columns) {
      st.cols.push_back(c.name);
      if (c.binary) st.binary = true;
    }
    st.ensure_done = true;
    reset_file(st);
    return Status::OK();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
    auto& st = state(table);
    ensure_columns(table, st);
    if (load_enabled_ && st.load_ok() && !contains_bytes(rows)) {
      if (!st.out.is_open()) open_file(st);
      for (const auto& r : rows) {
        for (size_t j = 0; j < r.size(); ++j) {
          if (j) st.out.put('\t');
          st.out << load_field(r[j]);
        }
        st.out.put('\n');
      }
      st.pending += (int64_t)rows.size();
    } else {
      Status s = insert_batch(table, rows);
      if (!s.ok()) return s;
    }
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  Status exec_sql(const std::string& sql) override {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    if (mysql_query(conn_, sql.c_str())) return err("mysql target");
    consume_results();
    return Status::OK();
  }

  Status commit() override {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    for (auto& kv : st_) {
      Status s = flush_load(kv.first, kv.second);
      if (!s.ok()) return s;
    }
    if (mysql_commit(conn_)) return err("commit");
    return Status::OK();
  }

  Status rollback() override {
    for (auto& kv : st_) {
      if (kv.second.out.is_open()) kv.second.out.close();
      std::error_code ec;
      std::filesystem::remove(kv.second.path, ec);
      kv.second.pending = 0;
    }
    if (!conn_) return Status::OK();
    if (mysql_rollback(conn_)) return err("rollback");
    return Status::OK();
  }

  void close() override {
    for (auto& kv : st_) {
      if (kv.second.out.is_open()) kv.second.out.close();
      std::error_code ec;
      std::filesystem::remove(kv.second.path, ec);
    }
    st_.clear();
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
  }

  int64_t written(const std::string& table) override {
    if (!conn_) return 0;
    const std::string q = "SELECT COUNT(*) FROM `" + table + "`";
    if (mysql_query(conn_, q.c_str())) return 0;
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return 0;
    MYSQL_ROW row = mysql_fetch_row(r);
    const int64_t n = (row && row[0]) ? std::strtoll(row[0], nullptr, 10) : 0;
    mysql_free_result(r);
    return n;
  }

  std::string index_ddl(const std::string& table, const IndexDef& idx) override {
    std::string cols;
    for (size_t i = 0; i < idx.columns.size(); ++i) {
      if (i) cols += ", ";
      cols += "`" + idx.columns[i] + "`";
    }
    return std::string("CREATE ") + (idx.unique ? "UNIQUE " : "") +
           "INDEX `" + idx.name + "` ON `" + table + "` (" + cols + ");";
  }

  std::string analyze_sql(const std::string& table) override {
    return "ANALYZE TABLE `" + table + "`;";
  }

  std::string describe() override {
    return "MySQL target (" + dsn_get(opts_.target, "host", "127.0.0.1") + ":" +
           dsn_get(opts_.target, "port", "3306") + "/" +
           dsn_get(opts_.target, "db", "?") + ", " +
           (load_enabled_ ? "LOAD DATA" : "INSERT") + ")";
  }

 private:
  struct TableState {
    std::vector<std::string> cols;
    std::string path;
    std::ofstream out;
    int64_t pending = 0;
    bool binary = false;
    bool ensure_done = false;
    bool load_ok() const { return ensure_done && !binary; }
  };

  TableState& state(const std::string& table) {
    auto it = st_.find(table);
    if (it != st_.end()) return it->second;
    TableState st;
    st.path = "/tmp/mig-load-" + std::to_string((long)getpid()) + "-" +
              std::to_string((unsigned long)(uintptr_t)this) + "-" +
              safe_name(table) + ".tsv";
    auto res = st_.emplace(table, std::move(st));
    return res.first->second;
  }

  void ensure_columns(const std::string& table, TableState& st) {
    if (st.ensure_done) return;
    if (!st.cols.empty()) { st.ensure_done = true; return; }
    const std::string q =
        "SELECT column_name, data_type FROM information_schema.columns "
        "WHERE table_schema=DATABASE() AND table_name='" + table +
        "' ORDER BY ordinal_position";
    if (mysql_query(conn_, q.c_str()) == 0) {
      if (MYSQL_RES* r = mysql_store_result(conn_)) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(r)) != nullptr) {
          if (row[0]) st.cols.emplace_back(row[0]);
          if (row[1]) {
            const std::string t = row[1];
            if (t.find("blob") != std::string::npos ||
                t.find("binary") != std::string::npos)
              st.binary = true;
          }
        }
        mysql_free_result(r);
      }
    }
    st.ensure_done = true;
  }

  bool server_local_infile() {
    if (!load_enabled_) return false;
    if (mysql_query(conn_, "SHOW VARIABLES LIKE 'local_infile'")) return false;
    MYSQL_RES* r = mysql_store_result(conn_);
    if (!r) return false;
    MYSQL_ROW row = mysql_fetch_row(r);
    const bool on = row && row[1] && std::strcmp(row[1], "ON") == 0;
    mysql_free_result(r);
    return on;
  }

  void reset_file(TableState& st) {
    if (st.out.is_open()) st.out.close();
    std::error_code ec;
    std::filesystem::remove(st.path, ec);
    st.pending = 0;
  }

  void open_file(TableState& st) {
    std::error_code ec;
    std::filesystem::remove(st.path, ec);
    st.out.open(st.path, std::ios::binary | std::ios::trunc);
  }

  Status flush_load(const std::string& table, TableState& st) {
    if (!st.out.is_open() || st.pending == 0) return Status::OK();
    st.out.close();
    if (!load_enabled_) {
      return Status::Err(Code::kDb,
                         "LOAD DATA buffer without local_infile for " + table);
    }
    std::string cols;
    for (size_t i = 0; i < st.cols.size(); ++i) {
      if (i) cols += ",";
      cols += "`" + st.cols[i] + "`";
    }
    std::string sql = "LOAD DATA LOCAL INFILE '" + st.path + "' INTO TABLE `" +
                      table + "` CHARACTER SET utf8mb4 "
                      "FIELDS TERMINATED BY '\\t' ESCAPED BY '\\\\' "
                      "LINES TERMINATED BY '\\n' (" + cols + ")";
    if (mysql_query(conn_, sql.c_str())) return err("load data " + table);
    consume_results();
    std::error_code ec;
    std::filesystem::remove(st.path, ec);
    st.pending = 0;
    return Status::OK();
  }

  std::string load_field(const Value& v) {
    switch (v.type) {
      case ValType::kNull: return "\\N";
      case ValType::kInt: return std::to_string(v.i);
      case ValType::kDouble: {
        std::ostringstream o;
        o << std::setprecision(17) << v.d;
        return o.str();
      }
      case ValType::kBool: return v.b ? "1" : "0";
      case ValType::kBytes: return load_escape(v.s);
      case ValType::kString:
      default: return load_escape(v.s);
    }
  }

  bool contains_bytes(const std::vector<Row>& rows) {
    for (const auto& r : rows)
      for (const auto& v : r)
        if (v.type == ValType::kBytes) return true;
    return false;
  }

  Status insert_batch(const std::string& table, const std::vector<Row>& rows) {
    std::string sql = "INSERT INTO `" + table + "` VALUES ";
    sql.reserve(rows.size() * 128);
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) sql += ",";
      sql += "(";
      const Row& r = rows[i];
      for (size_t j = 0; j < r.size(); ++j) {
        if (j) sql += ",";
        sql += literal(r[j]);
      }
      sql += ")";
    }
    if (mysql_query(conn_, sql.c_str())) return err("insert " + table);
    return Status::OK();
  }

  Status err(const std::string& what) {
    return Status::Err(Code::kDb, what + ": " + (conn_ ? mysql_error(conn_) : "no conn"));
  }

  void consume_results() {
    if (MYSQL_RES* r = mysql_store_result(conn_)) mysql_free_result(r);
    while (mysql_next_result(conn_) == 0) {
      if (MYSQL_RES* r2 = mysql_store_result(conn_)) mysql_free_result(r2);
    }
  }

  std::string literal(const Value& v) {
    switch (v.type) {
      case ValType::kNull: return "NULL";
      case ValType::kInt: return std::to_string(v.i);
      case ValType::kDouble: {
        std::ostringstream o;
        o << std::setprecision(17) << v.d;
        return o.str();
      }
      case ValType::kBool: return v.b ? "1" : "0";
      case ValType::kBytes: {
        static const char* hex = "0123456789abcdef";
        std::string out = "X'";
        for (unsigned char c : v.s) { out += hex[c >> 4]; out += hex[c & 0xF]; }
        out += "'";
        return out;
      }
      case ValType::kString:
      default: {
        std::string buf(v.s.size() * 2 + 1, '\0');
        const unsigned long n = mysql_real_escape_string(
            conn_, &buf[0], v.s.c_str(), (unsigned long)v.s.size());
        buf.resize(n);
        return "'" + buf + "'";
      }
    }
  }

  Options opts_;
  MYSQL* conn_ = nullptr;
  bool load_enabled_ = true;
  std::mutex mu_;
  std::map<std::string, int64_t> written_;
  std::map<std::string, TableState> st_;
};

}  // namespace

std::shared_ptr<ITarget> make_mysql_target(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<MySqlTarget>(o);
}

}  // namespace mig
