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

#include "JSExportMacros.h"
#include <cstdint>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>

// The process-wide threads mode as C++ reads it: one byte on a page of its own.
//
// It is declared const here and defined, not const, in ThreadsModePage.cpp, which is the only file that writes it.
// It is written at most once, in JSC::initialize() right after the options are finalized
// (Config::latchGILOffProcess), which is before any VM exists and so before any reader below runs, and the page is
// made read-only when the Config page is. With the flag off it is never written: the byte is zero from load to exit.
//
// To the compiler a load of it is a load of constant memory, which is what the declaration is for: the value a
// function has seen or has been told (JSC_THREADS_MODE_BODY) holds for every later test in that function and in what
// is inlined into it, across calls and stores, and those tests fold.
//
// Config::gilOffProcess and the options stay what generated code, the interpreter and the code that runs before the
// latch read; the byte is derived from them once.
extern "C" JS_EXPORT_PRIVATE const uint8_t g_jscThreadsModePage[];

namespace JSC {

// The byte. Zero: the process runs without JS threads and without the shared collector ("flag off").
enum ThreadsModeBit : uint8_t {
    ThreadsModeJSThreads = 1 << 0, // Options::useJSThreads()
    ThreadsModeTaggedButterflies = 1 << 1, // Options::useTaggedButterflies()
    ThreadsModeGILOffProcess = 1 << 2, // Config::gilOffProcess
    ThreadsModeSharedGCHeap = 1 << 3, // Options::useSharedGCHeap(): forced by the JS threads flag, and on by itself in the shared-heap tests
};

ALWAYS_INLINE uint8_t threadsMode() { return g_jscThreadsModePage[0]; }
ALWAYS_INLINE bool processUsesJSThreads() { return threadsMode() & ThreadsModeJSThreads; }
ALWAYS_INLINE bool processUsesTaggedButterflies() { return threadsMode() & ThreadsModeTaggedButterflies; }
ALWAYS_INLINE bool processIsGILOff() { return threadsMode() & ThreadsModeGILOffProcess; }
ALWAYS_INLINE bool processUsesSharedGCHeap() { return threadsMode() & ThreadsModeSharedGCHeap; }

// Functions compiled once per threads mode.
//
// A function whose body tests the mode (itself or in what it inlines) can be written once and compiled twice, as a
// template on `bool threaded`:
//
//     template<bool threaded> R fooPerThreadsMode(A a, B b)
//     {
//         JSC_THREADS_MODE_BODY(threaded);
//         ... the body, unchanged ...
//     }
//     ALWAYS_INLINE R foo(A a, B b) { return JSC_CALL_PER_THREADS_MODE(fooPerThreadsMode, a, b); }
//
// fooPerThreadsMode<false> is what a process without JS threads runs: every test of the mode in it is folded away, so it
// is the code the function has without the threads work. fooPerThreadsMode<true> is what every other process runs: the
// tests stay, as they were. foo() tests the byte once; where foo() is called from a body that is itself compiled per
// mode that test folds too, and the call is a direct call of the copy of the same mode.
//
// JSC_THREADS_MODE_BODY(threaded) must be the first statement of such a body. It states the mode to the compiler and
// asserts it in builds with assertions.
#if COMPILER(CLANG)
#define JSC_THREADS_MODE_ASSUME(condition) __builtin_assume(condition)
#elif COMPILER(GCC)
#define JSC_THREADS_MODE_ASSUME(condition) do { if (!(condition)) __builtin_unreachable(); } while (false)
#elif COMPILER(MSVC)
#define JSC_THREADS_MODE_ASSUME(condition) __assume(condition)
#else
#define JSC_THREADS_MODE_ASSUME(condition) ((void)0)
#endif

// In a build for ThreadSanitizer nothing is compiled per mode: every test below picks the copy with threads, which states
// nothing and keeps every test of the mode. The sanitizer names a racing access by the functions inlined around it, and the
// suppression file matches those names; code that the two copies have in common is hoisted above the test that picks the
// copy and loses them (thirteenth round: the inline caches' relaxed loads in operationInByIdOptimize).
#if TSAN_ENABLED
#define JSC_THREADS_MODE_BODY(threaded) ((void)0)
#elif ASSERT_ENABLED
#define JSC_THREADS_MODE_BODY(threaded) do { \
        if (!!g_jscThreadsModePage[0] != (threaded)) [[unlikely]] \
            JSC::threadsModeBodyMismatch(); \
    } while (false)
#else
#define JSC_THREADS_MODE_BODY(threaded) do { \
        if constexpr (threaded) \
            JSC_THREADS_MODE_ASSUME(g_jscThreadsModePage[0] != 0); \
        else \
            JSC_THREADS_MODE_ASSUME(g_jscThreadsModePage[0] == 0); \
    } while (false)
#endif

// The test that picks the copy. It is weighted so that the copy without threads is the fall-through, and not so
// far that the threaded copy counts as cold: the inliner stops inlining into cold code.
#if TSAN_ENABLED
#define JSC_THREADS_MODE_IS_THREADED() true
#elif COMPILER(GCC_COMPATIBLE)
#define JSC_THREADS_MODE_IS_THREADED() __builtin_expect_with_probability(!!JSC::threadsMode(), 0, 0.75)
#else
#define JSC_THREADS_MODE_IS_THREADED() (!!JSC::threadsMode())
#endif

#define JSC_CALL_PER_THREADS_MODE(function, ...) \
    (JSC_THREADS_MODE_IS_THREADED() ? function<true>(__VA_ARGS__) : function<false>(__VA_ARGS__))

// An entry point whose two copies are functions of their own, for a caller that picks the copy once and keeps the
// pointer (the interpreter's slow path table): name##PerThreadsMode<false> and <true>. The function that has the
// entry's name tests the mode and goes to one of them.
#define JSC_PER_THREADS_MODE_FUNCTIONS(prefix, returnType, name, parameters, arguments) \
    template<bool> static ALWAYS_INLINE returnType name##Body parameters; \
    template<bool threaded> static NEVER_INLINE returnType name##PerThreadsMode parameters \
    { \
        JSC_THREADS_MODE_BODY(threaded); \
        return name##Body<threaded> arguments; \
    } \
    prefix returnType name parameters \
    { \
        if (JSC_THREADS_MODE_IS_THREADED()) \
            return name##PerThreadsMode<true> arguments; \
        return name##PerThreadsMode<false> arguments; \
    } \
    template<bool> ALWAYS_INLINE returnType name##Body parameters

// Any function compiled once per mode, in place: the body becomes a lambda templated on the mode that is inlined twice
// into the function, which keeps its name, its linkage and its frame.
//     R Class::foo(A a)
//     {
//         JSC_PER_THREADS_MODE_BEGIN(R)
//         ... the body, unchanged ...
//         JSC_PER_THREADS_MODE_END
//     }
// The lambda is inlined in every build, a build without optimization included (ALWAYS_INLINE_LAMBDA only forces it with
// NDEBUG): the body reads the frame and the return address of the function it is the body of (DECLARE_CALL_FRAME).
#if COMPILER(GCC_COMPATIBLE)
#define JSC_PER_THREADS_MODE_INLINE_LAMBDA __attribute__((__always_inline__))
#elif COMPILER(MSVC)
#define JSC_PER_THREADS_MODE_INLINE_LAMBDA [[msvc::forceinline]]
#else
#define JSC_PER_THREADS_MODE_INLINE_LAMBDA
#endif

#define JSC_PER_THREADS_MODE_BEGIN(...) \
    auto jscPerThreadsModeBody = [&]<bool threaded>() JSC_PER_THREADS_MODE_INLINE_LAMBDA -> __VA_ARGS__ { \
        JSC_THREADS_MODE_BODY(threaded);

#define JSC_PER_THREADS_MODE_END \
    }; \
    if (JSC_THREADS_MODE_IS_THREADED()) \
        return jscPerThreadsModeBody.template operator()<true>(); \
    return jscPerThreadsModeBody.template operator()<false>();

// The same for a constructor or a destructor, which cannot return an expression.
#define JSC_PER_THREADS_MODE_END_WITHOUT_RETURN \
    }; \
    if (JSC_THREADS_MODE_IS_THREADED()) \
        jscPerThreadsModeBody.template operator()<true>(); \
    else \
        jscPerThreadsModeBody.template operator()<false>();

JS_EXPORT_PRIVATE NO_RETURN_DUE_TO_CRASH void threadsModeBodyMismatch();

// ThreadsModePage.cpp
JS_EXPORT_PRIVATE void latchThreadsModePage(uint8_t mode);
JS_EXPORT_PRIVATE void freezeThreadsModePage();

} // namespace JSC
