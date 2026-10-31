#!/bin/sh
#
# The benchmarks of WizardTales/microservice-benchmarks with meta, all of it
# in a network namespace of its own (unshare -rn, lo only): nothing here can
# reach or be reached by any mesh on the machine, 39000/39001 and
# microwizard's default 39999 included. Inside, everything sits on
# 127.0.0.1, ports 38xxx.
#
#   bench/run.sh [build dir, bench/build] [rounds 3] [seconds per run 5]
#
# Needs: make -C bench first; for the Node sides, a checkout of
# microservice-benchmarks with its node_modules and microwizard in MWB_DIR,
# and node in PATH. One JSON line per run on stdout, the round in "round".

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
B=${1:-$here/build}
ROUNDS=${2:-3}
SECONDS_PER=${3:-5}

if [ "${MWM_BENCH_INSIDE:-}" != 1 ]; then
  MWM_BENCH_INSIDE=1 exec unshare -rn "$0" "$B" "$ROUNDS" "$SECONDS_PER"
fi

ip link set lo up

META_PORT=38101
NODE_PORT=38201

"$B/bench-node" --isbase --port 38990 --tag mwmbench --pin cmd:add \
  --listen-port $META_PORT 2>/dev/null &
meta_pid=$!
node "$here/node-service.mjs" $NODE_PORT 2>/dev/null &
node_pid=$!
trap 'kill $meta_pid $node_pid 2>/dev/null' EXIT
sleep 1

tag() { sed "s/^{/{\"round\":$1,\"pair\":\"$2\",/"; }

for r in $(seq 1 "$ROUNDS"); do
  "$B/bench-local" "$SECONDS_PER" 1 | tag "$r" local
  for c in 1 2 1000; do
    for mode in actE act actStr; do
      node "$here/node-client.mjs" 127.0.0.1 $META_PORT $c "$SECONDS_PER" $mode | tag "$r" node-meta
    done
    "$B/bench-client" 127.0.0.1 $META_PORT $c "$SECONDS_PER" actE | tag "$r" meta-meta
    "$B/bench-client" 127.0.0.1 $NODE_PORT $c "$SECONDS_PER" actE | tag "$r" meta-node
  done
done
