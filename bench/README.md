# Benchmarks

## Node microwizard gegen mwm-node

    make -C bench
    bench/loadgen.sh [build dir] [Runden 3] [Sekunden 2]

Derselbe `cmd:add` auf derselben ndjson-Leitung (`k:"aE"`), einmal vom
Node-Dienst aus microservice-benchmarks (`node-service.mjs`), einmal von
`mwm-node`. Die Last macht `loadgen.c`: C, Verbindungen über bis zu sechs
Threads mit je einem epoll, eine feste Zahl Aufrufe pro Verbindung in Flug
(geschlossene Schleife), jede Antwort an Id und Ergebnis geprüft. Er meldet
seine eigene CPU mit, damit man sieht, wann er selbst die Grenze ist.

Gemessen 2026-10-31 auf einem Core Ultra 7 265H (P-Kerne 0–5, E-Kerne 6–13),
Linux 7.2, 127.0.0.1:

- Server auf P-Kernen: Node und meta mit 1 Carrier auf Kern 2, 2 Carrier auf
  2–3, 4 auf 2–5, 8 auf 0–7 (es gibt nur sechs P-Kerne).
- Lastgenerator auf den E-Kernen 8–13, nie auf denen des Servers.
- Median aus 3 Runden à 2 s. Kerne nicht isoliert, auf der Maschine lief
  anderes (Load 1–12), also etwa ±10 %.
- mwm-node auf Stand 61cf39e (metalanguage 6ef6338). Node 18.14.2 mit
  microwizard 1.1.1 wie im Benchmark-Repo; Node 22.22.2 mit microwizard
  1.5.0 misst gleich (siehe unten).
- Kein Aufruf fehlgeschlagen.

Node läuft auf einem Kern, der faire Vergleich ist also die Spalte
„meta 1 Carrier“.

### 1000 Aufrufe zugleich

Die Zahl aus dem README von microservice-benchmarks (`c=1000`).

| Server | 1 Verbindung × 1000 Calls | 1000 Verbindungen × 1 Call |
|---|---|---|
| Node 18, microwizard 1.1.1 | 169.000/s | 102.000/s |
| Node 22, microwizard 1.5.0 | 187.000/s | 104.000/s |
| meta, 1 Carrier | 1,91 Mio./s | 201.000/s |
| meta, 4 Carrier | 1,82 Mio./s | 491.000/s |
| meta, 8 Carrier | 1,71 Mio./s | 506.000/s |

p99:

| Server | 1 Verbindung × 1000 Calls | 1000 Verbindungen × 1 Call |
|---|---|---|
| Node 18 | 13,9 ms | 13,4 ms |
| Node 22 | 6,3 ms | 11,5 ms |
| meta, 1 Carrier | 0,93 ms | 98,6 ms |
| meta, 4 Carrier | 1,08 ms | 98,6 ms |
| meta, 8 Carrier | 1,15 ms | 4,5 ms |

- Eine Verbindung bedient immer ein Carrier; mehr Carrier helfen dort nicht.
- Bei 1000 Verbindungen und 4 oder 8 Carriern ist der Lastgenerator voll
  (93–100 % seiner Kerne): Untergrenzen.
- Die 98,6 ms sind ein Fairness-Problem der Runtime, kein Durchsatzproblem.
  Ein Carrier unter Last schaut alle 10 µs nach bereiten Sockets und stellt
  deren Reader vor seine Schlange (`META_POLL_US` in metalanguage
  `runtime/meta_tasks.c`). Die meisten Aufrufe kommen sofort dran (p50
  27 µs), wenige werden immer wieder überholt. Im Mittel wartet ein Aufruf
  bei meta 5 ms (1000 / 201.000/s), bei Node 9,8 ms; Node bedient jede
  Verbindung einmal pro Runde der Event-Loop und verteilt das gleichmäßig.
  Bei 8 Carriern ist der Server nicht voll, die Schlange läuft leer, das
  p99 fällt auf 4,5 ms.

### Verbindungen × Calls in Flug

| Verbindungen × Calls | Node 18, mw 1.1.1 | meta 1 Carrier | meta 2 Carrier | meta 4 Carrier | meta 8 Carrier |
|---|---|---|---|---|---|
| 1×1 | 35.000/s | 54.000/s | 50.000/s | 50.000/s | 50.000/s |
| 1×16 | 158.000/s | 607.000/s | 574.000/s | 560.000/s | 564.000/s |
| 1×64 | 176.000/s | 1,29 Mio./s | 1,20 Mio./s | 1,17 Mio./s | 1,18 Mio./s |
| 16×1 | 125.000/s | 193.000/s | 353.000/s | 514.000/s | 485.000/s |
| 16×16 | 173.000/s | 1,73 Mio./s | 2,74 Mio./s | 3,91 Mio./s | 4,50 Mio./s |
| 16×64 | 187.000/s | 2,89 Mio./s | 4,61 Mio./s | 6,31 Mio./s | 7,55 Mio./s |
| 64×1 | 126.000/s | 197.000/s | 325.000/s | 506.000/s | 528.000/s |
| 64×16 | 177.000/s | 1,74 Mio./s | 2,77 Mio./s | 3,73 Mio./s | 5,22 Mio./s |
| 64×64 | 186.000/s | 2,82 Mio./s | 4,41 Mio./s | 5,95 Mio./s | 7,89 Mio./s |
| 256×1 | 123.000/s | 200.000/s | 343.000/s | 514.000/s | 527.000/s |
| 256×16 | 176.000/s | 1,72 Mio./s | 2,74 Mio./s | 4,00 Mio./s | 5,05 Mio./s |
| 256×64 | 181.000/s | 2,64 Mio./s | 4,36 Mio./s | 6,10 Mio./s | 7,88 Mio./s |

- Server-CPU pro Aufruf (belegte Zeit seiner Kerne, softirq mitgezählt):
  Node 5,4–5,9 µs, meta mit 1 Carrier 0,35–0,58 µs bei 16 und mehr in Flug.
- Bei 1 Call pro Verbindung und 4/8 Carriern sowie bei 8 Carriern ab 16 in
  Flug ist der Lastgenerator nahe an seiner Grenze (65–100 %): Untergrenzen.

Rohdaten, eine JSON-Zeile pro Lauf: `results/2026-10-31/matrix.jsonl` und
`results/2026-10-31/1000.jsonl`. `loadgen-table.py` macht daraus Tabellen.

## Die Benchmarks aus microservice-benchmarks

    bench/run.sh [build dir] [Runden 3] [Sekunden 5]

Dieselben Messungen wie WizardTales/microservice-benchmarks (lokal im
Prozess, Node-Client gegen Dienst), mit meta auf der Dienst- und
Client-Seite, alles in einem eigenen Netz-Namespace. Hier misst der
Node-Client mit: er ist bei hoher Nebenläufigkeit die Grenze, für den
Vergleich der Server ist `loadgen.sh` da.
