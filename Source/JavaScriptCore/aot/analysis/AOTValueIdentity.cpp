/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueIdentity.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTUseLists.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static ValueID stripCasts(const Function& function, ValueID value, NOESCAPE const Invocable<bool(Opcode)> auto& shouldStrip)
{
    ValueID id = value;
    for (size_t remainingSteps = function.values.size(); shouldStrip(function.at(id).opcode); --remainingSteps) {
        if (!remainingSteps)
            return value;
        ASSERT(operandUsesOf(function.at(id).opcode, 0).contains(OperandUse::Returns));
        id = function.at(id).operand(0);
    }
    return id;
}

ValueID sameObjectThroughCasts(const Function& function, ValueID value)
{
    return stripCasts(function, value, [](Opcode opcode) {
        return isCastOrConversion(opcode);
    });
}

ValueID definitionThroughUpcasts(const Function& function, ValueID value)
{
    return stripCasts(function, value, [](Opcode opcode) {
        return opcode == Opcode::Upcast;
    });
}

Vector<ValueID, 2> findSingleUseCastChain(const Function& function, const UseLists& useLists, ValueID user)
{
    Vector<ValueID, 2> chain;
    for (ValueID id : CastChain(function, function.at(user).operand(0))) {
        std::optional<Use> use = useLists.singleUse(id);
        if (!use || use->kind != Use::Kind::Operand || use->user != user || use->operandIndex)
            return { };
        chain.append(id);
        user = id;
    }
    chain.reverse();
    return chain;
}

std::optional<ValueID> constantThroughCasts(const Program& program, const Function& function, ValueID id)
{
    CastChain chain(function, id);
    const Value& constant = function.at(chain.last());
    if (!isRematerializable(constant))
        return std::nullopt;
    const Type& constantType = program.typeOf(function, chain.last());
    for (ValueID cast : chain) {
        if (cast != chain.last() && !constantType.isSubsetOf(program.typeOf(function, cast), program))
            return std::nullopt;
    }
    return chain.last();
}

std::optional<int64_t> integerConstantThroughCasts(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constant = constantThroughCasts(program, function, id);
    if (!constant || function.at(*constant).opcode != Opcode::Integer)
        return std::nullopt;
    return function.at(*constant).integer();
}

// A value that no round has reached yet has no identity, and says nothing against the identity of a parameter that receives it: in a cycle with two entries
// a parameter is reached before a value that one of its edges passes. An identity only ever changes from the one value that all edges pass to the parameter itself.
ValueIdentities::ValueIdentities(const Function& function, const CFG& cfg)
    : m_identities(function.values.size())
{
    MergingParameters mergingParameters;
    bool didChange = false;
    auto update = [&](ValueID value, ValueID identity) {
        didChange |= m_identities[value.index()] != identity;
        m_identities[value.index()] = identity;
    };

    do {
        didChange = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            mergingParameters.clear();
            for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex)
                update(block.parameters[parameterIndex], identityOfParameter(function, cfg, id, parameterIndex, mergingParameters));
            for (ValueID value : block.values) {
                const Value& definition = function.at(value);
                ValueID source = isCastOrConversion(definition.opcode) ? m_identities[definition.operand(0).index()] : ValueID();
                update(value, source ? source : value);
            }
        }
    } while (didChange && m_didReadUnknownIdentity);

    for (size_t index = 0; index < m_identities.size(); ++index) {
        if (!m_identities[index])
            m_identities[index] = ValueID(index);
    }
}

ValueID ValueIdentities::identityOfParameter(const Function& function, const CFG& cfg, BlockID block, uint32_t parameterIndex, MergingParameters& mergingParameters)
{
    ValueID parameter = function.at(block).parameters[parameterIndex];
    if (block == cfg.root())
        return parameter;
    bool wasFoundToMerge = m_identities[parameter.index()] == parameter;
    ValueID common;
    bool mergesValues = false;
    for (Edge edge : cfg.incomingEdges(block)) {
        ValueID passed = valuePassedOn(function, edge, parameterIndex);
        if (!passed)
            return parameter;
        ValueID candidate = m_identities[passed.index()];
        m_didReadUnknownIdentity |= !candidate;
        if (!candidate || candidate == parameter)
            continue;
        if (!common)
            common = candidate;
        else if (common != candidate)
            mergesValues = true;
    }
    if (!common)
        return parameter;
    if (!mergesValues && !wasFoundToMerge)
        return common;

    Vector<uint32_t, 1>& earlierParameters = mergingParameters.add(common, Vector<uint32_t, 1>()).iterator->value;
    if (!wasFoundToMerge) {
        for (uint32_t earlierIndex : earlierParameters) {
            if (receiveSameValues(function, cfg, block, earlierIndex, parameterIndex))
                return function.at(block).parameters[earlierIndex];
        }
    }
    earlierParameters.append(parameterIndex);
    return parameter;
}

bool ValueIdentities::receiveSameValues(const Function& function, const CFG& cfg, BlockID block, uint32_t earlierIndex, uint32_t parameterIndex) const
{
    auto& parameters = function.at(block).parameters;
    return std::ranges::all_of(cfg.incomingEdges(block), [&](Edge edge) {
        ValueID passedToEarlier = m_identities[valuePassedOn(function, edge, earlierIndex).index()];
        ValueID passed = m_identities[valuePassedOn(function, edge, parameterIndex).index()];
        return passedToEarlier == passed || (passedToEarlier == parameters[earlierIndex] && passed == parameters[parameterIndex]);
    });
}

bool isRematerializable(const Value& value)
{
    Opcode opcode = value.opcode;
    return resultRuleOf(opcode) == ResultRule::Constant
        && interpretedOf(opcode) != Interpretation::Allocation
        && !numberOfNamedOperandsOf(opcode)
        && !hasVariadicOperand(opcode)
        && declaredBoundOf(opcode).isRemovableWhenUnused();
}

bool isDeferredClosure(const Function& function, ValueID id)
{
    const Value& definition = function.at(definitionThroughUpcasts(function, id));
    return definition.opcode == Opcode::NewClosure && definition.closureForm() == ClosureForm::Deferred;
}

bool hasCodeAtDefinition(const Function& function, ValueID id)
{
    return !isRematerializable(function.at(id)) && !isDeferredClosure(function, id);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
