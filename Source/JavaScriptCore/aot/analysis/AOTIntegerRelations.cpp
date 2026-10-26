/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIntegerRelations.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet edgeComparisons = relationalOperators | OpcodeSet { Opcode::StrictEq };
static constexpr OpcodeSet wholeBinaryOperators { Opcode::Add, Opcode::Sub, Opcode::Mul };
static constexpr OpcodeSet wholeUnaryOperators { Opcode::Increment, Opcode::Decrement };

struct ConstantStep {
    ValueID operand;
    int64_t offset { 0 };
};

static std::optional<int64_t> exactValueOf(CheckedInt64 sum)
{
    return sum.hasOverflowed() ? std::nullopt : std::optional<int64_t>(sum.value());
}

static std::optional<int64_t> constantOf(const Type& type)
{
    return type.isInteger() && type.integers.minimum == type.integers.maximum ? std::optional<int64_t>(type.integers.minimum) : std::nullopt;
}

static bool isAlwaysAtMost(Range left, Range right, int64_t offset)
{
    std::optional<int64_t> lowestBound = exactValueOf(CheckedInt64(right.minimum) + offset);
    return lowestBound && left.maximum <= *lowestBound;
}

static bool isInScope(FunctionAnalyses& analyses, BlockID scope, BlockID at)
{
    return scope == at || analyses.dominators().terminatorDominates(scope, at);
}

static std::optional<ValueID> findValueOnEdge(const Function& function, const auto& assumptions, Edge edge, uint32_t parameterIndex)
{
    bool mayLeaveBeforeAssumptionHolds = edge.isHandlerEdge() && std::ranges::any_of(assumptions, [&](const auto& assumption) {
        return assumption.scope == edge.from;
    });
    if (mayLeaveBeforeAssumptionHolds)
        return std::nullopt;

    ValueID passed = valuePassedOn(function, edge, parameterIndex);
    return passed ? std::optional(passed) : std::nullopt;
}

static std::optional<ConstantStep> constantStepOf(const Program& program, const Function& function, const Value& value)
{
    auto isNumber = [&](ValueID id) {
        return program.typeOf(function, id).isNumber();
    };
    auto constantOperand = [&](ValueID id) {
        return constantOf(program.typeOf(function, id));
    };

    if (value.opcode == Opcode::Increment || value.opcode == Opcode::Decrement) {
        if (!isNumber(value.operand(0)))
            return std::nullopt;
        return ConstantStep { value.operand(0), value.opcode == Opcode::Increment ? 1 : -1 };
    }
    if (value.opcode == Opcode::Add) {
        if (auto constant = constantOperand(value.operand(1)); constant && isNumber(value.operand(0)))
            return ConstantStep { value.operand(0), *constant };
        if (auto constant = constantOperand(value.operand(0)); constant && isNumber(value.operand(1)))
            return ConstantStep { value.operand(1), *constant };
        return std::nullopt;
    }
    if (value.opcode == Opcode::Sub) {
        if (auto constant = constantOperand(value.operand(1)); constant && isNumber(value.operand(0)))
            return ConstantStep { value.operand(0), -*constant };
    }
    return std::nullopt;
}

IntegerRelations::IntegerRelations(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_program(analyses.context().program)
    , m_function(analyses.function())
{
}

IntegerRelations::Answer IntegerRelations::isAtMost(ValueID left, ValueID right, int64_t offset, BlockID at)
{
    m_steps = 0;
    const Type& leftType = typeOf(left);
    const Type& rightType = typeOf(right);
    if (!isNumberOrAssumed(left, at) || !isNumberOrAssumed(right, at) || !m_analyses.cfg().isReachable(at))
        return Answer::Unknown;
    if (leftType.isInteger() && rightType.isInteger() && isAlwaysAtMost(leftType.integers, rightType.integers, offset))
        return Answer::Proven;

    Term leftTerm = normalize(left);
    if (proveTerms(leftTerm, normalize(right), offset, at))
        return Answer::Proven;

    Term rightTerm = normalize(right, at);
    return answerOf(rightTerm.usedAssumedSum && isKnownWhole(left, at) && proveTerms(leftTerm, rightTerm, offset, at));
}

IntegerRelations::Answer IntegerRelations::isAtLeast(ValueID id, int64_t minimum, BlockID at)
{
    m_steps = 0;
    const Type& type = typeOf(id);
    if (!type.isNumber() || !m_analyses.cfg().isReachable(at))
        return Answer::Unknown;
    if (type.isInteger() && type.integers.minimum >= minimum)
        return Answer::Proven;
    LowerBoundGoals goals;
    return answerOf(proveValueAtLeast(id, minimum, at, goals));
}

IntegerRelations::Answer IntegerRelations::isAtMost(ValueID id, int64_t maximum, BlockID at)
{
    m_steps = 0;
    const Type& type = typeOf(id);
    if (!type.isNumber() || !m_analyses.cfg().isReachable(at))
        return Answer::Unknown;
    if (type.isInteger() && type.integers.maximum <= maximum)
        return Answer::Proven;
    return answerOf(proveTerms(normalize(id), Term { }, maximum, at));
}

IntegerRelations::Answer IntegerRelations::isWholeNumber(ValueID id, BlockID at)
{
    m_steps = 0;
    AssumedWholeParameters assumed;
    return answerOf(m_analyses.cfg().isReachable(at) && proveWhole(id, at, assumed));
}

void IntegerRelations::assumeRange(ValueID id, Range range, BlockID scope)
{
    ASSERT(range.isSubsetOf(Range::exact()));
    Term term = normalize(id);
    if (!term.base)
        return;
    std::optional<int64_t> minimum = exactValueOf(CheckedInt64(range.minimum) - term.offset);
    std::optional<int64_t> maximum = exactValueOf(CheckedInt64(range.maximum) - term.offset);
    if (!minimum || !maximum)
        return;
    m_assumptions.append({ term.base, { *minimum, *maximum }, scope });
}

std::optional<Range> IntegerRelations::assumedRangeOf(ValueID base, BlockID at) const
{
    std::optional<Range> range;
    for (const Assumption& assumption : m_assumptions) {
        if (assumption.value != base || !isInScope(m_analyses, assumption.scope, at))
            continue;
        range = range ? range->intersectWith(assumption.range) : assumption.range;
    }
    return range;
}

bool IntegerRelations::isIdentityConversion(const Value& value) const
{
    if (isCast(value.opcode))
        return true;
    return (value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric) && typeOf(value.operand(0)).isNumber();
}

bool IntegerRelations::takeStep()
{
    BuildContext& build = m_analyses.context().build;
    if (++m_steps <= build.limit(LimitID::MaxIntegerRelationSteps))
        return true;
    if (!m_didReportLimit) {
        m_didReportLimit = true;
        build.gaveUp(LimitID::MaxIntegerRelationSteps, m_analyses.subject());
    }
    return false;
}

IntegerRelations::Term IntegerRelations::normalize(ValueID id, std::optional<BlockID> placeOfAssumedSums) const
{
    Term term { id };
    for (;;) {
        term.base = m_analyses.identityOf(term.base);
        const Value& value = m_function.at(term.base);
        if (isIdentityConversion(value)) {
            term.base = value.operand(0);
            continue;
        }

        const Type& type = typeOf(term.base);
        if (std::optional<int64_t> constant = constantOf(type)) {
            std::optional<int64_t> sum = exactValueOf(CheckedInt64(term.offset) + *constant);
            if (!sum)
                return term;
            return { ValueID(), *sum, term.usedAssumedSum };
        }

        std::optional<ConstantStep> step = constantStepOf(m_program, m_function, value);
        if (!step)
            return term;
        bool isAssumedSum = !type.isInteger();
        if (isAssumedSum) {
            if (!placeOfAssumedSums)
                return term;
            std::optional<Range> assumedRange = assumedRangeOf(term.base, *placeOfAssumedSums);
            if (!assumedRange || assumedRange->maximum >= maxExactInteger)
                return term;
        }
        std::optional<int64_t> sum = exactValueOf(CheckedInt64(term.offset) + step->offset);
        if (!sum)
            return term;
        term = { step->operand, *sum, term.usedAssumedSum || isAssumedSum };
    }
}

std::optional<Range> IntegerRelations::rangeOf(ValueID base, BlockID at) const
{
    if (!base)
        return Range::one(0);

    std::optional<Range> range = assumedRangeOf(base, at);
    if (const Type& type = typeOf(base); type.isInteger())
        range = range ? range->intersectWith(type.integers) : type.integers;
    return range;
}

void IntegerRelations::appendRelationship(Vector<Relationship, 2>& relationships, Term left, Term right, int64_t offset)
{
    if (std::optional<int64_t> folded = exactValueOf(CheckedInt64(offset) + right.offset - left.offset))
        relationships.append({ left.base, right.base, *folded });
}

Vector<IntegerRelations::Relationship, 2> IntegerRelations::relationshipsOfEdge(const Value& comparison, EdgeSide side, ValueID base, BlockID at)
{
    Vector<Relationship, 2> relationships;
    bool isTaken = side == EdgeSide::Taken;
    ValueID left = comparison.operand(0);
    ValueID right = comparison.operand(1);
    if (!isNumberOrAssumed(left, at) || !isNumberOrAssumed(right, at))
        return relationships;

    bool isMirrored = comparison.opcode == Opcode::Greater || comparison.opcode == Opcode::GreaterEq;
    if (isMirrored)
        std::swap(left, right);
    Term leftTerm = normalize(left);
    Term rightTerm = normalize(right);
    if (leftTerm.base != base && rightTerm.base != base)
        return relationships;

    if (comparison.opcode == Opcode::StrictEq) {
        if (isTaken) {
            appendRelationship(relationships, leftTerm, rightTerm, 0);
            appendRelationship(relationships, rightTerm, leftTerm, 0);
        }
        return relationships;
    }

    bool isStrict = comparison.opcode == Opcode::Less || comparison.opcode == Opcode::Greater;
    if (isTaken) {
        appendRelationship(relationships, leftTerm, rightTerm, isStrict && isKnownWhole(left, at) && isKnownWhole(right, at) ? -1 : 0);
        return relationships;
    }
    if (isIntegerOrAssumed(left, at) && isIntegerOrAssumed(right, at))
        appendRelationship(relationships, rightTerm, leftTerm, isStrict ? 0 : -1);
    return relationships;
}

bool IntegerRelations::isEnteredOverEdgeOfComparison(BlockID block) const
{
    std::optional<Edge> edge = m_analyses.cfg().singleIncomingEdge(block);
    if (!edge || edge->isHandlerEdge())
        return false;
    const Terminator& terminator = m_function.at(edge->from).terminator;
    return terminator.isConditionalBranch() && edgeComparisons.contains(m_function.at(terminator.conditionOperand()).opcode);
}

const Value& IntegerRelations::comparisonOnEdgeInto(BlockID block) const
{
    return m_function.at(m_function.at(m_analyses.cfg().singleIncomingEdge(block)->from).terminator.conditionOperand());
}

BlockID IntegerRelations::nextBlockBehindComparison(BlockID block)
{
    constexpr uint32_t notAsked = UINT32_MAX;
    constexpr uint32_t noBlock = UINT32_MAX - 1;
    if (m_nextBlockBehindComparison.size() != m_function.blocks.size())
        m_nextBlockBehindComparison.fill(notAsked, m_function.blocks.size());
    const Dominators& dominators = m_analyses.dominators();
    Vector<BlockID, 16> waiting;
    uint32_t answer = noBlock;
    for (BlockID current = block; current; current = dominators.idom(current)) {
        if (m_nextBlockBehindComparison[current.index()] != notAsked) {
            answer = m_nextBlockBehindComparison[current.index()];
            break;
        }
        waiting.append(current);
        BlockID above = dominators.idom(current);
        if (above && isEnteredOverEdgeOfComparison(above)) {
            answer = above.index();
            break;
        }
    }
    for (BlockID current : waiting)
        m_nextBlockBehindComparison[current.index()] = answer;
    uint32_t next = m_nextBlockBehindComparison[block.index()];
    return next == noBlock ? BlockID() : BlockID(next);
}

bool IntegerRelations::comparisonOnEdgeIntoIsAbout(BlockID block, ValueID base)
{
    if (m_comparedBases.size() != m_function.blocks.size()) {
        m_comparedBases.shrink(0);
        m_comparedBases.grow(m_function.blocks.size());
    }
    ComparedBases& bases = m_comparedBases[block.index()];
    if (!bases.isKnown) {
        const Value& comparison = comparisonOnEdgeInto(block);
        bases = { normalize(comparison.operand(0)).base, normalize(comparison.operand(1)).base, true };
    }
    return bases.left == base || bases.right == base;
}

void IntegerRelations::indexComparisonsByBase()
{
    const CFG& cfg = m_analyses.cfg();
    const Dominators& dominators = m_analyses.dominators();
    size_t numberOfBlocks = m_function.blocks.size();
    constexpr uint32_t notKnown = UINT32_MAX;
    m_depthInDominatorTree.fill(notKnown, numberOfBlocks);
    Vector<BlockID, 32> waiting;
    auto depthOf = [&](BlockID block) {
        uint32_t depth = 0;
        for (BlockID current = block; current; current = dominators.idom(current)) {
            if (m_depthInDominatorTree[current.index()] != notKnown) {
                depth = m_depthInDominatorTree[current.index()] + 1;
                break;
            }
            waiting.append(current);
        }
        while (!waiting.isEmpty())
            m_depthInDominatorTree[waiting.takeLast().index()] = depth++;
        return m_depthInDominatorTree[block.index()];
    };
    m_blocksBehindComparisonsAbout.clear();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        if (!cfg.isReachable(block))
            continue;
        depthOf(block);
        if (!isEnteredOverEdgeOfComparison(block))
            continue;
        comparisonOnEdgeIntoIsAbout(block, ValueID());
        const ComparedBases& bases = m_comparedBases[index];
        auto add = [&](ValueID base) {
            m_blocksBehindComparisonsAbout.add(base ? base.index() + 1 : 0, Vector<BlockID, 2>()).iterator->value.append(block);
        };
        add(bases.left);
        if (bases.right != bases.left)
            add(bases.right);
    }
    for (auto& entry : m_blocksBehindComparisonsAbout) {
        AOT::sort(entry.value, [&](BlockID first, BlockID second) {
            return std::pair { m_depthInDominatorTree[second.index()], second.index() } < std::pair { m_depthInDominatorTree[first.index()], first.index() };
        });
    }
    m_hasIndexOfComparisons = true;
}

template<typename Functor>
bool IntegerRelations::forEachDominatingRelationship(ValueID base, BlockID at, const Functor& functor)
{
    const CFG& cfg = m_analyses.cfg();
    const Dominators& dominators = m_analyses.dominators();
    BlockID definingBlock = base ? m_analyses.useLists().blockOf(base) : BlockID();
    if (definingBlock && !dominators.dominates(definingBlock, at))
        definingBlock = BlockID();
    auto above = [&](BlockID block) {
        BlockID next = block == definingBlock ? BlockID() : nextBlockBehindComparison(block);
        return next && definingBlock && !dominators.dominates(definingBlock, next) ? BlockID() : next;
    };
    auto holdsOnEdgeInto = [&](BlockID block) {
        EdgeSide side = cfg.singleIncomingEdge(block)->targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken;
        return std::ranges::any_of(relationshipsOfEdge(comparisonOnEdgeInto(block), side, base, at), functor);
    };
    if (m_hasIndexOfComparisons && m_depthInDominatorTree.size() != m_function.blocks.size())
        m_hasIndexOfComparisons = false;
    if (!m_hasIndexOfComparisons && m_numberOfBlocksWalked > m_function.blocks.size()) {
        m_numberOfBlocksWalked = 0;
        indexComparisonsByBase();
    }
    if (m_hasIndexOfComparisons) {
        auto found = m_blocksBehindComparisonsAbout.find(base ? base.index() + 1 : 0);
        if (found == m_blocksBehindComparisonsAbout.end())
            return false;
        Vector<BlockID, 16> blocks;
        for (BlockID block : found->value) {
            if (m_depthInDominatorTree[block.index()] > m_depthInDominatorTree[at.index()] || !dominators.dominates(block, at))
                continue;
            if (definingBlock && !dominators.dominates(definingBlock, block))
                break;
            blocks.append(block);
        }
        return std::ranges::any_of(blocks, holdsOnEdgeInto);
    }
    for (BlockID block = at; block; block = above(block)) {
        ++m_numberOfBlocksWalked;
        if (isEnteredOverEdgeOfComparison(block) && comparisonOnEdgeIntoIsAbout(block, base) && holdsOnEdgeInto(block))
            return true;
    }
    return false;
}

bool IntegerRelations::proveTerms(Term left, Term right, int64_t offset, BlockID at)
{
    std::optional<int64_t> goal = exactValueOf(CheckedInt64(offset) + right.offset - left.offset);
    UpperBoundGoals goals;
    return goal && prove(left.base, right.base, *goal, at, goals);
}

bool IntegerRelations::prove(ValueID left, ValueID right, int64_t offset, BlockID at, UpperBoundGoals& goals)
{
    if (!takeStep())
        return false;
    if (left == right && offset >= 0)
        return true;

    std::optional<Range> leftRange = rangeOf(left, at);
    std::optional<Range> rightRange = rangeOf(right, at);
    if (leftRange && rightRange && isAlwaysAtMost(*leftRange, *rightRange, offset))
        return true;

    bool isProvenByRelationship = forEachDominatingRelationship(left, at, [&](const Relationship& relationship) {
        if (relationship.left != left)
            return false;
        std::optional<int64_t> remaining = exactValueOf(CheckedInt64(offset) - relationship.offset);
        return remaining && prove(relationship.right, right, *remaining, at, goals);
    });
    return isProvenByRelationship || proveByInduction(left, right, offset, goals);
}

bool IntegerRelations::proveByInduction(ValueID left, ValueID right, int64_t offset, UpperBoundGoals& goals)
{
    if (!left)
        return false;
    const UseLists& useLists = m_analyses.useLists();
    const CFG& cfg = m_analyses.cfg();
    ValueLocation location = useLists.locationOf(left);
    if (location.placement != ValuePlacement::Parameter || location.block == cfg.root())
        return false;
    if (right && (useLists.blockOf(right) == location.block || !m_analyses.dominates(right, ProgramPoint::atHeadOf(location.block))))
        return false;

    bool isHypothesis = std::ranges::any_of(goals, [&](const Relationship& goal) {
        return goal.left == left && goal.right == right && goal.offset <= offset;
    });
    if (isHypothesis)
        return true;

    goals.append({ left, right, offset });
    bool holdsOnEveryEdge = std::ranges::all_of(cfg.incomingEdges(location.block), [&](Edge edge) {
        std::optional<ValueID> incoming = findValueOnEdge(m_function, m_assumptions, edge, location.index);
        if (!incoming)
            return false;
        Term argument = normalize(*incoming);
        std::optional<int64_t> remaining = exactValueOf(CheckedInt64(offset) - argument.offset);
        return remaining && prove(argument.base, right, *remaining, edge.from, goals);
    });
    goals.removeLast();
    return holdsOnEveryEdge;
}

bool IntegerRelations::proveValueAtLeast(ValueID id, int64_t minimum, BlockID at, LowerBoundGoals& goals)
{
    Term term = normalize(id);
    std::optional<int64_t> goal = exactValueOf(CheckedInt64(minimum) - term.offset);
    return goal && proveAtLeast(term.base, *goal, at, goals);
}

bool IntegerRelations::proveAtLeast(ValueID base, int64_t minimum, BlockID at, LowerBoundGoals& goals)
{
    if (!takeStep())
        return false;

    std::optional<Range> range = rangeOf(base, at);
    if (range && range->minimum >= minimum)
        return true;

    bool isProvenByRelationship = forEachDominatingRelationship(base, at, [&](const Relationship& relationship) {
        if (relationship.right != base)
            return false;
        std::optional<int64_t> remaining = exactValueOf(CheckedInt64(minimum) + relationship.offset);
        return remaining && proveAtLeast(relationship.left, *remaining, at, goals);
    });
    if (isProvenByRelationship)
        return true;
    if (!base)
        return false;

    const Value& value = m_function.at(base);
    bool isProvenByOperation = value.opcode == Opcode::Mul ? proveProductAtLeast(value, minimum, at, goals) : proveSumAtLeast(value, minimum, at, goals);
    return isProvenByOperation || proveParameterAtLeast(base, minimum, goals);
}

bool IntegerRelations::proveProductAtLeast(const Value& product, int64_t minimum, BlockID at, LowerBoundGoals& goals)
{
    ValueID left = product.operand(0);
    ValueID right = product.operand(1);
    auto isNonNegativeInteger = [&](ValueID id) {
        const Type& type = typeOf(id);
        return type.isInteger() && type.integers.minimum >= 0;
    };
    auto isPositive = [&](ValueID id) {
        return typeOf(id).isNumber() && proveValueAtLeast(id, 1, at, goals);
    };
    if (minimum <= 0 && isNonNegativeInteger(left) && isNonNegativeInteger(right))
        return true;
    return minimum <= 1 && isPositive(left) && isPositive(right);
}

bool IntegerRelations::proveSumAtLeast(const Value& value, int64_t minimum, BlockID at, LowerBoundGoals& goals)
{
    auto operandIsAtLeast = [&](ValueID id, int64_t bound) {
        return typeOf(id).isNumber() && proveValueAtLeast(id, bound, at, goals);
    };
    if (!Range::exact().contains(minimum))
        return false;

    if (value.opcode == Opcode::Increment) {
        ValueID operand = value.operand(0);
        return (minimum <= 1 && operandIsAtLeast(operand, 0)) || operandIsAtLeast(operand, minimum);
    }
    if (value.opcode == Opcode::Add) {
        ValueID left = value.operand(0);
        ValueID right = value.operand(1);
        return (operandIsAtLeast(left, 0) && operandIsAtLeast(right, minimum)) || (operandIsAtLeast(left, minimum) && operandIsAtLeast(right, 0));
    }
    return false;
}

bool IntegerRelations::proveParameterAtLeast(ValueID parameter, int64_t minimum, LowerBoundGoals& goals)
{
    const CFG& cfg = m_analyses.cfg();
    ValueLocation location = m_analyses.useLists().locationOf(parameter);
    if (location.placement != ValuePlacement::Parameter || location.block == cfg.root())
        return false;

    bool isHypothesis = std::ranges::any_of(goals, [&](const LowerBoundGoal& goal) {
        return goal.parameter == parameter && goal.minimum >= minimum;
    });
    if (isHypothesis)
        return true;

    goals.append({ parameter, minimum });
    bool holdsOnEveryEdge = std::ranges::all_of(cfg.incomingEdges(location.block), [&](Edge edge) {
        std::optional<ValueID> incoming = findValueOnEdge(m_function, m_assumptions, edge, location.index);
        return incoming && proveValueAtLeast(*incoming, minimum, edge.from, goals);
    });
    goals.removeLast();
    return holdsOnEveryEdge;
}

bool IntegerRelations::proveWhole(ValueID id, BlockID at, AssumedWholeParameters& assumed)
{
    if (!takeStep())
        return false;
    Term term = normalize(id);
    if (term.base && hasAssumption(term.base, at))
        return true;
    const Type& type = typeOf(id);
    if (!type.isNumber())
        return false;
    if (type.isNumberWithoutFraction() || !term.base)
        return true;

    const Value& value = m_function.at(id);
    if (isIdentityConversion(value) || wholeUnaryOperators.contains(value.opcode))
        return proveWhole(value.operand(0), at, assumed);
    if (wholeBinaryOperators.contains(value.opcode))
        return proveWhole(value.operand(0), at, assumed) && proveWhole(value.operand(1), at, assumed);

    const CFG& cfg = m_analyses.cfg();
    ValueLocation location = m_analyses.useLists().locationOf(id);
    if (location.placement != ValuePlacement::Parameter || location.block == cfg.root())
        return false;
    if (assumed.contains(id))
        return true;

    std::span<const Edge> incomingEdges = cfg.incomingEdges(location.block);
    if (incomingEdges.empty())
        return false;

    assumed.append(id);
    bool receivesOnlyWholeNumbers = std::ranges::all_of(incomingEdges, [&](Edge edge) {
        std::optional<ValueID> incoming = findValueOnEdge(m_function, m_assumptions, edge, location.index);
        return incoming && proveWhole(*incoming, edge.from, assumed);
    });
    assumed.removeLast();
    return receivesOnlyWholeNumbers;
}

bool IntegerRelations::isKnownWhole(ValueID id, BlockID at)
{
    AssumedWholeParameters assumed;
    return typeOf(id).isInteger() || proveWhole(id, at, assumed);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
