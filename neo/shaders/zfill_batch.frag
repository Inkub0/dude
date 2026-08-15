// Phase 3.2b consume — Increment 2: fragment for the batched depth prepass. The solid
// opaque zfill writes color {0,0,0,1} (u_color for MC_OPAQUE non-subview) under
// GLS_DEPTHFUNC_LESS; replicate that constant so the batch is pixel-identical (the
// depth prepass writes black where opaque geometry is; lit passes overwrite it later).

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );
}
