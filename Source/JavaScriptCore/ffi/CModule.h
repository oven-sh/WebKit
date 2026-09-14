/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#include "BIRModule.h"
#include "FFISignature.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <expected>
#include <wtf/Function.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

class Compilation;
class JSFFIFunction;
class JSGlobalObject;
class JSObject;

namespace FFI {

struct BIRLinkEnvironment;

// A C translation unit loaded into the process: the decoded BIR, its data segment, and the
// machine code B3 produced for every function. Immutable once created, so the DFG/FTL may read
// it from a compiler thread.
//
// Lifetime. A module that finished loading is never unloaded: its code, data and libraries stay for as
// long as the process lives, like a shared library the program was linked with. C hands out pointers
// into itself that no reference count sees (an atexit or signal handler, a thread's start routine, a
// callback given to a library, the address of a static object returned to JavaScript), and C code
// assumes they stay good. tryCreate keeps the reference that makes this so; the references its callers
// and the exported functions hold only matter for a module that failed to load, which is destroyed
// before any of its code has run.
class CModule final : public ThreadSafeRefCounted<CModule> {
    WTF_MAKE_TZONE_ALLOCATED(CModule);
    WTF_MAKE_NONCOPYABLE(CModule);
public:
    using ExternResolver = Function<void*(const CString& name)>;

    JS_EXPORT_PRIVATE static std::expected<Ref<CModule>, String> tryCreate(std::span<const uint8_t> bir, const ExternResolver&);
    JS_EXPORT_PRIVATE ~CModule();

    const BIR::Module& bir() const { return *m_bir; }
    void* entrypoint(unsigned functionIndex) const { return m_functionTable[functionIndex]; }
    void* const* functionTable() const { return m_functionTable.span().data(); }
    // This thread's copy of the module's `_Thread_local` objects, created on first use.
    JS_EXPORT_PRIVATE static void* SYSV_ABI threadLocalBase(void* module);
    // The (BIR::Arch, BIR::OS) a module has to have been compiled for to load in this process; nothing
    // where compiled C does not run.
    JS_EXPORT_PRIVATE static std::optional<std::pair<uint8_t, uint8_t>> hostTarget();
    BIRLinkEnvironment linkEnvironment() const;
    uint64_t relocatedAddress(const BIR::Reloc&, uint8_t* threadLocalBlock) const;

    // { exportName: JSFFIFunction }, with the JS-facing types the C declarations imply.
    JS_EXPORT_PRIVATE JSObject* createExportsObject(JSGlobalObject*);

private:
    CModule(std::unique_ptr<BIR::Module>&&);

    std::unique_ptr<BIR::Module> m_bir;
    uint8_t* m_data { nullptr };
    size_t m_dataAllocationSize { 0 };
    Vector<void*> m_functionTable;
    Vector<void*> m_externAddresses;
    Vector<std::unique_ptr<Compilation>> m_compilations;
    uint64_t m_threadLocalKey { 0 }; // Never reused, unlike `this`.
    Vector<void*> m_libraries; // dlopen handles for the BIR's `libraries`.
};

} // namespace FFI

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
