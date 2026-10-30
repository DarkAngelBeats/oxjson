#!/bin/bash
cd /tmp/oxj/rel; PY=/tmp/oxj/venv/bin/python; O=/tmp/oxj/rel/out; mkdir -p $O
DOCS="testdata/engine/atlas.json testdata/diverse/numbers.json testdata/diverse/twitter.json testdata/diverse/citm_catalog.json testdata/diverse/canada.json testdata/diverse/marine_ik.json testdata/bench/large_50k.json"
log() { echo "$(date -u +%FT%TZ) $*" | tee -a $O/run.log; }
log "start load: $(cat /proc/loadavg)"
for e in "" 1; do OXJSON_NO_AVX2=$e $PY tests/check_dumps_orjson.py > $O/check-dumps-avx2$e.txt 2>&1; log "check_dumps_orjson NO_AVX2=$e rc=$?"; done
$PY tests/check_portable.py > $O/check-portable.txt 2>&1; log "check_portable rc=$?"
for s in 1 2; do $PY bench/current_compare.py --repeats 7 --output $O/speed-$s.json > $O/speed-$s.log 2>&1; log "speed series $s rc=$?"; done
$PY bench/memory_audit.py --operations loads dumps --docs $DOCS --repeats 5 --rounds 10 --calls 20 --retain 6 --output $O/memory.json > $O/memory.log 2>&1; log "memory rc=$?"
log "done load: $(cat /proc/loadavg)"
