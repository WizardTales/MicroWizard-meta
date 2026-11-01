# Benchmarks

## Node microwizard against mwm-node

    make -C bench
    bench/loadgen.sh [build dir] [rounds 3] [seconds 2]

The same `cmd:add` on the same ndjson wire (`k:"aE"`), served once by the
Node service from microservice-benchmarks (`node-service.mjs`) and once by
`mwm-node`. The load comes from `loadgen.c`: C, connections spread over up to
six threads with one epoll each, a fixed number of calls in flight per
connection (closed loop), every answer checked by id and result. It reports
its own CPU as well, so one can see when the generator itself is the limit.

Measured on a Core Ultra 7 265H (P-cores 0–5, E-cores 6–13), Linux 7.2,
127.0.0.1: meta on 2026-11-01 with the microwizard-meta commit that adds these numbers
and metalanguage df5fa23, Node on 2026-10-31:

- Server on P-cores: Node and meta with 1 carrier on core 2, 2 carriers on
  2–3, 4 on 2–5, 8 on 0–7 (there are only six P-cores).
- Load generator on E-cores 8–13, never on the server's cores.
- Median of 3 rounds of 2 s. Cores not isolated, other work was running on
  the machine (load 4–7), so about ±10–15 %.
- Node 18.14.2 with microwizard 1.1.1 as in the benchmark repository, and
  Node 22.22.2 with microwizard 1.5.0.
- No call failed.

Node runs on one core, so the fair comparison is the column
"meta, 1 carrier".

### 1000 calls at once

The figure from the README of microservice-benchmarks (`c=1000`). Rate,
then p50 / p99:

| Server | 1 connection × 1000 calls | 1000 connections × 1 call |
|---|---|---|
| Node 18, microwizard 1.1.1 | 172 k/s, 5.5 / 9.7 ms | 108 k/s, 8.9 / 11.5 ms |
| Node 22, microwizard 1.5.0 | 189 k/s, 5.1 / 6.3 ms | 115 k/s, 8.4 / 10.2 ms |
| meta, 1 carrier | 2.48 M/s, 0.39 / 0.52 ms | 177 k/s, 5.4 / 7.3 ms |
| meta, 4 carriers | 2.23 M/s, 0.43 / 0.95 ms | 480 k/s, 1.6 / 5.8 ms |
| meta, 8 carriers | 2.24 M/s, 0.43 / 0.97 ms | 501 k/s, 1.9 / 4.1 ms |

- One connection is always served by one carrier; more carriers do not
  help there.
- At 1000 connections with 4 or 8 carriers the load generator is saturated
  (91–100 % of its cores): lower bounds.
- A connection's reader answers at most 32 lines or 64 KiB per turn,
  however many reads that takes, and then yields, and a carrier serves
  what its poll finds ahead of its queue only while nothing queued has
  waited 250 µs (metalanguage `doc/runtime.md`, "A saturated carrier left
  some readers waiting 100 ms" and "Sixty-four calls in flight kept a few
  connections waiting"). So every connection gets its turn, as in Node's
  event loop.

### Connections × calls in flight

| Connections × calls | Node 22, mw 1.5.0 | meta, 1 carrier | meta, 2 carriers | meta, 4 carriers | meta, 8 carriers |
|---|---|---|---|---|---|
| 1×1 | 36 k/s | 49 k/s | 47 k/s | 47 k/s | 50 k/s |
| 1×16 | 172 k/s | 570 k/s | 532 k/s | 548 k/s | 556 k/s |
| 1×64 | 184 k/s | 1.94 M/s | 1.77 M/s | 1.88 M/s | 1.85 M/s |
| 16×1 | 126 k/s | 182 k/s | 304 k/s | 470 k/s | 470 k/s |
| 16×16 | 179 k/s | 1.65 M/s | 2.66 M/s | 3.77 M/s | 4.39 M/s |
| 16×64 | 192 k/s | 2.33 M/s | 3.56 M/s | 5.03 M/s | 6.98 M/s |
| 64×1 | 124 k/s | 185 k/s | 300 k/s | 499 k/s | 522 k/s |
| 64×16 | 180 k/s | 1.62 M/s | 2.63 M/s | 3.85 M/s | 4.82 M/s |
| 64×64 | 191 k/s | 2.16 M/s | 3.64 M/s | 5.23 M/s | 6.72 M/s |
| 256×1 | 126 k/s | 184 k/s | 304 k/s | 474 k/s | 518 k/s |
| 256×16 | 179 k/s | 1.53 M/s | 2.57 M/s | 3.74 M/s | 4.93 M/s |
| 256×64 | 183 k/s | 2.12 M/s | 3.59 M/s | 5.10 M/s | 6.67 M/s |

p99:

| Connections × calls | Node 22, mw 1.5.0 | meta, 1 carrier | meta, 2 carriers | meta, 4 carriers | meta, 8 carriers |
|---|---|---|---|---|---|
| 1×1 | 0.06 ms | 0.04 ms | 0.05 ms | 0.04 ms | 0.04 ms |
| 1×16 | 0.18 ms | 0.05 ms | 0.07 ms | 0.07 ms | 0.06 ms |
| 1×64 | 0.56 ms | 0.08 ms | 0.09 ms | 0.08 ms | 0.09 ms |
| 16×1 | 0.24 ms | 0.29 ms | 0.29 ms | 0.10 ms | 0.12 ms |
| 16×16 | 1.74 ms | 0.29 ms | 0.22 ms | 0.16 ms | 0.14 ms |
| 16×64 | 5.90 ms | 0.50 ms | 0.45 ms | 0.32 ms | 0.30 ms |
| 64×1 | 1.00 ms | 0.40 ms | 0.32 ms | 0.40 ms | 0.34 ms |
| 64×16 | 6.16 ms | 0.69 ms | 0.75 ms | 0.64 ms | 1.51 ms |
| 64×64 | 23 ms | 2.00 ms | 2.23 ms | 2.56 ms | 3.15 ms |
| 256×1 | 2.69 ms | 1.51 ms | 1.44 ms | 2.88 ms | 1.08 ms |
| 256×16 | 25 ms | 3.01 ms | 3.28 ms | 3.08 ms | 4.13 ms |
| 256×64 | 671 ms | 8.13 ms | 8.91 ms | 12.3 ms | 6.29 ms |

- Server CPU per call (busy time of its cores, softirq included): Node
  5.2–5.8 µs with 16 or more in flight; meta with 1 carrier 0.4–0.65 µs
  from 1×64 and 16×16 on, 1.7 µs at 1×16.
- With 1 call per connection and 4/8 carriers, and with 8 carriers from 16
  in flight on, the load generator is close to its limit (60–100 %): lower
  bounds.
- With 64 calls in flight the p99 is 2–4 times what Little's law fixes
  as the mean (calls in flight over rate: 4.6 ms at 256×64 on two
  carriers, 3.2 ms on four): every connection waits about one turn of its
  carrier's queue, and connections do not spread over the carriers
  exactly evenly.

Raw data, one JSON line per run: meta in `results/2026-11-01/matrix.jsonl`
and `results/2026-11-01/1000.jsonl`, Node in the files of
`results/2026-10-31/`. `loadgen-table.py` turns them into tables.

## The benchmarks of microservice-benchmarks

    bench/run.sh [build dir] [rounds 3] [seconds 5]

The same measurements as WizardTales/microservice-benchmarks (in process,
Node client against service), with meta on the service and the client side,
all in a network namespace of its own. Here the Node client is part of the
measurement: at high concurrency it is the limit; for comparing servers use
`loadgen.sh`.
