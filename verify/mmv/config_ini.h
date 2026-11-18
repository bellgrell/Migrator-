#pragma once

#include <string>

#include "engine.h"
#include "status.h"

namespace mmv {

// Load a simple INI file into opts. CLI arguments applied afterwards override.
Status apply_config_file(const std::string& path, VerifyOptions& opts);

}  // namespace mmv
