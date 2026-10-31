# Der Zaun auf 127.0.0.0/8 – was vor `--any-address` geprüft sein muss

mwm-node sendet, lauscht und wählt sich nur auf 127.0.0.0/8 ein und löst
keine Namen auf. `--any-address` hebt das auf, und zwar alles auf einmal.
Bis jeder Punkt hier mit Namen und Datum abgehakt ist, bleibt der Schalter
aus – in keinem Manifest, keinem Default, keiner Umgebungsvariable.

Die Freigabe selbst ist eine Kontrolländerung: Sie läuft als Decision Record
über Hermes (c5ai), nicht über eine Zeile in diesem Repo.

## 1. Wo der Zaun sitzt – alle Stellen gehen nur zusammen auf

- [ ] `src/main.c` `main`: `host` und jede Basis müssen `127.` sein
- [ ] `src/main.c` `sendDatagram`: kein Datagramm außerhalb 127.0.0.0/8
- [ ] `src/client.c` `dialable`: nur Adress-Literale, ohne den Schalter nur
      127.0.0.0/8, IPv6 nur mit ihm
- [ ] keine weitere Stelle sendet, lauscht oder wählt sich ein (grep nach
      `meta_udpSend`, `meta_tcpListen`, `meta_tcpDial`)
- [ ] `--any-address` ist der einzige Weg, ihn zu heben; kein `wzdc_*`

## 2. Wer im Mesh mitreden darf

SWIM und ndjson haben weder Authentisierung noch Verschlüsselung – im
Node-Mesh von `core` genauso. Wer die Ports erreicht, kann beitreten, jeden
Pin ankündigen und damit Aufrufe samt `session` auf sich ziehen, und jeden
Handler rufen.

- [ ] die Ports (SWIM UDP, TCP der Listens) sind nur aus dem Pod-Netz
      erreichbar; NetworkPolicy für genau die Mesh-Mitglieder geprüft
- [ ] `host` ist eine interne Adresse; UDP und TCP binden nur an sie
- [ ] Tag (`mc~<tag>`) und Basen der Zielumgebung stimmen – ein falscher Tag
      ist ein fremdes Mesh

## 3. Wohin der Client sich einwählt

Die Adressen der Ziele stammen aus fremden Ankündigungen. Ohne Zaun wählt
sich der Client überall hin, wo ein Mitglied es ihm sagt.

- [ ] Entscheidung: eine Erlaubnisliste von Netzen (Pod-CIDR) statt
      „alles“ – gebaut, bevor der Zaun fällt, oder bewusst verworfen
- [ ] IPv6: swim-ng teilt Hosts mit `split(':')`, IPv6 geht im Mesh nicht;
      ob der Client IPv6-Ziele überhaupt annehmen soll, ist entschieden

## 4. Namen

- [ ] es bleibt dabei: kein DNS. `wzdc_baseName` wird abgelehnt; die Basen
      kommen als Adressen (in Kubernetes
      liefert sie in Node ein headless Service per DNS)
- [ ] `meta_tcpDial` bekommt nie einen Namen (dessen Timeout deckt die
      Auflösung nicht ab)

## 5. Verhalten unter Last und bei Fehlern

- [ ] ein Ziel, das nicht mehr liest: der Client-Task schreibt selbst und
      hängt dann bis zu `timeout` (5555 ms) – alle Aufrufe stehen so lange.
      Behoben oder als Risiko angenommen
- [ ] Zeilen bis 16 MiB (Dienst und Client), Datagramme bis 64 KiB:
      Speicher pro Verbindung und Mitglied abgeschätzt
- [ ] offene Aufrufe liegen in einer Liste (linear); bei der erwarteten
      Last gemessen
- [ ] die faulty-Einträge vergisst SWIM nie (wie swim-ng); bei
      Pod-Fluktuation über Wochen abgeschätzt
- [ ] eine Antwort auf eine sterbende Verbindung wartet volle 5555 ms (wie
      tcp.js) – bekannt und akzeptiert

## 6. Neustart und Uhr

- [ ] die Inkarnation ist die Wanduhr beim Start: NTP auf den Knoten, kein
      Sprung zurück über die Zeit eines Neustarts
- [ ] Node-Dienste mit sneeze 0.13.0 behalten nach einem schnellen Neustart
      auf demselben Port die alte Kennung als Ziel; entweder 0.13.1
      überall, oder der SWIM-Port ist zufällig (der Standard), oder der Pod
      bleibt länger als ~3 s weg
- [ ] SIGTERM: 500 ms Überlastantworten, dann Ende – passt zu
      `terminationGracePeriodSeconds` und einem preStop

## 7. Nachweis

- [ ] `test/run.sh` und `MWM_SAN=1 test/run.sh` grün auf dem Stand, der
      ausgerollt wird, gegen dasselbe meta
- [ ] ein Lauf gegen eine Kopie des Mesh (Staging) mit den Node-Versionen
      der Zielumgebung (microwizard 1.5.0/1.6.x, sneeze 0.13.0/0.13.1)
- [ ] die meta-Umgehungen (README, „Was meta dabei gefehlt hat“) sind
      entweder durch das gefixte meta ersetzt oder einzeln als harmlos
      bestätigt
