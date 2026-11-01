/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRules.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueSetID unknownValueFrom(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    const ValueFlowUniverse& universe = context.universe();
    context.noteUnknownSource(source);
    if (context.leavesOut(source))
        return universe.anyPrimitive();
    return context.maker().unite(universe.anyPrimitive(), universe.unknownObjectAlone());
}

ValueSetID undescribedValueThatIsNoBuffer(ValueFlowRuleContext& context) { return unknownValueFrom(context, ValueFlowUnknownSource::UndescribedNativeResult); }

ValueSetID unknownValue(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    if (source == ValueFlowUnknownSource::UndescribedNativeResult) {
        if (BuiltinID prototype = context.program().builtins().at(BuiltinClasses::Buffer).prototype)
            context.add(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins), context.maker().of(context.universe().objectOf(prototype)));
        // It may be an instance of any class of the host: nobody knows the prototype object above it.
        context.add(context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances), context.universe().unknownObjectAlone());
    }
    return unknownValueFrom(context, source);
}

static void runConvert(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (!statement.result)
        return;
    const ValueFlowOperand& converted = context.operandsOf(statement)[0];
    if (context.operationOf(statement).illTypedCause() == IllTypedCause::None) {
        context.connect(converted, statement.result);
        return;
    }

    ClimbingSet value = context.valueOf(converted);
    context.add(statement.result, without(context, value, ValueFlowUniverse::untrackedObject()));
    if (mayBeUntrackedObject(context, value))
        context.add(statement.result, context.maker().intersect(unknownValue(context, ValueFlowUnknownSource::UncheckedConversionOfUntracked), context.universe().objects()));
}

// Every use of `this` stands behind `this instanceof` the function itself. Which method answers that test, what `prototype` of the function holds and what stands above the global object are up to the program.
static void addWhatUndefinedBecomesBehindTestForOwnInstances(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet closure = only(context, universe.closureOf(statement.function));
    auto objectsFound = [&](ClimbingSet start, ValueFlowName name) {
        return context.restrict(context.valueOf(context.lookup(start, name)), universe.objects());
    };
    PropertyKeyID hasInstance = context.program().keyOf(BuiltinNames::hasInstanceSymbol);
    ValueSetID inheritedMethod = context.maker().of(universe.objectOf(Builtins::FunctionPrototypeSymbolHasInstance));
    ClimbingSet methods = context.unite(context.restrict(objectsFound(closure, ValueFlowName::property(hasInstance)), ValueSets::complement(inheritedMethod)), objectsFound(closure, ValueFlowName::getter(hasInstance)));
    ClimbingSet held = objectsFound(closure, ValueFlowName::property(context.program().keyOf(BuiltinNames::prototype)));
    ClimbingSet above = objectsFound(only(context, universe.objectOf(Builtins::GlobalThis)), prototypeLinkName);
    if (context.meet(methods, universe.knownObjects()) || context.meet(context.restrict(held, universe.knownObjects()), above))
        context.add(statement.result, only(context, universe.objectOf(Builtins::GlobalThis)));
    // A function that native code may call is handed every escaped object as `this`, itself too, so its own stores under keys nobody knows write all three with objects nobody knows.
    // Those do not name the global object. They make `this` an object nobody knows, of which no layout is said, and a store through it is to the global object what a store by unseen code is.
    if (context.meet(context.unite(methods, context.unite(held, above)), universe.placeholdersAlone()))
        context.add(statement.result, only(context, ValueFlowUniverse::unknownObject()));
}

static void runConversionToObject(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    if (statement.thrown)
        context.add(statement.thrown, only(context, ValueFlowUniverse::unknownObject()));
    if (!statement.result)
        return;

    auto mayBe = [&](ClimbingSet value, uint16_t primitives) {
        if ((primitives & Primitive::AnyString && context.meet(value, universe.strings())) || (primitives & Primitive::AnySymbol && context.meet(value, universe.symbols())))
            return true;
        for (uint16_t rest = primitives; rest; rest &= rest - 1) {
            if (context.contains(value, ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest))))
                return true;
        }
        return false;
    };
    ClimbingSet value = context.valueOf(context.operandsOf(statement)[0]);
    context.add(statement.result, context.restrict(value, universe.objects()));
    for (uint32_t ordinal = 0; ordinal < std::size(wrappersOfPrimitives); ++ordinal) {
        ValueFlowObjectID wrapper = universe.siteObject(statement, ordinal);
        if (wrapper && mayBe(value, wrappersOfPrimitives[ordinal].first))
            context.add(statement.result, only(context, wrapper));
    }
    if (statement.opcode != Opcode::ToThis || !mayBe(value, Primitive::Undefined | Primitive::Null))
        return;
    // Where every use stands behind a test that the global object fails, the uses never meet it. proveValuesOfOperations() says of the value itself that it may be the global object.
    context.functionWithBody(statement.function);
    if (isUsedOnlyAsInstanceOfOwnFunction(context.program(), statement.function, statement.site))
        return addWhatUndefinedBecomesBehindTestForOwnInstances(statement, context);
    context.add(statement.result, only(context, universe.objectOf(Builtins::GlobalThis)));
}

static void runNewDictionary(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID site = universe.siteObject(statement);
    ClimbingSet dictionary = only(context, site ? site : universe.resultOf(Builtins::ObjectCreate, 0));
    context.add(context.written(dictionary, prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    if (statement.result)
        context.add(statement.result, dictionary);
}

void callInProtocol(ValueFlowRuleContext& context, const ValueFlowProtocolCall& call)
{
    const ValueFlowUniverse& universe = context.universe();
    describeObjectsRefusedAsFunctions(context, call.callees);
    if (!context.meet(call.callees, universe.callables()))
        return;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ClimbingSet builtinFunctions = context.restrict(call.callees, universe.builtinFunctionsOfProtocols());
    if (call.origin == ValueFlowCalleeOrigin::MemberFoundUnderName && context.contains(call.callees, ValueFlowUniverse::unknownObject())) {
        context.forEachMemberInLine(context.valueWithin(ClimbingSet(universe.builtinFunctionsOfProtocols()), escaped), [&](ValueFlowObjectID callee) {
            if (context.program().builtins().at(universe.builtinOf(callee)).origin != BuiltinOrigin::Host)
                noteCallOfBuiltin(universe.builtinOf(callee), ValueFlowCallKind::CallByNativeCode, context);
        });
    }

    ValueFlowStatement protocol;
    protocol.function = context.functionOfStatement();
    protocol.result = call.result;
    applyEventsOfCall(protocol, context, builtinFunctions);

    if (holdsSomething(context, builtinFunctions)) {
        Vector<ValueFlowOperand, 2> arguments;
        for (ValueFlowNodeID argument : call.arguments)
            arguments.append({ argument, ValueSets::nothing, ValueSets::anything });
        ValueFlowBuiltinCall application;
        application.kind = ValueFlowCallKind::CallByNativeCode;
        application.thisValue = context.operandHolding(call.thisValue);
        application.isFoundOnReceiver = call.origin == ValueFlowCalleeOrigin::MemberFoundUnderName;
        application.arguments = arguments.span();
        application.otherArguments = { { }, call.otherArguments, ValueSets::anything };
        application.result = call.result;
        application.thrown = escaped;
        context.forEachMember(builtinFunctions, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = universe.builtinOf(callee);
            application.resultObject = universe.resultOf(builtin, 0);
            application.contentsObject = universe.resultOf(builtin, 1);
            applyBuiltin(builtin, application, context);
        });
    }

    RELEASE_ASSERT(call.arguments.size() <= numberOfGroupedArguments);
    ClimbingSet others = context.restrict(call.callees, context.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctionsOfProtocols())));
    forEachPartOfCallees(context, others, [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::CallByNativeCode, call.origin);
        context.add(group.thisValue, call.thisValue);
        for (unsigned index = 0; index < call.arguments.size(); ++index)
            context.connect(call.arguments[index], group.argument(index));
        context.add(group.otherArguments, call.otherArguments);
        if (call.result)
            context.connect(group.result, call.result);
        carryOut(context, group.thrown);
    });
}

static constexpr ValueFlowNameKind kindsOfMembers[] { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter };

static bool isRaised(ValueFlowRuleContext& context, ValueFlowHostNode flag) { return mayBeUnknownObject(context, context.valueOf(context.nodes().ofHost(flag))); }
static bool isRaisedSinceLastRun(ValueFlowRuleContext& context, ValueFlowHostNode flag) { return mayBeUnknownObject(context, context.newSince(context.nodes().ofHost(flag))); }

static LayoutID layoutOfInstance(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    LayoutID layout = context.universe().layoutOf(object);
    return layout && context.program().at(layout).kind == LayoutKind::Instance ? layout : LayoutID();
}

static bool isConstructedByProgram(ValueFlowRuleContext& context, ValueFlowObjectID object) { return layoutOfInstance(context, object) || context.universe().kindOf(object) == ValueFlowObjectKind::ConstructedObject; }

static bool mayCodeOutsideImageRun(ValueFlowRuleContext& context)
{
    return isRaised(context, ValueFlowHostNode::CodeOutsideImageIsLoaded) || std::ranges::any_of(context.program().modules, [](const Module& module) {
        return module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode;
    });
}

static ClimbingSet leaveHoldersOfExportsUnsearched(ValueFlowRuleContext& context, ClimbingSet objects)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet holdersOfExports = context.restrict(objects, universe.objectsOfLibraryFoundByNameAlone());
    if (!holdsSomething(context, holdersOfExports) || mayCodeOutsideImageRun(context))
        return objects;
    context.add(context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode), holdersOfExports);
    context.add(context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects), holdersOfExports);
    ClimbingSet prototypeObjects = context.restrict(holdersOfExports, universe.prototypeObjectsOfLibraryClasses());
    ClimbingSet others = context.restrict(holdersOfExports, ValueSets::complement(universe.prototypeObjectsOfLibraryClasses()));
    constexpr std::pair<ValueFlowNameKind, ValueFlowHostNode> membersUnderUnknownKey[] {
        { ValueFlowNameKind::UnknownKey, ValueFlowHostNode::ValuesUnderUnknownKeyInHeldHoldersOfExports },
        { ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowHostNode::GettersUnderUnknownKeyInHeldHoldersOfExports },
        { ValueFlowNameKind::GetterUnderNameOfEnumeration, ValueFlowHostNode::GettersUnderUnknownKeyInHeldHoldersOfExports },
        { ValueFlowNameKind::SetterUnderUnknownKey, ValueFlowHostNode::SettersUnderUnknownKeyInHeldHoldersOfExports },
    };
    for (auto [kind, node] : membersUnderUnknownKey) {
        context.connect(context.readWhatWasWritten(others, ValueFlowName::of(kind)), context.nodes().ofHost(node));
        context.connect(context.readWhatWasWritten(prototypeObjects, ValueFlowName::of(kind)), context.nodes().ofHost(ValueFlowHostNode::MembersUnderUnknownKeyInHeldPrototypeObjectsOfLibraryClasses));
    }
    for (ValueFlowNameKind kind : { ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter })
        context.connect(context.readWhatWasWritten(prototypeObjects, { kind, 0 }), ValueFlowProgramNodes::escaped());
    return context.restrict(objects, ValueSets::complement(universe.objectsOfLibraryFoundByNameAlone()));
}

ValueFlowNodeID heldPrototypeObjectsOfLibraryClasses(ValueFlowRuleContext& context)
{
    return context.masked(context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects), context.universe().prototypeObjectsOfLibraryClasses());
}

void notePrototypeObjectsAboveUnknownObject(ValueFlowRuleContext& context)
{
    ValueSetID prototypeObjectsOfHost = context.universe().builtinTables().heap().prototypeObjectsOfHost();
    ValueFlowNodeID withInstances = context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances);
    ValueFlowNodeID answersThatNoReadLists = context.nodes().ofHost(ValueFlowHostNode::AnswersThatNoReadLists);
    context.add(answersThatNoReadLists, context.universe().builtinTables().heap().prototypeObjectsOfLanguage());
    context.connect(context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsAboveEscapedObjects), answersThatNoReadLists);
    if (context.holdsAnyOf(ClimbingSet(context.universe().unknownObjectAlone()), withInstances))
        context.add(answersThatNoReadLists, prototypeObjectsOfHost);
    else
        context.connect(context.masked(withInstances, prototypeObjectsOfHost), answersThatNoReadLists);
}

static void followWhatHasArrived(ValueFlowRuleContext& context, ClimbingSet arrived)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();

    arrived = leaveHoldersOfExportsUnsearched(context, arrived);

    Vector<ValueFlowObjectID, 64> instances;
    context.forEachMember(known(context, arrived), [&](ValueFlowObjectID object) {
        if (isConstructedByProgram(context, object))
            instances.append(object);
    });
    ClimbingSet others = context.restrict(arrived, ValueSets::complement(context.maker().of(instances.span())));

    // What an object holds is in the hands that hold the object. The constructor of a library class also escapes where a lookup on a value nobody knows finds a member on it, and then it
    // is in nobody's hands: its `prototype` escapes with it. runEscape() carries out the `prototype` of a constructor that was itself carried out.
    ClimbingSet constructorsOfLibrary = context.restrict(arrived, universe.constructorsOfLibraryClasses());
    ClimbingSet otherHolders = context.restrict(arrived, ValueSets::complement(universe.constructorsOfLibraryClasses()));
    for (ValueFlowNameKind kind : kindsOfMembers) {
        carryOut(context, context.readWhatWasWritten(otherHolders, { kind, 0 }));
        if (!holdsSomething(context, constructorsOfLibrary))
            continue;
        ValueFlowNodeID held = context.readWhatWasWritten(constructorsOfLibrary, { kind, 0 });
        carryOut(context, context.masked(held, ValueSets::complement(universe.prototypeObjectsOfLibraryClasses())));
        context.connect(context.masked(held, universe.prototypeObjectsOfLibraryClasses()), escapedNode);
    }
    context.connect(context.masked(context.readWhatWasWritten(arrived, { ValueFlowNameKind::CopiedFrom, 0 }), ValueSets::complement(universe.builtinTables().heap().objectsHoldingOnlyWhatWasWritten())), escapedNode);
    context.connect(context.masked(context.readWhatWasWritten(others, prototypeLinkName), ValueSets::complement(universe.builtinTables().heap().objectsThatLetInUnseenCode())), escapedNode);
    context.connect(context.masked(context.readWhatWasWritten(others, prototypeLinkName), universe.builtinTables().heap().namespacesNotedWhenCarriedOut()), context.nodes().ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues));
    context.add(escapedNode, objectsHeldFromCreation(context, others));
    for (const ValueFlowClassOfEntries& classOfEntries : universe.builtinTables().heap().classesOfEntries()) {
        if (context.meet(arrived, classOfEntries.entries))
            context.add(escapedNode, only(context, classOfEntries.prototype));
    }
    context.mark(arrived, ObjectMutation::ReachNativeCode);
    // Beside the unknown object a hub lists no object that has escaped, so what it would find there is taken from here.
    context.connect(context.masked(context.lookup(known(context, arrived), prototypeLinkName), universe.builtinObjects()), context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsAboveEscapedObjects));

    letNativeCodeStepIterators(context, arrived);
    ValueSetID describedByEntries = universe.builtinTables().closuresThatEntriesDescribe();
    context.forEachMember(context.restrict(arrived, describedByEntries), [&](ValueFlowObjectID closure) {
        enterCodeThatEntryDescribes(closure, context);
    });
    context.forEachMember(context.restrict(arrived, context.maker().intersect(universe.closures(), ValueSets::complement(describedByEntries))), [&](ValueFlowObjectID closure) {
        enterFromNativeCode(context, universe.closureFunction(closure));
    });
}

static ValueFlowObjectID familyOfInstance(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    if (LayoutID layout = layoutOfInstance(context, object))
        return context.universe().instanceOf(layout);
    return context.universe().kindOf(object) == ValueFlowObjectKind::ConstructedObject ? object : ValueFlowObjectID();
}

static ValueSetID instancesOfFamily(ValueFlowRuleContext& context, ValueFlowObjectID family)
{
    if (LayoutID layout = context.universe().layoutOf(family))
        return context.universe().objectsWith(layout);
    return context.maker().of(family);
}

static void lookUpThroughInstances(ValueFlowRuleContext& context, ValueFlowObjectID family)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID instances = instancesOfFamily(context, family);

    bool unseenCodeRuns = isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
    ClimbingSet untrackedObject(universe.untrackedObjectAlone());
    ValueSetID functions = context.maker().unite(universe.closures(), universe.builtinFunctions());
    ValueSetID passedByNativeCode = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);
    ValueFlowName constructorName = ValueFlowName::property(program.keyOf(BuiltinNames::constructor));
    ValueFlowNodeID constructorsNode = context.nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances);

    ValueFlowNodeID chainNode = context.lookup(context.valueWithin(ClimbingSet(instances), escapedNode), prototypeLinkName);
    ClimbingSet chain = context.valueOf(chainNode);
    context.add(context.nodes().ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues), context.restrict(chain, universe.builtinTables().heap().namespacesWhoseHoldersListThem()));
    context.connect(context.masked(chainNode, universe.holdersOfLibraryExports()), context.nodes().ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues));
    if (mayBeUntrackedObject(context, chain))
        context.add(escapedNode, untrackedObject);
    ClimbingSet objectsOfProgram = context.restrict(known(context, chain), ValueSets::complement(universe.builtinObjects()));
    // Where a statement has put a value nobody knows above them, they find what stands above the instances of any other family.
    ValueFlowNodeID belowInstances = context.nodes().ofHost(ValueFlowHostNode::ObjectsPutBelowInstances);
    LayoutID layoutOfFamily = universe.layoutOf(family);
    FunctionID constructor = layoutOfFamily ? program.at(layoutOfFamily).constructor : universe.functionOf(family);
    ClimbingSet putAbove = context.valueOf(context.readWhatWasWritten(context.restrict(objectsOfProgram, ValueSets::complement(instances)), prototypeLinkName));
    if (constructor)
        putAbove = context.unite(putAbove, context.valueOf(context.lookup(only(context, universe.closureOf(constructor)), ValueFlowName::property(program.keyOf(BuiltinNames::prototype)))));
    if (mayBeUnknownObject(context, putAbove))
        context.add(belowInstances, ClimbingSet(instances));
    ClimbingSet above = context.restrict(objectsOfProgram, ValueSets::complement(instances));
    above = context.unite(above, context.restrict(context.valueOf(context.readWhatWasWritten(ClimbingSet(instances), prototypeLinkName)), instances));
    above = leaveHoldersOfExportsUnsearched(context, above);
    if (!holdsSomething(context, above))
        return;
    Vector<ValueFlowObjectID, 32> builtinsThatReachAbove;
    for (BuiltinID builtin : universe.builtinTables().builtinsThatReachAboveTheirOperands())
        universe.appendObjectsOf(builtin, builtinsThatReachAbove);
    if (unseenCodeRuns || (!context.isSabotaged(Sabotage::NothingClimbsAboveInstances) && context.holdsAnyOf(ClimbingSet(context.maker().of(builtinsThatReachAbove.span())), escapedNode)))
        context.add(escapedNode, above);
    context.add(context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances), above);

    ClimbingSet found;
    auto take = [&](ValueFlowNodeID members) {
        context.connect(context.masked(members, ValueSets::complement(functions)), escapedNode);
        found = context.unite(found, context.restrict(context.valueOf(members), functions));
    };
    bool mayBeLost = context.holdsAnyOf(above, ValueFlowProgramNodes::lost());
    for (ValueFlowNameKind kind : kindsOfMembers) {
        take(context.readWhatWasWritten(above, { kind, 0 }));
        if (mayBeLost)
            take(context.readWhatWasWritten(untrackedObject, { kind, 0 }));
    }
    context.add(constructorsNode, context.restrict(objectsHeldFromCreation(context, above), universe.closures()));
    context.connect(context.masked(context.readWhatWasWritten(above, constructorName), universe.closures()), constructorsNode);
    if (mayBeLost)
        context.connect(context.masked(context.readWhatWasWritten(untrackedObject, constructorName), universe.closures()), constructorsNode);

    Vector<ValueFlowObjectID, 16> functionsOfLibrary;
    context.forEachMember(context.restrict(found, universe.closures()), [&](ValueFlowObjectID closure) {
        FunctionRole role = program.at(universe.closureFunction(closure)).role;
        if (role == FunctionRole::Library || role == FunctionRole::Model)
            functionsOfLibrary.append(closure);
    });
    ValueSetID handedOut = context.maker().of(functionsOfLibrary.span());
    context.add(escapedNode, handedOut);
    ClimbingSet called = context.restrict(found, ValueSets::complement(handedOut));
    if (!holdsSomething(context, called))
        return;

    // Native code only calls them. Whoever reads one off a value nobody knows holds it.
    ValueFlowNodeID namesRead = context.nodes().ofHost(ValueFlowHostNode::NamesReadOffValuesNobodyKnows);
    ValueFlowNodeID namesFoundBelow = context.nodes().ofHost(ValueFlowHostNode::NamesFoundBelowValuesNobodyKnows);
    ValueFlowNodeID namesHandedOn = context.nodes().ofHost(ValueFlowHostNode::NamesOfReadsThatHandTheirValueOn);
    ClimbingSet anyName(markOfAnyName(universe));
    ClimbingSet anyIndex(context.maker().unite(markOfAnyIndex(universe), markOfAnyName(universe)));
    ClimbingSet names = context.unite(context.valueOf(context.read(above, ValueFlowCellName { ValueFlowNameKind::AddedNames })), context.valueOf(context.read(above, ValueFlowCellName { ValueFlowNameKind::NamesOfMethods })));
    Vector<ValueFlowObjectID, 16> declaredNames;
    context.forEachMember(known(context, above), [&](ValueFlowObjectID object) {
        LayoutID layout = universe.layoutOf(object);
        if (!layout)
            return;
        for (const Field& field : program.at(layout).fields) {
            if (field.key)
                declaredNames.append(universe.keyConstant(field.key));
        }
    });
    names = context.unite(names, ClimbingSet(context.maker().of(declaredNames.span())));
    bool holdsThemUnderAnyName = context.meet(names, ValueSets::complement(universe.keys()));
    bool isReadUnderSomeName = holdsSomething(context, context.valueOf(namesRead)) || (holdsSomething(context, context.valueOf(namesFoundBelow)) && holdsSomething(context, context.valueOf(namesHandedOn)));
    if (context.holdsAnyOf(anyName, namesRead) || (context.holdsAnyOf(anyName, namesFoundBelow) && context.holdsAnyOf(anyName, namesHandedOn)) || (holdsThemUnderAnyName && isReadUnderSomeName))
        context.add(escapedNode, called);
    else {
        if (context.holdsAnyOf(anyIndex, namesRead) || (context.holdsAnyOf(anyIndex, namesFoundBelow) && context.holdsAnyOf(anyIndex, namesHandedOn)))
            context.connect(context.masked(context.readWhatWasWritten(above, elementsName), functions), escapedNode);
        ClimbingSet keys = context.restrict(names, universe.keys());
        ClimbingSet foundBelow = context.holdsAnyOf(anyName, namesFoundBelow) ? keys : context.valueWithin(keys, namesFoundBelow);
        ClimbingSet handedOn = context.holdsAnyOf(anyName, namesHandedOn) ? foundBelow : context.valueWithin(foundBelow, namesHandedOn);
        context.forEachMember(context.unite(context.valueWithin(keys, namesRead), handedOn), [&](ValueFlowObjectID key) {
            context.connect(context.masked(context.readWhatWasWritten(above, ValueFlowName::property(program, universe.keyOf(key))), functions), escapedNode);
        });
    }

    ValueFlowCallGroup group = context.group(called, ValueFlowCallKind::CallByNativeCode);
    context.connect(context.masked(escapedNode, instances), group.thisValue);
    context.add(group.thisValue, context.valueWithin(context.valueOf(belowInstances), escapedNode));
    ValueFlowObjectID makersOfProxies[] { universe.objectOf(Builtins::Proxy), universe.objectOf(Builtins::ProxyRevocable) };
    if (holdsSomething(context, context.valueWithin(ClimbingSet(context.maker().of(makersOfProxies)), context.nodes().ofHost(ValueFlowHostNode::RunBuiltins))))
        context.add(group.thisValue, universe.unknownObjectAlone());
    context.add(group.otherArguments, passedByNativeCode);
    context.connect(group.result, escapedNode);
    context.connect(group.thrown, escapedNode);
}

static void runLookupThroughInstances(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    context.newSince(statement.result);
    lookUpThroughInstances(context, ValueFlowObjectID(context.nameOfKey().payload));
}

static void lookUpThroughFamiliesOf(ValueFlowRuleContext& context, ClimbingSet arrived)
{
    Vector<ValueFlowObjectID, 16> families;
    context.forEachMember(known(context, arrived), [&](ValueFlowObjectID object) {
        if (ValueFlowObjectID family = familyOfInstance(context, object))
            families.append(family);
    });
    if (families.isEmpty())
        return;

    ValueSetID& familiesMet = context.rememberedByStatement();
    ValueSetID familiesMetBefore = familiesMet ? familiesMet : ValueSets::nothing;
    familiesMet = context.maker().unite(familiesMetBefore, context.maker().of(families.span()));
    context.forEachMember(ClimbingSet(context.maker().intersect(familiesMet, ValueSets::complement(familiesMetBefore))), [&](ValueFlowObjectID family) {
        context.lookup(ClimbingSet(instancesOfFamily(context, family)), { ValueFlowNameKind::FoundThroughEscapedInstances, family.index() });
        lookUpThroughInstances(context, family);
    });
}

static void letNativeCodeCallBoundFunctions(ValueFlowRuleContext& context, ClimbingSet escapedCallables)
{
    if (!holdsSomething(context, escapedCallables))
        return;
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID passedByNativeCode = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);
    for (ValueFlowCallKind kind : { ValueFlowCallKind::CallByNativeCode, ValueFlowCallKind::ConstructByNativeCode }) {
        ValueFlowCallGroup group = context.group(escapedCallables, kind);
        context.add(group.otherArguments, passedByNativeCode);
        context.connect(group.result, escapedNode);
        context.connect(group.thrown, escapedNode);
    }
}

static void letNativeCodeTestAgainstBoundFunctions(ValueFlowRuleContext& context, ClimbingSet escapedCallables)
{
    if (!holdsSomething(context, escapedCallables))
        return;
    letNativeCodeTestAgainst(context, { context.read(escapedCallables, ValueFlowCellName { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }), ValueSets::nothing, ValueSets::anything });
}

static void letUnseenCodeCreateRecords(ValueFlowRuleContext& context, ClimbingSet escapedObjects)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    constexpr OptionSet<LayoutFlags> flagsOfOtherLayouts { LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction, LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsNeverAllocated, LayoutFlags::IsWrittenUnderDictionaryType, LayoutFlags::IsDerived, LayoutFlags::IsOfCreatedObject };
    Vector<ValueFlowObjectID, 16> records;
    context.forEachMember(known(context, escapedObjects), [&](ValueFlowObjectID object) {
        LayoutID id = universe.layoutOf(object);
        if (!id || object == universe.instanceOf(id))
            return;
        const Layout& layout = program.at(id);
        if (layout.kind != LayoutKind::Record || layout.constructor || layout.base || layout.flags.containsAny(flagsOfOtherLayouts) || layout.fields.isEmpty())
            return;
        bool holdsValuesAsTheyAre = std::ranges::all_of(layout.fields, [&](const Field& field) {
            const Type& type = program.at(field.declared.expected().predicted());
            return field.key && (type.isAnything() || (!type.anyFunction && type.functions.isEmpty()));
        });
        if (holdsValuesAsTheyAre)
            records.append(universe.instanceOf(id));
    });
    if (records.isEmpty())
        return;
    ValueSetID created = context.maker().of(records.span());
    context.add(context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers), created);
    context.add(context.written(ClimbingSet(created), prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    context.add(ValueFlowProgramNodes::escaped(), created);
}

// Whoever holds as a value a library module that the program asks for by name finds every member of it, under any key, and holds what the members answer.
// A module that only a load under a computed name may answer stays found by name alone.
static void handOutMembersOfModules(ValueFlowRuleContext& context, ClimbingSet modules)
{
    auto handOut = [&](ValueFlowNodeID values) {
        carryOut(context, values);
        context.connect(values, context.nodes().ofHost(ValueFlowHostNode::LibraryModulesHandedOut));
    };
    for (ValueFlowNameKind kind : kindsOfMembers)
        handOut(context.readWhatWasWritten(modules, { kind, 0 }));
    handOut(context.readWhatWasWritten(modules, { ValueFlowNameKind::CopiedFrom, 0 }));

    constexpr std::pair<ValueFlowNameKind, ValueFlowCallKind> callsOfMembers[] {
        { ValueFlowNameKind::EveryProperty, ValueFlowCallKind::CallByNativeCode },
        { ValueFlowNameKind::EveryGetter, ValueFlowCallKind::Get },
    };
    for (auto [kind, call] : callsOfMembers) {
        ClimbingSet members = context.valueWithin(ClimbingSet(context.universe().closures()), context.readWhatWasWritten(modules, { kind, 0 }));
        forEachPartOfCallees(context, members, [&](ClimbingSet callees) {
            ValueFlowCallGroup group = context.group(callees, call);
            context.add(group.thisValue, modules);
            handOut(group.result);
        });
    }
}

static void readBunUnderNamesThatHaveEscaped(ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ClimbingSet names(universe.builtinTables().heap().keysOfMembersOfBun());
    if (!holdsSomething(context, names) || !context.holdsAnyOf(only(context, ValueFlowUniverse::markOfNamedBun()), escapedNode) || !holdsSomething(context, context.membersMissingFrom(escapedNode, names)))
        return;
    ClimbingSet bun = only(context, universe.objectOf(Builtins::Bun));
    context.forEachMember(context.valueWithin(names, escapedNode), [&](ValueFlowObjectID name) {
        context.connect(context.lookup(bun, ValueFlowName::property(context.program(), universe.keyOf(name))), escapedNode);
    });
}

static void runEscape(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ClimbingSet escaped = context.valueOf(escapedNode);
    ClimbingSet arrived = context.newSince(escapedNode);
    bool writesSinceLastRun = isRaisedSinceLastRun(context, ValueFlowHostNode::UnknownCodeWrites);
    bool unseenCodeRunsSinceLastRun = isRaisedSinceLastRun(context, ValueFlowHostNode::UnseenCodeRuns);
    if (isRaisedSinceLastRun(context, ValueFlowHostNode::CodeOutsideImageIsLoaded))
        arrived = context.unite(arrived, context.restrict(escaped, universe.objectsOfLibraryFoundByNameAlone()));

    if (holdsSomething(context, arrived))
        followWhatHasArrived(context, arrived);
    readBunUnderNamesThatHaveEscaped(context);
    if (ClimbingSet constructors = context.newSince(context.masked(context.nodes().ofHost(ValueFlowHostNode::ValuesCarriedOut), universe.constructorsOfLibraryClasses())); holdsSomething(context, constructors)) {
        for (ValueFlowNameKind kind : kindsOfMembers)
            carryOut(context, context.masked(context.readWhatWasWritten(constructors, { kind, 0 }), universe.prototypeObjectsOfLibraryClasses()));
    }
    if (ClimbingSet carriedOut = context.newSince(context.nodes().ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues)); holdsSomething(context, carriedOut)) {
        if (ClimbingSet namespaces = context.restrict(carriedOut, universe.builtinTables().heap().namespacesWhoseHoldersListThem()); holdsSomething(context, namespaces))
            connectEnumerableMembersOf(namespaces, escapedNode, context);
        if (ClimbingSet holdersOfExports = context.restrict(carriedOut, universe.holdersOfLibraryExports()); holdsSomething(context, holdersOfExports))
            context.connect(context.readWhatWasWritten(holdersOfExports, { ValueFlowNameKind::EveryGetter, 0 }), escapedNode);
    }
    // As a receiver, an object that shows only what the library exports is handed to nobody new. Where the program has written to it, the member may be a function of the program's.
    ValueFlowNodeID receiversOfMembersNobodyKnows = context.nodes().ofHost(ValueFlowHostNode::ReceiversOfMembersNobodyKnows);
    context.connect(context.masked(receiversOfMembersNobodyKnows, ValueSets::complement(universe.builtinTables().heap().objectsShowingLibraryExports())), escapedNode);
    context.add(escapedNode, context.valueWithin(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::HoldersOfLibraryExportsThatProgramWrites)), receiversOfMembersNobodyKnows));
    context.add(context.nodes().ofHost(ValueFlowHostNode::LibraryModulesHandedOut), context.valueWithin(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::LibraryModulesRequestedByProgram)), context.nodes().ofHost(ValueFlowHostNode::NamespacesCarriedOutAsValues)));
    if (ClimbingSet modules = context.valueOf(context.nodes().ofHost(ValueFlowHostNode::LibraryModulesHandedOut)); holdsSomething(context, modules))
        handOutMembersOfModules(context, modules);

    // The members of the prototype object of a library class are found by name alone. Whoever lists its own keys has every name.
    if (ClimbingSet listed = context.newSince(context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfLibraryClassesWhoseOwnKeysAreListed)); holdsSomething(context, listed))
        context.connect(context.readWhatWasWritten(listed, { ValueFlowNameKind::EveryProperty, 0 }), escapedNode);

    if (context.holdsAnyOf(known(context, escaped), ValueFlowProgramNodes::lost())) {
        ClimbingSet untrackedObject(universe.untrackedObjectAlone());
        for (ValueFlowNameKind kind : kindsOfMembers)
            context.connect(context.readWhatWasWritten(untrackedObject, { kind, 0 }), escapedNode);
        context.connect(context.readWhatWasWritten(untrackedObject, prototypeLinkName), escapedNode);
    }

    ClimbingSet writable = writesSinceLastRun || unseenCodeRunsSinceLastRun ? escaped : arrived;
    if (mayUnknownCodeWrite(context) && holdsSomething(context, writable))
        letAnythingHappenTo(context, writable);

    if (isRaised(context, ValueFlowHostNode::UnseenCodeRuns))
        letUnseenCodeCreateRecords(context, unseenCodeRunsSinceLastRun ? escaped : arrived);

    letNativeCodeSettlePromises(context, context.valueWithin(ClimbingSet(universe.otherCallables()), escapedNode));
    letNativeCodeCallBoundFunctions(context, context.restrict(arrived, universe.otherCallables()));
    letNativeCodeTestAgainstBoundFunctions(context, context.valueWithin(ClimbingSet(universe.otherCallables()), escapedNode));
    lookUpThroughFamiliesOf(context, arrived);
}

bool hasRuleOfItsOwn(Opcode opcode)
{
    return flowRuleOf(opcode) != FlowRule::General;
}

void noteArraysThatMayGetHoles(ValueFlowRuleContext& context, ClimbingSet objects)
{
    context.add(context.nodes().ofHost(ValueFlowHostNode::ArraysThatMayHaveHoles), context.restrict(objects, context.universe().denseArrayLiterals()));
}

static uint8_t operandsThatMayGetHoles(Opcode opcode)
{
    static const std::array<uint8_t, numberOfOpcodes>& masks = *[] {
        auto result = makeUniqueWithoutFastMallocCheck<std::array<uint8_t, numberOfOpcodes>>();
        result->fill(0);
        for (Opcode member : allMembersOf<Opcode>) {
            if (member == Opcode::AppendElement || member == Opcode::AppendSpread || effectRuleOf(member) == EffectRule::Call)
                continue;
            uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(member) + (hasVariadicOperand(member) ? 1 : 0);
            for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
                if (!operandUsesOf(member, index).containsAny(usesThatMayCreateHoles))
                    continue;
                RELEASE_ASSERT(index < 8);
                (*result)[static_cast<unsigned>(member)] |= 1 << index;
            }
        }
        return result.release();
    }();
    return masks[static_cast<unsigned>(opcode)];
}

static void runOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (uint8_t mask = operandsThatMayGetHoles(statement.opcode)) {
        std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
        for (uint32_t index = 0; index < operands.size(); ++index) {
            if (mask & (1 << std::min(index, numberOfNamedOperandsOf(statement.opcode))))
                noteArraysThatMayGetHoles(context, context.valueOf(operands[index]));
        }
    }
    switch (flowRuleOf(statement.opcode)) {
    case FlowRule::General:
        break;
    case FlowRule::Arithmetic:
        return runArithmetic(statement, context);
    case FlowRule::Await:
        return runAwait(statement, context);
    case FlowRule::Call:
        return runCall(statement, context);
    case FlowRule::ConversionToObject:
        return runConversionToObject(statement, context);
    case FlowRule::Convert:
        return runConvert(statement, context);
    case FlowRule::DefineMethod:
        return runDefineMethod(statement, context);
    case FlowRule::Generator:
        return runGeneratorOperation(statement, context);
    case FlowRule::Heap:
        return runHeapOperation(statement, context);
    case FlowRule::InstanceOf:
        return runInstanceOf(statement, context);
    case FlowRule::Iteration:
        return runIterationOperation(statement, context);
    case FlowRule::Module:
        return runModuleOperation(statement, context);
    case FlowRule::NewClass:
        return runNewClass(statement, context);
    case FlowRule::NewDictionary:
        return runNewDictionary(statement, context);
    case FlowRule::Promise:
        return runPromiseOperation(statement, context);
    case FlowRule::ReadOfPrototypeProperty:
        return runReadOfPrototypeProperty(statement, context);
    case FlowRule::StepOfLibrary:
        return runStepOfLibrary(statement, context);
    case FlowRule::ToPropertyKey:
        return runToPropertyKey(statement, context);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void runRuleOnNodesNamed(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Operation:
        if (context.canBeReached(statement))
            runOperation(statement, context);
        return;
    case ValueFlowStatementKind::CallGroup:
        return runCallGroup(statement, context);
    case ValueFlowStatementKind::ModuleEvaluation:
        return runModuleEvaluation(statement, context);
    case ValueFlowStatementKind::Coroutine:
        return runCoroutine(statement, context);
    case ValueFlowStatementKind::HostRoots:
        return runHostRoots(statement, context);
    case ValueFlowStatementKind::UsedBuiltins:
        return runUsedBuiltins(statement, context);
    case ValueFlowStatementKind::UnseenCode:
        return runUnseenCode(statement, context);
    case ValueFlowStatementKind::ModulesLoadedByName:
        return runModulesLoadedByName(statement, context);
    case ValueFlowStatementKind::Escape:
        return runEscape(statement, context);
    case ValueFlowStatementKind::Lookup:
        switch (context.nameOfKey().kind) {
        case ValueFlowNameKind::OpenedIterators:
            return runOpenedIterators(statement, context);
        case ValueFlowNameKind::IteratedValues:
            return runIteratedValues(statement, context);
        case ValueFlowNameKind::IteratorResults:
            return runIteratorResults(statement, context);
        case ValueFlowNameKind::AwaitedValue:
            return runAwaitedValue(statement, context);
        case ValueFlowNameKind::FoundThroughEscapedInstances:
            return runLookupThroughInstances(statement, context);
        case ValueFlowNameKind::NamespaceExports:
            return runNamespaceExports(statement, context);
        case ValueFlowNameKind::Property:
        case ValueFlowNameKind::Getter:
        case ValueFlowNameKind::Setter:
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::GetterUnderNameOfEnumeration:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::EveryProperty:
        case ValueFlowNameKind::EveryGetter:
        case ValueFlowNameKind::GetterOutsideEnumerations:
        case ValueFlowNameKind::EverySetter:
        case ValueFlowNameKind::PrototypeLink:
        case ValueFlowNameKind::CopiedFrom:
        case ValueFlowNameKind::DeletedMember:
        case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        case ValueFlowNameKind::RedefinedMember:
        case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        case ValueFlowNameKind::PrivateField:
        case ValueFlowNameKind::Slot:
        case ValueFlowNameKind::HiddenSlot:
        case ValueFlowNameKind::InternalFields:
        case ValueFlowNameKind::SteppedValues:
        case ValueFlowNameKind::ReturnedValue:
        case ValueFlowNameKind::SentValues:
        case ValueFlowNameKind::OpenedBy:
        case ValueFlowNameKind::ResolvedPromises:
        case ValueFlowNameKind::RejectedPromises:
        case ValueFlowNameKind::AllocatedAs:
        case ValueFlowNameKind::MemberWritten:
        case ValueFlowNameKind::EveryMemberWritten:
        case ValueFlowNameKind::EveryMemberRead:
        case ValueFlowNameKind::RestArgument:
        case ValueFlowNameKind::AddedNames:
        case ValueFlowNameKind::NamesOfMethods:
        case ValueFlowNameKind::ReceiversOfCalls:
            return runLookup(statement, context);
        }
    case ValueFlowStatementKind::Behavior:
        return runBehavior(statement, context);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runValueFlowRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowInstancedFunctions* instanced = context.instancedFunctions();
    if (!instanced || !instanced->find(statement.function))
        return runRuleOnNodesNamed(statement, context);
    context.makeInstanceOfOtherCalls(statement.function);
    context.forEachMemberInLine(context.unite(context.valueOf(context.markersOf(statement.function)), only(context, ValueFlowInstancedFunctions::markerOfOtherCalls())), [&](ValueFlowObjectID marker) {
        ValueFlowRuleContext::InstanceOfRun instance(context, statement.function, marker);
        runRuleOnNodesNamed(statement, context);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
