import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from types import SimpleNamespace

from elc_ai.llm import AnthropicOnline, LlamaServer, LlmError, LlmRefused
from elc_ai.stt import WhisperServerStt


class Server:
    def __init__(self, handler):
        self.httpd = HTTPServer(("127.0.0.1", 0), handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_port}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


class LlamaTests(unittest.TestCase):
    def test_streams_sse_and_sends_history(self):
        seen = {}

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def do_POST(self):
                seen["body"] = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                seen["path"] = self.path
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for piece in ("Hel", "lo", " there."):
                    self.wfile.write(f'data: {json.dumps({"choices": [{"delta": {"content": piece}}]})}\n\n'.encode())
                self.wfile.write(b'data: {"choices":[{"delta":{}}]}\n\ndata: [DONE]\n\n')

        s = Server(H)
        try:
            out = list(LlamaServer(s.url).stream("SYS", [("hi", "hello")], "q", 50))
        finally:
            s.close()
        self.assertEqual("".join(out), "Hello there.")
        self.assertEqual(seen["path"], "/v1/chat/completions")
        roles = [m["role"] for m in seen["body"]["messages"]]
        self.assertEqual(roles, ["system", "user", "assistant", "user"])
        self.assertTrue(seen["body"]["stream"])
        self.assertEqual(seen["body"]["max_tokens"], 50)

    def test_connection_errors_become_llm_errors(self):
        with self.assertRaises(LlmError):
            list(LlamaServer("http://127.0.0.1:1", timeout_s=1).stream("s", [], "q"))


class WhisperTests(unittest.TestCase):
    def test_multipart_upload_and_cleanup(self):
        seen = {}

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def do_POST(self):
                seen["path"] = self.path
                seen["type"] = self.headers["Content-Type"]
                seen["body"] = self.rfile.read(int(self.headers["Content-Length"]))
                out = json.dumps({"text": " turn it up [BLANK_AUDIO]"}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

        s = Server(H)
        try:
            text = WhisperServerStt(s.url, prompt="El Camino").transcribe(b"\x01\x00" * 1600)
        finally:
            s.close()
        self.assertEqual(text, "turn it up")
        self.assertEqual(seen["path"], "/inference")
        self.assertTrue(seen["type"].startswith("multipart/form-data; boundary="))
        self.assertIn(b"RIFF", seen["body"])
        self.assertIn(b"El Camino", seen["body"])


class FakeStream:
    def __init__(self, pieces, stop, content=None):
        self.pieces, self.stop, self.content = pieces, stop, content or [SimpleNamespace(type="text")]
        self.text_stream = iter(pieces)

    def __enter__(self): return self
    def __exit__(self, *a): return False
    def get_final_message(self): return SimpleNamespace(stop_reason=self.stop, content=self.content)


class FakeClient:
    def __init__(self, streams, raise_on=None):
        self.streams, self.calls, self.raise_on = list(streams), [], raise_on
        self.messages = self

    def stream(self, **kw):
        self.calls.append(kw)
        if self.raise_on is not None:
            raise self.raise_on
        return self.streams.pop(0)


class AnthropicTests(unittest.TestCase):
    def test_request_shape(self):
        c = FakeClient([FakeStream(["It is ", "sunny."], "end_turn")])
        out = list(AnthropicOnline(client=c).stream("SYS", [("a", "b")], "q"))
        self.assertEqual("".join(out), "It is sunny.")
        kw = c.calls[0]
        self.assertEqual(kw["model"], "claude-opus-5-5")
        self.assertEqual(kw["system"], "SYS")
        self.assertEqual(kw["output_config"], {"effort": "low"})
        self.assertEqual(kw["tools"], [{"type": "web_search_20260209", "name": "web_search", "max_uses": 3}])
        self.assertEqual([m["role"] for m in kw["messages"]], ["user", "assistant", "user"])
        self.assertNotIn("thinking", kw)                 # adaptive by default on this model: nothing to disable
        self.assertNotIn("temperature", kw)              # sampling parameters are rejected by this model

    def test_no_search_when_turned_off(self):
        c = FakeClient([FakeStream(["x"], "end_turn")])
        list(AnthropicOnline(client=c, web_search=False).stream("s", [], "q"))
        self.assertNotIn("tools", c.calls[0])

    def test_a_paused_turn_is_continued(self):
        c = FakeClient([FakeStream(["Searching. "], "pause_turn", content=["block"]), FakeStream(["Done."], "end_turn")])
        out = list(AnthropicOnline(client=c).stream("s", [], "q"))
        self.assertEqual("".join(out), "Searching. Done.")
        self.assertEqual(len(c.calls), 2)
        self.assertEqual(c.calls[1]["messages"][-1], {"role": "assistant", "content": ["block"]})

    def test_a_refusal_is_reported(self):
        c = FakeClient([FakeStream([], "refusal")])
        with self.assertRaises(LlmRefused):
            list(AnthropicOnline(client=c).stream("s", [], "q"))

    def test_sdk_errors_become_llm_errors(self):
        c = FakeClient([], raise_on=ConnectionError("down"))
        with self.assertRaises(LlmError):
            list(AnthropicOnline(client=c).stream("s", [], "q"))


if __name__ == "__main__":
    unittest.main()
