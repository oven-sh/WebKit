/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParseEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTJSONKeyOrder.h"
#include "AOTJSONWords.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeCallEmitter.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
static_assert(JSONKeyOrder::isListedBit == 1ull << 32, "A numbered order leaves the upper half of its word to the shape word");
static_assert(sizeof(KeyOrderTransition::orderAndShapeWordsBefore) == sizeof(uint64_t) && sizeof(KeyOrderTransition::key) == sizeof(uint32_t) && sizeof(KeyOrderTransition::orderAndCountAfter) == sizeof(uint32_t));
static_assert(argumentBufferAccessOf(RuntimeOperation::JSONParseExtraMembers) == ArgumentBufferAccess::ReadsAndWrites, "updateInBuffer loads what the operation stored in the buffer");

static constexpr int32_t parsedOutcomeOfJSON = static_cast<int32_t>(JSONOutcome::Parsed);
static constexpr int32_t rejectedOutcomeOfJSON = static_cast<int32_t>(JSONOutcome::Rejected);
static constexpr int32_t unknownOutcomeOfJSON = static_cast<int32_t>(JSONOutcome::Unknown);

static constexpr unsigned logSizeOfKeyOrderTransition = std::countr_zero(sizeof(KeyOrderTransition));
static_assert(sizeof(KeyOrderTransition) == 1u << logSizeOfKeyOrderTransition);

static constexpr uint32_t constantHashTermsOfKeyOrderTransition(uint32_t shapeWord, uint32_t slotKey)
{
    return (shapeWord * KeyOrderTransition::hashMultiplier) ^ slotKey;
}
static_assert([] {
    constexpr uint32_t product = (5u ^ constantHashTermsOfKeyOrderTransition(3, 7)) * KeyOrderTransition::hashMultiplier;
    return KeyOrderTransition::hashOf(3ull << 32 | 5, 7) == (product ^ product >> KeyOrderTransition::hashFoldShift);
}());

// The step that a store which creates the field takes: the table of transitions that the run time has published, probed from the hash on.
Pointer JSONParseEmitter::findOrderAfter(Pointer numberedOrder, uint32_t shape, uint32_t slotKey, LBasicBlock miss)
{
    uint32_t shapeWord = shapeWordOf(shape);
    Integer orderAndCount(numberedOrder.value());
    LBasicBlock probe = newBlock();
    LBasicBlock hasSameWords = newBlock();
    LBasicBlock hasOtherWords = newBlock();
    LBasicBlock nextTransition = newBlock();
    LBasicBlock found = newBlock();

    Integer wordsBefore = bitOr(orderAndCount, integer(static_cast<int64_t>(static_cast<uint64_t>(shapeWord) << 32)));
    Word32 product = mul(bitXor(low32(orderAndCount), static_cast<int32_t>(constantHashTermsOfKeyOrderTransition(shapeWord, slotKey))), static_cast<int32_t>(KeyOrderTransition::hashMultiplier));
    Word32 hash = bitXor(product, shiftRightUnsigned(product, word32(KeyOrderTransition::hashFoldShift)));
    // A table that grows moves, and the pages it leaves are unmapped. Whatever grows it writes RuntimeState, which ends these two loads.
    const Machine::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    Word32 mask(m_out.load32(instanceField(heap, AOT_INSTANCE_OFFSET(objects.keyOrderTransitionMask))));
    Pointer transitions(m_out.loadPtr(instanceField(heap, AOT_INSTANCE_OFFSET(objects.keyOrderTransitions))));
    ValueFromBlock firstIndex = m_out.anchor(hash.value());
    m_out.jump(probe);

    m_out.appendTo(probe);
    Word32 index(m_out.phi(Machine::Int32, firstIndex));
    Pointer transition = offsetBy(transitions, zeroExtended(bitAnd(index, mask)), logSizeOfKeyOrderTransition);
    auto inTransition = [&](ptrdiff_t offset) {
        return m_out.address(heap, transition.value(), offset);
    };
    Integer wordsInTransition(m_out.load64(inTransition(KeyOrderTransition::offsetOfOrderAndShapeWordsBefore())));
    branch(equal(wordsInTransition, wordsBefore), { normalEdge(hasSameWords), unlikelyEdge(hasOtherWords) });

    m_out.appendTo(hasSameWords);
    Word32 slotKeyInTransition(m_out.load32(inTransition(KeyOrderTransition::offsetOfKey())));
    branch(equal(slotKeyInTransition, word32(static_cast<int32_t>(slotKey))), { normalEdge(found), unlikelyEdge(nextTransition) });

    m_out.appendTo(hasOtherWords);
    branch(isZero(wordsInTransition), { rareEdge(miss), normalEdge(nextTransition) });

    m_out.appendTo(nextTransition);
    m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, word32(1)).value()));
    m_out.jump(probe);

    m_out.appendTo(found);
    Word32 orderAndCountAfter(m_out.load32(inTransition(KeyOrderTransition::offsetOfOrderAndCountAfter())));
    return Pointer(zeroExtended(orderAndCountAfter).value());
}

// No parser names an allocation class, and neither does the NewObject of one: both speak of the general shape of the layout.
uint32_t JSONParseEmitter::shapeOfParsedObject(const ValueContext& value)
{
    uint32_t shape = value->allocationClass() ? 0 : m_plan.shapes().generalShapeOf(value->layout());
    if (!shape)
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
    return shape;
}

Pointer JSONParseEmitter::advanceKeyOrder(const ValueContext& value, Pointer order)
{
    uint32_t shape = shapeOfParsedObject(value);
    if (!shape)
        return order;
    const PlacedSlot& slot = m_plan.slot(value->layout(), value->field());
    if (m_plan.shapes().shape(shape).keysAreNeverEnumerated || !slot.indexInLayoutRecord)
        return order;

    auto advanceByOperation = [&](bool isOnRarePath) {
        return callOperation<Pointer>(value, RuntimeOperation::JSONObjectKey, { order, word32(static_cast<int32_t>(shape)), word32(*slot.indexInLayoutRecord) }, { .isOnRarePath = isOnRarePath });
    };
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (slotKey == noSlotKey)
        return advanceByOperation(false);

    LBasicBlock isNumbered = newBlock();
    LBasicBlock miss = newBlock();
    Join<Pointer> next(m_out);
    branch(below(order, pointerConstant(JSONKeyOrder::isListedBit)), { normalEdge(isNumbered), rareEdge(miss) });

    m_out.appendTo(isNumbered);
    next.add(findOrderAfter(order, shape, slotKey, miss));

    m_out.appendTo(miss);
    next.add(advanceByOperation(true));
    return next.finish();
}

JSONParseEmitter::ExtraMemberState JSONParseEmitter::updateInBuffer(ExtraMemberState state, NOESCAPE const Invocable<void(Pointer buffer)> auto& call)
{
    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(2);
    TypedPointer extraWord = m_out.address(m_heaps.argumentBuffer, buffer.value());
    TypedPointer orderWord = m_out.address(m_heaps.argumentBuffer, buffer.value(), sizeof(uint64_t));
    m_out.store64(state.extra.value(), extraWord);
    m_out.store64(state.order.value(), orderWord);
    call(buffer);
    return { Boxed(m_out.load64(extraWord)), Pointer(m_out.load64(orderWord)) };
}

void JSONParseEmitter::generateJSONObjectBegin(const ValueContext& value)
{
    define(value, pointerConstant(JSONKeyOrder().word()));
}

void JSONParseEmitter::generateJSONObjectFinish(const ValueContext& value)
{
    uint32_t shape = shapeOfParsedObject(value);
    if (!shape)
        return;
    Pointer order = in<Pointer>(value->orderOperand());
    CellPointer object = in<CellPointer>(value->objectOperand());
    Boxed extra = in<Boxed>(value->extraOperand());
    bool storesOrder = !m_plan.shapes().shape(shape).keysAreNeverEnumerated;
    bool mayHaveExtraProperties = !typeOf(value->extraOperand()).hasOnlyPrimitives(Primitive::Undefined);
    if (!storesOrder && !mayHaveExtraProperties) {
        define(value, word32(parsedOutcomeOfJSON));
        return;
    }

    LBasicBlock slowPath = newBlock();
    Join<Word32> outcome(m_out);
    if (mayHaveExtraProperties)
        branchUnless(equal(extra, undefined()), slowPath);
    branchUnless(below(order, pointerConstant(JSONKeyOrder::isListedBit)), slowPath);
    // Nobody has seen the object. A member that may be left out was defined by its name, though: where that has left a flag in the order word, the run time looks.
    if (storesOrder) {
        branchUnless(hasNoBits(loadField<Word32>(object, m_heaps.AOTObject_orderWord), ~orderAndCountMask), slowPath);
        storeField(object, m_heaps.AOTObject_orderWord, low32(Integer(order.value())));
    }
    outcome.add(word32(parsedOutcomeOfJSON));

    m_out.appendTo(slowPath);
    outcome.add(low32(callOperation<Integer>(value, RuntimeOperation::JSONObjectFinish, { object, extra, order }, { .isOnRarePath = true })));
    define(value, outcome.finish());
}

void JSONParseEmitter::generateJSONParseExtraMembers(const ValueContext& value)
{
    uint32_t shape = shapeOfParsedObject(value);
    if (!shape)
        return;
    Pointer cursor = cursorOf(value);
    // The operation tells by the first word of the buffer what it is asked for.
    bool appendsElements = !!value->listOperand();
    ExtraMemberState state { appendsElements ? boxCell(in<CellPointer>(value->listOperand())) : in<Boxed>(value->extraOperand()), in<Pointer>(value->orderOperand()) };
    auto parse = [&](bool isOnRarePath) {
        Pointer end;
        state = updateInBuffer(state, [&](Pointer buffer) {
            end = callOperation<Pointer>(value, RuntimeOperation::JSONParseExtraMembers, { word32(static_cast<int32_t>(shape)), cursor, buffer }, { .isOnRarePath = isOnRarePath });
        });
        return end;
    };

    Pointer next;
    if (!value->parsesWholeObject() && !appendsElements)
        next = parse(false);
    else {
        // While the bit of the shape is clear the run time is not asked, and the answer is the one that it gives where it knows no such form: the cursor itself.
        LBasicBlock wasSeenWithExtraMembers = newBlock();
        LBasicBlock continuation = newBlock();
        unsigned bit = shape % numberOfJSONShapeBits;
        Word32 bits(m_out.load32(instanceField(m_heaps.entire(AbstractHeap::RuntimeState), AOT_INSTANCE_OFFSET(json.shapesSeenWithExtraMembers) + bit / 32 * sizeof(uint32_t))));
        ValueFromBlock sameCursor = m_out.anchor(cursor.value());
        ValueFromBlock sameExtra = m_out.anchor(state.extra.value());
        ValueFromBlock sameOrder = m_out.anchor(state.order.value());
        branch(hasNoBits(bits, 1u << bit % 32), { normalEdge(continuation), unlikelyEdge(wasSeenWithExtraMembers) });

        m_out.appendTo(wasSeenWithExtraMembers);
        ValueFromBlock nextCursor = m_out.anchor(parse(true).value());
        ValueFromBlock extra = m_out.anchor(state.extra.value());
        ValueFromBlock order = m_out.anchor(state.order.value());
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        next = Pointer(m_out.phi(Machine::Int64, sameCursor, nextCursor));
        state = { Boxed(m_out.phi(Machine::Int64, sameExtra, extra)), Pointer(m_out.phi(Machine::Int64, sameOrder, order)) };
    }
    // Wherever the appended objects end, the elements from there on are parsed like those of any other list.
    if (appendsElements) {
        defineResults(value, { word32(parsedOutcomeOfJSON), undefined(), next, state.order });
        return;
    }
    Condition isParsed(m_out.notNull(next.value()));
    Word32 outcome = select(isParsed, select<Word32>(equal(next, cursor), rejectedOutcomeOfJSON, parsedOutcomeOfJSON), unknownOutcomeOfJSON);
    defineResults(value, { outcome, state.extra, cursorOrSourceEnd(isParsed, next.value()), state.order });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
