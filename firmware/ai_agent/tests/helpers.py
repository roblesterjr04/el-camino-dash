import os
import shutil
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # firmware/


def wait_for(pred, timeout=3.0, step=0.01):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def build_cpp(source: str, extra_files):
    """Compiles a small C++ program against the firmware sources; returns the binary path or None when no compiler."""
    cxx = shutil.which("c++") or shutil.which("g++")
    if not cxx:
        return None
    d = tempfile.mkdtemp(prefix="elcai")
    src = os.path.join(d, "t.cpp")
    Path(src).write_text(source)
    out = os.path.join(d, "t")
    cmd = [cxx, "-std=c++17", "-O0", "-I", str(ROOT / "ai_card/src"), "-I", str(ROOT / "elc_can/include"), src,
           *[str(ROOT / f) for f in extra_files], "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return out
