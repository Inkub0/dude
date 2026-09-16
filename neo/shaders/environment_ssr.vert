// DUDE SSR glass reflections (docs/rtx-reflections.md RR5c-SSR) — vertex stage, unbumped.
// The screen-space counterpart of environment_rt.vert: instead of a cube texcoord (or a TLAS trace) it
// hands the fragment the glass VIEW-space position + normal, so the frag can reflect the view ray and
// MARCH the depth buffer (ssr.frag's recipe), reflecting the live on-screen room in place of the baked
// cube/probe. u_modelViewMatrix is this surface's model->view (surf->space->modelViewMatrix), set by the
// backend for the rtGlass/ssrGlass path. Stage colour folds into var_Color exactly as the cube path, so
// the reflection keeps the material's dimming tint.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 2) in vec3 attr_Normal;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_ViewPos;      // ray origin (view space)
VARY(1) out vec3 var_ViewNormal;   // reflect normal (view space)
VARY(2) out vec4 var_Color;        // stage colour (tint), SVC applied

void main() {
	var_ViewPos    = ( u_modelViewMatrix * attr_Position ).xyz;		// model -> eye/view space
	var_ViewNormal = mat3( u_modelViewMatrix ) * attr_Normal;		// rotate the normal into view space
	var_Color      = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;
	gl_Position    = u_mvpMatrix * attr_Position;
}
