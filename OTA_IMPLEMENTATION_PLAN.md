# HC-05 Bluetooth OTA Reprogramming + Dead-Bug Wiring Plan

## Context

This guitar pedal (Arduino Uno, PlatformIO project) currently has no way to
reflash firmware without opening the enclosure and plugging in USB. The repo
already contains `HC05_BLUETOOTH_OTA_NOTES.md`, a validated reference distilled
from a prior working project, describing exactly how to add wireless (HC-05
Bluetooth SPP) reprogramming to a stock Optiboot Arduino: avrdude's own
STK500 sync bytes are used as the reset trigger, so no custom upload tooling
is needed — just a firmware listener, a one-transistor reset circuit, and a
second PlatformIO environment.

Enclosure space is tight, so rather than a perfboard/shield, the HC-05 will be
double-sided-taped to the underside of the Uno and the reset-trigger
circuitry (1 transistor + 2 resistors) and the RX voltage divider (2
resistors) will be dead-bugged directly to the Arduino's through-hole pin
headers from below.

**RF note**: a sealed metal pedal enclosure attenuates 2.4GHz Bluetooth
significantly (Faraday-cage effect), so range will be short — this is
acceptable here since OTA uploads only happen at the bench with a laptop
right next to the pedal, not in normal gig use. To maximize the odds of a
usable link without modifying the enclosure, the HC-05 should be positioned
under the board **as close as practical to an existing non-metal opening**
(e.g. above/near the input or output jack hole, or the LED bezel), rather
than dead-centered under the board, since RF leaks out through those gaps.
No drilling/external antenna is planned; if the signal proves unreliable
after assembly, revisit with an external antenna or a hole near the mounting
point as a follow-up.

Codebase findings (from exploration): Serial is currently **completely
unused** in `src/main.cpp` — no `Serial.begin()`, no conflicts to resolve.
`loop()` ([main.cpp:169-173](src/main.cpp#L169-L173)) is a 3-line dispatch
loop with no existing per-iteration overhead. Pins D7/D8/D9/D10/D12/D13 are
unreferenced anywhere in `src/`; D7 was selected as the reset-trigger pin
(unreferenced, not PWM/interrupt-capable, so no ambiguity with the
`timers.h`-defined-but-unused D9/D10 PWM pins). D0/D1 (hardware UART) are
implicitly reserved already and are exactly what the HC-05 needs. Power will
tap the Uno's onboard 5V/GND header pins (HC-05 breakout has its own 3.3V
regulator, accepts 3.6–6V on VCC).

## RAM budget (discovered during implementation)

Building the original firmware revealed it already overflowed the ATmega328P's
2048-byte SRAM before any OTA changes (2144 bytes / 104.7%, from the 2000-byte
`BUFFER_SIZE` delay buffer plus other globals). Enabling `Serial` for the OTA
listener made this worse. Resolution implemented:
- `build_flags` in `platformio.ini` shrink `HardwareSerial`'s buffers from the
  default 64+64 bytes to 16 (RX) + 1 (TX) — the sketch only needs to catch a
  2-byte sync sequence and never transmits.
- `BUFFER_SIZE` in `main.cpp` reduced from 2000 to 1600, shortening the Short
  Delay/Helicopter effects' max delay length to 80% of the original.
- Net result: 1811 / 2048 bytes (88.4%), leaving ~237 bytes of stack headroom
  instead of the original negative margin.

## Firmware changes

**File: [src/main.cpp](src/main.cpp)**

1. Add near the top (after existing includes):
   ```cpp
   #define OTA_RESET_TRIGGER_PIN 7
   ```
2. Add the resident listener function (adapted verbatim from the notes doc),
   placed above `setup()`:
   ```cpp
   void checkForOTAResetTrigger() {
     static const uint8_t SYNC_TRIGGER[] = {0x30, 0x20}; // avrdude STK_GET_SYNC + CRC_EOP
     static uint8_t matchIndex = 0;
     while (Serial.available()) {
       uint8_t b = Serial.read();
       if (b == SYNC_TRIGGER[matchIndex]) {
         matchIndex++;
         if (matchIndex == sizeof(SYNC_TRIGGER)) {
           pinMode(OTA_RESET_TRIGGER_PIN, OUTPUT);
           digitalWrite(OTA_RESET_TRIGGER_PIN, HIGH);
           while (true) {}
         }
       } else {
         matchIndex = (b == SYNC_TRIGGER[0]) ? 1 : 0;
       }
     }
   }
   ```
3. In `setup()` ([main.cpp:161](src/main.cpp#L161)), add `Serial.begin(115200);`
   (matching `upload_speed` chosen below) alongside the existing `setupIO();`
   call.
4. In `loop()` ([main.cpp:169](src/main.cpp#L169)), call
   `checkForOTAResetTrigger();` as the **first line**, before the existing
   switch-position/fx-read/dispatch logic — this must run every iteration
   with minimal added latency, and the existing loop body is cheap enough
   that this won't perceptibly affect audio-effect responsiveness.
5. This must be flashed once over USB before wireless upload can ever work.
   Every future sketch (USB or Bluetooth) must keep this code to retain
   wireless re-flash capability.

## PlatformIO config changes

**File: [platformio.ini](platformio.ini)**

Add `build_flags` to `env:uno` to shrink the Serial buffers (see RAM budget
above), and a second environment per the notes' pattern:
```ini
[env:uno]
...
build_flags =
  -D SERIAL_RX_BUFFER_SIZE=16
  -D SERIAL_TX_BUFFER_SIZE=1

[env:uno_bluetooth]
extends = env:uno
upload_port = COM<n>   ; HC-05's paired *outgoing* SPP COM port (see below)
upload_speed = 115200
```
`<n>` is discovered after Windows pairing (Control Panel → Devices and
Printers → Bluetooth Settings → COM Ports tab → outgoing port) — this is
environment-specific to Aaron's PC and filled in during setup, not guessable
in advance. (Implemented with a placeholder `COM3` until the real port is
identified.)

## HC-05 one-time AT configuration (no code, done via serial terminal)

Before wiring is buttoned up, configure the module (AT mode: hold KEY/EN high
at power-up, talk at fixed 38400 baud):
- `AT+ROLE=0` (slave)
- `AT+UART=115200,0,0` (match `upload_speed`)
- `AT+RESET` or power-cycle to return to normal SPP data mode
- Pair in Windows Bluetooth settings afterward.

## Hardware: dead-bug wiring on the underside of the Uno

Components needed: 1× HC-05 breakout, 1× NPN transistor (2N3904/2N5088 —
**verify E/B/C pinout against its actual datasheet before wiring**, don't
assume orientation), resistors: 1kΩ ×2, 2kΩ ×1, 10kΩ ×1.

Connections (all made directly between HC-05 pins / transistor leads / Uno
header pins, held in place dead-bug style, HC-05 body taped to the Uno's
underside with double-sided tape over a non-conductive area — avoid taping
directly over exposed header pins/traces):

- HC-05 VCC → Uno 5V header pin
- HC-05 GND → Uno GND header pin (shared with transistor emitter's GND leg)
- HC-05 RXD → voltage divider midpoint: Uno **D1 (TX)** → 1kΩ → node → 2kΩ →
  GND, node → HC-05 RXD (steps 5V logic down to a safe ~3.3V for the
  non-5V-tolerant HC-05 RX input)
- HC-05 TXD → Uno **D0 (RX)** directly (3.3V HIGH reads fine on 5V logic)
- Uno **D7** → 1kΩ → transistor base
- Transistor base → 10kΩ → GND (pulldown, keeps transistor off by default)
- Transistor collector → Uno **RESET** pin
- Transistor emitter → GND
- HC-05 KEY/EN pin: left unconnected/floating for normal operation (only
  pulled high momentarily, e.g. by hand, when entering AT config mode)

**Operational caveat carried over from the notes**: HC-05 TXD/RXD share D0/D1
with the onboard USB-serial chip. Whenever doing a **USB** upload, disconnect
the HC-05's TXD/RXD leads (or add a small physical disconnect point, e.g. a
2-pin header/jumper on those two dead-bug leads only) to avoid bus
contention; reconnect them for Bluetooth uploads. VCC/GND/reset-circuit
wiring can stay permanently connected.

## Diagram/schematic deliverable

Produce a labeled schematic (Artifact, using the diagramming skill) showing:
1. The Uno board outline with the relevant header pins called out (5V, GND,
   D0/RX, D1/TX, D7, RESET).
2. The HC-05 module positioned as "mounted underneath, dead-bugged, near an
   existing enclosure opening (jack/LED hole) for RF egress," with its 6 pins
   labeled (VCC, GND, TXD, RXD, STATE, KEY/EN — STATE unconnected).
3. The voltage-divider network (D1 → 1kΩ → node → 2kΩ → GND → HC-05 RXD) and
   direct HC-05 TXD → D0 link.
4. The transistor reset circuit (D7 → 1kΩ → base; base → 10kΩ → GND;
   collector → RESET; emitter → GND), with a clear pinout note to verify
   against the actual transistor's datasheet.
5. A callout/annotation marking the D0/D1 disconnect point needed before USB
   uploads.

This will be built as an HTML artifact with inline SVG (per the
artifact-diagramming skill), not a code file in the repo, since it's a
reference document rather than project source.

## Verification

1. Flash the modified `env:uno` build over USB first; confirm the pedal still
   works normally (footswitch/knob/effects unaffected by the added
   `Serial.begin()` + per-loop check).
2. Wire the dead-bug circuit with the HC-05 positioned near an existing
   enclosure opening (jack hole/LED bezel), pair the HC-05 in Windows,
   identify its outgoing COM port, fill in `upload_port` in
   `env:uno_bluetooth`. Before closing the enclosure fully, confirm the
   pairing/connection succeeds at bench distance — if it doesn't connect at
   all even with the lid off, troubleshoot wiring before revisiting RF
   placement.
3. Disconnect nothing (USB unplugged, running on pedal power or USB power
   with HC-05 leads connected) and run `pio run -e uno_bluetooth -t upload`;
   expect it to succeed, possibly after one failed sync retry (normal per the
   notes).
4. Confirm the pedal resumes normal operation immediately after the wireless
   upload completes.
5. Confirm a subsequent **USB** upload still works after temporarily
   disconnecting the HC-05 TXD/RXD leads.
