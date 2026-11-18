#include "engine_plan.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "schema.h"

namespace mmv {
namespace vdetail {

KeyValue parse_key(const std::string& t, const NormSpec& s) {
  if (s.type == LogicalType::kInt) return KeyValue::Int(std::strtoll(t.c_str(), nullptr, 10));
  if (s.type == LogicalType::kBytes) return KeyValue::Hex(t);
  return KeyValue::Str(t);
}

bool key_equal(const KeyValue& a, const KeyValue& b) {
  return a.kind == b.kind && a.i == b.i && a.s == b.s;
}

bool key_less(const KeyValue& a, const KeyValue& b) {
  if (a.kind == KeyKind::kInt && b.kind == KeyKind::kInt) return a.i < b.i;
  if (a.kind != KeyKind::kInt && b.kind != KeyKind::kInt) return a.s < b.s;
  return false;
}

bool tuple_equal(const std::vector<KeyValue>& a, const std::vector<KeyValue>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!key_equal(a[i], b[i])) return false;
  return true;
}

bool tuple_less(const std::vector<KeyValue>& a, const std::vector<KeyValue>& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (key_equal(a[i], b[i])) continue;
    return key_less(a[i], b[i]);
  }
  return a.size() < b.size();
}

std::string key_select_list(const std::vector<ColPair>& keys, bool source, Dialect d) {
  std::string sql;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i) sql += ", ";
    sql += sql_key_text(source ? keys[i].src_q : keys[i].tgt_q,
                        source ? keys[i].src_spec : keys[i].tgt_spec, d);
  }
  return sql;
}

std::string key_order_list(const std::vector<ColPair>& keys, bool source) {
  std::string sql;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i) sql += ",";
    sql += source ? keys[i].src_q : keys[i].tgt_q;
  }
  return sql;
}

Status sample_boundaries(SqlBackend& db, const std::string& table_q,
                         const std::vector<ColPair>& keys, int64_t chunk_rows,
                         int64_t max_chunks, std::vector<std::vector<KeyValue>>& out,
                         const std::string& extra_where) {
  out.clear();
  if (chunk_rows <= 0) chunk_rows = 100000;
  std::vector<std::string> qs;
  for (const auto& k : keys) qs.push_back(k.src_q);
  std::vector<KeyValue> last;
  for (int64_t i = 0; i < max_chunks; ++i) {
    std::string where = extra_where.empty() ? "TRUE" : "(" + extra_where + ")";
    if (!last.empty()) {
      TupleRange r;
      r.has_lo = true;
      r.lo = last;
      where += " AND (" + r.where(qs, db.dialect()) + ")";
    }
    const std::string sql = "SELECT " + key_select_list(keys, true, db.dialect()) +
                            " FROM " + table_q + " WHERE " + where + " ORDER BY " +
                            key_order_list(keys, true) + " LIMIT 1 OFFSET " +
                            std::to_string((long long)chunk_rows);
    ResultSet rs;
    Status st = db.query(sql, rs);
    if (!st.ok()) return st;
    if (rs.rows.empty()) break;
    std::vector<KeyValue> b;
    for (size_t j = 0; j < keys.size(); ++j)
      b.push_back(parse_key(cell_at(rs, 0, j), keys[j].src_spec));
    if (!last.empty() && tuple_equal(b, last)) break;
    out.push_back(b);
    last = b;
  }
  return Status::OK();
}

std::vector<TupleRange> ranges_from_bounds(const std::vector<std::vector<KeyValue>>& b) {
  std::vector<TupleRange> out;
  if (b.empty()) {
    out.push_back(TupleRange{});
    return out;
  }
  TupleRange first;
  first.has_hi = true;
  first.hi = b.front();
  out.push_back(first);
  for (size_t i = 0; i + 1 < b.size(); ++i) {
    TupleRange r;
    r.has_lo = r.has_hi = true;
    r.lo = b[i];
    r.hi = b[i + 1];
    out.push_back(r);
  }
  TupleRange last;
  last.has_lo = true;
  last.lo = b.back();
  out.push_back(last);
  return out;
}

std::vector<TupleRange> numeric_ranges(const std::vector<KeyRange>& ks) {
  std::vector<TupleRange> out;
  for (const auto& k : ks) {
    TupleRange r;
    r.has_lo = k.has_lo;
    r.has_hi = k.has_hi;
    if (k.has_lo) r.lo.push_back(KeyValue::Int(k.lo));
    if (k.has_hi) r.hi.push_back(KeyValue::Int(k.hi));
    out.push_back(std::move(r));
  }
  return out;
}

bool split_numeric(const TupleRange& r, TupleRange& a, TupleRange& b) {
  if (!r.has_lo || !r.has_hi || r.lo.size() != 1 || r.hi.size() != 1) return false;
  if (r.lo[0].kind != KeyKind::kInt || r.hi[0].kind != KeyKind::kInt) return false;
  const int64_t lo = r.lo[0].i, hi = r.hi[0].i;
  if (hi - lo <= 1) return false;
  const int64_t mid = lo + (hi - lo) / 2;
  a = r;
  a.hi = {KeyValue::Int(mid)};
  b = r;
  b.lo = {KeyValue::Int(mid)};
  return true;
}

Status split_sampled(SqlBackend& src, const std::string& table_q, const TupleRange& r,
                     const std::vector<ColPair>& keys, int64_t max_rows,
                     TupleRange& a, TupleRange& b, bool& ok) {
  ok = false;
  std::vector<std::string> qs;
  for (const auto& k : keys) qs.push_back(k.src_q);
  const std::string w = r.where(qs, src.dialect());
  ResultSet rs;
  // Bounded count: never scan more than max_rows + 1 rows just to decide.
  const int64_t cap = max_rows > 0 ? max_rows : 5000000;
  const std::string count_sql = "SELECT COUNT(*) FROM (SELECT 1 FROM " + table_q +
                                " WHERE " + w + " LIMIT " + std::to_string(cap + 1) + ") q";
  Status st = src.query(count_sql, rs);
  if (!st.ok()) return st;
  const int64_t n = (int64_t)std::strtoll(cell_at(rs, 0, 0).c_str(), nullptr, 10);
  if (n > cap) return Status::OK();  // too large to bisect; caller falls back to leaf row_diff
  if (n <= 1) return Status::OK();
  const int64_t off = n / 2;
  const std::string sql = "SELECT " + key_select_list(keys, true, src.dialect()) + " FROM " +
                          table_q + " WHERE " + w + " ORDER BY " + key_order_list(keys, true) +
                          " LIMIT 1 OFFSET " + std::to_string((long long)off);
  st = src.query(sql, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty()) return Status::OK();
  std::vector<KeyValue> mid;
  for (size_t j = 0; j < keys.size(); ++j)
    mid.push_back(parse_key(cell_at(rs, 0, j), keys[j].src_spec));
  if (r.first_key_only && mid.size() > 1) mid.resize(1);
  if (r.has_lo && !tuple_less(r.lo, mid)) return Status::OK();
  if (r.has_hi && !tuple_less(mid, r.hi)) return Status::OK();
  a = r;
  a.has_hi = true;
  a.hi = mid;
  b = r;
  b.has_lo = true;
  b.lo = mid;
  ok = true;
  return Status::OK();
}

// MIN/MAX on an indexed key are O(1); COUNT(*) is deliberately avoided.
Status bounds_of(SqlBackend& db, const std::string& table_q, const std::string& key_q,
                 Bounds& out) {
  ResultSet rs;
  Status st = db.query("SELECT MIN(" + key_q + "), MAX(" + key_q + ") FROM " + table_q, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty()) return Status::Err(Code::kDb, "empty bounds result");
  out.has = !rs.rows[0][0].is_null;
  if (out.has) {
    out.min = (int64_t)std::strtoll(cell_at(rs, 0, 0).c_str(), nullptr, 10);
    out.max = (int64_t)std::strtoll(cell_at(rs, 0, 1).c_str(), nullptr, 10);
  }
  return Status::OK();
}

Status table_stats(SqlBackend& db, const std::string& table, TableStats& out) {
  out = TableStats{};
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql = "SELECT COALESCE(c.reltuples,0)::bigint, COALESCE(c.relpages,0)::bigint, "
          "COALESCE((SELECT s.n_tup_ins + s.n_tup_upd + s.n_tup_del "
          "FROM pg_stat_user_tables s WHERE s.relid = c.oid), 0)::bigint "
          "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
          "WHERE n.nspname = current_schema() AND c.relname = " + db.quote_lit(table);
  } else {
    sql = "SELECT COALESCE(TABLE_ROWS,0), COALESCE(AVG_ROW_LENGTH,0), "
          "COALESCE(UNIX_TIMESTAMP(UPDATE_TIME),0) "
          "FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
          "AND TABLE_NAME = " + db.quote_lit(table);
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty()) return Status::OK();
  const int64_t a = (int64_t)std::strtoll(cell_at(rs, 0, 0).c_str(), nullptr, 10);
  const int64_t b = (int64_t)std::strtoll(cell_at(rs, 0, 1).c_str(), nullptr, 10);
  const int64_t c = (int64_t)std::strtoll(cell_at(rs, 0, 2).c_str(), nullptr, 10);
  out.change_token = std::max<int64_t>(0, c);
  if (db.dialect() == Dialect::kPg) {
    out.rows = std::max<int64_t>(0, a);
    out.row_bytes = (out.rows > 0 && b > 0) ? (b * 8192 / out.rows) : 0;
  } else {
    out.rows = std::max<int64_t>(0, a);
    out.row_bytes = std::max<int64_t>(0, b);
  }
  return Status::OK();
}

Status explain_uses_index(SqlBackend& db, const std::string& table_q,
                          const std::string& key_q, bool& uses,
                          const std::string& extra_where) {
  uses = true;
  ResultSet rs;
  Status st = db.query("SELECT MIN(" + key_q + ") FROM " + table_q +
                           (extra_where.empty() ? "" : " WHERE " + extra_where),
                       rs);
  if (!st.ok()) return st;
  if (rs.rows.empty() || rs.rows[0][0].is_null) return Status::OK();
  const int64_t min = (int64_t)std::strtoll(cell_at(rs, 0, 0).c_str(), nullptr, 10);
  if (min == std::numeric_limits<int64_t>::max()) return Status::OK();
  std::string where = key_q + " >= " + std::to_string(min) + " AND " + key_q +
                      " < " + std::to_string(min + 1);
  if (!extra_where.empty()) where = "(" + extra_where + ") AND " + where;
  st = db.query("EXPLAIN SELECT 1 FROM " + table_q + " WHERE " + where, rs);
  if (!st.ok()) return st;
  if (db.dialect() == Dialect::kPg) {
    bool index = false, seq = false;
    for (const auto& row : rs.rows) {
      const std::string line = row.empty() || row[0].is_null ? "" : row[0].text;
      if (line.find("Index Scan") != std::string::npos ||
          line.find("Index Only Scan") != std::string::npos ||
          line.find("Bitmap Index Scan") != std::string::npos)
        index = true;
      if (line.find("Seq Scan") != std::string::npos) seq = true;
    }
    if (!index && seq) uses = false;
  } else {
    int type_idx = -1;
    for (size_t i = 0; i < rs.columns.size(); ++i) {
      if (lower_ascii(rs.columns[i]) == "type") { type_idx = (int)i; break; }
    }
    if (type_idx >= 0) {
      for (size_t i = 0; i < rs.rows.size(); ++i) {
        if (lower_ascii(cell_at(rs, i, (size_t)type_idx)) == "all") uses = false;
      }
    }
  }
  return Status::OK();
}

Status key_indexed(SqlBackend& db, const std::string& table,
                   const std::vector<std::string>& keys, bool& covered) {
  covered = false;
  if (db.is_opengauss()) {
    // openGauss lacks LATERAL; its index metadata query would abort the
    // verification transaction. Allow the scan instead of failing the check.
    covered = true;
    return Status::OK();
  }
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql = "SELECT i.indexrelid::regclass::text, a.attname, s.ord "
          "FROM pg_index i "
          "JOIN pg_class t ON t.oid = i.indrelid "
          "JOIN pg_namespace n ON n.oid = t.relnamespace "
          "JOIN LATERAL generate_subscripts(i.indkey, 1) s(ord) ON true "
          "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = i.indkey[s.ord] "
          "WHERE n.nspname = current_schema() AND t.relname = " + db.quote_lit(table) +
          " AND i.indisvalid AND i.indpred IS NULL ORDER BY 1, 3";
  } else {
    sql = "SELECT INDEX_NAME, COLUMN_NAME, SEQ_IN_INDEX FROM information_schema.STATISTICS "
          "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " + db.quote_lit(table) +
          " ORDER BY INDEX_NAME, SEQ_IN_INDEX";
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  std::map<std::string, std::vector<std::pair<int, std::string>>> idx;
  for (size_t i = 0; i < rs.rows.size(); ++i) {
    const std::string name = cell_at(rs, i, 0);
    const std::string col = cell_at(rs, i, 1);
    const int seq = (int)std::strtol(cell_at(rs, i, 2).c_str(), nullptr, 10);
    if (name.empty() || col.empty()) continue;
    idx[name].push_back({seq, col});
  }
  for (auto& kv : idx) {
    auto& cols = kv.second;
    std::sort(cols.begin(), cols.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (cols.size() < keys.size()) continue;
    bool match = true;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (lower_ascii(cols[i].second) != lower_ascii(keys[i])) { match = false; break; }
    }
    if (match) { covered = true; break; }
  }
  return Status::OK();
}



Status partition_rows(SqlBackend& db, const std::string& table_q,
                      const std::string& where, int64_t& rows) {
  rows = 0;
  ResultSet rs;
  Status st = db.query("SELECT COUNT(*) FROM " + table_q + " WHERE " +
                           (where.empty() ? "TRUE" : where),
                       rs);
  if (!st.ok()) return st;
  if (!rs.rows.empty() && !rs.rows[0].empty() && !rs.rows[0][0].is_null)
    rows = (int64_t)std::strtoll(rs.rows[0][0].text.c_str(), nullptr, 10);
  return Status::OK();
}

Status list_partitions(SqlBackend& db, const std::string& table, bool leaf,
                       std::vector<std::string>& out) {
  out.clear();
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql =
        "SELECT n.nspname || '.' || c.relname FROM pg_partition_tree(" +
        quote_lit(table, Dialect::kPg) +
        "::regclass) pt JOIN pg_class c ON c.oid = pt.relid "
        "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE " +
        std::string(leaf ? "pt.isleaf" : "pt.level = 1") + " ORDER BY 1";
  } else {
    if (leaf) {
      sql =
          "SELECT COALESCE(SUBPARTITION_NAME, PARTITION_NAME) "
          "FROM information_schema.PARTITIONS WHERE TABLE_SCHEMA = DATABASE() "
          "AND TABLE_NAME = " + quote_lit(table, Dialect::kMySQL) +
          " AND (SUBPARTITION_NAME IS NOT NULL OR PARTITION_NAME IS NOT NULL) "
          "ORDER BY PARTITION_ORDINAL_POSITION, SUBPARTITION_ORDINAL_POSITION";
    } else {
      sql =
          "SELECT PARTITION_NAME FROM information_schema.PARTITIONS "
          "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " +
          quote_lit(table, Dialect::kMySQL) +
          " AND PARTITION_NAME IS NOT NULL "
          "GROUP BY PARTITION_NAME ORDER BY MIN(PARTITION_ORDINAL_POSITION)";
    }
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok() && db.dialect() == Dialect::kPg) {
    sql =
        "SELECT n.nspname || '.' || c.relname FROM pg_inherits i "
        "JOIN pg_class p ON p.oid = i.inhparent "
        "JOIN pg_class c ON c.oid = i.inhrelid "
        "JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE p.relname = " + quote_lit(table, Dialect::kPg) +
        " AND n.nspname = current_schema() ORDER BY 1";
    st = db.query(sql, rs);
  }
  if (!st.ok()) return st;
  for (size_t i = 0; i < rs.rows.size(); ++i) out.push_back(cell_at(rs, i, 0));
  return Status::OK();
}

Status partition_bounds(SqlBackend& db, const std::string& table_q,
                        const std::string& key_q, const std::string& where,
                        Bounds& out) {
  out = Bounds{};
  ResultSet rs;
  Status st = db.query("SELECT MIN(" + key_q + "), MAX(" + key_q + "), COUNT(*) FROM " +
                           table_q + " WHERE " + (where.empty() ? "TRUE" : where),
                       rs);
  if (!st.ok()) return st;
  if (rs.rows.empty() || rs.rows[0].size() < 3 || rs.rows[0][0].is_null ||
      rs.rows[0][1].is_null)
    return Status::OK();
  out.has = true;
  out.min = (int64_t)std::strtoll(rs.rows[0][0].text.c_str(), nullptr, 10);
  out.max = (int64_t)std::strtoll(rs.rows[0][1].text.c_str(), nullptr, 10);
  out.rows = (int64_t)std::strtoll(rs.rows[0][2].text.c_str(), nullptr, 10);
  return Status::OK();
}


Status list_partitions_level(SqlBackend& db, const std::string& table, int level,
                             std::vector<std::string>& out) {
  out.clear();
  if (level < 1) return Status::OK();
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql =
        "SELECT n.nspname || '.' || c.relname FROM pg_partition_tree(" +
        quote_lit(table, Dialect::kPg) +
        "::regclass) pt JOIN pg_class c ON c.oid = pt.relid "
        "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE pt.level = " +
        std::to_string(level) + " ORDER BY 1";
  } else if (level == 1) {
    sql =
        "SELECT PARTITION_NAME FROM information_schema.PARTITIONS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " +
        quote_lit(table, Dialect::kMySQL) +
        " AND PARTITION_NAME IS NOT NULL "
        "GROUP BY PARTITION_NAME ORDER BY MIN(PARTITION_ORDINAL_POSITION)";
  } else {
    sql =
        "SELECT SUBPARTITION_NAME FROM information_schema.PARTITIONS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " +
        quote_lit(table, Dialect::kMySQL) +
        " AND SUBPARTITION_NAME IS NOT NULL ORDER BY PARTITION_ORDINAL_POSITION, "
        "SUBPARTITION_ORDINAL_POSITION";
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok() && db.dialect() == Dialect::kPg) {
    if (level != 1) {
      out.clear();
      return Status::OK();
    }
    sql =
        "SELECT n.nspname || '.' || c.relname FROM pg_inherits i "
        "JOIN pg_class p ON p.oid = i.inhparent "
        "JOIN pg_class c ON c.oid = i.inhrelid "
        "JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE p.relname = " + quote_lit(table, Dialect::kPg) +
        " AND n.nspname = current_schema() ORDER BY 1";
    st = db.query(sql, rs);
  }
  if (!st.ok()) return st;
  for (size_t i = 0; i < rs.rows.size(); ++i) out.push_back(cell_at(rs, i, 0));
  return Status::OK();
}

Status partition_depth(SqlBackend& db, const std::string& table, int& depth) {
  depth = 0;
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql = "SELECT COALESCE(MAX(level),0)::text FROM pg_partition_tree(" +
          quote_lit(table, Dialect::kPg) + "::regclass)";
  } else {
    sql =
        "SELECT COALESCE(MAX(CASE WHEN SUBPARTITION_NAME IS NOT NULL THEN 2 ELSE 1 END),0) "
        "FROM information_schema.PARTITIONS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = " + quote_lit(table, Dialect::kMySQL);
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) {
    if (db.dialect() == Dialect::kPg) {
      depth = 1;
      return Status::OK();
    }
    return st;
  }
  if (!rs.rows.empty() && !rs.rows[0].empty() && !rs.rows[0][0].is_null)
    depth = (int)std::strtol(rs.rows[0][0].text.c_str(), nullptr, 10);
  return Status::OK();
}

Status explain_uses_where(SqlBackend& db, const std::string& table_q,
                          const std::string& where, bool& uses) {
  uses = true;
  ResultSet rs;
  Status st = db.query("EXPLAIN SELECT 1 FROM " + table_q + " WHERE " +
                           (where.empty() ? "TRUE" : where),
                       rs);
  if (!st.ok()) return st;
  if (db.dialect() == Dialect::kPg) {
    bool index = false, seq = false;
    for (const auto& row : rs.rows) {
      const std::string line = row.empty() || row[0].is_null ? "" : row[0].text;
      if (line.find("Index Scan") != std::string::npos ||
          line.find("Index Only Scan") != std::string::npos ||
          line.find("Bitmap Index Scan") != std::string::npos)
        index = true;
      if (line.find("Seq Scan") != std::string::npos) seq = true;
    }
    if (!index && seq) uses = false;
  } else {
    int type_idx = -1;
    for (size_t i = 0; i < rs.columns.size(); ++i)
      if (lower_ascii(rs.columns[i]) == "type") { type_idx = (int)i; break; }
    if (type_idx >= 0) {
      for (size_t i = 0; i < rs.rows.size(); ++i)
        if (lower_ascii(cell_at(rs, i, (size_t)type_idx)) == "all") uses = false;
    }
  }
  return Status::OK();
}

Status first_key_where(SqlBackend& db, const std::string& table_q,
                       const std::vector<ColPair>& keys,
                       const std::string& partition_where, std::string& out) {
  out.clear();
  if (keys.empty()) return Status::OK();
  std::vector<std::string> qs;
  for (const auto& k : keys) qs.push_back(k.src_q);
  const std::string sql =
      "SELECT " + key_select_list(keys, true, db.dialect()) + " FROM " + table_q +
      " WHERE " + (partition_where.empty() ? "TRUE" : partition_where) +
      " ORDER BY " + key_order_list(keys, true) + " LIMIT 1";
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty()) return Status::OK();
  TupleRange eq;
  eq.equality = true;
  eq.has_lo = true;
  for (size_t j = 0; j < keys.size(); ++j)
    eq.lo.push_back(parse_key(cell_at(rs, 0, j), keys[j].src_spec));
  out = eq.where(qs, db.dialect(), false);
  return Status::OK();
}


Status sample_key_where(SqlBackend& db, const std::string& table_q,
                        const std::vector<ColPair>& keys,
                        const std::string& partition_where, int n,
                        std::vector<std::string>& out) {
  out.clear();
  if (keys.empty() || n <= 0) return Status::OK();
  std::vector<std::string> qs;
  for (const auto& k : keys) qs.push_back(k.src_q);
  const std::string sql =
      "SELECT " + key_select_list(keys, true, db.dialect()) + " FROM " + table_q +
      " WHERE " + (partition_where.empty() ? "TRUE" : partition_where) +
      " ORDER BY " + key_order_list(keys, true) + " LIMIT " + std::to_string(n);
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  for (size_t r = 0; r < rs.rows.size(); ++r) {
    TupleRange eq;
    eq.equality = true;
    eq.has_lo = true;
    for (size_t j = 0; j < keys.size(); ++j)
      eq.lo.push_back(parse_key(cell_at(rs, r, j), keys[j].src_spec));
    out.push_back(eq.where(qs, db.dialect(), false));
  }
  return Status::OK();
}

Status partition_method(SqlBackend& db, const std::string& table, std::string& out) {
  out.clear();
  std::string sql;
  if (db.dialect() == Dialect::kPg) {
    sql = "SELECT pg_get_partkeydef(" + quote_lit(table, Dialect::kPg) + "::regclass)";
  } else {
    sql =
        "SELECT CONCAT(COALESCE(PARTITION_METHOD,''), ':', "
        "COALESCE(PARTITION_EXPRESSION,'')) FROM information_schema.PARTITIONS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = " +
        quote_lit(table, Dialect::kMySQL) + " LIMIT 1";
  }
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  if (!rs.rows.empty() && !rs.rows[0].empty() && !rs.rows[0][0].is_null)
    out = rs.rows[0][0].text;
  return Status::OK();
}

}  // namespace vdetail
}  // namespace mmv
