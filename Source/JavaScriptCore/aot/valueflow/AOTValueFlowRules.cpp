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
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

ValueSetID unknownValue(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    const ValueFlowUniverse& universe = context.universe();
    context.noteUnknownSource(source);
    // What native code answers without a description may be a Buffer, on which native code calls a method that is written in JavaScript: callLibraryMethodsOfUnknownReceiver().
    if (source == ValueFlowUnknownSource::UndescribedNativeResult) {
        const BuiltinTable& builtins = context.program().builtins();
        if (BuiltinID prototype = builtins.at(BuiltinClasses::Buffer).prototype)
            context.add(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins), context.maker().of(universe.objectOf(prototype)));
    }
    if (context.leavesOut(source))
        return universe.anyPrimitive();
    return context.maker().unite(universe.anyPrimitive(), universe.unknownObjectAlone());
}

void runConvert(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (!statement.result)
        return;
    const ValueFlowOperand& converted = context.operandsOf(statement)[0];
    if (context.program().at(statement.function).at(statement.site).illTypedCause() == IllTypedCause::None) {
        context.connect(converted, statement.result);
        return;
    }

    // The object is taken for what it may not be. It is handed to nobody, so what the analysis has stopped following does not escape.
    ClimbingSet value = context.valueOf(converted);
    context.add(statement.result, without(context, value, ValueFlowUniverse::untrackedObject()));
    if (mayBeUntrackedObject(context, value))
        context.add(statement.result, context.maker().intersect(unknownValue(context, ValueFlowUnknownSource::UncheckedConversionOfUntracked), context.universe().objects()));
}

// An ordinary object without a layout, as Object.create makes one below %Object.prototype%. An operation without a site of its own shares the object that stands for what that built-in makes.
void runNewDictionary(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID site = universe.siteObject(statement);
    ClimbingSet dictionary = only(context, site ? site : universe.resultOf(Builtins::ObjectCreate, 0));
    context.add(context.written(dictionary, prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    if (statement.result)
        context.add(statement.result, dictionary);
}

static constexpr ValueFlowNameKind kindsOfMembers[] { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter };

static bool isRaised(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    return mayBeUnknownObject(context, context.valueOf(context.nodes().ofHost(flag)));
}

static bool isRaisedSinceLastRun(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    return mayBeUnknownObject(context, context.newSince(context.nodes().ofHost(flag)));
}

// Of what a class or a constructor function of the program has made. None for any other object.
static LayoutID layoutOfInstance(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    LayoutID layout = context.universe().layoutOf(object);
    return layout && context.program().at(layout).kind == LayoutKind::Instance ? layout : LayoutID();
}

// What a class or a function of the program has constructed.
static bool isConstructedByProgram(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return layoutOfInstance(context, object) || context.universe().kindOf(object) == ValueFlowObjectKind::ConstructedObject;
}

// A package that stays outside the image is interpreted, and so is every module of the library that it loads and that the image does not hold. Such code calls whatever a module of the library exports.
static bool holdsCodeOutsideImage(const Program& program)
{
    return std::ranges::any_of(program.modules, [](const Module& module) {
        return module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode;
    });
}

// Whatever an escaped object holds has escaped with it, and a function that has escaped is called with operands nobody knows. All of this follows from the object alone, so it is said once, when the object arrives.
static void followWhatHasArrived(ValueFlowRuleContext& context, ClimbingSet arrived)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();

    // The object on which the exports of a module of the library hang is kept and not searched, however it came here: what hangs on it is held and not called. The rules of the host follow what is held.
    ClimbingSet holdersOfExports = context.restrict(arrived, universe.holdersOfLibraryExports());
    if (holdsSomething(context, holdersOfExports) && !holdsCodeOutsideImage(context.program())) {
        context.add(context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode), holdersOfExports);
        context.add(context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects), holdersOfExports);
        arrived = context.restrict(arrived, ValueSets::complement(universe.holdersOfLibraryExports()));
    }

    Vector<ValueFlowObjectID, 64> instances;
    context.forEachMember(known(context, arrived), [&](ValueFlowObjectID object) {
        if (isConstructedByProgram(context, object))
            instances.append(object);
    });
    ClimbingSet others = context.restrict(arrived, ValueSets::complement(context.maker().of(instances.span())));

    for (ValueFlowNameKind kind : kindsOfMembers)
        context.connect(context.read(arrived, { kind, 0 }), escapedNode);
    context.connect(context.read(arrived, { ValueFlowNameKind::CopiedFrom, 0 }), escapedNode);
    // What lies above an instance is found through it and is not handed out with it.
    context.connect(context.masked(context.read(others, prototypeLinkName), ValueSets::complement(universe.builtinTables().heap().objectsThatLetInUnseenCode())), escapedNode);
    context.add(escapedNode, objectsHeldFromCreation(context, others));
    // Whoever holds an entry climbs to the prototype object of its class, and from there to what the program has put above that.
    for (const ValueFlowClassOfEntries& classOfEntries : universe.builtinTables().heap().classesOfEntries()) {
        if (context.meet(arrived, classOfEntries.entries))
            context.add(escapedNode, only(context, classOfEntries.prototype));
    }
    context.mark(arrived, ObjectMutation::ReachNativeCode);

    letNativeCodeStepIterators(context, arrived);
    // Whoever holds the code behind a described entry calls it with what it holds, which has escaped: what the overloads say of such operands the rule of what has escaped says already.
    ValueSetID describedByEntries = universe.builtinTables().closuresThatEntriesDescribe();
    context.forEachMember(context.restrict(arrived, describedByEntries), [&](ValueFlowObjectID closure) {
        enterCodeThatEntryDescribes(closure, context);
    });
    context.forEachMember(context.restrict(arrived, context.maker().intersect(universe.closures(), ValueSets::complement(describedByEntries))), [&](ValueFlowObjectID closure) {
        enterFromNativeCode(context, universe.closureFunction(closure));
    });
}

// All that one class or one constructor function of the program constructs is a family. instanceOf() of the layout stands for it, and where the function has no layout, the object that stands for what it constructs.
static ValueFlowObjectID familyOfInstance(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    if (LayoutID layout = layoutOfInstance(context, object))
        return context.universe().instanceOf(layout);
    return context.universe().kindOf(object) == ValueFlowObjectKind::ConstructedObject ? object : ValueFlowObjectID();
}

// A layout also lists objects that no reached code makes, below classes that nothing has created.
static ValueSetID instancesOfFamily(ValueFlowRuleContext& context, ValueFlowObjectID family)
{
    if (LayoutID layout = context.universe().layoutOf(family))
        return context.universe().objectsWith(layout);
    return context.maker().of(family);
}

// Whoever holds an instance looks up through it what lies on the prototype object of its class and on the objects above that one.
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

    ClimbingSet chain = context.valueOf(context.lookup(context.valueWithin(ClimbingSet(instances), escapedNode), prototypeLinkName));
    if (mayBeUntrackedObject(context, chain))
        context.add(escapedNode, untrackedObject);
    ClimbingSet objectsOfProgram = context.restrict(known(context, chain), ValueSets::complement(universe.builtinObjects()));
    ClimbingSet above = context.restrict(objectsOfProgram, ValueSets::complement(instances));
    // The chain lists the instances themselves. One of them lies above another only where the program has put it there.
    above = context.unite(above, context.restrict(context.valueOf(context.read(ClimbingSet(instances), prototypeLinkName)), instances));
    if (!holdsSomething(context, above))
        return;
    if (unseenCodeRuns)
        context.add(escapedNode, above);
    context.add(context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances), above);

    ClimbingSet found;
    auto take = [&](ValueFlowNodeID members) {
        context.connect(context.masked(members, ValueSets::complement(functions)), escapedNode);
        found = context.unite(found, context.restrict(context.valueOf(members), functions));
    };
    // What was stored through the untracked object may lie on an object that the analysis has stopped following.
    bool mayBeLost = context.holdsAnyOf(above, ValueFlowProgramNodes::lost());
    for (ValueFlowNameKind kind : kindsOfMembers) {
        take(context.read(above, { kind, 0 }));
        if (mayBeLost)
            take(context.read(untrackedObject, { kind, 0 }));
    }
    context.add(constructorsNode, context.restrict(objectsHeldFromCreation(context, above), universe.closures()));
    context.connect(context.masked(context.read(above, constructorName), universe.closures()), constructorsNode);
    if (mayBeLost)
        context.connect(context.masked(context.read(untrackedObject, constructorName), universe.closures()), constructorsNode);

    // A call through a group is one that seen code may make. A function of the library that only native code leads to is no seen code: it escapes, and native code enters it.
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

    ValueFlowCallGroup group = context.group(called, ValueFlowCallKind::CallByNativeCode);
    context.connect(context.masked(escapedNode, instances), group.thisValue);
    // A proxy in front of an instance answers with what the instance inherits, and whoever has asked calls it on the proxy, which is the unknown object. So does a getter or a setter that is found through it.
    ValueFlowObjectID makersOfProxies[] { universe.objectOf(Builtins::Proxy), universe.objectOf(Builtins::ProxyRevocable) };
    if (holdsSomething(context, context.valueWithin(ClimbingSet(context.maker().of(makersOfProxies)), context.nodes().ofHost(ValueFlowHostNode::RunBuiltins))))
        context.add(group.thisValue, universe.unknownObjectAlone());
    context.add(group.otherArguments, passedByNativeCode);
    context.connect(group.result, escapedNode);
    context.connect(group.thrown, escapedNode);
}

// What is found moves, and it moves for one family at a time.
static void runLookupThroughInstances(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    context.newSince(statement.result);
    lookUpThroughInstances(context, ValueFlowObjectID(context.nameOfKey().payload));
}

// The statement of a family is made when the first of its instances arrives: the statement of kind Escape remembers which families it has met.
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

// Nobody reads out of a bound function what it was made of. Whoever holds one calls it, with whatever it likes behind the bound arguments.
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

// It also tests values against one: `value instanceof bound` asks the target in the place of the bound function, for its method under Symbol.hasInstance, which runs on the target, and for its prototype property.
static void letNativeCodeTestAgainstBoundFunctions(ValueFlowRuleContext& context, ClimbingSet escapedCallables)
{
    if (!holdsSomething(context, escapedCallables))
        return;
    // The target has not escaped with the bound function.
    letNativeCodeTestAgainst(context, { context.read(escapedCallables, { ValueFlowNameKind::HiddenSlot, BuiltinSlots::BoundFunctionTarget.index() }), ValueSets::nothing, ValueSets::anything });
}

// The interpreter gives an object literal the layout of a record with the same names (AOT::Interpreter::ObjectLiteralPlans), so that it passes the type tests of compiled code.
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
        bool acceptsEveryValue = std::ranges::all_of(layout.fields, [&](const Field& field) {
            return field.key && program.at(field.declared.type).isAnything();
        });
        if (acceptsEveryValue)
            records.append(universe.instanceOf(id));
    });
    if (records.isEmpty())
        return;
    ValueSetID created = context.maker().of(records.span());
    context.add(context.nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers), created);
    context.add(context.written(ClimbingSet(created), prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    context.add(ValueFlowProgramNodes::escaped(), created);
}

// What has escaped was handed to code that nobody compiles.
void runEscape(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ClimbingSet escaped = context.valueOf(escapedNode);
    ClimbingSet arrived = context.newSince(escapedNode);
    bool writesSinceLastRun = isRaisedSinceLastRun(context, ValueFlowHostNode::UnknownCodeWrites);
    bool unseenCodeRunsSinceLastRun = isRaisedSinceLastRun(context, ValueFlowHostNode::UnseenCodeRuns);

    if (holdsSomething(context, arrived))
        followWhatHasArrived(context, arrived);

    // An object that the analysis has stopped following also holds what was stored through the untracked object. It may be lost after it has escaped, so this is asked in every run.
    if (context.holdsAnyOf(known(context, escaped), ValueFlowProgramNodes::lost())) {
        ClimbingSet untrackedObject(universe.untrackedObjectAlone());
        for (ValueFlowNameKind kind : kindsOfMembers)
            context.connect(context.read(untrackedObject, { kind, 0 }), escapedNode);
        context.connect(context.read(untrackedObject, prototypeLinkName), escapedNode);
    }

    // What such code may do from now on it may do to all that it has been handed so far.
    ClimbingSet writable = writesSinceLastRun || unseenCodeRunsSinceLastRun ? escaped : arrived;
    if (mayUnknownCodeWrite(context) && holdsSomething(context, writable))
        letAnythingHappenTo(context, writable);

    if (isRaised(context, ValueFlowHostNode::UnseenCodeRuns))
        letUnseenCodeCreateRecords(context, unseenCodeRunsSinceLastRun ? escaped : arrived);

    // Which promises a function settles is read from the function, and that moves.
    letNativeCodeSettlePromises(context, context.valueWithin(ClimbingSet(universe.otherCallables()), escapedNode));
    letNativeCodeCallBoundFunctions(context, context.restrict(arrived, universe.otherCallables()));
    letNativeCodeTestAgainstBoundFunctions(context, context.valueWithin(ClimbingSet(universe.otherCallables()), escapedNode));
    lookUpThroughFamiliesOf(context, arrived);
}

// Every operation that has a rule of its own by its opcode. The bodies make a statement for these, for the operations of modules and for those of the heap, and for no other.
#define FOR_EACH_OPERATION_WITH_RULE(v) \
    v(Call, runCall) \
    v(CallList, runCall) \
    v(Construct, runCall) \
    v(ConstructList, runCall) \
    v(SuperConstruct, runCall) \
    v(SuperConstructList, runCall) \
    v(Convert, runConvert) \
    v(NewDictionary, runNewDictionary) \
    v(NewClass, runNewClass) \
    v(DefineMethod, runDefineMethod) \
    v(ResolveHeritage, runReadOfPrototypeProperty) \
    v(GetClassPrototype, runReadOfPrototypeProperty) \
    v(GetConstructPrototype, runReadOfPrototypeProperty) \
    v(IteratorOpen, runIterationOperation) \
    v(IteratorResume, runIterationOperation) \
    v(IteratorClose, runIterationOperation) \
    v(AppendSpread, runIterationOperation) \
    v(StepArrayIterator, runStepOfLibrary) \
    v(MapNext, runStepOfLibrary) \
    v(SetNext, runStepOfLibrary) \
    v(ResumeGenerator, runGeneratorOperation) \
    v(StepGenerator, runGeneratorOperation) \
    v(Await, runAwait) \
    v(CreatePromise, runPromiseOperation) \
    v(ResolvePromise, runPromiseOperation) \
    v(RejectPromise, runPromiseOperation) \
    v(InstanceOf, runInstanceOf) \
    v(Add, runArithmetic) \
    v(Sub, runArithmetic) \
    v(Mul, runArithmetic) \
    v(Div, runArithmetic) \
    v(Mod, runArithmetic) \
    v(Pow, runArithmetic) \
    v(BitAnd, runArithmetic) \
    v(BitOr, runArithmetic) \
    v(BitXor, runArithmetic) \
    v(LShift, runArithmetic) \
    v(RShift, runArithmetic) \
    v(Negate, runArithmetic) \
    v(ToNumeric, runArithmetic) \
    v(Increment, runArithmetic) \
    v(Decrement, runArithmetic) \
    v(BitNot, runArithmetic) \
    v(ToPropertyKey, runToPropertyKey)

bool hasRuleOfItsOwn(Opcode opcode)
{
    switch (opcode) {
#define RETURN_TRUE(opcode, rule) case Opcode::opcode:
    FOR_EACH_OPERATION_WITH_RULE(RETURN_TRUE)
#undef RETURN_TRUE
        return true;
    default:
        return isModuleOperation(opcode) || isHeapOperation(opcode);
    }
}

static void runOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    switch (statement.opcode) {
#define RUN_RULE(opcode, rule) \
    case Opcode::opcode: \
        rule(statement, context); \
        return;
    FOR_EACH_OPERATION_WITH_RULE(RUN_RULE)
#undef RUN_RULE
    default:
        if (isModuleOperation(statement.opcode))
            runModuleOperation(statement, context);
        else {
            RELEASE_ASSERT(isHeapOperation(statement.opcode));
            runHeapOperation(statement, context);
        }
        return;
    }
}

static void runEntryChecks(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& function = context.program().at(statement.function);
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    Vector<ValueFlowNodeID, 8> testedWhereSomeTestMayFail;
    bool someInstanceMayHaveOtherLayout = false;
    for (const ValueFlowOperand& turnedAway : operands) {
        bool mayHaveOtherLayout = mayHaveOtherLayoutThanItsClass(context, context.valueOf(ValueFlowOperand { turnedAway.node, turnedAway.constant, ValueSets::anything }));
        someInstanceMayHaveOtherLayout |= mayHaveOtherLayout;
        if ((mayHaveOtherLayout || context.meet(context.valueOf(turnedAway), ValueSets::anything)) && !testedWhereSomeTestMayFail.contains(turnedAway.node))
            testedWhereSomeTestMayFail.append(turnedAway.node);
    }
    if (testedWhereSomeTestMayFail.isEmpty())
        return;

    FunctionID genericVersion = context.universe().genericVersionOf(statement.function);
    context.reach(genericVersion);
    if (function.role != FunctionRole::EntryChecks) {
        // Behind a check that has failed in the typed body, generic code goes on with what the typed body was handed, and returns to the caller of the typed body. The generic version, run from its head, covers what that code does.
        for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index)
            context.connect(nodes.entryParameter(statement.function, index), nodes.entryParameter(genericVersion, index));
        for (unsigned index = 0; index < function.results.size(); ++index)
            context.connect(nodes.result(genericVersion, index), nodes.result(statement.function, index));
        if (const auto* summary = nodes.resultSummaryOf(statement.function))
            context.connect(nodes.result(genericVersion, 0), summary->ownResult);
        context.connect(nodes.thrown(genericVersion), nodes.thrown(statement.function));
        if (function.isUnsplitCoroutine() && function.has(FunctionFlags::IsGenerator))
            context.connect(nodes.yielded(genericVersion), nodes.yielded(statement.function));
        return;
    }

    // The generic version gets a parameter as it came where the test of another parameter may have failed. Where only its own tests may fail, it gets what they turn away.
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(statement.function, index);
        bool testOfAnotherMayFail = someInstanceMayHaveOtherLayout || testedWhereSomeTestMayFail.size() > 1 || testedWhereSomeTestMayFail[0] != parameter;
        if (testOfAnotherMayFail) {
            context.connect(parameter, nodes.entryParameter(genericVersion, index));
            continue;
        }
        for (const ValueFlowOperand& turnedAway : operands) {
            if (turnedAway.node == parameter)
                context.connect(context.masked(parameter, turnedAway.mask, turnedAway.maskKind), nodes.entryParameter(genericVersion, index));
        }
    }
}

static void runRuleOnNodesNamed(const ValueFlowStatement&, ValueFlowRuleContext&);

void runValueFlowRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowInstancedFunctions* instanced = context.instancedFunctions();
    if (!instanced || !instanced->find(statement.function)) {
        runRuleOnNodesNamed(statement, context);
        return;
    }
    // A direct call is edges alone, which end in every instance: the instance of the other calls is there as soon as a statement of the body runs.
    context.makeInstanceOfOtherCalls(statement.function);
    context.forEachMember(context.unite(context.valueOf(context.markersOf(statement.function)), only(context, ValueFlowInstancedFunctions::markerOfOtherCalls())), [&](ValueFlowObjectID marker) {
        ValueFlowRuleContext::InstanceOfRun instance(context, statement.function, marker);
        runRuleOnNodesNamed(statement, context);
    });
}

static void runRuleOnNodesNamed(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Operation:
        runOperation(statement, context);
        return;
    case ValueFlowStatementKind::CallGroup:
        runCallGroup(statement, context);
        return;
    case ValueFlowStatementKind::ModuleEvaluation:
        runModuleEvaluation(statement, context);
        return;
    case ValueFlowStatementKind::Coroutine:
        runCoroutine(statement, context);
        return;
    case ValueFlowStatementKind::EntryChecks:
        runEntryChecks(statement, context);
        return;
    case ValueFlowStatementKind::HostRoots:
        runHostRoots(statement, context);
        return;
    case ValueFlowStatementKind::UsedBuiltins:
        runUsedBuiltins(statement, context);
        return;
    case ValueFlowStatementKind::UnseenCode:
        runUnseenCode(statement, context);
        return;
    case ValueFlowStatementKind::ModulesLoadedByName:
        runModulesLoadedByName(statement, context);
        return;
    case ValueFlowStatementKind::Escape:
        runEscape(statement, context);
        return;
    case ValueFlowStatementKind::Lookup:
        switch (context.nameOfKey().kind) {
        case ValueFlowNameKind::OpenedIterators:
            runOpenedIterators(statement, context);
            return;
        case ValueFlowNameKind::IteratedValues:
            runIteratedValues(statement, context);
            return;
        case ValueFlowNameKind::IteratorResults:
            runIteratorResults(statement, context);
            return;
        case ValueFlowNameKind::AwaitedValue:
            runAwaitedValue(statement, context);
            return;
        case ValueFlowNameKind::FoundThroughEscapedInstances:
            runLookupThroughInstances(statement, context);
            return;
        default:
            runLookup(statement, context);
            return;
        }
    case ValueFlowStatementKind::Behavior:
        runBehavior(statement, context);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
