// environment.vert + view-space position/normal varyings for the glass SSR
// march (docs/ssr.md, glass extension). The cube-reflection math is untouched;
// only used while r_ssr + r_ssrGlass have this view's scene snapshot ready.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 2) in vec3 attr_Normal;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_Normal; // texcoord[0]
VARY(1) out vec3 var_ToEye;  // texcoord[1]
VARY(2) out vec4 var_Color;
VARY(3) out vec3 var_ViewPos;    // view-space position for the march
VARY(4) out vec3 var_ViewNormal; // view-space normal for the march

void main() {
    var_Normal = attr_Normal;
    var_ToEye = u_localViewOrigin.xyz - attr_Position.xyz;
    // match the fixed-function/ARB path: the reflection is modulated by the stage
    // colour (u_color, e.g. glass "red Parm0 green Parm1 blue Parm2"), with the
    // vertex-colour mode applied first (see environment.vert for the history).
    var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;

    // model -> view for the screen-space march (rigid model matrices, so the
    // upper 3x3 rotates normals correctly)
    var_ViewPos = ( u_modelViewMatrix * attr_Position ).xyz;
    var_ViewNormal = mat3( u_modelViewMatrix ) * attr_Normal;

    gl_Position = u_mvpMatrix * attr_Position;
}
