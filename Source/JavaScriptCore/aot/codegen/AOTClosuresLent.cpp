/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosuresLent.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTSweep.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(const Program& program, const Facts& facts, BuiltinID method)
{
    const BuiltinTable& table = program.builtins();
    if (!compiledMethodOfArray(method) || table.boxedCallStubOf(method))
        return false;
    bool namesFunction = false;
    for (const BuiltinOverload& overload : table.overloadsOf(method)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.libraryFunction());
        if (!held || !held->function || !facts.linked().onlyCallsWhatArrivesAt(held->function, program.at(held->function).firstParameterIndex()))
            return false;
        namesFunction = true;
    }
    return namesFunction;
}

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
    if (call.opcode != Opcode::CallDirect && call.opcode != Opcode::Call && call.opcode != Opcode::NewClosure)
        return { };
    const Terminator& end = code.at(made.block()).terminator;
    if (call.blockIndex != made.blockIndex || (end.kind == TerminatorKind::ReturnCall && end.callOperand() == user))
        return { };
    size_t numberOfPlaces = 0;
    if (call.opcode == Opcode::Call)
        numberOfPlaces = ClosureWord::numberOfBorrowedArguments;
    else if (call.function().index() < facts.linked().functions.size())
        numberOfPlaces = facts.linked().parametersOf(call.function()).size();
    if (!numberOfPlaces)
        return { };
    std::span<const ValueID> arguments = call.opcode == Opcode::NewClosure ? call.capturesOperands() : call.argumentsOperands();
    numberOfPlaces = std::min(numberOfPlaces, arguments.size());
    uint32_t position = 0;
    while (position < numberOfPlaces && arguments[position] != name)
        ++position;
    if (position == numberOfPlaces)
        return { };

    switch (call.opcode) {
    case Opcode::CallDirect:
        if (returnsResultOfFunction(call.role()) && !call.numberOfBoundArguments() && facts.linked().onlyCallsWhatArrivesAt(call.function(), position))
            return { LoanOfClosure::ToFunctionEntered, user };
        return { };
    case Opcode::Call:
        if (form != ClosureForm::Slim || position >= ClosureWord::numberOfBorrowedArguments || call.numberOfVariadicOperands() > numberOfArgumentGPRs)
            return { };
        if (BuiltinID method = call.methodOfReceiverAsMade(); method && (position || !stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(program, facts, method)))
            return { };
        return { LoanOfClosure::LeftToStubOfCall, user };
    case Opcode::NewClosure: {
        if (!facts.linked().onlyCallsWhatArrivesAt(call.function(), position))
            return { };
        ClosureLent holder = findLoanOfClosure(program, facts, code, user);
        if (holder.loan == LoanOfClosure::None)
            return { };
        return { LoanOfClosure::ToFunctionEntered, holder.call };
    }
    default:
        return { };
    }
}

uint8_t argumentsLentBy(const Program& program, const Facts& facts, const Code& code, const Operation& call)
{
    uint8_t lentArguments = 0;
    std::span<const ValueID> arguments = call.argumentsOperands();
    for (unsigned index = 0; index < arguments.size() && index < ClosureWord::numberOfBorrowedArguments; ++index) {
        if (!code.isConstant(arguments[index]) && findLoanOfClosure(program, facts, code, rootOfCellInSweptCode(code, arguments[index])).loan == LoanOfClosure::LeftToStubOfCall)
            lentArguments |= 1u << index;
    }
    return lentArguments;
}

bool mayCaptureClosuresOfFrames(const Facts& facts, const Code& code)
{
    return std::ranges::any_of(code.inlineFrames(), [&](const InlineFrame& frame) {
        return frame.callee && facts.linked().capturesWhatItOnlyCalls(frame.callee);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
