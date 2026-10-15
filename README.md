# LogiBattery

A small native C++ battery monitor for Logitech devices on **Windows 10/11 x64**.
Run `LogiBattery.exe`, hover over its tray icon for battery status, or click it
for devices and settings. The icon may initially appear under hidden icons.

## Features

- One unified tray icon, with optional individual device icons.
- Battery level, charging status and low-battery notifications.
- Offline devices hidden by default.
- Adjustable polling and optional launch at sign-in.
- Direct HID++ queries; no LGS or G Hub integration required.

| Tested device | Connection | Battery readout |
| --- | --- | --- |
| Logitech G700s | C531 wireless receiver | Full / Good / Low / Critical |
| Logitech ERGO M575 | Unifying receiver | Percentage |
<img width="866" height="389" alt="logibattery" src="https://github.com/user-attachments/assets/4c74a117-97ab-4aaa-9d23-a0bfc6d41033" />

Other HID++ devices may work but are unverified. The app shows reported levels
or voltage when no percentage is available. Bluetooth and wired G700s are
unverified; Centurion devices are unsupported.

Readings refresh every 60 seconds by default. Wake a sleeping device and select
**Refresh now** to check immediately. **Quit LogiBattery** closes all its icons.

## Credits and license

Protocol work draws on Solaar, LGSTrayBattery, the Linux HID++ driver. 

Licensed under [GPL-3.0-or-later](LICENSE.txt).
