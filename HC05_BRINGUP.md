# HC-05 Bring-up and Wireless Upload Procedure (Linux)

This procedure is for the pedal as built: every connection is soldered
according to the dead-bug diagram, so nothing gets rewired. The Uno
configures the HC-05 itself, using a one-time sketch
([src/hc05_config.cpp](src/hc05_config.cpp)).

Background: [HC05_BLUETOOTH_OTA_NOTES.md](HC05_BLUETOOTH_OTA_NOTES.md)
(how the technique works) and
[OTA_IMPLEMENTATION_PLAN.md](OTA_IMPLEMENTATION_PLAN.md) (wiring).

`pio` below means PlatformIO's CLI. In VSCode, use the PlatformIO terminal,
or run `~/.platformio/penv/bin/pio`.

---

## Part 1: One-time bring-up

### Step 1: Flash the config sketch over USB

```bash
pio run -e hc05_config -t upload
```

This is the **only USB upload you should ever need**. The config sketch
also contains the OTA listener, so the real pedal firmware can go on over
Bluetooth afterwards (Step 5).

> **With HC-05 TXD connected to D0, this upload fails** with
> `not in sync`, as it did on this pedal. The HC-05 holds D0 high, and the
> USB chip, which reaches D0 through a 1kΩ resistor, can't pull it low.
> Disconnect HC-05 TXD from D0 for this upload, then **reconnect it before
> Step 2**: the config sketch reads the module's replies on D0. Ideally put
> a 2-pin header and jumper in that wire, so any future USB upload is just
> a matter of pulling the jumper.
>
> If `/dev/ttyACM0` doesn't exist, the Uno may have come up as `ttyACM1`
> or `ttyACM2` after a replug. Check with `ls /dev/ttyACM*`. If you get
> `permission denied`, see the `dialout` note in Step 3.

### Step 2: Run it with the HC-05 in AT mode

1. Unplug the USB cable (power off).
2. Put the HC-05 in AT mode at power-up, using **either** of these:
   - If the breakout has a small push button: hold it down.
   - Otherwise: touch a jumper wire from the HC-05's **KEY/EN** pin to the
     Uno's **3.3V** pin and hold it there. A clip lead makes this easier.
     Nothing gets soldered.
3. Plug USB back in while still holding. After about 1 s, release. The
   HC-05's LED should blink **slowly** (about every 2 s), which means AT
   mode.
4. Wait about 10 seconds, then read the Uno's onboard **"L" LED**. If it
   shows "blip every 2 s" even though the HC-05 is blinking slowly, press the
   Uno's reset button. That restarts only the sketch, and the HC-05 stays in
   AT mode.

| L LED | Meaning | Next |
|---|---|---|
| **Solid on** | Configured: slave, 115200 baud | Go to Step 3 |
| **Fast blinking** | In AT mode, but a command was rejected | Power-cycle and retry Step 2 |
| **Short blip every 2 s** | The HC-05 wasn't in AT mode | Retry Step 2, holding KEY/button from *before* power-up |

AT settings are stored in the HC-05 permanently. This step never has to be
repeated unless the module is replaced.

### Step 3: Return to normal mode and pair

1. Unplug USB, make sure KEY/button is released, and power up again. The
   HC-05 LED now blinks **fast**, meaning it's waiting for a connection.
   The L LED blips every 2 s, meaning the config sketch is now running as
   an OTA listener.
2. Pair from the PC (the PIN is usually `1234`, sometimes `0000`):
   ```bash
   bluetoothctl
     power on
     scan on        # wait for "HC-05", note its MAC (98:D3:xx:xx:xx:xx)
     scan off
     pair <MAC>
     trust <MAC>
     quit
   ```
3. Create the serial port that `platformio.ini` expects:
   ```bash
   sudo rfcomm bind 0 <MAC> 1      # creates /dev/rfcomm0
   ```
   `rfcomm bind` doesn't survive a reboot, so re-run it after each restart.
   If you get `permission denied` on `/dev/rfcomm0`, run
   `sudo usermod -aG dialout $USER`, then log out and back in.
4. Stop ModemManager from grabbing the port. It probes new serial ports for
   modems, and this causes `Device or resource busy` errors:
   ```bash
   echo 'KERNEL=="rfcomm*", ENV{ID_MM_DEVICE_IGNORE}="1"' | sudo tee /etc/udev/rules.d/99-rfcomm-no-modemmanager.rules
   sudo udevadm control --reload-rules
   ```
   Or, if you don't use a cellular modem, `sudo systemctl disable --now ModemManager`.

### Step 4: Test the link and the reset circuit

```bash
pio device monitor -p /dev/rfcomm0 -b 115200
```

- **HC-05 LED** changes to a slow double-blink, which means the Bluetooth
  link is up. Nothing prints in the monitor. That's normal, because the
  sketches never transmit.
- **Type `0` then a space.** These are the STK500 sync bytes (`0x30 0x20`)
  that an upload starts with, and the listener's reset trigger.
  The Uno should reset: the L LED flickers for about a second while the
  bootloader runs, then goes back to blipping. **That proves the whole
  chain**: Bluetooth → D0 → listener → D7 → transistor → RESET.
- Exit with `Ctrl+C`. The upload needs the port, so the monitor must be
  closed first.

### Step 5: Upload the pedal firmware wirelessly

```bash
pio run -e uno_bluetooth -t upload
```

`uno_bluetooth` is the default env, so the PlatformIO toolbar's **Upload**
button (→) does the same thing. It takes about 12 s and looks like this:

```
Uploading .pio/build/uno_bluetooth/firmware.hex (4608 bytes) over /dev/rfcomm0
Writing  | ################################################## | 100%
Verifying | ################################################## | 100%
4608 bytes written and verified.
```

This env doesn't use avrdude. It uses
[scripts/bt_upload.py](scripts/bt_upload.py), a small uploader that sends
the bootloader the same commands avrdude would. avrdude was tested
repeatedly over this link and failed most uploads part-way through. The
script:

- brings the Bluetooth link up before sending anything;
- resets the board through the OTA listener;
- writes and verifies every page.

The bootloader occasionally drops out mid-upload (about 1 upload in 3 in
testing). When it does, you'll see
`Lost the bootloader at 0x.... -- recovering`, and the script resets the
board and carries on from the failed page. That's normal.

When it finishes, the pedal should start up and play normally. Bring-up is
done.

---

## Part 2: Everyday wireless uploads

1. Power the pedal.
2. If the PC has rebooted: `sudo rfcomm bind 0 <MAC> 1`
3. `pio run -e uno_bluetooth -t upload` (or the toolbar Upload button)

With the lid on, range is short because the metal enclosure blocks most of
the signal, so keep the laptop right next to the pedal.

**If the upload asks you to press RESET:** the sketch on the board isn't
answering the reset trigger. This usually means a previous upload died
part-way, or the sketch broke one of the rules in Part 3. Press the Uno's
reset button within 20 s. The script catches the bootloader during its
start-up window and continues. You need access to the board for this, so
open the lid.

---

## Part 3: Rules for writing new pedal sketches

Each new sketch you flash must be able to accept the *next* upload. If a
sketch breaks the rules below, the reset trigger stops working. You can
still recover by pressing the Uno's RESET button when the upload asks (see
Part 2), but that means opening the enclosure. So follow these every time:

1. **Include the listener and call it.**
   ```cpp
   #include "ota_listener.h"

   void setup() {
     otaBegin();                 // Serial at 115200
     // ... your setup ...
   }

   void loop() {
     checkForOTAResetTrigger();  // always the first line
     // ... your effect code ...
   }
   ```
2. **Keep every `loop()` pass short: well under 0.5 s.** The listener only
   runs between passes. The Short Delay effect's 1600-sample pass takes
   tens of milliseconds, which is fine. Never use a long `delay()`, a blocking
   `while` wait, or an effect that loops for seconds inside one pass.
3. **Never use `Serial.print()` / `Serial.write()`.** The TX buffer is shrunk
   to 1 byte to save RAM, and any multi-byte print **hangs the sketch
   forever**, taking the listener down with it. Don't call `Serial.end()`
   either.
4. **Don't use pins D0, D1 (HC-05) or D7 (reset trigger).** Writing D7 HIGH
   resets the board.
5. **Don't leave interrupts disabled for long**, and don't take over the
   USART or its interrupts. The listener depends on `Serial` receiving
   bytes in the background.
6. **Check RAM on every build.** The build output shows
   `RAM: ... (used N bytes from 2048)`. Keep at least about 200 bytes free
   for the stack. Currently 1811 are used, which leaves 237. Running out of
   RAM crashes the sketch unpredictably, and that can kill the listener too.
7. **Build and upload through `uno_bluetooth`** (or `uno`). Those envs carry
   the Serial-buffer build flags and exclude `hc05_config.cpp`. New `.cpp`
   files in `src/` are picked up automatically.

**Safe-testing habit:** before trying something risky (heavy
`noInterrupts()` use, big buffers, new timer code), upload it and then
immediately upload it a *second* time. If the second upload works, that
sketch doesn't block the listener.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| HC-05 LED never shows the connected double-blink | Pairing or `rfcomm bind` problem. Check the MAC with `bluetoothctl devices`; re-pair |
| Link connects, but typing `0 ` doesn't reset the board | Listener sketch not running, or a reset-circuit fault: check transistor E/B/C orientation and the D7/1k/10k joints |
| Board resets, but the upload never syncs | HC-05 baud isn't 115200. Redo Part 1 Step 2 (needs the config sketch on the board) |
| Works with the lid off, fails with it on | The enclosure is blocking the signal. Move the module closer to a jack or LED opening |
| `could not open port /dev/rfcomm0` | Monitor still open, `rfcomm` not bound, or not in the `dialout` group |
| `Could not exclusively lock port` / `Resource temporarily unavailable` | A serial monitor is still open on that port, perhaps in another terminal tab. `fuser -v /dev/rfcomm0` shows which process; close it with `Ctrl+C` |
| `Device or resource busy` on `/dev/rfcomm0` | ModemManager is probing the port. See Part 1 Step 3.4 |
| `Lost the bootloader ... recovering` | Normal now and then. Only a problem if it runs out of retries. Then move the laptop closer or open the lid |

---

## Recovery: no USB upload possible

First try a normal wireless upload and press RESET when it asks (Part 2).
This works whenever the bootloader is intact, even if the sketch is broken.

If that doesn't work either, the board has to be programmed a way that
doesn't use D0. Options, from least to most invasive:

- **ISP programming through the 6-pin ICSP header** using a USBasp or a
  second Arduino running "ArduinoISP". This bypasses D0/D1 completely and
  needs no desoldering. Burn the bootloader first (this also erases the
  chip), then write the sketch *without* erasing. That keeps Optiboot, so
  wireless uploads keep working afterwards. This needs its own PlatformIO
  env, which hasn't been set up yet.
- **Disconnect HC-05 TXD from D0** (pull the jumper, if you fitted one, or
  desolder that one wire), do the USB upload with `pio run -e uno -t upload`,
  then reconnect it.
