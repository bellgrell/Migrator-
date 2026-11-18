#include "engine.h"

#include "collation_profile.h"

#include "engine_digest.h"
#include "engine_session.h"

#include "cancel.h"
#include "chunk.h"
#include "dsn.h"
#include "engine_plan.h"
#include "engine_rowdiff.h"
#include "resume.h"
#include "schema.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace mmv {
namespace {

using namespace vdetail;

std::map<std::string, std::string> load_watermarks(const std::string& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    out[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return out;
}

bool save_watermarks(const std::string& path, const std::map<std::string, std::string>& m) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "# mmv-watermarks v1\n";
    for (const auto& kv : m) f << kv.first << '\t' << kv.second << '\n';
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string watermark_literal(const NormSpec& spec, const std::string& v, Dialect d) {
  const bool numeric = spec.type == LogicalType::kInt || spec.type == LogicalType::kDecimal ||
                       spec.type == LogicalType::kFloat;
  if (numeric && !v.empty() &&
      v.find_first_not_of("0123456789+-.eE") == std::string::npos)
    return v;
  return quote_lit(v, d);
}

}  // namespace

const char* mismatch_kind_name(MismatchKind k) {
  switch (k) {
    case MismatchKind::kValue: return "value";
    case MismatchKind::kMissingRow: return "missing_row";
    case MismatchKind::kExtraRow: return "extra_row";
    case MismatchKind::kChunk: return "chunk";
  }
  return "?";
}


std::map<std::string, std::vector<std::vector<std::string>>> load_keys_file(
    const std::string& path) {
  std::map<std::string, std::vector<std::vector<std::string>>> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const std::string table = line.substr(0, tab);
    const std::string rest = line.substr(tab + 1);
    std::vector<std::string> vals;
    std::string cur;
    for (char c : rest) {
      if (c == ',') { vals.push_back(cur); cur.clear(); }
      else cur += c;
    }
    vals.push_back(cur);
    out[table].push_back(std::move(vals));
  }
  return out;
}

std::string normalize_partition_name(std::string s, const std::string& table) {
  const size_t dot = s.rfind('.');
  if (dot != std::string::npos) s = s.substr(dot + 1);
  std::string o;
  for (unsigned char c : s)
    if (std::isalnum(c)) o += (char)std::tolower(c);
  std::string t;
  for (unsigned char c : table)
    if (std::isalnum(c)) t += (char)std::tolower(c);
  if (!t.empty() && o.rfind(t, 0) == 0) o = o.substr(t.size());
  return o;
}

Status json_overflow_column(SqlBackend& db, const std::string& table_q,
                            const std::string& where,
                            const std::vector<ColPair>& cols,
                            std::string& column, int64_t& bad) {
  for (const auto& p : cols) {
    if (p.tgt_spec.type != LogicalType::kJson) continue;
    const std::string expr = sql_canonical(p.tgt_q, p.tgt_spec, db.dialect());
    ResultSet rs;
    Status st = db.query("SELECT count(*) FROM " + table_q + " WHERE (" + where +
                             ") AND (" + expr + ") LIKE '%__MMV_JSON_OVF__%'",
                         rs);
    if (!st.ok()) return st;
    const int64_t n = rs.rows.empty()
                          ? 0
                          : (int64_t)std::strtoll(rs.rows[0][0].text.c_str(), nullptr, 10);
    if (n > 0) {
      column = p.name;
      bad = n;
      return Status::OK();
    }
  }
  return Status::OK();
}

std::string normalize_partition_method(std::string s) {
  std::string o;
  for (unsigned char c : s)
    if (std::isalnum(c)) o += (char)std::tolower(c);
  size_t p = 0;
  while ((p = o.find("columns", p)) != std::string::npos) o.erase(p, 7);
  return o;
}

Report run_verify(const VerifyOptions& opts) {
  Report rep;
  rep.partial = opts.partial;
  const auto t0 = std::chrono::steady_clock::now();

  const std::string wm_state_path =
      !opts.watermark_state.empty()
          ? opts.watermark_state
          : (!opts.resume_dir.empty() ? opts.resume_dir + "/watermarks.tsv"
                                      : ".mmv-watermarks.tsv");
  std::map<std::string, std::string> watermarks;
  if (opts.incremental) watermarks = load_watermarks(wm_state_path);

  std::map<std::string, std::vector<std::vector<std::string>>> changed_keys;
  if (!opts.keys_file.empty()) changed_keys = load_keys_file(opts.keys_file);

  Status st;
  auto src = connect_retry(opts.source_kind, opts.source_dsn, opts, st);
  if (!src) { rep.ok = false; rep.operational_error = true; rep.error = "source: " + st.msg; return rep; }
  auto tgt = connect_retry(opts.target_kind, opts.target_dsn, opts, st);
  if (!tgt) { rep.ok = false; rep.operational_error = true; rep.error = "target: " + st.msg; return rep; }
  rep.source_desc = src->describe();
  rep.target_desc = tgt->describe();

  // openGauss lacks sha256(bytea); both sides must use MD5 in that case.
  const bool og_mode = src->is_opengauss() || tgt->is_opengauss();
  set_opengauss_mode(og_mode);

  // Worker connections are reused across tables: connecting per table dominates
  // the runtime when a job has many small tables.
  std::mutex worker_cache_mu;
  std::vector<std::shared_ptr<SqlBackend>> worker_src_cache, worker_tgt_cache;
  auto cached_conn = [&](bool source, int idx, Status& st) -> std::shared_ptr<SqlBackend> {
    {
      std::lock_guard<std::mutex> lk(worker_cache_mu);
      auto& vec = source ? worker_src_cache : worker_tgt_cache;
      if ((size_t)idx < vec.size() && vec[idx]) return vec[idx];
    }
    auto conn = connect_retry(source ? opts.source_kind : opts.target_kind,
                              source ? opts.source_dsn : opts.target_dsn, opts, st);
    if (conn) {
      std::lock_guard<std::mutex> lk(worker_cache_mu);
      auto& vec = source ? worker_src_cache : worker_tgt_cache;
      if (vec.size() <= (size_t)idx) vec.resize((size_t)idx + 1);
      vec[idx] = conn;
    }
    return conn;
  };

  CollationProfile cprof;
  std::string cprof_err;
  if (!parse_collation_profile(opts.collation_profile, cprof, cprof_err)) {
    rep.ok = false;
    rep.operational_error = true;
    rep.error = cprof_err;
    return rep;
  }
  if (cprof.name != "binary") {
    std::string ce;
    Status cs = check_collation_profile(*src, *tgt, cprof, ce);
    if (!cs.ok()) {
      rep.ok = false;
      rep.operational_error = true;
      rep.error = "collation profile: " + ce;
      return rep;
    }
  }

  bool simple_partition =
      opts.source_kind == "og" || opts.source_kind == "opengauss" ||
      opts.target_kind == "og" || opts.target_kind == "opengauss";
  if (!simple_partition && src->dialect() == Dialect::kPg) {
    ResultSet prs;
    Status ps = src->query(
        "SELECT 1 FROM pg_proc WHERE proname = 'pg_partition_tree' LIMIT 1", prs);
    if (ps.ok() && prs.rows.empty()) simple_partition = true;
  }

  // --readonly keeps the coordinator connections in a read-only transaction
  // for the whole run, not just the per-worker snapshot window.
  TxGuard ro{src.get(), tgt.get()};
  if (opts.readonly && !opts.snapshot && !opts.snapshot_global) {
    Status rs = src->begin_readonly();
    if (!rs.ok()) {
      rep.ok = false; rep.operational_error = true;
      rep.error = "readonly source: " + rs.msg;
      return rep;
    }
    Status rt = tgt->begin_readonly();
    if (!rt.ok()) {
      src->end_readonly();
      rep.ok = false; rep.operational_error = true;
      rep.error = "readonly target: " + rt.msg;
      return rep;
    }
    ro.active = true;
  }
  if (opts.query_timeout > 0) {
    Status ts = src->set_timeout(opts.query_timeout);
    Status tt = tgt->set_timeout(opts.query_timeout);
    if (!ts.ok() || !tt.ok()) {
      rep.ok = false;
      rep.operational_error = true;
      rep.error = "query timeout: " + (ts.ok() ? tt.msg : ts.msg);
      return rep;
    }
  }

  std::string src_snap, tgt_snap;
  CoordGuard coord{src.get(), tgt.get()};
  if (opts.snapshot_global) {
    Dsn sd{opts.source_dsn}, td{opts.target_dsn};
    const bool same_mysql = src->dialect() == Dialect::kMySQL &&
                            tgt->dialect() == Dialect::kMySQL &&
                            sd.host() == td.host() && sd.port() == td.port();
    if (src->dialect() == Dialect::kPg) {
      Status st = src->begin_export_snapshot(src_snap);
      if (!st.ok()) {
        rep.ok = false;
        rep.operational_error = true;
        rep.error = "source snapshot: " + st.msg;
        return rep;
      }
      coord.s_active = true;
    } else if (opts.mysql_lock) {
      Status st = src->begin_global_read_lock();
      if (!st.ok()) {
        rep.ok = false;
        rep.operational_error = true;
        rep.error = "source mysql lock: " + st.msg;
        return rep;
      }
      coord.s_locked = true;
    } else if (!opts.assume_static) {
      rep.ok = false;
      rep.operational_error = true;
      rep.error = "MySQL source cannot export a snapshot; pass --mysql-lock or --assume-static";
      return rep;
    }
    if (tgt->dialect() == Dialect::kPg) {
      Status st = tgt->begin_export_snapshot(tgt_snap);
      if (!st.ok()) {
        rep.ok = false;
        rep.operational_error = true;
        rep.error = "target snapshot: " + st.msg;
        return rep;
      }
      coord.t_active = true;
    } else if (opts.mysql_lock && !same_mysql) {
      Status st = tgt->begin_global_read_lock();
      if (!st.ok()) {
        rep.ok = false;
        rep.operational_error = true;
        rep.error = "target mysql lock: " + st.msg;
        return rep;
      }
      coord.t_locked = true;
    } else if (!opts.mysql_lock && !opts.assume_static) {
      rep.ok = false;
      rep.operational_error = true;
      rep.error = "MySQL target cannot export a snapshot; pass --mysql-lock or --assume-static";
      return rep;
    }
  }

  std::vector<std::string> tables = opts.tables;
  if (opts.all_tables || tables.empty()) {
    Status ls = list_tables(*src, tables);
    if (!ls.ok()) { rep.ok = false; rep.operational_error = true; rep.error = ls.msg; return rep; }
  }
  if (!opts.exclude_tables.empty()) {
    std::set<std::string> ex;
    for (const auto& t : opts.exclude_tables) ex.insert(lower_ascii(t));
    tables.erase(std::remove_if(tables.begin(), tables.end(),
                                [&](const std::string& t) {
                                  return ex.count(lower_ascii(t)) != 0;
                                }),
                 tables.end());
  }

  for (const std::string& table : tables) {
    TableReport tr;
    tr.name = table;
    rep.tables.push_back(std::move(tr));
    TableReport& out = rep.tables.back();

    TableSchema ss, ts;
    Status e = load_schema(*src, table, ss);
    if (!e.ok()) { out.status = "error"; out.error = "source schema: " + e.msg; rep.ok = false; rep.operational_error = true; continue; }
    e = load_schema(*tgt, table, ts);
    if (!e.ok()) {
      if (opts.partial) {
        out.status = "skipped";
        out.note = "partial: target table missing: " + e.msg;
        continue;
      }
      out.status = "error"; out.error = "target schema: " + e.msg; rep.ok = false; rep.operational_error = true; continue;
    }

    for (auto& c : ss.columns) {
      for (const auto& ov : opts.column_overrides) {
        if (ov.target || ov.table != table ||
            lower_ascii(ov.column) != lower_ascii(c.name)) continue;
        c.spec.type = ov.type;
        c.spec.epsilon = ov.epsilon;
        c.spec.case_insensitive = ov.case_insensitive;
        c.spec.trim_space = ov.trim_space;
        c.spec.accent_insensitive = ov.accent_insensitive;
        c.spec.digest_bytes = ov.digest_bytes;
      }
    }
    for (auto& c : ts.columns) {
      for (const auto& ov : opts.column_overrides) {
        if (!ov.target || ov.table != table ||
            lower_ascii(ov.column) != lower_ascii(c.name)) continue;
        c.spec.type = ov.type;
        c.spec.epsilon = ov.epsilon;
        c.spec.case_insensitive = ov.case_insensitive;
        c.spec.trim_space = ov.trim_space;
        c.spec.accent_insensitive = ov.accent_insensitive;
        c.spec.digest_bytes = ov.digest_bytes;
      }
    }

    std::map<std::string, size_t> tmap;
    for (size_t i = 0; i < ts.columns.size(); ++i) tmap[lower_ascii(ts.columns[i].name)] = i;

    std::vector<ColPair> cols;
    std::vector<std::string> skipped_columns;
    const bool profile_mode = cprof.name != "binary";
    const std::string key_suffix = opts.collation_key_suffix;
    for (const auto& c : ss.columns) {
      if (profile_mode && !key_suffix.empty()) {
        const std::string ln = lower_ascii(c.name);
        const std::string ls = lower_ascii(key_suffix);
        if (ln.size() >= ls.size() && ln.compare(ln.size() - ls.size(), ls.size(), ls) == 0)
          continue;  // key column, compared through its source column
      }
      if (opts.fast && !opts.json_canon_suffix.empty()) {
        const std::string ln = lower_ascii(c.name);
        const std::string ls = lower_ascii(opts.json_canon_suffix);
        if (ln.size() >= ls.size() && ln.compare(ln.size() - ls.size(), ls.size(), ls) == 0)
          continue;  // precomputed JSON canonical column
      }
      if (c.generated) { skipped_columns.push_back(c.name + " (generated)"); continue; }
      if (opts.fast && c.spec.type == LogicalType::kJson && !opts.json_canon_suffix.empty()) {
        const ColumnInfo* sk = nullptr;
        const ColumnInfo* tk = nullptr;
        if (find_col(ss, c.name + opts.json_canon_suffix, &sk) &&
            find_col(ts, c.name + opts.json_canon_suffix, &tk)) {
          if ((sk->spec.type == LogicalType::kString || sk->spec.type == LogicalType::kBytes) &&
              (tk->spec.type == LogicalType::kString || tk->spec.type == LogicalType::kBytes)) {
            ColPair p;
            p.name = c.name;
            p.src_q = src->quote_ident(sk->name);
            p.tgt_q = tgt->quote_ident(tk->name);
            p.src_spec = sk->spec;
            p.tgt_spec = tk->spec;
            cols.push_back(std::move(p));
            continue;
          }
        }
      }
      if (profile_mode && (c.spec.type == LogicalType::kString ||
                           c.spec.type == LogicalType::kChar)) {
        if (src->dialect() == Dialect::kMySQL && tgt->dialect() == Dialect::kMySQL &&
            cprof.mysql_sql_key) {
          ColPair p;
          p.name = c.name;
          p.src_q = sql_collation_key(cprof, src->quote_ident(c.name), src->dialect());
          p.tgt_q = sql_collation_key(cprof, tgt->quote_ident(c.name), tgt->dialect());
          p.src_spec.type = LogicalType::kBytes;
          p.tgt_spec.type = LogicalType::kBytes;
          cols.push_back(std::move(p));
          continue;
        }
        const ColumnInfo* sk = nullptr;
        const ColumnInfo* tk = nullptr;
        if (!find_col(ss, c.name + key_suffix, &sk) ||
            !find_col(ts, c.name + key_suffix, &tk)) {
          out.status = "error";
          out.error = "collation profile '" + opts.collation_profile +
                      "' requires key column " + c.name + key_suffix;
          rep.ok = false;
          rep.operational_error = true;
          break;
        }
        if ((sk->spec.type != LogicalType::kBytes && sk->spec.type != LogicalType::kString) ||
            (tk->spec.type != LogicalType::kBytes && tk->spec.type != LogicalType::kString)) {
          out.status = "error";
          out.error = "collation key column has unsupported type: " + c.name + key_suffix;
          rep.ok = false;
          rep.operational_error = true;
          break;
        }
        ColPair p;
        p.name = c.name;
        p.src_q = src->quote_ident(sk->name);
        p.tgt_q = tgt->quote_ident(tk->name);
        p.src_spec = sk->spec;
        p.tgt_spec = tk->spec;
        cols.push_back(std::move(p));
        continue;
      }
      auto it = tmap.find(lower_ascii(c.name));
      if (it == tmap.end()) {
        if (opts.partial) { skipped_columns.push_back(c.name); continue; }
        out.status = "error";
        out.error = "column missing on target: " + c.name;
        rep.ok = false;
        rep.operational_error = true;
        break;
      }
      const auto& tc = ts.columns[it->second];
      ColPair p;
      p.name = c.name;
      p.src_q = src->quote_ident(c.name);
      p.tgt_q = tgt->quote_ident(tc.name);
      p.src_spec = c.spec;
      p.tgt_spec = tc.spec;
      if (!compatible(p.src_spec.type, p.tgt_spec.type, opts.allow_tz_mismatch)) {
        out.status = "error";
        out.error = "incompatible column type: " + c.name + " (" +
                    logical_name(p.src_spec.type) + " vs " +
                    logical_name(p.tgt_spec.type) + ")" +
                    (((p.src_spec.type == LogicalType::kTimestamp &&
                       p.tgt_spec.type == LogicalType::kTimestampTz) ||
                      (p.src_spec.type == LogicalType::kTimestampTz &&
                       p.tgt_spec.type == LogicalType::kTimestamp))
                         ? "; pass --allow-tz-mismatch if intended"
                         : "");
        rep.ok = false;
        rep.operational_error = true;
        break;
      }
      if (p.src_spec.type == LogicalType::kFloat) p.src_spec.epsilon = opts.epsilon;
      if (p.tgt_spec.type == LogicalType::kFloat) p.tgt_spec.epsilon = opts.epsilon;
      if (p.src_spec.type == LogicalType::kString || p.src_spec.type == LogicalType::kChar) {
        p.src_spec.case_insensitive = opts.case_insensitive;
        p.src_spec.trim_space = opts.trim_space || p.src_spec.type == LogicalType::kChar;
        p.src_spec.accent_insensitive = opts.accent_insensitive;
      }
      if (p.tgt_spec.type == LogicalType::kString || p.tgt_spec.type == LogicalType::kChar) {
        p.tgt_spec.case_insensitive = opts.case_insensitive;
        p.tgt_spec.trim_space = opts.trim_space || p.tgt_spec.type == LogicalType::kChar;
        p.tgt_spec.accent_insensitive = opts.accent_insensitive;
      }
      cols.push_back(std::move(p));
    }
    if (!out.error.empty()) continue;
    if (!skipped_columns.empty()) {
      out.note = "partial: skipped target-missing columns:";
      for (const auto& c : skipped_columns) out.note += " " + c;
    }
    if (cols.empty()) {
      out.status = "skipped";
      out.note = "partial: no comparable columns";
      continue;
    }

    std::vector<std::string> key_names;
    if (!opts.key.empty()) key_names = split_key_list(opts.key);
    else key_names = ss.pk;
    if (key_names.empty()) {
      // No primary key: compare one whole-table digest. Row-level localization
      // is impossible, but row count and content are still verified.
      Digest ds, dt;
      const int qret = (opts.snapshot || opts.snapshot_global) ? 0 : opts.retries;
      Status es = digest_range(*src, src->quote_ident(table), "", cols, true, ds,
                               qret, opts.retry_backoff_ms, false, og_mode);
      if (!es.ok()) {
        out.status = "error";
        out.error = "whole-table source digest: " + es.msg;
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      Status et = digest_range(*tgt, tgt->quote_ident(table), "", cols, false, dt,
                               qret, opts.retry_backoff_ms, false, og_mode);
      if (!et.ok()) {
        out.status = "error";
        out.error = "whole-table target digest: " + et.msg;
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      out.rows_source = ds.n;
      out.rows_target = dt.n;
      out.chunks = 1;
      out.completed_chunks = 1;
      if (ds == dt) {
        out.status = "ok";
        out.note = "no primary key: whole-table digest";
      } else {
        out.status = "mismatch";
        out.note = "no primary key: whole-table digest differs (no row localization)";
        rep.ok = false;
      }
      continue;
    }

    std::vector<ColPair> keys;
    bool key_ok = true;
    for (const auto& kn : key_names) {
      const ColumnInfo* sk = nullptr;
      const ColumnInfo* tk = nullptr;
      if (!find_col(ss, kn, &sk) || !find_col(ts, kn, &tk)) {
        out.status = "error";
        out.error = "key column not found: " + kn;
        rep.ok = false;
        key_ok = false;
        break;
      }
      ColPair p;
      p.name = sk->name;
      p.src_q = src->quote_ident(sk->name);
      p.tgt_q = tgt->quote_ident(tk->name);
      p.src_spec = sk->spec;
      p.tgt_spec = tk->spec;
      keys.push_back(std::move(p));
    }
    if (!key_ok) continue;

    std::vector<std::string> src_parts, tgt_parts;
    const bool leaf_parts = opts.partition_level == "leaf";
    if (!simple_partition &&
        (opts.parallel_unit == "partition" || opts.parallel_unit == "auto")) {
      Status ps = list_partitions(*src, table, leaf_parts, src_parts);
      Status pt = list_partitions(*tgt, table, leaf_parts, tgt_parts);
      bool ok = ps.ok() && pt.ok();
      if (!ok && opts.parallel_unit == "partition") {
        out.status = "error";
        out.error = "partition discovery: " + (ps.ok() ? pt.msg : ps.msg);
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      if (ok && leaf_parts && !src_parts.empty() && src_parts.size() != tgt_parts.size()) {
        int ds = 0, dt = 0;
        partition_depth(*src, table, ds);
        partition_depth(*tgt, table, dt);
        int level = std::max(ds, dt) - 1;
        bool found = false;
        for (; level >= 1; --level) {
          Status ps2 = list_partitions_level(*src, table, level, src_parts);
          Status pt2 = list_partitions_level(*tgt, table, level, tgt_parts);
          if (ps2.ok() && pt2.ok() && !src_parts.empty() &&
              src_parts.size() == tgt_parts.size()) {
            out.note = "leaf partition count mismatch; using level " +
                       std::to_string(level) + " partitions";
            found = true;
            break;
          }
        }
        if (!found) ok = false;
      }
      if (ok && !src_parts.empty() && src_parts.size() != tgt_parts.size()) ok = false;
      if (!ok) {
        if (opts.parallel_unit == "partition") {
          out.status = "error";
          out.error = "partition count mismatch or discovery failed";
          rep.ok = false;
          rep.operational_error = true;
          continue;
        }
        src_parts.clear();
        tgt_parts.clear();
      } else if (!src_parts.empty()) {
        std::map<std::string, std::string> by_name;
        for (const auto& t : tgt_parts)
          by_name[normalize_partition_name(t, table)] = t;
        std::vector<std::string> paired;
        bool all = true;
        for (const auto& sp : src_parts) {
          auto it = by_name.find(normalize_partition_name(sp, table));
          if (it == by_name.end()) { all = false; break; }
          paired.push_back(it->second);
        }
        if (all) tgt_parts = paired;
      }
    }
    const bool use_partitions =
        !src_parts.empty() && src_parts.size() == tgt_parts.size();
    if (use_partitions) {
      std::string ms, mt;
      Status sms = partition_method(*src, table, ms);
      Status smt = partition_method(*tgt, table, mt);
      if (sms.ok() && smt.ok() && !ms.empty() && !mt.empty() &&
          normalize_partition_method(ms) != normalize_partition_method(mt)) {
        out.status = "error";
        out.error = "partition method/expression mismatch: source " + ms +
                    ", target " + mt;
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
    }

    std::string inc_src, inc_tgt;
    const ColumnInfo* wm_s = nullptr;
    const ColumnInfo* wm_t = nullptr;
    if (opts.incremental) {
      if (!find_col(ss, opts.watermark_column, &wm_s) ||
          !find_col(ts, opts.watermark_column, &wm_t)) {
        out.status = "error";
        out.error = "watermark column not found: " + opts.watermark_column;
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      auto it = watermarks.find(table);
      if (it != watermarks.end() && !it->second.empty()) {
        inc_src = src->quote_ident(wm_s->name) + " > " +
                  watermark_literal(wm_s->spec, it->second, src->dialect());
        inc_tgt = tgt->quote_ident(wm_t->name) + " > " +
                  watermark_literal(wm_t->spec, it->second, tgt->dialect());
      }
    }

    TableStats stats;
    Status est = table_stats(*src, table, stats);
    if (!est.ok()) stats = TableStats{};

    bool src_idx = false, tgt_idx = false;
    Status si = key_indexed(*src, table, key_names, src_idx);
    Status ti = key_indexed(*tgt, table, key_names, tgt_idx);
    if (si.ok() && ti.ok()) {
      const int64_t est_rows = stats.rows;
      if ((!src_idx || !tgt_idx) && !opts.allow_seqscan && est_rows > 100000 &&
          !use_partitions) {
        const std::string side = !src_idx && !tgt_idx ? "both sides"
                                 : (!src_idx ? "source" : "target");
        out.status = "error";
        out.error = "key is not indexed on " + side +
                    "; refusing full scan at scale (index the key or pass --allow-seqscan)";
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      if (!opts.allow_seqscan && est_rows > 100000 &&
          keys[0].src_spec.type == LogicalType::kInt &&
          keys[0].tgt_spec.type == LogicalType::kInt) {
        bool su = true, tu = true;
        Status se = explain_uses_index(*src, src->quote_ident(table), keys[0].src_q, su);
        Status te = explain_uses_index(*tgt, tgt->quote_ident(table), keys[0].tgt_q, tu);
        if (se.ok() && te.ok() && (!su || !tu)) {
          const std::string side = !su && !tu ? "both sides" : (!su ? "source" : "target");
          out.status = "error";
          out.error = "key range does not use an index on " + side +
                      "; run ANALYZE, check partial/expression index, or pass --allow-seqscan";
          rep.ok = false;
          rep.operational_error = true;
          continue;
        }
      }
    }

    int64_t effective_rows = opts.chunk_rows;
    if (opts.chunk_bytes > 0 && stats.row_bytes > 0) {
      const int64_t by_bytes = std::max<int64_t>(1, opts.chunk_bytes / stats.row_bytes);
      if (by_bytes < effective_rows) effective_rows = by_bytes;
    }
    if (effective_rows < 1) effective_rows = 1;

    bool plan_bounded = false;
    int64_t plan_lo = 0, plan_hi = 0;
    const bool numeric_key = keys.size() == 1 && keys[0].src_spec.type == LogicalType::kInt &&
                             keys[0].tgt_spec.type == LogicalType::kInt;
    const std::string src_table_q = src->quote_ident(table);
    const std::string tgt_table_q = tgt->quote_ident(table);
    std::vector<TupleRange> ranges;

    if (use_partitions) {
      for (size_t pi = 0; pi < src_parts.size(); ++pi) {
        TupleRange whole;
        whole.partition = src_parts[pi];
        whole.partition_tgt = tgt_parts[pi];
        whole.partition_simple = simple_partition;
        const std::string stbl = table_ref(src_table_q, whole, src->dialect(), false);
        const std::string sw = whole.where({}, src->dialect(), false);
        if (!opts.allow_seqscan) {
          int64_t prows = 0;
          Status prs = partition_rows(*src, stbl, sw, prows);
          if (prs.ok() && prows >= opts.partition_index_min_rows) {
            bool uses = true;
            Status es = Status::OK();
            if (numeric_key) {
              es = explain_uses_index(*src, stbl, keys[0].src_q, uses, sw);
            } else {
              std::vector<std::string> probes;
              Status ks = sample_key_where(*src, stbl, keys, sw, 5, probes);
              uses = false;
              bool any = false;
              if (ks.ok()) {
                for (const auto& kw : probes) {
                  bool u = false;
                  Status e2 = explain_uses_where(*src, stbl, kw, u);
                  if (e2.ok()) {
                    any = true;
                    if (u) uses = true;
                  }
                }
              }
              if (!any) uses = true;
            }
            bool covered = false;
            Status mst = key_indexed(*src, table, key_names, covered);
            if (es.ok() && !uses && !(mst.ok() && covered)) {
              out.status = "error";
              out.error = "partition " + whole.partition +
                          " key range does not use an index; pass --allow-seqscan";
              rep.ok = false;
              rep.operational_error = true;
              break;
            }
          }
        }
        if (!out.error.empty()) break;
        if (opts.partition_subworkers > 1 && numeric_key) {
          Bounds pb;
          const std::string stbl = table_ref(src_table_q, whole, src->dialect(), false);
          const std::string sw = whole.where({keys[0].src_q}, src->dialect(), false);
          Status bs = partition_bounds(*src, stbl, keys[0].src_q, sw, pb);
          if (bs.ok() && pb.has) {
            int64_t hint = pb.rows > 0 ? pb.rows
                           : (stats.rows > 0
                                  ? std::max<int64_t>(1, stats.rows / (int64_t)src_parts.size())
                                  : 0);
            int64_t pchunk = effective_rows;
            if (hint > 0 && opts.partition_subworkers > 1) {
              pchunk = std::max<int64_t>(
                  1, (hint + opts.partition_subworkers - 1) / opts.partition_subworkers);
            }
            auto ks = plan_ranges(pb.min, pb.max, hint, pchunk);
            for (const auto& k : ks) {
              TupleRange r;
              r.partition = whole.partition;
              r.partition_tgt = whole.partition_tgt;
              r.partition_simple = simple_partition;
              r.has_lo = k.has_lo;
              r.has_hi = k.has_hi;
              if (k.has_lo) r.lo.push_back(KeyValue::Int(k.lo));
              if (k.has_hi) r.hi.push_back(KeyValue::Int(k.hi));
              ranges.push_back(std::move(r));
            }
            continue;
          }
        }
        if (opts.partition_subworkers > 1 && !numeric_key) {
          int64_t prows = 0;
          Status prs = partition_rows(*src, stbl, sw, prows);
          if (prs.ok() && prows > 0) {
            const int64_t chunk = std::max<int64_t>(1, prows / opts.partition_subworkers);
            std::vector<std::vector<KeyValue>> bounds;
            Status ss = sample_boundaries(*src, stbl, keys, chunk,
                                          opts.partition_subworkers + 1, bounds, sw);
            if (ss.ok() && !bounds.empty()) {
              auto subs = ranges_from_bounds(bounds);
              for (auto& sub : subs) {
                sub.partition = whole.partition;
                sub.partition_tgt = whole.partition_tgt;
                sub.partition_simple = simple_partition;
                ranges.push_back(std::move(sub));
              }
              continue;
            }
          }
        }
        ranges.push_back(std::move(whole));
      }
    } else if (numeric_key) {
      Bounds bs, bt;
      e = bounds_of(*src, src_table_q, keys[0].src_q, bs);
      if (!e.ok()) { out.status = "error"; out.error = "source bounds: " + e.msg; rep.ok = false; rep.operational_error = true; continue; }
      e = bounds_of(*tgt, tgt_table_q, keys[0].tgt_q, bt);
      if (!e.ok()) { out.status = "error"; out.error = "target bounds: " + e.msg; rep.ok = false; rep.operational_error = true; continue; }
      if (!bs.has && !bt.has) { out.status = "ok"; continue; }

      TupleRange window;
      bool have_lo = false, have_hi = false;
      int64_t lo = 0, hi = 0;
      auto take = [&](const Bounds& b) {
        if (!b.has) return;
        if (!have_lo || b.min < lo) { lo = b.min; have_lo = true; }
        if (!have_hi || b.max >= hi) { hi = b.max; have_hi = true; }
      };
      take(bs);
      take(bt);
      window.has_lo = have_lo;
      window.has_hi = have_hi;
      if (have_lo) window.lo.push_back(KeyValue::Int(lo));
      const bool max_edge = have_hi && hi == std::numeric_limits<int64_t>::max();
      if (have_hi && !max_edge) window.hi.push_back(KeyValue::Int(hi + 1));
      else window.has_hi = false;

      plan_bounded = window.has_lo && window.has_hi;
      plan_lo = lo;
      plan_hi = hi;
      if (window.has_lo && window.has_hi) {
        int64_t hint = stats.rows;
        if (hint <= 0) {
          const long double span = (long double)hi - (long double)lo + 1.0L;
          hint = (int64_t)(span / (long double)effective_rows);
          if (hint < 1) hint = 1;
        }
        ranges = numeric_ranges(plan_ranges(lo, hi, hint, effective_rows));
      } else {
        ranges.push_back(window);
      }
    } else {
      bool planned = false;
      const bool first_int = keys[0].src_spec.type == LogicalType::kInt &&
                             keys[0].tgt_spec.type == LogicalType::kInt;
      if (first_int) {
        Bounds b1, b2;
        Status e1 = bounds_of(*src, src_table_q, keys[0].src_q, b1);
        Status e2 = bounds_of(*tgt, tgt_table_q, keys[0].tgt_q, b2);
        if (e1.ok() && e2.ok() && (b1.has || b2.has)) {
          bool have_lo = false, have_hi = false;
          int64_t lo = 0, hi = 0;
          auto take = [&](const Bounds& b) {
            if (!b.has) return;
            if (!have_lo || b.min < lo) { lo = b.min; have_lo = true; }
            if (!have_hi || b.max >= hi) { hi = b.max; have_hi = true; }
          };
          take(b1);
          take(b2);
          int64_t hint = stats.rows;
          if (hint <= 0 && have_lo && have_hi) {
            const long double span = (long double)hi - (long double)lo + 1.0L;
            hint = (int64_t)(span / (long double)effective_rows);
            if (hint < 1) hint = 1;
          }
          if (have_lo && have_hi) {
            ranges = numeric_ranges(plan_ranges(lo, hi, hint, effective_rows));
          } else {
            TupleRange one;
            one.first_key_only = true;
            one.has_lo = have_lo;
            one.has_hi = have_hi;
            if (have_lo) one.lo.push_back(KeyValue::Int(lo));
            if (have_hi && hi != std::numeric_limits<int64_t>::max()) {
              one.hi.push_back(KeyValue::Int(hi + 1));
            } else {
              one.has_hi = false;
            }
            ranges.push_back(std::move(one));
          }
          for (auto& r : ranges) r.first_key_only = true;
          plan_bounded = have_lo && have_hi;
          plan_lo = lo;
          plan_hi = hi;
          planned = true;
        }
      }
      if (!planned) {
        if (stats.rows > 5000000 && !opts.allow_seqscan) {
          out.status = "error";
          out.error = "composite-key boundary planning scans the index; for >5M rows use a "
                      "single integer first key or pass --allow-seqscan";
          rep.ok = false;
          rep.operational_error = true;
          continue;
        }
        std::vector<std::vector<KeyValue>> bounds;
        e = sample_boundaries(*src, src_table_q, keys, effective_rows, 20000, bounds);
        if (!e.ok()) { out.status = "error"; out.error = "sample boundaries: " + e.msg; rep.ok = false; rep.operational_error = true; continue; }
        ranges = ranges_from_bounds(bounds);
      }
    }

    if (!opts.keys_file.empty()) {
      auto kit = changed_keys.find(table);
      if (kit == changed_keys.end()) { out.status = "ok"; continue; }
      ranges.clear();
      for (const auto& vals : kit->second) {
        if (vals.size() != keys.size()) {
          out.status = "error";
          out.error = "keys-file: wrong key arity for " + table;
          rep.ok = false;
          rep.operational_error = true;
          break;
        }
        TupleRange r;
        r.equality = true;
        r.has_lo = true;
        for (size_t i = 0; i < keys.size(); ++i)
          r.lo.push_back(parse_key(vals[i], keys[i].src_spec));
        ranges.push_back(std::move(r));
      }
      if (!out.error.empty()) continue;
      if (ranges.empty()) { out.status = "ok"; continue; }
    }

    std::string tok;
    if (!opts.resume_dir.empty()) {
      std::string src_tok, tgt_tok;
      src->table_change_token(table, src_tok);
      tgt->table_change_token(table, tgt_tok);
      if (!opts.assume_static && opts.resume_token.empty() && src_tok.empty() &&
          tgt_tok.empty()) {
        out.status = "error";
        out.error = "cannot establish a data-change token for resume; "
                    "pass --resume-token or --assume-static";
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
      tok = opts.resume_token;
      if (!opts.assume_static) tok += "|" + src_tok + "|" + tgt_tok;
    }

    std::string fp = "mmv-v1|" + table + "|" + src->describe() + "|" + tgt->describe();
    for (const auto& c : ss.columns) fp += "|s:" + c.name + ":" + logical_name(c.spec.type);
    for (const auto& c : ts.columns) fp += "|t:" + c.name + ":" + logical_name(c.spec.type);
    fp += "|k:";
    for (const auto& kn : key_names) fp += kn + ",";
    if (!src_parts.empty()) {
      fp += "|part:";
      for (const auto& p : src_parts) fp += p + ",";
      fp += "|parttgt:";
      for (const auto& p : tgt_parts) fp += p + ",";
    }
    fp += "|o:" + std::to_string(opts.chunk_rows) + "," + std::to_string(opts.chunk_bytes) +
          "," + std::to_string(opts.epsilon) + "," + (opts.case_insensitive ? "1" : "0") +
          (opts.trim_space ? "1" : "0") + (opts.row_mode ? "1" : "0");
    // Statistics (reltuples/row_bytes) are deliberately excluded: ANALYZE can
    // change them without changing a single row. The table change token plus
    // the planned bounds identify the data version instead.
    fp += "|ct:" + std::to_string(stats.change_token);
    fp += "|fpv:2";
    if (plan_bounded) fp += "|b:" + std::to_string(plan_lo) + "," + std::to_string(plan_hi);
    if (!opts.resume_dir.empty()) fp += "|tok:" + tok;

    ResumeConfig rc;
    rc.enabled = !opts.resume_dir.empty();
    rc.reset = opts.resume_reset;
    rc.gc = opts.resume_gc;
    rc.dir = opts.resume_dir;
    ResumeStore resume;
    {
      std::vector<TupleRange> planned = ranges;
      Status rr = resume.open(rc, table, fp, planned, ranges);
      if (!rr.ok()) {
        out.status = "error";
        out.error = "resume: " + rr.msg;
        rep.ok = false;
        rep.operational_error = true;
        continue;
      }
    }

    if (ranges.empty()) { out.status = "ok"; continue; }
    if (opts.sample_percent > 0 && opts.sample_percent < 100 &&
        opts.resume_dir.empty() && ranges.size() > 1) {
      const size_t total = ranges.size();
      const int pct = std::max(1, std::min(100, opts.sample_percent));
      const size_t step = std::max<size_t>(1, (size_t)(100 / pct));
      std::vector<TupleRange> sampled;
      for (size_t i = 0; i < ranges.size(); i += step) sampled.push_back(ranges[i]);
      if (!sampled.empty() && sampled.size() < total) {
        ranges.swap(sampled);
        out.note = "sampled " + std::to_string(pct) + "% of chunks (" +
                   std::to_string(ranges.size()) + "/" + std::to_string(total) + ")";
      }
    }
    out.chunks = (int64_t)ranges.size();
    const int64_t total_chunks = out.chunks;
    std::atomic<int64_t> completed_chunks{0};
    const auto table_start = std::chrono::steady_clock::now();

    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::vector<Mismatch> mismatches;
    std::vector<std::string> errors;
    int64_t failed = 0;
    int64_t resumed_chunks = 0;
    int64_t rows_src = 0, rows_tgt = 0;

    auto tick = [&](int64_t d) {
      if (!opts.progress || total_chunks <= 0) return;
      const int64_t step = std::max<int64_t>(1, total_chunks / 20);
      if (d != total_chunks && d % step != 0) return;
      std::lock_guard<std::mutex> lk(mu);
      const double sec = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - table_start).count();
      std::fprintf(stderr,
                   "[progress] %s %lld/%lld chunks (%.0f%%), rows %lld/%lld, %.1fs\n",
                   table.c_str(), (long long)d, (long long)total_chunks,
                   total_chunks ? 100.0 * (double)d / (double)total_chunks : 0.0,
                   (long long)rows_src, (long long)rows_tgt, sec);
    };

    const bool json_strict =
        opts.json_precision == "strict" && tgt->dialect() == Dialect::kMySQL;
    const int cores = (int)std::thread::hardware_concurrency();
    const int workers = choose_workers(opts.workers, ranges.size(), stats.rows, cores);
    if (!opts.quiet && opts.workers <= 0) {
      std::fprintf(stderr,
                   "[workers] %s auto=%d (cores=%d, chunks=%zu, rows~%lld)\n",
                   table.c_str(), workers, cores, ranges.size(),
                   (long long)stats.rows);
    }
    auto work = [&](int wid) {
      Status cs;
      auto s = cached_conn(true, wid, cs);
      if (!s) { std::lock_guard<std::mutex> lk(mu); errors.push_back("worker source: " + cs.msg); stop = true; return; }
      auto t = cached_conn(false, wid, cs);
      if (!t) { std::lock_guard<std::mutex> lk(mu); errors.push_back("worker target: " + cs.msg); stop = true; return; }

      if (opts.query_timeout > 0) {
        Status qs = s->set_timeout(opts.query_timeout);
        Status qt = t->set_timeout(opts.query_timeout);
        if (!qs.ok() || !qt.ok()) {
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("worker timeout: " + (qs.ok() ? qt.msg : qs.msg));
          stop = true;
          return;
        }
      }

      TxGuard tx{s.get(), t.get()};
      if (opts.readonly && !opts.snapshot && !opts.snapshot_global) {
        Status bs = s->begin_readonly();
        if (!bs.ok()) {
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("worker readonly source: " + bs.msg);
          stop = true;
          return;
        }
        Status bt = t->begin_readonly();
        if (!bt.ok()) {
          s->end_readonly();
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("worker readonly target: " + bt.msg);
          stop = true;
          return;
        }
        tx.active = true;
      } else if (opts.snapshot || opts.snapshot_global) {
        Status bs = (opts.snapshot_global && !src_snap.empty())
                        ? s->join_snapshot(src_snap)
                        : s->begin_readonly();
        if (!bs.ok()) {
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("source snapshot: " + bs.msg);
          stop = true;
          return;
        }
        Status bt = (opts.snapshot_global && !tgt_snap.empty())
                        ? t->join_snapshot(tgt_snap)
                        : t->begin_readonly();
        if (!bt.ok()) {
          s->end_readonly();
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("target snapshot: " + bt.msg);
          stop = true;
          return;
        }
        tx.active = true;
      }

      std::vector<std::string> src_keys, tgt_keys;
      for (const auto& k : keys) {
        src_keys.push_back(k.src_q);
        tgt_keys.push_back(k.tgt_q);
      }

      auto leaf = [&](const TupleRange& r) -> bool {
        std::vector<Mismatch> local;
        const int qret = (opts.snapshot || opts.snapshot_global) ? 0 : opts.retries;
        Status d = row_diff(*s, *t, src_table_q, tgt_table_q, r, keys, cols,
                            inc_src, inc_tgt, opts.client_normalize,
                            opts.blob_chunk_bytes, opts.max_mismatches, qret,
                            opts.retry_backoff_ms, local);
        std::lock_guard<std::mutex> lk(mu);
        if (!d.ok()) { errors.push_back("row diff: " + d.msg); stop = true; return true; }
        const bool bad = !local.empty();
        for (auto& m : local) {
          if ((int64_t)mismatches.size() >= opts.max_mismatches) break;
          mismatches.push_back(std::move(m));
        }
        return bad;
      };

      std::function<bool(const TupleRange&, int, int64_t&, int64_t&)> process;
      process = [&](const TupleRange& r, int depth, int64_t& n_src, int64_t& n_tgt) -> bool {
        if (mmv::g_interrupted) {
          stop = true;
          return true;
        }
        Digest ds, dt;
        const int qret = (opts.snapshot || opts.snapshot_global) ? 0 : opts.retries;
        const std::string sw =
            r.where(src_keys, s->dialect(), false) + (inc_src.empty() ? "" : " AND (" + inc_src + ")");
        Status es = digest_range(*s, table_ref(src_table_q, r, s->dialect(), false), sw, cols,
                                true, ds, qret, opts.retry_backoff_ms, false, og_mode);
        if (!es.ok()) { std::lock_guard<std::mutex> lk(mu); errors.push_back("source digest: " + es.msg); stop = true; return true; }
        const std::string tw =
            r.where(tgt_keys, t->dialect(), true) + (inc_tgt.empty() ? "" : " AND (" + inc_tgt + ")");
        Status et = digest_range(*t, table_ref(tgt_table_q, r, t->dialect(), true), tw, cols,
                                false, dt, qret, opts.retry_backoff_ms, json_strict, og_mode);
        if (!et.ok()) { std::lock_guard<std::mutex> lk(mu); errors.push_back("target digest: " + et.msg); stop = true; return true; }
        if (json_strict && dt.json_overflow > 0) {
          std::string col;
          int64_t bad = 0;
          Status qs = json_overflow_column(
              *t, table_ref(tgt_table_q, r, t->dialect(), true), tw, cols, col, bad);
          std::lock_guard<std::mutex> lk(mu);
          if (!qs.ok()) {
            errors.push_back("json precision check: " + qs.msg);
          } else {
            out.note = "use --json-precision off only if lossy storage is acceptable";
            errors.push_back(
                "json precision: " + std::to_string((long long)bad) +
                " row(s) in column " + col +
                " cannot be stored exactly by MySQL JSON; migrate to LONGTEXT "
                "canonical JSON or DECIMAL");
          }
          stop = true;
          return true;
        }
        if (ds.truncated || dt.truncated) {
          std::lock_guard<std::mutex> lk(mu);
          errors.push_back("row canonicalization exceeds 64MB on " +
                           std::string(ds.truncated && dt.truncated
                                           ? "both sides"
                                           : (ds.truncated ? "source" : "target")) +
                           "; MySQL GROUP_CONCAT may truncate; reduce row width or exclude large columns");
          stop = true;
          return true;
        }
        if (depth == 0) {
          std::lock_guard<std::mutex> lk(mu);
          rows_src += ds.n;
          rows_tgt += dt.n;
          n_src = ds.n;
          n_tgt = dt.n;
        }
        if (ds == dt) return false;
        if (opts.row_mode || depth >= 40) return leaf(r);

        TupleRange a, b;
        bool split = split_numeric(r, a, b);
        if (!split) {
          bool ok = false;
          Status ss = split_sampled(*s, src_table_q, r, keys, opts.max_split_rows, a, b, ok);
          if (!ss.ok()) { std::lock_guard<std::mutex> lk(mu); errors.push_back("split: " + ss.msg); stop = true; return true; }
          split = ok;
        }
        if (!split) return leaf(r);
        int64_t d1 = 0, d2 = 0, e1 = 0, e2 = 0;
        const bool bad_a = process(a, depth + 1, d1, d2);
        const bool bad_b = process(b, depth + 1, e1, e2);
        return bad_a || bad_b;
      };

      for (;;) {
        if (stop || mmv::g_interrupted) return;
        const size_t i = next.fetch_add(1);
        if (i >= ranges.size()) return;
        if (resume.done(i)) {
          int64_t ns = 0, nt = 0;
          resume.done_rows(i, ns, nt);
          {
            std::lock_guard<std::mutex> lk(mu);
            rows_src += ns;
            rows_tgt += nt;
            resumed_chunks++;
          }
          tick(completed_chunks.fetch_add(1) + 1);
          continue;
        }
        int64_t cs = 0, ct = 0;
        const bool bad = process(ranges[i], 0, cs, ct);
        resume.mark(i, !bad, cs, ct);
        if (bad) {
          std::lock_guard<std::mutex> lk(mu);
          failed++;
        }
        tick(completed_chunks.fetch_add(1) + 1);
      }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < workers; ++i) pool.emplace_back(work, i);
    for (auto& th : pool) th.join();

    out.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - table_start).count();
    out.rows_source = rows_src;
    out.rows_target = rows_tgt;
    out.approx_bytes_source = (int64_t)((long double)rows_src * (long double)stats.row_bytes);
    out.failed_chunks = failed;
    out.resumed_chunks = resumed_chunks;
    out.completed_chunks = completed_chunks.load();
    out.mismatches = std::move(mismatches);
    if (mmv::g_interrupted) {
      out.status = "cancelled";
      rep.cancelled = true;
      rep.ok = false;
    } else if (!errors.empty()) {
      out.status = "error";
      out.error = errors.front();
      rep.ok = false;
      rep.operational_error = true;
    } else if (out.mismatches.empty() && failed == 0) {
      out.status = "ok";
    } else {
      out.status = "mismatch";
      rep.ok = false;
    }
    if (opts.incremental && wm_s != nullptr && out.status == "ok") {
      ResultSet mr;
      Status ms = src->query(
          "SELECT MAX(" + src->quote_ident(wm_s->name) + ") FROM " + src_table_q, mr);
      if (ms.ok() && !mr.rows.empty() && !mr.rows[0][0].is_null)
        watermarks[table] = mr.rows[0][0].text;
    }
    rep.chunks += out.chunks;
    rep.completed_chunks += out.completed_chunks;
    rep.failed_chunks += out.failed_chunks;
    rep.resumed_chunks += out.resumed_chunks;
    rep.mismatches += (int64_t)out.mismatches.size();
  }

  const auto t1 = std::chrono::steady_clock::now();
  rep.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (opts.incremental) save_watermarks(wm_state_path, watermarks);
  return rep;
}

}  // namespace mmv
