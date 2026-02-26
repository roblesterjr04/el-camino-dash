from __future__ import annotations

import argparse
import sys

from dashboard.app import DashboardApp
from dashboard.config import ConfigError, load_dashboard_config


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="El Camino Pygame dashboard")
    parser.add_argument(
        "--config",
        default="config/dashboard.yaml",
        help="Path to dashboard YAML config",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        cfg = load_dashboard_config(args.config)
    except ConfigError as exc:
        print(f"Config error: {exc}", file=sys.stderr)
        return 2
    app = DashboardApp(cfg)
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
