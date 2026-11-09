#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "migrator.h"

namespace mig {

struct CdcEvent {
  std::string table;
  int op = 0;  // 0=insert 1=update 2=delete
  std::vector<Value> after;
  std::vector<Value> before;
  bool is_ddl = false;
  std::string ddl;
};

class BinlogReader {
 public:
  BinlogReader();
  ~BinlogReader();

  Status open(const Options& o, const std::string& file, uint64_t pos);
  Status next(bool& has_event, std::vector<CdcEvent>& events);
  void close();

  std::string file() const { return file_; }
  uint64_t pos() const { return pos_; }
  void set_file(const std::string& f) { file_ = f; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::string file_;
  uint64_t pos_ = 0;
};

Status apply_cdc_event(ITarget& tgt, const TableSchema& s, const CdcEvent& ev);

Status run_cdc(const Options& o, Metrics& m, int max_events = 0);
Status run_pg_cdc(const Options& o, Metrics& m, int max_events = 0);

}  // namespace mig
