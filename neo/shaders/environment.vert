// Translated from glprogs/environment.vfp (vertex program).
// Per-pixel cube reflection, unbumped.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 2) in vec3 attr_Normal;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_Normal; // texcoord[0]
VARY(1) out vec3 var_ToEye;  // texcoord[1]
VARY(2) out vec4 var_Color;

void main() {
    var_Normal = attr_Normal;
    var_ToEye = u_localViewOrigin.xyz - attr_Position.xyz;
    var_Color = attr_Color;
    gl_Position = u_mvpMatrix * attr_Position;
}