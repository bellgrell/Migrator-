#include "cdc.h"

#include <libpq-fe.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends.h"

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

std::string cq(const std::string& v) {
  std::string o = "'";
  for (char c : v) { if (c == '\\' || c == '\'') o += '\\'; o += c; }
  o += "'";
  return o;
}

std::string pg_conninfo(const Options& o) {
  const std::string host = dsn_get(o.source, "host", "127.0.0.1");
  const std::string port = dsn_get(o.source, "port", "5432");
  const std::string user = dsn_get(o.source, "user", "postgres");
  const std::string pw = dsn_get(o.source, "password", "");
  const std::string db = dsn_get(o.source, "db", "postgres");
  return "host=" + cq(host) + " port=" + cq(port) + " user=" + cq(user) +
         " password=" + cq(pw) + " dbname=" + cq(db) +
         " connect_timeout=" +
         std::to_string(o.connect_timeout > 0 ? o.connect_timeout : 10) +
         " keepalives=1 keepalives_idle=30 sslmode=" + cq(o.sslmode);
}

struct JVal {
  enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
  bool b = false;
  std::string raw;
  std::vector<JVal> arr;
  std::map<std::string, JVal> obj;
  const JVal* get(const std::string& k) const {
    auto it = obj.find(k);
    return it == obj.end() ? nullptr : &it->second;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& s) : s_(s) {}
  bool parse(JVal& out) {
    ws();
    if (!value(out)) return false;
    ws();
    return p_ == s_.size();
  }

 private:
  const std::string& s_;
  size_t p_ = 0;

  void ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r'))
      ++p_;
  }
  bool lit(const char* x) {
    const size_t n = std::strlen(x);
    if (s_.compare(p_, n, x) != 0) return false;
    p_ += n;
    return true;
  }
  bool string(std::string& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return false;
    ++p_;
    out.clear();
    while (p_ < s_.size()) {
      char c = s_[p_++];
      if (c == '"') return true;
      if (c != '\\') { out.push_back(c); continue; }
      if (p_ >= s_.size()) return false;
      char e = s_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (p_ + 4 > s_.size()) return false;
          auto hex = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return -1;
          };
          int v = 0;
          for (int i = 0; i < 4; ++i) {
            int d = hex(s_[p_++]);
            if (d < 0) return false;
            v = v * 16 + d;
          }
          if (v < 0x80) out.push_back((char)v);
          else if (v < 0x800) {
            out.push_back((char)(0xC0 | (v >> 6)));
            out.push_back((char)(0x80 | (v & 0x3F)));
          } else {
            out.push_back((char)(0xE0 | (v >> 12)));
            out.push_back((char)(0x80 | ((v >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (v & 0x3F)));
          }
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(JVal& v) {
    ws();
    if (p_ >= s_.size()) return false;
    if (s_[p_] == '{') {
      v.t = JVal::OBJ;
      ++p_;
      ws();
      if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
      while (true) {
        std::string k;
        ws();
        if (!string(k)) return false;
        ws();
        if (p_ >= s_.size() || s_[p_] != ':') return false;
        ++p_;
        JVal x;
        if (!value(x)) return false;
        v.obj[k] = std::move(x);
        ws();
        if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        return false;
      }
    }
    if (s_[p_] == '[') {
      v.t = JVal::ARR;
      ++p_;
      ws();
      if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
      while (true) {
        JVal x;
        if (!value(x)) return false;
        v.arr.push_back(std::move(x));
        ws();
        if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        return false;
      }
    }
    if (s_[p_] == '"') {
      v.t = JVal::STR;
      return string(v.raw);
    }
    if (lit("true")) { v.t = JVal::BOOL; v.b = true; return true; }
    if (lit("false")) { v.t = JVal::BOOL; v.b = false; return true; }
    if (lit("null")) { v.t = JVal::NUL; return true; }
    const size_t start = p_;
    if (s_[p_] == '-') ++p_;
    while (p_ < s_.size() && ((s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '.' ||
                              s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-'))
      ++p_;
    if (p_ == start) return false;
    v.t = JVal::NUM;
    v.raw = s_.substr(start, p_ - start);
    return true;
  }
};

std::string jstr(const JVal* v) {
  return (v && v->t == JVal::STR) ? v->raw : std::string();
}

bool hex_decode(const std::string& in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (in.size() % 2) return false;
  out.clear();
  for (size_t i = 0; i < in.size(); i += 2) {
    int a = val(in[i]), b = val(in[i + 1]);
    if (a < 0 || b < 0) return false;
    out.push_back((char)((a << 4) | b));
  }
  return true;
}

Value to_value(const JVal& v, const ColumnDef& c) {
  if (v.t == JVal::NUL) return Value::Null();
  if (v.t == JVal::BOOL) return Value::Bool(v.b);
  if (v.t == JVal::STR) {
    if (c.src_type.find("bytea") != std::string::npos) {
      std::string hex = v.raw;
      if (hex.size() >= 2 && hex[0] == '\\' && hex[1] == 'x') hex = hex.substr(2);
      std::string raw;
      if (hex_decode(hex, raw)) return Value::Bytes(raw);
    }
    return Value::Str(v.raw);
  }
  if (v.t == JVal::NUM) {
    const std::string& t = c.src_type;
    if (t.find("int") != std::string::npos)
      return Value::Int(std::strtoll(v.raw.c_str(), nullptr, 10));
    if (t.find("double") != std::string::npos || t.find("real") != std::string::npos)
      return Value::Dbl(std::strtod(v.raw.c_str(), nullptr));
    return Value::Str(v.raw);
  }
  return Value::Str("");
}

Status build_event(const JVal& ch, const TableSchema& s, CdcEvent& ev) {
  const JVal* kind = ch.get("kind");
  if (!kind || kind->t != JVal::STR) return Status::Err(Code::kInvalid, "wal2json: no kind");
  const std::string k = kind->raw;
  if (k == "insert" || k == "update") {
    const JVal* names = ch.get("columnnames");
    const JVal* vals = ch.get("columnvalues");
    if (!names || !vals || names->t != JVal::ARR || vals->t != JVal::ARR ||
        names->arr.size() != vals->arr.size())
      return Status::Err(Code::kInvalid, "wal2json: bad columns");
    ev.op = (k == "insert") ? 0 : 1;
    ev.after.assign(s.columns.size(), Value::Null());
    for (size_t i = 0; i < names->arr.size(); ++i) {
      const std::string n = jstr(&names->arr[i]);
      for (size_t j = 0; j < s.columns.size(); ++j) {
        if (s.columns[j].name == n) {
          ev.after[j] = to_value(vals->arr[i], s.columns[j]);
          break;
        }
      }
    }
    return Status::OK();
  }
  if (k == "delete") {
    const JVal* ok = ch.get("oldkeys");
    if (!ok) return Status::Err(Code::kInvalid, "wal2json: delete without oldkeys");
    const JVal* kn = ok->get("keynames");
    const JVal* kv = ok->get("keyvalues");
    if (!kn || !kv || kn->t != JVal::ARR || kv->t != JVal::ARR ||
        kn->arr.size() != kv->arr.size())
      return Status::Err(Code::kInvalid, "wal2json: bad oldkeys");
    ev.op = 2;
    ev.before.assign(s.columns.size(), Value::Null());
    for (size_t i = 0; i < kn->arr.size(); ++i) {
      const std::string n = jstr(&kn->arr[i]);
      for (size_t j = 0; j < s.columns.size(); ++j) {
        if (s.columns[j].name == n) {
          ev.before[j] = to_value(kv->arr[i], s.columns[j]);
          break;
        }
      }
    }
    return Status::OK();
  }
  return Status::Err(Code::kInvalid, "wal2json: unsupported kind " + k);
}

bool query_ok(PGconn* c, const std::string& sql, PGresult** out = nullptr) {
  PGresult* r = PQexec(c, sql.c_str());
  if (!r) return false;
  const ExecStatusType st = PQresultStatus(r);
  if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
    if (out) *out = r;
    else PQclear(r);
    return false;
  }
  if (out) *out = r;
  else PQclear(r);
  return true;
}

void save_position(const Options& o, const std::string& lsn) {
  std::ofstream out(o.pg_cdc_position_path, std::ios::trunc);
  if (out) out << lsn;
}

}  // namespace

Status run_pg_cdc(const Options& o, Metrics& m, int max_events) {
  if (!o.module_enabled("pg_cdc"))
    return Status::Err(Code::kUnsupported, "PG CDC module disabled");
  if (o.source_kind != "pg" && o.source_kind != "og")
    return Status::Err(Code::kUnsupported,
                       "PG CDC requires a PostgreSQL/openGauss source");
  if (o.target_kind == "mysql")
    return Status::Err(Code::kUnsupported, "PG CDC target MySQL is not supported");

  PGconn* conn = PQconnectdb(pg_conninfo(o).c_str());
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::string e = conn ? PQerrorMessage(conn) : "PQconnectdb failed";
    if (conn) PQfinish(conn);
    return Status::Err(Code::kDb, "pg cdc connect: " + e);
  }

  const std::string slot = o.cdc_slot.empty() ? "migrator_slot" : o.cdc_slot;
  {
    PGresult* r = nullptr;
    const std::string check = "SELECT 1 FROM pg_replication_slots WHERE slot_name=" + cq(slot);
    if (query_ok(conn, check, &r)) {
      const bool exists = r && PQntuples(r) > 0;
      if (r) PQclear(r);
      if (!exists) {
        const std::string create =
            "SELECT pg_create_logical_replication_slot(" + cq(slot) + ", 'wal2json')";
        if (!query_ok(conn, create)) {
          std::string e = PQerrorMessage(conn);
          PQfinish(conn);
          return Status::Err(Code::kDb, "create slot: " + e);
        }
        m.log("pg cdc slot created: " + slot);
      }
    } else {
      if (r) PQclear(r);
    }
  }

  Status s1;
  auto src = make_pg_source(o, s1);
  if (!src || !src->connect().ok()) {
    PQfinish(conn);
    return Status::Err(Code::kDb, "pg cdc source connect failed");
  }
  Status s2;
  auto tgt = make_target(o, s2);
  if (!tgt || !tgt->connect().ok()) {
    PQfinish(conn);
    return Status::Err(Code::kDb, "pg cdc target connect failed");
  }

  std::map<std::string, TableSchema> schemas;
  std::string last_lsn;
  int applied = 0;

  while (true) {
    // PostgreSQL 与 openGauss 的列名不同（lsn vs location），统一按位置取列：
    // 0 = lsn/location, 1 = xid, 2 = data
    const std::string q =
        "SELECT * FROM pg_logical_slot_peek_changes(" + cq(slot) +
        ", NULL, NULL, 'pretty-print', '0')";
    PGresult* r = PQexec(conn, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      std::string e = r ? PQresultErrorMessage(r) : PQerrorMessage(conn);
      if (r) PQclear(r);
      PQreset(conn);
      m.log("[warn] pg cdc query: " + e + ", retrying");
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    const int n = PQntuples(r);
    if (n == 0) {
      PQclear(r);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }

    for (int i = 0; i < n; ++i) {
      const int nfields = PQnfields(r);
      const std::string lsn = PQgetvalue(r, i, 0);
      const std::string data = PQgetvalue(r, i, nfields >= 3 ? 2 : 1);
      JVal root;
      JParser jp(data);
      if (!jp.parse(root)) {
        m.log("[warn] pg cdc: bad JSON at " + lsn);
        continue;
      }
      const JVal* changes = root.get("change");
      if (!changes || changes->t != JVal::ARR) continue;
      for (const auto& ch : changes->arr) {
        const std::string table = jstr(ch.get("table"));
        if (table.empty()) continue;
        auto it = schemas.find(table);
        if (it == schemas.end()) {
          TableSchema s;
          if (!src->read_schema(table, s).ok()) continue;
          schemas[table] = s;
          it = schemas.find(table);
        }
        CdcEvent ev;
        Status bs = build_event(ch, it->second, ev);
        if (!bs.ok()) {
          m.log("[warn] pg cdc: " + bs.str());
          continue;
        }
        ev.table = table;
        Status as = apply_cdc_event(*tgt, it->second, ev);
        if (as.ok()) {
          ++applied;
          m.rows_done += 1;
        } else {
          m.errors += 1;
          m.log("[error] pg cdc apply " + table + ": " + as.str());
        }
      }
      last_lsn = lsn;
    }
    PQclear(r);

    Status cs = tgt->commit();
    if (!cs.ok()) m.log("[warn] pg cdc commit: " + cs.str());

    if (!last_lsn.empty()) {
      const std::string adv = "SELECT pg_replication_slot_advance(" + cq(slot) +
                              ", " + cq(last_lsn) + ")";
      if (!query_ok(conn, adv)) m.log("[warn] pg cdc advance: " + std::string(PQerrorMessage(conn)));
      save_position(o, last_lsn);
      m.log("pg cdc applied " + std::to_string(applied) + " changes, lsn=" + last_lsn);

      const std::string lagq =
          "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), confirmed_flush_lsn) "
          "FROM pg_replication_slots WHERE slot_name=" + cq(slot);
      PGresult* lr = PQexec(conn, lagq.c_str());
      if (lr && PQresultStatus(lr) == PGRES_TUPLES_OK && PQntuples(lr) > 0 &&
          !PQgetisnull(lr, 0, 0)) {
        m.cdc_lag_bytes.store(
            (int64_t)std::strtoll(PQgetvalue(lr, 0, 0), nullptr, 10));
      }
      if (lr) PQclear(lr);

      if (!o.cdc_until_lsn.empty()) {
        const std::string cmp = "SELECT pg_wal_lsn_diff(" + cq(last_lsn) +
                                ", " + cq(o.cdc_until_lsn) + ")";
        PGresult* cr = PQexec(conn, cmp.c_str());
        if (cr && PQresultStatus(cr) == PGRES_TUPLES_OK && PQntuples(cr) > 0 &&
            !PQgetisnull(cr, 0, 0)) {
          const long long diff =
              std::strtoll(PQgetvalue(cr, 0, 0), nullptr, 10);
          PQclear(cr);
          if (diff >= 0) {
            m.log("pg cdc caught up to " + o.cdc_until_lsn);
            break;
          }
        } else if (cr) {
          PQclear(cr);
        }
      }
    }

    if (max_events > 0 && applied >= max_events) break;
  }

  PQfinish(conn);
  return Status::OK();
}

}  // namespace mig
