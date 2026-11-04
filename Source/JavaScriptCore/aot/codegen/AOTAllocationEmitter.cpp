/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccess.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsAllocation.h"
#include "AOTSweep.h"
#include "JSCell.h"
#include "JSObject.h"
#include <algorithm>
#include <bit>
#include <ranges>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// The fields that Facts appended follow the declared ones, and the layout's own record does not list them.
static MemoryLocation locationOfField(const Program& program, const SlotPlan& slots, LayoutID layout, FieldIndex field)
{
    if (program.at(layout).isEnvironment())
        return MemoryLocation::environmentField(layout, field);
    PropertyKeyID key = slots.keyOf(program, { layout, field });
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

static bool newObjectMayHaveOtherLayout(const Program& program, const Operation& newObject)
{
    return newObject.prototypeOperand() && program.prototypeMaySelectOtherClass(newObject.layout());
}

// A cell of the frame gets no fence. A cell that was handed back is an old one, which the collector may look at while it is filled: takeCellHandedBackOrAllocate().
// allocateInstance() hands the prototype to its stub in front of the fence: looksAtPrototypeBeforeAllocating().
bool newObjectFencesBeforeHandingCellOn(const Program& program, const Plan& plan, const Operation& newObject)
{
    UNUSED_PARAM(program);
    if (newObject.storage() != AllocationStorage::Heap || plan.layout(newObject.layout()).isEscapedOffset)
        return false;
    return !newObject.prototypeOperand() || !newObject.fields().empty();
}

// In line: 75 bytes a site. By the stub: 18 bytes and 3.5 instructions more.
bool AllocationEmitter::initializesFrameObjectInStub(const ValueContext& value, LayoutID layout)
{
    if (!isObjectLayout(m_program.at(layout).kind) || m_plan.getsFieldsOnlyAtConstruction(layout)) {
        m_out.noteForm(Form::Open, StateOfKnowledge::CellInFrameFilledOnce);
        return false;
    }
    return !generator().takesOpenForm(value, StateOfKnowledge::ObjectInFrame);
}

CellPointer AllocationEmitter::allocateFrameCell(const ValueContext& value, const MentionedAllocation& allocation, const PlannedLayout& layout)
{
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    if (!isObjectLayout(m_program.at(allocation.placed.layout).kind)) {
        m_out.noteForm(Form::Open, StateOfKnowledge::CellInFrameFilledOnce);
        return cellAllocation.allocateInFrame(value, layout.size);
    }
    if (initializesFrameObjectInStub(value, allocation.placed.layout))
        return CellPointer(callStub(value, initializeObjectInFrame(), { zeroExtended(numberOf(allocation.mention)).value(), cellAllocation.allocateInFrame(value, layout.size).value() }).first);

    LBasicBlock hasNoRecord = newBlock();
    Join<Integer> headerOfHeapCells(m_out);
    Pointer record = cellAllocation.allocationSiteRecord(allocation);
    Integer headerInRecord(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), record.value(), AllocationSiteRecord::offsetOfHeader())));
    branchUnless(isNotZero(headerInRecord), hasNoRecord);
    headerOfHeapCells.add(headerInRecord);

    // The operation fills the record of the site and allocates no cell.
    m_out.appendTo(hasNoRecord);
    headerOfHeapCells.add(callOperation<Integer>(value, RuntimeOperation::GetHeaderOfAllocation, { numberOf(allocation.mention) }, { .isOnRarePath = true }));
    Integer header = headerOfHeapCells.finish();

    CellPointer cell = cellAllocation.allocateInFrame(value, layout.size);
    CodeGenerator::InitializationScope scope(generator());
    cellAllocation.storeHeader(cell, header);
    storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(allocation.placed.orderWord)));
    m_out.store32(m_out.constInt32(static_cast<int32_t>(shapeWordOf(allocation.placed.shape))), m_out.address(m_heaps.entire(AbstractHeap::ShapeWords), cell.value(), offsetOfShapeWord));
    return cell;
}

// `{ ...source }`: the TryCopyFields that the block generates right behind the allocation of a literal without fields, and that fills the literal. Null: there is none.
static ValueID findCopyThatFillsNewObject(const CodeGenerator& generator, const ValueContext& value)
{
    if (value->storage() != AllocationStorage::Heap || value->prototypeOperand() || !value->fields().empty())
        return { };
    std::span<const ValueID> behind = value.operationsBehind();
    if (behind.empty())
        return { };
    const Operation& next = generator.operationAt(behind.front());
    return next.opcode == Opcode::TryCopyFields && next.objectOperand() == value.id() ? behind.front() : ValueID();
}

void AllocationEmitter::generateNewObject(const ValueContext& value)
{
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    LayoutID layout = value->layout();
    MentionedAllocation allocation = m_program.at(layout).isEnvironment()
        ? cellAllocation.environmentAllocationOf(layout)
        : cellAllocation.allocationOf(value, layout, value->allocationClass(), value->fields());
    if (generator().hasFailed()) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    if (ValueID copy = findCopyThatFillsNewObject(generator(), value)) {
        auto allocateEmpty = [&] {
            return allocateWithFields(value, allocation, value->fields(), value->valuesOperands(), value->storage());
        };
        if (std::optional<CellPointer> cell = emitter<AccessEmitter>().tryAllocateCopyOfFields(value, allocation, copy, allocateEmpty)) {
            define(value, *cell);
            return;
        }
    }
    if (looksAtPrototypeBeforeAllocating(value)) {
        define(value, allocateInstance(value, allocation, value->prototypeOperand()));
        return;
    }
    CellPointer cell = allocateWithFields(value, allocation, value->fields(), value->valuesOperands(), value->storage());
    if (ValueID prototype = value->prototypeOperand())
        storePrototype(value, cell, prototype);
    else
        define(value, cell);
}

void AllocationEmitter::generateNewDictionary(const ValueContext& value)
{
    // A run of definitions is stored by what its site remembers only where the cell has room for all of it.
    unsigned numberOfNames = value->roomForNames().value_or(0);
    unsigned addedPairsOfSlots = numberOfNames > JSFinalObject::defaultInlineCapacity ? (numberOfNames - JSFinalObject::defaultInlineCapacity + 1) / 2 : 0;
    define(value, CellPointer(callStub(value, newDictionary(std::min(addedPairsOfSlots, numberOfRoomierFinalObjects)), { }).first));
}

void AllocationEmitter::generateFinishObject(const ValueContext& value)
{
    define(value, get(value->objectOperand()));
}

void AllocationEmitter::clearBirthMark(CellPointer cell)
{
    CodeGenerator::InitializationScope scope(generator());
    Word32 flags = loadByte(cell, m_heaps.JSCell_typeInfoFlags);
    m_out.store32As8(bitAnd(flags, word32(static_cast<uint8_t>(~TypeInfoPerCellBit))).value(), cell.value(), m_heaps.JSCell_typeInfoFlags);
}

void AllocationEmitter::generateFinishBirth(const ValueContext& value)
{
    clearBirthMark(in<CellPointer>(value->objectOperand()));
}

namespace {

struct BirthFieldToCheck {
    FieldIndex field { 0 };
    bool holdsRawValue { false };
};
using BirthFieldsToCheck = Vector<BirthFieldToCheck, 8>;

} // anonymous namespace

// Mirrors Object::hasEveryFieldOfBirth(). Nothing: a field lies outside the cell, so only the run time can look.
static std::optional<BirthFieldsToCheck> birthFieldsToCheck(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    if (!planned.holdsCompiledObjects())
        return std::nullopt;
    BirthFieldsToCheck fields;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (!slot.indexInLayoutRecord)
            continue;
        bool holdsRawValue = planned.holdsRawValueFromBirth(field);
        if (!holdsRawValue && !slot.isFilledAtBirth)
            continue;
        if (slot.storage != SlotStorage::Inline)
            return std::nullopt;
        fields.append({ field, holdsRawValue });
    }
    return fields;
}

// `instance(B)` stands for whatever new.target makes of a B, an object of a class below among it, and a test that has passed leaves the name in the type.
// A compiled object keeps its shape for life, so the layouts that the test lists are those of the cell under each name that its passing edge has given out. Null: no such test stands behind the name.
static LayoutSetID layoutsListedByPassedShapeTest(const Code& code, ValueID name)
{
    while (name && code.at(name).opcode == Opcode::Narrowed) {
        const Operation& narrowed = code.at(name);
        ValueID test = narrowed.testOperand();
        if (test && narrowed.isOnTakenEdge() && !narrowed.narrowsHolderOfOperand() && !narrowed.narrowsOtherResultOfOperand()) {
            const Operation& asked = code.at(test);
            if (asked.opcode == Opcode::ShapeTest && asked.objectOperand() == narrowed.valueOperand())
                return asked.layouts();
        }
        name = narrowed.valueOperand();
    }
    return { };
}

static LayoutID layoutForInlineBirthCheck(TypeReader types, const Program& program, const Plan& plan, LayoutSetID layouts, BirthFieldsToCheck& fields)
{
    if (!layouts)
        return { };
    LayoutID first;
    for (LayoutID member : membersOf(types, layouts)) {
        std::optional<BirthFieldsToCheck> ofMember = birthFieldsToCheck(plan, member);
        if (!ofMember)
            return { };
        if (!first) {
            first = member;
            fields = WTF::move(*ofMember);
            continue;
        }
        bool checksTheSame = std::ranges::equal(*ofMember, fields, [&](const BirthFieldToCheck& mine, const BirthFieldToCheck& ofFirst) {
            return mine.holdsRawValue == ofFirst.holdsRawValue
                && plan.slot(member, mine.field).hasLocationOf(plan.slot(first, ofFirst.field))
                && plan.slots().keyOf(program, { member, mine.field }) == plan.slots().keyOf(program, { first, ofFirst.field });
        });
        if (!checksTheSame)
            return { };
    }
    return first;
}

void AllocationEmitter::generateTryFinishBirth(const ValueContext& value)
{
    CellPointer cell = in<CellPointer>(value->objectOperand());
    LBasicBlock isUnfinished = newBlock();
    LBasicBlock done = newBlock();
    Condition bearsMark = hasBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeInfoPerCellBit);

    Type type = typeOf(value->objectOperand());
    bool typeClosesLayouts = type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
    LayoutSetID provenLayouts = typeClosesLayouts ? type.layouts : layoutsListedByPassedShapeTest(generator().code(), value->objectOperand());
    BirthFieldsToCheck fields;
    LayoutID layout;
    BlockFrequency frequency = generator().code().at(value.block()).frequency;
    Place placeThatRulesOutLookInLine = frequency == BlockFrequency::Rare ? Place::RareBlock : frequency == BlockFrequency::Unlikely ? Place::UnlikelyBlock : Place::Anywhere;
    if (placeThatRulesOutLookInLine == Place::Anywhere)
        layout = layoutForInlineBirthCheck(types(), m_program, m_plan, provenLayouts, fields);
    m_out.noteForm(layout ? Form::Open : Form::Runtime, layout ? StateOfKnowledge::BirthFieldsInCell : StateOfKnowledge::Always, placeThatRulesOutLookInLine);
    if (!layout) {
        branch(bearsMark, { normalEdge(isUnfinished), normalEdge(done) });
        m_out.appendTo(isUnfinished);
        callVoidOperation(value, RuntimeOperation::TryFinishBirth, { cell }, { .isOnRarePath = true });
        m_out.jump(done);
        m_out.appendTo(done);
        return;
    }

    ScopeFork fork = m_out.forkScope();
    branch(bearsMark, { normalEdge(isUnfinished), normalEdge(done) });
    m_out.appendToArm(isUnfinished, fork);
    auto continueIf = [&](Condition holds) {
        LBasicBlock next = newBlock();
        branch(holds, { normalEdge(next), normalEdge(done) });
        m_out.appendToContinuation(next);
    };
    continueIf(hasNoBits(emitter<ShapeTestEmitter>().orderWordOf(cell), orderWordHasFieldOutsideSlot));
    SlotEmitter& slots = emitter<SlotEmitter>();
    for (const BirthFieldToCheck& checked : fields | std::views::reverse) {
        const PlacedSlot& slot = m_plan.slot(layout, checked.field);
        if (!traitsOf(slot.representation).hasAbsent)
            continue;
        MemoryLocation location = locationOfField(m_program, m_plan.slots(), layout, checked.field);
        continueIf(checked.holdsRawValue ? slots.holdsDeclaredType(cell, slot, location) : slots.holdsValue(cell, slot, location));
    }
    clearBirthMark(cell);
    m_out.jump(done);
    m_out.appendToJoin(done, fork);
}

AllocationEmitter::InitialFields AllocationEmitter::collectInitialFields(LayoutID layout, std::span<const uint32_t> fields, std::span<const ValueID> values)
{
    RELEASE_ASSERT(fields.size() == values.size());
    InitialFields initialFields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const PlacedSlot& slot = m_plan.slot(layout, fields[i]);
        if (!slot.hasStorage() || !needsStorageFor(slot, typeOf(values[i])))
            continue;
        initialFields.append(InitialField { &slot, fields[i], values[i] });
    }
    return initialFields;
}

static bool isZeroWhileEmpty(SlotRepresentation representation)
{
    SlotTraits traits = traitsOf(representation);
    return !traits.hasAbsent || !traits.absentBits;
}

ZeroedBy AllocationEmitter::whoZeroesBodyOfFreshCell(const ValueContext& value, const MentionedAllocation& allocation, const BitVector& isInitialField, AllocationStorage storage)
{
    // A cell that was handed back holds what it held.
    LayoutID layout = allocation.placed.layout;
    const SlotPlan& plan = m_plan.slots();
    if (storage != AllocationStorage::Heap || plan.layout(layout).isEscapedOffset)
        return ZeroedBy::Nobody;

    unsigned storesOfZero = static_cast<unsigned>(plan.layout(layout).bytesKeptZero.size());
    unsigned storesOfOtherPatterns = 0;
    auto countStoreInto = [&](const PlacedSlot& slot) {
        ++(isZeroWhileEmpty(slot.representation) ? storesOfZero : storesOfOtherPatterns);
    };
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage())
            countStoreInto(*hiddenWord);
    }
    forEachEmptySlot(layout, isInitialField, [&](FieldIndex, const PlacedSlot& slot) {
        countStoreInto(slot);
    });
    return emitter<CellAllocationEmitter>().whoZeroesBody(value, allocation, storesOfZero, storesOfOtherPatterns);
}

CellPointer AllocationEmitter::takeCellHandedBackOrAllocate(const ValueContext& value, const MentionedAllocation& allocation)
{
    TypedPointer word = emitter<StaticsEmitter>().wordOfEnvironmentHandedBack(allocation.placed.layout);
    Integer handedBack(m_out.load64(word));
    return selectByBranch<CellPointer>(isNotZero(handedBack), [&] {
        m_out.store64(m_out.constInt64(0), word);
        return CellPointer(handedBack.value());
    }, [&] {
        return emitter<CellAllocationEmitter>().allocate(value, allocation, ZeroedBy::Nobody);
    });
}

CellPointer AllocationEmitter::allocateCell(const ValueContext& value, const MentionedAllocation& allocation, const PlannedLayout& layout, AllocationStorage storage, ZeroedBy zeroedBy)
{
    RELEASE_ASSERT(zeroedBy == ZeroedBy::Nobody || storage == AllocationStorage::Heap);
    switch (storage) {
    case AllocationStorage::Heap:
        if (layout.isEscapedOffset) {
            RELEASE_ASSERT(zeroedBy == ZeroedBy::Nobody);
            return takeCellHandedBackOrAllocate(value, allocation);
        }
        if (zeroedBy == ZeroedBy::StubFromTemplate)
            return emitter<CellAllocationEmitter>().allocateFromTemplate(value, allocation, bodyOfEmptyCell(allocation).span());
        return emitter<CellAllocationEmitter>().allocate(value, allocation, zeroedBy);
    case AllocationStorage::Frame:
        return allocateFrameCell(value, allocation, layout);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AllocationEmitter::initializeHiddenFields(CellPointer cell, LayoutID layout, AllocationStorage storage, ZeroedBy zeroedBy, bool hasStateOfFrameCell)
{
    // A store whose receiver may also be on the heap runs its barrier, which must not mistake a frame cell for a marked one.
    // No store into an environment that lies only in frames runs one.
    const SlotPlan& plan = m_plan.slots();
    bool someBarrierMayReadState = !plan.layout(layout).liesOnlyInFrames;
    if (storage != AllocationStorage::Heap && !hasStateOfFrameCell && someBarrierMayReadState)
        m_out.store32As8(m_out.constInt32(frameCellState), m_out.address(m_heaps.collectorState, cell.value(), JSCell::cellStateOffset()));

    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage() && zeroedBy != ZeroedBy::StubFromTemplate && (zeroedBy == ZeroedBy::Nobody || !isZeroWhileEmpty(hiddenWord->representation)))
            emitter<SlotEmitter>().storeAbsent(m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), hiddenWord->offset), hiddenWord->representation);
    }

    if (std::optional<uint32_t> isEscapedOffset = plan.layout(layout).isEscapedOffset; isEscapedOffset && zeroedBy == ZeroedBy::Nobody)
        m_out.store32As8(m_out.constInt32(0), m_out.address(m_heaps.collectorState, cell.value(), *isEscapedOffset));
}

Representation AllocationEmitter::representationWantedBy(const InitialField& field)
{
    return field.slot->representation == SlotRepresentation::DeferredClosure ? Representation::Cell32 : Typed::representationOf(field.slot->representation);
}

// wantedRepresentationOf() leaves such a reference wide: narrowed in front of the allocation, all the narrow forms would be held at one time.
bool AllocationEmitter::isNarrowedWhereStored(const InitialField& field) const
{
    return representationOf(field.value) == Representation::Cell && representationWantedBy(field) == Representation::Cell32;
}

Operand AllocationEmitter::takeForSlot(const InitialField& field)
{
    return isNarrowedWhereStored(field) ? Operand(compress(in<CellPointer>(field.value))) : get(field.value);
}

void AllocationEmitter::storeInitialFields(CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    for (const InitialField& field : initialFields) {
        if (m_plan.layout(layout).holdsRawValueFromBirth(field.field))
            slots.markCellIfMarkerIsStored(cell, field.value);
        if (field.slot->isOutOfLine())
            continue;
        MemoryLocation location = locationOfField(m_program, m_plan.slots(), layout, field.field);
        if (field.slot->representation != SlotRepresentation::DeferredClosure)
            slots.storeSlot(cell, *field.slot, location, takeForSlot(field), nullptr, typeOf(field.value));
        else if (isDeferredClosure(field.value)) {
            LValue uncreated = m_out.constInt32(uncreatedDeferredClosure);
            m_out.store32(uncreated, slots.addressOf(cell, *field.slot, location));
        } else {
            NarrowReference reference = takeForSlot(field).as<NarrowReference>();
            m_out.store32(reference.value(), slots.addressOf(cell, *field.slot, location));
        }
    }
}

template<typename Visitor>
void AllocationEmitter::forEachEmptySlot(LayoutID layout, const BitVector& isInitialField, const Visitor& visitor)
{
    const PlannedLayout& planned = m_plan.layout(layout);
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
    const SlotPlan& plan = m_plan.slots();
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage() && !isZeroWhileEmpty(hiddenWord->representation))
            words.append({ hiddenWord->offset, hiddenWord->representation });
    }
    forEachEmptySlot(layout, BitVector(), [&](FieldIndex, const PlacedSlot& slot) {
        if (!isZeroWhileEmpty(slot.representation))
            words.append({ slot.offset, slot.representation });
    });
    return words;
}

Vector<uint8_t, 64> AllocationEmitter::bodyOfEmptyCell(const MentionedAllocation& allocation)
{
    LayoutID layout = allocation.placed.layout;
    CellInitialization form = isObjectLayout(m_program.at(layout).kind) ? CellInitialization::HeaderAndWords : CellInitialization::Header;
    uint32_t offsetOfBody = offsetOfTemplateInCell(form);
    Vector<uint8_t, 64> body(FillWith { }, sizeOfCellTemplate(form, allocation.placed.size), 0);
    for (const WordOfEmptyObject& word : wordsOfEmptyObject(layout)) {
        RELEASE_ASSERT(word.offset >= offsetOfBody && word.offset + widthOf(word.representation) <= allocation.placed.size);
        clearSlot(word.representation, body.mutableSpan().subspan(word.offset - offsetOfBody).data());
    }
    return body;
}

void AllocationEmitter::storeEmptySlots(CellPointer cell, LayoutID layout, const BitVector& isInitialField, ZeroedBy zeroedBy)
{
    if (zeroedBy == ZeroedBy::StubFromTemplate)
        return;
    SlotEmitter& slots = emitter<SlotEmitter>();
    forEachEmptySlot(layout, isInitialField, [&](FieldIndex field, const PlacedSlot& slot) {
        if (zeroedBy != ZeroedBy::Nobody && isZeroWhileEmpty(slot.representation))
            return;
        TypedPointer address = slots.addressOf(cell, slot, locationOfField(m_program, m_plan.slots(), layout, field));
        SlotTraits traits = traitsOf(slot.representation);
        if (traits.hasAbsent)
            slots.storeAbsent(address, slot.representation);
        else
            storeZero(m_out, address, traits.width);
    });

    if (zeroedBy != ZeroedBy::Nobody)
        return;
    // A read whose receiver may have another layout loads them under the name of the field that lies there in that layout.
    for (ReservedBytes bytes : m_plan.layout(layout).bytesKeptZero)
        storeZero(m_out, m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), bytes.offset), bytes.width);
}

void AllocationEmitter::storeFieldsInExtension(const ValueContext& value, CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    // `this.exports = undefined`: the order of keys says that the property is there, and a read that finds no granule gives undefined.
    auto takesGranule = [&](const InitialField& field) {
        return field.slot->isOutOfLine() && needsStorageFor(*field.slot, typeOf(field.value));
    };
    uint64_t granuleBits = 0;
    for (const InitialField& field : initialFields) {
        if (takesGranule(field))
            granuleBits |= granuleBitOf(field.slot->offset);
    }
    if (!granuleBits)
        return;
    // What the other makers of the object go on to store finds its granule.
    granuleBits |= m_plan.layout(layout).granulesOfFirstExtension;

    // A fresh cell has no Extension, and NewObject declares no read of a field: nothing is tested in front of the call.
    const PlacedSlot& extensionWord = m_plan.slots().extensionWord(layout);
    unsigned size = extensionCellSizeFor(std::popcount(granuleBits));
    if (size <= maxExtensionSizeFromStub)
        callStub(value, allocateExtension(size), { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(granuleBits)) });
    else
        callVoidOperation(value, RuntimeOperation::EnsureExtensionGranules, { cell, integer(static_cast<int64_t>(granuleBits)) });

    SlotEmitter& slots = emitter<SlotEmitter>();
    CodeGenerator::InitializationScope scope(generator());
    Pointer extension = slots.extensionOf(cell, extensionWord);
    for (const InitialField& field : initialFields) {
        if (takesGranule(field))
            slots.storeInExtension(extension, *field.slot, locationOfField(m_program, m_plan.slots(), layout, field.field), takeForSlot(field));
    }
    ensureStillAliveHere(cell);
}

// An object of another layout takes the place of one in which nothing is stored yet, so that one need not be made. Outside of loops the stub compares every prototype, so that the site need not.
bool AllocationEmitter::looksAtPrototypeBeforeAllocating(const ValueContext& value)
{
    if (!value->prototypeOperand() || !value->fields().empty() || value->storage() != AllocationStorage::Heap || !m_plan.layout(value->layout()).hasSlots())
        return false;
    return newObjectMayHaveOtherLayout(m_program, value.value()) || (!generator().isInLoop(value.block()) && !m_plan.layout(value->layout()).isEscapedOffset);
}

CellPointer AllocationEmitter::allocateInstance(const ValueContext& value, const MentionedAllocation& allocation, ValueID prototype)
{
    LayoutID layout = allocation.placed.layout;
    auto [cell, isCellOfAllocation] = emitter<CellAllocationEmitter>().allocateInstance(value, allocation, in<CellPointer>(prototype));
    if (!wordsOfEmptyObject(layout).isEmpty()) {
        LBasicBlock fill = newBlock();
        LBasicBlock isWhole = newBlock();
        m_out.branch(isCellOfAllocation.value(), normalEdge(fill), normalEdge(isWhole));
        m_out.appendTo(fill);
        {
            CodeGenerator::InitializationScope scope(generator());
            initializeHiddenFields(cell, layout, AllocationStorage::Heap, ZeroedBy::Stub, false);
            storeEmptySlots(cell, layout, BitVector(m_plan.layout(layout).numberOfFields()), ZeroedBy::Stub);
        }
        m_out.jump(isWhole);
        m_out.appendTo(isWhole);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return cell;
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
        // The mention that generateNewObject() made: equal mentions get one index.
        MentionedAllocation allocation = emitter<CellAllocationEmitter>().allocationOf(value, value->layout(), value->allocationClass(), value->fields());
        return callOperation<CellPointer>(value, RuntimeOperation::SetConstructPrototype, { cell, requested, numberOf(allocation.mention), word32(value->fields().empty()) }, { .isOnRarePath = true });
    };
    if (newObjectMayHaveOtherLayout(m_program, value.value()) || !generator().isInLoop(value.block())) {
        define(value, either<CellPointer>(hasSitePrototype, useCell, setByOperation));
        return;
    }

    // Each evaluation of a class and each closure of a constructor function has a prototype object of its own, and the site knows one of them.
    define(value, selectByBranch<CellPointer>(hasSitePrototype, useCell, [&] {
        LBasicBlock isUnknown = newBlock();
        Join<CellPointer> join(m_out);
        Word32 structureID = emitter<AccessEmitter>().findPrototypeTransition(cell, boxCell(requested), isUnknown);
        {
            CodeGenerator::InitializationScope scope(generator());
            emitter<AccessEmitter>().storeStructureAfterPrototypeTransition(cell, structureID);
        }
        join.add(cell);

        m_out.appendTo(isUnknown);
        join.add(setByOperation());
        return join.finish();
    }));
}

CellPointer AllocationEmitter::allocateWithFields(const ValueContext& value, const MentionedAllocation& allocation, std::span<const uint32_t> fields, std::span<const ValueID> values, AllocationStorage storage)
{
    auto failWithoutCell = [&](InternalErrorKind kind) {
        fail(value, kind);
        return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
    };

    LayoutID layoutID = allocation.placed.layout;
    const PlannedLayout& layout = m_plan.layout(layoutID);
    if (!layout.hasSlots())
        return failWithoutCell(InternalErrorKind::SlotAccessNotPlaced);

    InitialFields initialFields = collectInitialFields(layoutID, fields, values);
    BitVector isInitialField(layout.numberOfFields());
    for (const InitialField& field : initialFields) {
        bool defersClosure = field.slot->representation == SlotRepresentation::DeferredClosure;
        Representation wanted = representationWantedBy(field);
        bool hasNoValue = defersClosure && isDeferredClosure(field.value);
        if (!hasNoValue && representationOf(field.value) != wanted && !isNarrowedWhereStored(field))
            return failWithoutCell(InternalErrorKind::WrongRepresentation);
        if (!field.slot->isOutOfLine())
            isInitialField.set(field.field);
    }

    ZeroedBy zeroedBy = whoZeroesBodyOfFreshCell(value, allocation, isInitialField, storage);
    CellPointer cell = allocateCell(value, allocation, layout, storage, zeroedBy);
    {
        CodeGenerator::InitializationScope scope(generator());
        initializeHiddenFields(cell, layoutID, storage, zeroedBy, storage != AllocationStorage::Heap && initializesFrameObjectInStub(value, layoutID));
        storeInitialFields(cell, layoutID, initialFields);
        storeEmptySlots(cell, layoutID, isInitialField, zeroedBy);
    }
    // A cell that was handed back is an old one: the collector may have marked it.
    bool mayHaveStoredCellInOldCell = storage == AllocationStorage::Heap && layout.isEscapedOffset && std::ranges::any_of(initialFields, [](const InitialField& field) {
        return traitsOf(field.slot->representation).holdsCell;
    });
    if (mayHaveStoredCellInOldCell)
        emitter<BarrierEmitter>().barrier(value, cell);
    // An Extension is stored into the cell behind the fence, and the collector may know a cell that was handed back: such a fence is left to no later one.
    bool keepsItsFence = layout.isEscapedOffset || std::ranges::any_of(initialFields, [](const InitialField& field) {
        return field.slot->isOutOfLine();
    });
    if (storage == AllocationStorage::Heap && keepsItsFence)
        emitter<BarrierEmitter>().fenceAfterInitialization();
    else if (storage == AllocationStorage::Heap)
        emitter<BarrierEmitter>().fenceAfterInitialization(value);
    // Nothing collects between the allocation of a cell and that of its Extension.
    storeFieldsInExtension(value, cell, layoutID, initialFields);
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
