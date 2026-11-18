#include "checksum_normalizer.h"

#include <cstdio>
#include <string>

namespace mig {

std::string normalize_decimal_text(std::string s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  const size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  s = s.substr(b, e - b + 1);
  bool neg = false;
  if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
    neg = (s[0] == '-');
    s.erase(0, 1);
  }
  const size_t dot = s.find('.');
  if (dot != std::string::npos) {
    size_t last = s.find_last_not_of('0');
    if (last == std::string::npos) {
      s = "0";
    } else if (last > dot) {
      s.erase(last + 1);
    } else if (last == dot) {
      s.erase(dot);
    }
  }
  const size_t nz = s.find_first_not_of('0');
  if (nz == std::string::npos) {
    s = "0";
  } else if (nz > 0 && s[nz] != '.') {
    s.erase(0, nz);
  }
  if (s.empty()) s = "0";
  if (neg && s != "0") s = "-" + s;
  return s;
}

std::string normalize_value(const Value& v) {
  switch (v.type) {
    case ValType::kNull: return std::string("\x01", 1);
    case ValType::kInt: return std::string("\x07", 1) + std::to_string(v.i);
    case ValType::kDouble: {
      char buf[64];
      // Cross-engine FLOAT/DOUBLE text differs: MySQL's text protocol gives
      // only 6 significant digits for FLOAT, while openGauss can expose the
      // full float32. Six significant digits is the common precision both
      // sides share for FLOAT; DOUBLE comparisons still catch larger drift.
      std::snprintf(buf, sizeof(buf), "%.6g", v.d);
      std::string s(buf);
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      if (s == "-0") s = "0";
      return std::string("\x07", 1) + normalize_decimal_text(s);
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
    case ValType::kString: {
      if (v.dec) return std::string("\x07", 1) + normalize_decimal_text(v.s);
      // JSON is delivered as text; ignore insignificant whitespace so MySQL
      // JSON and openGauss jsonb hash the same bytes. Key ordering is still
      // significant (a full canonicalizer is future work).
      size_t b = 0;
      while (b < v.s.size() && (v.s[b] == ' ' || v.s[b] == '\t' ||
                                v.s[b] == '\n' || v.s[b] == '\r'))
        ++b;
      if (b < v.s.size() && (v.s[b] == '{' || v.s[b] == '[')) {
        std::string out;
        bool in_str = false, esc = false;
        for (char c : v.s) {
          if (in_str) {
            out.push_back(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
          } else {
            if (c == '"') { in_str = true; out.push_back(c); }
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            else out.push_back(c);
          }
        }
        return std::string("\x06", 1) + out;
      }
      return std::string("\x06", 1) + v.s;
    }
    default:
      return std::string("\x06", 1) + v.s;
  }
}

}  // namespace mig
