# shaderstats: offline shader cost and equivalence tools

These tools measure what a shader edit costs, and check that it still computes the same thing, without launching the game.
They compile the engine's shaders the way the build does, then hand them to **Mesa RADV**, the Vulkan driver of an AMD GPU.
On this machine that is the CPU's integrated GPU. Nothing here renders a frame; only `probe.py --nvidia` touches the NVIDIA
card, and only to compile pipelines for its register counts.

They answer two questions before a benchmark is worth running:

- Did the change cut instructions or registers?
- Does it still produce the same numbers, to rounding?

The benchmark (`tools/bench/timedemo-ab.sh`) still decides whether a change is faster on the real card.

## Setup

Requirements:

- Mesa RADV (`/usr/share/vulkan/icd.d/radeon_icd.json`);
- `glslangValidator`;
- gcc with the Vulkan headers and loader;
- python3, plus numpy for `em_test.py`.

Build the two helpers into `bin/`:

```sh
tools/shaderstats/build.sh
```

`bin/` and `out/` (test data) are git-ignored.

## Tools

| Tool | What it does |
|---|---|
| `check.py [DIR]` | Compiles every engine shader with the prelude and resolved includes, as a quick check after an edit. It doesn't build the engine. |
| `probe.py [--tree DIR] [--nort] [--peaks N] [program ...]` | Prints per-program statistics: VGPRs, waves per SIMD, instructions, VALU, VMEM, ACO's static latency estimate, scratch. Defaults to the ReSTIR DI, GI and light-grid programs. |
| `micro.py [label=DIR ...]` | Prints the cost of single `rt_emissive.glsl` functions, each in a minimal compute kernel, one column per shader tree. Add entries to `BODY` to measure other functions. |
| `em_test.py OLD_DIR NEW_DIR` | Equivalence test of `rtEmissiveTargetRow` between two trees, run on 400 random emitter rows and 200,000 receivers. It packs each tree's rows in that tree's own row layout, so a row layout change can be tested too. |
| `bin/vkstats` | Statistics for one pipeline: `vkstats vert.spv frag.spv [isa.txt]` or `vkstats x.comp.spv [isa.txt]`. The optional file receives the ISA. |
| `bin/vkrun` | Compute runner for equivalence harnesses. One device-address buffer, push constants `{ uvec2 base; uint n; uint mode; }`. Its usage is in the header of `vkrun.c`, including timing with `VKRUN_REPEAT`. |

To compare against another revision, extract its shader tree and pass both trees:

```sh
mkdir -p /tmp/master && git archive master neo/shaders | tar -x -C /tmp/master
tools/shaderstats/micro.py master=/tmp/master/neo/shaders now=neo/shaders
tools/shaderstats/probe.py --tree /tmp/master/neo/shaders restir_di
```

## Reading the numbers

**These are AMD numbers, used as a proxy.**

- Instruction and VALU cuts usually carry over to NVIDIA. Register counts don't carry over exactly.
- For the NVIDIA card's own figures, run `probe.py --nvidia [program ...]`. It compiles the pipelines with the NVIDIA driver
  and prints its register count and local memory (spills). The numbers match the in-game `r_vkPipelineStats restir_di`
  readout. Don't run it during a benchmark.

**Check that a variant's own runs agree before reading an A/B.**

- On 2026-11-03 a precomputed emitter-triangle normal seemed to make the DI gather 14% slower on the 3080 Ti, with NVIDIA's
  registers unchanged. The opposite change, one load fewer per triangle (16-bit edges, 2026-11-04), was exactly neutral.
- The per-window data explained it: two of the three runs of the "slower" binary matched master for ~20 s, then stepped up
  by 20% and stayed there; the third matched master all the way. A run changed mid-way (likely a buffer recreated during
  play landing in slower memory - Resizable BAR is off on this PC), not the code.
- The bench report now flags a variant whose own runs disagree far more than the other variant's do. Neither change
  measurably moved the triangle walk: there it is neither the loads nor the maths that limit it.

**`--nort` stubs every ray query.**

- RADV traverses rays in software, using registers and scratch that GPUs with hardware traversal don't need.
- With the stubs, the counts come from the shading code alone. This is the better proxy for register pressure on NVIDIA.

**`--peaks N` shows which code sets the register count.**

- GLSL inlines every call, so a program's register count is the maximum over all its paths, including option paths the current settings never run.
- `--peaks` lists the basic blocks with the highest VGPR demand (`ACO_DEBUG=liveinfo`) and their most common instructions.
- If the peak is on a non-default path, the usual fix is a compile-time variant that the CPU selects. The `DI_FULL_TARGET` programs are an example.

**The compiler undoes plain recomputation.**

- Common-subexpression elimination merges a value you recompute to save a register back into one long-lived value.
- To keep the recomputation, route it through an opaque zero (`diOpaqueZero` in `restir_di.frag`).

## Lessons

**Test with geometry like the game's.**

- Test samplers and solid-angle maths with long, thin triangles and receivers within one or two triangle sizes.
- Report the 99.9th percentile and the maximum, not only the median or standard deviation; the bad cases sit there.

**The bench needs a separate shader directory per build.**

- Engine binaries load SPIR-V from the compiled-in `DUDE_SHADER_SPV_DIR` before their embedded copy.
- When two bench binaries share one build directory, both run the last build's shaders.
- Give each reference build its own directory, then check that each binary contains its expected `restir_di.frag.spv`.

**Power caps distort clock-normalised results.**

- A change that raises occupancy draws more power per MHz.
- On a power-capped GPU, its runs settle at a lower clock, so clock-normalised results over-credit it.
- Lock or cap the GPU clock (for example with a LACT profile), and check `clocks.csv` for throttle reasons before trusting a difference.

**Static estimates are not frame time.**

- ACO's latency is a static, per-instruction estimate.
- Several register- and instruction-neutral rewrites benchmarked slower: the grid-fill top-N rewrites, for example.
