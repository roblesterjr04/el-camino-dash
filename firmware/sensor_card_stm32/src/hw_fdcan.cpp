#include "hw_fdcan.h"

#include <Arduino.h>

#include "elc_can_ids.h"

// The STM32duino core routes the FDCAN interrupt lines (which the G0 shares with TIM16 / TIM17) through its timer
// handlers to whatever handles these pointers name (HardwareTimer.cpp); it declares them and leaves the definitions
// to the sketch. FDCAN2 is not used (the G0B1 has it; the card does not wire it).
extern "C" {
FDCAN_HandleTypeDef *phfdcan1 = nullptr;
FDCAN_HandleTypeDef *phfdcan2 = nullptr;
}

namespace {
FDCAN_HandleTypeDef s_h;
FdCan *s_owner = nullptr;
const uint32_t kDlc[9] = {FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2, FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4,
                          FDCAN_DLC_BYTES_5, FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8};

uint8_t dlc_to_len(uint32_t dlc) {
  for (uint8_t i = 0; i <= 8; i++) {
    if (kDlc[i] == dlc) return i;
  }
  return 8;
}
}  // namespace

extern "C" void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef *h) {
  if (h->Instance != FDCAN1) return;
  RCC_PeriphCLKInitTypeDef pc = {};
  pc.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
  pc.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;  // 64 MHz
  HAL_RCCEx_PeriphCLKConfig(&pc);
  __HAL_RCC_FDCAN_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef g = {};
  g.Pin = GPIO_PIN_11 | GPIO_PIN_12;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  g.Alternate = GPIO_AF3_FDCAN1;
  HAL_GPIO_Init(GPIOA, &g);
}

extern "C" void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *h, uint32_t its) {
  (void)its;
  if (h == &s_h && s_owner) s_owner->onRx();
}

bool FdCan::begin() {
  _up = false;
  _tx_n = _rx_n = 0;
  _tx_h = _rx_h = 0;
  s_owner = this;
  s_h.Instance = FDCAN1;
  s_h.Init.ClockDivider = FDCAN_CLOCK_DIV1;
  s_h.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  s_h.Init.Mode = FDCAN_MODE_NORMAL;
  s_h.Init.AutoRetransmission = DISABLE;  // single-shot
  s_h.Init.TransmitPause = DISABLE;
  s_h.Init.ProtocolException = DISABLE;
  // 64 MHz kernel clock / 8 = 8 MHz: 16 time quanta of 125 ns at 500 kbit/s (sync 1 + seg1 11 + seg2 4, sample at
  // 75 %, SJW 2), the same quanta the ESP32 boards and rev A's MCP2515 use. 250 kbit/s doubles the prescaler.
  s_h.Init.NominalPrescaler = (uint32_t)(8 * 500 / ELC_CAN_BITRATE_KBPS);
  s_h.Init.NominalSyncJumpWidth = 2;
  s_h.Init.NominalTimeSeg1 = 11;
  s_h.Init.NominalTimeSeg2 = 4;
  s_h.Init.DataPrescaler = s_h.Init.NominalPrescaler;  // unused in classic mode, kept valid
  s_h.Init.DataSyncJumpWidth = 2;
  s_h.Init.DataTimeSeg1 = 11;
  s_h.Init.DataTimeSeg2 = 4;
  s_h.Init.StdFiltersNbr = 0;  // the global filter takes everything
  s_h.Init.ExtFiltersNbr = 0;
  s_h.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;  // frames leave in the order queued
  if (HAL_FDCAN_Init(&s_h) != HAL_OK) return false;
  // every 11-bit data frame into FIFO 0; extended and remote frames are not ours
  if (HAL_FDCAN_ConfigGlobalFilter(&s_h, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT, FDCAN_REJECT_REMOTE,
                                   FDCAN_REJECT_REMOTE) != HAL_OK)
    return false;
  phfdcan1 = &s_h;
  if (HAL_FDCAN_ConfigInterruptLines(&s_h, FDCAN_IT_GROUP_RX_FIFO0, FDCAN_INTERRUPT_LINE0) != HAL_OK) return false;
  if (HAL_FDCAN_ActivateNotification(&s_h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) return false;
  HAL_NVIC_SetPriority(TIM16_FDCAN_IT0_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(TIM16_FDCAN_IT0_IRQn);
  if (HAL_FDCAN_Start(&s_h) != HAL_OK) return false;
  _up = true;
  return true;
}

void FdCan::onRx() {
  FDCAN_RxHeaderTypeDef hd;
  uint8_t d[8];
  while (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) > 0) {
    if (HAL_FDCAN_GetRxMessage(&s_h, FDCAN_RX_FIFO0, &hd, d) != HAL_OK) break;
    _st.rx++;
    if (hd.IdType != FDCAN_STANDARD_ID || hd.RxFrameType != FDCAN_DATA_FRAME) continue;
    if (_rx_n >= kRx) {
      _st.rx_over++;
      continue;
    }
    ElcCanFrame &f = _rxq[(_rx_h + _rx_n) % kRx];
    f.id = (uint16_t)hd.Identifier;
    f.len = dlc_to_len(hd.DataLength);
    for (uint8_t i = 0; i < 8; i++) f.data[i] = i < f.len ? d[i] : 0;
    _rx_n++;
  }
}

bool FdCan::send(const ElcCanFrame &f) {
  if (!_up) return false;
  if (_tx_n >= kTx) {
    _st.tx_dropped++;
    return false;
  }
  _txq[(_tx_h + _tx_n) % kTx] = f;
  _tx_n++;
  return true;
}

bool FdCan::receive(ElcCanFrame &f) {
  noInterrupts();
  if (!_rx_n) {
    interrupts();
    return false;
  }
  f = _rxq[_rx_h];
  _rx_h = (uint8_t)((_rx_h + 1) % kRx);
  _rx_n--;
  interrupts();
  return true;
}

void FdCan::service(uint32_t now) {
  if (!_up) return;
  // lost frames (the FIFO was full when one arrived: the interrupt was held off that long)
  if (__HAL_FDCAN_GET_FLAG(&s_h, FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST)) {
    __HAL_FDCAN_CLEAR_FLAG(&s_h, FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
    _st.rx_lost++;
  }
  // acknowledged transmissions
  FDCAN_TxEventFifoTypeDef ev;
  while (HAL_FDCAN_GetTxEvent(&s_h, &ev) == HAL_OK) _st.tx_ok++;

  FDCAN_ProtocolStatusTypeDef ps;
  if (HAL_FDCAN_GetProtocolStatus(&s_h, &ps) == HAL_OK) {
    if (ps.BusOff) {
      if (_err_state != 2) {
        _st.bus_off++;
        _off_ms = now;
      }
      _err_state = 2;
      // the controller stopped itself (INIT set); after a second, start it again
      if (now - _off_ms >= 1000) {
        _off_ms = now;
        HAL_FDCAN_Start(&s_h);
      }
      return;
    }
    _err_state = ps.ErrorPassive ? 1 : 0;
    if (ps.LastErrorCode != FDCAN_PROTOCOL_ERROR_NO_CHANGE) _lec = (uint8_t)ps.LastErrorCode;
  }
  if (now - _check_ms >= 500) {
    _check_ms = now;
    FDCAN_ErrorCountersTypeDef ec;
    if (HAL_FDCAN_GetErrorCounters(&s_h, &ec) == HAL_OK) {
      _tec = (uint8_t)ec.TxErrorCnt;
      _rec = (uint8_t)ec.RxErrorCnt;
    }
  }

  while (_tx_n && HAL_FDCAN_GetTxFifoFreeLevel(&s_h) > 0) {
    const ElcCanFrame &f = _txq[_tx_h];
    FDCAN_TxHeaderTypeDef hd = {};
    hd.Identifier = f.id & 0x7FF;
    hd.IdType = FDCAN_STANDARD_ID;
    hd.TxFrameType = FDCAN_DATA_FRAME;
    hd.DataLength = kDlc[f.len > 8 ? 8 : f.len];
    hd.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hd.BitRateSwitch = FDCAN_BRS_OFF;
    hd.FDFormat = FDCAN_CLASSIC_CAN;
    hd.TxEventFifoControl = FDCAN_STORE_TX_EVENTS;
    hd.MessageMarker = 0;
    if (HAL_FDCAN_AddMessageToTxFifoQ(&s_h, &hd, (uint8_t *)f.data) != HAL_OK) break;
    _st.tx_sent++;
    _tx_h = (uint8_t)((_tx_h + 1) % kTx);
    _tx_n--;
  }
}
