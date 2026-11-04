/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- Two doors of AOTWalkCoroutines.cpp, which the hook of coroutines-2 puts behind buildSuspensionOfCoroutine(): each is three lines over the static function of that file.
// The frame of the unit can be saved where the walk stands in the unit's own body. Nothing: no suspension may be built. `driver`: the promise, or undefined, of an async function; the generator.
std::optional<CoroutineKind> kindOfSuspendingBody(Walker&, Reference& driver);
// The one way a suspension gets into Code: noteSuspensionOfCoroutine(), build(), invalidateHeap().
Reference buildSuspensionInCoroutine(Walker&, Draft&& suspension);

// ---- AOTWalkCoroutineThenables.cpp
// No lookup of `then` finds anything on a value of the type in any run, and none is a promise: primitives; layouts that linked() proves closed (no field, appended field or member under the key, up the whole row;
// no GainsProperty, ChangesPrototype, GetsAccessor; !reachesUnseenCode; listsEveryMemberAbove). False: it may, or nobody can tell: every object of an engine's class, `anything`.
bool isProvenToHaveNoThen(const Program&, const Facts&, TypeArena&, TypeID);
// The same, and a list that the state knows fresh and as made.
bool isProvenToHaveNoThen(Walker&, Reference);
// IsNotThenable, or the constant where the type decides: true as above, false of what is proven a promise. Null: the walk cannot go on.
Reference buildTestForNoThen(Walker&, Reference value, Origin);

// ---- AOTWalkCoroutinePromises.cpp
// No way from the entry of the unit to where the walk stands has passed a suspension: !hasBuiltSuspension, no loop around, the unit's own body.
bool frameWasNeverSuspended(const Walker&);
// buildSuspensionOfCoroutine(), in front of build(). It sets hasBuiltSuspension, then asks prepareSuspensionOfCoroutine() (coroutines-5).
void noteSuspensionOfCoroutine(Walker&, Draft& suspension);
// replaceCoroutineOperation(), behind its test of the opcode. True: built and bound. It asks, of an Await: takeWhatEdgesHaveAwaited() (3) > mergeAwaitIntoYield (2).
// Of a Yield: replaceYieldOfFusedGenerator() (4) > the yield of a merged await (2). Of an InitialYield: replaceInitialYieldOfGenerator() (4).
bool replaceSuspensionOfCoroutine(Walker&, const BodyValue&);
// buildAwaitBySavingFrame(), behind its two refusals. The awaited value has no `then`: the await is built, in a loop behind AwaitMayContinue, and the answer is `awaited` itself. Null: nothing was built.
Reference buildAwaitOfWhatHasNoThen(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin);
// convertReturnedValueOfCoroutine(), in front of the draft of ReturnAsyncResult. Null: nothing was built, and the stub returns the result.
Reference buildResultInFrontOfFirstSuspension(Walker&, Reference driver, Reference returned, Origin);
// `promise` is fresh and no settlement was built on it. FulfillFreshPromise where the value has no `then`; behind a test where `mayTest`; else ResolvePromise as ever. It always builds.
void resolvePromiseThatNothingHasTaken(Walker&, Reference promise, Reference value, bool mayTest, Origin);
// For buildCoroutineOperation(): CreatePromise (P2), ResolvePromise (P5), a Call of Promise.resolve (P7), HasJSType (P11), ReadsIteratorOfListUnseen (P12). False: nothing was built.
bool buildPromiseOperation(Walker&, const BodyValue&);
// What the state knows `promise` to be fulfilled with: this code has made and fulfilled it in the block in which the walk stands, and it is still fresh. Null: nothing.
Reference valueOfPromiseFulfilledHere(Walker&, Reference promise);
// mayBeWalkedAtInlining(), in the place of `function.isCoroutine()`. The CoroutineBody as written may be read at a call: coroutineNeverSuspends. False of every other coroutine.
bool bodyOfCoroutineMayBeCopied(const Walker&, FunctionID callee);
// Walker::leadEdge(), for the block on an edge that alone enters its target. Rare: the terminator is a Switch on ResultAt Mode of a yield and the case is not ResumeMode::Next. Normal else.
BlockFrequency frequencyOfBlockBehindResumption(const Walker&, const BodyTerminator&, uint32_t targetIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
