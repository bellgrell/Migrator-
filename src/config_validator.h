#pragma once

#include "migrator.h"

namespace mig {

// Central option validation.  Returns kInvalid with a user-facing message.
Status validate_options(const Options& o);

}  // namespace mig
