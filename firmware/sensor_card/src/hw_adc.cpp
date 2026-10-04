#include "hw_adc.h"

#include <Arduino.h>
#include <avr/interrupt.h>

#include "card_pins.h"

static const uint8_t kChannel[SLOT_COUNT] = {ADC_FUEL, ADC_TEMP, ADC_OIL, ADC_AMB, ADC_BATT, ADC_WAKE};
static volatile uint32_t s_sum[SLOT_COUNT];
static volatile uint16_t s_n[SLOT_COUNT];
static volatile uint8_t s_cur = 0;

static inline void select(uint8_t ch) {
  ADMUX = _BV(REFS0) | (ch & 7);  // AVcc reference, right adjusted
  if (ch & 8) ADCSRB |= _BV(MUX5);
  else ADCSRB &= (uint8_t) ~_BV(MUX5);
}

ISR(ADC_vect) {
  const uint16_t v = ADC;
  const uint8_t i = s_cur;
  s_sum[i] += v;
  s_n[i]++;
  const uint8_t next = (uint8_t)((i + 1) % SLOT_COUNT);
  s_cur = next;
  select(kChannel[next]);
  ADCSRA |= _BV(ADSC);
}

void adc_begin() {
  DIDR0 = 0x0F;  // ADC0-3: digital input buffers off
  DIDR2 = 0x03;  // ADC8-9
  select(kChannel[0]);
  ADCSRA = _BV(ADEN) | _BV(ADIE) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);  // 16 MHz / 128 = 125 kHz
  ADCSRA |= _BV(ADSC);
}

bool adc_take(AdcSlot s, float *avg) {
  noInterrupts();
  const uint32_t sum = s_sum[s];
  const uint16_t n = s_n[s];
  s_sum[s] = 0;
  s_n[s] = 0;
  interrupts();
  if (!n) return false;
  *avg = (float)sum / (float)n;
  return true;
}
