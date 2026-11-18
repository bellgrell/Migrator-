#include "engine_digest.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace mmv {
namespace vdetail {

namespace {

std::string parse_dec(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "0";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

}  // namespace

Status digest_range(SqlBackend& db, const std::string& table_q, const std::string& where,
                    const std::vector<ColPair>& cols, bool source, Digest& out,
                    int retries, int backoff_ms, bool check_json_overflow,
                    bool use_md5) {
  std::vector<std::pair<std::string, NormSpec>> fields;
  for (const auto& c : cols)
    fields.emplace_back(source ? c.src_q : c.tgt_q, source ? c.src_spec : c.tgt_spec);
  const std::string sql =
      sql_digest(table_q, where, fields, db.dialect(), check_json_overflow, use_md5);
  ResultSet rs;
  Status st;
  for (int attempt = 0;; ++attempt) {
    st = db.query(sql, rs);
    if (st.ok()) break;
    if (attempt >= retries) return st;
    std::fprintf(stderr, "[retry] %s digest %d/%d: %s\n", source ? "source" : "target",
                 attempt + 1, retries, st.msg.c_str());
    sleep_ms(backoff_ms * (attempt + 1));
  }
  if (rs.rows.size() != 1) return Status::Err(Code::kDb, "digest returned no row");
  out.n = (int64_t)std::strtoll(cell_at(rs, 0, 0).c_str(), nullptr, 10);
  out.h1 = parse_dec(cell_at(rs, 0, 1));
  out.h2 = parse_dec(cell_at(rs, 0, 2));
  out.max_row_bytes = (int64_t)std::strtoll(cell_at(rs, 0, 3).c_str(), nullptr, 10);
  out.json_overflow =
      rs.rows[0].size() > 4
          ? (int64_t)std::strtoll(cell_at(rs, 0, 4).c_str(), nullptr, 10)
          : 0;
  // MySQL GROUP_CONCAT limit is 64 MiB; flag near-limit rows instead of
  // silently accepting a truncated canonical string.
  out.truncated = db.dialect() == Dialect::kMySQL &&
                  out.max_row_bytes >= (64LL * 1024 * 1024) - 4096;
  return Status::OK();
}

}  // namespace vdetail
}  // namespace mmv
