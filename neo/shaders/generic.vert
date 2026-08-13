// New shader (was fixed function): GUI/2D drawing and plain old-style material
// stages. Texture matrix via the diffuse matrix slots; vertex color modes via
// modulate/add (1.0 / color / 1-color), then scaled by the stage color.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
// DUDE tessellation (docs/tessellation.md): the alpha channel carries the per-vertex
// UV-seam displacement mask idMD5Mesh stamps in (see tess.glsl). Also the ordinary
// vertex colour for 2D / vertex-lit stages — the two never collide because a stage
// that reads vertex colour isn't a tessellated body surface and vice-versa.
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_Color;
// model-space position + normal for the tessellation stages (DUDE tessellation,
// docs/tessellation.md); consumed only when this draw is tessellated. var_ModelNormal.w
// = the UV-seam mask and var_TexBump feeds Phase-2 displacement, so an on-body blend
// stage (e.g. the imp burning-corpse ember) displaces bit-identically to zfill.tese and
// survives depth-EQUAL. Unused by generic.frag / 2D draws.
VARY(2) out vec3 var_ModelPos;
VARY(3) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask
VARY(4) out vec2 var_TexBump;

void main() {
	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );

	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );

	var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;

	gl_Position = u_mvpMatrix * attr_Position;
}
