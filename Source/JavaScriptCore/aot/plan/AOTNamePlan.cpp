/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFacts.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTPropertyCachesFront.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// Image numbers names from one up, the promised ones first. A stub that goes by the column asks the table of fields under the same number.
static_assert(!RowDisplacementTable::hasColumn(0) && RowDisplacementTable::hasColumn(RowDisplacementTable::maxColumn) && FieldTableEntry::hasColumn(RowDisplacementTable::maxColumn));

namespace {

enum class PlaceOfNamedFieldInNamePlan : uint8_t { ValueInCell, ValueInExtension, DoubleInCell, Elsewhere };

template<typename T>
struct ByPlaceOfNamedFieldInNamePlan {
    T& operator[](PlaceOfNamedFieldInNamePlan place) { return values[static_cast<unsigned>(place)]; }
    T operator[](PlaceOfNamedFieldInNamePlan place) const { return values[static_cast<unsigned>(place)]; }

    std::array<T, 4> values { };
};

} // anonymous namespace

// computeSlotPlanParts() of AOTSlotPlan.cpp calls it in front of the layouts, and declares it for itself.
MentionsOfKeys countMentionsOfKeys(const Program&, const Facts&, const ParallelLoops&);
MentionsOfKeys countMentionsOfKeys(const Program& program, const Facts& facts, const ParallelLoops& loops)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    MentionsOfKeys mentions;
    mentions.all.fill(0, numberOfKeys);
    mentions.thatMayRunAgain.fill(0, numberOfKeys);
    const LinkTables& link = facts.linked();
    forEachRangeOfIndicesInParallel(loops, program.functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            FunctionID id(index);
            if (!link.function(id).isReached)
                continue;
            bool mayRunAgain = !facts.runsAtMostOnce(id) && !program.at(id).isModuleBody();
            const ThinSummary& summary = facts.summaries().at(id);
            for (const Vector<PropertyKeyID>* names : { &summary.namesRead, &summary.namesCalled, &summary.namesStored }) {
                for (PropertyKeyID key : *names) {
                    if (key.index() >= numberOfKeys)
                        continue;
                    AtomicRef(mentions.all[key.index()]).fetch_add(1, std::memory_order_relaxed);
                    if (mayRunAgain)
                        AtomicRef(mentions.thatMayRunAgain[key.index()]).fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });
    // `made[name] = 1` with a name out of `["a", "b"]`: a text that the program spells may come to be a key.
    for (size_t text : facts.summaries().textsSpelledThatNameFieldsNeverWritten()) {
        std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = StringID(static_cast<uint32_t>(text)) });
        if (key && key->index() < numberOfKeys)
            mentions.isSpelledAsText.set(key->index());
    }
    return mentions;
}

static BitVector chooseKeysWithColumnForNamePlan(const Program& program, const MentionsOfKeys& mentions, BuildContext& build)
{
    size_t numberOfColumns = static_cast<size_t>(std::min<uint64_t>(build.limit(LimitID::LookupTableNames), RowDisplacementTable::maxColumn));
    uint32_t numberOfKeys = static_cast<uint32_t>(mentions.all.size());
    BitVector keysWithColumn(numberOfKeys);

    Vector<uint32_t> mentionedKeys;
    for (uint32_t key = 0; key < numberOfKeys; ++key) {
        if (mentions.all[key] && program.types.hasStableNumber(PropertyKeyID(key)))
            mentionedKeys.append(key);
    }
    if (mentionedKeys.size() > numberOfColumns) {
        parallelSort(build.parallelLoops(), mentionedKeys.mutableSpan(), [&](uint32_t first, uint32_t second) {
            if (mentions.thatMayRunAgain[first] != mentions.thatMayRunAgain[second])
                return mentions.thatMayRunAgain[first] > mentions.thatMayRunAgain[second];
            return mentions.all[first] != mentions.all[second] ? mentions.all[first] > mentions.all[second] : first < second;
        });
        mentionedKeys.shrink(numberOfColumns);
    }
    for (uint32_t key : mentionedKeys)
        keysWithColumn.quickSet(key);

    // The walk reads built-in names that the program never spells. Image numbers every one of them anyway, so a promise adds nothing to the image.
    size_t columnsLeft = numberOfColumns - mentionedKeys.size();
    for (uint32_t key = 0; key < numberOfKeys && columnsLeft; ++key) {
        PropertyKeyID id(key);
        if (mentions.all[key] || !program.types.hasStableNumber(id) || !program.builtinNameOf(id))
            continue;
        keysWithColumn.quickSet(key);
        --columnsLeft;
    }
    return keysWithColumn;
}

static PlaceOfNamedFieldInNamePlan placeOfNamedFieldInSlotForNamePlan(const PlacedSlot& slot)
{
    if (traitsOf(slot.representation).valueKind == SlotValueKind::Double && !slot.isOutOfLine())
        return PlaceOfNamedFieldInNamePlan::DoubleInCell;
    if (slot.representation != SlotRepresentation::JSValue && slot.representation != SlotRepresentation::Cell)
        return PlaceOfNamedFieldInNamePlan::Elsewhere;
    return slot.isOutOfLine() ? PlaceOfNamedFieldInNamePlan::ValueInExtension : PlaceOfNamedFieldInNamePlan::ValueInCell;
}

static void forEachNamedSlotOfCompiledObjectsForNamePlan(const Program& program, const SlotPlanParts& parts, size_t firstLayout, size_t endOfLayouts, const Invocable<void(PropertyKeyID, const PlacedSlot&)> auto& functor)
{
    for (size_t index = firstLayout; index < endOfLayouts; ++index) {
        const PlannedLayout& planned = parts.layouts[index];
        if (!planned.holdsCompiledObjects() || parts.layoutsNeverAllocated.get(index))
            continue;
        const Vector<Field>& declared = program.at(LayoutID(index)).fields;
        unsigned numberDeclared = planned.numberOfFields() - static_cast<unsigned>(planned.appendedKeys.size());
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            bool isDeclared = field < numberDeclared && field < declared.size();
            PropertyKeyID key = isDeclared ? declared[field].key : field >= numberDeclared ? planned.appendedKeys[field - numberDeclared] : PropertyKeyID();
            if (key)
                functor(key, planned.slots[field]);
        }
    }
}

static FieldTableProbePlan planFieldTableProbesForNamePlan(const Program& program, const SlotPlanParts& parts, const MentionsOfKeys& mentions, const ParallelLoops& loops)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    Vector<ByPlaceOfNamedFieldInNamePlan<uint32_t>> layoutsByKey(numberOfKeys);
    forEachRangeOfIndicesInParallel(loops, parts.layouts.size(), [&](size_t first, size_t end) {
        forEachNamedSlotOfCompiledObjectsForNamePlan(program, parts, first, end, [&](PropertyKeyID key, const PlacedSlot& slot) {
            if (slot.hasStorage())
                AtomicRef(layoutsByKey[key.index()][placeOfNamedFieldInSlotForNamePlan(slot)]).fetch_add(1, std::memory_order_relaxed);
        });
    });

    FieldTableProbePlan plan;
    // A function that spells the name counts for each form by the share of the layouts that have it. An access under a key that the build does not know may ask for any name, so each name counts once by itself.
    ByPlaceOfNamedFieldInNamePlan<double> reads;
    for (unsigned key = 0; key < numberOfKeys; ++key) {
        const ByPlaceOfNamedFieldInNamePlan<uint32_t>& layouts = layoutsByKey[key];
        double total = 0;
        for (uint32_t count : layouts.values)
            total += count;
        // The sums are rounded, so they are taken in an order that is the same in every build.
        if (!total || !program.types.hasStableNumber(PropertyKeyID(key)))
            continue;
        double places = 1.0 + mentions.all[key];
        for (unsigned place = 0; place < reads.values.size(); ++place)
            reads.values[place] += places * (layouts.values[place] / total);
    }
    // It saves two tests on the one side and costs one on the other.
    plan.readsTestOutOfLineValuesSecond = 2 * reads[PlaceOfNamedFieldInNamePlan::ValueInExtension] > reads[PlaceOfNamedFieldInNamePlan::Elsewhere] + reads[PlaceOfNamedFieldInNamePlan::DoubleInCell];
    // A double is the last form asked for: that saves six tests and costs one.
    plan.readsTestDoublesSecond = 6 * reads[PlaceOfNamedFieldInNamePlan::DoubleInCell] > reads[PlaceOfNamedFieldInNamePlan::Elsewhere] + reads[PlaceOfNamedFieldInNamePlan::ValueInExtension];
    return plan;
}

// The forms are those of narrowFieldsAtSites (stubs/AOTStubsPropertyKeys.h), each under the representation whose code reads it. One more counter is for every other slot.
static Vector<SlotRepresentation> chooseNarrowFormsOfKeysForNamePlan(const Program& program, const SlotPlanParts& parts, const ParallelLoops& loops)
{
    static constexpr std::array forms { SlotRepresentation::OptionalCell32, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalDouble, SlotRepresentation::OptionalBoolean, SlotRepresentation::OptionalInt64 };
    auto counterOf = [](const PlacedSlot& slot) -> size_t {
        SlotRepresentation read = slot.representation == SlotRepresentation::Cell32 ? SlotRepresentation::OptionalCell32 : slot.representation;
        return slot.isOutOfLine() ? forms.size() : static_cast<size_t>(std::ranges::find(forms, read) - forms.begin());
    };
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    Vector<std::array<uint32_t, forms.size() + 1>> layoutsByKey;
    layoutsByKey.fill({ }, numberOfKeys);
    forEachRangeOfIndicesInParallel(loops, parts.layouts.size(), [&](size_t first, size_t end) {
        forEachNamedSlotOfCompiledObjectsForNamePlan(program, parts, first, end, [&](PropertyKeyID key, const PlacedSlot& slot) {
            if (slot.hasStorage())
                AtomicRef(layoutsByKey[key.index()][counterOf(slot)]).fetch_add(1, std::memory_order_relaxed);
        });
    });
    Vector<SlotRepresentation> chosen;
    chosen.fill(SlotRepresentation::JSValue, numberOfKeys);
    for (unsigned key = 0; key < numberOfKeys; ++key) {
        const auto& layouts = layoutsByKey[key];
        size_t most = static_cast<size_t>(std::ranges::max_element(layouts) - layouts.begin());
        if (most < forms.size() && layouts[most] > layouts[forms.size()])
            chosen[key] = forms[most];
    }
    return chosen;
}
// computeSlotPlanParts() of AOTSlotPlan.cpp calls it behind the layouts, and declares it for itself.
void planNames(const Program&, const MentionsOfKeys&, BuildContext&, SlotPlanParts&);
void planNames(const Program& program, const MentionsOfKeys& mentions, BuildContext& build, SlotPlanParts& parts)
{
    parts.keysWithColumn = chooseKeysWithColumnForNamePlan(program, mentions, build);
    parts.fieldTableProbes = planFieldTableProbesForNamePlan(program, parts, mentions, build.parallelLoops());
    parts.narrowFormsOfKeys = chooseNarrowFormsOfKeysForNamePlan(program, parts, build.parallelLoops());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
