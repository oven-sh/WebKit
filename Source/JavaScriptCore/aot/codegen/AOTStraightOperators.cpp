/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfOperators()
{
    return {
        Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow,
        Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
        Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Eq, Opcode::StrictEq,
        Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::BitNot, Opcode::ToNumeric,
        Opcode::ToBoolean, Opcode::LogicalNot, Opcode::TypeOf,
        Opcode::ToNumber, Opcode::InstanceOf, Opcode::ToString, Opcode::Strcat, Opcode::ToPropertyKey, Opcode::ToObject, Opcode::ToThis,
    };
}

static std::optional<BinaryOperator> binaryOperatorOfStraightForm(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
        return BinaryOperator::Add;
    case Opcode::Sub:
        return BinaryOperator::Sub;
    case Opcode::Mul:
        return BinaryOperator::Mul;
    case Opcode::Div:
        return BinaryOperator::Div;
    case Opcode::Mod:
        return BinaryOperator::Mod;
    case Opcode::BitAnd:
        return BinaryOperator::BitAnd;
    case Opcode::BitOr:
        return BinaryOperator::BitOr;
    case Opcode::BitXor:
        return BinaryOperator::BitXor;
    case Opcode::LShift:
        return BinaryOperator::LShift;
    case Opcode::RShift:
        return BinaryOperator::RShift;
    case Opcode::URShift:
        return BinaryOperator::URShift;
    case Opcode::Less:
        return BinaryOperator::Less;
    case Opcode::LessEq:
        return BinaryOperator::LessEq;
    case Opcode::Greater:
        return BinaryOperator::Greater;
    case Opcode::GreaterEq:
        return BinaryOperator::GreaterEq;
    default:
        return std::nullopt;
    }
}

static std::optional<UnaryOperationForm> unaryFormOfStraightForm(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Negate:
        return UnaryOperationForm::Negate;
    case Opcode::Increment:
        return UnaryOperationForm::Increment;
    case Opcode::Decrement:
        return UnaryOperationForm::Decrement;
    case Opcode::BitNot:
        return UnaryOperationForm::BitNot;
    case Opcode::ToNumeric:
        return UnaryOperationForm::ToNumeric;
    default:
        return std::nullopt;
    }
}

static constexpr OpcodeSet opcodesThatGiveStringsInStraightForm { Opcode::String, Opcode::ToString, Opcode::Strcat, Opcode::TypeOf };

static void generateStrcat(StraightEmitter& emitter, const BodyValue& value)
{
    std::span<const ValueID> parts = value.partsOperands();
    if (parts.empty()) {
        emitter.jit().loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(emptyString)), emitter.takeResultRegister());
        emitter.define(value);
        return;
    }
    if (parts.size() == 1) {
        emitter.callOperation(value, RuntimeOperation::ToString, { parts[0] });
        emitter.define(value);
        return;
    }

    bool everyPartIsString = std::ranges::all_of(parts, [&](ValueID part) {
        return opcodesThatGiveStringsInStraightForm.contains(emitter.index().opcodeOf(emitter.rootOf(part)));
    });
    if (!everyPartIsString) {
        for (size_t index = 0; index < parts.size(); ++index)
            emitter.storeInScratchArea(parts[index], static_cast<uint32_t>(index));
        emitter.callOperation(value, RuntimeOperation::ConcatenateValues, { StraightArgument::word(parts.size()), StraightArgument::addressInScratchArea(0) });
        emitter.define(value);
        return;
    }

    StraightArgument joined = parts[0];
    for (size_t next = 1; next < parts.size(); next += 2) {
        if (next + 1 < parts.size())
            emitter.callStub(value, AOT::concatenate(3), { joined, parts[next], parts[next + 1] });
        else
            emitter.callStub(value, AOT::concatenate(2), { joined, parts[next] });
        joined = StraightArgument::inRegister(emitter.resultOfCall());
    }
    emitter.define(value);
}

void StraightEmitter::generateOperator(const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (std::optional<BinaryOperator> binary = binaryOperatorOfStraightForm(opcode)) {
        callStub(value, binaryOperation(*binary, false), { value.leftOperand(), value.rightOperand() });
        if (isComparison(*binary))
            defineBoolean(value);
        else
            define(value);
        return;
    }
    if (std::optional<UnaryOperationForm> unary = unaryFormOfStraightForm(opcode)) {
        callStub(value, unaryOperation(*unary), { value.valueOperand() });
        define(value);
        return;
    }
    switch (opcode) {
    case Opcode::Pow:
        callOperation(value, RuntimeOperation::ValuePow, { value.leftOperand(), value.rightOperand() });
        define(value);
        return;
    case Opcode::Eq:
    case Opcode::StrictEq:
        callStub(value, AOT::equal(opcode == Opcode::StrictEq), { value.leftOperand(), value.rightOperand() });
        defineBoolean(value);
        return;
    case Opcode::ToBoolean:
        callStub(value, AOT::toBoolean(), { value.valueOperand() });
        defineBoolean(value);
        return;
    case Opcode::LogicalNot: {
        GPRReg result = loadIntoResultRegister(value.valueOperand());
        jit().xor32(CCallHelpers::TrustedImm32(1), result);
        define(value, result);
        return;
    }
    case Opcode::TypeOf:
        callStub(value, typeofString(), { value.valueOperand() });
        define(value);
        return;
    case Opcode::ToNumber:
        callStub(value, toNumberAsDouble(), { value.valueOperand() });
        defineDouble(value);
        return;
    case Opcode::InstanceOf:
        callStub(value, AOT::instanceOf(InstanceOfOperand::Constructor), { value.valueOperand(), value.constructorOperand() });
        defineBoolean(value);
        return;
    case Opcode::ToString:
        callOperation(value, RuntimeOperation::ToString, { value.valueOperand() });
        define(value);
        return;
    case Opcode::Strcat:
        generateStrcat(*this, value);
        return;
    case Opcode::ToPropertyKey:
        callStub(value, AOT::toPropertyKey(), { value.valueOperand() });
        define(value);
        return;
    case Opcode::ToObject: {
        StringID message = value.message() ? value.message() : program().strings.emptyString();
        callOperation(value, RuntimeOperation::ToObject, { value.valueOperand(), StraightArgument::number(mentions().add(NumberSpace::ShownString, message)) });
        define(value);
        return;
    }
    case Opcode::ToThis:
        callOperation(value, RuntimeOperation::ToThis, { value.valueOperand() });
        define(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
