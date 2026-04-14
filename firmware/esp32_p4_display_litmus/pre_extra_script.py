# Put the RISC-V toolchain bin/ on PATH (same as esp32_p4_tsps).
import os
import shutil
from os.path import expanduser
from pathlib import Path
from typing import List, Optional

Import("env")  # noqa: F821  # SCons


def _core_dir() -> Path:
    return Path(os.environ.get("PLATFORMIO_CORE_DIR", expanduser("~/.platformio"))).resolve()


def _collect_toolchain_roots(core: Path) -> List[Path]:
    roots: List[Path] = []
    for sub in ("packages", "tools"):
        base = core / sub
        if not base.is_dir():
            continue
        for p in base.iterdir():
            if p.is_dir() and p.name.startswith("toolchain-riscv32-esp"):
                roots.append(p)
    roots.sort(
        key=lambda x: (
            0 if "@" in x.name else 1,
            0 if x.parent.name == "tools" else 1,
            x.name,
        )
    )
    return roots


def _find_riscv_toolchain_bin() -> Optional[str]:
    cxx_name = "riscv32-esp-elf-g++.exe" if os.name == "nt" else "riscv32-esp-elf-g++"
    for p in _collect_toolchain_roots(_core_dir()):
        cxx = p / "bin" / cxx_name
        if cxx.is_file():
            return str(p / "bin")
    return None


def _prepend_riscv_toolchain() -> None:
    proj = env.subst("$PROJECT_DIR")
    pio_dir = Path(proj) / ".pio"
    retry_flag = pio_dir / "toolchain_reinstall_retry"

    bin_dir = _find_riscv_toolchain_bin()
    if bin_dir:
        env.PrependENVPath("PATH", bin_dir)
        os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")
        retry_flag.unlink(missing_ok=True)
        return

    core = _core_dir()
    removed = False
    for p in _collect_toolchain_roots(core):
        print("\nRemoving incomplete RISC-V toolchain tree:\n  %s" % p.resolve())
        shutil.rmtree(p, ignore_errors=True)
        removed = True

    if not removed:
        print(
            "ERROR: No riscv32-esp-elf-g++ under %s/{packages,tools}/toolchain-riscv32-esp*."
            % core
        )
        raise SystemExit(1)

    pio_dir.mkdir(parents=True, exist_ok=True)
    retry_flag.write_text("1", encoding="ascii")
    raise SystemExit(1)


_prepend_riscv_toolchain()
