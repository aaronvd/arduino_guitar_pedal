#include "dsp.h"

#define BUFFER_SIZE 2000

// create an array for the delay
byte array[BUFFER_SIZE];

//define variables
int j;
int fx;
int mode;
int value50;
int value300;
int value10000;
int delayed;
byte octavePrev;
boolean octaveState;

void setup() {
  setupIO();

  //set initial values
  j = 50;
  value50 = 50;
  value300 = 300;
  value10000 = 1000;
  delayed = 0;
  octavePrev = 128;
  octaveState = false;
}

void loop() {
  
    //check status of the effect potentiometer and rotary switch
    readKnobs();
  
    // *************
    // ***bitcrush**
    // *************
    if(mode == 6){
      value300 = 1 + ((float) fx / (float) 3);        
      if(delayed > value300) {  
        byte input = analogRead(left);
        input = (input >> 6 << 6);
        output(left, input);
        delayed = 0;
       } 
       delayed++;

    }
    
    
    // ***************
    // ***Overdrive***
    // ***************
    if(mode == 7){
      value50 = 1 + ((float) fx / (float) 20);    
      byte input = analogRead(left);
      input = (input * value50); 
      output(left, input);

    }


    //  *************************
    //  ***short crunchy delay***
    //  *************************

    if(mode == 9){
      for(int i = 0; i < BUFFER_SIZE; i ++) { // set up a loop
        //array[i] = array[i] + array[i - 1]; //removes noise and some delay
        output(left, array[i]);
        array[i] = analogRead(left);
      }
    }


    //  **********************
    //  ***clean helicopter***
    //  **********************
    if(mode == 10){
      value10000 = fx * 10;  
      if(delayed > value10000) { 
        for(int i = 0; i < BUFFER_SIZE; i ++) { // set up a loop
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
    if(mode == 11){
     value50 = 1 + ((float) fx / (float) 20);    
     byte input = analogRead(left);
     input = (input * j);
     output(left, input);
     
     j = j - 1;
     if (j <= 0) {
       j = value50;
     }

    }
    

    //  *******************
    //  ***octave down***
    //  *******************
    if(mode == 12){
      byte input = analogRead(left);

      // toggle a flip-flop each time the signal crosses the center point
      // going upward -- flipping every other cycle halves the fundamental frequency
      if(octavePrev < 128 && input >= 128) {
        octaveState = !octaveState;
      }
      octavePrev = input;

      byte level = 1 + ((float) fx / (float) 4);
      output(left, octaveState ? level : 0);
    }

}

void readKnobs(){
  //read the rotary switch
  //and determine which effect is selected

  // low-pass filter the raw switch reading so single-sample ADC noise
  // doesn't jitter the value on its own
  static int modeRawFiltered = 0;
  modeRawFiltered += (analogRead(2) - modeRawFiltered) / 4;

  // The 6 switch positions are NOT evenly spaced in raw ADC counts (measured:
  // 502, 603, 680, 766, 836, 929 for modes 6, 7, 9, 10, 11, 12), so a fixed
  // divisor doesn't reliably separate them -- thresholds below are set at
  // the midpoint between each pair of measured positions instead.
  if(modeRawFiltered < 552) {
    mode = 6;
  } else if(modeRawFiltered < 641) {
    mode = 7;
  } else if(modeRawFiltered < 723) {
    mode = 9;
  } else if(modeRawFiltered < 801) {
    mode = 10;
  } else if(modeRawFiltered < 882) {
    mode = 11;
  } else {
    mode = 12;
  }
    
  //reads the effects pot to adjust
  //the intensity of the effects above
  fx = analogRead(3);
}
