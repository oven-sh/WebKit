/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParseEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowResults.h"
#include "AOTJSONKeyOrder.h"
#include "AOTJSONWords.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
static_assert(JSONKeyOrder::isListedBit == 1ull << 32, "A numbered order leaves the upper half of its word to the shape word");
static_assert(sizeof(KeyOrderTransition::orderAndShapeWordsBefore) == sizeof(uint64_t) && sizeof(KeyOrderTransition::key) == sizeof(uint32_t) && sizeof(KeyOrderTransition::orderAndCountAfter) == sizeof(uint32_t));
static_assert(sizeof(ObjectWords::keyOrderTransitionMask) == sizeof(uint32_t));
static_assert(argumentBufferAccessOf(RuntimeOperation::JSONParseExtraMembers) == ArgumentBufferAccess::ReadsAndWrites, "updateInBuffer loads what the operation stored in the buffer");

static constexpr int32_t parsedOutcome = static_cast<int32_t>(JSONOutcome::Parsed);
static constexpr int32_t rejectedOutcome = static_cast<int32_t>(JSONOutcome::Rejected);
static constexpr int32_t unknownOutcome = static_cast<int32_t>(JSONOutcome::Unknown);

static constexpr unsigned logTransitionSize = std::countr_zero(sizeof(KeyOrderTransition));
static_assert(sizeof(KeyOrderTransition) == 1u << logTransitionSize);

static constexpr uint32_t constantHashTerms(uint32_t shapeWord, uint32_t slotKey)
{
    return (shapeWord * KeyOrderTransition::hashMultiplier) ^ slotKey;
}
static_assert([] {
    constexpr uint32_t product = (5u ^ constantHashTerms(3, 7)) * KeyOrderTransition::hashMultiplier;
    return KeyOrderTransition::hashOf(3ull << 32 | 5, 7) == (product ^ product >> KeyOrderTransition::hashFoldShift);
}());

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
    Word32 product = mul(bitXor(low32(orderAndCount), static_cast<int32_t>(constantHashTerms(shapeWord, slotKey))), static_cast<int32_t>(KeyOrderTransition::hashMultiplier));
    Word32 hash = bitXor(product, shiftRightUnsigned(product, word32(KeyOrderTransition::hashFoldShift)));
    // A table that grows moves, and the pages it leaves are unmapped: what an earlier probe loaded is loaded again.
    B3::PatchpointValue* clobber = m_out.patchpoint(B3::Void);
    clobber->effects = B3::Effects::none();
    m_heaps.decoratePatchpointWrite(&m_heaps.AOTInstance_mutableFields, clobber);
    clobber->setRunsWithoutFrame(true);
    clobber->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
    Word32 mask = loadMutable32(AOT_INSTANCE_OFFSET(objects.keyOrderTransitionMask));
    Pointer transitions(loadMutable64(AOT_INSTANCE_OFFSET(objects.keyOrderTransitions)).value());
    ValueFromBlock firstIndex = m_out.anchor(hash.value());
    m_out.jump(probe);

    m_out.appendTo(probe);
    Word32 index(m_out.phi(B3::Int32, firstIndex));
    Pointer transition = offsetBy(transitions, zeroExtended(bitAnd(index, mask)), logTransitionSize);
    auto inTransition = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), transition.value(), offset);
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

uint32_t JSONParseEmitter::shapeOfParsedObject(const ValueContext& value)
{
    const FlowResults& flow = inputs().flow;
    LayoutID layout = value->layout();
    AllocationSiteID site = flow.typedResultSite(layout);
    uint32_t shape = m_placement.shapes().shapeOf(layout, site ? flow.at(site).allocationClass : AllocationClassID());
    if (!shape)
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
    return shape;
}

Pointer JSONParseEmitter::advanceKeyOrder(const ValueContext& value, Pointer order)
{
    uint32_t shape = shapeOfParsedObject(value);
    if (!shape)
        return order;
    const PlacedSlot& slot = m_placement.slot(value->layout(), value->field());
    if (m_placement.shapes().shape(shape).keysAreNeverEnumerated || !slot.indexInLayoutRecord)
        return order;

    auto advanceByOperation = [&](bool isOnRarePath) {
        return callOperation<Pointer>(value, RuntimeOperation::JSONObjectKey, { order, word32(static_cast<int32_t>(shape)), word32(*slot.indexInLayoutRecord) }, { .isOnRarePath = isOnRarePath });
    };
    uint32_t slotKey = slotKeyFor(slot.offset, slot.isOutOfLine());
    if (value.shape() == CodeShape::Compact || slotKey == noSlotKey)
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
    bool storesOrder = !m_placement.shapes().shape(shape).keysAreNeverEnumerated;
    bool mayHaveExtraProperties = !typeOf(value->extraOperand()).hasOnlyPrimitives(Primitive::Undefined);
    if (!storesOrder && !mayHaveExtraProperties) {
        define(value, word32(parsedOutcome));
        return;
    }

    auto finishByOperation = [&](bool isOnRarePath) {
        return low32(callOperation<Integer>(value, RuntimeOperation::JSONObjectFinish, { object, extra, order }, { .isOnRarePath = isOnRarePath }));
    };
    if (value.shape() == CodeShape::Compact) {
        define(value, finishByOperation(false));
        return;
    }

    LBasicBlock slowPath = newBlock();
    Join<Word32> outcome(m_out);
    if (mayHaveExtraProperties)
        branchUnless(equal(extra, undefined()), slowPath);
    branchUnless(below(order, pointerConstant(JSONKeyOrder::isListedBit)), slowPath);
    if (storesOrder)
        storeField(object, m_heaps.AOTObject_orderWord, low32(Integer(order.value())));
    outcome.add(word32(parsedOutcome));

    m_out.appendTo(slowPath);
    outcome.add(finishByOperation(true));
    define(value, outcome.finish());
}

void JSONParseEmitter::generateJSONParseExtraMembers(const ValueContext& value)
{
    uint32_t shape = shapeOfParsedObject(value);
    if (!shape)
        return;
    Pointer cursor = cursorOf(value);
    ExtraMemberState state { in<Boxed>(value->extraOperand()), in<Pointer>(value->orderOperand()) };
    auto parse = [&](bool isOnRarePath) {
        Pointer next;
        state = updateInBuffer(state, [&](Pointer buffer) {
            next = callOperation<Pointer>(value, RuntimeOperation::JSONParseExtraMembers, { word32(static_cast<int32_t>(shape)), cursor, buffer }, { .isOnRarePath = isOnRarePath });
        });
        return next;
    };

    Pointer next;
    const Type& typeOfExtra = typeOf(value->extraOperand());
    bool appendsToList = !typeOfExtra.primitives && typeOfExtra.findClassPart(BuiltinClasses::Array);
    if (!appendsToList && !typeOfExtra.hasOnlyPrimitives(Primitive::Null))
        next = parse(false);
    else {
        LBasicBlock wasSeenWithExtraMembers = newBlock();
        LBasicBlock continuation = newBlock();
        unsigned bit = shape % numberOfJSONShapeBits;
        Word32 bits = loadMutable32(AOT_INSTANCE_OFFSET(json.shapesSeenWithExtraMembers) + bit / 32 * sizeof(uint32_t));
        ValueFromBlock sameCursor = m_out.anchor(cursor.value());
        ValueFromBlock noExtra = m_out.anchor(undefined().value());
        ValueFromBlock sameOrder = m_out.anchor(state.order.value());
        branch(isZero(bitAnd(bits, word32(static_cast<int32_t>(1u << bit % 32)))), { normalEdge(continuation), unlikelyEdge(wasSeenWithExtraMembers) });

        m_out.appendTo(wasSeenWithExtraMembers);
        ValueFromBlock nextCursor = m_out.anchor(parse(true).value());
        ValueFromBlock extra = m_out.anchor(state.extra.value());
        ValueFromBlock order = m_out.anchor(state.order.value());
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        next = Pointer(m_out.phi(B3::Int64, sameCursor, nextCursor));
        state = { Boxed(m_out.phi(B3::Int64, noExtra, extra)), Pointer(m_out.phi(B3::Int64, sameOrder, order)) };
    }
    if (appendsToList) {
        defineResults(value, { word32(parsedOutcome), state.extra, next, state.order });
        return;
    }
    Condition isParsed(m_out.notNull(next.value()));
    Word32 outcome = select(isParsed, select<Word32>(equal(next, cursor), rejectedOutcome, parsedOutcome), unknownOutcome);
    defineResults(value, { outcome, state.extra, cursorOrSourceEnd(isParsed, next.value()), state.order });
}

// No shape: the objects of the layout keep no order of their keys.
uint32_t JSONParseEmitter::shapeWithKeyOrderOf(LayoutID layout) const
{
    const ShapeTable& shapes = m_placement.shapes();
    ShapeRange range = shapes.shapesOf(layout);
    return range.isEmpty() || shapes.shape(range.first).keysAreNeverEnumerated ? 0 : range.first;
}

void JSONParseEmitter::generateGetOrderWord(const ValueContext& value)
{
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    CellPointer object = in<CellPointer>(value->objectOperand());
    Word32 orderWord = shapeTests.orderWordOf(object);
    // The word of an object of the layout's other shape, which keeps no order, says nothing about its keys.
    uint32_t shape = shapeWithKeyOrderOf(value->layout());
    if (shape && m_placement.shapes().shapesOf(value->layout()).count > 1)
        orderWord = select(equal(shapeTests.shapeWordOf(object), word32(static_cast<int32_t>(shapeWordOf(shape)))), orderWord, word32(static_cast<int32_t>(outOfLineOrderWord)));
    define(value, zeroExtended(orderWord));
}

void JSONParseEmitter::generateGetFieldInKeyOrder(const ValueContext& value)
{
    static constexpr int32_t orderHasEnded = -1;
    static constexpr int32_t orderIsNotListed = -2;
    static constexpr size_t maxFieldsWithoutRecord = 4;
    static_assert(sizeof(KeyOrderEntriesCacheEntry) == 16);

    // An order lists the fields by their places among the fields with a record, which stand in the sequence of the fields.
    LayoutID layout = value->layout();
    uint32_t shape = shapeWithKeyOrderOf(layout);
    const Vector<Field>& fields = m_program.at(layout).fields;
    Vector<uint32_t, maxFieldsWithoutRecord> firstRecordsBehindGaps;
    bool listsEveryKey = !!shape;
    uint32_t numberOfRecords = 0;
    for (FieldIndex field = 0; field < fields.size() && listsEveryKey; ++field) {
        if (m_placement.slot(layout, field).indexInLayoutRecord) {
            ++numberOfRecords;
            continue;
        }
        listsEveryKey = !isEnumerableStringField(m_program, fields[field]) && firstRecordsBehindGaps.size() < maxFieldsWithoutRecord;
        firstRecordsBehindGaps.append(numberOfRecords);
    }
    if (!listsEveryKey) {
        define(value, word32(orderIsNotListed));
        return;
    }

    Word32 orderWord = low32(integerOf(value->orderOperand()));
    Word32 position = low32(integerOf(value->positionOperand()));
    Word32 shapeWord = word32(static_cast<int32_t>(shapeWordOf(shape)));
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    LBasicBlock hasShape = newBlock();
    LBasicBlock isCached = newBlock();
    LBasicBlock isNotCached = newBlock();
    LBasicBlock isListed = newBlock();
    LBasicBlock isNotListed = newBlock();
    LBasicBlock hasKey = newBlock();
    LBasicBlock hasEnded = newBlock();
    Join<Pointer> entries(m_out);
    Join<Word32> result(m_out);

    Word32 hash = add(add(shapeWord, orderWord), shiftRightUnsigned(orderWord, keyOrderEntriesHashShift));
    static_assert(indexOfKeyOrderEntries(3, (5u << orderWordCountShift) + 4) == 3 + 4 + (5 << 5));
    LValue cached = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.keyOrderEntries)), zeroExtended(bitAnd(hash, numberOfCachedKeyOrderEntries - 1)), 4).value();
    branch(equal(Word32(m_out.load32(m_out.address(heap, cached, OBJECT_OFFSETOF(KeyOrderEntriesCacheEntry, shapeWord)))), shapeWord), { normalEdge(hasShape), rareEdge(isNotCached) });

    m_out.appendTo(hasShape);
    branch(equal(Word32(m_out.load32(m_out.address(heap, cached, OBJECT_OFFSETOF(KeyOrderEntriesCacheEntry, orderWord)))), orderWord), { normalEdge(isCached), rareEdge(isNotCached) });

    m_out.appendTo(isCached);
    entries.add(Pointer(m_out.loadPtr(m_out.address(heap, cached, OBJECT_OFFSETOF(KeyOrderEntriesCacheEntry, entries)))));

    m_out.appendTo(isNotCached);
    entries.add(callOperation<Pointer>(value, RuntimeOperation::KeyOrderEntries, { word32(static_cast<int32_t>(shape)), orderWord }, { .isOnRarePath = true }));

    Pointer list = entries.finish();
    branch(Condition(m_out.notNull(list.value())), { normalEdge(isListed), rareEdge(isNotListed) });

    m_out.appendTo(isListed);
    Word32 count = bitAnd(shiftRightUnsigned(orderWord, orderWordCountShift), maxFieldsInOrder);
    branch(below(position, count), { normalEdge(hasKey), normalEdge(hasEnded) });

    m_out.appendTo(hasKey);
    Word32 record(m_out.load16ZeroExt32(m_out.address(heap, offsetBy(list, zeroExtended(position), 1).value(), 0)));
    Word32 field = record;
    for (uint32_t firstRecord : firstRecordsBehindGaps)
        field = add(field, low32(zeroExtended(aboveOrEqual(record, word32(static_cast<int32_t>(firstRecord))))));
    result.add(field);

    m_out.appendTo(hasEnded);
    result.add(word32(orderHasEnded));

    m_out.appendTo(isNotListed);
    result.add(word32(orderIsNotListed));

    define(value, result.finish());
}

void JSONParseEmitter::generateGetKeyNamesInOrder(const ValueContext& value)
{
    // Where the objects keep no order, no position in an order is ever found.
    uint32_t shape = shapeWithKeyOrderOf(value->layout());
    if (!shape) {
        define(value, undefined());
        return;
    }
    define(value, callOperation<Boxed>(value, RuntimeOperation::KeyNamesInOrder, { word32(static_cast<int32_t>(shape)), low32(integerOf(value->orderOperand())), low32(integerOf(value->positionOperand())) }, { .isOnRarePath = true }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
