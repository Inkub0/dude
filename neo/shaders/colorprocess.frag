// Translated from glprogs/colorProcess.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec4 var_InvFraction;
VARY(1) in vec4 var_TargetScaled;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy * u_screenCorrection.xy;

    vec4 src = texture(u_currentRender, screenTc);

    // greyscale exactly as the ARB program: (r + g + b) * 0.33 — 0.33, not 1/3
    float grey = ( src.x + src.y + src.z ) * 0.33;

    // lerp between the source color and the grey-scaled target color:
    // src * (1 - fraction) + grey * (target * fraction). The ARB wrote only
    // result.color.xyz (alpha undefined); the stage's default replace blend
    // never reads alpha, so the source alpha is passed through.
    fragColor = vec4( src.rgb * var_InvFraction.rgb + grey * var_TargetScaled.rgb, src.a );
}
