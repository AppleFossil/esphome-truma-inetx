# Changelog

## Unreleased (example YAML only)
- Entities grouped into ESPHome sub-devices: "Truma Heater" (Combi: heating and hot water),
  "Truma Power" (EBL), "Truma Panel". Connection and diagnostics stay on the ESP itself.
- One example YAML (`truma-inetx.yaml`) instead of two: the language is chosen with one line,
  which loads `lang/de.yaml` or `lang/en.yaml` (entity names, device names, status texts).

## 0.6.5
- Panel clock: the panel ignores writes to `TimeAndDate.SystemTime`. The ESP now writes the
  time as text (`TimeAndDate.Time` "HH:MM", `TimeAndDate.Date` "DD.MM.YY"), checks the panel's
  next minute report and stops the daily attempts if the panel does not take it.

## 0.6.4
- Panel clock: the ESP writes the Home Assistant time 15 s after connecting and then once a day
  (option `time_sync`, default on) and checks the time the panel displays.
- New button `type: sync_time` ("Set Panel Clock").
- Subscribes to `TimeAndDate`, so "Panel Time" updates live.

## 0.6.3
- The panel rejects connection intervals of 30-50 ms ("unacceptable connection interval").
  The ESP now keeps ESPHome's interval (8.75-11.25 ms) and only raises the supervision timeout.
- Timer entities removed from the example YAML (`format: timer` still works if you add them).
- New diagnostic sensor `type: link_params` ("Radio Link"): interval, latency and supervision
  timeout actually in use, read back from the Bluetooth stack.

## 0.6.2
- After connecting, the ESP asks for a supervision timeout of 20 s (new option
  `supervision_timeout`). Previously the link dropped after 8 s without a packet.

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
