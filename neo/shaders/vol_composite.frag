// DUDE volumetric lighting (docs/volumetric-lighting.md): composite the marched volume onto the lit HDR
// scene. Blended ONE / SRC_ALPHA by the backend: scene * transmittance + in-scattered radiance.
//   unit 0 = the volumetric buffer (rgb in-scatter, a transmittance)
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_volume;

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = texture( u_volume, var_TexCoord );
}
