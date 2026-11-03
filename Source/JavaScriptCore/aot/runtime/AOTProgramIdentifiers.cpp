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
#include "AOTPropertyWalk.h"
#include "AOTRuntimeCounters.h"
#include "AOTThreshold.h"
#include "BuiltinNames.h"
#include "JSCInlines.h"
#include <algorithm>
#include <bit>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/StringHasherInlines.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramIdentifiers);

AOT_THRESHOLD(uint32_t, minNumberOfSymbolPlaces, 64);

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
ProgramIdentifiers::ProgramIdentifiers(VM& vm, const Image& image, ProgramIdentifiers* ofVM)
    : m_vm(vm)
    , m_image(image)
    , m_count(image.last().text().numberOfIdentifiers())
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    , m_text(image.text())
#else
    , m_atoms(image.text().identifierAtoms())
#endif
    , m_ofVM(ofVM)
    , m_realmsCreateTheirSymbols(!ofVM && image.programTables().flags & ProgramTables::mayRunInMultipleInstances)
    , m_symbols(createSymbols(ofVM ? minNumberOfSymbolPlaces : std::max(minNumberOfSymbolPlaces, WTF::roundUpToPowerOfTwo(static_cast<uint32_t>(image.programTables().identifiersOfEngineSymbols().size()) * 2 + 2))))
{
    RELEASE_ASSERT(&image == Image::current() && &image == &image.first());
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
    for (const Image* image = &m_image; image; image = image->above()) {
        for (uint32_t number : image->programTables().identifiersOfEngineSymbols())
            uid(number);
    }
}

NEVER_INLINE UniquedStringImpl* ProgramIdentifiers::uidSlowCase(uint32_t number)
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (m_text.stringRecords()[number].hasText())
        return static_cast<UniquedStringImpl*>(&m_text.ensureLazyHeader(number));
#endif
    if (UniquedStringImpl* uid = m_symbols.find(number))
        return uid;
    bool isOfRealm = isCreatedSymbol(number);
    RELEASE_ASSERT(!isOfRealm || !m_realmsCreateTheirSymbols);
    Identifier created = m_ofVM && !isOfRealm ? m_ofVM->identifier(number) : create(number);
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

bool ProgramIdentifiers::isCreatedSymbol(uint32_t number) const
{
    return m_image.holderOfStringHeader(number).text().identifier(number).kind() == PropertyKeyKind::CreatedSymbol;
}

Identifier ProgramIdentifiers::create(uint32_t number) const
{
    const ImageText& text = m_image.holderOfStringHeader(number).text();
    const ImageIdentifierWord& record = text.identifier(number);
    StringID string = record.text();
    switch (record.kind()) {
    case PropertyKeyKind::Text:
        if (!string)
            return Identifier();
        RELEASE_ASSERT(text.view(string).isEmpty());
        return Identifier(Identifier::EmptyIdentifierFlag::EmptyIdentifier);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
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
    {
        Locker locker { m_symbolsLock };
        if (UniquedStringImpl* uid = m_symbols.find(number))
            return uid;
    }
    return m_ofVM ? m_ofVM->tryGetIdentifierConcurrently(number) : nullptr;
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
        if (found != m_numbersOfSymbols.end())
            return found->value;
        return m_ofVM ? m_ofVM->numberOf(uid) : std::nullopt;
    }
    // The empty name is no atom of an image. Whichever image is the first to have it has numbered it.
    if (!uid->length()) {
        for (const Image* image = &m_image; image; image = image->above()) {
            if (std::optional<uint32_t> number = numberOfText(*image, StringView(*uid), uid->existingHash()))
                return number;
        }
        return std::nullopt;
    }
    ASSERT(!numberOfText(m_image, StringView(*uid), uid->existingHash()));
    return std::nullopt;
}

#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
// Names are short. The last word of each width ends where the characters end, and lies over the one in front of it where it has to.
static ALWAYS_INLINE bool areSameCharacters(const Latin1Character* first, const Latin1Character* second, size_t length)
{
    ASSERT(length);
    auto areSameAt = [&]<typename Word>(size_t offset, Word) ALWAYS_INLINE_LAMBDA {
        return WTF::unalignedLoad<Word>(first + offset) == WTF::unalignedLoad<Word>(second + offset);
    };
    if (length >= sizeof(uint64_t)) {
#pragma clang loop unroll(disable)
        for (size_t offset = 0; offset + sizeof(uint64_t) < length; offset += sizeof(uint64_t)) {
            if (!areSameAt(offset, uint64_t()))
                return false;
        }
        return areSameAt(length - sizeof(uint64_t), uint64_t());
    }
    if (length >= sizeof(uint32_t))
        return areSameAt(0, uint32_t()) && areSameAt(length - sizeof(uint32_t), uint32_t());
    if (length >= sizeof(uint16_t))
        return areSameAt(0, uint16_t()) && areSameAt(length - sizeof(uint16_t), uint16_t());
    return *first == *second;
}
#endif

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
    return IdentifierTable(image.programTables().identifiersByHash()).find(hash, [&](uint32_t number) ALWAYS_INLINE_LAMBDA {
        const UniquedStringImpl& atom = atoms[number];
        if (atom.length() != characters.length())
            return false;
        if (!atom.length())
            return true;
        if (atom.existingHash() != hash)
            return false;
        if (atom.is8Bit() && characters.is8Bit()) [[likely]]
            return areSameCharacters(atom.span8().data(), characters.span8().data(), characters.length());
        return StringView(atom) == characters;
    }, [&](uint32_t number) {
        const UniquedStringImpl& atom = atoms[number];
        if (!atom.length())
            return characters.isEmpty();
        return atom.existingHash() == hash && StringView(atom) == characters;
#endif
    });
}

// Every new atom comes through here, a thousand of them while a VM is created. The common answers need no frame: no name has the hash, or an 8-bit name is the text.
AtomStringImpl* ProgramIdentifiers::findAtom(StringView characters, unsigned hash)
{
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (const Image* image = Image::sharedByAllThreads(); image && !characters.isEmpty()) [[likely]] {
        std::optional<uint32_t> number = IdentifierTable(image->programTables().identifiersByHash()).inSlotFor(hash);
        if (!number)
            return nullptr;
        UniquedStringImpl& atom = image->text().identifierAtoms()[*number];
        if (atom.length() == characters.length() && atom.existingHash() == hash && atom.is8Bit() && characters.is8Bit() && areSameCharacters(atom.span8().data(), characters.span8().data(), characters.length())) [[likely]]
            return static_cast<AtomStringImpl*>(&atom);
    }
#endif
    return findAtomSlow(characters, hash);
}

NEVER_INLINE AtomStringImpl* ProgramIdentifiers::findAtomSlow(StringView characters, unsigned hash)
{
    const Image* imageOfThread = Image::current();
    if (characters.isEmpty() || !imageOfThread)
        return nullptr;
    const Image* holder = &imageOfThread->first();
    std::optional<uint32_t> number = numberOfText(*holder, characters, hash);
    while (!number && (holder = holder->above()))
        number = numberOfText(*holder, characters, hash);
    if (!number)
        return nullptr;
    const Image& image = *holder;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return static_cast<AtomStringImpl*>(&image.text().ensureLazyHeader(*number));
#else
    return static_cast<AtomStringImpl*>(image.text().identifierAtoms() + *number);
#endif
}

// The whole text up to 16 characters, the length and both ends of a longer one.
static ALWAYS_INLINE uint64_t fingerprintOf(std::span<const Latin1Character> characters)
{
    const Latin1Character* data = characters.data();
    size_t length = characters.size();
    uint64_t first;
    uint64_t last;
    if (length >= sizeof(uint64_t)) {
        first = unalignedLoad<uint64_t>(data);
        last = unalignedLoad<uint64_t>(data + length - sizeof(uint64_t));
    } else if (length >= sizeof(uint32_t)) {
        first = unalignedLoad<uint32_t>(data);
        last = unalignedLoad<uint32_t>(data + length - sizeof(uint32_t));
    } else {
        first = data[0];
        last = static_cast<uint64_t>(data[length / 2]) << 8 | data[length - 1];
    }
    uint64_t mixed = (first ^ length * 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
    mixed = ((mixed ^ mixed >> 32) ^ last) * 0xc4ceb9fe1a85ec53ull;
    return (mixed ^ mixed >> 29) | 1;
}

// Where the linker puts two literals decides whether they share a place by address, anew in every build. Two that do would put each other out at every turn.
NEVER_INLINE AtomStringImpl* ProgramIdentifiers::findAtomOfNativeTextByCharacters(std::span<const Latin1Character> characters)
{
    static_assert(hasOneBitSet(std::tuple_size_v<decltype(m_nativeTextsByCharacters)>));
    constexpr unsigned bitsOfSet = std::countr_zero(std::tuple_size_v<decltype(m_nativeTextsByCharacters)>);
    uint64_t fingerprint = fingerprintOf(characters);
    std::array<NativeTextFingerprintSet*, 2> sets { &m_nativeTextsByCharacters[fingerprint >> (64 - bitsOfSet)], &m_nativeTextsByCharacters[fingerprint << bitsOfSet >> (64 - bitsOfSet)] };
    for (NativeTextFingerprintSet* set : sets) {
        for (const NativeTextFingerprint& known : *set) {
            if (known.fingerprint != fingerprint)
                continue;
            if (!known.atom || (known.atom->length() == characters.size() && WTF::equal(known.atom->span8().data(), characters)))
                return known.atom;
        }
    }
    countNativeTextHashed(m_vm);
    AtomStringImpl* atom = findAtom(StringView(characters), StringHasher::computeHashAndMaskTop8Bits(characters));
    if (atom && !atom->is8Bit())
        atom = nullptr;
    NativeTextFingerprintSet& set = sets[0]->back().fingerprint && !sets[1]->back().fingerprint ? *sets[1] : *sets[0];
    std::move_backward(set.begin(), set.end() - 1, set.end());
    set.front() = { fingerprint, atom };
    return atom;
}

AtomStringImpl* ProgramIdentifiers::findAtomOfNativeText(std::span<const Latin1Character> characters)
{
    if (characters.empty() || characters.size() > String::MaxLength)
        return nullptr;
    static_assert(hasOneBitSet(std::tuple_size_v<decltype(m_recentNativeTexts)>));
    NativeText& recent = m_recentNativeTexts[std::bit_cast<uintptr_t>(characters.data()) * 0x9e3779b97f4a7c15ull >> (64 - std::countr_zero(m_recentNativeTexts.size()))];
    if (recent.characters == characters.data() && recent.length == characters.size()) [[likely]] {
        // The address may hold other characters by now. Whoever gets null asks the atom table, which is right whatever stands here.
        if (!recent.atom)
            return nullptr;
        if (WTF::equal(recent.atom->span8().data(), characters)) [[likely]]
            return recent.atom;
    }
    AtomStringImpl* atom = findAtomOfNativeTextByCharacters(characters);
    recent = { characters.data(), characters.size(), atom };
    return atom;
}

ProgramIdentifiers* identifiersOfRealm(VM& vm, JSGlobalObject* realm)
{
    if (Instance* instance = realm ? realm->aotInstance() : nullptr)
        return &identifiersOf(*instance);
    return vm.aotState().identifiers.get();
}

namespace PropertyWalk {

UniquedStringImpl* tryFindName(VM& vm, std::span<const Latin1Character> characters)
{
    ProgramIdentifiers* identifiers = vm.aotState().identifiers.get();
    return identifiers ? identifiers->findAtomOfNativeText(characters) : nullptr;
}

} // namespace PropertyWalk

String ProgramIdentifiers::textOf(const Image& image, uint32_t number)
{
    const ImageText& text = image.holderOfStringHeader(number).text();
    const ImageIdentifierWord& record = text.identifier(number);
    if (record.kind() != PropertyKeyKind::Text || !record.text())
        return String();
    return text.createAtom(record.text());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
