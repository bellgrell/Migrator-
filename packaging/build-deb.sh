#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=packaging/debian
VER=0.9.0
ARCH=$(dpkg --print-architecture)

rm -rf "$ROOT/usr/bin" "$ROOT/usr/share/doc/mini-migrator"
install -Dm755 build/migrator "$ROOT/usr/bin/mini-migrator"
install -Dm644 config/providers.ini "$ROOT/etc/mini-migrator/providers.ini"
install -Dm644 docs/dependencies.md "$ROOT/usr/share/doc/mini-migrator/dependencies.md"

mkdir -p "$ROOT/DEBIAN"
sed "s/^Architecture: .*/Architecture: $ARCH/" packaging/control.in > "$ROOT/DEBIAN/control"

DEB="mini-migrator_$VER"_"$ARCH.deb"
dpkg-deb --build --root-owner-group "$ROOT" "$DEB"
ls -lh "$DEB"
