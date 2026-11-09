#!/usr/bin/env bash
# openGauss 冒烟测试：MySQL(pkdb) -> openGauss(og_test)
# 前置：目标 openGauss 已按 docs/usage.md 第 16 节开启 md5 认证 + PG 兼容库
set -euo pipefail
cd "$(dirname "$0")/.."

MY_DSN="${MY_DSN:-host=127.0.0.1;port=3306;user=root;db=pkdb}"
OG_DSN="${OG_DSN:-host=127.0.0.1;port=15432;user=gaussdb;password=Enmo@123;db=og_test}"
OG_ADMIN_DSN="${OG_ADMIN_DSN:-host=127.0.0.1;port=15432;user=gaussdb;password=Enmo@123;db=postgres}"

echo "== 目标实例 =="
psql "$OG_ADMIN_DSN" -Atc "select version();"

echo "== 重建目标库（PG 兼容模式）=="
psql "$OG_ADMIN_DSN" -c "DROP DATABASE IF EXISTS og_test;" || true
psql "$OG_ADMIN_DSN" -c "CREATE DATABASE og_test WITH ENCODING='UTF8' DBCOMPATIBILITY='PG';"

echo "== 迁移 =="
./build/migrator --mode mysql2og --source "$MY_DSN" --target "$OG_DSN" \
  --batch 2000 --workers 4 --port 8130 --exit | tee /tmp/og-smoke.out

grep -q "verify : PASS" /tmp/og-smoke.out
echo "== PASS =="
