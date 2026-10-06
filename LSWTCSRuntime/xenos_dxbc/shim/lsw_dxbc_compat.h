// Force-included into the vendored Xenia shader-translator TUs (MinGW/clang build).
#pragma once
#include "../../xenos/shim/lsw_xenia_compat.h"
#include <emmintrin.h>
#ifndef _mm_cvtsi64x_si128
#define _mm_cvtsi64x_si128 _mm_cvtsi64_si128   // MSVC-only alias
#endif
