from __future__ import annotations

import argparse
import os
import sys

from dashboard.config import ConfigError, load_dashboard_config


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="El Camino Pygame dashboard")
    parser.add_argument(
        "--config",
        default="config/dashboard.yaml",
        help="Path to dashboard YAML config",
    )
    parser.add_argument(
        "--display-mode",
        choices=("auto", "hardware"),
        help="Display backend mode (defaults to DISPLAY_MODE env var or auto)",
    )
    parser.add_argument(
        "--render-mode",
        choices=("single_window", "multi_window", "spi"),
        help="Render topology mode (defaults to DISPLAY_RENDER_MODE env var or config)",
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Run pygame in headless mode (no OS window)",
    )
    return parser.parse_args()


def _resolve_display_mode(args: argparse.Namespace) -> str:
    arg_mode = str(args.display_mode or "").strip().lower()
    env_mode = str(os.environ.get("DISPLAY_MODE", "")).strip().lower()
    mode = arg_mode or env_mode or "auto"
    if mode not in {"auto", "hardware"}:
        raise ValueError(f"Unsupported display mode: {mode!r}")
    return mode


def _prepare_display_environment(mode: str, headless: bool) -> None:
    if headless:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
        return
    display = str(os.environ.get("DISPLAY", "")).strip()
    is_linux = sys.platform.startswith("linux")
    if mode == "hardware":
        if is_linux and not display:
            raise RuntimeError("DISPLAY is not set for hardware mode.")
        return
    if display:
        return
    if not is_linux:
        # macOS/Windows do not rely on X11 DISPLAY for native pygame windows.
        return
    raise RuntimeError("DISPLAY is not set in auto mode.")


def main() -> int:
    args = parse_args()
    headless = args.headless or str(os.environ.get("HEADLESS", "")).strip() == "1"
    if headless:
        os.environ["HEADLESS"] = "1"
    try:
        display_mode = _resolve_display_mode(args)
        _prepare_display_environment(display_mode, headless=headless)
    except (RuntimeError, ValueError) as exc:
        print(f"Display setup error: {exc}", file=sys.stderr)
        return 2

    try:
        cfg = load_dashboard_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}", file=sys.stderr)
        return 2
    if args.render_mode:
        os.environ["DISPLAY_RENDER_MODE"] = str(args.render_mode)

    # Import after display mode setup so pygame picks the selected backend.
    from dashboard.app import DashboardApp

    app = DashboardApp(cfg)
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
