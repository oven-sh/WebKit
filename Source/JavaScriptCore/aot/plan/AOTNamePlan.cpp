/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
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

namespace JSC { namespace AOT { namespace Typed {

static_assert(!RowDisplacementTable::hasColumn(0) && RowDisplacementTable::hasColumn(RowDisplacementTable::maxColumn) && FieldTableEntry::hasColumn(RowDisplacementTable::maxColumn));

MentionedKeys findMentionedKeys(const Program&, const Facts&, const ParallelLoops&);
MentionedKeys findMentionedKeys(const Program& program, const Facts& facts, const ParallelLoops& loops)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    MentionedKeys mentioned;
    mentioned.isMentioned.fill(0, numberOfKeys);
    const LinkTables& link = facts.linked();
    forEachRangeOfIndicesInParallel(loops, program.functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            FunctionID id(index);
            if (!link.function(id).isReached)
                continue;
            const ThinSummary& summary = facts.summaries().at(id);
            for (std::span<const PropertyKeyID> names : { summary.namesRead.span(), summary.namesCalled.span(), summary.namesStored.span() }) {
                for (PropertyKeyID key : names) {
                    if (key.index() < numberOfKeys)
                        AtomicRef(mentioned.isMentioned[key.index()]).store(1, std::memory_order_relaxed);
                }
            }
        }
    });
    for (const LinkedAccess& access : link.accessesUnderOneKey) {
        if (access.key.index() < numberOfKeys)
            mentioned.isMentioned[access.key.index()] = 1;
    }
    for (size_t text : facts.summaries().textsSpelledThatNameFieldsNeverWritten()) {
        std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = StringID(static_cast<uint32_t>(text)) });
        if (key && key->index() < numberOfKeys)
            mentioned.isSpelledAsText.set(key->index());
    }
    return mentioned;
}

static BitVector chooseKeysWithColumnForNamePlan(const Program& program, const MentionedKeys& mentioned, BuildContext& build)
{
    size_t numberOfColumns = static_cast<size_t>(std::min<uint64_t>(build.limit(LimitID::LookupTableNames), RowDisplacementTable::maxColumn));
    uint32_t numberOfKeys = static_cast<uint32_t>(mentioned.isMentioned.size());
    BitVector keysWithColumn(numberOfKeys);

    Vector<uint32_t> mentionedKeys;
    for (uint32_t key = 0; key < numberOfKeys; ++key) {
        if (mentioned.isMentioned[key] && program.types.hasStableNumber(PropertyKeyID(key)))
            mentionedKeys.append(key);
    }
    if (mentionedKeys.size() > numberOfColumns)
        mentionedKeys.shrink(numberOfColumns);
    for (uint32_t key : mentionedKeys)
        keysWithColumn.quickSet(key);

    size_t columnsLeft = numberOfColumns - mentionedKeys.size();
    for (uint32_t key = 0; key < numberOfKeys && columnsLeft; ++key) {
        PropertyKeyID id(key);
        if (mentioned.isMentioned[key] || !program.types.hasStableNumber(id) || !program.builtinNameOf(id))
            continue;
        keysWithColumn.quickSet(key);
        --columnsLeft;
    }
    return keysWithColumn;
}

static void forEachNamedSlotOfCompiledObjectsForNamePlan(const Program& program, const SlotPlan& slots, LayoutID id, const Invocable<void(PropertyKeyID, const PlacedSlot&)> auto& functor)
{
    const PlannedLayout& planned = slots.layout(id);
    if (!planned.holdsCompiledObjects() || slots.isNeverAllocated(id))
        return;
    const Vector<Field>& declared = program.at(id).fields;
    unsigned numberDeclared = planned.numberOfFields() - static_cast<unsigned>(planned.appendedKeys.size());
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        bool isDeclared = field < numberDeclared && field < declared.size();
        PropertyKeyID key = isDeclared ? declared[field].key : field >= numberDeclared ? planned.appendedKeys[field - numberDeclared] : PropertyKeyID();
        if (key)
            functor(key, planned.slots[field]);
    }
}

static ModulesThatWriteFields findModulesThatWriteFieldsForNamePlan(const Program& program, const LayoutsByNames& layouts, const SlotPlan& slots)
{
    size_t numberOfKeys = program.types.numberOfPropertyKeys();
    constexpr uint32_t mostLayoutsWithFlag = 2;
    Vector<uint32_t> modulesSeenUpTo;
    Vector<uint32_t> entriesOfKeys;
    modulesSeenUpTo.fill(0, numberOfKeys);
    entriesOfKeys.fill(0, numberOfKeys);
    Vector<uint64_t> written;
    Vector<uint32_t> inlineLayouts;
    for (uint32_t module = 0; module < program.modules.size(); ++module) {
        for (uint32_t layout : layouts.layoutsWrittenIn(ModuleID(module))) {
            forEachNamedSlotOfCompiledObjectsForNamePlan(program, slots, LayoutID(layout), [&](PropertyKeyID key, const PlacedSlot& slot) {
                if (std::exchange(modulesSeenUpTo[key.index()], module + 1) != module + 1) {
                    entriesOfKeys[key.index()] = static_cast<uint32_t>(written.size());
                    written.append(static_cast<uint64_t>(key.index()) << 32 | module);
                    inlineLayouts.append(0);
                }
                inlineLayouts[entriesOfKeys[key.index()]] += slot.storage == SlotStorage::Inline;
            });
        }
    }
    ModulesThatWriteFields result;
    Vector<uint32_t>& first = result.firstOfKeys;
    first.fill(0, numberOfKeys + 1);
    for (uint64_t pair : written)
        ++first[(pair >> 32) + 1];
    for (size_t next = 1; next <= numberOfKeys; ++next)
        first[next] += first[next - 1];
    result.modules.grow(written.size());
    result.inlineFields.grow(written.size());
    for (size_t entry = 0; entry < written.size(); ++entry) {
        uint32_t place = first[written[entry] >> 32]++;
        result.modules[place] = static_cast<uint32_t>(written[entry]);
        result.inlineFields[place] = !inlineLayouts[entry] ? InlineFieldsOfModule::None : inlineLayouts[entry] <= mostLayoutsWithFlag ? InlineFieldsOfModule::Few : InlineFieldsOfModule::Many;
    }
    for (size_t next = numberOfKeys; next; --next)
        first[next] = first[next - 1];
    first[0] = 0;
    return result;
}

void planKeys(const Program&, const MentionedKeys&, BuildContext&, SlotPlanParts&);
void planKeys(const Program& program, const MentionedKeys& mentioned, BuildContext& build, SlotPlanParts& parts)
{
    parts.keysWithColumn = chooseKeysWithColumnForNamePlan(program, mentioned, build);
    for (const Layout& layout : program.layouts) {
        for (const Field& field : layout.fields) {
            if (field.key && field.form.contains(FieldForm::IsAccessor))
                parts.keysOfAccessors.set(field.key.index());
        }
    }
}

ModulesThatWriteFields findModulesThatWriteFields(const Program&, const Facts&, const SlotPlan&, const ParallelLoops&);
ModulesThatWriteFields findModulesThatWriteFields(const Program& program, const Facts& facts, const SlotPlan& slots, const ParallelLoops& loops)
{
    if (facts.mechanisms().contains(Mechanism::Guesses))
        return findModulesThatWriteFieldsForNamePlan(program, facts.layoutsByNames(), slots);
    return findModulesThatWriteFieldsForNamePlan(program, LayoutsByNames::thatListsWritersAlone(program, facts.summaries(), loops), slots);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
