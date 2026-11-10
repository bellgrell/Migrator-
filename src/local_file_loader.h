#pragma once

#include "migrator.h"

#include <fstream>
#include <string>

namespace mig {

// Owns the temporary TSV used by MySQL LOAD DATA LOCAL INFILE: path, stream,
// escaping and pending-row count.  Keeps MySqlTarget focused on SQL/commit.
class LocalFileLoader {
 public:
  void set_path(std::string path) { path_ = std::move(path); }
  const std::string& path() const { return path_; }
  bool is_open() const { return out_.is_open(); }
  void open() { out_.open(path_, std::ios::binary | std::ios::trunc); }
  void write_row(const Row& row);
  void close_file();
  void remove_file();
  void reset();
  int64_t pending() const { return pending_; }

  static std::string escape(const std::string& s);
  static std::string field(const Value& v);

 private:
  std::string path_;
  std::ofstream out_;
  int64_t pending_ = 0;
};

}  // namespace mig
