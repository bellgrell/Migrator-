#include "sql_backend.h"

#include "dsn.h"

#ifdef HAVE_PG
#include <libpq-fe.h>

#include <cstring>
#include <sstream>
#include <vector>

namespace mmv {
namespace {

std::string conninfo_quote(const std::string& v) {
  std::string o = "'";
  for (char c : v) {
    if (c == '\\' || c == '\'') o += '\\';
    o += c;
  }
  return o + "'";
}

class PgBackend : public SqlBackend {
 public:
  ~PgBackend() override { if (conn_) PQfinish(conn_); }

  Status connect(const std::string& dsn) override {
    Dsn d{dsn};
    std::ostringstream ci;
    auto add = [&](const char* key, const std::string& v) {
      if (!v.empty()) ci << key << "=" << conninfo_quote(v) << " ";
    };
    add("host", d.host());
    add("port", d.port());
    add("user", d.user());
    add("password", d.password_resolved());
    add("dbname", d.dbname());
    add("sslmode", d.get("sslmode", "prefer"));
    add("connect_timeout", d.get("connect_timeout", "10"));
    conn_ = PQconnectdb(ci.str().c_str());
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK)
      return Status::Err(Code::kDb, std::string("pg connect: ") +
                                    (conn_ ? PQerrorMessage(conn_) : "PQconnectdb failed"));
    Status es = exec("SET client_encoding = 'UTF8'");
    if (!es.ok()) return es;
    es = exec("SET standard_conforming_strings = on");
    if (!es.ok()) return es;
    ResultSet rs;
    Status st = query("SELECT version()", rs);
    if (st.ok() && !rs.rows.empty()) version_ = rs.rows[0][0].text;
    if (version_.find("openGauss") != std::string::npos ||
        version_.find("opengauss") != std::string::npos) {
      exec("SET behavior_compat_options = 'display_leading_zero'");
      exec("SET extra_float_digits = 3");
    }
    desc_ = "pg " + d.host() + ":" + d.port() + "/" + d.dbname();
    return Status::OK();
  }

  Status query(const std::string& sql, ResultSet& out) override {
    out = ResultSet{};
    PGresult* r = PQexec(conn_, sql.c_str());
    if (!r) return Status::Err(Code::kDb, "PQexec failed");
    const ExecStatusType es = PQresultStatus(r);
    if (es != PGRES_TUPLES_OK) {
      std::string e = PQresultErrorMessage(r);
      PQclear(r);
      return Status::Err(Code::kDb, "pg query: " + e);
    }
    const int nf = PQnfields(r), nr = PQntuples(r);
    for (int i = 0; i < nf; ++i) out.columns.emplace_back(PQfname(r, i));
    out.rows.reserve(nr);
    for (int i = 0; i < nr; ++i) {
      std::vector<Cell> row;
      row.reserve(nf);
      for (int j = 0; j < nf; ++j) {
        Cell c;
        c.is_null = PQgetisnull(r, i, j) != 0;
        if (!c.is_null) c.text.assign(PQgetvalue(r, i, j), (size_t)PQgetlength(r, i, j));
        row.push_back(std::move(c));
      }
      out.rows.push_back(std::move(row));
    }
    PQclear(r);
    return Status::OK();
  }

  Status query_stream(const std::string& sql,
                      const std::function<bool(const std::vector<Cell>&)>& fn) override {
    if (!PQsendQuery(conn_, sql.c_str()))
      return Status::Err(Code::kDb, std::string("pg send: ") + PQerrorMessage(conn_));
    if (!PQsetSingleRowMode(conn_))
      return Status::Err(Code::kDb, "pg single-row mode failed");
    bool keep = true;
    Status out;
    PGresult* r = nullptr;
    while ((r = PQgetResult(conn_)) != nullptr) {
      const ExecStatusType es = PQresultStatus(r);
      if (es == PGRES_SINGLE_TUPLE) {
        if (keep) {
          const int nf = PQnfields(r);
          std::vector<Cell> row;
          row.reserve(nf);
          for (int j = 0; j < nf; ++j) {
            Cell c;
            c.is_null = PQgetisnull(r, 0, j) != 0;
            if (!c.is_null)
              c.text.assign(PQgetvalue(r, 0, j), (size_t)PQgetlength(r, 0, j));
            row.push_back(std::move(c));
          }
          keep = fn(row);
        }
      } else if (es != PGRES_TUPLES_OK && out.ok()) {
        out = Status::Err(Code::kDb,
                          "pg stream: " + std::string(PQresultErrorMessage(r)));
      }
      PQclear(r);
    }
    return out;
  }

  Status exec(const std::string& sql) override {
    PGresult* r = PQexec(conn_, sql.c_str());
    if (!r) return Status::Err(Code::kDb, "PQexec failed");
    const ExecStatusType es = PQresultStatus(r);
    if (es != PGRES_COMMAND_OK && es != PGRES_TUPLES_OK) {
      std::string e = PQresultErrorMessage(r);
      PQclear(r);
      return Status::Err(Code::kDb, "pg exec: " + e);
    }
    PQclear(r);
    return Status::OK();
  }

  Dialect dialect() const override { return Dialect::kPg; }
  std::string quote_ident(const std::string& n) const override { return mmv::quote_ident(n, Dialect::kPg); }
  std::string quote_lit(const std::string& s) const override { return mmv::quote_lit(s, Dialect::kPg); }
  std::string version() const override { return version_; }
  bool is_opengauss() const override {
    return version_.find("openGauss") != std::string::npos ||
           version_.find("opengauss") != std::string::npos;
  }
  std::string describe() const override { return desc_; }

  Status set_timeout(int seconds) override {
    if (seconds <= 0) return Status::OK();
    return exec("SET statement_timeout = " + std::to_string(seconds * 1000));
  }
  Status begin_readonly() override {
    return exec("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
  }
  Status end_readonly() override { return exec("COMMIT"); }

  Status begin_export_snapshot(std::string& id) override {
    id.clear();
    Status st = exec("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    if (!st.ok()) return st;
    ResultSet rs;
    st = query("SELECT pg_export_snapshot()", rs);
    if (!st.ok() || rs.rows.empty() || rs.rows[0][0].is_null) {
      exec("ROLLBACK");
      return Status::Err(Code::kDb, "pg_export_snapshot failed");
    }
    id = rs.rows[0][0].text;
    return Status::OK();
  }

  Status join_snapshot(const std::string& id) override {
    Status st = exec("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    if (!st.ok()) return st;
    st = exec("SET TRANSACTION SNAPSHOT " + quote_lit(id));
    if (!st.ok()) exec("ROLLBACK");
    return st;
  }

  Status table_change_token(const std::string& table, std::string& out) override {
    out.clear();
    ResultSet rs;
    Status st = query("SELECT COALESCE(n_tup_ins + n_tup_upd + n_tup_del, 0)::text "
                      "FROM pg_stat_user_tables WHERE schemaname = current_schema() "
                      "AND relname = " + quote_lit(table), rs);
    if (st.ok() && !rs.rows.empty() && !rs.rows[0][0].is_null) {
      out = "pg-tbl:" + rs.rows[0][0].text;
      return Status::OK();
    }
    return change_token(out);
  }

  Status change_token(std::string& out) override {
    out.clear();
    ResultSet rs;
    Status st = query("SELECT pg_current_wal_lsn()::text", rs);
    if (st.ok() && !rs.rows.empty() && !rs.rows[0][0].is_null) {
      out = "pg:" + rs.rows[0][0].text;
    }
    return Status::OK();
  }

 private:
  PGconn* conn_ = nullptr;
  std::string version_;
  std::string desc_;
};

}  // namespace

std::shared_ptr<SqlBackend> make_pg_backend(const std::string& dsn, Status& st) {
  auto b = std::make_shared<PgBackend>();
  st = b->connect(dsn);
  if (!st.ok()) return nullptr;
  return b;
}

}  // namespace mmv
#endif  // HAVE_PG
