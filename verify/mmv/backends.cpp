#include "sql_backend.h"

namespace mmv {

std::shared_ptr<SqlBackend> make_pg_backend(const std::string& dsn, Status& st);
std::shared_ptr<SqlBackend> make_mysql_backend(const std::string& dsn, Status& st);

std::shared_ptr<SqlBackend> make_backend(const std::string& kind, const std::string& dsn,
                                         Status& st) {
#ifdef HAVE_PG
  if (kind == "pg" || kind == "og" || kind == "opengauss") return make_pg_backend(dsn, st);
#endif
#ifdef HAVE_MYSQL
  if (kind == "mysql" || kind == "mariadb") return make_mysql_backend(dsn, st);
#endif
  st = Status::Err(Code::kUnsupported, "backend not compiled: " + kind);
  return nullptr;
}

}  // namespace mmv
