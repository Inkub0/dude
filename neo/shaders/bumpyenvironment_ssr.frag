// bumpyEnvironment.frag + glass screen-space reflections (docs/ssr.md, glass
// extension). The base per-pixel cube reflection is computed exactly as
// bumpyenvironment.frag (including its vanilla quirk of ignoring the stage
// colour); where the march finds the reflected scene on screen it replaces the
// cubemap by the hit confidence.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_environmentCubeMap;
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;

#include "glass_ssr.glsl"

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec3 var_ToEyeGlobal;
VARY(2) in vec3 var_ToGlobalRow0;
VARY(3) in vec3 var_ToGlobalRow1;
VARY(4) in vec3 var_ToGlobalRow2;
VARY(5) in vec4 var_Color;
VARY(6) in vec3 var_ViewPos;
VARY(7) in vec3 var_ViewTangent;
VARY(8) in vec3 var_ViewBitangent;
VARY(9) in vec3 var_ViewNormal;

layout(location = 0) out vec4 fragColor;

void main() {
    vec4 bump = texture(u_bumpMap, var_TexCoord);
    bump.x = bump.a;
    vec3 localNormal = bump.xyz * 2.0 - 1.0;

    mat3 normalMatrix = mat3(var_ToGlobalRow0, var_ToGlobalRow1, var_ToGlobalRow2);
    vec3 globalNormal = normalize(normalMatrix * localNormal);

    vec3 globalEye = normalize(var_ToEyeGlobal);
    vec3 r = reflect(-globalEye, globalNormal);
    vec3 cube = texture(u_environmentCubeMap, r).xyz;

    // the same per-pixel bump normal, in view space for the screen-space march
    vec3 viewNormal = normalize(var_ViewTangent * localNormal.x
                              + var_ViewBitangent * localNormal.y
                              + var_ViewNormal * localNormal.z);
    vec4 ssr = GlassSsrMarch(var_ViewPos, viewNormal);

    fragColor = vec4(mix(cube, ssr.rgb, ssr.a), 1.0);
}
