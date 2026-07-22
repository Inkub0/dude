// Diffuse cube (TG_DIFFUSE_CUBE): sample a cube map by the object-space vertex
// normal — matching the fixed-function path, which fed the vertex normal as the
// texcoord.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 2) in vec3 attr_Normal;

VARY(0) out vec3 var_TexCoord;

void main() {
	var_TexCoord = attr_Normal;
	gl_Position = u_mvpMatrix * attr_Position;
}
