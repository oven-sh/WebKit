/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringUses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageText.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(!RowDisplacementTable::rowMarkerColumn);
static_assert(defaultLimitValue(LimitID::LookupTableNames, hostArchitecture()) == RowDisplacementTable::maxColumn);
static_assert(RowDisplacementTable::maxColumn <= FieldTableEntry::maxName);
static_assert(StringID(0u) < StringID(), "a symbol without a description sorts last");

namespace {

struct NameCandidate {
    NumberedIdentifier identifier;
    bool hasPromisedColumn { false };
    bool isEngineSymbol { false };
    // Code holds its number to find its atom and looks nothing up under it, so it comes behind every other name.
    bool isOnlyCompared { false };
    // Code that runs at most once is not counted, so zero does not say that no code looks the name up.
    uint32_t numberOfFunctions { 0 };
    // Of the functions whose code holds the number of the name.
    FunctionByModule firstFunction;

    bool isLookedUpByCode() const { return !isOnlyCompared && (numberOfFunctions || firstFunction.function); }
};

struct StringByFirstFunction {
    friend auto operator<=>(const StringByFirstFunction&, const StringByFirstFunction&) = default;

    FunctionByModule firstFunction;
    StringID string;
};

} // anonymous namespace

static bool isFoundByText(const Typed::Program& program, const NumberedIdentifier& identifier)
{
    return !identifier.key || program.at(identifier.key).kind == PropertyKeyKind::Text;
}

static uint32_t numberOfFunctionsMentioning(const GatheredMentions::ByID& mentions, size_t id)
{
    return id < mentions.numberOfFunctions.size() ? mentions.numberOfFunctions[id] : 0;
}

// Plan promises columns by the accesses as they are written. A key whose every access the walk resolved, and that no table names, is no name of the image, whatever it was promised.
static Vector<NameCandidate> collectNumberedNames(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables, const BitVector& keysWithColumn)
{
    const GatheredMentions::ByID& mentionedKeys = mentions.byID(NumberSpace::Name);
    const GatheredMentions::ByID& mentionedTexts = mentions.byID(NumberSpace::NameOfText);
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();

    Vector<NameCandidate> names;
    BitVector isTextOfNumberedKey(program.strings.size());
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        bool isNamed = mentionedKeys.isMentioned.get(index) || tables.keys.get(index);
        if (!isNamed && !program.builtinNameOf(key))
            continue;
        NumberedIdentifier identifier { key, program.at(key).text };
        uint32_t numberOfFunctions = numberOfFunctionsMentioning(mentionedKeys, index);
        FunctionByModule firstFunction = mentions.firstFunctionMentioning(NumberSpace::Name, index);
        if (isFoundByText(program, identifier)) {
            isTextOfNumberedKey.quickSet(identifier.text.index());
            numberOfFunctions += numberOfFunctionsMentioning(mentionedTexts, identifier.text.index());
            firstFunction = std::min(firstFunction, mentions.firstFunctionMentioning(NumberSpace::NameOfText, identifier.text.index()));
        }
        names.append(NameCandidate {
            .identifier = identifier,
            .hasPromisedColumn = keysWithColumn.get(index),
            .isEngineSymbol = isKindOfEngineSymbol(program.at(key).kind),
            .numberOfFunctions = numberOfFunctions,
            .firstFunction = firstFunction,
        });
    }

    BitVector textsWithoutNumberedKey(program.strings.size());
    textsWithoutNumberedKey.merge(mentionedTexts.isMentioned);
    textsWithoutNumberedKey.merge(tables.textsNamedAsKeys);
    textsWithoutNumberedKey.exclude(isTextOfNumberedKey);
    for (size_t index : textsWithoutNumberedKey) {
        names.append(NameCandidate {
            .identifier = { PropertyKeyID(), StringID(index) },
            .numberOfFunctions = numberOfFunctionsMentioning(mentionedTexts, index),
            .firstFunction = mentions.firstFunctionMentioning(NumberSpace::NameOfText, static_cast<uint32_t>(index)),
        });
    }

    // `kind === "circle"`: as a name of the image the text has one atom, at a place that the image fixes.
    BitVector textsOnlyCompared(program.strings.size());
    textsOnlyCompared.merge(mentions.byID(NumberSpace::AtomOfComparedText).isMentioned);
    textsOnlyCompared.exclude(isTextOfNumberedKey);
    textsOnlyCompared.exclude(textsWithoutNumberedKey);
    for (size_t index : textsOnlyCompared) {
        names.append(NameCandidate {
            .identifier = { PropertyKeyID(), StringID(index) },
            .isOnlyCompared = true,
            .firstFunction = mentions.firstFunctionMentioning(NumberSpace::AtomOfComparedText, static_cast<uint32_t>(index)),
        });
    }
    return names;
}

// A key made after TypeTable::endStableNumbersOfPropertyKeys() is numbered in the order threads arrived, so no order may depend on its number.
static bool isNameKeyOrderedBefore(const Typed::Program& program, PropertyKeyID a, PropertyKeyID b)
{
    enum class OrderedBy : uint8_t { Number, Content, TextAlone };
    auto orderOf = [&](PropertyKeyID key) {
        if (!key)
            return OrderedBy::TextAlone;
        return program.types.hasStableNumber(key) ? OrderedBy::Number : OrderedBy::Content;
    };
    OrderedBy orderOfA = orderOf(a);
    if (orderOfA != orderOf(b))
        return orderOfA < orderOf(b);
    if (orderOfA == OrderedBy::Content)
        return std::is_lt(program.types.compareByContent(a, b));
    return a < b;
}

static void sortByUse(const Typed::Program& program, const ParallelLoops& loops, std::span<NameCandidate> names)
{
    parallelSort(loops, names, [&](const NameCandidate& a, const NameCandidate& b) {
        if (a.isOnlyCompared != b.isOnlyCompared)
            return b.isOnlyCompared;
        if (a.hasPromisedColumn != b.hasPromisedColumn)
            return a.hasPromisedColumn;
        if (a.numberOfFunctions != b.numberOfFunctions)
            return a.numberOfFunctions > b.numberOfFunctions;
        if (a.isLookedUpByCode() != b.isLookedUpByCode())
            return a.isLookedUpByCode();
        if (a.identifier.key != b.identifier.key)
            return isNameKeyOrderedBefore(program, a.identifier.key, b.identifier.key);
        return a.identifier.text < b.identifier.text;
    });
}

// RowDisplacementTable::numberForRank() scatters the hot names. Behind them the names that one function looks up have neighboring columns. Every name stays among those with a column, so no promise is broken.
static void groupColumnsByFirstFunction(std::span<NameCandidate> namesWithColumn)
{
    constexpr size_t numberOfHotNames = RowDisplacementTable::numberOfHotNames - 1;
    if (namesWithColumn.size() <= numberOfHotNames)
        return;
    AOT::stableSortInLine(namesWithColumn.subspan(numberOfHotNames), { }, &NameCandidate::firstFunction);
}

static void sortNamesWithoutColumn(const Typed::Program& program, const ParallelLoops& loops, std::span<NameCandidate> names)
{
    parallelSort(loops, names, [&](const NameCandidate& a, const NameCandidate& b) {
        // The table of fields takes more names than the front tables. The names that code looks up get the low numbers, which are the ones it takes.
        if (a.isLookedUpByCode() != b.isLookedUpByCode())
            return a.isLookedUpByCode();
        if (a.isOnlyCompared != b.isOnlyCompared)
            return b.isOnlyCompared;
        if (a.isEngineSymbol != b.isEngineSymbol)
            return a.isEngineSymbol;
        if (a.identifier.text != b.identifier.text)
            return a.identifier.text < b.identifier.text;
        return isNameKeyOrderedBefore(program, a.identifier.key, b.identifier.key);
    });
}

// `kind === "circle"` wants the cells of the text to hold an atom and makes none. Where nothing else makes one either, the image keeps no text for the sake of the comparison.
static BitVector collectTextsWhoseCellsHoldAtoms(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables)
{
    BitVector texts(program.strings.size());
    texts.merge(tables.stringConstants);
    for (NumberSpace space : { NumberSpace::String, NumberSpace::StringCell })
        texts.merge(mentions.byID(space).isMentioned);
    texts.filter(mentions.byID(NumberSpace::AtomOfString).isMentioned);
    return texts;
}

// A string that is left out here may lose its header, and code reads the header without asking whether it is there.
static BitVector collectStringsReadThroughHeaders(const NumberingInputs& inputs, const GatheredMentions& mentions, const BitVector& textsWhoseCellsHoldAtoms, const RegExpRequests& regExpRequests, std::span<const NameCandidate> names)
{
    BitVector strings(inputs.program.strings.size());
    strings.merge(mentions.byID(NumberSpace::String).isMentioned);
    // The run time makes an atom of the header.
    strings.merge(textsWhoseCellsHoldAtoms);
    for (const NameCandidate& name : names) {
        if (name.identifier.text)
            strings.quickSet(name.identifier.text.index());
    }
    for (const RegExpRequest& request : regExpRequests.all()) {
        strings.quickSet(request.pattern.index());
        if (request.atom)
            strings.quickSet(request.atom.index());
        for (StringID name : request.captureGroupNames) {
            if (name)
                strings.quickSet(name.index());
        }
        for (const RegExpNamedGroupStrings& group : request.namedGroups)
            strings.quickSet(group.name.index());
    }
    for (StringID string : inputs.plan.literalStrings().stringsWithoutWord())
        strings.quickSet(string.index());
    return strings;
}

// `cache.get("config")`, `x == "auto"`, `match.groups.year`: a text that is looked up or compared is made as an atom once, and not hashed at each use.
static BitVector collectAtomsOfStrings(const Typed::Program& program, const BitVector& textsWhoseCellsHoldAtoms, const RegExpRequests& regExpRequests)
{
    BitVector atoms(program.strings.size());
    atoms.merge(textsWhoseCellsHoldAtoms);
    for (const RegExpRequest& request : regExpRequests.all()) {
        for (StringID name : request.captureGroupNames) {
            if (name)
                atoms.quickSet(name.index());
        }
        for (const RegExpNamedGroupStrings& group : request.namedGroups)
            atoms.quickSet(group.name.index());
    }
    return atoms;
}

// Code takes the cell of the empty string and of one Latin-1 character from the VM and mentions none. A record of a table holds the number of whatever text it names.
static BitVector collectNumberedStringConstants(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables)
{
    BitVector constants(program.strings.size());
    constants.merge(tables.stringConstants);
    for (size_t string : mentions.byID(NumberSpace::StringCell).isMentioned) {
        if (!Typed::hasCellOfVM(program.strings[StringID(string)]))
            constants.quickSet(string);
    }
    return constants;
}

// [StringID]: the first function whose code names the text, as a string, as a constant or as a name, or that has it for its own name.
static Vector<FunctionByModule> findFirstFunctionsNamingTexts(const Typed::Program& program, const GatheredMentions& mentions, std::span<const NameCandidate> names)
{
    Vector<FunctionByModule> firstFunctions(FillWith { }, program.strings.size(), FunctionByModule { });
    auto lowerTo = [&](StringID text, FunctionByModule function) {
        if (text)
            firstFunctions[text.index()] = std::min(firstFunctions[text.index()], function);
    };
    for (NumberSpace space : { NumberSpace::String, NumberSpace::StringCell, NumberSpace::NameOfText, NumberSpace::AtomOfComparedText }) {
        for (size_t string : mentions.byID(space).isMentioned)
            lowerTo(StringID(string), mentions.firstFunctionMentioning(space, static_cast<uint32_t>(string)));
    }
    for (const NameCandidate& name : names) {
        lowerTo(name.identifier.text, name.firstFunction);
        if (name.identifier.key)
            lowerTo(name.identifier.text, mentions.firstFunctionMentioning(NumberSpace::KeyCell, name.identifier.key.index()));
    }
    for (size_t index = 0; index < program.functions.size(); ++index)
        lowerTo(program.functions[index].name, FunctionByModule::forFunction(program, FunctionID(index)));
    return firstFunctions;
}

// What start-up touches are the strings of the files it runs: neighbors in the sources share pages and cache lines. A string that no function names comes behind all that one does.
static Vector<StringID> sortStringsByFirstFunction(const ParallelLoops& loops, const BitVector& strings, const Invocable<FunctionByModule(size_t)> auto& firstFunctionOf)
{
    Vector<StringByFirstFunction> inOrder;
    inOrder.reserveInitialCapacity(strings.bitCount());
    for (size_t string : strings)
        inOrder.append(StringByFirstFunction { firstFunctionOf(string), StringID(string) });
    parallelSort(loops, inOrder.mutableSpan(), std::less<StringByFirstFunction> { });
    return inOrder.map([](const StringByFirstFunction& entry) {
        return entry.string;
    });
}

void ImageNumbersBuilder::numberIdentifiersAndStrings()
{
    const Typed::Program& program = inputs.program;
    const ParallelLoops& loops = context.parallelLoops();
    const BitVector& keysWithColumn = inputs.plan.slots().keysWithColumn();
    const size_t numberOfStrings = program.strings.size();

    if (size_t numberOfPromises = keysWithColumn.bitCount(); numberOfPromises > RowDisplacementTable::maxColumn) {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::ProgramTableCheckFailed,
            .limit = LimitID::LookupTableNames,
            .detail = makeString("the plan promises "_s, numberOfPromises, " names a column, and the front tables have "_s, RowDisplacementTable::maxColumn),
        });
    }

    Vector<NameCandidate> names = collectNumberedNames(program, mentions, tables, keysWithColumn);
    sortByUse(program, loops, names.mutableSpan());
    // Every number has a record in the image, so none is skipped. Where fewer promised names are left than there are columns, names without a promise get numbers that have columns too.
    // The run time files a slot by the number alone, so a stub that is handed a name without a promise asks the number.
    // A text that is only compared takes no column from a name that is looked up.
    auto numberOfNamesLookedUp = static_cast<uint64_t>(std::ranges::count_if(names, [](const NameCandidate& name) {
        return !name.isOnlyCompared;
    }));
    auto columnCount = static_cast<uint32_t>(std::min<uint64_t>(numberOfNamesLookedUp, RowDisplacementTable::maxColumn));
    groupColumnsByFirstFunction(names.mutableSpan().first(columnCount));
    sortNamesWithoutColumn(program, loops, names.mutableSpan().subspan(columnCount));

    IdentifierNumbering identifiers;
    identifiers.m_numbersByKey.fill(0, program.types.numberOfPropertyKeys());
    identifiers.m_numbersByText.fill(0, numberOfStrings);
    identifiers.m_identifiers.grow(names.size() + 1);
    for (uint32_t index = 0; index < names.size(); ++index) {
        const NumberedIdentifier& identifier = names[index].identifier;
        uint32_t number = RowDisplacementTable::numberForRank(index + 1, columnCount);
        identifiers.m_identifiers[number] = identifier;
        if (identifier.key)
            identifiers.m_numbersByKey[identifier.key.index()] = number;
        if (isFoundByText(program, identifier))
            identifiers.m_numbersByText[identifier.text.index()] = number;
    }
    identifiers.m_countWithLookupTableColumn = columnCount + 1;
    auto mentionedWithoutColumn = static_cast<uint64_t>(std::ranges::count_if(names.span().subspan(columnCount), &NameCandidate::isLookedUpByCode));
    // Above every number up to RowDisplacementTable::maxColumn that a name has: the stubs probe the table of fields under any such number without asking whether it has a column there,
    // and a key that the program computes may be a text that its code only compares. Past that number such a text widens nothing.
    uint64_t numbersProbedWithoutAsking = std::min<uint64_t>(names.size(), RowDisplacementTable::maxColumn);
    identifiers.m_countWithFieldTableColumn = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(columnCount + mentionedWithoutColumn, numbersProbedWithoutAsking), FieldTableEntry::maxName) + 1);

    if (context.exceeds(LimitID::MaxIdentifiers, identifiers.count()))
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = identifiers.count() });

    RELEASE_ASSERT(numbers.m_regExpRequests);
    BitVector textsWhoseCellsHoldAtoms = collectTextsWhoseCellsHoldAtoms(program, mentions, tables);
    BitVector stringsReadThroughHeaders = collectStringsReadThroughHeaders(inputs, mentions, textsWhoseCellsHoldAtoms, *numbers.m_regExpRequests, names.span());
    const BitVector& stringsShownByCode = mentions.byID(NumberSpace::ShownString).isMentioned;
    numbers.m_stringsOnlyShown = tables.strings;
    numbers.m_stringsOnlyShown.merge(stringsShownByCode);
    numbers.m_stringsOnlyShown.exclude(tables.stringsWithOtherUsesThanShowing);
    numbers.m_stringsOnlyShown.exclude(stringsReadThroughHeaders);
    numbers.m_namedStrings = WTF::move(stringsReadThroughHeaders);
    numbers.m_namedStrings.merge(tables.strings);
    numbers.m_namedStrings.merge(stringsShownByCode);

    BitVector numberedConstants = collectNumberedStringConstants(program, mentions, tables);
    numbers.m_atomsOfStrings = collectAtomsOfStrings(program, textsWhoseCellsHoldAtoms, *numbers.m_regExpRequests);
    BitVector stringsWithText = numbers.m_namedStrings;
    stringsWithText.merge(numberedConstants);
    stringsWithText.quickSet(program.strings.emptyString().index());

    // A value is a literal string that nothing spells only once a string from outside was that string, and that string lends its text to the instance: ProgramStrings::isTextOfLiteralStringWithoutText().
    // Where one realm follows another, an object of the earlier one may hold a word whose text was lent to the earlier instance alone.
    // A literal string whose text is in the image has a header there with its word: the run time and the stubs take a header that the run time made for a constant for that of no literal string.
    bool everyLiteralStringHasText = context.options().runsInSeveralRealms();
    numbers.m_literalStringsWithoutText = BitVector(numberOfStrings);
    for (StringID string : inputs.plan.literalStrings().strings()) {
        if (!everyLiteralStringHasText && !stringsWithText.quickGet(string.index())) {
            numbers.m_literalStringsWithoutText.quickSet(string.index());
            continue;
        }
        stringsWithText.quickSet(string.index());
        numbers.m_namedStrings.quickSet(string.index());
        numbers.m_stringsOnlyShown.clear(string.index());
    }

    BitVector textsOfNames(numberOfStrings);
    for (const NameCandidate& name : names) {
        if (name.identifier.text)
            textsOfNames.quickSet(name.identifier.text.index());
    }
    stringsWithText.exclude(textsOfNames);
    Vector<FunctionByModule> firstFunctionsNamingTexts = findFirstFunctionsNamingTexts(program, mentions, names.span());
    auto firstFunctionNamingText = [&](size_t string) {
        return firstFunctionsNamingTexts[string];
    };
    numbers.m_stringsInOrderOfText = sortStringsByFirstFunction(loops, textsOfNames, firstFunctionNamingText);
    // One word says where a string without a header lies while the text is short and begins in the first 16 MB of all text, and three words say it otherwise: ImageListedString.
    // A long text takes the three wherever it lies, and half of all text is long. So the long ones lie behind the others, which then begin early enough.
    Vector<StringID> otherStrings = sortStringsByFirstFunction(loops, stringsWithText, firstFunctionNamingText);
    std::ranges::stable_partition(otherStrings, [&](StringID string) {
        return program.strings[string].size() <= ImageString::lengthMask;
    });
    numbers.m_stringsInOrderOfText.appendVector(otherStrings);

    StringConstantNumbering constants;
    constants.m_strings = sortStringsByFirstFunction(loops, numberedConstants, [&](size_t string) {
        return mentions.firstFunctionMentioning(NumberSpace::StringCell, static_cast<uint32_t>(string));
    });
    constants.m_numbersPlusOne.fill(0, numberOfStrings);
    for (uint32_t number = 0; number < constants.count(); ++number)
        constants.m_numbersPlusOne[constants.m_strings[number].index()] = number + 1;

    context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
    context.statistics().count(StatisticID::NamesWithoutColumn, mentionedWithoutColumn);
    context.statistics().count(StatisticID::StringConstantsNumbered, constants.count());

    numbers.m_identifiers = makeUniqueWithoutFastMallocCheck<IdentifierNumbering>(WTF::move(identifiers));
    numbers.m_stringConstants = makeUniqueWithoutFastMallocCheck<StringConstantNumbering>(WTF::move(constants));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
