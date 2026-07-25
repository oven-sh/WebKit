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

#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

#include "FFISignature.h"

#include "Options.h"
#include <mutex>
#include <wtf/DataLog.h>
#include <wtf/Locker.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RawPointer.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>

// The invoke-thunk generator is JIT-only, 64-bit-only, and compiled out on
// ENABLE(JIT_OPERATION_VALIDATION) builds (spec section 14); on any other
// configuration Signature::invokeThunk() returns a null CodePtr and the
// JSFFIFunction/JSFFICallback creation entry points throw instead.
#if ENABLE(JIT) && USE(JSVALUE64) && !ENABLE(JIT_CAGE)
#include "FFIInvokeThunk.h"
#endif

namespace JSC { namespace FFI {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Signature);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SignatureRegistry);

// Zero-initialized process-global counters (constant-initialized; no static
// initializer). Read by $vm.ffiCompileCounts() and incremented by the
// IC-stub generator, SpeculativeJIT::compileCallFFI and the FTL lowering.
CompileCounts g_ffiCompileCounts;

Signature::Signature(std::span<const Type> arguments, Type returnType)
    : m_arguments(arguments)
    , m_returnType(returnType)
{
    m_hash = computeHash(arguments, returnType);
}

RefPtr<Signature> Signature::tryCreate(std::span<const Type> arguments, Type returnType)
{
    if (arguments.size() > maxArguments)
        return nullptr;
    if (!isValidReturnType(returnType))
        return nullptr;
    for (Type type : arguments) {
        if (!isValidArgumentType(type))
            return nullptr;
    }
    return SignatureRegistry::singleton().intern(arguments, returnType);
}

unsigned Signature::computeHash(std::span<const Type> arguments, Type returnType)
{
    unsigned hash = WTF::IntHash<unsigned>::hash(static_cast<unsigned>(returnType));
    hash = WTF::pairIntHash(hash, static_cast<unsigned>(arguments.size()));
    for (Type type : arguments)
        hash = WTF::pairIntHash(hash, static_cast<unsigned>(type));
    return hash;
}

bool Signature::structurallyEquals(std::span<const Type> arguments, Type returnType, const Signature& signature)
{
    if (returnType != signature.returnType())
        return false;
    if (arguments.size() != signature.argumentCount())
        return false;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != signature.argumentType(i))
            return false;
    }
    return true;
}

bool Signature::operator==(const Signature& other) const
{
    if (this == &other)
        return true;
    return structurallyEquals(arguments(), returnType(), other);
}

String Signature::toString() const
{
    StringBuilder builder;
    builder.append(name(m_returnType));
    builder.append('(');
    for (unsigned i = 0; i < argumentCount(); ++i) {
        if (i)
            builder.append(',');
        builder.append(name(argumentType(i)));
    }
    builder.append(')');
    return builder.toString();
}

CodePtr<JITThunkPtrTag> Signature::invokeThunk()
{
    // Fast path (every host-path FFI call): the thunk is published once and never changes,
    // so read it lock-free; only the one-time generation takes the lock (double-checked).
    if (auto* published = m_publishedInvokeThunk.load(std::memory_order_acquire)) [[likely]]
        return CodePtr<JITThunkPtrTag>::fromTaggedPtr(published);

    Locker locker { m_codeLock };
#if ENABLE(JIT) && USE(JSVALUE64) && !ENABLE(JIT_CAGE)
    // The thunk is signature-pure (target and slots are runtime parameters),
    // so it is generated at most once per Signature and shared process-wide.
    // A failed generation (executable-memory exhaustion) leaves the cache
    // empty and is retried on the next call.
    if (!m_invokeThunkCode) {
        m_invokeThunkCode = generateInvokeThunk(*this);
        if (m_invokeThunkCode) {
            dataLogLnIf(Options::verboseFFI(), "FFI: generated invoke thunk for ", toString(), " at ", RawPointer(m_invokeThunkCode.code().taggedPtr()));
            m_publishedInvokeThunk.store(m_invokeThunkCode.code().taggedPtr(), std::memory_order_release);
        }
    }
#endif
    return m_invokeThunkCode.code();
}

SignatureRegistry& SignatureRegistry::singleton()
{
    static LazyNeverDestroyed<SignatureRegistry> registry;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        registry.construct();
    });
    return registry;
}

// Lookup key for the registry's hash-consing table: a (arguments, returnType)
// shape hashed with the same function Signature::computeHash uses for the
// stored entry, so a structurally equal shape lands in the same bucket chain
// and matches without materializing a Signature (WTF HashSet::ensure
// translator pattern; see wasm/WasmTypeSectionState.cpp
// ProjectionLookupTranslator).
struct SignatureShape {
    std::span<const Type> arguments;
    Type returnType;
    unsigned hash;
};

struct SignatureShapeTranslator {
    static unsigned hash(const SignatureShape& shape) { return shape.hash; }
    static bool equal(const Ref<Signature>& entry, const SignatureShape& shape)
    {
        return Signature::structurallyEquals(shape.arguments, shape.returnType, entry.get());
    }
};

Ref<Signature> SignatureRegistry::intern(std::span<const Type> arguments, Type returnType)
{
    Locker locker { m_lock };

    // Hash-cons on structure: probe by shape and only allocate a Signature
    // when no structurally equal canonical entry exists yet (mirrors
    // Wasm::TypeInformation's canonical singleton table). Structural
    // equality therefore implies Signature* identity. Entries are immortal.
    SignatureShape shape { arguments, returnType, Signature::computeHash(arguments, returnType) };
    auto addResult = m_signatures.ensure<SignatureShapeTranslator>(shape, [&] {
        return adoptRef(*new Signature(arguments, returnType));
    });
    if (addResult.isNewEntry)
        dataLogLnIf(Options::verboseFFI(), "FFI: interned signature ", (*addResult.iterator)->toString());
    return addResult.iterator->copyRef();
}

size_t SignatureRegistry::size()
{
    Locker locker { m_lock };
    return m_signatures.size();
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
