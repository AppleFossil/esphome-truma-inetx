# Changelog

## 0.6.1
- New option `language: en | de` for the texts the ESP produces (connection status,
  "Last Disconnect", timer schedules). The German example YAML sets `de`, the English one `en`.

## 0.6.0
- Pairing code removed (the panel always pairs without a code).
- Sensors bound to fixed device addresses; room and water temperature no longer flip.
- `climate`: new option `appliance_address`.
- `text_sensor`: `format: timer` (readable timers) and `format: firmware`.
- Example YAML with all known parameters: 12 V main switch, air circulation, panel
  temperature, target temperatures, gas level, battery and tank state, display brightness
  and timeout, screensaver, firmware versions, panel clock, 6 timers.
- Example YAML in English (`truma-inetx.en.yaml`) and German (`truma-inetx.de.yaml`).
- Electric heating element removed from the example (Combi E only).

## 0.5.0
- The ESP never closes a working connection itself; if something goes wrong only the Truma
  session is restarted (the panel does not accept a new connection after a disconnect).
- New sensor "Last Disconnect" (time, reason, duration).

## 0.4.0
- Version number as a sensor, in the log and in the device info.
- Experiment with a pairing code (passkey).
