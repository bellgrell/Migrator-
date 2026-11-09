#!/bin/bash
set -u
cd "$HOME/Documents/mini-migrator" || exit 1

BIN=./build/migrator
PSQL=/opt/homebrew/opt/postgresql@16/bin/psql
OUT=bench/results.csv
mkdir -p bench out
echo "mode,workers,batch,rows,time_s,rows_per_s,verify" > "$OUT"

parse() {
  log=$1; mode=$2; w=$3; b=$4
  rows=$(awk -F': ' '/^ rows/ {v=$2} END{print v}' "$log" | tr -d ',')
  tm=$(awk -F': ' '/^ time/ {v=$2} END{print v}' "$log")
  spd=$(awk -F': ' '/^ speed/ {v=$2} END{print v}' "$log" | awk '{print $1}')
  ver=$(awk -F': ' '/^ verify/ {v=$2} END{print v}' "$log")
  echo "$mode,$w,$b,$rows,$tm,$spd,$ver" >> "$OUT"
  printf '  %-4s w=%-2s b=%-5s -> %10s rows/s (%ss, rows=%s, %s)\n' "$mode" "$w" "$b" "$spd" "$tm" "$rows" "$ver"
}

echo "=== A. sim 引擎吞吐（200 万行 / 6 表）==="
for w in 1 2 4 8; do
  for b in 500 5000; do
    log=/tmp/n_sim_$w-$b.log
    "$BIN" --mode sim --rows 2000000 --tables 6 --workers "$w" --batch "$b" \
      --log-every 0 --port 8299 --exit > "$log" 2>&1
    parse "$log" sim "$w" "$b"
  done
done

echo "=== B. 真实 MySQL -> PostgreSQL（10 万行）==="
for w in 1 2 4; do
  for b in 500 2000 10000; do
    "$PSQL" -h 127.0.0.1 -U bellgrell -d demo_dst -c 'TRUNCATE users;' > /dev/null 2>&1
    log=/tmp/n_real_$w-$b.log
    "$BIN" --mode mysql2pg \
      --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
      --target 'host=127.0.0.1;port=5432;user=bellgrell;db=demo_dst' \
      --workers "$w" --batch "$b" --log-every 0 --port 8299 --exit > "$log" 2>&1
    parse "$log" real "$w" "$b"
  done
done

echo "=== DONE ==="
