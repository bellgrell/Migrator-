#include "backends.h"

#include <mysql/mysql.h>

#include <cstdlib>
#include <cstring>
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

class MySqlTarget : public ITarget {
 public:
  explicit MySqlTarget(const Options& o) : opts_(o) {}
  ~MySqlTarget() override { close(); }

  Status connect() override {
    conn_ = mysql_init(nullptr);
    if (!conn_) return Status::Err(Code::kDb, "mysql_init failed");

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
    return Status::OK();
  }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    if (mysql_query(conn_, ddl.c_str())) return err("create_table " + s.name);
    std::lock_guard<std::mutex> lk(mu_);
    written_[s.name] = 0;
    return Status::OK();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
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
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  Status exec_sql(const std::string& sql) override {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    if (mysql_query(conn_, sql.c_str())) return err("mysql target");
    // ANALYZE TABLE / SHOW 等会返回结果集，必须消费掉，否则后续语句会 out of sync
    if (MYSQL_RES* r = mysql_store_result(conn_)) mysql_free_result(r);
    while (mysql_next_result(conn_) == 0) {
      if (MYSQL_RES* r2 = mysql_store_result(conn_)) mysql_free_result(r2);
    }
    return Status::OK();
  }

  Status commit() override {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    if (mysql_commit(conn_)) return err("commit");
    return Status::OK();
  }

  Status rollback() override {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    if (mysql_rollback(conn_)) return err("rollback");
    return Status::OK();
  }

  void close() override {
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
           dsn_get(opts_.target, "db", "?") + ")";
  }

 private:
  Status err(const std::string& what) {
    return Status::Err(Code::kDb, what + ": " + (conn_ ? mysql_error(conn_) : "no conn"));
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
  std::mutex mu_;
  std::map<std::string, int64_t> written_;
};

}  // namespace

std::shared_ptr<ITarget> make_mysql_target(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<MySqlTarget>(o);
}

}  // namespace mig
