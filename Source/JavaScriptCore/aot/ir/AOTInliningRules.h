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

// Four flags of the summary of a function, under the names of SummaryFlag.
struct FlagsOfSummaryForInlining {
    bool suspends { false };
    bool hasDirectEval { false };
    bool holdsOperationsBoundToFrame { false };
    bool mayReturnBeforeFirstCall { false };
};

inline constexpr OptionSet<FunctionCapabilities> capabilitiesNeededToBeInlined { FunctionCapabilities::MayBeInlined, FunctionCapabilities::HasOrdinaryFrame };
inline constexpr OpcodeSet opcodesOfMappedArguments { Opcode::CreateMappedArguments, Opcode::GetMappedArgument, Opcode::SetMappedArgument };
// What SummaryFlag::HoldsOperationsBoundToFrame is made of. A catch stands at the head of its handler block, which is walked in the frame of the callee.
inline constexpr OpcodeSet opcodesThatBindBodyToFrame = opcodesWhere([](Opcode opcode) { return boundToOf(opcode) == BoundTo::Frame && !catchOpcodes.contains(opcode); });

// The flag is made of the operations of a body. It would miss a terminator.
static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return boundToOf(kind) == BoundTo::Frame; }));
static_assert(boundToOf(Opcode::Await) == BoundTo::Frame && boundToOf(Opcode::Yield) == BoundTo::Frame && boundToOf(Opcode::InitialYield) == BoundTo::Frame && boundToOf(Opcode::DirectEval) == BoundTo::Frame);
// A template object belongs to its place in the source, which every copy of the body shares. So what is bound to its site may be walked at any number of calls.
static_assert(boundToOf(Opcode::NewTemplateObject) == BoundTo::Site);

// The record of a coroutine lacks both capabilities for the sake of a frame that is saved and entered again (initialCapabilitiesOf()), and the reader cannot tell whether anything suspends.
inline bool isMarkedNeverInlined(const Program& program, const Function& function)
{
    if (!(function.isCoroutine() ? capabilitiesOf(function.role) : function.capabilities).containsAll(capabilitiesNeededToBeInlined))
        return true;
    RuntimeFunctionID enclosing = function.enclosingRuntimeFunction;
    return enclosing && program.builtins().at(enclosing).flags.contains(RuntimeFunctionFlag::IsNeverInlined);
}

// Why the part is walked in the place of no call, whoever calls: by the record, the summary and the index of the body alone. The walk and the expanded sizes of Facts both ask here.
// On top come a cycle of calls (FactCallGraph::isInCycleOfInlining()) and what the call and the walk that stands at it say (Walker::whyPartIsNotInlined()).
// Null: the function has no summary, or no index.
inline NotInlinedCause whyNoCallInlines(const Program& program, FunctionID id, PartOfCallee part, const FlagsOfSummaryForInlining* summary, const BodyIndex* body)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return NotInlinedCause::HasNoBody;
    if (function.isModuleBody())
        return NotInlinedCause::IsModuleBody;
    // A call of a coroutine runs its ramp. The body as written is what the ramp starts and what every resumption goes on in.
    // Where nothing suspends the body of an async function returns and throws as any other does, and is walked in the place of the call that starts it.
    if (function.isCoroutine() && (function.has(FunctionFlags::IsGenerator) || !summary || summary->suspends))
        return NotInlinedCause::IsCoroutine;
    if (isMarkedNeverInlined(program, function))
        return NotInlinedCause::MarkedNeverInlined;
    // Without a summary every flag of a body reads as clear, which says nothing of the body.
    if (!summary || !body)
        return NotInlinedCause::HasNoBody;
    if (summary->suspends)
        return NotInlinedCause::IsCoroutine;

    switch (part) {
    case PartOfCallee::WholeBody:
        ASSERT(summary->holdsOperationsBoundToFrame || !body->holdsAnyOf(opcodesThatBindBodyToFrame));
        if (summary->holdsOperationsBoundToFrame || summary->hasDirectEval || body->holdsAnyOf(opcodesOfMappedArguments))
            return NotInlinedCause::IsBoundToFrame;
        break;
    case PartOfCallee::Head:
        // What a coroutine returns settles its promise, which the call behind a head would make once more.
        if (function.isCoroutine())
            return NotInlinedCause::IsCoroutine;
        // Code that a direct eval runs finds the variables of the frame by name, those that the head has made among them.
        if (summary->hasDirectEval || function.results.size() > 1)
            return NotInlinedCause::IsBoundToFrame;
        if (!summary->mayReturnBeforeFirstCall)
            return NotInlinedCause::NoRoom;
        break;
    }
    // The index makes one loop of such a body, with the entry as its head. An inlined body goes on in the block of Code that holds the call, which is the head of no loop.
    if (body->hasIrreducibleFlow())
        return NotInlinedCause::IsBoundToFrame;
    return NotInlinedCause::None;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
