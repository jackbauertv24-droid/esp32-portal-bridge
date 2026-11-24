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
| Wi-Fi + 4G together | Optional load balancing when both uplinks are online: each device+site pair is pinned to one link, the Wi-Fi share is adjustable (10-90 %); helps with several sites or devices, not one single download; uses 4G data |
| Data & budget | 4G and Wi-Fi usage per day and billing period (31-day chart, 6-period history), cost estimate from price per GB, monthly/daily limits or a monthly budget, warning alerts via the forwarding channels, optional stop (hotspot devices off 4G, board keeps 4G for its alerts), override until midnight |
| Connected devices | Who is on the hotspot: name (from the device's DHCP request, or your own), address, signal, live speed, data this visit and all time; block a device's internet, disconnect it, optional alert when a new device joins (name sent only to your channels, never into reports) |
| Wi-Fi survey | Full scan of nearby networks: signal (with min/max over repeated scans), channel and 40 MHz width, security, 802.11 modes, WPS, country; channel crowding chart with the best of 1/6/11; filter for open networks; one tap to add a network as an upstream Wi-Fi |
| Speed test | Latency, jitter, download and upload measured by the board through Wi-Fi, 4G or both in turn (live speed while running); sizes and time limit configurable, smaller defaults for 4G; last 100 results kept with network and signal, chart over time |
| GPS + signal tracker | Position (modem GNSS) and 4G radio (RSRP/RSRQ/SINR, band, cell) logged at an interval to daily files; map with dots coloured by signal (plain plot without internet), signal chart, CSV/GPX export, auto clean-up by age and space, clock set from GPS when offline; reply with a map link to a keyword SMS from allowed numbers only; positions never go into reports |
| Internet checks | Every 30 s (configurable) per uplink; a redirect or HTML reply means a login page |
| Automatic login | Replays a saved **recipe**, or finds the phone and code fields by itself; waits for the code SMS; checks internet afterwards; retries on a back-off |
| Assisted login | Login page opened **through the board**; type `<phone>` or `<code>` and the board fills in the SIM's number or the SMS code; everything recorded; one tap turns a recording into a recipe |
| SMS | Read from the SIM every 15 s and on arrival, multipart messages joined, saved to flash **before** removal from the SIM |
| SMS manager | Conversations by number, search, filters (unread, login codes, sent, failed), compose and reply (any language, long messages split automatically), forward, copy code, multi-delete, export, unread badge |
| Forwarding | Telegram bot, HTTPS webhook (JSON) and MQTT over TLS, each with its own delivery pointer and retry, so nothing is skipped across outages or restarts; device events (login results, uplink changes) too |
| Diagnostics | Device log, modem console, crash summary, recordings; **one-tap report to a GitHub issue** (or Telegram / download) with numbers masked and no SMS text |
| Maintenance | Firmware update from the admin page with automatic rollback, settings backup/restore |
| Debug page | Hardware tests: camera photo, TF card read/write, PSRAM, chip temperature, LED, BOOT button, speaker (modem text-to-speech), microphone record + playback, test voice call, GNSS fix, modem/SIM/4G/battery checks; results land in the device log for the report |

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
