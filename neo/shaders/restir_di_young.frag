// DUDE ReSTIR DI R3a - the YOUNG pass (docs/rtx-restir-di.md Plan R3): per pixel of the gather layout (left-packed under
// checkerboard), the reason this pixel counts as young - 0 = not, 1 = NRD's history short at the previous position,
// 2 = revealed from behind an occluder, 3 = held (its age since it became young) / entered the screen - written as
// ( reason / 4, age / 64 ) into a ping-pong target (unit 11 = LAST frame's). The boost pass (restir_di_boost.frag) reads it back CLUSTERED (3x3 majority), so isolated
// specks never trigger the K-pick path. Shares the gather's uniform packing + units 0-3 and the boost inputs 4/5/6/9/10.
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "rt_light_tex.glsl"
#include "rt_perforated.glsl"
#include "restir_di_common.glsl"
#include "restir_di_shade.glsl"
#include "restir_di_boost.glsl"

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

void main() {
	vec2  frag = gl_FragCoord.xy;
	vec2  sfrag  = frag;
	int   cbMode = int( u_localParam1.z + 0.5 );
	if ( cbMode > 0 ) {
		float gx = floor( frag.x ), gy = floor( frag.y );
		float parity = mod( gy + float( cbMode - 1 ), 2.0 );
		sfrag = vec2( gx * 2.0 + parity + 0.5, gy + 0.5 );
	}
	fragColor = vec4( 0.0 );
	if ( cbMode > 0 && frag.x * u_screenCorrection.x >= 0.5 ) { return; }
	const bool applyMode = u_localParam1.w > 0.5;
	const bool pbr       = u_localParam1.y > 0.5;
	DiSurface S; float raw; vec4 nt;
	if ( !diSurfaceAt( sfrag, applyMode, pbr, S, raw, nt ) ) { return; }		// sky / no geometry
	if ( nt.a < 0.5 ) { return; }												// the view weapon: never boosted
	float age;
	int   why = diBoostWanted( sfrag, S, age );
	fragColor = vec4( float( why ) * 0.25, age / 64.0, 0.0, 1.0 );		// ( reason / 4, age / 64 )
}
