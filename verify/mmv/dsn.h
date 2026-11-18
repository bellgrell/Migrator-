#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

namespace mmv {

// password=env:NAME / password=file:PATH / plain text (warned about by caller).
inline std::string resolve_secret(const std::string& v) {
  if (v.rfind("env:", 0) == 0) {
    const char* e = std::getenv(v.substr(4).c_str());
    return e ? std::string(e) : std::string();
  }
  if (v.rfind("file:", 0) == 0) {
    std::string path = v.substr(5);
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
      const char* home = std::getenv("HOME");
      if (home) path = std::string(home) + path.substr(1);
    }
    std::ifstream in(path);
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

struct Dsn {
  std::string raw;

  std::string get(const std::string& key, const std::string& def = "") const {
    std::string marker = key + "=";
    size_t pos = 0;
    while ((pos = raw.find(marker, pos)) != std::string::npos) {
      if (pos == 0 || raw[pos - 1] == ';' || raw[pos - 1] == ' ') {
        const size_t b = pos + marker.size();
        const size_t e = raw.find(';', b);
        std::string v = raw.substr(b, e == std::string::npos ? std::string::npos : e - b);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
        return v;
      }
      pos += marker.size();
    }
    return def;
  }

  std::string host() const { return get("host", "127.0.0.1"); }
  std::string port() const { return get("port", ""); }
  std::string user() const { return get("user", get("username", "")); }
  std::string password_resolved() const { return resolve_secret(get("password", "")); }
  bool password_is_plain() const {
    const std::string p = get("password", "");
    return !p.empty() && p.rfind("env:", 0) != 0 && p.rfind("file:", 0) != 0;
  }
  std::string dbname() const { return get("dbname", get("db", get("database", ""))); }
};

}  // namespace mmv
