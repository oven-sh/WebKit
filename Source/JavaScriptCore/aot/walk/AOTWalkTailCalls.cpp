/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function walk(n, acc) { if (!n) return acc; return walk(n.next, acc + n.size) }`: a call of the function by itself in tail position is a jump to a head behind the entry of its Body. A turn is an entry behind the guard.
// At a suspension: a function that suspends has no head.
// At a throw: a call under a handler does not come round. The poll at the head throws where the entry block throws.
// Withheld, with DirectCalls: no head is opened, and every ReturnCall says Required.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTailCallPlan.h"
#include "AOTThinSummary.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- Who may keep his frame

static bool calleeComesBackInTailCalls(const Walker& walker, FunctionID callee)
{
    return !walker.program().at(callee).hasBody() || walker.plan().endsItsTailCalls(callee);
}

bool tailCallMayKeepFrame(const Walker& walker, const BodyValue& call, FunctionID calleeOfDirectCall)
{
    if (calleeOfDirectCall)
        return calleeComesBackInTailCalls(walker, calleeOfDirectCall);
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        return calleeComesBackInTailCalls(walker, listed);

    if (call.opcode() != Opcode::Call && call.opcode() != Opcode::CallList)
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven || !walker.types().at(proven).isObject())
        return false;
    BuiltinID builtin = singleBuiltinOf(walker.types(), walker.types().at(proven));
    return builtin && callsOfBuiltinComeBack(walker.program(), builtin);
}

// A callee that ends its own tail calls costs one frame where the caller keeps its own. Whatever is entered by value may go on without end.
TailCallRequirement requirementOfTailCall(const Walker& walker, Reference call)
{
    if (!call || !call.isValue())
        return TailCallRequirement::Required;
    const Operation& record = walker.code().at(call.id());
    if (record.opcode != Opcode::CallDirect)
        return TailCallRequirement::Required;
    return calleeComesBackInTailCalls(walker, record.function()) ? TailCallRequirement::Allowed : TailCallRequirement::Required;
}

// `return node.visit(x)`: each arm of a guess at the receiver ends in a return of its own, so nothing meets behind the call.
bool readIsForCallReturnedAtOnce(const Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(read.id());
    if (!user || index.opcodeOf(user) != Opcode::Call || index.value(user).as<Opcode::Call>().calleeOperand() != read.id())
        return false;
    BlockID block = index.blockOf(user);
    BodyTerminator end = index.terminator(block);
    return block == index.blockOf(read.id()) && end.kind() == TerminatorKind::ReturnCall && end.operand(0) == user;
}

// ---- The head

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
