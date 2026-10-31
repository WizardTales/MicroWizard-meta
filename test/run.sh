#!/bin/sh
#
# Everything that needs no network beyond 127.0.0.1, in one run:
#
#   1. the harness's Node dependencies (pnpm, offline from the store,
#      otherwise from the registry)
#   2. the golden file of the visigoth trace regenerated from the real
#      visigoth and compared - it must never be edited by hand; likewise
#      the pattern trace against microwizard fix/pattern-shadow when there
#      is a checkout of it (MWM_MICROWIZARD)
#   3. make: mwm-node and the unit tests in meta (msgpack, visigoth,
#      balance, swim, mesh, pattern, service, client - the last three
#      times: as 1.6.2 (--no-patient), with patient, the default, and with a
#      short timeout); then pattern_test against a fresh trace with a random
#      seed (MWM_PATTERN_SEED), winner by winner against microwizard
#   4. reference.js: what a Node caller gets back from a Node service
#   5. interop.js: mwm-node in a mesh with Node nodes
#   6. meshclient.js: mwm-node's client in the mesh, compared with Node
#   7. deadpin.js: a pin whose last member is gone drops out of the
#      client's matching (microwizard fix/dead-pin)
#   8. concurrency.js: many calls at once over one connection, in both
#      directions, the --max-inflight cap and SIGTERM with calls running
#
# All on 127.0.0.1. The harness's base node sits on UDP 38999
# (MWM_BASE_PORT), not on 39999 (microwizard's default) and not on
# 39000/39001 (often used for a local dev mesh); if the port is taken this
# stops instead of joining somebody else's mesh.
#
#   test/run.sh            everything
#   test/run.sh unit       1-3 only
#   META=/path/meta test/run.sh
#   MWM_SAN=1 test/run.sh  everything with ASan and UBSan (build-san/)
#   MWM_MICROWIZARD=/path/to/MicroWizard test/run.sh
#                          the checkout with the matcher of fix/pattern-shadow;
#                          without it one beside the repository is looked
#                          for, otherwise the golden file is all there is

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
  # mwm-node ends with exit() from a task whose stack the leak check does
  # not see - whatever only a task stack holds would count as lost; the
  # unit tests end in main and are checked for leaks
  export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=1}
  export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
fi

# nothing from a wzdc configuration in the shell may reach in here
for v in $(env | sed -n 's/^\(wzdc_[^=]*\)=.*/\1/p'); do unset "$v"; done

if [ ! -d "$harness/node_modules/microwizard" ]; then
  echo "# Harness: pnpm install"
  (cd "$harness" && { pnpm install --offline >/dev/null 2>&1 || pnpm install >/dev/null; })
fi

echo "# visigoth trace against @wzrdtales/visigoth 2.0.5"
node "$harness/visigoth-trace.js" 7 3000 | cmp -s - "$here/fixtures/visigoth-trace.txt" || {
  echo "not ok - test/fixtures/visigoth-trace.txt is not what visigoth says" >&2
  exit 1
}

# microwizard with the matcher of fix/pattern-shadow (test/pattern.test.js
# marks it) and with node_modules
mw=
for d in ${MWM_MICROWIZARD:-} "$repo/../MicroWizard" "$repo/../microWizard"; do
  if [ -f "$d/test/pattern.test.js" ] && [ -d "$d/node_modules" ]; then
    mw=$(CDPATH= cd -- "$d" && pwd)
    break
  fi
done

if [ -n "$mw" ]; then
  echo "# pattern trace against $mw"
  node "$harness/pattern-trace.js" "$mw" 7 300 | cmp -s - "$here/fixtures/pattern-trace.txt" || {
    echo "not ok - test/fixtures/pattern-trace.txt is not what microwizard says" >&2
    exit 1
  }
elif [ -n "${MWM_MICROWIZARD:-}" ]; then
  echo "not ok - $MWM_MICROWIZARD has no test/pattern.test.js or no node_modules" >&2
  exit 1
else
  echo "# pattern trace: no microwizard with fix/pattern-shadow found, the golden file only"
fi

echo "# make"
make -s -C "$repo" ${META:+META="$META"} $san all >/dev/null
make -s -C "$repo" ${META:+META="$META"} $san check

if [ -n "$mw" ]; then
  seed=${MWM_PATTERN_SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
  echo "# pattern_test against microwizard, seed $seed (MWM_PATTERN_SEED=$seed repeats it)"
  node "$harness/pattern-trace.js" "$mw" "$seed" 2000 | "$build/pattern_test" - || {
    echo "not ok - pattern_test differs from microwizard at seed $seed" >&2
    exit 1
  }
fi

[ "${1:-}" = unit ] && exit 0

if ss -uln 2>/dev/null | grep -q ":$port "; then
  echo "not ok - UDP $port is taken; an old harness node? (pgrep -af 'base.js|echo.js')" >&2
  exit 1
fi

# the nodes the harness starts: no leak check at exit() (see above), and
# what the sanitizers say goes to files - nobody sees their stderr here
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
  echo "not ok - the sanitizers found something:" >&2
  cat "$reports"/* >&2
  exit 1
fi
