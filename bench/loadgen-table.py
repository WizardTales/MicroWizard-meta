#!/usr/bin/env python3
"""The median of bench/loadgen.sh's rounds, as a table: one row per server,
connections and calls in flight; rps, p50/p99 in us, the server cores' busy
microseconds per call (softirq included) and the generator's busiest share.

    bench/loadgen.sh | bench/loadgen-table.py
"""
import json
import statistics
import sys
from collections import OrderedDict

runs = OrderedDict()
for line in sys.stdin:
    line = line.strip()
    if not line.startswith('{'):
        continue
    r = json.loads(line)
    runs.setdefault((r['server'], r['conns'], r['inflight']), []).append(r)

print('| server | conns | inflight | rps | p50 us | p99 us | srv core-us/call | srv proc-us/call | gen cores busy | failed |')
print('|---|---|---|---|---|---|---|---|---|---|')
for (server, conns, inflight), rs in runs.items():
    med = lambda k: statistics.median(r.get(k, 0) for r in rs)
    print(f"| {server} | {conns} | {inflight} | {med('rps'):,.0f} | {med('p50us'):.0f} | "
          f"{med('p99us'):.0f} | {med('srvCoreUsPerCall'):.2f} | {med('srvusPerCall'):.2f} | "
          f"{med('genCoresBusy'):.2f} | {sum(r['failed'] for r in rs)} |")
