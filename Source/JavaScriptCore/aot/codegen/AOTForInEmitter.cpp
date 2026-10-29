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
#include "AOTRuntimeValidationEmitter.h"
#include "AOTVector.h"
#include "B3VariableValue.h"
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
    // The enumerator, and how many of its leading names the object is known to have.
    ResultsJoin<CellPointer, Integer> join(m_out);
    const bool hasStepCheckingPresenceOnce = m_function.values.containsIf([&](const Value& step) {
        return step.opcode == Opcode::ForInStep && step.keysChangeOnlyInSteps() && step.enumeratorOperand() == value.id();
    });
    // The cached state for the object's Structure and key order is current.
    auto addCurrent = [&](CellPointer enumerator) {
        join.add(enumerator, hasStepCheckingPresenceOnce ? zeroExtended(loadField<Word32>(enumerator, m_heaps.JSPropertyNameEnumerator_endOwnPropertyIndex)) : integer(0));
    };

    branch(isUndefinedOrNull(object, typeOf(value->objectOperand()).primitives), { unlikelyEdge(isNullish), normalEdge(isNotNullish) });

    m_out.appendTo(isNullish);
    join.add(CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(iteration.emptyEnumerator)).value()), integer(0));

    m_out.appendTo(isNotNullish);
    takeSlowPathUnless(m_out, isCell(object), slowPath);
    CellPointer cell = unboxCell(object);
    branch(hasMutableCellType(cell, CompiledObjectType), { normalEdge(isCompiledObject), normalEdge(isOtherCell) });

    m_out.appendTo(isCompiledObject);
    const B3::AbstractHeap& published = m_heaps.entire(AbstractHeap::RuntimeState);
    Word32 structureID = loadStructureID(cell);
    Word32 orderWord = emitter<ShapeTestEmitter>().orderWordOf(cell);
    Integer index = zeroExtended(bitAnd(add(shiftRightUnsigned(structureID, enumeratorHashShift), orderWord), numberOfCachedEnumerators - 1));
    LValue entry = offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(objects.enumerators)), index, 4).value();
    takeSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, structureID)))), structureID), slowPath);
    takeSlowPathUnless(m_out, equal(Word32(m_out.load32(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, orderWord)))), orderWord), slowPath);
    addCurrent(CellPointer(m_out.loadPtr(m_out.address(published, entry, OBJECT_OFFSETOF(EnumeratorCacheEntry, enumerator)))));

    m_out.appendTo(isOtherCell);
    Word32 indexingType = bitAnd(loadByte(cell, m_heaps.JSCell_indexingTypeAndMisc), IndexingTypeMask);
    takeSlowPathUnless(m_out, belowOrEqual(indexingType, word32(ArrayWithUndecided)), slowPath);

    Pointer previousOrRareData = loadField<Pointer>(loadStructure(cell), m_heaps.Structure_previousOrRareData);
    takeSlowPathUnless(m_out, notEqual(previousOrRareData, pointerConstant(0)), slowPath);
    takeSlowPathUnless(m_out, logicalNot(hasCellType(CellPointer(previousOrRareData.value()), StructureType)), slowPath);

    Pointer cachedEnumeratorAndFlag = loadField<Pointer>(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
    takeSlowPathUnless(m_out, notEqual(cachedEnumeratorAndFlag, pointerConstant(0)), slowPath);
    takeSlowPathUnless(m_out, hasNoBits(cachedEnumeratorAndFlag, StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag), slowPath);
    addCurrent(CellPointer(cachedEnumeratorAndFlag.value()));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::GetPropertyEnumerator, { object }), integer(0));

    auto [enumerator, numberOfCurrentNames] = join.finish();
    define(value, enumerator.as<CellPointer>());
    if (hasStepCheckingPresenceOnce)
        setNumberOfCurrentNames(value.id(), numberOfCurrentNames.as<Integer>());
}

B3::Variable* ForInEmitter::numberOfCurrentNamesOf(ValueID open)
{
    for (auto& [id, variable] : m_numbersOfCurrentNames) {
        if (id == open)
            return variable;
    }
    m_numbersOfCurrentNames.append({ open, m_proc.addVariable(B3::Int64) });
    return m_numbersOfCurrentNames.last().second;
}

void ForInEmitter::setNumberOfCurrentNames(ValueID open, Integer number)
{
    m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), numberOfCurrentNamesOf(open), number.value());
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
    // The count is only written right after the open itself.
    const bool checksPresenceOnce = value->keysChangeOnlyInSteps() && m_function.at(value->enumeratorOperand()).opcode == Opcode::ForInOpen;
    LBasicBlock isOwnNameToCheck = checksPresenceOnce ? newBlock() : isStructureName;
    if (checksPresenceOnce) {
        LBasicBlock isNotKnownToBeCurrent = newBlock();
        Integer numberOfCurrentNames(m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), numberOfCurrentNamesOf(value->enumeratorOperand())));
        branch(below(nameIndex, numberOfCurrentNames), { normalEdge(isStructureName), normalEdge(isNotKnownToBeCurrent) });
        m_out.appendTo(isNotKnownToBeCurrent);
    }
    branch(isOwnName(fields, nameIndex), { checksPresenceOnce ? unlikelyEdge(isOwnNameToCheck) : normalEdge(isOwnNameToCheck), normalEdge(isNotStructureName) });

    m_out.appendTo(isOwnNameToCheck);
    takeSlowPathUnless(m_out, isCell(object), slowPath);
    takeSlowPathUnless(m_out, hasCachedKeys(enumerator, fields, unboxCell(object)), slowPath);
    if (checksPresenceOnce) {
        setNumberOfCurrentNames(value->enumeratorOperand(), zeroExtended(fields.endOwnPropertyIndex));
        m_out.jump(isStructureName);
        m_out.appendTo(isStructureName);
        if (generator().validatesOrChecksProofs())
            emitter<RuntimeValidationEmitter>().checkPrecondition(value, hasCachedKeys(enumerator, fields, unboxCell(object)));
    }
    Condition isNotDone = boolean(false);
    Pointer names = loadField<Pointer>(enumerator, m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVector);
    CellPointer name(m_out.loadPtr(m_out.baseIndex(m_heaps.JSPropertyNameEnumerator_cachedPropertyNamesVectorContents, names.value(), nameIndex.value())));
    // The collector reaches the names through the enumerator alone, and B3 may load `names` once in front of the loop and let the enumerator die.
    ensureStillAliveHere(enumerator);
    join.add(isNotDone, name, position);

    m_out.appendTo(isNotStructureName);
    branch(greaterThanOrEqual(nameIndex, zeroExtended(fields.endGenericPropertyIndex)), { normalEdge(isPastEnd), unlikelyEdge(slowPath) });

    m_out.appendTo(isPastEnd);
    Condition hasEnded = boolean(true);
    join.add(hasEnded, emptyString(), position);

    m_out.appendTo(slowPath);
    Operand arguments[] = { enumerator, object, position };
    StubCallResult next = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::EnumeratorNext, arguments);
    // It may have run program code.
    if (checksPresenceOnce)
        setNumberOfCurrentNames(value->enumeratorOperand(), integer(0));
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
