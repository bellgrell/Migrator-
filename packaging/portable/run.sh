#!/usr/bin/env bash
# Portable launcher: uses bundled client libraries, no Docker required.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/bin/migrator" "$@"
