

## G-buffer parallax (2026-10-21, branch feat/gbuffer-parallax)
PROBLEM (user): parallax had no effect on the Path Tracing tier (ReSTIR DI), emissive on or off. CAUSE: POM was only in
interaction.frag (the forward pass); the PT tier lights from the G-buffer (gbuffer.frag), which sampled bump/albedo/spec/
occlusion with unshifted UVs. FIX: the POM march now runs in gbuffer.frag too, shifting every surface UV before the
fetches, so DI / RTAO / RTGI / reflections all get the relief. gbuffer.vert builds the tangent-space view vector
(var_TexViewVec, VARY 11) from u_localViewOrigin; gbuffer.tesc/.tese pass it through; the CPU (RB_RHI_NormalPrepass)
sets localViewOrigin, fills parallaxParms via RB_RHI_FillParallaxParmsSurf (same opaque static-world policy as the
interaction helper) and binds the height map on unit 11. No self-shadow term in the G-buffer (a forward-lighting effect).
Gated by r_parallax; off = bit-identical. NOTE: grates are PERFORATED and excluded from POM by design (the march swims
across the holes) - their relief is the normal map, not parallax.
