/*
 * SPDX-License-Identifier: MIT
 *
 * MHGU Recomp: the FidelityFX SDK 1.1.4 host sources built with MinGW-w64 clang (MSYS2 UCRT64). They were written
 * against MSVC, which pulls <new> and <cmath> in through its own headers and has swprintf_s; MinGW-w64 has the other
 * secure CRT functions they use (wcscpy_s, strcpy_s, sprintf_s). On other platforms ffx_1_1_4_portability.hpp
 * (the portable wrapper's own, unchanged) is used instead: see the Makefile.
 */

#pragma once

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <new>

using std::floor;
using std::log2;

#if !defined(_MSC_VER)
inline int ffx_mingw_swprintf_s(wchar_t* destination, std::size_t destinationSize, const wchar_t* format, ...)
{
    if (!destination || destinationSize == 0 || !format) {
        return -1;
    }
    va_list ap;
    va_start(ap, format);
    const int result = std::vswprintf(destination, destinationSize, format, ap);
    va_end(ap);
    if (result < 0) {
        destination[0] = L'\0';
    }
    return result;
}
#define swprintf_s ffx_mingw_swprintf_s
#endif

#include "ffx_error_site.hpp"
