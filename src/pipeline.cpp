#include "migrator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "backends.h"

namespace mig {
namespace {

struct TableJob {
  std::string name;
  TableSchema schema;
  std::string ddl;
  int64_t rows = 0;
  bool rebuild = false;
  std::string rebuild_sql;
};

std::string fmt_int(int64_t v) {
  std::string s = std::to_string(v);
  std::string out;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) {
    out.push_back(s[(size_t)i]);
    if (++c % 3 == 0 && i > 0) out.push_back(',');
  }
  return std::string(out.rbegin(), out.rend());
}

std::string file_timestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return std::string(buf);
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::shared_ptr<TableMetric> find_metric(Metrics& m, const std::string& name) {
  std::lock_guard<std::mutex> lk(m.mu);
  for (auto& t : m.tables) {
    if (t->name == name) return t;
  }
  return nullptr;
}

void export_log(Metrics& m, const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  const std::string path = dir + "/migration-" + file_timestamp() + ".log";
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    m.log("[warn] cannot write log file: " + path);
    return;
  }

  std::lock_guard<std::mutex> lk(m.mu);
  out << "mini-migrator migration log\n";
  out << "mode       : " << m.mode << "\n";
  out << "source     : " << m.source_desc << "\n";
  out << "target     : " << m.target_desc << "\n";
  out << "rows_done  : " << m.rows_done.load() << "\n";
  out << "rows_total : " << m.rows_total.load() << "\n";
  out << "errors     : " << m.errors.load() << "\n";
  out << "batches    : " << m.batches.load() << "\n";
  out << "bytes      : " << m.bytes_written.load() << "\n";
  out << "verify     : "
      << (m.verified.load() ? (m.verify_mismatch.load() == 0 ? "PASS" : "FAIL")
                            : "skipped")
      << "\n";
  out << "----------------------------------------\n";
  for (const auto& e : m.events) out << e << "\n";
  out.close();

  m.events.push_back("[export] log saved: " + path);
  while (m.events.size() > 500) m.events.pop_front();
}

bool is_int_type(const std::string& t) {
  return t.rfind("tinyint", 0) == 0 || t.rfind("smallint", 0) == 0 ||
         t.rfind("mediumint", 0) == 0 || t.rfind("int", 0) == 0 ||
         t.rfind("bigint", 0) == 0;
}

const ColumnDef* single_int_pk(const TableSchema& s) {
  if (s.primary_key.size() != 1) return nullptr;
  for (const auto& c : s.columns) {
    if (c.name == s.primary_key[0] && is_int_type(c.src_type)) return &c;
  }
  return nullptr;
}

const ColumnDef* single_pk(const TableSchema& s) {
  if (s.primary_key.size() != 1) return nullptr;
  for (const auto& c : s.columns) {
    if (c.name == s.primary_key[0]) return &c;
  }
  return nullptr;
}

std::string safe_name(const std::string& s) {
  std::string out;
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      out.push_back(c);
    else
      out.push_back('_');
  }
  return out;
}

std::string resume_file(const Options& o, const std::string& table) {
  return o.resume_dir + "/" + safe_name(table) + ".pos";
}

std::string read_checkpoint(const Options& o, const std::string& table) {
  std::ifstream in(resume_file(o, table), std::ios::binary);
  if (!in) return std::string();
  std::string v;
  std::getline(in, v);
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
    v.pop_back();
  return v;
}

void write_checkpoint(const Options& o, const std::string& table,
                      const std::string& v) {
  if (v.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(o.resume_dir, ec);
  std::ofstream out(resume_file(o, table), std::ios::trunc | std::ios::binary);
  if (out) out << v;
}

bool copy_table(const Options& o, const TableJob& job, ISource& src, ITarget& tgt,
                Metrics& m) {
  auto tmet = find_metric(m, job.name);
  if (!tmet) return false;

  tmet->state = 1;
  tmet->started_at = now_sec();
  m.log("=== table " + job.name + " (" + fmt_int(job.rows) + " rows, " +
        std::to_string(job.schema.indexes.size()) + " secondary indexes)");
  m.log("DDL for " + job.name + ":\n" + job.ddl);

  if (o.truncate_target) {
    Status ds = tgt.drop_table(job.name);
    if (!ds.ok()) m.log("[warn] drop_table " + job.name + ": " + ds.str());
  }
  Status st = tgt.create_table(job.schema, job.ddl);
  if (!st.ok()) {
    m.log("[error] create_table " + job.name + ": " + st.str());
    tmet->state = 3;
    m.errors += 1;
    tgt.rollback();
    return false;
  }

  const ColumnDef* pk = single_pk(job.schema);
  int pk_idx = -1;
  if (pk) {
    for (size_t i = 0; i < job.schema.columns.size(); ++i) {
      if (job.schema.columns[i].name == pk->name) { pk_idx = (int)i; break; }
    }
  }
  std::string after;
  if (o.resume && pk) after = read_checkpoint(o, job.name);
  if (o.resume && !pk)
    m.log("[warn] resume " + job.name + ": no single primary key, full copy");
  if (!after.empty()) {
    st = src.open_cursor_from(job.name, pk->name, after);
    if (!st.ok()) {
      m.log("[warn] resume " + job.name + ": " + st.str() + ", full copy");
      st = src.open_cursor(job.name);
    }
  } else {
    st = src.open_cursor(job.name);
  }
  if (!st.ok()) {
    m.log("[error] open_cursor " + job.name + ": " + st.str());
    tmet->state = 3;
    m.errors += 1;
    tgt.rollback();
    return false;
  }

  const int batch_size = o.batch_size > 0 ? o.batch_size : 1000;
  const int64_t total_batches =
      job.rows > 0 ? (job.rows + batch_size - 1) / batch_size : 0;
  const double t_start = now_sec();

  std::vector<Row> rows;
  int batch = 0;
  bool failed = false;
  std::string pending_pk;

  while (true) {
    rows.clear();
    st = src.fetch_batch(batch_size, rows);
    if (!st.ok()) {
      m.log("[error] fetch " + job.name + ": " + st.str());
      tmet->errors += 1;
      m.errors += 1;
      failed = true;
      break;
    }
    if (rows.empty()) break;

    st = tgt.write_batch(job.name, rows);
    if (!st.ok()) {
      m.log("[error] write " + job.name + ": " + st.str());
      tmet->errors += 1;
      m.errors += 1;
      failed = true;
      break;
    }

    if (pk_idx >= 0) pending_pk = rows.back()[(size_t)pk_idx].sql_literal();
    const int64_t n = (int64_t)rows.size();
    tmet->done += n;
    m.rows_done.fetch_add(n, std::memory_order_relaxed);
    m.batches.fetch_add(1, std::memory_order_relaxed);
    // 批内先本地累加，避免每个 Value 都打同一个原子 cache line
    int64_t bytes_local = 0;
    for (auto& r : rows) {
      for (auto& v : r) bytes_local += (int64_t)(v.s.size() + 12);
    }
    m.bytes_written.fetch_add(bytes_local, std::memory_order_relaxed);
    ++batch;

    if (o.commit_every > 0 && batch % o.commit_every == 0) {
      Status cs = tgt.commit();
      if (!cs.ok()) {
        m.log("[error] commit " + job.name + ": " + cs.str());
        tmet->errors += 1;
        m.errors += 1;
        failed = true;
        break;
      }
      if (o.resume && pk_idx >= 0) write_checkpoint(o, job.name, pending_pk);
    }
    if (batch % 10 == 0) m.sample();

    if (o.log_every > 0 && batch % o.log_every == 0) {
      const double el = now_sec() - t_start;
      const int64_t done = tmet->done.load();
      const double rps = el > 0.001 ? (double)done / el : 0.0;
      std::ostringstream line;
      line << "  batch " << batch << "/" << total_batches << "  rows "
           << fmt_int(done) << "/" << fmt_int(job.rows) << "  " << (int64_t)rps
           << " rows/s  "
           << (int)(100.0 * done / (job.rows ? job.rows : 1)) << "%";
      m.log(line.str());
    }

    if (o.throttle_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(o.throttle_ms));
    }
  }

  Status final_cs = tgt.commit();
  if (final_cs.ok() && o.resume && pk_idx >= 0)
    write_checkpoint(o, job.name, pending_pk);
  src.close_cursor();

  // 索引在数据全部写入后创建（批量导入快很多）
  if (!failed && o.build_indexes && !job.schema.indexes.empty()) {
    int built = 0;
    for (const auto& idx : job.schema.indexes) {
      Status is = tgt.exec_sql(tgt.index_ddl(job.name, idx));
      if (!is.ok()) {
        m.log("[warn] index " + idx.name + ": " + is.str());
      } else {
        ++built;
      }
    }
    m.log("indexes built for " + job.name + ": " + std::to_string(built) + "/" +
          std::to_string(job.schema.indexes.size()));
    // 索引 DDL 也在事务里，必须立即提交（worker 结束时不会自动提交）
    Status cs = tgt.commit();
    if (!cs.ok()) m.log("[warn] commit after indexes: " + cs.str());
  }

  if (failed) tgt.rollback();

  tmet->state = failed ? 3 : 2;
  tmet->finished_at = now_sec();
  m.log(std::string(failed ? "[fail] " : "ok ") + job.name + ": " +
        fmt_int(tmet->done.load()) + " rows");
  return !failed;
}

bool copy_table_ranges(const Options& o, const TableJob& job, ISource& src,
                       ITarget& tgt, Metrics& m) {
  if (o.resume) return copy_table(o, job, src, tgt, m);
  auto tmet = find_metric(m, job.name);
  if (!tmet) return false;

  const ColumnDef* pk = single_int_pk(job.schema);
  if (!pk) return copy_table(o, job, src, tgt, m);

  int64_t lo = 0, hi = 0;
  Status bs = src.pk_bounds(job.name, pk->name, lo, hi);
  if (!bs.ok()) {
    m.log("[warn] pk range " + job.name + ": " + bs.str() + " -> sequential");
    return copy_table(o, job, src, tgt, m);
  }

  const int parts = std::max(2, o.workers);
  tmet->state = 1;
  tmet->started_at = now_sec();
  m.log("=== table " + job.name + " (" + fmt_int(job.rows) +
        " rows) pk-range parallel: " + std::to_string(parts) + " parts [" +
        std::to_string(lo) + ", " + std::to_string(hi) + "] on " + pk->name);
  m.log("DDL for " + job.name + ":\n" + job.ddl);

  if (o.truncate_target) {
    Status ds = tgt.drop_table(job.name);
    if (!ds.ok()) m.log("[warn] drop_table " + job.name + ": " + ds.str());
  }
  Status st = tgt.create_table(job.schema, job.ddl);
  if (!st.ok()) {
    m.log("[error] create_table " + job.name + ": " + st.str());
    tmet->state = 3;
    m.errors += 1;
    tgt.rollback();
    return false;
  }
  // 建表必须提交，区间线程各自的新连接才能看到目标表
  st = tgt.commit();
  if (!st.ok()) {
    m.log("[error] commit after create " + job.name + ": " + st.str());
    tmet->state = 3;
    m.errors += 1;
    return false;
  }

  std::vector<int64_t> b((size_t)parts + 1);
  b[0] = lo;
  b[(size_t)parts] = hi;
  const long double span = (long double)hi - (long double)lo;
  for (int i = 1; i < parts; ++i) {
    b[(size_t)i] = lo + (int64_t)(span * i / parts);
    if (b[(size_t)i] <= b[(size_t)i - 1]) b[(size_t)i] = b[(size_t)i - 1] + 1;
  }
  for (int i = 1; i <= parts; ++i) {
    if (b[(size_t)i] < b[(size_t)i - 1]) b[(size_t)i] = b[(size_t)i - 1];
  }

  const int batch_size = o.batch_size > 0 ? o.batch_size : 1000;
  const double t_start = now_sec();
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int i = 0; i < parts; ++i) {
    const int64_t a = b[(size_t)i];
    const int64_t z = (i + 1 == parts) ? hi : b[(size_t)i + 1] - 1;
    if (z < a) continue;
    threads.emplace_back([&, i, a, z]() {
      Status s2;
      auto s = make_source(o, s2);
      auto g = make_target(o, s2);
      if (!s || !g) {
        m.log("[error] range " + std::to_string(i) + " init: " + s2.str());
        failures += 1;
        m.errors += 1;
        tmet->errors += 1;
        return;
      }
      if (!s->connect().ok() || !g->connect().ok()) {
        m.log("[error] range " + std::to_string(i) + " connect failed");
        failures += 1;
        m.errors += 1;
        tmet->errors += 1;
        return;
      }
      Status os = s->open_cursor_range(job.name, pk->name, a, z);
      if (!os.ok()) {
        m.log("[error] range " + std::to_string(i) + ": " + os.str());
        failures += 1;
        m.errors += 1;
        tmet->errors += 1;
        g->close();
        return;
      }

      std::vector<Row> rows;
      int batch = 0;
      bool failed = false;
      while (true) {
        rows.clear();
        Status fs = s->fetch_batch(batch_size, rows);
        if (!fs.ok()) {
          m.log("[error] fetch " + job.name + ": " + fs.str());
          failed = true;
          break;
        }
        if (rows.empty()) break;

        Status ws = g->write_batch(job.name, rows);
        if (!ws.ok()) {
          m.log("[error] write " + job.name + ": " + ws.str());
          failed = true;
          break;
        }

        const int64_t n = (int64_t)rows.size();
        tmet->done += n;
        m.rows_done.fetch_add(n, std::memory_order_relaxed);
        m.batches.fetch_add(1, std::memory_order_relaxed);
        int64_t bytes_local = 0;
        for (auto& r : rows) {
          for (auto& v : r) bytes_local += (int64_t)(v.s.size() + 12);
        }
        m.bytes_written.fetch_add(bytes_local, std::memory_order_relaxed);
        ++batch;

        if (o.commit_every > 0 && batch % o.commit_every == 0) {
          Status cs = g->commit();
          if (!cs.ok()) {
            m.log("[error] commit " + job.name + ": " + cs.str());
            failed = true;
            break;
          }
        }
        if (batch % 10 == 0) m.sample();
        if (o.throttle_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(o.throttle_ms));
        }
      }

      if (!failed) {
        Status cs = g->commit();
        if (!cs.ok()) {
          m.log("[error] final commit " + job.name + ": " + cs.str());
          failed = true;
        }
      } else {
        g->rollback();
      }
      s->close_cursor();
      g->close();
      if (failed) {
        failures += 1;
        m.errors += 1;
        tmet->errors += 1;
      }
    });
  }
  for (auto& t : threads) t.join();

  bool failed = failures.load() > 0;

  if (!failed && o.build_indexes && !job.schema.indexes.empty()) {
    int built = 0;
    for (const auto& idx : job.schema.indexes) {
      Status is = tgt.exec_sql(tgt.index_ddl(job.name, idx));
      if (!is.ok()) {
        m.log("[warn] index " + idx.name + ": " + is.str());
      } else {
        ++built;
      }
    }
    m.log("indexes built for " + job.name + ": " + std::to_string(built) + "/" +
          std::to_string(job.schema.indexes.size()));
    Status cs = tgt.commit();
    if (!cs.ok()) m.log("[warn] commit after indexes: " + cs.str());
  }

  tmet->state = failed ? 3 : 2;
  tmet->finished_at = now_sec();
  const double el = tmet->finished_at - t_start;
  m.log(std::string(failed ? "[fail] " : "ok ") + job.name + ": " +
        fmt_int(tmet->done.load()) + " rows in " + std::to_string((int)el) +
        "s (" + std::to_string(el > 0.001 ? (int64_t)(tmet->done.load() / el) : 0) +
        " rows/s)");
  return !failed;
}

bool prepare_jobs(ISource& src, const TypeMap& tm, const Options& o,
                  const std::unordered_map<std::string, TablePolicy>& policies,
                  Metrics& m, std::vector<TableJob>& jobs,
                  std::vector<TableJob>& rebuilds) {
  m.phase = "listing tables";
  std::vector<std::string> tables;
  Status st = src.list_tables(tables);
  if (!st.ok()) {
    m.log("[error] list_tables: " + st.str());
    return false;
  }

  int64_t total = 0;
  for (const auto& t : tables) {
    TablePolicy pol;
    auto it = policies.find(t);
    if (it != policies.end()) pol = it->second;

    if (pol.strategy == "skip") {
      m.log("[skip] table " + t);
      continue;
    }

    TableJob job;
    job.name = t;

    if (pol.strategy == "rebuild") {
      job.rebuild = true;
      job.rebuild_sql = pol.rebuild_sql;
      rebuilds.push_back(job);
      m.log("[rebuild] table " + t + " <- " +
            (pol.rebuild_sql.empty() ? "(no sql)" : pol.rebuild_sql));
      continue;
    }

    st = src.read_schema(t, job.schema);
    if (!st.ok()) {
      m.log("[error] read_schema " + t + ": " + st.str());
      m.errors += 1;
      continue;
    }
    for (auto& c : job.schema.columns) {
      c.dst_type = tm.map_column(job.schema.name, c.name, c.src_type);
      if (o.target_kind == "mysql") {
        if (c.dst_type == "varchar") c.dst_type = "varchar(255)";
        else if (c.dst_type == "char") c.dst_type = "char(1)";
        else if (c.dst_type == "decimal" || c.dst_type == "numeric")
          c.dst_type = "decimal(65,30)";
      }
    }
    job.ddl = (o.target_kind == "mysql") ? build_ddl_mysql(job.schema)
                                          : build_ddl(job.schema);
    job.rows = src.total_rows(t);
    if (job.rows < 0) job.rows = 0;
    total += job.rows;
    jobs.push_back(std::move(job));
  }

  // PostgreSQL 索引名在 schema 内唯一（MySQL 是每表唯一），跨表重名要加表名后缀
  if (o.target_kind != "mysql") {
    std::unordered_map<std::string, int> name_count;
    for (const auto& j : jobs) {
      for (const auto& idx : j.schema.indexes) name_count[idx.name] += 1;
    }
    for (auto& j : jobs) {
      for (auto& idx : j.schema.indexes) {
        if (name_count[idx.name] > 1) idx.name = idx.name + "_" + j.name;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(m.mu);
    m.tables.clear();
    for (auto& j : jobs) {
      auto t = std::make_shared<TableMetric>();
      t->name = j.name;
      t->total = j.rows;
      m.tables.push_back(t);
    }
    for (auto& j : rebuilds) {
      auto t = std::make_shared<TableMetric>();
      t->name = j.name;
      t->total = 0;
      t->state = 0;
      m.tables.push_back(t);
    }
  }
  m.rows_total = total;
  m.log("copy tables: " + std::to_string(jobs.size()) + ", rebuild: " +
        std::to_string(rebuilds.size()) + ", rows: " + fmt_int(total));
  return !jobs.empty() || !rebuilds.empty();
}

template <typename F>
Status connect_retry(const Options& o, Metrics& m, const char* what, F fn) {
  Status st;
  const int tries = o.connect_retries > 0 ? o.connect_retries : 0;
  for (int i = 0; i <= tries; ++i) {
    st = fn();
    if (st.ok()) return st;
    if (i == tries) break;
    m.log(std::string("[warn] ") + what + " connect failed: " + st.str() +
          ", retry " + std::to_string(i + 1) + "/" + std::to_string(tries));
    std::this_thread::sleep_for(std::chrono::milliseconds(
        (o.connect_backoff_ms > 0 ? o.connect_backoff_ms : 500) * (i + 1)));
  }
  return st;
}

void run_parallel(const Options& o, const std::vector<TableJob>& jobs, Metrics& m) {
  const int n = std::max(1, std::min<int>(o.workers, (int)jobs.size()));
  m.log("parallel mode: " + std::to_string(n) + " worker(s)");

  std::atomic<size_t> next{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(n);

  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&, i]() {
      Status st;
      auto s = make_source(o, st);
      auto g = make_target(o, st);
      if (!s || !g) {
        m.log("[error] worker " + std::to_string(i) + " init failed: " + st.str());
        failures += 1;
        return;
      }
      if (!s->connect().ok() || !g->connect().ok()) {
        m.log("[error] worker " + std::to_string(i) + " connect failed");
        failures += 1;
        return;
      }
      while (true) {
        const size_t idx = next.fetch_add(1);
        if (idx >= jobs.size()) break;
        if (!copy_table(o, jobs[idx], *s, *g, m)) failures += 1;
      }
      g->close();
    });
  }
  for (auto& t : threads) t.join();
}

}  // namespace

Status run_migration(const Options& o, const TypeMap& tm, ISource& src, ITarget& tgt,
                     Metrics& m) {
  m.started_at = now_sec();
  m.running = true;
  m.phase = "connecting";
  m.log("batch=" + std::to_string(o.batch_size) + " workers=" +
        std::to_string(o.workers) + " indexes=" +
        (o.build_indexes ? "on" : "off") + " analyze=" +
        (o.analyze_after ? "on" : "off"));

  Status st = connect_retry(o, m, "source", [&]() { return src.connect(); });
  if (!st.ok()) {
    m.log("[error] source connect: " + st.str());
    m.running = false;
    m.finished = true;
    m.finished_at = now_sec();
    m.phase = "error";
    return st;
  }
  if (o.consistent_snapshot) {
    std::string snap_file;
    uint64_t snap_pos = 0;
    Status ss = src.begin_snapshot(snap_file, snap_pos);
    if (ss.ok()) {
      m.log("consistent snapshot at " + snap_file + ":" + std::to_string(snap_pos));
      std::ofstream sp(o.snapshot_position_path, std::ios::trunc);
      sp << snap_file << ":" << snap_pos;
      std::ofstream cp(o.cdc_position_path, std::ios::trunc);
      cp << snap_file << ":" << snap_pos;
    } else {
      m.log("[warn] consistent snapshot: " + ss.str());
    }
  }

  st = connect_retry(o, m, "target", [&]() { return tgt.connect(); });
  if (!st.ok()) {
    m.log("[error] target connect: " + st.str());
    m.running = false;
    m.finished = true;
    m.finished_at = now_sec();
    m.phase = "error";
    return st;
  }

  // 连接后再取描述：目标端能报告实际是 PostgreSQL 还是 openGauss
  m.source_desc = src.describe();
  m.target_desc = tgt.describe();
  m.log("source: " + m.source_desc);
  m.log("target: " + m.target_desc);

  tgt.reset_counters();

  std::unordered_map<std::string, TablePolicy> policies;
  Status ps = load_table_policies(o.table_policy_path, policies);
  if (!ps.ok()) {
    m.log("[warn] table policy: " + ps.str());
  } else if (!policies.empty()) {
    m.log("table policies loaded: " + std::to_string(policies.size()));
  }

  std::vector<TableJob> jobs;
  std::vector<TableJob> rebuilds;
  if (!prepare_jobs(src, tm, o, policies, m, jobs, rebuilds)) {
    m.running = false;
    m.finished = true;
    m.finished_at = now_sec();
    m.phase = "error";
    return Status::Err(Code::kInvalid, "no tables to migrate");
  }

  if (!jobs.empty()) {
    int64_t total_rows = 0;
    for (const auto& j : jobs) total_rows += j.rows;

    std::vector<TableJob> plain;
    std::vector<TableJob> ranged;
    for (const auto& j : jobs) {
      const bool big = o.pk_parallel && o.workers > 1 &&
                       j.rows >= (int64_t)o.pk_min_rows &&
                       single_int_pk(j.schema) != nullptr &&
                       (jobs.size() == 1 || j.rows * 2 >= total_rows);
      (big ? ranged : plain).push_back(j);
    }

    if (!plain.empty()) {
      if (o.workers > 1 && plain.size() > 1) {
        run_parallel(o, plain, m);
      } else {
        for (const auto& job : plain) {
          m.phase = "copying " + job.name;
          copy_table(o, job, src, tgt, m);
        }
      }
    }
    for (const auto& job : ranged) {
      m.phase = "copying " + job.name + " (pk ranges)";
      copy_table_ranges(o, job, src, tgt, m);
    }
  }

  // rebuild 表：迁完基础表后，执行重建 SQL
  for (auto& rj : rebuilds) {
    auto tmet = find_metric(m, rj.name);
    if (tmet) tmet->state = 1;
    m.phase = "rebuilding " + rj.name;
    if (rj.rebuild_sql.empty()) {
      m.log("[warn] rebuild " + rj.name + ": no rebuild_sql configured");
      if (tmet) tmet->state = 2;
      continue;
    }
    const std::string sql = read_text(rj.rebuild_sql);
    if (sql.empty()) {
      m.log("[error] rebuild " + rj.name + ": cannot read " + rj.rebuild_sql);
      m.errors += 1;
      if (tmet) tmet->state = 3;
      continue;
    }
    Status rs = tgt.exec_sql(sql);
    if (rs.ok()) {
      m.log("rebuild ok: " + rj.name);
      if (tmet) tmet->state = 2;
    } else {
      m.log("[error] rebuild " + rj.name + ": " + rs.str());
      m.errors += 1;
      if (tmet) tmet->state = 3;
    }
  }

  // ANALYZE：更新统计信息，避免复杂查询走坏执行计划
  if (o.analyze_after && !jobs.empty()) {
    m.phase = "analyzing";
    int okc = 0;
    for (const auto& job : jobs) {
      Status as = tgt.exec_sql(tgt.analyze_sql(job.name));
      if (as.ok()) ++okc;
    }
    m.log("analyze done: " + std::to_string(okc) + "/" +
          std::to_string(jobs.size()) + " tables");
  }

  if (o.with_views && o.target_kind == "mysql") {
    m.log("[warn] views: MySQL target not supported yet, skipped");
  } else if (o.with_views) {
    std::vector<std::string> views;
    Status vs0 = src.list_views(views);
    if (!vs0.ok()) {
      m.log("[warn] list_views: " + vs0.str());
    } else if (!views.empty()) {
      m.phase = "migrating views";
      m.log("migrating " + std::to_string(views.size()) + " view(s)...");
      for (const auto& v : views) {
        std::string sql;
        Status rs = src.read_view_sql(v, sql);
        if (!rs.ok()) {
          m.errors += 1;
          m.log("[error] view " + v + ": " + rs.str());
          continue;
        }
        Status cs = tgt.exec_sql(convert_view_sql(sql));
        if (cs.ok()) {
          m.log("view ok: " + v);
        } else {
          m.errors += 1;
          m.log("[error] view " + v + ": " + cs.str());
        }
      }
    }
  }

  // 索引 / rebuild / ANALYZE 都在最后一个事务里，必须显式提交
  Status final_commit = tgt.commit();
  if (!final_commit.ok()) {
    m.log("[warn] final commit: " + final_commit.str());
  } else {
    m.log("final commit ok (DDL persisted)");
  }

  m.running = false;
  m.finished = true;
  m.finished_at = now_sec();
  m.phase = "done";
  m.sample();
  m.log("migration finished: " + fmt_int(m.rows_done.load()) + " rows, " +
        std::to_string(m.errors.load()) + " errors, " +
        std::to_string((int)(m.finished_at - m.started_at)) + "s");
  return Status::OK();
}

Status run_verify(const Options& o, ISource& src, ITarget& tgt, Metrics& m) {
  m.verifying = true;
  m.phase = "verifying";
  m.log("verifying row counts...");

  std::vector<std::shared_ptr<TableMetric>> snapshot;
  {
    std::lock_guard<std::mutex> lk(m.mu);
    snapshot = m.tables;
  }

  // --verify-only：没有迁移记录时自行枚举源表
  if (snapshot.empty()) {
    std::vector<std::string> tables;
    if (src.list_tables(tables).ok()) {
      for (const auto& t : tables) {
        auto tm = std::make_shared<TableMetric>();
        tm->name = t;
        tm->total = src.total_rows(t);
        snapshot.push_back(tm);
      }
    }
  }

  int64_t mismatch = 0;
  for (auto& t : snapshot) {
    if (t->total == 0) continue;  // rebuild / skip 表不参与行数校验
    const int64_t a = src.total_rows(t->name);
    const int64_t b = tgt.written(t->name);
    if (a != b) {
      ++mismatch;
      m.log("[mismatch] " + t->name + " source=" + fmt_int(a) + " target=" + fmt_int(b));
    } else {
      m.log("ok " + t->name + " rows=" + fmt_int(a));
    }
  }

  // ---- checksum：内容级校验 ----
  if (o.checksum && o.target_kind != "sim") {
    Status st;
    std::shared_ptr<ISource> dst;
    if (o.target_kind == "mysql") {
      Options vo = o;
      vo.mysql_source_dsn = o.target;
      dst = make_mysql_source(vo, st);
    } else {
      dst = make_pg_source(o, st);
    }
    if (!dst) {
      ++mismatch;
      m.log("[error] checksum: cannot create verifier");
    } else if (!dst->connect().ok()) {
      ++mismatch;
      m.log("[error] checksum: verifier connect failed");
    } else {
      m.log("computing checksums (source vs target)...");
      for (auto& t : snapshot) {
        if (t->total == 0) continue;
        Checksum a, b;
        const Status sa = compute_checksum(src, t->name, 2000, a);
        const Status sb = compute_checksum(*dst, t->name, 2000, b);
        if (!sa.ok() || !sb.ok()) {
          ++mismatch;
          m.log("[mismatch] checksum " + t->name + ": " +
                (sa.ok() ? sb.str() : sa.str()));
          continue;
        }
        if (checksum_equal(a, b)) {
          m.log("checksum ok " + t->name + " rows=" + fmt_int(a.rows));
        } else {
          ++mismatch;
          m.log("[mismatch] checksum " + t->name +
                " src(rows=" + fmt_int(a.rows) + ",sum=" + std::to_string(a.sum) +
                ",xor=" + std::to_string(a.xr) + ") dst(rows=" + fmt_int(b.rows) +
                ",sum=" + std::to_string(b.sum) + ",xor=" + std::to_string(b.xr) + ")");
        }
      }
      dst.reset();
    }
  }

  m.verify_mismatch = mismatch;
  m.verifying = false;
  m.verified = true;
  m.phase = mismatch == 0 ? "verified" : "verify_failed";
  m.log(mismatch == 0 ? "verify PASSED" : "verify FAILED");
  return mismatch == 0 ? Status::OK()
                       : Status::Err(Code::kInvalid, "row count mismatch");
}

void write_log_file(Metrics& m, const std::string& dir) {
  export_log(m, dir);
}

}  // namespace mig
