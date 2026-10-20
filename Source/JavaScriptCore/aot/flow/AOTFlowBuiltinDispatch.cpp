/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

FlowTypeMatch unitedMatch(FlowTypeMatch a, FlowTypeMatch b)
{
    if (a == FlowTypeMatch::NotReached)
        return b;
    if (b == FlowTypeMatch::NotReached || a == b)
        return a;
    return FlowTypeMatch::Sometimes;
}

static FlowTypeMatch strongerMatch(FlowTypeMatch a, FlowTypeMatch b)
{
    if (a == FlowTypeMatch::Always || b == FlowTypeMatch::Always)
        return FlowTypeMatch::Always;
    if (a == FlowTypeMatch::Sometimes || b == FlowTypeMatch::Sometimes)
        return FlowTypeMatch::Sometimes;
    return FlowTypeMatch::Never;
}

static FlowTypeMatch matchSinglePrimitive(const BuiltinType& type, BuiltinPrimitive primitive)
{
    return type.isAnything() || type.primitives.contains(primitive) ? FlowTypeMatch::Always : FlowTypeMatch::Never;
}

uint16_t primitivesOf(const BuiltinType& type)
{
    uint16_t primitives = 0;
    for (BuiltinPrimitive primitive : type.primitives) {
        switch (primitive) {
        case BuiltinPrimitive::Undefined:
            primitives |= Primitive::Undefined;
            break;
        case BuiltinPrimitive::Null:
            primitives |= Primitive::Null;
            break;
        case BuiltinPrimitive::Boolean:
            primitives |= Primitive::Boolean;
            break;
        case BuiltinPrimitive::Number:
            primitives |= Primitive::Number;
            break;
        case BuiltinPrimitive::String:
            primitives |= Primitive::AnyString;
            break;
        case BuiltinPrimitive::Symbol:
            primitives |= Primitive::AnySymbol;
            break;
        case BuiltinPrimitive::BigInt:
            primitives |= Primitive::BigInt;
            break;
        case BuiltinPrimitive::NotPassed:
            break;
        }
    }
    if (!type.stringLiterals.empty())
        primitives |= Primitive::LiteralString;
    return primitives;
}

static bool acceptsOnlyIntegers(const BuiltinType& type)
{
    return type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || !type.integers.isEmpty();
}

static bool acceptsOnlyListedStrings(const BuiltinType& type)
{
    return !type.primitives.contains(BuiltinPrimitive::String) && !type.stringLiterals.empty();
}

static bool isListedString(const Program& program, const BuiltinType& type, StringID string)
{
    return std::ranges::any_of(type.stringLiterals, [&](BuiltinName name) {
        return program.stringOf(name) == string;
    });
}

FlowTypeMatch matchPrimitives(const FlowSolver& solver, const FlowValue& value, const BuiltinType& type)
{
    const FlowValueTable& values = solver.graph().values();
    uint16_t present = value.primitives & ~Primitive::Absent;
    uint16_t accepted = present & primitivesOf(type);
    bool inside = false;
    bool outside = present & ~accepted;

    if (acceptsOnlyIntegers(type)) {
        if (accepted & Primitive::Double)
            outside = true;
        if (accepted & Primitive::Integer) {
            Range integers = values.rangeOf(value.integers);
            Range acceptedIntegers = type.integers.isEmpty() ? Range::full() : Range { type.integers.minimum, type.integers.maximum };
            if (!integers.intersectWith(acceptedIntegers).isEmpty())
                inside = true;
            if (!integers.isSubsetOf(acceptedIntegers))
                outside = true;
        }
        accepted &= ~Primitive::Number;
    }

    if (acceptsOnlyListedStrings(type)) {
        if (present & Primitive::String) {
            inside = true;
            outside = true;
        }
        for (StringID string : values.stringsOf(value.strings)) {
            if (isListedString(solver.program(), type, string))
                inside = true;
            else
                outside = true;
        }
        accepted &= ~Primitive::LiteralString;
    }

    if (accepted)
        inside = true;
    if (!inside)
        return FlowTypeMatch::Never;
    return outside ? FlowTypeMatch::Sometimes : FlowTypeMatch::Always;
}

static FlowTypeMatch matchFunction(const FlowSolver& solver, FlowObjectID object)
{
    const FlowObjectTable& objects = solver.objects();
    if (objects.isCallable(object))
        return FlowTypeMatch::Always;

    switch (objects.kindOf(object)) {
    case FlowObjectKind::Opaque:
        return FlowTypeMatch::Sometimes;
    case FlowObjectKind::Instance:
        // A proxy is callable when its target is.
        return isFunctionClass(solver.builtins(), objects.at(object).builtinClass) ? FlowTypeMatch::Sometimes : FlowTypeMatch::Never;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return FlowTypeMatch::Never;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowTypeMatch matchPlainData(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return FlowTypeMatch::Sometimes;
    case FlowObjectKind::Closure:
    case FlowObjectKind::ModuleNamespace:
        return FlowTypeMatch::Never;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowTypeMatch matchAllocationWithClass(const FlowSolver& solver, FlowObjectID object, FlowTypeMatch otherwise)
{
    if (!solver.objects().hasLayout(object))
        return FlowTypeMatch::Sometimes;

    const Layout& layout = solver.program().at(solver.objects().at(object).layout);
    return layout.has(LayoutFlags::HasHeritage) ? FlowTypeMatch::Sometimes : otherwise;
}

// What is done to an escaped object through a receiver nobody knows is recorded on the unknown object.
static bool mayLoseBuiltinBehavior(const FlowSolver& solver, FlowObjectID object)
{
    const FlowRecords& records = solver.records();
    OptionSet<ObjectMutation> mutations = records.possibleMutations(object);
    if (solver.hasReachedNativeCode(object))
        mutations.add(records.possibleMutations(unknownFlowObject));
    return mutations.containsAny(builtinBehaviorMutations);
}

static FlowTypeMatch matchInstanceOfTestedClass(const FlowSolver& solver, FlowObjectID object, const BuiltinTypePart& part)
{
    if (!part.contents.empty())
        return FlowTypeMatch::Sometimes;
    if (part.pristine.isEmpty())
        return FlowTypeMatch::Always;
    if (pristineTestOf(solver.builtins(), solver.objects().at(object).builtinClass) == PristineTest::NeverPasses)
        return FlowTypeMatch::Sometimes;
    return mayLoseBuiltinBehavior(solver, object) ? FlowTypeMatch::Sometimes : FlowTypeMatch::Always;
}

static FlowTypeMatch matchInstancePart(const FlowSolver& solver, FlowObjectID object, const BuiltinTypePart& part)
{
    const BuiltinTable& builtins = solver.builtins();
    const FlowObject& record = solver.objects().at(object);
    bool hasInstanceTest = builtins.at(part.builtinClass).instanceTest != BuiltinInstanceTest::None;
    FlowTypeMatch otherwise = hasInstanceTest ? FlowTypeMatch::Never : FlowTypeMatch::Sometimes;

    switch (record.kind) {
    case FlowObjectKind::Unknown:
        return FlowTypeMatch::Sometimes;
    case FlowObjectKind::Opaque:
        // The site names the class of what the built-in made, which may be of a class below it. A site without a class says nothing.
        if (!record.builtinClass || builtins.isSameOrSubclassOf(record.builtinClass, part.builtinClass) || builtins.isSameOrSubclassOf(part.builtinClass, record.builtinClass))
            return FlowTypeMatch::Sometimes;
        return otherwise;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        // A function of the table is an instance of Function and of no class below or beside it.
        if (isCallableBuiltin(builtins, record.builtin) && !builtins.isSameOrSubclassOf(BuiltinClasses::Function, part.builtinClass))
            return otherwise;
        return FlowTypeMatch::Sometimes;
    case FlowObjectKind::Instance:
        if (solver.objects().isInstanceOf(object, part.builtinClass))
            return matchInstanceOfTestedClass(solver, object, part);
        if (record.builtinClass && builtins.isSameOrSubclassOf(part.builtinClass, record.builtinClass))
            return FlowTypeMatch::Sometimes;
        return otherwise;
    case FlowObjectKind::Allocation:
        return matchAllocationWithClass(solver, object, otherwise);
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
        return otherwise;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowTypeMatch matchLayoutPart(const FlowSolver& solver, FlowObjectID object, const BuiltinTypePart& part)
{
    const FlowObjectTable& objects = solver.objects();
    if (objects.hasLayout(object) && objects.at(object).layout == solver.program().layoutOf(part.layout))
        return FlowTypeMatch::Always;
    return FlowTypeMatch::Sometimes;
}

static FlowTypeMatch matchBuiltinPart(const FlowSolver& solver, FlowObjectID object, const BuiltinTypePart& part)
{
    const BuiltinTable& builtins = solver.builtins();
    const FlowObject& record = solver.objects().at(object);
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        return FlowTypeMatch::Sometimes;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return builtins.canonicalID(record.builtin) == builtins.canonicalID(part.builtin) ? FlowTypeMatch::Always : FlowTypeMatch::Never;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
        return FlowTypeMatch::Never;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowTypeMatch matchObjectWithPart(const FlowSolver& solver, FlowObjectID object, const BuiltinTypePart& part)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        return matchInstancePart(solver, object, part);
    case BuiltinTypePartKind::Layout:
        return matchLayoutPart(solver, object, part);
    case BuiltinTypePartKind::Builtin:
        return matchBuiltinPart(solver, object, part);
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        return FlowTypeMatch::Sometimes;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowTypeMatch matchObject(const FlowSolver& solver, FlowObjectID object, const BuiltinType& type)
{
    if (type.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject }))
        return FlowTypeMatch::Always;
    if (object == unknownFlowObject)
        return type.mayBeObject() ? FlowTypeMatch::Sometimes : FlowTypeMatch::Never;

    FlowTypeMatch match = FlowTypeMatch::Never;
    if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
        match = strongerMatch(match, matchFunction(solver, object));
    if (type.flags.contains(BuiltinTypeFlag::HasPlainData))
        match = strongerMatch(match, matchPlainData(solver.objects().kindOf(object)));
    for (const BuiltinTypePart& part : type.parts)
        match = strongerMatch(match, matchObjectWithPart(solver, object, part));
    return match;
}

static bool testsPristineInstances(const BuiltinType& type)
{
    return std::ranges::any_of(type.parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && !part.pristine.isEmpty();
    });
}

static void waitOnLossOfBuiltinBehavior(FlowSolver& solver, FlowStatementSite site, FlowObjectID object)
{
    if (solver.objects().kindOf(object) != FlowObjectKind::Instance)
        return;
    solver.waitOnMutations(object, site, builtinBehaviorMutations | ObjectMutation::ReachNativeCode);
    if (solver.hasReachedNativeCode(object))
        solver.waitOnMutations(unknownFlowObject, site, builtinBehaviorMutations);
}

static FlowTypeMatch& matchOfObjectsSoFar(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand, BuiltinTypeID type)
{
    Vector<FlowMatchedObjects, 2>& matchedObjects = statementAt(solver.callState().builtins, site).matchedObjects;
    size_t index = matchedObjects.findIf([&](const FlowMatchedObjects& matched) {
        return matched.operand == operand && matched.type == type;
    });
    if (index == notFound) {
        index = matchedObjects.size();
        matchedObjects.append(FlowMatchedObjects { operand, type });
    }
    return matchedObjects[index].match;
}

// Objects only arrive in a node, and an instance only loses its built-in behavior, so each object is matched when it comes out of the cursor and Sometimes is final.
static FlowTypeMatch matchObjectsOf(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand, BuiltinTypeID typeID)
{
    // A run that leaves out a cursor with a member to repeat starts the statement over.
    FlowNewObjects arrived = solver.takeNewObjects(callTemporaryKey(site, FlowCallTemporaryRole::MatchedObjects, typeID.index()), operand);
    FlowTypeMatch& match = matchOfObjectsSoFar(solver, site, operand, typeID);
    if (match == FlowTypeMatch::Sometimes)
        return match;

    const BuiltinType& type = solver.builtins().at(typeID);
    bool mayStopMatching = testsPristineInstances(type);
    solver.charge(site, arrived.size());
    solver.forEachMember(arrived, [&](FlowObjectID object) {
        if (match == FlowTypeMatch::Sometimes)
            return;
        if (mayStopMatching)
            waitOnLossOfBuiltinBehavior(solver, site, object);
        match = unitedMatch(match, matchObject(solver, object, type));
    });
    return match;
}

static FlowTypeMatch matchPassedNode(FlowSolver& solver, const FlowBuiltinCall& call, FlowNodeID node, BuiltinTypeID typeID)
{
    const BuiltinType& type = solver.builtins().at(typeID);
    if (!node)
        return matchSinglePrimitive(type, BuiltinPrimitive::Undefined);

    FlowStatementSite site = call.request.site;
    const FlowValue& value = solver.valueOf(node);
    if (type.isAnything()) {
        if (!value.isEmpty())
            return FlowTypeMatch::Always;
        solver.waitOn(node, site);
        return FlowTypeMatch::NotReached;
    }

    // The cursor wakes the statement for an object that has not come out and for other primitives.
    FlowTypeMatch match = value.primitives ? matchPrimitives(solver, value, type) : FlowTypeMatch::NotReached;
    return unitedMatch(match, matchObjectsOf(solver, site, node, typeID));
}

// A branch on a rest parameter is taken when every argument that is passed for it lies in the tested type, so an argument that is not passed lies in it.
static FlowTypeMatch matchRestArguments(FlowSolver& solver, const FlowBuiltinCall& call, uint32_t firstArgument, BuiltinTypeID type)
{
    const FlowPassedArguments& arguments = call.arguments;
    bool hasUnreachedArgument = false;
    bool hasArgumentOutside = false;
    bool hasUndecidedArgument = false;
    auto visit = [&](uint32_t position) {
        FlowTypeMatch match = matchPassedNode(solver, call, arguments.at(position), type);
        if (!arguments.alwaysPasses(position))
            match = unitedMatch(match, FlowTypeMatch::Always);
        hasUnreachedArgument |= match == FlowTypeMatch::NotReached;
        hasArgumentOutside |= match == FlowTypeMatch::Never;
        hasUndecidedArgument |= match == FlowTypeMatch::Sometimes;
    };

    uint32_t numberOfPositions = arguments.positions.size();
    for (uint32_t position = firstArgument; position < numberOfPositions; ++position)
        visit(position);
    if (arguments.rest)
        visit(std::max(firstArgument, numberOfPositions));

    if (hasUnreachedArgument)
        return FlowTypeMatch::NotReached;
    if (hasArgumentOutside)
        return FlowTypeMatch::Never;
    return hasUndecidedArgument ? FlowTypeMatch::Sometimes : FlowTypeMatch::Always;
}

FlowTypeMatch matchOperand(FlowSolver& solver, const FlowBuiltinCall& call, BuiltinOperand operand, BuiltinTypeID type)
{
    if (operand.kind() == BuiltinOperand::Kind::ArgumentsFrom)
        return matchRestArguments(solver, call, operand.argumentIndex(), type);

    // A call without a receiver passes undefined as this.
    FlowTypeMatch matchWhenNotPassed = matchSinglePrimitive(solver.builtins().at(type), operand.isReceiver() ? BuiltinPrimitive::Undefined : BuiltinPrimitive::NotPassed);
    if (!mayBePassed(call, operand))
        return matchWhenNotPassed;

    FlowTypeMatch match = matchPassedNode(solver, call, operandNodeOf(call, operand), type);
    if (isAlwaysPassed(call, operand))
        return match;
    return unitedMatch(match, matchWhenNotPassed);
}

void selectFrom(FlowSolver& solver, const FlowBuiltinCall& call, BuiltinDispatchTarget target, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>& functor)
{
    if (target.isOverload()) {
        functor(target.overload);
        return;
    }

    const BuiltinTable& builtins = solver.builtins();
    const BuiltinDispatchNode& node = builtins.at(target.node);
    for (const BuiltinDispatchBranch& branch : node.branches) {
        switch (matchOperand(solver, call, node.operand, branch.tested)) {
        case FlowTypeMatch::NotReached:
            return;
        case FlowTypeMatch::Never:
            break;
        case FlowTypeMatch::Sometimes:
            selectFrom(solver, call, branch.target, functor);
            break;
        case FlowTypeMatch::Always:
            selectFrom(solver, call, branch.target, functor);
            return;
        }
    }
}

void forEachSelectedOverload(FlowSolver& solver, const FlowBuiltinCall& call, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>& functor)
{
    std::optional<BuiltinDispatchTarget> root = solver.builtins().dispatchOf(call.builtin, call.mode);
    if (!root)
        return;

    Vector<BuiltinOverloadID, 4> selected;
    selectFrom(solver, call, *root, [&](BuiltinOverloadID overload) {
        selected.appendIfNotContains(overload);
    });
    for (BuiltinOverloadID overload : selected)
        functor(overload);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
