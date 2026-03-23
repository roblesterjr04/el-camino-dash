"""LLM queries: Claude when online, Ollama when offline."""

from __future__ import annotations

import logging
import socket

import requests

from config import (
    CLAUDE_MAX_TOKENS,
    CLAUDE_MODEL,
    NETWORK_CHECK_HOST,
    NETWORK_CHECK_PORT,
    NETWORK_CHECK_TIMEOUT_S,
    OLLAMA_BASE_URL,
    OLLAMA_MODEL,
    SYSTEM_PROMPT_TEMPLATE,
)

logger = logging.getLogger(__name__)


def check_network() -> bool:
    try:
        with socket.create_connection(
            (NETWORK_CHECK_HOST, NETWORK_CHECK_PORT),
            timeout=NETWORK_CHECK_TIMEOUT_S,
        ):
            return True
    except OSError:
        return False


def query(user_text: str, telemetry_str: str) -> str:
    system = SYSTEM_PROMPT_TEMPLATE.format(telemetry=telemetry_str)
    if check_network():
        logger.info("[llm] using claude")
        try:
            import anthropic

            client = anthropic.Anthropic()
            msg = client.messages.create(
                model=CLAUDE_MODEL,
                max_tokens=CLAUDE_MAX_TOKENS,
                system=system,
                messages=[{"role": "user", "content": user_text}],
            )
            parts = []
            for block in msg.content:
                if getattr(block, "text", None):
                    parts.append(block.text)
            return "".join(parts).strip()
        except Exception as exc:
            logger.warning("Claude request failed: %s", exc)
            return "Sorry, I couldn't process that."

    logger.info("[llm] using ollama")
    try:
        url = f"{OLLAMA_BASE_URL.rstrip('/')}/api/chat"
        payload = {
            "model": OLLAMA_MODEL,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user_text},
            ],
            "stream": False,
        }
        r = requests.post(url, json=payload, timeout=120)
        r.raise_for_status()
        data = r.json()
        msg = data.get("message") or {}
        return (msg.get("content") or "").strip()
    except Exception as exc:
        logger.warning("Ollama request failed: %s", exc)
        return "Sorry, I couldn't process that."
