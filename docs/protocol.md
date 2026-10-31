# The MicroWizard protocol as it is on the wire

What a service in meta has to do to take part in a MicroWizard mesh and be
called from Node with `seneca.actAsync` – read from the sources, not from
memory, and where possible checked against running Node nodes. Every
statement here carries one of three markers:

- **[measured]** – observed by the harness in `test/harness/`, and a test
  pins it down (`reference.js`, `interop.js`, the golden files)
- **[source]** – read from the code, not observed separately
- **[open]** – read from the code, still to be checked

The versions, pinned in the harness (`test/harness/pnpm-workspace.yaml`):

| Package | Version | for |
| --- | --- | --- |
| microwizard | 1.5.0 | `index.js` (patterns, act), `mesh.js` (mesh), `transfer.js`, `tcp.js` (calls), `balance.js` (distribution) |
| sneeze-ng | 0.13.0 | `sneeze.js`: members by identifier, join retries |
| @wzrdtales/swim-ng | 0.6.2 | `lib/`: SWIM itself |
| msgpackr | 1.11.2 | the codec of swim-ng |
| @wzrdtales/visigoth | 2.0.5 | target selection and circuit breaker |
| ndjson | 2.0.0 | the framing of the calls |

Where the versions differ, the calling side of mwm-node (`src/client.c`)
follows the newer one: `balance.js` from microwizard 1.6.2 (a retry gets its
own `id`) with the consume from fix/consume-wait (`patient`, the default;
`--no-patient` is 1.6.2), the reply handling from microwizard
fix/unknown-reply (unusable replies no longer cost the connection) and the
membership rule from sneeze-ng fix/restart-same-host (0.13.1). Patterns are
matched on both sides as in microwizard fix/pattern-shadow, and a pin
without a provider drops out as in fix/dead-pin. Sections 8, 9, 10 and 11
each say what changed.

Two protocols: **SWIM over UDP** says who is there and which pattern each
one serves; **ndjson over TCP** carries the calls. In between sits a
distribution layer (balance.js with visigoth) that decides which of several
providers of a pattern gets a call.

---

## 1. Configuration

A service reads `rc('wzdc', defaults)`. From the environment,
`wzdc_a__b=x` becomes `{a: {b: 'x'}}` – always a string. [source]

| Key | Meaning |
| --- | --- |
| `wzdc_seneca__bases` | `"ip:port,ip:port"`. In the services usually split with `if (bases && bases.indexOf(','))` – `indexOf` is `-1`, and therefore truthy, when there is no comma in it, and `0` only when the string starts with a comma. So in practice it is always split. |
| `wzdc_seneca__host` | the address that is announced (TCP host and SWIM host) |
| `wzdc_seneca__port` | the SWIM port; without it: base 39999, otherwise random in [40000, 50000) |
| `wzdc_seneca__tag` | the mesh tag; members carry `tag$: "mc~<tag>"`, without a tag `"mc~mesh"` |
| `wzdc_baseName` | the name a `dnsSeed` resolves (for instance a headless Service in Kubernetes); `localhost` in the microwizard examples |

`seneca.monitor: true` turns on the terminal monitor in sneeze (keypress on
stdin, raw mode); `wzdc_seneca__monitor=false` turns it off. [source]

### Where the bases come from

`mesh.js` (`findBases`) asks `defined`, `custom`, `multicast`, `guess` in
turn and stops after the first source that returns something
(`discover.stop: true`). [source]

- **defined**: `options.sneeze.bases || options.bases || options.remotes`.
  If `wzdc_seneca__bases` is set, nothing else is asked – no DNS.
- **custom**: for instance a `dnsSeed`, a `dns.lookup(baseName, {all: true,
  family: 4})`. Returns bare addresses; `resolve_bases` appends `:39999`.
  An error in the lookup is *thrown*, from inside the callback – the
  process dies.
- **multicast**: `node-discover` with broadcast. Usually not turned off,
  but because of `stop` it never gets its turn as long as `custom` returns
  something.
- **guess**: `127.0.0.1:39999` (and `host:39999`), only if nothing came
  before.

`resolve_bases`: `"h:p"` stays, `":p"` becomes `127.0.0.1:p` (and in
addition `host:p`), `"h"` becomes `h:39999`. [source]

A base removes itself from the list (comparison with `host:port` as a
string) and stands alone if that empties the list – swim-ng then reports a
`JoinFailedError`, which sneeze swallows for a base. [source]

---

## 2. SWIM: the frame

### Datagram

A datagram is one message or a compound. [measured]

```
single message:    [type: u8] [msgpack body]
compound:          [0x00] [count: u16le] ( [length: u16le] [type: u8] [msgpack body] )*
```

| Type | Name | Body |
| --- | --- | --- |
| 0 | Compound | – |
| 1 | Compressed | defined, never sent, ignored on receipt |
| 2 | Encrypted | likewise |
| 10 | Ping | `{seq}` |
| 11 | PingReq | `{seq, destination}` |
| 12 | Sync | member data |
| 13 | Ack | `{seq}` |
| 14 | Update | member data |

The lengths in the compound are **little endian**, everything in the
msgpack body is big endian. A length that no longer fits in the buffer ends
reading the compound silently; a part that is shorter than stated is read
as far as it goes. A compound may contain compounds again (never seen).
[source]

`maxDgramSize` is 2048 (sneeze sets it; swim-ng itself would have 512). The
member data of a microwizard node is around 350 bytes [measured], so about
five updates fit in one datagram.

### How it is packed

**One message with piggyback** (`Net.sendMessage`, for Ping, Ack, PingReq
and the Update to a suspect): the room for updates is
`2048 − 1 − 2·2 − length(message)`. If updates fit, a compound
`[message, update, update, …]` goes out, otherwise the message alone. A
message without a body (does not occur) would be just the type byte.
[measured]

**Many messages** (`Net.sendMessages`, for Sync and the reply to it):
compounds without piggyback, each up to 2048 bytes. Starting value
`2048 − 1 − 2`; after the first full compound the code continues with
`2048 − 2` – it forgets the type byte, so a compound can end up one byte
over 2048. A single message that does not fit on its own is dropped (only a
debug log). Even a single Sync message goes out as a compound with one part.
[measured, `compound1-Sync` in `test/fixtures/swim-frames.txt`]

### Where replies go

Members are keyed by the string `host` from their data (`"ip:port"`), and
almost everything goes there – not to the source address of the datagram.
The exceptions go to the source address (`rinfo`): [source]

| Trigger | Reply | to |
| --- | --- | --- |
| Ping | Ack | source address |
| PingReq | Ping | `destination` from the PingReq |
| Ack to that Ping | Ack with the original `seq` | source address of the PingReq |
| Ack from a suspect | Update with its (suspect) state | source address of the Ack, looked up as member key |
| Sync | all members as Updates | `host` from the Sync |
| own Ping, PingReq | | `host` of the member |

`Net.sendBuffer` splits the key with `split(':')` – so IPv6 does not work.

### Binding

UDP is bound with `socket.bind(port)`, that is on **all interfaces**, no
matter what `host` says; `host` is only what is announced. The TCP listener,
on the other hand, binds to `host`, and on a **port chosen by the kernel**:
`mesh.js` passes a function as the port (`50000 + random`) that is never
called – `net.Server.listen(fn, host)` takes it as the callback, and the
port becomes 0. [measured: UDP `0.0.0.0:38999`, TCP `127.0.0.1:<ephemeral>`]

mwm-node binds UDP to `host`; in the mesh this makes no difference, because
everything is sent to the announced key anyway.

---

## 3. msgpack, as msgpackr writes it

swim-ng binds `msgpackr.pack` and `msgpackr.unpack` – the default instances
`new Packr({useRecords: false})` and `new Unpackr({useRecords: false})`.
Everything here is measured (`test/harness/pack-samples.js`,
`test/fixtures/msgpackr-samples.txt`).

### Writing

| JS value | Form | Example |
| --- | --- | --- |
| object | **always map16** (`0xde` + u16), even empty or with one key – never fixmap; from 65536 keys on msgpackr throws | `{seq:0}` → `de0001 a3736571 00` |
| array | fixarray < 16, array16, array32 | |
| string | by UTF-8 byte length: fixstr < 32, str8 < 256, str16, str32 | |
| integer in uint32 | fixint < 0x80 (because of `useRecords: false` up to 127, not just up to 63), `cc`, `cd`, `ce` | `128` → `cc80` |
| integer in int32, negative | negative fixint from −32, `d0`, `d1`, `d2` | `−33` → `d0df` |
| **any other number** | **float64** (`cb`) – including integers above 2³²−1 or below −2³¹ | `Date.now()` → `cb 427a18f44c65f000` |
| `-0` | fixint `00` (`-0 >>> 0 === -0`) | |
| BigInt | `d3` (int64) if it fits, otherwise `cf`; even `5n` becomes `d3 0000000000000005` | |
| `true` / `false` / `null` | `c3` / `c2` / `c0` | |
| `undefined` | **`d4 00 00`** (fixext 1, type 0) | |
| Buffer | bin8/16/32 | |

### Reading

- `cf` and `d3` become **BigInt**, not Number.
- `d4 00 00` becomes `undefined`.
- The keys of a map become properties of an object, so strings.

### The consequence for the incarnation

`incarnation` is `Date.now()` at startup – an integer around 1.79·10¹²,
so neither int32 nor uint32 – and therefore goes out **as float64**
[measured: `"incarnation":f:1793405011551` in every member message].
If a meta node sent it as uint64 (`cf`), it would arrive in Node as a
BigInt, and swim-ng does arithmetic with it:

- `data.incarnation > this.incarnation` still works between BigInt and
  Number
- `this.incarnation = data.incarnation + 1` throws a `TypeError` (BigInt
  plus Number) – in `Member.incarnate`, that is exactly when a node wants
  to refute a suspicion about itself
- `prev.incarnation === member.incarnation` in sneeze is false between `5n`
  and `5`: every update would be a new `add`

That is why `src/msgpack.c` writes every number by msgpackr's rule
(`mp_buf_t.number`/`.integer`): int32/uint32 in the smallest int form,
everything else as float64. Every numeric form is read; an incarnation as
float64 becomes int64 as long as it is integral (exact up to 2⁵³).

---

## 4. SWIM: the messages

All bodies are map16 with exactly these keys in this order.
[measured, `test/fixtures/swim-bodies.txt`]

```
Ping     {"seq": u}                              de0001 a3 736571 <seq>
Ack      {"seq": u}                              the seq of the Ping, unchanged
PingReq  {"seq": u, "destination": "ip:port"}
Sync     {"meta": …, "host": "ip:port", "state": u, "incarnation": f}
Update   {"meta": …, "host": "ip:port", "state": u, "incarnation": f}
```

- `seq` is a counter per Swim instance starting at 0, shared by Pings,
  PingReqs and forwarded Pings. An Ack is matched by `seq` alone; where it
  comes from is not checked.
- `state`: 0 alive, 1 suspect, 2 faulty. Any other value is silently
  ignored.
- `meta` is `undefined` for a member without meta, so `d4 00 00`.
- The order `meta, host, state, incarnation` comes from
  `Member.prototype.data()`; msgpackr writes keys in insertion order.

### The meta of a microwizard node

From `mesh.js` (`join`) and sneeze, for a service with
`listen: [{pin: 'role:echo,cmd:*', model: 'consume', type: 'tcp'}]`:
[measured]

```
{"meta": {
   "config": {
     "warn": {"unknown_message_id": true, "invalid_kind": true, "invalid_origin": true,
              "no_message_id": true, "message_loop": true, "own_message": true},
     "host": "127.0.0.1", "port": u:40709, "timeout": u:5555, "connectTimeout": u:355,
     "pin": ["role:echo,cmd:*"], "model": "consume", "type": "tcp", "ismesh": true},
   "instance": "51e269e35a4",
   "identifier$": "51e269e35a4~role:echo,cmd:*~1793405011550",
   "tag$": "mc~mwmtest",
   "v$": u:0},
 "host": "127.0.0.1:45640", "state": u:0, "incarnation": f:1793405011551}
```

- `config` is the answer of `listen`: the transport settings (`warn`,
  `host`, `port`, `timeout`, `connectTimeout`) with the listen options laid
  over them, keys ending in `$` removed. The order is that of the spread
  operation – first seen, first written.
- **`timeout` and `connectTimeout` are not decoration**: the client takes
  `config` into its TCP options (`{...settings, ...pinConfig}`). What a
  service announces here is how long a Node caller waits for it.
- `host`/`port` in `config` are the TCP address, the outer `host` is the
  SWIM address.
- `type` is missing on nodes without `listen` (base, gateway); the client
  then assumes TCP.
- `pin` is an **array with one string**, the pin as it was written
  (`role:mesh,base:true`, not sorted). A node without a pin (gateway,
  client) announces `["null:true"]`. Several pins in one `listen` would
  become an array inside the array; in practice this does not occur.
- `instance` is the process's `uid()`: 11 hex characters.
- `identifier$` = `instance ~ pin ~ Date.now()` (the array as a JS string).
  `incarnation` is a separate `Date.now()`, one millisecond later.
- **Every `listen` is a SWIM member of its own** with its own port and its
  own sneeze instance; a service with two listens appears twice – with the
  same `instance`, each with its own pin, TCP port and `identifier$`, and
  the same keys in `config`. [measured, `meshclient.js` 15] Each sneeze
  instance sees the other listens of the same process as members;
  `mesh.js` skips them (`clientInstance.id === meta.instance`).
- `sneezeOpts.port` is the same for all listens: with a fixed port
  (`wzdc_seneca__port`) only the first listen can bind. Without a fixed
  port each takes its own random one. [source] mwm-node, with `--port N`,
  gives the second pin N+1 and so on.

What the other side strictly needs from this [source]: `config.pin` as a
non-empty array (`removeClient` reads `meta.config.pin[0]` and throws
otherwise), `config.host`/`config.port`, `identifier$` (unique, part of the
client identifier), `tag$` (otherwise invisible), `instance` (different from
its own) and `v$`.

---

## 5. SWIM: failure detection

The timings are set by sneeze, not swim-ng (whose own defaults would be
20/4/12/3/10 ms): [source]

| | ms |
| --- | --- |
| `interval` – one Ping per interval | 333 |
| `pingTimeout` – then PingReq via others | 444 |
| `pingReqTimeout` – then suspicion | 555 |
| `pingReqGroupSize` | 7 |
| `suspectTimeout` – then faulty | 999 |
| `joinTimeout` | 999 |
| `joinCheckInterval` (swim-ng default) | 50 |
| `disseminationFactor` | 22 |

The sequence: [source, timings measured]

1. Every 333 ms (the first immediately, `setImmediate`) `Membership.next()`
   picks a member: at random from those that have not had their turn in
   this round yet; if the round is empty, a new one starts with all alive
   and suspect members.
2. Ping with a new `seq`. If an Ack with this `seq` arrives within 444 ms,
   all is well.
3. Otherwise PingReq `{seq, destination}` to up to 7 random members –
   **the member being checked can be among them**, `random()` does not
   exclude it; it is then asked to ping itself. Each PingReq has its own
   `seq`.
4. The receiver of a PingReq pings `destination` with its own `seq` and
   forwards an Ack received within 444 ms as an Ack with the original `seq`
   to the sender of the PingReq.
5. If no Ack arrives by any path within 555 ms, the member is suspected
   (`onSuspect`) – with the state it had at the time of the Ping. If there
   are no members for a PingReq, it stays that way: no suspicion.
6. 999 ms later it becomes faulty, with the incarnation of the suspicion.

A hard failure is noticed in the harness after **2.1 to 2.7 s** [measured,
`interop.js` steps 5 and 7], three nodes.

**Only the one who checked sets the clock.** A node that hears "X suspect"
records the state and waits for the faulty from the checker; it does not
start a timer of its own. [source]

An Ack from a member we consider suspect is answered by `Membership.onAck`
with an Update to that member containing its suspect state – that is how
it learns about it and can refute it. [source]

---

## 6. SWIM: states and incarnations

An Update about *another* member `h` with incarnation `i`: [source, rules
individually in `test/unit/swim_test.c`]

| Update | is accepted if | otherwise |
| --- | --- | --- |
| alive | no faulty entry for `h` with incarnation ≥ `i`, **and** `h` unknown or `i` **strictly greater** than the known one | dropped |
| suspect | no faulty entry with ≥ `i`, and `h` unknown, `i` greater, or `i` equal and known as alive | dropped |
| faulty | `h` known (alive/suspect) and `i` ≥ known | dropped – also if `h` was never known |

Consequences you need to know:

- **An alive with the same incarnation does not lift a suspicion.** Whoever
  is suspected has to increase its incarnation.
- **faulty entries are never forgotten** (`hostToFaulty`); they go out with
  every Sync. A restart on the same `ip:port` only comes back with a
  greater incarnation – which `Date.now()` at startup provides.
- An Update that is accepted deletes a faulty entry and (for alive) the own
  suspicion timer. A faulty does not delete the suspicion timer; it runs
  into nothing.
- A new member triggers `Change` and `Update`, a faulty likewise; everything
  else that is accepted triggers only `Update`. Anything dropped triggers
  `Drop`.

About **itself** (`host` equal to its own):

| Update | Effect |
| --- | --- |
| alive with `i` > own | own := `i` + 1, disseminate Update |
| alive otherwise | nothing |
| suspect or faulty with `i` > own | own := `i` + 1 |
| suspect or faulty with `i` = own | own := `i` + 1 |
| suspect or faulty with `i` < own | incarnation stays |
| suspect or faulty, in every case | disseminate its own state as an Update |

`preferCurrentMeta` is on (microwizard sets it): the own meta is never
replaced by that of a foreign Update.

---

## 7. SWIM: dissemination

Every accepted Update (including its own) goes into the disseminator; a
newer one about the same host replaces an older one. It piggybacks on
Ping, Ack and PingReq until it has been told `limit` times: [source,
measured for two members: 11 times]

```
limit = ceil(22 · log10(n + 1)),   n = members alive/suspect + 1 (oneself)
```

On every `Change`, `limit` is recomputed and whatever has already reached
it drops out. Selection is by tell count ascending, within the same count
in the order in which the updates entered that level, and only what still
fits (`length + 2 ≤ room`). Sync and the reply to it carry no piggybacked
updates.

---

## 8. SWIM: the join

[source, measured]

1. The new node sends a compound to every base (except itself): `Sync`
   with its own data, followed by an `Update` for every known member
   (alive, suspect, faulty – nobody on the first join).
2. The base accepts the sender as alive and replies with Updates about
   **everyone**: alive and suspect, itself, the faulty entries.
3. Every 50 ms the node checks whether one of the bases is now a member
   (the key must be exactly the string from the base list). Yes: done.
4. After 999 ms without a reply: `JoinFailedError`. sneeze retries up to
   22 times after 111–555 ms, **with a new Swim instance** (and, without a
   fixed port, on a new port) – the old one is not closed, it keeps running
   with its socket. After the last attempt: `error`, and the node stays
   outside.

After that the rest of the mesh learns about the newcomer via the base's
piggyback.

### When bases go away

With `discover.rediscover` (gateway and services have it, the base does
not), `mesh.js` attempts a new join when a member with pin
`base:true,role:mesh` becomes faulty and fewer than two bases are left.
`aliveBases` is never incremented in the process: `addClient` compares
`meta.config.pin[0]` with `'base:true,role:mesh'`, but the base announces
`'role:mesh,base:true'`. So the counter only ever goes down. [source]

A base that restarts faster than it becomes faulty starts with an empty
table; the others keep pinging it, it replies, and nobody sends it a Sync.
Afterwards it only knows whoever joins next. [source]

mwm-node does it more simply and more steadily: if none of the bases is a
member, it sends a Sync to all bases again every 1111 ms; if a join fails,
again after 111–555 ms – without an upper limit and on the same port.

### Restart on the same port

**A node that restarts on the same `ip:port` before it has become faulty**
(under ~2 s) is accepted with its new, higher incarnation – but under the
old identifier it remains in sneeze and in microwizard: sneeze keeps
members by `identifier$`, the SWIM entry is the same key, a faulty for the
old identifier never comes. The balancer keeps the old TCP port as a target
until the caller restarts; calls there run into the connectTimeout
(355 ms), open the breaker and go elsewhere on the next attempt. [measured
with an ad-hoc run: no `remove` for the old identifier]

So a service should either take a fresh SWIM port or stay away longer than
~3 s. `interop.js` waits for the faulty in step 4.

**sneeze-ng 0.13.1** (branch fix/restart-same-host) fixes this in
`add_node`: [source]

1. the same message again (incarnation, `host`, `v$` equal): nothing
2. the identifier is known under a *different* host with a *newer*
   incarnation: a message from an abandoned join attempt, nothing
3. under the same host there is a different identifier with a newer
   incarnation: the message is the stale one, nothing
4. otherwise all other identifiers under the same host go (`remove`), then
   the new one comes (`add`) – also with an equal incarnation

and `remove_node` only removes an identifier if it is under exactly this
host. `src/mesh.c` does it this way (`test/unit/mesh_test.c` rule by rule),
and with it the client: a service restarted on the same ports replaces its
old target after about 0.3 s instead of never. [measured, `meshclient.js`
14] The Node nodes in the harness run 0.13.0 and still keep the old
identifier.

mwm-node itself comes back after a restart with a new `instance`, hence a
new `identifier$`, and with `Date.now()` as incarnation – greater than
anything the previous run had, as long as the clock does not jump back.

---

## 9. sneeze and the mesh in microwizard

sneeze sits on top of swim-ng and reports `add`/`remove` by
**`identifier$`**: [source]

- Members without meta, with a different `tag$` or with its own identifier
  are skipped.
- `state` 0 → `add` if the identifier is new or the incarnation, `host` or
  `v$` has changed. `state` 2 → `remove`. `state` 1 says nothing: a suspect
  stays a target until it is faulty.
- `Change` and `Update` from swim-ng are evaluated.

`mesh.js` turns `add` into one client per pin: [source, measured]

- `pinConfig = config without pin(s), pin = config.pin[0]`, and as identifier
  `pinConfig.id = utilPattern(pinConfig) + '~' + identifier$`.
  `utilPattern` takes all primitive fields without `$` in the name, sorted:
  `connectTimeout:355,host:127.0.0.1,ismesh:true,model:consume,pin:role:echo,cmd:*,port:40709,timeout:5555,type:tcp~<identifier$>`
- `null:true` is skipped; so is a pin the node serves itself
  (`ignoreClient`, comparison of the pin as a string), and members of its
  own `instance`.
- A `type` other than `tcp` (or missing) the client cannot handle; `model:
  'observe'` (from 1.6 on) delivers to all targets instead of one – the
  client in meta does neither and skips such pins with a line on stderr.
- There is one balance client per pin (once), and under it one TCP client
  per provider – which only connects on the first call.
- `remove` takes the provider out of the balancer. In 1.5.0 the pin itself
  stays registered: without a provider a call yields `no-current-target`,
  and does so forever, even if another pin of a live service would match
  (a dead `role:a,cmd:x` wins over a live `role:a,cmd:*`). [source]
- **Since microwizard fix/dead-pin** (and so in the client in meta) a pin
  leaves the pattern tree when its last target goes (`#dropPin`): the call
  falls through to the next best pin; if there is none, it is `undefined`.
  A target that visigoth has merely opened has not gone; an own handler on
  the same pin keeps the node; if a member announces the pin again, it is
  back. The balance client (in meta: the group with its visigoth) remains
  in place. [measured, `deadpin.js`, `client_test`]
- With several listens every sneeze instance reports `add` and `remove`;
  `add` is deduplicated by the target's identifier, the first `remove`
  takes the target out. The client in meta does exactly the same.

---

## 10. Calls: ndjson over TCP

One connection per provider, one line of JSON per message
(`JSON.stringify` + `\n`), in both directions. What is written here comes
from `transfer.js` and `tcp.js`; what the cases result in on the caller's
side is pinned down by `test/harness/reference.js`.

### Request

```
{"id":"<uid>","k":"a","sync":true,"args":{...}}
{"id":"<uid>","k":"aE","sync":true,"p":"<pin>","d":{...}}
```

- `id`: the caller's `uid()`, 11 hex characters.
- `k: "a"` is the normal case. `args` is the whole argument: the pattern as
  an object with the second argument of `act` laid over it –
  `act('role:echo,cmd:ping', {n: 1})` sends
  `{"role":"echo","cmd":"ping","n":1}`. The values from the pattern string
  are strings. [measured]
- `k: "aE"` occurs when `actE` was called locally (then `meta.pin` is set):
  `p` is the pattern string, `d` the data. [source]
- `sync` is always `true` (only a caller that explicitly turns it off sends
  `false`).

### How the service finds the target

`handleRequest` calls `mc.act(args)` or `mc.actE(p, d)` respectively. For
`a` the pattern is built from **all primitive top-level fields** of `args`
(`key:${value}`, without duplicates, sorted; a number as JS writes it);
fields of type `object` (including `null`), `undefined`, `function`,
`symbol` do not count. For `aE` it is the pairs of `p`, exactly as the
regex of `convertData` finds them. The handler gets all of `args` or `d`
respectively. [source]

**Which pin wins** (`#matchPin`, like this since microwizard
fix/pattern-shadow; `src/pattern.c` for both sides of mwm-node): [measured,
`pattern_test`]

- A pin takes a message that has every one of its pairs; a pair with `*`
  takes any value of that key. Parts of the message that the pin does not
  name are ignored.
- Of the pins that take it, the one with the **most pairs** wins, then the
  one with the **most exact pairs** – a `role:a,cmd:ping` beats a
  `role:a,cmd:*` –, then the one the tree leads to first: the keys of the
  message alphabetically, for each first the exact branches, then the
  wildcard. For `a:*,b:1` against `a:1,b:*` and `a:1,b:1`, `a:1,b:*` wins.
- The tree is searched completely: an exact branch that leads nowhere does
  not hide a wildcard next to it. In 1.5.0 the matching ran greedily
  through the tree; a node with `role:mwmobserver,cmd:ping` got `no-target`
  for `role:mwmtest,cmd:ping`, although another service offered
  `role:mwmtest,cmd:*`.
- Pins with the same pairs (even written differently, `cmd:*,role:a`) are
  one node; it is held by whatever was added last, and if that goes (a pin
  without a provider, section 9), by what was added there before it.
- Own handlers and the pins of other services stand in one tree: the
  client adds every pin of a member with `add` like a handler. An own
  `role:a,cmd:ping` beats a foreign `role:a,cmd:*`, a foreign
  `role:x,cmd:*` beats an own `role:x`.
- `act(pattern, args)` matches the pairs of the pattern **and** the fields
  of `args`, both: if a key is in both, they are two parts.

On the service side of mwm-node only its own handlers answer: an incoming
call is not passed on to a foreign pin (Node would do it, because there one
tree serves both).

**If no handler is found** – for instance `role:echo,cmd:nothing` with a pin
`role:echo,cmd:*` without a handler for `nothing` –, microwizard writes
`no-target` to the console and replies with `res: undefined`, that is a
reply without any `res`. The caller gets `undefined` as the result.
[measured]

### Reply

```
{"id":"<id of the request>","k":"res","sync":true,"res":{...}}
{"id":"<id of the request>","k":"res","sync":true,"error":{"message":"…","name":"Error","stack":"…", …}}
```

- `sync` is taken over from the request. A request with `sync: false` gets
  no reply (the handler runs anyway).
- `error` is `Object.assign({}, err)` plus `message`, `name`, `stack` – so
  all enumerable properties of the thrown error (`code`, `details` …).
- A service that is closing (`role:mc,cmd:close`) answers everything with
  `error: {message: "retry_later_err_overload", name: "Error"}`. `close`
  waits until all running calls have replied (`#active`, checked every
  500 ms), then another 500 ms, then it closes; such a reply does not count
  as running.
- Calls over one connection all run concurrently: each line is an
  `await tp.handleRequest`, the reply goes out when it is done. microwizard
  has no limit. A service that wants to signal overload throws an error
  with `details: {message: "retry_later_err_overload"}` – only this form
  counts as overload for balance.js (section 11).
- `k` neither `a` nor `aE`: no reply (`{input, error: 'unknown method'}`
  without `sync` is not written).
- A line that is not JSON: the parser reports an error, and the service
  **closes the connection** (`connection.end()`).

### What the caller does with the reply

[source; measured where noted]

- `res` becomes the result of `actAsync`. [measured]
- `error` becomes a `new Error(message)` with all fields copied onto it.
- A reply with `sync: false` is accepted and otherwise ignored – the call
  keeps waiting until the timeout. [measured] **A reply must carry
  `sync: true`** (or omit it: if it is missing, `true` applies).
- **A reply with a `k` other than `res` or with `id: null`** runs into
  `this._context.options.warn…` – `_context` does not exist (the field is
  called `#context`), which throws a `TypeError` in the `_write` of the
  message stream. The process survives, but **the connection is dead
  afterwards**: this call and every further one over it runs into the
  timeout (5555 ms), until the keepalive discards it after up to two
  minutes. [measured with an ad-hoc server: first and second call each
  `timeout` after 5.56 s] **Since microwizard fix/unknown-reply** every
  unusable reply – not JSON, `null`, not an object, a different `k`,
  `id: null`, an unknown `id` – is dropped, reported once per kind every
  ten seconds, and the connection stays. The client in meta does it this
  way. [measured, `client_test` "junk": nine unusable lines before the
  reply, then one more call over the same connection]
- **A reply with an unknown `id`** runs into
  `this.#context.options.warn.unknown_message_id` – `#context` has no
  `options` (the settings are called `opts` there); the same `TypeError`,
  the same consequence. [measured: the call runs into the timeout] An entry
  is deleted 100 ms after its reply; the LRU holds 10000. **So a meta
  service must never reply unasked and never twice to the same `id` more
  than 100 ms apart.** A second reply within 100 ms and a late one after a
  timeout are harmless: the entry is still there then, the second `done`
  goes nowhere. [measured: twice 300 ms apart – the result arrived, the
  second reply hit a deleted entry; the process lives, the connection is,
  as above, dead afterwards]

### Timings and keepalive

| | |
| --- | --- |
| `connectTimeout` | 355 ms until the connection is up, otherwise `'timeout'` |
| `timeout` | 5555 ms until the reply, otherwise `'timeout'` |
| Reconnect | `reconnect-core`, `failAfter: 3` |
| Client keepalive | every 2 min `{"h":1}`; if **any** line comes back within 5 s, fine, otherwise the connection is discarded and only rebuilt on the next call |
| Service reply | `{"h":2}` immediately on `{"h":1}` |
| Service idle | `connection.end()` after 3 min without traffic |

Both timeouts come from the `config` of the announced meta (section 4).

- A connection that cannot be established (refused) is not an error, but
  after `connectTimeout` a `timeout`: the call waits for the next attempt
  of reconnect-core. [source; measured on the client in meta: a dead target
  costs 355 ms and the retry, `meshclient.js` 12]
- If a connection breaks, nothing that was already sent over it fails:
  those calls run into their `timeout` (5555 ms). Only the next call
  rebuilds it. [source; the same in meta]
- `callmap` belongs to the transport, not the connection: a reply finds its
  call by the `id` alone, whichever connection it comes over.

---

## 11. Distribution: balance.js and visigoth

Per pin one balance client with a visigoth in it, per provider one target.
visigoth 2.0.5 is an in-house development (fork) and not a textbook
breaker. Everything here is rebuilt in `src/visigoth.c` and checked against
the real visigoth: `test/harness/visigoth-trace.js` drives it through 3000
random steps (add, remove, choose, error, clock), `visigoth_test` replays
the same steps and compares result and ring after every step. [measured]

### The ring

`BalanceLinkedRing`: a linked list with a score, in microwizard always
score 1, and **one cursor for all calls** that only moves forward
(`getAndStep`: return the cursor, advance, at the end back to the start).
New targets with equal score go to the end. The ring size is 40
(`ringSize`); a 41st target gets `{code: 1, err: 'ring full'}` and no
element, and `balance.js` stores `target.e = undefined` – if that provider
goes away later, `visigoth.remove(undefined)` throws. [source]

Two quirks of `addScore` that never apply with score 1 and are rebuilt
anyway: a score above that of the head is hung in front of the head, but
never itself becomes the head (unreachable from the head); the search for
the place for a smaller score stops one element too late.

### The choice

```
choose():
  empty → error "no upstreams available"
  start = getAndStep(); u = start
  repeat:
    if u OPEN:
      if now − u.since > closingTimeout:  u := HALF-OPEN, since := now
      else: u = getAndStep(); continue (abort when back at start)
    (rater: round robin always returns 1 – "score ≤ 0 opens" never applies)
    if u not OPEN: chosen
    u = getAndStep()
  until u == start
  none → error "no upstreams available"
  chosen: callback(target, errored, stats); then HALF-OPEN → CLOSED
```

- **Round robin** in insertion order. Every step over an open target
  advances the shared cursor.
- `closingTimeout`: visigoth has 30000 ms, **balance.js sets 1000 ms**.
  The comparison is "greater than": exactly 1000 ms after opening it is
  still open.

### States

| from | to | by |
| --- | --- | --- |
| CLOSED | OPEN | **a single** reported error (`errored()`); there is no counter and no threshold |
| OPEN | HALF-OPEN | a `choose` that reaches the target more than `closingTimeout` after opening |
| HALF-OPEN | CLOSED | **immediately on being chosen** – not on success of the call |
| HALF-OPEN/CLOSED | OPEN | `errored()` of the running call |

The deviations from the textbook, explicitly:

- No error counter, no error rate: one error opens.
- Half-open is not a probing state with exactly one probe call. The target
  is closed as soon as it has been chosen; all following calls go there
  again until one of them reports an error. The comment in the code
  ("Close the circuit once it has been successful") describes something
  other than what the code does – the closing comes after the *synchronous*
  invocation of the callback, which only starts the call.
- No backoff: open is always exactly `closingTimeout` long, no matter how
  often.
- An open target is only checked when the cursor hits it – there is no
  timer.
- `errored()` sets OPEN with a new timestamp even if the target was already
  open (later errors extend it).

### What counts as an error (balance.js)

```
target.action(msg) → success: done(null, result)
                   → error:   if err.details?.message === 'retry_later_err_overload'
                                   or err.message === 'timeout':
                                errored(); if ++attempts < 10: choose again in 100 ms
                                else done({err: 'all-targets-overloaded', msg})
                              else: done({err, msg})   – no errored(), no retry
choose fails:                 done({err: 'no-current-target', msg})
```

- **Timeout** (connectTimeout 355 ms or timeout 5555 ms): counts.
  `done('timeout')` is a string; bluebird turns it into an
  `Error('timeout')`, which is why `err.message === 'timeout'` applies.
- **Overload** only counts if the error carries `details.message ===
  'retry_later_err_overload'`. The overload reply of a closing service
  (`error.message` directly, without `details`) **does not count** – it
  goes to the caller as an ordinary error.
- **Application errors** (handler throws) do not count and are not
  retried.
- `retryTimeout` is 100 ms, up to 10 attempts.
- **Since 1.6.2 every retry gets a new `id`** (`uid()`), so that a late
  reply to the previous attempt does not count as its own. [source;
  measured on the client in meta, `client_test` "failover"]
- **With a single provider there are no ten attempts**: the first error
  opens it for 1000 ms, the retry 100 ms later finds no target, the caller
  gets `{err: 'no-current-target', msg}`. Ten attempts only happen if
  there are enough providers. [measured, `reference.js` 4 and 6]

### What arrives at the caller

| Case | `actAsync` |
| --- | --- |
| reply with `res` | fulfilled with `res` |
| reply with `error`, not overload | **rejected with `{err: Error, msg}`** – an object, not an Error; `err` carries the fields from the service and, from bluebird, `isOperational` and `cause` [measured] |
| overload/timeout, all attempts used up | rejected with `{err: 'all-targets-overloaded', msg}` |
| no target choosable | rejected with `{err: 'no-current-target', msg}` |
| balance client without visigoth (between creation and first target) | **fulfilled** with `{err: 'no-target', msg}` |
| no matching pin at all locally | **fulfilled with `undefined`**, `no-target` on the console |

`src/balance.c` is this consume as a sequence of steps without a clock,
`test/unit/balance_test.c` plays through the cases with the visigoth port
on a fake clock, `meshclient.js` compares the client in meta case by case
with a Node caller.

### The `patient` change (default)

Decided and in microwizard on the branch fix/consume-wait (`balance.js`
consume, `index.js`); the default here. Anyone who needs consume exactly
like 1.6.2 turns it off: `bal_options_t.patient = 0`,
`mwm-node --no-patient` (`--patient` still exists, it is the default).

- if the choice fails after an attempt has failed, it waits **once** for
  `closingTimeout + retryTimeout` (1100 ms) – by then the previously opened
  target is half open – and chooses once more before it becomes
  `no-current-target`
- the whole call takes at most **one request timeout**: in Node
  `balance.timeout`, otherwise the transport's `timeout` (5555 ms), as
  `Micro` passes it to the balancer; here `bal_options_t.budget`, otherwise
  the client's `timeout` (`mwm-node --timeout`, 5555 ms). An attempt only
  gets what is left of it; if nothing is left after the choice, it is
  `all-targets-overloaded`, without an attempt; a wait that would end after
  it ends the call (`no-current-target` for the one wait,
  `all-targets-overloaded` for a retry)

With a single provider that is overloaded once, this yields the reply after
around 1.2 s instead of `no-current-target` after 0.1 s; if it is always
overloaded, `no-current-target` after the one wait. A timeout uses up the
budget, without a retry. Two providers that both do not reply cost, with
`closingTimeout` below the timeout, one timeout instead of ten. [measured,
`balance_test` – including the cases of `test/balance.test.js` from
fix/consume-wait with a short budget –, `client_test` and
`client_test --short` (timeout 400 ms)] Otherwise nothing changes with
several providers. `meshclient.js` compares with microwizard 1.5.0 and
therefore calls with `--no-patient`.

---

## 12. What a Node caller makes of it

The usual convention of the services is a reply `{code, msg}` on success
and `{code: >= 400, err: "Text"}` on a business error – both as `res`, not
as `error`. A caller checks `code` itself (for instance
`actAsync(...).then(defaultError)`, `test/harness/lib/gateway.js`).

- A **rejection** (`{err, msg}` from section 11) is not an `Error`.
  graphql-js 16 turns it into `Unexpected error value: { err: …, msg: … }` –
  with the arguments of the call in the text. A meta service should
  therefore answer business errors as `res: {code, err}`, never as `error`.
- A result of `undefined` (no handler behind the pin) runs, in such a
  `defaultError`, into `Cannot read properties of undefined (reading
  'code')`.

---

## 13. The typed notation

The golden files need a notation that says what the bytes *were* –
`msgpackr.unpack` forgets exactly that (float64 or uint64?).
`test/harness/lib/mpdump.js` reads the bytes itself, `mp_dump` in
`src/msgpack.c` writes the same:

```
nil  true  false                     c0, c3, c2 (and d4 00 00, msgpackr's undefined)
u:5   i:-5                           any int form; u for ≥ 0, i for < 0
f:1793405011551                      float32/64 with an integral value up to 2^53
f:0x3ff8000000000000                 any other float, as bits
"text"                               JSON.stringify escaping
bin:00ff                             bin as hex
[a,b]  {"k":v}                       array, map (keys written like values)
```

The golden files (`test/fixtures/`):

| File | from | what the test requires |
| --- | --- | --- |
| `msgpackr-samples.txt` | `pack-samples.js` | notation from the bytes and from the document equal; written back, the same bytes (except BigInt and `undefined`, which JS itself would not write that way) |
| `swim-bodies.txt` | `capture.js` + `fixtures.js` from real nodes | likewise |
| `swim-frames.txt` | likewise | the same parts from every datagram |
| `visigoth-trace.txt` | `visigoth-trace.js` | the same decision and the same ring after every step; `test/run.sh` regenerates it and compares |
| `pattern-trace.txt` | `pattern-trace.js` (seed 7, 300 random cases) from microwizard fix/pattern-shadow | the same winner for every message; `test/run.sh` regenerates it and compares if it finds a checkout of it (`MWM_MICROWIZARD`), and additionally compares a fresh trace with a random seed |

---

## 14. What mwm-node does of this, and what is open

The service side (`src/service.c`, the connections in `src/main.c` on
`meta_tcp.h` from the branch feat/stream): `reference.js` compares every
case from section 11 between the Node echo and the meta echo, the
gateway's view included – they are equal except for role and stack.

- A reply exactly once, with `k: "res"`, the `id` as it came (even as a
  number), and `sync` from the request; with `sync: false` and a foreign
  `k`, none.
- `{"h":1}` → `{"h":2}`; a line that is not JSON ends the connection; after
  3 min without a line it is closed.
- Patterns as in microwizard fix/pattern-shadow (section 10): all primitive
  top-level fields of `args`, `*` as wildcard; the most pairs, then the
  most exact, then the first path through the tree; for `aE` the pairs
  from `p`, the handler gets `d`. Without a matching handler: a reply
  without `res`.
- A thrown error goes out with its fields, `name: "Error"` and a `stack`
  (`<name>: <message>\n    at mwm-node (…)`) – the caller takes it over as
  it takes over the one from Node.
- SIGTERM/SIGINT: from then on `retry_later_err_overload` (only `message`
  and `name`, like transfer.js); the running calls finish and are answered,
  500 ms after the last one the process ends – like `close`, except that
  here it checks every 50 ms instead of every 500 ms. A second signal ends
  it immediately. [measured, `concurrency.js` 10]
- Per connection one reader and one task per call: any number of calls
  over the same connection run concurrently, the replies go out in the
  order in which they finish, each as one whole line. Across all
  connections together at most `--max-inflight` (10000) calls run; one
  above that is answered immediately by the reader, without a handler,
  with `error: {message: "retry_later_err_overload", name: "Error", details:
  {message: "retry_later_err_overload"}}` – the form a Node service throws
  for overload and that balance.js counts: the target opens, the call goes
  to another one 100 ms later. [measured, `concurrency.js` 2–9]
- Several pins (`--pin` several times): each its own listen as in
  microwizard (section 4). [measured, `meshclient.js` 15–17]

The calling side (`src/client.c`, `mwm_act`/`mwm_actE`, on the command line
`--client`):

- Targets from the members as in `mesh.js` (section 9), grouped by pin, one
  visigoth per group; consume like balance.js from fix/consume-wait, with
  `--no-patient` like 1.6.2 (section 11).
- One connection per target, established on the first call, with
  `connectTimeout` and `timeout` from the announced `config`, keepalive
  like tcp.js.
- One task owns everything – targets, connections, the table of open
  `id`s – and listens on a channel; one reader task per connection, one
  ticker. A call runs in the caller's task; `--client` turns every line of
  stdin into a task.
- Own handlers and groups in the same pattern tree (section 10): if an own
  handler wins, it answers without the mesh (like a local `act` in Node,
  whose throw is not wrapped in `{err, msg}` either). Handlers have to be
  added before `mwm_clientStart`.
- A group without a target leaves the tree, a new target brings it back
  (fix/dead-pin, section 9). [measured, `deadpin.js`]
- `meshclient.js` compares case by case with a Node caller against the
  Node and the meta echo; `client_test` covers with its own servers what is
  hard to produce in the mesh: unusable replies, dead port, silence,
  keepalive without reply, the `patient` option.

Open:

- mwm-node only talks to 127.0.0.0/8 unless started with `--any-address`;
  that restriction is only lifted after a review. There is no DNS
  discovery via `wzdc_baseName`.
- `model: 'observe'` and `act` with `to` (to a specific instance, 1.6)
  the client cannot do.
- The client task writes the requests itself; a target that no longer
  reads holds it up for up to `timeout`.
- A handler runs in one task per call; one that blocks (instead of
  parking) holds a carrier.
- `mwm_act` parks the calling task; many calls at once mean many tasks
  (`--client` makes one per line). A call that immediately returns a
  handle and is awaited later (`mwm_actStart` → `mwm_await`) would need a
  back channel in the handle – but channels cannot go into structs or onto
  the heap; with `@future` from doc/concurrency.md
  it would be `go mwm_act(...)` without a new API.
- Throughput against Node: bench/README.md.
