#include "resume.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <sstream>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mmv {
namespace {

#ifdef _WIN32
#define MMV_O_RDWR (_O_RDWR | _O_BINARY)
#define MMV_O_WRONLY (_O_WRONLY | _O_BINARY)
#define MMV_O_CREAT _O_CREAT
#define MMV_O_APPEND _O_APPEND
int mmv_open(const char* p, int flags, int mode) { return _open(p, flags, mode); }
long long mmv_write(int fd, const char* data, size_t n) {
  return (long long)_write(fd, data, (unsigned)n);
}
int mmv_fsync(int fd) { return _commit(fd); }
int mmv_close(int fd) { return _close(fd); }
bool mmv_lock(int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  OVERLAPPED ov{};
  return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &ov) != 0;
}
void mmv_unlock(int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) return;
  OVERLAPPED ov{};
  UnlockFileEx(h, 0, 1, 0, &ov);
}
#else
#define MMV_O_RDWR O_RDWR
#define MMV_O_WRONLY O_WRONLY
#define MMV_O_CREAT O_CREAT
#define MMV_O_APPEND O_APPEND
int mmv_open(const char* p, int flags, int mode) { return ::open(p, flags, mode); }
long long mmv_write(int fd, const char* data, size_t n) {
  return (long long)::write(fd, data, n);
}
int mmv_fsync(int fd) { return ::fsync(fd); }
int mmv_close(int fd) { return ::close(fd); }
bool mmv_lock(int fd) { return ::flock(fd, LOCK_EX | LOCK_NB) == 0; }
void mmv_unlock(int fd) { ::flock(fd, LOCK_UN); }
#endif


void gc_old_dirs(const std::string& base, const std::string& prefix,
                 const std::string& keep, int keep_count);

std::string hex_encode(const std::string& s) {
  static const char* h = "0123456789abcdef";
  std::string o;
  o.reserve(s.size() * 2);
  for (unsigned char c : s) {
    o += h[c >> 4];
    o += h[c & 0xF];
  }
  return o;
}

bool hex_decode(const std::string& s, std::string& out) {
  if (s.size() % 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    const int a = nib(s[i]), b = nib(s[i + 1]);
    if (a < 0 || b < 0) return false;
    out += (char)((a << 4) | b);
  }
  return true;
}

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

std::string safe_name(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (std::isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
    else o += '_';
    if (o.size() >= 40) break;
  }
  return o.empty() ? "table" : o;
}

std::string encode_tuple(const std::vector<KeyValue>& t) {
  if (t.empty()) return "-";
  std::string o;
  for (size_t i = 0; i < t.size(); ++i) {
    if (i) o += ",";
    if (t[i].kind == KeyKind::kInt) o += "i:" + std::to_string((long long)t[i].i);
    else if (t[i].kind == KeyKind::kHex) o += "h:" + hex_encode(t[i].s);
    else o += "s:" + hex_encode(t[i].s);
  }
  return o;
}

bool decode_tuple(const std::string& s, std::vector<KeyValue>& out) {
  out.clear();
  if (s == "-") return true;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t comma = s.find(',', pos);
    const std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (part.size() < 2) return false;
    const std::string kind = part.substr(0, 2);
    const std::string val = part.substr(2);
    if (kind == "i:") {
      out.push_back(KeyValue::Int(std::strtoll(val.c_str(), nullptr, 10)));
    } else if (kind == "s:" || kind == "h:") {
      std::string raw;
      if (!hex_decode(val, raw)) return false;
      out.push_back(kind == "s:" ? KeyValue::Str(raw) : KeyValue::Hex(raw));
    } else {
      return false;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << data;
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
  }
  return !ec;
}

Status write_manifest(const std::string& path, const std::string& table,
                      const std::string& fingerprint,
                      const std::vector<TupleRange>& ranges) {
  std::ostringstream o;
  o << "mmv-resume 1\n";
  o << "fingerprint " << hex_encode(fingerprint) << "\n";
  o << "table " << hex_encode(table) << "\n";
  o << "count " << ranges.size() << "\n";
  for (const auto& r : ranges) {
    o << "range " << (r.has_lo ? 1 : 0) << " " << encode_tuple(r.lo) << " "
      << (r.has_hi ? 1 : 0) << " " << encode_tuple(r.hi) << " "
      << (r.first_key_only ? 1 : 0) << " "
      << (r.partition.empty() ? "-" : hex_encode(r.partition)) << " "
      << (r.partition_tgt.empty() ? "-" : hex_encode(r.partition_tgt)) << "\n";
  }
  std::string body = o.str();
  body += "sum " + hex64(fnv1a(body)) + "\n";
  if (!write_file_atomic(path, body))
    return Status::Err(Code::kIO, "cannot write resume manifest: " + path);
  return Status::OK();
}

Status load_manifest(const std::string& path, const std::string& table,
                     const std::string& fingerprint,
                     std::vector<TupleRange>& ranges) {
  std::ifstream raw_in(path, std::ios::binary);
  if (!raw_in) return Status::Err(Code::kIO, "cannot read resume manifest: " + path);
  std::string raw((std::istreambuf_iterator<char>(raw_in)),
                  std::istreambuf_iterator<char>());
  const size_t sum_pos = raw.rfind("sum ");
  if (sum_pos != std::string::npos &&
      (sum_pos == 0 || raw[sum_pos - 1] == '\n')) {
    const size_t eol = raw.find('\n', sum_pos);
    const std::string sum = raw.substr(sum_pos + 4, eol == std::string::npos
                                                         ? std::string::npos
                                                         : eol - sum_pos - 4);
    const std::string body = raw.substr(0, sum_pos);
    if (sum != hex64(fnv1a(body)))
      return Status::Err(Code::kInvalid, "resume manifest checksum mismatch");
    raw = body;
  }
  std::istringstream in(raw);
  std::string line;
  if (!std::getline(in, line) || line != "mmv-resume 1")
    return Status::Err(Code::kInvalid, "bad resume manifest header");
  if (!std::getline(in, line) || line.rfind("fingerprint ", 0) != 0)
    return Status::Err(Code::kInvalid, "bad resume manifest fingerprint");
  std::string fp;
  if (!hex_decode(line.substr(12), fp) || fp != fingerprint)
    return Status::Err(Code::kInvalid, "resume fingerprint mismatch");
  if (!std::getline(in, line) || line.rfind("table ", 0) != 0)
    return Status::Err(Code::kInvalid, "bad resume manifest table");
  std::string tbl;
  if (!hex_decode(line.substr(6), tbl) || tbl != table)
    return Status::Err(Code::kInvalid, "resume table mismatch");
  if (!std::getline(in, line) || line.rfind("count ", 0) != 0)
    return Status::Err(Code::kInvalid, "bad resume manifest count");
  const size_t n = (size_t)std::strtoull(line.substr(6).c_str(), nullptr, 10);
  ranges.clear();
  ranges.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!std::getline(in, line)) return Status::Err(Code::kInvalid, "short resume manifest");
    std::istringstream ss(line);
    std::string tag, lo, hi;
    int has_lo = 0, has_hi = 0;
    if (!(ss >> tag >> has_lo >> lo >> has_hi >> hi) || tag != "range")
      return Status::Err(Code::kInvalid, "bad resume range line");
    TupleRange r;
    r.has_lo = has_lo != 0;
    r.has_hi = has_hi != 0;
    int first_only = 0;
    if (ss >> first_only) r.first_key_only = first_only != 0;
    std::string phex, pthex;
    if (ss >> phex && phex != "-") {
      if (!hex_decode(phex, r.partition))
        return Status::Err(Code::kInvalid, "bad resume partition");
    }
    if (ss >> pthex && pthex != "-") {
      if (!hex_decode(pthex, r.partition_tgt))
        return Status::Err(Code::kInvalid, "bad resume target partition");
    }
    if (!decode_tuple(lo, r.lo) || !decode_tuple(hi, r.hi))
      return Status::Err(Code::kInvalid, "bad resume range tuple");
    ranges.push_back(std::move(r));
  }
  return Status::OK();
}

}  // namespace

ResumeStore::~ResumeStore() {
  if (fd_ >= 0) {
    if (unsynced_ > 0) mmv_fsync(fd_);
    mmv_close(fd_);
  }
  if (lock_fd_ >= 0) {
    mmv_unlock(lock_fd_);
    mmv_close(lock_fd_);
  }
}

Status ResumeStore::open(const ResumeConfig& cfg, const std::string& table,
                         const std::string& fingerprint,
                         const std::vector<TupleRange>& planned,
                         std::vector<TupleRange>& out_ranges) {
  out_ranges = planned;
  enabled_ = cfg.enabled;
  if (!enabled_) return Status::OK();
  if (cfg.dir.empty()) return Status::Err(Code::kInvalid, "resume dir is empty");

  const std::string dir = cfg.dir + "/" + safe_name(table) + "-" + hex64(fnv1a(fingerprint));
  std::error_code ec;
  if (cfg.reset) std::filesystem::remove_all(dir, ec);
  ec.clear();
  std::filesystem::create_directories(dir, ec);
  if (ec) return Status::Err(Code::kIO, "cannot create resume dir: " + dir);

  lock_fd_ = mmv_open((dir + "/lock").c_str(), MMV_O_RDWR | MMV_O_CREAT, 0644);
  if (lock_fd_ < 0)
    return Status::Err(Code::kIO, "cannot open resume lock: " + dir + "/lock");
  if (!mmv_lock(lock_fd_)) {
    mmv_close(lock_fd_);
    lock_fd_ = -1;
    return Status::Err(Code::kIO, "resume dir is already in use: " + dir);
  }

  manifest_path_ = dir + "/manifest.txt";
  checkpoint_path_ = dir + "/checkpoint.txt";

  if (std::filesystem::exists(manifest_path_)) {
    Status st = load_manifest(manifest_path_, table, fingerprint, out_ranges);
    if (!st.ok()) return st;
  } else {
    Status st = write_manifest(manifest_path_, table, fingerprint, planned);
    if (!st.ok()) return st;
  }

  Status cs = load_checkpoint();
  if (!cs.ok()) return cs;

  fd_ = mmv_open(checkpoint_path_.c_str(), MMV_O_WRONLY | MMV_O_CREAT | MMV_O_APPEND, 0644);
  if (fd_ < 0) return Status::Err(Code::kIO, "cannot open checkpoint: " + checkpoint_path_);
  unsynced_ = 0;
  last_sync_ = std::chrono::steady_clock::now();
  gc_old_dirs(cfg.dir, safe_name(table) + "-", dir, cfg.gc);
  return Status::OK();
}

namespace {

void gc_old_dirs(const std::string& base, const std::string& prefix,
                 const std::string& keep, int keep_count) {
  if (keep_count <= 0) return;
  std::vector<std::filesystem::directory_entry> dirs;
  std::error_code ec;
  auto it = std::filesystem::directory_iterator(base, ec);
  for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    std::error_code de;
    if (!it->is_directory(de) || de) continue;
    const std::string name = it->path().filename().string();
    if (name.rfind(prefix, 0) != 0) continue;
    dirs.push_back(*it);
  }
  std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
    std::error_code ea, eb;
    return std::filesystem::last_write_time(a.path(), ea) >
           std::filesystem::last_write_time(b.path(), eb);
  });
  int kept = 0;
  for (const auto& d : dirs) {
    if (d.path().string() == keep) { ++kept; continue; }
    if (kept < keep_count) { ++kept; continue; }
    std::error_code rm;
    std::filesystem::remove_all(d.path(), rm);
  }
}

}  // namespace

Status ResumeStore::load_checkpoint() {
  std::ifstream in(checkpoint_path_);
  if (!in) return Status::OK();
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty() || lines[i].rfind("ok ", 0) != 0) continue;
    std::istringstream ss(lines[i]);
    std::string tag;
    size_t idx = 0;
    int64_t ns = 0, nt = 0;
    if (!(ss >> tag >> idx >> ns >> nt)) {
      // A crash can leave one partial trailing record; ignore only that.
      if (i + 1 == lines.size()) continue;
      return Status::Err(Code::kInvalid,
                         "corrupt checkpoint line " + std::to_string(i + 1));
    }
    std::string sum;
    if (ss >> sum) {
      const std::string payload = tag + " " + std::to_string(idx) + " " +
                                  std::to_string(ns) + " " + std::to_string(nt);
      if (sum != hex64(fnv1a(payload))) {
        // A torn final write is recoverable; corruption in the middle is not.
        if (i + 1 == lines.size()) continue;
        return Status::Err(Code::kInvalid,
                           "checkpoint checksum mismatch at line " +
                               std::to_string(i + 1));
      }
    }
    done_.insert(idx);
    done_rows_[idx] = {ns, nt};
  }
  return Status::OK();
}

bool ResumeStore::done(size_t index) {
  if (!enabled_) return false;
  std::lock_guard<std::mutex> lk(mu_);
  return done_.find(index) != done_.end();
}

bool ResumeStore::done_rows(size_t index, int64_t& n_src, int64_t& n_tgt) {
  if (!enabled_) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = done_rows_.find(index);
  if (it == done_rows_.end()) return false;
  n_src = it->second.first;
  n_tgt = it->second.second;
  return true;
}

void ResumeStore::mark(size_t index, bool ok, int64_t n_src, int64_t n_tgt) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lk(mu_);
  const std::string payload = (ok ? "ok " : "fail ") + std::to_string(index) + " " +
                              std::to_string(n_src) + " " + std::to_string(n_tgt);
  const std::string line = payload + " " + hex64(fnv1a(payload)) + "\n";
  const long long wrote = fd_ >= 0 ? mmv_write(fd_, line.data(), line.size()) : -1;
  if (wrote != (ssize_t)line.size()) {
    std::fprintf(stderr, "[resume] checkpoint write failed: %s\n", std::strerror(errno));
  } else {
    ++unsynced_;
    const auto now = std::chrono::steady_clock::now();
    if (unsynced_ >= 32 || now - last_sync_ >= std::chrono::milliseconds(250)) {
      mmv_fsync(fd_);
      unsynced_ = 0;
      last_sync_ = now;
    }
  }
  if (ok) {
    done_.insert(index);
    done_rows_[index] = {n_src, n_tgt};
  }
}

}  // namespace mmv
