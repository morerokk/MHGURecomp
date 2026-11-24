/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 *
 * Narrow compatibility layer for AMD FidelityFX SDK 1.1.4 host sources.
 * The upstream implementation was written against MSVC's CRT even where the
 * effect scheduler itself is otherwise platform independent.
 */

#pragma once

#if !defined(_WIN32)

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int wcscpy_s(wchar_t* destination, std::size_t destinationSize, const wchar_t* source)
{
    if (!destination || destinationSize == 0) {
        return EINVAL;
    }
    if (!source) {
        destination[0] = L'\0';
        return EINVAL;
    }

    const std::size_t length = std::wcslen(source);
    if (length >= destinationSize) {
        destination[0] = L'\0';
        return ERANGE;
    }

    std::wmemcpy(destination, source, length + 1);
    return 0;
}

template <std::size_t Size>
inline int wcscpy_s(wchar_t (&destination)[Size], const wchar_t* source)
{
    return wcscpy_s(destination, Size, source);
}

inline int strcpy_s(char* destination, std::size_t destinationSize, const char* source)
{
    if (!destination || destinationSize == 0) {
        return EINVAL;
    }
    if (!source) {
        destination[0] = '\0';
        return EINVAL;
    }

    const std::size_t length = std::strlen(source);
    if (length >= destinationSize) {
        destination[0] = '\0';
        return ERANGE;
    }

    std::memcpy(destination, source, length + 1);
    return 0;
}

inline int ffxPortableUtf8ToWide(const char* source, wchar_t* destination, std::size_t destinationSize)
{
    static_assert(sizeof(wchar_t) >= sizeof(std::uint32_t),
                  "The FidelityFX 1.1.4 non-Windows port expects UTF-32 wchar_t");
    if (!source || !destination || destinationSize == 0) {
        return EINVAL;
    }

    destination[0] = L'\0';
    std::size_t output = 0;
    const auto* input = reinterpret_cast<const unsigned char*>(source);
    while (*input) {
        std::uint32_t codePoint = 0;
        std::size_t continuationCount = 0;
        if (*input < 0x80) {
            codePoint = *input++;
        } else if ((*input & 0xe0) == 0xc0) {
            codePoint = *input++ & 0x1f;
            continuationCount = 1;
        } else if ((*input & 0xf0) == 0xe0) {
            codePoint = *input++ & 0x0f;
            continuationCount = 2;
        } else if ((*input & 0xf8) == 0xf0) {
            codePoint = *input++ & 0x07;
            continuationCount = 3;
        } else {
            return EILSEQ;
        }

        for (std::size_t i = 0; i < continuationCount; ++i) {
            if ((input[i] & 0xc0) != 0x80) {
                destination[0] = L'\0';
                return EILSEQ;
            }
            codePoint = (codePoint << 6) | (input[i] & 0x3f);
        }
        input += continuationCount;
        const std::uint32_t minimum = continuationCount == 1 ? 0x80u :
                                      continuationCount == 2 ? 0x800u :
                                      continuationCount == 3 ? 0x10000u : 0u;
        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            destination[0] = L'\0';
            return EILSEQ;
        }
        if (output + 1 >= destinationSize) {
            destination[0] = L'\0';
            return ERANGE;
        }
        destination[output++] = static_cast<wchar_t>(codePoint);
    }
    destination[output] = L'\0';
    return 0;
}

inline int ffxPortableWideToUtf8(const wchar_t* source, char* destination, std::size_t destinationSize)
{
    static_assert(sizeof(wchar_t) >= sizeof(std::uint32_t),
                  "The FidelityFX 1.1.4 non-Windows port expects UTF-32 wchar_t");
    if (!source || !destination || destinationSize == 0) {
        return EINVAL;
    }

    destination[0] = '\0';
    std::size_t output = 0;
    for (std::size_t i = 0; source[i]; ++i) {
        const std::uint32_t codePoint = static_cast<std::uint32_t>(source[i]);
        if (codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return EILSEQ;
        }

        unsigned char encoded[4]{};
        std::size_t encodedSize = 0;
        if (codePoint < 0x80) {
            encoded[0] = static_cast<unsigned char>(codePoint);
            encodedSize = 1;
        } else if (codePoint < 0x800) {
            encoded[0] = static_cast<unsigned char>(0xc0 | (codePoint >> 6));
            encoded[1] = static_cast<unsigned char>(0x80 | (codePoint & 0x3f));
            encodedSize = 2;
        } else if (codePoint < 0x10000) {
            encoded[0] = static_cast<unsigned char>(0xe0 | (codePoint >> 12));
            encoded[1] = static_cast<unsigned char>(0x80 | ((codePoint >> 6) & 0x3f));
            encoded[2] = static_cast<unsigned char>(0x80 | (codePoint & 0x3f));
            encodedSize = 3;
        } else {
            encoded[0] = static_cast<unsigned char>(0xf0 | (codePoint >> 18));
            encoded[1] = static_cast<unsigned char>(0x80 | ((codePoint >> 12) & 0x3f));
            encoded[2] = static_cast<unsigned char>(0x80 | ((codePoint >> 6) & 0x3f));
            encoded[3] = static_cast<unsigned char>(0x80 | (codePoint & 0x3f));
            encodedSize = 4;
        }
        if (output + encodedSize >= destinationSize) {
            destination[0] = '\0';
            return ERANGE;
        }
        std::memcpy(destination + output, encoded, encodedSize);
        output += encodedSize;
    }
    destination[output] = '\0';
    return 0;
}

template <typename... Args>
inline int sprintf_s(char* destination, std::size_t destinationSize, const char* format, Args... args)
{
    if (!destination || destinationSize == 0 || !format) {
        return -1;
    }

    const int result = std::snprintf(destination, destinationSize, format, args...);
    if (result < 0 || static_cast<std::size_t>(result) >= destinationSize) {
        destination[0] = '\0';
        return -1;
    }
    return result;
}

using std::floor;
using std::log2;

#endif

#include "ffx_error_site.hpp"
