/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageText.h"
#include "AOTInstance.h"
#include "IdentifierInlines.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class Image;

struct ProgramSymbol {
    uint32_t number;
    UniquedStringImpl* uid;
};

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
    ProgramIdentifiers(VM&, const Image&, ProgramIdentifiers* ofVM = nullptr);
    ~ProgramIdentifiers();

    uint32_t count() const { return m_count; }
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    UniquedStringImpl* identifiers() const { return m_atoms; }
#endif
    const ProgramSymbols& symbols() const { return m_symbols; }
    static ProgramIdentifiers& owning(const ProgramSymbols& symbols) { return *std::bit_cast<ProgramIdentifiers*>(std::bit_cast<uintptr_t>(&symbols) - OBJECT_OFFSETOF(ProgramIdentifiers, m_symbols)); }
    bool isOfOneRealm() const { return m_ofVM; }
    bool isSymbolOfRealm(uint32_t number) const { return (m_ofVM || m_realmsCreateTheirSymbols) && isCreatedSymbol(number); }
    void didGainInstance() { ++m_numberOfInstances; }
    bool didLoseLastInstance() { return !--m_numberOfInstances; }

    ALWAYS_INLINE UniquedStringImpl* uid(uint32_t number);
    ALWAYS_INLINE Identifier identifier(uint32_t number) { return Identifier::fromUid(m_vm, uid(number)); }
    UniquedStringImpl* tryGetIdentifierConcurrently(uint32_t number) const;
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOf(UniquedStringImpl*) const;
    void createEngineSymbols();

    void nameForHeapSnapshots(const void* variable, const AtomString& name) { m_namesOfVariables.set(variable, name); }
    void nameForHeapSnapshots(uint32_t closureNumber, uint32_t slot, const AtomString& name, bool isVariableOfModule)
    {
        m_namesOfCaptures.set(keyOfCapture(closureNumber, slot), name);
        if (isVariableOfModule)
            m_capturesThatAreVariablesOfModules.add(keyOfCapture(closureNumber, slot));
    }
    UniquedStringImpl* nameInHeapSnapshots(const void* variable) const
    {
        auto entry = m_namesOfVariables.find(variable);
        return entry == m_namesOfVariables.end() ? nullptr : entry->value.impl();
    }
    UniquedStringImpl* nameInHeapSnapshots(uint32_t closureNumber, uint32_t slot) const
    {
        auto entry = m_namesOfCaptures.find(keyOfCapture(closureNumber, slot));
        return entry == m_namesOfCaptures.end() ? nullptr : entry->value.impl();
    }
    bool isVariableOfModuleInHeapSnapshots(uint32_t closureNumber, uint32_t slot) const { return m_capturesThatAreVariablesOfModules.contains(keyOfCapture(closureNumber, slot)); }

    static String textOf(const Image&, uint32_t number);
    static AtomStringImpl* findAtom(StringView, unsigned hash);
    AtomStringImpl* findAtomOfNativeText(std::span<const Latin1Character>);

private:
    struct NativeText {
        const Latin1Character* characters { nullptr };
        size_t length { 0 };
        AtomStringImpl* atom { nullptr };
    };
    struct NativeTextFingerprint {
        uint64_t fingerprint { 0 };
        AtomStringImpl* atom { nullptr };
    };
    using NativeTextFingerprintSet = std::array<NativeTextFingerprint, 4>;

    static uint64_t keyOfCapture(uint32_t closureNumber, uint32_t slot) { return static_cast<uint64_t>(closureNumber) << 32 | slot; }
    AtomStringImpl* findAtomOfNativeTextByCharacters(std::span<const Latin1Character>);
    JS_EXPORT_PRIVATE UniquedStringImpl* uidSlowCase(uint32_t number);
    Identifier create(uint32_t number) const;
    bool isCreatedSymbol(uint32_t number) const;
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
    ProgramIdentifiers* const m_ofVM;
    const bool m_realmsCreateTheirSymbols;
    unsigned m_numberOfInstances { 0 };
    ProgramSymbols m_symbols;
    mutable Lock m_symbolsLock;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_numbersOfSymbols WTF_GUARDED_BY_LOCK(m_symbolsLock);
    UncheckedKeyHashMap<const void*, AtomString> m_namesOfVariables;
    UncheckedKeyHashMap<uint64_t, AtomString, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesOfCaptures;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_capturesThatAreVariablesOfModules;
    std::array<NativeText, 128> m_recentNativeTexts;
    std::array<NativeTextFingerprintSet, 32> m_nativeTextsByCharacters;
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

ALWAYS_INLINE ProgramIdentifiers& identifiersOf(const Instance& instance) { return ProgramIdentifiers::owning(*instance.image.programSymbols); }
JS_EXPORT_PRIVATE ProgramIdentifiers* identifiersOfRealm(VM&, JSGlobalObject* realmOrNull);

JS_EXPORT_PRIVATE Identifier enginePrivateName(VM&, const String& text);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
