#include "center_store.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <chan_screen.h>
#include <elc_store.h>

#include "center_sd_settings.h"

#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)
#include <FS.h>
#include <SD_MMC.h>
#define CENTER_STORE_SD 1
#endif

namespace {

// Relative to the SD mount, like settings.json (center_sd_settings.cpp).
constexpr const char *kRoot = "/elcamino/store";

class SdBackend : public ElcStoreBackend {
 public:
  bool ready() override {
#if CENTER_STORE_SD
    return center_sd_mounted();
#else
    return false;
#endif
  }

#if CENTER_STORE_SD
  int32_t size(const char *owner, const char *key) override {
    char p[64];
    path(p, sizeof(p), owner, key);
    if (!SD_MMC.exists(p)) return -1;
    File f = SD_MMC.open(p, "r");
    if (!f || f.isDirectory()) return -1;
    const int32_t n = (int32_t)f.size();
    f.close();
    return n;
  }

  int count(const char *owner) override {
    char d[48];
    snprintf(d, sizeof(d), "%s/%s", kRoot, owner);
    File dir = SD_MMC.open(d);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (!e.isDirectory()) n++;
      e.close();
    }
    dir.close();
    return n;
  }

  bool write(const char *owner, const char *key, const uint8_t *p, size_t n, bool append) override {
    char d[48];
    snprintf(d, sizeof(d), "%s/%s", kRoot, owner);
    if (!SD_MMC.exists(kRoot) && !SD_MMC.mkdir(kRoot)) return false;
    if (!SD_MMC.exists(d) && !SD_MMC.mkdir(d)) return false;
    char fp[64];
    path(fp, sizeof(fp), owner, key);
    File f = SD_MMC.open(fp, append ? FILE_APPEND : FILE_WRITE, true);
    if (!f) return false;
    const size_t w = n ? f.write(p, n) : 0;
    f.close();
    return w == n;
  }

  int32_t read(const char *owner, const char *key, uint32_t offset, uint8_t *out, size_t cap) override {
    char p[64];
    path(p, sizeof(p), owner, key);
    File f = SD_MMC.open(p, "r");
    if (!f || f.isDirectory()) return -1;
    int32_t n = -1;
    if (f.seek(offset)) n = (int32_t)f.read(out, cap);
    f.close();
    return n;
  }

  bool remove(const char *owner, const char *key) override {
    char p[64];
    path(p, sizeof(p), owner, key);
    return SD_MMC.remove(p);
  }

 private:
  static void path(char *out, size_t cap, const char *owner, const char *key) {
    snprintf(out, cap, "%s/%s/%s", kRoot, owner, key);
  }
#else
  int32_t size(const char *, const char *) override { return -1; }
  int count(const char *) override { return 0; }
  bool write(const char *, const char *, const uint8_t *, size_t, bool) override { return false; }
  int32_t read(const char *, const char *, uint32_t, uint8_t *, size_t) override { return -1; }
  bool remove(const char *, const char *) override { return false; }
#endif
};

SdBackend s_backend;
ElcStoreServer s_store;
bool s_begun = false;

const char *result_name(uint8_t r) {
  static const char *const kNames[] = {"ok", "not found", "bad key", "full", "no SD card", "busy", "I/O error", "bad request"};
  return r < sizeof(kNames) / sizeof(kNames[0]) ? kNames[r] : "?";
}

}  // namespace

void center_store_begin() {
  ElcChannels *ch = chan_screen();
  if (!ch) return;
  s_store.begin(ch, &s_backend);
  s_begun = true;
}

void center_store_transfer(const ElcChTransfer &t) {
  if (!s_begun || t.base != ELC_CH_STORE) return;
  if (s_store.handle(t)) {
    const ElcStoreServer::Stats &st = s_store.stats();
    if (st.last_result != ELC_STORE_OK) {
      Serial.printf("[store] node 0x%02X op 0x%02X: %s\n", (unsigned)st.last_node, (unsigned)st.last_op,
                    result_name(st.last_result));
    }
  }
}

bool center_store_command(const char *line, char *reply, size_t n) {
  if (strcmp(line, "store") != 0) return false;
  const ElcStoreServer::Stats &st = s_store.stats();
  int k = snprintf(reply, n, "store: %s, %lu puts (%lu B), %lu gets (%lu B), %lu deletes, %lu refused",
                   s_backend.ready() ? "SD card" : "no SD card", (unsigned long)st.puts, (unsigned long)st.bytes_in,
                   (unsigned long)st.gets, (unsigned long)st.bytes_out, (unsigned long)st.deletes,
                   (unsigned long)st.refused);
#if CENTER_STORE_SD
  if (s_backend.ready()) {
    File root = SD_MMC.open(kRoot);
    if (root && root.isDirectory()) {
      for (File d = root.openNextFile(); d && k > 0 && (size_t)k < n; d = root.openNextFile()) {
        if (d.isDirectory()) {
          k += snprintf(reply + k, n - (size_t)k, "\n  %s:", d.name());
          for (File e = d.openNextFile(); e && (size_t)k < n; e = d.openNextFile()) {
            k += snprintf(reply + k, n - (size_t)k, " %s (%lu B)", e.name(), (unsigned long)e.size());
            e.close();
          }
        }
        d.close();
      }
    }
    if (root) root.close();
  }
#endif
  (void)k;
  return true;
}
