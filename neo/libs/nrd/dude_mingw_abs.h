// DUDE: force-included into the vendored NRD sources when cross-building with MinGW.
//
// NRD's shared C++/HLSL math headers call the HLSL-style intrinsic `abs( x )` unqualified on
// floats. With glibc the C++ float overloads of abs are visible in the global namespace, so that
// resolves to abs(float). With MinGW's headers only C's `int abs(int)` is global in these
// translation units: `abs( float )` silently TRUNCATES to int (wrong maths everywhere it still
// compiles) and, where the int result feeds an overloaded function, fails to build
// ("call of overloaded 'saturate(int)' is ambiguous", MathLib/ml.hlsli). Making the std
// overloads visible globally - exactly what libstdc++'s own <stdlib.h> wrapper does - restores
// the intended float call on both counts, without touching the vendored files.
#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cstdlib>
using std::abs;
#endif
