/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBuiltinTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueSets.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// Listing keys and walking the chain of prototypes run the traps of a proxy and nothing else. The handler of a proxy has escaped when the proxy was made.
// What the species constructor makes goes to the result of one call, so the call sees to it.
static constexpr Protocols protocolsWithoutHub { Protocol::OwnKeys, Protocol::PrototypeChain, Protocol::Species };

static ValueSetID maskOf(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker, const BuiltinType& type)
{
    if (type.isAnything())
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 32> members;
    Vector<ValueSetID, 8> parts;
    auto addKind = [&](PrimitiveSet kind) {
        members.append(ValueFlowUniverse::primitive(kind));
    };
    if (type.primitives.contains(BuiltinPrimitive::Undefined))
        addKind(PrimitiveSet::Undefined);
    if (type.primitives.contains(BuiltinPrimitive::Null))
        addKind(PrimitiveSet::Null);
    if (type.primitives.contains(BuiltinPrimitive::Boolean)) {
        addKind(PrimitiveSet::False);
        addKind(PrimitiveSet::True);
    }
    if (type.primitives.contains(BuiltinPrimitive::Number)) {
        addKind(PrimitiveSet::Integer);
        addKind(PrimitiveSet::WholeNumber);
        addKind(PrimitiveSet::FractionalNumber);
    }
    if (type.primitives.contains(BuiltinPrimitive::BigInt))
        addKind(PrimitiveSet::BigInt);
    if (type.primitives.contains(BuiltinPrimitive::String))
        parts.append(universe.strings());
    else if (!type.stringLiterals.empty()) {
        // A string whose text nobody knows may be one of the literals.
        addKind(PrimitiveSet::String);
        for (BuiltinName literal : type.stringLiterals)
            members.append(universe.keyConstant(program.keyOf(literal)));
    }
    if (type.primitives.contains(BuiltinPrimitive::Symbol))
        parts.append(universe.symbols());

    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        parts.append(universe.objects());
    else {
        if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
            parts.append(universe.callables());
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
                parts.append(universe.instancesOf(part.builtinClass));
                break;
            case BuiltinTypePartKind::Layout:
                parts.append(universe.objectsWith(program.layoutOf(part.layout)));
                parts.append(universe.objectsThatMayBeAnything());
                break;
            case BuiltinTypePartKind::Builtin:
                universe.appendObjectsOf(part.builtin, members);
                members.append(ValueFlowUniverse::unknownObject());
                break;
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
                parts.append(universe.objects());
                break;
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                return ValueSets::anything;
            }
        }
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

static ValueSetID membersSurelyOf(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker, const BuiltinType& type)
{
    if (type.isAnything())
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 16> members;
    Vector<ValueSetID, 8> parts;
    auto addKind = [&](PrimitiveSet kind) {
        members.append(ValueFlowUniverse::primitive(kind));
    };
    if (type.primitives.contains(BuiltinPrimitive::Undefined))
        addKind(PrimitiveSet::Undefined);
    if (type.primitives.contains(BuiltinPrimitive::Null))
        addKind(PrimitiveSet::Null);
    if (type.primitives.contains(BuiltinPrimitive::Boolean)) {
        addKind(PrimitiveSet::False);
        addKind(PrimitiveSet::True);
    }
    if (type.primitives.contains(BuiltinPrimitive::Number) && !type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) && type.integers.isEmpty()) {
        addKind(PrimitiveSet::Integer);
        addKind(PrimitiveSet::WholeNumber);
        addKind(PrimitiveSet::FractionalNumber);
    }
    if (type.primitives.contains(BuiltinPrimitive::BigInt))
        addKind(PrimitiveSet::BigInt);
    if (type.primitives.contains(BuiltinPrimitive::String))
        parts.append(universe.strings());
    else {
        for (BuiltinName literal : type.stringLiterals)
            members.append(universe.keyConstant(program.keyOf(literal)));
    }
    if (type.primitives.contains(BuiltinPrimitive::Symbol))
        parts.append(universe.symbols());

    if (type.flags.contains(BuiltinTypeFlag::HasAnyObject))
        parts.append(universe.objects());
    else {
        if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction)) {
            parts.append(universe.closures());
            parts.append(universe.builtinFunctions());
        }
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
                // Whether an instance is pristine, and what it holds, is known only when the program runs.
                if (part.pristine.isEmpty() && part.contents.empty())
                    parts.append(universe.sureInstancesOf(part.builtinClass));
                break;
            case BuiltinTypePartKind::Builtin:
                members.append(universe.objectOf(part.builtin));
                break;
            // An object that is allocated with the layout loses it when a property is deleted or redefined.
            case BuiltinTypePartKind::Layout:
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                break;
            }
        }
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

ValueFlowBuiltinTables::ValueFlowBuiltinTables(const BuiltinTable& builtins)
{
    m_conversionToPrimitive = intern({ Protocol::ToPrimitive, false, { }, { } });
    auto behaviorFor = [&](OperandUses uses, Protocols declaredProtocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers) -> ValueFlowBehaviorID {
        Protocols protocols = declaredProtocols - protocolsWithoutHub;
        if (uses == OperandUses::all() || protocols.isEmpty())
            return { };
        return intern({ protocols, uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }), namedMembers, calledMembers });
    };
    auto add = [&](const BuiltinParameter& parameter) {
        if (ValueFlowBehaviorID behavior = behaviorFor(parameter.uses, parameter.protocols, parameter.namedMembers, parameter.calledMembers))
            m_behaviorOfParameter.add(&parameter, behavior);
    };
    for (uint32_t index = 0; index < builtins.overloadCount(); ++index) {
        const BuiltinOverload& overload = builtins.at(BuiltinOverloadID(index));
        if (overload.receiver)
            add(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            add(parameter);
    }

    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operand = 0; operand < numberOfDeclaredOperands; ++operand) {
            const OperandInfo& info = operandInfoOf(opcode, operand);
            if (ValueFlowBehaviorID behavior = behaviorFor(info.uses, info.protocols, info.namedMembers, { }))
                m_behaviorOfOperand.add(&info, behavior);
        }
    }
}

void ValueFlowBuiltinTables::makeSets(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker)
{
    const BuiltinTable& builtins = program.builtins();
    for (uint32_t index = 0; builtins.contains(BuiltinTypeID(index)); ++index) {
        m_maskOfType.append(Typed::maskOf(program, universe, maker, builtins.at(BuiltinTypeID(index))));
        m_membersSurelyOfType.append(Typed::membersSurelyOf(program, universe, maker, builtins.at(BuiltinTypeID(index))));

        const BuiltinType& type = builtins.at(BuiltinTypeID(index));
        ValueSetID whilePristine = ValueSets::nothing;
        for (const BuiltinTypePart& part : type.parts) {
            if (part.kind == BuiltinTypePartKind::Instance && !part.pristine.isEmpty() && part.contents.empty() && pristineTestOf(builtins, part.builtinClass) == PristineTest::MayPass)
                whilePristine = maker.unite(whilePristine, universe.sureInstancesOf(part.builtinClass));
        }
        m_membersOfTypeWhilePristine.append(whilePristine);

        ValueSetID primitives = maker.intersect(m_maskOfType.last(), universe.anyPrimitive());
        if (!type.isAnything() && !type.primitives.contains(BuiltinPrimitive::String) && !type.stringLiterals.empty()) {
            ValueSetID literals = maker.intersect(m_maskOfType.last(), universe.keys());
            primitives = maker.unite(maker.intersect(primitives, ValueSets::complement(maker.of(ValueFlowUniverse::primitive(PrimitiveSet::String)))), literals);
        }
        m_primitivesOfType.append(primitives);
    }

    m_heap = makeUnique<ValueFlowHeapTables>(program, universe, maker);
}

ValueFlowBehaviorID ValueFlowBuiltinTables::intern(const ValueFlowBehavior& behavior)
{
    auto found = std::ranges::find_if(m_behaviors, [&](const ValueFlowBehavior& other) {
        return other.protocols == behavior.protocols && other.isTransitive == behavior.isTransitive
            && std::ranges::equal(other.namedMembers, behavior.namedMembers) && std::ranges::equal(other.calledMembers, behavior.calledMembers);
    });
    if (found == m_behaviors.end()) {
        m_behaviors.append(behavior);
        return ValueFlowBehaviorID(m_behaviors.size() - 1);
    }
    return ValueFlowBehaviorID(static_cast<uint32_t>(found - m_behaviors.begin()));
}

ValueFlowBehaviorID ValueFlowBuiltinTables::behaviorOf(const BuiltinParameter& parameter) const
{
    return m_behaviorOfParameter.get(&parameter);
}

ValueFlowBehaviorID ValueFlowBuiltinTables::behaviorOf(const OperandInfo& operand) const
{
    return m_behaviorOfOperand.get(&operand);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
