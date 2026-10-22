/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExceptionEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeRecords.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeValidation.h"
#include "AirStackSlot.h"
#include "AirCode.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "AOTHandlerTable.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3VariableValue.h"
#include "Exception.h"

namespace JSC { namespace AOT { namespace Typed {

ExceptionEmitter::ExceptionEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_landingPadsBySuccessor(Box<LandingPadsBySuccessor>::create())
{
    for (const Block& block : m_function.blocks) {
        if (!block.handler)
            continue;
        BlockID handler = block.handler->block;
        TerminatorKind kind = block.terminator.kind;
        if (kind == TerminatorKind::Rethrow || (kind == TerminatorKind::Throw && handlerTypeOf(handler) != HandlerType::Finally))
            m_handlersEnteredByJump.set(handler.index());
    }
}

HandlerType ExceptionEmitter::handlerTypeOf(BlockID handler) const
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
    patchpoint->clobberLateOnSuccessor(landingPadIndex, registersClobberedAtLandingPads());
    patchpoint->effects.terminal = true;

    B3::Air::StackSlot* registerStorage = generator().checksKeptRegisters() ? emitter<RuntimeCallEmitter>().landingPadRegisterStorage() : nullptr;
    if (registerStorage)
        patchpoint->clobberEarly(RegisterSet::macroClobberedGPRs());
    RegisterSet clobberedOnTheWay = registersClobberedAtLandingPads();
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
        siteGenerator->run(jit, params);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        if (mayReturn && !params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), returnTarget = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*returnTarget, &jit);
            });
        }
        Box<CCallHelpers::Label> successor = labels.last();
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
        siteGenerator->run(jit, params);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        if (!params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), returnTarget = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*returnTarget, &jit);
            });
        }
        uint32_t landingPadNumber = static_cast<uint32_t>(records->landingPads.size());
        records->landingPads.append(labels[1]);
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
    generateLandingPad(handler, landingPad);
    return landingPad;
}

void ExceptionEmitter::generateLandingPad(const Target& handler, LBasicBlock landingPad)
{
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
}

B3::Variable* ExceptionEmitter::caughtVariableFor(BlockID handler)
{
    return m_caughtVariables.ensure(handler, [&] {
        return m_proc.addVariable(B3::Int64);
    }).iterator->value;
}

LValue ExceptionEmitter::getCaughtVariable(BlockID handler)
{
    return m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), caughtVariableFor(handler));
}

void ExceptionEmitter::setCaughtVariable(BlockID handler, LValue caught)
{
    m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), caughtVariableFor(handler), caught);
}

LValue ExceptionEmitter::caughtWord(const ValueContext& value)
{
    BlockID handler = value.block();
    if (LValue caught = m_caughtWords.get(handler))
        return caught;

    RuntimeOperation operation = handlerTypeOf(handler) == HandlerType::Finally ? RuntimeOperation::CatchException : RuntimeOperation::Catch;
    auto takeFromRuntime = [&] {
        return callOperation<Boxed>(value, operation, { }, { .followsThrow = true });
    };
    if (!isEnteredByJump(handler)) {
        LValue caught = takeFromRuntime().value();
        m_caughtWords.add(handler, caught);
        return caught;
    }

    Boxed passed(getCaughtVariable(handler));
    LBasicBlock wasPassed = newBlock();
    LBasicBlock wasUnwound = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isNotEmpty(passed).value(), normalEdge(wasPassed), normalEdge(wasUnwound));
    m_out.appendTo(wasPassed);
    join.add(passed);
    m_out.appendTo(wasUnwound);
    join.add(takeFromRuntime());
    LValue caught = join.finish().value();
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
        define(value, valueOfException(CellPointer(caughtWord(value))));
        return;
    case HandlerType::SynthesizedCatch:
    case HandlerType::SynthesizedFinally:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ExceptionEmitter::generateCatchException(const ValueContext& value)
{
    RELEASE_ASSERT(handlerTypeOf(value.block()) == HandlerType::Finally);
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

void ExceptionEmitter::throwValue(const TerminatorContext& terminator, Boxed thrown)
{
    const Target* handler = terminator.handler();
    if (!handler) {
        throwByOperation(terminator, RuntimeOperation::Throw, thrown);
        return;
    }
    switch (handlerTypeOf(handler->block)) {
    case HandlerType::Catch:
    case HandlerType::SynthesizedCatch:
        jumpToHandler(*handler, thrown.value());
        return;
    case HandlerType::Finally:
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

RegisterSet ExceptionEmitter::registersClobberedAtLandingPads() const
{
    return m_generator.emitter<RuntimeCallEmitter>().registersClobberedByCalls();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
