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

// What the six files of AOTWalkCoroutine{Calls, AwaitedCalls, AwaitsAtJoins, StepsByHand, PromiseAll, Guards}.cpp share, and nobody else reads.

// CoroutineStateOfUnit::keptByCalls.
struct KeptByCallsOfCoroutines final : KeptForUnitOfCoroutines {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptByCallsOfCoroutines);

    // C3. Of the Call of the unit's own body that is in hand: the Await that alone takes what it gives, with casts alone between the two. It ends with the operation.
    struct AwaitOfCallThatMayBeCopied {
        FunctionID callee;
        ValueID await;
        Reference driver;
        Vector<Reference, 4> held;
    };
    std::optional<AwaitOfCallThatMayBeCopied> awaitOfCallThatMayBeCopied;
    // A call in a copy has awaited for them. Each goes when the walk comes to it.
    Vector<ValueID, 2> awaitsTakenInCopies;

    // C5. By block of the unit's own body.
    BitVector blocksAskedForAwaitTakenByEdges;
    Vector<ValueID> awaitsTakenByEdges;
    bool someAwaitIsTakenByEdges { false };
    // What a jump brings is bound to what the await has given already: the call that made it went on to the await. Each goes when the walk comes to the jump.
    Vector<ValueID, 2> broughtValuesAwaited;

    // C7. The operations between a listed call and Promise.all are walked one at a time, and none of them begins the same again.
    bool isTakingListOfPromiseAllApart { false };
};

KeptByCallsOfCoroutines* findKeptByCallsOfCoroutines(Walker&);
KeptByCallsOfCoroutines& keptByCallsOfCoroutines(Walker&);

// ---- AOTWalkCoroutineCalls.cpp

bool buildRampAtCall(Walker&, const BodyValue& call);

// The body of an async function, and no body of a module, where the walk stands in it: an await may be built ahead of its place.
// The body of an async generator stores what lives on at its suspensions as they are written (HeapWordPlan of coroutines-5), so there each is built where it stands.
struct AwaitingBodyOfCoroutine {
    CoroutineKind kind;
    Reference driver;
};
std::optional<AwaitingBodyOfCoroutine> findAwaitingBodyOfCoroutine(Walker&);

Draft draftOfCoroutineCalls(Walker&, Opcode, Origin);
Reference undefinedOfCoroutineCalls(Walker&);
Reference buildNewPromiseOfCoroutineCalls(Walker&, Origin);
// ResolvePromise or RejectPromise. False: the walk cannot go on.
bool buildSettlementOfCoroutineCalls(Walker&, Opcode, Reference promise, Reference value, Origin);
// Failing is a fault of the compiler. Null: the walk cannot go on.
Reference proveOrTrapInCoroutineCalls(Walker&, Reference, TypeID promised, Origin);
// The walk goes on in a new block, from which whatever is thrown comes to the block that is answered.
BlockID standUnderHandlerOfCoroutineCalls(Walker&);
// The walk goes on in that block. What was thrown.
Reference catchInHandlerOfCoroutineCalls(Walker&, BlockID handler, Origin);
bool isCellOfFrameInCoroutineCalls(const Walker&, Reference);
// No operand of the Call lies in the frame, and each has by its own type what the ramp of the callee tests (C8). It builds nothing.
bool writtenOperandsMayStartBodyOfCoroutine(Walker&, const Draft& written, FunctionID callee);
// The walk never comes to the operation, so walkOperation() leaves nothing for what Link lists there.
void leaveAnythingForSwallowedOperation(Walker&, ValueID);
// `name`: the value, and on return the last cast of it. The one operation that takes that. Null: as findOnlyTakerBehindCasts().
ValueID followCastsToOnlyTaker(const BodyIndex&, ValueID& name);

// ---- AOTWalkCoroutineAwaitedCalls.cpp

bool buildAwaitedCall(Walker&, const BodyValue& call);

// The Await that a call of the unit's own body goes on to, by the input alone.
struct WaitOfCallInCoroutine {
    ValueID await;
    // Among the operations of the block of the call.
    uint32_t placeOfCall { 0 };
    // The place of the Await, or the number of operations of the block where the jump at its end brings the value to the Await.
    uint32_t endOfWait { 0 };
    // The name under which the jump brings it. Null: the Await stands in the block.
    ValueID brought;
};
// The Await alone takes what the operation gives, it takes no extra turn, it lies in the stretch being walked, and whatever stands between the two may as well stand in front of both.
std::optional<WaitOfCallInCoroutine> findWaitOfCallInCoroutine(Walker&, const BodyIndex&, BodyBlock, ValueID call);
// What stands between is walked, one operation at a time, and `held` is what the Await holds. False: the walk cannot go on.
bool walkUpToAwaitOfCall(Walker&, const BodyIndex&, BodyBlock, const WaitOfCallInCoroutine&, Vector<Reference, 4>& held);
// `given` stands for what the Await gives, and the walk goes on behind the wait.
void finishWaitOfCall(Walker&, BodyBlock, const WaitOfCallInCoroutine&, Reference given);

// ---- AOTWalkCoroutineAwaitsAtJoins.cpp

// The jump at the end of the block alone takes the value, and brings it to where awaitTakenByEdgesInto() takes it. Null: it does not.
ValueID findAwaitThatJumpBringsValueTo(Walker&, const BodyIndex&, BodyBlock, ValueID brought);
// What the Await holds, under the names that it has where the jump to `target` stands. Nothing: the build has failed.
std::optional<Vector<Reference, 4>> findWhatAwaitHoldsOnWayThrough(Walker&, const BodyIndex&, BodyTarget, ValueID await);
// A cast between a parameter that the edges have awaited for and the Await speaks of the promise. It is bound to its operand, whatever it claims. False: the operation is no such cast.
bool nameCastInFrontOfAwaitTakenByEdges(Walker&, const BodyValue& cast);

// ---- AOTWalkCoroutineStepsByHand.cpp, AOTWalkCoroutinePromiseAll.cpp

bool buildStepWrittenByHand(Walker&, const BodyValue& call);
bool buildPromiseAllOfElements(Walker&, const BodyValue& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
