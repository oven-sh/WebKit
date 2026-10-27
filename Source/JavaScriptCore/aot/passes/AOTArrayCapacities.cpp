/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr Range plannedTermRange { -(int64_t { 1 } << 32), int64_t { 1 } << 32 };

// The test in front of every addition costs a tenth of the addition, and a small table grows in its stub for little: by runs a plan loses up to 16 additions and wins from 64 on.
AOT_THRESHOLD(int64_t, minAdditionsWorthPlan, 32);

static_assert(static_cast<uint32_t>(MapNextResult::HasEntry) == static_cast<uint32_t>(SetNextResult::HasEntry));

std::optional<MeasuredStorage> storageMeasuredBy(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::GetArrayLength)
        return MeasuredStorage { value.arrayOperand(), value.guardOperand(), AbstractHeap::ArrayStorage };
    if (value.opcode == Opcode::GetTypedArrayLength)
        return MeasuredStorage { value.viewOperand(), { }, AbstractHeap::TypedArrayStorage };
    if (value.opcode != Opcode::CallBuiltin || !value.receiverOperand() || value.needsPristineReceiver())
        return std::nullopt;
    std::optional<InlineBuiltin> getter = inlineImplementationOf(program, value);
    if (getter == InlineBuiltin::ArrayLength || getter == InlineBuiltin::TypedArrayLength)
        return MeasuredStorage { value.receiverOperand(), { }, getter == InlineBuiltin::ArrayLength ? AbstractHeap::ArrayStorage : AbstractHeap::TypedArrayStorage };
    if (getter == InlineBuiltin::MapSize || getter == InlineBuiltin::SetSize)
        return MeasuredStorage { value.receiverOperand(), { }, AbstractHeap::CollectionStorage };
    return std::nullopt;
}

// The test for null and undefined in front of a read of a length, where the read measures what the test lets through.
static ValueID checkInFrontOf(const Function& function, const MeasuredStorage& measured)
{
    return !measured.guard && function.at(measured.owner).opcode == Opcode::RequireObjectCoercible ? measured.owner : ValueID();
}

static ValueID appendedArrayOf(const Value& append)
{
    if (append.opcode == Opcode::CallBuiltin)
        return append.receiverOperand();
    return append.opcode == Opcode::ElemSet ? append.objectOperand() : append.arrayOperand();
}

static ValueID walkedCollectionOf(const Value& step)
{
    if (step.opcode == Opcode::MapNext)
        return step.mapOperand();
    return step.opcode == Opcode::SetNext ? step.setOperand() : ValueID();
}

// nullopt: the class of the collection is not proven.
static std::optional<Value> makeSizeRead(FunctionPassContext& context, ValueID collection, bool isMap)
{
    TypingContext typing { context.program, context.function, context.types };
    BuiltinOperandTypes operands = operandTypesOf(typing, collection, { });
    OverloadMatch match = matchOverload({ typing, isMap ? Builtins::MapPrototypeSize : Builtins::SetPrototypeSize, BuiltinCallMode::Get, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return std::nullopt;
    Value sizeRead = makeBuiltinCall(match.overload, BuiltinCallForm::Whole, { collection, { } }, { });
    if (!overloadTakesOperands(typing, sizeRead))
        return std::nullopt;
    return sizeRead;
}

// nullopt: the value is no step through a Map or a Set, or the class of the collection is not proven.
static std::optional<Value> makeSizeReadOfWalkedCollection(FunctionPassContext& context, const Value& step)
{
    ValueID collection = walkedCollectionOf(step);
    return collection ? makeSizeRead(context, collection, step.opcode == Opcode::MapNext) : std::nullopt;
}

namespace {

enum class CapacityTermKind : uint8_t { Unknown, Constant, Integer, Number };
enum class AppendKind : uint8_t { Elements, List, StoreAtIndex };
enum class CountedIterations : uint8_t { Remaining, Finished };

struct CapacityTerm {
    bool isWhole() const { return kind == CapacityTermKind::Constant || kind == CapacityTermKind::Integer; }

    CapacityTermKind kind { CapacityTermKind::Unknown };
    ValueID value;
    int64_t constant { 0 };
};

struct TripCount {
    CapacityTerm added;
    CapacityTerm subtracted { CapacityTermKind::Constant, { }, 0 };
    int64_t adjustment { 0 };
};

struct CapacityPlan {
    bool hasRunTimeTerm() const { return !added.isEmpty() || !subtracted.isEmpty() || !roundedUp.isEmpty(); }

    void forEachTerm(NOESCAPE const Invocable<void(ValueID&)> auto& functor)
    {
        for (Vector<ValueID, 2>* terms : { &added, &subtracted, &roundedUp }) {
            for (ValueID& term : *terms)
                functor(term);
        }
    }

    bool tryAdd(const CapacityTerm& term, bool isSubtracted = false)
    {
        switch (term.kind) {
        case CapacityTermKind::Unknown:
            return false;
        case CapacityTermKind::Constant:
            constant += isSubtracted ? -term.constant : term.constant;
            return true;
        case CapacityTermKind::Integer:
            (isSubtracted ? subtracted : added).append(term.value);
            return true;
        case CapacityTermKind::Number:
            if (!isSubtracted)
                roundedUp.append(term.value);
            return !isSubtracted;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ValueID literal;
    ProgramPoint point;
    int64_t constant;
    Vector<ValueID, 2> added { };
    Vector<ValueID, 2> subtracted { };
    Vector<ValueID, 2> roundedUp { };
    Vector<ValueID, 2> repeatedLengthReads { };
    // Those whose test for null and undefined is repeated with them.
    Vector<ValueID, 2> repeatedChecks { };
};

// What an array still needs when its vector is full: the iterations a counted loop has left, times what each of them appends.
struct GrowthPlan {
    ValueID append;
    ProgramPoint point;
    ValueID array;
    BlockID loopHeader;
    TripCount remainingIterations;
    // Only of a collection.
    TripCount finishedIterations { };
    BlockID preheader { };
    ValueID sizeAtEntry { };
    ValueID identityOfArray { };
    unsigned elementsPerIteration { 1 };
};

struct AppendSite {
    AppendKind kind;
    ValueID value;
    BlockID block;
    unsigned count { 0 };
    ValueID index { };
};

struct PlannedLiteral {
    ValueID value;
    ProgramPoint point;
};

struct FailedStorageTest {
    Edge edge;
    ValueID array;
    Origin origin;
    // The array that the tested one is a copy of.
    ValueID copied;
    Target targetBeforeSplit { };
    ProgramPoint conversion { };
    bool mayEndProof { false };
};

// CFG::blocksReachableFrom(), as far as the questions make it go: an append stands a few blocks behind its literal, and a function may hold thousands of literals.
struct BlocksReachedFrom {
    const CFG& cfg;
    BlockID start;
    const BlockFrequencies* frequenciesIfOnlyByLikelyPaths { nullptr };
    bool hasStarted { false };
    BitVector isReached { };
    BitVector isPending { };

    bool contains(BlockID block)
    {
        std::span<const BlockID> order = cfg.reversePostOrder();
        const BlockFrequencies* frequencies = frequenciesIfOnlyByLikelyPaths;
        if (!std::exchange(hasStarted, true)) {
            isReached.ensureSize(cfg.numNodes());
            isPending.ensureSize(order.size());
            isReached.quickSet(start.index());
            isPending.quickSet(cfg.reversePostOrderIndex(start));
        }
        while (!isReached.quickGet(block.index())) {
            size_t position = isPending.findBit(0, true);
            if (position >= order.size())
                return false;
            isPending.quickClear(position);
            for (BlockID successor : cfg.successors(order[position])) {
                if ((!frequencies || !(frequencies->classOf(start) < frequencies->classOf(successor)).predicted()) && !isReached.quickSet(successor.index()))
                    isPending.quickSet(cfg.reversePostOrderIndex(successor));
            }
        }
        return true;
    }
};

struct CapacityPlanner {
    FunctionPassContext& context;
    const Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    Vector<BlockID> blocksWhereArmsMeet { };
    BitVector branchesAsked { };

    const NaturalLoop& loopOf(const AppendSite& site) { return *analyses.naturalLoops().innerMostLoopOf(site.block); }
    bool holdsAt(ValueID guard, ProgramPoint point) { return !guard || (analyses.dominates(guard, point) && analyses.isGuardAvailable(guard, point)); }

    Vector<CapacityPlan, 4> collectPlans()
    {
        Vector<ValueID, 8> candidates;
        bool hasAppend = false;
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::NewArray && value.holes().empty() && !value.capacityOperand()
                    && value.elementsOperands().size() <= std::min(context.limit(LimitID::MaxInlineArrayLiteralLength), context.limit(LimitID::MaxArrayLiteralBatch)))
                    candidates.append(id);
                else
                    hasAppend = hasAppend || appendKindOf(value);
            }
        }
        Vector<CapacityPlan, 4> plans;
        for (ValueID candidate : candidates) {
            if (std::optional<CapacityPlan> planned = hasAppend ? plan(candidate) : std::nullopt)
                plans.append(WTF::move(*planned));
        }
        return plans;
    }

    std::optional<CapacityPlan> plan(ValueID id)
    {
        std::optional<ObjectID> object = analyses.escape().objectDefinedBy(id);
        Vector<AppendSite, 4> sites = object ? collectSites(*object) : Vector<AppendSite, 4> { };
        if (sites.isEmpty())
            return std::nullopt;

        const UseLists& useLists = analyses.useLists();
        const NaturalLoops& loops = analyses.naturalLoops();
        ProgramPoint point = useLists.pointOf(id);
        const NaturalLoop* loopOfLiteral = loops.innerMostLoopOf(point.block);
        BitVector chain;
        for (BlockID block = point.block; block && loops.innerMostLoopOf(block) == loopOfLiteral && chain.add(block.index());)
            block = chainSuccessorOf(block).predicted();
        int64_t numberOfElements = function.at(id).elementsOperands().size();
        CapacityPlan result { id, point, numberOfElements };

        // Outside of a cycle nothing leads back to the literal, so an append that is not reached from it appends to another array: one that came out of a field, or from the other arm of a test.
        BlocksReachedFrom blocksBehindLiteral { analyses.cfg(), point.block };
        auto mayFollowLiteral = [&](const AppendSite& site) {
            if (loopOfLiteral || analyses.isInCycle(point.block))
                return true;
            if (site.block == point.block)
                return useLists.locationOf(site.value).index > point.index;
            bool isReached = blocksBehindLiteral.contains(site.block);
            if (context.build.options().verifiesAfterEveryPass) [[unlikely]]
                RELEASE_ASSERT(isReached == analyses.cfg().blocksReachableFrom(point.block, { }).get(site.block.index()));
            return isReached;
        };

        int64_t numberOfConditionalElements = 0;
        Vector<AppendSite, 2> loopSites;
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlocksReachedFrom blocksBehindLiteralByLikelyPaths { analyses.cfg(), point.block, &frequencies };
        for (const AppendSite& site : sites) {
            if ((frequencies.classOf(point.block) < frequencies.classOf(site.block)).predicted() || !mayFollowLiteral(site))
                continue;
            const NaturalLoop* loop = loops.innerMostLoopOf(site.block);
            bool isInOtherCycle = !loop && analyses.isInCycle(site.block);
            if (!isInOtherCycle && loop == loopOfLiteral) {
                bool alwaysRuns = chain.get(site.block.index()) && (site.block != point.block || useLists.locationOf(site.value).index >= point.index);
                if (alwaysRuns ? site.kind == AppendKind::StoreAtIndex : site.kind != AppendKind::Elements)
                    return std::nullopt;
                if (site.kind == AppendKind::List) {
                    const Value& append = function.at(site.value);
                    RELEASE_ASSERT(append.opcode == Opcode::AppendElements);
                    if (!analyses.dominates(append.sourceOperand(), point) || !holdsAt(append.guardOperand(), point) || !result.tryAdd({ CapacityTermKind::Integer, site.value }))
                        return std::nullopt;
                }
                (alwaysRuns ? result.constant : numberOfConditionalElements) += site.count;
            } else if (!isInOtherCycle && loop && likelyOuterLoopOf(*loop).predicted() == loopOfLiteral) {
                if (site.kind == AppendKind::List)
                    return std::nullopt;
                loopSites.append(site);
            } else if (blocksBehindLiteralByLikelyPaths.contains(site.block))
                return std::nullopt;
        }

        if (!loopSites.isEmpty()) {
            if (!tryAddLoopSites(result, { id, point }, loopSites))
                return std::nullopt;
        } else if (!result.hasRunTimeTerm() && result.constant + numberOfConditionalElements - numberOfElements <= 1)
            result.constant += numberOfConditionalElements;
        if (!result.hasRunTimeTerm() && (result.constant <= numberOfElements || context.exceeds(LimitID::MaxPlannedArrayCapacity, static_cast<uint64_t>(result.constant), id)))
            return std::nullopt;

        result.forEachTerm([&](ValueID term) {
            std::optional<MeasuredStorage> measured = storageMeasuredBy(context.program, function.at(term));
            if (!measured || analyses.dominates(term, point))
                return;
            result.repeatedLengthReads.append(term);
            if (!analyses.dominates(measured->owner, point))
                result.repeatedChecks.append(term);
        });
        return result;
    }

    std::optional<AppendKind> appendKindOf(const Value& value) const
    {
        if (value.opcode == Opcode::AppendElement || value.opcode == Opcode::AppendHole)
            return AppendKind::Elements;
        if (value.opcode == Opcode::CallBuiltin && inlineImplementationOf(context.program, value) == InlineBuiltin::ArrayPush)
            return AppendKind::Elements;
        if (value.opcode == Opcode::AppendElements)
            return AppendKind::List;
        if (value.opcode == Opcode::ElemSet || value.opcode == Opcode::PutArrayElement)
            return AppendKind::StoreAtIndex;
        return std::nullopt;
    }

    Vector<AppendSite, 4> collectSites(ObjectID object)
    {
        Vector<AppendSite, 4> sites;
        for (const ObjectUse& objectUse : analyses.escape().usesOf(object)) {
            const Use& use = objectUse.use;
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(use.user);
            bool isCall = user.opcode == Opcode::CallBuiltin;
            std::optional<AppendKind> kind = appendKindOf(user);
            if (!kind || !(isCall ? builtinOperandAt(user, use.operandIndex).isReceiver() : isWrittenOperand(user.opcode, use.operandIndex)))
                continue;
            AppendSite site { *kind, use.user, use.block };
            if (kind == AppendKind::Elements)
                site.count = isCall ? static_cast<unsigned>(builtinCallOf(context.program, user).arguments.size()) : 1;
            else if (kind == AppendKind::StoreAtIndex)
                site.index = user.opcode == Opcode::ElemSet ? user.propertyOperand() : user.positionOperand();
            sites.append(site);
        }
        return sites;
    }

    // SeparateUnlikelyLoopPaths leaves a loop around the loop, which only the unlikely paths close. A capacity is a prediction, and that loop is predicted not to go round.
    Expected<const NaturalLoop*> likelyOuterLoopOf(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Expected<BlockFrequency> frequencyOfLoop = frequencies.classOf(loop.header());
        auto isClosedByLikelyPath = [&](const NaturalLoop& outer) {
            return std::ranges::any_of(analyses.cfg().incomingEdges(outer.header()), [&](Edge edge) {
                return loops.belongsTo(edge.from, outer) && !(frequencyOfLoop < frequencies.classOf(edge.from)).predicted();
            });
        };
        const NaturalLoop* outer = loops.innerMostOuterLoop(loop);
        while (outer && !isClosedByLikelyPath(*outer))
            outer = loops.innerMostOuterLoop(*outer);
        return outer;
    }

    Expected<BlockID> chainSuccessorOf(BlockID block, unsigned numberOfBranchesAround = 0)
    {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return terminator.next().block;
        return terminator.kind == TerminatorKind::Branch ? blockWhereLikelyArmsMeet(block, numberOfBranchesAround) : Expected(BlockID());
    }

    Expected<BlockID> blockWhereLikelyArmsMeet(BlockID block, unsigned numberOfBranchesAround)
    {
        AOT_THRESHOLD(unsigned, maxBlocksOfArm, 16);
        AOT_THRESHOLD(unsigned, maxBranchesAround, 8);
        if (branchesAsked.get(block.index()))
            return blocksWhereArmsMeet[block.index()];
        if (blocksWhereArmsMeet.isEmpty())
            blocksWhereArmsMeet.grow(function.blocks.size());
        branchesAsked.set(block.index());
        if (numberOfBranchesAround >= maxBranchesAround)
            return { };

        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Vector<BlockID, 2> likely;
        for (BlockID successor : analyses.cfg().successors(block)) {
            if (!(frequencies.classOf(successor) < frequencies.classOf(block)).predicted() && !likely.contains(successor))
                likely.append(successor);
        }
        BlockID result;
        if (likely.size() == 1)
            result = likely[0];
        else if (likely.size() == 2) {
            Vector<BlockID, maxBlocksOfArm> firstArm;
            for (BlockID next = likely[0]; next && firstArm.size() < maxBlocksOfArm && !firstArm.contains(next); next = chainSuccessorOf(next, numberOfBranchesAround + 1).predicted())
                firstArm.append(next);
            unsigned numberOfBlocks = 0;
            for (BlockID next = likely[1]; next && numberOfBlocks++ < maxBlocksOfArm; next = chainSuccessorOf(next, numberOfBranchesAround + 1).predicted()) {
                if (firstArm.contains(next)) {
                    result = next;
                    break;
                }
            }
        }
        blocksWhereArmsMeet[block.index()] = result;
        return result;
    }

    bool tryAddLoopSites(CapacityPlan& plan, const PlannedLiteral& literal, Vector<AppendSite, 2>& sites)
    {
        const CFG& cfg = analyses.cfg();
        AOT::sort(sites, [&](const AppendSite& a, const AppendSite& b) {
            return cfg.reversePostOrderIndex(loopOf(a).header()) < cfg.reversePostOrderIndex(loopOf(b).header());
        });
        for (size_t i = 0; i < sites.size(); ++i) {
            const NaturalLoop& loop = loopOf(sites[i]);
            if ((i && &loop == &loopOf(sites[i - 1])) || (sites[i].kind == AppendKind::Elements && sites[i].count != 1))
                return false;
            if (!isEnteredOnLikelyPathsFrom(literal.point.block, loop).predicted() || !runsOncePerIteration(loop, sites[i].block).predicted())
                return false;
        }
        const NaturalLoop& firstLoop = loopOf(sites[0]);
        if (sites.size() > 1 && !areAlternativesTo(firstLoop.header(), literal, sites.subspan(1)))
            return false;
        TripCount count = tripCount(literal, firstLoop, sites[0]);
        plan.constant += count.adjustment;
        return plan.tryAdd(count.added) && plan.tryAdd(count.subtracted, true);
    }

    Expected<bool> isEnteredOnLikelyPathsFrom(BlockID block, const NaturalLoop& loop)
    {
        if (analyses.dominators().dominates(block, loop.header()))
            return true;
        constexpr unsigned maxBlocksBetween = 8;
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Vector<BlockID, maxBlocksBetween> between { loop.header() };
        for (size_t i = 0; i < between.size(); ++i) {
            bool hasLikelyEdge = false;
            for (Edge edge : analyses.cfg().incomingEdges(between[i])) {
                if ((frequencies.classOf(block) < frequencies.classOf(edge.from)).predicted() || loops.belongsTo(edge.from, loop))
                    continue;
                hasLikelyEdge = true;
                if (edge.from == block || between.contains(edge.from))
                    continue;
                if (between.size() == maxBlocksBetween)
                    return false;
                between.append(edge.from);
            }
            if (!hasLikelyEdge)
                return false;
        }
        return true;
    }

    // A capacity is a prediction: a path through a loop counts when it is as likely as the loop.
    Expected<bool> anyLikelyBlockOf(const NaturalLoop& loop, NOESCAPE const Invocable<bool(BlockID)> auto& predicate)
    {
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Expected<BlockFrequency> frequencyOfLoop = frequencies.classOf(loop.header());
        for (unsigned i = 0; i < loop.size(); ++i) {
            if (!(frequencyOfLoop < frequencies.classOf(loop.at(i))).predicted() && predicate(loop.at(i)))
                return true;
        }
        return false;
    }

    Expected<bool> runsOncePerIteration(const NaturalLoop& loop, BlockID block)
    {
        if (block == loop.header())
            return true;
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Expected<BlockFrequency> frequencyOfLoop = frequencies.classOf(loop.header());
        if ((frequencyOfLoop < frequencies.classOf(block)).predicted())
            return false;
        BitVector barrier;
        barrier.set(block.index());
        for (unsigned i = 0; i < loop.size(); ++i) {
            if ((frequencyOfLoop < frequencies.classOf(loop.at(i))).predicted())
                barrier.set(loop.at(i).index());
        }
        BitVector reachedWithoutBlock = analyses.cfg().blocksReachableFrom(loop.header(), barrier);
        return std::ranges::none_of(analyses.cfg().incomingEdges(loop.header()), [&](Edge edge) {
            return loops.belongsTo(edge.from, loop) && reachedWithoutBlock.get(edge.from.index());
        });
    }

    bool areAlternativesTo(BlockID header, const PlannedLiteral& literal, std::span<const AppendSite> others)
    {
        const CFG& cfg = analyses.cfg();
        BitVector nextAllocation;
        nextAllocation.set(literal.point.block.index());
        BitVector reachableFromHeader = cfg.blocksReachableFrom(header, nextAllocation);
        return std::ranges::none_of(others, [&](const AppendSite& other) {
            BlockID otherHeader = loopOf(other).header();
            return reachableFromHeader.get(otherHeader.index()) || cfg.blocksReachableFrom(otherHeader, nextAllocation).get(header.index());
        });
    }

    bool canMeasureAt(const MeasuredStorage& measured, ProgramPoint point)
    {
        return analyses.dominates(measured.owner, point) && !context.program.typeOf(function, measured.owner).mayBeUnspecified() && holdsAt(measured.guard, point);
    }

    // Nothing that anybody could tell lies between the point and the value, and no other way leads on from the point. So what the value does, a throw too, may as well be done at the point.
    bool followsWithoutVisibleEffect(ProgramPoint point, ValueID id)
    {
        ValueLocation target = analyses.useLists().locationOf(id);
        const std::optional<Target>& handler = function.at(point.block).handler;
        BlockID block = point.block;
        uint32_t index = point.index + 1;
        for (unsigned numberOfBlocks = 0; numberOfBlocks < 4; ++numberOfBlocks) {
            const Block& current = function.at(block);
            if (current.handler.has_value() != handler.has_value() || (handler && current.handler->block != handler->block))
                return false;
            uint32_t end = block == target.block ? target.index : current.values.size();
            for (; index < end; ++index) {
                if (function.at(current.values[index]).opcode != Opcode::LoopHint && !analyses.isUnobservable(current.values[index]))
                    return false;
            }
            if (block == target.block || current.terminator.kind != TerminatorKind::Jump)
                return block == target.block;
            block = current.terminator.targets[0].block;
            index = 0;
        }
        return false;
    }

    bool canMeasureBehindCheck(const MeasuredStorage& measured, ProgramPoint point)
    {
        ValueID check = checkInFrontOf(function, measured);
        return check && !function.at(point.block).handler && !context.program.typeOf(function, measured.owner).mayBeUnspecified()
            && analyses.dominates(function.at(check).valueOperand(), point) && followsWithoutVisibleEffect(point, check);
    }

    Expected<bool> mayChangeLengthIn(const NaturalLoop& loop, const MeasuredStorage& measured, ValueID literal)
    {
        auto mayWrite = [&](const EffectsOnEdges& effects) {
            return effects.normal.writes.contains(measured.heap) || effects.handler.writes.contains(measured.heap);
        };
        auto mayChangeLength = [&](ValueID id) {
            const EffectsOnEdges& effects = analyses.effectsOf(id);
            if (!mayWrite(effects))
                return false;
            // A capacity is a prediction: a callback is taken not to change the array that is walked for it.
            const Value& writer = function.at(id);
            if (effects.onAnyEdge().mayRunOtherCode() || writer.opcode == Opcode::MapNext || writer.opcode == Opcode::SetNext)
                return false;
            ValueID appended = appendKindOf(writer) ? appendedArrayOf(writer) : ValueID();
            // The definition of the measured object dominates the literal, so the literal is a newer object.
            return !appended || (analyses.sameObjectThroughCasts(appended) != literal && analyses.escape().mayAlias(appended, measured.owner));
        };
        return anyLikelyBlockOf(loop, [&](BlockID block) {
            return mayWrite(analyses.effectsOfTerminator(block)) || std::ranges::any_of(function.at(block).values, mayChangeLength);
        });
    }

    // `list.length = 0`, list.pop(), list.splice(..): the array does not hold what all the turns have appended.
    Expected<bool> mayTakeElementsAwayIn(const NaturalLoop& loop, ValueID array)
    {
        auto mayTakeAway = [&](ValueID id) {
            const Value& writer = function.at(id);
            const EffectsOnEdges& effects = analyses.effectsOf(id);
            if (!effects.normal.writes.contains(AbstractHeap::ArrayStorage) && !effects.handler.writes.contains(AbstractHeap::ArrayStorage))
                return false;
            // A capacity is a prediction: a callback is taken not to change the array.
            if (appendKindOf(writer) || effects.onAnyEdge().mayRunOtherCode())
                return false;
            return std::ranges::any_of(writer.operands(), [&](ValueID operand) {
                return operand && !context.program.typeOf(function, operand).isNumber() && analyses.escape().mayAlias(operand, array);
            });
        };
        return anyLikelyBlockOf(loop, [&](BlockID block) {
            return std::ranges::any_of(function.at(block).values, mayTakeAway);
        });
    }

    // An exit outside the header that is rarer than the loop does not count: where a type test fails, the loop continues in its generic copy.
    Expected<bool> hasOnlyRareSideExits(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Expected<BlockFrequency> frequencyOfLoop = frequencies.classOf(loop.header());
        return !anyLikelyBlockOf(loop, [&](BlockID block) {
            const Terminator& terminator = function.at(block).terminator;
            return block != loop.header() && (returnsToCaller(terminator.kind) || std::ranges::any_of(terminator.targets, [&](const Target& target) {
                return !loops.belongsTo(target.block, loop) && !(frequencyOfLoop < frequencies.classOf(target.block)).predicted();
            }));
        });
    }

    // The step through a Map or a Set in the header of a loop that is left only where the step finds no further entry.
    ValueID findStepControlling(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const Terminator& terminator = function.at(loop.header()).terminator;
        if (terminator.kind != TerminatorKind::Branch || !hasOnlyRareSideExits(loop).predicted() || !loops.belongsTo(terminator.taken().block, loop) || loops.belongsTo(terminator.notTaken().block, loop))
            return { };
        const Value& hasEntry = function.at(terminator.conditionOperand());
        if (hasEntry.opcode != Opcode::ResultAt || hasEntry.index() != static_cast<uint32_t>(MapNextResult::HasEntry))
            return { };
        ValueID step = hasEntry.resultsOperand();
        return walkedCollectionOf(function.at(step)) && analyses.useLists().blockOf(step) == loop.header() ? step : ValueID();
    }

    TripCount tripCountOfCollectionWalk(const PlannedLiteral& literal, const NaturalLoop& loop, const AppendSite& site)
    {
        ValueID step = site.kind != AppendKind::StoreAtIndex ? findStepControlling(loop) : ValueID();
        if (!step || !makeSizeReadOfWalkedCollection(context, function.at(step)))
            return { };
        MeasuredStorage measured { walkedCollectionOf(function.at(step)), { }, AbstractHeap::CollectionStorage };
        if (!canMeasureAt(measured, literal.point) || mayChangeLengthIn(loop, measured, literal.value).predicted())
            return { };
        return { .added = { CapacityTermKind::Integer, step } };
    }

    std::pair<const InductionVariable*, const InductionTest*> findUnitStepCounter(const NaturalLoop& loop)
    {
        for (const InductionVariable& variable : analyses.inductionVariables().of(loop).variables) {
            if (variable.step.kind != InductionStepKind::Constant || (variable.step.constant != 1 && variable.step.constant != -1))
                continue;
            for (const InductionTest& test : variable.tests) {
                if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
                    continue;
                if (test.exits == LoopExits::OnlyAtTest || (test.block == loop.header() && hasOnlyRareSideExits(loop).predicted()))
                    return { &variable, &test };
            }
        }
        return { };
    }

    TripCount tripCount(const PlannedLiteral& literal, const NaturalLoop& loop, const AppendSite& site)
    {
        auto [counter, test] = findUnitStepCounter(loop);
        if (!counter)
            return tripCountOfCollectionWalk(literal, loop, site);
        CapacityTerm start = termOf(literal, counter->start);
        CapacityTerm bound = termOf(literal, test->bound);
        if (start.kind == CapacityTermKind::Number)
            return { };
        int64_t adjustment = test->relation == Opcode::LessEq || test->relation == Opcode::GreaterEq;
        if (site.kind != AppendKind::StoreAtIndex)
            return counter->step.constant > 0 ? TripCount { bound, start, adjustment } : TripCount { start, bound, adjustment };

        if (counter->step.constant < 0 || start.kind != CapacityTermKind::Constant || start.constant < 0)
            return { };
        ValueID key = analyses.identityOf(site.index);
        if (key == counter->parameter)
            return { .added = bound, .adjustment = adjustment };
        std::optional<MeasuredStorage> measured = storageMeasuredBy(context.program, function.at(key));
        if (measured && measured->heap == AbstractHeap::ArrayStorage && analyses.sameObjectThroughCasts(measured->owner) == literal.value)
            return { bound, start, adjustment };
        return { };
    }

    // What a parameter of a block is given on the edges that are as likely as the block, where that is one value.
    Expected<ValueID> valueOnLikelyEdges(ValueID id)
    {
        if (function.at(id).opcode != Opcode::Parameter)
            return id;
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockID block = analyses.useLists().blockOf(id);
        uint32_t parameterIndex = static_cast<uint32_t>(function.at(block).parameters.find(id));
        ValueID given;
        for (Edge edge : analyses.cfg().incomingEdges(block)) {
            if ((frequencies.classOf(block) < frequencies.classOf(edge.from)).predicted())
                continue;
            ValueID passed = valuePassedOn(function, edge, parameterIndex);
            if (!passed || (given && given != passed))
                return id;
            given = passed;
        }
        return given ? given : id;
    }

    CapacityTerm termOf(const PlannedLiteral& literal, ValueID id)
    {
        if (!id)
            return { };
        id = valueOnLikelyEdges(sameObjectBehindFacts(function, id)).predicted();
        KnownValue known = knownValueOf(context.program, function, id);
        if (known.kind() == KnownValue::Kind::Integer)
            return plannedTermRange.contains(known.asInteger()) ? CapacityTerm { CapacityTermKind::Constant, { }, known.asInteger() } : CapacityTerm { };

        ValueID definition = sameObjectBehindFacts(function, id);
        const Type& type = context.program.typeOf(function, definition);
        bool isAvailable = analyses.dominates(definition, literal.point);
        if (isAvailable && type.isInteger() && type.integers.isSubsetOf(plannedTermRange))
            return { CapacityTermKind::Integer, definition };

        std::optional<MeasuredStorage> measured = storageMeasuredBy(context.program, function.at(definition));
        if (measured && (canMeasureAt(*measured, literal.point) || canMeasureBehindCheck(*measured, literal.point))) {
            const NaturalLoops& loops = analyses.naturalLoops();
            const NaturalLoop* outermost = nullptr;
            for (const NaturalLoop* loop = loops.innerMostLoopOf(analyses.useLists().blockOf(definition)); loop && !loops.belongsTo(literal.point.block, *loop); loop = loops.innerMostOuterLoop(*loop))
                outermost = loop;
            if (!outermost || !mayChangeLengthIn(*outermost, *measured, literal.value).predicted())
                return { CapacityTermKind::Integer, definition };
        }
        return isAvailable && type.isNumber() ? CapacityTerm { CapacityTermKind::Number, definition } : CapacityTerm { };
    }

    TripCount iterationsAt(ProgramPoint point, const NaturalLoop& loop, ValueID array, CountedIterations counted)
    {
        auto [counter, test] = findUnitStepCounter(loop);
        if (!counter)
            return { };
        auto termAtPoint = [&](ValueID id) {
            CapacityTerm term = termOf({ array, point }, id);
            return term.value && !analyses.dominates(term.value, point) ? CapacityTerm { } : term;
        };
        bool countsRemaining = counted == CountedIterations::Remaining;
        CapacityTerm current = termAtPoint(counter->parameter);
        CapacityTerm end = termAtPoint(countsRemaining ? test->bound : counter->start);
        int64_t adjustment = countsRemaining && (test->relation == Opcode::LessEq || test->relation == Opcode::GreaterEq);
        return countsRemaining == (counter->step.constant > 0) ? TripCount { end, current, adjustment } : TripCount { current, end, adjustment };
    }

    std::optional<GrowthPlan> planGrowth(const NaturalLoop& loop, ProgramPoint point)
    {
        const Program& program = context.program;
        ValueID id = function.at(point.block).values[point.index];
        const Value& append = function.at(id);
        if (appendKindOf(append) != AppendKind::Elements || (append.opcode == Opcode::CallBuiltin ? builtinCallOf(program, append).arguments.size() != 1 : append.opcode != Opcode::AppendElement))
            return std::nullopt;
        ValueID array = appendedArrayOf(append);
        if (analyses.blockFrequencies().classOf(point.block).predicted() == BlockFrequency::Rare || !program.typeOf(function, array).isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program))
            return std::nullopt;
        // The capacity test makes the append's load of the indexing mode control dependent, and B3 hoists such a load out of the loop only from a pointer that code generation publishes as dereferenceable.
        const Value& origin = function.at(analyses.sameObjectThroughCasts(array));
        if (!isInvariantIn(analyses, loop, array) || (origin.opcode != Opcode::Parameter && resultRuleOf(origin.opcode) != ResultRule::Allocation))
            return std::nullopt;
        // A capacity above the limit is found when the literal runs, which then allocates as if nothing were planned.
        if (origin.opcode == Opcode::NewArray && origin.capacityOperand()) {
            const Type& capacity = program.typeOf(function, origin.capacityOperand());
            if (capacity.isInteger() && capacity.integers.maximum <= static_cast<int64_t>(context.limit(LimitID::MaxPlannedArrayCapacity)))
                return std::nullopt;
        }
        if (!runsOncePerIteration(loop, point.block).predicted())
            return std::nullopt;
        TripCount count = iterationsAt(point, loop, array, CountedIterations::Remaining);
        if (count.added.kind == CapacityTermKind::Unknown || count.subtracted.kind == CapacityTermKind::Unknown)
            return std::nullopt;
        if (mayTakeElementsAwayIn(loop, array).predicted())
            return std::nullopt;
        return GrowthPlan { id, point, array, loop.header(), count };
    }

    // The one block from which the loop is entered, where that block leads nowhere else.
    BlockID findPreheader(const NaturalLoop& loop)
    {
        BlockID preheader;
        for (Edge edge : analyses.cfg().incomingEdges(loop.header())) {
            if (analyses.naturalLoops().belongsTo(edge.from, loop))
                continue;
            if (preheader)
                return { };
            preheader = edge.from;
        }
        return preheader && function.at(preheader).terminator.kind == TerminatorKind::Jump ? preheader : BlockID();
    }

    // The table of a Map or a Set grows by half, and each step leaves the table before it behind.
    std::optional<GrowthPlan> planCollectionGrowth(const NaturalLoop& loop, ProgramPoint point)
    {
        const Program& program = context.program;
        ValueID id = function.at(point.block).values[point.index];
        const Value& addition = function.at(id);
        std::optional<InlineBuiltin> implementation = addition.opcode == Opcode::CallBuiltin ? inlineImplementationOf(program, addition) : std::nullopt;
        if ((implementation != InlineBuiltin::MapSet && implementation != InlineBuiltin::SetAdd) || analyses.blockFrequencies().classOf(point.block).predicted() == BlockFrequency::Rare)
            return std::nullopt;
        ValueID collection = addition.receiverOperand();
        BlockID preheader = findPreheader(loop);
        if (!preheader || !isInvariantIn(analyses, loop, collection) || program.typeOf(function, collection).mayBeUnspecified())
            return std::nullopt;
        // A value that is invariant may still be computed in the loop.
        if (!analyses.dominates(collection, pointBeforeTerminatorOf(function, preheader)) || !makeSizeRead(context, collection, implementation == InlineBuiltin::MapSet))
            return std::nullopt;
        TripCount remaining = iterationsAt(point, loop, collection, CountedIterations::Remaining);
        TripCount finished = iterationsAt(point, loop, collection, CountedIterations::Finished);
        if (!remaining.added.isWhole() || !remaining.subtracted.isWhole() || !finished.added.isWhole() || !finished.subtracted.isWhole())
            return std::nullopt;

        // What is left and what is finished add up to all the turns of the loop: the counter goes out of the sum.
        auto rangeOf = [&](const CapacityTerm& term) {
            return term.kind == CapacityTermKind::Constant ? Range::one(term.constant) : program.typeOf(function, term.value).integers;
        };
        bool countsUp = remaining.subtracted.kind == CapacityTermKind::Integer && remaining.subtracted.value == finished.added.value;
        int64_t leastTurns = countsUp ? rangeOf(remaining.added).minimum - rangeOf(finished.subtracted).maximum : rangeOf(finished.added).minimum - rangeOf(remaining.subtracted).maximum;
        if (leastTurns + remaining.adjustment < minAdditionsWorthPlan)
            return std::nullopt;
        return GrowthPlan { id, point, collection, loop.header(), remaining, finished, preheader };
    }

    bool appendsTo(const GrowthPlan& plan, BlockID loopHeader, ValueID array) { return plan.loopHeader == loopHeader && analyses.identityOf(plan.array) == array; }

    void countElementsPerIteration(Vector<GrowthPlan, 4>& plans)
    {
        for (GrowthPlan& plan : plans) {
            plan.identityOfArray = analyses.identityOf(plan.array);
            plan.elementsPerIteration = std::ranges::count_if(plans, [&](const GrowthPlan& other) {
                return appendsTo(other, plan.loopHeader, plan.identityOfArray);
            });
        }
    }

    Vector<GrowthPlan, 4> collectGrowthPlans(Opcode reservation, std::optional<GrowthPlan> (CapacityPlanner::*planAt)(const NaturalLoop&, ProgramPoint))
    {
        Vector<GrowthPlan, 4> plans;
        if (!analyses.cfg().hasCycle() || !analyses.naturalLoops().numLoops())
            return plans;
        const NaturalLoops& loops = analyses.naturalLoops();
        Vector<std::pair<BlockID, ValueID>, 4> reservedArrays;
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const NaturalLoop* loop = loops.innerMostLoopOf(block);
            const BlockValues& values = function.at(block).values;
            for (uint32_t index = 0; loop && index < values.size(); ++index) {
                const Value& value = function.at(values[index]);
                if (value.opcode == reservation)
                    reservedArrays.append({ loop->header(), analyses.identityOf(value.operand(0)) });
                else if (std::optional<GrowthPlan> plan = (this->*planAt)(*loop, { block, index }))
                    plans.append(WTF::move(*plan));
            }
        }
        plans.removeAllMatching([&](const GrowthPlan& plan) {
            return reservedArrays.containsIf([&](const auto& reserved) {
                return appendsTo(plan, reserved.first, reserved.second);
            });
        });
        countElementsPerIteration(plans);
        return plans;
    }

    Vector<GrowthPlan, 4> collectCollectionGrowthPlans()
    {
        auto keyOf = [&](const GrowthPlan& plan) {
            return analyses.identityOf(builtinCallOf(context.program, function.at(plan.append)).arguments[0]);
        };
        // The second addition of a key adds nothing.
        Vector<GrowthPlan, 4> plans;
        for (GrowthPlan& plan : collectGrowthPlans(Opcode::ReserveCollectionCapacity, &CapacityPlanner::planCollectionGrowth)) {
            bool repeatsKey = plans.containsIf([&](const GrowthPlan& other) {
                return plan.loopHeader == other.loopHeader && plan.identityOfArray == other.identityOfArray && keyOf(plan) == keyOf(other);
            });
            if (!repeatsKey)
                plans.append(WTF::move(plan));
        }
        countElementsPerIteration(plans);
        return plans;
    }
};

} // anonymous namespace

static unsigned applyCapacityPlans(FunctionPassContext& context, Vector<CapacityPlan, 4>& plans)
{
    if (plans.isEmpty())
        return 0;
    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    unsigned numberOfPlannedLiterals = 0;
    for (CapacityPlan& plan : plans) {
        auto insert = [&](Value&& value, ValueID placeOfErrors = { }) {
            builder.setOrigin(function.at(placeOfErrors ? placeOfErrors : plan.literal).origin());
            return builder.insertBefore(plan.point, WTF::move(value));
        };
        plan.forEachTerm([&](ValueID& term) {
            const Value& value = function.at(term);
            if (value.opcode == Opcode::AppendElements)
                term = insert(IRBuilder::make(Opcode::GetArrayLength, { value.sourceOperand(), value.guardOperand() }));
            else if (std::optional<Value> sizeRead = makeSizeReadOfWalkedCollection(context, value))
                term = insert(WTF::move(*sizeRead));
            else if (plan.repeatedLengthReads.contains(term)) {
                Value read = value;
                if (plan.repeatedChecks.contains(term)) {
                    // The error names the place of the test that the program has.
                    ValueID check = checkInFrontOf(function, *storageMeasuredBy(context.program, read));
                    ValueID repeatedCheck = insert(Value(function.at(check)), check);
                    read.forEachChild([&](ValueID& operand) {
                        if (operand == check)
                            operand = repeatedCheck;
                    });
                }
                term = insert(WTF::move(read));
            }
        });

        ValueID sum = insert(makeInteger(plan.constant));
        for (ValueID term : plan.added)
            sum = insert(IRBuilder::make(Opcode::Add, { sum, term }));
        for (ValueID term : plan.subtracted)
            sum = insert(IRBuilder::make(Opcode::Sub, { sum, term }));
        for (ValueID term : plan.roundedUp) {
            ValueID truncated = insert(IRBuilder::make(Opcode::BitOr, { term, insert(makeInteger(0)) }));
            ValueID roundedUp = insert(IRBuilder::make(Opcode::Add, { truncated, insert(makeInteger(1)) }));
            sum = insert(IRBuilder::make(Opcode::Add, { sum, roundedUp }));
        }
        if (!context.program.typeOf(function, sum).isInteger())
            continue;
        function.edit(plan.literal, ValueEdit::Definition).setCapacityOperand(sum);
        ++numberOfPlannedLiterals;
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return numberOfPlannedLiterals;
}

static ValueID appendElementCount(IRBuilder& builder, const GrowthPlan& plan, const TripCount& count)
{
    auto appendTerm = [&](const CapacityTerm& term) {
        RELEASE_ASSERT(term.kind != CapacityTermKind::Unknown);
        return term.kind == CapacityTermKind::Constant ? builder.appendInteger(term.constant) : term.value;
    };
    ValueID iterations = builder.append(Opcode::Sub, { appendTerm(count.added), appendTerm(count.subtracted) });
    int64_t adjustment = count.adjustment;
    if (count.added.kind == CapacityTermKind::Number || count.subtracted.kind == CapacityTermKind::Number) {
        iterations = builder.append(Opcode::BitOr, { iterations, builder.appendInteger(0) });
        ++adjustment;
    }
    if (adjustment)
        iterations = builder.append(Opcode::Add, { iterations, builder.appendInteger(adjustment) });
    if (plan.elementsPerIteration > 1)
        iterations = builder.append(Opcode::Mul, { iterations, builder.appendInteger(plan.elementsPerIteration) });
    return iterations;
}

static unsigned applyGrowthPlans(FunctionPassContext& context, Vector<GrowthPlan, 4>&& plans)
{
    if (plans.isEmpty())
        return 0;
    Function& function = context.function;
    // An expansion moves only the values after its point.
    AOT::sort(plans, [](const GrowthPlan& a, const GrowthPlan& b) {
        return std::tie(a.point.block, a.point.index) > std::tie(b.point.block, b.point.index);
    });

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    // One read for each loop and collection, at the end of the block in front of the loop, where it moves no point.
    for (GrowthPlan& plan : plans) {
        if (!plan.preheader)
            continue;
        GrowthPlan* first = std::ranges::find_if(plans, [&](const GrowthPlan& other) {
            return other.loopHeader == plan.loopHeader && other.identityOfArray == plan.identityOfArray;
        });
        if (first != &plan) {
            plan.sizeAtEntry = first->sizeAtEntry;
            continue;
        }
        bool isMap = inlineImplementationOf(context.program, function.at(plan.append)) == InlineBuiltin::MapSet;
        builder.setOrigin(function.at(plan.preheader).terminator.origin());
        plan.sizeAtEntry = builder.insertBefore(pointBeforeTerminatorOf(function, plan.preheader), *makeSizeRead(context, plan.array, isMap));
    }
    if (builder.hasInsertions())
        builder.applyInsertions();

    for (const GrowthPlan& plan : plans) {
        Value append = function.at(plan.append);
        Expected<BlockFrequency> frequency = function.at(plan.point.block).frequency;
        builder.setOrigin(append.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(plan.point);
        auto appendAndRejoin = [&] {
            ValueID result = resultRuleOf(append.opcode) == ResultRule::Declared ? builder.appendWithType(Value(append), append.type) : builder.append(Value(append));
            builder.endExpansion(expansion, singleElementSpan(result).first(expansion.results.isEmpty() ? 0 : 1));
        };
        if (plan.preheader) {
            BlockID adds = builder.addBlock(frequency);
            BlockID grows = builder.addBlock(BlockFrequency::Unlikely);
            builder.emitBranch(builder.append(Opcode::IsCollectionTableFull, { plan.array }), Target { grows, { }, { } }, Target { adds, { }, { } });
            builder.moveToEndOf(grows);
            builder.append(Opcode::ReserveCollectionCapacity, { plan.array, appendElementCount(builder, plan, plan.remainingIterations), appendElementCount(builder, plan, plan.finishedIterations), plan.sizeAtEntry });
            builder.emitJump(adds);
            builder.moveToEndOf(adds);
            appendAndRejoin();
            continue;
        }
        // The append on the taken edge repeats the test, which B3 folds, so an array with spare capacity pays nothing.
        IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(Opcode::HasSpareArrayCapacity, { plan.array }), frequency, BlockFrequency::Rare);
        builder.moveToEndOf(blocks.taken);
        appendAndRejoin();
        builder.moveToEndOf(blocks.notTaken);
        builder.append(Opcode::ReserveArrayCapacity, { plan.array, appendElementCount(builder, plan, plan.remainingIterations) });
        appendAndRejoin();
    }
    return plans.size();
}

static constexpr OpcodeSet readsNamingArrayStorage { Opcode::GetArrayElement, Opcode::HasArrayElement, Opcode::IsElementPresent };
static constexpr OpcodeSet usesOfConstructedStorage = readsNamingArrayStorage | OpcodeSet { Opcode::TryPutByValDirect, Opcode::PutArrayElement, Opcode::HasArrayStorage };

static ArrayStorage storageHolding(FunctionPassContext& context, const Value& allocation, std::span<const ValueID> stores)
{
    const Function& function = context.function;
    TypeID stored = function.at(function.at(stores.front()).valueOperand()).type;
    for (ValueID store : stores.subspan(1))
        stored = context.types.unionOf(stored, function.at(function.at(store).valueOperand()).type);
    ArrayStorage kind = arrayStorageAtAllocation(context.types.at(stored));
    // The readers test for the kind that the declared elements give the array.
    if (kind == ArrayStorage::Int32 && allocation.expected().predicted() && arrayStorageDeclaredBy(context.program, context.types.at(allocation.expected().predicted())).predicted() == ArrayStorage::Double)
        return ArrayStorage::Double;
    return kind;
}

// One value may name either of several arrays under construction: a loop that is there twice makes the array twice and goes on in one place. A store through that value finds one kind of storage in all of them.
static unsigned chooseStorageOfConstructedArrays(FunctionPassContext& context)
{
    struct Group {
        Vector<ValueID, 2> directStores;
        Vector<ValueID, 2> directStoresOutsideRareCode;
        Vector<ValueID, 2> testedStores;
        Vector<ValueID, 2> tests;
        bool hasOtherUseOfStorage { false };
        bool hasChosenStorage { false };
        ArrayStorage chosen { ArrayStorage::Other };
    };
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.opcodesOfValues().intersects(constructionAllocations))
        return 0;

    Vector<ValueID, 2> allocations;
    Vector<unsigned, 2> leaders;
    HashMap<ValueID, unsigned> ordinals;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (!constructionAllocations.contains(function.at(id).opcode))
                continue;
            ordinals.add(id, allocations.size());
            leaders.append(allocations.size());
            allocations.append(id);
        }
    }
    auto leaderOf = [&](unsigned ordinal) {
        while (leaders[ordinal] != ordinal)
            ordinal = leaders[ordinal];
        return ordinal;
    };
    auto groupNamedBy = [&](ValueID operand) -> std::optional<unsigned> {
        ValueID array = analyses.sameObjectThroughCasts(operand).valueInFrontOfFacts();
        if (auto entry = ordinals.find(array); entry != ordinals.end())
            return leaderOf(entry->value);
        if (function.at(array).opcode != Opcode::Parameter || analyses.escape().gaveUp())
            return std::nullopt;
        const EscapeAnalysis& escape = analyses.escape();
        PointsToSet named = escape.pointsTo(array);
        if (named.mayBeUntracked || named.objects.isEmpty())
            return std::nullopt;
        for (ObjectID object : named.objects) {
            if (escape.at(object).kind != ObjectKind::Allocation || !ordinals.contains(escape.at(object).definition))
                return std::nullopt;
        }
        std::optional<unsigned> group;
        for (ObjectID object : named.objects) {
            unsigned leader = leaderOf(ordinals.get(escape.at(object).definition));
            if (group)
                leaders[std::max(*group, leader)] = std::min(*group, leader);
            group = group ? std::min(*group, leader) : leader;
        }
        return group;
    };

    Vector<std::tuple<ValueID, unsigned, bool>, 8> usesOfArrays;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        bool isInRareCode = function.at(block).frequency.predicted() == BlockFrequency::Rare;
        for (ValueID id : function.at(block).values) {
            if (!usesOfConstructedStorage.contains(function.at(id).opcode))
                continue;
            if (std::optional<unsigned> group = groupNamedBy(function.at(id).arrayOperand()))
                usesOfArrays.append({ id, *group, isInRareCode });
        }
    }

    // Two groups may have become one behind a use of either.
    Vector<Group, 2> groups(allocations.size());
    for (auto [id, group, isInRareCode] : usesOfArrays) {
        const Value& value = function.at(id);
        Group& uses = groups[leaderOf(group)];
        if (value.opcode == Opcode::TryPutByValDirect) {
            uses.directStores.append(id);
            if (!isInRareCode)
                uses.directStoresOutsideRareCode.append(id);
        } else if (value.opcode != Opcode::PutArrayElement && value.arrayStorage() == ArrayStorage::Double)
            continue;
        else if (readsNamingArrayStorage.contains(value.opcode) || value.arrayStorage() != ArrayStorage::Contiguous)
            uses.hasOtherUseOfStorage = true;
        else
            (value.opcode == Opcode::PutArrayElement ? uses.testedStores : uses.tests).append(id);
    }
    for (unsigned ordinal = 0; ordinal < allocations.size(); ++ordinal) {
        if (function.at(allocations[ordinal]).arrayStorage() != ArrayStorage::Other)
            groups[leaderOf(ordinal)].hasChosenStorage = true;
    }

    std::optional<BitVector> groupsReadAsDoubles;
    auto isReadAsDoubles = [&](unsigned group) {
        if (groupsReadAsDoubles)
            return groupsReadAsDoubles->quickGet(group);
        groupsReadAsDoubles.emplace(allocations.size());
        const EscapeAnalysis& escape = analyses.escape();
        for (BlockID block : escape.gaveUp() ? std::span<const BlockID> { } : analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& test = function.at(id);
                if (test.opcode != Opcode::HasArrayStorage || test.arrayStorage() != ArrayStorage::Double)
                    continue;
                for (ObjectID object : escape.pointsTo(test.arrayOperand()).objects) {
                    auto entry = escape.at(object).kind == ObjectKind::Allocation ? ordinals.find(escape.at(object).definition) : ordinals.end();
                    if (entry != ordinals.end())
                        groupsReadAsDoubles->quickSet(leaderOf(entry->value));
                }
            }
        }
        return groupsReadAsDoubles->quickGet(group);
    };
    auto setStorage = [&](const Vector<ValueID, 2>& values, ArrayStorage storage) {
        for (ValueID value : values)
            function.edit(value, ValueEdit::Definition).setArrayStorage(storage);
    };

    for (unsigned group = 0; group < groups.size(); ++group) {
        Group& uses = groups[group];
        if (uses.hasChosenStorage)
            continue;
        if (uses.directStores.isEmpty()) {
            if (uses.testedStores.isEmpty() || uses.hasOtherUseOfStorage)
                continue;
            // Only a store inside the length takes its value unseen. The others look for a NaN and leave it to the operation.
            bool holdsDoubles = std::ranges::all_of(uses.testedStores, [&](ValueID id) {
                const Value& store = function.at(id);
                const Type& stored = context.types.at(function.at(store.valueOperand()).type);
                return store.isInBounds() ? canStoreWithoutConversion(ArrayStorage::Double, stored) : stored.isNumber();
            });
            if (!holdsDoubles || !isReadAsDoubles(group))
                continue;
            uses.chosen = ArrayStorage::Double;
            setStorage(uses.testedStores, ArrayStorage::Double);
            setStorage(uses.tests, ArrayStorage::Double);
            continue;
        }
        // A direct store tests what it is given, so what rare code stores need not fit.
        const Vector<ValueID, 2>& decidingStores = uses.directStoresOutsideRareCode.isEmpty() ? uses.directStores : uses.directStoresOutsideRareCode;
        ArrayStorage kind = storageHolding(context, function.at(allocations[group]), decidingStores.span());
        // A reader that tests for doubles would convert each fresh array of integers and read it by the generic path.
        if (kind == ArrayStorage::Int32 && isReadAsDoubles(group))
            kind = ArrayStorage::Double;
        if (kind == ArrayStorage::Contiguous)
            continue;
        uses.chosen = kind;
        setStorage(uses.directStores, kind);
    }

    unsigned numberOfChangedAllocations = 0;
    for (unsigned ordinal = 0; ordinal < allocations.size(); ++ordinal) {
        ArrayStorage chosen = groups[leaderOf(ordinal)].chosen;
        if (chosen == ArrayStorage::Other)
            continue;
        function.edit(allocations[ordinal], ValueEdit::Definition).setArrayStorage(chosen);
        ++numberOfChangedAllocations;
    }
    return numberOfChangedAllocations;
}

static constexpr Effects effectsOfConversion = declaredBoundOf(Opcode::ConvertToDoubleStorage);

static constexpr Effects effectsOnConvertedArray = [] {
    Effects effects = Effects::none();
    effects.reads = effectsOfConversion.reads.restrictedTo(perObjectHeaps);
    effects.writes = effectsOfConversion.writes.restrictedTo(perObjectHeaps);
    return effects;
}();

static constexpr Effects effectsBesideConvertedArray = [] {
    Effects effects = effectsOfConversion;
    effects.writes.remove(perObjectHeaps);
    return effects;
}();

static bool allowsOnEveryExit(const FunctionSummaryView& summary, const Effects& effects)
{
    EffectsOnEdges allowed = summary.elsewhere();
    return effects.isSubsetOf(allowed.normal) && (!allowed.mayThrow() || effects.isSubsetOf(allowed.handler));
}

// A caller keeps what it has proven about the storage of an array across a call by the summary of the callee, and the summaries were made of the bodies without the conversions.
static bool summaryAllowsConversion(FunctionPassContext& context, const FunctionSummaryView& summary, bool functionMaySuspend, ValueID array)
{
    if (!allowsOnEveryExit(summary, effectsBesideConvertedArray))
        return false;
    bool isAllowed = true;
    forEachBaseObject(context.analyses, array, [&](const BaseObject& object) {
        switch (object.kind) {
        case BaseObject::Kind::OwnAllocation:
            // A saved frame holds the array, and the function goes on when its caller has long gone on.
            isAllowed = isAllowed && (!functionMaySuspend || allowsOnEveryExit(summary, effectsOnConvertedArray));
            return;
        case BaseObject::Kind::EntryParameter: {
            OperandMemory memory = summary.memory(object.entryParameter);
            isAllowed = isAllowed && effectsOnConvertedArray.reads.isSubsetOf(memory.reads) && effectsOnConvertedArray.writes.isSubsetOf(memory.writes);
            return;
        }
        case BaseObject::Kind::Untracked:
            isAllowed = isAllowed && allowsOnEveryExit(summary, effectsOnConvertedArray);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return isAllowed;
}

static bool flowAllowsConversion(FunctionPassContext& context, ValueID array)
{
    const Function& function = context.function;
    const FlowResults& flow = context.inputs.flow;
    const ClassPart* part = context.program.typeOf(function, array).singleBuiltinClass();
    if (part && part->arrayStorage != ArrayStorage::Other)
        return false;
    // Only flow calls contents proven, and only where it has seen every array that arrives. Those share what is decided about their storage: here, nothing.
    if (part && part->contentsAreProven)
        return true;
    bool isReceiver = false;
    for (const Use& use : context.analyses.useLists().usesOf(array)) {
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& access = function.at(use.user);
        if (!PayloadWord::accessSite(access.opcode) || OperandPosition::array(access.opcode) != use.operandIndex)
            continue;
        bool storageIsUndecided = Value::opcodesWithStorageIsUndecided.contains(access.opcode) && access.storageIsUndecided();
        if (!storageIsUndecided && !flow.mayChangeStorageOfReceiver(access.accessSite()))
            return false;
        isReceiver = true;
    }
    return isReceiver || flow.mayChangeStorageOfReceiver({ });
}

static bool convertsAtHead(const Function& function, BlockID block, ValueID array)
{
    const BlockValues& values = function.at(block).values;
    return !values.isEmpty() && function.at(values.first()).opcode == Opcode::ConvertToDoubleStorage && function.at(values.first()).arrayOperand() == array;
}

static const Value* findStorageTestBranchedOnBy(const Function& function, BlockID block)
{
    const Terminator& terminator = function.at(block).terminator;
    if (!terminator.isConditionalBranch())
        return nullptr;
    const Value& test = function.at(terminator.conditionOperand());
    return test.opcode == Opcode::HasArrayStorage ? &test : nullptr;
}

// The array has double storage there, and what stands behind the failing edge of a second test may rely on the first: a conversion on that edge would end its proof, and with it go all the conversions of the function.
static bool standsBehindPassedTest(FunctionPassContext& context, BlockID block, ValueID array)
{
    std::span<const BlockID> predecessors = context.analyses.cfg().predecessors(block);
    const Value* test = predecessors.size() == 1 ? findStorageTestBranchedOnBy(context.function, predecessors.front()) : nullptr;
    if (!test || test->arrayStorage() != ArrayStorage::Double || test->arrayOperand() != array)
        return false;
    const Terminator& terminator = context.function.at(predecessors.front()).terminator;
    return terminator.taken().block == block && terminator.notTaken().block != block;
}

// A copy has the storage of the array that it is made from. Once that one is converted, the copies to come pass the test.
static ValueID findCopiedArrayToConvert(FunctionPassContext& context, ValueID array)
{
    static constexpr std::array copyingBuiltins { Builtins::ArrayPrototypeSlice, Builtins::ArrayPrototypeConcat, Builtins::ArrayPrototypeToReversed, Builtins::ArrayPrototypeToSpliced, Builtins::ArrayPrototypeWith };
    const Program& program = context.program;
    const Value& copy = context.function.at(sameObjectThroughCasts(context.function, array));
    if (copy.opcode != Opcode::CallBuiltin)
        return { };
    BuiltinCall call = builtinCallOf(program, copy);
    BuiltinID builtin = program.builtins().canonicalID(program.builtins().at(call.overload).builtin);
    bool copies = call.receiver && std::ranges::any_of(copyingBuiltins, [&](BuiltinID other) {
        return builtin == program.builtins().canonicalID(other);
    });
    // A reader of that array would test for the same kind and convert it.
    const ClassPart* copied = copies ? program.typeOf(context.function, call.receiver).singleBuiltinClass() : nullptr;
    return copied && copied->arrayStorage == ArrayStorage::Other && arrayStorageDeclaredBy(program, *copied).predicted() == ArrayStorage::Double ? call.receiver : ValueID();
}

static Vector<FailedStorageTest, 4> collectFailedTestsForDoubleStorage(FunctionPassContext& context)
{
    const Function& function = context.function;
    CalleeSummary summary(context.analyses.effectContext(), context.id);
    bool functionMaySuspend = std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode);
        });
    });
    Vector<FailedStorageTest, 4> failedTests;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Value* test = findStorageTestBranchedOnBy(function, block);
        if (!test || test->arrayStorage() != ArrayStorage::Double)
            continue;
        ValueID array = test->arrayOperand();
        BlockID whenFailed = function.at(block).terminator.notTaken().block;
        // Whoever goes on to ask for another kind reads the array as it is.
        const Value* nextTest = findStorageTestBranchedOnBy(function, whenFailed);
        if (convertsAtHead(function, block, array) || convertsAtHead(function, whenFailed, array) || (nextTest && nextTest->arrayOperand() == array) || standsBehindPassedTest(context, block, array) || !flowAllowsConversion(context, array) || !summaryAllowsConversion(context, *summary, functionMaySuspend, array))
            continue;
        ValueID copied = findCopiedArrayToConvert(context, array);
        if (copied && (!flowAllowsConversion(context, copied) || !summaryAllowsConversion(context, *summary, functionMaySuspend, copied)))
            copied = { };
        failedTests.append(FailedStorageTest { Edge { block, 1 }, array, test->origin(), copied });
    }
    return failedTests;
}

// The code behind a failing edge is generic as a rule and relies on no proof about an array.
static void findConversionsEndingProofs(FunctionPassContext& context, Vector<FailedStorageTest, 4>& failedTests)
{
    if (failedTests.isEmpty())
        return;
    const Function& function = context.function;
    const GuardAvailability& guards = context.analyses.guards();
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        std::optional<GuardAvailability::Cursor> cursor;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& user = function.at(values[index]);
            if (reliesOf(user.opcode).isEmpty() || proofsNeededFromGuard(context.program, function, user).isEmpty())
                continue;
            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                ValueID guard = user.operand(operandIndex);
                if (user.operandKind(operandIndex) != OperandKind::Guard || !guard)
                    continue;
                if (!cursor)
                    cursor.emplace(guards, block);
                cursor->advanceTo(index);
                if (cursor->stateOf(guard, user) == GuardState::Available)
                    continue;
                for (FailedStorageTest& failed : failedTests)
                    failed.mayEndProof = failed.mayEndProof || guards.mayBeEndedAt(guard, user, failed.conversion);
            }
        }
    }
}

// Each copy costs a look at the whole function.
AOT_THRESHOLD(size_t, maxStorageTestsWithCopiedReads, 32);
AOT_THRESHOLD(size_t, maxReadsBehindStorageTest, 64);

struct ReadsBehindStorageTest {
    BlockID passed;
    ValueID guard;
    Vector<ValueID, 8> proofs;
    Vector<ValueID, 8> reads;
    Vector<ValueID, 4> repeatedTests;
    Vector<BlockID, 8> region;
};

static bool isBranchedOnIn(const Function& function, BlockID block, ValueID test)
{
    const Terminator& terminator = function.at(block).terminator;
    return terminator.isConditionalBranch() && terminator.conditionOperand() == test;
}

static bool isStorageProvenOnlyBy(FunctionAnalyses& analyses, ValueID test, const Value& read)
{
    auto locate = [&](ValueID parameter) {
        return analyses.useLists().locationOf(parameter);
    };
    bool hasOtherOrigin = false;
    forEachGuardOrigin(analyses.program(), analyses.function(), analyses.cfg(), locate, read.guardOperand(), [&](Proof proof, const GuardOrigin& origin) {
        hasOtherOrigin = proof == Proof::ArrayStorage && (origin.source != GuardSource::Test || origin.test != test);
        return hasOtherOrigin ? IterationStatus::Done : IterationStatus::Continue;
    });
    return !hasOtherOrigin;
}

// The proof of the test, the proofs made from it, and the reads that rely on them. Nothing else may: a store has other forms.
static bool collectReadsRelyingOn(FunctionPassContext& context, ValueID testID, ReadsBehindStorageTest& found)
{
    const Value& test = context.function.at(testID);
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    struct PassedProof {
        BlockID block;
        uint32_t index;
    };
    Vector<PassedProof, 4> passedProofs;
    found.proofs.append(found.guard);
    for (size_t next = 0; next < found.proofs.size(); ++next) {
        if (found.proofs.size() + found.reads.size() > maxReadsBehindStorageTest)
            return false;
        for (const Use& use : useLists.usesOf(found.proofs[next])) {
            if (use.kind == Use::Kind::EdgeArgument) {
                BlockID target = targetOf(function, use.edge()).block;
                ValueID parameter = function.at(target).parameters[use.operandIndex];
                if (!found.proofs.contains(parameter)) {
                    found.proofs.append(parameter);
                    passedProofs.append({ target, use.operandIndex });
                }
                continue;
            }
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = function.at(use.user);
            if (user.opcode == Opcode::AllGuards) {
                if (!found.proofs.contains(use.user))
                    found.proofs.append(use.user);
                continue;
            }
            if (!readsNamingArrayStorage.contains(user.opcode) || user.arrayOperand() != test.arrayOperand() || user.arrayStorage() != test.arrayStorage() || !isStorageProvenOnlyBy(context.analyses, testID, user))
                return false;
            if (found.reads.contains(use.user))
                continue;
            found.reads.append(use.user);
            if (user.opcode != Opcode::IsElementPresent)
                continue;
            for (const Use& useOfTest : useLists.usesOf(use.user)) {
                if (useOfTest.kind != Use::Kind::TerminatorOperand)
                    continue;
                if (!isBranchedOnIn(function, useOfTest.block, use.user) || numberOfEdgeResults(function, useOfTest.block, 0) > 1 || numberOfEdgeResults(function, useOfTest.block, 1))
                    return false;
                if (numberOfEdgeResults(function, useOfTest.block, 0))
                    found.proofs.append(function.at(function.at(useOfTest.block).terminator.taken().block).parameters.last());
            }
        }
    }
    // A proof that is handed on is joined with nothing but these.
    bool joinsTheseAlone = std::ranges::all_of(passedProofs, [&](const PassedProof& passed) {
        return std::ranges::all_of(context.analyses.cfg().incomingEdges(passed.block), [&](Edge edge) {
            return !edge.isHandlerEdge() && found.proofs.contains(targetOf(function, edge).arguments[passed.index]);
        });
    });
    return joinsTheseAlone && !found.reads.isEmpty();
}

// A validating build asks again each test that the proof stands for, and names the proof in no operand.
static void collectTestsRepeatedForValidation(FunctionPassContext& context, const Value& test, ReadsBehindStorageTest& found)
{
    if (!context.build.validates())
        return;
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    ValueID array = analyses.identityOf(test.arrayOperand());
    findReadsKeptForValidation(function).forEachSetBit([&](size_t index) {
        ValueID id(index);
        const Value& kept = function.at(id);
        if (kept.opcode != Opcode::HasArrayStorage || kept.arrayStorage() != test.arrayStorage() || analyses.identityOf(kept.arrayOperand()) != array || analyses.useLists().useCount(id) != 1)
            return;
        ValueLocation location = analyses.useLists().locationOf(id);
        if (analyses.dominators().dominates(found.passed, location.block) && analyses.guards().stateAt(found.guard, ProgramPoint { location.block, location.index }) == GuardState::Available)
            found.repeatedTests.append(id);
    });
}

// An element that is no number leaves where an absent one does: nothing has happened since that test. Answers the test.
static ValueID findPositionTestInFrontOf(FunctionPassContext& context, const ReadsBehindStorageTest& found, ValueID id)
{
    const Function& function = context.function;
    const Value& read = function.at(id);
    ValueLocation location = context.analyses.useLists().locationOf(id);
    std::span<const BlockID> predecessors = context.analyses.cfg().predecessors(location.block);
    bool isDense = pristineAspectsProvenBy(context.program.typeOf(function, read.arrayOperand()), BuiltinClasses::Array).contains(PristineAspects::Dense);
    if (predecessors.size() != 1 || !(read.isPresent() || (read.isInBounds() && isDense && !read.readsHoleAsNaN())))
        return { };
    const Terminator& terminator = function.at(predecessors.front()).terminator;
    if (!terminator.isConditionalBranch() || terminator.taken().block != location.block || terminator.notTaken().block == location.block || numberOfEdgeResults(function, predecessors.front(), 1))
        return { };
    const Value& test = function.at(terminator.conditionOperand());
    if (test.opcode == Opcode::IsElementPresent ? !found.reads.contains(terminator.conditionOperand()) : test.opcode != Opcode::IsInBounds || test.arrayOperand() != read.arrayOperand())
        return { };
    if (context.analyses.identityOf(test.positionOperand()) != context.analyses.identityOf(read.positionOperand()))
        return { };
    // What a validating build asks again changes nothing, and nobody but the validation takes the answer.
    auto isThereForValidationAlone = [&](ValueID earlier) {
        if (familyOf(function.at(earlier).opcode) == OperationFamily::RuntimeValidation)
            return true;
        std::span<const Use> uses = context.analyses.useLists().usesOf(earlier);
        return !uses.empty() && context.analyses.effectsOf(earlier).normal.isUnobservable() && std::ranges::all_of(uses, [&](const Use& use) {
            return use.kind == Use::Kind::Operand && familyOf(function.at(use.user).opcode) == OperationFamily::RuntimeValidation;
        });
    };
    bool nothingHasHappened = std::ranges::all_of(function.at(location.block).values.span().first(location.index), [&](ValueID earlier) {
        return found.proofs.contains(earlier) || isThereForValidationAlone(earlier);
    });
    return nothingHasHappened ? terminator.conditionOperand() : ValueID();
}

// What is asked of an element of Double storage may be answered by its type: `list[0] === null` is false whether there is an element or none. The read is gone then, and nothing is left of it.
// In storage that holds any value the answer is the same only if the list holds nothing but numbers and undefined, which a read that finds no element gives too.
static bool holdsNumbersInAnyStorage(FunctionPassContext& context, ValueID array)
{
    const Program& program = context.program;
    const ClassPart* part = program.typeOf(context.function, array).singleBuiltinClass();
    std::optional<ProvenValue<TypeID>> elements = part ? part->provenContentsAt(program.builtins(), ContentsSlot::Element) : std::nullopt;
    return elements && program.at(*elements).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
}

// Behind the test of its position an element of Double storage is no undefined either: `list[0] === undefined` is false, and the test is all that is left of the read.
// The copy asks an element where it reads it, so it reads behind each of these too.
static std::optional<Vector<ValueID, 4>> findPresenceTestsThatLostTheirRead(FunctionPassContext& context, const ReadsBehindStorageTest& found, const Vector<ValueID, 8>& testsInFrontOfReads)
{
    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, 4> tests;
    for (ValueID read : found.reads) {
        if (context.function.at(read).opcode != Opcode::IsElementPresent || testsInFrontOfReads.contains(read))
            continue;
        for (const Use& use : useLists.usesOf(read)) {
            if (use.kind != Use::Kind::TerminatorOperand)
                continue;
            // The branch is copied with the test.
            if (use.block != useLists.locationOf(read).block)
                return std::nullopt;
            tests.appendIfNotContains(read);
        }
    }
    return tests;
}

// A test of the bounds names no kind of storage, so it does not say whose read it has lost. There is no copy where a read may have relied on such a test together with the proof.
static bool mayHaveLostReadBehindBoundsTest(FunctionPassContext& context, BlockID tested, const Value& storageTest, const ReadsBehindStorageTest& found, const Vector<ValueID, 8>& testsInFrontOfReads)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.opcodesOfValues().contains(Opcode::IsInBounds))
        return false;
    ValueID array = analyses.identityOf(storageTest.arrayOperand());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch() || testsInFrontOfReads.contains(terminator.conditionOperand()))
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::IsInBounds || analyses.identityOf(test.arrayOperand()) != array)
            continue;
        BlockID inBounds = terminator.taken().block;
        if (analyses.dominators().dominates(inBounds, tested)) {
            // A proof that nobody relies on any more is no longer handed on.
            if (!numberOfEdgeResults(function, block, 0) || analyses.guards().stateAt(function.at(inBounds).parameters.last(), ProgramPoint::atHeadOf(found.passed)) != GuardState::Unavailable)
                return true;
        } else if (analyses.dominators().dominates(found.passed, block) && analyses.guards().stateAt(found.guard, ProgramPoint::atHeadOf(inBounds)) != GuardState::Unavailable)
            return true;
    }
    return false;
}

// The blocks between the passed test and the last use of its proof. A block that nothing of the kind follows is cut behind its last use.
static bool cutOutRegionOfReads(FunctionPassContext& context, IRBuilder& builder, ReadsBehindStorageTest& found)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    const UseLists& useLists = context.analyses.useLists();
    BitVector isInRegion(function.blocks.size());
    Vector<uint32_t> endOfUses(FillWith { }, function.blocks.size(), 0u);
    Vector<BlockID, 8> worklist;
    auto add = [&](BlockID block) {
        if (!isInRegion.set(block.index())) {
            worklist.append(block);
            found.region.append(block);
        }
    };
    bool isRepeated = false;
    auto addUse = [&](ValueID value) {
        ValueLocation location = useLists.locationOf(value);
        add(location.block);
        if (location.placement == ValuePlacement::InBlock)
            endOfUses[location.block.index()] = std::max(endOfUses[location.block.index()], location.index + 1);
        const NaturalLoop* loop = isRepeated ? nullptr : context.analyses.naturalLoops().innerMostLoopOf(location.block);
        isRepeated = isRepeated || (loop && isHotLoop(context, *loop).predicted());
    };
    std::ranges::for_each(found.proofs, addUse);
    std::ranges::for_each(found.reads, addUse);
    if (!isRepeated)
        return false;
    for (ValueID repeated : found.repeatedTests) {
        addUse(repeated);
        addUse(useLists.usesOf(repeated).front().user);
    }
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (block == found.passed)
            continue;
        for (BlockID predecessor : cfg.predecessors(block)) {
            if (!context.analyses.dominators().dominates(found.passed, predecessor))
                return false;
            add(predecessor);
        }
    }
    if (!isInRegion.get(found.passed.index()))
        return false;

    struct Cut {
        BlockID block;
        uint32_t index;
    };
    Vector<Cut, 4> cuts;
    for (BlockID block : found.region) {
        bool goesOn = std::ranges::any_of(cfg.successors(block), [&](BlockID successor) {
            return isInRegion.get(successor.index());
        }) || std::ranges::any_of(found.reads, [&](ValueID read) {
            return isBranchedOnIn(function, block, read);
        });
        if (!goesOn)
            cuts.append({ block, endOfUses[block.index()] });
    }
    for (const Cut& cut : cuts)
        builder.splitBlock(ProgramPoint { cut.block, cut.index });
    // The proof is a parameter of the first block, and the copy has a proof of its own.
    BlockID first = builder.splitBlock(ProgramPoint { found.passed, 0 });
    *std::ranges::find(found.region, found.passed) = first;
    std::swap(found.region.first(), *std::ranges::find(found.region, first));
    return true;
}

// General code makes a list of numbers with storage for any value, a list that has held a NaN keeps such storage, and a list that is written as numbers has Double storage. One reader meets them all.
// What reads the list behind the test for one kind is copied for the other, as far as the proof of the test goes. Out of storage for any value each element is asked whether it is a number.
// `failing` is the edge that an array of another kind goes along. Answers the block that asks for the second kind.
static BlockID addReadsOfOtherStorage(FunctionPassContext& context, IRBuilder& builder, BlockID tested, Edge failing)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Value test = *findStorageTestBranchedOnBy(function, tested);
    ArrayStorage other = test.arrayStorage() == ArrayStorage::Double ? ArrayStorage::Contiguous : ArrayStorage::Double;
    ReadsBehindStorageTest found { function.at(tested).terminator.taken().block, { }, { }, { }, { }, { } };
    if (numberOfEdgeResults(function, tested, 0) != 1 || analyses.cfg().predecessors(found.passed).size() != 1)
        return { };
    if (other == ArrayStorage::Contiguous && !holdsNumbersInAnyStorage(context, test.arrayOperand()))
        return { };
    found.guard = function.at(found.passed).parameters.last();
    if (!collectReadsRelyingOn(context, function.at(tested).terminator.conditionOperand(), found))
        return { };
    collectTestsRepeatedForValidation(context, test, found);
    Vector<ValueID, 8> testsInFrontOfReads;
    // Double storage is asked for where numbers are expected.
    auto fitsOtherStorage = [&](ValueID read) {
        if (other == ArrayStorage::Contiguous) {
            ValueID positionTest = findPositionTestInFrontOf(context, found, read);
            if (positionTest)
                testsInFrontOfReads.append(positionTest);
            return !!positionTest;
        }
        TypeID predicted = function.at(read).expected().predicted();
        const Type& type = predicted ? context.types.at(predicted) : context.program.typeOf(function, read);
        return type.withoutPrimitives(Primitive::Undefined).isNumber();
    };
    unsigned numberOfElementReads = 0;
    for (ValueID read : found.reads) {
        if (function.at(read).opcode != Opcode::GetArrayElement)
            continue;
        if (!fitsOtherStorage(read))
            return { };
        ++numberOfElementReads;
    }
    if (!numberOfElementReads)
        return { };
    std::optional<Vector<ValueID, 4>> testsThatLostTheirRead = Vector<ValueID, 4> { };
    if (other == ArrayStorage::Contiguous)
        testsThatLostTheirRead = findPresenceTestsThatLostTheirRead(context, found, testsInFrontOfReads);
    if (!testsThatLostTheirRead || !cutOutRegionOfReads(context, builder, found))
        return { };
    if (other == ArrayStorage::Contiguous && mayHaveLostReadBehindBoundsTest(context, tested, test, found, testsInFrontOfReads))
        return { };
    uint64_t bytes = 0;
    for (BlockID block : found.region)
        bytes += estimatedCodeBytes(context.program, function, function.at(block));
    if (bytes > context.limit(LimitID::MaxBytesOfSeparatedLoop) / 8 || RegionCloner::checkCanDuplicate(analyses, found.region.span(), found.region.span().first(1)) != CloneCheck::CanClone)
        return { };

    builder.setOrigin(test.origin());
    builder.setHandler(function.at(failing.from).handler);
    BlockID asksForOther = builder.addBlock(rarerOf(function.at(failing.from).frequency, BlockFrequency::Unlikely));
    builder.moveToEndOf(asksForOther);
    IRBuilder::ProvenBranch isOfOther = builder.emitBranchWithProof(builder.append(makeArrayStorageTest(test.arrayOperand(), other)), 0, BlockFrequency::Unlikely, BlockFrequency::Rare, { BlockID(), Target(targetOf(function, failing)) });
    builder.redirectEdge(failing, asksForOther, { });

    RegionCloner cloner(builder, function);
    cloner.mapValue(found.guard, isOfOther.provenOnTaken);
    cloner.cloneBlocks(found.region.span(), [&](ValueID original, Value& clone) {
        if (found.reads.contains(original) || found.repeatedTests.contains(original))
            clone.setArrayStorage(other);
        if (found.repeatedTests.contains(original))
            clone.setIsWritable(false);
    });
    for (BlockID block : cloner.clonedBlocks()) {
        Block& clone = function.edit(block, BlockEdit::Rest);
        clone.frequency = rarerOf(clone.frequency, BlockFrequency::Unlikely);
    }
    builder.moveToEndOf(isOfOther.taken);
    builder.emitJump(cloner.cloneOf(found.region.first()));
    cloner.repairSSA(analyses);

    Vector<ValueID, 8> copiedReads;
    for (ValueID original : found.reads) {
        if (function.at(original).opcode == Opcode::GetArrayElement)
            copiedReads.append(cloner.cloneOf(original));
    }
    for (ValueID original : *testsThatLostTheirRead) {
        Value presenceTest = function.at(cloner.cloneOf(original));
        Edge whenPresent { analyses.useLists().locationOf(cloner.cloneOf(original)).block, 0 };
        builder.setOrigin(presenceTest.origin());
        ValueID isPresent = ensureEdgeResult(builder, analyses, whenPresent, testedOperandIndex);
        ProgramPoint behindTest = ProgramPoint::atHeadOf(targetOf(function, whenPresent).block);
        Value read = IRBuilder::make(Opcode::GetArrayElement, { presenceTest.arrayOperand(), presenceTest.positionOperand() });
        read.setGuardOperand(builder.insertBefore(behindTest, IRBuilder::make(Opcode::AllGuards, { presenceTest.guardOperand(), isPresent })));
        read.setArrayStorage(other);
        read.setLevel(KnowledgeLevel::Expected);
        read.setKeyIsIntegral(presenceTest.keyIsIntegral());
        read.setIsPresent(true);
        copiedReads.append(builder.insertBefore(behindTest, WTF::move(read)));
        builder.applyInsertions();
    }

    // What a read gives follows its kind of storage.
    for (ValueID copied : copiedReads) {
        ValueLocation location = analyses.useLists().locationOf(copied);
        Target whenAbsent = function.at(analyses.cfg().predecessors(location.block).front()).terminator.targets.last();
        Value read = function.at(copied);
        read.setExpected({ });
        builder.setOrigin(read.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { location.block, location.index });
        ValueID element = builder.append(WTF::move(read));
        if (other == ArrayStorage::Double) {
            builder.endExpansion(expansion, element);
            continue;
        }
        IRBuilder::ProvenBranch isNumber = builder.emitBranchWithProof(builder.appendTypeTest(element, TypeTable::number), 0, BlockFrequency::Unlikely, BlockFrequency::Rare);
        builder.moveToEndOf(isNumber.taken);
        builder.endExpansion(expansion, isNumber.provenOnTaken);
        builder.moveToEndOf(isNumber.notTaken);
        builder.emitJump(whenAbsent.block, WTF::move(whenAbsent.arguments));
    }
    return asksForOther;
}

// An array of whole numbers that fails a test for Double storage is converted on the failing edge, so that it passes the test from then on.
static PassOutcome convertIntegerArraysBehindFailedTests(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::HasArrayStorage))
        return PassOutcome::Unchanged;
    // Splitting an edge invalidates the order of the blocks and what is known about the arrays, so every test is found before the first split.
    Vector<FailedStorageTest, 4> failedTests = collectFailedTestsForDoubleStorage(context);
    if (failedTests.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (FailedStorageTest& failed : failedTests) {
        failed.targetBeforeSplit = targetOf(function, failed.edge);
        failed.conversion = ProgramPoint::atHeadOf(builder.splitEdge(failed.edge));
        Block& block = function.edit(failed.conversion.block);
        block.frequency = rarerOf(block.frequency, BlockFrequency::Unlikely);
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
    }
    for (const FailedStorageTest& failed : failedTests) {
        builder.setOrigin(failed.origin);
        builder.insertBefore(failed.conversion, IRBuilder::make(Opcode::ConvertToDoubleStorage, { failed.array }));
        if (failed.copied)
            builder.insertBefore(failed.conversion, IRBuilder::make(Opcode::ConvertToDoubleStorage, { failed.copied }));
    }
    builder.applyInsertions();

    findConversionsEndingProofs(context, failedTests);
    unsigned numberOfConversions = 0;
    for (FailedStorageTest& failed : failedTests) {
        if (failed.mayEndProof)
            builder.redirectEdge(failed.edge, WTF::move(failed.targetBeforeSplit));
        else
            ++numberOfConversions;
    }
    // An array that holds what no Double storage holds is made from one that holds it too.
    for (const FailedStorageTest& failed : failedTests) {
        if (failed.mayEndProof || !failed.copied)
            continue;
        builder.setOrigin(failed.origin);
        IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { failed.conversion.block, 1 });
        IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(makeArrayStorageTest(failed.array, ArrayStorage::Double)), BlockFrequency::Rare, BlockFrequency::Unlikely);
        builder.moveToEndOf(blocks.taken);
        builder.append(Opcode::ConvertToDoubleStorage, { failed.copied });
        builder.endExpansion(expansion, std::span<const ValueID> { });
        builder.moveToEndOf(blocks.notTaken);
        builder.endExpansion(expansion, std::span<const ValueID> { });
    }
    if (numberOfConversions < failedTests.size())
        builder.emptyUnreachableBlocks();
    if (numberOfConversions)
        context.build.statistics().count(StatisticID::IntegerArrayConversionsPlaced, numberOfConversions);
    // An edge that got its target back has left an empty block behind.
    return PassOutcome::Changed;
}

static PassOutcome readListsOfNumbersInEitherStorage(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::HasArrayStorage))
        return PassOutcome::Unchanged;
    Vector<BlockID, 4> tests;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Value* test = findStorageTestBranchedOnBy(function, block);
        if (!test || (test->arrayStorage() != ArrayStorage::Double && test->arrayStorage() != ArrayStorage::Contiguous))
            continue;
        if (context.analyses.blockFrequencies().classOf(block).predicted() != BlockFrequency::Normal || isCold(heatClassOf(context, block)).predicted())
            continue;
        const Value* nextTest = findStorageTestBranchedOnBy(function, function.at(block).terminator.notTaken().block);
        if (!nextTest || nextTest->arrayOperand() != test->arrayOperand())
            tests.append(block);
        if (tests.size() == maxStorageTestsWithCopiedReads)
            break;
    }

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    bool didChange = false;
    Vector<FailedStorageTest, 4> converted;
    for (BlockID block : tests) {
        // An earlier copy may have cut the block.
        const Value* test = findStorageTestBranchedOnBy(function, block);
        if (!test)
            continue;
        ValueID array = test->arrayOperand();
        Origin origin = test->origin();
        BlockID whenFailed = function.at(block).terminator.notTaken().block;
        // A list that is converted is read as fast as any from then on, so the conversion stays in front. The copy of an earlier test may hold a copy of this one, which fails into the same block.
        bool convertsFirst = convertsAtHead(function, whenFailed, array);
        if (convertsFirst && (function.at(whenFailed).values.size() != 1 || function.at(whenFailed).terminator.kind != TerminatorKind::Jump || context.analyses.cfg().predecessors(whenFailed).size() != 1))
            continue;
        BlockID asksForOther = addReadsOfOtherStorage(context, builder, block, convertsFirst ? Edge { whenFailed, 0 } : Edge { block, 1 });
        if (!asksForOther)
            continue;
        didChange = true;
        if (convertsFirst)
            converted.append(FailedStorageTest { Edge { asksForOther, 1 }, array, origin, { }, { }, ProgramPoint::atHeadOf(whenFailed) });
    }
    // The copy comes back into code that relies on proofs. Where the conversion in front of it may end one, there is no copy.
    findConversionsEndingProofs(context, converted);
    bool didRemoveCopy = false;
    for (const FailedStorageTest& failed : converted) {
        if (!failed.mayEndProof)
            continue;
        builder.convertBranchToJump(failed.edge.from, failed.edge.targetIndex);
        didRemoveCopy = true;
    }
    if (didRemoveCopy)
        builder.emptyUnreachableBlocks();
    return didChange ? PassOutcome::Changed : PassOutcome::Unchanged;
}

PassOutcome planCollectionCapacities(FunctionPassContext& context)
{
    unsigned planned = applyGrowthPlans(context, CapacityPlanner { context }.collectCollectionGrowthPlans());
    if (!planned)
        return PassOutcome::Unchanged;
    context.build.statistics().count(StatisticID::CollectionGrowthsPlanned, planned);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

PassOutcome planArrayCapacities(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().intersects(arrayCapacityOpcodes))
        return PassOutcome::Unchanged;

    PassOutcome converted = convertIntegerArraysBehindFailedTests(context);
    unsigned constructed = chooseStorageOfConstructedArrays(context);
    // Behind the choice, which goes by the kind that the readers expect.
    if (readListsOfNumbersInEitherStorage(context) == PassOutcome::Changed)
        converted = PassOutcome::Changed;
    Vector<CapacityPlan, 4> plans = CapacityPlanner { context }.collectPlans();
    if (unsigned planned = applyCapacityPlans(context, plans))
        context.build.statistics().count(StatisticID::ArrayCapacitiesPlanned, planned);
    unsigned grown = applyGrowthPlans(context, CapacityPlanner { context }.collectGrowthPlans(Opcode::ReserveArrayCapacity, &CapacityPlanner::planGrowth));
    if (grown)
        context.build.statistics().count(StatisticID::ArrayGrowthsPlanned, grown);
    if (converted == PassOutcome::Unchanged && !constructed && plans.isEmpty() && !grown)
        return PassOutcome::Unchanged;

    if (constructed)
        context.analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
    context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
