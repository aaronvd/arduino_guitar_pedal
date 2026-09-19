# Arduino Guitar Pedal — Context

## Project
Arduino-based DSP guitar pedal. Reads guitar signal via ADC, processes it sample-by-sample in `loop()`, outputs via PWM. Effect is selected by a rotary switch (`mode`), intensity by a potentiometer (`fx`). Built against Arduino IDE 0021 (pre-1.0 API).

## Files
- `Arduino_Guitar_Pedal.pde` — main sketch: `setup()`, `loop()` with per-mode effect logic, `readKnobs()`.
- `dsp.h` / `dsp.cpp` — `setupIO()` (configures PWM timers + fast ADC), `output(channel, value)` (splits a value across two PWM pins for higher effective resolution).
- `timers.h` / `timers.cpp` — direct AVR timer/ADC register manipulation (waveform generation mode, prescalers) — chip-specific (ATmega328P/168/2560 class), not part of the standard Arduino API.

## Effects implemented (mode values from rotary switch, read in `readKnobs()`)
- `mode == 6` — **Bitcrush**: `input >> 6 << 6` zeroes low 6 bits (4 quantization levels); `fx` also controls a sample-skip counter (`delayed`/`value300`) for sample-rate reduction.
- `mode == 7` — **Overdrive**: `input * value50`, byte overflow wraps around for crude clipping distortion; `fx` sets gain.
- `mode == 9` — **Short crunchy delay**: 2000-byte buffer (`array`) played back and overwritten each loop pass; aliasing from same-cycle read/write gives the "crunchy" character (not a true tunable echo).
- `mode == 10` — **Clean helicopter**: same buffer loop, but sums adjacent samples (`array[i] + array[i-1]`) as a smoothing/low-pass filter, gated by a periodic timer (`delayed > value10000`, `value10000 = fx * 10`) for rhythmic chopping.
- `mode == 11` — **Ring modulator**: multiplies input by a ramping counter `j` (0 to `value50`, resets), acting as a crude carrier oscillator; `fx` sets ramp period.
- `mode == 12` — **"Your awesome sound"** (placeholder/experimental slot): currently sums live sample with one 20 samples prior (`array[i] + array[i-20]`) — fixed-delay comb/flanger effect, meant to be customized.

## Effect ideas discussed (not yet implemented)
- Tremolo — slow oscillating gain multiply (reuse ring-mod's ramping-counter pattern at sub-audio rate).
- Fuzz/hard clip — explicit clamp instead of overflow-wrap distortion.
- Octave-down — zero-crossing detection + frequency halving via the buffer.
- Real feedback delay — `array[i] = input + (array[i] >> 1)` style, tunable tap distance via `fx`, unlike the current non-echo "delay" modes.
- Envelope-follower auto-wah — running amplitude average modulating a low-pass cutoff.
- Reverse/glitch playback — read buffer chunks backward periodically.
Recommended next: tremolo or real feedback delay — biggest new sonic territory for least code, reusing existing patterns.

## Arduino IDE 0021 → modern IDE migration (not yet applied)
Required changes:
1. `dsp.cpp`: `#include "WProgram.h"` → `#include "Arduino.h"` (renamed in Arduino 1.0).
2. `timers.cpp`: `#include "WConstants.h"` → `#include "Arduino.h"` (folded into Arduino.h in 1.0).
3. Rename `Arduino_Guitar_Pedal.pde` → `Arduino_Guitar_Pedal.ino` (`.pde` deprecated since Arduino 1.0).

Notes:
- All direct AVR register access in `timers.cpp`/`timers.h` (`TCCR0A`, `OCR2B`, `ADCSRA`, etc.) is chip-specific, not Arduino-API-specific — works unchanged on any classic AVR board (Uno, Nano, Mega, Pro Mini — ATmega328P/168/2560). Will NOT work on non-AVR boards (Uno R4, Nano 33 IoT/Every, ESP/ARM-based boards) since those registers don't exist there.
- `analogReference(INTERNAL)`, `analogWrite()` are standard API, unaffected by version.
- These header renames + file extension rename are the only required changes assuming the target board stays AVR-based (e.g., classic Uno/Nano/Mega). No edits have been made yet — pending user confirmation to proceed.

## Status
No code changes made yet in this conversation — only analysis/explanation. Next step when picking this back up: apply the 3 migration changes above if desired, then optionally implement one of the new effects.
