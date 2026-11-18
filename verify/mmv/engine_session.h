#pragma once

#include <memory>
#include <string>

#include "engine.h"
#include "engine_common.h"
#include "sql_backend.h"

namespace mmv {
namespace vdetail {

struct TxGuard {
  SqlBackend* s = nullptr;
  SqlBackend* t = nullptr;
  bool active = false;
  ~TxGuard() {
    if (!active) return;
    if (t) t->end_readonly();
    if (s) s->end_readonly();
  }
};

struct CoordGuard {
  SqlBackend* s = nullptr;
  SqlBackend* t = nullptr;
  bool s_active = false;
  bool t_active = false;
  bool s_locked = false;
  bool t_locked = false;
  ~CoordGuard() {
    if (t_locked) t->end_global_read_lock();
    if (s_locked) s->end_global_read_lock();
    if (t_active) t->end_readonly();
    if (s_active) s->end_readonly();
  }
};

std::shared_ptr<SqlBackend> connect_retry(const std::string& kind, const std::string& dsn,
                                          const VerifyOptions& opts, Status& st);

}  // namespace vdetail
}  // namespace mmv
