/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName elements { ValueFlowNameKind::Elements, 0 };
static constexpr ValueFlowName prototypeLink { ValueFlowNameKind::PrototypeLink, 0 };

static ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

static ValueFlowName prototypeProperty(ValueFlowRuleContext& context)
{
    return ValueFlowName::property(context.program().keyOf(BuiltinNames::prototype));
}

static bool constructs(Opcode opcode)
{
    return opcode != Opcode::Call && opcode != Opcode::CallList;
}

static bool passesList(Opcode opcode)
{
    return opcode == Opcode::CallList || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList;
}

static ValueSetID otherThanClosures(ValueFlowRuleContext& context)
{
    return ValueSets::complement(context.universe().closures());
}

// The entry of a built-in is applied with the operands of this call. What cannot be called throws.
static void leaveSeenCode(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees, ValueFlowCallKind kind)
{
    const ValueFlowUniverse& universe = context.universe();
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context));

    // Where the unknown object may be called the operands escape, everything may happen to them and the result is unknown: no entry adds to that.
    bool mayCallAnything = context.contains(callees, ValueFlowUniverse::unknownObject());
    context.forEachMember(context.restrict(callees, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
        if (mayCallAnything)
            noteCallOfBuiltin(universe.builtinOf(callee), kind, context);
        else
            applyBuiltin(universe.builtinOf(callee), kind, statement, context);
    });
    if (!mayCallAnything) {
        if (context.meet(callees, universe.otherCallables()))
            callOtherCallables(context.restrict(callees, universe.otherCallables()), kind, statement, context);
        return;
    }

    openEscapedObjects(context);
    // A bound function among the callees is not followed here, so what it is bound to has to be found by escape.
    context.add(ValueFlowProgramNodes::escaped(), context.restrict(callees, universe.otherCallables()));
    for (const ValueFlowOperand& operand : context.operandsOf(statement).subspan(1))
        context.connect(operand, ValueFlowProgramNodes::escaped());
    if (statement.result)
        context.add(statement.result, unknownValue(context));
}

// A call without a receiver passes undefined.
static void passThis(ValueFlowRuleContext& context, const ValueFlowStatement& statement, const ValueFlowOperand& receiver, ValueFlowNodeID thisValue)
{
    if (context.program().at(statement.function).at(statement.site).operand(1))
        context.connect(receiver, thisValue);
    else
        context.add(thisValue, context.universe().undefinedValue());
}

void passNoFurtherArguments(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, unsigned numberOfArguments)
{
    for (unsigned index = numberOfArguments; index < numberOfGroupedArguments; ++index)
        context.add(group.argument(index), context.universe().undefinedValue());
}

// The operands of all six operations: the callee, then this or new.target, then the arguments or the list of them.
void runCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    ClimbingSet callees = context.valueOf(operands[0]);
    applyEventsOfCall(statement, context, callees);
    ValueFlowCallKind kind = constructs(statement.opcode) ? ValueFlowCallKind::Construct : ValueFlowCallKind::Call;
    std::span<const ValueFlowOperand> arguments = operands.subspan(2);
    // Any call may find the stack full, and that error is made by the run time.
    if (statement.thrown)
        context.add(statement.thrown, universe.unknownObjectAlone());

    if (context.meet(callees, otherThanClosures(context)))
        leaveSeenCode(statement, context, callees, kind);
    if (!context.meet(callees, universe.closures()))
        return;

    ClimbingSet closures = context.restrict(callees, universe.closures());
    if (statement.copiedCallee) {
        // The operands go into the copy, and what comes back comes out of it: those are edges of the body. The function's own body is for other callers.
        ValueSetID copied = context.maker().of(universe.closureOf(statement.copiedCallee));
        if (context.meet(closures, copied))
            context.reach(statement.copiedCallee);
        closures = context.restrict(closures, ValueSets::complement(copied));
        if (!context.meet(closures, ValueSets::anything))
            return;
    }

    ValueFlowCallGroup group = context.group(closures, kind);
    if (kind == ValueFlowCallKind::Call)
        passThis(context, statement, operands[1], group.thisValue);
    else {
        // Without the operand the callee is new.target.
        bool hasNewTarget = !!context.program().at(statement.function).at(statement.site).operand(1);
        context.connect(operands[hasNewTarget ? 1 : 0], group.newTarget);
    }
    if (passesList(statement.opcode)) {
        context.connect(context.lookup(context.valueOf(arguments[0]), elements), group.otherArguments);
        passNoFurtherArguments(context, group, 0);
    } else {
        for (unsigned index = 0; index < arguments.size(); ++index)
            context.connect(arguments[index], index < numberOfGroupedArguments ? group.argument(index) : group.otherArguments);
        passNoFurtherArguments(context, group, arguments.size());
    }
    if (statement.result)
        context.connect(group.result, statement.result);
    if (statement.thrown)
        context.connect(group.thrown, statement.thrown);
}

static void passArguments(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, ValueFlowCallKind kind, FunctionID function, const Function& callee)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    unsigned first = callee.firstParameterIndex();
    unsigned positional = callee.numberOfPositionalParameters();
    for (unsigned index = 0; index < positional; ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(function, first + index);
        if (index < numberOfGroupedArguments)
            context.connect(group.argument(index), parameter);
        context.connect(group.otherArguments, parameter);
        // A call that an operation spells out has said which positions it leaves empty. Nobody says so for the others.
        bool callersSayWhatTheyLeaveOut = (kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct) && index < numberOfGroupedArguments;
        if (!callersSayWhatTheyLeaveOut)
            context.add(parameter, universe.undefinedValue());
    }

    std::optional<unsigned> hiddenRest = callee.hiddenRestParameterIndex();
    bool hasRest = callee.has(FunctionFlags::HasRestParameter);
    if (!hasRest && !hiddenRest)
        return;
    ClimbingSet rest = only(context, universe.restArrayOf(function));
    context.add(nodes.entryParameter(function, hasRest ? first + positional : *hiddenRest), rest);
    ValueFlowNodeID contents = context.written(rest, elements);
    for (unsigned index = positional; index < numberOfGroupedArguments; ++index)
        context.connect(group.argument(index), contents);
    context.connect(group.otherArguments, contents);
}

// What a construction allocates depends on new.target alone.
static void allocateThis(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, ValueFlowNodeID thisParameter)
{
    const ValueFlowUniverse& universe = context.universe();
    const Program& program = context.program();
    context.forEachMember(context.valueOf(group.newTarget), [&](ValueFlowObjectID newTarget) {
        FunctionID constructor = universe.closureFunction(newTarget);
        LayoutID layout = constructor ? program.at(constructor).constructedClass : LayoutID();
        ClimbingSet instance = layout ? only(context, universe.instanceOf(layout)) : ClimbingSet(universe.unknownObjectAlone());
        if (thisParameter)
            context.add(thisParameter, instance);
        context.add(group.result, instance);
        if (!layout)
            return;
        context.connect(context.lookup(only(context, newTarget), prototypeProperty(context)), context.written(instance, prototypeLink));
    });
}

static void enter(ValueFlowRuleContext& context, const ValueFlowCallGroup& group, ValueFlowCallKind kind, ValueFlowObjectID closure, FunctionID function)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& callee = context.program().at(function);
    bool isConstruction = Typed::isConstruction(kind);
    if (isConstruction ? callee.constructorForm() == ConstructorForm::NotConstructor : callee.constructorForm() != ConstructorForm::NotConstructor && callee.kind != FunctionKind::Plain) {
        context.add(group.thrown, universe.unknownObjectAlone());
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
        allocateThis(context, group, callee.thisParameterIndex() ? nodes.entryParameter(function, *callee.thisParameterIndex()) : ValueFlowNodeID());
    else if (auto index = callee.thisParameterIndex())
        context.connect(group.thisValue, nodes.entryParameter(function, *index));
    passArguments(context, group, kind, function, callee);

    context.connect(nodes.thrown(function), group.thrown);
    if (callee.isUnsplitCoroutine()) {
        // What the body returns goes into that object: the statement of kind Coroutine in the body sees to it.
        context.add(group.result, only(context, universe.coroutineResultOf(function)));
        return;
    }
    context.connect(isConstruction ? context.masked(nodes.result(function, 0), universe.objects()) : nodes.result(function, 0), group.result);
}

void runCallGroup(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet callees = context.setOfKey();
    ValueFlowCallKind kind = context.callKindOfKey();
    ValueFlowCallGroup group = context.group(callees, kind);
    // A call that an operation spells out brings the functions of the program here and has said itself whether seen code calls them. A built-in comes here from a call that another built-in passes on.
    bool isSpelledOut = kind == ValueFlowCallKind::Call || kind == ValueFlowCallKind::Construct;
    applyEventsOfCall(statement, context, isSpelledOut ? context.restrict(callees, universe.builtinFunctions()) : callees);

    // Where the unknown object may be called the operands escape, everything may happen to them and the result is unknown: no entry adds to that.
    bool leavesSeenCode = context.contains(callees, ValueFlowUniverse::unknownObject());
    context.forEachMember(callees, [&](ValueFlowObjectID callee) {
        if (FunctionID function = universe.closureFunction(callee)) {
            enter(context, group, kind, callee, function);
            return;
        }
        // A lookup finds accessors and data properties of the table, which are applied in the mode of the group. A function of the table that the program has made an accessor is called like any function.
        bool isAccessorCall = kind == ValueFlowCallKind::Get || kind == ValueFlowCallKind::Set;
        bool isFunction = context.sets().contains(universe.builtinFunctions(), callee);
        BuiltinID builtin = universe.builtinOf(callee);
        if (!builtin || (!isFunction && !isAccessorCall))
            return;
        ValueFlowCallKind kindOfEntry = isFunction && isAccessorCall ? ValueFlowCallKind::Call : kind;
        if (leavesSeenCode)
            noteCallOfBuiltin(builtin, kindOfEntry, context);
        else
            applyBuiltin(builtin, kindOfEntry, group, context);
    });
    if (!leavesSeenCode) {
        if (context.meet(callees, universe.otherCallables()))
            callOtherCallables(context.restrict(callees, universe.otherCallables()), kind, group, context);
        return;
    }

    openEscapedObjects(context);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    context.add(escaped, context.restrict(callees, universe.otherCallables()));
    context.connect(group.thisValue, escaped);
    if (group.newTarget)
        context.connect(group.newTarget, escaped);
    for (unsigned index = 0; index < numberOfGroupedArguments; ++index)
        context.connect(group.argument(index), escaped);
    context.connect(group.otherArguments, escaped);
    context.add(group.result, unknownValue(context));
    context.add(group.thrown, unknownValue(context));
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
        context.connect(operands[1], context.written(constructors, prototypeLink));
        context.connect(operands[2], context.written(prototype, prototypeLink));
    }

    // A static field is a variable and a property of the class object at once.
    for (const StaticField& field : program.at(operation.layout()).staticFields) {
        ValueFlowNodeID variable = context.nodes().variable(field.variable);
        context.connect(variable, context.written(constructors, field.key ? ValueFlowName::property(field.key) : ValueFlowName { ValueFlowNameKind::UnknownKey, 0 }));
        context.connect(context.lookup(constructors, field.key ? ValueFlowName::property(field.key) : ValueFlowName { ValueFlowNameKind::EveryProperty, 0 }), variable);
    }
}

static ValueFlowName nameOfMember(MethodKind kind, PropertyKeyID key)
{
    switch (kind) {
    case MethodKind::Method:
        return ValueFlowName::property(key);
    case MethodKind::Getter:
        return ValueFlowName::getter(key);
    case MethodKind::Setter:
        return ValueFlowName::setter(key);
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

    if (PropertyKeyID key = operation.key()) {
        context.connect(operands[1], context.written(holder, nameOfMember(operation.methodKind(), key)));
        return;
    }
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
    context.connect(context.masked(context.lookup(holders, prototypeProperty(context)), universe.objects()), statement.result);
    switch (statement.opcode) {
    case Opcode::ResolveHeritage:
        // `extends null`
        context.add(statement.result, context.maker().of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
        break;
    case Opcode::GetConstructPrototype:
        // Where the property holds no object.
        context.add(statement.result, only(context, universe.objectOf(Builtins::ObjectPrototype)));
        break;
    default:
        break;
    }
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
