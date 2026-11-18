#!/bin/sh
# Build the Linux artifact. Requires Docker with network access.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="${OUT:-$HOME/Documents/migrator-release/linux}"
VER="${VER:-0.9.0}"
mkdir -p "$OUT"
docker build -f "$ROOT/packaging/Dockerfile" -t mini-migrator:"$VER" "$ROOT"
CID=$(docker create mini-migrator:"$VER")
docker cp "$CID":/usr/local/bin/migrator "$OUT/migrator-linux-x86_64"
docker rm "$CID" >/dev/null
docker save -o "$OUT/mini-migrator-$VER-linux-amd64-image.tar" mini-migrator:"$VER"
echo "wrote $OUT/migrator-linux-x86_64"
echo "wrote $OUT/mini-migrator-$VER-linux-amd64-image.tar"
