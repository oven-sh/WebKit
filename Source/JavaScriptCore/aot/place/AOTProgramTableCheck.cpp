/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTClassMemberTable.h"
#include "AOTDerivedFunctions.h"
#include "AOTFieldTable.h"
#include "AOTNumberLiteralStrings.h"
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

struct TableCheck {
    bool expect(bool holds, ProgramTableRule rule, LayoutID layout = { }, FunctionID function = { }) const
    {
        if (!holds)
            encoder.fail(rule, layout, function);
        return holds;
    }

    template<typename Record>
    const Record* tryGetRecord(uint32_t offset, uint64_t trailingSize = 0) const
    {
        if (offset % alignof(Record) || offset < sizeof(ProgramTables) || offset + sizeof(Record) + trailingSize > bytes.size())
            return nullptr;
        return &reinterpretCastSpanStartTo<Record>(bytes.subspan(offset));
    }

    std::span<const uint32_t> tryGetWords(uint32_t offset, uint64_t count) const
    {
        if (offset % alignof(uint32_t) || offset < sizeof(ProgramTables) || offset + count * sizeof(uint32_t) > bytes.size())
            return { };
        return spanReinterpretCast<const uint32_t>(bytes.subspan(offset, count * sizeof(uint32_t)));
    }

    std::span<const uint32_t> tryGetCountedWords(uint32_t offset) const
    {
        std::span<const uint32_t> count = tryGetWords(offset, 1);
        std::span<const uint32_t> list = count.empty() ? count : tryGetWords(offset, 1 + static_cast<uint64_t>(count[0]));
        return list.empty() ? list : list.subspan(1);
    }

    const TableEncoder& encoder;
    const Program& program;
    const ProgramTables& tables;
    std::span<const uint8_t> bytes;
};

struct ExpectedRuntimeType {
    TypeID id;
    uint16_t primitives { 0 };
    std::optional<Range> integers;
    Vector<CellTest, 4> cellTests;
    Vector<StringID, 4> literalStrings;
};

} // anonymous namespace

static bool isStrictlyAscending(const auto& entries, const auto& keyOf)
{
    return std::ranges::adjacent_find(entries, [](const auto& a, const auto& b) { return !(a < b); }, keyOf) == entries.end();
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

// Pairs of 32 bits and the number of bits set in front of them, one pair for 32 entries, and then a word for each bit that is set.
static bool isTableOfSetBits(std::span<const uint32_t> words, size_t numberOfGroupWords)
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

template<typename Entry>
static void checkModuleNames(const TableCheck& check, std::span<const Entry> list, uint32_t first, uint32_t end)
{
    if (!check.expect(first <= end && end <= list.size(), ProgramTableRule::TableOutOfBounds))
        return;
    const StringTable& strings = check.program.strings;
    StringID previous;
    for (const Entry& entry : list.subspan(first, end - first)) {
        StringID name = entry.name < check.encoder.identifiers.count() ? check.encoder.identifiers.at(entry.name).text : StringID();
        if (!check.expect(!!name, ProgramTableRule::NameWithoutIdentifier))
            return;
        Vector<char16_t> previousBuffer;
        Vector<char16_t> nameBuffer;
        if (!check.expect(!previous || codePointCompare(strings.viewOf(previous, previousBuffer), strings.viewOf(name, nameBuffer)) < 0, ProgramTableRule::TableNotSorted))
            return;
        previous = name;
    }
}

static void checkModuleTables(const TableCheck& check)
{
    auto& strings = check.program.strings;
    auto modules = check.tables.modules();
    size_t numberOfModules = check.program.modules.size();
    if (!check.expect(modules.size() == numberOfModules + 1, ProgramTableRule::TableOutOfBounds))
        return;
    for (size_t index = 0; index < numberOfModules; ++index) {
        checkModuleNames(check, check.tables.moduleExports(), modules[index].firstExport, modules[index + 1].firstExport);
        checkModuleNames(check, check.tables.syntheticExports(), modules[index].firstSyntheticExport, modules[index + 1].firstSyntheticExport);
    }
    auto everyModuleExists = [&](ModuleNumbers list) {
        for (uint32_t module : list) {
            if (module >= numberOfModules)
                return false;
        }
        return true;
    };
    auto namesModule = [&](const LinkPlanStepData& step) {
        return step.module < numberOfModules;
    };
    auto checkRequestLists = [&](std::span<const uint16_t> words, Number24 ModuleData::* first) {
        ModuleNumbers lists(words, modules.size());
        bool hasValidStarts = std::ranges::is_sorted(modules, { }, [&](const ModuleData& module) { return static_cast<uint32_t>(module.*first); }) && modules.back().*first == lists.size();
        check.expect(hasValidStarts && everyModuleExists(lists), ProgramTableRule::TableOutOfBounds);
    };
    checkRequestLists(check.tables.linkRequests(), &ModuleData::firstLinkRequest);
    checkRequestLists(check.tables.evaluationRequests(), &ModuleData::firstEvaluationRequest);
    check.expect(std::ranges::all_of(check.tables.entryPointLinkPlan(), namesModule), ProgramTableRule::TableOutOfBounds);
    std::span<const LinkPlanRootData> linkPlanRoots = check.tables.linkPlanRoots();
    bool everyPlanIsInBounds = std::ranges::all_of(check.tables.linkPlanStepsOfRoots(), namesModule) && std::ranges::all_of(linkPlanRoots, [&](const LinkPlanRootData& root) {
        return root.memberOfCycle < numberOfModules;
    }) && std::ranges::is_sorted(linkPlanRoots, { }, &LinkPlanRootData::module) && std::ranges::is_sorted(linkPlanRoots, { }, &LinkPlanRootData::firstStep)
        && (linkPlanRoots.empty() ? check.tables.linkPlanStepsOfRoots().empty() : linkPlanRoots.back().module == numberOfModules && linkPlanRoots.back().firstStep == check.tables.linkPlanStepsOfRoots().size());
    check.expect(everyPlanIsInBounds, ProgramTableRule::TableOutOfBounds);

    bool listsUncompiledCode = std::ranges::all_of(check.tables.modulesOfUncompiledCode(), [&](uint32_t module) {
        return module < numberOfModules && modules[module].isUncompiledCode();
    }) && static_cast<size_t>(std::ranges::count_if(modules.first(numberOfModules), &ModuleData::isUncompiledCode)) == check.tables.modulesOfUncompiledCode().size();
    check.expect(listsUncompiledCode, ProgramTableRule::TableOutOfBounds);

    std::span<const ModuleDirectoryData> directories = check.tables.moduleDirectories();
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
        return std::ranges::equal(path, strings[check.program.modules[index].path]);
    };
    ModuleNumbers modulesByPath(check.tables.modulesByPath(), modules.size());
    bool everyPathExists = everyModuleExists(modulesByPath);
    for (size_t index = 0; everyPathExists && index < numberOfModules; ++index)
        everyPathExists = hasPathOfProgram(index);
    bool everyDisplayPathExists = std::ranges::is_sorted(check.tables.moduleDisplayPaths(), { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; })
        && std::ranges::all_of(check.tables.moduleDisplayPaths(), [&](const ModuleDisplayPathData& entry) {
            return entry.module < numberOfModules && modules[entry.module].charactersInFrontOfDisplayPath == ModuleData::displayPathIsListed && entry.path < strings.size();
        })
        && static_cast<size_t>(std::ranges::count(modules.first(numberOfModules), ModuleData::displayPathIsListed, &ModuleData::charactersInFrontOfDisplayPath)) == check.tables.moduleDisplayPaths().size();
    if (!check.expect(everyPathExists && everyDisplayPathExists, ProgramTableRule::TableOutOfBounds))
        return;
    auto pathAt = [&](size_t index) {
        auto characters = byteCast<char>(strings[check.program.modules[modulesByPath[index]].path]);
        return std::string_view { characters.data(), characters.size() };
    };
    bool pathsAscend = true;
    for (size_t index = 1; pathsAscend && index < modulesByPath.size(); ++index)
        pathsAscend = pathAt(index - 1) < pathAt(index);
    check.expect(pathsAscend, ProgramTableRule::TableNotSorted);
}

static void checkSortedTables(const TableCheck& check)
{
    auto checkAscending = [&](auto entries, const auto& keyOf) {
        check.expect(isStrictlyAscending(entries, keyOf), ProgramTableRule::TableNotSorted);
    };
    checkAscending(check.tables.constructors(), &ConstructorData::function);
    checkAscending(check.tables.boundFunctions(), &BoundFunctionData::function);
    checkAscending(check.tables.constructEntrypoints(), &ConstructEntrypointData::function);
    checkAscending(check.tables.implementedBuiltins(), &ImplementedBuiltinData::function);
    checkAscending(check.tables.staticsRoots(), &StaticsRange::offset);
    checkAscending(check.tables.staticsNarrowRoots(), &StaticsRange::offset);
    checkAscending(check.tables.staticsRuns(), &StaticsRun::firstWord);
    check.expect(std::ranges::is_sorted(check.tables.staticsOfFiles(), { }, &StaticsOfFiles::firstWord) && std::ranges::is_sorted(check.tables.staticsOfFiles(), { }, &StaticsOfFiles::firstImportedGroup), ProgramTableRule::TableNotSorted);
    check.expect(std::ranges::all_of(check.tables.staticsOfImportedFiles(), [&](uint32_t file) {
        return file + 1 < check.tables.staticsOfFiles().size();
    }), ProgramTableRule::TableOutOfBounds);
    checkAscending(check.tables.importsOfHeldFiles(), &ImportOfHeldFileData::key);
    checkAscending(check.tables.compiledRuntimeModules(), &CompiledRuntimeModuleData::registryID);
    checkAscending(check.tables.compiledRuntimeFunctions(), &CompiledRuntimeFunctionData::id);

    check.expect(isStrictlyAscending(check.tables.builtinCells(), std::identity { }) && isStrictlyAscending(check.tables.eagerBuiltinCells(), std::identity { }), ProgramTableRule::TableNotSorted);
    check.expect(check.tables.eagerBuiltinCells().empty() || check.tables.eagerBuiltinCells().back() < check.tables.builtinCells().size(), ProgramTableRule::TableOutOfBounds);

    const BuiltinTable& builtins = check.program.builtins();
    std::span<const uint32_t> openedNames = check.tables.openedNames();
    bool everyMemberExists = std::ranges::all_of(check.tables.constantAndUnlockedBuiltinMembers().first(check.tables.numberOfWordsBelow(ProgramTables::openedName)), [&](uint32_t member) {
        return builtins.contains(BuiltinID(member & ~ProgramTables::unlockedBuiltinMember));
    });
    bool everyOpenedNameExists = !(openedNames.size() % 2);
    for (size_t index = 0; everyOpenedNameExists && index < openedNames.size(); index += 2)
        everyOpenedNameExists = builtins.contains(BuiltinID(openedNames[index] & ~ProgramTables::openedName)) && (openedNames[index + 1] & ~ProgramTables::openedName) < check.program.strings.size();
    if (check.expect(everyMemberExists && everyOpenedNameExists, ProgramTableRule::TableOutOfBounds)) {
        checkAscending(check.tables.constantBuiltinMembers(), [&](uint32_t member) {
            return std::pair { builtins.at(BuiltinID(member)).holder, BuiltinID(member) };
        });
        check.expect(isStrictlyAscending(check.tables.unlockedBuiltinMembers(), std::identity { }), ProgramTableRule::TableNotSorted);
        // Compiled code takes the one kind for constants and tests the other.
        check.expect(std::ranges::none_of(check.tables.constantBuiltinMembers(), [&](uint32_t member) {
            return ProgramTables::isUnlocked(check.tables.unlockedBuiltinMembers(), member);
        }), ProgramTableRule::TableOutOfBounds);
    }
    checkModuleTables(check);
}

static void checkListedFunctions(const TableCheck& check)
{
    for (size_t index = 0; index < check.program.functions.size(); ++index) {
        const Function& function = check.program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionID boundEntrypoint = function.role == FunctionRole::BoundFunction ? check.encoder.findEntrypoint(id) : FunctionID();
        check.expect(!boundEntrypoint || std::ranges::binary_search(check.tables.boundFunctions(), boundEntrypoint.index(), { }, &BoundFunctionData::function), ProgramTableRule::FunctionNotListed, { }, id);
        auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
        bool hasConstructEntrypoint = function.role == FunctionRole::GenericEntrypoint && entrypoint && entrypoint->constructEntrypoint;
        check.expect(!hasConstructEntrypoint || std::ranges::binary_search(check.tables.constructEntrypoints(), check.encoder.placement.closureNumberOf(id), { }, &ConstructEntrypointData::function), ProgramTableRule::FunctionNotListed, { }, id);
    }
    for (size_t index = 0; index < check.program.layouts.size(); ++index) {
        FunctionID constructor = check.program.layouts[index].constructor;
        FunctionID entrypoint = check.encoder.placement.isInUse(LayoutID(index)) ? check.encoder.findEntrypoint(constructor) : FunctionID();
        check.expect(!entrypoint || std::ranges::binary_search(check.tables.constructors(), entrypoint.index(), { }, &ConstructorData::function), ProgramTableRule::FunctionNotListed, LayoutID(index), constructor);
    }
}

static void checkGuardSets(const TableCheck& check, LayoutID id, uint32_t guard, MemberGuardAggregates aggregates)
{
    if (!aggregates.count)
        return;
    std::span<const uint32_t> words = check.tryGetWords(aggregates.offset, 2 * static_cast<uint64_t>(aggregates.count));
    if (!check.expect(!words.empty(), ProgramTableRule::TableOutOfBounds, id))
        return;
    auto isInStatics = [&](uint32_t number) {
        return number / 8 < check.encoder.inputs.instanceSizes.sizeOfStatics;
    };
    for (uint32_t index = 0; index < aggregates.count; ++index) {
        uint32_t set = MemberGuardAggregates::numberInListEntry(words[index]);
        std::span<const uint32_t> members = check.tryGetCountedWords(words[aggregates.count + index]);
        if (!check.expect(members.size() >= 2 && isInStatics(set) && std::ranges::all_of(members, isInStatics, MemberGuardAggregates::numberInListEntry), ProgramTableRule::TableOutOfBounds, id))
            return;
        if (!check.expect(std::ranges::contains(members, guard, MemberGuardAggregates::numberInListEntry) && !std::ranges::contains(members, set, MemberGuardAggregates::numberInListEntry), ProgramTableRule::GuardMissingFromSet, id))
            return;
    }
}

static void checkClassRecord(const TableCheck& check, const LayoutData& layout, LayoutID id)
{
    if (uint32_t offset = layout.classData().subclassesOffset) {
        std::span<const uint32_t> subclasses = check.tryGetCountedWords(offset);
        bool areClasses = !subclasses.empty() && std::ranges::all_of(subclasses, [&](uint32_t index) {
            return index < check.program.layouts.size() && check.program.layouts[index].kind == LayoutKind::Instance;
        });
        if (check.expect(areClasses, ProgramTableRule::TableOutOfBounds, id))
            check.expect(isStrictlyAscending(subclasses, std::identity { }), ProgramTableRule::TableNotSorted, id);
    }
    if (uint32_t offset = layout.classData().namesOfFieldsOffset)
        check.expect(layout.classData().instancesCarryBrand && check.tryGetCountedWords(offset).size() == check.program.at(id).fields.size(), ProgramTableRule::TableOutOfBounds, id);
    bool guardsAreSorted = isStrictlyAscending(layout.memberGuards(), [](const MemberGuardData& guard) {
        return std::tuple { guard.side(), guard.kind(), static_cast<uint32_t>(guard.name) };
    });
    check.expect(guardsAreSorted, ProgramTableRule::TableNotSorted, id);
    for (const MemberGuardData& guard : layout.memberGuards())
        checkGuardSets(check, id, MemberGuardAggregates::bitNumber(layout.classData().guardWordsOffset[static_cast<unsigned>(guard.side())], guard.bit), layout.aggregatesOf(guard));

    if (layout.classData().environment != LayoutData::noBase)
        return;
    for (std::span<const MethodData> methods : { layout.methods(), layout.staticMethods() }) {
        for (const MethodData& method : methods)
            check.expect(!(method.flags & methodTakesEnvironment) || (method.flags & methodIsCreatedAtDefinition), ProgramTableRule::ClassCapturesMismatch, id, FunctionID(method.function));
    }
}

static void checkObjectRecord(const TableCheck& check, uint32_t offset, LayoutID id, uint32_t shape)
{
    const LayoutData* layout = check.tryGetRecord<LayoutData>(offset);
    bool isInBounds = layout && offset >= sizeof(ProgramTables) + layout->prefixSize() && offset + sizeof(LayoutData) + layout->trailingSize() <= check.bytes.size();
    if (!check.expect(isInBounds, ProgramTableRule::TableOutOfBounds, id))
        return;
    check.expect(layout->shape == shape && layout->index == id.index(), ProgramTableRule::ShapeWithoutLayout, id);
    auto hasUnknownFlag = [](const FieldData& field) {
        return !!(field.flags & ~allFieldFlags);
    };
    check.expect(std::ranges::none_of(layout->fields(), hasUnknownFlag) && std::ranges::none_of(layout->staticFields(), hasUnknownFlag), ProgramTableRule::UnknownFieldFlag, id);
    if (layout->isClass())
        checkClassRecord(check, *layout, id);
}

static void checkLayoutRecords(const TableCheck& check)
{
    const Placement& placement = check.encoder.placement;
    const ShapeTable& shapes = placement.shapes();
    size_t numberOfLayouts = check.program.layouts.size();
    auto offsets = check.tables.layouts();

    size_t endOfShapes = std::max<size_t>(offsets.size(), static_cast<size_t>(shapes.numberOfShapes()) + 1);
    for (uint32_t shape = 0; shape < endOfShapes; ++shape) {
        uint32_t offset = shape < offsets.size() ? offsets[shape] : 0;
        LayoutID id = shapes.isAssigned(shape) ? shapes.shape(shape).layout : LayoutID();
        if (id && !placement.isInUse(id))
            id = { };
        if (!check.expect(!offset == !id, ProgramTableRule::ShapeWithoutLayout, id) || !id)
            continue;
        if (shape < check.tables.firstEnvironmentShape) {
            checkObjectRecord(check, offset, id, shape);
            continue;
        }
        const EnvironmentData* environment = check.tryGetRecord<EnvironmentData>(offset);
        if (check.expect(environment && check.tryGetRecord<EnvironmentData>(offset, static_cast<uint64_t>(environment->numberOfDescribedVariables) * sizeof(VariableData)), ProgramTableRule::TableOutOfBounds, id))
            check.expect(environment->shape == shape && environment->index == id.index(), ProgramTableRule::ShapeWithoutLayout, id);
    }

    if (!check.expect(check.tables.numberOfLayoutIndices == numberOfLayouts && isTableOfSetBits(check.tables.classes(), (numberOfLayouts + 31) / 32 * 2), ProgramTableRule::TableOutOfBounds))
        return;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        uint32_t offset = check.tables.offsetOfClass(index);
        if (!offset)
            continue;
        uint32_t generalShape = shapes.generalShapeOf(id);
        if (!generalShape || generalShape >= offsets.size() || offsets[generalShape] != offset)
            checkObjectRecord(check, offset, id, generalShape);
        check.expect(placement.rankOfClass(id) == check.tables.rankOfClass(index), ProgramTableRule::TableOutOfBounds, id);
    }
}

static void checkFieldTable(const TableCheck& check)
{
    const uint32_t highestObjectShape = check.tables.firstEnvironmentShape - 1;
    const uint32_t typeOfAnyValue = check.encoder.typeOffset(TypeTable::anything);
    const uint32_t numberOfColumns = check.encoder.identifiers.countWithFieldTableColumn();
    if (!check.expect(check.tables.numberOfFieldTableColumns == numberOfColumns && numberOfColumns <= FieldTableEntry::maxName + 1 && check.tables.numberOfFieldTableRows == highestObjectShape && highestObjectShape <= check.tables.fieldTable().size() && check.tables.fieldRecordIndices().size() == check.tables.fieldTable().size() - highestObjectShape, ProgramTableRule::TableOutOfBounds))
        return;
    auto entries = check.tables.fieldTableEntries();
    auto isRowInBounds = [&](uint32_t row) {
        return row <= entries.size() && entries.size() - row >= numberOfColumns;
    };
    if (!check.expect(std::ranges::all_of(check.tables.fieldTable().first(highestObjectShape), isRowInBounds), ProgramTableRule::TableOutOfBounds))
        return;

    Vector<uint32_t> numberOfEntriesOfRow(FillWith { }, entries.size(), 0);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        FieldTableEntry entry { entries[index] };
        if (!entry.isEmpty() && check.expect(entry.name() <= index, ProgramTableRule::FieldTableEntryWithoutField))
            ++numberOfEntriesOfRow[index - entry.name()];
    }

    BitVector rowsOfShapes(entries.size());
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t row = check.tables.fieldTableRowOf(shape);
        rowsOfShapes.quickSet(row);
        uint32_t numberOfFieldsFound = 0;
        LayoutID id = check.encoder.placement.shapes().shape(shape).layout;
        if (check.tables.hasLayoutForShape(shape)) {
            const LayoutData& layout = check.tables.layoutForShape(shape);
            for (const FieldData& field : layout.fields()) {
                if (!check.tables.hasFieldTableColumn(field.name))
                    continue;
                ++numberOfFieldsFound;
                uint32_t index = row + field.name;
                bool isInTable = findFieldInTable(check.tables, shape, field.name) == layout.indexOf(field) && FieldTableEntry { entries[index] } == FieldTableEntry::forField(layout, field)
                    && check.tables.fieldRecordIndices()[index] == FieldRecordIndex::create(field, layout.indexOf(field), check.tables.typeAt(field.type), typeOfAnyValue).bits;
                check.expect(isInTable, ProgramTableRule::FieldMissingFromFieldTable, id);
            }
        }
        check.expect(numberOfEntriesOfRow[row] == numberOfFieldsFound, ProgramTableRule::FieldTableEntryWithoutField, id);
    }

    auto rowsOfClasses = check.tables.classMemberRows();
    auto accessors = check.tables.classAccessors();
    if (!check.expect(rowsOfClasses.size() == check.tables.numberOfClasses(), ProgramTableRule::TableOutOfBounds))
        return;
    const uint32_t* offsetsOfClasses = check.tables.classes().data() + check.tables.numberOfClassGroupWords();
    for (uint32_t rank = 0; rank < rowsOfClasses.size(); ++rank) {
        std::optional<Vector<DeclaredClassMember>> members = collectDeclaredClassMembers(check.encoder, *check.tables.at<LayoutData>(offsetsOfClasses[rank]));
        uint32_t row = rowsOfClasses[rank];
        if (!check.expect(members.has_value() == (row != ClassRowEntry::noRow), ProgramTableRule::MemberMissingFromClassMemberTable) || !members)
            continue;
        if (!check.expect(isRowInBounds(row), ProgramTableRule::TableOutOfBounds))
            continue;
        // Rows that say the same share their entries, whoever reads them.
        rowsOfShapes.quickSet(row);
        for (const DeclaredClassMember& member : *members) {
            uint32_t index = row + member.name;
            FieldTableEntry entry { entries[index] };
            uint32_t number = ClassRowEntry::numberIn(entry, check.tables.fieldRecordIndices()[index]);
            bool isRight = entry.name() == member.name && (member.method
                ? ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Method && number == member.method
                : ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Accessor && number < accessors.size() && std::bit_cast<ClassAccessorData>(accessors[number]) == ClassAccessorData { member.getter, member.setter });
            check.expect(isRight, ProgramTableRule::MemberMissingFromClassMemberTable);
        }
        check.expect(numberOfEntriesOfRow[row] == members->size(), ProgramTableRule::ClassMemberEntryWithoutMember);
    }
    for (uint32_t row = 0; row < entries.size(); ++row)
        check.expect(!numberOfEntriesOfRow[row] || rowsOfShapes.quickGet(row), ProgramTableRule::FieldTableEntryWithoutField);
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
        // A function without a generic entrypoint has no closures, and a class without a rank has no instances, so their tests have no record.
        plan.cellTests.removeAllMatching([&](const CellTest& test) {
            if (test.kind == CellTestKind::CarriesBrandOfClass)
                return !encoder.placement.rankOfClass(test.classLayout);
            return test.kind == CellTestKind::IsClosureOf && !encoder.findEntrypoint(test.function);
        });
        // In a plan, String says that some string passes. In a record it says that every string passes.
        uint16_t primitives = plan.stringLiterals.isEmpty() ? plan.passingPrimitives : (plan.passingPrimitives & ~Primitive::String) | Primitive::LiteralString;
        plan.stringLiterals.removeAllMatching([&](StringID literal) {
            return encoder.inputs.pipeline.literalStrings->isLeftOut(literal);
        });
        return { .id = id, .primitives = primitives, .integers = plan.integers, .cellTests = WTF::move(plan.cellTests), .literalStrings = WTF::move(plan.stringLiterals) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOf(const TableCheck& check, const RuntimeCellTest& encoded, const CellTest& test)
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
        const ShapeTestData* shapeTest = check.tryGetRecord<ShapeTestData>(encoded.shapeTestOffset());
        if (!shapeTest)
            return false;
        const ShapeTable& shapes = check.encoder.placement.shapes();
        ShapeTestData expected = shapes.dataOf(shapes.testOf(test.layouts));
        return shapeTest->last == expected.last && shapeTest->count == expected.count && shapeTest->bitSetOffset == expected.bitSetOffset;
    }
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        return encoded.builtinClass() == test.builtinClass;
    case CellTestKind::IsBuiltinCell:
        return check.encoder.placement.builtinCell(test.builtin) == encoded.builtinCellNumber();
    case CellTestKind::IsClosureOf:
        return encoded.closureNumber() == check.encoder.placement.closureNumberOf(check.encoder.findEntrypoint(test.function));
    case CellTestKind::IsSymbol:
        return encoded.symbolKeyNumber() == check.encoder.identifierNumber(test.symbol, "the symbol that a type test compares with"_s);
    case CellTestKind::SharingModeIs:
        return encoded.operand == static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return encoded.module() == test.module;
    case CellTestKind::CarriesBrandOfClass:
        return check.encoder.placement.rankOfClass(test.classLayout) == encoded.rankOfClass();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isEncodingOf(const TableCheck& check, const RuntimeType& encoded, const ExpectedRuntimeType& expected)
{
    auto integers = encoded.integerRange();
    if (encoded.primitives != expected.primitives || integers.has_value() != expected.integers.has_value())
        return false;
    if (integers && (integers->first != expected.integers->minimum || integers->second != expected.integers->maximum))
        return false;
    if (!std::ranges::equal(encoded.literalStrings(), expected.literalStrings.span(), { }, { }, &StringID::index))
        return false;
    return std::ranges::equal(encoded.cellTests(), expected.cellTests, [&](const RuntimeCellTest& cellTest, const CellTest& test) {
        return isEncodingOf(check, cellTest, test);
    });
}

static void checkRuntimeTypes(const TableCheck& check)
{
    std::span<const uint32_t> words = check.tables.types();
    if (!check.expect(!words.empty() && words[0] == check.program.types.size() && isTableOfSetBits(words.subspan(1), ProgramTables::numberOfTypeGroupWords(check.program.types.size())), ProgramTableRule::TableOutOfBounds))
        return;
    auto expectedTypes = collectInParallel<ExpectedRuntimeType>(check.encoder.build.parallelLoops(), check.program.types.size(), [&](unsigned index, Vector<ExpectedRuntimeType>& collected) {
        if (check.tables.offsetOfType(index))
            collected.append(computeExpectedRuntimeType(check.encoder, TypeID(index)));
    });
    for (const ExpectedRuntimeType& expected : expectedTypes) {
        uint32_t offset = check.tables.offsetOfType(expected.id.index());
        const RuntimeType* encoded = check.tryGetRecord<RuntimeType>(offset);
        if (check.expect(encoded && offset + RuntimeType::sizeInBytes(encoded->numberOfCellTests, encoded->numberOfLiteralStrings, encoded->flags) <= check.bytes.size(), ProgramTableRule::TableOutOfBounds) && !isEncodingOf(check, *encoded, expected))
            check.encoder.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RuntimeTypeStricterThanPlan, .count = expected.id.index() });
    }
}

static void checkObfuscation(const TableCheck& check)
{
    if (!check.encoder.obfuscates())
        return;
    uint32_t emptyString = check.program.strings.emptyString().index();
    auto modules = check.tables.modules();
    bool hasFunctionName = std::ranges::any_of(check.tables.functionNames(), [&](uint32_t name) {
        return name != emptyString;
    }) || std::ranges::any_of(check.tables.functionNameRuns(), [&](const FunctionNameRunData& run) {
        return run.name != emptyString;
    });
    // The names of the files of the host's library are no names of the program.
    bool hasDisplayPath = false;
    for (size_t index = 0; index < std::min<size_t>(modules.size(), check.program.modules.size()); ++index)
        hasDisplayPath |= modules[index].charactersInFrontOfDisplayPath != ModuleData::hasNoDisplayPath && !check.program.modules[index].runtimeModule;
    bool hasTypeText = std::ranges::any_of(check.tables.typeSites(), [](const TypeSiteData& site) {
        return site.text != TypeSiteData::noText;
    });
    check.expect(!hasFunctionName && !hasDisplayPath && !hasTypeText, ProgramTableRule::NameLeftInObfuscatedBuild);
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

static bool namesOtherMembers(std::span<const uint64_t> words, const BitVector& headers, uint64_t word)
{
    if (DataWord::kindOf(word) != DataWord::Kind::Node || !headers.get(DataWord::operandOf(word)))
        return false;
    return DataNodeHeader::decode(words[DataWord::operandOf(word)]).kind() == DataNodeHeader::Kind::OtherMembers;
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
        if (!namesOtherMembers(words, headers, body.back()))
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
        if (namesOtherMembers(words, headers, word))
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
                if (broken == ProgramTableRule::None)
                    broken = namesOtherMembers(words, *headers, word) ? ProgramTableRule::DataNotEncoded : findBrokenRule(encoder, tables, *headers, word);
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

void checkProgramTables(const TableEncoder& encoder, std::span<const uint8_t> bytes)
{
    if (!checkTableBounds(encoder, bytes))
        return;
    TableCheck check { encoder, encoder.program, reinterpretCastSpanStartTo<ProgramTables>(bytes), bytes };
    checkSortedTables(check);
    checkListedFunctions(check);
    checkLayoutRecords(check);
    checkFieldTable(check);
    checkRuntimeTypes(check);
    checkObfuscation(check);
    for (size_t index : encoder.slots.keysWithColumn()) {
        std::optional<uint32_t> number = encoder.identifiers.numberOf(PropertyKeyID(index));
        if (!check.expect(!number || RowDisplacementTable::hasColumn(*number), ProgramTableRule::KeyWithoutPromisedColumn))
            break;
    }
    ProgramTableRule brokenDataRule = findBrokenDataRule(encoder, check.tables);
    check.expect(brokenDataRule == ProgramTableRule::None, brokenDataRule);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
