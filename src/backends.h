#pragma once

#include "migrator.h"

namespace mig {

std::shared_ptr<ISource> make_sim_source(const Options& o);
std::shared_ptr<ITarget> make_sim_target(const Options& o);

#ifdef HAVE_MYSQL
std::shared_ptr<ISource> make_mysql_source(const Options& o, Status& st);
#endif

#ifdef HAVE_PG
std::shared_ptr<ITarget> make_pg_target(const Options& o, Status& st);
#endif

#ifdef HAVE_MYSQL
std::shared_ptr<ITarget> make_mysql_target(const Options& o, Status& st);
#endif

}  // namespace mig
