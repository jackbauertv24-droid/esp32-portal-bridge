// Pin assignments for the Waveshare ESP32-S3-A7670E-4G.
// Sources and confidence for each value are recorded in docs/HARDWARE.md.
#pragma once

// Modem UART, from the ESP32's point of view (Waveshare demos: RX 17, TX 18, 115200).
#define MODEM_UART_RX 17
#define MODEM_UART_TX 18
#define MODEM_BAUD 115200

// Driven HIGH by Waveshare's own GNSS demo before talking to the modem.
// Held high here; never pulsed.
#define MODEM_EN_PIN 21

// WS2812B RGB LED (Waveshare RGB demo).
#define RGB_LED_PIN 38

// MAX17048 battery gauge. Waveshare demos use SDA 15 / SCL 16; one community
// project reports 3 / 2, so both pairs are probed at boot and the one that
// answers is reported.
#define I2C_SDA_A 15
#define I2C_SCL_A 16
#define I2C_SDA_B 3
#define I2C_SCL_B 2
#define MAX17048_ADDR 0x36
