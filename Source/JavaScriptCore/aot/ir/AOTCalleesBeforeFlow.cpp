/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCalleesBeforeFlow.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// A binding that is never assigned again holds what its one initialization gave it.
FunctionID CalleesBeforeFlow::findCertainCallee(const Value& call) const
{
    auto onlyClosureIn = [&](ClosureHolder holder) {
        std::span<const FunctionID> closures = m_closures.closuresIn(holder);
        return closures.size() == 1 ? closures[0] : FunctionID();
    };
    const Value& callee = m_function.at(definitionThroughSameValues(m_function, call.calleeOperand()));
    switch (callee.opcode) {
    case Opcode::NewClosure:
        return callee.function();
    case Opcode::GetModuleVariable:
        if (!m_program.at(callee.variable()).flags.contains(VariableFlags::IsImmutable))
            return { };
        return onlyClosureIn(ClosureHolder::variable(callee.variable()));
    case Opcode::GetEnvironmentField: {
        const Vector<Field>& fields = m_program.at(callee.layout()).fields;
        if (callee.field() >= fields.size() || !fields[callee.field()].flags.contains(FieldFlags::IsImmutable))
            return { };
        return onlyClosureIn(ClosureHolder::environmentField(callee.layout(), callee.field()));
    }
    default:
        return { };
    }
}

std::optional<Type> CalleesBeforeFlow::tryTypeOfResultOfOnlyClosure(const Value& call) const
{
    FunctionID id = findCertainCallee(call);
    if (!id)
        return std::nullopt;
    const Function& callee = m_program.at(id);
    if (callee.results.size() != 1 || callee.isUnsplitCoroutine())
        return std::nullopt;
    return m_typing.typeOf(callee.results[0]);
}

// The receiver is an instance of a built-in class, a primitive or a built-in object by its type, and start-up locks the member under the name. The instance is taken to be
// as its class created it. One that is not (an own property under the name, a subclass with a method or a species of its own) may give another kind of object, so the
// answer says what to expect and proves nothing.
std::optional<Type> CalleesBeforeFlow::tryTypeOfResultOfLockedBuiltin(const Value& call) const
{
    PropertyKeyID key = call.calleeKey();
    ValueID thisValue = call.thisValueOperand();
    if (!key || !thisValue)
        return std::nullopt;
    const Value& read = m_function.at(definitionThroughSameValues(m_function, call.calleeOperand()));
    bool readsMemberOfThisValue = read.opcode == Opcode::PropGet && read.key() == key
        && definitionThroughSameValues(m_function, read.objectOperand()) == definitionThroughSameValues(m_function, thisValue);
    if (!readsMemberOfThisValue)
        return std::nullopt;

    // The read of the member has thrown on undefined and null.
    Type receiverType = m_typing.typeOf(thisValue).withoutNullish();
    BuiltinLookupContext lookup { m_program, m_noFlowResults };
    BuiltinReceiver receiver = builtinReceiverOf(m_program, receiverType);
    BuiltinMemberAnswer found;
    switch (receiver.kind) {
    case BuiltinReceiverKind::Unknown:
        return std::nullopt;
    case BuiltinReceiverKind::BuiltinObject:
        found = findMemberFrom(lookup, receiver.object, key);
        break;
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        found = findMemberOfInstances(lookup, receiver.builtinClass, key);
        break;
    }
    if (!found.isConstant())
        return std::nullopt;
    for (ClassPart& part : receiverType.classes)
        part.provenPristine = { PristineAspects::Storage, PristineAspects::Names, PristineAspects::AsCreated };
    BuiltinOperandTypes operands = operandTypesOf(m_typing, thisValue, call.variadicOperands());
    operands.receiver = m_types.intern(receiverType);
    return resultTypeOfReachableOverloads(m_typing, found.member, BuiltinCallMode::Call, operands);
}

void CalleesBeforeFlow::forEachExpectedLayout(ValueID id, NOESCAPE const LayoutFunctor& functor) const
{
    const Value& value = m_function.at(id);
    TypeID visited;
    for (TypeID type : { value.type, value.expected() }) {
        if (!type || type == visited)
            continue;
        visited = type;
        for (LayoutID layout : membersOf(m_program, m_types.at(type)))
            functor(layout);
    }
}

void CalleesBeforeFlow::forEachClosureIn(ClosureHolder holder, NOESCAPE const CalleeFunctor& functor) const
{
    for (FunctionID closure : m_closures.closuresIn(holder))
        functor(closure);
}

void CalleesBeforeFlow::forEachInheritedClosure(ClosureHolder::Kind kind, LayoutID layout, ClassSide side, PropertyKeyID key, NOESCAPE const CalleeFunctor& functor) const
{
    LayoutID current = layout;
    for (size_t steps = 0; current && steps < m_program.layouts.size(); ++steps) {
        ClosureHolder holder = ClosureHolder::member(kind, current, side, key);
        if (!m_closures.closuresIn(holder).empty()) {
            forEachClosureIn(holder, functor);
            return;
        }
        const Layout& passed = m_program.at(current);
        current = side == ClassSide::Prototype ? passed.base : predictedHeritageOf(passed);
    }
}

void CalleesBeforeFlow::forEachKnownMethod(const Value& read, NOESCAPE const CalleeFunctor& functor) const
{
    bool hasExpectedLayout = false;
    forEachExpectedLayout(read.objectOperand(), [&](LayoutID layout) {
        hasExpectedLayout = true;
        forEachInheritedClosure(ClosureHolder::Kind::Member, layout, ClassSide::Prototype, read.key(), functor);
    });
    // The class that a mixin makes overrides the methods of whatever it is applied to.
    if (hasExpectedLayout) {
        for (LayoutID layout : m_closures.classesWithUnknownBase())
            forEachClosureIn(ClosureHolder::member(ClosureHolder::Kind::Member, layout, ClassSide::Prototype, read.key()), functor);
    }
    forEachConstructedClass(read.objectOperand(), [&](LayoutID constructed) {
        forEachInheritedClosure(ClosureHolder::Kind::Member, constructed, ClassSide::Static, read.key(), functor);
    });
}

void CalleesBeforeFlow::forEachKnownSuperMethod(const Value& read, NOESCAPE const CalleeFunctor& functor) const
{
    const Value& base = m_function.at(definitionThroughSameValues(m_function, read.baseOperand()));
    if (base.opcode != Opcode::GetSuperBase || !read.key())
        return;
    ClassSide side = base.homeKind() == HomeObjectKind::ClassPrototype ? ClassSide::Prototype : ClassSide::Static;
    forEachConstructedClass(base.homeOperand(), [&](LayoutID constructed) {
        if (LayoutID parent = predictedHeritageOf(m_program.at(constructed)))
            forEachInheritedClosure(ClosureHolder::Kind::Member, parent, side, read.key(), functor);
    });
}

void CalleesBeforeFlow::forEachConstructedClass(ValueID classObject, NOESCAPE const LayoutFunctor& functor) const
{
    forEachHeldClosure(classObject, [&](FunctionID constructor) {
        if (LayoutID constructed = m_program.at(constructor).constructedClass)
            functor(constructed);
    });
}

void CalleesBeforeFlow::forEachStaticField(ValueID classObject, PropertyKeyID key, NOESCAPE const VariableFunctor& functor) const
{
    forEachConstructedClass(classObject, [&](LayoutID constructed) {
        LayoutID current = constructed;
        for (size_t steps = 0; current && steps < m_program.layouts.size(); ++steps) {
            const Vector<StaticField>& fields = m_program.at(current).staticFields;
            size_t index = fields.findIf([&](const StaticField& field) {
                return field.key == key;
            });
            if (index != notFound) {
                functor(fields[index].variable);
                return;
            }
            current = predictedHeritageOf(m_program.at(current));
        }
    });
}

std::optional<uint32_t> CalleesBeforeFlow::captureIndexOf(ValueID id) const
{
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    for (uint32_t index = 0; index < std::min<size_t>(m_function.captures.size(), parameters.size()); ++index) {
        if (parameters[index] == id)
            return index;
    }
    return std::nullopt;
}

bool CalleesBeforeFlow::forEachClosureInHolderReadBy(ValueID definitionID, NOESCAPE const CalleeFunctor& functor) const
{
    const Value& definition = m_function.at(definitionID);
    switch (definition.opcode) {
    case Opcode::GetModuleVariable:
        forEachClosureIn(ClosureHolder::variable(definition.variable()), functor);
        return true;
    case Opcode::GetEnvironmentField:
        forEachClosureIn(ClosureHolder::environmentField(definition.layout(), definition.field()), functor);
        return true;
    case Opcode::Parameter:
        if (std::optional<uint32_t> capture = captureIndexOf(definitionID))
            forEachClosureIn(ClosureHolder::capture(m_id, *capture), functor);
        return true;
    default:
        return false;
    }
}

// The closures a value may be, without those it may have been read from a member of an object: the receiver of that read is a value like this one.
void CalleesBeforeFlow::forEachHeldClosure(ValueID id, NOESCAPE const CalleeFunctor& functor) const
{
    const Type& type = m_typing.typeOf(id);
    if (!type.functions.isEmpty() && !type.anyFunction) {
        for (FunctionID function : type.functions)
            functor(function);
        return;
    }
    forEachDefinition(id, [&](ValueID definition) {
        forEachClosureInHolderReadBy(definition, functor);
    });
}

void CalleesBeforeFlow::forEachKnownCallee(ValueID callee, NOESCAPE const CalleeFunctor& functor) const
{
    const Type& type = m_typing.typeOf(callee);
    if (!type.functions.isEmpty() && !type.anyFunction) {
        for (FunctionID function : type.functions)
            functor(function);
        return;
    }
    forEachDefinition(callee, [&](ValueID definitionID) {
        if (forEachClosureInHolderReadBy(definitionID, functor))
            return;
        const Value& definition = m_function.at(definitionID);
        if (definition.opcode == Opcode::PropGet)
            forEachKnownMethod(definition, functor);
        else if (definition.opcode == Opcode::SuperGet)
            forEachKnownSuperMethod(definition, functor);
    });
}

void CalleesBeforeFlow::forEachIncomingValue(BlockID block, uint32_t parameterIndex, NOESCAPE const ValueFunctor& functor) const
{
    for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
        const Target& target = targetOf(m_function, edge);
        if (parameterIndex < target.arguments.size()) {
            functor(target.arguments[parameterIndex]);
            continue;
        }
        if (edge.isHandlerEdge())
            continue;
        EdgeResultPosition position { edge.from, edge.targetIndex, parameterIndex - static_cast<uint32_t>(target.arguments.size()) };
        std::optional<EdgeResult> result = tryGetEdgeResult(m_function, position);
        if (result && result->kind == EdgeResultKind::NarrowedOperand)
            functor(result->subject);
    }
}

void CalleesBeforeFlow::forEachDefinition(ValueID value, NOESCAPE const ValueFunctor& functor) const
{
    value = definitionThroughSameValues(m_function, value);
    if (m_function.at(value).opcode != Opcode::Parameter) {
        functor(value);
        return;
    }

    BitVector visited;
    Vector<ValueID, 8> worklist { value };
    while (!worklist.isEmpty()) {
        ValueID id = definitionThroughSameValues(m_function, worklist.takeLast());
        if (visited.set(id.index()))
            continue;
        if (m_function.at(id).opcode != Opcode::Parameter) {
            functor(id);
            continue;
        }
        ValueLocation location = m_analyses.useLists().locationOf(id);
        if (location.block == entryBlock) {
            functor(id);
            continue;
        }
        forEachIncomingValue(location.block, location.index, [&](ValueID incoming) {
            worklist.append(incoming);
        });
    }
}

std::optional<Type> CalleesBeforeFlow::tryTypeOfResultOfCertainCallee(const Value& call) const
{
    if (std::optional<Type> result = tryTypeOfResultOfOnlyClosure(call))
        return result;
    return tryTypeOfResultOfLockedBuiltin(call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
