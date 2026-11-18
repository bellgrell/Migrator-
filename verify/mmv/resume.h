#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "chunk.h"
#include "status.h"

namespace mmv {

struct ResumeConfig {
  bool enabled = false;
  bool reset = false;
  int gc = 0;  // keep current + newest (gc-1) old plan dirs; 0 disables
  std::string dir;
};

// Per-table plan manifest + append-only checkpoint.
//
// The directory name includes a hash of the fingerprint, so a changed schema,
// option set, or table statistic creates a fresh run instead of silently
// reusing stale chunk results.
class ResumeStore {
 public:
  ResumeStore() = default;
  ~ResumeStore();
  ResumeStore(const ResumeStore&) = delete;
  ResumeStore& operator=(const ResumeStore&) = delete;

  Status open(const ResumeConfig& cfg, const std::string& table,
              const std::string& fingerprint,
              const std::vector<TupleRange>& planned,
              std::vector<TupleRange>& out_ranges);

  bool enabled() const { return enabled_; }
  bool done(size_t index);
  bool done_rows(size_t index, int64_t& n_src, int64_t& n_tgt);
  void mark(size_t index, bool ok, int64_t n_src, int64_t n_tgt);
  int64_t loaded_done() const { return (int64_t)done_.size(); }

 private:
  bool enabled_ = false;
  std::string manifest_path_;
  std::string checkpoint_path_;
  Status load_checkpoint();
  std::set<size_t> done_;
  std::map<size_t, std::pair<int64_t, int64_t>> done_rows_;
  std::mutex mu_;
  int fd_ = -1;
  // Checkpoint records are append-only and checksummed per line. fsync is
  // batched (every 32 records or 250 ms): losing the tail on a crash only
  // means those chunks are verified again, never that a bad chunk is skipped.
  int unsynced_ = 0;
  std::chrono::steady_clock::time_point last_sync_ = std::chrono::steady_clock::now();
  int lock_fd_ = -1;  // exclusive lock on the resume directory
};

}  // namespace mmv
