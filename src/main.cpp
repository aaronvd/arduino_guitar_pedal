#include <Arduino.h>
#include "dsp.h"
#include "ota_listener.h"

// shrunk from 2000 -- SRAM is tight enough on the Uno (2048 bytes total)
// that this buffer alone was already overflowing it before the OTA Serial
// listener was added; this leaves headroom for that plus the stack. Effect
// of the smaller buffer: the Short Delay/Helicopter effects' max delay
// length is proportionately shorter than before.
#define BUFFER_SIZE 1600

// shared scratch buffer for time-based effects (short delay, helicopter).
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
};
const int NUM_EFFECTS = sizeof(effectLibrary) / sizeof(effectLibrary[0]);

// which effect is assigned to each of the 6 switch positions -- edit by
// name, no need to count indices into effectLibrary
const EffectId presetForPosition[6] = {
  EFFECT_SHORT_DELAY,
  EFFECT_HELICOPTER,
  EFFECT_BITCRUSH,
  EFFECT_SQUARE_FUZZ,
  EFFECT_SINE_RING_MOD,
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
