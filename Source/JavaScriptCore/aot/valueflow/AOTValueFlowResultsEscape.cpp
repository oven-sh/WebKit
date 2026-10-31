/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFlowSinkEquations.h"
#include "AOTHeldOperands.h"
#include "AOTProgramCaches.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noNode = UINT32_MAX;

enum class SinkTransform : uint8_t {
    Same,
    Argument,
    RepeatedArgument,
    Leaves,
    IfReturned,
    Lifetime,
    Contents,
    Held,
};

struct SinkDependency {
    uint32_t node { 0 };
    uint32_t condition { 0 };
    SinkTransform transform { SinkTransform::Same };
};

struct SinkEquations {
    std::span<const SinkDependency> dependenciesOf(uint32_t node) const LIFETIME_BOUND { return dependencies.span().subspan(offsets[node], offsets[node + 1] - offsets[node]); }

    ParallelArray<OperandUses> base;
    ParallelArray<OperandUses> sinks;
    Vector<uint32_t> offsets;
    ParallelArray<SinkDependency> dependencies;
};

struct OperandNodes {
    bool hasNodesOf(FunctionID function) const { return firstNodeOfFunction[function.index()] != firstNodeOfFunction[function.index() + 1]; }
    uint32_t entryParameter(FunctionID function, uint32_t entryParameter) const { return firstNodeOfFunction[function.index()] + entryParameter; }
    uint32_t environmentField(LayoutID layout, uint32_t field) const { return firstNodeOfLayout[layout.index()] + field; }
    uint32_t size() const { return firstNodeOfLayout.last(); }

    Vector<uint32_t> firstNodeOfFunction;
    Vector<uint32_t> firstNodeOfLayout;
    Vector<uint8_t> mayBeReferenceByType;
};

struct BaseUses {
    uint32_t subject { 0 };
    OperandUses uses;
};

struct SinkRule {
    uint32_t subject { 0 };
    SinkDependency dependency;
};

struct CollectedRules {
    Vector<BaseUses> baseUses;
    Vector<SinkRule> dependencies;
};

// A node for the parameters that take one operand of a call with many callees: its uses are the union of theirs.
// The operand depends on that one node, and calls with the same callees share it. n calls of any of n functions would be n * n rules otherwise.
// The transforms of an argument distribute over a union, so every other node gets the same uses either way.
class ParameterUnions {
public:
    explicit ParameterUnions(uint32_t firstNode)
        : m_firstNode(firstNode)
    {
    }

    uint32_t nodeFor(Vector<uint32_t>&& parameters)
    {
        Hasher hasher;
        for (uint32_t parameter : parameters)
            add(hasher, parameter);

        Locker locker { m_lock };
        auto& unions = m_unionsByHash.add(hasher.hash(), Vector<uint32_t, 1> { }).iterator->value;
        for (uint32_t index : unions) {
            if (m_parameters[index] == parameters)
                return m_firstNode + index;
        }
        unions.append(m_parameters.size());
        m_parameters.append(WTF::move(parameters));
        return m_firstNode + unions.last();
    }

    // Where one operand of a call with many callees goes. It follows from the set of callees and from the place of the operand in the call, so it is worked out once for all calls of the set.
    struct ParametersOfOperand {
        bool isPassedToUnknownParameter { false };
        uint32_t node { noNode };

        friend bool operator==(const ParametersOfOperand&, const ParametersOfOperand&) = default;
    };

    static std::optional<uint64_t> keyOf(CalleeSetID callees, Opcode opcode, uint32_t operandIndex, bool isCalledValue, bool flagOfCall)
    {
        constexpr uint32_t opcodeBits = 12;
        constexpr uint32_t operandBits = 18;
        if (!callees || TypeTable::isProvisional(callees) || static_cast<uint32_t>(opcode) >> opcodeBits || operandIndex >> operandBits)
            return std::nullopt;
        return static_cast<uint64_t>(callees.index()) << 32 | static_cast<uint64_t>(opcode) << (operandBits + 2) | static_cast<uint64_t>(operandIndex) << 2 | static_cast<uint64_t>(isCalledValue) << 1 | static_cast<uint64_t>(flagOfCall);
    }

    std::optional<ParametersOfOperand> find(uint64_t key)
    {
        Locker locker { m_lock };
        auto known = m_parametersOfOperands.find(key);
        return known == m_parametersOfOperands.end() ? std::nullopt : std::optional(known->value);
    }

    void remember(uint64_t key, ParametersOfOperand parameters)
    {
        Locker locker { m_lock };
        m_parametersOfOperands.add(key, parameters);
    }

    uint32_t size() const { return m_parameters.size(); }

    CollectedRules rules() const
    {
        CollectedRules rules;
        for (uint32_t index = 0; index < m_parameters.size(); ++index) {
            for (uint32_t parameter : m_parameters[index])
                rules.dependencies.append(SinkRule { m_firstNode + index, SinkDependency { parameter, parameter, SinkTransform::Same } });
        }
        return rules;
    }

private:
    uint32_t m_firstNode;
    Lock m_lock;
    Vector<Vector<uint32_t>> m_parameters;
    UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_unionsByHash;
    UncheckedKeyHashMap<uint64_t, ParametersOfOperand, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_parametersOfOperands;
};

} // anonymous namespace

static OperandUses applyTransform(SinkTransform transform, OperandUses uses)
{
    constexpr OperandUses contentsUses { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::PassesToUserCode };
    switch (transform) {
    case SinkTransform::Same:
    case SinkTransform::IfReturned:
        return uses;
    case SinkTransform::Argument:
        return uses - OperandUse::Returns;
    case SinkTransform::RepeatedArgument: {
        OperandUses result = uses - OperandUse::Returns;
        if (uses.containsAny({ OperandUse::Calls, OperandUse::CallsPerElement }))
            result.add(OperandUse::CallsRepeatedly);
        return result;
    }
    case SinkTransform::Leaves: {
        OperandUses result = uses - OperandUse::Returns;
        if (uses.contains(OperandUse::Returns))
            result.add(OperandUse::Retains);
        return result;
    }
    case SinkTransform::Lifetime: {
        if (!uses.containsAny(escapingUses))
            return { };
        OperandUses result { OperandUse::Retains };
        if (uses.contains(OperandUse::PassesToUserCode))
            result.add(OperandUse::PassesToUserCode);
        return result;
    }
    case SinkTransform::Contents: {
        if (uses.containsOnly(OperandUse::Inspects))
            return { };
        OperandUses result = (uses & contentsUses) | OperandUses { OperandUse::AppliesTransitively };
        if (uses.containsAny(escapingUses))
            result.add(OperandUse::RetainsContents);
        return result;
    }
    case SinkTransform::Held: {
        OperandUses result = applyTransform(SinkTransform::Lifetime, uses);
        if (uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }))
            result.add(applyTransform(SinkTransform::Leaves, uses));
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t conditionOf(const SinkDependency& dependency)
{
    return dependency.transform == SinkTransform::IfReturned ? dependency.condition : dependency.node;
}

static void solveSinkEquations(SinkEquations& equations, std::span<const uint32_t> nodesAskedFor, const ParallelLoops& loops)
{
    RELEASE_ASSERT(equations.base.size() == equations.sinks.size() && equations.offsets.size() == equations.sinks.size() + 1);
    constexpr uint32_t unvisited = 0;
    constexpr uint32_t solved = UINT32_MAX;
    constexpr uint32_t nodesPerShare = 1 << 16;
    struct Frame {
        uint32_t node;
        uint32_t nextDependency;
        uint32_t lowLink;
        bool hasSelfEdge;
    };
    ParallelArray<OperandUses>& sinks = equations.sinks;
    uint32_t numberOfNodes = sinks.size();
    Vector<uint32_t> visitIndices(numberOfNodes);
    uint32_t unsolvedCount = 0;
    Vector<Frame> frames;
    Vector<uint32_t> componentStack;
    Vector<uint32_t> userOffsets;
    Vector<uint32_t> userCursors;
    Vector<uint32_t> users;
    Vector<uint32_t> worklist;
    BitVector isQueued(numberOfNodes);
    forEachIndexInParallel(loops, (numberOfNodes + nodesPerShare - 1) / nodesPerShare, [&](unsigned share) {
        uint32_t first = share * nodesPerShare;
        std::ranges::fill(visitIndices.mutableSpan().subspan(first, std::min(nodesPerShare, numberOfNodes - first)), unvisited);
    });

    auto solveIfTrivial = [&](uint32_t node) {
        OperandUses uses = equations.base[node] | sinks[node];
        if (uses != OperandUses::all() && equations.offsets[node] != equations.offsets[node + 1])
            return false;
        sinks[node] = uses;
        visitIndices[node] = solved;
        return true;
    };
    auto beginVisit = [&](uint32_t node) {
        uint32_t index = ++unsolvedCount;
        visitIndices[node] = index;
        frames.append(Frame { node, equations.offsets[node], index, false });
    };
    auto findUnvisitedDependency = [&](Frame& frame) -> std::optional<uint32_t> {
        auto needsVisit = [&](uint32_t target) {
            uint32_t index = visitIndices[target];
            if (index == unvisited)
                return !solveIfTrivial(target);
            if (index != solved) {
                frame.lowLink = std::min(frame.lowLink, index);
                frame.hasSelfEdge |= target == frame.node;
            }
            return false;
        };
        uint32_t end = equations.offsets[frame.node + 1];
        for (; frame.nextDependency < end; ++frame.nextDependency) {
            const SinkDependency& dependency = equations.dependencies[frame.nextDependency];
            if (needsVisit(dependency.node))
                return dependency.node;
            uint32_t condition = conditionOf(dependency);
            if (condition != dependency.node && needsVisit(condition))
                return condition;
        }
        return std::nullopt;
    };
    auto evaluate = [&](uint32_t node) {
        OperandUses uses = equations.base[node] | sinks[node];
        for (const SinkDependency& dependency : equations.dependenciesOf(node)) {
            if (dependency.transform == SinkTransform::IfReturned && !sinks[dependency.condition].contains(OperandUse::Returns))
                continue;
            uses.add(applyTransform(dependency.transform, sinks[dependency.node]));
        }
        if (uses == sinks[node])
            return false;
        sinks[node] = uses;
        return true;
    };
    auto evaluateCycle = [&](std::span<const uint32_t> members, uint32_t firstVisitIndex) {
        auto forEachDependencyInCycle = [&](const Invocable<void(uint32_t user, uint32_t dependency)> auto& functor) {
            for (uint32_t user : members) {
                for (const SinkDependency& dependency : equations.dependenciesOf(user)) {
                    uint32_t condition = conditionOf(dependency);
                    if (visitIndices[dependency.node] != solved)
                        functor(user, dependency.node);
                    if (condition != dependency.node && visitIndices[condition] != solved)
                        functor(user, condition);
                }
            }
        };
        userOffsets.fill(0, members.size() + 1);
        forEachDependencyInCycle([&](uint32_t, uint32_t dependency) {
            ++userOffsets[visitIndices[dependency] - firstVisitIndex + 1];
        });
        for (size_t member = 0; member < members.size(); ++member)
            userOffsets[member + 1] += userOffsets[member];
        userCursors = userOffsets;
        users.resize(userOffsets.last());
        forEachDependencyInCycle([&](uint32_t user, uint32_t dependency) {
            users[userCursors[visitIndices[dependency] - firstVisitIndex]++] = user;
        });

        auto evaluateAndQueueUsers = [&](uint32_t node) {
            isQueued.quickClear(node);
            if (!evaluate(node))
                return;
            uint32_t member = visitIndices[node] - firstVisitIndex;
            for (uint32_t index = userOffsets[member]; index < userOffsets[member + 1]; ++index) {
                uint32_t user = users[index];
                if (!isQueued.quickGet(user)) {
                    isQueued.quickSet(user);
                    worklist.append(user);
                }
            }
        };
        for (uint32_t member : members)
            isQueued.quickSet(member);
        for (uint32_t member : members)
            evaluateAndQueueUsers(member);
        while (!worklist.isEmpty())
            evaluateAndQueueUsers(worklist.takeLast());
    };
    auto finishVisit = [&](const Frame& frame) {
        uint32_t index = visitIndices[frame.node];
        bool isComponentRoot = frame.lowLink == index;
        if (isComponentRoot && unsolvedCount == index && !frame.hasSelfEdge) {
            evaluate(frame.node);
            visitIndices[frame.node] = solved;
            unsolvedCount = index - 1;
            return;
        }

        componentStack.append(frame.node);
        if (!isComponentRoot) {
            Frame& parent = frames.last();
            parent.lowLink = std::min(parent.lowLink, frame.lowLink);
            return;
        }

        size_t firstMember = componentStack.size() - (unsolvedCount - index + 1);
        std::span<const uint32_t> members = componentStack.subspan(firstMember);
        evaluateCycle(members, index);
        for (uint32_t member : members)
            visitIndices[member] = solved;
        componentStack.shrink(firstMember);
        unsolvedCount = index - 1;
    };

    for (uint32_t root : nodesAskedFor) {
        if (visitIndices[root] != unvisited || solveIfTrivial(root))
            continue;
        beginVisit(root);
        while (!frames.isEmpty()) {
            Frame frame = frames.last();
            if (std::optional<uint32_t> dependency = findUnvisitedDependency(frame)) {
                frames.last() = frame;
                beginVisit(*dependency);
                continue;
            }
            frames.removeLast();
            finishVisit(frame);
        }
    }
}

static bool mayHoldReference(const Program& program, const OperandNodes& nodes, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    return nodes.mayBeReferenceByType[(value.type ? value.type : TypeTable::anything).index()] || numberOfResultsOf(program, function, value).has_value();
}

static bool hasFollowedFields(const Layout& layout)
{
    return layout.kind == LayoutKind::Environment;
}

static OperandNodes numberNodes(const Program& program, const ParallelLoops& loops)
{
    OperandNodes nodes;
    unsigned numberOfFunctions = program.functions.size();
    nodes.firstNodeOfFunction.fill(0, numberOfFunctions + 1);
    nodes.mayBeReferenceByType = Vector<uint8_t>(program.types.size());
    constexpr unsigned typesPerTask = 4096;
    forEachIndexInParallel(loops, (program.types.size() + typesPerTask - 1) / typesPerTask, [&](unsigned task) {
        for (size_t index = task * typesPerTask; index < std::min<size_t>(program.types.size(), (task + 1) * typesPerTask); ++index)
            nodes.mayBeReferenceByType[index] = mayBeReference(program.at(TypeID(index)));
    });
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        uint32_t count = 0;
        for (uint32_t value = 0; value < function.values.size(); ++value)
            count += mayHoldReference(program, nodes, function, ValueID(value));
        for (ValueID parameter : function.at(entryBlock).parameters)
            count += !mayHoldReference(program, nodes, function, parameter);
        nodes.firstNodeOfFunction[index + 1] = count;
    });
    uint64_t total = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        total += nodes.firstNodeOfFunction[index + 1];
        RELEASE_ASSERT(total < noNode);
        nodes.firstNodeOfFunction[index + 1] = static_cast<uint32_t>(total);
    }

    nodes.firstNodeOfLayout.fill(0, program.layouts.size() + 1);
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        nodes.firstNodeOfLayout[index] = static_cast<uint32_t>(total);
        if (hasFollowedFields(program.layouts[index]))
            total += program.layouts[index].fields.size();
        RELEASE_ASSERT(total < noNode);
    }
    nodes.firstNodeOfLayout[program.layouts.size()] = static_cast<uint32_t>(total);
    return nodes;
}

static bool readsArgumentsOfFrame(Opcode opcode)
{
    switch (opcode) {
    case Opcode::GetArgument:
    case Opcode::GetMyArgumentByVal:
    case Opcode::CreateRest:
    case Opcode::CallForwardVarargs:
    case Opcode::ConstructSuperForwardVarargs:
        return true;
    default:
        return false;
    }
}

static bool isDroppedBy(const Function& callee, const Value& call, uint32_t operandIndex)
{
    if (!callee.hasBody())
        return false;
    if (OperandPosition::thisValue(call.opcode) == operandIndex)
        return !callee.has(FunctionFlags::HasThis);
    std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
    if (!argument || !VariadicOperandPosition::arguments(call.opcode) || operandKindOf(call.opcode, operandIndex) == OperandKind::Declared)
        return false;
    if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }))
        return false;
    return *argument >= callee.numberOfPositionalParameters();
}

static OperandUses usesBesideDeclaredOnes(const Value& user, uint32_t operandIndex)
{
    OperandUses uses;
    if (receiverOf(user.opcode) == operandIndex) {
        switch (accessOf(user.opcode)) {
        case Access::Store:
        case Access::Define:
            uses.add(OperandUse::StoresByKey);
            break;
        case Access::None:
        case Access::Read:
        case Access::Has:
            break;
        }
    }
    if (user.opcode == Opcode::NewObject && OperandPosition::prototype(user.opcode) == operandIndex)
        uses.add(OperandUse::BecomesPrototype);
    return uses;
}

static void collectRulesOfBody(const Program& program, const FlowResults& flow, const OperandNodes& nodes, ParameterUnions& parameterUnions, FunctionID id, CollectedRules& rules)
{
    const Function& function = program.at(id);
    EffectSummaries unknownSummaries;
    EffectContext effects { program, unknownSummaries };
    uint32_t firstNode = nodes.firstNodeOfFunction[id.index()];
    uint32_t numberOfNodes = nodes.firstNodeOfFunction[id.index() + 1] - firstNode;
    BitVector nodesThatMayBecomeAnything(numberOfNodes);
    BitVector isAsked(numberOfNodes);
    BitVector isOfThisWalk(numberOfNodes);
    BitVector hasUserThatWasPassed(numberOfNodes);
    Vector<uint32_t, 16> nodesOfNextWalk;
    bool asksForSources = false;
    bool collectsAboutEveryNode = true;

    Vector<uint32_t> nodeOfValue;
    nodeOfValue.fill(noNode, function.values.size());
    uint32_t nextNode = firstNode;
    for (ValueID parameter : function.at(entryBlock).parameters)
        nodeOfValue[parameter.index()] = nextNode++;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (nodeOfValue[index] == noNode && mayHoldReference(program, nodes, function, ValueID(index)))
            nodeOfValue[index] = nextNode++;
    }
    RELEASE_ASSERT(nextNode == nodes.firstNodeOfFunction[id.index() + 1]);

    BitVector blocksInCycles(function.blocks.size());
    Vector<BlockID, 16> blocksInOrder;
    if (function.blocks.size() == 1)
        blocksInOrder.append(entryBlock);
    else {
        CFG cfg(function);
        BlockCycles cycles(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            blocksInOrder.append(block);
            if (cycles.isInCycle(block))
                blocksInCycles.quickSet(block.index());
        }
        for (uint32_t index = 0; blocksInOrder.size() < function.blocks.size() && index < function.blocks.size(); ++index) {
            if (!cfg.isReachable(BlockID(index)))
                blocksInOrder.append(BlockID(index));
        }
    }

    auto nodeOf = [&](ValueID value) {
        return value ? nodeOfValue[value.index()] : noNode;
    };
    auto addUses = [&](uint32_t subject, OperandUses uses) {
        rules.baseUses.append(BaseUses { subject, uses });
        if (uses == OperandUses::all() && subject - firstNode < nodesThatMayBecomeAnything.size())
            nodesThatMayBecomeAnything.quickSet(subject - firstNode);
    };
    auto mayBecomeAnything = [&](uint32_t subject) {
        return nodesThatMayBecomeAnything.quickGet(subject - firstNode);
    };
    auto isOfThisBody = [&](uint32_t node) {
        return node - firstNode < numberOfNodes;
    };
    auto ask = [&](uint32_t node) {
        if (!asksForSources || !isOfThisBody(node) || isAsked.quickSet(node - firstNode))
            return;
        if (hasUserThatWasPassed.quickGet(node - firstNode))
            nodesOfNextWalk.append(node);
        else
            isOfThisWalk.quickSet(node - firstNode);
    };
    auto addDependency = [&](uint32_t subject, uint32_t source, SinkTransform transform) {
        if (subject == noNode || source == noNode)
            return;
        rules.dependencies.append(SinkRule { subject, SinkDependency { source, source, transform } });
        ask(source);
    };
    auto isCollectedAtThisUser = [&](uint32_t subject) {
        if (isOfThisWalk.quickGet(subject - firstNode))
            return true;
        hasUserThatWasPassed.quickSet(subject - firstNode);
        return false;
    };

    auto passTo = [&](uint32_t subject, FunctionID callee, std::optional<uint32_t> entryParameter, ValueID call, bool isInCycle) {
        if (!entryParameter || !nodes.hasNodesOf(callee)) {
            addUses(subject, OperandUses::all());
            return;
        }
        uint32_t parameter = nodes.entryParameter(callee, *entryParameter);
        addDependency(subject, parameter, isInCycle ? SinkTransform::RepeatedArgument : SinkTransform::Argument);
        if (uint32_t result = nodeOf(call); result != noNode)
            rules.dependencies.append(SinkRule { subject, SinkDependency { result, parameter, SinkTransform::IfReturned } });
    };
    AOT_THRESHOLD(size_t, minCalleesOfParameterUnion, 16);
    bool checksAnswers = program.propertyAccessCache() && program.propertyAccessCache()->checksAnswers;
    auto passToEach = [&](uint32_t subject, std::span<const FunctionID> callees, ValueID call, bool isInCycle, std::optional<uint64_t> keyOfOperand, NOESCAPE const auto& forEachEntryParameter) {
        if (callees.size() < minCalleesOfParameterUnion) {
            for (FunctionID callee : callees) {
                forEachEntryParameter(callee, [&](std::optional<uint32_t> entryParameter) {
                    passTo(subject, callee, entryParameter, call, isInCycle);
                });
            }
            return;
        }
        auto findParameters = [&] {
            Vector<uint32_t> parameters;
            ParameterUnions::ParametersOfOperand found;
            for (FunctionID callee : callees) {
                forEachEntryParameter(callee, [&](std::optional<uint32_t> entryParameter) {
                    if (!entryParameter || !nodes.hasNodesOf(callee))
                        found.isPassedToUnknownParameter = true;
                    else
                        parameters.append(nodes.entryParameter(callee, *entryParameter));
                });
            }
            if (!parameters.isEmpty())
                found.node = parameterUnions.nodeFor(WTF::move(parameters));
            return found;
        };
        std::optional<ParameterUnions::ParametersOfOperand> found = keyOfOperand ? parameterUnions.find(*keyOfOperand) : std::nullopt;
        RELEASE_ASSERT(!found || !checksAnswers || *found == findParameters());
        if (!found) {
            found = findParameters();
            if (keyOfOperand)
                parameterUnions.remember(*keyOfOperand, *found);
        }
        if (found->isPassedToUnknownParameter)
            addUses(subject, OperandUses::all());
        if (found->node == noNode)
            return;
        uint32_t parameter = found->node;
        addDependency(subject, parameter, isInCycle ? SinkTransform::RepeatedArgument : SinkTransform::Argument);
        if (uint32_t result = nodeOf(call); result != noNode)
            rules.dependencies.append(SinkRule { subject, SinkDependency { result, parameter, SinkTransform::IfReturned } });
    };
    auto visitOperandOfCall = [&](ValueID callID, uint32_t operandIndex, uint32_t subject, const CalleeList& callees, bool isInCycle) {
        const Value& call = function.at(callID);
        OperandUses declaredUses = declaredBehaviorOf(call.opcode, operandIndex).uses;
        if (declaredUses.contains(OperandUse::RetainsContents))
            return;

        bool isCalledValue = declaredUses.containsAny(callingUses);
        OperandUses usesOfCalledValue = declaredUses | OperandUse::Inspects;
        if (isInCycle)
            usesOfCalledValue.add(OperandUse::CallsRepeatedly);
        if (isCalledValue && call.opcode == Opcode::Call && !callees.listsEveryCallee()) {
            addUses(subject, usesOfCalledValue);
            return;
        }
        std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
        if (!functions || maySuspend(call.opcode)) {
            addUses(subject, OperandUses::all());
            return;
        }

        CalleeSetID setOfCallees = calleeSetOfCall(program, call);
        if (isCalledValue) {
            addUses(subject, usesOfCalledValue);
            std::optional<unsigned> newTarget = OperandPosition::newTarget(call.opcode);
            passToEach(subject, functions->value(), callID, isInCycle, ParameterUnions::keyOf(setOfCallees, call.opcode, operandIndex, true, newTarget && !call.operand(*newTarget)), [&](FunctionID callee, const auto& pass) {
                for (uint32_t entryParameter : entryParametersOfCalledValue(program.at(callee), call))
                    pass(entryParameter);
            });
            return;
        }
        addUses(subject, OperandUse::Inspects);
        passToEach(subject, functions->value(), callID, isInCycle, ParameterUnions::keyOf(setOfCallees, call.opcode, operandIndex, false, isHeldOperand(call, operandIndex)), [&](FunctionID callee, const auto& pass) {
            if (!isDroppedBy(program.at(callee), call, operandIndex))
                pass(entryParameterOf(program.at(callee), call, operandIndex));
        });
    };
    auto followFieldOfEnvironment = [&](const Value& user, uint32_t operandIndex, uint32_t subject) {
        switch (user.opcode) {
        case Opcode::PutEnvironmentField:
            if (hasFollowedFields(program.at(user.layout())))
                addDependency(subject, nodes.environmentField(user.layout(), user.field()), SinkTransform::Same);
            return;
        case Opcode::NewObject:
            if (std::optional<uint32_t> index = user.variadicOperandIndex(operandIndex); index && hasFollowedFields(program.at(user.layout())))
                addDependency(subject, nodes.environmentField(user.layout(), user.fields()[*index]), SinkTransform::Same);
            return;
        default:
            return;
        }
    };
    auto findOwnAllocation = [&](ValueID held) -> ValueID {
        while (held) {
            const Value& value = function.at(held);
            if (interpretedOf(value.opcode) == Interpretation::Allocation)
                return held;
            if (!isCastOrConversion(value.opcode))
                return { };
            held = value.operand(0);
        }
        return { };
    };
    auto visitDeclaredOperand = [&](ValueID userID, uint32_t operandIndex, uint32_t subject) {
        const Value& user = function.at(userID);
        uint32_t result = nodeOf(userID);

        switch (user.opcode) {
        case Opcode::Check:
            addUses(subject, OperandUses::all());
            return;
        case Opcode::ResultAt:
            addDependency(subject, result, SinkTransform::Same);
            return;
        case Opcode::GetEnvironmentField:
            if (hasFollowedFields(program.at(user.layout()))) {
                addUses(subject, OperandUse::Inspects);
                addDependency(nodes.environmentField(user.layout(), user.field()), result, SinkTransform::Leaves);
                return;
            }
            break;
        default:
            break;
        }

        OperandSummary summary = summarizeOperand(effects, function, user, operandIndex);
        OperandUses uses = summary.uses | usesBesideDeclaredOnes(user, operandIndex);
        if (uses == OperandUses::all()) {
            addUses(subject, uses);
            return;
        }

        bool isHandedOn = uses.contains(OperandUse::Returns);
        if (isHandedOn) {
            uses.remove(OperandUse::Returns);
            addDependency(subject, result, SinkTransform::Same);
        }

        bool isHeldByResult = false;
        if (summary.uses.contains(OperandUse::Retains)) {
            ValueID holder;
            switch (summary.retainedIn) {
            case RetainedIn::UnknownMemory:
                break;
            case RetainedIn::Result:
                isHeldByResult = true;
                holder = userID;
                break;
            case RetainedIn::Operand:
                holder = findOwnAllocation(user.operand(summary.retainingOperand));
                break;
            }
            if (nodeOf(holder) != noNode) {
                uses.remove(OperandUse::Retains);
                addDependency(subject, nodeOf(holder), SinkTransform::Held);
            }
            followFieldOfEnvironment(user, operandIndex, subject);
            if (std::optional<uint32_t> capture = user.variadicOperandIndex(operandIndex); capture && user.opcode == Opcode::NewClosure) {
                if (nodes.hasNodesOf(user.function()))
                    addDependency(subject, nodes.entryParameter(user.function(), *capture), SinkTransform::Leaves);
                else if (flow.mayBeReached(user.function()))
                    uses = OperandUses::all();
            }
        }

        if (!isHandedOn && !isHeldByResult && interpretedOf(user.opcode) != Interpretation::Allocation)
            addDependency(subject, result, SinkTransform::Contents);
        addUses(subject, uses);
    };
    auto visit = [&](ValueID userID, bool isInCycle) {
        const Value& user = function.at(userID);
        std::optional<CalleeList> callees;
        bool speaksOfOperandsAlone = user.opcode != Opcode::GetEnvironmentField;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            uint32_t subject = nodeOf(operand);
            if (subject == noNode || (speaksOfOperandsAlone && mayBecomeAnything(subject)) || user.operandKind(operandIndex) == OperandKind::Guard)
                return;
            if (!collectsAboutEveryNode && !isCollectedAtThisUser(subject))
                return;
            if (effectRuleOf(user.opcode) != EffectRule::Call) {
                visitDeclaredOperand(userID, operandIndex, subject);
                return;
            }
            if (!callees)
                callees = calleesOfCall(program, user);
            visitOperandOfCall(userID, operandIndex, subject, *callees, isInCycle);
        });
    };
    auto visitCallBehindItsRules = [&](ValueID callID) {
        const Value& call = function.at(callID);
        call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            uint32_t subject = nodeOf(operand);
            if (subject == noNode || call.operandKind(operandIndex) == OperandKind::Guard || !isCollectedAtThisUser(subject) || mayBecomeAnything(subject))
                return;
            if (declaredBehaviorOf(call.opcode, operandIndex).uses.contains(OperandUse::RetainsContents))
                visitDeclaredOperand(callID, operandIndex, subject);
            else
                ask(nodeOf(callID));
        });
    };
    auto visitTerminator = [&](const Terminator& terminator) {
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            uint32_t subject = nodeOf(operand);
            if (subject == noNode || terminator.operandKind(operandIndex) == OperandKind::Guard || !isCollectedAtThisUser(subject) || mayBecomeAnything(subject))
                return;
            addUses(subject, summarizeOperand(effects, function, terminator, operandIndex).uses);
        });
    };
    auto visitEdgesOutOf = [&](BlockID block) {
        forEachOutgoingEdge(function, block, [&](Edge, const Target& target) {
            const CompactList<ValueID>& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                if (uint32_t subject = nodeOf(target.arguments[index]); subject != noNode && isCollectedAtThisUser(subject) && !mayBecomeAnything(subject))
                    addDependency(subject, nodeOf(parameters[index]), SinkTransform::Same);
            }
        });
        forEachEdgeResult(function, block, [&](const EdgeResult& result, ValueID parameter) {
            switch (result.kind) {
            case EdgeResultKind::NarrowedOperand:
                if (uint32_t subject = nodeOf(result.subject); subject != noNode && isCollectedAtThisUser(subject))
                    addDependency(subject, nodeOf(parameter), SinkTransform::Same);
                return;
            case EdgeResultKind::Guard:
                return;
            case EdgeResultKind::DeclaredResult:
                function.at(block).terminator.forEachOperand([&](ValueID operand, uint32_t) {
                    if (uint32_t subject = nodeOf(operand); subject != noNode && isCollectedAtThisUser(subject))
                        addDependency(subject, nodeOf(parameter), SinkTransform::Contents);
                });
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    };
    auto walk = [&](bool isFirstWalk) {
        for (BlockID block : blocksInOrder) {
            for (ValueID valueID : function.at(block).values) {
                const Value& value = function.at(valueID);
                if (value.opcode == Opcode::Check)
                    continue;
                if (effectRuleOf(value.opcode) == EffectRule::Call) {
                    visitCallBehindItsRules(valueID);
                    continue;
                }
                if (value.opcode == Opcode::GetEnvironmentField && hasFollowedFields(program.at(value.layout()))) {
                    if (!isFirstWalk)
                        continue;
                    collectsAboutEveryNode = true;
                    visit(valueID, false);
                    collectsAboutEveryNode = false;
                    continue;
                }
                visit(valueID, false);
            }
            visitTerminator(function.at(block).terminator);
            visitEdgesOutOf(block);
        }
    };

    bool savesFrame = function.isUnsplitCoroutine();
    bool handsArgumentsToAnything = false;
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            Opcode opcode = function.at(valueID).opcode;
            savesFrame |= maySuspend(opcode);
            handsArgumentsToAnything |= readsArgumentsOfFrame(opcode);
        }
    }
    if (savesFrame || handsArgumentsToAnything) {
        for (ValueID parameter : function.at(entryBlock).parameters)
            addUses(nodeOf(parameter), handsArgumentsToAnything ? OperandUses::all() : OperandUses { OperandUse::Retains });
    }

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        bool isInCycle = blocksInCycles.quickGet(index);
        for (ValueID valueID : function.blocks[index].values) {
            Opcode opcode = function.at(valueID).opcode;
            if (opcode == Opcode::Check || effectRuleOf(opcode) == EffectRule::Call)
                visit(valueID, isInCycle);
        }
    }

    asksForSources = true;
    collectsAboutEveryNode = false;
    for (ValueID parameter : function.at(entryBlock).parameters)
        ask(nodeOf(parameter));
    walk(true);
    AOT_THRESHOLD(unsigned, mostWalksByDemand, 6);
    for (unsigned numberOfWalks = 1; !nodesOfNextWalk.isEmpty(); ++numberOfWalks) {
        isOfThisWalk.clearAll();
        hasUserThatWasPassed.clearAll();
        for (uint32_t node : std::exchange(nodesOfNextWalk, { }))
            isOfThisWalk.quickSet(node - firstNode);
        for (uint32_t node = 0; numberOfWalks == mostWalksByDemand && node < numberOfNodes; ++node) {
            if (!isAsked.quickSet(node))
                isOfThisWalk.quickSet(node);
        }
        walk(false);
    }

    auto isAboutNodeNobodyAsksFor = [&](const auto& rule) {
        return isOfThisBody(rule.subject) && !isAsked.quickGet(rule.subject - firstNode);
    };
    rules.baseUses.removeAllMatching(isAboutNodeNobodyAsksFor);
    rules.dependencies.removeAllMatching(isAboutNodeNobodyAsksFor);
}

static SinkEquations buildEquations(uint32_t numberOfNodes, const Vector<CollectedRules>& collected, const ParallelLoops& loops)
{
    constexpr uint32_t nodesPerShare = 1 << 14;
    unsigned numberOfShares = (numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    auto forEachShare = [&](const Invocable<void(unsigned share, uint32_t first, uint32_t end)> auto& functor) {
        forEachIndexInParallel(loops, numberOfShares, [&](unsigned share) {
            uint32_t first = share * nodesPerShare;
            functor(share, first, std::min(numberOfNodes, first + nodesPerShare));
        });
    };

    Vector<OperandUses::StorageType> base(numberOfNodes);
    Vector<uint32_t> cursors(numberOfNodes);
    forEachShare([&](unsigned, uint32_t first, uint32_t end) {
        std::ranges::fill(base.mutableSpan().subspan(first, end - first), 0);
        std::ranges::fill(cursors.mutableSpan().subspan(first, end - first), 0);
    });
    forEachIndexInParallel(loops, collected.size(), [&](unsigned index) {
        for (const BaseUses& uses : collected[index].baseUses)
            AtomicRef(base[uses.subject]).fetch_or(uses.uses.toRaw(), std::memory_order_relaxed);
        for (const SinkRule& rule : collected[index].dependencies)
            AtomicRef(cursors[rule.subject]).fetch_add(1, std::memory_order_relaxed);
    });
    Vector<uint32_t> firstDependencyOfShare(numberOfShares);
    forEachShare([&](unsigned share, uint32_t first, uint32_t end) {
        uint32_t numberInShare = 0;
        for (uint32_t node = first; node < end; ++node)
            numberInShare += cursors[node];
        firstDependencyOfShare[share] = numberInShare;
    });
    uint32_t numberOfDependencies = 0;
    for (uint32_t& first : firstDependencyOfShare)
        first = std::exchange(numberOfDependencies, numberOfDependencies + first);

    SinkEquations equations;
    auto baseOf = [&](unsigned node) {
        return OperandUses::fromRaw(base[node]);
    };
    equations.base = ParallelArray<OperandUses>(loops, numberOfNodes, baseOf);
    equations.sinks = ParallelArray<OperandUses>(loops, numberOfNodes, baseOf);
    equations.offsets = Vector<uint32_t>(numberOfNodes + 1);
    forEachShare([&](unsigned share, uint32_t first, uint32_t end) {
        uint32_t position = firstDependencyOfShare[share];
        for (uint32_t node = first; node < end; ++node) {
            equations.offsets[node] = position;
            position += std::exchange(cursors[node], position);
        }
    });
    equations.offsets[numberOfNodes] = numberOfDependencies;

    equations.dependencies = ParallelArray<SinkDependency>(loops, numberOfDependencies, [](unsigned) {
        return SinkDependency { };
    });
    forEachIndexInParallel(loops, collected.size(), [&](unsigned index) {
        for (const SinkRule& rule : collected[index].dependencies)
            equations.dependencies[AtomicRef(cursors[rule.subject]).fetch_add(1, std::memory_order_relaxed)] = rule.dependency;
    });
    return equations;
}

static Vector<uint32_t> collectNodesAskedFor(const Program& program, const OperandNodes& nodes, const ParallelLoops& loops)
{
    Vector<uint32_t> asked = collectInParallel<uint32_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        for (uint32_t parameter = 0; parameter < function.at(entryBlock).parameters.size(); ++parameter)
            found.append(nodes.entryParameter(FunctionID(index), parameter));
    });
    for (uint32_t node = nodes.firstNodeOfLayout.first(); node < nodes.size(); ++node)
        asked.append(node);
    return asked;
}

void computeOperandSummaries(const Program& program, FlowResults& results, BuildContext& build)
{
    if (!results.hasResults())
        return;

    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    OperandNodes nodes = numberNodes(program, loops);
    Vector<CollectedRules> collected(numberOfFunctions);
    ParameterUnions parameterUnions(nodes.size());
    {
        ProgramCaches caches(program, build);
        forEachIndexInParallel(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index) {
            if (program.functions[index].hasBody())
                collectRulesOfBody(program, results, nodes, parameterUnions, FunctionID(index), collected[index]);
        });
    }
    collected.append(parameterUnions.rules());
    SinkEquations equations = buildEquations(nodes.size() + parameterUnions.size(), collected, loops);
    build.destroyLater(WTF::move(collected));
    solveSinkEquations(equations, collectNodesAskedFor(program, nodes, loops).span(), loops);

    Vector<Vector<OperandSummary>> parameterSummaries(results.functionCount());
    forEachIndexInParallel(loops, parameterSummaries.size(), [&](unsigned index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (!function.hasBody() || !build.isEnabled(FlowResultKind::OperandSummaries, index))
            return;
        const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
        parameterSummaries[index] = Vector<OperandSummary>(parameters.size(), [&](size_t parameter) {
            if (!mayBeReference(program.typeOf(function, parameters[parameter])))
                return OperandSummary { OperandUse::Inspects, RetainedIn::UnknownMemory, 0 };
            return OperandSummary { equations.sinks[nodes.entryParameter(id, parameter)], RetainedIn::UnknownMemory, 0 };
        });
    });

    Vector<Vector<OperandUses>> storedValueUses(program.layouts.size());
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!hasFollowedFields(layout) || !build.isEnabled(FlowResultKind::OperandSummaries, index))
            continue;
        storedValueUses[index] = Vector<OperandUses>(layout.fields.size(), [&](size_t field) {
            return equations.sinks[nodes.environmentField(LayoutID(index), field)];
        });
    }
    results.setOperandSummaries(WTF::move(parameterSummaries), WTF::move(storedValueUses));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
