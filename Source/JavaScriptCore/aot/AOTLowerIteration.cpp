/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "AOTOperationsObjects.h"
#include "B3ValueInlines.h"
#include "BytecodeOperandsForCheckpoint.h"
#include "BytecodeStructs.h"
#include "JSArrayIterator.h"
#include "JSPropertyNameEnumerator.h"
#include "StructureRareData.h"
#include "JSCInlines.h"
#include "Watchpoint.h"

namespace JSC { namespace AOT {

using namespace B3;

LValue Lowering::inlineWatchpointSetIsStillValid(LValue set)
{
    LBasicBlock notThinInvalidated = m_out.newBlock();
    LBasicBlock fat = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    LValue data = m_out.loadPtr(set, m_heaps.InlineWatchpointSet_data);
    results.append(m_out.anchor(m_out.booleanFalse));
    m_out.branch(m_out.equal(data, m_out.constIntPtr(InlineWatchpointSet::encodeState(IsInvalidated))), rarely(continuation), usually(notThinInvalidated));

    m_out.appendTo(notThinInvalidated, fat);
    results.append(m_out.anchor(m_out.booleanTrue));
    m_out.branch(m_out.testNonZeroPtr(data, m_out.constIntPtr(InlineWatchpointSet::IsThinFlag)), unsure(continuation), unsure(fat));

    m_out.appendTo(fat, continuation);
    results.append(m_out.anchor(m_out.notEqual(m_out.load8ZeroExt32(data, m_heaps.WatchpointSet_state), m_out.constInt32(IsInvalidated))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, results);
}

void Lowering::checkIsObjectOrThrowIteratorResultIsNotObject(Node* node, LValue value)
{
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock notObject = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value), usually(cellCase), rarely(notObject));
    m_out.appendTo(cellCase, notObject);
    m_out.branch(isObjectCell(value), usually(continuation), rarely(notObject));
    m_out.appendTo(notObject, continuation);
    vmCall(node, Void, Entry::operationAOTThrowIteratorResultIsNotObject, m_globalObject);
    m_out.unreachable();
    m_out.appendTo(continuation);
}

// iterator = symbolIterator.call(iterable); next = iterator.next. Unless the runtime knows a shortcut for the iterable, in which
// case next is a marker (a sentinel cell, or a number if iterator is one) that tells op_iterator_next which.
void Lowering::lowerIteratorOpen(Node* node, bool isAsync)
{
    VirtualRegister iteratorRegister, nextRegister, symbolIteratorRegister, iterableRegister;
    if (isAsync) {
        auto bytecode = node->as<OpAsyncIteratorOpen>();
        iteratorRegister = bytecode.m_iterator;
        nextRegister = bytecode.m_next;
        symbolIteratorRegister = bytecode.m_symbolIterator;
        iterableRegister = bytecode.m_iterable;
    } else {
        auto bytecode = node->as<OpIteratorOpen>();
        iteratorRegister = bytecode.m_iterator;
        nextRegister = bytecode.m_next;
        symbolIteratorRegister = bytecode.m_symbolIterator;
        iterableRegister = bytecode.m_iterable;
    }
    LValue iterable = lowJSValue(node->use(iterableRegister));
    if (Graph::iteratedFactOf(node)) {
        setProj(node, iteratorRegister, iterable);
        setProj(node, nextRegister, m_out.constInt64(JSValue::encode(jsNumber(0))));
        return;
    }
    LValue symbolIterator = lowJSValue(node->use(symbolIteratorRegister));

    if (usesStubs && !isAsync) {
        // Every loop has to be ready for anything, and how that is done is the same for all of them.
        PatchpointValue* opened = callStub(Stub::IteratorOpen, m_proc.addTuple({ Int64, Int64 }),
            { { iterable, GPRInfo::argumentGPR0 }, { symbolIterator, GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, static_cast<unsigned>(WellKnownIdentifier::Next))), GPRInfo::argumentGPR2 } },
            { });
        opened->resultConstraints = { ValueRep::reg(GPRInfo::argumentGPR0), ValueRep::reg(GPRInfo::argumentGPR1) };
        setProj(node, iteratorRegister, m_out.extract(opened, 0));
        setProj(node, nextRegister, m_out.extract(opened, 1));
        return;
    }

    LBasicBlock fastCase = m_out.newBlock();
    LBasicBlock genericCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    // An array as the realm makes them is gone through by its index, and there is no iterator: what the runtime would say (IterationMode::FastArray).
    std::optional<ValueFromBlock> iteratorOfArray;
    std::optional<ValueFromBlock> nextOfArray;
    if (!isAsync && !isWithout(WithoutIteratorOpen) && Options::useImmutableIntrinsics() && Options::useUnboxedFastArrayIteration() && mayBe(node->use(iterableRegister)->type, TArray)) {
        LBasicBlock isArray = m_out.newBlock();
        LBasicBlock isSomethingElse = m_out.newBlock();
        m_out.branch(isCellAnd(node->use(iterableRegister), iterable, [&](LValue cell) { return isOriginalArray(cell); }), unsure(isArray), unsure(isSomethingElse));
        m_out.appendTo(isArray);
        iteratorOfArray = m_out.anchor(fixedPointer(Instance::offsetOfSentinelOfArrayIteration()));
        nextOfArray = m_out.anchor(m_out.constInt64(JSValue::encode(jsNumber(0))));
        m_out.jump(continuation);
        m_out.appendTo(isSomethingElse);
    }

    LValue fastIterator = vmCall(node, Int64, isAsync ? Entry::operationAOTAsyncIteratorOpenTryFast : Entry::operationAOTIteratorOpenTryFast, m_globalObject, iterable, symbolIterator, scratchAddress());
    m_out.branch(m_out.notZero64(fastIterator), unsure(fastCase), unsure(genericCase));

    m_out.appendTo(fastCase, genericCase);
    ValueFromBlock fastIteratorResult = m_out.anchor(fastIterator);
    ValueFromBlock fastNextResult = m_out.anchor(m_out.load64(scratchWord(0)));
    m_out.jump(continuation);

    m_out.appendTo(genericCase, continuation);
    LValue iterator = emitCall(node, symbolIterator, Arguments { iterable });
    checkIsObjectOrThrowIteratorResultIsNotObject(node, iterator);
    LValue next = getByIdCached(node, iterator, TAnyObject, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::Next));
    ValueFromBlock genericIteratorResult = m_out.anchor(iterator);
    ValueFromBlock genericNextResult = m_out.anchor(next);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    Vector<ValueFromBlock, 3> iterators { fastIteratorResult, genericIteratorResult };
    Vector<ValueFromBlock, 3> nexts { fastNextResult, genericNextResult };
    if (iteratorOfArray) {
        iterators.append(*iteratorOfArray);
        nexts.append(*nextOfArray);
    }
    setProj(node, iteratorRegister, m_out.phi(Int64, iterators));
    setProj(node, nextRegister, m_out.phi(Int64, nexts));
}

// result = next.call(iterator); done = result.done; value = done ? (nothing anybody looks at) : result.value. Or the shortcut.
void Lowering::lowerIteratorNext(Node* node)
{
    auto bytecode = node->as<OpIteratorNext>();
    Node* nextNode = node->use(bytecode.m_next);
    Node* iteratorNode = node->use(bytecode.m_iterator);
    LValue next = lowJSValue(nextNode);
    LValue iterator = lowJSValue(iteratorNode);
    LValue iterable = lowJSValue(node->use(bytecode.m_iterable));

    if (Graph::iteratedFactOf(node)) {
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = unboxInt32(next);
        LValue butterfly = m_out.loadPtr(iterable, m_heaps.JSObject_butterfly);
        LValue isInBounds = m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength));
        ValueFromBlock noValue = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.branch(isInBounds, usually(inBounds), rarely(continuation));
        m_out.appendTo(inBounds, continuation);
        ValueFromBlock element = m_out.anchor(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index))));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setProj(node, bytecode.m_done, boxBoolean(m_out.logicalNot(isInBounds)));
        setProj(node, bytecode.m_value, m_out.phi(Int64, noValue, element));
        setProj(node, bytecode.m_next, boxInt32(m_out.add(index, m_out.int32One)));
        return;
    }

    if constexpr (usesStubs) {
        // Every loop over an array would have a copy of how that is done, and every loop has to be ready for anything else: of which every loop would have a copy as well.
        unsigned slotOfDone = allocateSite(node, static_cast<unsigned>(WellKnownIdentifier::Done));
        unsigned slotOfValue = allocateSite(node, static_cast<unsigned>(WellKnownIdentifier::Value));
        RELEASE_ASSERT(slotOfValue == slotOfDone + 1);
        PatchpointValue* result = callStub(Stub::IteratorNext, m_proc.addTuple({ Int64, Int64, Int64 }),
            { { next, GPRInfo::argumentGPR0 }, { iterator, GPRInfo::argumentGPR1 }, { iterable, GPRInfo::argumentGPR2 }, { slotAddress(slotOfDone), GPRInfo::argumentGPR3 } },
            { });
        result->resultConstraints = { ValueRep::reg(GPRInfo::argumentGPR0), ValueRep::reg(GPRInfo::argumentGPR1), ValueRep::reg(GPRInfo::argumentGPR2) };
        setProj(node, bytecode.m_done, m_out.extract(result, 0));
        setProj(node, bytecode.m_value, m_out.extract(result, 1));
        setProj(node, bytecode.m_next, m_out.extract(result, 2));
        return;
    }

    LBasicBlock nextIsCell = m_out.newBlock();
    LBasicBlock nextIsNotCell = m_out.newBlock();
    LBasicBlock markedCase = m_out.newBlock();
    LBasicBlock indexCase = m_out.newBlock();
    LBasicBlock indexIsInt32 = m_out.newBlock();
    LBasicBlock iterableIsCell = m_out.newBlock();
    LBasicBlock iterableIsArray = m_out.newBlock();
    LBasicBlock rightShape = m_out.newBlock();
    LBasicBlock inBounds = m_out.newBlock();
    LBasicBlock indexFast = m_out.newBlock();
    LBasicBlock indexSlow = m_out.newBlock();
    LBasicBlock genericCase = m_out.newBlock();
    LBasicBlock notDone = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    Vector<ValueFromBlock, 5> doneResults;
    Vector<ValueFromBlock, 5> valueResults;
    Vector<ValueFromBlock, 5> nextResults;
    auto finish = [&](LValue done, LValue value, LValue newNext) {
        doneResults.append(m_out.anchor(done));
        valueResults.append(m_out.anchor(value));
        nextResults.append(m_out.anchor(newNext));
    };
    // The operations behind the shortcuts hand back the value, or nothing at the end.
    auto doneIfEmpty = [&](LValue value) { return boxBoolean(m_out.isZero64(value)); };

    m_out.branch(isCell(next), unsure(nextIsCell), unsure(nextIsNotCell));

    m_out.appendTo(nextIsCell, nextIsNotCell);
    m_out.branch(isSentinelCell(next), unsure(markedCase), unsure(genericCase));

    // Then, and only then, iterator may be a sentinel instead of an object: iterable is an array, and next the index to visit.
    m_out.appendTo(nextIsNotCell, markedCase);
    LValue iteratorIsSentinel = isCellAnd(iteratorNode, iterator, [&](LValue cell) { return isSentinelCell(cell); });
    m_out.branch(iteratorIsSentinel, unsure(indexCase), unsure(genericCase));

    m_out.appendTo(markedCase, indexCase);
    {
        LValue value = vmCall(node, Int64, Entry::operationAOTIteratorNextTryFast, m_globalObject, iterator);
        finish(doneIfEmpty(value), value, next);
        m_out.jump(continuation);
    }

    // An element that is there, in storage that holds JSValues. The end, holes and everything else are the runtime's.
    m_out.appendTo(indexCase, indexIsInt32);
    m_out.branch(isInt32(next), usually(indexIsInt32), rarely(indexSlow));

    m_out.appendTo(indexIsInt32, iterableIsCell);
    LValue index = unboxInt32(next);
    m_out.branch(isCell(iterable), usually(iterableIsCell), rarely(indexSlow));

    m_out.appendTo(iterableIsCell, iterableIsArray);
    m_out.branch(isCellOfType(iterable, ArrayType), usually(iterableIsArray), rarely(indexSlow));

    m_out.appendTo(iterableIsArray, rightShape);
    LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(iterable, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
    m_out.branch(m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape))), usually(rightShape), rarely(indexSlow));

    // As unsigned: the index of a finished iteration, -1, is above any length.
    m_out.appendTo(rightShape, inBounds);
    LValue butterfly = m_out.loadPtr(iterable, m_heaps.JSObject_butterfly);
    LValue isInBounds = m_out.bitAnd(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), m_out.notEqual(index, m_out.constInt32(std::numeric_limits<int32_t>::max())));
    LBasicBlock isAtTheEnd = m_out.newBlock();
    m_out.branch(isInBounds, usually(inBounds), isWithout(WithoutIteratorEnd) ? rarely(indexSlow) : unsure(isAtTheEnd));

    // That is all: JSArrayIterator::nextValueWithIndexInFrame().
    m_out.appendTo(isAtTheEnd);
    finish(m_out.constInt64(JSValue::ValueTrue), m_out.constInt64(JSValue::encode(jsUndefined())), m_out.constInt64(JSValue::encode(jsNumber(JSArrayIterator::doneIndex))));
    m_out.jump(continuation);

    m_out.appendTo(inBounds, indexFast);
    LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
    m_out.branch(m_out.notZero64(element), usually(indexFast), rarely(indexSlow));

    m_out.appendTo(indexFast, indexSlow);
    finish(m_out.constInt64(JSValue::ValueFalse), element, boxInt32(m_out.add(index, m_out.int32One)));
    m_out.jump(continuation);

    m_out.appendTo(indexSlow, genericCase);
    {
        m_out.store64(next, scratchWord(0));
        LValue value = vmCall(node, Int64, Entry::operationAOTIteratorNextWithIndex, m_globalObject, iterable, scratchAddress());
        finish(doneIfEmpty(value), value, m_out.load64(scratchWord(0)));
        m_out.jump(continuation);
    }

    m_out.appendTo(genericCase, notDone);
    LValue result = emitCall(node, next, Arguments { iterator });
    checkIsObjectOrThrowIteratorResultIsNotObject(node, result);
    LValue done = getByIdCached(node, result, TAnyObject, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::Done));
    LValue isDone;
    {
        // As toBoolean(), for a value that is not a node.
        LBasicBlock notBoolean = m_out.newBlock();
        LBasicBlock haveDone = m_out.newBlock();
        ValueFromBlock booleanResult = m_out.anchor(unboxBoolean(done));
        m_out.branch(isBoolean(done), usually(haveDone), rarely(notBoolean));
        m_out.appendTo(notBoolean, haveDone);
        ValueFromBlock otherResult = m_out.anchor(m_out.notZero64(plainCall(Int64, Entry::operationAOTToBoolean, m_globalObject, done)));
        m_out.jump(haveDone);
        m_out.appendTo(haveDone);
        isDone = m_out.phi(Int32, booleanResult, otherResult);
    }
    finish(done, m_out.constInt64(JSValue::encode(jsUndefined())), next);
    m_out.branch(isDone, unsure(continuation), unsure(notDone));

    m_out.appendTo(notDone, continuation);
    finish(done, getByIdCached(node, result, TAnyObject, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::Value)), next);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setProj(node, bytecode.m_done, m_out.phi(Int64, doneResults));
    setProj(node, bytecode.m_value, m_out.phi(Int64, valueResults));
    setProj(node, bytecode.m_next, m_out.phi(Int64, nextResults));
}

// dst = next.call(iterator [, value]), or, if next is the marker, the value is queued on the producer.
void Lowering::lowerAsyncIteratorNext(Node* node)
{
    auto bytecode = node->as<OpAsyncIteratorNext>();
    Node* nextNode = node->use(bytecode.m_next);
    LValue next = lowJSValue(nextNode);
    LValue iterator = lowJSValue(node->use(bytecode.m_iterator));
    LValue resumeValue = bytecode.m_hasValue ? lowJSValue(node->use(resumeValueOperandFor(bytecode))) : nullptr;

    LBasicBlock markedCase = m_out.newBlock();
    LBasicBlock genericCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    LValue isMarked = isCellAnd(nextNode, next, [&](LValue cell) { return isSentinelCell(cell); });
    m_out.branch(isMarked, unsure(markedCase), unsure(genericCase));

    m_out.appendTo(markedCase, genericCase);
    ValueFromBlock markedResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTAsyncIteratorNextWithDriver, m_globalObject, iterator, lowCell(node->use(bytecode.m_driver)),
        resumeValue ? resumeValue : m_out.constInt64(JSValue::encode(JSValue()))));
    m_out.jump(continuation);

    m_out.appendTo(genericCase, continuation);
    Arguments arguments { iterator };
    if (resumeValue)
        arguments.append(resumeValue);
    ValueFromBlock genericResult = m_out.anchor(emitCall(node, next, arguments));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, markedResult, genericResult));
}

// The first half of op_iterator_close_check (see the parser): the iterator to close. If there is no iterator object, and closing
// one could be noticed after all (somebody gave array iterators a return method), this is where it gets made. If it could not,
// the marker stays, which is how the second half knows that there is nothing to do.
void Lowering::lowerIteratorCloseCheck(Node* node)
{
    auto bytecode = node->as<OpIteratorCloseCheck>();
    Node* iteratorNode = node->use(bytecode.m_iterator);
    LValue iterator = lowJSValue(iteratorNode);
    if (!mayBe(iteratorNode->type, TCellOther)) {
        setJSValue(node, iterator);
        return;
    }
    if constexpr (usesStubs) {
        setJSValue(node, callStub(Stub::IteratorCloseCheck, Int64,
            { { iterator, GPRInfo::argumentGPR0 }, { lowJSValue(node->use(bytecode.m_iterable)), GPRInfo::argumentGPR1 }, { lowJSValue(node->use(bytecode.m_next)), GPRInfo::argumentGPR2 } }, { }));
        return;
    }

    LBasicBlock markedCase = m_out.newBlock();
    LBasicBlock materialize = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    LValue isMarked = isCellAnd(iteratorNode, iterator, [&](LValue cell) { return isSentinelCell(cell); });
    results.append(m_out.anchor(iterator));
    m_out.branch(isMarked, unsure(markedCase), unsure(continuation));

    m_out.appendTo(markedCase, materialize);
    LValue isStillValid = inlineWatchpointSetIsStillValid(m_out.add(m_globalObject, m_out.constIntPtr(JSGlobalObject::offsetOfArrayIteratorProtocolWatchpointSet())));
    results.append(m_out.anchor(iterator));
    m_out.branch(isStillValid, usually(continuation), rarely(materialize));

    m_out.appendTo(materialize, continuation);
    results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTMaterializeArrayIterator, m_globalObject, lowJSValue(node->use(bytecode.m_iterable)), lowJSValue(node->use(bytecode.m_next)))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

// The second half: whether to jump over the code that closes the iterator.
LValue Lowering::iteratorCloseCheckCondition(Node* node)
{
    Node* iteratorNode = node->use(node->as<OpIteratorCloseCheck>().m_iterator);
    if (Graph::iteratedFactOf(node))
        return m_out.booleanTrue;
    if (!mayBe(iteratorNode->type, TCellOther))
        return m_out.booleanFalse;
    return isCellAnd(iteratorNode, lowJSValue(iteratorNode), [&](LValue cell) { return isSentinelCell(cell); });
}

bool Lowering::tryLowerIteration(Node* node)
{
    auto low = [&](VirtualRegister reg) { return lowJSValue(node->use(reg)); };

    switch (node->opcode) {
    case op_iterator_open:
        lowerIteratorOpen(node, false);
        return true;
    case op_async_iterator_open:
        lowerIteratorOpen(node, true);
        return true;
    case op_iterator_next:
        lowerIteratorNext(node);
        return true;
    case op_async_iterator_next:
        lowerAsyncIteratorNext(node);
        return true;
    case op_iterator_close_check:
        lowerIteratorCloseCheck(node);
        return true;

    case op_get_property_enumerator: {
        // What the Structure of an object with no elements remembers having made for the last of its kind, if that goes for whatever the object inherits from as well.
        Node* baseNode = node->use(node->as<OpGetPropertyEnumerator>().m_base);
        LValue base = lowJSValue(baseNode);
        LBasicBlock generic = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (!isSubtype(baseNode->type, TCell))
            orElse(isCell(base), generic);
        static_assert(NonArray <= ArrayWithUndecided && ArrayClass <= ArrayWithUndecided);
        orElse(m_out.belowOrEqual(m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingTypeMask)), m_out.constInt32(ArrayWithUndecided)), generic);
        LValue previousOrRareData = m_out.loadPtr(structureOf(base), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), generic);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), generic);
        LValue cachedAndFlag = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
        orElse(m_out.notNull(cachedAndFlag), generic);
        orElse(m_out.testIsZeroPtr(cachedAndFlag, m_out.constIntPtr(StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag)), generic);
        ValueFromBlock remembered = m_out.anchor(cachedAndFlag);
        m_out.jump(continuation);
        m_out.appendTo(generic);
        ValueFromBlock made = m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTGetPropertyEnumerator, m_globalObject, base));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(pointerType(), remembered, made));
        return true;
    }
    case op_enumerator_next: {
        auto bytecode = node->as<OpEnumeratorNext>();
        Node* baseNode = node->use(bytecode.m_base);
        LValue base = lowJSValue(baseNode);
        LValue enumerator = low(bytecode.m_enumerator);
        LValue mode = low(bytecode.m_mode);
        LValue index = low(bytecode.m_index);
        // The object is still of the Structure that the names were taken from, and has no elements: the next name is the next in the list. JSPropertyNameEnumerator::computeNext().
        LBasicBlock generic = m_out.newBlock();
        LBasicBlock hasOne = m_out.newBlock();
        LBasicBlock isAtTheEnd = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (!isSubtype(baseNode->type, TCell))
            orElse(isCell(base), generic);
        orElse(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureID)), generic);
        orElse(m_out.isZero32(m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength)), generic);
        static_assert(!JSPropertyNameEnumerator::InitMode);
        orElse(m_out.testIsZero32(unboxInt32(mode), m_out.constInt32(~JSPropertyNameEnumerator::OwnStructureMode)), generic);
        LValue next = m_out.select(m_out.isZero32(unboxInt32(mode)), m_out.int32Zero, m_out.add(unboxInt32(index), m_out.int32One));
        LValue end = m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_endStructurePropertyIndex);
        m_out.branch(m_out.below(next, end), usually(hasOne), unsure(isAtTheEnd));

        m_out.appendTo(hasOne);
        Vector<ValueFromBlock, 3> names;
        Vector<ValueFromBlock, 3> modes;
        Vector<ValueFromBlock, 3> indices;
        LValue ownStructureMode = m_out.constInt64(JSValue::encode(jsNumber(static_cast<int32_t>(JSPropertyNameEnumerator::OwnStructureMode))));
        names.append(m_out.anchor(m_out.loadPtr(m_out.baseIndex(m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVectorContents, m_out.loadPtr(enumerator, m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVector), m_out.zeroExtPtr(next)))));
        modes.append(m_out.anchor(ownStructureMode));
        indices.append(m_out.anchor(boxInt32(next)));
        m_out.jump(continuation);

        m_out.appendTo(isAtTheEnd);
        orElse(m_out.equal(m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex), end), generic);
        names.append(m_out.anchor(fixedPointer(Instance::offsetOfSentinelString())));
        modes.append(m_out.anchor(ownStructureMode));
        indices.append(m_out.anchor(boxInt32(next)));
        m_out.jump(continuation);

        m_out.appendTo(generic);
        m_out.store64(mode, scratchWord(0));
        m_out.store64(index, scratchWord(1));
        names.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTEnumeratorNext, m_globalObject, base, enumerator, scratchAddress())));
        modes.append(m_out.anchor(m_out.load64(scratchWord(0))));
        indices.append(m_out.anchor(m_out.load64(scratchWord(1))));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setProj(node, bytecode.m_propertyName, m_out.phi(pointerType(), names));
        setProj(node, bytecode.m_mode, m_out.phi(Int64, modes));
        setProj(node, bytecode.m_index, m_out.phi(Int64, indices));
        return true;
    }
    case op_enumerator_get_by_val: {
        auto bytecode = node->as<OpEnumeratorGetByVal>();
        Node* baseNode = node->use(bytecode.m_base);
        LValue base = lowJSValue(baseNode);
        LValue enumerator = low(bytecode.m_enumerator);
        // The name is one of the object's own, and the object is as it was: how far down the list the name is says where the property is.
        LBasicBlock generic = m_out.newBlock();
        LBasicBlock isInObject = m_out.newBlock();
        LBasicBlock isOutside = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (!isSubtype(baseNode->type, TCell))
            orElse(isCell(base), generic);
        orElse(m_out.equal(unboxInt32(low(bytecode.m_mode)), m_out.constInt32(JSPropertyNameEnumerator::OwnStructureMode)), generic);
        orElse(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureID)), generic);
        LValue index = unboxInt32(low(bytecode.m_index));
        LValue inlineCapacity = m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_cachedInlineCapacity);
        m_out.branch(m_out.below(index, inlineCapacity), unsure(isInObject), unsure(isOutside));
        m_out.appendTo(isInObject);
        ValueFromBlock inObject = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(base, m_out.add(m_out.shl(m_out.zeroExtPtr(index), m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage()))))));
        m_out.jump(continuation);
        m_out.appendTo(isOutside);
        LValue howFarOut = m_out.zeroExtPtr(m_out.sub(index, inlineCapacity));
        ValueFromBlock outside = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(m_out.sub(m_out.loadPtr(base, m_heaps.JSObject_butterfly), m_out.shl(howFarOut, m_out.constInt32(3))), m_out.constIntPtr(static_cast<intptr_t>(offsetInButterfly(firstOutOfLineOffset)) * static_cast<intptr_t>(sizeof(EncodedJSValue)))))));
        m_out.jump(continuation);
        m_out.appendTo(generic);
        ValueFromBlock found = m_out.anchor(vmCall(node, Int64, Entry::operationAOTEnumeratorGetByVal, m_globalObject, low(bytecode.m_base), low(bytecode.m_propertyName), low(bytecode.m_index), low(bytecode.m_mode), low(bytecode.m_enumerator)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, inObject, outside, found));
        return true;
    }
    case op_enumerator_in_by_val: {
        auto bytecode = node->as<OpEnumeratorInByVal>();
        setBoolean(node, m_out.notZero64(vmCall(node, Int64, Entry::operationAOTEnumeratorInByVal, m_globalObject, low(bytecode.m_base), low(bytecode.m_propertyName), low(bytecode.m_index), low(bytecode.m_mode), low(bytecode.m_enumerator))));
        return true;
    }
    case op_enumerator_put_by_val: {
        auto bytecode = node->as<OpEnumeratorPutByVal>();
        vmCall(node, Void, Entry::operationAOTEnumeratorPutByVal, m_globalObject, low(bytecode.m_base), low(bytecode.m_propertyName), low(bytecode.m_value), low(bytecode.m_index), low(bytecode.m_mode), low(bytecode.m_enumerator), m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
        return true;
    }
    case op_enumerator_has_own_property: {
        auto bytecode = node->as<OpEnumeratorHasOwnProperty>();
        setBoolean(node, m_out.notZero64(vmCall(node, Int64, Entry::operationAOTEnumeratorHasOwnProperty, m_globalObject, low(bytecode.m_base), low(bytecode.m_propertyName), low(bytecode.m_index), low(bytecode.m_mode), low(bytecode.m_enumerator))));
        return true;
    }
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
