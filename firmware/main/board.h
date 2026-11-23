// Pin assignments for the Waveshare ESP32-S3-A7670E-4G. Sources and confidence
// for every value are in docs/HARDWARE.md.
#pragma once

#define MODEM_UART_RX 17  // ESP32 receives from the modem
#define MODEM_UART_TX 18  // ESP32 transmits to the modem
#define MODEM_BAUD 115200
#define MODEM_EN_PIN 21   // held high (Waveshare GNSS demo); never pulsed

#define RGB_LED_PIN 38    // WS2812B

#define I2C_SDA_A 15      // MAX17048 battery gauge, Waveshare demo pins
#define I2C_SCL_A 16
#define I2C_SDA_B 3       // alternative reported by a community project
#define I2C_SCL_B 2
#define MAX17048_ADDR 0x36
