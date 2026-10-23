/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIdentifierTable.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramIdentifiers.h"
#include "AOTVector.h"
#include "BuiltinNames.h"
#include "Identifier.h"
#include "IdentifierInlines.h"
#include "JSCInlines.h"
#include <atomic>
#include <optional>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> programIdentifiersTests();

static constexpr ASCIILiteral programWithTextNames =
    "layout L0 name=\"Names\" fields=[{key=\"alpha\" declared={type=number}}, {key=\"beta\" declared={type=number}}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]\n"
    "    Return v1\n"
    "}\n"_s;

static constexpr ASCIILiteral programWithSymbols =
    "layout L0 name=\"Names\" fields=["
    "{key=\"alpha\" declared={type=number}}, "
    "{key=\"\" declared={type=number}}, "
    "{key=@\"then\" declared={type=number}}, "
    "{key=@\"nameOnlyThisProgramHas\" declared={type=number}}, "
    "{key=#\"secret\"@L0 declared={type=number} flags=[IsPrivate]}, "
    "{key=symbol(\"described\")@m0:120 declared={type=number}}, "
    "{key=symbol@m0:121 declared={type=number}}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0, v0, v0, v0, v0, v0 layout=L0 fields=[0, 1, 2, 3, 4, 5, 6]\n"
    "    Return v1\n"
    "}\n"_s;

static const ImageText& imageText()
{
    return Image::current()->text();
}

static uint32_t findNumber(const Invocable<bool(const ImageIdentifierWord&)> auto& matches)
{
    std::optional<uint32_t> found;
    for (uint32_t number = 1; number < imageText().numberOfIdentifiers(); ++number) {
        if (!matches(imageText().identifier(number)))
            continue;
        AOT_TEST_CHECK(!found);
        found = number;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

static uint32_t findNumber(PropertyKeyKind kind, ASCIILiteral text)
{
    return findNumber([&](const ImageIdentifierWord& record) {
        return record.kind() == kind && record.text() && imageText().equals(record.text(), text);
    });
}

static uint32_t findNumber(WellKnownSymbol symbol)
{
    return findNumber([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::WellKnownSymbol && record.wellKnownSymbol() == symbol;
    });
}

static uint32_t findNumberOfSymbolWithoutDescription()
{
    return findNumber([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::CreatedSymbol && !record.text();
    });
}

// Every other number has a header without a length: the run time makes or finds that name.
static bool isAtomOfImage(uint32_t number)
{
    const ImageIdentifierWord& record = imageText().identifier(number);
    return record.kind() == PropertyKeyKind::Text && record.text() && !imageText().view(record.text()).isEmpty();
}

static String copyOfText(uint32_t number)
{
    AOT_TEST_CHECK(isAtomOfImage(number));
    String copy = imageText().view(imageText().identifier(number).text()).toString();
    AOT_TEST_CHECK(!copy.impl()->isAtom() && !copy.impl()->isStatic());
    return copy;
}

static const SymbolImpl& symbolOf(const Identifier& identifier)
{
    AOT_TEST_CHECK(identifier.impl()->isSymbol());
    return uncheckedDowncast<SymbolImpl>(*identifier.impl());
}

static UniquedStringImpl* atomOfImage(uint32_t number)
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return static_cast<UniquedStringImpl*>(&imageText().ensureLazyHeader(number));
}

static bool hasNoHeader(uint32_t number)
{
    return !imageText().stringRecords()[number].hasText() && !imageText().tryGetLazyHeader(number);
#else
    return imageText().identifierAtoms() + number;
}

static bool hasNoHeader(uint32_t number)
{
    return !atomOfImage(number)->length();
#endif
}

static void testOnlySymbolsStartAbsent(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    AOT_TEST_CHECK(table.count() == imageText().numberOfIdentifiers());
    AOT_TEST_CHECK(table.count() > 2);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    AOT_TEST_CHECK(!table.symbols().count);
    for (uint32_t number = 0; number < table.count(); ++number) {
        AOT_TEST_CHECK(!table.symbols().find(number));
        if (!isAtomOfImage(number)) {
            AOT_TEST_CHECK(hasNoHeader(number));
            AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(number));
            continue;
        }
        UniquedStringImpl* header = atomOfImage(number);
        AOT_TEST_CHECK(imageText().tryGetLazyHeader(number) == header && imageText().lazyHeaderNumber(*header) == number);
#else
    AOT_TEST_CHECK(table.identifiers() == imageText().identifierAtoms());
    AOT_TEST_CHECK(!table.symbols().count);
    for (uint32_t number = 0; number < table.count(); ++number) {
        UniquedStringImpl* header = table.identifiers() + number;
        AOT_TEST_CHECK(!table.symbols().find(number));
        if (!isAtomOfImage(number)) {
            AOT_TEST_CHECK(!header->length());
            AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(number));
            continue;
        }
#endif
        AOT_TEST_CHECK(header->isStatic() && header->isAtom() && !header->isSymbol());
        AOT_TEST_CHECK(StringView(*header) == imageText().view(imageText().identifier(number).text()));
        AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(number) == header);
    }
}

static void testTextNameIsAtomOfImage(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t alpha = findNumber(PropertyKeyKind::Text, "alpha"_s);
    uint32_t beta = findNumber(PropertyKeyKind::Text, "beta"_s);
    UniquedStringImpl* atom = atomOfImage(alpha);

    AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(alpha) == atom);
    AOT_TEST_CHECK(table.uid(alpha) == atom);

    Identifier name = table.identifier(alpha);
    AOT_TEST_CHECK(name.string().string() == "alpha"_s);
    AOT_TEST_CHECK(name.impl() == atom);
    AOT_TEST_CHECK(table.identifier(alpha).impl() == atom);
    AOT_TEST_CHECK(table.uid(beta) == atomOfImage(beta) && atomOfImage(beta) != atom);
    AOT_TEST_CHECK(!table.symbols().count);
}

static void testTextMadeAtRunTimeIsAtomOfImage(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers& table = *vm.aotState().identifiers;
    uint32_t alpha = findNumber(PropertyKeyKind::Text, "alpha"_s);
    UniquedStringImpl* atom = atomOfImage(alpha);
    static constexpr char16_t wideCharacters[] = { 'a', 'l', 'p', 'h', 'a' };
    String copy = copyOfText(alpha);
    String longer = makeString(copy, "Omega"_s);

    AOT_TEST_CHECK(Identifier::fromString(vm, "alpha"_s).impl() == atom);
    AOT_TEST_CHECK(Identifier::fromString(vm, copy).impl() == atom);
    AOT_TEST_CHECK(AtomStringImpl::add("alpha"_span).get() == atom);
    AOT_TEST_CHECK(AtomStringImpl::add(std::span<const char16_t> { wideCharacters }).get() == atom);
    AOT_TEST_CHECK(AtomStringImpl::add(longer.impl(), 0, 5).get() == atom);
    AOT_TEST_CHECK(AtomStringImpl::lookUp("alpha"_span8).get() == atom);
    AOT_TEST_CHECK(AtomStringImpl::lookUp(std::span<const char16_t> { wideCharacters }).get() == atom);
    AOT_TEST_CHECK(AtomStringImpl::lookUp(copy.impl()).get() == atom);
    AOT_TEST_CHECK(!copy.impl()->isAtom());

    AOT_TEST_CHECK(ProgramIdentifiers::findAtom(copy, copy.impl()->hash()) == atom);
    AOT_TEST_CHECK(!ProgramIdentifiers::findAtom(longer, longer.impl()->hash()));
    AOT_TEST_CHECK(!ProgramIdentifiers::findAtom(emptyString(), emptyString().impl()->hash()));
}

static void testUnnamedNumberZero(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());

    auto checkNumberZero = [&] {
        AOT_TEST_CHECK(!table.uid(0));
        AOT_TEST_CHECK(table.identifier(0).isNull());
        AOT_TEST_CHECK(hasNoHeader(0));
        AOT_TEST_CHECK(!table.symbols().find(0));
        AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(0));
    };

    checkNumberZero();
    table.createEngineSymbols();
    checkNumberZero();
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), 0).isNull());
}

static void testReadFromAnotherThread(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t iterator = findNumber(WellKnownSymbol::Iterator);
    UniquedStringImpl* symbol = table.uid(iterator);

    Vector<UniquedStringImpl*> results(FillWith { }, table.count(), nullptr);
    Thread::create("program-identifiers-tests"_s, [&] {
        for (uint32_t number = 0; number < table.count(); ++number)
            results[number] = table.tryGetIdentifierConcurrently(number);
    })->waitForCompletion();

    for (uint32_t number = 0; number < table.count(); ++number) {
        if (number == iterator)
            AOT_TEST_CHECK(results[number] == symbol);
        else if (isAtomOfImage(number))
            AOT_TEST_CHECK(results[number] == atomOfImage(number));
        else
            AOT_TEST_CHECK(!results[number]);
    }
}

static unsigned countCompleteEntries(const ProgramIdentifiers& table)
{
    unsigned count = 0;
    for (uint32_t number = 1; number < table.count(); ++number) {
        UniquedStringImpl* uid = table.tryGetIdentifierConcurrently(number);
        if (!uid)
            continue;
        ++count;
        AOT_TEST_CHECK(uid->isSymbol() || uid->isAtom());
        AOT_TEST_CHECK(table.numberOf(uid) == number);
    }
    return count;
}

static void testReaderSeesNullOrCompleteEntry(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    std::atomic<bool> hasCreatedAll { false };
    unsigned lastCount = 0;
    Ref<Thread> reader = Thread::create("program-identifiers-tests"_s, [&] {
        bool isLastRound = false;
        while (!isLastRound) {
            isLastRound = hasCreatedAll.load();
            unsigned count = countCompleteEntries(table);
            AOT_TEST_CHECK(count >= lastCount);
            lastCount = count;
        }
    });
    for (uint32_t number = 1; number < table.count(); ++number)
        table.uid(number);
    hasCreatedAll = true;
    reader->waitForCompletion();
    AOT_TEST_CHECK(lastCount == table.count() - 1);
}

static void testNumberOfTextNeedsNoCreation(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t alpha = findNumber(PropertyKeyKind::Text, "alpha"_s);
    Identifier name = Identifier::fromString(vm, "alpha"_s);

    AOT_TEST_CHECK(table.numberOf(name.impl()) == alpha);
    AOT_TEST_CHECK(table.numberOf(atomOfImage(alpha)) == alpha);
    AOT_TEST_CHECK(!table.symbols().count);
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), alpha) == "alpha"_s);
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), alpha).impl() == name.impl());
}

static void testNameOutsideProgramHasNoNumber(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    Identifier name = Identifier::fromString(vm, "nameThatNoProgramHas"_s);
    Ref<SymbolImpl> symbol = SymbolImpl::create(*name.impl());
    AOT_TEST_CHECK(!name.impl()->isStatic());

    auto checkBothHaveNoNumber = [&] {
        AOT_TEST_CHECK(!table.numberOf(name.impl()));
        AOT_TEST_CHECK(!table.numberOf(symbol.ptr()));
    };

    checkBothHaveNoNumber();
    for (uint32_t number = 1; number < table.count(); ++number)
        table.uid(number);
    checkBothHaveNoNumber();
}

// Every name in a slot of the table is found by a copy of its text, and two names share a bucket.
static void testNamesOfOneBucket(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers& table = *vm.aotState().identifiers;
    IdentifierTable byHash(Image::current()->programTables().identifiersByHash());
    AOT_TEST_CHECK(!byHash.isEmpty() && hasOneBitSet(byHash.header().numberOfBuckets));
    Vector<unsigned> namesInBucket(FillWith { }, byHash.header().numberOfBuckets, 0u);
    unsigned mostNamesInBucket = 0;
    for (uint32_t entry : byHash.slots()) {
        if (!entry || !isAtomOfImage(byHash.numberIn(entry)))
            continue;
        uint32_t number = byHash.numberIn(entry);
        UniquedStringImpl* atom = atomOfImage(number);
        mostNamesInBucket = std::max(mostNamesInBucket, ++namesInBucket[atom->existingHash() & (byHash.header().numberOfBuckets - 1)]);
        String copy = copyOfText(number);
        AOT_TEST_CHECK(atom->existingHash() == copy.impl()->hash() && imageText().hash(imageText().identifier(number).text()) == atom->existingHash());
        AOT_TEST_CHECK(ProgramIdentifiers::findAtom(copy, copy.impl()->hash()) == atom);
        AOT_TEST_CHECK(Identifier::fromString(vm, copy).impl() == atom);
        AOT_TEST_CHECK(table.numberOf(atom) == number);
    }
    AOT_TEST_CHECK(mostNamesInBucket > 1);
}

static void testTableOfIdentifiers()
{
    AOT_TEST_CHECK(IdentifierTable::create({ }).isEmpty());
    AOT_TEST_CHECK(!IdentifierTable({ }).find(7, [](uint32_t) { return true; }));

    // Hashes of 24 bits. Three identifiers have one hash, and some hashes differ only above the bits that choose the bucket, or only below the bits that the entry keeps.
    Vector<IdentifierTable::Key> keys;
    for (uint32_t number = 1; number <= 5000; ++number)
        keys.append({ (number * 0x9e3779b1u) >> 8, number });
    keys.appendList<IdentifierTable::Key>({ { 0x123456, 6000 }, { 0x123456, 6001 }, { 0x123456, 6002 }, { 0x923456, 6003 }, { 0x123457, 6004 }, { 0, 6005 }, { 0xffffff, 70000 } });
    Vector<uint32_t> words = IdentifierTable::create(keys.span());
    IdentifierTable table(words.span());
    AOT_TEST_CHECK(table.header().numberOfOthers == 2 && table.others()[0].number == 6001 && table.others()[1].number == 6002);
    AOT_TEST_CHECK(table.header().numberOfSlots < keys.size() * 3 / 2);
    AOT_TEST_CHECK(words.size() == IdentifierTable::wordsInHeader + table.header().numberOfSlots + (table.header().numberOfBuckets + 1) / 2 + 2 * table.header().numberOfOthers);
    for (const IdentifierTable::Key& key : keys) {
        unsigned numberOfLooks = 0;
        std::optional<uint32_t> found = table.find(key.hash, [&](uint32_t number) {
            ++numberOfLooks;
            return number == key.number;
        });
        AOT_TEST_CHECK(found == key.number && numberOfLooks <= (key.hash == 0x123456 ? 3u : 1u));
    }
    // A text that no identifier has: with a hash of the table every identifier of that hash is looked at, with another hash next to none.
    unsigned numberOfLooks = 0;
    auto isNoText = [&](uint32_t) {
        ++numberOfLooks;
        return false;
    };
    AOT_TEST_CHECK(!table.find(0x123456, isNoText) && numberOfLooks == 3);
    numberOfLooks = 0;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> hashes;
    for (const IdentifierTable::Key& key : keys)
        hashes.add(key.hash);
    unsigned numberOfSearches = 0;
    for (uint32_t hash = 1; hash < 1u << 24; hash += 1009) {
        if (hashes.contains(hash))
            continue;
        ++numberOfSearches;
        AOT_TEST_CHECK(!table.find(hash, isNoText));
    }
    AOT_TEST_CHECK(numberOfSearches > 16000 && numberOfLooks < numberOfSearches / 100);
}

static void testWellKnownSymbolIsTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t iterator = findNumber(WellKnownSymbol::Iterator);
    uint32_t hasInstance = findNumber(WellKnownSymbol::HasInstance);

    AOT_TEST_CHECK(!table.numberOf(vm.propertyNames->iteratorSymbol.impl()));
    AOT_TEST_CHECK(hasNoHeader(iterator));
    AOT_TEST_CHECK(table.identifier(iterator) == vm.propertyNames->iteratorSymbol);
    AOT_TEST_CHECK(table.symbols().find(iterator) == vm.propertyNames->iteratorSymbol.impl());
    AOT_TEST_CHECK(table.identifier(hasInstance) == vm.propertyNames->hasInstanceSymbol);
    AOT_TEST_CHECK(table.numberOf(vm.propertyNames->iteratorSymbol.impl()) == iterator);
    AOT_TEST_CHECK(table.numberOf(vm.propertyNames->hasInstanceSymbol.impl()) == hasInstance);
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), iterator).isNull());
}

static void testRegisteredSymbolIsTheRegistrys(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t number = findNumber(PropertyKeyKind::RegisteredSymbol, "nodejs.util.inspect.custom"_s);
    Ref<RegisteredSymbolImpl> symbol = vm.symbolRegistry().symbolForKey("nodejs.util.inspect.custom"_s);

    AOT_TEST_CHECK(!table.numberOf(symbol.ptr()));
    AOT_TEST_CHECK(table.identifier(number).impl() == symbol.ptr());
    AOT_TEST_CHECK(table.numberOf(symbol.ptr()) == number);
}

static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void testEngineSymbolsAreCreatedTogether(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    table.createEngineSymbols();

    for (uint32_t number = 1; number < table.count(); ++number) {
        UniquedStringImpl* created = table.symbols().find(number);
        AOT_TEST_CHECK(!!created == isEngineSymbol(imageText().identifier(number).kind()));
        if (created)
            AOT_TEST_CHECK(table.numberOf(created) == number);
    }
    AOT_TEST_CHECK(table.numberOf(vm.propertyNames->iteratorSymbol.impl()) == findNumber(WellKnownSymbol::Iterator));
    AOT_TEST_CHECK(table.numberOf(vm.propertyNames->builtinNames().thenPrivateName().impl()) == findNumber(PropertyKeyKind::EnginePrivateName, "then"_s));
}

static void testStartUpCreatesEngineSymbols(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers& table = *vm.aotState().identifiers;
    AOT_TEST_CHECK(table.numberOf(vm.propertyNames->iteratorSymbol.impl()) == findNumber(WellKnownSymbol::Iterator));
}

static void testEnginePrivateNameIsTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t then = findNumber(PropertyKeyKind::EnginePrivateName, "then"_s);

    AOT_TEST_CHECK(table.identifier(then) == vm.propertyNames->builtinNames().thenPrivateName());
    AOT_TEST_CHECK(symbolOf(table.identifier(then)).isPrivate());
    AOT_TEST_CHECK(table.numberOf(table.identifier(then).impl()) == then);
}

static void testEnginePrivateNameIsAddedToTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t number = findNumber(PropertyKeyKind::EnginePrivateName, "nameOnlyThisProgramHas"_s);

    Identifier name = table.identifier(number);
    AOT_TEST_CHECK(symbolOf(name).isPrivate());
    AOT_TEST_CHECK(StringView(name.impl()) == "nameOnlyThisProgramHas"_s);
    AOT_TEST_CHECK(vm.propertyNames->builtinNames().lookUpPrivateName(String("nameOnlyThisProgramHas"_s)) == name.impl());
    AOT_TEST_CHECK(enginePrivateName(vm, "nameOnlyThisProgramHas"_s) == name);
    AOT_TEST_CHECK(vm.aotState().identifiers->identifier(number) == name);
}

static void testPrivateNameIsPrivateSymbol(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t number = findNumber(PropertyKeyKind::PrivateName, "secret"_s);

    Identifier name = table.identifier(number);
    AOT_TEST_CHECK(symbolOf(name).isPrivate());
    AOT_TEST_CHECK(StringView(name.impl()) == "secret"_s);
    AOT_TEST_CHECK(table.numberOf(name.impl()) == number);
    AOT_TEST_CHECK(!vm.propertyNames->builtinNames().lookUpPrivateName(String("secret"_s)));
}

static void testCreatedSymbolHasItsDescription(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t described = findNumber(PropertyKeyKind::CreatedSymbol, "described"_s);
    uint32_t undescribed = findNumberOfSymbolWithoutDescription();

    const SymbolImpl& describedSymbol = symbolOf(table.identifier(described));
    AOT_TEST_CHECK(!describedSymbol.isPrivate() && !describedSymbol.isRegistered() && !describedSymbol.isNullSymbol());
    AOT_TEST_CHECK(StringView(describedSymbol) == "described"_s);

    const SymbolImpl& undescribedSymbol = symbolOf(table.identifier(undescribed));
    AOT_TEST_CHECK(!undescribedSymbol.isPrivate() && !undescribedSymbol.isRegistered() && undescribedSymbol.isNullSymbol());

    AOT_TEST_CHECK(table.numberOf(table.identifier(described).impl()) == described);
    AOT_TEST_CHECK(table.numberOf(table.identifier(undescribed).impl()) == undescribed);
}

static void testSymbolIsCreatedOnce(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t numbers[] = {
        findNumber(PropertyKeyKind::CreatedSymbol, "described"_s),
        findNumberOfSymbolWithoutDescription(),
        findNumber(PropertyKeyKind::PrivateName, "secret"_s),
    };

    Vector<UniquedStringImpl*, 3> symbols;
    for (uint32_t number : numbers) {
        AOT_TEST_CHECK(hasNoHeader(number));
        AOT_TEST_CHECK(!table.symbols().find(number));
        AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(number));

        UniquedStringImpl* symbol = table.uid(number);
        AOT_TEST_CHECK(symbol->isSymbol());
        AOT_TEST_CHECK(table.uid(number) == symbol);
        AOT_TEST_CHECK(table.identifier(number).impl() == symbol);
        AOT_TEST_CHECK(table.symbols().find(number) == symbol);
        AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(number) == symbol);
        AOT_TEST_CHECK(!symbols.contains(symbol));
        symbols.append(symbol);
    }
    AOT_TEST_CHECK(table.symbols().count == symbols.size());
}

static void testEmptyTextIsTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t number = findNumber(PropertyKeyKind::Text, ""_s);
    UniquedStringImpl* empty = vm.propertyNames->emptyIdentifier.impl();
    AOT_TEST_CHECK(empty == StringImpl::empty());

    AOT_TEST_CHECK(hasNoHeader(number));
    AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(number));
    AOT_TEST_CHECK(table.numberOf(empty) == number);

    AOT_TEST_CHECK(table.uid(number) == empty);
    AOT_TEST_CHECK(table.identifier(number) == vm.propertyNames->emptyIdentifier);
    AOT_TEST_CHECK(table.symbols().find(number) == empty);
    AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(number) == empty);
    AOT_TEST_CHECK(table.numberOf(empty) == number);

    String text = ProgramIdentifiers::textOf(*Image::current(), number);
    AOT_TEST_CHECK(!text.isNull() && text.isEmpty());
}

static constexpr ASCIILiteral tableOfSymbolsGrowsTest = "program-identifiers/table-of-symbols-grows"_s;
static constexpr unsigned numberOfLayoutsWithSymbols = 8;
static constexpr unsigned numberOfSymbolsPerLayout = 16;

static Vector<char> createProgramWithManySymbols()
{
    StringBuilder builder;
    for (unsigned layout = 0; layout < numberOfLayoutsWithSymbols; ++layout) {
        builder.append("layout L"_s, layout, " name=\"Symbols"_s, layout, "\" fields=["_s);
        for (unsigned field = 0; field < numberOfSymbolsPerLayout; ++field)
            builder.append(field ? ", "_s : ""_s, "{key=symbol@m0:"_s, 100 + layout * numberOfSymbolsPerLayout + field, " declared={type=number}}"_s);
        builder.append("]\n"_s);
    }
    builder.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
        "entry module=m0\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Integer integer=1\n"_s);
    for (unsigned layout = 0; layout < numberOfLayoutsWithSymbols; ++layout) {
        builder.append("    v"_s, layout + 1, " = NewObject -"_s);
        for (unsigned field = 0; field < numberOfSymbolsPerLayout; ++field)
            builder.append(", v0"_s);
        builder.append(" layout=L"_s, layout, " fields=["_s);
        for (unsigned field = 0; field < numberOfSymbolsPerLayout; ++field)
            builder.append(field ? ", "_s : ""_s, field);
        builder.append("]\n"_s);
    }
    builder.append("    v"_s, numberOfLayoutsWithSymbols + 1, " = NewArray "_s);
    for (unsigned layout = 0; layout < numberOfLayoutsWithSymbols; ++layout)
        builder.append(layout ? ", v"_s : "v"_s, layout + 1);
    builder.append(" contents=anything\n    Return v"_s, numberOfLayoutsWithSymbols + 1, "\n}\n"_s);
    // String::ascii() makes a question mark of every line break.
    String text = builder.toString();
    Vector<char> characters;
    for (Latin1Character character : text.span8())
        characters.append(static_cast<char>(character));
    characters.append('\0');
    return characters;
}

static ASCIILiteral programWithManySymbols()
{
    static NeverDestroyed<Vector<char>> text(createProgramWithManySymbols());
    return ASCIILiteral::fromLiteralUnsafe(text->span().data());
}

static void checkTableOfSymbolsGrows(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    const ProgramSymbols& symbols = table.symbols();
    uint32_t numberOfPlacesAtStart = symbols.mask + 1;

    Vector<uint32_t> numbers;
    for (uint32_t number = 1; number < table.count(); ++number) {
        if (!isAtomOfImage(number))
            numbers.append(number);
    }
    AOT_TEST_CHECK(numbers.size() >= numberOfLayoutsWithSymbols * numberOfSymbolsPerLayout);
    AOT_TEST_CHECK(numbers.size() > numberOfPlacesAtStart / 2);

    Vector<UniquedStringImpl*> uids;
    for (uint32_t number : numbers) {
        uids.append(table.uid(number));
        AOT_TEST_CHECK(symbols.count == uids.size());
        AOT_TEST_CHECK(symbols.count * 2 <= symbols.mask + 1);
    }
    AOT_TEST_CHECK(symbols.mask + 1 > numberOfPlacesAtStart);

    for (size_t index = 0; index < numbers.size(); ++index) {
        AOT_TEST_CHECK(symbols.find(numbers[index]) == uids[index]);
        AOT_TEST_CHECK(table.uid(numbers[index]) == uids[index]);
        AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(numbers[index]) == uids[index]);
        AOT_TEST_CHECK(table.numberOf(uids[index]) == numbers[index]);
    }
    AOT_TEST_CHECK(symbols.count == numbers.size());
}

static void testTableOfSymbolsGrows()
{
    runWithLoadedProgram(tableOfSymbolsGrowsTest, programWithManySymbols(), checkTableOfSymbolsGrows);
}

template<LoadedProgramTest body>
static constexpr void (*withTextNames)() = runNativeTestWithLoadedProgram<programIdentifiersTests, programWithTextNames, body>;

template<LoadedProgramTest body>
static constexpr void (*withSymbols)() = runNativeTestWithLoadedProgram<programIdentifiersTests, programWithSymbols, body>;

std::span<const NativeTest> programIdentifiersTests()
{
    static constexpr NativeTest tests[] = {
        { "program-identifiers/only-symbols-start-absent"_s, withTextNames<testOnlySymbolsStartAbsent> },
        { "program-identifiers/text-name-is-atom-of-image"_s, withTextNames<testTextNameIsAtomOfImage> },
        { "program-identifiers/text-made-at-run-time-is-atom-of-image"_s, withTextNames<testTextMadeAtRunTimeIsAtomOfImage> },
        { "program-identifiers/unnamed-number-zero"_s, withTextNames<testUnnamedNumberZero> },
        { "program-identifiers/read-from-another-thread"_s, withTextNames<testReadFromAnotherThread> },
        { "program-identifiers/reader-sees-null-or-complete-entry"_s, withTextNames<testReaderSeesNullOrCompleteEntry> },
        { "program-identifiers/number-of-text-needs-no-creation"_s, withTextNames<testNumberOfTextNeedsNoCreation> },
        { "program-identifiers/name-outside-program-has-no-number"_s, withTextNames<testNameOutsideProgramHasNoNumber> },
        { "program-identifiers/names-of-one-bucket"_s, withTextNames<testNamesOfOneBucket> },
        { "program-identifiers/table-of-identifiers"_s, testTableOfIdentifiers },
        { "program-identifiers/well-known-symbol-is-the-engines"_s, withTextNames<testWellKnownSymbolIsTheEngines> },
        { "program-identifiers/registered-symbol-is-the-registrys"_s, withTextNames<testRegisteredSymbolIsTheRegistrys> },
        { "program-identifiers/start-up-creates-engine-symbols"_s, withTextNames<testStartUpCreatesEngineSymbols> },
        { "program-identifiers/engine-symbols-are-created-together"_s, withSymbols<testEngineSymbolsAreCreatedTogether> },
        { "program-identifiers/engine-private-name-is-the-engines"_s, withSymbols<testEnginePrivateNameIsTheEngines> },
        { "program-identifiers/engine-private-name-is-added-to-the-engines"_s, withSymbols<testEnginePrivateNameIsAddedToTheEngines> },
        { "program-identifiers/private-name-is-private-symbol"_s, withSymbols<testPrivateNameIsPrivateSymbol> },
        { "program-identifiers/created-symbol-has-its-description"_s, withSymbols<testCreatedSymbolHasItsDescription> },
        { "program-identifiers/symbol-is-created-once"_s, withSymbols<testSymbolIsCreatedOnce> },
        { "program-identifiers/empty-text-is-the-engines"_s, withSymbols<testEmptyTextIsTheEngines> },
        { tableOfSymbolsGrowsTest, testTableOfSymbolsGrows },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
