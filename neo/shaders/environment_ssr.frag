// environment.frag + glass screen-space reflections (docs/ssr.md, glass
// extension). The base cube reflection is computed exactly as environment.frag;
// where the march finds the reflected scene on screen, it replaces the cubemap
// by the hit confidence, and the stage colour modulates the result either way
// (so r_gl3ReflectionScale and material parm tints keep working).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_environmentCubeMap;

#include "glass_ssr.glsl"

VARY(0) in vec3 var_Normal;
VARY(1) in vec3 var_ToEye;
VARY(2) in vec4 var_Color;
VARY(3) in vec3 var_ViewPos;
VARY(4) in vec3 var_ViewNormal;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 normal = normalize(var_Normal);
    vec3 eye = normalize(var_ToEye);
    vec3 r = reflect(-eye, normal);
    vec4 cube = texture(u_environmentCubeMap, r);

    vec4 ssr = GlassSsrMarch(var_ViewPos, normalize(var_ViewNormal));
    fragColor = vec4(mix(cube.rgb, ssr.rgb, ssr.a), cube.a) * var_Color;
}
