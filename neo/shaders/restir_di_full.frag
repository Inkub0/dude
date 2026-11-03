// DUDE ReSTIR DI gather, the FULL-TARGET variant (restir_di.frag with DI_FULL_TARGET 1): loaded instead of restir_di while a
// cheap RIS target is off (r_rtReStirDICheapTarget 0 / r_rtEmissiveCheapTarget 0, the debug A/Bs) - the default program carries
// none of those paths (register count).
#define DI_FULL_TARGET 1
#include "restir_di.frag"
