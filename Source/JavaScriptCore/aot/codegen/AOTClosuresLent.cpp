/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosuresLent.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTFacts.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

ValueID callThatBorrowsClosure(const Code& code, ValueID closure)
{
    const Operation& made = code.at(closure);
    if (made.opcode != Opcode::NewClosure || made.loan() == LoanOfClosure::None)
        return { };
    ValueID user = code.onlyUserOf(closure);
    while (user && (opcodesThatGiveSameCell.contains(code.at(user).opcode) || code.at(user).opcode == Opcode::NewClosure))
        user = code.onlyUserOf(user);
    RELEASE_ASSERT(user);
    return user;
}

bool isLentToFunctionsEnteredAlone(const Code& code, ValueID closure)
{
    while (true) {
        const Operation& made = code.at(closure);
        if (made.opcode != Opcode::NewClosure)
            return true;
        if (made.loan() != LoanOfClosure::ToFunctionEntered)
            return false;
        closure = code.onlyUserOf(closure);
        while (closure && opcodesThatGiveSameCell.contains(code.at(closure).opcode))
            closure = code.onlyUserOf(closure);
        RELEASE_ASSERT(closure);
    }
}

uint8_t argumentsLentBy(const Code& code, const Operation& call)
{
    uint8_t lentArguments = 0;
    std::span<const ValueID> arguments = call.argumentsOperands();
    for (unsigned index = 0; index < arguments.size() && index < ClosureWord::numberOfBorrowedArguments; ++index) {
        if (code.isConstant(arguments[index]))
            continue;
        const Operation& made = code.at(rootOfCellInSweptCode(code, arguments[index]));
        if (made.opcode == Opcode::NewClosure && made.loan() == LoanOfClosure::LeftToStubOfCall)
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
