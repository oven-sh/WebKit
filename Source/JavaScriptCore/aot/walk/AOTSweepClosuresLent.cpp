/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTStubsCompiledMethods.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(const Program& program, const Facts& facts, BuiltinID method)
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

static LoanOfClosure findLoanOfSweptClosure(const SweepInputs& inputs, const Code& code, ValueID closure)
{
    const Program& program = inputs.program;
    const LinkTables& linked = inputs.facts.linked();
    const Operation& made = code.at(closure);
    ClosureForm form = made.closureForm();
    if (form != ClosureForm::Slim && form != ClosureForm::Full)
        return LoanOfClosure::None;
    if (program.at(made.function()).calleeParameterIndex())
        return LoanOfClosure::None;

    ValueID name = closure;
    ValueID user = code.onlyUserOf(name);
    while (user && opcodesThatGiveSameCell.contains(code.at(user).opcode)) {
        name = user;
        user = code.onlyUserOf(name);
    }
    if (!user)
        return LoanOfClosure::None;
    const Operation& call = code.at(user);
    if (call.opcode != Opcode::CallDirect && call.opcode != Opcode::Call && call.opcode != Opcode::NewClosure)
        return LoanOfClosure::None;
    const Terminator& end = code.at(made.block()).terminator;
    if (call.blockIndex != made.blockIndex || (end.kind == TerminatorKind::ReturnCall && end.callOperand() == user))
        return LoanOfClosure::None;
    size_t numberOfPlaces = 0;
    if (call.opcode == Opcode::Call)
        numberOfPlaces = ClosureWord::numberOfBorrowedArguments;
    else if (call.function().index() < linked.functions.size())
        numberOfPlaces = linked.parametersOf(call.function()).size();
    if (!numberOfPlaces)
        return LoanOfClosure::None;
    std::span<const ValueID> arguments = call.opcode == Opcode::NewClosure ? call.capturesOperands() : call.argumentsOperands();
    numberOfPlaces = std::min(numberOfPlaces, arguments.size());
    uint32_t position = 0;
    while (position < numberOfPlaces && arguments[position] != name)
        ++position;
    if (position == numberOfPlaces)
        return LoanOfClosure::None;

    switch (call.opcode) {
    case Opcode::CallDirect:
        if (returnsResultOfFunction(call.role()) && !call.numberOfBoundArguments() && linked.onlyCallsWhatArrivesAt(call.function(), position))
            return LoanOfClosure::ToFunctionEntered;
        return LoanOfClosure::None;
    case Opcode::Call:
        if (form != ClosureForm::Slim || position >= ClosureWord::numberOfBorrowedArguments || call.numberOfVariadicOperands() > numberOfArgumentGPRs)
            return LoanOfClosure::None;
        if (BuiltinID method = call.methodOfReceiverAsMade(); method && (position || !stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(program, inputs.facts, method)))
            return LoanOfClosure::None;
        return LoanOfClosure::LeftToStubOfCall;
    case Opcode::NewClosure:
        if (!linked.onlyCallsWhatArrivesAt(call.function(), position) || findLoanOfSweptClosure(inputs, code, user) == LoanOfClosure::None)
            return LoanOfClosure::None;
        return LoanOfClosure::ToFunctionEntered;
    default:
        return LoanOfClosure::None;
    }
}

bool Sweep::beginLendingClosures()
{
    const Code& code = *m_code;
    m_closuresThatMayBeLent.shrink(0);
    if (!code.opcodesOfValues().contains(Opcode::NewClosure) || !m_inputs->facts.mechanisms().contains(Mechanism::CellsInFrames))
        return false;
    CodeUnit unit = code.unit();
    bool isCoroutine = m_inputs->program.at(unit.function).isCoroutine();
    return !isBodyOfCoroutine(unit.role, isCoroutine) && !(isCoroutine && walksInputBody(unit.role));
}

bool Sweep::noteIfClosureMayBeLent(ValueID closure)
{
    const Code& code = *m_code;
    if (code.at(closure).storage() != AllocationStorage::Heap || findLoanOfSweptClosure(*m_inputs, code, closure) == LoanOfClosure::None)
        return false;
    m_closuresThatMayBeLent.append(closure);
    return true;
}

void Sweep::lendClosures()
{
    Code& code = *m_code;
    for (ValueID closure : m_closuresThatMayBeLent) {
        ++m_totals.operationsSwept;
        if (code.at(closure).storage() != AllocationStorage::Heap)
            continue;
        LoanOfClosure loan = findLoanOfSweptClosure(*m_inputs, code, closure);
        if (loan == LoanOfClosure::None) {
            m_coverageIsOutOfDate = true;
            continue;
        }
        code.layClosureInFrame(closure, loan);
    }
    m_lendsClosures = false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
