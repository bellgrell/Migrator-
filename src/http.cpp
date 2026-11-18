#include "http.h"

#include "logger.h"
#include "migrator.h"
#include "module.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { ::closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
#endif

#ifdef MIG_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace mig {
namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxRequestTarget = 2048;

// ---------------------------------------------------------------- utilities

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

std::string base64_decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    const int v = val(c);
    if (v < 0) return std::string();
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((char)((buf >> bits) & 0xff));
    }
  }
  return out;
}

std::string iso8601_utc() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

long long epoch_millis() {
  using namespace std::chrono;
  return (long long)duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

std::string make_request_id() {
  static std::atomic<uint64_t> counter{0};
  return std::to_string(epoch_millis()) + "-" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int h = hex(s[i + 1]), l = hex(s[i + 2]);
      if (h >= 0 && l >= 0) {
        out.push_back((char)((h << 4) | l));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

bool host_is_loopback(const std::string& host) {
  const std::string h = lower_ascii(trim(host));
  if (h.empty() || h == "localhost") return true;
  if (h == "::1") return true;
  if (h.rfind("127.", 0) == 0) return true;
  return false;
}

// ---------------------------------------------------------------- audit log

class AuditLog {
 public:
  bool open(const std::string& path, std::string& err) {
    path_ = path;
    if (path.empty()) {
      enabled_ = false;
      return true;
    }
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) {
      err = "cannot create audit directory " + parent.string() + ": " + ec.message();
      return false;
    }
    file_.open(path, std::ios::app | std::ios::binary);
    if (!file_) {
      err = "cannot open audit log: " + path;
      return false;
    }
    enabled_ = true;
    return true;
  }

  void write_line(const std::string& line) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mu_);
    file_ << line << '\n';
    file_.flush();
  }

  bool enabled() const { return enabled_; }
  const std::string& path() const { return path_; }

 private:
  std::mutex mu_;
  std::ofstream file_;
  std::string path_;
  bool enabled_ = false;
};

// Simple failed-auth throttle: 20 failures per source IP per 60 seconds.
class AuthLimiter {
 public:
  bool blocked(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& q = hits_[ip];
    const double now = now_sec();
    while (!q.empty() && now - q.front() > 60.0) q.pop_front();
    return q.size() >= 20;
  }
  void note_failure(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& q = hits_[ip];
    const double now = now_sec();
    while (!q.empty() && now - q.front() > 60.0) q.pop_front();
    q.push_back(now);
    if (hits_.size() > 4096) hits_.clear();
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::deque<double>> hits_;
};

// ---------------------------------------------------------------- transport

struct Conn {
  socket_t fd = kInvalidSocket;
#ifdef MIG_HAVE_OPENSSL
  SSL* ssl = nullptr;
#endif

  int read_some(char* buf, size_t n) {
#ifdef MIG_HAVE_OPENSSL
    if (ssl) {
      const int r = SSL_read(ssl, buf, (int)n);
      if (r > 0) return r;
      const int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_ZERO_RETURN) return 0;
      return -1;
    }
#endif
#if defined(_WIN32)
    return ::recv(fd, buf, (int)n, 0);
#else
    return (int)::recv(fd, buf, n, 0);
#endif
  }

  bool write_all(const std::string& data) {
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
      const size_t chunk = std::min<size_t>(left, 65536);
      int n = 0;
#ifdef MIG_HAVE_OPENSSL
      if (ssl) {
        n = SSL_write(ssl, p, (int)chunk);
      } else
#endif
      {
#if defined(_WIN32)
        n = ::send(fd, p, (int)chunk, 0);
#else
        n = (int)::send(fd, p, chunk, 0);
#endif
      }
      if (n <= 0) return false;
      p += n;
      left -= (size_t)n;
    }
    return true;
  }
};

// ---------------------------------------------------------------- request

struct Request {
  std::string method = "GET";
  std::string target = "/";
  std::string path = "/";
  std::string query;
  std::string version = "HTTP/1.1";
  std::map<std::string, std::string> headers;
  std::string body;
  std::string remote;
  bool head_only = false;
};

struct Response {
  int code = 200;
  std::string ctype = "application/json; charset=utf-8";
  std::string body;
  std::map<std::string, std::string> headers;
};

const char* status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

std::string header_value(const Request& r, const std::string& name) {
  const auto it = r.headers.find(lower_ascii(name));
  return it == r.headers.end() ? std::string() : it->second;
}

bool parse_request_head(const std::string& head, Request& req, int& code,
                        std::string& detail) {
  const size_t line_end = head.find("\r\n");
  if (line_end == std::string::npos) {
    code = 400;
    detail = "bad request line";
    return false;
  }
  std::istringstream first(head.substr(0, line_end));
  if (!(first >> req.method >> req.target >> req.version)) {
    code = 400;
    detail = "bad request line";
    return false;
  }
  if (req.method != "GET" && req.method != "POST" && req.method != "HEAD" &&
      req.method != "OPTIONS") {
    code = 405;
    detail = "method not allowed";
    return false;
  }
  if (req.method == "HEAD") req.head_only = true;
  if (req.target.size() > kMaxRequestTarget) {
    code = 414;
    detail = "request target too long";
    return false;
  }
  const size_t q = req.target.find('?');
  req.path = q == std::string::npos ? req.target : req.target.substr(0, q);
  req.query = q == std::string::npos ? std::string() : req.target.substr(q + 1);

  size_t pos = line_end + 2;
  while (pos < head.size()) {
    const size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) break;
    const std::string line = head.substr(pos, e - pos);
    pos = e + 2;
    if (line.empty()) break;
    const size_t c = line.find(':');
    if (c == std::string::npos) continue;
    const std::string name = lower_ascii(trim(line.substr(0, c)));
    const std::string value = trim(line.substr(c + 1));
    if (name.empty()) continue;
    if (req.headers.size() > 64) {
      code = 431;
      detail = "too many headers";
      return false;
    }
    req.headers[name] = value;
  }
  return true;
}

// Returns false when the connection should not produce a response (closed).
bool read_request(Conn& c, const HttpOptions& opt, Request& req, int& code,
                  std::string& detail) {
  std::string buf;
  char tmp[4096];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    if (buf.size() > kMaxHeaderBytes) {
      code = 431;
      detail = "header block too large";
      return true;
    }
    const int n = c.read_some(tmp, sizeof tmp);
    if (n < 0) {
      code = 408;
      detail = "read timeout";
      return true;
    }
    if (n == 0) return false;
    buf.append(tmp, (size_t)n);
    header_end = buf.find("\r\n\r\n");
  }
  if (!parse_request_head(buf.substr(0, header_end + 2), req, code, detail))
    return true;

  if (!header_value(req, "transfer-encoding").empty()) {
    code = 411;
    detail = "chunked transfer encoding is not supported";
    return true;
  }
  size_t content_length = 0;
  const std::string cl = header_value(req, "content-length");
  if (!cl.empty()) {
    try {
      content_length = (size_t)std::stoull(cl);
    } catch (...) {
      code = 400;
      detail = "bad content-length";
      return true;
    }
  }
  if (content_length > (size_t)opt.max_body_bytes) {
    code = 413;
    detail = "body too large";
    return true;
  }
  std::string body = buf.substr(header_end + 4);
  while (body.size() < content_length) {
    const int n = c.read_some(tmp, sizeof tmp);
    if (n <= 0) {
      code = 400;
      detail = "truncated body";
      return true;
    }
    body.append(tmp, (size_t)n);
  }
  if (body.size() > content_length) body.resize(content_length);
  req.body = std::move(body);
  return true;
}

// ---------------------------------------------------------------- auth

enum class AuthResult { kDisabled, kOk, kFailed };

AuthResult authenticate(const Request& req, const HttpOptions& opt) {
  if (opt.token.empty()) return AuthResult::kDisabled;
  const std::string auth = header_value(req, "authorization");
  if (!auth.empty()) {
    const std::string low = lower_ascii(auth);
    if (low.rfind("bearer ", 0) == 0) {
      return constant_time_equal(trim(auth.substr(7)), opt.token)
                 ? AuthResult::kOk
                 : AuthResult::kFailed;
    }
    if (low.rfind("basic ", 0) == 0) {
      const std::string decoded = base64_decode(trim(auth.substr(6)));
      const size_t colon = decoded.find(':');
      const std::string password =
          colon == std::string::npos ? decoded : decoded.substr(colon + 1);
      return constant_time_equal(password, opt.token) ? AuthResult::kOk
                                                      : AuthResult::kFailed;
    }
  }
  const std::string alt = header_value(req, "x-auth-token");
  if (!alt.empty()) {
    return constant_time_equal(alt, opt.token) ? AuthResult::kOk
                                                : AuthResult::kFailed;
  }
  return AuthResult::kFailed;
}

const char* auth_name(AuthResult a) {
  switch (a) {
    case AuthResult::kDisabled: return "none";
    case AuthResult::kOk: return "ok";
    default: return "failed";
  }
}

// ---------------------------------------------------------------- routing

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

std::string query_param(const std::string& query, const std::string& name) {
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string pair =
        query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = pair.find('=');
    const std::string key = url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
    if (key == name) {
      return eq == std::string::npos ? std::string() : url_decode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::string();
}

bool read_bool_json(const std::string& cfg, const char* name, bool def) {
  const std::string key = std::string("\"") + name + "\"";
  const size_t p = cfg.find(key);
  if (p == std::string::npos) return def;
  const size_t c = cfg.find(':', p + key.size());
  if (c == std::string::npos) return def;
  const size_t v = cfg.find_first_not_of(" \t\r\n", c + 1);
  if (v == std::string::npos) return def;
  return !(cfg.compare(v, 5, "false") == 0 || cfg[v] == '0');
}

std::string modules_json() {
  std::string cfg;
  std::ifstream in("config/modules.json");
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    cfg = ss.str();
  }
  std::string body = "{";
  bool first = true;
  for (const char* n : {"pg_cdc", "repair", "metrics", "snapshot", "resume"}) {
    if (!first) body += ",";
    first = false;
    body += "\"" + std::string(n) + "\":";
    body += read_bool_json(cfg, n, true) ? "true" : "false";
  }
  body += "}";
  return body;
}

std::string verify_json() {
  std::string mode = "local";
  std::ifstream in("config/verify.json");
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string cfg = ss.str();
    const size_t p = cfg.find("\"mode\"");
    if (p != std::string::npos) {
      const size_t c = cfg.find(':', p + 6);
      if (c != std::string::npos) {
        const size_t q = cfg.find('"', c + 1);
        if (q != std::string::npos) {
          const size_t e = cfg.find('"', q + 1);
          if (e != std::string::npos) mode = cfg.substr(q + 1, e - q - 1);
        }
      }
    }
  }
  return "{\"mode\":\"" + mode + "\"}";
}

std::string extract_json_string(const std::string& b, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  const size_t p = b.find(k);
  if (p == std::string::npos) return std::string();
  const size_t c = b.find(':', p + k.size());
  if (c == std::string::npos) return std::string();
  const size_t q = b.find('"', c + 1);
  if (q == std::string::npos) return std::string();
  const size_t e = b.find('"', q + 1);
  if (e == std::string::npos) return std::string();
  return b.substr(q + 1, e - q - 1);
}

std::string truncate_for_audit(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  return s.substr(0, max) + "...";
}

Response route_request(Request& req, const HttpOptions& opt, Metrics& m) {
  Response out;
  const std::string& path = req.path;

  if (req.method == "OPTIONS") {
    out.code = 204;
    out.body.clear();
    return out;
  }

  if (path == "/" || path == "/index.html") {
    out.ctype = "text/html; charset=utf-8";
    if (!read_file("web/index.html", out.body)) {
      out.body =
          "<html><body style='font-family:monospace;background:#0b1020;color:#e6edf3'>"
          "<h2>web/index.html not found</h2>"
          "<p>请从项目根目录启动（需要 web/index.html 与 config/）。</p></body></html>";
    } else {
      out.headers["Content-Security-Policy"] =
          "default-src 'self'; style-src 'self' 'unsafe-inline'; "
          "script-src 'self' 'unsafe-inline'; img-src 'self' data:; "
          "connect-src 'self'; frame-ancestors 'none'";
    }
    return out;
  }

  if (path == "/api/health") {
    out.body = "{\"ok\":true}";
    return out;
  }

  if (path == "/api/status") {
    out.body = m.status_json();
    return out;
  }
  if (path == "/api/tables") {
    out.body = m.tables_json();
    return out;
  }
  if (path == "/api/events") {
    out.body = m.events_json();
    return out;
  }
  if (path == "/api/series") {
    out.body = m.series_json();
    return out;
  }
  if (path == "/api/features") {
    std::string body = "{";
    bool first = true;
    for (const auto* mod : ModuleManager::instance().all()) {
      if (!first) body += ",";
      first = false;
      body += "\"" + mod->name() + "\":" + (mod->compiled() ? "true" : "false");
    }
    body += "}";
    out.body = body;
    return out;
  }
  if (path == "/api/modules") {
    if (req.method == "POST") {
      std::string b = trim(req.body);
      if (b.size() < 2 || b.front() != '{' || b.back() != '}') {
        out.code = 400;
        out.body = "{\"error\":\"bad json\"}";
        return out;
      }
      std::ofstream file("config/modules.json", std::ios::trunc);
      if (!file) {
        out.code = 500;
        out.body = "{\"error\":\"cannot write config/modules.json\"}";
        return out;
      }
      file << b;
      out.body = "{\"ok\":true,\"restart_required\":true}";
    } else {
      out.body = modules_json();
    }
    return out;
  }
  if (path == "/api/verify") {
    if (req.method == "POST") {
      const std::string mode = extract_json_string(req.body, "mode");
      if (mode != "local" && mode != "remote" && mode != "count_only") {
        out.code = 400;
        out.body = "{\"error\":\"bad mode\"}";
        return out;
      }
      std::ofstream file("config/verify.json", std::ios::trunc);
      if (!file) {
        out.code = 500;
        out.body = "{\"error\":\"cannot write config/verify.json\"}";
        return out;
      }
      file << "{\"mode\":\"" << mode << "\"}";
      out.body = "{\"ok\":true,\"restart_required\":true}";
    } else {
      out.body = verify_json();
    }
    return out;
  }
  if (path == "/api/security") {
    std::ostringstream o;
    o << "{\"auth\":" << (opt.token.empty() ? "false" : "true")
      << ",\"bind\":\"" << json_escape(opt.bind) << "\""
      << ",\"port\":" << opt.port
      << ",\"tls\":" << (opt.tls_cert.empty() ? "false" : "true")
      << ",\"tls_available\":" << (http_tls_available() ? "true" : "false")
      << ",\"audit\":\"" << json_escape(opt.audit_path) << "\""
      << ",\"allow_insecure\":" << (opt.allow_insecure ? "true" : "false")
      << ",\"cors\":" << (opt.cors ? "true" : "false")
      << ",\"max_body_bytes\":" << opt.max_body_bytes << "}";
    out.body = o.str();
    return out;
  }
  if (path == "/api/audit/tail") {
    size_t n = 50;
    const std::string ns = query_param(req.query, "n");
    if (!ns.empty()) {
      try {
        n = (size_t)std::stoul(ns);
      } catch (...) {
        n = 50;
      }
    }
    if (n == 0 || n > 200) n = 200;
    std::string body;
    if (opt.audit_path.empty() || !read_file(opt.audit_path, body)) {
      out.body = "[]";
      return out;
    }
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < body.size()) {
      const size_t e = body.find('\n', pos);
      const std::string line =
          body.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      if (!line.empty()) lines.push_back(line);
      if (e == std::string::npos) break;
      pos = e + 1;
    }
    std::string arr = "[";
    const size_t start = lines.size() > n ? lines.size() - n : 0;
    for (size_t i = start; i < lines.size(); ++i) {
      if (i > start) arr += ",";
      arr += lines[i];
    }
    arr += "]";
    out.body = arr;
    return out;
  }
  if (path == "/metrics") {
    if (!m.metrics_enabled.load()) {
      out.code = 404;
      out.body = "{\"error\":\"metrics module disabled\"}";
      return out;
    }
#ifdef MIG_ENABLE_METRICS
    out.ctype = "text/plain; version=0.0.4; charset=utf-8";
    out.body = m.prometheus();
#else
    out.code = 404;
    out.body = "{\"error\":\"metrics module not built\"}";
#endif
    return out;
  }

  out.code = 404;
  out.body = "{\"error\":\"not found\"}";
  return out;
}

// ---------------------------------------------------------------- response

std::string build_response(const Response& r, const HttpOptions& opt,
                           const std::string& request_id, bool head_only) {
  std::ostringstream o;
  o << "HTTP/1.1 " << r.code << " " << status_text(r.code) << "\r\n";
  o << "Content-Type: " << r.ctype << "\r\n";
  o << "Content-Length: " << r.body.size() << "\r\n";
  o << "Cache-Control: no-store\r\n";
  o << "Connection: close\r\n";
  o << "Server: mini-migrator\r\n";
  o << "X-Content-Type-Options: nosniff\r\n";
  o << "X-Frame-Options: DENY\r\n";
  o << "Referrer-Policy: no-referrer\r\n";
  o << "X-Request-Id: " << request_id << "\r\n";
  if (!opt.token.empty() && r.code == 401)
    o << "WWW-Authenticate: Bearer realm=\"mini-migrator\"\r\n";
  for (const auto& kv : r.headers) o << kv.first << ": " << kv.second << "\r\n";
  if (opt.cors) {
    o << "Access-Control-Allow-Origin: *\r\n";
    o << "Access-Control-Allow-Headers: Authorization, Content-Type, X-Auth-Token\r\n";
    o << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  }
  o << "\r\n";
  if (!head_only) o << r.body;
  return o.str();
}

void log_audit(AuditLog& audit, const Request& req, const Response& resp,
               AuthResult auth, const std::string& request_id,
               const std::string& tls_desc) {
  if (!audit.enabled()) return;
  std::ostringstream o;
  o << "{\"ts\":\"" << iso8601_utc() << "\""
    << ",\"epoch_ms\":" << epoch_millis()
    << ",\"request_id\":\"" << request_id << "\""
    << ",\"remote\":\"" << json_escape(req.remote) << "\""
    << ",\"method\":\"" << json_escape(req.method) << "\""
    << ",\"path\":\"" << json_escape(req.path) << "\""
    << ",\"status\":" << resp.code
    << ",\"auth\":\"" << auth_name(auth) << "\""
    << ",\"transport\":\"" << tls_desc << "\""
    << ",\"body_bytes\":" << req.body.size()
    << ",\"user_agent\":\""
    << json_escape(truncate_for_audit(header_value(req, "user-agent"), 160))
    << "\"";
  if (!req.body.empty() && req.method == "POST") {
    o << ",\"detail\":\""
      << json_escape(truncate_for_audit(req.body, 320)) << "\"";
  }
  o << "}";
  audit.write_line(o.str());
}

// ---------------------------------------------------------------- TLS

#ifdef MIG_HAVE_OPENSSL
SSL_CTX* make_tls_context(const HttpOptions& opt, std::string& err) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    err = "SSL_CTX_new failed";
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 |
                               SSL_OP_NO_TLSv1_1 |
                               SSL_OP_CIPHER_SERVER_PREFERENCE);
  if (SSL_CTX_use_certificate_chain_file(ctx, opt.tls_cert.c_str()) != 1) {
    err = "cannot load TLS certificate: " + opt.tls_cert;
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, opt.tls_key.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    err = "cannot load TLS private key: " + opt.tls_key;
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    err = "TLS certificate and private key do not match";
    SSL_CTX_free(ctx);
    return nullptr;
  }
  return ctx;
}
#endif

}  // namespace

bool http_tls_available() {
#ifdef MIG_HAVE_OPENSSL
  return true;
#else
  return false;
#endif
}

int validate_http_options(const HttpOptions& opt, std::string& err) {
  const bool loopback = host_is_loopback(opt.bind);
  if (!loopback && !opt.allow_insecure) {
    if (opt.token.empty()) {
      err = "refusing to expose the dashboard on " + opt.bind +
            " without --http-token (or pass --http-allow-insecure)";
      return 4;
    }
    if (opt.tls_cert.empty() || opt.tls_key.empty()) {
      err = "refusing plaintext dashboard on non-loopback " + opt.bind +
            "; set --tls-cert/--tls-key or pass --http-allow-insecure";
      return 4;
    }
  }
  if ((!opt.tls_cert.empty() || !opt.tls_key.empty()) &&
      (opt.tls_cert.empty() || opt.tls_key.empty())) {
    err = "both --tls-cert and --tls-key are required";
    return 1;
  }
  if (!opt.tls_cert.empty() && !http_tls_available()) {
    err = "this build has no OpenSSL; rebuild with OpenSSL to use --tls-cert";
    return 1;
  }
  if (opt.io_timeout_sec <= 0 || opt.io_timeout_sec > 3600) {
    err = "http timeout must be between 1 and 3600 seconds";
    return 1;
  }
  if (opt.max_body_bytes <= 0) {
    err = "http body limit must be positive";
    return 1;
  }
  return 0;
}

int run_http_server(const HttpOptions& opt, Metrics& m) {
  // Fail closed before opening a socket.
  std::string err;
  const int vrc = validate_http_options(opt, err);
  if (vrc != 0) {
    std::fprintf(stderr, "[fatal] %s\n", err.c_str());
    return vrc;
  }

  AuditLog audit;
  if (!audit.open(opt.audit_path, err)) {
    std::fprintf(stderr, "[fatal] %s\n", err.c_str());
    return 1;
  }

#ifdef MIG_HAVE_OPENSSL
  SSL_CTX* tls_ctx = nullptr;
  if (!opt.tls_cert.empty()) {
    tls_ctx = make_tls_context(opt, err);
    if (!tls_ctx) {
      std::fprintf(stderr, "[fatal] %s\n", err.c_str());
      return 1;
    }
  }
#endif

#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#else
  ::signal(SIGPIPE, SIG_IGN);
#endif

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(opt.port);
  const char* node = (opt.bind.empty() || opt.bind == "*") ? nullptr : opt.bind.c_str();
  const int gai = ::getaddrinfo(node, port_str.c_str(), &hints, &res);
  if (gai != 0 || !res) {
    std::fprintf(stderr, "[fatal] cannot resolve bind address %s: %s\n",
                 opt.bind.c_str(), gai_strerror(gai));
    return 2;
  }

  socket_t sfd = kInvalidSocket;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    sfd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sfd == kInvalidSocket) continue;
    int yes = 1;
    ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
#if defined(IPV6_V6ONLY)
    if (ai->ai_family == AF_INET6) {
      int off = 0;
      ::setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
    }
#endif
    if (::bind(sfd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
    close_socket(sfd);
    sfd = kInvalidSocket;
  }
  ::freeaddrinfo(res);
  if (sfd == kInvalidSocket) {
    std::fprintf(stderr, "[fatal] cannot bind %s:%d\n", opt.bind.c_str(), opt.port);
    return 2;
  }
  if (::listen(sfd, 64) != 0) {
    std::fprintf(stderr, "[fatal] listen failed on %s:%d\n", opt.bind.c_str(),
                 opt.port);
    close_socket(sfd);
    return 3;
  }

  const char* scheme = opt.tls_cert.empty() ? "http" : "https";
  std::fprintf(stderr,
               "[http] dashboard %s://%s:%d/ auth=%s tls=%s audit=%s\n", scheme,
               opt.bind.c_str(), opt.port,
               opt.token.empty() ? "off" : "on",
               opt.tls_cert.empty() ? "off" : "on",
               opt.audit_path.empty() ? "off" : opt.audit_path.c_str());

  AuthLimiter limiter;
  while (true) {
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    socket_t c = ::accept(sfd, (sockaddr*)&peer, &plen);
    if (c == kInvalidSocket) continue;

#if defined(_WIN32)
    const DWORD tv = (DWORD)opt.io_timeout_sec * 1000;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
    timeval tv{};
    tv.tv_sec = opt.io_timeout_sec;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif

    char host[NI_MAXHOST] = {0};
    if (::getnameinfo((sockaddr*)&peer, plen, host, sizeof host, nullptr, 0,
                      NI_NUMERICHOST) == 0) {
      // host filled
    } else {
      std::snprintf(host, sizeof host, "unknown");
    }

    Conn conn;
    conn.fd = c;
    const std::string remote = host;
    bool tls_ok = true;
#ifdef MIG_HAVE_OPENSSL
    if (tls_ctx) {
      SSL* ssl = SSL_new(tls_ctx);
      if (!ssl) {
        tls_ok = false;
      } else {
        SSL_set_fd(ssl, (int)c);
        if (SSL_accept(ssl) != 1) {
          tls_ok = false;
          SSL_free(ssl);
          ssl = nullptr;
        } else {
          conn.ssl = ssl;
        }
      }
    }
#endif
    if (!tls_ok) {
      close_socket(c);
      continue;
    }

    Request req;
    req.remote = remote;
    int code = 0;
    std::string detail;
    Response resp;
    const std::string rid = make_request_id();
    AuthResult auth = AuthResult::kDisabled;

    const bool keep = read_request(conn, opt, req, code, detail);
    if (!keep) {
      conn.write_all(build_response(
          Response{408, "application/json; charset=utf-8",
                   "{\"error\":\"request timeout\"}", {}},
          opt, rid, false));
    } else if (code != 0) {
      resp.code = code;
      resp.body = "{\"error\":\"" + json_escape(detail) + "\"}";
    } else {
      const bool public_path =
          req.path == "/api/health" || req.path == "/api/health/";
      if (public_path) {
        // Liveness must never depend on credentials or the failure throttle.
        auth = AuthResult::kDisabled;
        resp = route_request(req, opt, m);
      } else if (limiter.blocked(remote)) {
        resp.code = 429;
        resp.body = "{\"error\":\"too many failed authentication attempts\"}";
      } else {
        auth = authenticate(req, opt);
        if (auth == AuthResult::kFailed) {
          limiter.note_failure(remote);
          resp.code = 401;
          resp.body = "{\"error\":\"unauthorized\"}";
        } else {
          resp = route_request(req, opt, m);
        }
      }
    }

#ifdef MIG_HAVE_OPENSSL
    const std::string tls_desc = conn.ssl ? "tls" : "plain";
#else
    const std::string tls_desc = "plain";
#endif
    log_audit(audit, req, resp, auth, rid, tls_desc);
    conn.write_all(build_response(resp, opt, rid, req.head_only));

#ifdef MIG_HAVE_OPENSSL
    if (conn.ssl) {
      SSL_shutdown(conn.ssl);
      SSL_free(conn.ssl);
    }
#endif
    close_socket(c);
  }

  close_socket(sfd);
  return 0;
}

}  // namespace mig
