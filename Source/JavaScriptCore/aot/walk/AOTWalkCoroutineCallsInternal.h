/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkCoroutineCalls.h"
#include "AOTWalkCoroutineState.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct KeptByCallsOfCoroutines final : KeptForUnitOfCoroutines {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptByCallsOfCoroutines);

    struct AwaitOfCallThatMayBeCopied {
        FunctionID callee;
        ValueID await;
        Reference driver;
        Vector<Reference, 4> held;
    };
    std::optional<AwaitOfCallThatMayBeCopied> awaitOfCallThatMayBeCopied;
    Vector<ValueID, 2> awaitsTakenInCopies;


    bool isTakingListOfPromiseAllApart { false };
};

KeptByCallsOfCoroutines* findKeptByCallsOfCoroutines(Walker&);
KeptByCallsOfCoroutines& keptByCallsOfCoroutines(Walker&);

bool buildRampAtCall(Walker&, const BodyValue& call);

struct AwaitingBodyOfCoroutine {
    CoroutineKind kind;
    Reference driver;
};
std::optional<AwaitingBodyOfCoroutine> findAwaitingBodyOfCoroutine(Walker&);

Draft draftOfCoroutineCalls(Walker&, Opcode, Origin);
Reference undefinedOfCoroutineCalls(Walker&);
Reference buildNewPromiseOfCoroutineCalls(Walker&, Origin);
bool buildSettlementOfCoroutineCalls(Walker&, Opcode, Reference promise, Reference value, Origin);
Reference proveOrTrapInCoroutineCalls(Walker&, Reference, TypeID promised, Origin);
BlockID standUnderHandlerOfCoroutineCalls(Walker&);
Reference catchInHandlerOfCoroutineCalls(Walker&, BlockID handler, Origin);
bool isCellOfFrameInCoroutineCalls(const Walker&, Reference);
bool writtenOperandsMayStartBodyOfCoroutine(Walker&, const Draft& written);
void leaveAnythingForSwallowedOperation(Walker&, ValueID);
ValueID followCastsToOnlyTaker(const BodyIndex&, ValueID& name);

struct WaitOfCallInCoroutine {
    ValueID await;
    uint32_t placeOfCall { 0 };
    uint32_t endOfWait { 0 };
};
std::optional<WaitOfCallInCoroutine> findWaitOfCallInCoroutine(Walker&, const BodyIndex&, BodyBlock, ValueID call);
bool walkUpToAwaitOfCall(Walker&, const BodyIndex&, BodyBlock, const WaitOfCallInCoroutine&, Vector<Reference, 4>& held);
void finishWaitOfCall(Walker&, BodyBlock, const WaitOfCallInCoroutine&, Reference given);

bool buildAwaitedCall(Walker&, const BodyValue& call);

bool nameCastInFrontOfAwaitTakenInCopy(Walker&, const BodyValue& cast);

bool buildStepWrittenByHand(Walker&, const BodyValue& call);
bool buildAwaitedStepWrittenByHand(Walker&, const BodyValue& call);
bool buildPromiseAllOfElements(Walker&, const BodyValue& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
