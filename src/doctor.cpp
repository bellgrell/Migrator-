#include "doctor.h"
#include "providers.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#endif
#ifdef HAVE_PG
#include <libpq-fe.h>
#endif
#ifdef HAVE_ODBC
#include <sql.h>
#include <sqlext.h>
#endif

namespace mig {
namespace {

struct Item {
  std::string name;
  std::string status;  // OK / WARN / FAIL
  std::string detail;
};

void add(std::vector<Item>& v, const std::string& n, const std::string& s,
         const std::string& d) {
  v.push_back(Item{n, s, d});
}

std::string run_capture(const std::string& cmd) {
  std::string out;
  FILE* p = popen((cmd + " 2>&1").c_str(), "r");
  if (!p) return out;
  char buf[512];
  while (fgets(buf, sizeof buf, p)) {
    out += buf;
    if (out.size() > 4096) break;
  }
  pclose(p);
  return out;
}

std::string first_line(std::string s) {
  const size_t p = s.find('\n');
  if (p != std::string::npos) s = s.substr(0, p);
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

bool exists(const std::string& p) { return access(p.c_str(), R_OK) == 0; }

std::string find_tool(const char* env, const std::string& home_rel) {
  const char* e = std::getenv(env);
  if (e && *e && access(e, X_OK) == 0) return e;
  std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
  std::string cand = home + home_rel;
  if (access(cand.c_str(), X_OK) == 0) return cand;
  return "";
}

}  // namespace

int run_doctor() {
  std::vector<Item> items;
  const Providers& pv = providers();
  {
    const char* e = std::getenv("MIG_PROVIDERS");
    std::string cfg = (e && *e) ? e : "config/providers.ini";
    if (!exists(cfg) && !(e && *e) && exists("/etc/mini-migrator/providers.ini"))
      cfg = "/etc/mini-migrator/providers.ini";
    add(items, "providers.ini", exists(cfg) ? "OK" : "WARN",
        exists(cfg) ? cfg : cfg + " (missing, using built-in defaults)");
    add(items, "mssql driver (cfg)", "OK", pv.mssql_driver);
    add(items, "oracle driver (cfg)", "OK", pv.oracle_driver);
  }

#ifdef MIG_VERSION
  add(items, "build", "OK", std::string("mini-migrator ") + MIG_VERSION);
#else
  add(items, "build", "OK", "mini-migrator");
#endif

#ifdef HAVE_MYSQL
  add(items, "mysql client", "OK", std::string(mysql_get_client_info()));
#else
  add(items, "mysql client", "WARN", "not compiled in (no HAVE_MYSQL)");
#endif

#ifdef HAVE_PG
  add(items, "libpq", "OK", std::to_string(PQlibVersion()));
#else
  add(items, "libpq", "WARN", "not compiled in (no HAVE_PG)");
#endif

#ifdef HAVE_ODBC
  {
    SQLHENV env = SQL_NULL_HANDLE;
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) == SQL_SUCCESS) {
      SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
      std::vector<std::string> drivers;
      SQLCHAR desc[256];
      SQLSMALLINT dlen = 0;
      SQLUSMALLINT dir = SQL_FETCH_FIRST;
      while (SQLDrivers(env, dir, desc, sizeof desc, &dlen, nullptr, 0, nullptr) ==
             SQL_SUCCESS) {
        drivers.emplace_back((char*)desc, (size_t)dlen);
        dir = SQL_FETCH_NEXT;
      }
      SQLFreeHandle(SQL_HANDLE_ENV, env);
      if (drivers.empty()) {
        add(items, "odbc drivers", "WARN", "none registered");
      } else {
        std::string list;
        for (const auto& d : drivers) list += (list.empty() ? "" : ", ") + d;
        add(items, "odbc drivers", "OK", list);
      }
      for (const char* need : {"ODBC Driver 18 for SQL Server",
                               "Oracle 23 ODBC Driver"}) {
        bool found = false;
        for (const auto& d : drivers) if (d.find(need) != std::string::npos) found = true;
        add(items, std::string("driver: ") + need, found ? "OK" : "WARN",
            found ? "registered" : "not found (optional, needed for that target)");
      }
    } else {
      add(items, "odbc", "FAIL", "cannot allocate ODBC env");
    }
  }
  const char* sysini = std::getenv("ODBCSYSINI");
  add(items, "ODBCSYSINI", sysini && *sysini ? "OK" : "WARN",
      sysini && *sysini ? sysini : "unset (system-wide /etc/odbcinst.ini will be used)");
  std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
  if (!home.empty()) {
    add(items, "~/.odbc/odbcinst.ini", exists(home + "/.odbc/odbcinst.ini") ? "OK" : "WARN",
        exists(home + "/.odbc/odbcinst.ini") ? "present" : "missing");
  }
#else
  add(items, "odbc", "WARN", "not compiled in (no HAVE_ODBC)");
#endif

  {
    std::string bcp = pv.bcp_path;
    if (bcp.empty() || access(bcp.c_str(), X_OK) != 0)
      bcp = find_tool("MIG_BCP", "/msodbc/tools/root/opt/mssql-tools18/bin/bcp");
    if (bcp.empty()) {
      add(items, "bcp", "WARN", "not found (set MIG_BCP; needed for MSSQL target)");
    } else {
      const std::string out = run_capture("'" + bcp + "' -v");
      const bool good = out.find("BCP -") != std::string::npos ||
                        out.find("Bulk Copy") != std::string::npos;
      const std::string v = first_line(out);
      add(items, "bcp", good ? "OK" : "WARN",
          bcp + " | " + (v.empty() ? out.substr(0, 120) : v));
    }
  }

  {
    std::string ld = pv.sqlldr_path;
    if (ld.empty() || access(ld.c_str(), X_OK) != 0)
      ld = find_tool("MIG_SQLLDR", "/oracle-ic/instantclient_23_26/sqlldr");
    if (ld.empty()) {
      add(items, "sqlldr", "WARN", "not found (set MIG_SQLLDR; needed for Oracle target)");
    } else {
      const std::string out = run_capture("'" + ld + "'");
      const size_t p = out.find("SQL*Loader");
      const bool good = p != std::string::npos;
      std::string v = good ? first_line(out.substr(p)) : first_line(out);
      add(items, "sqlldr", good ? "OK" : "WARN",
          ld + " | " + (v.empty() ? out.substr(0, 120) : v));
    }
  }

  std::printf("\n=== mini-migrator doctor ===\n");
  int fails = 0, warns = 0;
  for (const auto& it : items) {
    std::printf("[%-4s] %-28s %s\n", it.status.c_str(), it.name.c_str(),
                it.detail.c_str());
    if (it.status == "FAIL") ++fails;
    if (it.status == "WARN") ++warns;
  }
  std::printf("\n%d checks: %d ok, %d warn, %d fail\n",
              (int)items.size(), (int)items.size() - warns - fails, warns, fails);
  return fails > 0 ? 1 : 0;
}

}  // namespace mig
