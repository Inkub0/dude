// Translated from glprogs/interaction.vfp (vertex program).
// Per-light interaction pass: bump + diffuse + specular against one light.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_TexLightVec;   // texcoord[0]: light vector in tangent space
VARY(1) out vec2 var_TexBump;       // texcoord[1]
VARY(2) out vec2 var_TexFalloff;    // texcoord[2]: (falloff S, 0.5)
VARY(3) out vec4 var_TexProjection; // texcoord[3]: light projection (x, y, -, w)
VARY(4) out vec2 var_TexDiffuse;    // texcoord[4]
VARY(5) out vec2 var_TexSpecular;   // texcoord[5]
VARY(6) out vec3 var_TexHalfVec;    // texcoord[6]: half-angle vector in tangent space
VARY(7) out vec4 var_Color;
VARY(8) out vec3 var_TexViewVec;    // view vector in tangent space (PBR path only)
VARY(9) out vec3 var_ShadowCubeVec; // world-space light->frag vector (point-light cube shadow)
// model-space position + normal, consumed only by the tessellation stages
// (DUDE tessellation, docs/tessellation.md) for the PN control net. Unused by
// the fragment shader — a benign "output not consumed" in the flat pipeline.
VARY(10) out vec3 var_ModelPos;
VARY(11) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask
VARY(12) out vec4 var_ShadowProjection; // UNBAKED projection for the 2D shadow lookup

void main() {
	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );

	// vector to light in tangent space
	vec3 toLight = u_localLightOrigin.xyz - attr_Position.xyz;
	var_TexLightVec = vec3( dot( attr_Tangent, toLight ),
	                        dot( attr_Bitangent, toLight ),
	                        dot( attr_Normal, toLight ) );

	// surface texcoords through the material texture matrices
	var_TexBump     = vec2( dot( st, u_bumpMatrixS ),     dot( st, u_bumpMatrixT ) );
	var_TexDiffuse  = vec2( dot( st, u_diffuseMatrixS ),  dot( st, u_diffuseMatrixT ) );
	var_TexSpecular = vec2( dot( st, u_specularMatrixS ), dot( st, u_specularMatrixT ) );

	// light falloff: one texgen, t is the 0.5 default (ARB defaultTexCoord)
	var_TexFalloff = vec2( dot( attr_Position, u_lightFalloffS ), 0.5 );

	// light projection: three texgens, projective lookup by w
	var_TexProjection = vec4( dot( attr_Position, u_lightProjectionS ),
	                          dot( attr_Position, u_lightProjectionT ),
	                          0.0,
	                          dot( attr_Position, u_lightProjectionQ ) );

	// same projection but from the UNBAKED planes, for the 2D shadow-map lookup:
	// var_TexProjection carries the light stage's texture matrix (rotating fan gobo),
	// but the shadow depth map was rendered raw, so it must be sampled raw. Equal to
	// var_TexProjection for lights without a projection texture matrix.
	var_ShadowProjection = vec4( dot( attr_Position, u_shadowProjectionS ),
	                             dot( attr_Position, u_shadowProjectionT ),
	                             0.0,
	                             dot( attr_Position, u_shadowProjectionQ ) );

	// half-angle vector in tangent space (normalize both, add; length-free in fp)
	vec3 toView = normalize( u_localViewOrigin.xyz - attr_Position.xyz );
	vec3 halfV = normalize( toLight ) + toView;
	var_TexHalfVec = vec3( dot( attr_Tangent, halfV ),
	                       dot( attr_Bitangent, halfV ),
	                       dot( attr_Normal, halfV ) );

	// same view vector in tangent space, kept separate for the PBR N.V/Fresnel terms
	var_TexViewVec = vec3( dot( attr_Tangent, toView ),
	                       dot( attr_Bitangent, toView ),
	                       dot( attr_Normal, toView ) );

	// world-oriented light->fragment vector for point-light cube shadow lookups:
	// rotate the model-space (frag - light) vector by the model->world rotation.
	// Matches the caster's light-relative space (shadow_sm_cube.vert). Cheap enough
	// to always compute; only sampled when u_shadowParms.x selects the cube path.
	vec3 fragToLight = -toLight;	// attr_Position - localLightOrigin, model space
	var_ShadowCubeVec = vec3( dot( u_modelMatrixRow0.xyz, fragToLight ),
	                          dot( u_modelMatrixRow1.xyz, fragToLight ),
	                          dot( u_modelMatrixRow2.xyz, fragToLight ) );

	// 1.0, color, or 1.0 - color, selected by modulate/add
	var_Color = attr_Color * u_vertexColorModulate + u_vertexColorAdd;

	gl_Position = u_mvpMatrix * attr_Position;
}
