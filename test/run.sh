#!/bin/sh
#
# Alles, was ohne Netz nach draußen geht, in einem Lauf:
#
#   1. die Node-Abhängigkeiten des Harness (pnpm, offline aus dem Store,
#      sonst aus der Registry)
#   2. die Golden Files der visigoth-Spur gegen das echte visigoth neu
#      erzeugt und verglichen - sie dürfen nicht von Hand gepflegt sein;
#      ebenso die Muster-Spur gegen microwizard fix/pattern-shadow, wenn
#      es einen Checkout davon gibt (MWM_MICROWIZARD)
#   3. make: mwm-node und die Unit-Tests in meta (msgpack, visigoth,
#      balance, swim, mesh, pattern, service, client - der dreimal: wie
#      1.6.2 (--no-patient), mit patient, dem Standard, und mit kurzem
#      Timeout); dann pattern_test gegen eine frische Spur mit zufälligem
#      Seed (MWM_PATTERN_SEED), Gewinner für Gewinner gegen microwizard
#   4. reference.js: was ein Node-Aufrufer von einem Node-Dienst zurückbekommt
#   5. interop.js: mwm-node im Mesh mit Node-Knoten
#   6. meshclient.js: der Client von mwm-node im Mesh, gegen Node verglichen
#   7. deadpin.js: ein Pin, dessen letztes Mitglied weg ist, fällt aus dem
#      Abgleich des Clients (microwizard fix/dead-pin)
#   8. concurrency.js: viele Aufrufe zugleich über eine Verbindung, in beide
#      Richtungen, die Grenze --max-inflight und SIGTERM mit laufenden Aufrufen
#
# Alles auf 127.0.0.1. Der Basisknoten des Harness liegt auf UDP 38999
# (MWM_BASE_PORT), nicht auf 39999 (der Standard von microwizard) und nicht auf 39000/39001
# (gern ein lokales Dev-Mesh); ist der Port schon belegt, bricht das hier
# ab, statt sich in ein fremdes Mesh zu hängen.
#
#   test/run.sh            alles
#   test/run.sh unit       nur 1-3
#   META=/pfad/meta test/run.sh
#   MWM_SAN=1 test/run.sh  alles mit ASan und UBSan (build-san/)
#   MWM_MICROWIZARD=/pfad/zu/microWizard test/run.sh
#                          der Checkout mit dem Matcher von fix/pattern-shadow;
#                          ohne ihn wird neben dem Repo gesucht, sonst
#                          bleibt es bei der Golden File

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$here")
harness=$here/harness
port=${MWM_BASE_PORT:-38999}
build=$repo/build
san=

if [ -n "${MWM_SAN:-}" ]; then
  build=$repo/build-san
  san=SAN=1
  # mwm-node endet mit exit() aus einem Task, dessen Stack die Leck-Suche
  # nicht sieht - was nur ein Task-Stack hält, gälte ihr als verloren; die
  # Unit-Tests enden in main und werden auf Lecks geprüft
  export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=1}
  export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
fi

# nichts aus einer wzdc-Konfiguration der Shell darf hier hineinreichen
for v in $(env | sed -n 's/^\(wzdc_[^=]*\)=.*/\1/p'); do unset "$v"; done

if [ ! -d "$harness/node_modules/microwizard" ]; then
  echo "# Harness: pnpm install"
  (cd "$harness" && { pnpm install --offline >/dev/null 2>&1 || pnpm install >/dev/null; })
fi

echo "# visigoth-Spur gegen @wzrdtales/visigoth 2.0.5"
node "$harness/visigoth-trace.js" 7 3000 | cmp -s - "$here/fixtures/visigoth-trace.txt" || {
  echo "not ok - test/fixtures/visigoth-trace.txt ist nicht, was visigoth sagt" >&2
  exit 1
}

# microwizard mit dem Matcher von fix/pattern-shadow (test/pattern.test.js
# ist sein Kennzeichen) und mit node_modules
mw=
for d in ${MWM_MICROWIZARD:-} "$repo/../microWizard-pattern-shadow" \
         "$repo/../../wt/microWizard-pattern-shadow" "$repo/../../microWizard"; do
  if [ -f "$d/test/pattern.test.js" ] && [ -d "$d/node_modules" ]; then
    mw=$(CDPATH= cd -- "$d" && pwd)
    break
  fi
done

if [ -n "$mw" ]; then
  echo "# Muster-Spur gegen $mw"
  node "$harness/pattern-trace.js" "$mw" 7 300 | cmp -s - "$here/fixtures/pattern-trace.txt" || {
    echo "not ok - test/fixtures/pattern-trace.txt ist nicht, was microwizard sagt" >&2
    exit 1
  }
elif [ -n "${MWM_MICROWIZARD:-}" ]; then
  echo "not ok - $MWM_MICROWIZARD hat kein test/pattern.test.js oder keine node_modules" >&2
  exit 1
else
  echo "# Muster-Spur: kein microwizard mit fix/pattern-shadow gefunden, nur die Golden File"
fi

echo "# make"
make -s -C "$repo" ${META:+META="$META"} $san all >/dev/null
make -s -C "$repo" ${META:+META="$META"} $san check

if [ -n "$mw" ]; then
  seed=${MWM_PATTERN_SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
  echo "# pattern_test gegen microwizard, Seed $seed (MWM_PATTERN_SEED=$seed wiederholt ihn)"
  node "$harness/pattern-trace.js" "$mw" "$seed" 2000 | "$build/pattern_test" - || {
    echo "not ok - pattern_test weicht bei Seed $seed von microwizard ab" >&2
    exit 1
  }
fi

[ "${1:-}" = unit ] && exit 0

if ss -uln 2>/dev/null | grep -q ":$port "; then
  echo "not ok - UDP $port ist belegt; ein alter Harness-Knoten? (pgrep -af 'base.js|echo.js')" >&2
  exit 1
fi

# die Knoten, die der Harness startet: keine Leck-Suche bei exit() (s. o.),
# und was die Sanitizer sagen, in Dateien - ihr stderr sieht hier keiner
if [ -n "$san" ]; then
  reports=$(mktemp -d)
  trap 'rm -rf "$reports"' EXIT
  export ASAN_OPTIONS="$ASAN_OPTIONS:detect_leaks=0:log_path=$reports/asan"
  export UBSAN_OPTIONS="$UBSAN_OPTIONS:log_path=$reports/ubsan"
fi

echo "# reference.js"
node "$harness/reference.js" "$build/mwm-node"

echo "# interop.js"
node "$harness/interop.js" "$build/mwm-node"

echo "# meshclient.js"
node "$harness/meshclient.js" "$build/mwm-node"

echo "# deadpin.js"
node "$harness/deadpin.js" "$build/mwm-node"

echo "# concurrency.js"
node "$harness/concurrency.js" "$build/mwm-node"

if [ -n "$san" ] && [ -n "$(ls -A "$reports")" ]; then
  echo "not ok - die Sanitizer haben etwas gefunden:" >&2
  cat "$reports"/* >&2
  exit 1
fi
