/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramIdentifiers.h"
#include "BuiltinNames.h"
#include "Identifier.h"
#include "IdentifierInlines.h"
#include "JSCInlines.h"
#include <atomic>
#include <optional>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
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
    "    v1 = NewObject -, v0, v0, v0, v0, v0, v0 layout=L0 fields=[0, 1, 2, 3, 4, 5]\n"
    "    Return v1\n"
    "}\n"_s;

static const ImageText& imageText()
{
    return Image::current()->text();
}

static uint32_t findNumber(const Invocable<bool(const ImageIdentifier&)> auto& matches)
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
    return findNumber([&](const ImageIdentifier& record) {
        return record.kind() == kind && record.text() && imageText().equals(record.text(), text);
    });
}

static uint32_t findNumber(WellKnownSymbol symbol)
{
    return findNumber([&](const ImageIdentifier& record) {
        return record.kind() == PropertyKeyKind::WellKnownSymbol && record.hashOrSymbol() == static_cast<uint32_t>(symbol);
    });
}

static uint32_t findNumberOfSymbolWithoutDescription()
{
    return findNumber([&](const ImageIdentifier& record) {
        return record.kind() == PropertyKeyKind::CreatedSymbol && !record.text();
    });
}

static Identifier atomOf(VM& vm, uint32_t number)
{
    const ImageIdentifier& record = imageText().identifier(number);
    AOT_TEST_CHECK(record.kind() == PropertyKeyKind::Text && record.text());
    return Identifier::fromString(vm, imageText().createAtom(record.text()));
}

static const SymbolImpl& symbolOf(const Identifier& identifier)
{
    AOT_TEST_CHECK(identifier.impl()->isSymbol());
    return uncheckedDowncast<SymbolImpl>(*identifier.impl());
}

static void testTableStartsEmpty(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    AOT_TEST_CHECK(table.count() == imageText().numberOfIdentifiers());
    AOT_TEST_CHECK(table.count() > 2);
    for (uint32_t number = 0; number < table.count(); ++number) {
        AOT_TEST_CHECK(!table.identifiers()[number]);
        AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(number));
    }
}

static void testFirstUseCreates(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t alpha = findNumber(PropertyKeyKind::Text, "alpha"_s);
    uint32_t beta = findNumber(PropertyKeyKind::Text, "beta"_s);

    AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(alpha));
    AOT_TEST_CHECK(!table.identifiers()[alpha]);

    const Identifier& name = table.identifier(alpha);
    AOT_TEST_CHECK(name.string().string() == "alpha"_s);
    AOT_TEST_CHECK(name.impl() == Identifier::fromString(program.vm(), "alpha"_s).impl());
    AOT_TEST_CHECK(table.tryGetIdentifierConcurrently(alpha)->impl() == name.impl());
    AOT_TEST_CHECK(table.identifiers()[alpha] == name.impl());
    AOT_TEST_CHECK(&table.identifier(alpha) == &name);
    AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(beta));
}

static void testUnnamedNumberZero(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());

    auto checkNumberZero = [&] {
        AOT_TEST_CHECK(table.identifier(0).isNull());
        AOT_TEST_CHECK(!table.identifiers()[0]);
        AOT_TEST_CHECK(!table.tryGetIdentifierConcurrently(0));
    };

    checkNumberZero();
    table.identifier(1);
    table.identifier(2);
    checkNumberZero();
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), 0).isNull());
}

static void testReadFromAnotherThread(LoadedProgram& program)
{
    ProgramIdentifiers table(program.vm(), *Image::current());
    uint32_t alpha = findNumber(PropertyKeyKind::Text, "alpha"_s);
    const Identifier& name = table.identifier(alpha);

    Vector<const Identifier*> results(FillWith { }, table.count(), nullptr);
    Thread::create("program-identifiers-tests"_s, [&] {
        for (uint32_t number = 0; number < table.count(); ++number)
            results[number] = table.tryGetIdentifierConcurrently(number);
    })->waitForCompletion();

    for (uint32_t number = 0; number < table.count(); ++number) {
        if (number == alpha)
            AOT_TEST_CHECK(results[number] && results[number]->impl() == name.impl());
        else
            AOT_TEST_CHECK(!results[number]);
    }
}

static unsigned countCompleteEntries(const ProgramIdentifiers& table)
{
    unsigned count = 0;
    for (uint32_t number = 1; number < table.count(); ++number) {
        const Identifier* identifier = table.tryGetIdentifierConcurrently(number);
        if (!identifier)
            continue;
        ++count;
        AOT_TEST_CHECK(identifier->impl()->isSymbol() || identifier->impl()->isAtom());
        AOT_TEST_CHECK(table.numberOf(identifier->impl()) == number);
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
        table.identifier(number);
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
    AOT_TEST_CHECK(!table.identifiers()[alpha]);

    AOT_TEST_CHECK(table.identifier(alpha).impl() == name.impl());
    AOT_TEST_CHECK(table.numberOf(name.impl()) == alpha);
    AOT_TEST_CHECK(ProgramIdentifiers::textOf(*Image::current(), alpha) == "alpha"_s);
}

static void testNameOutsideProgramHasNoNumber(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    Identifier name = Identifier::fromString(vm, "nameThatNoProgramHas"_s);
    Ref<SymbolImpl> symbol = SymbolImpl::create(*name.impl());

    auto checkBothHaveNoNumber = [&] {
        AOT_TEST_CHECK(!table.numberOf(name.impl()));
        AOT_TEST_CHECK(!table.numberOf(symbol.ptr()));
    };

    checkBothHaveNoNumber();
    for (uint32_t number = 1; number < table.count(); ++number)
        table.identifier(number);
    checkBothHaveNoNumber();
}

namespace {

struct NamesWithOneHashSlot {
    uint32_t inItsSlot;
    uint32_t displaced;
};

} // anonymous namespace

static NamesWithOneHashSlot findNamesWithOneHashSlot()
{
    std::span<const uint32_t> numbersByHash = Image::current()->programTables().identifiersByHash();
    AOT_TEST_CHECK(hasOneBitSet(numbersByHash.size()));
    uint32_t mask = static_cast<uint32_t>(numbersByHash.size() - 1);
    auto slotOf = [&](uint32_t number) {
        return imageText().identifier(number).hashOrSymbol() & mask;
    };
    for (uint32_t slot = 0; slot < numbersByHash.size(); ++slot) {
        if (!numbersByHash[slot])
            continue;
        uint32_t number = numbersByHash[slot] - 1;
        if (slotOf(number) == slot)
            continue;
        uint32_t first = numbersByHash[slotOf(number)] - 1;
        if (slotOf(first) == slotOf(number))
            return { first, number };
    }
    failNativeTest(__FILE__, __LINE__, "two names of the image have one hash slot");
}

static void testNamesWithOneHashSlot(LoadedProgram& program)
{
    VM& vm = program.vm();
    NamesWithOneHashSlot names = findNamesWithOneHashSlot();
    Identifier inItsSlot = atomOf(vm, names.inItsSlot);
    Identifier displaced = atomOf(vm, names.displaced);
    AOT_TEST_CHECK(inItsSlot != displaced);

    for (bool createsDisplacedFirst : { false, true }) {
        ProgramIdentifiers table(vm, *Image::current());
        auto checkNumbers = [&] {
            AOT_TEST_CHECK(table.numberOf(inItsSlot.impl()) == names.inItsSlot);
            AOT_TEST_CHECK(table.numberOf(displaced.impl()) == names.displaced);
        };
        checkNumbers();
        table.identifier(createsDisplacedFirst ? names.displaced : names.inItsSlot);
        checkNumbers();
        table.identifier(createsDisplacedFirst ? names.inItsSlot : names.displaced);
        checkNumbers();
    }
}

static void testWellKnownSymbolIsTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramIdentifiers table(vm, *Image::current());
    uint32_t iterator = findNumber(WellKnownSymbol::Iterator);
    uint32_t hasInstance = findNumber(WellKnownSymbol::HasInstance);

    AOT_TEST_CHECK(!table.numberOf(vm.propertyNames->iteratorSymbol.impl()));
    AOT_TEST_CHECK(table.identifier(iterator) == vm.propertyNames->iteratorSymbol);
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
        UniquedStringImpl* created = table.identifiers()[number];
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

    const Identifier& name = table.identifier(number);
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

    const Identifier& name = table.identifier(number);
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
        UniquedStringImpl* symbol = table.identifier(number).impl();
        AOT_TEST_CHECK(symbol->isSymbol());
        AOT_TEST_CHECK(table.identifier(number).impl() == symbol);
        AOT_TEST_CHECK(!symbols.contains(symbol));
        symbols.append(symbol);
    }
}

template<LoadedProgramTest body>
static constexpr void (*withTextNames)() = runNativeTestWithLoadedProgram<programIdentifiersTests, programWithTextNames, body>;

template<LoadedProgramTest body>
static constexpr void (*withSymbols)() = runNativeTestWithLoadedProgram<programIdentifiersTests, programWithSymbols, body>;

std::span<const NativeTest> programIdentifiersTests()
{
    static constexpr NativeTest tests[] = {
        { "program-identifiers/table-starts-empty"_s, withTextNames<testTableStartsEmpty> },
        { "program-identifiers/first-use-creates"_s, withTextNames<testFirstUseCreates> },
        { "program-identifiers/unnamed-number-zero"_s, withTextNames<testUnnamedNumberZero> },
        { "program-identifiers/read-from-another-thread"_s, withTextNames<testReadFromAnotherThread> },
        { "program-identifiers/reader-sees-null-or-complete-entry"_s, withTextNames<testReaderSeesNullOrCompleteEntry> },
        { "program-identifiers/number-of-text-needs-no-creation"_s, withTextNames<testNumberOfTextNeedsNoCreation> },
        { "program-identifiers/name-outside-program-has-no-number"_s, withTextNames<testNameOutsideProgramHasNoNumber> },
        { "program-identifiers/names-with-one-hash-slot"_s, withTextNames<testNamesWithOneHashSlot> },
        { "program-identifiers/well-known-symbol-is-the-engines"_s, withTextNames<testWellKnownSymbolIsTheEngines> },
        { "program-identifiers/registered-symbol-is-the-registrys"_s, withTextNames<testRegisteredSymbolIsTheRegistrys> },
        { "program-identifiers/start-up-creates-engine-symbols"_s, withTextNames<testStartUpCreatesEngineSymbols> },
        { "program-identifiers/engine-symbols-are-created-together"_s, withSymbols<testEngineSymbolsAreCreatedTogether> },
        { "program-identifiers/engine-private-name-is-the-engines"_s, withSymbols<testEnginePrivateNameIsTheEngines> },
        { "program-identifiers/engine-private-name-is-added-to-the-engines"_s, withSymbols<testEnginePrivateNameIsAddedToTheEngines> },
        { "program-identifiers/private-name-is-private-symbol"_s, withSymbols<testPrivateNameIsPrivateSymbol> },
        { "program-identifiers/created-symbol-has-its-description"_s, withSymbols<testCreatedSymbolHasItsDescription> },
        { "program-identifiers/symbol-is-created-once"_s, withSymbols<testSymbolIsCreatedOnce> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
