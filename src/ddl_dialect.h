#pragma once

#include <memory>
#include <string>

namespace mig {

// DDL dialect used by CDC/DDL sync.  The binlog reader always sees MySQL DDL;
// the dialect converts it into the target database's dialect.
class IDdlDialect {
 public:
  virtual ~IDdlDialect() = default;
  virtual std::string convert(const std::string& ddl,
                              const std::string& source_db) = 0;
};

std::unique_ptr<IDdlDialect> make_ddl_dialect(const std::string& target_kind);

}  // namespace mig
