//
// HousePower MiniBMS Cell Module
// OpenSource Replacement Firmware
// Copyright 2021 Martin Bartosch
// 
// See LICENSE file.
//

#include <Arduino.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/sleep.h>

//////////////////////////////////////////////////////////////////////////
// If the CALIBRATION_MODE flag is defined the firmware is stripped down to the following 
// functionality:
// - disable all outputs
// - the firmware samples Vcc every 2 seconds and prints the current value to the serial output
// NOTE: only enable this for initial calibration of the module

// #define CALIBRATION_MODE

// HOW TO CALIBRATE:
// 1. attach RXD of a terminal program on the host computer (9600 Baud 8N1) to PB2 (pin 7) of the ATTiny
// 2. compile and deploy this source with CALIBRATION_MODE enabled
// 3. attach stable power supply to cell board Vcc, voltage should be about 3.1 - 3.4 V
// 4. WAIT AT LEAST 8 SECONDS BEFORE TAKING MEASUREMENTS
// 5. attach a precise volt meter as close as possible to Vcc/GND of cell board
// 6. take voltage reading on volt meter, note as "voltage metered"
// 7. check value of "Vcc (uncalibrated)" output on the terminal console, note as "voltage software"
// 8. insert these two values (measured in mV) in the below constants calibration_voltage_metered and calibration_voltage_software
// 9. disable CALIBRATION_MODE
// 10. compile and deploy the calibrated source to this particular cell module

// NOTE: Calibration is for one particular ATTiny processor and must be repeated for each module to be deployed!

// enable debugging console output on PB2/Pin 7 (may require ATTTiny85 due to flash size requirements)
// #define DEBUG

//////////////////////////////////////////////////////////////////////////
// USER CONFIGURATION
// CHANGE THESE VALUES ACCORDING TO THE CALIBRATION RESULTS

// Voltage (mV) measured using a good volt meter
const unsigned long calibration_voltage_metered = 3200;

// Voltage (mV) reported in CALIBRATION_MODE (Vcc uncalibrated output)
const unsigned long calibration_voltage_software = 3200;

// ONLY CHANGE THE BELOW VALUES IF YOU KNOW WHAT YOU ARE DOING
// cell module voltage thresholds (in mV)
const int c_LVoltage_engage    = 2900L;
const int c_LVoltage_disengage = 2950L;
const int c_HVoltage_engage    = 3600L;
const int c_HVoltage_disengage = 3550L;

// shunting thresholds
const int c_ShuntVoltage_engage    = 3500L;
const int c_ShuntVoltage_disengage = 3450L;

// number of voltage measurements to average
const byte c_MovingAverageWindow = 5;

// number of consecutive measurements (~1 s cycle time) a new state needs 
// to be stable before beeing committed
const unsigned int c_StateSettleTime = 3;

// special handling/notification if LVC or HVC happened within this 
// time interval (~30 minutes)
const unsigned int c_RecentCutOffDuration = 30 * 60;

// END OF USER CONFIGURATION
//////////////////////////////////////////////////////////////////////////

// custom voltage calibration
const unsigned long calibration_factor_custom = ((1024UL * 11 * 1000) / (10 * calibration_voltage_software)) * calibration_voltage_metered;
// default value used for calibration
const unsigned long calibration_factor_default = ((1024UL * 11 * 1000) / (10 * 3200L)) * 3200UL;

// Averaged cell voltage, start with a sensible and likely value
// (will be averaged over c_MovingAverageWindow values)
long cellvoltage = 3200;

//////////////////////////////////////////////////////////////////////////
// Hardware setup
// Pin assignments
#define PIN_LED   PB1
#define PIN_LOOP  PB3
#define PIN_SHUNT PB4
#define PIN_AUX   PB2

// Convenience macros
#define LED_ON  PORTB |=  (1 << PIN_LED)
#define LED_OFF PORTB &= ~(1 << PIN_LED)

#define SHUNT_ON  PORTB |=  (1 << PIN_SHUNT)
#define SHUNT_OFF PORTB &= ~(1 << PIN_SHUNT)

#define LOOP_CLOSE PORTB |=  (1 << PIN_LOOP)
#define LOOP_OPEN  PORTB &= ~(1 << PIN_LOOP)

#if defined DEBUG || defined CALIBRATION_MODE
  #define PIN_TX        PIN_AUX
  #include <SoftwareSerial.h>
  SoftwareSerial mySerial(-1, PIN_TX);  //rx, tx
  #define debugln(x) mySerial.println(x)
  #define debug(x) mySerial.print(x)
#endif

//////////////////////////////////////////////////////////////////////////
// Commonly used watchdog timeouts
#define WD_TIMEOUT_64ms _BV(WDP1)
#define WD_TIMEOUT_125ms (_BV(WDP1) | _BV(WDP0))
#define WD_TIMEOUT_250ms _BV(WDP2)
#define WD_TIMEOUT_500ms (_BV(WDP2) | _BV(WDP0))
#define WD_TIMEOUT_1000ms (_BV(WDP2) | _BV(WDP1))
#define WD_TIMEOUT_2000ms (_BV(WDP2) | _BV(WDP1) | _BV(WDP0))

//////////////////////////////////////////////////////////////////////////
// Voltage measurement
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
#define ADMUX_VCCWRT1V1 (_BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1))
#elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
#define ADMUX_VCCWRT1V1 (_BV(MUX5) | _BV(MUX0))
#elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
#define ADMUX_VCCWRT1V1 (_BV(MUX3) | _BV(MUX2))
#else
#define ADMUX_VCCWRT1V1 (_BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1))
#endif  

long readADC() {
  // Read 1.1V reference against AVcc
  // set the reference to Vcc and the measurement to the internal 1.1V reference
  if (ADMUX != ADMUX_VCCWRT1V1)
  {
    ADMUX = ADMUX_VCCWRT1V1;

    // Bandgap reference start-up time: max 70us
    // Wait for Vref to settle.
    delayMicroseconds(350); 
  }

  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA,ADSC)) {}; // measuring

  uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH  
  uint8_t high = ADCH; // unlocks both

  long result = (high << 8) | low;
  return(result);
}

// calculate Vcc voltage using the given calibration factor
// returns (calibrated) voltage in mV
long readVcc(long calibration_factor) {
  // Calculate Vcc (in mV)  
  return(calibration_factor / readADC());
}

long avg_buffer[c_MovingAverageWindow];
byte avg_index = 0;

// calculate moving average
long moving_average(long val) {
  avg_buffer[avg_index++] = val;
  avg_index = avg_index % c_MovingAverageWindow;

  long sum = 0;
  for (byte ii = 0; ii < c_MovingAverageWindow; ii++) {
    sum += avg_buffer[ii];
  }
  return sum / c_MovingAverageWindow;
}

//////////////////////////////////////////////////////////////////////////
// Business logic
//////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
// Cell state calculation

// Possible cell states
enum { 
  e_CellInvalid = 0, 
  e_CellNorm, 
  e_CellLVC, 
  e_CellHVC, 
  e_TOTALCELLSTATES };

#ifdef DEBUG
const char CellStateDescription[e_TOTALCELLSTATES][6] = { 
  "n/a", 
  "OK", 
  "LVC", 
  "HVC" };
#endif

// Current cell state
byte cellstate = e_CellInvalid;

// Tentative new cell state
byte cellstate_pending = e_CellInvalid;

// cycle age of the pending new state
unsigned int cellstate_pending_age = 0;

// shunting state
bool shunting = false;
bool shunting_pending = false;
// cycle age of the pending new shunting state
unsigned int shunting_pending_age = 0;

void determine_cellstate() {
  // use previous state as default
  byte cellstate_new = cellstate;
  bool shunting_new = shunting;

  // determine new shunting state
  // start shunting
  if (!shunting && (cellvoltage > c_ShuntVoltage_engage)) {
    shunting_new = true;
  }
  // stop shunting
  if (shunting && (cellvoltage < c_ShuntVoltage_disengage)) {
    shunting_new = false;
  }

  // determine new cell state
  // coming from a HVC we need to get below the lower HVC threshold to leave the HVC state
  if ((cellstate == e_CellHVC) && (cellvoltage < c_HVoltage_disengage))
    cellstate_new = e_CellNorm;

  // coming from a LVC reset this condition when passing the upper LVC threshold
  if ((cellstate == e_CellLVC) && (cellvoltage > c_LVoltage_disengage))
    cellstate_new = e_CellNorm;

  // special case: after bootup the cell state is invalid, make sure we leave that state
  if ((cellstate == e_CellInvalid) && (cellvoltage > c_LVoltage_engage) && (cellvoltage < c_HVoltage_engage))
    cellstate_new = e_CellNorm;

  if ((cellvoltage > c_LVoltage_disengage) && (cellvoltage < c_HVoltage_disengage))
    cellstate_new = e_CellNorm; // final decision

  if (cellvoltage >= c_HVoltage_engage)
    cellstate_new = e_CellHVC; // final decision

  if (cellvoltage <= c_LVoltage_engage)
    cellstate_new = e_CellLVC; // final decision


  if (shunting_pending != shunting_new) {
    shunting_pending = shunting_new;
    shunting_pending_age = 0;    
  } else {
    if (shunting_pending_age > c_StateSettleTime) {
      shunting = shunting_new;
    } else {
      shunting_pending_age++;
    }
  }


  if (cellstate_new == e_CellInvalid) // should not happen
    return;

  if (cellstate_pending != cellstate_new) {
    cellstate_pending = cellstate_new;
    cellstate_pending_age = 0;    
  } else {
    if (cellstate_pending_age > c_StateSettleTime) {
      cellstate = cellstate_new;
    } else {
      cellstate_pending_age++;
    }
  }
}

// cycle age of last HVC or LVC event
unsigned int last_cutoff_age = 0;
// special value indicating that no cutoff has happened recently
const unsigned int c_NoCutoffEvent = 0xffff;

//////////////////////////////////////////////////////////////////////////

// Watchdog interrupt (executed when watchdog times out)
ISR (WDT_vect) {
}

// Enter deep sleep for the specified duration
// duration is the correct bit combination of WDP3, WDP2, WDP1, WDP0
void deep_sleep(byte duration) {
  // clear unwanted bits from duration
  duration &= 0b00100111;

  // disable ADC
  ADCSRA &= ~_BV(ADEN); 

  // set up watchdog timer
  wdt_reset(); // Reset Watchdog Timer
  MCUSR &= ~_BV(WDRF);                       // clear watchdog Reset Flag

  WDTCR = _BV(WDCE) | _BV(WDE);              // watchdog Change Enable, Enable
  WDTCR = _BV(WDIE) | duration; // watchdog Interrupt Enable, set timeout
  
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);  
  noInterrupts();      // disable interrupts to assure deterministic execution
  sleep_enable();
  sleep_bod_disable(); // disable brown-out detection
  interrupts();        // enable interrupts
  sleep_cpu();         // go to sleep
  // ZZZZZZ....
  sleep_disable(); // wake up

  // re-enable ADC
  ADCSRA |= _BV(ADEN);
}

void setup() {
#if defined DEBUG || defined CALIBRATION_MODE
  // serial debugging
  mySerial.begin(9600);
  debugln();
#endif

#ifdef CALIBRATION_MODE
  debugln("Calibration mode");

  debug("Calibration factor default: ");
  debugln(calibration_factor_default);
  debug("Calibration factor custom: ");
  debugln(calibration_factor_custom);
#endif

  // initialize Port B: configure all pins as OUTPUT
  DDRB  = 0b00011111;

  // set all outputs to LOW (LED off, Shunt off, Loop open)
  PORTB = 0b00000000;

  // initialize moving average with nominal value
  for (byte ii = 0; ii < c_MovingAverageWindow; ii++) {
    avg_buffer[ii] = 3200;
  }

  last_cutoff_age = 0;
}


void loop() {
#ifndef CALIBRATION_MODE
  // normal mode
  // xor mask for LED (used to invert the LED if recent LVC/HVC has happend)
  bool invert_led = false;

  if (last_cutoff_age != c_NoCutoffEvent)
    last_cutoff_age++;

  // cell measurement shall be done without any loads
  LED_OFF;
  SHUNT_OFF;

  delayMicroseconds(200);
  cellvoltage = moving_average(readVcc(calibration_factor_custom));

  determine_cellstate();

  switch (cellstate) {
  case e_CellLVC:
    LOOP_OPEN;
    LED_OFF;
    SHUNT_OFF;
    last_cutoff_age = 0;
    deep_sleep(WD_TIMEOUT_1000ms);
    break;
  case e_CellNorm:
    if (last_cutoff_age < c_RecentCutOffDuration) {
      // recent HVC/LVC happened
      invert_led = true;
    } else {
      // normal state has been stable for some time
      last_cutoff_age = c_NoCutoffEvent;
    }

    LOOP_CLOSE;

    if (! shunting) {
      // normal cell state
      SHUNT_OFF;

      if (! invert_led)
        LED_ON;
      else
       LED_OFF;
      delay(20);
      if (! invert_led)
        LED_OFF;
      else
        LED_ON;
    
      deep_sleep(WD_TIMEOUT_1000ms);
    } else {
      // shunting, but no HVC yet
      SHUNT_ON;

      LED_OFF;
      delay(500);
      LED_ON;
      delay(500);

      SHUNT_OFF;

      delay(100);
      LED_OFF;
    }
    break;
  case e_CellHVC:
    LOOP_OPEN;
    SHUNT_ON;

    last_cutoff_age = 0;
    for (byte ii = 0; ii < 10; ii++) {
      LED_OFF;
      delay(50);
      LED_ON;
      delay(50);
    }
    SHUNT_OFF;
    delay(100);
    break;
  case e_CellInvalid:
    // fall-through intended
  default:
    LOOP_CLOSE;
    SHUNT_OFF;
    for (byte ii = 0; ii < 3; ii++) {
      LED_ON;
      delay(166);
      LED_OFF;
      delay(166);
    }
  }

  #ifdef DEBUG
    debug("Vcc: ");
    debug(cellvoltage);
    debug(" [Cell curr: ");
    debug(CellStateDescription[cellstate]);
    debug(" pend: ");
    debug(CellStateDescription[cellstate_pending]);
    debug(" age: ");
    debug(cellstate_pending_age);
    debug("] [Shunt curr: ");
    debug(shunting);
    debug(" pend: ");
    debug(shunting_pending);
    debug(" age: ");
    debug(shunting_pending_age);
    debug("] cutoffage: ");
    debug(last_cutoff_age);
    debugln();
  #endif


#else
  // calibration mode
  delayMicroseconds(200);
  long adc_value = moving_average(readADC());
  debug("Vcc (uncalibrated): ");
  debug(calibration_factor_default / adc_value);
  debug(" Vcc (calibrated): ");
  debug(calibration_factor_custom / adc_value);
  debug(" adc averaged value: ");
  debug(adc_value);
  debugln("");
  // just show a hint that something's going on, should 
  // not cause too much voltage drop to confuse an external volt meter
  LED_ON;
  delay(20);
  LED_OFF;

  // initialize Port B: configure all pins as INPUT to save power
  DDRB  = 0b00000000;
  // low power state for 2 s, should be enough time for external volt meter to settle
  deep_sleep(WD_TIMEOUT_2000ms);
  // initialize Port B: configure all pins as INPUT
  DDRB  = 0b00011111;

#endif
}
