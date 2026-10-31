#!/bin/sh
#
# Node microwizard against mwm-node, measured with bench/loadgen: the same
# cmd:add on the same wire, servers and generator pinned to disjoint cores.
#
#   bench/loadgen.sh [build dir, bench/build] [rounds 3] [seconds 2]
#
# On 127.0.0.1, ports 38xxx, without a namespace: mwm-node is a lone base on
# SWIM port 38990 with no bases, so nothing goes to a mesh port, and the
# loopback fence holds. The core sets are for the Core Ultra 7 265H this was
# written on (P 0-5, E 6-13, LP-E 14-15) and can be set from outside:
#
#   SRV1 SRV2 SRV4 SRV8   the server's cores for 1, 2, 4, 8 of them
#   GEN                   the generator's cores (6 threads at most)
#   NODES                 "version:MWB_DIR ..." of the Node services
#   CARRIERS CONNS INFLIGHT   what to go through
#
# One JSON line per run on stdout, with "server", "round" and "cores".

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
B=${1:-$here/build}
ROUNDS=${2:-3}
SECONDS_PER=${3:-2}

SRV1=${SRV1:-2}
SRV2=${SRV2:-2-3}
SRV4=${SRV4:-2-5}
SRV8=${SRV8:-0-7}
GEN=${GEN:-8-13}
GEN_THREADS=6
NODES=${NODES:-18.14.2:${MWB_DIR:-$here/../../microservice-benchmarks}}
CARRIERS=${CARRIERS:-1 2 4 8}
CONNS=${CONNS:-1 16 64 256}
INFLIGHT=${INFLIGHT:-1 16 64}

PORT=38301

unset wzdc_seneca__bases wzdc_baseName 2>/dev/null || true

coresFor() { eval echo "\$SRV$1"; }

# measure SERVER-NAME CORES PID
measure() {
  for r in $(seq 1 "$ROUNDS"); do
    for c in $CONNS; do
      for f in $INFLIGHT; do
        t=$GEN_THREADS
        [ "$c" -lt "$t" ] && t=$c
        LG_SRV_CPUS=$2 LG_GEN_CPUS=$GEN taskset -c "$GEN" \
          "$B/loadgen" 127.0.0.1 $PORT "$c" "$t" "$f" "$SECONDS_PER" aE 0.5 "$3" |
          sed "s/^{/{\"server\":\"$1\",\"round\":$r,\"cores\":\"$2\",/"
      done
    done
  done
}

for spec in $NODES; do
  v=${spec%%:*}
  dir=${spec#*:}
  MWB_DIR=$dir taskset -c "$SRV1" "$HOME/.nvm/versions/node/v$v/bin/node" \
    "$here/node-service.mjs" $PORT 2>/dev/null &
  pid=$!
  sleep 1.5
  mw=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$dir/node_modules/microwizard/package.json")
  measure "node$v-mw$mw" "$SRV1" $pid
  kill $pid
  wait $pid 2>/dev/null || true
done

for n in $CARRIERS; do
  cores=$(coresFor "$n")
  META_CARRIERS=$n taskset -c "$cores" "$B/bench-node" --isbase --port 38990 \
    --tag mwmbench --pin cmd:add --listen-port $PORT 2>/dev/null &
  pid=$!
  sleep 1
  measure "meta-c$n" "$cores" $pid
  kill $pid
  wait $pid 2>/dev/null || true
done
