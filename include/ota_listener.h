#pragma once
#include <Arduino.h>

// Wireless (HC-05 Bluetooth) re-flash support -- see HC05_BRINGUP.md and
// HC05_BLUETOOTH_OTA_NOTES.md. EVERY sketch flashed onto the pedal must
// call otaBegin() in setup() and checkForOTAResetTrigger() at the top of
// loop(), otherwise the next upload can't be done wirelessly.

// spare GPIO wired to the base of an NPN transistor whose collector pulls
// the Uno's RESET pin low
#define OTA_RESET_TRIGGER_PIN 7

// must match the HC-05's configured data-mode baud (AT+UART) and
// Optiboot's upload speed (upload_speed in platformio.ini)
#define OTA_UPLOAD_BAUD 115200

inline void otaBegin() {
  Serial.begin(OTA_UPLOAD_BAUD);
}

// Watches incoming Serial bytes for avrdude's STK500 sync sequence and, on
// a match, pulls RESET low via the transistor on OTA_RESET_TRIGGER_PIN --
// this is what lets avrdude's own normal retry behavior bootstrap a
// wireless upload over the HC-05 with no separate trigger protocol needed.
inline void checkForOTAResetTrigger() {
  static const uint8_t SYNC_TRIGGER[] = {0x30, 0x20}; // Cmnd_STK_GET_SYNC, Sync_CRC_EOP
  static uint8_t matchIndex = 0;
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (b == SYNC_TRIGGER[matchIndex]) {
      matchIndex++;
      if (matchIndex == sizeof(SYNC_TRIGGER)) {
        pinMode(OTA_RESET_TRIGGER_PIN, OUTPUT);
        digitalWrite(OTA_RESET_TRIGGER_PIN, HIGH);
        while (true) {} // reset happens almost immediately
      }
    } else {
      matchIndex = (b == SYNC_TRIGGER[0]) ? 1 : 0;
    }
  }
}
