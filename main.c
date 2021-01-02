#include <msp430.h> 
#include <stdint.h>
#include <stdbool.h>

// Pin definitions
#define P1_IPIN_SENSE 0x20
#define P1_OPIN_RED 0x80
#define P1_OPIN_GREEN 0x40
#define ALL_P1_OUTPUTS_MASK (P1_OPIN_RED | P1_OPIN_GREEN)

// Getter macros
#define GET_PINS(port, mask) (port & mask)
#define GET_SENSE() GET_PINS(P1IN, P1_IPIN_SENSE)

// Setter macros
#define SET_OUTPUT(port, mask, on) (on ? (port |= mask) : (port &= ~mask))
#define SET_RED_LED(on) SET_OUTPUT(P1OUT, P1_OPIN_RED, on)
#define SET_GREEN_LED(on) SET_OUTPUT(P1OUT, P1_OPIN_GREEN, on)
// All control IO are on port 1
#define ALL_OUTPUTS_OFF() P1OUT &= ~ALL_P1_OUTPUTS_MASK

// Maximum number of cleared samples before shutting off all LEDs
#define MAX_CLEAR_COUNT 3000

// Maximum number of blocked sampled before activating
#define MAX_BLOCKED_COUNT 5

// True when cleared signal received in this cycle
bool gSense = false;

// Set to true once we are inactive
bool gInactive = false;

// Number of consecutive cleared cycles up to MAX_CLEAR_COUNT
int16_t gClearCount = 0;

// Number of consecutive blocked cycles in inactive state up to MAX_BLOCKED_COUNT
int16_t gBlockedCount = 0;

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
    // Timer 0 interrupt
    TA0CTL = TASSEL_1 | MC_1;   // ACLK, up mode
    CCR0 = 399;                 // Check every 20ms to 100ms (typ=33ms)
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

//Timer ISR
#pragma vector = TIMER0_A0_VECTOR
__interrupt void Timer_A0(void)
{
    // Check if we are cleared or not
    if (gSense)
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
    // Reset flag
    gSense = false;
}

// Sense ISR
#pragma vector=PORT1_VECTOR
__interrupt void Port_1(void)
{
  if (P1IFG & P1_IPIN_SENSE)
  {
      // low->high sense
      P1IFG &= ~P1_IPIN_SENSE; // Clear interrupt flag
      gSense = true;
  }
}
