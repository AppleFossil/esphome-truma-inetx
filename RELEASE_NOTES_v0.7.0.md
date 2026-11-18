# v0.7.0 – first public beta / erste öffentliche Beta

🇬🇧 **Connect the Truma iNet X panel directly to Home Assistant with an ESP32 – no Bluetooth proxy, no HA integration.**
The ESP pairs with the panel itself and keeps the Bluetooth connection open; Home Assistant talks to the ESP over WiFi.

🇩🇪 **Das Truma iNet X Panel direkt per ESP32 mit Home Assistant verbinden – ohne Bluetooth-Proxy, ohne HA-Integration.**
Der ESP koppelt sich selbst mit dem Panel und hält die Bluetooth-Verbindung; Home Assistant spricht per WLAN mit dem ESP.

## Highlights
- Stable connection: long-term tests of 30+ hours on a classic ESP32 / stabile Verbindung, Langzeittests über 30 Stunden
- Heater, hot water, batteries, shore power, tanks, water pump, panel display and clock / Heizung, Warmwasser, Batterien, Landstrom, Tanks, Wasserpumpe, Display und Uhr des Panels
- Panel clock synced from Home Assistant / Panel-Uhr wird aus Home Assistant gestellt
- Entities grouped into devices: Heater, Power, Panel / Entitäten nach Geräten gruppiert
- English and German / Englisch und Deutsch

## Getting started / Loslegen
See the **Quick start** in the [README](README.md) / siehe **Schnellstart** in der [README (Deutsch)](README.de.md).
Requires a **classic ESP32** and ESPHome ≥ 2025.11 / benötigt einen **klassischen ESP32** und ESPHome ≥ 2025.11.

## Known issues / Bekannte Punkte
- Room and water temperature alternate between two values about 0.4–0.5 °C apart / Raum- und Wassertemperatur springen zwischen zwei Werten (0,4–0,5 °C)
- Some status values are not decoded yet / einige Statuswerte sind noch nicht entschlüsselt
- ESP32-C3/S3/C6 are not supported / ESP32-C3/S3/C6 werden nicht unterstützt

## Testers wanted / Tester gesucht
Please open an [issue](https://github.com/Moormerlander/esphome-truma-inetx/issues) with your panel type and software, heater/AC, ESP32 board, how long the connection holds ("Last Disconnect") and the ESPHome log if something fails.
Bitte ein [Issue](https://github.com/Moormerlander/esphome-truma-inetx/issues) anlegen mit Panel-Typ und -Software, Heizung/Klimaanlage, ESP32-Board, wie lange die Verbindung hält („Letzte Trennung“) und bei Problemen dem ESPHome-Log.

Full list of changes / alle Änderungen: [CHANGELOG.md](CHANGELOG.md)
