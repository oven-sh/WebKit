/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageText.h"
#include "IdentifierInlines.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class Image;

struct ProgramSymbol {
    // Zero in an entry that is not in use.
    uint32_t number;
    UniquedStringImpl* uid;
};

// The identifiers that are no atoms of the image, by number: symbols and private names, which are made at run time, and the empty text, whose atom is the engine's.
struct ProgramSymbols {
    static constexpr uint32_t numberOfEntriesFor(uint32_t numberOfPlaces) { return numberOfPlaces + numberOfPlaces / 2 + 1; }

    UniquedStringImpl* find(uint32_t number) const
    {
        for (const ProgramSymbol* entry = &entries[number & mask]; entry->number; ++entry) {
            if (entry->number == number)
                return entry->uid;
        }
        return nullptr;
    }

    ProgramSymbol* entries;
    uint32_t mask;
    uint32_t count;
};

class ProgramIdentifiers {
    WTF_MAKE_NONCOPYABLE(ProgramIdentifiers);
    WTF_MAKE_TZONE_ALLOCATED(ProgramIdentifiers);
public:
    ProgramIdentifiers(VM&, const Image&);
    ~ProgramIdentifiers();

    uint32_t count() const { return m_count; }
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    // The atoms of the image, read-only: the name with the number n is identifiers() + n. A header whose length is zero is no name, and symbols() has the uid of that number.
    UniquedStringImpl* identifiers() const { return m_atoms; }
#endif
    const ProgramSymbols& symbols() const { return m_symbols; }

    // Null for the number 0 only.
    ALWAYS_INLINE UniquedStringImpl* uid(uint32_t number);
    ALWAYS_INLINE Identifier identifier(uint32_t number) { return Identifier::fromUid(m_vm, uid(number)); }
    // Null for a symbol or a private name that uid() has not made yet.
    UniquedStringImpl* tryGetIdentifierConcurrently(uint32_t number) const;
    // Exact once createEngineSymbols() has run: every other symbol the program names exists only after uid() created it.
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOf(UniquedStringImpl*) const;
    void createEngineSymbols();

    static String textOf(const Image&, uint32_t number);
    // What the atom table asks before it makes an atom. Text made at run time that equals a name of the image is that name.
    static AtomStringImpl* findAtom(StringView, unsigned hash);
    // For native code that has the text of a name as characters of its own. Null if no image has the text. Asked again about the same characters at the same address, it hashes nothing.
    AtomStringImpl* findAtomOfNativeText(std::span<const Latin1Character>);

private:
    struct NativeText {
        const Latin1Character* characters { nullptr };
        size_t length { 0 };
        AtomStringImpl* atom { nullptr };
    };

    JS_EXPORT_PRIVATE UniquedStringImpl* uidSlowCase(uint32_t number);
    Identifier create(uint32_t number) const;
    void addSymbol(uint32_t number, UniquedStringImpl*) WTF_REQUIRES_LOCK(m_symbolsLock);
    static std::optional<uint32_t> numberOfText(const Image&, StringView, unsigned hash);
    static AtomStringImpl* findAtomSlow(StringView, unsigned hash);

    VM& m_vm;
    const Image& m_image;
    const uint32_t m_count;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    const ImageText& m_text;
#else
    UniquedStringImpl* const m_atoms;
#endif
    bool m_hasCreatedEngineSymbols { false };
    // Only the thread of the VM changes the table, with the lock held. It and its stubs read the table without the lock, every other thread with it.
    ProgramSymbols m_symbols;
    mutable Lock m_symbolsLock;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_numbersOfSymbols WTF_GUARDED_BY_LOCK(m_symbolsLock);
    std::array<NativeText, 128> m_recentNativeTexts;
};

ALWAYS_INLINE UniquedStringImpl* ProgramIdentifiers::uid(uint32_t number)
{
    RELEASE_ASSERT(number < m_count);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (StringImpl* atom = m_text.tryGetLazyHeader(number)) [[likely]]
        return static_cast<UniquedStringImpl*>(atom);
#else
    UniquedStringImpl* atom = m_atoms + number;
    if (atom->length()) [[likely]]
        return atom;
#endif
    return uidSlowCase(number);
}

JS_EXPORT_PRIVATE Identifier enginePrivateName(VM&, const String& text);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
