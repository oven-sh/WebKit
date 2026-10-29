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
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned slotKeysPerWord = std::numeric_limits<uint64_t>::digits / bitsPerSlotKey;

static_assert(slotKeyFor(outOfLineSlotKey - 1, true) <= slotKeyMask);

struct SlotRunEmitter::Entry {
    const PlacedSlot& slot() const { return access->slot; }
    MemoryLocation location() const { return MemoryLocation::objectField(access->key, access->receivers); }
    bool needsGranule() const { return slot().isOutOfLine() && needsStorage; }

    const FieldAccess* access;
    ValueID value;
    bool mayCreate;
    bool isExpectedToCreate;
    bool needsStorage;
    // The slot is in the cell, or in a granule that the run sees to before it stores.
    bool hasStorage { false };
};

struct SlotRunEmitter::Run {
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
        run.entries.append({ &access, stored[index], placedKey.mayAdd && slotAccess.mayCreateListedField(access), slotAccess.isExpectedToCreate(placedKey.accessSite), needsStorageFor(access.slot, typeOf(stored[index])) });
    }
    uint64_t granuleBits = 0;
    for (const Entry& entry : run.entries) {
        if (entry.needsGranule())
            granuleBits |= granuleBitOf(entry.slot().offset);
    }
    for (Entry& entry : run.entries)
        entry.hasStorage = !entry.slot().isOutOfLine() || granuleBits & granuleBitOf(entry.slot().offset);

    CellPointer cell = in<CellPointer>(value->objectOperand());
    SlotEmitter& slots = emitter<SlotEmitter>();
    if (auto entry = std::ranges::find_if(run.entries, &Entry::needsGranule); entry != run.entries.end()) {
        uint64_t granuleBitsBehind = slotAccess.granuleBitsOfStoresBehind(value, *entry->access);
        slots.ensureExtensionHolds(value, cell, entry->access->extensionWord, granuleBits, granuleBits | granuleBitsBehind, emitter<BarrierEmitter>().barrierOfExtension(value));
    }

    // The keys are listed first, while the slots still say which fields are missing. No code of the program runs before the stores.
    std::optional<StateOfNewObject> newObject = slotAccess.stateOfNewObjectBefore(value, value->objectOperand());
    listMissingFields(value, cell, run, newObject);

    for (const Entry& entry : run.entries)
        slotAccess.markCellIfMarkerReplacesRawValue(cell, *entry.access, entry.value);

    std::optional<Pointer> extension;
    Vector<const Entry*, 4> storedWhereGranulesAre;
    for (const Entry& entry : run.entries) {
        if (!entry.slot().isOutOfLine()) {
            slots.storeSlot(cell, entry.slot(), entry.location(), get(entry.value), nullptr, typeOf(entry.value));
            continue;
        }
        if (!entry.hasStorage) {
            storedWhereGranulesAre.append(&entry);
            continue;
        }
        if (!extension)
            extension = slots.extensionOf(cell, entry.access->extensionWord);
        slots.storeInExtension(*extension, entry.slot(), entry.location(), get(entry.value));
    }

    // An object that is filled for the first time has no Extension, and one test says so for all the granules that are then not there.
    LBasicBlock allStored = nullptr;
    if (newObject && !newObject->mayHaveExtension && !extension)
        storedWhereGranulesAre.shrink(0);
    if (!storedWhereGranulesAre.isEmpty() && !extension && value.shape() != CodeShape::Compact) {
        const PlacedSlot& extensionWord = storedWhereGranulesAre[0]->access->extensionWord;
        LBasicBlock hasExtension = newBlock();
        allStored = newBlock();
        branch(slots.hasNoExtension(cell, extensionWord), { normalEdge(allStored), unlikelyEdge(hasExtension) });
        m_out.appendTo(hasExtension);
    }
    for (const Entry* entry : storedWhereGranulesAre)
        slots.storeIfExtensionHolds(cell, entry->slot(), entry->location(), get(entry->value), entry->access->extensionWord);
    if (allStored) {
        m_out.jump(allStored);
        m_out.appendTo(allStored);
    }
    emitter<BarrierEmitter>().barrierAfterStores(value, cell, value->valuesOperands());
}

// A field without storage may be listed already. The run time passes over a key that the object lists.
Condition SlotRunEmitter::isMissing(CellPointer cell, const Entry& entry)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    if (!entry.hasStorage)
        return logicalNot(slots.holdsValue(cell, entry.slot(), entry.location(), &entry.access->extensionWord));
    return slots.holdsAbsent(slots.addressOf(cell, entry.slot(), entry.location(), &entry.access->extensionWord), entry.slot().representation);
}

void SlotRunEmitter::listMissingFields(const ValueContext& value, CellPointer cell, const Run& run, const std::optional<StateOfNewObject>& newObject)
{
    Vector<PropertyKeyID, 4> createdKeys;
    bool isExpectedToCreate = true;
    for (const Entry& entry : run.entries) {
        if (!entry.mayCreate)
            continue;
        createdKeys.append(entry.access->key);
        isExpectedToCreate &= entry.isExpectedToCreate;
    }
    if (createdKeys.isEmpty())
        return;

    SlotAccessEmitter& slotAccess = emitter<SlotAccessEmitter>();
    auto listFieldsIfAnyIsMissing = [&](const std::optional<RememberedKeyOrderStep>& stepThatDoesNotApply) {
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
        slotAccess.takeRememberedKeyOrderStepOr(value, cell, stepThatDoesNotApply, [&](std::optional<uint32_t> placePlusOne) {
            return listFieldsInRuntime(value, cell, run, placePlusOne);
        });
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    };

    // The order word alone says that every field of the run is missing, so the slots are tested only where the step does not apply.
    slotAccess.listCreatedFields(value, cell, value->receivers(), createdKeys.span(), newObject, isExpectedToCreate, listFieldsIfAnyIsMissing);
}

bool SlotRunEmitter::listFieldsInRuntime(const ValueContext& value, CellPointer cell, const Run& run, std::optional<uint32_t> placePlusOne)
{
    SlotAccessEmitter& slotAccess = emitter<SlotAccessEmitter>();
    if (std::ranges::count_if(run.entries, &Entry::mayCreate) == 1) {
        const Entry& created = *std::ranges::find_if(run.entries, &Entry::mayCreate);
        return slotAccess.didCreateField(value, cell, *created.access, created.needsStorage, placePlusOne);
    }

    // A call takes the keys of one word, in the sequence of the run, and passes over a key that the object lists already.
    if (std::optional<Vector<uint64_t, 4>> words = run.packCreatedSlotKeys()) {
        if (placePlusOne && words->size() == 1) {
            callStub(value, didCreateFieldAtPlace(CreatedFieldsAtPlace::Run), { cell.value(), integer(static_cast<int64_t>(words->first())).value(), m_out.constInt64(*placePlusOne) });
            return true;
        }
        for (uint64_t word : *words) {
            Integer slotKeys = integer(static_cast<int64_t>(word));
            callStub(value, didCreateField(CreatedFields::Run), { cell.value(), slotKeys.value() });
        }
        return false;
    }

    for (const Entry& entry : run.entries) {
        if (!entry.mayCreate)
            continue;
        LBasicBlock creates = newBlock();
        LBasicBlock next = newBlock();
        branch(isMissing(cell, entry), { normalEdge(creates), normalEdge(next) });
        m_out.appendTo(creates);
        slotAccess.didCreateField(value, cell, *entry.access, entry.needsStorage);
        m_out.jump(next);
        m_out.appendTo(next);
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
