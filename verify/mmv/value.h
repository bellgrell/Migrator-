#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mmv {

enum class ValType { kNull = 0, kInt, kDouble, kString, kBool, kBytes };

struct Value {
  ValType type = ValType::kNull;
  int64_t i = 0;
  double d = 0.0;
  bool b = false;
  std::string s;  // kString or raw bytes

  static Value Null() { return Value(); }
  static Value Int(int64_t v) { Value x; x.type = ValType::kInt; x.i = v; return x; }
  static Value Dbl(double v) { Value x; x.type = ValType::kDouble; x.d = v; return x; }
  static Value Str(std::string v) { Value x; x.type = ValType::kString; x.s = std::move(v); return x; }
  static Value Bool(bool v) { Value x; x.type = ValType::kBool; x.b = v; return x; }
  static Value Bytes(std::string raw) { Value x; x.type = ValType::kBytes; x.s = std::move(raw); return x; }
  bool is_null() const { return type == ValType::kNull; }
};

using Row = std::vector<Value>;

// Query result with text cells; NULL is tracked separately so that NULL and
// the empty string never collapse into each other.
struct Cell {
  bool is_null = true;
  std::string text;
};

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
};

}  // namespace mmv
