"""Firmware update files (.elcfw) for the CAN and Wi-Fi updates (docs/protocols/can_channels.md, The firmware profile).

As a PlatformIO script (extra_scripts = pre:../scripts/elc_fw.py):
  - defines ELC_FW_VARIANT as the environment's name, so the firmware knows which build it is, and
  - after firmware.bin is linked, writes <env>.elcfw next to it: a 64-byte header (product, build, size, CRC-32, build
    time, git commit) followed by firmware.bin. That file is what the console's update page and tools/fw_update.py take.
  The product number comes from `custom_elc_product` in the environment (platformio.ini).

From the command line (the same file, by hand):
  python3 elc_fw.py pack firmware.bin --product 1 --variant display-waveshare4c -o display-waveshare4c.elcfw
  python3 elc_fw.py show some.elcfw
"""

import argparse
import struct
import subprocess
import sys
import time
import zlib

MAGIC = b"ELFW"
HEADER_SIZE = 64
HEADER_VERSION = 1
# The registry in docs/protocols/can_channels.md (Products)
PRODUCTS = {1: "round display", 2: "audio bridge", 3: "console", 4: "audio card", 5: "GPS card", 6: "LTE card",
            7: "AI voice card", 8: "sensor card", 9: "AM/FM radio card"}


def make_header(image, product, variant, fw=1, build_time=None, commit=0):
    v = variant.encode("ascii")
    if len(v) > 32:
        raise ValueError("variant longer than 32 bytes: %s" % variant)
    if build_time is None:
        build_time = int(time.time())
    head = struct.pack("<4sBBHHHIII32sI", MAGIC, HEADER_VERSION, HEADER_SIZE, product, fw, 0, len(image),
                       zlib.crc32(image) & 0xFFFFFFFF, build_time, v.ljust(32, b"\0"), commit)
    return head + struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)


def parse_header(data):
    """The header's fields as a dict, or raises ValueError."""
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ValueError("not an .elcfw file")
    (magic, ver, size, product, fw, flags, image_size, image_crc, build_time, variant, commit,
     hcrc) = struct.unpack("<4sBBHHHIII32sII", data[:HEADER_SIZE])
    if ver != HEADER_VERSION or size != HEADER_SIZE:
        raise ValueError("header version %d / size %d not understood" % (ver, size))
    if zlib.crc32(data[:60]) & 0xFFFFFFFF != hcrc:
        raise ValueError("header CRC does not match")
    return {"product": product, "fw": fw, "flags": flags, "image_size": image_size, "image_crc": image_crc,
            "build_time": build_time, "variant": variant.rstrip(b"\0").decode("ascii", "replace"), "commit": commit}


def check_file(data):
    """Parses and checks a whole .elcfw (header and image). Returns the header dict."""
    h = parse_header(data)
    image = data[HEADER_SIZE:]
    if len(image) != h["image_size"]:
        raise ValueError("image is %d bytes, header says %d" % (len(image), h["image_size"]))
    if zlib.crc32(image) & 0xFFFFFFFF != h["image_crc"]:
        raise ValueError("image CRC does not match the header")
    if not image or image[0] != 0xE9:
        raise ValueError("the image does not start like an ESP32 app image (0xE9)")
    return h


def describe(h):
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(h["build_time"]))
    return "%s (product %d), build %s, v%d, %d bytes, built %s, commit %08x" % (
        PRODUCTS.get(h["product"], "?"), h["product"], h["variant"], h["fw"], h["image_size"], when, h["commit"])


def git_commit(cwd):
    try:
        out = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=cwd, capture_output=True, text=True,
                             timeout=5)
        return int(out.stdout.strip()[:8], 16) if out.returncode == 0 else 0
    except Exception:  # noqa: BLE001 - no git is fine
        return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="firmware.bin -> .elcfw")
    p.add_argument("bin")
    p.add_argument("--product", type=lambda s: int(s, 0), required=True)
    p.add_argument("--variant", required=True, help="the PlatformIO environment the image was built for")
    p.add_argument("--fw", type=int, default=1)
    p.add_argument("-o", "--out", required=True)
    s = sub.add_parser("show", help="print and check an .elcfw header")
    s.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd == "pack":
        image = open(a.bin, "rb").read()
        data = make_header(image, a.product, a.variant, a.fw, commit=git_commit(".")) + image
        check_file(data)
        open(a.out, "wb").write(data)
        print("%s: %s" % (a.out, describe(parse_header(data))))
    else:
        data = open(a.file, "rb").read()
        try:
            print(describe(check_file(data)))
        except ValueError as e:
            print("%s: %s" % (a.file, e))
            return 1
    return 0


# ---- as a PlatformIO extra script ----
try:
    Import("env")  # noqa: F821  # SCons
except NameError:
    env = None

if env is not None:
    import os

    _pioenv = env.subst("$PIOENV")
    env.Append(CPPDEFINES=[("ELC_FW_VARIANT", env.StringifyMacro(_pioenv))])

    def _option(name, default):
        try:
            return env.GetProjectOption(name)
        except Exception:  # noqa: BLE001 - option not set
            return default

    def _fw_version():
        for d in env.get("CPPDEFINES", []):
            if isinstance(d, (list, tuple)) and d and d[0] == "ELC_CAN_FW_VERSION":
                return int(str(d[1]), 0)
        return 1

    def _pack(source, target, env):  # noqa: ARG001 - SCons signature
        product = int(str(_option("custom_elc_product", "0")), 0)
        if not product:
            print("elc_fw: custom_elc_product not set for %s; no .elcfw written" % _pioenv)
            return
        bin_path = str(target[0]) if target else ""
        if not bin_path.endswith(".bin"):
            bin_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
        image = open(bin_path, "rb").read()
        data = make_header(image, product, _pioenv, _fw_version(), commit=git_commit(env.subst("$PROJECT_DIR"))) + image
        out = os.path.join(env.subst("$BUILD_DIR"), _pioenv + ".elcfw")
        open(out, "wb").write(data)
        print("elc_fw: %s\n        %s" % (out, describe(parse_header(data))))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _pack)
elif __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
