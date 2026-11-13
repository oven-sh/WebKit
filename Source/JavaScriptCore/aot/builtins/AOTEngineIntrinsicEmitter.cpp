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
    CellPointer array = objectOrElse(value->arrayOperand(), invalid);
    LBasicBlock continuation = m_out.m_block;

    m_out.appendTo(invalid);
    m_out.unreachable();

    m_out.appendTo(continuation);
    Operand lengthAsHanded = get(value->lengthOperand());
    if (!lengthAsHanded.is<Number>()) {
        defineAs(value, callOperation<Boxed>(value, RuntimeOperation::ArraySpeciesCreate, { boxedOperand(value, value->lengthOperand()), array }), typeOf(value.id()));
        return;
    }

    LBasicBlock asksRunTime = newBlock();
    Join<Boxed> join(m_out);
    auto wholeNumberBehind = [](Number number) -> LValue {
        return number.value()->opcode() == Machine::IToD && number.value()->child(0)->type() == Machine::Int64 ? number.value()->child(0) : nullptr;
    };
    Number number = lengthAsHanded.as<Number>();
    LValue whole = wholeNumberBehind(number);
    Integer count = whole ? Integer(whole) : emitter<ConversionEmitter>().toInteger(number);
    Condition hasRoomInLine = belowOrEqual(count, integer(maxHoleArrayCapacity));
    branchUnless(whole ? hasRoomInLine : logicalAnd(hasRoomInLine, equal(toNumber(count), number)), asksRunTime);
    branchUnless(logicalAnd(hasCellType(array, ArrayType), emitter<ElementAccessEmitter>().isPristineArray(array, { PristineAspects::Storage, PristineAspects::Names })), asksRunTime);
    branchUnless(emitter<GuardEmitter>().belongsToInstance(array), asksRunTime);
    branchUnless(emitter<GuardEmitter>().realmWatchesSpeciesOfLists(), asksRunTime);
    bool isEmpty = count.value()->hasInt64() && !count.value()->asInt64();
    ArrayAllocationEmitter allocation(generator());
    std::optional<Integer> roomOfEmptyList;
    if (Operand room = value->capacityOperand() ? get(value->capacityOperand()) : Operand(); isEmpty && value->capacityOperand() && room.is<Number>())
        roomOfEmptyList = wholeNumberBehind(room.as<Number>()) ? Integer(wholeNumberBehind(room.as<Number>())) : emitter<ConversionEmitter>().toInteger(room.as<Number>());
    allocation.allocateListLike(value, array, count, roomOfEmptyList, std::nullopt, asksRunTime, scopedLambda<void(CellPointer)>([&](CellPointer list) {
        join.add(Boxed(list.value()));
    }), value->likeOperand() && typeOf(value->likeOperand()).isUndefinedOrNull() ? &arrayStorageFormOf(ArrayStorage::Int32) : nullptr);

    m_out.appendTo(asksRunTime);
    join.add(callOperation<Boxed>(value, RuntimeOperation::ArraySpeciesCreate, { boxedOperand(value, value->lengthOperand()), array }));
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

void EngineIntrinsicEmitter::generateStepMatchIterator(const ValueContext& value)
{
    Boxed iterator = boxedOperand(value, value->iteratorOperand());
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::StepMatchIterator, { iterator }), typeOf(value.id()));
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
