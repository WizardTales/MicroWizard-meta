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
Linux 7.2, 127.0.0.1:

- Server on P-cores: Node and meta with 1 carrier on core 2, 2 carriers on
  2–3, 4 on 2–5, 8 on 0–7 (there are only six P-cores).
- Load generator on E-cores 8–13, never on the server's cores.
- Median of 3 rounds of 2 s. Cores not isolated, other work was running on
  the machine (load 1–12), so about ±10 %.
- Node 18.14.2 with microwizard 1.1.1 as in the benchmark repository;
  Node 22.22.2 with microwizard 1.5.0 measures the same (see below).
- No call failed.

Node runs on one core, so the fair comparison is the column
"meta, 1 carrier".

### 1000 calls at once

The figure from the README of microservice-benchmarks (`c=1000`).

| Server | 1 connection × 1000 calls | 1000 connections × 1 call |
|---|---|---|
| Node 18, microwizard 1.1.1 | 169 k/s | 102 k/s |
| Node 22, microwizard 1.5.0 | 187 k/s | 104 k/s |
| meta, 1 carrier | 1.91 M/s | 201 k/s |
| meta, 4 carriers | 1.82 M/s | 491 k/s |
| meta, 8 carriers | 1.71 M/s | 506 k/s |

p99:

| Server | 1 connection × 1000 calls | 1000 connections × 1 call |
|---|---|---|
| Node 18 | 13.9 ms | 13.4 ms |
| Node 22 | 6.3 ms | 11.5 ms |
| meta, 1 carrier | 0.93 ms | 98.6 ms |
| meta, 4 carriers | 1.08 ms | 98.6 ms |
| meta, 8 carriers | 1.15 ms | 4.5 ms |

- One connection is always served by one carrier; more carriers do not
  help there.
- At 1000 connections with 4 or 8 carriers the load generator is saturated
  (93–100 % of its cores): lower bounds.
- The 98.6 ms are a fairness issue of the runtime, not a throughput issue.
  A busy carrier looks for ready sockets every 10 µs and puts their readers
  ahead of its queue (`META_POLL_US` in metalanguage
  `runtime/meta_tasks.c`). Most calls are served at once (p50 27 µs), a few
  get overtaken again and again. On average a call waits 5 ms with meta
  (1000 / 201 k/s) and 9.8 ms with Node; Node serves every connection once
  per turn of its event loop and spreads the wait evenly. With 8 carriers
  the server is not saturated, its queue runs dry, and p99 falls to 4.5 ms.

### Connections × calls in flight

| Connections × calls | Node 18, mw 1.1.1 | meta, 1 carrier | meta, 2 carriers | meta, 4 carriers | meta, 8 carriers |
|---|---|---|---|---|---|
| 1×1 | 35 k/s | 54 k/s | 50 k/s | 50 k/s | 50 k/s |
| 1×16 | 158 k/s | 607 k/s | 574 k/s | 560 k/s | 564 k/s |
| 1×64 | 176 k/s | 1.29 M/s | 1.20 M/s | 1.17 M/s | 1.18 M/s |
| 16×1 | 125 k/s | 193 k/s | 353 k/s | 514 k/s | 485 k/s |
| 16×16 | 173 k/s | 1.73 M/s | 2.74 M/s | 3.91 M/s | 4.50 M/s |
| 16×64 | 187 k/s | 2.89 M/s | 4.61 M/s | 6.31 M/s | 7.55 M/s |
| 64×1 | 126 k/s | 197 k/s | 325 k/s | 506 k/s | 528 k/s |
| 64×16 | 177 k/s | 1.74 M/s | 2.77 M/s | 3.73 M/s | 5.22 M/s |
| 64×64 | 186 k/s | 2.82 M/s | 4.41 M/s | 5.95 M/s | 7.89 M/s |
| 256×1 | 123 k/s | 200 k/s | 343 k/s | 514 k/s | 527 k/s |
| 256×16 | 176 k/s | 1.72 M/s | 2.74 M/s | 4.00 M/s | 5.05 M/s |
| 256×64 | 181 k/s | 2.64 M/s | 4.36 M/s | 6.10 M/s | 7.88 M/s |

- Server CPU per call (busy time of its cores, softirq included): Node
  5.4–5.9 µs, meta with 1 carrier 0.35–0.58 µs at 16 or more in flight.
- With 1 call per connection and 4/8 carriers, and with 8 carriers from 16
  in flight on, the load generator is close to its limit (65–100 %): lower
  bounds.

Raw data, one JSON line per run: `results/2026-10-31/matrix.jsonl` and
`results/2026-10-31/1000.jsonl`. `loadgen-table.py` turns them into tables.

## The benchmarks of microservice-benchmarks

    bench/run.sh [build dir] [rounds 3] [seconds 5]

The same measurements as WizardTales/microservice-benchmarks (in process,
Node client against service), with meta on the service and the client side,
all in a network namespace of its own. Here the Node client is part of the
measurement: at high concurrency it is the limit; for comparing servers use
`loadgen.sh`.
