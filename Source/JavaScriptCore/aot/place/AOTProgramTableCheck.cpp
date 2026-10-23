/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTDerivedFunctions.h"
#include "AOTClassMemberTable.h"
#include "AOTFieldTable.h"
#include "AOTObjectVisiting.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeType.h"
#include "AOTThreads.h"
#include "AOTTypeTestPlan.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <functional>
#include <string_view>
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ExpectedRuntimeType {
    TypeID id;
    uint16_t primitives { 0 };
    std::optional<Range> integers;
    Vector<CellTest, 4> cellTests;
    Vector<StringID, 4> literalStrings;
};

} // anonymous namespace

template<typename Entry, typename KeyFunctor>
static bool isStrictlyAscending(std::span<const Entry> entries, const KeyFunctor& keyOf)
{
    for (size_t i = 1; i < entries.size(); ++i) {
        if (!(std::invoke(keyOf, entries[i - 1]) < std::invoke(keyOf, entries[i])))
            return false;
    }
    return true;
}

static std::string_view viewOf(std::span<const uint8_t> bytes)
{
    auto characters = byteCast<char>(bytes);
    return { characters.data(), characters.size() };
}

template<typename Record>
static const Record* tryGetRecord(std::span<const uint8_t> bytes, uint32_t offset)
{
    if (offset % alignof(Record) || offset < sizeof(ProgramTables) || offset + sizeof(Record) > bytes.size())
        return nullptr;
    return &reinterpretCastSpanStartTo<Record>(bytes.subspan(offset));
}

static bool checkTableBounds(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramTables)) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return false;
    }
    auto& tables = reinterpretCastSpanStartTo<ProgramTables>(bytes);
    bool isInBounds = true;
    auto checkRange = [&](ProgramTableIndex index, size_t elementSize, size_t alignment) {
        if (!encoder.writtenTables.get(static_cast<unsigned>(index)))
            encoder.fail(ProgramTableRule::TableNotWritten);
        TableRange range = tables.ranges[static_cast<unsigned>(index)];
        if (!(range.offset % alignment) && range.offset + static_cast<uint64_t>(range.count) * elementSize <= bytes.size())
            return;
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        isInBounds = false;
    };
#define AOT_CHECK_PROGRAM_TABLE_RANGE(accessor, ElementType) checkRange(ProgramTableIndex::accessor, sizeof(ElementType), alignof(ElementType));
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_CHECK_PROGRAM_TABLE_RANGE)
#undef AOT_CHECK_PROGRAM_TABLE_RANGE
    return isInBounds;
}

static void checkConstantBuiltinMembers(const TableEncoder& encoder, const ProgramTables& tables)
{
    const BuiltinTable& builtins = encoder.program.builtins();
    auto members = tables.constantBuiltinMembers();
    bool everyMemberExists = std::ranges::all_of(members, [&](uint32_t member) {
        return builtins.contains(BuiltinID(member));
    });
    if (!everyMemberExists) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    bool isSorted = isStrictlyAscending(members, [&](uint32_t member) {
        BuiltinID id(member);
        return std::pair { builtins.at(id).holder, id };
    });
    if (!isSorted)
        encoder.fail(ProgramTableRule::TableNotSorted);
}

static void checkBuiltinCells(const TableEncoder& encoder, const ProgramTables& tables)
{
    if (!isStrictlyAscending(tables.builtinCells(), std::identity { }) || !isStrictlyAscending(tables.eagerBuiltinCells(), std::identity { }))
        encoder.fail(ProgramTableRule::TableNotSorted);
    if (!tables.eagerBuiltinCells().empty() && tables.eagerBuiltinCells().back() >= tables.builtinCells().size())
        encoder.fail(ProgramTableRule::TableOutOfBounds);
}

static StringID findIdentifierText(const TableEncoder& encoder, uint32_t number)
{
    return number < encoder.identifiers.count() ? encoder.identifiers.at(number).text : StringID();
}

// WTF::codePointCompare() orders by UTF-16 code unit.
static std::strong_ordering compareCodeUnits(const StringTable& strings, StringID first, StringID second)
{
    Vector<char16_t> firstBuffer;
    Vector<char16_t> secondBuffer;
    return codePointCompare(strings.viewOf(first, firstBuffer), strings.viewOf(second, secondBuffer));
}

template<typename Entry>
static void checkModuleNames(const TableEncoder& encoder, std::span<const Entry> list, uint32_t first, uint32_t end)
{
    if (first > end || end > list.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    StringID previous;
    for (const Entry& entry : list.subspan(first, end - first)) {
        StringID name = findIdentifierText(encoder, entry.name);
        if (!name) {
            encoder.fail(ProgramTableRule::NameWithoutIdentifier);
            return;
        }
        if (previous && compareCodeUnits(encoder.program.strings, previous, name) >= 0) {
            encoder.fail(ProgramTableRule::TableNotSorted);
            return;
        }
        previous = name;
    }
}

static void checkModuleTables(const TableEncoder& encoder, const ProgramTables& tables)
{
    auto& strings = encoder.program.strings;
    auto modules = tables.modules();
    size_t numberOfModules = encoder.program.modules.size();
    if (modules.size() != numberOfModules + 1) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    for (size_t index = 0; index < numberOfModules; ++index) {
        checkModuleNames(encoder, tables.moduleExports(), modules[index].firstExport, modules[index + 1].firstExport);
        checkModuleNames(encoder, tables.syntheticExports(), modules[index].firstSyntheticExport, modules[index + 1].firstSyntheticExport);
    }
    auto everyModuleExists = [&](ModuleNumbers list) {
        for (uint32_t module : list) {
            if (module >= numberOfModules)
                return false;
        }
        return true;
    };
    auto checkRequestLists = [&](std::span<const uint16_t> words, Number24 ModuleData::* first) {
        ModuleNumbers lists(words, modules.size());
        bool hasValidStarts = std::ranges::is_sorted(modules, { }, [&](const ModuleData& module) { return static_cast<uint32_t>(module.*first); }) && modules.back().*first == lists.size();
        if (!hasValidStarts || !everyModuleExists(lists))
            encoder.fail(ProgramTableRule::TableOutOfBounds);
    };
    checkRequestLists(tables.linkRequests(), &ModuleData::firstLinkRequest);
    checkRequestLists(tables.evaluationRequests(), &ModuleData::firstEvaluationRequest);
    bool everyStepNamesModule = std::ranges::all_of(tables.entryPointLinkPlan(), [&](const LinkPlanStepData& step) {
        return step.module < numberOfModules;
    });
    if (!everyStepNamesModule)
        encoder.fail(ProgramTableRule::TableOutOfBounds);
    std::span<const LinkPlanRootData> linkPlanRoots = tables.linkPlanRoots();
    bool everyPlanIsInBounds = std::ranges::all_of(tables.linkPlanStepsOfRoots(), [&](const LinkPlanStepData& step) {
        return step.module < numberOfModules;
    }) && std::ranges::all_of(linkPlanRoots, [&](const LinkPlanRootData& root) {
        return root.memberOfCycle < numberOfModules;
    }) && std::ranges::is_sorted(linkPlanRoots, { }, &LinkPlanRootData::module) && std::ranges::is_sorted(linkPlanRoots, { }, &LinkPlanRootData::firstStep)
        && (linkPlanRoots.empty() ? tables.linkPlanStepsOfRoots().empty() : linkPlanRoots.back().module == numberOfModules && linkPlanRoots.back().firstStep == tables.linkPlanStepsOfRoots().size());
    if (!everyPlanIsInBounds)
        encoder.fail(ProgramTableRule::TableOutOfBounds);

    bool listsUncompiledCode = std::ranges::all_of(tables.modulesOfUncompiledCode(), [&](uint32_t module) {
        return module < numberOfModules && modules[module].isUncompiledCode();
    }) && static_cast<size_t>(std::ranges::count_if(modules.first(numberOfModules), &ModuleData::isUncompiledCode)) == tables.modulesOfUncompiledCode().size();
    if (!listsUncompiledCode)
        encoder.fail(ProgramTableRule::TableOutOfBounds);

    // The pieces of a path make the path that the program has for the module.
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
            if (piece >= strings.size())
                return false;
            path.append(strings[StringID(piece)]);
        }
        return std::ranges::equal(path, strings[encoder.program.modules[index].path]);
    };
    ModuleNumbers modulesByPath(tables.modulesByPath(), modules.size());
    bool everyPathExists = everyModuleExists(modulesByPath);
    for (size_t index = 0; everyPathExists && index < numberOfModules; ++index)
        everyPathExists = hasPathOfProgram(index);
    bool everyDisplayPathExists = std::ranges::is_sorted(tables.moduleDisplayPaths(), { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; })
        && std::ranges::all_of(tables.moduleDisplayPaths(), [&](const ModuleDisplayPathData& entry) {
            return entry.module < numberOfModules && modules[entry.module].charactersInFrontOfDisplayPath == ModuleData::displayPathIsListed && entry.path < strings.size();
        })
        && static_cast<size_t>(std::ranges::count(modules.first(numberOfModules), ModuleData::displayPathIsListed, &ModuleData::charactersInFrontOfDisplayPath)) == tables.moduleDisplayPaths().size();
    if (!everyPathExists || !everyDisplayPathExists) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    auto pathAt = [&](size_t index) {
        return viewOf(strings[encoder.program.modules[modulesByPath[index]].path]);
    };
    for (size_t index = 1; index < modulesByPath.size(); ++index) {
        if (!(pathAt(index - 1) < pathAt(index))) {
            encoder.fail(ProgramTableRule::TableNotSorted);
            break;
        }
    }
}

static void checkSortedTables(const TableEncoder& encoder, const ProgramTables& tables)
{
    auto checkAscending = [&](auto entries, const auto& keyOf) {
        if (!isStrictlyAscending(entries, keyOf))
            encoder.fail(ProgramTableRule::TableNotSorted);
    };
    checkAscending(tables.constructors(), &ConstructorData::function);
    checkAscending(tables.boundFunctions(), &BoundFunctionData::function);
    checkAscending(tables.constructEntrypoints(), &ConstructEntrypointData::function);
    checkAscending(tables.implementedBuiltins(), &ImplementedBuiltinData::function);
    checkAscending(tables.staticsRoots(), &StaticsRange::offset);
    checkAscending(tables.staticsNarrowRoots(), &StaticsRange::offset);
    checkAscending(tables.staticsRuns(), &StaticsRun::firstWord);
    checkAscending(tables.compiledRuntimeModules(), &CompiledRuntimeModuleData::registryID);
    checkAscending(tables.compiledRuntimeFunctions(), &CompiledRuntimeFunctionData::id);
    checkBuiltinCells(encoder, tables);
    checkConstantBuiltinMembers(encoder, tables);
    checkModuleTables(encoder, tables);
}

static void checkListedFunctions(const TableEncoder& encoder, const ProgramTables& tables)
{
    const Program& program = encoder.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionID boundEntrypoint = function.role == FunctionRole::BoundFunction ? encoder.findEntrypoint(id) : FunctionID();
        if (boundEntrypoint && !std::ranges::binary_search(tables.boundFunctions(), boundEntrypoint.index(), { }, &BoundFunctionData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, { }, id);
        auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
        bool hasConstructEntrypoint = function.role == FunctionRole::GenericEntrypoint && entrypoint && entrypoint->constructEntrypoint;
        if (hasConstructEntrypoint && !std::ranges::binary_search(tables.constructEntrypoints(), id.index(), { }, &ConstructEntrypointData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, { }, id);
    }
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        FunctionID entrypoint = encoder.placement.isInUse(LayoutID(index)) ? encoder.findEntrypoint(constructor) : FunctionID();
        if (entrypoint &&!std::ranges::binary_search(tables.constructors(), entrypoint.index(), { }, &ConstructorData::function))
            encoder.fail(ProgramTableRule::FunctionNotListed, LayoutID(index), constructor);
    }
}

static void checkClassCaptures(const TableEncoder& encoder, const LayoutData& record, LayoutID id)
{
    if (record.classData().environment != LayoutData::noBase)
        return;
    auto checkMethods = [&](std::span<const MethodData> methods) {
        for (const MethodData& method : methods) {
            if ((method.flags & methodTakesEnvironment) && !(method.flags & methodIsCreatedAtDefinition))
                encoder.fail(ProgramTableRule::ClassCapturesMismatch, id, FunctionID(method.function));
        }
    };
    checkMethods(record.methods());
    checkMethods(record.staticMethods());
}

static std::span<const uint32_t> tryGetWords(std::span<const uint8_t> bytes, uint32_t offset, uint64_t count)
{
    if (offset % alignof(uint32_t) || offset < sizeof(ProgramTables) || offset + count * sizeof(uint32_t) > bytes.size())
        return { };
    return spanReinterpretCast<const uint32_t>(bytes.subspan(offset, count * sizeof(uint32_t)));
}

static std::span<const uint32_t> tryGetMembersOfGuardSet(std::span<const uint8_t> bytes, uint32_t offset)
{
    std::span<const uint32_t> count = tryGetWords(bytes, offset, 1);
    if (count.empty())
        return { };
    std::span<const uint32_t> list = tryGetWords(bytes, offset, 1 + static_cast<uint64_t>(count[0]));
    return list.empty() ? list : list.subspan(1);
}

static ProgramTableRule findBrokenGuardSetRule(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t guard, MemberGuardAggregates aggregates)
{
    if (!aggregates.count)
        return ProgramTableRule::None;
    std::span<const uint32_t> words = tryGetWords(bytes, aggregates.offset, 2 * static_cast<uint64_t>(aggregates.count));
    if (words.empty())
        return ProgramTableRule::TableOutOfBounds;
    auto isInStatics = [&](uint32_t number) {
        return number / 8 < encoder.inputs.instanceSizes.sizeOfStatics;
    };
    for (uint32_t index = 0; index < aggregates.count; ++index) {
        uint32_t set = MemberGuardAggregates::numberInListEntry(words[index]);
        std::span<const uint32_t> members = tryGetMembersOfGuardSet(bytes, words[aggregates.count + index]);
        if (members.size() < 2 || !isInStatics(set) || !std::ranges::all_of(members, isInStatics, MemberGuardAggregates::numberInListEntry))
            return ProgramTableRule::TableOutOfBounds;
        if (!std::ranges::contains(members, guard, MemberGuardAggregates::numberInListEntry) || std::ranges::contains(members, set, MemberGuardAggregates::numberInListEntry))
            return ProgramTableRule::GuardMissingFromSet;
    }
    return ProgramTableRule::None;
}

static ProgramTableRule findBrokenSubclassRule(const TableEncoder& encoder, std::span<const uint8_t> bytes, const LayoutData& layout)
{
    uint32_t offset = layout.classData().subclassesOffset;
    if (!offset)
        return ProgramTableRule::None;
    std::span<const uint32_t> count = tryGetWords(bytes, offset, 1);
    std::span<const uint32_t> list = count.empty() ? count : tryGetWords(bytes, offset, 1 + static_cast<uint64_t>(count[0]));
    if (list.size() < 2)
        return ProgramTableRule::TableOutOfBounds;
    std::span<const uint32_t> subclasses = list.subspan(1);
    bool areClasses = std::ranges::all_of(subclasses, [&](uint32_t index) {
        return index < encoder.program.layouts.size() && encoder.program.layouts[index].kind == LayoutKind::Instance;
    });
    if (!areClasses)
        return ProgramTableRule::TableOutOfBounds;
    return isStrictlyAscending(subclasses, std::identity { }) ? ProgramTableRule::None : ProgramTableRule::TableNotSorted;
}

static void checkMemberGuards(const TableEncoder& encoder, std::span<const uint8_t> bytes, const LayoutData& layout, LayoutID id)
{
    if (ProgramTableRule rule = findBrokenSubclassRule(encoder, bytes, layout); rule != ProgramTableRule::None)
        encoder.fail(rule, id);
    bool guardsAreSorted = isStrictlyAscending(layout.memberGuards(), [](const MemberGuardData& guard) {
        return std::tuple { guard.side(), guard.kind(), static_cast<uint32_t>(guard.name) };
    });
    if (!guardsAreSorted)
        encoder.fail(ProgramTableRule::TableNotSorted, id);
    for (const MemberGuardData& guard : layout.memberGuards()) {
        uint32_t number = MemberGuardAggregates::bitNumber(layout.classData().guardWordsOffset[static_cast<unsigned>(guard.side())], guard.bit);
        if (ProgramTableRule rule = findBrokenGuardSetRule(encoder, bytes, number, layout.aggregatesOf(guard)); rule != ProgramTableRule::None)
            encoder.fail(rule, id);
    }
}

static const LayoutData* tryGetLayoutData(std::span<const uint8_t> bytes, uint32_t offset)
{
    const LayoutData* layout = tryGetRecord<LayoutData>(bytes, offset);
    if (!layout || offset < sizeof(ProgramTables) + layout->prefixSize())
        return nullptr;
    return offset + sizeof(LayoutData) + layout->trailingSize() <= bytes.size() ? layout : nullptr;
}

static void checkObjectRecord(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutID id, uint32_t shape)
{
    const LayoutData* layout = tryGetLayoutData(bytes, offset);
    if (!layout) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        return;
    }
    if (layout->shape != shape || layout->index != id.index())
        encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
    auto hasUnknownFlag = [](const FieldData& field) {
        return !!(field.flags & ~allFieldFlags);
    };
    if (std::ranges::any_of(layout->fields(), hasUnknownFlag) || std::ranges::any_of(layout->staticFields(), hasUnknownFlag))
        encoder.fail(ProgramTableRule::UnknownFieldFlag, id);
    if (!layout->isClass())
        return;
    checkMemberGuards(encoder, bytes, *layout, id);
    checkClassCaptures(encoder, *layout, id);
}

static void checkEnvironmentRecord(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutID id, uint32_t shape)
{
    const EnvironmentData* environment = tryGetRecord<EnvironmentData>(bytes, offset);
    if (!environment || offset + sizeof(EnvironmentData) + static_cast<uint64_t>(environment->numberOfDescribedVariables) * sizeof(VariableData) > bytes.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        return;
    }
    if (environment->shape != shape || environment->index != id.index())
        encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
}

static bool isClassTableWellFormed(const ProgramTables& tables, size_t numberOfLayouts)
{
    auto words = tables.classes();
    size_t numberOfGroups = (numberOfLayouts + 31) / 32;
    if (tables.numberOfLayoutIndices != numberOfLayouts || words.size() < numberOfGroups * 2)
        return false;
    size_t numberOfClasses = 0;
    for (size_t group = 0; group < numberOfGroups; ++group) {
        if (words[group * 2 + 1] != numberOfClasses)
            return false;
        numberOfClasses += std::popcount(words[group * 2]);
    }
    return words.size() == numberOfGroups * 2 + numberOfClasses;
}

static void checkLayoutRecords(const TableEncoder& encoder, const ProgramTables& tables, std::span<const uint8_t> bytes)
{
    const ShapeTable& shapes = encoder.placement.shapes();
    size_t numberOfLayouts = encoder.program.layouts.size();
    auto offsets = tables.layouts();

    size_t endOfShapes = std::max<size_t>(offsets.size(), static_cast<size_t>(shapes.numberOfShapes()) + 1);
    for (uint32_t shape = 0; shape < endOfShapes; ++shape) {
        uint32_t offset = shape < offsets.size() ? offsets[shape] : 0;
        LayoutID id = shapes.isAssigned(shape) ? shapes.shape(shape).layout : LayoutID();
        if (id && !encoder.placement.isInUse(id))
            id = { };
        if (!offset != !id)
            encoder.fail(ProgramTableRule::ShapeWithoutLayout, id);
        else if (id && shape < tables.firstEnvironmentShape)
            checkObjectRecord(encoder, bytes, offset, id, shape);
        else if (id)
            checkEnvironmentRecord(encoder, bytes, offset, id, shape);
    }

    if (!isClassTableWellFormed(tables, numberOfLayouts)) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        uint32_t offset = tables.offsetOfClass(index);
        uint32_t generalShape = shapes.generalShapeOf(id);
        bool isGeneralShapeRecord = generalShape && generalShape < offsets.size() && offsets[generalShape] == offset;
        if (offset && !isGeneralShapeRecord)
            checkObjectRecord(encoder, bytes, offset, id, generalShape);
        if (offset && encoder.placement.rankOfClass(id) != tables.rankOfClass(index))
            encoder.fail(ProgramTableRule::TableOutOfBounds, id);
    }
}

static void checkFieldTable(const TableEncoder& encoder, const ProgramTables& tables)
{
    const uint32_t highestObjectShape = tables.firstEnvironmentShape - 1;
    const uint32_t typeOfAnyValue = encoder.typeOffset(TypeTable::anything);
    const auto numberOfColumns = static_cast<uint32_t>(std::min<uint64_t>(encoder.identifiers.count(), FieldTableEntry::maxName + 1));
    if (tables.numberOfFieldTableRows != highestObjectShape || highestObjectShape > tables.fieldTable().size() || tables.fieldRecordIndices().size() != tables.fieldTable().size() - highestObjectShape) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    auto entries = tables.fieldTableEntries();
    bool everyRowIsInBounds = std::ranges::all_of(tables.fieldTable().first(highestObjectShape), [&](uint32_t row) {
        return row <= entries.size() && entries.size() - row >= numberOfColumns;
    });
    if (!everyRowIsInBounds) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }

    // An entry belongs to the rows that begin its name in front of it.
    Vector<uint32_t> numberOfEntriesOfRow(FillWith { }, entries.size(), 0);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        FieldTableEntry entry { entries[index] };
        if (entry.isEmpty())
            continue;
        if (entry.name() > index) {
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField);
            continue;
        }
        ++numberOfEntriesOfRow[index - entry.name()];
    }

    BitVector rowsOfShapes(entries.size());
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t row = tables.fieldTableRowOf(shape);
        rowsOfShapes.quickSet(row);
        uint32_t numberOfFieldsFound = 0;
        LayoutID id = encoder.placement.shapes().shape(shape).layout;
        if (tables.hasLayoutForShape(shape)) {
            const LayoutData& layout = tables.layoutForShape(shape);
            for (const FieldData& field : layout.fields()) {
                if (!FieldTableEntry::hasColumn(field.name))
                    continue;
                ++numberOfFieldsFound;
                uint32_t index = row + field.name;
                if (findFieldInTable(tables, shape, field.name) != layout.indexOf(field) || FieldTableEntry { entries[index] } != FieldTableEntry::forField(layout, field)
                    || tables.fieldRecordIndices()[index] != FieldRecordIndex::create(field, layout.indexOf(field), tables.typeAt(field.type), typeOfAnyValue).bits)
                    encoder.fail(ProgramTableRule::FieldMissingFromFieldTable, id);
            }
        }
        if (numberOfEntriesOfRow[row] != numberOfFieldsFound)
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField, id);
    }

    auto rowsOfClasses = tables.classMemberRows();
    auto accessors = tables.classAccessors();
    if (rowsOfClasses.size() != tables.numberOfClasses()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    const uint32_t* offsetsOfClasses = tables.classes().data() + tables.numberOfClassGroupWords();
    for (uint32_t rank = 0; rank < rowsOfClasses.size(); ++rank) {
        std::optional<Vector<DeclaredClassMember>> members = collectDeclaredClassMembers(encoder, *tables.at<LayoutData>(offsetsOfClasses[rank]));
        uint32_t row = rowsOfClasses[rank];
        if (!members || row == ClassRowEntry::noRow) {
            if (members || row != ClassRowEntry::noRow)
                encoder.fail(ProgramTableRule::MemberMissingFromClassMemberTable);
            continue;
        }
        if (row > entries.size() || entries.size() - row < numberOfColumns) {
            encoder.fail(ProgramTableRule::TableOutOfBounds);
            continue;
        }
        // Rows that say the same share their entries, whoever reads them.
        rowsOfShapes.quickSet(row);
        for (const DeclaredClassMember& member : *members) {
            uint32_t index = row + member.name;
            FieldTableEntry entry { entries[index] };
            uint32_t number = ClassRowEntry::numberIn(entry, tables.fieldRecordIndices()[index]);
            bool isRight = entry.name() == member.name && (member.method
                ? ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Method && number == member.method
                : ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Accessor && number < accessors.size() && std::bit_cast<ClassAccessorData>(accessors[number]) == ClassAccessorData { member.getter, member.setter });
            if (!isRight)
                encoder.fail(ProgramTableRule::MemberMissingFromClassMemberTable);
        }
        if (numberOfEntriesOfRow[row] != members->size())
            encoder.fail(ProgramTableRule::ClassMemberEntryWithoutMember);
    }
    for (uint32_t row = 0; row < entries.size(); ++row) {
        if (numberOfEntriesOfRow[row] && !rowsOfShapes.quickGet(row))
            encoder.fail(ProgramTableRule::FieldTableEntryWithoutField);
    }
}

// In a plan, String says that some string passes. In a record it says that every string passes.
static uint16_t expectedPrimitivesOf(const TypeTestPlan& plan)
{
    if (plan.stringLiterals.isEmpty())
        return plan.passingPrimitives;
    return (plan.passingPrimitives & ~Primitive::String) | Primitive::LiteralString;
}

static ExpectedRuntimeType computeExpectedRuntimeType(const TableEncoder& encoder, TypeID id)
{
    const Program& program = encoder.program;
    TypeTestPlan plan = computeTypeTestPlanFromAnything(program, program.at(id).withoutClassProofs().withUntestableBuiltinsWidened());
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return { .id = id, .primitives = program.at(TypeTable::anything).primitives, .cellTests = { CellTest::isObject() } };
    case TypeTestVerdict::NeverPasses:
        return { .id = id };
    case TypeTestVerdict::NeedsTest:
        // A function without a generic entrypoint has no closures, so its test has no record.
        plan.cellTests.removeAllMatching([&](const CellTest& test) {
            return test.kind == CellTestKind::IsClosureOf && !encoder.findEntrypoint(test.function);
        });
        return { .id = id, .primitives = expectedPrimitivesOf(plan), .integers = plan.integers, .cellTests = WTF::move(plan.cellTests), .literalStrings = WTF::move(plan.stringLiterals) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const RuntimeType* tryGetRuntimeType(std::span<const uint8_t> bytes, uint32_t offset)
{
    const RuntimeType* type = tryGetRecord<RuntimeType>(bytes, offset);
    if (!type)
        return nullptr;
    return offset + RuntimeType::sizeInBytes(type->numberOfCellTests, type->numberOfLiteralStrings, type->flags) <= bytes.size() ? type : nullptr;
}

static bool isShapeTestOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, uint32_t offset, LayoutSetID layouts)
{
    const ShapeTestData* encoded = tryGetRecord<ShapeTestData>(bytes, offset);
    if (!encoded)
        return false;
    const ShapeTable& shapes = encoder.placement.shapes();
    ShapeTestData expected = shapes.dataOf(shapes.testOf(layouts));
    return encoded->last == expected.last && encoded->count == expected.count && encoded->bitSetOffset == expected.bitSetOffset;
}

static bool isEncodingOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, const RuntimeCellTest& encoded, const CellTest& test)
{
    if (encoded.kind != test.kind)
        return false;
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        return true;
    case CellTestKind::CellTypeIs:
        return encoded.cellTypes().first == test.cellTypes.first && encoded.cellTypes().last == test.cellTypes.last;
    case CellTestKind::ShapeIn:
        return isShapeTestOf(encoder, bytes, encoded.shapeTestOffset(), test.layouts);
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        return encoded.builtinClass() == test.builtinClass;
    case CellTestKind::IsBuiltinCell:
        return encoder.placement.builtinCell(test.builtin) == encoded.builtinCellNumber();
    case CellTestKind::IsClosureOf:
        return encoded.function() == encoder.findEntrypoint(test.function);
    case CellTestKind::IsSymbol:
        return encoded.symbolKeyNumber() == encoder.identifierNumber(test.symbol);
    case CellTestKind::SharingModeIs:
        return encoded.operand == static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return encoded.module() == test.module;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOf(const TableEncoder& encoder, std::span<const uint8_t> bytes, const RuntimeType& encoded, const ExpectedRuntimeType& expected)
{
    if (encoded.primitives != expected.primitives)
        return false;
    auto integers = encoded.integerRange();
    if (integers.has_value() != expected.integers.has_value())
        return false;
    if (integers && (integers->first != expected.integers->minimum || integers->second != expected.integers->maximum))
        return false;
    if (!std::ranges::equal(encoded.literalStrings(), expected.literalStrings.span(), { }, { }, &StringID::index))
        return false;
    auto cellTests = encoded.cellTests();
    if (cellTests.size() != expected.cellTests.size())
        return false;
    for (size_t index = 0; index < cellTests.size(); ++index) {
        if (!isEncodingOf(encoder, bytes, cellTests[index], expected.cellTests[index]))
            return false;
    }
    return true;
}

static void checkRuntimeTypes(const TableEncoder& encoder, const ProgramTables& tables, std::span<const uint8_t> bytes)
{
    const Program& program = encoder.program;
    std::span<const uint32_t> words = tables.types();
    uint32_t numberOfGroupWords = ProgramTables::numberOfTypeGroupWords(program.types.size());
    if (words.size() < 1 + numberOfGroupWords || words[0] != program.types.size()) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    size_t numberOfRecords = 0;
    for (uint32_t group = 0; group < numberOfGroupWords; group += 2) {
        if (words[1 + group + 1] != numberOfRecords) {
            encoder.fail(ProgramTableRule::TableOutOfBounds);
            return;
        }
        numberOfRecords += std::popcount(words[1 + group]);
    }
    if (words.size() != 1 + numberOfGroupWords + numberOfRecords) {
        encoder.fail(ProgramTableRule::TableOutOfBounds);
        return;
    }
    auto expectedTypes = collectInParallel<ExpectedRuntimeType>(encoder.build.parallelLoops(), program.types.size(), [&](unsigned index, Vector<ExpectedRuntimeType>& collected) {
        if (tables.offsetOfType(index))
            collected.append(computeExpectedRuntimeType(encoder, TypeID(index)));
    });
    for (const ExpectedRuntimeType& expected : expectedTypes) {
        const RuntimeType* encoded = tryGetRuntimeType(bytes, tables.offsetOfType(expected.id.index()));
        if (!encoded)
            encoder.fail(ProgramTableRule::TableOutOfBounds);
        else if (!isEncodingOf(encoder, bytes, *encoded, expected))
            encoder.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RuntimeTypeStricterThanPlan, .count = expected.id.index() });
    }
}

static void checkObfuscation(const TableEncoder& encoder, const ProgramTables& tables)
{
    if (!encoder.obfuscates())
        return;
    uint32_t emptyString = encoder.program.strings.emptyString().index();
    auto modules = tables.modules();
    bool hasFunctionName = std::ranges::any_of(tables.functionNames(), [&](uint32_t name) {
        return name != emptyString;
    }) || std::ranges::any_of(tables.functionNameRuns(), [&](const FunctionNameRunData& run) {
        return run.name != emptyString;
    });
    bool hasDisplayPath = std::ranges::any_of(modules.first(std::min<size_t>(modules.size(), encoder.program.modules.size())), [&](const ModuleData& module) {
        return module.charactersInFrontOfDisplayPath != ModuleData::hasNoDisplayPath;
    });
    bool hasTypeText = std::ranges::any_of(tables.typeSites(), [](const TypeSiteData& site) {
        return site.text != TypeSiteData::noText;
    });
    if (hasFunctionName || hasDisplayPath || hasTypeText)
        encoder.fail(ProgramTableRule::NameLeftInObfuscatedBuild);
}

static void checkColumns(const TableEncoder& encoder)
{
    for (size_t index : encoder.slots.keysWithColumn()) {
        std::optional<uint32_t> number = encoder.identifiers.numberOf(PropertyKeyID(index));
        if (!number || RowDisplacementTable::hasColumn(*number))
            continue;
        encoder.fail(ProgramTableRule::KeyWithoutPromisedColumn);
        return;
    }
}

static std::optional<BitVector> tryFindNodeHeaders(const ProgramTables& tables)
{
    auto words = tables.data();
    BitVector headers(words.size());
    for (size_t position = DataNodeHeader::firstNode; position < words.size();) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        size_t remainingWords = words.size() - position;
        if (header.count >= remainingWords || DataNodeHeader::numberOfWords(header.kind(), header.count) > remainingWords)
            return std::nullopt;
        headers.quickSet(position);
        position += DataNodeHeader::numberOfWords(header.kind(), header.count);
    }
    return headers;
}

static ProgramTableRule findBrokenRule(const ProgramTables& tables, DataNodeHeader header)
{
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        return ProgramTableRule::None;
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
        return header.operand < tables.allocations().size() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    case DataNodeHeader::Kind::ObjectBody:
        if (header.operand >= tables.allocations().size())
            return ProgramTableRule::TableOutOfBounds;
        // forEachDataWordInBody() finds the references of the body by the record of the layout.
        if (!tables.hasLayoutForShape(tables.allocations()[header.operand].shape))
            return ProgramTableRule::ShapeWithoutLayout;
        return tables.allocations()[header.operand].size() == offsetOfFirstField + header.count * sizeof(uint64_t) ? ProgramTableRule::None : ProgramTableRule::DataNotEncoded;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsDataWords(DataNodeHeader header)
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

// False where the record of the layout has a reference outside the body.
static bool forEachDataWordInBody(const ProgramTables& tables, std::span<const uint64_t> words, size_t position, const Invocable<void(uint64_t)> auto& functor)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    const LayoutData& layout = tables.layoutForShape(tables.allocations()[header.operand].shape);
    std::span<const uint32_t> references = inlineReferenceTableOf(layout);
    if (references.empty())
        return true;
    const uint8_t* fields = reinterpret_cast<const uint8_t*>(words.subspan(position + 1, header.count).data()) - offsetOfFirstField;
    size_t endOfBody = offsetOfFirstField + header.count * sizeof(uint64_t);
    auto isInBody = [&](uint32_t offset, size_t width) {
        return offset >= offsetOfFirstField && offset + width <= endOfBody;
    };
    size_t numberOfWideSlots = references[0] + references[1];
    std::span<const uint32_t> offsets = references.subspan(numberOfInlineReferenceCounts);
    if (numberOfWideSlots > offsets.size())
        return false;
    for (uint32_t offset : offsets.first(numberOfWideSlots)) {
        if (!isInBody(offset, sizeof(uint64_t)))
            return false;
        functor(*reinterpret_cast<const uint64_t*>(fields + offset));
    }
    for (uint32_t offset : offsets.subspan(numberOfWideSlots)) {
        if (!isInBody(offset, sizeof(uint32_t)))
            return false;
        if (uint32_t word = *reinterpret_cast<const uint32_t*>(fields + offset); !NarrowDataWord::isFinal(word))
            functor(NarrowDataWord::widen(word));
    }
    return true;
}

static std::span<const uint64_t> valuesOfNode(std::span<const uint64_t> words, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    return words.subspan(position + (header.kind() == DataNodeHeader::Kind::Array ? DataNodeHeader::wordsBeforeElements : 1), header.count);
}

// An array points at the words of such a node, and the collector does not look at them.
static bool isStorageAsEngineHasIt(std::span<const uint64_t> words, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    if (header.kind() != DataNodeHeader::Kind::Array)
        return true;
    if (words[position + 1] != (header.count | static_cast<uint64_t>(header.count) << 32))
        return false;
    if (!(header.operand & DataNodeHeader::isStorage) || !holdsDataWords(header))
        return true;
    return std::ranges::all_of(valuesOfNode(words, position), [](uint64_t word) {
        return DataWord::kindOf(word) == DataWord::Kind::Immediate;
    });
}

static bool namesNodeOfKind(std::span<const uint64_t> words, const BitVector& headers, uint64_t word, DataNodeHeader::Kind kind)
{
    if (DataWord::kindOf(word) != DataWord::Kind::Node || !headers.get(DataWord::operandOf(word)))
        return false;
    return DataNodeHeader::decode(words[DataWord::operandOf(word)]).kind() == kind;
}

static ProgramTableRule findBrokenOtherMembersRule(const TableEncoder& encoder, std::span<const uint64_t> words, const BitVector& headers, size_t position)
{
    DataNodeHeader header = DataNodeHeader::decode(words[position]);
    std::span<const uint64_t> body = words.subspan(position + 1, DataNodeHeader::numberOfWords(header.kind(), header.count) - 1);
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array:
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectBody:
        break;
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
        if (!namesNodeOfKind(words, headers, body.back(), DataNodeHeader::Kind::OtherMembers))
            return ProgramTableRule::DataNotEncoded;
        break;
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        for (uint32_t number : spanReinterpretCast<const uint32_t>(body.subspan(header.count)).first(header.count)) {
            if (!number || number >= encoder.identifiers.count())
                return ProgramTableRule::NameWithoutIdentifier;
        }
        break;
    }
    for (uint64_t word : valuesOfNode(words, position).first(holdsDataWords(header) ? header.count : 0)) {
        if (namesNodeOfKind(words, headers, word, DataNodeHeader::Kind::OtherMembers))
            return ProgramTableRule::DataNotEncoded;
    }
    return ProgramTableRule::None;
}

static ProgramTableRule findBrokenRule(const TableEncoder& encoder, const ProgramTables& tables, const BitVector& headers, uint64_t word)
{
    uint32_t operand = DataWord::operandOf(word);
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Immediate:
        return ProgramTableRule::None;
    case DataWord::Kind::StringConstant:
        return operand < encoder.inputs.stringConstants.count() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    case DataWord::Kind::BigIntConstant:
        return operand < tables.bigIntConstants().size() ? ProgramTableRule::None : ProgramTableRule::TableOutOfBounds;
    case DataWord::Kind::Node:
        return headers.get(operand) ? ProgramTableRule::None : ProgramTableRule::DataNotEncoded;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProgramTableRule findBrokenDataRule(const TableEncoder& encoder, const ProgramTables& tables)
{
    std::optional<BitVector> headers = tryFindNodeHeaders(tables);
    if (!headers)
        return ProgramTableRule::TableOutOfBounds;
    auto words = tables.data();
    for (size_t position : *headers) {
        DataNodeHeader header = DataNodeHeader::decode(words[position]);
        if (ProgramTableRule rule = findBrokenRule(tables, header); rule != ProgramTableRule::None)
            return rule;
        if (ProgramTableRule rule = findBrokenOtherMembersRule(encoder, words, *headers, position); rule != ProgramTableRule::None)
            return rule;
        if (!isStorageAsEngineHasIt(words, position))
            return ProgramTableRule::DataNotEncoded;
        if (header.kind() == DataNodeHeader::Kind::ObjectBody) {
            ProgramTableRule broken = ProgramTableRule::None;
            bool everyReferenceIsInBody = forEachDataWordInBody(tables, words, position, [&](uint64_t word) {
                if (broken != ProgramTableRule::None)
                    return;
                broken = namesNodeOfKind(words, *headers, word, DataNodeHeader::Kind::OtherMembers) ? ProgramTableRule::DataNotEncoded : findBrokenRule(encoder, tables, *headers, word);
            });
            if (!everyReferenceIsInBody)
                return ProgramTableRule::TableOutOfBounds;
            if (broken != ProgramTableRule::None)
                return broken;
        }
        if (!holdsDataWords(header))
            continue;
        for (uint64_t word : valuesOfNode(words, position)) {
            if (ProgramTableRule rule = findBrokenRule(encoder, tables, *headers, word); rule != ProgramTableRule::None)
                return rule;
        }
    }
    return ProgramTableRule::None;
}

static void checkData(const TableEncoder& encoder, const ProgramTables& tables)
{
    ProgramTableRule rule = findBrokenDataRule(encoder, tables);
    if (rule != ProgramTableRule::None)
        encoder.fail(rule);
}

void checkProgramTables(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (!checkTableBounds(encoder, bytes))
        return;
    auto& tables = reinterpretCastSpanStartTo<ProgramTables>(bytes);
    checkSortedTables(encoder, tables);
    checkListedFunctions(encoder, tables);
    checkLayoutRecords(encoder, tables, bytes);
    checkFieldTable(encoder, tables);
    checkRuntimeTypes(encoder, tables, bytes);
    checkObfuscation(encoder, tables);
    checkColumns(encoder);
    checkData(encoder, tables);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
