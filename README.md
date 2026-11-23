# portal-bridge

Firmware for the **Waveshare ESP32-S3-A7670E-4G** board. It is a battery-powered
**4G hotspot** that can also take its internet from a **Wi-Fi network**, including
Wi-Fi behind a login page that asks for a phone number and an SMS code. It logs in
with the SIM in its own modem, and **every SMS the SIM receives is saved and
forwarded** to you.

Everything is set from the admin page: join the board's hotspot and open
`http://192.168.0.1`. Nothing about a venue, carrier or person is built into the
firmware.

Builds run on GitHub Actions (ESP-IDF 5.5); nothing is installed on a PC.

## What it does

| Area | Behaviour |
|---|---|
| Hotspot | WPA2 hotspot, up to 10 devices, admin app at its gateway address |
| Uplinks | 4G (PPP over the modem's USB link) and up to three upstream Wi-Fi networks; modes `wifi_then_4g` (default), `4g_then_wifi`, `wifi`, `4g`; NAT to the hotspot |
| Internet checks | Every 30 s (configurable) per uplink; a redirect or HTML reply means a login page |
| Automatic login | Replays a saved **recipe**, or finds the phone and code fields by itself; waits for the code SMS; checks internet afterwards; retries on a back-off |
| Assisted login | Login page opened **through the board**; type `<phone>` or `<code>` and the board fills in the SIM's number or the SMS code; everything recorded; one tap turns a recording into a recipe |
| SMS | Read from the SIM every 15 s and on arrival, multipart messages joined, saved to flash **before** removal from the SIM |
| Forwarding | Telegram bot, HTTPS webhook (JSON) and MQTT over TLS, each with its own delivery pointer and retry, so nothing is skipped across outages or restarts; device events (login results, uplink changes) too |
| Diagnostics | Device log, modem console, crash summary, recordings; **one-tap report to a GitHub issue** (or Telegram / download) with numbers masked and no SMS text |
| Maintenance | Firmware update from the admin page with automatic rollback, settings backup/restore |

## Board switches (back side)

Waveshare documents the DIP block as power switches for the **camera**, the **USB hub** and
the **4G module**, plus a **USB route** switch for the modem: *4G USB ↔ Type-C* or
*4G USB ↔ ESP32-S3*. The ESP32's own USB-to-UART chip (used for flashing and the serial
log) sits behind the board's USB hub (CH334) on the Type-C port, so it should be reachable in
both route positions (read from the schematic; the first flash confirms it).

| Switch | Set to | Why |
|---|---|---|
| 4G | **ON** | powers the modem (SMS, 4G data) |
| USB hub | **ON** | the Type-C port reaches the USB-to-UART chip through it (flashing, serial log) |
| Camera | OFF | not used by this firmware; saves power |
| USB route (labelled only ON/OFF) | **OFF** = modem USB ↔ ESP32 | the firmware takes 4G data over this link; in the ON position (modem USB ↔ Type-C) a PC gets the modem instead and the board has no 4G data |

Insert the SIM and attach the LTE antenna.

## First install (USB cable, once)

1. Download `portal-bridge-<version>-full.bin` from the
   [latest release](../../releases/latest).
2. Set the switches as in the table above and connect USB-C to a computer.
3. Open <https://espressif.github.io/esptool-js/> in Chrome or Edge → **Connect** →
   choose the port → address **0x0** → the `-full.bin` file → **Program**.
   If it will not connect: hold **BOOT**, tap **RESET**, release **BOOT**, retry.
4. Press **RESET**.

Later versions: admin app → **Tools → Firmware update** → the `-ota.bin` file.

## First setup

1. Join Wi-Fi **PortalBridge-XXXX**, password `portalbridge`.
2. Open `http://192.168.0.1`, log in as `admin` / `portalbridge`.
3. Set your own admin password, hotspot name and hotspot password. The board
   restarts; reconnect with the new values.
4. **Settings**:
   - *Phone number*: country code `852`, your local number (if the SIM does not
     store it), and the format the login page expects.
   - *Upstream Wi-Fi*: the venue network name (use **Scan**).
   - *Forwarding*: Telegram bot token from @BotFather, send your bot a message,
     **Find my Telegram chat id**, save, **Test Telegram**.
   - *Reports*: a GitHub fine-grained token with *Issues: read and write* on this
     repository, so **Tools → Send to GitHub issue** works.

## LED

blue starting · yellow setup pending · green online · cyan blinking: login page
detected · red no internet

## Repository layout

```
firmware/main/        ESP-IDF application (C) and the admin app (www/index.html)
firmware/core/        pure-C logic shared with host tests: SMS PDU decoder, HTML form
                      scanner, URL resolver, phone/code helpers, redaction
tests/                host unit tests for firmware/core
docs/                 design and hardware notes
```
