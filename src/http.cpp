#include "migrator.h"
#include "module.h"

#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { ::closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
#endif

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace mig {
namespace {

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

void send_all(socket_t fd, const std::string& data) {
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
#if defined(_WIN32)
    const int n = ::send(fd, p, (int)left, 0);
#else
    const ssize_t n = ::send(fd, p, left, 0);
#endif
    if (n <= 0) return;
    p += n;
    left -= (size_t)n;
  }
}

std::string status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 400: return "Bad Request";
    default: return "OK";
  }
}

void handle(socket_t fd, Metrics& m) {
  char buf[8192];
#if defined(_WIN32)
  const int n = ::recv(fd, buf, (int)sizeof(buf) - 1, 0);
#else
  const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
#endif
  if (n <= 0) return;
  buf[n] = 0;

  std::string req(buf);
  std::string method = "GET";
  std::string path = "/";
  {
    const size_t sp1 = req.find(' ');
    if (sp1 != std::string::npos) {
      method = req.substr(0, sp1);
      const size_t sp2 = req.find(' ', sp1 + 1);
      if (sp2 != std::string::npos) path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }
  }
  std::string req_body;
  {
    const size_t hdr = req.find("\r\n\r\n");
    if (hdr != std::string::npos) req_body = req.substr(hdr + 4);
  }
  const size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);

  int code = 200;
  std::string body;
  std::string ctype = "application/json; charset=utf-8";

  if (path == "/" || path == "/index.html") {
    ctype = "text/html; charset=utf-8";
    if (!read_file("web/index.html", body)) {
      body =
          "<html><body style='font-family:monospace;background:#0b1020;color:#e6edf3'>"
          "<h2>web/index.html not found</h2>"
          "<p>请从项目根目录启动（需要 web/index.html 与 config/）。</p></body></html>";
    }
  } else if (path == "/api/status") {
    body = m.status_json();
  } else if (path == "/api/tables") {
    body = m.tables_json();
  } else if (path == "/api/events") {
    body = m.events_json();
  } else if (path == "/api/series") {
    body = m.series_json();
  } else if (path == "/api/modules") {
    ctype = "application/json; charset=utf-8";
    if (method == "POST") {
      std::string b = req_body;
      const size_t s = b.find_first_not_of(" \t\r\n");
      if (s != std::string::npos) b = b.substr(s);
      while (!b.empty() && (b.back() == '\n' || b.back() == '\r' || b.back() == ' '))
        b.pop_back();
      if (b.size() < 2 || b.front() != '{' || b.back() != '}') {
        code = 400;
        body = "{\"error\":\"bad json\"}";
      } else {
        std::ofstream out("config/modules.json", std::ios::trunc);
        out << b;
        body = "{\"ok\":true,\"restart_required\":true}";
      }
    } else {
      std::string cfg;
      std::ifstream in("config/modules.json");
      if (in) { std::ostringstream ss; ss << in.rdbuf(); cfg = ss.str(); }
      auto on = [&](const char* n, bool def) {
        const std::string key = std::string("\"") + n + "\"";
        const size_t p = cfg.find(key);
        if (p == std::string::npos) return def;
        const size_t c = cfg.find(':', p + key.size());
        if (c == std::string::npos) return def;
        const size_t v = cfg.find_first_not_of(" \t\r\n", c + 1);
        if (v == std::string::npos) return def;
        return !(cfg.compare(v, 5, "false") == 0 || cfg[v] == '0');
      };
      body = "{\"pg_cdc\":";
      body += on("pg_cdc", true) ? "true" : "false";
      body += ",\"repair\":";
      body += on("repair", true) ? "true" : "false";
      body += ",\"metrics\":";
      body += on("metrics", true) ? "true" : "false";
      body += ",\"snapshot\":";
      body += on("snapshot", true) ? "true" : "false";
      body += ",\"resume\":";
      body += on("resume", true) ? "true" : "false";
      body += "}";
    }
  } else if (path == "/api/health") {
    body = "{\"ok\":true}";
  } else if (path == "/api/features") {
    body = "{";
    bool first = true;
    for (const auto* mod : ModuleManager::instance().all()) {
      if (!first) body += ",";
      first = false;
      body += "\"" + mod->name() + "\":" +
              (mod->compiled() ? "true" : "false");
    }
    body += "}";
  } else if (path == "/metrics") {
    if (!m.metrics_enabled.load()) {
      code = 404;
      body = "{\"error\":\"metrics module disabled\"}";
    } else {
#ifdef MIG_ENABLE_METRICS
    ctype = "text/plain; version=0.0.4; charset=utf-8";
    body = m.prometheus();
#else
    code = 404;
    body = "{\"error\":\"metrics module not built\"}";
#endif
    }
  } else {
    code = 404;
    body = "{\"error\":\"not found\"}";
  }

  std::ostringstream resp;
  resp << "HTTP/1.1 " << code << " " << status_text(code) << "\r\n"
       << "Content-Type: " << ctype << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  send_all(fd, resp.str());
}

}  // namespace

int run_http_server(int port, Metrics& m) {
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#else
  ::signal(SIGPIPE, SIG_IGN);
#endif

  socket_t sfd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == kInvalidSocket) return 1;

  int yes = 1;
  ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)port);

  if (::bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    close_socket(sfd);
    return 2;
  }
  if (::listen(sfd, 64) < 0) {
    close_socket(sfd);
    return 3;
  }

  while (true) {
    socket_t c = ::accept(sfd, nullptr, nullptr);
    if (c == kInvalidSocket) continue;
    handle(c, m);
    close_socket(c);
  }
  return 0;
}

}  // namespace mig
