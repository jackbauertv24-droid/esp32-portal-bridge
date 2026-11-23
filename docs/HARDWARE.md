# Hardware notes: Waveshare ESP32-S3-A7670E-4G

Values used by the firmware, where each came from, and how sure we are.
"Verified" means confirmed on a real board by this project; nothing is verified
until the first diagnostics report comes back.

| Signal | Value | Source | Confidence |
|---|---|---|---|
| SoC | ESP32-S3R8 (8 MB octal PSRAM in package) | Waveshare V2 schematic | high |
| Flash | 16 MB (W25Q128JVSIQ) | schematic | high |
| Modem UART (ESP32 side) | RX GPIO17, TX GPIO18, 115200 baud | every Waveshare demo (Arduino + ESP-IDF) | high |
| Modem enable | GPIO21 held HIGH | Waveshare GNSS demo sets it HIGH before AT traffic | medium |
| PWRKEY | not driven | community reports disagree (GPIO42 vs not wired); a speculative pulse can switch a running modem **off**, so the firmware never pulses it | — |
| RGB LED | WS2812B on GPIO38 | Waveshare RGB demo; schematic net `RGB_CTRL` | high |
| Battery gauge | MAX17048 at I2C 0x36 | schematic + Waveshare demo | high |
| Gauge I2C pins | SDA 15 / SCL 16 | Waveshare demo; one community repo says 3 / 2 | medium — both probed at boot, report says which answered |
| USB routing | DIP switch on the back: USB-C to the USB-UART bridge (CH343) or to the modem | Waveshare docs | high |
| Power DIP switches | camera, USB hub, 4G module | Waveshare docs | high |

## Why GPIO33 is never touched

One community project drove GPIO33 as a "modem enable". On the ESP32-S3R8 the
in-package octal PSRAM uses GPIO33–37, so driving GPIO33 with PSRAM enabled
risks corrupting memory. Waveshare's own demos do not use it.

## Open questions the first report answers

- Does the modem answer with only GPIO21 high and the DIP switch on, and how long
  does it take after power-up?
- Modem firmware revision (`AT+CGMR`), SMS service support (`AT+CSMS?`) and
  storage (`AT+CPMS?`) on a Hong Kong network, where SMS may only be available over LTE.
- Which I2C pin pair the battery gauge answers on.
