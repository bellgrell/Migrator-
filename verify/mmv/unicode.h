#pragma once

#include <string>

namespace mmv {

// Full Unicode NFD decomposition + combining-mark removal, with optional
// Unicode case folding. Requires ICU; callers should guard with HAVE_ICU.
bool unicode_available();
std::string unicode_fold(const std::string& s, bool casefold);

// ICU collation sort key as lower-case hex. strength: 0=primary,
// 1=secondary, 2=tertiary, 3=quaternary.
std::string unicode_collation_key(const std::string& s, const std::string& locale,
                                  int strength);

}  // namespace mmv
