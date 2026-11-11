/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfArrays()
{
    return {
        Opcode::NewArray, Opcode::AppendElement, Opcode::AppendSpread, Opcode::AppendHole,
        Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument,
    };
}

static void storeListOfStraightArrays(StraightEmitter& emitter, std::span<const ValueID> values)
{
    for (size_t i = 0; i < values.size(); ++i)
        emitter.storeInScratchArea(values[i], static_cast<uint32_t>(i));
}

static void generateNewArray(StraightEmitter& emitter, const BodyValue& value)
{
    std::span<const ValueID> elements = value.elementsOperands();
    if (!value.holes().empty() || elements.size() > maxValuesInArrayLiteral) {
        emitter.fail(value);
        return;
    }
    if (elements.size() <= maxArrayElementsInRegisters) {
        Vector<StraightArgument, maxArrayElementsInRegisters> operands;
        for (ValueID element : elements)
            operands.append(StraightArgument(element));
        emitter.callStub(value.origin(), arrayLiteral(static_cast<unsigned>(elements.size())), operands.span(), { });
    } else {
        storeListOfStraightArrays(emitter, elements);
        emitter.callOperation(value, RuntimeOperation::NewArray, { StraightArgument::word(elements.size()), StraightArgument::addressInScratchArea(0) });
    }
    emitter.define(value);
}

static void generateAppendSpread(StraightEmitter& emitter, const BodyValue& value)
{
    Origin origin = value.origin();
    origin.isTailCall = false;
    emitter.callOperation(value, RuntimeOperation::AppendIterable, { value.arrayOperand(), value.iterableOperand() }, { .origin = origin });
}

static void loadCountOfArguments(StraightEmitter& emitter, ValueID count, GPRReg destination)
{
    CCallHelpers& jit = emitter.jit();
    emitter.load(count, destination);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(destination);
    FPRReg number = emitter.takeResultFPR();
    jit.unboxDoubleWithoutAssertions(destination, destination, number);
    jit.truncateDoubleToUint32(number, destination);
    isInt32.link(&jit);
    jit.zeroExtend32ToWord(destination, destination);
}

static void generateCreateArguments(StraightEmitter& emitter, const BodyValue& value)
{
    bool isMapped = value.opcode() == Opcode::CreateMappedArguments;
    StubKey stub = isMapped ? createMappedArguments() : createArguments();
    std::span<const ValueID> parameters = value.parametersOperands();
    storeListOfStraightArrays(emitter, parameters);

    GPRReg count = emitter.catalog().summaryOf(stub).signature.operands[isMapped ? 1 : 0].gpr();
    loadCountOfArguments(emitter, value.countOperand(), count);
    StraightArgument countInRegister = StraightArgument::inRegister(count);
    StraightArgument addressOfParameters = StraightArgument::addressInScratchArea(0);
    if (isMapped)
        emitter.callStub(value, stub, { value.calleeOperand(), countInRegister, value.restOperand(), addressOfParameters, StraightArgument::word(parameters.size()) });
    else
        emitter.callStub(value, stub, { countInRegister, value.restOperand(), addressOfParameters, StraightArgument::word(parameters.size()) });
    emitter.define(value);
}

void StraightEmitter::generateArray(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::NewArray:
        generateNewArray(*this, value);
        return;
    case Opcode::AppendElement:
        callStub(value, arrayPush(), { value.arrayOperand(), value.valueOperand() });
        return;
    case Opcode::AppendSpread:
        generateAppendSpread(*this, value);
        return;
    case Opcode::AppendHole:
        callOperation(value, RuntimeOperation::AppendHole, { value.arrayOperand() });
        return;
    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments:
        generateCreateArguments(*this, value);
        return;
    case Opcode::GetMappedArgument:
        callOperation(value, RuntimeOperation::GetMappedArgument, { value.argumentsOperand(), StraightArgument::word(value.index()) });
        define(value);
        return;
    case Opcode::SetMappedArgument:
        callOperation(value, RuntimeOperation::SetMappedArgument, { value.argumentsOperand(), StraightArgument::word(value.index()), value.valueOperand() });
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
