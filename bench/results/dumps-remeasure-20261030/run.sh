#!/bin/bash
# usage: run.sh PYTHON ROOT   (ROOT holds base/ and cur/ trees; results go to ROOT/out)
PY="$1"; R="$2"; O="$R/out"; mkdir -p "$O"
DOCS="testdata/engine/atlas.json testdata/diverse/numbers.json testdata/diverse/twitter.json testdata/diverse/citm_catalog.json testdata/diverse/canada.json testdata/diverse/marine_ik.json testdata/bench/large_50k.json"
log() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log"; }
log "start load: $(cat /proc/loadavg 2>/dev/null)"
for s in 1 2; do
  for t in base cur; do
    log "speed $t series $s"
    (cd "$R/$t" && "$PY" bench/current_compare.py --repeats 7 --output "$O/speed-$t-$s.json" > "$O/speed-$t-$s.log" 2>&1) || log "FAIL speed $t $s"
  done
done
for t in base cur; do
  log "memory $t"
  (cd "$R/$t" && "$PY" bench/memory_audit.py --operations dumps --docs $DOCS --repeats 5 --rounds 10 --calls 20 --retain 6 --output "$O/memory-$t.json" > "$O/memory-$t.log" 2>&1) || log "FAIL memory $t"
done
log "done load: $(cat /proc/loadavg 2>/dev/null)"
