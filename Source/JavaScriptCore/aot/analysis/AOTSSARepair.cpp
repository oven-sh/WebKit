/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSSARepair.h"

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <wtf/SSACalculator.h>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class SSARepairAdapter {
public:
    using CFG = Typed::CFG;
    using Value = ValueID;

    explicit SSARepairAdapter(FunctionAnalyses& analyses)
        : m_analyses(analyses)
    {
    }

    CFG& cfg() const { return m_analyses.cfg(); }
    Dominators& dominators() { return m_analyses.dominators(); }

    static void dumpValue(PrintStream& out, ValueID value) { out.print("v", value.index()); }

private:
    FunctionAnalyses& m_analyses;
};

} // anonymous namespace

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

static std::optional<OperandKind> kindOfOperandNamedBy(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex);
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operandKind(use.operandIndex);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

SSARepair::SSARepair(IRBuilder& builder, FunctionAnalyses& analyses)
    : m_builder(builder)
    , m_analyses(analyses)
{
    RELEASE_ASSERT(&builder.function() == &analyses.function());
}

SSARepair::~SSARepair()
{
    setIsPending(false);
}

void SSARepair::setIsPending(bool isPending)
{
    if (std::exchange(m_isPending, isPending) == isPending)
        return;
    if (isPending)
        m_analyses.didBeginRepair();
    else
        m_analyses.didEndRepair();
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
    if (m_variables[variable].values.size() > 1)
        setIsPending(true);
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
    auto endRepair = makeScopeExit([&] {
        setIsPending(false);
    });
    Origin savedOrigin = m_builder.origin();
    separateUsesAsGuards();
    m_didRun = true;
    if (m_variables.isEmpty())
        return;

    splitCoveredBlocksAfterDefinitions();

    const Function& function = m_analyses.function();
    Vector<Variable> constantVariables;
    Vector<Variable> mergedVariables;
    for (Variable variable = 0; variable < m_variables.size(); ++variable) {
        VariableData& data = m_variables[variable];
        const UseLists& useLists = m_analyses.useLists();
        for (ValueID value : data.values) {
            for (const Use& use : useLists.usesOf(value))
                data.uses.append(RecordedUse { use, useLists.pointOf(use), value, false });
        }
        if (data.uses.isEmpty())
            continue;
        AOT::sort(data.uses, [&](const RecordedUse& a, const RecordedUse& b) {
            return useLists.isBefore(a.use, b.use);
        });

        const CFG& cfg = m_analyses.cfg();
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
        if (data.definitions.isEmpty())
            continue;

        const Value& first = function.at(data.values.first());
        bool needsParameters = !std::ranges::all_of(data.values, [&](ValueID id) {
            const Value& value = function.at(id);
            return isRematerializable(value) && value.opcode == first.opcode && value.hasSamePayloadAs(first);
        });
        if (!needsParameters) {
            for (RecordedUse& recorded : data.uses) {
                recorded.isDominated = m_analyses.dominates(recorded.operand, recorded.use);
                needsParameters |= !recorded.isDominated && isUseAtBlockHead(recorded.use);
            }
        }
        (needsParameters ? mergedVariables : constantVariables).append(variable);
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
        const UseLists& useLists = m_analyses.useLists();
        const CFG& cfg = m_analyses.cfg();
        for (Variable variable = 0; variable < m_variables.size(); ++variable) {
            Vector<ValueID, 2> values = m_variables[variable].values;
            values.removeAllMatching([&](ValueID value) {
                return useLists.locationOf(value).placement == ValuePlacement::Detached;
            });
            auto namesGuardFromOtherBlock = [&](ValueID value, const Use& use) {
                return kindOfOperandNamedBy(function, use) == OperandKind::Guard && use.block != useLists.locationOf(value).block;
            };
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
        bool provesNothing = function.at(definition.value).opcode == Opcode::Unspecified;
        ValueID wrapper = m_builder.insertBefore(definition.pointAfter, provesNothing ? IRBuilder::make(Opcode::AllGuards) : IRBuilder::make(Opcode::AllGuards, { definition.value }));
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

    AOT::sort(cuts, [](const ProgramPoint& a, const ProgramPoint& b) {
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

void SSARepair::rematerializeConstants(std::span<const Variable> variables)
{
    Function& function = m_builder.function();
    const CFG& cfg = m_analyses.cfg();
    Vector<unsigned> constantAtTailStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<ValueID> constantAtTail(FillWith { }, cfg.numNodes(), ValueID());
    for (Variable variable : variables) {
        const VariableData& data = m_variables[variable];
        auto setConstantAtTail = [&](BlockID block, ValueID constant) {
            constantAtTailStamp[block.index()] = variable + 1;
            constantAtTail[block.index()] = constant;
        };
        auto findConstantAtHead = [&](BlockID block) {
            const Dominators& dominators = m_analyses.dominators();
            Vector<BlockID, 16> visitedDominators;
            ValueID constant;
            for (BlockID dominator = dominators.idom(block); dominator; dominator = dominators.idom(dominator)) {
                if (constantAtTailStamp[dominator.index()] == variable + 1 && dominators.terminatorDominates(dominator, block)) {
                    constant = constantAtTail[dominator.index()];
                    break;
                }
                visitedDominators.append(dominator);
            }
            if (cfg.hasHandlers())
                return constant;
            for (BlockID dominator : visitedDominators)
                setConstantAtTail(dominator, constant);
            return constant;
        };
        for (const Definition& definition : data.definitions)
            setConstantAtTail(definition.block, definition.value);

        BlockID block;
        ValueID constant;
        for (const RecordedUse& recorded : data.uses) {
            if (recorded.isDominated)
                continue;
            if (recorded.use.block != block) {
                block = recorded.use.block;
                constant = findLocalDefinition(data, recorded);
                if (!constant)
                    constant = findConstantAtHead(block);
                if (!constant) {
                    constant = m_builder.addValue(Value(function.at(data.values.first())));
                    m_builder.didAddDetachedValue(constant);
                    m_builder.m_insertions.append(IRBuilder::QueuedInsertion { recorded.point, constant });
                    setConstantAtTail(block, constant);
                }
            }
            replaceUse(function, recorded.use, constant);
            m_analyses.didChangeBlock(recorded.use.block);
        }
    }
}

void SSARepair::insertParameters(std::span<const Variable> variables)
{
    using Calculator = WTF::SSACalculator<SSARepairAdapter>;
    Function& function = m_builder.function();
    const CFG& cfg = m_analyses.cfg();
    Vector<unsigned> liveAtHeadStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<unsigned> hasDefinitionStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<uint32_t> knownNumberOfParametersForEdgeResults(FillWith { }, cfg.numNodes(), std::numeric_limits<uint32_t>::max());
    auto numberOfParametersForEdgeResults = [&](BlockID block) {
        uint32_t& number = knownNumberOfParametersForEdgeResults[block.index()];
        if (number == std::numeric_limits<uint32_t>::max())
            number = m_builder.numberOfParametersForEdgeResults(block);
        return number;
    };

    Calculator calculator(m_analyses);
    for (Variable variable : variables) {
        Calculator::Variable* calculatorVariable = calculator.newVariable();
        for (const Definition& definition : m_variables[variable].definitions)
            calculator.newDef(calculatorVariable, definition.block, definition.value);
    }

    auto computeLiveness = [&](const VariableData& data, unsigned stamp) {
        for (const Definition& definition : data.definitions)
            hasDefinitionStamp[definition.block.index()] = stamp;

        Vector<BlockID, 16> worklist;
        auto mark = [&](BlockID block) {
            if (std::exchange(liveAtHeadStamp[block.index()], stamp) != stamp)
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
                    if (hasDefinitionStamp[edge.from.index()] != stamp)
                        mark(edge.from);
                    break;
                case EdgeKind::Handler:
                    if (!findLastDefinitionBefore(data, edge.from, firstValueOrder))
                        mark(edge.from);
                    break;
                }
            }
        }
    };
    auto computeParameterType = [&](VariableData& data) {
        const Program& program = m_analyses.context().program;
        const Value& first = function.at(data.values.first());
        data.type = first.type;
        TypeID expected = first.expectedOrType().predicted();
        for (ValueID id : data.values) {
            const Value& value = function.at(id);
            RELEASE_ASSERT(!numberOfResultsOf(program, function, value));
            data.type = m_builder.m_types.unionOf(data.type, value.type);
            expected = m_builder.m_types.unionOf(expected, value.expectedOrType().predicted());
        }
        data.expected = expected == data.type ? TypeID() : expected;
    };

    std::optional<Variable> livenessVariable;
    calculator.computePhis([&](Calculator::Variable* calculatorVariable, BlockID block) -> ValueID {
        Variable variable = variables[calculatorVariable->index()];
        VariableData& data = m_variables[variable];
        if (livenessVariable != variable) {
            computeLiveness(data, variable + 1);
            livenessVariable = variable;
        }
        if (liveAtHeadStamp[block.index()] != variable + 1)
            return ValueID();
        if (!data.type)
            computeParameterType(data);

        m_builder.setOrigin(function.at(data.values.first()).origin());
        ValueID parameter = m_builder.addParameter(block, data.type, numberOfParametersForEdgeResults(block));
        function.edit(parameter, ValueEdit::Type).setExpected(ExpectedType(data.expected));
        data.definitions.append(Definition { block, headOrder, parameter });
        return parameter;
    });

    for (Variable variable : variables)
        sortDefinitions(m_variables[variable]);

    for (unsigned index = 0; index < variables.size(); ++index) {
        const VariableData& data = m_variables[variables[index]];
        for (const RecordedUse& recorded : data.uses) {
            const Use& use = recorded.use;
            ValueID definition = findLocalDefinition(data, recorded);
            if (!definition) {
                Calculator::Def* reachingDefinition = calculator.reachingDefAtHead(use.block, calculator.variable(index));
                RELEASE_ASSERT(reachingDefinition);
                definition = reachingDefinition->value();
            }
            if (definition == recorded.operand)
                continue;
            if (std::optional<OperandKind> kind = kindOfOperandNamedBy(function, use); kind && mustBeDefinition(*kind))
                RELEASE_ASSERT(function.at(definition).opcode != Opcode::Parameter);
            replaceUse(function, use, definition);
            m_analyses.didChangeBlock(use.block);
        }
    }

    auto reachingDefinitionOnEdge = [&](Calculator::Variable* calculatorVariable, Edge edge) -> ValueID {
        Calculator::Def* reachingDefinition = nullptr;
        switch (edge.kind) {
        case EdgeKind::Normal:
            reachingDefinition = calculator.reachingDefAtTail(edge.from, calculatorVariable);
            break;
        case EdgeKind::Handler:
            if (const Definition* definition = findLastDefinitionBefore(m_variables[variables[calculatorVariable->index()]], edge.from, firstValueOrder))
                return definition->value;
            reachingDefinition = calculator.reachingDefAtHead(edge.from, calculatorVariable);
            break;
        }
        RELEASE_ASSERT(reachingDefinition);
        return reachingDefinition->value();
    };
    for (BlockID block : cfg.reversePostOrder()) {
        const Vector<Calculator::Def*>& phis = calculator.phisForBlock(block);
        if (phis.isEmpty())
            continue;
        for (Edge edge : cfg.incomingEdges(block)) {
            for (Calculator::Def* phi : phis)
                m_builder.appendArgument(edge, reachingDefinitionOnEdge(phi->variable(), edge), numberOfParametersForEdgeResults(block));
        }
    }
    for (Edge edge : m_analyses.edgesFromUnreachableBlocks()) {
        BlockID target = targetOf(function, edge).block;
        for (Calculator::Def* phi : calculator.phisForBlock(target))
            m_builder.appendArgument(edge, m_variables[variables[phi->variable()->index()]].values.first(), numberOfParametersForEdgeResults(target));
    }
}

ValueID SSARepair::findLocalDefinition(const VariableData& data, const RecordedUse& recorded)
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
    AOT::stableSort(data.definitions, [](const Definition& a, const Definition& b) {
        return std::tie(a.block, a.order) < std::tie(b.block, b.order);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
