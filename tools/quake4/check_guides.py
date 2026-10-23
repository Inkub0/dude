#!/usr/bin/env python3
"""Differential check for the engine's material-guide expansion (neo/framework/DeclGuides.cpp).

An independent Python implementation of the same rules expands every extracted Quake 4
.mtr file and compares, byte for byte, with what the C++ produced (tools/quake4/tests/
guide_expand_test.cpp), and compares the C++ line map (expanded line -> file line, the
<file>.lines it writes) with its own. Also checks structure: braces balance, no
guide/inlineGuide reference survives, every guide-defined material exists, and every
material statement starts its own line (the material parser is line-sensitive: an
expansion squeezed onto one line loses everything after qer_editorimage).

Usage: check_guides.py <extracted q4base dir> <C++ output dir>
"""
import os
import re
import sys

IDENT = re.compile(r"[A-Za-z0-9_]")


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 1
            if j < n and text[j] == '"':
                j += 1
            out.append(text[i:j]); i = j
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(" "); i += 1
        elif text.startswith("/*", i):
            out.append("  "); i += 2
            while i < n and not text.startswith("*/", i):
                out.append("\n" if text[i] == "\n" else " "); i += 1
            if i < n:
                out.append("  "); i += 2
        else:
            out.append(c); i += 1
    return "".join(out)


def load_templates(gdir):
    tpls = {}
    for f in sorted(os.listdir(gdir)):
        if not f.endswith(".guide"):
            continue
        t = strip_comments(open(os.path.join(gdir, f), encoding="latin-1", newline="").read())
        for m in re.finditer(r"(?:(?<=^)|(?<=[\s{};]))(?:inlineGuide|guide)\s+(\w+)\s*\(([^)]*)\)\s*\{", t, re.I):
            start = m.end()
            depth, k = 1, start
            while k < len(t) and depth:
                if t[k] == '"':
                    k += 1
                    while k < len(t) and t[k] not in '"\n':
                        k += 1
                elif t[k] == "{":
                    depth += 1
                elif t[k] == "}":
                    depth -= 1
                k += 1
            name = m.group(1).lower()
            parms = [p.strip() for p in m.group(2).split(",")]
            if parms == [""]:
                parms = []
            if name not in tpls:
                tpls[name] = (parms, t[start:k - 1])
    return tpls


def clean_arg(a):
    a = a.strip()
    if len(a) >= 2 and a[0] == '"' and a[-1] == '"':
        a = a[1:-1]
    return a


def substitute(parms, body, args):
    order = sorted(range(len(parms)), key=lambda i: -len(parms[i]))
    out, i = [], 0
    while i < len(body):
        for k in order:
            p = parms[k]
            if p and body.startswith(p, i):
                out.append(args[k] if k < len(args) else "")
                i += len(p)
                break
        else:
            out.append(body[i]); i += 1
    return "".join(out)


def parse_args(text, p):
    """text[p] just past '('; returns (args, end) or None."""
    args, cur, parens, n = [], [], 0, len(text)
    while p < n:
        c = text[p]
        if c == '"':
            j = p + 1
            while j < n and text[j] not in '"\n':
                j += 1
            if j < n and text[j] == '"':
                j += 1
            cur.append(text[p:j]); p = j; continue
        if c in "{}":
            return None
        if c == "(":
            parens += 1
        elif c == ")":
            if parens == 0:
                last = clean_arg("".join(cur))
                if args or last:
                    args.append(last)
                return args, p + 1
            parens -= 1
        elif c == "," and parens == 0:
            args.append(clean_arg("".join(cur))); cur = []; p += 1; continue
        cur.append(c); p += 1
    return None


def expand(text, tpls, depth=0, lines=None):
    """lines, when given, receives the source line of every output line after the first."""
    out, i, n, brace, line = [], 0, len(text), 0, 1

    def newline(src):
        if lines is not None:
            lines.append(src)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j]); i = j; continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for _ in range(text.count("\n", i, j)):
                line += 1; newline(line)
            out.append(text[i:j]); i = j; continue
        if c == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 1
            if j < n and text[j] == '"':
                j += 1
            out.append(text[i:j]); i = j; continue
        start_ok = i == 0 or text[i - 1] in " \t\r\n{};"
        kw = None
        if start_ok and text[i:i + 11].lower() == "inlineguide" and i + 11 < n and not IDENT.match(text[i + 11]):
            kw = 11
        elif start_ok and brace == 0 and text[i:i + 5].lower() == "guide" and i + 5 < n and not IDENT.match(text[i + 5]):
            kw = 5
        if kw:
            res = ref(text, i + kw, kw == 11, tpls, depth)
            if res:
                exp, end = res
                out.append(exp)
                for _ in range(exp.count("\n")):
                    newline(line)
                for _ in range(text.count("\n", i, end)):
                    out.append("\n"); line += 1; newline(line)
                i = end; continue
        if c == "{":
            brace += 1
        elif c == "}":
            brace -= 1
        elif c == "\n":
            line += 1; newline(line)
        out.append(c); i += 1
    return "".join(out)


def ref(text, p, inline, tpls, depth):
    n = len(text)
    while p < n and text[p] in " \t\r\n":
        p += 1
    decl = ""
    if not inline:
        if p < n and text[p] == '"':
            j = p + 1
            while j < n and text[j] not in '"\n':
                j += 1
            decl = text[p + 1:j]; p = j + 1 if j < n and text[j] == '"' else j
        else:
            j = p
            while j < n and text[j] not in " \t\r\n({}":
                j += 1
            decl = text[p:j]; p = j
        if not decl:
            return None
    while p < n and text[p] in " \t\r\n":
        p += 1
    j = p
    while j < n and IDENT.match(text[j]):
        j += 1
    name = text[p:j]; p = j
    while p < n and text[p] in " \t\r\n":
        p += 1
    if not name or p >= n or text[p] != "(":
        return None
    parsed = parse_args(text, p + 1)
    if parsed is None or name.lower() not in tpls:
        return None
    args, end = parsed
    parms, body = tpls[name.lower()]
    body = substitute(parms, body, args)
    if depth < 4:
        body = expand(body, tpls, depth + 1)
    return (body if inline else decl + " {" + body + "}"), end


def main():
    src, cpp = sys.argv[1], sys.argv[2]
    tpls = load_templates(os.path.join(src, "guides"))
    print(f"templates (python): {len(tpls)}")
    mismatches = problems = guide_materials = 0
    mdir = os.path.join(src, "materials")
    names = sorted(os.path.relpath(os.path.join(r, f), mdir)
                   for r, _, fs in os.walk(mdir) for f in fs if f.endswith(".mtr"))
    for f in names:
        orig = open(os.path.join(src, "materials", f), encoding="latin-1", newline="").read()
        want_lines = [1]
        want = expand(orig, tpls, lines=want_lines)
        got = open(os.path.join(cpp, "materials", f), encoding="latin-1", newline="").read()
        map_path = os.path.join(cpp, "materials", f + ".lines")
        if want != orig:
            got_lines = [int(x) for x in open(map_path).read().split()] if os.path.exists(map_path) else []
            if got_lines != want_lines:
                problems += 1; print(f"LINEMAP {f}: c++ {len(got_lines)} entries, python {len(want_lines)}")
            if len(want_lines) != got.count("\n") + 1 or want_lines[-1] != orig.count("\n") + 1 \
                    or any(b < a for a, b in zip(want_lines, want_lines[1:])):
                problems += 1; print(f"LINEMAP {f}: does not cover the file monotonically")
        if got != want:
            mismatches += 1
            k = next(i for i in range(min(len(got), len(want)) + 1) if i >= min(len(got), len(want)) or got[i] != want[i])
            print(f"MISMATCH {f} at {k}: c++ {got[max(0, k - 40):k + 60]!r}\n          python {want[max(0, k - 40):k + 60]!r}")
        # one statement per line inside a guide-defined material: qer_editorimage must not
        # share its line with anything that follows it
        # (retail text after the image name, like "cs_pipetech_01_d .tga", is skipped by the
        # engine as shipped; only braces or statements there mean an expansion was squeezed)
        for m in re.finditer(r"qer_editorimage[ \t]+\S+([^\n]*)", strip_comments(got), re.I):
            if re.search(r"[{}]|\b(diffusemap|bumpmap|specularmap|map|blend|materialType)\b", m.group(1), re.I):
                problems += 1; print(f"SAMELINE {f}: '{m.group(0)[:60]}' - the parser skips the rest of that line")
                break
        clean = strip_comments(got)
        clean = re.sub(r'"[^"\n]*"', '""', clean)
        if clean.count("{") != clean.count("}"):
            problems += 1; print(f"BRACES {f}: {clean.count('{')} open, {clean.count('}')} close")
        left = re.findall(r"(?:^|[\s{};])(?:inlineGuide|guide)\s+\S+", clean, re.I)
        if left:
            problems += 1; print(f"LEFTOVER {f}: {left[:3]}")
        for m in re.finditer(r"(?:^|[\s{};])guide\s+(\S+)\s+\w+\s*\(", strip_comments(orig)):
            guide_materials += 1
            if not re.search(re.escape(m.group(1)) + r" \{", got):
                problems += 1; print(f"MISSING {f}: material {m.group(1)}")
    print(f"guide-defined materials: {guide_materials}, mismatching files: {mismatches}, structural problems: {problems}")
    sys.exit(1 if mismatches or problems else 0)


if __name__ == "__main__":
    main()
