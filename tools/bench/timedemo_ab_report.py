#!/usr/bin/env python3
"""Summarise a tools/bench/timedemo-ab.sh result directory.

Per run: the benchmark report (fps, frame time, 1% low, GPU ms, cvar dump), the r_vkGpuTime 3
windows from stdout (per-pass scopes, sample-weighted) and the nvidia-smi clock log (loaded samples
only: clock, temperature, power, throttle reasons). Per variant (warm-up excluded): mean and spread
over the rounds, scope table with deltas against the first variant, the CLOCK-INDEPENDENT change of
every scope (paired demo windows against the median shift of the untouched passes - the number to
trust on a card that throttles), and a check that every run really had the variant's cvars.

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
		"cpu_fe": r'CPU renderer \(ms/frame\): frontend ([0-9.]+)',
		"cpu_be": r'CPU renderer \(ms/frame\): frontend [0-9.]+\s+backend ([0-9.]+)',
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


def merge_order( order, seq ):
	"""Merge the scope names of one printed window (seq, in print order) into order: a name not seen yet goes right after the
	name printed before it, so a scope missing from the base order still lands next to its neighbours, not at the end
	(2026-11-04: rt.animblas.blas was listed under post)."""
	prev = None
	for k in seq:
		if k not in order:
			order.insert( order.index( prev ) + 1 if prev in order else len( order ), k )
		prev = k
	return order


def parse_scopes( run ):
	"""Sample-weighted ms/frame per scope over the full windows of one run, the window count, and the full windows themselves
	({scope: ms}, in demo order - window i of every run covers the same stretch of the real-time demo)."""
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
		return None, 0, []
	res = { "frame (r_vkGpuTime)": sum( w["frame"] * w["n"] for w in full ) / total }
	# the base order = the window order seen most often: the backend prints a window's scopes in the order they FIRST RAN in
	# it, so a scope that only started mid-window (the first animated character) is printed last in that one window
	orders = {}
	for w in full:
		key = tuple( w["scopes"] )
		orders[key] = orders.get( key, 0 ) + 1
	names = list( max( orders, key=lambda o: ( orders[o], len( o ) ) ) ) if orders else []
	for w in full:
		merge_order( names, list( w["scopes"] ) )
	for k in names:
		res[k] = sum( w["scopes"].get( k, 0.0 ) * w["n"] for w in full ) / total
	per_window = [ dict( w["scopes"], **{ "frame (r_vkGpuTime)": w["frame"] } ) for w in full ]
	return res, len( full ), per_window


# nvidia-smi clocks_throttle_reasons.active bits (the clock log's last column)
THROTTLE_POWER   = 0x4				# SW power cap
THROTTLE_HW      = 0x8 | 0x80		# HW slowdown, HW power brake
THROTTLE_THERMAL = 0x20 | 0x40		# SW thermal slowdown, HW thermal slowdown
THROTTLE_REPORT_FRACTION = 0.10		# a reason active in more of a run's loaded samples than this is reported


def parse_clocks( run ):
	"""The nvidia-smi log of one run, restricted to the samples the GPU was LOADED: power >= 60 % of the run's 90th-percentile
	power (loading screens and the shell draw far less). Not a fixed clock floor: a hot or capped card runs the demo at
	400-700 MHz (2026-11-04: a 600 MHz floor dropped half the loaded samples and hid the thermal throttling). Without a power
	column, a clock >= 50 % of the run's 90th percentile."""
	rows = []
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
			try:
				watts = float( row[4].strip().split()[0] )
			except ( ValueError, IndexError ):
				watts = None
			try:
				temp = float( row[3].strip() )
			except ValueError:
				temp = None
			try:
				bits = int( row[5].strip(), 16 )
			except ValueError:
				bits = 0
			rows.append( ( mhz, watts, temp, bits ) )
	if not rows:
		return None
	def p90( xs ):
		xs = sorted( xs )
		return xs[ int( 0.9 * ( len( xs ) - 1 ) ) ]
	powered = [ r for r in rows if r[1] is not None ]
	if len( powered ) >= 0.8 * len( rows ):
		floor = 0.6 * p90( [ r[1] for r in powered ] )
		loaded = [ r for r in powered if r[1] >= floor ]
	else:
		floor = 0.5 * p90( [ r[0] for r in rows ] )
		loaded = [ r for r in rows if r[0] >= floor ]
	if not loaded:
		return None
	n = len( loaded )
	frac = lambda mask: sum( 1 for r in loaded if r[3] & mask ) / n
	temps = [ r[2] for r in loaded if r[2] is not None ]
	watts = [ r[1] for r in loaded if r[1] is not None ]
	return { "mean": statistics.mean( r[0] for r in loaded ), "min": min( r[0] for r in loaded ), "n": n,
	         "temp_max": max( temps ) if temps else None, "watts": statistics.mean( watts ) if watts else None,
	         "thermal": frac( THROTTLE_THERMAL ), "power": frac( THROTTLE_POWER ), "hw": frac( THROTTLE_HW ) }


PAIRED_MIN_MS = 0.05	# scopes below this (in both windows) neither get a line nor vote in a window's reference shift
AGREE_MIN_MS = 0.5		# run-agreement check: scopes at least this big whose runs of ONE variant differ (clock-independent) by
AGREE_MAX = 0.08		# more than this AND by AGREE_FACTOR x the other variants' own worst run-to-run difference on that scope
AGREE_FACTOR = 3.0		# (the boost, FSR2 and the denoiser differ 5-20 % run to run anyway; the gather ~1 %)


def paired_change( wins_a, wins_b, k, leaves ):
	"""Per demo window: scope k's ratio b / a over the median ratio of the window's leaf scopes, minus 1 (a list)."""
	out = []
	for wa, wb in zip( wins_a, wins_b ):
		if wa.get( k, 0.0 ) < PAIRED_MIN_MS or wb.get( k, 0.0 ) < PAIRED_MIN_MS:
			continue
		ref = [ wb[j] / wa[j] for j in wa if j in leaves and wa[j] >= PAIRED_MIN_MS and wb.get( j, 0.0 ) >= PAIRED_MIN_MS ]
		if len( ref ) >= 5:
			out.append( ( wb[k] / wa[k] ) / statistics.median( ref ) - 1.0 )
	return out


def paired_section( measured, names, scope_names ):
	"""Clock-independent change of every scope vs the first variant. For each (base run, variant run) pair and each demo window
	in both, a scope's ratio variant / base is divided by the MEDIAN ratio of the window's leaf scopes: heat, the power cap and
	the clock slow every pass alike, a change only the passes it touches. Robust while most passes are untouched (the
	median ignores the changed ones); a change that moves most passes shows up as 'everything else' moving instead."""
	base = names[0]
	quant = lambda xs, q: sorted( xs )[ int( q * ( len( xs ) - 1 ) ) ]
	# only LEAF scopes vote in the reference: a parent (frame, world, post) moves with whatever changed inside it
	depth = lambda k: len( k ) - len( k.lstrip( " " ) )
	leaves = { k for i, k in enumerate( scope_names ) if k != "frame (r_vkGpuTime)"
	           and not ( i + 1 < len( scope_names ) and depth( scope_names[i + 1] ) > depth( k ) ) }
	base_runs = [ r for r in measured if r["variant"] == base and r["windows"] ]
	others = [ v for v in names[1:] ]
	warnings = []
	# RUN AGREEMENT: two runs of the SAME binary and settings must agree clock-independently. A run that drifts mid-way
	# (2026-11-04 trinormal: two of three B runs stepped up +20 % in the gather after ~20 s - a runtime buffer reallocation
	# landing in slower memory? - while the third matched master) is not the variant's speed: it made a neutral change look 14 %
	# slower. Checked per variant, before any A/B reading.
	worst = {}
	for v in names:
		rs = [ r for r in measured if r["variant"] == v and r["windows"] ]
		for k in scope_names:
			if k not in leaves or not rs or statistics.mean( r["scopes"].get( k, 0.0 ) for r in rs ) < AGREE_MIN_MS:
				continue
			for i in range( len( rs ) ):
				for j in range( i + 1, len( rs ) ):
					ch = paired_change( rs[i]["windows"], rs[j]["windows"], k, leaves )
					if ch and ( ( v, k ) not in worst or abs( statistics.median( ch ) ) > abs( worst[v, k][0] ) ):
						worst[v, k] = ( statistics.median( ch ), rs[i]["tag"], rs[j]["tag"] )
	for ( v, k ), ( w, ta, tb ) in worst.items():
		others_worst = max( ( abs( worst[u, k][0] ) for u in names if u != v and ( u, k ) in worst ), default=0.0 )
		if abs( w ) > AGREE_MAX and abs( w ) > AGREE_FACTOR * others_worst:
			warnings.append( f"variant {v}: its own runs disagree on {k.strip()} by {w * 100:+.0f}% ({tb} vs {ta}, clock-independent; the other "
			                 f"variants' runs within {others_worst * 100:.0f}%) - a run changed mid-way, not the code: read the per-window data "
			                 "before trusting the A/B" )
	if not base_runs or not others:
		return warnings
	print( f"\nclock-independent change vs {base}: each scope vs the median shift of the leaf scopes in the same demo window,"
	       f" paired over every {base} x variant run pair (median, p10..p90; * = the whole p10..p90 range on one side of 0):" )
	print( f"  {'scope':<30}{base + ' ms':>10}" + "".join( f"{v:>34}" for v in others ) )
	for k in scope_names:
		base_ms = statistics.mean( r["scopes"].get( k, 0.0 ) for r in base_runs )
		if base_ms < PAIRED_MIN_MS:
			continue
		cells = []
		for v in others:
			adj = []
			for a in base_runs:
				for b in ( r for r in measured if r["variant"] == v and r["windows"] ):
					adj += paired_change( a["windows"], b["windows"], k, leaves )
			if len( adj ) < 5:
				cells.append( f"{'-':>34}" )
				continue
			med, lo, hi = statistics.median( adj ), quant( adj, 0.1 ), quant( adj, 0.9 )
			sure = "*" if lo > 0 or hi < 0 else " "
			cells.append( f"{med * 100:+6.1f}% ({lo * 100:+5.1f}..{hi * 100:+5.1f}) {med * base_ms:+6.2f} ms{sure}" )
		print( f"  {k:<30}{base_ms:>10.3f}" + "".join( cells ) )
	return warnings


def fmt_clocks( c ):
	if not c:
		return "no clock log"
	out = f"{c['mean']:.0f} MHz (min {c['min']:.0f})"
	if c["temp_max"] is not None:
		out += f" {c['temp_max']:.0f} C"
	if c["watts"] is not None:
		out += f" {c['watts']:.0f} W"
	why = [ f"{name} {c[k] * 100:.0f}%" for k, name in ( ( "thermal", "thermal" ), ( "power", "power cap" ), ( "hw", "HW slowdown" ) )
	        if c[k] > THROTTLE_REPORT_FRACTION ]
	return out + ( f"  throttled: {', '.join( why )}" if why else "" )


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
		scopes, nwin, wins = parse_scopes( run )
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
			# emissive mode on: the demo's map must have come up AUTHORED (the setup copied into the sandbox), or the run
			# measured nothing of emissive mode
			if expect.get( "r_rtEmissiveLighting" ) == "1":
				log = read( os.path.join( run, "stdout.log" ) )
				if "emissive authoring:" in log and not re.search( r'^emissive authoring: \S+ - .*: \d+ lights,', log, re.M ):
					problems.append( f"{tag}: emissive mode on but the demo's map is UNAUTHORED (no setup in the sandbox)" )
		# the GPU the game actually used (2026-11-04: a broken NVIDIA driver after an update - every run fell back to the iGPU)
		log = read( os.path.join( run, "stdout.log" ) )
		m = re.search( r'^VK: backend up - (.*?), \d+ MB VRAM', log, re.M )
		gl = None if m else re.search( r'^OpenGL renderer: ([^/\n]*)', log, re.M )
		gpu = m.group( 1 ) if m else ( gl.group( 1 ).strip() if gl else "?" )
		# r_vkGpuTime is Vulkan-only: a GL3 run has no scope windows by design
		if scopes is None and not gl:
			problems.append( f"{tag}: no r_vkGpuTime windows with >= {MIN_WINDOW_SAMPLES} samples" )
		if clocks is None and rnd != "w":
			problems.append( f"{tag}: no clock log - nvidia-smi gave nothing (a broken driver? the run used: {gpu})" )
		runs.append( { "tag": tag, "variant": v, "round": rnd, "rep": rep, "scopes": scopes or {}, "nwin": nwin, "clocks": clocks, "gpu": gpu,
		               "windows": wins } )

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
		clk = fmt_clocks( r["clocks"] )
		warm = "  (warm-up)" if r["round"] == "w" else ""
		norm = f"  @median clock {r['gpu_norm']:6.2f} ms" if r.get( "gpu_norm" ) else ""
		print( f"  {r['tag']:<14} fps {rep.get('fps') or 0:6.1f}  GPU {rep.get('gpu_avg') or 0:6.2f} ms{norm}  "
		       f"{r['nwin']:2d} windows  {clk}{warm}" )
	# flag runs off the MEDIAN clock (the best run is usually just the coolest one)
	if ref_clock:
		for r in runs:
			if r["clocks"] and r["round"] != "w" and abs( r["clocks"]["mean"] - ref_clock ) > 0.05 * ref_clock:
				problems.append( f"{r['tag']}: mean clock {r['clocks']['mean']:.0f} MHz is more than 5% off the median ({ref_clock:.0f}) - compare it with care" )
	# throttling: one line per reason over the measured runs (the card's limits, not the variants', set the clock then - A/B stays
	# comparable while every variant ran throttled alike; the per-run lines above show each run's share)
	measured_clk = [ r for r in runs if r["clocks"] and r["round"] != "w" ]
	for key, name in ( ( "thermal", "thermal slowdown (the card's temperature limit)" ), ( "power", "the power cap" ),
	                   ( "hw", "HW slowdown / power brake" ) ):
		hit = [ r for r in measured_clk if r["clocks"][key] > THROTTLE_REPORT_FRACTION ]
		if hit:
			lo = min( r["clocks"][key] for r in hit ) * 100
			hi = max( r["clocks"][key] for r in hit ) * 100
			temps = [ r["clocks"]["temp_max"] for r in hit if r["clocks"]["temp_max"] is not None ]
			tmax = f", up to {max( temps ):.0f} C" if temps and key == "thermal" else ""
			problems.append( f"{len( hit )} of {len( measured_clk )} measured runs limited by {name} in {lo:.0f}-{hi:.0f}% of their loaded "
			                 f"samples{tmax}; clocks {min( r['clocks']['mean'] for r in measured_clk ):.0f}-"
			                 f"{max( r['clocks']['mean'] for r in measured_clk ):.0f} MHz - compare variants that ran in the same phase" )
	display = sorted( { ( r["rep"] or {} ).get( "display", "?" ) for r in runs } )
	print( f"\ndisplay: {' | '.join( display )}" )
	gpus = sorted( { r["gpu"] for r in runs } )
	print( f"GPU: {' | '.join( gpus )}" )
	if len( gpus ) > 1:
		problems.append( f"the runs used different GPUs: {', '.join( gpus )}" )

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
			       f"   CPU {fmt( [ r['rep']['cpu_total'] for r in rs ] )} ms"
			       f" (frontend {fmt( [ r['rep']['cpu_fe'] for r in rs ] )} / backend {fmt( [ r['rep']['cpu_be'] for r in rs ] )})" )
			if ref_clock:
				print( f"     GPU at the median clock {ref_clock:.0f} MHz: {fmt( [ r.get( 'gpu_norm' ) for r in rs ] )} ms"
				       "   (each run's GPU time x its clock / the median - assumes time scales with the core clock)" )

		# scope table: every scope seen, in first-seen order of the first variant's runs
		scope_names = []
		for v in names:
			for r in measured:
				if r["variant"] == v:
					merge_order( scope_names, list( r["scopes"] ) )
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

		problems += paired_section( measured, names, scope_names )

	print()
	if problems:
		print( "CHECK:" )
		for p in problems:
			print( f"  ! {p}" )
	else:
		print( "checks: all runs real-time, variant cvars confirmed in every report, clocks within 5% of the median, no throttle reason "
		       f"in more than {THROTTLE_REPORT_FRACTION * 100:.0f}% of any measured run's loaded samples" )
	return 0


def _same_number( a, b ):
	try:
		return abs( float( a ) - float( b ) ) < 1e-6
	except ValueError:
		return False


if __name__ == "__main__":
	sys.exit( main() )
