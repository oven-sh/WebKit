/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSSARepair.h"

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// In most functions the frontiers of all blocks together are about as many as the blocks, and one walk up from the predecessors of each join finds them all. In a deep nest of
// handlers a block has every handler around it in its frontier: there the frontier of a block is found when it is asked for, from the edges that leave what the block dominates.
class DominanceFrontiers {
public:
    DominanceFrontiers(const CFG& cfg, const Dominators& dominators)
        : m_cfg(cfg)
        , m_dominators(dominators)
    {
        AOT_THRESHOLD(size_t, mostPairsForEachBlock, 8);
        unsigned numberOfBlocks = cfg.numNodes();
        Vector<std::pair<BlockID, BlockID>> pairs;
        m_lastJoin.fill(noBlock, numberOfBlocks);
        m_first.fill(0, numberOfBlocks + 1);
        for (unsigned index = 0; index < numberOfBlocks; ++index) {
            BlockID join = cfg.node(index);
            if (!join || cfg.predecessors(join).size() < 2)
                continue;
            BlockID dominator = dominators.idom(join);
            for (BlockID predecessor : cfg.predecessors(join)) {
                for (BlockID runner = predecessor; runner && runner != dominator; runner = dominators.idom(runner)) {
                    if (std::exchange(m_lastJoin[runner.index()], index) == index)
                        break;
                    pairs.append({ runner, join });
                    ++m_first[runner.index() + 1];
                }
            }
            if (pairs.size() > mostPairsForEachBlock * numberOfBlocks) {
                findFrontiersWhenAsked();
                return;
            }
        }
        for (unsigned index = 0; index < numberOfBlocks; ++index)
            m_first[index + 1] += m_first[index];
        Vector<uint32_t> next(m_first.span().first(numberOfBlocks));
        m_joins.grow(pairs.size());
        for (auto [runner, join] : pairs)
            m_joins[next[runner.index()]++] = join;
    }

    std::span<const BlockID> of(BlockID block)
    {
        if (!m_blockWithNumber.isEmpty())
            return findFrontierOf(block);
        uint32_t begin = m_first[block.index()];
        return m_joins.span().subspan(begin, m_first[block.index() + 1] - begin);
    }

private:
    static constexpr uint32_t noBlock = std::numeric_limits<uint32_t>::max();

    void findFrontiersWhenAsked()
    {
        m_lastJoin.fill(noBlock);
        m_first.fill(noBlock);
        m_blockWithNumber.fill(BlockID(), m_cfg.numNodes());
        for (BlockID block : m_cfg.reversePostOrder())
            m_blockWithNumber[m_dominators.numberOf(block)] = block;
    }

    std::span<const BlockID> findFrontierOf(BlockID block)
    {
        if (!m_cfg.isReachable(block))
            return { };
        if (m_first[block.index()] == noBlock) {
            m_first[block.index()] = m_ranges.size();
            uint32_t begin = m_joins.size();
            for (uint32_t number = m_dominators.numberOf(block); number <= m_dominators.lastNumberBelow(block); ++number) {
                for (BlockID successor : m_cfg.successors(m_blockWithNumber[number])) {
                    if (m_dominators.strictlyDominates(block, successor) || m_cfg.predecessors(successor).size() < 2 || std::exchange(m_lastJoin[successor.index()], block.index()) == block.index())
                        continue;
                    m_joins.append(successor);
                }
            }
            AOT::sort(m_joins.mutableSpan().subspan(begin));
            m_ranges.append({ begin, static_cast<uint32_t>(m_joins.size()) });
        }
        auto [begin, end] = m_ranges[m_first[block.index()]];
        return m_joins.span().subspan(begin, end - begin);
    }

    const CFG& m_cfg;
    const Dominators& m_dominators;
    Vector<uint32_t> m_first;
    Vector<BlockID> m_joins;
    Vector<uint32_t> m_lastJoin;
    Vector<BlockID> m_blockWithNumber;
    Vector<std::pair<uint32_t, uint32_t>> m_ranges;
};

// The definition of one variable that reaches a place stands in the nearest block above the place in the dominator tree. The blocks that define and the places asked about
// are walked together in the order of the tree, so an answer costs the same however far above its place it lies.
class NearestDefinitions {
public:
    static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();

    explicit NearestDefinitions(const CFG& cfg, const Dominators& dominators)
        : m_cfg(cfg)
        , m_dominators(dominators)
    {
        RELEASE_ASSERT(cfg.numNodes() < 1u << (64 - numberShift));
    }

    void clear()
    {
        m_events.shrink(0);
        m_definitions.shrink(0);
        m_answers.shrink(0);
    }

    void defineAtTail(BlockID block, uint32_t definition)
    {
        if (!m_cfg.isReachable(block))
            return;
        addEvent(block, Kind::Definition, m_definitions.size());
        m_definitions.append({ m_dominators.lastNumberBelow(block), definition });
    }

    uint32_t askAtHead(BlockID block) { return ask(block, Kind::QuestionAtHead); }
    uint32_t askAtTail(BlockID block) { return ask(block, Kind::QuestionAtTail); }

    void solve()
    {
        AOT::sort(m_events);
        m_stack.shrink(0);
        for (uint64_t event : m_events) {
            uint32_t number = static_cast<uint32_t>(event >> numberShift);
            while (!m_stack.isEmpty() && m_stack.last().lastNumberBelow < number)
                m_stack.removeLast();
            uint32_t index = static_cast<uint32_t>(event);
            if (static_cast<Kind>((event >> kindShift) & 3) == Kind::Definition)
                m_stack.append(m_definitions[index]);
            else if (!m_stack.isEmpty())
                m_answers[index] = m_stack.last().definition;
        }
    }

    uint32_t answerTo(uint32_t question) const { return m_answers[question]; }

private:
    enum class Kind : uint8_t { QuestionAtHead, Definition, QuestionAtTail };

    struct DefinitionAtTail {
        uint32_t lastNumberBelow;
        uint32_t definition;
    };

    static constexpr unsigned kindShift = 32;
    static constexpr unsigned numberShift = 34;

    void addEvent(BlockID block, Kind kind, size_t index)
    {
        m_events.append(static_cast<uint64_t>(m_dominators.numberOf(block)) << numberShift | static_cast<uint64_t>(kind) << kindShift | static_cast<uint32_t>(index));
    }

    uint32_t ask(BlockID block, Kind kind)
    {
        RELEASE_ASSERT(m_cfg.isReachable(block));
        uint32_t question = m_answers.size();
        addEvent(block, kind, question);
        m_answers.append(none);
        return question;
    }

    const CFG& m_cfg;
    const Dominators& m_dominators;
    Vector<uint64_t> m_events;
    Vector<DefinitionAtTail> m_definitions;
    Vector<DefinitionAtTail, 16> m_stack;
    Vector<uint32_t> m_answers;
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

    // A definition that stands for "no use is reached from here" gives the parameters it gets to its type, and a use is legal where it stood.
    bool hasUnspecifiedDefinition = std::ranges::any_of(m_variables, [&](const VariableData& data) {
        return data.type && m_builder.m_types.at(data.type).mayBeUnspecified();
    });
    if (hasUnspecifiedDefinition)
        nameFactsWhereUnspecifiedValuesAreUsed(m_builder, m_analyses);
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
    Function& function = m_builder.function();
    const CFG& cfg = m_analyses.cfg();
    const Dominators& dominators = m_analyses.dominators();
    Vector<uint32_t> knownNumberOfParametersForEdgeResults(FillWith { }, cfg.numNodes(), std::numeric_limits<uint32_t>::max());
    auto numberOfParametersForEdgeResults = [&](BlockID block) {
        uint32_t& number = knownNumberOfParametersForEdgeResults[block.index()];
        if (number == std::numeric_limits<uint32_t>::max())
            number = m_builder.numberOfParametersForEdgeResults(block);
        return number;
    };

    DominanceFrontiers frontiers(cfg, dominators);
    NearestDefinitions nearest(cfg, dominators);
    auto forEachBlockWithDefinitions = [&](const VariableData& data, const auto& visitLastDefinitionInBlock) {
        for (size_t index = 0; index < data.definitions.size(); ++index) {
            if (index + 1 == data.definitions.size() || data.definitions[index + 1].block != data.definitions[index].block)
                visitLastDefinitionInBlock(data.definitions[index]);
        }
    };

    // A join in the iterated dominance frontier of the definitions gets a parameter if the variable is live at its head. There are two ways to find out, and each is
    // the cheap one where the other costs a walk over the function for each variable: to walk back from the uses through every block in which the variable is live, and
    // to take every such join for a definition and see which of them reach a use. They take turns until one has ended.
    Vector<unsigned> hasDefinitionStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<unsigned> liveAtHeadStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<BlockID, 16> liveBlocksToVisit;
    auto beginWalkFromUses = [&](const VariableData& data, unsigned stamp) {
        liveBlocksToVisit.shrink(0);
        for (const RecordedUse& recorded : data.uses) {
            if (!findLocalDefinition(data, recorded) && std::exchange(liveAtHeadStamp[recorded.use.block.index()], stamp) != stamp)
                liveBlocksToVisit.append(recorded.use.block);
        }
    };
    auto continueWalkFromUses = [&](const VariableData& data, unsigned stamp, size_t steps) {
        auto mark = [&](BlockID block) {
            if (std::exchange(liveAtHeadStamp[block.index()], stamp) != stamp)
                liveBlocksToVisit.append(block);
        };
        while (!liveBlocksToVisit.isEmpty()) {
            if (!steps)
                return false;
            auto incomingEdges = cfg.incomingEdges(liveBlocksToVisit.takeLast());
            steps -= std::min(steps, incomingEdges.size() + 1);
            for (Edge edge : incomingEdges) {
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
        return true;
    };

    Vector<unsigned> isJoinStamp(FillWith { }, cfg.numNodes(), 0u);
    Vector<uint32_t> indexOfJoin(FillWith { }, cfg.numNodes(), 0u);
    Vector<BlockID> joins;
    Vector<BlockID, 16> definingBlocksToVisit;
    auto beginWalkOverFrontiers = [&](const VariableData& data) {
        joins.shrink(0);
        definingBlocksToVisit.shrink(0);
        forEachBlockWithDefinitions(data, [&](const Definition& definition) {
            definingBlocksToVisit.append(definition.block);
        });
    };
    auto continueWalkOverFrontiers = [&](unsigned stamp, size_t steps) {
        while (!definingBlocksToVisit.isEmpty()) {
            if (!steps)
                return false;
            auto frontier = frontiers.of(definingBlocksToVisit.takeLast());
            steps -= std::min(steps, frontier.size() + 1);
            for (BlockID join : frontier) {
                if (std::exchange(isJoinStamp[join.index()], stamp) == stamp)
                    continue;
                indexOfJoin[join.index()] = joins.size();
                joins.append(join);
                definingBlocksToVisit.append(join);
                steps -= std::min(steps, cfg.incomingEdges(join).size());
            }
        }
        return true;
    };

    struct Source {
        uint32_t definitionOrQuestion;
        bool isQuestion;
    };
    constexpr uint32_t writtenDefinition = NearestDefinitions::none - 1;
    BitVector joinReachesUse;
    Vector<uint32_t> questionsOfUses;
    Vector<Source> sourcesOfJoins;
    Vector<uint32_t> firstSourceOfJoin;
    Vector<uint32_t, 16> joinsToVisit;
    auto findJoinsThatReachUses = [&](const VariableData& data, unsigned stamp) {
        joinReachesUse.clearAll();
        joinReachesUse.ensureSize(joins.size());
        questionsOfUses.shrink(0);
        sourcesOfJoins.shrink(0);
        firstSourceOfJoin.shrink(0);
        joinsToVisit.shrink(0);
        nearest.clear();
        auto didReach = [&](uint32_t definition) {
            if (definition < writtenDefinition && !joinReachesUse.quickSet(definition))
                joinsToVisit.append(definition);
        };

        forEachBlockWithDefinitions(data, [&](const Definition& definition) {
            nearest.defineAtTail(definition.block, writtenDefinition);
        });
        for (uint32_t index = 0; index < joins.size(); ++index) {
            if (hasDefinitionStamp[joins[index].index()] != stamp)
                nearest.defineAtTail(joins[index], index);
        }
        auto sourceAtHead = [&](BlockID block) -> Source {
            if (isJoinStamp[block.index()] == stamp)
                return { indexOfJoin[block.index()], false };
            return { nearest.askAtHead(block), true };
        };
        for (const RecordedUse& recorded : data.uses) {
            if (findLocalDefinition(data, recorded))
                continue;
            Source source = sourceAtHead(recorded.use.block);
            if (source.isQuestion)
                questionsOfUses.append(source.definitionOrQuestion);
            else
                didReach(source.definitionOrQuestion);
        }
        for (BlockID join : joins) {
            firstSourceOfJoin.append(sourcesOfJoins.size());
            for (Edge edge : cfg.incomingEdges(join)) {
                switch (edge.kind) {
                case EdgeKind::Normal:
                    sourcesOfJoins.append({ nearest.askAtTail(edge.from), true });
                    break;
                case EdgeKind::Handler:
                    sourcesOfJoins.append(findLastDefinitionBefore(data, edge.from, firstValueOrder) ? Source { writtenDefinition, false } : sourceAtHead(edge.from));
                    break;
                }
            }
        }
        firstSourceOfJoin.append(sourcesOfJoins.size());
        nearest.solve();

        for (uint32_t question : questionsOfUses)
            didReach(nearest.answerTo(question));
        while (!joinsToVisit.isEmpty()) {
            uint32_t index = joinsToVisit.takeLast();
            for (const Source& source : sourcesOfJoins.span().subspan(firstSourceOfJoin[index], firstSourceOfJoin[index + 1] - firstSourceOfJoin[index]))
                didReach(source.isQuestion ? nearest.answerTo(source.definitionOrQuestion) : source.definitionOrQuestion);
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

    struct AddedParameter {
        BlockID block;
        uint32_t indexOfVariable;
        uint32_t firstArgument;
    };
    Vector<AddedParameter> addedParameters;
    Vector<uint32_t> firstAddedParameter;
    Vector<unsigned> wasVisitedStamp(FillWith { }, cfg.numNodes(), 0u);
    uint32_t numberOfArguments = 0;
    AOT_THRESHOLD(size_t, stepsOfFirstTurn, 64);
    for (uint32_t indexOfVariable = 0; indexOfVariable < variables.size(); ++indexOfVariable) {
        firstAddedParameter.append(addedParameters.size());
        VariableData& data = m_variables[variables[indexOfVariable]];
        unsigned stamp = indexOfVariable + 1;
        for (const Definition& definition : data.definitions)
            hasDefinitionStamp[definition.block.index()] = stamp;

        bool didWalkFromUses = false;
        beginWalkOverFrontiers(data);
        for (size_t steps = stepsOfFirstTurn; !continueWalkOverFrontiers(stamp, steps); steps *= 2) {
            if (steps == stepsOfFirstTurn)
                beginWalkFromUses(data, stamp);
            if ((didWalkFromUses = continueWalkFromUses(data, stamp, steps)))
                break;
        }
        if (!didWalkFromUses) {
            if (joins.isEmpty())
                continue;
            findJoinsThatReachUses(data, stamp);
        }
        auto needsParameter = [&](BlockID block) {
            if (didWalkFromUses)
                return liveAtHeadStamp[block.index()] == stamp;
            return joinReachesUse.quickGet(indexOfJoin[block.index()]);
        };

        definingBlocksToVisit.shrink(0);
        forEachBlockWithDefinitions(data, [&](const Definition& definition) {
            definingBlocksToVisit.append(definition.block);
        });
        while (!definingBlocksToVisit.isEmpty()) {
            for (BlockID block : frontiers.of(definingBlocksToVisit.takeLast())) {
                if (std::exchange(wasVisitedStamp[block.index()], stamp) == stamp || !needsParameter(block))
                    continue;
                if (!data.type)
                    computeParameterType(data);

                m_builder.setOrigin(function.at(data.values.first()).origin());
                ValueID parameter = m_builder.addParameter(block, data.type, numberOfParametersForEdgeResults(block));
                function.edit(parameter, ValueEdit::Type).setExpected(ExpectedType(data.expected));
                data.definitions.append(Definition { block, headOrder, parameter });
                addedParameters.append({ block, indexOfVariable, numberOfArguments });
                numberOfArguments += cfg.incomingEdges(block).size();
                definingBlocksToVisit.append(block);
            }
        }
    }
    firstAddedParameter.append(addedParameters.size());

    Vector<ValueID> arguments(FillWith { }, numberOfArguments, ValueID());
    Vector<std::pair<uint32_t, uint32_t>> questionsOfArguments;
    auto definitionThatAnswers = [&](uint32_t question) {
        uint32_t answer = nearest.answerTo(question);
        RELEASE_ASSERT(answer != NearestDefinitions::none);
        return ValueID(answer);
    };
    for (uint32_t indexOfVariable = 0; indexOfVariable < variables.size(); ++indexOfVariable) {
        VariableData& data = m_variables[variables[indexOfVariable]];
        sortDefinitions(data);
        nearest.clear();
        forEachBlockWithDefinitions(data, [&](const Definition& definition) {
            nearest.defineAtTail(definition.block, definition.value.index());
        });

        questionsOfUses.shrink(0);
        for (const RecordedUse& recorded : data.uses) {
            if (!findLocalDefinition(data, recorded))
                questionsOfUses.append(nearest.askAtHead(recorded.use.block));
        }
        questionsOfArguments.shrink(0);
        for (const AddedParameter& added : addedParameters.span().subspan(firstAddedParameter[indexOfVariable], firstAddedParameter[indexOfVariable + 1] - firstAddedParameter[indexOfVariable])) {
            uint32_t argument = added.firstArgument;
            for (Edge edge : cfg.incomingEdges(added.block)) {
                const Definition* definitionAtHead = edge.kind == EdgeKind::Handler ? findLastDefinitionBefore(data, edge.from, firstValueOrder) : nullptr;
                if (definitionAtHead)
                    arguments[argument] = definitionAtHead->value;
                else
                    questionsOfArguments.append({ argument, edge.kind == EdgeKind::Handler ? nearest.askAtHead(edge.from) : nearest.askAtTail(edge.from) });
                ++argument;
            }
        }
        nearest.solve();

        size_t nextQuestion = 0;
        for (const RecordedUse& recorded : data.uses) {
            const Use& use = recorded.use;
            ValueID definition = findLocalDefinition(data, recorded);
            if (!definition)
                definition = definitionThatAnswers(questionsOfUses[nextQuestion++]);
            if (definition == recorded.operand)
                continue;
            if (std::optional<OperandKind> kind = kindOfOperandNamedBy(function, use); kind && mustBeDefinition(*kind))
                RELEASE_ASSERT(function.at(definition).opcode != Opcode::Parameter);
            replaceUse(function, use, definition);
            m_analyses.didChangeBlock(use.block);
        }
        for (auto [argument, question] : questionsOfArguments)
            arguments[argument] = definitionThatAnswers(question);
    }

    Vector<uint32_t> firstAddedInBlock(FillWith { }, cfg.numNodes() + 1, 0u);
    for (const AddedParameter& added : addedParameters)
        ++firstAddedInBlock[added.block.index() + 1];
    for (unsigned index = 0; index < cfg.numNodes(); ++index)
        firstAddedInBlock[index + 1] += firstAddedInBlock[index];
    Vector<uint32_t> nextAddedInBlock(firstAddedInBlock.span().first(cfg.numNodes()));
    Vector<uint32_t> addedByBlock(FillWith { }, addedParameters.size(), 0u);
    for (uint32_t index = 0; index < addedParameters.size(); ++index)
        addedByBlock[nextAddedInBlock[addedParameters[index].block.index()]++] = index;
    auto addedIn = [&](BlockID block) {
        uint32_t begin = firstAddedInBlock[block.index()];
        return addedByBlock.span().subspan(begin, firstAddedInBlock[block.index() + 1] - begin);
    };

    for (BlockID block : cfg.reversePostOrder()) {
        auto added = addedIn(block);
        if (added.empty())
            continue;
        uint32_t indexOfEdge = 0;
        for (Edge edge : cfg.incomingEdges(block)) {
            for (uint32_t index : added)
                m_builder.appendArgument(edge, arguments[addedParameters[index].firstArgument + indexOfEdge], numberOfParametersForEdgeResults(block));
            ++indexOfEdge;
        }
    }
    for (Edge edge : m_analyses.edgesFromUnreachableBlocks()) {
        BlockID target = targetOf(function, edge).block;
        for (uint32_t index : addedIn(target))
            m_builder.appendArgument(edge, m_variables[variables[addedParameters[index].indexOfVariable]].values.first(), numberOfParametersForEdgeResults(target));
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
