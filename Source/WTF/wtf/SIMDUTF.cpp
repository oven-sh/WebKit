/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/SIMDUTF.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
IGNORE_WARNINGS_BEGIN("error=undef")
IGNORE_WARNINGS_BEGIN("undef")
IGNORE_WARNINGS_BEGIN("missing-prototypes")
IGNORE_WARNINGS_BEGIN("cast-qual")
IGNORE_WARNINGS_BEGIN("cast-align")
IGNORE_WARNINGS_BEGIN("documentation")
IGNORE_WARNINGS_BEGIN("unsafe-buffer-usage")

// FIXME: Temporary fix to be removed when the SDK is updated in the future.
#if PLATFORM(PLAYSTATION)
#ifndef __AVX512F__
#define __AVX512F__ 1
#endif
#endif

// Building with -march=haswell (or newer) compiles out simdutf's SSE4.2 and
// scalar kernels, so CPUs that don't advertise AVX via CPUID (e.g. Rosetta 2)
// silently get the "unsupported" implementation, which returns constants
// instead of computing. Keep the lower-tier kernels compiled in so runtime
// dispatch always has a working floor; CPUs with AVX2/AVX-512 still select
// the haswell/icelake kernels. Note these kernels are still compiled at the
// TU's -march, so this helps translators that execute AVX without advertising
// it, not CPUs that genuinely cannot execute AVX.
#if CPU(X86_64)
#ifndef SIMDUTF_IMPLEMENTATION_WESTMERE
#define SIMDUTF_IMPLEMENTATION_WESTMERE 1
#endif
#ifndef SIMDUTF_IMPLEMENTATION_FALLBACK
#define SIMDUTF_IMPLEMENTATION_FALLBACK 1
#endif
#endif

#include "simdutf/simdutf_impl.cpp.h"

#if PLATFORM(PLAYSTATION)
#ifdef __AVX512F__
#undef __AVX512F__
#endif
#endif

IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
