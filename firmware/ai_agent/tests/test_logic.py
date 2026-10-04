import struct
import tempfile
import time
import unittest
from pathlib import Path

from elc_ai import canmsg as C
from elc_ai.connectivity import Connectivity
from elc_ai.context import VehicleContext
from elc_ai.endpoint import Endpointer
from elc_ai.intents import handle, normalise
from elc_ai.llm import FakeLlm, FirstTokenTimeout, first_token_guard
from elc_ai.retrieval import Manual
from elc_ai.router import ALWAYS, AUTO, LOCAL, OFFLINE_ONLY, ONLINE, needs_fresh, route
from elc_ai.stt import clean_transcript, wav_bytes
from elc_ai.tts import clean_for_speech, split_sentences


def value(sensor, raw, status=0):
    return bytes([0x02, sensor]) + raw.to_bytes(4, "little", signed=True) + bytes([status])


def car(units=0):
    ctx = VehicleContext()
    ctx.ingest(C.CH_SYSTEM, 1, bytes([0x03, units, 0, 0]))
    ctx.ingest(C.CH_SENSOR, 1, value(4, 276))        # oil pressure 276 kPa  = 40 psi
    ctx.ingest(C.CH_SENSOR, 1, value(3, 902))        # engine 90.2 C = 194 F
    ctx.ingest(C.CH_SENSOR, 1, value(2, 455))        # fuel 45.5 %
    ctx.ingest(C.CH_SENSOR, 1, value(0, 886))        # 88.6 km/h = 55 mph
    ctx.ingest(C.CH_SENSOR, 1, value(5, 1))
    ctx.ingest(C.CH_SENSOR, 1, value(8, 240000))     # 240 km range
    ctx.ingest(C.CH_SENSOR, 1, value(9, 850))        # 8.50 km/L = 20 mpg
    return ctx


class IntentTests(unittest.TestCase):
    def test_readings_in_the_drivers_units(self):
        ctx = car()
        self.assertEqual(handle("what's the oil pressure?", ctx).text, "The oil pressure is 40 psi.")
        self.assertEqual(handle("How hot is the engine", ctx).text, "The engine temperature is 194 degrees Fahrenheit.")
        self.assertEqual(handle("how fast am I going", ctx).text, "The road speed is 55 miles per hour.")
        self.assertEqual(handle("how much gas do I have left", ctx).text, "The tank is at 46 percent.")
        self.assertEqual(handle("how far can I go", ctx).text, "About 149 miles to empty.")
        self.assertIn("20.0 miles per gallon", handle("what's my mpg", ctx).text)
        self.assertIn("charging", handle("is the alternator charging", ctx).text)
        m = car(units=0b111)
        self.assertEqual(handle("oil pressure", m).text, "The oil pressure is 276 kilopascals.")
        self.assertEqual(handle("engine temperature", m).text, "The engine temperature is 90 degrees Celsius.")
        self.assertEqual(handle("how fast", m).text, "The road speed is 89 kilometers per hour.")

    def test_missing_stale_and_faulty_sensors_are_not_invented(self):
        ctx = VehicleContext()
        self.assertIn("not getting", handle("what's the oil pressure", ctx).text)
        t = [100.0]
        ctx = VehicleContext(clock=lambda: t[0])
        ctx.ingest(C.CH_SENSOR, 1, value(4, 276))
        t[0] += 10
        self.assertIn("not getting", handle("oil pressure", ctx).text)   # stale
        ctx.ingest(C.CH_SENSOR, 1, value(4, 0, status=2))
        self.assertIn("open circuit", handle("oil pressure", ctx).text)

    def test_commands(self):
        ctx = car()
        r = handle("turn it up", ctx, sink=0x14)
        self.assertEqual(r.actions, [(C.CH_AUDIO_CMD, C.audio_volume(0x14, C.VOLUME_STEP, 5))])
        self.assertEqual(handle("Hey, can you lower the volume please", ctx, 0x14).actions[0][1], C.audio_volume(0x14, C.VOLUME_STEP, -5))
        self.assertEqual(handle("set the volume to 30", ctx, 0x14).actions[0][1], C.audio_volume(0x14, C.VOLUME_SET, 30))
        self.assertEqual(handle("next song", ctx, 0x14).actions[0][1], C.audio_media(0x14, C.MEDIA_NEXT))
        self.assertEqual(handle("skip", ctx, 0x14).actions[0][1], C.audio_media(0x14, C.MEDIA_NEXT))
        self.assertEqual(handle("next", ctx, 0x14).actions[0][1], C.audio_media(0x14, C.MEDIA_NEXT))
        self.assertEqual(handle("previous track", ctx, 0x14).actions[0][1], C.audio_media(0x14, C.MEDIA_PREV))
        self.assertEqual(handle("pause the music", ctx, 0x14).actions[0][1], C.audio_media(0x14, C.MEDIA_PAUSE))
        self.assertEqual(handle("switch to the radio", ctx, 0x14).actions[0][1], C.audio_source(0x14, C.SRC_RADIO))
        self.assertEqual(handle("switch to bluetooth", ctx, 0x14).actions[0][1], C.audio_source(0x14, C.SRC_BLUETOOTH))
        self.assertEqual(handle("turn the music off", ctx, 0x14).actions[0][1], C.audio_source(0x14, C.SRC_OFF))
        # mute is a toggle on the sink: only send it when it changes something
        ctx.ingest(C.CH_AUDIO_CMD, 0x14, bytes([1, 40, 1, 0, 0, 0, 1, 0]))
        self.assertEqual(handle("mute", ctx, 0x14).actions, [])
        self.assertEqual(len(handle("unmute", ctx, 0x14).actions), 1)
        self.assertTrue(handle("forget our conversation", ctx).forget)

    def test_what_is_not_an_intent(self):
        ctx = car()
        for q in ("who won the game last night", "tell me a joke", "how do I adjust the points gap on a 283", "", "the next exit"):
            self.assertIsNone(handle(q, ctx), q)

    def test_alerts_time_place_weather(self):
        ctx = VehicleContext()
        self.assertEqual(handle("any warnings", ctx).text, "No alerts. Everything I can see looks normal.")
        ctx.ingest(C.CH_ALERT, 0x15, bytes([0x01, 2, 2, 1, 2, 0, 0]) + b"Trans fluid hot")
        ctx.ingest(C.CH_ALERT, 0x15, bytes([0x02, 4, 0, 0, 0, 1]))
        self.assertIn("Trans fluid hot", handle("is anything wrong", ctx).text)
        self.assertEqual(handle("what time is it", ctx).text, "I don't have the time yet.")
        t = [0.0]
        ctx = VehicleContext(clock=lambda: t[0])
        ctx.ingest(C.CH_SYSTEM, 1, bytes([0x01]) + (1790608980).to_bytes(4, "little") + bytes([(-20) & 0xFF, 1]))
        # 1790608980 = 2026-10-?? UTC; offset -5 h
        txt = handle("what time is it", ctx).text
        self.assertRegex(txt, r"^It's \d{1,2}:\d\d (AM|PM)\.$")
        t[0] += 120
        self.assertNotEqual(txt, handle("what time is it", ctx).text)   # the clock keeps running between TIME messages
        self.assertEqual(handle("where am i", ctx).text, "I don't have a location yet.")
        self.assertIsNone(handle("what's the weather", ctx))     # not known here: a model may know
        ctx.ingest(C.CH_ONLINE, 0x14, bytes([0x01, 1, 1, 0, 85, 83, 0, 6]) + b"Austin" + bytes([0]))
        self.assertIn("Austin", handle("where am i", ctx).text)

    def test_normalise(self):
        self.assertEqual(normalise("Hey, could you PLEASE turn it up?!"), "turn it up")


class RouteTests(unittest.TestCase):
    def test_routing(self):
        self.assertEqual(route("who won the game last night", AUTO, True), ONLINE)
        self.assertEqual(route("who won the game last night", AUTO, False), LOCAL)
        self.assertEqual(route("who won the game last night", OFFLINE_ONLY, True), LOCAL)
        self.assertEqual(route("tell me a joke", AUTO, True), LOCAL)
        self.assertEqual(route("tell me a joke", ALWAYS, True), ONLINE)
        self.assertEqual(route("tell me a joke", ALWAYS, False), LOCAL)
        self.assertEqual(route("is my oil pressure normal for a 283", ALWAYS, True), LOCAL)   # the car's own data stays here
        self.assertEqual(route("what's the latest recall on my el camino", AUTO, True), ONLINE)
        self.assertTrue(needs_fresh("coffee shops near me"))
        self.assertFalse(needs_fresh("how does a carburetor work"))


class ConnectivityTests(unittest.TestCase):
    def test_hysteresis(self):
        t = [0.0]
        results = [True]
        c = Connectivity(lambda: results[0], interval_s=15, down_after=2, up_after=1, clock=lambda: t[0])
        self.assertTrue(c.check())
        results[0] = False
        t[0] += 16
        self.assertTrue(c.check())      # one bad probe is not enough
        t[0] += 16
        self.assertFalse(c.check())
        results[0] = True
        t[0] += 16
        self.assertTrue(c.check())
        c.mark_failure()
        self.assertTrue(c.up)
        c.mark_failure()
        self.assertFalse(c.up)
        t[0] += 4                        # a failed request makes the next probe come sooner than the interval
        self.assertTrue(c.check())

    def test_probe_errors_count_as_down(self):
        def boom():
            raise OSError("no route")
        c = Connectivity(boom)
        self.assertFalse(c.check())


def frames(ms, amp, hz=300):
    out = b""
    for i in range(ms // 20):
        out += struct.pack("<320h", *[int(amp * __import__("math").sin(2 * 3.14159 * hz * (i * 320 + k) / 16000)) for k in range(320)])
    return out


class EndpointTests(unittest.TestCase):
    def run_ep(self, audio, **kw):
        ep = Endpointer(**kw)
        for i in range(0, len(audio), 640):
            if ep.feed(audio[i:i + 640]):
                break
        return ep

    def test_speech_then_silence_ends(self):
        ep = self.run_ep(frames(300, 150) + frames(1000, 9000) + frames(2000, 100))
        self.assertEqual(ep.reason, "silence")
        self.assertTrue(1700 <= ep.elapsed_ms <= 2500)

    def test_no_speech(self):
        ep = self.run_ep(frames(6000, 120))
        self.assertEqual(ep.reason, "nospeech")

    def test_a_long_phrase_is_cut_at_the_limit(self):
        ep = self.run_ep(frames(300, 150) + frames(15000, 9000))
        self.assertEqual(ep.reason, "max")
        self.assertLessEqual(ep.elapsed_ms, 10100)

    def test_a_pause_inside_a_phrase_does_not_end_it(self):
        ep = self.run_ep(frames(300, 150) + frames(800, 9000) + frames(400, 100) + frames(800, 9000) + frames(1500, 100))
        self.assertEqual(ep.reason, "silence")
        self.assertGreater(ep.elapsed_ms, 2400)


class RetrievalTests(unittest.TestCase):
    def test_finds_the_right_chunk(self):
        m = Manual(chunk_words=30, overlap=5)
        m.add_text("engine.txt", "The 283 V8 uses a points distributor. Set the point gap to 0.016 inch and the dwell to 30 degrees. "
                                 "Check the timing with a strobe light at idle speed with the vacuum advance disconnected.")
        m.add_text("brakes.txt", "Bleed the brakes starting at the right rear wheel. Use DOT 3 fluid only and never reuse old fluid. " * 3)
        hit = m.search("what is the points gap", k=1)
        self.assertEqual(hit[0][1], "engine.txt")
        self.assertEqual(m.search("how do I bleed the brakes", k=1)[0][1], "brakes.txt")
        self.assertEqual(m.search("quantum chromodynamics"), [])
        self.assertEqual(Manual().search("anything"), [])

    def test_dir(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "a.txt").write_text("Torque the head bolts to 65 foot pounds in three steps.")
            Path(d, "skip.bin").write_text("ignored")
            m = Manual()
            self.assertEqual(m.add_dir(d), 1)
            self.assertIn("65 foot pounds", m.search("head bolt torque")[0][2])


class TextTests(unittest.TestCase):
    def test_sentences(self):
        s, rest = split_sentences("It is 72.5 degrees. Sunny and clear! Dr. Smith says")
        self.assertEqual(s, ["It is 72.5 degrees.", "Sunny and clear!"])
        self.assertEqual(rest, "Dr. Smith says")
        s, rest = split_sentences("Mr. Jones left. Done")
        self.assertEqual(s, ["Mr. Jones left."])

    def test_speech_cleanup(self):
        self.assertEqual(clean_for_speech("**Oil** is at 40°F, see https://x.org/a and `code`"), "Oil is at 40 degrees Fahrenheit, see and code")
        self.assertEqual(clean_transcript("[BLANK_AUDIO]"), "")
        self.assertEqual(clean_transcript(" turn it up (music) "), "turn it up")
        self.assertEqual(wav_bytes(b"\x00\x00" * 160)[:4], b"RIFF")


class GuardTests(unittest.TestCase):
    def test_first_token_timeout(self):
        slow = FakeLlm("late", delay_s=0.5)
        with self.assertRaises(FirstTokenTimeout):
            list(first_token_guard(lambda: slow.stream("", [], "q"), 0.1))
        ok = FakeLlm(pieces=["a", "b"])
        self.assertEqual(list(first_token_guard(lambda: ok.stream("", [], "q"), 1.0)), ["a", "b"])
        bad = FakeLlm(error=ValueError("x"))
        with self.assertRaises(ValueError):
            list(first_token_guard(lambda: bad.stream("", [], "q"), 1.0))


if __name__ == "__main__":
    unittest.main()
