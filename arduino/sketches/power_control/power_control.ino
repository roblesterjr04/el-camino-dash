#include <avr/io.h>
#include <avr/sleep.h>
#include <util/delay.h>

// Pin definitions (using PB numbers)
#define PI_POWER    PB0   // Pin 5 — controls U3 via 2N3904
#define PI_SIGNAL   PB4   // Pin 3 — tells Pi to shutdown
#define IGN_SENSE   1     // ADC1, Pin 7 — reads ignition voltage
#define PI_CONFIRM  3     // ADC3, Pin 2 — Pi confirms shutdown

// ADC thresholds
// 12V ignition through 10k/3.3k divider = ~2.98V
// ATtiny ADC is 10-bit, Vref = 5V, so 2.98V = ~610 counts
#define IGN_ON_THRESHOLD   580   // ~2.85V — ignition on
#define IGN_OFF_THRESHOLD  500   // ~2.44V — ignition off

bool piRunning = false;

// ─── ADC SETUP ────────────────────────────────────────────
void adcInit() {
  ADMUX  = (1 << ADLAR);          // left adjust, Vcc as reference
  ADCSRA = (1 << ADEN)            // enable ADC
         | (1 << ADPS2)           // prescaler /16 → 500kHz @ 8MHz
         | (1 << ADPS0);
}

uint16_t adcRead(uint8_t ch) {
  ADMUX = (ADMUX & 0xF0) | (ch & 0x0F);  // select channel
  ADCSRA |= (1 << ADSC);                  // start conversion
  while (ADCSRA & (1 << ADSC));           // wait
  return ADC;
}

// ─── PI POWER CONTROL ─────────────────────────────────────
void piPowerOn() {
  PORTB |= (1 << PI_POWER);     // PB0 HIGH → 2N3904 on → U3 ON
}

void piPowerOff() {
  PORTB &= ~(1 << PI_POWER);    // PB0 LOW → 2N3904 off → U3 OFF
}

// ─── SHUTDOWN SIGNAL ──────────────────────────────────────
void signalPiShutdownStart() {
  PORTB |=  (1 << PI_SIGNAL);   // hold high until Pi confirms shutdown
}

void signalPiShutdownEnd() {
  PORTB &= ~(1 << PI_SIGNAL);
}

// ─── WAIT FOR PI CONFIRM ──────────────────────────────────
void waitForPiConfirm() {
  uint16_t timeout = 0;
  
  // Wait for Pi to pull confirm pin LOW (Pi drives it low on shutdown)
  while (adcRead(PI_CONFIRM) > 100) {  
    _delay_ms(100);
    timeout++;
    if (timeout > 300) break;   // 30 second timeout, then cut anyway
  }
  
  _delay_ms(2000);              // extra 2 sec safety margin
}

// ─── BOOT SEQUENCE ────────────────────────────────────────
void bootPi() {
  piPowerOn();
  _delay_ms(500);               // let rail stabilize before Pi sees power
  piRunning = true;
}

// ─── SHUTDOWN SEQUENCE ────────────────────────────────────
void shutdownPi() {
  signalPiShutdownStart();
  waitForPiConfirm();
  signalPiShutdownEnd();
  piPowerOff();
  piRunning = false;
}

// ─── MAIN ─────────────────────────────────────────────────
int main(void) {

  // GPIO setup
  DDRB |=  (1 << PI_POWER);    // PB0 output
  DDRB |=  (1 << PI_SIGNAL);   // PB1 output
  DDRB &= ~(1 << PB2);         // PB2 input (PI_CONFIRM via ADC3)

  // Start with Pi off
  piPowerOff();
  PORTB &= ~(1 << PI_SIGNAL);

  adcInit();
  _delay_ms(100);               // settle time after power on

  while (1) {
    uint16_t ign = adcRead(IGN_SENSE);

    if (!piRunning && ign > IGN_ON_THRESHOLD) {
      bootPi();
    }
    else if (piRunning && ign < IGN_OFF_THRESHOLD) {
      shutdownPi();
    }

    _delay_ms(500);             // poll every 500ms
  }
}