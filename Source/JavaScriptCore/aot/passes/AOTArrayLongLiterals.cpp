/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LongLiteral {
    ValueID value;
    ProgramPoint point;
};

struct ChainPoint {
    bool isEarlierThan(ChainPoint other) const
    {
        if (chainIndex != other.chainIndex)
            return chainIndex > other.chainIndex;
        return index < other.index;
    }

    uint32_t chainIndex { 0 };
    uint32_t index { 0 };
};

struct ElementStore {
    ProgramPoint point;
    uint32_t arrayIndex { 0 };
    ValueID element;
};

struct LiteralPlan {
    ValueID literal;
    uint32_t length { 0 };
    ProgramPoint allocation;
    Vector<std::pair<uint32_t, ValueID>> storesAfterAllocation;
    Vector<ElementStore> laterStores;
};

struct DominatorChain {
    bool isUsable(uint32_t chainIndex) const { return chainIndex < nearestUsablePoints.size() && nearestUsablePoints[chainIndex].chainIndex == chainIndex; }
    ProgramPoint programPointOf(ChainPoint point) const { return { blocks[point.chainIndex], point.index }; }

    ChainPoint moveToUsableBlock(ChainPoint point) const
    {
        if (point.chainIndex >= nearestUsablePoints.size() || isUsable(point.chainIndex))
            return point;
        return nearestUsablePoints[point.chainIndex];
    }

    // blocks[0] holds the literal and blocks[j + 1] is the immediate dominator of blocks[j], so a greater chain index is earlier.
    Vector<BlockID, 8> blocks;
    Vector<ChainPoint, 8> nearestUsablePoints;
};

bool isComputedElement(const Program& program, const Function& function, ValueID element)
{
    return !isRematerializable(function.at(element)) && !program.typeOf(function, element).isNumber();
}

void forEachElement(const Value& literal, NOESCAPE const Invocable<void(uint32_t, ValueID)> auto& functor)
{
    std::span<const uint32_t> holes = literal.holes();
    uint32_t arrayIndex = 0;
    for (ValueID element : literal.elementsOperands()) {
        for (; !holes.empty() && holes.front() == arrayIndex; holes = holes.subspan(1))
            ++arrayIndex;
        functor(arrayIndex++, element);
    }
}

Vector<LongLiteral, 4> findLongLiterals(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<LongLiteral, 4> literals;
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::NewArray)
                continue;
            uint64_t numberOfComputedElements = 0;
            for (ValueID element : value.elementsOperands())
                numberOfComputedElements += isComputedElement(context.program, function, element);
            if (context.exceeds(LimitID::MaxArrayLiteralBatch, numberOfComputedElements, values[index]))
                literals.append(LongLiteral { values[index], { BlockID(blockIndex), index } });
        }
    }
    return literals;
}

class LiteralPlanner {
    WTF_MAKE_NONCOPYABLE(LiteralPlanner);
public:
    explicit LiteralPlanner(FunctionPassContext&);

    LiteralPlan createPlan(const LongLiteral&);

private:
    uint32_t chainIndexOf(BlockID dominator, BlockID literalBlock) const { return m_dominators.depthOf(literalBlock) - m_dominators.depthOf(dominator); }
    ChainPoint availabilityOf(ValueID element, const LongLiteral&) const;
    bool isOwnElement(ValueID element, const LongLiteral&) const;
    Vector<ChainPoint, 8> collectOwnAvailabilities(const LongLiteral&);

    uint32_t computeCycleBound(std::span<const BlockID> chainBlocks);
    OrGaveUp<uint32_t> computePostDominanceBound(std::span<const BlockID> chainBlocks, ValueID literal);
    OrGaveUp<DominatorChain> buildChain(const LongLiteral&, std::span<const ChainPoint> ownAvailabilities);

    static ChainPoint chooseAllocation(const DominatorChain&, const LongLiteral&, std::span<const ChainPoint> ownAvailabilities);
    void planStores(LiteralPlan&, const DominatorChain&, ChainPoint allocation, const LongLiteral&) const;
    void planStoresAtLiteral(LiteralPlan&) const;

    FunctionPassContext& m_context;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const Dominators& m_dominators;
    const UseLists& m_useLists;
    BitVector m_visitedElements;
};

LiteralPlanner::LiteralPlanner(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_cfg(context.analyses.cfg())
    , m_dominators(context.analyses.dominators())
    , m_useLists(context.analyses.useLists())
    , m_visitedElements(context.function.values.size())
{
}

LiteralPlan LiteralPlanner::createPlan(const LongLiteral& literal)
{
    const Value& value = m_function.at(literal.value);
    RELEASE_ASSERT(!value.capacityOperand() && value.literalStorage() == ArrayLiteralStorage::Fresh);

    LiteralPlan plan;
    plan.literal = literal.value;
    plan.length = static_cast<uint32_t>(value.elementsOperands().size() + value.holes().size());
    plan.allocation = literal.point;
    if (!m_cfg.isReachable(literal.point.block)) {
        planStoresAtLiteral(plan);
        return plan;
    }

    Vector<ChainPoint, 8> ownAvailabilities = collectOwnAvailabilities(literal);
    OrGaveUp<DominatorChain> chain = buildChain(literal, ownAvailabilities.span());
    if (!chain) {
        planStoresAtLiteral(plan);
        return plan;
    }

    ChainPoint allocation = chooseAllocation(*chain, literal, ownAvailabilities.span());
    plan.allocation = chain->programPointOf(allocation);
    planStores(plan, *chain, allocation, literal);
    return plan;
}

ChainPoint LiteralPlanner::availabilityOf(ValueID element, const LongLiteral& literal) const
{
    ProgramPoint point = m_useLists.pointAfter(element);

    RELEASE_ASSERT(m_dominators.dominates(point.block, literal.point.block));
    ChainPoint availability { chainIndexOf(point.block, literal.point.block), point.index };
    ChainPoint literalPoint { 0, literal.point.index };
    RELEASE_ASSERT(!literalPoint.isEarlierThan(availability));
    return availability;
}

bool LiteralPlanner::isOwnElement(ValueID element, const LongLiteral& literal) const
{
    if (!isComputedElement(m_context.program, m_function, element))
        return false;
    return std::ranges::all_of(m_useLists.usesOf(element), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && use.user == literal.value;
    });
}

Vector<ChainPoint, 8> LiteralPlanner::collectOwnAvailabilities(const LongLiteral& literal)
{
    std::span<const ValueID> elements = m_function.at(literal.value).elementsOperands();
    Vector<ChainPoint, 8> availabilities;
    for (ValueID element : elements) {
        if (!m_visitedElements.quickSet(element.index()) && isOwnElement(element, literal))
            availabilities.append(availabilityOf(element, literal));
    }
    for (ValueID element : elements)
        m_visitedElements.quickClear(element.index());
    return availabilities;
}

uint32_t LiteralPlanner::computeCycleBound(std::span<const BlockID> chainBlocks)
{
    BlockID literalBlock = chainBlocks.front();
    std::span<const BlockID> dominators = chainBlocks.subspan(1);
    if (!m_analyses.isInCycle(literalBlock))
        return static_cast<uint32_t>(dominators.size());

    // A cycle through the literal that avoids one of its dominators avoids every earlier dominator.
    auto firstAvoided = std::ranges::partition_point(dominators, [&](BlockID dominator) {
        return !m_cfg.isInCycleAvoiding(literalBlock, dominator);
    });
    return static_cast<uint32_t>(firstAvoided - dominators.begin());
}

OrGaveUp<uint32_t> LiteralPlanner::computePostDominanceBound(std::span<const BlockID> chainBlocks, ValueID literal)
{
    BitVector visited(m_cfg.numNodes());
    visited.quickSet(chainBlocks.front().index());
    uint64_t numberOfBlocksSearched = 0;
    Vector<BlockID, 16> worklist;
    for (uint32_t chainIndex = 1; chainIndex < chainBlocks.size(); ++chainIndex) {
        if (!visited.quickSet(chainBlocks[chainIndex].index()))
            worklist.append(chainBlocks[chainIndex]);
        while (!worklist.isEmpty()) {
            const Terminator& terminator = m_function.at(worklist.takeLast()).terminator;
            if (returnsToCaller(terminator.kind))
                return chainIndex - 1;
            if (m_context.exceeds(LimitID::MaxBlocksSearchedForLiteralStores, ++numberOfBlocksSearched, literal))
                return std::unexpected(GaveUp { LimitID::MaxBlocksSearchedForLiteralStores });
            for (const Target& target : terminator.targets) {
                if (!visited.quickSet(target.block.index()))
                    worklist.append(target.block);
            }
        }
    }
    return static_cast<uint32_t>(chainBlocks.size() - 1);
}

OrGaveUp<DominatorChain> LiteralPlanner::buildChain(const LongLiteral& literal, std::span<const ChainPoint> ownAvailabilities)
{
    BlockID literalBlock = literal.point.block;
    DominatorChain chain;
    chain.blocks.append(literalBlock);
    chain.nearestUsablePoints.append({ 0, indexAfterCaughtValues(m_function, m_function.at(literalBlock)) });

    uint32_t top = 0;
    for (ChainPoint availability : ownAvailabilities)
        top = std::max(top, availability.chainIndex);
    for (uint32_t chainIndex = 1; chainIndex <= top; ++chainIndex)
        chain.blocks.append(m_dominators.idom(chain.blocks.last()));
    chain.blocks.shrink(computeCycleBound(chain.blocks.span()) + 1);
    OrGaveUp<uint32_t> last = computePostDominanceBound(chain.blocks.span(), literal.value);
    if (!last)
        return std::unexpected(last.error());
    if (!*last)
        return chain;

    const NaturalLoops& loops = m_analyses.naturalLoops();
    const NaturalLoop* literalLoop = loops.innerMostLoopOf(literalBlock);
    const std::optional<Target>& literalHandler = m_function.at(literalBlock).handler;
    for (uint32_t chainIndex = 1; chainIndex <= *last; ++chainIndex) {
        BlockID block = chain.blocks[chainIndex];
        bool isUsable = loops.innerMostLoopOf(block) == literalLoop
            && m_function.at(block).handler == literalHandler
            && m_dominators.terminatorDominates(block, literalBlock);
        ChainPoint nearestUsablePoint = isUsable ? ChainPoint { chainIndex, indexAfterCaughtValues(m_function, m_function.at(block)) } : chain.nearestUsablePoints.last();
        chain.nearestUsablePoints.append(nearestUsablePoint);
    }
    return chain;
}

ChainPoint LiteralPlanner::chooseAllocation(const DominatorChain& chain, const LongLiteral& literal, std::span<const ChainPoint> ownAvailabilities)
{
    ChainPoint earliest { 0, literal.point.index };
    for (ChainPoint availability : ownAvailabilities) {
        ChainPoint candidate = chain.moveToUsableBlock(availability);
        if (chain.isUsable(candidate.chainIndex) && candidate.isEarlierThan(earliest))
            earliest = candidate;
    }
    return earliest;
}

void LiteralPlanner::planStores(LiteralPlan& plan, const DominatorChain& chain, ChainPoint allocation, const LongLiteral& literal) const
{
    forEachElement(m_function.at(literal.value), [&](uint32_t arrayIndex, ValueID element) {
        ChainPoint availability = chain.moveToUsableBlock(availabilityOf(element, literal));
        if (allocation.isEarlierThan(availability))
            plan.laterStores.append(ElementStore { chain.programPointOf(availability), arrayIndex, element });
        else
            plan.storesAfterAllocation.append({ arrayIndex, element });
    });
}

void LiteralPlanner::planStoresAtLiteral(LiteralPlan& plan) const
{
    forEachElement(m_function.at(plan.literal), [&](uint32_t arrayIndex, ValueID element) {
        plan.storesAfterAllocation.append({ arrayIndex, element });
    });
}

Vector<LiteralPlan> planLiterals(FunctionPassContext& context, std::span<const LongLiteral> literals)
{
    LiteralPlanner planner(context);
    Vector<LiteralPlan> plans;
    plans.reserveInitialCapacity(literals.size());
    for (const LongLiteral& literal : literals)
        plans.append(planner.createPlan(literal));
    return plans;
}

struct AllocatedArray {
    ValueID array;
    ValueID length;
};

class LiteralRewriter {
    WTF_MAKE_NONCOPYABLE(LiteralRewriter);
public:
    LiteralRewriter(FunctionPassContext&, std::span<const LiteralPlan>);

    void rewrite();

private:
    ValueID insertInteger(ProgramPoint, int64_t);
    AllocatedArray insertAllocation(const LiteralPlan&);
    void insertStore(ProgramPoint, ValueID array, uint32_t arrayIndex, ValueID element);

    void insertAllocations();
    void insertLaterStores();
    void replaceLiterals();

    Origin originOf(const LiteralPlan& plan) const { return m_function.at(plan.literal).origin(); }

    Function& m_function;
    IRBuilder m_builder;
    std::span<const LiteralPlan> m_plans;
    Vector<AllocatedArray> m_arrays;
};

LiteralRewriter::LiteralRewriter(FunctionPassContext& context, std::span<const LiteralPlan> plans)
    : m_function(context.function)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_plans(plans)
    , m_arrays(plans.size())
{
}

void LiteralRewriter::rewrite()
{
    insertAllocations();
    insertLaterStores();
    m_builder.applyInsertions();
    replaceLiterals();
}

ValueID LiteralRewriter::insertInteger(ProgramPoint point, int64_t integer)
{
    Value constant = IRBuilder::make(Opcode::Integer);
    constant.setInteger(integer);
    return m_builder.insertBefore(point, WTF::move(constant));
}

AllocatedArray LiteralRewriter::insertAllocation(const LiteralPlan& plan)
{
    ValueID length = insertInteger(plan.allocation, plan.length);
    Value allocation = IRBuilder::make(Opcode::NewContiguousArray, { length });
    allocation.setAllocationSite(m_function.at(plan.literal).allocationSite());
    return { m_builder.insertBefore(plan.allocation, WTF::move(allocation)), length };
}

void LiteralRewriter::insertStore(ProgramPoint point, ValueID array, uint32_t arrayIndex, ValueID element)
{
    ValueID position = insertInteger(point, arrayIndex);
    m_builder.insertBefore(point, IRBuilder::make(Opcode::PutContiguousIndex, { array, position, element }));
}

void LiteralRewriter::insertAllocations()
{
    for (uint32_t planIndex = 0; planIndex < m_plans.size(); ++planIndex) {
        const LiteralPlan& plan = m_plans[planIndex];
        m_builder.setOrigin(originOf(plan));
        m_arrays[planIndex] = insertAllocation(plan);
        for (auto [arrayIndex, element] : plan.storesAfterAllocation)
            insertStore(plan.allocation, m_arrays[planIndex].array, arrayIndex, element);
    }
}

void LiteralRewriter::insertLaterStores()
{
    for (uint32_t planIndex = 0; planIndex < m_plans.size(); ++planIndex) {
        const LiteralPlan& plan = m_plans[planIndex];
        m_builder.setOrigin(originOf(plan));
        for (const ElementStore& store : plan.laterStores)
            insertStore(store.point, m_arrays[planIndex].array, store.arrayIndex, store.element);
    }
}

void LiteralRewriter::replaceLiterals()
{
    for (uint32_t planIndex = 0; planIndex < m_plans.size(); ++planIndex) {
        Value& literal = m_function.edit(m_plans[planIndex].literal);
        literal.become(Opcode::TruncateNewArray);
        literal.setArrayOperand(m_arrays[planIndex].array);
        literal.setLengthOperand(m_arrays[planIndex].length);
    }
}

} // anonymous namespace

PassOutcome expandLongArrayLiterals(FunctionPassContext& context)
{
    Vector<LongLiteral, 4> literals = findLongLiterals(context);
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    Vector<LiteralPlan> plans = planLiterals(context, literals.span());
    LiteralRewriter(context, plans.span()).rewrite();
    // Values are put into the blocks that are there.
    context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
