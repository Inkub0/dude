// Translated from glprogs/heatHaze.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender; // _currentRender
SAMPLER_BINDING(1) uniform sampler2D u_normalMap;     // distortion normal map

VARY(0) in vec2 var_TexDistort;
VARY(1) in vec2 var_DeformMag;

layout(location = 0) out vec4 fragColor;

void main() {
    // Texture fetch from the bump/normal sampler using interpolated distortion coordinates
    vec4 bump = texture(u_normalMap, var_TexDistort);

    // Map the normal to [-1..+1] range (unpacking swizzled alpha channel)
    vec2 localNormal = bump.xy * 2.0 - 1.0;

    // Use interpolated texcoord as base instead of gl_FragCoord*u_windowCoord
    vec2 screenTc = clamp(localNormal * var_DeformMag + var_TexDistort, 0.0, 1.0);

    // Apply correction factor (for non-power-of-two and alignment)
    screenTc *= u_screenCorrection.xy;

    fragColor = vec4(texture(u_currentRender, screenTc).xyz, 1.0);
}
