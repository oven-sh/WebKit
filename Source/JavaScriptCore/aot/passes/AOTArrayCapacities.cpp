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
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeTestSite.h"
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
                if ((!frequencies || !(frequencies->classOf(start) < frequencies->classOf(successor))) && !isReached.quickSet(successor.index()))
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
            block = chainSuccessorOf(block);
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
            if (frequencies.classOf(point.block) < frequencies.classOf(site.block) || !mayFollowLiteral(site))
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
            } else if (!isInOtherCycle && loop && likelyOuterLoopOf(*loop) == loopOfLiteral) {
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
    const NaturalLoop* likelyOuterLoopOf(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
        auto isClosedByLikelyPath = [&](const NaturalLoop& outer) {
            return std::ranges::any_of(analyses.cfg().incomingEdges(outer.header()), [&](Edge edge) {
                return loops.belongsTo(edge.from, outer) && !(frequencyOfLoop < frequencies.classOf(edge.from));
            });
        };
        const NaturalLoop* outer = loops.innerMostOuterLoop(loop);
        while (outer && !isClosedByLikelyPath(*outer))
            outer = loops.innerMostOuterLoop(*outer);
        return outer;
    }

    BlockID chainSuccessorOf(BlockID block, unsigned numberOfBranchesAround = 0)
    {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return terminator.next().block;
        return terminator.kind == TerminatorKind::Branch ? blockWhereLikelyArmsMeet(block, numberOfBranchesAround) : BlockID();
    }

    BlockID blockWhereLikelyArmsMeet(BlockID block, unsigned numberOfBranchesAround)
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
            if (!(frequencies.classOf(successor) < frequencies.classOf(block)) && !likely.contains(successor))
                likely.append(successor);
        }
        BlockID result;
        if (likely.size() == 1)
            result = likely[0];
        else if (likely.size() == 2) {
            Vector<BlockID, maxBlocksOfArm> firstArm;
            for (BlockID next = likely[0]; next && firstArm.size() < maxBlocksOfArm && !firstArm.contains(next); next = chainSuccessorOf(next, numberOfBranchesAround + 1))
                firstArm.append(next);
            unsigned numberOfBlocks = 0;
            for (BlockID next = likely[1]; next && numberOfBlocks++ < maxBlocksOfArm; next = chainSuccessorOf(next, numberOfBranchesAround + 1)) {
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
            if (!isEnteredOnLikelyPathsFrom(literal.point.block, loop) || !runsOncePerIteration(loop, sites[i].block))
                return false;
        }
        const NaturalLoop& firstLoop = loopOf(sites[0]);
        if (sites.size() > 1 && !areAlternativesTo(firstLoop.header(), literal, sites.subspan(1)))
            return false;
        TripCount count = tripCount(literal, firstLoop, sites[0]);
        plan.constant += count.adjustment;
        return plan.tryAdd(count.added) && plan.tryAdd(count.subtracted, true);
    }

    bool isEnteredOnLikelyPathsFrom(BlockID block, const NaturalLoop& loop)
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
                if (frequencies.classOf(block) < frequencies.classOf(edge.from) || loops.belongsTo(edge.from, loop))
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
    bool anyLikelyBlockOf(const NaturalLoop& loop, NOESCAPE const Invocable<bool(BlockID)> auto& predicate)
    {
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
        for (unsigned i = 0; i < loop.size(); ++i) {
            if (!(frequencyOfLoop < frequencies.classOf(loop.at(i))) && predicate(loop.at(i)))
                return true;
        }
        return false;
    }

    bool runsOncePerIteration(const NaturalLoop& loop, BlockID block)
    {
        if (block == loop.header())
            return true;
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
        if (frequencyOfLoop < frequencies.classOf(block))
            return false;
        BitVector barrier;
        barrier.set(block.index());
        for (unsigned i = 0; i < loop.size(); ++i) {
            if (frequencyOfLoop < frequencies.classOf(loop.at(i)))
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
        return analyses.dominates(measured.owner, point) && !mayBeUnspecified(analyses, measured.owner) && holdsAt(measured.guard, point);
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
        return check && !function.at(point.block).handler && !mayBeUnspecified(analyses, measured.owner)
            && analyses.dominates(function.at(check).valueOperand(), point) && followsWithoutVisibleEffect(point, check);
    }

    bool mayChangeLengthIn(const NaturalLoop& loop, const MeasuredStorage& measured, ValueID literal)
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
    bool mayTakeElementsAwayIn(const NaturalLoop& loop, ValueID array)
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
    bool hasOnlyRareSideExits(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
        return !anyLikelyBlockOf(loop, [&](BlockID block) {
            const Terminator& terminator = function.at(block).terminator;
            return block != loop.header() && (returnsToCaller(terminator.kind) || std::ranges::any_of(terminator.targets, [&](const Target& target) {
                return !loops.belongsTo(target.block, loop) && !(frequencyOfLoop < frequencies.classOf(target.block));
            }));
        });
    }

    // The step through a Map or a Set in the header of a loop that is left only where the step finds no further entry.
    ValueID findStepControlling(const NaturalLoop& loop)
    {
        const NaturalLoops& loops = analyses.naturalLoops();
        const Terminator& terminator = function.at(loop.header()).terminator;
        if (terminator.kind != TerminatorKind::Branch || !hasOnlyRareSideExits(loop) || !loops.belongsTo(terminator.taken().block, loop) || loops.belongsTo(terminator.notTaken().block, loop))
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
        if (!canMeasureAt(measured, literal.point) || mayChangeLengthIn(loop, measured, literal.value))
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
                if (test.exits == LoopExits::OnlyAtTest || (test.block == loop.header() && hasOnlyRareSideExits(loop)))
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
    ValueID valueOnLikelyEdges(ValueID id)
    {
        if (function.at(id).opcode != Opcode::Parameter)
            return id;
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        BlockID block = analyses.useLists().blockOf(id);
        uint32_t parameterIndex = static_cast<uint32_t>(function.at(block).parameters.find(id));
        ValueID given;
        for (Edge edge : analyses.cfg().incomingEdges(block)) {
            if (frequencies.classOf(block) < frequencies.classOf(edge.from))
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
        id = valueOnLikelyEdges(analyses.sameObjectThroughCasts(id));
        KnownValue known = knownValueOf(context.program, function, id);
        if (known.kind() == KnownValue::Kind::Integer)
            return plannedTermRange.contains(known.asInteger()) ? CapacityTerm { CapacityTermKind::Constant, { }, known.asInteger() } : CapacityTerm { };

        ValueID definition = analyses.sameObjectThroughCasts(id);
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
            if (!outermost || !mayChangeLengthIn(*outermost, *measured, literal.value))
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
        if (analyses.blockFrequencies().classOf(point.block) == BlockFrequency::Rare || !program.typeOf(function, array).isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program))
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
        if (!runsOncePerIteration(loop, point.block))
            return std::nullopt;
        TripCount count = iterationsAt(point, loop, array, CountedIterations::Remaining);
        if (count.added.kind == CapacityTermKind::Unknown || count.subtracted.kind == CapacityTermKind::Unknown)
            return std::nullopt;
        if (mayTakeElementsAwayIn(loop, array))
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
        if ((implementation != InlineBuiltin::MapSet && implementation != InlineBuiltin::SetAdd) || analyses.blockFrequencies().classOf(point.block) == BlockFrequency::Rare)
            return std::nullopt;
        ValueID collection = addition.receiverOperand();
        BlockID preheader = findPreheader(loop);
        if (!preheader || !isInvariantIn(analyses, loop, collection) || mayBeUnspecified(analyses, collection))
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
        BlockFrequency frequency = function.at(plan.point.block).frequency;
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
    if (kind == ArrayStorage::Int32 && allocation.expected().predicted() && arrayStorageDeclaredBy(context.program, context.types.at(allocation.expected().predicted())) == ArrayStorage::Double)
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
        ValueID array = analyses.sameObjectThroughCasts(operand);
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
        bool isInRareCode = function.at(block).frequency == BlockFrequency::Rare;
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
            bool holdsDoubles = std::ranges::all_of(uses.testedStores, [&](ValueID store) {
                return canStoreWithoutConversion(ArrayStorage::Double, context.types.at(function.at(function.at(store).valueOperand()).type));
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
    static constexpr OpcodeSet accessesOfElements { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::HasArrayElement };
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
        if (use.kind != Use::Kind::Operand || !accessesOfElements.contains(function.at(use.user).opcode) || function.at(use.user).arrayOperand() != array)
            continue;
        if (!flow.mayChangeStorageOfReceiver(function.at(use.user).accessSite()))
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
    return copied && copied->arrayStorage == ArrayStorage::Other && arrayStorageDeclaredBy(program, *copied) == ArrayStorage::Double ? call.receiver : ValueID();
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
