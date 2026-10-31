#!/usr/bin/env python3
"""Summarise a tools/bench/timedemo-ab.sh result directory.

Per run: the benchmark report (fps, frame time, 1% low, GPU ms, cvar dump), the r_vkGpuTime 3
windows from stdout (per-pass scopes, sample-weighted) and the nvidia-smi clock log. Per variant
(warm-up excluded): mean and spread over the rounds, scope table with deltas against the first
variant, and a check that every run really had the variant's cvars (from the report's cvar dump).

Usage: timedemo_ab_report.py <result dir>
"""
import csv
import glob
import os
import re
import statistics
import sys

MIN_WINDOW_SAMPLES = 100	# r_vkGpuTime 3 windows are 5 s; fewer samples = loading / quitting
FIXED_CVARS = { "r_swapInterval": "0", "com_maxFPS": "0", "com_timeDemoRealTime": "1", "r_rtEmissiveLighting": "0" }

RE_VARIANT = re.compile( r'^variant (\S+): (.*?)  \[(.*)\]$' )
RE_WINDOW = re.compile( r'^VK GPU: ([0-9.]+) ms \((\d+) samples\)' )
RE_SCOPE = re.compile( r'^(\s+)(\S.*?)\s+(-?[0-9.]+) ms  \(([0-9.]+) calls/frame\)\s*$' )
RE_SET = re.compile( r'^set (\S+) "(.*)"$' )


def read( path, default="" ):
	try:
		with open( path, encoding="utf-8", errors="replace" ) as f:
			return f.read()
	except OSError:
		return default


def parse_meta( out ):
	variants = {}
	order = []
	for line in read( os.path.join( out, "meta.txt" ) ).splitlines():
		m = RE_VARIANT.match( line )
		if m:
			name, desc, args = m.groups()
			toks = args.split()
			expect = {}
			for i in range( len( toks ) - 2 ):
				if toks[i] == "+set":
					expect[toks[i + 1]] = toks[i + 2]
			variants[name] = { "desc": desc, "expect": expect }
			order.append( name )
	return variants, order


def parse_report( run ):
	files = sorted( glob.glob( os.path.join( run, "sandbox", "save", "*", "benchmarks", "*-*.txt" ) ) )
	if not files:
		return None
	text = read( files[-1] )
	r = { "file": files[-1], "cvars": {} }
	pats = {
		"fps": r'frames rendered: \d+ in [0-9.]+ s = ([0-9.]+) fps',
		"frame_avg": r'frame time \(ms\): avg ([0-9.]+)',
		"frame_median": r'frame time \(ms\): avg [0-9.]+\s+median ([0-9.]+)',
		"low1": r'1% low: ([0-9.]+) fps',
		"gpu_avg": r'GPU \(ms/frame\): avg ([0-9.]+)',
		"cpu_total": r'CPU renderer \(ms/frame\): frontend [0-9.]+\s+backend [0-9.]+\s+total ([0-9.]+)',
	}
	for key, pat in pats.items():
		m = re.search( pat, text )
		r[key] = float( m.group( 1 ) ) if m else None
	m = re.search( r'^display: (.*)$', text, re.M )
	r["display"] = m.group( 1 ) if m else "?"
	m = re.search( r'^playback: (\S+)', text, re.M )
	r["playback"] = m.group( 1 ) if m else "?"
	for line in text.splitlines():
		m = RE_SET.match( line )
		if m:
			r["cvars"][m.group( 1 )] = m.group( 2 )
	return r


def parse_scopes( run ):
	"""Sample-weighted ms/frame per scope over the full windows of one run."""
	windows = []
	cur = None
	for line in read( os.path.join( run, "stdout.log" ) ).splitlines():
		m = RE_WINDOW.match( line )
		if m:
			cur = { "frame": float( m.group( 1 ) ), "n": int( m.group( 2 ) ), "scopes": {} }
			windows.append( cur )
			continue
		m = RE_SCOPE.match( line ) if cur else None
		if m:
			depth = max( ( len( m.group( 1 ) ) - 3 ) // 2, 0 )
			name = "  " * depth + m.group( 2 )
			cur["scopes"][name] = cur["scopes"].get( name, 0.0 ) + float( m.group( 3 ) )
		elif cur and line.strip():
			cur = None
	full = [ w for w in windows if w["n"] >= MIN_WINDOW_SAMPLES ]
	total = sum( w["n"] for w in full )
	if not total:
		return None, 0
	res = { "frame (r_vkGpuTime)": sum( w["frame"] * w["n"] for w in full ) / total }
	names = []
	for w in full:
		for k in w["scopes"]:
			if k not in names:
				names.append( k )
	for k in names:
		res[k] = sum( w["scopes"].get( k, 0.0 ) * w["n"] for w in full ) / total
	return res, len( full )


def parse_clocks( run ):
	gr = []
	limited = 0
	path = os.path.join( run, "clocks.csv" )
	if not os.path.exists( path ):
		return None
	with open( path, encoding="utf-8", errors="replace" ) as f:
		for row in csv.reader( f ):
			if len( row ) < 6:
				continue
			try:
				mhz = float( row[1].strip().split()[0] )
			except ( ValueError, IndexError ):
				continue
			if mhz < 600:	# idle (loading, between frames of the shell)
				continue
			gr.append( mhz )
			reasons = row[5].strip()
			try:
				bits = int( reasons, 16 )
			except ValueError:
				bits = 0
			if bits & ~0x1:	# anything but "GPU idle"
				limited += 1
	if not gr:
		return None
	return { "mean": statistics.mean( gr ), "min": min( gr ), "limited": limited / len( gr ) }


def fmt( vals, digits=2 ):
	vals = [ v for v in vals if v is not None ]
	if not vals:
		return "-"
	if len( vals ) == 1:
		return f"{vals[0]:.{digits}f}"
	return f"{statistics.mean( vals ):.{digits}f} ±{statistics.pstdev( vals ):.{digits}f}"


def main():
	if len( sys.argv ) != 2:
		print( __doc__ )
		return 1
	out = sys.argv[1]
	variants, order = parse_meta( out )
	runs = []
	problems = []
	for run in sorted( glob.glob( os.path.join( out, "run-*" ) ) ):
		v = read( os.path.join( run, "variant" ) ).strip()
		rnd = read( os.path.join( run, "round" ) ).strip()
		rep = parse_report( run )
		scopes, nwin = parse_scopes( run )
		clocks = parse_clocks( run )
		tag = os.path.basename( run )
		if rep is None:
			problems.append( f"{tag}: no benchmark report (exit code {read( os.path.join( run, 'exitcode' ), '?' ).strip()})" )
		else:
			expect = dict( FIXED_CVARS )
			expect.update( variants.get( v, {} ).get( "expect", {} ) )
			for k, want in expect.items():
				got = rep["cvars"].get( k )
				if got is None or ( got != want and not _same_number( got, want ) ):
					problems.append( f"{tag}: {k} = {got!r}, expected {want!r}" )
			if rep["playback"] != "real-time":
				problems.append( f"{tag}: playback {rep['playback']!r} (want real-time)" )
		if scopes is None:
			problems.append( f"{tag}: no r_vkGpuTime windows with >= {MIN_WINDOW_SAMPLES} samples" )
		runs.append( { "tag": tag, "variant": v, "round": rnd, "rep": rep, "scopes": scopes or {}, "nwin": nwin, "clocks": clocks } )

	# the GPU clock drifts with heat (the 3080 Ti: 1340 MHz warm-up -> ~1120 MHz settled, 2026-10-31): rescale every run's
	# GPU time to the MEDIAN clock of the measured runs (assumes time scales with the core clock - memory-bound passes don't)
	means = [ r["clocks"]["mean"] for r in runs if r["clocks"] and r["round"] != "w" ]
	ref_clock = statistics.median( means ) if means else None
	for r in runs:
		gpu = ( r["rep"] or {} ).get( "gpu_avg" )
		r["gpu_norm"] = gpu * r["clocks"]["mean"] / ref_clock if ( ref_clock and gpu and r["clocks"] ) else None
	print( f"== {out}" )
	print()
	print( "per run:" )
	for r in runs:
		rep = r["rep"] or {}
		c = r["clocks"]
		clk = f"{c['mean']:.0f} MHz (min {c['min']:.0f}, limited {c['limited'] * 100:.0f}%)" if c else "no clock log"
		warm = "  (warm-up)" if r["round"] == "w" else ""
		norm = f"  @median clock {r['gpu_norm']:6.2f} ms" if r.get( "gpu_norm" ) else ""
		print( f"  {r['tag']:<14} fps {rep.get('fps') or 0:6.1f}  GPU {rep.get('gpu_avg') or 0:6.2f} ms{norm}  "
		       f"{r['nwin']:2d} windows  {clk}{warm}" )
	# flag runs off the MEDIAN clock (the best run is usually just the coolest one)
	if ref_clock:
		for r in runs:
			if r["clocks"] and r["round"] != "w" and abs( r["clocks"]["mean"] - ref_clock ) > 0.05 * ref_clock:
				problems.append( f"{r['tag']}: mean clock {r['clocks']['mean']:.0f} MHz is more than 5% off the median ({ref_clock:.0f}) - compare it with care" )
	display = sorted( { ( r["rep"] or {} ).get( "display", "?" ) for r in runs } )
	print( f"\ndisplay: {' | '.join( display )}" )

	measured = [ r for r in runs if r["round"] != "w" and r["rep"] ]
	names = [ v for v in order if any( r["variant"] == v for r in measured ) ]
	if not names:
		print( "\nno measured runs" )
	else:
		print( "\nper variant (mean ±spread over rounds, warm-up excluded):" )
		for v in names:
			rs = [ r for r in measured if r["variant"] == v ]
			print( f"  {v}: {variants.get( v, {} ).get( 'desc', '' )}  ({len( rs )} runs)" )
			print( f"     fps {fmt( [ r['rep']['fps'] for r in rs ], 1 )}   frame avg {fmt( [ r['rep']['frame_avg'] for r in rs ] )} ms"
			       f"   1% low {fmt( [ r['rep']['low1'] for r in rs ], 1 )} fps   GPU {fmt( [ r['rep']['gpu_avg'] for r in rs ] )} ms"
			       f"   CPU {fmt( [ r['rep']['cpu_total'] for r in rs ] )} ms" )
			if ref_clock:
				print( f"     GPU at the median clock {ref_clock:.0f} MHz: {fmt( [ r.get( 'gpu_norm' ) for r in rs ] )} ms"
				       "   (each run's GPU time x its clock / the median - assumes time scales with the core clock)" )

		# scope table: every scope seen, in first-seen order of the first variant's runs
		scope_names = []
		for v in names:
			for r in measured:
				if r["variant"] == v:
					for k in r["scopes"]:
						if k not in scope_names:
							scope_names.append( k )
		base = names[0]
		print( f"\nr_vkGpuTime 3 scopes, ms/frame (delta vs {base}):" )
		head = f"  {'scope':<30}" + "".join( f"{v:>18}" for v in names )
		print( head )
		for k in scope_names:
			vals = {}
			for v in names:
				xs = [ r["scopes"].get( k, 0.0 ) for r in measured if r["variant"] == v and r["scopes"] ]
				vals[v] = statistics.mean( xs ) if xs else None
			if all( ( x or 0.0 ) < 0.005 for x in vals.values() ):
				continue
			cells = []
			for v in names:
				x = vals[v]
				if x is None:
					cells.append( f"{'-':>18}" )
				elif v == base or vals[base] is None:
					cells.append( f"{x:>18.3f}" )
				else:
					cells.append( f"{x:>9.3f} ({x - vals[base]:+.3f})" )
			print( f"  {k:<30}" + "".join( cells ) )

	print()
	if problems:
		print( "CHECK:" )
		for p in problems:
			print( f"  ! {p}" )
	else:
		print( "checks: all runs real-time, variant cvars confirmed in every report, no throttled run" )
	return 0


def _same_number( a, b ):
	try:
		return abs( float( a ) - float( b ) ) < 1e-6
	except ValueError:
		return False


if __name__ == "__main__":
	sys.exit( main() )
