#include "mcp2515.h"

#include <Arduino.h>
#include <SPI.h>

#include "card_pins.h"

namespace {
const uint8_t CMD_RESET = 0xC0, CMD_READ = 0x03, CMD_WRITE = 0x02, CMD_STATUS = 0xA0, CMD_BITMOD = 0x05,
              CMD_LOAD_TX0 = 0x40, CMD_RTS0 = 0x81, CMD_RX0 = 0x90, CMD_RX1 = 0x94;
const uint8_t REG_CNF3 = 0x28, REG_CNF2 = 0x29, REG_CNF1 = 0x2A, REG_CANINTE = 0x2B, REG_CANINTF = 0x2C,
              REG_EFLG = 0x2D, REG_TXB0CTRL = 0x30, REG_RXB0CTRL = 0x60, REG_RXB1CTRL = 0x70, REG_CANSTAT = 0x0E,
              REG_CANCTRL = 0x0F;
const SPISettings kSpi(4000000, MSBFIRST, SPI_MODE0);
}  // namespace

static inline void select(uint8_t cs) {
  SPI.beginTransaction(kSpi);
  digitalWrite(cs, LOW);
}
static inline void deselect(uint8_t cs) {
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
}

uint8_t Mcp2515::readReg(uint8_t a) {
  select(_cs);
  SPI.transfer(CMD_READ);
  SPI.transfer(a);
  const uint8_t v = SPI.transfer(0);
  deselect(_cs);
  return v;
}

void Mcp2515::writeReg(uint8_t a, uint8_t v) {
  select(_cs);
  SPI.transfer(CMD_WRITE);
  SPI.transfer(a);
  SPI.transfer(v);
  deselect(_cs);
}

void Mcp2515::bitModify(uint8_t a, uint8_t mask, uint8_t v) {
  select(_cs);
  SPI.transfer(CMD_BITMOD);
  SPI.transfer(a);
  SPI.transfer(mask);
  SPI.transfer(v);
  deselect(_cs);
}

uint8_t Mcp2515::readStatus() {
  select(_cs);
  SPI.transfer(CMD_STATUS);
  const uint8_t v = SPI.transfer(0);
  deselect(_cs);
  return v;
}

bool Mcp2515::probe(uint8_t cs) {
  _cs = cs;
  select(_cs);
  SPI.transfer(CMD_RESET);
  deselect(_cs);
  delay(10);  // oscillator start-up
  if ((readReg(REG_CANSTAT) & 0xE0) != 0x80) return false;  // a reset chip is in configuration mode
  writeReg(REG_CNF1, 0x55);
  if (readReg(REG_CNF1) != 0x55) return false;
  writeReg(REG_CNF1, 0xAA);
  return readReg(REG_CNF1) == 0xAA;
}

bool Mcp2515::configure() {
  if (_mhz == 8) {  // 8 TQ of 250 ns, sample point 62 %
    writeReg(REG_CNF1, 0x00);
    writeReg(REG_CNF2, 0x90);
    writeReg(REG_CNF3, 0x02);
  } else {  // 16 TQ of 125 ns: sync 1, prop 3, PS1 8, PS2 4 (sample point 75 %), SJW 2
    writeReg(REG_CNF1, 0x40);
    writeReg(REG_CNF2, 0xBA);
    writeReg(REG_CNF3, 0x03);
  }
  writeReg(REG_CANINTE, 0x03);              // RX interrupts (INT is not wired to anything we need)
  writeReg(REG_RXB0CTRL, 0x64);             // accept everything, roll over into RXB1
  writeReg(REG_RXB1CTRL, 0x60);
  writeReg(REG_CANCTRL, 0x08);              // normal mode, one-shot
  delay(2);
  return (readReg(REG_CANSTAT) & 0xE0) == 0x00;
}

bool Mcp2515::begin(uint8_t cs, uint8_t mhz) {
  _mhz = mhz == 8 ? 8 : 16;
  _up = false;
  _inflight = false;
  _tx_n = _rx_n = 0;
  _tx_h = _rx_h = 0;
  // every candidate select line starts deselected, and D53 (SS) stays an output whatever the select is: the AVR's SPI
  // master drops out the moment SS is an input and goes low
  for (uint8_t i = 0; i < sizeof(kCsCandidates); i++) {
    pinMode(kCsCandidates[i], OUTPUT);
    digitalWrite(kCsCandidates[i], HIGH);
  }
  SPI.begin();
  if (cs) {
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    _up = probe(cs) && configure();
  } else {
    for (uint8_t i = 0; i < sizeof(kCsCandidates) && !_up; i++) _up = probe(kCsCandidates[i]) && configure();
  }
  return _up;
}

bool Mcp2515::send(const ElcCanFrame &f) {
  if (!_up) return false;
  if (_tx_n >= kTx) {
    _st.tx_dropped++;
    return false;
  }
  _txq[(_tx_h + _tx_n) % kTx] = f;
  _tx_n++;
  return true;
}

bool Mcp2515::receive(ElcCanFrame &f) {
  if (!_rx_n) return false;
  f = _rxq[_rx_h];
  _rx_h = (uint8_t)((_rx_h + 1) % kRx);
  _rx_n--;
  return true;
}

void Mcp2515::load(const ElcCanFrame &f) {
  const uint8_t len = f.len > 8 ? 8 : f.len;
  select(_cs);
  SPI.transfer(CMD_LOAD_TX0);
  SPI.transfer((uint8_t)(f.id >> 3));
  SPI.transfer((uint8_t)((f.id & 7) << 5));
  SPI.transfer(0);
  SPI.transfer(0);
  SPI.transfer(len);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(f.data[i]);
  deselect(_cs);
  select(_cs);
  SPI.transfer(CMD_RTS0);
  deselect(_cs);
}

void Mcp2515::readRx(uint8_t instr) {
  uint8_t b[13];
  select(_cs);
  SPI.transfer(instr);  // the buffer's flag clears when the chip is deselected
  for (uint8_t i = 0; i < 13; i++) b[i] = SPI.transfer(0);
  deselect(_cs);
  _st.rx++;
  if ((b[1] & 0x08) || (b[1] & 0x10)) return;  // extended or remote frames are not ours
  if (_rx_n >= kRx) return;
  ElcCanFrame &f = _rxq[(_rx_h + _rx_n) % kRx];
  f.id = (uint16_t)(((uint16_t)b[0] << 3) | (b[1] >> 5));
  f.len = (uint8_t)(b[4] & 0x0F);
  if (f.len > 8) f.len = 8;
  for (uint8_t i = 0; i < 8; i++) f.data[i] = i < f.len ? b[5 + i] : 0;
  _rx_n++;
}

void Mcp2515::service(uint32_t now) {
  if (!_up) return;
  uint8_t st = readStatus();  // bit0 RX0IF, bit1 RX1IF, bit2 TXB0 request pending
  if (st & 0x01) readRx(CMD_RX0);
  if (st & 0x02) readRx(CMD_RX1);

  if (_inflight && !(st & 0x04)) {  // TXB0 is free: did the frame go?
    const uint8_t ctrl = readReg(REG_TXB0CTRL);
    if (ctrl & 0x30) {              // TXERR or MLOA
      if ((ctrl & 0x20) && _attempts < 4) {  // lost arbitration: try again
        _attempts++;
        load(_cur);
        _sent_ms = now;
      } else {
        _st.tx_fail++;  // no one acknowledged, or the bus is in trouble
        _inflight = false;
      }
    } else {
      _st.tx_ok++;
      _inflight = false;
    }
  } else if (_inflight && now - _sent_ms > 20) {  // never finished: abort it
    bitModify(REG_TXB0CTRL, 0x08, 0x00);
    _st.tx_fail++;
    _inflight = false;
  }
  if (!_inflight && _tx_n) {
    _cur = _txq[_tx_h];
    _tx_h = (uint8_t)((_tx_h + 1) % kTx);
    _tx_n--;
    load(_cur);
    _inflight = true;
    _attempts = 1;
    _sent_ms = now;
  }

  if (now - _check_ms >= 500) {  // error flags: overflow, bus-off
    _check_ms = now;
    _eflg = readReg(REG_EFLG);
    if (_eflg & 0xC0) {
      _st.rx_over++;
      bitModify(REG_EFLG, 0xC0, 0x00);
    }
    if (_eflg & 0x20) {  // bus-off: start again
      _st.bus_off++;
      begin(_cs, _mhz);
    }
  }
}
