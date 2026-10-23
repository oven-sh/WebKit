/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGenericCopy.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTMethodDispatch.h"
#include "AOTPass.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTSinkAllocations.h"
#include "AOTSort.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool isReadAsDefinition(const Program& program, const Function& function, const Value& value)
{
    if (numberOfResultsOf(program, function, value))
        return true;
    std::optional<OpaqueKind> opaque = value.type ? program.at(value.type).opaque : std::nullopt;
    return opaque && mustBeDefinition(operandKindOf(*opaque));
}

static std::optional<uint32_t> endOfReadsInBlock(const UseLists& useLists, ValueID producer, BlockID block, uint32_t numberOfValues)
{
    uint32_t end = 0;
    for (const Use& use : useLists.usesOf(producer)) {
        if (use.block != block)
            return std::nullopt;
        switch (use.kind) {
        case Use::Kind::Operand:
            end = std::max(end, useLists.locationOf(use.user).index + 1);
            break;
        case Use::Kind::TerminatorOperand:
            end = numberOfValues;
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return std::nullopt;
        }
    }
    return end;
}

// Also a call that nobody has resolved yet: its callee is the function itself.
static BuiltinID findCalledBuiltin(const BuiltinTable& builtins, const Function& function, const Value& call)
{
    if (call.opcode == Opcode::Call) {
        const Value& callee = function.at(call.calleeOperand());
        return callee.opcode == Opcode::Builtin ? builtins.canonicalID(callee.builtin()) : BuiltinID();
    }
    if (call.opcode != Opcode::CallBuiltin || (call.builtinCallForm() != BuiltinCallForm::Whole && call.builtinCallForm() != BuiltinCallForm::AnyReceiver))
        return { };
    const BuiltinOverload& overload = builtins.at(call.overload());
    return overload.mode == BuiltinCallMode::Call ? builtins.canonicalID(overload.builtin) : BuiltinID();
}

// Object.defineProperties(target, Object.getOwnPropertyDescriptors(source)), where the descriptors go nowhere else: SimplifyBuiltinCalls makes one operation of the two calls where it finds the first in the block of the second.
static std::optional<uint32_t> endOfDefinitionByDescriptors(const Program& program, const Function& function, const UseLists& useLists, ValueID description, BlockID block)
{
    const BuiltinTable& builtins = program.builtins();
    if (findCalledBuiltin(builtins, function, function.at(description)) != Builtins::ObjectGetOwnPropertyDescriptors)
        return std::nullopt;
    for (ValueID link = description;;) {
        std::optional<Use> use = useLists.singleUse(link);
        if (!use || use->kind != Use::Kind::Operand || use->block != block)
            return std::nullopt;
        const Value& user = function.at(use->user);
        if (isCast(user.opcode)) {
            link = use->user;
            continue;
        }
        if (findCalledBuiltin(builtins, function, user) != Builtins::ObjectDefineProperties)
            return std::nullopt;
        std::span<const ValueID> arguments = user.opcode == Opcode::Call ? user.variadicOperands() : user.argumentsOperands();
        if (arguments.size() != 2 || arguments[1] != link)
            return std::nullopt;
        return useLists.locationOf(use->user).index + 1;
    }
}

static bool isReadOnlyBefore(const UseLists& useLists, ValueID producer, ProgramPoint end)
{
    return std::ranges::all_of(useLists.usesOf(producer), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && use.block == end.block && useLists.locationOf(use.user).index < end.index;
    });
}

static bool canCopyInsideFunction(CloneCheck check)
{
    switch (check) {
    case CloneCheck::CanClone:
    case CloneCheck::FunctionMayNotBeCloned:
        return true;
    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ProgramPoint> findRejoinPoints(FunctionAnalyses& analyses, NOESCAPE const ScopedLambda<bool(ValueID)>& mayCallUnknownCode, NOESCAPE const ScopedLambda<bool(ProgramPoint)>& mayRejoinAt)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    const NaturalLoops* loops = cfg.hasCycle() ? &analyses.naturalLoops() : nullptr;
    const UseLists& useLists = analyses.useLists();

    Vector<ProgramPoint> points;
    BitVector isVisitedHead(function.blocks.size());
    auto addHead = [&](BlockID block) {
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        if (isVisitedHead.add(block.index()) && mayRejoinAt(head))
            points.append(head);
    };

    Vector<uint32_t, 32> endOfEarlierReads;
    for (BlockID block : cfg.reversePostOrder()) {
        const Block& contents = function.at(block);
        if (contents.isHandlerBlock() || (loops && loops->headerOf(block)))
            addHead(block);

        const BlockValues& values = contents.values;
        uint32_t numberOfValues = values.size();
        endOfEarlierReads.shrink(0);
        uint32_t lastPoint = 0;
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID call = values[index];
            if (!mayCallUnknownCode(call))
                continue;

            if (endOfEarlierReads.isEmpty()) {
                uint32_t end = 0;
                for (ValueID producer : values) {
                    if (isReadAsDefinition(program, function, function.at(producer)))
                        end = std::max(end, endOfReadsInBlock(useLists, producer, block, numberOfValues).value_or(0));
                    else
                        end = std::max(end, endOfDefinitionByDescriptors(program, function, useLists, producer, block).value_or(0));
                    endOfEarlierReads.append(end);
                }
            }
            uint32_t pointIndex = index + 1;
            while (endOfEarlierReads[pointIndex - 1] > pointIndex)
                pointIndex = endOfEarlierReads[pointIndex - 1];
            if (pointIndex == lastPoint)
                continue;
            if (numberOfResultsOf(program, function, function.at(call)) && !isReadOnlyBefore(useLists, call, { block, pointIndex }))
                continue;

            if (pointIndex == numberOfValues) {
                for (const Target& target : contents.terminator.targets)
                    addHead(target.block);
                break;
            }
            lastPoint = pointIndex;
            ProgramPoint point { block, pointIndex };
            if (mayRejoinAt(point))
                points.append(point);
        }
    }

    AOT::sort(points, { }, [&](ProgramPoint point) {
        return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
    });
    return points;
}

GenericCopy::GenericCopy(FunctionPassContext& context, IRBuilder& builder)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_builder(builder)
{
    RELEASE_ASSERT(&builder.function() == &m_function);
}

void GenericCopy::addCopiedBlock(BlockID block)
{
    m_requiredBlocks.append(block);
}

void GenericCopy::addStart(BlockID block)
{
    m_starts.append(block);
}

void GenericCopy::addEntry(BlockID block)
{
    m_entries.append(block);
}

void GenericCopy::addRejoinBlock(BlockID block)
{
    m_isRejoinBlock.set(block.index());
}

void GenericCopy::addRejoinBlockWithoutTests(BlockID block)
{
    m_isRejoinBlockWithoutTests.set(block.index());
}

void GenericCopy::addValueToBeNarrowed(ValueID value)
{
    m_valuesToBeNarrowed.append(value);
}

void GenericCopy::addValueNarrowedInUnknownWay(ValueID value)
{
    m_valuesNarrowedInUnknownWay.append(value);
}

void GenericCopy::addValueKnownBeyondItsType(ValueID value)
{
    m_valuesKnownBeyondTheirTypes.append(value);
}

bool GenericCopy::isRare(BlockID block) const
{
    return m_function.at(block).frequency == BlockFrequency::Rare;
}

void GenericCopy::computeBlocksReachingNonRareCode()
{
    m_reachesNonRareCode.clearAll();
    m_reachesNonRareCode.ensureSize(m_function.blocks.size());
    std::span<const BlockID> order = m_analyses.cfg().reversePostOrder();
    for (bool changed = true; std::exchange(changed, false);) {
        for (BlockID block : order | std::views::reverse) {
            if (m_reachesNonRareCode.get(block.index()))
                continue;
            m_function.at(block).forEachTarget([&](const Target& target) {
                if (!isRare(target.block) || m_reachesNonRareCode.get(target.block.index()))
                    changed |= m_reachesNonRareCode.add(block.index());
            });
        }
    }
}

void GenericCopy::computeNarrowedValues()
{
    const UseLists& useLists = m_analyses.useLists();
    m_mayBeNarrowed.clearAll();
    m_mayBeNarrowed.ensureSize(m_function.values.size());

    Vector<ValueID, 16> worklist;
    auto add = [&](ValueID value) {
        if (m_mayBeNarrowed.add(value.index()))
            worklist.append(value);
    };
    auto addEdgeResults = [&](BlockID from) {
        const Terminator& terminator = m_function.at(from).terminator;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            const CompactList<ValueID>& parameters = m_function.at(terminator.targets[targetIndex].block).parameters;
            uint32_t first = firstEdgeResultParameter(m_function, from, targetIndex);
            uint32_t end = first + numberOfEdgeResults(m_function, from, targetIndex);
            for (uint32_t index = first; index < end && index < parameters.size(); ++index)
                add(parameters[index]);
        }
    };

    for (ValueID value : m_valuesToBeNarrowed)
        add(value);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            switch (use.kind) {
            case Use::Kind::Operand:
                add(use.user);
                break;
            case Use::Kind::TerminatorOperand:
                addEdgeResults(use.block);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument: {
                const CompactList<ValueID>& parameters = m_function.at(targetOf(m_function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    add(parameters[use.operandIndex]);
                break;
            }
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
}

BitVector findValuesTypedCodeKnowsMoreAbout(FunctionAnalyses& analyses, const TypeArena& types, const ValuesNarrowedByTests& narrowed)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    BitVector isKnownBetter(function.values.size());
    BitVector isNarrowedInUnknownWay(function.values.size());

    enum class Role : uint8_t { Result, ProofOfTest };
    Vector<ValueID, 16> worklist;
    auto add = [&](ValueID id, bool restsOnUnknownNarrowing, Role role = Role::Result) {
        const Value& value = function.at(id);
        if (!value.type)
            return;
        bool hasOpenExpectation = value.expected() && value.expected() != value.type;
        bool isUnknown = false;
        if (!hasOpenExpectation) {
            const Type& type = types.at(value.type);
            if (!type.isJavaScriptValue())
                return;
            bool isAnswerOrProof = role == Role::ProofOfTest || type.hasOnlyPrimitives(Primitive::Boolean);
            if (!isAnswerOrProof && (!restsOnUnknownNarrowing || type.isInt32()))
                return;
            isUnknown = restsOnUnknownNarrowing;
        }
        bool changed = isKnownBetter.add(id.index());
        if (isUnknown)
            changed |= isNarrowedInUnknownWay.add(id.index());
        if (changed)
            worklist.append(id);
    };
    auto addEdgeResults = [&](BlockID from, bool restsOnUnknownNarrowing) {
        const Terminator& terminator = function.at(from).terminator;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            const CompactList<ValueID>& parameters = function.at(terminator.targets[targetIndex].block).parameters;
            uint32_t first = firstEdgeResultParameter(function, from, targetIndex);
            uint32_t end = first + numberOfEdgeResults(function, from, targetIndex);
            for (uint32_t index = first; index < end && index < parameters.size(); ++index)
                add(parameters[index], restsOnUnknownNarrowing, Role::ProofOfTest);
        }
    };

    for (ValueID value : narrowed.asExpected)
        add(value, false);
    for (ValueID value : narrowed.inUnknownWay)
        add(value, true);
    while (!worklist.isEmpty()) {
        ValueID source = worklist.takeLast();
        bool isUnknown = isNarrowedInUnknownWay.get(source.index());
        for (const Use& use : useLists.usesOf(source)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                add(use.user, isUnknown);
                break;
            case Use::Kind::TerminatorOperand:
                addEdgeResults(use.block, isUnknown);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument: {
                const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    add(parameters[use.operandIndex], isUnknown);
                break;
            }
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    for (ValueID value : narrowed.knownBeyondTheirTypes)
        isKnownBetter.set(value.index());
    return isKnownBetter;
}

bool GenericCopy::hasValueAtHead(BlockID block, const BitVector& values, AnswersByBlock& answers) const
{
    if (answers.isKnown.get(block.index()))
        return answers.answer.get(block.index());
    auto isAmongValues = [&](ValueID value) {
        return values.get(value.index());
    };
    bool answer = std::ranges::any_of(m_function.at(block).parameters, isAmongValues) || std::ranges::any_of(m_analyses.liveness().liveAtHead(block), isAmongValues);
    answers.isKnown.set(block.index());
    answers.answer.set(block.index(), answer);
    return answer;
}

bool GenericCopy::knowsMoreInTypedCodeAtHead(BlockID block) const
{
    return hasValueAtHead(block, m_isKnownBetterInTypedCode, m_knowsMoreInTypedCodeAtHead);
}

bool GenericCopy::hasNarrowedValueAtHead(BlockID block) const
{
    return hasValueAtHead(block, m_mayBeNarrowed, m_hasNarrowedValueAtHead);
}

void GenericCopy::computeRegion(Extent extent)
{
    size_t numberOfBlocks = m_function.blocks.size();
    m_region.shrink(0);
    m_isInRegion.clearAll();
    m_isInRegion.ensureSize(numberOfBlocks);
    m_exits.shrink(0);
    m_untestedRejoins.shrink(0);
    m_testedRejoinBlocks.shrink(0);
    m_keepsAllocationsApart.clearAll();

    auto add = [&](BlockID block) {
        if (!m_isInRegion.add(block.index()))
            return false;
        m_region.append(block);
        return true;
    };

    Vector<BlockID, 16> worklist;
    for (BlockID block : m_starts) {
        if (add(block))
            worklist.append(block);
    }
    if (extent == Extent::OnlyCopiedBlocks)
        worklist.shrink(0);

    bool comesBack = extent == Extent::ToWaysBack;
    bool comesBackWithoutTests = comesBack && !m_isRejoinBlockWithoutTests.isEmpty();
    BlockFrequency frequencyOfStarts = BlockFrequency::Rare;
    for (BlockID block : m_starts)
        frequencyOfStarts = std::min(frequencyOfStarts, m_function.at(block).frequency);
    BitVector isExit(numberOfBlocks);
    while (!worklist.isEmpty()) {
        m_function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            BlockID block = target.block;
            if (isInRegion(block) || isExit.get(block.index()))
                return;
            bool isRarerRejoinBlock = m_leavesAtRarerRejoinBlocks && m_isRejoinBlock.get(block.index()) && m_function.at(block).frequency > frequencyOfStarts;
            bool isRejoinBlock = isRarerRejoinBlock || (comesBack && m_isRejoinBlock.get(block.index()));
            bool endsInRareCode = isRare(block) && !m_reachesNonRareCode.get(block.index());
            if ((isRejoinBlock || endsInRareCode) && !m_mustBeCopied.get(block.index()) && (isRarerRejoinBlock || !hasNarrowedValueAtHead(block))) {
                isExit.set(block.index());
                m_exits.append(block);
                if (isRejoinBlock)
                    m_untestedRejoins.append(block);
                return;
            }
            bool needsNoTest = comesBackWithoutTests && (m_isRejoinBlockWithoutTests.get(block.index()) || m_isRejoinBlock.get(block.index())) && !knowsMoreInTypedCodeAtHead(block);
            if (needsNoTest && !m_mustBeCopied.get(block.index())) {
                isExit.set(block.index());
                m_exits.append(block);
                m_untestedRejoins.append(block);
                m_keepsAllocationsApart.set(block.index());
                return;
            }
            add(block);
            worklist.append(block);
            if (isRejoinBlock && !endsInRareCode && !m_function.at(block).isHandlerBlock())
                m_testedRejoinBlocks.append(block);
        });
    }
    m_numberOfBlocksFromStarts = m_region.size();

    for (BlockID block : m_requiredBlocks)
        add(block);
    auto isCopied = [&](BlockID block) {
        return isInRegion(block);
    };
    m_exits.removeAllMatching(isCopied);
    m_untestedRejoins.removeAllMatching(isCopied);
}

// If the copy can reach a loop's copied header from outside the loop, an edge from the copy into the loop's body would bypass the header.
bool GenericCopy::addsLoopEntry(BlockID block) const
{
    if (!m_analyses.isInCycle(block))
        return false;
    const CFG& cfg = m_analyses.cfg();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const Block& contents = m_function.at(block);
    bool onlyJumps = contents.values.isEmpty() && contents.terminator.kind == TerminatorKind::Jump;
    for (const NaturalLoop* loop = loops.innerMostLoopOf(block); loop; loop = loops.innerMostOuterLoop(*loop)) {
        BlockID header = loop->header();
        if (header == block || !isInRegion(header))
            continue;
        if (onlyJumps && contents.terminator.next().block == header && !m_isRejoinBlock.get(header.index()))
            continue;
        for (Edge edge : cfg.incomingEdges(header)) {
            if (isInRegion(edge.from) && !loops.belongsTo(edge.from, *loop))
                return true;
        }
    }
    return false;
}

bool GenericCopy::mergesAllocationWithItsCopyAt(BlockID block) const
{
    const UseLists& useLists = m_analyses.useLists();
    return std::ranges::any_of(m_analyses.liveness().liveAtHead(block), [&](ValueID value) {
        ValueID identity = m_analyses.identityOf(value);
        Opcode opcode = m_function.at(identity).opcode;
        if (resultRuleOf(opcode) != ResultRule::Allocation && opcode != Opcode::NewClosure)
            return false;
        BlockID definingBlock = useLists.blockOf(identity);
        return definingBlock && isInRegion(definingBlock);
    });
}

// An allocation that sinks is built in the tested code only where something takes the object itself, or nowhere. Where it met its copy it would be built in front of the meeting, each time the tested code runs.
bool GenericCopy::mergesAllocationThatSinksWithItsCopyAt(BlockID block) const
{
    const UseLists& useLists = m_analyses.useLists();
    return std::ranges::any_of(m_analyses.liveness().liveAtHead(block), [&](ValueID value) {
        ValueID identity = m_analyses.identityOf(value);
        if (interpretedOf(m_function.at(identity).opcode) != Interpretation::Allocation)
            return false;
        BlockID definingBlock = useLists.blockOf(identity);
        if (!definingBlock || !isInRegion(definingBlock))
            return false;
        if (!m_allocationsThatSink) {
            m_allocationsThatSink.emplace(m_function.values.size());
            for (const SunkAllocation& allocation : predictSinking(m_analyses)) {
                if (allocation.outcome == SunkAllocationOutcome::Removed || allocation.outcome == SunkAllocationOutcome::Sunk)
                    m_allocationsThatSink->set(allocation.allocation.index());
            }
        }
        return m_allocationsThatSink->get(identity.index());
    });
}

bool GenericCopy::canLeaveCopyAt(BlockID block) const
{
    if (m_keepsAllocationsApart.get(block.index()) && mergesAllocationWithItsCopyAt(block))
        return false;
    return !addsLoopEntry(block) && RegionCloner::canMergeAt(m_analyses, m_isInRegion, block);
}

bool GenericCopy::forceCopyOfUnusableExits()
{
    bool changed = false;
    for (BlockID block : m_exits) {
        if (canLeaveCopyAt(block))
            continue;
        m_mustBeCopied.set(block.index());
        changed = true;
    }
    return changed;
}

bool GenericCopy::tryPlan(Extent extent, uint64_t maxBlocks, const RegionCloner::LivenessSource& liveness)
{
    m_mustBeCopied.clearAll();
    m_testedRejoins.shrink(0);
    if (extent == Extent::OnlyCopiedBlocks)
        computeRegion(extent);
    else {
        do {
            computeRegion(extent);
            if (m_numberOfBlocksFromStarts > maxBlocks) {
                m_wasCutShortByLimit = true;
                return false;
            }
        } while (forceCopyOfUnusableExits());
    }

    if (!canCopyInsideFunction(RegionCloner::checkCanDuplicate(m_analyses, m_region.span(), m_entries.span(), liveness)))
        return false;
    recordTestedRejoins();
    return true;
}

void GenericCopy::recordTestedRejoins()
{
    if (m_testedRejoinBlocks.isEmpty())
        return;
    const Liveness& liveness = m_analyses.liveness();
    UnspecifiedValues unspecifiedValues(m_analyses);
    for (BlockID block : m_testedRejoinBlocks) {
        if (!canLeaveCopyAt(block) || mergesAllocationThatSinksWithItsCopyAt(block))
            continue;
        TestedRejoin rejoin { block, depthOfLoopEnteredAt(block), { }, { } };
        for (ValueID value : liveness.liveAtHead(block)) {
            if (m_mayBeNarrowed.get(value.index()))
                rejoin.narrowedLiveValues.append(value);
        }
        for (ValueID parameter : m_function.at(block).parameters) {
            if (unspecifiedValues.mayContain(parameter))
                rejoin.valuesThatMayBeUnspecified.append(parameter);
        }
        for (ValueID value : rejoin.narrowedLiveValues) {
            if (unspecifiedValues.mayContain(value))
                rejoin.valuesThatMayBeUnspecified.append(value);
        }
        m_testedRejoins.append(WTF::move(rejoin));
    }
}

auto GenericCopy::plan(uint64_t maxBlocks) -> Extent
{
    return plan(maxBlocks, [&]() -> const Liveness& {
        return m_analyses.liveness();
    });
}

auto GenericCopy::plan(uint64_t maxBlocks, NOESCAPE const RegionCloner::LivenessSource& livenessInsideCopiedBlocks) -> Extent
{
    RELEASE_ASSERT(!m_cloner);
    if (m_starts.isEmpty()) {
        if (tryPlan(Extent::OnlyCopiedBlocks, maxBlocks, livenessInsideCopiedBlocks))
            return Extent::OnlyCopiedBlocks;
        m_region.shrink(0);
        return Extent::NotCopied;
    }

    // A copy that goes on from its starts asks what is live at the blocks it comes to.
    auto liveness = [&]() -> const Liveness& {
        return m_analyses.liveness();
    };
    computeBlocksReachingNonRareCode();
    computeNarrowedValues();
    if (!m_isRejoinBlockWithoutTests.isEmpty())
        m_isKnownBetterInTypedCode = findValuesTypedCodeKnowsMoreAbout(m_analyses, m_context.types, { m_valuesToBeNarrowed.span(), m_valuesNarrowedInUnknownWay.span(), m_valuesKnownBeyondTheirTypes.span() });
    if ((!m_isRejoinBlock.isEmpty() || !m_isRejoinBlockWithoutTests.isEmpty()) && tryPlan(Extent::ToWaysBack, maxBlocks, liveness))
        return m_untestedRejoins.isEmpty() && m_testedRejoins.isEmpty() ? Extent::ToEndOfFunction : Extent::ToWaysBack;
    if (tryPlan(Extent::ToEndOfFunction, maxBlocks, liveness))
        return Extent::ToEndOfFunction;
    if (tryPlan(Extent::OnlyCopiedBlocks, maxBlocks, liveness))
        return Extent::OnlyCopiedBlocks;
    m_region.shrink(0);
    return Extent::NotCopied;
}

// Until SSA is repaired, the copy would name a narrowed value defined outside the region directly and be typed from the narrow type. It names a detached cast with the wide type instead.
void GenericCopy::addPlaceholders()
{
    const UseLists& useLists = m_analyses.useLists();
    Vector<ValueID, 8> valuesWithoutCopy;
    for (ValueID value : m_valuesToBeNarrowed) {
        BlockID block = useLists.blockOf(value);
        TypeID type = m_function.at(value).type;
        if (!block || isInRegion(block) || !type || !m_context.types.at(type).isJavaScriptValue())
            continue;
        if (m_hasPlaceholder.add(value.index()))
            valuesWithoutCopy.append(value);
    }

    for (ValueID value : valuesWithoutCopy) {
        Value placeholder = IRBuilder::make(Opcode::Upcast, { value });
        placeholder.setCastType(m_function.at(value).type);
        m_builder.setOrigin(m_function.at(value).origin());
        ValueID id = m_builder.addDetached(WTF::move(placeholder));
        m_function.edit(id, ValueEdit::Type).setExpected({ });
        m_cloner->mapValue(value, id);
    }
}

void GenericCopy::clone(BlockFrequency (*frequencyOfCopy)(BlockFrequency))
{
    RELEASE_ASSERT(!m_cloner && !m_region.isEmpty());
    Origin savedOrigin = m_builder.origin();
    m_cloner.emplace(m_builder, m_function);
    addPlaceholders();
    m_cloner->cloneBlocks(m_region.span(), [](ValueID, Value&) { });
    for (BlockID block : m_region)
        m_function.edit(copyOf(block), BlockEdit::Rest).frequency = frequencyOfCopy(m_function.at(block).frequency);
    m_numberOfValuesAfterCloning = m_function.values.size();
    m_builder.setOrigin(savedOrigin);
}

BlockID GenericCopy::copyOf(BlockID block) const
{
    return m_cloner ? m_cloner->cloneOf(block) : BlockID();
}

ValueID GenericCopy::copyOf(ValueID value) const
{
    return m_hasPlaceholder.get(value.index()) ? ValueID() : nameInCopy(value);
}

ValueID GenericCopy::nameInCopy(ValueID value) const
{
    return m_cloner ? m_cloner->cloneOf(value) : ValueID();
}

std::span<const BlockID> GenericCopy::copiedBlocks() const
{
    return m_cloner ? m_cloner->clonedBlocks() : std::span<const BlockID> { };
}

Vector<TypeID> GenericCopy::computeTypesIgnoringCopy(std::span<const ValueID> changedValues)
{
    struct StoredTypes {
        TypeID type;
        TypeID expected;
    };
    Vector<StoredTypes> storedTypes(m_function.values.size(), [&](size_t index) {
        const Value& value = m_function.values[index];
        return StoredTypes { value.type, value.expected() };
    });

    TypePropagation propagation(TypePropagationInputs { m_context.program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo });
    propagation.restartDependentsOf(changedValues);
    propagation.solveTypes();

    Vector<TypeID> types(m_function.values.size(), [&](size_t index) {
        return m_function.values[index].type;
    });
    for (size_t index = 0; index < storedTypes.size(); ++index) {
        const Value& typed = m_function.values[index];
        if (typed.type == storedTypes[index].type && typed.expectedForTyping() == storedTypes[index].expected)
            continue;
        Value& value = m_function.edit(ValueID(index), ValueEdit::Type);
        value.type = storedTypes[index].type;
        value.setExpected(storedTypes[index].expected);
    }
    m_analyses.didChangeValuesButNotUses();
    return types;
}

Vector<TypeID> GenericCopy::collectTypesOfOtherDefinitions() const
{
    Vector<TypeID> joinedTypes(m_function.values.size());
    for (const OtherDefinition& other : m_otherDefinitions) {
        TypeID& joined = joinedTypes[other.inTypedCode.index()];
        TypeID type = m_function.at(other.definition).type;
        joined = joined ? m_context.types.unionOf(joined, type) : type;
    }
    return joinedTypes;
}

// Once a value enters the copy on a failed check, the types stored in the copy are no upper bound until SSA is repaired and the function is typed again: a use in the copy still names the clone, not the value on the failing edge.
TypeID GenericCopy::typeBoundInCopy(ValueID inTypedCode, ValueID inCopy, const RejoinTypes& rejoinTypes) const
{
    TypeArena& types = m_context.types;
    TypeID type = m_function.at(inCopy).type;
    if (TypeID otherTypes = rejoinTypes.ofOtherDefinitions[inTypedCode.index()])
        type = types.unionOf(type, otherTypes);
    if (m_otherDefinitions.isEmpty() || m_hasPlaceholder.get(inTypedCode.index()))
        return type;
    TypeID typeInTypedCode = rejoinTypes.ignoringCopy[inTypedCode.index()];
    if (!m_mayBeNarrowed.get(inTypedCode.index()) || !types.at(typeInTypedCode).isJavaScriptValue())
        return typeInTypedCode;
    return types.withPrimitives(TypeTable::anything, types.at(typeInTypedCode).primitives & Primitive::Absent);
}

// An AssumeType relies on the checks that dominate it. If the caller erased its copy, a value that rejoins is also tested for the assumed type.
TypeID GenericCopy::typeAssumedInTypedCode(ValueID value, TypeID type, const BitVector& isCopy)
{
    const UseLists& useLists = m_analyses.useLists();
    Vector<ValueID, 4> worklist;
    worklist.append(value);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand || isCopy.get(use.block.index()))
                continue;
            const Value& cast = m_function.at(use.user);
            if (cast.opcode != Opcode::Upcast && cast.opcode != Opcode::AssumeType)
                continue;
            worklist.append(use.user);
            ValueID copy = copyOf(use.user);
            if (cast.opcode == Opcode::AssumeType && copy && m_function.at(copy).opcode != Opcode::AssumeType)
                type = m_context.types.intersectionOf(type, cast.castType());
        }
    }
    return type;
}

auto GenericCopy::planRejoin(const TestedRejoin& rejoin, const RejoinTypes& rejoinTypes, const BitVector& isCopy) -> std::optional<RejoinPlan>
{
    TypeArena& types = m_context.types;
    const Program& program = m_context.program;
    BlockID copy = copyOf(rejoin.block);
    RELEASE_ASSERT(copy);
    const CompactList<ValueID>& parameters = m_function.at(rejoin.block).parameters;
    const CompactList<ValueID>& parametersOfCopy = m_function.at(copy).parameters;
    if (parameters.size() != parametersOfCopy.size())
        return std::nullopt;

    RejoinPlan plan;
    plan.block = rejoin.block;
    plan.depthOfLoop = rejoin.depthOfLoop;
    for (Edge edge : m_analyses.cfg().incomingEdges(copy)) {
        if (!isCopy.get(edge.from.index()))
            continue;
        if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != parameters.size())
            return std::nullopt;
        plan.edges.append(edge);
    }
    if (plan.edges.isEmpty())
        return std::nullopt;

    PropertyAccessContext accessContext { program, &m_context.inputs.flow };
    auto addTest = [&](std::optional<uint32_t> parameterIndex, ValueID inTypedCode, TypeID typeInCopy) {
        TypeID typeInTypedCode = typeAssumedInTypedCode(inTypedCode, rejoinTypes.ignoringCopy[inTypedCode.index()], isCopy);
        if (m_typesTestedOnWayBack == TypesTestedOnWayBack::Expected && m_mayBeNarrowed.get(inTypedCode.index())) {
            if (TypeID expected = m_function.at(inTypedCode).expected())
                typeInTypedCode = types.intersectionOf(typeInTypedCode, types.intern(types.at(expected).withoutClassProofs()));
        }
        const Type& given = types.at(typeInCopy);
        const Type& wanted = types.at(typeInTypedCode);
        if (given.isSubsetOf(wanted, program))
            return true;
        if (!given.isJavaScriptValue() || !wanted.isJavaScriptValue() || rejoin.valuesThatMayBeUnspecified.contains(inTypedCode))
            return false;
        Type testable = testableStoredTypeOf(accessContext, given, wanted, types);
        // No test looks at the contents of an array or at what is pristine about it.
        if (testable.isNever() || testable != testable.withoutClassProofs())
            return false;
        Type passing = given.castTo(testable, types);
        if (passing.isNever() || !passing.isSubsetOf(wanted, program))
            return false;
        plan.tests.append(RejoinTest { parameterIndex, inTypedCode, types.intern(testable) });
        return true;
    };
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        TypeID typeInCopy = typeBoundInCopy(parameters[index], parametersOfCopy[index], rejoinTypes);
        if (!addTest(index, parameters[index], typeInCopy))
            return std::nullopt;
        plan.carriedTypes.append(typeInCopy);
    }
    for (ValueID value : rejoin.narrowedLiveValues) {
        ValueID valueInCopy = nameInCopy(value);
        if (valueInCopy && !addTest(std::nullopt, value, typeBoundInCopy(value, valueInCopy, rejoinTypes)))
            return std::nullopt;
    }
    return plan;
}

// An edge from the copy straight to the header would make the copy's blocks part of the natural loop.
auto GenericCopy::planPreheader(BlockID header, bool isTestedRejoin, const BitVector& isCopy) -> std::optional<PreheaderPlan>
{
    // Most functions have no cycle, and the loops are found from the dominators, which the blocks just added have made stale.
    if (m_function.at(header).isHandlerBlock() || !m_analyses.isInCycle(header))
        return std::nullopt;
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const NaturalLoop* loop = loops.headerOf(header);
    if (!loop)
        return std::nullopt;

    PreheaderPlan plan;
    plan.header = header;
    bool isEnteredFromCopy = isTestedRejoin;
    for (Edge edge : m_analyses.cfg().incomingEdges(header)) {
        bool isFromCopy = isCopy.get(edge.from.index());
        if (!isFromCopy && loops.belongsTo(edge.from, *loop))
            continue;
        if (edge.isHandlerEdge())
            return std::nullopt;
        isEnteredFromCopy |= isFromCopy;
        plan.edges.append(edge);
    }
    if (!isEnteredFromCopy)
        return std::nullopt;
    return plan;
}

// All blocks are added before any parameter: IRBuilder::addParameter() needs the CFG, and adding a block invalidates it.
void GenericCopy::addRejoinBlocks(Vector<RejoinPlan, 4>& rejoins, Vector<PreheaderPlan, 4>& preheaders, const RejoinTypes& rejoinTypes)
{
    for (RejoinPlan& plan : rejoins)
        plan.testBlock = m_builder.addBlock(m_function.at(copyOf(plan.block)).frequency);
    for (PreheaderPlan& plan : preheaders) {
        plan.preheader = m_builder.addBlock(m_function.at(plan.header).frequency);
        size_t numberOfParameters = m_function.at(plan.header).parameters.size();
        for (Edge& edge : plan.edges) {
            if (targetOf(m_function, edge).arguments.size() != numberOfParameters)
                edge = Edge { m_builder.splitEdge(edge), 0 };
        }
    }

    for (RejoinPlan& plan : rejoins) {
        m_builder.setOrigin(m_function.at(plan.block).terminator.origin());
        Vector<ValueID, 4> parametersOfCopy(m_function.at(copyOf(plan.block)).parameters.span());
        for (size_t index = 0; index < parametersOfCopy.size(); ++index) {
            ValueID carried = m_builder.addParameter(plan.testBlock, plan.carriedTypes[index]);
            m_function.edit(carried, ValueEdit::Type).setExpected(m_function.at(parametersOfCopy[index]).expected());
            plan.carriedValues.append(carried);
        }
    }
    for (PreheaderPlan& plan : preheaders) {
        m_builder.setOrigin(m_function.at(plan.header).terminator.origin());
        for (ValueID parameter : Vector<ValueID, 4>(m_function.at(plan.header).parameters.span())) {
            TypeID type = rejoinTypes.ignoringCopy.isEmpty() ? m_function.at(parameter).type : rejoinTypes.ignoringCopy[parameter.index()];
            ValueID added = m_builder.addParameter(plan.preheader, type);
            m_function.edit(added, ValueEdit::Type).setExpected(m_function.at(parameter).expected());
            plan.parameters.append(added);
        }
    }
}

void GenericCopy::emitRejoin(const RejoinPlan& plan, BlockID target)
{
    BlockID copy = copyOf(plan.block);
    BlockFrequency frequency = m_function.at(copy).frequency;
    m_builder.setOrigin(m_function.at(plan.block).terminator.origin());
    for (Edge edge : plan.edges)
        m_builder.redirectEdge(edge, plan.testBlock, Arguments(targetOf(m_function, edge).arguments));

    m_builder.moveToEndOf(plan.testBlock);
    Arguments narrowed = plan.carriedValues;
    for (const RejoinTest& test : plan.tests) {
        ValueID subject = test.parameterIndex ? plan.carriedValues[*test.parameterIndex] : nameInCopy(test.value);
        ValueID typeTest = m_builder.appendTypeTest(subject, test.testedType);
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(typeTest, OperandIndex::TypeTest::value, frequency, frequency);
        m_builder.moveToEndOf(branch.notTaken);
        m_builder.emitJump(copy, Arguments(plan.carriedValues));
        m_builder.moveToEndOf(branch.taken);
        if (test.parameterIndex)
            narrowed[*test.parameterIndex] = branch.provenOnTaken;
        else
            m_otherDefinitions.append(OtherDefinition { test.value, branch.provenOnTaken });
    }
    m_builder.emitJump(target, WTF::move(narrowed));
}

void GenericCopy::emitPreheader(const PreheaderPlan& plan)
{
    m_builder.setOrigin(m_function.at(plan.header).terminator.origin());
    for (Edge edge : plan.edges)
        m_builder.redirectEdge(edge, plan.preheader, Arguments(targetOf(m_function, edge).arguments));
    m_builder.moveToEndOf(plan.preheader);
    m_builder.emitJump(plan.header, Arguments(plan.parameters));
}

unsigned GenericCopy::depthOfLoopEnteredAt(BlockID block) const
{
    if (!m_analyses.cfg().hasCycle())
        return 0;
    const NaturalLoops& loops = m_analyses.naturalLoops();
    unsigned depth = loops.loopDepth(block);
    m_function.at(block).forEachTarget([&](const Target& target) {
        depth = std::max(depth, loops.loopDepth(target.block));
    });
    return depth;
}

void GenericCopy::keepWaysBackWithinSizeOfCopy(Vector<RejoinPlan, 4>& rejoins) const
{
    uint64_t numberOfTests = 0;
    for (const RejoinPlan& rejoin : rejoins)
        numberOfTests += rejoin.tests.size();
    uint64_t numberOfCopiedValues = 0;
    for (BlockID block : copiedBlocks())
        numberOfCopiedValues += m_function.at(block).values.size();
    if (numberOfTests <= numberOfCopiedValues)
        return;

    struct Candidate {
        unsigned depthOfLoop;
        size_t numberOfTests;
        size_t index;
    };
    Vector<Candidate> candidates(rejoins.size(), [&](size_t index) {
        const RejoinPlan& rejoin = rejoins[index];
        return Candidate { rejoin.depthOfLoop, rejoin.tests.size(), index };
    });
    AOT::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.depthOfLoop != b.depthOfLoop)
            return a.depthOfLoop > b.depthOfLoop;
        return std::pair { a.numberOfTests, a.index } < std::pair { b.numberOfTests, b.index };
    });

    BitVector isKept(rejoins.size());
    uint64_t numberOfKeptTests = 0;
    for (const Candidate& candidate : candidates) {
        if (numberOfKeptTests + candidate.numberOfTests > numberOfCopiedValues)
            continue;
        numberOfKeptTests += candidate.numberOfTests;
        isKept.quickSet(candidate.index);
    }
    Statistics& statistics = m_context.build.statistics();
    statistics.count(StatisticID::WaysBackNotMadeForSizeOfCopy, rejoins.size() - isKept.bitCount());
    statistics.count(StatisticID::TestsOnWaysBackNotMade, numberOfTests - numberOfKeptTests);

    size_t index = 0;
    rejoins.removeAllMatching([&](const RejoinPlan&) {
        return !isKept.quickGet(index++);
    });
}

void GenericCopy::addWaysBack(std::span<const ValueID> changedValues, TypesTestedOnWayBack typesTested)
{
    RELEASE_ASSERT(m_cloner);
    m_typesTestedOnWayBack = typesTested;
    if (m_testedRejoins.isEmpty() && m_untestedRejoins.isEmpty())
        return;

    BitVector isCopy(m_function.blocks.size());
    for (BlockID block : copiedBlocks())
        isCopy.set(block.index());

    RejoinTypes rejoinTypes;
    Vector<RejoinPlan, 4> rejoins;
    if (!m_testedRejoins.isEmpty()) {
        rejoinTypes.ignoringCopy = computeTypesIgnoringCopy(changedValues);
        rejoinTypes.ofOtherDefinitions = collectTypesOfOtherDefinitions();
        for (const TestedRejoin& rejoin : m_testedRejoins) {
            std::optional<RejoinPlan> plan = planRejoin(rejoin, rejoinTypes, isCopy);
            if (!plan)
                continue;
            if (m_testsOnlyWaysBackIntoLoops && !plan->depthOfLoop && !plan->tests.isEmpty())
                continue;
            rejoins.append(WTF::move(*plan));
        }
        keepWaysBackWithinSizeOfCopy(rejoins);
    }

    Vector<PreheaderPlan, 4> preheaders;
    for (RejoinPlan& rejoin : rejoins) {
        if (std::optional<PreheaderPlan> plan = planPreheader(rejoin.block, true, isCopy)) {
            rejoin.preheaderIndex = preheaders.size();
            preheaders.append(WTF::move(*plan));
        }
    }
    for (BlockID block : m_untestedRejoins) {
        if (std::optional<PreheaderPlan> plan = planPreheader(block, false, isCopy))
            preheaders.append(WTF::move(*plan));
    }
    m_numberOfWaysBack = rejoins.size() + m_untestedRejoins.size();
    if (rejoins.isEmpty() && preheaders.isEmpty())
        return;

    Origin savedOrigin = m_builder.origin();
    std::optional<Target> savedHandler = m_builder.handler();
    m_builder.setHandler(std::nullopt);

    addRejoinBlocks(rejoins, preheaders, rejoinTypes);
    for (const RejoinPlan& rejoin : rejoins)
        emitRejoin(rejoin, rejoin.preheaderIndex ? preheaders[*rejoin.preheaderIndex].preheader : rejoin.block);
    for (const PreheaderPlan& preheader : preheaders)
        emitPreheader(preheader);

    m_builder.setHandler(WTF::move(savedHandler));
    m_builder.setOrigin(savedOrigin);
}

void GenericCopy::addDefinition(ValueID inTypedCode, ValueID otherDefinition)
{
    // An older value may already belong to another SSARepair variable.
    RELEASE_ASSERT(m_cloner && otherDefinition.index() >= m_numberOfValuesAfterCloning);
    m_otherDefinitions.append(OtherDefinition { inTypedCode, otherDefinition });
    if (!m_hasPlaceholder.get(inTypedCode.index()))
        return;

    ValueID name = nameInCopy(inTypedCode);
    TypeID type = m_context.types.unionOf(m_function.at(name).type, m_function.at(otherDefinition).type);
    if (type == m_function.at(name).type)
        return;
    Value& placeholder = m_function.edit(name);
    placeholder.type = type;
    placeholder.setCastType(type);
    m_analyses.didChangeValuesButNotUses();
}

void GenericCopy::addVariablesTo(SSARepair& repair)
{
    RELEASE_ASSERT(m_cloner);
    m_cloner->addVariablesTo(repair, m_analyses);

    Vector<ValueID, 16> values;
    for (size_t index : m_hasPlaceholder)
        values.append(ValueID(index));
    for (const OtherDefinition& other : m_otherDefinitions)
        values.append(other.inTypedCode);
    AOT::sort(values);
    removeRepeatedElements(values);

    AOT::stableSort(m_otherDefinitions, { }, &OtherDefinition::inTypedCode);
    std::span<const OtherDefinition> remaining = m_otherDefinitions.span();
    for (ValueID value : values) {
        std::optional<SSARepair::Variable> variable = repair.findVariable(value);
        if (!variable)
            variable = repair.addVariable(value);
        // RegionCloner only visits values still listed in the region, and the caller may have moved this one to a new block.
        if (ValueID copy = nameInCopy(value); copy && !repair.findVariable(copy))
            repair.addDefinition(*variable, copy);
        for (; !remaining.empty() && remaining.front().inTypedCode == value; remaining = remaining.subspan(1))
            repair.addDefinition(*variable, remaining.front().definition);
    }
}

bool isClosedOnlyByWaysBack(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const NaturalLoops& loops = analyses.naturalLoops();
    const Function& function = analyses.function();
    BlockFrequency frequencyOfHeader = function.at(loop.header()).frequency;
    return std::ranges::all_of(analyses.cfg().incomingEdges(loop.header()), [&](Edge edge) {
        return !loops.belongsTo(edge.from, loop) || isRarerThan(function.at(edge.from).frequency, frequencyOfHeader);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
