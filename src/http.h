#pragma once

#include <string>

namespace mig {

struct Metrics;

// Runtime options for the built-in dashboard/metrics HTTP server.
//
// Security model (fail-closed):
//   * loopback bind  -> plaintext allowed, auth optional
//   * non-loopback   -> requires a token AND TLS unless --http-allow-insecure
//   * token set      -> every endpoint except /api/health requires the token
//   * every request  -> one JSON line in the audit log
struct HttpOptions {
  int port = 8123;
  std::string bind = "127.0.0.1";
  std::string token;                    // bearer token, empty = auth disabled
  std::string audit_path = "out/audit.log";
  std::string tls_cert;                 // PEM certificate chain
  std::string tls_key;                  // PEM private key
  bool allow_insecure = false;          // explicitly allow remote plaintext
  bool cors = false;                    // emit permissive CORS headers
  int max_body_bytes = 1048576;         // request body limit (1 MiB)
  int io_timeout_sec = 10;              // per-connection read/write timeout
};

// 0 on clean exit. Startup failures: 1 socket/TLS setup, 2 bind, 3 listen,
// 4 refused because a remote bind would be plaintext/unauthenticated.
int run_http_server(const HttpOptions& opt, Metrics& m);

// Pre-flight check used before a long migration starts. Returns 0 when the
// dashboard may start, 1 for bad TLS settings, 4 for an unsafe bind.
int validate_http_options(const HttpOptions& opt, std::string& err);

// True when this binary was built with OpenSSL support.
bool http_tls_available();

}  // namespace mig
