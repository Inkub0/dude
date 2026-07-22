// Translated from glprogs/heatHaze.vfp (vertex program).
// Screen-space refraction: scroll the distortion normal map, and scale the
// deform magnitude by projection distance.

#include "renderparms.glsl"
layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexDistort;  // texcoord[1]: scrolled normal map coords
VARY(1) out vec2 var_DeformMag;   // texcoord[2]

void main() {
    var_TexDistort = attr_TexCoord + u_localParam0.xy;

    vec4 viewPos = u_modelViewMatrix * vec4(1, 0, 0, 1);
    float projX = dot(viewPos.xy, vec2(u_projectionMatrix[0][1], u_projectionMatrix[1][1]));
    float projW = max(viewPos.w + 1e-6, 1.0);

    float deform = min(projX / projW, 0.02);
    var_DeformMag = vec2(deform * u_localParam1.x, deform * u_localParam1.y);

    gl_Position = u_mvpMatrix * attr_Position;
}
