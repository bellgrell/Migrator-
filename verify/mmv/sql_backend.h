#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "normalize.h"
#include "status.h"
#include "value.h"

namespace mmv {

class SqlBackend {
 public:
  virtual ~SqlBackend() = default;
  virtual Status connect(const std::string& dsn) = 0;
  virtual Status query(const std::string& sql, ResultSet& out) = 0;
  // Stream rows to `fn` without materializing the result set. Returning false
  // stops delivery; the backend still drains the query so the connection stays
  // usable. The default implementation falls back to query().
  virtual Status query_stream(const std::string& sql,
                              const std::function<bool(const std::vector<Cell>&)>& fn) {
    ResultSet rs;
    Status st = query(sql, rs);
    if (!st.ok()) return st;
    for (const auto& row : rs.rows) {
      if (!fn(row)) break;
    }
    return Status::OK();
  }
  virtual Status exec(const std::string& sql) = 0;
  virtual Dialect dialect() const = 0;
  virtual std::string quote_ident(const std::string& name) const = 0;
  virtual std::string quote_lit(const std::string& s) const = 0;
  virtual std::string version() const = 0;
  // openGauss is PostgreSQL-compatible but lacks sha256(bytea), to_jsonb(),
  // LATERAL and WITH ORDINALITY; callers use this to pick compatible SQL.
  virtual bool is_opengauss() const { return false; }
  virtual std::string describe() const = 0;
  // Cheap data-change token (WAL LSN / binlog position).
  // Unsupported by default so resume cannot silently reuse stale chunks.
  virtual Status change_token(std::string& out) {
    out.clear();
    return Status::Err(Code::kUnsupported, "data-change token not supported");
  }
  // Table-scoped change token; falls back to the global token.
  virtual Status table_change_token(const std::string& table, std::string& out) {
    (void)table;
    return change_token(out);
  }
  virtual Status set_timeout(int seconds) {
    (void)seconds;
    return Status::Err(Code::kUnsupported, "per-query timeout not supported");
  }
  virtual Status begin_readonly() {
    return Status::Err(Code::kUnsupported, "read-only snapshot not supported");
  }
  virtual Status end_readonly() {
    return Status::Err(Code::kUnsupported, "read-only snapshot not supported");
  }
  // Export a snapshot that other connections can join (PostgreSQL only).
  virtual Status begin_global_read_lock() {
    return Status::Err(Code::kUnsupported, "global read lock not supported");
  }
  virtual Status end_global_read_lock() {
    return Status::Err(Code::kUnsupported, "global read lock not supported");
  }
  virtual Status begin_export_snapshot(std::string& id) {
    id.clear();
    return Status::Err(Code::kUnsupported, "snapshot export not supported");
  }
  virtual Status join_snapshot(const std::string& id) {
    (void)id;
    return begin_readonly();
  }
};

// kind: pg | og | mysql
std::shared_ptr<SqlBackend> make_backend(const std::string& kind, const std::string& dsn,
                                         Status& st);

}  // namespace mmv
