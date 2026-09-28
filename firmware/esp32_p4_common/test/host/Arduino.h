// Minimal Arduino stand-in so sensor_packet.cpp can be unit-tested on the host (see run_tests.sh).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class HardwareSerial {
 public:
  int available() { return (int)rx.size(); }
  int read() {
    if (rx.empty()) {
      return -1;
    }
    const uint8_t c = rx.front();
    rx.pop_front();
    return c;
  }
  size_t write(uint8_t c) {
    tx.push_back(c);
    return 1;
  }
  void feed(const std::vector<uint8_t> &bytes) { rx.insert(rx.end(), bytes.begin(), bytes.end()); }

  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
};
