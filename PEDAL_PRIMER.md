# How This Pedal Works: A Primer

This walks through the pedal from the guitar jack to the output jack: what
the hardware does, how the code turns that into effects, and the quirks you
need to know before writing new effects. It assumes you're comfortable with
Arduino code but new to audio DSP (digital signal processing).

Companion docs: [HC05_BRINGUP.md](HC05_BRINGUP.md) (wireless uploads, and the
rules every sketch must follow) and
[OTA_IMPLEMENTATION_PLAN.md](OTA_IMPLEMENTATION_PLAN.md) (Bluetooth wiring).

---

## 1. The big picture

```
 guitar ──► input circuit ──► A0 (ADC) ──► effect code ──► PWM pins 3 + 11 ──► output filter ──► amp
                                  ▲            ▲
             rotary switch ──► A2 │            │
             fx knob ───────► A3 ─┘            │
                                               │
                         loop(): one pass = one sample (mostly)
```

The Uno has no audio hardware. It fakes it with two general-purpose
peripherals:

- **Input:** the **ADC** (analog-to-digital converter) measures the guitar's
  voltage on pin A0, thousands of times a second. Each measurement is one
  **sample**.
- **Output:** **PWM** (pulse-width modulation) pins switch on and off very
  fast. A filter averages that switching into a smooth voltage that follows
  the samples.

Everything in between is ordinary C++ in `loop()`: read a sample, do some
math, write a sample.

---

## 2. Hardware, as implied by the code

The analog circuit isn't documented in this repo. This section is what the
code expects. Measure the real circuit before relying on any specific
voltage.

| Pin | Role | Set up in |
|---|---|---|
| **A0** | Guitar signal in (`left` = channel 0) | `analogRead(left)` in each effect |
| **A2** | 6-position rotary switch, wired as a resistor ladder | `readSwitchPosition()` |
| **A3** | Effect ("fx") knob, a potentiometer | `analogRead(3)` in `loop()` |
| **3 + 11** | Audio out, left channel (two PWM pins combined) | `setupIO()`, `output()` |
| **5 + 6** | Audio out, right channel. Configured, but no effect uses it | `setupIO()` |
| **D0, D1, D7** | HC-05 serial and the reset trigger: **off limits** | [ota_listener.h](include/ota_listener.h) |

### The input side

A guitar pickup produces a small AC signal that swings above and below 0 V.
The ADC can only measure 0 V up to its reference voltage. So the input
circuit has to:
- **bias** the signal: lift it so it sits in the middle of the ADC's range;
- probably amplify it.

`setupIO()` calls `analogReference(INTERNAL)`. That sets the ADC's full scale
to the chip's internal **1.1 V** reference instead of 5 V, so each of the
1024 steps is about 1.07 mV. That suits small signals.

### The output side: the dual-PWM trick

`analogWrite()` normally gives 8 bits (256 levels) at about 490 Hz. That's
useless for audio: the 490 Hz switching is audible, and 256 levels is
coarse. `setupIO()` fixes both:

- **Faster switching:** it puts Timer2 (pins 3 and 11) into *fast PWM* with
  no prescaler. The PWM then runs at 16 MHz / 256 = **62.5 kHz**, well above
  hearing, so the output filter can remove it.
- **More resolution:** `output()` splits a **10-bit** value across two pins:
  ```cpp
  pwm3  = value >> 2;           // top 8 bits
  pwm11 = (value & B11) << 6;   // bottom 2 bits
  ```
  A resistor network mixes pin 11 in at a much lower weight than pin 3. Pin
  3 provides the coarse steps, and pin 11 fills in finer steps between them.
  For this particular bit split, pin 11 needs about 1/256 of pin 3's weight.

`pwm3` and `pwm11` are just names for the timer compare registers `OCR2B`
and `OCR2A` ([timers.h](src/timers.h)). Writing them changes the duty cycle
instantly, much faster than calling `analogWrite()`.

---

## 3. Digital audio in three ideas

**Sampling rate.** How often you read the input. Sound can only be
reproduced up to half the sampling rate. This pedal doesn't run a fixed
clock: each pass through `loop()` takes one sample, so the rate is "however
fast the loop runs". That's roughly **7–10 kHz**, and it varies by effect.
So the pedal hears up to about 4–5 kHz: fine for guitar, a bit dark. Anything
that makes the loop slower lowers the sample rate.

**Bit depth.** How finely each sample is measured. The ADC gives 10 bits
(0–1023). Fewer bits means coarser steps and more grit, which is literally
what Bitcrush does on purpose.

**The midpoint.** Silence isn't 0. It's the bias point, the middle of the
range: 512 on a 10-bit scale, or 128 on an 8-bit scale. A sample's
"loudness" is its distance from the midpoint, and its sign (above or below)
is which half of the waveform you're in. Many effects get much easier if
you first subtract the midpoint, so you're working with a signed value.
Make it louder by multiplying, then add the midpoint back before output.

---

## 4. The code, top to bottom

### Startup: `setup()`

```cpp
setupIO();      // fast PWM on the output pins, 1.1 V ADC reference, faster ADC
otaBegin();     // Serial at 115200 for the wireless-upload listener
// fill activeEffect[] from presetForPosition[]
```

`setupIO()` also speeds up the ADC with `analogPrescale(analogPrescale32)`.
The ADC clock is normally 16 MHz / 128 = 125 kHz, about 9,600 readings per
second. Dividing by 32 instead gives 500 kHz, about 38,000 readings per
second, at a small cost in accuracy. Without this, three reads per sample
would cap the pedal at about 3 kHz.

### The main loop: one sample per pass

```cpp
void loop() {
  checkForOTAResetTrigger();          // wireless upload listener -- must stay first
  int position = readSwitchPosition();// which of 6 effects (A2)
  int fx = analogRead(3);             // knob, 0..1023 (A3)
  activeEffect[position](fx);         // run that effect for one sample
}
```

Each effect is a function `void effect_xxx(int fx)` that usually:
1. reads one input sample,
2. transforms it,
3. writes one output sample.

Then `loop()` runs again. The knobs are re-read on every sample, which is
simple and costs two of the three ADC readings per pass.

### Reading the rotary switch

The switch connects A2 to different points on a resistor ladder, so each
position gives a different voltage. The positions aren't evenly spaced (they
measured 502, 603, 680, 766, 836 and 929), so `readSwitchPosition()`:
- smooths the reading with a simple low-pass filter,
  `rawFiltered += (raw - rawFiltered) / 4`;
- compares it against thresholds halfway between neighbouring positions.

That smoothing line is a **one-pole low-pass filter**, the most useful
one-liner in DSP. It reappears in tone controls, envelope followers and
auto-wah.

### The effect library

```cpp
enum EffectId { EFFECT_BITCRUSH, EFFECT_OVERDRIVE, ... };     // a name for each effect

const Effect effectLibrary[] PROGMEM = {                      // id -> name -> function (in flash)
  { EFFECT_BITCRUSH, "Bitcrush", effect_bitcrush },
  ...
};

const EffectId presetForPosition[6] = { ... };                // switch position -> effect
EffectFn activeEffect[6];                                     // filled in at startup
```

- **Adding an effect** means writing the function, adding an enum value, and
  adding one line to `effectLibrary`.
- **Putting it on the switch** means changing one entry in
  `presetForPosition`.
- `findEffect()` resolves the names to function pointers once in `setup()`,
  so the loop just calls `activeEffect[position](fx)`, with no lookup per
  sample.
- The library is stored in flash (`PROGMEM`) to save RAM, so `findEffect()`
  reads it with `pgm_read_word()` / `pgm_read_ptr()` rather than directly.

**Effect state lives in `static` locals.** For example, Bitcrush's
`delayed` counter is declared inside the function, so it keeps its value
between calls. Because each effect's state is private, effects can't
interfere with each other.

**The shared buffer.** `byte array[BUFFER_SIZE]` (1600 bytes) is the only
large block of memory, and the time-based effects share it. That's safe
because only one effect runs at a time. If you switch effects, the new one
starts with the previous one's leftover samples, which is harmless.

---

## 5. The existing effects, explained

**Bitcrush** combines two kinds of degradation:
- *Sample-rate reduction:* it only takes a new sample every `fx/3` passes,
  holding the old value in between. That gives the staircase sound of a
  lower sample rate.
- *Bit reduction:* `(input >> bits) << bits` zeroes the low bits. The knob is
  cubed before scaling to `bits`, so most of the knob's travel goes to the
  extreme low-bit settings, where each step sounds most different.

**Overdrive** multiplies the input by a gain (1–51) and stores the result in
a `byte`. Large results don't clip: they **wrap around** (300 becomes 44),
which folds the waveform back on itself. That's harsher and more synth-like
than true clipping. A "Fuzz" that clamps instead (`min(x, 255)`) would sound
quite different.

**Short Delay** works through the whole 1600-sample buffer in one call. At
each slot it outputs what's stored there, then records a new sample in its
place. So you hear the input from one buffer-length ago, plus graininess.
There's no feedback and no dry signal, so it's more of a "late, crunchy
copy" than an echo.

**Helicopter** works through the same buffer, but adds each sample to its
neighbour (`array[i] + array[i-1]`). That smooths it (a crude low-pass
filter), and it only does this every `fx × 10` calls, giving a choppy,
rhythmic gating effect.

> Short Delay and Helicopter each take **tens of milliseconds per call**,
> processing a whole block. During that time the knobs aren't read and the
> upload listener doesn't run. That's within the rules, but a
> one-sample-per-call circular buffer is the better pattern for new effects
> (see section 7).

**Ring Mod** multiplies the input by a counter `j` that counts down
repeatedly. That counter is a crude sawtooth "carrier" oscillator.
Multiplying two signals produces their sum and difference frequencies, which
gives the metallic, bell-like ring-mod sound. The knob sets the counter's
period, and so the carrier pitch.

**Octave Down** watches for the input crossing the midpoint (128) going
upward, and flips a flip-flop each time it does. The flip-flop changes once
per input cycle, so it completes a full cycle every *two* input cycles:
half the frequency, one octave down. The output is a square wave at that
pitch. The knob sets its volume. This is how classic analog octave pedals
work, and why they track single notes well and chords badly.

---

## 6. Quirks and gotchas (read before writing effects)

1. **`byte input = analogRead(left)` keeps only the low 8 bits.**
   `analogRead` returns 0–1023. Storing it in a `byte` throws away the top 2
   bits, so the value wraps every 256 steps (about 0.27 V with the 1.1 V
   reference). Whether that wrapping happens while you play depends on the
   input bias and signal level. It's part of the existing effects'
   character. For new effects that should be clean, use
   `int input = analogRead(left);` (0–1023, midpoint ~512), or
   `analogRead(left) >> 2` for a proper 8-bit value (midpoint ~128).

2. **`output()` expects 0–1023, but the existing effects pass 0–255.** That
   only uses the bottom quarter of the output range, so the output is
   quieter than it could be. A new effect working in 10 bits can pass its
   result straight through. An 8-bit one can do `output(left, x << 2)`.

3. **`millis()`, `micros()` and `delay()` run 64× too fast.** `setupIO()`
   removes Timer0's prescaler to get fast PWM on pins 5 and 6. But Timer0 is
   also the Arduino's clock, so one real second reads as 64 "seconds". Use
   loop/sample counters for timing (as Bitcrush and Helicopter do), or
   divide by 64.

4. **Timer0's interrupt now fires 62,500 times a second**, for the same
   reason. Each one costs a few microseconds, which takes a noticeable bite
   out of every sample. If the right channel (pins 5 and 6) is never used,
   leaving Timer0 at its default would speed up the loop and fix `millis()`.

5. **Floating-point math is slow.** The AVR has no floating-point hardware,
   so a single `float` divide costs tens of microseconds. Several effects do
   `(float) fx / 20` on every sample, which lowers their sample rate. Prefer
   integers and shifts (`x >> 3` instead of `x / 8`), and precomputed tables
   in flash (`PROGMEM`) for things like sine waves.

6. **RAM is nearly full.** 1753 of 2048 bytes are used, leaving 295 for the
   stack. Use the shared `array`, not new buffers. Put lookup tables in
   flash with `PROGMEM`. Check the `RAM:` line on every build.

7. **The upload rules apply to every effect** (details in
   [HC05_BRINGUP.md](HC05_BRINGUP.md), Part 3): keep each call short, never
   use `Serial.print`, and stay off pins D0, D1 and D7.

---

## 7. Patterns for new effects

Most effects are combinations of four building blocks.

**Signed working value.** Do the math centred on zero:
```cpp
int x = analogRead(left) - 512;    // -512..511, silence = 0
x = x * gain >> 2;                 // louder (fixed-point gain)
output(left, constrain(x + 512, 0, 1023));   // clamp = hard clipping
```

**One-pole low-pass filter.** Tone control, smoothing, and the core of
auto-wah:
```cpp
static int y = 0;
y += (x - y) >> k;    // larger k = darker / slower
```

**Envelope follower.** "How loud am I playing right now?" Use it for noise
gates, swells, auto-wah and compressors:
```cpp
static int env = 0;
int level = abs(x);
env += (level - env) >> 6;    // smoothed loudness
```

**Circular buffer.** One sample in and one out per call, for echo, flanger,
reverse and stutter:
```cpp
static int i = 0;
int delayed = array[i];               // the sample from BUFFER_SIZE passes ago
array[i] = mix(input, delayed);       // add feedback here for an echo
i = (i + 1) % length;                 // shorter length = shorter delay
```

**LFO (low-frequency oscillator)** from a sine table. Use it for tremolo,
vibrato and flanger sweeps:
```cpp
const byte SINE[64] PROGMEM = { ... };
static unsigned int phase = 0;
phase += rate;                                   // bigger rate = faster wobble
byte s = pgm_read_byte(&SINE[phase >> 10]);      // top 6 bits index the table
```

### Adding an effect: checklist

1. Write `void effect_name(int fx)`. Keep its state in `static` locals.
2. Add `EFFECT_NAME` to `enum EffectId`, and a line to `effectLibrary`.
3. Put it on a switch position in `presetForPosition`.
4. Build. Check the `RAM:` figure.
5. Upload with `pio run -e uno_bluetooth -t upload` and listen.

---

## 8. Things worth measuring

These would turn some of the estimates above into facts:

- **The input's idle value.** With the guitar plugged in but silent, what
  does `analogRead(A0)` return? That's the true midpoint. It shows whether
  the `byte` wraparound (quirk 1) happens, and where Octave Down's 128
  threshold should be.
- **The real sample rate per effect.** Toggle a spare pin (D8, say) once per
  `loop()` and measure its frequency with a multimeter's Hz mode or a scope.
  The sample rate is twice the measured frequency.
- **The output network.** The resistor values on pins 3 and 11 confirm the
  pin 11 weighting, and the output filter's cutoff shows how much 62.5 kHz
  whine gets through.
