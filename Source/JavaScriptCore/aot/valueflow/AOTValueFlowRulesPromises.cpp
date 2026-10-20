/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName promiseValue = nameOf(BuiltinSlots::PromiseValue);
static constexpr ValueFlowName promiseReason = nameOf(BuiltinSlots::PromiseReason);
static constexpr ValueFlowName resolvedPromises = ValueFlowName::of(ValueFlowNameKind::ResolvedPromises);
static constexpr ValueFlowName rejectedPromises = ValueFlowName::of(ValueFlowNameKind::RejectedPromises);

void resolvePromises(ValueFlowRuleContext& context, ClimbingSet promises, ClimbingSet values)
{
    ValueFlowNodeID settledWith = context.lookup(values, awaitedValueName);
    forEachNodeOfSlotWrittenThrough(context, promises, promiseValue, [&](ValueFlowNodeID held) {
        context.connect(settledWith, held);
    });

    // A promise takes over the rejection of the promise it is resolved with. What a thenable rejects with comes from native code, like the error for a promise that is resolved with itself.
    ClimbingSet objects = context.restrict(values, context.universe().objects());
    if (!holdsSomething(context, objects))
        return;
    forEachNodeOfSlotWrittenThrough(context, promises, promiseReason, [&](ValueFlowNodeID held) {
        connectContentsOfSlot(context, objects, promiseReason, held);
        context.add(held, unknownValue(context));
    });
}

void rejectPromises(ValueFlowRuleContext& context, ClimbingSet promises, const ValueFlowOperand& reasons)
{
    forEachNodeOfSlotWrittenThrough(context, promises, promiseReason, [&](ValueFlowNodeID held) {
        context.connect(reasons, held);
    });
    // The host reports a rejection that no handler takes.
    context.connect(reasons, ValueFlowProgramNodes::escaped());
}

void runAwait(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ClimbingSet values = context.valueOf(context.operandsOf(statement)[0]);
    // A thenable is asked whether or not somebody reads the answer.
    ValueFlowNodeID awaited = context.lookup(values, awaitedValueName);
    if (statement.result)
        context.connect(awaited, statement.result);
    if (!statement.thrown)
        return;
    context.add(statement.thrown, unknownValue(context));
    connectContentsOfSlot(context, context.restrict(values, context.universe().objects()), promiseReason, statement.thrown);
}

// A promise whose `constructor` is not Promise is settled through Promise.prototype.then, which constructs the species of that constructor.
// The getter of a built-in constructor under @@species answers its receiver, so a class of the program that extends Promise is its own species.
static void constructSpeciesOfPromises(ValueFlowRuleContext& context, ClimbingSet promises)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet constructors = without(context, context.valueOf(context.lookup(promises, nameOf(context, BuiltinNames::constructor))), universe.objectOf(Builtins::Promise));
    ClimbingSet species = context.unite(constructors, context.valueOf(context.lookup(constructors, nameOf(context, BuiltinNames::speciesSymbol))));
    context.forEachMember(context.restrict(species, universe.builtinFunctions()), [&](ValueFlowObjectID constructor) {
        noteCallOfBuiltin(universe.builtinOf(constructor), ValueFlowCallKind::ConstructByNativeCode, context);
    });
    ClimbingSet constructed = context.restrict(species, context.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctions())));
    if (!holdsSomething(context, constructed))
        return;

    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueFlowCallGroup group = context.group(constructed, ValueFlowCallKind::ConstructByNativeCode);
    context.add(group.newTarget, constructed);
    context.add(group.otherArguments, unknownValue(context));
    context.connect(group.result, escaped);
    context.connect(group.thrown, escaped);
}

// A promise of the realm finds the built-in `then` on its prototype, which is locked, unless something of that name was put on the promise itself, properties were copied onto it or it was given another prototype.
static bool mayLackBuiltinThen(ValueFlowRuleContext& context, ClimbingSet promises)
{
    if (context.meet(promises, context.valueOf(ValueFlowProgramNodes::escaped())))
        return true;
    PropertyKeyID then = context.program().keyOf(BuiltinNames::then);
    for (ValueFlowName name : { ValueFlowName::property(then), ValueFlowName::getter(then), prototypeLinkName, ValueFlowName::of(ValueFlowNameKind::CopiedFrom) }) {
        if (holdsSomething(context, context.valueOf(context.read(promises, name))))
            return true;
    }
    return false;
}

static void askThenables(ValueFlowRuleContext& context, ClimbingSet thenables, ValueFlowNodeID result)
{
    // `then` of a promise of the realm is the built-in function. What the promise holds stands for its call. The built-in runs all the same.
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID builtinThen = universe.objectOf(Builtins::PromisePrototypeThen);
    ClimbingSet found = context.valueOf(context.lookup(thenables, nameOf(context, BuiltinNames::then)));
    if (context.contains(found, builtinThen))
        noteCallOfBuiltin(Builtins::PromisePrototypeThen, ValueFlowCallKind::CallByNativeCode, context);
    ClimbingSet methods = without(context, found, builtinThen);
    if (!context.meet(methods, universe.callables()))
        return;
    // The two functions that a thenable is handed are native code's: whatever it calls them with comes back from there.
    callInProtocol(context, { .callees = methods, .thisValue = thenables, .otherArguments = unknownValue(context) });
    context.add(result, unknownValue(context));
}

// What awaiting any member of the set gives.
void runAwaitedValue(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet values = context.setOfKey();
    ValueFlowNodeID result = statement.result;

    // Whatever is no thenable is the answer itself.
    ValueSetID promisesOfRealm = universe.sureInstancesOf(BuiltinClasses::Promise);
    context.add(result, context.restrict(values, ValueSets::complement(promisesOfRealm)));
    if (mayLackBuiltinThen(context, context.restrict(values, promisesOfRealm)))
        context.add(result, values);
    connectContentsOfSlot(context, context.restrict(values, universe.objects()), promiseValue, result);

    // The unknown object stands for thenables of the table too. It is looked at by itself: a method nobody knows would get every other thenable as `this`.
    ClimbingSet objects = known(context, values);
    if (mayBeUnknownObject(context, values))
        askThenables(context, ClimbingSet(universe.unknownObjectAlone()), result);
    askThenables(context, objects, result);
    constructSpeciesOfPromises(context, context.restrict(objects, universe.instancesOf(BuiltinClasses::Promise)));
}

bool applyConstructionOfPromise(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!isConstruction(call.kind) || universe.objectOf(builtin) != universe.objectOf(Builtins::Promise) || !call.resultObject || !call.contentsObject)
        return false;

    // What a promise is rejected with escapes, so one function may reject them all.
    context.use(builtin, BuiltinUse::Constructed);
    ClimbingSet promise = only(context, call.resultObject);
    ClimbingSet resolve = only(context, call.contentsObject);
    ClimbingSet reject = only(context, universe.resultOf(Builtins::Promise, 1));
    context.connect(context.masked(context.lookup(context.valueOf(call.thisValue), nameOf(context, BuiltinNames::prototype)), universe.objects()), context.written(promise, prototypeLinkName));
    context.add(context.written(context.unite(resolve, reject), prototypeLinkName), only(context, universe.objectOf(Builtins::FunctionPrototype)));
    context.add(context.written(resolve, resolvedPromises), promise);
    context.add(context.written(reject, rejectedPromises), promise);

    ClimbingSet executors = context.valueOf(call.otherArguments);
    if (!call.arguments.empty())
        executors = call.argumentsMayBeAbsent ? context.unite(executors, context.valueOf(call.arguments[0])) : context.valueOf(call.arguments[0]);
    std::array<ValueFlowNodeID, 2> settlingFunctions { context.holding(resolve), context.holding(reject) };
    callInProtocol(context, { .callees = executors, .thisValue = ClimbingSet(universe.undefinedValue()), .arguments = settlingFunctions });
    // What the executor throws is what the promise is rejected with.
    context.add(context.written(promise, promiseReason), unknownValue(context));

    if (call.result)
        context.add(call.result, promise);
    if (call.thrown)
        context.add(call.thrown, unknownValue(context));
    return true;
}

void settlePromisesThrough(ClimbingSet functions, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    ClimbingSet resolved = context.valueOf(context.read(functions, resolvedPromises));
    ClimbingSet rejected = context.valueOf(context.read(functions, rejectedPromises));
    if (!holdsSomething(context, context.unite(resolved, rejected)))
        return;

    ValueFlowOperand nothingPassed { { }, context.universe().undefinedValue(), ValueSets::anything };
    Vector<ValueFlowOperand, 3> passed;
    if (!call.arguments.empty())
        passed.append(call.arguments[0]);
    if (call.arguments.empty() || call.argumentsMayBeAbsent) {
        passed.append(call.otherArguments);
        passed.append(nothingPassed);
    }
    for (const ValueFlowOperand& operand : passed) {
        if (holdsSomething(context, resolved))
            resolvePromises(context, resolved, context.valueOf(operand));
        if (holdsSomething(context, rejected))
            rejectPromises(context, rejected, operand);
    }
    if (call.result)
        context.add(call.result, context.universe().undefinedValue());
}

void letNativeCodeSettlePromises(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    context.add(context.written(context.valueOf(context.read(escaped, resolvedPromises)), promiseValue), unknownValue(context));
    context.add(context.written(context.valueOf(context.read(escaped, rejectedPromises)), promiseReason), unknownValue(context));
}

void runPromiseOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);

    switch (statement.opcode) {
    case Opcode::CreatePromise: {
        ClimbingSet promise = only(context, universe.siteObject(statement));
        context.connect(context.masked(context.lookup(context.valueOf(operands[0]), nameOf(context, BuiltinNames::prototype)), universe.objects()), context.written(promise, prototypeLinkName));
        if (statement.thrown)
            context.add(statement.thrown, unknownValue(context));
        return;
    }
    case Opcode::ResolvePromise: {
        ClimbingSet promises = context.restrict(context.valueOf(operands[0]), universe.objects());
        context.mark(promises, ObjectMutation::StoreAfterCreation);
        resolvePromises(context, promises, context.valueOf(operands[1]));
        return;
    }
    case Opcode::RejectPromise: {
        ClimbingSet promises = context.restrict(context.valueOf(operands[0]), universe.objects());
        context.mark(promises, ObjectMutation::StoreAfterCreation);
        rejectPromises(context, promises, operands[1]);
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
