/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExceptionEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTCodeRecords.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeValidation.h"
#include "AOTVector.h"
#include "AirStackSlot.h"
#include "AirCode.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3PatchpointValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "Exception.h"

namespace JSC { namespace AOT { namespace Typed {

ExceptionEmitter::ExceptionEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_landingPadsBySuccessor(Box<LandingPadsBySuccessor>::create())
{
    findValuesCaughtByCleanups();
    for (const Block& block : m_function.blocks) {
        if (!block.handler)
            continue;
        BlockID handler = block.handler->block;
        TerminatorKind kind = block.terminator.kind;
        if (kind == TerminatorKind::Rethrow || (kind == TerminatorKind::Throw && (handlerTypeOf(handler) != HandlerType::Finally || keepsException(handler))))
            m_handlersEnteredByJump.set(handler.index());
    }
}

// A cleanup that takes the value hands it from block to block, to the Throw at its end.
void ExceptionEmitter::findValuesCaughtByCleanups()
{
    struct Caught {
        ValueID value;
        BlockID cleanup;
    };
    Vector<Caught, 4> worklist;
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        BlockID id(index);
        const Block& block = m_function.at(id);
        if (!block.isHandlerBlock() || block.role == BlockRole::CatchHandler)
            continue;
        for (ValueID value : block.values) {
            if (m_function.at(value).opcode == Opcode::CatchValue)
                worklist.append({ value, id });
        }
    }
    if (worklist.isEmpty())
        return;

    struct Edge {
        ValueID argument;
        ValueID parameter;
    };
    Vector<Edge> edges;
    for (const Block& block : m_function.blocks) {
        block.forEachTarget([&](const Target& target) {
            const auto& parameters = m_function.at(target.block).parameters;
            for (size_t i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                edges.append({ target.arguments[i], parameters[i] });
        });
    }
    AOT::sort(edges, { }, [](const Edge& edge) { return edge.argument.index(); });
    while (!worklist.isEmpty()) {
        Caught caught = worklist.takeLast();
        auto added = m_valuesCaughtByCleanups.add(caught.value, caught.cleanup);
        if (!added.isNewEntry) {
            if (added.iterator->value == caught.cleanup || !added.iterator->value)
                continue;
            caught.cleanup = BlockID();
            added.iterator->value = caught.cleanup;
        }
        auto first = std::ranges::lower_bound(edges, caught.value.index(), { }, [](const Edge& edge) { return edge.argument.index(); });
        for (; first != edges.end() && first->argument == caught.value; ++first)
            worklist.append({ first->parameter, caught.cleanup });
    }
    findCleanupsThatKeepException();
}

// Where the function itself ends what the cleanup throws on, nobody lists the frames of the throw. What a suspended frame holds beside its values is not kept alive.
void ExceptionEmitter::findCleanupsThatKeepException()
{
    if (m_function.role == FunctionRole::CoroutineBody)
        return;
    struct ThrowOfCleanup {
        BlockID cleanup;
        BlockID handler;
    };
    BitVector throwsOn;
    Vector<ThrowOfCleanup, 4> throws;
    for (const Block& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::Throw)
            continue;
        BlockID cleanup = m_valuesCaughtByCleanups.get(block.terminator.valueOperand());
        if (!cleanup || handlerTypeByOperations(cleanup) != HandlerType::Catch)
            continue;
        throwsOn.set(cleanup.index());
        throws.append({ cleanup, block.handler ? block.handler->block : BlockID() });
    }

    // What a cleanup throws on leaves the function, is ended by a handler of the function, or goes to another cleanup that throws it on.
    Vector<BlockID, 4> worklist;
    Vector<ThrowOfCleanup, 4> throwsIntoCleanups;
    for (ThrowOfCleanup thrown : throws) {
        for (BlockID handler = thrown.handler; ; handler = m_function.at(handler).handler ? m_function.at(handler).handler->block : BlockID()) {
            if (!handler) {
                if (!m_cleanupsThatKeepException.set(thrown.cleanup.index()))
                    worklist.append(thrown.cleanup);
                break;
            }
            if (throwsOn.get(handler.index())) {
                throwsIntoCleanups.append({ thrown.cleanup, handler });
                break;
            }
            if (handlerTypeByOperations(handler) != HandlerType::Finally)
                break;
        }
    }
    AOT::sort(throwsIntoCleanups, { }, [](const ThrowOfCleanup& thrown) { return thrown.handler.index(); });
    while (!worklist.isEmpty()) {
        BlockID outer = worklist.takeLast();
        auto first = std::ranges::lower_bound(throwsIntoCleanups, outer.index(), { }, [](const ThrowOfCleanup& thrown) { return thrown.handler.index(); });
        for (; first != throwsIntoCleanups.end() && first->handler == outer; ++first) {
            if (!m_cleanupsThatKeepException.set(first->cleanup.index()))
                worklist.append(first->cleanup);
        }
    }
}

HandlerType ExceptionEmitter::handlerTypeByOperations(BlockID handler) const
{
    HandlerType type = HandlerType::SynthesizedCatch;
    for (ValueID id : m_function.at(handler).values) {
        Opcode opcode = m_function.at(id).opcode;
        if (opcode == Opcode::CatchException)
            return HandlerType::Finally;
        if (opcode != Opcode::CatchValue)
            break;
        type = HandlerType::Catch;
    }
    return type;
}

HandlerType ExceptionEmitter::handlerTypeOf(BlockID handler) const
{
    return keepsException(handler) ? HandlerType::Finally : handlerTypeByOperations(handler);
}

static WTF::Vector<Box<CCallHelpers::Label>> emitSiteAndJumpToContinuation(CCallHelpers& jit, const B3::StackmapGenerationParams& params, B3::StackmapGenerator& siteGenerator, bool mayReturn)
{
    siteGenerator.run(jit, params);
    WTF::Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
    if (mayReturn && !params.fallsThroughToSuccessor(0)) {
        params.addLatePath([jump = jit.jump(), returnTarget = labels[0]](CCallHelpers& jit) {
            jump.linkTo(*returnTarget, &jit);
        });
    }
    return labels;
}

void ExceptionEmitter::attachLandingPad(const OperationContext& context, B3::PatchpointValue* patchpoint, uint32_t site, bool mayReturn)
{
    const Target* handler = context.handler();
    if (!handler)
        return;

    LBasicBlock block = m_out.m_block;
    RELEASE_ASSERT(block->last() == patchpoint && patchpoint->generator());
    RELEASE_ASSERT(!mayReturn || !block->numSuccessors());
    LBasicBlock landingPad = landingPadFor(*handler);
    LBasicBlock continuation = mayReturn ? newBlock() : nullptr;
    if (continuation)
        block->appendSuccessor(normalEdge(continuation));
    unsigned landingPadIndex = block->numSuccessors();
    block->appendSuccessor(rareEdge(landingPad));
    RegisterSet clobberedOnTheWay = emitter<RuntimeCallEmitter>().registersClobberedByCalls();
    patchpoint->clobberLateOnSuccessor(landingPadIndex, clobberedOnTheWay);
    patchpoint->effects.terminal = true;

    B3::Air::StackSlot* registerStorage = generator().checksKeptRegisters() ? emitter<RuntimeCallEmitter>().landingPadRegisterStorage() : nullptr;
    if (registerStorage)
        patchpoint->clobberEarly(RegisterSet::macroClobberedGPRs());
    clobberedOnTheWay.merge(patchpoint->earlyClobbered());
    clobberedOnTheWay.merge(patchpoint->lateClobbered());
    patchpoint->setGenerator([records = &m_records.exceptions, indexBySuccessor = m_landingPadsBySuccessor, siteGenerator = patchpoint->generator(), site, type = handlerTypeOf(handler->block), mayReturn, registerStorage, stubCalls = &m_records.stubCalls, clobbered = WTF::move(clobberedOnTheWay), function = m_id](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        std::optional<KeptRegisterCheck> check;
        if (registerStorage) {
            // A register that the function does not save holds a value of whoever called or resumed it, and a suspension changes who that is.
            RegisterSet notCompared = RegisterSet::vmCalleeSaveRegisters();
            notCompared.exclude(params.code().calleeSaveRegisters());
            notCompared.filter(params.code().mutableRegs());
            notCompared.merge(clobbered);
            check = KeptRegisterCheck { notCompared, params.code().addressInFrame(registerStorage->offsetFromFP()), function, ValueID().index() };
            emitStoresOfKeptRegisters(jit, *check);
        }
        Box<CCallHelpers::Label> successor = emitSiteAndJumpToContinuation(jit, params, *siteGenerator, mayReturn).last();
        // What a check compares depends on the site, so it is not shared.
        uint32_t landingPadNumber = static_cast<uint32_t>(records->landingPads.size());
        bool isNewEntry = true;
        if (!check) {
            auto added = indexBySuccessor->add(successor.get(), landingPadNumber);
            landingPadNumber = added.iterator->value;
            isNewEntry = added.isNewEntry;
        }
        // On x86-64 the stub that jumps to a landing pad has set the stack pointer.
        if (isNewEntry && isX86_64() && !check)
            records->landingPads.append(successor);
        else if (isNewEntry) {
            Box<CCallHelpers::Label> unwindTarget = Box<CCallHelpers::Label>::create();
            records->landingPads.append(unwindTarget);
            params.addLatePath([unwindTarget, successor, bytesBelowFrameRecord = params.code().bytesBelowFrameRecord(), check, stubCalls](CCallHelpers& jit) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                *unwindTarget = jit.label();
                if (!isX86_64())
                    jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(bytesBelowFrameRecord)), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
                if (check) {
                    emitChecksOfKeptRegisters(jit, *check, ValidationFailureKind::UnwindingChangedKeptRegister, std::nullopt, [&] {
                        stubCalls->reportChangedRegister(jit);
                    });
                }
                jit.jump().linkTo(*successor, &jit);
            });
        }
        records->sites.append(SiteLandingPad { site, landingPadNumber, type });
    });

    if (continuation)
        m_out.appendTo(continuation);
}

LBasicBlock ExceptionEmitter::attachLandingOfMisfit(B3::PatchpointValue* patchpoint, uint32_t site)
{
    LBasicBlock block = m_out.m_block;
    RELEASE_ASSERT(block->last() == patchpoint && patchpoint->generator() && !block->numSuccessors() && !generator().checksKeptRegisters());
    LBasicBlock continuation = newBlock();
    LBasicBlock landing = newBlock();
    block->appendSuccessor(normalEdge(continuation));
    block->appendSuccessor(rareEdge(landing));
    patchpoint->effects.terminal = true;
    patchpoint->setGenerator([records = &m_records.exceptions, siteGenerator = patchpoint->generator(), site](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        Box<CCallHelpers::Label> landing = emitSiteAndJumpToContinuation(jit, params, *siteGenerator, true)[1];
        uint32_t landingPadNumber = static_cast<uint32_t>(records->landingPads.size());
        records->landingPads.append(landing);
        records->sites.append(SiteLandingPad { site, landingPadNumber, landingOfMisfit });
    });
    m_out.appendTo(continuation);
    return landing;
}

LBasicBlock ExceptionEmitter::landingPadFor(const Target& handler)
{
    auto& landingPads = m_landingPads.add(handler.block, Vector<LandingPad, 2> { }).iterator->value;
    uint64_t maxSearched = build().limit(LimitID::MaxLandingPadsSearched);
    for (size_t i = landingPads.size(); i-- && i + maxSearched >= landingPads.size();) {
        if (landingPads[i].arguments == handler.arguments)
            return landingPads[i].block;
    }
    if (landingPads.size() > maxSearched)
        build().gaveUp(LimitID::MaxLandingPadsSearched, analyses().subject());

    LBasicBlock landingPad = newBlock();
    landingPads.append(LandingPad { handler.arguments, landingPad });

    LBasicBlock siteBlock = m_out.m_block;
    m_out.appendTo(landingPad);
    // No operation of such a handler takes the pending exception from the run time.
    if (handlerTypeOf(handler.block) == HandlerType::SynthesizedCatch)
        callWithoutSite<Boxed>(RuntimeOperation::Catch, { }, { .isOnRarePath = true, .followsThrow = true });
    if (isEnteredByJump(handler.block))
        setCaughtVariable(handler.block, m_out.int64Zero);
    generator().passHandlerArguments(handler);
    m_out.jump(generator().blockFor(handler.block));
    m_out.appendTo(siteBlock);
    return landingPad;
}

B3::Variable* ExceptionEmitter::caughtVariableFor(BlockID handler)
{
    return m_caughtVariables.ensure(handler, [&] {
        return m_proc.addVariable(B3::Int64);
    }).iterator->value;
}

void ExceptionEmitter::setCaughtVariable(BlockID handler, LValue caught)
{
    m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), caughtVariableFor(handler), caught);
}

// Only a throw writes the word, so that a way through the cleanup on which nothing was thrown pays nothing.
TypedPointer ExceptionEmitter::wordKeptBy(BlockID cleanup)
{
    B3::Air::StackSlot* slot = m_keptWords.ensure(cleanup, [&] {
        return m_proc.addStackSlot(sizeof(uint64_t));
    }).iterator->value;
    return m_out.address(m_heaps.frameSlots, m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), slot), 0);
}

void ExceptionEmitter::keep(BlockID cleanup, LValue kept)
{
    m_out.store64(kept, wordKeptBy(cleanup));
}

// Zero unless the value comes from one cleanup, which keeps something.
LValue ExceptionEmitter::keptBy(ValueID thrown)
{
    BlockID cleanup = m_valuesCaughtByCleanups.get(thrown);
    if (!cleanup || !keepsException(cleanup))
        return m_out.int64Zero;
    return m_out.load64(wordKeptBy(cleanup));
}

LValue ExceptionEmitter::caughtWord(const ValueContext& value)
{
    BlockID handler = value.block();
    if (LValue caught = m_caughtWords.get(handler))
        return caught;

    RuntimeOperation operation = handlerTypeOf(handler) == HandlerType::Finally ? RuntimeOperation::CatchException : RuntimeOperation::Catch;
    auto takeFromRuntime = [&] {
        if (!keepsException(handler))
            return callOperation<Boxed>(value, operation, { }, { .followsThrow = true });
        StubCallResult caught = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::CatchValueAndException, { }, { .followsThrow = true });
        keep(handler, caught.second);
        return Boxed(caught.first);
    };
    auto takePassedOrFromRuntime = [&] {
        Boxed passed(m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), caughtVariableFor(handler)));
        LBasicBlock wasPassed = newBlock();
        LBasicBlock wasUnwound = newBlock();
        Join<Boxed> join(m_out);
        m_out.branch(isNotEmpty(passed).value(), normalEdge(wasPassed), normalEdge(wasUnwound));
        m_out.appendTo(wasPassed);
        join.add(passed);
        m_out.appendTo(wasUnwound);
        join.add(takeFromRuntime());
        return join.finish();
    };
    LValue caught = (isEnteredByJump(handler) ? takePassedOrFromRuntime() : takeFromRuntime()).value();
    m_caughtWords.add(handler, caught);
    return caught;
}

Boxed ExceptionEmitter::valueOfException(CellPointer exception)
{
    return Boxed(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), exception.value(), JSC::Exception::valueOffset())));
}

void ExceptionEmitter::generateCatchValue(const ValueContext& value)
{
    switch (handlerTypeOf(value.block())) {
    case HandlerType::Catch:
        define(value, Boxed(caughtWord(value)));
        return;
    case HandlerType::Finally:
        define(value, keepsException(value.block()) ? Boxed(caughtWord(value)) : valueOfException(CellPointer(caughtWord(value))));
        return;
    case HandlerType::SynthesizedCatch:
    case HandlerType::SynthesizedFinally:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ExceptionEmitter::generateCatchException(const ValueContext& value)
{
    RELEASE_ASSERT(handlerTypeByOperations(value.block()) == HandlerType::Finally);
    define(value, Operand(value.representation(), caughtWord(value)));
}

void ExceptionEmitter::jumpToHandler(const Target& handler, LValue caught)
{
    setCaughtVariable(handler.block, caught);
    generator().passHandlerArguments(handler);
    m_out.jump(generator().blockFor(handler.block));
}

void ExceptionEmitter::throwByOperation(const TerminatorContext& terminator, RuntimeOperation operation, Boxed thrown)
{
    callVoidOperation(terminator, operation, { thrown });
    m_out.unreachable();
}

void ExceptionEmitter::throwAfterCleanup(const TerminatorContext& terminator, Boxed thrown)
{
    callVoidOperation(terminator, RuntimeOperation::ThrowAfterCleanup, { thrown, Integer(keptBy(terminator->valueOperand())) });
    m_out.unreachable();
}

void ExceptionEmitter::throwValue(const TerminatorContext& terminator, Boxed thrown)
{
    const Target* handler = terminator.handler();
    bool endsCleanup = m_valuesCaughtByCleanups.contains(terminator->valueOperand());
    if (!handler) {
        if (endsCleanup)
            throwAfterCleanup(terminator, thrown);
        else
            throwByOperation(terminator, RuntimeOperation::Throw, thrown);
        return;
    }
    switch (handlerTypeOf(handler->block)) {
    case HandlerType::Catch:
    case HandlerType::SynthesizedCatch:
        jumpToHandler(*handler, thrown.value());
        return;
    case HandlerType::Finally:
        if (keepsException(handler->block)) {
            keep(handler->block, endsCleanup ? keptBy(terminator->valueOperand()) : callOperation<Integer>(terminator, RuntimeOperation::AddressOfThrow, { }, { .capturesStackTrace = true }).value());
            jumpToHandler(*handler, thrown.value());
        } else if (endsCleanup)
            throwAfterCleanup(terminator, thrown);
        else
            throwByOperation(terminator, RuntimeOperation::ThrowToHandlerInFunction, thrown);
        return;
    case HandlerType::SynthesizedFinally:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ExceptionEmitter::rethrow(const TerminatorContext& terminator, CellPointer exception)
{
    const Target* handler = terminator.handler();
    if (!handler) {
        throwByOperation(terminator, RuntimeOperation::Throw, boxCell(exception));
        return;
    }
    switch (handlerTypeOf(handler->block)) {
    case HandlerType::Finally:
        if (keepsException(handler->block)) {
            keep(handler->block, exception.value());
            jumpToHandler(*handler, valueOfException(exception).value());
        } else
            jumpToHandler(*handler, exception.value());
        return;
    case HandlerType::Catch:
    case HandlerType::SynthesizedCatch:
        jumpToHandler(*handler, valueOfException(exception).value());
        return;
    case HandlerType::SynthesizedFinally:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
