/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "B3ExtractValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "LocalAllocator.h"
#include "VMTraps.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static RegisterSet registersClobberedByCalls()
{
#if CPU(ARM64)
    return RegisterSet { callMarkerGPR };
#else
    RegisterSet result;
    for (GPRReg reg : stubTemporaryGPRs)
        result.add(reg, IgnoreVectors);
    result.add(callMarkerGPR, IgnoreVectors);
    return result;
#endif
}

void Lowering::didEmitWrite(Writes writes, bool writesShapeWords, bool convertsArrayStorage)
{
    if (writes == Writes::Fields && m_isStoringToLocalAllocation) {
        writes = Writes::RuntimeState;
        writesShapeWords = false;
    }
    if (writes <= m_function.writes && (!writesShapeWords || m_function.writesShapeWords) && (!convertsArrayStorage || m_function.convertsArrayStorage || m_function.writes == Writes::Heap))
        return;
    dataLogLn("AOT: f", m_id.index(), " has more effects (", static_cast<unsigned>(writes), writesShapeWords ? ", shape words" : "", convertsArrayStorage ? ", array storage" : "", ") than its summary (", static_cast<unsigned>(m_function.writes), m_function.writesShapeWords ? ", shape words" : "", m_function.convertsArrayStorage ? ", array storage" : "", ")");
    if (m_currentBlock && m_currentIndexInBlock < m_currentBlock->values.size())
        dataLogLn("AOT: while lowering this value, or the end of its block: ", nameOf(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).opcode));
    m_program.dump(WTF::dataFile(), m_id);
    CRASH();
}

void Lowering::applyEffects(PatchpointValue* patchpoint, OperationEffects effects)
{
    RELEASE_ASSERT_WITH_MESSAGE(!m_environmentAwaitingBarrier || effects.writes == Writes::Pure || m_isReportingFactViolation, "AOT: a garbage collection may happen between a store to an environment and its barrier");
    RELEASE_ASSERT_WITH_MESSAGE(!m_isFillingArrayAtCreation || effects.writes == Writes::Pure || m_isReportingFactViolation, "AOT: a garbage collection may happen between the allocation of an array and the store of its last initial element");
    if (m_isFillingReusedEnvironment && effects.writes != Writes::Pure && !m_isReportingFactViolation) [[unlikely]] {
        dataLogLn("AOT: f", m_id.index(), ": a garbage collection may happen between the allocation of a reused environment and its barrier");
        CRASH();
    }
    if (!m_fieldsAwaitingFirstStore.isEmpty() && effects.writes != Writes::Pure && !effects.writesOnlyTypedArrayProperties && !m_isReportingFactViolation) [[unlikely]] {
        BitVector others = m_fieldsAwaitingFirstStore;
        if (isLoweringInitializingStore()) {
            if (auto field = fieldInitializedBy(m_fieldsAwaitingFirstStoreLayout, m_function.at(m_currentBlock->values[m_currentIndexInBlock])))
                others.clear(*field);
        }
        RELEASE_ASSERT_WITH_MESSAGE(others.isEmpty(), "AOT: a garbage collection may happen between an allocation and the first store to an uninitialized field");
    }
    bool writesShapeWords = effects.writesShapeWords && m_program.castsMayRetag;
    didEmitWrite(effects.writes, writesShapeWords, effects.convertsArrayStorage);
    bool isTerminal = patchpoint->effects.terminal;
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = isTerminal;
    if (effects.writes == Writes::Pure && !effects.convertsArrayStorage)
        return;
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->effects.exitsSideways = effects.exitsSideways;
    patchpoint->effects.controlDependent = true;
    const AbstractHeap* first = &m_heaps.AOTTyped_collectorState;
    const AbstractHeap* last = &m_heaps.AOTTypedObject_orderWord;
    switch (effects.writes) {
    case Writes::Pure:
    case Writes::Nothing:
        last = first;
        break;
    case Writes::RuntimeState:
        last = &m_heaps.AOTTyped_runtimeState;
        break;
    case Writes::TypedArrayProperties:
        first = &m_heaps.TypedArrayProperties;
        last = effects.writesOnlyTypedArrayProperties ? first : &m_heaps.AOTTyped_runtimeState;
        break;
    case Writes::Fields:
        if (!effects.writesOnlyOrderWords)
            first = &m_heaps.AOTTyped_fields;
        break;
    case Writes::Heap:
        first = &m_heaps.root;
        last = &m_heaps.JSCellHeaderAndNamedProperties;
        break;
    }
    if (effects.convertsArrayStorage) {
        first = &m_heaps.root;
        last = &m_heaps.JSCellHeaderAndNamedProperties;
    }
    if (writesShapeWords)
        last = &m_heaps.AOTTypedObject_shapeWord;
    m_heaps.decoratePatchpointWrite(first, last, patchpoint);
}

static_assert(static_cast<unsigned>(Stub::ColdOperationVoid) + 1 == static_cast<unsigned>(Stub::LeafColdOperationVoid) && static_cast<unsigned>(Stub::ColdOperationValue) + 1 == static_cast<unsigned>(Stub::LeafColdOperationValue)
    && static_cast<unsigned>(Stub::ColdOperationDouble) + 1 == static_cast<unsigned>(Stub::LeafColdOperationDouble) && static_cast<unsigned>(Stub::ColdPlainOperation) + 1 == static_cast<unsigned>(Stub::LeafColdPlainOperation)
    && static_cast<unsigned>(Stub::ColdPlainOperationWithoutInstance) + 1 == static_cast<unsigned>(Stub::LeafColdPlainOperationWithoutInstance) && static_cast<unsigned>(Stub::ColdPlainOperationDouble) + 1 == static_cast<unsigned>(Stub::LeafColdPlainOperationDouble)
    && static_cast<unsigned>(Stub::ColdPlainOperationDoubleWithoutInstance) + 1 == static_cast<unsigned>(Stub::LeafColdPlainOperationDoubleWithoutInstance));
static_assert(static_cast<unsigned>(Stub::ColdOperationVoid) + 2 == static_cast<unsigned>(Stub::ColdOperationVoidWithoutFPRs) && static_cast<unsigned>(Stub::ColdOperationVoid) + 3 == static_cast<unsigned>(Stub::LeafColdOperationVoidWithoutFPRs)
    && static_cast<unsigned>(Stub::ColdOperationValue) + 2 == static_cast<unsigned>(Stub::ColdOperationValueWithoutFPRs) && static_cast<unsigned>(Stub::ColdOperationValue) + 3 == static_cast<unsigned>(Stub::LeafColdOperationValueWithoutFPRs)
    && static_cast<unsigned>(Stub::ColdPlainOperation) + 2 == static_cast<unsigned>(Stub::ColdPlainOperationWithoutFPRs) && static_cast<unsigned>(Stub::ColdPlainOperation) + 3 == static_cast<unsigned>(Stub::LeafColdPlainOperationWithoutFPRs));

Vector<uint32_t, 3> throwSiteWords(ThrowKind kind, Rep rep, unsigned registerNumber, uint32_t a, std::optional<uint32_t> b)
{
    bool hasWideA = a > ThrowSite::maxNarrowA;
    Vector<uint32_t, 3> words { static_cast<uint32_t>(kind) | static_cast<uint32_t>(rep) << ThrowSite::repShift | registerNumber << ThrowSite::registerShift | (hasWideA ? ThrowSite::hasWideA : a << ThrowSite::aShift) | (b ? ThrowSite::hasB : 0) };
    if (hasWideA)
        words.append(a);
    if (b)
        words.append(*b);
    return words;
}

void Lowering::unwindToLandingPad(PatchpointValue* patchpoint, CallSite site, RegistersAtLandingPad registers, bool mayReturn)
{
    if (!m_activeHandler)
        return;
    if (!m_currentLandingPad)
        m_currentLandingPad = landingPadFor(*m_activeHandler);
    RELEASE_ASSERT(m_callSitesAwaitingLandingPad);
    --m_callSitesAwaitingLandingPad;
    m_context.emitsCalls = true;
    m_context.unwindsToLandingPads = true;
    patchpoint->effects.terminal = true;
    if (registers == RegistersAtLandingPad::Clobbered) {
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
        patchpoint->clobberLate(registersClobberedByCalls());
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
    }
    uint32_t number = CallSiteIndex(PackedSite::bits(site.bits)).bytecodeIndex().offset();
    if (exceptionCaughtIn(m_activeHandler->block))
        number |= FunctionReportableSites::handlerTakesException;
    else if (ignoresCaughtValue(m_activeHandler->block))
        number |= FunctionReportableSites::handlerIgnoresValue;
    patchpoint->setGenerator([context = &m_context, generator = patchpoint->generator(), number, mayReturn](CCallHelpers& jit, const StackmapGenerationParams& params) {
        generator->run(jit, params);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        if (mayReturn && !params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), continuation = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*continuation, &jit);
            });
        }
        Box<CCallHelpers::Label> handler = labels.last();
        auto added = context->landingPadsByHandler.add(handler.get(), context->landingPads.size());
        if (added.isNewEntry) {
            Box<CCallHelpers::Label> landingPad = Box<CCallHelpers::Label>::create();
            context->landingPads.append(landingPad);
            params.addLatePath([landingPad, handler, bytesBelowFrameRecord = params.code().bytesBelowFrameRecord()](CCallHelpers& jit) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                *landingPad = jit.label();
                jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(bytesBelowFrameRecord)), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
                jit.jump().linkTo(*handler, &jit);
            });
        }
        context->landingPadsForCallSites.append({ number, added.iterator->value });
    });
    LBasicBlock continuation = m_out.newBlock();
    if (mayReturn)
        m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(m_currentLandingPad));
    m_out.appendTo(continuation);
}

void Lowering::branchToLandingPad(PatchpointValue* patchpoint, std::optional<GPRReg> exception)
{
    if (!m_currentLandingPad)
        m_currentLandingPad = landingPadFor(*m_activeHandler);
    RELEASE_ASSERT(m_callSitesAwaitingLandingPad);
    --m_callSitesAwaitingLandingPad;
    m_context.emitsCalls = true;
    m_context.unwindsToLandingPads = true;
    patchpoint->effects.terminal = true;
    patchpoint->setGenerator([context = &m_context, generator = patchpoint->generator(), exception](CCallHelpers& jit, const StackmapGenerationParams& params) {
        RELEASE_ASSERT(!context->hasNoFrame(params.proc().code()));
        generator->run(jit, params);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([jump = exception ? jit.branchTestPtr(CCallHelpers::NonZero, *exception) : jit.jump(), landingPad = labels.last()](CCallHelpers& jit) {
            jump.linkTo(*landingPad, &jit);
        });
        if (exception && !params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), continuation = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*continuation, &jit);
            });
        }
    });
    LBasicBlock continuation = m_out.newBlock();
    if (exception)
        m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(m_currentLandingPad));
    m_out.appendTo(continuation);
}

static bool callThrowAtSite(LoweringContext& context, CCallHelpers& jit, bool isLeaf, CallSite site, bool returnsToSite, const Vector<uint32_t, 3>& words, bool valueIsInWordRegister, bool isThrowStatement)
{
#if CPU(X86_64) && OS(LINUX)
    static_assert(throwWordGPR == X86Registers::r11);
    if (!returnsToSite && !valueIsInWordRegister && !(isLeaf && isThrowStatement)) {
        context.throwSiteTraps.append({ jit.labelIgnoringWatchpoints(), site.bits });
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(isLeaf ? trapInstructionWithoutFrame : trapInstruction));
        return false;
    }
#else
    UNUSED_PARAM(isThrowStatement);
#endif
    if (isLeaf && !hasFramelessFunctionStubs) {
        jit.emitFunctionPrologue();
        isLeaf = false;
    }
    if (!returnsToSite && words.size() == 1 && !valueIsInWordRegister) {
        ArgumentMoves moves;
        static const bool wordIsWrongForTesting = Options::aotSlowPathFaultForTesting() && !strcmp(byteCast<char>(Options::aotSlowPathFaultForTesting()), "word");
        appendArgumentMove(moves, throwWordGPR, ArgumentSource::Constant, words[0] + wordIsWrongForTesting);
        context.stubCalls.call(jit, isLeaf ? Stub::LeafThrowAtSiteByWord : Stub::ThrowAtSiteByWord, site, moves);
        return true;
    }
    context.stubCalls.call(jit, isLeaf ? Stub::LeafThrowAtSite : Stub::ThrowAtSite, site);
    return false;
}

uint32_t Lowering::throwAt(ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b, unsigned newErrorClassPlusOne)
{
    RELEASE_ASSERT(!newErrorClassPlusOne || (!operand && newErrorClassPlusOne < ThrowSite::numberOfRegistersPerBank));
    if (operand && operand->rep == Rep::JSValue && operand->value->hasInt64() && operand->value->asInt64() == JSValue::encode(jsUndefined()))
        operand = std::nullopt;
    if (a == UINT32_MAX)
        a = ThrowSite::none;
    if (b == UINT32_MAX)
        b = std::nullopt;
    RELEASE_ASSERT(!(a >> 31) && !(b.value_or(0) >> 31));
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, OperationEffects { Writes::Nothing });
    Rep rep = operand ? operand->rep : Rep::Void;
    if (operand)
        patchpoint->append(ConstrainedValue(operand->value, ValueRep::SomeRegister));
    patchpoint->clobber(*registersClobberedBy(Stub::ColdOperationVoid));
    if (LoweringContext::coldSitesForceFrame() || (!hasFramelessFunctionStubs && m_activeHandler)) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasFramelessFunctionStubs)
        m_context.hasColdSites = true;
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, kind, rep, a, b, site, newErrorClassPlusOne](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        unsigned number = rep == Rep::Void ? newErrorClassPlusOne : rep == Rep::Double || rep == Rep::OptionalDouble ? static_cast<unsigned>(params[0].fpr()) : static_cast<unsigned>(params[0].gpr());
        auto words = throwSiteWords(kind, rep, number, a, b);
        if (callThrowAtSite(*context, jit, isLeaf, site, false, words, rep != Rep::Void && params[0].isGPR() && params[0].gpr() == throwWordGPR, !!newErrorClassPlusOne))
            return;
        context->recordDataInCode(jit, words.size());
        for (uint32_t word : words)
            jit.m_assembler.buffer().putInt(word);
    });
    unwindToLandingPad(patchpoint, site, RegistersAtLandingPad::PreservedFromSite, false);
    return site.bits;
}

void Lowering::requireOrThrowAt(Word32 condition, ThrowKind kind, std::optional<Operand> operand, uint32_t a)
{
    if (m_isInLoop || m_activeHandler || m_environmentAwaitingBarrier || m_isFillingArrayAtCreation || !m_fieldsAwaitingFirstStore.isEmpty()) {
        require(condition, [&] {
            throwAt(kind, operand, a);
        });
        return;
    }
    if (operand && operand->rep == Rep::JSValue && operand->value->hasInt64() && operand->value->asInt64() == JSValue::encode(jsUndefined()))
        operand = std::nullopt;
    if (a == UINT32_MAX)
        a = ThrowSite::none;
    RELEASE_ASSERT(!(a >> 31));
    didEmitWrite(Writes::Nothing);
    B3::CheckValue* check = m_out.speculate(logicalNot(condition).value());
    Rep rep = operand ? operand->rep : Rep::Void;
    if (operand)
        check->append(ConstrainedValue(operand->value, ValueRep::SomeRegister));
#if CPU(ARM64)
    if (operand && hasFramelessFunctionStubs)
        check->clobberEarly(RegisterSet { stubTemporaryGPRs[1] });
#endif
    if (LoweringContext::coldSitesForceFrame()) {
        m_context.emitsCalls = true;
        check->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasFramelessFunctionStubs)
        m_context.hasColdSites = true;
    CallSite site { nextSite() };
    check->setGenerator([context = &m_context, kind, rep, a, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        unsigned number = rep == Rep::Void ? 0 : rep == Rep::Double || rep == Rep::OptionalDouble ? static_cast<unsigned>(params[0].fpr()) : static_cast<unsigned>(params[0].gpr());
        auto words = throwSiteWords(kind, rep, number, a, std::nullopt);
        if (callThrowAtSite(*context, jit, isLeaf, site, false, words, rep != Rep::Void && params[0].isGPR() && params[0].gpr() == throwWordGPR, false))
            return;
        context->recordDataInCode(jit, words.size());
        for (uint32_t word : words)
            jit.m_assembler.buffer().putInt(word);
    });
}

static bool constantsVaryBySite(Entry);

bool Lowering::canCallOnLatePath(Entry entry, bool mayThrow, const Vector<LValue, 8>& arguments) const
{
    if (m_isInLoop || m_activeHandler || m_environmentAwaitingBarrier || m_isFillingArrayAtCreation || !m_fieldsAwaitingFirstStore.isEmpty())
        return false;
    bool withInstance = hasInstanceParameter(entry);
    if (!withInstance && mayThrow)
        return false;
    for (LValue argument : arguments) {
        if (argument->type() == Double)
            return false;
    }
    return withInstance + arguments.size() <= GPRInfo::numberOfArgumentRegisters;
}

LValue Lowering::callOnLatePath(Entry entry, bool mayThrow, OperationEffects effects, const Vector<LValue, 8>& arguments, const Vector<LValue, 4>& checked, Ref<FastPathChecks>&& checks, LValue fastValue, std::optional<unsigned> firstAccessedIndex)
{
    RELEASE_ASSERT(canCallOnLatePath(entry, mayThrow, arguments));
    bool withInstance = hasInstanceParameter(entry);
    bool returnsValue = !!fastValue;
    PatchpointValue* patchpoint = m_out.patchpoint(returnsValue ? Int64 : Void);
    applyEffects(patchpoint, effects);
    if (returnsValue) {
        RELEASE_ASSERT(fastValue->type() == Int64);
        patchpoint->append(ConstrainedValue(fastValue, ValueRep::ColdAny));
        patchpoint->resultConstraints = { ValueRep::SomeRegisterWithClobber };
    }
    for (LValue value : checked)
        patchpoint->append(ConstrainedValue(value, ValueRep::SomeRegister));
    for (LValue argument : arguments)
        patchpoint->append(ConstrainedValue(argument->hasInt32() ? m_out.constInt64(static_cast<uint32_t>(argument->asInt32())) : argument, ValueRep::ColdAny));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    if (LoweringContext::coldSitesForceFrame()) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasFramelessFunctionStubs)
        m_context.hasColdSites = true;
    CallSite site;
    if (mayThrow)
        site.bits = nextSite();
    uint32_t immediate = static_cast<uint32_t>(static_cast<unsigned>(entry) * sizeof(void*));
    Box<LoweringContext::LatePathCallAccess> firstAccess;
    if (firstAccessedIndex)
        firstAccess = Box<LoweringContext::LatePathCallAccess>::create(LoweringContext::LatePathCallAccess { returnsValue + *firstAccessedIndex });
    patchpoint->setGenerator([context = &m_context, checks = WTF::move(checks), firstAccess, firstAccessedIndex, mayThrow, withInstance, returnsValue, immediate, site, checkedCount = checked.size(), numberOfArguments = arguments.size(), usesInlineConstants = constantsVaryBySite(entry)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg result = returnsValue ? params[0].gpr() : InvalidGPRReg;
        unsigned firstCheckedIndex = params.size() - numberOfArguments - checkedCount;
        unsigned firstArgumentIndex = params.size() - numberOfArguments;
        Vector<GPRReg, 4> checkedRegisters;
        for (unsigned i = 0; i < checkedCount; ++i)
            checkedRegisters.append(params[firstCheckedIndex + i].gpr());
        CCallHelpers::Label firstInstruction = jit.labelIgnoringWatchpoints();
        CCallHelpers::JumpList slowCases = checks->run(jit, result, checkedRegisters.span());
        if (firstAccess && firstAccess->nullishTest != std::numeric_limits<uint32_t>::max())
            context->faultingAccesses.append({ firstInstruction, checkedRegisters[*firstAccessedIndex], firstAccess->nullishTest });
        CCallHelpers::Label done = jit.label();
        bool isLeaf = context->hasNoFrame(params.proc().code());
        bool createsFrame = isLeaf && !hasFramelessFunctionStubs;
        Stub stub = mayThrow ? (returnsValue ? Stub::ColdOperationValue : Stub::ColdOperationVoid) : withInstance ? Stub::ColdPlainOperation : Stub::ColdPlainOperationWithoutInstance;
        if (withInstance) {
            bool holdsFPR = false;
            RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
                holdsFPR |= reg.isFPR();
            });
            if (!holdsFPR)
                stub = static_cast<Stub>(static_cast<unsigned>(stub) + 2);
        }
        if (isLeaf && !createsFrame)
            stub = static_cast<Stub>(static_cast<unsigned>(stub) + 1);
        ArgumentMoves moves;
        Vector<std::pair<GPRReg, int64_t>, 4> inlineConstants;
        RegisterSet clobbered = *registersClobberedBy(Stub::ColdOperationValue);
        if (returnsValue || !mayThrow)
            clobbered.add(GPRInfo::returnValueGPR, IgnoreVectors);
        RegisterSet argumentRegisters;
        for (unsigned i = 0; i < numberOfArguments; ++i) {
            if (const ValueRep& rep = params[firstArgumentIndex + i]; rep.isGPR())
                argumentRegisters.add(rep.gpr(), IgnoreVectors);
        }
        for (unsigned i = 0; i < numberOfArguments; ++i) {
            GPRReg destination = operationArgumentGPR(withInstance + i);
            const ValueRep& rep = params[firstArgumentIndex + i];
            if (rep.isGPR()) {
                if (rep.gpr() != destination) {
                    appendArgumentMove(moves, destination, ArgumentSource::Register, rep.gpr());
                    clobbered.add(destination, IgnoreVectors);
                }
                continue;
            }
            clobbered.add(destination, IgnoreVectors);
            if (rep.isConstant()) {
                if (usesInlineConstants && !argumentRegisters.contains(destination, IgnoreVectors))
                    inlineConstants.append({ destination, rep.value() });
                else
                    appendArgumentMove(moves, destination, ArgumentSource::Constant, rep.value());
                continue;
            }
            RELEASE_ASSERT(rep.isStack() && !isLeaf);
            appendArgumentMove(moves, destination, params.value()->child(params.value()->numChildren() - numberOfArguments + i)->type() == Int32 ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
        }
        Vector<GPRReg, 8> saved;
        clobbered.forEach([&](Reg reg) {
            if (reg.isGPR() && reg.gpr() != result && params.usedRegisters().contains(reg, IgnoreVectors) && !RegisterSet::macroClobberedGPRs().contains(reg, IgnoreVectors))
                saved.append(reg.gpr());
        });
        if (saved.size() & 1)
            saved.append(jit.scratchRegister());
        params.addNearbyLatePath(jit, [=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            slowCases.link(&jit);
            if (createsFrame)
                jit.emitFunctionPrologue();
            for (unsigned i = 0; i < saved.size(); i += 2)
                jit.pushPair(saved[i], saved[i + 1]);
#if CPU(ARM64)
            if (isLeaf)
                jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
            for (auto& [destination, constant] : inlineConstants)
                jit.move(CCallHelpers::TrustedImm64(constant), destination);
            if (moves.isEmpty())
                context->stubCalls.call(jit, stub, immediate, site);
            else
                context->stubCalls.call(jit, stub, immediate, site, moves);
            if (returnsValue && result != GPRInfo::returnValueGPR)
                jit.move(GPRInfo::returnValueGPR, result);
            for (unsigned i = saved.size(); i; i -= 2)
                jit.popPair(saved[i - 2], saved[i - 1]);
            if (createsFrame)
                jit.emitFunctionEpilogueWithEmptyFrame();
            jit.jump().linkTo(done, &jit);
        });
    });
    if (firstAccess)
        m_context.latePathCallAccesses.add(patchpoint->generator(), firstAccess);
    return patchpoint;
}

static bool constantsVaryBySite(Entry entry)
{
    switch (entry) {
    case Entry::operationAOTTypedSetByName:
    case Entry::operationAOTTypedSetByNameSloppy:
    case Entry::operationAOTTypedThrowIfReadOnly:
    case Entry::operationAOTTypedDefineByName:
    case Entry::operationAOTTypedNewArrayBufferFromData:
    case Entry::operationAOTTypedEnsureStorage:
    case Entry::operationAOTTypedThrowUnlessCallable:
    case Entry::operationAOTTypedFactViolated:
        return true;
    default:
        return false;
    }
}

LValue Lowering::callEntry(Entry entry, LType type, bool mayThrow, OperationEffects effects, const Vector<LValue, 8>& arguments)
{
    bool withInstance = hasInstanceParameter(entry);
    unsigned numberOfGPRs = withInstance;
    for (LValue argument : arguments)
        numberOfGPRs += argument->type() != Double;
    bool isSlowPathCall = m_out.m_block->frequency() < 1 && (withInstance || !mayThrow) && numberOfGPRs <= GPRInfo::numberOfArgumentRegisters;
    bool takesArgumentsInCurrentRegisters = isSlowPathCall;
    Vector<LValue, 8> children = arguments;
    if (takesArgumentsInCurrentRegisters) {
        for (LValue& child : children) {
            if (child->hasInt32())
                child = m_out.constInt64(static_cast<uint32_t>(child->asInt32()));
        }
    }
    LValue checkedValue = isSlowPathCall && type == Int64 && mayThrow ? m_valueCheckedByColdOperation : nullptr;
    ArgumentMoves checkedValueTests;
    if (checkedValue)
        checkedValueTests = *m_coldOperationTypeTests;
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    {
        SetForScope isReporting(m_isReportingFactViolation, entry == Entry::operationAOTTypedFactViolated);
        applyEffects(patchpoint, effects);
    }
    unsigned nextGPR = withInstance;
    unsigned nextFPR = 0;
    for (LValue argument : children) {
        if (argument->type() == Double) {
            RELEASE_ASSERT(nextFPR < FPRInfo::numberOfArgumentRegisters);
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++))));
            continue;
        }
        RELEASE_ASSERT(nextGPR < numberOfOperationArgumentGPRs);
        if (takesArgumentsInCurrentRegisters) {
            patchpoint->append(ConstrainedValue(argument, argument == checkedValue ? ValueRep::reg(GPRInfo::returnValueGPR) : ValueRep::ColdAny));
            patchpoint->clobberLate(RegisterSet { operationArgumentGPR(nextGPR++) });
            continue;
        }
        patchpoint->append(ConstrainedValue(argument, ValueRep::reg(operationArgumentGPR(nextGPR++))));
    }
    if (effects.writes == Writes::Pure && !mayThrow && type != Void && arguments.size() <= 3)
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::FirstOperation) + static_cast<int32_t>(entry);
    CallSite site;
    if (mayThrow)
        site.bits = nextSite();
    uint32_t immediate = static_cast<uint32_t>(static_cast<unsigned>(entry) * sizeof(void*));
    if (isSlowPathCall) {
        bool returnsDouble = type == Double;
        bool returnsValue = type != Void && !returnsDouble;
        bool stubChecksException = mayThrow;
        patchpoint->clobber(*registersClobberedBy(Stub::ColdOperationValue));
        if (checkedValue)
            patchpoint->clobber(*registersClobberedBy(Stub::CoerceToType));
        if (!stubChecksException && type == Void)
            patchpoint->clobberLate(RegisterSet { GPRInfo::returnValueGPR });
        if (LoweringContext::coldSitesForceFrame() || (!hasFramelessFunctionStubs && m_activeHandler)) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else if (!hasFramelessFunctionStubs)
            m_context.hasColdSites = true;
        if (returnsValue)
            patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        else if (returnsDouble)
            patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
        static_assert(static_cast<unsigned>(Stub::ArrayElementAs) + 1 == static_cast<unsigned>(Stub::LeafArrayElementAs));
        static_assert(static_cast<unsigned>(Stub::ArrayElement) + 1 == static_cast<unsigned>(Stub::LeafArrayElement));
        std::optional<Stub> elementStub;
        if (m_usesElementStub && stubChecksException && takesArgumentsInCurrentRegisters) {
            if (entry == Entry::operationAOTTypedArrayElementAs)
                elementStub = Stub::ArrayElementAs;
            else if (entry == Entry::operationAOTTypedArrayGetAtIndex)
                elementStub = Stub::ArrayElement;
        }
        static_assert(static_cast<unsigned>(Stub::AbsentFieldOfFlaggedObject) + 1 == static_cast<unsigned>(Stub::LeafAbsentFieldOfFlaggedObject));
        static_assert(static_cast<unsigned>(Stub::AbsentFieldOfFlaggedObjectChecked) + 1 == static_cast<unsigned>(Stub::LeafAbsentFieldOfFlaggedObjectChecked));
        static_assert(static_cast<unsigned>(Stub::AbsentFieldOfFlaggedObjectAsNumber) + 1 == static_cast<unsigned>(Stub::LeafAbsentFieldOfFlaggedObjectAsNumber));
        if (m_queriesExtraPropertiesForAbsentField && stubChecksException && takesArgumentsInCurrentRegisters) {
            if (entry == Entry::operationAOTTypedGetByName)
                elementStub = Stub::AbsentFieldOfFlaggedObject;
            else if (entry == Entry::operationAOTTypedGetByNameChecked)
                elementStub = Stub::AbsentFieldOfFlaggedObjectChecked;
            else if (entry == Entry::operationAOTTypedGetByNameCheckedAsNumber)
                elementStub = Stub::AbsentFieldOfFlaggedObjectAsNumber;
        }
        patchpoint->setGenerator([context = &m_context, stubChecksException, elementStub, withInstance, returnsValue, returnsDouble, immediate, site, takesArgumentsInCurrentRegisters, checkedValueTests = WTF::move(checkedValueTests), materializesConstantsAtSite = constantsVaryBySite(entry) || checkedValue](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
            if (isLeaf)
                jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
            Stub stub;
            if (stubChecksException)
                stub = returnsDouble ? Stub::ColdOperationDouble : returnsValue ? Stub::ColdOperationValue : Stub::ColdOperationVoid;
            else if (returnsDouble)
                stub = withInstance ? Stub::ColdPlainOperationDouble : Stub::ColdPlainOperationDoubleWithoutInstance;
            else
                stub = withInstance ? Stub::ColdPlainOperation : Stub::ColdPlainOperationWithoutInstance;
            if (elementStub)
                stub = *elementStub;
            else if (withInstance && !returnsDouble) {
                bool holdsFPR = false;
                RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
                    holdsFPR |= reg.isFPR();
                });
                if (!holdsFPR)
                    stub = static_cast<Stub>(static_cast<unsigned>(stub) + 2);
            }
            bool createsFrame = isLeaf && !hasFramelessFunctionStubs;
            if (isLeaf && !createsFrame)
                stub = static_cast<Stub>(static_cast<unsigned>(stub) + 1);
            ArgumentMoves moves;
            if (!isLeaf)
                moves = checkedValueTests;
            unsigned numberOfChildren = takesArgumentsInCurrentRegisters ? params.value()->numChildren() : 0;
            unsigned destinationIndex = withInstance;
            RegisterSet holdsArgument;
            for (unsigned i = 0; i < numberOfChildren; ++i) {
                if (const ValueRep& rep = params[params.size() - numberOfChildren + i]; rep.isGPR())
                    holdsArgument.add(rep.gpr(), IgnoreVectors);
            }
            for (unsigned i = 0; i < numberOfChildren; ++i) {
                B3::Type childType = params.value()->child(i)->type();
                if (childType == Double)
                    continue;
                GPRReg destination = operationArgumentGPR(destinationIndex++);
                const ValueRep& rep = params[params.size() - numberOfChildren + i];
                if (rep.isGPR()) {
                    if (rep.gpr() != destination)
                        appendArgumentMove(moves, destination, ArgumentSource::Register, rep.gpr());
                    continue;
                }
                if (rep.isConstant()) {
                    if (materializesConstantsAtSite && !holdsArgument.contains(destination, IgnoreVectors))
                        jit.move(CCallHelpers::TrustedImm64(rep.value()), destination);
                    else
                        appendArgumentMove(moves, destination, ArgumentSource::Constant, rep.value());
                    continue;
                }
                RELEASE_ASSERT(rep.isStack() && !isLeaf);
                appendArgumentMove(moves, destination, childType == Int32 ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
            }
            if (createsFrame)
                jit.emitFunctionPrologue();
            if (elementStub && moves.isEmpty())
                context->stubCalls.call(jit, stub, site);
            else if (elementStub)
                context->stubCalls.call(jit, stub, site, moves);
            else if (moves.isEmpty())
                context->stubCalls.call(jit, stub, immediate, site);
            else
                context->stubCalls.call(jit, stub, immediate, site, moves);
            if (createsFrame)
                jit.emitFunctionEpilogueWithEmptyFrame();
        });
        if (mayThrow)
            unwindToLandingPad(patchpoint, site, RegistersAtLandingPad::PreservedFromSite);
        return patchpoint;
    }
    m_context.emitsCalls = true;
    bool branchesToLandingPad = false;
    Stub stub;
    if (!mayThrow)
        stub = withInstance ? Stub::PlainOperationWithInstance : Stub::PlainOperation;
    else if (branchesToLandingPad)
        stub = type == Double ? Stub::UncheckedOperationDoubleWithInstance : Stub::UncheckedOperationWithInstance;
    else if (type == Double)
        stub = withInstance ? Stub::OperationDoubleWithInstance : Stub::OperationDouble;
    else if (type == Void)
        stub = withInstance ? Stub::OperationVoidWithInstance : Stub::OperationVoid;
    else
        stub = withInstance ? Stub::OperationValueWithInstance : Stub::OperationValue;
    if (entry == Entry::operationAOTTypedStringReplaceRegExpString && stub == Stub::OperationValueWithInstance)
        stub = Stub::StringReplaceRegExpStringMayNotMatch;
    if (entry == Entry::operationAOTTypedRegExpExec && stub == Stub::OperationValueWithInstance)
        stub = Stub::RegExpExecWithFastPath;
    if (entry == Entry::operationAOTTypedStringMatchRegExp && stub == Stub::OperationValueWithInstance)
        stub = Stub::StringMatchRegExpWithFastPath;
    if (entry == Entry::operationAOTTypedNewRegExpLiteral && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::NewRegExpLiteral : Stub::UncheckedNewRegExpLiteral;
    if (entry == Entry::operationAOTTypedRegExpTest && stub == Stub::OperationValueWithInstance)
        stub = Stub::RegExpTestWithFastPath;
    if (entry == Entry::operationAOTTypedRegExpFind && stub == Stub::OperationValueWithInstance)
        stub = Stub::RegExpFindWithFastPath;
    if (entry == Entry::operationAOTTypedStringToLowerCase && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::StringToLowerCase : Stub::UncheckedStringToLowerCase;
    if (entry == Entry::operationAOTTypedStringToUpperCase && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::StringToUpperCase : Stub::UncheckedStringToUpperCase;
    if (entry == Entry::operationAOTTypedNewTypedArray && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::NewTypedArray : Stub::UncheckedNewTypedArray;
    if (entry == Entry::operationAOTTypedNewMap && stub == Stub::PlainOperationWithInstance)
        stub = Stub::NewMap;
    if (entry == Entry::operationAOTTypedStringToNumber8 && (stub == Stub::OperationDoubleWithInstance || stub == Stub::PlainOperationWithInstance))
        stub = Stub::StringToNumber8;
    if (entry == Entry::operationAOTTypedParseInt8 && (stub == Stub::OperationDoubleWithInstance || stub == Stub::PlainOperationWithInstance))
        stub = Stub::ParseInt8;
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    patchpoint->clobberLate(registersClobberedByCalls());
#endif
    if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, immediate, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (stub == Stub::NewRegExpLiteral || stub == Stub::UncheckedNewRegExpLiteral || stub == Stub::StringReplaceRegExpStringMayNotMatch || stub == Stub::RegExpExecWithFastPath || stub == Stub::StringMatchRegExpWithFastPath || stub == Stub::RegExpTestWithFastPath || stub == Stub::RegExpFindWithFastPath || stub == Stub::StringToLowerCase || stub == Stub::StringToUpperCase || stub == Stub::UncheckedStringToLowerCase || stub == Stub::UncheckedStringToUpperCase || stub == Stub::NewTypedArray || stub == Stub::UncheckedNewTypedArray || stub == Stub::NewMap || stub == Stub::StringToNumber8 || stub == Stub::ParseInt8)
            stubCalls->call(jit, stub, site);
        else
            stubCalls->call(jit, stub, immediate, site);
    });
    if (branchesToLandingPad)
        branchToLandingPad(patchpoint, type == Void || type == Double ? GPRInfo::returnValueGPR : GPRInfo::returnValueGPR2);
    else if (mayThrow)
        unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

uint32_t Lowering::calleeIndex(FunctionID function)
{
    return m_context.calleeIndices.ensure(function.index(), [&] {
        m_context.knownCallees.append(keyOf(function));
        return m_context.knownCallees.size() - 1;
    }).iterator->value;
}

bool Lowering::tailCallDropsMachineFrame(const Block& block, const Value& call) const
{
    if (!call.isInTailPosition || m_isEntry || (m_function.isGenericEntrypoint() && !call.inlineFrame) || m_function.isSharedOutlinedBody || !m_function.resultTypes.isEmpty() || block.terminator.kind == TerminatorKind::Invoke)
        return false;
    for (uint32_t frame = call.inlineFrame; frame; frame = m_function.inlineFrames[frame].parent) {
        auto& inlinedCall = m_function.inlineFrames[frame];
        if (inlinedCall.isTailCall)
            continue;
        if (!m_function.isGenericEntrypoint() || inlinedCall.parent)
            return false;
        auto entry = m_program.genericEntrypoints.find(inlinedCall.callee.index());
        if (entry == m_program.genericEntrypoints.end() || entry->value != m_id)
            return false;
    }
    return true;
}

bool Lowering::isWidenedInIndirectTailCallCycle() const
{
    auto& function = m_program.at(m_id);
    return function.isInIndirectTailCallCycle && !function.isCalledFromDroppableFrame;
}

bool Lowering::mayJumpToWidenedFunction(FunctionID function) const
{
    auto& callee = m_program.at(function);
    if (!callee.isInIndirectTailCallCycle || callee.isCalledFromDroppableFrame || callee.blocks.isEmpty() || callee.takesCapturesFromClosure || callee.reservedStackArgumentCount || !holdsCells(resultRepOf(m_program, function)))
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (unsigned i = 0; i < callee.blocks[0].parameters.size(); ++i)
        ++(b3TypeOf(repOf(declaredArgumentType(callee, i))) == Double ? numberOfFPRs : numberOfGPRs);
    return numberOfGPRs <= numberOfDirectArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
}

bool Lowering::mayCallDirectlyFromTailCallSite(FunctionID function, const Type& result) const
{
    auto& callee = m_program.at(function);
    if (!m_dropsDroppableCallerFrame || !callee.hasTailCallSites || mayJumpToWidenedFunction(function))
        return true;
    if (!m_tailCall || callee.blocks.isEmpty())
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (unsigned i = 0; i < callee.blocks[0].parameters.size(); ++i)
        ++(b3TypeOf(repOf(declaredArgumentType(callee, i))) == Double ? numberOfFPRs : numberOfGPRs);
    return canTailCallDirectly(function, numberOfGPRs, numberOfFPRs, result, false);
}

bool Lowering::passesPointerIntoFrame(const Value& call) const
{
    bool passes = false;
    call.forEachChild([&](ValueID child) {
        passes |= m_function.mayPointIntoFrame.get(originOf(child).index());
    });
    return passes;
}

static bool hasSameRegisterRepresentation(Rep from, Rep to)
{
    return from == to || (from == Rep::Cell && to == Rep::JSValue);
}

std::optional<ValueID> Lowering::valueReturnedAfter(const Block& block) const
{
    constexpr unsigned maxBlocks = 8;
    Vector<const Target*, maxBlocks> hops;
    const Terminator* terminator = &block.terminator;
    while (terminator->kind != TerminatorKind::Return) {
        if (terminator->kind != TerminatorKind::Jump || hops.size() == maxBlocks)
            return std::nullopt;
        auto& target = terminator->targets[0];
        if (!m_function.at(target.block).values.isEmpty())
            return std::nullopt;
        hops.append(&target);
        terminator = &m_function.at(target.block).terminator;
    }
    ValueID value = terminator->value;
    for (auto* hop : hops | std::views::reverse) {
        if (size_t index = value ? m_function.at(hop->block).parameters.find(value) : notFound; index != notFound)
            value = hop->arguments[index];
    }
    return value;
}

std::optional<unsigned> Lowering::returnedParameterOf(FunctionID function) const
{
    auto& callee = m_program.at(function);
    std::optional<unsigned> found;
    for (auto& block : callee.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        if (!block.terminator.value)
            return std::nullopt;
        auto& returned = callee.at(m_program.originOf(callee, block.terminator.value));
        if (returned.opcode != Opcode::Parameter || returned.immediates[0] || (found && *found != returned.immediates[1]))
            return std::nullopt;
        found = returned.immediates[1];
    }
    return found;
}

bool Lowering::canTailCallDirectly(FunctionID function, unsigned numberOfGPRs, unsigned numberOfFPRs, const Type& result, bool returnsArgument) const
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty() || !callee.resultTypes.isEmpty() || callee.kind == FunctionKind::Module || m_program.moduleEvaluationIndexByFunction.contains(function.index()))
        return false;
    Rep own = resultRepOf(m_program, m_id);
    Rep returnedValue = resultRepOf(m_program, function);
    if (own != Rep::Void && (returnedValue == Rep::Void || (!hasSameRegisterRepresentation(returnedValue, own) && !(returnsArgument && returnedValue == Rep::JSValue && own == Rep::Cell))))
        return false;
    if (!returnsArgument && !m_program.at(callee.resultType ? callee.resultType : m_program.types.anything).isSubsetOf(result, m_program))
        return false;
    unsigned onStack = numberOfGPRs - std::min<unsigned>(numberOfGPRs, numberOfDirectArgumentGPRs) + numberOfFPRs - std::min<unsigned>(numberOfFPRs, FPRInfo::numberOfArgumentRegisters);
    return std::max<unsigned>(onStack, callee.reservedStackArgumentCount) <= (m_function.isGenericEntrypoint() ? 0u : m_function.reservedStackArgumentCount);
}

void Lowering::findTailCallsEmittedAsJumps()
{
    for (auto& block : m_function.blocks) {
        if (block.values.isEmpty())
            continue;
        ValueID id = block.values.last();
        auto& call = m_function.at(id);
        if (!call.isInTailPosition || call.leavesDroppableFrame || (call.opcode == Opcode::CallDirect && call.isJumpToContinuation()) || !tailCallDropsMachineFrame(block, call))
            continue;
        auto returned = valueReturnedAfter(block);
        if (!returned)
            continue;
        bool returnsArgument = false;
        if (*returned != id && (*returned || resultRepOf(m_program, m_id) != Rep::Void)) {
            if (call.opcode != Opcode::CallDirect || !*returned)
                continue;
            auto returnedParameter = returnedParameterOf(call.function());
            if (!returnedParameter || *returnedParameter >= call.arguments.size() || m_program.originOf(m_function, call.arguments[*returnedParameter]) != m_program.originOf(m_function, *returned))
                continue;
            returnsArgument = true;
        }
        if (passesPointerIntoFrame(call))
            continue;
        if (call.opcode != Opcode::CallDirect) {
            m_tailCallCandidates.set(id.index());
            continue;
        }
        if (!call.arguments.isEmpty() && isPassedInMemory(call.arguments.last()))
            continue;
        auto& callee = m_program.at(call.function());
        if (callee.blocks.isEmpty())
            continue;
        unsigned numberOfGPRs = 0;
        unsigned numberOfFPRs = 0;
        for (unsigned i = 0; i < callee.blocks[0].parameters.size(); ++i)
            ++(b3TypeOf(repOf(declaredArgumentType(callee, i))) == Double ? numberOfFPRs : numberOfGPRs);
        if (canTailCallDirectly(call.function(), numberOfGPRs, numberOfFPRs, m_program.at(call.type), returnsArgument)) {
            m_tailCallCandidates.set(id.index());
            m_tailCallsEmittedAsJumps.set(id.index());
            if (returnsArgument)
                m_tailCallsReturningArgument.set(id.index());
            m_hasSelfTailCall |= call.function() == m_id && &m_function == &m_program.at(m_id) && !m_function.has(FunctionUsesArgumentBuffer);
        }
    }
}

void Lowering::didEmitTailCall()
{
    if (!m_program.hasCheckedStackOnEntry(m_id))
        m_context.needsStackCheckForTailCalls = true;
}

bool Lowering::isLastCallOfGenericEntrypoint(FunctionID function) const
{
    if ((!m_function.isGenericEntrypoint() && (m_function.valueEntryTarget != function || m_function.isValueEntryClone)) || !m_currentBlock || m_currentIndexInBlock + 1 != m_currentBlock->values.size())
        return false;
    if (m_function.isGenericEntrypoint() && m_program.at(function).isCalledFromDroppableFrame)
        return false;
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Return || terminator.value != m_currentBlock->values.last() || m_function.at(terminator.value).opcode != Opcode::CallDirect || m_function.at(terminator.value).function() != function)
        return false;
    Rep rep = resultRepOf(m_program, function);
    for (ValueID argument : m_function.at(terminator.value).arguments) {
        if (LayoutID supertype = typeOf(argument).singleLayout(); (supertype && m_program.at(supertype).mayBeStackAllocated) || m_function.mayPointIntoFrame.get(originOf(argument).index()))
            return false;
    }
    Rep own = resultRepOf(m_program, m_id);
    return own == Rep::Void || own == rep || (own == Rep::JSValue && rep == Rep::Cell);
}

bool Lowering::callsFromDroppableFrame(FunctionID function) const
{
    if (!m_function.isGenericEntrypoint() || !m_program.at(function).isCalledFromDroppableFrame || m_activeHandler || !m_currentBlock || m_currentIndexInBlock + 1 != m_currentBlock->values.size())
        return false;
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Return || terminator.value != m_currentBlock->values.last() || m_function.at(terminator.value).opcode != Opcode::CallDirect || m_function.at(terminator.value).function() != function)
        return false;
    for (ValueID argument : m_function.at(terminator.value).arguments) {
        if (LayoutID supertype = typeOf(argument).singleLayout(); (supertype && m_program.at(supertype).mayBeStackAllocated) || m_function.mayPointIntoFrame.get(originOf(argument).index()))
            return false;
    }
    return true;
}

Word32 Lowering::isCalledFromDroppableFrame()
{
    m_context.emitsCalls = true;
    m_context.tailCallsNeedFrame = true;
    PatchpointValue* returnAddress = m_out.patchpoint(Int64);
    returnAddress->effects = Effects::none();
    returnAddress->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.loadPtr(CCallHelpers::Address(GPRInfo::callFrameRegister, CallFrame::returnPCOffset()), params[0].gpr());
    });
    return equal(Integer(m_out.load64(TypedPointer(m_heaps.root, m_out.add(returnAddress, m_out.constIntPtr(droppableFrameSignatureOffset))))), integer(static_cast<int64_t>(droppableFrameSignature)));
}

bool Lowering::mayJumpTo(FunctionID function, const Vector<LValue, 8>& arguments) const
{
    if (!isLastCallOfGenericEntrypoint(function) || m_program.at(function).reservedStackArgumentCount)
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (LValue argument : arguments)
        ++(argument->type() == Double ? numberOfFPRs : numberOfGPRs);
    return numberOfGPRs <= numberOfDirectArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
}

void Lowering::jumpToFunction(uint32_t callee, const Vector<LValue, 8>& arguments, LValue closure, bool dropsCallerFrame)
{
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    Vector<ConstrainedValue, 8> inRegisters;
    Vector<LValue, 4> onStack;
    if (closure)
        inRegisters.append(ConstrainedValue(closure, ValueRep::reg(calleeGPR)));
    for (LValue argument : arguments) {
        bool isPassed = !m_unusedParameterPlaceholders.contains(argument);
        if (argument->type() == Double && nextFPR < FPRInfo::numberOfArgumentRegisters) {
            FPRReg reg = FPRInfo::toArgumentRegister(nextFPR++);
            if (isPassed)
                inRegisters.append(ConstrainedValue(argument, ValueRep::reg(reg)));
        } else if (argument->type() != Double && nextGPR < numberOfDirectArgumentGPRs) {
            GPRReg reg = directArgumentGPR(nextGPR++);
            if (isPassed)
                inRegisters.append(ConstrainedValue(argument, ValueRep::reg(reg)));
        } else
            onStack.append(argument->type() == Int32 ? m_out.zeroExt(argument, Int64) : argument);
    }
    unsigned numberInRegisters = inRegisters.size();
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    for (auto& argument : inRegisters)
        patchpoint->append(argument);
    for (LValue argument : onStack)
        patchpoint->append(ConstrainedValue(argument, ValueRep::WarmAny));
    if (!onStack.isEmpty()) {
        m_context.readsCallerFrame = true;
        patchpoint->numGPScratchRegisters = 1;
    }
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    RELEASE_ASSERT(!dropsCallerFrame || onStack.isEmpty());
    patchpoint->setGenerator([context = &m_context, callee, numberInRegisters, dropsCallerFrame](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = numberInRegisters; i < params.size(); ++i) {
            CCallHelpers::Address slot(GPRInfo::callFrameRegister, static_cast<int32_t>(sizeof(CallerFrameAndPC) + (i - numberInRegisters) * sizeof(Register)));
            const ValueRep& rep = params[i];
            if (rep.isGPR())
                jit.store64(rep.gpr(), slot);
            else if (rep.isFPR())
                jit.storeDouble(rep.fpr(), slot);
            else {
                if (rep.isConstant())
                    jit.move(CCallHelpers::TrustedImm64(rep.value()), params.gpScratch(0));
                else
                    jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(rep.offsetFromFP())), params.gpScratch(0));
                jit.store64(params.gpScratch(0), slot);
            }
        }
        context->emitFrameTeardown(jit, params.proc().code());
        if (dropsCallerFrame)
            dropDroppableCallerFrame(jit);
        context->stubCalls.jumpToFunction(jit, callee);
    });
    if (!dropsCallerFrame)
        m_out.appendTo(m_out.newBlock());
}

void Lowering::callFromCollapsibleFrame(FunctionID function, const Vector<LValue, 8>& arguments, LValue closure)
{
    m_context.emitsCalls = true;
    m_context.readsCallerFrame = true;
    m_context.tailCallsNeedFrame = true;
    m_context.dropsCollapsibleCallerFrames = !m_function.has(FunctionUsesArgumentBuffer);
    uint32_t callee = calleeIndex(function);
    Vector<LValue, 8> widened;
    for (LValue argument : arguments)
        widened.append(argument->type() == Int32 ? m_out.zeroExt(argument, Int64) : argument);
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    unsigned onStack = 0;
    if (closure)
        patchpoint->append(ConstrainedValue(closure, ValueRep::reg(calleeGPR)));
    for (unsigned i = 0; i < arguments.size(); ++i) {
        bool isPassed = !m_unusedParameterPlaceholders.contains(arguments[i]);
        if (arguments[i]->type() == Double && nextFPR < FPRInfo::numberOfArgumentRegisters) {
            FPRReg reg = FPRInfo::toArgumentRegister(nextFPR++);
            if (isPassed)
                patchpoint->append(ConstrainedValue(arguments[i], ValueRep::reg(reg)));
        } else if (arguments[i]->type() != Double && nextGPR < numberOfDirectArgumentGPRs) {
            GPRReg reg = directArgumentGPR(nextGPR++);
            if (isPassed)
                patchpoint->append(ConstrainedValue(arguments[i], ValueRep::reg(reg)));
        } else
            patchpoint->append(ConstrainedValue(widened[i], ValueRep::stackArgument(static_cast<int32_t>(onStack++ * sizeof(Register)))));
    }
    if (unsigned room = std::max<unsigned>(onStack, m_program.at(function).reservedStackArgumentCount))
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(room * sizeof(Register)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, callee, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->emitCalleeSaveRestoreKeepingFrame(jit, params.proc().code());
        context->stubCalls.callFunction(jit, callee, site);
        emitCollapsibleFrameReturn(jit);
    });
    m_out.appendTo(m_out.newBlock());
}

LValue Lowering::callFunction(FunctionID function, LType type, const Vector<LValue, 8>& arguments, FunctionID summarizedAs, LValue closure, bool isFromDroppableFrame)
{
    m_context.emitsCalls = true;
    m_context.tailCallsNeedFrame |= isFromDroppableFrame;
    uint32_t callee = calleeIndex(function);
    Vector<LValue, 8> widened;
    for (LValue argument : arguments)
        widened.append(argument->type() == Int32 ? m_out.zeroExt(argument, Int64) : argument);
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    auto& summary = m_program.at(summarizedAs ? summarizedAs : function);
    OperationEffects effects { summary.writes, true, summary.writesShapeWords };
    if (!m_activeHandler && summary.writes != Writes::Pure)
        effects = { std::max(summary.writesOnReturn, Writes::Nothing), true, summary.writesShapeWordsOnReturn };
    effects.convertsArrayStorage = m_activeHandler ? summary.convertsArrayStorage : summary.convertsArrayStorageOnReturn;
    applyEffects(patchpoint, effects);
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    unsigned onStack = 0;
    Vector<LValue, 8> inGPRs;
    for (LValue argument : arguments) {
        if (argument->type() != Double)
            inGPRs.append(argument);
    }
    unsigned endOfOmittedGPRs = inGPRs.size();
    unsigned firstOmittedGPR = endOfOmittedGPRs;
    if (endOfOmittedGPRs <= numberOfDirectArgumentGPRs) {
        while (firstOmittedGPR && inGPRs[firstOmittedGPR - 1]->hasInt() && inGPRs[firstOmittedGPR - 1]->asInt() == static_cast<int64_t>(JSValue::ValueUndefined))
            --firstOmittedGPR;
    }
    for (unsigned i = 0; i < arguments.size(); ++i) {
        LValue argument = arguments[i];
        bool isPassed = !m_unusedParameterPlaceholders.contains(argument);
        if (argument->type() == Double && nextFPR < FPRInfo::numberOfArgumentRegisters) {
            FPRReg reg = FPRInfo::toArgumentRegister(nextFPR++);
            if (isPassed)
                patchpoint->append(ConstrainedValue(argument, ValueRep::reg(reg)));
        } else if (argument->type() != Double && nextGPR >= firstOmittedGPR)
            ++nextGPR;
        else if (argument->type() != Double && nextGPR < numberOfDirectArgumentGPRs) {
            GPRReg reg = directArgumentGPR(nextGPR++);
            if (isPassed)
                patchpoint->append(ConstrainedValue(argument, ValueRep::reg(reg)));
        } else
            patchpoint->append(ConstrainedValue(widened[i], ValueRep::stackArgument(static_cast<int32_t>(onStack++ * sizeof(Register)))));
    }
    if (closure)
        patchpoint->append(ConstrainedValue(closure, ValueRep::reg(calleeGPR)));
    if (unsigned room = std::max<unsigned>(onStack, m_program.at(function).reservedStackArgumentCount))
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(room * sizeof(Register)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    if (type.isTuple()) {
        patchpoint->resultConstraints.clear();
        patchpoint->resultConstraints.appendVector(resultRegisters(m_program.at(function)));
    } else if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    if (firstOmittedGPR == endOfOmittedGPRs)
        firstOmittedGPR = endOfOmittedGPRs = 0;
    patchpoint->setGenerator([context = &m_context, callee, site, firstOmittedGPR, endOfOmittedGPRs, isFromDroppableFrame](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->stubCalls.callFunction(jit, callee, site, static_cast<uint8_t>(firstOmittedGPR), static_cast<uint8_t>(endOfOmittedGPRs));
        if (isFromDroppableFrame) {
            emitDroppableFrameExit(jit, [&] {
                context->emitFrameTeardown(jit, params.proc().code());
            });
        }
    });
    unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

LValue Lowering::callFunctionFromColdBlock(FunctionID function, bool returnsValue)
{
    uint32_t callee = calleeIndex(function);
    PatchpointValue* patchpoint = m_out.patchpoint(returnsValue ? Int64 : Void);
    if (returnsValue)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    auto& summary = m_program.at(function);
    OperationEffects effects { summary.writes, true, summary.writesShapeWords };
    if (!m_activeHandler && summary.writes != Writes::Pure)
        effects = { std::max(summary.writesOnReturn, Writes::Nothing), true, summary.writesShapeWordsOnReturn };
    effects.convertsArrayStorage = m_activeHandler ? summary.convertsArrayStorage : summary.convertsArrayStorageOnReturn;
    applyEffects(patchpoint, effects);
    patchpoint->clobber(*registersClobberedBy(Stub::ColdCallFunction));
    if (!hasFramelessFunctionStubs) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    }
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, callee, site, returnsValue](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (returnsValue)
            context->stubCalls.call(jit, isLeaf ? Stub::LeafColdCallFunctionForValue : Stub::ColdCallFunctionForValue, site);
        else
            context->stubCalls.call(jit, isLeaf ? Stub::LeafColdCallFunction : Stub::ColdCallFunction, site);
        context->stubCalls.jumpToFunction(jit, callee);
    });
    unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

static std::optional<Stub> stubForSpeculatedMethod(SpeculatedMethod method, bool methodIsRead)
{
    static_assert(static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 1 == static_cast<unsigned>(Stub::ArrayPushOrCallValue) && static_cast<unsigned>(Stub::SetAddOrCallMethod) + 1 == static_cast<unsigned>(Stub::SetAddOrCallValue));
    static_assert(static_cast<unsigned>(Stub::SetAddOrCallMethod) == static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(SpeculatedMethod::SetAdd) - static_cast<unsigned>(SpeculatedMethod::ArrayPush)));
    static_assert(static_cast<unsigned>(Stub::RegExpTestOrCallValue) == static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(SpeculatedMethod::RegExpTest) - static_cast<unsigned>(SpeculatedMethod::ArrayPush)) + 1);
    static_assert(static_cast<unsigned>(Stub::StringMatchOrCallValue) == static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(SpeculatedMethod::StringMatch) - static_cast<unsigned>(SpeculatedMethod::ArrayPush)) + 1);
    if (method == SpeculatedMethod::None)
        return std::nullopt;
    return static_cast<Stub>(static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(method) - static_cast<unsigned>(SpeculatedMethod::ArrayPush)) + methodIsRead);
}

Boxed Lowering::callValue(Stub stub, const Vector<LValue, 8>& operands, OperationEffects effects, LValue word, SpeculatedMethod method, StringID methodName, std::optional<StringID> nameForCallableCheck)
{
    bool didEmitCalls = std::exchange(m_context.emitsCalls, true);
    unsigned count = operands.size() - 2;
    RELEASE_ASSERT(count <= numberOfArgumentGPRs);
    RELEASE_ASSERT(method == SpeculatedMethod::None || stub == Stub::CallValue);
    RELEASE_ASSERT(!methodName || (method == SpeculatedMethod::None && stub == Stub::CallValue && methodName.index() <= ThrowSite::maxNarrowA));
    if (methodName)
        stub = Stub::CallValueNamed;
    bool isTailCall = method == SpeculatedMethod::None && stub == Stub::CallValue && canTailCallValue();
    if (method == SpeculatedMethod::None && stub == Stub::CallValue && operands[1]->hasInt64() && operands[1]->asInt64() == static_cast<int64_t>(JSValue::encode(jsUndefined())))
        stub = Stub::CallValueWithoutThis;
    CallSite callableCheckSite;
    if (nameForCallableCheck) {
        RELEASE_ASSERT(!m_activeHandler);
        SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
        callableCheckSite = { nextSite() };
    }
    auto checkCallable = [stubCalls = &m_context.stubCalls, nameForCallableCheck, callableCheckSite](CCallHelpers& jit) {
        if (!nameForCallableCheck)
            return;
        if (*nameForCallableCheck)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(nameForCallableCheck->index())), stubTemporaryGPRs[2]);
        stubCalls->call(jit, *nameForCallableCheck ? Stub::CheckCallableNamed : Stub::CheckCallable, callableCheckSite);
    };
    bool checksDroppableCallerFrame = method == SpeculatedMethod::None && (stub == Stub::CallValue || stub == Stub::CallValueWithoutThis) && m_dropsDroppableCallerFrame;
    if (isTailCall || checksDroppableCallerFrame) {
        LBasicBlock staysInFrame = nullptr;
        if (checksDroppableCallerFrame) {
            RELEASE_ASSERT(!nameForCallableCheck);
            LBasicBlock dropsFrames = m_out.newBlock();
            staysInFrame = m_out.newBlock();
            m_out.branch(isCalledFromDroppableFrame().value(), unsure(dropsFrames), unsure(staysInFrame));
            m_out.appendTo(dropsFrames);
        } else {
            m_context.emitsCalls = didEmitCalls || nameForCallableCheck;
            m_context.tailCallsNeedFrame |= !!nameForCallableCheck;
        }
        PatchpointValue* jump = m_out.patchpoint(Void);
        jump->append(ConstrainedValue(operands[0], ValueRep::reg(calleeGPR)));
        if (stub != Stub::CallValueWithoutThis)
            jump->append(ConstrainedValue(operands[1], ValueRep::reg(thisGPR)));
        for (unsigned i = 0; i < count; ++i)
            jump->append(ConstrainedValue(operands[i + 2], ValueRep::reg(argumentGPR(i))));
        jump->clobber(RegisterSet::macroClobberedGPRs());
        jump->effects.terminal = true;
        jump->setGenerator([context = &m_context, stub, count, checkCallable, checksDroppableCallerFrame](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            checkCallable(jit);
            context->emitFrameTeardown(jit, params.proc().code());
            if (checksDroppableCallerFrame)
                dropDroppableCallerFrame(jit);
            context->stubCalls.tailCall(jit, stub, count);
        });
        if (!checksDroppableCallerFrame) {
            m_out.appendTo(m_out.newBlock());
            return Boxed(m_out.int64Zero);
        }
        m_out.appendTo(staysInFrame);
    }
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, effects);
    patchpoint->append(ConstrainedValue(operands[0], ValueRep::reg(calleeGPR)));
    if (stub != Stub::CallValueWithoutThis)
        patchpoint->append(ConstrainedValue(operands[1], ValueRep::reg(thisGPR)));
    for (unsigned i = 0; i < count; ++i)
        patchpoint->append(ConstrainedValue(operands[i + 2], ValueRep::reg(argumentGPR(i))));
    RELEASE_ASSERT(!!word == (stub == Stub::ConstructClosure));
    if (word)
        patchpoint->append(ConstrainedValue(word, ValueRep::reg(stubTemporaryGPRs[2])));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    uint32_t throwWord = methodName ? throwSiteWords(ThrowKind::NotAFunction, Rep::JSValue, static_cast<unsigned>(calleeGPR), methodName.index(), std::nullopt)[0] : 0;
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, count, site, throwWord, asks = stubForSpeculatedMethod(method, true), checkCallable](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        checkCallable(jit);
        if (stub == Stub::CallValueNamed)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(throwWord)), stubTemporaryGPRs[2]);
        if (asks)
            stubCalls->call(jit, *asks, site);
        else
            stubCalls->call(jit, stub, count, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

Boxed Lowering::callValueWithArray(Stub stub, Boxed callee, Boxed thisValue, Boxed list, bool mayTailCall)
{
    if (mayTailCall && (canTailCallValue() || m_dropsDroppableCallerFrame)) {
        bool checksDroppableCallerFrame = m_dropsDroppableCallerFrame;
        LBasicBlock staysInFrame = nullptr;
        if (checksDroppableCallerFrame) {
            LBasicBlock dropsFrames = m_out.newBlock();
            staysInFrame = m_out.newBlock();
            m_out.branch(isCalledFromDroppableFrame().value(), unsure(dropsFrames), unsure(staysInFrame));
            m_out.appendTo(dropsFrames);
        }
        PatchpointValue* jump = m_out.patchpoint(Void);
        jump->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
        jump->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
        jump->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(0))));
        jump->clobber(RegisterSet::macroClobberedGPRs());
        jump->effects.terminal = true;
        jump->setGenerator([context = &m_context, stub, checksDroppableCallerFrame](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            context->emitFrameTeardown(jit, params.proc().code());
            if (checksDroppableCallerFrame)
                dropDroppableCallerFrame(jit);
            context->stubCalls.tailCall(jit, stub);
        });
        if (!checksDroppableCallerFrame) {
            m_out.appendTo(m_out.newBlock());
            return Boxed(m_out.int64Zero);
        }
        m_out.appendTo(staysInFrame);
    }
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
    patchpoint->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(0))));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, site, stub](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->call(jit, stub, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

Boxed Lowering::callValueWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list, LValue constructorWord)
{
    LValue extendedCount = m_out.zeroExtPtr(count.value());
    if (!constructorWord && canTailCallValue()) {
        m_context.readsCallerFrame = true;
        m_context.tailCallsNeedFrame = true;
        PatchpointValue* jump = m_out.patchpoint(Void);
        jump->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
        jump->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
        jump->append(ConstrainedValue(extendedCount, ValueRep::reg(argumentGPR(0))));
        jump->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(1))));
        jump->clobber(RegisterSet::macroClobberedGPRs());
        jump->effects.terminal = true;
        jump->setGenerator([context = &m_context](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            context->emitCalleeSaveRestoreKeepingFrame(jit, params.proc().code());
            context->stubCalls.tailCall(jit, Stub::TailCallList);
        });
        m_out.appendTo(m_out.newBlock());
        return Boxed(m_out.int64Zero);
    }
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
    patchpoint->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(extendedCount, ValueRep::reg(argumentGPR(0))));
    patchpoint->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(1))));
    if (constructorWord)
        patchpoint->append(ConstrainedValue(constructorWord, ValueRep::reg(stubTemporaryGPRs[2])));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, site, stub = constructorWord ? Stub::ConstructClosureList : Stub::CallList](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->call(jit, stub, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

FunctionID Lowering::listEntryFor(FunctionID function)
{
    return m_listEntries.ensure(function.index(), [&]() -> FunctionID {
        auto& callee = m_program.at(function);
        if (!callee.has(Format::FunctionHasRestParameter) || callee.kind == FunctionKind::Constructor || callee.parameterTypes.isEmpty() || callee.blocks.isEmpty())
            return { };
        auto found = m_program.genericEntrypoints.find(function.index());
        if (found == m_program.genericEntrypoints.end())
            return { };
        auto& entry = m_program.at(found->value);
        if (!entry.has(FunctionUsesArgumentBuffer) || resultRepOf(m_program, found->value) != Rep::JSValue)
            return { };
        for (auto& block : entry.blocks) {
            for (ValueID id : block.values) {
                if (entry.at(id).opcode == Opcode::CallDirect && entry.at(id).function() == function)
                    return { };
            }
        }
        return found->value;
    }).iterator->value;
}

Boxed Lowering::callListEntry(FunctionID entry, LValue closure, Boxed thisValue, const Arguments& arguments)
{
    m_context.emitsCalls = true;
    uint32_t callee = calleeIndex(entry);
    auto [count, list] = argumentBuffer(arguments);
    LValue extendedCount = m_out.zeroExtPtr(count.value());
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    auto& summary = m_program.at(entry);
    OperationEffects effects { std::max(summary.writes, Writes::Nothing), true, summary.writesShapeWords };
    if (!m_activeHandler)
        effects = { std::max(summary.writesOnReturn, Writes::Nothing), true, summary.writesShapeWordsOnReturn };
    effects.convertsArrayStorage = m_activeHandler ? summary.convertsArrayStorage : summary.convertsArrayStorageOnReturn;
    applyEffects(patchpoint, effects);
    patchpoint->append(ConstrainedValue(closure, ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(extendedCount, ValueRep::reg(argumentGPR(0))));
    patchpoint->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(1))));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, callee, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->callFunction(jit, callee, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

bool Lowering::isPassedInMemory(ValueID array)
{
    if (!std::exchange(m_didFindRestArraysPassedInMemory, true)) {
        Vector<const Block*> newArrayBlock;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& call = m_function.at(id);
                if (call.takesElementsOfArrayLiteral)
                    m_restArraysPassedInMemory.set(call.arguments[0].index());
                if (call.opcode != Opcode::CallDirect || call.arguments.isEmpty() || call.isJumpToContinuation())
                    continue;
                ValueID rest = call.arguments.last();
                if (m_function.at(rest).opcode != Opcode::NewArray || useCountOf(rest) != 1)
                    continue;
                if (newArrayBlock.isEmpty()) {
                    newArrayBlock.fill(nullptr, m_function.values.size());
                    for (auto& other : m_function.blocks) {
                        for (ValueID value : other.values) {
                            if (m_function.at(value).opcode == Opcode::NewArray)
                                newArrayBlock[value.index()] = &other;
                        }
                    }
                }
                if (newArrayBlock[rest.index()] != &block)
                    continue;
                FunctionID entry = listEntryFor(call.function());
                auto& callee = m_program.at(call.function());
                if (!entry || !callee.captureTypes.isEmpty() || call.arguments.size() != callee.blocks[0].parameters.size())
                    continue;
                auto& summary = m_program.at(entry);
                if (summary.writes > callee.writes || summary.writesOnReturn > callee.writesOnReturn || summary.writesShapeWords > callee.writesShapeWords || summary.writesShapeWordsOnReturn > callee.writesShapeWordsOnReturn || summary.convertsArrayStorage > callee.convertsArrayStorage || summary.convertsArrayStorageOnReturn > callee.convertsArrayStorageOnReturn)
                    continue;
                bool isCoercedAgain = false;
                for (unsigned i = 0; i + 1 < callee.parameterTypes.size(); ++i)
                    isCoercedAgain |= callee.parameterTypes[i] && callee.parameterCastReason(i) == CastReason::As && m_program.at(callee.parameterTypes[i]).hasObjects();
                if (isCoercedAgain)
                    continue;
                m_restArraysPassedInMemory.set(rest.index());
                m_listEntryByCall.add(id.index(), entry);
            }
        }
    }
    return m_restArraysPassedInMemory.get(array.index());
}

LValue Lowering::callAllocationStub(Stub stub, const Vector<std::pair<LValue, GPRReg>, 4>& operands, OperationEffects effects, bool mayThrow, bool isSlowPathCall, bool resultIsDouble, LValue number, std::optional<uint32_t> thunkImmediate, Stub stubWithoutThunk, uint32_t operandWithoutThunk, std::span<const LValue> additionalNumbers)
{
    PatchpointValue* patchpoint = m_out.patchpoint(resultIsDouble ? Double : Int64);
    applyEffects(patchpoint, effects);
    std::optional<uint32_t> keyEncodedInResult;
    if (hasResultGPRVariants(stub) && !Options::useAOTWideReferences() && !thunkImmediate && operands.size() == 1 && operands[0].second == lazyConstantKeyGPR && operands[0].first->hasInt64() && static_cast<uint64_t>(operands[0].first->asInt64()) <= std::numeric_limits<int32_t>::max())
        keyEncodedInResult = static_cast<uint32_t>(operands[0].first->asInt64());
    RegisterSet taken;
    for (auto& [value, reg] : operands) {
        if (keyEncodedInResult)
            break;
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(reg)));
        taken.add(reg, IgnoreVectors);
    }
    if (number)
        patchpoint->append(ConstrainedValue(number, ValueRep::reg(FPRInfo::fpRegT0)));
    for (unsigned i = 0; i < additionalNumbers.size(); ++i)
        patchpoint->append(ConstrainedValue(additionalNumbers[i], ValueRep::reg(FPRInfo::toRegister(1 + i))));
    patchpoint->resultConstraints = { keyEncodedInResult ? ValueRep::SomeRegister : resultIsDouble ? ValueRep::reg(FPRInfo::fpRegT0) : ValueRep::reg(GPRInfo::returnValueGPR) };
    if (auto changed = registersClobberedBy(stub)) {
        static_assert(hasFramelessFunctionStubs == isARM64());
        if (!hasFramelessFunctionStubs) {
            if (!isSlowPathCall || LoweringContext::coldSitesForceFrame() || m_activeHandler) {
                m_context.emitsCalls = true;
                patchpoint->clobberLate(RegisterSet { callMarkerGPR });
            } else
                m_context.hasColdSites = true;
        }
        patchpoint->clobberLate(*changed);
        if (isARM64()) {
            changed->exclude(taken);
            patchpoint->clobberEarly(*changed);
        } else
            patchpoint->clobberEarly(RegisterSet::macroClobberedGPRs());
    } else {
        m_context.emitsCalls = true;
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        patchpoint->clobberLate(registersClobberedByCalls());
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
#endif
    }
    CallSite site;
    if (mayThrow)
        site.bits = nextSite();
    patchpoint->setGenerator([context = &m_context, stub, site, thunkImmediate, stubWithoutThunk, operandWithoutThunk, keyEncodedInResult](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasFramelessFunctionStubs && hasNoFrame;
        bool createsFrame = !hasFramelessFunctionStubs && hasNoFrame;
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (createsFrame)
            jit.emitFunctionPrologue();
        if (keyEncodedInResult) {
            GPRReg result = params[0].gpr();
            Stub form = stubVariantForResultGPR(stub, result);
            bool isCreatedElsewhere = form == stub && result != GPRInfo::returnValueGPR;
            if (isCreatedElsewhere) {
                jit.push(GPRInfo::returnValueGPR);
                jit.push(GPRInfo::returnValueGPR);
            }
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(*keyEncodedInResult)), isCreatedElsewhere ? GPRInfo::returnValueGPR : result);
            context->stubCalls.call(jit, form, site);
            if (isCreatedElsewhere) {
                jit.move(GPRInfo::returnValueGPR, result);
                jit.pop(GPRInfo::returnValueGPR);
                jit.pop(GPRInfo::returnValueGPR);
            }
        } else if (thunkImmediate && !isLeaf)
            context->stubCalls.call(jit, stub, *thunkImmediate, site);
        else if (thunkImmediate) {
            jit.move(CCallHelpers::TrustedImm32(operandWithoutThunk), lazyConstantKeyGPR);
            context->stubCalls.call(jit, static_cast<Stub>(static_cast<unsigned>(stubWithoutThunk) + 1), site);
        } else
            context->stubCalls.call(jit, isLeaf ? static_cast<Stub>(static_cast<unsigned>(stub) + 1) : stub, site);
        if (createsFrame)
            jit.emitFunctionEpilogueWithEmptyFrame();
    });
    if (mayThrow)
        unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

Lowering::SubstringCharacters Lowering::substringCharacters(CellPointer string)
{
    PatchpointValue* patchpoint = m_out.patchpoint(m_proc.addTuple({ Int64, Int64, Int64 }));
    applyEffects(patchpoint, { Writes::Nothing, false });
    patchpoint->append(ConstrainedValue(string.value(), ValueRep::reg(stubTemporaryGPRs[3])));
    patchpoint->resultConstraints = { ValueRep::reg(stubTemporaryGPRs[3]), ValueRep::reg(stubTemporaryGPRs[0]), ValueRep::reg(stubTemporaryGPRs[4]) };
    if (auto changed = registersClobberedBy(Stub::SubstringCharacters)) {
        patchpoint->clobberLate(*changed);
        changed->exclude(RegisterSet { stubTemporaryGPRs[3] });
        patchpoint->clobberEarly(*changed);
    } else {
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
    }
    patchpoint->setGenerator([context = &m_context](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (context->hasNoFrame(params.proc().code()))
            emitSubstringCharacters(jit);
        else
            context->stubCalls.call(jit, Stub::SubstringCharacters, CallSite { });
    });
    auto result = [&](int32_t index) {
        return m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), Int64, patchpoint, index);
    };
    return { result(0), m_out.castToInt32(result(1)), m_out.castToInt32(result(2)) };
}

Lowering::SubstringCharacters Lowering::ropeCharacters(CellPointer string)
{
    RELEASE_ASSERT(!m_activeHandler);
    PatchpointValue* patchpoint = m_out.patchpoint(m_proc.addTuple({ Int64, Int64, Int64 }));
    applyEffects(patchpoint, effectsForOperation<operationAOTTypedFlatten>());
    patchpoint->append(ConstrainedValue(string.value(), ValueRep::reg(stubTemporaryGPRs[3])));
    patchpoint->resultConstraints = { ValueRep::reg(stubTemporaryGPRs[3]), ValueRep::reg(stubTemporaryGPRs[0]), ValueRep::reg(stubTemporaryGPRs[4]) };
    if (auto changed = registersClobberedBy(Stub::RopeCharacters)) {
        patchpoint->clobberLate(*changed);
        changed->exclude(RegisterSet { stubTemporaryGPRs[3] });
        patchpoint->clobberEarly(*changed);
    } else
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    if (!hasFramelessFunctionStubs) {
        if (LoweringContext::coldSitesForceFrame()) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else
            m_context.hasColdSites = true;
    }
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasFramelessFunctionStubs && hasNoFrame;
        bool createsFrame = !hasFramelessFunctionStubs && hasNoFrame;
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (createsFrame)
            jit.emitFunctionPrologue();
        context->stubCalls.call(jit, isLeaf ? Stub::LeafRopeCharacters : Stub::RopeCharacters, site);
        if (createsFrame)
            jit.emitFunctionEpilogueWithEmptyFrame();
    });
    auto result = [&](int32_t index) {
        return m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), Int64, patchpoint, index);
    };
    return { result(0), m_out.castToInt32(result(1)), m_out.castToInt32(result(2)) };
}

CellPointer Lowering::allocate(uint32_t site, Integer number, FieldInit fieldInit)
{
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        return AOT_TYPED_OPERATION(AllocSlow, low32(number));
    RELEASE_ASSERT(fieldInit != FieldInit::FromTemplate || m_program.allocations[site].numberOfLiterals);
    Stub stub = fieldInit == FieldInit::FromTemplate ? Stub::AllocateTypedObjectFromTemplate : isObjectLayout(m_program.at(m_program.allocations[site].layout).kind) ? Stub::AllocateTypedObject : Stub::AllocateTypedCell;
    return CellPointer(callAllocationStub(stub, { { number.value(), stubTemporaryGPRs[0] } }, { Writes::RuntimeState, false }, false));
}

CellPointer Lowering::allocateObjectCopy(uint32_t site, CellPointer source, unsigned fieldBytes)
{
    constexpr unsigned chunkBytes = 16;
    constexpr unsigned maxBytesCopiedWithoutLoop = 64;
    RELEASE_ASSERT(fieldBytes && !(fieldBytes % chunkBytes));
    Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(site).value(), stubTemporaryGPRs[0] }, { source.value(), GPRInfo::argumentGPR1 } };
    Stub stub = Stub::CopyTypedObjectAnySize;
    if (fieldBytes <= maxBytesCopiedWithoutLoop)
        stub = static_cast<Stub>(static_cast<unsigned>(Stub::CopyTypedObject16) + 2 * (fieldBytes / chunkBytes - 1));
    else
        operands.append({ m_out.constInt64(fieldBytes - chunkBytes), copyEndOffsetGPR });
    return CellPointer(callAllocationStub(stub, operands, { Writes::RuntimeState, false }, false));
}

std::pair<CellPointer, Pointer> Lowering::allocateArray(Word32 length, Word32 vectorLength, LBasicBlock slowPath)
{
    LValue header = changing64(Instance::offsetOfNewArrayHeader(ArrayWithContiguous));
    orElse(m_out.notZero64(header), slowPath);
    LValue size = m_out.add(m_out.shl(m_out.zeroExtPtr(vectorLength.value()), m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size, slowPath), slowPath);
    applyEffects(storage->as<PatchpointValue>(), { Writes::RuntimeState, false });
    LValue butterfly = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(length.value(), butterfly, m_heaps.Butterfly_publicLength);
    m_out.store32(vectorLength.value(), butterfly, m_heaps.Butterfly_vectorLength);
    splatWords(butterfly, length.value(), vectorLength.value(), m_out.int64Zero, m_heaps.indexedContiguousProperties.atAnyIndex());
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(array, header);
    m_out.storePtr(butterfly, array, m_heaps.JSObject_butterfly);
    return { CellPointer(array), Pointer(butterfly) };
}

CellPointer Lowering::newArray(const Arguments& elements)
{
    auto outOfLine = [&] {
        auto [count, list] = argumentBuffer(elements);
        return AOT_TYPED_OPERATION(NewArray, count, list);
    };
    constexpr unsigned maxInlineLength = Program::maxInlineArrayLiteralLength;
    unsigned length = elements.size();
    bool isStaticallyCold = !m_isInLoop && !m_program.isHotOutsideLoops(m_id);
    if (length > maxInlineLength || (length > maxArrayElementsInRegisters && isStaticallyCold))
        return outOfLine();
    Vector<Boxed, 8> boxed;
    for (ValueID element : elements)
        boxed.append(in<Boxed>(element));
    if (length <= maxArrayElementsInRegisters) {
        Vector<std::pair<LValue, GPRReg>, 4> operands;
        for (unsigned i = 0; i < length; ++i)
            operands.append({ boxed[i].value(), GPRInfo::toArgumentRegister(1 + i) });
        return CellPointer(callAllocationStub(arrayLiteralStub(length), operands, effectsForOperation<operationAOTTypedNewArray>(), true));
    }
    unsigned vectorLength = std::max(length, length ? BASE_CONTIGUOUS_VECTOR_LEN : BASE_CONTIGUOUS_VECTOR_LEN_EMPTY) | 1;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto [array, butterfly] = allocateArray(word32(length), word32(vectorLength), slowPath);
    for (unsigned i = 0; i < length; ++i)
        m_out.store64(boxed[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(array.value());
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(outOfLine().value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

CellPointer Lowering::newArrayFromBuffer(Word32 count, Pointer buffer)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock copyElement = m_out.newBlock();
    LBasicBlock copied = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Word32 minVectorLength = select(isZero(count), word32(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY), word32(BASE_CONTIGUOUS_VECTOR_LEN));
    Word32 vectorLength = bitOr(select(below(minVectorLength, count), count, minVectorLength), word32(1));
    auto [array, butterfly] = allocateArray(count, vectorLength, slowPath);
    LValue end = m_out.shl(m_out.zeroExtPtr(count.value()), m_out.constInt32(3));
    ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
    m_out.jump(copyLoop);
    m_out.appendTo(copyLoop);
    LValue offset = m_out.phi(pointerType(), start);
    m_out.branch(m_out.below(offset, end), unsure(copyElement), unsure(copied));
    m_out.appendTo(copyElement);
    LValue element = m_out.load64(TypedPointer(m_heaps.AOTTyped_argumentBuffer, m_out.add(buffer.value(), offset)));
    m_out.store64(element, TypedPointer(m_heaps.indexedContiguousProperties.atAnyIndex(), m_out.add(butterfly.value(), offset)));
    m_out.addIncomingToPhi(offset, m_out.anchor(m_out.add(offset, m_out.intPtrEight)));
    m_out.jump(copyLoop);
    m_out.appendTo(copied);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(array.value());
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArray, count, buffer).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

Word32 Lowering::restLength(uint32_t numberOfArgumentsToSkip)
{
    Word32 count = m_argumentCount;
    if (!numberOfArgumentsToSkip)
        return count;
    Word32 skipped = word32(numberOfArgumentsToSkip);
    return select(below(skipped, count), Word32(m_out.sub(count.value(), skipped.value())), word32(0));
}

CellPointer Lowering::createRest(uint32_t numberOfArgumentsToSkip)
{
    return newArrayFromBuffer(restLength(numberOfArgumentsToSkip), offsetBy(m_argumentBuffer, integer(numberOfArgumentsToSkip), 3));
}

Boxed Lowering::lowerGetMyArgumentByVal(const Value& value)
{
    uint32_t skipped = value.numberOfArgumentsToSkip();
    ValueID key = value.child(0);
    bool isNumber = typeOf(key).isNumber();
    Rep rep = get(key).rep;
    LBasicBlock slowPath = isNumber ? nullptr : newColdBlock();
    Join join(m_out, Rep::JSValue, Int64);
    if (rep == Rep::Cell || rep == Rep::Int32)
        m_out.jump(slowPath);
    else {
        LBasicBlock outside = m_out.newBlock();
        Integer index = indexOrElse(key, isNumber ? outside : slowPath);
        if (index.value()->hasInt64() && static_cast<uint64_t>(index.value()->asInt64()) > maxArguments)
            m_out.jump(outside);
        else {
            orElse(below(index, zeroExtended(restLength(skipped))), outside);
            join.add(Boxed(m_out.load64(TypedPointer(m_heaps.AOTTyped_argumentBuffer, offsetBy(m_argumentBuffer, add(index, integer(skipped)), 3).value()))));
        }
        m_out.appendTo(outside);
        join.add(undefined());
    }
    if (isNumber)
        return join.finish().as<Boxed>();
    m_out.appendTo(slowPath);
    Boxed boxedKey = in<Boxed>(key);
    Boxed array = box(AOT_TYPED_OPERATION(NewArray, restLength(skipped), offsetBy(m_argumentBuffer, integer(skipped), 3)));
    if (value.readsArgumentsObject()) {
        LValue buffer = transientBuffer(2 * sizeof(EncodedJSValue));
        m_out.store64(array.value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, 0));
        m_out.store64(boxedKey.value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, sizeof(EncodedJSValue)));
        join.add(AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(LanguageOperation::GetFromArguments)), word32(2), Pointer(buffer)));
    } else
        join.add(emitGetByVal(array, boxedKey, false));
    return join.finish().as<Boxed>();
}

Boxed Lowering::lowerCallForwardVarargs(const Value& value)
{
    uint32_t skipped = value.numberOfArgumentsToSkip();
    return callValueWithList(in<Boxed>(value.child(0)), in<Boxed>(value.child(1)), restLength(skipped), offsetBy(m_argumentBuffer, integer(skipped), 3));
}

Boxed Lowering::keyConstant(StringID name)
{
    if (isConstantSymbol(m_program.string(name)))
        return box(AOT_TYPED_OPERATION(ConstantSymbol, word32(name.index())));
    return box(stringConstant(name));
}

CellPointer Lowering::stringConstant(StringID string)
{
    auto bytes = m_program.string(string);
    if (bytes.empty())
        return CellPointer(emptyString());
    if (bytes.size() == 1 && isASCII(bytes[0]))
        return CellPointer(singleCharacterString(m_out.constInt32(bytes[0])));
    uint32_t key = string.index() + 1;
    static constexpr uint64_t stringsKey = 1ull << 32;
    auto callStubInBlock = [&](Stub stub, Stub stubInBlock1, uint32_t number) {
        Stub whole = stub;
        uint32_t integralNumber = number;
        if (uint32_t block = number >> 16; isARM64() && block && block < numberOfTypedStringBlocks) {
            stub = static_cast<Stub>(static_cast<unsigned>(stubInBlock1) + typedStringStubsPerBlock * (block - 1));
            number &= 0xffff;
        }
        if (thunkFor(whole, integralNumber))
            return callAllocationStub(whole, { }, { Writes::Pure, false }, false, false, false, nullptr, integralNumber, stub, number);
        return callAllocationStub(stub, { { m_out.constInt64(number), lazyConstantKeyGPR } }, { Writes::Pure, false }, false);
    };
    if (m_program.stringsCreatedOnce.get(string.index()) && m_program.runsOnce(m_id) && !m_isInLoop) {
        LValue cell = callStubInBlock(Stub::TypedStringCached, Stub::TypedStringCachedInBlock1, string.index());
        cell->as<PatchpointValue>()->pureFunctionKey = stringsKey | key;
        return CellPointer(cell);
    }
    if (!m_isInLoop || Options::useAOTWideReferences()) {
        LValue cell = callStubInBlock(Stub::TypedString, Stub::TypedStringInBlock1, key);
        cell->as<PatchpointValue>()->pureFunctionKey = stringsKey | key;
        return CellPointer(cell);
    }
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { Writes::Pure, false });
    patchpoint->pureFunctionKey = stringsKey | key;
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    if (auto changed = registersClobberedBy(Stub::TypedString)) {
        if (!hasFramelessFunctionStubs) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        }
        patchpoint->clobberLate(*changed);
    } else {
        m_context.emitsCalls = true;
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        patchpoint->clobberLate(registersClobberedByCalls());
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
#endif
    }
    patchpoint->setGenerator([context = &m_context, key](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg result = GPRInfo::returnValueGPR;
        if (auto place = indexOfTypedStringWithWord(key)) {
#if CPU(ARM64)
            jit.m_assembler.add<64>(result, instanceGPR, UInt12(Instance::offsetOfTypedStringWord(*place) >> 12), 12);
            jit.load32(CCallHelpers::Address(result, Instance::offsetOfTypedStringWord(*place) & 0xfff), result);
            CCallHelpers::Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, result);
            jit.addLeftShift64(cellCageBaseGPR, result, CCallHelpers::TrustedImm32(cell32Shift), result);
#else
            jit.load32(CCallHelpers::Address(instanceGPR, Instance::offsetOfTypedStringWord(*place)), result);
            CCallHelpers::Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, result);
            jit.getEffectiveAddress(CCallHelpers::BaseIndex(cellCageBaseGPR, result, static_cast<CCallHelpers::Scale>(cell32Shift)), result);
#endif
            CCallHelpers::Label done = jit.label();
            bool isLeaf = hasFramelessFunctionStubs && context->hasNoFrame(params.proc().code());
            params.addNearbyLatePath(jit, [=](CCallHelpers& jit) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                isEmpty.link(&jit);
                if (isLeaf) {
                    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(key)), lazyConstantKeyGPR);
#if CPU(ARM64)
                    jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
                    context->stubCalls.call(jit, Stub::LeafTypedString, CallSite { });
                } else
                    context->stubCalls.call(jit, Stub::TypedString, key, CallSite { });
                jit.jump().linkTo(done, &jit);
            });
            return;
        }
#if CPU(X86_64)
        constexpr GPRReg slots = CCallHelpers::s_scratchRegister;
        constexpr GPRReg at = stubTemporaryGPRs[0];
#else
        constexpr GPRReg slots = stubTemporaryGPRs[2];
        constexpr GPRReg at = stubTemporaryGPRs[3];
#endif
        static_assert(noOverlap(result, slots, at, instanceGPR));
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfTypedStrings()), slots);
        jit.load32(CCallHelpers::Address(slots, offsetOfMaskInStringTable), at);
        jit.and32(CCallHelpers::TrustedImm32(static_cast<int32_t>(startInStringTable(key))), at);
        jit.load64(CCallHelpers::BaseIndex(slots, at, CCallHelpers::TimesEight), result);
        CCallHelpers::Jump hasKindAndCode = jit.branch32(CCallHelpers::NotEqual, result, CCallHelpers::TrustedImm32(static_cast<int32_t>(key)));
        jit.urshift64(CCallHelpers::TrustedImm32(32), result);
#if CPU(ARM64)
        jit.addLeftShift64(cellCageBaseGPR, result, CCallHelpers::TrustedImm32(cell32Shift), result);
#else
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(cellCageBaseGPR, result, static_cast<CCallHelpers::Scale>(cell32Shift)), result);
#endif
        CCallHelpers::Label done = jit.label();
        bool isLeaf = hasFramelessFunctionStubs && context->hasNoFrame(params.proc().code());
        params.addNearbyLatePath(jit, [=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            hasKindAndCode.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(key)), lazyConstantKeyGPR);
#if CPU(ARM64)
            if (isLeaf)
                jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
            context->stubCalls.call(jit, isLeaf ? Stub::LeafTypedString : Stub::TypedString, CallSite { });
            jit.jump().linkTo(done, &jit);
        });
    });
    return CellPointer(patchpoint);
}

void Lowering::poll()
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* limit = m_out.patchpoint(Int64);
    limit->effects = Effects::none();
    limit->effects.reads = HeapRange::top();
    limit->effects.writesLocalState = true;
    if (!m_proc.loopPollGenerator()) {
        m_proc.setLoopPollGenerator(createSharedTask<StackmapGeneratorFunction>([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfTrapAwareSoftStackLimit()), params[0].gpr());
        }));
    }
    limit->setGenerator(RefPtr<StackmapGenerator>(m_proc.loopPollGenerator()));
    static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);
    m_out.branch(m_out.lessThan(limit, m_out.int64Zero), rarely(slowPath), usually(continuation));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(PollSlow);
    if (auto* handler = m_out.m_block->last()->as<PatchpointValue>(); sizeOfLoopPoll && handler && !m_activeHandler)
        m_context.loopPollSites.add(handler->generator(), m_lastSite);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

LValue Lowering::transientBuffer(size_t bytes)
{
    if (!m_transientBuffer)
        m_transientBuffer = m_proc.addStackSlot(bytes);
    else
        m_transientBuffer->ensureSize(bytes);
    return m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), m_transientBuffer);
}

std::pair<Word32, Pointer> Lowering::argumentBuffer(const Arguments& arguments)
{
    Vector<LValue, 8> boxed;
    for (unsigned i = 0; i < arguments.size(); ++i)
        boxed.append(in<Boxed>(arguments[i]).value());
    LValue slot = transientBuffer(std::max<size_t>(arguments.size(), 1) * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < arguments.size(); ++i)
        m_out.store64(boxed[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, slot, i * sizeof(EncodedJSValue)));
    return { word32(arguments.size()), Pointer(slot) };
}

const Type& Lowering::declaredArgumentType(const Function& callee, unsigned index) const
{
    return m_program.declaredArgumentType(callee, index);
}

LValue Lowering::unusedParameterPlaceholder(Rep rep)
{
    LValue argument;
    if (b3TypeOf(rep) == Double)
        argument = m_out.m_block->appendNew<B3::ConstDoubleValue>(m_proc, m_out.origin(), 0);
    else if (b3TypeOf(rep) == Int32)
        argument = m_out.m_block->appendNew<B3::Const32Value>(m_proc, m_out.origin(), 0);
    else
        argument = m_out.m_block->appendNew<B3::Const64Value>(m_proc, m_out.origin(), 0);
    m_unusedParameterPlaceholders.add(argument);
    return argument;
}

Operand Lowering::callCompiled(FunctionID function, const Vector<Argument, 8>& arguments, const Type& result, LValue* multiResultCall, bool isCallBeingLowered)
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty())
        return failWith("a call of a function without a body"_s);
    unsigned numberOfParameters = callee.blocks[0].parameters.size();
    RELEASE_ASSERT(arguments.size() <= numberOfParameters);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Vector<LValue, 8> passed;
    LValue closure = nullptr;
    if (callee.takesCapturesFromClosure) {
        RELEASE_ASSERT((m_function.isGenericEntrypoint() && m_program.genericEntrypoints.get(function.index()) == m_id) || m_function.valueEntryTarget == function);
        closure = m_values[m_function.blocks[0].parameters[0].index()].value;
    }
    for (unsigned i = closure ? callee.captureTypes.size() : 0; i < numberOfParameters; ++i) {
        if (callee.unusedParameters.get(i)) {
            passed.append(unusedParameterPlaceholder(repOf(declaredArgumentType(callee, i))));
            continue;
        }
        if (callee.mayRunWithoutPromise && i + 1 == numberOfParameters) {
            RELEASE_ASSERT(arguments.size() < numberOfParameters);
            passed.append(m_callIsAwaitedWithoutPromise ? m_out.int64Zero : lowerNewPromise().value());
            continue;
        }
        Argument given = i < arguments.size() ? arguments[i] : Argument { undefined(), &undefinedType };
        passed.append(ensure(given.operand, *given.type, declaredArgumentType(callee, i)).value);
    }
    if (!callee.resultTypes.isEmpty()) {
        if (!multiResultCall)
            return failWith("a call with multiple results that are never read"_s);
        Vector<B3::Type> types;
        for (TypeID type : callee.resultTypes)
            types.append(b3TypeOf(repOf(m_program.at(type))));
        *multiResultCall = callFunction(function, m_proc.addTuple(WTF::move(types)), passed);
        return ensure(undefined(), undefinedType, result);
    }
    Rep rep = resultRepOf(m_program, function);
    if (mayJumpTo(function, passed)) {
        jumpToFunction(calleeIndex(function), passed, closure);
        return placeholder(repOf(result));
    }
    if (isLastCallOfGenericEntrypoint(function) && !m_activeHandler) {
        callFromCollapsibleFrame(function, passed, closure);
        return placeholder(repOf(result));
    }
    bool leavesDroppableFrame = isCallBeingLowered && callee.isCalledFromDroppableFrame && (m_leavesDroppableFrame || (m_tailCall && isWidenedInIndirectTailCallCycle()));
    if (isCallBeingLowered && m_tailCall && !leavesDroppableFrame) {
        unsigned numberOfGPRs = 0;
        unsigned numberOfFPRs = 0;
        for (LValue argument : passed)
            ++(argument->type() == Double ? numberOfFPRs : numberOfGPRs);
        if (canTailCallDirectly(function, numberOfGPRs, numberOfFPRs, result, m_tailCallReturnsArgument)) {
            if (function == m_id && m_selfTailCallLoopHeader && passed.size() == m_selfTailCallPhis.size()) {
                poll();
                for (unsigned i = 0; i < passed.size(); ++i)
                    m_out.addIncomingToPhi(m_selfTailCallPhis[i], m_out.anchor(passed[i]));
                m_out.jump(m_selfTailCallLoopHeader);
                m_out.appendTo(m_out.newBlock());
                return placeholder(repOf(result));
            }
            didEmitTailCall();
            jumpToFunction(calleeIndex(function), passed);
            return placeholder(repOf(result));
        }
    }
    if (isCallBeingLowered && m_dropsDroppableCallerFrame && mayJumpToWidenedFunction(function)) {
        LBasicBlock dropsFrames = m_out.newBlock();
        LBasicBlock staysInFrame = m_out.newBlock();
        m_out.branch(isCalledFromDroppableFrame().value(), unsure(dropsFrames), unsure(staysInFrame));
        m_out.appendTo(dropsFrames);
        didEmitTailCall();
        jumpToFunction(calleeIndex(function), passed, nullptr, true);
        m_out.appendTo(staysInFrame);
    }
    LValue value = callFunction(function, b3TypeOf(rep), passed, { }, closure, leavesDroppableFrame || callsFromDroppableFrame(function));
    if (rep == Rep::Void)
        return ensure(undefined(), undefinedType, result);
    if (callee.mayRunWithoutPromise && !m_callIsAwaitedWithoutPromise)
        return ensure(CellPointer(value), Type::engineClass(m_builtins.promiseClass()), result);
    if (!callee.callerResultCasts.isEmpty()) {
        Operand converted(rep, value);
        const Type* type = &m_program.at(m_program.types.anything);
        for (auto& cast : callee.callerResultCasts) {
            auto& to = m_program.at(cast.type);
            if (cast.castReason() == CastReason::Coerce)
                converted = lowerCoercion(converted, *type, to, cast, OperationEffects { });
            else if (cast.castReason() == CastReason::As || (cast.castReason() == CastReason::Flow && !cast.string() && (cast.keepsUnconvertibleObjects() || narrowingMayRetag(m_program, to))))
                converted = coerce(converted, *type, cast, OperationEffects { });
            else
                converted = ensure(converted, *type, to);
            type = &to;
        }
        return ensure(converted, *type, result);
    }
    if (callee.resultIsCoercedByCallers) {
        Value cast;
        cast.opcode = Opcode::Cast;
        cast.type = callee.resultTypeCheckedByCallers;
        cast.rep = repOf(m_program.at(cast.type));
        cast.immediates[1] = static_cast<uint32_t>(CastReason::As);
        return ensure(coerce(Operand(rep, value), m_program.at(m_program.types.anything), cast, OperationEffects { }), m_program.at(cast.type), result);
    }
    return ensure(Operand(rep, value), m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), result);
}

Operand Lowering::jumpToContinuation(const Value& value)
{
    Vector<LValue, 8> passed;
    for (ValueID id : value.arguments)
        passed.append(in(id, repOf(m_program.at(m_function.at(id).type))).value);
    for (ValueID id : value.arguments) {
        RELEASE_ASSERT_WITH_MESSAGE(!m_function.at(originOf(id)).isStackAllocatedClosure() && !m_function.stackAllocatedEnvironments.get(originOf(id).index()), "AOT: a stack-allocated object is passed to a continuation");
    }
    m_context.knownCallees.append(continuationKey(m_id, value.continuation()));
    jumpToFunction(m_context.knownCallees.size() - 1, passed);
    return placeholder(value.rep);
}

std::optional<Operand> Lowering::readHostObjectMember(const Value& call)
{
    if (!m_function.cachedHostObjectName || resultRepOf(m_program, call.function()) != Rep::JSValue || resultRepOf(m_program, m_id) != Rep::JSValue)
        return std::nullopt;
    uint32_t cacheNumber = m_program.globalMemberCacheNumber(m_function.cachedHostObjectName, m_function.name);
    if (!cacheNumber)
        return std::nullopt;
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LValue cached = globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value));
    m_out.branch(m_out.notZero64(cached), usually(hit), rarely(miss));
    m_out.appendTo(miss);
    jumpToFunction(calleeIndex(call.function()), { });
    m_out.unreachable();
    m_out.appendTo(hit);
    return Operand(Boxed(cached));
}

std::optional<Operand> Lowering::readHostObjectMemberInline(ValueID result, const Value& call)
{
    auto& reader = m_program.at(call.function());
    if (!reader.cachedHostObjectName || m_out.m_block->frequency() < 1 || prefersGenericPathInColdCode())
        return std::nullopt;
    uint32_t cacheNumber = m_program.globalMemberCacheNumber(reader.cachedHostObjectName, reader.name);
    if (!cacheNumber || resultRepOf(m_program, reader.genericHostObjectReader) != Rep::JSValue)
        return std::nullopt;
    LBasicBlock miss = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue cached = globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value));
    ValueFromBlock known = m_out.anchor(cached);
    m_out.branch(m_out.notZero64(cached), usually(continuation), rarely(miss));
    m_out.appendTo(miss);
    ValueFromBlock found = m_out.anchor(callFunction(reader.genericHostObjectReader, Int64, { }));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return ensure(Operand(Boxed(m_out.phi(Int64, known, found))), m_program.at(m_program.types.anything), typeOf(result));
}

std::optional<Operand> Lowering::readAsyncLocalStorage(const Value& call)
{
    if (m_function.genericAsyncLocalStorageReader != call.function() || resultRepOf(m_program, call.function()) != Rep::JSValue || resultRepOf(m_program, m_id) != Rep::JSValue)
        return std::nullopt;
    auto& parameterTypes = m_program.at(call.function()).parameterTypes;
    if (call.arguments.size() != 1 || parameterTypes.size() != 1 || repOf(m_program.at(parameterTypes[0])) != Rep::JSValue)
        return std::nullopt;
    enum : unsigned { Storage, StoredValue, Previous, Masked };
    auto placeIn = [&](Boxed frame, unsigned place) {
        return m_out.address(m_heaps.properties.atAnyNumber(), frame.value(), JSObject::offsetOfInlineStorage() + place * sizeof(EncodedJSValue));
    };
    Boxed storage = in<Boxed>(call.arguments[0]);
    LBasicBlock miss = newUnlikelyBlock();
    LBasicBlock hasContext = m_out.newBlock();
    LBasicBlock look = m_out.newBlock();
    LBasicBlock goOn = m_out.newBlock();
    LBasicBlock hasPrevious = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock isNotBound = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    orElse(isCell(storage), miss);
    LBasicBlock hasOtherStructure = m_out.newBlock();
    LBasicBlock isStorage = m_out.newBlock();
    Word32 storageStructureID = loadField<Word32>(asCell(storage), m_heaps.JSCell_structureID);
    m_out.branch(equal(storageStructureID, Word32(loadRuntimeState32(Instance::offsetOfAsyncLocalStorageStructureID(0)))).value(), unsure(isStorage), unsure(hasOtherStructure));
    m_out.appendTo(hasOtherStructure);
    orElse(equal(storageStructureID, Word32(loadRuntimeState32(Instance::offsetOfAsyncLocalStorageStructureID(1)))), miss);
    m_out.jump(isStorage);
    m_out.appendTo(isStorage);
    Word32 frameStructureID(loadRuntimeState32(Instance::offsetOfAsyncContextFrameStructureID()));
    auto isFrameOrElse = [&](Boxed value) {
        orElse(isCell(value), miss);
        orElse(equal(loadField<Word32>(asCell(value), m_heaps.JSCell_structureID), frameStructureID), miss);
    };
    LValue context = m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfGlobalObject()), JSGlobalObject::offsetOfAsyncContextData()));
    Boxed first(m_out.load64(m_out.address(context, m_heaps.JSInternalFieldObjectImpl_internalFields[0])));
    m_out.branch(equal(first, undefined()).value(), unsure(isNotBound), unsure(hasContext));

    m_out.appendTo(hasContext);
    isFrameOrElse(first);
    orElse(equal(Boxed(m_out.load64(placeIn(first, Masked))), undefined()), miss);
    ValueFromBlock start = m_out.anchor(first.value());
    m_out.jump(look);

    m_out.appendTo(look);
    Boxed frame(m_out.phi(Int64, start));
    m_out.branch(equal(Boxed(m_out.load64(placeIn(frame, Storage))), storage).value(), unsure(hit), unsure(goOn));

    m_out.appendTo(goOn);
    Boxed previous(m_out.load64(placeIn(frame, Previous)));
    m_out.branch(equal(previous, undefined()).value(), unsure(isNotBound), unsure(hasPrevious));

    m_out.appendTo(hasPrevious);
    isFrameOrElse(previous);
    m_out.addIncomingToPhi(frame.value(), m_out.anchor(previous.value()));
    m_out.jump(look);

    m_out.appendTo(miss);
    jumpToFunction(calleeIndex(call.function()), { storage.value() });
    m_out.unreachable();

    m_out.appendTo(isNotBound);
    LValue defaultValueSlot = m_out.add(storage.value(), m_out.zeroExtPtr(loadRuntimeState32(Instance::offsetOfAsyncLocalStorageDefaultValueOffset())));
    ValueFromBlock defaultValue = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), defaultValueSlot)));
    m_out.jump(continuation);

    m_out.appendTo(hit);
    ValueFromBlock bound = m_out.anchor(m_out.load64(placeIn(frame, StoredValue)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(Boxed(m_out.phi(Int64, bound, defaultValue)));
}

bool Lowering::isModuleEvaluatedBefore(ValueID evaluation, uint32_t row)
{
    if (!m_moduleEvaluationSites) {
        m_moduleEvaluationSites.emplace();
        for (unsigned block = 0; block < m_function.blocks.size(); ++block) {
            auto& values = m_function.blocks[block].values;
            for (unsigned index = 0; index < values.size(); ++index) {
                auto& call = m_function.at(values[index]);
                if (call.opcode != Opcode::CallDirect)
                    continue;
                if (auto found = m_program.moduleEvaluationIndexByFunction.find(call.function().index()); found != m_program.moduleEvaluationIndexByFunction.end())
                    m_moduleEvaluationSites->append({ values[index], found->value, BlockID(block), index });
            }
        }
    }
    unsigned ownIndex = 0;
    for (auto& site : *m_moduleEvaluationSites) {
        if (site.call == evaluation)
            ownIndex = site.index;
    }
    for (auto& other : *m_moduleEvaluationSites) {
        if (other.row != row || other.call == evaluation)
            continue;
        auto& block = m_function.blocks[other.block.index()];
        if (block.terminator.kind == TerminatorKind::Invoke && other.index + 1 == block.values.size())
            continue;
        if (other.block == m_currentBlockID ? other.index < ownIndex : dominates(other.block, m_currentBlockID))
            return true;
    }
    return false;
}

Operand Lowering::lowerCallDirect(ValueID result, const Value& value)
{
    if (value.isJumpToContinuation())
        return jumpToContinuation(value);
    if (auto read = readHostObjectMember(value))
        return *read;
    if (auto read = readHostObjectMemberInline(result, value))
        return *read;
    if (auto read = readAsyncLocalStorage(value))
        return *read;
    if (auto row = m_program.moduleEvaluationIndexByFunction.find(value.function().index()); row != m_program.moduleEvaluationIndexByFunction.end()) {
        LType returnedValue = b3TypeOf(resultRepOf(m_program, m_program.moduleEvaluator));
        if (isModuleEvaluatedBefore(result, row->value))
            return convert(undefined(), typeOf(result), value.rep);
        if (!m_out.m_block->frequency()) {
            callFunction(m_program.moduleEvaluatorIfNeeded, returnedValue, { m_out.constInt64(row->value) }, value.function());
            return convert(undefined(), typeOf(result), value.rep);
        }
        LBasicBlock evaluate = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue isEvaluated = marked(m_out.load8ZeroExt32(addressOf(m_program.moduleEvaluations[row->value].isEvaluated)), IsSafe);
        m_out.branch(m_out.equal(isEvaluated, m_out.constInt32(static_cast<uint8_t>(JSValue::encode(jsBoolean(true))))), usually(continuation), rarely(evaluate));
        inColdBlock(evaluate, [&] {
            callFunction(m_program.moduleEvaluatorForExternalCall(), returnedValue, { m_out.constInt64(row->value) }, value.function());
            m_out.jump(continuation);
        });
        m_out.appendTo(continuation);
        return convert(undefined(), typeOf(result), value.rep);
    }
    if (!value.arguments.isEmpty() && isPassedInMemory(value.arguments.last())) {
        auto& callee = m_program.at(value.function());
        bool hasThis = callee.has(Format::FunctionHasThis);
        Arguments passed;
        passed.append(value.arguments.span().subspan(hasThis, value.arguments.size() - hasThis - 1));
        passed.appendVector(m_function.at(value.arguments.last()).arguments);
        Boxed returnedValue = callListEntry(m_listEntryByCall.get(result.index()), m_out.int64Zero, hasThis ? in<Boxed>(value.arguments[0]) : undefined(), passed);
        return ensure(returnedValue, m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), typeOf(result));
    }
    if (m_program.at(value.function()).kind == FunctionKind::Module && value.arguments.isEmpty() && !useCountOf(result) && m_out.m_block->frequency() < 1) {
        callFunctionFromColdBlock(value.function());
        return placeholder(value.rep);
    }
    if (auto& callee = m_program.at(value.function()); callee.kind == FunctionKind::Module && value.arguments.isEmpty() && callee.resultType && callee.resultTypes.isEmpty() && !callee.blocks.isEmpty() && m_out.m_block->frequency() < 1) {
        if (Rep rep = resultRepOf(m_program, value.function()); rep == Rep::Cell || rep == Rep::JSValue)
            return ensure(Operand(rep, callFunctionFromColdBlock(value.function(), true)), m_program.at(callee.resultType), typeOf(result));
    }
    Vector<Argument, 8> arguments;
    for (ValueID id : value.arguments)
        arguments.append(argument(id));
    LValue call = nullptr;
    SetForScope isAwaitedWithoutPromise(m_callIsAwaitedWithoutPromise, value.isAwaitedWithoutPromise);
    Operand returnedValue = callCompiled(value.function(), arguments, typeOf(result), &call, true);
    if (call)
        m_multiResultCalls.add(result.index(), call);
    return returnedValue;
}

Vector<B3::ValueRep, 4> Lowering::resultRegisters(const Function& function) const
{
    Vector<B3::ValueRep, 4> result;
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (TypeID type : function.resultTypes)
        result.append(b3TypeOf(repOf(m_program.at(type))) == Double ? ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) : ValueRep::reg(argumentGPR(nextGPR++)));
    return result;
}

void Lowering::returnResults(const Value& results)
{
    auto& types = m_function.resultTypes;
    RELEASE_ASSERT(results.opcode == Opcode::Results && results.arguments.size() == types.size());
    auto registers = resultRegisters(m_function);
    Vector<LValue, 4> values;
    for (unsigned i = 0; i < types.size(); ++i)
        values.append(ensure(get(results.arguments[i]), typeOf(results.arguments[i]), m_program.at(types[i])).value);
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    for (unsigned i = 0; i < types.size(); ++i)
        patchpoint->append(ConstrainedValue(values[i], registers[i]));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setGenerator([context = &m_context](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->emitFrameTeardown(jit, params.proc().code());
        jit.ret();
    });
}

Operand Lowering::lowerResultAt(const Value& value)
{
    auto& callee = m_program.at(m_function.at(value.child(0)).function());
    auto& type = m_program.at(callee.resultTypes[value.resultIndex()]);
    Rep rep = repOf(type);
    LValue extracted = m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), b3TypeOf(rep), m_multiResultCalls.get(value.child(0).index()), static_cast<int32_t>(value.resultIndex()));
    return ensure(Operand(rep, extracted), type, m_program.at(value.type));
}

FunctionID Lowering::genericEntrypointFor(FunctionID function)
{
    auto found = m_program.genericEntrypoints.find(function.index());
    if (found == m_program.genericEntrypoints.end()) {
        fail("a function value without a generic entrypoint"_s);
        return function;
    }
    return found->value;
}

static unsigned referenceCountAmong(const Program& program, std::span<const TypeID> types)
{
    unsigned count = 0;
    for (TypeID type : types)
        count += holdsCells(repOf(program.at(type)));
    return count;
}

static unsigned captureSlot(const Program& program, std::span<const TypeID> types, unsigned index)
{
    if (holdsCells(repOf(program.at(types[index]))))
        return referenceCountAmong(program, types.first(index));
    return referenceCountAmong(program, types) + index - referenceCountAmong(program, types.first(index));
}

Operand Lowering::closureCapture(CellPointer closure, const Function& callee, unsigned index)
{
    Rep rep = repOf(m_program.at(callee.captureTypes[index]));
    auto& word = (m_program.usesSlimClosures(callee) ? m_heaps.AOTTypedSlimClosure_captures : m_heaps.AOTTypedClosure_captures)[captureSlot(m_program, callee.captureTypes.span(), index)];
    if (b3TypeOf(rep) == Double)
        return Operand(rep, marked(m_out.loadDouble(closure.value(), word), IsFixed));
    if (rep == Rep::Int32)
        return Operand(rep, marked(m_out.load32(closure.value(), word), IsFixed));
    return Operand(rep, marked(m_out.load64(closure.value(), word), IsFixed));
}

CellPointer Lowering::closureConstant(FunctionID function)
{
    uint32_t general = genericEntrypointFor(function).index();
    return lazilyInitialized<CellPointer>(Stub::TypedClosure, Instance::offsetOfTypedClosures(), general, [&] {
        return AOT_TYPED_OPERATION(ClosureConstant, word32(general));
    });
}

CellPointer Lowering::lowerNewClosure(const Value& value, LValue cachedClosureVariable, Store variableStore, bool* isStoredByStub)
{
    auto& callee = m_program.at(value.function());
    bool isResumeClosure = m_program.isResumeFunction(callee);
    FunctionID general = isResumeClosure ? value.function() : genericEntrypointFor(value.function());
    Vector<TypeID, 8> types;
    if (callee.kind == FunctionKind::Constructor)
        types.append(m_function.at(value.arguments[0]).type);
    else
        types.appendVector(callee.captureTypes);
    RELEASE_ASSERT(types.size() == value.arguments.size() && types.size() <= Closure::maxNumberOfCaptures);
    Vector<Operand, 8> captures;
    for (unsigned i = 0; i < types.size(); ++i)
        captures.append(ensure(get(value.arguments[i]), typeOf(value.arguments[i]), m_program.at(types[i])));
    RELEASE_ASSERT(m_program.closureReferenceAndCaptureCounts(callee) == std::pair(referenceCountAmong(m_program, types.span()), static_cast<unsigned>(types.size())));
    bool isStackAllocated = value.isStackAllocatedClosure();
    auto callStub = [&](Vector<std::pair<LValue, GPRReg>, 4>& operands, size_t size) {
        if (isStackAllocated) {
            m_context.emitsCalls = true;
            operands.append({ m_out.lockedStackSlot(size), GPRInfo::returnValueGPR });
        }
        if (cachedClosureVariable && !isStackAllocated && types.size() == 1 && captures[0].rep == Rep::Cell && value.arguments[0] == value.child(0) && (variableStore == Store::JSValue || variableStore == Store::OptionalCell32) && !Options::validateAOTTypedFacts()) {
            *isStoredByStub = true;
            operands.append({ cachedClosureVariable, GPRInfo::toArgumentRegister(3) });
            if (Options::verboseAOTCompilation())
                dataLogLn("AOT: f", m_id.index(), ": a cached closure is stored by the stub that creates it");
            return CellPointer(callAllocationStub(variableStore == Store::OptionalCell32 ? Stub::NewTypedClosureOnce32 : Stub::NewTypedClosureOnce, operands, { Writes::RuntimeState, false }, false));
        }
        return CellPointer(callAllocationStub(static_cast<Stub>(static_cast<unsigned>(isStackAllocated ? Stub::InitializeTypedClosure0 : Stub::NewTypedClosure0) + 2 * types.size()), operands, { Writes::RuntimeState, false }, false));
    };
    if (m_program.usesSlimClosures(callee)) {
        auto& entrypoint = m_program.at(general);
        uint64_t request = general.index() | static_cast<uint64_t>(slimClosureKindOf(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs)) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
        if (!isStackAllocated && Options::countAOTTypedAllocations()) [[unlikely]]
            AOT_TYPED_OPERATION(CountClosure, word32(general.index()), word32(types.size()));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(static_cast<int64_t>(request)).value(), stubTemporaryGPRs[0] } };
        for (unsigned i = 0; i < types.size(); ++i) {
            LValue word = captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : b3TypeOf(captures[i].rep) == Double ? m_out.bitCast(captures[i].value, Int64) : captures[i].value;
            operands.append({ word, GPRInfo::toArgumentRegister(2 + captureSlot(m_program, types.span(), i)) });
        }
        return callStub(operands, SlimClosure::allocationSize(types.size()));
    }
    bool isCreatedByStub = !isResumeClosure && callee.kind != FunctionKind::Constructor && !closureStructureOf(callee.declaredAs) && types.size() <= ClosureRequest::maxNumberOfCaptures
        && !Options::countAOTTypedAllocations();
    for (auto& capture : captures)
        isCreatedByStub &= b3TypeOf(capture.rep) != Double;
    if (isCreatedByStub) {
        auto& entrypoint = m_program.at(general);
        uint64_t request = ClosureRequest::encode(general.index(), FunctionTables::executableKind(static_cast<uint8_t>(entrypoint.kind) | entrypoint.declaredAs, false), isStackAllocated ? 0 : m_program.closureAllocation(types.size()));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(static_cast<int64_t>(request)).value(), stubTemporaryGPRs[0] } };
        for (unsigned i = 0; i < types.size(); ++i)
            operands.append({ captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : captures[i].value, GPRInfo::toArgumentRegister(2 + captureSlot(m_program, types.span(), i)) });
        return callStub(operands, Closure::allocationSize(types.size()));
    }
    Pointer entry(marked(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedFunctions()), static_cast<ptrdiff_t>(general.index()) * sizeof(uint64_t))), IsFixed));
    auto& closureEntrypoint = m_program.at(general);
    CellPointer executable(fixedPointer(Instance::offsetOfTypedExecutable(FunctionTables::executableKind(static_cast<uint8_t>(closureEntrypoint.kind) | closureEntrypoint.declaredAs, false))));
    if (isResumeClosure)
        entry = Pointer(m_out.bitOr(entry.value(), m_out.constIntPtr(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        if (!isStackAllocated)
            AOT_TYPED_OPERATION(CountClosure, word32(general.index()), word32(types.size()));
    CellPointer cell = isStackAllocated ? stackAllocatedCell(Closure::allocationSize(types.size())) : allocate(m_program.closureAllocation(types.size()));
    if (isStackAllocated)
        storeHeader(cell.value(), m_out.bitOr(m_out.zeroExtPtr(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID())), m_out.constInt64(static_cast<int64_t>(static_cast<uint64_t>(TypeInfoBlob(NonArray, TypeInfo(JSFunctionType, Closure::StructureFlags)).blob()) << 32))));
    if (unsigned which = callee.kind == FunctionKind::Constructor ? classObjectClosureStructure : closureStructureOf(callee.declaredAs))
        storeField(cell, m_heaps.JSCell_structureID, Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID(which))));
    storeField(cell, m_heaps.JSObject_butterfly, integer(0));
    storeField(cell, m_heaps.JSCallee_scope, CellPointer(fixedPointer(Instance::offsetOfGlobalObject())));
    storeField(cell, m_heaps.JSFunction_executableOrRareData, executable);
    for (unsigned i = 0; i < types.size(); ++i) {
        auto& word = m_heaps.AOTTypedClosure_captures[captureSlot(m_program, types.span(), i)];
        if (b3TypeOf(captures[i].rep) == Double)
            m_out.storeDouble(captures[i].value, cell.value(), word);
        else
            m_out.store64(captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : captures[i].value, cell.value(), word);
    }
    storeField(cell, m_heaps.AOTTypedClosure_entry, entry);
    if (isStackAllocated)
        return cell;
    fenceAfterInitialization();
    return cell;
}

CellPointer Lowering::lowerNewClosureOnce(const Value& value)
{
    CellPointer environment = in<CellPointer>(value.child(0));
    auto& field = m_program.at(typeOf(value.child(0)).singleLayout()).fields[value.field()];
    RELEASE_ASSERT(repOf(field.store) == Rep::JSValue && !field.isOutOfLine);
    TypedPointer variable = m_out.address(m_heaps.AOTTyped_runtimeState, environment.value(), field.offset);
    LBasicBlock isNotCreated = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    Boxed held = load(field.store, variable, 0, environment.value()).as<Boxed>();
    ValueFromBlock created = m_out.anchor(held.value());
    m_out.branch(isCell(held).value(), unsure(continuation), unsure(isNotCreated));
    m_out.appendTo(isNotCreated);
    bool isStoredByStub = false;
    CellPointer closure = lowerNewClosure(value, m_out.add(environment.value(), m_out.constIntPtr(field.offset)), field.store, &isStoredByStub);
    didEmitWrite(Writes::RuntimeState);
    if (!isStoredByStub) {
        store(field.store, box(closure), variable);
        barrier(environment);
    }
    ValueFromBlock fresh = m_out.anchor(closure.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, created, fresh));
}

void Lowering::checkCallableBeforeTailCall(Boxed& callee, StringID methodName)
{
    SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
    LBasicBlock isCellBlock = m_out.newBlock();
    LBasicBlock isUnusual = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(callee).value(), usually(isCellBlock), rarely(isUnusual));
    m_out.appendTo(isCellBlock);
    m_out.branch(hasTypeOfFunctionFlag(asCell(callee)).value(), usually(continuation), rarely(isUnusual));
    inColdBlock(isUnusual, [&] {
        AOT_TYPED_OPERATION(ThrowUnlessCallable, box(asCell(callee)), word32(methodName ? static_cast<int32_t>(methodName.index()) : -1));
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    callee = box(asCell(callee));
}

Boxed Lowering::callGeneric(Boxed callee, Boxed thisValue, const Arguments& arguments, StringID methodName, SpeculatedMethod method, bool isKnownToBeFunction)
{
    std::optional<StringID> nameForCallableCheck;
    if (method == SpeculatedMethod::None && isLoweringCallInTailPosition()) {
        if (!isKnownToBeFunction) {
            if (!m_activeHandler && !m_dropsDroppableCallerFrame && arguments.size() <= numberOfArgumentGPRs && thrownNullish(callee).value() == callee.value())
                nameForCallableCheck = methodName;
            else
                checkCallableBeforeTailCall(callee, methodName);
        }
        methodName = { };
    }
    bool passesNameInRegister = methodName && method == SpeculatedMethod::None && arguments.size() <= numberOfArgumentGPRs && methodName.index() <= ThrowSite::maxNarrowA && thrownNullish(callee).value() == callee.value();
    if (methodName && !passesNameInRegister) {
        requireOrThrowAt(isCell(callee), ThrowKind::NotAFunction, Operand(thrownNullish(callee)), methodName.index());
        callee = box(asCell(callee));
    }
    if (arguments.size() > numberOfArgumentGPRs) {
        if (methodName) {
            requireOrThrowAt(isCallable(asCell(callee)), ThrowKind::NotAFunction, Operand(callee), methodName.index());
        }
        auto [count, list] = argumentBuffer(arguments);
        return callValueWithList(callee, thisValue, count, list);
    }
    Vector<LValue, 8> operands { callee.value(), thisValue.value() };
    for (ValueID argument : arguments)
        operands.append(genericCallArgument(argument).value());
    return callValue(Stub::CallValue, operands, { }, nullptr, method, passesNameInRegister ? methodName : StringID(), nameForCallableCheck);
}

Boxed Lowering::copyClosureToHeap(Boxed closure)
{
    Vector<std::pair<LValue, GPRReg>, 4> operands { { closure.value(), GPRInfo::argumentGPR1 } };
    LValue copy = callAllocationStub(static_cast<Stub>(static_cast<unsigned>(Stub::CopySlimClosure0) + m_function.copiedArgumentCaptureCount), operands, { Writes::RuntimeState, false }, false);
    return either<Boxed>(Word32(m_out.notNull(copy)), [&] {
        return Boxed(copy);
    }, [&] {
        return Boxed(AOT_TYPED_OPERATION(CopyClosureOutOfFrame, closure));
    });
}

Boxed Lowering::genericCallArgument(ValueID id)
{
    return id == m_argumentCopiedToHeap ? m_heapCopy : in<Boxed>(id);
}

Boxed Lowering::callMethodGeneric(Boxed object, StringID name, const Arguments& arguments)
{
    bool isReadThroughSlot = name != m_length && !m_builtins.callOrApply(m_program.builtinName(name)) && !bindIfNamed(name) && !bufferAccessorNamed(name) && !isNamedLastIndex(name);
    if (isReadThroughSlot && arguments.size() <= numberOfArgumentGPRs && !isLoweringCallInTailPosition()) {
        if (auto number = tableIndexOfName(name)) {
            if (auto method = collectionMethodNamed(m_program.string(name)); method != CollectionMethod::None)
                setCollectionMethodNamed(*number, method);
            return callMethodThroughSlot(object, *number, arguments, SpeculatedMethod::None, true);
        }
        if (auto slot = allocateSite(name, 0))
            return callMethodThroughSlot(object, *slot, arguments);
    }
    if (isX86() && isReadThroughSlot && arguments.size() <= numberOfArgumentGPRs && isLoweringCallInTailPosition() && canTailCallValue() && !m_activeHandler) {
        if (auto number = tableIndexOfName(name))
            return tailCallMethodByName(object, *number, arguments);
    }
    Boxed method;
    {
        SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
        method = emitGetById(object, name, true);
    }
    return callGeneric(method, object, arguments, name);
}

Boxed Lowering::tailCallMethodByName(Boxed object, unsigned nameIndex, const Arguments& arguments)
{
    m_context.emitsCalls = true;
    m_context.tailCallsNeedFrame = true;
    unsigned count = arguments.size();
    CallSite site;
    {
        SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
        site = { nextSite() };
    }
    Vector<LValue, 8> operands;
    for (ValueID argument : arguments)
        operands.append(genericCallArgument(argument).value());
    LValue name = m_out.constInt64(nameIndex | PropertyLookupTable::hasKindAndCode);
    PatchpointValue* jump = m_out.patchpoint(Void);
    jump->append(ConstrainedValue(name, ValueRep::reg(calleeGPR)));
    jump->append(ConstrainedValue(object.value(), ValueRep::reg(thisGPR)));
    for (unsigned i = 0; i < count; ++i)
        jump->append(ConstrainedValue(operands[i], ValueRep::reg(argumentGPR(i))));
    jump->clobber(RegisterSet::macroClobberedGPRs());
    jump->effects.terminal = true;
    jump->setGenerator([context = &m_context, count, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->stubCalls.call(jit, Stub::FindMethodByName, site);
        context->emitFrameTeardown(jit, params.proc().code());
        context->stubCalls.tailCall(jit, Stub::CallValue, count);
    });
    m_out.appendTo(m_out.newBlock());
    return Boxed(m_out.int64Zero);
}

Boxed Lowering::callMethodThroughSlot(Boxed object, unsigned slot, const Arguments& arguments, SpeculatedMethod method, bool slotIsNameIndex)
{
    m_context.emitsCalls = true;
    unsigned count = arguments.size();
    Vector<LValue, 8> operands;
    for (ValueID argument : arguments)
        operands.append(genericCallArgument(argument).value());
    RELEASE_ASSERT(!slotIsNameIndex || method == SpeculatedMethod::None);
    bool methodHasDedicatedStub = slotIsNameIndex && methodCallHasDedicatedStub(count, slot);
    LValue address = methodHasDedicatedStub ? nullptr : slotIsNameIndex ? m_out.constInt64(slot | PropertyLookupTable::hasKindAndCode) : slotAddress(slot, SlotAddressUse::FastPath).value();
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
    if (address)
        patchpoint->append(ConstrainedValue(address, ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(object.value(), ValueRep::reg(thisGPR)));
    for (unsigned i = 0; i < count; ++i)
        patchpoint->append(ConstrainedValue(operands[i], ValueRep::reg(argumentGPR(i))));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite callSite { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, count, callSite, slotIsNameIndex, methodHasDedicatedStub, slot, asks = stubForSpeculatedMethod(method, false)](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (methodHasDedicatedStub)
            stubCalls->callMethodWithDedicatedStub(jit, count, slot, callSite);
        else if (asks)
            stubCalls->call(jit, *asks, callSite);
        else
            stubCalls->call(jit, slotIsNameIndex ? Stub::CallMethodByName : Stub::CallMethod, count, callSite);
    });
    unwindToLandingPad(patchpoint, callSite);
    return Boxed(patchpoint);
}

bool Lowering::canCallInsteadOfClosure(FunctionID function, unsigned numberOfArguments) const
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty() || callee.isGenericEntrypoint() || callee.takesCalleeClosure || callee.takesCapturesFromClosure || callee.rampForClosures || !m_program.genericEntrypoints.contains(function.index()))
        return false;
    if (callee.kind == FunctionKind::Constructor || callee.kind == FunctionKind::Module)
        return false;
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    if (hasRest && callee.parameterTypes.isEmpty())
        return false;
    auto& undefinedType = m_program.at(m_program.types.undefined);
    for (unsigned i = 0; i < callee.parameterTypes.size(); ++i) {
        TypeID type = callee.parameterTypes[i];
        if (!type)
            continue;
        if (hasRest && i + 1 == callee.parameterTypes.size()) {
            if (!Type::engineClass(m_builtins.arrayClass()).isSubsetOf(m_program.at(type), m_program))
                return false;
        } else if (i >= numberOfArguments && !undefinedType.isSubsetOf(m_program.at(type), m_program))
            return false;
        if (callee.parameterCastReason(i) != CastReason::As)
            continue;
        for (TypeID elements = m_program.at(type).elements; elements; elements = m_program.at(elements).elements) {
            if (!m_program.at(elements).objectLayouts.isEmpty())
                return false;
        }
    }
    return true;
}

Operand Lowering::callClosure(FunctionID function, CellPointer closure, ValueID thisValue, const Arguments& arguments, const Type& result, LBasicBlock otherwise)
{
    auto& callee = m_program.at(function);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Type arrayType = Type::engineClass(m_builtins.arrayClass());
    Vector<Argument, 8> all;
    auto pass = [&](ValueID id, CastReason reason) {
        auto& type = declaredArgumentType(callee, all.size());
        SetForScope isCoerced(m_rejectsUntypedObjects, (reason == CastReason::As && m_program.annotationCoercesUntypedObjects(type)) || m_program.isRelaxedForLogging(type));
        SetForScope useCastTest(m_useCastTest, reason == CastReason::As);
        all.append(Argument { narrowOrElse(get(id), typeOf(id), type, otherwise), &type });
    };
    for (unsigned i = 0; i < callee.captureTypes.size(); ++i)
        all.append(Argument { closureCapture(closure, callee, i), &declaredArgumentType(callee, i) });
    if (callee.has(Format::FunctionHasThis))
        pass(thisValue, CastReason::Store);
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i) {
        if (i < arguments.size())
            pass(arguments[i], callee.parameterCastReason(i));
        else
            all.append(Argument { undefined(), &undefinedType });
    }
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        all.append(Argument { newArrayWithType(rest, declaredArgumentType(callee, all.size())), &arrayType });
    }
    return callCompiled(function, all, result, nullptr, true);
}

bool Lowering::isGuardedCallee(ValueID callee)
{
    if (!m_didFindGuardedCallees) {
        m_didFindGuardedCallees = true;
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::IsClosureOf || value.opcode == Opcode::IsBuiltinFunction)
                m_guardedCallees.set(value.child(0).index());
        }
    }
    return m_guardedCallees.get(callee.index());
}

Operand Lowering::lowerIndirectCall(ValueID callee, ValueID thisValue, const Arguments& arguments, const Type& type, StringID methodName)
{
    auto genericPath = [&] {
        return ensure(callGeneric(in<Boxed>(callee), thisValue ? in<Boxed>(thisValue) : undefined(), arguments, methodName, SpeculatedMethod::None, typeOf(callee).isSubsetOf(m_program.at(m_program.types.function), m_program)), type);
    };
    FunctionID expected = m_function.at(callee).predictedClosure();
    if (expected && !mayCallDirectlyFromTailCallSite(expected, type))
        return genericPath();
    if (!expected || isGuardedCallee(callee) || prefersGenericPathInColdCode())
        return genericPath();
    FunctionID listEntry = listEntryFor(expected);
    if (listEntry && isLoweringCallInTailPosition())
        return genericPath();
    if (listEntry && m_dropsDroppableCallerFrame)
        return genericPath();
    if (!listEntry && (!canCallInsteadOfClosure(expected, arguments.size()) || (!thisValue && m_program.at(expected).has(Format::FunctionHasThis))))
        return genericPath();
    auto& functionType = m_program.at(m_program.types.function);
    return ifPredictionHolds({ }, repOf(type), [&](LBasicBlock otherwise) {
        Boxed boxed = in<Boxed>(callee);
        if (!typeOf(callee).isSubsetOf(functionType, m_program))
            orElse(isCell(boxed, typeOf(callee)), otherwise);
        CellPointer closure = asCell(boxed);
        checkClosureOfOrElse(closure, expected, false, otherwise);
        if (listEntry)
            return ensure(callListEntry(listEntry, closure.value(), thisValue ? in<Boxed>(thisValue) : undefined(), arguments), type);
        return callClosure(expected, closure, thisValue, arguments, type, otherwise);
    }, genericPath);
}

Word32 Lowering::lowerIsBuiltinFunction(const Value& value)
{
    BuiltinName global = value.comparedGlobal();
    return equal(in<Boxed>(value.child(0)), box(builtinCell(global ? m_builtins.cellOf(global) : m_builtins.cellOf(value.builtin()))));
}

const Function* Lowering::definingFunctionOf(ValueID id) const
{
    auto& definition = m_function.at(originOf(id));
    if (definition.opcode != Opcode::FunctionReference && definition.opcode != Opcode::NewClosure)
        return nullptr;
    return &m_program.at(definition.function());
}

Boxed Lowering::getCallOrApply(Boxed base, StringID name, BuiltinID method, const Function* definedAs)
{
    CellPointer builtin = builtinCell(m_builtins.cellOf(method));
    LBasicBlock throughCache = m_out.newBlock();
    Join join(m_out, Rep::JSValue, Int64);
    if (!definedAs)
        orElse(isCell(base), throughCache);
    CellPointer cell = asCell(base);
    auto checkCalledFullClosureOrElse = [&] {
        orElse(hasClosureStructure(cell, Instance::offsetOfTypedClosureStructureID()), throughCache);
        if (!definedAs || definedAs->kind == FunctionKind::Constructor)
            orElse(isZero(bitAnd(loadField<Integer>(cell, m_heaps.AOTTypedClosure_entry), integer(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit)))), throughCache);
    };
    if (definedAs && m_program.usesSlimClosures(*definedAs))
        orElse(isUnreifiedSlimClosure(cell), throughCache);
    else if (definedAs)
        checkCalledFullClosureOrElse();
    else {
        LBasicBlock inheritsProperty = m_out.newBlock();
        LBasicBlock isNotSlim = m_out.newBlock();
        m_out.branch(isUnreifiedSlimClosure(cell).value(), usually(inheritsProperty), rarely(isNotSlim));
        m_out.appendTo(isNotSlim);
        checkCalledFullClosureOrElse();
        m_out.jump(inheritsProperty);
        m_out.appendTo(inheritsProperty);
    }
    join.add(box(builtin));
    m_out.appendTo(throughCache);
    join.add(emitCachedGetById(base, name));
    return join.finish().as<Boxed>();
}

std::optional<Operand> Lowering::lowerCallOrApply(ValueID result, ValueID function, StringID name, std::optional<Boxed> method, const Arguments& arguments)
{
    auto& type = typeOf(result);
    BuiltinID builtin = m_builtins.callOrApply(m_program.builtinName(name));
    if (!builtin || !(typeOf(function).anyFunction || typeOf(function).anyObject))
        return std::nullopt;
    Boxed callee = in<Boxed>(function);
    Boxed found;
    {
        SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
        found = method ? *method : emitGetById(callee, name, false, definingFunctionOf(function));
    }
    ValueID thisValue = arguments.isEmpty() ? ValueID() : arguments[0];
    return eitherOperand(equal(found, box(builtinCell(m_builtins.cellOf(builtin)))), repOf(type), [&] {
        if (m_program.builtinName(name) == m_builtins.applyName())
            return lowerApply(function, thisValue, arguments.size() > 1 ? arguments[1] : ValueID(), type);
        Arguments passed;
        if (arguments.size() > 1)
            passed.append(arguments.span().subspan(1));
        if (auto call = lowerBuiltinFunctionCall(result, function, thisValue, passed))
            return *call;
        return lowerIndirectCall(function, thisValue, passed, type);
    }, [&] {
        return ensure(callGeneric(found, callee, arguments), type);
    });
}

Operand Lowering::lowerApply(ValueID callee, ValueID thisValue, ValueID list, const Type& type, LanguageOperation operation)
{
    if (operation == LanguageOperation::Apply && (!list || typeOf(list).isUndefinedOrNull()))
        return lowerIndirectCall(callee, thisValue, { }, type);
    std::optional<bool> isMax = isMathMaxOrMin(callee);
    if (!isMax) {
        Boxed function = in<Boxed>(callee);
        Boxed receiver = thisValue ? in<Boxed>(thisValue) : undefined();
        if (operation == LanguageOperation::Apply) {
            if (isLoweringCallInTailPosition())
                checkCallableBeforeTailCall(function, { });
            return ensure(callValueWithArray(Stub::ApplyArray, function, receiver, in<Boxed>(list)), type);
        }
        Boxed boxedList = in<Boxed>(list);
        if (!m_dropsDroppableCallerFrame && (!m_tailCall || !m_tailCall->mustBeProperTailCall || !canTailCallValue()))
            return ensure(callValueWithArray(Stub::SpreadArray, function, receiver, boxedList, false), type);
        auto isOrdinaryArray = either<Word32>(isCell(boxedList, typeOf(list)), [&] {
            return Word32(isOriginalArray(asCell(boxedList).value()));
        }, [&] {
            return word32(0);
        });
        return eitherOperand(isOrdinaryArray, repOf(type), [&] {
            Boxed callable = function;
            checkCallableBeforeTailCall(callable, { });
            return ensure(callValueWithArray(Stub::SpreadArray, callable, receiver, boxedList), type);
        }, [&] {
            return ensure(callValueWithArray(Stub::SpreadArray, function, receiver, boxedList, false), type);
        });
    }
    Boxed function;
    Boxed receiver;
    auto fetchFunction = [&] {
        function = in<Boxed>(callee);
        receiver = thisValue ? in<Boxed>(thisValue) : undefined();
    };
    Boxed boxedList = in<Boxed>(list);
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    orElse(isCell(boxedList, typeOf(list)), slowPath);
    CellPointer array = asCell(boxedList);
    orElse(Word32(isOriginalArray(array.value())), slowPath);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    join.add(numberResult(minOrMaxOfElements(*isMax, array, butterfly, length, slowPath), type));

    m_out.appendTo(slowPath);
    fetchFunction();
    LValue buffer = transientBuffer(3 * sizeof(EncodedJSValue));
    LValue inBuffer[] = { function.value(), receiver.value(), boxedList.value() };
    for (unsigned i = 0; i < std::size(inBuffer); ++i)
        m_out.store64(inBuffer[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    join.add(ensure(AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(operation)), word32(std::size(inBuffer)), Pointer(buffer)), type));
    return join.finish();
}

Operand Lowering::lowerCallBySignature(ValueID result, const Value& value, StringID methodName)
{
    auto& type = typeOf(result);
    m_context.emitsCalls = true;
    FunctionID adapter(value.immediates[1]);
    auto& convention = m_program.at(adapter);
    RELEASE_ASSERT(convention.isValueEntry && convention.blocks[0].parameters.size() == value.arguments.size() + 2);
    Boxed callee = in<Boxed>(value.child(0));
    bool passesThis = value.child(1) && m_function.at(value.child(1)).opcode != Opcode::Undefined;
    LValue thisValue = passesThis ? in<Boxed>(value.child(1)).value() : nullptr;
    if (methodName) {
        requireOrThrowAt(isCell(callee), ThrowKind::NotAFunction, Operand(thrownNullish(callee)), methodName.index());
        callee = box(asCell(callee));
    }
    Vector<LValue, 8> passed;
    for (unsigned i = 0; i < value.arguments.size(); ++i) {
        Argument given = argument(value.arguments[i]);
        passed.append(ensure(given.operand, *given.type, declaredArgumentType(convention, i + 2)).value);
    }
    Rep rep = resultRepOf(m_program, adapter);
    PatchpointValue* patchpoint = m_out.patchpoint(b3TypeOf(rep));
    applyEffects(patchpoint, { });
    patchpoint->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
    if (passesThis)
        patchpoint->append(ConstrainedValue(thisValue, ValueRep::reg(thisGPR)));
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (LValue argument : passed)
        patchpoint->append(ConstrainedValue(argument, argument->type() == Double ? ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) : ValueRep::reg(directArgumentGPR(nextGPR++))));
    RELEASE_ASSERT(nextGPR <= numberOfArgumentGPRs && nextFPR <= FPRInfo::numberOfArgumentRegisters);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(RegisterSet { stubTemporaryGPRs[0] });
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    if (b3TypeOf(rep) == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (rep != Rep::Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, adapter = calleeIndex(adapter), site, passesThis](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->callFunction(jit, adapter, site, passesThis ? StubCall::firstOmittedGPRForPreambleWithThis : StubCall::firstOmittedGPRForPreamble, StubCall::endOfOmittedGPRsForPreamble);
    });
    unwindToLandingPad(patchpoint, site);
    if (rep == Rep::Void)
        return placeholder(repOf(type));
    return ensure(Operand(rep, patchpoint), m_program.at(convention.resultType), type);
}

Operand Lowering::lowerCall(ValueID result, const Value& value)
{
    ValueID callee = value.child(0);
    ValueID thisValue = value.child(1);
    auto& type = typeOf(result);
    auto definitionOf = [&](ValueID id) -> const Value& {
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return m_function.at(id);
    };
    StringID methodName;
    if (auto& read = definitionOf(callee); thisValue && read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(thisValue))
        methodName = read.string();
    else if (thisValue)
        methodName = value.string();
    if (auto found = m_knownNullishIn.find(m_currentBlockID.index()); found != m_knownNullishIn.end() && m_currentBlock->terminator.kind == TerminatorKind::Unreachable && m_currentIndexInBlock + 1 == m_currentBlock->values.size() && !Options::auditAOTTypedAssertions()) {
        ValueID tested = callee;
        while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
            tested = m_function.at(tested).child(0);
        if (found->value == tested) {
            SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
            throwAt(ThrowKind::NotAFunction, encodesUndefinedAsZero(callee) ? Operand(Boxed(m_values[callee.index()].value)) : Operand(in<Boxed>(callee)), methodName ? methodName.index() : ThrowSite::none);
            return placeholder(repOf(type));
        }
    }
    if (value.speculatedMethod != SpeculatedMethod::None && thisValue && value.arguments.size() <= numberOfArgumentGPRs)
        return ensure(callGeneric(in<Boxed>(callee), in<Boxed>(thisValue), value.arguments, methodName, value.speculatedMethod), type);
    auto genericPath = [&] {
        if (value.isSignatureCall && !canTailCallValue() && !m_dropsDroppableCallerFrame)
            return lowerCallBySignature(result, value, methodName);
        return lowerIndirectCall(callee, thisValue, value.arguments, type, methodName);
    };
    if (auto call = lowerBuiltinFunctionCall(result, callee, thisValue, value.arguments))
        return *call;
    if (BuiltinName global = globalNameOf(callee)) {
        if (auto builtin = predictedBuiltin(BuiltinKind::Function, { }, global, value.arguments))
            return lowerPredictedBuiltin(result, *builtin, std::nullopt, genericPath);
    } else if (auto& read = definitionOf(callee); read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(thisValue)) {
        if (auto call = isGuardedCallee(callee) ? std::nullopt : lowerCallOrApply(result, thisValue, read.string(), in<Boxed>(callee), value.arguments))
            return *call;
        if (auto bound = isGuardedCallee(callee) ? std::nullopt : lowerBind(thisValue, read.string(), in<Boxed>(callee), value.arguments, type))
            return *bound;
        if (auto access = isGuardedCallee(callee) ? std::nullopt : lowerBufferAccess(thisValue, read.string(), in<Boxed>(callee), value.arguments, type))
            return *access;
        if (auto builtin = predictedBuiltin(BuiltinKind::Method, thisValue, read.string(), value.arguments))
            return lowerPredictedBuiltin(result, *builtin, in<Boxed>(callee), genericPath);
    } else if (m_function.isSharedOutlinedBody && thisValue && methodName) {
        if (auto builtin = predictedBuiltin(BuiltinKind::Method, thisValue, methodName, value.arguments))
            return lowerPredictedBuiltin(result, *builtin, in<Boxed>(callee), genericPath);
    }
    return genericPath();
}

Integer Lowering::entryWordIfClosure(CellPointer cell)
{
    return either<Integer>(hasClosureStructure(cell, Instance::offsetOfTypedClosureStructureID()), [&] {
        return loadField<Integer>(cell, m_heaps.AOTTypedClosure_entry);
    }, [&] {
        return Integer(callAllocationStub(Stub::TypedClosureEntry, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
    });
}

std::optional<Pointer> Lowering::createThisCache()
{
    constexpr unsigned numberOfSlots = 3;
    if (m_context.sites.size() + numberOfSlots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    unsigned slot = m_context.sites.size();
    for (unsigned i = 0; i < numberOfSlots; ++i)
        m_context.sites.append(Site { });
    return slotAddress(slot);
}

CellPointer Lowering::createThis(CellPointer constructor, CellPointer newTarget, std::optional<Pointer> siteCache)
{
    if (!siteCache)
        siteCache = createThisCache();
    if (!siteCache)
        return AOT_TYPED_OPERATION(CreateThis, constructor, newTarget);
    Pointer cache = *siteCache;
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock isFilled = m_out.newBlock();
    LBasicBlock isEmpty = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID));
    LValue isValid = m_out.notZero32(m_out.load32(inCache(0)));
    isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load32(inCache(2 * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer))), loadRuntimeState32(Instance::offsetOfTypedPrototypeEpoch())));
    isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer))), newTarget.value()));
    m_out.branch(isValid, usually(isFilled), rarely(isEmpty));
    m_out.appendTo(isFilled);
    ValueFromBlock fast = m_out.anchor(callICStub(Stub::NewObjectLiteral0, Int64, { { cache.value(), GPRInfo::argumentGPR1 } }, std::nullopt, { Writes::RuntimeState, false }));
    m_out.jump(continuation);
    m_out.appendTo(isEmpty);
    static_assert(Typed::WritesOf<operationAOTTypedCreateThisAt>::value == Writes::RuntimeState);
    ValueFromBlock slow = m_out.anchor(callICStub(Stub::CreateThisByNewTarget, Int64, { { cache.value(), GPRInfo::argumentGPR1 }, { constructor.value(), GPRInfo::argumentGPR2 }, { newTarget.value(), GPRInfo::argumentGPR3 } }, std::nullopt, { Writes::RuntimeState }));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

CellPointer Lowering::lowerCreateThis(const Value& value)
{
    Operand operand = get(value.child(0));
    CellPointer newTarget = operand.rep == Rep::JSValue ? asCell(operand.as<Boxed>()) : operand.as<CellPointer>();
    if (!value.child(1))
        return createThis(newTarget, newTarget);
    Operand constructor = get(value.child(1));
    return createThis(constructor.rep == Rep::JSValue ? asCell(constructor.as<Boxed>()) : constructor.as<CellPointer>(), newTarget);
}

Boxed Lowering::constructClosure(CellPointer constructor, Integer word, CellPointer newTarget, const Arguments& arguments, std::optional<Pointer> siteCache, std::optional<uint32_t> numberOfArgumentsToSkip)
{
    Boxed created = box(createThis(constructor, newTarget, siteCache));
    auto call = [&] {
        if (numberOfArgumentsToSkip)
            return callValueWithList(box(constructor), created, restLength(*numberOfArgumentsToSkip), offsetBy(m_argumentBuffer, integer(*numberOfArgumentsToSkip), 3), word.value());
        if (arguments.size() > numberOfArgumentGPRs) {
            auto [count, list] = argumentBuffer(arguments);
            return callValueWithList(box(constructor), created, count, list, word.value());
        }
        Vector<LValue, 8> operands { constructor.value(), created.value() };
        for (ValueID argument : arguments)
            operands.append(in<Boxed>(argument).value());
        return callValue(Stub::ConstructClosure, operands, { }, word.value());
    };
    Boxed returned = call();
    return either<Boxed>(isCell(returned), [&] {
        return select(aboveOrEqual(loadCellType(asCell(returned)), word32(ObjectType)), returned, created);
    }, [&] {
        return created;
    });
}

Boxed Lowering::constructDerivedClosure(CellPointer constructor, Integer word, Boxed newTarget, const Arguments& arguments)
{
    if (arguments.size() >= numberOfArgumentGPRs) {
        Vector<LValue, 8> inList { newTarget.value() };
        for (ValueID argument : arguments)
            inList.append(in<Boxed>(argument).value());
        LValue list = transientBuffer(inList.size() * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < inList.size(); ++i)
            m_out.store64(inList[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, list, i * sizeof(EncodedJSValue)));
        return callValueWithList(box(constructor), undefined(), word32(inList.size()), Pointer(list), word.value());
    }
    Vector<LValue, 8> operands { constructor.value(), undefined().value(), newTarget.value() };
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    return callValue(Stub::ConstructClosure, operands, { }, word.value());
}

Operand Lowering::lowerConstructSuper(ValueID result, const Value& value)
{
    Boxed classObject = in<Boxed>(value.child(0));
    Boxed newTarget = in<Boxed>(value.child(1));
    auto& object = m_program.at(m_program.types.object);
    std::optional<uint32_t> skipped;
    if (value.opcode == Opcode::ConstructSuperForwardVarargs)
        skipped = value.numberOfArgumentsToSkip();
    auto genericPath = [&] {
        Boxed list = box(skipped ? AOT_TYPED_OPERATION(NewArray, restLength(*skipped), offsetBy(m_argumentBuffer, integer(*skipped), 3)) : newArray(value.arguments));
        return AOT_TYPED_OPERATION(ConstructSuper, classObject, newTarget, list, value.child(2) ? in<Boxed>(value.child(2)) : Boxed(m_out.int64Zero));
    };
    auto& constructor = m_function.at(value.child(0));
    LayoutID ofClass = constructor.opcode == Opcode::FunctionReference ? m_program.at(constructor.function()).constructedClass : LayoutID();
    if (value.isOnSlowPath || (ofClass && m_program.at(ofClass).extendsGlobalClass))
        return ensure(genericPath(), object, typeOf(result));
    LBasicBlock otherwise = m_out.newBlock();
    Join join(m_out, Rep::JSValue, Int64);
    orElse(isCell(classObject, typeOf(value.child(0))), otherwise);
    Boxed parent = value.child(2) ? in<Boxed>(value.child(2)) : Boxed(m_out.load64(structureOf(classObject.value()), m_heaps.Structure_prototype));
    orElse(logicalAnd(isNotEmpty(parent), isCell(parent)), otherwise);
    LBasicBlock isNotOrdinary = m_out.newBlock();
    Integer word = entryWordIfClosure(asCell(parent));
    orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::constructsOrdinaryObjectBit)))), isNotOrdinary);
    orElse(isCell(newTarget, typeOf(value.child(1))), otherwise);
    orElse(logicalNot(isZero(entryWordIfClosure(asCell(newTarget)))), otherwise);
    join.add(constructClosure(asCell(parent), word, asCell(newTarget), value.arguments, std::nullopt, skipped));
    m_out.appendTo(isNotOrdinary);
    auto& explicitNewTarget = m_function.at(value.child(1));
    if (!skipped || (explicitNewTarget.opcode == Opcode::GetArgument && explicitNewTarget.immediates[0] + 1 == *skipped)) {
        orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::isDerivedConstructorBit)))), otherwise);
        join.add(skipped ? callValueWithList(parent, undefined(), restLength(*skipped - 1), offsetBy(m_argumentBuffer, integer(*skipped - 1), 3), word.value()) : constructDerivedClosure(asCell(parent), word, newTarget, value.arguments));
    } else
        m_out.jump(otherwise);
    m_out.appendTo(otherwise);
    join.add(genericPath());
    return ensure(join.finish().as<Boxed>(), object, typeOf(result));
}

Boxed Lowering::lowerGetNewTarget()
{
    TypedPointer field = m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTyped()), State::offsetOfNewTarget());
    Boxed held(m_out.load64(field));
    m_out.store64(m_out.int64Zero, field);
    didEmitWrite(Writes::RuntimeState);
    return select(isNotEmpty(held), held, undefined());
}

Operand Lowering::lowerConstruct(ValueID result, const Value& value)
{
    if (FunctionID body = m_program.constructBody(value); body && !m_isInLoop && canUseConstructBody(m_program, m_id, m_function, value)) {
        if (auto cache = createThisCache()) {
            Vector<Argument, 8> arguments { argument(value.child(0)) };
            for (ValueID id : value.arguments)
                arguments.append(argument(id));
            arguments.append(Argument { Operand(Rep::Int64, cache->value()), &m_program.at(m_program.types.integer) });
            return callCompiled(body, arguments, typeOf(result));
        }
    }
    std::optional<Pointer> siteCache;
    if (value.child(1))
        siteCache = Pointer(in<Integer>(value.child(1)).value());
    Boxed callee = in<Boxed>(value.child(0));
    auto& object = m_program.at(m_program.types.object);
    auto genericPath = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        return AOT_TYPED_OPERATION(Construct, callee, count, list);
    };
    ValueID constructor = value.child(0);
    while (m_function.at(constructor).opcode == Opcode::Cast)
        constructor = m_function.at(constructor).child(0);
    if (m_function.at(constructor).opcode == Opcode::GetGlobal)
        return ensure(genericPath(), object, typeOf(result));
    LBasicBlock otherwise = m_out.newBlock();
    Join join(m_out, Rep::JSValue, Int64);
    orElse(isCell(callee, typeOf(value.child(0))), otherwise);
    CellPointer closure = asCell(callee);
    LBasicBlock isNotOrdinary = m_out.newBlock();
    Integer word = entryWordIfClosure(closure);
    orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::constructsOrdinaryObjectBit)))), isNotOrdinary);
    join.add(constructClosure(closure, word, closure, value.arguments, siteCache));
    m_out.appendTo(isNotOrdinary);
    orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::isDerivedConstructorBit)))), otherwise);
    join.add(constructDerivedClosure(closure, word, callee, value.arguments));
    m_out.appendTo(otherwise);
    join.add(genericPath());
    return ensure(join.finish().as<Boxed>(), object, typeOf(result));
}

Operand Lowering::lowerConstructWith(ValueID result, const Value& value)
{
    Boxed callee = in<Boxed>(value.arguments[0]);
    CellPointer object = in<CellPointer>(value.arguments[1]);
    Arguments passed;
    for (unsigned i = 2; i < value.arguments.size(); ++i)
        passed.append(value.arguments[i]);
    auto [count, list] = argumentBuffer(passed);
    return ensure(AOT_TYPED_OPERATION(ConstructWith, callee, object, count, list), m_program.at(m_program.types.object), typeOf(result));
}

std::optional<Lowering::MethodTargets> Lowering::methodTargets(LayoutID supertype, StringID name) const
{
    static constexpr unsigned maxTargets = 32;
    static constexpr unsigned maxShapes = 256;
    auto& subtypes = m_program.at(supertype).subtypes;
    if (subtypes.isEmpty() || subtypes.size() > maxShapes || m_program.mayRunInMultipleInstances)
        return std::nullopt;
    bool layoutHasField = !!m_program.at(supertype).fieldNamed(name);
    MethodTargets targets;
    for (LayoutID subtype : subtypes) {
        FunctionID function;
        const Field* fieldCheckedFirst = hasMethodField(m_program, subtype, name) && findMethod(m_program, subtype, name, MethodKind::Method) ? &m_program.at(subtype).fields[*m_program.at(subtype).fieldNamed(name)] : nullptr;
        if (m_program.at(subtype).fieldNamed(name) && !fieldCheckedFirst) {
            if (!layoutHasField)
                return std::nullopt;
        } else {
            if (m_program.at(subtype).mayHaveOtherPrototype)
                return std::nullopt;
            for (LayoutID ancestor = subtype; ancestor && !function; ancestor = m_program.at(ancestor).base) {
                if (m_program.at(ancestor).hasComputedMethodName)
                    return std::nullopt;
                for (auto& method : m_program.at(ancestor).methods) {
                    if (method.name != name)
                        continue;
                    if (method.kind != MethodKind::Method || method.mayBeReplaced)
                        return std::nullopt;
                    function = method.function;
                }
            }
            if (!function || !m_program.at(function).captureTypes.isEmpty() || m_program.at(function).blocks.isEmpty())
                return std::nullopt;
        }
        size_t at = targets.findIf([&](auto& target) {
            return target.function == function && !target.fieldCheckedFirst == !fieldCheckedFirst && (!fieldCheckedFirst || (target.fieldCheckedFirst->slot == fieldCheckedFirst->slot && target.fieldCheckedFirst->offset == fieldCheckedFirst->offset && target.fieldCheckedFirst->isOutOfLine == fieldCheckedFirst->isOutOfLine));
        });
        if (at == notFound) {
            if (targets.size() == maxTargets)
                return std::nullopt;
            at = targets.size();
            targets.append({ function, { }, fieldCheckedFirst });
        }
        targets[at].shapes.append(m_program.at(subtype).shape);
    }
    return targets;
}

Lowering::Argument Lowering::argumentFor(const Function& callee, unsigned parameter, ValueID id)
{
    TypeID declared = callee.parameterTypes[parameter];
    if (!declared || callee.parameterCastReason(parameter) != CastReason::As)
        return argument(id);
    Value cast;
    cast.opcode = Opcode::Cast;
    cast.type = declared;
    cast.rep = repOf(m_program.at(declared));
    cast.children[0] = id;
    cast.immediates[1] = static_cast<uint32_t>(CastReason::As);
    return { coerce(get(id), typeOf(id), cast), &m_program.at(declared) };
}

Operand Lowering::callMethod(FunctionID function, CellPointer thisObject, const Arguments& arguments, const Type& result)
{
    auto& callee = m_program.at(function);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Type arrayType = Type::engineClass(m_builtins.arrayClass());
    Vector<Argument, 8> all;
    if (callee.has(Format::FunctionHasThis))
        all.append({ thisObject, &declaredArgumentType(callee, 0) });
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i) {
        SetForScope calleeScope(m_parameterCheckCallee, callee.has(Format::FunctionIsBuiltin | Format::FunctionIsInBuiltinModule | Format::FunctionIsHiddenInStackTraces) ? FunctionID() : function);
        all.append(i < arguments.size() ? argumentFor(callee, i, arguments[i]) : Argument { undefined(), &undefinedType });
    }
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        all.append({ newArrayWithType(rest, declaredArgumentType(callee, all.size())), &arrayType });
    }
    return callCompiled(function, all, result, nullptr, true);
}

Operand Lowering::lowerCallMethod(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    auto& type = typeOf(result);
    if (m_tableMethodCalls.get(result.index()))
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    LayoutID supertype = receiver.singleLayout();
    auto targets = supertype ? methodTargets(supertype, name) : std::nullopt;
    if (m_function.copiesFirstArgumentToHeap && !m_program.typeOf(m_function, object).singleLayout() && !value.arguments.isEmpty() && value.arguments.size() <= numberOfArgumentGPRs) {
        SetForScope argumentCopiedToHeap(m_argumentCopiedToHeap, value.arguments[0]);
        SetForScope heapCopy(m_heapCopy, copyClosureToHeap(in<Boxed>(value.arguments[0])));
        return ensure(callMethodGeneric(in<Boxed>(object), name, value.arguments), type);
    }
    if (!targets && value.speculatedMethod != SpeculatedMethod::None) {
        if (auto slot = allocateSite(name, 0))
            return ensure(callMethodThroughSlot(in<Boxed>(object), *slot, value.arguments, value.speculatedMethod), type);
    }
    if (!targets) {
        ValueID methodGetById = value.child(1);
        if (auto call = methodGetById ? std::nullopt : lowerCallOrApply(result, object, name, std::nullopt, value.arguments))
            return *call;
        if (auto bound = methodGetById ? std::nullopt : lowerBind(object, name, std::nullopt, value.arguments, type))
            return *bound;
        if (auto access = methodGetById ? std::nullopt : lowerBufferAccess(object, name, std::nullopt, value.arguments, type))
            return *access;
        auto genericPath = [&] {
            if (!methodGetById)
                return ensure(callMethodGeneric(in<Boxed>(object), name, value.arguments), type);
            return ensure(callGeneric(either<Boxed>(isNotEmpty(in<Boxed>(methodGetById)), [&] {
                return in<Boxed>(methodGetById);
            }, [&] {
                return box(builtinCell(m_builtins.cellOf(m_builtins.findForInstance(BuiltinKind::Method, m_builtins.classes()[value.predictedReceiverClassPlusOne - 1], m_program.builtinName(name)))));
            }), in<Boxed>(object), value.arguments, name), type);
        };
        if (value.predictedReceiverClassPlusOne) {
            auto& engineClass = m_builtins.classes()[value.predictedReceiverClassPlusOne - 1];
            Vector<Type, 4> argumentTypes;
            for (ValueID argument : value.arguments)
                argumentTypes.append(typeOf(argument));
            BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, engineClass, m_program.builtinName(name)), argumentTypes.span());
            if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Operation)
                return genericPath();
            Value call;
            call.opcode = Opcode::CallBuiltin;
            call.children = { object, ValueID(), value.child(2) };
            call.immediates = { entry.index(), engineClass.index };
            call.arguments = value.arguments;
            call.type = value.type;
            Type expected = &engineClass == &m_builtins.stringClass() ? m_program.at(m_program.types.string) : Type::engineClass(engineClass);
            auto inlinePath = [&](LBasicBlock) {
                if (auto searched = lowerArraySearchAsSingleCall(result, call))
                    return *searched;
                return lowerResolvedBuiltin(result, call);
            };
            auto isEntrypointTarget = [&](ValueID read) {
                while (m_function.at(read).opcode == Opcode::Cast)
                    read = m_function.at(read).child(0);
                if (m_function.at(read).opcode != Opcode::BuiltinFunction)
                    return false;
                auto& found = m_builtins.at(m_function.at(read).builtin());
                auto& wanted = m_builtins.at(entry);
                return found.kind == wanted.kind && found.holderName == wanted.holderName && found.nameName == wanted.nameName;
            };
            if (!methodGetById || isEntrypointTarget(methodGetById))
                return ifPredictionHolds({ Expectation { object, &expected, true } }, repOf(type), inlinePath, genericPath);
            Rep rep = repOf(type);
            Join join(m_out, rep, b3TypeOf(rep));
            LBasicBlock wasNotRead = m_out.newBlock();
            LBasicBlock hasChanged = newColdBlock();
            LBasicBlock callsValue = newColdBlock();
            Boxed read = in<Boxed>(methodGetById);
            ValueFromBlock readValue = m_out.anchor(read.value());
            m_out.branch(isEmpty(read).value(), usually(wasNotRead), rarely(callsValue));
            m_out.appendTo(wasNotRead);
            SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
            join.add(ifPredictionHolds({ Expectation { object, &expected, true } }, rep, inlinePath, genericPath, hasChanged));
            m_out.appendTo(hasChanged);
            ValueFromBlock entrypointTarget = m_out.anchor(box(builtinCell(m_builtins.cellOf(entry))).value());
            m_out.jump(callsValue);
            m_out.appendTo(callsValue);
            join.add(ensure(callGeneric(Boxed(m_out.phi(Int64, readValue, entrypointTarget)), in<Boxed>(object), value.arguments, name), type));
            return join.finish();
        }
        if (auto builtin = predictedBuiltin(BuiltinKind::Method, object, name, value.arguments))
            return lowerPredictedBuiltin(result, *builtin, std::nullopt, genericPath);
        return genericPath();
    }

    CellPointer cell;
    {
        SetForScope isBeforeTailCall(m_isBeforeTailCall, true);
        cell = nonNullish(object, name);
    }
    return ifSubtype(cell, object, supertype, repOf(type), [&] {
        return callSubtypeMethod(cell, receiver, *targets, value, type);
    }, [&] {
        return ensure(callMethodGeneric(box(cell), name, value.arguments), type);
    });
}

Operand Lowering::callSubtypeMethod(CellPointer cell, const Type& receiver, const MethodTargets& targets, const Value& value, const Type& type)
{
    auto call = [&](const MethodTarget& target) {
        if (target.function && !mayCallDirectlyFromTailCallSite(target.function, type))
            return ensure(callMethodGeneric(box(cell), value.string(), value.arguments), type);
        if (target.fieldCheckedFirst) {
            const Field& field = *target.fieldCheckedFirst;
            return eitherOperand(hasField(cell, field), repOf(type), [&] {
                SetForScope fieldIsPresent(m_fieldIsPresent, true);
                Boxed callee = readField(cell, field, m_program.at(m_program.types.anything)).as<Boxed>();
                return ensure(callGeneric(callee, box(cell), value.arguments, value.string()), type);
            }, [&] {
                return callMethod(target.function, cell, value.arguments, type);
            });
        }
        if (target.function)
            return callMethod(target.function, cell, value.arguments, type);
        const Field& field = *fieldFor(receiver, value.string());
        Boxed callee = readProperty(cell, receiver.singleLayout(), field, m_program.at(m_program.types.anything)).as<Boxed>();
        bool holdsOnlyFunctions = field.type && m_program.at(field.type).isSubsetOf(m_program.at(m_program.types.function), m_program);
        return ensure(callGeneric(callee, box(cell), value.arguments, holdsOnlyFunctions ? StringID() : value.string()), type);
    };
    if (targets.size() == 1)
        return call(targets.first());

    unsigned usual = 0;
    for (unsigned i = 1; i < targets.size(); ++i) {
        if (targets.at(i).shapes.size() > targets.at(usual).shapes.size())
            usual = i;
    }
    Vector<LBasicBlock, 4> blocks;
    Vector<FTL::SwitchCase> cases;
    for (unsigned i = 0; i < targets.size(); ++i) {
        blocks.append(m_out.newBlock());
        if (i == usual)
            continue;
        for (uint32_t shape : targets.at(i).shapes)
            cases.append(FTL::SwitchCase(m_out.constInt32(static_cast<int32_t>(Typed::shapeWordOf(shape))), blocks[i], FTL::Weight()));
    }
    LBasicBlock continuation = m_out.newBlock();
    m_out.switchInstruction(shapeWordOf(cell).value(), cases, blocks[usual], FTL::Weight());
    Rep rep = repOf(type);
    Vector<ValueFromBlock, 4> results;
    for (unsigned i = 0; i < targets.size(); ++i) {
        m_out.appendTo(blocks[i]);
        Operand returnedValue = call(targets.at(i));
        RELEASE_ASSERT(returnedValue.rep == rep);
        results.append(m_out.anchor(returnedValue.value));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), results));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
