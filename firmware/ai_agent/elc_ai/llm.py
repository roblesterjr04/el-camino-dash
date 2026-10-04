"""Language models: the on-board one (llama.cpp's server) and the online one (Claude, through the Anthropic SDK, with
its web search tool). Both stream text so the first sentence can be spoken while the rest is still coming."""
from __future__ import annotations

import json
import os
import queue
import threading
import urllib.request
from typing import Callable, Iterator, List, Optional, Sequence, Tuple

History = Sequence[Tuple[str, str]]  # (user, assistant) turns


class LlmError(Exception):
    pass


class LlmRefused(LlmError):
    """The online model declined (a safety stop). The on-board model answers instead."""


class FirstTokenTimeout(LlmError):
    pass


class LlmBackend:
    name = "llm"

    def stream(self, system: str, history: History, user: str, max_tokens: int = 200) -> Iterator[str]:  # pragma: no cover
        raise NotImplementedError


def _messages(history: History, user: str) -> List[dict]:
    m: List[dict] = []
    for u, a in history:
        m.append({"role": "user", "content": u})
        m.append({"role": "assistant", "content": a})
    m.append({"role": "user", "content": user})
    return m


class LlamaServer(LlmBackend):
    """llama.cpp's `llama-server`: its OpenAI-compatible /v1/chat/completions, on this module only (127.0.0.1)."""
    name = "local"

    def __init__(self, url: str = "http://127.0.0.1:8080", model: str = "local", timeout_s: float = 90.0, temperature: float = 0.4):
        self.url, self.model, self.timeout, self.temperature = url.rstrip("/"), model, timeout_s, temperature

    def stream(self, system: str, history: History, user: str, max_tokens: int = 200) -> Iterator[str]:
        body = {"model": self.model, "stream": True, "max_tokens": max_tokens, "temperature": self.temperature,
                "messages": [{"role": "system", "content": system}] + _messages(history, user)}
        req = urllib.request.Request(self.url + "/v1/chat/completions", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                for raw in r:
                    line = raw.decode("utf-8", "ignore").strip()
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        return
                    try:
                        delta = json.loads(data)["choices"][0].get("delta", {}).get("content")
                    except (ValueError, KeyError, IndexError):
                        continue
                    if delta:
                        yield delta
        except OSError as e:
            raise LlmError(f"local model: {e}") from e


class AnthropicOnline(LlmBackend):
    """Claude through the Anthropic SDK (`pip install anthropic`), with the server-side web search tool. The key comes from
    the ANTHROPIC_API_KEY environment variable of the service (never from CAN, never from the card).

    Only the question, a short history and a small context line are sent: no audio, no sensor data, no position."""
    name = "online"

    def __init__(self, model: str = "claude-opus-5-5", effort: str = "low", web_search: bool = True, max_searches: int = 3,
                 client=None):
        self.model, self.effort, self.web_search, self.max_searches = model, effort, web_search, max_searches
        self._client = client

    def _get_client(self):
        if self._client is None:
            import anthropic  # imported late: the agent runs without it when the card is used offline
            self._client = anthropic.Anthropic(timeout=30.0, max_retries=1)
        return self._client

    def stream(self, system: str, history: History, user: str, max_tokens: int = 400) -> Iterator[str]:
        client = self._get_client()
        tools = []
        if self.web_search:
            tools = [{"type": "web_search_20260209", "name": "web_search", "max_uses": self.max_searches}]
        messages = _messages(history, user)
        for _ in range(4):  # a long web search can pause the turn: continue it a few times
            kwargs = dict(model=self.model, max_tokens=max_tokens, system=system, messages=messages,
                          output_config={"effort": self.effort})
            if tools:
                kwargs["tools"] = tools
            try:
                with client.messages.stream(**kwargs) as s:
                    for piece in s.text_stream:
                        yield piece
                    final = s.get_final_message()
            except LlmError:
                raise
            except Exception as e:  # SDK errors (rate limit, connection, status): the caller falls back to the local model
                raise LlmError(f"online model: {type(e).__name__}: {e}") from e
            if final.stop_reason == "refusal":
                raise LlmRefused("the online model declined")
            if final.stop_reason == "pause_turn":
                messages = messages + [{"role": "assistant", "content": final.content}]
                continue
            return


def first_token_guard(factory: Callable[[], Iterator[str]], timeout_s: float) -> Iterator[str]:
    """Runs a stream on a thread and raises FirstTokenTimeout when nothing arrives within timeout_s. After the first piece
    the rest is read without a deadline (the caller has something to say by then)."""
    q: "queue.Queue[Tuple[str, object]]" = queue.Queue()
    stop = threading.Event()

    def pump() -> None:
        try:
            for piece in factory():
                if stop.is_set():
                    return
                q.put(("t", piece))
            q.put(("end", None))
        except Exception as e:  # noqa: BLE001 - handed to the consumer
            q.put(("err", e))

    threading.Thread(target=pump, daemon=True).start()
    try:
        kind, val = q.get(timeout=timeout_s)
    except queue.Empty:
        stop.set()
        raise FirstTokenTimeout(f"nothing within {timeout_s:g} s")
    while True:
        if kind == "t":
            yield val  # type: ignore[misc]
        elif kind == "err":
            raise val  # type: ignore[misc]
        else:
            return
        kind, val = q.get()


class FakeLlm(LlmBackend):
    name = "fake"

    def __init__(self, reply: str = "", pieces: Optional[List[str]] = None, error: Optional[Exception] = None, delay_s: float = 0.0,
                 name: str = "fake"):
        self.reply, self.pieces, self.error, self.delay, self.name = reply, pieces, error, delay_s, name
        self.calls: List[Tuple[str, str]] = []

    def stream(self, system: str, history: History, user: str, max_tokens: int = 200) -> Iterator[str]:
        self.calls.append((system, user))
        if self.delay:
            import time
            time.sleep(self.delay)
        if self.error:
            raise self.error
        for p in (self.pieces if self.pieces is not None else [self.reply]):
            yield p
