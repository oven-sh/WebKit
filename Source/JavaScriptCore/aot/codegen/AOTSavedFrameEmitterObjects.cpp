/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTGenerator.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTTypeTestEmitter.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

SavedFrameEmitter::SavedFrameEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

CellPointer SavedFrameEmitter::newGenerator(const ValueContext& value, CoroutineKind kind)
{
    Boxed callee = boxCell(in<CellPointer>(value->calleeOperand()));
    return callOperation<CellPointer>(value, RuntimeOperation::NewGenerator, { callee, word32(kind == CoroutineKind::AsyncGenerator) });
}

void SavedFrameEmitter::generateNewGenerator(const ValueContext& value)
{
    define(value, newGenerator(value, CoroutineKind::Generator));
}

void SavedFrameEmitter::generateNewAsyncGenerator(const ValueContext& value)
{
    define(value, newGenerator(value, CoroutineKind::AsyncGenerator));
}

void SavedFrameEmitter::generateReturnAsyncResult(const ValueContext& value)
{
    const Type& promiseType = typeOf(value->promiseOperand());
    Boxed promise = in<Boxed>(value->promiseOperand());
    Boxed result = in<Boxed>(value->valueOperand());

    Integer word;
    if (promiseType.hasOnlyPrimitives(Primitive::Undefined))
        word = integer(0);
    else if (!(promiseType.primitives & Primitive::Undefined))
        word = Integer(promise.value());
    else
        word = select(equal(promise, undefined()), integer(0), Integer(promise.value()));

    define(value, Boxed(callStub(value, returnAsyncResult(), { word.value(), result.value() }).first));
}

void SavedFrameEmitter::generatePromiseAllOfElements(const ValueContext& value)
{
    Vector<Boxed, 8> elements;
    for (ValueID element : value->elementsOperands())
        elements.append(in<Boxed>(element));
    Pointer buffer = argumentBuffer(elements.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::PromiseAll, { word32(static_cast<int32_t>(elements.size())), buffer }));
}

void SavedFrameEmitter::generateStepGenerator(const ValueContext& value)
{
    Boxed generator = in<Boxed>(value->generatorOperand());
    Boxed sentValue = in<Boxed>(value->valueOperand());
    LValue mode = m_out.constInt64(static_cast<int64_t>(value->resumeMode()));
    StubCallResult step = callStub(value, stepGenerator(value->check()), { generator.value(), mode, sentValue.value() });
    std::array<Operand, numberOfEnumerators<StepGeneratorResult>> results;
    results[static_cast<size_t>(StepGeneratorResult::Value)] = Boxed(step.first);
    results[static_cast<size_t>(StepGeneratorResult::Status)] = low32(Integer(step.second));
    defineResults(value, results);
}

void SavedFrameEmitter::branchUnlessUnmodifiedGenerator(Operand operand, const Type& type, LBasicBlock otherwise)
{
    if (!type.hasObjects()) {
        m_out.jump(otherwise);
        m_out.appendTo(newBlock());
        return;
    }

    CellPointer cell;
    switch (operand.representation) {
    case Representation::Cell:
        cell = operand.as<CellPointer>();
        break;
    case Representation::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        // The empty value has no tag bits, so it passes isCell.
        if (type.primitives & Primitive::Absent)
            branchUnless(isNotEmpty(boxed), otherwise);
        if (!type.isObject())
            branchUnless(isCell(boxed), otherwise);
        cell = unboxCell(boxed);
        break;
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        RELEASE_ASSERT_NOT_REACHED();
    }

    const ClassPart* classPart = type.singleBuiltinClass();
    if (!classPart || classPart->builtinClass != BuiltinClasses::Generator)
        branchUnless(emitter<TypeTestEmitter>().inheritsClassInfo(cell, BuiltinClasses::Generator), otherwise);

    // The stored prototype is only known to be a cell while the flag is clear, so test the flag first.
    Word32 hasOtherPrototype(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::PrototypeLinks), cell.value(), Generator::offsetOfHasOtherPrototype())));
    branchUnless(isZero(hasOtherPrototype), otherwise);
    branchUnless(Condition(m_out.isNull(m_out.loadPtr(cell.value(), m_heaps.JSObject_butterfly))), otherwise);
    CellPointer prototype(m_out.load64(loadStructure(cell).value(), m_heaps.Structure_prototype));
    branchUnless(equal(loadStructureID(prototype), loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.emptyGeneratorPrototypeStructureID))), otherwise);
}

void SavedFrameEmitter::generateIsUnmodifiedGenerator(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchUnlessUnmodifiedGenerator(get(operand), typeOf(operand), targets.notTaken.block());
        m_out.jump(targets.taken.block());
    });
}

void SavedFrameEmitter::generateIsUnmodifiedAsyncGenerator(const ValueContext& value)
{
    CellPointer generator = in<CellPointer>(value->valueOperand());
    const ClassPart* classPart = typeOf(value->valueOperand()).singleBuiltinClass();
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock otherwise = targets.notTaken.block();
        if (!classPart || classPart->builtinClass != BuiltinClasses::AsyncGenerator)
            branchUnless(equal(loadCellType(generator), word32(JSAsyncGeneratorType)), otherwise);
        branchUnless(Condition(m_out.isNull(m_out.loadPtr(generator.value(), m_heaps.JSObject_butterfly))), otherwise);
        Boxed prototype(m_out.load64(loadStructure(generator).value(), m_heaps.Structure_prototype));
        branchUnless(isCell(prototype), otherwise);
        branchUnless(equal(loadStructureID(unboxCell(prototype)), loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.emptyAsyncGeneratorPrototypeStructureID))), otherwise);
        m_out.jump(targets.taken.block());
    });
}

void SavedFrameEmitter::generateStepAsyncGenerator(const ValueContext& value)
{
    constexpr size_t numberOfResults = numberOfEnumerators<StepGeneratorResult>;
    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(numberOfResults);
    callVoidOperation(value, RuntimeOperation::StepAsyncGenerator, { in<CellPointer>(value->generatorOperand()), in<Boxed>(value->valueOperand()), buffer });
    auto resultAt = [&](StepGeneratorResult result) {
        return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), static_cast<size_t>(result) * sizeof(EncodedJSValue))));
    };
    std::array<Operand, numberOfResults> results;
    results[static_cast<size_t>(StepGeneratorResult::Value)] = resultAt(StepGeneratorResult::Value);
    results[static_cast<size_t>(StepGeneratorResult::Status)] = low32(resultAt(StepGeneratorResult::Status));
    defineResults(value, results);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
