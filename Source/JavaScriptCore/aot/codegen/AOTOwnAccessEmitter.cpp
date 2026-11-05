/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTRuntimeCallEmitter.h"
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
        CallOptions options { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::Name, key) } };
        return Condition(m_out.castToInt32(callStub(value, inById(PresenceQuestion::HasOwn), { object.value() }, options).first));
    };
    if (!value->receivers()) {
        defineTest(value, askRunTime());
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock asksRunTime = newBlock();
        emitter<GuardEmitter>().branchOnOwnPropertyByCell(value, targets, asksRunTime);
        m_out.appendTo(asksRunTime);
        branch(askRunTime(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
