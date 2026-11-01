/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImagesBelow.h"
#include "AOTLayoutData.h"
#include "AOTPhaseTimes.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRepresentation.h"
#include "AOTSharedMemo.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>
#include <wtf/UnionFind.h>

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
    // Its slots are given: readLayoutsBelow().
    if (layout.has(LayoutFlags::IsOfImageBelow))
        return LayoutStorage::None;
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
        return !weights[a].isSamePredictionAs(weights[b]) ? (weights[a] > weights[b]).predicted() : a < b;
    });

    auto keysByWeight = order.span();
    auto numberOfColumns = static_cast<size_t>(std::min<uint64_t>(keysByWeight.size(), context.build.limit(LimitID::LookupTableNames)));
    BitVector keysWithColumn(keysByWeight.size());
    if (const ImagesBelow* below = context.build.imagesBelow()) {
        // The columns are given out: a name has the one of its number there, or none.
        for (uint32_t key : keysByWeight) {
            std::optional<uint32_t> number = weights[key].predicted() > 0 ? numberInImagesBelow(context.program, PropertyKeyID(key), context.program.at(PropertyKeyID(key)).text, *below) : std::nullopt;
            if (number && RowDisplacementTable::hasColumn(*number))
                keysWithColumn.quickSet(key);
        }
        return keysWithColumn;
    }
    for (uint32_t key : keysByWeight.first(numberOfColumns))
        keysWithColumn.quickSet(key);

    auto usedKeysWithoutColumn = std::ranges::count_if(keysByWeight.subspan(numberOfColumns), [&](uint32_t key) {
        return weights[key].predicted() > 0;
    });
    context.build.statistics().count(StatisticID::KeysWithoutColumn, static_cast<uint64_t>(usedKeysWithoutColumn));
    return keysWithColumn;
}

static Vector<PlannedLayout> createPlannedLayouts(const SlotPlanContext& context, PackedLayouts&& packed)
{
    Statistics& statistics = context.build.statistics();
    Vector<PlannedLayout> layouts(context.program.layouts.size());
    forEachIndexInParallel(context.build.parallelLoops(), layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        PlannedLayout& planned = layouts[index];
        planned.storage = context.layoutStorage[index];
        if (!planned.hasSlots())
            return;

        bool isCompiledObject = planned.storage == LayoutStorage::CompiledObject;
        planned.isRecordAlone = isCompiledObject && builtinBaseClassOf(context.program, id) && !context.build.isSabotaged(Sabotage::ClassesBelowBuiltinsHaveSlots);
        planned.holdsWideReferences = holdsWideReferences(context, id);
        planned.keysAreNeverEnumerated = isCompiledObject && keysAreNeverEnumerated(context.program, context.inputs.flow, id);
        planned.size = packed.sizes[index];
        planned.extensionSize = packed.extensionSizes[index];
        planned.isEscapedOffset = packed.isEscapedOffsets[index];
        planned.bytesKeptZero = WTF::move(packed.bytesKeptZero[index]);
        ASSERT(std::ranges::is_sorted(planned.bytesKeptZero));
        planned.slots = packed.slots.subvector(context.rows.firstRowOf(id), context.rows.numberOfRowsOf(id));

        uint64_t slots = 0;
        uint64_t fieldsOutOfLine = 0;
        uint64_t deferredClosureSlots = 0;
        for (PlacedSlot& slot : planned.slots.mutableSpan().first(planned.numberOfFields())) {
            if (!slot.hasStorage())
                continue;
            if (isCompiledObject)
                slot.indexInLayoutRecord = planned.numberOfRecordFields++;
            ++slots;
            fieldsOutOfLine += slot.isOutOfLine();
            deferredClosureSlots += slot.representation == SlotRepresentation::DeferredClosure;
        }
        statistics.count(StatisticID::Slots, slots);
        statistics.count(StatisticID::FieldsOutOfLine, fieldsOutOfLine);
        statistics.count(StatisticID::HiddenWordsPlaced, planned.hasExtension() + planned.hasExtraPropertiesWord());
        statistics.count(StatisticID::DeferredClosureSlots, deferredClosureSlots);
    });
    return layouts;
}

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
    widenSlotsIntoPadding(context, uses, joinedRows, claims.span(), WidenedRepresentations::Numbers, packed);
    if (!finishStep("widenSlotsIntoPadding(Numbers)"_s))
        return { };
    widenSlotsIntoPadding(context, uses, joinedRows, claims.span(), WidenedRepresentations::References, packed);
    if (!finishStep("widenSlotsIntoPadding(References)"_s))
        return { };
    reserveEmptySlotsInPadding(context, uses, packed);
    if (!finishStep("reserveEmptySlotsInPadding"_s))
        return { };
    widenSlotsIntoPadding(context, uses, joinedRows, claims.span(), WidenedRepresentations::SmallIntegers, packed);
    if (!finishStep("widenSlotsIntoPadding(SmallIntegers)"_s))
        return { };

    Vector<PlannedLayout> layouts = createPlannedLayouts(context, WTF::move(packed));
    times.didFinish("createPlannedLayouts"_s);
    return layouts;
}

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
        Expected<bool> isMadeInLoop = uses.layouts[index].isMadeInLoop;
        for (FieldIndex field = 0; field < context.rows.numberOfFieldsOf(id); ++field)
            isMadeInLoop |= uses.rows[context.rows.rowOf(id, field)].hasHoldersMadeInLoop;
        if (!isMadeInLoop.predicted())
            layouts.quickSet(index);
    }
    for (const TaggedFamily& family : program.taggedFamilies) {
        layouts.quickClear(family.maker.index());
        for (const TaggedLayout& member : family.members)
            layouts.quickClear(member.layout.index());
    }
    return layouts;
}

static BitVector findKeysDefinedByDescriptor(const Program& program, const SlotUses& uses)
{
    BitVector keys(program.types.numberOfPropertyKeys());
    uses.keysDefinedByDescriptor.forEachSetBit([&](size_t index) {
        keys.quickSet(index);
    });
    for (const Layout& layout : program.layouts) {
        for (const ClassMember& member : layout.classFlow.members) {
            if (member.key && member.kind != MethodKind::Method)
                keys.quickSet(member.key.index());
        }
    }
    return keys;
}

// The objects lie as the image that makes them has laid them out.
static void readLayoutsBelow(const Program& program, BuildContext& build, Vector<PlannedLayout>& layouts)
{
    if (program.recordsOfImagesBelow.isEmpty())
        return;
    for (const Program::RecordOfImageBelow& found : program.recordsOfImagesBelow) {
        ImagesBelow::Record record = build.imagesBelow()->recordAt(found.image, found.firstShape, found.numberOfShapes);
        const Layout& layout = program.at(found.layout);
        PlannedLayout& planned = layouts[found.layout.index()];
        planned = { };
        if (layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        planned.storage = LayoutStorage::CompiledObject;
        planned.size = record.layout->size;
        planned.slots = Vector<PlacedSlot>(layout.fields.size() + numberOfHiddenWords);
        std::span<const FieldData> fields = record.layout->fields();
        RELEASE_ASSERT(fields.size() <= layout.fields.size());
        for (size_t index = 0; index < fields.size(); ++index) {
            PlacedSlot& slot = planned.slots[index];
            slot.representation = fields[index].slot.representation();
            slot.storage = SlotStorage::Inline;
            slot.offset = fields[index].slot.offset;
        }
        if (std::optional<SlotData> extraProperties = record.layout->extraPropertiesSlot()) {
            PlacedSlot& slot = planned.slots[planned.extraPropertiesWordIndex()];
            slot.representation = extraProperties->representation();
            slot.storage = SlotStorage::Inline;
            slot.offset = extraProperties->offset;
        }
    }
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
    SlotPlan plan;
    runTasksInParallel(build.parallelLoops(), {
        [&] { plan.m_layouts = planLayouts(context, uses, times); },
        [&] { plan.m_variables = timed("planVariables"_s, [&] { return planVariables(context, uses); }); },
        [&] { plan.m_closureForms = timed("chooseClosureForms"_s, [&] { return chooseClosureForms(context, uses); }); },
        [&] { plan.m_classCreation = timed("planClassCreation"_s, [&] { return planClassCreation(context); }); },
        [&] { plan.m_keysWithColumn = timed("chooseKeysWithColumn"_s, [&] { return chooseKeysWithColumn(context, uses); }); },
        [&] { plan.m_keysDefinedByDescriptor = findKeysDefinedByDescriptor(program, uses); },
    });
    if (build.diagnostics().hasErrors())
        return { };
    readLayoutsBelow(program, build, plan.m_layouts);
    return plan;
}

struct SlotAgreementEntry {
    PropertyKeyID key;
    LayoutSetID receivers;
    Vector<LayoutID> members;
    SlotAgreement agreement;
};

class SlotAgreementMemo : public SharedMemo<SlotAgreementEntry> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotAgreementMemo);
};

SlotPlan::SlotPlan()
    : m_agreements(makeUnique<SlotAgreementMemo>())
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

BitVector PlannedLayout::padding() const
{
    BitVector padding(size);
    if (!hasSlots())
        return padding;
    for (uint32_t offset = storage == LayoutStorage::Environment ? offsetOfFirstEnvironmentVariable : offsetOfFirstField; offset < size; ++offset)
        padding.quickSet(offset);
    auto occupy = [&](uint32_t offset, uint32_t width) {
        for (uint32_t end = offset + width; offset < end; ++offset)
            padding.quickClear(offset);
    };
    for (const PlacedSlot& slot : slots) {
        if (slot.storage == SlotStorage::Inline)
            occupy(slot.offset, slot.width());
    }
    for (ReservedBytes bytes : bytesKeptZero)
        occupy(bytes.offset, bytes.width);
    if (isEscapedOffset)
        occupy(*isEscapedOffset, sizeof(bool));
    return padding;
}

SlotAgreement SlotPlan::agreementOf(const Program& program, PropertyKeyID key, std::span<const LayoutID> members) const
{
    SlotAgreement agreement;
    for (LayoutID member : members) {
        if (program.at(member).flow.allocation == AllocationState::NeverAllocated)
            continue;
        std::optional<uint32_t> field;
        if (holdsCompiledObjects(member))
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
            return holdsCompiledObjects(member) && layout(member).keepsZero(shared.offset, shared.width());
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
    AOT::sort(all.members);
    return all;
}

const SlotAgreement& SlotPlan::agreementOf(const Program& program, PropertyKeyID key, LayoutSetID receivers) const
{
    if (!receivers)
        return m_agreementOfNoReceivers;
    std::span<const LayoutID> members = program.at(receivers).completeMembers();
    // An id that a thread made for itself names another set in the next phase, so such a set is found by its members.
    bool isFoundByMembers = TypeTable::isProvisional(receivers);
    uint32_t hash = pairIntHash(key.index(), isFoundByMembers ? hashOfLayouts(members) : receivers.index());
    auto isWanted = [&](const SlotAgreementEntry& entry) {
        if (entry.key != key)
            return false;
        return isFoundByMembers ? !entry.receivers && std::ranges::equal(entry.members, members) : entry.receivers == receivers;
    };
    if (const SlotAgreementEntry* known = m_agreements->find(hash, isWanted))
        return known->agreement;
    SlotAgreementEntry entry { key, { }, { }, agreementOf(program, key, members) };
    if (isFoundByMembers)
        entry.members = Vector<LayoutID>(members);
    else
        entry.receivers = receivers;
    return m_agreements->add(hash, WTF::move(entry), isWanted).agreement;
}

MemberCreation SlotPlan::creationOf(const Program& program, LayoutID classLayout, FunctionID member) const
{
    UNUSED_PARAM(program);
    bool isCreatedByClassRecord = std::ranges::binary_search(classCreation(classLayout).functionsCreatedByClassRecord, member.index(), { }, &FunctionID::index);
    return isCreatedByClassRecord ? MemberCreation::ByClassRecord : MemberCreation::AtDefinition;
}

static LayoutID provenBaseOf(const Layout& layout)
{
    return layout.flow.baseState == BaseState::Layout ? layout.flow.base : LayoutID();
}

InheritedRows::InheritedRows(const Program& program, const SlotRows& rows, std::span<const LayoutStorage> layoutStorage)
{
    unsigned numberOfLayouts = program.layouts.size();
    auto isCompiledObject = [&](LayoutID id) {
        return layoutStorage[id.index()] == LayoutStorage::CompiledObject;
    };

    Vector<LayoutID> roots(numberOfLayouts);
    m_depths.fill(0, numberOfLayouts);
    BitVector isPacked(numberOfLayouts);
    m_isInFamily.ensureSize(numberOfLayouts);
    for (unsigned index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        LayoutID root = id;
        for (LayoutID base = nearestLayoutAlwaysAbove(program.at(id).flow); base; base = nearestLayoutAlwaysAbove(program.at(base).flow)) {
            RELEASE_ASSERT(m_depths[index] < numberOfLayouts);
            ++m_depths[index];
            root = base;
            if (!isCompiledObject(id))
                continue;
            isPacked.quickSet(base.index());
            m_isInFamily.quickSet(base.index());
            m_isInFamily.quickSet(index);
        }
        roots[index] = root;
        if (isCompiledObject(id))
            isPacked.quickSet(index);
    }

    for (unsigned index : isPacked) {
        if (m_layoutsByDepth.size() <= m_depths[index])
            m_layoutsByDepth.grow(m_depths[index] + 1);
        m_layoutsByDepth[m_depths[index]].append(LayoutID(index));
    }

    m_declaringRow = Vector<SlotRow>(rows.count(), [](size_t row) {
        return static_cast<SlotRow>(row);
    });
    Vector<std::tuple<uint32_t, SlotRow, SlotRow>> members;
    Vector<std::pair<SlotRow, SlotRow>> rowsInUnallocatedBases;
    for (unsigned index : m_isInFamily) {
        LayoutID id(index);
        auto add = [&](SlotRow row, LayoutID declaringLayout, SlotRow declaringRow) {
            m_declaringRow[row] = declaringRow;
            if (isCompiledObject(id))
                members.append({ m_depths[declaringLayout.index()], declaringRow, row });
            else
                rowsInUnallocatedBases.append({ declaringRow, row });
        };
        for (FieldIndex field = 0; field < rows.numberOfFieldsOf(id); ++field) {
            FieldReference declaration = declarationOf(program, { id, field });
            add(rows.rowOf(id, field), declaration.layout, rows.rowOf(declaration.layout, declaration.field));
        }
        LayoutID root = roots[index];
        add(rows.extensionWordOf(id), root, rows.extensionWordOf(root));
        add(rows.extraPropertiesWordOf(id), root, rows.extraPropertiesWordOf(root));
    }
    AOT::sortInLine(members);

    m_rows.reserveInitialCapacity(members.size());
    for (auto [depth, declaringRow, row] : members) {
        if (m_families.isEmpty() || m_families.last().declaringRow != declaringRow)
            m_families.append(Family { declaringRow, static_cast<uint32_t>(m_rows.size()), 0 });
        ++m_families.last().numberOfRows;
        m_rows.append(row);
    }

    Vector<uint32_t> firstOfDeclaringRow(FillWith { }, rows.count() + 1, 0u);
    for (auto [declaringRow, row] : rowsInUnallocatedBases)
        ++firstOfDeclaringRow[declaringRow + 1];
    for (size_t row = 0; row < rows.count(); ++row)
        firstOfDeclaringRow[row + 1] += firstOfDeclaringRow[row];
    m_rowsInUnallocatedBases.grow(rowsInUnallocatedBases.size());
    Vector<uint32_t> nextOfDeclaringRow = firstOfDeclaringRow;
    for (auto [declaringRow, row] : rowsInUnallocatedBases)
        m_rowsInUnallocatedBases[nextOfDeclaringRow[declaringRow]++] = row;
    for (Family& family : m_families) {
        family.firstRowInUnallocatedBases = firstOfDeclaringRow[family.declaringRow];
        family.numberOfRowsInUnallocatedBases = firstOfDeclaringRow[family.declaringRow + 1] - firstOfDeclaringRow[family.declaringRow];
        std::span<SlotRow> rowsOfFamily = m_rowsInUnallocatedBases.mutableSpan().subspan(family.firstRowInUnallocatedBases, family.numberOfRowsInUnallocatedBases);
        if (!std::ranges::is_sorted(rowsOfFamily))
            AOT::sort(rowsOfFamily);
    }
}

static void add(SlotRowUse& sum, const SlotRowUse& use)
{
    sum.accessSites += use.accessSites;
    sum.accessWeight = saturatedSum(sum.accessWeight, use.accessWeight);
    sum.fillHeat += use.fillHeat;
    sum.holderHeat += use.holderHeat;
    sum.hasHoldersMadeInLoop |= use.hasHoldersMadeInLoop;
    sum.initializingSites += use.initializingSites;
    sum.sitesInitializingWithValue += use.sitesInitializingWithValue;
    sum.storesOfConstructorWithValue += use.storesOfConstructorWithValue;
    sum.storesOfUndefined += use.storesOfUndefined;
    sum.isRequiredByDeclaringType |= use.isRequiredByDeclaringType;
    sum.isOptionalInTypeOfThis |= use.isOptionalInTypeOfThis;
    sum.environmentUses += use.environmentUses;
    sum.storesWithoutAllocation += use.storesWithoutAllocation;
    sum.storesIntoFilledSlots += use.storesIntoFilledSlots;
    sum.storesIntoNewObjects += use.storesIntoNewObjects;
    sum.privateDefinitionsByName += use.privateDefinitionsByName;
}

void shareUsesOfInheritedRows(const InheritedRows& inheritedRows, SlotUses& uses)
{
    for (SlotRowUse& use : uses.rows)
        use.initializingSitesOfOwnLayout = use.initializingSites;
    auto share = [&](SlotRow row, SlotRowUse sum) {
        sum.initializingSitesOfOwnLayout = uses.rows[row].initializingSitesOfOwnLayout;
        uses.rows[row] = sum;
    };
    for (const InheritedRows::Family& family : inheritedRows.families()) {
        SlotRowUse sum;
        for (SlotRow row : inheritedRows.rowsOf(family))
            add(sum, uses.rows[row]);
        for (SlotRow row : inheritedRows.rowsInUnallocatedBasesOf(family))
            add(sum, uses.rows[row]);
        for (SlotRow row : inheritedRows.rowsOf(family))
            share(row, sum);
        share(family.declaringRow, sum);
    }
}

namespace {

constexpr uint32_t noSet = UINT32_MAX;

struct JoinedRowSet : WTF::UnionFind<JoinedRowSet> { };

} // anonymous namespace

void JoinedRows::collectAccesses(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    Vector<const SlotAccessKey*> keys;
    uint32_t numberOfReceivers = 0;
    for (const SlotAccessUse& use : uses.accesses) {
        const SlotAccessKey& access = use.access;
        if (!access.receivers)
            continue;
        if (!keys.isEmpty() && keys.last()->key == access.key && keys.last()->receivers == access.receivers) {
            m_accesses.last().weight = saturatedSum(m_accesses.last().weight, use.weight);
            continue;
        }
        keys.append(&access);
        m_accesses.append(Access { numberOfReceivers, 0, use.weight });
        numberOfReceivers += program.at(access.receivers).written().size();
    }

    m_rowsOfAccesses.grow(numberOfReceivers);
    forEachIndexInParallel(context.build.parallelLoops(), m_accesses.size(), [&](unsigned index) {
        Access& access = m_accesses[index];
        context.forEachHolder(keys[index]->key, keys[index]->receivers, [&](LayoutID, SlotRow row) {
            m_rowsOfAccesses[access.firstRow + access.numberOfRows++] = row;
        });
    });
}

JoinedRows::JoinedRows(const SlotPlanContext& context, const SlotUses& uses)
{
    collectAccesses(context, uses);

    uint32_t numberOfRows = context.rows.count();
    Vector<JoinedRowSet> forest(numberOfRows);
    auto rootOf = [&](SlotRow row) {
        return static_cast<SlotRow>(forest[row].find() - forest.begin());
    };
    auto join = [&](std::span<const SlotRow> rows) {
        for (SlotRow row : rows.subspan(1))
            forest[row].unify(&forest[rows[0]]);
    };
    for (const InheritedRows::Family& family : context.inheritedRows.families()) {
        if (!context.rows.isHiddenWord(family.declaringRow))
            join(context.inheritedRows.rowsOf(family));
    }
    for (const Access& access : m_accesses) {
        if (access.numberOfRows)
            join(rowsOf(access));
    }

    Vector<uint32_t> setOfRoot(FillWith { }, numberOfRows, noSet);
    for (const Access& access : m_accesses) {
        if (access.numberOfRows < 2)
            continue;
        uint32_t& index = setOfRoot[rootOf(rowsOf(access)[0])];
        if (index == noSet) {
            index = m_sets.size();
            m_sets.append(Set { });
        }
        m_sets[index].weight = saturatedSum(m_sets[index].weight, access.weight);
    }
    auto setOf = [&](SlotRow row) {
        return setOfRoot[rootOf(row)];
    };

    m_accesses.removeAllMatching([&](const Access& access) {
        return !access.numberOfRows || setOf(rowsOf(access)[0]) == noSet;
    });
    AOT::stableSortInLine(m_accesses, { }, [&](const Access& access) {
        return setOf(rowsOf(access)[0]);
    });
    for (uint32_t index = 0; index < m_accesses.size(); ++index) {
        Set& joined = m_sets[setOf(rowsOf(m_accesses[index])[0])];
        if (!joined.numberOfAccesses++)
            joined.firstAccess = index;
    }

    for (SlotRow row = 0; row < numberOfRows; ++row) {
        if (uint32_t index = setOf(row); index != noSet)
            ++m_sets[index].numberOfRows;
    }
    uint32_t numberOfJoinedRows = 0;
    for (Set& joined : m_sets) {
        joined.firstRow = numberOfJoinedRows;
        numberOfJoinedRows += std::exchange(joined.numberOfRows, 0);
    }
    m_rows.grow(numberOfJoinedRows);
    for (SlotRow row = 0; row < numberOfRows; ++row) {
        uint32_t index = setOf(row);
        if (index == noSet)
            continue;
        Set& joined = m_sets[index];
        m_rows[joined.firstRow + joined.numberOfRows++] = row;
    }

    AOT::sortInLine(m_sets, [](const Set& a, const Set& b) {
        return std::tuple(b.weight.predicted(), a.firstRow) < std::tuple(a.weight.predicted(), b.firstRow);
    });
}

static constexpr OpcodeSet constantsComparedBitwise { Opcode::Undefined, Opcode::Null, Opcode::Boolean };

bool isDescribedToRunTime(const Program& program, const FlowResults& flow, ModuleID exporter, const ModuleExport& moduleExport)
{
    switch (moduleExport.kind) {
    case ModuleExportKind::Variable:
        // Only the variable says whether the name is an export.
        if (moduleExport.isDroppedWhileEmpty)
            return true;
        switch (flow.module(exporter).namespaceUse) {
        case NamespaceUse::Unknown:
            return true;
        case NamespaceUse::NamedReadsOnly:
            return std::ranges::binary_search(flow.module(exporter).exportsReadByName, moduleExport.variable);
        case NamespaceUse::NeverCreated:
            return program.at(moduleExport.variable).flow.reads == ReadState::Unknown;
        }
        RELEASE_ASSERT_NOT_REACHED();
    case ModuleExportKind::Namespace:
    case ModuleExportKind::Indirect:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findVariablesDescribedToRunTime(const Program& program, const FlowResults& flow)
{
    BitVector described(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            described.quickSet(variable.index());
    };
    for (unsigned index = 0; index < program.modules.size(); ++index) {
        auto& module = program.modules[index];
        for (auto& entry : module.exports) {
            if (isDescribedToRunTime(program, flow, ModuleID(index), entry))
                add(entry.variable);
        }
        for (auto& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (auto& scope : program.evalScopes) {
        for (auto& binding : scope.bindings)
            add(binding.variable);
    }
    for (auto& layout : program.layouts) {
        for (auto& field : layout.staticFields)
            add(field.variable);
    }
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].flags.contains(VariableFlags::HoldsMock))
            described.quickSet(index);
    }
    return described;
}

static const Type& typeOfStoredValues(const Program& program, const Variable& variable)
{
    return program.at(program.provenTypeOf(variable.declared));
}

static bool isFilledWithUndefinedValue(const Program& program, const Variable& variable)
{
    return variable.initialization == VariableInitialization::Undefined
        && variable.flow.initialization == InitializationState::Unknown
        && defaultSlotRepresentationOf(typeOfStoredValues(program, variable)) == SlotRepresentation::JSValue;
}

static bool canHoldAbsentValue(const Program& program, const Variable& variable)
{
    return !!(typeOfStoredValues(program, variable).primitives & Primitive::Absent);
}

static BitVector findZeroEncodingCandidates(const Program& program, const BitVector& described)
{
    BitVector candidates(program.variables.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!described.quickGet(index) && isFilledWithUndefinedValue(program, variable) && !canHoldAbsentValue(program, variable))
            candidates.quickSet(index);
    }
    return candidates;
}

BitVector findZeroEncodingCandidates(const SlotPlanContext& context)
{
    return findZeroEncodingCandidates(context.program, findVariablesDescribedToRunTime(context.program, context.inputs.flow));
}

static bool isColdBlock(const Block& block)
{
    TerminatorKind kind = block.terminator.kind;
    return block.frequency.predicted() != BlockFrequency::Normal
        || kind == TerminatorKind::Throw
        || kind == TerminatorKind::Rethrow
        || kind == TerminatorKind::Unreachable;
}

static bool canUseEncodedValue(const Program& program, const Function& function, const Use& use)
{
    if (isColdBlock(function.at(use.block)))
        return true;
    switch (use.kind) {
    case Use::Kind::Operand:
        break;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    auto& user = function.at(use.user);
    if (user.opcode == Opcode::TypeTest)
        return program.at(user.testedType()).isUndefinedOrNull();
    if (user.opcode == Opcode::StrictEq) {
        uint32_t otherOperand = use.operandIndex == OperandIndex::StrictEq::left ? OperandIndex::StrictEq::right : OperandIndex::StrictEq::left;
        auto constant = constantThroughCasts(program, function, user.operand(otherOperand));
        return constant && constantsComparedBitwise.contains(function.at(*constant).opcode);
    }
    if (user.opcode == Opcode::Check)
        return use.operandIndex == OperandIndex::Check::value && !(program.at(user.type).primitives & Primitive::Undefined);
    return false;
}

void findUncheckedVariableReads(FunctionAnalyses& analyses, const BitVector& candidates, Vector<VariableID>& unchecked)
{
    auto& program = analyses.program();
    auto& function = analyses.function();
    auto& useLists = analyses.useLists();
    size_t numberBefore = unchecked.size();
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& read = function.at(id);
            if (read.opcode != Opcode::GetModuleVariable || !candidates.quickGet(read.variable().index()))
                continue;
            bool isChecked = std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
                return canUseEncodedValue(program, function, use);
            });
            if (!isChecked)
                unchecked.append(read.variable());
        }
    }
    auto added = unchecked.mutableSpan().subspan(numberBefore);
    AOT::sort(added);
    unchecked.shrink(unchecked.size() - std::ranges::unique(added).size());
}

static PlannedVariable planFunctionVariable(const Variable& variable)
{
    if (variable.flow.stores != StoreCount::Never || variable.flags.contains(VariableFlags::IsReplaceable))
        return { .storage = VariableStorage::Statics, .encoding = VariableEncoding::FunctionAsZero, .representation = SlotRepresentation::JSValue };
    return { .storage = VariableStorage::ClosureTable, .encoding = VariableEncoding::FunctionAsZero, .representation = SlotRepresentation::DeferredClosure };
}

static SlotRepresentation variableRepresentationOf(const SlotPlanContext& context, const Variable& variable)
{
    const Type& stored = typeOfStoredValues(context.program, variable);
    bool canObserveDeadZone = variable.initialization == VariableInitialization::Uninitialized && variable.flow.initialization == InitializationState::Unknown;
    bool isStaticField = !!variable.owner;
    if (canObserveDeadZone || isStaticField)
        return representationFor(context, stored.withPrimitives(Primitive::Absent), { });
    return representationFor(context, stored, { });
}

static PlannedVariable planVariable(const SlotPlanContext& context, const Variable& variable, const VariableUse& use, bool isDescribed, bool isZeroEncodingCandidate)
{
    if (!use.isNamedByCode && !isDescribed)
        return { .storage = VariableStorage::None };
    switch (variable.initialization) {
    case VariableInitialization::Function:
        return planFunctionVariable(variable);
    case VariableInitialization::Uninitialized:
    case VariableInitialization::Undefined:
        break;
    }
    bool readsZeroAsUndefined = isZeroEncodingCandidate && !use.hasUncheckedRead;
    bool holdsAnyValue = readsZeroAsUndefined || variable.flags.contains(VariableFlags::IsReplaceable);
    return {
        .storage = VariableStorage::Statics,
        .encoding = readsZeroAsUndefined ? VariableEncoding::UndefinedAsZero : VariableEncoding::Plain,
        .representation = holdsAnyValue ? SlotRepresentation::JSValue : variableRepresentationOf(context, variable),
        .isUsedBeforeProgramEntry = use.isUsedBeforeProgramEntry || context.program.at(variable.module).kind == ModuleKind::RuntimeSource,
    };
}

Vector<PlannedVariable> planVariables(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    const BitVector described = findVariablesDescribedToRunTime(program, context.inputs.flow);
    const BitVector candidates = findZeroEncodingCandidates(program, described);
    Vector<PlannedVariable> variables(program.variables.size());
    forEachIndexInParallel(context.build.parallelLoops(), program.variables.size(), [&](unsigned index) {
        variables[index] = planVariable(context, program.variables[index], uses.variables[index], described.quickGet(index), candidates.quickGet(index));
    });
    return variables;
}

namespace {

struct BaseChainSummary {
    bool everyBaseIsProvenOnce { false };
    bool endsWithoutHeritage { false };
};

} // anonymous namespace

static bool hasNoHeritage(const Layout& layout)
{
    return layout.flow.baseState == BaseState::None && !layout.has(LayoutFlags::HasHeritage);
}

static Vector<BaseChainSummary> summarizeBaseChains(const Program& program)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    size_t layoutCount = program.layouts.size();
    Vector<WalkState> states(FillWith { }, layoutCount, WalkState::Unvisited);
    Vector<BaseChainSummary> summaries(layoutCount);
    Vector<LayoutID> path;
    for (size_t index = 0; index < layoutCount; ++index) {
        for (LayoutID id(index); id && states[id.index()] == WalkState::Unvisited; id = provenBaseOf(program.at(id))) {
            states[id.index()] = WalkState::OnPath;
            path.append(id);
        }
        for (; !path.isEmpty(); path.removeLast()) {
            LayoutID id = path.last();
            const Layout& layout = program.at(id);
            LayoutID base = provenBaseOf(layout);
            BaseChainSummary& summary = summaries[id.index()];
            if (!base) {
                summary.everyBaseIsProvenOnce = true;
                summary.endsWithoutHeritage = hasNoHeritage(layout);
            } else if (states[base.index()] == WalkState::Done) {
                summary.everyBaseIsProvenOnce = isEvaluatedOnce(program, program.at(base)) && summaries[base.index()].everyBaseIsProvenOnce;
                summary.endsWithoutHeritage = summaries[base.index()].endsWithoutHeritage;
            }
            states[id.index()] = WalkState::Done;
        }
    }
    return summaries;
}

static bool isCreatedWithItsHolder(const Program& program, ClassSide side, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool capturesNothingButClassEnvironment(const Program& program, const Function& constructor, const Function& function)
{
    if (function.captures.isEmpty())
        return true;
    if (function.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = function.captures[0].written();
    return captured && captured == constructor.captures[0].written() && program.at(captured).environment;
}

static bool canBeCreatedByClassRecord(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || member.isAssignedByProgram || program.isIndexLike(member.key) || isCreatedWithItsHolder(program, member.side, member.key))
        return false;
    const Function& function = program.at(member.function);
    return function.name && capturesNothingButClassEnvironment(program, constructor, function);
}

static ClassCreationPlan planClass(const Program& program, const Layout& layout, const BaseChainSummary& bases)
{
    const Vector<ClassMember>& members = layout.classFlow.members;
    ClassCreationPlan plan;
    plan.members.fill(MemberCreation::AtDefinition, members.size());
    if (layout.has(LayoutFlags::IsConstructorFunction))
        return plan;

    const Function& constructor = program.at(layout.constructor);
    std::array<bool, numberOfClassSides> recordCreatesSide;
    recordCreatesSide.fill(true);
    for (size_t index = 0; index < members.size(); ++index) {
        bool& recordCreatesNextMember = recordCreatesSide[static_cast<unsigned>(members[index].side)];
        recordCreatesNextMember = recordCreatesNextMember && canBeCreatedByClassRecord(program, constructor, members[index]);
        if (recordCreatesNextMember)
            plan.members[index] = MemberCreation::ByClassRecord;
    }
    Vector<FunctionID> functionsCreatedAtDefinition;
    for (size_t index = 0; index < members.size(); ++index)
        (plan.members[index] == MemberCreation::ByClassRecord ? plan.functionsCreatedByClassRecord : functionsCreatedAtDefinition).append(members[index].function);
    AOT::sort(plan.functionsCreatedByClassRecord, { }, &FunctionID::index);
    AOT::sort(functionsCreatedAtDefinition, { }, &FunctionID::index);
    plan.functionsCreatedByClassRecord.removeAllMatching([&](FunctionID& function) {
        return std::ranges::binary_search(functionsCreatedAtDefinition, function.index(), { }, &FunctionID::index);
    });

    bool recordCreatesPrototypeSide = recordCreatesSide[static_cast<unsigned>(ClassSide::Prototype)];
    bool recordCreatesStaticSide = recordCreatesSide[static_cast<unsigned>(ClassSide::Static)];
    plan.createsMembersLazily = isEvaluatedOnce(program, layout) && recordCreatesPrototypeSide;
    plan.createsPrototypeBeforeClassObject = plan.createsMembersLazily && bases.everyBaseIsProvenOnce && bases.endsWithoutHeritage;
    plan.createsClassObjectOnMutation = plan.createsMembersLazily && bases.everyBaseIsProvenOnce && recordCreatesStaticSide;
    return plan;
}

static bool isClassWithConstantNames(const Layout& layout)
{
    if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || layout.has(LayoutFlags::IsConstructorFunction))
        return false;
    return std::ranges::all_of(layout.classFlow.members, [](const ClassMember& member) {
        return member.key && !member.isAssignedByProgram;
    });
}

static bool definitionMakesWhatRecordsSay(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (!isClassWithConstantNames(layout))
            return false;
        if (hasNoHeritage(layout))
            return true;
        switch (layout.flow.baseState) {
        case BaseState::BuiltinClass:
            return true;
        case BaseState::Unknown:
            if (!layout.base)
                return true;
            break;
        case BaseState::Layout:
            break;
        case BaseState::None:
            return false;
        }
        current = expectedBaseOf(layout);
    }
    return false;
}

Vector<ClassCreationPlan> planClassCreation(const SlotPlanContext& context)
{
    const Program& program = context.program;
    Vector<BaseChainSummary> baseChains = summarizeBaseChains(program);
    Vector<ClassCreationPlan> plans(program.layouts.size());
    uint64_t membersCreatedByClassRecord = 0;
    uint64_t membersCreatedAtDefinition = 0;
    uint64_t classesCreatedLazily = 0;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            continue;
        ClassCreationPlan& plan = plans[index];
        plan = planClass(program, layout, baseChains[index]);
        plan.guardsHoldWhileClear = definitionMakesWhatRecordsSay(program, LayoutID(index));
        uint64_t byClassRecord = std::ranges::count(plan.members, MemberCreation::ByClassRecord);
        membersCreatedByClassRecord += byClassRecord;
        membersCreatedAtDefinition += plan.members.size() - byClassRecord;
        classesCreatedLazily += plan.createsMembersLazily;
    }

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::ClassMembersCreatedByClassRecord, membersCreatedByClassRecord);
    statistics.count(StatisticID::ClassMembersCreatedAtDefinition, membersCreatedAtDefinition);
    statistics.count(StatisticID::ClassesCreatedLazily, classesCreatedLazily);
    return plans;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
