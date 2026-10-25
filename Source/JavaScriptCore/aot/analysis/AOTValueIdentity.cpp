/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueIdentity.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreshold.h"
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
    return stripCasts(function, value, [](Opcode opcode) { return isCastOrConversion(opcode); });
}

ValueID definitionThroughUpcasts(const Function& function, ValueID value)
{
    return stripCasts(function, value, [](Opcode opcode) { return opcode == Opcode::Upcast; });
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

struct ValueIdentities::PassedValues {
    ValueID parameterAt(uint32_t parameterIndex) const { return parameters[parameterIndex]; }
    ValueID valuePassedOn(size_t edgeIndex, uint32_t parameterIndex) const { return valuesByEdge[edgeIndex * parameters.size() + parameterIndex]; }

    std::span<const ValueID> parameters;
    size_t numberOfEdges;
    std::span<const ValueID> valuesByEdge;
    bool isRoot;
};

namespace {

class NarrowedOperands {
public:
    NarrowedOperands(const Function& function, Edge edge, const Target& target)
        : m_function(function)
        , m_edge(edge)
        , m_proofs(target.proofs.span())
        , m_numberOfArguments(target.arguments.size())
    {
        if (edge.isHandlerEdge())
            return;
        const Terminator& terminator = function.at(edge.from).terminator;
        if (!terminator.isConditionalBranch() || targetResultsOf(terminator.kind, edge.targetIndex, static_cast<uint32_t>(terminator.targets.size())) != TargetResults::Proofs)
            return;
        const Value& test = function.at(terminator.conditionOperand());
        Proof proof = provesOf(test.opcode);
        if (proof != Proof::None && !provesForStretch(proof))
            m_test = &test;
    }

    ValueID valuePassedTo(uint32_t parameterIndex) const
    {
        uint32_t resultIndex = parameterIndex - m_numberOfArguments;
        if (m_test && resultIndex < m_proofs.size()) {
            uint32_t operandIndex = m_proofs[resultIndex];
            if (operandIndex < m_test->numberOfOperands() && m_test->operandKind(operandIndex) != OperandKind::Guard) {
                if (ValueID operand = m_test->operand(operandIndex))
                    return operand;
            }
        }
        return valuePassedOn(m_function, m_edge, parameterIndex);
    }

private:
    const Function& m_function;
    Edge m_edge;
    std::span<const uint32_t> m_proofs;
    uint32_t m_numberOfArguments;
    const Value* m_test { nullptr };
};

} // anonymous namespace

class ValueIdentities::MergingParameters {
public:
    explicit MergingParameters(size_t numberOfValues)
        : m_numberOfValues(numberOfValues)
    {
    }

    void clear()
    {
        m_parameters.shrink(0);
        ++m_block;
    }

    std::optional<uint32_t> findIf(ValueID firstValue, NOESCAPE const Invocable<bool(uint32_t)> auto& predicate) const
    {
        if (m_parameters.size() <= maxSearchedParameters) {
            for (const Entry& entry : m_parameters) {
                if (entry.firstValue == firstValue && predicate(entry.parameterIndex))
                    return entry.parameterIndex;
            }
            return std::nullopt;
        }
        const Chain& chain = m_chainOfFirstValue[firstValue.index()];
        if (chain.block != m_block)
            return std::nullopt;
        for (uint32_t position = chain.first; position != noPosition; position = m_parameters[position].next) {
            if (predicate(m_parameters[position].parameterIndex))
                return m_parameters[position].parameterIndex;
        }
        return std::nullopt;
    }

    void add(ValueID firstValue, uint32_t parameterIndex)
    {
        m_parameters.append({ firstValue, parameterIndex, noPosition });
        if (m_parameters.size() == maxSearchedParameters + 1) {
            if (m_chainOfFirstValue.isEmpty())
                m_chainOfFirstValue.grow(m_numberOfValues);
            for (uint32_t position = 0; position < m_parameters.size(); ++position)
                appendToChain(position);
        } else if (m_parameters.size() > maxSearchedParameters)
            appendToChain(m_parameters.size() - 1);
    }

private:
    static constexpr size_t maxSearchedParameters = 16;
    static constexpr uint32_t noPosition = UINT32_MAX;

    struct Entry {
        ValueID firstValue;
        uint32_t parameterIndex;
        uint32_t next;
    };

    struct Chain {
        uint32_t block { 0 };
        uint32_t first { noPosition };
        uint32_t last { noPosition };
    };

    void appendToChain(uint32_t position)
    {
        Chain& chain = m_chainOfFirstValue[m_parameters[position].firstValue.index()];
        if (chain.block != m_block) {
            chain = { m_block, position, position };
            return;
        }
        m_parameters[chain.last].next = position;
        chain.last = position;
    }

    size_t m_numberOfValues;
    uint32_t m_block { 0 };
    Vector<Entry, maxSearchedParameters + 1> m_parameters;
    Vector<Chain> m_chainOfFirstValue;
};

ValueIdentities::ValueIdentities(const Function& function, const CFG& cfg, RepeatedWalks repeatedWalks, const OpcodeSet* opcodesOfValues, Evaluation evaluation)
    : m_function(function)
    , m_cfg(&cfg)
    , m_numberOfValues(function.values.size())
    , m_takesShortcuts(repeatedWalks == RepeatedWalks::StartAtFirstChangedBlock)
{
    auto hasParameters = [&](BlockID block) { return !function.at(block).parameters.isEmpty(); };
    if (m_takesShortcuts && opcodesOfValues && !opcodesOfValues->intersects(castsAndConversions) && std::ranges::none_of(cfg.reversePostOrder().subspan(1), hasParameters)) {
        m_isComplete = true;
        return;
    }

    m_identities.grow(m_numberOfValues);
    if (evaluation == Evaluation::Complete || !m_takesShortcuts)
        computeAll();
}

void ValueIdentities::complete()
{
    if (m_isComplete)
        return;
    m_identities.fill(ValueID());
    m_walkPositions.clear();
    m_listedParameters.clear();
    computeAll();
}

void ValueIdentities::computeAll()
{
    const Function& function = m_function;
    const CFG& cfg = *m_cfg;
    m_isComplete = true;
    m_keepsWhatWalksRead = m_takesShortcuts && cfg.hasCycle();
    if (m_keepsWhatWalksRead) {
        m_firstPassedValue.fill(notKept, cfg.reversePostOrder().size());
        m_operandsOfCasts.grow(function.values.size());
    }
    walk(function, cfg, 0);
    while (m_firstBlockThatReadUnknownIdentity) {
        std::optional<uint32_t> firstBlockToWalkAgain = expectLessOfParametersThatDoNotHold(function, cfg);
        if (!firstBlockToWalkAgain)
            break;
        if (!m_takesShortcuts)
            firstBlockToWalkAgain = 0;
        for (BlockID id : cfg.reversePostOrder().subspan(*firstBlockToWalkAgain)) {
            const Block& block = function.at(id);
            for (ValueID parameter : block.parameters)
                m_identities[parameter.index()] = ValueID();
            for (ValueID value : block.values)
                m_identities[value.index()] = ValueID();
        }
        m_didWalk = true;
        walk(function, cfg, *firstBlockToWalkAgain);
    }
    m_expectations.clear();
    m_passedValues.clear();
    m_firstPassedValue.clear();
    m_operandsOfCasts.clear();

    for (size_t index = 0; index < m_identities.size(); ++index) {
        if (!m_identities[index])
            m_identities[index] = ValueID(index);
    }
}

template<size_t inlineCapacity>
static void appendPassedValues(const Function& function, Edge edge, size_t numberOfParameters, bool takesShortcuts, Vector<ValueID, inlineCapacity>& passedValues)
{
    const Target& target = targetOf(function, edge);
    std::span<const ValueID> arguments = target.arguments.span();
    if (arguments.size() >= numberOfParameters) {
        passedValues.append(arguments.first(numberOfParameters));
        return;
    }
    passedValues.append(arguments);
    if (!takesShortcuts) {
        for (uint32_t parameterIndex = arguments.size(); parameterIndex < numberOfParameters; ++parameterIndex)
            passedValues.append(valuePassedOn(function, edge, parameterIndex));
        return;
    }
    NarrowedOperands narrowedOperands(function, edge, target);
    for (uint32_t parameterIndex = arguments.size(); parameterIndex < numberOfParameters; ++parameterIndex)
        passedValues.append(narrowedOperands.valuePassedTo(parameterIndex));
}

auto ValueIdentities::passedValuesInto(const Function& function, const CFG& cfg, uint32_t orderIndex) -> PassedValues
{
    BlockID block = cfg.reversePostOrder()[orderIndex];
    std::span<const ValueID> parameters = function.at(block).parameters.span();
    std::span<const Edge> edges = cfg.incomingEdges(block);
    uint32_t first = m_keepsWhatWalksRead ? m_firstPassedValue[orderIndex] : notKept;
    if (first == notKept) {
        if (!m_keepsWhatWalksRead)
            m_passedValues.shrink(0);
        first = m_passedValues.size();
        for (Edge edge : edges)
            appendPassedValues(function, edge, parameters.size(), m_takesShortcuts, m_passedValues);
        if (m_keepsWhatWalksRead)
            m_firstPassedValue[orderIndex] = first;
    }
    return { parameters, edges.size(), m_passedValues.subspan(first, edges.size() * parameters.size()), block == cfg.root() };
}

void ValueIdentities::walk(const Function& function, const CFG& cfg, uint32_t firstOrderIndex)
{
    MergingParameters mergingParameters(function.values.size());
    if (m_firstBlockThatReadUnknownIdentity && *m_firstBlockThatReadUnknownIdentity >= firstOrderIndex)
        m_firstBlockThatReadUnknownIdentity = std::nullopt;
    std::span<const BlockID> order = cfg.reversePostOrder();
    RELEASE_ASSERT(m_identities.size() == function.values.size());
    ValueID* identities = m_identities.mutableSpan().data();
    const Value* values = function.values.span().data();
    ValueID* operandsOfCasts = m_operandsOfCasts.mutableSpan().data();
    auto readOperandOfCast = [&](ValueID value) {
        const Value& definition = values[value.index()];
        return isCastOrConversion(definition.opcode) ? definition.operand(0) : ValueID();
    };
    for (uint32_t orderIndex = firstOrderIndex; orderIndex < order.size(); ++orderIndex) {
        BlockID id = order[orderIndex];
        const Block& block = function.at(id);
        if (!block.parameters.isEmpty()) {
            m_didReadUnknownIdentity = false;
            std::span<const ValueID> parameters = block.parameters.span();
            std::span<const Edge> edges = cfg.incomingEdges(id);
            const Target* onlyTarget = m_takesShortcuts && !m_keepsWhatWalksRead && edges.size() == 1 ? &targetOf(function, edges[0]) : nullptr;
            if (onlyTarget && onlyTarget->arguments.size() >= parameters.size()) {
                std::span<const ValueID> arguments = onlyTarget->arguments.span();
                for (size_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
                    ValueID parameter = parameters[parameterIndex];
                    ValueID identity = parameter;
                    if (ValueID passed = arguments[parameterIndex]) {
                        ValueID source = identities[passed.index()];
                        m_didReadUnknownIdentity |= !source;
                        if (source)
                            identity = source;
                    }
                    identities[parameter.index()] = identity;
                }
            } else {
                PassedValues passedValues = passedValuesInto(function, cfg, orderIndex);
                if (m_takesShortcuts && passedValues.numberOfEdges == 1) {
                    for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex)
                        identities[parameters[parameterIndex].index()] = identityOfParameterWithSingleEdge(passedValues, parameterIndex);
                } else {
                    mergingParameters.clear();
                    for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex)
                        identities[parameters[parameterIndex].index()] = identityOfParameter(passedValues, parameterIndex, mergingParameters);
                }
            }
            if (m_didReadUnknownIdentity && !m_firstBlockThatReadUnknownIdentity)
                m_firstBlockThatReadUnknownIdentity = orderIndex;
        }
        auto assignIdentities = [&](const auto& operandOfCast) {
            for (ValueID value : block.values) {
                ValueID operand = operandOfCast(value);
                ValueID source = operand ? identities[operand.index()] : ValueID();
                identities[value.index()] = source ? source : value;
            }
        };
        if (m_keepsWhatWalksRead && m_didWalk)
            assignIdentities([&](ValueID value) { return operandsOfCasts[value.index()]; });
        else if (m_keepsWhatWalksRead)
            assignIdentities([&](ValueID value) { return operandsOfCasts[value.index()] = readOperandOfCast(value); });
        else
            assignIdentities(readOperandOfCast);
    }
}

ValueID ValueIdentities::identityOfParameterWithSingleEdge(const PassedValues& passedValues, uint32_t parameterIndex)
{
    ValueID parameter = passedValues.parameterAt(parameterIndex);
    ASSERT(m_expectations.isEmpty() || m_expectations[parameter.index()] == Expectation::IsPassedOneValue);
    ValueID passed = passedValues.valuePassedOn(0, parameterIndex);
    if (!passed)
        return parameter;
    ValueID identity = m_identities[passed.index()];
    m_didReadUnknownIdentity |= !identity;
    return identity ? identity : parameter;
}

ValueID ValueIdentities::identityOfParameter(const PassedValues& passedValues, uint32_t parameterIndex, MergingParameters& mergingParameters)
{
    ValueID parameter = passedValues.parameterAt(parameterIndex);
    if (passedValues.isRoot)
        return parameter;
    ValueID common;
    bool mergesValues = false;
    for (size_t edgeIndex = 0; edgeIndex < passedValues.numberOfEdges; ++edgeIndex) {
        ValueID passed = passedValues.valuePassedOn(edgeIndex, parameterIndex);
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
    Expectation expectation = m_expectations.isEmpty() ? Expectation::IsPassedOneValue : m_expectations[parameter.index()];
    if (!mergesValues && expectation == Expectation::IsPassedOneValue)
        return common;

    if (expectation != Expectation::IsValueOfItsOwn) {
        std::optional<uint32_t> earlierIndex = mergingParameters.findIf(common, [&](uint32_t earlierIndex) {
            return receiveSameValues(passedValues, earlierIndex, parameterIndex);
        });
        if (earlierIndex)
            return passedValues.parameterAt(*earlierIndex);
    }
    mergingParameters.add(common, parameterIndex);
    return parameter;
}

bool ValueIdentities::receiveSameValues(const PassedValues& passedValues, uint32_t earlierIndex, uint32_t parameterIndex) const
{
    for (size_t edgeIndex = 0; edgeIndex < passedValues.numberOfEdges; ++edgeIndex) {
        ValueID passedToEarlier = m_identities[passedValues.valuePassedOn(edgeIndex, earlierIndex).index()];
        ValueID passedValue = passedValues.valuePassedOn(edgeIndex, parameterIndex);
        ValueID passed = m_identities[passedValue.index()];
        // The parameter that is asked about has no identity yet, so an edge that passes it to itself is known by the value.
        bool isPassedToItself = passedValue == passedValues.parameterAt(parameterIndex) || passed == passedValues.parameterAt(parameterIndex);
        bool isSame = passedToEarlier == passed || (passedToEarlier == passedValues.parameterAt(earlierIndex) && isPassedToItself);
        if (!isSame)
            return false;
    }
    return true;
}

std::optional<uint32_t> ValueIdentities::expectLessOfParametersThatDoNotHold(const Function& function, const CFG& cfg)
{
    std::optional<uint32_t> firstChangedBlock;
    std::span<const BlockID> order = cfg.reversePostOrder();
    for (uint32_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
        BlockID block = order[orderIndex];
        auto& parameters = function.at(block).parameters;
        if (m_takesShortcuts && cfg.incomingEdges(block).size() == 1)
            continue;
        std::optional<PassedValues> passedValues;
        for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
            ValueID parameter = parameters[parameterIndex];
            ValueID identity = m_identities[parameter.index()];
            if (identity == parameter)
                continue;
            if (!passedValues)
                passedValues = passedValuesInto(function, cfg, orderIndex);
            bool isPassedOneValue = true;
            for (size_t edgeIndex = 0; edgeIndex < passedValues->numberOfEdges && isPassedOneValue; ++edgeIndex) {
                ValueID passed = m_identities[passedValues->valuePassedOn(edgeIndex, parameterIndex).index()];
                isPassedOneValue = !passed || passed == identity || passed == parameter;
            }
            if (isPassedOneValue)
                continue;

            uint32_t earlierIndex = 0;
            while (earlierIndex < parameterIndex && parameters[earlierIndex] != identity)
                ++earlierIndex;
            bool followsEarlierParameter = earlierIndex < parameterIndex;
            if (followsEarlierParameter && receiveSameValues(*passedValues, earlierIndex, parameterIndex))
                continue;

            if (m_expectations.isEmpty())
                m_expectations.fill(Expectation::IsPassedOneValue, function.values.size());
            m_expectations[parameter.index()] = followsEarlierParameter ? Expectation::IsValueOfItsOwn : Expectation::ReceivesWhatEarlierParameterReceives;
            if (!firstChangedBlock)
                firstChangedBlock = orderIndex;
        }
    }
    return firstChangedBlock;
}

void ValueIdentities::recordWalkPositions()
{
    m_walkPositions.fill(notListed, m_numberOfValues);
    m_listedParameters.shrink(0);
    uint32_t position = 0;
    for (BlockID id : m_cfg->reversePostOrder()) {
        const Block& block = m_function.at(id);
        uint32_t index = 0;
        for (ValueID parameter : block.parameters) {
            uint32_t walkPosition = position++ << 1 | 1;
            m_walkPositions[parameter.index()] = walkPosition;
            m_listedParameters.append({ walkPosition, id, index++ });
        }
        for (ValueID value : block.values)
            m_walkPositions[value.index()] = position++ << 1;
    }
    m_remainingOnDemandWork = 8 * m_listedParameters.size() + 256;
}

auto ValueIdentities::listedParameterAt(uint32_t walkPosition) const -> const ListedParameter&
{
    auto found = std::ranges::lower_bound(m_listedParameters, walkPosition, { }, &ListedParameter::walkPosition);
    RELEASE_ASSERT(found != m_listedParameters.end() && found->walkPosition == walkPosition);
    return *found;
}

ValueID ValueIdentities::resolve(ValueID id)
{
    if (ValueID identity = tryResolve(id))
        return identity;
    complete();
    return m_identities[id.index()];
}

ValueID ValueIdentities::tryResolve(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_numberOfValues);
    if (m_identities.isEmpty())
        return id;
    if (m_isComplete)
        return m_identities[id.index()];
    if (m_walkPositions.isEmpty())
        recordWalkPositions();
    ValueID blockingParameter;
    return tryResolve(id, blockingParameter);
}

ValueID ValueIdentities::tryResolve(ValueID id, ValueID& blockingParameter)
{
    Vector<ValueID, 8> chain;
    ValueID identity;
    for (ValueID current = id;;) {
        identity = m_identities[current.index()];
        if (identity)
            break;

        uint32_t walkPosition = m_walkPositions[current.index()];
        bool isListedParameter = walkPosition != notListed && (walkPosition & 1);
        ValueID source;
        if (isListedParameter) {
            const ListedParameter& parameter = listedParameterAt(walkPosition);
            std::span<const Edge> edges = m_cfg->incomingEdges(parameter.block);
            if (edges.size() > 1) {
                identity = tryResolveMergeParameter(current, parameter);
                if (!identity) {
                    blockingParameter = current;
                    return { };
                }
                break;
            }
            if (edges.size() == 1) {
                const Target& target = targetOf(m_function, edges[0]);
                source = parameter.index < target.arguments.size() ? target.arguments[parameter.index] : NarrowedOperands(m_function, edges[0], target).valuePassedTo(parameter.index);
            }
        } else if (walkPosition != notListed && isCastOrConversion(m_function.at(current).opcode))
            source = m_function.at(current).operand(0);

        if (!source || m_walkPositions[source.index()] >= walkPosition) {
            identity = current;
            m_identities[current.index()] = current;
            break;
        }
        chain.append(current);
        current = source;
    }
    for (ValueID element : chain)
        m_identities[element.index()] = identity;
    return identity;
}

ValueID ValueIdentities::tryResolvePassedValue(ValueID passed, ValueID parameter)
{
    if (m_walkPositions[passed.index()] == notListed)
        return { };
    ValueID blockingParameter;
    ValueID identity = tryResolve(passed, blockingParameter);
    if (!identity && blockingParameter == parameter)
        return parameter;
    return identity;
}

ValueID ValueIdentities::tryResolveMergeParameter(ValueID id, const ListedParameter& listed)
{
    AOT_THRESHOLD(size_t, maxDepth, 32);

    BlockID block = listed.block;
    std::span<const ValueID> parameters = m_function.at(block).parameters.span();
    std::span<const Edge> edges = m_cfg->incomingEdges(block);
    size_t work = parameters.size() * edges.size();
    if (m_blocksBeingResolved.size() >= maxDepth || m_blocksBeingResolved.contains(block) || work > m_remainingOnDemandWork)
        return { };
    m_remainingOnDemandWork -= work;

    Vector<ValueID, 32> passedValueStorage;
    for (Edge edge : edges)
        appendPassedValues(m_function, edge, parameters.size(), true, passedValueStorage);
    PassedValues passedValues { parameters, edges.size(), passedValueStorage.span(), false };

    uint32_t firstWalkPosition = listed.walkPosition - 2 * listed.index;
    bool readsOnlyEarlierValues = std::ranges::all_of(passedValueStorage, [&](ValueID passed) {
        return !passed || m_walkPositions[passed.index()] < firstWalkPosition;
    });

    m_blocksBeingResolved.append(block);
    ValueID identity = readsOnlyEarlierValues ? tryResolveParametersInOrder(passedValues, passedValueStorage.span(), id) : tryResolveLoopParameter(passedValues, listed.index, listed.walkPosition);
    m_blocksBeingResolved.removeLast();
    return identity;
}

ValueID ValueIdentities::tryResolveParametersInOrder(const PassedValues& passedValues, std::span<const ValueID> passedValueList, ValueID id)
{
    for (ValueID passed : passedValueList) {
        ValueID blockingParameter;
        if (passed && !tryResolve(passed, blockingParameter))
            return { };
    }
    MergingParameters mergingParameters(m_numberOfValues);
    mergingParameters.clear();
    size_t numberOfParameters = passedValueList.size() / passedValues.numberOfEdges;
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex)
        m_identities[passedValues.parameterAt(parameterIndex).index()] = identityOfParameter(passedValues, parameterIndex, mergingParameters);
    return m_identities[id.index()];
}

ValueID ValueIdentities::tryResolveLoopParameter(const PassedValues& passedValues, uint32_t parameterIndex, uint32_t walkPosition)
{
    ValueID parameter = passedValues.parameterAt(parameterIndex);
    ValueID common;
    ValueID firstPassedValue;
    bool mergesValues = false;
    bool isPassedOneEarlierValueAndItself = true;
    for (size_t edgeIndex = 0; edgeIndex < passedValues.numberOfEdges; ++edgeIndex) {
        ValueID passed = passedValues.valuePassedOn(edgeIndex, parameterIndex);
        if (!passed) {
            m_identities[parameter.index()] = parameter;
            return parameter;
        }
        ValueID identity = tryResolvePassedValue(passed, parameter);
        if (identity == parameter)
            continue;
        if (!firstPassedValue)
            firstPassedValue = passed;
        if (!identity || passed != firstPassedValue || m_walkPositions[passed.index()] >= walkPosition)
            isPassedOneEarlierValueAndItself = false;
        if (!identity)
            continue;
        if (!common)
            common = identity;
        else if (common != identity)
            mergesValues = true;
    }

    ValueID identity;
    if (mergesValues) {
        for (uint32_t earlierIndex = 0; earlierIndex < parameterIndex; ++earlierIndex) {
            if (!receiveDifferentValues(passedValues, earlierIndex, parameterIndex))
                return { };
        }
        identity = parameter;
    } else if (common && isPassedOneEarlierValueAndItself)
        identity = common;
    else
        return { };
    m_identities[parameter.index()] = identity;
    return identity;
}

bool ValueIdentities::receiveDifferentValues(const PassedValues& passedValues, uint32_t earlierIndex, uint32_t parameterIndex)
{
    if (passedValues.numberOfEdges > m_remainingOnDemandWork)
        return false;
    m_remainingOnDemandWork -= passedValues.numberOfEdges;

    ValueID earlier = passedValues.parameterAt(earlierIndex);
    ValueID parameter = passedValues.parameterAt(parameterIndex);
    for (size_t edgeIndex = 0; edgeIndex < passedValues.numberOfEdges; ++edgeIndex) {
        ValueID passedToEarlier = passedValues.valuePassedOn(edgeIndex, earlierIndex);
        if (!passedToEarlier)
            return true;
        ValueID earlierIdentity = tryResolvePassedValue(passedToEarlier, earlier);
        ValueID identity = tryResolvePassedValue(passedValues.valuePassedOn(edgeIndex, parameterIndex), parameter);
        if (!earlierIdentity || !identity)
            continue;
        if (earlierIdentity != identity && (earlierIdentity != earlier || identity != parameter))
            return true;
    }
    return false;
}

void ValueIdentities::didEraseValues(std::span<const ValueID> erased)
{
    if (m_identities.isEmpty())
        return;
    for (ValueID id : erased) {
        m_identities[id.index()] = id;
        if (!m_walkPositions.isEmpty())
            m_walkPositions[id.index()] = notListed;
    }
}

bool isRematerializable(const Value& value)
{
    Opcode opcode = value.opcode;
    return resultRuleOf(opcode) == ResultRule::Constant
        && interpretedOf(opcode) != Interpretation::Allocation
        && !numberOfNamedOperandsOf(opcode)
        && !hasVariadicOperand(opcode)
        && declaredBoundOf(opcode).isUnobservable();
}

bool isDeferredClosure(const Function& function, ValueID id)
{
    const Value& definition = function.at(definitionThroughUpcasts(function, id));
    return definition.opcode == Opcode::NewClosure && definition.closureForm() == ClosureForm::Deferred;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
