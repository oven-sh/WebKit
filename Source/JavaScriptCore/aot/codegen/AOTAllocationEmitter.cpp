/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTAllocationSiteRecord.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTFrameAllocation.h"
#include "AOTPlacement.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include "JSCell.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

static MemoryLocation locationOfField(const Program& program, LayoutID layoutID, FieldIndex field)
{
    const Layout& layout = program.at(layoutID);
    if (layout.isEnvironment())
        return MemoryLocation::environmentField(layoutID, field);
    PropertyKeyID key = layout.fields[field].key;
    return key ? MemoryLocation::objectField(key) : MemoryLocation(AbstractHeap::ObjectFields);
}

static bool isListedWithoutStorage(const PlacedSlot& slot, const Type& stored)
{
    return slot.isOutOfLine() && slot.mayExistWithoutStorage && !stored.isNever() && stored.hasOnlyPrimitives(Primitive::Undefined);
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

namespace {

class FrameCellEmitter final : public OperationEmitter {
public:
    explicit FrameCellEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    CellPointer allocate(const ValueContext&, AllocationID, const PlannedLayout&);

private:
    Integer loadHeapCellHeader(const ValueContext&, AllocationID);
};

Integer FrameCellEmitter::loadHeapCellHeader(const ValueContext& value, AllocationID id)
{
    LBasicBlock hasNoRecord = newBlock();
    Join<Integer> header(m_out);
    Pointer record = emitter<CellAllocationEmitter>().allocationSiteRecord(id);
    Integer headerInRecord(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), record.value(), AllocationSiteRecord::offsetOfHeader())));
    branchUnless(isNotZero(headerInRecord), hasNoRecord);
    header.add(headerInRecord);

    // The operation fills the record of the site, and the cell it returns has the header that the record now holds.
    m_out.appendTo(hasNoRecord);
    CellPointer heapCell = callOperation<CellPointer>(value, RuntimeOperation::AllocateSlow, { word32(static_cast<int32_t>(id.index())) }, { .isOnRarePath = true });
    header.add(loadField<Integer>(heapCell, m_heaps.JSCell_header));
    return header.finish();
}

CellPointer FrameCellEmitter::allocate(const ValueContext& value, AllocationID id, const PlannedLayout& layout)
{
    const PlacedAllocation& allocation = m_placement.allocation(id);
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    if (!isObjectLayout(m_program.at(allocation.layout).kind))
        return cellAllocation.allocateInFrame(layout.size);

    Integer header = loadHeapCellHeader(value, id);
    CellPointer cell = cellAllocation.allocateInFrame(layout.size);
    CodeGenerator::InitializationScope scope(generator());
    cellAllocation.storeHeader(cell, header);
    storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(allocation.orderWord)));
    m_out.store32(m_out.constInt32(static_cast<int32_t>(shapeWordOf(allocation.shape))), m_out.address(m_heaps.entire(AbstractHeap::ShapeWords), cell.value(), offsetOfShapeWord));
    return cell;
}

} // anonymous namespace

AllocationEmitter::AllocationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void AllocationEmitter::generateNewObject(const ValueContext& value)
{
    AllocationID allocation = value->allocation();
    if (!allocation) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    AllocationStorage storage = value->storage();
    CellPointer cell = allocateWithFields(value, allocation, value->fields(), value->valuesOperands(), storage);
    ValueID prototype = value->prototypeOperand();
    if (!prototype) {
        define(value, cell);
        return;
    }
    storePrototype(value, cell, prototype);
}

void AllocationEmitter::generateFinishObject(const ValueContext& value)
{
    define(value, get(value->objectOperand()));
}

AllocationEmitter::InitialFields AllocationEmitter::collectInitialFields(LayoutID layout, std::span<const uint32_t> fields, std::span<const ValueID> values)
{
    RELEASE_ASSERT(fields.size() == values.size());
    InitialFields initialFields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const PlacedSlot& slot = m_placement.slot(layout, fields[i]);
        bool isStoreElided = !slot.hasStorage() || slot.representation == SlotRepresentation::DeferredClosure;
        if (isStoreElided || isListedWithoutStorage(slot, typeOf(values[i])))
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
    LBasicBlock reused = newBlock();
    LBasicBlock fresh = newBlock();
    Join<CellPointer> join(m_out);
    branch(isNotZero(released), { normalEdge(reused), normalEdge(fresh) });

    m_out.appendTo(reused);
    m_out.store64(m_out.constInt64(0), freeCell);
    join.add(CellPointer(released.value()));

    m_out.appendTo(fresh);
    join.add(emitter<CellAllocationEmitter>().allocate(value, id));
    return join.finish();
}

CellPointer AllocationEmitter::allocateCell(const ValueContext& value, AllocationID id, const PlannedLayout& layout, AllocationStorage storage)
{
    switch (storage) {
    case AllocationStorage::Heap:
        if (std::optional<uint32_t> freeCellOffset = m_placement.freeCellOffset(m_placement.allocation(id).layout))
            return takeReleasedCellOrAllocate(value, id, *freeCellOffset);
        return emitter<CellAllocationEmitter>().allocate(value, id);
    case AllocationStorage::Frame:
    case AllocationStorage::FrameUntilMaterialized:
        return FrameCellEmitter(generator()).allocate(value, id, layout);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AllocationEmitter::initializeHiddenFields(CellPointer cell, LayoutID layout, AllocationStorage storage)
{
    // A store whose receiver may also be on the heap runs its barrier, which must not mistake a frame cell for a marked one.
    // Only an environment whose layout is never on the heap is out of reach of such a store.
    bool mayReachBarrier = isObjectLayout(m_program.at(layout).kind) || !inputs().frameAllocation.isOnlyInFrames(layout);
    if (storage != AllocationStorage::Heap && mayReachBarrier)
        m_out.store32As8(m_out.constInt32(frameCellState), m_out.address(m_heaps.collectorState, cell.value(), JSCell::cellStateOffset()));

    const SlotPlan& plan = m_placement.slots();
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage())
            emitter<SlotEmitter>().storeAbsent(m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), hiddenWord->offset), hiddenWord->representation);
    }

    if (std::optional<uint32_t> isEscapedOffset = plan.layout(layout).isEscapedOffset)
        m_out.store32As8(m_out.constInt32(0), m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), cell.value(), *isEscapedOffset));
}

void AllocationEmitter::storeInitialFields(const ValueContext&, CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlacedSlot& extensionWord = m_placement.slots().extensionWord(layout);
    for (const InitialField& field : initialFields) {
        const PlacedSlot& slot = *field.slot;
        if (isStoredInCell(slot, typeOf(field.value)))
            slots.storeSlot(cell, slot, locationOfField(m_program, layout, field.field), get(field.value), slot.needsExtension() ? &extensionWord : nullptr);
    }
}

void AllocationEmitter::storeEmptySlots(CellPointer cell, LayoutID layout, const BitVector& isInitialField)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlannedLayout& planned = m_placement.layout(layout);
    const Layout& declared = m_program.at(layout);
    bool isCoroutineFrame = declared.kind == LayoutKind::CoroutineFrame;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.storage != SlotStorage::Inline || isInitialField.get(field))
            continue;
        SlotTraits traits = traitsOf(slot.representation);
        bool isWrittenBeforeRead = isCoroutineFrame && !traits.holdsCell;
        if (isWrittenBeforeRead)
            continue;
        TypedPointer address = slots.addressOf(cell, slot, locationOfField(m_program, layout, field));
        if (traits.hasAbsent)
            slots.storeAbsent(address, slot.representation);
        else
            storeZero(m_out, address, traits.width);
    }

    const B3::AbstractHeap& anyFieldHeap = m_heaps.entire(declared.isEnvironment() ? AbstractHeap::EnvironmentFields : AbstractHeap::ObjectFields);
    for (ReservedBytes bytes : planned.bytesKeptZero)
        storeZeroBytes(m_out, anyFieldHeap, cell, bytes);
}

void AllocationEmitter::emitBarrierIfReleased(const ValueContext& value, CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    if (!m_placement.freeCellOffset(layout))
        return;
    bool mayHaveStoredCell = std::ranges::any_of(initialFields, [&](const InitialField& field) {
        return traitsOf(field.slot->representation).holdsCell && typeOf(field.value).hasCells();
    });
    if (mayHaveStoredCell)
        emitter<BarrierEmitter>().barrier(value, cell);
}

void AllocationEmitter::storeFieldsInExtension(const ValueContext& value, CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlacedSlot& extensionWord = m_placement.slots().extensionWord(layout);
    Vector<ValueID, 8> storedOutOfLine;
    uint64_t granuleBits = 0;
    for (const InitialField& field : initialFields) {
        const PlacedSlot& slot = *field.slot;
        if (isStoredInCell(slot, typeOf(field.value)))
            continue;
        if (slot.isOutOfLine()) {
            granuleBits |= granuleBitOf(slot.offset);
            storedOutOfLine.append(field.value);
            continue;
        }
        if (!slot.doubleOffsetInExtension) {
            fail(value, InternalErrorKind::SlotAccessNotPlaced);
            continue;
        }

        Operand stored = get(field.value);
        TypeID inlineNumberType = types().intern(Type::integer(inlineInt32Range).withPrimitives(markerPrimitives));
        const TypeTestPlan& inlineNumberTest = typeTestPlanFor(m_function.at(field.value).type, stored.representation, inlineNumberType);
        ifElse(hasType(stored, inlineNumberTest), [] { }, [&] {
            slots.ensureExtensionHolds(value, cell, extensionWord, granuleBitOf(*slot.doubleOffsetInExtension));
        });
        clobberExtensionWord(m_out, m_heaps);
        CodeGenerator::InitializationScope scope(generator());
        slots.storeSlot(cell, slot, locationOfField(m_program, layout, field.field), stored, &extensionWord);
    }
    if (!granuleBits)
        return;

    slots.ensureExtensionHolds(value, cell, extensionWord, granuleBits);
    clobberExtensionWord(m_out, m_heaps);
    {
        CodeGenerator::InitializationScope scope(generator());
        Pointer extension = slots.extensionOf(cell, extensionWord);
        for (const InitialField& field : initialFields) {
            if (!field.slot->isOutOfLine())
                continue;
            MemoryLocation location = locationOfField(m_program, layout, field.field);
            if (field.slot->representation == SlotRepresentation::Int32OrDouble)
                slots.storeSlot(cell, *field.slot, location, get(field.value), &extensionWord);
            else
                slots.storeInExtension(extension, *field.slot, location, get(field.value));
        }
    }
    emitter<BarrierEmitter>().barrierAfterStores(value, cell, storedOutOfLine.span());
}

// The operation returns the object to use: the prototype of a subclass selects the layout of that subclass.
void AllocationEmitter::storePrototype(const ValueContext& value, CellPointer cell, ValueID prototype)
{
    CellPointer requested = in<CellPointer>(prototype);
    Boxed sitePrototype = loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype);
    Condition hasSitePrototype = equal(boxCell(requested), sitePrototype);
    auto useCell = [&] {
        return cell;
    };
    auto setByOperation = [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::SetConstructPrototype, { cell, requested, word32(static_cast<int32_t>(value->allocation().index())) }, { .isOnRarePath = true });
    };
    if (mayAllocateOtherLayout(m_program, value.value())) {
        define(value, either<CellPointer>(hasSitePrototype, useCell, setByOperation));
        return;
    }

    // A released cell is an old one, and an Extension is allocated after its cell: the collector may have marked either.
    LayoutID layout = m_placement.allocation(value->allocation()).layout;
    bool mayBeMarked = m_placement.freeCellOffset(layout) || std::ranges::any_of(collectInitialFields(layout, value->fields(), value->valuesOperands()), [&](const InitialField& field) {
        return !isStoredInCell(*field.slot, typeOf(field.value));
    });

    // Each evaluation of a class and each closure of a constructor function has a prototype object of its own, and the site knows one of them.
    // Code on the failing side of "the class was evaluated once" is compact, and from the second evaluation on it is what runs.
    // B3 does not see the operation store a Structure into the cell, so the code that follows reads the header of the result.
    define(value, selectByBranch<CellPointer>(hasSitePrototype, useCell, [&] {
        LBasicBlock isUnknown = newBlock();
        Join<CellPointer> join(m_out);
        Word32 structureID = emitter<AccessEmitter>().findPrototypeTransition(cell, boxCell(requested), isUnknown);
        {
            CodeGenerator::InitializationScope scope(generator());
            storeField(cell, m_heaps.JSCell_structureID, structureID);
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
        if (representationOf(field.value) != Typed::representationOf(field.slot->representation))
            return failWithoutCell(InternalErrorKind::WrongRepresentation);
        if (isStoredInCell(*field.slot, typeOf(field.value)))
            isInitialField.set(field.field);
    }

    CellPointer cell = allocateCell(value, id, layout, storage);
    {
        CodeGenerator::InitializationScope scope(generator());
        initializeHiddenFields(cell, layoutID, storage);
        storeInitialFields(value, cell, layoutID, initialFields);
        storeEmptySlots(cell, layoutID, isInitialField);
    }
    if (storage == AllocationStorage::Heap) {
        emitBarrierIfReleased(value, cell, layoutID, initialFields);
        emitter<BarrierEmitter>().fenceAfterInitialization();
    }
    storeFieldsInExtension(value, cell, layoutID, initialFields);
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
