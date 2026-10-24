/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTRuleSupport.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ChainPoint {
    bool isEarlierThan(ChainPoint other) const { return chainIndex != other.chainIndex ? chainIndex > other.chainIndex : index < other.index; }

    uint32_t chainIndex { 0 };
    uint32_t index { 0 };
};

struct ElementStore {
    ProgramPoint point;
    uint32_t arrayIndex;
    ValueID element;
};

struct LiteralPlan {
    ValueID literal;
    uint32_t length;
    ProgramPoint allocation;
    Vector<ElementStore> stores { };
    ValueID array { };
    ValueID lengthOfArray { };
};

struct DominatorChain {
    bool isUsable(uint32_t chainIndex) const { return chainIndex < nearestUsablePoints.size() && nearestUsablePoints[chainIndex].chainIndex == chainIndex; }
    ProgramPoint programPointOf(ChainPoint point) const { return { blocks[point.chainIndex], point.index }; }
    ChainPoint moveToUsableBlock(ChainPoint point) const { return point.chainIndex >= nearestUsablePoints.size() || isUsable(point.chainIndex) ? point : nearestUsablePoints[point.chainIndex]; }

    // blocks[0] holds the literal and blocks[j + 1] is the immediate dominator of blocks[j], so a greater chain index is earlier.
    Vector<BlockID, 8> blocks;
    Vector<ChainPoint, 8> nearestUsablePoints;
};

bool isComputedElement(const Program& program, const Function& function, ValueID element)
{
    return !isRematerializable(function.at(element)) && !program.typeOf(function, element).isNumber();
}

struct LiteralPlanner {
    FunctionPassContext& context;
    const Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    const CFG& cfg { analyses.cfg() };
    const Dominators& dominators { analyses.dominators() };
    const UseLists& useLists { analyses.useLists() };
    BitVector visitedElements { BitVector(function.values.size()) };

    LiteralPlan createPlan(ValueID id, ProgramPoint point)
    {
        const Value& literal = function.at(id);
        RELEASE_ASSERT(!literal.capacityOperand() && literal.literalStorage() == ArrayLiteralStorage::Fresh);
        LiteralPlan plan { id, static_cast<uint32_t>(literal.elementsOperands().size() + literal.holes().size()), point };
        auto availabilityOf = [&](ValueID element) {
            ProgramPoint available = useLists.pointAfter(element);
            RELEASE_ASSERT(dominators.dominates(available.block, point.block));
            ChainPoint availability { dominators.depthOf(point.block) - dominators.depthOf(available.block), available.index };
            RELEASE_ASSERT(!(ChainPoint { 0, point.index }).isEarlierThan(availability));
            return availability;
        };

        ChainPoint allocation { 0, point.index };
        std::optional<DominatorChain> chain;
        if (cfg.isReachable(point.block)) {
            Vector<ChainPoint, 8> ownAvailabilities;
            for (ValueID element : literal.elementsOperands()) {
                if (!visitedElements.quickSet(element.index()) && isOwnElement(element, id))
                    ownAvailabilities.append(availabilityOf(element));
            }
            for (ValueID element : literal.elementsOperands())
                visitedElements.quickClear(element.index());
            if ((chain = buildChain(id, point.block, ownAvailabilities.span()))) {
                for (ChainPoint availability : ownAvailabilities) {
                    ChainPoint candidate = chain->moveToUsableBlock(availability);
                    if (chain->isUsable(candidate.chainIndex) && candidate.isEarlierThan(allocation))
                        allocation = candidate;
                }
                plan.allocation = chain->programPointOf(allocation);
            }
        }

        std::span<const uint32_t> holes = literal.holes();
        uint32_t arrayIndex = 0;
        for (ValueID element : literal.elementsOperands()) {
            for (; !holes.empty() && holes.front() == arrayIndex; holes = holes.subspan(1))
                ++arrayIndex;
            ChainPoint availability = chain ? chain->moveToUsableBlock(availabilityOf(element)) : allocation;
            plan.stores.append({ allocation.isEarlierThan(availability) ? chain->programPointOf(availability) : plan.allocation, arrayIndex++, element });
        }
        return plan;
    }

    bool isOwnElement(ValueID element, ValueID literal) const
    {
        return isComputedElement(context.program, function, element) && std::ranges::all_of(useLists.usesOf(element), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && use.user == literal;
        });
    }

    uint32_t computeCycleBound(std::span<const BlockID> chainBlocks)
    {
        BlockID literalBlock = chainBlocks.front();
        std::span<const BlockID> dominatorsOfLiteral = chainBlocks.subspan(1);
        if (!analyses.isInCycle(literalBlock))
            return static_cast<uint32_t>(dominatorsOfLiteral.size());
        // A cycle through the literal that avoids one of its dominators avoids every earlier dominator.
        auto firstAvoided = std::ranges::partition_point(dominatorsOfLiteral, [&](BlockID dominator) {
            return !cfg.isInCycleAvoiding(literalBlock, dominator);
        });
        return static_cast<uint32_t>(firstAvoided - dominatorsOfLiteral.begin());
    }

    // Nothing where the search has gone on for too long.
    std::optional<uint32_t> computePostDominanceBound(std::span<const BlockID> chainBlocks, ValueID literal)
    {
        BitVector visited(cfg.numNodes());
        visited.quickSet(chainBlocks.front().index());
        uint64_t numberOfBlocksSearched = 0;
        Vector<BlockID, 16> worklist;
        for (uint32_t chainIndex = 1; chainIndex < chainBlocks.size(); ++chainIndex) {
            if (!visited.quickSet(chainBlocks[chainIndex].index()))
                worklist.append(chainBlocks[chainIndex]);
            while (!worklist.isEmpty()) {
                const Terminator& terminator = function.at(worklist.takeLast()).terminator;
                if (returnsToCaller(terminator.kind))
                    return chainIndex - 1;
                if (context.exceeds(LimitID::MaxBlocksSearchedForLiteralStores, ++numberOfBlocksSearched, literal))
                    return std::nullopt;
                for (const Target& target : terminator.targets) {
                    if (!visited.quickSet(target.block.index()))
                        worklist.append(target.block);
                }
            }
        }
        return static_cast<uint32_t>(chainBlocks.size() - 1);
    }

    std::optional<DominatorChain> buildChain(ValueID literal, BlockID literalBlock, std::span<const ChainPoint> ownAvailabilities)
    {
        DominatorChain chain;
        chain.blocks.append(literalBlock);
        chain.nearestUsablePoints.append({ 0, indexAfterCaughtValues(function, function.at(literalBlock)) });
        uint32_t top = 0;
        for (ChainPoint availability : ownAvailabilities)
            top = std::max(top, availability.chainIndex);
        for (uint32_t chainIndex = 1; chainIndex <= top; ++chainIndex)
            chain.blocks.append(dominators.idom(chain.blocks.last()));
        chain.blocks.shrink(computeCycleBound(chain.blocks.span()) + 1);
        std::optional<uint32_t> last = computePostDominanceBound(chain.blocks.span(), literal);
        if (!last)
            return std::nullopt;
        if (!*last)
            return chain;

        const NaturalLoops& loops = analyses.naturalLoops();
        for (uint32_t chainIndex = 1; chainIndex <= *last; ++chainIndex) {
            BlockID block = chain.blocks[chainIndex];
            bool isUsable = loops.innerMostLoopOf(block) == loops.innerMostLoopOf(literalBlock) && function.at(block).handler == function.at(literalBlock).handler && dominators.terminatorDominates(block, literalBlock);
            chain.nearestUsablePoints.append(isUsable ? ChainPoint { chainIndex, indexAfterCaughtValues(function, function.at(block)) } : chain.nearestUsablePoints.last());
        }
        return chain;
    }
};

} // anonymous namespace

PassOutcome expandLongArrayLiterals(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<LiteralPlan> plans;
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode != Opcode::NewArray)
                continue;
            uint64_t numberOfComputedElements = std::ranges::count_if(value.elementsOperands(), [&](ValueID element) {
                return isComputedElement(context.program, function, element);
            });
            if (context.exceeds(LimitID::MaxArrayLiteralBatch, numberOfComputedElements, values[index]))
                plans.append({ values[index], 0, { BlockID(blockIndex), index } });
        }
    }
    if (plans.isEmpty())
        return PassOutcome::Unchanged;
    LiteralPlanner planner { context };
    for (LiteralPlan& plan : plans)
        plan = planner.createPlan(plan.literal, plan.allocation);

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    auto insertStores = [&](const LiteralPlan& plan, bool standBehindAllocation) {
        builder.setOrigin(function.at(plan.literal).origin());
        for (const ElementStore& store : plan.stores) {
            if ((store.point == plan.allocation) == standBehindAllocation)
                builder.insertBefore(store.point, IRBuilder::make(Opcode::PutContiguousIndex, { plan.array, builder.insertBefore(store.point, makeInteger(store.arrayIndex)), store.element }));
        }
    };
    for (LiteralPlan& plan : plans) {
        builder.setOrigin(function.at(plan.literal).origin());
        plan.lengthOfArray = builder.insertBefore(plan.allocation, makeInteger(plan.length));
        Value allocation = IRBuilder::make(Opcode::NewContiguousArray, { plan.lengthOfArray });
        allocation.setAllocationSite(function.at(plan.literal).allocationSite());
        plan.array = builder.insertBefore(plan.allocation, WTF::move(allocation));
        insertStores(plan, true);
    }
    for (const LiteralPlan& plan : plans)
        insertStores(plan, false);
    builder.applyInsertions();
    for (const LiteralPlan& plan : plans) {
        Value& literal = function.edit(plan.literal);
        literal.become(Opcode::TruncateNewArray);
        literal.setArrayOperand(plan.array);
        literal.setLengthOperand(plan.lengthOfArray);
    }
    // Values are put into the blocks that are there.
    context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
