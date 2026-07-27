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

#if ENABLE(JIT) && USE(JSVALUE64) && !ENABLE(JIT_CAGE)
#include "FFIInvokeThunk.h"
#endif

namespace JSC { namespace FFI {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Signature);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SignatureRegistry);

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
    if (auto* published = m_publishedInvokeThunk.load(std::memory_order_acquire)) [[likely]]
        return CodePtr<JITThunkPtrTag>::fromTaggedPtr(published);

    Locker locker { m_codeLock };
#if ENABLE(JIT) && USE(JSVALUE64) && !ENABLE(JIT_CAGE)
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

    SignatureShape shape { arguments, returnType, Signature::computeHash(arguments, returnType) };
    auto addResult = m_signatures.ensure<SignatureShapeTranslator>(shape, [&] {
        return adoptRef(*new Signature(arguments, returnType));
    });
    if (addResult.isNewEntry)
        dataLogLnIf(Options::verboseFFI(), "FFI: interned signature ", (*addResult.iterator)->toString());
    return addResult.iterator->copyRef();
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
