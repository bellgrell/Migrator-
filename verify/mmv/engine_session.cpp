#include "engine_session.h"

#include <cstdio>
#include <string>

namespace mmv {
namespace vdetail {

namespace {

std::string with_tls(const std::string& kind, const std::string& dsn, bool require) {
  if (!require) return dsn;
  const bool pg = kind == "pg" || kind == "og" || kind == "opengauss";
  if (pg && dsn.find("sslmode=") == std::string::npos)
    return dsn + ";sslmode=require";
  const bool my = kind == "mysql" || kind == "mariadb";
  if (my && dsn.find("ssl-mode=") == std::string::npos)
    return dsn + ";ssl-mode=REQUIRED";
  return dsn;
}

}  // namespace

std::shared_ptr<SqlBackend> connect_retry(const std::string& kind, const std::string& dsn,
                                          const VerifyOptions& opts, Status& st) {
  for (int i = 0; i <= opts.retries; ++i) {
    st = Status::OK();
    auto b = make_backend(kind, with_tls(kind, dsn, opts.require_tls), st);
    if (b) return b;
    if (i < opts.retries) {
      std::fprintf(stderr, "[retry] %s connect %d/%d: %s\n", kind.c_str(), i + 1,
                   opts.retries, st.msg.c_str());
      sleep_ms(opts.retry_backoff_ms * (i + 1));
    }
  }
  return nullptr;
}

}  // namespace vdetail
}  // namespace mmv
