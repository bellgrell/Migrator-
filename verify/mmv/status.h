#pragma once

#include <string>
#include <utility>

namespace mmv {

enum class Code { kOk = 0, kInvalid, kUnsupported, kDb, kIO, kMismatch };

struct Status {
  Code code = Code::kOk;
  std::string msg;
  Status() = default;
  Status(Code c, std::string m) : code(c), msg(std::move(m)) {}
  bool ok() const { return code == Code::kOk; }
  static Status OK() { return Status(); }
  static Status Err(Code c, std::string m) { return Status(c, std::move(m)); }
};

inline const char* code_name(Code c) {
  switch (c) {
    case Code::kOk: return "ok";
    case Code::kInvalid: return "invalid";
    case Code::kUnsupported: return "unsupported";
    case Code::kDb: return "db";
    case Code::kIO: return "io";
    case Code::kMismatch: return "mismatch";
  }
  return "?";
}

}  // namespace mmv
