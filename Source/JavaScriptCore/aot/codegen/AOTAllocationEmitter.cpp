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
#include "AOTEnvironmentEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
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
bool newObjectFencesBeforeHandingCellOn(const Program& program, const Plan& plan, const Operation& newObject)
{
    UNUSED_PARAM(program);
    if (newObject.storage() != AllocationStorage::Heap || plan.layout(newObject.layout()).cellsAreHandedBack)
        return false;
    return !newObject.prototypeOperand() || !newObject.fields().empty();
}

bool AllocationEmitter::initializesFrameObjectInStub(const ValueContext& value, LayoutID layout)
{
    if (!isObjectLayout(m_program.at(layout).kind)) {
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

    m_out.appendTo(hasNoRecord);
    headerOfHeapCells.add(callOperation<Integer>(value, RuntimeOperation::GetHeaderOfAllocation, { numberOf(allocation.mention) }, { .isOnRarePath = true }));
    Integer header = headerOfHeapCells.finish();

    CellPointer cell = cellAllocation.allocateInFrame(value, layout.size);
    CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
    cellAllocation.storeHeader(cell, header);
    storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(allocation.placed.orderWord)));
    uint32_t shapeWord = shapeWordOf(allocation.placed.shape);
    m_out.store32(m_out.constInt32(static_cast<int32_t>(allocation.placed.isBornUnfinished ? shapeWord & ~shapeWordIsUnmarked : shapeWord)), m_out.address(m_heaps.entire(AbstractHeap::ShapeWords), cell.value(), offsetOfShapeWord));
    return cell;
}

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
        : cellAllocation.allocationOf(value, layout, value->fields());
    if (generator().hasFailed()) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    if (ValueID copy = findCopyThatFillsNewObject(generator(), value); copy && operationAt(copy).sourceLayout()) {
        define(value, allocateCopyFromOtherLayout(value, copy));
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
        storePrototype(value, cell, in<CellPointer>(prototype));
    else if (value->storage() == AllocationStorage::Heap && m_program.at(layout).kind == LayoutKind::Instance && !m_plan.slots().hasDirectInstances(layout))
        storePrototype(value, cell, unboxCell(loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype)));
    else
        define(value, cell);
}

CellPointer AllocationEmitter::allocateCopyFromOtherLayout(const ValueContext& value, ValueID copy)
{
    AccessEmitter& accesses = emitter<AccessEmitter>();
    const Operation& words = operationAt(copy);
    LayoutID layoutID = value->layout();
    const PlannedLayout& layout = m_plan.layout(layoutID);
    Vector<FieldIndex, 8> takers = accesses.fieldsThatTakeCopyFromOtherLayout(words);
    MentionedAllocation allocation = emitter<CellAllocationEmitter>().allocationOf(value, layoutID, takers.span());
    BitVector isCopied(layout.numberOfFields());
    for (FieldIndex field : takers)
        isCopied.set(field);
    EmptySlots emptySlots = collectEmptySlots(layoutID, isCopied);
    CellPointer source = in<CellPointer>(words.sourceOperand());
    ZeroedBy zeroedBy = whoZeroesBodyOfFreshCell(value, allocation, emptySlots.span(), AllocationStorage::Heap);
    CellPointer cell = allocateCell(value, allocation, layout, AllocationStorage::Heap, zeroedBy);
    {
        CodeGenerator::InitializationScope scope(generator());
        initializeHiddenFields(cell, layoutID, AllocationStorage::Heap, zeroedBy, false);
        accesses.copyFieldsFromOtherLayout(words, source, cell);
        storeEmptySlots(cell, layoutID, emptySlots.span(), zeroedBy, value.block());
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    accesses.noteCopyMadeAtAllocation(copy);
    return cell;
}

void AllocationEmitter::generateNewDictionary(const ValueContext& value)
{
    define(value, CellPointer(callStub(value, newDictionary(), { }).first));
}

void AllocationEmitter::generateFinishObject(const ValueContext& value)
{
    define(value, get(value->objectOperand()));
}

void AllocationEmitter::clearBirthMark(CellPointer cell, const Type& type)
{
    if (type.hasOnlyLayouts() && !type.primitives && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        std::span<const LayoutID> members = membersOf(types(), type.layouts);
        ShapeRange shapes = members.size() == 1 ? m_plan.shapes().shapesOf(members[0]) : ShapeRange { };
        if (shapes.count == 1) {
            CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
            storeField(cell, m_heaps.AOTObject_shapeWord, word32(static_cast<int32_t>(shapeWordOf(shapes.first))));
            return;
        }
    }
    emitter<ShapeTestEmitter>().clearMark(cell);
}

void AllocationEmitter::generateFinishBirth(const ValueContext& value)
{
    clearBirthMark(in<CellPointer>(value->objectOperand()), typeOf(value->objectOperand()));
}

namespace {

struct BirthFieldToCheck {
    FieldIndex field { 0 };
    bool holdsRawValue { false };
};
using BirthFieldsToCheck = Vector<BirthFieldToCheck, 8>;

} // anonymous namespace

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
    Condition bearsMark = emitter<ShapeTestEmitter>().bearsMark(cell);

    Type type = typeOf(value->objectOperand());
    bool typeClosesLayouts = type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
    LayoutSetID provenLayouts = typeClosesLayouts ? type.layouts : layoutsListedByPassedShapeTest(generator().code(), value->objectOperand());
    BirthFieldsToCheck fields;
    LayoutID layout = layoutForInlineBirthCheck(types(), m_program, m_plan, provenLayouts, fields);
    m_out.noteForm(layout ? Form::Open : Form::Runtime, layout ? StateOfKnowledge::BirthFieldsInCell : StateOfKnowledge::Always);
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
    SlotEmitter& slots = emitter<SlotEmitter>();
    auto finishBehindLooks = [&](bool everyFieldIsListed) {
        for (const BirthFieldToCheck& checked : fields | std::views::reverse) {
            const PlacedSlot& slot = m_plan.slot(layout, checked.field);
            if (!traitsOf(slot.representation).hasAbsent || (everyFieldIsListed && !checked.holdsRawValue))
                continue;
            MemoryLocation location = locationOfField(m_program, m_plan.slots(), layout, checked.field);
            continueIf(checked.holdsRawValue ? slots.holdsDeclaredType(cell, slot, location) : slots.holdsValue(cell, slot, location));
        }
        clearBirthMark(cell, type);
        m_out.jump(done);
    };
    Vector<FieldIndex, 16> fieldsOfBirth;
    for (const BirthFieldToCheck& checked : fields)
        fieldsOfBirth.append(checked.field);
    std::optional<uint32_t> orderWordOfFinishedObject = membersOf(types(), provenLayouts).size() == 1 && fieldsOfBirth.size() > 1 ? m_plan.keyOrders().findOrderWordOfShortestBeginningThatLists(layout, fieldsOfBirth.span()) : std::nullopt;
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    if (orderWordOfFinishedObject) {
        LBasicBlock listsThemAll = newBlock();
        LBasicBlock listsOtherwise = newBlock();
        ScopeFork byOrderWord = m_out.forkScope();
        branch(equal(orderWord, static_cast<int32_t>(*orderWordOfFinishedObject)), { normalEdge(listsThemAll), unlikelyEdge(listsOtherwise) });
        m_out.appendToArm(listsThemAll, byOrderWord);
        finishBehindLooks(true);
        m_out.appendToArm(listsOtherwise, byOrderWord);
    }
    continueIf(hasNoBits(orderWord, orderWordHasFieldOutsideSlot));
    finishBehindLooks(false);
    m_out.appendToJoin(done, fork);
}

AllocationEmitter::InitialFields AllocationEmitter::collectInitialFields(LayoutID layout, std::span<const uint32_t> fields, std::span<const ValueID> values)
{
    RELEASE_ASSERT(fields.size() == values.size());
    InitialFields initialFields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const PlacedSlot& slot = m_plan.slot(layout, fields[i]);
        if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage && !needsStorageFor(slot, typeOf(values[i]))))
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

ZeroedBy AllocationEmitter::whoZeroesBodyOfFreshCell(const ValueContext& value, const MentionedAllocation& allocation, std::span<const FieldIndex> emptySlots, AllocationStorage storage)
{
    LayoutID layout = allocation.placed.layout;
    const SlotPlan& plan = m_plan.slots();
    if (storage != AllocationStorage::Heap || plan.layout(layout).cellsAreHandedBack)
        return ZeroedBy::Nobody;

    unsigned storesOfZero = 0;
    unsigned storesOfOtherPatterns = 0;
    auto countStoreInto = [&](const PlacedSlot& slot) {
        ++(isZeroWhileEmpty(slot.representation) ? storesOfZero : storesOfOtherPatterns);
    };
    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage())
            countStoreInto(*hiddenWord);
    }
    const PlannedLayout& planned = m_plan.layout(layout);
    for (FieldIndex field : emptySlots)
        countStoreInto(planned.slots[field]);
    return emitter<CellAllocationEmitter>().whoZeroesBody(value, allocation, storesOfZero, storesOfOtherPatterns);
}

CellPointer AllocationEmitter::takeCellHandedBackOrAllocate(const ValueContext& value, const MentionedAllocation& allocation)
{
    TypedPointer word = emitter<StaticsEmitter>().wordOfEnvironmentHandedBack(allocation.placed.layout);
    Integer handedBack(m_out.load64(word));
    return selectByBranch<CellPointer>(isNotZero(handedBack), [&] {
        Machine::DeclaredAccessesSuspension readsCellThatNobodyHolds(m_out);
        CellPointer cell(handedBack.value());
        Integer link(m_out.load64(emitter<EnvironmentEmitter>().linkToNextCellHandedBack(cell, allocation.placed.layout)));
        m_out.store64(sub(link, integer(tagOfLinkBetweenEnvironmentsHandedBack)).value(), word);
        return cell;
    }, [&] {
        return emitter<CellAllocationEmitter>().allocate(value, allocation, ZeroedBy::Nobody);
    });
}

CellPointer AllocationEmitter::allocateCell(const ValueContext& value, const MentionedAllocation& allocation, const PlannedLayout& layout, AllocationStorage storage, ZeroedBy zeroedBy)
{
    RELEASE_ASSERT(zeroedBy == ZeroedBy::Nobody || storage == AllocationStorage::Heap);
    switch (storage) {
    case AllocationStorage::Heap:
        if (layout.cellsAreHandedBack) {
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
    const SlotPlan& plan = m_plan.slots();
    if (storage != AllocationStorage::Heap && !hasStateOfFrameCell)
        m_out.store32As8(m_out.constInt32(frameCellState), m_out.address(m_heaps.collectorState, cell.value(), JSCell::cellStateOffset()));

    for (const PlacedSlot* hiddenWord : { &plan.extensionWord(layout), &plan.extraPropertiesWord(layout) }) {
        if (hiddenWord->hasStorage() && zeroedBy != ZeroedBy::StubFromTemplate && (zeroedBy == ZeroedBy::Nobody || !isZeroWhileEmpty(hiddenWord->representation)))
            emitter<SlotEmitter>().storeAbsent(m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), hiddenWord->offset), hiddenWord->representation);
    }
}

Representation AllocationEmitter::representationWantedBy(const InitialField& field)
{
    return field.slot->representation == SlotRepresentation::DeferredClosure ? Representation::Cell32 : Typed::representationOf(field.slot->representation);
}

bool AllocationEmitter::isNarrowedWhereStored(const InitialField& field) const
{
    return representationOf(field.value) == Representation::Cell && representationWantedBy(field) == Representation::Cell32;
}

ALWAYS_INLINE_INTO_OTHER_FILES Operand AllocationEmitter::takeForSlot(const InitialField& field)
{
    return isNarrowedWhereStored(field) ? Operand(compress(in<CellPointer>(field.value))) : get(field.value);
}

ALWAYS_INLINE_INTO_OTHER_FILES void AllocationEmitter::storeInitialFields(CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    struct WordStoredOften {
        int64_t bits { 0 };
        unsigned numberOfStores { 0 };
        LValue inRegister { nullptr };
    };
    constexpr unsigned storesThatPayForRegister = 3;
    Vector<WordStoredOften, 4> words;
    auto findWordWrittenInto = [&](const InitialField& field) -> WordStoredOften* {
        static constexpr OpcodeSet wordsWithoutCode { Opcode::Undefined, Opcode::Null, Opcode::Boolean };
        if (!isX86_64() || field.slot->isOutOfLine() || field.slot->representation != SlotRepresentation::JSValue || !m_code.isConstant(field.value) || !wordsWithoutCode.contains(opcodeOf(field.value)))
            return nullptr;
        LValue written = get(field.value).value;
        if (!written->hasInt() || !written->asInt() || !isRepresentableAs<int32_t>(written->asInt()))
            return nullptr;
        for (WordStoredOften& word : words) {
            if (word.bits == written->asInt())
                return &word;
        }
        words.append(WordStoredOften { written->asInt() });
        return &words.last();
    };
    for (const InitialField& field : initialFields) {
        if (WordStoredOften* word = findWordWrittenInto(field))
            ++word->numberOfStores;
    }
    auto takeWordOrValue = [&](const InitialField& field) {
        Operand taken = takeForSlot(field);
        WordStoredOften* word = findWordWrittenInto(field);
        if (!word || word->numberOfStores < storesThatPayForRegister)
            return taken;
        if (!word->inRegister)
            word->inRegister = m_out.opaque(taken.value);
        return Operand(taken.representation, word->inRegister);
    };
    for (const InitialField& field : initialFields) {
        if (m_plan.layout(layout).holdsRawValueFromBirth(field.field))
            slots.markCellIfMarkerIsStored(cell, field.value);
        if (field.slot->isOutOfLine())
            continue;
        MemoryLocation location = locationOfField(m_program, m_plan.slots(), layout, field.field);
        if (SlotEmitter::storeLooksAtType(field.slot->representation))
            slots.storeSlot(cell, *field.slot, location, takeWordOrValue(field), nullptr, typeOf(field.value));
        else if (field.slot->representation != SlotRepresentation::DeferredClosure)
            slots.storeSlot(cell, *field.slot, location, takeWordOrValue(field));
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
    const FieldIndex numberOfFields = planned.numberOfFields();
    for (FieldIndex field = 0; field < numberOfFields; ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.storage != SlotStorage::Inline || isInitialField.get(field))
            continue;
        bool isWrittenBeforeRead = isCoroutineFrame && !traitsOf(slot.representation).holdsCell;
        if (!isWrittenBeforeRead)
            visitor(field, slot);
    }
}

AllocationEmitter::EmptySlots AllocationEmitter::collectEmptySlots(LayoutID layout, const BitVector& isInitialField)
{
    EmptySlots emptySlots;
    forEachEmptySlot(layout, isInitialField, [&](FieldIndex field, const PlacedSlot&) {
        emptySlots.append(field);
    });
    return emptySlots;
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

void AllocationEmitter::storeEmptySlots(CellPointer cell, LayoutID layout, std::span<const FieldIndex> emptySlots, ZeroedBy zeroedBy, BlockID block)
{
    if (zeroedBy == ZeroedBy::StubFromTemplate)
        return;
    SlotEmitter& slots = emitter<SlotEmitter>();
    const PlannedLayout& planned = m_plan.layout(layout);
    auto isStored = [&](const PlacedSlot& slot) {
        return zeroedBy == ZeroedBy::Nobody || !isZeroWhileEmpty(slot.representation);
    };
    auto fillsWord = [&](const PlacedSlot& slot) {
        return isStored(slot) && widthOf(slot.representation) == sizeof(uint64_t) && !(slot.offset % sizeof(uint64_t));
    };
    BitVector isStoredInLoop;
    bool storesWordsInLoop = false;
    auto fillMoreWordsThanAreStoredInLine = [&] {
        size_t numberOfWords = std::ranges::count_if(emptySlots, [&](FieldIndex field) { return fillsWord(planned.slots[field]); });
        return numberOfWords * sizeof(uint64_t) > CellAllocationEmitter::maxBytesZeroedInLine;
    };
    if (emptySlots.size() * sizeof(uint64_t) > CellAllocationEmitter::maxBytesZeroedInLine && !generator().isInLoop(block) && fillMoreWordsThanAreStoredInLine()) {
        size_t numberOfWords = planned.size / sizeof(uint64_t);
        Vector<int64_t, 64> wordsWhileEmpty(FillWith { }, numberOfWords, 0);
        BitVector isEmptyWord(numberOfWords);
        for (FieldIndex field : emptySlots) {
            const PlacedSlot& slot = planned.slots[field];
            if (!fillsWord(slot))
                continue;
            SlotTraits traits = traitsOf(slot.representation);
            wordsWhileEmpty[slot.offset / sizeof(uint64_t)] = traits.hasAbsent ? static_cast<int64_t>(traits.absentBits) : 0;
            isEmptyWord.quickSet(slot.offset / sizeof(uint64_t));
        }
        isStoredInLoop.ensureSize(numberOfWords);
        for (size_t begin = 0, end = 0; begin < numberOfWords; begin = end) {
            end = begin + 1;
            if (!isEmptyWord.quickGet(begin))
                continue;
            while (end < numberOfWords && isEmptyWord.quickGet(end) && wordsWhileEmpty[end] == wordsWhileEmpty[begin])
                ++end;
            constexpr size_t wordsInTurn = CellAllocationEmitter::bytesStoredInTurnOfLoop / sizeof(uint64_t);
            size_t wordsInChunks = (end - begin) / wordsInTurn * wordsInTurn;
            if ((end - begin) * sizeof(uint64_t) <= CellAllocationEmitter::maxBytesZeroedInLine)
                continue;
            emitter<CellAllocationEmitter>().storeWordInLoop(cell, m_program.at(layout), static_cast<unsigned>(begin * sizeof(uint64_t)), static_cast<unsigned>((begin + wordsInChunks) * sizeof(uint64_t)), wordsWhileEmpty[begin]);
            for (size_t word = begin; word < begin + wordsInChunks; ++word)
                isStoredInLoop.quickSet(word);
            storesWordsInLoop = true;
        }
    }
    for (FieldIndex field : emptySlots) {
        const PlacedSlot& slot = planned.slots[field];
        if (!isStored(slot) || (storesWordsInLoop && fillsWord(slot) && isStoredInLoop.get(slot.offset / sizeof(uint64_t))))
            continue;
        TypedPointer address = slots.addressOf(cell, slot, locationOfField(m_program, m_plan.slots(), layout, field));
        SlotTraits traits = traitsOf(slot.representation);
        if (traits.hasAbsent)
            slots.storeAbsent(address, slot.representation);
        else
            storeZero(m_out, address, traits.width);
    }
}

void AllocationEmitter::storeFieldsInExtension(const ValueContext& value, CellPointer cell, LayoutID layout, const InitialFields& initialFields)
{
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
    granuleBits |= m_plan.layout(layout).granulesOfFirstExtension;

    const PlacedSlot& extensionWord = m_plan.slots().extensionWord(layout);
    if (value->storage() == AllocationStorage::Heap && AOT::hasFlag(generator().build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime))
        emitter<BarrierEmitter>().stopIfCellIsOldAndNotRemembered(value, cell);
    unsigned size = extensionCellSizeFor(std::popcount(granuleBits));
    if (size <= maxExtensionSizeFromStub)
        callStub(value, allocateExtension(size), { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(granuleBits)) });
    else
        callVoidOperation(value, RuntimeOperation::EnsureExtensionGranules, { cell, integer(static_cast<int64_t>(granuleBits)) });

    SlotEmitter& slots = emitter<SlotEmitter>();
    bool storesCellInHeap = value->storage() == AllocationStorage::Heap && std::ranges::any_of(initialFields, [&](const InitialField& field) {
        return takesGranule(field) && traitsOf(field.slot->representation).holdsCell && typeOf(field.value).hasCells();
    });
    {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap(storesCellInHeap));
        Pointer extension = slots.extensionOf(cell, extensionWord);
        for (const InitialField& field : initialFields) {
            if (takesGranule(field))
                slots.storeInExtension(extension, *field.slot, locationOfField(m_program, m_plan.slots(), layout, field.field), takeForSlot(field));
        }
    }
    if (storesCellInHeap)
        emitter<BarrierEmitter>().fenceAfterLateStores();
    ensureStillAliveHere(cell);
}

bool AllocationEmitter::looksAtPrototypeBeforeAllocating(const ValueContext& value)
{
    if (!value->prototypeOperand() || !value->fields().empty() || value->storage() != AllocationStorage::Heap || !m_plan.layout(value->layout()).hasSlots())
        return false;
    return newObjectMayHaveOtherLayout(m_program, value.value()) || (!generator().isInLoop(value.block()) && !m_plan.layout(value->layout()).cellsAreHandedBack);
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
            storeEmptySlots(cell, layout, collectEmptySlots(layout, BitVector(m_plan.layout(layout).numberOfFields())).span(), ZeroedBy::Stub, value.block());
        }
        m_out.jump(isWhole);
        m_out.appendTo(isWhole);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return cell;
}

void AllocationEmitter::storePrototype(const ValueContext& value, CellPointer cell, CellPointer requested)
{
    Boxed sitePrototype = loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype);
    Condition hasSitePrototype = equal(boxCell(requested), sitePrototype);
    auto useCell = [&] {
        return cell;
    };
    auto setByOperation = [&] {
        MentionedAllocation allocation = emitter<CellAllocationEmitter>().allocationOf(value, value->layout(), value->fields());
        return callOperation<CellPointer>(value, RuntimeOperation::SetConstructPrototype, { cell, requested, numberOf(allocation.mention), word32(value->fields().empty()) }, { .isOnRarePath = true });
    };
    if (!m_plan.slots().hasDirectInstances(value->layout())) {
        define(value, setByOperation());
        return;
    }
    if (newObjectMayHaveOtherLayout(m_program, value.value()) || !generator().isInLoop(value.block())) {
        define(value, either<CellPointer>(hasSitePrototype, useCell, setByOperation));
        return;
    }

    define(value, selectByBranch<CellPointer>(hasSitePrototype, useCell, [&] {
        LBasicBlock isUnknown = newBlock();
        Join<CellPointer> join(m_out);
        Word32 structureID = emitter<AccessEmitter>().findPrototypeTransition(cell, boxCell(requested), isUnknown);
        {
            CodeGenerator::InitializationScope scope(generator());
            emitter<AccessEmitter>().storeStructureAfterPrototypeTransition(cell, structureID);
        }
        emitter<BarrierEmitter>().fenceAfterLateStores();
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

    EmptySlots emptySlots = collectEmptySlots(layoutID, isInitialField);
    ZeroedBy zeroedBy = whoZeroesBodyOfFreshCell(value, allocation, emptySlots.span(), storage);
    CellPointer cell = allocateCell(value, allocation, layout, storage, zeroedBy);
    {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap(storage == AllocationStorage::Heap));
        initializeHiddenFields(cell, layoutID, storage, zeroedBy, storage != AllocationStorage::Heap && initializesFrameObjectInStub(value, layoutID));
        storeInitialFields(cell, layoutID, initialFields);
        storeEmptySlots(cell, layoutID, emptySlots.span(), zeroedBy, value.block());
    }
    // A cell that was handed back is an old one: the collector may have marked it.
    bool mayHaveStoredCellInOldCell = storage == AllocationStorage::Heap && layout.cellsAreHandedBack && std::ranges::any_of(initialFields, [](const InitialField& field) {
        return traitsOf(field.slot->representation).holdsCell;
    });
    if (mayHaveStoredCellInOldCell)
        emitter<BarrierEmitter>().barrier(value, cell);
    // An Extension is stored into the cell behind the fence, and the collector may know a cell that was handed back: such a fence is left to no later one.
    bool keepsItsFence = layout.cellsAreHandedBack || std::ranges::any_of(initialFields, [](const InitialField& field) {
        return field.slot->isOutOfLine();
    });
    if (storage == AllocationStorage::Heap && keepsItsFence)
        emitter<BarrierEmitter>().fenceAfterInitialization();
    else if (storage == AllocationStorage::Heap)
        emitter<BarrierEmitter>().fenceAfterInitialization(value);
    storeFieldsInExtension(value, cell, layoutID, initialFields);
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
