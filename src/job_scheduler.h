#pragma once

#include "migrator.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mig {

struct TableJob {
  std::string name;
  TableSchema schema;
  std::string ddl;
  int64_t rows = 0;
  bool rebuild = false;
  std::string rebuild_sql;
};

// Builds the ordered copy/rebuild job list from the source schema and table
// policies.  Kept out of pipeline.cpp so scheduling policy and table
// preparation can evolve independently.
class JobScheduler {
 public:
  static bool prepare(ISource& src, ITarget& tgt, const TypeMap& tm,
                      const Options& o,
                      const std::unordered_map<std::string, TablePolicy>& policies,
                      Metrics& m, std::vector<TableJob>& jobs,
                      std::vector<TableJob>& rebuilds);
};

}  // namespace mig
