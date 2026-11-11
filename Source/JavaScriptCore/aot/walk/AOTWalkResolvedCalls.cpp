/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkResolvedCalls.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"

namespace JSC { namespace AOT { namespace Typed {

bool callThroughTargetResolvedOnceOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (call.isDoneInPlace() || !walker.has(Mechanism::LoopInvariants))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop || call.calleeOperand().index() >= index.block(index.loop(loop).head).firstParameter().index())
        return false;

    Reference callee = walker.referenceBoundTo(call.calleeOperand());
    if (!callee || !callee.isRecord())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfCallee = walker.code().at(callee.id()).type;
    Type type = types.at(typeOfCallee);
    if (!type.hasObjects() || type.mayBeUnspecified() || (type.primitives & Primitive::Absent) || !hasKind(types, typeOfCallee, OperandKind::Any))
        return false;
    if ((type.isObject() && singleBuiltinOf(types, type)) || findOnlyCallee(walker, call, callee))
        return false;

    Draft asWritten = walker.draftOf(call);
    if (asWritten.isTailCall || asWritten.resultIsReturnedAtOnce || asWritten.expectedResult() || asWritten.origin().isTailCall)
        return false;

    Draft resolution(Opcode::ResolveCallTarget, asWritten.origin());
    walker.chooseConservativeFields(resolution);
    resolution.setCalleeOperand(callee);
    resolution.setArgumentCount(static_cast<uint32_t>(asWritten.argumentsOperands().size()));
    BlockID turn = walker.currentBlock();
    Reference target = walker.build(WTF::move(resolution));
    if (!target)
        return walker.isUnreachable();
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
