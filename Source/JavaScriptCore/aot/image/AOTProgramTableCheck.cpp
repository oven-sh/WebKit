/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTClassGuards.h"
#include "AOTClassMemberTable.h"
#include "AOTFieldTable.h"
#include "AOTGlobalBindings.h"
#include "AOTIRProgram.h"
#include "AOTObjectVisiting.h"
#include "AOTObjectWords.h"
#include "AOTPlan.h"
#include "AOTPrefixCode.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeType.h"
#include "AOTStringUses.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"
#include "IndexingType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

namespace {

struct ElementOfProgramTable {
    ASCIILiteral table;
    uint32_t size;
    uint32_t alignment;
};

struct BoundsOfTablePart {
    uint64_t start { 0 };
    uint64_t end { 0 };
};

struct JoinedTables {
    bool expect(bool holds, ASCIILiteral rule, RowOwner owner = { }, ASCIILiteral subject = { }) const
    {
        if (!holds)
            encoder.fail(rule, owner, subject.isNull() ? String() : String(subject));
        return holds;
    }

    bool liesInPart(TablePartID part, uint64_t offset, uint64_t size, size_t alignment) const
    {
        BoundsOfTablePart bounds = parts[static_cast<unsigned>(part)];
        return !(offset % alignment) && offset >= bounds.start && offset + size <= bounds.end;
    }

    template<typename Record>
    const Record* tryGetRecord(TablePartID part, uint32_t offset, uint64_t trailingSize = 0) const
    {
        if (!liesInPart(part, offset, sizeof(Record) + trailingSize, alignof(Record)))
            return nullptr;
        return &reinterpretCastSpanStartTo<Record>(bytes.subspan(offset));
    }

    std::span<const uint32_t> tryGetWords(TablePartID part, uint32_t offset, uint64_t count) const
    {
        if (!liesInPart(part, offset, count * sizeof(uint32_t), alignof(uint32_t)))
            return { };
        return spanReinterpretCast<const uint32_t>(bytes.subspan(offset, count * sizeof(uint32_t)));
    }

    std::span<const uint32_t> tryGetCountedWords(TablePartID part, uint32_t offset) const
    {
        std::span<const uint32_t> count = tryGetWords(part, offset, 1);
        std::span<const uint32_t> list = count.empty() ? count : tryGetWords(part, offset, 1 + static_cast<uint64_t>(count[0]));
        return list.empty() ? list : list.subspan(1);
    }

    StringID stringWithNumber(uint32_t number) const { return number < stringsByNumber.size() ? stringsByNumber[number] : StringID(); }

    uint32_t offsetOfType(TypeID type) const
    {
        std::optional<uint32_t> place = numbers.placeOfType(type);
        return place ? tables.offsetOfType(*place) : 0;
    }

    uint32_t guardBitInStatics(LayoutID classLayout, unsigned side) const
    {
        const Vector<uint32_t>& bits = numbers.statics().classGuardBits;
        size_t index = static_cast<size_t>(classLayout.index()) * numberOfClassSides + side;
        return index < bits.size() ? bits[index] : noClassGuardBit;
    }

    const TableEncoder& encoder;
    const Typed::Program& program;
    const Typed::Plan& plan;
    const ImageNumbers& numbers;
    const ProgramTables& tables;
    std::span<const uint8_t> bytes;
    std::array<BoundsOfTablePart, numberOfTableParts> parts;
    Vector<StringID> stringsByNumber;
};

struct ExpectedTypeRecord {
    uint16_t primitives { 0 };
    std::optional<Typed::Range> integers;
    Vector<Typed::CellTest, 4> cellTests;
    Vector<StringID, 4> literalStrings;
};

enum class TypeRecordFault : uint8_t { None, LiesOutsidePart, DiffersFromNumbers, DiffersFromPlan };

} // anonymous namespace

static constexpr std::array<ElementOfProgramTable, numberOfProgramTables> elementsOfProgramTables { {
#define AOT_ELEMENT_OF_PROGRAM_TABLE(accessor, ElementType) { #accessor ""_s, sizeof(ElementType), alignof(ElementType) },
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_ELEMENT_OF_PROGRAM_TABLE)
#undef AOT_ELEMENT_OF_PROGRAM_TABLE
} };

static bool runsCheckOfProgramTables()
{
    return ASSERT_ENABLED || ASAN_ENABLED;
}

static bool ascendsStrictlyInTable(const auto& entries, const auto& keyOf)
{
    return std::ranges::adjacent_find(entries, [](const auto& a, const auto& b) { return !(a < b); }, keyOf) == entries.end();
}

static bool checkRangesOfProgramTables(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramTables)) {
        encoder.fail("TableOutOfBounds"_s);
        return false;
    }
    std::span<const TableRange> ranges(reinterpretCastSpanStartTo<ProgramTables>(bytes).ranges);
    bool everyTableCanBeRead = true;
    for (unsigned index = 0; index < numberOfProgramTables; ++index) {
        const ElementOfProgramTable& element = elementsOfProgramTables[index];
        TableRange range = ranges[index];
        ASCIILiteral brokenRule;
        if (range.offset < sizeof(ProgramTables))
            brokenRule = "TableNotWritten"_s;
        else if (range.offset % element.alignment || range.offset + static_cast<uint64_t>(range.count) * element.size > bytes.size())
            brokenRule = "TableOutOfBounds"_s;
        if (brokenRule.isNull())
            continue;
        encoder.fail(brokenRule, { }, String(element.table));
        everyTableCanBeRead = false;
    }
    return everyTableCanBeRead;
}

static BoundsOfTablePart landmarkOfTablePart(const ProgramTables& tables, TablePartID part)
{
    auto boundsOf = [&](ProgramTableIndex index) -> BoundsOfTablePart {
        TableRange range = std::span<const TableRange>(tables.ranges)[static_cast<unsigned>(index)];
        return { range.offset, range.offset + static_cast<uint64_t>(range.count) * elementsOfProgramTables[static_cast<unsigned>(index)].size };
    };
    switch (part) {
    case TablePartID::Header:
        return { 0, sizeof(ProgramTables) };
    case TablePartID::Types:
        return boundsOf(ProgramTableIndex::types);
    case TablePartID::Data:
        return boundsOf(ProgramTableIndex::data);
    case TablePartID::Layouts:
        return boundsOf(ProgramTableIndex::layouts);
    case TablePartID::FieldTable:
        return boundsOf(ProgramTableIndex::fieldTable);
    case TablePartID::Statics:
        return boundsOf(ProgramTableIndex::staticsLayout);
    case TablePartID::Functions:
        return boundsOf(ProgramTableIndex::functionRecordNumbers);
    case TablePartID::FunctionSources:
        return boundsOf(ProgramTableIndex::functionSources);
    case TablePartID::Modules:
        return boundsOf(ProgramTableIndex::modules);
    case TablePartID::Requests:
        return boundsOf(ProgramTableIndex::requests);
    case TablePartID::Constants:
        return boundsOf(ProgramTableIndex::keyCellIdentifiers);
    case TablePartID::ShapeSets:
        return boundsOf(ProgramTableIndex::shapeSetBits);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool findBoundsOfTableParts(JoinedTables& check)
{
    std::array<BoundsOfTablePart, numberOfTableParts> landmarks;
    for (unsigned part = 0; part < numberOfTableParts; ++part)
        landmarks[part] = landmarkOfTablePart(check.tables, static_cast<TablePartID>(part));
    bool areInOrder = true;
    for (unsigned part = 0; part < numberOfTableParts; ++part) {
        BoundsOfTablePart& bounds = check.parts[part];
        bounds.start = part ? landmarks[part - 1].end : 0;
        bounds.end = part + 1 < numberOfTableParts ? landmarks[part + 1].start : check.bytes.size();
        areInOrder &= bounds.start <= landmarks[part].start && landmarks[part].end <= bounds.end;
    }
    return check.expect(areInOrder, "TablePartsOutOfOrder"_s);
}

static Vector<StringID> findStringsByNumberInTables(const TableEncoder& encoder)
{
    Vector<StringID> strings;
    for (size_t index : encoder.numbers.namedStrings()) {
        uint32_t number = encoder.stringNumber(StringID(index));
        if (number >= strings.size())
            strings.grow(number + 1);
        strings[number] = StringID(index);
    }
    return strings;
}

static bool checkHeaderOfProgramTables(const JoinedTables& check)
{
    using WordsOfInstanceSizes = std::array<uint32_t, sizeof(InstanceSizes) / sizeof(uint32_t)>;
    constexpr uint32_t everyFlag = ProgramTables::mayRunInMultipleInstances | ProgramTables::neverReadsLegacyRegExpStatics;
    const ProgramTables& tables = check.tables;
    const Typed::ShapeTable& shapes = check.plan.shapes();
    const FunctionNumbers& functions = check.numbers.functions();
    bool isHeader = tables.magic == ProgramTables::expectedMagic && !(tables.flags & ~everyFlag);
    bool agreesWithPlan = tables.numberOfShapes == shapes.numberOfShapes() && tables.firstEnvironmentShape == shapes.firstEnvironmentShape() && tables.numberOfLayoutIndices == check.program.layouts.size();
    bool agreesWithNumbers = std::bit_cast<WordsOfInstanceSizes>(tables.instanceSizes) == std::bit_cast<WordsOfInstanceSizes>(check.numbers.instanceSizes())
        && tables.closureFunctions.first == functions.firstClosureFunction().index() && tables.closureFunctions.end == functions.endOfClosureFunctions().index();
    bool holds = check.expect(isHeader, "HeaderNotWritten"_s);
    holds &= check.expect(agreesWithPlan, "HeaderDiffersFromPlan"_s);
    holds &= check.expect(agreesWithNumbers, "HeaderDiffersFromNumbers"_s);
    return holds;
}

static bool holdsGroupsAndWordsOfSetBits(std::span<const uint32_t> words, size_t numberOfGroupWords)
{
    if (words.size() < numberOfGroupWords)
        return false;
    size_t numberOfRecords = 0;
    for (size_t group = 0; group < numberOfGroupWords; group += 2) {
        if (words[group + 1] != numberOfRecords)
            return false;
        numberOfRecords += std::popcount(words[group]);
    }
    return words.size() == numberOfGroupWords + numberOfRecords;
}

static bool checkIndexOfTypeRecords(const JoinedTables& check)
{
    std::span<const uint32_t> words = check.tables.types();
    size_t numberOfTypes = check.numbers.typesNeededAtRunTime().size();
    size_t numberOfGroupWords = ProgramTables::numberOfTypeGroupWords(numberOfTypes);
    bool listsEveryType = !words.empty() && words[0] == numberOfTypes && holdsGroupsAndWordsOfSetBits(words.subspan(1), numberOfGroupWords) && words.size() == 1 + numberOfGroupWords + numberOfTypes;
    return check.expect(listsEveryType, "TableOutOfBounds"_s, { }, "types"_s);
}

template<typename Entry>
static void checkNamesOfModule(const JoinedTables& check, std::span<const Entry> list, uint32_t first, uint32_t end)
{
    if (!check.expect(first <= end && end <= list.size(), "TableOutOfBounds"_s))
        return;
    const Typed::StringTable& strings = check.program.strings;
    const IdentifierNumbering& identifiers = check.numbers.identifiers();
    StringID previous;
    for (const Entry& entry : list.subspan(first, end - first)) {
        StringID name;
        if (entry.name < identifiers.count())
            name = identifiers.at(entry.name).text;
        else if (std::is_same_v<Entry, ModuleExportData> && entry.name < check.stringsByNumber.size())
            name = check.stringsByNumber[entry.name];
        if (!check.expect(!!name, "NameWithoutIdentifier"_s))
            return;
        Vector<char16_t> previousBuffer;
        Vector<char16_t> nameBuffer;
        if (!check.expect(!previous || codePointCompare(strings.viewOf(previous, previousBuffer), strings.viewOf(name, nameBuffer)) < 0, "TableNotSorted"_s))
            return;
        previous = name;
    }
}

static void checkModuleTables(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const Typed::StringTable& strings = check.program.strings;
    std::span<const ModuleData> modules = tables.modules();
    size_t numberOfModules = check.program.modules.size();
    if (!check.expect(modules.size() == numberOfModules + 1, "TableOutOfBounds"_s, { }, "modules"_s))
        return;
    std::span<const ModuleExportData> exports = tables.moduleExports();
    for (size_t index = 0; index < numberOfModules; ++index) {
        uint32_t endOfExports = modules[index + 1].firstExport;
        while (endOfExports > modules[index].firstExport && endOfExports <= exports.size() && exports[endOfExports - 1].isHidden())
            --endOfExports;
        checkNamesOfModule(check, exports, modules[index].firstExport, endOfExports);
        checkNamesOfModule(check, tables.syntheticExports(), modules[index].firstSyntheticExport, modules[index + 1].firstSyntheticExport);
    }
    auto everyModuleExists = [&](ModuleNumbers list) {
        for (uint32_t module : list) {
            if (module >= numberOfModules)
                return false;
        }
        return true;
    };
    auto checkRequestLists = [&](std::span<const uint16_t> words, Number24 ModuleData::* first, ASCIILiteral table) {
        ModuleNumbers lists(words, modules.size());
        bool hasValidStarts = std::ranges::is_sorted(modules, { }, [&](const ModuleData& module) { return static_cast<uint32_t>(module.*first); }) && modules.back().*first == lists.size();
        return check.expect(hasValidStarts && everyModuleExists(lists), "TableOutOfBounds"_s, { }, table);
    };
    bool canReadLinkRequests = checkRequestLists(tables.linkRequests(), &ModuleData::firstLinkRequest, "linkRequests"_s);
    ModuleNumbers linkRequests(tables.linkRequests(), modules.size());
    for (size_t index = 0; canReadLinkRequests && index < numberOfModules; ++index) {
        ModuleNumbers list = linkRequests.subspan(modules[index].firstLinkRequest, modules[index + 1].firstLinkRequest - modules[index].firstLinkRequest);
        bool namesNoModuleThatOnlyPassesOn = std::ranges::none_of(list, [&](uint32_t module) { return modules[module].onlyPassesLinkOn(); });
        bool namesItsLeader = list.size() == 1 && modules[list[0]].needsLink() && modules[list[0]].linkFollowsRequests();
        check.expect(namesNoModuleThatOnlyPassesOn && (!modules[index].onlyPassesLinkOn() || namesItsLeader), "LinkRequestNamesModuleThatOnlyPassesOn"_s, { }, "linkRequests"_s);
    }
    checkRequestLists(tables.evaluationRequests(), &ModuleData::firstEvaluationRequest, "evaluationRequests"_s);
    ModuleNumbers groupsLoadedWithGroups(tables.groupsLoadedWithGroups(), modules.size());
    auto isHeadOfLoadGroup = [&](uint32_t module) {
        return module < numberOfModules && modules[module].isESModuleOfProgram() && modules[module].isHeadOfLoadGroup();
    };
    for (size_t index = 0; index < numberOfModules; ++index) {
        if (!modules[index].isESModuleOfProgram())
            continue;
        uint32_t number = modules[index].numberInLoadGroup();
        if (!modules[index].isHeadOfLoadGroup()) {
            bool goesByRequests = modules[index].placeInLoadGroup() == ModuleData::PlaceInLoadGroup::BelowHead && number < numberOfModules && (!modules[number].isESModuleOfProgram() || modules[number].placeInLoadGroup() == ModuleData::PlaceInLoadGroup::BelowHead);
            check.expect(isHeadOfLoadGroup(number) || goesByRequests, "TableOutOfBounds"_s, { }, "groupsLoadedWithGroups"_s);
            continue;
        }
        bool hasList = number && number <= groupsLoadedWithGroups.size() && groupsLoadedWithGroups[number - 1] <= groupsLoadedWithGroups.size() - number;
        if (!check.expect(!number || hasList, "TableOutOfBounds"_s, { }, "groupsLoadedWithGroups"_s) || !number)
            continue;
        ModuleNumbers list = groupsLoadedWithGroups.subspan(number, groupsLoadedWithGroups[number - 1]);
        check.expect(std::ranges::all_of(list, [&](uint32_t listed) {
            return isHeadOfLoadGroup(listed) || (listed < numberOfModules && modules[listed].isESModuleOfProgram() && modules[listed].numberInLoadGroup() == listed);
        }), "TableOutOfBounds"_s, { }, "groupsLoadedWithGroups"_s);
    }
    check.expect(std::ranges::all_of(tables.entryPointLinkPlan(), [&](const LinkPlanStepData& step) {
        return step.module < numberOfModules;
    }), "TableOutOfBounds"_s, { }, "entryPointLinkPlan"_s);

    std::span<const uint32_t> starts = tables.moduleStarts();
    std::span<const ModuleStartStepData> steps = tables.moduleStartSteps();
    auto namesWhatExists = [&](ModuleStartStepData step) {
        switch (step.operation()) {
        case ModuleStartStepData::Operation::Trap:
        case ModuleStartStepData::Operation::Return:
            return !step.operand() && !step.hasFlag();
        case ModuleStartStepData::Operation::CallStart:
        case ModuleStartStepData::Operation::CallStartUnlessStopped:
            return step.operand() < starts.size() && starts[step.operand()] && !step.hasFlag();
        case ModuleStartStepData::Operation::CallBody:
            return step.operand() < check.encoder.inputs.functions.size() && !step.hasFlag();
        case ModuleStartStepData::Operation::Begin:
        case ModuleStartStepData::Operation::BeginOrReturn:
        case ModuleStartStepData::Operation::Finish:
        case ModuleStartStepData::Operation::FinishAndReturn:
            return step.operand() < numberOfModules;
        case ModuleStartStepData::Operation::ReturnIfLoadingStops:
        case ModuleStartStepData::Operation::AbandonOnThrow:
            return step.operand() < numberOfModules && !step.hasFlag();
        }
        return false;
    };
    bool everyRowBeginsBehindAnEnd = std::ranges::all_of(starts, [&](uint32_t firstStep) {
        return firstStep < steps.size() && (!firstStep || steps[firstStep - 1].endsRow());
    });
    bool hasRowsOfPlan = starts.empty() ? steps.empty() : starts.size() == numberOfModules && !steps.empty() && !steps.front().bits && steps.back().endsRow();
    check.expect(hasRowsOfPlan && everyRowBeginsBehindAnEnd && std::ranges::all_of(steps, namesWhatExists), "TableOutOfBounds"_s, { }, "moduleStartSteps"_s);

    std::span<const UncompiledModuleData> uncompiledModules = tables.modulesOfUncompiledCode();
    bool listsUncompiledCode = std::ranges::is_sorted(uncompiledModules, { }, &UncompiledModuleData::module) && std::ranges::all_of(uncompiledModules, [&](const UncompiledModuleData& entry) {
        if (entry.module >= numberOfModules || !modules[entry.module].isUncompiledCode())
            return false;
        StringID typeAttribute = check.program.modules[entry.module].typeAttribute;
        return entry.typeAttribute == (typeAttribute ? check.encoder.stringNumber(typeAttribute) : UncompiledModuleData::noTypeAttribute);
    }) && static_cast<size_t>(std::ranges::count_if(modules.first(numberOfModules), &ModuleData::isUncompiledCode)) == uncompiledModules.size();
    check.expect(listsUncompiledCode, "TableOutOfBounds"_s, { }, "modulesOfUncompiledCode"_s);

    std::span<const ModuleDirectoryData> directories = tables.moduleDirectories();
    auto hasPathOfProgram = [&](size_t index) {
        Vector<uint32_t, 16> pieces { modules[index].publicPathOrName };
        for (uint32_t directory = modules[index].directory; directory != Number24::max; directory = directories[directory].parent) {
            if (directory >= directories.size() || pieces.size() > directories.size())
                return false;
            pieces.append(directories[directory].name);
        }
        Vector<uint8_t, 256> path;
        for (uint32_t piece : pieces | std::views::reverse) {
            StringID string = check.stringWithNumber(piece);
            if (!string)
                return false;
            path.append(strings[string]);
        }
        return std::ranges::equal(path, strings[check.program.modules[index].path]);
    };
    ModuleNumbers modulesByPath(tables.modulesByPath(), modules.size());
    bool everyPathExists = everyModuleExists(modulesByPath);
    for (size_t index = 0; everyPathExists && index < numberOfModules; ++index)
        everyPathExists = hasPathOfProgram(index);
    std::span<const ModuleDisplayPathData> displayPaths = tables.moduleDisplayPaths();
    bool everyDisplayPathExists = std::ranges::is_sorted(displayPaths, { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; })
        && std::ranges::all_of(displayPaths, [&](const ModuleDisplayPathData& entry) {
            return entry.module < numberOfModules && modules[entry.module].charactersInFrontOfDisplayPath == ModuleData::displayPathIsListed && check.stringWithNumber(entry.path);
        })
        && static_cast<size_t>(std::ranges::count(modules.first(numberOfModules), ModuleData::displayPathIsListed, &ModuleData::charactersInFrontOfDisplayPath)) == displayPaths.size();
    if (!check.expect(everyPathExists && everyDisplayPathExists, "PathOfModuleDiffersFromProgram"_s))
        return;
    auto pathAt = [&](size_t index) {
        auto characters = byteCast<char>(strings[check.program.modules[modulesByPath[index]].path]);
        return std::string_view { characters.data(), characters.size() };
    };
    bool pathsAscend = true;
    for (size_t index = 1; pathsAscend && index < modulesByPath.size(); ++index)
        pathsAscend = pathAt(index - 1) < pathAt(index);
    check.expect(pathsAscend, "TableNotSorted"_s, { }, "modulesByPath"_s);
    std::span<const uint32_t> entries = tables.modulesByHashOfPath();
    ModuleNumbers first(tables.firstModulesByHashOfPath(), modules.size());
    size_t numberOfBuckets = ModuleByHashOfPath::numberOfBuckets(modulesByPath.size());
    bool everyBucketExists = entries.size() == modulesByPath.size() && first.size() == (numberOfBuckets ? numberOfBuckets + 1 : 0);
    for (size_t bucket = 0; everyBucketExists && bucket < numberOfBuckets; ++bucket)
        everyBucketExists = first[bucket] <= first[bucket + 1] && first[bucket + 1] <= entries.size();
    if (!check.expect(everyBucketExists, "TableOutOfBounds"_s, { }, "modulesByHashOfPath"_s))
        return;
    bool everyModuleIsFound = true;
    for (uint32_t module : modulesByPath) {
        ModuleByHashOfPath sought(modules.size(), StringView(String::fromUTF8(strings[check.program.modules[module].path])).hash());
        size_t bucket = sought.bucket(numberOfBuckets);
        everyModuleIsFound = everyModuleIsFound && std::ranges::contains(entries.subspan(first[bucket], first[bucket + 1] - first[bucket]), sought.entryFor(module));
    }
    check.expect(everyModuleIsFound, "TableNotSorted"_s, { }, "modulesByHashOfPath"_s);
}

static void checkReturnedCaptures(const JoinedTables& check)
{
    std::span<const ReturnedCaptureData> rows = check.tables.returnedCaptures();
    if (rows.empty())
        return;
    if (!check.expect(rows.size() > ReturnedCaptureData::numberOfRowsBehind && !rows.back().closureNumberPlusOne(), "TableOutOfBounds"_s, { }, "returnedCaptures"_s))
        return;
    uint32_t capacity = static_cast<uint32_t>(rows.size() - ReturnedCaptureData::numberOfRowsBehind);
    bool isProbed = true;
    for (size_t place = 0; place < rows.size(); ++place) {
        if (!rows[place].closureNumberPlusOne())
            continue;
        size_t first = ReturnedCaptureData::placeOf(rows[place].closureNumberPlusOne(), capacity);
        isProbed = isProbed && first <= place && std::ranges::all_of(rows.subspan(first, place - first), [](const ReturnedCaptureData& row) { return !!row.closureNumberPlusOne(); });
    }
    check.expect(isProbed, "TableNotSorted"_s, { }, "returnedCaptures"_s);
}

static void checkOrderOfProgramTables(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const uint32_t sizeOfStatics = tables.instanceSizes.sizeOfStatics;
    auto checkAscending = [&](auto entries, const auto& keyOf, ASCIILiteral table) {
        check.expect(ascendsStrictlyInTable(entries, keyOf), "TableNotSorted"_s, { }, table);
    };
    checkAscending(tables.constructors(), &ConstructorData::function, "constructors"_s);
    checkAscending(tables.keyedLookups(), [](const ReturnedCaptureData& row) { return row.closureNumberPlusOne(); }, "keyedLookups"_s);
    checkAscending(tables.constructEntrypoints(), &ConstructEntrypointData::function, "constructEntrypoints"_s);
    checkAscending(tables.implementedBuiltins(), &ImplementedBuiltinData::function, "implementedBuiltins"_s);
    checkAscending(tables.functionsWithLargeCounts(), [](const FunctionWithLargeCountsData& entry) -> uint32_t { return entry.function; }, "functionsWithLargeCounts"_s);
    checkAscending(tables.unsharedFunctionRecords(), &UnsharedFunctionRecordData::function, "unsharedFunctionRecords"_s);
    checkAscending(tables.closurePropertyNames(), std::identity { }, "closurePropertyNames"_s);
    checkAscending(tables.fieldGetters(), &FieldGetterData::function, "fieldGetters"_s);
    checkReturnedCaptures(check);
    auto checkSorted = [&](auto entries, const auto& keyOf, ASCIILiteral table) {
        check.expect(std::ranges::is_sorted(entries, { }, keyOf), "TableNotSorted"_s, { }, table);
    };
    checkSorted(tables.ambiguousExports(), &AmbiguousExportData::module, "ambiguousExports"_s);
    checkSorted(tables.asyncFunctionsWithoutAwait(), std::identity { }, "asyncFunctionsWithoutAwait"_s);
    checkSorted(tables.exportStatements(), &ExportStatementData::requiredName, "exportStatements"_s);
    checkSorted(tables.functionsImportingOwnOriginal(), std::identity { }, "functionsImportingOwnOriginal"_s);
    checkSorted(tables.methodTableDependents(), &MethodTableDependentData::guardBit, "methodTableDependents"_s);
    checkSorted(tables.importedModuleAliases(), std::identity { }, "importedModuleAliases"_s);
    checkSorted(tables.modulesThatDependOnMoreThanSource(), std::identity { }, "modulesThatDependOnMoreThanSource"_s);
    checkSorted(tables.modulesWithReadOnlyBuiltins(), std::identity { }, "modulesWithReadOnlyBuiltins"_s);
    checkSorted(tables.modulesRejectingUncompiledCode(), std::identity { }, "modulesRejectingUncompiledCode"_s);
    checkSorted(tables.addedPrototypeNames(), std::identity { }, "addedPrototypeNames"_s);
    checkSorted(tables.namesOfStatements(), &NameOfStatementData::module, "namesOfStatements"_s);
    checkSorted(tables.requestedOriginals(), &RequestedOriginalData::function, "requestedOriginals"_s);
    checkSorted(tables.quotedSourceGroups(), [](const QuotedSourceGroupData& group) { return std::tuple { group.module, group.line, group.column }; }, "quotedSourceGroups"_s);
    checkSorted(tables.sourceStamps(), &SourceStampData::module, "sourceStamps"_s);
    checkSorted(tables.starsAsWritten(), &StarAsWrittenData::module, "starsAsWritten"_s);
    checkSorted(tables.writersOfExports(), [](const WriterOfExportData& entry) { return static_cast<uint64_t>(entry.module) << 32 | entry.place; }, "writersOfExports"_s);
    checkAscending(tables.staticsRoots(), &StaticsRange::offset, "staticsRoots"_s);
    checkAscending(tables.staticsNarrowRoots(), &StaticsRange::offset, "staticsNarrowRoots"_s);
    checkAscending(tables.staticsRuns(), &StaticsRun::firstWord, "staticsRuns"_s);
    checkAscending(tables.importsOfHeldFiles(), &ImportOfHeldFileData::key, "importsOfHeldFiles"_s);
    checkAscending(tables.compiledRuntimeModules(), &CompiledRuntimeModuleData::registryID, "compiledRuntimeModules"_s);
    checkAscending(tables.compiledRuntimeFunctions(), &CompiledRuntimeFunctionData::id, "compiledRuntimeFunctions"_s);
    checkAscending(tables.stateOfLibraryModules(), [](const StateOfLibraryModuleData& state) { return static_cast<uint64_t>(state.moduleAndKind) << 32 | state.first; }, "stateOfLibraryModules"_s);
    checkAscending(tables.builtinCells(), std::identity { }, "builtinCells"_s);
    checkAscending(tables.eagerBuiltinCells(), std::identity { }, "eagerBuiltinCells"_s);
    checkAscending(tables.classGuardCounts(), std::identity { }, "classGuardCounts"_s);
    checkAscending(tables.classesBelowWithGuardsOfNames(), std::identity { }, "classesBelowWithGuardsOfNames"_s);
    checkAscending(tables.assignedMembers(), &AssignedMemberData::place, "assignedMembers"_s);
    check.expect(std::ranges::all_of(tables.assignedMembers(), [&](const AssignedMemberData& member) {
        uint32_t offset = byteOffsetOfClassGuardBit(member.guardBit);
        return !(offset % sizeof(EncodedJSValue)) && offset + sizeof(EncodedJSValue) <= sizeOfStatics && tables.someClassGuardNames(member.name);
    }), "TableOutOfBounds"_s);

    auto liesInStatics = [&](const StaticsRange& range) {
        return static_cast<uint64_t>(range.offset) + range.size <= sizeOfStatics;
    };
    check.expect(std::ranges::all_of(tables.classGuardCounts(), [&](const ClassGuardCountData& count) {
        return byteOffsetOfClassGuardBit(count.guardBit) < sizeOfStatics && !(count.offsetOfCount % sizeof(uint32_t)) && liesInStatics({ count.offsetOfCount, sizeof(uint32_t) });
    }), "TableOutOfBounds"_s, { }, "classGuardCounts"_s);
    check.expect(std::ranges::all_of(tables.staticsRoots(), liesInStatics) && std::ranges::all_of(tables.staticsNarrowRoots(), liesInStatics) && std::ranges::all_of(tables.staticsClearedEachCollection(), liesInStatics) && std::ranges::all_of(tables.variablesWithViewMirror(), liesInStatics), "TableOutOfBounds"_s, { }, "staticsRoots"_s);
    check.expect(std::ranges::all_of(tables.stateOfLibraryModules(), [&](const StateOfLibraryModuleData& state) {
        uint64_t end = static_cast<uint64_t>(state.first) + state.count;
        switch (state.kind()) {
        case StateOfLibraryModuleData::Kind::Classes:
            return end <= tables.numberOfLayoutIndices;
        case StateOfLibraryModuleData::Kind::Closures:
            return end <= tables.numberOfClosureCells;
        case StateOfLibraryModuleData::Kind::Variables:
            return end <= sizeOfStatics;
        }
        return false;
    }), "TableOutOfBounds"_s, { }, "stateOfLibraryModules"_s);
    check.expect(tables.eagerBuiltinCells().empty() || tables.eagerBuiltinCells().back() < tables.builtinCells().size(), "TableOutOfBounds"_s, { }, "eagerBuiltinCells"_s);

    const BuiltinTable& builtins = check.program.builtins();
    std::span<const uint32_t> openedNames = tables.openedNames();
    bool everyMemberExists = std::ranges::all_of(tables.constantAndUnlockedBuiltinMembers().first(tables.numberOfWordsBelow(ProgramTables::openedName)), [&](uint32_t member) {
        return builtins.contains(BuiltinID(member & ~(ProgramTables::unlockedBuiltinMember | ProgramTables::memberAskedByByteAlone)));
    });
    bool everyOpenedNameExists = !(openedNames.size() % 2);
    for (size_t index = 0; everyOpenedNameExists && index < openedNames.size(); index += 2)
        everyOpenedNameExists = builtins.contains(BuiltinID(openedNames[index] & ~ProgramTables::openedName)) && check.stringWithNumber(openedNames[index + 1] & ~ProgramTables::openedName);
    if (!check.expect(everyMemberExists && everyOpenedNameExists, "TableOutOfBounds"_s, { }, "constantAndUnlockedBuiltinMembers"_s))
        return;
    checkAscending(tables.constantBuiltinMembers(), [&](uint32_t member) {
        return std::pair { builtins.at(BuiltinID(member)).holder, BuiltinID(member) };
    }, "constantAndUnlockedBuiltinMembers"_s);
    checkAscending(tables.membersAskedByByteAlone(), std::identity { }, "constantAndUnlockedBuiltinMembers"_s);
    check.expect(std::ranges::all_of(tables.membersAskedByByteAlone(), [&](uint32_t member) {
        return isMemberAskedByByteAlone(builtins, BuiltinID(member & ~ProgramTables::memberAskedByByteAlone));
    }), "BuiltinMemberIsNotAskedByByteAlone"_s);
    checkAscending(tables.unlockedBuiltinMembers(), std::identity { }, "constantAndUnlockedBuiltinMembers"_s);
    check.expect(std::ranges::none_of(tables.constantBuiltinMembers(), [&](uint32_t member) {
        return ProgramTables::isUnlocked(tables.unlockedBuiltinMembers(), member);
    }), "BuiltinMemberIsConstantAndUnlocked"_s);
}

static void checkRowsNamedByNumber(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const ImageNumbers& numbers = check.numbers;
    auto numberOfString = [&](StringID string) { return check.encoder.stringNumber(string); };
    auto numberOfKey = [&](PropertyKeyID key) { return numbers.identifiers().numberOf(key).value_or(0); };
    auto expectRows = [&](bool agree, ASCIILiteral table) {
        check.expect(agree, "RowsDifferFromNumbers"_s, { }, table);
    };
    expectRows(std::ranges::equal(tables.builtinCells(), numbers.builtinCells(), { }, { }, &BuiltinCellKey::encoded), "builtinCells"_s);
    expectRows(std::ranges::equal(tables.eagerBuiltinCells(), numbers.eagerBuiltinCells()), "eagerBuiltinCells"_s);
    expectRows(std::ranges::equal(tables.membersAskedByByteAlone(), numbers.membersAskedByByteAlone(), { }, [](uint32_t member) { return member & ~ProgramTables::memberAskedByByteAlone; }, &BuiltinID::index), "constantAndUnlockedBuiltinMembers"_s);
    expectRows(std::ranges::equal(tables.bigIntConstants(), numbers.bigIntConstants(), { }, { }, numberOfString), "bigIntConstants"_s);
    expectRows(std::ranges::equal(tables.keyCellIdentifiers(), numbers.keyCells(), { }, { }, numberOfKey), "keyCellIdentifiers"_s);
    bool namesEveryGlobalBinding = std::ranges::equal(tables.globalBindings(), numbers.globalBindings(), { }, [](uint32_t word) { return word & GlobalBindings::identifierMask; }, [&](const PlacedGlobalBinding& binding) { return numberOfKey(binding.key); });
    expectRows(namesEveryGlobalBinding && tables.globalBindings().size() == tables.instanceSizes.numberOfGlobalBindings, "globalBindings"_s);

    const AllocationTable allocations = tables.allocations();
    std::span<const AllocationRow> allocationRows = tables.allocationRows();
    std::span<const uint32_t> layouts = tables.layouts();
    expectRows(allocations.size() == numbers.allocations().size(), "allocations"_s);
    bool everyAllocationHasForm = std::ranges::all_of(allocationRows, [&](const AllocationRow& row) { return row.form < tables.allocationForms().size(); });
    if (!check.expect(everyAllocationHasForm, "TableOutOfBounds"_s, { }, "allocationForms"_s))
        return;
    check.expect(std::ranges::all_of(allocationRows, [&](const AllocationRow& row) {
        return row.shape < layouts.size() && layouts[row.shape];
    }), "ShapeWithoutLayout"_s, { }, "allocations"_s);
    std::span<const ConstructedAllocationData> constructedAllocations = tables.constructedAllocations();
    bool listsConstructedAllocations = ascendsStrictlyInTable(constructedAllocations, [](const ConstructedAllocationData& entry) -> uint32_t { return entry.allocation; })
        && std::ranges::all_of(constructedAllocations, [&](const ConstructedAllocationData& entry) { return entry.allocation < allocations.size() && allocations[entry.allocation].isConstructed; })
        && static_cast<size_t>(std::ranges::count_if(allocationRows, [&](const AllocationRow& row) { return !!tables.allocationForms()[row.form].isConstructed; })) == constructedAllocations.size();
    check.expect(listsConstructedAllocations, "TableNotSorted"_s, { }, "constructedAllocations"_s);

    std::span<const uint32_t> runs = tables.propertyRuns();
    uint32_t numberOfRuns = numbers.propertyRuns().size();
    bool everyRunIsInBounds = !numberOfRuns || (runs.size() > numberOfRuns && std::ranges::is_sorted(runs.first(numberOfRuns + 1)) && runs[0] > numberOfRuns && runs[numberOfRuns] <= runs.size());
    check.expect(everyRunIsInBounds, "TableOutOfBounds"_s, { }, "propertyRuns"_s);
}

static void checkFunctionTables(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const Typed::CallingConventions& conventions = check.plan.conventions();
    const FunctionNumbers& functions = check.numbers.functions();
    size_t numberOfClosureFunctions = tables.closureFunctions.end - tables.closureFunctions.first;
    SparseNumberTable cells(tables.closureCells());
    bool hasRowForEachFunction = tables.numberOfFunctions() == functions.numberOfFunctions() && tables.closureFactNumbers().size() == numberOfClosureFunctions
        && std::ranges::all_of(tables.closureFactNumbers(), [&](uint16_t number) { return number < tables.closureFacts().size(); }) && tables.closureCells().size() >= NumberGroup::numberOfGroupWords(numberOfClosureFunctions);
    if (!check.expect(hasRowForEachFunction, "TableOutOfBounds"_s, { }, "functionRecordNumbers"_s))
        return;
    for (uint32_t number = 0; number < numberOfClosureFunctions; ++number) {
        CodeUnit unit = conventions.unitAt(functions.unitOf(FunctionID(tables.closureFunctionWithNumber(number))));
        uint32_t cell = cells.at(number);
        bool isCellOfFunction = unit.role != Typed::CodeRole::GenericCallEntry || cell == check.numbers.closureCellNumber(unit.function);
        if (!check.expect(isCellOfFunction && (!cell || cell < tables.numberOfClosureCells), "ClosureCellDiffersFromNumbers"_s, unit.function))
            return;
    }
    check.expect(std::ranges::all_of(tables.functionVariables(), [&](const FunctionVariableData& variable) {
        return variable.offset < tables.instanceSizes.sizeOfStatics && variable.function < tables.numberOfFunctions();
    }), "TableOutOfBounds"_s, { }, "functionVariables"_s);

    for (const FunctionSources& sources : tables.functionSources()) {
        uint64_t sizeOfEntries = static_cast<uint64_t>(sources.numberOfEntries) * sizeof(FunctionSourceEntry);
        bool liesInPart = (!sources.numberOfEntries || check.liesInPart(TablePartID::FunctionSources, sources.entriesOffset, sizeOfEntries, alignof(FunctionSourceEntry))) && (!sources.sourceSize || check.liesInPart(TablePartID::FunctionSources, sources.sourceOffset, sources.sourceSize, 1));
        if (!check.expect(liesInPart, "OffsetOutsideItsPart"_s, { }, "functionSources"_s))
            return;
        auto entries = spanReinterpretCast<const FunctionSourceEntry>(check.bytes.subspan(sources.numberOfEntries ? sources.entriesOffset : 0, sizeOfEntries));
        check.expect(std::ranges::is_sorted(entries, { }, [](const FunctionSourceEntry& entry) -> uint32_t { return entry.function; }), "TableNotSorted"_s, { }, "functionSources"_s);
        check.expect(std::ranges::all_of(entries, [&](const FunctionSourceEntry& entry) { return entry.start < entry.end && entry.end <= sources.sourceSize; }), "TableOutOfBounds"_s, { }, "functionSources"_s);
    }
}

static void checkNamesForHeapSnapshots(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    Vector<VariableData> variables;
    uint32_t lastModule = 0;
    uint32_t endOfClosureNumbers = 0;
    bool endsWhereModuleEnds = forEachNamedVariable(decodeVarintBytes(tables.namedVariables()).span(), [&](uint32_t module, size_t) {
        lastModule = module;
    }, [&](const VariableData& variable) {
        variables.append(variable);
    }, [&](uint32_t first, uint32_t length) {
        endOfClosureNumbers = std::max(endOfClosureNumbers, first + length);
    });
    check.expect(endsWhereModuleEnds, "TableNotSorted"_s, { }, "namedVariables"_s);
    check.expect(tables.namedVariables().empty() || lastModule < tables.modules().size(), "TableOutOfBounds"_s, { }, "namedVariables"_s);
    check.expect(endOfClosureNumbers <= tables.closureFunctions.end - tables.closureFunctions.first, "TableOutOfBounds"_s, { }, "namedVariables"_s);
    check.expect(std::ranges::all_of(variables, [&](const VariableData& variable) {
        if (!check.stringWithNumber(variable.name))
            return false;
        if (variable.slot.representation() == SlotRepresentation::DeferredClosure)
            return variable.slot.offset >= tables.closureFunctions.first && variable.slot.offset < tables.closureFunctions.end;
        SlotTraits traits = traitsOf(variable.slot.representation());
        return traits.holdsCell && !(variable.slot.offset % traits.width) && variable.slot.offset + traits.width <= tables.instanceSizes.sizeOfStatics;
    }), "TableOutOfBounds"_s, { }, "namedVariables"_s);

    Vector<NamedCaptureData> captures;
    bool endsWhereClosureNumberEnds = forEachNamedCapture(decodeVarintBytes(tables.namedCaptures()).span(), [&](const NamedCaptureData& capture) {
        captures.append(capture);
    });
    check.expect(endsWhereClosureNumberEnds && ascendsStrictlyInTable(captures.span(), [](const NamedCaptureData& capture) { return capture.closureNumber << 8 | capture.slot; }), "TableNotSorted"_s, { }, "namedCaptures"_s);
    check.expect(std::ranges::all_of(captures, [&](const NamedCaptureData& capture) {
        return tables.hasClosureNumber(capture.closureNumber) && capture.slot < tables.captureCountsOfClosureNumber(capture.closureNumber).references && check.stringWithNumber(capture.name);
    }), "TableOutOfBounds"_s, { }, "namedCaptures"_s);
}

static void checkListedFunctions(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const Typed::CallingConventions& conventions = check.plan.conventions();
    const FunctionNumbers& functions = check.numbers.functions();
    auto functionInImage = [&](CodeUnit unit) {
        return unit && conventions.exists(unit) && check.encoder.hasCode(unit) ? functions.idOf(conventions.indexOf(unit)) : FunctionID();
    };
    for (size_t index = 0; index < check.program.functions.size(); ++index) {
        FunctionID id(index);
        CodeUnit callEntry = conventions.closureEntryOf(id);
        FunctionID entry = functionInImage(callEntry);
        if (!entry || !functionInImage(conventions.genericEntry(callEntry).constructEntry))
            continue;
        bool isListed = entry >= functions.firstClosureFunction() && entry < functions.endOfClosureFunctions() && std::ranges::binary_search(tables.constructEntrypoints(), functions.closureNumberOf(entry), { }, &ConstructEntrypointData::function);
        check.expect(isListed, "FunctionNotListed"_s, id, "constructEntrypoints"_s);
    }
    for (size_t index = 0; index < check.program.layouts.size(); ++index) {
        FunctionID constructor = check.program.layouts[index].constructor;
        if (!constructor || !check.plan.isAllocatable(LayoutID(index)))
            continue;
        std::optional<FunctionID> entry = check.encoder.findEntrypoint(constructor);
        check.expect(!entry || std::ranges::binary_search(tables.constructors(), entry->index(), { }, &ConstructorData::function), "FunctionNotListed"_s, LayoutID(index), "constructors"_s);
    }
    Typed::BitVector isCalledByHost = Typed::functionsCalledByHost(check.program);
    for (const Typed::RuntimeFunctionEntry& listed : check.program.runtimeFunctions) {
        if (!listed.function || !isCalledByHost.get(listed.function.index()) || !functionInImage(conventions.closureEntryOf(listed.function)))
            continue;
        check.expect(std::ranges::binary_search(tables.compiledRuntimeFunctions(), listed.id.index(), { }, &CompiledRuntimeFunctionData::id), "FunctionNotListed"_s, listed.function, "compiledRuntimeFunctions"_s);
        check.expect(check.plan.functionKind(check.program, conventions.closureEntryOf(listed.function)).isInRuntimeModule, "FunctionOutsideRuntimeModule"_s, listed.function, "compiledRuntimeFunctions"_s);
    }
}

template<typename Record>
static bool hasReferenceTableInPart(const JoinedTables& check, uint32_t offset, const Record& record, bool hasExtension)
{
    if (!record.referencesOffset)
        return true;
    int64_t start = static_cast<int64_t>(offset) + record.referencesOffset;
    if (start < 0 || start > UINT32_MAX)
        return false;
    std::span<const uint32_t> counts = check.tryGetWords(TablePartID::Layouts, static_cast<uint32_t>(start), numberOfInlineReferenceCounts);
    if (counts.empty())
        return false;
    uint64_t numberOfWords = numberOfInlineReferenceCounts + static_cast<uint64_t>(counts[0]) + counts[1] + counts[2] + (hasExtension ? numberOfExtensionHeaderWords : 0);
    return !check.tryGetWords(TablePartID::Layouts, static_cast<uint32_t>(start), numberOfWords).empty();
}

static bool hasKeyOrdersInPart(const JoinedTables& check, const LayoutData& layout)
{
    if (!layout.numberOfOrders)
        return true;
    std::span<const uint32_t> orders = check.tryGetWords(TablePartID::Layouts, layout.ordersOffset, layout.numberOfOrders);
    return !orders.empty() && std::ranges::all_of(orders, [&](uint32_t offset) {
        const KeyOrderData* order = offset ? check.tryGetRecord<KeyOrderData>(TablePartID::Layouts, offset) : nullptr;
        return !offset || (order && check.tryGetRecord<KeyOrderData>(TablePartID::Layouts, offset, order->length * sizeof(uint16_t)));
    });
}

static void checkRecordAgainstPlan(const JoinedTables& check, const LayoutData& layout, LayoutID id)
{
    const Typed::SlotPlan& slots = check.plan.slots();
    const Typed::PlannedLayout& planned = slots.layout(id);
    bool hasPlannedForm = layout.size == planned.size && layout.numberOfFields == planned.numberOfRecordFields && !!layout.hasExtensionData == planned.hasExtension() && !!layout.hasExtraPropertiesSlot == planned.hasExtraPropertiesWord();
    if (!check.expect(hasPlannedForm, "RecordDiffersFromPlan"_s, id))
        return;
    bool hasPlannedHiddenWords = (!planned.hasExtension() || (layout.extensionData().extension == slots.extensionWord(id).data() && layout.extensionSize() == planned.extensionSize && layout.granulesOfFirstExtension() == planned.granulesOfFirstExtension))
        && (!planned.hasExtraPropertiesWord() || layout.slotOfExtraProperties() == slots.extraPropertiesWord(id).data());
    check.expect(hasPlannedHiddenWords, "RecordDiffersFromPlan"_s, id);
    std::span<const FieldData> fields = layout.fields();
    for (const Typed::PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
        if (!slot.indexInLayoutRecord || !slot.hasStorage())
            continue;
        if (!check.expect(*slot.indexInLayoutRecord < fields.size(), "RecordDiffersFromPlan"_s, id))
            return;
        const FieldData& field = fields[*slot.indexInLayoutRecord];
        if (!check.expect(field.slot == slot.data() && field.isOutOfLine() == slot.isOutOfLine(), "RecordDiffersFromPlan"_s, id))
            return;
        if (!check.expect(!slot.typeInLayoutRecord() || check.tables.offsetOfTypeOf(field) == check.offsetOfType(slot.typeInLayoutRecord()), "FieldTypeDiffersFromPlan"_s, id))
            return;
        if (!field.holdsRawValueFromBirth)
            continue;
        const RuntimeType* type = check.tryGetRecord<RuntimeType>(TablePartID::Types, check.tables.offsetOfTypeOf(field));
        if (!check.expect(type && !type->primitiveSet().containsAny({ PrimitiveSet::Undefined, PrimitiveSet::Null }), "TypeOfRawFieldTakesMarker"_s, id))
            return;
    }
}

static void checkClassRecord(const JoinedTables& check, const LayoutData& layout, LayoutID id)
{
    const ClassData& classData = layout.classData();
    if (uint32_t offset = classData.subclassesOffset) {
        std::span<const uint32_t> subclasses = check.tryGetCountedWords(TablePartID::Layouts, offset);
        bool areClasses = std::ranges::all_of(subclasses, [&](uint32_t index) {
            return index < check.program.layouts.size() && check.program.layouts[index].kind == LayoutKind::Instance;
        });
        if (check.expect(!subclasses.empty(), "OffsetOutsideItsPart"_s, id) && check.expect(areClasses, "TableOutOfBounds"_s, id)) {
            check.expect(ascendsStrictlyInTable(subclasses, std::identity { }), "TableNotSorted"_s, id);
            check.expect(std::ranges::all_of(subclasses, [&](uint32_t index) {
                uint32_t offsetOfClassBelow = check.tables.offsetOfClass(index);
                const LayoutData* below = offsetOfClassBelow ? check.tryGetRecord<LayoutData>(TablePartID::Layouts, offsetOfClassBelow) : nullptr;
                if (!below || !below->isClass() || !check.liesInPart(TablePartID::Layouts, offsetOfClassBelow - below->prefixSize(), below->prefixSize(), alignof(LayoutData)))
                    return true;
                return !(below->classData().sidesWithGuardsHereOrBelow & ~classData.sidesWithGuardsHereOrBelow);
            }), "ClassDoesNotListenForGuardsBelow"_s, id);
            check.expect(std::ranges::all_of(subclasses, [&](uint32_t index) {
                uint32_t offsetOfClassBelow = check.tables.offsetOfClass(index);
                const LayoutData* below = offsetOfClassBelow ? check.tryGetRecord<LayoutData>(TablePartID::Layouts, offsetOfClassBelow) : nullptr;
                if (!below || !below->isClass() || !check.liesInPart(TablePartID::Layouts, offsetOfClassBelow - below->prefixSize(), below->prefixSize(), alignof(LayoutData)))
                    return true;
                return below->assumedBase() == id.index();
            }), "ClassListedBelowNamesAnotherBase"_s, id);
        }
    }
    const Typed::Layout& written = check.program.at(id);
    bool isKnownByStructure = Typed::isKnownByStructureOfClass(written);
    if (check.expect(!!classData.propertiesOfHeirsOffset == isKnownByStructure, "PropertiesOfHeirsDifferFromProgram"_s, id) && isKnownByStructure) {
        const PropertiesOfHeirsData* properties = check.tryGetRecord<PropertiesOfHeirsData>(TablePartID::Layouts, classData.propertiesOfHeirsOffset);
        bool isInPart = properties && check.tryGetRecord<PropertiesOfHeirsData>(TablePartID::Layouts, classData.propertiesOfHeirsOffset, static_cast<uint64_t>(properties->numberOfNames) * sizeof(uint32_t));
        if (check.expect(isInPart, "OffsetOutsideItsPart"_s, id)) {
            bool namesFieldsInOrder = properties->builtinClass() == Typed::builtinClassOfInstances(written).index() && properties->isBelowMixin() == !!written.mixedBuiltinBase && properties->numberOfNames == written.fields.size();
            for (size_t field = 0; namesFieldsInOrder && field < written.fields.size(); ++field)
                namesFieldsInOrder = check.numbers.identifiers().numberOf(written.fields[field].key) == std::optional<uint32_t>(properties->names()[field] & ~PropertiesOfHeirsData::mayBeStoredLate) && Typed::isDecidedByClassStructure(written.fields[field]) == !(properties->names()[field] & PropertiesOfHeirsData::mayBeStoredLate);
            check.expect(namesFieldsInOrder, "PropertiesOfHeirsDifferFromProgram"_s, id);
        }
    }
    for (unsigned side = 0; side < numberOfClassSides; ++side) {
        uint32_t bit = classData.guardBit[side];
        check.expect(bit == check.guardBitInStatics(id, side), "GuardBitDiffersFromStatics"_s, id);
        check.expect(bit == noClassGuardBit || byteOffsetOfClassGuardBit(bit) < check.tables.instanceSizes.sizeOfStatics, "TableOutOfBounds"_s, id);
        uint32_t offsetOfNamedGuards = classData.namedGuardsOffset[side];
        check.expect((bit == noClassGuardBit && !offsetOfNamedGuards) || classData.hasGuardsHereOrBelow(static_cast<ClassSide>(side)), "ClassDoesNotListenForItsGuards"_s, id);
        if (!offsetOfNamedGuards)
            continue;
        std::span<const uint32_t> count = check.tryGetWords(TablePartID::Layouts, offsetOfNamedGuards, 1);
        std::span<const uint32_t> list = count.empty() ? count : check.tryGetWords(TablePartID::Layouts, offsetOfNamedGuards, 1 + 2 * static_cast<uint64_t>(count[0]));
        if (!check.expect(!list.empty(), "OffsetOutsideItsPart"_s, id))
            continue;
        bool everyNameHasItsBit = true;
        for (size_t place = 1; place < list.size(); place += 2)
            everyNameHasItsBit &= check.tables.someClassGuardNames(list[place]);
        check.expect(everyNameHasItsBit, "NameOfClassGuardWithoutItsBit"_s, id);
        bool isListedUnderEveryName = true;
        if (layout.assumedBase() != LayoutData::noBase) {
            for (size_t place = 1; place < list.size(); place += 2)
                isListedUnderEveryName &= std::ranges::binary_search(check.tables.classesBelowWithGuardsOfNames(), ClassBelowWithGuardOfNameData { list[place], id.index() * numberOfClassSides + side });
        }
        check.expect(isListedUnderEveryName, "ClassBelowNotListedUnderNameOfItsGuard"_s, id);
    }
    check.expect(!!classData.guardsHoldWhileClear == check.plan.classGuardsHoldWhileClear(id), "GuardPolarityDiffersFromPlan"_s, id);
    check.expect(!!classData.guardsOfPrototypeSideHoldWhileClear == check.plan.bitsOfClassGuardsAreClearWhileTheyHold(id, ClassSide::Prototype), "GuardPolarityDiffersFromPlan"_s, id);
    check.expect(!!classData.guardsOfNamesAwaitBeginning == guardsOfNamesAwaitBeginningOf(check.program, check.plan, id), "GuardPolarityDiffersFromPlan"_s, id);
    check.expect(std::ranges::all_of(layout.staticFields(), [&](const FieldData& field) {
        return field.slot.representation() == SlotRepresentation::JSValue && std::ranges::binary_search(check.numbers.statics().roots, static_cast<uint32_t>(field.slot.offset));
    }), "StaticFieldWithoutBoxedVariable"_s, id);

    if (classData.environment != LayoutData::noBase)
        return;
    for (std::span<const MethodData> methods : { layout.methods(), layout.staticMethods() }) {
        for (const MethodData& method : methods)
            check.expect(!(method.flags & methodTakesEnvironment) || (method.flags & methodIsCreatedAtDefinition), "ClassCapturesMismatch"_s, id);
    }
}

static void checkObjectRecord(const JoinedTables& check, uint32_t offset, LayoutID id, uint32_t shape)
{
    const LayoutData* layout = check.tryGetRecord<LayoutData>(TablePartID::Layouts, offset);
    bool isInPart = layout && check.liesInPart(TablePartID::Layouts, offset - layout->prefixSize(), layout->prefixSize(), alignof(LayoutData)) && check.tryGetRecord<LayoutData>(TablePartID::Layouts, offset, layout->trailingSize())
        && hasReferenceTableInPart(check, offset, *layout, layout->hasExtension()) && hasKeyOrdersInPart(check, *layout);
    if (!check.expect(isInPart, "OffsetOutsideItsPart"_s, id))
        return;
    check.expect(layout->shape == shape && layout->index == id.index(), "ShapeWithoutLayout"_s, id);
    const uint16_t knownFlags = check.tables.shiftOfTypeNumbers == ProgramTables::shiftOfNearTypeNumbers ? allFieldFlags : allFieldFlags | fieldTypeIsLeftToRunTime;
    auto hasUnknownFlag = [&](const FieldData& field) {
        return !!(field.flags & ~knownFlags);
    };
    check.expect(std::ranges::none_of(layout->fields(), hasUnknownFlag) && std::ranges::none_of(layout->staticFields(), hasUnknownFlag), "UnknownFieldFlag"_s, id);
    check.expect(std::ranges::all_of(layout->fields(), [&](const FieldData& field) {
        return !field.typeNumber || check.tryGetRecord<RuntimeType>(TablePartID::Types, check.tables.offsetOfTypeOf(field));
    }), "OffsetOutsideItsPart"_s, id);
    checkRecordAgainstPlan(check, *layout, id);
    if (layout->isClass())
        checkClassRecord(check, *layout, id);
}

static void checkLayoutRecords(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const Typed::ShapeTable& shapes = check.plan.shapes();
    uint32_t numberOfLayouts = check.program.layouts.size();
    std::span<const uint32_t> offsets = tables.layouts();
    if (!check.expect(offsets.size() == static_cast<size_t>(shapes.numberOfShapes()) + 1, "TableOutOfBounds"_s, { }, "layouts"_s))
        return;

    for (uint32_t shape = 0; shape < offsets.size(); ++shape) {
        uint32_t offset = offsets[shape];
        LayoutID id = shapes.isAssigned(shape) ? shapes.shape(shape).layout : LayoutID();
        if (id && !check.plan.isAllocatable(id))
            id = { };
        if (!check.expect(!offset == !id, "ShapeWithoutLayout"_s, id) || !id)
            continue;
        if (shape < tables.firstEnvironmentShape) {
            checkObjectRecord(check, offset, id, shape);
            continue;
        }
        const EnvironmentData* environment = check.tryGetRecord<EnvironmentData>(TablePartID::Layouts, offset);
        bool isInPart = environment && check.tryGetRecord<EnvironmentData>(TablePartID::Layouts, offset, static_cast<uint64_t>(environment->numberOfDescribedVariables) * sizeof(VariableData)) && hasReferenceTableInPart(check, offset, *environment, false);
        if (check.expect(isInPart, "OffsetOutsideItsPart"_s, id))
            check.expect(environment->shape == shape && environment->index == id.index() && environment->moduleAfterNone <= tables.modules().size(), "ShapeWithoutLayout"_s, id);
    }

    if (!check.expect(holdsGroupsAndWordsOfSetBits(tables.classes(), tables.numberOfClassGroupWords()), "TableOutOfBounds"_s, { }, "classes"_s))
        return;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        uint32_t offset = tables.offsetOfClass(index);
        std::optional<uint32_t> rank = check.numbers.rankOfClass(id);
        if (!check.expect(!!offset == rank.has_value() && (!rank || *rank == tables.rankOfClass(index)), "ClassRankDiffersFromNumbers"_s, id))
            continue;
        if (!offset) {
            for (unsigned side = 0; side < numberOfClassSides; ++side)
                check.expect(!check.plan.bitsOfClassGuardsAreClearWhileTheyHold(id, static_cast<ClassSide>(side)) || check.guardBitInStatics(id, side) == noClassGuardBit, "GuardWithoutClassRecord"_s, id);
            continue;
        }
        uint32_t generalShape = shapes.generalShapeOf(id);
        if (!generalShape || offsets[generalShape] != offset)
            checkObjectRecord(check, offset, id, generalShape);
    }

    uint64_t subclassStructuresEnd = 0;
    const Vector<uint32_t>& subclassStructuresOffsets = check.numbers.statics().subclassStructuresOffsets;
    for (uint32_t index = 0; index < subclassStructuresOffsets.size(); ++index) {
        uint32_t offset = subclassStructuresOffsets[index];
        if (offset == PlacedStatics::noOffset)
            continue;
        LayoutID heir(index);
        if (!check.expect(index < numberOfLayouts && tables.offsetOfClass(index) && Typed::isKnownByStructureOfClass(check.program.at(heir)), "HeirLookedAtWithoutRecord"_s, heir))
            continue;
        check.expect(offset >= subclassStructuresEnd && !(offset % alignof(StructuresOfHeir)), "StructuresOfHeirsOverlap"_s, heir);
        subclassStructuresEnd = offset + StructuresOfHeir::sizeWithDisplacementsOfFields(check.program.at(heir).fields.size());
    }
    check.expect(subclassStructuresEnd <= tables.instanceSizes.sizeOfStatics, "SubclassStructuresBeyondStatics"_s);
}

static void checkFieldTable(const JoinedTables& check)
{
    const ProgramTables& tables = check.tables;
    const uint32_t highestObjectShape = tables.firstEnvironmentShape - 1;
    const uint16_t numberOfAnyType = static_cast<uint16_t>(check.offsetOfType(Typed::TypeTable::anything) >> tables.shiftOfTypeNumbers);
    const uint32_t numberOfColumns = check.numbers.identifiers().countWithFieldTableColumn();
    bool hasRowsAndColumns = tables.firstEnvironmentShape && tables.numberOfFieldTableColumns == numberOfColumns && numberOfColumns <= FieldTableEntry::maxName + 1 && tables.numberOfFieldTableRows == highestObjectShape
        && highestObjectShape <= tables.fieldTable().size() && tables.fieldRecordIndices().size() == tables.fieldTable().size() - highestObjectShape;
    if (!check.expect(hasRowsAndColumns, "TableOutOfBounds"_s, { }, "fieldTable"_s))
        return;
    std::span<const uint32_t> entries = tables.fieldTableEntries();
    auto isRowInBounds = [&](uint32_t row) {
        return row <= entries.size() && entries.size() - row >= numberOfColumns;
    };
    if (!check.expect(std::ranges::all_of(tables.fieldTable().first(highestObjectShape), isRowInBounds), "TableOutOfBounds"_s, { }, "fieldTable"_s))
        return;

    Vector<uint32_t> numberOfEntriesOfRow(FillWith { }, entries.size(), 0);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        FieldTableEntry entry { entries[index] };
        if (!entry.isEmpty() && check.expect(entry.name() <= index, "FieldTableEntryWithoutField"_s))
            ++numberOfEntriesOfRow[index - entry.name()];
    }

    BitVector rowsWithReader(entries.size());
    using WordsOfFieldRecord = std::array<uint32_t, sizeof(FieldData) / sizeof(uint32_t)>;
    Vector<const FieldData*> recordsThatStoresGoBy(FillWith { }, entries.size(), nullptr);
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t row = tables.fieldTableRowOf(shape);
        rowsWithReader.quickSet(row);
        uint32_t numberOfFieldsFound = 0;
        LayoutID id = check.plan.shapes().shape(shape).layout;
        if (tables.hasLayoutForShape(shape)) {
            const LayoutData& layout = tables.layoutForShape(shape);
            for (const FieldData& field : layout.fields()) {
                if (!tables.hasFieldTableColumn(field.name))
                    continue;
                ++numberOfFieldsFound;
                uint32_t index = row + field.name;
                bool isInTable = findFieldInTable(tables, shape, field.name) == layout.indexOf(field) && FieldTableEntry { entries[index] } == FieldTableEntry::forField(layout, field)
                    && tables.fieldRecordIndices()[index] == FieldRecordIndex::create(field, layout.indexOf(field), tables.typeAt(tables.offsetOfTypeOf(field)), numberOfAnyType).bits;
                check.expect(isInTable, "FieldMissingFromFieldTable"_s, id);
                if (tables.fieldRecordIndices()[index] & FieldRecordIndex::isStoredWithoutTypeTest)
                    continue;
                const FieldData*& recordOfFirstReader = recordsThatStoresGoBy[index];
                if (!recordOfFirstReader)
                    recordOfFirstReader = &field;
                check.expect(std::bit_cast<WordsOfFieldRecord>(*recordOfFirstReader) == std::bit_cast<WordsOfFieldRecord>(field), "LayoutsWithDifferentRecordsReadOneFieldTableEntry"_s, id);
            }
        }
        check.expect(numberOfEntriesOfRow[row] == numberOfFieldsFound, "FieldTableEntryWithoutField"_s, id);
    }

    std::span<const uint32_t> rowsOfClasses = tables.classMemberRows();
    std::span<const uint64_t> accessors = tables.classAccessors();
    if (!check.expect(rowsOfClasses.size() == tables.numberOfClasses(), "TableOutOfBounds"_s, { }, "classMemberRows"_s))
        return;
    std::span<const uint32_t> offsetsOfClasses = tables.classes().subspan(tables.numberOfClassGroupWords());
    for (uint32_t rank = 0; rank < rowsOfClasses.size(); ++rank) {
        LayoutID id(tables.at<LayoutData>(offsetsOfClasses[rank])->index);
        std::optional<Vector<DeclaredClassMember>> members = collectDeclaredClassMembers(check.encoder, id);
        uint32_t row = rowsOfClasses[rank];
        if (!check.expect(members.has_value() == (row != ClassRowEntry::noRow), "MemberMissingFromClassMemberTable"_s, id) || !members)
            continue;
        if (!check.expect(isRowInBounds(row), "TableOutOfBounds"_s, id))
            continue;
        rowsWithReader.quickSet(row);
        for (const DeclaredClassMember& member : *members) {
            if (!check.expect(tables.hasFieldTableColumn(member.name), "MemberMissingFromClassMemberTable"_s, id))
                continue;
            uint32_t index = row + member.name;
            FieldTableEntry entry { entries[index] };
            uint32_t number = ClassRowEntry::numberIn(entry, tables.fieldRecordIndices()[index]);
            bool isRight = entry.name() == member.name && (member.method
                ? ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Method && number == member.method
                : ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Accessor && number < accessors.size() && std::bit_cast<ClassAccessorData>(accessors[number]) == ClassAccessorData { member.getter, member.setter });
            check.expect(isRight, "MemberMissingFromClassMemberTable"_s, id);
        }
        check.expect(numberOfEntriesOfRow[row] == members->size(), "ClassMemberEntryWithoutMember"_s, id);
    }
    for (uint32_t row = 0; row < entries.size(); ++row)
        check.expect(!numberOfEntriesOfRow[row] || rowsWithReader.quickGet(row), "FieldTableEntryWithoutField"_s);
}

static bool shapeSetLiesInTable(std::span<const uint64_t> table, const ShapeTestData& test)
{
    uint64_t firstWord = PagedBitSet::firstWordAt(test.bitSetOffset);
    if (firstWord >= table.size())
        return false;
    std::span<const uint64_t> words = table.subspan(firstWord);
    PagedBitSet::Entries entries = PagedBitSet::entriesAt(test.bitSetOffset);
    if (entries == PagedBitSet::Entries::Hashed) {
        unsigned shift = PagedBitSet::shiftIn(words[0]);
        return shift && shift <= 31 && PagedBitSet::numberOfWordsOfHashedSet(shift) <= words.size();
    }
    uint64_t numberOfPages = Typed::numberOfPagesInShapeSet(test.count);
    if (PagedBitSet::numberOfEntryWords(numberOfPages, entries) > words.size())
        return false;
    for (uint64_t page = 0; page < numberOfPages; ++page) {
        if (PagedBitSet::wordOfPage(words.data(), entries, page) >= words.size())
            return false;
    }
    return true;
}

static bool passesShapesOfLayouts(const JoinedTables& check, const ShapeTestData& test, std::span<const LayoutID> layouts)
{
    const Typed::ShapeTable& shapes = check.plan.shapes();
    if (test.count > static_cast<uint64_t>(test.last) + 1 || (test.count && test.last > shapes.numberOfShapes()))
        return false;
    if (test.bitSetOffset != noBitSet && !shapeSetLiesInTable(check.tables.shapeSetBits(), test))
        return false;
    Typed::ShapeTable::ShapeList members;
    shapes.appendMemberShapes(layouts, members);
    size_t numberOfShapesThatPass = 0;
    for (uint32_t shape = test.first(), remaining = test.count; remaining; ++shape, --remaining)
        numberOfShapesThatPass += shapes.isAssigned(shape) && check.tables.passes(test, shape);
    return numberOfShapesThatPass == members.size() && std::ranges::all_of(members, [&](uint32_t shape) {
        return check.tables.passes(test, shape);
    });
}

static ExpectedTypeRecord computeExpectedTypeRecord(const JoinedTables& check, Typed::TypeArena& arena, TypeID type)
{
    Typed::TypeTestPlan plan = Typed::computeTypeTestPlanFromAnything(arena, arena.at(arena.testablePartOf(type)));
    switch (plan.verdict) {
    case Typed::TypeTestVerdict::AlwaysPasses:
        return { .primitives = arena.at(Typed::TypeTable::anything).primitives, .cellTests = { Typed::CellTest::isObject() } };
    case Typed::TypeTestVerdict::NeverPasses:
        return { };
    case Typed::TypeTestVerdict::NeedsTest:
        plan.cellTests.removeAllMatching([&](const Typed::CellTest& test) {
            if (test.kind == CellTestKind::ShapeIn) {
                Typed::ShapeTable::ShapeList shapes;
                check.plan.shapes().appendMemberShapes(Typed::membersOf(arena, test.layouts), shapes);
                return shapes.isEmpty();
            }
            return test.kind == CellTestKind::IsClosureOf && !check.encoder.findEntrypoint(test.function);
        });
        removeRepeatedElements(plan.cellTests, [](const Typed::CellTest& a, const Typed::CellTest& b) {
            return a.kind == CellTestKind::CellTypeIs && b.kind == CellTestKind::CellTypeIs && a.cellTypes.first == b.cellTypes.first && a.cellTypes.last == b.cellTypes.last;
        });
        uint16_t primitives = plan.stringLiterals.isEmpty() ? plan.passingPrimitives : (plan.passingPrimitives & ~Typed::Primitive::String) | Typed::Primitive::LiteralString;
        return { .primitives = primitives, .integers = plan.integers, .cellTests = WTF::move(plan.cellTests), .literalStrings = WTF::move(plan.stringLiterals) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOfCellTest(const JoinedTables& check, const Typed::TypeArena& arena, const RuntimeCellTest& encoded, const Typed::CellTest& test)
{
    if (encoded.kind != test.kind)
        return false;
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        return true;
    case CellTestKind::CellTypeIs:
        return encoded.cellTypes().first == test.cellTypes.first && encoded.cellTypes().last == test.cellTypes.last;
    case CellTestKind::ShapeIn: {
        const ShapeTestData* shapeTest = check.tryGetRecord<ShapeTestData>(TablePartID::Types, encoded.shapeTestOffset());
        return shapeTest && passesShapesOfLayouts(check, *shapeTest, Typed::membersOf(arena, test.layouts));
    }
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        return encoded.builtinClass() == test.builtinClass;
    case CellTestKind::IsBuiltinCell: {
        const uint32_t key = BuiltinCellKey::object(test.builtin).encoded();
        return check.numbers.numberOf(NumberSpace::BuiltinCell, singleElementSpan(key)) == encoded.builtinCellNumber();
    }
    case CellTestKind::IsClosureOf:
        return encoded.closureNumber() == check.numbers.functions().closureNumberOf(*check.encoder.findEntrypoint(test.function));
    case CellTestKind::IsSymbol:
        return check.numbers.identifiers().numberOf(test.symbol) == encoded.symbolKeyNumber();
    case CellTestKind::SharingModeIs:
        return encoded.operand == static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return encoded.module() == test.module;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOfTypeRecord(const JoinedTables& check, const Typed::TypeArena& arena, const RuntimeType& encoded, const ExpectedTypeRecord& expected)
{
    auto integers = encoded.integerRange();
    if (encoded.primitives != expected.primitives || integers.has_value() != expected.integers.has_value())
        return false;
    if (integers && (integers->first != expected.integers->minimum || integers->second != expected.integers->maximum))
        return false;
    if (!std::ranges::is_permutation(encoded.literalStrings(), expected.literalStrings, { }, { }, [&](StringID string) { return check.encoder.stringNumber(string); }))
        return false;

    std::span<const RuntimeCellTest> tests = encoded.cellTests();
    size_t numberOfTestsOfMoreThanOneFunction = std::ranges::count_if(expected.cellTests, [](const Typed::CellTest& test) { return test.kind != CellTestKind::IsClosureOf; });
    if (tests.size() != expected.cellTests.size() || encoded.numberOfTestsOfMoreThanOneFunctionIfFew != std::min<size_t>(RuntimeType::hasManyTestsOfMoreThanOneFunction, numberOfTestsOfMoreThanOneFunction))
        return false;
    auto testsOneFunction = [](const RuntimeCellTest& test) { return test.kind == CellTestKind::IsClosureOf; };
    std::span<const RuntimeCellTest> testsOfOneFunctionEach = tests.subspan(numberOfTestsOfMoreThanOneFunction);
    if (std::ranges::any_of(tests.first(numberOfTestsOfMoreThanOneFunction), testsOneFunction) || !std::ranges::all_of(testsOfOneFunctionEach, testsOneFunction) || !ascendsStrictlyInTable(testsOfOneFunctionEach, &RuntimeCellTest::operand))
        return false;
    return std::ranges::all_of(expected.cellTests, [&](const Typed::CellTest& test) {
        if (test.kind == CellTestKind::IsClosureOf)
            return encoded.listsClosureNumber(check.numbers.functions().closureNumberOf(*check.encoder.findEntrypoint(test.function)));
        return std::ranges::any_of(tests.first(numberOfTestsOfMoreThanOneFunction), [&](const RuntimeCellTest& cellTest) {
            return isEncodingOfCellTest(check, arena, cellTest, test);
        });
    });
}

static TypeRecordFault findFaultOfTypeRecord(const JoinedTables& check, Typed::TypeArena& arena, TypeID type, uint32_t place, bool numbersHaveOffsets)
{
    uint32_t offset = check.tables.offsetOfType(place);
    const RuntimeType* encoded = check.tryGetRecord<RuntimeType>(TablePartID::Types, offset);
    if (!encoded || !check.liesInPart(TablePartID::Types, offset, RuntimeType::sizeInBytes(encoded->numberOfCellTests, encoded->numberOfLiteralStrings, encoded->flags), alignof(RuntimeType)))
        return TypeRecordFault::LiesOutsidePart;
    if (std::optional<uint32_t> contents = encoded->contents(); contents && !check.tryGetRecord<RuntimeType>(TablePartID::Types, *contents))
        return TypeRecordFault::LiesOutsidePart;
    if (std::optional<uint32_t> layoutSet = encoded->layoutSet(); layoutSet && *layoutSet >= check.tables.layoutSets().size())
        return TypeRecordFault::LiesOutsidePart;
    const uint32_t key = type.bits();
    if (numbersHaveOffsets && check.numbers.numberOf(NumberSpace::Type, singleElementSpan(key)) != offset)
        return TypeRecordFault::DiffersFromNumbers;
    return isEncodingOfTypeRecord(check, arena, *encoded, computeExpectedTypeRecord(check, arena, type)) ? TypeRecordFault::None : TypeRecordFault::DiffersFromPlan;
}

static void checkRuntimeTypes(const JoinedTables& check)
{
    std::span<const TypeID> types = check.numbers.typesNeededAtRunTime();
    if (types.empty())
        return;
    const uint32_t firstKey = types[0].bits();
    bool numbersHaveOffsets = check.expect(check.numbers.numberOf(NumberSpace::Type, singleElementSpan(firstKey)).has_value(), "NumbersLackOffsetsOfTypes"_s);

    RELEASE_ASSERT(check.program.types.isFrozen());
    const ParallelLoops& loops = check.encoder.build.parallelLoops();
    Vector<std::unique_ptr<Typed::TypeArena>> arenas;
    arenas.reserveInitialCapacity(loops.width());
    for (unsigned worker = 0; worker < loops.width(); ++worker)
        arenas.append(makeUniqueWithoutFastMallocCheck<Typed::TypeArena>(const_cast<Typed::TypeTable&>(check.program.types), Typed::ArenaKeeps::UntilReset));
    Vector<TypeRecordFault> faults(FillWith { }, types.size(), TypeRecordFault::None);
    forEachIndexInParallel(loops, static_cast<unsigned>(types.size()), [&](unsigned place, unsigned worker) {
        faults[place] = findFaultOfTypeRecord(check, *arenas[worker], types[place], place, numbersHaveOffsets);
    });

    for (uint32_t place = 0; place < faults.size(); ++place) {
        switch (faults[place]) {
        case TypeRecordFault::None:
            break;
        case TypeRecordFault::LiesOutsidePart:
            check.encoder.fail("OffsetOutsideItsPart"_s, { }, makeString("the record of type "_s, place));
            break;
        case TypeRecordFault::DiffersFromNumbers:
            check.encoder.fail("RowsDifferFromNumbers"_s, { }, makeString("the record of type "_s, place));
            break;
        case TypeRecordFault::DiffersFromPlan:
            check.encoder.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RuntimeTypeStricterThanPlan, .count = place });
            break;
        }
    }
}

static void checkObfuscationOfProgramTables(const JoinedTables& check)
{
    if (!check.encoder.obfuscates())
        return;
    const ProgramTables& tables = check.tables;
    uint32_t emptyString = check.encoder.stringNumber(StringID());
    std::span<const ModuleData> modules = tables.modules();
    bool hasFunctionName = check.encoder.build.options().hidesFunctionNames() && std::ranges::any_of(tables.functionNames(), [&](uint32_t name) {
        return name != emptyString;
    });
    bool hasDisplayPath = false;
    for (size_t index = 0; index < std::min<size_t>(modules.size(), check.program.modules.size()); ++index)
        hasDisplayPath |= modules[index].charactersInFrontOfDisplayPath != ModuleData::hasNoDisplayPath && !check.program.modules[index].runtimeModule;
    bool hasTypeText = std::ranges::any_of(tables.typeSites(), [](const TypeSiteData& site) {
        return site.text != TypeSiteData::noText;
    });
    bool hasVariableName = !tables.namedVariables().empty() || !tables.namedCaptures().empty();
    check.expect(!hasFunctionName && !hasDisplayPath && !hasTypeText && !hasVariableName, "NameLeftInObfuscatedBuild"_s);
}

static void checkPromisedColumns(const JoinedTables& check)
{
    const IdentifierNumbering& identifiers = check.numbers.identifiers();
    for (size_t index : check.plan.slots().keysWithColumn()) {
        std::optional<uint32_t> number = identifiers.numberOf(PropertyKeyID(index));
        if (!check.expect(!number || RowDisplacementTable::hasColumn(*number), "KeyWithoutPromisedColumn"_s))
            return;
    }
}

static std::optional<BitVector> findNodeHeadersOfDataTable(std::span<const uint64_t> words)
{
    BitVector headers(words.size());
    for (size_t position = DataNodeHeader::firstNode; position < words.size();) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        size_t remainingWords = words.size() - position;
        if (header.kindBits > static_cast<uint32_t>(DataNodeHeader::Kind::OrdinaryObject) || header.count >= remainingWords || DataNodeHeader::numberOfWords(header.kind(), header.count) > remainingWords)
            return std::nullopt;
        headers.quickSet(position);
        position += DataNodeHeader::numberOfWords(header.kind(), header.count);
    }
    return headers;
}

static ASCIILiteral findBrokenRuleOfDataNode(const ProgramTables& tables, DataNodeHeader header)
{
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        return { };
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
        return header.operand < tables.allocations().size() ? ASCIILiteral() : "TableOutOfBounds"_s;
    case DataNodeHeader::Kind::ObjectBody:
        if (header.operand >= tables.allocations().size())
            return "TableOutOfBounds"_s;
        if (!tables.hasLayoutForShape(tables.allocations()[header.operand].shape))
            return "ShapeWithoutLayout"_s;
        return tables.allocations()[header.operand].size() == offsetOfFirstField + header.count * sizeof(uint64_t) ? ASCIILiteral() : "DataNotEncoded"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool dataNodeHoldsDataWords(DataNodeHeader header)
{
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
        return (header.operand & DataNodeHeader::indexingTypeMask) != ArrayWithDouble;
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        return true;
    case DataNodeHeader::Kind::ObjectBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool forEachDataWordInObjectBody(const ProgramTables& tables, std::span<const uint64_t> words, size_t position, const Invocable<void(uint64_t)> auto& functor)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    const LayoutData& layout = tables.layoutForShape(tables.allocations()[header.operand].shape);
    std::span<const uint32_t> references = inlineReferenceTableOf(layout);
    if (references.empty())
        return true;
    std::span<const uint8_t> body = asBytes(words.subspan(position + 1, header.count));
    auto tryGetSlot = [&](uint32_t offset, size_t width) -> std::span<const uint8_t> {
        if (offset % width || offset < offsetOfFirstField || offset - offsetOfFirstField + width > body.size())
            return { };
        return body.subspan(offset - offsetOfFirstField, width);
    };
    size_t numberOfWideSlots = references[0] + references[1];
    std::span<const uint32_t> offsets = references.subspan(numberOfInlineReferenceCounts);
    if (numberOfWideSlots > offsets.size())
        return false;
    for (uint32_t offset : offsets.first(numberOfWideSlots)) {
        std::span<const uint8_t> slot = tryGetSlot(offset, sizeof(uint64_t));
        if (slot.empty())
            return false;
        functor(reinterpretCastSpanStartTo<uint64_t>(slot));
    }
    for (uint32_t offset : offsets.subspan(numberOfWideSlots)) {
        std::span<const uint8_t> slot = tryGetSlot(offset, sizeof(uint32_t));
        if (slot.empty())
            return false;
        if (uint32_t word = reinterpretCastSpanStartTo<uint32_t>(slot); !NarrowDataWord::isFinal(word))
            functor(NarrowDataWord::widen(word));
    }
    return true;
}

static std::span<const uint64_t> valuesOfDataNode(std::span<const uint64_t> words, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    return words.subspan(position + (header.kind() == DataNodeHeader::Kind::Array ? DataNodeHeader::wordsBeforeElements : 1), header.count);
}

static bool dataNodeIsStorageAsEngineHasIt(std::span<const uint64_t> words, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    if (header.kind() != DataNodeHeader::Kind::Array)
        return true;
    if (words[position + 1] != (header.count | static_cast<uint64_t>(header.count) << 32))
        return false;
    if (!(header.operand & DataNodeHeader::isStorage) || !dataNodeHoldsDataWords(header))
        return true;
    return std::ranges::all_of(valuesOfDataNode(words, position), [](uint64_t word) {
        return DataWord::kindOf(word) == DataWord::Kind::Immediate;
    });
}

static bool dataWordNamesOtherMembers(std::span<const uint64_t> words, const BitVector& headers, uint64_t word)
{
    if (DataWord::kindOf(word) != DataWord::Kind::Node || !headers.get(DataWord::operandOf(word)))
        return false;
    return DataNodeHeader::decode(words[DataWord::operandOf(word)]).kind() == DataNodeHeader::Kind::OtherMembers;
}

static ASCIILiteral findBrokenRuleOfOtherMembers(const JoinedTables& check, std::span<const uint64_t> words, const BitVector& headers, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    std::span<const uint64_t> body = words.subspan(position + 1, DataNodeHeader::numberOfWords(header.kind(), header.count) - 1);
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectBody:
        break;
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
        if (!dataWordNamesOtherMembers(words, headers, body.back()))
            return "DataNotEncoded"_s;
        break;
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        for (uint32_t number : spanReinterpretCast<const uint32_t>(body.subspan(header.count)).first(header.count)) {
            if (!number || number >= check.numbers.identifiers().count())
                return "NameWithoutIdentifier"_s;
        }
        break;
    }
    for (uint64_t word : valuesOfDataNode(words, position).first(dataNodeHoldsDataWords(header) ? header.count : 0)) {
        if (dataWordNamesOtherMembers(words, headers, word))
            return "DataNotEncoded"_s;
    }
    return { };
}

static ASCIILiteral findBrokenRuleOfDataWord(const JoinedTables& check, const BitVector& headers, uint64_t word)
{
    uint32_t operand = DataWord::operandOf(word);
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Immediate:
        return { };
    case DataWord::Kind::StringConstant:
        return operand < check.numbers.stringConstants().count() ? ASCIILiteral() : "TableOutOfBounds"_s;
    case DataWord::Kind::BigIntConstant:
        return operand < check.tables.bigIntConstants().size() ? ASCIILiteral() : "TableOutOfBounds"_s;
    case DataWord::Kind::Node:
        return headers.get(operand) ? ASCIILiteral() : "DataNotEncoded"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral findBrokenRuleOfDataTable(const JoinedTables& check)
{
    std::span<const uint64_t> words = check.tables.data();
    std::optional<BitVector> headers = findNodeHeadersOfDataTable(words);
    if (!headers)
        return "TableOutOfBounds"_s;
    for (size_t position : *headers) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        if (ASCIILiteral rule = findBrokenRuleOfDataNode(check.tables, header); !rule.isNull())
            return rule;
        if (ASCIILiteral rule = findBrokenRuleOfOtherMembers(check, words, *headers, position); !rule.isNull())
            return rule;
        if (!dataNodeIsStorageAsEngineHasIt(words, position))
            return "DataNotEncoded"_s;
        if (header.kind() == DataNodeHeader::Kind::ObjectBody) {
            ASCIILiteral broken;
            bool everyReferenceIsInBody = forEachDataWordInObjectBody(check.tables, words, position, [&](uint64_t word) {
                if (broken.isNull())
                    broken = dataWordNamesOtherMembers(words, *headers, word) ? "DataNotEncoded"_s : findBrokenRuleOfDataWord(check, *headers, word);
            });
            if (!everyReferenceIsInBody)
                return "TableOutOfBounds"_s;
            if (!broken.isNull())
                return broken;
        }
        if (!dataNodeHoldsDataWords(header))
            continue;
        for (uint64_t word : valuesOfDataNode(words, position)) {
            if (ASCIILiteral rule = findBrokenRuleOfDataWord(check, *headers, word); !rule.isNull())
                return rule;
        }
    }
    return { };
}

void checkProgramTables(const TableInputs& inputs, const ImageNumbers& numbers, std::span<const uint8_t> bytes, BuildContext& build)
{
    if (!runsCheckOfProgramTables())
        return;
    TableEncoder encoder(inputs, numbers, nullptr, TablePartID::Header, build);
    if (!checkRangesOfProgramTables(encoder, bytes))
        return;
    JoinedTables check { encoder, inputs.program, inputs.plan, numbers, reinterpretCastSpanStartTo<ProgramTables>(bytes), bytes, { }, { } };
    if (!findBoundsOfTableParts(check) || !checkHeaderOfProgramTables(check) || !checkIndexOfTypeRecords(check))
        return;
    check.stringsByNumber = findStringsByNumberInTables(encoder);
    checkOrderOfProgramTables(check);
    checkModuleTables(check);
    checkRowsNamedByNumber(check);
    checkFunctionTables(check);
    if (!build.diagnostics().hasErrors())
        checkNamesForHeapSnapshots(check);
    checkListedFunctions(check);
    checkLayoutRecords(check);
    checkRuntimeTypes(check);
    checkObfuscationOfProgramTables(check);
    checkPromisedColumns(check);
    if (build.diagnostics().hasErrors())
        return;
    checkFieldTable(check);
    if (ASCIILiteral rule = findBrokenRuleOfDataTable(check); !rule.isNull())
        encoder.fail(rule, { }, "data"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
