/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations and Guesses, what comes out of the object that a call of a coroutine answers with: LinkTables::coroutines in the words of PositionTypes and of an expectation.
// It builds nothing and keeps nothing. At a suspension and at a throw: nothing, an answer speaks of every run of the function. Withheld: the table is empty, and every answer is null.

#include "config.h"
#include "AOTWalkCoroutinePositions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTLinkTables.h"
#include "AOTObjects.h"
#include "AOTTypeArena.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

FunctionID coroutineWhoseCallsAnswerWith(const ObjectTable& objects, ObjectID object)
{
    if (!object)
        return { };
    const ObjectRecord& record = objects.at(object);
    return record.kind == ObjectKind::CoroutineResult ? FunctionID(record.payload) : FunctionID();
}

static const LinkedCoroutine* findCoroutineInWalkCoroutinePositions(const ObjectTable& objects, const LinkTables& tables, ObjectID object)
{
    FunctionID function = coroutineWhoseCallsAnswerWith(objects, object);
    return function ? tables.findCoroutine(function) : nullptr;
}

// A type without values says that what stands behind the operation never runs. Nobody who asks here is told that much.
static TypeID unlessNeverInWalkCoroutinePositions(TypeID type)
{
    return type && !isNeverType(type) ? type : TypeID();
}

TypeID awaitedOfObjectOfCoroutine(TypeArena& types, const LinkTables& tables, bool tablesAreProof, ObjectID object)
{
    const LinkedCoroutine* coroutine = findCoroutineInWalkCoroutinePositions(types.objects(), tables, object);
    if (!coroutine)
        return { };
    if (tablesAreProof && (!coroutine->objectsStayAsMade || !types.builtins().mayHavePristineInstances(types.objects().classOf(object))))
        return { };
    return unlessNeverInWalkCoroutinePositions(coroutine->fulfilledWith);
}

TypeID returnedByBodyThatNeverSuspends(const CallingConventions& conventions, const LinkTables& tables, FunctionID function)
{
    if (!conventions.of(function).coroutineNeverSuspends)
        return { };
    const LinkedCoroutine* coroutine = tables.findCoroutine(function);
    return coroutine ? unlessNeverInWalkCoroutinePositions(coroutine->returned) : TypeID();
}

TypeID typeHeldOfResultOfCoroutine(const Walker& walker, FunctionID callee)
{
    if (!walker.facts().held().findCoroutine(callee) && !walker.facts().linked().findCoroutine(callee))
        return { };
    TypeArena& types = walker.types();
    return types.forObject(types.objects().coroutineResultOf(callee));
}

TypeID typeOfPromiseExpectedToGive(Walker& walker, TypeID settledWith)
{
    TypeArena& types = walker.types();
    if (!settledWith || !types.at(settledWith).isJavaScriptValue())
        return { };
    return types.builtinClass(BuiltinClasses::Promise, singleElementSpan(settledWith));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
