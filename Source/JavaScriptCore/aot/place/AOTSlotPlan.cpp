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
#include <algorithm>
#include <iterator>
#include <limits>

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

// Reaching unknown JavaScript clears every proof of a class. That alone is no witness of a mutation, so such a class counts only for the stores under unnamed keys that flow saw.
bool someAllocationClassMay(const Program& program, const FlowResults& flow, LayoutID layout, OptionSet<ObjectMutation> mutations)
{
    return std::ranges::any_of(program.at(layout).flow.allocationClasses, [&](AllocationClassID allocationClass) {
        const AllocationClass& record = flow.at(allocationClass);
        if (record.impossibleMutations.contains(ObjectMutation::ReachUnknownJavaScript))
            return !record.impossibleMutations.containsAll(mutations);
        return record.isStoredUnderUnnamedKeys && mutations.contains(ObjectMutation::AddProperty);
    });
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

static LayoutStorage storageFor(const Layout& layout)
{
    bool mayBeAllocated = layout.flow.allocation != AllocationState::NeverAllocated;
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
    return Vector<LayoutStorage>(program.layouts.size(), [&](size_t index) {
        const Layout& layout = program.layouts[index];
        LayoutStorage storage = storageFor(layout);
        if (storage == LayoutStorage::CompiledObject && layout.fields.size() > build.limit(LimitID::FieldsInKeyOrder))
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index), .limit = LimitID::FieldsInKeyOrder, .count = static_cast<uint32_t>(layout.fields.size()) });
        return storage;
    });
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

static void countStatistics(std::span<const PlannedLayout> layouts, Statistics& statistics)
{
    uint64_t slots = 0;
    uint64_t fieldsOutOfLine = 0;
    uint64_t hiddenWords = 0;
    uint64_t deferredClosureSlots = 0;
    for (const PlannedLayout& layout : layouts) {
        for (const PlacedSlot& slot : layout.slots.span().first(layout.numberOfFields())) {
            if (!slot.hasStorage())
                continue;
            ++slots;
            fieldsOutOfLine += slot.isOutOfLine();
            deferredClosureSlots += slot.representation == SlotRepresentation::DeferredClosure;
        }
        hiddenWords += layout.hasExtension();
        hiddenWords += layout.hasExtraPropertiesWord();
    }
    statistics.count(StatisticID::Slots, slots);
    statistics.count(StatisticID::FieldsOutOfLine, fieldsOutOfLine);
    statistics.count(StatisticID::HiddenWordsPlaced, hiddenWords);
    statistics.count(StatisticID::DeferredClosureSlots, deferredClosureSlots);
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

    SlotUses uses = collectSlotUses(context);
    bool dumpsPlan = build.options().logging.dumpedPass == PassID::PlanSlots;
    if (dumpsPlan) {
        dumpFillEstimates(WTF::dataFile(), program, uses);
        dumpClosureObservers(WTF::dataFile(), program, inputs.flow, build);
    }
    shareUsesOfInheritedRows(inheritedRows, uses);
    if (!finishStep("collectSlotUses"_s))
        return { };
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

    Vector<PlannedVariable> variables = planVariables(context, uses);
    if (!finishStep("planVariables"_s))
        return { };
    Vector<ClosureForm> closureForms = chooseClosureForms(context);
    if (!finishStep("chooseClosureForms"_s))
        return { };
    Vector<ClassCreationPlan> classCreation = planClassCreation(context);
    if (!finishStep("planClassCreation"_s))
        return { };
    BitVector keysWithColumn = chooseKeysWithColumn(context, uses);
    if (!finishStep("chooseKeysWithColumn"_s))
        return { };

    Vector<PlannedLayout> layouts = createPlannedLayouts(context, WTF::move(packed));
    countStatistics(layouts.span(), build.statistics());
    times.didFinish("createPlannedLayouts"_s);
    SlotPlan plan(SlotPlanParts {
        .layouts = WTF::move(layouts),
        .variables = WTF::move(variables),
        .closureForms = WTF::move(closureForms),
        .classCreation = WTF::move(classCreation),
        .keysWithColumn = WTF::move(keysWithColumn),
    });
    if (dumpsPlan)
        plan.dump(WTF::dataFile(), program);
    return plan;
}

SlotPlan::SlotPlan(SlotPlanParts&& parts)
    : m_layouts(WTF::move(parts.layouts))
    , m_variables(WTF::move(parts.variables))
    , m_closureForms(WTF::move(parts.closureForms))
    , m_classCreation(WTF::move(parts.classCreation))
    , m_keysWithColumn(WTF::move(parts.keysWithColumn))
{
}

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

SlotAgreement SlotPlan::agreementOf(const Program& program, PropertyKeyID key, LayoutSetID receivers) const
{
    if (!receivers)
        return { };
    return agreementOf(program, key, program.at(receivers).members.span());
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
