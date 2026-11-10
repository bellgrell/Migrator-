#pragma once

// Application-level I/O scheduler for range-parallel copies.
//
// The scheduler does not try to control the database server's physical disk
// scheduler (that is owned by the OS/DB engine).  It schedules *client-side*
// batch reads so that:
//   - at most N range readers have an outstanding read at a time;
//   - readers take turns (FIFO/round-robin), so no range starves;
//   - optional SCAN mode only admits ranges inside a moving key window,
//     which keeps the source-side access pattern clustered.
//
// A bounded per-range queue adds read-ahead: the reader can fetch the next
// batch while the writer is still sending the current one.

#include "migrator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mig {

enum class IoPolicy { kOff = 0, kFair = 1, kScan = 2 };

inline IoPolicy parse_io_policy(const std::string& s) {
  if (s == "off" || s == "none" || s == "0") return IoPolicy::kOff;
  if (s == "scan" || s == "elevator") return IoPolicy::kScan;
  return IoPolicy::kFair;
}

class IoScheduler {
 public:
  IoScheduler(int ranges, int max_inflight, int window, IoPolicy policy)
      : n_(std::max(1, ranges)),
        max_inflight_(max_inflight > 0 ? max_inflight : std::max(1, ranges)),
        window_(window > 0 ? window : n_),
        policy_(policy),
        cancelled_(static_cast<size_t>(n_), false),
        done_(static_cast<size_t>(n_), false) {}

  // Returns false when this range was cancelled (writer failed).
  bool begin(int id) {
    if (policy_ == IoPolicy::kOff) return true;
    std::unique_lock<std::mutex> lk(mu_);
    const auto requested_at = std::chrono::steady_clock::now();
    waiting_.push_back(id);
    cv_.wait(lk, [&] {
      if (cancelled_[(size_t)id]) return true;
      if (inflight_ >= max_inflight_) return false;
      // SCAN must not let a not-yet-admitted low range block ranges that are
      // already inside the window (head-of-line deadlock).
      if (policy_ == IoPolicy::kScan) return window_ok(id);
      return !waiting_.empty() && waiting_.front() == id;
    });
    if (cancelled_[(size_t)id]) {
      waiting_.erase(std::find(waiting_.begin(), waiting_.end(), id));
      cv_.notify_all();
      return false;
    }
    waiting_.erase(std::find(waiting_.begin(), waiting_.end(), id));
    ++inflight_;
    if (inflight_ > peak_inflight_) peak_inflight_ = inflight_;
    const auto now = std::chrono::steady_clock::now();
    total_wait_us_ +=
        std::chrono::duration_cast<std::chrono::microseconds>(now - requested_at)
            .count();
    ++wait_count_;
    cv_.notify_all();
    return true;
  }

  void end(int id) {
    (void)id;
    if (policy_ == IoPolicy::kOff) return;
    std::unique_lock<std::mutex> lk(mu_);
    if (inflight_ > 0) --inflight_;
    cv_.notify_all();
  }

  void finish(int id) {
    if (policy_ == IoPolicy::kOff) return;
    std::unique_lock<std::mutex> lk(mu_);
    done_[(size_t)id] = true;
    while (cursor_ < n_ && done_[(size_t)cursor_]) ++cursor_;
    cv_.notify_all();
  }

  void cancel(int id) {
    if (policy_ == IoPolicy::kOff) return;
    std::unique_lock<std::mutex> lk(mu_);
    cancelled_[(size_t)id] = true;
    waiting_.erase(std::find(waiting_.begin(), waiting_.end(), id));
    cv_.notify_all();
  }

  int peak_inflight() const { return peak_inflight_; }
  int64_t wait_count() const { return wait_count_; }
  double avg_wait_ms() const {
    return wait_count_ > 0 ? (double)total_wait_us_ / 1000.0 / (double)wait_count_
                           : 0.0;
  }

 private:
  bool window_ok(int id) const {
    if (policy_ != IoPolicy::kScan) return true;
    return id < cursor_ + std::max(1, window_);
  }

  int n_;
  int max_inflight_;
  int window_;
  IoPolicy policy_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<int> waiting_;
  int inflight_ = 0;
  int cursor_ = 0;
  int peak_inflight_ = 0;
  int64_t wait_count_ = 0;
  int64_t total_wait_us_ = 0;
  std::vector<bool> cancelled_;
  std::vector<bool> done_;
};

// Bounded batch queue (read-ahead buffer for one range).
class BatchQueue {
 public:
  explicit BatchQueue(size_t capacity) : cap_(capacity > 0 ? capacity : 1) {}

  bool push(std::vector<Row>&& rows) {
    std::unique_lock<std::mutex> lk(mu_);
    not_full_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
    if (closed_) return false;
    q_.push_back(std::move(rows));
    not_empty_.notify_one();
    return true;
  }

  bool pop(std::vector<Row>& out) {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [&] { return closed_ || !q_.empty(); });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void close() {
    std::unique_lock<std::mutex> lk(mu_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

 private:
  size_t cap_;
  std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<std::vector<Row>> q_;
  bool closed_ = false;
};

}  // namespace mig
