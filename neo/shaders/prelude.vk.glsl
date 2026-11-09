// 460 (not 450): GL_EXT_ray_query declares its types only from GLSL 4.60 - under 450
// the extension enables but rayQueryEXT stays undeclared. 460 is a strict superset of
// 450 for everything this codebase writes; the SPIR-V target is vulkan1.4 either way.
#version 460

// Buffer references (GPU pointers) for manual vertex fetch — the Phase 3.2b BDA
// path (zfill_bda, docs/gpu-offload-plan.md §3.2b). Declared once here because
// compile_spv.py injects `invariant gl_Position;` ahead of each shader body, and
// an #extension must precede any statement. Inert for shaders that never name a
// buffer_reference type, so it changes no other shader's SPIR-V.
#extension GL_EXT_buffer_reference : require

// Vulkan prelude. Set 0 holds the per-draw uniform block, set 1 the
// combined image samplers, addressed by the same unit numbers the ARB
// programs used.
// (glslang's Vulkan mode predefines the VULKAN macro; don't redefine it here)
#define UBO_BINDING(b) layout(std140, set = 0, binding = b)
#define SAMPLER_BINDING(b) layout(set = 1, binding = b)
// SPIR-V requires explicit locations on varyings (matched by location)
#define VARY(loc) layout(location = loc)
// Transpiled-ARB fragment.position (ArbToGlsl emits RB_WPOS): Vulkan's gl_FragCoord is
// top-down, but the ARB programs (and the _currentRender capture they sample) assume GL's
// bottom-up window origin, so flip Y into GL convention: the flip height is the target's
// (u_fenv[31].y, a DUDE slot RB_RHI_RenderCustomStage fills for a view rendered into a crop,
// which lands on the target's bottom rows), else the view height 1/u_fenv[1].y (a full view).
// Only transpiled shaders expand this — hand-written shaders use gl_FragCoord directly and
// flip via u_windowCoord.
#define RB_WPOS vec4( gl_FragCoord.x, ( u_fenv[31].y > 0.0 ? u_fenv[31].y : 1.0 / u_fenv[1].y ) - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w )

// Ray-query arguments the spec allows (VUID-RuntimeSpirv-OpRayQueryInitializeKHR-06348..06351):
// origin and direction finite and not NaN, 0 <= tMin <= tMax. Anything else is undefined behaviour,
// which one driver shrugs off and another turns into a GPU hang (device lost: a frozen picture,
// GitHub #12 on Intel Arc). Every ray goes through dudeRayQueryInit, which hands an invalid ray
// to the hardware as a zero-length one: no traversal, a miss - what the callers already do when a
// ray leaves the scene. Valid rays pass through untouched. The comparisons are written so a NaN
// fails them (a NaN compares false), without isnan(), which a driver may fold away.
void dudeSanitizeRay( inout vec3 o, inout vec3 d, inout float tMin, inout float tMax ) {
	bool ok = all( lessThan( abs( o ), vec3( 1e30 ) ) ) && all( lessThan( abs( d ), vec3( 1e30 ) ) )
		&& dot( d, d ) > 1e-30 && tMin >= 0.0 && tMax >= tMin;
	if ( !ok ) {
		o = vec3( 0.0 );
		d = vec3( 0.0, 0.0, 1.0 );
		tMin = 0.0;
		tMax = 0.0;
	}
}
// same arguments as rayQueryInitializeEXT (needs GL_EXT_ray_query in the including shader)
#define dudeRayQueryInit( rq, as, flags, mask, o, tMin, d, tMax ) do { \
	vec3 dudeRayO_ = ( o ); vec3 dudeRayD_ = ( d ); float dudeRayTMin_ = ( tMin ); float dudeRayTMax_ = ( tMax ); \
	dudeSanitizeRay( dudeRayO_, dudeRayD_, dudeRayTMin_, dudeRayTMax_ ); \
	rayQueryInitializeEXT( rq, as, flags, mask, dudeRayO_, dudeRayTMin_, dudeRayD_, dudeRayTMax_ ); \
} while ( false )
