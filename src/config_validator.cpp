#include "config_validator.h"

#include <string>

namespace mig {

Status validate_options(const Options& o) {
  if (o.workers < 1 || o.workers > 64)
    return Status::Err(Code::kInvalid, "workers must be in [1, 64]");
  if (o.batch_size < 1 || o.batch_size > 1000000)
    return Status::Err(Code::kInvalid, "batch must be in [1, 1000000]");
  if (o.commit_every < 0)
    return Status::Err(Code::kInvalid, "commit must be >= 0");
  if (o.io_queue_depth < 0 || o.io_queue_depth > 64)
    return Status::Err(Code::kInvalid, "io-queue-depth must be in [0, 64]");
  if (o.io_prefetch < 0 || o.io_prefetch > 64)
    return Status::Err(Code::kInvalid, "io-prefetch must be in [0, 64]");
  if (o.io_window < 0)
    return Status::Err(Code::kInvalid, "io-window must be >= 0");
  if (o.load_mode != "auto" && o.load_mode != "insert" &&
      o.load_mode != "ignore" && o.load_mode != "replace")
    return Status::Err(Code::kInvalid, "load-mode must be auto/insert/ignore/replace");
  if (o.mode != "sim" && (o.source.empty() || o.target.empty()))
    return Status::Err(Code::kInvalid, "source and target DSN are required");
  if (o.consistent_snapshot && o.resume)
    return Status::Err(Code::kInvalid,
                       "consistent-snapshot and resume are mutually exclusive");
  return Status::OK();
}

}  // namespace mig
