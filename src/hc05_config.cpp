// One-time HC-05 configuration sketch -- see HC05_BRINGUP.md.
//
// Built only by the hc05_config env. Lets the Uno itself send the HC-05's
// AT commands over the existing D0/D1 wiring, so nothing has to be
// rewired. Reports what happened on the onboard "L" LED (D13):
//
//   solid on           AT mode found, module configured -- power-cycle now
//   fast blink         AT mode found, but a command failed
//   blip every 2 sec   module not in AT mode -- acting as a normal OTA
//                      listener at 115200, so the real pedal firmware can
//                      be uploaded wirelessly
#include <Arduino.h>
#include "ota_listener.h"

// full AT mode (KEY/EN high or button held at power-up) is always 38400
#define AT_MODE_BAUD 38400
#define REPLY_TIMEOUT_MS 1000

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)

// the settings wireless uploads depend on
const char* const CONFIG_COMMANDS[] = {
  "AT+ROLE=0",                                 // slave: the PC connects to it
  "AT+UART=" TO_STRING(OTA_UPLOAD_BAUD) ",0,0", // data-mode baud = upload speed
};
const int NUM_CONFIG_COMMANDS = sizeof(CONFIG_COMMANDS) / sizeof(CONFIG_COMMANDS[0]);

enum Result { LISTENING, CONFIGURED, FAILED };
Result result;

// sends one AT command and waits for the module's "OK" or "ERROR" line
bool sendCommand(const char* cmd) {
  while (Serial.available()) Serial.read(); // discard anything stale
  Serial.print(cmd);
  Serial.print("\r\n");

  char line[24];
  uint8_t len = 0;
  unsigned long start = millis();
  while (millis() - start < REPLY_TIMEOUT_MS) {
    if (!Serial.available()) continue;
    char c = Serial.read();
    if (c == '\n') {
      line[len] = '\0';
      if (strcmp(line, "OK") == 0) return true;
      if (strncmp(line, "ERROR", 5) == 0) return false;
      len = 0; // some other reply line, keep waiting for OK/ERROR
    } else if (c != '\r' && len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
  return false;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  delay(1000); // give the HC-05 time to boot after power-up
  Serial.begin(AT_MODE_BAUD);

  // the HC-05 can take a few seconds to start answering after a cold
  // power-up, so keep trying for about 8 seconds
  bool inATMode = false;
  for (int i = 0; i < 8 && !inATMode; i++) {
    inATMode = sendCommand("AT");
  }
  if (!inATMode) {
    Serial.end();
    otaBegin();
    result = LISTENING;
    return;
  }

  for (int i = 0; i < NUM_CONFIG_COMMANDS; i++) {
    if (!sendCommand(CONFIG_COMMANDS[i])) {
      result = FAILED;
      return;
    }
  }
  result = CONFIGURED;
}

void loop() {
  switch (result) {
    case LISTENING:
      checkForOTAResetTrigger();
      digitalWrite(LED_BUILTIN, millis() % 2000 < 50);
      break;
    case CONFIGURED:
      digitalWrite(LED_BUILTIN, HIGH);
      break;
    case FAILED:
      digitalWrite(LED_BUILTIN, millis() % 200 < 100);
      break;
  }
}
