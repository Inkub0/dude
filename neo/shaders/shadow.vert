// Translated from glprogs/shadow.vp.
// Stencil shadow volume projection: verts with w=1 stay put, verts with
// w=0 project away from the light to infinity.
// NOTE: attr_Position must be fed the 4-component shadowCache_t position.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

void main() {
	// R0 = pos - light (assumes light.w == 0), then R0 += pos.w * light:
	// w=1 verts -> original position, w=0 verts -> direction from light
	vec4 projected = attr_Position - u_localLightOrigin;
	projected = attr_Position.w * u_localLightOrigin + projected;

	gl_Position = u_mvpMatrix * projected;
}
