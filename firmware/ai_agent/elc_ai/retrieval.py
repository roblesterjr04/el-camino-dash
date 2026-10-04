"""Owner's / shop manual search for the on-board model: BM25 over chunks of the text files in a directory. Offline,
tiny, no dependencies."""
from __future__ import annotations

import math
import re
from collections import Counter
from pathlib import Path
from typing import Dict, List, Tuple

_STOP = set("a an and are as at be but by do does for from how i if in is it its my of on or so that the this to was what when where which who why will with you your can".split())
_WORD = re.compile(r"[a-z0-9]+")


def _stem(w: str) -> str:
    return w[:-1] if len(w) > 3 and w.endswith("s") and not w.endswith("ss") else w


def tokens(s: str) -> List[str]:
    return [_stem(w) for w in _WORD.findall(s.lower()) if w not in _STOP]


class Manual:
    def __init__(self, chunk_words: int = 120, overlap: int = 20, k1: float = 1.4, b: float = 0.75):
        self.chunk_words, self.overlap, self.k1, self.b = chunk_words, overlap, k1, b
        self.chunks: List[Tuple[str, str]] = []  # (source, text)
        self._tf: List[Counter] = []
        self._df: Counter = Counter()
        self._len: List[int] = []

    def add_text(self, source: str, text: str) -> None:
        words = text.split()
        step = max(1, self.chunk_words - self.overlap)
        for i in range(0, max(1, len(words)), step):
            piece = " ".join(words[i:i + self.chunk_words])
            if not piece:
                continue
            toks = tokens(piece)
            if not toks:
                continue
            self.chunks.append((source, piece))
            tf = Counter(toks)
            self._tf.append(tf)
            self._len.append(len(toks))
            for t in tf:
                self._df[t] += 1
            if i + self.chunk_words >= len(words):
                break

    def add_dir(self, path: str) -> int:
        n = 0
        for p in sorted(Path(path).glob("**/*")):
            if p.suffix.lower() in (".txt", ".md") and p.is_file():
                self.add_text(p.name, p.read_text(errors="replace"))
                n += 1
        return n

    def search(self, query: str, k: int = 3, min_score: float = 0.2) -> List[Tuple[float, str, str]]:
        q = tokens(query)
        if not q or not self.chunks:
            return []
        n = len(self.chunks)
        avg = sum(self._len) / n
        scored: List[Tuple[float, str, str]] = []
        for i, tf in enumerate(self._tf):
            s = 0.0
            for t in q:
                f = tf.get(t, 0)
                if not f:
                    continue
                idf = math.log(1 + (n - self._df[t] + 0.5) / (self._df[t] + 0.5))
                s += idf * f * (self.k1 + 1) / (f + self.k1 * (1 - self.b + self.b * self._len[i] / avg))
            if s >= min_score:
                scored.append((s, self.chunks[i][0], self.chunks[i][1]))
        scored.sort(key=lambda x: -x[0])
        return scored[:k]
