// A7670E driver: AT commands over UART, a background task that brings the
// modem up and keeps a status snapshot fresh, and read-only SMS listing.
#pragma once
#include <Arduino.h>

struct ModemStatus {
  bool alive = false;            // answered "AT"
  uint32_t aliveSinceMs = 0;
  uint32_t lastRefreshMs = 0;
  uint32_t bootWaitMs = 0;       // how long the modem took to first answer
  String model, revision, imei, iccid, imsi, ownNumber;
  String simState;               // READY, SIM PIN, NOT INSERTED, ...
  String operatorName;
  int csq = 99;                  // 0..31, 99 = unknown
  int rssiDbm = 0;               // derived from csq, 0 = unknown
  int regCs = -1, regEps = -1;   // +CREG / +CEREG <stat>
  String cpsi;                   // raw +CPSI: line (radio, band, cell)
  String smsStorage;             // raw +CPMS? reply
  uint32_t urcCount = 0;
  uint32_t smsNotifications = 0; // +CMTI seen since boot
};

// Copy of the latest snapshot (taken under the modem lock).
ModemStatus modem_status();

void modem_start_task();

// Sends one AT command and returns every response line, joined with "\n".
// `ok` is set when the final result was OK. Thread safe; waits for the modem lock.
String modem_cmd(const char *cmd, uint32_t timeoutMs = 3000, bool *ok = nullptr);

// Lists every SMS stored in the modem's current read storage as a JSON array.
// Read-only: messages are not deleted (reading does mark them as read).
String modem_list_sms_json(int *count = nullptr, int *decodeErrors = nullptr);

const char *reg_state_name(int stat);
