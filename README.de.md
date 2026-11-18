# Truma iNet X → ESPHome (direkte BLE-Verbindung)

🇬🇧 [English version](README.md) · 🇩🇪 Deutsch

ESPHome-Komponente für einen **klassischen ESP32**: Der ESP koppelt sich selbst
mit dem Truma iNet X Panel und hält die Bluetooth-Verbindung dauerhaft. Home Assistant
spricht nur noch per ESPHome-API über WLAN mit dem ESP.

Damit entfallen die HA-Integration `truma_inetx` und der Bluetooth-Proxy.

> **Status (0.6.0, November 2026):** Getestet an einem iNet X Pro Panel (Software 3.4.27)
> mit Combi 6 und Schaudt EBL. Mit einem **klassischen ESP32** (ESP32-D0WD) läuft die
> Verbindung stabil (Langzeittest: über 18 Stunden ohne Abbruch). Mit dem ESP32-C3 klappt
> das Wiederverbinden nicht zuverlässig, siehe unten.

## Wichtigste Erkenntnisse aus dem Praxistest

1. **Am Panel darf der ESP nur genau einmal in der Geräteliste stehen.** Das Panel legt
   bei jeder Kopplung einen *neuen* Eintrag an und ersetzt den alten nicht. Stehen zwei
   Einträge mit derselben MAC drin, verwirft das Bluetooth-Modul des Panels nach ein bis
   zwei Stunden die Kopplung („Panel rejects this device: pair again“).
   **Vor jedem Neu-Koppeln alle alten Einträge am Panel löschen.**
2. **Das Panel nimmt nach einer getrennten Verbindung keine neue an** – weder vom ESP noch
   von der Truma-App –, bis das Panel stromlos gemacht wurde (Bluetooth-Fehler `0x3e`).
   Deshalb trennt der ESP ab 0.5.0 eine laufende Verbindung **nie** selbst. Bei Problemen
   startet er nur die Truma-Sitzung über die bestehende Funkverbindung neu, auch beim
   Button „Neu verbinden“.
3. **Jedes Flashen trennt die Verbindung.** Danach: Panel stromlos machen, Einträge am
   Panel löschen, „Kopplung löschen“, neu koppeln.
4. Das Panel koppelt immer **ohne Code** („Just Works“). Das Bluetooth-Modul ist ein eigenes
   Gerät auf dem Truma-Bus (Adresse 0x0601, `BleDeviceManagement`).
5. Solange der ESP verbunden ist, zeigt das Panel „Zur Kopplung weiterer Geräte bitte andere
   iNet X Apps schliessen“. Es lässt nur eine App-Verbindung gleichzeitig zu.
6. Mehrere Geräte melden dieselben Werte (z. B. `AirHeating.Temp`). Ohne `address:` springt
   der Wert hin und her – Sensoren deshalb immer an ein Gerät binden (Heizung 0x0201,
   Panel 0x0101, Elektroblock 0x0405).

### Kopplung Schritt für Schritt

1. Panel stromlos machen und wieder einschalten (nur nötig, wenn vorher eine Verbindung
   bestand oder geflasht wurde).
2. Am Panel unter *Verbundene Smartphones* **alle** Einträge des ESP löschen.
3. In HA „Kopplung löschen“ drücken.
4. Am Panel „Neues Gerät hinzufügen“ starten, direkt danach in HA „Panel koppeln“.
5. Kontrollieren: am Panel steht **genau ein** Eintrag mit Bluetooth-Symbol.

### Diagnose-Sensoren

- „Letzte Trennung“: Zeitpunkt, Grund (HCI-Code) und Dauer der letzten Verbindung
- „BLE freie Plätze“, „BLE-Modul Zustand“: Zustand des Bluetooth-Moduls im Panel
- „Firmware-Version“: Version dieser Komponente
- „Funkverbindung“: tatsächlich verwendetes Verbindungsintervall und Funk-Timeout

---

## Warum das stabiler sein sollte

Die HA-Integration funktioniert, aber die Verbindung führt über viele Ebenen, die sie
selbst nicht kontrolliert. Ihre eigene Doku beschreibt die Ursachen:

| Problem mit HA + Proxy | Mit dem dedizierten ESP |
|---|---|
| HA wählt **bei jedem Verbindungsaufbau neu**, ob es über den Proxy oder den lokalen Adapter geht. Die Kopplung (Bond) existiert aber nur auf *einem* davon. | Es gibt genau einen Central, und der hat den Bond. |
| Das Panel nutzt eine **ständig wechselnde Bluetooth-Adresse (RPA)**. Ob der Host die auflösen kann, hängt von Kernel und BlueZ ab (Regression ab Linux 6.19). | Der ESP speichert beim Koppeln den Identitätsschlüssel (IRK) des Panels und löst die Adresse selbst auf (AES, wie in der BT-Spezifikation). |
| Der Proxy teilt seine Verbindungs-Slots mit anderen BLE-Geräten (Batterie-BMS, Sensoren …). | Der ESP macht nichts anderes. |
| BlueZ-Schreibzugriffe ohne Timeout, hängende Disconnects, Reload-Probleme in HA. | Jede Stufe hat ihr eigenes Timeout, und der ESP verbindet sich selbstständig neu. |
| Stromsparen/Neustarts des HA-Hosts unterbrechen die Verbindung. | Der ESP hält die Verbindung auch, wenn HA neu startet. Die Werte sind sofort wieder da. |

Was bleibt, sind Eigenheiten des **Panels selbst**:
- Nach einer getrennten Verbindung nimmt das Panel keine neue an, bis es stromlos gemacht
  wurde (siehe „Wichtigste Erkenntnisse“).
- Nach einem Neustart ist der interne Bus ca. 90 s „taub“. Der ESP wiederholt automatisch.

---

## Hardware

- **Klassischer ESP32** (ESP32-WROOM / „ESP32 DevKit“, `board: esp32dev`)

> **Wichtig:** Mit dem **ESP32-C3** (und vermutlich S3/C6) klappen Koppeln und Datenverbindung,
> aber **keine Wiederverbindung** nach einem Abbruch. Der Bluetooth-Stack dieser Chips sucht
> gekoppelte Geräte über die Adressauflösung im Funkchip, und das findet das Panel nicht
> (gemessen am 14.11.2026). Der klassische ESP32 löst die Adresse in Software auf, genau wie
> der ESPHome-Bluetooth-Proxy, mit dem die Wiederverbindung laut Original-Projekt funktioniert.
- USB-Stromversorgung aus dem Bordnetz (12 V → 5 V USB-Adapter)
- **Platzierung:** möglichst nah am Panel (wenige Meter, Sichtlinie). Entfernung zeigt sich
  als Verbindungsabbrüche.


---

## Installation

1. Im ESPHome Builder ein neues Gerät anlegen und den Inhalt von
   [`truma-inetx.yaml`](truma-inetx.yaml) hineinkopieren. Sie ist auf Deutsch eingestellt;
   für Englisch oben bei `packages: language:` `lang/de.yaml` durch `lang/en.yaml` ersetzen.
   Die Sprachdatei stellt alle Entitätsnamen, die Gerätenamen und die Statustexte um ([`lang/`](lang)).
2. Die Secrets eintragen (Vorlage: [`secrets.example.yaml`](secrets.example.yaml)).
3. Flashen. Voraussetzung: ESPHome ≥ 2025.11 und ein **klassischer ESP32** (`board: esp32dev`).
   Die Komponente wird von ESPHome automatisch aus diesem Repository geladen
   (`external_components: source: github://Moormerlander/esphome-truma-inetx@main`).
4. Gerät in Home Assistant hinzufügen (wird automatisch gefunden).
5. Koppeln wie unter „Kopplung Schritt für Schritt“ beschrieben.

### Vorher: alte Verbindung aufräumen

Das Panel speichert nur ca. **4 Bluetooth-Geräte** und lehnt neue ab, wenn die Liste voll ist.

- HA-Integration „Truma iNet X (BLE)“ in Home Assistant **löschen**. Sonst konkurrieren
  zwei Centrals um das Panel.
- In der Truma-App bzw. am Panel die **gespeicherte Geräteliste leeren** (mindestens den
  alten HA-Eintrag entfernen).

---

## Koppeln

1. Panel im **normalen Bildschirm** lassen.
2. In Home Assistant den Button **„Panel koppeln“** drücken. Der ESP ist jetzt 120 s im
   Kopplungsmodus (siehe Sensor „Verbindungsstatus“).
3. **Jetzt** das Panel in den **Gerät-hinzufügen-Modus** setzen (Truma-App oder Panel-Menü).
4. Warten. Im Log erscheint `Paired with the Truma panel`, danach `Connected`.

Hinweise:
- Das Panel zeigt am Ende evtl. **„Etwas ist schiefgelaufen“**. Das bezieht sich nur auf
  den App-Handshake von Truma und kann ignoriert werden. Maßgeblich sind ein grüner Balken
  über dem Bluetooth-Symbol am Panel und der Sensor „Panel verbunden“.
- **Auf dem Campingplatz:** Im Kopplungsmodus verbindet sich der ESP nur mit einem Panel,
  das *ohne Namen* wirbt. Das tut nur ein Panel im Gerät-hinzufügen-Modus. Fremde Panels
  im Normalbetrieb senden ihren Namen und werden übersprungen. Ein Panel, das die
  Kopplung verweigert, wird für den Rest des Kopplungsfensters ignoriert.
- Nach dem Koppeln verbindet sich der ESP **nur noch mit diesem einen Panel**
  (Adressauflösung über den gespeicherten IRK).
- Status **„Panel rejects this device: pair again“**: Das Panel hat den ESP vergessen.
  Geräteliste am Panel leeren und neu koppeln.

---

## Konfiguration

```yaml
truma_inetx:
  id: truma_hub
  time_id: ha_time          # optional: Uhrzeit fürs Panel
  user_name: "Home Assistant"
  pairing_timeout: 120s
  log_parameters: false     # true = jede Wertänderung ins Log
  language: de              # en | de: Sprache von Status, „Letzte Trennung“ und Zeitschaltuhren
  supervision_timeout: 20s  # so lange überlebt die Verbindung ohne Lebenszeichen vom Panel (2–32 s)
  time_sync: true           # Panel-Uhr nach dem Verbinden und täglich aus time_id stellen
```

Der Bus hinter dem Panel ist generisch. Jeder Wert ist `topic` + `param` und kommt von einem
Gerät mit Busadresse (Panel 0x0101, Heizung 0x0201, Elektroblock 0x04xx, Gasflaschen 0x06xx …).
Ohne `address:` wird das Gerät genommen, das den Wert zuletzt gemeldet hat. Mit
`address: 0x0603` legst du es fest, z. B. bei zwei Gasflaschen.

**Welche Werte gibt es bei mir?** Button „Alle Parameter ins Log“ drücken. Dann steht
jeder Parameter jedes Geräts mit aktuellem Wert im Log. Alternativ `log_parameters: true`.

| Plattform | Optionen |
|---|---|
| `sensor` | `topic`, `param`, `address`. Zahlenwert roh, skalieren mit `filters: - multiply: 0.1`. Listen liefern ihre Länge. |
| `binary_sensor` | `topic`/`param`/`address` und optional `on_values: [1]` (Standard: alles ≠ 0), **oder** `type: connected \| paired \| pairing_mode` |
| `text_sensor` | `topic`/`param`/`address` mit optional `format: timer` (Zeitschaltuhr lesbar) **oder** `format: firmware` + `address` (Firmware-Version eines Geräts), **oder** `type: status \| version \| last_disconnect \| link_params` |
| `number` | `topic`, `param`, `address`, `min_value`, `max_value`, `step`, `scale` (Anzeige = Wert × scale; Temperaturen: `0.1`) |
| `select` | `topic`, `param`, `address`, `options: {0: "Eco", 1: "Comfort"}` |
| `switch` | `topic`, `param`, `address`, `on_value` (1), `off_value` (0) |
| `button` | `type: pair \| unpair \| reconnect \| dump \| measure \| sync_time \| write` (bei `write`: `topic`, `param`, `value`, `address`) |
| `climate` | `supported_modes: [HEAT, FAN_ONLY, COOL, AUTO, DRY]`, `appliance_address: 0x0201` (Heizung, deren Werte angezeigt werden) |

Die Thermostat-Entität funktioniert so:
- Der Modus ist `RoomClimate.Mode` am Panel.
- Die Solltemperatur liegt je nach Modus in `AirHeating.TgtTemp`, `AirCooling.TgtTemp`
  (Klimaanlage) oder `RoomClimate.TgtTemp` (Auto).
- Die Isttemperatur ist `AirHeating.Temp`.

Schreibzugriffe gehen an das Gerät, das den Wert meldet. Nur `RoomClimate` geht ans Panel.

### Eigene Aktionen in Home Assistant

```yaml
api:
  actions:
    - action: truma_write
      variables:
        topic: string
        param: string
        value: int
      then:
        - lambda: 'id(truma_hub).write_param(topic, param, value);'
```

---

## Wie es intern funktioniert

1. **Finden:** Das Panel wirbt mit der Service-UUID `fc310002-…`. Seine Adresse wechselt
   ständig. Sie wird gegen den beim Koppeln gespeicherten IRK geprüft.
2. **Verbinden & verschlüsseln:** Erst Verschlüsselung (Bond), dann Notifications
   auf CMD und DATA_R. Upstream hat gezeigt: andersherum bricht das Panel ab.
3. **Session:**
   - Registrierung (Panel vergibt eine Busadresse)
   - Topics abonnieren
   - Identität und Uhrzeit senden
   - Parameter-Abfrage an jedes Busgerät (nur Panel und Heizung antworten auf Broadcast)
   - Tankmessung anstoßen
4. **Halten:** Werte kommen als Push. Tanks werden jede Minute neu gemessen.
5. **Überwachung:** Kommen 90 s lang keine Daten, wird die Sitzung über die bestehende
   Funkverbindung neu gestartet. Die Funkverbindung selbst trennt der ESP nie.

Transport: Jede Nachricht läuft über `InitDataTransfer → Ready → Daten → DataAck`.
Bleibt ein Ack aus, wird die Sitzung neu gestartet. Das Ack trägt keine Kennung,
deshalb wäre die Zuordnung danach nicht mehr sicher (upstream gemessen).

---

## Tests

```bash
cd tests
g++ -std=c++17 -I../components/truma_inetx test_core.cpp ../components/truma_inetx/truma_core.cpp -lcrypto -o test_core
./test_core        # -v für ausführliches Log
```

Was die Tests abdecken:
- CBOR (RFC-8949-Testvektoren)
- Frame-Aufbau (byte-identisch zur Python-Referenz)
- RPA-Auflösung (Testvektor aus der Bluetooth-Spezifikation)
- eine komplette Session gegen ein simuliertes Panel: fragmentierte Antworten bei MTU 23,
  Geräte ohne Ack, Schreiben mit Echo, abgelehnte Writes, Stall-Watchdog, verirrte
  Ready/Ack-Pakete

---

## Bei Problemen

- Log-Level `DEBUG` (oder `logs: { truma_inetx: VERBOSE }`). Der Sensor „Verbindungsstatus“
  zeigt die aktuelle Stufe.
- „Letzte Trennung“ zeigt einen Funk-Timeout (0x08): Der ESP32 teilt sich ein Funkteil
  zwischen WLAN und Bluetooth. Im Router einen **festen WLAN-Kanal** einstellen (automatische
  Kanalwechsel lassen den ESP neu suchen) oder ein ESP32-Board mit LAN verwenden
  (z. B. WT32-ETH01, Olimex ESP32-POE), dann hat Bluetooth das Funkteil für sich allein.
- Dauerhaft Status 0x3E / `connect failed` nach einer Trennung oder nach dem Flashen:
  Panel stromlos machen, dann neu koppeln (siehe „Kopplung Schritt für Schritt“).
- `the panel assigned no address` direkt nach Panel-Neustart: normal, verschwindet nach ca. 1–2 Minuten.

---

## Lizenz & Dank

- Protokollwissen und Messungen: [rpodgorny/hass-truma-inetx](https://github.com/rpodgorny/hass-truma-inetx) (GPL-3.0)
- Ursprüngliches Reverse Engineering: [daaaaan/truma-inetx-ble](https://github.com/daaaaan/truma-inetx-ble)

Der C++-Code ist eine Neuimplementierung, es wurde kein Code aus dem unlizenzierten
Upstream-Protokollmodul übernommen. Weil Logik und Messwissen aus dem GPL-Projekt stammen,
sollte dieses Projekt ebenfalls unter **GPL-3.0** veröffentlicht werden.

„Truma“ und „iNet X“ sind Marken der Truma Gerätetechnik GmbH & Co. KG. Dieses Projekt ist
nicht mit Truma verbunden.
