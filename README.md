# portal-bridge

Firmware for the **Waveshare ESP32-S3-A7670E-4G** board that turns it into a
self-contained Wi-Fi bridge for networks protected by an SMS-verified captive
portal:

- it joins the venue Wi-Fi and re-shares it as its own hotspot;
- when the portal asks for a phone number, it submits the number of the SIM in
  the board's 4G modem, reads the SMS code that arrives and completes the login;
- every SMS that reaches the SIM is forwarded to a private channel, so none is
  missed;
- it is managed from a browser by joining its hotspot and opening `http://192.168.0.1`.

Everything is built by GitHub Actions; there is nothing to install on a PC.

## Status

| Milestone | What it adds | State |
|---|---|---|
| **1. Foundation** | Hotspot + admin page, modem/SIM diagnostics, read-only SMS inbox, diagnostics report, firmware upload from the browser | **current** |
| 2. SMS forwarding | Store every SMS in flash before removing it from the SIM, rejoin multipart messages, forward to Telegram / HTTPS webhook / MQTT over TLS with retry | planned |
| 3. Wi-Fi re-sharing | Join an upstream Wi-Fi, NAT it to the hotspot, avoid address clashes with the upstream network | planned |
| 4. Portal auto-login | Detect the portal, fill the phone form, wait for the SMS code, submit it; record every portal exchange for offline debugging; manual fallback | planned |
| 5. 4G data | Optional uplink over the SIM's mobile data (choice on the admin page) | planned |

## Flashing (first time, USB cable)

1. Download `portal-bridge-<version>-full.bin` from the
   [latest release](../../releases/latest).
2. On the back of the board, set the USB DIP switch so the USB-C port goes to the
   ESP32 (USB-to-UART), and switch the **4G** DIP switch **on**.
3. Plug the board into a computer and open <https://espressif.github.io/esptool-js/>
   in Chrome or Edge.
4. **Connect** → choose the board's serial port → set the flash address to **0x0** →
   pick the `-full.bin` file → **Program**.
   If it will not connect: hold **BOOT**, tap **RESET**, release **BOOT**, try again.
5. Press **RESET** on the board when it finishes.

Later updates need no cable: admin page → **Firmware** → upload the `-ota.bin` file.

## First setup

1. Join the Wi-Fi **PortalBridge-XXXX** with the password `portalbridge`.
2. Open `http://192.168.0.1`, log in as `admin` / `portalbridge`.
3. You land on **First-time setup**: choose your own admin password, hotspot name and
   hotspot password. The board restarts; reconnect with the new values.

The LED shows the state: blue booting, yellow setup pending, red modem not answering,
green modem up.

## Reporting a problem

Admin page → **Download report** gives a text file with the board, modem and SIM
status and the device log. Phone numbers, IMEI, ICCID and IMSI are masked and SMS
contents are left out, so it can be attached to a GitHub issue in this repository.

## Layout

```
firmware/portal_bridge/   Arduino sketch (ESP32 Arduino core 3.3.x)
tests/                    host-side unit tests for the pure-C parts
docs/HARDWARE.md          pin map with sources and confidence
.github/workflows/        CI: tests, firmware build, releases on v* tags
```
