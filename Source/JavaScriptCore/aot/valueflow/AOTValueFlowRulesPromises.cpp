/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include "AOTVector.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName promiseValue = nameOf(BuiltinSlots::PromiseValue);
static constexpr ValueFlowName promiseReason = nameOf(BuiltinSlots::PromiseReason);
static constexpr ValueFlowName resolvedPromises = ValueFlowName::of(ValueFlowNameKind::ResolvedPromises);
static constexpr ValueFlowName rejectedPromises = ValueFlowName::of(ValueFlowNameKind::RejectedPromises);

// What a thenable rejects with comes from native code, like the error for a promise that is resolved with itself.
void adoptRejectionOf(ValueFlowRuleContext& context, ClimbingSet values, ValueFlowNodeID reason)
{
    ClimbingSet objects = context.restrict(values, context.universe().objects());
    if (!holdsSomething(context, objects))
        return;
    connectContentsOfSlot(context, objects, promiseReason, reason);
    context.add(reason, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
}

void resolvePromises(ValueFlowRuleContext& context, ClimbingSet promises, ClimbingSet values)
{
    ValueFlowNodeID settledWith = context.lookup(values, awaitedValueName);
    forEachNodeOfSlotWrittenThrough(context, promises, promiseValue, [&](ValueFlowNodeID held) {
        context.connect(settledWith, held);
    });

    forEachNodeOfSlotWrittenThrough(context, promises, promiseReason, [&](ValueFlowNodeID held) {
        adoptRejectionOf(context, values, held);
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
    context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    connectContentsOfSlot(context, context.restrict(values, context.universe().objects()), promiseReason, statement.thrown);
}

// A promise whose `constructor` is not Promise is settled through Promise.prototype.then, which constructs the species of that constructor.
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

    // The executor that the constructor is handed is native code's, and so is whoever takes the promise.
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    forEachPartOfCallees(context, constructed, [&](ClimbingSet part) {
        ValueFlowCallGroup group = context.group(part, ValueFlowCallKind::ConstructByNativeCode, ValueFlowCalleeOrigin::MemberFoundUnderName);
        context.add(group.newTarget, part);
        context.add(group.otherArguments, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult));
        context.connect(group.result, escaped);
        context.connect(group.thrown, escaped);
    });
}

// A promise of the realm finds the built-in `then` on its prototype, which is locked, unless something of that name was put on the promise itself, properties were copied onto it or it was given another prototype.
static bool mayLackBuiltinThen(ValueFlowRuleContext& context, ClimbingSet promises)
{
    if (!holdsSomething(context, promises))
        return false;
    if (context.holdsAnyOf(promises, ValueFlowProgramNodes::escaped()))
        return true;
    bool mayBeUntracked = context.holdsAnyOf(promises, ValueFlowProgramNodes::lost());
    ClimbingSet untrackedObject(context.universe().untrackedObjectAlone());
    PropertyKeyID then = context.program().keyOf(BuiltinNames::then);
    for (ValueFlowName name : { ValueFlowName::property(then), ValueFlowName::getter(then), ValueFlowName::of(ValueFlowNameKind::CopiedFrom) }) {
        if (holdsSomething(context, context.valueOf(context.read(promises, name))))
            return true;
        if (mayBeUntracked && holdsSomething(context, context.valueOf(context.read(untrackedObject, name))))
            return true;
    }
    // A built-in says what lies above the promise that it allocates.
    ValueSetID otherPrototypes = ValueSets::complement(context.maker().of(context.universe().objectOf(Builtins::PromisePrototype)));
    if (context.meet(context.valueOf(context.read(promises, prototypeLinkName)), otherPrototypes))
        return true;
    return mayBeUntracked && context.meet(context.valueOf(context.read(untrackedObject, prototypeLinkName)), otherPrototypes);
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
    callInProtocol(context, { .callees = methods, .thisValue = thenables, .otherArguments = unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult) });
    context.add(result, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult));
}

// As far as its layout says, an instance of a class that extends something may be a promise.
static ClimbingSet instancesFindingBuiltinThen(ValueFlowRuleContext& context, ClimbingSet instances)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueSetID mayBeBuiltinThen = context.maker().unite(context.maker().of(universe.objectOf(Builtins::PromisePrototypeThen)), universe.placeholdersAlone());
    Vector<ValueFlowObjectID, 8> finding;
    context.forEachMember(instances, [&](ValueFlowObjectID instance) {
        if (context.meet(context.valueOf(context.lookup(only(context, instance), nameOf(context, BuiltinNames::then))), mayBeBuiltinThen))
            finding.append(instance);
    });
    return ClimbingSet(context.maker().of(finding.span()));
}

static ValueSetID promisesAllocatedByBuiltins(ValueFlowRuleContext& context, ClimbingSet values)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueSetID otherClasses = ValueSets::complement(context.maker().of(universe.builtinTables().objectStandingFor(BuiltinClasses::Promise)));
    Vector<ValueFlowObjectID, 8> promises;
    context.forEachMember(context.restrict(values, context.maker().intersect(universe.objectsThatMayBeAnything(), universe.knownObjects())), [&](ValueFlowObjectID object) {
        ClimbingSet named = context.valueOf(context.written(only(context, object), allocatedAsName));
        if (holdsSomething(context, named) && !context.meet(named, otherClasses))
            promises.append(object);
    });
    return context.maker().of(promises.span());
}

// What awaiting any member of the set gives.
void runAwaitedValue(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet values = context.setOfKey();
    ValueFlowNodeID result = statement.result;

    // Whatever is no thenable is the answer itself.
    ValueSetID promisesOfRealm = context.maker().unite(universe.sureInstancesOf(BuiltinClasses::Promise), promisesAllocatedByBuiltins(context, values));
    context.add(result, context.restrict(values, ValueSets::complement(promisesOfRealm)));
    if (mayLackBuiltinThen(context, context.restrict(values, promisesOfRealm)))
        context.add(result, values);
    connectContentsOfSlot(context, context.restrict(values, universe.objects()), promiseValue, result);

    forEachPlaceholderAndOthers(context, context.restrict(values, universe.objects()), [&](ClimbingSet thenables) {
        askThenables(context, thenables, result);
    });
    // Native code does this to an escaped promise anyway.
    ClimbingSet promises = context.restrict(without(context, values, ValueFlowUniverse::unknownObject()), universe.instancesOf(BuiltinClasses::Promise));
    ValueSetID instancesOfProgram = universe.placeholders().objectsWithLayoutOfProgram;
    promises = context.unite(context.restrict(promises, ValueSets::complement(instancesOfProgram)), instancesFindingBuiltinThen(context, context.restrict(promises, instancesOfProgram)));
    forEachPlaceholderAndOthers(context, promises, [&](ClimbingSet part) {
        constructSpeciesOfPromises(context, part);
    });
}

bool applyConstructionOfPromise(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!isConstruction(call.kind) || universe.objectOf(builtin) != universe.objectOf(Builtins::Promise))
        return false;

    // A call without objects of its own shares those of the built-in. What a promise is rejected with escapes, so one function may reject them all.
    context.use(builtin, BuiltinUse::Constructed);
    ClimbingSet promise = only(context, call.resultObject ? call.resultObject : universe.resultOf(Builtins::Promise, 0));
    ClimbingSet resolve = only(context, call.contentsObject ? call.contentsObject : universe.resultOf(Builtins::Promise, 1));
    ClimbingSet reject = only(context, universe.resultOf(Builtins::Promise, 1));
    context.connect(context.masked(context.lookup(context.valueOf(call.thisValue), nameOf(context, BuiltinNames::prototype)), universe.objects()), context.written(promise, prototypeLinkName));
    context.add(context.written(context.unite(resolve, reject), prototypeLinkName), only(context, universe.objectOf(Builtins::FunctionPrototype)));
    context.add(context.written(promise, allocatedAsName), only(context, universe.builtinTables().objectStandingFor(BuiltinClasses::Promise)));
    context.add(context.written(resolve, resolvedPromises), promise);
    context.add(context.written(reject, rejectedPromises), promise);

    ClimbingSet executors = context.valueOf(call.otherArguments);
    if (!call.arguments.empty())
        executors = call.argumentsMayBeAbsent ? context.unite(executors, context.valueOf(call.arguments[0])) : context.valueOf(call.arguments[0]);
    std::array<ValueFlowNodeID, 2> settlingFunctions { context.holding(resolve), context.holding(reject) };
    callInProtocol(context, { .callees = executors, .thisValue = ClimbingSet(universe.undefinedValue()), .arguments = settlingFunctions, .origin = ValueFlowCalleeOrigin::Value });
    // What the executor throws is what the promise is rejected with.
    context.add(context.written(promise, promiseReason), unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));

    if (call.result)
        context.add(call.result, promise);
    if (call.thrown)
        context.add(call.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
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
    ValueSetID passedByNativeCode = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);
    auto settle = [&](ValueFlowName settledPromises, ValueFlowName slot) {
        forEachNodeOfSlotWrittenThrough(context, context.valueOf(context.read(escaped, settledPromises)), slot, [&](ValueFlowNodeID held) {
            context.add(held, passedByNativeCode);
        });
    };
    settle(resolvedPromises, promiseValue);
    settle(rejectedPromises, promiseReason);
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
            context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
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
