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

Measured 2026-10-31 on a Core Ultra 7 265H (P-cores 0–5, E-cores 6–13),
Linux 7.2, 127.0.0.1, with microwizard-meta 1f89586 on metalanguage 2769af7:

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
| meta, 1 carrier | 2.54 M/s, 0.39 / 0.51 ms | 189 k/s, 5.1 / 6.2 ms |
| meta, 4 carriers | 2.28 M/s, 0.43 / 0.97 ms | 487 k/s, 1.7 / 5.1 ms |
| meta, 8 carriers | 2.30 M/s, 0.42 / 1.00 ms | 510 k/s, 1.9 / 3.9 ms |

- One connection is always served by one carrier; more carriers do not
  help there.
- At 1000 connections with 4 or 8 carriers the load generator is saturated
  (91–100 % of its cores): lower bounds.
- A connection's reader answers at most 32 lines or 64 KiB per turn and
  then yields, and a carrier serves what its poll finds ahead of its queue
  only while nothing queued has waited 250 µs (metalanguage
  `doc/runtime.md`, "A saturated carrier left some readers waiting 100
  ms"). So every connection gets its turn, as in Node's event loop.

### Connections × calls in flight

| Connections × calls | Node 22, mw 1.5.0 | meta, 1 carrier | meta, 2 carriers | meta, 4 carriers | meta, 8 carriers |
|---|---|---|---|---|---|
| 1×1 | 36 k/s | 51 k/s | 49 k/s | 49 k/s | 48 k/s |
| 1×16 | 172 k/s | 543 k/s | 543 k/s | 550 k/s | 556 k/s |
| 1×64 | 184 k/s | 1.83 M/s | 1.90 M/s | 1.86 M/s | 1.84 M/s |
| 16×1 | 126 k/s | 163 k/s | 305 k/s | 490 k/s | 456 k/s |
| 16×16 | 179 k/s | 1.36 M/s | 2.40 M/s | 3.99 M/s | 4.48 M/s |
| 16×64 | 192 k/s | 1.96 M/s | 3.17 M/s | 5.43 M/s | 6.74 M/s |
| 64×1 | 124 k/s | 165 k/s | 291 k/s | 492 k/s | 525 k/s |
| 64×16 | 180 k/s | 1.43 M/s | 2.52 M/s | 3.94 M/s | 5.15 M/s |
| 64×64 | 191 k/s | 1.87 M/s | 3.22 M/s | 5.20 M/s | 7.07 M/s |
| 256×1 | 126 k/s | 165 k/s | 310 k/s | 481 k/s | 517 k/s |
| 256×16 | 179 k/s | 1.43 M/s | 2.39 M/s | 3.79 M/s | 5.08 M/s |
| 256×64 | 183 k/s | 1.86 M/s | 3.20 M/s | 5.31 M/s | 6.99 M/s |

p99:

| Connections × calls | Node 22, mw 1.5.0 | meta, 1 carrier | meta, 2 carriers | meta, 4 carriers | meta, 8 carriers |
|---|---|---|---|---|---|
| 1×1 | 0.06 ms | 0.04 ms | 0.04 ms | 0.04 ms | 0.04 ms |
| 1×16 | 0.18 ms | 0.05 ms | 0.07 ms | 0.07 ms | 0.07 ms |
| 1×64 | 0.56 ms | 0.08 ms | 0.08 ms | 0.09 ms | 0.09 ms |
| 16×1 | 0.24 ms | 0.29 ms | 0.29 ms | 0.11 ms | 0.12 ms |
| 16×16 | 1.74 ms | 0.32 ms | 0.32 ms | 0.31 ms | 0.25 ms |
| 16×64 | 5.90 ms | 1.83 ms | 4.85 ms | 2.62 ms | 0.67 ms |
| 64×1 | 1.00 ms | 0.43 ms | 0.37 ms | 0.43 ms | 0.34 ms |
| 64×16 | 6.16 ms | 0.77 ms | 0.72 ms | 0.67 ms | 1.34 ms |
| 64×64 | 23 ms | 9.70 ms | 24 ms | 7.60 ms | 1.97 ms |
| 256×1 | 2.69 ms | 1.70 ms | 1.47 ms | 3.21 ms | 1.05 ms |
| 256×16 | 25 ms | 3.15 ms | 3.28 ms | 3.41 ms | 4.00 ms |
| 256×64 | 671 ms | 25 ms | 67 ms | 53 ms | 7.86 ms |

- Server CPU per call (busy time of its cores, softirq included): Node
  5.2–5.8 µs with 16 or more in flight; meta with 1 carrier 0.5–0.75 µs
  from 1×64 and 16×16 on, 1.8 µs at 1×16.
- With 1 call per connection and 4/8 carriers, and with 8 carriers from 16
  in flight on, the load generator is close to its limit (60–100 %): lower
  bounds.
- With 64 calls in flight on 2 or 4 carriers, p99 still reaches 8–67 ms
  while p50 stays at 0.04–0.05 ms: deep pipelines over several carriers
  still leave a few calls waiting. Open.

Raw data, one JSON line per run: `results/2026-10-31/matrix.jsonl` and
`results/2026-10-31/1000.jsonl`. `loadgen-table.py` turns them into tables.

## The benchmarks of microservice-benchmarks

    bench/run.sh [build dir] [rounds 3] [seconds 5]

The same measurements as WizardTales/microservice-benchmarks (in process,
Node client against service), with meta on the service and the client side,
all in a network namespace of its own. Here the Node client is part of the
measurement: at high concurrency it is the limit; for comparing servers use
`loadgen.sh`.
