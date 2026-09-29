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

#if ELC_CAN_HAVE_TWAI

bool ElcCan::begin(int tx_pin, int rx_pin, uint8_t node, ElcCanNodeType type) {
  _node = node;
  _type = type;
  if (tx_pin < 0 || rx_pin < 0) {
    return false;
  }
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL);
  g.tx_queue_len = kTxQueueLen;
  g.rx_queue_len = 32;
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
  return true;
}

bool ElcCan::send(const ElcCanFrame &f) {
  if (!_stats.running || _recovering) {
    _stats.tx_fail++;
    return false;
  }
  twai_message_t m = {};
  m.identifier = f.id;
  m.data_length_code = f.len > 8 ? 8 : f.len;
  m.ss = 1;  // single shot: see elc_can.h
  memcpy(m.data, f.data, m.data_length_code);
  if (twai_transmit(&m, 0) == ESP_OK) {
    _stats.tx_ok++;
    return true;
  }
  _stats.tx_fail++;
  return false;
}

size_t ElcCan::txFree() const {
  if (!_stats.running || _recovering) {
    return 0;
  }
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK || st.msgs_to_tx >= kTxQueueLen) {
    return 0;
  }
  return kTxQueueLen - st.msgs_to_tx;
}

void ElcCan::poll() {
  if (!_stats.running) {
    return;
  }
  const uint32_t now = millis();
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
  int budget = 64;  // bound the work per loop pass
  while (budget-- > 0 && twai_receive(&m, 0) == ESP_OK) {
    if (m.extd || m.rtr) {
      continue;
    }
    ElcCanFrame f;
    f.id = (uint16_t)m.identifier;
    f.len = m.data_length_code > 8 ? 8 : m.data_length_code;
    memcpy(f.data, m.data, f.len);
    _stats.rx++;
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

  heartbeat(now);
}

#else  // no TWAI on this target (host build or unsupported chip): inert

bool ElcCan::begin(int, int, uint8_t node, ElcCanNodeType type) {
  _node = node;
  _type = type;
  return false;
}
bool ElcCan::send(const ElcCanFrame &) {
  _stats.tx_fail++;
  return false;
}
void ElcCan::poll() {}
size_t ElcCan::txFree() const { return 0; }

#endif

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
    snprintf(out + w, n - (size_t)w, " none");
  }
}
