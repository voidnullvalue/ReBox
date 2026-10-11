#!/usr/bin/env python3
"""Host tests for the Jeopardy PC importer and the JEPDB1 binary format.

Exercises real importer fixtures (j-archive JSON and jwolle1 TSV), media,
duplicate, malformed and incomplete-category exclusion, the original demo pack
and a structural round-trip of the written database.
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIX = os.path.join(HERE, "fixtures")

spec = importlib.util.spec_from_file_location("import_questions", os.path.join(ROOT, "tools", "import_questions.py"))
iq = importlib.util.module_from_spec(spec)
sys.modules["import_questions"] = iq
spec.loader.exec_module(iq)

checks = 0
failures = 0


def check(cond, msg):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", msg)


def read_header(path):
    with open(path, "rb") as fh:
        data = fh.read()
    magic, version, flags, cat_count, clue_count, string_bytes, _ = struct.unpack_from("<8sIIIIII", data, 0)
    return data, magic, version, cat_count, clue_count, string_bytes


def test_demo():
    cats, rep = iq.build(packs=[os.path.join(ROOT, "tools", "demo_questions.json")])
    check(rep.categories == 29, f"demo categories {rep.categories}")
    check(rep.imported == 125, f"demo clues {rep.imported}")
    check(rep.malformed == 0 and rep.media == 0 and rep.duplicate == 0, "demo clean")
    rounds = {}
    for c in cats:
        rounds[c.round] = rounds.get(c.round, 0) + 1
    check(rounds.get(iq.ROUND_J) == 12, "12 single Jeopardy categories")
    check(rounds.get(iq.ROUND_DJ) == 12, "12 Double Jeopardy categories")
    check(rounds.get(iq.ROUND_FINAL) == 5, "5 Final categories (reserve for tiebreak)")
    for c in cats:
        if c.round in (iq.ROUND_J, iq.ROUND_DJ):
            check(len(c.clues) == 5, f"coherent category {c.name} has 5 clues")
            values = [q.value for q in c.clues]
            check(values == sorted(values), f"{c.name} values ascending")
    return cats


def test_jarchive():
    cats, rep = iq.build(jarchives=[os.path.join(FIX, "jarchive_game.json")])
    check(rep.categories == 3, f"jarchive categories {rep.categories}")
    check(rep.imported == 11, f"jarchive clues {rep.imported}")
    check(rep.media == 1, "jarchive media excluded")
    check(rep.duplicate == 1, "jarchive duplicate excluded")
    check(rep.malformed == 1, "jarchive malformed excluded")
    check(rep.incomplete == 1, "jarchive incomplete category dropped")


def test_tsv():
    cats, rep = iq.build(tsvs=[os.path.join(FIX, "jwolle_sample.tsv")])
    check(rep.categories == 3, f"tsv categories {rep.categories}")
    check(rep.imported == 11, f"tsv clues {rep.imported}")
    check(rep.media == 1, "tsv media excluded")
    check(rep.duplicate == 1, "tsv duplicate excluded")
    check(rep.malformed == 3, "tsv malformed excluded")
    # daily double flag carried from daily_double_value
    dd = [q for c in cats for q in c.clues if q.flags & iq.QFLAG_DD]
    check(len(dd) == 1, "one daily double flagged from TSV")
    # final clue flagged
    final = [q for c in cats if c.round == iq.ROUND_FINAL for q in c.clues]
    check(all(q.flags & iq.QFLAG_FINAL for q in final), "final clue flagged")


def test_combined(tmp):
    cats, rep = iq.build(jarchives=[os.path.join(FIX, "jarchive_game.json")],
                         tsvs=[os.path.join(FIX, "jwolle_sample.tsv")])
    check(rep.categories == 6, f"combined categories {rep.categories}")
    check(rep.imported == 22, f"combined clues {rep.imported}")
    out = os.path.join(tmp, "combined.jdb")
    iq.write_db(cats, out)
    data, magic, version, cat_count, clue_count, string_bytes = read_header(out)
    check(magic == b"JEPDB001", "magic")
    check(version == 1, "version")
    check(cat_count == 6, "header category count")
    check(clue_count == 22, "header clue count")
    # checksum covers everything but the trailing u32
    body = data[:-4]
    check(struct.unpack_from("<I", data, len(data) - 4)[0] == iq._fnv(body), "checksum")
    # record sizes match the documented layout
    check(len(data) == 32 + cat_count * 32 + clue_count * 36 + string_bytes + 4, "record sizes")
    # an out-of-range round in a pack is ignored rather than crashing
    bad = os.path.join(tmp, "bad.json")
    with open(bad, "w") as fh:
        fh.write('{"jeopardy":[{"category":"X","clues":[{"value":200,"clue":"a","response":"b"}]}]}')
    _, rep2 = iq.build(packs=[bad])
    check(rep2.incomplete == 1, "single-clue category is incomplete")


def main():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="jpq-import-test-") as tmp:
        test_demo()
        test_jarchive()
        test_tsv()
        test_combined(tmp)
    if failures:
        print(f"import: {failures}/{checks} checks FAILED")
        return 1
    print(f"import: {checks} checks passed (jarchive JSON, jwolle TSV, exclusions, JEPDB1 format)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
