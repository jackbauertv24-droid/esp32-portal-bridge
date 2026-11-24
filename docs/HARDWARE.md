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
| 4G data | USB host PPP, VID 1E0E PID 9011, modem interface 5 | Espressif iot_usbh_modem supported-module table (A7670E-FASE, A7670M7_V1.11.1, ESP32-S3) | high for the module; untested on this board |
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

## USB route DIP switch (position 4, printed only ON/OFF)

Traced in the V1 schematic: DIP position 4 drives net `USB_SET`, which goes to `SEL`
(pin 10) of the FSUSB42 USB switch (U12) and has a 47 kΩ pull-down (R65). The switch's
common side is the 3.3 V rail. FSUSB42 pins: 1/2 `D+/D-` = modem USB, 4/5 `HSD1` = ESP32
GPIO19/20 (via R56/R57), 6/7 `HSD2` = USB hub (CH334) → Type-C.

- **OFF** → `SEL` low → `HSD1`: modem USB ↔ **ESP32** (what this firmware needs for 4G data)
- **ON** → `SEL` high → `HSD2`: modem USB ↔ **Type-C** (a PC uses the board as a modem)

Confidence: high from the schematic and the FSUSB42 truth table; not yet confirmed on a board.

## Camera, TF card, audio (used by the Debug page)

| Signal | Value | Source |
|---|---|---|
| Camera | XCLK 39, SCCB SDA 15 / SCL 16 (shared with the battery gauge), D0-D7 = GPIO 7-14, VSYNC 42, HREF 41, PCLK 46, no PWDN/RESET | Waveshare demo `CAMERA_MODEL_WAVESHARE_7670_BOARD` |
| TF card | SDMMC 1-bit: CLK 5, CMD 4, D0 6 | Waveshare SD demo |
| Speaker / microphone | wired to the A7670E (MIC_P/N, REC_SPK_P/N), not the ESP32; driven with AT+CTTS, AT+CREC, AT+CCMXPLAY, AT+CLVL | V1 schematic nets; SIMCom A7600 audio application note |

GPIO42 is a camera data line, which also rules out the community claim that it is the modem PWRKEY.
