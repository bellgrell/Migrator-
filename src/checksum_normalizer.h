#pragma once

#include "migrator.h"

#include <string>

namespace mig {

// Cross-database canonical form used by checksum verification.
// PostgreSQL bools, MySQL tinyint(1), numeric trailing zeros and binary
// values are normalized here so both sides hash the same bytes.
std::string normalize_value(const Value& v);

}  // namespace mig
