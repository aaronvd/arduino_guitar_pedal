#include <Arduino.h>
#include "dsp.h"

#define BUFFER_SIZE 2000

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
};

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

struct Effect {
  EffectId id;
  const char* name;
  EffectFn process;
};

// the effect library -- add new effects here as you build them. Order
// doesn't matter since presets reference effects by id, not position.
const Effect effectLibrary[] = {
  { EFFECT_BITCRUSH,    "Bitcrush",    effect_bitcrush },
  { EFFECT_OVERDRIVE,   "Overdrive",   effect_overdrive },
  { EFFECT_SHORT_DELAY, "Short Delay", effect_shortDelay },
  { EFFECT_HELICOPTER,  "Helicopter",  effect_helicopter },
  { EFFECT_RING_MOD,    "Ring Mod",    effect_ringMod },
  { EFFECT_OCTAVE_DOWN, "Octave Down", effect_octaveDown },
};
const int NUM_EFFECTS = sizeof(effectLibrary) / sizeof(effectLibrary[0]);

// which effect is assigned to each of the 6 switch positions -- edit by
// name, no need to count indices into effectLibrary
const EffectId presetForPosition[6] = {
  EFFECT_BITCRUSH,
  EFFECT_OVERDRIVE,
  EFFECT_SHORT_DELAY,
  EFFECT_HELICOPTER,
  EFFECT_RING_MOD,
  EFFECT_OCTAVE_DOWN,
};

EffectFn activeEffect[6];

EffectFn findEffect(EffectId id) {
  for(int i = 0; i < NUM_EFFECTS; i++) {
    if(effectLibrary[i].id == id) return effectLibrary[i].process;
  }
  return NULL; // only happens if presetForPosition has a typo/duplicate bug
}

int readSwitchPosition(); // 0-5

void setup() {
  setupIO();

  for(int i = 0; i < 6; i++) {
    activeEffect[i] = findEffect(presetForPosition[i]);
  }
}

void loop() {
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
