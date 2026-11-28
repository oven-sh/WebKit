/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

enum class NotInlinedCause : uint8_t { None, NoRoom, IsOnFrameStack, IsInCycleOfCalls, IsCoroutine, IsModuleBody, SeveralCallees, UnknownCallee, HasNoBody, IsBoundToFrame, MarkedNeverInlined };

enum class PartOfCallee : uint8_t { WholeBody, Head };

struct FlagsOfSummaryForInlining {
    bool suspends { false };
    bool hasDirectEval { false };
    bool holdsOperationsBoundToFrame { false };
    bool mayReturnBeforeFirstCall { false };
};

inline constexpr OptionSet<FunctionCapabilities> capabilitiesNeededToBeInlined { FunctionCapabilities::MayBeInlined, FunctionCapabilities::HasOrdinaryFrame };
inline constexpr OpcodeSet opcodesOfMappedArguments { Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument };
inline constexpr OpcodeSet opcodesThatReachParametersThroughArguments { Opcode::GetMappedArgument, Opcode::SetMappedArgument };
inline constexpr OpcodeSet opcodesThatBindBodyToFrame = opcodesWhere([](Opcode opcode) { return boundToOf(opcode) == BoundTo::Frame && !catchOpcodes.contains(opcode); });

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return boundToOf(kind) == BoundTo::Frame; }));
static_assert(boundToOf(Opcode::Await) == BoundTo::Frame && boundToOf(Opcode::Yield) == BoundTo::Frame && boundToOf(Opcode::InitialYield) == BoundTo::Frame && boundToOf(Opcode::DirectEval) == BoundTo::Frame);
static_assert(boundToOf(Opcode::NewTemplateObject) == BoundTo::Site);

inline bool isMarkedNeverInlined(const Program& program, const Function& function)
{
    if (!(function.isCoroutine() ? capabilitiesOf(function.role) : function.capabilities).containsAll(capabilitiesNeededToBeInlined))
        return true;
    RuntimeFunctionID enclosing = function.enclosingRuntimeFunction;
    return enclosing && program.builtins().at(enclosing).flags.contains(RuntimeFunctionFlag::IsNeverInlined);
}

inline NotInlinedCause whyNoCallInlines(const Program& program, FunctionID id, PartOfCallee part, const FlagsOfSummaryForInlining* summary, const BodyIndex* body)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return NotInlinedCause::HasNoBody;
    if (function.isModuleBody())
        return NotInlinedCause::IsModuleBody;
    if (function.isCoroutine() && (function.has(FunctionFlags::IsGenerator) || !summary || summary->suspends))
        return NotInlinedCause::IsCoroutine;
    if (isMarkedNeverInlined(program, function))
        return NotInlinedCause::MarkedNeverInlined;
    if (!summary || !body)
        return NotInlinedCause::HasNoBody;
    if (summary->suspends)
        return NotInlinedCause::IsCoroutine;

    switch (part) {
    case PartOfCallee::WholeBody:
        ASSERT(summary->holdsOperationsBoundToFrame || !body->holdsAnyOf(opcodesThatBindBodyToFrame));
        if (summary->holdsOperationsBoundToFrame || summary->hasDirectEval || body->holdsAnyOf(opcodesThatReachParametersThroughArguments))
            return NotInlinedCause::IsBoundToFrame;
        break;
    case PartOfCallee::Head:
        if (function.isCoroutine())
            return NotInlinedCause::IsCoroutine;
        if (summary->hasDirectEval || function.results.size() > 1)
            return NotInlinedCause::IsBoundToFrame;
        if (!summary->mayReturnBeforeFirstCall)
            return NotInlinedCause::NoRoom;
        break;
    }
    if (body->hasIrreducibleFlow())
        return NotInlinedCause::IsBoundToFrame;
    return NotInlinedCause::None;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
