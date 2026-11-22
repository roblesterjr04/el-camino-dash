import subprocess
import unittest

from elc_ai import canmsg as C
from elc_ai import link as L
from helpers import build_cpp


class LinkTests(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(L.crc16(b"123456789"), 0x29B1)

    def test_cobs_vectors(self):
        self.assertEqual(L.cobs_encode(b"\x00"), b"\x01\x01")
        self.assertEqual(L.cobs_encode(b"\x11\x22\x00\x33"), b"\x03\x11\x22\x02\x33")
        for n in (253, 254, 255, 256, 600):
            data = bytes((1 + i % 255) for i in range(n))
            enc = L.cobs_encode(data)
            self.assertNotIn(0, enc)
            self.assertEqual(L.cobs_decode(enc), data)
        self.assertIsNone(L.cobs_decode(b"\x05\x01"))
        self.assertIsNone(L.cobs_decode(b""))

    def test_frames_and_errors(self):
        tx, p = L.Sender(), L.Parser()
        payload = bytes(i % 7 == 0 and 0 or i % 256 for i in range(640))
        got = list(p.feed(b"\x55\x00" + tx.frame(L.T_AUDIO, payload)))
        self.assertEqual(len(got), 1)
        self.assertEqual((got[0].type, got[0].payload), (L.T_AUDIO, payload))
        bad_before = p.bad
        f2 = bytearray(tx.frame(L.T_PING, b"\x01\x02\x03"))
        f2[2] ^= 0x10
        f3 = tx.frame(L.T_PING, b"\x01\x02\x03")
        self.assertEqual(list(p.feed(bytes(f2))), [])
        self.assertEqual(p.bad, bad_before + 1)
        self.assertEqual(len(list(p.feed(f3))), 1)
        self.assertEqual(p.gaps, 1)
        # chunked delivery gives the same frames
        data = tx.frame(L.T_PONG, b"abc") * 3
        p2 = L.Parser()
        out = []
        for i in range(0, len(data), 3):
            out += list(p2.feed(data[i:i + 3]))
        self.assertEqual(len(out), 3)

    def test_same_frame_as_the_esp32(self):
        """The C++ side builds the same bytes."""
        exe = build_cpp(r'''
            #include <stdio.h>
            #include "ai_link.h"
            int main() {
              uint8_t p[300]; for (int i = 0; i < 300; i++) p[i] = (uint8_t)(i % 5 ? i : 0);
              uint8_t w[ailink::kMaxWire];
              ailink::Tx tx;
              size_t n = tx.frame(ailink::T_AUDIO, p, 300, w, sizeof w);
              for (size_t i = 0; i < n; i++) printf("%02x", w[i]);
              printf("\n");
              n = tx.frame(ailink::T_PONG, (const uint8_t *)"\x07\x2a\x00\x01", 4, w, sizeof w);
              for (size_t i = 0; i < n; i++) printf("%02x", w[i]);
              printf("\n");
              return 0; }''', ["ai_card/src/ai_link.cpp", "elc_can/src/elc_chan.cpp", "elc_can/src/elc_can_codec.cpp"])
        if exe is None:
            self.skipTest("no C++ compiler")
        lines = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        tx = L.Sender()
        payload = bytes(i % 5 and i % 256 or 0 for i in range(300))
        self.assertEqual(lines[0], tx.frame(L.T_AUDIO, payload).hex())
        self.assertEqual(lines[1], tx.frame(L.T_PONG, L.pong(7, 42, 0, 1)).hex())


class CanMessageTests(unittest.TestCase):
    def test_decoders_match_the_firmware_encoders(self):
        exe = build_cpp(r'''
            #include <stdio.h>
            #include <elc_chan_msg.h>
            static void out(const char *n, const ElcChWriter &w) { printf("%s ", n); for (size_t i = 0; i < w.size(); i++) printf("%02x", w.data()[i]); printf("\n"); }
            int main() {
              { ElcSensorDefine d; d.sensor = 4; d.quantity = ELC_Q_PRESSURE; d.exponent = 0; d.flags = 3; d.period_ms = 200;
                d.caution_lo = 100; d.warn_lo = 50; elc_ch_copy_text(d.name, sizeof d.name, "Oil pressure"); ElcChWriter w; elc_sensor_encode(d, w); out("sdef", w); }
              { ElcSensorValue v; v.sensor = 4; v.value = 276; v.status = 0; ElcChWriter w; elc_sensor_encode(v, w); out("sval", w); }
              { ElcSensorValue v; v.sensor = 3; v.value = -55; v.status = 2; ElcChWriter w; elc_sensor_encode(v, w); out("sbad", w); }
              { ElcSysVehicle v; v.flags = 3; v.backlight = 80; v.speed_x10 = 553; ElcChWriter w; elc_sys_encode(v, w); out("veh", w); }
              { ElcSysPrefs v; v.units = 7; v.theme = 1; v.clock24 = 1; ElcChWriter w; elc_sys_encode(v, w); out("prefs", w); }
              { ElcSysTime v; v.utc = 1790608980u; v.offset_q = -20; v.source = 1; ElcChWriter w; elc_sys_encode(v, w); out("time", w); }
              { ElcSysPower v; v.event = 1; v.seconds = 8; ElcChWriter w; elc_sys_encode(v, w); out("power", w); }
              { ElcAlertDefine v; v.alert = 2; v.severity = 2; v.flags = 1; v.icon = 2; v.text[0] = 'H'; v.text[1] = 'o'; v.text[2] = 't'; ElcChWriter w; elc_alert_encode(v, w); out("adef", w); }
              { ElcAlertState v; v.active = 0x5; v.generation = 1; ElcChWriter w; elc_alert_encode(v, w); out("ast", w); }
              { ElcAudioStatus v; v.volume = 40; v.flags = 1; v.source = 2; ElcChWriter w; elc_audio_encode(v, w); out("astat", w); }
              { ElcChWriter w; elc_audio_encode(ElcAudioVolume{0x14, 1, -5}, w); out("vol", w); }
              { ElcChWriter w; elc_audio_encode(ElcAudioMedia{0x14, ELC_MEDIA_NEXT, 0}, w); out("media", w); }
              { ElcChWriter w; elc_audio_encode(ElcAudioSource{0x14, 0xFF}, w); out("src", w); }
              { ElcChWriter w; elc_audio_encode(ElcAudioPlay{0x14, 2, 0x01, 100, 1}, w); out("play", w); }
              return 0; }''', ["elc_can/src/elc_chan.cpp", "elc_can/src/elc_chan_msg.cpp", "elc_can/src/elc_can_codec.cpp"])
        if exe is None:
            self.skipTest("no C++ compiler")
        r = {}
        for line in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines():
            k, v = line.split()
            r[k] = bytes.fromhex(v)
        d = C.decode(C.CH_SENSOR, 0x01, r["sdef"])
        self.assertEqual((d.sensor, d.quantity, d.exponent, d.period_ms, d.caution_lo, d.warn_lo, d.name),
                         (4, C.Q_PRESSURE, 0, 200, 100, 50, "Oil pressure"))
        v = C.decode(C.CH_SENSOR, 0x01, r["sval"])
        self.assertEqual((v.node, v.sensor, v.value, v.status), (1, 4, 276, 0))
        self.assertEqual(C.decode(C.CH_SENSOR, 1, r["sbad"]).status, 2)
        veh = C.decode(C.CH_SYSTEM, 1, r["veh"])
        self.assertEqual((veh.flags, veh.backlight, veh.speed_kmh), (3, 80, 55.3))
        self.assertEqual(C.decode(C.CH_SYSTEM, 1, r["prefs"]).units, 7)
        t = C.decode(C.CH_SYSTEM, 1, r["time"])
        self.assertEqual((t.utc, t.offset_min, t.source), (1790608980, -300, 1))
        self.assertEqual(C.decode(C.CH_SYSTEM, 1, r["power"]).seconds, 8)
        a = C.decode(C.CH_ALERT, 0x15, r["adef"])
        self.assertEqual((a.node, a.alert, a.severity, a.text), (0x15, 2, 2, "Hot"))
        self.assertEqual(C.decode(C.CH_ALERT, 0x15, r["ast"]).active, 5)
        st = C.decode(C.CH_AUDIO_CMD, 0x14, r["astat"])
        self.assertEqual((st.volume, st.flags, st.source), (40, 1, 2))
        self.assertEqual(C.audio_volume(0x14, C.VOLUME_STEP, -5), r["vol"])
        self.assertEqual(C.audio_media(0x14, C.MEDIA_NEXT), r["media"])
        self.assertEqual(C.audio_source(0x14, C.SRC_OFF), r["src"])
        self.assertEqual(C.audio_play(0x14, 2), r["play"])

    def test_online_messages(self):
        # built by hand from can_channels.md (the firmware has no ONLINE codec yet)
        place = bytes([0x01, 7, 0b001, 45, ord("U"), ord("S"), 11]) + b"Main Street" + bytes([6]) + b"Austin" + bytes([5]) + b"Texas"
        p = C.decode(C.CH_ONLINE, 0x14, place)
        self.assertEqual((p.valid, p.speed_limit, p.country, p.street, p.locality, p.region), (True, 45, "US", "Main Street", "Austin", "Texas"))
        # a truncated string is rejected, not misread
        self.assertIsNone(C.decode(C.CH_ONLINE, 0x14, place[:-3]))
        weather = bytes([0x02]) + (1790600000).to_bytes(4, "little") + (255).to_bytes(2, "little", signed=True) + \
            (0x7FFF).to_bytes(2, "little") + bytes([1, 60]) + (35).to_bytes(2, "little") + (180).to_bytes(2, "little") + \
            bytes([40]) + (300).to_bytes(2, "little", signed=True) + (150).to_bytes(2, "little", signed=True) + \
            (1790620000).to_bytes(4, "little") + (1790660000).to_bytes(4, "little")
        w = C.decode(C.CH_ONLINE, 0x14, weather)
        self.assertEqual((w.temp_c, w.feels_c, w.rain_pct, w.high_c, w.low_c, w.wind_ms), (25.5, None, 40, 30.0, 15.0, 3.5))
        tz = bytes([0x03, 0xEC, 1, 15]) + b"America/Chicago"
        z = C.decode(C.CH_ONLINE, 0x14, tz)
        self.assertEqual((z.offset_min, z.dst, z.name), (-300, True, "America/Chicago"))
        s = C.decode(C.CH_ONLINE, 0x14, bytes([0x05, 0b0101, 3, 0, 4, 2]))
        self.assertEqual((s.internet_up, s.place_valid, s.weather_valid, s.last_error), (True, False, True, 2))


if __name__ == "__main__":
    unittest.main()
