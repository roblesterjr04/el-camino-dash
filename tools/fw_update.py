#!/usr/bin/env python3
"""Firmware update over CAN from a laptop (docs/protocols/can_channels.md, BULK, The firmware profile).

The same update the console's Wi-Fi page does, for the bench and for recovery: this program is the service tool
(node 0x1F) and sends an .elcfw file as a BULK firmware WRITE. Every node of the file's product and build takes it at
once (the 4" build reaches the left and right screens together), checks it, writes its spare OTA slot and restarts
into it. --dest sends it to one node only.

How it reaches the bus:
  --can IFACE[:CHANNEL]   a USB-CAN adapter on the main board's CAN tap (J16), through python-can
                          (pip install python-can), e.g. --can socketcan:can0, --can slcan:/dev/ttyACM0,
                          --can pcan:PCAN_USBBUS1, --can gs_usb:0. 500 kbit/s.
  --usb PORT              one board on USB as a gateway (docs/protocols/usb_link.md), for the bench: the frames go
                          out on its CAN bus. pip install pyserial.

    python3 tools/fw_update.py --can socketcan:can0 --list
    python3 tools/fw_update.py --can socketcan:can0 firmware/esp32_p4_display/.pio/build/display-waveshare4c/display-waveshare4c.elcfw
    python3 tools/fw_update.py --usb /dev/cu.usbmodem1101 --dest 0x13 console.elcfw
    python3 tools/fw_update.py --test       # against the real receiver code (needs a C++ compiler), no hardware

The car must stand still: a screen refuses an update while the sensors say it moves.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "firmware", "scripts"))

from chan_sim import Reassembler, transfer_frames  # noqa: E402
import elc_fw  # noqa: E402

CH_BULK, CH_BULK_DATA, CH_NET = 0x320, 0x340, 0x3C0
BULK_INFO, BULK_WRITE, BULK_WANT, BULK_END = 0x01, 0x82, 0x83, 0x84
KIND_FIRMWARE = 2
REFUSED = 0xFF
CHUNK = 5
SERVICE_NODE = 0x1F
END_TEXT = {0: "updated, restarting", 1: "failed: CRC mismatch", 2: "failed: flash", 3: "aborted / lost contact",
            4: "failed: image rejected"}
WHY_TEXT = {1: "does not take this", 2: "skipped (another build)", 3: "refused: too big", 4: "busy",
            5: "refused: the car is moving"}


class Receiver:
    def __init__(self, node: int) -> None:
        self.node = node
        self.state = "accepted"  # accepted, done, failed, refused
        self.result = 0
        self.acked = 0
        self.cursor = 0
        self.end = 0
        self.heard = 0.0
        self.last_first = -1

    def text(self) -> str:
        if self.state == "accepted":
            return "receiving"
        if self.state == "refused":
            return WHY_TEXT.get(self.result, "refused")
        return END_TEXT.get(self.result, "failed (%d)" % self.result)


class Writer:
    """The writing end of a BULK WRITE: ElcBulkWriter in firmware/elc_can/src/elc_bulk.cpp, the same rules."""

    COLLECT_MS, RETRY_MS, TRIES, SILENT_MS = 600, 1000, 3, 6000

    def __init__(self, node: int, send) -> None:
        self.node = node
        self.send = send  # send(can_id, data) -> bool
        self.tid = 0
        self.tag = 0x40
        self.session = 0
        self.state = "idle"
        self.rx: dict[int, Receiver] = {}
        self.pace_x10 = 25
        self.frames = 0
        self.resends = 0

    def transfer(self, payload: bytes) -> None:
        for f in transfer_frames(payload, self.tid):
            self.send(CH_BULK + self.node, f)
        self.tid = (self.tid + 1) & 0x1F

    def start(self, data: bytes, h: dict, dest: int, now: float) -> None:
        self.data = data
        self.dest = dest
        self.tag = (self.tag + 1) & 0xFF
        self.session = (self.session + 1) & 7
        self.chunks = (len(data) + CHUNK - 1) // CHUNK
        variant = h["variant"].encode()[:32]
        self.write_msg = struct.pack("<BBBBHIIBH", BULK_WRITE, dest, self.tag, KIND_FIRMWARE, h["product"], len(data),
                                     h["image_crc"], self.session, h["fw"]) + variant
        self.rx = {}
        self.tries = 0
        self.tokens = 0.0
        self.last = now
        self.high = 0
        self.state = "offering"
        self.send_write(now)

    def send_write(self, now: float) -> None:
        self.transfer(self.write_msg)
        self.write_ms = now
        self.tries += 1

    @property
    def busy(self) -> bool:
        return self.state in ("offering", "sending")

    def low_water(self) -> int:
        acc = [r.acked for r in self.rx.values() if r.state == "accepted"]
        return min(acc) if acc else self.chunks

    def progress(self) -> float:
        acc = [r.acked for r in self.rx.values() if r.state == "accepted"]
        if acc:
            return min(acc) / self.chunks
        return 1.0 if self.succeeded() else 0.0

    def succeeded(self) -> bool:
        st = [r.state for r in self.rx.values()]
        return "done" in st and "failed" not in st and "accepted" not in st

    def on_transfer(self, src: int, p: bytes, now: float) -> None:
        if not self.busy or not p or (self.dest and src != self.dest):
            return
        r = self.rx.get(src)
        op = p[0]
        if op == BULK_INFO and len(p) >= 12:
            to, tag, session = p[1], p[2], p[3]
            if to != self.node or tag != self.tag:
                return
            if r is None:
                r = Receiver(src)
                if session == REFUSED:
                    r.state, r.result = "refused", (p[12] if len(p) > 12 else 0)
                elif self.state == "sending" and self.low_water() > 0:
                    r.state, r.result = "failed", 3  # too late to join a multicast
                self.rx[src] = r
            r.heard = now
            if self.dest and r.state == "accepted":
                self.state = "sending"
            if self.dest and r.state == "refused":
                self.state = "done"
        elif op == BULK_WANT and len(p) >= 7:
            dest, session = p[1], p[2] & 7
            first = p[3] | (p[4] << 8) | (p[5] << 16)
            credit = p[6]
            if dest != self.node or session != self.session:
                return
            if r is None:
                if first != 0 or (self.state == "sending" and self.low_water() > 0):
                    return
                r = self.rx[src] = Receiver(src)
            if r.state != "accepted" or first > self.chunks:
                return
            r.heard = now
            again = first == r.last_first or credit == 1 or first > r.cursor
            r.last_first = first
            r.acked = first
            if again:
                r.cursor = first
            r.end = min(first + credit, self.chunks)
            if self.dest:
                self.state = "sending"
        elif op == BULK_END and len(p) >= 4:
            dest, session, result = p[1], p[2] & 7, p[3]
            if dest != self.node or session != self.session or r is None or r.state != "accepted":
                return
            r.state, r.result = ("done" if result == 0 else "failed"), result

    def abort(self) -> None:
        if self.busy:
            self.transfer(bytes([BULK_END, self.dest, self.session, 3]))
            for r in self.rx.values():
                if r.state == "accepted":
                    r.state, r.result = "failed", 3
            self.state = "done"

    def poll(self, now: float) -> None:
        dt = now - self.last
        self.last = now
        if not self.busy:
            return
        if self.state == "offering":
            if now - self.write_ms < (self.RETRY_MS if self.dest else self.COLLECT_MS):
                return
            if any(r.state == "accepted" for r in self.rx.values()):
                self.state = "sending"
            elif self.tries < self.TRIES:
                self.send_write(now)
                return
            else:
                self.state = "done"
                return
        for r in self.rx.values():
            if r.state == "accepted":
                if not r.heard:
                    r.heard = now
                elif now - r.heard > self.SILENT_MS:
                    r.state, r.result = "failed", 3
        if not any(r.state == "accepted" for r in self.rx.values()):
            self.state = "done"
            return
        self.tokens = min(self.tokens + dt * self.pace_x10 / 10.0, 16.0)
        while self.tokens >= 1.0:
            want = [r.cursor for r in self.rx.values() if r.state == "accepted" and r.cursor < r.end]
            if not want:
                break
            c = min(want)
            chunk = self.data[c * CHUNK:(c + 1) * CHUNK]
            h = ((self.session & 7) << 21) | (c & 0x1FFFFF)
            if not self.send(CH_BULK_DATA + self.node, struct.pack("<I", h)[:3] + chunk):
                break
            self.tokens -= 1.0
            self.frames += 1
            if c < self.high:
                self.resends += 1
            self.high = max(self.high, c + 1)
            for r in self.rx.values():
                if r.state == "accepted" and r.cursor == c and r.cursor < r.end:
                    r.cursor += 1


# ---- ways onto the bus ----

class PythonCan:
    def __init__(self, spec: str) -> None:
        try:
            import can  # type: ignore
        except ImportError:
            sys.exit("python-can is not installed: pip install python-can")
        iface, _, channel = spec.partition(":")
        kw = {"interface": iface, "bitrate": 500000}
        if channel:
            kw["channel"] = int(channel) if channel.isdigit() and iface == "gs_usb" else channel
        self.can = can
        self.bus = can.Bus(**kw)

    def send(self, can_id: int, data: bytes) -> bool:
        try:
            self.bus.send(self.can.Message(arbitration_id=can_id, data=data, is_extended_id=False), timeout=0.05)
            return True
        except self.can.CanError:
            return False

    def recv(self):
        out = []
        while True:
            m = self.bus.recv(timeout=0)
            if m is None:
                return out
            if not m.is_extended_id and not m.is_remote_frame:
                out.append((m.arbitration_id, bytes(m.data)))

    def close(self) -> None:
        self.bus.shutdown()


class UsbLink:
    """One board's USB link in gateway mode (can_relay.py speaks the same records)."""

    def __init__(self, port: str) -> None:
        try:
            import serial  # type: ignore
        except ImportError:
            sys.exit("pyserial is not installed: pip install pyserial")
        import can_relay as cr
        self.cr = cr
        self.ser = serial.Serial(port, cr.DEFAULT_BAUD, timeout=0)
        self.parser = cr.Parser()
        self.ka = 0.0

    def send(self, can_id: int, data: bytes) -> bool:
        self.ser.write(self.cr.frame_record(can_id, data))
        return True

    def recv(self):
        now = time.monotonic()
        if now - self.ka > 0.5:
            self.ka = now
            self.ser.write(self.cr.keepalive_record(True, 0))
        out = []
        for kind, p in self.parser.feed(self.ser.read(4096)):
            if kind == "rec" and p[:1] == b"F" and len(p) >= 4:
                can_id, n = struct.unpack_from("<HB", p, 1)
                out.append((can_id, bytes(p[4:4 + n])))
        return out

    def close(self) -> None:
        self.ser.close()


def announces(p: bytes) -> dict | None:
    if len(p) < 19 or p[0] != 0x01:
        return None
    ver, typ, product, fw, uid, gen, prod, cons, flags = struct.unpack_from("<BBHHIBIIB", p, 1)
    return {"type": typ, "product": product, "fw": fw, "name": p[20:].decode("utf-8", "replace")}


def list_nodes(bus) -> None:
    for f in transfer_frames(bytes([0x81, 0x00, 0x01]), 0):  # NET QUERY to everyone: ANNOUNCE
        bus.send(CH_NET + SERVICE_NODE, f)
    rs = Reassembler()
    seen = {}
    t0 = time.monotonic()
    while time.monotonic() - t0 < 1.5:
        for can_id, data in bus.recv():
            if can_id & ~0x1F == CH_NET:
                p = rs.feed(can_id, data, time.monotonic())
                a = announces(p) if p else None
                if a:
                    seen[can_id & 0x1F] = a
        time.sleep(0.002)
    if not seen:
        print("no node answered")
    for node in sorted(seen):
        a = seen[node]
        print("0x%02X  %-20s product %d (%s), firmware v%d" % (node, a["name"], a["product"],
                                                              elc_fw.PRODUCTS.get(a["product"], "?"), a["fw"]))


def run(bus, data: bytes, h: dict, dest: int, pace_x10: int) -> bool:
    w = Writer(SERVICE_NODE, bus.send)
    w.pace_x10 = pace_x10
    rs = Reassembler()
    t0 = time.monotonic()
    ms = lambda: (time.monotonic() - t0) * 1000.0  # noqa: E731
    w.start(data, h, dest, ms())
    shown = -1.0
    try:
        while w.busy:
            now = ms()
            for can_id, d in bus.recv():
                if can_id & ~0x1F == CH_BULK:
                    p = rs.feed(can_id, d, now / 1000.0)
                    if p:
                        w.on_transfer(can_id & 0x1F, p, now)
            w.poll(now)
            if now - shown >= 500:
                shown = now
                who = ", ".join("0x%02X %s" % (r.node, r.text()) for r in w.rx.values()) or "asking"
                sys.stdout.write("\r%5.1f%%  %s  (%d frames, %d resent)   " % (w.progress() * 100, who, w.frames,
                                                                             w.resends))
                sys.stdout.flush()
            time.sleep(0.0003)
    except KeyboardInterrupt:
        w.abort()
        print("\naborted")
        return False
    print()
    for r in w.rx.values():
        print("0x%02X: %s" % (r.node, r.text()))
    if not w.rx:
        print("no node of product %d, build %s answered" % (h["product"], h["variant"]))
    print("%.1f s, %d frames, %d resent" % (ms() / 1000.0, w.frames, w.resends))
    return w.succeeded()


# ---- self-test against the firmware's own receiver ----

class PipeTarget:
    """firmware/elc_can/test/host/fw_target_pipe.cpp: the real ElcFwTarget, fed line by line."""

    def __init__(self, exe: str, node: int, product: int, variant: str, drop: int = 0) -> None:
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.node = node
        self.cmd("n %x %x %s" % (node, product, variant))
        if drop:
            self.cmd("drop %d" % drop)

    def cmd(self, line: str) -> None:
        self.p.stdin.write(line + "\n")

    def tick(self, now_ms: int, frames) -> list:
        for can_id, data in frames:
            self.cmd("f %03x %s" % (can_id, data.hex()))
        self.cmd("t %d" % now_ms)
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline().strip()
            if line == "." or not line:
                return out
            if line.startswith("f "):
                _, i, d = (line.split() + [""])[:3]
                out.append((int(i, 16), bytes.fromhex(d)))

    def result(self) -> tuple[int, int, int]:
        self.cmd("q")
        self.p.stdin.flush()
        r = self.p.stdout.readline().split()
        self.p.wait()
        return int(r[1]), int(r[2]), int(r[3], 16)


def _test() -> None:
    import random
    import zlib
    lib = os.path.join(REPO, "firmware", "elc_can")
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "fw_target_pipe")
    subprocess.run(["c++", "-std=c++17", "-O1", "-I", os.path.join(lib, "include"),
                    os.path.join(lib, "test", "host", "fw_target_pipe.cpp"), os.path.join(lib, "src", "elc_chan.cpp"),
                    os.path.join(lib, "src", "elc_bulk.cpp"), os.path.join(lib, "src", "elc_fw.cpp"),
                    "-o", exe], check=True)
    random.seed(5)
    image = bytes([0xE9]) + bytes(random.getrandbits(8) for _ in range(30000))
    data = elc_fw.make_header(image, 1, "display-waveshare4c", 3) + image

    def sim(targets, data, dest=0, max_ms=60000):
        h = elc_fw.check_file(data)
        queue = []  # frames on their way (3 a millisecond)
        w = Writer(SERVICE_NODE, lambda i, d: queue.append((i, d)) or True)
        rs = Reassembler()
        w.start(data, h, dest, 0)
        now = 0
        while w.busy and now < max_ms:
            now += 1
            batch, queue[:] = queue[:3], queue[3:]
            for t in targets:
                for can_id, d in t.tick(now, batch):
                    if can_id & ~0x1F == CH_BULK:
                        p = rs.feed(can_id, d, now / 1000.0)
                        if p:
                            w.on_transfer(can_id & 0x1F, p, now)
            w.poll(now)
        return w, now

    crc = zlib.crc32(image) & 0xFFFFFFFF
    ok = True
    # both 4" screens; the 3.4" center stays out of it; one 4" screen loses a data frame in 29
    ts = [PipeTarget(exe, 0x02, 1, "display-waveshare4c"), PipeTarget(exe, 0x03, 1, "display-waveshare4c", drop=29),
          PipeTarget(exe, 0x01, 1, "display-waveshare3c4")]
    w, ms = sim(ts, data)
    res = [t.result() for t in ts]
    good = w.succeeded() and res[0] == (3, len(image), crc) and res[1] == (3, len(image), crc) and res[2][0] == 0 \
        and w.rx[0x01].state == "refused" and w.rx[0x01].result == 2
    print("multicast: %s in %d ms, %d frames for %d chunks, %d resent" % (
        "ok" if good else "FAILED", ms, w.frames, w.chunks, w.resends))
    ok &= good
    # one node by number: the console
    t = PipeTarget(exe, 0x13, 3, "console")
    w, ms = sim([t], elc_fw.make_header(image, 3, "console") + image, dest=0x13)
    good = w.succeeded() and t.result() == (3, len(image), crc)
    print("one node:  %s in %d ms" % ("ok" if good else "FAILED", ms))
    ok &= good
    # a screen file with no screen on the bus: three WRITEs, then nobody
    t = PipeTarget(exe, 0x13, 3, "console")
    w, ms = sim([t], data)
    good = not w.succeeded() and not w.rx and t.result()[0] == 0
    print("nobody:    %s (%d ms, no receivers)" % ("ok" if good else "FAILED", ms))
    ok &= good
    print("all fw_update tests passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="an .elcfw file (PlatformIO writes it next to firmware.bin)")
    ap.add_argument("--can", metavar="IFACE[:CHANNEL]", help="python-can interface and channel")
    ap.add_argument("--usb", metavar="PORT", help="a board's USB port, as a gateway onto its bus")
    ap.add_argument("--dest", type=lambda s: int(s, 0), default=0, help="one node (default: every node of the build)")
    ap.add_argument("--pace", type=float, default=2.5, help="data frames per millisecond (default 2.5)")
    ap.add_argument("--list", action="store_true", help="list the nodes on the bus and their firmware")
    ap.add_argument("--test", action="store_true", help="self-test against the firmware's receiver (no hardware)")
    a = ap.parse_args()
    if a.test:
        _test()
    if not a.can and not a.usb:
        ap.error("--can or --usb is needed")
    bus = PythonCan(a.can) if a.can else UsbLink(a.usb)
    try:
        if a.list:
            list_nodes(bus)
            return
        if not a.file:
            ap.error("an .elcfw file is needed")
        data = open(a.file, "rb").read()
        try:
            h = elc_fw.check_file(data)
        except ValueError as e:
            sys.exit("%s: %s" % (a.file, e))
        print("%s: %s" % (os.path.basename(a.file), elc_fw.describe(h)))
        sys.exit(0 if run(bus, data, h, a.dest, int(a.pace * 10)) else 1)
    finally:
        bus.close()


if __name__ == "__main__":
    main()
