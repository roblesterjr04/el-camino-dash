#include "fw_portal.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <elc_can.h>
#include <elc_fw.h>
#include <elc_fw_esp32.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "oled_gfx.h"

namespace {

const uint32_t kIdleMs = 10u * 60u * 1000u;  // the hotspot closes after this long with nothing uploaded
const size_t kRing = 32 * 1024;               // upload bytes held for the screens that have not taken them yet
const uint16_t kPaceX10 = 25;                  // one data frame per 0.4 ms (the firmware profile, can_channels.md)

FwPortalConfig s_cfg;
WebServer *s_server = nullptr;
bool s_active = false;
uint32_t s_touch_ms = 0;
char s_ssid[24] = {0}, s_pass[12] = {0};
char s_msg[64] = "";

// ---- one upload ----
enum class Job : uint8_t { None, Header, Local, Remote, Done, Failed };
struct {
  Job state = Job::None;
  uint8_t head[ELC_FW_HEADER_SIZE];
  size_t head_n = 0;
  ElcFwHeader hdr;
  uint32_t got = 0;       // bytes of the file received from the browser
  uint32_t crc = 0;       // image CRC so far (local)
  char result[200] = "";  // what the page and the OLED say when it is over
  bool restart = false;
} s_job;

ElcFwFlashEsp32 s_local;  // the console's own spare app slot
ElcBulkWriter s_writer;

// the ring: file bytes [s_base, s_written) are held
uint8_t s_ring[kRing];
uint32_t s_base = 0, s_written = 0;

size_t ring_read(uint32_t off, uint8_t *buf, size_t n, void *) {
  if (off < s_base || off + n > s_written) return 0;
  for (size_t i = 0; i < n; i++) buf[i] = s_ring[(off + i) % kRing];
  return n;
}

bool raw_send(const ElcCanFrame &f, void *) { return s_cfg.chan && s_cfg.chan->sendRaw(f); }

const char *product_name(uint16_t p) {
  switch (p) {
    case ELC_PRODUCT_DISPLAY: return "screens";
    case ELC_PRODUCT_CONSOLE: return "console";
    case ELC_PRODUCT_AUDIO_CARD: return "audio card";
    case ELC_PRODUCT_RADIO: return "radio card";
    case ELC_PRODUCT_LTE: return "LTE card";
    default: return "card";
  }
}

const char *node_name(uint8_t n, char *buf, size_t cap) {
  const ElcChannels::Node &nd = s_cfg.chan->nodeInfo(n);
  if (nd.announced && nd.a.name[0]) snprintf(buf, cap, "%s", nd.a.name);
  else snprintf(buf, cap, "Node 0x%02X", (unsigned)n);
  return buf;
}

const char *rx_text(const ElcBulkWriter::Receiver &r) {
  using S = ElcBulkWriter::RxState;
  if (r.state == S::Accepted) return "receiving";
  if (r.state == S::Done) return "updated, restarting";
  if (r.state == S::Refused) {
    switch (r.result) {
      case ELC_BULK_WHY_TARGET: return "skipped (another build)";
      case ELC_BULK_WHY_BUSY: return "busy";
      case ELC_BULK_WHY_NOT_NOW: return "refused: car moving";
      case ELC_BULK_WHY_SIZE: return "refused: too big";
      default: return "refused";
    }
  }
  switch (r.result) {
    case ELC_BULK_CRC: return "failed: CRC";
    case ELC_BULK_NO_SPACE: return "failed: flash";
    case ELC_BULK_REJECTED: return "failed: image rejected";
    default: return "failed: lost contact";
  }
}

/** Keeps the bus, the writer and the OLED going while the upload handler holds the loop. */
void pump() {
  static uint32_t s_draw_ms = 0;
  if (s_cfg.can) s_cfg.can->poll();  // the channels poll the writer (a service) from here
  const uint32_t now = millis();
  if (s_cfg.redraw && now - s_draw_ms >= 100) {
    s_draw_ms = now;
    s_cfg.redraw(now);
  }
  delay(0);
}

void fail(const char *why) {
  snprintf(s_job.result, sizeof(s_job.result), "%s", why);
  if (s_job.state == Job::Local) s_local.abort();
  if (s_job.state == Job::Remote && s_writer.busy()) s_writer.abort(millis());
  s_job.state = Job::Failed;
}

void remote_result() {
  char nm[28];
  int w = 0;
  bool any = false;
  for (int i = 0; i < s_writer.receivers(); i++) {
    const auto &r = s_writer.receiver(i);
    w += snprintf(s_job.result + w, sizeof(s_job.result) - (size_t)w, "%s%s: %s", any ? "; " : "",
                  node_name(r.node, nm, sizeof(nm)), rx_text(r));
    any = true;
    if (w >= (int)sizeof(s_job.result)) break;
  }
  if (!any) snprintf(s_job.result, sizeof(s_job.result), "No %s of build %s answered", product_name(s_job.hdr.product), s_job.hdr.variant);
  s_job.state = s_writer.succeeded() ? Job::Done : Job::Failed;
}

/** The header is complete: decide where the file goes. */
void start_job() {
  if (!elc_fw_header_decode(s_job.head, ELC_FW_HEADER_SIZE, s_job.hdr)) {
    fail("Not an .elcfw file (use the one PlatformIO writes next to firmware.bin)");
    return;
  }
  const ElcFwHeader &h = s_job.hdr;
  if (s_cfg.stopped && !s_cfg.stopped()) {
    fail("The car is moving");
    return;
  }
  if (h.product == ELC_PRODUCT_CONSOLE) {
    if (elc_fw_esp32_target() && elc_fw_esp32_target()->active()) {
      fail("The console is taking an update over CAN right now");
      return;
    }
    if (strcmp(h.variant, ELC_FW_VARIANT) != 0) {
      fail("A console file for another build");
      return;
    }
    s_local.allowStaging(false);
    if (!s_local.begin(h.image_size)) {
      snprintf(s_job.result, sizeof(s_job.result), "Console: %s", s_local.error());
      s_job.state = Job::Failed;
      return;
    }
    s_job.crc = 0;
    s_job.state = Job::Local;
    snprintf(s_msg, sizeof(s_msg), "Console %s", h.variant);
    return;
  }
  // to every node of that product and build, over CAN; the ring holds the file from its first byte
  s_base = s_written = 0;
  memcpy(s_ring, s_job.head, ELC_FW_HEADER_SIZE);
  s_written = ELC_FW_HEADER_SIZE;
  s_writer.setPace(kPaceX10);
  if (!s_writer.start(elc_fw_offer(h, 0), ring_read, nullptr, millis())) {
    fail("Another update is still running");
    return;
  }
  s_job.state = Job::Remote;
  snprintf(s_msg, sizeof(s_msg), "%s %s", product_name(h.product), h.variant);
}

void feed(const uint8_t *p, size_t n) {
  s_touch_ms = millis();
  while (n && s_job.state == Job::Header) {
    const size_t k = ELC_FW_HEADER_SIZE - s_job.head_n < n ? ELC_FW_HEADER_SIZE - s_job.head_n : n;
    memcpy(s_job.head + s_job.head_n, p, k);
    s_job.head_n += k;
    s_job.got += (uint32_t)k;
    p += k;
    n -= k;
    if (s_job.head_n == ELC_FW_HEADER_SIZE) start_job();
  }
  if (!n) return;
  if (s_job.state == Job::Local) {
    if (s_job.got + n > ELC_FW_HEADER_SIZE + s_job.hdr.image_size || !s_local.write(p, n)) {
      fail(s_local.error()[0] ? s_local.error() : "The file is longer than its header says");
      return;
    }
    s_job.crc = elc_crc32(p, n, s_job.crc);
    s_job.got += (uint32_t)n;
    pump();
  } else if (s_job.state == Job::Remote) {
    while (n) {
      if (!s_writer.busy()) {  // nobody took it, or every receiver failed: say who said what
        remote_result();
        s_job.state = Job::Failed;
        return;
      }
      // let go of what every receiver already has, then copy in what fits
      const uint32_t low = s_writer.lowWater() * ELC_BULK_CHUNK_BYTES;
      if (low > s_base) s_base = low < s_written ? low : s_written;
      const size_t room = kRing - (s_written - s_base);
      if (!room) {
        pump();
        continue;
      }
      const size_t k = room < n ? room : n;
      for (size_t i = 0; i < k; i++) s_ring[(s_written + i) % kRing] = p[i];
      s_written += (uint32_t)k;
      s_job.got += (uint32_t)k;
      p += k;
      n -= k;
    }
    pump();
  }
}

void finish_upload() {
  if (s_job.state == Job::Local) {
    if (s_job.got != ELC_FW_HEADER_SIZE + s_job.hdr.image_size) {
      fail("The upload was cut short");
    } else if (s_job.crc != s_job.hdr.image_crc) {
      fail("CRC mismatch: the file is damaged");
    } else {
      const uint8_t r = s_local.commit();
      if (r == ELC_BULK_OK) {
        snprintf(s_job.result, sizeof(s_job.result), "Console updated. It restarts now; the page will drop.");
        s_job.state = Job::Done;
        s_job.restart = true;
      } else {
        snprintf(s_job.result, sizeof(s_job.result), "Console: %s", s_local.error());
        s_job.state = Job::Failed;
      }
    }
  } else if (s_job.state == Job::Remote) {
    if (s_job.got != ELC_FW_HEADER_SIZE + s_job.hdr.image_size) {
      fail("The upload was cut short");
      return;
    }
    while (s_writer.busy()) pump();  // the last 32 KB, then each screen's END
    remote_result();
  } else if (s_job.state == Job::Header) {
    fail("The file is too short");
  }
}

// ---- the page ----

const char kPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>El Camino firmware</title>
<style>body{font:16px system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:560px}
h1{font-size:20px}input,button{font:inherit}button{padding:10px 18px;margin-top:12px;background:#e8a33a;border:0;
border-radius:6px;color:#111}progress{width:100%;height:18px}#log{white-space:pre-wrap;color:#ccc}small{color:#999}</style>
</head><body><h1>El Camino firmware update</h1>
<p>Upload one <b>.elcfw</b> file at a time (PlatformIO writes it next to firmware.bin):</p>
<ul><li><b>console.elcfw</b>: this console</li><li><b>display-waveshare4c.elcfw</b>: the left and right screens</li>
<li><b>display-waveshare3c4.elcfw</b>: the center screen</li></ul>
<p><small>Keep the ignition on and the car still. A screen shows its progress and restarts when done; if the new firmware
does not start well it goes back to the old one by itself.</small></p>
<input type="file" id="f" accept=".elcfw"><br><button id="b">Upload</button>
<p><progress id="p" max="1000" value="0"></progress></p><div id="log"></div>
<script>
const $=id=>document.getElementById(id);let polling=null;
function poll(){fetch('/status').then(r=>r.json()).then(s=>{$('p').value=s.progress;$('log').textContent=s.text}).catch(()=>{})}
$('b').onclick=()=>{const f=$('f').files[0];if(!f){return}
 const x=new XMLHttpRequest(),d=new FormData();d.append('file',f,f.name);
 $('b').disabled=true;$('log').textContent='Sending '+f.name+' ...';polling=setInterval(poll,700);
 x.onload=()=>{clearInterval(polling);poll();$('log').textContent=x.responseText;$('b').disabled=false};
 x.onerror=()=>{clearInterval(polling);$('log').textContent='The connection dropped (the console restarts after its own update).';$('b').disabled=false};
 x.open('POST','/update');x.send(d)};
poll();
</script></body></html>)HTML";

void on_page() {
  s_touch_ms = millis();
  s_server->send_P(200, "text/html", kPage);
}

uint16_t job_progress() {
  if (s_job.state == Job::Remote) return s_writer.progress();
  if (s_job.state == Job::Local && s_job.hdr.image_size)
    return (uint16_t)((uint64_t)s_job.got * 1000 / (ELC_FW_HEADER_SIZE + s_job.hdr.image_size));
  return s_job.state == Job::Done ? 1000 : 0;
}

void job_text(char *out, size_t n) {
  switch (s_job.state) {
    case Job::None: snprintf(out, n, "Ready."); break;
    case Job::Header: snprintf(out, n, "Reading the file ..."); break;
    case Job::Local: snprintf(out, n, "Console: %u%%", (unsigned)(job_progress() / 10)); break;
    case Job::Remote: {
      int w = snprintf(out, n, "%s, %u%%:", s_msg, (unsigned)(s_writer.progress() / 10));
      char nm[28];
      for (int i = 0; i < s_writer.receivers() && w > 0 && (size_t)w < n; i++) {
        const auto &r = s_writer.receiver(i);
        w += snprintf(out + w, n - (size_t)w, "\n  %s: %s", node_name(r.node, nm, sizeof(nm)), rx_text(r));
      }
      if (s_writer.state() == ElcBulkWriter::State::Offering && w > 0 && (size_t)w < n)
        snprintf(out + w, n - (size_t)w, " asking the %s", product_name(s_job.hdr.product));
      break;
    }
    default: snprintf(out, n, "%s", s_job.result); break;
  }
}

void on_status() {
  s_touch_ms = millis();
  char text[400], body[520];
  job_text(text, sizeof(text));
  // JSON string: escape quotes, backslashes and newlines
  char esc[480];
  size_t j = 0;
  for (const char *c = text; *c && j + 2 < sizeof(esc); c++) {
    if (*c == '"' || *c == '\\') esc[j++] = '\\';
    if (*c == '\n') {
      esc[j++] = '\\';
      esc[j++] = 'n';
      continue;
    }
    esc[j++] = *c;
  }
  esc[j] = '\0';
  snprintf(body, sizeof(body), "{\"progress\":%u,\"busy\":%s,\"text\":\"%s\"}", (unsigned)job_progress(),
           fw_portal_busy() ? "true" : "false", esc);
  s_server->send(200, "application/json", body);
}

void on_upload() {
  HTTPUpload &u = s_server->upload();
  switch (u.status) {
    case UPLOAD_FILE_START:
      if (s_writer.busy()) s_writer.abort(millis());
      s_job.state = Job::Header;
      s_job.head_n = 0;
      s_job.got = 0;
      s_job.result[0] = '\0';
      s_job.restart = false;
      s_msg[0] = '\0';
      Serial.printf("fw: upload %s\n", u.filename.c_str());
      break;
    case UPLOAD_FILE_WRITE: feed(u.buf, u.currentSize); break;
    case UPLOAD_FILE_END: finish_upload(); break;
    case UPLOAD_FILE_ABORTED: fail("The upload was cut off"); break;
    default: break;
  }
}

void on_update_done() {
  if (s_job.state == Job::Header || s_job.state == Job::None) fail("No file received");
  Serial.printf("fw: %s\n", s_job.result);
  s_server->sendHeader("Connection", "close");
  s_server->send(s_job.state == Job::Done ? 200 : 500, "text/plain", s_job.result);
  if (s_job.restart) {
    const uint32_t t0 = millis();
    while (millis() - t0 < 800) pump();  // let the reply out
    ESP.restart();
  }
}

}  // namespace

void fw_portal_begin(const FwPortalConfig &cfg) {
  s_cfg = cfg;
  if (cfg.chan) s_writer.begin(cfg.chan, raw_send, nullptr);
}

bool fw_portal_active() { return s_active; }

bool fw_portal_busy() {
  return s_active && (s_job.state == Job::Header || s_job.state == Job::Local || s_job.state == Job::Remote);
}

bool fw_portal_start(uint32_t now_ms) {
  if (s_active) return true;
  if (s_cfg.stopped && !s_cfg.stopped()) {
    snprintf(s_msg, sizeof(s_msg), "Stop the car first");
    return false;
  }
  // a fresh password each time, from letters and digits that read clearly on the OLED
  static const char kChars[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 8; i++) s_pass[i] = kChars[esp_random() % (sizeof(kChars) - 1)];
  s_pass[8] = '\0';
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(s_ssid, sizeof(s_ssid), "ElCamino-%04X", (unsigned)((mac >> 32) & 0xFFFF));
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(s_ssid, s_pass, 6, 0, 2)) {
    WiFi.mode(WIFI_OFF);
    snprintf(s_msg, sizeof(s_msg), "Wi-Fi did not start");
    return false;
  }
  if (!s_server) {
    s_server = new WebServer(80);
    s_server->on("/", HTTP_GET, on_page);
    s_server->on("/status", HTTP_GET, on_status);
    s_server->on("/update", HTTP_POST, on_update_done, on_upload);
    s_server->onNotFound(on_page);
  }
  s_server->begin();
  s_active = true;
  s_touch_ms = now_ms;
  s_job.state = Job::None;
  s_msg[0] = '\0';
  Serial.printf("fw: hotspot %s up (password %s), page http://%s/\n", s_ssid, s_pass, WiFi.softAPIP().toString().c_str());
  return true;
}

void fw_portal_stop() {
  if (!s_active) return;
  if (s_writer.busy()) s_writer.abort(millis());
  if (s_job.state == Job::Local) s_local.abort();
  s_job.state = Job::None;
  if (s_server) s_server->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_active = false;
  Serial.println("fw: hotspot off");
}

void fw_portal_poll(uint32_t now_ms) {
  if (!s_active) return;
  s_server->handleClient();
  if (fw_portal_busy()) return;
  if (now_ms - s_touch_ms > kIdleMs) {
    Serial.println("fw: 10 minutes with nothing uploaded");
    fw_portal_stop();
  } else if (s_cfg.stopped && !s_cfg.stopped()) {
    Serial.println("fw: the car moved");
    fw_portal_stop();
  }
}

void fw_portal_draw(OledGfx &g, uint32_t now_ms, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small) {
  (void)now_ms;
  g.clear();
  g.text("FIRMWARE UPDATE", 4, 13, small, 9);
  const int clients = WiFi.softAPgetStationNum();
  g.text(clients ? "connected" : "waiting for a phone", 252, 13, small, clients ? 12 : 6, TextAlign::Right);
  if (fw_portal_busy() || s_job.state == Job::Done || s_job.state == Job::Failed) {
    char b[64];
    if (s_job.state == Job::Done || s_job.state == Job::Failed) {
      g.text(s_job.state == Job::Done ? "Done" : "Failed", 4, 34, title, 15);
      g.text(s_job.result, 4, 54, small, 10, TextAlign::Left, 248);
    } else {
      snprintf(b, sizeof(b), "%s", s_msg[0] ? s_msg : "Reading the file");
      g.text(b, 4, 32, body, 15, TextAlign::Left, 200);
      snprintf(b, sizeof(b), "%u%%", (unsigned)(job_progress() / 10));
      g.text(b, 252, 32, body, 15, TextAlign::Right);
      g.bar(4, 42, 248, 6, job_progress() / 1000.0f, 13, 3);
      g.text("Keep the ignition on", 4, 61, small, 8);
    }
    return;
  }
  char b[48];
  snprintf(b, sizeof(b), "Wi-Fi  %s", s_ssid);
  g.text(b, 4, 30, body, 15);
  snprintf(b, sizeof(b), "Password  %s", s_pass);
  g.text(b, 4, 46, body, 15);
  g.text("then open 192.168.4.1          Back: close", 4, 61, small, 8);
  (void)title;
}

bool fw_portal_command(const char *line, char *reply, size_t n) {
  if (strncmp(line, "fw", 2) != 0 || (line[2] && line[2] != ' ')) return false;
  if (!strcmp(line, "fw start")) {
    if (fw_portal_start(millis())) snprintf(reply, n, "fw: hotspot %s, password %s, http://192.168.4.1/", s_ssid, s_pass);
    else snprintf(reply, n, "fw: %s", s_msg);
    return true;
  }
  if (!strcmp(line, "fw stop")) {
    fw_portal_stop();
    snprintf(reply, n, "fw: hotspot off");
    return true;
  }
  if (elc_fw_esp32_command(line, reply, n)) {  // fw, fw good: this console as a CAN target, and its slots
    if (!strcmp(line, "fw")) {
      const size_t w = strlen(reply);
      char t[300];
      job_text(t, sizeof(t));
      snprintf(reply + w, n - w, "\nfw: hotspot %s; last upload: %s", s_active ? s_ssid : "off", t);
    }
    return true;
  }
  return false;
}
