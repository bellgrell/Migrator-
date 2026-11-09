#include "backends.h"

namespace mig {

std::shared_ptr<ISource> make_source(const Options& o, Status& st) {
  if (o.mode == "sim") return make_sim_source(o);
#ifdef HAVE_MYSQL
  if (o.mode == "mysql" || o.mode == "mysql2pg") return make_mysql_source(o, st);
#endif
  st = Status::Err(Code::kUnsupported, "source mode not built: " + o.mode);
  return nullptr;
}

std::shared_ptr<ITarget> make_target(const Options& o, Status& st) {
  if (o.mode == "sim") return make_sim_target(o);
#ifdef HAVE_PG
  if (o.mode == "postgres" || o.mode == "opengauss" || o.mode == "mysql2pg")
    return make_pg_target(o, st);
#endif
  st = Status::Err(Code::kUnsupported, "target mode not built: " + o.mode);
  return nullptr;
}

}  // namespace mig
