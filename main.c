#include <msp430.h> 
#include <stdint.h>
#include <stdbool.h>
// VLO library found on TI website
// Application Report: https://www.ti.com/lit/an/slaa340a/slaa340a.pdf
// Library download location: http://www.ti.com/lit/zip/slaa340
// Refer to the copyright notice in VLO_Library.asm.
// This library allows me to use the Very Low Oscillator (VLO) with greater timing precision.
#include "VLO_Library.h"

void measureVloFrequency();

// Define the following to use a more simplified VLO frequency calculation in measureVloFrequency
// #define USE_SIMPLE_VLO_FREQUENCY_CALC

// Pin definitions
#define P1_IPIN_SENSE 0x20
#define P1_OPIN_RED 0x81 // Pin 1.0 for debug, pin 1.7 in application
#define P1_OPIN_GREEN 0x40
#define ALL_P1_OUTPUTS_MASK (P1_OPIN_RED | P1_OPIN_GREEN)

// Getter macros
#define GET_PINS(port, mask) (port & mask)
#define GET_SENSE() GET_PINS(P1IN, P1_IPIN_SENSE)

// Setter macros
#define SET_OUTPUT(port, mask, on) (on ? (port |= mask) : (port &= ~mask))
#define TOGGLE_OUTPUT(port, mask) (port ^= mask)
#define SET_RED_LED(on) SET_OUTPUT(P1OUT, P1_OPIN_RED, on)
#define SET_GREEN_LED(on) SET_OUTPUT(P1OUT, P1_OPIN_GREEN, on)
#define TOGGLE_LEDS() TOGGLE_OUTPUT(P1OUT, P1_OPIN_RED | P1_OPIN_GREEN)
// All control IO are on port 1
#define ALL_OUTPUTS_OFF() P1OUT &= ~ALL_P1_OUTPUTS_MASK

// Expected amount of time between each sense
#define EXPECTED_SENSE_PERIOD_S 0.007
// The number of senses for each timer cycle
#define EXPECTED_SENSES_PER_CYCLE 2
#define TIMER_CYCLE_PERIOD_S (EXPECTED_SENSE_PERIOD_S * EXPECTED_SENSES_PER_CYCLE)
#define TIMER_CYCLE_FREQUENCY (uint16_t)(1.0 / TIMER_CYCLE_PERIOD_S)
// Number of bins we have for storing the last several samples
#define SENSE_BIN_COUNT 10
// The clear threshold before marking the sensor as cleared
#define SENSE_THRESHOLD_PERCENT 90
#define SENSE_THRESHOLD_COUNT (int16_t)(SENSE_BIN_COUNT * EXPECTED_SENSES_PER_CYCLE * SENSE_THRESHOLD_PERCENT / 100.0 + 0.5)

// Number of seconds clear is sensed before deactivating
#define IDLE_DEACTIVATION_TIME_S 60
#define MAX_CLEAR_COUNT (int16_t)(IDLE_DEACTIVATION_TIME_S / TIMER_CYCLE_PERIOD_S + 0.5)

// Maximum number of VLO measurements before error is flagged
#define MAX_VLO_MEASUREMENTS 20
// VLO count +/- tolerance from measurement to measurement
#define VLO_MEASUREMENT_COUNT_TOL 3
// Min/max VLO frequencies from specs
#define MIN_VLO_FREQUENCY 4000
#define MAX_VLO_FREQUENCY 20000
// The clock frequency the VLO count is relative to (coded into VLO_Library.asm)
#define VLO_COUNT_REL_CLOCK_FREQUENCY 8000000
// Computed min and max VLO counts
#define MIN_VLO_COUNT (VLO_COUNT_REL_CLOCK_FREQUENCY / MAX_VLO_FREQUENCY)
#define MAX_VLO_COUNT (VLO_COUNT_REL_CLOCK_FREQUENCY / MIN_VLO_FREQUENCY)

// Maximum number of blocked cycles before activating
#define MAX_BLOCKED_COUNT 14

// Saves the measured frequency of the VLO
uint16_t gVloFrequency = 0;

// Number of times the cleared signal is received
int16_t gSenseCount = 0;

// Set to true once we are inactive
bool gInactive = false;

// Number of consecutive cleared cycles up to MAX_CLEAR_COUNT
int16_t gClearCount = 0;

// Number of consecutive blocked cycles in inactive state up to MAX_BLOCKED_COUNT
int16_t gBlockedCount = 0;

// Number of sense signals received in the last several cycles
int16_t gSenseBins[SENSE_BIN_COUNT] = {0};
// The current bin to fill
uint8_t gSenseBinPtr = 0;

/**
 * main.c
 */
int main(void)
{
    WDTCTL = WDTPW | WDTHOLD;   // stop watchdog timer
    BCSCTL1 = CALBC1_1MHZ;      // Set range   DCOCTL = CALDCO_1MHZ;
    BCSCTL2 &= ~(DIVS_3);       // SMCLK = DCO = 1MHz
    // Set ACLK to internal VLO (4-20 kHz, typ=12kHz)
    BCSCTL3 &= ~LFXT1S0;
    BCSCTL3 |= LFXT1S1;
    BCSCTL1 &= ~XTS;
    // All P1 setup for GPIO
    P1SEL = 0;
    P1SEL2 = 0;
    // Nothing on P2
    P2SEL = 0;
    P2SEL2 = 0;
    // Setup P1 outputs
    P1OUT = 0x00;
    P1DIR = ALL_P1_OUTPUTS_MASK;
    // 1.5 low->high interrupt
    P1IE = P1_IPIN_SENSE;
    P1IES = 0;
    // Measure the frequency of VLO and store into gVloFrequency
    measureVloFrequency();
    // Timer 0 interrupt
    TA0CTL = TASSEL_1 | MC_1;   // ACLK, up mode
    uint16_t timerValue = gVloFrequency / TIMER_CYCLE_FREQUENCY - 1;
    CCR0 = timerValue;
    CCTL0 = CCIE;               // Enable interrupt

    __enable_interrupt();

    while(1)
    {
        // Interrupt driven from here on out
        LPM3;
    }
	
    // Unreachable
	return 0;
}

void measureVloFrequency()
{
    // It seems like it takes some time for the VLO to power up, so give it a moment
    // 100 ms is probably overkill, but this is only used for startup cal :)
    __delay_cycles(100000);
    // Measure and calculate the VLO frequency
    int vloValue = 0;
#ifdef USE_SIMPLE_VLO_FREQUENCY_CALC
    vloValue = TI_measureVLO();
#else
    // This will make sure we get at last 2 consistent measurements before moving on
    // It's probably more overkill, but it doesn't hurt anything besides the need for more program space
    int lastVloValue = 0;
    bool isValid = false;
    uint16_t calculationCount = 0;
    do
    {
        lastVloValue = vloValue;
        vloValue = TI_measureVLO();
        isValid = (vloValue >= MIN_VLO_COUNT &&
                   vloValue <= MAX_VLO_COUNT &&
                   vloValue <= lastVloValue + VLO_MEASUREMENT_COUNT_TOL &&
                   vloValue >= lastVloValue - VLO_MEASUREMENT_COUNT_TOL);
        ++calculationCount;
    } while (calculationCount < MAX_VLO_MEASUREMENTS && !isValid);
    // Check if error was detected
    if (!isValid)
    {
        // Flash red/green then go to sleep forever
        SET_RED_LED(true);
        SET_GREEN_LED(false);
        uint8_t i = 20;
        for (; i > 0; --i)
        {
            __delay_cycles(500000);
            TOGGLE_LEDS();
        }
        ALL_OUTPUTS_OFF();
        LPM4; // Stuck here forever
        while(1);
    }
#endif
    // Finally, compute the frequency from the count
    gVloFrequency = VLO_COUNT_REL_CLOCK_FREQUENCY / vloValue;
}

//Timer ISR
#pragma vector = TIMER0_A0_VECTOR
__interrupt void Timer_A0(void)
{
    // Sum up the number of samples in the last several cycles
    gSenseBins[gSenseBinPtr++] = gSenseCount;
    gSenseCount = 0;
    if (gSenseBinPtr >= SENSE_BIN_COUNT)
    {
        gSenseBinPtr = 0;
    }
    int16_t totalSenseCount = 0;
    int8_t i = SENSE_BIN_COUNT;
    int16_t *pBin = &gSenseBins[0];
    for (; i > 0; --i, pBin++)
    {
        totalSenseCount += *pBin;
    }
    // Check if we are cleared or not
    if (totalSenseCount >= SENSE_THRESHOLD_COUNT)
    {
        // Sensor cleared
        if (gClearCount >= MAX_CLEAR_COUNT)
        {
            // 1 or more minutes have passed since sensors have been cleared
            // Turn off both LEDs
            ALL_OUTPUTS_OFF();
            gInactive = true;
        }
        else
        {
            ++gClearCount;
            SET_GREEN_LED(true);
            SET_RED_LED(false);
            gInactive = false;
        }
        gBlockedCount = 0;
    }
    else
    {
        // Sensor blocked
        if (!gInactive || gBlockedCount >= MAX_BLOCKED_COUNT)
        {
            SET_RED_LED(true);
            SET_GREEN_LED(false);
            gClearCount = 0;
            gInactive = false;
        }
        else
        {
            // Inactive, and we haven't reached the number of blocked counts to reactivate yet
            ++gBlockedCount;
        }
    }
}

// Sense ISR
#pragma vector=PORT1_VECTOR
__interrupt void Port_1(void)
{
  if (P1IFG & P1_IPIN_SENSE)
  {
      // low->high sense
      P1IFG &= ~P1_IPIN_SENSE; // Clear interrupt flag
      ++gSenseCount;
  }
}
