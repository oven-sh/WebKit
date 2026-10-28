/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCopy.h"

#if ENABLE(AOT)

#include "AOTArrayChains.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTCalleeVersions.h"
#include "AOTCodeShapes.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCoroutines.h"
#include "AOTCreateEntrypoints.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectSummaries.h"
#include "AOTEffects.h"
#include "AOTEffectsCache.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTGuardAvailability.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTInlineHeads.h"
#include "AOTKnownValue.h"
#include "AOTMemoryLocation.h"
#include "AOTOperatorForms.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTProgramPasses.h"
#include "AOTPropertyAccess.h"
#include "AOTProvisionalFunctions.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTSinkAllocations.h"
#include "AOTSort.h"
#include "AOTSwitchWindow.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <ranges>
#include <wtf/Atomics.h>
#include <wtf/CommaPrinter.h>
#include <wtf/GenerationalSet.h>
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace CodeSize {

constexpr unsigned simpleOperation = 8;
constexpr unsigned cellConstant = 7;
constexpr unsigned typeCheck = 16;
constexpr unsigned directCall = 8;
constexpr unsigned indirectCall = 24;
constexpr unsigned perOperand = 4;
constexpr unsigned allocation = 48;
constexpr unsigned accessByName = 20;
constexpr unsigned slotAccess = 8;
constexpr unsigned branch = 8;
constexpr unsigned jump = 4;
constexpr unsigned perCase = 8;
constexpr unsigned perPlaceInWindow = 16;
constexpr unsigned throwSite = 16;
constexpr unsigned stringSwitch = 24;

} // namespace CodeSize

static constexpr OpcodeSet opcodesWithoutCode = castsThatProveNothing | OpcodeSet {
    Opcode::Parameter,
    Opcode::Undefined,
    Opcode::Null,
    Opcode::Absent,
    Opcode::Boolean,
    Opcode::Integer,
    Opcode::Double,
    Opcode::KeyConstant,
    Opcode::Unspecified,
    Opcode::Results,
    Opcode::ResultAt,
    Opcode::LoopHint,
    Opcode::AssumeType,
    Opcode::FinishObject,
    Opcode::AllGuards,
    Opcode::Entered,
    Opcode::Specified,
    Opcode::LogicalNot,
    Opcode::ValidateType,
    Opcode::ValidateCallee,
    Opcode::ValidateFirstRun,
    Opcode::ValidateSameValue,
    Opcode::ShouldFailPrediction,
};

static unsigned switchCaseBytes(const Terminator& terminator)
{
    ASSERT(!terminator.targets.isEmpty());
    if (std::optional<SwitchWindow> window = findSwitchWindow(terminator))
        return CodeSize::perPlaceInWindow * static_cast<unsigned>(window->places.size());
    return CodeSize::perCase * static_cast<unsigned>(terminator.targets.size() - 1);
}

// Where the row of an operation bounds what all its values do, and the bound leaves out everything that is asked below, no value needs its effects computed.
static bool neverCallsCollectsOrThrows(Opcode opcode)
{
    static const std::array<bool, numberOfOpcodes> answers = [] {
        std::array<bool, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>) {
            Effects atMost = declaredBoundOf(member);
            result[static_cast<unsigned>(member)] = effectFactsOf(member).staysWithinDeclaredBound && !atMost.mayCallUnknownCode && !atMost.mayCollect && !atMost.mayThrow;
        }
        return result;
    }();
    return answers[static_cast<unsigned>(opcode)];
}

unsigned estimatedDirectCallBytes(unsigned numberOfOperands)
{
    return CodeSize::directCall + CodeSize::perOperand * numberOfOperands;
}

AOT_THRESHOLD(unsigned, maxDepthOfConstantExpression, 6);

// The back end folds arithmetic on constants, such as `A | B` of two enum members.
static bool isConstantExpression(const Function& function, const Value& value, unsigned remainingDepth = maxDepthOfConstantExpression)
{
    static constexpr OpcodeSet foldableOpcodes { Opcode::BitOr, Opcode::BitAnd, Opcode::BitXor, Opcode::Add, Opcode::Sub, Opcode::Mul };
    if (value.opcode == Opcode::Integer || value.opcode == Opcode::Double)
        return true;
    if (!remainingDepth)
        return false;
    if (isCastThatProvesNothing(value.opcode))
        return isConstantExpression(function, function.at(value.valueOperand()), remainingDepth - 1);
    if (!foldableOpcodes.contains(value.opcode))
        return false;
    return isConstantExpression(function, function.at(value.leftOperand()), remainingDepth - 1) && isConstantExpression(function, function.at(value.rightOperand()), remainingDepth - 1);
}

unsigned estimatedCodeBytes(const Program& program, const Function& function, const Value& value)
{
    Opcode opcode = value.opcode;

    if (opcodesWithoutCode.contains(opcode) || isConstantExpression(function, value))
        return 0;
    if (isRematerializable(value))
        return CodeSize::cellConstant;
    if (provesOf(opcode) == Proof::TestedType && knownValueOfType(program.at(value.type)).isKnown())
        return 0;
    if (opcode == Opcode::RequireObjectCoercible && !program.typeOf(function, value.valueOperand()).mayBeUndefinedOrNull())
        return 0;
    if (isCast(opcode) || provesForStretch(provesOf(opcode)) || provesOf(opcode) == Proof::TestedType)
        return CodeSize::typeCheck;
    if (opcode == Opcode::ToBoolean)
        return program.typeOf(function, value.valueOperand()).isBoolean() ? 0 : CodeSize::simpleOperation;

    // What takes the caught value calls a stub without operands.
    if (opcode == Opcode::CatchValue || opcode == Opcode::CatchException)
        return estimatedDirectCallBytes(0);

    unsigned operandBytes = CodeSize::perOperand * value.numberOfOperands();

    switch (accessClassOf(opcode)) {
    case AccessClass::None:
        break;
    case AccessClass::Slot:
        return CodeSize::slotAccess;
    case AccessClass::ByName:
        // A body is measured before its accesses are resolved. Where the type of the receiver names its layouts the access ends as a slot operation.
        return isNamedAccess(opcode) && program.typeOf(function, value.operand(*receiverOf(opcode))).layouts ? CodeSize::slotAccess : CodeSize::accessByName;
    case AccessClass::DirectCall:
        return estimatedDirectCallBytes(value.numberOfOperands());
    case AccessClass::IndirectCall:
        return CodeSize::indirectCall + operandBytes;
    }

    if (neverCallsCollectsOrThrows(opcode))
        return allocatesInlineOf(opcode) ? CodeSize::allocation + operandBytes : CodeSize::simpleOperation;

    EffectSummaries unknownSummaries;
    Effects effects = effectsOf(EffectContext { program, unknownSummaries }, function, value).normal;
    if (effects.mayCallUnknownCode)
        return CodeSize::indirectCall + operandBytes;
    if (allocatesInlineOf(opcode))
        return CodeSize::allocation + operandBytes;
    if (effects.mayCollect || effects.mayThrow)
        return CodeSize::indirectCall + operandBytes;
    return CodeSize::simpleOperation;
}

unsigned estimatedCodeBytes(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return CodeSize::jump;
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
        return CodeSize::branch;
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchOnShape:
        return CodeSize::branch + switchCaseBytes(terminator);
    case TerminatorKind::SwitchString:
        return CodeSize::stringSwitch + switchCaseBytes(terminator);
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return CodeSize::throwSite;
    case TerminatorKind::TryCallInline:
        return CodeSize::indirectCall + CodeSize::perOperand * terminator.numberOfOperands();
    case TerminatorKind::Unreachable:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned estimatedCodeBytes(const Program& program, const Function& function, const Block& block)
{
    unsigned bytes = estimatedCodeBytes(block.terminator);
    for (ValueID id : block.values)
        bytes = saturatingSum<unsigned>(bytes, estimatedCodeBytes(program, function, function.at(id)));
    return bytes;
}

static std::optional<uint32_t> targetDecidedByTypeOfCondition(const Program& program, const Function& function, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    return decidedTargetOf(terminator, knownValueOfType(program.typeOf(function, terminator.conditionOperand())));
}

static void takeOffWhatDecidedBranchesCutOff(const Program& program, const Function& function, const CFG& cfg, Vector<uint32_t>& blockBytes)
{
    BitVector isCutOff(function.blocks.size());
    for (BlockID id : cfg.reversePostOrder()) {
        std::span<const Edge> incoming = cfg.incomingEdges(id);
        bool isReachedByNoEdge = !incoming.empty() && std::ranges::all_of(incoming, [&](Edge edge) {
            if (isCutOff.get(edge.from.index()))
                return true;
            std::optional<uint32_t> decided = edge.isHandlerEdge() ? std::nullopt : targetDecidedByTypeOfCondition(program, function, function.at(edge.from).terminator);
            return decided && *decided != edge.targetIndex;
        });
        if (isReachedByNoEdge) {
            isCutOff.set(id.index());
            blockBytes[id.index()] = 0;
            continue;
        }
        const Terminator& terminator = function.at(id).terminator;
        if (targetDecidedByTypeOfCondition(program, function, terminator))
            blockBytes[id.index()] -= std::min(blockBytes[id.index()], estimatedCodeBytes(terminator));
    }
}

namespace {

struct IntegerComparison {
    ValueID subject;
    int64_t constant { 0 };
};

} // anonymous namespace

static std::optional<IntegerComparison> findIntegerComparison(FunctionAnalyses& analyses, const Block& block)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (block.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    ValueID condition = definitionThroughUpcasts(function, block.terminator.conditionOperand());
    const Value& comparison = function.at(condition);
    if (comparison.opcode != Opcode::StrictEq || !block.values.contains(condition))
        return std::nullopt;
    for (unsigned index = 0; index < 2; ++index) {
        std::optional<int64_t> constant = integerConstantThroughCasts(program, function, comparison.operand(index));
        ValueID subject = comparison.operand(1 - index);
        if (constant && program.typeOf(function, subject).isInteger() && !integerConstantThroughCasts(program, function, subject))
            return IntegerComparison { analyses.identityOf(subject), *constant };
    }
    return std::nullopt;
}

static unsigned estimateSwitchBytes(Vector<std::pair<int64_t, BlockID>, 8>& cases)
{
    AOT::sort(cases, { }, [](const auto& entry) { return entry.first; });
    unsigned bytes = CodeSize::branch;
    for (size_t begin = 0; begin < cases.size();) {
        size_t end = begin;
        Vector<BlockID, SwitchWindow::maxPlaces + 1> places;
        for (; end < cases.size() && cases[end].first - cases[begin].first < SwitchWindow::numberOfValues; ++end) {
            if (places.size() <= SwitchWindow::maxPlaces && !places.contains(cases[end].second))
                places.append(cases[end].second);
        }
        unsigned bytesPerCase = CodeSize::perCase * static_cast<unsigned>(end - begin);
        bytes += places.size() <= SwitchWindow::maxPlaces ? std::min(CodeSize::perPlaceInWindow * static_cast<unsigned>(places.size()), bytesPerCase) : bytesPerCase;
        begin = end;
    }
    return bytes;
}

// The back end compiles `kind === A || kind === B || ..` like a switch: a range check and a bit test.
static void estimateComparisonChainsAsSwitches(FunctionAnalyses& analyses, Vector<uint32_t>& blockBytes)
{
    constexpr unsigned linkBytes = CodeSize::simpleOperation + CodeSize::branch;
    AOT_THRESHOLD(unsigned, minLinks, 3);
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    BitVector isLink(function.blocks.size());
    auto linkAt = [&](BlockID id) {
        return blockBytes[id.index()] >= linkBytes ? findIntegerComparison(analyses, function.at(id)) : std::nullopt;
    };
    for (BlockID head : cfg.reversePostOrder()) {
        std::optional<IntegerComparison> first = isLink.get(head.index()) ? std::nullopt : linkAt(head);
        if (!first)
            continue;
        isLink.set(head.index());
        Vector<std::pair<int64_t, BlockID>, 8> cases { { first->constant, function.at(head).terminator.taken().block } };
        Vector<BlockID, 8> links { head };
        for (BlockID next = function.at(head).terminator.notTaken().block; cfg.singleIncomingEdge(next) && !isLink.get(next.index()); next = function.at(next).terminator.notTaken().block) {
            std::optional<IntegerComparison> link = linkAt(next);
            if (!link || link->subject != first->subject)
                break;
            isLink.set(next.index());
            cases.append({ link->constant, function.at(next).terminator.taken().block });
            links.append(next);
        }
        unsigned unfoldedBytes = linkBytes * static_cast<unsigned>(links.size());
        unsigned chainBytes = links.size() < minLinks ? unfoldedBytes : estimateSwitchBytes(cases);
        if (chainBytes >= unfoldedBytes)
            continue;
        for (BlockID link : links)
            blockBytes[link.index()] -= linkBytes;
        blockBytes[head.index()] += chainBytes;
    }
}

ValueID findCopyIntoCopy(const Function& function, const Block& block, ValueID copyObject)
{
    auto leavesNothingOut = [](const Value& value) {
        return !value.request() && value.excludedKeys().empty() && value.excludedOperands().empty();
    };
    ASSERT(function.at(copyObject).opcode == Opcode::CopyObject);
    std::span<const ValueID> values = block.values.span();
    size_t next = std::ranges::find(values, copyObject) - values.begin() + 1;
    if (next >= values.size() || !leavesNothingOut(function.at(copyObject)))
        return { };
    const Value& copy = function.at(values[next]);
    if (copy.opcode != Opcode::CopyDataProperties || copy.targetOperand() != copyObject || !leavesNothingOut(copy))
        return { };
    return values[next];
}

static ASCIILiteral nameOf(InlineBarrier barrier)
{
    switch (barrier) {
    case InlineBarrier::Unknown:
        return "Unknown"_s;
    case InlineBarrier::None:
        return "None"_s;
    case InlineBarrier::NoBody:
        return "NoBody"_s;
    case InlineBarrier::NotInlinable:
        return "NotInlinable"_s;
    case InlineBarrier::NeverInlined:
        return "NeverInlined"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isBoundToFrame(const Function& function, const Block& block, uint32_t indexInBlock)
{
    Opcode opcode = function.at(block.values[indexInBlock]).opcode;
    if (boundToOf(opcode) != BoundTo::Frame)
        return false;
    bool readsCaughtValue = opcode == Opcode::CatchValue || opcode == Opcode::CatchException;
    return !readsCaughtValue || indexInBlock >= indexAfterCaughtValues(function, block);
}

bool isWrapperCreatedByEngine(const Function& function)
{
    switch (function.creator.kind()) {
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::EntryChecks:
    case CreatorRecord::Kind::BoundFunction:
        return true;
    case CreatorRecord::Kind::Coroutine:
        return function.hasOrdinaryFrame() && function.role != FunctionRole::AsyncWorker;
    case CreatorRecord::Kind::None:
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::Version:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::ChainStep:
    case CreatorRecord::Kind::StubBody:
    case CreatorRecord::Kind::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isRepeated(BuiltinCallbackTimes times)
{
    switch (times) {
    case BuiltinCallbackTimes::Repeatedly:
    case BuiltinCallbackTimes::PerElement:
        return true;
    case BuiltinCallbackTimes::Later:
    case BuiltinCallbackTimes::Once:
    case BuiltinCallbackTimes::PerEvent:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isStoreByKey(Opcode opcode)
{
    return storedOf(opcode) && receiverOf(opcode) && Value::opcodesWithKey.contains(opcode);
}

// The summary counts neither the bytes nor the stores of these blocks.
static Expected<bool> isSlowPath(const Function& function, const Block& block)
{
    return block.frequency.predicted() == BlockFrequency::Rare && function.at(entryBlock).frequency.predicted() != BlockFrequency::Rare;
}

// `catch (e) { throw e; }` does no work of the program. Under a `catch` of the caller the copy loses it, and its throw is a jump.
static bool onlyThrowsCaughtValueAgain(const Function& function, const Block& block)
{
    return block.isHandlerBlock() && block.values.size() == 1 && function.at(block.values[0]).opcode == Opcode::CatchValue && block.terminator.kind == TerminatorKind::Throw && block.terminator.valueOperand() == block.values[0];
}

enum class FeasibleSuccessors : uint8_t { All, HandlerOnly, None };

static bool isFeasibleEdge(Edge edge, FeasibleSuccessors feasibleSuccessors)
{
    switch (feasibleSuccessors) {
    case FeasibleSuccessors::All:
        return true;
    case FeasibleSuccessors::HandlerOnly:
        return edge.isHandlerEdge();
    case FeasibleSuccessors::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector computeNoReturnBlocks(const Program& program, const Function& function, const CFG& cfg, NOESCAPE const Invocable<bool(FunctionID)> auto& neverReturns)
{
    auto isCoroutineOrCreatedByOne = [](const Function& candidate) {
        return candidate.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || candidate.creator.is<CreatorRecord::Coroutine>();
    };
    auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    if (isCoroutineOrCreatedByOne(function) || (entrypoint && entrypoint->target && isCoroutineOrCreatedByOne(program.at(entrypoint->target))))
        return { };

    unsigned numberOfBlocks = function.blocks.size();
    Vector<FeasibleSuccessors> feasibleSuccessors(FillWith { }, numberOfBlocks, FeasibleSuccessors::All);
    BitVector mayReturn(numberOfBlocks);
    Vector<BlockID, 16> worklist;
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        bool hasDeadEnd = std::ranges::any_of(block.values, [&](ValueID valueID) {
            const Value& value = function.at(valueID);
            return value.type == TypeTable::never || (value.opcode == Opcode::CallDirect && neverReturns(value.function()));
        });
        if (hasDeadEnd)
            feasibleSuccessors[id.index()] = block.handler ? FeasibleSuccessors::HandlerOnly : FeasibleSuccessors::None;
        else if (returnsToCaller(block.terminator.kind)) {
            mayReturn.quickSet(id.index());
            worklist.append(id);
        }
    }
    while (!worklist.isEmpty()) {
        for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
            unsigned predecessor = edge.from.index();
            if (isFeasibleEdge(edge, feasibleSuccessors[predecessor]) && !mayReturn.quickSet(predecessor))
                worklist.append(edge.from);
        }
    }
    BitVector result;
    for (BlockID id : cfg.reversePostOrder()) {
        if (!mayReturn.quickGet(id.index()))
            result.ensureSizeAndSet(id.index(), numberOfBlocks);
    }
    return result;
}

// The blocks that are reached from the entry along edges that are expected to be taken and that are admitted, each asked once in the order reached, without those that neither return nor lead to another of them.
static BitVector findHeadBlocks(const Function& function, FunctionAnalyses& analyses, NOESCAPE const Invocable<bool(BlockID)> auto& admits)
{
    BitVector blocks;
    BitVector seen;
    Vector<BlockID, 8> queue { entryBlock };
    for (unsigned next = 0; next < queue.size(); ++next) {
        BlockID id = queue[next];
        if (seen.set(id.index()) || !admits(id))
            continue;
        blocks.set(id.index());
        const auto& targets = function.at(id).terminator.targets;
        for (uint32_t index = 0; index < targets.size(); ++index) {
            if (analyses.blockFrequencies().classOf(Edge { id, index }).predicted() == BlockFrequency::Normal)
                queue.append(targets[index].block);
        }
    }
    bool changed;
    do {
        changed = false;
        for (BlockID id : queue) {
            const Terminator& terminator = function.at(id).terminator;
            if (!blocks.get(id.index()) || terminator.kind == TerminatorKind::Return)
                continue;
            if (terminator.targets.containsIf([&](const Target& target) { return blocks.get(target.block.index()); }))
                continue;
            blocks.clear(id.index());
            changed = true;
        }
    } while (changed);
    return blocks;
}

InliningSummary summarizeFunctionForInlining(const AnalysisContext& context, const Function& function, Expected<HeatClass> heat)
{
    RELEASE_ASSERT(context.flow);
    BuildContext& build = context.build;
    const Program& program = context.program;
    FunctionAnalyses analyses(context, function);
    InliningSummary summary;
    Vector<uint32_t> blockBytes;
    GenerationalSet<uint32_t> visitedValues;

    auto computeBarrier = [&]() -> InlineBarrier {
        if (!function.hasBody())
            return InlineBarrier::NoBody;
        if (!function.mayBeInlined() || function.isModuleBody())
            return InlineBarrier::NotInlinable;
        RuntimeFunctionID runtimeFunction = function.enclosingRuntimeFunction;
        if (runtimeFunction && program.builtins().at(runtimeFunction).flags.contains(RuntimeFunctionFlag::IsNeverInlined))
            return InlineBarrier::NeverInlined;
        return InlineBarrier::None;
    };

    auto isLikely = [&](BlockID id) {
        return function.at(id).frequency.predicted() == BlockFrequency::Normal;
    };

    // A declared type has its test in the body already. Without a declared type the same test is placed after the body is copied.
    auto bytesOfDeclaredTypeTest = [&](const Block& block) -> uint32_t {
        const Terminator& terminator = block.terminator;
        if (!terminator.isConditionalBranch() || block.values.isEmpty() || terminator.conditionOperand() != block.values.last())
            return 0;
        const Value& test = function.at(block.values.last());
        if (test.opcode != Opcode::TypeTest || !isLikely(terminator.taken().block) || isLikely(terminator.notTaken().block) || targetDecidedByTypeOfCondition(program, function, terminator))
            return 0;
        return estimatedCodeBytes(program, function, test) + estimatedCodeBytes(terminator);
    };

    auto computeLikelyBytes = [&]() -> uint32_t {
        if (!isLikely(entryBlock) || !function.has(FunctionFlags::IsChecked))
            return std::numeric_limits<uint32_t>::max();
        uint32_t likelyBytes = 0;
        uint32_t otherBytes = 0;
        for (BlockID id : analyses.cfg().reversePostOrder()) {
            uint32_t bytes = blockBytes[id.index()];
            if (isLikely(id))
                likelyBytes = saturatingSum<uint32_t>(likelyBytes, bytes - std::min(bytes, bytesOfDeclaredTypeTest(function.at(id))));
            else
                otherBytes = saturatingSum<uint32_t>(otherBytes, bytes);
        }
        // The copy for values that fail a test is about as large as the function without its types.
        return std::max(likelyBytes, otherBytes);
    };

    auto entryParameterBehind = [&](ValueID value) -> std::optional<uint32_t> {
        size_t index = function.at(entryBlock).parameters.find(sameObjectThroughCasts(function, value).valueInFrontOfFacts());
        if (index == notFound)
            return std::nullopt;
        return static_cast<uint32_t>(index);
    };

    auto entryParameterWithIdentityOf = [&](ValueID value) -> std::optional<uint32_t> {
        if (std::optional<uint32_t> index = entryParameterBehind(value))
            return index;
        return entryParameterBehind(analyses.identityOf(value));
    };

    auto entryParameterMeasuredBy = [&](ValueID length) -> std::optional<uint32_t> {
        std::optional<MeasuredStorage> measured = storageMeasuredBy(program, function.at(sameObjectThroughCasts(function, length)));
        if (!measured || measured->heap != AbstractHeap::ArrayStorage)
            return std::nullopt;
        return entryParameterWithIdentityOf(measured->owner);
    };

    auto findHead = [&] {
        uint32_t admittedBytes = 0;
        BitVector headBlocks = findHeadBlocks(function, analyses, [&](BlockID id) {
            const Block& block = function.at(id);
            if (!canEndHeadBlockOf(block.terminator.kind) || block.handler || summary.blocksBoundToFrame.get(id.index()))
                return false;
            bool leavesNoTrace = std::ranges::all_of(block.values, [&](ValueID value) {
                const Effects& effects = analyses.effectsOf(value).normal;
                return (!effects.mayRunOtherCode() && effects.writes.containsOnly(bookkeepingHeaps)) || build.isSabotaged(Sabotage::HeadsThatAreCopiedLeaveNoTrace);
            });
            if (!leavesNoTrace || build.exceeds(LimitID::MaxInlineHeadBytes, uint64_t { admittedBytes } + blockBytes[id.index()], analyses.subject()))
                return false;
            admittedBytes += blockBytes[id.index()];
            return true;
        });
        bool returns = false;
        uint32_t headBytes = 0;
        for (size_t index : headBlocks) {
            returns |= function.blocks[index].terminator.kind == TerminatorKind::Return;
            headBytes += blockBytes[index];
        }
        if (!returns || !headBlocks.get(entryBlock.index()))
            return;
        summary.headBlocks = WTF::move(headBlocks);
        summary.headBytes = headBytes;
    };

    auto findDecidingParameter = [&](ValueID condition) -> std::optional<uint32_t> {
        if (!visitedValues.size())
            visitedValues.resize(function.values.size());
        visitedValues.clear();

        std::optional<uint32_t> decidingParameter;
        uint64_t chainLength = 0;
        Vector<ValueID, 16> worklist { condition };
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            if (visitedValues.contains(id.index()))
                continue;
            visitedValues.add(id.index());
            const Value& value = function.at(id);
            if (value.opcode == Opcode::Parameter) {
                std::optional<uint32_t> index = entryParameterWithIdentityOf(id);
                if (!index || (decidingParameter && *decidingParameter != *index))
                    return std::nullopt;
                decidingParameter = index;
                continue;
            }
            if (isRematerializable(value))
                continue;
            // Resolving a rope changes no result: `kind === "error"` is decided by the parameter as `kind === 1` is.
            const Effects& effects = analyses.effectsOf(id).normal;
            constexpr AbstractHeapSet heapsThatChangeNoResult = heapsWhoseWritesNobodySees - AbstractHeapSet { AbstractHeap::MemberGuards };
            bool isDecidedByOperands = effects.isRemovableWhenUnused() && effects.reads.containsOnly(heapsThatChangeNoResult);
            if (!isDecidedByOperands || build.exceeds(LimitID::MaxConditionChainLength, ++chainLength, analyses.subject(condition)))
                return std::nullopt;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                worklist.append(operand);
            });
        }
        return decidingParameter;
    };

    auto findSpreadsOfParameters = [&](NOESCAPE const Invocable<DominatedRange(BlockID)> auto& rangeAround) {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (!blockBytes[block.index()])
                continue;
            for (ValueID id : function.at(block).values) {
                const Value& spread = function.at(id);
                if (spread.opcode != Opcode::AppendSpread)
                    continue;
                const Value& list = function.at(sameObjectThroughCasts(function, spread.arrayOperand()));
                std::optional<uint32_t> entryParameter = entryParameterWithIdentityOf(spread.iterableOperand());
                if (!entryParameter || list.opcode != Opcode::NewArray)
                    continue;
                // The elements of the literal are passed either way.
                uint32_t bytesOfAllocation = estimatedCodeBytes(program, function, list) - (estimatedDirectCallBytes(list.numberOfOperands()) - estimatedDirectCallBytes(0));
                summary.spreadsOfParameters.append({ *entryParameter, { rangeAround(block), estimatedCodeBytes(program, function, spread) + bytesOfAllocation } });
            }
        }
    };

    auto findBytesBehindFailedTests = [&](NOESCAPE const Invocable<DominatedRange(BlockID)> auto& rangeAround) {
        CFG& cfg = analyses.cfg();
        BitVector failedTargets(function.blocks.size());
        Vector<ParameterCondition*, 4> tests;
        for (ParameterCondition& condition : summary.conditions) {
            BlockID failed = function.at(condition.block).terminator.notTaken().block;
            if (!cfg.singleIncomingEdge(failed) || (function.at(failed).frequency <= function.at(entryBlock).frequency).predicted())
                continue;
            failedTargets.set(failed.index());
            tests.append(&condition);
        }
        if (tests.size() < 2)
            return;

        auto& parts = summary.partsOfBytesOnlyIfSomeTestFails;
        BitVector reachedWhereTestsPass = cfg.blocksReachableFrom(cfg.root(), failedTargets);
        for (BlockID block : cfg.reversePostOrder()) {
            uint32_t bytes = blockBytes[block.index()];
            if (!bytes || reachedWhereTestsPass.get(block.index()))
                continue;
            DominatedRange range = rangeAround(block);
            bool isOwnedByOneTest = std::ranges::any_of(tests, [&](const ParameterCondition* test) {
                return test->onlyIfFalse.range.contains(range);
            });
            if (isOwnedByOneTest)
                continue;
            summary.bytesOnlyIfSomeTestFails = saturatingSum<uint32_t>(summary.bytesOnlyIfSomeTestFails, bytes);
            size_t part = parts.findIf([&](const BytesInRange& other) {
                return other.range.begin == range.begin;
            });
            if (part == notFound) {
                part = parts.size();
                parts.append({ range, 0 });
            }
            parts[part].bytes = saturatingSum<uint32_t>(parts[part].bytes, bytes);
        }
        if (!summary.bytesOnlyIfSomeTestFails)
            return;

        for (ParameterCondition* test : tests)
            test->isTestInFrontOfSharedBytes = true;
    };

    auto findConditions = [&] {
        CFG& cfg = analyses.cfg();
        for (BlockID block : cfg.reversePostOrder()) {
            auto& terminator = function.at(block).terminator;
            if (terminator.kind == TerminatorKind::Switch || terminator.kind == TerminatorKind::SwitchString) {
                std::optional<uint32_t> entryParameter = entryParameterWithIdentityOf(terminator.subjectOperand());
                bool isOnLength = !entryParameter;
                if (isOnLength)
                    entryParameter = entryParameterMeasuredBy(terminator.subjectOperand());
                if (!entryParameter)
                    continue;
                bool isOnStrings = terminator.kind == TerminatorKind::SwitchString;
                summary.switches.append(ParameterSwitch { block, *entryParameter, isOnStrings, isOnLength });
                for (size_t position = 0; position + 1 < terminator.targets.size(); ++position)
                    summary.switches.last().onlyInCase.append({ isOnStrings ? int64_t { terminator.strings()[position].index() } : terminator.cases()[position], BytesInRange { } });
                continue;
            }
            if (terminator.kind != TerminatorKind::Branch || targetDecidedByTypeOfCondition(program, function, terminator))
                continue;
            if (std::optional<uint32_t> entryParameter = findDecidingParameter(terminator.conditionOperand()))
                summary.conditions.append(ParameterCondition { block, terminator.conditionOperand(), *entryParameter });
        }
        if (summary.conditions.isEmpty() && summary.switches.isEmpty()) {
            findSpreadsOfParameters([](BlockID) {
                return DominatedRange { };
            });
            return;
        }

        struct Subtree {
            BytesInRange* own { nullptr };
            uint32_t totalBytes { 0 };
        };
        constexpr uint32_t noSubtree = std::numeric_limits<uint32_t>::max();
        Vector<Subtree, 8> subtrees;
        Vector<uint32_t> subtreeAtRoot(FillWith { }, function.blocks.size(), noSubtree);
        auto addSubtree = [&](const Target& target, BytesInRange& own) {
            if (!cfg.singleIncomingEdge(target.block))
                return;
            subtreeAtRoot[target.block.index()] = subtrees.size();
            subtrees.append(Subtree { &own });
        };
        for (ParameterCondition& condition : summary.conditions) {
            auto& terminator = function.at(condition.block).terminator;
            addSubtree(terminator.taken(), condition.onlyIfTrue);
            addSubtree(terminator.notTaken(), condition.onlyIfFalse);
        }
        for (ParameterSwitch& parameterSwitch : summary.switches) {
            auto& terminator = function.at(parameterSwitch.block).terminator;
            for (size_t position = 0; position < parameterSwitch.onlyInCase.size(); ++position)
                addSubtree(terminator.targets[position], parameterSwitch.onlyInCase[position].second);
            addSubtree(terminator.otherwise(), parameterSwitch.onlyOtherwise);
        }

        Vector<uint32_t, 16> enclosingSubtrees;
        Vector<uint32_t> innermostSubtrees(FillWith { }, function.blocks.size(), noSubtree);
        uint32_t nextPreorderIndex = 0;
        auto enter = [&](BlockID block) {
            uint32_t preorderIndex = nextPreorderIndex++;
            if (uint32_t subtree = subtreeAtRoot[block.index()]; subtree != noSubtree) {
                subtrees[subtree].own->range.begin = preorderIndex;
                enclosingSubtrees.append(subtree);
            }
            if (enclosingSubtrees.isEmpty())
                return;
            innermostSubtrees[block.index()] = enclosingSubtrees.last();
            Subtree& innermost = subtrees[enclosingSubtrees.last()];
            innermost.own->bytes = saturatingSum<uint32_t>(innermost.own->bytes, blockBytes[block.index()]);
            innermost.totalBytes = saturatingSum<uint32_t>(innermost.totalBytes, blockBytes[block.index()]);
        };
        auto leave = [&](BlockID block) {
            if (subtreeAtRoot[block.index()] == noSubtree)
                return;
            Subtree& subtree = subtrees[enclosingSubtrees.takeLast()];
            subtree.own->range.end = nextPreorderIndex;
            if (!subtree.totalBytes)
                subtree.own->range = { };
            if (!enclosingSubtrees.isEmpty()) {
                Subtree& parent = subtrees[enclosingSubtrees.last()];
                parent.totalBytes = saturatingSum<uint32_t>(parent.totalBytes, subtree.totalBytes);
            }
        };
        analyses.dominators().walkTree(enter, leave);

        auto rangeAround = [&](BlockID block) {
            uint32_t subtree = innermostSubtrees[block.index()];
            return subtree == noSubtree ? DominatedRange { } : subtrees[subtree].own->range;
        };
        auto testIn = [&](BlockID block, ValueID subject) {
            BytesInRange test { rangeAround(block), 0 };
            if (!blockBytes[block.index()])
                return test;
            test.bytes = estimatedCodeBytes(function.at(block).terminator);
            for (ValueID id = subject; function.at(block).values.contains(id); id = function.at(id).operand(0)) {
                test.bytes += estimatedCodeBytes(program, function, function.at(id));
                if (!isCast(function.at(id).opcode))
                    break;
            }
            return test;
        };
        for (ParameterCondition& condition : summary.conditions)
            condition.test = testIn(condition.block, condition.condition);
        for (ParameterSwitch& parameterSwitch : summary.switches)
            parameterSwitch.test = testIn(parameterSwitch.block, function.at(parameterSwitch.block).terminator.subjectOperand());
        findBytesBehindFailedTests(rangeAround);
        findSpreadsOfParameters(rangeAround);

        summary.conditions.removeAllMatching([](const ParameterCondition& condition) {
            return condition.onlyIfTrue.range.isEmpty() && condition.onlyIfFalse.range.isEmpty() && !condition.isTestInFrontOfSharedBytes;
        });
    };

    auto isRepeatedBuiltinCallback = [&](const Use& use) -> bool {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        if (user.opcode != Opcode::CallBuiltin)
            return false;
        BuiltinOperand operand = builtinOperandAt(user, use.operandIndex);
        return std::ranges::any_of(program.builtins().at(user.overload()).flow.callbacks, [&](const BuiltinCallback& callback) {
            return callback.callee == operand && isRepeated(callback.times);
        });
    };

    auto declaredUsesAt = [&](const Use& use) -> OperandUses {
        switch (use.kind) {
        case Use::Kind::Operand:
            return function.at(use.user).operandUses(use.operandIndex);
        case Use::Kind::TerminatorOperand:
            return function.at(use.block).terminator.operandUses(use.operandIndex);
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    // The unknown summary claims every calling use. Code that nobody sees is as unseen after inlining, so there only the operation's own declaration counts.
    auto isCalledAt = [&](const ObjectUse& objectUse) -> bool {
        if (!objectUse.uses.containsAny(callingUses))
            return false;
        return !objectUse.uses.containsAll(callingUses) || declaredUsesAt(objectUse.use).containsAny(callingUses);
    };

    auto findCalledAndIteratedParameters = [&] {
        auto& entryParameters = function.at(entryBlock).parameters;
        auto mayBeFunction = [&](ValueID parameter) {
            return program.typeOf(function, parameter).mayBeFunction();
        };
        bool mayCallParameter = entryParameters.containsIf(mayBeFunction);
        bool hasLoop = std::ranges::any_of(function.blocks, [&](const Block& block) {
            return std::ranges::any_of(block.values, [&](ValueID id) {
                Opcode opcode = function.at(id).opcode;
                return opcode == Opcode::IteratorOpen || opcode == Opcode::StepGenerator;
            });
        });
        if (!mayCallParameter && !hasLoop)
            return;

        const EscapeAnalysis& escape = analyses.escape();
        if (escape.gaveUp())
            return;
        const BlockCycles* cycles = mayCallParameter ? &analyses.blockCycles() : nullptr;
        auto iterates = [&](const ObjectUse& objectUse) {
            if (objectUse.use.kind != Use::Kind::Operand)
                return false;
            const Value& user = function.at(objectUse.use.user);
            uint32_t operandIndex = objectUse.use.operandIndex;
            if (user.opcode == Opcode::StepGenerator)
                return operandIndex == OperandIndex::StepGenerator::generator;
            return user.opcode == Opcode::IteratorOpen && operandIndex == OperandIndex::IteratorOpen::iterable && user.iteratorKind() == IteratorKind::Sync;
        };
        for (uint32_t i = 0; i < entryParameters.size(); ++i) {
            bool mayBeCalled = mayCallParameter && mayBeFunction(entryParameters[i]);
            if (!hasLoop && !mayBeCalled)
                continue;
            std::optional<ObjectID> object = escape.objectDefinedBy(entryParameters[i]);
            if (!object)
                continue;
            if (hasLoop && std::ranges::any_of(escape.usesOf(*object), iterates))
                summary.iteratedParameters.set(i);
            if (!mayBeCalled)
                continue;
            for (const ObjectUse& objectUse : escape.usesOf(*object)) {
                if (!isCalledAt(objectUse))
                    continue;
                summary.calledParameters.set(i);
                if (cycles->isInCycle(objectUse.use.block) || isRepeatedBuiltinCallback(objectUse.use)) {
                    summary.parametersCalledInCycles.set(i);
                    break;
                }
            }
        }
    };

    auto findReceiverParameters = [&] {
        auto& entryParameters = function.at(entryBlock).parameters;
        Vector<ReceiverParameter, 8> receivers(entryParameters.size(), [](size_t index) {
            return ReceiverParameter { static_cast<uint32_t>(index), 0, 0 };
        });
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (isSlowPath(function, function.at(block)).predicted())
                continue;
            for (ValueID id : function.at(block).values) {
                const Value& access = function.at(id);
                if (access.opcode == Opcode::ElemGet) {
                    if (std::optional<uint32_t> key = entryParameterBehind(analyses.identityOf(access.propertyOperand()))) {
                        size_t position = summary.keyParameters.findIf([&](const KeyParameter& other) { return other.entryParameter == *key; });
                        if (position == notFound) {
                            position = summary.keyParameters.size();
                            summary.keyParameters.append({ *key, 0 });
                        }
                        ++summary.keyParameters[position].accesses;
                    }
                    continue;
                }
                bool isStore = isStoreByKey(access.opcode);
                if (!isNamedAccess(access.opcode) && !isStore)
                    continue;
                ValueID receiver = access.operand(*receiverOf(access.opcode));
                std::optional<uint32_t> index = entryParameterBehind(analyses.identityOf(receiver));
                if (!index)
                    continue;
                receivers[*index].stores += isStore;
                receivers[*index].accessesByName += isNamedAccess(access.opcode) && !program.typeOf(function, receiver).layouts;
            }
        }
        for (const ReceiverParameter& receiver : receivers) {
            if (receiver.accessesByName || receiver.stores)
                summary.receiverParameters.append(receiver);
        }
    };

    summary.barrier = computeBarrier();
    if (summary.barrier == InlineBarrier::NoBody)
        return summary;

    const CFG& cfg = analyses.cfg();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        bool isBound = boundToOf(block.terminator.kind) == BoundTo::Frame;
        for (uint32_t index = 0; !isBound && index < block.values.size(); ++index)
            isBound = isBoundToFrame(function, block, index);
        if (isBound)
            summary.blocksBoundToFrame.set(id.index());
        if (block.terminator.kind == TerminatorKind::Return && !block.terminator.valueOperand())
            summary.hasBareReturn = true;
    }
    summary.noReturnBlocks = computeNoReturnBlocks(program, function, cfg, [](FunctionID) {
        return false;
    });
    summary.neverReturns = summary.noReturnBlocks.get(entryBlock.index());
    if (summary.barrier != InlineBarrier::None)
        return summary;

    blockBytes.fill(0, function.blocks.size());
    for (BlockID id : cfg.reversePostOrder()) {
        if (isSlowPath(function, function.at(id)).predicted() || onlyThrowsCaughtValueAgain(function, function.at(id)))
            continue;
        blockBytes[id.index()] = estimatedCodeBytes(program, function, function.at(id));
    }
    takeOffWhatDecidedBranchesCutOff(program, function, cfg, blockBytes);
    estimateComparisonChainsAsSwitches(analyses, blockBytes);
    for (uint32_t bytes : blockBytes)
        summary.codeBytes = saturatingSum<uint32_t>(summary.codeBytes, bytes);
    summary.likelyCodeBytes = computeLikelyBytes();
    summary.hasCycle = cfg.hasCycle();
    summary.isWrapper = isWrapperCreatedByEngine(function);
    // A loop of the library runs once for each element, however seldom the function is entered: a caller asks what it does with a function that it is handed.
    if (isCold(heat).predicted() && !(function.role == FunctionRole::Library && cfg.hasCycle()))
        return summary;

    if (function.results.size() == 1) {
        findHead();
        findConditions();
    }
    findCalledAndIteratedParameters();
    findReceiverParameters();
    return summary;
}

InliningSummaries summarizeForInlining(const Program& program, const CallGraph& callGraph, const Heat& heat, const FlowResults& flow, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> largestFirst = largeFunctions(program, loops);
    InliningSummaries result;
    result.m_summaries = ParallelArray<InliningSummary>(loops, program.functions.size(), largestFirst.span(), [&](unsigned index) {
        FunctionID id(index);
        if (!build.isEnabled(PassID::SummarizeForInlining, id))
            return InliningSummary { };
        AnalysisContext context { build, program, &flow, nullptr, id };
        return summarizeFunctionForInlining(context, program.at(id), heat.of(program, id));
    });

    ParallelArray<InliningSummary>& summaries = result.m_summaries;
    Vector<FunctionID> worklist = findIndicesInParallel(loops, summaries.size(), [&](unsigned index) {
        return summaries[index].neverReturns;
    }).map([](uint32_t index) {
        return FunctionID(index);
    });
    BitVector noReturnFunctions(summaries.size());
    for (FunctionID function : worklist)
        noReturnFunctions.quickSet(function.index());
    auto neverReturns = [&](FunctionID callee) {
        return noReturnFunctions.get(callee.index());
    };
    while (!worklist.isEmpty()) {
        Vector<FunctionID> callers;
        for (FunctionID callee : worklist) {
            for (FunctionID caller : callGraph.callersOf(callee)) {
                const InliningSummary& summary = summaries[caller.index()];
                if (!summary.neverReturns && summary.barrier != InlineBarrier::Unknown && summary.barrier != InlineBarrier::NoBody)
                    callers.append(caller);
            }
        }
        sortAndRemoveDuplicates(loops, callers);
        forEachIndexInParallel(loops, callers.size(), [&](unsigned index) {
            const Function& function = program.at(callers[index]);
            summaries[callers[index].index()].noReturnBlocks = computeNoReturnBlocks(program, function, CFG(function), neverReturns);
        });

        worklist.shrink(0);
        for (FunctionID caller : callers) {
            InliningSummary& summary = summaries[caller.index()];
            if (!summary.noReturnBlocks.get(entryBlock.index()))
                continue;
            summary.neverReturns = true;
            noReturnFunctions.quickSet(caller.index());
            worklist.append(caller);
        }
    }

    result.m_build = &build;
    result.m_flow = &flow;
    result.m_locksOfAnswers = makeUniqueWithoutFastMallocCheck<std::array<Lock, InliningSummaries::numberOfLocks>>();
    return result;
}

static bool returnsFreshAggregateAskedOnce(const InliningSummary& summary, const AnalysisContext& context)
{
    using Answer = InliningSummary::Answer;
    AtomicRef answer(summary.returnsFreshAggregate);
    Answer known = answer.load(std::memory_order_relaxed);
    if (known == Answer::NotAsked) {
        FunctionAnalyses analyses(context, context.program.at(context.id));
        known = returnsFreshAggregate(analyses) ? Answer::Yes : Answer::No;
        answer.store(known, std::memory_order_relaxed);
    }
    return known == Answer::Yes;
}

bool InliningSummaries::returnsFreshAggregate(const Program& program, FunctionID id) const
{
    using Answer = InliningSummary::Answer;
    RELEASE_ASSERT(contains(id));
    const InliningSummary& summary = m_summaries[id.index()];
    if (summary.barrier != InlineBarrier::None)
        return false;

    Answer known = AtomicRef(summary.returnsFreshAggregate).load(std::memory_order_relaxed);
    if (known != Answer::NotAsked)
        return known == Answer::Yes;

    Locker locker { (*m_locksOfAnswers)[id.index() % numberOfLocks] };
    return returnsFreshAggregateAskedOnce(summary, AnalysisContext { *m_build, program, m_flow, nullptr, id });
}

static void dumpIndices(PrintStream& out, ASCIILiteral opening, ASCIILiteral prefix, const BitVector& indices)
{
    CommaPrinter separator(" "_s, opening);
    for (size_t index : indices)
        out.print(separator, prefix, index);
    if (separator.didPrint())
        out.print("]");
}

void InliningSummaries::dump(PrintStream& out, const Program& program) const
{
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody())
            continue;
        const InliningSummary& summary = of(program, id);
        out.print("f", index, ": barrier=", nameOf(summary.barrier), " bytes=", summary.codeBytes);
        dumpIndices(out, " head=["_s, "b"_s, summary.headBlocks);
        if (!summary.headBlocks.isEmpty())
            out.print(" headBytes=", summary.headBytes);
        dumpIndices(out, " noReturn=["_s, "b"_s, summary.noReturnBlocks);
        out.print(summary.neverReturns ? " neverReturns" : "");
        dumpIndices(out, " boundToFrame=["_s, "b"_s, summary.blocksBoundToFrame);
        out.print(summary.hasBareReturn ? " bareReturn" : "", summary.isWrapper ? " wrapper" : "", contains(id) && returnsFreshAggregate(program, id) ? " returnsFreshAggregate" : "");
        dumpIndices(out, " called=["_s, ""_s, summary.calledParameters);
        dumpIndices(out, " calledInCycles=["_s, ""_s, summary.parametersCalledInCycles);
        dumpIndices(out, " iterated=["_s, ""_s, summary.iteratedParameters);
        for (const ParameterCondition& condition : summary.conditions) {
            out.print(" condition(v", condition.condition.index(), " p", condition.entryParameter, " true=", condition.onlyIfTrue.bytes, " false=", condition.onlyIfFalse.bytes, ")");
            if (condition.isTestInFrontOfSharedBytes)
                out.print(" guardsSharedBytes(v", condition.condition.index(), ")");
        }
        for (const ParameterSwitch& parameterSwitch : summary.switches) {
            out.print(" switch(p", parameterSwitch.entryParameter, parameterSwitch.isOnLength ? ".length" : "");
            for (auto& [key, code] : parameterSwitch.onlyInCase)
                out.print(parameterSwitch.isOnStrings ? " s" : " ", key, "=", code.bytes);
            out.print(" otherwise=", parameterSwitch.onlyOtherwise.bytes, ")");
        }
        if (summary.bytesOnlyIfSomeTestFails)
            out.print(" someTestFails=", summary.bytesOnlyIfSomeTestFails);
        for (const SpreadOfParameter& spread : summary.spreadsOfParameters)
            out.print(" spread(p", spread.entryParameter, " bytes=", spread.code.bytes, ")");
        for (const ReceiverParameter& receiver : summary.receiverParameters)
            out.print(" receiver(p", receiver.entryParameter, " accesses=", receiver.accessesByName, " stores=", receiver.stores, ")");
        for (const KeyParameter& key : summary.keyParameters)
            out.print(" key(p", key.entryParameter, " accesses=", key.accesses, ")");
        out.print("\n");
    }
}

size_t inlineFrameCountOf(const Function& function)
{
    return std::max<size_t>(function.inlineFrames.size(), 1);
}

uint32_t appendInlineFrames(Function& caller, const InlineFrame& callFrame, const Function& callee)
{
    Vector<InlineFrame>& frames = caller.inlineFrames;
    if (frames.isEmpty())
        frames.append(InlineFrame { });

    uint32_t firstFrame = frames.size();
    frames.append(callFrame);
    for (size_t index = 1; index < callee.inlineFrames.size(); ++index) {
        InlineFrame frame = callee.inlineFrames[index];
        frame.parent += firstFrame;
        frames.append(frame);
    }
    return firstFrame;
}

ValueID upcastToParameterType(IRBuilder& builder, const Value& parameter, ValueID argument)
{
    const Program& program = builder.context().program;
    // A cast takes a JavaScript value, and a raw word names the value it was made from as its base.
    if (!program.at(parameter.type).isJavaScriptValue())
        return argument;

    ValueID converted = builder.appendUpcast(argument, parameter.type);
    TypeID expected = parameter.expected().predicted();
    if (!expected || expected == builder.function().at(converted).expected().predicted())
        return converted;

    if (program.typeOf(builder.function(), converted).isSubsetOf(program.at(expected), program))
        return converted;

    // Of two expectations that exclude each other the one nearer to the use stands.
    TypeArena& types = builder.types();
    TypeID proven = builder.function().at(converted).type;
    TypeID narrowed = types.intersectionOf(builder.function().at(converted).expectedOrType().predicted(), expected);
    if (narrowed == TypeTable::never)
        narrowed = types.intersectionOf(proven, expected);
    // An intersection may keep a part of one side that the subset test does not find in the other.
    if (narrowed == TypeTable::never || narrowed == proven || !types.at(narrowed).isSubsetOf(types.at(proven), program))
        return converted;

    // What the copy expects of the value stands on a cast that changes nothing else: the caller's other uses of the argument expect what the caller does.
    Value cast = IRBuilder::make(Opcode::Upcast, { converted });
    cast.setCastType(proven);
    cast.setExpected(ExpectedType(narrowed));
    return builder.append(WTF::move(cast));
}

bool takesFactOfEntry(const Program& program, const Value& value, bool calleeIsPredicted)
{
    const LocationFlow* flow = flowOfVariableReadBy(program, value);
    return flow && holdsDeclaredTypeOnlyBehindEntry(*flow, calleeIsPredicted) && !namesFactOrGivesWordThatMayBeAnything(value);
}

ValueID readWordAndAssumeInPlace(Function& function, TypeArena& types, ValueID id, ValueID fact)
{
    Value word = function.at(id);
    word.setMayPrecedeItsStore(true);
    word.setLeansOnEntryOfBody(false);
    word.type = types.intern(types.at(word.type).withPrimitives(Primitive::Unspecified));
    word.setExpected({ });
    ValueID read = function.add(WTF::move(word));

    Value& value = function.edit(id, ValueEdit::Definition);
    value.becomeAssumeType(read, Proven::behindFact(value.type));
    value.setFactOperand(fact);
    return read;
}

// A function that is declared beside others lies in a field of the environment that it captures.
static ValueID sameEnvironment(const Program& program, const Function& function, const TypeArena& types, ValueID value)
{
    while (true) {
        value = sameObjectBehindFacts(function, value);
        const Value& capture = function.at(value);
        if (capture.opcode != Opcode::GetCapture)
            return value;
        const Value& closure = function.at(sameObjectBehindFacts(function, capture.closureOperand()));
        if (closure.opcode != Opcode::GetEnvironmentField || closure.mayPrecedeItsStore() || types.at(capture.type).environment != closure.layout())
            return value;
        if (!flowOf(program, { closure.layout(), closure.field() }).location.holdsClosureOverOwnEnvironment)
            return value;
        value = closure.environmentOperand();
    }
}

// A field that is stored before any function that captures the environment can run is stored wherever a function is entered with the environment, and what is stored stays stored.
// A part of the function that makes the environment may be entered with it too: there a store stands in front of a call that flow has followed, and no read goes ahead of a store to what it reads.
static bool leansOnEntryOfBody(const Program& program, const Function& function, const TypeArena& types, const Value& read)
{
    return flowOf(program, { read.layout(), read.field() }).location.isStoredBeforeCapturersRun
        && function.at(entryBlock).parameters.contains(sameEnvironment(program, function, types, read.environmentOperand()));
}

void nameFactOfEntry(const Program& program, Function& function, TypeArena& types, std::span<const BlockID> blocks, ValueID fact, bool calleeIsPredicted)
{
    Vector<ValueID, 8> readsOfFields;
    for (BlockID block : blocks) {
        for (ValueID id : function.at(block).values) {
            if (!takesFactOfEntry(program, function.at(id), calleeIsPredicted))
                continue;
            if (function.at(id).opcode == Opcode::GetModuleVariable)
                function.edit(id).setFactOperand(fact);
            else if (!calleeIsPredicted && leansOnEntryOfBody(program, function, types, function.at(id)))
                function.edit(id).setLeansOnEntryOfBody(true);
            else
                readsOfFields.append(id);
        }
    }
    if (readsOfFields.isEmpty())
        return;

    auto takesFact = [&](ValueID id) {
        return readsOfFields.contains(id);
    };
    for (BlockID block : blocks) {
        if (std::ranges::none_of(function.at(block).values, takesFact))
            continue;
        BlockValues values;
        values.reserveInitialCapacity(function.at(block).values.size() + 1);
        for (ValueID id : function.at(block).values) {
            if (takesFact(id))
                values.append(readWordAndAssumeInPlace(function, types, id, fact));
            values.append(id);
        }
        function.edit(block, BlockEdit::Values).values = WTF::move(values);
    }
}

void nameFactsWhereReadsStand(const Program& program, Function& function, TypeArena& types)
{
    if (function.inlineFrames.size() <= 1)
        return;
    auto needsFact = [&](ValueID id) {
        const Value& value = function.at(id);
        bool leansOnEntryOfBody = value.opcode == Opcode::GetEnvironmentField && value.leansOnEntryOfBody();
        return value.inlineFrame && !leansOnEntryOfBody && holdsDeclaredTypeOnlyBehindEntry(program, function, value) && !namesFactOrGivesWordThatMayBeAnything(value);
    };
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (std::ranges::none_of(function.at(block).values, needsFact))
            continue;
        BlockValues values;
        values.reserveInitialCapacity(function.at(block).values.size() + 2);
        for (ValueID id : function.at(block).values) {
            if (needsFact(id)) {
                Value fact = IRBuilder::make(Opcode::Entered);
                fact.setFunction(function.inlineFrames[function.at(id).inlineFrame].callee);
                fact.type = types.intern(Type::forOpaque(OpaqueKind::Fact));
                fact.setOrigin(function.at(id).origin());
                ValueID entered = function.add(WTF::move(fact));
                values.append(entered);
                if (function.at(id).opcode == Opcode::GetModuleVariable)
                    function.edit(id).setFactOperand(entered);
                else
                    values.append(readWordAndAssumeInPlace(function, types, id, entered));
            }
            values.append(id);
        }
        function.edit(block, BlockEdit::Values).values = WTF::move(values);
    }
}

// What a function says of what it is handed holds from its entry on. In a copy the value is the caller's, which has it at hand earlier.
// A cast of a word that may be anything only hands the word on: with a fact it would say that the word is a value.
bool narrowsWhatFunctionIsHanded(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::AssumeType || value.factOperand() || program.typeOf(function, value.valueOperand()).mayBeUnspecified())
        return false;
    RootThroughCasts handed = sameObjectThroughCasts(function, value.valueOperand());
    return function.at(handed).opcode == Opcode::Parameter && function.at(entryBlock).parameters.contains(handed.valueInFrontOfFacts());
}

void cloneBlocksIntoInlineFrame(IRBuilder& builder, RegionCloner& cloner, std::span<const BlockID> region, uint32_t firstFrame)
{
    const Program& program = builder.context().program;
    const Function& source = cloner.source();
    bool calleeIsPredicted = liesInCopyOfPredictedCallee(builder.function(), firstFrame);
    bool needsFact = std::ranges::any_of(region, [&](BlockID block) {
        return std::ranges::any_of(source.at(block).values, [&](ValueID id) {
            return takesFactOfEntry(program, source.at(id), calleeIsPredicted) || narrowsWhatFunctionIsHanded(program, source, source.at(id));
        });
    });
    ValueID entered;
    if (needsFact) {
        Value fact = IRBuilder::make(Opcode::Entered);
        fact.setFunction(builder.function().inlineFrames[firstFrame].callee);
        entered = builder.append(WTF::move(fact));
    }
    cloner.cloneBlocks(region, [&](ValueID original, Value& clone) {
        clone.inlineFrame += firstFrame;
        if (clone.opcode == Opcode::GetEnvironmentField)
            clone.setLeansOnEntryOfBody(false);
        if (narrowsWhatFunctionIsHanded(program, source, source.at(original)))
            clone.setFactOperand(entered);
    });
    for (BlockID id : cloner.clonedBlocks())
        builder.function().edit(id, BlockEdit::Terminator).terminator.inlineFrame += firstFrame;
    if (entered)
        nameFactOfEntry(program, builder.function(), builder.types(), cloner.clonedBlocks(), entered, calleeIsPredicted);
}

// No program writes a construction in tail position. One that stands there begins the construction of the frame again, in place of the frame.
void keepFrameBelowTailCalls(Function& function, std::span<const BlockID> blocks, uint32_t frame)
{
    for (BlockID block : blocks) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            bool beginsAgain = value.opcode == Opcode::CallDirect && value.isConstruction();
            if (value.inlineFrame == frame && value.isTailCall && !beginsAgain)
                function.edit(id, ValueEdit::Definition).isTailCall = false;
        }
    }
    // A frame stands behind its parent.
    for (size_t index = frame + 1; index < function.inlineFrames.size(); ++index) {
        if (function.inlineFrames[index].parent == frame && !function.inlineFrames[index].isConstruction)
            function.inlineFrames[index].isTailCall = false;
    }
}

static ValueID argumentAt(const Value& call, size_t entryParameter)
{
    if (entryParameter >= call.numberOfOperands())
        return { };
    return call.operand(static_cast<uint32_t>(entryParameter));
}

static bool passesArgumentThat(const Value& call, const BitVector& entryParameters, NOESCAPE const Invocable<bool(ValueID, uint32_t)> auto& holds)
{
    for (size_t entryParameter : entryParameters) {
        ValueID argument = argumentAt(call, entryParameter);
        if (argument && holds(argument, static_cast<uint32_t>(entryParameter)))
            return true;
    }
    return false;
}

static bool hasSignatureMismatch(const FunctionPassContext& context, const InliningSummary& summary, const Value& call, const Function& callee)
{
    const Program& program = context.program;
    if (call.numberOfOperands() != callee.numberOfEntryParameters())
        return true;
    if (callee.results.size() != 1)
        return summary.hasBareReturn;

    TypeID resultType = TypingContext { program, callee, context.types }.typeIDOf(callee.result());
    if (resultType != call.type && !program.at(resultType).isSubsetOf(program.at(call.type), program))
        return true;
    return summary.hasBareReturn && !program.at(TypeTable::undefined).isSubsetOf(program.at(resultType), program);
}

// A chain of array methods whose links have places of their own calls its step from a frame of that step, which holds nothing else and is no copy of the step.
static bool isSiteOfChain(const Program& program, const Function& caller, const Value& call)
{
    return call.inlineFrame && caller.inlineFrames[call.inlineFrame].callee == call.function() && program.at(call.function()).role == FunctionRole::ChainStep;
}

// A function that calls what it is given does something else with each function it is given: a traversal in the callback of the same traversal repeats nothing.
static bool closesCycle(const FunctionPassContext& context, const InliningSummary& summary, const Value& call)
{
    FunctionID callee = call.function();
    if (callee == context.id)
        return true;
    bool differsByCall = passesArgumentThat(call, summary.calledParameters, [&](ValueID argument, uint32_t) {
        return !!context.program.typeOf(context.function, argument).singleFunction();
    });
    const Vector<InlineFrame>& frames = context.function.inlineFrames;
    auto functionOf = [&](uint32_t frame) {
        return frame ? frames[frame].callee : context.id;
    };
    uint32_t innermostFrame = isSiteOfChain(context.program, context.function, call) ? frames[call.inlineFrame].parent : call.inlineFrame;
    for (uint32_t frame = innermostFrame; frame; frame = frames[frame].parent) {
        if (frames[frame].callee != callee)
            continue;
        if (!differsByCall || (frames[frame].callPosition == call.position && functionOf(frames[frame].parent) == functionOf(call.inlineFrame)))
            return true;
    }
    return false;
}

InlineDecision canInline(const FunctionPassContext& context, const InliningSummary& summary, ValueID callID)
{
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    if (summary.barrier == InlineBarrier::None) {
        const Function& callee = context.program.at(call.function());
        if (hasSignatureMismatch(context, summary, call, callee))
            return InlineDecision::SignatureMismatch;
        if (context.build.exceeds(LimitID::InlineFramesPerFunction, inlineFrameCountOf(caller) + inlineFrameCountOf(callee), context.subject(callID)))
            return InlineDecision::CallerSizeLimit;
    }
    if (closesCycle(context, summary, call))
        return InlineDecision::ClosesCycle;
    return InlineDecision::Unknown;
}

// One traversal runs the callbacks of all methods of a chain. A trace that is taken in the callback of a link names the link and the place of its call, so the calls of each link lie in a frame of the traversal of their own, below the frame of the link.
static void placeCallsOfLinks(const Program& program, Function& caller, const Function& traversal, std::span<const BlockID> region, const RegionCloner& cloner, const Value& call)
{
    if (!call.inlineFrame)
        return;
    const InlineFrame step = caller.inlineFrames[call.inlineFrame];
    if (!step.parent || caller.inlineFrames[step.parent].callee != step.callee || program.at(step.callee).role != FunctionRole::ChainStep)
        return;
    unsigned numberOfLinks = ChainShape::decode(program.at(step.callee).derivation.parameter).numberOfLinks();
    RELEASE_ASSERT(numberOfLinks < step.parent);
    uint32_t frameOfFirstLink = step.parent - numberOfLinks;

    // The traversal reads each callback from the list of its operands, the one list that it reads at constant positions, where it calls it.
    static constexpr int64_t isNoRead = -1;
    auto positionInOperands = [&](ValueID id) {
        const Value& value = traversal.at(id);
        if (value.opcode == Opcode::GetArrayElement)
            return integerConstantThroughCasts(program, traversal, value.positionOperand()).value_or(isNoRead);
        if (value.opcode == Opcode::ElemGet)
            return integerConstantThroughCasts(program, traversal, value.propertyOperand()).value_or(isNoRead);
        return isNoRead;
    };
    // A read with more than one path arrives as a parameter of the block that holds the call.
    Vector<std::optional<int64_t>> positionOfParameter(traversal.values.size());
    for (BlockID block : region) {
        for (const Target& target : traversal.at(block).terminator.targets) {
            const auto& parameters = traversal.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                int64_t position = positionInOperands(sameObjectThroughCasts(traversal, target.arguments[index]).valueInFrontOfFacts());
                std::optional<int64_t>& known = positionOfParameter[parameters[index].index()];
                known = known.value_or(position) == position ? position : isNoRead;
            }
        }
    }

    std::array<uint32_t, ChainShape::maxNumberOfLinks> frames { };
    for (BlockID block : region) {
        for (ValueID id : traversal.at(block).values) {
            const Value& value = traversal.at(id);
            if (value.opcode != Opcode::Call || value.inlineFrame)
                continue;
            ValueID callback = sameObjectThroughCasts(traversal, value.calleeOperand()).valueInFrontOfFacts();
            int64_t position = positionOfParameter[callback.index()].value_or(positionInOperands(callback));
            if (position < 0 || position % 2 || position >= 2 * static_cast<int64_t>(numberOfLinks))
                continue;
            uint32_t& frame = frames[position / 2];
            if (!frame) {
                uint32_t frameOfLink = frameOfFirstLink + static_cast<uint32_t>(position / 2);
                caller.inlineFrames.append(InlineFrame { .callee = call.function(), .callPosition = program.at(caller.inlineFrames[frameOfLink].callee).position, .parent = frameOfLink, .module = traversal.module });
                frame = caller.inlineFrames.size() - 1;
            }
            caller.edit(cloner.cloneOf(id), ValueEdit::Definition).inlineFrame = frame;
        }
    }
}

// Copies the body that is given, or else the body of the function that the call names: the head blocks that are given, or else all of it.
static void copyBodyIn(IRBuilder& builder, ProgramPoint point, const Function* body, const BitVector* headBlocks, const BitVector& noReturnBlocks, IRBuilder::DeferredResultReads* resultReads)
{
    const Program& program = builder.context().program;
    Function& caller = builder.function();
    ValueID callID = caller.at(point.block).values[point.index];

    std::optional<Terminator> siteReturnCall;
    if (const Terminator& terminator = caller.at(point.block).terminator; terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == callID) {
        siteReturnCall = terminator;
        builder.demoteReturnCall(point.block);
    }

    const Value call = caller.at(callID);
    RELEASE_ASSERT(call.opcode == Opcode::CallDirect);
    // The task that asked for a derived function first built it, with ids of its own arena.
    std::optional<Function> calleeWithAdoptedIDs;
    if (!body && ProvisionalFunctions::contains(call.function())) {
        calleeWithAdoptedIDs.emplace(program.at(call.function()));
        adoptIDs(*calleeWithAdoptedIDs, builder.types());
        body = &*calleeWithAdoptedIDs;
    }
    const Function& callee = body ? *body : program.at(call.function());
    Expected<BlockFrequency> frequencyOfCall = caller.at(point.block).frequency;
    uint32_t firstFrame = appendInlineFrames(caller, InlineFrame {
        .callee = call.function(),
        .callPosition = call.position,
        .parent = call.inlineFrame,
        .isTailCall = !!call.isTailCall,
        .isConstruction = call.isConstruction(),
        .callIsPartOfConstruction = !!call.isPartOfConstruction,
        .callsOwnFunctionObject = call.callsOwnFunctionObject(),
        .calleeIsPredicted = call.calleeIsPredicted(),
        .module = callee.module,
    }, callee);
    IRBuilder::Expansion expansion = resultReads ? builder.beginExpansion(point, *resultReads) : builder.beginExpansion(point);

    RegionCloner cloner(builder, callee);
    const auto& parameters = callee.at(entryBlock).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index)
        cloner.mapValue(parameters[index], upcastToParameterType(builder, callee.at(parameters[index]), call.operand(index)));

    CFG cfg(callee);
    Vector<BlockID, 8> region;
    BlockID fullCallBlock;
    if (!headBlocks)
        region.append(cfg.reversePostOrder());
    else {
        fullCallBlock = builder.addBlock(rarerOf(BlockFrequency::Unlikely, frequencyOfCall));
        for (BlockID block : cfg.reversePostOrder()) {
            if (!headBlocks->get(block.index()))
                continue;
            region.append(block);
            for (const Target& target : callee.at(block).terminator.targets) {
                if (!headBlocks->get(target.block.index()) && !cloner.cloneOf(target.block))
                    cloner.mapBlock(target.block, fullCallBlock);
            }
        }
    }
    cloneBlocksIntoInlineFrame(builder, cloner, region.span(), firstFrame);
    placeCallsOfLinks(program, caller, callee, region.span(), cloner, call);
    // No stack trace lists the callee, so the built-in function that the call names stands right below what the callee calls.
    if (BuiltinID calledBuiltin = call.calledBuiltin(); calledBuiltin && callee.isHiddenInStackTraces()) {
        for (BlockID block : cloner.clonedBlocks()) {
            for (ValueID id : caller.at(block).values) {
                if (Value::opcodesWithCalledBuiltin.contains(caller.at(id).opcode) && !caller.at(id).calledBuiltin())
                    caller.edit(id).setCalledBuiltin(calledBuiltin);
            }
        }
    }
    builder.emitJump(cloner.cloneOf(entryBlock));

    auto replaceReturn = [&](BlockID block) {
        ValueID returned = std::exchange(caller.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { }).valueOperand();
        builder.moveToEndOf(block);
        if (callee.results.size() == 1) {
            builder.endExpansion(expansion, returned ? returned : builder.appendUndefined());
            return;
        }

        RELEASE_ASSERT(returned);
        Vector<ValueID, 4> results;
        if (caller.at(returned).opcode == Opcode::Results) {
            results.append(caller.at(returned).valuesOperands());
            const BlockValues& values = caller.at(block).values;
            if (!values.isEmpty() && values.last() == returned) {
                BitVector indexes;
                indexes.set(values.size() - 1);
                builder.eraseValues(block, indexes);
            }
        } else {
            for (uint32_t index = 0; index < expansion.results.size(); ++index) {
                Value result = IRBuilder::make(Opcode::ResultAt, { returned });
                result.setIndex(index);
                results.append(builder.append(WTF::move(result)));
            }
        }
        builder.endExpansion(expansion, results.span());
    };

    auto finishTerminator = [&](BlockID clone) {
        switch (caller.at(clone).terminator.kind) {
        case TerminatorKind::Return:
            replaceReturn(clone);
            return;
        case TerminatorKind::ReturnCall: {
            if (!siteReturnCall) {
                builder.demoteReturnCall(clone);
                replaceReturn(clone);
                return;
            }
            Terminator& returnCall = caller.edit(clone, BlockEdit::Terminator).terminator;
            Vector<TailCast, 4> casts;
            for (TailCast cast : returnCall.casts())
                casts.append(cast);
            for (TailCast cast : siteReturnCall->casts())
                casts.append(cast);
            returnCall.setCasts(casts.span());
            return;
        }
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    builder.setOrigin(call.origin());
    std::span<const BlockID> clones = cloner.clonedBlocks();
    for (size_t index = 0; index < clones.size(); ++index) {
        Block& block = caller.edit(clones[index], { BlockEdit::Arguments, BlockEdit::Rest });
        block.frequency = rarerOf(block.frequency, frequencyOfCall);
        if (noReturnBlocks.get(region[index].index()))
            block.frequency = BlockFrequency::Rare;
        for (Target& target : block.terminator.targets) {
            if (target.block != fullCallBlock)
                continue;
            target.arguments.clear();
            target.proofs.clear();
        }
        finishTerminator(clones[index]);
    }
    if (call.isConstruction())
        keepFrameBelowTailCalls(caller, cloner.clonedBlocks(), firstFrame);

    if (fullCallBlock) {
        Origin origin = call.origin();
        // The ReturnCall that was demoted at the site owns this copy of the call.
        if (siteReturnCall)
            origin.isTailCall = false;
        builder.setOrigin(origin);
        builder.moveToEndOf(fullCallBlock);
        Value fullCall = call;
        fullCall.setIsAfterInlinedHead(true);
        fullCall.isTailCall = origin.isTailCall;
        ValueID result = builder.append(WTF::move(fullCall));
        if (siteReturnCall) {
            siteReturnCall->setCallOperand(result);
            builder.emitTerminator(WTF::move(*siteReturnCall));
        } else
            builder.endExpansion(expansion, result);
    }
    builder.setHandler(std::nullopt);
}

void copyIn(FunctionPassContext& context, IRBuilder& builder, const InlineRequest& request)
{
    RELEASE_ASSERT(&builder.function() == &context.function);
    RELEASE_ASSERT(context.function.at(request.point.block).values[request.point.index] == request.call);
    const BitVector* headBlocks = nullptr;
    switch (request.part) {
    case InlinedPart::WholeBody:
        break;
    case InlinedPart::Head:
        headBlocks = &request.callee.headBlocks;
        break;
    }
    copyBodyIn(builder, request.point, nullptr, headBlocks, request.callee.noReturnBlocks, request.deferredResultReads);
    context.analyses.didChangeControlFlow();
}

namespace InlineModel {

constexpr uint32_t callOverheadBytes = 16;
constexpr uint32_t argumentOverheadBytes = 4;
constexpr uint32_t allocationCallBytes = 24;
// What goes with the call by value of an argument that is known: a direct call takes its place.
constexpr uint32_t knownCalleeArgumentBytes = 16;
// A loop of the library calls what it is handed once for each element, so the direct call in the copy pays more often than in a callee that calls its parameter in a cycle of any kind.
constexpr uint32_t libraryLoopFactor = 2;
constexpr uint32_t knownReceiverAccessBytes = 64;
// What goes with an access by name to a receiver that is known: an access by slot takes its place.
constexpr uint32_t slotInPlaceOfNameBytes = 12;
AOT_THRESHOLD(unsigned, depthWithoutBets, 3);
constexpr uint32_t constructorBytes = 256;
constexpr uint8_t percentileOfMostEnteredFunctions = 98;
constexpr uint32_t storeIntoOwnAllocationBytes = 32;
// What an entrypoint that has to convert the result pays to enter the tests: a frame, a call, a return.
constexpr uint32_t bodyCopiedIntoHotEntrypointBytes = 48;
AOT_THRESHOLD(uint32_t, bodyMovedToArmBytes, 32);

} // namespace InlineModel

// A benefit above the bytes that go with the call is a bet that the copy runs often enough to pay for its bytes.
enum class Bets : uint8_t { OnSpeed, None };

// Every block parameter that a summarized condition depends on has the identity of the one entry parameter that decides it.
static KnownValue evaluateForArgument(const Program& program, const Function& caller, ValueID argument, const Function& callee, ValueID id)
{
    const Value& value = callee.at(id);
    if (value.opcode == Opcode::Parameter)
        return knownValueOf(program, caller, argument);

    if (value.opcode == Opcode::TypeTest && callee.at(sameObjectThroughCasts(callee, value.valueOperand())).opcode == Opcode::Parameter) {
        switch (evaluateTypeTest(program, program.typeOf(caller, argument), program.at(value.testedType()))) {
        case KnownBoolean::True:
            return KnownValue::boolean(true);
        case KnownBoolean::False:
            return KnownValue::boolean(false);
        case KnownBoolean::Unknown:
            break;
        }
    }

    if (isRematerializable(value))
        return knownValueOf(program, callee, id);
    return evaluateWithKnownOperands(program, callee, value, [&](ValueID operand) {
        return evaluateForArgument(program, caller, argument, callee, operand);
    });
}

static bool namesOneFunction(const Program& program, const Type& type)
{
    return type.singleFunction() || singleBuiltinFunction(program, type);
}

enum class ArgumentTypes : uint8_t { Proven, Expected };

static bool passesEveryEntryCheck(const Program& program, const Function& caller, const Value& call, ArgumentTypes argumentTypes = ArgumentTypes::Proven)
{
    const Function& checks = program.at(call.function());
    auto& parameters = checks.at(entryBlock).parameters;
    return std::ranges::all_of(checks.blocks, [&](const Block& block) {
        return std::ranges::all_of(block.values, [&](ValueID id) {
            const Value& test = checks.at(id);
            if (test.opcode != Opcode::TypeTest)
                return true;
            size_t index = parameters.find(test.valueOperand());
            ValueID argument = index == notFound ? ValueID() : argumentAt(call, index);
            if (!argument)
                return false;
            const Type& type = argumentTypes == ArgumentTypes::Proven ? program.typeOf(caller, argument) : program.expectedOrTypeOf(caller, argument).predicted();
            return type.isSubsetOf(program.at(test.testedType()), program);
        });
    });
}

// The tests only pass their arguments on, so what the caller knows of the function behind them is lost at a call of the tests.
static bool knowsMoreThanEntryChecksPassOn(FunctionPassContext& context, ValueID callID)
{
    const Program& program = context.program;
    const Value& call = context.function.at(callID);
    if (willEnterAwaitedCalleeWithoutPromise(context, callID))
        return true;
    FunctionID body = program.at(call.function()).creator.as<CreatorRecord::EntryChecks>().body;
    return passesArgumentThat(call, context.inputs.inlining.of(program, body).calledParameters, [&](ValueID argument, uint32_t entryParameter) {
        return namesOneFunction(program, program.typeOf(context.function, argument)) && canHaveCalleeVersion(program, body, entryParameter);
    });
}

static bool isHeldByCallAlone(FunctionPassContext& context, ValueID value, ValueID call)
{
    const UseLists& useLists = context.analyses.useLists();
    while (true) {
        std::span<const Use> uses = useLists.usesOf(value);
        if (uses.size() != 1 || uses[0].kind != Use::Kind::Operand)
            return false;
        if (uses[0].user == call)
            return true;
        if (!isCastWithoutTest(context.function.at(uses[0].user).opcode))
            return false;
        value = uses[0].user;
    }
}

// `f(a, b)` for `function f(...list)`: the caller makes the list for the call.
static std::optional<int64_t> lengthOfListMadeForCall(FunctionPassContext& context, ValueID argument, ValueID call)
{
    ValueID definition = sameObjectThroughCasts(context.function, argument).valueInFrontOfFacts();
    const Value& list = context.function.at(definition);
    if (list.opcode == Opcode::EmptyArgumentList)
        return 0;
    if (list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand() || !isHeldByCallAlone(context, definition, call))
        return std::nullopt;
    return list.elementsOperands().size();
}

// The tests that the site decides go with the sides that it folds. They are counted where a loop of the caller repeats a hot call: elsewhere the estimate stays on the safe side, and no more is copied than the sides alone allow.
enum class DecidedTests : uint8_t { AreCharged, Go };

static uint32_t computeFoldedBytes(FunctionPassContext& context, ValueID callID, const InliningSummary& summary, DecidedTests decided)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    Vector<DominatedRange, 4> foldedRanges;
    Vector<BytesInRange, 4> decidedTests;
    bool everyTestPasses = true;
    for (const ParameterCondition& condition : summary.conditions) {
        ValueID argument = argumentAt(call, condition.entryParameter);
        KnownBoolean outcome = argument ? evaluateForArgument(program, caller, argument, program.at(call.function()), condition.condition).toBoolean(program) : KnownBoolean::Unknown;
        everyTestPasses &= outcome == KnownBoolean::True || !condition.isTestInFrontOfSharedBytes;
        if (outcome != KnownBoolean::Unknown)
            decidedTests.append(condition.test);
        switch (outcome) {
        case KnownBoolean::True:
            foldedRanges.append(condition.onlyIfFalse.range);
            break;
        case KnownBoolean::False:
            foldedRanges.append(condition.onlyIfTrue.range);
            break;
        case KnownBoolean::Unknown:
            break;
        }
    }
    for (const ParameterSwitch& parameterSwitch : summary.switches) {
        ValueID argument = argumentAt(call, parameterSwitch.entryParameter);
        std::optional<int64_t> constant;
        if (argument && parameterSwitch.isOnLength)
            constant = lengthOfListMadeForCall(context, argument, callID);
        else if (argument && !parameterSwitch.isOnStrings)
            constant = integerConstantThroughCasts(program, caller, argument);
        else if (StringID literal = argument ? program.typeOf(caller, argument).singleStringLiteral() : StringID())
            constant = literal.index();
        if (!constant)
            continue;
        decidedTests.append(parameterSwitch.test);
        bool isListed = false;
        for (auto& [key, code] : parameterSwitch.onlyInCase) {
            if (key == *constant)
                isListed = true;
            else
                foldedRanges.append(code.range);
        }
        if (isListed)
            foldedRanges.append(parameterSwitch.onlyOtherwise.range);
    }

    // A range holds another where it holds its beginning, so what is asked is whether a number lies in one of the ranges: they are joined where they meet.
    AOT::sort(foldedRanges, { }, &DominatedRange::begin);
    size_t numberOfJoinedRanges = 0;
    for (const DominatedRange& range : foldedRanges) {
        if (range.isEmpty())
            continue;
        if (numberOfJoinedRanges && range.begin <= foldedRanges[numberOfJoinedRanges - 1].end)
            foldedRanges[numberOfJoinedRanges - 1].end = std::max(foldedRanges[numberOfJoinedRanges - 1].end, range.end);
        else
            foldedRanges[numberOfJoinedRanges++] = range;
    }
    foldedRanges.shrink(numberOfJoinedRanges);

    uint32_t foldedBytes = 0;
    auto isFolded = [&](const BytesInRange& code) {
        auto behind = std::ranges::upper_bound(foldedRanges, code.range.begin, { }, &DominatedRange::begin);
        return behind != foldedRanges.begin() && std::prev(behind)->contains(code.range);
    };
    auto addIf = [&](bool condition, const BytesInRange& code) {
        if (condition)
            foldedBytes = saturatingSum<uint32_t>(foldedBytes, code.bytes);
    };
    if (everyTestPasses) {
        for (const BytesInRange& part : summary.partsOfBytesOnlyIfSomeTestFails)
            addIf(!isFolded(part), part);
    }
    for (const BytesInRange& test : decidedTests)
        addIf(decided == DecidedTests::Go && !isFolded(test), test);
    for (const SpreadOfParameter& spread : summary.spreadsOfParameters) {
        ValueID argument = argumentAt(call, spread.entryParameter);
        addIf(argument && !isFolded(spread.code) && lengthOfListMadeForCall(context, argument, callID), spread.code);
    }
    for (const ParameterCondition& condition : summary.conditions) {
        addIf(isFolded(condition.onlyIfTrue), condition.onlyIfTrue);
        addIf(isFolded(condition.onlyIfFalse), condition.onlyIfFalse);
    }
    for (const ParameterSwitch& parameterSwitch : summary.switches) {
        for (auto& [key, code] : parameterSwitch.onlyInCase)
            addIf(isFolded(code), code);
        addIf(isFolded(parameterSwitch.onlyOtherwise), parameterSwitch.onlyOtherwise);
    }
    // A decided test in the code behind failed tests is counted with that code and by itself.
    return std::min(foldedBytes, summary.codeBytes);
}

static uint64_t computeKnownCalleeBenefit(FunctionPassContext& context, ValueID callID, const InliningSummary& summary, Bets bets)
{
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    uint64_t cycleWeight = bets == Bets::OnSpeed ? context.limit(LimitID::InlineCycleWeight) : 1;
    if (bets == Bets::OnSpeed && context.program.at(call.function()).role == FunctionRole::Library)
        cycleWeight *= InlineModel::libraryLoopFactor;
    uint64_t bytes = 0;
    for (size_t entryParameter : summary.calledParameters) {
        ValueID argument = argumentAt(call, entryParameter);
        if (!argument || !namesOneFunction(context.program, context.program.typeOf(caller, argument)))
            continue;
        bytes += InlineModel::knownCalleeArgumentBytes * (summary.parametersCalledInCycles.get(entryParameter) ? cycleWeight : 1);
        // A closure that the caller makes for the call alone is not made where the copy calls its function directly.
        ValueID definition = sameObjectThroughCasts(caller, argument).valueInFrontOfFacts();
        if (caller.at(definition).opcode == Opcode::NewClosure && isHeldByCallAlone(context, definition, callID))
            bytes += estimatedCodeBytes(context.program, caller, caller.at(definition));
    }
    return bytes;
}

// A loop of the library calls the function that is written at the call once for each element, however seldom the call itself is reached.
static bool handsOwnClosureToLoopOfLibrary(FunctionPassContext& context, ValueID callID, const InliningSummary& summary)
{
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    if (context.inputs.heat.isCompact(context.program, context.id).predicted())
        return false;
    if (context.program.at(call.function()).role != FunctionRole::Library)
        return false;
    // Behind the tests of the receiver each arm hands on the one closure.
    return passesArgumentThat(call, summary.parametersCalledInCycles, [&](ValueID argument, uint32_t) {
        return context.program.typeOf(caller, argument).singleFunction() && caller.at(sameObjectThroughCasts(caller, argument)).opcode == Opcode::NewClosure;
    });
}

// Beside the allocation a store that creates a field becomes an entry of the allocation's list, and the allocation proves what the store tests.
static uint64_t computeOwnAllocationBenefit(const Program& program, const Function& callee, const ReceiverParameter& receiver)
{
    uint64_t bytes = uint64_t { InlineModel::storeIntoOwnAllocationBytes } * receiver.stores;
    ValueID object = callee.at(entryBlock).parameters[receiver.entryParameter];
    for (const Block& block : callee.blocks) {
        if (isSlowPath(callee, block).predicted())
            continue;
        for (ValueID id : block.values) {
            const Value& store = callee.at(id);
            if (!isStoreByKey(store.opcode) || sameObjectThroughCasts(callee, store.operand(*receiverOf(store.opcode))) != object)
                continue;
            const Value& stored = callee.at(definitionThroughUpcasts(callee, store.operand(*storedOf(store.opcode))));
            // The field of an object can make such a closure when it is first read.
            if (stored.opcode != Opcode::NewClosure || program.at(stored.function()).constructorForm() != ConstructorForm::NotConstructor)
                continue;
            std::span<const ValueID> captures = stored.capturesOperands();
            if (captures.empty() || (captures.size() == 1 && sameObjectThroughCasts(callee, captures[0]) == object))
                bytes += estimatedCodeBytes(program, callee, stored);
        }
    }
    return bytes;
}

static uint64_t computeKnownReceiverBenefit(const FunctionPassContext& context, const Value& call, const InliningSummary& summary, Bets bets)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    uint64_t bytesOfAccess = bets == Bets::OnSpeed ? InlineModel::knownReceiverAccessBytes : InlineModel::slotInPlaceOfNameBytes;
    uint64_t bytes = 0;
    for (const ReceiverParameter& receiver : summary.receiverParameters) {
        ValueID argument = argumentAt(call, receiver.entryParameter);
        if (!argument)
            continue;
        const Value& definition = caller.at(sameObjectThroughCasts(caller, argument));
        if (program.typeOf(caller, argument).layouts || proofsOfAllocation(program, caller, definition).contains(Proof::Shape))
            bytes += bytesOfAccess * receiver.accessesByName;
        if (receiver.stores && definition.opcode == Opcode::NewObject && allocatesOwnLayout(program, definition))
            bytes += computeOwnAllocationBenefit(program, program.at(call.function()), receiver);
    }
    return bytes;
}

// A read under a name that is written at the call remembers where it found the name. A read under a key that arrives looks it up each time.
static uint64_t computeKnownKeyBenefit(const FunctionPassContext& context, const Value& call, const InliningSummary& summary, Bets bets)
{
    uint64_t bytesOfAccess = bets == Bets::OnSpeed ? InlineModel::knownReceiverAccessBytes : InlineModel::slotInPlaceOfNameBytes;
    uint64_t bytes = 0;
    for (const KeyParameter& key : summary.keyParameters) {
        ValueID argument = argumentAt(call, key.entryParameter);
        if (argument && context.program.typeOf(context.function, argument).singleStringLiteral())
            bytes += bytesOfAccess * key.accesses;
    }
    return bytes;
}

// The frame holds the call that leads to the function here.
static unsigned numberOfCallSitesCountedHere(const FunctionPassContext& context, uint32_t frame)
{
    FunctionID holder = frame ? context.function.inlineFrames[frame].callee : context.id;
    return holder.index() < context.inputs.callGraph.numberOfFunctions() ? 1 : 0;
}

// The call graph is built before calls are resolved: a call by value that lists the function may be a direct call by the time its caller is optimized.
static bool mayBeEnteredElsewhere(const FunctionPassContext& context, FunctionID function, uint32_t frame)
{
    const CallGraph& callGraph = context.inputs.callGraph;
    return callGraph.numberOfCallSitesInCode(function) > numberOfCallSitesCountedHere(context, frame) || callGraph.isExpectedByCode(function);
}

// Closures, class records, bound functions and the host name the entrypoint of a function, so what finds the function there enters the body through the call in the entrypoint.
static bool isEnteredThroughCallerAlone(const FunctionPassContext& context, FunctionID function, Expected<HeatClass> heat)
{
    const Function& caller = context.function;
    const auto* entrypoint = caller.creator.tryAs<CreatorRecord::Entrypoint>();
    if (caller.role != FunctionRole::GenericEntrypoint || !entrypoint || entrypoint->target != function)
        return false;
    if (entrypointVariantsOf(context.program, context.program.at(function)) == EntrypointVariants::Both)
        return false;
    // Flow lists a function at every call that it has followed a closure to, and types name it where a built-in or an access may enter it.
    const CallGraph& callGraph = context.inputs.callGraph;
    if (!context.inputs.flow.hasResults() || function.index() >= callGraph.numberOfFunctions())
        return false;
    // Hot code is worth the copy beside the body that such a call keeps.
    if (callGraph.isExpectedByCode(function) && !isHot(heat).predicted())
        return false;
    return std::ranges::none_of(callGraph.callersOf(function), [&](FunctionID other) {
        return other != context.id;
    });
}

// The generic version of a caller repeats the call and never takes the body in. The program has written one call.
static bool isCalledAgainByGenericVersionAlone(const FunctionPassContext& context, FunctionID function)
{
    if (context.inputs.callGraph.numberOfCallSites(function) != 2)
        return false;
    return std::ranges::count_if(context.function.values, [&](const Value& value) {
        return value.opcode == Opcode::CallDirect && value.function() == function && value.isNeverInlined();
    }) == 1;
}

// The body goes only where the call in the frame is the one call that the graph counts.
static bool hasSingleCallSite(const FunctionPassContext& context, FunctionID function, uint32_t frame, Expected<HeatClass> heat)
{
    const CallGraph& callGraph = context.inputs.callGraph;
    bool isTheCallThatIsCounted = numberOfCallSitesCountedHere(context, frame) || isHot(heat).predicted();
    if (isTheCallThatIsCounted && callGraph.callSiteSet(function) == CallSiteSet::Complete && (callGraph.numberOfCallSites(function) == 1 || isCalledAgainByGenericVersionAlone(context, function)))
        return true;
    return isEnteredThroughCallerAlone(context, function, heat);
}

// The frame of the built-in that a traversal stands for holds the step of the traversal and nothing else: the two are one level. So are the site of a chain and its step.
static bool isLevelOfCopies(const Program& program, const Function& caller, uint32_t frame)
{
    const InlineFrame& inlineFrame = caller.inlineFrames[frame];
    const Function& callee = program.at(inlineFrame.callee);
    if (inlineFrame.calledBuiltin || isWrapperCreatedByEngine(callee))
        return false;
    if (callee.role != FunctionRole::ChainStep || !inlineFrame.parent)
        return true;
    FunctionID holder = caller.inlineFrames[inlineFrame.parent].callee;
    return holder != inlineFrame.callee && !program.at(holder).enclosingRuntimeFunction;
}

unsigned inlineDepthOf(const Program& program, const Function& caller, uint32_t inlineFrame)
{
    unsigned depth = 0;
    for (uint32_t frame = inlineFrame; frame; frame = caller.inlineFrames[frame].parent)
        depth += isLevelOfCopies(program, caller, frame);
    return depth;
}

// A body with one call site moves into its caller and is no copy. So what is called from it has the depth that it had there.
static unsigned depthOfCopies(const FunctionPassContext& context, uint32_t inlineFrame, Expected<HeatClass> heat)
{
    const Function& caller = context.function;
    unsigned depth = inlineDepthOf(context.program, caller, inlineFrame);
    Vector<uint32_t, 8> framesFromInnermost;
    for (uint32_t frame = inlineFrame; frame; frame = caller.inlineFrames[frame].parent)
        framesFromInnermost.append(frame);
    for (uint32_t frame : framesFromInnermost | std::views::reverse) {
        if (caller.inlineFrames[frame].calledBuiltin)
            continue;
        if (!hasSingleCallSite(context, caller.inlineFrames[frame].callee, caller.inlineFrames[frame].parent, heat))
            break;
        depth -= isLevelOfCopies(context.program, caller, frame);
    }
    return depth;
}

// An entrypoint tests what it was given and enters the body.
static bool wouldCopySharedBodyIntoEntrypoint(const FunctionPassContext& context, BlockID block, ValueID id, Expected<HeatClass> heat)
{
    const Function& caller = context.function;
    const Value& call = caller.at(id);
    if (caller.role != FunctionRole::GenericEntrypoint)
        return false;
    // In hot code the copy knows what the entrypoint has tested and needs no way back to it. An entrypoint that is nothing but a tail call has tested nothing, and jumps.
    const Terminator& terminator = caller.at(block).terminator;
    bool onlyEntersBody = caller.blocks.size() == 1 && caller.at(block).values.size() == 1 && terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
    if (isHot(heat).predicted() && !onlyEntersBody)
        return false;
    // Wrappers lead to the body. What the body calls has the budget that it has there, unless the body is itself a copy.
    bool entersBody = !inlineDepthOf(context.program, caller, call.inlineFrame);
    if (entersBody && mayBeEnteredElsewhere(context, call.function(), call.inlineFrame))
        return true;
    for (uint32_t frame = call.inlineFrame; frame; frame = caller.inlineFrames[frame].parent) {
        if (!caller.inlineFrames[frame].calledBuiltin && mayBeEnteredElsewhere(context, caller.inlineFrames[frame].callee, caller.inlineFrames[frame].parent))
            return true;
    }
    return false;
}

// The tests that were split off the head of a function go where the entrypoint that takes them in is all that enters them.
static bool wouldCopySharedEntryChecksIntoEntrypoint(const FunctionPassContext& context, const Value& call, Expected<HeatClass> heat)
{
    const Program& program = context.program;
    const Function& callee = program.at(call.function());
    if (context.function.role != FunctionRole::GenericEntrypoint || call.isAlwaysInlined() || callee.role != FunctionRole::EntryChecks)
        return false;
    if (isHot(heat).predicted()) {
        Representation result = defaultRepresentationOf(program.at(call.type));
        bool convertsResult = result != Representation::JSValue && result != Representation::Cell;
        if (convertsResult && context.inputs.inlining.of(program, callee.creator.as<CreatorRecord::EntryChecks>().body).codeBytes <= InlineModel::bodyCopiedIntoHotEntrypointBytes)
            return false;
    }
    return mayBeEnteredElsewhere(context, call.function(), call.inlineFrame);
}

static uint32_t inlineBudgetBytes(const FunctionPassContext& context, Expected<HeatClass> heat, unsigned depth)
{
    uint64_t budget = 0;
    switch (heat.predicted()) {
    case HeatClass::RunsOnce:
    case HeatClass::Cold:
        budget = context.limit(LimitID::InlineBudgetColdBytes);
        break;
    case HeatClass::Unknown:
    case HeatClass::Warm:
        budget = context.limit(LimitID::InlineBudgetWarmBytes);
        break;
    case HeatClass::Hot:
        budget = context.limit(LimitID::InlineBudgetHotBytes);
        break;
    }
    return clampTo<uint32_t>(budget >> std::min(depth, 31u));
}

static uint32_t budgetAt(const FunctionPassContext& context, const InliningSummary& summary, BlockID block, const Value& call, Expected<HeatClass> heat, unsigned depth, bool isRepeatedByCaller)
{
    uint32_t budget = inlineBudgetBytes(context, heat, depth);
    // What a call costs, a loop around it pays at every turn, and what the loop knows of the arguments stops at the call. Of a function with one call site there is one copy at most.
    // A callee with a loop of its own spends its time there, and the call costs it little.
    if (isRepeatedByCaller && !summary.hasCycle && hasSingleCallSite(context, call.function(), call.inlineFrame, heat))
        return std::max(budget, clampTo<uint32_t>(context.limit(LimitID::InlineBudgetOnlyCallBytes) >> std::min(depth, 31u)));
    // How seldom one arm of a function that runs often is taken is the weakest estimate there is. A small function that only such an arm calls moves there: nothing is copied, and the call goes.
    bool isArmOfBusyFunction = heat.predicted() == HeatClass::Cold && context.function.at(block).frequency.predicted() == BlockFrequency::Normal && !isCold(context.inputs.heat.of(context.program, context.id)).predicted();
    if (isArmOfBusyFunction && !summary.hasCycle && context.function.role != FunctionRole::GenericEntrypoint && hasSingleCallSite(context, call.function(), call.inlineFrame, heat))
        return std::max(budget, InlineModel::bodyMovedToArmBytes);
    // A function that is entered from many places runs often, though none of the places may.
    if (isHot(heat).predicted() || isCold(heat).predicted() || context.inputs.heat.percentileOf(context.program, call.function()).predicted() < InlineModel::percentileOfMostEnteredFunctions)
        return budget;
    return std::max(budget, inlineBudgetBytes(context, HeatClass::Hot, depth) / 4 * 3);
}

// The inliner makes a summary of its own for a callee that was derived in this loop, against the effect summaries of the loop.
static bool calleeReturnsFreshAggregate(FunctionPassContext& context, const InliningSummary& summary, FunctionID callee)
{
    const InliningSummaries& inlining = context.inputs.inlining;
    if (inlining.contains(callee))
        return inlining.returnsFreshAggregate(context.program, callee);
    return returnsFreshAggregateAskedOnce(summary, AnalysisContext { context.build, context.program, &context.inputs.flow, &context.inputs.effects, callee });
}

// A field that nothing reads goes in the end, and an allocation that is stored nowhere else goes with it. A call would stay.
static bool isStoredOnlyInFieldsThatNothingReads(FunctionPassContext& context, ValueID value)
{
    return std::ranges::all_of(context.analyses.useLists().usesOf(value), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& holder = context.function.at(use.user);
        if (isCast(holder.opcode))
            return isStoredOnlyInFieldsThatNothingReads(context, use.user);
        if (holder.opcode != Opcode::NewObject)
            return false;
        std::optional<uint32_t> position = holder.variadicOperandIndex(use.operandIndex);
        if (!position || *position >= holder.fields().size())
            return false;
        const LocationFlow& location = fieldAt(context.program, { holder.layout(), holder.fields()[*position] }).flow.location;
        return location.writers == WriterState::AllKnown && location.storeNaming == StoreNaming::EveryStoreNamesLocation && location.reads == ReadState::NeverRead;
    });
}

// Code that runs once has no time to win, so a copy there is worth the bytes that it takes off. The wrapper makes the object whose parts its worker returns, and that takes more code than the call.
// It goes to such a place where the caller does without the object: it takes the object apart, or keeps it where nothing reads. And it goes where the calls are so few that they and the one body
// which they need take more than a copy at each place. That body is the wrapper with its worker, which other places may enter by itself.
static bool wouldOnlyMoveAllocationIntoCodeThatRunsOnce(FunctionPassContext& context, const InliningSummary& summary, BlockID block, ValueID call, Expected<HeatClass> heat)
{
    FunctionID callee = context.function.at(call).function();
    if (heat.predicted() != HeatClass::RunsOnce || context.program.at(callee).creator.kind() != CreatorRecord::Kind::ResultWrapper)
        return false;
    const InliningSummaries& inlining = context.inputs.inlining;
    FunctionID worker = context.program.at(callee).creator.as<CreatorRecord::ResultWrapper>().worker;
    if (!inlining.contains(worker))
        return false;
    uint64_t bytesThatCallsSave = uint64_t { context.inputs.callGraph.numberOfCallSites(callee) } * (InlineModel::allocationCallBytes - CodeSize::directCall);
    if (bytesThatCallsSave <= uint64_t { summary.codeBytes } + inlining.of(context.program, worker).codeBytes)
        return false;
    if (isStoredOnlyInFieldsThatNothingReads(context, call))
        return false;
    return !sinkableAllocationsAfterInlining(context, block, call, [&] {
        return calleeReturnsFreshAggregate(context, summary, callee);
    }).predicted();
}

static FunctionID functionAsWritten(const Program& program, FunctionID id)
{
    while (FunctionID source = program.at(id).derivation.source)
        id = source;
    return id;
}

// The call stands behind the test that no function object was renamed, beside the call by value that is made once one was. The forms of the callee itself enter one another as those of any function do.
// The caller's own function object is called without the test.
static bool standsBesideCallByValue(const FunctionPassContext& context, const Value& call)
{
    return !call.callsOwnFunctionObject() && context.program.at(call.function()).capabilities.containsAll({ FunctionCapabilities::RecordsCallee, FunctionCapabilities::RecordsCalleeOnceRenamed }) && functionAsWritten(context.program, context.id) != functionAsWritten(context.program, call.function());
}

InlineVerdict computeInlineVerdict(FunctionPassContext& context, const InliningSummary& summary, BlockID block, ValueID call)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    const Value& value = caller.at(call);
    RELEASE_ASSERT(value.opcode == Opcode::CallDirect);
    const Function& callee = program.at(value.function());

    if (callee.role == FunctionRole::CoroutineRamp && willEnterAwaitedCalleeWithoutPromise(context, call))
        return { .decision = InlineDecision::Barrier };

    bool canInlineWholeBody = summary.canInlineWholeBody();
    if (!canInlineWholeBody && !summary.canInlineHead())
        return { .decision = InlineDecision::Barrier };

    Expected<HeatClass> heat = heatClassOf(context, block);
    // However often the caller runs, this part of it is not expected to.
    if (caller.at(block).frequency.predicted() != BlockFrequency::Normal && !isCold(heat).predicted())
        heat = HeatClass::Cold;
    else if (heat.predicted() == HeatClass::Cold && caller.at(block).frequency.predicted() == BlockFrequency::Normal && handsOwnClosureToLoopOfLibrary(context, call, summary))
        heat = HeatClass::Warm;
    // In a loop of the caller the call runs more often than the caller. A caller that runs once repeats it often only in a loop within a loop.
    double leastFrequencyOfHotCall = context.inputs.heat.of(program, context.id).predicted() == HeatClass::RunsOnce ? frequencyConstants.iterationsPerLoop : HeatModel::minWeightOfCallInLoop;
    if (heat.predicted() == HeatClass::Warm && context.inputs.heat.callsInLoopsAreHot().predicted() && context.analyses.blockFrequencies().frequencyOf(block).predicted() >= leastFrequencyOfHotCall)
        heat = HeatClass::Hot;

    // How often the caller is entered is a product along paths of calls. A loop around the call is in the text.
    bool isRepeatedByCaller = isHot(heat).predicted() && context.analyses.isInCycle(block);
    bool runsInLoopAroundCaller = heat.predicted() == HeatClass::Warm && context.inputs.heat.isCalledInLoop(program, context.id).predicted() && context.analyses.blockFrequencies().frequencyOf(block).predicted() >= HeatModel::minWeightOfNearlyCertainCall;
    uint32_t foldedBytes = computeFoldedBytes(context, call, summary, isRepeatedByCaller ? DecidedTests::Go : DecidedTests::AreCharged);
    uint32_t callBytes = estimatedCodeBytes(program, caller, value);
    uint32_t removedBytes = saturatingSum<uint32_t>(foldedBytes, callBytes);
    uint32_t cost = summary.codeBytes > removedBytes ? summary.codeBytes - removedBytes : 0;
    // In a loop of the caller, what counts is the code that the loop runs. What folds may be code that is not expected to run, so it is not taken off a second time.
    // A loop of the callee takes its heat from the function that holds it, and a caller that runs once has none to give.
    bool countsLikelyCode = isRepeatedByCaller && (!summary.hasCycle || context.inputs.heat.of(program, context.id).predicted() != HeatClass::RunsOnce);
    uint32_t likelyCost = !countsLikelyCode ? std::numeric_limits<uint32_t>::max() : summary.likelyCodeBytes > callBytes ? summary.likelyCodeBytes - callBytes : 0;

    unsigned depth = depthOfCopies(context, value.inlineFrame, heat);
    // The budget halves with each level and is small where code runs seldom. A bet that did neither let a body in at every level, and what a caller knew of one object cost it many times its size.
    Bets bets = isCold(heat).predicted() || depth >= InlineModel::depthWithoutBets ? Bets::None : Bets::OnSpeed;
    uint32_t budget = wouldCopySharedBodyIntoEntrypoint(context, block, call, heat) ? 0 : budgetAt(context, summary, block, value, heat, depth, isRepeatedByCaller);
    InlineVerdict verdict { .costBytes = cost, .budgetBytes = budget };

    auto addBenefit = [&](InlineBenefit benefit, uint64_t bytes) {
        verdict.benefits.add(benefit);
        verdict.benefitBytes = clampTo<uint32_t>(verdict.benefitBytes + bytes);
    };
    // The bets are sized for whole bodies: beside the likely code they count for no more than the budget.
    auto isWithinBudget = [&] {
        uint64_t allowance = uint64_t { verdict.budgetBytes } + verdict.benefitBytes;
        return cost <= allowance || (cost <= 2 * allowance && likelyCost <= uint64_t { verdict.budgetBytes } + std::min(verdict.benefitBytes, verdict.budgetBytes));
    };

    bool isEntryChecks = callee.role == FunctionRole::EntryChecks;
    bool callerIsEntrypoint = caller.role == FunctionRole::GenericEntrypoint || caller.role == FunctionRole::TableEntrypoint;
    bool leavesEntryChecksOutside = wouldCopySharedEntryChecksIntoEntrypoint(context, value, heat);
    // In a loop of the caller the tests run at every turn wherever they stand. Beside the call they are the same tests, and the typed body behind them can be copied in.
    bool isExpectedToPassInLoop = isEntryChecks && !isCold(heat).predicted() && context.analyses.isInCycle(block) && passesEveryEntryCheck(program, caller, value, ArgumentTypes::Expected);
    bool opensTypedBody = isEntryChecks && (callerIsEntrypoint || passesEveryEntryCheck(program, caller, value) || isExpectedToPassInLoop || (!isCold(heat).predicted() && knowsMoreThanEntryChecksPassOn(context, call)));
    bool isAlwaysWorthInlining = summary.isWrapper && !leavesEntryChecksOutside && (!isEntryChecks || opensTypedBody) && !wouldOnlyMoveAllocationIntoCodeThatRunsOnce(context, summary, block, call, heat);
    if (canInlineWholeBody && (value.isAlwaysInlined() || isAlwaysWorthInlining)) {
        addBenefit(value.isAlwaysInlined() ? InlineBenefit::CallOverhead : InlineBenefit::Wrapper, cost);
        verdict.decision = InlineDecision::Inline;
        return verdict;
    }
    if (caller.role == FunctionRole::EntryChecks || leavesEntryChecksOutside) {
        verdict.decision = InlineDecision::CostExceedsBenefit;
        return verdict;
    }
    if (isEntryChecks) {
        const auto& record = callee.creator.as<CreatorRecord::EntryChecks>();
        const InliningSummaries& inlining = context.inputs.inlining;
        cost = saturatingSum<uint32_t>(cost, inlining.of(program, record.body).codeBytes, inlining.of(program, record.fallback).codeBytes);
        likelyCost = std::numeric_limits<uint32_t>::max();
        verdict.costBytes = cost;
    }

    if (foldedBytes)
        verdict.benefits.add(InlineBenefit::FoldedCode);
    uint64_t callOverheadBytes = InlineModel::callOverheadBytes + uint64_t { InlineModel::argumentOverheadBytes } * value.numberOfOperands();
    addBenefit(InlineBenefit::CallOverhead, callOverheadBytes);
    // The place holds two ways to the callee already. A third is taken where it is no larger than the call in whose place it stands.
    if (standsBesideCallByValue(context, value)) {
        verdict.budgetBytes = 0;
        verdict.decision = canInlineWholeBody && cost <= callOverheadBytes ? InlineDecision::Inline : InlineDecision::CostExceedsBenefit;
        return verdict;
    }
    if (uint64_t bytes = computeKnownCalleeBenefit(context, call, summary, bets))
        addBenefit(InlineBenefit::KnownCalleeArgument, bytes);
    if (uint64_t bytes = computeKnownReceiverBenefit(context, value, summary, bets))
        addBenefit(InlineBenefit::KnownReceiver, bytes);
    if (uint64_t bytes = computeKnownKeyBenefit(context, value, summary, bets))
        addBenefit(InlineBenefit::KnownKey, bytes);

    bool isTooDeep = false;
    if (canInlineWholeBody) {
        // Only where the creation of a generator and the loop over it stand in one function is the loop merged with the body of the generator, or takes at least its first step without a suspension.
        bool iteratesGeneratorOfCaller = passesArgumentThat(value, summary.iteratedParameters, [&](ValueID argument, uint32_t) {
            return caller.at(sameObjectThroughCasts(caller, argument)).opcode == Opcode::NewGenerator;
        });
        if (iteratesGeneratorOfCaller && !context.exceeds(LimitID::MaxFusedGeneratorBytes, cost, call))
            addBenefit(InlineBenefit::SunkAllocation, cost);
        // A factory is mostly called by value, so heat says little of the `new` inside it. A small constructor goes where its object is wanted, whatever the heat.
        bool makesOrFillsNewObject = callee.role == FunctionRole::ConstructBody || callee.role == FunctionRole::AllocatedThisVersion || callee.has(FunctionFlags::AllocatesOwnThis);
        if (cost <= InlineModel::constructorBytes && makesOrFillsNewObject)
            addBenefit(InlineBenefit::SunkAllocation, cost);
        // The limit bounds what each level adds to the caller. A body that costs no more than its call adds nothing to the code, but each level is one more look at all of the caller.
        isTooDeep = (cost > callOverheadBytes && context.exceeds(LimitID::MaxInlineDepth, depth + 1, call)) || context.exceeds(LimitID::MaxInlineNesting, inlineDepthOf(program, caller, value.inlineFrame) + 1, call);
        // Where code runs seldom, an allocation that goes is worth its own bytes and no more.
        if (!isTooDeep && !isWithinBudget() && (!isCold(heat).predicted() || callerIsEntrypoint)) {
            uint64_t bytesOfAllocation = isCold(heat).predicted() ? InlineModel::allocationCallBytes : context.limit(LimitID::InlineSunkAllocationBytes);
            unsigned allocations = sinkableAllocationsAfterInlining(context, block, call, [&] {
                return calleeReturnsFreshAggregate(context, summary, value.function());
            }).predicted();
            if (allocations)
                addBenefit(InlineBenefit::SunkAllocation, allocations * bytesOfAllocation);
        }
    }

    if (canInlineWholeBody && !isTooDeep && isWithinBudget())
        verdict.decision = InlineDecision::Inline;
    else if ((isHot(heat).predicted() || runsInLoopAroundCaller) && summary.canInlineHead()) {
        verdict.decision = InlineDecision::InlineHead;
        verdict.costBytes = summary.headBytes;
    } else if (!canInlineWholeBody)
        verdict.decision = InlineDecision::Barrier;
    else
        verdict.decision = isTooDeep ? InlineDecision::DepthLimit : InlineDecision::CostExceedsBenefit;
    return verdict;
}

enum class FollowUp : uint8_t { None, MarkNeverInlined, ReportDecline, Rewrite };

struct DecisionHandling {
    FollowUp followUp;
    LimitID limit;
    StatisticID statistic;
};

struct InlineSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID call;
    const InliningSummary* summary { nullptr };
    double heat { 0 };
    InlineVerdict verdict;
};

static constexpr DecisionHandling handlingOf(InlineDecision decision)
{
    switch (decision) {
    case InlineDecision::Unknown:
        return { FollowUp::None, LimitID::None, StatisticID::InlineDecisionUnknown };
    case InlineDecision::Inline:
        return { FollowUp::Rewrite, LimitID::None, StatisticID::InlineDecisionInline };
    case InlineDecision::InlineHead:
        return { FollowUp::Rewrite, LimitID::None, StatisticID::InlineDecisionInlineHead };
    case InlineDecision::Barrier:
        return { FollowUp::ReportDecline, LimitID::None, StatisticID::InlineDecisionBarrier };
    case InlineDecision::SignatureMismatch:
        return { FollowUp::None, LimitID::None, StatisticID::InlineDecisionSignatureMismatch };
    case InlineDecision::ClosesCycle:
        return { FollowUp::MarkNeverInlined, LimitID::None, StatisticID::InlineDecisionClosesCycle };
    case InlineDecision::DepthLimit:
        return { FollowUp::ReportDecline, LimitID::MaxInlineDepth, StatisticID::InlineDecisionDepthLimit };
    case InlineDecision::CallerSizeLimit:
        return { FollowUp::ReportDecline, LimitID::MaxInlinedCallerBytes, StatisticID::InlineDecisionCallerSizeLimit };
    case InlineDecision::CostExceedsBenefit:
        return { FollowUp::ReportDecline, LimitID::None, StatisticID::InlineDecisionCostExceedsBenefit };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static StatisticID statisticFor(InlineBenefit benefit)
{
    switch (benefit) {
    case InlineBenefit::CallOverhead:
        return StatisticID::InlineBenefitCallOverhead;
    case InlineBenefit::FoldedCode:
        return StatisticID::InlineBenefitFoldedCode;
    case InlineBenefit::KnownCalleeArgument:
        return StatisticID::InlineBenefitKnownCalleeArgument;
    case InlineBenefit::KnownReceiver:
        return StatisticID::InlineBenefitKnownReceiver;
    case InlineBenefit::KnownKey:
        return StatisticID::InlineBenefitKnownKey;
    case InlineBenefit::SunkAllocation:
        return StatisticID::InlineBenefitSunkAllocation;
    case InlineBenefit::Wrapper:
        return StatisticID::InlineBenefitWrapper;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAccepted(const InlineSite& site)
{
    return handlingOf(site.verdict.decision).followUp == FollowUp::Rewrite;
}

static InlinedPart partToCopy(const InlineSite& site)
{
    RELEASE_ASSERT(isAccepted(site));
    return site.verdict.decision == InlineDecision::InlineHead ? InlinedPart::Head : InlinedPart::WholeBody;
}

// Nothing is gained in rare code by itself. But an object that goes on into code that runs often meets the one which that code has taken apart, and that one is put together for as long as this one is a cell:
// which it is where a call makes it, fills it or takes it.
static bool handsObjectToFrequentCode(FunctionPassContext& context, ValueID call)
{
    const Function& caller = context.function;
    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, 4> worklist;
    BitVector isSeen(caller.values.size());
    auto add = [&](ValueID value) {
        if (value && !isSeen.quickSet(value.index()) && context.program.typeOf(caller, value).hasObjects())
            worklist.append(value);
    };
    if (context.program.at(caller.at(call).function()).role == FunctionRole::ResultWrapper)
        add(call);
    caller.at(call).forEachOperand([&](ValueID operand, uint32_t) {
        add(sameObjectThroughCasts(caller, operand).valueInFrontOfFacts());
    });
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            if (use.kind == Use::Kind::EdgeArgument) {
                const Block& target = caller.at(targetOf(caller, use.edge()).block);
                if (target.frequency.predicted() == BlockFrequency::Normal)
                    return true;
                if (use.operandIndex < target.parameters.size())
                    add(target.parameters[use.operandIndex]);
            }
            if (use.kind == Use::Kind::Operand && interpretedOf(caller.at(use.user).opcode) == Interpretation::Cast)
                add(use.user);
        }
    }
    return false;
}

static Vector<InlineSite> findInlineSites(FunctionPassContext& context, uint32_t firstFrame)
{
    const Function& caller = context.function;
    const CFG& cfg = context.analyses.cfg();
    Vector<InlineSite> sites;
    for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        if (!cfg.isReachable(blockID))
            continue;
        bool isRare = isRecordedAsRare(caller, blockID).predicted();
        const Block& block = caller.at(blockID);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            const Value& value = caller.at(id);
            bool isCandidate = value.opcode == Opcode::CallDirect && !value.isNeverInlined() && !value.isAfterInlinedHead();
            if (value.inlineFrame >= firstFrame && isCandidate && (!isRare || handsObjectToFrequentCode(context, id)))
                sites.append(InlineSite { .block = blockID, .index = index, .call = id });
        }
    }
    // FuseArrayChains comes in front of this pass and has not seen the calls of a body that this run has copied. What it would fuse waits for the next round.
    auto leaveArrayChains = [&] {
        bool hasArrayMethod = sites.containsIf([&](const InlineSite& site) {
            return !!arrayMethodCallOf(context.program, caller, site.call);
        });
        if (!hasArrayMethod || caller.role == FunctionRole::ChainStep || caller.enclosingRuntimeFunction || !findChainTraversal(context.program))
            return;

        BitVector members;
        for (const ArrayChain& chain : findArrayChains(context)) {
            if (checkInterleaving(context, chain) != ArrayChainDeclineReason::None)
                continue;
            members.set(chain.lastCall.index());
            if (chain.rangeCall)
                members.set(chain.rangeCall.index());
            for (const ChainCallback& callback : chain.callbacks)
                members.set(callback.call.index());
        }
        sites.removeAllMatching([&](const InlineSite& site) {
            return members.get(site.call.index());
        });
    };
    if (firstFrame)
        leaveArrayChains();
    if (sites.isEmpty())
        return sites;

    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    sites.removeAllMatching([&](const InlineSite& site) {
        return frequencies.classOf(site.block).predicted() == BlockFrequency::Rare && !handsObjectToFrequentCode(context, site.call);
    });
    return sites;
}

static PassOutcome markNoReturnCallBlocks(FunctionPassContext& context)
{
    Function& caller = context.function;
    PassOutcome outcome = PassOutcome::Unchanged;
    for (BlockID blockID : context.analyses.cfg().reversePostOrder()) {
        const Block& block = caller.at(blockID);
        if (block.frequency.predicted() == BlockFrequency::Rare)
            continue;
        bool callsNoReturnFunction = block.values.containsIf([&](ValueID id) {
            const Value& value = caller.at(id);
            return value.opcode == Opcode::CallDirect && context.inputs.inlining.neverReturns(context.program, value.function());
        });
        if (!callsNoReturnFunction)
            continue;
        caller.edit(blockID, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        outcome = PassOutcome::Changed;
    }
    if (outcome == PassOutcome::Changed)
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
    return outcome;
}

static const InliningSummary& inliningSummaryOf(FunctionPassContext& context, FunctionID callee)
{
    const Program& program = context.program;
    const InliningSummaries& inlining = context.inputs.inlining;
    if (inlining.contains(callee))
        return inlining.of(program, callee);
    FunctionID source = callee;
    while (ProvisionalFunctions::contains(source))
        source = program.at(source).derivation.source;
    if (!context.build.isEnabled(PassID::SummarizeForInlining, findFunctionAsRead(&program, source)))
        return inlining.of(program, callee);

    // Nothing of the caller goes into it.
    return context.inputs.derivedFunctions->inliningSummaryOf(callee, [&] {
        AnalysisContext calleeContext { context.build, program, &context.inputs.flow, &context.inputs.effects, callee };
        return summarizeFunctionForInlining(calleeContext, program.at(callee), HeatClass::Warm);
    });
}

static void spendCallerBudget(FunctionPassContext& context, Vector<InlineSite>& sites)
{
    Vector<uint32_t, 8> order;
    for (uint32_t index = 0; index < sites.size(); ++index) {
        if (isAccepted(sites[index]))
            order.append(index);
    }
    // Heat only says who comes first.
    if (order.size() > 1) {
        for (uint32_t index : order) {
            double heat = context.heatOf(sites[index].block).predicted();
            sites[index].heat = heat > 0 ? heat : 0;
        }
    }
    AOT::sort(order, [&](uint32_t a, uint32_t b) {
        const InlineSite& first = sites[a];
        const InlineSite& second = sites[b];
        if (first.heat != second.heat)
            return first.heat > second.heat;
        uint64_t scaledBenefitOfFirst = uint64_t { first.verdict.benefitBytes } * (uint64_t { second.verdict.costBytes } + 1);
        uint64_t scaledBenefitOfSecond = uint64_t { second.verdict.benefitBytes } * (uint64_t { first.verdict.costBytes } + 1);
        if (scaledBenefitOfFirst != scaledBenefitOfSecond)
            return scaledBenefitOfFirst > scaledBenefitOfSecond;
        return a < b;
    });

    uint64_t frameCount = inlineFrameCountOf(context.function);
    // The limit is on what inlining adds. A caller that is large by itself takes in as much as a small one.
    auto computeInlinedBytes = [&] {
        const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
        uint64_t bytes = 0;
        for (BlockID block : context.analyses.cfg().reversePostOrder()) {
            if (frequencies.classOf(block).predicted() == BlockFrequency::Rare)
                continue;
            for (ValueID id : context.function.at(block).values) {
                const Value& value = context.function.at(id);
                if (value.inlineFrame)
                    bytes += estimatedCodeBytes(context.program, context.function, value);
            }
        }
        return bytes;
    };
    std::optional<uint64_t> inlinedBytes;
    auto fitsInCaller = [&](const InlineSite& site) {
        const Value& call = context.function.at(site.call);
        uint64_t frameCountWithCopy = frameCount + inlineFrameCountOf(context.program.at(call.function()));
        if (context.exceeds(LimitID::InlineFramesPerFunction, frameCountWithCopy, site.call))
            return false;

        // The traversal of a chain of array methods folds to a fraction of its size, and called it would read at run time what it is to do.
        uint32_t costBytes = site.verdict.costBytes;
        if (costBytes && !site.verdict.benefits.contains(InlineBenefit::Wrapper) && !call.isAlwaysInlined()) {
            if (!inlinedBytes)
                inlinedBytes = computeInlinedBytes();
            if (context.exceeds(LimitID::MaxInlinedCallerBytes, *inlinedBytes + costBytes, site.call))
                return false;
            *inlinedBytes += costBytes;
        }
        frameCount = frameCountWithCopy;
        return true;
    };
    for (uint32_t index : order) {
        if (!fitsInCaller(sites[index]))
            sites[index].verdict.decision = InlineDecision::CallerSizeLimit;
    }
}

static PassOutcome callCalleeVersions(FunctionPassContext& context, std::span<const InlineSite> sites)
{
    const Program& program = context.program;
    Function& caller = context.function;
    PassOutcome outcome = PassOutcome::Unchanged;
    for (const InlineSite& site : sites) {
        InlineDecision decision = site.verdict.decision;
        if (decision == InlineDecision::Inline || decision == InlineDecision::Barrier || decision == InlineDecision::SignatureMismatch)
            continue;
        const Value& call = caller.at(site.call);
        const Function& callee = program.at(call.function());
        for (size_t index : site.summary->calledParameters) {
            if (index >= call.numberOfOperands())
                break;
            uint32_t entryParameter = static_cast<uint32_t>(index);
            const Type& passed = program.typeOf(caller, call.operand(entryParameter));
            FunctionID argument = passed.singleFunction();
            BuiltinID builtinArgument = singleBuiltinFunction(program, passed);
            if ((!argument && !builtinArgument) || ProvisionalFunctions::contains(argument) || !canHaveCalleeVersion(program, call.function(), entryParameter))
                continue;
            if (context.exceeds(LimitID::MaxValuesInCalleeVersion, callee.values.size(), site.call))
                break;
            // The type of the parameter lists the functions that it is seen to receive, and each may get a copy of the callee.
            auto wouldHaveTooManyCopies = [&] {
                const Type& parameter = program.typeOf(callee, callee.at(entryBlock).parameters[entryParameter]);
                return context.exceeds(LimitID::MaxBytesOfCalleeVersions, uint64_t { parameter.functions.size() } * site.summary->codeBytes, site.call);
            };
            bool mayBeNullish = passed.mayBeUndefinedOrNull();
            bool sharesCopy = isLibraryCode(program, callee) || isInColderPartOfProgram(context.inputs.heat, program, context.id).predicted();
            // All who pass one built-in function share the copy for it, and few built-in functions are passed at all.
            bool isForCalleeAlone = builtinArgument || ((isHot(heatClassOf(context, site.block)).predicted() || !sharesCopy) && !wouldHaveTooManyCopies());
            caller.edit(site.call, ValueEdit::Definition).setFunction(context.derivedFunction(!isForCalleeAlone
                ? calleeVersionKeyForAnyFunction(call.function(), entryParameter, mayBeNullish)
                : argument ? calleeVersionKey(call.function(), entryParameter, argument, mayBeNullish)
                : calleeVersionKeyForBuiltin(call.function(), entryParameter, builtinArgument, mayBeNullish)));
            outcome = PassOutcome::Changed;
            break;
        }
    }
    // The call names another function. Its operands and its place are as they were.
    if (outcome == PassOutcome::Changed)
        context.analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
    return outcome;
}

static void copyAcceptedSites(FunctionPassContext& context, IRBuilder& builder, std::span<const InlineSite> sites)
{
    Function& caller = context.function;
    uint32_t firstCopiedValue = static_cast<uint32_t>(caller.values.size());
    Vector<ValueID> changedValues;
    for (size_t index = sites.size(); index--;) {
        if (isAccepted(sites[index]))
            changedValues.append(sites[index].call);
    }

    // From the last site to the first: what stands in front of a site stays where it is.
    IRBuilder::DeferredResultReads resultReads(builder, changedValues.span());
    for (size_t index = sites.size(); index--;) {
        const InlineSite& site = sites[index];
        if (isAccepted(site))
            copyIn(context, builder, InlineRequest { { site.block, site.index }, site.call, partToCopy(site), *site.summary, &resultReads });
    }
    resultReads.finish();

    for (uint32_t index = static_cast<uint32_t>(caller.values.size()); index-- > firstCopiedValue;)
        changedValues.append(ValueID(index));
    narrowValueTypes(TypePropagationInputs { context.program, context.id, caller, context.analyses, context.types, context.build, &context.typingMemo }, changedValues.span());
}

PassOutcome inlineCalls(FunctionPassContext& context)
{
    Function& caller = context.function;
    IRBuilder builder(context.analyses.context(), caller, context.types, &context.analyses);
    PassOutcome outcome = PassOutcome::Unchanged;
    // A copied body has calls of its own. The frames of a copy are new, and they keep their numbers where typing cuts the copy and numbers the values again.
    uint32_t firstUndecidedFrame = 0;
    while (true) {
        Vector<InlineSite> sites = findInlineSites(context, firstUndecidedFrame);
        outcome |= markNoReturnCallBlocks(context);
        if (sites.isEmpty())
            break;

        for (InlineSite& site : sites) {
            site.summary = &inliningSummaryOf(context, caller.at(site.call).function());
            InlineDecision legality = canInline(context, *site.summary, site.call);
            site.verdict = legality != InlineDecision::Unknown ? InlineVerdict { .decision = legality } : computeInlineVerdict(context, *site.summary, site.block, site.call);
        }
        spendCallerBudget(context, sites);

        // The traversal in the place of the built-in's own text: a call like any other, which this run goes on to look at.
        BitVector arrayMethodCalls;
        for (const InlineSite& site : sites) {
            if (isAccepted(site) && arrayMethodCallOf(context.program, caller, site.call))
                arrayMethodCalls.set(site.call.index());
        }
        if (!arrayMethodCalls.isEmpty() && fuseArrayMethodsTakenAlone(context, arrayMethodCalls) == PassOutcome::Changed) {
            context.analyses.invalidate(allAnalyses - context.keptAnalyses);
            outcome = PassOutcome::Changed;
            continue;
        }

        bool hasAcceptedSite = false;
        Statistics& statistics = context.build.statistics();
        for (const InlineSite& site : sites) {
            statistics.count(handlingOf(site.verdict.decision).statistic);
            if (handlingOf(site.verdict.decision).followUp == FollowUp::MarkNeverInlined) {
                caller.edit(site.call, ValueEdit::Definition).setIsNeverInlined(true);
                outcome = PassOutcome::Changed;
            }
            if (!isAccepted(site))
                continue;
            hasAcceptedSite = true;
            for (InlineBenefit benefit : site.verdict.benefits)
                statistics.count(statisticFor(benefit));
        }
        outcome |= callCalleeVersions(context, sites.span());
        if (!hasAcceptedSite) {
            if (firstUndecidedFrame || !context.build.options().collectsDeclines())
                break;
            for (const InlineSite& site : sites) {
                DecisionHandling handling = handlingOf(site.verdict.decision);
                if (handling.followUp != FollowUp::ReportDecline || !isHot(heatClassOf(context, site.block)).predicted())
                    continue;

                uint64_t allowedBytes = uint64_t { site.verdict.budgetBytes } + site.verdict.benefitBytes;
                uint64_t excessBytes = site.verdict.costBytes > allowedBytes ? site.verdict.costBytes - allowedBytes : 0;
                context.addDecline(Wire::DeclineKind::HotCallNotInlined, site.call, { }, handling.limit, static_cast<uint32_t>(excessBytes));
            }
            break;
        }
        firstUndecidedFrame = static_cast<uint32_t>(inlineFrameCountOf(caller));
        copyAcceptedSites(context, builder, sites.span());
        outcome = PassOutcome::Changed;
    }
    // Every analysis is dropped behind a copied body, those that look into a call behind the choice of a callee version, the frequencies behind the mark on a block.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return outcome;
}

// Most calls that run go to a function that leaves early: it reads a field or two of what it was handed, tests them and returns.

struct Head {
    FunctionID function;
    BitVector blocks;
    // Only edges that are not expected to be taken lead out of it.
    Expected<bool> holdsEveryLikelyPath { false };
};

struct CallerSites {
    FunctionID caller;
    Vector<ValueSite> sites;
};

static bool hasEffectsOfCall(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Inline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Head> findHead(const AnalysisContext& context)
{
    const Function& function = context.program.at(context.id);
    FunctionAnalyses analyses(context, function);
    uint64_t numberOfBlocks = 0;
    uint64_t operations = 0;
    // What throws in the copy throws what the function would have thrown at that place.
    auto onlyReads = [&](const EffectsOnEdges& effects) {
        const Effects& normal = effects.normal;
        return !normal.mayCollect && !normal.mayRunOtherCode() && !normal.mayBeTerminated && (normal.writes.containsOnly(bookkeepingHeaps) || context.build.isSabotaged(Sabotage::HeadsThatAreCopiedLeaveNoTrace));
    };
    BitVector blocks = findHeadBlocks(function, analyses, [&](BlockID id) {
        const Block& block = function.at(id);
        if (block.handler || !canEndBlockOfHeadOf(block.terminator.kind) || boundToOf(block.terminator.kind) != BoundTo::Nothing)
            return false;
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            const Value& value = function.at(block.values[index]);
            if (boundToOf(value.opcode) != BoundTo::Nothing || isBoundToFrame(function, block, index) || hasEffectsOfCall(value.opcode))
                return false;
            if (hasOperatorForm(value.opcode) && mayCallRuntime(value.opcode, value.form()))
                return false;
        }
        if (!onlyReads(analyses.effectsOfTerminator(id)))
            return false;
        for (ValueID value : block.values) {
            if (!onlyReads(analyses.effectsOf(value)))
                return false;
        }
        // Most functions are turned down by their entry block, and the graph of none of those is made.
        if (analyses.isInCycle(id))
            return false;
        uint64_t size = block.values.size() + 1;
        if (context.build.exceeds(LimitID::MaxSelectedHeadBlocks, numberOfBlocks + 1, analyses.subject()) || context.build.exceeds(LimitID::MaxSelectedHeadOperations, operations + size, analyses.subject()))
            return false;
        ++numberOfBlocks;
        operations += size;
        return true;
    });

    // Every block that is left leads to a Return in the head.
    if (!blocks.get(entryBlock.index()))
        return std::nullopt;
    bool holdsEveryLikelyPath = true;
    for (size_t index : blocks) {
        const auto& targets = function.at(BlockID(index)).terminator.targets;
        for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
            holdsEveryLikelyPath &= blocks.get(targets[targetIndex].block.index()) || analyses.blockFrequencies().classOf(Edge { BlockID(index), targetIndex }).predicted() != BlockFrequency::Normal;
    }
    return Head { context.id, WTF::move(blocks), holdsEveryLikelyPath };
}

static Vector<ValueSite> findCallsOfHeads(const AnalysisContext& context, const Heat& heat, std::span<const Head* const> headOf)
{
    const Program& program = context.program;
    const Function& caller = program.at(context.id);
    Vector<ValueSite> sites;
    if (!caller.hasBody() || context.flow->runsAtMostOnce(program, context.id) || !context.build.isEnabled(PassID::InlineHeads, context.id))
        return sites;

    std::optional<FunctionAnalyses> analyses;
    for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
        const Block& block = caller.blocks[blockIndex];
        if (block.frequency.predicted() != BlockFrequency::Normal || block.handler)
            continue;
        ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
        for (uint32_t valueIndex = 0; valueIndex < block.values.size(); ++valueIndex) {
            ValueID call = block.values[valueIndex];
            const Value& value = caller.at(call);
            if (value.opcode != Opcode::CallDirect || call == tailCall || value.isAfterInlinedHead() || value.function() == context.id || !headOf[value.function().index()])
                continue;
            const Function& callee = program.at(value.function());
            if (value.numberOfOperands() != callee.numberOfEntryParameters() || !program.at(program.provenTypeOf(callee.result())).isSubsetOf(program.at(value.type), program))
                continue;
            if (!analyses) {
                analyses.emplace(context, caller);
                // Without looking at the blocks: none of them is in a cycle or runs more often than the function is entered, and code that runs that often is not hot.
                if (caller.role != FunctionRole::EntryChecks && !isHot(heat.classOf(heat.activationsOf(program, context.id) * roundingOfFrequencySums)).predicted() && runsNoBlockMoreOftenThanEntry(analyses->cfg()))
                    return sites;
            }
            // A copy buys time where time is spent.
            Expected<HeatClass> heatOfCall = heatClassOf(heat, *analyses, BlockID(blockIndex));
            bool isInCycle = analyses->isInCycle(BlockID(blockIndex));
            // The tests in front of a body are entered for the body.
            bool entersOwnBody = caller.role == FunctionRole::EntryChecks && caller.creator.as<CreatorRecord::EntryChecks>().body == value.function();
            if (isHot(heatOfCall).predicted() || (headOf[value.function().index()]->holdsEveryLikelyPath.predicted() && (isInCycle || entersOwnBody) && !isCold(heatOfCall).predicted()))
                sites.append({ BlockID(blockIndex), valueIndex, call });
        }
    }
    return sites;
}

// A copy often returns what it was handed or has read from a slot, and the caller knows the type of that.
static void skipTestsThatArgumentsDecide(IRBuilder& builder, FunctionAnalyses& analyses, const TypeArena& types)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    Vector<std::pair<Edge, Target>, 8> skippingEdges;
    Vector<Edge, 8> edgesFromCalls;
    const CFG& cfg = analyses.cfg();
    const UseLists& useLists = analyses.useLists();
    // A callee that promises its result answers a test that stands right behind the call.
    auto mayPromiseToPass = [&](BlockID block, ValueID result, const Type& tested) {
        if (function.at(block).handler || function.at(block).values.isEmpty() || function.at(block).values.last() != result)
            return false;
        const Value& call = function.at(result);
        if (call.opcode != Opcode::CallDirect || !call.isAfterInlinedHead() || call.isConstruction())
            return false;
        const Function& callee = program.at(call.function());
        return callee.results.size() == 1 && callee.results[0].isExpected() && program.at(callee.results[0].expected().predicted()).isSubsetOf(tested, program);
    };
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        if (block.isHandlerBlock() || block.values.size() != 1 || !block.terminator.isConditionalBranch() || cfg.incomingEdges(id).size() < 2)
            continue;
        const Value& test = function.at(block.values[0]);
        if (test.opcode != Opcode::TypeTest || block.terminator.conditionOperand() != block.values[0] || useLists.useCount(block.values[0]) != 1)
            continue;
        size_t testedIndex = block.parameters.find(test.valueOperand());
        if (testedIndex == notFound)
            continue;
        bool parametersDieInBlock = std::ranges::all_of(block.parameters, [&](ValueID parameter) {
            return std::ranges::all_of(useLists.usesOf(parameter), [&](const Use& use) {
                return use.block == id;
            });
        });
        if (!parametersDieInBlock)
            continue;
        for (Edge edge : cfg.incomingEdges(id)) {
            if (edge.isHandlerEdge() || edge.from == id || function.at(edge.from).terminator.kind != TerminatorKind::Jump)
                continue;
            const Arguments& incomingArguments = targetOf(function, edge).arguments;
            KnownBoolean outcome = evaluateTypeTest(program, program.typeOf(function, incomingArguments[testedIndex]), types.at(test.testedType()));
            if (outcome == KnownBoolean::Unknown) {
                if (mayPromiseToPass(edge.from, incomingArguments[testedIndex], types.at(test.testedType())))
                    edgesFromCalls.append(edge);
                continue;
            }
            Edge chosenEdge { id, outcome == KnownBoolean::True ? 0u : 1u };
            if (targetOf(function, chosenEdge).block == id || !canFollowWithoutTest(function, chosenEdge))
                continue;
            Target target;
            target.block = targetOf(function, chosenEdge).block;
            for (ValueID argument : argumentsWithoutTest(function, chosenEdge)) {
                size_t index = block.parameters.find(argument);
                target.arguments.append(index == notFound ? argument : incomingArguments[index]);
            }
            if (argumentsFitParameters(program, function, target))
                skippingEdges.append({ edge, WTF::move(target) });
        }
    }
    if (skippingEdges.isEmpty() && edgesFromCalls.isEmpty())
        return;
    for (auto& [edge, target] : skippingEdges)
        builder.redirectEdge(edge, WTF::move(target));
    for (Edge edge : edgesFromCalls) {
        BlockID mergePoint = targetOf(function, edge).block;
        Arguments incomingArguments = targetOf(function, edge).arguments;
        Terminator branch = function.at(mergePoint).terminator;
        const Value& test = function.at(function.at(mergePoint).values[0]);
        size_t testedIndex = function.at(mergePoint).parameters.find(test.valueOperand());
        Value copy = IRBuilder::make(Opcode::TypeTest, { incomingArguments[testedIndex] });
        copy.setTestedType(test.testedType());
        builder.setOrigin(test.origin());
        function.edit(edge.from, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(edge.from);
        branch.setConditionOperand(builder.append(WTF::move(copy)));
        for (Target& target : branch.targets) {
            for (ValueID& argument : target.arguments) {
                size_t index = function.at(mergePoint).parameters.find(argument);
                if (index != notFound)
                    argument = incomingArguments[index];
            }
        }
        builder.emitTerminator(WTF::move(branch));
    }
    analyses.didChangeControlFlow();
}

void inlineHeads(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, const ScopedLambda<void(FunctionID)>* readEditedFunction, const DirectCalleesOfFunction* directCalleesOf)
{
    PhaseTimes times(build, PassID::InlineHeads);
    const Program& program = editor.program();
    unsigned numberOfFunctions = program.functions.size();
    auto contextOf = [&](FunctionID id) {
        return AnalysisContext { build, program, &inputs.flow, &inputs.effects, id };
    };

    Vector<Head> heads;
    Vector<CallerSites> callerSites;
    Vector<const Head*> headOf;
    {
        ProgramCaches caches(program, build);
        Vector<unsigned> large = largeFunctions(program, build.parallelLoops());
        heads = collectInParallel<Head>(build.parallelLoops(), numberOfFunctions, large.span(), [&](unsigned index, Vector<Head>& found) {
            const Function& function = program.at(FunctionID(index));
            if (!function.hasBody() || !function.hasOrdinaryFrame() || !function.mayBeCloned() || function.results.size() != 1 || !build.isEnabled(PassID::InlineHeads, FunctionID(index)))
                return;
            if (auto head = findHead(contextOf(FunctionID(index))))
                found.append(WTF::move(*head));
        });
        times.didFinish("find heads"_s);
        if (heads.isEmpty())
            return;
        headOf.fill(nullptr, numberOfFunctions);
        for (const Head& head : heads)
            headOf[head.function.index()] = &head;

        callerSites = collectInParallel<CallerSites>(build.parallelLoops(), numberOfFunctions, large.span(), [&](unsigned index, Vector<CallerSites>& found) {
            const Vector<FunctionID>* directCallees = directCalleesOf ? (*directCalleesOf)(FunctionID(index)) : nullptr;
            if (directCallees && std::ranges::none_of(*directCallees, [&](FunctionID callee) { return !!headOf[callee.index()]; }))
                return;
            Vector<ValueSite> sites = findCallsOfHeads(contextOf(FunctionID(index)), inputs.heat, headOf.span());
            if (!sites.isEmpty())
                found.append({ FunctionID(index), WTF::move(sites) });
        });
    }
    times.didFinish("find sites"_s);
    build.statistics().count(StatisticID::SelectedHeadsFound, heads.size());
    if (callerSites.isEmpty())
        return;

    // A caller may be a callee too, and the callers are edited all at once: the heads are copied from what the callees were.
    Vector<std::unique_ptr<Function>> bodyOf(numberOfFunctions);
    BitVector isCopiedFrom(numberOfFunctions);
    Vector<FunctionID> callees;
    Vector<FunctionID> callers;
    uint64_t numberOfSites = 0;
    for (const CallerSites& entry : callerSites) {
        for (const ValueSite& site : entry.sites) {
            FunctionID callee = program.at(entry.caller).at(site.value).function();
            if (!isCopiedFrom.quickSet(callee.index()))
                callees.append(callee);
        }
        numberOfSites += entry.sites.size();
        callers.append(entry.caller);
    }
    forEachIndexInParallel(build.parallelLoops(), callees.size(), [&](unsigned index) {
        bodyOf[callees[index].index()] = makeUniqueWithoutFastMallocCheck<Function>(program.at(callees[index]));
    });
    build.statistics().count(StatisticID::SelectedHeadsInlined, numberOfSites);
    times.didFinish("copy callees"_s);

    lowerFunctionsInParallel(editor, build, callers.span(), { }, [&](unsigned index, FunctionID id, Function& caller, TaskArenas& arenas) {
        uint32_t firstCopiedValue = static_cast<uint32_t>(caller.values.size());
        Vector<ValueID> changedValues;
        for (const ValueSite& site : callerSites[index].sites | std::views::reverse)
            changedValues.append(site.value);

        FunctionAnalyses analyses(contextOf(id), caller);
        {
            IRBuilder builder(contextOf(id), caller, arenas.types, &analyses);
            // From the last site to the first: what stands in front of a site stays where it is.
            IRBuilder::DeferredResultReads resultReads(builder, changedValues.span());
            for (const ValueSite& site : callerSites[index].sites | std::views::reverse) {
                RELEASE_ASSERT(caller.at(site.block).values[site.index] == site.value);
                FunctionID callee = caller.at(site.value).function();
                copyBodyIn(builder, site.point(), bodyOf[callee.index()].get(), &headOf[callee.index()]->blocks, { }, &resultReads);
                analyses.didChangeControlFlow();
            }
            resultReads.finish();
        }

        // An argument often says more than the parameter does, and decides tests of the copy.
        for (uint32_t value = static_cast<uint32_t>(caller.values.size()); value-- > firstCopiedValue;)
            changedValues.append(ValueID(value));
        narrowValueTypes(TypePropagationInputs { program, id, caller, analyses, arenas.types, build }, changedValues.span());
        // Typing gives a value what its declaration expects of it. Behind selection nothing tests for that any more.
        for (Value& value : caller.editAllValues()) {
            if (value.expected().predicted())
                value.setExpected({ });
        }
        analyses.didChangeControlFlow();
        IRBuilder builder(contextOf(id), caller, arenas.types, &analyses);
        skipTestsThatArgumentsDecide(builder, analyses, arenas.types);
    });
    times.didFinish("copy heads in"_s);
    if (readEditedFunction)
        readFunctionsInParallel(program, build, callers.span(), *readEditedFunction);
    times.didFinish("read callers again"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
