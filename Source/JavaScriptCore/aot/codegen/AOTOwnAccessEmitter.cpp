/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStaleAnswers.h"
#include "AOTStubCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeIDs.h"

namespace JSC { namespace AOT { namespace Typed {

void OwnAccessEmitter::generatePropHasOwn(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    PropertyKeyID key = value->key();
    if (liesInThread(key)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    auto askRunTime = [&] {
        AccessEmitter& accesses = emitter<AccessEmitter>();
        if (accesses.isSymbolOfRealm(key) || value->receivers())
            return Condition(m_out.castToInt32(callStub(value, inByValCached(PresenceQuestion::HasOwn), { object.value(), accesses.cellOfKey(value, key).value() }).first));
        Condition hasOwn = accesses.findsName(value, object, key, PresenceQuestion::HasOwn);
        if (accesses.verifiesFastPathAnswers(key)) [[unlikely]]
            accesses.verifyFastPathAnswer(value, object, key, boxBoolean(hasOwn), StubThatAnswers::PresenceOfOwnName, accesses.slowPathReadCount());
        return hasOwn;
    };
    if (!value->receivers()) {
        defineTest(value, askRunTime());
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock asksRunTime = newBlock();
        if (emitter<AccessEmitter>().verifiesFastPathAnswers(key) && m_program.at(key).kind != PropertyKeyKind::PrivateName) [[unlikely]] {
            LBasicBlock saysYes = newBlock();
            LBasicBlock saysNo = newBlock();
            emitter<GuardEmitter>().branchOnOwnPropertyByCell(value, { normalEdge(saysYes), normalEdge(saysNo) }, asksRunTime);
            int32_t kindAndStub = static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Value, StubThatAnswers::TypedPresenceOfOwnName));
            for (bool answer : { true, false }) {
                m_out.appendTo(answer ? saysYes : saysNo);
                callVoidOperation(value, RuntimeOperation::StopAtStaleAnswer, { object, boxBoolean(boolean(answer)), identifierNumber(key), word32(kindAndStub) });
                m_out.jump(answer ? targets.taken : targets.notTaken);
            }
        } else
            emitter<GuardEmitter>().branchOnOwnPropertyByCell(value, targets, asksRunTime);
        m_out.appendTo(asksRunTime);
        branch(askRunTime(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
