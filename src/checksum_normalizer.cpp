#include "checksum_normalizer.h"

#include <cstdio>
#include <string>

namespace mig {

std::string normalize_value(const Value& v) {
  switch (v.type) {
    case ValType::kNull: return std::string("\x01", 1);
    case ValType::kInt: return std::string("\x02", 1) + std::to_string(v.i);
    case ValType::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.10f", v.d);
      std::string s(buf);
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      if (s == "-0") s = "0";
      return std::string("\x03", 1) + s;
    }
    case ValType::kBool: return std::string("\x02", 1) + (v.b ? "1" : "0");
    case ValType::kBytes: {
      static const char* hex = "0123456789abcdef";
      std::string out("\x05", 1);
      for (unsigned char c : v.s) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
      }
      return out;
    }
    case ValType::kString:
    default:
      return std::string("\x06", 1) + v.s;
  }
}

}  // namespace mig
