/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include <wtf/Platform.h>

// What kind of system this is, as CPython's configure script puts it.

// sys.platform
#if OS(DARWIN)
#define PYTHON_PLATFORM "darwin"
#elif OS(WINDOWS)
#define PYTHON_PLATFORM "win32"
#else
#define PYTHON_PLATFORM "linux"
#endif

// sys.implementation._multiarch, which there is none of on Windows: MULTIARCH
#if OS(DARWIN)
#define PYTHON_MULTIARCH "darwin"
#elif OS(LINUX)
#if CPU(X86_64)
#define PYTHON_MULTIARCH_CPU "x86_64"
#elif CPU(ARM64)
#define PYTHON_MULTIARCH_CPU "aarch64"
#endif
#if defined(PYTHON_MULTIARCH_CPU) && defined(__GLIBC__)
#define PYTHON_MULTIARCH PYTHON_MULTIARCH_CPU "-linux-gnu"
#elif defined(PYTHON_MULTIARCH_CPU)
#define PYTHON_MULTIARCH PYTHON_MULTIARCH_CPU "-linux-musl"
#endif
#endif
