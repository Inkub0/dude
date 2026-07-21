// Uniform interface for TRANSPILED ARB shaders (ArbToGlsl). Unlike the
// hand-written shaders (renderparms.glsl), transpiled programs keep the raw
// ARB parameter model: the backend fills u_env/u_local exactly as the old
// qglProgramEnvParameter4fv/qglProgramLocalParameter4fv calls did, plus the
// fixed-function matrices that ARB programs read via state.matrix.*.
// A pipeline uses either RenderParams or ArbParams on binding 0, never both.

UBO_BINDING(0) uniform ArbParams {
	mat4 u_mvpMatrix;         // state.matrix.mvp / ARB_position_invariant
	mat4 u_modelViewMatrix;   // state.matrix.modelview
	mat4 u_projectionMatrix;  // state.matrix.projection
	mat4 u_textureMatrix;     // state.matrix.texture[0]
	vec4 u_env[32];           // program.env[N]
	vec4 u_local[8];          // program.local[N]
};
