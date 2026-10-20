/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Identifier.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class Image;

class ProgramIdentifiers {
    WTF_MAKE_NONCOPYABLE(ProgramIdentifiers);
    WTF_MAKE_TZONE_ALLOCATED(ProgramIdentifiers);
public:
    ProgramIdentifiers(VM&, const Image&);
    ~ProgramIdentifiers();

    uint32_t count() const { return m_count; }
    UniquedStringImpl* const* identifiers() const { return m_identifiers; }

    ALWAYS_INLINE const Identifier& identifier(uint32_t number);
    const Identifier* tryGetIdentifierConcurrently(uint32_t number) const;
    // Exact once createEngineSymbols() has run: every other symbol the program names exists only after identifier() created it.
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOf(UniquedStringImpl*) const;
    void createEngineSymbols();

    static String textOf(const Image&, uint32_t number);

private:
    JS_EXPORT_PRIVATE const Identifier& identifierSlowCase(uint32_t number);
    Identifier create(uint32_t number) const;
    std::optional<uint32_t> numberOfText(const UniquedStringImpl&) const;

    VM& m_vm;
    const Image& m_image;
    const uint32_t m_count;
    UniquedStringImpl** m_identifiers;
    std::span<const uint32_t> m_numbersByHash;
    mutable Lock m_symbolsLock;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_numbersOfSymbols WTF_GUARDED_BY_LOCK(m_symbolsLock);
};

ALWAYS_INLINE const Identifier& ProgramIdentifiers::identifier(uint32_t number)
{
    static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
    RELEASE_ASSERT(number < m_count);
    if (m_identifiers[number]) [[likely]]
        return *reinterpret_cast<const Identifier*>(&m_identifiers[number]);
    return identifierSlowCase(number);
}

JS_EXPORT_PRIVATE Identifier enginePrivateName(VM&, const String& text);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
