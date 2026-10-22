/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSSARepair.h"

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t headOrder = 0;
static constexpr uint32_t firstValueOrder = 1;

static uint32_t cutIndexAfter(const Function& function, ValueLocation definition)
{
    return std::max(definition.index + 1, indexAfterCaughtValues(function, function.at(definition.block)));
}

static BlockID findContinuationAfter(const Function& function, const CFG& cfg, ValueLocation definition)
{
    const Block& block = function.at(definition.block);
    if (cutIndexAfter(function, definition) != block.values.size() || block.terminator.kind != TerminatorKind::Jump)
        return BlockID();
    const Target& next = block.terminator.next();
    if (!next.arguments.isEmpty() || !cfg.singleIncomingEdge(next.block))
        return BlockID();
    return next.block;
}

static bool isUseAtBlockHead(const Use& use)
{
    switch (use.kind) {
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    case Use::Kind::Operand:
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mustNameDefinition(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return mustBeDefinition(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand:
        return mustBeDefinition(function.at(use.block).terminator.operandKind(use.operandIndex));
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool namesGuard(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

SSARepair::SSARepair(IRBuilder& builder, FunctionAnalyses& analyses)
    : m_builder(builder)
    , m_analyses(analyses)
{
    RELEASE_ASSERT(&builder.function() == &analyses.function());
}

SSARepair::Variable SSARepair::addVariable(ValueID definition)
{
    Variable variable = m_variables.size();
    m_variables.append(VariableData { });
    addDefinition(variable, definition);
    return variable;
}

void SSARepair::addDefinition(Variable variable, ValueID definition)
{
    RELEASE_ASSERT(!m_didRun);
    RELEASE_ASSERT(definition.index() < m_analyses.function().values.size());
    bool wasDefinition = m_isDefinition.set(definition.index());
    RELEASE_ASSERT(!wasDefinition);
    m_variables[variable].values.append(definition);
}

std::optional<SSARepair::Variable> SSARepair::findVariable(ValueID definition) const
{
    if (!m_isDefinition.get(definition.index()))
        return std::nullopt;
    for (Variable variable = 0; variable < m_variables.size(); ++variable) {
        if (m_variables[variable].values.contains(definition))
            return variable;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SSARepair::run()
{
    RELEASE_ASSERT(!m_didRun);
    RELEASE_ASSERT(m_builder.m_insertions.isEmpty());
    Origin savedOrigin = m_builder.origin();
    separateUsesAsGuards();
    m_didRun = true;
    if (m_variables.isEmpty())
        return;

    splitCoveredBlocksAfterDefinitions();

    Vector<Variable> constantVariables;
    Vector<Variable> mergedVariables;
    for (Variable variable = 0; variable < m_variables.size(); ++variable) {
        VariableData& data = m_variables[variable];
        collectUses(data);
        if (data.uses.isEmpty())
            continue;
        collectDefinitions(data);
        if (data.definitions.isEmpty())
            continue;
        if (!hasOneConstant(data)) {
            mergedVariables.append(variable);
            continue;
        }
        findDominatedUses(data);
        bool needsDefinitionAtBlockHead = data.uses.containsIf([](const RecordedUse& recorded) {
            return !recorded.isDominated && isUseAtBlockHead(recorded.use);
        });
        if (needsDefinitionAtBlockHead)
            mergedVariables.append(variable);
        else
            constantVariables.append(variable);
    }

    if (!constantVariables.isEmpty())
        rematerializeConstants(constantVariables.span());
    if (!mergedVariables.isEmpty())
        insertParameters(mergedVariables.span());

    if (!m_builder.m_insertions.isEmpty())
        m_builder.applyInsertions();
    m_analyses.didChangeValuesButNotUses();
    m_builder.setOrigin(savedOrigin);
}

// An operation may name an allocation as the guard for what holds of a new object. A parameter that two allocations meet in is the object and no guard.
// So the uses as a guard get a variable of their own: its definitions are an AllGuards behind each allocation, and the parameters they meet in are guards.
void SSARepair::separateUsesAsGuards()
{
    struct NamedDefinition {
        Variable variable;
        ValueID value;
        ProgramPoint pointAfter;
        Vector<Use, 4> usesAsGuard;
    };

    Function& function = m_builder.function();
    Vector<NamedDefinition, 4> namedDefinitions;
    {
        // An insertion ends the life of the use lists.
        const UseLists& useLists = m_analyses.useLists();
        const CFG& cfg = m_analyses.cfg();
        for (Variable variable = 0; variable < m_variables.size(); ++variable) {
            Vector<ValueID, 2> values = m_variables[variable].values;
            values.removeAllMatching([&](ValueID value) {
                return useLists.locationOf(value).placement == ValuePlacement::Detached;
            });
            // A use in the block of its definition keeps that definition.
            auto namesGuardFromOtherBlock = [&](ValueID value, const Use& use) {
                return namesGuard(function, use) && use.block != useLists.locationOf(value).block;
            };
            // So does a use that no other definition reaches.
            auto isReachedByOtherDefinition = [&](ValueID value) {
                BitVector visited;
                Vector<BlockID, 16> worklist;
                for (const Use& use : useLists.usesOf(value)) {
                    if (namesGuardFromOtherBlock(value, use) && !visited.set(use.block.index()))
                        worklist.append(use.block);
                }
                auto holdsOtherDefinition = [&](BlockID block) {
                    return std::ranges::any_of(values, [&](ValueID other) {
                        return other != value && useLists.locationOf(other).block == block;
                    });
                };
                BlockID definingBlock = useLists.locationOf(value).block;
                while (!worklist.isEmpty()) {
                    BlockID block = worklist.takeLast();
                    auto incomingEdges = cfg.incomingEdges(block);
                    if (holdsOtherDefinition(block) || incomingEdges.empty())
                        return true;
                    for (Edge edge : incomingEdges) {
                        if (edge.kind == EdgeKind::Handler)
                            return true;
                        if (edge.from != definingBlock && !visited.set(edge.from.index()))
                            worklist.append(edge.from);
                    }
                }
                return false;
            };
            bool isNamedAsGuard = std::ranges::any_of(values, isReachedByOtherDefinition);
            // Every way to a use needs a definition of the guard.
            bool everyDefinitionCanBeNamed = std::ranges::all_of(values, [&](ValueID value) {
                return function.at(value).type != TypeTable::guard && useLists.locationOf(value).placement == ValuePlacement::InBlock;
            });
            if (!isNamedAsGuard || !everyDefinitionCanBeNamed)
                continue;
            for (ValueID value : values) {
                NamedDefinition definition { variable, value, useLists.pointAfter(value), { } };
                for (const Use& use : useLists.usesOf(value)) {
                    if (namesGuardFromOtherBlock(value, use))
                        definition.usesAsGuard.append(use);
                }
                namedDefinitions.append(WTF::move(definition));
            }
        }
    }
    if (namedDefinitions.isEmpty())
        return;

    std::optional<Variable> guards;
    for (size_t index = 0; index < namedDefinitions.size(); ++index) {
        const NamedDefinition& definition = namedDefinitions[index];
        m_builder.setOrigin(function.at(definition.value).origin());
        ValueID wrapper = m_builder.insertBefore(definition.pointAfter, IRBuilder::make(Opcode::AllGuards, { definition.value }));
        for (const Use& use : definition.usesAsGuard) {
            replaceUse(function, use, wrapper);
            m_analyses.didChangeBlock(use.block);
        }
        if (index && namedDefinitions[index - 1].variable == definition.variable)
            addDefinition(*guards, wrapper);
        else
            guards = addVariable(wrapper);
    }
    m_builder.applyInsertions();
}

void SSARepair::splitCoveredBlocksAfterDefinitions()
{
    const Function& function = m_analyses.function();
    const CFG& cfg = m_analyses.cfg();
    if (!cfg.hasHandlers())
        return;

    const UseLists& useLists = m_analyses.useLists();
    Vector<ProgramPoint, 8> cuts;
    for (const VariableData& data : m_variables) {
        bool isUsed = data.values.containsIf([&](ValueID value) {
            return !!useLists.useCount(value);
        });
        if (!isUsed)
            continue;
        for (ValueID value : data.values) {
            ValueLocation location = useLists.locationOf(value);
            if (location.placement != ValuePlacement::InBlock || !cfg.hasHandler(location.block))
                continue;
            if (!findContinuationAfter(function, cfg, location))
                cuts.append(ProgramPoint { location.block, cutIndexAfter(function, location) });
        }
    }
    if (cuts.isEmpty())
        return;

    std::sort(cuts.begin(), cuts.end(), [](const ProgramPoint& a, const ProgramPoint& b) {
        return std::tie(a.block, a.index) < std::tie(b.block, b.index);
    });
    removeRepeatedElements(cuts);

    Vector<uint32_t, 8> indexes;
    for (size_t first = 0; first < cuts.size(); first += indexes.size()) {
        BlockID block = cuts[first].block;
        indexes.shrink(0);
        for (size_t next = first; next < cuts.size() && cuts[next].block == block; ++next)
            indexes.append(cuts[next].index);
        m_builder.setOrigin(function.at(block).terminator.origin());
        m_builder.splitBlock(block, indexes.span());
    }
}

void SSARepair::collectUses(VariableData& data)
{
    const UseLists& useLists = m_analyses.useLists();
    for (ValueID value : data.values) {
        for (const Use& use : useLists.usesOf(value))
            data.uses.append(RecordedUse { use, useLists.pointOf(use), value, false });
    }
    std::sort(data.uses.begin(), data.uses.end(), [&](const RecordedUse& a, const RecordedUse& b) {
        return useLists.isBefore(a.use, b.use);
    });
}

void SSARepair::collectDefinitions(VariableData& data)
{
    const Function& function = m_analyses.function();
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    for (ValueID value : data.values) {
        ValueLocation location = useLists.locationOf(value);
        switch (location.placement) {
        case ValuePlacement::Detached:
            break;
        case ValuePlacement::Parameter:
            data.definitions.append(Definition { location.block, headOrder, value });
            break;
        case ValuePlacement::InBlock: {
            if (!cfg.hasHandler(location.block)) {
                data.definitions.append(Definition { location.block, firstValueOrder + location.index, value });
                break;
            }
            // The handler edge of a covered block never carries a value of that block, so the value counts as defined at the head of the one block its terminator enters.
            BlockID continuation = findContinuationAfter(function, cfg, location);
            RELEASE_ASSERT(continuation);
            data.definitions.append(Definition { continuation, headOrder, value });
            break;
        }
        }
    }

    data.values.shrink(0);
    for (const Definition& definition : data.definitions)
        data.values.append(definition.value);
    sortDefinitions(data);
}

bool SSARepair::hasOneConstant(const VariableData& data) const
{
    const Function& function = m_analyses.function();
    const Value& first = function.at(data.values.first());
    for (ValueID id : data.values) {
        const Value& value = function.at(id);
        if (!isRematerializable(value) || value.opcode != first.opcode || !value.hasSamePayloadAs(first))
            return false;
    }
    return true;
}

void SSARepair::findDominatedUses(VariableData& data)
{
    for (RecordedUse& recorded : data.uses)
        recorded.isDominated = m_analyses.dominates(recorded.operand, recorded.use);
}

void SSARepair::rematerializeConstants(std::span<const Variable> variables)
{
    unsigned numberOfBlocks = m_analyses.cfg().numNodes();
    m_constantAtTailStamp.fill(0, numberOfBlocks);
    m_constantAtTail.fill(ValueID(), numberOfBlocks);
    for (Variable variable : variables)
        rematerializeConstant(variable);
}

void SSARepair::rematerializeConstant(Variable variable)
{
    const VariableData& data = m_variables[variable];
    Function& function = m_builder.function();
    for (const Definition& definition : data.definitions)
        setConstantAtTail(variable, definition.block, definition.value);

    BlockID block;
    ValueID constant;
    for (const RecordedUse& recorded : data.uses) {
        if (recorded.isDominated)
            continue;
        if (recorded.use.block != block) {
            block = recorded.use.block;
            constant = findLocalDefinition(data, recorded);
            if (!constant)
                constant = findConstantAtHead(variable, block);
            if (!constant) {
                constant = insertConstant(data, recorded.point);
                setConstantAtTail(variable, block, constant);
            }
        }
        replaceUse(function, recorded.use, constant);
        m_analyses.didChangeBlock(recorded.use.block);
    }
}

ValueID SSARepair::insertConstant(const VariableData& data, ProgramPoint point)
{
    Function& function = m_builder.function();
    ValueID copy = m_builder.addValue(Value(function.at(data.values.first())));
    m_builder.didAddDetachedValue(copy);
    m_builder.m_insertions.insert(point, copy);
    return copy;
}

void SSARepair::setConstantAtTail(Variable variable, BlockID block, ValueID constant)
{
    m_constantAtTailStamp[block.index()] = variable + 1;
    m_constantAtTail[block.index()] = constant;
}

ValueID SSARepair::findConstantAtHead(Variable variable, BlockID block)
{
    const CFG& cfg = m_analyses.cfg();
    const Dominators& dominators = m_analyses.dominators();
    Vector<BlockID, 16> visitedDominators;
    ValueID constant;
    for (BlockID dominator = dominators.idom(block); dominator; dominator = dominators.idom(dominator)) {
        if (m_constantAtTailStamp[dominator.index()] == variable + 1 && dominators.terminatorDominates(dominator, block)) {
            constant = m_constantAtTail[dominator.index()];
            break;
        }
        visitedDominators.append(dominator);
    }
    if (cfg.hasHandlers())
        return constant;
    for (BlockID dominator : visitedDominators)
        setConstantAtTail(variable, dominator, constant);
    return constant;
}

void SSARepair::insertParameters(std::span<const Variable> variables)
{
    unsigned numberOfBlocks = m_analyses.cfg().numNodes();
    m_liveAtHeadStamp.fill(0, numberOfBlocks);
    m_hasDefinitionStamp.fill(0, numberOfBlocks);
    m_numberOfParametersForEdgeResults.fill(std::numeric_limits<uint32_t>::max(), numberOfBlocks);

    Calculator calculator(m_analyses);
    for (Variable variable : variables) {
        Calculator::Variable* calculatorVariable = calculator.newVariable();
        for (const Definition& definition : m_variables[variable].definitions)
            calculator.newDef(calculatorVariable, definition.block, definition.value);
    }

    std::optional<Variable> livenessVariable;
    calculator.computePhis([&](Calculator::Variable* calculatorVariable, BlockID block) -> ValueID {
        Variable variable = variables[calculatorVariable->index()];
        if (livenessVariable != variable) {
            computeLiveness(variable);
            livenessVariable = variable;
        }
        if (!isLiveAtHead(variable, block))
            return ValueID();
        return addMergeParameter(m_variables[variable], block);
    });

    for (Variable variable : variables)
        sortDefinitions(m_variables[variable]);

    // A recorded use names an argument by index, and appendArgument shifts the arguments after the one it inserts.
    for (unsigned index = 0; index < variables.size(); ++index)
        rewriteUses(calculator, calculator.variable(index), m_variables[variables[index]]);
    appendArguments(calculator, variables);
}

void SSARepair::computeLiveness(Variable variable)
{
    const CFG& cfg = m_analyses.cfg();
    const VariableData& data = m_variables[variable];
    unsigned stamp = variable + 1;
    for (const Definition& definition : data.definitions)
        m_hasDefinitionStamp[definition.block.index()] = stamp;

    Vector<BlockID, 16> worklist;
    auto mark = [&](BlockID block) {
        unsigned& blockStamp = m_liveAtHeadStamp[block.index()];
        if (blockStamp == stamp)
            return;
        blockStamp = stamp;
        worklist.append(block);
    };
    for (const RecordedUse& recorded : data.uses) {
        if (!findLocalDefinition(data, recorded))
            mark(recorded.use.block);
    }
    while (!worklist.isEmpty()) {
        for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
            switch (edge.kind) {
            case EdgeKind::Normal:
                if (m_hasDefinitionStamp[edge.from.index()] != stamp)
                    mark(edge.from);
                break;
            case EdgeKind::Handler:
                if (!findLastDefinitionBefore(data, edge.from, firstValueOrder))
                    mark(edge.from);
                break;
            }
        }
    }
}

void SSARepair::computeParameterType(VariableData& data)
{
    const Program& program = m_analyses.context().program;
    const Function& function = m_analyses.function();
    const Value& first = function.at(data.values.first());
    data.type = first.type;
    TypeID expected = first.expectedOrType();
    for (ValueID id : data.values) {
        const Value& value = function.at(id);
        RELEASE_ASSERT(!numberOfResultsOf(program, function, value));
        data.type = m_builder.m_types.unionOf(data.type, value.type);
        expected = m_builder.m_types.unionOf(expected, value.expectedOrType());
    }
    data.expected = expected == data.type ? TypeID() : expected;
}

uint32_t SSARepair::numberOfParametersForEdgeResults(BlockID block)
{
    uint32_t& number = m_numberOfParametersForEdgeResults[block.index()];
    if (number == std::numeric_limits<uint32_t>::max())
        number = m_builder.numberOfParametersForEdgeResults(block);
    return number;
}

ValueID SSARepair::addMergeParameter(VariableData& data, BlockID block)
{
    if (!data.type)
        computeParameterType(data);

    Function& function = m_builder.function();
    m_builder.setOrigin(function.at(data.values.first()).origin());
    ValueID parameter = m_builder.addParameter(block, data.type, numberOfParametersForEdgeResults(block));
    function.edit(parameter).setExpected(data.expected);
    data.definitions.append(Definition { block, headOrder, parameter });
    return parameter;
}

void SSARepair::appendArguments(Calculator& calculator, std::span<const Variable> variables)
{
    const Function& function = m_analyses.function();
    const CFG& cfg = m_analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        const Vector<Calculator::Def*>& phis = calculator.phisForBlock(block);
        if (phis.isEmpty())
            continue;
        for (Edge edge : cfg.incomingEdges(block)) {
            for (Calculator::Def* phi : phis) {
                ValueID argument = reachingDefinitionOnEdge(calculator, phi->variable(), m_variables[variables[phi->variable()->index()]], edge);
                m_builder.appendArgument(edge, argument, numberOfParametersForEdgeResults(block));
            }
        }
    }

    for (Edge edge : m_analyses.edgesFromUnreachableBlocks()) {
        BlockID target = targetOf(function, edge).block;
        for (Calculator::Def* phi : calculator.phisForBlock(target))
            m_builder.appendArgument(edge, m_variables[variables[phi->variable()->index()]].values.first(), numberOfParametersForEdgeResults(target));
    }
}

ValueID SSARepair::reachingDefinitionOnEdge(Calculator& calculator, Calculator::Variable* calculatorVariable, const VariableData& data, Edge edge)
{
    Calculator::Def* reachingDefinition = nullptr;
    switch (edge.kind) {
    case EdgeKind::Normal:
        reachingDefinition = calculator.reachingDefAtTail(edge.from, calculatorVariable);
        break;
    case EdgeKind::Handler:
        if (const Definition* definition = findLastDefinitionBefore(data, edge.from, firstValueOrder))
            return definition->value;
        reachingDefinition = calculator.reachingDefAtHead(edge.from, calculatorVariable);
        break;
    }
    RELEASE_ASSERT(reachingDefinition);
    return reachingDefinition->value();
}

void SSARepair::rewriteUses(Calculator& calculator, Calculator::Variable* calculatorVariable, const VariableData& data)
{
    Function& function = m_builder.function();
    for (const RecordedUse& recorded : data.uses) {
        const Use& use = recorded.use;
        ValueID definition = findLocalDefinition(data, recorded);
        if (!definition) {
            Calculator::Def* reachingDefinition = calculator.reachingDefAtHead(use.block, calculatorVariable);
            RELEASE_ASSERT(reachingDefinition);
            definition = reachingDefinition->value();
        }
        if (definition == recorded.operand)
            continue;
        if (mustNameDefinition(function, use))
            RELEASE_ASSERT(function.at(definition).opcode != Opcode::Parameter);
        replaceUse(function, use, definition);
        m_analyses.didChangeBlock(use.block);
    }
}

ValueID SSARepair::findLocalDefinition(const VariableData& data, const RecordedUse& recorded) const
{
    const Definition* definition = findLastDefinitionBefore(data, recorded.use.block, firstValueOrder + recorded.point.index);
    return definition ? definition->value : ValueID();
}

const SSARepair::Definition* SSARepair::findLastDefinitionBefore(const VariableData& data, BlockID block, uint32_t order)
{
    auto definitions = data.definitions.span();
    auto upperBound = std::ranges::partition_point(definitions, [&](const Definition& definition) {
        return std::tie(definition.block, definition.order) < std::tie(block, order);
    });
    if (upperBound == definitions.begin())
        return nullptr;
    const Definition& last = *std::prev(upperBound);
    return last.block == block ? &last : nullptr;
}

void SSARepair::sortDefinitions(VariableData& data)
{
    std::stable_sort(data.definitions.begin(), data.definitions.end(), [](const Definition& a, const Definition& b) {
        return std::tie(a.block, a.order) < std::tie(b.block, b.order);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
