# microwizard-meta

[MicroWizard](https://github.com/WizardTales/MicroWizard) – die
Reimplementierung von seneca und seneca-mesh – in
[meta](#bauen). Ziel: Dienste in meta, die einem bestehenden
MicroWizard-Mesh beitreten, von Node mit `seneca.actAsync` gerufen werden und
selbst andere Dienste rufen, drahtkompatibel mit microwizard 1.5.0 (die
Aufrufseite wie 1.6.2, consume wie fix/consume-wait, der Musterabgleich wie
fix/pattern-shadow, Pins ohne Anbieter wie fix/dead-pin).

| Pfad | Inhalt |
| --- | --- |
| [docs/zaun.md](docs/zaun.md) | was geprüft sein muss, bevor der Zaun auf 127.0.0.0/8 fallen darf |
| [docs/protokoll.md](docs/protokoll.md) | beide Protokolle, wie sie auf der Leitung liegen: SWIM über UDP, ndjson über TCP, msgpack wie msgpackr es schreibt, visigoth, was das Gateway mit Antworten macht – aus den Quellen gelesen und gegen Node-Knoten geprüft |
| `src/msgpack.c` | msgpack in den Formen von msgpackr 1.11.2: Schreiber, Cursor, Dokument (`json_t`), typisierte Darstellung |
| `src/swim.c` | SWIM wie swim-ng 0.6.2 unter sneeze-ng 0.13.0, ohne Socket und ohne Uhr: Join, Ping/Ack/PingReq, Huckepack, suspect → faulty, Inkarnationen |
| `src/mesh.c` | die Mitgliedertabelle von sneeze nach `identifier$`, mit der Neustart-Regel von sneeze-ng 0.13.1 |
| `src/visigoth.c` | Zielwahl und Circuit Breaker von @wzrdtales/visigoth 2.0.5, Entscheidung für Entscheidung |
| `src/balance.c` | consume aus balance.js als Schritte ohne Uhr: mit der Option `patient` (Standard) wie fix/consume-wait, ohne wie 1.6.2 |
| `src/pattern.c` | der Musterbaum und `#matchPin` von microwizard fix/pattern-shadow, für beide Seiten: welcher Pin eine Nachricht bekommt |
| `src/client.c` | die Aufrufseite: Ziele pro Pin, eigene Handler und fremde Pins in einem Baum, eine ndjson-Verbindung pro Ziel, `mwm_act`/`mwm_actE` |
| `src/service.c` | die Dienstseite der Aufrufe: Zeile rein, Handler nach Muster, Antwortzeile raus |
| `src/echo.c` | der Echo-Dienst der Tests, dieselben Befehle wie `test/harness/echo.js` |
| `src/main.c` | `mwm-node`: ein Listen pro Pin (UDP, swim, Tabelle, TCP), ein `select` über Sockets, Signale und Frist, Aufrufe von stdin |
| `test/unit/` | Unit-Tests in meta, gegen Golden Files von echten Node-Knoten |
| `test/harness/` | die Referenz in Node (microwizard 1.5.0, Versionen wie in `core` festgenagelt): Basis, Echo-Dienst, Client, Mitschnitt, Vergleiche |
| `test/fixtures/` | die Golden Files; `docs/protokoll.md` Abschnitt 13 sagt, woher jede kommt |

## Bauen

meta schreibt C, das C wird übersetzt. Der TCP-Teil braucht
`meta_tcp.h`, `meta_pollUntil`, `meta_signalFd`, die Besitzregeln und
`json_t.truthy()` von metalanguage parser-rewrite ab 6b2b20e:

    META=../../metalanguage/meta ./build.sh      # build/mwm-node
    META=../../metalanguage/meta make check      # nur die Unit-Tests
    META=../../metalanguage/meta make SAN=1 check   # mit ASan und UBSan, in build-san/

Ohne `META` nimmt es `meta` aus dem PATH, sonst `../../metalanguage/meta`, und
sagt, wenn dem `meta_tcp.h` fehlt.

## Testen

Alles mit einem Befehl, alles auf 127.0.0.1:

    META=../../metalanguage/meta test/run.sh

1. die Node-Abhängigkeiten des Harness (`pnpm install --offline`, sonst aus
   der Registry)
2. die visigoth-Spur neu aus dem echten visigoth und gegen die Golden File;
   die Muster-Spur neu aus microwizard fix/pattern-shadow, wenn es einen
   Checkout davon findet (`MWM_MICROWIZARD`, sonst neben dem Repo gesucht)
3. `make` und die Unit-Tests: msgpack (gegen Bytes von msgpackr und
   mitgeschnittene SWIM-Körper), visigoth (3000 Schritte gegen die JS-Spur),
   balance (consume auf falscher Uhr, wie 1.6.2 und mit `patient`, auch die
   Fälle von `test/balance.test.js` aus fix/consume-wait), swim (Regeln auf
   falscher Uhr), mesh (die Mitgliederregeln), pattern (die Fälle von
   `test/pattern.test.js` aus fix/pattern-shadow eins zu eins, dann die
   Muster-Spur: derselbe Gewinner wie microwizard für jede Nachricht),
   service (Zeile für Zeile),
   client (gegen eigene Server, die sich auf Bestellung danebenbenehmen:
   mit `--no-patient` wie 1.6.2, dann `patient`, dann `patient` mit 400 ms
   Timeout); mit dem microwizard-Checkout dazu `pattern_test` gegen eine
   frische Spur mit zufälligem Seed (`MWM_PATTERN_SEED` wiederholt einen)
4. `reference.js`: jeder Aufruffall gegen den Node-Echo-Dienst und den
   meta-Echo-Dienst, beide müssen dasselbe ergeben – auch in der Sicht des
   Gateways (`actAsync().then(defaultError)`, graphql-js)
5. `interop.js`: mwm-node tritt einer Node-Basis bei, Node sieht es mit seinem
   Pin, es sieht die Node-Knoten; hart beendet ist es für Node faulty, neu
   gestartet wieder da; ein hart beendeter Node-Dienst ist für mwm-node faulty
6. `meshclient.js`: mwm-node `--client --no-patient` ruft den Node- und den
   meta-Echo und bekommt in jedem Fall, was ein Node-Aufrufer bekommt; Round Robin
   und Failover über zwei Node-Echos, einer davon hart beendet; ein auf
   denselben Ports neu gestarteter Dienst ersetzt sein altes Ziel sofort;
   zwei Pins in mwm-node sehen für Node aus wie zwei Listens eines
   Node-Dienstes
7. `deadpin.js`: A dient `role:deadpin,cmd:ping`, B `role:deadpin,cmd:*`,
   der Client von mwm-node ruft `cmd:ping` – A antwortet; A hart beendet,
   antwortet B (nicht für immer `no-current-target`); A neu gestartet,
   wieder A (wie `test/dead-pin-mesh.test.js` aus fix/dead-pin)
8. `concurrency.js`: 200 Aufrufe zugleich mit 0–500 ms Verzögerung über eine
   Verbindung – Node-Aufrufer → meta-Echo (und als Referenz → Node-Echo),
   rohes ndjson → meta-Echo, meta-Client → Node- und meta-Echo: jede Antwort
   mit ihrem Token, keine verloren oder doppelt, in der Reihenfolge, in der
   sie fertig werden, alles in etwa der längsten Verzögerung; ein Ping hinter
   acht langsamen Aufrufen kommt sofort; über `--max-inflight` sofort
   Überlast, beim Node- wie beim meta-Aufrufer setzt sich jeder Aufruf, und
   danach geht es wieder; SIGTERM mit laufenden Aufrufen

`test/run.sh unit` macht nur 1–3. `MWM_VERBOSE=1` zeigt in `interop.js`,
`meshclient.js` und `concurrency.js` jede Zeile der Knoten. `MWM_SAN=1 test/run.sh` macht alles
mit ASan und UBSan; was die Sanitizer in den Knoten des Harness finden,
lässt den Lauf scheitern.

**Sicherheit.** Der Harness-Basisknoten liegt auf UDP 38999 (`MWM_BASE_PORT`),
die meta-Knoten des Harness und die Server von `client_test` auf festen
Ports in 389xx – nicht auf 39999 (der Standard von microwizard) und nicht
auf 39000/39001 (gern für ein lokales Dev-Mesh genommen), mit eigenem Tag `mwmtest`; jede `wzdc_*`-Variable der Shell wird
verworfen, Multicast, Guess und DNS-Seed sind aus. Ist 38999 belegt, bricht
`test/run.sh` ab. mwm-node selbst sendet nur nach 127.0.0.0/8, lauscht nur
dort, wählt sich nur dort ein und löst keine Namen auf; `--any-address` hebt
den Zaun auf und bleibt aus, bis [docs/zaun.md](docs/zaun.md) abgehakt ist.

## mwm-node

    mwm-node --bases 127.0.0.1:38999 --tag mwmtest --pin 'role:meta,cmd:*' \
             --echo meta --events

    # zwei Pins, zwei Listens; und Aufrufe von stdin, eine JSON-Zeile je Aufruf
    mwm-node --bases 127.0.0.1:38999 --tag mwmtest --pin 'role:a,cmd:*' \
             --pin 'role:b,cmd:*' --client
    {"tag":1,"act":"role:echo,cmd:ping","args":{"n":1}}

Konfiguration wie bei einem Node-Dienst mit microwizard aus `wzdc_seneca__bases`,
`__host`, `__port`, `__tag`, dazu die Schalter oben; der Kopf von
`src/main.c` beschreibt alle. `--events` schreibt eine JSON-Zeile pro
Mesh-Ereignis (`started`, `joined`, `add`, `remove`, je mit `listen`) und
pro Ziel des Clients (`target-add`, `target-remove`). SIGTERM geht wie
`role:mc,cmd:close`: ab da `retry_later_err_overload`, die laufenden Aufrufe
werden fertig, 500 ms nach dem letzten ist Ende.

Jeder Aufruf läuft in einem eigenen Task, wie jeder in Node ein eigenes
Promise ist; über alle Verbindungen höchstens `--max-inflight` (10000)
zugleich, darüber antwortet der Leser sofort `retry_later_err_overload` mit
`details` – die Überlast, die balance.js zählt und anderswo wiederholt.

Der Client ruft wie consume aus microwizard fix/consume-wait: nach einem
gescheiterten Versuch ohne wählbares Ziel einmal warten, bis das Ziel halb
offen ist, und der ganze Aufruf in einem Anfrage-Timeout (`--timeout`, 5555
ms). `--no-patient` ruft genau wie 1.6.2 (docs/protokoll.md, Abschnitt 11).

## Last

Node microwizard gegen mwm-node, mit einem Lastgenerator in C auf eigenen
Kernen: `bench/README.md`. Bei 1000 Aufrufen zugleich über eine Verbindung
1,91 Mio./s gegen 187.000/s (Node 22, microwizard 1.5.0), beide auf einem
Kern.

Die Messung hier unten ist älter, von vor den Carrier-Warteschlangen in
metalanguage (perf/scheduler); die 64 Carrier und `meta_schedLock` gibt es
so nicht mehr.

    node test/bench/bench.mjs build/mwm-node     # die Optionen im Kopf der Datei

Geschlossene Schleife (ein Aufruf geht, sobald einer zurück ist), mwm-node
auf 12 Kernen (0–11: 6 P, 6 E eines Core Ultra 7 265H), die Last auf 4
anderen; CPU aus `perf stat` (`/proc` zählt auf diesem Kernel zu wenig).
Gemessen 2026-10-31 neben anderer Arbeit auf der Maschine, also ±10 %:

| Dienst, 2048 in Flug | META_CARRIERS=1 | Standard (64 Carrier) |
| --- | --- | --- |
| `burn` (~1 ms CPU) | 1 631/s, 1 Kern | 12 622/s, 12 Kerne (7,7×) |
| `delay` (10 ms schlafen) | 70 968/s, p50 29 ms, 1 Kern voll | 156 689/s, p50 12,5 ms, 8,1 Kerne |
| `ping` (nichts) | 178 167/s, 1 Kern | 170 711/s, 7,5 Kerne |

Rechnen skaliert über die Kerne. Kurze Aufrufe nicht: mit 64 Carriern
kostet ein Ping 35 µs CPU statt 6 µs, bei 6,7 Kontextwechseln pro Aufruf
statt 0,12 – jeder `go` und jedes Wecken nimmt `meta_schedLock` und weckt
mit `pthread_cond_signal` einen schlafenden Carrier, der eine Zeile lang
arbeitet und wieder schläft (metalanguage doc/runtime.md, „One run queue for
every carrier“). Der Client (`--client`, ein Task pro Aufruf an den
meta-Echo) schafft 60 000–70 000 Aufrufe/s, begrenzt durch seinen einen
Client-Task.

## Golden Files neu erzeugen

    cd test/harness
    node capture.js /tmp/cap.jsonl 3 5         # Basis, Echo, Client; Echo wird hart beendet
    node fixtures.js /tmp/cap.jsonl ../fixtures
    node pack-samples.js > ../fixtures/msgpackr-samples.txt
    node visigoth-trace.js 7 3000 > ../fixtures/visigoth-trace.txt

## Was meta dabei gefehlt hat

Behoben in metalanguage parser-rewrite (bis 6b2b20e), und hier ohne Umweg:

- Ein `json_t` in einem zurückgegebenen Struct gilt nicht mehr als geleakt:
  Handler geben `(mwm_answer_t){thrown, doc}` zurück, `mwm_act`/`mwm_actE`
  `(mwm_settled_t){rejected, doc}`.
- `select` mit `readable(fd)` und `until(t)`, und darunter
  `meta_pollUntil(fds, n, t)`: `swim` wartet in einem Aufruf auf SIGTERM,
  SIGINT, die Sockets der Listens – eines pro Pin, ihre Zahl steht erst zur
  Laufzeit fest – und die nächste Frist. Kein epoll mehr.
- Signale: `meta_signalFd(SIGTERM)` / `meta_signalTake`, keine Signalmaske in
  `main`, kein signalfd, kein timerfd.
- `push`/`set` gelten als Speichern: die Peers von `mesh.c` liegen in einem
  wachsenden Array, Gruppen und Ziele des Clients ebenso, die wartenden
  Aufrufe in einem Keyed Container nach `id`.
- Der Eintrag eines `go` geht mit dem Task: `--client` macht einen Task pro
  Aufruf statt acht fester Aufrufer, und ein Leser pro Verbindungsaufbau
  bleibt nicht mehr liegen.
- Eine Methode auf einem Element eines Zeigerarrays (`listeners[i].stop()`)
  geht an den Zeiger, nicht mehr an die Adresse des Elements.
- `json_t.truthy()` ist die JS-Wahrheit, mit der `!data.error` und
  `data.sync` gelesen werden (`truth()` ist nur das JSON-`true`).
- `meta_tcpDial` deckt das Auflösen eines Namens mit dem Timeout ab; der
  Client nimmt trotzdem nur Adress-Literale an, weil der Zaun keine Namen
  auflöst (`dialable`).

Noch offen:

- Kanäle gehen nicht in Structs oder auf den Heap. Der Client hat darum
  einen Kanal auf Dateiebene, auf den alle schreiben, und einen Task, dem
  alles gehört; ein Aufrufer und ein Leser bekommen ihren Rückkanal als
  Zeiger auf einen eigenen lokalen Kanal mitgeschickt. Das ist ohnehin die
  Form, die doc/tcp.md empfiehlt – nur ist sie hier die einzige.
- `receive` mit Frist gibt es nicht, und Kanäle und Deskriptoren gehen
  nicht in ein `select`: ein Aufrufer wartet ohne Frist auf seine eine
  Antwort, und der Client-Task garantiert sie – mit einem Ticker alle
  100 ms, der die Fristen prüft.
- Ein wachsendes Array hat kein `remove`: `src/client.c` (`unlist`) und
  `src/mesh.c` (`dropAt`) schieben mit `memmove` über `items` und `count`.
- Ein Header, den auch der C-Compiler liest, kann kein wachsendes Array
  enthalten; `mesh_t` hält die Peers darum hinter einem Zeiger auf ein
  Struct aus `mesh.c`.
