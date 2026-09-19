#include "dsp.h"
#include "timers.h"
#include "Arduino.h"

void setupIO() {
  // prepare left
  waveformGenerationMode(3, fastPWM);
  timerPrescale(3, 1);
  // analogWrite(pin, 0) would call digitalWrite() under the hood on modern
  // cores, which disables the pin's PWM compare output entirely -- leaving
  // our later direct OCR2B/OCR2A writes in output() with no effect on the
  // physical pin. A non-zero value forces analogWrite() down the path that
  // actually enables PWM, which then stays enabled since we never call
  // analogWrite()/digitalWrite() on these pins again.
  analogWrite(3, 1);
  analogWrite(11, 1);

  // prepare right
  waveformGenerationMode(5, fastPWM);
  timerPrescale(5, 1);
  analogWrite(5, 1);
  analogWrite(6, 1);

  // faster input
  analogReference(INTERNAL);
  analogPrescale(analogPrescale32);
}
  
void output(int channel, short value) {
  if(channel == left) {
    pwm3 = value >> 2;
    pwm11 = (value & B11) << 6;
  } else if(channel == right) {
    pwm5 = value >> 2;
    pwm6 = (value & B11) << 6;
  }
}
