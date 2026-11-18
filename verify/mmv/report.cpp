#include "engine.h"

#include <cstdio>
#include <sstream>

namespace mmv {
namespace {

std::string jesc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

}  // namespace

std::string report_json(const Report& r, const std::string& version) {
  int64_t total_rows = 0;
  int64_t total_bytes = 0;
  for (const auto& t : r.tables) {
    total_rows += t.rows_source;
    total_bytes += t.approx_bytes_source;
  }
  std::ostringstream o;
  o << "{\"schema\":\"mmv-report-v1\",\"tool\":\"mm-verify\",\"version\":\""
    << jesc(version) << "\",";
  o << "\"ok\":" << (r.ok ? "true" : "false") << ",";
  o << "\"operational_error\":" << (r.operational_error ? "true" : "false") << ",";
  o << "\"cancelled\":" << (r.cancelled ? "true" : "false") << ",";
  o << "\"partial\":" << (r.partial ? "true" : "false") << ",";
  o << "\"source\":\"" << jesc(r.source_desc) << "\",";
  o << "\"target\":\"" << jesc(r.target_desc) << "\",";
  o << "\"elapsed_ms\":" << (long long)r.elapsed_ms << ",";
  o << "\"rows_per_sec\":" << (r.elapsed_ms > 0 ? (double)total_rows / (r.elapsed_ms / 1000.0) : 0.0)
    << ",";
  o << "\"mb_per_sec\":" << (r.elapsed_ms > 0 ? (double)total_bytes / 1e6 / (r.elapsed_ms / 1000.0) : 0.0)
    << ",";
  o << "\"chunks\":" << (long long)r.chunks << ",";
  o << "\"completed_chunks\":" << (long long)r.completed_chunks << ",";
  o << "\"failed_chunks\":" << (long long)r.failed_chunks << ",";
  o << "\"resumed_chunks\":" << (long long)r.resumed_chunks << ",";
  o << "\"mismatches\":" << (long long)r.mismatches << ",";
  if (!r.error.empty()) o << "\"error\":\"" << jesc(r.error) << "\",";
  o << "\"tables\":[";
  for (size_t i = 0; i < r.tables.size(); ++i) {
    const auto& t = r.tables[i];
    if (i) o << ",";
    o << "{\"name\":\"" << jesc(t.name) << "\",\"status\":\"" << jesc(t.status)
      << "\",\"rows_source\":" << (long long)t.rows_source
      << ",\"rows_target\":" << (long long)t.rows_target
      << ",\"elapsed_ms\":" << (long long)t.elapsed_ms
      << ",\"rows_per_sec\":" << (t.elapsed_ms > 0 ? (double)t.rows_source / (t.elapsed_ms / 1000.0) : 0.0)
      << ",\"mb_per_sec\":" << (t.elapsed_ms > 0 ? (double)t.approx_bytes_source / 1e6 / (t.elapsed_ms / 1000.0) : 0.0)
      << ",\"chunks\":" << (long long)t.chunks
      << ",\"completed_chunks\":" << (long long)t.completed_chunks
      << ",\"failed_chunks\":" << (long long)t.failed_chunks
      << ",\"resumed_chunks\":" << (long long)t.resumed_chunks;
    if (!t.error.empty()) o << ",\"error\":\"" << jesc(t.error) << "\"";
    if (!t.note.empty()) o << ",\"note\":\"" << jesc(t.note) << "\"";
    o << ",\"mismatches\":[";
    for (size_t j = 0; j < t.mismatches.size(); ++j) {
      const auto& m = t.mismatches[j];
      if (j) o << ",";
      o << "{\"kind\":\"" << mismatch_kind_name(m.kind) << "\",\"key\":\"" << jesc(m.key)
        << "\",\"column\":\"" << jesc(m.column) << "\",\"source_value\":\""
        << jesc(m.source_value) << "\",\"target_value\":\"" << jesc(m.target_value) << "\"}";
    }
    o << "]}";
  }
  o << "]}";
  return o.str();
}

std::string metrics_json(const Report& r, const std::string& version) {
  int64_t rs = 0, rt = 0, bytes = 0;
  for (const auto& t : r.tables) {
    rs += t.rows_source;
    rt += t.rows_target;
    bytes += t.approx_bytes_source;
  }
  const double sec = r.elapsed_ms > 0 ? r.elapsed_ms / 1000.0 : 0.0;
  std::ostringstream o;
  o << "{\"schema\":\"mmv-metrics-v1\",\"tool\":\"mm-verify\",\"version\":\""
    << jesc(version) << "\",";
  o << "\"elapsed_ms\":" << (long long)r.elapsed_ms << ",";
  o << "\"rows_source\":" << (long long)rs << ",";
  o << "\"rows_target\":" << (long long)rt << ",";
  o << "\"rows_per_sec\":" << (sec > 0 ? (double)rs / sec : 0.0) << ",";
  o << "\"mb_per_sec\":" << (sec > 0 ? (double)bytes / 1e6 / sec : 0.0) << ",";
  o << "\"chunks\":" << (long long)r.chunks << ",";
  o << "\"completed_chunks\":" << (long long)r.completed_chunks << ",";
  o << "\"failed_chunks\":" << (long long)r.failed_chunks << ",";
  o << "\"resumed_chunks\":" << (long long)r.resumed_chunks << ",";
  o << "\"mismatches\":" << (long long)r.mismatches << ",";
  o << "\"tables\":[";
  for (size_t i = 0; i < r.tables.size(); ++i) {
    const auto& t = r.tables[i];
    if (i) o << ",";
    const double ts = t.elapsed_ms > 0 ? t.elapsed_ms / 1000.0 : 0.0;
    o << "{\"name\":\"" << jesc(t.name) << "\",\"status\":\"" << jesc(t.status)
      << "\",\"elapsed_ms\":" << (long long)t.elapsed_ms
      << ",\"rows_source\":" << (long long)t.rows_source
      << ",\"rows_target\":" << (long long)t.rows_target
      << ",\"rows_per_sec\":" << (ts > 0 ? (double)t.rows_source / ts : 0.0)
      << ",\"mb_per_sec\":" << (ts > 0 ? (double)t.approx_bytes_source / 1e6 / ts : 0.0)
      << ",\"chunks\":" << (long long)t.chunks
      << ",\"completed_chunks\":" << (long long)t.completed_chunks
      << ",\"failed_chunks\":" << (long long)t.failed_chunks
      << ",\"resumed_chunks\":" << (long long)t.resumed_chunks << "}";
  }
  o << "]}";
  return o.str();
}

std::string prometheus_metrics(const Report& r, const std::string& version) {
  std::ostringstream o;
  auto table_label = [](const std::string& s) { return jesc(s); };
  o << "# HELP mmv_up 1 when the verifier produced a report.\n";
  o << "# TYPE mmv_up gauge\nmmv_up 1\n";
  o << "mmv_info{version=\"" << jesc(version) << "\"} 1\n";
  o << "# HELP mmv_ok 1 when every table matched.\n";
  o << "# TYPE mmv_ok gauge\nmmv_ok " << (r.ok ? 1 : 0) << "\n";
  o << "# HELP mmv_elapsed_seconds Wall-clock verification time.\n";
  o << "# TYPE mmv_elapsed_seconds gauge\nmmv_elapsed_seconds "
    << (r.elapsed_ms / 1000.0) << "\n";
  o << "# HELP mmv_rows_source Rows read from the source.\n";
  o << "# TYPE mmv_rows_source counter\nmmv_rows_source ";
  int64_t rs = 0, rt = 0, bytes = 0;
  for (const auto& t : r.tables) { rs += t.rows_source; rt += t.rows_target; bytes += t.approx_bytes_source; }
  o << rs << "\n";
  o << "# TYPE mmv_rows_target counter\nmmv_rows_target " << rt << "\n";
  const double sec = r.elapsed_ms > 0 ? r.elapsed_ms / 1000.0 : 0.0;
  o << "# TYPE mmv_mb_per_sec gauge\nmmv_mb_per_sec "
    << (sec > 0 ? (double)bytes / 1e6 / sec : 0.0) << "\n";
  o << "# TYPE mmv_chunks gauge\nmmv_chunks " << (long long)r.chunks << "\n";
  o << "# TYPE mmv_completed_chunks gauge\nmmv_completed_chunks "
    << (long long)r.completed_chunks << "\n";
  o << "# TYPE mmv_failed_chunks gauge\nmmv_failed_chunks "
    << (long long)r.failed_chunks << "\n";
  o << "# TYPE mmv_resumed_chunks gauge\nmmv_resumed_chunks "
    << (long long)r.resumed_chunks << "\n";
  o << "# TYPE mmv_mismatches gauge\nmmv_mismatches " << (long long)r.mismatches << "\n";
  o << "# TYPE mmv_table_rows_source gauge\n";
  for (const auto& t : r.tables)
    o << "mmv_table_rows_source{table=\"" << table_label(t.name) << "\"} "
      << (long long)t.rows_source << "\n";
  o << "# TYPE mmv_table_mismatches gauge\n";
  for (const auto& t : r.tables)
    o << "mmv_table_mismatches{table=\"" << table_label(t.name) << "\"} "
      << (long long)t.mismatches.size() << "\n";
  o << "# TYPE mmv_table_elapsed_seconds gauge\n";
  for (const auto& t : r.tables)
    o << "mmv_table_elapsed_seconds{table=\"" << table_label(t.name) << "\"} "
      << (t.elapsed_ms / 1000.0) << "\n";
  return o.str();
}

std::string report_text(const Report& r) {
  std::ostringstream o;
  o << "source: " << r.source_desc << "\n";
  o << "target: " << r.target_desc << "\n";
  if (!r.error.empty() && r.tables.empty()) o << "error: " << r.error << "\n";
  for (const auto& t : r.tables) {
    o << "[" << t.status << "] " << t.name;
    if (t.chunks) {
      o << "  rows " << t.rows_source << " vs " << t.rows_target << ", chunks "
        << t.completed_chunks << "/" << t.chunks << " ok";
      if (t.resumed_chunks > 0) o << ", resumed " << t.resumed_chunks;
    }
    o << "\n";
    if (!t.error.empty()) o << "    error: " << t.error << "\n";
    if (!t.note.empty()) o << "    note: " << t.note << "\n";
    for (const auto& m : t.mismatches) {
      o << "    " << mismatch_kind_name(m.kind) << " key=" << m.key;
      if (!m.column.empty()) o << " col=" << m.column;
      if (!m.source_value.empty() || !m.target_value.empty())
        o << " source=\"" << m.source_value << "\" target=\"" << m.target_value << "\"";
      o << "\n";
    }
  }
  o << "elapsed " << (long long)r.elapsed_ms << " ms, chunks " << r.completed_chunks
    << "/" << r.chunks << ", failed " << r.failed_chunks << ", mismatches " << r.mismatches
    << "\n";
  const bool err = r.operational_error || (!r.error.empty() && r.tables.empty());
  o << (r.cancelled ? "RESULT: CANCELLED"
                    : (err ? "RESULT: ERROR" : (r.ok ? "RESULT: OK" : "RESULT: FAIL")))
    << "\n";
  return o.str();
}

}  // namespace mmv
