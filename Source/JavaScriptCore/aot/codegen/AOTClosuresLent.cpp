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
    while (user && (cellPreservingOpcodes.contains(code.at(user).opcode) || code.at(user).opcode == Opcode::NewClosure))
        user = code.onlyUserOf(user);
    RELEASE_ASSERT(user);
    return user;
}

ValueID callThatBorrowsObject(const Code& code, ValueID object)
{
    const Operation& made = code.at(object);
    if (made.opcode != Opcode::NewObject || made.storage() != AllocationStorage::Frame)
        return { };
    ValueID user = code.onlyUserOf(object);
    while (user && cellPreservingOpcodes.contains(code.at(user).opcode))
        user = code.onlyUserOf(user);
    return user && code.at(user).opcode == Opcode::CallDirect && code.at(user).blockIndex == made.blockIndex ? user : ValueID();
}

static bool holdsLentClosure(const Code& code, const Operation& holder)
{
    return std::ranges::any_of(holder.capturesOperands(), [&](ValueID capture) {
        if (!capture || code.isConstant(capture))
            return false;
        const Operation& held = code.at(underlyingCellOf(code, capture));
        return held.opcode == Opcode::NewClosure && held.loan() != LoanOfClosure::None;
    });
}

bool lentClosureMayStandInHeap(const Code& code, ValueID closure)
{
    while (true) {
        const Operation& made = code.at(closure);
        if (made.opcode != Opcode::NewClosure)
            return true;
        if (made.loan() == LoanOfClosure::LeftToStubOfCall)
            return !holdsLentClosure(code, made);
        if (made.loan() != LoanOfClosure::ToFunctionEntered)
            return false;
        closure = code.onlyUserOf(closure);
        while (closure && cellPreservingOpcodes.contains(code.at(closure).opcode))
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
        const Operation& made = code.at(underlyingCellOf(code, arguments[index]));
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
