// DUDE parallax (docs/parallax.md "Pass parity"): the generic stage program for a material's ambient / emissive
// stages on a surface whose lit passes march its relief (RB_RHI_StageParallax picks it). World surfaces only, which
// never tessellate, so there are no tess stages.
#define DUDE_GENERIC_PARALLAX 1
#include "generic.vert"
