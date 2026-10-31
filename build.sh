#!/bin/sh
#
# Builds mwm-node and the unit tests into build/.
#
#   ./build.sh               with `meta` from PATH, otherwise ../../metalanguage/meta
#   META=/path/meta ./build.sh
#
# The recipe itself is the Makefile; this only finds meta.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -z "${META:-}" ]; then
  META=$(command -v meta 2>/dev/null || echo "$here/../../metalanguage/meta")
fi

[ -x "$META" ] || { echo "no meta at $META - set META" >&2; exit 1; }

make -C "$here" META="$META" all
echo "$here/build/mwm-node"
