# Changelog

## 0.6.0
- Kopplungscode entfernt (das Panel koppelt immer ohne Code).
- Sensoren an feste Geraeteadressen gebunden; Raum- und Wassertemperatur springen nicht mehr.
- `climate`: neue Option `appliance_address`.
- `text_sensor`: `format: timer` (Zeitschaltuhren lesbar) und `format: firmware`.
- Beispiel-YAML mit allen bekannten Parametern: 12-V-Hauptschalter, Umluft, Panel-Temperatur,
  Solltemperaturen, Gas-Stufe, Batterie- und Tankstatus, Display-Helligkeit/-Timeout,
  Bildschirmschoner, Firmware-Versionen, Panel-Uhr, 6 Zeitschaltuhren.
- Elektro-Heizstab aus dem Beispiel entfernt (nur fuer Combi E).

## 0.5.0
- Der ESP trennt eine laufende Verbindung nie mehr selbst; bei Problemen wird nur die
  Truma-Sitzung neu gestartet (das Panel nimmt nach einer Trennung keine neue Verbindung an).
- Neuer Sensor "Letzte Trennung" (Zeitpunkt, Grund, Dauer).

## 0.4.0
- Versionsnummer als Sensor, im Log und in der Geraeteinfo.
- Versuch mit Kopplungscode (Passkey).
