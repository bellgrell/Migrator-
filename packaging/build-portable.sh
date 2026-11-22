#!/usr/bin/env bash
# Build a portable, Docker-free Linux package.
# Usage: bash packaging/build-portable.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-1.0.0}"
DIST="dist/migrator-${VER}-linux-x64"
rm -rf "$DIST"
mkdir -p "$DIST"/{bin,lib,config}
cp build/migrator "$DIST/bin/"
cp config/*.json config/*.ini "$DIST/config/" 2>/dev/null || true
for l in libmysqlclient.so.24 libpq.so.5 libodbc.so.2 libltdl.so.7 libzstd.so.1; do
  cp "/usr/lib/x86_64-linux-gnu/$l" "$DIST/lib/" 2>/dev/null || true
done
cp packaging/portable/run.sh "$DIST/"
cp packaging/portable/INSTALL.md "$DIST/"
chmod +x "$DIST/run.sh" "$DIST/bin/migrator"
tar -C dist -czf "dist/migrator-${VER}-linux-x64.tar.gz" "migrator-${VER}-linux-x64"
echo "built dist/migrator-${VER}-linux-x64.tar.gz"
