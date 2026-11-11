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
#include "IndexingType.h"
#include "JSPropertyNameEnumerator.h"
#include "StructureRareData.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(!NonArray && ArrayClass == 1 && UndecidedShape == 2 && ArrayWithUndecided == 3);
static_assert(sizeof(WriteBarrier<JSString>) == sizeof(JSString*));
static_assert(sizeof(EnumeratorCacheEntry) == 16 && hasOneBitSet(numberOfCachedEnumerators));
static_assert(indexOfCachedEnumerator(3 << enumeratorHashShift, numberOfCachedEnumerators + 4) == 7);
static_assert(!static_cast<size_t>(ForInStepResult::Done) && static_cast<size_t>(ForInStepResult::Key) == 1 && static_cast<size_t>(ForInStepResult::Position) == 2);
static_assert(static_cast<size_t>(ForInStepResult::CurrentNames) == 3 && numberOfEnumerators<ForInStepResult> == 4);
static_assert(!static_cast<size_t>(ForInOpenResult::Enumerator) && static_cast<size_t>(ForInOpenResult::CurrentNames) == 1 && numberOfEnumerators<ForInOpenResult> == 2);

static void takeForInSlowPathUnless(Output& out, Condition mustHold, LBasicBlock slowPath)
{
    LBasicBlock continuation = out.newBlock();
    out.branch(mustHold.value(), normalEdge(continuation), unlikelyEdge(slowPath));
    out.appendTo(continuation);
}

void ForInEmitter::generateForInOpen(const ValueContext& value)
{
    define(value, openEnumerator(value, false)[static_cast<size_t>(ForInOpenResult::Enumerator)].as<CellPointer>());
}

void ForInEmitter::generateForInOpenCountingNames(const ValueContext& value)
{
    defineResults(value, openEnumerator(value, true));
}

std::array<Operand, 2> ForInEmitter::openEnumerator(const ValueContext& value, bool countsCurrentNames)
{
    Boxed object = in<Boxed>(value->objectOperand());
    LBasicBlock isNullish = newBlock();
    LBasicBlock isNotNullish = newBlock();
    LBasicBlock isCompiledObject = newBlock();
    LBasicBlock isOtherCell = newBlock();
    LBasicBlock slowPath = newBlock();
    ResultsJoin<CellPointer, Integer> join(m_out);
    auto addCurrent = [&](CellPointer enumerator) {
        join.add(enumerator, countsCurrentNames ? zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endOwnPropertyIndex)) : integer(0));
    };

    branch(isUndefinedOrNull(object, typeOf(value->objectOperand()).primitives), { unlikelyEdge(isNullish), normalEdge(isNotNullish) });

    m_out.appendTo(isNullish);
    join.add(CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(iteration.emptyEnumerator)).value()), integer(0));

    m_out.appendTo(isNotNullish);
    takeForInSlowPathUnless(m_out, isCell(object), slowPath);
    CellPointer cell = unboxCell(object);
    branch(hasMutableCellType(cell, CompiledObjectType), { normalEdge(isCompiledObject), normalEdge(isOtherCell) });

    m_out.appendTo(isCompiledObject);
    const Machine::AbstractHeap& published = m_heaps.entire(AbstractHeap::RuntimeState);
    Word32 structureID = loadStructureID(cell);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    Integer index = zeroExtended(bitAnd(add(shiftRightUnsigned(structureID, enumeratorHashShift), orderWord), numberOfCachedEnumerators - 1));
    LValue entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.enumerators)), index, 4).value();
    takeForInSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, structureID)))), structureID), slowPath);
    takeForInSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, orderWord)))), orderWord), slowPath);
    addCurrent(CellPointer(m_out.loadPtr(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, enumerator)))));

    m_out.appendTo(isOtherCell);
    Word32 indexingType = bitAnd(loadByte(cell, m_heaps.JSCell_indexingTypeAndMisc), IndexingTypeMask);
    takeForInSlowPathUnless(m_out, belowOrEqual(indexingType, word32(ArrayWithUndecided)), slowPath);

    Pointer previousOrRareData = loadField<Pointer>(loadStructure(cell), m_heaps.Structure_previousOrRareData);
    takeForInSlowPathUnless(m_out, notEqual(previousOrRareData, pointerConstant(0)), slowPath);
    takeForInSlowPathUnless(m_out, logicalNot(hasCellType(CellPointer(previousOrRareData.value()), StructureType)), slowPath);

    Pointer cachedEnumeratorAndFlag = loadField<Pointer>(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
    takeForInSlowPathUnless(m_out, notEqual(cachedEnumeratorAndFlag, pointerConstant(0)), slowPath);
    takeForInSlowPathUnless(m_out, hasNoBits(cachedEnumeratorAndFlag, StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag), slowPath);
    addCurrent(CellPointer(cachedEnumeratorAndFlag.value()));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::GetPropertyEnumerator, { object }), integer(0));

    return join.finish();
}

void ForInEmitter::generateForInStep(const ValueContext& value)
{
    CellPointer enumerator = in<CellPointer>(value->enumeratorOperand());
    Boxed object = in<Boxed>(value->objectOperand());
    Integer position = integerOf(value->positionOperand());
    LBasicBlock isOwnNameToCheck = newBlock();
    LBasicBlock isNotStructureName = newBlock();
    std::optional<BlockEdge> end = generator().edgeAfterLastStep(value, static_cast<uint32_t>(ForInStepResult::Done));
    LBasicBlock isPastEnd = end ? nullptr : newBlock();
    LBasicBlock slowPath = newBlock();
    ResultsJoin<Condition, CellPointer, Integer, Integer> join(m_out);
    Join<Integer> isStructureName(m_out);

    const bool checksPresenceOnce = !!value->currentNamesOperand();
    EnumeratorFields fields;
    if (!checksPresenceOnce)
        fields = loadFields(enumerator);
    Integer nameIndex = checksPresenceOnce ? nameIndexOf(enumerator, position) : nameIndexOf(fields, position);
    Integer numberOfCurrentNames = checksPresenceOnce ? integerOf(value->currentNamesOperand()) : integer(0);
    if (checksPresenceOnce) {
        LBasicBlock isKnownToBeCurrent = newBlock();
        LBasicBlock isNotKnownToBeCurrent = newBlock();
        branch(below(nameIndex, numberOfCurrentNames), { normalEdge(isKnownToBeCurrent), normalEdge(isNotKnownToBeCurrent) });
        m_out.appendTo(isKnownToBeCurrent);
        isStructureName.add(numberOfCurrentNames);
        m_out.appendTo(isNotKnownToBeCurrent);
        fields = loadFields(enumerator);
    }
    branch(isOwnName(fields, nameIndex), { checksPresenceOnce ? unlikelyEdge(isOwnNameToCheck) : normalEdge(isOwnNameToCheck), normalEdge(isNotStructureName) });

    m_out.appendTo(isOwnNameToCheck);
    takeForInSlowPathUnless(m_out, isCell(object), slowPath);
    takeForInSlowPathUnless(m_out, hasCachedKeys(enumerator, fields, unboxCell(object)), slowPath);
    isStructureName.add(checksPresenceOnce ? zeroExtended(fields.endOwnPropertyIndex) : numberOfCurrentNames);

    Integer numberOfNamesCurrentAtName = isStructureName.finish();
    Condition isNotDone = boolean(false);
    Pointer names = loadField<Pointer>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVector);
    CellPointer name(m_out.loadPtr(m_out.baseIndex(m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVectorContents, names.value(), nameIndex.value())));
    ensureStillAliveHere(enumerator);
    join.add(isNotDone, name, position, numberOfNamesCurrentAtName);

    m_out.appendTo(isNotStructureName);
    branch(greaterThanOrEqual(nameIndex, zeroExtended(fields.endGenericPropertyIndex)), { end ? *end : normalEdge(isPastEnd), unlikelyEdge(slowPath) });

    if (!end) {
        m_out.appendTo(isPastEnd);
        Condition hasEnded = boolean(true);
        join.add(hasEnded, emptyString(), position, numberOfCurrentNames);
    }

    m_out.appendTo(slowPath);
    Operand arguments[] = { enumerator, object, position };
    StubCallResult next = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::EnumeratorNext, arguments);
    Condition isDone = isZero(Integer(next.first));
    if (end) {
        LBasicBlock goesOn = newBlock();
        branch(isDone, { *end, normalEdge(goesOn) });
        m_out.appendTo(goesOn);
        join.add(isNotDone, CellPointer(next.first), Integer(next.second), integer(0));
    } else
        join.add(isDone, select(isDone, emptyString(), CellPointer(next.first)), Integer(next.second), integer(0));

    auto results = join.finish();
    if (end)
        results[static_cast<size_t>(ForInStepResult::Done)] = isNotDone;
    defineResults(value, results);
}

ForInEmitter::EnumeratorFields ForInEmitter::loadFields(CellPointer enumerator)
{
    return {
        .cachedStructureID = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureID),
        .indexedLength = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength),
        .endOwnPropertyIndex = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endOwnPropertyIndex),
        .endGenericPropertyIndex = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex),
    };
}

Condition ForInEmitter::hasCachedKeys(CellPointer enumerator, const EnumeratorFields& fields, CellPointer object)
{
    Condition hasCachedStructure = equal(loadStructureID(object), fields.cachedStructureID);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOfAnyCell(object);
    Word32 mask = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWordMask);
    Word32 cachedOrderWord = loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedOrderWord);
    return logicalAnd(hasCachedStructure, equal(bitAnd(orderWord, mask), cachedOrderWord));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
