#include "backends.h"

#include <libpq-fe.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mig {
namespace {

std::string cq(const std::string& v) {
  std::string o = "'";
  for (char c : v) { if (c == '\\' || c == '\'') o += '\\'; o += c; }
  o += "'";
  return o;
}

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
    const std::string& dsn =
        opts_.pg_source_uses_source ? opts_.source : opts_.target;
    const std::string host = dsn_get(dsn, "host", "127.0.0.1");
    const std::string port = dsn_get(dsn, "port", "5432");
    const std::string user = dsn_get(dsn, "user", "postgres");
    const std::string pw_key = std::string("pass") + "word";
    const std::string pw = dsn_get(dsn, pw_key, "");
    const std::string db = dsn_get(dsn, "db", "postgres");

    const std::string ci =
        "host=" + cq(host) + " port=" + cq(port) + " user=" + cq(user) +
        " password=" + cq(pw) + " dbname=" + cq(db) +
        " connect_timeout=" + std::to_string(opts_.connect_timeout > 0 ? opts_.connect_timeout : 10) +
        " keepalives=1 keepalives_idle=30 sslmode=" + cq(opts_.sslmode);
    conn_ = PQconnectdb(ci.c_str());
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

  Status list_views(std::vector<std::string>& out) override {
    out.clear();
    PGresult* r = PQexec(conn_,
        "SELECT viewname FROM pg_views WHERE schemaname='public' "
        "AND viewname NOT LIKE 'pg_%' ORDER BY viewname");
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg list_views failed");
    }
    for (int i = 0; i < PQntuples(r); ++i)
      out.push_back(PQgetvalue(r, i, 0));
    PQclear(r);
    return Status::OK();
  }

  Status read_view_sql(const std::string& name, std::string& sql) override {
    std::string quoted;
    for (char c : name) {
      if (c == '"') quoted += "\"\"";
      else quoted += c;
    }
    const std::string q =
        "SELECT pg_get_viewdef('\"" + quoted + "\"'::regclass, true)";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg read_view_sql failed: " + name);
    }
    std::string def = PQgetvalue(r, 0, 0);
    PQclear(r);
    sql = "CREATE OR REPLACE VIEW \"" + quoted + "\" AS " + def;
    return Status::OK();
  }

  Status read_schema(const std::string& table, TableSchema& out) override {
    out = TableSchema();
    out.name = table;
    std::string et;
    for (char ch : table) { if (ch == '\'') et += "''"; else et += ch; }
    const std::string q =
        "SELECT column_name, data_type, is_nullable, character_maximum_length, "
        "numeric_precision, numeric_scale, udt_name "
        "FROM information_schema.columns WHERE table_schema='public' AND table_name='" +
        et + "' ORDER BY ordinal_position";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg read_schema failed");
    }
    for (int i = 0; i < PQntuples(r); ++i) {
      ColumnDef c;
      c.name = PQgetvalue(r, i, 0);
      std::string type = PQgetvalue(r, i, 1);
      std::string low = type;
      for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
      if (!PQgetisnull(r, i, 3)) {
        type += "(" + std::string(PQgetvalue(r, i, 3)) + ")";
      } else if ((low == "numeric" || low == "decimal") && !PQgetisnull(r, i, 4)) {
        type += "(" + std::string(PQgetvalue(r, i, 4));
        if (!PQgetisnull(r, i, 5)) type += "," + std::string(PQgetvalue(r, i, 5));
        type += ")";
      }
      c.src_type = type;
      c.dst_type = type;
      c.nullable = std::strcmp(PQgetvalue(r, i, 2), "YES") == 0;
      const std::string udt = PQgetvalue(r, i, 6);
      c.binary = (udt == "bytea");
      out.columns.push_back(c);
    }
    PQclear(r);

    // 主键
    const std::string pkq =
        "SELECT a.attname FROM pg_index i JOIN pg_attribute a ON a.attrelid=i.indrelid "
        "AND a.attnum=ANY(i.indkey) WHERE i.indrelid='\"" + et + "\"'::regclass "
        "AND i.indisprimary ORDER BY a.attnum";
    PGresult* pr = PQexec(conn_, pkq.c_str());
    if (pr && PQresultStatus(pr) == PGRES_TUPLES_OK) {
      for (int i = 0; i < PQntuples(pr); ++i) {
        const std::string col = PQgetvalue(pr, i, 0);
        out.primary_key.push_back(col);
        for (auto& c : out.columns) if (c.name == col) c.primary = true;
      }
    }
    if (pr) PQclear(pr);

    // 二级索引（简单列索引，跳过 pkey 与表达式索引）
    const std::string iq =
        "SELECT indexname, indexdef FROM pg_indexes WHERE schemaname='public' "
        "AND tablename='" + et + "' ORDER BY indexname";
    PGresult* ir = PQexec(conn_, iq.c_str());
    if (ir && PQresultStatus(ir) == PGRES_TUPLES_OK) {
      for (int i = 0; i < PQntuples(ir); ++i) {
        const std::string name = PQgetvalue(ir, i, 0);
        const std::string def = PQgetvalue(ir, i, 1);
        if (name.size() >= 5 && name.substr(name.size() - 5) == "_pkey") continue;
        const size_t lp = def.rfind('(');
        const size_t rp = def.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp <= lp) continue;
        IndexDef idx;
        idx.name = name;
        idx.unique = def.find("CREATE UNIQUE INDEX") != std::string::npos;
        std::string list = def.substr(lp + 1, rp - lp - 1);
        std::string cur;
        for (size_t k = 0; k <= list.size(); ++k) {
          if (k == list.size() || list[k] == ',') {
            size_t a = cur.find_first_not_of(" ");
            size_t b = cur.find_last_not_of(" ");
            if (a != std::string::npos) {
              std::string col = cur.substr(a, b - a + 1);
              const bool simple = col.find_first_of(" ()") == std::string::npos;
              if (simple) idx.columns.push_back(col);
            }
            cur.clear();
          } else {
            cur.push_back(list[k]);
          }
        }
        if (!idx.columns.empty()) out.indexes.push_back(idx);
      }
    }
    if (ir) PQclear(ir);
    return Status::OK();
  }

  Status begin_snapshot(std::string& file, uint64_t& pos) override {
    exec("COMMIT");
    Status st = exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
    if (!st.ok()) return st;
    file = server_is_og_ ? "opengauss-snapshot" : "postgres-snapshot";
    pos = 0;
    return Status::OK();
  }

  void end_snapshot() override { exec("COMMIT"); }

  Status open_cursor_ordered(const std::string& table,
                             const std::string& column) override {
    close_cursor();
    const std::string sel = "SELECT * FROM \"" + table + "\" ORDER BY \"" +
                            column + "\"";
    Status st = exec("DECLARE mig_cur NO SCROLL CURSOR FOR " + sel);
    if (!st.ok()) st = exec("DECLARE mig_cur CURSOR FOR " + sel);
    if (!st.ok()) return st;
    cursor_open_ = true;
    return Status::OK();
  }

  Status open_cursor_from(const std::string& table, const std::string& column,
                          const std::string& after_literal) override {
    close_cursor();
    const std::string sel = "SELECT * FROM \"" + table + "\" WHERE \"" + column +
                            "\" > " + after_literal + " ORDER BY \"" + column + "\"";
    Status st = exec("DECLARE mig_cur NO SCROLL CURSOR FOR " + sel);
    if (!st.ok()) st = exec("DECLARE mig_cur CURSOR FOR " + sel);
    if (!st.ok()) return st;
    cursor_open_ = true;
    return Status::OK();
  }

  Status pk_bounds(const std::string& table, const std::string& column,
                   int64_t& lo, int64_t& hi) override {
    const std::string q = "SELECT MIN(\"" + column + "\"), MAX(\"" + column +
                          "\") FROM \"" + table + "\"";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      return Status::Err(Code::kDb, "pg pk_bounds failed: " + table);
    }
    const bool empty = PQntuples(r) == 0 || PQgetisnull(r, 0, 0) ||
                       PQgetisnull(r, 0, 1);
    if (!empty) {
      lo = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
      hi = std::strtoll(PQgetvalue(r, 0, 1), nullptr, 10);
    }
    PQclear(r);
    if (empty) return Status::Err(Code::kNotFound, "empty table: " + table);
    return Status::OK();
  }

  Status open_cursor_range(const std::string& table, const std::string& column,
                           int64_t lo, int64_t hi) override {
    close_cursor();
    const std::string sel = "SELECT * FROM \"" + table + "\" WHERE \"" +
                            column + "\" >= " + std::to_string(lo) +
                            " AND \"" + column + "\" <= " +
                            std::to_string(hi) + " ORDER BY \"" + column + "\"";
    Status st = exec("DECLARE mig_cur NO SCROLL CURSOR FOR " + sel);
    if (!st.ok()) st = exec("DECLARE mig_cur CURSOR FOR " + sel);
    if (!st.ok()) return st;
    cursor_open_ = true;
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
    const std::string& dsn =
        opts_.pg_source_uses_source ? opts_.source : opts_.target;
    return kind + " source (" + dsn_get(dsn, "host", "127.0.0.1") + ":" +
           dsn_get(dsn, "port", "5432") + "/" +
           dsn_get(dsn, "db", "postgres") + ")";
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
