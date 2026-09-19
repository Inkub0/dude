#!/usr/bin/env python3
"""Bake the in-game PBR editor's live overrides into the main authored table, in place.

    python3 tools/pbr_bake_inplace.py                # bake, verify, write base/pbr/pbr_materials.cfg
    python3 tools/pbr_bake_inplace.py --dry-run      # report only
    python3 tools/pbr_bake_inplace.py --reset        # ...and reset the override file to an empty stub

The material editor (editPbrMaterial) and the category sliders write their deltas to
pbr/pbr_overrides.cfg in the writable "dude folder" (fs_savepath); that file loads after
pbr/pbr_materials.cfg and wins on collision. Baking folds those deltas down into the
shipped table so it stays the single source of truth (docs/pbr-materials.md, sec. 6).

Why not `pbr_make_overrides.py --bake`: it regenerates and reformats the whole table, and it
predates the "@cat <name> m r w e" category-default rows - it parses them as one material
named "@cat". This tool edits the table IN PLACE instead, so the diff is the edit:

  - a material already in the table: its LAST row is replaced (the engine keeps the last
    row of a key), tagged "# baked from live overrides <date>"
  - a material new to the table: appended under a dated section at the end
  - an @cat row: updated in the table's category-defaults block (added there if missing)
  - rows that already say the same thing are left alone, provenance comment and all

Before anything is written the result is checked to be RUNTIME-IDENTICAL: the table plus
the overrides, and the baked table alone, must parse to the same values under the engine's
rules (Material.cpp PBR_ParseTableBuffer: '#' starts a comment, keys are lowercased, later
rows win, an override row replaces the WHOLE entry - '*' inherits from the category, not
from the row it replaces).

The default --overrides path is the Linux dude folder; pass it explicitly elsewhere.
Commit the table afterwards:  pbr: bake the live material overrides of <date> into pbr_materials.cfg
"""

import argparse
import datetime
import os
import re
import sys

# the categories with live sliders (Material.cpp PBR_CategoryForName); anything else -
# "none", the long tail (glass / wood / cardboard ...) - keeps its explicit numbers
CATEGORIES = ("skin", "eyes", "flesh", "metal", "metal_painted", "ceramic_sheen",
              "metal_rust", "stone")

CAT_BLOCK_HEADER = "# ---- category defaults (baked from overrides) ----"

STUB = ("# DUDE PBR per-game overrides. Baked into pbr_materials.cfg on {date}.\n"
        "# Empty: the editor writes live deltas here (dude folder) again as you tune.\n\n")

_FLOAT = re.compile(r"\s*[+-]?(\d+\.?\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?)")


def strip_comment(line):
    i = line.find("#")
    return (line[:i] if i >= 0 else line).strip()


def atof(tok):
    """C atof: the leading numeric prefix, 0.0 when there is none."""
    m = _FLOAT.match(tok)
    return float(m.group(0)) if m else 0.0


def is_inherit(tok):
    """PBR_ParseNum's '-1': empty, '*', anything starting with '-', or 'auto'."""
    return not tok or tok[0] in "*-" or tok.lower() == "auto"


def parse_num(tok, lo, hi):
    if is_inherit(tok):
        return None
    return round(min(max(atof(tok), lo), hi), 4)


def parse_entry(toks):
    """(metal, rough, category, wet, env) exactly as the engine would hold them."""
    t = (toks + [""] * 6)[:6]
    cat = t[3].lower() if t[3].lower() in CATEGORIES else "none"
    return (parse_num(t[1], 0.0, 1.0), parse_num(t[2], 0.03, 1.0), cat,
            parse_num(t[4], 0.0, 8.0), parse_num(t[5], 0.0, 4.0))


def parse_cat_row(toks):
    """'@cat <name> m r w e' -> (name, values), or None when the engine would not take it
    as a category row (sscanf needs all four floats; unknown names are dropped)."""
    if len(toks) < 6 or toks[0].lower() != "@cat":
        return None
    if any(_FLOAT.match(t) is None for t in toks[2:6]):
        return None
    name = toks[1].lower()
    if name not in CATEGORIES:
        return None
    return name, tuple(round(atof(t), 4) for t in toks[2:6])


def effective(*texts):
    """The values the engine ends up with after loading these files in order."""
    materials, categories = {}, {}
    for text in texts:
        for raw in text.splitlines():
            s = strip_comment(raw)
            if not s:
                continue
            toks = s.split()
            if toks[0].lower() == "@cat":
                # (a malformed @cat row falls through the engine's sscanf into a junk material
                # literally named "@cat"; nothing can ever look that up, so it is not modelled)
                row = parse_cat_row(toks)
                if row:
                    categories[row[0]] = row[1]
                continue
            if len(toks) >= 3:
                materials[toks[0].lower()] = parse_entry(toks)
    return materials, categories


def fmt_num(tok):
    if is_inherit(tok):
        return "*"
    v = atof(tok)
    return "%.2f" % v if abs(round(v, 2) - v) < 1e-9 else "%g" % v


def fmt_row(toks, date):
    name, m, r, cat, w, e = (toks + [""] * 6)[:6]
    if not cat or cat[0] in "*-":
        cat = "none"
    return (f"{name:<52} {fmt_num(m):>4} {fmt_num(r):>4}  {cat:<13}  {fmt_num(w)}  {fmt_num(e)}"
            f"   # baked from live overrides {date}")


def bake(table, overrides, date):
    """-> (baked table text, report dict). Pure: touches no files."""
    lines = table.split("\n")
    rows, cat_rows = {}, {}
    for i, raw in enumerate(lines):
        s = strip_comment(raw)
        if not s:
            continue
        toks = s.split()
        row = parse_cat_row(toks)
        if row:
            cat_rows[row[0]] = i
        elif len(toks) >= 3 and toks[0].lower() != "@cat":
            rows.setdefault(toks[0].lower(), []).append(i)

    have_mats, have_cats = effective(table)
    report = {"updated": [], "new": {}, "equal": 0, "cats": [], "dups": [], "skipped": []}

    def shift(at):
        for v in rows.values():
            v[:] = [j + 1 if j >= at else j for j in v]
        for k in cat_rows:
            if cat_rows[k] >= at:
                cat_rows[k] += 1

    for raw in overrides.splitlines():
        s = strip_comment(raw)
        if not s:
            continue
        toks = s.split()
        if toks[0].lower() == "@cat":
            row = parse_cat_row(toks)
            if not row:
                report["skipped"].append(s)
                continue
            name, vals = row
            if have_cats.get(name) == vals:
                continue
            newline = "@cat %s %.3f %.3f %.3f %.3f" % ((name,) + tuple(atof(t) for t in toks[2:6]))
            if name in cat_rows:
                lines[cat_rows[name]] = newline
            else:
                if cat_rows:
                    at = max(cat_rows.values()) + 1
                else:       # no block yet: open one above the first data row
                    at = next((i for i, l in enumerate(lines) if strip_comment(l)), len(lines))
                    lines.insert(at, CAT_BLOCK_HEADER)
                    shift(at)
                    at += 1
                lines.insert(at, newline)
                shift(at)
                cat_rows[name] = at
            report["cats"].append((name, have_cats.get(name), vals))
            have_cats[name] = vals
            continue
        if len(toks) < 3:
            report["skipped"].append(s)
            continue
        key = toks[0].lower()
        if have_mats.get(key) == parse_entry(toks):
            report["equal"] += 1
            continue
        if key in rows:
            if len(rows[key]) > 1:
                report["dups"].append((toks[0], len(rows[key])))
            i = rows[key][-1]               # the row the engine keeps
            report["updated"].append((toks[0], strip_comment(lines[i]), " ".join(toks[1:])))
            lines[i] = fmt_row(toks, date)
        else:
            report["new"][key] = toks       # edited twice: the last edit wins, first position
        have_mats[key] = parse_entry(toks)

    if report["new"]:
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", "", f"# ---- materials new in live overrides (baked {date}) ----"]
        lines += [fmt_row(t, date) for t in report["new"].values()]
        lines.append("")
    return "\n".join(lines), report


def main():
    repo = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", default=os.path.join(repo, "base", "pbr", "pbr_materials.cfg"),
                    help="the main authored table (default: base/pbr/pbr_materials.cfg of this repo)")
    ap.add_argument("--overrides",
                    default=os.path.expanduser("~/.local/share/dude/base/pbr/pbr_overrides.cfg"),
                    help="the live override file the editor writes (default: the Linux dude folder)")
    ap.add_argument("--date", default=datetime.date.today().isoformat(),
                    help="date stamped on baked rows (default: today)")
    ap.add_argument("--dry-run", action="store_true", help="report, write nothing")
    ap.add_argument("--reset", action="store_true",
                    help="after a verified bake, reset --overrides to an empty dated stub")
    args = ap.parse_args()

    for path in (args.table, args.overrides):
        if not os.path.isfile(path):
            sys.exit(f"pbr_bake_inplace: no such file: {path}")
    with open(args.table, "r", encoding="utf-8") as fh:
        table = fh.read()
    with open(args.overrides, "r", encoding="utf-8", errors="replace") as fh:
        overrides = fh.read()

    baked, rep = bake(table, overrides, args.date)

    before, after = effective(table, overrides), effective(baked)
    if before != after:
        diffs = [k for k in set(before[0]) | set(after[0]) if before[0].get(k) != after[0].get(k)]
        sys.exit("pbr_bake_inplace: NOT runtime-identical - nothing written "
                 f"({len(diffs)} material diffs, e.g. {sorted(diffs)[:5]}; "
                 f"categories differ: {before[1] != after[1]})")

    changed = len(rep["updated"]) + len(rep["new"]) + len(rep["cats"])
    print(f"runtime-identical: {len(after[0])} materials, {len(after[1])} category rows")
    print(f"updated in place: {len(rep['updated'])}   new: {len(rep['new'])}   "
          f"already equal (left alone): {rep['equal']}   @cat changed: {len(rep['cats'])}")
    for name, old, new in rep["cats"]:
        print(f"  @cat {name}: {old} -> {new}")
    for name, count in rep["dups"]:
        print(f"  note: {name} appears {count}x in the table; replaced the last (winning) row")
    for row in rep["skipped"]:
        print(f"  skipped (malformed, or an @cat row for a category the engine does not know): {row}")

    if args.dry_run:
        print("dry run: nothing written")
        return 0
    if changed:
        with open(args.table, "w", encoding="utf-8") as fh:
            fh.write(baked)
        print(f"wrote {args.table}")
    else:
        print("nothing to bake")
    if args.reset:
        stub = STUB.format(date=args.date)
        if effective(baked, stub) != after:     # cannot happen; cheap to be sure
            sys.exit("pbr_bake_inplace: stub check failed - override file left alone")
        with open(args.overrides, "w", encoding="utf-8") as fh:
            fh.write(stub)
        print(f"reset {args.overrides}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
