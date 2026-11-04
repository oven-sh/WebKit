/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LoopInvariants: whom a loop calls is asked once. Of a value that stays unknown and is the same in every turn the address of its code is resolved in front of the loop, and each turn jumps to the answer.
// At a suspension: only loops that never suspend are open, so the frame stands where the stack was tested for it. At a throw: what cannot be called throws in the turn, behind its arguments, as written.
// Withheld: each turn calls the value as it is written.

#include "config.h"
#include "AOTWalkResolvedCalls.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// The stub of a Call moves a closure of the frame that is lent to the callee to the heap for a callee that keeps it. No other stub does.
static bool lendsClosureOfFrameInResolvedCalls(const Code& code, const Draft& call)
{
    return std::ranges::any_of(call.operands(), [&](Reference operand) {
        if (!operand || !operand.isValue())
            return false;
        const Operation* made = &code.at(operand.id());
        if (made->opcode == Opcode::Box)
            made = &code.at(made->operand(0));
        return made->opcode == Opcode::NewClosure && made->isLent();
    });
}

// What the callee is, where its code lies and whether that takes the arguments as they are passed is the same in every turn.
bool callThroughTargetResolvedOnceOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (call.isDoneInPlace() || !walker.has(Mechanism::LoopInvariants))
        return false;
    // By the body as written, before anything is asked of what is built: the call stands in a loop, and what it calls is older than the head of that loop.
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop || call.calleeOperand().index() >= index.block(index.loop(loop).head).firstParameter().index())
        return false;

    // A test in the loop gives the callee a name of the loop. What is only known under such a name to be a value may be the mark of a dead zone in front of it.
    Reference callee = walker.referenceBoundTo(call.calleeOperand());
    if (!callee || !callee.isRecord())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfCallee = walker.code().at(callee.id()).type;
    Type type = types.at(typeOfCallee);
    if (!type.hasObjects() || type.mayBeUnspecified() || !hasKind(types, typeOfCallee, OperandKind::Any))
        return false;
    // replaceOperation() has a form of its own for a built-in, and a callee that is known is entered directly wherever that may be done.
    if ((type.isObject() && singleBuiltinOf(types, type)) || findOnlyCallee(walker, call, callee))
        return false;

    // No ReturnCall takes a CallResolved, and it has no word for an expected result.
    Draft asWritten = walker.draftOf(call);
    if (asWritten.isTailCall || asWritten.resultIsReturnedAtOnce || asWritten.expectedResult() || asWritten.origin().isTailCall)
        return false;
    if (lendsClosureOfFrameInResolvedCalls(walker.code(), asWritten))
        return false;

    // build() lays what no turn changes in front of the outermost open loop that its operands stand in front of: Walker::findLoopToBuildInFrontOf().
    Draft resolution(Opcode::ResolveCallTarget, asWritten.origin());
    walker.chooseConservativeFields(resolution);
    resolution.setCalleeOperand(callee);
    resolution.setArgumentCount(static_cast<uint32_t>(asWritten.argumentsOperands().size()));
    BlockID turn = walker.currentBlock();
    Reference target = walker.build(WTF::move(resolution));
    if (!target)
        return walker.isUnreachable();
    // The loop is not open, or the callee has a name of the loop: asked in every turn, the answer saves nothing. Nobody takes it, and the sweep drops it.
    if (!target.isValue() || walker.code().blockOf(target.id()) == turn)
        return false;

    Draft resolved(Opcode::CallResolved, asWritten.origin());
    walker.chooseConservativeFields(resolved);
    resolved.setCalleeOperand(asWritten.calleeOperand());
    resolved.setTargetOperand(target);
    resolved.setThisValueOperand(asWritten.thisValueOperand());
    resolved.setArgumentsOperands(asWritten.argumentsOperands());
    resolved.setCalleeKey(asWritten.calleeKey());
    resolved.setCalleeName(asWritten.calleeName());
    resolved.setCalledBuiltin(asWritten.calledBuiltin());
    resolved.expected = asWritten.expected;
    resolved.inputValueGiven = asWritten.inputValueGiven;
    if (Reference result = walker.build(WTF::move(resolved)))
        walker.bind(call.id(), result);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
