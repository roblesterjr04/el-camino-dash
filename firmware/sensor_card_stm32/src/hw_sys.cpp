#include "hw_sys.h"

#include <Arduino.h>
#include <IWatchdog.h>

namespace {
bool s_hse = false;
uint8_t s_reset = 0xFF;  // read once

void clock_from(uint32_t source, uint32_t plln) {
  RCC_OscInitTypeDef osc = {};
  osc.OscillatorType = source == RCC_PLLSOURCE_HSE ? RCC_OSCILLATORTYPE_HSE : RCC_OSCILLATORTYPE_HSI;
  osc.HSEState = source == RCC_PLLSOURCE_HSE ? RCC_HSE_ON : RCC_HSE_OFF;
  osc.HSIState = RCC_HSI_ON;  // the HSI stays on either way: it is the clock until the PLL is up
  osc.HSIDiv = RCC_HSI_DIV1;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = source;
  osc.PLL.PLLM = RCC_PLLM_DIV1;
  osc.PLL.PLLN = plln;  // VCO 128 MHz
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = RCC_PLLQ_DIV2;
  osc.PLL.PLLR = RCC_PLLR_DIV2;  // SYSCLK 64 MHz
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    if (source == RCC_PLLSOURCE_HSE) {  // the crystal did not start: run from the 16 MHz HSI instead
      clock_from(RCC_PLLSOURCE_HSI, 8);
      return;
    }
    Error_Handler();
  }
  s_hse = source == RCC_PLLSOURCE_HSE;
  RCC_ClkInitTypeDef clk = {};
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}
}  // namespace

/** Replaces the generic variant's (weak) clock setup, which runs from the HSI. */
extern "C" void SystemClock_Config(void) {
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
  clock_from(RCC_PLLSOURCE_HSE, 16);  // 8 MHz x 16 / 2 = 64 MHz
}

bool sys_clock_is_hse() { return s_hse; }

uint8_t sys_reset_cause() {
  if (s_reset == 0xFF) {
    const uint32_t csr = RCC->CSR;
    uint8_t r = 0;
    if (csr & RCC_CSR_PWRRSTF) r |= 0x01;
    if (csr & RCC_CSR_PINRSTF) r |= 0x02;
    if (csr & RCC_CSR_OBLRSTF) r |= 0x04;
    if (csr & RCC_CSR_IWDGRSTF) r |= 0x08;
    if (csr & RCC_CSR_WWDGRSTF) r |= 0x10;
    if (csr & RCC_CSR_SFTRSTF) r |= 0x20;
    if (csr & RCC_CSR_LPWRRSTF) r |= 0x40;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    s_reset = r;
  }
  return s_reset;
}

uint32_t sys_unique_id() {
  uint32_t x = HAL_GetUIDw0() ^ (HAL_GetUIDw1() * 0x9E3779B1u) ^ ((HAL_GetUIDw2() << 7) | (HAL_GetUIDw2() >> 25));
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  return x ? x : 0xA5C3F00D;
}

void sys_watchdog_begin_2s() { IWatchdog.begin(2000000); }
void sys_watchdog_feed() { IWatchdog.reload(); }

void sys_restart() {
  NVIC_SystemReset();
  while (true) {
  }
}
