#!/usr/bin/env bash
# 多方向 500 万行压力测试：MySQL / PostgreSQL / openGauss 互转
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=./build/migrator
OUT=bench/results-matrix.csv
PW=$(docker exec og printenv GS_PASSWORD) || { echo "openGauss container not running"; exit 1; }

MY_SRC='host=127.0.0.1;port=3306;user=root;db=pkdb'
PG_SRC='host=127.0.0.1;port=5432;user=bellgrell;db=pg_from_og'
OG_SRC="host=127.0.0.1;port=15432;user=gaussdb;password=$PW;db=og_test"
PG_TGT='host=127.0.0.1;port=5432;user=bellgrell;db=stress_pg'
OG_TGT="host=127.0.0.1;port=15432;user=gaussdb;password=$PW;db=stress_og"
MY_TGT='host=127.0.0.1;port=3306;user=root;db=stress_my'

reset_pg() { psql -h 127.0.0.1 -U bellgrell -d postgres -q -c "DROP DATABASE IF EXISTS $1" -c "CREATE DATABASE $1"; }
reset_og() { PGPASSWORD="$PW" psql -h 127.0.0.1 -p 15432 -U gaussdb -d postgres -q -c "DROP DATABASE IF EXISTS $1" -c "CREATE DATABASE $1 WITH ENCODING='UTF8' DBCOMPATIBILITY='PG'"; }
reset_my() { mysql -h 127.0.0.1 -uroot -e "DROP DATABASE IF EXISTS $1; CREATE DATABASE $1 CHARACTER SET utf8mb4"; }

echo "case,mode,rows,time_s,rows_per_s,verify,errors" > "$OUT"

run_case() {
  name=$1; mode=$2; src=$3; tgt=$4; kind=$5; db=$6
  case "$kind" in
    pg) reset_pg "$db" ;;
    og) reset_og "$db" ;;
    my) reset_my "$db" ;;
  esac
  log="/tmp/stress-$name.log"
  "$BIN" --mode "$mode" --source "$src" --target "$tgt" \
    --batch 2000 --workers 4 --port 8150 --exit > "$log" 2>&1
  rows=$(awk -F': ' '/^ rows/ {v=$2} END{print v}' "$log" | tr -d ',')
  tm=$(awk -F': ' '/^ time/ {v=$2} END{print v}' "$log")
  spd=$(awk -F': ' '/^ speed/ {v=$2} END{print v}' "$log" | awk '{print $1}')
  ver=$(awk -F': ' '/^ verify/ {v=$2} END{print v}' "$log")
  err=$(awk -F': ' '/^ errors/ {v=$2} END{print v}' "$log")
  echo "$name,$mode,$rows,$tm,$spd,$ver,$err" | tee -a "$OUT"
}

echo "=== 500 万行方向矩阵 ==="
run_case mysql2pg    mysql2pg    "$MY_SRC" "$PG_TGT" pg stress_pg
run_case mysql2og    mysql2og    "$MY_SRC" "$OG_TGT" og stress_og
run_case mysql2mysql mysql2mysql "$MY_SRC" "$MY_TGT" my stress_my
run_case og2pg       og2pg       "$OG_SRC" "$PG_TGT" pg stress_pg
run_case og2mysql    og2mysql    "$OG_SRC" "$MY_TGT" my stress_my
run_case pg2og       pg2og       "$PG_SRC" "$OG_TGT" og stress_og
run_case pg2mysql    pg2mysql    "$PG_SRC" "$MY_TGT" my stress_my

echo "=== DONE ==="
