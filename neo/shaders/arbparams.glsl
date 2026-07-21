// Uniform interface for TRANSPILED ARB shaders (ArbToGlsl). Unlike the
// hand-written shaders (renderparms.glsl), transpiled programs keep the raw
// ARB parameter model: the backend fills env/local exactly as the old
// qglProgramEnvParameter4fv/qglProgramLocalParameter4fv calls did, plus the
// fixed-function matrices that ARB programs read via state.matrix.*.
//
// env/local spaces are PER TARGET in ARB (vertex env[1] = global view origin
// while fragment env[1] = window coord, per RB_SetProgramEnvironment), so the
// block carries separate arrays per stage.
// A pipeline uses either RenderParams or ArbParams on binding 0, never both.

UBO_BINDING(0) uniform ArbParams {
	mat4 u_mvpMatrix;         // state.matrix.mvp / ARB_position_invariant
	mat4 u_modelViewMatrix;   // state.matrix.modelview
	mat4 u_projectionMatrix;  // state.matrix.projection
	mat4 u_textureMatrix;     // state.matrix.texture[0]
	vec4 u_venv[32];          // vertex   program.env[N]
	vec4 u_fenv[32];          // fragment program.env[N]
	vec4 u_vlocal[8];         // vertex   program.local[N]
	vec4 u_flocal[8];         // fragment program.local[N]
};
