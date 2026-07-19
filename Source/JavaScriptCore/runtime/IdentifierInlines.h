/*
 * Copyright (C) 2014-2019 Apple Inc. All rights reserved.
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

#include "CallFrame.h"
#include "Identifier.h"
#include "Symbol.h"
#include "VM.h"
#include <JavaScriptCore/JSString.h>

namespace JSC  {

#if USE(BUN_JSC_ADDITIONS)
// Phase D.4 canonical-representation coherence: a 2..7-char Latin-1 name has
// exactly one m_bits value — the encodeInline8 fiber word. Every Identifier
// construction path funnels through this so PropertyTable's key==entry.key()
// pointer compare hits whether the key came from the parser span ctor,
// JSString::toIdentifier, or any fromString()/fromUid() caller.
ALWAYS_INLINE uintptr_t Identifier::canonicalFiberWordFor(const StringImpl* impl)
{
    if (!impl || impl->isSymbol())
        return 0;
    unsigned len = impl->length();
    if (len < 2 || len > JSString::maxInlineLength8)
        return 0;
    if (impl->is8Bit()) [[likely]]
        return JSString::encodeInline8(impl->span8());
    auto span16 = impl->span16();
    Latin1Character narrowed[JSString::maxInlineLength8];
    for (unsigned i = 0; i < len; ++i) {
        char16_t c = span16[i];
        if (c > 0xff)
            return 0;
        narrowed[i] = static_cast<Latin1Character>(c);
    }
    return JSString::encodeInline8({ narrowed, len });
}

inline Identifier::Identifier(VM& vm, std::span<const Latin1Character> string)
{
    // Phase D.2 producer: short Latin-1 identifiers live as a tagged fiber word
    // (no AtomStringImpl materialized). size 0/1 and >7 fall through to add().
    if (string.size() >= 2 && string.size() <= JSString::maxInlineLength8) {
        m_bits = JSString::encodeInline8(string);
        return;
    }
    m_bits = reinterpret_cast<uintptr_t>(&add(vm, string).leakRef());
}
#else
inline Identifier::Identifier(VM& vm, std::span<const Latin1Character> string)
    : m_string(add(vm, string))
{
    ASSERT(m_string.impl()->isAtom());
}
#endif

#if USE(BUN_JSC_ADDITIONS)
inline Identifier::Identifier(VM& vm, std::span<const char16_t> string)
{
    // D.4 coherence: a 16-bit span whose content fits Latin-1 must yield the
    // same fiber word the Latin-1 span ctor would.
    if (string.size() >= 2 && string.size() <= JSString::maxInlineLength8) {
        Latin1Character narrowed[JSString::maxInlineLength8];
        bool allLatin1 = true;
        for (size_t i = 0; i < string.size(); ++i) {
            char16_t c = string[i];
            if (c > 0xff) { allLatin1 = false; break; }
            narrowed[i] = static_cast<Latin1Character>(c);
        }
        if (allLatin1) {
            m_bits = JSString::encodeInline8({ narrowed, string.size() });
            return;
        }
    }
    m_bits = reinterpret_cast<uintptr_t>(&add(vm, string).leakRef());
    ASSERT(reinterpret_cast<StringImpl*>(m_bits)->isAtom());
}

ALWAYS_INLINE Identifier::Identifier(VM& vm, ASCIILiteral literal)
{
    // D.4 coherence: ASCII is Latin-1; 2..7 chars must be a fiber word.
    size_t len = literal.length();
    if (len >= 2 && len <= JSString::maxInlineLength8) {
        m_bits = JSString::encodeInline8(literal.span8());
        return;
    }
    m_bits = reinterpret_cast<uintptr_t>(&add(vm, literal).leakRef());
    ASSERT(reinterpret_cast<StringImpl*>(m_bits)->isAtom());
}

inline Identifier::Identifier(VM& vm, AtomStringImpl* string)
{
    if (string) {
        if (uintptr_t fiber = canonicalFiberWordFor(string)) {
            m_bits = fiber;
        } else {
            string->ref();
            m_bits = reinterpret_cast<uintptr_t>(string);
        }
    }
#ifndef NDEBUG
    checkCurrentAtomStringTable(vm);
    if (string)
        ASSERT_WITH_MESSAGE(!string->length() || string->isSymbol() || AtomStringImpl::isInAtomStringTable(string), "The atomic string comes from an other thread!");
#else
    UNUSED_PARAM(vm);
#endif
}

inline Identifier::Identifier(VM& vm, const AtomString& string)
{
    if (auto* rep = string.impl()) {
        if (uintptr_t fiber = canonicalFiberWordFor(rep)) {
            m_bits = fiber;
        } else {
            rep->ref();
            m_bits = reinterpret_cast<uintptr_t>(static_cast<UniquedStringImpl*>(rep));
        }
    }
#ifndef NDEBUG
    checkCurrentAtomStringTable(vm);
    if (!string.isNull())
        ASSERT_WITH_MESSAGE(!string.length() || string.impl()->isSymbol() || AtomStringImpl::isInAtomStringTable(string.impl()), "The atomic string comes from an other thread!");
#else
    UNUSED_PARAM(vm);
#endif
}

inline Identifier::Identifier(VM& vm, const String& string)
{
    if (uintptr_t fiber = canonicalFiberWordFor(string.impl())) {
        m_bits = fiber;
        return;
    }
    m_bits = reinterpret_cast<uintptr_t>(&add(vm, string.impl()).leakRef());
    ASSERT(reinterpret_cast<StringImpl*>(m_bits)->isAtom());
}

inline Identifier::Identifier(VM& vm, StringImpl* rep)
{
    if (uintptr_t fiber = canonicalFiberWordFor(rep)) {
        m_bits = fiber;
        return;
    }
    m_bits = reinterpret_cast<uintptr_t>(&add(vm, rep).leakRef());
    ASSERT(reinterpret_cast<StringImpl*>(m_bits)->isAtom());
}

inline Identifier::Identifier(VM&, Ref<AtomStringImpl>&& impl)
{
    // D.4 coherence: JSString::toIdentifier / JSRopeString::toIdentifier land
    // here for non-inline strings — must match the parser's fiber word for
    // the same 2..7-char Latin-1 content.
    if (uintptr_t fiber = canonicalFiberWordFor(impl.ptr())) {
        m_bits = fiber;
        return;
    }
    m_bits = reinterpret_cast<uintptr_t>(&impl.leakRef());
}
#else
inline Identifier::Identifier(VM& vm, std::span<const char16_t> string)
    : m_string(add(vm, string))
{
    ASSERT(m_string.impl()->isAtom());
}

ALWAYS_INLINE Identifier::Identifier(VM& vm, ASCIILiteral literal)
    : m_string(add(vm, literal))
{
    ASSERT(m_string.impl()->isAtom());
}

inline Identifier::Identifier(VM& vm, AtomStringImpl* string)
    : m_string(string)
{
#ifndef NDEBUG
    checkCurrentAtomStringTable(vm);
    if (string)
        ASSERT_WITH_MESSAGE(!string->length() || string->isSymbol() || AtomStringImpl::isInAtomStringTable(string), "The atomic string comes from an other thread!");
#else
    UNUSED_PARAM(vm);
#endif
}

inline Identifier::Identifier(VM& vm, const AtomString& string)
    : m_string(string)
{
#ifndef NDEBUG
    checkCurrentAtomStringTable(vm);
    if (!string.isNull())
        ASSERT_WITH_MESSAGE(!string.length() || string.impl()->isSymbol() || AtomStringImpl::isInAtomStringTable(string.impl()), "The atomic string comes from an other thread!");
#else
    UNUSED_PARAM(vm);
#endif
}

inline Identifier::Identifier(VM& vm, const String& string)
    : m_string(add(vm, string.impl()))
{
    ASSERT(m_string.impl()->isAtom());
}

inline Identifier::Identifier(VM& vm, StringImpl* rep)
    : m_string(add(vm, rep))
{
    ASSERT(m_string.impl()->isAtom());
}
#endif

inline Ref<AtomStringImpl> Identifier::add(VM& vm, ASCIILiteral literal)
{
    if (literal.length() == 1)
        return vm.smallStrings.singleCharacterStringRep(literal.codeUnitAt(0));
    return AtomStringImpl::add(literal);
}


template <typename T>
Ref<AtomStringImpl> Identifier::add(VM& vm, std::span<const T> string)
{
    if (string.size() == 1) {
        T c = string.front();
        if (canUseSingleCharacterString(c))
            return vm.smallStrings.singleCharacterStringRep(c);
    }
    if (string.empty())
        return *static_cast<AtomStringImpl*>(StringImpl::empty());

#if USE(BUN_JSC_ADDITIONS)
    // Short Latin-1: the inline fiber word is a content-unique key. One
    // direct-mapped compare replaces AtomStringImpl::add's hash+probe.
    if constexpr (std::is_same_v<T, Latin1Character>) {
        if (string.size() <= JSString::maxInlineLength8) {
            uintptr_t fiber = JSString::encodeInline8(string);
            if (auto* cached = vm.inlineAtomCache.lookup(fiber))
                return *cached;
            auto atom = AtomStringImpl::add(string);
            vm.inlineAtomCache.insert(fiber, atom.get());
            return atom.releaseNonNull();
        }
    }
#endif

    return *AtomStringImpl::add(string);
}

inline Ref<AtomStringImpl> Identifier::add(VM& vm, StringImpl* r)
{
#ifndef NDEBUG
    checkCurrentAtomStringTable(vm);
#endif
    return *AtomStringImpl::addWithStringTableProvider(vm, r);
}

inline Identifier Identifier::createLatin1(VM& vm, std::span<const char16_t> string)
{
    return Identifier(vm, add8(vm, string));
}

SUPPRESS_NODELETE inline Identifier Identifier::fromUid(VM& vm, UniquedStringImpl* uid)
{
#if USE(BUN_JSC_ADDITIONS)
    // D.4 coherence: uid may already be a fiber word (from Identifier::impl())
    // — keep it verbatim. A real impl funnels through the canonicalizing ctor.
    if (isInlinePropertyKey(uid))
        return fromFiberWord(reinterpret_cast<uintptr_t>(uid));
    if (!uid || !uid->isSymbol())
        return Identifier(vm, static_cast<StringImpl*>(uid));
    return static_cast<SymbolImpl&>(*uid);
#else
    if (!uid || !uid->isSymbol())
        return Identifier(vm, uid);
    return static_cast<SymbolImpl&>(*uid);
#endif
}

inline Identifier Identifier::fromUid(const PrivateName& name)
{
    return name.uid();
}

inline Identifier Identifier::fromUid(SymbolImpl& symbol)
{
    return symbol;
}

ALWAYS_INLINE Identifier Identifier::fromString(VM& vm, ASCIILiteral s)
{
    return Identifier(vm, s);
}

inline Identifier Identifier::fromString(VM& vm, std::span<const Latin1Character> s)
{
    return Identifier(vm, s);
}

inline Identifier Identifier::fromString(VM& vm, std::span<const char16_t> s)
{
    return Identifier(vm, s);
}

inline Identifier Identifier::fromString(VM& vm, const String& string)
{
    return Identifier(vm, string.impl());
}

inline Identifier Identifier::fromString(VM& vm, AtomStringImpl* atomStringImpl)
{
    return Identifier(vm, atomStringImpl);
}

inline Identifier Identifier::fromString(VM& vm, Ref<AtomStringImpl>&& atomStringImpl)
{
    return Identifier(vm, WTF::move(atomStringImpl));
}

inline Identifier Identifier::fromString(VM& vm, const AtomString& atomString)
{
    return Identifier(vm, atomString);
}

inline Identifier Identifier::fromString(VM& vm, SymbolImpl* symbolImpl)
{
    return Identifier(vm, symbolImpl);
}

inline JSValue identifierToJSValue(VM& vm, const Identifier& identifier)
{
    if (identifier.isSymbol())
        return Symbol::create(vm, static_cast<SymbolImpl&>(*identifier.impl()));
    return jsString(vm, identifier.string());
}

inline JSValue identifierToSafePublicJSValue(VM& vm, const Identifier& identifier) 
{
    if (identifier.isSymbol() && !identifier.isPrivateName())
        return Symbol::create(vm, static_cast<SymbolImpl&>(*identifier.impl()));
    return jsString(vm, identifier.string());
}

} // namespace JSC
