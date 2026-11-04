#!/usr/bin/env python3
"""Did any code from a reference tree land in DUDE's Quake 4 work verbatim?

docs/quake4-port.md 3, rule 1: nothing SDK-derived goes into DUDE, and decompile-derived code
(OpenQ4 src/bse, idTech4A++ raven/bse, Quake4Decompiled) is reference-only, never copied. This
compares every DUDE source file added or changed since the Quake 4 work began against reference
trees, token by token, and reports runs of identical code.

How: comments and whitespace are dropped, the rest is tokenised; every window of K tokens is
hashed. Windows that already existed anywhere in DUDE before the Quake 4 work (id's GPL code,
dhewm3, earlier DUDE work - all of which the reference ports share) are removed first, so what
is left is new DUDE code that also exists in a reference tree.

Expected and fine: Quake 4 interface declarations (method signatures, enum/flag values) that
DUDE must repeat to load the game library - the SDK and the ports carry the same ones. Anything
with statements in it (loops, arithmetic, calls) deserves a read.

Usage:
  tools/quake4/check_provenance.py [--since <commit>] [--k 24] [--min-run 2] \
      --ref sdk=~/quake4-refs/q4sdk-frisk-initial \
      --ref openq4-bse=~/quake4-refs/openq4/src/bse ...
Exit status 1 when any file has a run of --fail-run windows or more against a ref whose name
ends in '!' (e.g. --ref 'openq4-bse!=...': decompile-derived trees, zero tolerance). Text that is
also in an --interface tree (the SDK) is not held against a '!' ref.

The release gate (docs/releasing.md) runs:
  tools/quake4/check_provenance.py --interface ~/quake4-refs/q4sdk-frisk-initial \
      --ref 'openq4-bse!=~/quake4-refs/openq4/src/bse' \
      --ref 'idtech4a-bse!=~/quake4-refs/idtech4a/doom3/neo/raven/bse' \
      --ref q4sdk=~/quake4-refs/q4sdk-frisk-initial
"""
import argparse
import collections
import os
import re
import subprocess
import sys

DUDE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXTS = (".cpp", ".h", ".c", ".hpp", ".inl", ".glsl", ".vert", ".frag", ".comp", ".py")
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d[\w.]*|::|->|<<|>>|[<>=!+\-*/&|]=|&&|\|\||\S')
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def tokens(text):
	"""[(token, line)]"""
	out = []
	text = COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
	for ln, line in enumerate(text.split("\n"), 1):
		if line.lstrip().startswith("#include"):
			continue
		for t in TOKEN.findall(line):
			out.append((t, ln))
	return out


def windows(toks, k):
	h = []
	for i in range(len(toks) - k + 1):
		h.append(hash(tuple(t for t, _ in toks[i:i + k])))
	return h


def git(*args, **kw):
	return subprocess.run(["git", "-C", DUDE] + list(args), capture_output=True, check=True, **kw).stdout


def tree_files(rev, prefix="neo"):
	"""{path: text} of a past revision, through one cat-file --batch."""
	names = [n for n in git("ls-tree", "-r", "--name-only", rev, prefix, text=True).split("\n")
		if n.endswith(EXTS)]
	proc = subprocess.run(["git", "-C", DUDE, "cat-file", "--batch"],
		input="".join(f"{rev}:{n}\n" for n in names).encode(), capture_output=True, check=True).stdout
	out, pos = {}, 0
	for n in names:
		nl = proc.index(b"\n", pos)
		size = int(proc[pos:nl].split()[2])
		out[n] = proc[nl + 1:nl + 1 + size].decode("latin-1")
		pos = nl + 1 + size + 1
	return out


def walk(root):
	for d, _, names in os.walk(root):
		for n in names:
			if n.endswith(EXTS):
				p = os.path.join(d, n)
				try:
					with open(p, encoding="latin-1") as f:
						yield p, f.read()
				except OSError:
					pass


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--since", help="DUDE revision before the Quake 4 work (default: parent of the "
		"first commit whose subject says quake4)")
	ap.add_argument("--ref", action="append", required=True, help="name=path; name ending in ! = zero tolerance")
	ap.add_argument("--k", type=int, default=24, help="tokens per window (default 24, about two lines)")
	ap.add_argument("--min-run", type=int, default=2, help="report runs of at least this many windows")
	ap.add_argument("--fail-run", type=int, default=4)
	ap.add_argument("--interface", action="append", default=[], help="tree of the Quake 4 SDK: its "
		"windows (the published engine interface every port repeats) never count against a '!' ref")
	ap.add_argument("--show", type=int, default=4, help="runs shown per file and ref")
	a = ap.parse_args()

	since = a.since
	if not since:
		first = git("log", "--reverse", "-i", "--grep=quake4", "--format=%H", text=True).split()[0]
		since = first + "^"
	since = git("rev-parse", "--short", since, text=True).strip()

	base = set()
	for text in tree_files(since).values():
		base.update(windows(tokens(text), a.k))

	changed = [p for p in git("diff", "--name-only", "--diff-filter=AM", since, "HEAD", "--",
		"neo", "tools/quake4", text=True).split("\n") if p.endswith(EXTS)]
	mine = {}
	for p in changed:
		with open(os.path.join(DUDE, p), encoding="latin-1") as f:
			toks = tokens(f.read())
		mine[p] = (toks, windows(toks, a.k))

	iface = set()
	for root in a.interface:
		for _, text in walk(os.path.expanduser(root)):
			iface.update(windows(tokens(text), a.k))
	print(f"baseline {since}: {len(base)} windows; {len(changed)} DUDE files changed since; k={a.k}")
	failed = False
	for spec in a.ref:
		name, path = spec.split("=", 1)
		strict = name.endswith("!")
		path = os.path.expanduser(path)
		ref = set()
		nfiles = 0
		for _, text in walk(path):
			ref.update(windows(tokens(text), a.k))
			nfiles += 1
		ref -= base
		if strict:
			ref -= iface
		print(f"\n== {name} ({path}: {nfiles} files, {len(ref)} windows not already in DUDE's baseline)")
		rows = []
		for p, (toks, wins) in mine.items():
			runs, cur = [], None
			for i, h in enumerate(wins):
				if h in ref and h not in base:
					cur = [i, i] if cur is None else [cur[0], i]
				elif cur is not None:
					runs.append(cur)
					cur = None
			if cur is not None:
				runs.append(cur)
			runs = [r for r in runs if r[1] - r[0] + 1 >= a.min_run]
			if runs:
				total = sum(r[1] - r[0] + 1 for r in runs)
				longest = max(r[1] - r[0] + 1 for r in runs)
				rows.append((longest, total, p, runs, toks))
		rows.sort(reverse=True)
		if not rows:
			print("   no shared runs")
		for longest, total, p, runs, toks in rows:
			flag = " FAIL" if strict and longest >= a.fail_run else ""
			failed |= bool(flag)
			print(f"  {p}: {len(runs)} runs, longest {longest} windows, {total} total{flag}")
			for r in sorted(runs, key=lambda r: r[0] - r[1])[:a.show]:
				l0, l1 = toks[r[0]][1], toks[r[1] + a.k - 1][1]
				snippet = " ".join(t for t, _ in toks[r[0]:r[0] + 14])
				print(f"      lines {l0}-{l1}: {snippet[:110]}")
	sys.exit(1 if failed else 0)


if __name__ == "__main__":
	main()
