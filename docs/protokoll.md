# Das Protokoll von MicroWizard, wie es auf der Leitung liegt

Was ein Dienst in meta können muss, um in einem MicroWizard-Mesh
mitzuspielen und von Node mit `seneca.actAsync` gerufen zu werden – gelesen aus den
Quellen, nicht aus der Erinnerung, und wo es ging gegen laufende Node-Knoten
geprüft. Jede Aussage hier trägt eine von drei Marken:

- **[gemessen]** – vom Harness in `test/harness/` beobachtet, und ein Test
  hält es fest (`reference.js`, `interop.js`, die Golden Files)
- **[Quelle]** – aus dem Code gelesen, nicht eigens beobachtet
- **[offen]** – aus dem Code gelesen, noch zu prüfen

Die Stände, im Harness festgenagelt (`test/harness/pnpm-workspace.yaml`):

| Paket | Version | wofür |
| --- | --- | --- |
| microwizard | 1.5.0 | `index.js` (Muster, act), `mesh.js` (Mesh), `transfer.js`, `tcp.js` (Aufrufe), `balance.js` (Verteilung) |
| sneeze-ng | 0.13.0 | `sneeze.js`: Mitglieder nach Kennung, Join-Wiederholung |
| @wzrdtales/swim-ng | 0.6.2 | `lib/`: SWIM selbst |
| msgpackr | 1.11.2 | der Codec von swim-ng |
| @wzrdtales/visigoth | 2.0.5 | Zielwahl und Circuit Breaker |
| ndjson | 2.0.0 | die Rahmung der Aufrufe |

Die Aufrufseite von mwm-node (`src/client.c`) folgt dort, wo sich die Stände
unterscheiden, dem neueren: `balance.js` aus microwizard 1.6.2 (eine
Wiederholung bekommt eine eigene `id`) mit dem consume aus fix/consume-wait
(`patient`, Standard; `--no-patient` ist 1.6.2), der Antwortbehandlung aus
microwizard fix/unknown-reply (unbrauchbare Antworten kosten die Verbindung
nicht mehr) und der Mitgliederregel aus sneeze-ng fix/restart-same-host
(0.13.1). Muster werden auf beiden Seiten abgeglichen wie in microwizard
fix/pattern-shadow, und ein Pin ohne Anbieter fällt heraus wie in
fix/dead-pin. Abschnitte 8, 9, 10 und 11 sagen jeweils, was sich geändert
hat.

Zwei Protokolle: **SWIM über UDP** sagt, wer da ist und welches Muster er
bedient; **ndjson über TCP** trägt die Aufrufe. Dazwischen sitzt eine
Verteilschicht (balance.js mit visigoth), die entscheidet, welcher von
mehreren Anbietern eines Musters einen Aufruf bekommt.

---

## 1. Konfiguration

Ein Dienst liest `rc('wzdc', defaults)`. Aus der Umgebung wird
`wzdc_a__b=x` zu `{a: {b: 'x'}}` – immer ein String. [Quelle]

| Schlüssel | Bedeutung |
| --- | --- |
| `wzdc_seneca__bases` | `"ip:port,ip:port"`. In den Diensten üblicherweise mit `if (bases && bases.indexOf(','))` aufgeteilt – `indexOf` ist `-1` und damit wahr, wenn kein Komma darin steht, und `0` nur, wenn der String mit einem Komma beginnt. Aufgeteilt wird also praktisch immer. |
| `wzdc_seneca__host` | die Adresse, die angekündigt wird (TCP-Host und SWIM-Host) |
| `wzdc_seneca__port` | der SWIM-Port; ohne ihn: Basis 39999, sonst zufällig in [40000, 50000) |
| `wzdc_seneca__tag` | das Mesh-Tag; Mitglieder tragen `tag$: "mc~<tag>"`, ohne Tag `"mc~mesh"` |
| `wzdc_baseName` | der Name, den ein `dnsSeed` auflöst (etwa ein headless Service in Kubernetes); in den Beispielen von microwizard `localhost` |

`seneca.monitor: true` schaltet in sneeze den Terminal-Monitor ein
(Keypress auf stdin, Rohmodus); `wzdc_seneca__monitor=false` schaltet ihn
ab. [Quelle]

### Woher die Basen kommen

`mesh.js` (`findBases`) fragt der Reihe nach `defined`, `custom`, `multicast`,
`guess` und hört nach der ersten Quelle auf, die etwas liefert
(`discover.stop: true`). [Quelle]

- **defined**: `options.sneeze.bases || options.bases || options.remotes`.
  Sind `wzdc_seneca__bases` gesetzt, wird nichts weiter gefragt – kein DNS.
- **custom**: etwa ein `dnsSeed`, ein `dns.lookup(baseName, {all: true,
  family: 4})`. Liefert nackte Adressen; `resolve_bases` hängt `:39999` an.
  Ein Fehler beim Lookup wird *geworfen*, aus dem Callback heraus – der
  Prozess stirbt.
- **multicast**: `node-discover` mit Broadcast. Meist nicht abgeschaltet,
  kommt aber wegen `stop` nicht an die Reihe, solange `custom` etwas liefert.
- **guess**: `127.0.0.1:39999` (und `host:39999`), nur wenn bis hier nichts kam.

`resolve_bases`: `"h:p"` bleibt, `":p"` wird `127.0.0.1:p` (und zusätzlich
`host:p`), `"h"` wird `h:39999`. [Quelle]

Eine Basis nimmt sich selbst aus der Liste (Vergleich mit `host:port` als
String) und steht allein da, wenn das die Liste leert – swim-ng meldet dann
einen `JoinFailedError`, den sneeze bei einer Basis schluckt. [Quelle]

---

## 2. SWIM: der Rahmen

### Datagramm

Ein Datagramm ist eine Nachricht oder ein Compound. [gemessen]

```
Einzelnachricht:   [typ: u8] [msgpack-Körper]
Compound:          [0x00] [anzahl: u16le] ( [länge: u16le] [typ: u8] [msgpack-Körper] )*
```

| Typ | Name | Körper |
| --- | --- | --- |
| 0 | Compound | – |
| 1 | Compressed | definiert, nie gesendet, beim Empfang ignoriert |
| 2 | Encrypted | ebenso |
| 10 | Ping | `{seq}` |
| 11 | PingReq | `{seq, destination}` |
| 12 | Sync | Mitgliedsdaten |
| 13 | Ack | `{seq}` |
| 14 | Update | Mitgliedsdaten |

Die Längen im Compound sind **little endian**, alles im msgpack-Körper big
endian. Eine Länge, die nicht mehr in den Puffer passt, beendet das Lesen des
Compounds still; ein Teil, der kürzer ist als angegeben, wird so weit gelesen,
wie er reicht. Ein Compound darf wieder Compounds enthalten (gesehen nie).
[Quelle]

`maxDgramSize` ist 2048 (sneeze setzt es; swim-ng selbst hätte 512). Die
Mitgliedsdaten eines microwizard-Knotens sind rund 350 Byte groß
[gemessen], es passen also etwa fünf Updates in ein Datagramm.

### Wie gepackt wird

**Eine Nachricht mit Huckepack** (`Net.sendMessage`, für Ping, Ack, PingReq
und das Update an einen Verdächtigen): Platz für Updates ist
`2048 − 1 − 2·2 − länge(nachricht)`. Passen Updates hinein, geht ein Compound
`[nachricht, update, update, …]` hinaus, sonst die Nachricht allein. Eine
Nachricht ohne Körper (kommt nicht vor) wäre nur das Typbyte. [gemessen]

**Viele Nachrichten** (`Net.sendMessages`, für Sync und die Antwort darauf):
Compounds ohne Huckepack, je bis 2048 Byte. Startwert `2048 − 1 − 2`; nach dem
ersten vollen Compound rechnet der Code mit `2048 − 2` weiter – er vergisst
das Typbyte, ein Compound kann damit ein Byte über 2048 kommen. Eine einzelne
Nachricht, die allein nicht passt, wird verworfen (nur ein Debug-Log). Auch
eine einzelne Sync-Nachricht geht als Compound mit einem Teil hinaus.
[gemessen, `compound1-Sync` in `test/fixtures/swim-frames.txt`]

### Wohin geantwortet wird

Mitglieder sind nach dem String `host` aus ihren Daten verschlüsselt
(`"ip:port"`), und fast alles geht dorthin – nicht an die Absenderadresse des
Datagramms. Die Ausnahmen gehen an die Absenderadresse (`rinfo`): [Quelle]

| Auslöser | Antwort | an |
| --- | --- | --- |
| Ping | Ack | Absenderadresse |
| PingReq | Ping | `destination` aus der PingReq |
| Ack auf diesen Ping | Ack mit der ursprünglichen `seq` | Absenderadresse der PingReq |
| Ack von einem Verdächtigen | Update mit seinem (verdächtigen) Stand | Absenderadresse des Acks, nachgeschlagen als Mitgliedsschlüssel |
| Sync | alle Mitglieder als Updates | `host` aus dem Sync |
| eigener Ping, PingReq | | `host` des Mitglieds |

`Net.sendBuffer` teilt den Schlüssel mit `split(':')` – IPv6 geht damit nicht.

### Bindung

UDP wird mit `socket.bind(port)` gebunden, also auf **allen Schnittstellen**,
egal was `host` sagt; `host` ist nur, was angekündigt wird. Der TCP-Listener
dagegen bindet an `host`, und zwar auf einen **vom Kernel gewählten Port**:
`mesh.js` gibt als Port eine Funktion (`50000 + random`), die nie aufgerufen
wird – `net.Server.listen(fn, host)` nimmt sie als Callback, der Port wird 0.
[gemessen: UDP `0.0.0.0:38999`, TCP `127.0.0.1:<ephemer>`]

mwm-node bindet UDP an `host`; im Mesh macht das keinen Unterschied, weil
ohnehin an den angekündigten Schlüssel gesendet wird.

---

## 3. msgpack, wie msgpackr es schreibt

swim-ng bindet `msgpackr.pack` und `msgpackr.unpack` – die Standardinstanzen
`new Packr({useRecords: false})` und `new Unpackr({useRecords: false})`.
Alles hier ist gemessen (`test/harness/pack-samples.js`,
`test/fixtures/msgpackr-samples.txt`).

### Schreiben

| JS-Wert | Form | Beispiel |
| --- | --- | --- |
| Objekt | **immer map16** (`0xde` + u16), auch leer oder mit einem Schlüssel – nie fixmap; ab 65536 Schlüsseln wirft msgpackr | `{seq:0}` → `de0001 a3736571 00` |
| Array | fixarray < 16, array16, array32 | |
| String | nach UTF-8-Bytelänge: fixstr < 32, str8 < 256, str16, str32 | |
| ganze Zahl in uint32 | fixint < 0x80 (wegen `useRecords: false` bis 127, nicht nur bis 63), `cc`, `cd`, `ce` | `128` → `cc80` |
| ganze Zahl in int32, negativ | negativer fixint ab −32, `d0`, `d1`, `d2` | `−33` → `d0df` |
| **jede andere Zahl** | **float64** (`cb`) – auch ganze Zahlen über 2³²−1 oder unter −2³¹ | `Date.now()` → `cb 427a18f44c65f000` |
| `-0` | fixint `00` (`-0 >>> 0 === -0`) | |
| BigInt | `d3` (int64), wenn es passt, sonst `cf`; auch `5n` wird `d3 0000000000000005` | |
| `true` / `false` / `null` | `c3` / `c2` / `c0` | |
| `undefined` | **`d4 00 00`** (fixext 1, Typ 0) | |
| Buffer | bin8/16/32 | |

### Lesen

- `cf` und `d3` werden zu **BigInt**, nicht zu Number.
- `d4 00 00` wird `undefined`.
- Schlüssel einer Map werden Eigenschaften eines Objekts, also Strings.

### Die Folge für die Inkarnation

`incarnation` ist `Date.now()` beim Start – eine ganze Zahl um 1,79·10¹²,
also weder int32 noch uint32 – und geht deshalb **als float64** hinaus
[gemessen: `"incarnation":f:1793405011551` in jeder Mitgliedsnachricht].
Schickte ein meta-Knoten sie als uint64 (`cf`), käme sie in Node als BigInt
an, und swim-ng rechnet mit ihr:

- `data.incarnation > this.incarnation` geht zwischen BigInt und Number noch
- `this.incarnation = data.incarnation + 1` wirft einen `TypeError` (BigInt
  plus Number) – in `Member.incarnate`, also genau dann, wenn ein Knoten
  einen Verdacht über sich widerlegen will
- `prev.incarnation === member.incarnation` in sneeze ist zwischen `5n` und
  `5` falsch: jedes Update wäre ein neues `add`

Deshalb schreibt `src/msgpack.c` jede Zahl nach der Regel von msgpackr
(`mp_buf_t.number`/`.integer`): int32/uint32 in der kleinsten int-Form,
alles andere als float64. Gelesen wird jede numerische Form; eine Inkarnation
als float64 wird zu int64, solange sie ganz ist (exakt bis 2⁵³).

---

## 4. SWIM: die Nachrichten

Alle Körper sind map16 mit genau diesen Schlüsseln in dieser Reihenfolge.
[gemessen, `test/fixtures/swim-bodies.txt`]

```
Ping     {"seq": u}                              de0001 a3 736571 <seq>
Ack      {"seq": u}                              die seq des Pings, unverändert
PingReq  {"seq": u, "destination": "ip:port"}
Sync     {"meta": …, "host": "ip:port", "state": u, "incarnation": f}
Update   {"meta": …, "host": "ip:port", "state": u, "incarnation": f}
```

- `seq` ist ein Zähler pro Swim-Instanz ab 0, für Pings, PingReqs und
  weitergeleitete Pings gemeinsam. Die Zuordnung eines Acks geht allein über
  `seq`; woher er kommt, wird nicht geprüft.
- `state`: 0 alive, 1 suspect, 2 faulty. Ein anderer Wert wird still
  ignoriert.
- `meta` ist bei einem Mitglied ohne Meta `undefined`, also `d4 00 00`.
- Die Reihenfolge `meta, host, state, incarnation` kommt aus
  `Member.prototype.data()`; msgpackr schreibt Schlüssel in
  Einfügereihenfolge.

### Die Meta eines microwizard-Knotens

Aus `mesh.js` (`join`) und sneeze, für einen Dienst mit
`listen: [{pin: 'role:echo,cmd:*', model: 'consume', type: 'tcp'}]`:
[gemessen]

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

- `config` ist die Antwort von `listen`: die Transporteinstellungen
  (`warn`, `host`, `port`, `timeout`, `connectTimeout`) mit den Listen-Optionen
  darüber, Schlüssel mit `$` am Ende entfernt. Die Reihenfolge ist die der
  Spread-Operation – zuerst gesehen, zuerst geschrieben.
- **`timeout` und `connectTimeout` sind nicht Zierde**: der Client übernimmt
  `config` in seine TCP-Optionen (`{...settings, ...pinConfig}`). Was ein
  Dienst hier ankündigt, ist, wie lange ein Node-Aufrufer auf ihn wartet.
- `host`/`port` in `config` sind die TCP-Adresse, `host` außen die SWIM-Adresse.
- `type` fehlt bei Knoten ohne `listen` (Basis, Gateway), der Client nimmt
  dann TCP an.
- `pin` ist ein **Array mit einem String**, der Pin so, wie er geschrieben
  wurde (`role:mesh,base:true`, nicht sortiert). Ein Knoten ohne Pin
  (Gateway, Client) kündigt `["null:true"]` an. Mehrere Pins in einem
  `listen` würden ein Array im Array; in der Praxis kommt das nicht vor.
- `instance` ist `uid()` des Prozesses: 11 Hex-Zeichen.
- `identifier$` = `instance ~ pin ~ Date.now()` (das Array als JS-String).
  `incarnation` ist ein eigener `Date.now()`, eine Millisekunde später.
- **Jedes `listen` ist ein eigenes SWIM-Mitglied** mit eigenem Port und
  eigener sneeze-Instanz; ein Dienst mit zwei Listens erscheint zweimal –
  mit derselben `instance`, je eigenem Pin, TCP-Port und `identifier$`, und
  denselben Schlüsseln in `config`. [gemessen, `meshclient.js` 15] Jede
  sneeze-Instanz sieht die anderen Listens desselben Prozesses als
  Mitglieder; `mesh.js` übergeht sie (`clientInstance.id === meta.instance`).
- `sneezeOpts.port` ist für alle Listens derselbe: mit festem Port
  (`wzdc_seneca__port`) kann nur das erste Listen binden. Ohne festen Port
  nimmt jedes einen eigenen zufälligen. [Quelle] mwm-node gibt mit `--port N`
  dem zweiten Pin N+1 und so fort.

Was die Gegenseite davon zwingend braucht [Quelle]: `config.pin` als
nicht-leeres Array (`removeClient` liest `meta.config.pin[0]` und wirft
sonst), `config.host`/`config.port`, `identifier$` (eindeutig, steckt in der
Client-Kennung), `tag$` (sonst unsichtbar), `instance` (verschieden von der
eigenen) und `v$`.

---

## 5. SWIM: Fehlererkennung

Die Zeiten setzt sneeze, nicht swim-ng (dessen eigene Vorgaben wären
20/4/12/3/10 ms): [Quelle]

| | ms |
| --- | --- |
| `interval` – ein Ping pro Intervall | 333 |
| `pingTimeout` – dann PingReq über andere | 444 |
| `pingReqTimeout` – dann Verdacht | 555 |
| `pingReqGroupSize` | 7 |
| `suspectTimeout` – dann faulty | 999 |
| `joinTimeout` | 999 |
| `joinCheckInterval` (swim-ng-Vorgabe) | 50 |
| `disseminationFactor` | 22 |

Der Ablauf: [Quelle, Zeiten gemessen]

1. Alle 333 ms (der erste sofort, `setImmediate`) wählt `Membership.next()`
   ein Mitglied: zufällig aus denen, die in dieser Runde noch nicht dran
   waren; ist die Runde leer, beginnt eine neue mit allen alive und suspect.
2. Ping mit neuer `seq`. Kommt binnen 444 ms ein Ack mit dieser `seq`, ist
   alles gut.
3. Sonst PingReq `{seq, destination}` an bis zu 7 zufällige Mitglieder –
   **das geprüfte Mitglied kann darunter sein**, `random()` nimmt es nicht
   aus; es wird dann gebeten, sich selbst zu pingen. Jedes PingReq hat eine
   eigene `seq`.
4. Der Empfänger einer PingReq pingt `destination` mit eigener `seq` und
   leitet einen Ack binnen 444 ms als Ack mit der ursprünglichen `seq` an den
   Absender der PingReq weiter.
5. Kommt binnen 555 ms über keinen Weg ein Ack, wird das Mitglied verdächtigt
   (`onSuspect`) – mit dem Stand, den es beim Ping hatte. Gibt es keine
   Mitglieder für eine PingReq, bleibt es dabei: kein Verdacht.
6. 999 ms später wird es faulty, mit der Inkarnation des Verdachts.

Ein harter Ausfall wird im Harness nach **2,1 bis 2,7 s** bemerkt [gemessen,
`interop.js` Schritte 5 und 7], drei Knoten.

**Nur wer selbst geprüft hat, stellt die Uhr.** Ein Knoten, der „X suspect“
hört, merkt sich den Zustand und wartet auf das faulty des Prüfers; er startet
keinen eigenen Timer. [Quelle]

Ein Ack von einem Mitglied, das wir für verdächtig halten, beantwortet
`Membership.onAck` mit einem Update an dieses Mitglied, das seinen
verdächtigen Stand enthält – so erfährt es davon und kann widersprechen.
[Quelle]

---

## 6. SWIM: Zustände und Inkarnationen

Ein Update über ein *anderes* Mitglied `h` mit Inkarnation `i`: [Quelle,
Regeln einzeln in `test/unit/swim_test.c`]

| Update | wird übernommen, wenn | sonst |
| --- | --- | --- |
| alive | kein faulty-Eintrag für `h` mit Inkarnation ≥ `i`, **und** `h` unbekannt oder `i` **echt größer** als die bekannte | verworfen |
| suspect | kein faulty-Eintrag mit ≥ `i`, und `h` unbekannt, `i` größer, oder `i` gleich und bekannt als alive | verworfen |
| faulty | `h` bekannt (alive/suspect) und `i` ≥ bekannte | verworfen – auch wenn `h` nie bekannt war |

Folgen, die man kennen muss:

- **Ein alive mit derselben Inkarnation hebt einen Verdacht nicht auf.** Wer
  verdächtigt wird, muss seine Inkarnation erhöhen.
- **faulty-Einträge werden nie vergessen** (`hostToFaulty`), sie gehen bei
  jedem Sync mit hinaus. Ein Neustart auf demselben `ip:port` kommt nur mit
  einer größeren Inkarnation zurück – die `Date.now()` beim Start liefert.
- Ein Update, das übernommen wird, löscht einen faulty-Eintrag und (bei alive)
  den eigenen Verdachtstimer. Ein faulty löscht den Verdachtstimer nicht; er
  läuft ins Leere.
- Ein neues Mitglied löst `Change` und `Update` aus, ein faulty ebenso; alles
  andere, was übernommen wird, nur `Update`. Verworfenes löst `Drop` aus.

Über **sich selbst** (`host` gleich dem eigenen):

| Update | Wirkung |
| --- | --- |
| alive mit `i` > eigene | eigene := `i` + 1, Update verbreiten |
| alive sonst | nichts |
| suspect oder faulty mit `i` > eigene | eigene := `i` + 1 |
| suspect oder faulty mit `i` = eigene | eigene := `i` + 1 |
| suspect oder faulty mit `i` < eigene | Inkarnation bleibt |
| suspect oder faulty, in jedem Fall | den eigenen Stand als Update verbreiten |

`preferCurrentMeta` ist an (microwizard setzt es): die eigene Meta wird nie
durch die eines fremden Updates ersetzt.

---

## 7. SWIM: Verbreitung

Jedes übernommene Update (auch das eigene) kommt in den Disseminator, ein
neueres über denselben Host ersetzt ein älteres. Es reitet huckepack auf
Ping, Ack und PingReq mit, bis es `limit`-mal erzählt wurde: [Quelle,
gemessen für zwei Mitglieder: 11-mal]

```
limit = ceil(22 · log10(n + 1)),   n = Mitglieder alive/suspect + 1 (man selbst)
```

Bei jedem `Change` wird `limit` neu berechnet und was es schon erreicht hat,
fällt heraus. Ausgewählt wird nach Erzählzahl aufsteigend, innerhalb gleicher
Zahl in der Reihenfolge, in der die Updates in diese Stufe kamen, und nur was
noch passt (`länge + 2 ≤ platz`). Sync und die Antwort darauf tragen keine
Huckepack-Updates.

---

## 8. SWIM: der Join

[Quelle, gemessen]

1. Der neue Knoten schickt an jede Basis (außer sich selbst) ein Compound:
   `Sync` mit den eigenen Daten, dahinter ein `Update` für jeden bekannten
   (alive, suspect, faulty – beim ersten Join niemand).
2. Die Basis übernimmt den Absender als alive und antwortet mit Updates über
   **alle**: alive und suspect, sich selbst, die faulty-Einträge.
3. Alle 50 ms prüft der Knoten, ob eine der Basen jetzt Mitglied ist (der
   Schlüssel muss genau der String aus der Basenliste sein). Ja: fertig.
4. Nach 999 ms ohne Antwort: `JoinFailedError`. sneeze versucht es bis zu
   22-mal nach 111–555 ms wieder, **mit einer neuen Swim-Instanz** (und ohne
   festen Port auf einem neuen Port) – die alte wird nicht geschlossen, sie
   läuft mit ihrem Socket weiter. Nach dem letzten Versuch: `error`, und der
   Knoten bleibt draußen.

Danach erfährt der Rest des Mesh von dem Neuen über das Huckepack der Basis.

### Wenn Basen gehen

`mesh.js` versucht mit `discover.rediscover` (Gateway und Dienste haben es,
die Basis nicht) einen neuen Join, wenn ein Mitglied mit Pin
`base:true,role:mesh` faulty wird und weniger als zwei Basen übrig sind.
`aliveBases` wird dabei nie hochgezählt: `addClient` vergleicht
`meta.config.pin[0]` mit `'base:true,role:mesh'`, die Basis kündigt aber
`'role:mesh,base:true'` an. Der Zähler wird also nur kleiner. [Quelle]

Eine Basis, die schneller neu startet, als sie faulty wird, beginnt mit
leerer Tabelle; die anderen pingen sie weiter, sie antwortet, und niemand
schickt ihr einen Sync. Sie kennt danach nur, wer neu dazukommt. [Quelle]

mwm-node macht es einfacher und stetiger: ist keine der Basen Mitglied,
schickt es alle 1111 ms wieder einen Sync an alle Basen; scheitert ein Join,
nach 111–555 ms wieder – ohne Obergrenze und auf demselben Port.

### Neustart auf demselben Port

**Ein Knoten, der auf demselben `ip:port` neu startet, bevor er faulty
geworden ist** (unter ~2 s), wird mit seiner neuen, höheren Inkarnation
übernommen – aber unter der alten Kennung bleibt er in sneeze und in
microwizard stehen: sneeze führt Mitglieder nach `identifier$`, der SWIM-
Eintrag ist derselbe Schlüssel, ein faulty für die alte Kennung kommt nie. Der
Balancer behält den alten TCP-Port als Ziel, bis der Aufrufer neu startet;
Aufrufe dorthin laufen in den connectTimeout (355 ms), öffnen den Breaker und
gehen beim nächsten Versuch woandershin. [gemessen mit einem Ad-hoc-Lauf:
kein `remove` für die alte Kennung]

Ein Dienst sollte also entweder einen frischen SWIM-Port nehmen oder länger
als ~3 s weg sein. `interop.js` wartet in Schritt 4 auf das faulty.

**sneeze-ng 0.13.1** (Zweig fix/restart-same-host) behebt das in
`add_node`: [Quelle]

1. dieselbe Nachricht noch einmal (Inkarnation, `host`, `v$` gleich): nichts
2. die Kennung ist unter einem *anderen* Host mit *neuerer* Inkarnation
   bekannt: eine Nachricht von einem verlassenen Join-Versuch, nichts
3. unter demselben Host steht eine andere Kennung mit neuerer Inkarnation:
   die Nachricht ist die veraltete, nichts
4. sonst gehen alle anderen Kennungen unter demselben Host (`remove`), dann
   kommt die neue (`add`) – bei gleicher Inkarnation ebenfalls

und `remove_node` entfernt eine Kennung nur, wenn sie unter genau diesem
Host steht. `src/mesh.c` macht es so (`test/unit/mesh_test.c` Regel für
Regel), und damit der Client: ein auf denselben Ports neu gestarteter Dienst
ersetzt sein altes Ziel nach rund 0,3 s statt nie. [gemessen, `meshclient.js`
14] Die Node-Knoten im Harness laufen mit 0.13.0 und behalten die
alte Kennung weiterhin.

mwm-node selbst kommt nach einem Neustart mit neuer `instance`, damit neuer
`identifier$`, und mit `Date.now()` als Inkarnation – größer als alles, was
der Lauf davor hatte, solange die Uhr nicht zurückspringt.

---

## 9. sneeze und das Mesh in microwizard

sneeze sitzt auf swim-ng und meldet `add`/`remove` nach **`identifier$`**:
[Quelle]

- Mitglieder ohne Meta, mit anderem `tag$` oder mit der eigenen Kennung
  werden übergangen.
- `state` 0 → `add`, wenn die Kennung neu ist oder sich Inkarnation, `host`
  oder `v$` geändert haben. `state` 2 → `remove`. `state` 1 sagt nichts:
  ein Verdächtiger bleibt Ziel, bis er faulty ist.
- Ausgewertet werden `Change` und `Update` von swim-ng.

`mesh.js` macht aus `add` einen Client pro Pin: [Quelle, gemessen]

- `pinConfig = config ohne pin(s), pin = config.pin[0]`, und als Kennung
  `pinConfig.id = utilPattern(pinConfig) + '~' + identifier$`.
  `utilPattern` nimmt alle primitiven Felder ohne `$` im Namen, sortiert:
  `connectTimeout:355,host:127.0.0.1,ismesh:true,model:consume,pin:role:echo,cmd:*,port:40709,timeout:5555,type:tcp~<identifier$>`
- `null:true` wird übergangen; ein Pin, den der Knoten selbst bedient,
  ebenso (`ignoreClient`, Vergleich des Pins als String), und Mitglieder der
  eigenen `instance`.
- `type` außer `tcp` (oder fehlend) kann der Client nicht; `model:
  'observe'` (ab 1.6) liefert an alle Ziele statt an eines – beides macht
  der Client in meta nicht und übergeht solche Pins mit einer Zeile auf
  stderr.
- Pro Pin gibt es einen Balance-Client (einmal), darunter pro Anbieter einen
  TCP-Client – der verbindet sich erst beim ersten Aufruf.
- `remove` nimmt den Anbieter aus dem Balancer. In 1.5.0 bleibt der Pin
  selbst registriert: ohne Anbieter ergibt ein Aufruf `no-current-target`,
  und das für immer, auch wenn ein weiterer Pin eines lebenden Dienstes
  passen würde (ein toter `role:a,cmd:x` gewinnt gegen ein lebendes
  `role:a,cmd:*`). [Quelle]
- **Seit microwizard fix/dead-pin** (und so im Client in meta) verlässt ein
  Pin den Musterbaum, wenn sein letztes Ziel geht (`#dropPin`): der Aufruf
  fällt auf den nächstbesten Pin durch, gibt es keinen, ist er `undefined`.
  Ein Ziel, das visigoth nur geöffnet hat, ist nicht gegangen; ein eigener
  Handler auf demselben Pin hält den Knoten wieder; kündigt ein Mitglied
  den Pin wieder an, ist er wieder da. Der Balance-Client (in meta: die
  Gruppe mit ihrem visigoth) bleibt dabei bestehen. [gemessen, `deadpin.js`,
  `client_test`]
- Mit mehreren Listens meldet jede sneeze-Instanz `add` und `remove`;
  `add` wird über die Kennung des Ziels entdoppelt, das erste `remove`
  nimmt das Ziel heraus. Der Client in meta macht es genauso.

---

## 10. Aufrufe: ndjson über TCP

Eine Verbindung pro Anbieter, eine Zeile JSON pro Nachricht
(`JSON.stringify` + `\n`), in beide Richtungen. Was hier steht, ist aus
`transfer.js` und `tcp.js`; was die Fälle beim Aufrufer ergeben, hält
`test/harness/reference.js` fest.

### Anfrage

```
{"id":"<uid>","k":"a","sync":true,"args":{...}}
{"id":"<uid>","k":"aE","sync":true,"p":"<pin>","d":{...}}
```

- `id`: `uid()` des Aufrufers, 11 Hex-Zeichen.
- `k: "a"` ist der Normalfall. `args` ist das ganze Argument: das Muster als
  Objekt mit dem zweiten Argument von `act` darübergelegt –
  `act('role:echo,cmd:ping', {n: 1})` schickt
  `{"role":"echo","cmd":"ping","n":1}`. Die Werte aus dem Muster-String sind
  Strings. [gemessen]
- `k: "aE"` entsteht, wenn lokal `actE` gerufen wurde (dann steht `meta.pin`):
  `p` ist der Muster-String, `d` die Daten. [Quelle]
- `sync` ist immer `true` (nur ein Aufrufer, der es ausdrücklich abschaltet,
  schickt `false`).

### Wie der Dienst das Ziel findet

`handleRequest` ruft `mc.act(args)` bzw. `mc.actE(p, d)`. Für `a` wird das
Muster aus **allen primitiven Feldern auf oberster Ebene** von `args` gebaut
(`key:${value}`, ohne Doppelte, sortiert; eine Zahl so, wie JS sie schreibt);
Felder vom Typ `object` (auch `null`), `undefined`, `function`, `symbol`
zählen nicht. Für `aE` sind es die Paare von `p`, genau so, wie die Regex von
`convertData` sie findet. Der Handler bekommt `args` ganz bzw. `d`. [Quelle]

**Welcher Pin gewinnt** (`#matchPin`, so seit microwizard fix/pattern-shadow;
`src/pattern.c` für beide Seiten von mwm-node): [gemessen, `pattern_test`]

- Ein Pin nimmt eine Nachricht, die jedes seiner Paare hat; ein Paar mit `*`
  jeden Wert dieses Schlüssels. Teile der Nachricht, die der Pin nicht
  nennt, werden übergangen.
- Von den Pins, die sie nehmen, gewinnt der mit den **meisten Paaren**, dann
  der mit den **meisten exakten Paaren** – ein `role:a,cmd:ping` schlägt ein
  `role:a,cmd:*` –, dann der, auf den der Baum zuerst führt: die Schlüssel
  der Nachricht alphabetisch, bei jedem erst die exakten Zweige, dann der
  Platzhalter. Bei `a:*,b:1` gegen `a:1,b:*` und `a:1,b:1` gewinnt `a:1,b:*`.
- Der Baum wird ganz abgesucht: ein exakter Zweig, der ins Leere führt,
  verdeckt keinen Platzhalter daneben. In 1.5.0 lief der Abgleich gierig
  durch den Baum; ein Knoten mit `role:mwmobserver,cmd:ping` bekam für
  `role:mwmtest,cmd:ping` `no-target`, obwohl ein anderer Dienst
  `role:mwmtest,cmd:*` anbot.
- Pins mit denselben Paaren (auch anders geschrieben, `cmd:*,role:a`) sind
  ein Knoten; ihn hält, was zuletzt hinzugefügt wurde, und geht das (ein
  Pin ohne Anbieter, Abschnitt 9), was davor dort hinzugefügt wurde.
- Eigene Handler und die Pins anderer Dienste stehen in einem Baum: der
  Client fügt jeden Pin eines Mitglieds mit `add` hinzu wie einen Handler.
  Ein eigener `role:a,cmd:ping` schlägt ein fremdes `role:a,cmd:*`, ein
  fremdes `role:x,cmd:*` schlägt einen eigenen `role:x`.
- `act(muster, args)` gleicht die Paare des Musters **und** die Felder von
  `args` ab, beide: steht ein Schlüssel in beiden, sind es zwei Teile.

Auf der Dienstseite von mwm-node antworten nur die eigenen Handler: ein
hereinkommender Aufruf wird nicht an einen fremden Pin weitergegeben (Node
täte es, weil dort ein Baum für beides da ist).

**Findet sich kein Handler** – etwa `role:echo,cmd:nothing` bei einem Pin
`role:echo,cmd:*` ohne Handler für `nothing` –, schreibt microwizard
`no-target` auf die Konsole und antwortet mit `res: undefined`, also einer
Antwort ganz ohne `res`. Der Aufrufer bekommt `undefined` als Ergebnis.
[gemessen]

### Antwort

```
{"id":"<id der anfrage>","k":"res","sync":true,"res":{...}}
{"id":"<id der anfrage>","k":"res","sync":true,"error":{"message":"…","name":"Error","stack":"…", …}}
```

- `sync` wird aus der Anfrage übernommen. Eine Anfrage mit `sync: false`
  bekommt keine Antwort (der Handler läuft trotzdem).
- `error` ist `Object.assign({}, err)` plus `message`, `name`, `stack` – also
  alle aufzählbaren Eigenschaften des geworfenen Fehlers (`code`, `details`
  …).
- Ein Dienst, der gerade schließt (`role:mc,cmd:close`), antwortet allem mit
  `error: {message: "retry_later_err_overload", name: "Error"}`. `close`
  wartet, bis alle laufenden Aufrufe geantwortet haben (`#active`, alle
  500 ms geprüft), dann noch 500 ms, dann schließt es; eine solche Antwort
  zählt nicht als laufend.
- Aufrufe über eine Verbindung laufen alle gleichzeitig: jede Zeile ist ein
  `await tp.handleRequest`, die Antwort geht hinaus, wenn sie fertig ist.
  Eine Grenze hat microwizard nicht. Ein Dienst, der Überlast melden will,
  wirft einen Fehler mit `details: {message: "retry_later_err_overload"}` –
  nur diese Form zählt balance.js als Überlast (Abschnitt 11).
- `k` weder `a` noch `aE`: keine Antwort (`{input, error: 'unknown method'}`
  ohne `sync` wird nicht geschrieben).
- Eine Zeile, die kein JSON ist: der Parser meldet einen Fehler, und der
  Dienst **schließt die Verbindung** (`connection.end()`).

### Was der Aufrufer mit der Antwort macht

[Quelle; gemessen, wo vermerkt]

- `res` wird das Ergebnis von `actAsync`. [gemessen]
- `error` wird ein `new Error(message)` mit allen Feldern daraufkopiert.
- Eine Antwort mit `sync: false` wird angenommen und sonst ignoriert – der
  Aufruf wartet weiter bis zum Timeout. [gemessen] **Eine Antwort muss `sync: true`
  tragen** (oder es weglassen: fehlt es, gilt `true`).
- **Eine Antwort mit einem anderen `k` als `res` oder mit `id: null`**
  läuft in `this._context.options.warn…` – `_context` gibt es nicht (das
  Feld heißt `#context`), das wirft einen `TypeError` im `_write` des
  Nachrichtenstroms. Der Prozess überlebt, aber **die Verbindung ist danach
  tot**: dieser Aufruf und jeder weitere über sie läuft in den Timeout (5555
  ms), bis der Keepalive sie nach bis zu zwei Minuten verwirft. [gemessen mit
  einem Ad-hoc-Server: erster und zweiter Aufruf je `timeout` nach 5,56 s]
  **Seit microwizard fix/unknown-reply** wird jede unbrauchbare Antwort –
  kein JSON, `null`, kein Objekt, ein anderes `k`, `id: null`, eine
  unbekannte `id` – verworfen, einmal je Art alle zehn Sekunden gemeldet,
  und die Verbindung bleibt. Der Client in meta macht es so. [gemessen,
  `client_test` „junk“: neun unbrauchbare Zeilen vor der Antwort, danach
  noch ein Aufruf über dieselbe Verbindung]
- **Eine Antwort mit einer unbekannten `id`** läuft in
  `this.#context.options.warn.unknown_message_id` – `#context` hat kein
  `options` (die Einstellungen heißen dort `opts`); derselbe `TypeError`,
  dieselbe Folge. [gemessen: der Aufruf läuft in den Timeout] Ein Eintrag wird
  100 ms nach seiner Antwort gelöscht; die LRU fasst 10000. **Ein meta-Dienst
  darf also nie ungefragt antworten und nie zweimal auf dieselbe `id` mit
  mehr als 100 ms Abstand.** Eine zweite Antwort binnen 100 ms und eine späte
  nach einem Timeout sind harmlos: der Eintrag ist dann noch da, das zweite
  `done` geht ins Leere. [gemessen: zweimal im Abstand von 300 ms – das
  Ergebnis kam an, die zweite Antwort traf einen gelöschten Eintrag; der
  Prozess lebt, die Verbindung ist danach, wie oben, tot]

### Zeiten und Keepalive

| | |
| --- | --- |
| `connectTimeout` | 355 ms bis die Verbindung steht, sonst `'timeout'` |
| `timeout` | 5555 ms bis zur Antwort, sonst `'timeout'` |
| Reconnect | `reconnect-core`, `failAfter: 3` |
| Keepalive des Clients | alle 2 min `{"h":1}`; kommt binnen 5 s **irgendeine** Zeile zurück, gut, sonst wird die Verbindung verworfen und erst beim nächsten Aufruf neu aufgebaut |
| Antwort des Dienstes | `{"h":2}` sofort auf `{"h":1}` |
| Leerlauf beim Dienst | nach 3 min ohne Verkehr `connection.end()` |

Beide Timeouts kommen aus der `config` der angekündigten Meta (Abschnitt 4).

- Eine Verbindung, die nicht zustande kommt (refused), ist kein Fehler,
  sondern nach `connectTimeout` ein `timeout`: der Aufruf wartet auf den
  nächsten Versuch von reconnect-core. [Quelle; gemessen am Client in meta:
  ein totes Ziel kostet 355 ms und die Wiederholung, `meshclient.js` 12]
- Bricht eine Verbindung ab, schlägt das nichts fehl, was schon über sie
  gesendet wurde: diese Aufrufe laufen in ihr `timeout` (5555 ms). Erst der
  nächste Aufruf baut neu auf. [Quelle; so auch in meta]
- `callmap` gehört dem Transport, nicht der Verbindung: eine Antwort findet
  ihren Aufruf allein über die `id`, über welche Verbindung sie auch kommt.

---

## 11. Verteilung: balance.js und visigoth

Pro Pin ein Balance-Client mit einem visigoth darin, pro Anbieter ein Ziel.
visigoth 2.0.5 ist eine Eigenentwicklung (Fork) und kein Lehrbuch-Breaker.
Alles hier ist in `src/visigoth.c` nachgebaut und gegen das echte visigoth
geprüft: `test/harness/visigoth-trace.js` treibt es durch 3000 zufällige
Schritte (Hinzufügen, Entfernen, Wählen, Fehler, Uhr), `visigoth_test`
spielt dieselben Schritte nach und vergleicht nach jedem Schritt Ergebnis
und Ring. [gemessen]

### Der Ring

`BalanceLinkedRing`: eine verkettete Liste mit Score, in microwizard immer
Score 1, und **ein Cursor für alle Aufrufe**, der nur vorwärts läuft
(`getAndStep`: den Cursor zurückgeben, weiterrücken, am Ende zurück an den
Anfang). Neue Ziele kommen bei gleichem Score ans Ende. Die Ringgröße ist
40 (`ringSize`); ein 41. Ziel bekommt `{code: 1, err: 'ring full'}` und kein
Element, und `balance.js` speichert `target.e = undefined` – fällt dieser
Anbieter später weg, wirft `visigoth.remove(undefined)`. [Quelle]

Zwei Eigenheiten von `addScore`, die bei Score 1 nie greifen und nachgebaut
sind: ein Score über dem des Kopfes wird vor den Kopf gehängt, aber nie
selbst zum Kopf (vom Kopf aus unerreichbar); die Suche nach der Stelle für
einen kleineren Score hält ein Element zu spät.

### Die Wahl

```
choose():
  leer → Fehler "no upstreams available"
  start = getAndStep(); u = start
  wiederhole:
    wenn u OPEN:
      wenn jetzt − u.seit > closingTimeout:  u := HALF-OPEN, seit := jetzt
      sonst: u = getAndStep(); weiter (Abbruch, wenn wieder bei start)
    (rater: Round Robin gibt immer 1 – "Score ≤ 0 öffnet" greift nie)
    wenn u nicht OPEN: gewählt
    u = getAndStep()
  bis u == start
  keiner → Fehler "no upstreams available"
  gewählt: callback(target, errored, stats); danach HALF-OPEN → CLOSED
```

- **Round Robin** in Einfügereihenfolge. Jeder Schritt über ein offenes Ziel
  rückt den gemeinsamen Cursor weiter.
- `closingTimeout`: visigoth hat 30000 ms, **balance.js setzt 1000 ms**.
  Verglichen wird mit „größer als“: genau 1000 ms nach dem Öffnen ist noch
  offen.

### Zustände

| von | nach | wodurch |
| --- | --- | --- |
| CLOSED | OPEN | **ein einziger** gemeldeter Fehler (`errored()`); es gibt keinen Zähler und keine Schwelle |
| OPEN | HALF-OPEN | ein `choose`, das das Ziel mehr als `closingTimeout` nach dem Öffnen erreicht |
| HALF-OPEN | CLOSED | **sofort beim Gewähltwerden** – nicht beim Erfolg des Aufrufs |
| HALF-OPEN/CLOSED | OPEN | `errored()` des laufenden Aufrufs |

Die Abweichungen vom Lehrbuch, ausdrücklich:

- Kein Fehlerzähler, keine Fehlerquote: ein Fehler öffnet.
- Half-open ist kein Probezustand mit genau einem Probeaufruf. Das Ziel ist
  geschlossen, sobald es gewählt wurde; alle folgenden Aufrufe gehen wieder
  hin, bis einer davon einen Fehler meldet. Der Kommentar im Code („Close the
  circuit once it has been successful“) beschreibt etwas anderes, als der Code
  tut – das Schließen steht nach dem *synchronen* Aufruf des Callbacks, der den
  Aufruf erst startet.
- Kein Backoff: offen ist immer genau `closingTimeout` lang, egal wie oft.
- Ein offenes Ziel wird nur geprüft, wenn der Cursor darauf trifft – es gibt
  keinen Timer.
- `errored()` setzt OPEN mit neuem Zeitstempel, auch wenn das Ziel schon offen
  war (spätere Fehler verlängern).

### Was als Fehler zählt (balance.js)

```
target.action(msg) → Erfolg:  done(null, ergebnis)
                   → Fehler:  wenn err.details?.message === 'retry_later_err_overload'
                                   oder err.message === 'timeout':
                                errored(); wenn ++versuche < 10: in 100 ms neu wählen
                                sonst done({err: 'all-targets-overloaded', msg})
                              sonst: done({err, msg})   – kein errored(), kein Retry
choose scheitert:             done({err: 'no-current-target', msg})
```

- **Zeitüberschreitung** (connectTimeout 355 ms oder timeout 5555 ms): zählt.
  `done('timeout')` ist ein String; bluebird macht daraus ein
  `Error('timeout')`, darum greift `err.message === 'timeout'`.
- **Überlast** zählt nur, wenn der Fehler `details.message ===
  'retry_later_err_overload'` trägt. Die Überlastantwort eines schließenden
  Dienstes (`error.message` direkt, ohne `details`) **zählt nicht** – sie
  geht als gewöhnlicher Fehler an den Aufrufer.
- **Anwendungsfehler** (Handler wirft) zählen nicht und werden nicht
  wiederholt.
- `retryTimeout` ist 100 ms, bis zu 10 Versuche.
- **Seit 1.6.2 bekommt jede Wiederholung eine neue `id`** (`uid()`), damit
  eine späte Antwort auf den Versuch davor nicht als ihre gilt. [Quelle;
  gemessen am Client in meta, `client_test` „failover“]
- **Mit einem einzigen Anbieter gibt es keine zehn Versuche**: der erste
  Fehler öffnet ihn für 1000 ms, die Wiederholung 100 ms später findet kein
  Ziel, der Aufrufer bekommt `{err: 'no-current-target', msg}`. Zehn Versuche
  gibt es nur, wenn genug Anbieter da sind. [gemessen, `reference.js` 4 und 6]

### Was beim Aufrufer ankommt

| Fall | `actAsync` |
| --- | --- |
| Antwort mit `res` | erfüllt mit `res` |
| Antwort mit `error`, keine Überlast | **verworfen mit `{err: Error, msg}`** – einem Objekt, kein Error; `err` trägt die Felder des Dienstes und von bluebird `isOperational` und `cause` [gemessen] |
| Überlast/Timeout, alle Versuche verbraucht | verworfen mit `{err: 'all-targets-overloaded', msg}` |
| kein Ziel wählbar | verworfen mit `{err: 'no-current-target', msg}` |
| Balance-Client ohne visigoth (zwischen Anlegen und erstem Ziel) | **erfüllt** mit `{err: 'no-target', msg}` |
| gar kein passender Pin lokal | **erfüllt mit `undefined`**, `no-target` auf der Konsole |

`src/balance.c` ist dieses consume als Folge von Schritten ohne Uhr,
`test/unit/balance_test.c` spielt die Fälle mit dem visigoth-Port auf
falscher Uhr durch, `meshclient.js` vergleicht den Client in meta Fall für
Fall mit einem Node-Aufrufer.

### Die Änderung `patient` (Standard)

Entschieden und in microwizard auf dem Zweig fix/consume-wait
(`balance.js` consume, `index.js`); hier der Standard. Wer consume genau wie
1.6.2 braucht, schaltet sie ab: `bal_options_t.patient = 0`,
`mwm-node --no-patient` (`--patient` gibt es weiter, es ist der Standard).

- scheitert die Wahl, nachdem ein Versuch gescheitert ist, wird **einmal**
  `closingTimeout + retryTimeout` (1100 ms) gewartet – dann ist das zuvor
  geöffnete Ziel halb offen – und noch einmal gewählt, bevor es
  `no-current-target` heißt
- der ganze Aufruf dauert höchstens **ein Anfrage-Timeout**: in Node
  `balance.timeout`, sonst das `timeout` des Transports (5555 ms), wie
  `Micro` es dem Balancer gibt; hier `bal_options_t.budget`, sonst das
  `timeout` des Clients (`mwm-node --timeout`, 5555 ms). Ein Versuch bekommt
  nur, was davon übrig ist; ist nach der Wahl nichts mehr übrig, heißt es
  `all-targets-overloaded`, ohne Versuch; eine Wartezeit, die danach enden
  würde, beendet den Aufruf (`no-current-target` für das eine Warten,
  `all-targets-overloaded` für eine Wiederholung)

Mit einem einzigen Anbieter, der einmal überlastet ist, wird daraus nach
rund 1,2 s die Antwort statt nach 0,1 s `no-current-target`; ist er immer
überlastet, nach dem einen Warten `no-current-target`. Ein Timeout
verbraucht das Budget, ohne Wiederholung. Zwei Anbieter, die beide nicht
antworten, kosten mit `closingTimeout` unter dem Timeout ein Timeout statt
zehn. [gemessen, `balance_test` – auch die Fälle von `test/balance.test.js`
aus fix/consume-wait mit kurzem Budget –, `client_test` und
`client_test --short` (Timeout 400 ms)] Sonst ändert sich mit mehreren
Anbietern nichts. `meshclient.js` vergleicht mit microwizard 1.5.0 und ruft darum mit `--no-patient`.

---

## 12. Was ein Node-Aufrufer daraus macht

Die übliche Konvention der Dienste ist eine Antwort `{code, msg}` bei Erfolg
und `{code: >= 400, err: "Text"}` bei einem fachlichen Fehler – beides als
`res`, nicht als `error`. Ein Aufrufer prüft `code` selbst (etwa
`actAsync(...).then(defaultError)`, `test/harness/lib/gateway.js`).

- Eine **Verwerfung** (`{err, msg}` aus Abschnitt 11) ist kein `Error`.
  graphql-js 16 macht daraus `Unexpected error value: { err: …, msg: … }` –
  mit den Argumenten des Aufrufs im Text. Ein meta-Dienst sollte fachliche
  Fehler deshalb als `res: {code, err}` beantworten, nie als `error`.
- Ein Ergebnis `undefined` (kein Handler hinter dem Pin) läuft in einem
  solchen `defaultError` auf `Cannot read properties of undefined (reading
  'code')`.

---

## 13. Die typisierte Darstellung

Für die Golden Files braucht es eine Schreibweise, die sagt, was die Bytes
*waren* – `msgpackr.unpack` vergisst genau das (float64 oder uint64?).
`test/harness/lib/mpdump.js` liest die Bytes selbst, `mp_dump` in
`src/msgpack.c` schreibt dasselbe:

```
nil  true  false                     c0, c3, c2 (und d4 00 00, msgpackr's undefined)
u:5   i:-5                           jede int-Form; u für ≥ 0, i für < 0
f:1793405011551                      float32/64 mit ganzem Wert bis 2^53
f:0x3ff8000000000000                 jeder andere float, als Bits
"text"                               JSON.stringify-Escaping
bin:00ff                             bin als Hex
[a,b]  {"k":v}                       Array, Map (Schlüssel wie Werte geschrieben)
```

Die Golden Files (`test/fixtures/`):

| Datei | woher | was der Test verlangt |
| --- | --- | --- |
| `msgpackr-samples.txt` | `pack-samples.js` | Darstellung aus den Bytes und aus dem Dokument gleich; zurückgeschrieben dieselben Bytes (außer BigInt und `undefined`, die JS selbst nicht so schreiben würde) |
| `swim-bodies.txt` | `capture.js` + `fixtures.js` von echten Knoten | ebenso |
| `swim-frames.txt` | ebenso | dieselben Teile aus jedem Datagramm |
| `visigoth-trace.txt` | `visigoth-trace.js` | dieselbe Entscheidung und derselbe Ring nach jedem Schritt; `test/run.sh` erzeugt sie neu und vergleicht |
| `pattern-trace.txt` | `pattern-trace.js` (Seed 7, 300 Zufallsfälle) aus microwizard fix/pattern-shadow | derselbe Gewinner für jede Nachricht; `test/run.sh` erzeugt sie neu und vergleicht, wenn es einen Checkout davon findet (`MWM_MICROWIZARD`), und vergleicht dazu eine frische Spur mit zufälligem Seed |

---

## 14. Was mwm-node davon tut, und was offen ist

Die Dienstseite (`src/service.c`, die Verbindungen in `src/main.c` auf
`meta_tcp.h` vom Zweig feat/stream): `reference.js` vergleicht jeden Fall aus
Abschnitt 11 zwischen dem Node-Echo und dem meta-Echo, die Sicht des
Gateways eingeschlossen – sie sind gleich bis auf Rolle und Stack.

- Eine Antwort genau einmal, mit `k: "res"`, der `id` so, wie sie kam (auch
  als Zahl), und `sync` aus der Anfrage; bei `sync: false` und fremdem `k`
  keine.
- `{"h":1}` → `{"h":2}`; eine Zeile, die kein JSON ist, beendet die
  Verbindung; nach 3 min ohne Zeile wird sie geschlossen.
- Muster wie microwizard fix/pattern-shadow (Abschnitt 10): alle primitiven
  Felder auf oberster Ebene von `args`, `*` als Platzhalter; die meisten
  Paare, dann die meisten exakten, dann der erste Weg durch den Baum; für
  `aE` die Paare aus `p`, der Handler bekommt `d`. Ohne passenden Handler:
  eine Antwort ohne `res`.
- Ein geworfener Fehler geht mit seinen Feldern, `name: "Error"` und einem
  `stack` (`<name>: <message>\n    at mwm-node (…)`) hinaus – den übernimmt
  der Aufrufer, wie er den aus Node übernimmt.
- SIGTERM/SIGINT: ab da `retry_later_err_overload` (nur `message` und `name`,
  wie transfer.js); die laufenden Aufrufe werden fertig und beantwortet,
  500 ms nach dem letzten endet der Prozess – wie `close`, nur dass hier
  alle 50 ms statt alle 500 ms nachgesehen wird. Ein zweites Signal beendet
  sofort. [gemessen, `concurrency.js` 10]
- Pro Verbindung ein Leser und ein Task pro Aufruf: beliebig viele Aufrufe
  über dieselbe Verbindung laufen gleichzeitig, die Antworten gehen in der
  Reihenfolge, in der sie fertig sind, jede als eine ganze Zeile. Über alle
  Verbindungen zusammen laufen höchstens `--max-inflight` (10000) Aufrufe;
  einer darüber wird vom Leser sofort beantwortet, ohne Handler, mit
  `error: {message: "retry_later_err_overload", name: "Error", details:
  {message: "retry_later_err_overload"}}` – der Form, die ein Node-Dienst
  für Überlast wirft und die balance.js zählt: das Ziel öffnet, der Aufruf
  geht 100 ms später an ein anderes. [gemessen, `concurrency.js` 2–9]
- Mehrere Pins (`--pin` mehrmals): jeder ein eigenes Listen wie in
  microwizard (Abschnitt 4). [gemessen, `meshclient.js` 15–17]

Die Aufrufseite (`src/client.c`, `mwm_act`/`mwm_actE`, auf der Kommandozeile
`--client`):

- Ziele aus den Mitgliedern wie in `mesh.js` (Abschnitt 9), gruppiert nach
  Pin, je Gruppe ein visigoth; consume wie balance.js aus fix/consume-wait,
  mit `--no-patient` wie 1.6.2 (Abschnitt 11).
- Pro Ziel eine Verbindung, beim ersten Aufruf aufgebaut, mit `connectTimeout`
  und `timeout` aus der angekündigten `config`, Keepalive wie tcp.js.
- Ein Task besitzt alles – Ziele, Verbindungen, die Tabelle offener `id`s –
  und hört auf einen Kanal; ein Leser-Task pro Verbindung, ein Ticker. Ein
  Aufruf läuft im Task des Aufrufers; `--client` macht aus jeder Zeile von
  stdin einen Task.
- Eigene Handler und Gruppen im selben Musterbaum (Abschnitt 10): gewinnt
  ein eigener Handler, antwortet er ohne das Mesh (wie ein lokales `act` in
  Node, dessen Wurf auch nicht in `{err, msg}` verpackt wird). Handler
  gehören vor `mwm_clientStart` hinzugefügt.
- Eine Gruppe ohne Ziel verlässt den Baum, ein neues Ziel bringt sie
  zurück (fix/dead-pin, Abschnitt 9). [gemessen, `deadpin.js`]
- `meshclient.js` vergleicht Fall für Fall mit einem Node-Aufrufer gegen den
  Node- und den meta-Echo; `client_test` deckt mit eigenen Servern ab, was
  im Mesh schwer herzustellen ist: unbrauchbare Antworten, toter Port,
  Schweigen, Keepalive ohne Antwort, die Option `patient`.

Offen:

- Der Zaun auf 127.0.0.0/8 (`--any-address`) fällt erst nach der Durchsicht
  in [zaun.md](zaun.md); DNS-Discovery über `wzdc_baseName` gibt es nicht.
- `model: 'observe'` und `act` mit `to` (an eine bestimmte Instanz, 1.6)
  kann der Client nicht.
- Der Client-Task schreibt die Anfragen selbst; ein Ziel, das nicht mehr
  liest, hält ihn bis zu `timeout` auf (zaun.md, Punkt 5).
- Ein Handler läuft in einem Task pro Aufruf; einer, der blockiert (statt zu
  parken), hält einen Carrier.
- `mwm_act` parkt den Task, der ruft; viele Aufrufe zugleich heißen viele
  Tasks (`--client` macht einen pro Zeile). Ein Aufruf, der sofort einen
  Griff zurückgibt und später abgewartet wird (`mwm_actStart` →
  `mwm_await`), bräuchte einen Rückkanal im Griff – Kanäle gehen aber nicht
  in Structs oder auf den Heap (README, „Noch offen“); mit `@future` aus
  doc/concurrency.md wäre es `go mwm_act(...)` ohne neue API.
- Der Client-Task, dem alles gehört, ist ein Task: über ihn gehen alle
  Aufrufe eines Prozesses, er schafft hier etwa 60 000–70 000 Aufrufe/s
  (README, „Last“).
