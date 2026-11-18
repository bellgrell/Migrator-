#!/usr/bin/env bash
# Build a native Windows binary with MSYS2 (run inside "MSYS2 MINGW64").
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

echo "== installing toolchain (MSYS2 mingw64) =="
pacman -S --needed --noconfirm   mingw-w64-x86_64-gcc   mingw-w64-x86_64-cmake   mingw-w64-x86_64-make   mingw-w64-x86_64-postgresql   mingw-w64-x86_64-unixodbc   mingw-w64-x86_64-openssl   mingw-w64-x86_64-mariadb-connector-c

# mariadb-connector-c 的头文件在 include/mariadb 下，补一个兼容入口
INC="/mingw64/include"
if [ -f "$INC/mariadb/mysql.h" ] && [ ! -e "$INC/mysql/mysql.h" ]; then
  mkdir -p "$INC/mysql"
  ln -sf ../mariadb/mysql.h "$INC/mysql/mysql.h" || true
fi

echo "== configure =="
cmake -B build-win -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_QT_GUI=OFF \
  -DCMAKE_PREFIX_PATH=/mingw64

echo "== build =="
cmake --build build-win -j"$(nproc)"

BIN="build-win/migrator.exe"
[ -f "$BIN" ] || { echo "build finished but $BIN not found"; exit 1; }

echo "== package =="
OUT="$ROOT/out-win"
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib"
cp "$BIN" "$OUT/bin/"
cp -r config web "$OUT/" 2>/dev/null || true
# 收集 mingw 运行时依赖
for dll in $(ldd "$BIN" | awk '/=> \//{print $3}'); do
  case "$dll" in
    /mingw64/bin/*) cp -L "$dll" "$OUT/lib/" ;;
  esac
done
cat > "$OUT/run.cmd" <<'EOF'
@echo off
set PATH=%~dp0lib;%PATH%
"%~dp0bin\migrator.exe" %*
EOF
echo "done: $OUT"
