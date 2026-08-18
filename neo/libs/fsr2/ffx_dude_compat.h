// [dhewm-rt] MSVC-ism compatibility shims for building the vendored FidelityFX FSR2 sources
// on gcc/clang (Linux). FSR2 upstream ships/targets MSVC; these provide the handful of
// Microsoft-only helpers it uses. Included at the top of the FSR2 .cpp translation units
// (ffx_fsr2.cpp, vk/ffx_fsr2_vk.cpp). Not an upstream file. See docs/fsr-temporal-pipeline.md.
#pragma once

#include <cstddef>      // size_t
#include <cwchar>       // wcscmp / wchar_t functions
#include <cstdlib>      // wcstombs

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#if !defined(_MSC_VER)
// MSVC's bounds-checked wide-string copy (C11 Annex K). Minimal portable equivalent: copy
// src (NUL included) into dst when it fits, always NUL-terminate, 0 == success. FSR2 calls
// the MSVC template array-overload wcscpy_s(fixedArray, src) (2 args, deduces the size), so
// provide both that and the explicit (dst, count, src) form.
static inline int ffx_dude_wcscpy_s(wchar_t* dst, size_t destCount, const wchar_t* src) {
    if (!dst || !src || destCount == 0) return 22;              // EINVAL
    size_t i = 0;
    for (; src[i] != L'\0' && i + 1 < destCount; ++i) dst[i] = src[i];
    dst[i] = L'\0';
    return src[i] == L'\0' ? 0 : 34;                            // ERANGE if truncated
}
template<size_t N>
static inline int ffx_dude_wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    return ffx_dude_wcscpy_s(dst, N, src);
}
#define wcscpy_s ffx_dude_wcscpy_s

// MSVC's bounds-checked wide->multibyte convert (used only in FSR2's _DEBUG resource naming).
// retval receives bytes written including the NUL; portable equivalent via wcstombs.
static inline int ffx_dude_wcstombs_s(size_t* retval, char* dst, size_t dstsz, const wchar_t* src, size_t /*count*/) {
    size_t n = (dst && dstsz) ? wcstombs(dst, src, dstsz) : 0;
    if (n == (size_t)-1) { if (retval) *retval = 0; if (dst && dstsz) dst[0] = '\0'; return 22; }
    if (n >= dstsz) n = dstsz ? dstsz - 1 : 0;
    if (dst && dstsz) dst[n] = '\0';
    if (retval) *retval = n + 1;
    return 0;
}
#define wcstombs_s ffx_dude_wcstombs_s
#endif
