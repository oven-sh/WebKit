/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTObjectWords.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTObjectBuiltinEmitter.h"
#include "AOTPlacement.h"
#include "AOTResultRules.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSort.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsAllocation.h"
#include "AOTUseLists.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include "JSCell.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static MemoryLocation locationOfField(const Program& program, LayoutID layoutID, FieldIndex field)
{
    const Layout& layout = program.at(layoutID);
    if (layout.isEnvironment())
        return MemoryLocation::environmentField(layoutID, field);
    PropertyKeyID key = layout.fields[field].key;
    return key ? MemoryLocation::objectField(key) : MemoryLocation(AbstractHeap::ObjectFields);
}

static void storeZero(Output& out, TypedPointer pointer, unsigned width)
{
    switch (width) {
    case sizeof(uint8_t):
        out.store32As8(out.constInt32(0), pointer);
        return;
    case sizeof(uint16_t):
        out.store32As16(out.constInt32(0), pointer);
        return;
    case sizeof(uint32_t):
        out.store32(out.constInt32(0), pointer);
        return;
    case sizeof(uint64_t):
        out.store64(out.constInt64(0), pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeZeroBytes(Output& out, const B3::AbstractHeap& heap, CellPointer cell, ReservedBytes bytes)
{
    uint32_t end = bytes.offset + bytes.width;
    for (uint32_t offset = bytes.offset; offset < end;) {
        uint32_t width = sizeof(uint64_t);
        while (offset % width || offset + width > end)
            width /= 2;
        storeZero(out, out.address(heap, cell.value(), offset), width);
        offset += width;
    }
}

// NewObject declares no write to object fields, so without this B3 would forward the zero stored at creation past a call that gives the cell an extension.
static void clobberExtensionWord(Output& out, AOTHeaps& heaps)
{
    B3::PatchpointValue* clobber = out.patchpoint(B3::Void);
    clobber->effects = B3::Effects::none();
    heaps.decoratePatchpointWrite(&heaps.entire(AbstractHeap::ObjectFields), clobber);
    clobber->setRunsWithoutFrame(true);
    clobber->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
}

CellPointer AllocationEmitter::allocateFrameCell(const ValueContext& value, AllocationID id, const PlannedLayout& layout)
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    if (!isObjectLayout(m_program.at(allocation.layout).kind))
        return cellAllocation.allocateInFrame(layout.size);

    LBasicBlock hasNoRecord = newBlock();
    Join<Integer> headerOfHeapCells(m_out);
    Pointer record = cellAllocation.allocationSiteRecord(id);
    Integer headerInRecord(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), record.value(), AllocationSiteRecord::offsetOfHeader())));
    branchUnless(isNotZero(headerInRecord), hasNoRecord);
    headerOfHeapCells.add(headerInRecord);

    // The operation fills the record of the site and allocates no cell.
    m_out.appendTo(hasNoRecord);
    headerOfHeapCells.add(callOperation<Integer>(value, RuntimeOperation::GetHeaderOfAllocation, { word32(static_cast<int32_t>(id.index())) }, { .isOnRarePath = true }));
    Integer header = headerOfHeapCells.finish();

    CellPointer cell = cellAllocation.allocateInFrame(layout.size);
    CodeGenerator::InitializationScope scope(generator());
    cellAllocation.storeHeader(cell, header);
    storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(allocation.orderWord)));
    m_out.store32(m_out.constInt32(static_cast<int32_t>(shapeWordOf(allocation.shape))), m_out.address(m_heaps.entire(AbstractHeap::ShapeWords), cell.value(), offsetOfShapeWord));
    return cell;
}

ValueID AllocationEmitter::findCopyThatFills(const ValueContext& value)
{
    if (value.shape() != CodeShape::Fast || value->storage() != AllocationStorage::Heap || value->prototypeOperand() || !value->fields().empty() || generator().validates())
        return { };
    const BlockValues& values = m_function.at(value.block()).values;
    size_t index = values.find(value.id());
    if (index == notFound || index + 1 == values.size())
        return { };
    const Value& next = m_function.at(values[index + 1]);
    if (next.opcode != Opcode::TryCopyFields || next.objectOperand() != value.id())
        return { };
    return values[index + 1];
}

void AllocationEmitter::generateNewObject(const ValueContext& value)
{
    AllocationID allocation = value->allocation();
    if (!allocation) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    if (m_allocationChosenByTable.operation == value.id()) {
        generateNewObjectChosenByTable(value, std::exchange(m_allocationChosenByTable, { }));
        return;
    }

    AllocationStorage storage = value->storage();
    if (ValueID copy = findCopyThatFills(value)) {
        auto allocateEmpty = [&] {
            return allocateWithFields(value, allocation, value->fields(), value->valuesOperands(), storage);
        };
        if (std::optional<CellPointer> cell = emitter<AccessEmitter>().tryAllocateCopyOfFields(value, copy, ScopedLambda<CellPointer()>(allocateEmpty))) {
            define(value, *cell);
            return;
        }
    }
    CellPointer cell = allocateWithFields(value, allocation, value->fields(), value->valuesOperands(), storage);
    if (ValueID prototype = value->prototypeOperand())
        storePrototype(value, cell, prototype);
    else
        define(value, cell);
}

void AllocationEmitter::generateNewDictionary(const ValueContext& value)
{
    // A run of definitions is stored by its site's cache only where the cell has room for all of it.
    unsigned names = 0;
    Vector<ValueID, 4> aliases { value.id() };
    for (size_t index = 0; index < aliases.size(); ++index) {
        for (const Use& use : analyses().useLists().usesOf(aliases[index])) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = m_function.at(use.user);
            if (user.opcode == Opcode::TryPutByIds && user.isDefinition() && user.objectOperand() == aliases[index])
                names += user.keys().size();
            else if (use.user != value.id() && analyses().identityOf(use.user) == value.id())
                aliases.appendIfNotContains(use.user);
        }
    }
    unsigned addedPairsOfSlots = names > JSFinalObject::defaultInlineCapacity ? (names - JSFinalObject::defaultInlineCapacity + 1) / 2 : 0;
    define(value, CellPointer(callStub(value, newDictionary(std::min(addedPairsOfSlots, numberOfRoomierFinalObjects)), { }).first));
}

void AllocationEmitter::generateFinishObject(const ValueContext& value)
{
    define(value, get(value->objectOperand()));
}

void AllocationEmitter::generateFinishBirth(const ValueContext& value)
{
    CellPointer cell = in<CellPointer>(value->objectOperand());
    CodeGenerator::InitializationScope scope(generator());
    Word32 flags = loadByte(cell, m_heaps.JSCell_typeInfoFlags);
    m_out.store32As8(bitAnd(flags, word32(static_cast<uint8_t>(~TypeInfoPerCellBit))).value(), cell.value(), m_heaps.JSCell_typeInfoFlags);
}

void AllocationEmitter::generateTryFinishBirth(const ValueContext& value)
{
    CellPointer cell = in<CellPointer>(value->objectOperand());
    LBasicBlock isUnfinished = newBlock();
    LBasicBlock done = newBlock();
    branch(hasBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit), { normalEdge(isUnfinished), normalEdge(done) });
    m_out.appendTo(isUnfinished);
    callVoidOperation(value, RuntimeOperation::TryFinishBirth, { cell }, { .isOnRarePath = true });
    m_out.jump(done);
    m_out.appendTo(done);
}

AllocationEmitter::InitialFields AllocationEmitter::collectInitialFields(LayoutID layout, std::span<const uint32_t> fields, std::span<const ValueID> values)
{
    RELEASE_ASSERT(fields.size() == values.size());
    InitialFields initialFields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const PlacedSlot& slot = m_placement.slot(layout, fields[i]);
        if (!slot.hasStorage() || !needsStorageFor(slot, typeOf(values[i])))
            continue;
        initialFields.append(InitialField { &slot, fields[i], values[i] });
    }
    return initialFields;
}

CellPointer AllocationEmitter::takeReleasedCellOrAllocate(const ValueContext& value, AllocationID id, uint32_t freeCellOffset)
{
    // Every collection clears the word.
    TypedPointer freeCell = emitter<StaticsEmitter>().inStatics(m_heaps.collectorState, freeCellOffset);
    Integer released(m_out.load64(freeCell));
    return selectByBranch<CellPointer>(isNotZero(released), [&] {
        m_out.store64(m_out.constInt64(0), freeCell);
        return CellPointer(released.value());
    }, [&] {
        return emitter<CellAllocationEmitter>().allocate(value, id);
    });
}

static bool isZeroWhileEmpty(SlotRepresentation representation)
{
    SlotTraits traits = traitsOf(representation);
    return !traits.hasAbsent || !traits.absentBits;
}

ZeroedBy AllocationEmitter::whoZeroesBodyOfFreshCell(const ValueContext& value, AllocationID id, const BitVector& isInitialField, AllocationStorage storage)
{
    LayoutID layout = m_placement.allocation(id).layout;
    if (storage != AllocationStorage::Heap || m_placement.freeCellOffset(layout))
        return ZeroedBy::Nobody;

    const SlotPlan& plan = m_placement.slots();
    unsigned storesOfZero = plan.layout(layout).isEscapedOffset.has_value() + static_cast<unsigned>(m_placement.layout(layout).bytesKeptZero.size());
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) })
        storesOfZero += hiddenWord->hasStorage() && isZeroWhileEmpty(hiddenWord->representation);
    forEachEmptySlot(layout, isInitialField, [&](FieldIndex, const PlacedSlot& slot) {
        storesOfZero += isZeroWhileEmpty(slot.representation);
    });
    return emitter<CellAllocationEmitter>().whoZeroesBody(value, id, storesOfZero);
}

CellPointer AllocationEmitter::allocateCell(const ValueContext& value, AllocationID id, const PlannedLayout& layout, AllocationStorage storage, ZeroedBy zeroedBy)
{
    RELEASE_ASSERT(zeroedBy == ZeroedBy::Nobody || storage == AllocationStorage::Heap);
    switch (storage) {
    case AllocationStorage::Heap:
        if (std::optional<uint32_t> freeCellOffset = m_placement.freeCellOffset(m_placement.allocation(id).layout)) {
            RELEASE_ASSERT(zeroedBy == ZeroedBy::Nobody);
            return takeReleasedCellOrAllocate(value, id, *freeCellOffset);
        }
        return emitter<CellAllocationEmitter>().allocate(value, id, zeroedBy);
    case AllocationStorage::Frame:
    case AllocationStorage::FrameUntilMaterialized:
        return allocateFrameCell(value, id, layout);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AllocationEmitter::initializeHiddenFields(CellPointer cell, LayoutID layout, AllocationStorage storage, ZeroedBy zeroedBy)
{
    // A store whose receiver may also be on the heap runs its barrier, which must not mistake a frame cell for a marked one.
    bool mayReachBarrier = isObjectLayout(m_program.at(layout).kind) || !inputs().frameAllocation.isOnlyInFrames(layout);
    if (storage != AllocationStorage::Heap && mayReachBarrier)
        m_out.store32As8(m_out.constInt32(frameCellState), m_out.address(m_heaps.collectorState, cell.value(), JSCell::cellStateOffset()));

    const SlotPlan& plan = m_placement.slots();
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage() && (zeroedBy == ZeroedBy::Nobody || !isZeroWhileEmpty(hiddenWord->representation)))
            emitter<SlotEmitter>().storeAbsent(m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), hiddenWord->offset), hiddenWord->representation);
    }

    if (std::optional<uint32_t> isEscapedOffset = plan.layout(layout).isEscapedOffset; isEscapedOffset && zeroedBy == ZeroedBy::Nobody)
        m_out.store32As8(m_out.constInt32(0), m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), cell.value(), *isEscapedOffset));
}

void AllocationEmitter::storeInitialFields(CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    for (const InitialField& field : initialFields) {
        if (holdsRawValueFromBirth(m_program, { layout, field.field }))
            slots.markCellIfMarkerIsStored(cell, field.value);
        if (field.slot->isOutOfLine())
            continue;
        MemoryLocation location = locationOfField(m_program, layout, field.field);
        if (field.slot->representation != SlotRepresentation::DeferredClosure)
            slots.storeSlot(cell, *field.slot, location, get(field.value), nullptr, typeOf(field.value));
        else if (isDeferredClosure(m_function, field.value))
            m_out.store32(m_out.constInt32(uncreatedDeferredClosure), slots.addressOf(cell, *field.slot, location));
        else
            m_out.store32(in<NarrowReference>(field.value).value(), slots.addressOf(cell, *field.slot, location));
    }
}

template<typename Visitor>
void AllocationEmitter::forEachEmptySlot(LayoutID layout, const BitVector& isInitialField, const Visitor& visitor)
{
    const PlannedLayout& planned = m_placement.layout(layout);
    bool isCoroutineFrame = m_program.at(layout).kind == LayoutKind::CoroutineFrame;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.storage != SlotStorage::Inline || isInitialField.get(field))
            continue;
        bool isWrittenBeforeRead = isCoroutineFrame && !traitsOf(slot.representation).holdsCell;
        if (!isWrittenBeforeRead)
            visitor(field, slot);
    }
}

Vector<AllocationEmitter::WordOfEmptyObject, 8> AllocationEmitter::wordsOfEmptyObject(LayoutID layout)
{
    Vector<WordOfEmptyObject, 8> words;
    const SlotPlan& plan = m_placement.slots();
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage() && !isZeroWhileEmpty(hiddenWord->representation))
            words.append({ hiddenWord->offset, hiddenWord->representation });
    }
    forEachEmptySlot(layout, BitVector(), [&](FieldIndex, const PlacedSlot& slot) {
        if (!isZeroWhileEmpty(slot.representation))
            words.append({ slot.offset, slot.representation });
    });
    AOT::sort(words, { }, &WordOfEmptyObject::offset);
    return words;
}

void AllocationEmitter::storeEmptySlots(CellPointer cell, LayoutID layout, const BitVector& isInitialField, ZeroedBy zeroedBy)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlannedLayout& planned = m_placement.layout(layout);
    const Layout& declared = m_program.at(layout);
    forEachEmptySlot(layout, isInitialField, [&](FieldIndex field, const PlacedSlot& slot) {
        if (zeroedBy != ZeroedBy::Nobody && isZeroWhileEmpty(slot.representation))
            return;
        TypedPointer address = slots.addressOf(cell, slot, locationOfField(m_program, layout, field));
        SlotTraits traits = traitsOf(slot.representation);
        if (traits.hasAbsent)
            slots.storeAbsent(address, slot.representation);
        else
            storeZero(m_out, address, traits.width);
    });

    if (zeroedBy != ZeroedBy::Nobody)
        return;
    const B3::AbstractHeap& anyFieldHeap = m_heaps.entire(declared.isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields);
    for (ReservedBytes bytes : planned.bytesKeptZero)
        storeZeroBytes(m_out, anyFieldHeap, cell, bytes);
}

void AllocationEmitter::barrierIfCellWasStored(const ValueContext& value, CellPointer cell, const InitialFields& initialFields)
{
    bool mayHaveStoredCell = std::ranges::any_of(initialFields, [&](const InitialField& field) {
        return traitsOf(field.slot->representation).holdsCell && typeOf(field.value).hasCells();
    });
    if (mayHaveStoredCell)
        emitter<BarrierEmitter>().barrier(value, cell);
}

void AllocationEmitter::storeFieldsInExtension(const ValueContext& value, CellPointer cell, LayoutID layout, const InitialFields& initialFields, OwnerBarrier barrier)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlacedSlot& extensionWord = m_placement.slots().extensionWord(layout);
    Vector<ValueID, 8> storedOutOfLine;
    uint64_t granuleBits = 0;
    for (const InitialField& field : initialFields) {
        if (!field.slot->isOutOfLine())
            continue;
        granuleBits |= granuleBitOf(field.slot->offset);
        storedOutOfLine.append(field.value);
    }
    if (!granuleBits)
        return;

    slots.ensureExtensionHolds(value, cell, extensionWord, granuleBits, barrier);
    clobberExtensionWord(m_out, m_heaps);
    {
        CodeGenerator::InitializationScope scope(generator());
        Pointer extension = slots.extensionOf(cell, extensionWord);
        for (const InitialField& field : initialFields) {
            if (field.slot->isOutOfLine())
                slots.storeInExtension(extension, *field.slot, locationOfField(m_program, layout, field.field), get(field.value));
        }
    }
    if (barrier == OwnerBarrier::Emitted)
        emitter<BarrierEmitter>().barrierAfterStores(value, cell, storedOutOfLine.span());
}

// The operation returns the object to use: the prototype of a subclass selects the layout of that subclass.
void AllocationEmitter::storePrototype(const ValueContext& value, CellPointer cell, ValueID prototype, std::optional<Word32> numberOfAllocation)
{
    CellPointer requested = in<CellPointer>(prototype);
    Boxed sitePrototype = loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype);
    Condition hasSitePrototype = equal(boxCell(requested), sitePrototype);
    auto useCell = [&] {
        return cell;
    };
    auto setByOperation = [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::SetConstructPrototype, { cell, requested, numberOfAllocation.value_or(word32(static_cast<int32_t>(value->allocation().index()))), word32(value->fields().empty()) }, { .isOnRarePath = true });
    };
    if (mayAllocateOtherLayout(m_program, value.value())) {
        define(value, either<CellPointer>(hasSitePrototype, useCell, setByOperation));
        return;
    }

    // A released cell is an old one: the collector may have marked it.
    bool mayBeMarked = !!m_placement.freeCellOffset(m_placement.allocation(value->allocation()).layout);

    // Each evaluation of a class and each closure of a constructor function has a prototype object of its own, and the site knows one of them.
    define(value, selectByBranch<CellPointer>(hasSitePrototype, useCell, [&] {
        LBasicBlock isUnknown = newBlock();
        Join<CellPointer> join(m_out);
        Word32 structureID = emitter<AccessEmitter>().findPrototypeTransition(cell, boxCell(requested), isUnknown);
        {
            CodeGenerator::InitializationScope scope(generator());
            emitter<AccessEmitter>().storeStructureAfterPrototypeTransition(cell, structureID);
        }
        if (mayBeMarked)
            emitter<BarrierEmitter>().barrier(value, cell);
        join.add(cell);

        m_out.appendTo(isUnknown);
        join.add(setByOperation());
        return join.finish();
    }));
}

CellPointer AllocationEmitter::allocateWithFields(const ValueContext& value, AllocationID id, std::span<const uint32_t> fields, std::span<const ValueID> values, AllocationStorage storage)
{
    auto failWithoutCell = [&](InternalErrorKind kind) {
        fail(value, kind);
        return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
    };

    LayoutID layoutID = m_placement.allocation(id).layout;
    const PlannedLayout& layout = m_placement.layout(layoutID);
    if (!layout.hasSlots())
        return failWithoutCell(InternalErrorKind::SlotAccessNotPlaced);

    InitialFields initialFields = collectInitialFields(layoutID, fields, values);
    BitVector isInitialField(layout.numberOfFields());
    for (const InitialField& field : initialFields) {
        bool defersClosure = field.slot->representation == SlotRepresentation::DeferredClosure;
        Representation wanted = defersClosure ? Representation::Cell32 : Typed::representationOf(field.slot->representation);
        bool hasNoValue = defersClosure && isDeferredClosure(m_function, field.value);
        if (!hasNoValue && representationOf(field.value) != wanted)
            return failWithoutCell(InternalErrorKind::WrongRepresentation);
        if (!field.slot->isOutOfLine())
            isInitialField.set(field.field);
    }

    ZeroedBy zeroedBy = whoZeroesBodyOfFreshCell(value, id, isInitialField, storage);
    CellPointer cell = allocateCell(value, id, layout, storage, zeroedBy);
    {
        CodeGenerator::InitializationScope scope(generator());
        initializeHiddenFields(cell, layoutID, storage, zeroedBy);
        storeInitialFields(cell, layoutID, initialFields);
        storeEmptySlots(cell, layoutID, isInitialField, zeroedBy);
    }
    if (storage == AllocationStorage::Heap) {
        if (m_placement.freeCellOffset(layoutID))
            barrierIfCellWasStored(value, cell, initialFields);
        emitter<BarrierEmitter>().fenceAfterInitialization();
    }
    // Nothing collects between the allocation of a cell and that of its Extension. A released cell is an old one.
    storeFieldsInExtension(value, cell, layoutID, initialFields, m_placement.freeCellOffset(layoutID) ? OwnerBarrier::Emitted : OwnerBarrier::Omitted);
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
