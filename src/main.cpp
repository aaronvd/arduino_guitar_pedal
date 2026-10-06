#include <Arduino.h>
#include "dsp.h"
#include "ota_listener.h"

// shrunk from 2000 -- SRAM is tight enough on the Uno (2048 bytes total)
// that this buffer alone was already overflowing it before the OTA Serial
// listener was added; this leaves headroom for that plus the stack. Effect
// of the smaller buffer: the Short Delay/Helicopter effects' max delay
// length is proportionately shorter than before.
#define BUFFER_SIZE 1600

// shared scratch buffer for time-based effects (short delay, helicopter,
// echo, stutter, reverse).
// Only one effect runs per loop pass, so it's safe for them to share this
// rather than each carrying its own 2000-byte buffer -- SRAM is far too
// tight on the Uno for more than one copy of this.
byte array[BUFFER_SIZE];

typedef void (*EffectFn)(int fx);

enum EffectId {
  EFFECT_BITCRUSH,
  EFFECT_OVERDRIVE,
  EFFECT_SHORT_DELAY,
  EFFECT_HELICOPTER,
  EFFECT_RING_MOD,
  EFFECT_OCTAVE_DOWN,
  EFFECT_TREMOLO,
  EFFECT_OCTAVE_UP,
  EFFECT_FUZZ,
  EFFECT_WAVEFOLDER,
  EFFECT_SQUARE_FUZZ,
  EFFECT_SINE_RING_MOD,
  EFFECT_ECHO,
  EFFECT_SWELL,
  EFFECT_NOISE_GATE,
  EFFECT_LOWPASS,
  EFFECT_AUTO_WAH,
  EFFECT_STUTTER,
  EFFECT_REVERSE,
};

// ***************************
// ***shared signal helpers***
// ***************************
// The effects below work on the full 10-bit input as a signed value
// centred on zero (silence = 0), rather than the low 8 bits like the
// older effects -- see PEDAL_PRIMER.md, "Quirks and gotchas".

// Reads one input sample with the input circuit's bias removed. The bias
// point hasn't been measured, so rather than assume 512 this tracks the
// input's long-term average (a slow one-pole low-pass filter, ~0.1 s) and
// subtracts it.
int readCentered() {
  static long biasScaled = 512L << 8; // running average, x256 for precision
  int raw = analogRead(left);
  biasScaled += (((long) raw << 8) - biasScaled) >> 10;
  return raw - (int) (biasScaled >> 8);
}

// Writes a signed sample centred on the middle of output()'s 0-1023 range,
// hard-clipping anything that won't fit.
void writeCentered(long x) {
  output(left, constrain(x + 512, 0, 1023));
}

// one cycle of a sine wave, -127..127, kept in flash to save RAM
const int8_t SINE[64] PROGMEM = {
     0,   12,   25,   37,   49,   60,   71,   81,   90,   98,  106,  112,  117,  122,  125,  126,
   127,  126,  125,  122,  117,  112,  106,   98,   90,   81,   71,   60,   49,   37,   25,   12,
     0,  -12,  -25,  -37,  -49,  -60,  -71,  -81,  -90,  -98, -106, -112, -117, -122, -125, -126,
  -127, -126, -125, -122, -117, -112, -106,  -98,  -90,  -81,  -71,  -60,  -49,  -37,  -25,  -12,
};

// Sine oscillator: advances phase by `increment` (a full cycle is 65536)
// and returns the current value, -127..127. Frequency in Hz is roughly
// increment * sampleRate / 65536, and sampleRate is only approximate
// (~7-10 kHz, varying per effect), so tune increments by ear.
int8_t sineStep(unsigned int &phase, unsigned int increment) {
  phase += increment;
  return (int8_t) pgm_read_byte(&SINE[phase >> 10]); // top 6 bits index the table
}

// Envelope follower: the smoothed loudness of a centred signal, 0..~512.
// Rises at a rate set by attackShift and falls at releaseShift -- each is
// a one-pole filter shift, so larger = slower (3 is a few samples, 9 is
// tens of milliseconds). env16 is the caller's state, kept x16 for
// precision so small changes aren't lost to rounding.
int envelopeStep(int &env16, int x, byte attackShift, byte releaseShift) {
  int target = abs(x) << 4;
  env16 += (target - env16) >> (target > env16 ? attackShift : releaseShift);
  return env16 >> 4;
}

// The shared buffer holds bytes, so buffered effects store centred samples
// as signed 8-bit: the 10-bit value scaled down by 4, losing its 2 lowest
// bits (inaudible next to the rest of the pedal's grit).
void storeSample(int i, int x) {
  array[i] = (byte) (int8_t) constrain(x >> 2, -128, 127);
}

int loadSample(int i) {
  return (int8_t) array[i] << 2;
}

// *************
// ***bitcrush**
// *************
void effect_bitcrush(int fx) {
  static int delayed = 0;

  int value300 = 1 + ((float) fx / (float) 3);
  if(delayed > value300) {
    byte input = analogRead(left);
    // fx also sets how many low bits get zeroed (0 = full 8-bit/clean,
    // 7 = crushed down to 1 bit), on top of the sample-rate reduction above.
    // Cubing fx before scaling gives the low bit-depths (which sound far
    // more drastic per step than the high ones) most of the knob's
    // travel, instead of splitting it evenly and feeling touchy at one end.
    long fxCubed = (long) fx * fx * fx;
    byte bits = fxCubed * 7L / ((long) 1023 * 1023 * 1023);
    input = (input >> bits) << bits;
    output(left, input);
    delayed = 0;
  }
  delayed++;
}

// ***************
// ***Overdrive***
// ***************
void effect_overdrive(int fx) {
  int value50 = 1 + ((float) fx / (float) 20);
  byte input = analogRead(left);
  input = (input * value50);
  output(left, input);
}

//  *************************
//  ***short crunchy delay***
//  *************************
void effect_shortDelay(int fx) {
  for(int i = 0; i < BUFFER_SIZE; i++) { // set up a loop
    output(left, array[i]);
    array[i] = analogRead(left);
  }
}

//  **********************
//  ***clean helicopter***
//  **********************
void effect_helicopter(int fx) {
  static int delayed = 0;

  int value10000 = fx * 10;
  if(delayed > value10000) {
    for(int i = 0; i < BUFFER_SIZE; i++) { // set up a loop
      array[i] = array[i] + array[i - 1]; //removes noise and delay
      output(left, array[i]);
      array[i] = analogRead(left);
    }
    delayed = 0;
  }
  delayed++;
}

//  ********************
//  ***ring modulator***
//  ********************
void effect_ringMod(int fx) {
  static int j = 50;

  int value50 = 1 + ((float) fx / (float) 20);
  byte input = analogRead(left);
  input = (input * j);
  output(left, input);

  j = j - 1;
  if(j <= 0) {
    j = value50;
  }
}

//  *****************
//  ***octave down***
//  *****************
void effect_octaveDown(int fx) {
  static byte octavePrev = 128;
  static boolean octaveState = false;

  byte input = analogRead(left);

  // toggle a flip-flop each time the signal crosses the center point going
  // upward -- flipping every other cycle halves the fundamental frequency
  if(octavePrev < 128 && input >= 128) {
    octaveState = !octaveState;
  }
  octavePrev = input;

  byte level = 1 + ((float) fx / (float) 4);
  output(left, octaveState ? level : 0);
}

//  *************
//  ***tremolo***
//  *************
void effect_tremolo(int fx) {
  static unsigned int phase = 0;

  // fx sets the speed: increment 4..131, roughly 0.5-15 Hz
  int8_t lfo = sineStep(phase, 4 + (fx >> 3));
  int gain = 128 + lfo; // 1..255, i.e. volume swings between ~0% and ~100%
  writeCentered((long) readCentered() * gain >> 8);
}

//  ***************
//  ***octave up***
//  ***************
void effect_octaveUp(int fx) {
  static int rectifiedAverage = 0;

  // full-wave rectifying flips the bottom half of the waveform up, so it
  // repeats twice per input cycle: double the frequency, one octave up
  int x = readCentered();
  int rectified = abs(x);
  // rectifying leaves everything above zero; take out that offset
  rectifiedAverage += (rectified - rectifiedAverage) >> 6;
  int octave = (rectified - rectifiedAverage) * 2;

  // fx is the mix: 0 = all dry, 1023 = all octave
  writeCentered(((long) x * (1023 - fx) + (long) octave * fx) >> 10);
}

//  **********
//  ***fuzz***
//  **********
void effect_fuzz(int fx) {
  // unlike Overdrive, which wraps around on overflow, this clamps at the
  // top and bottom of the output range (hard clipping) -- fx sets the gain,
  // 1..64x, and so how hard the signal is pushed into the clipping
  int gain = 1 + (fx >> 4);
  writeCentered((long) readCentered() * gain);
}

//  ****************
//  ***wavefolder***
//  ****************
void effect_wavefolder(int fx) {
  const long FOLD = 384; // fold threshold -- the output never exceeds this

  // amplify, then reflect anything beyond +/-FOLD back inward, as many
  // times as it takes. Done in closed form: the input-to-output curve is
  // a triangle wave with period 4*FOLD.
  int gain = 1 + (fx >> 6); // 1..16x
  long y = (long) readCentered() * gain;
  long m = (y + FOLD) % (4 * FOLD);
  if(m < 0) m += 4 * FOLD;
  writeCentered(m < 2 * FOLD ? m - FOLD : 3 * FOLD - m);
}

//  *****************
//  ***square fuzz***
//  *****************
void effect_squareFuzz(int fx) {
  const int LEVEL = 200;

  // output only full-on or full-off depending on which side of the midpoint
  // the signal is -- an extreme, synth-like fuzz. fx sets a dead zone
  // (0..127 counts either side of the midpoint) where the output is silent,
  // so quiet noise between notes is gated out instead of fuzzed.
  int deadZone = fx >> 3;
  int x = readCentered();
  if(x > deadZone) {
    writeCentered(LEVEL);
  } else if(x < -deadZone) {
    writeCentered(-LEVEL);
  } else {
    writeCentered(0);
  }
}

//  *************************
//  ***sine ring modulator***
//  *************************
void effect_sineRingMod(int fx) {
  static unsigned int phase = 0;

  // like Ring Mod, but the carrier is a real sine wave instead of a ramping
  // counter, for a cleaner, more bell-like tone. fx sets the carrier
  // frequency, squared so the knob's travel feels even: increment
  // 100..16452, roughly 10 Hz-2 kHz
  unsigned int increment = 100 + (unsigned int) (((long) fx * fx) >> 6);
  int8_t carrier = sineStep(phase, increment);
  writeCentered((long) readCentered() * carrier >> 7);
}

//  **********
//  ***echo***
//  **********
void effect_echo(int fx) {
  static int i = 0;

  // A circular buffer handling one sample per call (unlike Short Delay,
  // which works through the whole buffer at once). Each slot holds the
  // input from `length` samples ago plus half of what was there before --
  // that feedback is what makes the echo repeat, each repeat half as loud.
  // fx sets the delay time: 100..BUFFER_SIZE samples, roughly 10-200 ms
  // depending on the real sample rate.
  int length = 100 + (int) ((long) fx * (BUFFER_SIZE - 100) >> 10);
  if(i >= length) i = 0;

  int x = readCentered();
  int delayed = loadSample(i);
  storeSample(i, x + (delayed >> 1));
  i++;
  writeCentered((long) x + delayed);
}

//  ***********
//  ***swell***
//  ***********
void effect_swell(int fx) {
  static int fast16 = 0, slow16 = 0;
  static unsigned int gain = 0; // 0..65535 = silent..full volume

  // "Slow gear": each new note fades in instead of starting with a pluck,
  // like a violin bow. A note's attack shows up as the fast envelope
  // jumping well above the slow one; that restarts the fade from silence.
  // The +10 keeps background noise from triggering it -- raise it if the
  // pedal swells on its own, lower it if soft notes don't trigger.
  int x = readCentered();
  int fast = envelopeStep(fast16, x, 2, 5);
  int slow = envelopeStep(slow16, x, 7, 7);
  if(fast > 2 * slow + 10) gain = 0;

  // fx sets the fade-in time: increment 82 (~0.1 s) down to 4 (~2 s)
  unsigned int increment = 4 + ((1023 - fx) * 200L >> 10);
  gain = (gain < 65535 - increment) ? gain + increment : 65535;

  writeCentered((long) x * (gain >> 8) >> 8);
}

//  ****************
//  ***noise gate***
//  ****************
void effect_noiseGate(int fx) {
  static int env16 = 0;
  static int gateGain = 0; // 0..256 = closed..open

  // Mutes the output when the input's loudness drops below a threshold, so
  // hum and hiss between notes go silent. The gate eases open in ~8 ms and
  // closed in ~30 ms rather than switching instantly, which would click.
  // fx sets the threshold: 0..127 counts of envelope.
  int x = readCentered();
  int env = envelopeStep(env16, x, 2, 9);
  if(env > (fx >> 3)) {
    gateGain = min(gateGain + 4, 256);
  } else if(gateGain > 0) {
    gateGain--;
  }
  writeCentered((long) x * gateGain >> 8);
}

//  **************
//  ***low-pass***
//  **************
void effect_lowpass(int fx) {
  static long y16 = 0; // filter output, x16 for precision

  // One-pole low-pass filter -- a tone control. Each sample, the output
  // moves a fraction a/256 of the way toward the input: small a = slow to
  // follow = only low frequencies get through. fx sets a (squared so the
  // dark end gets more of the knob): 3..256, roughly 15 Hz up to no
  // filtering at all.
  int a = min(3 + (int) (((long) fx * fx) >> 12), 256);
  y16 += ((((long) readCentered() << 4) - y16) * a) >> 8;
  writeCentered(y16 >> 4);
}

//  **************
//  ***auto-wah***
//  **************
void effect_autoWah(int fx) {
  static int env16 = 0;
  static long low = 0, band = 0;
  const long DAMPING = 1024; // 1/Q in Q12 (Q = 4) -- lower = more "quack"

  // A resonant band-pass filter (a Chamberlin state-variable filter, in
  // fixed point: 4096 = 1.0) whose centre frequency follows the envelope,
  // so picking harder opens the filter, like rocking a wah pedal forward.
  // fx sets the sensitivity: how far a given loudness moves the filter.
  int x = readCentered();
  int env = envelopeStep(env16, x, 3, 8);

  // f = 2*sin(pi * centre / sampleRate) in Q12: 900 is ~280 Hz and 4000
  // ~1.3 kHz at 8 kHz -- capped there because this filter goes unstable
  // as f approaches 2 - DAMPING
  long f = min(900 + ((long) env * fx >> 7), 4000L);
  low += f * band >> 12;
  long high = x - low - (DAMPING * band >> 12);
  band += f * high >> 12;
  writeCentered(band >> 1); // resonance makes the band-pass output loud
}

//  *************
//  ***stutter***
//  *************
void effect_stutter(int fx) {
  const byte REPEATS = 3;
  static int i = 0;
  static byte pass = 0; // 0 = recording (live), 1..REPEATS = replaying

  // Records a short chunk while passing the live signal through, then
  // replays that chunk REPEATS times, then records the next one -- a
  // glitchy, stuttering repeat. fx sets the chunk length: 50..BUFFER_SIZE
  // samples. Short chunks buzz like a granular synth; long ones stutter.
  int length = 50 + (int) ((long) fx * (BUFFER_SIZE - 50) >> 10);
  if(i >= length) {
    i = 0;
    pass = (pass + 1) % (REPEATS + 1);
  }

  int x = readCentered(); // read even while replaying, to keep timing even
  if(pass == 0) {
    storeSample(i, x);
    writeCentered(x);
  } else {
    writeCentered(loadSample(i));
  }
  i++;
}

//  *************
//  ***reverse***
//  *************
void effect_reverse(int fx) {
  const int HALF = BUFFER_SIZE / 2;
  static int i = 0;
  static bool recordIntoSecondHalf = false;

  // Ping-pongs between the buffer's two halves: one records the live input
  // while the other plays the previous chunk backwards, then they swap.
  // Mixed with the dry signal so notes stay recognisable. Chunks can only
  // be up to half the buffer (~0.1 s), so this is a short, swirly reverse
  // rather than a long backwards swell. fx sets the chunk length:
  // 100..HALF samples.
  int length = 100 + (int) ((long) fx * (HALF - 100) >> 10);
  if(i >= length) {
    i = 0;
    recordIntoSecondHalf = !recordIntoSecondHalf;
  }

  int recordStart = recordIntoSecondHalf ? HALF : 0;
  int playStart = recordIntoSecondHalf ? 0 : HALF;
  int x = readCentered();
  storeSample(recordStart + i, x);
  int reversed = loadSample(playStart + length - 1 - i);
  i++;
  writeCentered((x >> 1) + reversed);
}

struct Effect {
  EffectId id;
  char name[14]; // stored inline (not a pointer) so it lives in flash too
  EffectFn process;
};

// the effect library -- add new effects here as you build them. Order
// doesn't matter since presets reference effects by id, not position.
// Kept in flash (PROGMEM) rather than RAM, which is nearly full -- so it
// has to be read with pgm_read_*(), as findEffect() does.
const Effect effectLibrary[] PROGMEM = {
  { EFFECT_BITCRUSH,    "Bitcrush",    effect_bitcrush },
  { EFFECT_OVERDRIVE,   "Overdrive",   effect_overdrive },
  { EFFECT_SHORT_DELAY, "Short Delay", effect_shortDelay },
  { EFFECT_HELICOPTER,  "Helicopter",  effect_helicopter },
  { EFFECT_RING_MOD,    "Ring Mod",    effect_ringMod },
  { EFFECT_OCTAVE_DOWN, "Octave Down", effect_octaveDown },
  { EFFECT_TREMOLO,       "Tremolo",       effect_tremolo },
  { EFFECT_OCTAVE_UP,     "Octave Up",     effect_octaveUp },
  { EFFECT_FUZZ,          "Fuzz",          effect_fuzz },
  { EFFECT_WAVEFOLDER,    "Wavefolder",    effect_wavefolder },
  { EFFECT_SQUARE_FUZZ,   "Square Fuzz",   effect_squareFuzz },
  { EFFECT_SINE_RING_MOD, "Sine Ring Mod", effect_sineRingMod },
  { EFFECT_ECHO,          "Echo",          effect_echo },
  { EFFECT_SWELL,         "Swell",         effect_swell },
  { EFFECT_NOISE_GATE,    "Noise Gate",    effect_noiseGate },
  { EFFECT_LOWPASS,       "Low-pass",      effect_lowpass },
  { EFFECT_AUTO_WAH,      "Auto-Wah",      effect_autoWah },
  { EFFECT_STUTTER,       "Stutter",       effect_stutter },
  { EFFECT_REVERSE,       "Reverse",       effect_reverse },
};
const int NUM_EFFECTS = sizeof(effectLibrary) / sizeof(effectLibrary[0]);

// which effect is assigned to each of the 6 switch positions -- edit by
// name, no need to count indices into effectLibrary
const EffectId presetForPosition[6] = {
  EFFECT_SWELL,
  EFFECT_BITCRUSH,
  EFFECT_SHORT_DELAY,
  EFFECT_HELICOPTER,
  EFFECT_AUTO_WAH,
  EFFECT_TREMOLO,
};

EffectFn activeEffect[6];

EffectFn findEffect(EffectId id) {
  for(int i = 0; i < NUM_EFFECTS; i++) {
    if((EffectId) pgm_read_word(&effectLibrary[i].id) == id) {
      return (EffectFn) pgm_read_ptr(&effectLibrary[i].process);
    }
  }
  return NULL; // only happens if presetForPosition has a typo/duplicate bug
}

int readSwitchPosition(); // 0-5

void setup() {
  setupIO();
  otaBegin();

  for(int i = 0; i < 6; i++) {
    activeEffect[i] = findEffect(presetForPosition[i]);
  }
}

void loop() {
  checkForOTAResetTrigger();
  int position = readSwitchPosition();
  int fx = analogRead(3);
  activeEffect[position](fx);
}

int readSwitchPosition() {
  //read the rotary switch and determine which of the 6 positions it's on

  // low-pass filter the raw switch reading so single-sample ADC noise
  // doesn't jitter the value on its own
  static int rawFiltered = 0;
  rawFiltered += (analogRead(2) - rawFiltered) / 4;

  // The 6 switch positions are NOT evenly spaced in raw ADC counts (measured:
  // 502, 603, 680, 766, 836, 929), so a fixed divisor doesn't reliably
  // separate them -- thresholds below are set at the midpoint between each
  // pair of measured positions instead.
  if(rawFiltered < 552) {
    return 0;
  } else if(rawFiltered < 641) {
    return 1;
  } else if(rawFiltered < 723) {
    return 2;
  } else if(rawFiltered < 801) {
    return 3;
  } else if(rawFiltered < 882) {
    return 4;
  } else {
    return 5;
  }
}
