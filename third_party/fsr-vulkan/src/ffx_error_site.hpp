/*
 * SPDX-License-Identifier: MIT
 *
 * (MHGU Recomp) Every FFX_ERROR_BACKEND_API_ERROR an FFX source returns reports its file and line
 * (ffxVkPortableNoteError, ffx_vk_fsr3_portable.cpp): the Vulkan backend returns that one code from ~40 places,
 * which on its own says nothing about why a context could not be made. Included at the end of the portability
 * headers (the Makefile's -include for every FSR C++ source), after ffx_error.h's enum has its value, so later
 * uses get the macro; none of them is a constant expression (case label, initialiser).
 */

#pragma once

#include <FidelityFX/host/ffx_error.h>

extern "C" void ffxVkPortableNoteError(const char* file, int line);
extern "C" void ffxVkPortableLogf(const char* format, ...);  // (to ffxVkPortableSetLog's function, if any)

#undef FFX_ERROR_BACKEND_API_ERROR
#define FFX_ERROR_BACKEND_API_ERROR (ffxVkPortableNoteError(__FILE__, __LINE__), (FfxErrorCode)0x8000000d)
