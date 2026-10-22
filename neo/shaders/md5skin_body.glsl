// MD5 GPU skinning compute kernel — shared body for md5skin.comp and its PREVPOS
// variant md5skin_prevpos.comp. Build-time SPIR-V (compiled by compile_spv.py and
// embedded), so it needs NO runtime shaderc: it works on the mingw/Windows build and
// under Proton, where shaderc is absent and the old runtime-compiled string returned 0
// (which silently disabled GPU skinning -> skinned motion vectors dead -> animated
// characters smeared under the temporal passes). Do NOT add a #version line: the
// Vulkan prelude (prelude.vk.glsl) supplies #version 460, a strict superset of the 450
// this was authored against.
//
// One thread per OUTPUT vertex. It reads the joint palette and the per-vertex expanded
// weight run and, in a single loop, accumulates:
//  * position — the full affine jointMat*scaledWeight, mirroring idJointMat::operator*
//    exactly (row-major 3x4, base = joint*12 floats). Bit-exact vs CPU TransformVerts.
//    skinScale scales the offset (xyz) not the weight (w), matching TransformScaledVerts.
//  * TBN — linear-blend skinning: sum_w weight * (jointRot * jointLocalBindTBN), then
//    normalize. Option B (the LBS bind-TBN blend); the re-derive pass (md5derive.comp)
//    may overwrite it.
// The output SSBO is a raw idDrawVert float array (15 floats/vert); only xyz + normal +
// tangents are written, st (v[o+3..4]) and color (v[o+14]) stay as CreateBuffer seeded.
//
// PREVPOS variant: before overwriting the output vertex, it copies the buffer's CURRENT
// contents (= last frame's pose) into the PrevPos buffer, which the normal/velocity
// prepass reads to build a per-vertex skinned motion vector. When prevValid==0 (not
// skinned last frame) it stores the current pose -> zero animation velocity.
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Joints   { float jm[]; };
layout(std430, binding = 1) readonly buffer Weights  { vec4 sw[]; };
layout(std430, binding = 2) readonly buffer WIndex   { ivec2 wi[]; };
layout(std430, binding = 3) readonly buffer WStart   { uint wstart[]; };
layout(std430, binding = 4)          buffer OutVert  { float v[]; };
layout(std430, binding = 5) readonly buffer LocalTBN { vec4 lt[]; };
#ifdef PREVPOS
layout(std430, binding = 6)          buffer PrevPos  { vec4 pp[]; };	// last frame's model-space position per vert
#endif
layout(push_constant) uniform PC { uint numVerts; float skinScale; uint prevValid; } pc;
vec3 jrot( uint b, vec3 n ) {			// rotate n by the joint's 3x3 (rows), no translation
	return vec3( dot(vec3(jm[b+0u], jm[b+1u], jm[b+2u]),  n),
	             dot(vec3(jm[b+4u], jm[b+5u], jm[b+6u]),  n),
	             dot(vec3(jm[b+8u], jm[b+9u], jm[b+10u]), n) );
}
void main() {
	uint i = gl_GlobalInvocationID.x;
	if ( i >= pc.numVerts ) return;
	uint j = wstart[i];
	vec3 p = vec3(0.0), nrm = vec3(0.0), t0 = vec3(0.0), t1 = vec3(0.0);
	for ( int guard = 0; guard < 256; guard++ ) {
		int base = wi[j].x;
		vec4 w = sw[j];
		vec4 ws = vec4( w.xyz * pc.skinScale, w.w );
		p.x += dot( vec4(jm[base+0], jm[base+1], jm[base+2],  jm[base+3]),  ws );
		p.y += dot( vec4(jm[base+4], jm[base+5], jm[base+6],  jm[base+7]),  ws );
		p.z += dot( vec4(jm[base+8], jm[base+9], jm[base+10], jm[base+11]), ws );
		uint b = uint(base);
		nrm += w.w * jrot( b, lt[j*3u+0u].xyz );
		t0  += w.w * jrot( b, lt[j*3u+1u].xyz );
		t1  += w.w * jrot( b, lt[j*3u+2u].xyz );
		if ( wi[j].y == 1 ) break;
		j++;
	}
	nrm = normalize( nrm );  t0 = normalize( t0 );  t1 = normalize( t1 );
	uint o = i*15u;
#ifdef PREVPOS
	pp[i] = ( pc.prevValid != 0u ) ? vec4( v[o+0u], v[o+1u], v[o+2u], 1.0 ) : vec4( p, 1.0 );
#endif
	v[o+0u]=p.x;   v[o+1u]=p.y;   v[o+2u]=p.z;		// st = v[o+3..4] (seeded, untouched)
	v[o+5u]=nrm.x; v[o+6u]=nrm.y; v[o+7u]=nrm.z;
	v[o+8u]=t0.x;  v[o+9u]=t0.y;  v[o+10u]=t0.z;
	v[o+11u]=t1.x; v[o+12u]=t1.y; v[o+13u]=t1.z;	// color = v[o+14] (seeded, untouched)
}
