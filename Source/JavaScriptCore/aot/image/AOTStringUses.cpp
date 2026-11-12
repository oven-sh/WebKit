/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringUses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodedTexts.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageText.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(!RowDisplacementTable::rowMarkerColumn);
static_assert(defaultLimitValue(LimitID::LookupTableNames, hostArchitecture()) == RowDisplacementTable::maxColumn);
static_assert(RowDisplacementTable::maxColumn <= FieldTableEntry::maxName);
static_assert(StringID(0u) < StringID(), "a symbol without a description sorts last");

namespace {

struct StringByFirstFunction {
    static StringByFirstFunction create(const FunctionByModule& firstFunction, StringID string)
    {
        return { static_cast<uint64_t>(firstFunction.module.index()) << 32 | firstFunction.position, static_cast<uint64_t>(firstFunction.function.index()) << 32 | string.index() };
    }

    StringID string() const { return StringID(static_cast<uint32_t>(functionAndString)); }

    uint64_t moduleAndPosition;
    uint64_t functionAndString;
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

static Vector<NameCandidate> collectNumberedNames(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables, const Typed::BitVector& keysWithColumn)
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
    textsWithoutNumberedKey.merge(mentions.byID(NumberSpace::NameHeader).isMentioned);
    textsWithoutNumberedKey.merge(tables.textsNamedAsKeys);
    textsWithoutNumberedKey.exclude(isTextOfNumberedKey);
    for (size_t index : textsWithoutNumberedKey) {
        names.append(NameCandidate {
            .identifier = { PropertyKeyID(), StringID(index) },
            .numberOfFunctions = numberOfFunctionsMentioning(mentionedTexts, index),
            .firstFunction = mentions.firstFunctionMentioning(NumberSpace::NameOfText, static_cast<uint32_t>(index)),
        });
    }

    return names;
}

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

static void sortNamesByWordAndThen(const ParallelLoops& loops, std::span<NameCandidate> names, const Invocable<uint64_t(const NameCandidate&)> auto& wordOf, const Invocable<bool(const NameCandidate&, const NameCandidate&)> auto& comesFirstAmongNamesWithOneWord)
{
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, names, wordOf);
    for (size_t first = 0, end = 0; first < names.size(); first = end) {
        uint64_t word = wordOf(names[first]);
        for (end = first + 1; end < names.size() && wordOf(names[end]) == word; ++end) { }
        std::span<NameCandidate> namesWithOneWord = names.subspan(first, end - first);
        if (!std::ranges::is_sorted(namesWithOneWord, comesFirstAmongNamesWithOneWord))
            parallelSort(loops, namesWithOneWord, comesFirstAmongNamesWithOneWord);
    }
}

static void sortByUse(const Typed::Program& program, const ParallelLoops& loops, std::span<NameCandidate> names)
{
    sortNamesByWordAndThen(loops, names, [](const NameCandidate& name) {
        return static_cast<uint64_t>(!name.hasPromisedColumn) << 33 | static_cast<uint64_t>(UINT32_MAX - name.numberOfFunctions) << 1 | static_cast<uint64_t>(!name.isLookedUpByCode());
    }, [&](const NameCandidate& a, const NameCandidate& b) {
        if (a.identifier.key != b.identifier.key)
            return isNameKeyOrderedBefore(program, a.identifier.key, b.identifier.key);
        return a.identifier.text < b.identifier.text;
    });
}

static void groupColumnsByFirstFunction(std::span<NameCandidate> namesWithColumn)
{
    constexpr size_t numberOfHotNames = RowDisplacementTable::numberOfHotNames - 1;
    if (namesWithColumn.size() <= numberOfHotNames)
        return;
    AOT::stableSortInLine(namesWithColumn.subspan(numberOfHotNames), { }, &NameCandidate::firstFunction);
}

static void sortNamesWithoutColumn(const Typed::Program& program, const ParallelLoops& loops, std::span<NameCandidate> names)
{
    sortNamesByWordAndThen(loops, names, [](const NameCandidate& name) {
        return static_cast<uint64_t>(!name.isLookedUpByCode()) << 33 | static_cast<uint64_t>(!name.isEngineSymbol) << 32 | name.identifier.text.index();
    }, [&](const NameCandidate& a, const NameCandidate& b) {
        return isNameKeyOrderedBefore(program, a.identifier.key, b.identifier.key);
    });
}

static BitVector collectTextsWhoseCellsHoldAtoms(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables)
{
    BitVector texts(program.strings.size());
    texts.merge(tables.stringConstants);
    for (NumberSpace space : { NumberSpace::String, NumberSpace::StringCell })
        texts.merge(mentions.byID(space).isMentioned);
    texts.filter(mentions.byID(NumberSpace::AtomOfString).isMentioned);
    return texts;
}

static BitVector collectStringsReadThroughHeaders(const NumberingInputs& inputs, const GatheredMentions& mentions, const BitVector& textsWhoseCellsHoldAtoms, const RegExpRequests& regExpRequests, std::span<const NameCandidate> names)
{
    BitVector strings(inputs.program.strings.size());
    strings.merge(mentions.byID(NumberSpace::String).isMentioned);
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
    return strings;
}

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

static Vector<FunctionByModule> findFirstFunctionsNamingTexts(const Typed::Program& program, const GatheredMentions& mentions, std::span<const NameCandidate> names)
{
    Vector<FunctionByModule> firstFunctions(FillWith { }, program.strings.size(), FunctionByModule { });
    auto lowerTo = [&](StringID text, FunctionByModule function) {
        if (text)
            firstFunctions[text.index()] = std::min(firstFunctions[text.index()], function);
    };
    for (NumberSpace space : { NumberSpace::String, NumberSpace::StringCell, NumberSpace::StringCharacters, NumberSpace::NameOfText }) {
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

static void sortStringsWithFirstFunctions(const ParallelLoops& loops, std::span<StringByFirstFunction> strings)
{
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, strings, [](const StringByFirstFunction& entry) {
        return entry.functionAndString >> 32;
    });
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, strings, [](const StringByFirstFunction& entry) {
        return entry.moduleAndPosition;
    });
}

static Vector<StringID> sortStringsByFirstFunction(const ParallelLoops& loops, const BitVector& strings, const Invocable<FunctionByModule(size_t)> auto& firstFunctionOf)
{
    Vector<StringByFirstFunction> inOrder;
    inOrder.reserveInitialCapacity(strings.bitCount());
    for (size_t string : strings)
        inOrder.append(StringByFirstFunction::create(firstFunctionOf(string), StringID(string)));
    sortStringsWithFirstFunctions(loops, inOrder.mutableSpan());
    return inOrder.map([](const StringByFirstFunction& entry) {
        return entry.string();
    });
}

static Vector<StringID> sortTextsForCoding(const Typed::StringTable& strings, const BitVector& candidates, const ParallelLoops& loops)
{
    struct TextByFront {
        uint64_t front;
        StringID id;
    };
    Vector<TextByFront> texts;
    for (size_t index : candidates) {
        StringID id(index);
        std::span<const uint8_t> text = strings[id];
        if (!CodedTexts::canHold(text))
            continue;
        uint64_t front = 0;
        for (size_t place = 0; place < sizeof(front); ++place)
            front = front << 8 | (place < text.size() ? text[place] : 0);
        texts.append({ front, id });
    }
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, texts.mutableSpan(), [](const TextByFront& text) {
        return text.front;
    });
    for (size_t begin = 0; begin < texts.size();) {
        size_t end = begin + 1;
        while (end < texts.size() && texts[end].front == texts[begin].front)
            ++end;
        AOT::sort(texts.mutableSpan().subspan(begin, end - begin), [&](const TextByFront& a, const TextByFront& b) {
            return std::ranges::lexicographical_compare(strings[a.id], strings[b.id]);
        });
        begin = end;
    }
    return texts.map([](const TextByFront& text) {
        return text.id;
    });
}

void ImageNumbersBuilder::collectNamesAndStringsWithText()
{
    const Typed::Program& program = inputs.program;
    const Typed::BitVector& keysWithColumn = inputs.plan.slots().keysWithColumn();

    if (size_t numberOfPromises = keysWithColumn.bitCount(); numberOfPromises > RowDisplacementTable::maxColumn) {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::ProgramTableCheckFailed,
            .limit = LimitID::LookupTableNames,
            .detail = makeString("the plan promises "_s, numberOfPromises, " names a column, and the front tables have "_s, RowDisplacementTable::maxColumn),
        });
    }

    auto toNumber = makeUniqueWithoutFastMallocCheck<NamesAndStringsToNumber>();
    toNumber->names = collectNumberedNames(program, mentions, tables, keysWithColumn);
    std::span<const NameCandidate> names = toNumber->names.span();
    RELEASE_ASSERT(numbers.m_regExpRequests);
    numbers.m_bytesBetweenNameHeaders = context.recordedOptions().form() == ImageForm::PositionIndependent ? sizeof(uint32_t) : sizeof(ImageStringHeader);
    runLanes(context.parallelLoops(), {
        [&] {
            BitVector textsWhoseCellsHoldAtoms = collectTextsWhoseCellsHoldAtoms(program, mentions, tables);
            BitVector stringsReadThroughHeaders = collectStringsReadThroughHeaders(inputs, mentions, textsWhoseCellsHoldAtoms, *numbers.m_regExpRequests, names);
            const BitVector& stringsShownByCode = mentions.byID(NumberSpace::ShownString).isMentioned;
            toNumber->numberedConstants = collectNumberedStringConstants(program, mentions, tables);
            BitVector stringsWithOnlyPlacedCharacters = mentions.byID(NumberSpace::StringCharacters).isMentioned;
            stringsWithOnlyPlacedCharacters.exclude(toNumber->numberedConstants);
            numbers.m_stringsOnlyShown = tables.strings;
            numbers.m_stringsOnlyShown.merge(stringsShownByCode);
            numbers.m_stringsOnlyShown.merge(stringsWithOnlyPlacedCharacters);
            numbers.m_stringsOnlyShown.exclude(tables.stringsWithOtherUsesThanShowing);
            numbers.m_stringsOnlyShown.exclude(stringsReadThroughHeaders);
            numbers.m_namedStrings = WTF::move(stringsReadThroughHeaders);
            numbers.m_namedStrings.merge(tables.strings);
            numbers.m_namedStrings.merge(stringsShownByCode);
            numbers.m_namedStrings.merge(stringsWithOnlyPlacedCharacters);

            numbers.m_atomsOfStrings = collectAtomsOfStrings(program, textsWhoseCellsHoldAtoms, *numbers.m_regExpRequests);
            numbers.m_otherStringsWithText = numbers.m_namedStrings;
            numbers.m_otherStringsWithText.merge(toNumber->numberedConstants);
            numbers.m_otherStringsWithText.quickSet(program.strings.emptyString().index());

            numbers.m_textsOfNames = BitVector(program.strings.size());
            for (const NameCandidate& name : names) {
                if (name.identifier.text)
                    numbers.m_textsOfNames.quickSet(name.identifier.text.index());
            }
            numbers.m_otherStringsWithText.exclude(numbers.m_textsOfNames);

            BitVector candidates = numbers.m_stringsOnlyShown;
            candidates.exclude(toNumber->numberedConstants);
            candidates.exclude(stringsWithOnlyPlacedCharacters);
            candidates.exclude(tables.modulePathStrings);
            toNumber->codedStrings = sortTextsForCoding(program.strings, candidates, context.parallelLoops());
            for (StringID id : toNumber->codedStrings)
                numbers.m_otherStringsWithText.quickClear(id.index());
        },
        [&] {
            toNumber->firstFunctionsNamingTexts = findFirstFunctionsNamingTexts(program, mentions, names);
        },
    });
    numbers.m_namesAndStringsToNumber = WTF::move(toNumber);
}

void ImageNumbersBuilder::numberIdentifiersAndStrings()
{
    const Typed::Program& program = inputs.program;
    const size_t numberOfStrings = program.strings.size();

    if (!numbers.m_namesAndStringsToNumber)
        collectNamesAndStringsWithText();
    std::unique_ptr<NamesAndStringsToNumber> toNumber = std::exchange(numbers.m_namesAndStringsToNumber, nullptr);
    Vector<NameCandidate>& names = toNumber->names;
    auto firstFunctionNamingText = [&](size_t string) {
        return toNumber->firstFunctionsNamingTexts[string];
    };

    auto isASCII = [&](StringID string) {
        return charactersAreAllASCII(byteCast<Latin1Character>(std::span<const uint8_t> { program.strings[string] }));
    };
    auto fitsConstantSlots = [&] {
        uint64_t mostBytesOfText = 0;
        for (const BitVector* strings : { &numbers.m_textsOfNames, &numbers.m_otherStringsWithText }) {
            for (size_t string : *strings)
                mostBytesOfText += isASCII(StringID(string)) ? program.strings[StringID(string)].size() : program.strings[StringID(string)].size() * sizeof(char16_t);
        }
        return mostBytesOfText <= ImagePlacedString::maxOffset;
    };

    IdentifierNumbering identifiers;
    uint64_t mentionedWithoutColumn = 0;
    Vector<StringID> otherStrings;
    StringConstantNumbering constants;
    runLanes(context.parallelLoops(), {
        [&] {
            sortByUse(program, context.parallelLoops(), names.mutableSpan());
            auto columnCount = static_cast<uint32_t>(std::min<uint64_t>(names.size(), RowDisplacementTable::maxColumn));
            runLanes(context.parallelLoops(), {
                [&] { groupColumnsByFirstFunction(names.mutableSpan().first(columnCount)); },
                [&] { sortNamesWithoutColumn(program, context.parallelLoops(), names.mutableSpan().subspan(columnCount)); },
            });

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
            mentionedWithoutColumn = static_cast<uint64_t>(std::ranges::count_if(names.span().subspan(columnCount), &NameCandidate::isLookedUpByCode));
            uint64_t numbersProbedWithoutAsking = std::min<uint64_t>(names.size(), RowDisplacementTable::maxColumn);
            identifiers.m_countWithFieldTableColumn = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(columnCount + mentionedWithoutColumn, numbersProbedWithoutAsking), FieldTableEntry::maxName) + 1);

            if (context.exceeds(LimitID::MaxIdentifiers, identifiers.count()))
                context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = identifiers.count() });
        },
        [&] {
            numbers.m_stringsInOrderOfText = sortStringsByFirstFunction(context.parallelLoops(), numbers.m_textsOfNames, firstFunctionNamingText);
        },
        [&] {
            otherStrings = sortStringsByFirstFunction(context.parallelLoops(), numbers.m_otherStringsWithText, firstFunctionNamingText);
            std::ranges::stable_partition(otherStrings, [&](StringID string) {
                return program.strings[string].size() <= ImageString::lengthMask;
            });
        },
        [&] {
            constants.m_strings = sortStringsByFirstFunction(context.parallelLoops(), toNumber->numberedConstants, [&](size_t string) {
                return mentions.firstFunctionMentioning(NumberSpace::StringCell, static_cast<uint32_t>(string));
            });
            if (fitsConstantSlots()) {
                auto others = std::ranges::stable_partition(constants.m_strings, [&](StringID string) {
                    return ImagePlacedString::fitsLength(program.strings[string].size()) && !numbers.m_textsOfNames.get(string.index()) && !numbers.m_atomsOfStrings.get(string.index()) && !numbers.m_namedStrings.get(string.index())
                        && isASCII(string);
                });
                constants.m_numberOfPlaced = static_cast<uint32_t>(others.begin() - constants.m_strings.begin());
            }
            constants.m_numbersPlusOne.fill(0, numberOfStrings);
            for (uint32_t number = 0; number < constants.count(); ++number)
                constants.m_numbersPlusOne[constants.m_strings[number].index()] = number + 1;
        },
    });
    numbers.m_stringsInOrderOfText.appendVector(otherStrings);

    context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
    context.statistics().count(StatisticID::NamesWithoutColumn, mentionedWithoutColumn);
    context.statistics().count(StatisticID::StringConstantsNumbered, constants.count());

    numbers.m_stringNumbers = StringNumbers(numbers.m_namedStrings, WTF::move(toNumber->codedStrings), numberOfStrings, &identifiers);
    numbers.m_identifiers = makeUniqueWithoutFastMallocCheck<IdentifierNumbering>(WTF::move(identifiers));
    numbers.m_stringConstants = makeUniqueWithoutFastMallocCheck<StringConstantNumbering>(WTF::move(constants));
    destroyInBackground(context.backgroundJobs(), WTF::move(toNumber));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
