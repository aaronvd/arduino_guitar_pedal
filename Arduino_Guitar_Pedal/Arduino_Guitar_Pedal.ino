#include "dsp.h"

// Uno has 2048 bytes of SRAM total. array[2000] alone worked fine before,
// but the temporary Serial debug output below needs ~200 bytes of RX/TX
// buffers, which doesn't fit alongside the full 2000-byte buffer. Trimmed
// to leave real headroom for the stack while debugging (1850 only left
// -18 bytes -- an outright overflow, not just a tight fit).
#define BUFFER_SIZE 1600

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
int lastMode = -1; // DEBUG: for detecting mode changes

void setup() {
  setupIO();
  Serial.begin(9600); // DEBUG

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
  //dividing by 75 ensures proper discrete values
  //for if statements above
  mode = analogRead(2);
  mode = mode / 75;
    
  //reads the effects pot to adjust
  //the intensity of the effects above
  fx = analogRead(3);

  // DEBUG: report mode/fx only when mode changes, so it doesn't
  // spam the audio loop or slow down sample-rate-sensitive effects
  if(mode != lastMode) {
    Serial.print("mode: ");
    Serial.print(mode);
    Serial.print("  fx: ");
    Serial.println(fx);
    lastMode = mode;
  }
}
