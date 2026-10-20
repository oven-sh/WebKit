/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotRunEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTLayout.h"
#include "AOTMemoryLocation.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned slotKeysPerWord = std::numeric_limits<uint64_t>::digits / bitsPerSlotKey;

static_assert(slotKeyFor(outOfLineSlotKey - 1, true) <= slotKeyMask);

struct SlotRunEmitter::Entry {
    const PlacedSlot& slot() const { return access->slot; }
    MemoryLocation location() const { return MemoryLocation::objectField(access->key, access->receivers); }
    bool storeMayCollect() const { return slot().representation == SlotRepresentation::Int32OrDouble; }

    const FieldAccess* access;
    ValueID value;
    bool mayCreate;
    bool isExpectedToCreate;
};

struct SlotRunEmitter::Run {
    Vector<PropertyKeyID, 4> createdKeys() const
    {
        Vector<PropertyKeyID, 4> keys;
        for (const Entry& entry : entries) {
            if (entry.mayCreate)
                keys.append(entry.access->key);
        }
        return keys;
    }

    bool isExpectedToCreate() const
    {
        return std::ranges::all_of(entries, [](const Entry& entry) {
            return !entry.mayCreate || entry.isExpectedToCreate;
        });
    }

    const Entry* findOutOfLineEntry() const
    {
        auto found = std::ranges::find_if(entries, [](const Entry& entry) {
            return entry.slot().isOutOfLine();
        });
        return found == entries.end() ? nullptr : &*found;
    }

    uint64_t granuleBits() const
    {
        uint64_t bits = 0;
        for (const Entry& entry : entries) {
            if (entry.slot().isOutOfLine())
                bits |= granuleBitOf(entry.slot().offset);
        }
        return bits;
    }

    std::optional<Vector<uint64_t, 4>> packCreatedSlotKeys() const
    {
        Vector<uint64_t, 4> words;
        unsigned count = 0;
        for (const Entry& entry : entries) {
            if (!entry.mayCreate)
                continue;
            uint32_t slotKey = slotKeyFor(entry.slot().offset, entry.slot().isOutOfLine());
            if (slotKey == noSlotKey)
                return std::nullopt;
            unsigned positionInWord = count++ % slotKeysPerWord;
            if (!positionInWord)
                words.append(0);
            words.last() |= static_cast<uint64_t>(slotKey) << (bitsPerSlotKey * positionInWord);
        }
        return words;
    }

    Vector<Entry, 4> entries;
};

static bool isPlacedStore(const FieldAccess& access)
{
    switch (access.coverage) {
    case FieldAccess::Coverage::EveryMember:
        return access.isStore && access.slot.hasStorage() && access.slot.representation != SlotRepresentation::DeferredClosure;
    case FieldAccess::Coverage::NoMember:
    case FieldAccess::Coverage::SomeMembers:
    case FieldAccess::Coverage::EveryMemberHasSlot:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

SlotRunEmitter::SlotRunEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void SlotRunEmitter::generatePutSlots(const ValueContext& value)
{
    PayloadListView<PlacedKey> placedKeys = value->placedKeys();
    std::span<const ValueID> stored = value->valuesOperands();
    RELEASE_ASSERT(placedKeys.size() == stored.size());

    SlotAccessEmitter& slotAccess = emitter<SlotAccessEmitter>();
    Run run;
    for (size_t index = 0; index < stored.size(); ++index) {
        PlacedKey placedKey = placedKeys[index];
        if (!placedKey.fieldAccess) {
            fail(value, InternalErrorKind::PlacedPayloadMissing);
            return;
        }
        const FieldAccess& access = m_placement.fieldAccess(placedKey.fieldAccess);
        if (!isPlacedStore(access)) {
            fail(value, InternalErrorKind::SlotAccessNotPlaced);
            return;
        }
        run.entries.append({ &access, stored[index], placedKey.mayAdd && slotAccess.mayCreateListedField(access), slotAccess.isExpectedToCreate(placedKey.accessSite) });
    }
    storeRun(value, in<CellPointer>(value->objectOperand()), run);
}

void SlotRunEmitter::storeRun(const ValueContext& value, CellPointer cell, const Run& run)
{
    RELEASE_ASSERT(!value->laterBarrierCovers());
    SlotEmitter& slots = emitter<SlotEmitter>();

    if (const Entry* outOfLine = run.findOutOfLineEntry())
        slots.ensureExtensionHolds(value, cell, outOfLine->access->extensionWord, run.granuleBits());

    // The keys are listed first, while the slots still say which fields are missing. No code of the program runs before the stores.
    listMissingFields(value, cell, run);

    // A store that may collect precedes every store of a cell, so that nothing collects between such a store and the barrier.
    for (const Entry& entry : run.entries) {
        if (entry.storeMayCollect())
            slots.storeSlot(value, cell, entry.slot(), entry.location(), get(entry.value), &entry.access->extensionWord);
    }
    std::optional<Pointer> extension;
    for (const Entry& entry : run.entries) {
        if (entry.storeMayCollect())
            continue;
        if (!entry.slot().isOutOfLine()) {
            slots.storeSlot(cell, entry.slot(), entry.location(), get(entry.value));
            continue;
        }
        if (!extension)
            extension = slots.extensionOf(cell, entry.access->extensionWord);
        slots.storeInExtension(*extension, entry.slot(), entry.location(), get(entry.value));
    }
    emitter<BarrierEmitter>().barrierAfterStores(value, cell, value->valuesOperands());
}

Condition SlotRunEmitter::isMissing(CellPointer cell, const Entry& entry)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    return slots.holdsAbsent(slots.addressOf(cell, entry.slot(), entry.location(), &entry.access->extensionWord), entry.slot().representation);
}

void SlotRunEmitter::listMissingFields(const ValueContext& value, CellPointer cell, const Run& run)
{
    Vector<PropertyKeyID, 4> createdKeys = run.createdKeys();
    if (createdKeys.isEmpty())
        return;

    SlotAccessEmitter& slotAccess = emitter<SlotAccessEmitter>();
    auto listFieldsIfAnyIsMissing = [&](KeyOrderPlaces places) {
        LBasicBlock creates = newBlock();
        LBasicBlock continuation = newBlock();
        for (const Entry& entry : run.entries) {
            if (!entry.mayCreate)
                continue;
            LBasicBlock isFilled = newBlock();
            branch(isMissing(cell, entry), { unlikelyEdge(creates), normalEdge(isFilled) });
            m_out.appendTo(isFilled);
        }
        m_out.jump(continuation);

        m_out.appendTo(creates);
        slotAccess.takeKeyOrderStepOr(cell, slotAccess.testKeyOrderStep(cell, value->receivers(), createdKeys.span(), places), [&] {
            listFieldsInRuntime(value, cell, run);
        });
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    };

    // The order word alone says that every field of the run is missing, so the slots are tested only where the step does not apply.
    std::optional<KeyOrderStep> stepTestedFirst;
    if (run.isExpectedToCreate())
        stepTestedFirst = slotAccess.testKeyOrderStep(cell, value->receivers(), createdKeys.span(), KeyOrderPlaces::MostLikely);
    slotAccess.takeKeyOrderStepOr(cell, stepTestedFirst, [&] {
        listFieldsIfAnyIsMissing(stepTestedFirst ? KeyOrderPlaces::Others : KeyOrderPlaces::All);
    });
}

void SlotRunEmitter::listFieldsInRuntime(const ValueContext& value, CellPointer cell, const Run& run)
{
    SlotAccessEmitter& slotAccess = emitter<SlotAccessEmitter>();
    if (std::ranges::count_if(run.entries, &Entry::mayCreate) == 1) {
        const Entry& created = *std::ranges::find_if(run.entries, &Entry::mayCreate);
        slotAccess.didCreateField(value, cell, *created.access);
        return;
    }

    // A call takes the keys of one word, in the sequence of the run, and passes over a key that the object lists already.
    if (std::optional<Vector<uint64_t, 4>> words = run.packCreatedSlotKeys()) {
        for (uint64_t word : *words) {
            Integer slotKeys = integer(static_cast<int64_t>(word));
            if (value.shape() == CodeShape::Compact)
                callVoidOperation(value, RuntimeOperation::DidCreateFieldsAt, { cell, slotKeys });
            else
                callStub(value, didCreateField(CreatedFields::Run), { cell.value(), slotKeys.value() });
        }
        return;
    }

    for (const Entry& entry : run.entries) {
        if (!entry.mayCreate)
            continue;
        LBasicBlock creates = newBlock();
        LBasicBlock next = newBlock();
        branch(isMissing(cell, entry), { normalEdge(creates), normalEdge(next) });
        m_out.appendTo(creates);
        slotAccess.didCreateField(value, cell, *entry.access);
        m_out.jump(next);
        m_out.appendTo(next);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
