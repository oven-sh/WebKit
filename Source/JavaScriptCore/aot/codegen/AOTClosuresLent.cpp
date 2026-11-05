/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosuresLent.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

ClosureLent findLoanOfClosure(const Program& program, const Facts& facts, const Code& code, ValueID closure)
{
    const Operation& made = code.at(closure);
    if (made.opcode != Opcode::NewClosure || made.storage() != AllocationStorage::Heap || !facts.mechanisms().contains(Mechanism::CellsInFrames))
        return { };
    ClosureForm form = made.closureForm();
    if (form != ClosureForm::Slim && form != ClosureForm::Full)
        return { };
    CodeUnit unit = code.unit();
    bool isCoroutine = program.at(unit.function).isCoroutine();
    if (isBodyOfCoroutine(unit.role, isCoroutine) || (isCoroutine && walksInputBody(unit.role)) || program.at(made.function()).calleeParameterIndex())
        return { };

    ValueID name = closure;
    ValueID user = code.onlyUserOf(name);
    while (user && opcodesThatGiveSameCell.contains(code.at(user).opcode)) {
        name = user;
        user = code.onlyUserOf(name);
    }
    if (!user)
        return { };
    const Operation& call = code.at(user);
    const Terminator& end = code.at(made.block()).terminator;
    if (call.blockIndex != made.blockIndex || (end.kind == TerminatorKind::ReturnCall && end.callOperand() == user))
        return { };
    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    uint32_t operandIndex = 0;
    while (call.operand(operandIndex) != name)
        ++operandIndex;
    if (operandIndex < numberOfNamedOperands)
        return { };
    uint32_t position = operandIndex - numberOfNamedOperands;

    switch (call.opcode) {
    case Opcode::CallDirect:
        if (returnsResultOfFunction(call.role()) && !call.numberOfBoundArguments() && facts.linked().onlyCallsWhatArrivesAt(call.function(), position))
            return { LoanOfClosure::ToFunctionEntered, user };
        return { };
    default:
        return { };
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
