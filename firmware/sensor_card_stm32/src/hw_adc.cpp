#include "hw_adc.h"

#include <Arduino.h>

#include "adc_scale.h"
#include "card_pins.h"

namespace {
const uint8_t kScans = 32;
ADC_HandleTypeDef s_adc;
DMA_HandleTypeDef s_dma;
volatile uint16_t s_ring[kScans * SLOT_COUNT];
bool s_running = false;
LevelInput s_ign(kIgnDivTop, kIgnDivBottom, kIgnOnVolts, kIgnOffVolts);
LevelInput s_alt(kAltDivTop, kAltDivBottom, kAltOnVolts, kAltOffVolts);

const uint32_t kChannels[SLOT_COUNT] = {ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_4, ADC_CHANNEL_5,
                                        ADC_CHANNEL_6, ADC_CHANNEL_7, ADC_CHANNEL_8, ADC_CHANNEL_9};

void gpio_analog(GPIO_TypeDef *port, uint32_t pins) {
  GPIO_InitTypeDef g = {};
  g.Pin = pins;
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(port, &g);
}

/** Mean of a slot's 12-bit samples over the ring; false while the ring still holds the 0xFFFF it was filled with. */
bool mean12(AdcSlot s, float *out) {
  uint32_t sum = 0;
  uint16_t n = 0;
  for (uint8_t i = 0; i < kScans; i++) {
    const uint16_t v = s_ring[i * SLOT_COUNT + s];
    if (v > 4095) continue;
    sum += v;
    n++;
  }
  if (!n) return false;
  *out = (float)sum / (float)n;
  return true;
}
}  // namespace

void adc_begin() {
  for (uint16_t i = 0; i < kScans * SLOT_COUNT; i++) s_ring[i] = 0xFFFF;
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_ADC_CLK_ENABLE();
  gpio_analog(GPIOA, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);
  gpio_analog(GPIOB, GPIO_PIN_0 | GPIO_PIN_1);

  s_dma.Instance = DMA1_Channel1;
  s_dma.Init.Request = DMA_REQUEST_ADC1;
  s_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  s_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  s_dma.Init.MemInc = DMA_MINC_ENABLE;
  s_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  s_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  s_dma.Init.Mode = DMA_CIRCULAR;
  s_dma.Init.Priority = DMA_PRIORITY_LOW;
  if (HAL_DMA_Init(&s_dma) != HAL_OK) return;

  s_adc.Instance = ADC1;
  s_adc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;  // 64 / 4 = 16 MHz
  s_adc.Init.Resolution = ADC_RESOLUTION_12B;
  s_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  s_adc.Init.ScanConvMode = ADC_SCAN_SEQ_FIXED;  // the selected channels in ascending order: IN0 1 4 5 6 7 8 9
  s_adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  s_adc.Init.LowPowerAutoWait = DISABLE;
  s_adc.Init.LowPowerAutoPowerOff = DISABLE;
  s_adc.Init.ContinuousConvMode = ENABLE;
  s_adc.Init.NbrOfConversion = 1;
  s_adc.Init.DiscontinuousConvMode = DISABLE;
  s_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  s_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  s_adc.Init.DMAContinuousRequests = ENABLE;
  s_adc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  // 160.5 cycles of sampling at 16 MHz: 10 us, enough for the 10k series resistor in front of every input
  s_adc.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_160CYCLES_5;
  s_adc.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_160CYCLES_5;
  s_adc.Init.OversamplingMode = ENABLE;  // 16 samples averaged in hardware, the result still 12 bits
  s_adc.Init.Oversampling.Ratio = ADC_OVERSAMPLING_RATIO_16;
  s_adc.Init.Oversampling.RightBitShift = ADC_RIGHTBITSHIFT_4;
  s_adc.Init.Oversampling.TriggeredMode = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
  s_adc.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&s_adc) != HAL_OK) return;
  __HAL_LINKDMA(&s_adc, DMA_Handle, s_dma);

  ADC_ChannelConfTypeDef ch = {};
  ch.Rank = ADC_RANK_CHANNEL_NUMBER;
  ch.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    ch.Channel = kChannels[i];
    if (HAL_ADC_ConfigChannel(&s_adc, &ch) != HAL_OK) return;
  }
  if (HAL_ADCEx_Calibration_Start(&s_adc) != HAL_OK) return;
  s_running = HAL_ADC_Start_DMA(&s_adc, (uint32_t *)s_ring, kScans * SLOT_COUNT) == HAL_OK;
}

bool adc_take(AdcSlot s, float *avg) {
  if (!s_running) return false;
  float m;
  if (!mean12(s, &m)) return false;
  switch (s) {
    case SLOT_BATT: *avg = adc_batt_counts((uint16_t)(m + 0.5f)); break;
    default: *avg = m * (1023.0f / kAdcMax12); break;
  }
  return true;
}

uint16_t adc_raw12(AdcSlot s) {
  // the DMA counter says where the next write goes; the newest complete scan is the one before it
  if (!s_running) return 0xFFFF;
  const uint32_t remaining = __HAL_DMA_GET_COUNTER(&s_dma);
  const uint32_t next = (kScans * SLOT_COUNT - remaining) % (kScans * SLOT_COUNT);
  const uint32_t scan = (next / SLOT_COUNT + kScans - 1) % kScans;
  return s_ring[scan * SLOT_COUNT + s];
}

bool adc_ign_on() {
  float m;
  if (s_running && mean12(SLOT_IGN, &m)) s_ign.update((uint16_t)(m + 0.5f));
  return s_ign.state;
}

bool adc_alt_high() {
  float m;
  if (s_running && mean12(SLOT_ALT, &m)) s_alt.update((uint16_t)(m + 0.5f));
  return s_alt.state;
}
