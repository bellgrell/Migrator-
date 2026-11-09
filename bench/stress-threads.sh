#!/usr/bin/env bash
# LOAD DATA 后的线程扩展性测试：MySQL 目标三条路径 + mysql2og 对照
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=./build/migrator
OUT=bench/results-threads.csv
PW=$(docker exec og printenv GS_PASSWORD) || { echo "openGauss not running"; exit 1; }
rm -f /tmp/mig-load-* 2>/dev/null

MY_SRC='host=127.0.0.1;port=3306;user=root;db=pkdb'
PG_SRC='host=127.0.0.1;port=5432;user=bellgrell;db=pg_from_og'
OG_SRC="host=127.0.0.1;port=15432;user=gaussdb;password=$PW;db=og_test"
MY_TGT='host=127.0.0.1;port=3306;user=root;db=stress_my'
OG_TGT="host=127.0.0.1;port=15432;user=gaussdb;password=$PW;db=stress_og"

reset_my() { mysql -h 127.0.0.1 -uroot -e "DROP DATABASE IF EXISTS $1; CREATE DATABASE $1 CHARACTER SET utf8mb4"; }
reset_og() { PGPASSWORD="$PW" psql -h 127.0.0.1 -p 15432 -U gaussdb -d postgres -q -c "DROP DATABASE IF EXISTS $1" -c "CREATE DATABASE $1 WITH ENCODING='UTF8' DBCOMPATIBILITY='PG'"; }

echo "mode,workers,rows,time_s,rows_per_s,verify,errors" > "$OUT"

run_case() {
  name=$1; mode=$2; src=$3; tgt=$4; kind=$5; db=$6; w=$7
  case "$kind" in my) reset_my "$db" ;; og) reset_og "$db" ;; esac
  log="/tmp/threads-$name-w$w.log"
  "$BIN" --mode "$mode" --source "$src" --target "$tgt" \
    --batch 2000 --workers "$w" --port 8151 --exit > "$log" 2>&1
  rows=$(awk -F': ' '/^ rows/ {v=$2} END{print v}' "$log" | tr -d ',')
  tm=$(awk -F': ' '/^ time/ {v=$2} END{print v}' "$log")
  spd=$(awk -F': ' '/^ speed/ {v=$2} END{print v}' "$log" | awk '{print $1}')
  ver=$(awk -F': ' '/^ verify/ {v=$2} END{print v}' "$log")
  err=$(awk -F': ' '/^ errors/ {v=$2} END{print v}' "$log")
  echo "$name,$w,$rows,$tm,$spd,$ver,$err" | tee -a "$OUT"
}

for w in 1 2 4 8; do
  run_case mysql2mysql mysql2mysql "$MY_SRC" "$MY_TGT" my stress_my "$w"
  run_case pg2mysql    pg2mysql    "$PG_SRC" "$MY_TGT" my stress_my "$w"
  run_case og2mysql    og2mysql    "$OG_SRC" "$MY_TGT" my stress_my "$w"
  run_case mysql2og    mysql2og    "$MY_SRC" "$OG_TGT" og stress_og "$w"
done

echo "=== DONE ==="
