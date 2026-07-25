/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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

#include "FFIType.h"
#include "JSCPtrTag.h"
#include "JSExportMacros.h"
#include "MacroAssemblerCodeRef.h"
#include <atomic>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/Forward.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PlatformCallingConventions.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace FFI {

// The invoke-thunk entry point used by every tier (host path, IC stub,
// DFG/FTL): JSC operation calling convention (SYSV_ABI on Windows x64).
//     void thunk(void* target, uint64_t* slots)
using InvokeThunkFunction = void (JIT_OPERATION_ATTRIBUTES *)(void* target, uint64_t* slots);

// The canonical slot buffer (spec section 4): every FFI call, in either
// direction, marshals through `uint64_t slots[signature.slotCount()]`.
// slots[i] for i < argumentCount() is native argument i (declaration
// order); slots[argumentCount()] is the return slot. Each slot is 8 bytes
// and the buffer is 8-byte aligned.
constexpr size_t slotSize = 8;

// Byte offset of argument slot `index` within the slot buffer.
constexpr size_t argumentSlotOffset(unsigned index) { return static_cast<size_t>(index) * slotSize; }

// An interned, immutable native function signature: an ordered list of
// argument types plus a return type. Structurally equal signatures are the
// same Signature* (hash-consed by SignatureRegistry) and signatures are
// immortal, so a Signature* may be compared for identity and cached freely.
// This class has NO JSValue-facing API; FFI::signatureFromJS (BunFFI.h)
// builds one from a JS descriptor.
//
// The out-of-line entry points are JS_EXPORT_PRIVATE: the testFFI
// executable (SPEC section 11.3) links libJavaScriptCore (built with hidden
// default visibility, like testmasm) and constructs Signatures directly for
// its golden-layout / interning / invoke-thunk-differential tests, alongside
// the exported entry points of FFICallingConvention.h and FFIInvokeThunk.h.
class Signature final : public ThreadSafeRefCounted<Signature> {
    WTF_MAKE_TZONE_ALLOCATED(Signature);
    WTF_MAKE_NONCOPYABLE(Signature);
public:
    static constexpr unsigned maxArguments = 32;

    // Interning entry point. Validates that every argument type
    // isValidArgumentType, the return type isValidReturnType, and
    // arguments.size() <= maxArguments; returns nullptr on validation
    // failure (the caller throws a TypeError). On success returns the
    // canonical interned Signature for this (arguments, returnType) shape.
    JS_EXPORT_PRIVATE static RefPtr<Signature> tryCreate(std::span<const Type> arguments, Type returnType);

    // Parameter count: every native parameter is a JS-caller-supplied
    // argument, so this is also what JSFFIFunction's `length` and its arity
    // check use.
    unsigned argumentCount() const { return static_cast<unsigned>(m_arguments.size()); }
    Type argumentType(unsigned index) const { return m_arguments[index]; }
    std::span<const Type> arguments() const LIFETIME_BOUND { return m_arguments.span(); }
    Type returnType() const { return m_returnType; }

    // Slot buffer layout (spec section 4).
    unsigned slotCount() const { return argumentCount() + 1; }
    // FFI-SPEC-GAP: the spec writes `static size_t Signature::slotBufferBytes()`
    // but defines it in terms of the per-instance slotCount(); it is an
    // instance method (used by the callback thunk and the FTL lowering as
    // signature.slotBufferBytes()).
    size_t slotBufferBytes() const { return static_cast<size_t>(slotCount()) * slotSize; }
    // Byte offset of the return slot (== argumentSlotOffset(argumentCount())).
    size_t returnSlotOffset() const { return static_cast<size_t>(argumentCount()) * slotSize; }

    unsigned hash() const { return m_hash; }
    // Canonical rendering, e.g. "f64(i32,f64)": type names from FFI::name(),
    // no spaces. Used for thunk/stub names and $vm.ffiSignatureString.
    JS_EXPORT_PRIVATE String toString() const;

    // Structural equality: same argument types in order and same return
    // type. Interned signatures with structural equality are the same object.
    JS_EXPORT_PRIVATE bool operator==(const Signature&) const;

    JS_EXPORT_PRIVATE static bool structurallyEquals(std::span<const Type> arguments, Type returnType, const Signature&);
    JS_EXPORT_PRIVATE static unsigned computeHash(std::span<const Type> arguments, Type returnType);

    // The lazily generated, signature-pure invoke thunk (spec section 7),
    // shared process-wide. Generated on first call by
    // FFI::generateInvokeThunk (FFIInvokeThunk.h) under an internal lock; the
    // Signature only caches the resulting code ref. Returns a null CodePtr on
    // executable-memory allocation failure or when the JIT is unavailable.
    // This is the ONLY per-signature JIT artifact stored here; the IC stub is
    // per-JSFFIFunction.
    JS_EXPORT_PRIVATE CodePtr<JITThunkPtrTag> invokeThunk();

private:
    friend class SignatureRegistry;
    Signature(std::span<const Type> arguments, Type returnType);

    FixedVector<Type> m_arguments;
    Type m_returnType;
    unsigned m_hash { 0 };

    Lock m_codeLock;
    MacroAssemblerCodeRef<JITThunkPtrTag> m_invokeThunkCode WTF_GUARDED_BY_LOCK(m_codeLock);
    // Lock-free published copy of m_invokeThunkCode's code pointer for the per-call fast path
    // in invokeThunk(); written once (release) under m_codeLock after successful generation.
    std::atomic<void*> m_publishedInvokeThunk { nullptr };
};

// Hashes/compares registry entries by the referenced signature's structure
// (not by pointer identity), so a structurally equal lookup finds the
// existing canonical entry (hash-consing; mirrors
// Wasm::CanonicalSingletonEntryHash). equal() dereferences its arguments,
// so it must never be applied to the empty/deleted bucket values.
struct SignatureHash {
    static unsigned hash(const Ref<Signature>& signature) { return signature->hash(); }
    static bool equal(const Ref<Signature>& a, const Ref<Signature>& b) { return a.ptr() == b.ptr() || Signature::structurallyEquals(a->arguments(), a->returnType(), b.get()); }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

// Process-global signature interning table (mirrors
// Wasm::TypeInformation). Signatures are immortal: entries are never
// removed, so a Signature* obtained from intern() stays valid forever.
class SignatureRegistry {
    WTF_MAKE_TZONE_ALLOCATED(SignatureRegistry);
    WTF_MAKE_NONCOPYABLE(SignatureRegistry);
public:
    SignatureRegistry() = default;

    JS_EXPORT_PRIVATE static SignatureRegistry& singleton();

    // Returns the canonical Signature for this shape, creating and registering
    // it on first request. Does NOT validate (Signature::tryCreate does).
    JS_EXPORT_PRIVATE Ref<Signature> intern(std::span<const Type> arguments, Type returnType);

    // Number of interned signatures. Used by tests.
    JS_EXPORT_PRIVATE size_t size();

private:
    Lock m_lock;
    UncheckedKeyHashSet<Ref<Signature>, SignatureHash> m_signatures WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
