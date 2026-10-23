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
#include "AOTProgramCaches.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <memory>
#include <optional>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noNode = UINT32_MAX;

// One node for each value that may hold a reference, and one for each field of an environment.
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

// What the operations of one body do with the values that they take.
class BodyRuleCollector {
public:
    BodyRuleCollector(const Program& program, const FlowResults& flow, const OperandNodes& nodes, FunctionID id, CollectedRules& rules)
        : m_program(program)
        , m_flow(flow)
        , m_nodes(nodes)
        , m_id(id)
        , m_function(program.at(id))
        , m_rules(rules)
        , m_effects { program, m_unknownSummaries }
    {
    }

    void collect();

private:
    void numberValues();
    void findBlocksInCycles();
    uint32_t nodeOf(ValueID value) const { return value ? m_nodeOfValue[value.index()] : noNode; }

    void addUses(uint32_t subject, OperandUses uses)
    {
        m_rules.baseUses.append(BaseUses { subject, uses });
        if (uses == OperandUses::all() && subject - m_firstNode < m_mayBecomeAnything.size())
            m_mayBecomeAnything.quickSet(subject - m_firstNode);
    }
    bool mayBecomeAnything(uint32_t subject) const { return m_mayBecomeAnything.quickGet(subject - m_firstNode); }
    void addDependency(uint32_t subject, uint32_t source, SinkTransform transform)
    {
        if (subject != noNode && source != noNode)
            m_rules.dependencies.append(SinkRule { subject, SinkDependency { source, source, transform } });
    }

    void visitEntryParameters();
    void visit(ValueID, bool isInCycle);
    void visit(const Terminator&);
    void visitEdgesOutOf(BlockID);
    void visitOperandOfCall(ValueID call, uint32_t operandIndex, uint32_t subject, const CalleeList&, bool isInCycle);
    void visitDeclaredOperand(ValueID user, uint32_t operandIndex, uint32_t subject);
    void passTo(uint32_t subject, FunctionID callee, std::optional<uint32_t> entryParameter, ValueID call, bool isInCycle);
    OperandUses usesBesideDeclaredOnes(const Value& user, uint32_t operandIndex) const;
    void followFieldOfEnvironment(const Value& user, uint32_t operandIndex, uint32_t subject);
    ValueID findOwnAllocation(ValueID) const;

    const Program& m_program;
    const FlowResults& m_flow;
    const OperandNodes& m_nodes;
    FunctionID m_id;
    const Function& m_function;
    CollectedRules& m_rules;
    EffectSummaries m_unknownSummaries;
    EffectContext m_effects;
    Vector<uint32_t> m_nodeOfValue;
    uint32_t m_firstNode { 0 };
    BitVector m_mayBecomeAnything;
    BitVector m_blocksInCycles;
};

} // anonymous namespace

static bool mayHoldReference(const Program& program, const OperandNodes& nodes, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    return nodes.mayBeReferenceByType[(value.type ? value.type : TypeTable::anything).index()] || numberOfResultsOf(program, function, value).has_value();
}

static bool hasFollowedFields(const Layout& layout)
{
    return layout.kind == LayoutKind::Environment;
}

static uint32_t numberOfNodesOf(const Program& program, const OperandNodes& nodes, const Function& function)
{
    if (!function.hasBody())
        return 0;
    uint32_t count = 0;
    for (uint32_t index = 0; index < function.values.size(); ++index)
        count += mayHoldReference(program, nodes, function, ValueID(index));
    for (ValueID parameter : function.at(entryBlock).parameters)
        count += !mayHoldReference(program, nodes, function, parameter);
    return count;
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
        nodes.firstNodeOfFunction[index + 1] = numberOfNodesOf(program, nodes, program.functions[index]);
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

void BodyRuleCollector::numberValues()
{
    m_nodeOfValue.fill(noNode, m_function.values.size());
    m_firstNode = m_nodes.firstNodeOfFunction[m_id.index()];
    m_mayBecomeAnything = BitVector(m_nodes.firstNodeOfFunction[m_id.index() + 1] - m_firstNode);
    uint32_t next = m_firstNode;
    for (ValueID parameter : m_function.at(entryBlock).parameters)
        m_nodeOfValue[parameter.index()] = next++;
    for (uint32_t index = 0; index < m_function.values.size(); ++index) {
        if (m_nodeOfValue[index] == noNode && mayHoldReference(m_program, m_nodes, m_function, ValueID(index)))
            m_nodeOfValue[index] = next++;
    }
    RELEASE_ASSERT(next == m_nodes.firstNodeOfFunction[m_id.index() + 1]);
}

void BodyRuleCollector::findBlocksInCycles()
{
    m_blocksInCycles = BitVector(m_function.blocks.size());
    if (m_function.blocks.size() == 1)
        return;
    CFG cfg(m_function);
    BlockCycles cycles(cfg);
    for (BlockID block : cfg.reversePostOrder()) {
        if (cycles.isInCycle(block))
            m_blocksInCycles.quickSet(block.index());
    }
}

void BodyRuleCollector::collect()
{
    numberValues();
    findBlocksInCycles();
    visitEntryParameters();
    auto speaksFirst = [&](ValueID id) {
        Opcode opcode = m_function.at(id).opcode;
        return opcode == Opcode::Check || effectRuleOf(opcode) == EffectRule::Call;
    };
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        bool isInCycle = m_blocksInCycles.quickGet(index);
        for (ValueID id : m_function.blocks[index].values) {
            if (speaksFirst(id))
                visit(id, isInCycle);
        }
    }
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        BlockID block(index);
        bool isInCycle = m_blocksInCycles.quickGet(index);
        for (ValueID id : m_function.at(block).values) {
            if (!speaksFirst(id))
                visit(id, isInCycle);
        }
        visit(m_function.at(block).terminator);
        visitEdgesOutOf(block);
    }
}

// These read the arguments of the frame, which are no operands of theirs.
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

// A saved frame holds what is live in it, and the function goes on when its caller has long gone on.
void BodyRuleCollector::visitEntryParameters()
{
    bool savesFrame = m_function.isUnsplitCoroutine();
    bool handsArgumentsToAnything = false;
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values) {
            Opcode opcode = m_function.at(id).opcode;
            savesFrame |= maySuspend(opcode);
            handsArgumentsToAnything |= readsArgumentsOfFrame(opcode);
        }
    }
    if (!savesFrame && !handsArgumentsToAnything)
        return;
    for (ValueID parameter : m_function.at(entryBlock).parameters)
        addUses(nodeOf(parameter), handsArgumentsToAnything ? OperandUses::all() : OperandUses { OperandUse::Retains });
}

void BodyRuleCollector::visitEdgesOutOf(BlockID block)
{
    forEachOutgoingEdge(m_function, block, [&](Edge, const Target& target) {
        const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (uint32_t subject = nodeOf(target.arguments[index]); subject != noNode && !mayBecomeAnything(subject))
                addDependency(subject, nodeOf(parameters[index]), SinkTransform::Same);
        }
    });
    forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            addDependency(nodeOf(result.subject), nodeOf(parameter), SinkTransform::Same);
            return;
        case EdgeResultKind::Guard:
            return;
        case EdgeResultKind::DeclaredResult:
            m_function.at(block).terminator.forEachOperand([&](ValueID operand, uint32_t) {
                addDependency(nodeOf(operand), nodeOf(parameter), SinkTransform::Contents);
            });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

void BodyRuleCollector::visit(const Terminator& terminator)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        uint32_t subject = nodeOf(operand);
        if (subject == noNode || mayBecomeAnything(subject) || terminator.operandKind(operandIndex) == OperandKind::Guard)
            return;
        addUses(subject, summarizeOperand(m_effects, m_function, terminator, operandIndex).uses);
    });
}

void BodyRuleCollector::visit(ValueID id, bool isInCycle)
{
    const Value& user = m_function.at(id);
    std::optional<CalleeList> callees;
    bool speaksOfOperandsAlone = user.opcode != Opcode::GetEnvironmentField;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        uint32_t subject = nodeOf(operand);
        if (subject == noNode || (speaksOfOperandsAlone && mayBecomeAnything(subject)) || user.operandKind(operandIndex) == OperandKind::Guard)
            return;
        if (effectRuleOf(user.opcode) != EffectRule::Call) {
            visitDeclaredOperand(id, operandIndex, subject);
            return;
        }
        if (!callees)
            callees = calleesOfCall(m_program, user);
        visitOperandOfCall(id, operandIndex, subject, *callees, isInCycle);
    });
}

void BodyRuleCollector::passTo(uint32_t subject, FunctionID callee, std::optional<uint32_t> entryParameter, ValueID call, bool isInCycle)
{
    if (!entryParameter || !m_nodes.hasNodesOf(callee)) {
        addUses(subject, OperandUses::all());
        return;
    }
    uint32_t parameter = m_nodes.entryParameter(callee, *entryParameter);
    addDependency(subject, parameter, isInCycle ? SinkTransform::RepeatedArgument : SinkTransform::Argument);
    if (uint32_t result = nodeOf(call); result != noNode)
        m_rules.dependencies.append(SinkRule { subject, SinkDependency { result, parameter, SinkTransform::IfReturned } });
}

// The call leaves the operand behind: a receiver where the callee has no `this`, an argument behind the last parameter of a callee that cannot ask for the arguments of its frame.
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

// As summarizeOperand() judges the operand of a call, with the entry parameters of the callees in place of their summaries.
void BodyRuleCollector::visitOperandOfCall(ValueID id, uint32_t operandIndex, uint32_t subject, const CalleeList& callees, bool isInCycle)
{
    const Value& call = m_function.at(id);
    OperandUses declaredUses = declaredBehaviorOf(call.opcode, operandIndex).uses;
    if (declaredUses.contains(OperandUse::RetainsContents)) {
        visitDeclaredOperand(id, operandIndex, subject);
        return;
    }

    bool isCalledValue = declaredUses.containsAny(callingUses);
    OperandUses usesOfCalledValue = declaredUses | OperandUse::Inspects;
    if (isInCycle)
        usesOfCalledValue.add(OperandUse::CallsRepeatedly);
    if (isCalledValue && call.opcode == Opcode::Call && !callees.isComplete) {
        addUses(subject, usesOfCalledValue);
        return;
    }
    if (!callees.isComplete || !callees.builtins.isEmpty() || maySuspend(call.opcode)) {
        addUses(subject, OperandUses::all());
        return;
    }

    if (isCalledValue) {
        addUses(subject, usesOfCalledValue);
        for (FunctionID callee : callees.functions) {
            for (uint32_t entryParameter : entryParametersOfCalledValue(m_program.at(callee), call))
                passTo(subject, callee, entryParameter, id, isInCycle);
        }
        return;
    }
    addUses(subject, OperandUse::Inspects);
    for (FunctionID callee : callees.functions) {
        if (!isDroppedBy(m_program.at(callee), call, operandIndex))
            passTo(subject, callee, entryParameterOf(m_program.at(callee), call, operandIndex), id, isInCycle);
    }
}

// The schema leaves what a store by name does to its receiver to the effects of the operation, and what becomes of a prototype to the operation that reads it.
OperandUses BodyRuleCollector::usesBesideDeclaredOnes(const Value& user, uint32_t operandIndex) const
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

// What becomes of a value in a field of an environment is what the readers of the field do with it, wherever they are.
void BodyRuleCollector::followFieldOfEnvironment(const Value& user, uint32_t operandIndex, uint32_t subject)
{
    switch (user.opcode) {
    case Opcode::PutEnvironmentField:
        if (hasFollowedFields(m_program.at(user.layout())))
            addDependency(subject, m_nodes.environmentField(user.layout(), user.field()), SinkTransform::Same);
        return;
    case Opcode::NewObject:
        if (std::optional<uint32_t> index = user.variadicOperandIndex(operandIndex); index && hasFollowedFields(m_program.at(user.layout())))
            addDependency(subject, m_nodes.environmentField(user.layout(), user.fields()[*index]), SinkTransform::Same);
        return;
    default:
        return;
    }
}

// The allocation of this body that the value is, behind casts. None for a value that may be an object of somebody else: such an object outlives the activation whatever becomes of the value here.
ValueID BodyRuleCollector::findOwnAllocation(ValueID id) const
{
    while (id) {
        const Value& value = m_function.at(id);
        if (interpretedOf(value.opcode) == Interpretation::Allocation)
            return id;
        if (!isCastOrConversion(value.opcode))
            return { };
        id = value.operand(0);
    }
    return { };
}

void BodyRuleCollector::visitDeclaredOperand(ValueID id, uint32_t operandIndex, uint32_t subject)
{
    const Value& user = m_function.at(id);
    uint32_t result = nodeOf(id);

    switch (user.opcode) {
    case Opcode::Check:
        // Where the test fails, a copy of the operations behind it runs that knows nothing about the value.
        addUses(subject, OperandUses::all());
        return;
    case Opcode::ResultAt:
        addDependency(subject, result, SinkTransform::Same);
        return;
    case Opcode::GetEnvironmentField:
        if (hasFollowedFields(m_program.at(user.layout()))) {
            addUses(subject, OperandUse::Inspects);
            addDependency(m_nodes.environmentField(user.layout(), user.field()), result, SinkTransform::Leaves);
            return;
        }
        break;
    default:
        break;
    }

    OperandSummary summary = summarizeOperand(m_effects, m_function, user, operandIndex);
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
            holder = id;
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
        // Whoever calls the closure runs its function, which does with a capture what it does with the entry parameter. A function that is never reached does nothing.
        if (std::optional<uint32_t> capture = user.variadicOperandIndex(operandIndex); capture && user.opcode == Opcode::NewClosure) {
            if (m_nodes.hasNodesOf(user.function()))
                addDependency(subject, m_nodes.entryParameter(user.function(), *capture), SinkTransform::Leaves);
            else if (m_flow.mayBeReached(user.function()))
                uses = OperandUses::all();
        }
    }

    if (!isHandedOn && !isHeldByResult && interpretedOf(user.opcode) != Interpretation::Allocation)
        addDependency(subject, result, SinkTransform::Contents);
    addUses(subject, uses);
}

// The order of the dependencies of a node follows from which thread came first. What is solved for is the least solution, which follows from the equations alone.
static SinkEquations buildEquations(const OperandNodes& nodes, const Vector<CollectedRules>& collected, const ParallelLoops& loops)
{
    constexpr uint32_t nodesPerShare = 1 << 14;
    uint32_t numberOfNodes = nodes.size();
    unsigned numberOfShares = (numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    auto forEachShare = [&](const Invocable<void(unsigned share, uint32_t first, uint32_t end)> auto& functor) {
        forEachIndexInParallel(loops, numberOfShares, [&](unsigned share) {
            uint32_t first = share * nodesPerShare;
            functor(share, first, std::min(numberOfNodes, first + nodesPerShare));
        });
    };

    // Plain numbers, which a vector leaves as they are: the threads that count are the first to touch them.
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

// The summaries are read off the entry parameters and off the fields of environments. What becomes of a value that none of them depends on is nobody's question.
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
    {
        ProgramCaches caches(program, build);
        forEachIndexInParallel(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index) {
            if (program.functions[index].hasBody())
                BodyRuleCollector(program, results, nodes, FunctionID(index), collected[index]).collect();
        });
    }
    SinkEquations equations = buildEquations(nodes, collected, loops);
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
