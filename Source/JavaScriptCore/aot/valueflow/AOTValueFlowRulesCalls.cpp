/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTKnownClosures.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueFlowName prototypeProperty(ValueFlowRuleContext& context)
{
    return ValueFlowName::property(context.program().keyOf(BuiltinNames::prototype));
}

static bool constructs(Opcode opcode)
{
    return opcode != Opcode::Call && opcode != Opcode::CallList;
}

static ClimbingSet unknownObject(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return context.restrict(ClimbingSet(unknownValue(context, source)), context.universe().objects());
}

static PropertyKeyID keyReadFromReceiver(const Program&, const ValueFlowStatement&);

// The entry of a built-in and a bound function are applied with the operands of this call, whatever else may be called beside them.
static void callListedBuiltins(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees, ValueFlowCallKind kind)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet builtins = context.restrict(callees, universe.builtinFunctions());
    PropertyKeyID key = context.meet(builtins, ValueSets::anything) ? keyReadFromReceiver(context.program(), statement) : PropertyKeyID();
    ValueFlowObjectsToldApart receivers;
    Vector<ClimbingSet, 16> answers;
    if (key) {
        ClimbingSet all = context.valueOf(context.operandsOf(statement)[1]);
        receivers = tellObjectsApart(context, context.restrict(all, universe.knownObjects()));
        // Above a primitive lies the prototype object of its wrapper. On undefined and null the read has thrown.
        context.forEachMember(context.restrict(all, universe.anyPrimitive()), [&](ValueFlowObjectID kindOfPrimitive) {
            receivers.groups.append(context.maker().of(kindOfPrimitive));
        });
        for (ValueSetID group : receivers.groups)
            answers.append(context.valueOf(context.lookup(ClimbingSet(group), ValueFlowName::property(key))));
    }
    context.forEachMember(builtins, [&](ValueFlowObjectID callee) {
        if (!key) {
            applyBuiltin(universe.builtinOf(callee), kind, statement, context);
            return;
        }
        ValueSetID found = context.maker().unite(ValueSets::complement(context.maker().unite(universe.knownObjects(), universe.anyPrimitive())), receivers.rest);
        for (size_t index = 0; index < answers.size(); ++index) {
            if (context.contains(answers[index], callee) || context.meet(answers[index], universe.placeholdersAlone()))
                found = context.maker().unite(found, receivers.groups[index]);
        }
        applyBuiltin(universe.builtinOf(callee), kind, statement, context, found);
    });
    if (context.meet(callees, universe.otherCallables()))
        callOtherCallables(context.restrict(callees, universe.otherCallables()), kind, statement, context);
}

// The receivers of a call that every callee is passed: what the mask lets through of the operand, and the objects.
struct ReceiversOfEveryCallee {
    ValueSetID mask { ValueSets::anything };
    ValueSetID objects { ValueSets::nothing };
};

// A call without a receiver passes undefined.
static void passThis(ValueFlowRuleContext& context, const ValueFlowStatement& statement, const ValueFlowOperand& receiver, const ReceiversOfEveryCallee& passedReceivers, ValueFlowNodeID thisValue)
{
    if (!context.program().at(statement.function).at(statement.site).operand(1)) {
        context.add(thisValue, context.universe().undefinedValue());
        return;
    }
    if (passedReceivers.mask == ValueSets::anything) {
        context.connect(receiver, thisValue);
        return;
    }
    // The mask of an operand may put the objects of a family in the place of a placeholder, so it does not make one mask with another.
    if (receiver.node)
        context.connect(context.masked(context.masked(receiver.node, receiver.mask, receiver.maskKind), passedReceivers.mask), thisValue);
    context.add(thisValue, context.restrict(context.valueOf(ValueFlowOperand { { }, receiver.constant, receiver.mask, receiver.maskKind }), passedReceivers.mask));
    context.add(thisValue, passedReceivers.objects);
}

void passNoFurtherArguments(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, unsigned numberOfArguments, unsigned numberOfPositionsTaken)
{
    for (unsigned index = numberOfArguments; index < numberOfPositionsTaken; ++index)
        context.add(group.argument(index), context.universe().undefinedValue());
}

// No callee looks at a position from this one on. The callees are functions of the program that a group enters.
static unsigned numberOfPositionsTakenBy(ValueFlowRuleContext& context, ClimbingSet closures)
{
    AOT_THRESHOLD(unsigned, mostCalleesLookedAt, 4);
    const ValueFlowUniverse& universe = context.universe();
    unsigned numberOfCallees = 0;
    unsigned positions = 0;
    context.forEachMember(closures, [&](ValueFlowObjectID closure) {
        if (positions == numberOfGroupedArguments)
            return;
        FunctionID function = universe.closureFunction(closure);
        if (!function || ++numberOfCallees > mostCalleesLookedAt || context.sets().contains(universe.builtinTables().closuresThatEntriesDescribe(), closure)) {
            positions = numberOfGroupedArguments;
            return;
        }
        const Function& callee = context.program().at(function);
        if (callee.has(FunctionFlags::HasRestParameter) || callee.hiddenRestParameterIndex())
            positions = numberOfGroupedArguments;
        else
            positions = std::min(numberOfGroupedArguments, std::max(positions, callee.numberOfPositionalParameters()));
    });
    return positions;
}

ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext& context, ClimbingSet knownObjects)
{
    const ValueFlowUniverse& universe = context.universe();
    uint64_t mostGroups = context.mostMembersToldApart();
    struct Member {
        uint64_t group;
        ValueFlowObjectID object;
    };
    Vector<Member, 64> members;
    context.forEachMember(knownObjects, [&](ValueFlowObjectID object) {
        members.append(Member { 0, object });
    });

    ValueFlowObjectsToldApart result;
    if (members.size() <= mostGroups) {
        for (const Member& member : members)
            result.groups.append(context.maker().of(member.object));
        return result;
    }

    for (Member& member : members) {
        if (LayoutID layout = universe.layoutOf(member.object))
            member.group = layout.index();
        else if (BuiltinClassID builtinClass = universe.classOf(member.object))
            member.group = 1ull << 32 | builtinClass.index();
        else
            member.group = 2ull << 32 | member.object.index();
    }
    AOT::stableSort(members, { }, &Member::group);
    Vector<ValueFlowObjectID, 64> objects;
    for (size_t first = 0; first < members.size();) {
        bool isRest = result.groups.size() >= mostGroups;
        size_t end = first;
        objects.shrink(0);
        while (end < members.size() && (isRest || members[end].group == members[first].group))
            objects.append(members[end++].object);
        ValueSetID set = context.maker().of(objects.span());
        if (isRest)
            result.rest = set;
        else
            result.groups.append(set);
        first = end;
    }
    if (result.rest != ValueSets::nothing)
        context.noteMembersNotToldApart();
    return result;
}

static bool constructsClass(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

// One group holds what the callees answer to all who construct them.
static ValueSetID objectsAnsweredTo(ValueFlowRuleContext& context, ClimbingSet callees, ClimbingSet newTargets)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet chain = context.valueOf(context.lookup(callees, prototypeLinkName));
    if (context.meet(chain, universe.placeholdersAlone()) || context.meet(chain, universe.otherCallables()))
        return ValueSets::anything;
    if (context.meet(context.restrict(chain, universe.closures()), ValueSets::complement(universe.constructorsAnsweringOnlyWhatTheyConstruct())))
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 8> instances;
    bool mayBeOtherThanClass = false;
    context.forEachMember(newTargets, [&](ValueFlowObjectID newTarget) {
        FunctionID constructor = universe.closureFunction(newTarget);
        if (constructor && constructsClass(program.at(constructor)))
            instances.append(universe.instanceOf(program.at(constructor).constructedClass));
        else
            mayBeOtherThanClass = true;
    });
    if (mayBeOtherThanClass)
        return ValueSets::anything;
    return context.maker().unite(ValueSets::complement(universe.instancesOfClasses()), context.maker().of(instances.span()));
}

static bool throwsInsteadOfRunning(const Function& callee, bool isConstruction);

// What the call gets back from a function that returns some of its parameters as they came: what else the function returns, and the call's own operands in the place of those parameters.
static void takeResultThroughSummary(ValueFlowRuleContext& context, const ValueFlowStatement& statement, FunctionID function, const ValueFlowProgramNodes::ResultSummary& summary)
{
    const Program& program = context.program();
    const Function& callee = program.at(function);
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    std::span<const ValueFlowOperand> arguments = operands.subspan(2);
    context.connect(summary.ownResult, statement.result);
    for (const auto& passed : summary.passedParameters) {
        const ValueFlowOperand* operand = nullptr;
        if (callee.entryParameterKind(passed.entryParameter) == EntryParameter::This) {
            if (program.at(statement.function).at(statement.site).operand(1))
                operand = &operands[1];
        } else if (size_t position = passed.entryParameter - callee.firstParameterIndex(); position < arguments.size())
            operand = &arguments[position];
        if (!operand) {
            context.connect(context.masked(context.holding(ClimbingSet(context.universe().undefinedValue())), passed.mask, passed.maskKind), statement.result);
            continue;
        }
        // The mask of an operand may put the objects of a family in the place of a placeholder, so it does not make one mask with another.
        if (operand->node)
            context.connect(context.masked(context.masked(operand->node, operand->mask, operand->maskKind), passed.mask, passed.maskKind), statement.result);
        if (operand->constant)
            context.connect(context.masked(context.holding(context.valueOf(ValueFlowOperand { { }, operand->constant, operand->mask, operand->maskKind })), passed.mask, passed.maskKind), statement.result);
    }
}

// A call that knows its callees one by one takes from each what that callee gives to this call, where that is less than what the callee gives to all its callers together.
static bool takesResultOfEachCallee(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet callees)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    if (passesList(statement.opcode))
        return false;

    Vector<FunctionID, 8> answering;
    bool answer = true;
    bool gainsSomething = false;
    context.forEachMember(callees, [&](ValueFlowObjectID closure) {
        FunctionID function = universe.closureFunction(closure);
        if (!answer || !function || answering.size() >= context.mostMembersToldApart()) {
            answer = false;
            return;
        }
        if (throwsInsteadOfRunning(program.at(function), false))
            return;
        gainsSomething |= !!nodes.resultSummaryOf(function);
        answering.append(function);
    });
    if (!answer || !gainsSomething)
        return false;

    for (FunctionID function : answering) {
        if (program.at(function).isUnsplitCoroutine())
            context.add(statement.result, only(context, universe.coroutineResultOf(function)));
        else if (const auto* summary = nodes.resultSummaryOf(function))
            takeResultThroughSummary(context, statement, function, *summary);
        else if (!program.at(function).results.isEmpty())
            context.connect(nodes.result(function, 0), statement.result);
    }
    return true;
}

static constexpr ValueFlowName restArgument(unsigned index)
{
    return { ValueFlowNameKind::RestArgument, index };
}

static constexpr ValueFlowName laterRestArguments = restArgument(numberOfGroupedArguments);
static constexpr ValueFlowName otherRestArguments = restArgument(numberOfGroupedArguments + 1);

// The call into which the function spreads the array of its rest parameter, where it does nothing else with the array: `constructor(...args) { super(...args) }`.
static ValueID findCallThatAloneTakesRestArray(const Function& function)
{
    if (!function.has(FunctionFlags::HasRestParameter) || !function.hasBody())
        return { };
    ValueID rest = function.at(entryBlock).parameters[function.firstParameterIndex() + function.numberOfPositionalParameters()];
    auto isRestArray = [&](ValueID id) {
        return id && definitionThroughSameValues(function, id) == rest;
    };
    constexpr uint32_t indexOfList = 2;
    ValueID call;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isRestArray(id))
                continue;
            const Value& value = function.at(id);
            for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
                if (!isRestArray(value.operand(index)))
                    continue;
                if (call || !passesList(value.opcode) || index != indexOfList)
                    return { };
                call = id;
            }
        }
        bool isUsedOutsideValues = false;
        block.forEachChildOutsideValues([&](ValueID child) {
            isUsedOutsideValues |= isRestArray(child);
        });
        if (isUsedOutsideValues)
            return { };
    }
    return call;
}

// What passArguments() has put down for such a call goes on to the same positions. False for any other call.
static bool passRestArrayByPosition(ValueFlowRuleContext& context, const ValueFlowStatement& statement, const ValueFlowCallGroup& group)
{
    const Function& function = context.program().at(statement.function);
    if (findCallThatAloneTakesRestArray(function) != statement.site)
        return false;
    ClimbingSet rest = only(context, context.universe().restArrayOf(statement.function));
    unsigned numberOfKnownPositions = numberOfGroupedArguments - std::min(numberOfGroupedArguments, function.numberOfPositionalParameters());
    for (unsigned index = 0; index < numberOfKnownPositions; ++index)
        context.connect(context.read(rest, restArgument(index)), group.argument(index));
    ValueFlowNodeID later = context.read(rest, laterRestArguments);
    for (unsigned index = numberOfKnownPositions; index < numberOfGroupedArguments; ++index) {
        context.connect(later, group.argument(index));
        context.add(group.argument(index), context.universe().undefinedValue());
    }
    context.connect(later, group.laterArguments);
    context.connect(context.read(rest, otherRestArguments), group.otherArguments);
    return true;
}

// The operands of all six operations: the callee, then this or new.target, then the arguments or the list of them. They go into the group of the callees, and what the callees answer and throw comes out of it.
static void passOperands(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet callees, ValueFlowCallKind kind, const ReceiversOfEveryCallee& passedReceivers = { }, ValueFlowCalleeOrigin origin = ValueFlowCalleeOrigin::Value)
{
    // Without the operand the callee is new.target: each callee its own, and none that of another.
    if (kind == ValueFlowCallKind::Construct && !context.program().at(statement.function).at(statement.site).operand(1)) {
        Vector<ValueFlowObjectID, 8> members;
        context.forEachMember(callees, [&](ValueFlowObjectID callee) {
            if (members.size() <= context.mostMembersToldApart())
                members.append(callee);
        });
        if (members.size() > 1 && members.size() <= context.mostMembersToldApart()) {
            for (ValueFlowObjectID callee : members)
                passOperands(context, statement, only(context, callee), kind, passedReceivers, origin);
            return;
        }
    }
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    std::span<const ValueFlowOperand> arguments = operands.subspan(2);
    ValueFlowCallGroup group = context.group(callees, kind, origin);
    unsigned numberOfPositionsTaken = numberOfPositionsTakenBy(context, callees);
    ValueSetID answeredObjects = ValueSets::anything;
    if (kind == ValueFlowCallKind::Call)
        passThis(context, statement, operands[1], passedReceivers, group.thisValue);
    else if (context.program().at(statement.function).at(statement.site).operand(1)) {
        context.connect(operands[1], group.newTarget);
        context.connect(operands[1], context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
        answeredObjects = objectsAnsweredTo(context, callees, context.valueOf(operands[1]));
    } else {
        // Without the operand the callee is new.target.
        context.add(group.newTarget, callees);
        answeredObjects = objectsAnsweredTo(context, callees, callees);
    }
    if (passesList(statement.opcode)) {
        ClimbingSet lists = context.valueOf(arguments[0]);
        bool passesRestArray = passRestArrayByPosition(context, statement, group);
        if (passesRestArray)
            lists = context.restrict(lists, ValueSets::complement(context.maker().of(context.universe().restArrayOf(statement.function))));
        if (!passesRestArray || context.meet(lists, ValueSets::anything)) {
            context.connect(context.lookup(lists, elementsName), group.otherArguments);
            passNoFurtherArguments(context, group, 0, numberOfPositionsTaken);
        }
    } else {
        for (unsigned index = 0; index < arguments.size(); ++index)
            context.connect(arguments[index], group.argumentAt(index));
        passNoFurtherArguments(context, group, arguments.size(), numberOfPositionsTaken);
    }
    if (statement.result && (kind != ValueFlowCallKind::Call || !takesResultOfEachCallee(context, statement, callees)))
        context.connect(context.masked(group.result, answeredObjects), statement.result);
    if (statement.thrown)
        context.connect(group.thrown, statement.thrown);
}

// A class is not called and a function that is no constructor is not constructed: the run time throws in front of the body.
static bool throwsInsteadOfRunning(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.constructorForm() != ConstructorForm::NotConstructor && callee.kind != FunctionKind::Plain;
}

// Where a function takes `this`. None for a function that does not look at it.
static ValueFlowNodeID thisParameterOf(ValueFlowRuleContext& context, FunctionID function)
{
    std::optional<unsigned> index = context.program().at(function).thisParameterIndex();
    return index ? context.nodes().entryParameter(function, *index) : ValueFlowNodeID();
}

// `o.m(..)`: the callee is what the read of the key from the receiver has answered. None for any other call: `super.m()` names a key too and reads from another object than the one it passes as `this`.
static PropertyKeyID keyReadFromReceiver(const Program& program, const ValueFlowStatement& statement)
{
    if (constructs(statement.opcode))
        return { };
    const Function& function = program.at(statement.function);
    const Value& call = function.at(statement.site);
    PropertyKeyID key = call.calleeKey();
    ValueID receiver = call.thisValueOperand();
    if (!key || !receiver || program.isIndexLike(key))
        return { };
    const Value& read = function.at(definitionThroughSameValues(function, call.calleeOperand()));
    if (read.opcode != Opcode::PropGet || read.isFreeName() || read.key() != key)
        return { };
    return definitionThroughSameValues(function, read.objectOperand()) == definitionThroughSameValues(function, receiver) ? key : PropertyKeyID();
}

ValueID definitionThroughTests(const Function& function, ValueID id)
{
    AOT_THRESHOLD(unsigned, mostTests, 4);
    for (unsigned steps = 0; steps < mostTests; ++steps) {
        id = definitionThroughSameValues(function, id);
        if (function.at(id).opcode != Opcode::Parameter)
            return id;
        // No edge leads to the entry block.
        if (function.at(entryBlock).parameters.contains(id))
            return id;
        BlockID owner;
        size_t parameterIndex = notFound;
        for (uint32_t block = 0; block < function.blocks.size() && !owner; ++block) {
            parameterIndex = function.blocks[block].parameters.find(id);
            if (parameterIndex != notFound)
                owner = BlockID(block);
        }
        if (!owner)
            return id;
        unsigned numberOfEdges = 0;
        ValueID passed;
        for (uint32_t from = 0; from < function.blocks.size(); ++from) {
            const auto& targets = function.blocks[from].terminator.targets;
            for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
                const Target& target = targets[targetIndex];
                if (target.block != owner)
                    continue;
                // What comes over two edges is no one value.
                if (++numberOfEdges > 1)
                    return id;
                passed = parameterIndex < target.arguments.size() ? target.arguments[parameterIndex]
                    : operandNarrowedOnEdge(function, { BlockID(from), targetIndex, static_cast<uint32_t>(parameterIndex - target.arguments.size()) });
            }
        }
        if (numberOfEdges != 1 || !passed)
            return id;
        id = passed;
    }
    return id;
}

// Also `method = o.m; .. method.call(o)` as the library spells the protocols of the language: the member is read once, tested, and called on the object that it was read from.
static PropertyKeyID keyOfMemberCalledOnItsHolder(const Program& program, const ValueFlowStatement& statement)
{
    if (PropertyKeyID key = keyReadFromReceiver(program, statement))
        return key;
    if (constructs(statement.opcode))
        return { };
    const Function& function = program.at(statement.function);
    const Value& call = function.at(statement.site);
    ValueID receiver = call.thisValueOperand();
    if (!receiver)
        return { };
    const Value& read = function.at(definitionThroughTests(function, call.calleeOperand()));
    if (read.opcode != Opcode::PropGet || read.isFreeName() || program.isIndexLike(read.key()))
        return { };
    return definitionThroughTests(function, read.objectOperand()) == definitionThroughTests(function, receiver) ? read.key() : PropertyKeyID();
}

// In `o.m(..)` a function runs with a receiver as `this` only where the read of `m` from that receiver answers the function: a method of one class never sees an instance of a class that overrides it.
static ReceiversOfEveryCallee passKnownReceiversToTheirCallees(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet closures)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    PropertyKeyID key = keyReadFromReceiver(program, statement);
    if (!key)
        return { };

    ValueFlowObjectID undefinedAndNull[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) };
    ValueSetID neverReceivers = context.maker().of(undefinedAndNull);
    ValueFlowObjectsToldApart receivers = tellObjectsApart(context, context.restrict(context.valueOf(context.operandsOf(statement)[1]), universe.knownObjects()));
    for (ValueSetID group : receivers.groups) {
        ClimbingSet answered = context.valueOf(context.lookup(ClimbingSet(group), ValueFlowName::property(key)));
        context.forEachMember(context.restrict(answered, universe.closures()), [&](ValueFlowObjectID closure) {
            if (!context.contains(closures, closure))
                return;
            FunctionID function = universe.closureFunction(closure);
            ValueFlowNodeID thisParameter = thisParameterOf(context, function);
            if (!thisParameter || throwsInsteadOfRunning(program.at(function), false))
                return;
            context.add(thisParameter, group);
            // A function that returns `this` gives the group of the callees back what the group has passed: see enter(). What went past the group comes back past it.
            const auto* summary = statement.result ? context.nodes().resultSummaryOf(function) : nullptr;
            if (!summary || program.at(function).isUnsplitCoroutine())
                return;
            for (const auto& passed : summary->passedParameters) {
                if (program.at(function).entryParameterKind(passed.entryParameter) == EntryParameter::This)
                    context.connect(context.masked(context.holding(ClimbingSet(group)), passed.mask, passed.maskKind), statement.result);
            }
        });
    }
    return { ValueSets::complement(context.maker().unite(universe.knownObjects(), neverReceivers)), receivers.rest };
}

// The operands go into the group of the placeholder as into any group. The rule of that group says what the placeholder stands for.
static void callPlaceholder(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ValueFlowObjectID placeholder, ValueFlowCallKind kind)
{
    ValueFlowCalleeOrigin origin = isCodeBehindDeclaredEntries(context.program(), statement.function) ? ValueFlowCalleeOrigin::OperandOfAppliedEntry : ValueFlowCalleeOrigin::Value;
    passOperands(context, statement, only(context, placeholder), kind, { }, origin);
}

// One function of the library runs for all calls of a built-in, so it is entered with operands nobody knows, and the callback that it calls is one of them.
static void callOperandOfDescribedCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    for (const ValueFlowOperand& operand : context.operandsOf(statement).subspan(1))
        context.connect(operand, ValueFlowProgramNodes::escaped());
    ValueSetID anything = unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee);
    if (statement.result)
        context.add(statement.result, anything);
    if (statement.thrown)
        context.add(statement.thrown, anything);
}

ClimbingSet escapedMembersUnder(ValueFlowRuleContext& context, PropertyKeyID key)
{
    return context.valueWithin(ClimbingSet(context.universe().builtinTables().heap().under(key).membersOfEngineAndLibrary), ValueFlowProgramNodes::escaped());
}

// On an object that only the program has in its hands, and above it, lies what the program has stored, under whatever name it likes: `table.run = unknown[key]`.
static bool findsValueNobodyKnowsThatProgramHasStored(ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID key)
{
    const ValueFlowUniverse& universe = context.universe();
    // What a built-in has made holds what native code has put there.
    ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(context.maker().unite(universe.builtinObjects(), universe.objectsThatMayBeAnything())));
    ClimbingSet start = context.restrict(receivers, objectsOfProgram);
    if (!context.meet(start, ValueSets::anything))
        return false;
    ClimbingSet chain = context.unite(start, context.restrict(context.valueOf(context.lookup(start, prototypeLinkName)), objectsOfProgram));
    ClimbingSet inHandsOfProgram = context.membersMissingFrom(ValueFlowProgramNodes::escaped(), chain);
    return context.contains(context.valueOf(context.read(inHandsOfProgram, ValueFlowName::property(key))), ValueFlowUniverse::unknownObject());
}

// `x.key(..)` where nobody knows what the read of the key has answered.
static void callMemberNobodyKnows(const ValueFlowStatement& statement, ValueFlowRuleContext& context, PropertyKeyID key)
{
    context.forEachMember(context.restrict(escapedMembersUnder(context, key), context.universe().builtinFunctions()), [&](ValueFlowObjectID member) {
        applyBuiltinFoundThroughUnknownObject(context.universe().builtinOf(member), statement, context);
    });

    letUnknownCodeWrite(context);
    callOperandOfDescribedCall(statement, context);
}

// A call that passes constants alone where its callee takes a key enters the instance of each of them, with that constant alone as the key. Answers the callees that are left: they are entered like any function.
static ClimbingSet passOperandsToInstances(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet closures, ValueFlowCallKind kind)
{
    const ValueFlowInstancedFunctions* instanced = context.instancedFunctions();
    if (!instanced || !context.meet(closures, instanced->closures()) || passesList(statement.opcode))
        return closures;

    const ValueFlowUniverse& universe = context.universe();
    auto numberOfMembers = [&](ClimbingSet set) {
        unsigned number = 0;
        context.forEachMember(set, [&](ValueFlowObjectID) {
            ++number;
        });
        return number;
    };
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    Vector<ValueFlowObjectID, 4> entered;
    context.forEachMember(context.restrict(closures, instanced->closures()), [&](ValueFlowObjectID closure) {
        FunctionID function = universe.closureFunction(closure);
        size_t index = 2 + instanced->find(function)->positionOfKey;
        if (index >= operands.size())
            return;
        ClimbingSet keys = context.valueOf(operands[index]);
        if (context.meet(keys, ValueSets::complement(universe.keys())) || numberOfMembers(keys) > ValueFlowInstancedFunctions::mostMarkersOfCall)
            return;
        ClimbingSet markers = context.valueOf(context.markersOf(function));
        bool mayMakeInstances = numberOfMembers(markers) < ValueFlowInstancedFunctions::mostInstances;
        bool hasInstanceForEachKey = true;
        context.forEachMember(keys, [&](ValueFlowObjectID key) {
            hasInstanceForEachKey &= context.contains(markers, key);
        });
        if (!mayMakeInstances && !hasInstanceForEachKey)
            return;

        // Until a key arrives the call does not happen.
        entered.append(closure);
        context.forEachMember(keys, [&](ValueFlowObjectID key) {
            Vector<ValueFlowOperand, 8> operandsOfInstance(operands);
            operandsOfInstance[index] = ValueFlowOperand { { }, context.maker().of(key), { } };
            ValueFlowRuleContext::OperandsInPlace inPlace(context, operandsOfInstance.span());
            ValueFlowRuleContext::InstanceEntered instance(context, key);
            passOperands(context, statement, only(context, closure), kind);
        });
    });
    return context.restrict(closures, ValueSets::complement(context.maker().of(entered.span())));
}

// `await Promise.resolve(<a built-in>)`, which is how import("bun") is spelled: the built-in. A pattern tests what it takes apart, so the way leads through tests.
static ValueID builtinResolvedAndAwaited(const Program& program, const Function& function, ValueID id)
{
    const Value& awaited = function.at(id);
    if (awaited.opcode != Opcode::Await)
        return { };
    const Value& resolved = function.at(definitionThroughSameValues(function, awaited.operand(0)));
    if (resolved.opcode != Opcode::Call || resolved.numberOfOperands() != 3 || !resolved.operand(2))
        return { };
    BuiltinID callee = entryNamedByOperations(program, function, resolved.calleeOperand());
    if (!callee || program.builtins().canonicalID(callee) != program.builtins().canonicalID(Builtins::PromiseResolve))
        return { };
    ValueID holder = definitionThroughSameValues(function, resolved.operand(2));
    return function.at(holder).opcode == Opcode::Builtin ? holder : ValueID();
}

BuiltinID entryNamedByOperations(const Program& program, const Function& function, ValueID value)
{
    constexpr unsigned mostReads = 4;
    Vector<PropertyKeyID, mostReads> keys;
    auto followReads = [&](NOESCAPE const Invocable<ValueID(ValueID)> auto& definitionOf) {
        keys.shrink(0);
        ValueID id = definitionOf(value);
        while (function.at(id).opcode == Opcode::PropGet && keys.size() < mostReads) {
            keys.append(function.at(id).key());
            id = definitionOf(function.at(id).objectOperand());
        }
        return id;
    };
    ValueID id = followReads([&](ValueID read) {
        return definitionThroughSameValues(function, read);
    });
    if (function.at(id).opcode != Opcode::Builtin) {
        // The walk through tests goes over every block, so it is for a read that may come to something: under the name of a member of Bun, of what a block or an Await hands on.
        const BuiltinTable& builtins = program.builtins();
        Opcode last = function.at(id).opcode;
        if (!function.has(FunctionFlags::IsAsync) || (last != Opcode::Parameter && last != Opcode::Await) || !builtins.contains(Builtins::Bun))
            return { };
        if (!keys.isEmpty() && !findOwnMember(program, builtins.canonicalID(Builtins::Bun), keys.last()))
            return { };
        if (keys.isEmpty() && function.at(entryBlock).parameters.contains(id))
            return { };
        id = builtinResolvedAndAwaited(program, function, followReads([&](ValueID read) {
            return definitionThroughTests(function, read);
        }));
        if (!id)
            return { };
    }
    BuiltinID entry = function.at(id).builtin();
    for (PropertyKeyID key : keys | std::views::reverse) {
        entry = findOwnMember(program, program.builtins().canonicalID(entry), key);
        if (!entry)
            return { };
    }
    return entry;
}

void runCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet callees = context.valueOf(context.operandsOf(statement)[0]);
    applyEventsOfCall(statement, context, callees);
    ValueFlowCallKind kind = constructs(statement.opcode) ? ValueFlowCallKind::Construct : ValueFlowCallKind::Call;
    // Any call may find the stack full, and what cannot be called throws. The run time makes both errors.
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));

    // One body of the library runs for every call of such a function, so what this call does with its operands the overloads of the entry say. A construction runs the body for a new object, which no overload describes.
    ValueSetID describedByEntries = kind == ValueFlowCallKind::Call ? universe.builtinTables().closuresThatEntriesDescribe() : ValueSets::nothing;
    context.forEachMember(context.restrict(callees, describedByEntries), [&](ValueFlowObjectID closure) {
        applyEntryDescribing(closure, kind, statement, context);
    });
    ValueSetID enteredClosures = context.maker().intersect(universe.closures(), ValueSets::complement(describedByEntries));
    if (context.meet(callees, enteredClosures)) {
        ClimbingSet closures = passOperandsToInstances(context, statement, context.restrict(callees, enteredClosures), kind);
        if (context.meet(closures, ValueSets::anything))
            passOperands(context, statement, closures, kind, passKnownReceiversToTheirCallees(context, statement, closures));
    }
    callListedBuiltins(statement, context, callees, kind);
    // Beside the unknown object the normal form leaves out a function that has escaped, and of what the unknown object covers no rule applies a function of the host: see runUsedBuiltins().
    if (context.contains(callees, ValueFlowUniverse::unknownObject())) {
        const Function& function = context.program().at(statement.function);
        ValueID callee = function.at(statement.site).operand(0);
        BuiltinID named = callee ? entryNamedByOperations(context.program(), function, callee) : BuiltinID();
        if (named && context.program().builtins().at(named).origin == BuiltinOrigin::Host && context.sets().contains(universe.builtinFunctions(), universe.objectOf(named)))
            applyBuiltin(named, kind, statement, context);
    }
    for (ValueFlowObjectID placeholder : { ValueFlowUniverse::untrackedObject(), ValueFlowUniverse::unknownObject() }) {
        if (!context.contains(callees, placeholder))
            continue;
        PropertyKeyID keyOfMember = placeholder == ValueFlowUniverse::unknownObject() ? keyOfMemberCalledOnItsHolder(context.program(), statement) : PropertyKeyID();
        if (keyOfMember && findsValueNobodyKnowsThatProgramHasStored(context, context.valueOf(context.operandsOf(statement)[1]), keyOfMember))
            keyOfMember = { };
        if (placeholder == ValueFlowUniverse::unknownObject() && isCodeThatRecordsDescribe(context.universe(), statement.function))
            callOperandOfDescribedCall(statement, context);
        else if (keyOfMember)
            callMemberNobodyKnows(statement, context, keyOfMember);
        else
            callPlaceholder(statement, context, placeholder, kind);
    }

    // A base class that nobody sees makes the object for the class of the program, with the layout that the operation names and the prototype that new.target says. Whoever has made it may have kept it.
    bool isSuperConstruct = statement.opcode == Opcode::SuperConstruct || statement.opcode == Opcode::SuperConstructList;
    if (isSuperConstruct && context.contains(callees, ValueFlowUniverse::unknownObject())) {
        ClimbingSet made = only(context, universe.siteObject(statement));
        context.connect(context.lookup(context.valueOf(context.operandsOf(statement)[1]), prototypeProperty(context)), context.written(made, prototypeLinkName));
        context.add(ValueFlowProgramNodes::escaped(), made);
        if (statement.result)
            context.add(statement.result, made);
    }
}

static void passArguments(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, ValueFlowCallKind kind, FunctionID function, const Function& callee)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    unsigned first = callee.firstParameterIndex();
    unsigned positional = callee.numberOfPositionalParameters();
    for (unsigned index = 0; index < positional; ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(function, first + index);
        context.connect(group.argumentAt(index), parameter);
        context.connect(group.otherArguments, parameter);
        // A call that an operation spells out has said which positions it leaves empty. Nobody says so for the others.
        bool callersSayWhatTheyLeaveOut = (kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct || kind == ValueFlowCallKind::CallByHost) && index < numberOfGroupedArguments;
        if (!callersSayWhatTheyLeaveOut)
            context.add(parameter, universe.undefinedValue());
    }

    std::optional<unsigned> hiddenRest = callee.hiddenRestParameterIndex();
    bool hasRest = callee.has(FunctionFlags::HasRestParameter);
    if (!hasRest && !hiddenRest)
        return;
    ClimbingSet rest = only(context, universe.restArrayOf(function));
    context.add(nodes.entryParameter(function, hasRest ? first + positional : *hiddenRest), rest);
    ValueFlowNodeID contents = context.written(rest, elementsName);
    for (unsigned index = positional; index < numberOfGroupedArguments; ++index)
        context.connect(group.argument(index), contents);
    context.connect(group.laterArguments, contents);
    context.connect(group.otherArguments, contents);
    if (context.restArgumentEntered(function, 0)) {
        for (unsigned index = positional; index < numberOfGroupedArguments; ++index)
            context.connect(group.argument(index), context.restArgumentEntered(function, index - positional));
        context.connect(group.laterArguments, context.restArgumentEntered(function, numberOfGroupedArguments));
        context.connect(group.otherArguments, context.restArgumentEntered(function, numberOfGroupedArguments));
    }
    if (!findCallThatAloneTakesRestArray(callee))
        return;

    bool callersSayWhatTheyLeaveOut = kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct || kind == ValueFlowCallKind::CallByHost;
    for (unsigned index = positional; index < numberOfGroupedArguments; ++index) {
        ValueFlowNodeID argument = context.written(rest, restArgument(index - positional));
        context.connect(group.argument(index), argument);
        if (!callersSayWhatTheyLeaveOut)
            context.add(argument, universe.undefinedValue());
    }
    context.connect(group.laterArguments, context.written(rest, laterRestArguments));
    context.connect(group.otherArguments, context.written(rest, otherRestArguments));
}

// The constructor of the base class at the upper end allocates the instances of such a class: ValueFlowUniverse::objectAllocatedUnder().
static bool isAllocatedByBaseClassOfProgram(const Program& program, LayoutID layout)
{
    for (size_t steps = 0; steps < program.layouts.size() && layout && program.at(layout).has(LayoutFlags::HasHeritage); ++steps)
        layout = predictedHeritageOf(program.at(layout));
    return layout && !program.at(layout).has(LayoutFlags::HasHeritage) && !program.at(layout).has(LayoutFlags::IsConstructorFunction);
}

namespace {

// What the constructions of a group allocate. It depends on new.target alone, so it is the same for every callee, and a group may have thousands of both.
class AllocatedInstances {
public:
    AllocatedInstances(ValueFlowRuleContext& context, const ValueFlowCallGroup& group)
        : m_context(context)
        , m_group(group)
    {
    }

    void passTo(ValueFlowNodeID thisParameter)
    {
        if (!m_isCollected)
            collect();
        add(thisParameter);
    }

private:
    void add(ValueFlowNodeID node)
    {
        if (!node)
            return;
        if (m_holdsObjectWithoutLayout)
            m_context.noteUnknownSource(ValueFlowUnknownSource::ObjectWithoutLayout);
        m_context.add(node, m_instances);
    }

    void collect()
    {
        const ValueFlowUniverse& universe = m_context.universe();
        const Program& program = m_context.program();
        ClimbingSet newTargets = m_context.valueOf(m_group.newTarget);
        // The untracked object stands for what flow has stopped following, and every function among that has a layout to allocate or none.
        if (m_context.contains(newTargets, ValueFlowUniverse::untrackedObject())) {
            ClimbingSet lostCallables = m_context.valueWithin(ClimbingSet(universe.callables()), ValueFlowProgramNodes::lost());
            newTargets = m_context.unite(m_context.restrict(newTargets, ValueSets::complement(universe.untrackedObjectAlone())), lostCallables);
        }
        Vector<ValueFlowObjectID, 16> instances;
        m_context.forEachMember(newTargets, [&](ValueFlowObjectID newTarget) {
            FunctionID constructor = universe.closureFunction(newTarget);
            LayoutID layout = constructor ? program.at(constructor).constructedClass : LayoutID();
            // The unknown object stands for what is made for a new.target that is no function of the program.
            if (!constructor) {
                m_holdsObjectWithoutLayout = true;
                // What the program has given such a function as `prototype`, a bound function say, lies above the new object. Through the unknown object only what has escaped is found.
                if (m_context.meet(only(m_context, newTarget), universe.knownObjects()))
                    m_context.connect(m_context.masked(m_context.lookup(only(m_context, newTarget), prototypeProperty(m_context)), ValueSets::complement(universe.builtinObjects())), ValueFlowProgramNodes::escaped());
                return;
            }
            ValueFlowObjectID instance = layout ? universe.instanceOf(layout) : universe.constructedObjectOf(constructor);
            instances.append(instance);
            // `Object.setPrototypeOf(Circle, function () {})`, `Reflect.construct(function () {}, [], Circle)`: a function that is no class makes the Circle, and no constructor above Circle runs.
            if (layout && isAllocatedByBaseClassOfProgram(program, layout))
                m_context.add(m_context.written(only(m_context, instance), ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey)), universe.undefinedValue());
            ValueFlowNodeID held = m_context.lookup(only(m_context, newTarget), prototypeProperty(m_context));
            m_context.connect(held, m_context.written(only(m_context, instance), prototypeLinkName));
            if (!layout && m_context.meet(m_context.valueOf(held), ValueSets::complement(universe.objects())))
                m_context.add(m_context.written(only(m_context, instance), prototypeLinkName), only(m_context, universe.objectOf(Builtins::ObjectPrototype)));
        });
        m_instances = ClimbingSet(m_context.maker().of(instances.span()));
        if (m_holdsObjectWithoutLayout)
            m_instances = m_context.unite(m_instances, unknownObject(m_context, ValueFlowUnknownSource::ObjectWithoutLayout));
        m_isCollected = true;
        add(m_group.result);
    }

    ValueFlowRuleContext& m_context;
    const ValueFlowCallGroup& m_group;
    ClimbingSet m_instances;
    bool m_isCollected { false };
    bool m_holdsObjectWithoutLayout { false };
};

} // anonymous namespace

static void enter(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, ValueFlowCallKind kind, ValueFlowObjectID closure, FunctionID function, AllocatedInstances& allocated)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& callee = context.program().at(function);
    bool isConstruction = Typed::isConstruction(kind);
    if (throwsInsteadOfRunning(callee, isConstruction)) {
        context.add(group.thrown, unknownObject(context, ValueFlowUnknownSource::ThrownByRunTime));
        return;
    }

    context.reach(function);
    if (auto index = callee.calleeParameterIndex())
        context.add(nodes.entryParameter(function, *index), only(context, closure));
    if (auto index = callee.argumentCountParameterIndex())
        context.add(nodes.entryParameter(function, *index), universe.anyPrimitive());
    if (auto index = callee.newTargetParameterIndex()) {
        if (isConstruction)
            context.connect(group.newTarget, nodes.entryParameter(function, *index));
        else
            context.add(nodes.entryParameter(function, *index), universe.undefinedValue());
    }
    if (isConstruction && callee.kind == FunctionKind::Plain)
        allocated.passTo(thisParameterOf(context, function));
    else
        context.connect(group.thisValue, thisParameterOf(context, function));
    passArguments(context, group, kind, function, callee);

    context.connect(nodes.thrown(function), group.thrown);
    if (callee.isUnsplitCoroutine()) {
        // What the body returns goes into that object: the statement of kind Coroutine in the body sees to it.
        context.add(group.result, only(context, universe.coroutineResultOf(function)));
        return;
    }
    const auto* summary = isConstruction ? nullptr : nodes.resultSummaryOf(function);
    if (!summary) {
        context.connect(isConstruction ? context.masked(nodes.result(function, 0), universe.objects()) : nodes.result(function, 0), group.result);
        return;
    }

    // The callers of the group get back what the callers of the group have passed, as passArguments() hands it in, and not what other callers of the function have.
    context.connect(summary->ownResult, group.result);
    for (const auto& passed : summary->passedParameters) {
        auto giveBack = [&](ValueFlowNodeID given) {
            context.connect(context.masked(given, passed.mask, passed.maskKind), group.result);
        };
        if (callee.entryParameterKind(passed.entryParameter) == EntryParameter::This) {
            giveBack(group.thisValue);
            continue;
        }
        unsigned position = passed.entryParameter - callee.firstParameterIndex();
        giveBack(group.argumentAt(position));
        giveBack(group.otherArguments);
        giveBack(context.holding(ClimbingSet(universe.undefinedValue())));
    }
}

void runCallGroup(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet key = context.setOfKey();
    ValueFlowCallKind kind = context.callKindOfKey();
    ValueFlowRuleContext::InstanceEntered instance(context, context.markerOfKey());
    ValueFlowCallGroup group = context.group(key, kind, ValueFlowCalleeOrigin::MemberFoundUnderName);

    // Whoever asks for a group gives a placeholder a key of its own. Whatever can be called among what the placeholder covers may be the callee, with the operands of the group.
    auto callablesIn = [&](ValueFlowNodeID covered) {
        return context.valueWithin(ClimbingSet(universe.callables()), covered);
    };
    if (context.contains(key, calleeOfDescribedCall(universe))) {
        ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
        context.connect(group.thisValue, escaped);
        if (group.newTarget)
            context.connect(group.newTarget, escaped);
        for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
            context.connect(group.argument(index), escaped);
        context.connect(group.laterArguments, escaped);
        context.connect(group.otherArguments, escaped);
        context.add(group.result, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
        context.add(group.thrown, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
        return;
    }

    ClimbingSet listed = context.restrict(key, universe.knownObjects());
    ClimbingSet lost = context.contains(key, ValueFlowUniverse::untrackedObject()) ? callablesIn(ValueFlowProgramNodes::lost()) : ClimbingSet();
    bool callsUnknownObject = context.contains(key, ValueFlowUniverse::unknownObject());

    // A call that an operation spells out brings the functions of the program here and has said itself whether seen code calls them.
    bool isSpelledOut = kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct;
    if (kind != ValueFlowCallKind::CallByHost)
        applyEventsOfCall(statement, context, context.unite(isSpelledOut ? context.restrict(listed, universe.builtinFunctions()) : listed, lost));

    ClimbingSet callees = context.unite(listed, lost);
    // The operands of such a call escape and its result is unknown, which is all that the entry of a built-in could say.
    if (callsUnknownObject)
        callees = context.unite(callees, context.valueWithin(ClimbingSet(universe.otherCallables()), ValueFlowProgramNodes::escaped()));
    AllocatedInstances allocated(context, group);
    // Another callable is neither a function of the program nor, unless it is a built-in object, an entry of the table: callOtherCallables() below deals with those, as a set. A placeholder stands for thousands.
    ClimbingSet dealtWithOneByOne = callees;
    if (callsUnknownObject || context.contains(key, ValueFlowUniverse::untrackedObject()))
        dealtWithOneByOne = context.restrict(callees, ValueSets::complement(context.maker().intersect(universe.otherCallables(), ValueSets::complement(universe.builtinObjects()))));
    context.forEachMember(dealtWithOneByOne, [&](ValueFlowObjectID callee) {
        if (FunctionID function = universe.closureFunction(callee)) {
            // The host's group is the one through which the code behind a described entry is entered.
            if (kind != ValueFlowCallKind::CallByHost && !isConstruction(kind) && context.sets().contains(universe.builtinTables().closuresThatEntriesDescribe(), callee)) {
                bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
                applyEntryDescribing(callee, isAccessorCall ? ValueFlowCallKind::Call : kind, group, context);
                return;
            }
            enter(context, group, kind, callee, function, allocated);
            return;
        }
        // A lookup finds accessors and data properties of the table, which are applied in the mode of the group. A function of the table that the program has made an accessor is called like any function.
        bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
        bool isFunction = context.sets().contains(universe.builtinFunctions(), callee);
        BuiltinID builtin = universe.builtinOf(callee);
        if (!builtin || (!isFunction && !isAccessorCall))
            return;
        applyBuiltin(builtin, isFunction && isAccessorCall ? ValueFlowCallKind::Call : kind, group, context);
    });
    if (context.meet(callees, universe.otherCallables()))
        callOtherCallables(context.restrict(callees, universe.otherCallables()), kind, group, context);
    if (!callsUnknownObject)
        return;

    // Code that nobody compiles may run: it keeps the operands, does to them what it likes and answers what it likes.
    letUnknownCodeWrite(context);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    context.connect(group.thisValue, escaped);
    if (group.newTarget)
        context.connect(group.newTarget, escaped);
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        context.connect(group.argument(index), escaped);
    context.connect(group.laterArguments, escaped);
    context.connect(group.otherArguments, escaped);
    context.add(group.result, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
    context.add(group.thrown, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
}

// The operands: the constructor, what it extends, and what the prototype object inherits from.
void runNewClass(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const Program& program = context.program();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = program.at(statement.function).at(statement.site);
    ClimbingSet constructors = context.valueOf(operands[0]);
    ClimbingSet prototype = only(context, universe.prototypeOf(operation.layout()));

    context.add(context.written(constructors, prototypeProperty(context)), prototype);
    context.connect(operands[0], context.written(prototype, ValueFlowName::property(program.keyOf(BuiltinNames::constructor))));
    // Without a heritage both inherit from what every function and every object is born with.
    if (operation.heritageOperand()) {
        context.connect(operands[1], context.written(constructors, prototypeLinkName));
        context.connect(operands[2], context.written(prototype, prototypeLinkName));
        // `extends null`: nothing lies above the prototype object, and above the class object lies what every function is born with. That is what `super()` finds, and it is no constructor.
        if (context.contains(context.valueOf(operands[1]), ValueFlowUniverse::primitive(PrimitiveSet::Null)))
            context.add(context.written(constructors, prototypeLinkName), only(context, universe.objectOf(Builtins::FunctionPrototype)));
    }

    // A static field is a variable and a property of the class object at once. The variable holds what is stored into the class object itself: nothing that the object inherits, and nothing where a lookup finds nothing.
    bool hasEscaped = context.holdsAnyOf(constructors, ValueFlowProgramNodes::escaped());
    bool isLost = context.holdsAnyOf(constructors, ValueFlowProgramNodes::lost());
    for (const StaticField& field : program.at(operation.layout()).staticFields) {
        ValueFlowNodeID variable = context.nodes().variable(field.variable);
        context.connect(variable, context.written(constructors, field.key ? ValueFlowName::property(field.key) : ValueFlowName { ValueFlowNameKind::UnknownKey, 0 }));
        ValueFlowName found = field.key ? ValueFlowName::property(field.key) : ValueFlowName { ValueFlowNameKind::EveryProperty, 0 };
        context.connect(context.read(withSourcesOfCopies(context, context.restrict(constructors, universe.knownObjects())), found), variable);
        // A store through a placeholder may hit whatever the placeholder stands for.
        if (hasEscaped)
            context.connect(context.read(ClimbingSet(universe.unknownObjectAlone()), found), variable);
        if (isLost)
            context.connect(context.read(ClimbingSet(universe.untrackedObjectAlone()), found), variable);
    }
}

// A name that a number converts to is an index: `x[0]()` finds the method `0() { }`, and whoever reads or stores under an index asks for the elements.
static ValueFlowName nameOfMember(const Program& program, MethodKind kind, PropertyKeyID key)
{
    bool isIndex = program.isIndexLike(key);
    switch (kind) {
    case MethodKind::Method:
        return isIndex ? ValueFlowName::of(ValueFlowNameKind::Elements) : ValueFlowName::property(key);
    case MethodKind::Getter:
        return isIndex ? ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey) : ValueFlowName::getter(key);
    case MethodKind::Setter:
        return isIndex ? ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey) : ValueFlowName::setter(key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The operands: the class object, the function, and the key where it is computed.
void runDefineMethod(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = context.program().at(statement.function).at(statement.site);
    ClimbingSet holder = operation.side() == ClassSide::Static ? context.valueOf(operands[0]) : only(context, universe.prototypeOf(operation.layout()));

    auto define = [&](PropertyKeyID key) {
        context.connect(operands[1], context.written(holder, nameOfMember(context.program(), operation.methodKind(), key)));
        if (operation.methodKind() != MethodKind::Method && operation.side() != ClassSide::Static && key == context.program().keyOf(BuiltinNames::constructor))
            context.mark(holder, ObjectMutation::RedefineProperty);
    };
    if (PropertyKeyID key = operation.key()) {
        define(key);
        return;
    }
    ClimbingSet keys = context.valueOf(operands[2]);
    context.forEachMember(context.restrict(keys, universe.keys()), [&](ValueFlowObjectID key) {
        define(universe.keyOf(key));
    });
    if (!context.meet(keys, ValueSets::complement(universe.keys())))
        return;
    if (operation.methodKind() != MethodKind::Method && operation.side() != ClassSide::Static)
        context.mark(holder, ObjectMutation::RedefineProperty);
    switch (operation.methodKind()) {
    case MethodKind::Method:
        context.connect(operands[1], context.written(holder, { ValueFlowNameKind::UnknownKey, 0 }));
        return;
    case MethodKind::Getter:
        context.connect(operands[1], context.written(holder, { ValueFlowNameKind::GetterUnderUnknownKey, 0 }));
        return;
    case MethodKind::Setter:
        context.connect(operands[1], context.written(holder, { ValueFlowNameKind::SetterUnderUnknownKey, 0 }));
        return;
    }
}

// ResolveHeritage, GetClassPrototype, GetConstructPrototype: each reads `prototype` of its first operand.
void runReadOfPrototypeProperty(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (!statement.result)
        return;
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet holders = context.valueOf(context.operandsOf(statement)[0]);
    ValueSetID null = context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Null));
    // A class may extend a function whose `prototype` is null.
    ValueSetID answers = statement.opcode == Opcode::ResolveHeritage ? context.maker().unite(universe.objects(), null) : universe.objects();
    context.connect(context.masked(context.lookup(holders, prototypeProperty(context)), answers), statement.result);
    switch (statement.opcode) {
    case Opcode::ResolveHeritage:
        // `extends null`
        if (context.meet(holders, null) || context.contains(holders, ValueFlowUniverse::unknownObject()))
            context.add(statement.result, null);
        break;
    case Opcode::GetConstructPrototype: {
        // Where the property holds no object. Nobody can store to the prototype property of a class.
        bool mayHoldNoObject = false;
        context.forEachMember(holders, [&](ValueFlowObjectID holder) {
            FunctionID function = universe.closureFunction(holder);
            mayHoldNoObject |= !function || !constructsClass(context.program().at(function));
        });
        if (mayHoldNoObject)
            context.add(statement.result, only(context, universe.objectOf(Builtins::ObjectPrototype)));
        break;
    }
    default:
        break;
    }
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
