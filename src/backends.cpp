#include "backends.h"

namespace mig {

std::shared_ptr<ISource> make_source(const Options& o, Status& st) {
  const std::string k = o.source_kind.empty() ? "sim" : o.source_kind;
  if (k == "sim") return make_sim_source(o);
#ifdef HAVE_MYSQL
  if (k == "mysql") return make_mysql_source(o, st);
#endif
#ifdef HAVE_PG
  if (k == "pg" || k == "og") return make_pg_source(o, st);
#endif
  st = Status::Err(Code::kUnsupported, "source kind not built: " + k);
  return nullptr;
}

std::shared_ptr<ITarget> make_target(const Options& o, Status& st) {
  const std::string k = o.target_kind.empty() ? "sim" : o.target_kind;
  if (k == "sim") return make_sim_target(o);
#ifdef HAVE_MYSQL
  if (k == "mysql") return make_mysql_target(o, st);
#endif
#ifdef HAVE_PG
  if (k == "pg" || k == "og") return make_pg_target(o, st);
#endif
  st = Status::Err(Code::kUnsupported, "target kind not built: " + k);
  return nullptr;
}

}  // namespace mig
