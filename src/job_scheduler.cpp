#include "job_scheduler.h"

#include <string>

namespace mig {
namespace {

std::string fmt_count(int64_t v) {
  std::string s = std::to_string(v);
  std::string out;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) {
    out.push_back(s[(size_t)i]);
    if (++c % 3 == 0 && i > 0) out.push_back(',');
  }
  return std::string(out.rbegin(), out.rend());
}

}  // namespace

bool JobScheduler::prepare(
    ISource& src, ITarget& tgt, const TypeMap& tm, const Options& o,
    const std::unordered_map<std::string, TablePolicy>& policies, Metrics& m,
    std::vector<TableJob>& jobs, std::vector<TableJob>& rebuilds) {
  m.phase = "listing tables";
  std::vector<std::string> tables;
  Status st = src.list_tables(tables);
  if (!st.ok()) {
    m.log("[error] list_tables: " + st.str());
    return false;
  }

  if (!o.only_tables.empty()) {
    std::vector<std::string> filtered;
    for (const auto& t : tables) {
      for (const auto& n : o.only_tables) {
        if (n == t) { filtered.push_back(t); break; }
      }
    }
    for (const auto& n : o.only_tables) {
      bool found = false;
      for (const auto& t : tables) if (t == n) { found = true; break; }
      if (!found) m.log("[warn] only-tables: not found: " + n);
    }
    tables.swap(filtered);
  }
  if (!o.exclude_tables.empty()) {
    std::vector<std::string> kept;
    for (const auto& t : tables) {
      bool skip = false;
      for (const auto& n : o.exclude_tables) if (n == t) { skip = true; break; }
      if (!skip) kept.push_back(t);
    }
    tables.swap(kept);
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
    job.ddl = tgt.build_ddl(job.schema);
    job.rows = src.total_rows(t);
    if (job.rows < 0) job.rows = 0;
    total += job.rows;
    jobs.push_back(std::move(job));
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
        std::to_string(rebuilds.size()) + ", rows: " + fmt_count(total));
  return !jobs.empty() || !rebuilds.empty();
}

}  // namespace mig
