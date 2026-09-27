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
    LValue symbolIterator = lowJSValue(node->use(symbolIteratorRegister));

    LBasicBlock fastCase = m_out.newBlock();
    LBasicBlock genericCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

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
    setProj(node, iteratorRegister, m_out.phi(Int64, fastIteratorResult, genericIteratorResult));
    setProj(node, nextRegister, m_out.phi(Int64, fastNextResult, genericNextResult));
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

    if constexpr (usesStubs) {
        // Every loop over an array would have a copy of how that is done, and every loop has to be ready for anything else.
        PatchpointValue* shortcut = callStub(Stub::IteratorNext, m_proc.addTuple({ Int64, Int64, Int64, Int32 }),
            { { next, GPRInfo::argumentGPR0 }, { iterator, GPRInfo::argumentGPR1 }, { iterable, GPRInfo::argumentGPR2 } },
            { { GPRInfo::regT10, CallSiteIndex(node->bytecodeIndex).bits() } });
        shortcut->resultConstraints = { ValueRep::reg(GPRInfo::argumentGPR0), ValueRep::reg(GPRInfo::argumentGPR1), ValueRep::reg(GPRInfo::argumentGPR2), ValueRep::reg(GPRInfo::regT9) };

        LBasicBlock genericCase = m_out.newBlock();
        LBasicBlock notDone = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 3> doneResults;
        Vector<ValueFromBlock, 3> valueResults;
        Vector<ValueFromBlock, 3> nextResults;
        auto finish = [&](LValue done, LValue value, LValue newNext) {
            doneResults.append(m_out.anchor(done));
            valueResults.append(m_out.anchor(value));
            nextResults.append(m_out.anchor(newNext));
        };
        auto getWellKnown = [&](LValue base, WellKnownIdentifier identifier) -> LValue {
            return callStub(Stub::GetByIdWellKnown, Int64, { { base, GPRInfo::argumentGPR0 }, { slotAddress(allocateSite(node, static_cast<unsigned>(identifier))), GPRInfo::argumentGPR1 } }, { });
        };

        finish(m_out.extract(shortcut, 0), m_out.extract(shortcut, 1), m_out.extract(shortcut, 2));
        m_out.branch(m_out.isZero32(m_out.extract(shortcut, 3)), usually(continuation), unsure(genericCase));

        m_out.appendTo(genericCase, notDone);
        LValue result = emitCall(node, next, Arguments { iterator });
        checkIsObjectOrThrowIteratorResultIsNotObject(node, result);
        LValue done = getWellKnown(result, WellKnownIdentifier::Done);
        finish(done, m_out.constInt64(JSValue::encode(jsUndefined())), next);
        LValue isDone = callStub(Stub::ToBoolean, Int32, { { done, GPRInfo::argumentGPR0 } }, { }, StubClobbers::Temporaries);
        m_out.branch(isDone, unsure(continuation), unsure(notDone));

        m_out.appendTo(notDone, continuation);
        finish(done, getWellKnown(result, WellKnownIdentifier::Value), next);
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setProj(node, bytecode.m_done, m_out.phi(Int64, doneResults));
        setProj(node, bytecode.m_value, m_out.phi(Int64, valueResults));
        setProj(node, bytecode.m_next, m_out.phi(Int64, nextResults));
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
    m_out.branch(isInBounds, usually(inBounds), rarely(indexSlow));

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

    case op_get_property_enumerator:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTGetPropertyEnumerator, m_globalObject, low(node->as<OpGetPropertyEnumerator>().m_base)));
        return true;
    case op_enumerator_next: {
        auto bytecode = node->as<OpEnumeratorNext>();
        m_out.store64(low(bytecode.m_mode), scratchWord(0));
        m_out.store64(low(bytecode.m_index), scratchWord(1));
        LValue name = vmCall(node, pointerType(), Entry::operationAOTEnumeratorNext, m_globalObject, low(bytecode.m_base), low(bytecode.m_enumerator), scratchAddress());
        setProj(node, bytecode.m_propertyName, name);
        setProj(node, bytecode.m_mode, m_out.load64(scratchWord(0)));
        setProj(node, bytecode.m_index, m_out.load64(scratchWord(1)));
        return true;
    }
    case op_enumerator_get_by_val: {
        auto bytecode = node->as<OpEnumeratorGetByVal>();
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTEnumeratorGetByVal, m_globalObject, low(bytecode.m_base), low(bytecode.m_propertyName), low(bytecode.m_index), low(bytecode.m_mode), low(bytecode.m_enumerator)));
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
