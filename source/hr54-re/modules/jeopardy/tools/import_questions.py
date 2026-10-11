#!/usr/bin/env python3
"""Import Jeopardy question sources into the portable JEPDB1 binary database.

Supported inputs (any combination, merged and de-duplicated):

  * ``--pack FILE``       ReBox original pack JSON (schema 1, see demo_questions.json)
  * ``--jarchive FILE``   j-archive style per-game JSON (rounds -> categories -> clues)
  * ``--tsv FILE``        jwolle1 ``combined_season*.tsv`` / season TSV export

Only the Python standard library is used. Media-dependent, malformed, empty and
duplicate clues are excluded. Categories are only emitted when they form a
coherent board unit: five clues for Jeopardy/Double Jeopardy, one for Final.

Binary layout (all integers little-endian, so the file is identical on the
little-endian host and the big-endian receiver)::

    header  : magic[8]='JEPDB001', u32 version, u32 flags, u32 cat_count,
              u32 clue_count, u32 string_bytes, u32 reserved   (32 bytes)
    category: u32 name_off, u16 name_len, u8 round, u8 difficulty, u32 year,
              u32 episode, u32 airdate, u32 first_clue, u16 clue_count,
              u16 reserved, u32 reserved2                      (32 bytes)
    clue    : u32 category, u32 value, u32 episode, u32 airdate, u8 round,
              u8 difficulty, u8 flags, u8 reserved, u32 clue_off, u16 clue_len,
              u16 reserved2, u32 resp_off, u16 resp_len, u16 reserved3 (36 bytes)
    strings : concatenated UTF-8
    checksum: u32 FNV-1a over every preceding byte
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from typing import Iterable, List, Optional

JDB_MAGIC = b"JEPDB001"
JDB_VERSION = 1
ROUND_J, ROUND_DJ, ROUND_FINAL = 1, 2, 3
QFLAG_DD, QFLAG_FINAL = 1, 2

_MEDIA_PAT = re.compile(r"(<a\b|</?[a-z]+\b[^>]*>|href\s*=|https?://|\[(?:video|audio|image|picture)|"
                        r"\(\s*(?:video|audio|image|picture)\s*\)|seen\s+here|shown\s+here|"
                        r"audio\s+clue|video\s+clue|visual\s+clue)", re.IGNORECASE)
_WS_PAT = re.compile(r"\s+")


@dataclass
class Clue:
    category: str
    round: int
    value: int
    clue: str
    response: str
    difficulty: int = 3
    flags: int = 0
    episode: int = 0
    airdate: int = 0


@dataclass
class Category:
    name: str
    round: int
    episode: int
    airdate: int
    clues: List[Clue] = field(default_factory=list)


class Report:
    def __init__(self) -> None:
        self.imported = 0
        self.media = 0
        self.malformed = 0
        self.duplicate = 0
        self.incomplete = 0
        self.categories = 0

    def as_dict(self) -> dict:
        return {
            "categories": self.categories,
            "clues": self.imported,
            "excluded": {
                "media": self.media,
                "malformed": self.malformed,
                "duplicate": self.duplicate,
                "incomplete_category": self.incomplete,
            },
        }


def norm(text: Optional[str]) -> str:
    if text is None:
        return ""
    return _WS_PAT.sub(" ", str(text)).strip()


def is_media(text: str) -> bool:
    return bool(_MEDIA_PAT.search(text))


def parse_airdate(value) -> int:
    """Return YYYYMMDD as int, or 0 when unknown."""
    if value is None:
        return 0
    if isinstance(value, (int, float)):
        n = int(value)
        return n if 19000000 < n < 30000000 else 0
    s = str(value).strip()
    m = re.match(r"^(\d{4})-(\d{1,2})-(\d{1,2})", s)
    if m:
        return int(m.group(1)) * 10000 + int(m.group(2)) * 100 + int(m.group(3))
    digits = re.sub(r"\D", "", s)
    if len(digits) == 8:
        return int(digits)
    return 0


def year_of(airdate: int) -> int:
    return airdate // 10000 if airdate else 0


def first_key(obj: dict, keys: Iterable[str], default=None):
    for k in keys:
        if k in obj and obj[k] not in (None, ""):
            return obj[k]
    return default


def round_from_name(name: str, fallback: int = ROUND_J) -> int:
    n = str(name).lower()
    if "double" in n:
        return ROUND_DJ
    if "final" in n:
        return ROUND_FINAL
    if "jeopardy" in n or "single" in n:
        return ROUND_J
    return fallback


class Collector:
    """Accumulates de-duplicated, validated categories from mixed sources."""

    def __init__(self) -> None:
        self.cats: dict = {}
        self.seen: set = set()
        self.report = Report()
        self._episode = 0
        self._episodes: dict = {}

    def episode_id(self, key) -> int:
        if key in self._episodes:
            return self._episodes[key]
        self._episode += 1
        self._episodes[key] = self._episode
        return self._episode

    def add_clue(self, cat_name: str, rnd: int, value: int, clue: str, response: str,
                 airdate: int, episode: int, dd: bool) -> bool:
        clue = norm(clue)
        response = norm(response)
        cat_name = norm(cat_name)
        if not cat_name or not clue or not response:
            self.report.malformed += 1
            return False
        if is_media(clue) or is_media(response):
            self.report.media += 1
            return False
        if rnd in (ROUND_J, ROUND_DJ) and value <= 0:
            self.report.malformed += 1
            return False
        key = (rnd, cat_name.lower(), clue.lower(), response.lower())
        if key in self.seen:
            self.report.duplicate += 1
            return False
        self.seen.add(key)
        ck = (rnd, episode, cat_name.lower())
        cat = self.cats.get(ck)
        if cat is None:
            cat = Category(name=cat_name, round=rnd, episode=episode, airdate=airdate)
            self.cats[ck] = cat
        cat.clues.append(Clue(category=cat_name, round=rnd, value=int(value), clue=clue,
                              response=response, flags=(QFLAG_DD if dd else 0),
                              episode=episode, airdate=airdate))
        return True

    def finalize(self) -> List[Category]:
        out: List[Category] = []
        for cat in self.cats.values():
            need = 1 if cat.round == ROUND_FINAL else 5
            if len(cat.clues) < need:
                self.report.incomplete += 1
                continue
            cat.clues.sort(key=lambda c: (c.value, c.clue))
            # difficulty is the clue's rank in the coherent category (1..5)
            for i, c in enumerate(cat.clues):
                c.difficulty = min(i + 1, 5) if cat.round != ROUND_FINAL else 5
            if cat.round == ROUND_FINAL:
                for c in cat.clues:
                    c.flags |= QFLAG_FINAL
            out.append(cat)
        out.sort(key=lambda c: (c.round, c.episode, c.name))
        self.report.categories = len(out)
        self.report.imported = sum(len(c.clues) for c in out)
        return out


# ---------------------------------------------------------------------------
# Source parsers
# ---------------------------------------------------------------------------

def import_pack(path: str, col: Collector) -> None:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: pack must be a JSON object")
    episode = col.episode_id(("pack", data.get("id", path)))
    airdate = parse_airdate(first_key(data, ["airdate", "air_date", "date"], 0))
    rounds = [
        (ROUND_J, data.get("jeopardy") or data.get("single") or data.get("j")),
        (ROUND_DJ, data.get("double") or data.get("double_jeopardy") or data.get("dj")),
        (ROUND_FINAL, data.get("final") or data.get("final_jeopardy")),
    ]
    for rnd, block in rounds:
        if not block:
            continue
        for cat in block:
            name = first_key(cat, ["category", "name", "title"], "")
            questions = cat.get("clues") or cat.get("questions")
            if questions is None:
                # a flat single-clue entry (common for Final)
                questions = [cat]
            for q in questions:
                col.add_clue(
                    name, rnd,
                    first_key(q, ["value", "clue_value", "amount"], 0),
                    first_key(q, ["clue", "question", "prompt", "answer"], ""),
                    first_key(q, ["response", "correct_response", "question"], ""),
                    parse_airdate(first_key(q, ["airdate", "air_date"], airdate)),
                    episode,
                    bool(first_key(q, ["daily_double", "dailyDouble", "dd"], False)),
                )


def _jarchive_rounds(data: dict):
    """Yield (round_number, categories_list) for several j-archive JSON shapes."""
    game = data.get("game", data)
    rounds = game.get("rounds")
    if isinstance(rounds, list):
        for r in rounds:
            name = first_key(r, ["round", "name", "title"], "")
            yield round_from_name(name), r.get("categories") or [], first_key(r, ["air_date", "airdate"])
        return
    if isinstance(rounds, dict):
        for key, r in rounds.items():
            if not isinstance(r, dict):
                continue
            name = first_key(r, ["round", "name", "title"], key)
            yield round_from_name(name), r.get("categories") or [], first_key(r, ["air_date", "airdate"])
        return
    # flat: categories at top level with a "round" field
    cats = game.get("categories")
    if isinstance(cats, list):
        for c in cats:
            yield round_from_name(first_key(c, ["round", "round_name"], ""), ROUND_J), [c], None


def import_jarchive(path: str, col: Collector) -> None:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if isinstance(data, list):
        for i, game in enumerate(data):
            _import_jarchive_game(game, col, f"{path}#{i}")
        return
    if not isinstance(data, dict):
        raise ValueError(f"{path}: jarchive must be an object or list")
    _import_jarchive_game(data, col, path)


def _import_jarchive_game(data: dict, col: Collector, key) -> None:
    game = data.get("game", data)
    airdate = parse_airdate(first_key(game, ["air_date", "airdate", "date", "aired"], 0))
    ep_key = first_key(game, ["game_id", "id", "episode", "air_date", "airdate"], key)
    episode = col.episode_id(("jarchive", ep_key))
    for rnd, categories, rdate in _jarchive_rounds(game):
        for cat in categories:
            if not isinstance(cat, dict):
                col.report.malformed += 1
                continue
            name = first_key(cat, ["category", "name", "title"], "")
            for q in cat.get("clues") or cat.get("questions") or []:
                if not isinstance(q, dict):
                    col.report.malformed += 1
                    continue
                col.add_clue(
                    name, rnd,
                    first_key(q, ["value", "clue_value", "amount", "clueValue"], 0),
                    first_key(q, ["clue", "question", "prompt", "answer"], ""),
                    first_key(q, ["correct_response", "response", "correctResponse", "question"], ""),
                    parse_airdate(first_key(q, ["airdate", "air_date"], rdate or airdate)),
                    episode,
                    bool(first_key(q, ["daily_double", "dailyDouble", "dd"], False)),
                )


def import_tsv(path: str, col: Collector) -> None:
    with open(path, "r", encoding="utf-8") as fh:
        header = fh.readline().rstrip("\n").split("\t")
        idx = {name: i for i, name in enumerate(header)}
        required = ["round", "category", "answer", "question"]
        if not all(k in idx for k in required):
            raise ValueError(f"{path}: TSV missing required columns {required}")
        for line in fh:
            if not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < len(header):
                parts += [""] * (len(header) - len(parts))
            try:
                rnd = int(parts[idx["round"]])
            except ValueError:
                col.report.malformed += 1
                continue
            if rnd not in (ROUND_J, ROUND_DJ, ROUND_FINAL):
                col.report.malformed += 1
                continue
            notes = parts[idx.get("notes", -1)] if "notes" in idx else ""
            if notes and re.search(r"video|audio|image|visual", notes, re.IGNORECASE):
                col.report.media += 1
                continue
            airdate = parse_airdate(parts[idx.get("air_date", -1)] if "air_date" in idx else 0)
            episode = col.episode_id(("tsv", airdate))
            try:
                value = int(parts[idx["clue_value"]] or 0) if "clue_value" in idx else 0
            except ValueError:
                value = 0
            dd_val = 0
            if "daily_double_value" in idx:
                try:
                    dd_val = int(parts[idx["daily_double_value"]] or 0)
                except ValueError:
                    dd_val = 0
            col.add_clue(
                parts[idx["category"]], rnd, value,
                parts[idx["answer"]], parts[idx["question"]],
                airdate, episode, dd_val > 0,
            )


# ---------------------------------------------------------------------------
# Binary writer
# ---------------------------------------------------------------------------

def _fnv(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def write_db(categories: List[Category], out_path: str) -> None:
    strings = bytearray()

    def add_str(s: str):
        b = s.encode("utf-8")
        off = len(strings)
        strings.extend(b)
        return off, len(b)

    clue_records = []
    cat_records = []
    for ci, cat in enumerate(categories):
        first = len(clue_records)
        for clue in cat.clues:
            coff, clen = add_str(clue.clue)
            roff, rlen = add_str(clue.response)
            clue_records.append(struct.pack(
                "<IIIIBBBBIHHIHH",
                ci, clue.value, clue.episode, clue.airdate,
                clue.round, clue.difficulty, clue.flags, 0,
                coff, clen, 0, roff, rlen, 0))
        name_off, name_len = add_str(cat.name)
        diff = max((c.difficulty for c in cat.clues), default=3)
        cat_records.append(struct.pack(
            "<IHBBIIIIHHI",
            name_off, name_len, cat.round, diff,
            year_of(cat.airdate), cat.episode, cat.airdate,
            first, len(cat.clues), 0, 0))

    header = struct.pack(
        "<8sIIIIII",
        JDB_MAGIC, JDB_VERSION, 0, len(categories), len(clue_records),
        len(strings), 0)
    body = header + b"".join(cat_records) + b"".join(clue_records) + bytes(strings)
    checksum = struct.pack("<I", _fnv(body))
    with open(out_path, "wb") as fh:
        fh.write(body)
        fh.write(checksum)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build(packs=(), jarchives=(), tsvs=()) -> tuple:
    col = Collector()
    for p in packs:
        import_pack(p, col)
    for j in jarchives:
        import_jarchive(j, col)
    for t in tsvs:
        import_tsv(t, col)
    return col.finalize(), col.report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Build a JEPDB1 Jeopardy database")
    ap.add_argument("--pack", action="append", default=[], help="ReBox pack JSON")
    ap.add_argument("--jarchive", action="append", default=[], help="j-archive game JSON")
    ap.add_argument("--tsv", action="append", default=[], help="jwolle1 TSV export")
    ap.add_argument("--out", required=True, help="output .jdb path")
    ap.add_argument("--report", action="store_true", help="print a JSON report")
    args = ap.parse_args(argv)
    if not (args.pack or args.jarchive or args.tsv):
        ap.error("at least one input source is required")
    categories, report = build(args.pack, args.jarchive, args.tsv)
    write_db(categories, args.out)
    if args.report:
        json.dump(report.as_dict(), sys.stdout, sort_keys=True)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
