#include "backends.h"
#include "db_base.h"
#include "pg_dialect.h"

#include <libpq-fe.h>

#include <cctype>
#include <ctime>
#include <cstring>
#include <cerrno>
#include <sys/select.h>
#include <sys/time.h>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
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


void copy_escape_append(std::string& out, const std::string& s) {
  out.reserve(out.size() + s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += ch;
    }
  }
}

void copy_value_append(std::string& out, const Value& v) {
  switch (v.type) {
    case ValType::kNull: out += "\\N"; break;
    case ValType::kInt: out += std::to_string(v.i); break;
    case ValType::kDouble: {
      char buf[64];
      const int n = std::snprintf(buf, sizeof buf, "%.17g", v.d);
      if (n > 0) out.append(buf, (size_t)n);
      break;
    }
    case ValType::kBool: out += v.b ? "true" : "false"; break;
    case ValType::kBytes: out += copy_value(v); break;
    case ValType::kString:
    default: copy_escape_append(out, v.s); break;
  }
}


void append_be16(std::string& o, uint16_t v) {
  o.push_back((char)((v >> 8) & 0xFF));
  o.push_back((char)(v & 0xFF));
}
void append_be32(std::string& o, uint32_t v) {
  o.push_back((char)((v >> 24) & 0xFF));
  o.push_back((char)((v >> 16) & 0xFF));
  o.push_back((char)((v >> 8) & 0xFF));
  o.push_back((char)(v & 0xFF));
}
void append_be64(std::string& o, uint64_t v) {
  for (int i = 7; i >= 0; --i) o.push_back((char)((v >> (8 * i)) & 0xFF));
}
std::string base_type(const std::string& t) {
  std::string s;
  for (char c : t) s.push_back((char)std::tolower((unsigned char)c));
  const size_t p = s.find('(');
  if (p != std::string::npos) s = s.substr(0, p);
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}
bool pg_binary_supported(const ColumnDef& c) {
  const std::string t = base_type(c.dst_type);
  return t == "smallint" || t == "int2" || t == "int" || t == "integer" ||
         t == "int4" || t == "bigint" || t == "int8" || t == "boolean" ||
         t == "bool" || t == "real" || t == "float4" ||
         t == "double precision" || t == "float8" || t == "text" ||
         t == "varchar" || t == "character varying" || t == "char" ||
         t == "character" || t == "bytea" || t == "date" ||
         t == "timestamp" || t == "timestamp without time zone";
}
bool parse_timestamp_us(const std::string& s, bool date_only, int64_t& out) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (date_only) {
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return false;
  } else {
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 6)
      return false;
  }
  struct tm tm {};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
  tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
  const time_t t = timegm(&tm);
  if (t == (time_t)-1) return false;
  if (date_only) {
    out = (int64_t)((t - 946684800LL) / 86400);
  } else {
    out = (int64_t)(t - 946684800LL) * 1000000LL;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
      const double frac = std::atof(s.c_str() + dot);
      out += (int64_t)(frac * 1000000.0 + 0.5);
    }
  }
  return true;
}
bool encode_binary_value(std::string& out, const Value& v, const ColumnDef& c) {
  const std::string t = base_type(c.dst_type);
  auto to_i = [&]() -> int64_t {
    if (v.type == ValType::kInt) return v.i;
    if (v.type == ValType::kDouble) return (int64_t)v.d;
    return std::strtoll(v.s.c_str(), nullptr, 10);
  };
  if (t == "smallint" || t == "int2") { append_be16(out, (uint16_t)to_i()); return true; }
  if (t == "int" || t == "integer" || t == "int4") { append_be32(out, (uint32_t)to_i()); return true; }
  if (t == "bigint" || t == "int8") { append_be64(out, (uint64_t)to_i()); return true; }
  if (t == "boolean" || t == "bool") {
    out.push_back((char)(((v.type == ValType::kBool) ? v.b : (to_i() != 0)) ? 1 : 0));
    return true;
  }
  if (t == "real" || t == "float4") {
    const float f = (v.type == ValType::kDouble) ? (float)v.d : (float)std::strtod(v.s.c_str(), nullptr);
    uint32_t bits; std::memcpy(&bits, &f, sizeof bits); append_be32(out, bits); return true;
  }
  if (t == "double precision" || t == "float8") {
    const double d = (v.type == ValType::kDouble) ? v.d : std::strtod(v.s.c_str(), nullptr);
    uint64_t bits; std::memcpy(&bits, &d, sizeof bits); append_be64(out, bits); return true;
  }
  if (t == "text" || t == "varchar" || t == "character varying" || t == "char" || t == "character") {
    if (v.type == ValType::kString || v.type == ValType::kBytes) out += v.s;
    else out += v.sql_literal();
    return true;
  }
  if (t == "bytea") { out += v.s; return true; }
  if (t == "date" || t == "timestamp" || t == "timestamp without time zone") {
    int64_t micros = 0;
    if (!parse_timestamp_us(v.s, t == "date", micros)) return false;
    if (t == "date") append_be32(out, (uint32_t)micros);
    else append_be64(out, (uint64_t)micros);
    return true;
  }
  return false;
}

class PgTarget : public ITarget, public DbBase {
 public:
  explicit PgTarget(const Options& o)
      : DbBase(o), use_copy_(o.use_copy && !o.resume), resume_(o.resume),
        og_requested_(o.opengauss), og_(o.opengauss) {}
  ~PgTarget() override { close(); }

  Status connect() override {
    const std::string host = dsn_get(opts_.target, "host", "127.0.0.1");
    const std::string port = dsn_get(opts_.target, "port", "5432");
    const std::string user = dsn_get(opts_.target, "user", "postgres");
    const std::string pw_key = std::string("pass") + "word";
    const std::string pw = dsn_get(opts_.target, pw_key, "");
    const std::string db = dsn_get(opts_.target, "db", "postgres");

    const std::string ci =
        "host=" + cq(host) + " port=" + cq(port) + " user=" + cq(user) +
        " password=" + cq(pw) + " dbname=" + cq(db) +
        " connect_timeout=" + std::to_string(connect_timeout()) +
        " keepalives=1 keepalives_idle=30 sslmode=" + cq(opts_.sslmode);
    conn_ = PQconnectdb(ci.c_str());

    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
      std::string e = conn_ ? PQerrorMessage(conn_) : "PQconnectdb failed";
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
    server_is_og_ = PgDialect::is_open_gauss(server_version_);
    // 请求 openGauss 但连到 PG 时，不做 openGauss 特化（describe 会标注）
    if (og_ && !server_is_og_) og_ = false;

    // openGauss 的 numeric 输出默认省略整数位的 0（.17），
    // 打开 display_leading_zero 才和 MySQL/PostgreSQL 的 0.17 一致
    const std::string compat = PgDialect::compat_sql(og_);
    if (!compat.empty()) exec_simple(compat);

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

  Status drop_table(const std::string& table) override {
    return exec_sql("DROP TABLE IF EXISTS \"" + table + "\" CASCADE;");
  }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    Status st = end_copy();
    if (!st.ok()) return st;
    st = exec_sql(ddl);
    if (!st.ok()) return st;
    std::lock_guard<std::mutex> lk(mu_);
    written_[s.name] = 0;
    schemas_[s.name] = s;
    return Status::OK();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    if (rows.empty()) return Status::OK();
    if (!use_copy_) return insert_batch(table, rows);

    bool binary = false;
    auto sit = schemas_.find(table);
    if (!og_ && sit != schemas_.end()) {
      bool has_bytea = false;
      for (const auto& c : sit->second.columns) {
        if (base_type(c.dst_type) == "bytea") { has_bytea = true; break; }
      }
      if (has_bytea) {
        binary = true;
        for (const auto& c : sit->second.columns) {
          if (!pg_binary_supported(c)) { binary = false; break; }
        }
      }
    }
    if (binary) {
      if (!copying_ || copy_table_ != table || !copy_binary_) {
        Status st = begin_copy_binary(table);
        if (!st.ok()) return st;
      }
      const TableSchema& s = schemas_[table];
      std::string bbuf;
      const size_t flush_at = 4 * 1024 * 1024;
      bbuf.reserve(flush_at);
      auto flush_b = [&]() -> Status {
        if (bbuf.empty()) return Status::OK();
        if (PQputCopyData(conn_, bbuf.data(), (int)bbuf.size()) != 1)
          return Status::Err(Code::kDb, std::string("PQputCopyData: ") + PQerrorMessage(conn_));
        Status fst = flush_copy_output();
        if (!fst.ok())
          return Status::Err(Code::kDb, std::string("PQflush: ") + PQerrorMessage(conn_));
        bbuf.clear();
        return Status::OK();
      };
      for (const auto& r : rows) {
        if (r.size() != s.columns.size())
          return Status::Err(Code::kInvalid, "binary copy column count mismatch");
        append_be16(bbuf, (uint16_t)s.columns.size());
        for (size_t j = 0; j < r.size(); ++j) {
          if (r[j].type == ValType::kNull) { append_be32(bbuf, 0xFFFFFFFFu); continue; }
          std::string field;
          if (!encode_binary_value(field, r[j], s.columns[j]))
            return Status::Err(Code::kInvalid, "binary copy unsupported type");
          append_be32(bbuf, (uint32_t)field.size());
          bbuf += field;
        }
        if (bbuf.size() >= flush_at) {
          Status fs = flush_b();
          if (!fs.ok()) return fs;
        }
      }
      Status fs = flush_b();
      if (!fs.ok()) return fs;
      std::lock_guard<std::mutex> lk(mu_);
      written_[table] += (int64_t)rows.size();
      return Status::OK();
    }
    if (!copying_ || copy_table_ != table) {
      Status st = begin_copy(table);
      if (!st.ok()) return st;
    }

    std::string buf;
    const size_t per_row = rows[0].size() * 8 + 8;
    const size_t flush_at = 4 * 1024 * 1024;
    const size_t reserve_bytes = rows.size() * per_row;
    buf.reserve(reserve_bytes < flush_at ? reserve_bytes : flush_at);
    for (const auto& r : rows) {
      for (size_t j = 0; j < r.size(); ++j) {
        if (j) buf += '\t';
        copy_value_append(buf, r[j]);
      }
      buf += '\n';
      if (buf.size() >= flush_at) {
        if (PQputCopyData(conn_, buf.data(), (int)buf.size()) != 1) {
          return Status::Err(Code::kDb, std::string("PQputCopyData: ") + PQerrorMessage(conn_));
        }
        Status fst = flush_copy_output();
        if (!fst.ok()) {
          return Status::Err(Code::kDb, std::string("PQflush: ") + PQerrorMessage(conn_));
        }
        buf.clear();
      }
    }
    if (!buf.empty()) {
      if (PQputCopyData(conn_, buf.data(), (int)buf.size()) != 1) {
        return Status::Err(Code::kDb, std::string("PQputCopyData: ") + PQerrorMessage(conn_));
      }
      Status fst = flush_copy_output();
      if (!fst.ok()) {
        return Status::Err(Code::kDb, std::string("PQflush: ") + PQerrorMessage(conn_));
      }
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

  bool supports_upsert() const override { return PgDialect::supports_upsert(og_); }

  // PostgreSQL/openGauss index names are schema-global.  Keep the
  // table-scoped name mapping here so pipeline.cpp has no target branch.
  std::string index_ddl(const std::string& table, const IndexDef& idx) override {
    const std::string key = table + "." + idx.name;
    auto it = index_names_.find(key);
    if (it != index_names_.end()) {
      IndexDef adjusted = idx;
      adjusted.name = it->second;
      return build_index_ddl(table, adjusted);
    }
    std::lock_guard<std::mutex> lk(index_mu_);
    std::string name = idx.name;
    int n = 0;
    while (!used_index_names_.insert(name).second) {
      name = idx.name + "_" + table;
      if (n++ > 0) name += "_" + std::to_string(n);
    }
    index_names_[key] = name;
    IndexDef adjusted = idx;
    adjusted.name = name;
    return build_index_ddl(table, adjusted);
  }

  void reset_names() override {
    std::lock_guard<std::mutex> lk(index_mu_);
    used_index_names_.clear();
    index_names_.clear();
  }

  int64_t written(const std::string& table) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_) return 0;
    const std::string q = "SELECT COUNT(*) FROM \"" + table + "\"";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
      if (r) PQclear(r);
      // Missing table / failed query aborts the transaction.  Recover so a
      // later repair on another table is not blocked by an aborted tx.
      exec_simple("ROLLBACK");
      exec_simple("BEGIN");
      return 0;
    }
    int64_t n = 0;
    if (PQntuples(r) > 0)
      n = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
    PQclear(r);
    return n;
  }

  std::string describe() override {
    std::string mode = use_copy_ ? "COPY" : "INSERT";
    std::string kind = PgDialect::name(server_is_og_);
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

  Status flush_copy_output() {
    int rc = PQflush(conn_);
    while (rc == 1) {
      const int sock = PQsocket(conn_);
      if (sock < 0) return Status::Err(Code::kDb, "PQflush: invalid socket");
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(sock, &wfds);
      struct timeval tv;
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      const int sr = select(sock + 1, nullptr, &wfds, nullptr, &tv);
      if (sr < 0)
        return Status::Err(Code::kDb, std::string("select: ") + std::strerror(errno));
      rc = PQflush(conn_);
    }
    if (rc < 0)
      return Status::Err(Code::kDb, std::string("PQflush: ") + PQerrorMessage(conn_));
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

  Status begin_copy_binary(const std::string& table) {
    Status st = end_copy();
    if (!st.ok()) return st;
    std::string q = "COPY ";
    q.push_back('"');
    q += table;
    q.push_back('"');
    q += " FROM STDIN WITH (FORMAT binary)";
    PGresult* r = PQexec(conn_, q.c_str());
    if (!r) return Status::Err(Code::kDb, "COPY binary exec failed");
    const bool ok = PQresultStatus(r) == PGRES_COPY_IN;
    std::string e = ok ? std::string() : PQresultErrorMessage(r);
    PQclear(r);
    if (!ok) return Status::Err(Code::kDb, "COPY binary begin: " + e);

    copying_ = true;
    copy_table_ = table;
    copy_binary_ = true;
    std::string hdr = "PGCOPY";
    hdr.push_back((char)10);
    hdr.push_back((char)255);
    hdr.push_back((char)13);
    hdr.push_back((char)10);
    hdr.push_back((char)0);
    append_be32(hdr, 0);
    append_be32(hdr, 0);
    if (PQputCopyData(conn_, hdr.data(), (int)hdr.size()) != 1)
      return Status::Err(Code::kDb, std::string("PQputCopyData hdr: ") + PQerrorMessage(conn_));
    Status fst = flush_copy_output();
    if (!fst.ok())
      return Status::Err(Code::kDb, std::string("PQflush hdr: ") + PQerrorMessage(conn_));
    return Status::OK();
  }

  Status end_copy() {
    if (!copying_) return Status::OK();
    const bool was_binary = copy_binary_;
    copying_ = false;
    copy_binary_ = false;

    if (was_binary) {
      std::string tr;
      append_be16(tr, (uint16_t)0xFFFF);
      if (PQputCopyData(conn_, tr.data(), (int)tr.size()) != 1)
        return Status::Err(Code::kDb, std::string("PQputCopyData tr: ") + PQerrorMessage(conn_));
      Status fst = flush_copy_output();
      if (!fst.ok())
        return Status::Err(Code::kDb, std::string("PQflush tr: ") + PQerrorMessage(conn_));
    }

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
    if (resume_) o << " ON CONFLICT DO NOTHING";
    Status st = exec_simple(o.str());
    if (!st.ok()) return st;
    std::lock_guard<std::mutex> lk(mu_);
    written_[table] += (int64_t)rows.size();
    return Status::OK();
  }

  PGconn* conn_ = nullptr;
  bool use_copy_ = true;
  bool resume_ = false;
  bool og_requested_ = false;
  bool og_ = false;
  bool server_is_og_ = false;
  std::string server_version_;
  bool copying_ = false;
  bool copy_binary_ = false;
  std::string copy_table_;
  std::map<std::string, TableSchema> schemas_;
  std::mutex mu_;
  std::map<std::string, int64_t> written_;
  std::map<std::string, std::string> index_names_;
  static std::mutex index_mu_;
  static std::set<std::string> used_index_names_;
};

std::mutex PgTarget::index_mu_;
std::set<std::string> PgTarget::used_index_names_;

}  // namespace

std::shared_ptr<ITarget> make_pg_target(const Options& o, Status& st) {
  st = Status::OK();
  return std::make_shared<PgTarget>(o);
}

}  // namespace mig
