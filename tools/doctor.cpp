// mm-doctor：mini-migrator 配套环境自检工具
// 检查项：连通性、认证、版本、wal_level、复制槽/插件、权限、
//         关键参数、binlog、面板端口，并给出可复制的修复命令。
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "dsn_secret.h"

#ifdef HAVE_PG
#include <libpq-fe.h>
#endif
#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#endif

namespace {

int g_fail = 0, g_warn = 0, g_ok = 0;

void ok(const std::string& label, const std::string& detail = "") {
  ++g_ok;
  std::printf("[ OK ] %-28s %s\n", label.c_str(), detail.c_str());
}
void warn(const std::string& label, const std::string& detail,
          const std::string& fix = "") {
  ++g_warn;
  std::printf("[WARN] %-28s %s\n", label.c_str(), detail.c_str());
  if (!fix.empty()) std::printf("        修复: %s\n", fix.c_str());
}
void fail(const std::string& label, const std::string& detail,
          const std::string& fix = "") {
  ++g_fail;
  std::printf("[FAIL] %-28s %s\n", label.c_str(), detail.c_str());
  if (!fix.empty()) std::printf("        修复: %s\n", fix.c_str());
}
void info(const std::string& label, const std::string& detail) {
  std::printf("[INFO] %-28s %s\n", label.c_str(), detail.c_str());
}

std::string trim(std::string s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}

struct Dsn {
  std::map<std::string, std::string> kv;
  void parse(const std::string& s) {
    size_t p = 0;
    while (p <= s.size()) {
      size_t c = s.find(';', p);
      std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
      size_t eq = item.find('=');
      if (eq != std::string::npos)
        kv[trim(item.substr(0, eq))] = trim(item.substr(eq + 1));
      if (c == std::string::npos) break;
      p = c + 1;
    }
  }
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
  bool has(const std::string& k) const { return kv.count(k) > 0; }
};

std::string quote_lit(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

double tcp_probe(const std::string& host, const std::string& port, std::string& err) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
    err = "无法解析主机";
    return -1;
  }
  double best = -1;
  for (auto* p = res; p; p = p->ai_next) {
    int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    struct timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    auto t0 = std::chrono::steady_clock::now();
    int rc = connect(fd, p->ai_addr, p->ai_addrlen);
    auto t1 = std::chrono::steady_clock::now();
    if (rc == 0) {
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (best < 0 || ms < best) best = ms;
    } else {
      err = std::strerror(errno);
    }
    close(fd);
  }
  freeaddrinfo(res);
  return best;
}

bool port_busy(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool busy = bind(fd, (sockaddr*)&a, sizeof a) != 0;
  close(fd);
  return busy;
}

#ifdef HAVE_PG
std::string pg_get(PGconn* c, const std::string& sql) {
  PGresult* r = PQexec(c, sql.c_str());
  if (!r) return "";
  std::string out;
  if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
    out = PQgetvalue(r, 0, 0);
  PQclear(r);
  return out;
}

void pg_value(PGconn* c, const std::string& sql, const std::string& label,
              const std::string& good, const std::string& fix) {
  PGresult* r = PQexec(c, sql.c_str());
  std::string v;
  if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0)
    v = PQgetvalue(r, 0, 0);
  if (r) PQclear(r);
  if (v == good) ok(label, v);
  else warn(label, v.empty() ? "(查询失败)" : v, fix);
}

void check_pg(const Dsn& d, const std::string& kind) {
  (void)kind;
  const std::string host = d.get("host", "127.0.0.1");
  const std::string port = d.get("port", "5432");
  const std::string user = d.get("user", "postgres");
  const std::string db = d.get("db", "postgres");
  const std::string pw = mig::resolve_secret(d.get("password"));

  std::string ci = "host=" + host + " port=" + port + " user=" + user +
                   " dbname=" + db + " connect_timeout=5";
  if (!pw.empty()) ci += " password=" + pw;
  PGconn* c = PQconnectdb(ci.c_str());
  if (!c || PQstatus(c) != CONNECTION_OK) {
    std::string e = c ? PQerrorMessage(c) : "PQconnectdb failed";
    while (!e.empty() && (e.back() == '\n' || e.back() == ' ')) e.pop_back();
    if (e.find("locked") != std::string::npos)
      fail("认证/账号", e, "gsql -c \"ALTER USER " + user + " ACCOUNT UNLOCK;\"");
    else
      fail("认证", e, "检查用户名/密码、pg_hba.conf、DSN 中的 password=");
    if (c) PQfinish(c);
    return;
  }
  ok("认证", user + " @" + db);

  info("版本", pg_get(c, "select version()").substr(0, 60));
  if (!d.has("db")) warn("DSN db", "未指定 db=，实际连到 " + db, "DSN 显式加 db=<目标库>");
  else ok("DSN db", db);

  const std::string wal = pg_get(c, "show wal_level");
  if (wal == "logical") ok("wal_level", wal);
  else warn("wal_level", wal, "ALTER SYSTEM SET wal_level='logical'; 然后重启实例");

  pg_value(c, "select setting from pg_settings where name='max_replication_slots'",
           "max_replication_slots", "10", "调大到 >= 10");
  pg_value(c, "select setting from pg_settings where name='max_connections'",
           "max_connections", "200", "");

  const std::string role = pg_get(c, "select current_user");
  const std::string super = pg_get(
      c, "select case when rolsuper then 't' else 'f' end from pg_roles where rolname=" +
             quote_lit(role));
  if (super == "t") ok("权限", "superuser");
  else {
    const std::string sysadmin = pg_get(
        c, "select case when rolsystemadmin then 't' else 'f' end from pg_roles where rolname=" +
               quote_lit(role));
    if (sysadmin == "t") ok("权限", "SYSADMIN");
    else warn("权限", "非 superuser/SYSADMIN", "CDC 与部分 DDL 需要更高权限");
  }

  // 插件探测：建一个临时槽再删，可同时验证 wal_level 与插件
  const std::string slot = "mm_doctor_probe";
  PGresult* r = PQexec(c,
      ("select pg_create_logical_replication_slot(" + quote_lit(slot) + ", 'wal2json')").c_str());
  if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
    ok("wal2json", "可加载");
    PQexec(c, ("select pg_drop_replication_slot(" + quote_lit(slot) + ")").c_str());
  } else {
    std::string e = r ? PQresultErrorMessage(r) : PQerrorMessage(c);
    while (!e.empty() && (e.back() == '\n' || e.back() == ' ')) e.pop_back();
    fail("wal2json", e, "安装 wal2json 或改用 test_decoding");
  }
  if (r) PQclear(r);

  const std::string slots = pg_get(c, "select count(*) from pg_replication_slots");
  if (slots == "0") ok("复制槽", "无残留槽");
  else warn("复制槽", slots + " 个残留槽", "select slot_name, active from pg_replication_slots;");
  PQfinish(c);
}
#endif

#ifdef HAVE_MYSQL
void check_mysql(const Dsn& d) {
  const std::string host = d.get("host", "127.0.0.1");
  const std::string port = d.get("port", "3306");
  const std::string user = d.get("user", "root");
  const std::string db = d.get("db");
  const std::string pw = mig::resolve_secret(d.get("password"));

  MYSQL* m = mysql_init(nullptr);
  unsigned int to = 5;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &to);
  if (!mysql_real_connect(m, host.c_str(), user.c_str(), pw.c_str(),
                          db.empty() ? nullptr : db.c_str(),
                          (unsigned)std::stoi(port), nullptr, 0)) {
    fail("认证", mysql_error(m), "检查用户/密码与授权");
    mysql_close(m);
    return;
  }
  ok("认证", user + (db.empty() ? "" : " @" + db));
  info("版本", std::string(mysql_get_server_info(m)));

  auto q = [&](const char* sql) -> std::string {
    if (mysql_query(m, sql) != 0) return "";
    MYSQL_RES* res = mysql_store_result(m);
    if (!res) return "";
    MYSQL_ROW row = mysql_fetch_row(res);
    std::string v = (row && row[0]) ? row[0] : "";
    mysql_free_result(res);
    return v;
  };
  const std::string log_bin = q("show variables like 'log_bin'");
  (void)log_bin;
  if (mysql_query(m, "show variables like 'log_bin'") == 0) {
    MYSQL_RES* res = mysql_store_result(m);
    MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
    std::string v = (row && row[1]) ? row[1] : "";
    if (res) mysql_free_result(res);
    if (v == "ON") ok("binlog", "ON");
    else warn("binlog", v.empty() ? "(未知)" : v, "开启 log_bin 才能做 CDC");
  }
  if (mysql_query(m, "show variables like 'binlog_format'") == 0) {
    MYSQL_RES* res = mysql_store_result(m);
    MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
    std::string v = (row && row[1]) ? row[1] : "";
    if (res) mysql_free_result(res);
    if (v == "ROW") ok("binlog_format", v);
    else warn("binlog_format", v.empty() ? "(未知)" : v, "设为 ROW");
  }
  mysql_close(m);
}
#endif

void usage() {
  std::printf(
      "mm-doctor - mini-migrator 环境自检\n\n"
      "用法:\n"
      "  mm-doctor --source <dsn> --target <dsn> [--source-type mysql|pg|og]"
      " [--target-type mysql|pg|og] [--mode <mode>]\n\n"
      "示例:\n"
      "  mm-doctor --source \"host=127.0.0.1;port=3306;user=root;db=src\" \\\n"
      "            --target \"host=10.0.0.5;port=5432;user=migrator;db=dst\" \\\n"
      "            --source-type mysql --target-type og\n");
}

}  // namespace

int main(int argc, char** argv) {
  Dsn src, tgt;
  std::string st, tt, mode;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--source") src.parse(next());
    else if (a == "--target") tgt.parse(next());
    else if (a == "--source-type") st = next();
    else if (a == "--target-type") tt = next();
    else if (a == "--mode") mode = next();
    else if (a == "-h" || a == "--help") { usage(); return 0; }
  }
  if (st.empty() || tt.empty()) {
    if (!mode.empty()) {
      size_t p = mode.find('2');
      if (p != std::string::npos) {
        if (st.empty()) st = mode.substr(0, p);
        if (tt.empty()) tt = mode.substr(p + 1);
      }
    }
  }
  if ((src.kv.empty() && tgt.kv.empty()) || st.empty() || tt.empty()) {
    usage();
    return 2;
  }

  std::printf("mm-doctor  源=%s  目标=%s\n", st.c_str(), tt.c_str());
  auto plain_secret = [](const Dsn& d) {
    if (!d.has("password")) return false;
    const std::string v = d.get("password");
    return v.rfind("env:", 0) != 0 && v.rfind("file:", 0) != 0;
  };
  if (plain_secret(src)) warn("源 DSN", "含明文密码", "改用 password=env:NAME 或 file:PATH");
  if (plain_secret(tgt)) warn("目标 DSN", "含明文密码", "改用 password=env:NAME 或 file:PATH");
  std::printf("----------------------------------------\n");

  auto probe = [](const Dsn& d, const std::string& kind, const char* label) {
    const std::string host = d.get("host", "127.0.0.1");
    const std::string port = d.get("port", kind == "mysql" ? "3306" : "5432");
    std::string err;
    double ms = tcp_probe(host, port, err);
    if (ms < 0) fail(std::string(label) + " 连通性", host + ":" + port + " " + err,
                     "检查网络/防火墙/端口映射");
    else info(std::string(label) + " 延迟", host + ":" + port + " " +
                  std::to_string((int)ms) + " ms");
  };

  info("项目", "检查开始");
  probe(src, st, "源");
  probe(tgt, tt, "目标");

  if (st == "pg" || st == "og") {
#ifdef HAVE_PG
    std::printf("-- 源（%s）--\n", st.c_str());
    check_pg(src, st);
#else
    warn("源", "编译时未启用 libpq", "make PG_PREFIX=...");
#endif
  } else if (st == "mysql") {
#ifdef HAVE_MYSQL
    std::printf("-- 源（mysql）--\n");
    check_mysql(src);
#else
    warn("源", "编译时未启用 MySQL 客户端", "make MYSQL_PREFIX=...");
#endif
  }

  if (tt == "pg" || tt == "og") {
#ifdef HAVE_PG
    std::printf("-- 目标（%s）--\n", tt.c_str());
    check_pg(tgt, tt);
#else
    warn("目标", "编译时未启用 libpq", "");
#endif
  } else if (tt == "mysql") {
#ifdef HAVE_MYSQL
    std::printf("-- 目标（mysql）--\n");
    check_mysql(tgt);
#else
    warn("目标", "编译时未启用 MySQL 客户端", "");
#endif
  }

  if (port_busy(8123)) warn("面板端口", "8123 已被占用", "改用 --port 或先停止占用进程");
  else ok("面板端口", "8123 可用");

  std::printf("----------------------------------------\n");
  std::printf("通过 %d / 警告 %d / 失败 %d\n", g_ok, g_warn, g_fail);
  return g_fail > 0 ? 2 : 0;
}
