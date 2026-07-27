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
    // match the fixed-function/ARB path: the reflection is modulated by the stage
    // colour (u_color, e.g. glass "red Parm0 green Parm1 blue Parm2"), with the
    // vertex-colour mode applied first. SVC_IGNORE (the usual reflect case) zeroes
    // attr_Color and leaves u_color — without this, GL3 multiplied the cube by the
    // raw geometry vertex colour, so the dimming stage colour was lost and the
    // reflection washed the surface with a colour tint (docs/known-bugs.md glass tint).
    var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;
    gl_Position = u_mvpMatrix * attr_Position;
}