#!/usr/bin/env bash
# M4 RT review: time offline renders of the gen_device_projects.py projects through `pqdaw --render` (the playback path,
# no engine gate: LinkConfig::sleepAfterMs = 0 for renders, so idle units / sleeping regions are what is measured).
# The JIT compiles in the background while the render runs: run 1 with an empty PQ_JIT_CACHE_DIR is mostly interpreted;
# later runs with the same cache dir get every JIT image from the cache within milliseconds (mostly JIT).
# usage: render_devices.sh <pqdaw.exe> <project root> <out dir> <seconds> <runs> <name>...
exe="$1"; root="$2"; out="$3"; secs="$4"; runs="$5"; shift 5
mkdir -p "$out"
for p in "$@"; do
  for i in $(seq 1 "$runs"); do
    load="${LOAD:-?}"  # recorded by the caller (Get-CimInstance Win32_Processor LoadPercentage)
    line=$("$exe" --project "$root/$p" --render "$out/$p.wav" --seconds "$secs" 2>&1 | grep -o "in [0-9.]* s ([0-9.]*x realtime)")
    # % of one core = 100 / realtime factor
    rt=$(echo "$line" | sed -E 's/.*\(([0-9.]+)x realtime\).*/\1/')
    pct=$(awk -v r="$rt" 'BEGIN{ if (r>0) printf "%.2f", 100.0/r; else print "?" }')
    echo "$p run $i: $line = $pct % of a core (load $load %)"
  done
done
