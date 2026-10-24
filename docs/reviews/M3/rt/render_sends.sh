#!/usr/bin/env bash
# M3 RT review: render the generated projects (gen_sends_projects.py) offline and print the process-loop time.
# usage: render_sends.sh <pqdaw.exe> <project root> [seconds] [runs]
exe="$1"; root="$2"; secs="${3:-20}"; runs="${4:-3}"
for p in t32_r0 t32_r1 t32_r4 t32_r0_notes t32_r1_notes t32_r4_notes; do
  for i in $(seq 1 "$runs"); do
    out=$("$exe" --project "$root/$p" --render "$root/$p.wav" --seconds "$secs" 2>&1 | grep -o "in [0-9.]* s ([0-9.]*x realtime)")
    echo "$p run $i: $out"
  done
done
