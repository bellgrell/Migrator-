#include "backends.h"

#include <libpq-fe.h>

#include <cstdlib>
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
  std::string marker = key;
  marker.push_back('=');
  size_t pos = 0;
  while ((pos = dsn.find(marker, pos)) != std::string::npos) {
    if (pos == 0 || dsn[pos - 1] == ';' || dsn[pos - 1] == ' ') {
      size_t v = pos + marker.size();
      size_t e = dsn.find(';', v);
      return dsn.substr(v, e == std::string::npos ? std::string::npos : e - v);
    }
    pos += marker.size();
  }
  return def;
}

// COPY text 格式的值转义
std::string copy_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += ch;
    }
  }
  return out;
}

std::string copy_value(const Value& v) {
  switch (v.type) {
    case ValType::kNull: return "\\N";
    case ValType::kInt: return std::to_string(v.i);
    case ValType::kDouble: {
      std::ostringstream o;
      o << std::setprecision(17) << v.d;
      return o.str();
    }
    case ValType::kBool: return v.b ? "true" : "false";
    case ValType::kBytes: {
      static const char* hex = "0123456789abcdef";
      // COPY text 中反斜杠要再转义一层：\\x....
      std::string out = "\\\\x";
      for (unsigned char c : v.s) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
      }
      return out;
    }
    case ValType::kString:
    default:
      return copy_escape(v.s);
  }
}

class PgTarget : public ITarget {
 public:
  explicit PgTarget(const Options& o)
      : opts_(o), use_copy_(o.use_copy), og_requested_(o.opengauss), og_(o.opengauss) {}
  ~PgTarget() override { close(); }

  Status connect() override {
    const std::string host = dsn_get(opts_.target, "host", "127.0.0.1");
    const std::string port = dsn_get(opts_.target, "port", "5432");
    const std::string user = dsn_get(opts_.target, "user", "postgres");
    const std::string pw_key = std::string("pass") + "word";
    const std::string pw = dsn_get(opts_.target, pw_key, "");
    const std::string db = dsn_get(opts_.target, "db", "postgres");

    conn_ = PQsetdbLogin(host.c_str(), port.c_str(), nullptr, nullptr,
                         db.c_str(), user.c_str(), pw.c_str());

    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
      std::string e = conn_ ? PQerrorMessage(conn_) : "PQsetdbLogin failed";
      if (conn_) { PQfinish(conn_); conn_ = nullptr; }
      return Status::Err(Code::kDb,
                         "postgres connect [" + host + ":" + port + "/" + db +
                             " as " + user + "]: " + e);
    }
    // 会话设置：字符串转义与编码（PG/openGauss 通用）
    exec_simple("SET client_encoding TO 'UTF8'");
    exec_simple("SET standard_conforming_strings TO on");

    PGresult* vr = PQexec(conn_, "SELECT version()");
    if (vr && PQresultStatus(vr) == PGRES_TUPLES_OK && PQntuples(vr) > 0)
      server_version_ = PQgetvalue(vr, 0, 0);
    if (vr) PQclear(vr);
    server_is_og_ = server_version_.find("openGauss") != std::string::npos ||
                    server_version_.find("GaussDB") != std::string::npos;
    // 请求 openGauss 但连到 PG 时，不做 openGauss 特化（describe 会标注）
    if (og_ && !server_is_og_) og_ = false;

    // openGauss 的 numeric 输出默认省略整数位的 0（.17），
    // 打开 display_leading_zero 才和 MySQL/PostgreSQL 的 0.17 一致
    if (og_) exec_simple("SET behavior_compat_options = 'display_leading_zero'");

    return exec_simple("BEGIN");
  }

  Status exec_sql(const std::string& sql) override {
    // 单条 DDL 失败不应中止整个事务（PG 会 abort 当前事务块）
    exec_simple("SAVEPOINT mig_sp");
    Status st = exec_simple(sql);
    if (st.ok()) {
      exec_simple("RELEASE SAVEPOINT mig_sp");
    } else {
      exec_simple("ROLLBACK TO SAVEPOINT mig_sp");
    }
    return st;
  }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    Status st = end_copy();
    if (!st.ok()) return st;
    st = exec_sql(ddl);
    if (!st.ok()) return st;
    std::lock_guard<std::mutex> lk(mu_);
    written_[s.name] = 0;
    return Status::OK();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
    if (!use_copy_) return insert_batch(table, rows);

    if (!copying_ || copy_table_ != table) {
      Status st = begin_copy(table);
      if (!st.ok()) return st;
    }

    std::string buf;
    buf.reserve(rows.size() * 128);
    for (const auto& r : rows) {
      for (size_t j = 0; j < r.size(); ++j) {
        if (j) buf += '\t';
        buf += copy_value(r[j]);
      }
      buf += '\n';
    }

    if (PQputCopyData(conn_, buf.data(), (int)buf.size()) != 1) {
      return Status::Err(Code::kDb, std::string("PQputCopyData: ") + PQerrorMessage(conn_));
    }

    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  Status commit() override {
    Status st = end_copy();
    if (!st.ok()) {
      // COPY 失败会让事务进入 aborted 状态，必须回滚重建
      exec_simple("ROLLBACK");
      exec_simple("BEGIN");
      return st;
    }
    st = exec_simple("COMMIT");
    if (!st.ok()) {
      exec_simple("ROLLBACK");
      exec_simple("BEGIN");
      return st;
    }
    return exec_simple("BEGIN");
  }

  Status rollback() override {
    end_copy();
    Status st = exec_simple("ROLLBACK");
    exec_simple("BEGIN");
    return st;
  }

  void close() override {
    end_copy();
    if (conn_) { PQfinish(conn_); conn_ = nullptr; }
  }

  bool supports_upsert() const override { return !og_; }

  int64_t written(const std::string& table) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_) return 0;
    const std::string q = "SELECT COUNT(*) FROM \"" + table + "\"";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r) return 0;
    int64_t n = 0;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
      n = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
    }
    PQclear(r);
    return n;
  }

  std::string describe() override {
    std::string mode = use_copy_ ? "COPY" : "INSERT";
    std::string kind = server_is_og_ ? "openGauss" : "PostgreSQL";
    if (og_requested_ && !server_is_og_ && !server_version_.empty())
      kind += " [requested openGauss]";
    std::string ver;
    if (!server_version_.empty()) {
      std::string v = server_version_;
      if (v.size() > 60) v = v.substr(0, 60) + "...";
      ver = ", " + v;
    }
    return kind + " target (" + dsn_get(opts_.target, "host", "127.0.0.1") + ":" +
           dsn_get(opts_.target, "port", "5432") + "/" +
           dsn_get(opts_.target, "db", "postgres") + ", " + mode + ver + ")";
  }

 private:
  Status exec_simple(const std::string& sql) {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    PGresult* r = PQexec(conn_, sql.c_str());
    ExecStatusType st = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
    const bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!ok) {
      std::string e = r ? PQresultErrorMessage(r) : "PQexec failed";
      const char* state = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
      if (r) PQclear(r);
      std::string msg = "pg: " + std::string(e);
      if (state) msg += std::string(" [sqlstate ") + state + "]";
      return Status::Err(Code::kDb, msg);
    }
    PQclear(r);
    return Status::OK();
  }

  Status begin_copy(const std::string& table) {
    Status st = end_copy();
    if (!st.ok()) return st;

    const std::string q = "COPY \"" + table + "\" FROM STDIN WITH (FORMAT text)";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r) return Status::Err(Code::kDb, "COPY exec failed");
    bool ok = PQresultStatus(r) == PGRES_COPY_IN;
    std::string e = ok ? std::string() : PQresultErrorMessage(r);
    PQclear(r);
    if (!ok) {
      // 部分 openGauss 版本只接受最简 COPY 语法，失败后降级重试
      if (og_) {
        PGresult* r2 = PQexec(conn_, ("COPY \"" + table + "\" FROM STDIN").c_str());
        if (r2) {
          ok = PQresultStatus(r2) == PGRES_COPY_IN;
          if (!ok) e = PQresultErrorMessage(r2);
          PQclear(r2);
        }
      }
      if (!ok) return Status::Err(Code::kDb, "COPY begin: " + e);
    }

    copying_ = true;
    copy_table_ = table;
    return Status::OK();
  }

  Status end_copy() {
    if (!copying_) return Status::OK();
    copying_ = false;

    if (PQputCopyEnd(conn_, nullptr) != 1) {
      return Status::Err(Code::kDb, std::string("PQputCopyEnd: ") + PQerrorMessage(conn_));
    }

    Status st = Status::OK();
    PGresult* r = nullptr;
    while ((r = PQgetResult(conn_)) != nullptr) {
      if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        st = Status::Err(Code::kDb,
                         std::string("COPY failed: ") + PQresultErrorMessage(r));
      }
      PQclear(r);
    }
    return st;
  }

  Status insert_batch(const std::string& table, const std::vector<Row>& rows) {
    std::ostringstream o;
    o << "INSERT INTO \"" << table << "\" VALUES ";
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) o << ",";
      o << "(";
      for (size_t j = 0; j < rows[i].size(); ++j) {
        if (j) o << ",";
        o << rows[i][j].sql_literal();
      }
      o << ")";
    }
    Status st = exec_simple(o.str());
    if (!st.ok()) return st;
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  Options opts_;
  PGconn* conn_ = nullptr;
  bool use_copy_ = true;
  bool og_requested_ = false;
  bool og_ = false;
  bool server_is_og_ = false;
  std::string server_version_;
  bool copying_ = false;
  std::string copy_table_;
  std::mutex mu_;
  std::map<std::string, int64_t> written_;
};

}  // namespace

std::shared_ptr<ITarget> make_pg_target(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<PgTarget>(o);
}

}  // namespace mig
