// Translated from glprogs/colorProcess.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec4 var_InvFraction;
VARY(1) in vec4 var_TargetScaled;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 screenTc = vec2(gl_FragCoord.x * u_windowCoord.x * u_screenCorrection.x, 
                         gl_FragCoord.y * u_windowCoord.y * u_screenCorrection.y);

    vec4 src = texture(u_currentRender, screenTc);

    // Dot product replaces (src.x + src.y + src.z) / 3.0
    float grey = dot(src.rgb, vec3(1/3));
    vec4 target = vec4(grey * var_TargetScaled.r, grey * var_TargetScaled.g, grey * var_TargetScaled.b, 1.0);

    // mix is cleaner and often optimized to a single FMA operation
    fragColor = mix(src * var_InvFraction, target, 1.0); // The lerp factor was implicitly 1.0 in the original (lerping against source with 0 weight)
}
