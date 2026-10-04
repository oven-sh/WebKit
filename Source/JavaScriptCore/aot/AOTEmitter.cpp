/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEmitter.h"

#if ENABLE(AOT)

#include "B3ArgumentRegValue.h"
#include "B3Generate.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "JSLexicalEnvironment.h"
#include "JSPromise.h"
#include "StructureRareData.h"
#include "TypeInfoBlob.h"

namespace JSC { namespace AOT {

using namespace B3;

void Emitter::findPinnedRegisters()
{
    m_instance = registerOnEntry(instanceGPR);
    m_numberTag = registerOnEntry(GPRInfo::numberTagRegister);
    m_notCellMask = registerOnEntry(GPRInfo::notCellMaskRegister);
    m_vm = m_out.loadPtr(m_instance, m_heaps.AOTInstance_vm);
    m_globalObject = m_out.loadPtr(m_instance, m_heaps.AOTInstance_globalObject);
    m_table = m_out.loadPtr(m_instance, m_heaps.AOTInstance_runtimeTable);
}

void Emitter::orElse(LValue condition, LBasicBlock otherwise)
{
    LBasicBlock next = m_out.newBlock();
    m_out.branch(condition, usually(next), rarely(otherwise));
    m_out.appendTo(next);
}

LValue Emitter::fixedPointer(ptrdiff_t offset)
{
    return m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, m_instance, offset));
}

LValue Emitter::fixed32(ptrdiff_t offset)
{
    return m_out.load32(m_out.address(m_heaps.AOTInstance_fixedFields, m_instance, offset));
}

LValue Emitter::changing32(ptrdiff_t offset)
{
    return m_out.load32(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, offset));
}

LValue Emitter::structureWithID(LValue structureID)
{
    return m_out.bitOr(m_out.zeroExtPtr(structureID), fixedPointer(Instance::offsetOfStructureIDBase()));
}

LValue Emitter::familyOfStructureWithID(LValue structureID)
{
    return m_out.load16ZeroExt32(TypedPointer(m_heaps.root, m_out.add(fixedPointer(Instance::offsetOfFamilyBase()), m_out.zeroExtPtr(structureID))));
}

LValue Emitter::departedFamily(unsigned family)
{
    RELEASE_ASSERT(Instance::hasByteForFamily(family));
    return m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfDepartedFamily(family)));
}

LValue Emitter::fieldIDInSlot(LValue structureID, unsigned slot)
{
    RELEASE_ASSERT(slot < Structure::numberOfSlotsWithFieldIDs);
    return m_out.load16ZeroExt32(TypedPointer(m_heaps.root, m_out.add(fixedPointer(Instance::offsetOfFieldIDInSlotBase(slot)), m_out.zeroExtPtr(structureID))));
}

template<typename Functor>
void Emitter::forEachUpTo(LValue count, const Functor& body)
{
    LBasicBlock head = m_out.newBlock();
    LBasicBlock inside = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    LValue limit = m_out.zeroExtPtr(count);
    ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
    m_out.jump(head);
    m_out.appendTo(head);
    LValue index = m_out.phi(pointerType(), start);
    m_out.branch(m_out.below(index, limit), unsure(inside), unsure(done));
    m_out.appendTo(inside);
    body(index);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
    m_out.jump(head);
    m_out.appendTo(done);
}

LValue Emitter::allocateHeapCell(LValue allocator, LBasicBlock slowPath)
{
    orElse(m_out.notNull(allocator), slowPath);
    PatchpointValue* allocation = m_out.patchpoint(pointerType());
    allocation->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    allocation->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList outOfSpace;
        jit.emitAllocateWithNonNullAllocator(params[0].gpr(), JITAllocator::variable(), params[1].gpr(), params.gpScratch(0), outOfSpace, CCallHelpers::SlowAllocationResult::ClearToNull);
        outOfSpace.link(&jit);
    });
    orElse(m_out.notNull(allocation), slowPath);
    return allocation;
}

LValue Emitter::allocatorForSize(LValue subspace, LValue size, LBasicBlock slowPath)
{
    static_assert(!(MarkedSpace::sizeStep & (MarkedSpace::sizeStep - 1)), "MarkedSpace::sizeStep must be a power of two.");
    unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    LValue sizeClassIndex = m_out.lShr(m_out.add(size, m_out.constIntPtr(MarkedSpace::sizeStep - 1)), m_out.constInt32(stepShift));
    orElse(m_out.belowOrEqual(sizeClassIndex, m_out.constIntPtr(MarkedSpace::largeCutoff >> stepShift)), slowPath);
    return m_out.loadPtr(m_out.baseIndex(m_heaps.CompleteSubspace_allocatorForSizeStep, subspace, sizeClassIndex));
}

LValue Emitter::allocatorForSize(LValue subspace, size_t size)
{
    RELEASE_ASSERT(size <= MarkedSpace::largeCutoff);
    return m_out.loadPtr(subspace, m_heaps.CompleteSubspace_allocatorForSizeStep[MarkedSpace::sizeClassToIndex(size)]);
}

void Emitter::storeHeader(LValue cell, LValue structureID, uint32_t typeInfoBlob)
{
    m_out.store32(structureID, cell, m_heaps.JSCell_structureID);
    m_out.store32(m_out.constInt32(typeInfoBlob), cell, m_heaps.JSCell_usefulBytes);
}

void Emitter::storeStructure(LValue cell, LValue structure)
{
    m_out.store32(m_out.castToInt32(structure), cell, m_heaps.JSCell_structureID);
    m_out.store32(m_out.load32(structure, m_heaps.Structure_indexingModeIncludingHistory), cell, m_heaps.JSCell_usefulBytes);
}

void Emitter::splatWords(LValue base, LValue begin, LValue end, LValue value, const AbstractHeap& heap)
{
    LBasicBlock initLoop = m_out.newBlock();
    LBasicBlock initDone = m_out.newBlock();
    ValueFromBlock originalIndex = m_out.anchor(end);
    ValueFromBlock originalPointer = m_out.anchor(m_out.add(base, m_out.shl(m_out.signExt32ToPtr(begin), m_out.constInt32(3))));
    m_out.branch(m_out.notEqual(end, begin), unsure(initLoop), unsure(initDone));

    m_out.appendTo(initLoop, initDone);
    LValue index = m_out.phi(Int32, originalIndex);
    LValue pointer = m_out.phi(pointerType(), originalPointer);
    m_out.store64(value, TypedPointer(heap, pointer));
    LValue nextIndex = m_out.sub(index, m_out.int32One);
    m_out.addIncomingToPhi(index, m_out.anchor(nextIndex));
    m_out.addIncomingToPhi(pointer, m_out.anchor(m_out.add(pointer, m_out.intPtrEight)));
    m_out.branch(m_out.notEqual(nextIndex, begin), unsure(initLoop), unsure(initDone));

    m_out.appendTo(initDone);
}

void Emitter::mutatorFence()
{
    LBasicBlock fence = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(m_vm, m_heaps.VM_heap_mutatorShouldBeFenced), rarely(fence), usually(done));
    m_out.appendTo(fence, done);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(done);
    m_out.appendTo(done);
}

uint32_t Emitter::arrayTypeInfoBlob(IndexingType indexingType)
{
    return TypeInfoBlob(indexingType, TypeInfo(ArrayType, JSArray::StructureFlags)).blob();
}

Emitter::ArrayValues Emitter::allocateJSArray(LValue publicLength, LValue vectorLength, LValue structureID, uint32_t typeInfoBlob, LBasicBlock slowPath)
{
    orElse(m_out.notZero32(structureID), slowPath);
    LValue size = m_out.add(m_out.shl(m_out.zeroExtPtr(vectorLength), m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue startOfStorage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size, slowPath), slowPath);
    LValue butterfly = m_out.add(startOfStorage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(publicLength, butterfly, m_heaps.Butterfly_publicLength);
    m_out.store32(vectorLength, butterfly, m_heaps.Butterfly_vectorLength);
    splatWords(butterfly, publicLength, vectorLength, m_out.int64Zero, m_heaps.indexedContiguousProperties.atAnyIndex());
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), slowPath);
    storeHeader(array, structureID, typeInfoBlob);
    m_out.storePtr(butterfly, array, m_heaps.JSObject_butterfly);
    return { array, butterfly };
}

static LValue vectorLengthFor(FTL::Output& out, LValue count)
{
    LValue least = out.select(out.isZero32(count), out.constInt32(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY), out.constInt32(BASE_CONTIGUOUS_VECTOR_LEN));
    return out.bitOr(out.select(out.above(count, least), count, least), out.int32One);
}

LValue Emitter::newArrayFromValues(LValue values, LValue count, bool areInt32, LBasicBlock giveUp)
{
    LValue structureID = changing32(areInt32 ? Instance::offsetOfNewArrayWithInt32StructureID() : Instance::offsetOfNewArrayWithContiguousStructureID());
    auto [array, butterfly] = allocateJSArray(count, vectorLengthFor(m_out, count), structureID, arrayTypeInfoBlob(areInt32 ? ArrayWithInt32 : ArrayWithContiguous), giveUp);
    LValue elements = butterfly;
    forEachUpTo(count, [&](LValue index) {
        m_out.store64(m_out.load64(m_out.baseIndex(m_heaps.variables, values, index)), m_out.baseIndex(m_heaps.indexedContiguousProperties, elements, index));
    });
    mutatorFence();
    return array;
}

LValue Emitter::newArrayFromButterfly(LValue immutableButterfly, LBasicBlock giveUp)
{
    LValue mode = m_out.load8ZeroExt32(immutableButterfly, m_heaps.JSCell_indexingTypeAndMisc);
    LValue which = m_out.lShr(m_out.sub(m_out.bitAnd(mode, m_out.constInt32(IndexingShapeMask)), m_out.constInt32(Int32Shape)), m_out.constInt32(IndexingShapeShift));
    orElse(m_out.below(which, m_out.constInt32(3)), giveUp);
    LValue structureID = m_out.load32(TypedPointer(m_heaps.AOTInstance_mutableFields,
        m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(which), m_out.constInt32(2)), m_out.constIntPtr(Instance::offsetOfNewCopyOnWriteArrayStructureIDs())))));
    orElse(m_out.notZero32(structureID), giveUp);
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), giveUp);
    m_out.store32(structureID, array, m_heaps.JSCell_structureID);
    static_assert(!TypeInfoBlob::indexingModeIncludingHistoryOffset());
    m_out.store32(m_out.bitOr(m_out.bitAnd(mode, m_out.constInt32(AllArrayTypesAndHistory)), m_out.constInt32(arrayTypeInfoBlob(NonArray))), array, m_heaps.JSCell_usefulBytes);
    m_out.storePtr(m_out.add(immutableButterfly, m_out.constIntPtr(JSCellButterfly::offsetOfData())), array, m_heaps.JSObject_butterfly);
    mutatorFence();
    return array;
}

LValue Emitter::newActivation(LValue scope, LValue symbolTable, LValue initialValue, LValue count, LBasicBlock giveUp)
{
    LValue structureID = fixed32(Instance::offsetOfActivationStructureID());
    orElse(m_out.notZero32(structureID), giveUp);
    LValue size = m_out.add(m_out.shl(m_out.zeroExtPtr(count), m_out.constInt32(3)), m_out.constIntPtr(JSLexicalEnvironment::offsetOfVariables()));
    LValue result = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfActivationSpace()), size, giveUp), giveUp);
    storeHeader(result, structureID, TypeInfoBlob(NonArray, TypeInfo(LexicalEnvironmentType, JSLexicalEnvironment::StructureFlags)).blob());
    m_out.storePtr(m_out.intPtrZero, result, m_heaps.JSObject_butterfly);
    m_out.storePtr(scope, result, m_heaps.JSScope_next);
    m_out.storePtr(symbolTable, result, m_heaps.JSSymbolTableObject_symbolTable);
    forEachUpTo(count, [&](LValue index) {
        m_out.store64(initialValue, m_out.baseIndex(m_heaps.JSLexicalEnvironment_variables, result, index));
    });
    mutatorFence();
    return result;
}

LValue Emitter::allocateObject(Instance::InlineAllocation kind, LBasicBlock giveUp)
{
    LValue result = allocateHeapCell(m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind))), giveUp);
    m_out.store64(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind))), result, m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, result, m_heaps.JSObject_butterfly);
    return result;
}

LValue Emitter::newPromise(LBasicBlock giveUp)
{
    LValue result = allocateObject(Instance::InlineAllocation::Promise, giveUp);
    m_out.store64(m_out.int64Zero, result, m_heaps.JSPromise_packed);
    m_out.store64(m_out.constInt64(JSValue::encode(JSValue())), result, m_heaps.JSPromise_slot);
    mutatorFence();
    return result;
}

LValue Emitter::newResolvedPromise(LValue value, LBasicBlock giveUp)
{
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock isNotObject = m_out.newBlock();
    m_out.branch(isCell(value), unsure(isCellCase), unsure(isNotObject));
    m_out.appendTo(isCellCase);
    m_out.branch(isObjectCell(value), unsure(giveUp), unsure(isNotObject));
    m_out.appendTo(isNotObject);
    LValue result = allocateObject(Instance::InlineAllocation::Promise, giveUp);
    static_assert(CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer == 48);
    m_out.store64(m_out.constInt64((static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << 48), result, m_heaps.JSPromise_packed);
    m_out.store64(value, result, m_heaps.JSPromise_slot);
    mutatorFence();
    return result;
}

LValue Emitter::newInternalFieldObject(Instance::InlineAllocation kind, std::span<const JSValue> initialValues, LBasicBlock giveUp)
{
    LValue result = allocateObject(kind, giveUp);
    for (unsigned i = 0; i < initialValues.size(); ++i)
        m_out.store64(m_out.constInt64(JSValue::encode(initialValues[i])), result, m_heaps.JSInternalFieldObjectImpl_internalFields[i]);
    mutatorFence();
    return result;
}

LValue Emitter::newMapOrSet(Instance::InlineAllocation kind, LBasicBlock giveUp)
{
    LValue result = allocateObject(kind, giveUp);
    m_out.storePtr(m_out.intPtrZero, result, kind == Instance::InlineAllocation::Map ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    mutatorFence();
    return result;
}

LValue Emitter::isOriginalArray(LValue cell)
{
    LValue kind = m_out.bitAnd(m_out.lShr(m_out.load8ZeroExt32(cell, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(Instance::arrayKindShift)), m_out.constInt32(Instance::numberOfArrayKinds - 1));
    LValue expected = m_out.load32(TypedPointer(m_heaps.AOTInstance_fixedFields,
        m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(kind), m_out.constInt32(2)), m_out.constIntPtr(Instance::offsetOfOriginalArrayStructureIDs())))));
    return m_out.equal(m_out.load32(cell, m_heaps.JSCell_structureID), expected);
}

LValue Emitter::newArrayWithSpread(LValue values, LValue count, LValue spreadMask, LBasicBlock giveUp)
{
    orElse(m_out.notZero32(changing32(Instance::offsetOfArraysLackInheritedElements())), giveUp);
    auto isSpreadAt = [&](LValue index) {
        return m_out.testNonZero32(m_out.lShr(spreadMask, m_out.castToInt32(index)), m_out.int32One);
    };
    LValue limit = m_out.zeroExtPtr(count);

    LBasicBlock measure = m_out.newBlock();
    LBasicBlock measureLoop = m_out.newBlock();
    LBasicBlock measureArray = m_out.newBlock();
    LBasicBlock measured = m_out.newBlock();
    LBasicBlock measureNext = m_out.newBlock();
    ValueFromBlock firstIndex = m_out.anchor(m_out.intPtrZero);
    ValueFromBlock stillEmpty = m_out.anchor(m_out.intPtrZero);
    m_out.jump(measure);

    m_out.appendTo(measure);
    LValue index = m_out.phi(pointerType(), firstIndex);
    LValue lengthSoFar = m_out.phi(pointerType(), stillEmpty);
    m_out.branch(m_out.below(index, limit), unsure(measureLoop), unsure(measured));

    m_out.appendTo(measureLoop);
    LBasicBlock measureValue = m_out.newBlock();
    LBasicBlock measureCell = m_out.newBlock();
    LValue source = m_out.load64(m_out.baseIndex(m_heaps.variables, values, index));
    m_out.branch(isSpreadAt(index), unsure(measureArray), unsure(measureValue));

    m_out.appendTo(measureValue);
    ValueFromBlock one = m_out.anchor(m_out.intPtrOne);
    m_out.branch(isCell(source), unsure(measureCell), unsure(measureNext));
    m_out.appendTo(measureCell);
    ValueFromBlock oneCell = m_out.anchor(m_out.intPtrOne);
    m_out.branch(isCellOfType(source, JSCellButterflyType), rarely(giveUp), usually(measureNext));

    m_out.appendTo(measureArray);
    orElse(isCell(source), giveUp);
    orElse(isOriginalArray(source), giveUp);
    LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(source, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
    orElse(m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape))), giveUp);
    ValueFromBlock many = m_out.anchor(m_out.zeroExtPtr(m_out.load32(m_out.loadPtr(source, m_heaps.JSObject_butterfly), m_heaps.Butterfly_publicLength)));
    m_out.jump(measureNext);

    m_out.appendTo(measureNext);
    m_out.addIncomingToPhi(lengthSoFar, m_out.anchor(m_out.add(lengthSoFar, m_out.phi(pointerType(), one, oneCell, many))));
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
    m_out.jump(measure);

    m_out.appendTo(measured);
    orElse(m_out.below(lengthSoFar, m_out.constIntPtr(MarkedSpace::largeCutoff / sizeof(EncodedJSValue))), giveUp);
    LValue length = m_out.castToInt32(lengthSoFar);
    auto [array, butterfly] = allocateJSArray(length, vectorLengthFor(m_out, length), changing32(Instance::offsetOfNewArrayWithContiguousStructureID()), arrayTypeInfoBlob(ArrayWithContiguous), giveUp);
    LValue elements = butterfly;

    LBasicBlock fill = m_out.newBlock();
    LBasicBlock fillLoop = m_out.newBlock();
    LBasicBlock fillFromValue = m_out.newBlock();
    LBasicBlock fillFromArray = m_out.newBlock();
    LBasicBlock copy = m_out.newBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock fillNext = m_out.newBlock();
    LBasicBlock filled = m_out.newBlock();
    ValueFromBlock fillStart = m_out.anchor(m_out.intPtrZero);
    ValueFromBlock firstPlace = m_out.anchor(m_out.intPtrZero);
    m_out.jump(fill);

    m_out.appendTo(fill);
    LValue which = m_out.phi(pointerType(), fillStart);
    LValue place = m_out.phi(pointerType(), firstPlace);
    m_out.branch(m_out.below(which, limit), unsure(fillLoop), unsure(filled));

    m_out.appendTo(fillLoop);
    LValue value = m_out.load64(m_out.baseIndex(m_heaps.variables, values, which));
    m_out.branch(isSpreadAt(which), unsure(fillFromArray), unsure(fillFromValue));

    m_out.appendTo(fillFromValue);
    m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, elements, place));
    ValueFromBlock afterValue = m_out.anchor(m_out.add(place, m_out.intPtrOne));
    m_out.jump(fillNext);

    m_out.appendTo(fillFromArray);
    LValue from = m_out.loadPtr(value, m_heaps.JSObject_butterfly);
    LValue elementCount = m_out.zeroExtPtr(m_out.load32(from, m_heaps.Butterfly_publicLength));
    ValueFromBlock copyStart = m_out.anchor(m_out.intPtrZero);
    m_out.jump(copy);

    m_out.appendTo(copy);
    LValue copied = m_out.phi(pointerType(), copyStart);
    ValueFromBlock afterArray = m_out.anchor(m_out.add(place, copied));
    m_out.branch(m_out.below(copied, elementCount), unsure(copyLoop), unsure(fillNext));

    m_out.appendTo(copyLoop);
    LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, from, copied));
    m_out.store64(m_out.select(m_out.isZero64(element), m_out.constInt64(JSValue::encode(jsUndefined())), element), m_out.baseIndex(m_heaps.indexedContiguousProperties, elements, m_out.add(place, copied)));
    m_out.addIncomingToPhi(copied, m_out.anchor(m_out.add(copied, m_out.intPtrOne)));
    m_out.jump(copy);

    m_out.appendTo(fillNext);
    m_out.addIncomingToPhi(place, m_out.anchor(m_out.phi(pointerType(), afterValue, afterArray)));
    m_out.addIncomingToPhi(which, m_out.anchor(m_out.add(which, m_out.intPtrOne)));
    m_out.jump(fill);

    m_out.appendTo(filled);
    mutatorFence();
    return array;
}

LValue Emitter::newArrayLike(LValue length, LValue array, LBasicBlock giveUp)
{
    orElse(isOriginalArray(array), giveUp);
    return newArrayWithSize(length, giveUp);
}

LValue Emitter::newArrayWithSize(LValue length, LBasicBlock giveUp)
{
    orElse(isInt32(length), giveUp);
    LValue count = unboxInt32(length);
    orElse(m_out.below(count, m_out.constInt32(MarkedSpace::largeCutoff / sizeof(EncodedJSValue))), giveUp);
    auto [result, butterfly] = allocateJSArray(m_out.int32Zero, vectorLengthFor(m_out, count), changing32(Instance::offsetOfNewArrayWithContiguousStructureID()), arrayTypeInfoBlob(ArrayWithContiguous), giveUp);
    m_out.store32(count, butterfly, m_heaps.Butterfly_publicLength);
    mutatorFence();
    return result;
}

LValue Emitter::singleCharacterString(LValue character)
{
    return m_out.loadPtr(m_out.baseIndex(m_heaps.singleCharacterStrings, fixedPointer(Instance::offsetOfSingleCharacterStrings()), m_out.zeroExtPtr(character)));
}

Emitter::StringParts Emitter::stringParts(LValue string, LBasicBlock giveUp)
{
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue fiber0 = m_out.loadPtr(string, m_heaps.JSString_value);
    m_out.branch(m_out.testNonZeroPtr(fiber0, m_out.constIntPtr(JSString::isRopeInPointer)), unsure(rope), unsure(flat));

    m_out.appendTo(flat);
    ValueFromBlock flatBase = m_out.anchor(string);
    ValueFromBlock flatImpl = m_out.anchor(fiber0);
    ValueFromBlock flatLength = m_out.anchor(m_out.load32NonNegative(fiber0, m_heaps.StringImpl_length));
    ValueFromBlock flatOffset = m_out.anchor(m_out.int32Zero);
    m_out.jump(continuation);

    m_out.appendTo(rope);
    orElse(m_out.testNonZeroPtr(fiber0, m_out.constIntPtr(JSRopeString::isSubstringInPointer)), giveUp);
    LValue fiber1 = m_out.load64(string, m_heaps.JSRopeString_fiber1);
    LValue fiber2 = m_out.load64(string, m_heaps.JSRopeString_fiber2);
    LValue base = m_out.bitOr(m_out.lShr(fiber1, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(fiber2, m_out.constInt64(0xffff)), m_out.constInt32(32)));
    ValueFromBlock sliceBase = m_out.anchor(base);
    ValueFromBlock sliceImpl = m_out.anchor(m_out.loadPtr(base, m_heaps.JSString_value));
    ValueFromBlock sliceLength = m_out.anchor(m_out.castToInt32(fiber1));
    ValueFromBlock sliceOffset = m_out.anchor(m_out.castToInt32(m_out.lShr(fiber2, m_out.constInt32(16))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return { m_out.phi(pointerType(), flatBase, sliceBase), m_out.phi(pointerType(), flatImpl, sliceImpl), m_out.phi(Int32, flatLength, sliceLength), m_out.phi(Int32, flatOffset, sliceOffset) };
}

LValue Emitter::substringOf(LValue string, const StringParts& pieces, LValue from, LValue to, LBasicBlock giveUp)
{
    LBasicBlock emptyCase = m_out.newBlock();
    LBasicBlock notEmptyCase = m_out.newBlock();
    LBasicBlock oneCharCase = m_out.newBlock();
    LBasicBlock is8Bit = m_out.newBlock();
    LBasicBlock is16Bit = m_out.newBlock();
    LBasicBlock bitsContinuation = m_out.newBlock();
    LBasicBlock multiCharCase = m_out.newBlock();
    LBasicBlock isPartial = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;

    LValue span = m_out.sub(to, from);
    LValue start = m_out.add(from, pieces.offset);
    m_out.branch(m_out.lessThanOrEqual(span, m_out.int32Zero), unsure(emptyCase), unsure(notEmptyCase));

    m_out.appendTo(emptyCase);
    results.append(m_out.anchor(emptyString()));
    m_out.jump(continuation);

    m_out.appendTo(notEmptyCase);
    LValue flags = m_out.load32(pieces.impl, m_heaps.StringImpl_hashAndFlags);
    m_out.branch(m_out.equal(span, m_out.int32One), unsure(oneCharCase), unsure(multiCharCase));

    m_out.appendTo(oneCharCase);
    LValue storage = m_out.loadPtr(pieces.impl, m_heaps.StringImpl_data);
    m_out.branch(m_out.testIsZero32(flags, m_out.constInt32(StringImpl::flagIs8Bit())), unsure(is16Bit), unsure(is8Bit));

    m_out.appendTo(is8Bit);
    ValueFromBlock char8Bit = m_out.anchor(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.characters8, storage, m_out.zeroExtPtr(start))));
    m_out.jump(bitsContinuation);

    m_out.appendTo(is16Bit);
    LValue char16BitValue = m_out.load16ZeroExt32(m_out.baseIndex(m_heaps.characters16, storage, m_out.zeroExtPtr(start)));
    ValueFromBlock char16Bit = m_out.anchor(char16BitValue);
    m_out.branch(m_out.above(char16BitValue, m_out.constInt32(maxSingleCharacterString)), rarely(giveUp), usually(bitsContinuation));

    m_out.appendTo(bitsContinuation);
    results.append(m_out.anchor(singleCharacterString(m_out.phi(Int32, char8Bit, char16Bit))));
    m_out.jump(continuation);

    m_out.appendTo(multiCharCase);
    results.append(m_out.anchor(string));
    m_out.branch(m_out.equal(span, pieces.length), unsure(continuation), unsure(isPartial));

    m_out.appendTo(isPartial);
    orElse(m_out.notEqual(span, m_out.constInt32(2)), giveUp);
    LValue rope = allocateHeapCell(fixedPointer(Instance::offsetOfRopeStringAllocator()), giveUp);
    storeHeader(rope, fixed32(Instance::offsetOfStringStructureID()), TypeInfoBlob(NonArray, TypeInfo(StringType, JSString::StructureFlags)).blob());
    LValue baseIs8BitFlag = m_out.bitAnd(flags, m_out.constInt32(StringImpl::flagIs8Bit()));
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    m_out.storePtr(m_out.bitOr(m_out.constIntPtr(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), m_out.zeroExtPtr(baseIs8BitFlag)), rope, m_heaps.JSRopeString_fiber0);
    m_out.storePtr(m_out.bitOr(m_out.zeroExtPtr(span), m_out.shl(pieces.base, m_out.constInt32(32))), rope, m_heaps.JSRopeString_fiber1);
    m_out.storePtr(m_out.bitOr(m_out.lShr(pieces.base, m_out.constInt32(32)), m_out.shl(m_out.zeroExtPtr(start), m_out.constInt32(16))), rope, m_heaps.JSRopeString_fiber2);
    mutatorFence();
    results.append(m_out.anchor(rope));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), results);
}

LValue Emitter::stringSlice(LValue string, LValue start, LValue end, LBasicBlock giveUp)
{
    StringParts pieces = stringParts(string, giveUp);
    LValue length = pieces.length;
    auto pickIndex = [&](LValue index) {
        return m_out.select(m_out.greaterThanOrEqual(index, m_out.int32Zero),
            m_out.select(m_out.above(index, length), length, index),
            m_out.select(m_out.lessThan(m_out.add(length, index), m_out.int32Zero), m_out.int32Zero, m_out.add(length, index)));
    };
    return substringOf(string, pieces, pickIndex(start), pickIndex(end), giveUp);
}

LValue Emitter::stringSubstring(LValue string, LValue start, LValue end, LBasicBlock giveUp)
{
    StringParts pieces = stringParts(string, giveUp);
    LValue length = pieces.length;
    auto clampIndex = [&](LValue index) {
        return m_out.select(m_out.lessThan(index, m_out.int32Zero), m_out.int32Zero, m_out.select(m_out.greaterThan(index, length), length, index));
    };
    LValue isReversed = m_out.greaterThan(start, end);
    return substringOf(string, pieces, clampIndex(m_out.select(isReversed, end, start)), clampIndex(m_out.select(isReversed, start, end)), giveUp);
}

LValue Emitter::makeRope(LValue first, LValue second, LValue third, LBasicBlock giveUp)
{
    struct FlagsAndLength {
        LValue flags;
        LValue length;
    };
    auto getFlagsAndLength = [&](LValue child) {
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock ropeCase = m_out.newBlock();
        LBasicBlock notRopeCase = m_out.newBlock();
        m_out.branch(isRopeString(child), unsure(ropeCase), unsure(notRopeCase));

        m_out.appendTo(ropeCase);
        ValueFromBlock flagsForRope = m_out.anchor(m_out.load32NonNegative(child, m_heaps.JSRopeString_flags));
        ValueFromBlock lengthForRope = m_out.anchor(m_out.load32NonNegative(child, m_heaps.JSRopeString_length));
        m_out.jump(continuation);

        m_out.appendTo(notRopeCase);
        LValue stringImpl = m_out.loadPtr(child, m_heaps.JSString_value);
        ValueFromBlock flagsForNonRope = m_out.anchor(m_out.load32NonNegative(stringImpl, m_heaps.StringImpl_hashAndFlags));
        ValueFromBlock lengthForNonRope = m_out.anchor(m_out.load32NonNegative(stringImpl, m_heaps.StringImpl_length));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        return FlagsAndLength { m_out.phi(Int32, flagsForRope, flagsForNonRope), m_out.phi(Int32, lengthForRope, lengthForNonRope) };
    };

    FlagsAndLength firstInfo = getFlagsAndLength(first);
    FlagsAndLength secondInfo = getFlagsAndLength(second);
    LValue flags = m_out.bitAnd(firstInfo.flags, secondInfo.flags);
    LValue utf16Length = m_out.add(m_out.zeroExtPtr(firstInfo.length), m_out.zeroExtPtr(secondInfo.length));
    if (third) {
        FlagsAndLength thirdInfo = getFlagsAndLength(third);
        flags = m_out.bitAnd(flags, thirdInfo.flags);
        utf16Length = m_out.add(utf16Length, m_out.zeroExtPtr(thirdInfo.length));
    }
    orElse(m_out.belowOrEqual(utf16Length, m_out.constIntPtr(JSString::MaxLength)), giveUp);
    LValue length = m_out.castToInt32(utf16Length);

    LBasicBlock make = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    if (!third) {
        LBasicBlock firstNonEmptyCase = m_out.newBlock();
        results.append(m_out.anchor(second));
        m_out.branch(m_out.isZero32(firstInfo.length), rarely(continuation), usually(firstNonEmptyCase));
        m_out.appendTo(firstNonEmptyCase);
        results.append(m_out.anchor(first));
        m_out.branch(m_out.isZero32(secondInfo.length), rarely(continuation), usually(make));
    } else {
        results.append(m_out.anchor(emptyString()));
        m_out.branch(m_out.isZero32(length), rarely(continuation), usually(make));
    }

    m_out.appendTo(make);
    LValue result = allocateHeapCell(fixedPointer(Instance::offsetOfRopeStringAllocator()), giveUp);
    storeHeader(result, fixed32(Instance::offsetOfStringStructureID()), TypeInfoBlob(NonArray, TypeInfo(StringType, JSString::StructureFlags)).blob());
    m_out.storePtr(m_out.bitOr(m_out.bitOr(first, m_out.constIntPtr(JSString::isRopeInPointer)), m_out.bitAnd(m_out.constIntPtr(JSRopeString::is8BitInPointer), m_out.zeroExtPtr(flags))), result, m_heaps.JSRopeString_fiber0);
    m_out.storePtr(m_out.bitOr(m_out.zeroExtPtr(length), m_out.shl(second, m_out.constInt32(32))), result, m_heaps.JSRopeString_fiber1);
    if (!third)
        m_out.storePtr(m_out.lShr(second, m_out.constInt32(32)), result, m_heaps.JSRopeString_fiber2);
    else
        m_out.storePtr(m_out.bitOr(m_out.lShr(second, m_out.constInt32(32)), m_out.shl(third, m_out.constInt32(16))), result, m_heaps.JSRopeString_fiber2);
    mutatorFence();
    results.append(m_out.anchor(result));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), results);
}

LValue Emitter::addStrings(LValue first, LValue second, LBasicBlock giveUp)
{
    orElse(m_out.bitAnd(isCell(first), isCell(second)), giveUp);
    orElse(m_out.bitAnd(isCellOfType(first, StringType), isCellOfType(second, StringType)), giveUp);
    return makeRope(first, second, nullptr, giveUp);
}

LValue Emitter::int32ToString(LValue value, LBasicBlock giveUp)
{
    LBasicBlock smallIntCase = m_out.newBlock();
    LBasicBlock intCacheCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.below(value, m_out.constInt32(NumericStrings::cacheSize)), unsure(smallIntCase), unsure(intCacheCase));

    m_out.appendTo(smallIntCase);
    LValue smallIntString = m_out.loadPtr(m_out.baseIndex(m_heaps.SmallIntCache, fixedPointer(Instance::offsetOfSmallIntStrings()), m_out.zeroExtPtr(value), JSValue(), NumericStrings::StringWithJSString::offsetOfJSString()));
    ValueFromBlock smallIntResult = m_out.anchor(smallIntString);
    m_out.branch(m_out.isNull(smallIntString), unsure(giveUp), unsure(continuation));

    m_out.appendTo(intCacheCase);
    LValue wide = m_out.zeroExt(value, Int64);
    LValue first = m_out.bitXor(wide, m_out.constInt64(0x2d358dccaa6c78a5ULL));
    LValue second = m_out.bitXor(wide, m_out.constInt64(0x8bb84b93962eacc9ULL));
    LValue hash = m_out.castToInt32(m_out.bitXor(m_out.mul(first, second), m_out.uMulHigh(first, second)));
    LValue index = m_out.zeroExtPtr(m_out.bitAnd(hash, m_out.constInt32(NumericStrings::cacheSize - 1)));
    LValue cache = fixedPointer(Instance::offsetOfIntStrings());
    LValue key = m_out.load32(m_out.baseIndex(m_heaps.IntCache, cache, index, JSValue(), NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()));
    LValue cachedString = m_out.loadPtr(m_out.baseIndex(m_heaps.IntCache, cache, index, JSValue(), NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()));
    orElse(m_out.equal(key, value), giveUp);
    ValueFromBlock intCacheResult = m_out.anchor(cachedString);
    m_out.branch(m_out.isNull(cachedString), unsure(giveUp), unsure(continuation));

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), smallIntResult, intCacheResult);
}

LValue Emitter::stringOrInt32ToString(LValue value, LBasicBlock giveUp)
{
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock notCellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value), usually(cellCase), rarely(notCellCase));

    m_out.appendTo(cellCase);
    ValueFromBlock string = m_out.anchor(value);
    m_out.branch(isCellOfType(value, StringType), usually(continuation), rarely(giveUp));

    m_out.appendTo(notCellCase);
    orElse(isInt32(value), giveUp);
    ValueFromBlock converted = m_out.anchor(int32ToString(unboxInt32(value), giveUp));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), string, converted);
}

LValue Emitter::addStringsOrInt32s(LValue first, LValue second, LBasicBlock giveUp)
{
    orElse(m_out.bitOr(isCell(first), isCell(second)), giveUp);
    LValue firstString = stringOrInt32ToString(first, giveUp);
    return makeRope(firstString, stringOrInt32ToString(second, giveUp), nullptr, giveUp);
}

LValue Emitter::concatenate(LValue values, LValue count, LBasicBlock giveUp)
{
    auto at = [&](unsigned i) {
        return stringOrInt32ToString(m_out.load64(m_out.baseIndex(m_heaps.variables, values, m_out.constIntPtr(i))), giveUp);
    };
    orElse(m_out.below(m_out.sub(count, m_out.constInt32(2)), m_out.constInt32(4)), giveUp);
    LBasicBlock twoCase = m_out.newBlock();
    LBasicBlock atLeastThreeCase = m_out.newBlock();
    LBasicBlock atLeastFourCase = m_out.newBlock();
    LBasicBlock fourCase = m_out.newBlock();
    LBasicBlock fiveCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    LValue first = at(0);
    LValue second = at(1);
    m_out.branch(m_out.equal(count, m_out.constInt32(2)), unsure(twoCase), unsure(atLeastThreeCase));

    m_out.appendTo(twoCase);
    results.append(m_out.anchor(makeRope(first, second, nullptr, giveUp)));
    m_out.jump(continuation);

    m_out.appendTo(atLeastThreeCase);
    LValue firstThree = makeRope(first, second, at(2), giveUp);
    results.append(m_out.anchor(firstThree));
    m_out.branch(m_out.equal(count, m_out.constInt32(3)), unsure(continuation), unsure(atLeastFourCase));

    m_out.appendTo(atLeastFourCase);
    LValue fourth = at(3);
    m_out.branch(m_out.equal(count, m_out.constInt32(4)), unsure(fourCase), unsure(fiveCase));

    m_out.appendTo(fourCase);
    results.append(m_out.anchor(makeRope(firstThree, fourth, nullptr, giveUp)));
    m_out.jump(continuation);

    m_out.appendTo(fiveCase);
    results.append(m_out.anchor(makeRope(firstThree, fourth, at(4), giveUp)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), results);
}

LValue Emitter::keysOfObject(LValue object, LBasicBlock giveUp)
{
    LValue previousOrRareData = m_out.loadPtr(structureOf(object), m_heaps.Structure_previousOrRareData);
    orElse(m_out.notNull(previousOrRareData), giveUp);
    orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), giveUp);
    ASSERT(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()) == 1);
    LValue cached = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedEnumerableStrings);
    orElse(m_out.above(cached, m_out.constIntPtr(1)), giveUp);
    return newArrayFromButterfly(cached, giveUp);
}

void Emitter::addTypedField(LValue object, LValue storedValue, LValue slot, LBasicBlock giveUp)
{
    LValue structureID = m_out.load32(object, m_heaps.JSCell_structureID);
    static_assert(Instance::fieldAdditionIndex(0x120, 3) == (((0x120u >> 4) ^ (3 * 0x9e5u)) & (Instance::numberOfFieldAdditions - 1)));
    LValue index = m_out.bitAnd(m_out.bitXor(m_out.lShr(structureID, m_out.constInt32(4)), m_out.mul(slot, m_out.constInt32(0x9e5))), m_out.constInt32(Instance::numberOfFieldAdditions - 1));
    static_assert(sizeof(Instance::FieldAddition) == 16);
    LValue entry = m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(index), m_out.constInt32(4)), m_out.constIntPtr(Instance::offsetOfFieldAdditions())));
    orElse(m_out.equal(m_out.load32(TypedPointer(m_heaps.AOTInstance_mutableFields, entry)), structureID), giveUp);
    orElse(m_out.equal(m_out.load32(m_out.address(m_heaps.AOTInstance_mutableFields, entry, OBJECT_OFFSETOF(Instance::FieldAddition, slot))), slot), giveUp);
    m_out.store64(storedValue, TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(object, m_out.add(m_out.shl(m_out.zeroExtPtr(slot), m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage())))));
    m_out.store32(m_out.load32(m_out.address(m_heaps.AOTInstance_mutableFields, entry, OBJECT_OFFSETOF(Instance::FieldAddition, structureIDAfterAddition))), object, m_heaps.JSCell_structureID);
}

void Emitter::setArrayLength(LValue array, LValue length, LBasicBlock giveUp)
{
    orElse(isCell(array), giveUp);
    orElse(isInt32(length), giveUp);
    LValue mode = m_out.bitAnd(m_out.load8ZeroExt32(array, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IsArray | IndexingShapeMask | CopyOnWrite));
    orElse(m_out.bitOr(m_out.equal(mode, m_out.constInt32(ArrayWithContiguous)), m_out.equal(mode, m_out.constInt32(ArrayWithInt32))), giveUp);
    orElse(isCellOfType(array, ArrayType), giveUp);
    LValue butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
    LValue before = m_out.load32(butterfly, m_heaps.Butterfly_publicLength);
    LValue afterwards = unboxInt32(length);
    orElse(m_out.belowOrEqual(afterwards, before), giveUp);
    orElse(m_out.belowOrEqual(m_out.sub(before, afterwards), m_out.constInt32(64)), giveUp);
    splatWords(butterfly, afterwards, before, m_out.int64Zero, m_heaps.indexedContiguousProperties.atAnyIndex());
    m_out.store32(afterwards, butterfly, m_heaps.Butterfly_publicLength);
}

namespace {

class Helpers final : public Emitter {
public:
    explicit Helpers(Procedure& proc)
        : Emitter(proc)
    {
    }

    void build(Stub stub)
    {
        m_out.initialize(m_heaps);
        m_out.setFrequency(1);
        LBasicBlock start = m_out.newBlock();
        LBasicBlock giveUp = m_out.newBlock();
        m_out.appendTo(start);
        m_out.initializeConstants(m_proc, start);
        findPinnedRegisters();
        LValue arguments[4];
        for (unsigned i = 0; i < std::size(arguments); ++i)
            arguments[i] = registerOnEntry(GPRInfo::toArgumentRegister(i));
        auto int32At = [&](unsigned i) { return m_out.castToInt32(arguments[i]); };

        LValue result = nullptr;
        switch (stub) {
        case Stub::HelperNewArray:
            result = newArrayFromValues(arguments[0], int32At(1), false, giveUp);
            break;
        case Stub::HelperNewInt32Array:
            result = newArrayFromValues(arguments[0], int32At(1), true, giveUp);
            break;
        case Stub::HelperNewArrayBuffer:
            result = newArrayFromButterfly(arguments[0], giveUp);
            break;
        case Stub::HelperNewActivation:
            result = newActivation(arguments[0], arguments[1], arguments[2], int32At(3), giveUp);
            break;
        case Stub::HelperNewPromise:
            result = newPromise(giveUp);
            break;
        case Stub::HelperNewResolvedPromise:
            result = newResolvedPromise(arguments[0], giveUp);
            break;
        case Stub::HelperNewGenerator:
            result = newInternalFieldObject(Instance::InlineAllocation::Generator, JSGenerator::initialValues(), giveUp);
            break;
        case Stub::HelperNewAsyncFunctionGenerator:
            result = newInternalFieldObject(Instance::InlineAllocation::AsyncFunctionGenerator, JSAsyncFunctionGenerator::initialValues(), giveUp);
            break;
        case Stub::HelperNewMap:
            result = newMapOrSet(Instance::InlineAllocation::Map, giveUp);
            break;
        case Stub::HelperNewSet:
            result = newMapOrSet(Instance::InlineAllocation::Set, giveUp);
            break;
        case Stub::HelperNewArrayWithSpread:
            result = newArrayWithSpread(arguments[0], int32At(1), int32At(2), giveUp);
            break;
        case Stub::HelperNewArrayWithSpecies:
            result = newArrayLike(arguments[0], arguments[1], giveUp);
            break;
        case Stub::HelperNewArrayWithSize:
            result = newArrayWithSize(arguments[0], giveUp);
            break;
        case Stub::HelperStringSlice:
            result = stringSlice(arguments[0], int32At(1), int32At(2), giveUp);
            break;
        case Stub::HelperStringSubstring:
            result = stringSubstring(arguments[0], int32At(1), int32At(2), giveUp);
            break;
        case Stub::HelperMakeRope2:
            result = makeRope(arguments[0], arguments[1], nullptr, giveUp);
            break;
        case Stub::HelperMakeRope3:
            result = makeRope(arguments[0], arguments[1], arguments[2], giveUp);
            break;
        case Stub::HelperInt32ToString:
            result = int32ToString(int32At(0), giveUp);
            break;
        case Stub::HelperObjectKeys:
            result = keysOfObject(arguments[0], giveUp);
            break;
        case Stub::HelperAddStrings:
            result = addStringsOrInt32s(arguments[0], arguments[1], giveUp);
            break;
        case Stub::HelperStrcat:
            result = concatenate(arguments[0], int32At(1), giveUp);
            break;
        case Stub::HelperSetArrayLength:
            setArrayLength(arguments[0], arguments[1], giveUp);
            result = m_out.intPtrOne;
            break;
        case Stub::HelperAddField:
            addTypedField(arguments[0], arguments[1], int32At(2), giveUp);
            result = m_out.intPtrOne;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        m_out.ret(result);
        m_out.appendTo(giveUp);
        m_out.ret(m_out.intPtrZero);
    }
};

} // anonymous namespace

void pinRegisters(Procedure& proc)
{
    proc.pinRegister(instanceGPR);
    proc.pinRegister(GPRInfo::numberTagRegister);
    proc.pinRegister(GPRInfo::notCellMaskRegister);
#if CPU(ARM64)
    if (proc.mutableGPRs().contains(ARM64Registers::x18, IgnoreVectors))
        proc.pinRegister(ARM64Registers::x18);
#endif
}

void generateHelper(CCallHelpers& jit, Stub stub)
{
    Procedure proc(/* usesSIMD = */ false);
    proc.setPositionIndependent();
    pinRegisters(proc);
    Helpers helpers(proc);
    helpers.build(stub);
    for (Value* value : proc.values()) {
        if (value->hasInt64())
            RELEASE_ASSERT_WITH_MESSAGE(static_cast<uint64_t>(value->asInt64()) < 4 * GB || static_cast<uint64_t>(value->asInt64()) >= (1ULL << 47), "An address in the code of a helper");
    }
    prepareForGeneration(proc);
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
