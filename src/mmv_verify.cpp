#include "mmv_verify.h"

#include "migrator.h"

#include "cancel.h"
#include "engine.h"

#include <csignal>
#include <string>
#include <vector>

// mm-verify's main.cpp normally owns this flag; the bridge provides it when
// the engine is linked into the migrator.
namespace mmv {
volatile std::sig_atomic_t g_interrupted = 0;
}

namespace mig {
namespace {

std::string mmv_kind(const std::string& k) {
  if (k == "postgres") return "pg";
  if (k == "opengauss") return "og";
  return k;
}

std::string table_detail(const mmv::TableReport& t) {
  if (!t.error.empty()) return t.error;
  if (t.status == "mismatch" && !t.mismatches.empty()) {
    const auto& m = t.mismatches.front();
    std::string d = std::string(mmv::mismatch_kind_name(m.kind)) + " key=" + m.key;
    if (!m.column.empty()) d += " column=" + m.column;
    if (t.mismatches.size() > 1)
      d += " (+" + std::to_string(t.mismatches.size() - 1) + " more)";
    return d;
  }
  return t.status == "error" ? "error" : "";
}

}  // namespace

namespace {
mmv::LogicalType logical_from_family(const std::string& f) {
  if (f == "int") return mmv::LogicalType::kInt;
  if (f == "decimal") return mmv::LogicalType::kDecimal;
  if (f == "float") return mmv::LogicalType::kFloat;
  if (f == "bool") return mmv::LogicalType::kBool;
  if (f == "string") return mmv::LogicalType::kString;
  if (f == "char") return mmv::LogicalType::kChar;
  if (f == "bytes") return mmv::LogicalType::kBytes;
  if (f == "date") return mmv::LogicalType::kDate;
  if (f == "time") return mmv::LogicalType::kTime;
  if (f == "timestamp") return mmv::LogicalType::kTimestamp;
  if (f == "timestamptz") return mmv::LogicalType::kTimestampTz;
  if (f == "json") return mmv::LogicalType::kJson;
  if (f == "uuid") return mmv::LogicalType::kUuid;
  return mmv::LogicalType::kUnknown;
}
}  // namespace

MmvVerifyResult run_mmv_verify(const Options& o, const std::vector<std::string>& tables) {
  return run_mmv_verify(o, tables, {});
}

MmvVerifyResult run_mmv_verify(const Options& o, const std::vector<std::string>& tables,
                               const std::vector<MmvColumnOverride>& overrides) {
  MmvVerifyResult out;
  mmv::VerifyOptions v;
  v.source_kind = mmv_kind(o.source_kind);
  v.target_kind = mmv_kind(o.target_kind);
  v.source_dsn = o.source;
  v.target_dsn = o.target;
  v.tables = tables;
  v.assume_static = true;  // post-load / verify-only: target is not being written
  v.readonly = true;
  v.quiet = true;
  // Verification is digest-only and parallelizes well; migrator's default
  // workers=1 would serialize it. Use auto unless the user explicitly asked
  // for more than one migration worker.
  v.workers = o.workers > 1 ? o.workers : 0;
  v.max_mismatches = 50;
  v.progress = false;
  // 500k rows per chunk mirrors mmv --fast: on a 10M table this cuts 100
  // round trips to 20, which matters more than per-chunk scan size.
  v.chunk_rows = 500000;

  for (const auto& ov : overrides) {
    mmv::ColumnSpecOverride m;
    m.table = ov.table;
    m.column = ov.column;
    m.target = ov.target;
    m.type = logical_from_family(ov.family);
    m.epsilon = ov.epsilon;
    m.case_insensitive = ov.case_insensitive;
    m.trim_space = ov.trim_space;
    m.accent_insensitive = ov.accent_insensitive;
    m.digest_bytes = ov.digest_bytes;
    v.column_overrides.push_back(std::move(m));
  }

  const mmv::Report rep = mmv::run_verify(v);
  out.ok = rep.ok;
  out.mismatches = rep.mismatches;
  out.detail = rep.error;

  for (const auto& name : tables) {
    MmvTableResult tr;
    tr.table = name;
    const mmv::TableReport* found = nullptr;
    for (const auto& t : rep.tables) {
      if (t.name == name) {
        found = &t;
        break;
      }
    }
    if (!found) {
      tr.ok = false;
      tr.detail = "table was not verified";
    } else {
      tr.ok = (found->status == "ok");
      tr.mismatches = (int64_t)found->mismatches.size();
      tr.detail = table_detail(*found);
      for (const auto& m : found->mismatches) {
        MmvMismatch x;
        x.kind = mmv::mismatch_kind_name(m.kind);
        x.key = m.key;
        x.column = m.column;
        x.source_value = m.source_value;
        x.target_value = m.target_value;
        x.key_values = m.key_values;
        tr.diffs.push_back(std::move(x));
      }
    }
    out.tables.push_back(std::move(tr));
  }
  if (out.detail.empty() && !out.ok && out.tables.size() == 1)
    out.detail = out.tables.front().detail;
  return out;
}

MmvVerifyResult run_mmv_table_verify(const Options& o, const std::string& table) {
  MmvVerifyResult batch = run_mmv_verify(o, {table});
  MmvVerifyResult out;
  out.ok = batch.ok;
  out.mismatches = batch.mismatches;
  out.detail = batch.detail;
  if (!batch.tables.empty()) out.detail = batch.tables.front().detail;
  return out;
}

}  // namespace mig
