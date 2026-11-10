#pragma once
#include <cstdlib>
#include <fstream>
#include <string>

namespace mig {

// 解析密码来源：
//   env:NAME    从环境变量读取
//   file:PATH   从文件首行读取（K8s Secret 挂载常用）
//   其他        视为明文
inline std::string resolve_secret(const std::string& v) {
  if (v.rfind("env:", 0) == 0) {
    const char* e = std::getenv(v.substr(4).c_str());
    return e ? std::string(e) : std::string();
  }
  if (v.rfind("file:", 0) == 0) {
    std::ifstream in(v.substr(5));
    std::string line;
    if (in && std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
      return line;
    }
    return std::string();
  }
  return v;
}

}  // namespace mig
