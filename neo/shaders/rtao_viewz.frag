// DUDE RTAO viewZ guide pass (docs/rtx-rtao.md H4b): linearize _currentDepth into NRD's
// IN_VIEWZ — SIGNED view-space Z (negative in front of an RH camera, matching the
// worldToView/viewToClip matrices handed to the denoiser). R16F target at view res.
// The GTAO depth mip stores POSITIVE depth (its own convention), hence a separate pass.
//
//   u_depthTexRecip.xy = gl_FragCoord -> _currentDepth tc

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// same near / near-infinite far GL clip-depth -> linear eye-z constants ssao.frag uses
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): the game lowers the near plane
// for cinematic cameras, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	float raw = texture( u_currentDepth, gl_FragCoord.xy * u_depthTexRecip.xy ).x;
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative
	fragColor = vec4( vz, 0.0, 0.0, 1.0 );
}
