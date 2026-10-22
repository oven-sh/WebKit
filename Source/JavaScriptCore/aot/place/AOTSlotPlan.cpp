/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTClosureObservers.h"
#include "AOTFlowResults.h"
#include "AOTPhaseTimes.h"
#include "AOTSlotPlanSteps.h"
#include "AOTThreads.h"
#include "IndexingHeader.h"
#include "JSArray.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <limits>
#include <wtf/HashFunctions.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, Architecture::ARM64) <= std::numeric_limits<uint16_t>::max());
static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, Architecture::X86_64) <= std::numeric_limits<uint16_t>::max());

SlotRows::SlotRows(const Program& program)
{
    m_firstRow.reserveInitialCapacity(program.layouts.size() + 1);
    m_firstRow.append(0u);
    uint64_t numberOfRows = 0;
    for (const Layout& layout : program.layouts) {
        numberOfRows += layout.fields.size() + numberOfHiddenWords;
        RELEASE_ASSERT(numberOfRows <= std::numeric_limits<SlotRow>::max());
        m_firstRow.append(static_cast<SlotRow>(numberOfRows));
    }
}

LayoutID SlotRows::layoutOf(SlotRow row) const
{
    ASSERT(row < count());
    auto firstRows = m_firstRow.span();
    return LayoutID(static_cast<uint32_t>(std::ranges::upper_bound(firstRows, row) - firstRows.begin() - 1));
}

bool everyAllocationClassCannot(const Program& program, const FlowResults& flow, LayoutID layout, OptionSet<ObjectMutation> mutations)
{
    const Vector<AllocationClassID>& classes = program.at(layout).flow.allocationClasses;
    return !classes.isEmpty() && std::ranges::all_of(classes, [&](AllocationClassID allocationClass) {
        return flow.at(allocationClass).impossibleMutations.containsAll(mutations);
    });
}

// An object keeps its shape whatever happens to it, so it has to be ready for all that is not proven impossible. Reaching unknown JavaScript clears every proof of a class,
// and of a layout without classes nothing is proven.
bool someAllocationClassMay(const Program& program, const FlowResults& flow, LayoutID layout, OptionSet<ObjectMutation> mutations)
{
    if (program.at(layout).flow.allocation == AllocationState::NeverAllocated)
        return false;
    return !everyAllocationClassCannot(program, flow, layout, mutations);
}

bool keysAreNeverEnumerated(const Program& program, const FlowResults& flow, LayoutID layout)
{
    const Vector<AllocationClassID>& classes = program.at(layout).flow.allocationClasses;
    return !classes.isEmpty() && std::ranges::all_of(classes, [&](AllocationClassID allocationClass) {
        return flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
    });
}

bool mayHaveUnorderedShape(const Program& program, const FlowResults& flow, LayoutID layout)
{
    return std::ranges::any_of(program.at(layout).flow.allocationClasses, [&](AllocationClassID allocationClass) {
        return flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
    });
}

bool declaresPrivateName(const Layout& layout)
{
    constexpr OptionSet<FieldFlags> privateNameFlags { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    return std::ranges::any_of(layout.fields, [&](const Field& field) {
        return field.flags.containsAny(privateNameFlags) && !field.flags.contains(FieldFlags::IsInherited);
    });
}

static LayoutStorage storageFor(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    // The constructor of a built-in class makes the instances of the classes below it: cells of that class, which have no slots.
    bool mayBeAllocated = layout.flow.allocation != AllocationState::NeverAllocated && !builtinBaseClassOf(program, id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        // The generic operations on a private name find it in the record of the class that declares it, whatever object they are given.
        return mayBeAllocated || declaresPrivateName(layout) ? LayoutStorage::CompiledObject : LayoutStorage::None;
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return mayBeAllocated ? LayoutStorage::Environment : LayoutStorage::None;
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return LayoutStorage::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<LayoutStorage> chooseLayoutStorage(const Program& program, const SlotPlanInputs&, BuildContext& build)
{
    Vector<LayoutStorage> storageOfLayouts(program.layouts.size());
    forEachIndexInParallel(build.parallelLoops(), storageOfLayouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        LayoutStorage storage = storageFor(program, LayoutID(index));
        if (storage == LayoutStorage::CompiledObject && layout.fields.size() > build.limit(LimitID::FieldsInKeyOrder))
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index), .limit = LimitID::FieldsInKeyOrder, .count = static_cast<uint32_t>(layout.fields.size()) });
        storageOfLayouts[index] = storage;
    });
    return storageOfLayouts;
}

BitVector chooseKeysWithColumn(const SlotPlanContext& context, const SlotUses& uses)
{
    const Vector<SiteWeight>& weights = uses.keyWeights;
    Vector<uint32_t> order(weights.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    parallelSort(context.build.parallelLoops(), order.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return weights[a] != weights[b] ? weights[a] > weights[b] : a < b;
    });

    auto keysByWeight = order.span();
    auto numberOfColumns = static_cast<size_t>(std::min<uint64_t>(keysByWeight.size(), context.build.limit(LimitID::LookupTableNames)));
    BitVector keysWithColumn(keysByWeight.size());
    for (uint32_t key : keysByWeight.first(numberOfColumns))
        keysWithColumn.quickSet(key);

    auto usedKeysWithoutColumn = std::ranges::count_if(keysByWeight.subspan(numberOfColumns), [&](uint32_t key) {
        return weights[key] > 0;
    });
    context.build.statistics().count(StatisticID::KeysWithoutColumn, static_cast<uint64_t>(usedKeysWithoutColumn));
    return keysWithColumn;
}

static void numberRecordFields(PlannedLayout& planned)
{
    uint16_t next = 0;
    for (PlacedSlot& slot : planned.slots.mutableSpan().first(planned.numberOfFields())) {
        if (slot.hasStorage())
            slot.indexInLayoutRecord = next++;
    }
    planned.numberOfRecordFields = next;
}

static Vector<PlannedLayout> createPlannedLayouts(const SlotPlanContext& context, PackedLayouts&& packed)
{
    Vector<PlannedLayout> layouts(context.program.layouts.size());
    forEachIndexInParallel(context.build.parallelLoops(), layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        PlannedLayout& planned = layouts[index];
        planned.storage = context.layoutStorage[index];
        if (!planned.hasSlots())
            return;

        planned.holdsWideReferences = holdsWideReferences(context, id);
        planned.size = packed.sizes[index];
        planned.extensionSize = packed.extensionSizes[index];
        planned.isEscapedOffset = packed.isEscapedOffsets[index];
        planned.bytesKeptZero = WTF::move(packed.bytesKeptZero[index]);
        ASSERT(std::ranges::is_sorted(planned.bytesKeptZero));
        planned.slots = packed.slots.subvector(context.rows.firstRowOf(id), context.rows.numberOfRowsOf(id));
        if (planned.storage != LayoutStorage::CompiledObject)
            return;

        planned.keysAreNeverEnumerated = keysAreNeverEnumerated(context.program, context.inputs.flow, id);
        numberRecordFields(planned);
    });
    return layouts;
}

// One cell may not take more room than the two cells that it replaces, and it lies in a block: the collector finds the outer object from the inner one by the size of the cells of the block.
static bool fitInOneCell(const SlotPlanContext& context, uint32_t sizeOfOuter, uint32_t sizeOfInner)
{
    size_t together = context.sizeClasses.cellSizeFor(sizeOfOuter + sizeOfInner);
    return together <= MarkedSpace::largeCutoff && together <= context.sizeClasses.cellSizeFor(sizeOfOuter) + context.sizeClasses.cellSizeFor(sizeOfInner);
}

// The cell of the array, the indexing header, the elements. The capacity of a literal is odd, so the whole is a number of atoms.
static uint32_t sizeOfInnerArray(uint32_t capacity)
{
    return roundUpToMultipleOf<MarkedBlock::atomSize>(sizeof(JSArray) + sizeof(IndexingHeader) + capacity * sizeof(EncodedJSValue));
}

static void placeInnerObjects(const SlotPlanContext& context, Vector<PlannedLayout>& layouts)
{
    uint64_t numberOfOuterLayouts = 0;
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        PlannedLayout& outer = layouts[index];
        for (const InnerObject& inner : context.program.layouts[index].innerObjects) {
            if (outer.storage != LayoutStorage::CompiledObject || outer.mayBeInnerObject)
                continue;
            if (inner.layout && (layouts[inner.layout.index()].storage != LayoutStorage::CompiledObject || !context.program.at(inner.layout).innerObjects.isEmpty()))
                continue;
            uint32_t size = inner.layout ? layouts[inner.layout.index()].size : sizeOfInnerArray(inner.arrayCapacity);
            const PlacedSlot& slot = outer.slots[inner.field];
            SlotValueKind valueKind = traitsOf(slot.representation).valueKind;
            if (slot.storage != SlotStorage::Inline || (valueKind != SlotValueKind::Cell && valueKind != SlotValueKind::Boxed))
                continue;
            static_assert(MarkedBlock::atomSize == 16);
            ASSERT(!(outer.size % MarkedBlock::atomSize) && !(size % MarkedBlock::atomSize));
            uint32_t offset = outer.innerObjects.isEmpty() ? outer.size : outer.sizeWithInnerObjects;
            if (!fitInOneCell(context, offset, size))
                continue;
            outer.innerObjects.append({ inner.field, inner.layout, inner.arrayCapacity, offset, inner.isAlwaysInPlace });
            outer.sizeWithInnerObjects = offset + size;
            if (inner.layout)
                layouts[inner.layout.index()].mayBeInnerObject = true;
        }
        numberOfOuterLayouts += !outer.innerObjects.isEmpty();
    }
    context.build.statistics().count(StatisticID::LayoutsWithInnerObjects, numberOfOuterLayouts);
}

static uint32_t endOfBytesInUse(const PlannedLayout& layout)
{
    uint32_t end = offsetOfFirstField;
    for (const PlacedSlot& slot : layout.slots) {
        if (slot.storage == SlotStorage::Inline)
            end = std::max(end, slot.end());
    }
    for (const ReservedBytes& reserved : layout.bytesKeptZero)
        end = std::max(end, reserved.offset + reserved.width);
    if (layout.isEscapedOffset)
        end = std::max(end, *layout.isEscapedOffset + 1);
    return end;
}

// The code is written for the room already, so there is no refusing here. The packer is untouched: the room lies behind what it has placed, in the padding first.
static void placeFlattenedObjects(const SlotPlanContext& context, Vector<PlannedLayout>& layouts)
{
    constexpr uint32_t alignmentOfSlots = sizeof(EncodedJSValue);
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        PlannedLayout& outer = layouts[index];
        if (context.program.layouts[index].flattenedObjects.isEmpty() || outer.storage != LayoutStorage::CompiledObject)
            continue;
        uint32_t end = endOfBytesInUse(outer);
        for (const FlattenedObject& flattened : context.program.layouts[index].flattenedObjects) {
            const PlannedLayout& inner = layouts[flattened.layout.index()];
            RELEASE_ASSERT(inner.storage == LayoutStorage::CompiledObject && !inner.hasExtension() && inner.flattenedObjects.isEmpty() && context.program.at(flattened.layout).flattenedObjects.isEmpty());
            uint32_t begin = roundUpToMultipleOf<alignmentOfSlots>(end);
            uint32_t endInOwnCell = roundUpToMultipleOf<alignmentOfSlots>(endOfBytesInUse(inner));
            outer.flattenedObjects.append({ flattened.field, flattened.layout, begin - offsetOfFirstField, offsetOfFirstField, endInOwnCell });
            end = outer.flattenedObjects.last().end();
        }
        outer.size = roundUpToMultipleOf<MarkedBlock::atomSize>(end);
    }
}

static void countStatistics(const ParallelLoops& loops, std::span<const PlannedLayout> layouts, Statistics& statistics)
{
    std::atomic<uint64_t> slots { 0 };
    std::atomic<uint64_t> fieldsOutOfLine { 0 };
    std::atomic<uint64_t> hiddenWords { 0 };
    std::atomic<uint64_t> deferredClosureSlots { 0 };
    forEachIndexInParallel(loops, layouts.size(), [&](unsigned index) {
        const PlannedLayout& layout = layouts[index];
        if (layout.slots.isEmpty())
            return;
        uint64_t slotsOfLayout = 0;
        uint64_t fieldsOutOfLineOfLayout = 0;
        uint64_t deferredClosureSlotsOfLayout = 0;
        for (const PlacedSlot& slot : layout.slots.span().first(layout.numberOfFields())) {
            if (!slot.hasStorage())
                continue;
            ++slotsOfLayout;
            fieldsOutOfLineOfLayout += slot.isOutOfLine();
            deferredClosureSlotsOfLayout += slot.representation == SlotRepresentation::DeferredClosure;
        }
        slots.fetch_add(slotsOfLayout, std::memory_order_relaxed);
        fieldsOutOfLine.fetch_add(fieldsOutOfLineOfLayout, std::memory_order_relaxed);
        hiddenWords.fetch_add(layout.hasExtension() + layout.hasExtraPropertiesWord(), std::memory_order_relaxed);
        deferredClosureSlots.fetch_add(deferredClosureSlotsOfLayout, std::memory_order_relaxed);
    });
    statistics.count(StatisticID::Slots, slots.load(std::memory_order_relaxed));
    statistics.count(StatisticID::FieldsOutOfLine, fieldsOutOfLine.load(std::memory_order_relaxed));
    statistics.count(StatisticID::HiddenWordsPlaced, hiddenWords.load(std::memory_order_relaxed));
    statistics.count(StatisticID::DeferredClosureSlots, deferredClosureSlots.load(std::memory_order_relaxed));
}

// Each step needs what the step before it made. Nothing else in the plan depends on where the fields lie.
static Vector<PlannedLayout> planLayouts(const SlotPlanContext& context, const SlotUses& uses, PhaseTimes& times)
{
    auto finishStep = [&](ASCIILiteral name) {
        times.didFinish(name);
        return !context.build.diagnostics().hasErrors();
    };

    JoinedRows joinedRows(context, uses);
    if (!finishStep("joinRows"_s))
        return { };
    Vector<SlotClaim> claims = chooseSlotRepresentations(context, uses, joinedRows);
    if (!finishStep("chooseSlotRepresentations"_s))
        return { };

    PackedLayouts packed = packObjects(context, uses, joinedRows, claims.span());
    if (!finishStep("packObjects"_s))
        return { };
    packEnvironments(context, uses, claims.span(), packed);
    if (!finishStep("packEnvironments"_s))
        return { };
    widenSlotsIntoPadding(context, uses, claims.span(), WidenedRepresentations::Numbers, packed);
    if (!finishStep("widenSlotsIntoPadding(Numbers)"_s))
        return { };
    widenSlotsIntoPadding(context, uses, claims.span(), WidenedRepresentations::References, packed);
    if (!finishStep("widenSlotsIntoPadding(References)"_s))
        return { };
    reserveEmptySlotsInPadding(context, uses, packed);
    if (!finishStep("reserveEmptySlotsInPadding"_s))
        return { };

    Vector<PlannedLayout> layouts = createPlannedLayouts(context, WTF::move(packed));
    placeFlattenedObjects(context, layouts);
    placeInnerObjects(context, layouts);
    countStatistics(context.build.parallelLoops(), layouts.span(), context.build.statistics());
    times.didFinish("createPlannedLayouts"_s);
    return layouts;
}

// By the estimate. What a program makes for each event is below it, what its hot loops and recursive functions make far above it.
// One loop alone is far below it too, and may fill an array with any number of objects: what is made in a loop is not made seldom, whatever the estimate.
static constexpr double maxCreationsOfLayoutMadeSeldom = 1e9;

// The holders of a row are the objects of the layout and of every layout that inherits the row or hands it down.
// The objects of a tagged family are the nodes of a tree: there are many of them, whatever the estimate says of a constructor that is called through a variable.
static BitVector findLayoutsMadeSeldom(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    BitVector layouts(program.layouts.size());
    if (context.build.recordedOptions().has(TestingSwitch::NoLayoutIsMadeSeldom))
        return layouts;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!context.isCompiledObject(id))
            continue;
        double creations = uses.layouts[index].creationHeat;
        bool isMadeInLoop = uses.layouts[index].isMadeInLoop;
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(id); ++field) {
            const SlotRowUse& row = uses.rows[context.rows.rowOf(id, field)];
            creations = std::max(creations, row.holderHeat);
            isMadeInLoop |= row.hasHoldersMadeInLoop;
        }
        if (creations < maxCreationsOfLayoutMadeSeldom && !isMadeInLoop)
            layouts.quickSet(index);
    }
    for (const TaggedFamily& family : program.taggedFamilies) {
        layouts.quickClear(family.maker.index());
        for (const TaggedLayout& member : family.members)
            layouts.quickClear(member.layout.index());
    }
    return layouts;
}

SlotPlan planSlots(const Program& program, const SlotPlanInputs& inputs, BuildContext& build)
{
    PhaseTimes times(build, PassID::PlanSlots);
    auto finishStep = [&](ASCIILiteral name) {
        times.didFinish(name);
        return !build.diagnostics().hasErrors();
    };

    CellSizeClasses sizeClasses(build.options().sizeClassProgression);
    SlotRows rows(program);
    Vector<LayoutStorage> storage = chooseLayoutStorage(program, inputs, build);
    if (!finishStep("chooseLayoutStorage"_s))
        return { };
    InheritedRows inheritedRows(program, rows, storage.span());
    SlotPlanContext context { program, inputs, build, sizeClasses, rows, storage.span(), build.recordedOptions().useWideReferences(), inheritedRows };

    for (const Layout& layout : program.layouts) {
        for (const FlattenedObject& flattened : layout.flattenedObjects)
            context.layoutsFlattenedInOthers.set(flattened.layout.index());
    }

    SlotUses uses = collectSlotUses(context);
    bool dumpsPlan = build.options().logging.dumpedPass == PassID::PlanSlots;
    if (dumpsPlan) {
        dumpFillEstimates(WTF::dataFile(), program, uses);
        dumpClosureObservers(WTF::dataFile(), program, inputs.flow, build);
    }
    shareUsesOfInheritedRows(inheritedRows, uses);
    context.layoutsMadeSeldom = findLayoutsMadeSeldom(context, uses);
    if (!finishStep("collectSlotUses"_s))
        return { };

    auto timed = [&](ASCIILiteral name, const auto& step) {
        PhaseTimes timesOfStep(build, PassID::PlanSlots);
        auto result = step();
        timesOfStep.didFinish(name);
        return result;
    };
    SlotPlanParts parts;
    runTasksInParallel(build.parallelLoops(), {
        [&] { parts.layouts = planLayouts(context, uses, times); },
        [&] { parts.variables = timed("planVariables"_s, [&] { return planVariables(context, uses); }); },
        [&] { parts.closureForms = timed("chooseClosureForms"_s, [&] { return chooseClosureForms(context, uses); }); },
        [&] { parts.classCreation = timed("planClassCreation"_s, [&] { return planClassCreation(context); }); },
        [&] { parts.keysWithColumn = timed("chooseKeysWithColumn"_s, [&] { return chooseKeysWithColumn(context, uses); }); },
    });
    if (build.diagnostics().hasErrors())
        return { };
    parts.formsExpectedOfNames = FormsExpectedOfNames(program, parts.layouts.span(), inputs.heat);

    SlotPlan plan(WTF::move(parts));
    if (dumpsPlan)
        plan.dump(WTF::dataFile(), program);
    return plan;
}

class SlotAgreementMemo {
    WTF_MAKE_NONCOPYABLE(SlotAgreementMemo);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotAgreementMemo);
public:
    SlotAgreementMemo() = default;

    const SlotAgreement& ensure(PropertyKeyID key, LayoutSetID receivers, std::span<const LayoutID> members, NOESCAPE const Invocable<SlotAgreement()> auto& compute) LIFETIME_BOUND
    {
        bool isFoundByMembers = TypeTable::isProvisional(receivers);
        uint32_t hash = isFoundByMembers ? hashOf(key, members) : pairIntHash(key.index(), receivers.index());
        auto isWanted = [&](const Entry& entry) {
            if (entry.key != key)
                return false;
            return isFoundByMembers ? !entry.receivers && std::ranges::equal(entry.members, members) : entry.receivers == receivers;
        };

        Shard& shard = m_shards[hash >> (32 - shardBits)];
        {
            Locker locker { shard.lock };
            if (const Entry* known = shard.find(hash, isWanted))
                return known->agreement;
        }
        Entry entry { key, { }, { }, compute() };
        if (isFoundByMembers)
            entry.members = Vector<LayoutID>(members);
        else
            entry.receivers = receivers;

        Locker locker { shard.lock };
        if (const Entry* known = shard.find(hash, isWanted))
            return known->agreement;
        shard.index.add(hash, shard.entries.size());
        shard.entries.append(WTF::move(entry));
        return shard.entries.last().agreement;
    }

private:
    static constexpr unsigned shardBits = 6;

    // An id that a thread made for itself names another set in the next phase, so such a set is found by its members.
    struct Entry {
        PropertyKeyID key;
        LayoutSetID receivers;
        Vector<LayoutID> members;
        SlotAgreement agreement;
    };

    struct Shard {
        const Entry* find(uint32_t hash, NOESCAPE const Invocable<bool(const Entry&)> auto& isWanted) const
        {
            std::optional<uint32_t> position = index.find(hash, [&](uint32_t candidate) {
                return isWanted(entries[candidate]);
            });
            return position ? &entries[*position] : nullptr;
        }

        Lock lock;
        SegmentedVector<Entry, 16> entries;
        HashedIndex index;
    };

    static uint32_t hashOf(PropertyKeyID key, std::span<const LayoutID> members)
    {
        uint32_t hash = pairIntHash(key.index(), static_cast<unsigned>(members.size()));
        for (LayoutID member : members)
            hash = pairIntHash(hash, member.index());
        return hash;
    }

    std::array<Shard, 1u << shardBits> m_shards;
};

SlotPlan::SlotPlan()
    : m_agreements(makeUnique<SlotAgreementMemo>())
{
}

SlotPlan::SlotPlan(SlotPlanParts&& parts)
    : m_layouts(WTF::move(parts.layouts))
    , m_variables(WTF::move(parts.variables))
    , m_closureForms(WTF::move(parts.closureForms))
    , m_classCreation(WTF::move(parts.classCreation))
    , m_keysWithColumn(WTF::move(parts.keysWithColumn))
    , m_formsExpectedOfNames(WTF::move(parts.formsExpectedOfNames))
    , m_agreements(makeUnique<SlotAgreementMemo>())
{
}

SlotPlan::SlotPlan(SlotPlan&&) = default;
SlotPlan& SlotPlan::operator=(SlotPlan&&) = default;
SlotPlan::~SlotPlan() = default;

bool PlannedLayout::keepsZero(uint32_t offset, uint32_t width) const
{
    auto reserved = bytesKeptZero.span();
    auto next = std::ranges::upper_bound(reserved, offset, { }, &ReservedBytes::offset);
    if (next == reserved.begin())
        return false;
    const ReservedBytes& entry = *std::prev(next);
    return width <= entry.width && offset - entry.offset <= entry.width - width;
}

SlotAgreement SlotPlan::agreementOf(const Program& program, PropertyKeyID key, std::span<const LayoutID> members) const
{
    SlotAgreement agreement;
    for (LayoutID member : members) {
        if (program.at(member).flow.allocation == AllocationState::NeverAllocated)
            continue;
        std::optional<uint32_t> field;
        if (storageOf(member) == LayoutStorage::CompiledObject)
            field = program.at(member).findField(key);
        if (!field || !slot(member, *field).hasStorage()) {
            agreement.membersWithoutSlot.append(member);
            continue;
        }

        const PlacedSlot& placed = slot(member, *field);
        const PlacedSlot& word = placed.needsExtension() ? extensionWord(member) : m_unplannedSlot;
        auto group = std::ranges::find_if(agreement.groups, [&](const SlotGroup& candidate) {
            return candidate.slot.hasLocationOf(placed) && candidate.extensionWord.hasLocationOf(word);
        });
        if (group != agreement.groups.end())
            group->members.append(member);
        else
            agreement.groups.append(SlotGroup { placed, word, { member } });
    }

    agreement.differsInOffsetAlone = agreement.groups.size() > 1 && std::ranges::all_of(agreement.groups, [&](const SlotGroup& group) {
        return group.slot.storage == SlotStorage::Inline && group.slot.representation == agreement.groups[0].slot.representation && group.slot.representation != SlotRepresentation::DeferredClosure;
    });

    if (!agreement.hasOneLocation() || agreement.coversEveryMember())
        return agreement;
    const PlacedSlot& shared = agreement.groups[0].slot;
    agreement.membersWithoutSlotKeepBytesZero = shared.storage == SlotStorage::Inline
        && representsAbsentAsZero(shared.representation)
        && std::ranges::all_of(agreement.membersWithoutSlot, [&](LayoutID member) {
            return storageOf(member) == LayoutStorage::CompiledObject && layout(member).keepsZero(shared.offset, shared.width());
        });
    return agreement;
}

SlotGroup SlotAgreement::groupOfAllOffsets() const
{
    RELEASE_ASSERT(differsInOffsetAlone);
    SlotGroup all = *std::ranges::min_element(groups, { }, [](const SlotGroup& group) {
        return group.slot.offset;
    });
    all.members.clear();
    for (const SlotGroup& group : groups)
        all.members.append(group.members.span());
    std::ranges::sort(all.members);
    return all;
}

const SlotAgreement& SlotPlan::agreementOf(const Program& program, PropertyKeyID key, LayoutSetID receivers) const
{
    if (!receivers)
        return m_agreementOfNoReceivers;
    std::span<const LayoutID> members = program.at(receivers).members.span();
    return m_agreements->ensure(key, receivers, members, [&] {
        return agreementOf(program, key, members);
    });
}

MemberCreation SlotPlan::creationOf(const Program& program, LayoutID classLayout, FunctionID member) const
{
    const Vector<ClassMember>& members = program.at(classLayout).classFlow.members;
    const Vector<MemberCreation>& creations = classCreation(classLayout).members;
    MemberCreation creation = MemberCreation::AtDefinition;
    for (size_t index = 0; index < members.size(); ++index) {
        if (members[index].function != member)
            continue;
        if (index >= creations.size() || creations[index] == MemberCreation::AtDefinition)
            return MemberCreation::AtDefinition;
        creation = MemberCreation::ByClassRecord;
    }
    return creation;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
