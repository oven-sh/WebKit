/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExceptionEmitter.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCodeRecords.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "Exception.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

ExceptionEmitter::ExceptionEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
    if (!m_code.hasHandlers())
        return;

    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfBlocks = m_code.numberOfBlocks();
    m_caughtPhis = arena.createSpan<LValue>(numberOfBlocks);
    m_keptWords = arena.createSpan<Machine::StackSlot*>(numberOfBlocks);
    m_caughtWords = arena.createSpan<LValue>(numberOfBlocks);
    m_landingPadsOfBlocks = arena.createSpan<uint32_t>(numberOfBlocks);

    Vector<BlockID, 8> blocksThatThrowIntoHandlers;
    findValuesCaughtByCleanups(blocksThatThrowIntoHandlers);
    for (BlockID id : blocksThatThrowIntoHandlers) {
        BlockID handler = handlerOf(id);
        if (!handler)
            continue;
        if (m_code.at(id).terminator.kind == TerminatorKind::Rethrow || handlerTypeOf(handler) != HandlerType::Finally || keepsException(handler))
            m_handlersEnteredByJump.set(handler.index());
    }
}

BlockID ExceptionEmitter::handlerOf(BlockID block) const
{
    EdgeID handler = m_code.handlerOf(block);
    return handler ? m_code.at(handler).to : BlockID();
}

uint32_t ExceptionEmitter::findOrAddLandingPad(const Edge& handler)
{
    unsigned hash = handler.to.index();
    for (ValueID argument : handler.arguments())
        hash = WTF::pairIntHash(hash, argument.index());
    uint64_t key = static_cast<uint64_t>(handler.to.index() + 1) << 32 | hash;

    uint32_t added = static_cast<uint32_t>(m_landingPads.size());
    uint32_t next = noLandingPad;
    auto first = m_firstLandingPadsByKey.add(key, added);
    if (!first.isNewEntry) {
        for (uint32_t index = first.iterator->value; index != noLandingPad; index = m_landingPads[index].next) {
            const Edge& other = *m_landingPads[index].handler;
            if (other.to == handler.to && std::ranges::equal(other.arguments(), handler.arguments()))
                return index;
        }
        next = first.iterator->value;
        first.iterator->value = added;
    }
    m_landingPads.append(LandingPad { &handler, nullptr, 0, next, HandlerType::Catch });
    return added;
}

void ExceptionEmitter::findValuesCaughtByCleanups(Vector<BlockID, 8>& blocksThatThrowIntoHandlers)
{
    struct Caught {
        ValueID value;
        BlockID cleanup;
    };
    Vector<Caught, 4> worklist;
    forEachReachedBlock([&](BlockID id, const Block& block) {
        TerminatorKind kind = block.terminator.kind;
        if (block.handler && (kind == TerminatorKind::Rethrow || kind == TerminatorKind::Throw))
            blocksThatThrowIntoHandlers.append(id);
        if (!block.isHandlerBlock() || block.role == BlockRole::CatchHandler)
            return;
        for (ValueID value : m_code.valuesOf(id)) {
            if (opcodeOf(value) == Opcode::CatchValue)
                worklist.append({ value, id });
        }
    });
    if (worklist.isEmpty())
        return;

    struct PassedArgument {
        ValueID argument;
        ValueID parameter;
    };
    Vector<PassedArgument, 16> passedArguments;
    forEachReachedBlock([&](BlockID id, const Block&) {
        m_code.forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            std::span<const ValueID> arguments = edge.arguments();
            if (arguments.empty())
                return;
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            for (size_t i = 0; i < arguments.size() && i < parameters.size(); ++i) {
                ValueID definition = definitionOf(arguments[i]);
                if (!definition)
                    continue;
                Opcode opcode = opcodeOf(definition);
                if (opcode == Opcode::CatchValue || opcode == Opcode::Parameter)
                    passedArguments.append({ definition, parameters[i] });
            }
        });
    });
    AOT::sortInLine(passedArguments, { }, [](const PassedArgument& passed) { return passed.argument.index(); });

    m_cleanupsOfCaughtValues = m_proc.arena().createSpan<BlockID>(m_code.numberOfValues());
    while (!worklist.isEmpty()) {
        Caught caught = worklist.takeLast();
        BlockID& cleanup = m_cleanupsOfCaughtValues[caught.value.index()];
        bool wasCaught = m_valuesCaughtByCleanups.set(caught.value.index());
        if (wasCaught) {
            if (cleanup == caught.cleanup || !cleanup)
                continue;
            caught.cleanup = BlockID();
        }
        cleanup = caught.cleanup;
        auto first = std::ranges::lower_bound(passedArguments, caught.value.index(), { }, [](const PassedArgument& passed) { return passed.argument.index(); });
        for (; first != passedArguments.end() && first->argument == caught.value; ++first)
            worklist.append({ first->parameter, caught.cleanup });
    }
    findCleanupsThatKeepException();
}

void ExceptionEmitter::findCleanupsThatKeepException()
{
    if (functionSuspends())
        return;
    struct ThrowOfCleanup {
        BlockID cleanup;
        BlockID handler;
    };
    BitVector throwsOn;
    Vector<ThrowOfCleanup, 4> throws;
    forEachReachedBlock([&](BlockID id, const Block& block) {
        if (block.terminator.kind != TerminatorKind::Throw)
            return;
        BlockID cleanup = cleanupThatCaught(block.terminator.valueOperand());
        if (!cleanup || handlerTypeByOperations(cleanup) != HandlerType::Catch)
            return;
        throwsOn.set(cleanup.index());
        throws.append({ cleanup, handlerOf(id) });
    });

    Vector<BlockID, 4> worklist;
    Vector<ThrowOfCleanup, 4> throwsIntoCleanups;
    for (ThrowOfCleanup thrown : throws) {
        for (BlockID handler = thrown.handler; ; handler = handlerOf(handler)) {
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

bool ExceptionEmitter::isCaughtByCleanup(ValueID value) const
{
    return m_valuesCaughtByCleanups.get(definitionOf(value).index());
}

BlockID ExceptionEmitter::cleanupThatCaught(ValueID value) const
{
    ValueID definition = definitionOf(value);
    return m_valuesCaughtByCleanups.get(definition.index()) ? m_cleanupsOfCaughtValues[definition.index()] : BlockID();
}

HandlerType ExceptionEmitter::handlerTypeByOperations(BlockID handler) const
{
    HandlerType type = HandlerType::SynthesizedCatch;
    for (ValueID id : m_code.valuesOf(handler)) {
        Opcode opcode = opcodeOf(id);
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

void ExceptionEmitter::attachLandingPad(const OperationContext& context, Machine::PatchOperation* patch, uint32_t site, bool mayReturn)
{
    const Edge* handler = context.handler();
    if (!handler)
        return;

    LBasicBlock block = m_out.m_block;
    RELEASE_ASSERT(block->last() == patch && patch->generator());
    RELEASE_ASSERT(!mayReturn || !block->numSuccessors());
    const LandingPad& landingPad = landingPadFor(context.block(), *handler);
    LBasicBlock continuation = mayReturn ? newBlock() : nullptr;
    if (continuation)
        block->appendSuccessor(normalEdge(continuation));
    unsigned landingPadSuccessor = block->numSuccessors();
    block->appendSuccessor(rareEdge(landingPad.block));
    patch->clobberLateOnSuccessor(landingPadSuccessor, emitter<RuntimeCallEmitter>().registersClobberedByCalls());
    patch->effects.terminal = true;

    patch->setGenerator([records = &m_records.exceptions, siteGenerator = patch->generator(), site, landingPadNumber = landingPad.indexInRecords, landingPadSuccessor, type = landingPad.type, mayReturn](CCallHelpers& jit, const Machine::PatchParameters& params) {
        siteGenerator->run(jit, params);
        std::span<MacroAssembler::Label* const> labels = params.successorLabels();
        if (mayReturn && !params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), returnTarget = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*returnTarget, &jit);
            });
        }

        if (!records->landingPads[landingPadNumber]) {
            MacroAssembler::Label* headOfLandingPad = labels[landingPadSuccessor];
            if (isX86_64())
                records->landingPads[landingPadNumber] = headOfLandingPad;
            else {
                MacroAssembler::Label* unwindTarget = params.proc().arena().create<MacroAssembler::Label>();
                records->landingPads[landingPadNumber] = unwindTarget;
                params.addLatePath([unwindTarget, headOfLandingPad, bytesBelowFrameRecord = params.frame().bytesBelowFrameRecord()](CCallHelpers& jit) {
                    AllowMacroScratchRegisterUsage allowScratch(jit);
                    *unwindTarget = jit.label();
                    jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(bytesBelowFrameRecord)), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
                    jit.jump().linkTo(*headOfLandingPad, &jit);
                });
            }
        }
        records->sites.append(SiteLandingPad { site, landingPadNumber, type });
    });

    if (continuation)
        m_out.appendToContinuation(continuation);
}

ALWAYS_INLINE_INTO_OTHER_FILES const ExceptionEmitter::LandingPad& ExceptionEmitter::landingPadFor(BlockID block, const Edge& handler)
{
    uint32_t& indexPlusOne = m_landingPadsOfBlocks[block.index()];
    if (!indexPlusOne)
        indexPlusOne = findOrAddLandingPad(handler) + 1;
    LandingPad& landingPad = m_landingPads[indexPlusOne - 1];
    RELEASE_ASSERT(landingPad.handler->to == handler.to);
    if (landingPad.block)
        return landingPad;

    landingPad.block = newBlock();
    landingPad.indexInRecords = static_cast<uint32_t>(m_records.exceptions.landingPads.size());
    landingPad.type = handlerTypeOf(handler.to);
    m_records.exceptions.landingPads.append(nullptr);

    LBasicBlock siteBlock = m_out.m_block;
    m_out.appendTo(landingPad.block);
    if (landingPad.type == HandlerType::SynthesizedCatch)
        callWithoutSite<Boxed>(RuntimeOperation::Catch, { }, { .isOnRarePath = true, .followsThrow = true });
    if (generator().keepsValuesInHeapFrame()) [[unlikely]]
        emitter<SavedFrameEmitter>().passHandlerArgumentsInLandingPad(block, handler);
    else
        generator().passHandlerArguments(handler);
    if (isEnteredByJump(handler.to))
        passCaughtWord(handler.to, m_out.int64Zero);
    m_out.jump(generator().blockFor(handler.to));
    m_out.appendTo(siteBlock);
    return landingPad;
}

LValue ExceptionEmitter::caughtPhiOf(BlockID handler)
{
    LValue& phi = m_caughtPhis[handler.index()];
    if (!phi)
        phi = m_out.phiIn(generator().blockFor(handler), Machine::Int64);
    return phi;
}

void ExceptionEmitter::passCaughtWord(BlockID handler, LValue caught)
{
    m_out.upsilon(caught, caughtPhiOf(handler));
}

TypedPointer ExceptionEmitter::wordKeptBy(BlockID cleanup)
{
    Machine::StackSlot*& slot = m_keptWords[cleanup.index()];
    if (!slot)
        slot = m_proc.addStackSlot(sizeof(uint64_t));
    return m_out.address(m_heaps.frameSlots, m_out.slotBase(slot));
}

void ExceptionEmitter::keep(BlockID cleanup, LValue kept)
{
    RELEASE_ASSERT(!functionSuspends());
    m_out.store64(kept, wordKeptBy(cleanup));
}

LValue ExceptionEmitter::keptBy(ValueID thrown)
{
    BlockID cleanup = cleanupThatCaught(thrown);
    if (!cleanup || !keepsException(cleanup))
        return m_out.int64Zero;
    return m_out.load64(wordKeptBy(cleanup));
}

LValue ExceptionEmitter::caughtWord(const ValueContext& value)
{
    BlockID handler = value.block();
    RELEASE_ASSERT(m_code.at(handler).isHandlerBlock());
    if (LValue caught = m_caughtWords[handler.index()])
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
        Boxed passed(caughtPhiOf(handler));
        return selectByBranch<Boxed>(isNotEmpty(passed), [&] {
            return passed;
        }, takeFromRuntime);
    };
    LValue caught = (isEnteredByJump(handler) ? takePassedOrFromRuntime() : takeFromRuntime()).value();
    m_caughtWords[handler.index()] = caught;
    return caught;
}

Boxed ExceptionEmitter::valueOfException(CellPointer exception)
{
    Machine::AbstractHeap valueOfAnyException(m_heaps.entire(AbstractHeap::RuntimeState).index(), JSC::Exception::valueOffset(), AOTHeaps::Traits::Immutable);
    return Boxed(m_out.load64(exception.value(), valueOfAnyException));
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

void ExceptionEmitter::jumpToHandler(const Edge& handler, LValue caught)
{
    RELEASE_ASSERT(isEnteredByJump(handler.to));
    generator().passHandlerArguments(handler);
    passCaughtWord(handler.to, caught);
    m_out.jump(generator().blockFor(handler.to));
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
    const Edge* handler = terminator.handler();
    bool endsCleanup = isCaughtByCleanup(terminator->valueOperand());
    if (!handler) {
        if (endsCleanup)
            throwAfterCleanup(terminator, thrown);
        else
            throwByOperation(terminator, RuntimeOperation::Throw, thrown);
        return;
    }
    switch (handlerTypeOf(handler->to)) {
    case HandlerType::Catch:
    case HandlerType::SynthesizedCatch:
        jumpToHandler(*handler, thrown.value());
        return;
    case HandlerType::Finally:
        if (keepsException(handler->to)) {
            keep(handler->to, endsCleanup ? keptBy(terminator->valueOperand()) : callOperation<Integer>(terminator, RuntimeOperation::AddressOfThrow, { }, { .capturesStackTrace = true }).value());
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
    const Edge* handler = terminator.handler();
    if (!handler) {
        throwByOperation(terminator, RuntimeOperation::Throw, boxCell(exception));
        return;
    }
    switch (handlerTypeOf(handler->to)) {
    case HandlerType::Finally:
        if (keepsException(handler->to)) {
            keep(handler->to, exception.value());
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
