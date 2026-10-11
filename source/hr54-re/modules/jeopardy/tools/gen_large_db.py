#!/usr/bin/env python3
"""Generate a deliberately large JEPDB1 database for the stage-1 host tests.

The database has far more than 127 categories and more than 1000 clues so the
int32 clue/category indices can be exercised, and it places every usable
single/double category *after* 160 one-clue decoys so any selected board forces
high indices. Historical daily doubles are flagged on the last clue of every
usable single/double category, and the usable categories are spread across two
episodes for whole-episode selection.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("import_questions", os.path.join(HERE, "import_questions.py"))
iq = importlib.util.module_from_spec(spec)
sys.modules["import_questions"] = iq
spec.loader.exec_module(iq)

J_VALUES = [200, 400, 600, 800, 1000]
DJ_VALUES = [400, 800, 1200, 1600, 2000]


def make_cat(name: str, rnd: int, episode: int, airdate: int) -> iq.Category:
    cat = iq.Category(name=name, round=rnd, episode=episode, airdate=airdate)
    values = DJ_VALUES if rnd == iq.ROUND_DJ else J_VALUES
    for i, v in enumerate(values):
        cat.clues.append(iq.Clue(
            category=name, round=rnd, value=v,
            clue=f"{name} clue {i + 1}", response=f"{name} answer {i + 1}",
            difficulty=min(i + 1, 5),
            # historical daily double on the last clue of each usable category
            flags=(iq.QFLAG_DD if i == len(values) - 1 else 0),
            episode=episode, airdate=airdate))
    return cat


def build() -> list:
    cats: list = []
    airdate = 20200101
    # 160 one-clue decoys: valid records that can never form a board, pushing
    # every usable category index above 127.
    for i in range(160):
        d = iq.Category(name=f"DECOY {i}", round=iq.ROUND_J, episode=1, airdate=airdate)
        d.clues.append(iq.Clue(category=f"DECOY {i}", round=iq.ROUND_J, value=200,
                               clue=f"decoy clue {i}", response=f"decoy answer {i}",
                               difficulty=1, flags=0, episode=1, airdate=airdate))
        cats.append(d)
    # usable single + double boards for two complete episodes
    for ep in (1, 2):
        for i in range(240 if ep == 1 else 60):
            cats.append(make_cat(f"J{ep} CAT {i}", iq.ROUND_J, ep, airdate + ep))
        for i in range(240 if ep == 1 else 60):
            cats.append(make_cat(f"DJ{ep} CAT {i}", iq.ROUND_DJ, ep, airdate + ep))
        for i in range(20 if ep == 1 else 8):
            name = f"FINAL{ep} CAT {i}"
            rc = iq.Category(name=name, round=iq.ROUND_FINAL, episode=ep, airdate=airdate + ep)
            rc.clues.append(iq.Clue(category=name, round=iq.ROUND_FINAL, value=0,
                                    clue=f"{name} clue", response=f"{name} answer",
                                    difficulty=5, flags=iq.QFLAG_FINAL,
                                    episode=ep, airdate=airdate + ep))
            cats.append(rc)
    return cats


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Generate a large JEPDB1 database")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    cats = build()
    iq.write_db(cats, args.out)
    clues = sum(len(c.clues) for c in cats)
    print(f"large db: {len(cats)} categories, {clues} clues -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
