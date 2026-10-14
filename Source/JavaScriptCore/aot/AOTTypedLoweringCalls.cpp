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

#if CPU(ARM64)
static constexpr GPRReg exceptionGPR = ARM64Registers::x16;
#else
static constexpr GPRReg exceptionGPR = CCallHelpers::s_scratchRegister;
#endif

void Lowering::didEmitWrite(Writes writes, bool writesShapeWords)
{
    // No caller has tested the shape of an object that this call has made.
    if (writes == Writes::Fields && m_isStoringToLocalAllocation) {
        writes = Writes::RuntimeState;
        writesShapeWords = false;
    }
    if (writes <= m_function.writes && (!writesShapeWords || m_function.writesShapeWords))
        return;
    dataLogLn("AOT: f", m_id.index(), " changes more (", static_cast<unsigned>(writes), writesShapeWords ? ", shape words" : "", ") than was summed up for it (", static_cast<unsigned>(m_function.writes), m_function.writesShapeWords ? ", shape words" : "", ")");
    if (m_currentBlock && m_currentIndexInBlock < m_currentBlock->values.size())
        dataLogLn("AOT: while this was lowered, or the end of its block: ", nameOf(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).opcode));
    m_program.dump(WTF::dataFile(), m_id);
    CRASH();
}

// The heaps of the program are numbered last: fields and variables, typed-array bytes, collector state, contiguous array elements, run-time
// state, extension pointers, order words, shape words. So whatever a call writes is one range. See B3AbstractHeapRepository.cpp.
void Lowering::applyEffects(PatchpointValue* patchpoint, OperationEffects effects)
{
    RELEASE_ASSERT_WITH_MESSAGE(!m_environmentAwaitingBarrier || effects.writes == Writes::Pure || m_isReportingThatFactDoesNotHold, "AOT: what may collect garbage comes between a store to an environment and its barrier");
    RELEASE_ASSERT_WITH_MESSAGE(!m_isFillingArrayAtBirth || effects.writes == Writes::Pure || m_isReportingThatFactDoesNotHold, "AOT: what may collect garbage comes between the birth of an array and the last element it is born with");
    if (m_isFillingReusedEnvironment && effects.writes != Writes::Pure && !m_isReportingThatFactDoesNotHold) [[unlikely]] {
        dataLogLn("AOT: f", m_id.index(), ": what may collect garbage comes between the birth of an environment that is used again and its barrier");
        CRASH();
    }
    if (!m_fieldsAwaitingFirstStore.isEmpty() && effects.writes != Writes::Pure && !effects.writesOnlyTypedArrayProperties && !m_isReportingThatFactDoesNotHold) [[unlikely]] {
        BitVector others = m_fieldsAwaitingFirstStore;
        if (isLoweringInitializingStore()) {
            if (auto field = fieldInitializedBy(m_layoutOfFieldsAwaitingFirstStore, m_function.at(m_currentBlock->values[m_currentIndexInBlock])))
                others.clear(*field);
        }
        RELEASE_ASSERT_WITH_MESSAGE(others.isEmpty(), "AOT: what may collect garbage comes between an allocation and the first store to a field that has no initial state");
    }
    bool writesShapeWords = effects.writesShapeWords && m_program.castsMayRetag;
    didEmitWrite(effects.writes, writesShapeWords);
    bool isTerminal = patchpoint->effects.terminal;
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = isTerminal;
    if (effects.writes == Writes::Pure)
        return;
    // Whatever may collect garbage or unwind looks at all there is.
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->effects.exitsSideways = effects.exitsSideways;
    patchpoint->effects.controlDependent = true;
    // Any call may make a string, say, and so come to collect garbage: what a write barrier goes by is read again after it.
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
    // Whoever gives an object another shape gives it an order of that shape.
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

void Lowering::unwindToLandingPad(PatchpointValue* patchpoint, CallSite site)
{
    if (!m_watchingHandler)
        return;
    if (!m_currentLandingPad)
        m_currentLandingPad = landingPadFor(*m_watchingHandler);
    RELEASE_ASSERT(m_callSitesAwaitingLandingPad);
    --m_callSitesAwaitingLandingPad;
    m_context.emitsCalls = true;
    m_context.unwindsToLandingPads = true;
    patchpoint->effects.terminal = true;
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
    uint32_t number = CallSiteIndex(PackedSite::bits(site.bits)).bytecodeIndex().offset();
    patchpoint->setGenerator([context = &m_context, generator = patchpoint->generator(), number](CCallHelpers& jit, const StackmapGenerationParams& params) {
        generator->run(jit, params);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        if (!params.fallsThroughToSuccessor(0)) {
            params.addLatePath([jump = jit.jump(), continuation = labels[0]](CCallHelpers& jit) {
                jump.linkTo(*continuation, &jit);
            });
        }
        Box<CCallHelpers::Label> handler = labels[1];
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
    m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(m_currentLandingPad));
    m_out.appendTo(continuation);
}

void Lowering::branchToLandingPad(PatchpointValue* patchpoint, std::optional<GPRReg> exception)
{
    if (!m_currentLandingPad)
        m_currentLandingPad = landingPadFor(*m_watchingHandler);
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

static bool callThrowAtSite(LoweringContext& context, CCallHelpers& jit, bool isLeaf, CallSite site, bool returnsToSite, const Vector<uint32_t, 3>& words, bool valueIsInWordRegister)
{
#if CPU(X86_64) && OS(LINUX)
    static_assert(throwWordGPR == X86Registers::r11);
    static const bool traps = [] {
        const char* asGiven = getenv("AOT_SWITCH_THROW_TRAP");
        if (asGiven && strcmp(asGiven, "0") && strcmp(asGiven, "1")) {
            dataLogLn("AOT: AOT_SWITCH_THROW_TRAP is not 0 | 1: ", asGiven);
            exit(1);
        }
        return !asGiven || asGiven[0] == '1';
    }();
    if (traps && !returnsToSite && !isLeaf && !valueIsInWordRegister) {
        context.trapsAtThrowSites.append({ jit.labelIgnoringWatchpoints(), site.bits });
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(0xf4));
        return false;
    }
#endif
    if (isLeaf && !hasStubsForFunctionsWithoutFrame) {
        jit.emitFunctionPrologue();
        isLeaf = false;
    }
    if (!returnsToSite && words.size() == 1 && !valueIsInWordRegister) {
        ArgumentMoves moves;
        static const bool wordIsWrongForTesting = getenv("BUN_AOT_SWITCH_ARMS_BREAK") && !strcmp(getenv("BUN_AOT_SWITCH_ARMS_BREAK"), "word");
        appendArgumentMove(moves, throwWordGPR, ArgumentSource::Constant, words[0] + wordIsWrongForTesting);
        context.stubCalls.call(jit, isLeaf ? Stub::LeafThrowAtSiteByWord : Stub::ThrowAtSiteByWord, site, moves);
        return true;
    }
    context.stubCalls.call(jit, isLeaf ? Stub::LeafThrowAtSite : Stub::ThrowAtSite, site);
    return false;
}

uint32_t Lowering::throwAt(ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b, unsigned classOfNewErrorPlusOne)
{
    RELEASE_ASSERT(!classOfNewErrorPlusOne || (!operand && classOfNewErrorPlusOne < ThrowSite::numberOfRegistersPerBank));
    // A site without a value says `undefined`: no register is set for it.
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
    patchpoint->clobber(*registersChangedBy(Stub::ColdOperationVoid));
    if (LoweringContext::coldSitesForceFrame() || (!hasStubsForFunctionsWithoutFrame && m_watchingHandler)) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasStubsForFunctionsWithoutFrame)
        m_context.holdsColdSites = true;
    CallSite site { nextSite() };
    uint32_t returnsToSite = m_watchingHandler ? 1u << 31 : 0;
    patchpoint->setGenerator([context = &m_context, kind, rep, a, b, site, returnsToSite, classOfNewErrorPlusOne](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        unsigned number = rep == Rep::Void ? classOfNewErrorPlusOne : rep == Rep::Double || rep == Rep::OptionalDouble ? static_cast<unsigned>(params[0].fpr()) : static_cast<unsigned>(params[0].gpr());
        auto words = throwSiteWords(kind, rep, number, a, b);
        words[0] |= returnsToSite;
        if (callThrowAtSite(*context, jit, isLeaf, site, returnsToSite, words, rep != Rep::Void && params[0].isGPR() && params[0].gpr() == throwWordGPR))
            return;
        context->noteDataInCode(jit, words.size());
        for (uint32_t word : words)
            jit.m_assembler.buffer().putInt(word);
    });
    if (returnsToSite)
        branchToLandingPad(patchpoint, std::nullopt);
    return site.bits;
}

void Lowering::requireOrThrowAt(Word32 condition, ThrowKind kind, std::optional<Operand> operand, uint32_t a)
{
    if (m_isInLoop || m_watchingHandler || m_environmentAwaitingBarrier || m_isFillingArrayAtBirth || !m_fieldsAwaitingFirstStore.isEmpty()) {
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
    if (operand && hasStubsForFunctionsWithoutFrame)
        check->clobberEarly(RegisterSet { stubTemporaryGPRs[1] });
#endif
    if (LoweringContext::coldSitesForceFrame()) {
        m_context.emitsCalls = true;
        check->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasStubsForFunctionsWithoutFrame)
        m_context.holdsColdSites = true;
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
        if (callThrowAtSite(*context, jit, isLeaf, site, false, words, rep != Rep::Void && params[0].isGPR() && params[0].gpr() == throwWordGPR))
            return;
        context->noteDataInCode(jit, words.size());
        for (uint32_t word : words)
            jit.m_assembler.buffer().putInt(word);
    });
}

LValue Lowering::callEntry(Entry entry, LType type, bool mayThrow, OperationEffects effects, const Vector<LValue, 8>& arguments)
{
    bool withInstance = takesInstance(entry);
    unsigned numberOfGPRs = withInstance;
    for (LValue argument : arguments)
        numberOfGPRs += argument->type() != Double;
    bool isOffUsualPath = m_out.m_block->frequency() < 1 && (withInstance || !mayThrow) && numberOfGPRs <= GPRInfo::numberOfArgumentRegisters;
    bool argumentsAreTakenWhereTheyAre = isOffUsualPath;
    Vector<LValue, 8> children = arguments;
    if (argumentsAreTakenWhereTheyAre) {
        for (LValue& child : children) {
            if (child->hasInt32())
                child = m_out.constInt64(static_cast<uint32_t>(child->asInt32()));
        }
    }
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    {
        SetForScope isReporting(m_isReportingThatFactDoesNotHold, entry == Entry::operationAOTTypedFactDoesNotHold);
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
        if (argumentsAreTakenWhereTheyAre) {
            patchpoint->append(ConstrainedValue(argument, ValueRep::ColdAny));
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
    // Off the usual path the stub keeps every register, so that the usual path need not keep what it holds out of the call's way, and brings a
    // frame for a function that has none.
    if (isOffUsualPath) {
        bool returnsDouble = type == Double;
        bool returnsValue = type != Void && !returnsDouble;
        bool stubChecksException = mayThrow && !m_watchingHandler;
        patchpoint->clobber(*registersChangedBy(Stub::ColdOperationValue));
        if (!stubChecksException && type == Void)
            patchpoint->clobberLate(RegisterSet { GPRInfo::returnValueGPR });
        if (LoweringContext::coldSitesForceFrame() || (!hasStubsForFunctionsWithoutFrame && m_watchingHandler)) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else if (!hasStubsForFunctionsWithoutFrame)
            m_context.holdsColdSites = true;
        if (returnsValue)
            patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        else if (returnsDouble)
            patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
        patchpoint->setGenerator([context = &m_context, stubChecksException, withInstance, returnsValue, returnsDouble, immediate, site, argumentsAreTakenWhereTheyAre](CCallHelpers& jit, const StackmapGenerationParams& params) {
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
            // The kinds that are entered often have a variant that saves no FP register, two further on.
            if (withInstance && !returnsDouble) {
                bool holdsFPR = false;
                RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
                    holdsFPR |= reg.isFPR();
                });
                if (!holdsFPR)
                    stub = static_cast<Stub>(static_cast<unsigned>(stub) + 2);
            }
            // Each is followed by its variant for a caller without a frame.
            bool makesFrame = isLeaf && !hasStubsForFunctionsWithoutFrame;
            if (isLeaf && !makesFrame)
                stub = static_cast<Stub>(static_cast<unsigned>(stub) + 1);
            ArgumentMoves moves;
            unsigned numberOfChildren = argumentsAreTakenWhereTheyAre ? params.value()->numChildren() : 0;
            unsigned destinationIndex = withInstance;
            for (unsigned i = 0; i < numberOfChildren; ++i) {
                B3::Type typeOfChild = params.value()->child(i)->type();
                if (typeOfChild == Double)
                    continue;
                GPRReg destination = operationArgumentGPR(destinationIndex++);
                const ValueRep& rep = params[params.size() - numberOfChildren + i];
                if (rep.isGPR()) {
                    if (rep.gpr() != destination)
                        appendArgumentMove(moves, destination, ArgumentSource::Register, rep.gpr());
                    continue;
                }
                if (rep.isConstant()) {
                    appendArgumentMove(moves, destination, ArgumentSource::Constant, rep.value());
                    continue;
                }
                RELEASE_ASSERT(rep.isStack() && !isLeaf);
                appendArgumentMove(moves, destination, typeOfChild == Int32 ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
            }
            if (makesFrame)
                jit.emitFunctionPrologue();
            if (moves.isEmpty())
                context->stubCalls.call(jit, stub, immediate, site);
            else
                context->stubCalls.call(jit, stub, immediate, site, moves);
            if (makesFrame)
                jit.emitFunctionEpilogueWithEmptyFrame();
        });
        if (mayThrow && !stubChecksException)
            branchToLandingPad(patchpoint, type == Void ? GPRInfo::returnValueGPR : exceptionGPR);
        return patchpoint;
    }
    m_context.emitsCalls = true;
    bool branchesToLandingPad = mayThrow && withInstance && m_watchingHandler;
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
        stub = Stub::StringReplaceRegExpStringThatMayFindNothing;
    if (entry == Entry::operationAOTTypedRegExpExec && stub == Stub::OperationValueWithInstance)
        stub = Stub::RegExpExecThatAsksBodyFirst;
    if (entry == Entry::operationAOTTypedNewRegExpLiteral && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::NewRegExpLiteral : Stub::UncheckedNewRegExpLiteral;
    if (entry == Entry::operationAOTTypedRegExpTest && stub == Stub::OperationValueWithInstance)
        stub = Stub::RegExpTestThatAsksBodyFirst;
    if (entry == Entry::operationAOTTypedStringToLowerCase && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::StringToLowerCase : Stub::UncheckedStringToLowerCase;
    if (entry == Entry::operationAOTTypedStringToUpperCase && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::StringToUpperCase : Stub::UncheckedStringToUpperCase;
    if (entry == Entry::operationAOTTypedNewTypedArray && (stub == Stub::OperationValueWithInstance || stub == Stub::UncheckedOperationWithInstance))
        stub = stub == Stub::OperationValueWithInstance ? Stub::NewTypedArray : Stub::UncheckedNewTypedArray;
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
        if (stub == Stub::NewRegExpLiteral || stub == Stub::UncheckedNewRegExpLiteral || stub == Stub::StringReplaceRegExpStringThatMayFindNothing || stub == Stub::RegExpExecThatAsksBodyFirst || stub == Stub::RegExpTestThatAsksBodyFirst || stub == Stub::StringToLowerCase || stub == Stub::StringToUpperCase || stub == Stub::UncheckedStringToLowerCase || stub == Stub::UncheckedStringToUpperCase || stub == Stub::NewTypedArray || stub == Stub::UncheckedNewTypedArray)
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

uint32_t Lowering::indexOfCallee(FunctionID function)
{
    return m_context.calleeIndices.ensure(function.index(), [&] {
        m_context.knownCallees.append(keyOf(function));
        return m_context.knownCallees.size() - 1;
    }).iterator->value;
}

// The call being lowered is all that is left for a general entry to do, and what comes of it is what the entry yields, as it is.
bool Lowering::mayJumpTo(FunctionID function, const Vector<LValue, 8>& arguments) const
{
    if (!m_function.isGenericEntrypoint() || !m_currentBlock || m_currentIndexInBlock + 1 != m_currentBlock->values.size())
        return false;
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Return || terminator.value != m_currentBlock->values.last() || m_function.at(terminator.value).opcode != Opcode::CallDirect || m_function.at(terminator.value).function() != function)
        return false;
    Rep rep = resultRepOf(m_program, function);
    // What lies in this frame has to be there while the callee runs.
    for (ValueID argument : m_function.at(terminator.value).arguments) {
        if (LayoutID view = typeOf(argument).onlyView(); (view && m_program.at(view).mayBeInFrame) || m_function.mayPointIntoFrame.get(originOf(argument).index()))
            return false;
    }
    if (resultRepOf(m_program, m_id) != Rep::JSValue || (rep != Rep::JSValue && rep != Rep::Cell))
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (LValue argument : arguments)
        ++(argument->type() == Double ? numberOfFPRs : numberOfGPRs);
    return numberOfGPRs <= numberOfDirectArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
}

void Lowering::jumpToFunction(uint32_t callee, const Vector<LValue, 8>& arguments)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (LValue argument : arguments)
        patchpoint->append(ConstrainedValue(argument, argument->type() == Double ? ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) : ValueRep::reg(directArgumentGPR(nextGPR++))));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setGenerator([context = &m_context, callee](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->emitFrameTeardown(jit, params.proc().code());
        context->stubCalls.jumpToFunction(jit, callee);
    });
    // What is still lowered for the block is not reached.
    m_out.appendTo(m_out.newBlock());
}

LValue Lowering::callFunction(FunctionID function, LType type, const Vector<LValue, 8>& arguments, FunctionID summedUpAs)
{
    m_context.emitsCalls = true;
    uint32_t callee = indexOfCallee(function);
    // What goes on the stack takes a whole word there.
    Vector<LValue, 8> widened;
    for (LValue argument : arguments)
        widened.append(argument->type() == Int32 ? m_out.zeroExt(argument, Int64) : argument);
    PatchpointValue* patchpoint = m_out.patchpoint(type);
    // Where no handler watches, what follows the call only runs if the call comes back. It is pure only if it never throws either.
    auto& summary = m_program.at(summedUpAs ? summedUpAs : function);
    OperationEffects effects { summary.writes, true, summary.writesShapeWords };
    if (!m_watchingHandler && summary.writes != Writes::Pure)
        effects = { std::max(summary.writesIfItReturns, Writes::Nothing), true, summary.writesShapeWordsIfItReturns };
    applyEffects(patchpoint, effects);
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    unsigned onStack = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        LValue argument = arguments[i];
        if (argument->type() == Double && nextFPR < FPRInfo::numberOfArgumentRegisters)
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++))));
        else if (argument->type() != Double && nextGPR < numberOfDirectArgumentGPRs)
            patchpoint->append(ConstrainedValue(argument, ValueRep::reg(directArgumentGPR(nextGPR++))));
        else
            patchpoint->append(ConstrainedValue(widened[i], ValueRep::stackArgument(static_cast<int32_t>(onStack++ * sizeof(Register)))));
    }
    if (onStack)
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(onStack * sizeof(Register)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    if (type.isTuple()) {
        patchpoint->resultConstraints.clear();
        patchpoint->resultConstraints.appendVector(registersOfResults(m_program.at(function)));
    } else if (type == Double)
        patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    else if (type != Void)
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, callee, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        stubCalls->callFunction(jit, callee, site);
    });
    unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

void Lowering::callFunctionFromColdBlock(FunctionID function)
{
    uint32_t callee = indexOfCallee(function);
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    auto& summary = m_program.at(function);
    OperationEffects effects { summary.writes, true, summary.writesShapeWords };
    if (!m_watchingHandler && summary.writes != Writes::Pure)
        effects = { std::max(summary.writesIfItReturns, Writes::Nothing), true, summary.writesShapeWordsIfItReturns };
    applyEffects(patchpoint, effects);
    patchpoint->clobber(*registersChangedBy(Stub::ColdCallFunction));
    if (!hasStubsForFunctionsWithoutFrame) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    }
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, callee, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        context->stubCalls.call(jit, isLeaf ? Stub::LeafColdCallFunction : Stub::ColdCallFunction, site);
        // Not run in line: the stub calls it, and returns behind it.
        context->stubCalls.jumpToFunction(jit, callee);
    });
    unwindToLandingPad(patchpoint, site);
}

// The operands: the callee, `this`, the arguments.
static std::optional<Stub> stubThatAsksFor(MethodAskedByStub method, bool methodIsRead)
{
    static_assert(static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 1 == static_cast<unsigned>(Stub::ArrayPushOrCallValue) && static_cast<unsigned>(Stub::SetAddOrCallMethod) + 1 == static_cast<unsigned>(Stub::SetAddOrCallValue));
    static_assert(static_cast<unsigned>(Stub::SetAddOrCallMethod) == static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(MethodAskedByStub::SetAdd) - static_cast<unsigned>(MethodAskedByStub::ArrayPush)));
    if (method == MethodAskedByStub::None)
        return std::nullopt;
    return static_cast<Stub>(static_cast<unsigned>(Stub::ArrayPushOrCallMethod) + 2 * (static_cast<unsigned>(method) - static_cast<unsigned>(MethodAskedByStub::ArrayPush)) + methodIsRead);
}

Boxed Lowering::callValue(Stub stub, const Vector<LValue, 8>& operands, OperationEffects effects, LValue word, MethodAskedByStub method)
{
    m_context.emitsCalls = true;
    unsigned count = operands.size() - 2;
    RELEASE_ASSERT(count <= numberOfArgumentGPRs);
    RELEASE_ASSERT(method == MethodAskedByStub::None || stub == Stub::CallValue);
    if (method == MethodAskedByStub::None && stub == Stub::CallValue && operands[1]->hasInt64() && operands[1]->asInt64() == static_cast<int64_t>(JSValue::encode(jsUndefined())))
        stub = Stub::CallValueWithoutThis;
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
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, count, site, asks = stubThatAsksFor(method, true)](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (asks)
            stubCalls->call(jit, *asks, site);
        else
            stubCalls->call(jit, stub, count, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Boxed(patchpoint);
}

// The arguments are what a list holds: Stub::ApplyArray, Stub::SpreadArray.
Boxed Lowering::callValueWithArray(Stub stub, Boxed callee, Boxed thisValue, Boxed list)
{
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

// The arguments are in memory, which is this function's or lives as long as it runs: how many, and where the first is.
Boxed Lowering::callValueWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list, LValue wordOfConstructor)
{
    m_context.emitsCalls = true;
    LValue extendedCount = m_out.zeroExtPtr(count.value());
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
    patchpoint->append(ConstrainedValue(callee.value(), ValueRep::reg(calleeGPR)));
    patchpoint->append(ConstrainedValue(thisValue.value(), ValueRep::reg(thisGPR)));
    patchpoint->append(ConstrainedValue(extendedCount, ValueRep::reg(argumentGPR(0))));
    patchpoint->append(ConstrainedValue(list.value(), ValueRep::reg(argumentGPR(1))));
    if (wordOfConstructor)
        patchpoint->append(ConstrainedValue(wordOfConstructor, ValueRep::reg(stubTemporaryGPRs[2])));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->clobberLate(registersClobberedByCalls());
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, site, stub = wordOfConstructor ? Stub::ConstructClosureList : Stub::CallList](CCallHelpers& jit, const StackmapGenerationParams&) {
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

// As lowerEntryParameters() takes them.
Boxed Lowering::callListEntry(FunctionID entry, LValue closure, Boxed thisValue, const Arguments& arguments)
{
    m_context.emitsCalls = true;
    uint32_t callee = indexOfCallee(entry);
    auto [count, list] = argumentBuffer(arguments);
    LValue extendedCount = m_out.zeroExtPtr(count.value());
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    // It reads the arguments from memory, so it is never pure.
    auto& summary = m_program.at(entry);
    OperationEffects effects { std::max(summary.writes, Writes::Nothing), true, summary.writesShapeWords };
    if (!m_watchingHandler)
        effects = { std::max(summary.writesIfItReturns, Writes::Nothing), true, summary.writesShapeWordsIfItReturns };
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
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& call = m_function.at(id);
                if (call.opcode != Opcode::CallDirect || call.arguments.isEmpty() || call.isJumpToContinuation())
                    continue;
                ValueID rest = call.arguments.last();
                if (m_function.at(rest).opcode != Opcode::NewArray || numberOfUsesOf(rest) != 1 || !block.values.contains(rest))
                    continue;
                FunctionID entry = listEntryFor(call.function());
                auto& callee = m_program.at(call.function());
                if (!entry || !callee.captureTypes.isEmpty() || call.arguments.size() != callee.blocks[0].parameters.size())
                    continue;
                // Whoever calls this function has summed it up with the function, not with its entry.
                auto& summary = m_program.at(entry);
                if (summary.writes > callee.writes || summary.writesIfItReturns > callee.writesIfItReturns || summary.writesShapeWords > callee.writesShapeWords || summary.writesShapeWordsIfItReturns > callee.writesShapeWordsIfItReturns)
                    continue;
                bool isCoercedAgain = false;
                for (unsigned i = 0; i + 1 < callee.parameterTypes.size(); ++i)
                    isCoercedAgain |= callee.parameterTypes[i] && callee.reasonToHoldParameter(i) == CastReason::As && m_program.at(callee.parameterTypes[i]).hasObjects();
                if (isCoercedAgain)
                    continue;
                m_restArraysPassedInMemory.set(rest.index());
                m_listEntryOfCall.add(id.index(), entry);
            }
        }
    }
    return m_restArraysPassedInMemory.get(array.index());
}

LValue Lowering::callAllocationStub(Stub stub, const Vector<std::pair<LValue, GPRReg>, 4>& operands, OperationEffects effects, bool mayThrow, bool isOffUsualPath)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, effects);
    RegisterSet taken;
    for (auto& [value, reg] : operands) {
        patchpoint->append(ConstrainedValue(value, ValueRep::reg(reg)));
        taken.add(reg, IgnoreVectors);
    }
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    if (auto changed = registersChangedBy(stub)) {
        static_assert(hasStubsForFunctionsWithoutFrame == isARM64());
        if (!hasStubsForFunctionsWithoutFrame) {
            if (!isOffUsualPath || LoweringContext::coldSitesForceFrame() || m_watchingHandler) {
                m_context.emitsCalls = true;
                patchpoint->clobberLate(RegisterSet { callMarkerGPR });
            } else
                m_context.holdsColdSites = true;
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
    patchpoint->setGenerator([context = &m_context, stub, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasStubsForFunctionsWithoutFrame && hasNoFrame;
        bool makesFrame = !hasStubsForFunctionsWithoutFrame && hasNoFrame;
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (makesFrame)
            jit.emitFunctionPrologue();
        context->stubCalls.call(jit, isLeaf ? static_cast<Stub>(static_cast<unsigned>(stub) + 1) : stub, site);
        if (makesFrame)
            jit.emitFunctionEpilogueWithEmptyFrame();
    });
    if (mayThrow)
        unwindToLandingPad(patchpoint, site);
    return patchpoint;
}

Lowering::CharactersOfSubstring Lowering::charactersOfSubstring(CellPointer string)
{
    PatchpointValue* patchpoint = m_out.patchpoint(m_proc.addTuple({ Int64, Int64, Int64 }));
    applyEffects(patchpoint, { Writes::Nothing, false });
    patchpoint->append(ConstrainedValue(string.value(), ValueRep::reg(stubTemporaryGPRs[3])));
    patchpoint->resultConstraints = { ValueRep::reg(stubTemporaryGPRs[3]), ValueRep::reg(stubTemporaryGPRs[0]), ValueRep::reg(stubTemporaryGPRs[4]) };
    if (auto changed = registersChangedBy(Stub::CharactersOfSubstring)) {
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
            emitCharactersOfSubstring(jit);
        else
            context->stubCalls.call(jit, Stub::CharactersOfSubstring, CallSite { });
    });
    auto result = [&](int32_t index) {
        return m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), Int64, patchpoint, index);
    };
    return { result(0), m_out.castToInt32(result(1)), m_out.castToInt32(result(2)) };
}

CellPointer Lowering::allocate(uint32_t site, Integer number, FieldInit fieldInit)
{
    // Allocations are counted in State::allocate().
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        return AOT_TYPED_OPERATION(AllocSlow, low32(number));
    RELEASE_ASSERT(fieldInit != FieldInit::FromTemplate || m_program.allocations[site].numberOfLiterals);
    Stub stub = fieldInit == FieldInit::FromTemplate ? Stub::AllocateTypedObjectFromTemplate : isObjectLayout(m_program.at(m_program.allocations[site].layout).kind) ? Stub::AllocateTypedObject : Stub::AllocateTypedCell;
    return CellPointer(callAllocationStub(stub, { { number.value(), stubTemporaryGPRs[0] } }, { Writes::RuntimeState, false }, false));
}

// The cell and the butterfly of a new array with contiguous storage, both allocated inline. The slots from the length up to the capacity
// are cleared. The caller stores the elements and then fences.
std::pair<CellPointer, Pointer> Lowering::allocateArray(Word32 length, Word32 vectorLength, LBasicBlock slowPath)
{
    // Zero once arrays are no longer allocated with this structure.
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

// An array literal, or the array for a rest parameter where the arguments are known.
CellPointer Lowering::newArray(const Arguments& elements)
{
    auto outOfLine = [&] {
        auto [count, list] = argumentBuffer(elements);
        return AOT_TYPED_OPERATION(NewArray, count, list);
    };
    constexpr unsigned maxInlineLength = Program::maxLengthOfArrayLiteralMadeInline;
    unsigned length = elements.size();
    bool isColdByText = !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id);
    if (length > maxInlineLength || (length > maxArrayElementsInRegisters && isColdByText))
        return outOfLine();
    Vector<Boxed, 8> boxed;
    for (ValueID element : elements)
        boxed.append(in<Boxed>(element));
    if (length <= maxArrayElementsInRegisters) {
        Vector<std::pair<LValue, GPRReg>, 4> operands;
        for (unsigned i = 0; i < length; ++i)
            operands.append({ boxed[i].value(), GPRInfo::toArgumentRegister(1 + i) });
        return CellPointer(callAllocationStub(arrayLiteralStub(length), operands, effectsOfOperation<operationAOTTypedNewArray>(), true));
    }
    // With the header the butterfly is a multiple of 16 bytes.
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

// The array for a rest parameter of an entry point that takes its arguments in memory.
CellPointer Lowering::newArrayFromBuffer(Word32 count, Pointer buffer)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock copyElement = m_out.newBlock();
    LBasicBlock copied = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Word32 leastVectorLength = select(isZero(count), word32(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY), word32(BASE_CONTIGUOUS_VECTOR_LEN));
    Word32 vectorLength = bitOr(select(below(leastVectorLength, count), count, leastVectorLength), word32(1));
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

// A number is the usual key: the argument, or `undefined` where there is none, as neither an array nor an `arguments` object has or
// inherits anything else under a number. For any other key the array is made and asked.
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
        // There are never that many.
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

CellPointer Lowering::stringConstant(StringID string)
{
    // What the engine has from the start.
    auto bytes = m_program.string(string);
    if (bytes.empty())
        return CellPointer(emptyString());
    if (bytes.size() == 1 && isASCII(bytes[0]))
        return CellPointer(singleCharacterString(m_out.constInt32(bytes[0])));
    uint32_t key = string.index() + 1;
    // The string of a number is one cell for as long as the Instance lives, which keeps it. Making it, the first time, is nothing the program can tell, nor the collector: State::string(). So it is a function of the number,
    // which B3 moves out of loops and takes for another of the same number.
    static constexpr uint64_t keyOfStrings = 1ull << 32;
    auto callStubInBlock = [&](Stub stub, Stub stubInBlock1, uint32_t number) {
        if (uint32_t block = number >> 16; isARM64() && block && block < numberOfBlocksOfTypedStrings) {
            stub = static_cast<Stub>(static_cast<unsigned>(stubInBlock1) + numberOfTypedStringStubsInBlock * (block - 1));
            number &= 0xffff;
        }
        return callAllocationStub(stub, { { m_out.constInt64(number), stubTemporaryGPRs[0] } }, { Writes::Pure, false }, false);
    };
    if (m_program.stringsMadeOnce.get(string.index()) && m_program.runsOnce(m_id) && !m_isInLoop) {
        LValue cell = callStubInBlock(Stub::TypedStringMadeOnce, Stub::TypedStringMadeOnceInBlock1, string.index());
        cell->as<PatchpointValue>()->pureFunctionKey = keyOfStrings | key;
        return CellPointer(cell);
    }
    if (!m_isInLoop || Options::useAOTWideReferences()) {
        LValue cell = callStubInBlock(Stub::TypedString, Stub::TypedStringInBlock1, key);
        cell->as<PatchpointValue>()->pureFunctionKey = keyOfStrings | key;
        return CellPointer(cell);
    }
    // Where it stays in a loop, the slot where the string is looked for first is looked at in place: see startInStringTable().
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { Writes::Pure, false });
    patchpoint->pureFunctionKey = keyOfStrings | key;
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    // As callAllocationStub() has it.
    if (auto changed = registersChangedBy(Stub::TypedString)) {
        if (!hasStubsForFunctionsWithoutFrame) {
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
        constexpr GPRReg slots = stubTemporaryGPRs[2];
        constexpr GPRReg at = stubTemporaryGPRs[3];
        static_assert(noOverlap(result, slots, at, instanceGPR));
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfTypedStrings()), slots);
        jit.load32(CCallHelpers::Address(slots, offsetOfMaskInStringTable), at);
        jit.and32(CCallHelpers::TrustedImm32(static_cast<int32_t>(startInStringTable(key))), at);
        jit.load64(CCallHelpers::BaseIndex(slots, at, CCallHelpers::TimesEight), result);
        CCallHelpers::Jump isElsewhere = jit.branch32(CCallHelpers::NotEqual, result, CCallHelpers::TrustedImm32(static_cast<int32_t>(key)));
        jit.urshift64(CCallHelpers::TrustedImm32(32), result);
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfCellCageBase()), slots);
#if CPU(ARM64)
        jit.addLeftShift64(slots, result, CCallHelpers::TrustedImm32(cell32Shift), result);
#else
        jit.lshift64(CCallHelpers::TrustedImm32(cell32Shift), result);
        jit.add64(slots, result);
#endif
        CCallHelpers::Label done = jit.label();
        bool isLeaf = hasStubsForFunctionsWithoutFrame && context->hasNoFrame(params.proc().code());
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            isElsewhere.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(key)), stubTemporaryGPRs[0]);
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
    // VMTraps keeps it at the marker for as long as an asynchronous event is pending.
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
    // A limit is an address.
    static_assert(static_cast<int64_t>(StackManager::StopRequestMarkerValue) < 0);
    m_out.branch(m_out.lessThan(limit, m_out.int64Zero), rarely(slowPath), usually(continuation));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(PollSlow);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// The values, boxed, one after the other on the stack: their number and the address of the first.
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

// What the function is handed as the parameter of that number of its entry block: what it is declared to take. The representation of that
// is how it is passed. See lowerEntryParameters().
const Type& Lowering::typePassedAs(const Function& callee, unsigned index) const
{
    if (index < callee.captureTypes.size())
        return m_program.at(callee.captureTypes[index]);
    unsigned at = index - callee.captureTypes.size();
    TypeID declared;
    if (callee.has(Format::FunctionHasThis) && !at--)
        declared = callee.thisType;
    else if (at < callee.parameterTypes.size())
        declared = callee.parameterTypes[at];
    else
        return m_program.at(callee.at(callee.blocks[0].parameters[index]).type);
    return m_program.at(declared ? declared : m_program.types.anything);
}

// A call of a function of the program. An argument that is of the type of the parameter, which is what the verifier has seen to for a
// CallDirect, is handed over as it is.
Operand Lowering::callCompiled(FunctionID function, const Vector<Argument, 8>& arguments, const Type& result, LValue* callWithSeveralResults)
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty())
        return failWith("a call of a function without a body"_s);
    unsigned numberOfParameters = callee.blocks[0].parameters.size();
    RELEASE_ASSERT(arguments.size() <= numberOfParameters);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Vector<LValue, 8> passed;
    for (unsigned i = 0; i < numberOfParameters; ++i) {
        Argument given = i < arguments.size() ? arguments[i] : Argument { undefined(), &undefinedType };
        passed.append(ensure(given.operand, *given.type, typePassedAs(callee, i)).value);
    }
    if (!callee.resultTypes.isEmpty()) {
        if (!callWithSeveralResults)
            return failWith("a call whose several results nobody takes"_s);
        Vector<B3::Type> types;
        for (TypeID type : callee.resultTypes)
            types.append(b3TypeOf(repOf(m_program.at(type))));
        *callWithSeveralResults = callFunction(function, m_proc.addTuple(WTF::move(types)), passed);
        return ensure(undefined(), undefinedType, result);
    }
    Rep rep = resultRepOf(m_program, function);
    if (mayJumpTo(function, passed)) {
        jumpToFunction(indexOfCallee(function), passed);
        return ensure(Operand(rep, m_out.int64Zero), m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), result);
    }
    LValue value = callFunction(function, b3TypeOf(rep), passed);
    if (rep == Rep::Void)
        return ensure(undefined(), undefinedType, result);
    return ensure(Operand(rep, value), m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), result);
}

// Each in the representation of the type it is DECLARED with: that is how the continuation takes it, see lowerEntryParameters(). Behind a nil test typeOf() and get() know more.
Operand Lowering::jumpToContinuation(const Value& value)
{
    Vector<LValue, 8> passed;
    for (ValueID id : value.arguments)
        passed.append(in(id, repOf(m_program.at(m_function.at(id).type))).value);
    for (ValueID id : value.arguments) {
        RELEASE_ASSERT_WITH_MESSAGE(!m_function.at(originOf(id)).isClosureInFrame() && !m_function.environmentsInFrame.get(originOf(id).index()), "AOT: what lies in this frame is handed to a continuation");
    }
    m_context.knownCallees.append(keyOfContinuation(m_id, value.continuation()));
    jumpToFunction(m_context.knownCallees.size() - 1, passed);
    return placeholder(value.rep);
}

Operand Lowering::lowerCallDirect(ValueID result, const Value& value)
{
    if (value.isJumpToContinuation())
        return jumpToContinuation(value);
    if (auto row = m_program.moduleEvaluationOfFunction.find(value.function().index()); row != m_program.moduleEvaluationOfFunction.end()) {
        LType yielded = b3TypeOf(resultRepOf(m_program, m_program.moduleEvaluator));
        if (!m_isInLoop && !m_program.isHotOutsideItsLoops(m_id)) {
            callFunction(m_program.moduleEvaluatorIfNeeded, yielded, { m_out.constInt64(row->value) }, value.function());
            return convert(undefined(), typeOf(result), value.rep);
        }
        LBasicBlock evaluate = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue isEvaluated = marked(m_out.load64(addressOf(m_program.moduleEvaluations[row->value].isEvaluated)), IsSafe);
        m_out.branch(m_out.equal(isEvaluated, m_out.constInt64(JSValue::encode(jsBoolean(true)))), usually(continuation), rarely(evaluate));
        inColdBlock(evaluate, [&] {
            callFunction(m_program.moduleEvaluatorForCallFromOutside(), yielded, { m_out.constInt64(row->value) }, value.function());
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
        Boxed yielded = callListEntry(m_listEntryOfCall.get(result.index()), m_out.int64Zero, hasThis ? in<Boxed>(value.arguments[0]) : undefined(), passed);
        return ensure(yielded, m_program.at(callee.resultType ? callee.resultType : m_program.types.anything), typeOf(result));
    }
    // Nobody looks at what it yields: the stub puts every register back.
    if (m_program.at(value.function()).kind == FunctionKind::Module && value.arguments.isEmpty() && !numberOfUsesOf(result) && m_out.m_block->frequency() < 1) {
        callFunctionFromColdBlock(value.function());
        return placeholder(value.rep);
    }
    Vector<Argument, 8> arguments;
    for (ValueID id : value.arguments)
        arguments.append(argument(id));
    LValue call = nullptr;
    Operand yielded = callCompiled(value.function(), arguments, typeOf(result), &call);
    if (call)
        m_callsWithSeveralResults.add(result.index(), call);
    return yielded;
}

Vector<B3::ValueRep, 4> Lowering::registersOfResults(const Function& function) const
{
    Vector<B3::ValueRep, 4> result;
    unsigned nextGPR = 0;
    unsigned nextFPR = 0;
    for (TypeID type : function.resultTypes)
        result.append(b3TypeOf(repOf(m_program.at(type))) == Double ? ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) : ValueRep::reg(argumentGPR(nextGPR++)));
    return result;
}

// Ends the block.
void Lowering::returnResults(const Value& results)
{
    auto& types = m_function.resultTypes;
    RELEASE_ASSERT(results.opcode == Opcode::Results && results.arguments.size() == types.size());
    auto registers = registersOfResults(m_function);
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
    LValue extracted = m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), b3TypeOf(rep), m_callsWithSeveralResults.get(value.child(0).index()), static_cast<int32_t>(value.resultIndex()));
    return ensure(Operand(rep, extracted), type, m_program.at(value.type));
}

FunctionID Lowering::genericEntrypointFor(FunctionID function)
{
    auto found = m_program.genericEntrypoints.find(function.index());
    if (found == m_program.genericEntrypoints.end()) {
        fail("a function value without a general entry"_s);
        return function;
    }
    return found->value;
}

// Where a closure keeps what it captures: first the references, which the collector follows, then what holds no cell, each as it is kept in
// a register. See Closure::captures().
static unsigned numberOfReferencesAmong(const Program& program, std::span<const TypeID> types)
{
    unsigned count = 0;
    for (TypeID type : types)
        count += holdsCells(repOf(program.at(type)));
    return count;
}

static unsigned slotOfCapture(const Program& program, std::span<const TypeID> types, unsigned index)
{
    if (holdsCells(repOf(program.at(types[index]))))
        return numberOfReferencesAmong(program, types.first(index));
    return numberOfReferencesAmong(program, types) + index - numberOfReferencesAmong(program, types.first(index));
}

// That the value is a closure of the function was found by a test, which the load stays behind. The word is written once, before anyone
// sees the closure.
Operand Lowering::captureOfClosure(CellPointer closure, const Function& callee, unsigned index)
{
    Rep rep = repOf(m_program.at(callee.captureTypes[index]));
    auto& word = (m_program.makesSlimClosures(callee) ? m_heaps.AOTTypedSlimClosure_captures : m_heaps.AOTTypedClosure_captures)[slotOfCapture(m_program, callee.captureTypes.span(), index)];
    if (b3TypeOf(rep) == Double)
        return Operand(rep, marked(m_out.loadDouble(closure.value(), word), IsFixed));
    if (rep == Rep::Int32)
        return Operand(rep, marked(m_out.load32(closure.value(), word), IsFixed));
    return Operand(rep, marked(m_out.load64(closure.value(), word), IsFixed));
}

// A function that captures nothing as a value: one object, which is made when it is first asked for.
CellPointer Lowering::closureConstant(FunctionID function)
{
    uint32_t general = genericEntrypointFor(function).index();
    return lazilyInitialized<CellPointer>(Stub::TypedClosure, Instance::offsetOfTypedClosures(), general, [&] {
        return AOT_TYPED_OPERATION(ClosureConstant, word32(general));
    });
}

// A function value that captures something.
CellPointer Lowering::lowerNewClosure(const Value& value, LValue variableOfClosureMadeOnce, Store storeOfVariable, bool* isStoredByStub)
{
    auto& callee = m_program.at(value.function());
    // Only the run time enters the resume closure of a coroutine, at Stub::EnterResume, which passes the frame and the resume mode as compiled
    // code does: its entry is the function itself, and nothing can call it.
    bool isResumeClosure = m_program.isResumeFunction(callee);
    FunctionID general = isResumeClosure ? value.function() : genericEntrypointFor(value.function());
    // A class object captures the environment of its class, whether its constructor takes that or not.
    Vector<TypeID, 8> types;
    if (callee.kind == FunctionKind::Constructor)
        types.append(m_function.at(value.arguments[0]).type);
    else
        types.appendVector(callee.captureTypes);
    RELEASE_ASSERT(types.size() == value.arguments.size() && types.size() <= Closure::maxNumberOfCaptures);
    // Whoever reads a word goes by the type of the capture, untested.
    Vector<Operand, 8> captures;
    for (unsigned i = 0; i < types.size(); ++i)
        captures.append(ensure(get(value.arguments[i]), typeOf(value.arguments[i]), m_program.at(types[i])));
    // The collector has the two numbers by function.
    RELEASE_ASSERT(m_program.referencesAndCapturesOfClosures(callee) == std::pair(numberOfReferencesAmong(m_program, types.span()), static_cast<unsigned>(types.size())));
    bool isInFrame = value.isClosureInFrame();
    auto callStub = [&](Vector<std::pair<LValue, GPRReg>, 4>& operands, size_t size) {
        if (isInFrame) {
            m_context.emitsCalls = true;
            operands.append({ m_out.lockedStackSlot(size), GPRInfo::returnValueGPR });
        }
        if (variableOfClosureMadeOnce && !isInFrame && types.size() == 1 && captures[0].rep == Rep::Cell && value.arguments[0] == value.child(0) && (storeOfVariable == Store::JSValue || storeOfVariable == Store::OptionalCell32) && !Options::validateAOTTypedFacts()) {
            *isStoredByStub = true;
            operands.append({ variableOfClosureMadeOnce, GPRInfo::toArgumentRegister(3) });
            if (Options::verboseAOTCompilation())
                dataLogLn("AOT: f", m_id.index(), ": a closure made once is stored by the stub that makes it");
            return CellPointer(callAllocationStub(storeOfVariable == Store::OptionalCell32 ? Stub::NewTypedClosureOnce32 : Stub::NewTypedClosureOnce, operands, { Writes::RuntimeState, false }, false));
        }
        return CellPointer(callAllocationStub(static_cast<Stub>(static_cast<unsigned>(isInFrame ? Stub::InitializeTypedClosure0 : Stub::NewTypedClosure0) + 2 * types.size()), operands, { Writes::RuntimeState, false }, false));
    };
    if (m_program.makesSlimClosures(callee)) {
        auto& goesBy = m_program.at(general);
        uint64_t request = general.index() | static_cast<uint64_t>(slimClosureKindOf(static_cast<uint8_t>(goesBy.kind) | goesBy.declaredAs)) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
        if (!isInFrame && Options::countAOTTypedAllocations()) [[unlikely]]
            AOT_TYPED_OPERATION(CountClosure, word32(general.index()), word32(types.size()));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(static_cast<int64_t>(request)).value(), stubTemporaryGPRs[0] } };
        for (unsigned i = 0; i < types.size(); ++i) {
            LValue word = captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : b3TypeOf(captures[i].rep) == Double ? m_out.bitCast(captures[i].value, Int64) : captures[i].value;
            operands.append({ word, GPRInfo::toArgumentRegister(2 + slotOfCapture(m_program, types.span(), i)) });
        }
        return callStub(operands, SlimClosure::allocationSize(types.size()));
    }
    // As a rule one stub makes it. What follows is for the others, and says what the stub does.
    bool isMadeByStub = !isResumeClosure && callee.kind != FunctionKind::Constructor && !closureStructureOf(callee.declaredAs) && types.size() <= ClosureRequest::maxNumberOfCaptures
        && !Options::countAOTTypedAllocations();
    for (auto& capture : captures)
        isMadeByStub &= b3TypeOf(capture.rep) != Double;
    if (isMadeByStub) {
        auto& goesBy = m_program.at(general);
        uint64_t request = ClosureRequest::encode(general.index(), FunctionTables::kindOfExecutable(static_cast<uint8_t>(goesBy.kind) | goesBy.declaredAs, false), isInFrame ? 0 : m_program.allocationOfClosure(types.size()));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { integer(static_cast<int64_t>(request)).value(), stubTemporaryGPRs[0] } };
        for (unsigned i = 0; i < types.size(); ++i)
            operands.append({ captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : captures[i].value, GPRInfo::toArgumentRegister(2 + slotOfCapture(m_program, types.span(), i)) });
        return callStub(operands, Closure::allocationSize(types.size()));
    }
    // A constant of the build.
    Pointer entry(marked(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedFunctions()), static_cast<ptrdiff_t>(general.index()) * sizeof(uint64_t))), IsFixed));
    auto& closuresGoBy = m_program.at(general);
    CellPointer executable(fixedPointer(Instance::offsetOfTypedExecutable(FunctionTables::kindOfExecutable(static_cast<uint8_t>(closuresGoBy.kind) | closuresGoBy.declaredAs, false))));
    if (isResumeClosure)
        entry = Pointer(m_out.bitOr(entry.value(), m_out.constIntPtr(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        if (!isInFrame)
            AOT_TYPED_OPERATION(CountClosure, word32(general.index()), word32(types.size()));
    CellPointer cell = isInFrame ? cellInFrame(Closure::allocationSize(types.size())) : allocate(m_program.allocationOfClosure(types.size()));
    if (isInFrame)
        storeHeader(cell.value(), m_out.bitOr(m_out.zeroExtPtr(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID())), m_out.constInt64(static_cast<int64_t>(static_cast<uint64_t>(TypeInfoBlob(NonArray, TypeInfo(JSFunctionType, Closure::StructureFlags)).blob()) << 32))));
    // That is as of a plain function. An async function or a generator function has another prototype, and so another Structure.
    if (unsigned which = callee.kind == FunctionKind::Constructor ? closureStructureOfClassObjects : closureStructureOf(callee.declaredAs))
        storeField(cell, m_heaps.JSCell_structureID, Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID(which))));
    storeField(cell, m_heaps.JSObject_butterfly, integer(0));
    storeField(cell, m_heaps.JSCallee_scope, CellPointer(fixedPointer(Instance::offsetOfGlobalObject())));
    storeField(cell, m_heaps.JSFunction_executableOrRareData, executable);
    for (unsigned i = 0; i < types.size(); ++i) {
        auto& word = m_heaps.AOTTypedClosure_captures[slotOfCapture(m_program, types.span(), i)];
        if (b3TypeOf(captures[i].rep) == Double)
            m_out.storeDouble(captures[i].value, cell.value(), word);
        else
            m_out.store64(captures[i].rep == Rep::Int32 ? m_out.zeroExtPtr(captures[i].value) : captures[i].value, cell.value(), word);
    }
    // Last: it says how many of the words above the collector follows.
    storeField(cell, m_heaps.AOTTypedClosure_entry, entry);
    if (isInFrame)
        return cell;
    fenceAfterInitialization();
    return cell;
}

// The closure that the variable of the environment holds, which is made when it is first asked for. Nothing else looks at the variable, so
// it is run-time state to everyone else: see fieldHoldsClosureMadeOnce.
CellPointer Lowering::lowerNewClosureOnce(const Value& value)
{
    CellPointer environment = in<CellPointer>(value.child(0));
    auto& field = m_program.at(typeOf(value.child(0)).onlyView()).fields[value.field()];
    RELEASE_ASSERT(repOf(field.store) == Rep::JSValue && !field.isOutOfLine);
    TypedPointer variable = m_out.address(m_heaps.AOTTyped_runtimeState, environment.value(), field.offset);
    LBasicBlock isNotMade = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    Boxed held = load(field.store, variable, 0, environment.value()).as<Boxed>();
    ValueFromBlock made = m_out.anchor(held.value());
    m_out.branch(isCell(held).value(), unsure(continuation), unsure(isNotMade));
    m_out.appendTo(isNotMade);
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
    return CellPointer(m_out.phi(Int64, made, fresh));
}

// A call of a value of which nothing is known.
Boxed Lowering::callGenerally(Boxed callee, Boxed thisValue, const Arguments& arguments, StringID nameOfMethod, MethodAskedByStub method)
{
    if (nameOfMethod) {
        // The stub could only say what the value is, not what was asked for.
        requireOrThrowAt(isCell(callee), ThrowKind::NotAFunction, Operand(thrownNil(callee)), nameOfMethod.index());
        callee = box(asCell(callee));
    }
    if (arguments.size() > numberOfArgumentGPRs) {
        // The operation that was called here said the name for whatever cannot be called.
        if (nameOfMethod) {
            requireOrThrowAt(isCallable(asCell(callee)), ThrowKind::NotAFunction, Operand(callee), nameOfMethod.index());
        }
        auto [count, list] = argumentBuffer(arguments);
        return callValueWithList(callee, thisValue, count, list);
    }
    Vector<LValue, 8> operands { callee.value(), thisValue.value() };
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    return callValue(Stub::CallValue, operands, { }, nullptr, method);
}

Boxed Lowering::callMethodGenerally(Boxed object, StringID name, const Arguments& arguments)
{
    // See emitGetById().
    bool isReadThroughSlot = name != m_length && !m_builtins.callOrApply(m_program.builtinName(name)) && !bindIfNamed(name) && !bufferAccessorNamed(name) && !isNamedLastIndex(name);
    if (isReadThroughSlot && arguments.size() <= numberOfArgumentGPRs) {
        if (auto slot = allocateSite(name, 0))
            return callMethodThroughSlot(object, *slot, arguments);
    }
    return callGenerally(emitGetById(object, name, true), object, arguments, name);
}

Boxed Lowering::callMethodThroughSlot(Boxed object, unsigned slot, const Arguments& arguments, MethodAskedByStub method)
{
    m_context.emitsCalls = true;
    unsigned count = arguments.size();
    Vector<LValue, 8> operands;
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    LValue address = slotAddress(slot, SlotAddressIsFor::UsualPath).value();
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, { });
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
    CallSite siteOfCall { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, count, siteOfCall, asks = stubThatAsksFor(method, false)](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (asks)
            stubCalls->call(jit, *asks, siteOfCall);
        else
            stubCalls->call(jit, Stub::CallMethod, count, siteOfCall);
    });
    unwindToLandingPad(patchpoint, siteOfCall);
    return Boxed(patchpoint);
}

// Whether a call of a closure of the function with so many arguments comes down to a call of the function: what its general entry does
// in between is to take the environment out of the closure, to hold `this` and the arguments to the types of the parameters, with
// `undefined` for those that are missing, and to make an array of the rest.
bool Lowering::mayBeCalledInPlaceOfItsClosure(FunctionID function, unsigned numberOfArguments) const
{
    auto& callee = m_program.at(function);
    if (callee.blocks.isEmpty() || callee.isGenericEntrypoint() || callee.takesItsClosure || callee.rampForClosures || !m_program.genericEntrypoints.contains(function.index()))
        return false;
    // To call a class throws.
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
            if (!Type::engineClass(m_builtins.arrayClass()).isWithin(m_program.at(type), m_program))
                return false;
        } else if (i >= numberOfArguments && !undefinedType.isWithin(m_program.at(type), m_program))
            return false;
        // An array that is coerced is gone through even though it is an array: see coerce().
        if (callee.reasonToHoldParameter(i) != CastReason::As)
            continue;
        for (TypeID elements = m_program.at(type).elements; elements; elements = m_program.at(elements).elements) {
            if (!m_program.at(elements).views.isEmpty())
                return false;
        }
    }
    return true;
}

// The cell is a closure of the function. Where `this` or an argument is not of the type of the parameter: to the block, before anything is done.
Operand Lowering::callClosure(FunctionID function, CellPointer closure, ValueID thisValue, const Arguments& arguments, const Type& result, LBasicBlock otherwise)
{
    auto& callee = m_program.at(function);
    auto& undefinedType = m_program.at(m_program.types.undefined);
    Type arrayType = Type::engineClass(m_builtins.arrayClass());
    Vector<Argument, 8> all;
    // What the general entry would coerce is for the general entry, also where it would pass for the type as it is.
    auto pass = [&](ValueID id, CastReason reason) {
        auto& type = typePassedAs(callee, all.size());
        SetForScope isCoerced(m_foreignObjectsDoNotFit, reason == CastReason::As && m_program.annotatedPlaceLooksAtForeignObjects(type));
        SetForScope useCastTest(m_useCastTest, reason == CastReason::As);
        all.append(Argument { fitOrElse(get(id), typeOf(id), type, otherwise), &type });
    };
    for (unsigned i = 0; i < callee.captureTypes.size(); ++i)
        all.append(Argument { captureOfClosure(closure, callee, i), &typePassedAs(callee, i) });
    if (callee.has(Format::FunctionHasThis))
        pass(thisValue, CastReason::Store);
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i) {
        if (i < arguments.size())
            pass(arguments[i], callee.reasonToHoldParameter(i));
        else
            all.append(Argument { undefined(), &undefinedType });
    }
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        all.append(Argument { newArray(rest), &arrayType });
    }
    return callCompiled(function, all, result);
}

// Whether resolveCalls() has put a test of the value in front of a call of it (IsClosureOf, IsBuiltinFunction). The call that is left is the
// one for where the test fails.
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

// An indirect call. No `this`: `undefined`.
Operand Lowering::lowerIndirectCall(ValueID callee, ValueID thisValue, const Arguments& arguments, const Type& type, StringID nameOfMethod)
{
    auto generally = [&] {
        return ensure(callGenerally(in<Boxed>(callee), thisValue ? in<Boxed>(thisValue) : undefined(), arguments, nameOfMethod), type);
    };
    // A closure of one function of the program.
    FunctionID expected = m_function.at(callee).predictedClosure;
    if (!expected || isGuardedCallee(callee) || !m_program.isHotOutsideItsLoops(m_id))
        return generally();
    FunctionID listEntry = listEntryFor(expected);
    if (!listEntry && (!mayBeCalledInPlaceOfItsClosure(expected, arguments.size()) || (!thisValue && m_program.at(expected).has(Format::FunctionHasThis))))
        return generally();
    auto& functionType = m_program.at(m_program.types.function);
    return ifAsExpected({ }, repOf(type), [&](LBasicBlock otherwise) {
        Boxed boxed = in<Boxed>(callee);
        if (!typeOf(callee).isWithin(functionType, m_program))
            orElse(isCell(boxed, typeOf(callee)), otherwise);
        CellPointer closure = asCell(boxed);
        closureOfOrElse(closure, expected, false, otherwise);
        if (listEntry)
            return ensure(callListEntry(listEntry, closure.value(), thisValue ? in<Boxed>(thisValue) : undefined(), arguments), type);
        return callClosure(expected, closure, thisValue, arguments, type, otherwise);
    }, generally);
}

Word32 Lowering::lowerIsBuiltinFunction(const Value& value)
{
    return equal(in<Boxed>(value.child(0)), box(builtinCell(m_builtins.cellOf(value.builtin()))));
}

// `base.call` or `base.apply`. A closure with the structure it was allocated with has no property of that name and inherits from
// Function.prototype, which is immutable. A class may declare `static call`, which does not show in its structure.
const Function* Lowering::functionDefining(ValueID id) const
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
    auto isFullClosureThatIsCalledOrElse = [&] {
        orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID()))), throughCache);
        if (!definedAs || definedAs->kind == FunctionKind::Constructor)
            orElse(isZero(bitAnd(loadField<Integer>(cell, m_heaps.AOTTypedClosure_entry), integer(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit)))), throughCache);
    };
    if (definedAs && m_program.makesSlimClosures(*definedAs))
        orElse(isUnreifiedSlimClosure(cell), throughCache);
    else if (definedAs)
        isFullClosureThatIsCalledOrElse();
    else {
        LBasicBlock inheritsIt = m_out.newBlock();
        LBasicBlock isNotSlim = m_out.newBlock();
        m_out.branch(isUnreifiedSlimClosure(cell).value(), usually(inheritsIt), rarely(isNotSlim));
        m_out.appendTo(isNotSlim);
        isFullClosureThatIsCalledOrElse();
        m_out.jump(inheritsIt);
        m_out.appendTo(inheritsIt);
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
    Boxed found = method ? *method : emitGetById(callee, name, false, functionDefining(function));
    ValueID thisValue = arguments.isEmpty() ? ValueID() : arguments[0];
    return eitherOperand(equal(found, box(builtinCell(m_builtins.cellOf(builtin)))), repOf(type), [&] {
        if (m_program.builtinName(name) == m_builtins.applyName())
            return lowerApply(function, thisValue, arguments.size() > 1 ? arguments[1] : ValueID(), type);
        Arguments passed;
        if (arguments.size() > 1)
            passed.append(arguments.span().subspan(1));
        if (auto call = lowerCallOfBuiltinFunction(result, function, thisValue, passed))
            return *call;
        return lowerIndirectCall(function, thisValue, passed, type);
    }, [&] {
        return ensure(callGenerally(found, callee, arguments), type);
    });
}

// An indirect call with the arguments in a list. Apply: `f.apply(thisArg, list)`, the list is array-like; none, `undefined` or `null` is no
// arguments. CallWithSpread: `f(...list)`, the list is iterable. The two are the same for an array as the engine makes them, which finds the
// iteration of arrays: up to as many elements as there are argument registers are loaded from it straight into the registers, by a stub.
Operand Lowering::lowerApply(ValueID callee, ValueID thisValue, ValueID list, const Type& type, LanguageOperation operation)
{
    if (operation == LanguageOperation::Apply && (!list || typeOf(list).isUndefinedOrNull()))
        return lowerIndirectCall(callee, thisValue, { }, type);
    // Only the general way asks for the function then.
    std::optional<bool> isMax = isMathMaxOrMin(callee);
    if (!isMax) {
        Boxed function = in<Boxed>(callee);
        Boxed receiver = thisValue ? in<Boxed>(thisValue) : undefined();
        return ensure(callValueWithArray(operation == LanguageOperation::Apply ? Stub::ApplyArray : Stub::SpreadArray, function, receiver, in<Boxed>(list)), type);
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
    join.add(yieldNumber(minOrMaxOfElements(*isMax, array, butterfly, length, slowPath), type));

    m_out.appendTo(slowPath);
    fetchFunction();
    LValue buffer = transientBuffer(3 * sizeof(EncodedJSValue));
    LValue inBuffer[] = { function.value(), receiver.value(), boxedList.value() };
    for (unsigned i = 0; i < std::size(inBuffer); ++i)
        m_out.store64(inBuffer[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    join.add(ensure(AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(operation)), word32(std::size(inBuffer)), Pointer(buffer)), type));
    return join.finish();
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
    // `o.m(..)` written as a read and a call: what is no function is reported by the name, as a call by name reports it.
    StringID nameOfMethod;
    if (auto& read = definitionOf(callee); thisValue && read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(thisValue))
        nameOfMethod = read.string();
    else if (thisValue)
        nameOfMethod = value.string();
    // What call resolution leaves where the callee has just been found nil: it can only throw, and needs no argument for that. No handler watches it: its block ends behind it.
    if (auto found = m_foundNilIn.find(m_currentBlockID.index()); found != m_foundNilIn.end() && m_currentBlock->terminator.kind == TerminatorKind::Unreachable && m_currentIndexInBlock + 1 == m_currentBlock->values.size() && !Options::auditAOTTypedAssertions()) {
        ValueID tested = callee;
        while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
            tested = m_function.at(tested).child(0);
        if (found->value == tested) {
            throwAt(ThrowKind::NotAFunction, Operand(in<Boxed>(callee)), nameOfMethod ? nameOfMethod.index() : ThrowSite::none);
            return placeholder(repOf(type));
        }
    }
    if (value.methodAskedByStub != MethodAskedByStub::None && thisValue && value.arguments.size() <= numberOfArgumentGPRs)
        return ensure(callGenerally(in<Boxed>(callee), in<Boxed>(thisValue), value.arguments, nameOfMethod, value.methodAskedByStub), type);
    auto generally = [&] {
        return lowerIndirectCall(callee, thisValue, value.arguments, type, nameOfMethod);
    };
    if (auto call = lowerCallOfBuiltinFunction(result, callee, thisValue, value.arguments))
        return *call;
    // A function of the engine: a global one, or what `o` was found to have under the name.
    if (BuiltinName global = globalDefinedBy(callee)) {
        if (auto builtin = expectedBuiltin(BuiltinKind::Function, { }, global, value.arguments))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
    } else if (auto& read = definitionOf(callee); read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(thisValue)) {
        if (auto call = isGuardedCallee(callee) ? std::nullopt : lowerCallOrApply(result, thisValue, read.string(), in<Boxed>(callee), value.arguments))
            return *call;
        if (auto bound = isGuardedCallee(callee) ? std::nullopt : lowerBind(thisValue, read.string(), in<Boxed>(callee), value.arguments, type))
            return *bound;
        if (auto access = isGuardedCallee(callee) ? std::nullopt : lowerBufferAccess(thisValue, read.string(), in<Boxed>(callee), value.arguments, type))
            return *access;
        if (auto builtin = expectedBuiltin(BuiltinKind::Method, thisValue, read.string(), value.arguments))
            return lowerExpectedBuiltin(result, *builtin, in<Boxed>(callee), generally);
    }
    return generally();
}

// Zero: the cell is no closure of the program.
Integer Lowering::entryWordIfClosure(CellPointer cell)
{
    Word32 isAsAllocated = equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID())));
    return either<Integer>(isAsAllocated, [&] {
        return loadField<Integer>(cell, m_heaps.AOTTypedClosure_entry);
    }, [&] {
        return Integer(callAllocationStub(Stub::TypedClosureEntry, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
    });
}

// Both are closures of the program. The site remembers what it made for the new target the last time, as a CreateThis whose callee is known does: see newObjectWithProperties().
std::optional<Pointer> Lowering::cacheOfCreateThis()
{
    constexpr unsigned numberOfSlots = 3;
    if (m_context.sites.size() + numberOfSlots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
        return std::nullopt;
    unsigned slot = m_context.sites.size();
    for (unsigned i = 0; i < numberOfSlots; ++i)
        m_context.sites.append(Site { });
    return slotAddress(slot);
}

CellPointer Lowering::createThis(CellPointer constructor, CellPointer newTarget, std::optional<Pointer> cacheOfSite)
{
    if (!cacheOfSite)
        cacheOfSite = cacheOfCreateThis();
    if (!cacheOfSite)
        return AOT_TYPED_OPERATION(CreateThis, constructor, newTarget);
    Pointer cache = *cacheOfSite;
    auto inCache = [&](size_t offset) {
        // A collection may clear the slot.
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

// [[Construct]] of a closure with ClosureWord::constructsOrdinaryObjectBit.
Boxed Lowering::constructClosure(CellPointer constructor, Integer word, CellPointer newTarget, const Arguments& arguments, std::optional<Pointer> cacheOfSite, std::optional<uint32_t> numberOfArgumentsToSkip)
{
    Boxed made = box(createThis(constructor, newTarget, cacheOfSite));
    Vector<LValue, 8> operands { constructor.value(), made.value() };
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    Boxed returned = numberOfArgumentsToSkip ? callValueWithList(box(constructor), made, restLength(*numberOfArgumentsToSkip), offsetBy(m_argumentBuffer, integer(*numberOfArgumentsToSkip), 3), word.value()) : callValue(Stub::ConstructClosure, operands, { }, word.value());
    return either<Boxed>(isCell(returned), [&] {
        return select(aboveOrEqual(typeOfCell(asCell(returned)), word32(ObjectType)), returned, made);
    }, [&] {
        return made;
    });
}

// [[Construct]] of a closure with ClosureWord::isDerivedConstructorBit.
Boxed Lowering::constructDerivedClosure(CellPointer constructor, Integer word, Boxed newTarget, const Arguments& arguments)
{
    Vector<LValue, 8> operands { constructor.value(), undefined().value(), newTarget.value() };
    for (ValueID argument : arguments)
        operands.append(in<Boxed>(argument).value());
    return callValue(Stub::ConstructClosure, operands, { }, word.value());
}

// The parent is what the class object inherits from, which its structure says. The usual one is a function or a class of untyped code.
Operand Lowering::lowerConstructSuper(ValueID result, const Value& value)
{
    Boxed classObject = in<Boxed>(value.child(0));
    Boxed newTarget = in<Boxed>(value.child(1));
    auto& object = m_program.at(m_program.types.object);
    std::optional<uint32_t> skipped;
    if (value.opcode == Opcode::ConstructSuperForwardVarargs)
        skipped = value.numberOfArgumentsToSkip();
    auto generally = [&] {
        Vector<LValue, 4> inBuffer { classObject.value(), newTarget.value(), box(skipped ? AOT_TYPED_OPERATION(NewArray, restLength(*skipped), offsetBy(m_argumentBuffer, integer(*skipped), 3)) : newArray(value.arguments)).value() };
        if (value.child(2))
            inBuffer.append(in<Boxed>(value.child(2)).value());
        LValue buffer = transientBuffer(inBuffer.size() * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < inBuffer.size(); ++i)
            m_out.store64(inBuffer[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        return AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(LanguageOperation::SuperConstruct)), word32(inBuffer.size()), Pointer(buffer));
    };
    if (value.arguments.size() > numberOfArgumentGPRs || isCompact(10))
        return ensure(generally(), object, typeOf(result));
    LBasicBlock otherwise = m_out.newBlock();
    Join join(m_out, Rep::JSValue, Int64);
    orElse(isCell(classObject, typeOf(value.child(0))), otherwise);
    // The empty value: the object itself says what it inherits from.
    Boxed parent = value.child(2) ? in<Boxed>(value.child(2)) : Boxed(m_out.load64(structureOf(classObject.value()), m_heaps.Structure_prototype));
    orElse(logicalAnd(isNotEmpty(parent), isCell(parent)), otherwise);
    LBasicBlock isNotOrdinary = m_out.newBlock();
    Integer word = entryWordIfClosure(asCell(parent));
    orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::constructsOrdinaryObjectBit)))), isNotOrdinary);
    orElse(isCell(newTarget, typeOf(value.child(1))), otherwise);
    orElse(logicalNot(isZero(entryWordIfClosure(asCell(newTarget)))), otherwise);
    join.add(constructClosure(asCell(parent), word, asCell(newTarget), value.arguments, std::nullopt, skipped));
    m_out.appendTo(isNotOrdinary);
    auto& givenNewTarget = m_function.at(value.child(1));
    if (skipped ? givenNewTarget.opcode == Opcode::GetArgument && givenNewTarget.immediates[0] + 1 == *skipped : value.arguments.size() < numberOfArgumentGPRs) {
        orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::isDerivedConstructorBit)))), otherwise);
        join.add(skipped ? callValueWithList(parent, undefined(), restLength(*skipped - 1), offsetBy(m_argumentBuffer, integer(*skipped - 1), 3), word.value()) : constructDerivedClosure(asCell(parent), word, newTarget, value.arguments));
    } else
        m_out.jump(otherwise);
    m_out.appendTo(otherwise);
    join.add(generally());
    return ensure(join.finish().as<Boxed>(), object, typeOf(result));
}

// Nothing is known of the callee. The usual one is a function or a class of untyped code, which says so in its entry word.
// See State::newTarget. It is taken: what is left is the empty value, which reads as `undefined`.
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
    if (FunctionID body = m_program.bodyOfConstruction(value); body && !m_isInLoop && mayLeaveConstructionToBody(m_program, m_id, m_function, value)) {
        if (auto cache = cacheOfCreateThis()) {
            Vector<Argument, 8> arguments { argument(value.child(0)) };
            for (ValueID id : value.arguments)
                arguments.append(argument(id));
            arguments.append(Argument { Operand(Rep::Int64, cache->value()), &m_program.at(m_program.types.integer) });
            return callCompiled(body, arguments, typeOf(result));
        }
    }
    std::optional<Pointer> cacheOfSite;
    if (value.child(1))
        cacheOfSite = Pointer(in<Integer>(value.child(1)).value());
    Boxed callee = in<Boxed>(value.child(0));
    auto& object = m_program.at(m_program.types.object);
    auto generally = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        // What comes of it is an object, whatever was constructed.
        return AOT_TYPED_OPERATION(Construct, callee, count, list);
    };
    if (value.arguments.size() > numberOfArgumentGPRs || isCompact(10))
        return ensure(generally(), object, typeOf(result));
    LBasicBlock otherwise = m_out.newBlock();
    Join join(m_out, Rep::JSValue, Int64);
    orElse(isCell(callee, typeOf(value.child(0))), otherwise);
    CellPointer closure = asCell(callee);
    LBasicBlock isNotOrdinary = m_out.newBlock();
    Integer word = entryWordIfClosure(closure);
    orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::constructsOrdinaryObjectBit)))), isNotOrdinary);
    join.add(constructClosure(closure, word, closure, value.arguments, cacheOfSite));
    m_out.appendTo(isNotOrdinary);
    if (value.arguments.size() < numberOfArgumentGPRs) {
        orElse(logicalNot(isZero(bitAnd(word, integer(1LL << ClosureWord::isDerivedConstructorBit)))), otherwise);
        join.add(constructDerivedClosure(closure, word, callee, value.arguments));
    } else
        m_out.jump(otherwise);
    m_out.appendTo(otherwise);
    join.add(generally());
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

// The methods of a class that is laid out cannot be replaced, but for those the program is seen to replace, which are left to the call by
// name. An object of the class can be given a property of the name of any other only if that is the method, bound to the object.
std::optional<Lowering::MethodTargets> Lowering::methodTargets(LayoutID view, StringID name) const
{
    static constexpr unsigned maxTargets = 32;
    static constexpr unsigned maxShapes = 256;
    auto& members = m_program.at(view).members;
    if (members.isEmpty() || members.size() > maxShapes)
        return std::nullopt;
    bool viewHasField = !!m_program.at(view).fieldNamed(name);
    MethodTargets targets;
    for (LayoutID member : members) {
        FunctionID function;
        if (m_program.at(member).fieldNamed(name)) {
            if (!viewHasField)
                return std::nullopt;
        } else {
            if (m_program.at(member).mayHaveOtherPrototype)
                return std::nullopt;
            for (LayoutID ancestor = member; ancestor && !function; ancestor = m_program.at(ancestor).base) {
                if (m_program.at(ancestor).hasMethodOfUnknownName)
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
            return target.function == function;
        });
        if (at == notFound) {
            if (targets.size() == maxTargets)
                return std::nullopt;
            at = targets.size();
            targets.append({ function, { } });
        }
        targets[at].shapes.append(m_program.at(member).shape);
    }
    return targets;
}

// What is passed for a parameter, held to its type as the general entry of the function holds it: an annotation coerces. So a call does
// the same whichever way it gets to the function.
Lowering::Argument Lowering::argumentFor(const Function& callee, unsigned parameter, ValueID id)
{
    TypeID declared = callee.parameterTypes[parameter];
    if (!declared || callee.reasonToHoldParameter(parameter) != CastReason::As)
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
    // The object is of a class that has the method, so it is what the method takes `this` as.
    if (callee.has(Format::FunctionHasThis))
        all.append({ thisObject, &typePassedAs(callee, 0) });
    bool hasRest = callee.has(Format::FunctionHasRestParameter);
    unsigned numberOfPlainParameters = callee.parameterTypes.size() - hasRest;
    for (unsigned i = 0; i < numberOfPlainParameters; ++i)
        all.append(i < arguments.size() ? argumentFor(callee, i, arguments[i]) : Argument { undefined(), &undefinedType });
    if (hasRest) {
        Arguments rest;
        for (unsigned i = numberOfPlainParameters; i < arguments.size(); ++i)
            rest.append(arguments[i]);
        all.append({ newArray(rest), &arrayType });
    }
    return callCompiled(function, all, result);
}

// What resolveCalls() has left.
Operand Lowering::lowerCallMethod(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    auto& type = typeOf(result);
    LayoutID view = receiver.onlyView();
    auto targets = view ? methodTargets(view, name) : std::nullopt;
    if (!targets && value.methodAskedByStub != MethodAskedByStub::None) {
        if (auto slot = allocateSite(name, 0))
            return ensure(callMethodThroughSlot(in<Boxed>(object), *slot, value.arguments, value.methodAskedByStub), type);
    }
    if (!targets) {
        if (auto call = lowerCallOrApply(result, object, name, std::nullopt, value.arguments))
            return *call;
        if (auto bound = lowerBind(object, name, std::nullopt, value.arguments, type))
            return *bound;
        if (auto access = lowerBufferAccess(object, name, std::nullopt, value.arguments, type))
            return *access;
        auto generally = [&] {
            return ensure(callMethodGenerally(in<Boxed>(object), name, value.arguments), type);
        };
        if (auto builtin = expectedBuiltin(BuiltinKind::Method, object, name, value.arguments))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
        return generally();
    }

    CellPointer cell = nonNil(object, name);
    return ifOfMember(cell, object, view, repOf(type), [&] {
        return callMethodOfMember(cell, receiver, *targets, value, type);
    }, [&] {
        return ensure(callMethodGenerally(box(cell), name, value.arguments), type);
    });
}

// The object is of a member of the view the receiver is looked at as.
Operand Lowering::callMethodOfMember(CellPointer cell, const Type& receiver, const MethodTargets& targets, const Value& value, const Type& type)
{
    auto call = [&](const MethodTarget& target) {
        if (target.function)
            return callMethod(target.function, cell, value.arguments, type);
        // A function that is kept in a field is read as the field is, and called as any value is.
        Boxed callee = readProperty(cell, receiver.onlyView(), *fieldFor(receiver, value.string()), m_program.at(m_program.types.anything)).as<Boxed>();
        return ensure(callGenerally(callee, box(cell), value.arguments), type);
    };
    if (targets.size() == 1)
        return call(targets.first());

    // By shape. The object is of a member of the view, so the function most of them have needs no case.
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
        Operand yielded = call(targets.at(i));
        RELEASE_ASSERT(yielded.rep == rep);
        results.append(m_out.anchor(yielded.value));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), results));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
