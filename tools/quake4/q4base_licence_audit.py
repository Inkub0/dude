#!/usr/bin/env python3
"""Which GPL code ends up inside the Quake 4 game library, and whose is it?

q4base.so is Raven's game code (Quake 4 SDK EULA) linked with DUDE's idlib and compiled against
DUDE's headers (GPL). Shipping that binary is only a problem for the GPL code inside it that
nobody can offer under the SDK terms. This tool measures it (docs/quake4-release-licensing.md):

  1. every DUDE source line that produced machine code in the binary, from its DWARF line
     tables (covers the idlib objects the linker pulled from libidlib.a AND the inline/template
     code the game compiled out of DUDE's headers);
  2. git blame of each such line, sorted into
       id-sdk   id's GPL release (root commit) AND the same text is in the SDK's idlib
                -> id code we also hold under the SDK EULA
       id-gpl   id's GPL release, text NOT in the SDK -> GPL-only id code
       own      written by the DUDE author (--own names); own commits whose message names
                dhewm3 / idTech4A++ / OpenQ4 are listed for a human read (they usually say
                "consulted, our own code"; one that says "ported" is third-party code)
       third    anyone else (dhewm3 and other contributors)
  3. optional: the size of the game repository's commits that carry dhewm3 code.

Lines that are blank-ish after normalising (braces, 'return;', ...) are counted apart as
'trivial': they produce code (a closing brace is an epilogue) but carry no expression.

Usage:
  tools/quake4/q4base_licence_audit.py --so path/q4base.so --sdk path/to/pristine/sdk/source \
      [--game-repo ~/dude-quake4-game] [--own Inkub0] [--json out.json]
Needs llvm-dwarfdump (or set LLVM_DWARFDUMP) and a q4base.so built with debug info (DUDE's
default build flags have -g).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

DUDE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PORT_RE = re.compile(r"dhewm3|idtech4a|openq4|glkarin|themuffinator|dark ?mod", re.I)
# DUDE commit messages name dhewm3 for its ABI check all the time; that is not a port
NOT_PORT_RE = re.compile(r"check_dhewm3_abi(\.py)?|dhewm3 (1\.5\.5 )?(game )?ABI|dhewm3-ABI", re.I)
OWN_UNCOMMITTED = "(uncommitted)"
TRIVIAL_RE = re.compile(r"^[{}();,\s]*$|^(return|break|continue|else|default:|do)\s*;?\s*[{}]?$")


def norm(line):
	"""comment-free, whitespace-free: Raven's SDK and id's GPL release space the same code
	differently ('strcmp(a, b)' vs 'strcmp( a, b )'), which is not a different expression."""
	line = re.sub(r"//.*$", "", line)
	return "".join(line.split())


def is_trivial(text):
	n = norm(text)
	return len(n) < 4 or bool(TRIVIAL_RE.match(n)) or n in ("return;", "break;", "continue;", "else{", "}else{", "do{")


def dwarf_lines(so):
	"""{abs path: set(line)} for every row of every line table."""
	tool = os.environ.get("LLVM_DWARFDUMP", "llvm-dwarfdump")
	proc = subprocess.Popen([tool, "--debug-line", so], stdout=subprocess.PIPE, text=True,
		errors="replace")
	result = collections.defaultdict(set)
	dirs, files = {}, {}
	cur_name = None
	# address, line, column, file; a sequence the linker discarded (--gc-sections, folded
	# COMDAT) keeps its rows with a tombstone start address (0 or ~0): skip those sequences
	row = re.compile(r"^0x([0-9a-f]+)\s+(\d+)\s+\d+\s+(\d+)\s")
	seq_start, dead = True, False
	for line in proc.stdout:
		if line.startswith("debug_line["):
			dirs, files = {}, {}
			continue
		m = re.match(r'include_directories\[\s*(\d+)\] = "(.*)"', line)
		if m:
			dirs[int(m.group(1))] = m.group(2)
			continue
		m = re.match(r"file_names\[\s*(\d+)\]:", line)
		if m:
			cur_name = int(m.group(1))
			files[cur_name] = [None, 0]
			continue
		m = re.match(r'\s+name: "(.*)"', line)
		if m and cur_name is not None:
			files[cur_name][0] = m.group(1)
			continue
		m = re.match(r"\s+dir_index: (\d+)", line)
		if m and cur_name is not None:
			files[cur_name][1] = int(m.group(1))
			continue
		m = row.match(line)
		if m:
			addr, ln, fi = int(m.group(1), 16), int(m.group(2)), int(m.group(3))
			if seq_start:
				dead = addr == 0 or addr >= 0xffffffff00000000
				seq_start = False
			if "end_sequence" in line:
				seq_start = True
				continue
			if dead or ln == 0 or fi not in files:
				continue
			name, di = files[fi]
			d = dirs.get(di, "")
			if not os.path.isabs(d):
				d = os.path.join(dirs.get(0, ""), d)
			path = os.path.normpath(os.path.join(d, name))
			result[path].add(ln)
	proc.wait()
	if proc.returncode:
		sys.exit("llvm-dwarfdump failed")
	return result


def blame(path):
	"""[(commit, text)] per line, 1-based index via list position + 1."""
	out = subprocess.run(["git", "-C", DUDE, "blame", "-w", "-M", "--line-porcelain", "--", path],
		capture_output=True, text=True, errors="replace", check=True).stdout
	lines, commit = [], None
	for l in out.splitlines():
		if re.match(r"^[0-9a-f]{40} ", l):
			commit = l.split()[0]
		elif l.startswith("\t"):
			lines.append((commit, l[1:]))
	return lines


def commit_info(commits):
	info = {}
	for c in commits:
		if set(c) == {"0"}:		# git blame's mark for uncommitted lines: this checkout's author
			info[c] = (OWN_UNCOMMITTED, "uncommitted changes", "")
			continue
		out = subprocess.run(["git", "-C", DUDE, "show", "-s", "--format=%an%x00%s%x00%B", c],
			capture_output=True, text=True, errors="replace", check=True).stdout
		an, subj, body = out.split("\0", 2)
		info[c] = (an, subj, body)
	return info


def sdk_texts(sdk):
	"""normalised lines per SDK basename, plus one pool for the whole SDK source (idlib and the
	engine interface headers are id code too; the game code is Raven's, also under the EULA)."""
	per, pool = collections.defaultdict(set), set()
	for root, _, names in os.walk(sdk):
		for n in names:
			if not n.endswith((".h", ".cpp", ".inl")):
				continue
			with open(os.path.join(root, n), encoding="latin-1") as f:
				for line in f:
					t = norm(line)
					if t:
						per[n.lower()].add(t)
						pool.add(t)
	return per, pool


def game_repo_ports(repo):
	out = subprocess.run(["git", "-C", repo, "log", "-i", "--grep=dhewm3", "--format=%h%x00%s",
		"--numstat"], capture_output=True, text=True, check=True).stdout
	res, cur = [], None
	for l in out.splitlines():
		if "\0" in l:
			h, s = l.split("\0", 1)
			cur = {"commit": h, "subject": s, "added": 0, "removed": 0}
			res.append(cur)
		elif l.strip() and cur is not None:
			a, r, _ = l.split("\t", 2)
			if a != "-":
				cur["added"] += int(a)
				cur["removed"] += int(r)
	return res


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--so", required=True)
	ap.add_argument("--sdk", required=True, help="pristine Quake 4 SDK source/ (has idlib/)")
	ap.add_argument("--game-repo")
	ap.add_argument("--own", action="append", default=None, help="author name(s) of DUDE's own code")
	ap.add_argument("--json")
	a = ap.parse_args()
	own = set(a.own or ["Inkub0"])

	neo = os.path.join(DUDE, "neo") + os.sep
	code = {p: ls for p, ls in dwarf_lines(a.so).items() if p.startswith(neo) and os.path.exists(p)}
	root = subprocess.run(["git", "-C", DUDE, "rev-list", "--max-parents=0", "HEAD"],
		capture_output=True, text=True, check=True).stdout.split()
	per_sdk, pool = sdk_texts(a.sdk)

	cls_total = collections.Counter()
	cls_trivial = collections.Counter()
	per_file = collections.defaultdict(collections.Counter)
	per_commit = collections.defaultdict(lambda: collections.Counter())
	samples = collections.defaultdict(list)
	idgpl = collections.defaultdict(list)
	blames = {}
	mentions = collections.Counter()
	for p in sorted(code):
		rel = os.path.relpath(p, DUDE)
		try:
			blames[rel] = blame(rel)
		except subprocess.CalledProcessError:
			continue
	info = commit_info({c for b in blames.values() for c, _ in b})

	for rel, b in blames.items():
		base = os.path.basename(rel).lower()
		for ln in sorted(code[os.path.join(DUDE, rel)]):
			if ln > len(b):
				continue
			c, text = b[ln - 1]
			an, subj, body = info[c]
			t = norm(text)
			if c in root:
				cls = "id-sdk" if (t in per_sdk.get(base, ()) or t in pool) else "id-gpl"
			elif an in own or an == OWN_UNCOMMITTED:
				cls = "own"
				if PORT_RE.search(NOT_PORT_RE.sub("", body)):
					mentions[c] += 1
			else:
				cls = "third"
			if is_trivial(text):
				cls_trivial[cls] += 1
				continue
			cls_total[cls] += 1
			per_file[rel][cls] += 1
			if cls in ("third", "id-gpl"):
				per_commit[(cls, c)][rel] += 1
				if len(samples[(cls, c)]) < 3:
					samples[(cls, c)].append(f"{rel}:{ln}: {t[:100]}")
				if cls == "id-gpl":
					idgpl[rel].append(f"{ln}: {t[:120]}")

	total = sum(cls_total.values())
	print(f"q4base: {len(code)} DUDE files produce code; {total} substantive code lines "
		f"(+{sum(cls_trivial.values())} trivial)")
	for cls in ("id-sdk", "id-gpl", "own", "third"):
		n = cls_total[cls]
		print(f"  {cls:9s} {n:6d}  {100.0 * n / max(total, 1):5.1f} %   (+{cls_trivial[cls]} trivial)")
	print("\nfiles with id-gpl or third lines:")
	rows = sorted(per_file.items(), key=lambda kv: -(kv[1]["id-gpl"] + kv[1]["third"]))
	for rel, cnt in rows:
		if cnt["id-gpl"] + cnt["third"]:
			print(f"  {rel:50s} id-gpl {cnt['id-gpl']:5d}  third {cnt['third']:5d}  own {cnt['own']:4d}")
	print("\nown commits whose message names another project (read them: consulted or ported?):")
	for c, n in mentions.most_common():
		print(f"  {n:5d}  {c[:8]} {info[c][1][:90]}")
	print("\ncommits behind 'third' lines:")
	commits = sorted(((k, sum(v.values())) for k, v in per_commit.items() if k[0] != "id-gpl"),
		key=lambda kv: -kv[1])
	for (cls, c), n in commits:
		an, subj, _ = info[c]
		print(f"  {n:5d}  {cls:8s} {c[:8]} {an}: {subj[:80]}")
		for s in samples[(cls, c)]:
			print(f"           {s}")

	ports = game_repo_ports(a.game_repo) if a.game_repo else []
	if ports:
		print(f"\ngame repository commits that carry dhewm3 code ({len(ports)}):")
		for p in sorted(ports, key=lambda p: -p["added"]):
			print(f"  +{p['added']:4d} -{p['removed']:4d}  {p['commit']} {p['subject'][:80]}")

	if a.json:
		with open(a.json, "w") as f:
			json.dump({
				"classes": cls_total, "trivial": cls_trivial,
				"files": {k: dict(v) for k, v in per_file.items()},
				"id_gpl_lines": idgpl,
				"own_mentions": {c: n for c, n in mentions.items()},
				"commits": [{"class": cls, "commit": c, "author": info[c][0], "subject": info[c][1],
					"lines": n, "samples": samples[(cls, c)]} for (cls, c), n in commits],
				"game_repo_ports": ports,
			}, f, indent=1)


if __name__ == "__main__":
	main()
