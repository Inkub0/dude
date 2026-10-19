// DUDE RT emissive table debug overlay (docs/rtx-emissive-triangles.md E1d, r_rtEmissiveTableDebug): the table's
// triangles drawn as real world-space geometry (tr_main.cpp R_RtEmissiveDebugGeometry: one idDrawVert per table vertex -
// xyz world, st, normal = (flux, scale, triangle index), tangent = (glow slot, flags, 0), color = stage tint), through
// the view's world->clip matrix. The fragment stage depth-tests against the sealed scene depth itself.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec3 var_FluxScaleIndex;
VARY(2) flat out vec3 var_SlotFlags;
VARY(3) out vec4 var_Tint;

void main() {
	var_TexCoord       = attr_TexCoord;
	var_FluxScaleIndex = attr_Normal;
	var_SlotFlags      = attr_Tangent;
	var_Tint           = attr_Color;
	gl_Position = u_mvpMatrix * vec4( attr_Position.xyz, 1.0 );
}
