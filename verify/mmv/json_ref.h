#pragma once

#include <string>

namespace mmv {

// Client-side JSON reference canonicalizer.
//
// Produces the same path/type/length/value stream as sql_json_canonical():
// entries are sorted by path byte order and joined with '|'. Returns false and
// fills err on malformed JSON or unsupported nesting.
bool json_canonical_ref(const std::string& input, std::string& out, std::string& err,
                         bool* overflow = nullptr);

}  // namespace mmv
