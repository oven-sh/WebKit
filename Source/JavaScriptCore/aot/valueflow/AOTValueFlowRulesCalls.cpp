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
#include "AOTValueFlowTypedResults.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueFlowName prototypeProperty(ValueFlowRuleContext& context)
{
    return ValueFlowName::property(context.program().keyOf(BuiltinNames::prototype));
}

static bool constructs(Opcode opcode)
{
    return opcode != Opcode::Call && opcode != Opcode::CallList;
}

static void callListedBuiltins(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees, ValueFlowCallKind kind, ValueSetID receiversFollowedBehindUnknownObject = ValueSets::nothing)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet builtins = context.restrict(callees, universe.builtinFunctions());
    PropertyKeyID key = context.meet(builtins, ValueSets::anything) ? statement.key : PropertyKeyID();
    ValueFlowObjectsToldApart receivers;
    Vector<ClimbingSet, 16> answers;
    if (key) {
        ClimbingSet all = context.valueOf(context.operandsOf(statement)[1]);
        receivers = tellObjectsApart(context, context.restrict(all, universe.knownObjects()));
        // The object nobody knows is a receiver of every callee, so which kind of primitive finds which callee adds nothing. Told apart, each kind costs a lookup under every name that is called on such a value.
        if (context.contains(all, ValueFlowUniverse::unknownObject())) {
            if (ValueSetID primitives = context.withMembersOf(ValueSets::nothing, context.restrict(all, universe.anyPrimitive())); primitives != ValueSets::nothing)
                receivers.groups.append(primitives);
        } else {
            context.forEachMemberInLine(context.restrict(all, universe.anyPrimitive()), [&](ValueFlowObjectID kindOfPrimitive) {
                receivers.groups.append(context.maker().of(kindOfPrimitive));
            });
        }
        for (ValueSetID group : receivers.groups)
            answers.append(context.valueOf(context.lookup(ClimbingSet(group), ValueFlowName::property(context.program(), key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver)));
        // A text or a symbol that is known finds what any of its kind finds.
        for (auto [kind, all] : { std::pair { PrimitiveSet::String, universe.strings() }, std::pair { PrimitiveSet::Symbol, universe.symbols() } }) {
            ValueSetID known = context.maker().intersect(all, universe.keys());
            if (!context.meet(context.valueOf(context.operandsOf(statement)[1]), known))
                continue;
            receivers.groups.append(known);
            answers.append(context.valueOf(context.lookup(only(context, ValueFlowUniverse::primitive(kind)), ValueFlowName::property(context.program(), key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver)));
        }
        if (context.contains(all, ValueFlowUniverse::unknownObject()))
            receivers.rest = context.maker().unite(receivers.rest, receiversFollowedBehindUnknownObject);
    }
    context.forEachMemberInLine(builtins, [&](ValueFlowObjectID callee) {
        if (!key) {
            applyBuiltin(universe.builtinOf(callee), kind, statement, context);
            return;
        }
        ValueSetID found = context.maker().unite(ValueSets::complement(context.maker().unite(context.maker().unite(universe.knownObjects(), universe.anyPrimitive()), universe.keys())), receivers.rest);
        for (size_t index = 0; index < answers.size(); ++index) {
            if (context.contains(answers[index], callee) || context.meet(answers[index], universe.placeholdersAlone()))
                found = context.maker().unite(found, receivers.groups[index]);
        }
        applyBuiltin(universe.builtinOf(callee), kind, statement, context, found);
    });
    if (context.meet(callees, universe.otherCallables()))
        callOtherCallables(context.restrict(callees, universe.otherCallables()), kind, statement, context);
}

struct ReceiversOfEveryCallee {
    ValueSetID mask { ValueSets::anything };
    ValueSetID objects { ValueSets::nothing };
};

static void passThis(ValueFlowRuleContext& context, const ValueFlowStatement& statement, const ValueFlowOperand& receiver, const ReceiversOfEveryCallee& passedReceivers, ValueFlowNodeID thisValue)
{
    if (!statement.factsOfText.contains(ValueFlowFactOfText::HasSecondOperand)) {
        context.add(thisValue, context.universe().undefinedValue());
        return;
    }
    if (passedReceivers.mask == ValueSets::anything) {
        context.connect(receiver, thisValue);
        return;
    }
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

static unsigned numberOfPositionsTakenBy(ValueFlowRuleContext& context, ClimbingSet closures)
{
    AOT_THRESHOLD(unsigned, mostCalleesLookedAt, 4);
    if (context.numberOfMembers(closures) > mostCalleesLookedAt)
        return numberOfGroupedArguments;
    const ValueFlowUniverse& universe = context.universe();
    unsigned positions = 0;
    context.forEachMemberInLine(closures, [&](ValueFlowObjectID closure) {
        if (positions == numberOfGroupedArguments)
            return;
        FunctionID function = universe.closureFunction(closure);
        if (!function || context.sets().contains(universe.builtinTables().closuresThatEntriesDescribe(), closure)) {
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

static ValueFlowObjectsToldApart tellManyObjectsApart(ValueFlowRuleContext& context, ClimbingSet knownObjects, bool isAllElseAtRest)
{
    const ValueFlowUniverse& universe = context.universe();
    uint64_t mostGroups = context.mostMembersToldApart();
    struct Member {
        uint64_t group;
        ValueFlowObjectID object;
    };
    Vector<Member, 64> members;
    context.forEachMemberInLine(knownObjects, [&](ValueFlowObjectID object) {
        members.append(Member { 0, object });
    });

    ValueFlowObjectsToldApart result;
    // Of many, some may yet turn out to have escaped, and a set that holds the object nobody knows lists none of those. How many are left shows once all else is at rest: until then the first are told apart and the others wait.
    if (!isAllElseAtRest) {
        for (size_t index = 0; index < mostGroups; ++index)
            result.groups.append(context.maker().of(members[index].object));
        return result;
    }

    constexpr uint64_t builtinObjectsTogether = 2ull << 32;
    for (Member& member : members) {
        if (LayoutID layout = universe.layoutOf(member.object))
            member.group = layout.index();
        else if (BuiltinClassID builtinClass = universe.classOf(member.object))
            member.group = 1ull << 32 | builtinClass.index();
        else if (universe.builtinOf(member.object))
            member.group = builtinObjectsTogether;
        else
            member.group = 3ull << 32 | member.object.index();
    }
    AOT::stableSortInLine(members, { }, &Member::group);
    uint64_t numberOfGroups = 0;
    for (size_t index = 0; index < members.size(); ++index)
        numberOfGroups += !index || members[index].group != members[index - 1].group;
    if (numberOfGroups < mostGroups) {
        uint64_t placesLeft = mostGroups - numberOfGroups;
        for (Member& member : members) {
            if (member.group != builtinObjectsTogether)
                continue;
            if (!placesLeft--)
                break;
            member.group = 4ull << 32 | member.object.index();
        }
        AOT::stableSortInLine(members, { }, &Member::group);
    }
    Vector<ValueFlowObjectID, 64> objects;
    for (size_t first = 0; first < members.size();) {
        size_t end = first;
        objects.shrink(0);
        while (end < members.size() && members[end].group == members[first].group)
            objects.append(members[end++].object);
        result.groups.append(context.maker().of(objects.span()));
        first = end;
    }
    return result;
}

ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext& context, ClimbingSet knownObjects)
{
    if (context.numberOfMembers(knownObjects) > context.mostMembersToldApart()) {
        bool isAllElseAtRest = isHostFlagRaised(context, ValueFlowHostNode::AllElseIsAtRest);
        const ValueFlowObjectsToldApart& known = context.objectsToldApartRememberedOf(knownObjects, isAllElseAtRest, [&] {
            return tellManyObjectsApart(context, knownObjects, isAllElseAtRest);
        });
        if (known.rest != ValueSets::nothing)
            context.noteMembersNotToldApart();
        return known;
    }
    ValueFlowObjectsToldApart result;
    context.forEachMemberInLine(knownObjects, [&](ValueFlowObjectID object) {
        result.groups.append(context.maker().of(object));
    });
    return result;
}

static bool constructsClass(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

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

bool throwsInsteadOfRunning(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.constructorForm() != ConstructorForm::NotConstructor && callee.kind != FunctionKind::Plain;
}

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
            if (statement.factsOfText.contains(ValueFlowFactOfText::HasSecondOperand))
                operand = &operands[1];
        } else if (size_t position = passed.entryParameter - callee.firstParameterIndex(); position < arguments.size())
            operand = &arguments[position];
        if (!operand) {
            context.connect(context.masked(context.holding(ClimbingSet(context.universe().undefinedValue())), passed.mask, passed.maskKind), statement.result);
            continue;
        }
        if (operand->node)
            context.connect(context.masked(context.masked(operand->node, operand->mask, operand->maskKind), passed.mask, passed.maskKind), statement.result);
        if (operand->constant)
            context.connect(context.masked(context.holding(context.valueOf(ValueFlowOperand { { }, operand->constant, operand->mask, operand->maskKind })), passed.mask, passed.maskKind), statement.result);
    }
}

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
    context.forEachMemberInLine(callees, [&](ValueFlowObjectID closure) {
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

static constexpr ValueFlowCellName restArgument(unsigned index)
{
    return ValueFlowCellName { ValueFlowNameKind::RestArgument, index };
}

static constexpr ValueFlowCellName laterRestArguments = restArgument(numberOfGroupedArguments);
static constexpr ValueFlowCellName otherRestArguments = restArgument(numberOfGroupedArguments + 1);

static ValueID findCallThatAloneTakesRestArray(ValueFlowRuleContext& context, FunctionID id)
{
    if (const Function& record = context.program().at(id); !record.has(FunctionFlags::HasRestParameter) || !record.hasBody())
        return { };
    const Function& function = context.functionWithBody(id);
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

static bool passRestArrayByPosition(ValueFlowRuleContext& context, const ValueFlowStatement& statement, const ValueFlowCallGroup& group)
{
    const Function& function = context.program().at(statement.function);
    if (findCallThatAloneTakesRestArray(context, statement.function) != statement.site)
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

static void passOperands(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet callees, ValueFlowCallKind kind, const ReceiversOfEveryCallee& passedReceivers = { }, ValueFlowCalleeOrigin origin = ValueFlowCalleeOrigin::Value)
{
    if (kind == ValueFlowCallKind::Construct && !statement.factsOfText.contains(ValueFlowFactOfText::HasSecondOperand)) {
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
    else if (statement.factsOfText.contains(ValueFlowFactOfText::HasSecondOperand)) {
        context.connect(operands[1], group.newTarget);
        context.connect(operands[1], context.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands));
        answeredObjects = objectsAnsweredTo(context, callees, context.valueOf(operands[1]));
    } else {
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

static ValueFlowNodeID thisParameterOf(ValueFlowRuleContext& context, FunctionID function)
{
    std::optional<unsigned> index = context.program().at(function).thisParameterIndex();
    return index ? context.nodes().entryParameter(function, *index) : ValueFlowNodeID();
}

ValueID definitionThroughTests(const Function& function, ValueID id)
{
    AOT_THRESHOLD(unsigned, mostTests, 4);
    for (unsigned steps = 0; steps < mostTests; ++steps) {
        id = definitionThroughSameValues(function, id);
        if (function.at(id).opcode != Opcode::Parameter)
            return id;
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

static PropertyKeyID keyOfMemberCalledOnItsHolder(ValueFlowRuleContext& context, const ValueFlowStatement& statement)
{
    if (PropertyKeyID key = statement.key)
        return key;
    const ValueFlowRareFactsOfText* facts = context.rareFactsOfText(statement);
    return facts ? facts->keyOfMemberCalledOnItsHolder : PropertyKeyID();
}

PropertyKeyID keyOfMemberCalledOnItsHolderByText(const Program& program, const Function& function, const Value& call)
{
    if (constructs(call.opcode))
        return { };
    ValueID receiver = call.thisValueOperand();
    if (!receiver)
        return { };
    const Value& read = function.at(definitionThroughTests(function, call.calleeOperand()));
    // A member of the namespace object of a module of the library is read out of the variable of the export.
    if (read.opcode == Opcode::GetModuleVariable) {
        const Value& holder = function.at(definitionThroughTests(function, receiver));
        if (holder.opcode != Opcode::ModuleNamespace)
            return { };
        for (const SyntheticExport& entry : program.at(holder.module()).syntheticExports) {
            if (entry.variable == read.variable())
                return program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }).value_or(PropertyKeyID());
        }
        return { };
    }
    if (read.opcode != Opcode::PropGet || read.isFreeName() || program.isIndexLike(read.key()))
        return { };
    return definitionThroughTests(function, read.objectOperand()) == definitionThroughTests(function, receiver) ? read.key() : PropertyKeyID();
}

// What a module of the library has stored in the object that it exports reaches that object by itself: as a receiver the object is handed to nobody new.
// Where the program has written to the object, the member may be a function of the program's.
static ValueSetID objectsShowingOnlyWhatLibraryExports(ValueFlowRuleContext& context)
{
    return context.withMembersOf(ValueSets::nothing, context.membersMissingFrom(context.nodes().ofHost(ValueFlowHostNode::HoldersOfLibraryExportsThatProgramWrites), ClimbingSet(context.universe().builtinTables().heap().objectsShowingLibraryExports())));
}

// Asked of the objects at hand alone. A statement with no such holder among its objects is not woken when the program writes to one more of them.
static ClimbingSet withoutObjectsShowingOnlyWhatLibraryExports(ValueFlowRuleContext& context, ClimbingSet objects)
{
    ClimbingSet showing = context.restrict(objects, context.universe().builtinTables().heap().objectsShowingLibraryExports());
    ClimbingSet unwritten = context.membersMissingFrom(context.nodes().ofHost(ValueFlowHostNode::HoldersOfLibraryExportsThatProgramWrites), showing);
    return context.restrict(objects, ValueSets::complement(context.withMembersOf(ValueSets::nothing, unwritten)));
}

void passKnownReceiversToWhatTheyFind(ValueFlowRuleContext& context, ClimbingSet receivers, ValueFlowName name, ValueFlowNodeID receiversAnswered)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowInstancedFunctions* instanced = context.instancedFunctions();
    ValueSetID entered = context.maker().intersect(universe.closures(), ValueSets::complement(context.maker().unite(universe.builtinTables().closuresThatEntriesDescribe(), instanced ? instanced->closures() : ValueSets::nothing)));
    ValueFlowObjectsToldApart toldApart = tellObjectsApart(context, context.restrict(receivers, universe.knownObjects()));
    for (ValueSetID group : toldApart.groups) {
        ClimbingSet answered = context.valueOf(context.lookup(ClimbingSet(group), name, ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver));
        std::span<const ValueFlowObjectID> takingReceiver = context.closuresTakingReceiverRememberedOf(context.restrict(answered, entered), [&](ValueFlowObjectID closure) {
            FunctionID function = universe.closureFunction(closure);
            return thisParameterOf(context, function) && !throwsInsteadOfRunning(program.at(function), false);
        });
        for (ValueFlowObjectID closure : takingReceiver) {
            FunctionID function = universe.closureFunction(closure);
            context.add(thisParameterOf(context, function), group);
            const auto* summary = context.nodes().resultSummaryOf(function);
            if (!summary || program.at(function).isUnsplitCoroutine())
                continue;
            for (const auto& passed : summary->passedParameters) {
                if (program.at(function).entryParameterKind(passed.entryParameter) == EntryParameter::This)
                    context.connect(context.masked(context.holding(ClimbingSet(group)), passed.mask, passed.maskKind), receiversAnswered);
            }
        }
    }
}

static ReceiversOfEveryCallee passKnownReceiversToTheirCallees(ValueFlowRuleContext& context, const ValueFlowStatement& statement, ClimbingSet closures)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    PropertyKeyID key = statement.key;
    if (!key)
        return { };

    ValueFlowObjectID undefinedAndNull[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) };
    ValueSetID neverReceivers = context.maker().of(undefinedAndNull);
    const ValueFlowOperand& receiver = context.operandsOf(statement)[1];
    ClimbingSet all = context.valueOf(receiver);
    ClimbingSet known = context.restrict(all, universe.knownObjects());
    if (context.meet(known, ValueSets::anything)) {
        ValueFlowNodeID receiversAnswered = context.lookup(known, ValueFlowName { ValueFlowNameKind::ReceiversOfCalls, key.index() });
        if (statement.result)
            context.connect(receiversAnswered, statement.result);
    }
    if (const ValueFlowInstancedFunctions* instanced = context.instancedFunctions(); instanced && context.meet(closures, instanced->closures())) {
        ClimbingSet enteredAsTheyAre = context.restrict(closures, instanced->closures());
        ValueFlowObjectsToldApart receivers = tellObjectsApart(context, known);
        for (ValueSetID group : receivers.groups) {
            ClimbingSet answered = context.valueOf(context.lookup(ClimbingSet(group), ValueFlowName::property(program, key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver));
            context.forEachMember(context.restrict(answered, instanced->closures()), [&](ValueFlowObjectID closure) {
                FunctionID function = universe.closureFunction(closure);
                if (!context.contains(enteredAsTheyAre, closure) || !thisParameterOf(context, function) || throwsInsteadOfRunning(program.at(function), false))
                    return;
                context.add(thisParameterOf(context, function), group);
                const auto* summary = statement.result ? context.nodes().resultSummaryOf(function) : nullptr;
                if (!summary || program.at(function).isUnsplitCoroutine())
                    return;
                for (const auto& passed : summary->passedParameters) {
                    if (program.at(function).entryParameterKind(passed.entryParameter) == EntryParameter::This)
                        context.connect(context.masked(context.holding(ClimbingSet(group)), passed.mask, passed.maskKind), statement.result);
                }
            });
        }
    }

    // Primitives and objects nobody knows go, as a known object does, to the functions that their own lookup answers.
    ValueSetID others = ValueSets::complement(context.maker().unite(universe.knownObjects(), neverReceivers));
    ClimbingSet otherReceivers = context.restrict(all, others);
    if (context.meet(otherReceivers, ValueSets::anything)) {
        Vector<ValueFlowObjectID, 8> found;
        context.forEachMember(context.restrict(context.valueOf(context.lookup(otherReceivers, ValueFlowName::property(context.program(), key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver)), universe.closures()), [&](ValueFlowObjectID closure) {
            if (!context.contains(closures, closure))
                return;
            found.append(closure);
            FunctionID function = universe.closureFunction(closure);
            const auto* summary = statement.result && receiver.node ? context.nodes().resultSummaryOf(function) : nullptr;
            if (!summary || program.at(function).isUnsplitCoroutine() || throwsInsteadOfRunning(program.at(function), false))
                return;
            for (const auto& passed : summary->passedParameters) {
                if (program.at(function).entryParameterKind(passed.entryParameter) == EntryParameter::This)
                    context.connect(context.masked(context.masked(context.masked(receiver.node, receiver.mask, receiver.maskKind), others), passed.mask, passed.maskKind), statement.result);
            }
        });
        if (!found.isEmpty())
            passThis(context, statement, receiver, { others, ValueSets::nothing }, context.group(ClimbingSet(context.maker().of(found.span())), ValueFlowCallKind::Call).thisValue);
    }
    return { ValueSets::nothing, ValueSets::nothing };
}

static void callOperandOfDescribedCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context, PropertyKeyID keyReadFromReceiver = { }, bool isCalledOnItsHolder = false)
{
    // A member that nobody knows is called on the receivers that find it, or find a function that it stands for. Any other object of the program stays with the callees that it finds.
    if (keyReadFromReceiver) {
        const ValueFlowUniverse& universe = context.universe();
        const ValueFlowOperand& receiver = context.operandsOf(statement)[1];
        ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()));
        ValueFlowObjectsToldApart receivers = tellObjectsApart(context, context.membersMissingFrom(ValueFlowProgramNodes::escaped(), withoutObjectsShowingOnlyWhatLibraryExports(context, context.restrict(context.valueOf(receiver), objectsOfProgram))));
        for (ValueSetID group : receivers.groups) {
            ClimbingSet answered = context.valueOf(context.lookup(ClimbingSet(group), ValueFlowName::property(context.program(), keyReadFromReceiver), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver));
            if (context.meet(answered, universe.placeholdersAlone()) || context.holdsAnyOf(context.restrict(answered, universe.callables()), ValueFlowProgramNodes::escaped()))
                context.add(ValueFlowProgramNodes::escaped(), group);
        }
        context.add(ValueFlowProgramNodes::escaped(), receivers.rest);
        passThis(context, statement, receiver, { ValueSets::complement(objectsOfProgram), ValueSets::nothing }, ValueFlowProgramNodes::escaped());
    }
    if (!keyReadFromReceiver) {
        ValueFlowOperand receiver = context.operandsOf(statement)[1];
        // Which of these receivers escape is one question for all such calls, and the answer changes as the program writes to more holders. runEscape() asks it of one node.
        // Asked here, every call would run again at each change and mask its receiver anew.
        if (isCalledOnItsHolder && receiver.maskKind == ValueFlowMaskKind::Plain)
            context.connect(receiver, context.nodes().ofHost(ValueFlowHostNode::ReceiversOfMembersNobodyKnows));
        else {
            if (isCalledOnItsHolder)
                receiver.mask = context.maker().intersect(receiver.mask, ValueSets::complement(objectsShowingOnlyWhatLibraryExports(context)));
            context.connect(receiver, ValueFlowProgramNodes::escaped());
        }
    }
    bool isInLibrary = isLibraryCode(context.program(), context.program().at(statement.function));
    for (const ValueFlowOperand& argument : context.operandsOf(statement).subspan(2)) {
        if (isInLibrary)
            context.connect(argument, ValueFlowProgramNodes::escaped());
        else
            carryOut(context, argument);
    }
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

static bool findsValueNobodyKnowsThatProgramHasStored(ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID key)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(context.maker().unite(universe.builtinObjects(), universe.objectsThatMayBeAnything())));
    ClimbingSet start = withoutObjectsShowingOnlyWhatLibraryExports(context, context.restrict(receivers, objectsOfProgram));
    if (!context.meet(start, ValueSets::anything))
        return false;
    ClimbingSet chain = context.unite(start, withoutObjectsShowingOnlyWhatLibraryExports(context, context.restrict(context.valueOf(context.lookup(start, prototypeLinkName)), objectsOfProgram)));
    ClimbingSet inHandsOfProgram = context.membersMissingFrom(ValueFlowProgramNodes::escaped(), chain);
    return context.contains(context.valueOf(context.readWhatWasWritten(inHandsOfProgram, ValueFlowName::property(context.program(), key))), ValueFlowUniverse::unknownObject());
}

// Reflect.set, Object.assign, Object.defineProperty: called as a value nobody knows, it writes into an operand that may be an object nobody knows either.
static void letEscapedFunctionsChangeUnknownObject(ValueFlowRuleContext& context)
{
    if (context.holdsAnyOf(ClimbingSet(context.universe().builtinTables().heap().functionsThatChangeOperand()), ValueFlowProgramNodes::escaped()))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ObjectNobodyKnowsIsWritten), context.universe().unknownObjectAlone());
}

static void callMemberNobodyKnows(const ValueFlowStatement& statement, ValueFlowRuleContext& context, PropertyKeyID key, bool receiversAreToldApart)
{
    context.forEachMemberInLine(context.restrict(escapedMembersUnder(context, key), context.universe().builtinFunctions()), [&](ValueFlowObjectID member) {
        applyBuiltinFoundThroughUnknownObject(context.universe().builtinOf(member), statement, context);
    });
    // A built-in that the program has stored under the name escaped with the store, and no row lists it under that name. Escape covers what it does to its operands, but for the species that it reads.
    raiseHostFlag(context, ValueFlowHostNode::MemberNobodyKnowsIsCalled);

    letUnknownCodeWrite(context);
    letEscapedFunctionsChangeUnknownObject(context);
    // A proxy answers any name with what its trap returns.
    if (context.holdsAnyOf(ClimbingSet(context.universe().builtinTables().heap().objectsThatLetInUnseenCode()), ValueFlowProgramNodes::escaped()))
        letWhateverHasEscapedBeCalled(context);
    callOperandOfDescribedCall(statement, context, receiversAreToldApart && statement.key == key ? key : PropertyKeyID(), true);
}

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

static bool mayRefuseDefinition(ValueFlowRuleContext& context, ClimbingSet objects)
{
    constexpr OptionSet<ObjectMutation> mutations { ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity };
    const ValueFlowUniverse& universe = context.universe();
    if (context.meet(objects, universe.placeholdersAlone()) || context.meet(context.markedAmong(objects, mutations), ValueSets::anything) || context.holdsAnyOf(objects, ValueFlowProgramNodes::lost()))
        return true;
    if (!context.holdsAnyOf(objects, ValueFlowProgramNodes::escaped()))
        return false;
    return isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns) || context.meet(context.markedAmong(ClimbingSet(universe.unknownObjectAlone()), mutations), ValueSets::anything);
}

// A field that is declared without a value is defined as undefined. Nobody sees that value as long as super() calls the constructors that the classes name.
// Where the constructors above show the object to other code, a definition that throws leaves the ones in front of it to be seen.
static void observeDefinitionsBehindSuper(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees)
{
    const Program& program = context.program();
    LayoutID classLayout = program.at(statement.function).constructedClass;
    if (!classLayout)
        return;
    auto definitions = context.nodes().unobservedDefinitionsBehindSuperOf(classLayout);
    if (definitions.empty())
        return;
    LayoutID parent = predictedHeritageOf(program.at(classLayout));
    FunctionID named = parent ? program.at(parent).constructor : FunctionID();
    bool callsOtherConstructor = false;
    context.forEachMember(callees, [&](ValueFlowObjectID callee) {
        callsOtherConstructor |= context.universe().closureFunction(callee) != named;
    });
    for (const auto& entry : definitions) {
        auto definesOnObjectThatMayRefuse = [&] {
            return entry.areObservedWhereObjectRefusesDefinition && mayRefuseDefinition(context, context.valueOf(context.nodes().entryParameter(entry.initializer, *program.at(entry.initializer).thisParameterIndex())));
        };
        if (callsOtherConstructor || definesOnObjectThatMayRefuse())
            context.add(entry.value, context.universe().undefinedValue());
    }
}

static bool passesObjectThatLetsInUnseenCode(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    return std::ranges::any_of(context.operandsOf(statement).subspan(1), [&](const ValueFlowOperand& operand) {
        return listsObjectThatLetsInUnseenCode(context, operand);
    });
}

static ClimbingSet builtinFunctionsThatUnknownCalleeStandsFor(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (PropertyKeyID key = keyOfMemberCalledOnItsHolder(context, statement))
        return context.restrict(escapedMembersUnder(context, key), context.universe().builtinTables().heap().functionsThatCallOrBindOperand());
    return builtinFunctionsListedBesideUnknownObject(context, context.operandsOf(statement)[0].node);
}

// A function without `this` and a bound function do not. What nobody knows gets the receiver where the call lets it escape, which deferredExportsAt() sees.
static bool mayLookAtReceiver(ValueFlowRuleContext& context, ClimbingSet callees)
{
    const ValueFlowUniverse& universe = context.universe();
    bool mayLook = false;
    context.forEachMember(context.restrict(callees, universe.knownObjects()), [&](ValueFlowObjectID callee) {
        if (FunctionID function = universe.closureFunction(callee))
            mayLook |= context.program().at(function).has(FunctionFlags::HasThis);
        else if (BuiltinClassID builtinClass = universe.classOf(callee))
            mayLook |= builtinClass != BuiltinClasses::BoundFunction;
        else
            mayLook |= context.sets().contains(universe.builtinFunctions(), callee);
    });
    return mayLook;
}

// What lets in unseen code stays listed beside a value nobody knows, so once it has escaped every callee nobody knows lists it.
// Where such a callee is called, runUsedBuiltins() calls the functions of the host that have escaped, with operands nobody knows, once for the whole program. The operands of this call escape with it.
// Applied here as well, a function that reads a hundred options would read them off the operands of every such call.
static ClimbingSet withoutFunctionsOfHostThatWhoeverCallsWhatHasEscapedCalls(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ClimbingSet listed = context.restrict(callees, context.maker().intersect(universe.builtinFunctions(), universe.builtinTables().heap().objectsThatLetInUnseenCode()));
    if (!context.meet(listed, ValueSets::anything))
        return callees;
    ClimbingSet called = context.valueWithin(context.valueWithin(listed, ValueFlowProgramNodes::escaped()), context.nodes().ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders));
    Vector<ValueFlowObjectID, 8> ofHost;
    Vector<ValueFlowObjectID, 32> returned;
    context.forEachMember(called, [&](ValueFlowObjectID function) {
        if (builtins.at(universe.builtinOf(function)).origin != BuiltinOrigin::Host)
            return;
        ofHost.append(function);
        appendLoadersAndEvaluatorsReturnedBy(universe, builtins, builtins.canonicalID(universe.builtinOf(function)), returned);
    });
    if (ofHost.isEmpty())
        return callees;
    letWhateverHasEscapedBeCalled(context);
    if (statement.result && !returned.isEmpty())
        context.add(statement.result, ClimbingSet(context.maker().of(returned.span())));
    return context.restrict(callees, ValueSets::complement(context.maker().of(ofHost.span())));
}

void runCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet callees = context.valueOf(context.operandsOf(statement)[0]);
    if (ClimbingSet namespaces = constructs(statement.opcode) ? ClimbingSet() : context.restrict(context.valueOf(context.operandsOf(statement)[1]), universe.namespaces()); context.meet(namespaces, ValueSets::anything) && mayLookAtReceiver(context, callees)) {
        context.forEachMember(namespaces, [&](ValueFlowObjectID namespaceObject) {
            namespaceOf(context, universe.moduleOf(namespaceObject));
        });
    }
    context.proxiesAskedByTheirTrapsIn(context.operandsOf(statement)[0]);
    applyEventsOfCall(statement, context, callees);
    // Next to an unknown value, the callee shows nothing that has escaped. A library function that may call an evaluator and has arrived here is still called here.
    if (ValueFlowNodeID callee = context.operandsOf(statement)[0].node; callee && context.contains(callees, ValueFlowUniverse::unknownObject()))
        applyEventsOfCall(statement, context, context.valueWithin(ClimbingSet(universe.eventRaisingLibraryClosures()), callee));
    describeObjectsRefusedAsFunctions(context, callees);
    ValueFlowCallKind kind = constructs(statement.opcode) ? ValueFlowCallKind::Construct : ValueFlowCallKind::Call;
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));

    ValueSetID describedByEntries = kind == ValueFlowCallKind::Call ? universe.builtinTables().closuresThatEntriesDescribe() : ValueSets::nothing;
    context.forEachMemberInLine(context.restrict(callees, describedByEntries), [&](ValueFlowObjectID closure) {
        applyEntryDescribing(closure, kind, statement, context);
    });
    ValueSetID enteredClosures = context.maker().intersect(universe.closures(), ValueSets::complement(describedByEntries));
    if (context.meet(callees, enteredClosures)) {
        ClimbingSet closures = passOperandsToInstances(context, statement, context.restrict(callees, enteredClosures), kind);
        if (context.meet(closures, ValueSets::anything))
            passOperands(context, statement, closures, kind, passKnownReceiversToTheirCallees(context, statement, closures));
    }
    // A promise of the realm answers with the methods of promises, whatever else the callee may be.
    ClimbingSet listedCallees = callees;
    if (PropertyKeyID key = context.contains(callees, ValueFlowUniverse::unknownObject()) ? statement.key : PropertyKeyID()) {
        ClimbingSet all = context.valueOf(context.operandsOf(statement)[1]);
        ClimbingSet receivers = context.restrict(all, context.maker().unite(universe.sureInstancesOf(BuiltinClasses::Promise), promisesAllocatedByBuiltins(context, all)));
        if (context.meet(receivers, ValueSets::anything))
            listedCallees = context.unite(callees, context.valueWithin(ClimbingSet(universe.builtinFunctions()), context.lookup(receivers, ValueFlowName::property(context.program(), key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver)));
        // Beside the unknown object the built-ins that have escaped are not listed. A built-in object finds them all the same. An object of the program escapes with the call, which covers what they do to it.
        ClimbingSet hidden = context.restrict(escapedMembersUnder(context, key), universe.builtinFunctions());
        ClimbingSet known = context.restrict(all, universe.builtinObjects());
        if (context.meet(hidden, ValueSets::anything) && context.meet(known, ValueSets::anything))
            listedCallees = context.unite(listedCallees, context.valueWithin(hidden, context.lookup(known, ValueFlowName::property(context.program(), key), ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver)));
        if (statement.result && statement.function && !isLibraryCode(context.program(), context.program().at(statement.function)) && context.holdsAnything(context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects))) {
            AOT_THRESHOLD(unsigned, mostCoveredFunctionsLookedAt, 8);
            ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()));
            ClimbingSet gone = context.valueWithin(context.restrict(all, objectsOfProgram), ValueFlowProgramNodes::escaped());
            if (context.meet(gone, ValueSets::anything)) {
                unsigned lookedAt = 0;
                context.forEachMember(context.valueWithin(ClimbingSet(universe.closures()), context.readWhatWasWritten(gone, ValueFlowName::property(context.program(), key))), [&](ValueFlowObjectID closure) {
                    FunctionID function = universe.closureFunction(closure);
                    if (!function || ++lookedAt > mostCoveredFunctionsLookedAt || isLibraryCode(context.program(), context.program().at(function)))
                        return;
                    auto listModulesIn = [&](ValueFlowNodeID result) {
                        context.add(statement.result, context.valueWithin(ClimbingSet(universe.holdersOfLibraryExports()), result));
                    };
                    if (const auto* summary = context.nodes().resultSummaryOf(function))
                        listModulesIn(summary->ownResult);
                    else {
                        for (unsigned index = 0; index < context.program().at(function).results.size(); ++index)
                            listModulesIn(context.nodes().result(function, index));
                    }
                });
            }
        }
    }
    ValueSetID receiversFollowedBehindUnknownObject = ValueSets::nothing;
    if (context.contains(callees, ValueFlowUniverse::unknownObject())) {
        ClimbingSet handingOn = builtinFunctionsThatUnknownCalleeStandsFor(statement, context);
        if (context.meet(handingOn, ValueSets::anything) && passesObjectThatLetsInUnseenCode(statement, context)) {
            listedCallees = context.unite(listedCallees, handingOn);
            receiversFollowedBehindUnknownObject = universe.builtinTables().heap().functionsThatCallOrBindOperand();
        }
    }
    if (context.contains(callees, ValueFlowUniverse::unknownObject()))
        listedCallees = withoutFunctionsOfHostThatWhoeverCallsWhatHasEscapedCalls(statement, context, listedCallees);
    callListedBuiltins(statement, context, listedCallees, kind, receiversFollowedBehindUnknownObject);
    allocateRowsOfQuery(statement, context);
    if (context.contains(callees, ValueFlowUniverse::unknownObject())) {
        const ValueFlowRareFactsOfText* facts = context.rareFactsOfText(statement);
        BuiltinID named = facts ? facts->entryNamed : BuiltinID();
        const BuiltinTable& builtins = context.program().builtins();
        // Behind a built-in of the engine only a Bun that the program named is looked for.
        auto mayHandOnNamedBun = [&] {
            ClimbingSet mark = only(context, ValueFlowUniverse::markOfNamedBun());
            return std::ranges::any_of(context.operandsOf(statement).subspan(1), [&](const ValueFlowOperand& operand) {
                return operand.node && context.holdsAnyOf(mark, operand.node);
            });
        };
        if (named && context.sets().contains(universe.builtinFunctions(), universe.objectOf(named)) && (builtins.at(named).origin == BuiltinOrigin::Host || mayHandOnNamedBun()))
            applyBuiltin(named, kind, statement, context);
    }
    for (ValueFlowObjectID placeholder : { ValueFlowUniverse::untrackedObject(), ValueFlowUniverse::unknownObject() }) {
        if (!context.contains(callees, placeholder))
            continue;
        PropertyKeyID keyOfMember = placeholder == ValueFlowUniverse::unknownObject() ? keyOfMemberCalledOnItsHolder(context, statement) : PropertyKeyID();
        if (keyOfMember && findsValueNobodyKnowsThatProgramHasStored(context, context.valueOf(context.operandsOf(statement)[1]), keyOfMember))
            keyOfMember = { };
        if (placeholder == ValueFlowUniverse::unknownObject() && isCodeThatRecordsDescribe(context.universe(), statement.function))
            callOperandOfDescribedCall(statement, context);
        else if (keyOfMember)
            callMemberNobodyKnows(statement, context, keyOfMember, context.meet(callees, enteredClosures));
        else
            passOperands(context, statement, only(context, placeholder), kind, { }, isCodeBehindDeclaredEntries(context.program(), context.universe().builtinTables(), statement.function) ? ValueFlowCalleeOrigin::OperandOfAppliedEntry : ValueFlowCalleeOrigin::Value);
    }

    bool isSuperConstruct = statement.opcode == Opcode::SuperConstruct || statement.opcode == Opcode::SuperConstructList;
    if (isSuperConstruct)
        observeDefinitionsBehindSuper(statement, context, callees);
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
    bool callersSayWhatTheyLeaveOut = kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct || kind == ValueFlowCallKind::CallByHost;
    for (unsigned index = 0; index < positional; ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(function, first + index);
        context.connect(group.argumentAt(index), parameter);
        context.connect(group.otherArguments, parameter);
        if (kind == ValueFlowCallKind::CallByNativeCode && index < numberOfGroupedArguments)
            context.connect(group.anyArgumentLeftOut(), parameter);
        else if (!callersSayWhatTheyLeaveOut || index >= numberOfGroupedArguments)
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
    if (!findCallThatAloneTakesRestArray(context, function))
        return;

    for (unsigned index = positional; index < numberOfGroupedArguments; ++index) {
        ValueFlowNodeID argument = context.written(rest, restArgument(index - positional));
        context.connect(group.argument(index), argument);
        if (!callersSayWhatTheyLeaveOut)
            context.add(argument, universe.undefinedValue());
    }
    context.connect(group.laterArguments, context.written(rest, laterRestArguments));
    context.connect(group.otherArguments, context.written(rest, otherRestArguments));
}

static bool isAllocatedByBaseClassOfProgram(const Program& program, LayoutID layout)
{
    for (size_t steps = 0; steps < program.layouts.size() && layout && program.at(layout).has(LayoutFlags::HasHeritage); ++steps)
        layout = predictedHeritageOf(program.at(layout));
    return layout && !program.at(layout).has(LayoutFlags::HasHeritage) && !program.at(layout).has(LayoutFlags::IsConstructorFunction);
}

struct AllocatedInstances {
    ClimbingSet instances;
    bool isCollected { false };
    bool holdsObjectWithoutLayout { false };
};

static void passAllocatedInstances(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, AllocatedInstances& allocated, ValueFlowNodeID thisParameter)
{
    auto add = [&](ValueFlowNodeID node) {
        if (!node)
            return;
        if (allocated.holdsObjectWithoutLayout)
            context.noteUnknownSource(ValueFlowUnknownSource::ObjectWithoutLayout);
        context.add(node, allocated.instances);
    };
    if (allocated.isCollected)
        return add(thisParameter);

    const ValueFlowUniverse& universe = context.universe();
    const Program& program = context.program();
    ClimbingSet newTargets = context.valueOf(group.newTarget);
    if (context.contains(newTargets, ValueFlowUniverse::untrackedObject())) {
        ClimbingSet lostCallables = context.valueWithin(ClimbingSet(universe.callables()), ValueFlowProgramNodes::lost());
        newTargets = context.unite(context.restrict(newTargets, ValueSets::complement(universe.untrackedObjectAlone())), lostCallables);
    }
    Vector<ValueFlowObjectID, 16> instances;
    context.forEachMember(newTargets, [&](ValueFlowObjectID newTarget) {
        FunctionID constructor = universe.closureFunction(newTarget);
        LayoutID layout = constructor ? program.at(constructor).constructedClass : LayoutID();
        if (!constructor) {
            allocated.holdsObjectWithoutLayout = true;
            if (context.meet(only(context, newTarget), universe.knownObjects())) {
                // Flow does not follow the new object, so it records no link from it to its prototype.
                auto held = context.masked(context.lookup(only(context, newTarget), prototypeProperty(context)), ValueSets::complement(universe.builtinObjects()));
                context.connect(held, ValueFlowProgramNodes::escaped());
                context.mark(context.restrict(context.valueOf(held), universe.objects()), ObjectMutation::BecomePrototype);
            }
            return;
        }
        ValueFlowObjectID instance = layout ? universe.instanceOf(layout) : universe.constructedObjectOf(constructor);
        instances.append(instance);
        if (layout && isAllocatedByBaseClassOfProgram(program, layout))
            context.add(context.written(only(context, instance), ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey)), universe.undefinedValue());
        ValueFlowNodeID held = context.lookup(only(context, newTarget), prototypeProperty(context));
        context.connect(held, context.written(only(context, instance), prototypeLinkName));
        if (!layout && context.meet(context.valueOf(held), ValueSets::complement(universe.objects())))
            context.add(context.written(only(context, instance), prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    });
    allocated.instances = ClimbingSet(context.maker().of(instances.span()));
    if (allocated.holdsObjectWithoutLayout)
        allocated.instances = context.unite(allocated.instances, unknownObject(context, ValueFlowUnknownSource::ObjectWithoutLayout));
    allocated.isCollected = true;
    add(group.result);
    add(thisParameter);
}

static bool mayBeBelow(const Program& program, LayoutID layout, LayoutID base)
{
    for (size_t steps = 0; layout && steps <= program.layouts.size(); ++steps) {
        if (layout == base)
            return true;
        if (!program.at(layout).has(LayoutFlags::HasHeritage))
            return false;
        layout = predictedHeritageOf(program.at(layout));
    }
    return true;
}

// The unknown object stands for every constructor that has escaped. A constructor function makes an instance of new.target's class when that class is its own or below it.
static void passInstancesOfEscapedConstructors(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, LayoutID constructed, ValueFlowNodeID thisParameter)
{
    if (!constructed || !context.contains(context.valueOf(group.newTarget), ValueFlowUniverse::unknownObject()))
        return;
    const ValueFlowUniverse& universe = context.universe();
    const Program& program = context.program();
    Vector<ValueFlowObjectID, 16> instances;
    context.forEachMember(context.valueWithin(ClimbingSet(universe.constructorsWithLayouts()), ValueFlowProgramNodes::escaped()), [&](ValueFlowObjectID newTarget) {
        LayoutID layout = program.at(universe.closureFunction(newTarget)).constructedClass;
        if (!mayBeBelow(program, layout, constructed))
            return;
        ValueFlowObjectID instance = universe.instanceOf(layout);
        instances.append(instance);
        context.connect(context.lookup(only(context, newTarget), prototypeProperty(context)), context.written(only(context, instance), prototypeLinkName));
    });
    ClimbingSet made(context.maker().of(instances.span()));
    context.add(group.result, made);
    if (thisParameter)
        context.add(thisParameter, made);
}

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
    if (isConstruction && callee.kind == FunctionKind::Plain) {
        passAllocatedInstances(context, group, allocated, thisParameterOf(context, function));
        passInstancesOfEscapedConstructors(context, group, callee.constructedClass, thisParameterOf(context, function));
    } else
        context.connect(group.thisValue, thisParameterOf(context, function));
    passArguments(context, group, kind, function, callee);

    context.connect(nodes.thrown(function), group.thrown);
    if (callee.isUnsplitCoroutine()) {
        context.add(group.result, only(context, universe.coroutineResultOf(function)));
        return;
    }
    const auto* summary = isConstruction ? nullptr : nodes.resultSummaryOf(function);
    if (!summary) {
        context.connect(isConstruction ? context.masked(nodes.result(function, 0), universe.objects()) : nodes.result(function, 0), group.result);
        return;
    }

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
    ValueFlowCallGroup group = context.group(key, kind, ValueFlowCalleeOrigin::MemberFoundUnderName, ValueFlowArgumentsLeftOut::SaidByCaller);

    auto handOperandsToCodeNobodySees = [&] {
        ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
        ValueFlowNodeID operands = context.nodes().ofHost(ValueFlowHostNode::OperandsHandedToUnseenCode);
        context.connect(group.thisValue, operands);
        for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
            context.connect(group.argument(index), operands);
        context.connect(group.laterArguments, operands);
        context.connect(group.otherArguments, operands);
        context.connect(group.thisValue, escaped);
        if (group.newTarget)
            context.connect(group.newTarget, escaped);
        for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
            carryOut(context, group.argument(index));
        carryOut(context, group.laterArguments);
        carryOut(context, group.otherArguments);
        context.add(group.result, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
        context.add(group.thrown, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
    };
    if (context.contains(key, calleeOfDescribedCall(universe))) {
        handOperandsToCodeNobodySees();
        key = context.restrict(key, ValueSets::complement(context.maker().of(calleeOfDescribedCall(universe))));
        if (!context.meet(key, ValueSets::anything))
            return;
    }

    ClimbingSet listed = context.restrict(key, universe.knownObjects());
    ClimbingSet lost = context.contains(key, ValueFlowUniverse::untrackedObject()) ? context.valueWithin(ClimbingSet(universe.callables()), ValueFlowProgramNodes::lost()) : ClimbingSet();
    bool callsUnknownObject = context.contains(key, ValueFlowUniverse::unknownObject());

    bool isSpelledOut = kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct;
    if (kind != ValueFlowCallKind::CallByHost)
        applyEventsOfCall(statement, context, context.unite(isSpelledOut ? context.restrict(listed, universe.builtinFunctions()) : listed, lost));

    ClimbingSet callees = context.unite(listed, lost);
    AllocatedInstances allocated;
    ClimbingSet dealtWithOneByOne = callees;
    if (callsUnknownObject || context.contains(key, ValueFlowUniverse::untrackedObject()))
        dealtWithOneByOne = context.restrict(callees, ValueSets::complement(context.maker().intersect(universe.otherCallables(), ValueSets::complement(universe.builtinObjects()))));
    bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
    context.forEachMemberInLine(dealtWithOneByOne, [&](ValueFlowObjectID callee) {
        if (FunctionID function = universe.closureFunction(callee)) {
            if (kind != ValueFlowCallKind::CallByHost && !isConstruction(kind) && context.sets().contains(universe.builtinTables().closuresThatEntriesDescribe(), callee))
                applyEntryDescribing(callee, isAccessorCall ? ValueFlowCallKind::Call : kind, group, context);
            else
                enter(context, group, kind, callee, function, allocated);
            return;
        }
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

    letUnknownCodeWrite(context);
    letEscapedFunctionsChangeUnknownObject(context);
    handOperandsToCodeNobodySees();
}

void runNewClass(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const Program& program = context.program();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = context.operationOf(statement);
    ClimbingSet constructors = context.valueOf(operands[0]);
    ClimbingSet prototype = only(context, universe.prototypeOf(operation.layout()));

    context.add(context.written(constructors, prototypeProperty(context)), prototype);
    context.connect(operands[0], context.written(prototype, ValueFlowName::property(program.keyOf(BuiltinNames::constructor))));
    if (operation.heritageOperand()) {
        context.connect(operands[1], context.written(constructors, prototypeLinkName));
        context.connect(operands[2], context.written(prototype, prototypeLinkName));
        if (context.contains(context.valueOf(operands[1]), ValueFlowUniverse::primitive(PrimitiveSet::Null)))
            context.add(context.written(constructors, prototypeLinkName), only(context, universe.objectOf(Builtins::FunctionPrototype)));
    }

    bool hasEscaped = context.holdsAnyOf(constructors, ValueFlowProgramNodes::escaped());
    bool isLost = context.holdsAnyOf(constructors, ValueFlowProgramNodes::lost());
    for (const StaticField& field : program.at(operation.layout()).staticFields) {
        ValueFlowNodeID variable = context.nodes().variable(field.variable);
        context.connect(variable, context.written(constructors, field.key ? ValueFlowName::property(program, field.key) : ValueFlowName { ValueFlowNameKind::UnknownKey, 0 }));
        ValueFlowName found = field.key ? ValueFlowName::property(program, field.key) : ValueFlowName { ValueFlowNameKind::EveryProperty, 0 };
        context.connect(context.readWhatWasWritten(withSourcesOfCopies(context, context.restrict(constructors, universe.knownObjects())), found), variable);
        if (hasEscaped)
            context.connect(context.readWhatWasWritten(ClimbingSet(universe.unknownObjectAlone()), found), variable);
        if (isLost)
            context.connect(context.readWhatWasWritten(ClimbingSet(universe.untrackedObjectAlone()), found), variable);
    }
}

static ValueFlowName nameOfMember(const Program& program, MethodKind kind, PropertyKeyID key)
{
    switch (kind) {
    case MethodKind::Method:
        return key ? ValueFlowName::property(program, key) : unknownKeyName;
    case MethodKind::Getter:
        return key ? ValueFlowName::getter(program, key) : ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey);
    case MethodKind::Setter:
        return key ? ValueFlowName::setter(program, key) : ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runDefineMethod(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = context.operationOf(statement);
    ClimbingSet holder = operation.side() == ClassSide::Static ? context.valueOf(operands[0]) : only(context, universe.prototypeOf(operation.layout()));

    auto define = [&](PropertyKeyID key) {
        context.connect(operands[1], context.written(holder, nameOfMember(context.program(), operation.methodKind(), key)));
        context.add(context.written(holder, ValueFlowCellName { ValueFlowNameKind::NamesOfMethods }), only(context, universe.keyConstant(key)));
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
    if (operation.methodKind() != MethodKind::Setter && context.meet(keys, ValueSets::complement(context.maker().unite(universe.keys(), context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::String))))))
        context.connect(operands[1], context.nodes().ofHost(isWrittenInProgram(context.program().at(statement.function)) ? ValueFlowHostNode::StoredUnderSymbolsNobodyKnows : ValueFlowHostNode::StoredByLibraryUnderSymbolsNobodyKnows));
    context.connect(operands[1], context.written(holder, nameOfMember(context.program(), operation.methodKind(), { })));
    context.add(context.written(holder, ValueFlowCellName { ValueFlowNameKind::NamesOfMethods }), universe.unknownObjectAlone());
}

void runReadOfPrototypeProperty(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet holders = context.valueOf(context.operandsOf(statement)[0]);
    ValueSetID null = context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Null));
    ValueSetID answers = statement.opcode == Opcode::ResolveHeritage ? context.maker().unite(universe.objects(), null) : universe.objects();
    // A getter runs, and the operation throws, whether or not anybody takes the answer.
    ValueFlowNodeID held = context.lookup(holders, prototypeProperty(context));
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    if (!statement.result)
        return;
    context.connect(context.masked(held, answers), statement.result);
    switch (statement.opcode) {
    case Opcode::ResolveHeritage:
        if (context.meet(holders, null) || context.contains(holders, ValueFlowUniverse::unknownObject()))
            context.add(statement.result, null);
        break;
    case Opcode::GetConstructPrototype: {
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
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
