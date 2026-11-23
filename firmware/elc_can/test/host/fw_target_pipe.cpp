// A firmware update target on a pipe, for tools/fw_update.py --test: the real ElcChannels + ElcFwTarget (as the
// screens run them) with the image kept in memory, driven line by line on stdin, its frames written to stdout.
//
//   in:   n <node> <product> <variant> [capacity]    set up (once, first)
//         f <id hex> <data hex>                       a frame from the bus
//         t <ms>                                      time moves to <ms> and the channels are polled; answers "."
//                                                     after the frames that sent (so the other end knows it has all)
//         drop <every>                                lose one in <every> data frames on the way in (0 = none)
//         q                                           print the result and exit
//   out:  f <id hex> <data hex>                       a frame this node sent
//         r <phase> <bytes> <crc32 hex>               on q: the target's phase and what is in its flash

#include <elc_bulk.h>
#include <elc_fw.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct MemFlash : ElcFwFlash {
  std::vector<uint8_t> img;
  uint32_t want = 0, cap = 8u << 20;
  bool committed = false;
  uint32_t capacity() override { return cap; }
  bool begin(uint32_t n) override {
    img.clear();
    want = n;
    return true;
  }
  bool write(const uint8_t *p, size_t n) override {
    img.insert(img.end(), p, p + n);
    return img.size() <= want;
  }
  uint8_t commit() override {
    committed = img.size() == want && !img.empty() && img[0] == 0xE9;
    return committed ? ELC_BULK_OK : ELC_BULK_REJECTED;
  }
  void abort() override { img.clear(); }
};

static bool out_frame(const ElcCanFrame &f, void *) {
  std::printf("f %03x ", f.id);
  for (int i = 0; i < f.len; i++) std::printf("%02x", f.data[i]);
  std::printf("\n");
  return true;
}

int main() {
  ElcChannels ch;
  ElcFwTarget target;
  MemFlash flash;
  uint32_t now = 0, drop_every = 0, data_seen = 0;
  char line[512];
  while (std::fgets(line, sizeof(line), stdin)) {
    if (line[0] == 'n') {
      unsigned node = 0, product = 0, cap = 0;
      char variant[40] = {0};
      if (std::sscanf(line, "n %x %x %39s %u", &node, &product, variant, &cap) >= 3) {
        if (cap) flash.cap = cap;
        ElcNetAnnounce me;
        me.type = ELC_TYPE_DISPLAY;
        me.product = (uint16_t)product;
        snprintf(me.name, sizeof(me.name), "pipe target");
        ch.begin((uint8_t)node, me, out_frame, nullptr);
        target.begin(&ch, &flash, (uint16_t)product, variant, 1);
      }
    } else if (line[0] == 'f') {
      unsigned id = 0;
      char hex[40] = {0};
      if (std::sscanf(line, "f %x %39s", &id, hex) >= 1) {
        ElcCanFrame f;
        f.id = (uint16_t)id;
        f.len = (uint8_t)(strlen(hex) / 2 > 8 ? 8 : strlen(hex) / 2);
        for (int i = 0; i < f.len; i++) {
          char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
          f.data[i] = (uint8_t)strtoul(b, nullptr, 16);
        }
        const bool data = (f.id & ~0x1F) == ELC_CH_BULK_DATA;
        if (!(data && drop_every && (++data_seen % drop_every) == 0)) ch.feed(f, now);
      }
    } else if (line[0] == 't') {
      now = (uint32_t)strtoul(line + 2, nullptr, 10);
      ch.poll(now);
      std::printf(".\n");
    } else if (!strncmp(line, "drop", 4)) {
      drop_every = (uint32_t)strtoul(line + 5, nullptr, 10);
    } else if (line[0] == 'q') {
      std::printf("r %d %u %08x\n", (int)target.phase(), (unsigned)flash.img.size(),
                  (unsigned)elc_crc32(flash.img.data(), flash.img.size()));
      std::fflush(stdout);
      return 0;
    }
    std::fflush(stdout);
  }
  return 0;
}
