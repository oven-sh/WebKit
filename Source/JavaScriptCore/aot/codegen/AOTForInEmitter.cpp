/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTForInEmitter.h"

#if ENABLE(AOT)

#include "AOTResultsJoin.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSPropertyNameEnumerator.h"
#include "StructureRareData.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(!NonArray && ArrayClass == 1 && UndecidedShape == 2 && ArrayWithUndecided == 3);
static_assert(sizeof(WriteBarrier<JSString>) == sizeof(JSString*));
static_assert(sizeof(EnumeratorCacheEntry) == 16 && hasOneBitSet(numberOfCachedEnumerators));
static_assert(indexOfCachedEnumerator(3 << enumeratorHashShift, numberOfCachedEnumerators + 4) == 7);
static_assert(!static_cast<size_t>(ForInStepResult::Done) && static_cast<size_t>(ForInStepResult::Key) == 1 && static_cast<size_t>(ForInStepResult::Position) == 2);
static_assert(numberOfEnumerators<ForInStepResult> == 3);

static void takeSlowPathUnless(Output& out, Condition mustHold, LBasicBlock slowPath)
{
    LBasicBlock continuation = out.newBlock();
    out.branch(mustHold.value(), normalEdge(continuation), unlikelyEdge(slowPath));
    out.appendTo(continuation);
}

void ForInEmitter::generateForInOpen(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    LBasicBlock isNullish = newBlock();
    LBasicBlock isNotNullish = newBlock();
    LBasicBlock isCompiledObject = newBlock();
    LBasicBlock isOtherCell = newBlock();
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);

    branch(isUndefinedOrNull(object, typeOf(value->objectOperand()).primitives), { unlikelyEdge(isNullish), normalEdge(isNotNullish) });

    m_out.appendTo(isNullish);
    join.add(CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(iteration.emptyEnumerator)).value()));

    m_out.appendTo(isNotNullish);
    takeSlowPathUnless(m_out, isCell(object), slowPath);
    CellPointer cell = unboxCell(object);
    branch(hasMutableCellType(cell, CompiledObjectType), { normalEdge(isCompiledObject), normalEdge(isOtherCell) });

    m_out.appendTo(isCompiledObject);
    const B3::AbstractHeap& published = m_heaps.entire(AbstractHeap::RuntimeState);
    Word32 structureID = loadStructureID(cell);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    Integer index = zeroExtended(bitAnd(add(shiftRightUnsigned(structureID, word32(enumeratorHashShift)), orderWord), word32(numberOfCachedEnumerators - 1)));
    LValue entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.enumerators)), index, 4).value();
    takeSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, structureID)))), structureID), slowPath);
    takeSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, orderWord)))), orderWord), slowPath);
    join.add(CellPointer(m_out.loadPtr(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, enumerator)))));

    m_out.appendTo(isOtherCell);
    Word32 indexingType = bitAnd(loadByte(cell, m_heaps.JSCell_indexingTypeAndMisc), word32(IndexingTypeMask));
    takeSlowPathUnless(m_out, belowOrEqual(indexingType, word32(ArrayWithUndecided)), slowPath);

    Pointer previousOrRareData = loadField<Pointer>(loadStructure(cell), m_heaps.Structure_previousOrRareData);
    takeSlowPathUnless(m_out, notEqual(previousOrRareData, pointerConstant(0)), slowPath);
    takeSlowPathUnless(m_out, logicalNot(hasCellType(CellPointer(previousOrRareData.value()), StructureType)), slowPath);

    Pointer cachedEnumeratorAndFlag = loadField<Pointer>(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
    takeSlowPathUnless(m_out, notEqual(cachedEnumeratorAndFlag, pointerConstant(0)), slowPath);
    takeSlowPathUnless(m_out, hasNoBits(cachedEnumeratorAndFlag, StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag), slowPath);
    join.add(CellPointer(cachedEnumeratorAndFlag.value()));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::GetPropertyEnumerator, { object }));

    define(value, join.finish());
}

void ForInEmitter::generateForInStep(const ValueContext& value)
{
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    Boxed object = in<Boxed>(value->objectOperand());
    Integer position = integerOf(value->positionOperand());
    LBasicBlock isStructureName = newBlock();
    LBasicBlock isNotStructureName = newBlock();
    LBasicBlock isPastEnd = newBlock();
    LBasicBlock slowPath = newBlock();
    ResultsJoin<Condition, CellPointer, Integer> join(m_out);

    EnumeratorFields fields = loadFields(enumerator);
    Integer nameIndex = nameIndexOf(fields, position);
    branch(isOwnName(fields, nameIndex), { normalEdge(isStructureName), normalEdge(isNotStructureName) });

    m_out.appendTo(isStructureName);
    takeSlowPathUnless(m_out, isCell(object), slowPath);
    takeSlowPathUnless(m_out, hasCachedKeys(enumerator, fields, unboxCell(object)), slowPath);
    Condition isNotDone = boolean(false);
    Pointer names = loadField<Pointer>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVector);
    CellPointer name(m_out.loadPtr(m_out.baseIndex(m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVectorContents, names.value(), nameIndex.value())));
    // The collector reaches the names through the enumerator alone, and B3 may load `names` once in front of the loop and let the enumerator die.
    ensureStillAliveHere(enumerator);
    join.add(isNotDone, name, position);

    m_out.appendTo(isNotStructureName);
    branch(greaterThanOrEqual(nameIndex, zeroExtended(fields.endGenericPropertyIndex)), { normalEdge(isPastEnd), unlikelyEdge(slowPath) });

    m_out.appendTo(isPastEnd);
    join.add(boolean(true), emptyString(), position);

    m_out.appendTo(slowPath);
    Operand arguments[] = { enumerator, object, position };
    StubCallResult next = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::EnumeratorNext, arguments);
    Condition isDone = isZero(Integer(next.first));
    join.add(isDone, select(isDone, emptyString(), CellPointer(next.first)), Integer(next.second));

    defineResults(value, join.finish());
}

ForInEmitter::EnumeratorFields ForInEmitter::loadFields(CellPointer enumerator)
{
    return {
        .cachedStructureID = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureID),
        .indexedLength = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength),
        .endStructurePropertyIndex = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endStructurePropertyIndex),
        .endOwnPropertyIndex = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endOwnPropertyIndex),
        .endGenericPropertyIndex = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex),
    };
}

Condition ForInEmitter::hasCachedKeys(CellPointer enumerator, const EnumeratorFields& fields, CellPointer object)
{
    Condition hasCachedStructure = equal(loadStructureID(object), fields.cachedStructureID);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(object);
    Word32 mask = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWordMask);
    Word32 cachedOrderWord = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWord);
    return logicalAnd(hasCachedStructure, equal(bitAnd(orderWord, mask), cachedOrderWord));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
