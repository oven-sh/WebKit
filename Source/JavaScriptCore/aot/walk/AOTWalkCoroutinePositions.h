/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

class CallingConventions;
class ObjectTable;
class TypeArena;
struct LinkTables;

// Null: the object is no ObjectKind::CoroutineResult.
FunctionID coroutineWhoseCallsAnswerWith(const ObjectTable&, ObjectID);
// For PositionsOfWalk. `tables`: Facts::linked(), or Facts::held() for whoever only guesses. Each: null where nothing is known.
// Value of a promise: fulfilledWith. Element of a generator: yielded joined with returned, which is what a step may hand out whatever its status.
TypeID contentsOfObjectOfCoroutine(TypeArena&, const LinkTables& tables, ObjectID, ContentsSlot);
// fulfilledWith, where objectsStayAsMade: `constructor` and `then` are what the locked table lists. In held() objectsStayAsMade is not asked: the use tests.
TypeID awaitedOfObjectOfCoroutine(TypeArena&, const LinkTables& tables, bool tablesAreProof, ObjectID);
// `returned`, where FunctionConvention::coroutineNeverSuspends.
TypeID returnedByBodyThatNeverSuspends(const CallingConventions&, const LinkTables& tables, FunctionID);
// For Guesses. Each is an expectation; null: nothing is expected.
// typeHeldOfResultOfCallForGuesses(), of a coroutine: the object that its calls answer with.
TypeID typeHeldOfResultOfCoroutine(const Walker&, FunctionID callee);
// expectationOfCallForGuesses(), under ExpectedResultFlags::IsRowsOfQuery: Promise<rows>.
TypeID typeOfPromiseExpectedToGive(Walker&, TypeID settledWith);
// Of ResultAt Value of a step whose iterable is proven or held to be objects of generator functions: `yielded` of each, joined. linked() first, then held().
TypeID typeExpectedOfValueSteppedOutOf(Walker&, ValueID iterable);
// Of ResultAt Sent of a Yield of the body being read.
TypeID typeExpectedOfValueSentIn(Walker&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
