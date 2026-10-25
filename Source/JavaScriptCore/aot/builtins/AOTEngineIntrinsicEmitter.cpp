/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEngineIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTRuntimeValidation.h"
#include "AOTSavedFrameEmitter.h"
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

void EngineIntrinsicEmitter::generateArraySpeciesCreate(const ValueContext& value)
{
    LBasicBlock invalid = newBlock();
    Boxed length = boxedOperand(value, value->lengthOperand());
    CellPointer array = objectOrElse(value->arrayOperand(), invalid);
    LBasicBlock continuation = m_out.m_block;

    m_out.appendTo(invalid);
    if (generator().validates())
        callVoidOperation(value, RuntimeOperation::ReportValidationFailure, { boxedOperand(value, value->arrayOperand()), word32(static_cast<int32_t>(ValidationFailureKind::ValueType)), word32(m_id.index()), word32(value.id().index()) }, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(continuation);
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::ArraySpeciesCreate, { length, array }), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateGetByValWithThis(const ValueContext& value)
{
    Boxed base = boxedOperand(value, value->objectOperand());
    Boxed thisValue = boxedOperand(value, value->thisValueOperand());
    Boxed key = boxedOperand(value, value->propertyOperand());
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::GetByValWithThis, { base, thisValue, key }), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateGetOwnProperty(const ValueContext& value)
{
    Boxed object = boxedOperand(value, value->objectOperand());
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::GetOwnProperty, { object, identifierNumber(value->key()) }), typeOf(value.id()));
}

void EngineIntrinsicEmitter::generateToLength(const ValueContext& value)
{
    Boxed operand = boxedOperand(value, value->valueOperand());
    defineAs(value, either<Boxed>(isInt32(operand), [&] {
        Word32 length = unboxInt32(operand);
        return boxInt32(select(lessThan(length, word32(0)), word32(0), length));
    }, [&] {
        return callOperation<Boxed>(value, RuntimeOperation::ToLength, { operand }, { .isOnRarePath = true });
    }), m_program.at(TypeTable::number));
}

void EngineIntrinsicEmitter::generateThrowNewError(const ValueContext& value)
{
    emitter<ThrowEmitter>().throwError(value, value->errorKind(), value->message());
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
