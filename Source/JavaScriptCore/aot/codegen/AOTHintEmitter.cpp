/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHintEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTCodeRecords.h"
#include "AOTCodeSites.h"
#include "AOTExceptionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTInductionVariables.h"
#include "AOTInstance.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "LinkBuffer.h"
#include "StackManager.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr ptrdiff_t offsetOfTrapAwareSoftStackLimit = AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit();
static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);

static constexpr StubKey pollSlowKey = callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });

static void emitFaultingLoad(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool isFrameless = !params.context().frameIsSetUp;
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    size_t start = jit.m_assembler.codeSize();
    jit.load32(CCallHelpers::Address(instanceGPR, InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0)), jit.scratchRegister());
    RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
    jit.addLinkTask([=](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), isFrameless, OptionalIndex { site } });
    });
}

static void emitCompareAndCall(CCallHelpers& jit, const B3::StackmapGenerationParams& params, CodeRecords* records, uint32_t site)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    RELEASE_ASSERT(params.context().frameIsSetUp);
    CCallHelpers::Jump isInterrupted = jit.branchTest64(CCallHelpers::Signed, CCallHelpers::Address(instanceGPR, offsetOfTrapAwareSoftStackLimit));
    CCallHelpers::Label done = jit.label();
    params.addLatePath([=](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        isInterrupted.link(&jit);
        records->stubCalls.call(jit, pollSlowKey, offsetInRuntimeTable(AOT::Entry::operationAOTPollSlow), OptionalIndex { site });
        jit.jump().linkTo(done, &jit);
    });
}


static bool readsLengthOfData(InlineBuiltin builtin)
{
    return builtin == InlineBuiltin::ArrayLength || builtin == InlineBuiltin::StringLength || builtin == InlineBuiltin::TypedArrayLength;
}

namespace {

// Of all the turns of a loop, or of those that stay on its likely paths.
enum class Turns : uint8_t { All, Likely };

} // anonymous namespace

// An operation of the loop that names the kind of the array's storage relies on a proof of it. Where the proof was made in front of the loop, the storage has been of that kind ever since.
// Where it is made in the loop, a turn on which it fails leaves the likely paths.
static bool hasProvenStorage(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID array, Turns turns)
{
    const Function& function = analyses.function();
    auto locate = [&](ValueID parameter) {
        return analyses.useLists().locationOf(parameter);
    };
    for (unsigned index = 0; index < loop.size(); ++index) {
        if (function.at(loop.at(index)).frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID id : function.at(loop.at(index)).values) {
            const Value& user = function.at(id);
            if (!proofsNeededFromGuard(analyses.program(), function, user).contains(Proof::ArrayStorage) || !user.guardOperand() || analyses.identityOf(guardedOperandOf(user)) != array)
                continue;
            bool isProven = false;
            bool isProvenInFrontOfLoop = true;
            forEachGuardOrigin(analyses.program(), function, analyses.cfg(), locate, user.guardOperand(), [&](Proof proof, const GuardOrigin& origin) {
                if (proof == Proof::ArrayStorage) {
                    isProven = true;
                    isProvenInFrontOfLoop &= origin.source == GuardSource::Test && !analyses.naturalLoops().belongsTo(analyses.useLists().locationOf(origin.test).block, loop);
                }
                return IterationStatus::Continue;
            });
            if (isProven && (isProvenInFrontOfLoop || turns == Turns::Likely))
                return true;
        }
    }
    return false;
}

// The length of an array may be set to more than the array holds. Then the array is not dense, and its storage is none of the kinds that have a name.
static bool holdsItsLength(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID array, Turns turns)
{
    const Type& type = analyses.program().typeOf(analyses.function(), array);
    if (pristineAspectsProvenBy(type, BuiltinClasses::Array).contains(PristineAspects::Dense))
        return true;
    const ClassPart* part = type.singleBuiltinClass();
    if (part && !type.mayBeUndefinedOrNull() && part->contentsAreProven && part->arrayStorage != ArrayStorage::Other)
        return true;
    return hasProvenStorage(analyses, loop, analyses.identityOf(array), turns);
}

// A text that was joined out of others has a length and no characters until it is laid out flat. An operation on a likely path of the loop that works on the flat form has seen to that.
static bool isLaidOutFlat(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID string)
{
    const Function& function = analyses.function();
    auto isFlat = [&](ValueID value) {
        return analyses.program().typeOf(function, value).isOpaque(OpaqueKind::FlatString);
    };
    if (isFlat(string))
        return true;
    ValueID text = analyses.identityOf(string);
    for (unsigned index = 0; index < loop.size(); ++index) {
        const Block& block = function.at(loop.at(index));
        if (block.frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID id : block.values) {
            const Value& user = function.at(id);
            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                ValueID operand = user.operand(operandIndex);
                if (!operand || !isFlat(operand))
                    continue;
                ValueID flat = analyses.identityOf(operand);
                if (flat == text || (function.at(flat).opcode == Opcode::FlattenString && analyses.identityOf(function.at(flat).operand(0)) == text))
                    return true;
            }
        }
    }
    return false;
}

static bool isLengthOfData(FunctionAnalyses& analyses, const NaturalLoop& loop, InlineBuiltin builtin, ValueID receiver, Turns turns)
{
    if (!readsLengthOfData(builtin))
        return false;
    if (builtin == InlineBuiltin::StringLength)
        return receiver && isLaidOutFlat(analyses, loop, receiver);
    if (builtin == InlineBuiltin::ArrayLength)
        return receiver && holdsItsLength(analyses, loop, receiver, turns);
    return true;
}

static bool isLengthOfDataWhereComputed(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID id, Turns turns)
{
    const Value& value = analyses.function().at(analyses.identityOf(id));
    switch (value.opcode) {
    case Opcode::GetArrayLength:
        return holdsItsLength(analyses, loop, value.operand(0), turns);
    case Opcode::GetStringLength:
        return isLaidOutFlat(analyses, loop, value.operand(0));
    case Opcode::GetTypedArrayLength:
        return true;
    case Opcode::CallInline:
        return isLengthOfData(analyses, loop, value.inlineBuiltin(), value.operand(0), turns);
    case Opcode::CallBuiltin:
        return analyses.program().builtins().at(value.overload()).builtin == Builtins::TypedArrayPrototypeLength;
    default:
        return false;
    }
}

static bool isLengthOfData(FunctionAnalyses& analyses, const NaturalLoop& loop, ValueID id, Turns turns)
{
    const Function& function = analyses.function();
    ValueLocation location = analyses.useLists().locationOf(analyses.identityOf(id));
    if (location.placement != ValuePlacement::Parameter)
        return isLengthOfDataWhereComputed(analyses, loop, id, turns);
    if (location.block == entryBlock)
        return false;
    return std::ranges::all_of(analyses.cfg().incomingEdges(location.block), [&](Edge edge) {
        if (edge.isHandlerEdge())
            return false;
        const Terminator& terminator = function.at(edge.from).terminator;
        const Target& target = terminator.targets[edge.targetIndex];
        if (location.index < target.arguments.size())
            return isLengthOfDataWhereComputed(analyses, loop, target.arguments[location.index], turns);
        return terminator.kind == TerminatorKind::TryCallInline && &target == &terminator.done() && isLengthOfData(analyses, loop, terminator.inlineBuiltin(), terminator.receiverOperand(), turns);
    });
}

static bool countsUpToLengthOfData(FunctionAnalyses& analyses, const NaturalLoop& loop, Turns turns)
{
    return std::ranges::any_of(analyses.inductionVariables().of(loop).variables, [&](const InductionVariable& counter) {
        if (counter.direction != InductionDirection::Increasing || counter.step.kind != InductionStepKind::Constant)
            return false;
        const InductionTest* test = counter.boundingTest();
        return test && isLengthOfData(analyses, loop, test->bound, turns);
    });
}

static Expected<bool> likelyPathsAlwaysComplete(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID id = loop.at(index);
        if (loops.innerMostLoopOf(id) != &loop)
            return false;
        const Block& block = function.at(id);
        if (block.frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID value : block.values) {
            if (function.at(value).opcode != Opcode::LoopHint && !analyses.effectsOf(value).onAnyEdge().alwaysCompletes())
                return false;
        }
        if (!analyses.effectsOfTerminator(id).onAnyEdge().alwaysCompletes())
            return false;
    }
    return true;
}

HintEmitter::HintEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Expected<bool> HintEmitter::endsWithinLengthOfData(BlockID block)
{
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(block);
    return loop && countsUpToLengthOfData(analyses(), *loop, Turns::All) && likelyPathsAlwaysComplete(analyses(), *loop).predicted();
}

Expected<bool> HintEmitter::likelyTurnsEndWithinLengthOfData(BlockID block)
{
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(block);
    return loop && countsUpToLengthOfData(analyses(), *loop, Turns::Likely) && likelyPathsAlwaysComplete(analyses(), *loop).predicted();
}

// In a function without handlers an allocation that finds no room throws by itself.
bool HintEmitter::allocatesWhereErrorsAreCaught(BlockID block)
{
    if (!analyses().cfg().hasHandlers())
        return false;
    const NaturalLoop* loop = analyses().naturalLoops().innerMostLoopOf(block);
    if (!loop)
        return false;
    for (unsigned index = 0; index < loop->size(); ++index) {
        BlockID member = loop->at(index);
        for (ValueID id : m_function.at(member).values) {
            if (m_function.at(id).opcode != Opcode::LoopHint && analyses().effectsOf(id).onAnyEdge().mayCollect)
                return true;
        }
        if (analyses().effectsOfTerminator(member).onAnyEdge().mayCollect)
            return true;
    }
    return false;
}

void HintEmitter::generateLoopHint(const ValueContext& value)
{
    if (generator().validates())
        emitter<BarrierEmitter>().validateBeforeCollection(value);
    if (!endsWithinLengthOfData(value.block()).predicted() || allocatesWhereErrorsAreCaught(value.block()))
        emitPoll(value);
}

void HintEmitter::pollWhereNoFrameIsSetUp(const OperationContext& context)
{
    RELEASE_ASSERT(hasFaultingLoopPolls);
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setRunsWithoutFrame(true);
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(context);
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        if (!params.context().frameIsSetUp)
            emitFaultingLoad(jit, params, records, site);
    });
}

void HintEmitter::emitPoll(const ValueContext& value)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->isLoopPoll = !emitter<ExceptionEmitter>().unwindsWithinFunction(value);
    patchpoint->throwsWhatIsOwed = allocatesWhereErrorsAreCaught(value.block());
    patchpoint->likelyTurnsEndWithinData = likelyTurnsEndWithinLengthOfData(value.block()).predicted();
    Effects effects = value.effects().onAnyEdge();
    RELEASE_ASSERT(effects.mayBeTerminated && effects.mayThrow && !effects.mayThrowByRule());
    // Handling a trap stops the thread for the collector or ends it. It looks at no object of the program.
    emitter<RuntimeCallEmitter>().applyEffects(patchpoint, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    uint32_t site = emitter<RuntimeCallEmitter>().addSite(value);

    if (hasFaultingLoopPolls) {
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            emitFaultingLoad(jit, params, records, site);
        });
        emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
        return;
    }

    patchpoint->clobberLate(inputs().stubCatalog.summaryOf(pollSlowKey).clobbered);
    m_records.totals.entriesUsed.set(static_cast<unsigned>(AOT::Entry::operationAOTPollSlow));
    patchpoint->setGenerator([records = &m_records, site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitCompareAndCall(jit, params, records, site);
    });
    emitter<ExceptionEmitter>().attachLandingPad(value, patchpoint, site, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
