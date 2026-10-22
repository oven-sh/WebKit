/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramIdentifiers.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIdentifierTable.h"
#include "AOTImage.h"
#include "BuiltinNames.h"
#include "JSCInlines.h"
#include <algorithm>
#include <bit>
#include <span>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramIdentifiers);

static constexpr uint32_t minNumberOfSymbolPlaces = 64;

static ProgramSymbols createSymbols(uint32_t numberOfPlaces)
{
    ASSERT(hasOneBitSet(numberOfPlaces));
    auto* entries = static_cast<ProgramSymbol*>(fastZeroedMalloc(ProgramSymbols::numberOfEntriesFor(numberOfPlaces) * sizeof(ProgramSymbol)));
    return { entries, numberOfPlaces - 1, 0 };
}

static void insert(ProgramSymbols& symbols, ProgramSymbol symbol)
{
    ASSERT(symbol.number && symbol.uid && (symbols.count + 1) * 2 <= symbols.mask + 1);
    ProgramSymbol* entry = &symbols.entries[symbol.number & symbols.mask];
    while (entry->number)
        ++entry;
    *entry = symbol;
    ++symbols.count;
}

Identifier enginePrivateName(VM& vm, const String& text)
{
    if (PrivateSymbolImpl* symbol = vm.propertyNames->builtinNames().lookUpPrivateName(text))
        return Identifier::fromUid(vm, symbol);
    Ref<PrivateSymbolImpl> symbol = PrivateSymbolImpl::create(*text.impl());
    Identifier name = Identifier::fromUid(vm, symbol.ptr());
    vm.propertyNames->appendExternalName(Identifier::fromString(vm, text), name);
    SUPPRESS_UNCOUNTED_LOCAL auto* leaked = &symbol.leakRef();
    UNUSED_VARIABLE(leaked);
    return name;
}

// The VM makes this object before it makes a name of its own, so from its first atom on a text that the image has is the atom of the image.
ProgramIdentifiers::ProgramIdentifiers(VM& vm, const Image& image)
    : m_vm(vm)
    , m_image(image)
    , m_count(image.text().numberOfIdentifiers())
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    , m_text(image.text())
#else
    , m_atoms(image.text().identifierAtoms())
#endif
    , m_symbols(createSymbols(std::max(minNumberOfSymbolPlaces, WTF::roundUpToPowerOfTwo(static_cast<uint32_t>(image.programTables().identifiersOfEngineSymbols().size()) * 2 + 2))))
{
    RELEASE_ASSERT(&image == Image::current());
    AtomStringTable::setReadOnlyAtomLookup(findAtom);
}

ProgramIdentifiers::~ProgramIdentifiers()
{
    for (const ProgramSymbol& symbol : std::span { m_symbols.entries, ProgramSymbols::numberOfEntriesFor(m_symbols.mask + 1) }) {
        if (symbol.number)
            symbol.uid->deref();
    }
    fastFree(m_symbols.entries);
}

void ProgramIdentifiers::createEngineSymbols()
{
    if (m_hasCreatedEngineSymbols)
        return;
    m_hasCreatedEngineSymbols = true;
    for (uint32_t number : m_image.programTables().identifiersOfEngineSymbols())
        uid(number);
}

NEVER_INLINE UniquedStringImpl* ProgramIdentifiers::uidSlowCase(uint32_t number)
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (m_text.stringRecords()[number].hasText())
        return static_cast<UniquedStringImpl*>(&m_text.ensureLazyHeader(number));
#endif
    if (UniquedStringImpl* uid = m_symbols.find(number))
        return uid;
    Identifier created = create(number);
    if (created.isNull()) {
        RELEASE_ASSERT(!number);
        return nullptr;
    }
    UniquedStringImpl* uid = &Ref { *created.impl() }.leakRef();
    Locker locker { m_symbolsLock };
    addSymbol(number, uid);
    return uid;
}

void ProgramIdentifiers::addSymbol(uint32_t number, UniquedStringImpl* uid)
{
    if ((m_symbols.count + 1) * 2 > m_symbols.mask + 1) {
        ProgramSymbols grown = createSymbols((m_symbols.mask + 1) * 2);
        for (const ProgramSymbol& symbol : std::span { m_symbols.entries, ProgramSymbols::numberOfEntriesFor(m_symbols.mask + 1) }) {
            if (symbol.number)
                insert(grown, symbol);
        }
        fastFree(std::exchange(m_symbols, grown).entries);
    }
    insert(m_symbols, { number, uid });
    if (uid->isSymbol())
        m_numbersOfSymbols.add(uid, number);
}

Identifier ProgramIdentifiers::create(uint32_t number) const
{
    const ImageText& text = m_image.text();
    const ImageIdentifierWord& record = text.identifier(number);
    StringID string = record.text();
    switch (record.kind()) {
    case PropertyKeyKind::Text:
        if (!string)
            return Identifier();
        RELEASE_ASSERT(text.view(string).isEmpty());
        return Identifier(Identifier::EmptyIdentifierFlag::EmptyIdentifier);
    case PropertyKeyKind::EnginePrivateName:
        return enginePrivateName(m_vm, text.createString(string));
    case PropertyKeyKind::PrivateName:
        return Identifier::fromUid(PrivateSymbolImpl::create(*text.createString(string).impl()).get());
    case PropertyKeyKind::RegisteredSymbol:
        return Identifier::fromUid(m_vm.symbolRegistry().symbolForKey(text.createString(string)).get());
    case PropertyKeyKind::WellKnownSymbol: {
        const BuiltinTable& builtins = BuiltinTable::singleton();
        BuiltinName name = builtins.nameOf(record.wellKnownSymbol());
        RELEASE_ASSERT(name);
        return builtins.identifierOf(m_vm, name);
    }
    case PropertyKeyKind::CreatedSymbol:
        if (!string)
            return Identifier::fromUid(SymbolImpl::createNullSymbol().get());
        return Identifier::fromUid(SymbolImpl::create(*text.createString(string).impl()).get());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

UniquedStringImpl* ProgramIdentifiers::tryGetIdentifierConcurrently(uint32_t number) const
{
    RELEASE_ASSERT(number < m_count);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (m_text.stringRecords()[number].hasText())
        return static_cast<UniquedStringImpl*>(&m_text.ensureLazyHeader(number));
#else
    UniquedStringImpl* atom = m_atoms + number;
    if (atom->length())
        return atom;
#endif
    if (!number)
        return nullptr;
    Locker locker { m_symbolsLock };
    return m_symbols.find(number);
}

std::optional<uint32_t> ProgramIdentifiers::numberOf(UniquedStringImpl* uid) const
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (std::optional<uint32_t> number = m_text.lazyHeaderNumber(*uid)) {
        RELEASE_ASSERT(*number < m_count);
        return number;
    }
#else
    uintptr_t offset = std::bit_cast<uintptr_t>(uid) - std::bit_cast<uintptr_t>(m_atoms);
    if (offset < static_cast<uintptr_t>(m_count) * sizeof(UniquedStringImpl))
        return static_cast<uint32_t>(offset / sizeof(UniquedStringImpl));
#endif
    if (uid->isSymbol()) {
        Locker locker { m_symbolsLock };
        auto found = m_numbersOfSymbols.find(uid);
        if (found == m_numbersOfSymbols.end())
            return std::nullopt;
        return found->value;
    }
    if (!uid->length())
        return numberOfText(m_image, StringView(*uid), uid->existingHash());
    ASSERT(!numberOfText(m_image, StringView(*uid), uid->existingHash()));
    return std::nullopt;
}

std::optional<uint32_t> ProgramIdentifiers::numberOfText(const Image& image, StringView characters, unsigned hash)
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    const ImageText& text = image.text();
    return IdentifierTable(image.programTables().identifiersByHash()).find(hash, [&](uint32_t number) {
        const ImageStringRecord& record = text.stringRecords()[number];
        if (!record.hasText())
            return characters.isEmpty();
        return record.hash() == hash && text.view(record) == characters;
#else
    const UniquedStringImpl* atoms = image.text().identifierAtoms();
    return IdentifierTable(image.programTables().identifiersByHash()).find(hash, [&](uint32_t number) {
        const UniquedStringImpl& atom = atoms[number];
        if (!atom.length())
            return characters.isEmpty();
        return atom.existingHash() == hash && StringView(atom) == characters;
#endif
    });
}

AtomStringImpl* ProgramIdentifiers::findAtom(StringView characters, unsigned hash)
{
    if (characters.isEmpty())
        return nullptr;
    const Image& image = *Image::current();
    std::optional<uint32_t> number = numberOfText(image, characters, hash);
    if (!number)
        return nullptr;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return static_cast<AtomStringImpl*>(&image.text().ensureLazyHeader(*number));
#else
    return static_cast<AtomStringImpl*>(image.text().identifierAtoms() + *number);
#endif
}

String ProgramIdentifiers::textOf(const Image& image, uint32_t number)
{
    const ImageText& text = image.text();
    const ImageIdentifierWord& record = text.identifier(number);
    if (record.kind() != PropertyKeyKind::Text || !record.text())
        return String();
    return text.createAtom(record.text());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
