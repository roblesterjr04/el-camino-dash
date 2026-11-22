"""The assistant: one utterance from the card's microphone stream to the spoken answer.

    LISTEN, AUDIO ... -> end of speech -> STT -> intent engine -> (route) -> local or online model -> sentences -> TTS
    -> SPEECH frames within the card's credit -> SPEECH_END.

The link reader calls on_frame(); the work runs on one worker thread. docs/hardware/ai_card.md, "Routing"."""
from __future__ import annotations

import queue
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Deque, List, Optional, Tuple

from . import VERSION_U16
from . import canmsg as C
from . import link as L
from .config import Config
from .connectivity import Connectivity
from .context import VehicleContext
from .endpoint import Endpointer
from .intents import Reply, handle as handle_intent
from .llm import FirstTokenTimeout, LlmBackend, LlmError, LlmRefused, first_token_guard
from .retrieval import Manual
from .router import LOCAL, ONLINE, route
from .stt import SttBackend
from .tts import TtsBackend, clean_for_speech, split_sentences

ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING = 0, 1, 2, 3
SRC_NONE, SRC_INTENT, SRC_LOCAL, SRC_ONLINE = 0, 1, 2, 3
TEXT_HEARD, TEXT_ANSWER, TEXT_ERROR = 1, 2, 3
SPEECH_CHUNK = 960


@dataclass
class Settings:
    online: int = 0  # 0 auto, 1 always, 2 offline only
    speak: bool = True
    level: int = 80


class Cancelled(Exception):
    pass


class Assistant:
    def __init__(self, send: Callable[[bytes], None], ctx: VehicleContext, stt: SttBackend, tts: TtsBackend, local: LlmBackend,
                 online: Optional[LlmBackend], connectivity: Connectivity, cfg: Config, manual: Optional[Manual] = None,
                 soc_temp: Callable[[], int] = lambda: 0, on_shutdown: Callable[[int], None] = lambda reason: None,
                 clock: Callable[[], float] = time.monotonic):
        self._raw_send = send
        self._send_lock = threading.Lock()
        self._tx = L.Sender()
        self.ctx, self.stt, self.tts, self.local, self.online = ctx, stt, tts, local, online
        self.conn, self.cfg, self.manual = connectivity, cfg, manual
        self.soc_temp, self.on_shutdown, self.clock = soc_temp, on_shutdown, clock
        self.settings = Settings()
        self.history: Deque[Tuple[str, str]] = deque(maxlen=cfg.history_turns)
        self.card: dict = {}
        self.sink = 0
        self.card_ctx: dict = {}
        self.state = ST_IDLE
        self._ep: Optional[Endpointer] = None
        self._pcm = bytearray()
        self._gen = 0                      # incremented by every LISTEN / CANCEL: a job from an older one is dropped
        self._jobs: "queue.Queue[Tuple[int, bytes]]" = queue.Queue()
        self._credit = 0
        self._credit_cv = threading.Condition()
        self._tag = 0
        self.log: List[str] = []
        self.last_source = SRC_NONE
        self._worker = threading.Thread(target=self._work, daemon=True)
        self._stop = False
        self.started = False

    # ---- sending ----
    def send(self, ftype: int, payload: bytes = b"") -> None:
        with self._send_lock:
            self._raw_send(self._tx.frame(ftype, payload))

    def say_ready(self) -> None:
        flags = (1 if self.online is not None else 0) | (2 if self.local is not None else 0)
        self.send(L.T_READY, L.ready(VERSION_U16, flags, "elc-ai"))

    def _state(self, st: int, source: int = SRC_NONE) -> None:
        self.state = st
        self.send(L.T_STATE, L.state(st, source))

    def _can_tx(self, base: int, payload: bytes) -> None:
        self._tag = (self._tag + 1) & 0xFF
        self.send(L.T_CAN_TX, L.can_tx(self._tag, base, payload))

    def start(self) -> None:
        if not self.started:
            self.started = True
            self._worker.start()
        self.send(L.T_SUBSCRIBE, L.subscribe([C.CH_SYSTEM, C.CH_SENSOR, C.CH_ALERT, C.CH_ONLINE, C.CH_AUDIO_CMD]))
        self.send(L.T_CTX_REQUEST)
        self.say_ready()

    def stop(self) -> None:
        self._stop = True
        self._jobs.put((-1, b""))

    # ---- frames from the card ----
    def on_frame(self, f: L.Frame) -> None:
        t, p = f.type, f.payload
        if t == L.T_PING:
            temp = max(-128, min(127, self.soc_temp()))
            online_up = 1 if self.conn.up else 0
            self.send(L.T_PONG, L.pong(p[0] if p else 0, temp, 0, online_up))
        elif t == L.T_HELLO:
            if len(p) >= 8:
                self.card = {"proto": p[0], "fw": L.read_u16(p, 1), "slot": p[3], "node": p[4], "flags": p[5]}
        elif t == L.T_SETTINGS:
            if len(p) >= 4:
                self.settings = Settings(online=p[0], speak=bool(p[1]), level=p[2])
                if p[3] & 1:
                    self.history.clear()
        elif t == L.T_SHUTDOWN:
            self.send(L.T_SHUTDOWN_ACK)
            self.on_shutdown(p[0] if p else 0)
        elif t == L.T_LISTEN:
            self._begin_listening()
        elif t == L.T_AUDIO:
            self._audio(bytes(p))
        elif t == L.T_CANCEL:
            self._cancel()
        elif t == L.T_SPOKEN:
            if p and p[0] == 2:  # the card could not play it: stop sending
                self._cancel(keep_state=False)
        elif t == L.T_CAN_RX:
            if len(p) >= 3:
                base, src = L.read_u16(p, 0), p[2]
                self.ctx.ingest(base, src, bytes(p[3:]))
                if base == C.CH_ONLINE and p[3:4] == b"\x05" and len(p) > 4:
                    self.conn.hint(bool(p[4] & 1))
        elif t == L.T_CAN_NODES:
            nodes = {}
            n = p[0] if p else 0
            for i in range(n):
                o = 1 + i * 4
                if o + 4 <= len(p):
                    nodes[p[o]] = (p[o + 1], L.read_u16(p, o + 2))
            self.ctx.set_nodes(nodes)
            self.sink = next((nd for nd, (ty, _) in sorted(nodes.items()) if ty == 5), 0)
        elif t == L.T_CTX:
            if len(p) >= 5:
                self.card_ctx[p[0]] = L.read_s32(p, 1)
        elif t == L.T_SPEECH_CREDIT:
            if len(p) >= 2:
                with self._credit_cv:
                    self._credit = L.read_u16(p, 0)
                    self._credit_cv.notify_all()

    # ---- listening ----
    def _begin_listening(self) -> None:
        self._gen += 1
        self._ep = Endpointer(silence_ms=self.cfg.silence_ms)
        self._pcm = bytearray()
        self._state(ST_LISTENING)

    def _audio(self, pcm: bytes) -> None:
        if self._ep is None or self.state != ST_LISTENING:
            return
        self._pcm += pcm
        if self._ep.feed(pcm):
            reason = self._ep.reason
            audio = bytes(self._pcm)
            self._ep = None
            self._pcm = bytearray()
            self.send(L.T_LISTEN_STOP)
            if reason == "nospeech":
                self._state(ST_IDLE)
                return
            self._state(ST_THINKING)
            self._jobs.put((self._gen, audio))

    def _cancel(self, keep_state: bool = True) -> None:
        self._gen += 1
        self._ep = None
        self._pcm = bytearray()
        with self._credit_cv:
            self._credit_cv.notify_all()
        if self.state != ST_IDLE:
            self.state = ST_IDLE
            self.send(L.T_STATE, L.state(ST_IDLE, SRC_NONE))

    # ---- the worker ----
    def _work(self) -> None:
        while not self._stop:
            gen, audio = self._jobs.get()
            if gen < 0:
                return
            if gen != self._gen:
                continue
            try:
                self._run(gen, audio)
            except Cancelled:
                pass
            except Exception as e:  # noqa: BLE001 - a bug must not leave the card waiting
                self.log.append(f"error: {type(e).__name__}: {e}")
                self.send(L.T_TEXT, L.text(TEXT_ERROR, "Sorry, something went wrong."))
            finally:
                if gen == self._gen:
                    self.send(L.T_SPEECH_END)
                    if self.state != ST_IDLE:
                        self._state(ST_IDLE)

    def _check(self, gen: int) -> None:
        if gen != self._gen:
            raise Cancelled()

    def _run(self, gen: int, audio: bytes) -> None:
        text = self.stt.transcribe(audio)
        self._check(gen)
        if not text:
            return
        self.send(L.T_TEXT, L.text(TEXT_HEARD, text))
        reply = handle_intent(text, self.ctx, self.sink)
        if reply is not None:
            for base, payload in reply.actions:
                self._can_tx(base, payload)
            if reply.forget:
                self.history.clear()
            self._answer(gen, reply.text, SRC_INTENT)
            return
        answer, source = self._ask(gen, text)
        if answer:
            self.history.append((text, answer))

    def _answer(self, gen: int, text: str, source: int) -> None:
        self.last_source = source
        self.send(L.T_TEXT, L.text(TEXT_ANSWER, text))
        self._say(gen, text, source)

    # ---- asking a model ----
    def _system_local(self, question: str) -> str:
        s = (f"You are the voice assistant in {self.cfg.vehicle}. Your answers are spoken aloud: one or two short sentences, "
             "plain words, no lists, no markdown. Live data from the car:\n" + self.ctx.snapshot())
        if self.manual:
            hits = self.manual.search(question, k=2)
            if hits:
                s += "\n\nOwner's manual excerpts:\n" + "\n---\n".join(h[2] for h in hits)
        s += "\n\nIf the question needs current information from the internet, or you do not know the answer, reply with exactly: UNKNOWN"
        return s

    def _system_online(self) -> str:
        return (f"You are the voice assistant in {self.cfg.vehicle}. Your answers are spoken aloud: at most three short "
                "sentences, plain words, no lists, no markdown, no web addresses. " + self.ctx.online_context())

    def _online_ready(self) -> bool:
        return self.online is not None and self.settings.online != 2 and self.conn.up

    def _ask(self, gen: int, question: str) -> Tuple[str, int]:
        where = route(question, self.settings.online, self._online_ready())
        if where == ONLINE:
            got = self._stream_answer(gen, self.online, self._system_online(), question, SRC_ONLINE, guard=self.cfg.first_token_s,
                                      max_tokens=400)
            if got is not None:
                return got, SRC_ONLINE
            # fell through (timeout, error, refusal): the on-board model answers
        got = self._stream_answer(gen, self.local, self._system_local(question), question, SRC_LOCAL, max_tokens=self.cfg.local_max_tokens,
                                  watch_unknown=True)
        if got is None:  # UNKNOWN from the on-board model
            if self._online_ready():
                got = self._stream_answer(gen, self.online, self._system_online(), question, SRC_ONLINE, guard=self.cfg.first_token_s,
                                          max_tokens=400)
                if got is not None:
                    return got, SRC_ONLINE
            msg = "I don't know that one, and I can't look it up right now." if self.settings.online != 2 else "I don't know that one."
            self._answer(gen, msg, SRC_LOCAL)
            return msg, SRC_LOCAL
        return got, SRC_LOCAL

    def _stream_answer(self, gen: int, backend: Optional[LlmBackend], system: str, question: str, source: int, guard: float = 0.0,
                       max_tokens: int = 200, watch_unknown: bool = False) -> Optional[str]:
        """Speaks the answer sentence by sentence as it streams. Returns the text, or None when this backend gave nothing
        usable (error, no first token in time, refusal, or UNKNOWN) and nothing was spoken."""
        if backend is None:
            return None
        chime = threading.Timer(2.0, self._thinking_chime) if source == SRC_LOCAL else None
        if chime:
            chime.daemon = True
            chime.start()
        spoken_any = False
        full = ""
        pending = ""
        decided = not watch_unknown
        try:
            def factory():
                return backend.stream(system, list(self.history), question, max_tokens)
            stream = first_token_guard(factory, guard) if guard else factory()
            for piece in stream:
                self._check(gen)
                pending += piece
                if not decided:
                    if len(pending.strip()) < 7 and not pending.rstrip().endswith((".", "!", "?")):
                        continue
                    if pending.strip().upper().startswith("UNKNOWN"):
                        return None
                    decided = True
                sentences, pending = split_sentences(pending)
                for s in sentences:
                    spoken_any = self._emit(gen, s, source, spoken_any) or spoken_any
                    full += (" " if full else "") + s
            if not decided and pending.strip().upper().startswith("UNKNOWN"):
                return None
            if pending.strip():
                spoken_any = self._emit(gen, pending.strip(), source, spoken_any) or spoken_any
                full += (" " if full else "") + pending.strip()
        except (LlmError, FirstTokenTimeout, LlmRefused) as e:
            self.log.append(f"{getattr(backend, 'name', 'llm')}: {e}")
            if source == SRC_ONLINE and not isinstance(e, LlmRefused):
                self.conn.mark_failure()  # a refusal is not a network problem
            if spoken_any:
                return full  # part of an answer was already said: keep it, do not start another
            return None
        finally:
            if chime:
                chime.cancel()
        if full:
            self.last_source = source
            self.send(L.T_TEXT, L.text(TEXT_ANSWER, full))
        return full or None

    def _emit(self, gen: int, sentence: str, source: int, already: bool) -> bool:
        if not already and self.settings.speak:
            self._state(ST_SPEAKING, source)
        return self._say(gen, sentence, source, announce=False)

    def _thinking_chime(self) -> None:
        if self.state == ST_THINKING and self.sink:
            self._can_tx(C.CH_AUDIO_CMD, C.audio_play(self.sink, 2))

    # ---- speaking ----
    def _say(self, gen: int, text: str, source: int, announce: bool = True) -> bool:
        if not self.settings.speak:
            return False
        t = clean_for_speech(text)
        if not t:
            return False
        if announce and self.state != ST_SPEAKING:
            self._state(ST_SPEAKING, source)
        pcm = self.tts.synth(t)
        self._check(gen)
        code = self.tts.rate_code
        for i in range(0, len(pcm), SPEECH_CHUNK):
            chunk = pcm[i:i + SPEECH_CHUNK]
            if not self._wait_credit(gen, len(chunk)):
                raise Cancelled()
            self.send(L.T_SPEECH, L.speech(code, chunk))
        return True

    def _wait_credit(self, gen: int, n: int, timeout: float = 2.0) -> bool:
        """Waits for room on the card. False when this answer was cancelled (checked first, so a stale answer can never
        spend credit meant for the next one) or the card gave no room for `timeout` seconds."""
        deadline = self.clock() + timeout
        with self._credit_cv:
            while True:
                if gen != self._gen:
                    return False
                if self._credit >= n:
                    self._credit -= n
                    return True
                left = deadline - self.clock()
                if left <= 0:
                    return False
                self._credit_cv.wait(min(left, 0.1))
