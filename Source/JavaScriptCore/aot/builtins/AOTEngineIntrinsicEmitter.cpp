/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEngineIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThrowEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

EngineIntrinsicEmitter::EngineIntrinsicEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void EngineIntrinsicEmitter::generateIsConstructor(const ValueContext& value)
{
    Boxed operand = boxedOperand(value, value->valueOperand());
    defineAs(value, callWithoutSite<Condition>(RuntimeOperation::IsConstructor, { operand }), m_program.at(TypeTable::boolean));
}

void EngineIntrinsicEmitter::generateReadsIteratorOfListUnseen(const ValueContext& value)
{
    Boxed operand = boxedOperand(value, value->valueOperand());
    defineAs(value, callWithoutSite<Condition>(RuntimeOperation::ReadsIteratorOfListUnseen, { operand }), m_program.at(TypeTable::boolean));
}

void EngineIntrinsicEmitter::generateArraySpeciesCreate(const ValueContext& value)
{
    LBasicBlock invalid = newBlock();
    Boxed length = boxedOperand(value, value->lengthOperand());
    CellPointer array = objectOrElse(value->arrayOperand(), invalid);
    LBasicBlock continuation = m_out.m_block;

    m_out.appendTo(invalid);
    m_out.unreachable();

    m_out.appendTo(continuation);
    Operand lengthAsHanded = get(value->lengthOperand());
    if (!lengthAsHanded.is<Number>()) {
        defineAs(value, callOperation<Boxed>(value, RuntimeOperation::ArraySpeciesCreate, { length, array }), typeOf(value.id()));
        return;
    }

    // A list as its class makes it has no `constructor` of its own and stands below the Array.prototype of its realm, so while the realm watches the species of lists the answer is a new list: speciesConstructArray().
    // Most lists that map() and filter() are asked for hold one or two, and the call into the run time costs more than all that is done with them.
    LBasicBlock asksRunTime = newBlock();
    Join<Boxed> join(m_out);
    Number number = lengthAsHanded.as<Number>();
    Integer count = emitter<ConversionEmitter>().toInteger(number);
    branchUnless(logicalAnd(belowOrEqual(count, integer(maxHoleArrayCapacity)), equal(toNumber(count), number)), asksRunTime);
    branchUnless(logicalAnd(hasCellType(array, ArrayType), emitter<ElementAccessEmitter>().isPristineArray(array, { PristineAspects::Storage, PristineAspects::Names })), asksRunTime);
    branchUnless(emitter<GuardEmitter>().belongsToInstance(array), asksRunTime);
    branchUnless(emitter<GuardEmitter>().realmWatchesSpeciesOfLists(), asksRunTime);
    // `list.filter(..)`: the text says how many the new list can come to hold. Most lists that are asked hold one or two, so it has room for as many, and for no more than an empty list has anyway.
    bool isEmpty = count.value()->hasInt64() && !count.value()->asInt64();
    ArrayAllocationEmitter allocation(generator());
    const ArrayStorageForm& form = arrayStorageFormOf(ArrayStorage::Contiguous);
    CellPointer list = CellPointer(m_out.int64Zero);
    if (Operand room = value->roomOperand() ? get(value->roomOperand()) : Operand(); isEmpty && value->roomOperand() && room.is<Number>()) {
        Integer most = integer(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY);
        Integer asked = emitter<ConversionEmitter>().toInteger(room.as<Number>());
        list = allocation.allocateEmptyArrayWithRoom(value, select(below(asked, most), asked, most), form, asksRunTime);
    } else
        list = allocation.allocateArrayOfHoles(value, count, form, asksRunTime);
    join.add(Boxed(list.value()));

    m_out.appendTo(asksRunTime);
    join.add(callOperation<Boxed>(value, RuntimeOperation::ArraySpeciesCreate, { length, array }));
    defineAs(value, join.finish(), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateGetByValWithThis(const ValueContext& value)
{
    Boxed base = boxedOperand(value, value->objectOperand());
    Boxed thisValue = boxedOperand(value, value->thisValueOperand());
    Boxed key = boxedOperand(value, value->propertyOperand());
    defineAs(value, Boxed(callStub(value, getSuperByVal(), { base.value(), key.value(), thisValue.value() }).first), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateGetOwnProperty(const ValueContext& value)
{
    Boxed object = boxedOperand(value, value->objectOperand());
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::GetOwnProperty, { object, identifierNumber(value->key()) }), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateToLength(const ValueContext& value)
{
    Boxed operand = boxedOperand(value, value->valueOperand());
    // Each arm comes in the representation of the result: what was found to be an int32 is not boxed to be asked again what it is.
    define(value, eitherOperand(isInt32(operand), value.representation(), [&] {
        Word32 length = unboxInt32(operand);
        return inRepresentation(value, value.representation(), select(lessThan(length, 0), 0, length), types().at(types().integer(Range { 0, std::numeric_limits<int32_t>::max() })));
    }, [&] {
        return inRepresentation(value, value.representation(), callOperation<Boxed>(value, RuntimeOperation::ToLength, { operand }, { .isOnRarePath = true }), m_program.at(TypeTable::number));
    }));
}

void EngineIntrinsicEmitter::generateThrowNewError(const ValueContext& value)
{
    emitter<ThrowEmitter>().throwError(value, value->errorKind(), value->message(), value->throwPosition());
}

void EngineIntrinsicEmitter::generateResumeGenerator(const ValueContext& value)
{
    Boxed generatorObject = boxedOperand(value, value->generatorOperand());
    Boxed sentValue = boxedOperand(value, value->valueOperand());
    Boxed iteratorResult = callOperation<Boxed>(value, RuntimeOperation::ResumeGenerator, { generatorObject, word32(static_cast<int32_t>(value->resumeMode())), sentValue });
    defineAs(value, iteratorResult, typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateStepGenerator(const ValueContext& value)
{
    emitter<SavedFrameEmitter>().generateStepGenerator(value);
}

void EngineIntrinsicEmitter::generateIsGeneratorSuspendedAtStart(const ValueContext& value)
{
    LBasicBlock isNotObject = newBlock();
    Join<Condition> join(m_out);
    CellPointer cell = objectOrElse(value->generatorOperand(), isNotObject);
    join.add(callOperation<Condition>(value, RuntimeOperation::IsGeneratorSuspendedAtStart, { cell }));
    m_out.appendTo(isNotObject);
    join.add(boolean(false));
    defineAs(value, join.finish(), m_program.at(TypeTable::boolean));
}

void EngineIntrinsicEmitter::generateIsUnmodifiedGenerator(const ValueContext& value)
{
    emitter<SavedFrameEmitter>().generateIsUnmodifiedGenerator(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
