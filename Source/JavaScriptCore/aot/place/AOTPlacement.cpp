/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacement.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTDefinitions.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowResults.h"
#include "AOTGrouping.h"
#include "AOTInterningPhases.h"
#include "AOTLimits.h"
#include "AOTNameClass.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacementSteps.h"
#include "AOTRegExpRequests.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <wtf/MathExtras.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>


namespace JSC { namespace AOT { namespace Typed {

template<typename Element, typename Key>
static std::optional<uint32_t> findPosition(const Vector<Element>& ascending, Key key, const auto& keyOf)
{
    auto* found = tryBinarySearch<const Element, Key>(ascending, ascending.size(), key, [&](const Element* element) {
        return std::invoke(keyOf, *element);
    });
    if (!found)
        return std::nullopt;
    return static_cast<uint32_t>(found - ascending.begin());
}

FieldAccessID Placement::findFieldAccess(PropertyKeyID key, LayoutSetID receivers, bool isStore) const
{
    auto position = findPosition(m_fieldAccesses, SlotAccessKey { key, receivers, isStore }, [](const FieldAccess& access) {
        return SlotAccessKey { access.key, access.receivers, access.isStore };
    });
    return position ? FieldAccessID(*position) : FieldAccessID();
}

std::optional<uint32_t> Placement::moduleBuiltinCellOffset(BuiltinID builtin, ModuleID module) const
{
    auto& cells = m_statics.moduleBuiltinCells;
    auto position = findPosition(cells, ModuleBuiltinUse { builtin, module }, [](const ModuleBuiltinCell& cell) {
        return ModuleBuiltinUse { cell.builtin, cell.module };
    });
    if (!position)
        return std::nullopt;
    return cells[*position].offset;
}

std::optional<uint32_t> Placement::builtinCell(BuiltinCellKey key) const
{
    return findPosition(m_constants.builtinCells, key.encoded(), &BuiltinCellKey::encoded);
}

std::optional<uint32_t> Placement::bigIntConstant(StringID digits) const
{
    return findPosition(m_constants.bigIntConstants, digits, std::identity { });
}

std::optional<uint32_t> Placement::requireEdge(ModuleID referrer, ModuleID target) const
{
    return findPosition(m_constants.requireEdges, RequireEdge { referrer, target }, std::identity { });
}

AllocationID AllocationTable::find(LayoutID layout, uint32_t shape, std::span<const FieldIndex> initialFields) const
{
    for (AllocationID id : ofLayout(layout)) {
        const PlacedAllocation& allocation = allocations[id.index()];
        if (allocation.shape == shape && equalSpans(allocation.initialFields.span(), initialFields))
            return id;
    }
    return { };
}

std::optional<uint32_t> KeyOrders::findOrderWord(LayoutID layout, std::span<const FieldIndex> fields) const
{
    using FieldList = std::span<const FieldIndex>;
    if (fields.empty())
        return orderWordOf(0, 0);
    auto orders = of(layout);
    auto isLexicographicallyLess = [](FieldList a, FieldList b) {
        return std::ranges::lexicographical_compare(a, b);
    };
    auto order = std::ranges::lower_bound(orders, fields, isLexicographicallyLess, &Vector<FieldIndex>::span);
    if (order == orders.end() || !spanHasPrefix(order->span(), fields))
        return std::nullopt;
    return orderWordOf(static_cast<uint32_t>(order - orders.begin()), static_cast<uint32_t>(fields.size()));
}

std::span<const KeyOrderContinuation> KeyOrders::continuationsCreating(LayoutID layout, std::span<const FieldIndex> fields) const
{
    auto isBefore = [&](const PlacedFieldCreation& creation) {
        if (creation.layout != layout)
            return creation.layout < layout;
        return std::ranges::lexicographical_compare(creation.fields, fields);
    };
    auto found = std::ranges::partition_point(fieldCreations, isBefore);
    if (found == fieldCreations.end() || found->layout != layout || !std::ranges::equal(found->fields, fields))
        return { };
    return found->continuations.span();
}

std::optional<uint32_t> KeyOrders::numberOfStoreThatMayAdd(FunctionID function, uint32_t storesInFront) const
{
    if (function.index() + 1 >= firstStoreThatMayAdd.size())
        return std::nullopt;
    uint32_t first = firstStoreThatMayAdd[function.index()];
    if (storesInFront >= firstStoreThatMayAdd[function.index() + 1] - first)
        return std::nullopt;
    return first + storesInFront;
}

GuardBitID GuardBitTable::find(LayoutID classLayout, ClassSide side, MemberGuardKind kind, PropertyKeyID key) const
{
    using GuardBitKey = std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID>;
    auto position = findPosition(bits, GuardBitKey { classLayout, side, kind, key }, [](const PlacedGuardBit& bit) {
        return GuardBitKey { bit.classLayout, bit.side, bit.kind, bit.key };
    });
    return position ? GuardBitID(*position) : GuardBitID();
}

std::span<const PlacedGuardBit> GuardBitTable::ofClass(LayoutID classLayout) const
{
    return std::span<const PlacedGuardBit>(std::ranges::equal_range(bits, classLayout, { }, &PlacedGuardBit::classLayout));
}

static void countStatistics(const Placement& placement, const PlacedSites& sites, Statistics& statistics)
{
    const ShapeTable& shapes = placement.shapes();
    auto& tests = sites.shapeTests;
    uint64_t neverPasses = 0;
    uint64_t ranges = 0;
    uint64_t bitSets = 0;
    auto countOf = [&](ShapeTestForm form) -> uint64_t& {
        switch (form) {
        case ShapeTestForm::NeverPasses:
            return neverPasses;
        case ShapeTestForm::AlwaysPasses:
        case ShapeTestForm::OneShape:
        case ShapeTestForm::UpTo:
        case ShapeTestForm::From:
        case ShapeTestForm::Range:
        case ShapeTestForm::Block:
        case ShapeTestForm::BlockOrEarlier:
            return ranges;
        case ShapeTestForm::ImmediateBits:
        case ShapeTestForm::BitSet:
            return bitSets;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (size_t index = 0; index < tests.size(); ++index) {
        if (!index || tests[index].tested != tests[index - 1].tested)
            ++countOf(shapes.testOf(tests[index].tested).form);
    }

    statistics.count(StatisticID::Shapes, shapes.shapeCountInRange(1, shapes.numberOfShapes()));
    statistics.count(StatisticID::ShapeTestNeverPasses, neverPasses);
    statistics.count(StatisticID::ShapeTestRange, ranges);
    statistics.count(StatisticID::ShapeTestBitSet, bitSets);
    statistics.count(StatisticID::GuardBitsPlaced, placement.guardBits().bits.size());
    statistics.count(StatisticID::RequestsShared, placement.requests().numberOfSharedRequests());
}

// encodeLayoutRecords gives every layout of kind Instance that is in use a class record, in the order of the layouts.
static Vector<std::optional<uint32_t>> rankClasses(const Program& program, const BitVector& layoutsOutOfUse)
{
    Vector<std::optional<uint32_t>> ranks(program.layouts.size());
    uint32_t numberOfClasses = 0;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Instance && !layoutsOutOfUse.get(index))
            ranks[index] = numberOfClasses++;
    }
    return ranks;
}

static FieldReference fieldOf(const Program& program, LayoutID member, PropertyKeyID key)
{
    return { member, *program.at(member).findField(key) };
}

static LayoutID findMemberOutsideLocation(const SlotAccessKey& access, const SlotAgreement& agreement)
{
    if (agreement.hasOffsetByShape(access.isStore))
        return { };
    if (agreement.groups.size() > 1)
        return agreement.groups[1].members[0];
    if (access.isStore && !agreement.coversEveryMember())
        return agreement.membersWithoutSlot[0];
    return { };
}

static String describeDisagreement(const Program& program, const SlotAccessKey& access, const SlotAgreement& agreement)
{
    StringPrintStream out;
    out.print(access.isStore ? "a store"_s : "a read"_s);
    out.print(" through "_s, program.types.at(access.receivers).members.size(), " layouts, which keep it in "_s, agreement.groups.size(), " places; "_s);
    out.print(agreement.membersWithoutSlot.size(), " of them have no slot for it"_s);
    return out.toString();
}

// The uses are folded by what they access. Only the report of an error asks where one of them stands: the first, by function, block and value.
static InternalError errorAtFirstSite(const Program& program, const SlotAccessKey& access, InternalError&& error)
{
    auto isSite = [&](const Value& value) {
        if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
            return access.isStore && value.receivers() == access.receivers && std::ranges::any_of(value.placedKeys(), [&](PlacedKey placedKey) {
                return placedKey.key == access.key;
            });
        }
        return Value::opcodesWithFieldAccess.contains(value.opcode) && value.receivers() == access.receivers && value.key() == access.key && isSlotStore(value.opcode) == access.isStore;
    };
    for (uint32_t functionIndex = 0; functionIndex < program.functions.size(); ++functionIndex) {
        const Function& function = program.functions[functionIndex];
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID id : function.blocks[blockIndex].values) {
                if (!isSite(function.at(id)))
                    continue;
                error.function = FunctionID(functionIndex);
                error.block = BlockID(blockIndex);
                error.value = id;
                return WTF::move(error);
            }
        }
    }
    return WTF::move(error);
}

// intersectionOf keeps a built-in class that may overlap a class of the other type, so its result can hold an object that one member does not declare.
static TypeID storableContentsOf(const Program& program, TypeArena& arena, TypeID intersection, PropertyKeyID key, const SlotGroup& group)
{
    TypeID contents = intersection;
    for (LayoutID member : group.members) {
        TypeID declared = slotTypeOf(program, fieldAt(program, declarationOf(program, fieldOf(program, member, key))));
        contents = arena.differenceOf(contents, arena.differenceOf(contents, declared));
    }
    return contents;
}

static FieldAccess createFieldAccess(const PlacementContext& context, TypeArena& arena, const SlotAccessKey& access)
{
    const Program& program = context.program;
    FieldAccess row;
    row.key = access.key;
    row.receivers = access.receivers;
    row.isStore = access.isStore;
    row.contents = TypeTable::absent;

    SlotAgreement agreement = context.slots().agreementOf(program, access.key, access.receivers);
    if (LayoutID member = findMemberOutsideLocation(access, agreement)) {
        context.build.diagnostics().add(errorAtFirstSite(program, access, InternalError { .kind = InternalErrorKind::SlotAccessNotPlaced, .layout = member, .detail = describeDisagreement(program, access, agreement) }));
        return row;
    }
    if (agreement.groups.isEmpty())
        return row;

    row.findsOffsetByShape = agreement.hasOffsetByShape(access.isStore);
    const SlotGroup group = row.findsOffsetByShape ? agreement.groupOfAllOffsets() : agreement.groups[0];
    bool hasSeveralMembers = group.members.size() > 1;
    if (agreement.coversEveryMember())
        row.coverage = FieldAccess::Coverage::EveryMember;
    else
        row.coverage = agreement.membersWithoutSlotKeepBytesZero ? FieldAccess::Coverage::EveryMemberHasSlot : FieldAccess::Coverage::SomeMembers;
    row.slot = group.slot;
    if (hasSeveralMembers) {
        row.slot.deferredFunction = { };
        row.slot.indexInLayoutRecord = std::nullopt;
    }
    row.extensionWord = group.extensionWord;
    bool everyReceiverIsMember = group.members.size() == program.types.at(access.receivers).members.size();
    row.membersWithField = everyReceiverIsMember ? access.receivers : arena.completeLayoutSet(group.members.span());

    TypeID contents;
    bool isAlwaysFilled = true;
    for (LayoutID member : group.members) {
        FieldReference field = fieldOf(program, member, access.key);
        const PlacedSlot& slot = context.slots().slot(field);
        row.mayBeFilledWithoutStorage |= slot.mayExistWithoutStorage;
        if (hasSeveralMembers) {
            // A read asks the key order where some member may list the field without storage. A store of undefined asks for no storage only where every member may.
            if (access.isStore)
                row.slot.mayExistWithoutStorage &= slot.mayExistWithoutStorage;
            else
                row.slot.mayExistWithoutStorage |= slot.mayExistWithoutStorage;
            row.slot.deferredClosureCapturesObject |= slot.deferredClosureCapturesObject;
            row.slot.deferredClosureCapturesEnvironment |= slot.deferredClosureCapturesEnvironment;
        }
        TypeID type = slotTypeOf(program, fieldAt(program, declarationOf(program, field)));
        if (!contents)
            contents = type;
        else
            contents = row.isStore ? arena.intersectionOf(contents, type) : arena.unionOf(contents, type);
        isAlwaysFilled &= flowOf(program, field).occupancy == Occupancy::AlwaysFilled;
    }

    // A load from an Extension that is missing or too short answers absent, whatever flow proved about the field.
    bool slotMayBeEmpty = canLeaveSlot(row.slot.representation) && (row.slot.isOutOfLine() || !isAlwaysFilled);
    if (row.isStore)
        row.contents = hasSeveralMembers ? storableContentsOf(program, arena, contents, row.key, group) : contents;
    else if (row.coverage == FieldAccess::Coverage::EveryMember && !slotMayBeEmpty)
        row.contents = contents;
    else
        row.contents = arena.withPrimitives(contents, Primitive::Absent);
    return row;
}

static AllocationTable numberAllocations(const PlacementContext& context, const PlacedSites& sites)
{
    static_assert(!(Closure::allocationSize(1) % 16));
    static_assert(defaultLimitValue(LimitID::MaxCaptures, hostArchitecture()) <= Closure::maxNumberOfCaptures);
    RELEASE_ASSERT(sites.maxClosureCaptures <= Closure::maxNumberOfCaptures);
    auto& program = context.program;
    AllocationTable table;
    table.allocations.reserveInitialCapacity(sites.maxClosureCaptures / 2 + 1 + sites.allocations.size());
    for (unsigned index = 0; index <= sites.maxClosureCaptures / 2; ++index) {
        table.closureAllocations.append(AllocationID(index));
        table.allocations.append(PlacedAllocation { .layout = context.closureLayout, .numberOfCaptures = static_cast<uint16_t>(2 * index + 1) });
    }
    for (auto& use : sites.allocations) {
        auto& layout = program.at(declaredLayoutOf(program, use.layout));
        table.allocations.append(PlacedAllocation {
            .layout = use.layout,
            .constructedBy = layout.kind == LayoutKind::Instance ? layout.constructor : FunctionID(),
            .initialFields = use.initialFields,
        });
    }
    groupByIndex(program.layouts.size(), table.firstOfLayout, table.byLayout, [&](const auto& visit) {
        for (size_t i = 0; i < table.allocations.size(); ++i)
            visit(table.allocations[i].layout.index(), AllocationID(i));
    });
    return table;
}

static void finishAllocations(const PlacementContext& context, const ShapeTable& shapes, const PlacedOrderWords& orderWords, std::span<const AllocationUse> uses, AllocationTable& table)
{
    auto& allocations = table.allocations;
    size_t numberOfClosureAllocations = table.closureAllocations.size();
    RELEASE_ASSERT(allocations.size() == numberOfClosureAllocations + uses.size());
    RELEASE_ASSERT(orderWords.ofAllocation.size() == allocations.size());

    for (AllocationID id : table.closureAllocations) {
        auto& allocation = allocations[id.index()];
        allocation.shape = shapes.generalShapeOf(context.closureLayout);
        allocation.size = static_cast<uint32_t>(WTF::roundUpToMultipleOf<16>(Closure::allocationSize(allocation.numberOfCaptures)));
        allocation.orderWord = 0;
    }

    for (size_t i = 0; i < uses.size(); ++i) {
        size_t row = numberOfClosureAllocations + i;
        auto& allocation = allocations[row];
        ShapeRange range = shapes.shapesOf(uses[i].layout);
        allocation.shape = range.first + (uses[i].keysAreNeverEnumerated && range.count == 2 ? 1 : 0);
        allocation.size = context.slots().layout(allocation.layout).size;
        bool keysAreNeverEnumerated = shapes.isAssigned(allocation.shape) && shapes.shape(allocation.shape).keysAreNeverEnumerated;
        allocation.orderWord = keysAreNeverEnumerated ? unorderedOrderWord : orderWords.ofAllocation[row];
    }
}

enum class TypesTestedByCode : uint8_t { Exclude, Include };
enum class LayoutsOutOfUse : uint8_t { Exclude, Include };

static BitVector computeTypesNeededAtRunTime(const PlacementContext& context, const PlacedSites& sites, TypesTestedByCode typesTestedByCode, LayoutsOutOfUse layoutsOutOfUse = LayoutsOutOfUse::Exclude)
{
    const Program& program = context.program;
    BitVector neededTypes(program.types.size());
    BitVector visitedLayouts(program.layouts.size());
    Vector<TypeID> worklist;
    auto addType = [&](TypeID type) {
        if (type && neededTypes.add(type.index()))
            worklist.append(type);
    };
    auto addFieldTypes = [&](LayoutID id) {
        if (!visitedLayouts.add(id.index()) || (layoutsOutOfUse == LayoutsOutOfUse::Exclude && sites.layoutsOutOfUse.get(id.index())))
            return;
        FieldIndex numberOfFields = program.at(id).fields.size();
        for (FieldIndex index = 0; index < numberOfFields; ++index)
            addType(recordedTypeOf(program, fieldAt(program, declarationOf(program, { id, index }))));
    };

    addType(TypeTable::anything);
    addType(TypeTable::number);
    for (size_t index : sites.typesInRunTimePayloads)
        addType(TypeID(index));
    if (typesTestedByCode == TypesTestedByCode::Include) {
        for (size_t index : sites.typesTestedByCode)
            addType(TypeID(index));
    }
    // The tables name the type of a variable in two places: a static field of a class, and a binding that a direct eval sees.
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields)
            addType(program.at(field.variable).declared.type);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                addType(program.at(binding.variable).declared.type);
            else if (binding.layout && binding.field && binding.kind != EvalBindingKind::MappedArgument)
                addType(fieldAt(program, declarationOf(program, { binding.layout, *binding.field })).declared.type);
        }
    }
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (context.receivesShape(id) || layout.kind == LayoutKind::Instance || layout.isEnvironment())
            addFieldTypes(id);
    }
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.contents)
                addType(contents);
        }
        for (LayoutID layout : membersOf(program, type))
            addFieldTypes(layout);
    }
    return neededTypes;
}

static void appendCellsOfClass(const BuiltinClass& builtinClass, Vector<BuiltinCellKey>& cells)
{
    if (builtinClass.constructor)
        cells.append(BuiltinCellKey::object(builtinClass.constructor));
    if (builtinClass.prototype)
        cells.append(BuiltinCellKey::object(builtinClass.prototype));
}

static void sortAndRemoveKeysWithoutCell(const BuiltinTable& builtins, Vector<BuiltinCellKey>& cells)
{
    AOT::sort(cells, { }, &BuiltinCellKey::encoded);
    removeRepeatedElements(cells);
    cells.removeAllMatching([&](BuiltinCellKey key) {
        return !key.hasCell(builtins);
    });
}

// The run time finds a cell by the names on its path, from the nearest holder that start-up found: the holder of a locked member, or an eager cell.
static std::optional<BuiltinCellKey> findFirstReplaceableStep(const PlacementContext& context, BuiltinCellKey key)
{
    const BuiltinTable& builtins = context.program.builtins();
    // The word of a value that the host makes is filled at its first read and never at start: where the member may be replaced, the test of its location answers for the word.
    BuiltinCellKey first = key.kind() == BuiltinCellKey::Kind::Value ? BuiltinCellKey::object(builtins.at(key.builtin()).holder) : key;
    for (BuiltinCellKey step = first; step.builtin(); step = BuiltinCellKey::object(builtins.at(step.builtin()).holder)) {
        if (mayBeReplaced(context.program, context.inputs.flow, step.builtin()))
            return step;
        if (isLockedLocation(builtins, step.builtin()))
            break;
    }
    return std::nullopt;
}

static Vector<BuiltinCellKey> findEagerCells(const PlacementContext& context, std::span<const BuiltinCellKey> cellsInCode)
{
    Vector<BuiltinCellKey> eagerCells;
    for (BuiltinCellKey key : cellsInCode) {
        if (std::optional<BuiltinCellKey> step = findFirstReplaceableStep(context, key))
            eagerCells.append(*step);
    }
    sortAndRemoveKeysWithoutCell(context.program.builtins(), eagerCells);
    return eagerCells;
}

static void numberBuiltinCells(const PlacementContext& context, PlacedSites& sites, const BitVector& typesWithCells, ConstantNumbering& numbering)
{
    const Program& program = context.program;
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinCellKey> cells = WTF::move(sites.builtinCells);
    for (const BitVector* types : { &sites.typesInRunTimePayloads, &sites.typesTestedByCode }) {
        for (size_t index : *types) {
            for (const ClassPart& part : program.at(TypeID(index)).classes)
                appendCellsOfClass(builtins.at(part.builtinClass), cells);
        }
    }
    for (size_t index : typesWithCells) {
        for (BuiltinID id : program.at(TypeID(index)).builtins)
            cells.append(BuiltinCellKey::object(id));
    }
    for (const Layout& layout : program.layouts) {
        if (BuiltinID parent = builtinParentNamedByClassRecord(program, layout))
            cells.append(BuiltinCellKey::object(parent));
    }
    sortAndRemoveKeysWithoutCell(builtins, cells);
    Vector<BuiltinCellKey> eagerCells = findEagerCells(context, cells.span());

    cells.appendVector(eagerCells);
    for (const BuiltinUseRecord& use : context.inputs.flow.builtinUses())
        cells.append(BuiltinCellKey::object(BuiltinID(use.builtin)));
    sortAndRemoveKeysWithoutCell(builtins, cells);

    numbering.eagerBuiltinCells = eagerCells.map([&](BuiltinCellKey key) {
        return static_cast<uint32_t>(std::ranges::lower_bound(cells, key.encoded(), { }, &BuiltinCellKey::encoded) - cells.begin());
    });
    numbering.builtinCells = WTF::move(cells);
}

static void numberKeyCells(const PlacementContext& context, const BodyScansForPlacement& scans, ConstantNumbering& numbering)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    auto uses = collectInParallel<KeyCellUse>(loops, program.functions.size(), [&](unsigned index, Vector<KeyCellUse>& found) {
        if (program.functions[index].hasBody())
            found.appendVector(scans.keyCellUsesOf(FunctionID(index)));
    });
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (isSymbolKind(program.at(PropertyKeyID(index)).kind))
            uses.append(KeyCellUse { .key = PropertyKeyID(index) });
    }

    parallelSort(loops, uses.mutableSpan(), std::less<KeyCellUse> { });
    removeRepeatedElements(uses, [](const KeyCellUse& first, const KeyCellUse& later) {
        return first.key == later.key;
    });
    AOT::sort(uses, { }, [](const KeyCellUse& use) {
        return std::tuple(use.module, use.sourceFunction, use.key);
    });

    numbering.keyCells = uses.map([](const KeyCellUse& use) {
        return use.key;
    });
    numbering.keyCellNumbersPlusOne.fill(0, numberOfKeys);
    for (uint32_t number = 0; number < uses.size(); ++number)
        numbering.keyCellNumbersPlusOne[uses[number].key.index()] = number + 1;
}

static ConstantNumbering numberConstants(const PlacementContext& context, PlacedSites& sites, const BitVector& typesWithCells, const BodyScansForPlacement& scans)
{
    ConstantNumbering numbering;
    numberBuiltinCells(context, sites, typesWithCells, numbering);
    numberKeyCells(context, scans, numbering);
    numbering.bigIntConstants = WTF::move(sites.bigIntConstants);
    for (const RequireEdgeUse& use : sites.requireEdges) {
        if (!numbering.requireEdges.isEmpty() && numbering.requireEdges.last() == use.edge)
            continue;
        numbering.requireEdges.append(use.edge);
        numbering.requireEdgeSpecifiers.append(use.specifier);
    }
    return numbering;
}

// A task is the same rows at any number of threads, and the phase gives out the final ids in the order of the tasks.
static constexpr size_t fieldAccessesPerTask = 256;

static void forEachTaskOfFieldAccesses(const ParallelLoops& loops, std::span<FieldAccess> rows, const Invocable<void(unsigned task, size_t firstRow, std::span<FieldAccess>)> auto& functor)
{
    forEachIndexInParallel(loops, static_cast<unsigned>((rows.size() + fieldAccessesPerTask - 1) / fieldAccessesPerTask), [&](unsigned task) {
        size_t first = task * fieldAccessesPerTask;
        functor(task, first, rows.subspan(first, std::min(fieldAccessesPerTask, rows.size() - first)));
    });
}

struct PlacementInSteps::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(const PlacementInputs& inputs, BuildContext& build, BodyScansForPlacement* scans)
        : inputs(inputs)
        , build(build)
        , scans(scans)
        , sizeClasses(build.options().sizeClassProgression)
    {
    }

    bool hasFailed() const { return build.diagnostics().hasErrors(); }

    void run(ASCIILiteral name, NOESCAPE const ScopedLambda<void()>& step) const
    {
        if (hasFailed())
            return;
        PhaseTimes times(build, PassID::NumberAndEncode);
        step();
        times.didFinish(name);
    }

    // Each chain is a sequence of steps that reads nothing another chain writes.
    void runBeside(std::initializer_list<ScopedLambda<void()>> chains) const
    {
        if (!hasFailed())
            runTasksInParallel(build.parallelLoops(), chains);
    }

    const PlacementInputs& inputs;
    BuildContext& build;
    BodyScansForPlacement* scans;
    std::optional<BodyScansForPlacement> ownScans;
    CellSizeClasses sizeClasses;
    std::optional<PlacementContext> context;
    PlacedSites sites;
    ShapeVariants variants;
    BitVector typesThatOrderShapes;
    LiteralSites literals;
    MethodTableFunctions methodTableFunctions;
    PlacedOrderWords orderWords;
    Vector<Vector<ValueID>> valuesWithOtherWords;
};

PlacementInSteps::PlacementInSteps(const PlacementInputs& inputs, BuildContext& build, BodyScansForPlacement* scans)
    : m_state(makeUnique<State>(inputs, build, scans))
{
}

PlacementInSteps::~PlacementInSteps() = default;

Placement PlacementInSteps::numberWhatRepresentationsAsk(ProgramEditor& editor, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites)
{
    State& state = *m_state;
    const ParallelLoops& loops = state.build.parallelLoops();
    Placement placement;
    placement.m_slots = &state.inputs.slots;
    placement.m_closureLayout = findClosureLayout(editor.program());
    if (!placement.m_closureLayout) {
        Layout closure;
        closure.kind = LayoutKind::Closure;
        placement.m_closureLayout = editor.addLayout(WTF::move(closure));
    }
    const Program& program = editor.program();
    state.context.emplace(PlacementContext { program, state.inputs, state.sizeClasses, state.build, state.build.recordedOptions().useWideReferences(), placement.m_closureLayout });
    const PlacementContext& context = *state.context;
    if (!state.scans) {
        state.scans = &state.ownScans.emplace(program, state.inputs, state.build);
        state.run("scanBodies"_s, [&] {
            Vector<uint32_t> largestFirst = largeFunctions(program, loops);
            forEachIndexInParallel(loops, program.functions.size(), largestFirst.span(), [&](unsigned index) {
                if (program.functions[index].hasBody())
                    state.scans->scan(FunctionID(index));
            });
        });
    }

    state.run("placeGlobalBindings"_s, [&] {
        TypeArena arena(editor.types());
        placement.m_globalBindings = placeGlobalBindings(editor, context, arena, *state.scans);
    });

    PlacedSites& sites = state.sites;
    // A reader of the bodies makes the types that it asks about in the phase that is open, so none is opened or closed beside it.
    auto phase = makeUniqueWithoutFastMallocCheck<TypeInterningPhase>(editor.types(), 0u);
    state.runBeside({
        [&] { readBodiesBesideCollectingSites(); },
        [&] {
            state.runBeside({
                [&] { state.run("collectPlacedSites"_s, [&] { sites = state.scans->takeSites(context); }); },
                [&] { state.run("collectMethodTableFunctions"_s, [&] { state.methodTableFunctions = collectMethodTableFunctions(context); }); },
            });
            if (state.hasFailed())
                return;
            placement.m_fieldAccesses.grow(sites.fieldAccesses.size());
            unsigned firstTask = phase->addTasks(static_cast<unsigned>((sites.fieldAccesses.size() + fieldAccessesPerTask - 1) / fieldAccessesPerTask));
            RELEASE_ASSERT(!firstTask);
            state.runBeside({
                [&] {
                    state.run("createFieldAccesses"_s, [&] {
                        forEachTaskOfFieldAccesses(loops, placement.m_fieldAccesses.mutableSpan(), [&](unsigned task, size_t firstRow, std::span<FieldAccess> rows) {
                            TypeArena arena(*phase, task);
                            for (size_t index = 0; index < rows.size(); ++index)
                                rows[index] = createFieldAccess(context, arena, sites.fieldAccesses[firstRow + index].access);
                        });
                    });
                },
                [&] {
                    state.run("computeTypesNeededAtRunTime"_s, [&] {
                        placement.m_typesNeededAtRunTime = computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Exclude);
                        placement.m_typesWithCells = computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Include);
                        state.typesThatOrderShapes = sites.layoutsOutOfUse.isEmpty() ? placement.m_typesNeededAtRunTime : computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Exclude, LayoutsOutOfUse::Include);
                    });
                },
                [&] { state.run("chooseShapeVariants"_s, [&] { state.variants = chooseShapeVariants(context, sites); }); },
                [&] {
                    state.run("placeGuardBits"_s, [&] { placement.m_guardBits = placeGuardBits(context, sites); });
                    state.run("placeGuardSets"_s, [&] { placeGuardSets(context, sites, placement.m_guardBits); });
                    state.run("placeStatics"_s, [&] { std::tie(placement.m_statics, state.literals) = placeStatics(context, sites, placement.m_guardBits); });
                },
            });
        },
    });
    TypeRemap types = phase->finish(StringRemap(), &loops);
    state.build.destroyLater(WTF::move(phase));
    if (!types.isIdentity()) {
        forEachTaskOfFieldAccesses(loops, placement.m_fieldAccesses.mutableSpan(), [&](unsigned task, size_t, std::span<FieldAccess> rows) {
            if (types.isIdentityFor(task))
                return;
            for (FieldAccess& row : rows) {
                types.apply(row.membersWithField);
                types.apply(row.contents);
            }
        });
    }
    if (state.hasFailed())
        return { };
    return placement;
}

void PlacementInSteps::numberRest(Placement& placement)
{
    State& state = *m_state;
    if (state.hasFailed())
        return;
    const PlacementContext& context = *state.context;
    PlacedSites& sites = state.sites;
    const BitVector& typesNeeded = state.typesThatOrderShapes;

    state.runBeside({
        [&] {
            ShapeTestCandidates candidates;
            ShapeNumbering numbering;
            state.run("collectShapeTestCandidates"_s, [&] { candidates = collectShapeTestCandidates(context, sites, placement.m_fieldAccesses.span(), state.variants, typesNeeded); });
            state.run("numberShapes"_s, [&] { numbering = numberShapes(context, candidates, state.variants); });
            state.run("deriveShapeTests"_s, [&] { placement.m_shapes = deriveShapeTests(context, sites, numbering, state.variants, typesNeeded, placement.m_fieldAccesses); });
        },
        [&] {
            state.run("numberAllocations"_s, [&] { placement.m_allocations = numberAllocations(context, sites); });
            state.run("collectKeyOrders"_s, [&] { std::tie(placement.m_keyOrders, state.orderWords) = collectKeyOrders(context, sites, placement.m_allocations); });
        },
        [&] { state.run("numberConstants"_s, [&] { placement.m_constants = numberConstants(context, sites, placement.m_typesWithCells, *state.scans); }); },
        [&] {
            state.run("numberFunctions"_s, [&] { placement.m_functions = numberFunctions(context, sites); });
            state.run("rankClasses"_s, [&] { placement.m_classRanks = rankClasses(context.program, sites.layoutsOutOfUse); });
        },
    });
    state.run("finishAllocations"_s, [&] { finishAllocations(context, placement.m_shapes, state.orderWords, sites.allocations.span(), placement.m_allocations); });
    state.run("placeMethodTable"_s, [&] { placement.m_methodTable = placeMethodTable(context, sites, placement.m_shapes, placement.m_guardBits, state.methodTableFunctions); });
    placement.m_layoutsOutOfUse = sites.layoutsOutOfUse;
    placement.m_bodiesWithoutCodeWhenPlaced = sites.bodiesWithoutCode;
    placement.m_numberOfRegExpRecords = static_cast<unsigned>(state.inputs.regExpRequests.all().size());
}

void PlacementInSteps::prepareToResolveFieldAccesses()
{
    m_state->valuesWithOtherWords = Vector<Vector<ValueID>>(m_state->context->program.functions.size());
}

void PlacementInSteps::resolveFieldAccessesOf(const Placement& placement, FunctionID id, Function& function)
{
    resolveFieldAccesses(placement, function, m_state->valuesWithOtherWords[id.index()]);
}

void PlacementInSteps::resolveSites(ProgramEditor& editor, Placement& placement)
{
    State& state = *m_state;
    state.run("resolveSites"_s, [&] {
        const Program& program = state.context->program;
        if (state.valuesWithOtherWords.isEmpty()) {
            prepareToResolveFieldAccesses();
            forEachIndexInParallel(state.build.parallelLoops(), program.functions.size(), [&](unsigned index) {
                if (program.functions[index].hasBody())
                    resolveFieldAccessesOf(placement, FunctionID(index), editor.atForPlacedWords(FunctionID(index)));
            });
        }
        placement.m_resolvedSites = resolveOtherWords(editor, *state.context, { state.sites, state.literals, state.orderWords, placement }, state.valuesWithOtherWords.span());
    });
    if (state.hasFailed()) {
        placement = { };
        return;
    }
    countStatistics(placement, state.sites, state.build.statistics());
    state.build.destroyLater(WTF::move(state.sites));
    state.build.destroyLater(WTF::move(state.valuesWithOtherWords));
    state.scans->destroyLater();
}

Placement numberAndEncode(ProgramEditor& editor, const PlacementInputs& inputs, BuildContext& build, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites, BodyScansForPlacement* scans)
{
    PlacementInSteps steps(inputs, build, scans);
    Placement placement = steps.numberWhatRepresentationsAsk(editor, readBodiesBesideCollectingSites);
    steps.numberRest(placement);
    steps.resolveSites(editor, placement);
    return placement;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
