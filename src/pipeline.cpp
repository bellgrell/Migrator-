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
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "backends.h"
#include "checksum_normalizer.h"
#include "platform.h"
#include "io_scheduler.h"
#include "mmv_verify.h"
#include "type_system.h"
#include "job_scheduler.h"

namespace mig {
namespace {

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

Status reconnect_source(ISource& src, const Options& o, Metrics& m,
                        const std::string& table, const std::string& pk,
                        const std::string& after) {
  src.close_cursor();
  Status st;
  const int tries = o.connect_retries > 0 ? o.connect_retries : 0;
  for (int i = 0; i <= tries; ++i) {
    if (i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          (o.connect_backoff_ms > 0 ? o.connect_backoff_ms : 500) * i));
    }
    st = src.connect();
    if (!st.ok()) continue;
    st = after.empty() ? src.open_cursor(table)
                       : src.open_cursor_from(table, pk, after);
    if (st.ok()) {
      m.log("[warn] source reconnected: " + table +
            (after.empty() ? "" : " after " + after));
      return st;
    }
  }
  return st;
}

Status drop_target_table(ITarget& tgt, const Options& o, const std::string& table,
                           Metrics& m) {
  // End any open transaction before DDL; an idle-in-transaction connection is
  // exactly what makes DROP TABLE wait behind autovacuum.
  tgt.commit();
  if (o.target_kind == "pg" || o.target_kind == "og")
    tgt.exec_sql("SET lock_timeout = '5s'");
  else if (o.target_kind == "mysql")
    tgt.exec_sql("SET SESSION lock_wait_timeout = 5");
  Status st = tgt.drop_table(table);
  for (int attempt = 0; !st.ok() && attempt < 3; ++attempt) {
    m.log("[warn] drop_table " + table + " blocked: " + st.str() + "; retry " +
          std::to_string(attempt + 1) + "/3");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    st = tgt.drop_table(table);
  }
  return st;
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

  const ColumnDef* pk = single_pk(job.schema);
  int pk_idx = -1;
  if (pk) {
    for (size_t i = 0; i < job.schema.columns.size(); ++i) {
      if (job.schema.columns[i].name == pk->name) { pk_idx = (int)i; break; }
    }
  }
  if (o.truncate_target) {
    Status ds = drop_target_table(tgt, o, job.name, m);
    if (!ds.ok()) m.log("[warn] drop_table " + job.name + ": " + ds.str());
  }
  if (o.resume && !pk) {
    m.log("[warn] resume " + job.name + ": no primary key, resetting target table");
    tgt.drop_table(job.name);
  }
  Status st = tgt.create_table(job.schema, job.ddl);
  if (!st.ok()) {
    m.log("[error] create_table " + job.name + ": " + st.str());
    tmet->state = 3;
    m.errors += 1;
    tgt.rollback();
    return false;
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

  int batch_size = o.batch_size > 0 ? o.batch_size : 1000;
  bool has_binary = false;
  for (const auto& c : job.schema.columns) {
    if (c.binary) { has_binary = true; break; }
  }
  if (has_binary && batch_size > 256) {
    batch_size = 256;
    m.log("[info] binary table " + job.name + ": batch capped to 256");
  }
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
      if (pk_idx >= 0 && !pending_pk.empty()) {
        m.log("[warn] fetch failed, reconnecting source: " + st.str());
        st = reconnect_source(src, o, m, job.name, pk->name, pending_pk);
        if (st.ok()) continue;
      }
      m.log("[error] fetch " + job.name + ": " + st.str());
      tmet->errors += 1;
      m.errors += 1;
      failed = true;
      break;
    }
    if (rows.empty()) break;

    st = tgt.write_batch(job.name, rows);
    if (!st.ok()) {
      // 批次失败可能是“部分写入 + 断连”。回滚当前事务后切成幂等模式重试一次，
      // 避免重放时主键冲突，也避免把半批次算进 rows_done。
      m.log("[warn] write " + job.name + ": " + st.str() +
            "; rollback and retry idempotently");
      tgt.rollback();
      tgt.enable_idempotent();
      st = tgt.write_batch(job.name, rows);
    }
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
    m.add_rows(n);
    m.add_batch();
    // 批内先本地累加，避免每个 Value 都打同一个原子 cache line
    int64_t bytes_local = 0;
    for (auto& r : rows) {
      for (auto& v : r) bytes_local += (int64_t)(v.s.size() + 12);
    }
    m.add_bytes(bytes_local);
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
  if (!final_cs.ok()) {
    m.log("[error] final commit " + job.name + ": " + final_cs.str());
    tmet->errors += 1;
    m.errors += 1;
    failed = true;
  }
  if (!failed && final_cs.ok() && o.resume && pk_idx >= 0)
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

  std::string pk_name;
  if (!src.pk_range_column(job.schema, pk_name).ok())
    return copy_table(o, job, src, tgt, m);

  int64_t lo = 0, hi = 0;
  Status bs = src.pk_bounds(job.name, pk_name, lo, hi);
  if (!bs.ok()) {
    m.log("[warn] pk range " + job.name + ": " + bs.str() + " -> sequential");
    return copy_table(o, job, src, tgt, m);
  }

  const int parts = std::max(2, o.workers);
  tmet->state = 1;
  tmet->started_at = now_sec();
  m.log("=== table " + job.name + " (" + fmt_int(job.rows) +
        " rows) pk-range parallel: " + std::to_string(parts) + " parts [" +
        std::to_string(lo) + ", " + std::to_string(hi) + "] on " + pk_name);
  m.log("DDL for " + job.name + ":\n" + job.ddl);

  if (o.truncate_target) {
    Status ds = drop_target_table(tgt, o, job.name, m);
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

  int batch_size = o.batch_size > 0 ? o.batch_size : 1000;
  bool has_binary = false;
  for (const auto& c : job.schema.columns) {
    if (c.binary) { has_binary = true; break; }
  }
  if (has_binary && batch_size > 256) {
    batch_size = 256;
    m.log("[info] binary range " + job.name + ": batch capped to 256");
  }
  const double t_start = now_sec();
  std::atomic<int> failures{0};

  const IoPolicy sched_policy = parse_io_policy(o.io_scheduler);
  const int sched_depth = o.io_queue_depth > 0 ? o.io_queue_depth : parts;
  int sched_window = o.io_window > 0 ? o.io_window : parts;
  if (sched_policy == IoPolicy::kScan && o.io_window <= 0)
    sched_window = std::max(2, parts / 2);
  IoScheduler sched(parts, sched_depth, sched_window, sched_policy);

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
      Status os = s->open_cursor_range(job.name, pk_name, a, z);
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
      const bool use_pipeline =
          sched_policy != IoPolicy::kOff && o.io_prefetch > 0;
      std::atomic<bool> reader_failed{false};
      std::mutex err_mu;
      std::string reader_err;

      auto note_reader_error = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        if (reader_err.empty()) reader_err = e;
        reader_failed.store(true);
      };
      auto account = [&](const std::vector<Row>& got) {
        const int64_t n = (int64_t)got.size();
        tmet->done += n;
        m.add_rows(n);
        m.add_batch();
        int64_t bytes_local = 0;
        for (auto& r : got) {
          for (auto& v : r) bytes_local += (int64_t)(v.s.size() + 12);
        }
        m.add_bytes(bytes_local);
        ++batch;
      };

      if (!use_pipeline) {
        while (true) {
          rows.clear();
          if (!sched.begin(i)) {
            failed = true;
            break;
          }
          Status fs = s->fetch_batch(batch_size, rows);
          sched.end(i);
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
          account(rows);

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
      } else {
        BatchQueue q((size_t)o.io_prefetch);
        std::thread reader([&]() {
          std::vector<Row> buf;
          while (true) {
            if (!sched.begin(i)) break;
            buf.clear();
            Status fs = s->fetch_batch(batch_size, buf);
            sched.end(i);
            if (!fs.ok()) {
              note_reader_error("fetch " + job.name + ": " + fs.str());
              break;
            }
            if (buf.empty()) break;
            if (!q.push(std::move(buf))) break;
          }
          q.close();
          s->close_cursor();
          sched.finish(i);
        });

        while (q.pop(rows)) {
          Status ws = g->write_batch(job.name, rows);
          if (!ws.ok()) {
            m.log("[error] write " + job.name + ": " + ws.str());
            failed = true;
            break;
          }
          account(rows);

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

        if (!failed && reader_failed.load()) {
          std::lock_guard<std::mutex> lk(err_mu);
          m.log("[error] " + reader_err);
          failed = true;
        }
        if (failed) {
          q.close();
          sched.cancel(i);
        }
        reader.join();
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
      if (!use_pipeline) {
        s->close_cursor();
        sched.finish(i);
      }
      g->close();
      if (failed) {
        failures += 1;
        m.errors += 1;
        tmet->errors += 1;
      }
    });
  }
  for (auto& t : threads) t.join();

  if (sched_policy != IoPolicy::kOff) {
    m.log("io scheduler: policy=" + o.io_scheduler + " depth=" +
          std::to_string(sched_depth) + " prefetch=" +
          std::to_string(o.io_prefetch) + " peak_inflight=" +
          std::to_string(sched.peak_inflight()) + " waits=" +
          std::to_string(sched.wait_count()) + " avg_wait_ms=" +
          std::to_string(sched.avg_wait_ms()));
  }

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
  tgt.reset_names();  // 索引名等目标端注册表在每轮开始时清空
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
      if (o.source_kind == "mysql") {
        std::ofstream cp(o.cdc_position_path, std::ios::trunc);
        cp << snap_file << ":" << snap_pos;
      }
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
  if (!JobScheduler::prepare(src, tgt, tm, o, policies, m, jobs, rebuilds)) {
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
      const bool odbc_target =
          (o.target_kind == "mssql" || o.target_kind == "oracle");
      const bool big = o.pk_parallel && !odbc_target && o.workers > 1 &&
                       j.rows >= (int64_t)o.pk_min_rows &&
                       [&] { std::string c; return src.pk_range_column(j.schema, c).ok(); }() &&
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
    // Persist COPY/DDL before ANALYZE. If ANALYZE fails inside the same
    // transaction, PostgreSQL aborts it and the final COMMIT would silently
    // roll back the table we just loaded.
    tgt.commit();
    int okc = 0;
    for (const auto& job : jobs) {
      Status as = tgt.exec_sql(tgt.analyze_sql(job.name));
      if (as.ok()) {
        ++okc;
      } else {
        m.log("[warn] analyze " + job.name + ": " + as.str());
        tgt.rollback();
      }
    }
    tgt.commit();
    m.log("analyze done: " + std::to_string(okc) + "/" +
          std::to_string(jobs.size()) + " tables");
  }

  if (o.with_views) {
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
        Status cs = tgt.exec_sql(convert_view_sql(sql, o.source_kind, o.target_kind));
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

int acquire_verify_lock(const std::string& path, bool wait, std::string& err) {
#ifndef _WIN32
  const int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    err = "cannot open verify lock: " + path;
    return -1;
  }
  int rc = flock(fd, LOCK_EX | LOCK_NB);
  for (int i = 0; rc != 0 && wait && i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    rc = flock(fd, LOCK_EX | LOCK_NB);
  }
  if (rc != 0) {
    err = "another migration/verification is running (lock: " + path + ")";
    close(fd);
    return -1;
  }
  return fd;
#else
  (void)wait;
  static mig::InstanceLock win_verify_lock;
  if (!win_verify_lock.acquire(path, err)) return -1;
  return 0;
#endif
}

void write_verify_status(const std::string& path, const std::string& state,
                         int64_t pid, int code, const std::string& error,
                         const std::string& tables) {
  const std::string tmp = path + ".tmp";
  std::ofstream f(tmp, std::ios::trunc);
  if (!f) return;
  f << "state=" << state << "\n";
  f << "pid=" << (long long)pid << "\n";
  f << "exit_code=" << code << "\n";
  f << "tables=" << tables << "\n";
  std::string clean;
  for (char c : error) clean += (c == '\n' || c == '\r') ? ' ' : c;
  f << "error=" << clean << "\n";
  f.close();
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
}

Status spawn_async_verify(const Options& o, Metrics& m, int lock_fd) {
  std::vector<std::string> tables;
  {
    std::lock_guard<std::mutex> lk(m.mu);
    for (const auto& t : m.tables) {
      if (t && t->total > 0) tables.push_back(t->name);
    }
  }
  if (tables.empty()) return Status::Err(Code::kInvalid, "no tables to verify");
  std::string list;
  for (size_t i = 0; i < tables.size(); ++i) {
    if (i) list += ",";
    list += tables[i];
  }
  const std::string stamp = std::to_string((long long)std::time(nullptr));
  const std::string log = o.out_dir + "/verify-async-" + stamp + ".log";
  const std::string status = o.out_dir + "/verify-async-" + stamp + ".status";
  write_verify_status(status, "running", 0, -1, "", list);
  std::vector<std::string> args = {
      o.exe_path.empty() ? std::string("migrator") : o.exe_path,
      "--mode", o.mode,
      "--source", o.source,
      "--target", o.target,
      "--verify-only", "--verify-engine", "mmv", "--exit",
      "--only-tables", list,
      "--verify-status-file", status,
      "--connect-timeout", std::to_string(o.connect_timeout),
      "--connect-retries", std::to_string(o.connect_retries)};

#ifdef _WIN32
  std::string cmdline;
  const char q = '"';
  for (const auto& a : args) {
    if (!cmdline.empty()) cmdline += " ";
    cmdline += q;
    cmdline += a;
    cmdline += q;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE hlog = CreateFileA(log.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hlog;
  si.hStdError = hlog;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::vector<char> cmd(cmdline.begin(), cmdline.end());
  cmd.push_back(0);
  const BOOL started =
      CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                     DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                     nullptr, &si, &pi);
  if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
  if (!started) {
    return Status::Err(Code::kIO,
                       "CreateProcess failed: " +
                           std::to_string((unsigned long)GetLastError()));
  }
  write_verify_status(status, "running", (int64_t)pi.dwProcessId, -1, "", list);
  m.log("[verify] async started pid=" +
        std::to_string((unsigned long)pi.dwProcessId) + " log=" + log +
        " status=" + status);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return Status::OK();
#else
  const pid_t pid = fork();
  if (pid < 0) return Status::Err(Code::kIO, "fork failed");
  if (pid == 0) {
    setsid();
    const int fd = open(log.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > 2) close(fd);
    }
    std::vector<char*> av;
    av.reserve(args.size() + 1);
    for (auto& s : args) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
    _exit(127);
  }
  if (lock_fd >= 0) close(lock_fd);  // child inherited the flock
  m.log("[verify] async started pid=" + std::to_string((long long)pid) +
        " log=" + log + " status=" + status);
  return Status::OK();
#endif
}

std::string printable_value(const Value& v) {
  if (v.type == ValType::kString) return v.s;
  if (v.type == ValType::kBytes) {
    static const char* hex = "0123456789abcdef";
    std::string o = "0x";
    for (unsigned char c : v.s) {
      o += hex[c >> 4];
      o += hex[c & 0xF];
    }
    return o;
  }
  if (v.type == ValType::kNull) return "NULL";
  return v.json();
}

// 校验和不一致时按主键逐行列定位：给出表、键、列、两侧取值，
// 并写入 mmv_table_mismatches，repair 可以直接复用。
void localize_checksum_diff(ISource& a, ISource& b, const TableSchema& s,
                            Metrics& m) {
  if (s.primary_key.size() != 1) {
    m.log("[mismatch] " + s.name +
          ": no single primary key, column localization skipped");
    return;
  }
  const std::string pk = s.primary_key[0];
  int pk_idx = -1;
  for (size_t i = 0; i < s.columns.size(); ++i)
    if (s.columns[i].name == pk) { pk_idx = (int)i; break; }
  if (pk_idx < 0) return;

  Status sa = a.open_cursor_ordered(s.name, pk);
  Status sb = b.open_cursor_ordered(s.name, pk);
  if (!sa.ok() || !sb.ok()) {
    m.log("[mismatch] " + s.name + ": localization failed: " +
          (sa.ok() ? sb.str() : sa.str()));
    return;
  }

  const int kMax = 20;
  int shown = 0;
  while (shown < kMax) {
    std::vector<Row> ra, rb;
    Status fa = a.fetch_batch(1000, ra);
    Status fb = b.fetch_batch(1000, rb);
    if (!fa.ok() || !fb.ok()) break;
    if (ra.empty() || rb.empty()) break;
    const size_t n = std::min(ra.size(), rb.size());
    for (size_t i = 0; i < n && shown < kMax; ++i) {
      const std::string ka = printable_value(ra[i][(size_t)pk_idx]);
      const std::string kb = printable_value(rb[i][(size_t)pk_idx]);
      if (ka != kb) {
        m.log("[mismatch] " + s.name + ": key skew src=" + ka + " dst=" + kb);
        shown = kMax;
        break;
      }
      for (size_t j = 0; j < s.columns.size() && shown < kMax; ++j) {
        if (normalize_value(ra[i][j]) == normalize_value(rb[i][j])) continue;
        const std::string ca = printable_value(ra[i][j]);
        const std::string cb = printable_value(rb[i][j]);
        m.log("[mismatch] " + s.name + " key=" + ka + " column=" +
              s.columns[j].name +
              " src=" + (ca.size() > 60 ? ca.substr(0, 60) + "..." : ca) +
              " dst=" + (cb.size() > 60 ? cb.substr(0, 60) + "..." : cb));
        {
          std::lock_guard<std::mutex> lk(m.mu);
          RepairMismatch rm;
          rm.kind = "value";
          rm.key = ka;
          rm.column = s.columns[j].name;
          rm.source_value = ca;
          rm.target_value = cb;
          m.mmv_table_mismatches[s.name].push_back(std::move(rm));
        }
        ++shown;
      }
    }
    if (ra.size() < 1000 || rb.size() < 1000) break;
  }
  a.close_cursor();
  b.close_cursor();
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
  const bool verify_only = snapshot.empty();

  // --verify-only：没有迁移记录时自行枚举源表
  if (snapshot.empty()) {
    std::vector<std::string> tables;
    if (src.list_tables(tables).ok()) {
      if (!o.only_tables.empty()) {
        std::vector<std::string> filtered;
        for (const auto& t : tables) {
          for (const auto& n : o.only_tables) {
            if (n == t) { filtered.push_back(t); break; }
          }
        }
        tables.swap(filtered);
      }
      if (!o.exclude_tables.empty()) {
        std::vector<std::string> kept;
        for (const auto& t : tables) {
          bool skip = false;
          for (const auto& n : o.exclude_tables) {
            if (n == t) { skip = true; break; }
          }
          if (!skip) kept.push_back(t);
        }
        tables.swap(kept);
      }
      for (const auto& t : tables) {
        auto tm = std::make_shared<TableMetric>();
        tm->name = t;
        tm->total = src.total_rows(t);
        snapshot.push_back(tm);
      }
    }
  }

  // verify-only has no migration metrics: count target rows through a verifier
  // source instead of tgt.written().
  std::shared_ptr<ISource> count_dst;
  if (verify_only && o.target_kind != "sim") {
    Status cst;
    if (o.target_kind == "mysql") {
      Options vo = o;
      vo.mysql_source_dsn = o.target;
      count_dst = make_mysql_source(vo, cst);
#ifdef HAVE_ODBC
    } else if (o.target_kind == "mssql" || o.target_kind == "oracle") {
      Options vo = o;
      vo.source_kind = o.target_kind;
      vo.source = o.target;
      count_dst = make_odbc_source(vo, cst);
#endif
    } else {
      Options vo = o;
      vo.pg_source_uses_source = false;
      count_dst = make_pg_source(vo, cst);
    }
    if (count_dst && !count_dst->connect().ok()) count_dst.reset();
  }

  int64_t mismatch = 0;
  for (auto& t : snapshot) {
    if (t->total == 0) continue;  // rebuild / skip 表不参与行数校验
    const int64_t a = src.total_rows(t->name);
    int64_t b = 0;
    if (verify_only) {
      if (!count_dst) {
        ++mismatch;
        m.log("[error] verify-only: cannot open target verifier");
        continue;
      }
      b = count_dst->total_rows(t->name);
    } else {
      b = tgt.written(t->name);
    }
    if (a != b) {
      ++mismatch;
      m.log("[mismatch] " + t->name + " source=" + fmt_int(a) + " target=" + fmt_int(b));
    } else {
      m.log("ok " + t->name + " rows=" + fmt_int(a));
    }
  }

  // ---- checksum：内容级校验 ----
  if (o.checksum && o.target_kind != "sim" && o.verify_engine == "mmv") {
    m.log("mmv verify: chunked digest + localization");
    std::vector<std::string> mmv_tables;
    for (auto& t : snapshot) {
      if (t->total != 0) mmv_tables.push_back(t->name);
    }
    std::vector<MmvColumnOverride> overrides;
    {
      RuleSet type_rules;
      if (!o.type_rules_path.empty()) {
        std::string err;
        if (!type_rules.load_tsv(o.type_rules_path, err))
          m.log("[warn] type rules: " + err);
      }
      const std::string sd = (o.source_kind == "og") ? "pg" : o.source_kind;
      const std::string td = (o.target_kind == "og") ? "pg" : o.target_kind;
      for (const auto& table : mmv_tables) {
        TableSchema ts;
        if (!src.read_schema(table, ts).ok()) continue;
        for (const auto& c : ts.columns) {
          const auto rr = type_rules.resolve(sd, c.src_type, td);
          if (!rr.ok) continue;
          MmvColumnOverride ov;
          ov.table = table;
          ov.column = c.name;
          ov.family = RuleSet::family_name(rr.dst.family);
          ov.digest_bytes = rr.dst.family == TypeFamily::kBytes ||
                            rr.dst.family == TypeFamily::kBit ||
                            rr.dst.family == TypeFamily::kGeometry;
          ov.target = false;
          overrides.push_back(ov);
          ov.target = true;
          overrides.push_back(ov);
        }
      }
    }
    const MmvVerifyResult vr = run_mmv_verify(o, mmv_tables, overrides);
    {
      std::lock_guard<std::mutex> lk(m.mu);
      m.mmv_verified = true;
      m.mmv_mismatches = vr.mismatches;
      m.mmv_failed_tables.clear();
      m.mmv_table_mismatches.clear();
      for (const auto& tr : vr.tables) {
        if (!tr.ok) m.mmv_failed_tables.push_back(tr.table);
        for (const auto& mm : tr.diffs) {
          RepairMismatch rm;
          rm.kind = mm.kind;
          rm.key = mm.key;
          rm.column = mm.column;
          rm.source_value = mm.source_value;
          rm.target_value = mm.target_value;
          rm.key_values = mm.key_values;
          m.mmv_table_mismatches[tr.table].push_back(std::move(rm));
        }
      }
    }
    for (const auto& tr : vr.tables) {
      if (tr.ok) {
        m.log("mmv ok " + tr.table);
      } else {
        ++mismatch;
        m.log("[mismatch] mmv " + tr.table + ": " + tr.detail);
      }
    }
    if (vr.tables.empty() && !vr.ok) {
      ++mismatch;
      m.log("[error] mmv: " + vr.detail);
    }
  } else if (o.checksum && o.target_kind != "sim") {
    Status st;
    std::shared_ptr<ISource> dst;
    if (o.target_kind == "mysql") {
      Options vo = o;
      vo.mysql_source_dsn = o.target;
      dst = make_mysql_source(vo, st);
#ifdef HAVE_ODBC
    } else if (o.target_kind == "mssql" || o.target_kind == "oracle") {
      Options vo = o;
      vo.source_kind = o.target_kind;
      vo.source = o.target;
      dst = make_odbc_source(vo, st);
#endif
    } else {
      Options vo = o;
      vo.pg_source_uses_source = false;
      dst = make_pg_source(vo, st);
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
        int cbatch = 2000;
        TableSchema schema;
        if (src.read_schema(t->name, schema).ok()) {
          for (const auto& c : schema.columns) {
            if (c.binary || c.src_type.find("blob") != std::string::npos ||
                c.src_type.find("binary") != std::string::npos) {
              cbatch = 8;
              break;
            }
          }
        }
        Checksum a, b;
        const std::string pk_col =
            schema.primary_key.empty() ? std::string() : schema.primary_key[0];
        const Status sa = compute_checksum(src, t->name, cbatch, a,
                                           (std::size_t)o.checksum_batch_bytes,
                                           (std::size_t)o.checksum_max_rows, pk_col);
        const Status sb = compute_checksum(*dst, t->name, cbatch, b,
                                           (std::size_t)o.checksum_batch_bytes,
                                           (std::size_t)o.checksum_max_rows, pk_col);
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
          localize_checksum_diff(src, *dst, schema, m);
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
  if (mismatch == 0) return Status::OK();
  return Status::Err(Code::kInvalid,
                     "verification failed: " + std::to_string(mismatch) +
                         " table(s) differ (see verify log)");
}

namespace {

std::string hex_lower(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(s.size() * 2);
  for (unsigned char c : s) {
    o.push_back(d[c >> 4]);
    o.push_back(d[c & 0xF]);
  }
  return o;
}

std::string encode_key_values(const std::vector<std::string>& vals) {
  std::string o;
  for (const auto& s : vals) o += std::to_string(s.size()) + ":" + s + "|";
  return o;
}

std::string repair_key_text(const Row& r, const std::vector<int>& idx) {
  std::vector<std::string> vals;
  vals.reserve(idx.size());
  for (int i : idx) {
    if ((size_t)i >= r.size()) {
      vals.emplace_back();
      continue;
    }
    const Value& v = r[(size_t)i];
    if (v.type == ValType::kNull) vals.emplace_back();
    else if (v.type == ValType::kInt) vals.push_back(std::to_string((long long)v.i));
    else if (v.type == ValType::kBool) vals.push_back(v.b ? "1" : "0");
    else if (v.type == ValType::kBytes) vals.push_back(hex_lower(v.s));
    else vals.push_back(v.s);
  }
  return encode_key_values(vals);
}

bool row_values_equal(const Row& a, const Row& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const Value& x = a[i];
    const Value& y = b[i];
    if (x.type != y.type) {
      if (x.sql_literal() != y.sql_literal()) return false;
      continue;
    }
    switch (x.type) {
      case ValType::kNull:
        break;
      case ValType::kInt:
        if (x.i != y.i) return false;
        break;
      case ValType::kDouble:
        if (x.d != y.d) return false;
        break;
      case ValType::kBool:
        if (x.b != y.b) return false;
        break;
      default:
        if (x.s != y.s) return false;
        break;
    }
  }
  return true;
}

}  // namespace

Status run_repair(const Options& o, ISource& src, ITarget& tgt, Metrics& m) {
#ifndef MIG_ENABLE_REPAIR
  (void)o; (void)src; (void)tgt; (void)m;
  return Status::Err(Code::kUnsupported, "repair module not built");
#else
  if (o.target_kind == "sim") return Status::Err(Code::kUnsupported, "repair not supported for sim");
  Status st;
  std::shared_ptr<ISource> dst;
  if (o.target_kind == "mysql") {
    Options vo = o;
    vo.mysql_source_dsn = o.target;
    dst = make_mysql_source(vo, st);
#ifdef HAVE_ODBC
  } else if (o.target_kind == "mssql" || o.target_kind == "oracle") {
    Options vo = o;
    vo.source_kind = o.target_kind;
    vo.source = o.target;
    dst = make_odbc_source(vo, st);
#endif
  } else {
    Options vo = o;
    vo.pg_source_uses_source = false;
    dst = make_pg_source(vo, st);
  }
  if (!dst || !dst->connect().ok())
    return Status::Err(Code::kDb, "repair: target verifier connect failed");

  std::vector<std::shared_ptr<TableMetric>> snapshot;
  {
    std::lock_guard<std::mutex> lk(m.mu);
    snapshot = m.tables;
  }
  if (snapshot.empty()) {
    std::vector<std::string> names;
    if (src.list_tables(names).ok()) {
      for (auto& name : names) {
        auto tm = std::make_shared<TableMetric>();
        tm->name = name;
        tm->total = src.total_rows(name);
        snapshot.push_back(tm);
      }
    }
  }

  // 只修复目标端实际存在的表；否则 DECLARE 失败会把 PG 事务打成 aborted，
  // 后续表（包括真正需要修复的表）都会被跳过。
  std::vector<std::string> target_tables;
  if (dst->list_tables(target_tables).ok() && !target_tables.empty()) {
    std::unordered_set<std::string> present(target_tables.begin(),
                                            target_tables.end());
    snapshot.erase(
        std::remove_if(snapshot.begin(), snapshot.end(),
                       [&](const std::shared_ptr<TableMetric>& t) {
                         return present.count(t->name) == 0;
                       }),
        snapshot.end());
  }

  // mmv already told us which tables differ; do not rescan healthy tables.
  if (o.verify_engine == "mmv" && m.mmv_verified.load()) {
    std::vector<std::string> failed;
    {
      std::lock_guard<std::mutex> lk(m.mu);
      failed = m.mmv_failed_tables;
    }
    if (failed.empty()) {
      m.log("repair: mmv found no mismatches; nothing to repair");
      return Status::OK();
    }
    std::unordered_set<std::string> want(failed.begin(), failed.end());
    snapshot.erase(
        std::remove_if(snapshot.begin(), snapshot.end(),
                       [&](const std::shared_ptr<TableMetric>& t) {
                         return want.count(t->name) == 0;
                       }),
        snapshot.end());
    m.log("repair: mmv targeted " + std::to_string(snapshot.size()) +
          " table(s), " + std::to_string(m.mmv_mismatches.load()) + " mismatch(es)");
  }

  int repaired = 0;
  for (auto& t : snapshot) {
    if (!t || t->total <= 0) continue;
    TableSchema s;
    if (!src.read_schema(t->name, s).ok()) continue;
    if (s.primary_key.empty()) {
      m.log("[repair] skip " + t->name + ": no primary key");
      continue;
    }
    int pk_idx = -1;
    if (s.primary_key.size() == 1) {
      for (size_t i = 0; i < s.columns.size(); ++i)
        if (s.columns[i].name == s.primary_key[0]) { pk_idx = (int)i; break; }
      if (pk_idx < 0) continue;
    }

    // 整数主键：双游标按主键排序流式归并，内存 O(batch)；
    // 其他主键类型退回下面的集合比较（保持兼容）。
    if (single_int_pk(s) != nullptr) {
      Status oa = src.open_cursor_ordered(t->name, s.primary_key[0]);
      Status ob = dst->open_cursor_ordered(t->name, s.primary_key[0]);
      if (oa.ok() && ob.ok()) {
        std::vector<Row> ra, rb, wbuf;
        std::vector<Row> vbuf;
        std::vector<std::string> vkeys;
        size_t ia = 0, ib = 0;
        auto refill = [](ISource& s2, std::vector<Row>& v, size_t& i) -> bool {
          if (i < v.size()) return true;
          v.clear();
          i = 0;
          return s2.fetch_batch(2000, v).ok() && !v.empty();
        };
        auto cmp_pk = [](const Value& x, const Value& y) -> int {
          if (x.type == ValType::kInt && y.type == ValType::kInt)
            return x.i < y.i ? -1 : (x.i > y.i ? 1 : 0);
          const std::string a2 = x.sql_literal();
          const std::string b2 = y.sql_literal();
          return a2.compare(b2);
        };
        bool ha = refill(src, ra, ia);
        bool hb = refill(*dst, rb, ib);
        int64_t missing = 0, extra = 0, changed = 0;
        std::vector<std::string> dbuf;
        bool repair_failed = false;
        auto flush_w = [&]() {
          if (!wbuf.empty()) {
            Status ws = tgt.write_batch(t->name, wbuf);
            if (!ws.ok()) {
              m.log("[error] repair write " + t->name + ": " + ws.str());
              repair_failed = true;
            }
            // COPY is still active here; end it before any DELETE runs.
            Status cs = tgt.commit();
            if (!cs.ok()) {
              m.log("[error] repair write-commit " + t->name + ": " + cs.str());
              repair_failed = true;
            }
            wbuf.clear();
          }
        };
        auto flush_d = [&]() {
          for (auto& lit : dbuf) {
            Status ds = tgt.exec_sql(tgt.delete_sql(t->name, s.primary_key[0], lit));
            if (!ds.ok()) {
              m.log("[error] repair delete " + t->name + ": " + ds.str());
              repair_failed = true;
            }
          }
          if (!dbuf.empty()) {
            Status cs = tgt.commit();
            if (!cs.ok()) {
              m.log("[error] repair commit " + t->name + ": " + cs.str());
              repair_failed = true;
            }
          }
          dbuf.clear();
        };
        auto flush_v = [&]() {
          if (vkeys.empty()) return;
          for (auto& lit : vkeys) {
            Status ds = tgt.exec_sql(tgt.delete_sql(t->name, s.primary_key[0], lit));
            if (!ds.ok()) {
              m.log("[error] repair replace-delete " + t->name + ": " + ds.str());
              repair_failed = true;
            }
          }
          Status cs = tgt.commit();
          if (!cs.ok()) {
            m.log("[error] repair replace-commit " + t->name + ": " + cs.str());
            repair_failed = true;
          }
          Status ws = tgt.write_batch(t->name, vbuf);
          if (!ws.ok()) {
            m.log("[error] repair replace-write " + t->name + ": " + ws.str());
            repair_failed = true;
          }
          Status cs2 = tgt.commit();
          if (!cs2.ok()) {
            m.log("[error] repair replace-write-commit " + t->name + ": " + cs2.str());
            repair_failed = true;
          }
          vkeys.clear();
          vbuf.clear();
        };
        while (ha || hb) {
          if (!ha && !hb) break;
          if (!ha) {
            dbuf.push_back(rb[ib][(size_t)pk_idx].sql_literal());
            ++ib;
            ++extra;
            hb = refill(*dst, rb, ib);
          } else if (!hb) {
            wbuf.push_back(ra[ia]);
            ++ia;
            ++missing;
            ha = refill(src, ra, ia);
          } else {
            const int c = cmp_pk(ra[ia][(size_t)pk_idx], rb[ib][(size_t)pk_idx]);
            if (c < 0) {
              wbuf.push_back(ra[ia]);
              ++ia;
              ++missing;
              ha = refill(src, ra, ia);
            } else if (c > 0) {
              dbuf.push_back(rb[ib][(size_t)pk_idx].sql_literal());
              ++ib;
              ++extra;
              hb = refill(*dst, rb, ib);
            } else {
              if (!row_values_equal(ra[ia], rb[ib])) {
                vkeys.push_back(rb[ib][(size_t)pk_idx].sql_literal());
                vbuf.push_back(ra[ia]);
                ++changed;
              }
              ++ia;
              ++ib;
              ha = refill(src, ra, ia);
              hb = refill(*dst, rb, ib);
            }
          }
          if (wbuf.size() >= 500) flush_w();
          if (dbuf.size() >= 500) flush_d();
          if (vkeys.size() >= 500) flush_v();
        }
        flush_w();
        flush_d();
        flush_v();
        Status fcs = tgt.commit();
        if (!fcs.ok()) {
          m.log("[error] repair final commit " + t->name + ": " + fcs.str());
          repair_failed = true;
        }
        src.close_cursor();
        dst->close_cursor();
        if (repair_failed) {
          tgt.rollback();
          return Status::Err(Code::kDb, "repair failed for " + t->name);
        }
        m.log("repair " + t->name + ": +" + std::to_string(missing) + " / -" +
              std::to_string(extra) + " / ~" + std::to_string(changed) + " (stream)");
        repaired += 1;
        continue;
      }
      src.close_cursor();
      dst->close_cursor();
    }

    std::unordered_set<std::string> a;
    std::unordered_set<std::string> b;
    std::vector<int> pk_idx_all;
    for (const auto& key : s.primary_key) {
      int idx = -1;
      for (size_t i = 0; i < s.columns.size(); ++i)
        if (s.columns[i].name == key) { idx = (int)i; break; }
      if (idx < 0) { pk_idx_all.clear(); break; }
      pk_idx_all.push_back(idx);
    }
    if (pk_idx_all.empty()) continue;

    std::unordered_set<std::string> value_keys;
    {
      std::lock_guard<std::mutex> lk(m.mu);
      auto it = m.mmv_table_mismatches.find(t->name);
      if (it != m.mmv_table_mismatches.end()) {
        for (const auto& mm : it->second) {
          if (mm.kind == "value" && !mm.key_values.empty())
            value_keys.insert(encode_key_values(mm.key_values));
        }
      }
    }
    std::unordered_map<std::string, Row> flagged_target;
    std::vector<Row> replace_src, replace_tgt;

    // Target first: keep only rows whose keys mmv flagged as value mismatches.
    st = dst->open_cursor(t->name);
    if (!st.ok()) continue;
    std::vector<Row> rows;
    while (true) {
      rows.clear();
      if (!dst->fetch_batch(2000, rows).ok() || rows.empty()) break;
      for (auto& r : rows) {
        if (r.size() < s.columns.size()) continue;
        const std::string k = repair_key_text(r, pk_idx_all);
        b.insert(k);
        if (value_keys.count(k)) flagged_target.emplace(k, r);
      }
    }
    dst->close_cursor();

    st = src.open_cursor(t->name);
    if (!st.ok()) continue;
    while (true) {
      rows.clear();
      if (!src.fetch_batch(2000, rows).ok() || rows.empty()) break;
      for (auto& r : rows) {
        if (r.size() < s.columns.size()) continue;
        const std::string k = repair_key_text(r, pk_idx_all);
        a.insert(k);
        if (value_keys.count(k)) {
          auto it = flagged_target.find(k);
          if (it != flagged_target.end() && !row_values_equal(r, it->second)) {
            replace_src.push_back(r);
            replace_tgt.push_back(it->second);
          }
        }
      }
    }
    src.close_cursor();

    std::vector<std::string> missing;
    std::vector<std::string> extra;
    for (auto& k : a) if (!b.count(k)) missing.push_back(k);
    for (auto& k : b) if (!a.count(k)) extra.push_back(k);
    if (missing.empty() && extra.empty() && replace_src.empty()) continue;

    if (!missing.empty()) {
      std::unordered_set<std::string> want(missing.begin(), missing.end());
      st = src.open_cursor(t->name);
      if (st.ok()) {
        std::vector<Row> batch;
        int commits = 0;
        while (true) {
          rows.clear();
          if (!src.fetch_batch(2000, rows).ok() || rows.empty()) break;
          for (auto& r : rows) {
            if (r.size() == s.columns.size() &&
                want.count(repair_key_text(r, pk_idx_all)))
              batch.push_back(r);
          }
          if (batch.size() >= 500) {
            tgt.write_batch(t->name, batch);
            batch.clear();
            if (++commits % (o.commit_every > 0 ? o.commit_every : 10) == 0) tgt.commit();
          }
        }
        if (!batch.empty()) tgt.write_batch(t->name, batch);
        tgt.commit();
        src.close_cursor();
      }
    }

    if (!extra.empty()) {
      std::unordered_set<std::string> drop(extra.begin(), extra.end());
      st = dst->open_cursor(t->name);
      if (st.ok()) {
        int n = 0;
        while (true) {
          rows.clear();
          if (!dst->fetch_batch(2000, rows).ok() || rows.empty()) break;
          for (auto& r : rows) {
            if (r.size() < s.columns.size()) continue;
            if (drop.count(repair_key_text(r, pk_idx_all))) {
              const std::string del = tgt.delete_row_sql(s, r);
              if (!del.empty()) tgt.exec_sql(del);
              if (++n % 200 == 0) tgt.commit();
            }
          }
        }
        tgt.commit();
        dst->close_cursor();
      }
    }

    if (!replace_src.empty()) {
      for (const auto& r : replace_tgt) {
        const std::string del = tgt.delete_row_sql(s, r);
        if (!del.empty()) tgt.exec_sql(del);
      }
      tgt.commit();
      tgt.write_batch(t->name, replace_src);
      tgt.commit();
    }

    m.log("repair " + t->name + ": +" + std::to_string(missing.size()) + " / -" +
          std::to_string(extra.size()) + " / ~" + std::to_string(replace_src.size()));
    repaired += 1;
  }
  m.log("repair finished: " + std::to_string(repaired) + " table(s) adjusted");
  return Status::OK();
#endif
}

void write_log_file(Metrics& m, const std::string& dir) {
  export_log(m, dir);
}

}  // namespace mig
