#!/usr/bin/env python3
"""Generate a JEPDB1 database whose categories can form random boards but never
a single complete broadcast episode (J + DJ + Final). Episode-selection mode
must therefore fail rather than silently falling back to unrelated random
categories. Used by the stage-1 host tests.
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
AIRDATE = 20200202


def make_cat(name: str, rnd: int, episode: int, values: list) -> iq.Category:
    cat = iq.Category(name=name, round=rnd, episode=episode, airdate=AIRDATE)
    for i, v in enumerate(values):
        cat.clues.append(iq.Clue(
            category=name, round=rnd, value=v,
            clue=f"{name} clue {i + 1}", response=f"{name} answer {i + 1}",
            difficulty=min(i + 1, 5), episode=episode, airdate=AIRDATE))
    return cat


def build() -> list:
    cats: list = []
    # six single categories from episode 1 ...
    for i in range(6):
        cats.append(make_cat(f"J{i}", iq.ROUND_J, 1, J_VALUES))
    # ... six double categories from episode 2 ...
    for i in range(6):
        cats.append(make_cat(f"DJ{i}", iq.ROUND_DJ, 2, DJ_VALUES))
    # ... and one Final from a third episode: no episode is ever complete.
    rc = iq.Category(name="FINAL", round=iq.ROUND_FINAL, episode=3, airdate=AIRDATE)
    rc.clues.append(iq.Clue(category="FINAL", round=iq.ROUND_FINAL, value=0,
                            clue="final clue", response="final answer",
                            difficulty=5, flags=iq.QFLAG_FINAL,
                            episode=3, airdate=AIRDATE))
    cats.append(rc)
    return cats


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Generate an incomplete-episode JEPDB1 database")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    cats = build()
    iq.write_db(cats, args.out)
    clues = sum(len(c.clues) for c in cats)
    print(f"gap db: {len(cats)} categories, {clues} clues -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
