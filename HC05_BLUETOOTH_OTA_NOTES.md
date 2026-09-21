# Wireless (Bluetooth/HC-05) Reprogramming for AVR Arduino — Reusable Notes

Portable reference distilled from the `HC05_Bluetooth_Test` project. Use
this to bootstrap wireless-upload support on a new AVR-based Arduino
project (Uno, Nano, or anything else running a stock Optiboot-style
bootloader talking STK500v1 over serial).

## The core idea

`avrdude` doesn't need to know it's talking over Bluetooth — a paired
HC-05's SPP connection just shows up as a normal COM port, and the stock
`arduino`/`stk500v1` upload protocol works over it unmodified. The only
real problem to solve is **triggering a reset** at upload time: over USB,
the Arduino IDE/avrdude toggle DTR to reset the board automatically; over
Bluetooth SPP there's no DTR line, so something else has to pull RESET low
at the right moment.

## The technique that worked: let avrdude trigger its own reset

Don't build a custom protocol or a separate control channel. Instead, make
the currently-running sketch watch the *incoming Serial data* for
avrdude's own STK500 sync bytes and treat that as the reset signal:

- avrdude sends `Cmnd_STK_GET_SYNC, Sync_CRC_EOP` = bytes `0x30 0x20`,
  repeatedly, as soon as it opens the port (this is its normal retry
  behavior when it doesn't get a response — visible in its output as
  `stk500_getsync() attempt N of 10`).
- The running sketch matches this exact 2-byte sequence on `Serial` and,
  when seen, pulls RESET low via a transistor.
- The first sync attempt reboots the board into the bootloader; one of
  avrdude's very next automatic retries (sent shortly after) lands inside
  the bootloader's short post-reset listening window, and the rest of the
  upload proceeds as completely stock avrdude behavior.

This means: **no custom Python scripts, no pre-upload hooks, no magic
strings, and a single serial connection stays open for the whole
upload** — critical, because closing and reopening a Windows Bluetooth SPP
COM port in quick succession was observed to leave it in a broken state
(the reopen returns almost instantly but data silently stops flowing,
causing avrdude to hang indefinitely). Anything that requires closing the
port between "trigger reset" and "start upload" should be avoided.

## Hardware: the auto-reset transistor circuit

One NPN transistor (2N3904, 2N5088, etc. — anything that saturates easily
at low current works; verify its actual E/B/C pinout against its
datasheet, don't assume it matches another part in the same package):

```
   Spare GPIO pin ──[1kΩ]── transistor base
   Transistor base ──[10kΩ]── GND        (pulldown: keeps it OFF by default)
   Transistor collector ── Arduino RESET pin
   Transistor emitter ── GND
```

Driving the GPIO HIGH pulls RESET low (reset). Driving it LOW (or leaving
it floating — the pulldown handles that) leaves RESET alone, held high by
the board's own existing pull-up. Pick any spare digital pin that isn't
used for anything else in the sketch.

## Firmware pattern

Resident code, called every `loop()` iteration, alongside whatever the
sketch actually does:

```cpp
#define RESET_TRIGGER_PIN 7  // whichever spare pin you wired up
const uint8_t SYNC_TRIGGER[] = {0x30, 0x20};  // avrdude's STK_GET_SYNC + CRC_EOP

void checkForResetTrigger()
{
  static uint8_t matchIndex = 0;
  while (Serial.available())
  {
    uint8_t b = Serial.read();
    if (b == SYNC_TRIGGER[matchIndex])
    {
      matchIndex++;
      if (matchIndex == sizeof(SYNC_TRIGGER))
      {
        pinMode(RESET_TRIGGER_PIN, OUTPUT);
        digitalWrite(RESET_TRIGGER_PIN, HIGH);
        while (true) {}  // reset happens almost immediately
      }
    }
    else
    {
      matchIndex = (b == SYNC_TRIGGER[0]) ? 1 : 0;
    }
  }
}
```

Call `checkForResetTrigger()` early in every `loop()`. **This code has to
already be on the chip before you can reprogram it wirelessly** — the very
first flash containing this listener must happen over USB. After that,
every subsequent sketch you flash (over USB or Bluetooth) should keep
including this function so wireless re-upload keeps working — otherwise
you lose the ability to re-flash wirelessly until you plug in USB again.

## Wiring pattern

- HC-05 must be on the board's **hardware UART pins** (RX0/TX1 on an Uno),
  not a SoftwareSerial pair — the bootloader only listens on the hardware
  UART.
- HC-05 RX is 3.3V logic and NOT 5V tolerant: put a voltage divider
  (e.g. 1kΩ + 2kΩ) between the Arduino's TX pin and HC-05 RX.
- HC-05 TX → Arduino RX can connect directly (3.3V reads fine as HIGH on
  5V logic boards).
- **Pin conflict**: the HC-05 and the onboard USB-serial chip both want
  pins 0/1. Having both connected simultaneously causes bus contention
  that corrupts both USB uploads and general serial behavior. Disconnect
  (or switch out) the HC-05 whenever doing a USB upload.

## PlatformIO config pattern

```ini
[env:<board>]
platform = atmelavr
board = <board>
framework = arduino

; Upload over the HC-05's paired Bluetooth SPP COM port.
[env:<board>_bluetooth]
extends = env:<board>
upload_port = COM<n>     ; the HC-05's paired *outgoing* COM port
upload_speed = 115200    ; must match the HC-05's configured UART baud
```

No `extra_scripts` needed — this is the whole config. Upload with:

```
pio run -e <board>_bluetooth -t upload
```

## HC-05 one-time AT configuration

Before any of this works, the HC-05 itself needs configuring via AT
commands (enter AT mode by holding KEY/EN high at power-up — full AT mode
runs at a fixed 38400 baud regardless of the module's data-mode baud):

- `AT+ROLE=0` — slave mode
- `AT+UART=<baud>,0,0` — set the module's data-mode baud to match
  `upload_speed` above (matching your bootloader's expected upload speed,
  e.g. 115200 for Optiboot on an Uno)
- Power-cycle / `AT+RESET` afterward to drop back into normal SPP data
  mode

Then pair the module in Windows Bluetooth settings, and find its assigned
**outgoing** COM port via Control Panel → Devices and Printers → Bluetooth
Settings → COM Ports tab (the modern Settings app doesn't show this
mapping).

## Checklist for a new project

1. Wire HC-05 to hardware UART pins with the RX voltage divider.
2. Wire the transistor auto-reset circuit to a spare GPIO + RESET.
3. Configure the HC-05 via AT commands (role, baud) and pair it in Windows.
4. Add `checkForResetTrigger()` (watching for `0x30 0x20`) to the sketch,
   called every `loop()`.
5. Flash that sketch once over USB.
6. Add the `<board>_bluetooth` PlatformIO env with the paired COM port.
7. Disconnect the HC-05 from pins 0/1 before any future USB upload; keep
   it connected for Bluetooth uploads.
8. `pio run -e <board>_bluetooth -t upload` — expect an occasional failed
   attempt (Bluetooth latency variance) before a retry succeeds; this is
   normal, not a sign something is broken.

## Dead ends worth remembering (so you don't repeat them)

- **Manual reset-button timing** works in principle but is unreliable in
  practice — avrdude's sync retry window and the bootloader's listen
  window are both short, so this needs excellent timing and many retries.
- **A separate pre-upload script that sends a custom trigger string, then
  closes its connection for avrdude to reopen** fails hard on Windows: the
  reopen looks instant but the underlying Bluetooth link doesn't come back
  in a working state within any reasonable delay, so avrdude hangs
  indefinitely rather than failing cleanly. Don't close/reopen the port
  between triggering the reset and letting avrdude take over.
- **A custom Python STK500v1 client to avoid avrdude entirely** is a real
  fallback if the avrdude-native-trigger technique above doesn't sync
  reliably on your setup, but wasn't needed here and adds real
  implementation/maintenance cost — try the simpler technique first.
- Extending the bootloader's post-reset timeout (reflashing a
  longer-timeout Optiboot variant via ISP) is a legitimate, more robust
  fix if reconnects on your hardware are slow/flaky, but requires an ISP
  programmer and wasn't necessary once the avrdude-native trigger was
  used correctly.
