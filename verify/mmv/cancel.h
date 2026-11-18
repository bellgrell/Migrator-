#pragma once

#include <csignal>

namespace mmv {
extern volatile std::sig_atomic_t g_interrupted;
}
