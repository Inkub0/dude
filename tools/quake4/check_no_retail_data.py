#!/usr/bin/env python3
"""Release gate: no retail game data in what we ship.

DUDE and the Quake 4 game library run on the player's own copy of the game; the release
archives must not carry any of its files, whole or edited (docs/quake4-release-licensing.md).
This indexes every file of a retail install - loose files and every member of every .pk4/.zip,
by size + CRC-32 from the zip directory, so nothing is decompressed - and then checks every file
in the given release archives/folders, descending into nested .zip/.pk4 (and .tar.gz):

  FAIL  same content as a retail file (any name)
  FAIL  same game path in the same game folder as a retail file (guis/mainmenu.gui inside a
        q4base/*.pk4, or a loose q4base/guis/mainmenu.gui): an override of the game's own file
        is an edited copy of it
  note  tiny files (< --min-size bytes) that match by content only: too small to mean anything

Usage:
  tools/quake4/check_no_retail_data.py --retail "/path/to/Quake 4" dist/release-X/*.tar.gz ...
  --allow GLOB   game paths that may collide (DUDE-authored files that deliberately replace one),
                 each needs a line in docs/quake4-release-licensing.md
Exit status 1 on any FAIL.
"""
import argparse
import fnmatch
import io
import os
import sys
import tarfile
import zipfile
import zlib

ARCHIVES = (".pk4", ".zip")
GAME_DIRS = ("q4base", "q4mp", "base", "d3xp", "d3le")


def game_split(path):
	"""(game dir, path as the game sees it) of a loose file; game dir '' when there is none"""
	p = path.replace("\\", "/").lower().lstrip("./")
	parts = p.split("/")
	for i, part in enumerate(parts[:-1]):
		if part in GAME_DIRS:
			return part, "/".join(parts[i + 1:])
	return "", p


def crc_of(data):
	return zlib.crc32(data) & 0xffffffff


def index_retail(root):
	by_content, by_path = {}, {}
	n = 0
	for d, _, names in os.walk(root):
		for name in names:
			full = os.path.join(d, name)
			rel = os.path.relpath(full, root)
			if name.lower().endswith(ARCHIVES):
				try:
					with zipfile.ZipFile(full) as z:
						for info in z.infolist():
							if info.is_dir():
								continue
							where = f"{rel}:{info.filename}"
							by_content.setdefault((info.file_size, info.CRC), where)
							by_path.setdefault((game_split(rel)[0], info.filename.lower()), where)
							n += 1
				except zipfile.BadZipFile:
					pass
				continue
			size = os.path.getsize(full)
			crc = 0
			with open(full, "rb") as f:
				for chunk in iter(lambda: f.read(1 << 20), b""):
					crc = zlib.crc32(chunk, crc)
			by_content.setdefault((size, crc & 0xffffffff), rel)
			by_path.setdefault(game_split(rel), rel)
			n += 1
	return by_content, by_path, n


def walk_shipped(target):
	"""yield (display path, (game dir, game path), size, crc)"""
	def from_zip(z, prefix, pk4_dir):
		for info in z.infolist():
			if info.is_dir():
				continue
			disp = f"{prefix}:{info.filename}"
			# a pk4's members are game paths in the folder holding the pk4; a zip's are package paths
			gp = (pk4_dir, info.filename.lower()) if pk4_dir is not None else game_split(info.filename)
			if info.filename.lower().endswith(ARCHIVES):
				yield from nested(z.read(info), disp, info.filename)
			yield disp, gp, info.file_size, info.CRC

	def nested(data, disp, path):
		try:
			with zipfile.ZipFile(io.BytesIO(data)) as z:
				pk4_dir = game_split(path)[0] if path.lower().endswith(".pk4") else None
				yield from from_zip(z, disp, pk4_dir)
		except zipfile.BadZipFile:
			pass

	if os.path.isdir(target):
		for d, _, names in os.walk(target):
			for name in names:
				full = os.path.join(d, name)
				rel = os.path.relpath(full, target)
				with open(full, "rb") as f:
					data = f.read()
				if name.lower().endswith(ARCHIVES):
					yield from nested(data, rel, rel)
				yield rel, game_split(rel), len(data), crc_of(data)
	elif target.endswith((".tar.gz", ".tgz", ".tar")):
		with tarfile.open(target) as t:
			for m in t.getmembers():
				if not m.isfile():
					continue
				data = t.extractfile(m).read()
				disp = f"{os.path.basename(target)}:{m.name}"
				if m.name.lower().endswith(ARCHIVES):
					yield from nested(data, disp, m.name)
				yield disp, game_split(m.name), len(data), crc_of(data)
	elif target.lower().endswith(ARCHIVES):
		with open(target, "rb") as f:
			data = f.read()
		yield from nested(data, os.path.basename(target), target)
	else:
		with open(target, "rb") as f:
			data = f.read()
		yield target, game_split(target), len(data), crc_of(data)


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--retail", action="append", required=True, help="a retail install (repeatable)")
	ap.add_argument("--allow", action="append", default=[])
	ap.add_argument("--min-size", type=int, default=64)
	ap.add_argument("targets", nargs="+")
	a = ap.parse_args()

	by_content, by_path = {}, {}
	for r in a.retail:
		c, p, n = index_retail(r)
		for k, v in c.items():
			by_content.setdefault(k, v)
		for k, v in p.items():
			by_path.setdefault(k, v)
		print(f"retail {r}: {n} files indexed")

	fails = notes = checked = 0
	for t in a.targets:
		for disp, gp, size, crc in walk_shipped(t):
			checked += 1
			hit = by_content.get((size, crc))
			if hit and size >= a.min_size:
				print(f"FAIL  {disp}: same content as retail {hit}")
				fails += 1
			elif hit and size > 0:
				print(f"note  {disp}: {size} bytes, same content as retail {hit} (too small to matter)")
				notes += 1
			if gp in by_path and not any(fnmatch.fnmatch(gp[1], g) for g in a.allow):
				print(f"FAIL  {disp}: replaces the retail file {by_path[gp]}")
				fails += 1
	print(f"{checked} shipped files checked: {fails} FAIL, {notes} notes")
	sys.exit(1 if fails else 0)


if __name__ == "__main__":
	main()
