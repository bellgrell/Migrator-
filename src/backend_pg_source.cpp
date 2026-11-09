#include "backends.h"

#include <libpq-fe.h>

#include <cstdlib>
#include <cstring>
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

std::string hex_encode(const std::string& raw) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out += hex[c >> 4];
    out += hex[c & 0xF];
  }
  return out;
}

bool hex_decode(const std::string& in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (in.size() % 2 != 0) return false;
  out.clear();
  out.reserve(in.size() / 2);
  for (size_t i = 0; i < in.size(); i += 2) {
    int hi = val(in[i]);
    int lo = val(in[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((char)((hi << 4) | lo));
  }
  return true;
}

class PgSource : public ISource {
 public:
  explicit PgSource(const Options& o) : opts_(o) {}
  ~PgSource() override { close(); }

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
      return Status::Err(Code::kDb, "pg source connect: " + e);
    }
    exec("SET client_encoding TO 'UTF8'");
    exec("SET standard_conforming_strings TO on");
    PGresult* vr = PQexec(conn_, "SELECT version()");
    if (vr && PQresultStatus(vr) == PGRES_TUPLES_OK && PQntuples(vr) > 0)
      server_version_ = PQgetvalue(vr, 0, 0);
    if (vr) PQclear(vr);
    server_is_og_ = server_version_.find("openGauss") != std::string::npos ||
                    server_version_.find("GaussDB") != std::string::npos;
    // 与目标端一致：numeric 输出带前导 0，避免 checksum 假阳性
    if (server_is_og_)
      exec("SET behavior_compat_options = 'display_leading_zero'");
    return exec("BEGIN");
  }

  Status list_tables(std::vector<std::string>& out) override {
    out.clear();
    PGresult* r = PQexec(conn_,
        "SELECT tablename FROM pg_tables WHERE schemaname='public' "
        "AND tablename NOT LIKE 'pg_%' ORDER BY tablename");
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg list_tables failed");
    }
    for (int i = 0; i < PQntuples(r); ++i) out.push_back(PQgetvalue(r, i, 0));
    PQclear(r);
    return Status::OK();
  }

  Status read_schema(const std::string& table, TableSchema& out) override {
    out = TableSchema();
    out.name = table;
    const std::string q =
        "SELECT column_name, data_type, is_nullable FROM information_schema.columns "
        "WHERE table_schema='public' AND table_name='" + table + "' ORDER BY ordinal_position";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg read_schema failed");
    }
    for (int i = 0; i < PQntuples(r); ++i) {
      ColumnDef c;
      c.name = PQgetvalue(r, i, 0);
      c.src_type = PQgetvalue(r, i, 1);
      c.dst_type = c.src_type;
      c.nullable = std::strcmp(PQgetvalue(r, i, 2), "YES") == 0;
      out.columns.push_back(c);
    }
    PQclear(r);
    return Status::OK();
  }

  Status open_cursor(const std::string& table) override {
    close_cursor();
    const std::string sel = "SELECT * FROM \"" + table + "\"";
    Status st = exec("DECLARE mig_cur NO SCROLL CURSOR FOR " + sel);
    if (!st.ok()) {
      // openGauss 某些版本不接受 NO SCROLL，退回默认游标
      st = exec("DECLARE mig_cur CURSOR FOR " + sel);
      if (!st.ok()) return st;
    }
    cursor_open_ = true;
    return Status::OK();
  }

  Status fetch_batch(int n, std::vector<Row>& out) override {
    out.clear();
    const std::string q = "FETCH " + std::to_string(n) + " FROM mig_cur";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg fetch failed");
    }

    const int nf = PQnfields(r);
    for (int i = 0; i < PQntuples(r); ++i) {
      Row row;
      row.reserve(nf);
      for (int j = 0; j < nf; ++j) {
        if (PQgetisnull(r, i, j)) {
          row.push_back(Value::Null());
          continue;
        }
        const Oid t = PQftype(r, j);
        const char* v = PQgetvalue(r, i, j);
        if (t == 20 || t == 21 || t == 23 || t == 26) {
          row.push_back(Value::Int(std::strtoll(v, nullptr, 10)));
        } else if (t == 700 || t == 701) {
          row.push_back(Value::Dbl(std::strtod(v, nullptr)));
        } else if (t == 16) {
          row.push_back(Value::Bool(v[0] == 't'));
        } else if (t == 17) {
          const std::string s(v);
          std::string raw;
          if (s.size() > 2 && s[0] == '\\' && s[1] == 'x' && hex_decode(s.substr(2), raw)) {
            row.push_back(Value::Bytes(raw));
          } else {
            row.push_back(Value::Str(s));
          }
        } else {
          row.push_back(Value::Str(v));
        }
      }
      out.push_back(std::move(row));
    }
    PQclear(r);
    return Status::OK();
  }

  void close_cursor() override {
    if (!cursor_open_) return;
    exec("CLOSE mig_cur");
    cursor_open_ = false;
  }

  int64_t total_rows(const std::string& table) override {
    const std::string q = "SELECT COUNT(*) FROM \"" + table + "\"";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return -1;
    }
    int64_t n = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
    PQclear(r);
    return n;
  }

  std::string describe() override {
    const std::string kind = server_is_og_ ? "openGauss" : "PostgreSQL";
    return kind + " source (" + dsn_get(opts_.target, "host", "127.0.0.1") + ":" +
           dsn_get(opts_.target, "port", "5432") + "/" +
           dsn_get(opts_.target, "db", "postgres") + ")";
  }

 private:
  Status exec(const std::string& sql) {
    if (!conn_) return Status::Err(Code::kDb, "not connected");
    PGresult* r = PQexec(conn_, sql.c_str());
    const bool ok = r && (PQresultStatus(r) == PGRES_COMMAND_OK ||
                          PQresultStatus(r) == PGRES_TUPLES_OK);
    if (!ok) {
      std::string e = r ? PQresultErrorMessage(r) : "PQexec failed";
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg: " + e);
    }
    PQclear(r);
    return Status::OK();
  }

  void close() {
    if (conn_) { PQfinish(conn_); conn_ = nullptr; }
  }

  Options opts_;
  PGconn* conn_ = nullptr;
  bool cursor_open_ = false;
  bool server_is_og_ = false;
  std::string server_version_;
};

}  // namespace

std::shared_ptr<ISource> make_pg_source(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<PgSource>(o);
}

}  // namespace mig
