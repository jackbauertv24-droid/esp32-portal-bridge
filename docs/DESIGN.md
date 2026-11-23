# Design: 4G hotspot with captive-portal Wi-Fi sharing

Status: **proposed**, awaiting confirmation before the firmware is rebuilt around it.

## Positioning

A battery-powered **4G hotspot** that can also use a **Wi-Fi network as its uplink**,
including Wi-Fi behind an SMS-verified captive portal, which it logs into with the
SIM in its own modem. Every SMS reaching the SIM is kept and forwarded.

## Principles

1. **The admin page is the single source of truth.** Every behaviour is a setting
   stored on the board and editable at `http://<admin address>/`. Nothing about a
   venue, a country, a carrier or a person is compiled in; the public repository
   holds only generic defaults.
2. **Settings are one schema.** Each setting is declared once (key, type, default,
   validation, help text). The admin forms, the JSON export/import and the
   diagnostics report are all generated from that table, so a new setting cannot
   be half-wired.
3. **Record first, automate second.** The board records what it sees (portal pages,
   requests, SMS arrival, uplink changes) so failures can be diagnosed offline and
   automation can be learned from a real session instead of guessed.
4. **Everything degrades to a manual path.** If automation fails, the admin page
   offers the same operation by hand.

## Uplinks

| Mode (setting `uplink.mode`) | Behaviour |
|---|---|
| `4g` | Mobile data only |
| `wifi` | Upstream Wi-Fi only |
| `wifi_then_4g` | Wi-Fi when it has internet, 4G otherwise |
| `4g_then_wifi` | 4G when registered, Wi-Fi otherwise |

Internet reachability is judged by configurable probe URLs (`net.probe_urls`,
expected status 204/200) every `net.probe_interval_s`. A captive portal is
recognised when a probe is redirected or answered with HTML.

**4G data goes over the modem's USB link, not the UART.** The UART tops out near
0.1–0.9 Mbit/s; LTE Cat-1 offers up to ~10 Mbit/s down. Espressif's
`iot_usbh_modem` component lists the **A7670E-FASE (firmware A7670M7_V1.11.1)** as
tested on ESP32-S3 for USB PPP, with a second USB AT port for SMS and status while
data is up. This needs the board's USB DIP switch set to *modem ↔ ESP32*, so in
normal use firmware updates go through the admin page (OTA), and USB flashing needs
the switch flipped back.

## Settings (initial list, all editable)

- **Hotspot:** SSID, password, channel, hidden, max clients, admin address and subnet
  (auto-moved if it clashes with the upstream network).
- **Admin:** password, session timeout.
- **Phone identity:** country code (`+852` default for the form; editable), local
  number, and the **format** submitted to forms (`local`, `+CC local`, `CC local`,
  `00CC local`), since portals differ.
- **4G:** APN, user, password, PIN, roaming allowed, monthly data cap and warning.
- **Upstream Wi-Fi:** list of known networks (SSID, password, priority), optional
  MAC address override for the uplink interface.
- **Portal:** probe URLs, detection interval, per-network login recipe, OTP rule
  (sender filter + regex with one capture group, default 4–8 digits), OTP wait
  timeout, retry/back-off, re-login schedule.
- **SMS forwarding:** targets (Telegram bot token + chat id; HTTPS webhook URL +
  secret header; MQTT over TLS broker, user, password, topic), which messages
  (all / exclude portal OTPs), retry policy.
- **Recording:** on/off per area, size limits, auto-redaction on export.
- **Device:** LED brightness, log level, time zone, NTP server.
- **Backup:** export/import all settings as JSON (secrets optional).

## Portal handling: assisted login with placeholders, then replay

**Assisted (mirror) mode.** The board fetches the real portal through its own
Wi-Fi uplink and serves a rewritten copy to a phone on its hotspot at
`/portal/…`. The person fills the form as usual but may type the placeholders
`<phone>` and `<code>`:

- `<phone>` is replaced with the configured number in the configured format;
- `<code>` makes the board wait for the next matching SMS received *after* the
  phone number was submitted, and substitute its code.

All requests are made by the board, so the portal authorises **the board**, and
every device behind the hotspot gets through. A small injected script routes
`fetch`, `XMLHttpRequest` and `form.submit()` through the mirror, so pages that
submit with JavaScript are captured too.

**Recording.** Each exchange is stored: method, URL, request headers and form
fields (placeholders kept as placeholders), response status, headers and body up
to a size cap. Recordings can be viewed on the admin page and exported redacted.

**Recipe.** A successful recording is converted into a recipe: ordered steps,
with fields marked `{phone}`, `{code}` or `{extract:<regex or form field from the
previous response>}`. The recipe is editable on the admin page. When the probe
detects the portal again, the board replays the recipe on its own; if a step
fails it records the failure, notifies through the SMS-forwarding channel and
falls back to assisted mode.

**Limits.** CAPTCHAs and third-party logins (Google/Facebook) cannot be automated.
Values computed by page JavaScript are captured in assisted mode but may need a
recipe edit to replay.

## Platform

ESP-IDF (instead of Arduino) because USB-host PPP for the modem is provided by
Espressif's `iot_usbh_modem` / esp-modem components there. Pure-C parts
(`sms_pdu`, `redact`) and their tests carry over unchanged.

## Milestones

1. **Platform**: ESP-IDF skeleton, schema-driven settings + admin page, logs,
   OTA, LED, modem AT over USB, diagnostics report.
2. **4G hotspot**: USB PPP, NAT to the hotspot, APN settings, data counter.
3. **SMS**: durable store, multipart join, forwarding with retry.
4. **Wi-Fi uplink**: known networks, uplink modes and failover, portal detection.
5. **Assisted portal login**: mirror, placeholders, recorder.
6. **Automatic login**: recipes generated from recordings, replay, notifications.
