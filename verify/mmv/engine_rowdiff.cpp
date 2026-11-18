#include "engine_rowdiff.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mmv {
namespace vdetail {

std::string key_id(const std::vector<std::string>& f) {
  std::string o;
  for (const auto& s : f) o += std::to_string(s.size()) + ":" + s + "|";
  return o;
}

std::string key_show(const std::vector<std::string>& f) {
  std::string o;
  for (size_t i = 0; i < f.size(); ++i) {
    if (i) o += ",";
    o += f[i];
  }
  return o;
}

struct RowRec {
  std::vector<std::string> values;
  std::vector<std::string> key_values;
  std::string display;
};

namespace {

std::string row_sql(SqlBackend& db, const std::string& table_q, const std::string& where,
                    const std::vector<ColPair>& keys, const std::vector<ColPair>& cols,
                    bool source, bool client_norm, bool skip_blobs, size_t cap) {
  std::string sql = "SELECT ";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i) sql += ", ";
    sql += sql_key_text(source ? keys[i].src_q : keys[i].tgt_q,
                        source ? keys[i].src_spec : keys[i].tgt_spec, db.dialect());
  }
  for (const auto& c : cols) {
    const NormSpec& sp = source ? c.src_spec : c.tgt_spec;
    const std::string& q = source ? c.src_q : c.tgt_q;
    if (skip_blobs && sp.type == LogicalType::kBytes)
      sql += ", NULL";
    else if (client_norm && (sp.type == LogicalType::kString || sp.type == LogicalType::kChar))
      sql += ", " + (db.dialect() == Dialect::kPg ? "(" + q + ")::text"
                                                   : "CAST(" + q + " AS CHAR)");
    else if (sp.type == LogicalType::kBytes)
      sql += ", CASE WHEN " + q + " IS NULL THEN NULL ELSE " +
             sql_blob_digest(q, db.dialect()) + " END";
    else
      sql += ", " + sql_field(q, sp, db.dialect());
  }
  sql += " FROM " + table_q + " WHERE " + where + " ORDER BY ";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i) sql += ",";
    sql += source ? keys[i].src_q : keys[i].tgt_q;
  }
  sql += " LIMIT " + std::to_string((long long)cap + 1);
  return sql;
}

RowRec make_rec(const std::vector<Cell>& row, const std::vector<ColPair>& keys,
                const std::vector<ColPair>& cols, bool source, bool client_norm,
                bool skip_blobs) {
  RowRec rec;
  rec.key_values.reserve(keys.size());
  for (size_t i = 0; i < keys.size(); ++i)
    rec.key_values.push_back(i < row.size() && !row[i].is_null ? row[i].text : "");
  rec.display = key_show(rec.key_values);
  rec.values.reserve(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) {
    const size_t idx = keys.size() + i;
    const NormSpec& sp = source ? cols[i].src_spec : cols[i].tgt_spec;
    if (skip_blobs && sp.type == LogicalType::kBytes) {
      rec.values.push_back("\\N");
      continue;
    }
    const bool blob = sp.type == LogicalType::kBytes;
    const bool str_client =
        client_norm && (sp.type == LogicalType::kString || sp.type == LogicalType::kChar);
    if (idx < row.size() && !row[idx].is_null) {
      if (str_client)
        rec.values.push_back(canonical(Value::Str(row[idx].text), sp));
      else
        rec.values.push_back(blob ? "sha256:" + row[idx].text : row[idx].text);
    } else {
      rec.values.push_back("\\N");
    }
  }
  return rec;
}

}  // namespace

// Target side: materialize up to cap rows so matching keys can be looked up.
Status fetch_rows(SqlBackend& db, const std::string& table_q, const std::string& where,
                  const std::vector<ColPair>& keys, const std::vector<ColPair>& cols,
                  bool source, bool client_norm, bool skip_blobs, size_t cap,
                  std::map<std::string, RowRec>& out, bool& truncated, int retries,
                  int backoff_ms) {
  out.clear();
  truncated = false;
  const std::string sql =
      row_sql(db, table_q, where, keys, cols, source, client_norm, skip_blobs, cap);
  ResultSet rs;
  Status st;
  for (int attempt = 0;; ++attempt) {
    st = db.query(sql, rs);
    if (st.ok()) break;
    if (attempt >= retries) return st;
    std::fprintf(stderr, "[retry] row-diff %s %d/%d: %s\n", source ? "source" : "target",
                 attempt + 1, retries, st.msg.c_str());
    sleep_ms(backoff_ms * (attempt + 1));
  }
  if (rs.rows.size() > cap) {
    truncated = true;
    rs.rows.resize(cap);
  }
  for (const auto& row : rs.rows) {
    RowRec rec = make_rec(row, keys, cols, source, client_norm, skip_blobs);
    out.emplace(key_id(rec.key_values), std::move(rec));
  }
  return Status::OK();
}

// Source side: stream rows without materializing them; the caller compares
// each row against the target map and erases matches.
Status stream_rows(SqlBackend& db, const std::string& table_q, const std::string& where,
                   const std::vector<ColPair>& keys, const std::vector<ColPair>& cols,
                   bool source, bool client_norm, bool skip_blobs, size_t cap,
                   const std::function<bool(const std::string&, const RowRec&)>& fn,
                   bool& truncated, int retries, int backoff_ms) {
  truncated = false;
  const std::string sql =
      row_sql(db, table_q, where, keys, cols, source, client_norm, skip_blobs, cap);
  for (int attempt = 0;; ++attempt) {
    size_t delivered = 0;
    Status st = db.query_stream(sql, [&](const std::vector<Cell>& row) {
      if (delivered >= cap) {
        truncated = true;
        return false;
      }
      ++delivered;
      RowRec rec = make_rec(row, keys, cols, source, client_norm, skip_blobs);
      return fn(key_id(rec.key_values), rec);
    });
    if (st.ok() || delivered > 0 || attempt >= retries) return st;
    std::fprintf(stderr, "[retry] row-diff %s %d/%d: %s\n", source ? "source" : "target",
                 attempt + 1, retries, st.msg.c_str());
    sleep_ms(backoff_ms * (attempt + 1));
  }
}

void add_mismatch(std::vector<Mismatch>& out, int64_t limit, MismatchKind kind,
                  const std::string& key, const std::string& col, const std::string& sv,
                  const std::string& tv,
                  const std::vector<std::string>* key_values = nullptr);

std::string key_predicate(const std::vector<ColPair>& keys,
                          const std::vector<std::string>& vals, bool source, Dialect d) {
  auto lit = [&](size_t i) -> std::string {
    const NormSpec& sp = source ? keys[i].src_spec : keys[i].tgt_spec;
    if (sp.type == LogicalType::kInt) return vals[i];
    if (sp.type == LogicalType::kBytes) return key_literal(KeyValue::Hex(vals[i]), d);
    return quote_lit(vals[i], d);
  };
  if (keys.size() == 1)
    return (source ? keys[0].src_q : keys[0].tgt_q) + " = " + lit(0);
  std::string l = "(", r = "(";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i) { l += ","; r += ","; }
    l += source ? keys[i].src_q : keys[i].tgt_q;
    r += lit(i);
  }
  return l + ") = " + r + ")";
}

Status blob_length(SqlBackend& db, const std::string& table, const std::string& col,
                   const std::string& where, int64_t& len, bool& is_null) {
  ResultSet rs;
  Status st = db.query("SELECT CASE WHEN " + col + " IS NULL THEN NULL ELSE octet_length(" +
                           col + ") END FROM " + table + " WHERE " + where,
                       rs);
  if (!st.ok()) return st;
  if (rs.rows.empty()) return Status::Err(Code::kDb, "blob length returned no row");
  is_null = rs.rows[0].empty() || rs.rows[0][0].is_null;
  len = is_null ? 0 : (int64_t)std::strtoll(rs.rows[0][0].text.c_str(), nullptr, 10);
  return Status::OK();
}

Status blob_chunk(SqlBackend& db, const std::string& table, const std::string& col,
                  const std::string& where, int64_t off, int64_t n, std::string& hex) {
  const std::string sql =
      db.dialect() == Dialect::kPg
          ? "SELECT encode(substring(" + col + " from " + std::to_string((long long)off) +
                " for " + std::to_string((long long)n) + "), 'hex') FROM " + table +
                " WHERE " + where
          : "SELECT LOWER(HEX(SUBSTRING(" + col + ", " + std::to_string((long long)off) +
                ", " + std::to_string((long long)n) + "))) FROM " + table + " WHERE " + where;
  ResultSet rs;
  Status st = db.query(sql, rs);
  if (!st.ok()) return st;
  if (rs.rows.empty() || rs.rows[0].empty()) return Status::Err(Code::kDb, "blob chunk no row");
  hex = rs.rows[0][0].is_null ? "" : rs.rows[0][0].text;
  return Status::OK();
}

Status compare_blob_chunks(SqlBackend& a, SqlBackend& b, const std::string& ta,
                           const std::string& tb, const std::string& ca,
                           const std::string& cb, const std::string& wa,
                           const std::string& wb, int64_t chunk,
                           const std::vector<std::string>& key_values,
                           const std::string& display, const std::string& colname,
                           int64_t max_mismatches, std::vector<Mismatch>& out) {
  int64_t la = 0, lb = 0;
  bool na = false, nb = false;
  Status st = blob_length(a, ta, ca, wa, la, na);
  if (!st.ok()) return st;
  st = blob_length(b, tb, cb, wb, lb, nb);
  if (!st.ok()) return st;
  if (na != nb) {
    add_mismatch(out, max_mismatches, MismatchKind::kValue, display, colname,
                 na ? "NULL" : "not NULL", nb ? "NULL" : "not NULL", &key_values);
    return Status::OK();
  }
  if (na) return Status::OK();
  if (la != lb) {
    add_mismatch(out, max_mismatches, MismatchKind::kValue, display, colname,
                 "length " + std::to_string((long long)la),
                 "length " + std::to_string((long long)lb), &key_values);
    return Status::OK();
  }
  if (chunk <= 0) return Status::OK();
  for (int64_t off = 1; off <= la; off += chunk) {
    const int64_t n = std::min<int64_t>(chunk, la - off + 1);
    std::string ha, hb;
    st = blob_chunk(a, ta, ca, wa, off, n, ha);
    if (!st.ok()) return st;
    st = blob_chunk(b, tb, cb, wb, off, n, hb);
    if (!st.ok()) return st;
    if (ha != hb) {
      size_t i = 0;
      while (i < ha.size() && i < hb.size() && ha[i] == hb[i]) ++i;
      const int64_t byte_off = off + (int64_t)(i / 2);
      const size_t start = i > 24 ? i - 24 : 0;
      add_mismatch(out, max_mismatches, MismatchKind::kValue, display, colname,
                   "byte " + std::to_string((long long)byte_off) + ":" + ha.substr(start, 64),
                   "byte " + std::to_string((long long)byte_off) + ":" + hb.substr(start, 64),
                   &key_values);
      return Status::OK();
    }
  }
  return Status::OK();
}

void add_mismatch(std::vector<Mismatch>& out, int64_t limit, MismatchKind kind,
                  const std::string& key, const std::string& col, const std::string& sv,
                  const std::string& tv,
                  const std::vector<std::string>* key_values) {
  if ((int64_t)out.size() >= limit) return;
  Mismatch m;
  m.kind = kind;
  m.key = key;
  if (key_values) m.key_values = *key_values;
  m.column = col;
  m.source_value = sv;
  m.target_value = tv;
  out.push_back(std::move(m));
}

Status row_diff(SqlBackend& src, SqlBackend& tgt, const std::string& src_table_q,
                const std::string& tgt_table_q, const TupleRange& range,
                const std::vector<ColPair>& keys, const std::vector<ColPair>& cols,
                const std::string& extra_src, const std::string& extra_tgt,
                bool client_norm, int64_t blob_chunk_bytes, int64_t max_mismatches,
                int retries, int backoff_ms, std::vector<Mismatch>& out) {
  constexpr size_t kCap = 50000;
  std::vector<std::string> sq, tq;
  for (const auto& k : keys) {
    sq.push_back(k.src_q);
    tq.push_back(k.tgt_q);
  }
  std::string sw = range.where(sq, src.dialect(), false);
  std::string tw = range.where(tq, tgt.dialect(), true);
  if (!extra_src.empty()) sw += " AND (" + extra_src + ")";
  if (!extra_tgt.empty()) tw += " AND (" + extra_tgt + ")";
  const bool skip_blobs = blob_chunk_bytes > 0;
  const std::string s_tbl = table_ref(src_table_q, range, src.dialect(), false);
  const std::string t_tbl = table_ref(tgt_table_q, range, tgt.dialect(), true);

  // Materialize the target side, then stream the source side against it.
  std::map<std::string, RowRec> b;
  bool tb = false;
  Status st = fetch_rows(tgt, t_tbl, tw, keys, cols, false, client_norm, skip_blobs,
                         kCap, b, tb, retries, backoff_ms);
  if (!st.ok()) return st;
  if (tb) {
    add_mismatch(out, max_mismatches, MismatchKind::kChunk, "range", "",
                 "", "range too large");
    return Status::OK();
  }

  const size_t mark = out.size();
  bool ta = false;
  bool limit_hit = false;
  // BLOB chunks must be compared after the source stream ends: the stream
  // owns the source connection, so nested queries would corrupt it.
  struct BlobPair {
    std::vector<std::string> src_keys;
    std::vector<std::string> tgt_keys;
    std::string display;
  };
  std::vector<BlobPair> blob_pairs;
  st = stream_rows(src, s_tbl, sw, keys, cols, true, client_norm, skip_blobs, kCap,
                   [&](const std::string& kid, const RowRec& rec) {
                     if ((int64_t)out.size() >= max_mismatches) {
                       limit_hit = true;
                       return false;
                     }
                     auto it = b.find(kid);
                     if (it == b.end()) {
                       add_mismatch(out, max_mismatches, MismatchKind::kMissingRow,
                                    rec.display, "", "present", "missing",
                                    &rec.key_values);
                       return (int64_t)out.size() < max_mismatches;
                     }
                     for (size_t i = 0; i < cols.size() && (int64_t)out.size() < max_mismatches;
                          ++i) {
                       if (skip_blobs && (rec.values[i] == "\\N" ||
                                          it->second.values[i] == "\\N"))
                         continue;
                       if (rec.values[i] != it->second.values[i])
                         add_mismatch(out, max_mismatches, MismatchKind::kValue,
                                      rec.display, cols[i].name, rec.values[i],
                                      it->second.values[i], &rec.key_values);
                     }
                     if (blob_chunk_bytes > 0)
                       blob_pairs.push_back({rec.key_values, it->second.key_values,
                                             rec.display});
                     b.erase(it);
                     return (int64_t)out.size() < max_mismatches;
                   },
                   ta, retries, backoff_ms);
  if (!st.ok()) return st;
  if (ta) {
    out.resize(mark);
    add_mismatch(out, max_mismatches, MismatchKind::kChunk, "range", "",
                 "range too large", "");
    return Status::OK();
  }

  // Anything left in the target map has no matching source row.
  if (!limit_hit) {
    for (const auto& kv : b) {
      if ((int64_t)out.size() >= max_mismatches) break;
      add_mismatch(out, max_mismatches, MismatchKind::kExtraRow, kv.second.display, "",
                   "missing", "present", &kv.second.key_values);
    }
  }

  if (blob_chunk_bytes > 0) {
    for (const auto& p : blob_pairs) {
      for (size_t i = 0; i < cols.size() && (int64_t)out.size() < max_mismatches; ++i) {
        if (cols[i].src_spec.type != LogicalType::kBytes) continue;
        Status bs = compare_blob_chunks(
            src, tgt, s_tbl, t_tbl, cols[i].src_q, cols[i].tgt_q,
            key_predicate(keys, p.src_keys, true, src.dialect()),
            key_predicate(keys, p.tgt_keys, false, tgt.dialect()),
            blob_chunk_bytes, p.src_keys, p.display, cols[i].name, max_mismatches, out);
        if (!bs.ok()) return bs;
      }
    }
  }
  return Status::OK();
}

}  // namespace vdetail
}  // namespace mmv
