/*
 * Copyright (C) 2020-2021 Apple Inc. All rights reserved.
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

#include "CacheableIdentifier.h"

#include "Identifier.h"
#if USE(BUN_JSC_ADDITIONS)
#include "IdentifierInlines.h"
#endif
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include "VM.h"
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

template <typename CodeBlockType>
inline CacheableIdentifier CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(CodeBlockType* codeBlock, const Identifier& i)
{
    return createFromIdentifierOwnedByCodeBlock(codeBlock, i.impl());
}

template <typename CodeBlockType>
inline CacheableIdentifier CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(CodeBlockType* codeBlock, UniquedStringImpl* uid)
{
    ASSERT_UNUSED(codeBlock, codeBlock->hasIdentifier(uid));
    return CacheableIdentifier(uid);
}

inline CacheableIdentifier CacheableIdentifier::createFromImmortalIdentifier(UniquedStringImpl* uid)
{
    return CacheableIdentifier(uid);
}

inline CacheableIdentifier CacheableIdentifier::createFromSharedStub(UniquedStringImpl* uid)
{
    return CacheableIdentifier(uid);
}

inline CacheableIdentifier CacheableIdentifier::createFromCell(JSCell* i)
{
#if USE(BUN_JSC_ADDITIONS)
    // Phase D: only the small-8-bit-inline form (2..5 Latin-1 chars) is a
    // canonical fiber-word key; big-inline / 16-bit-inline span two words and
    // must be atomized in place so uid()/getValueImpl() yield a real atom.
    if (i->isString()) {
        JSString* s = uncheckedDowncast<JSString>(i);
        if (s->isInline() && !(enableIdentifierFiberWords && s->is8Bit() && s->length() <= maxFiberWordKeyLength))
            s->resolveInlineToAtomString(nullptr);
    }
#endif
    return CacheableIdentifier(i);
}

inline CacheableIdentifier::CacheableIdentifier(UniquedStringImpl* uid)
{
    setUidBits(uid);
}

inline CacheableIdentifier::CacheableIdentifier(JSCell* identifier)
{
    ASSERT(isCacheableIdentifierCell(identifier));
    setCellBits(identifier);
}

inline JSCell* CacheableIdentifier::cell() const
{
    ASSERT(isCell());
    return std::bit_cast<JSCell*>(m_bits);
}

inline UniquedStringImpl* CacheableIdentifier::uid() const
{
    if (!m_bits)
        return nullptr;
    if (isUid())
        return std::bit_cast<UniquedStringImpl*>(m_bits & ~s_uidTag);
    if (isSymbolCell())
        return &uncheckedDowncast<Symbol>(cell())->uid();
    ASSERT(isStringCell());
    JSString* string = uncheckedDowncast<JSString>(cell());
#if USE(BUN_JSC_ADDITIONS)
    // D.4 coherence: return the canonical fiber word for any 2..5-char Latin-1
    // key so PropertyName / PropertyTable::find hit regardless of whether the
    // cell is inline or already atom-backed (string literals).
    if (enableIdentifierFiberWords && string->isInline() && string->is8Bit() && string->length() <= maxFiberWordKeyLength)
        return std::bit_cast<UniquedStringImpl*>(string->inlineFiberWord());
    StringImpl* impl = string->getValueImpl();
    // Fast-bail: a non-inline atom-backed cell with len>5 can never encode as a
    // fiber word — return the atom impl directly and skip canonicalFiberWordFor.
    if (!string->isInline() && impl->length() > maxFiberWordKeyLength && impl->isAtom()) [[likely]]
        return std::bit_cast<UniquedStringImpl*>(impl);
    if (uintptr_t fiber = Identifier::canonicalFiberWordFor(impl))
        return std::bit_cast<UniquedStringImpl*>(fiber);
    return std::bit_cast<UniquedStringImpl*>(impl);
#else
    return std::bit_cast<UniquedStringImpl*>(string->getValueImpl());
#endif
}

inline bool CacheableIdentifier::isSymbol() const
{
#if USE(BUN_JSC_ADDITIONS)
    return m_bits && uidIsSymbol(uid());
#else
    return m_bits && uid()->isSymbol();
#endif
}

inline bool CacheableIdentifier::isPrivateName() const
{
    return isSymbol() && static_cast<SymbolImpl&>(*uid()).isPrivate();
}

inline unsigned CacheableIdentifier::hash() const
{
#if USE(BUN_JSC_ADDITIONS)
    return uidHash(uid());
#else
    return uid()->symbolAwareHash();
#endif
}

inline bool CacheableIdentifier::isCacheableIdentifierCell(JSCell* cell)
{
    if (cell->isSymbol())
        return true;
    if (!cell->isString())
        return false;
    JSString* string = uncheckedDowncast<JSString>(cell);
#if USE(BUN_JSC_ADDITIONS)
    // Phase D: only small-8-bit-inline (2..5 Latin-1) has a content-unique
    // single-word key usable as a uid without atomization.
    if (enableIdentifierFiberWords && string->isInline() && string->is8Bit() && string->length() <= maxFiberWordKeyLength)
        return true;
#endif
    if (const StringImpl* impl = string->tryGetValueImpl())
        return impl->isAtom();
    return false;
}

inline bool CacheableIdentifier::isCacheableIdentifierCell(JSValue value)
{
    if (!value.isCell())
        return false;
    return isCacheableIdentifierCell(value.asCell());
}

inline GCOwnedDataScope<const UniquedStringImpl*> CacheableIdentifier::getCacheableIdentifier(JSCell* cell)
{
    if (cell->isSymbol())
        return { cell, &asSymbol(cell)->uid() };
    if (!cell->isString())
        return { };
    JSString* string = uncheckedDowncast<JSString>(cell);
#if USE(BUN_JSC_ADDITIONS)
    // D.4 coherence: hand back the canonical fiber word so downstream
    // PropertyName / CacheableIdentifier::uid() match the parser's key.
    if (string->isInline()) {
        if (enableIdentifierFiberWords && string->is8Bit() && string->length() <= maxFiberWordKeyLength)
            return { cell, std::bit_cast<const UniquedStringImpl*>(string->inlineFiberWord()) };
        return { cell, string->resolveInlineToAtomString(nullptr) };
    }
    if (const StringImpl* impl = string->tryGetValueImpl(); impl && impl->isAtom()) {
        if (uintptr_t fiber = Identifier::canonicalFiberWordFor(impl))
            return { cell, std::bit_cast<const UniquedStringImpl*>(fiber) };
        return { cell, static_cast<const AtomStringImpl*>(impl) };
    }
    return { };
#else
    if (const StringImpl* impl = string->tryGetValueImpl(); impl && impl->isAtom())
        return { cell, static_cast<const AtomStringImpl*>(impl) };
    return { };
#endif
}

inline GCOwnedDataScope<const UniquedStringImpl*> CacheableIdentifier::getCacheableIdentifier(JSValue value)
{
    if (!value.isCell())
        return { };
    return getCacheableIdentifier(value.asCell());
}

inline bool CacheableIdentifier::isSymbolCell() const
{
    return isCell() && cell()->isSymbol();
}

inline bool CacheableIdentifier::isStringCell() const
{
    return isCell() && cell()->isString();
}

inline void CacheableIdentifier::ensureIsCell(VM& vm)
{
    if (!isCell()) {
#if USE(BUN_JSC_ADDITIONS)
        UniquedStringImpl* rep = uid();
        if (uidIsSymbol(rep))
            setCellBits(Symbol::create(vm, static_cast<SymbolImpl&>(*rep)));
        else if (isInlinePropertyKey(rep))
            setCellBits(JSString::createInlineFromFiber(vm, reinterpret_cast<uintptr_t>(rep)));
        else
            setCellBits(jsString(vm, String(static_cast<AtomStringImpl*>(rep))));
#else
        if (uid()->isSymbol())
            setCellBits(Symbol::create(vm, static_cast<SymbolImpl&>(*uid())));
        else
            setCellBits(jsString(vm, String(static_cast<AtomStringImpl*>(uid()))));
#endif
    }
    ASSERT(isCell());
}

inline void CacheableIdentifier::setCellBits(JSCell* cell)
{
    RELEASE_ASSERT(isCacheableIdentifierCell(cell));
    m_bits = std::bit_cast<uintptr_t>(cell);
}

inline void CacheableIdentifier::setUidBits(UniquedStringImpl* uid)
{
    m_bits = std::bit_cast<uintptr_t>(uid) | s_uidTag;
}

template<typename Visitor>
inline void CacheableIdentifier::visitAggregate(Visitor& visitor) const
{
    if (m_bits && isCell())
        visitor.appendUnbarriered(cell());
}

inline bool CacheableIdentifier::operator==(const CacheableIdentifier& other) const
{
    return uid() == other.uid();
}

inline bool CacheableIdentifier::operator==(const Identifier& other) const
{
    return uid() == other.impl();
}

} // namespace JSC
