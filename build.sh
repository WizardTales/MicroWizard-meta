#!/bin/sh
#
# Baut mwm-node und die Unit-Tests nach build/.
#
#   ./build.sh               mit `meta` aus dem PATH, sonst ../../metalanguage/meta
#   META=/pfad/meta ./build.sh
#
# Das eigentliche Rezept ist das Makefile; das hier sucht nur meta.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -z "${META:-}" ]; then
  META=$(command -v meta 2>/dev/null || echo "$here/../../metalanguage/meta")
fi

[ -x "$META" ] || { echo "kein meta unter $META - META setzen" >&2; exit 1; }

make -C "$here" META="$META" all
echo "$here/build/mwm-node"
