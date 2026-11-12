/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTClassGuards.h"
#include "AOTCurrentWork.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstanceLayout.h"
#include "AOTObjectWords.h"
#include "AOTOperationData.h"
#include "AOTPhaseTimes.h"
#include "AOTRegExpRequests.h"
#include "AOTRuntimeWords.h"
#include "AOTStringUses.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static_assert(sizeof(EncodedJSValue) == sizeof(JSCell*), "The word of a value lies as far into its table as the word of a cell with the same number.");

template<typename Element, typename Key>
static std::optional<uint32_t> placeAmongNumberedThings(std::span<const Element> ascending, const Key& key, const auto& keyOfElement)
{
    auto found = std::ranges::lower_bound(ascending, key, { }, keyOfElement);
    if (found == ascending.end() || std::invoke(keyOfElement, *found) != key)
        return std::nullopt;
    return static_cast<uint32_t>(found - ascending.begin());
}

static std::optional<uint32_t> placedOffsetAt(const Vector<uint32_t>& offsets, uint32_t index)
{
    if (index >= offsets.size() || offsets[index] == PlacedStatics::noOffset)
        return std::nullopt;
    return offsets[index];
}

static std::optional<uint32_t> unitIndexOfClosureEntry(uint32_t function)
{
    uint64_t index = static_cast<uint64_t>(function) * Typed::numberOfCodeRolesOfOneFunction + static_cast<uint64_t>(Typed::CodeRole::GenericCallEntry);
    if (index > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(index);
}

static ALWAYS_INLINE std::optional<uint32_t> offsetOfMentionedWordInStatics(const ImageNumbers& numbers, NumberSpace space, std::span<const uint32_t> key)
{
    if (key.empty() || (hasOneIDAsKey(space) && key.size() != 1))
        return std::nullopt;

    const PlacedStatics& statics = numbers.statics();
    switch (space) {
    case NumberSpace::Variable:
        if (key[0] >= statics.variables.size() || !statics.variables[key[0]].hasWordInStatics())
            return std::nullopt;
        return statics.variables[key[0]].offset;
    case NumberSpace::ModuleValue:
        return placedOffsetAt(statics.moduleValueOffsets, key[0]);
    case NumberSpace::ModuleNamespace:
        return placedOffsetAt(statics.moduleNamespaceOffsets, key[0]);
    case NumberSpace::ModuleBuiltinCell: {
        if (key.size() != 2)
            return std::nullopt;
        std::optional<uint32_t> place = placeAmongNumberedThings(statics.moduleBuiltinCells.span(), std::pair<uint32_t, uint32_t>(key[0], key[1]), [](const ModuleBuiltinCell& cell) {
            return std::pair<uint32_t, uint32_t>(cell.builtin.index(), cell.module.index());
        });
        if (!place)
            return std::nullopt;
        return statics.moduleBuiltinCells[*place].offset;
    }
    case NumberSpace::DataStorage:
        return placedOffsetAt(statics.dataStorageOffsets, key[0]);
    case NumberSpace::RegExpLiteralCell: {
        std::optional<uint32_t> place = numbers.regExpLiteralCells().find(key);
        if (!place)
            return std::nullopt;
        return placedOffsetAt(statics.regExpLiteralCellOffsets, *place);
    }
    case NumberSpace::TemplateObjectCell: {
        std::optional<uint32_t> place = numbers.templateObjectCells().find(key);
        if (!place)
            return std::nullopt;
        return placedOffsetAt(statics.templateObjectCellOffsets, *place);
    }
    case NumberSpace::ArrayProfile: {
        std::optional<uint32_t> place = numbers.arrayProfiles().find(key);
        if (!place)
            return std::nullopt;
        return placedOffsetAt(statics.arrayProfileOffsets, *place);
    }
    case NumberSpace::ReadGroupCache: {
        std::optional<uint32_t> place = numbers.readGroupCaches().find(key);
        if (!place)
            return std::nullopt;
        return placedOffsetAt(statics.readGroupCacheOffsets, *place);
    }
    case NumberSpace::EnvironmentHandedBack:
        return placedOffsetAt(statics.environmentHandedBackOffsets, key[0]);
    case NumberSpace::HeirLookedAt:
        return placedOffsetAt(statics.structuresOfHeirOffsets, key[0]);
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::GlobalBinding:
    case NumberSpace::SiteCache:
    case NumberSpace::RequireEdge:
    case NumberSpace::CardOfVariable:
    case NumberSpace::CardOfWord:
    case NumberSpace::DataElements:
    case NumberSpace::ClassGuard:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::PropertyRun:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::Type:
    case NumberSpace::MethodSelector:
    case NumberSpace::RegExp:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<int64_t> distanceFromInstanceToMentionedWord(const ImageNumbers& numbers, NumberSpace space, std::span<const uint32_t> key)
{
    InstanceSizes sizes = numbers.instanceSizes();
    auto byteOfStatics = [&](std::optional<uint32_t> offset) -> std::optional<int64_t> {
        if (!offset || *offset >= sizes.sizeOfStatics)
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfStatics(sizes)) + *offset;
    };
    auto cardOfStatics = [&](std::optional<uint32_t> offset) -> std::optional<int64_t> {
        if (!offset || *offset >= sizes.sizeOfStatics)
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfCards(sizes)) + (*offset >> InstanceLayout::cardShift);
    };

    switch (space) {
    case NumberSpace::StringCell: {
        std::optional<uint32_t> number = numbers.numberOf(space, key);
        if (!number || *number >= sizes.numberOfStringCells)
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfStringCell(*number));
    }
    case NumberSpace::BuiltinCell: {
        std::span<const BuiltinCellKey> cells = numbers.builtinCells();
        std::optional<uint32_t> number = numbers.numberOf(space, key);
        if (!number || !InstanceLayout::holdsBuiltinCells(cells.size()))
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfBuiltinCells + *number * sizeof(JSCell*));
    }
    case NumberSpace::GlobalBinding: {
        std::optional<uint32_t> number = numbers.numberOf(space, key);
        if (!number || *number >= sizes.numberOfGlobalBindings)
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfGlobalBindings(sizes) + *number * sizeof(GlobalBinding));
    }
    case NumberSpace::SiteCache: {
        std::optional<uint32_t> number = numbers.numberOf(space, key);
        if (!number || *number >= sizes.numberOfSiteCaches)
            return std::nullopt;
        if (ImageNumbers::isPlaceWithSiteAnswers(key))
            return static_cast<int64_t>(InstanceLayout::offsetOfIndexOfSiteAnswers(sizes, *number));
        return static_cast<int64_t>(InstanceLayout::offsetOfSiteCache(sizes, *number));
    }
    case NumberSpace::Variable:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::HeirLookedAt:
        return byteOfStatics(offsetOfMentionedWordInStatics(numbers, space, key));
    case NumberSpace::CardOfVariable:
        return cardOfStatics(offsetOfMentionedWordInStatics(numbers, NumberSpace::Variable, key));
    case NumberSpace::CardOfWord:
        if (key.size() < 2 || key[0] >= numberOfNumberSpaces)
            return std::nullopt;
        return cardOfStatics(offsetOfMentionedWordInStatics(numbers, static_cast<NumberSpace>(key[0]), key.subspan(1)));
    case NumberSpace::ClassGuard: {
        std::optional<uint32_t> bit = numbers.numberOf(space, key);
        if (!bit || maskOfClassGuardBit(*bit) != 1)
            return std::nullopt;
        return byteOfStatics(byteOffsetOfClassGuardBit(*bit));
    }
    case NumberSpace::ClosureCell:
        return std::nullopt;
    case NumberSpace::MethodSelector: {
        std::optional<uint32_t> origin = numbers.numberOf(space, key);
        if (!origin)
            return std::nullopt;
        return static_cast<int64_t>(InstanceLayout::offsetOfMethodTable(sizes)) + static_cast<int64_t>(*origin) * static_cast<int64_t>(sizeof(uint32_t));
    }
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::KeyCell:
    case NumberSpace::BigInt:
    case NumberSpace::RequireEdge:
    case NumberSpace::DataElements:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::PropertyRun:
    case NumberSpace::Type:
    case NumberSpace::RegExp:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ImageNumbers::ImageNumbers()
    : m_identifiers(makeUniqueWithoutFastMallocCheck<IdentifierNumbering>())
    , m_stringConstants(makeUniqueWithoutFastMallocCheck<StringConstantNumbering>())
    , m_regExpRequests(makeUniqueWithoutFastMallocCheck<RegExpRequests>())
{
}

ImageNumbers::~ImageNumbers() = default;

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<uint32_t> ImageNumbers::numberOfInLine(NumberSpace space, std::span<const uint32_t> key) const
{
    if (key.empty() || (hasOneIDAsKey(space) && key.size() != 1))
        return std::nullopt;

    switch (space) {
    case NumberSpace::Name:
        return m_identifiers->numberOf(PropertyKeyID(key[0]));
    case NumberSpace::NameOfText:
        return m_identifiers->numberOfText(StringID(key[0]));
    case NumberSpace::String:
    case NumberSpace::ShownString:
        return m_stringNumbers.numberOf(StringID(key[0]));
    case NumberSpace::StringCell:
        return m_stringConstants->numberOf(StringID(key[0]));
    case NumberSpace::AtomOfString:
    case NumberSpace::StringCharacters:
        return std::nullopt;
    case NumberSpace::NameHeader: {
        std::optional<uint32_t> number = m_identifiers->numberOfText(StringID(key[0]));
        return number ? std::optional(*number * m_bytesBetweenNameHeaders) : std::nullopt;
    }
    case NumberSpace::KeyCell: {
        std::optional<uint32_t> numberPlusOne = numberIn(m_keyCellNumbersPlusOne, PropertyKeyID(key[0]));
        if (!numberPlusOne)
            return std::nullopt;
        return *numberPlusOne - 1;
    }
    case NumberSpace::BuiltinCell:
        return placeAmongNumberedThings(builtinCells(), key[0], &BuiltinCellKey::encoded);
    case NumberSpace::BigInt:
        return placeAmongNumberedThings(bigIntConstants(), StringID(key[0]), std::identity { });
    case NumberSpace::GlobalBinding:
        if (key.size() != 2)
            return std::nullopt;
        return placeAmongNumberedThings(globalBindings(), std::pair<uint32_t, uint32_t>(key[0], key[1]), [](const PlacedGlobalBinding& binding) {
            return std::pair<uint32_t, uint32_t>(binding.key.index(), binding.flags.toRaw());
        });
    case NumberSpace::SiteCache: {
        std::optional<uint32_t> place = m_siteCaches.find(key);
        return place ? std::optional(m_numbersOfSites[*place]) : std::nullopt;
    }
    case NumberSpace::RequireEdge:
        if (key.size() != 3)
            return std::nullopt;
        return placeAmongNumberedThings(requireEdges(), RequireEdge { ModuleID(key[0]), ModuleID(key[1]), StringID(key[2]) }, std::identity { });
    case NumberSpace::Variable:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::HeirLookedAt:
        return offsetOfMentionedWordInStatics(*this, space, key);
    case NumberSpace::DataElements: {
        if (key[0] >= m_nodesOfData.size() || !m_nodesOfData[key[0]])
            return std::nullopt;
        uint64_t offset = (static_cast<uint64_t>(m_nodesOfData[key[0]]) + DataNodeHeader::wordsBeforeElements) * sizeof(uint64_t);
        if (offset > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        return static_cast<uint32_t>(offset);
    }
    case NumberSpace::CardOfVariable:
    case NumberSpace::CardOfWord:
        return std::nullopt;
    case NumberSpace::MethodSelector:
        return placedOffsetAt(m_originsOfMethodSelectors, key[0]);
    case NumberSpace::ClassGuard: {
        constexpr uint64_t numberOfSides = numberOfEnumerators<ClassSide>;
        if (key.size() == 4) {
            const Vector<PlacedClassGuardOfName>& guards = askerOfClassGuardKey(key) == AskerOfClassGuardOfName::AssignedMember ? m_statics.guardsOfAssignedMembers : m_statics.classGuardsOfNames;
            std::optional<uint32_t> place = placeAmongNumberedThings(guards.span(), std::array<uint32_t, 4> { key[0], key[1], key[2], key[3] }, &PlacedClassGuardOfName::key);
            if (!place)
                return std::nullopt;
            return guards[*place].bit;
        }
        if (key.size() > 4) {
            std::optional<uint32_t> place = m_classGuards.find(key);
            if (!place || *place >= m_statics.bitsOfClassGuards.size())
                return std::nullopt;
            return m_statics.bitsOfClassGuards[*place];
        }
        if (key.size() != 2 || key[1] >= numberOfSides)
            return std::nullopt;
        uint64_t index = key[0] * numberOfSides + key[1];
        if (index >= m_statics.classGuardBits.size() || m_statics.classGuardBits[index] == noClassGuardBit)
            return std::nullopt;
        return m_statics.classGuardBits[index];
    }
    case NumberSpace::ClassRank:
        return rankOfClass(LayoutID(key[0]));
    case NumberSpace::ClosureFunction: {
        std::optional<uint32_t> unit = unitIndexOfClosureEntry(key[0]);
        if (!unit)
            return std::nullopt;
        FunctionID entry = m_functions.idOf(*unit);
        if (!entry || entry < m_functions.firstClosureFunction() || entry >= m_functions.endOfClosureFunctions())
            return std::nullopt;
        return m_functions.closureNumberOf(entry);
    }
    case NumberSpace::ClosureCell: {
        uint32_t number = closureCellNumber(FunctionID(key[0]));
        if (!number)
            return std::nullopt;
        return number;
    }
    case NumberSpace::Allocation:
        return m_allocations.find(key);
    case NumberSpace::Request: {
        std::optional<uint32_t> place = m_requests.find(key);
        if (!place || *place >= m_offsetsOfRequests.size())
            return std::nullopt;
        return m_offsetsOfRequests[*place];
    }
    case NumberSpace::PropertyRun:
        return m_propertyRuns.find(key);
    case NumberSpace::Type: {
        if (key.size() != 1)
            return std::nullopt;
        std::optional<uint32_t> place = placeOfType(TypeID::fromBits(key[0]));
        if (!place || *place >= m_offsetsOfTypes.size())
            return std::nullopt;
        return m_offsetsOfTypes[*place];
    }
    case NumberSpace::RegExp: {
        using FlagBits = OptionSet<Yarr::Flags>::StorageType;
        if (key.size() != 2 || key[1] > std::numeric_limits<FlagBits>::max())
            return std::nullopt;
        RegExpRecordID record = m_regExpRequests->recordOf(StringID(key[0]), OptionSet<Yarr::Flags>::fromRaw(static_cast<FlagBits>(key[1])));
        if (!record)
            return std::nullopt;
        return record.index();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> ImageNumbers::numberOf(NumberSpace space, std::span<const uint32_t> key) const
{
    return numberOfInLine(space, key);
}

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<uint32_t> ImageNumbers::valueOf(const Mention& mention, std::span<const uint32_t> mentionWords, bool asInstanceWord) const
{
    if (asInstanceWord || mention.numberOfWords != 1)
        return valueOfSlowCase(mention, mentionWords, asInstanceWord);

    uint32_t id = mention.firstWord;
    std::optional<uint32_t> number;
    switch (mention.space) {
    case NumberSpace::Name:
        number = m_identifiers->numberOf(PropertyKeyID(id));
        break;
    case NumberSpace::NameOfText:
        number = m_identifiers->numberOfText(StringID(id));
        break;
    case NumberSpace::String:
    case NumberSpace::ShownString:
        number = m_stringNumbers.numberOf(StringID(id));
        break;
    case NumberSpace::StringCell:
        number = m_stringConstants->numberOf(StringID(id));
        break;
    default:
        return valueOfSlowCase(mention, mentionWords, asInstanceWord);
    }
    if (!number)
        return std::nullopt;
    return *number + static_cast<uint32_t>(mention.addend);
}

std::optional<uint32_t> ImageNumbers::valueOfSlowCase(const Mention& mention, std::span<const uint32_t> mentionWords, bool asInstanceWord) const
{
    std::span<const uint32_t> key = keyOf(mention, mentionWords);
    if (!asInstanceWord) {
        std::optional<uint32_t> number = numberOfInLine(mention.space, key);
        if (!number)
            return std::nullopt;
        if (mention.space == NumberSpace::SiteCache)
            number = safeCast<uint32_t>(InstanceLayout::offsetOfIndexOfSiteAnswers(m_instanceSizes, *number));
        return *number + static_cast<uint32_t>(mention.addend);
    }

    std::optional<int64_t> distanceToWord = distanceFromInstanceToMentionedWord(*this, mention.space, key);
    if (!distanceToWord)
        return std::nullopt;
    int64_t distance = *distanceToWord + mention.addend;
    if (distance < std::numeric_limits<int32_t>::min() || distance > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(static_cast<int32_t>(distance));
}

NamesOfTables::NamesOfTables(const Typed::Program& program)
    : keys(program.types.numberOfPropertyKeys())
    , textsNamedAsKeys(program.strings.size())
    , strings(program.strings.size())
    , stringsWithOtherUsesThanShowing(program.strings.size())
    , modulePathStrings(program.strings.size())
    , stringConstants(program.strings.size())
    , bigInts(program.strings.size())
    , variables(program.variables.size())
    , functionsWithClosureCell(program.functions.size())
{
}

void NamesOfTables::merge(const NamesOfTables& other)
{
    keys.merge(other.keys);
    textsNamedAsKeys.merge(other.textsNamedAsKeys);
    strings.merge(other.strings);
    stringsWithOtherUsesThanShowing.merge(other.stringsWithOtherUsesThanShowing);
    modulePathStrings.merge(other.modulePathStrings);
    stringConstants.merge(other.stringConstants);
    bigInts.merge(other.bigInts);
    allocations.appendVector(other.allocations);
    types.appendVector(other.types);
    variables.merge(other.variables);
    functionsWithClosureCell.merge(other.functionsWithClosureCell);
}

StringNumbers::StringNumbers(const BitVector& named, Vector<StringID>&& sortedCodedStrings, size_t numberOfStrings, const IdentifierNumbering* identifiers)
    : m_numbers(FillWith { }, numberOfStrings, noNumber)
    , m_numberOfWords(identifiers ? identifiers->count() : 0)
    , m_coded(WTF::move(sortedCodedStrings))
{
    for (uint32_t number = 1; number < m_numberOfWords; ++number) {
        StringID text = identifiers->at(number).text;
        if (text && identifiers->numberOfText(text) == number)
            m_numbers[text.index()] = number;
    }
    BitVector isCoded(numberOfStrings);
    for (StringID id : m_coded)
        isCoded.quickSet(id.index());
    for (size_t index : named) {
        if (m_numbers[index] == noNumber && !isCoded.quickGet(index))
            m_numbers[index] = m_numberOfWords++;
    }
    uint32_t number = m_numberOfWords;
    for (StringID id : m_coded)
        m_numbers[id.index()] = number++;
}

static uint64_t countThingsNumbered(const ImageNumbers& numbers)
{
    const PlacedStatics& statics = numbers.statics();
    auto numberPlaced = [](const Vector<uint32_t>& places, uint32_t noPlace) {
        return static_cast<uint64_t>(places.size()) - static_cast<uint64_t>(std::ranges::count(places, noPlace));
    };

    uint64_t count = numbers.identifiers().count() - 1;
    count += numbers.namedStrings().bitCount();
    count += numbers.stringConstants().count();
    count += numbers.regExpRequests().all().size();
    count += numbers.functions().numberOfFunctions();
    count += numbers.numberOfClosureCells();
    count += numbers.keyCells().size();
    count += numbers.builtinCells().size();
    count += numbers.bigIntConstants().size();
    count += numbers.globalBindings().size();
    count += numbers.siteCaches().size();
    count += numbers.requireEdges().size();
    count += numbers.allocations().size();
    count += numbers.requests().size();
    count += numbers.propertyRuns().size();
    count += numbers.typesNeededAtRunTime().size();
    count += static_cast<uint64_t>(std::ranges::count_if(statics.variables, &PlacedVariable::hasWordInStatics));
    count += statics.moduleBuiltinCells.size();
    count += numberPlaced(statics.moduleValueOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.moduleNamespaceOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.dataStorageOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.regExpLiteralCellOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.templateObjectCellOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.arrayProfileOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.readGroupCacheOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.environmentHandedBackOffsets, PlacedStatics::noOffset);
    count += numberPlaced(statics.structuresOfHeirOffsets, PlacedStatics::noOffset);
    count += numberPlaced(numbers.originsOfMethodSelectors(), PlacedStatics::noOffset);
    count += numberPlaced(statics.classGuardBits, noClassGuardBit);
    count += statics.classGuardsOfNames.size();
    count += statics.guardsOfAssignedMembers.size();
    count += statics.classGuardCounts.size();
    const Vector<PlacedClassGuardOfName>& namesOfSharedGuards = statics.namesOfClassGuardsOfSeveralNames;
    for (size_t index = 0; index < namesOfSharedGuards.size(); ++index)
        count += !index || namesOfSharedGuards[index].bit != namesOfSharedGuards[index - 1].bit;
    return count;
}

static void runStepsOfNumbering(ImageNumbersBuilder& builder, ASCIILiteral name, std::initializer_list<void (ImageNumbersBuilder::*)()> steps)
{
    StepTimes times(builder.context, "image: number"_s);
    for (void (ImageNumbersBuilder::*step)() : steps) {
        if (builder.context.diagnostics().hasErrors())
            continue;
        CurrentWorkScope currentWork("image"_s);
        (builder.*step)();
    }
    times.didFinish(name);
}

void collectRequestsForRegExps(const NumberingInputs& inputs, GatheredMentions& mentions, const NamesOfTables& tables, ImageNumbers& numbers, BuildContext& context)
{
    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    runStepsOfNumbering(builder, "collect the requests for regular expressions"_s, { &ImageNumbersBuilder::collectRegExpRequests });
}

void numberAllButIdentifiersAndStrings(const NumberingInputs& inputs, GatheredMentions& mentions, const NamesOfTables& tables, ImageNumbers& numbers, BuildContext& context)
{
    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    runLanes(context.parallelLoops(), {
        [&] { runStepsOfNumbering(builder, "collect names and strings with text"_s, { &ImageNumbersBuilder::collectNamesAndStringsWithText }); },
        [&] { runStepsOfNumbering(builder, "number rows"_s, { &ImageNumbersBuilder::numberRows }); },
        [&] { runStepsOfNumbering(builder, "number sites"_s, { &ImageNumbersBuilder::numberSites }); },
        [&] { runStepsOfNumbering(builder, "number types"_s, { &ImageNumbersBuilder::numberTypes }); },
        [&] { runStepsOfNumbering(builder, "number closure cells, class ranks"_s, { &ImageNumbersBuilder::checkEntriesOfMentionedClosures, &ImageNumbersBuilder::numberClosureCellsAndClassRanks }); },
        [&] { runStepsOfNumbering(builder, "number constants"_s, { &ImageNumbersBuilder::numberConstants }); },
    });
}

void numberIdentifiersAndStrings(const NumberingInputs& inputs, GatheredMentions& mentions, const NamesOfTables& tables, ImageNumbers& numbers, BuildContext& context)
{
    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    runStepsOfNumbering(builder, "number identifiers and strings"_s, { &ImageNumbersBuilder::numberIdentifiersAndStrings });
}

void layOutStaticsOfMentions(const NumberingInputs& inputs, GatheredMentions& mentions, const NamesOfTables& tables, ImageNumbers& numbers, BuildContext& context)
{
    if (context.diagnostics().hasErrors())
        return;
    StepTimes times(context, "image: number"_s);
    {
        ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
        CurrentWorkScope currentWork("image"_s);
        builder.layOutStatics();
    }
    times.didFinish("lay out statics"_s);
    if (context.diagnostics().hasErrors())
        return;

    InstanceSizes sizes = numbers.instanceSizes();
    if (sizes.sizeOfStatics != numbers.statics().size || sizes.numberOfStringCells != numbers.stringConstants().count() || sizes.numberOfGlobalBindings != numbers.globalBindings().size() || sizes.numberOfSiteCaches != numbers.numbersOfSites().back()) {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .detail = "the sizes of the instance are not those of the statics, the string constants, the global bindings and the site caches, so no distance from the instance would be right"_s,
        });
        return;
    }
    if (!InstanceLayout::isAddressable(sizes)) {
        context.reportLimitExceeded(LimitID::MaxStaticsSize, std::max(InstanceLayout::bytesReservedBelowInstance(sizes), InstanceLayout::bytesReservedFromInstance(sizes)));
        return;
    }
    context.statistics().count(StatisticID::ThingsNumbered, countThingsNumbered(numbers));
}

void numberMentions(const NumberingInputs& inputs, GatheredMentions& mentions, const NamesOfTables& tables, ImageNumbers& numbers, BuildContext& context)
{
    ImageNumbersBuilder::numberFunctions(inputs, numbers, context);
    collectRequestsForRegExps(inputs, mentions, tables, numbers, context);
    numberAllButIdentifiersAndStrings(inputs, mentions, tables, numbers, context);
    numberIdentifiersAndStrings(inputs, mentions, tables, numbers, context);
    layOutStaticsOfMentions(inputs, mentions, tables, numbers, context);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
