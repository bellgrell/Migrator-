#include "migrator.h"

#include <cmath>
#include <sstream>

namespace mig {

void Metrics::log(const std::string& s) {
  std::lock_guard<std::mutex> lk(mu);
  char ts[32];
  double t = now_sec() - started_at;
  std::snprintf(ts, sizeof ts, "[%7.2fs] ", t < 0 ? 0.0 : t);
  events.emplace_back(std::string(ts) + s);
  while (events.size() > 500) events.pop_front();
}

void Metrics::sample() {
  std::lock_guard<std::mutex> lk(mu);
  series.emplace_back(now_sec(), rows_done.load());
  while (series.size() > 240) series.pop_front();
}

static double pct(int64_t a, int64_t b) {
  if (b <= 0) return 0.0;
  return 100.0 * (double)a / (double)b;
}

std::string Metrics::status_json() {
  std::lock_guard<std::mutex> lk(mu);
  double now = finished.load() ? finished_at : now_sec();
  double elapsed = (started_at > 0 && now > started_at) ? (now - started_at) : 0.0;
  int64_t done = rows_done.load();
  int64_t total = rows_total.load();
  double rps = elapsed > 0.5 ? (double)done / elapsed : 0.0;
  double eta = 0.0;
  if (rps > 100.0 && total > done) eta = (double)(total - done) / rps;

  std::ostringstream o;
  o << "{";
  o << "\"running\":" << (running.load() ? "true" : "false") << ",";
  o << "\"finished\":" << (finished.load() ? "true" : "false") << ",";
  o << "\"phase\":\"" << json_escape(phase) << "\",";
  o << "\"mode\":\"" << json_escape(mode) << "\",";
  o << "\"source\":\"" << json_escape(source_desc) << "\",";
  o << "\"target\":\"" << json_escape(target_desc) << "\",";
  o << "\"rows_done\":" << done << ",";
  o << "\"rows_total\":" << total << ",";
  o << "\"percent\":" << pct(done, total) << ",";
  o << "\"errors\":" << errors.load() << ",";
  o << "\"batches\":" << batches.load() << ",";
  o << "\"bytes\":" << bytes_written.load() << ",";
  o << "\"elapsed\":" << elapsed << ",";
  o << "\"rows_per_sec\":" << rps << ",";
  o << "\"eta\":" << eta << ",";
  o << "\"tables\":" << tables.size() << ",";
  o << "\"verify\":{\"enabled\":" << (verifying.load() || verified.load() ? "true" : "false")
    << ",\"running\":" << (verifying.load() ? "true" : "false")
    << ",\"done\":" << (verified.load() ? "true" : "false")
    << ",\"mismatch\":" << verify_mismatch.load() << "},";
  o << "\"now\":" << now_sec();
  o << "}";
  return o.str();
}

std::string Metrics::tables_json() {
  std::lock_guard<std::mutex> lk(mu);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < tables.size(); ++i) {
    auto& t = tables[i];
    if (i) o << ",";
    int st = t->state.load();
    const char* stn = st == 0 ? "pending" : st == 1 ? "running" : st == 2 ? "done" : "failed";
    o << "{\"name\":\"" << json_escape(t->name) << "\","
      << "\"total\":" << t->total << ","
      << "\"done\":" << t->done.load() << ","
      << "\"errors\":" << t->errors.load() << ","
      << "\"state\":\"" << stn << "\","
      << "\"percent\":" << pct(t->done.load(), t->total) << "}";
  }
  o << "]";
  return o.str();
}

std::string Metrics::events_json() {
  std::lock_guard<std::mutex> lk(mu);
  std::ostringstream o;
  o << "[";
  bool first = true;
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (!first) o << ",";
    first = false;
    o << "\"" << json_escape(*it) << "\"";
  }
  o << "]";
  return o.str();
}

std::string Metrics::prometheus() {
  std::lock_guard<std::mutex> lk(mu);
  std::ostringstream o;
  auto gauge = [&](const char* name, const char* help, int64_t v) {
    o << "# HELP " << name << " " << help << "\n";
    o << "# TYPE " << name << " gauge\n";
    o << name << " " << v << "\n";
  };
  gauge("migrator_rows_done", "Rows migrated", rows_done.load());
  gauge("migrator_rows_total", "Rows planned", rows_total.load());
  gauge("migrator_errors_total", "Errors", errors.load());
  gauge("migrator_batches_total", "Batches", batches.load());
  gauge("migrator_bytes_written_total", "Bytes written", bytes_written.load());
  gauge("migrator_verify_mismatch", "Verification mismatches", verify_mismatch.load());
  o << "# HELP migrator_running 1 if migration is running\n# TYPE migrator_running gauge\n"
    << "migrator_running " << (running.load() ? 1 : 0) << "\n";
  o << "# HELP migrator_verified 1 if verification passed\n# TYPE migrator_verified gauge\n"
    << "migrator_verified " << (verified.load() && verify_mismatch.load() == 0 ? 1 : 0) << "\n";
  for (const auto& t : tables) {
    if (!t) continue;
    o << "migrator_table_rows_done{table=\"" << json_escape(t->name) << "\"} "
      << t->done.load() << "\n";
    o << "migrator_table_rows_total{table=\"" << json_escape(t->name) << "\"} "
      << t->total << "\n";
    o << "migrator_table_errors{table=\"" << json_escape(t->name) << "\"} "
      << t->errors.load() << "\n";
  }
  return o.str();
}

std::string Metrics::series_json() {
  std::lock_guard<std::mutex> lk(mu);
  std::ostringstream o;
  o << "[";
  double t0 = series.empty() ? 0.0 : series.front().first;
  bool first = true;
  for (auto& p : series) {
    if (!first) o << ",";
    first = false;
    o << "[" << (p.first - t0) << "," << p.second << "]";
  }
  o << "]";
  return o.str();
}

}  // namespace mig
