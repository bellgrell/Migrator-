#include "providers.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mig {
namespace {

std::string trim(std::string s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string env_or(const char* k, const std::string& def) {
  const char* v = std::getenv(k);
  return (v && *v) ? std::string(v) : def;
}

}  // namespace

Providers Providers::load(const std::string& path) {
  Providers p;
  std::string file = path;
  if (file.empty()) {
    const char* e = std::getenv("MIG_PROVIDERS");
    if (e && *e) {
      file = e;
    } else {
      file = "config/providers.ini";
      std::ifstream probe(file);
      if (!probe) file = "/etc/mini-migrator/providers.ini";
    }
  }
  std::ifstream in(file);
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line[0] == ';') continue;
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = trim(line.substr(0, eq));
      const std::string v = trim(line.substr(eq + 1));
      if (v.empty()) continue;
      if (k == "mssql_driver") p.mssql_driver = v;
      else if (k == "oracle_driver") p.oracle_driver = v;
      else if (k == "bcp_path") p.bcp_path = v;
      else if (k == "sqlldr_path") p.sqlldr_path = v;
    }
  }
  p.mssql_driver = env_or("MIG_MSSQL_DRIVER", p.mssql_driver);
  p.oracle_driver = env_or("MIG_ORACLE_DRIVER", p.oracle_driver);
  p.bcp_path = env_or("MIG_BCP", p.bcp_path);
  p.sqlldr_path = env_or("MIG_SQLLDR", p.sqlldr_path);
  return p;
}

const Providers& providers() {
  static const Providers p = Providers::load();
  return p;
}

}  // namespace mig
