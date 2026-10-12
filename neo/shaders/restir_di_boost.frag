// DUDE ReSTIR DI gather WITH the R3a disocclusion boost (docs/rtx-restir-di.md Plan R3): restir_di.frag compiled with
// DI_BOOST 1. A separate program because the boost's K-pick arrays cost the plain gather occupancy even where nothing is
// boosted; the DI pass loads this one only while r_rtReStirDIBoost > 1.
#define DI_BOOST 1
#include "restir_di.frag"
