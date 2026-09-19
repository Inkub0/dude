// DUDE decal surface mask (RB_RHI_DecalMaskPass / decalmask.frag): what the surface under an
// overlay decal keeps of its own PBR response. Every channel is a retention, 1 = untouched,
// multiply-blended so overlapping decals compound:
//   r  metalness kept        dirt, blood and soot are dielectrics
//   g  smoothness kept       dirt scatters: roughness climbs to DUDE_DECAL_DIRT_ROUGH
//   b  dryness kept          wet blood films over: roughness drops to DUDE_DECAL_WET_ROUGH
// Shared by interaction.frag (direct light) and decalmat.frag (the SSR/RT rough-metal buffer)
// so highlights and reflections agree about the same stain.

#define DUDE_DECAL_DIRT_ROUGH 0.85
#define DUDE_DECAL_WET_ROUGH  0.12

void dudeDecalSurface( vec3 keep, inout float rough, inout float metal ) {
	metal *= keep.r;
	rough = mix( max( rough, DUDE_DECAL_DIRT_ROUGH ), rough, keep.g );
	rough = mix( min( rough, DUDE_DECAL_WET_ROUGH ), rough, keep.b );
}
