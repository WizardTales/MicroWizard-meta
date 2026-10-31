# microwizard-meta

[MicroWizard](https://github.com/WizardTales/MicroWizard) – the
reimplementation of seneca and seneca-mesh – in [meta](#building). The goal:
services written in meta that join an existing MicroWizard mesh, are called
from Node with `seneca.actAsync` and call other services themselves,
wire-compatible with microwizard 1.5.0 (the calling side as 1.6.2, consume as
fix/consume-wait, pattern matching as fix/pattern-shadow, pins without a
provider as fix/dead-pin).

| Path | Contents |
| --- | --- |
| [docs/protocol.md](docs/protocol.md) | both protocols as they are on the wire: SWIM over UDP, ndjson over TCP, msgpack as msgpackr writes it, visigoth, what a caller does with answers – read from the sources and checked against Node nodes |
| `src/msgpack.c` | msgpack in the forms of msgpackr 1.11.2: writer, cursor, document (`json_t`), typed representation |
| `src/swim.c` | SWIM as swim-ng 0.6.2 under sneeze-ng 0.13.0, without a socket and without a clock: join, ping/ack/ping-req, piggybacking, suspect → faulty, incarnations |
| `src/mesh.c` | sneeze's member table by `identifier$`, with the restart rule of sneeze-ng 0.13.1 |
| `src/visigoth.c` | target choice and circuit breaker of @wzrdtales/visigoth 2.0.5, decision for decision |
| `src/balance.c` | consume from balance.js as steps without a clock: with the `patient` option (default) as fix/consume-wait, without it as 1.6.2 |
| `src/pattern.c` | the pattern tree and `#matchPin` of microwizard fix/pattern-shadow, for both sides: which pin gets a message |
| `src/client.c` | the calling side: targets per pin, own handlers and remote pins in one tree, one ndjson connection per target, `mwm_act`/`mwm_actE` |
| `src/service.c` | the serving side of calls: line in, handler by pattern, answer line out |
| `src/echo.c` | the echo service of the tests, the same commands as `test/harness/echo.js` |
| `src/main.c` | `mwm-node`: one listen per pin (UDP, SWIM, table, TCP), one `select` over sockets, signals and deadline, calls from stdin |
| `test/unit/` | unit tests in meta, against golden files from real Node nodes |
| `test/harness/` | the reference in Node (microwizard 1.5.0, versions pinned): base, echo service, client, capture, comparisons |
| `test/fixtures/` | the golden files; `docs/protocol.md` section 13 says where each comes from |
| `bench/` | benchmarks against Node microwizard, see [bench/README.md](bench/README.md) |

## Building

meta writes C, and the C is compiled. The TCP part needs `meta_tcp.h`,
`meta_pollUntil`, `meta_signalFd`, the ownership rules and `json_t.truthy()`
from metalanguage parser-rewrite 6b2b20e or later:

    META=../../metalanguage/meta ./build.sh      # build/mwm-node
    META=../../metalanguage/meta make check      # unit tests only
    META=../../metalanguage/meta make SAN=1 check   # with ASan and UBSan, in build-san/

Without `META` it takes `meta` from PATH, otherwise `../../metalanguage/meta`,
and says so when that one lacks `meta_tcp.h`.

## Testing

Everything with one command, everything on 127.0.0.1:

    META=../../metalanguage/meta test/run.sh

1. the harness's Node dependencies (`pnpm install --offline`, otherwise from
   the registry)
2. the visigoth trace regenerated from the real visigoth and compared with
   the golden file; the pattern trace regenerated from microwizard
   fix/pattern-shadow when a checkout of it is found (`MWM_MICROWIZARD`,
   otherwise looked for beside the repository)
3. `make` and the unit tests: msgpack (against bytes from msgpackr and
   captured SWIM bodies), visigoth (3000 steps against the JS trace),
   balance (consume on a fake clock, as 1.6.2 and with `patient`, including
   the cases of `test/balance.test.js` from fix/consume-wait), swim (rules on
   a fake clock), mesh (the member rules), pattern (the cases of
   `test/pattern.test.js` from fix/pattern-shadow one to one, then the
   pattern trace: the same winner as microwizard for every message),
   service (line by line), client (against servers of its own that
   misbehave on request: with `--no-patient` as 1.6.2, then `patient`, then
   `patient` with a 400 ms timeout); with the microwizard checkout also
   `pattern_test` against a fresh trace with a random seed
   (`MWM_PATTERN_SEED` repeats one)
4. `reference.js`: every call case against the Node echo service and the
   meta echo service; both must give the same result – also as a gateway
   sees it (`actAsync().then(defaultError)`, graphql-js)
5. `interop.js`: mwm-node joins a Node base, Node sees it with its pin, it
   sees the Node nodes; killed it is faulty for Node, restarted it is back; a
   killed Node service is faulty for mwm-node
6. `meshclient.js`: mwm-node `--client --no-patient` calls the Node and the
   meta echo and gets in every case what a Node caller gets; round robin and
   failover over two Node echos, one of them killed; a service restarted on
   the same ports replaces its old target at once; two pins in mwm-node look
   to Node like two listens of one Node service
7. `deadpin.js`: A serves `role:deadpin,cmd:ping`, B `role:deadpin,cmd:*`,
   mwm-node's client calls `cmd:ping` – A answers; A killed, B answers (not
   `no-current-target` forever); A restarted, A again (as
   `test/dead-pin-mesh.test.js` from fix/dead-pin)
8. `concurrency.js`: 200 calls at once with 0–500 ms delay over one
   connection – Node caller → meta echo (and as reference → Node echo), raw
   ndjson → meta echo, meta client → Node and meta echo: every answer with
   its token, none lost or doubled, in the order they finish, all within
   about the longest delay; a ping behind eight slow calls comes back at
   once; above `--max-inflight` overload at once, every call settles for the
   Node and the meta caller alike, and afterwards calls go through again;
   SIGTERM with calls running

`test/run.sh unit` does 1–3 only. `MWM_VERBOSE=1` shows every line of the
nodes in `interop.js`, `meshclient.js` and `concurrency.js`.
`MWM_SAN=1 test/run.sh` runs everything with ASan and UBSan; whatever the
sanitizers find in the harness's nodes fails the run.

**Safety.** The harness's base node sits on UDP 38999 (`MWM_BASE_PORT`), the
harness's meta nodes and the servers of `client_test` on fixed ports in
389xx – not on 39999 (microwizard's default) and not on 39000/39001 (often
used for a local dev mesh), with a tag of their own, `mwmtest`; every
`wzdc_*` variable of the shell is dropped, multicast, guess and DNS seed are
off. If 38999 is taken, `test/run.sh` stops. mwm-node itself sends only to
127.0.0.0/8, listens only there, dials only there and resolves no names;
`--any-address` lifts that fence. SWIM and the ndjson calls have no
authentication, so only lift it on a network where every peer is trusted.

## mwm-node

    mwm-node --bases 127.0.0.1:38999 --tag mwmtest --pin 'role:meta,cmd:*' \
             --echo meta --events

    # two pins, two listens; and calls from stdin, one JSON line per call
    mwm-node --bases 127.0.0.1:38999 --tag mwmtest --pin 'role:a,cmd:*' \
             --pin 'role:b,cmd:*' --client
    {"tag":1,"act":"role:echo,cmd:ping","args":{"n":1}}

Configuration as for a Node service with microwizard, from
`wzdc_seneca__bases`, `__host`, `__port`, `__tag`, plus the switches above;
the head of `src/main.c` describes them all. `--events` writes one JSON line
per mesh event (`started`, `joined`, `add`, `remove`, each with `listen`) and
per client target (`target-add`, `target-remove`). SIGTERM works like
`role:mc,cmd:close`: from then on `retry_later_err_overload`, the running
calls finish, and 500 ms after the last one it exits.

Every call runs in a task of its own, as every call in Node is a promise of
its own; at most `--max-inflight` (10000) at once over all connections,
above that the reader answers `retry_later_err_overload` with `details` at
once – the overload that balance.js counts and retries elsewhere.

The client calls like consume from microwizard fix/consume-wait: after a
failed attempt with no target to choose, wait once until the target is half
open, and the whole call within one request timeout (`--timeout`, 5555 ms).
`--no-patient` calls exactly as 1.6.2 (docs/protocol.md, section 11).

## Load

Node microwizard against mwm-node, with a load generator in C on cores of
its own: [bench/README.md](bench/README.md). At 1000 calls at once over one
connection, 1.91 M/s against 187 k/s (Node 22, microwizard 1.5.0), both on
one core.

## Regenerating the golden files

    cd test/harness
    node capture.js /tmp/cap.jsonl 3 5         # base, echo, client; the echo gets killed
    node fixtures.js /tmp/cap.jsonl ../fixtures
    node pack-samples.js > ../fixtures/msgpackr-samples.txt
    node visigoth-trace.js 7 3000 > ../fixtures/visigoth-trace.txt

## License

MIT, see [LICENSE](LICENSE).
