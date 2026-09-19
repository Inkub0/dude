// DUDE decal surface mask: applies the decal retention buffer to the SSR rough/metal G-buffer
// once, so every reflection consumer (ssr, ssr_composite, ssr_rt, ssr_rt_composite) sees the
// same dirty surface the direct lighting does. Both inputs and the output are view-sized and
// were rasterized in the same framebuffer orientation, so a texel-for-texel fetch is exact on
// either backend - no uv convention to get wrong.

#include "renderparms.glsl"
#include "decalsurface.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_materialBuffer;  // x = roughness, y = metalness
SAMPLER_BINDING(1) uniform sampler2D u_decalMask;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	ivec2 p = ivec2( gl_FragCoord.xy );
	vec4 mt = texelFetch( u_materialBuffer, p, 0 );
	vec3 keep = texelFetch( u_decalMask, p, 0 ).rgb;
	dudeDecalSurface( keep, mt.x, mt.y );
	fragColor = mt;
}
