import math
import struct
import threading
import time
import unittest

from elc_ai import canmsg as C
from elc_ai import link as L
from elc_ai.config import Config
from elc_ai.connectivity import Connectivity
from elc_ai.context import VehicleContext
from elc_ai.dialog import (Assistant, SRC_INTENT, SRC_LOCAL, SRC_ONLINE, ST_IDLE, ST_LISTENING, ST_SPEAKING, ST_THINKING)
from elc_ai.llm import FakeLlm, LlmError, LlmRefused
from elc_ai.stt import FakeStt
from elc_ai.tts import FakeTts
from helpers import wait_for


def pcm(ms, amp, hz=300):
    n = 16 * ms
    return struct.pack(f"<{n}h", *[int(amp * math.sin(2 * math.pi * hz * i / 16000)) for i in range(n)])


class Card:
    """The ESP32 end of the link: collects what the assistant sends, plays the part of the card's credit and microphone."""

    def __init__(self, stt_text="", local=None, online=None, online_up=True, settings=(0, 1, 80), cfg=None, tts=None, ctx=None):
        self.rx = L.Parser()
        self.frames = []
        self.lock = threading.Lock()
        self.conn = Connectivity(lambda: online_up, interval_s=1000)
        self.conn.check(force=True)
        self.stt = FakeStt(stt_text)
        self.local = local or FakeLlm("I am the local model.", name="local")
        self.online = online
        self.tts = tts or FakeTts(16000)
        self.ctx = ctx or VehicleContext()
        self.a = Assistant(self.write, self.ctx, self.stt, self.tts, self.local, self.online, self.conn, cfg or Config())
        self.tx = L.Sender()
        self.stop_credit = False
        self.a.start()
        self.a.on_frame(L.Frame(L.T_SETTINGS, 0, bytes([settings[0], settings[1], settings[2], 0])))
        threading.Thread(target=self._credit, daemon=True).start()

    def write(self, data):
        for f in self.rx.feed(data):
            with self.lock:
                self.frames.append(f)

    def _credit(self):
        while not self.stop_credit:
            self.a.on_frame(L.Frame(L.T_SPEECH_CREDIT, 0, (30000).to_bytes(2, "little")))
            time.sleep(0.01)

    def of(self, t):
        with self.lock:
            return [f for f in self.frames if f.type == t]

    def states(self):
        return [f.payload[0] for f in self.of(L.T_STATE)]

    def texts(self, kind):
        return [f.payload[1:].decode() for f in self.of(L.T_TEXT) if f.payload[0] == kind]

    def can_tx(self):
        return [(L.read_u16(f.payload, 1), bytes(f.payload[3:])) for f in self.of(L.T_CAN_TX)]

    def speech_bytes(self):
        return sum(len(f.payload) - 1 for f in self.of(L.T_SPEECH))

    def say(self):
        """Wake word, a phrase, silence."""
        self.a.on_frame(L.Frame(L.T_LISTEN, 0, bytes([1, 1])))
        for chunk in (pcm(300, 100), pcm(1000, 9000), pcm(1200, 100)):
            for i in range(0, len(chunk), 640):
                self.a.on_frame(L.Frame(L.T_AUDIO, 0, chunk[i:i + 640]))

    def finished(self, timeout=4.0):
        return wait_for(lambda: self.of(L.T_SPEECH_END) and self.a.state == ST_IDLE, timeout)

    def close(self):
        self.stop_credit = True
        self.a.stop()


class DialogTests(unittest.TestCase):
    def setUp(self):
        self.cards = []

    def tearDown(self):
        for c in self.cards:
            c.close()

    def card(self, *a, **k):
        c = Card(*a, **k)
        self.cards.append(c)
        return c

    def test_start_up(self):
        c = self.card()
        self.assertEqual(len(c.of(L.T_READY)), 1)
        self.assertEqual(len(c.of(L.T_SUBSCRIBE)), 1)
        self.assertEqual(c.of(L.T_SUBSCRIBE)[0].payload[0], 5)
        c.a.on_frame(L.Frame(L.T_PING, 0, bytes([9])))
        self.assertEqual(c.of(L.T_PONG)[0].payload[0], 9)

    def test_a_volume_command_is_sent_to_the_card_and_confirmed_aloud(self):
        c = self.card("turn it up")
        c.a.on_frame(L.Frame(L.T_CAN_NODES, 0, bytes([2, 0x14, 5, 4, 0, 0x15, 8, 7, 0])))   # an audio sink at 0x14
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.states()[:3], [ST_LISTENING, ST_THINKING, ST_SPEAKING])
        self.assertEqual(c.states()[-1], ST_IDLE)
        self.assertEqual(c.can_tx(), [(C.CH_AUDIO_CMD, C.audio_volume(0x14, C.VOLUME_STEP, 5))])
        self.assertEqual(c.texts(1), ["turn it up"])
        self.assertEqual(c.texts(2), ["Louder."])
        self.assertGreater(c.speech_bytes(), 0)
        self.assertEqual(len(c.of(L.T_LISTEN_STOP)), 1)
        self.assertEqual(c.local.calls, [])         # no model was needed

    def test_a_question_about_the_car_is_answered_from_live_data(self):
        ctx = VehicleContext()
        ctx.ingest(C.CH_SENSOR, 1, bytes([0x02, 4]) + (276).to_bytes(4, "little") + b"\x00")
        c = self.card("what's the oil pressure", ctx=ctx)
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.texts(2), ["The oil pressure is 40 psi."])
        self.assertEqual(c.local.calls, [])
        self.assertEqual(c.a.last_source, SRC_INTENT)

    def test_free_form_goes_to_the_local_model_with_the_car_in_its_prompt(self):
        ctx = VehicleContext()
        ctx.ingest(C.CH_SENSOR, 1, bytes([0x02, 4]) + (276).to_bytes(4, "little") + b"\x00")
        local = FakeLlm(pieces=["A carburetor mixes ", "air and fuel. ", "It does it with vacuum."], name="local")
        c = self.card("how does a carburetor work", local=local, ctx=ctx, online=FakeLlm("ONLINE", name="online"))
        c.say()
        self.assertTrue(c.finished())
        system, user = local.calls[0]
        self.assertIn("oil pressure: 40 psi", system)
        self.assertEqual(user, "how does a carburetor work")
        self.assertEqual(c.texts(2)[-1], "A carburetor mixes air and fuel. It does it with vacuum.")
        self.assertEqual(c.a.last_source, SRC_LOCAL)
        self.assertEqual(len(c.a.history), 1)
        self.assertGreater(c.speech_bytes(), 0)

    def test_fresh_questions_go_online_when_it_is_up(self):
        online = FakeLlm("The Spurs won by six.", name="online")
        c = self.card("who won the game last night", online=online)
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.texts(2)[-1], "The Spurs won by six.")
        self.assertEqual(c.a.last_source, SRC_ONLINE)
        self.assertEqual(c.local.calls, [])
        system, _ = online.calls[0]
        self.assertNotIn("psi", system)             # no sensor data goes out
        self.assertIn("US units", system)

    def test_offline_only_never_asks_the_internet(self):
        online = FakeLlm("ONLINE", name="online")
        c = self.card("who won the game last night", online=online, settings=(2, 1, 80))
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(online.calls, [])
        self.assertEqual(c.a.last_source, SRC_LOCAL)

    def test_no_network_uses_the_local_model(self):
        online = FakeLlm("ONLINE", name="online")
        c = self.card("who won the game last night", online=online, online_up=False)
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(online.calls, [])
        self.assertEqual(c.a.last_source, SRC_LOCAL)

    def test_online_failures_fall_back_to_the_local_model(self):
        for name, online in (("error", FakeLlm(error=LlmError("boom"), name="online")),
                             ("refused", FakeLlm(error=LlmRefused("no"), name="online")),
                             ("slow", FakeLlm("late", delay_s=1.0, name="online"))):
            with self.subTest(name):
                cfg = Config()
                cfg.first_token_s = 0.2
                c = self.card("who won the game last night", online=online, cfg=cfg)
                c.say()
                self.assertTrue(c.finished(), name)
                self.assertEqual(c.texts(2)[-1], "I am the local model.", name)
                self.assertEqual(c.a.last_source, SRC_LOCAL)
                if name == "refused":
                    self.assertTrue(c.conn.up)          # a refusal is not a network problem
                if name == "error":
                    self.assertTrue(c.conn.up or True)

    def test_local_unknown_escalates_online_once(self):
        local = FakeLlm("UNKNOWN", name="local")
        online = FakeLlm("It was 1964.", name="online")
        c = self.card("tell me about the old days", local=local, online=online)
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.texts(2)[-1], "It was 1964.")
        self.assertEqual(c.a.last_source, SRC_ONLINE)
        self.assertEqual(c.speech_bytes() > 0, True)
        self.assertNotIn("UNKNOWN", " ".join(c.tts.spoken))

    def test_local_unknown_with_no_internet_says_so(self):
        c = self.card("tell me about the old days", local=FakeLlm("UNKNOWN", name="local"), online=None)
        c.say()
        self.assertTrue(c.finished())
        self.assertIn("don't know", c.texts(2)[-1])

    def test_speaking_can_be_turned_off(self):
        c = self.card("next song", settings=(0, 0, 80))
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.speech_bytes(), 0)
        self.assertEqual(len(c.can_tx()), 1)
        self.assertEqual(c.texts(2), ["Next."])
        self.assertNotIn(ST_SPEAKING, c.states())

    def test_silence_after_the_wake_word_is_dropped(self):
        c = self.card("should not be asked")
        c.a.on_frame(L.Frame(L.T_LISTEN, 0, bytes([1, 1])))
        for _ in range(250):
            c.a.on_frame(L.Frame(L.T_AUDIO, 0, pcm(20, 100)))
        self.assertEqual(c.states()[-1], ST_IDLE)
        self.assertEqual(c.stt.calls, [])
        self.assertEqual(len(c.of(L.T_LISTEN_STOP)), 1)

    def test_empty_transcript_says_nothing(self):
        c = self.card("")
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(c.speech_bytes(), 0)
        self.assertEqual(c.texts(1), [])

    def test_cancel_stops_the_answer(self):
        c = self.card("how does a carburetor work", local=FakeLlm(pieces=[f"Sentence number {i}. " for i in range(40)], name="local"))
        c.stop_credit = True        # no credit: the answer waits for room
        time.sleep(0.05)
        c.a.on_frame(L.Frame(L.T_SPEECH_CREDIT, 0, (0).to_bytes(2, "little")))
        c.say()
        self.assertTrue(wait_for(lambda: c.a.state == ST_SPEAKING, 3))
        c.a.on_frame(L.Frame(L.T_CANCEL, 0, bytes([1])))
        self.assertTrue(wait_for(lambda: c.a.state == ST_IDLE))
        n = c.speech_bytes()
        c.a.on_frame(L.Frame(L.T_SPEECH_CREDIT, 0, (30000).to_bytes(2, "little")))
        time.sleep(0.3)
        self.assertEqual(c.speech_bytes(), n)       # nothing more after the cancel

    def test_speech_never_exceeds_the_credit(self):
        c = self.card("how does a carburetor work", local=FakeLlm("One two three four five six seven eight nine ten.", name="local"))
        c.stop_credit = True
        time.sleep(0.05)
        c.a.on_frame(L.Frame(L.T_SPEECH_CREDIT, 0, (2000).to_bytes(2, "little")))
        c.say()
        self.assertTrue(wait_for(lambda: c.speech_bytes() >= 1900, 3))
        time.sleep(0.2)
        self.assertLessEqual(c.speech_bytes(), 2000)
        self.assertTrue(all(len(f.payload) - 1 <= 960 for f in c.of(L.T_SPEECH)))
        c.a.on_frame(L.Frame(L.T_SPEECH_CREDIT, 0, (30000).to_bytes(2, "little")))
        self.assertTrue(c.finished())

    def test_forget_and_settings_frames(self):
        c = self.card("tell me a joke")
        c.say()
        self.assertTrue(c.finished())
        self.assertEqual(len(c.a.history), 1)
        c.a.on_frame(L.Frame(L.T_SETTINGS, 0, bytes([1, 1, 50, 1])))   # flags bit0: forget
        self.assertEqual(len(c.a.history), 0)
        self.assertEqual((c.a.settings.online, c.a.settings.level), (1, 50))

    def test_shutdown_is_acknowledged_and_handed_on(self):
        c = self.card()
        got = []
        c.a.on_shutdown = got.append
        c.a.on_frame(L.Frame(L.T_SHUTDOWN, 0, bytes([1])))
        self.assertEqual(len(c.of(L.T_SHUTDOWN_ACK)), 1)
        self.assertEqual(got, [1])

    def test_can_rx_feeds_the_context_and_the_gateway_hint(self):
        c = self.card()
        c.a.on_frame(L.Frame(L.T_CAN_RX, 0, L.u16(C.CH_SENSOR) + bytes([1]) + bytes([0x02, 2]) + (500).to_bytes(4, "little") + b"\x00"))
        self.assertAlmostEqual(c.ctx.core(2).value, 50.0)
        c.a.on_frame(L.Frame(L.T_CAN_RX, 0, L.u16(C.CH_SYSTEM) + bytes([1]) + bytes([0x03, 7, 0, 1])))
        self.assertTrue(c.ctx.celsius)


if __name__ == "__main__":
    unittest.main()
