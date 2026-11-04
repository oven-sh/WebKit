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

TypeID contentsOfObjectOfCoroutine(TypeArena& types, const LinkTables& tables, ObjectID object, ContentsSlot slot)
{
    const LinkedCoroutine* coroutine = findCoroutineInWalkCoroutinePositions(types.objects(), tables, object);
    if (!coroutine)
        return { };
    switch (slot) {
    case ContentsSlot::Value:
        return unlessNeverInWalkCoroutinePositions(coroutine->fulfilledWith);
    // A generator that has not begun or has ended hands back what return() is passed, and a step behind the end gives undefined. Both are among what is sent.
    case ContentsSlot::Element:
        if (!coroutine->yielded || !coroutine->returned || !coroutine->sent)
            return { };
        return types.unionOf(types.unionOf(coroutine->yielded, coroutine->returned), coroutine->sent);
    case ContentsSlot::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
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

// Null unless every object of the type is what the calls of a generator function answer with, and something is known of what each of the functions yields.
static TypeID typeYieldedByInWalkCoroutinePositions(Walker& walker, TypeID generators)
{
    TypeArena& types = walker.types();
    if (!generators || holdsOnlyPrimitives(generators))
        return { };
    Type type = types.at(generators);
    if (!type.isObject() || !type.hasOnlyNamedObjects())
        return { };

    const Facts& facts = walker.facts();
    Vector<TypeID, 4> yieldedByEach;
    bool everyFunctionIsDescribed = types.at(type.objects).allOf([&](ObjectID object) {
        const LinkedCoroutine* proven = findCoroutineInWalkCoroutinePositions(types.objects(), facts.linked(), object);
        TypeID yielded = proven ? proven->yielded : TypeID();
        if (!yielded) {
            const LinkedCoroutine* held = findCoroutineInWalkCoroutinePositions(types.objects(), facts.held(), object);
            yielded = held ? held->yielded : TypeID();
        }
        if (!yielded)
            return false;
        yieldedByEach.append(yielded);
        return true;
    });
    if (!everyFunctionIsDescribed)
        return { };

    TypeID joined = TypeTable::never;
    for (TypeID yielded : yieldedByEach)
        joined = joinOf(types, facts.families(), joined, yielded);
    return isNeverType(joined) ? TypeID() : types.describedByShape(joined);
}

TypeID typeExpectedOfValueSteppedOutOf(Walker& walker, ValueID iterable)
{
    TypeArena& types = walker.types();
    Reference reference = walker.referenceOf(iterable);
    TypeID proven = reference && reference.isValue() ? walker.typeOf(reference) : TypeID();
    if (TypeID yielded = proven ? typeYieldedByInWalkCoroutinePositions(walker, types.withoutNullish(proven)) : TypeID())
        return yielded;
    ExpectationOfValue expectation = findExpectation(walker, iterable);
    return expectation.type ? typeYieldedByInWalkCoroutinePositions(walker, types.withoutNullish(expectation.type)) : TypeID();
}

TypeID typeExpectedOfValueSentIn(Walker& walker)
{
    const LinkedCoroutine* coroutine = walker.facts().linked().findCoroutine(walker.currentFunction());
    return coroutine ? coroutine->sent : TypeID();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
