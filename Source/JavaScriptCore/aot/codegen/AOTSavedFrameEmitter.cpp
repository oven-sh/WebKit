/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuildContext.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTFrameMapEmitter.h"
#include "AOTGenerator.h"
#include "AOTHeldOperands.h"
#include "AOTIRFunction.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSavedFrame.h"
#include "AOTStubCatalog.h"
#include "AOTTypeTestEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "MarkedSpace.h"
#include <array>
#include <optional>
#include <ranges>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(awaitConditionsOnState.size() == 2);
static_assert(awaitConditionsOnState[0].word == AwaitConditionWord { AwaitConditionBase::Instance, AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue), sizeof(void*), false });
static_assert(awaitConditionsOnState[0].test == AwaitConditionTest::EqualsFramePointer);
static_assert(awaitConditionsOnState[1].word == AwaitConditionWord { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfStart(), sizeof(size_t), false });
static_assert(awaitConditionsOnState[1].other == AwaitConditionWord { AwaitConditionBase::MicrotaskQueue, MicrotaskQueue::offsetOfEnd(), sizeof(size_t), false });
static_assert(awaitConditionsOnState[1].test == AwaitConditionTest::EqualsOther);
static_assert(allMembersOf<YieldResult> == std::array { YieldResult::Sent, YieldResult::Mode } && allMembersOf<StepGeneratorResult> == std::array { StepGeneratorResult::Value, StepGeneratorResult::Status });
static_assert(sizeof(Generator) <= MarkedSpace::preciseCutoff && !(sizeof(Generator) % MarkedSpace::sizeStep));
static_assert(Generator::offsetOfSavedFrame() == sizeof(Generator::Base) && Generator::offsetOfState() == Generator::offsetOfSavedFrame() + sizeof(uint64_t) && Generator::offsetOfState() + sizeof(uint64_t) == sizeof(Generator));
static_assert(Generator::offsetOfHasOtherPrototype() == Generator::offsetOfState() + sizeof(uint32_t) && !static_cast<int32_t>(Generator::State::SuspendedAtStart));

CoroutineKind SavedFrameEmitter::kindOfFunction() const
{
    const Function& coroutine = function();
    RELEASE_ASSERT(coroutine.isCoroutine() && isBodyOfCoroutine(m_unit.role, true));
    if (!coroutine.has(FunctionFlags::IsGenerator))
        return CoroutineKind::AsyncFunction;
    return coroutine.has(FunctionFlags::IsAsync) ? CoroutineKind::AsyncGenerator : CoroutineKind::Generator;
}

LValue SavedFrameEmitter::cellOrZero(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    Boxed boxed = in<Boxed>(driver);
    return m_out.select(isCell(boxed).value(), boxed.value(), m_out.int64Zero);
}

LValue SavedFrameEmitter::driverAsPassed(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    return in<Boxed>(driver).value();
}

StubCallResult SavedFrameEmitter::suspend(const ValueContext& value, StubKey key, std::initializer_list<LValue> operands, ValueID driverID, RunsCodeOfAwaited runsCodeOfAwaited)
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSummary& summary = catalog.summaryOf(key);
    const StubSignature& signature = summary.signature;
    RELEASE_ASSERT(operands.size() && signature.numberOfOperands == operands.size() + 1);
    Reg siteWordRegister = signature.operands[operands.size()];
    bool hasMode = signature.result == StubResult::TwoGPRs;
    RELEASE_ASSERT(hasMode || signature.result == StubResult::GPR);
    LType type = hasMode ? m_proc.addTuple({ Machine::Int64, Machine::Int64 }) : LType(Machine::Int64);

    RegisterSet lateClobbered = summary.clobbered;
    lateClobbered.merge(RegisterSet::macroClobberedGPRs());
    lateClobbered.add(siteWordRegister, IgnoreVectors);
    CallPatchpointRequest request {
        .resultType = type,
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = lateClobbered,
        .effects = catalog.effectsOf(key),
        .argumentBufferAccess = ArgumentBufferAccess::ReadsAndWrites,
        .holdsUniqueRecord = true,
    };

    LValue driver = driverAsPassed(driverID);
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    Machine::PatchOperation* patch = calls.createCallPatchpoint(value, request);
    patch->setNeedsFrameRecord(true);
    unsigned operandIndex = 0;
    for (LValue operand : operands) {
        RELEASE_ASSERT(operand);
        patch->append(operand, Machine::ValueRep::reg(signature.operands[operandIndex++]));
    }
    unsigned driverChild = m_proc.resultCount(type) + patch->numChildren();
    patch->setOperandEncodedAsFrameWordIndex(patch->numChildren());
    patch->append(driver, Machine::ValueRep::LateColdAny);
    FrameMapEmitter::Roots roots = emitter<FrameMapEmitter>().appendRoots(patch, value);
    std::span<const FrameSlotKind> kindsOfRoots = m_proc.arena().copySpan<FrameSlotKind>(roots.kinds.span());
    patch->resultConstraints = { Machine::ValueRep::reg(signature.firstResult) };
    if (hasMode)
        patch->resultConstraints.append(Machine::ValueRep::reg(signature.secondResult));

    Origin originOfSuspension = value.origin();
    originOfSuspension.isTailCall = false;
    uint32_t site = calls.addSite(value, { .origin = originOfSuspension, .callerHasLeft = runsCodeOfAwaited == RunsCodeOfAwaited::BehindFunction });
    patch->setGenerator([records = &m_records, build = &build(), subject = subjectOf(value), operation = nameOf(value.opcode()), key, site, firstRoot = roots.firstChild, kindsOfRoots, driverChild, siteWordGPR = siteWordRegister.gpr()](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FrameMapEmitter::Roots rootsOfSite { firstRoot, Vector<FrameSlotKind, 8>(kindsOfRoots) };
        std::optional<uint64_t> word = FrameMapEmitter::siteWordFor(records->frameMaps, params, rootsOfSite, driverChild);
        if (size_t numberOfWords = records->frameMaps.words.size(); numberOfWords > build->limit(LimitID::MaxFrameMapWords)) {
            if (!std::exchange(records->frameMaps.didReportLimit, true))
                build->reportLimitExceeded(LimitID::MaxFrameMapWords, numberOfWords, subject);
            jit.breakpoint();
            return;
        }
        if (!word) {
            build->diagnostics().add(InternalError { .kind = InternalErrorKind::FrameMapNotEncodable, .function = subject.function, .block = subject.block, .value = subject.value, .location = subject.location, .operation = operation });
            jit.breakpoint();
            return;
        }
        if (*word >> 32)
            jit.move(CCallHelpers::TrustedImm64(*word), siteWordGPR);
        else
            jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(*word)), siteWordGPR);
        if (FrameMapEmitter::savesDeadPreservedRegister(params))
            jit.move(CCallHelpers::TrustedImm32(0), SavedFrame::preservedRegister);
        records->stubCalls.call(jit, key, OptionalIndex { site });
    });
    calls.finishCallPatchpoint(value, patch, site, request);

    if (!hasMode)
        return { patch, nullptr };
    return { m_out.extract(patch, 0), m_out.extract(patch, 1) };
}

// A saved frame resumes at another address, so the register is read where the address is needed. The result must not live across a suspension.
Pointer SavedFrameEmitter::framePointerAtThisPoint()
{
    Machine::PatchOperation* patch = m_out.patchpoint(Machine::pointerType());
    patch->effects = Machine::Effects::none();
    patch->effects.reads = Machine::HeapSet::top();
    patch->setNeedsFrameRecord(true);
    patch->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.move(GPRInfo::callFrameRegister, params[0].gpr());
    });
    return Pointer(patch);
}

void SavedFrameEmitter::generateAwaitMayContinue(const ValueContext& value)
{
    const Machine::AbstractHeap& queueWords = m_heaps.entire(AbstractHeap::RuntimeState);
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isFrameOfLastJob = newBlock();
        Pointer frameThatMayContinue(m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue))));
        branch(equal(frameThatMayContinue, framePointerAtThisPoint()), { BlockEdge(isFrameOfLastJob, targets.taken.edgeClass()), targets.notTaken });

        m_out.appendToContinuation(isFrameOfLastJob);
        LValue queue = m_out.loadPtr(mutableInstanceField(offsetOfMicrotaskQueue));
        Integer start(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfStart())));
        Integer end(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfEnd())));
        branch(equal(start, end), targets);
    });
}

void SavedFrameEmitter::generateAwaitBySavingFrame(const ValueContext& value)
{
    ValueID awaited = value->awaitedOperand();
    ValueID driver = value->promiseOperand();
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncFunction: {
        AwaitedOperand form = m_plan.moduleEvaluation().isBodyResumedByJobOfLoader(function()) ? AwaitedOperand::ValueInModuleBody : AwaitedOperand::Value;
        define(value, Boxed(suspend(value, awaitBySavingFrame(form), { in<Boxed>(awaited).value() }, driver, RunsCodeOfAwaited::BehindFunction).first));
        return;
    }
    case CoroutineKind::AsyncGenerator:
        define(value, Boxed(suspend(value, AOT::suspendGenerator(GeneratorSuspension::AsyncAwait), { cellOrZero(driver), in<Boxed>(awaited).value() }, driver).first));
        return;
    case CoroutineKind::Generator:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAwaitCallBySavingFrame(const ValueContext& value)
{
    CodeUnit callee = conventions().unitEnteredBy(value.value());
    if (kindOfFunction() != CoroutineKind::AsyncFunction || value->role() != CodeRole::CoroutineBody) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    Vector<Operand, 8> arguments;
    for (ValueID argument : value->argumentsOperands().first(value->numberOfVariadicOperands() - numberOfHeldOperandsOf(value.value())))
        arguments.append(get(argument));
    arguments.append(undefined());
    Operand returned = emitter<CallEmitter>().callDirect(value, callee, arguments.span(), { .callerSuspendsAtReturn = true });
    RELEASE_ASSERT(isSameWord(returned.representation, Representation::JSValue));
    define(value, Boxed(suspend(value, awaitCallBySavingFrame(), { returned.value }, value->promiseOperand(), RunsCodeOfAwaited::BehindFunction).first));
}

void SavedFrameEmitter::suspendGenerator(const ValueContext& value, GeneratorSuspension form)
{
    Boxed yielded = in<Boxed>(value->valueOperand());
    ValueID generator = value->generatorOperand();
    defineValueAndWord(value, suspend(value, AOT::suspendGenerator(form), { in<CellPointer>(generator).value(), yielded.value() }, generator));
}

void SavedFrameEmitter::defineValueAndWord(const ValueContext& value, StubCallResult result)
{
    defineResults(value, { Boxed(result.first), low32(Integer(result.second)) });
}

void SavedFrameEmitter::generateYieldBySavingFrame(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::Generator:
        if (generator().keepsValuesInHeapFrame())
            yieldOutOfHeapFrame(value);
        else
            suspendGenerator(value, GeneratorSuspension::Yield);
        return;
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYield);
        return;
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAsyncGeneratorYield(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYieldAfterAwait);
        return;
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ptrdiff_t offsetOfWordInHeapFrame(uint32_t index)
{
    return SavedFrame::offsetOfWords() + static_cast<ptrdiff_t>(index) * static_cast<ptrdiff_t>(sizeof(uint64_t));
}

void SavedFrameEmitter::findHeapWordsAliveAtSuspensions()
{
    if (std::exchange(m_hasLookedForHeapWordsAliveAtSuspensions, true))
        return;
    for (const LiveAtSuspension& record : m_code.suspensions()) {
        for (ValueID live : m_code.liveAt(record.suspension)) {
            if (m_code.at(live).opcode == Opcode::NewHeapWords && !m_heapWordsAliveAtSuspensions.contains(live))
                m_heapWordsAliveAtSuspensions.append(live);
        }
    }
}

bool SavedFrameEmitter::areNamedOnlyByReadsAndStoresOfWords(ValueID made) const
{
    uint32_t usersNotSeen = m_code.useCountOf(made);
    if (usersNotSeen == std::numeric_limits<uint8_t>::max())
        return false;
    static_assert(!OperandIndex::GetHeapFrameWord::frame && !OperandIndex::PutHeapFrameWord::frame);
    uint32_t numberOfBlocks = m_code.numberOfBlocks();
    uint32_t first = m_code.blockOf(made).index();
    for (uint32_t step = 0; usersNotSeen && step < numberOfBlocks; ++step) {
        BlockID block((first + step) % numberOfBlocks);
        if (!m_code.isReached(block))
            continue;
        bool areNamedOtherwise = false;
        for (ValueID id : m_code.valuesOf(block)) {
            const Operation& operation = m_code.at(id);
            bool takesWordsFirst = operation.opcode == Opcode::GetHeapFrameWord || operation.opcode == Opcode::PutHeapFrameWord;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand != made)
                    return;
                if (takesWordsFirst && !operandIndex)
                    --usersNotSeen;
                else
                    areNamedOtherwise = true;
            });
        }
        for (ValueID operand : m_code.at(block).terminator.operands())
            areNamedOtherwise |= operand == made;
        if (m_code.at(block).subject == made)
            --usersNotSeen;
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments())
                areNamedOtherwise |= argument == made;
        });
        if (areNamedOtherwise)
            return false;
    }
    return !usersNotSeen;
}

Machine::StackSlot* SavedFrameEmitter::slotOfHeapWordsInFrame(ValueID frame)
{
    const Operation& made = m_code.at(frame);
    if (made.opcode != Opcode::NewHeapWords || !m_facts.mechanisms().contains(Mechanism::CellsInFrames))
        return nullptr;
    for (const HeapWordsInFrame& words : m_heapWordsInFrame) {
        if (words.made == frame)
            return words.slot;
    }
    findHeapWordsAliveAtSuspensions();
    Machine::StackSlot* slot = nullptr;
    if (!m_heapWordsAliveAtSuspensions.contains(frame) && areNamedOnlyByReadsAndStoresOfWords(frame)) {
        slot = m_proc.addStackSlot(made.numberOfWords() * sizeof(uint64_t));
        slot->setIsDeadAtSuspensions(true);
    }
    m_heapWordsInFrame.append(HeapWordsInFrame { frame, slot });
    return slot;
}

TypedPointer SavedFrameEmitter::wordInFrame(Machine::StackSlot* slot, uint32_t index)
{
    return m_out.address(m_heaps.entire(AbstractHeap::InternalFields), m_out.slotBase(slot), static_cast<ptrdiff_t>(index) * static_cast<ptrdiff_t>(sizeof(uint64_t)));
}

void SavedFrameEmitter::generateNewHeapWords(const ValueContext& value)
{
    uint32_t numberOfWords = value->numberOfWords();
    RELEASE_ASSERT(numberOfWords && numberOfWords <= SavedFrame::maxNumberOfWords);
    if (Machine::StackSlot* slot = slotOfHeapWordsInFrame(value.valueID())) {
        build().statistics().count(StatisticID::AllocateInFramesChanges);
        for (uint32_t index = 0; index < numberOfWords; ++index)
            m_out.store64(m_out.int64Zero, wordInFrame(slot, index));
        define(value, undefined());
        return;
    }
    if (numberOfWords <= SavedFrame::maxWordsAllocatedInStub) {
        define(value, boxCell(CellPointer(callStub(value, AOT::newHeapFrame(), { m_out.constInt64(numberOfWords), m_out.intPtrZero }).first)));
        return;
    }
    CellPointer frame = callOperation<CellPointer>(value, RuntimeOperation::NewHeapFrame, { word32(static_cast<int32_t>(numberOfWords)), Pointer(m_out.intPtrZero) });
    define(value, boxCell(frame));
}

void SavedFrameEmitter::generateGetHeapFrameWord(const ValueContext& value)
{
    if (Machine::StackSlot* slot = slotOfHeapWordsInFrame(value->frameOperand())) {
        defineNarrowed(value, Boxed(m_out.load64(wordInFrame(slot, value->index()))));
        return;
    }
    LValue frame = in<Boxed>(value->frameOperand()).value();
    defineNarrowed(value, Boxed(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame, offsetOfWordInHeapFrame(value->index())))));
}

void SavedFrameEmitter::generatePutHeapFrameWord(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    const Type& storedType = typeOf(operand);
    LValue stored = in<Boxed>(operand).value();
    if (m_cellCageBase && storedType.mayBeUnspecified())
        stored = m_out.select(m_out.equal(stored, m_cellCageBase), m_out.int64Zero, stored);
    if (Machine::StackSlot* slot = slotOfHeapWordsInFrame(value->frameOperand())) {
        m_out.store64(stored, wordInFrame(slot, value->index()));
        return;
    }
    CellPointer frame = unboxCell(in<Boxed>(value->frameOperand()));
    m_out.store64(stored, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame.value(), offsetOfWordInHeapFrame(value->index())));
    if (value->remembersFrame())
        emitter<BarrierEmitter>().barrierAfterStore(value, frame, storedType);
}

bool SavedFrameEmitter::findBlocksBehindFirstYield()
{
    for (BlockID block : generator().order()) {
        for (ValueID id : m_code.valuesOf(block)) {
            if (opcodeOf(id) != Opcode::YieldBySavingFrame)
                continue;
            m_firstYield = id;
            m_blockOfFirstYield = block;
            break;
        }
        if (m_firstYield)
            break;
    }
    if (!m_firstYield || m_code.handlerOf(m_blockOfFirstYield))
        return false;

    Vector<BlockID, 16> worklist { m_blockOfFirstYield };
    while (!worklist.isEmpty()) {
        m_code.forEachEdgeOf(worklist.takeLast(), [&](EdgeID, const Edge& edge) {
            if (!m_blocksBehindFirstYield.set(edge.to.index()))
                worklist.append(edge.to);
        });
    }
    if (m_blocksBehindFirstYield.get(m_blockOfFirstYield.index()))
        return false;
    for (size_t index : m_blocksBehindFirstYield) {
        BlockID block(static_cast<uint32_t>(index));
        if (m_code.at(block).terminator.kind == TerminatorKind::ReturnCall || !generator().blockDominates(m_blockOfFirstYield, block))
            return false;
    }
    return std::ranges::all_of(m_code.suspensions(), [&](const LiveAtSuspension& record) {
        BlockID block = m_code.blockOf(record.suspension);
        return opcodeOf(record.suspension) == Opcode::YieldBySavingFrame && (block == m_blockOfFirstYield || m_blocksBehindFirstYield.get(block.index()));
    });
}

bool SavedFrameEmitter::chooseValuesKeptInHeapFrame(Vector<ValueID, 16>& kept)
{
    auto choose = [&](ValueID id, ValueID suspension, bool isRead) {
        if (!id || id == suspension || m_code.isConstant(id) || !m_code.isNeeded(id) || m_wordsOfKeptValues[id.index()])
            return true;
        const Operation& operation = operationAt(id);
        if (isDeferredClosure(id) || typeOf(id).isOpaque(OpaqueKind::StringBuilder))
            return false;
        bool hasSeveralResults = operation.opcode == Opcode::CallDirect ? conventions().numberOfResults(conventions().unitEnteredBy(operation)) > 1 : numberOfResultsOf(m_program, operation).has_value();
        return !hasSeveralResults && choosesWordFor(formKeptInHeapFrame(id), isRead, id, kept);
    };
    for (const LiveAtSuspension& record : m_code.suspensions()) {
        for (ValueID live : m_code.liveAt(record.suspension)) {
            if (!choose(live, record.suspension, true))
                return false;
        }
        for (ValueID held : heldOperandsOf(operationAt(record.suspension))) {
            if (!choose(held, record.suspension, false))
                return false;
        }
    }
    return kept.size() < SavedFrame::maxNumberOfWords;
}

Representation SavedFrameEmitter::formKeptInHeapFrame(ValueID id) const
{
    return m_generator.isCarriedBoxed(id) ? Representation::JSValue : representationOf(id);
}

bool SavedFrameEmitter::choosesWordFor(Representation representation, bool isRead, ValueID id, Vector<ValueID, 16>& kept)
{
    switch (representation) {
    case Representation::Void:
        return true;
    case Representation::Unchosen:
    case Representation::Cell32:
    case Representation::RawWord:
        return false;
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        if (!isRead)
            return true;
        break;
    case Representation::JSValue:
    case Representation::Cell:
        if (!isRead && !typeOf(id).hasCells())
            return true;
        ++m_numberOfTracedWords;
        break;
    }
    m_wordsOfKeptValues[id.index()] = 1;
    kept.append(id);
    return true;
}

bool SavedFrameEmitter::layOutHeapFrame()
{
    if (kindOfFunction() != CoroutineKind::Generator || !m_facts.mechanisms().contains(Mechanism::TypedOperations))
        return false;
    if (m_code.opcodesOfValues().contains(Opcode::ReleaseHint) || conventions().entryConventionOf(m_unit) != EntryConvention::Direct || m_code.suspensions().size() >= fusedCallEntry)
        return false;
    if (!findBlocksBehindFirstYield())
        return false;

    Machine::Arena& arena = m_proc.arena();
    m_wordsOfKeptValues = arena.createSpan<uint16_t>(m_code.numberOfValues());
    Vector<ValueID, 16> kept;
    if (!chooseValuesKeptInHeapFrame(kept))
        return false;

    m_stretchesWhereNamed = arena.createSpan<uint32_t>(m_code.numberOfValues());
    m_keptValuesAsDefined = arena.createSpan<LValue>(m_code.numberOfValues());
    m_lastStretchesOfBlocks = arena.createSpan<uint32_t>(m_code.numberOfBlocks());
    uint32_t nextTraced = 0;
    uint32_t nextRaw = m_numberOfTracedWords;
    for (ValueID id : kept) {
        m_wordsOfKeptValues[id.index()] = static_cast<uint16_t>((frameSlotKindOf(formKeptInHeapFrame(id)) ? nextTraced : nextRaw)++ + 1);
        if (opcodeOf(id) == Opcode::Parameter)
            m_keptValuesAsDefined[id.index()] = generator().wordOf(id);
    }
    m_numberOfWordsInHeapFrame = std::max(nextRaw, 1u);
    m_slotOfHeapFrame = m_proc.addStackSlot(sizeof(uint64_t));
    return true;
}

LValue SavedFrameEmitter::readHeapFrameOutOfSlot()
{
    return m_out.loadPtr(m_out.address(m_heaps.frameSlots, m_out.slotBase(m_slotOfHeapFrame)));
}

LValue SavedFrameEmitter::heapFrame()
{
    if (!isNamedInThisStretch(std::exchange(m_stretchOfHeapFrame, m_stretch)))
        m_heapFrame = readHeapFrameOutOfSlot();
    return m_heapFrame;
}

TypedPointer SavedFrameEmitter::wordOfHeapFrame(LValue frame, ValueID kept)
{
    return m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame, offsetOfWordInHeapFrame(m_wordsOfKeptValues[kept.index()] - 1));
}

void SavedFrameEmitter::storeIntoHeapFrame(LValue frame, ValueID kept)
{
    LValue stored = generator().wordOf(kept);
    RELEASE_ASSERT(stored);
    if (isTracedInHeapFrame(kept) && typeOf(kept).mayBeUnspecified())
        stored = m_out.select(m_out.equal(stored, m_cellCageBase), m_out.int64Zero, stored);
    m_out.store(stored, wordOfHeapFrame(frame, kept));
    m_valuesStoredInHeapFrame.set(kept.index());
    m_keptValuesAsDefined[kept.index()] = generator().wordOf(kept);
}

LValue SavedFrameEmitter::readOutOfHeapFrame(LValue frame, ValueID kept)
{
    return m_out.load(wordOfHeapFrame(frame, kept), machineTypeOf(formKeptInHeapFrame(kept)));
}

void SavedFrameEmitter::keepInHeapFrame(const ValueContext& defined)
{
    ValueID id = defined.id();
    if (!isKeptInHeapFrame(id) || !standsBehindFirstYield(defined.block()))
        return;
    if (!generator().wordOf(id)) {
        fail(defined, InternalErrorKind::InvalidSuspension);
        return;
    }
    LValue frame = heapFrame();
    storeIntoHeapFrame(frame, id);
    if (isTracedInHeapFrame(id) && typeOf(id).hasCells())
        emitter<BarrierEmitter>().barrier(defined, CellPointer(frame));
}

void SavedFrameEmitter::beginStretchOfHeapFrame(BlockID block, LValue frameHandedIn)
{
    m_stretch = ++m_numberOfStretches;
    BlockID onlyPredecessor = frameHandedIn ? BlockID() : generator().onlyPredecessorOf(block);
    m_stretchInFront = onlyPredecessor ? m_lastStretchesOfBlocks[onlyPredecessor.index()] : 0;
    m_lastStretchesOfBlocks[block.index()] = m_stretch;
    if (frameHandedIn) {
        m_heapFrame = frameHandedIn;
        m_stretchOfHeapFrame = m_stretch;
    }
}

ValueID SavedFrameEmitter::readBackOutOfHeapFrame(BlockID block, ValueID yieldInFront)
{
    bool isBehindFirstYield = standsBehindFirstYield(block);
    if (!isBehindFirstYield && !m_hasPassedFirstYield)
        return { };

    const Block& written = m_code.at(block);
    std::span<const ValueID> values = m_code.valuesOf(block);
    if (yieldInFront)
        values = values.subspan(std::ranges::find(values, yieldInFront) - values.begin() + 1);
    else {
        for (ValueID parameter : written.parameters())
            m_stretchesWhereNamed[parameter.index()] = m_stretch;
    }

    ValueID neverStored;
    auto name = [&](ValueID id) {
        if (!id || !isKeptInHeapFrame(id) || isNamedInThisStretch(std::exchange(m_stretchesWhereNamed[id.index()], m_stretch)))
            return;
        if (!m_valuesStoredInHeapFrame.get(id.index())) {
            neverStored = id;
            return;
        }
        if (!isBehindFirstYield) {
            generator().nameAgain(id, m_keptValuesAsDefined[id.index()]);
            return;
        }
        generator().nameAgain(id, readOutOfHeapFrame(heapFrame(), id));
    };
    if (!yieldInFront)
        name(written.subject);
    for (ValueID id : values) {
        const Operation& operation = operationAt(id);
        if (operation.opcode == Opcode::YieldBySavingFrame) {
            name(operation.generatorOperand());
            name(operation.valueOperand());
            return neverStored;
        }
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            name(operand);
        });
        m_stretchesWhereNamed[id.index()] = m_stretch;
    }
    for (ValueID operand : written.terminator.operands())
        name(operand);
    for (uint32_t index = 0; index < written.terminator.numberOfEdges; ++index) {
        for (ValueID argument : m_code.at(written.terminator.edge(index)).arguments())
            name(argument);
    }
    if (EdgeID handler = m_code.handlerOf(block); handler && (written.terminator.kind == TerminatorKind::Throw || written.terminator.kind == TerminatorKind::Rethrow)) {
        for (ValueID argument : m_code.at(handler).arguments())
            name(argument);
    }
    return neverStored;
}

void SavedFrameEmitter::passHandlerArgumentsInLandingPad(BlockID site, const Edge& handler)
{
    Vector<std::pair<ValueID, LValue>, 8> namedAtSite;
    {
        CodeGenerator::InitializationScope scope(generator());
        bool isBehindFirstYield = standsBehindFirstYield(site);
        LValue frame = nullptr;
        for (ValueID argument : handler.arguments()) {
            if (!isKeptInHeapFrame(argument) || !m_valuesStoredInHeapFrame.get(argument.index()))
                continue;
            if (isBehindFirstYield && !frame)
                frame = readHeapFrameOutOfSlot();
            namedAtSite.append({ argument, generator().wordOf(argument) });
            generator().nameAgain(argument, isBehindFirstYield ? readOutOfHeapFrame(frame, argument) : m_keptValuesAsDefined[argument.index()]);
        }
    }
    generator().passHandlerArguments(handler);
    for (auto [argument, lowered] : namedAtSite | std::views::reverse)
        generator().nameAgain(argument, lowered);
}

LValue SavedFrameEmitter::addressOfEntrypoint(unsigned index)
{
    Machine::PatchOperation* patch = m_out.patchpoint(Machine::pointerType());
    patch->effects = Machine::Effects::none();
    patch->setGenerator([index](CCallHelpers& jit, const Machine::PatchParameters& params) {
        MacroAssembler::Label site = jit.movePCRelativeAddress(params[0].gpr(), PCRelativeReach::Far);
        jit.addLinkTask([site, procedure = &params.proc(), index](LinkBuffer& linkBuffer) {
            RELEASE_ASSERT(linkBuffer.link(site, addressKindFor(PCRelativeReach::Far), procedure->entrypointLabel(index)));
        });
    });
    return patch;
}

void SavedFrameEmitter::makeHeapFrame(const ValueContext& firstYield, CellPointer generatorCell, LValue resumeAddress)
{
    LValue frame;
    if (m_numberOfWordsInHeapFrame <= SavedFrame::maxWordsAllocatedInStub)
        frame = callStub(firstYield, AOT::newHeapFrame(), { m_out.constInt64(m_numberOfWordsInHeapFrame), resumeAddress }).first;
    else
        frame = callOperation<CellPointer>(firstYield, RuntimeOperation::NewHeapFrame, { word32(static_cast<int32_t>(m_numberOfWordsInHeapFrame)), Pointer(resumeAddress) }).value();

    const Machine::AbstractHeap& fields = m_heaps.entire(AbstractHeap::InternalFields);
    if (m_numberOfTracedWords != m_numberOfWordsInHeapFrame)
        m_out.store32As16(m_out.constInt32(static_cast<int32_t>(m_numberOfTracedWords)), m_out.address(fields, frame, SavedFrame::offsetOfNumberOfWords()));
    auto store = [&](ValueID id) {
        if (id && isKeptInHeapFrame(id) && !m_valuesStoredInHeapFrame.get(id.index()))
            storeIntoHeapFrame(frame, id);
    };
    for (ValueID live : m_code.liveAt(firstYield.id()))
        store(live);
    for (ValueID held : heldOperandsOf(firstYield.value()))
        store(held);
    emitter<BarrierEmitter>().fenceAfterInitialization();
    m_out.storePtr(frame, m_out.address(fields, generatorCell.value(), Generator::offsetOfSavedFrame()));
    emitter<BarrierEmitter>().barrier(firstYield, generatorCell);
}

void SavedFrameEmitter::yieldOutOfHeapFrame(const ValueContext& value)
{
    const StubSignature& signature = inputs().stubCatalog.summaryOf(enterGeneratorFrame()).signature;
    RELEASE_ASSERT(signature.numberOfOperands == 3);
    CellPointer generatorCell = in<CellPointer>(value->generatorOperand());
    Boxed yielded = in<Boxed>(value->valueOperand());
    uint16_t entrypoint = m_numberOfEntrypoints++;
    LValue resumeAddress = addressOfEntrypoint(entrypoint);
    LValue suspendedAtYield = m_out.constInt32(static_cast<int32_t>(Generator::State::SuspendedAtYield));

    if (value.id() == m_firstYield) {
        makeHeapFrame(value, generatorCell, resumeAddress);
        LValue state = m_out.load32(stateOf(generatorCell));
        m_out.store32(m_out.select(m_out.equal(state, m_out.constInt32(static_cast<int32_t>(Generator::State::Running))), suspendedAtYield, state), stateOf(generatorCell));
        m_hasPassedFirstYield = true;
    } else {
        m_out.storePtr(resumeAddress, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), heapFrame(), SavedFrame::offsetOfResumeAddress()));
        m_out.store32(suspendedAtYield, stateOf(generatorCell));
    }
    Operand returned = yielded;
    emitter<CallEmitter>().returnValues(singleElementSpan(returned));

    m_out.appendTo(m_proc.addEntrypoint(entrypoint));
    LValue frame = m_out.argumentRegister(signature.operands[0]);
    LValue mode = m_out.argumentRegister(signature.operands[1]);
    LValue sent = m_out.argumentRegister(signature.operands[2]);
    m_out.storePtr(frame, m_out.address(m_heaps.frameSlots, m_out.slotBase(m_slotOfHeapFrame)));
    defineValueAndWord(value, { sent, mode });
    beginStretchOfHeapFrame(value.block(), frame);
    if (readBackOutOfHeapFrame(value.block(), value.id()))
        fail(value, InternalErrorKind::InvalidSuspension);
}

CellPointer SavedFrameEmitter::allocateGenerator(std::optional<CellPointer> closure, LBasicBlock slowPath)
{
    std::optional<Word32> structureID;
    std::optional<Pointer> structure;
    if (closure) {
        constexpr ptrdiff_t offsetOfStructureID = FunctionRareData::offsetOfInternalFunctionAllocationProfile() + InternalFunctionAllocationProfile::offsetOfStructureID();
        Pointer taggedRareData = loadField<Pointer>(*closure, m_heaps.JSFunction_executableOrRareData);
        branchUnless(Condition(m_out.testNonZeroPtr(taggedRareData.value(), m_out.constIntPtr(JSFunction::rareDataTag))), slowPath);
        structureID = Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), taggedRareData.value(), offsetOfStructureID - static_cast<ptrdiff_t>(JSFunction::rareDataTag))));
        branchUnless(isNotZero(*structureID), slowPath);

        structure = decodeStructureID(*structureID);
        Pointer classInfos = loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.instanceClassInfos));
        Pointer classInfo(m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, classInfos.value(), static_cast<ptrdiff_t>(BuiltinClasses::Generator.index()) * sizeof(const ClassInfo*))));
        branchUnless(equal(loadField<Pointer>(*structure, m_heaps.Structure_classInfo), classInfo), slowPath);
        Pointer realm = loadField<Pointer>(*structure, m_heaps.Structure_realm);
        branchUnless(equal(realm, loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject))), slowPath);
    }

    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    Pointer allocator(m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(coroutines.generatorAllocator))));
    branchUnless(Condition(m_out.notNull(allocator.value())), slowPath);
    CellPointer result = cells.allocateHeapCell(allocator, sizeof(Generator), slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        if (closure) {
            m_out.store32(structureID->value(), result.value(), m_heaps.JSCell_structureID);
            m_out.store32(m_out.load32(structure->value(), m_heaps.Structure_indexingModeIncludingHistory), result.value(), m_heaps.JSCell_usefulBytes);
        } else
            cells.storeHeader(result, loadImmutable64(AOT_INSTANCE_OFFSET(coroutines.plainGeneratorHeader)));
        m_out.storePtr(m_out.intPtrZero, result.value(), m_heaps.JSNonArray_butterfly);
        m_out.store64(m_out.int64Zero, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), result.value(), Generator::offsetOfSavedFrame()));
        m_out.store32(m_out.int32Zero, stateOf(result));
        m_out.store32(m_out.int32Zero, m_out.address(m_heaps.entire(AbstractHeap::PrototypeLinks), result.value(), Generator::offsetOfHasOtherPrototype()));
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return result;
}

void SavedFrameEmitter::generateNewGenerator(const ValueContext& value)
{
    std::optional<CellPointer> closure;
    if (value->calleeOperand())
        closure = in<CellPointer>(value->calleeOperand());

    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateGenerator(closure, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewGenerator, { closure ? boxCell(*closure) : Boxed(m_out.int64Zero), word32(static_cast<int32_t>(NewGeneratorKind::Sync)) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void SavedFrameEmitter::generateNewAsyncGenerator(const ValueContext& value)
{
    define(value, CellPointer(callStub(value, AOT::newAsyncGenerator(), { in<CellPointer>(value->calleeOperand()).value() }).first));
}

void SavedFrameEmitter::generateTakeGeneratorPrototype(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::TakeGeneratorPrototype, { in<CellPointer>(value->generatorOperand()), boxCell(in<CellPointer>(value->calleeOperand())) });
}

TypedPointer SavedFrameEmitter::stateOf(CellPointer generator)
{
    return m_out.address(m_heaps.entire(AbstractHeap::InternalFields), generator.value(), Generator::offsetOfState());
}

void SavedFrameEmitter::generateReturnAsyncResult(const ValueContext& value)
{
    Boxed promise = in<Boxed>(value->promiseOperand());
    Boxed result = in<Boxed>(value->valueOperand());
    define(value, Boxed(callStub(value, returnAsyncResult(), { promise.value(), result.value() }, { .callerHasLeft = true }).first));
}

void SavedFrameEmitter::generateStepGenerator(const ValueContext& value)
{
    Boxed generator = in<Boxed>(value->generatorOperand());
    Boxed sentValue = in<Boxed>(value->valueOperand());
    LValue mode = m_out.constInt64(static_cast<int64_t>(value->resumeMode()));
    defineValueAndWord(value, callStub(value, stepGenerator(value->check()), { generator.value(), mode, sentValue.value() }));
}

static bool everyCellIsOfCoroutineClass(TypeReader types, const Type& type, BuiltinClassID builtinClass)
{
    if (!type.hasOnlyClassObjects())
        return false;
    const ClassPart* classPart = singleBuiltinClassOf(types, type);
    return classPart && classPart->builtinClass == builtinClass;
}

void SavedFrameEmitter::branchUnlessUnmodifiedGenerator(Operand operand, const Type& type, LBasicBlock otherwise)
{
    if (!type.hasObjects()) {
        m_out.jump(otherwise);
        m_out.appendTo(newBlock());
        return;
    }

    CellPointer cell;
    switch (operand.representation) {
    case Representation::Cell:
        cell = operand.as<CellPointer>();
        break;
    case Representation::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        if (type.primitives & Primitive::Absent)
            branchUnless(isNotEmpty(boxed), otherwise);
        if (!type.isObject())
            branchUnless(isCell(boxed), otherwise);
        cell = unboxCell(boxed);
        break;
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (!everyCellIsOfCoroutineClass(types(), type, BuiltinClasses::Generator))
        branchUnless(emitter<TypeTestEmitter>().inheritsClassInfo(cell, BuiltinClasses::Generator), otherwise);

    Word32 hasOtherPrototype(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::PrototypeLinks), cell.value(), Generator::offsetOfHasOtherPrototype())));
    branchUnless(isZero(hasOtherPrototype), otherwise);
    branchUnless(Condition(m_out.isNull(m_out.loadPtr(cell.value(), m_heaps.JSObject_butterfly))), otherwise);
    CellPointer prototype(m_out.load64(loadStructure(cell).value(), m_heaps.Structure_prototype));
    Word32 structureID = loadStructureID(prototype);
    Condition hangsBelowEmptyPrototype = equal(structureID, loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.emptyGeneratorPrototypeStructureID)));
    Condition isAsAllPlainOnesAreMade = equal(loadStructureID(cell), loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.plainGeneratorHeader)));
    branchUnless(Condition(m_out.bitOr(hangsBelowEmptyPrototype.value(), isAsAllPlainOnesAreMade.value())), otherwise);
}

void SavedFrameEmitter::generateIsUnmodifiedGenerator(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchUnlessUnmodifiedGenerator(get(operand), typeOf(operand), targets.notTaken.block());
        m_out.jump(targets.taken);
    });
}

static std::optional<StepConsumer> stepConsumerOf(CoroutineKind kind, bool awaitsCall)
{
    switch (kind) {
    case CoroutineKind::AsyncFunction:
        return awaitsCall ? StepConsumer::AsyncFunctionAwaitingCall : StepConsumer::AsyncFunction;
    case CoroutineKind::AsyncGenerator:
        return awaitsCall ? StepConsumer::AsyncGeneratorAwaitingCall : StepConsumer::AsyncGenerator;
    case CoroutineKind::Generator:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateStepAsyncGenerator(const ValueContext& value)
{
    ValueID driver = value->driverOperand();
    std::optional<StepConsumer> consumer = stepConsumerOf(kindOfFunction(), !!value->calledBuiltin());
    if (!consumer || opcodeOf(driver) == Opcode::Absent) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    defineValueAndWord(value, suspend(value, stepAsyncGenerator(*consumer), { in<CellPointer>(value->generatorOperand()).value(), cellOrZero(driver), in<Boxed>(value->valueOperand()).value() }, driver));
}

void SavedFrameEmitter::generatePromiseAllOfElements(const ValueContext& value)
{
    Vector<Boxed, 8> elements;
    for (ValueID element : value->elementsOperands())
        elements.append(in<Boxed>(element));
    Pointer buffer = argumentBuffer(elements.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::PromiseAll, { word32(static_cast<int32_t>(elements.size())), buffer, word32(static_cast<int32_t>(value->directCalls())) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
