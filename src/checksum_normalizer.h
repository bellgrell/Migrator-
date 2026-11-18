#pragma once

#include "migrator.h"

#include <string>

namespace mig {

// Cross-database canonical form used by checksum verification.
// PostgreSQL bools, MySQL tinyint(1), numeric trailing zeros and binary
// values are normalized here so both sides hash the same bytes.
std::string normalize_value(const Value& v);

// Canonical decimal text: no trailing fractional zeros, no leading zeros,
// no "-0". Used by DECIMAL/NUMERIC columns before FNV hashing.
std::string normalize_decimal_text(std::string s);

}  // namespace mig
