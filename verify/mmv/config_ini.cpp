#include "config_ini.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace mmv {
namespace {

std::string trim(std::string s) {
  size_t b = 0;
  while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
  size_t e = s.size();
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool truthy(const std::string& s) {
  const std::string v = lower(trim(s));
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace

Status apply_config_file(const std::string& path, VerifyOptions& o) {
  std::ifstream in(path);
  if (!in) return Status::Err(Code::kIO, "cannot open config: " + path);
  std::string line;
  std::string section;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = lower(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return Status::Err(Code::kInvalid,
                         "config line " + std::to_string(lineno) + ": missing '='");
    const std::string key = lower(trim(line.substr(0, eq)));
    const std::string val = trim(line.substr(eq + 1));

    if (section == "source") {
      if (key == "type") o.source_kind = val;
      else if (key == "dsn") o.source_dsn = val;
    } else if (section == "target") {
      if (key == "type") o.target_kind = val;
      else if (key == "dsn") o.target_dsn = val;
    } else if (section == "verify") {
      if (key == "key") o.key = val;
      else if (key == "tables") {
        o.tables.clear();
        std::string cur;
        for (char c : val) {
          if (c == ',') { if (!cur.empty()) o.tables.push_back(trim(cur)); cur.clear(); }
          else cur += c;
        }
        if (!cur.empty()) o.tables.push_back(trim(cur));
      } else if (key == "chunk_rows") o.chunk_rows = std::atoll(val.c_str());
      else if (key == "chunk_bytes") o.chunk_bytes = std::atoll(val.c_str());
      else if (key == "workers") o.workers = std::atoi(val.c_str());
      else if (key == "max_mismatches") o.max_mismatches = std::atoll(val.c_str());
      else if (key == "max_split_rows") o.max_split_rows = std::atoll(val.c_str());
      else if (key == "mode") o.row_mode = lower(val) == "row";
      else if (key == "epsilon") o.epsilon = std::atof(val.c_str());
      else if (key == "case_insensitive") o.case_insensitive = truthy(val);
      else if (key == "trim_trailing_spaces") o.trim_space = truthy(val);
      else if (key == "accent_insensitive") o.accent_insensitive = truthy(val);
      else if (key == "allow_seqscan") o.allow_seqscan = truthy(val);
      else if (key == "allow_tz_mismatch") o.allow_tz_mismatch = truthy(val);
      else if (key == "query_timeout") o.query_timeout = std::atoi(val.c_str());
      else if (key == "retries") o.retries = std::atoi(val.c_str());
      else if (key == "retry_backoff_ms") o.retry_backoff_ms = std::atoi(val.c_str());
      else if (key == "progress") o.progress = truthy(val);
      else if (key == "snapshot") o.snapshot = truthy(val);
      else if (key == "snapshot_global") o.snapshot_global = truthy(val);
      else if (key == "assume_static") o.assume_static = truthy(val);
      else if (key == "resume_dir") o.resume_dir = val;
      else if (key == "resume_gc") o.resume_gc = std::atoi(val.c_str());
      else if (key == "parallel_unit") o.parallel_unit = val;
      else if (key == "resume_reset") o.resume_reset = truthy(val);
      else if (key == "resume_token") o.resume_token = val;
      else if (key == "mysql_lock") o.mysql_lock = truthy(val);
      else if (key == "partial") o.partial = truthy(val);
      else if (key == "readonly") o.readonly = truthy(val);
      else if (key == "incremental") o.incremental = truthy(val);
      else if (key == "watermark_column") o.watermark_column = val;
      else if (key == "watermark_state") o.watermark_state = val;
      else if (key == "require_tls") o.require_tls = truthy(val);
      else if (key == "client_normalize") o.client_normalize = truthy(val);
      else if (key == "blob_chunk_bytes") o.blob_chunk_bytes = std::atoll(val.c_str());
      else if (key == "keys_file") o.keys_file = val;
      else if (key == "exclude") {
        o.exclude_tables.clear();
        std::string cur;
        for (char c : val) {
          if (c == ',') {
            if (!cur.empty()) o.exclude_tables.push_back(trim(cur));
            cur.clear();
          } else {
            cur += c;
          }
        }
        if (!cur.empty()) o.exclude_tables.push_back(trim(cur));
      }
    }
  }
  return Status::OK();
}

}  // namespace mmv
