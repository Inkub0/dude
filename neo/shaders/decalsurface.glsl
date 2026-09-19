// DUDE decal surface mask (RB_RHI_DecalMaskPass / decalmask.frag): what the surface under an
// overlay decal keeps of its own PBR response. Every channel is a retention, 1 = untouched,
// multiply-blended so overlapping decals compound:
//   r  metalness kept        dirt, blood and soot are dielectrics
//   g  smoothness kept       dirt scatters: roughness climbs to DUDE_DECAL_DIRT_ROUGH
// Shared by interaction.frag (direct light) and decalmat.frag (the SSR/RT rough-metal buffer)
// so highlights and reflections agree about the same stain.
//
// Wetness is NOT a property of the surface underneath (tried, three ways: a wet wall glints
// along the WALL's relief, and a levelled film under point lights shows nothing at all). Wet
// blood is lit as its own surface - see the film path in interaction.frag.

#define DUDE_DECAL_DIRT_ROUGH 0.85

void dudeDecalSurface( vec2 keep, inout float rough, inout float metal ) {
	metal *= keep.r;
	rough = mix( max( rough, DUDE_DECAL_DIRT_ROUGH ), rough, keep.g );
}
