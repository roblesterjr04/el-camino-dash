#include "elc_can.h"

#include <stdio.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <soc/soc_caps.h>
#endif

#if defined(ARDUINO_ARCH_ESP32) && SOC_TWAI_SUPPORTED && __has_include(<driver/twai.h>)
#define ELC_CAN_HAVE_TWAI 1
#include <driver/gpio.h>
#include <driver/twai.h>
#else
#define ELC_CAN_HAVE_TWAI 0
#endif

static const uint32_t kHeartbeatMs = 1000;
static const uint32_t kRecoverRetryMs = 1000;
static const size_t kTxQueueLen = 16;

uint8_t elc_can_read_slot_id(int id0_pin, int id1_pin) {
#if defined(ARDUINO_ARCH_ESP32)
  uint8_t slot = 0;
  if (id0_pin >= 0) {
    pinMode(id0_pin, INPUT_PULLUP);
  }
  if (id1_pin >= 0) {
    pinMode(id1_pin, INPUT_PULLUP);
  }
  delay(2);
  if (id0_pin >= 0 && digitalRead(id0_pin) == LOW) slot |= 1;
  if (id1_pin >= 0 && digitalRead(id1_pin) == LOW) slot |= 2;
  return slot > 2 ? 2 : slot;
#else
  (void)id0_pin;
  (void)id1_pin;
  return 0;
#endif
}

uint8_t elc_can_read_card_slot(int adc_pin) {
#if defined(ARDUINO_ARCH_ESP32)
  if (adc_pin < 0) return 0;
  pinMode(adc_pin, INPUT);  // the card's own 10k pull-up; no internal pull
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(adc_pin);
    delay(1);
  }
  return elc_can_card_slot_from_mv((uint16_t)(sum / 8));
#else
  (void)adc_pin;
  return 0;
#endif
}

#if ELC_CAN_HAVE_TWAI

bool ElcCan::begin(int tx_pin, int rx_pin, uint8_t node, ElcCanNodeType type) {
  _node = node;
  _type = type;
  elc_usb_set_identity(node, (ElcCanNodeType)type, ELC_CAN_FW_VERSION);
  if (tx_pin < 0 || rx_pin < 0) {
    return false;
  }
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL);
  g.tx_queue_len = kTxQueueLen;
  g.rx_queue_len = 128;  // album art streams at ~1250 frames/s: room for a slow loop pass
#if ELC_CAN_BITRATE_KBPS == 250
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
#else
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
#endif
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) {
    return false;
  }
  // No transceiver fitted (bench): keep RX recessive instead of floating into bus errors.
  gpio_pullup_en((gpio_num_t)rx_pin);
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  _stats.running = true;
  elc_usb_set_can_running(true);
  return true;
}

bool ElcCan::bus_send(const ElcCanFrame &f) {
  if (!_stats.running || _recovering) {
    return false;
  }
  twai_message_t m = {};
  m.identifier = f.id;
  m.data_length_code = f.len > 8 ? 8 : f.len;
  m.ss = 1;  // single shot: see elc_can.h
  memcpy(m.data, f.data, m.data_length_code);
  return twai_transmit(&m, 0) == ESP_OK;
}

size_t ElcCan::txFree() const {
  if (!_stats.running || _recovering) {
    return elc_usb_active() ? kTxQueueLen : 0;  // the USB link alone does not queue
  }
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK || st.msgs_to_tx >= kTxQueueLen) {
    return 0;
  }
  return kTxQueueLen - st.msgs_to_tx;
}

void ElcCan::poll() {
  const uint32_t now = millis();
  // Frames from the USB relay: delivered here like the bus's; in gateway mode they also go out on the bus.
  elc_usb_pump();
  ElcCanFrame uf;
  int ubudget = 512;  // bound the work per loop pass
  while (ubudget-- > 0 && elc_usb_take_frame(uf)) {
    deliver(uf, now);
    if (elc_usb_gateway()) bus_send(uf);
  }

  if (_stats.running) {
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
      _stats.tec = st.tx_error_counter;
      _stats.rec = st.rx_error_counter;
      if (st.state == TWAI_STATE_BUS_OFF && (!_recovering || now - _recover_ms >= kRecoverRetryMs)) {
        if (!_recovering) {
          _stats.bus_off++;
        }
        _recovering = true;
        _recover_ms = now;
        twai_initiate_recovery();
      } else if (st.state == TWAI_STATE_STOPPED && _recovering) {
        if (twai_start() == ESP_OK) {
          _recovering = false;
        }
      }
    }

    twai_message_t m;
    int budget = 256;  // bound the work per loop pass
    while (budget-- > 0 && twai_receive(&m, 0) == ESP_OK) {
      if (m.extd || m.rtr) {
        continue;
      }
      ElcCanFrame f;
      f.id = (uint16_t)m.identifier;
      f.len = m.data_length_code > 8 ? 8 : m.data_length_code;
      memcpy(f.data, m.data, f.len);
      _stats.rx++;
      if (elc_usb_gateway()) elc_usb_send_frame(f);  // the laptop sees the whole bus
      deliver(f, now);
    }
  }

  if (running()) heartbeat(now);
}

#else  // no TWAI on this target (host build or unsupported chip): the USB link only

bool ElcCan::begin(int, int, uint8_t node, ElcCanNodeType type) {
  _node = node;
  _type = type;
  elc_usb_set_identity(node, type, ELC_CAN_FW_VERSION);
  return false;
}
bool ElcCan::bus_send(const ElcCanFrame &) { return false; }
size_t ElcCan::txFree() const { return elc_usb_active() ? kTxQueueLen : 0; }
void ElcCan::poll() {
#if defined(ARDUINO)
  const uint32_t now = millis();
  elc_usb_pump();
  ElcCanFrame uf;
  int ubudget = 512;
  while (ubudget-- > 0 && elc_usb_take_frame(uf)) deliver(uf, now);
  if (running()) heartbeat(now);
#endif
}

#endif

bool ElcCan::send(const ElcCanFrame &f) {
  const bool usb = elc_usb_send_frame(f);
  const bool bus = bus_send(f);
  if (bus) {
    _stats.tx_ok++;
  } else {
    _stats.tx_fail++;
  }
  return usb || bus;
}

void ElcCan::deliver(const ElcCanFrame &f, uint32_t now) {
  ElcCanHeartbeat hb;
  if (elc_can_decode_heartbeat(f, hb) && hb.node < ELC_CAN_MAX_NODES) {
    ElcCanNodeInfo &n = _nodes[hb.node];
    n.seen = true;
    n.last_ms = now;
    n.hb = hb;
  }
  if (_handler) {
    _handler(f, _ctx);
  }
}

void ElcCan::heartbeat(uint32_t now) {
  if (_last_hb_ms != 0 && now - _last_hb_ms < kHeartbeatMs) {
    return;
  }
  _last_hb_ms = now ? now : 1;
  ElcCanHeartbeat hb;
  hb.node = _node;
  hb.type = _type;
  hb.fw_version = ELC_CAN_FW_VERSION;
  hb.uptime_s = now / 1000u;
  ElcCanFrame f;
  elc_can_encode_heartbeat(hb, f);
  send(f);
}

bool ElcCan::nodeAlive(uint8_t node, uint32_t max_age_ms) const {
  if (node >= ELC_CAN_MAX_NODES || !_nodes[node].seen) {
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  return millis() - _nodes[node].last_ms <= max_age_ms;
#else
  (void)max_age_ms;
  return true;
#endif
}

void ElcCan::describe(char *out, size_t n) const {
  if (!out || n == 0) {
    return;
  }
  int w = snprintf(out, n, "can: %s node 0x%02x tx %lu (fail %lu) rx %lu bus-off %lu tec %lu rec %lu; nodes:",
                   _stats.running ? (_recovering ? "recovering" : "up") : "off", _node,
                   (unsigned long)_stats.tx_ok, (unsigned long)_stats.tx_fail, (unsigned long)_stats.rx,
                   (unsigned long)_stats.bus_off, (unsigned long)_stats.tec, (unsigned long)_stats.rec);
  bool any = false;
  for (uint8_t i = 0; i < ELC_CAN_MAX_NODES && w > 0 && (size_t)w < n; i++) {
    if (i != _node && nodeAlive(i)) {
      w += snprintf(out + w, n - (size_t)w, " 0x%02x(type %u)", i, (unsigned)_nodes[i].hb.type);
      any = true;
    }
  }
  if (!any && w > 0 && (size_t)w < n) {
    w += snprintf(out + w, n - (size_t)w, " none");
  }
  if (w > 0 && (size_t)w + 2 < n) {
    out[w++] = ';';
    out[w++] = ' ';
    elc_usb_describe(out + w, n - (size_t)w);
  }
}
