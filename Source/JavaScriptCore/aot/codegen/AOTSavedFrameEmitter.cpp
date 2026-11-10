/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBarrierEmitter.h"
#include "AOTBodyIndex.h"
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
#include "AOTSort.h"
#include "AOTStubCatalog.h"
#include "AOTTypeTestEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "FunctionRareData.h"
#include "JSAsyncGenerator.h"
#include "JSFunction.h"
#include "MarkedSpace.h"
#include <algorithm>
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
    if (m_keepsWordsInMachineFrame)
        clearWordsOfWhatIsDeadAcross(value);
    bool frameLiesOnHeap = generator().keepsValuesInHeapFrame() && !m_keepsWordsInMachineFrame;
    uint16_t entrypoint = 0;
    if (frameLiesOnHeap) {
        RELEASE_ASSERT(hasMode);
        entrypoint = m_numberOfEntrypoints++;
        m_out.storePtr(addressOfEntrypoint(entrypoint), m_out.address(m_heaps.entire(AbstractHeap::InternalFields), heapFrame(), SavedFrame::offsetOfResumeAddress()));
        clearWordsOfWhatIsDeadAcross(value);
    }

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
    if (signature.filledOutOfResultInFront)
        patch->setRegisterThatCalleeFillsOutOfResult(signature.filledOutOfResultInFront);
    unsigned driverChild = m_proc.resultCount(type) + patch->numChildren();
    if (!frameLiesOnHeap) {
        patch->setOperandEncodedAsFrameWordIndex(patch->numChildren());
        patch->append(driver, Machine::ValueRep::LateColdAny);
    }
    FrameMapEmitter::Roots roots { m_proc.resultCount(type) + patch->numChildren(), { } };
    if (!frameLiesOnHeap)
        roots = emitter<FrameMapEmitter>().appendRoots(patch, value);
    std::span<const FrameSlotKind> kindsOfRoots = m_proc.arena().copySpan<FrameSlotKind>(roots.kinds.span());
    patch->resultConstraints = { Machine::ValueRep::reg(signature.firstResult) };
    if (hasMode)
        patch->resultConstraints.append(Machine::ValueRep::reg(signature.secondResult));

    Origin originOfSuspension = value.origin();
    originOfSuspension.isTailCall = false;
    uint32_t site = calls.addSite(value, { .origin = originOfSuspension, .callerHasLeft = runsCodeOfAwaited == RunsCodeOfAwaited::BehindFunction });
    patch->setGenerator([records = &m_records, build = &build(), subject = subjectOf(value), operation = nameOf(value.opcode()), key, site, firstRoot = roots.firstChild, slotOfKeptWords = roots.slotOfKeptWords, tracedWordsInSlot = roots.numberOfTracedWordsInSlot, kindsOfRoots, driverChild, frameLiesOnHeap, siteWordGPR = siteWordRegister.gpr()](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FrameMapEmitter::Roots rootsOfSite { firstRoot, Vector<FrameSlotKind, 8>(kindsOfRoots), slotOfKeptWords, tracedWordsInSlot };
        std::optional<uint64_t> word = frameLiesOnHeap ? FrameMapEmitter::siteWordOfFrameOnHeap(params) : FrameMapEmitter::siteWordFor(records->frameMaps, params, rootsOfSite, driverChild);
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
        if (params.value()->leavesMoveOutOfResultToCallee())
            records->stubCalls.enterNextStubInFront(params.value()->registerThatCalleeFillsOutOfResult(), params.value()->registerThatCalleeMovesIntoResult());
        records->stubCalls.call(jit, key, OptionalIndex { site });
    });
    calls.finishCallPatchpoint(value, patch, site, request);
    StubCallResult result = hasMode ? StubCallResult { m_out.extract(patch, 0), m_out.extract(patch, 1) } : StubCallResult { patch, nullptr };
    if (m_keepsWordsInMachineFrame) {
        m_stretch = ++m_numberOfStretches;
        m_stretchInFront = 0;
        m_lastStretchesOfBlocks[value.block().index()] = m_stretch;
        m_stretchesWhereNamed[value.id().index()] = m_stretch;
        if (readBackOutOfHeapFrame(value.block(), value.id()))
            fail(value, InternalErrorKind::InvalidSuspension);
    }
    if (frameLiesOnHeap) {
        const StubSignature& entry = catalog.summaryOf(enterGeneratorFrame()).signature;
        RELEASE_ASSERT(entry.numberOfOperands == 3);
        LBasicBlock throws = newBlock();
        LBasicBlock takes = newBlock();
        LBasicBlock goesOn = newBlock();
        ValueFromBlock valueInPlace = m_out.anchor(result.first);
        ValueFromBlock wordInPlace = m_out.anchor(result.second);
        m_out.jump(goesOn);

        m_out.appendTo(m_proc.addEntrypoint(entrypoint));
        LValue frame = m_out.argumentRegister(entry.operands[0]);
        LValue word = m_out.argumentRegister(entry.operands[1]);
        LValue sent = m_out.argumentRegister(entry.operands[2]);
        m_out.storePtr(frame, m_out.address(m_heaps.frameSlots, m_out.slotBase(m_slotOfHeapFrame)));
        m_out.branch(m_out.testNonZero64(word, m_out.constInt64(1 << SavedFrame::throwsIntoAwaitShift)), unlikelyEdge(throws), normalEdge(takes));

        m_out.appendTo(throws);
        callVoidOperation(value, RuntimeOperation::Throw, { Boxed(sent) });
        m_out.unreachable();

        m_out.appendTo(takes);
        ValueFromBlock valueSent = m_out.anchor(sent);
        ValueFromBlock wordSent = m_out.anchor(word);
        m_out.jump(goesOn);

        m_out.appendTo(goesOn);
        result = { m_out.phi(Machine::Int64, valueInPlace, valueSent), m_out.phi(Machine::Int64, wordInPlace, wordSent) };
        beginStretchOfHeapFrame(value.block(), readHeapFrameOutOfSlot());
        m_stretchesWhereNamed[value.id().index()] = m_stretch;
        if (readBackOutOfHeapFrame(value.block(), value.id()))
            fail(value, InternalErrorKind::InvalidSuspension);
    }
    return result;
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
        if (value.id() == m_firstYield && generator().keepsValuesInHeapFrame() && !m_keepsWordsInMachineFrame)
            yieldOutOfHeapFrame(value);
        else
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
    if (made.opcode != Opcode::NewHeapWords || !m_code.has(Mechanism::CellsInFrames, m_facts))
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
    static constexpr OpcodeSet suspensionsOfAsyncGenerators { Opcode::YieldBySavingFrame, Opcode::AsyncGeneratorYield, Opcode::AwaitBySavingFrame, Opcode::StepAsyncGenerator };
    OpcodeSet suspensionsThatLeaveHeapFrame = kindOfFunction() == CoroutineKind::AsyncGenerator ? suspensionsOfAsyncGenerators : OpcodeSet { Opcode::YieldBySavingFrame };
    return std::ranges::all_of(m_code.suspensions(), [&](const LiveAtSuspension& record) {
        BlockID block = m_code.blockOf(record.suspension);
        return suspensionsThatLeaveHeapFrame.contains(opcodeOf(record.suspension)) && (block == m_blockOfFirstYield || m_blocksBehindFirstYield.get(block.index()));
    });
}

bool SavedFrameEmitter::chooseValuesKeptInHeapFrame(Vector<ValueID, 16>& kept)
{
    Vector<uint32_t> lastSuspensionsCrossed;
    BitVector crossesSeveral;
    if (m_keepsWordsInMachineFrame) {
        lastSuspensionsCrossed.fill(0, m_code.numberOfValues());
        uint32_t number = 0;
        auto note = [&](ValueID id) {
            if (!id)
                return;
            uint32_t last = std::exchange(lastSuspensionsCrossed[id.index()], number);
            if (last && last != number)
                crossesSeveral.set(id.index());
        };
        for (const LiveAtSuspension& record : m_code.suspensions()) {
            ++number;
            for (ValueID live : m_code.liveAt(record.suspension))
                note(live);
            for (ValueID held : heldOperandsOf(operationAt(record.suspension)))
                note(held);
        }
    }
    auto choose = [&](ValueID id, ValueID suspension, bool isRead) {
        if (!id || m_wordsOfKeptValues[id.index()] || id == suspension || m_code.isConstant(id) || !m_code.isNeeded(id))
            return true;
        if (m_keepsWordsInMachineFrame && !crossesSeveral.get(id.index()))
            return true;
        const Operation& operation = operationAt(id);
        if (isDeferredClosure(id) || typeOf(id).isOpaque(OpaqueKind::StringBuilder) || generator().waitsForBoxOfSum(id, operation))
            return m_keepsWordsInMachineFrame;
        bool hasSeveralResults = operation.opcode == Opcode::CallDirect ? conventions().numberOfResults(conventions().unitEnteredBy(operation)) > 1 : numberOfResultsOf(m_program, operation).has_value();
        return (!hasSeveralResults && choosesWordFor(formKeptInHeapFrame(id), isRead, id, kept)) || m_keepsWordsInMachineFrame;
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
    if (!m_facts.mechanisms().contains(Mechanism::TypedOperations))
        return false;
    if (kindOfFunction() != CoroutineKind::AsyncFunction) {
        if (conventions().entryConventionOf(m_unit) != EntryConvention::Direct || m_code.suspensions().size() >= fusedCallEntry)
            return false;
        if (!findBlocksBehindFirstYield())
            return false;
    } else {
        uint64_t lengthOfLists = 0;
        for (const LiveAtSuspension& record : m_code.suspensions())
            lengthOfLists += m_code.liveAt(record.suspension).size();
        if (lengthOfLists <= m_code.numberOfValues())
            return false;
        m_keepsWordsInMachineFrame = true;
        m_hasPassedFirstYield = true;
        for (BlockID block : generator().order())
            m_blocksBehindFirstYield.set(block.index());
    }

    Machine::Arena& arena = m_proc.arena();
    m_wordsOfKeptValues = arena.createSpan<uint16_t>(m_code.numberOfValues());
    Vector<ValueID, 16> kept;
    if (!chooseValuesKeptInHeapFrame(kept) || (m_keepsWordsInMachineFrame && kept.isEmpty())) {
        m_keepsWordsInMachineFrame = false;
        return false;
    }

    m_stretchesWhereNamed = arena.createSpan<uint32_t>(m_code.numberOfValues());
    m_keptValuesAsDefined = arena.createSpan<LValue>(m_code.numberOfValues());
    m_lastStretchesOfBlocks = arena.createSpan<uint32_t>(m_code.numberOfBlocks());
    m_wordsThatMayBeSetAtStartsOfBlocks.grow(m_code.numberOfBlocks());
    std::span<const BlockID> order = generator().order();
    m_tracedWordsDefinedInFrontOfPlaces = arena.createSpan<uint32_t>(order.size() + 1);
    layBlocksOfEachLoopTogether();
    auto placeOfDefinition = [&](ValueID id) {
        return m_placesOfBlocks[m_code.blockOf(id).index()];
    };
    AOT::stableSort(kept, { }, placeOfDefinition);
    uint32_t nextTraced = 0;
    uint32_t nextRaw = m_numberOfTracedWords;
    for (ValueID id : kept) {
        bool isTraced = !!frameSlotKindOf(formKeptInHeapFrame(id));
        m_wordsOfKeptValues[id.index()] = static_cast<uint16_t>((isTraced ? nextTraced : nextRaw)++ + 1);
        m_tracedWordsDefinedInFrontOfPlaces[placeOfDefinition(id) + 1] += isTraced;
        if (opcodeOf(id) == Opcode::Parameter)
            m_keptValuesAsDefined[id.index()] = generator().wordOf(id);
    }
    for (uint32_t place = 0; place < order.size(); ++place)
        m_tracedWordsDefinedInFrontOfPlaces[place + 1] += m_tracedWordsDefinedInFrontOfPlaces[place];
    findWordsNeededAtStartsOfBlocks();
    m_numberOfWordsInHeapFrame = std::max(nextRaw, 1u);
    if (!m_keepsWordsInMachineFrame) {
        m_slotOfHeapFrame = m_proc.addStackSlot(sizeof(uint64_t));
        if (m_everyWayBackLeadsToWhatDominates) {
            m_suspensionsInFrontOfPlaces = arena.createSpan<uint32_t>(order.size() + 1);
            for (const LiveAtSuspension& record : m_code.suspensions())
                ++m_suspensionsInFrontOfPlaces[m_placesOfBlocks[m_code.blockOf(record.suspension).index()] + 1];
            for (uint32_t place = 0; place < order.size(); ++place)
                m_suspensionsInFrontOfPlaces[place + 1] += m_suspensionsInFrontOfPlaces[place];
        }
        return true;
    }
    giveWordsWhoseLivesAreApartOnePlace(static_cast<uint32_t>(kept.size()));
    m_slotOfKeptWords = m_proc.addStackSlot(m_numberOfWordsInHeapFrame * sizeof(uint64_t));
    findLoopsInsideStretches();
    return true;
}

void SavedFrameEmitter::giveWordsWhoseLivesAreApartOnePlace(uint32_t numberOfWords)
{
    static constexpr uint32_t none = UINT32_MAX;
    m_numberOfTracedPlaces = m_numberOfTracedWords;
    if (!m_everyWayBackLeadsToWhatDominates)
        return;

    struct Life {
        uint64_t begin { 0 };
        uint64_t end { 0 };
    };
    std::span<const BlockID> order = generator().order();
    Vector<Life> lives;
    lives.fill(Life(), numberOfWords);
    for (uint32_t turn = static_cast<uint32_t>(order.size()); turn--;) {
        BlockID block = order[turn];
        uint64_t startOfBlock = static_cast<uint64_t>(m_placesOfBlocks[block.index()]) << 32;
        auto noteUse = [&](ValueID id, uint32_t inBlock) {
            if (!id || !isKeptInHeapFrame(id))
                return;
            Life& life = lives[m_wordsOfKeptValues[id.index()] - 1];
            uint64_t end = startOfBlock | inBlock;
            if (end <= life.end)
                return;
            uint32_t placeOfDefinition = m_placesOfBlocks[m_code.blockOf(id).index()];
            for (uint32_t head = m_turnsOfHeads.quickGet(turn) ? turn : m_headsAroundTurns[turn]; head != none; head = m_headsAroundTurns[head]) {
                uint32_t behind = m_placesBehindLoops[order[head].index()];
                if (placeOfDefinition >= m_placesOfBlocks[order[head].index()] && placeOfDefinition < behind)
                    break;
                end = (static_cast<uint64_t>(behind) << 32) - 1;
            }
            life.end = end;
        };
        auto noteDefinition = [&](ValueID id, uint32_t inBlock) {
            if (!isKeptInHeapFrame(id))
                return;
            Life& life = lives[m_wordsOfKeptValues[id.index()] - 1];
            life.begin = startOfBlock | inBlock;
            life.end = std::max(life.end, life.begin);
        };
        const Block& written = m_code.at(block);
        for (ValueID operand : written.terminator.operands())
            noteUse(operand, none);
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments())
                noteUse(argument, none);
        });
        std::span<const ValueID> values = m_code.valuesOf(block);
        for (uint32_t inBlock = static_cast<uint32_t>(values.size()); inBlock; --inBlock) {
            ValueID id = values[inBlock - 1];
            const Operation& operation = operationAt(id);
            noteDefinition(id, inBlock);
            if (operation.traits().maySuspend()) {
                for (ValueID live : m_code.liveAt(id))
                    noteUse(live, inBlock);
                for (ValueID held : heldOperandsOf(operation))
                    noteUse(held, inBlock);
            }
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                noteUse(operand, inBlock);
            });
        }
        for (ValueID parameter : written.parameters())
            noteDefinition(parameter, 0);
        noteUse(written.subject, 0);
    }

    m_placesOfWords = m_proc.arena().createSpan<uint16_t>(numberOfWords);
    auto givePlaces = [&](uint32_t firstWord, uint32_t endOfWords, uint32_t firstPlace) {
        auto endsLater = [](const auto& a, const auto& b) { return a > b; };
        Vector<uint32_t, 64> byBegin;
        for (uint32_t word = firstWord; word < endOfWords; ++word)
            byBegin.append(word);
        AOT::stableSort(byBegin, { }, [&](uint32_t word) { return lives[word].begin; });
        Vector<std::pair<uint64_t, uint32_t>, 64> endsOfPlaces;
        uint32_t numberOfPlaces = firstPlace;
        for (uint32_t word : byBegin) {
            uint32_t place = numberOfPlaces;
            if (!endsOfPlaces.isEmpty() && endsOfPlaces.first().first < lives[word].begin) {
                std::ranges::pop_heap(endsOfPlaces, endsLater);
                place = endsOfPlaces.takeLast().second;
            } else
                ++numberOfPlaces;
            m_placesOfWords[word] = static_cast<uint16_t>(place);
            endsOfPlaces.append({ lives[word].end, place });
            std::ranges::push_heap(endsOfPlaces, endsLater);
        }
        return numberOfPlaces;
    };
    uint32_t numberOfTracedPlaces = givePlaces(0, m_numberOfTracedWords, 0);
    uint32_t numberOfPlaces = givePlaces(m_numberOfTracedWords, numberOfWords, numberOfTracedPlaces);
    if (numberOfPlaces == numberOfWords) {
        m_placesOfWords = { };
        return;
    }
    m_numberOfTracedPlaces = numberOfTracedPlaces;
    m_numberOfWordsInHeapFrame = std::max(numberOfPlaces, 1u);
    m_numbersOfNeededWordsAtPlaces = m_proc.arena().createSpan<uint16_t>(numberOfTracedPlaces);
}

void SavedFrameEmitter::findLoopsInsideStretches()
{
    if (!m_everyWayBackLeadsToWhatDominates)
        return;

    std::span<const BlockID> order = generator().order();
    m_orderOfBlocks = order;
    uint32_t numberOfBlocks = static_cast<uint32_t>(order.size());
    Vector<uint32_t, 64> turns;
    Vector<uint32_t, 64> turnsAtPlaces;
    Vector<uint32_t, 64> waysInFromFront;
    turns.fill(0, m_code.numberOfBlocks());
    waysInFromFront.fill(0, m_code.numberOfBlocks());
    turnsAtPlaces.fill(0, numberOfBlocks);
    for (uint32_t turn = 0; turn < numberOfBlocks; ++turn) {
        turns[order[turn].index()] = turn;
        turnsAtPlaces[m_placesOfBlocks[order[turn].index()]] = turn;
    }
    auto liesInLoopOf = [&](BlockID block, BlockID head) {
        uint32_t place = m_placesOfBlocks[block.index()];
        return place >= m_placesOfBlocks[head.index()] && place < m_placesBehindLoops[head.index()];
    };
    for (BlockID from : order) {
        m_code.forEachEdgeOf(from, [&](EdgeID, const Edge& edge) {
            if (m_code.isReached(edge.to) && m_placesBehindLoops[edge.to.index()] && !liesInLoopOf(from, edge.to))
                ++waysInFromFront[edge.to.index()];
        });
    }

    m_frontsOfLoopsInsideStretches = m_proc.arena().createSpan<uint32_t>(m_code.numberOfBlocks());
    m_headsOfLoopsInsideStretchesBehind = m_proc.arena().createSpan<uint32_t>(m_code.numberOfBlocks());
    uint32_t placeBehindLastLoopFound = 0;
    for (uint32_t place = 0; place < numberOfBlocks; ++place) {
        uint32_t turnOfHead = turnsAtPlaces[place];
        BlockID head = order[turnOfHead];
        uint32_t behind = m_placesBehindLoops[head.index()];
        if (!behind || place < placeBehindLastLoopFound || !turnOfHead || waysInFromFront[head.index()] != 1)
            continue;
        BlockID front = order[turnOfHead - 1];
        if (liesInLoopOf(front, head) || m_frontsOfLoopsInsideStretches[front.index()] || m_headsOfLoopsInsideStretchesBehind[front.index()])
            continue;
        bool frontLeadsToHead = false;
        m_code.forEachEdgeOf(front, [&](EdgeID, const Edge& edge) {
            frontLeadsToHead |= edge.to == head;
        });
        bool liesInsideStretch = frontLeadsToHead;
        for (uint32_t inside = place; inside < behind && liesInsideStretch; ++inside) {
            BlockID block = order[turnsAtPlaces[inside]];
            const Block& written = m_code.at(block);
            liesInsideStretch = !written.isHandlerBlock() && !written.isEntry;
            for (ValueID parameter : written.parameters())
                liesInsideStretch &= !isKeptInHeapFrame(parameter);
            for (ValueID id : m_code.valuesOf(block))
                liesInsideStretch &= !isKeptInHeapFrame(id) && !operationAt(id).traits().maySuspend();
        }
        if (!liesInsideStretch)
            continue;
        placeBehindLastLoopFound = behind;
        m_headsOfLoopsInsideStretchesBehind[front.index()] = turnOfHead + 1;
        for (uint32_t turn = turnOfHead; turn < numberOfBlocks && liesInLoopOf(order[turn], head); ++turn)
            m_frontsOfLoopsInsideStretches[order[turn].index()] = turnOfHead;
    }
}

bool SavedFrameEmitter::goesOnInStretchAroundLoop(BlockID block)
{
    if (!liesInLoopInsideStretch(block))
        return false;
    m_stretch = m_lastStretchesOfBlocks[generator().order()[m_frontsOfLoopsInsideStretches[block.index()] - 1].index()];
    m_stretchInFront = 0;
    m_lastStretchesOfBlocks[block.index()] = m_stretch;
    m_wordsToClearAtStart.shrink(0);
    return true;
}

void SavedFrameEmitter::layBlocksOfEachLoopTogether()
{
    static constexpr uint32_t none = UINT32_MAX;
    std::span<const BlockID> order = generator().order();
    uint32_t numberOfBlocks = static_cast<uint32_t>(order.size());
    m_placesOfBlocks = m_proc.arena().createSpan<uint32_t>(m_code.numberOfBlocks());
    m_placesBehindLoops = m_proc.arena().createSpan<uint32_t>(m_code.numberOfBlocks());
    for (uint32_t turn = 0; turn < numberOfBlocks; ++turn)
        m_placesOfBlocks[order[turn].index()] = turn;

    m_headsAroundTurns = m_proc.arena().createSpan<uint32_t>(numberOfBlocks);
    std::span<uint32_t> headsAround = m_headsAroundTurns;
    std::ranges::fill(headsAround, none);
    Vector<uint32_t, 64> sizes;
    sizes.fill(1, numberOfBlocks);
    BitVector& heads = m_turnsOfHeads;
    heads.ensureSize(numberOfBlocks);
    auto layInLoop = [&](uint32_t inner, uint32_t head) {
        while (inner != head) {
            if (head > inner) {
                m_everyWayBackLeadsToWhatDominates = false;
                return;
            }
            uint32_t around = headsAround[inner];
            if (around == head)
                return;
            if (around == none || around < head) {
                headsAround[inner] = head;
                if (around == none)
                    return;
                inner = std::exchange(head, around);
            } else
                inner = around;
        }
    };
    for (uint32_t turn = numberOfBlocks; turn--;) {
        m_code.forEachEdgeOf(order[turn], [&](EdgeID, const Edge& edge) {
            if (!m_code.isReached(edge.to))
                return;
            uint32_t turnOfTarget = m_placesOfBlocks[edge.to.index()];
            if (turnOfTarget <= turn) {
                heads.quickSet(turnOfTarget);
                m_everyWayBackLeadsToWhatDominates &= generator().blockDominates(edge.to, order[turn]);
                layInLoop(turn, turnOfTarget);
            } else if (headsAround[turnOfTarget] != none)
                layInLoop(turn, headsAround[turnOfTarget]);
        });
    }
    if (!m_everyWayBackLeadsToWhatDominates) {
        for (size_t turn : heads)
            m_placesBehindLoops[order[turn].index()] = numberOfBlocks;
        return;
    }

    for (uint32_t turn = numberOfBlocks; turn--;) {
        if (headsAround[turn] != none)
            sizes[headsAround[turn]] += sizes[turn];
    }
    Vector<uint32_t, 64> nextPlacesInLoops;
    nextPlacesInLoops.fill(0, numberOfBlocks);
    uint32_t nextPlaceOutsideLoops = 0;
    for (uint32_t turn = 0; turn < numberOfBlocks; ++turn) {
        uint32_t& next = headsAround[turn] == none ? nextPlaceOutsideLoops : nextPlacesInLoops[headsAround[turn]];
        uint32_t place = std::exchange(next, next + sizes[turn]);
        nextPlacesInLoops[turn] = place + 1;
        m_placesOfBlocks[order[turn].index()] = place;
        if (heads.quickGet(turn))
            m_placesBehindLoops[order[turn].index()] = place + sizes[turn];
    }
}

LValue SavedFrameEmitter::readHeapFrameOutOfSlot()
{
    if (m_keepsWordsInMachineFrame)
        return nullptr;
    return m_out.loadPtr(m_out.address(m_heaps.frameSlots, m_out.slotBase(m_slotOfHeapFrame)));
}

LValue SavedFrameEmitter::heapFrame()
{
    if (m_keepsWordsInMachineFrame)
        return nullptr;
    if (!heapFrameIsNamed()) {
        m_heapFrame = readHeapFrameOutOfSlot();
        m_stretchOfHeapFrame = m_stretch;
        m_heapFrameHoldsBelowItsStretch = m_standsAtStartOfStretch || !m_code.handlerOf(m_blocksOfStretches[m_stretch]);
    }
    return m_heapFrame;
}

TypedPointer SavedFrameEmitter::wordOfHeapFrame(LValue frame, ValueID kept)
{
    if (m_slotOfKeptWords)
        return wordInFrame(m_slotOfKeptWords, placeOfWord(m_wordsOfKeptValues[kept.index()] - 1));
    return m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame, offsetOfWordInHeapFrame(m_wordsOfKeptValues[kept.index()] - 1));
}

void SavedFrameEmitter::storeIntoHeapFrame(LValue frame, ValueID kept)
{
    LValue stored = generator().wordOf(kept);
    RELEASE_ASSERT(stored);
    if (m_cellCageBase && isTracedInHeapFrame(kept) && typeOf(kept).mayBeUnspecified())
        stored = m_out.select(m_out.equal(stored, m_cellCageBase), m_out.int64Zero, stored);
    m_out.store(stored, wordOfHeapFrame(frame, kept));
    m_valuesStoredInHeapFrame.set(kept.index());
    if (isTracedInHeapFrame(kept))
        m_wordsThatMayBeSet.set(m_wordsOfKeptValues[kept.index()] - 1);
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
    if (!m_slotOfKeptWords && isTracedInHeapFrame(id) && typeOf(id).hasCells())
        emitter<BarrierEmitter>().barrier(defined, CellPointer(frame));
}

bool SavedFrameEmitter::isNamedAbove(uint32_t stretchWhereNamed)
{
    if (stretchWhereNamed == m_stretch)
        return true;
    if (stretchWhereNamed >= m_firstStretchesAbove.size() || m_firstStretchesAbove[stretchWhereNamed] != m_firstStretchesAbove[m_stretch])
        return false;
    return generator().blockDominates(m_blocksOfStretches[stretchWhereNamed], m_blocksOfStretches[m_stretch]);
}

uint32_t SavedFrameEmitter::stretchThatGoesOnInto(BlockID block)
{
    if (m_code.at(block).isHandlerBlock() || m_code.at(block).isEntry)
        return 0;
    if (BlockID onlyPredecessor = generator().onlyPredecessorOf(block))
        return m_lastStretchesOfBlocks[onlyPredecessor.index()];
    BlockID above = m_suspensionsInFrontOfPlaces.empty() ? BlockID() : generator().nearestDominatorOf(block);
    if (!above)
        return 0;
    uint32_t placeAbove = m_placesOfBlocks[above.index()];
    uint32_t end = std::max(m_placesOfBlocks[block.index()], m_placesBehindLoops[block.index()]);
    if (m_suspensionsInFrontOfPlaces[end] != m_suspensionsInFrontOfPlaces[placeAbove + (m_code.handlerOf(above) ? 0 : 1)])
        return 0;
    return m_lastStretchesOfBlocks[above.index()];
}

void SavedFrameEmitter::beginStretchOfHeapFrame(BlockID block, LValue frameHandedIn)
{
    m_stretch = ++m_numberOfStretches;
    BlockID onlyPredecessor = frameHandedIn ? BlockID() : generator().onlyPredecessorOf(block);
    m_stretchInFront = onlyPredecessor ? m_lastStretchesOfBlocks[onlyPredecessor.index()] : 0;
    uint32_t above = frameHandedIn ? 0 : stretchThatGoesOnInto(block);
    while (m_firstStretchesAbove.size() <= m_stretch) {
        m_firstStretchesAbove.append(0);
        m_blocksOfStretches.append(BlockID());
    }
    m_firstStretchesAbove[m_stretch] = above && above < m_stretch && m_firstStretchesAbove[above] ? m_firstStretchesAbove[above] : m_stretch;
    m_blocksOfStretches[m_stretch] = block;
    m_lastStretchesOfBlocks[block.index()] = m_stretch;
    m_standsAtStartOfStretch = true;
    if (frameHandedIn) {
        m_heapFrame = frameHandedIn;
        m_stretchOfHeapFrame = m_stretch;
        m_heapFrameHoldsBelowItsStretch = true;
    } else
        followWordsThatMayBeSetInto(block);
}

template<typename Functor>
void SavedFrameEmitter::forEachArgumentReadAlong(const Edge& edge, const Functor& functor) const
{
    std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
    std::span<const ValueID> arguments = edge.arguments();
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (m_generator.wordOf(parameters[index]))
            functor(arguments[index]);
    }
}

template<typename Read, typename Defined>
ValueID SavedFrameEmitter::forEachValueReadInStretch(BlockID block, ValueID yieldInFront, const Read& read, const Defined& defined) const
{
    const Block& written = m_code.at(block);
    std::span<const ValueID> values = m_code.valuesOf(block);
    size_t place = 0;
    if (yieldInFront) {
        place = m_placeBehindSuspensionFoundLast;
        if (!place || place > values.size() || values[place - 1] != yieldInFront)
            place = std::ranges::find(values, yieldInFront) - values.begin() + 1;
    } else
        read(written.subject);
    for (; place < values.size(); ++place) {
        ValueID id = values[place];
        const Operation& operation = operationAt(id);
        if ((m_keepsWordsInMachineFrame || operation.opcode != Opcode::YieldBySavingFrame) && operation.traits().maySuspend()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                read(operand);
            });
            m_placeBehindSuspensionFoundLast = place + 1;
            return id;
        }
        if (operation.opcode == Opcode::YieldBySavingFrame) {
            read(operation.generatorOperand());
            read(operation.valueOperand());
            m_placeBehindSuspensionFoundLast = place + 1;
            return id;
        }
        if (operation.opcode == Opcode::Narrowed)
            read(operation.operand(OperandIndex::Narrowed::value));
        else if (operation.opcode != Opcode::ReleaseEnvironment || m_code.marksOf(id).contains(SweepMark::ReleasesOperand)) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                read(operand);
            });
        }
        defined(id);
    }
    for (ValueID operand : written.terminator.operands())
        read(operand);
    for (uint32_t index = 0; index < written.terminator.numberOfEdges; ++index)
        forEachArgumentReadAlong(m_code.at(written.terminator.edge(index)), read);
    if (EdgeID handler = m_code.handlerOf(block); handler && (written.terminator.kind == TerminatorKind::Throw || written.terminator.kind == TerminatorKind::Rethrow))
        forEachArgumentReadAlong(m_code.at(handler), read);
    if (uint32_t turn = m_headsOfLoopsInsideStretchesBehind.empty() ? 0 : m_headsOfLoopsInsideStretchesBehind[block.index()]) {
        for (--turn; turn < m_orderOfBlocks.size() && m_frontsOfLoopsInsideStretches[m_orderOfBlocks[turn].index()]; ++turn)
            forEachValueReadInStretch(m_orderOfBlocks[turn], ValueID(), read, defined);
    }
    return { };
}

void SavedFrameEmitter::findWordsNeededAtStartsOfBlocks()
{
    static constexpr uint32_t none = UINT32_MAX;
    std::span<const BlockID> order = generator().order();
    uint32_t numberOfBlocks = static_cast<uint32_t>(order.size());
    Machine::Arena& arena = m_proc.arena();
    m_blocksAboveYields.ensureSize(m_code.numberOfBlocks());
    m_changesOfNeedsAtTurns = arena.createSpan<RangeOfWords>(numberOfBlocks);
    m_wordsReadLastAtTurns = arena.createSpan<RangeOfWords>(numberOfBlocks);
    std::span<uint32_t> turns = arena.createSpan<uint32_t>(m_code.numberOfBlocks());
    for (uint32_t turn = 0; turn < numberOfBlocks; ++turn)
        turns[order[turn].index()] = turn;

    BitVector blocksThatYield(m_code.numberOfBlocks());
    Vector<uint16_t, 64> needs;
    Vector<uint16_t, 16> argumentsOfHandlers;
    Vector<RangeOfWords, 64> needsAtTurns;
    Vector<RangeOfWords, 64> argumentsOfHandlersAtTurns;
    Vector<uint32_t, 64> lastTurnsNoted;
    Vector<uint32_t, 64> waysInFromFront;
    needsAtTurns.grow(numberOfBlocks);
    argumentsOfHandlersAtTurns.grow(numberOfBlocks);
    lastTurnsNoted.fill(none, m_numberOfTracedWords);
    waysInFromFront.fill(0, numberOfBlocks);
    auto forEachTurnLedTo = [&](uint32_t turn, const auto& functor) {
        m_code.forEachEdgeOf(order[turn], [&](EdgeID, const Edge& edge) {
            if (m_code.isReached(edge.to))
                functor(turns[edge.to.index()]);
        });
    };
    for (uint32_t turn = 0; turn < numberOfBlocks; ++turn) {
        BlockID block = order[turn];
        auto wordDefinedElsewhere = [&](ValueID id) -> std::optional<uint16_t> {
            if (!id || !isKeptInHeapFrame(id) || !isTracedInHeapFrame(id) || m_code.blockOf(id) == block)
                return std::nullopt;
            return static_cast<uint16_t>(m_wordsOfKeptValues[id.index()] - 1);
        };
        auto need = [&](ValueID id) {
            if (std::optional<uint16_t> word = wordDefinedElsewhere(id); word && std::exchange(lastTurnsNoted[*word], turn) != turn)
                needs.append(*word);
        };
        needsAtTurns[turn].first = static_cast<uint32_t>(needs.size());
        ValueID yield;
        do {
            yield = forEachValueReadInStretch(block, yield, need, [](ValueID) { });
            if (yield) {
                m_blocksAboveYields.quickSet(block.index());
                blocksThatYield.quickSet(block.index());
                for (ValueID live : m_code.liveAt(yield))
                    need(live);
                for (ValueID held : heldOperandsOf(operationAt(yield)))
                    need(held);
            }
        } while (yield);
        argumentsOfHandlersAtTurns[turn].first = static_cast<uint32_t>(argumentsOfHandlers.size());
        if (EdgeID handler = m_code.handlerOf(block)) {
            forEachArgumentReadAlong(m_code.at(handler), [&](ValueID id) {
                need(id);
                if (std::optional<uint16_t> word = wordDefinedElsewhere(id))
                    argumentsOfHandlers.append(*word);
            });
        }
        needsAtTurns[turn].end = static_cast<uint32_t>(needs.size());
        argumentsOfHandlersAtTurns[turn].end = static_cast<uint32_t>(argumentsOfHandlers.size());
        forEachTurnLedTo(turn, [&](uint32_t target) {
            waysInFromFront[target] += target > turn;
        });
    }
    for (uint32_t turn = numberOfBlocks; turn--;) {
        if (!m_blocksAboveYields.quickGet(order[turn].index()))
            continue;
        if (BlockID above = generator().nearestDominatorOf(order[turn]))
            m_blocksAboveYields.quickSet(above.index());
    }
    if (!m_numberOfTracedWords)
        return;

    size_t lengthOfSet = (m_numberOfTracedWords + 63) / 64;
    Vector<uint64_t, 64> sets;
    Vector<uint32_t, 16> setsThatAreFree;
    Vector<uint32_t, 64> setsAtTurns;
    Vector<uint32_t, 64> waysInLeft;
    auto setAt = [&](uint32_t place) {
        return sets.mutableSpan().subspan(place * lengthOfSet, lengthOfSet);
    };
    auto takeSet = [&] {
        if (!setsThatAreFree.isEmpty())
            return setsThatAreFree.takeLast();
        sets.grow(sets.size() + lengthOfSet);
        return static_cast<uint32_t>(sets.size() / lengthOfSet - 1);
    };
    auto forEachWordIn = [&](std::span<const uint64_t> set, const auto& functor) {
        for (size_t index = 0; index < set.size(); ++index) {
            for (uint64_t bits = set[index]; bits; bits &= bits - 1)
                functor(static_cast<uint16_t>(index * 64 + std::countr_zero(bits)));
        }
    };
    auto add = [&](std::span<uint64_t> set, uint16_t word) {
        set[word / 64] |= 1ull << (word % 64);
    };

    Vector<uint32_t, 64> placesOfHeads;
    placesOfHeads.fill(none, numberOfBlocks);
    uint32_t numberOfHeads = 0;
    for (size_t turn : m_turnsOfHeads)
        placesOfHeads[turn] = numberOfHeads++;
    Vector<uint64_t, 64> setsOfHeads;
    setsOfHeads.fill(0, numberOfHeads * lengthOfSet);
    auto setOfHeadAt = [&](uint32_t turn) {
        return setsOfHeads.mutableSpan().subspan(placesOfHeads[turn] * lengthOfSet, lengthOfSet);
    };
    Vector<uint16_t, 64> changes;
    Vector<uint16_t, 64> readLast;
    uint32_t behind = takeSet();
    uint32_t above = takeSet();
    auto goFromBehind = [&](bool waysBackAreKnown) {
        setsAtTurns.fill(none, numberOfBlocks);
        waysInLeft = waysInFromFront;
        uint32_t turnAbove = none;
        bool someHeadNeedsMore = false;
        for (uint32_t turn = numberOfBlocks; turn--;) {
            BlockID block = order[turn];
            uint32_t place = takeSet();
            std::ranges::fill(setAt(place), 0);
            forEachTurnLedTo(turn, [&](uint32_t target) {
                std::span<uint64_t> needed = setAt(place);
                std::span<const uint64_t> atTarget = target <= turn ? setOfHeadAt(target) : setAt(setsAtTurns[target]);
                for (size_t index = 0; index < lengthOfSet; ++index)
                    needed[index] |= atTarget[index];
                if (target > turn && !--waysInLeft[target])
                    setsThatAreFree.append(std::exchange(setsAtTurns[target], none));
            });
            std::span<uint64_t> needed = setAt(place);
            bool isAsked = waysBackAreKnown && m_blocksBehindFirstYield.get(block.index()) && m_blocksAboveYields.quickGet(block.index());
            if (isAsked) {
                std::ranges::copy(needed, setAt(behind).begin());
                for (uint16_t word : argumentsOfHandlers.span().subspan(argumentsOfHandlersAtTurns[turn].first, argumentsOfHandlersAtTurns[turn].size()))
                    add(setAt(behind), word);
            }
            for (uint16_t word : needs.span().subspan(needsAtTurns[turn].first, needsAtTurns[turn].size()))
                add(needed, word);
            uint32_t placeOfBlock = m_placesOfBlocks[block.index()];
            for (uint32_t word = m_tracedWordsDefinedInFrontOfPlaces[placeOfBlock]; word < m_tracedWordsDefinedInFrontOfPlaces[placeOfBlock + 1]; ++word)
                needed[word / 64] &= ~(1ull << (word % 64));

            if (!waysBackAreKnown && m_turnsOfHeads.quickGet(turn)) {
                someHeadNeedsMore |= !std::ranges::equal(needed, setOfHeadAt(turn));
                std::ranges::copy(needed, setOfHeadAt(turn).begin());
            }
            if (isAsked) {
                if (!blocksThatYield.quickGet(block.index())) {
                    m_wordsReadLastAtTurns[turn].first = static_cast<uint32_t>(readLast.size());
                    std::span<uint64_t> neededBehind = setAt(behind);
                    for (size_t index = 0; index < lengthOfSet; ++index)
                        neededBehind[index] = needed[index] & ~neededBehind[index];
                    forEachWordIn(neededBehind, [&](uint16_t word) {
                        readLast.append(word);
                    });
                    m_wordsReadLastAtTurns[turn].end = static_cast<uint32_t>(readLast.size());
                }
                if (turnAbove != none) {
                    m_changesOfNeedsAtTurns[turnAbove].first = static_cast<uint32_t>(changes.size());
                    std::span<uint64_t> neededAbove = setAt(above);
                    for (size_t index = 0; index < lengthOfSet; ++index)
                        neededAbove[index] ^= needed[index];
                    forEachWordIn(neededAbove, [&](uint16_t word) {
                        changes.append(word);
                    });
                    m_changesOfNeedsAtTurns[turnAbove].end = static_cast<uint32_t>(changes.size());
                }
                std::ranges::copy(needed, setAt(above).begin());
                turnAbove = turn;
            }
            if (waysInLeft[turn])
                setsAtTurns[turn] = place;
            else
                setsThatAreFree.append(place);
        }
        if (turnAbove != none) {
            m_changesOfNeedsAtTurns[turnAbove].first = static_cast<uint32_t>(changes.size());
            forEachWordIn(setAt(above), [&](uint16_t word) {
                changes.append(word);
            });
            m_changesOfNeedsAtTurns[turnAbove].end = static_cast<uint32_t>(changes.size());
        }
        return someHeadNeedsMore;
    };
    if (m_everyWayBackLeadsToWhatDominates) {
        goFromBehind(false);
        for (size_t turn : m_turnsOfHeads) {
            if (uint32_t around = m_headsAroundTurns[turn]; around != none) {
                std::span<uint64_t> needed = setOfHeadAt(turn);
                std::span<const uint64_t> neededAround = setOfHeadAt(around);
                for (size_t index = 0; index < lengthOfSet; ++index)
                    needed[index] |= neededAround[index];
            }
        }
    } else {
        while (goFromBehind(false)) { }
    }
    goFromBehind(true);

    m_changesOfNeeds = arena.createSpan<uint16_t>(changes.size());
    std::ranges::copy(changes, m_changesOfNeeds.begin());
    m_wordsReadLast = arena.createSpan<uint16_t>(readLast.size());
    std::ranges::copy(readLast, m_wordsReadLast.begin());
}

void SavedFrameEmitter::followWordsThatMayBeSetInto(BlockID block)
{
    std::span<const BlockID> order = generator().order();
    while (order[m_numberOfBlocksFollowed] != block) {
        uint32_t placeLeft = m_placesOfBlocks[order[m_numberOfBlocksFollowed].index()];
        m_code.forEachEdgeOf(order[m_numberOfBlocksFollowed], [&](EdgeID, const Edge& edge) {
            if (m_code.isReached(edge.to) && m_placesOfBlocks[edge.to.index()] > placeLeft)
                m_wordsThatMayBeSetAtStartsOfBlocks[edge.to.index()].merge(m_wordsThatMayBeSet);
        });
        BlockID next = order[++m_numberOfBlocksFollowed];
        for (uint16_t word : m_changesOfNeeds.subspan(m_changesOfNeedsAtTurns[m_numberOfBlocksFollowed].first, m_changesOfNeedsAtTurns[m_numberOfBlocksFollowed].size()))
        {
            bool isNeeded = !m_wordsNeeded.get(word);
            m_wordsNeeded.set(word, isNeeded);
            if (!m_numbersOfNeededWordsAtPlaces.empty())
                m_numbersOfNeededWordsAtPlaces[placeOfWord(word)] += isNeeded ? 1 : -1;
        }
        if (!m_numbersOfNeededWordsAtPlaces.empty()) {
            for (uint16_t word : m_changesOfNeeds.subspan(m_changesOfNeedsAtTurns[m_numberOfBlocksFollowed].first, m_changesOfNeedsAtTurns[m_numberOfBlocksFollowed].size()))
                RELEASE_ASSERT(m_numbersOfNeededWordsAtPlaces[placeOfWord(word)] <= 1);
        }
        m_wordsThatMayBeSet = std::exchange(m_wordsThatMayBeSetAtStartsOfBlocks[next.index()], BitVector());
        uint32_t placeBehindLoopOfNext = m_blocksBehindFirstYield.get(next.index()) ? m_placesBehindLoops[next.index()] : 0;
        if (placeBehindLoopOfNext && !m_keepsWordsInMachineFrame && m_everyWayBackLeadsToWhatDominates && m_blocksAboveYields.quickGet(next.index()) && !m_code.at(next).isHandlerBlock() && !liesInLoopInsideStretch(next)) {
            uint32_t placeOfNext = m_placesOfBlocks[next.index()];
            for (uint32_t word = m_tracedWordsDefinedInFrontOfPlaces[placeOfNext]; word < m_tracedWordsDefinedInFrontOfPlaces[placeBehindLoopOfNext]; ++word) {
                if (m_wordsNeeded.get(word) || word < m_tracedWordsDefinedInFrontOfPlaces[placeOfNext + 1])
                    m_wordsThatMayBeSet.set(word);
            }
            m_wordsNeededAtHeads.set(next.index() + 1, m_wordsNeeded);
        } else if (uint32_t placeBehindLoop = placeBehindLoopOfNext) {
            for (uint32_t word = m_everyWayBackLeadsToWhatDominates ? m_tracedWordsDefinedInFrontOfPlaces[m_placesOfBlocks[next.index()]] : 0; word < m_tracedWordsDefinedInFrontOfPlaces[placeBehindLoop]; ++word)
                m_wordsThatMayBeSet.set(word);
        }
    }

    m_wordsToClearAtStart.shrink(0);
    if (!m_blocksBehindFirstYield.get(block.index()) || !m_blocksAboveYields.quickGet(block.index()))
        return;
    BitVector dead = m_wordsThatMayBeSet;
    dead.exclude(m_wordsNeeded);
    uint32_t place = m_placesOfBlocks[block.index()];
    for (uint32_t word = m_tracedWordsDefinedInFrontOfPlaces[place]; word < m_tracedWordsDefinedInFrontOfPlaces[place + 1]; ++word)
        dead.clear(word);
    bool shares = !m_numbersOfNeededWordsAtPlaces.empty();
    BitVector placesFilledHere;
    for (uint32_t word = m_tracedWordsDefinedInFrontOfPlaces[place]; shares && word < m_tracedWordsDefinedInFrontOfPlaces[place + 1]; ++word)
        placesFilledHere.set(placeOfWord(word));
    for (size_t word : dead) {
        if (!shares || (!m_numbersOfNeededWordsAtPlaces[placeOfWord(word)] && !placesFilledHere.get(placeOfWord(word))))
            m_wordsToClearAtStart.append(static_cast<uint32_t>(word));
    }
    for (uint16_t word : m_wordsReadLast.subspan(m_wordsReadLastAtTurns[m_numberOfBlocksFollowed].first, m_wordsReadLastAtTurns[m_numberOfBlocksFollowed].size())) {
        if (m_wordsThatMayBeSet.get(word))
            m_wordsToClearAtStart.append(word);
    }
}

Vector<uint32_t, 8> SavedFrameEmitter::findWordsToClearOnWayBackAlong(const Edge& edge)
{
    Vector<uint32_t, 8> words;
    auto needed = m_wordsNeededAtHeads.find(edge.to.index() + 1);
    if (needed == m_wordsNeededAtHeads.end())
        return words;
    BitVector dead = m_wordsThatMayBeSet;
    dead.exclude(needed->value);
    uint32_t placeOfHead = m_placesOfBlocks[edge.to.index()];
    for (size_t word : dead) {
        if (word < m_tracedWordsDefinedInFrontOfPlaces[placeOfHead] || word >= m_tracedWordsDefinedInFrontOfPlaces[placeOfHead + 1])
            words.append(static_cast<uint32_t>(word));
    }
    return words;
}

void SavedFrameEmitter::clearWordsOnWayBackAlong(const Edge& edge)
{
    Vector<uint32_t, 8> words = findWordsToClearOnWayBackAlong(edge);
    if (words.isEmpty())
        return;
    Machine::DeclaredAccessesSuspension suspension(m_out);
    LValue frame = heapFrameIsNamed() ? m_heapFrame : readHeapFrameOutOfSlot();
    for (uint32_t word : words)
        m_out.store64(m_out.int64Zero, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame, offsetOfWordInHeapFrame(word)));
}

void SavedFrameEmitter::clearWordsOfWhatIsDeadAcross(const ValueContext& yield)
{
    BitVector wordsAliveAcross;
    auto note = [&](ValueID id) {
        if (id && isKeptInHeapFrame(id) && isTracedInHeapFrame(id))
            wordsAliveAcross.set(m_wordsOfKeptValues[id.index()] - 1);
    };
    for (ValueID live : m_code.liveAt(yield.id()))
        note(live);
    for (ValueID held : heldOperandsOf(yield.value()))
        note(held);
    if (!m_placesOfWords.empty()) {
        BitVector rawWordsAlive;
        for (ValueID live : m_code.liveAt(yield.id())) {
            if (live && isKeptInHeapFrame(live) && !isTracedInHeapFrame(live))
                rawWordsAlive.set(m_wordsOfKeptValues[live.index()] - 1);
        }
        BitVector rawPlacesTaken;
        for (size_t word : rawWordsAlive)
            RELEASE_ASSERT(!rawPlacesTaken.set(placeOfWord(word)));
    }
    if (EdgeID handler = m_code.handlerOf(yield.block()))
        m_wordsThatMayBeSetAtStartsOfBlocks[m_code.at(handler).to.index()].merge(m_wordsThatMayBeSet);
    if (yield.id() != m_firstYield) {
        m_wordsThatMayBeSet.exclude(wordsAliveAcross);
        BitVector placesTaken;
        if (!m_placesOfWords.empty()) {
            for (size_t word : wordsAliveAcross)
                RELEASE_ASSERT(!placesTaken.set(placeOfWord(word)));
        }
        for (size_t word : m_wordsThatMayBeSet) {
            if (!placesTaken.set(placeOfWord(word)))
                clearWordOfHeapFrame(word);
        }
    }
    m_wordsThatMayBeSet = WTF::move(wordsAliveAcross);
}

void SavedFrameEmitter::clearWordOfHeapFrame(size_t word)
{
    if (m_slotOfKeptWords) {
        m_out.store64(m_out.int64Zero, wordInFrame(m_slotOfKeptWords, placeOfWord(word)));
        return;
    }
    m_out.store64(m_out.int64Zero, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), heapFrame(), offsetOfWordInHeapFrame(static_cast<uint32_t>(word))));
}

ValueID SavedFrameEmitter::readBackOutOfHeapFrame(BlockID block, ValueID yieldInFront)
{
    bool isBehindFirstYield = standsBehindFirstYield(block);
    if (!isBehindFirstYield && !m_hasPassedFirstYield)
        return { };

    if (!yieldInFront) {
        for (ValueID parameter : m_code.at(block).parameters())
            m_stretchesWhereNamed[parameter.index()] = m_stretch;
    }

    ValueID neverStored;
    forEachValueReadInStretch(block, yieldInFront, [&](ValueID id) {
        if (!id || !isKeptInHeapFrame(id))
            return;
        if (m_keepsWordsInMachineFrame ? isNamedInThisStretch(std::exchange(m_stretchesWhereNamed[id.index()], m_stretch)) : isNamedAbove(m_stretchesWhereNamed[id.index()]))
            return;
        m_stretchesWhereNamed[id.index()] = m_stretch;
        if (!m_valuesStoredInHeapFrame.get(id.index()) || (isBehindFirstYield && isTracedInHeapFrame(id) && !mayBeSetInHeapFrame(id))) {
            neverStored = id;
            return;
        }
        if (!isBehindFirstYield) {
            generator().nameAgain(id, m_keptValuesAsDefined[id.index()]);
            return;
        }
        generator().nameAgain(id, readOutOfHeapFrame(heapFrame(), id));
    }, [&](ValueID id) {
        m_stretchesWhereNamed[id.index()] = m_stretch;
    });
    if (!yieldInFront && !neverStored) {
        for (uint32_t word : m_wordsToClearAtStart) {
            clearWordOfHeapFrame(word);
            m_wordsThatMayBeSet.clear(word);
        }
    }
    m_standsAtStartOfStretch = false;
    return neverStored;
}

void SavedFrameEmitter::passHandlerArgumentsInLandingPad(BlockID site, const Edge& handler)
{
    Vector<std::pair<ValueID, LValue>, 8> namedAtSite;
    {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
        bool isBehindFirstYield = standsBehindFirstYield(site);
        LValue frame = nullptr;
        for (ValueID argument : handler.arguments()) {
            if (!isKeptInHeapFrame(argument) || !m_valuesStoredInHeapFrame.get(argument.index()))
                continue;
            if (isBehindFirstYield && isTracedInHeapFrame(argument) && !mayBeSetInHeapFrame(argument))
                continue;
            if (isBehindFirstYield && !frame)
                frame = readHeapFrameOutOfSlot();
            namedAtSite.append({ argument, generator().wordOf(argument) });
            generator().nameAgain(argument, isBehindFirstYield ? readOutOfHeapFrame(frame, argument) : m_keptValuesAsDefined[argument.index()]);
        }
    }
    {
        SetForScope whilePassing(m_passesHandlerArguments, true);
        generator().passHandlerArguments(handler);
    }
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

void SavedFrameEmitter::clearWordsInMachineFrame()
{
    for (uint32_t word = 0; m_slotOfKeptWords && word < m_numberOfTracedPlaces; ++word)
        m_out.store64(m_out.int64Zero, wordInFrame(m_slotOfKeptWords, word));
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
    if (kindOfFunction() == CoroutineKind::AsyncGenerator) {
        m_out.store32As8(m_out.constInt32(static_cast<int32_t>(CoroutineKind::AsyncGenerator)), m_out.address(fields, frame, SavedFrame::offsetOfKind()));
        m_out.store32(m_out.int32One, m_out.address(fields, frame, SavedFrame::offsetOfLiveSlotMapOffsetPlusOne()));
        emitter<BarrierEmitter>().fenceAfterInitialization();
        m_out.store64(frame, m_out.address(fields, generatorCell.value(), JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Frame))));
        m_out.store64(frame, m_out.address(fields, generatorCell.value(), JSAsyncGenerator::offsetOfInternalField(static_cast<unsigned>(JSAsyncGenerator::Field::Next))));
        m_out.storePtr(frame, m_out.address(m_heaps.frameSlots, m_out.slotBase(m_slotOfHeapFrame)));
        m_heapFrame = frame;
        m_stretchOfHeapFrame = m_stretch;
    } else {
        emitter<BarrierEmitter>().fenceAfterInitialization();
        m_out.storePtr(frame, m_out.address(fields, generatorCell.value(), Generator::offsetOfSavedFrame()));
    }
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
        if (kindOfFunction() == CoroutineKind::Generator) {
            LValue state = m_out.load32(stateOf(generatorCell));
            m_out.store32(m_out.select(m_out.equal(state, m_out.constInt32(static_cast<int32_t>(Generator::State::Running))), suspendedAtYield, state), stateOf(generatorCell));
        }
        m_hasPassedFirstYield = true;
    } else {
        m_out.storePtr(resumeAddress, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), heapFrame(), SavedFrame::offsetOfResumeAddress()));
        m_out.store32(suspendedAtYield, stateOf(generatorCell));
    }
    clearWordsOfWhatIsDeadAcross(value);
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

GeneratorFunctionOrigin SavedFrameEmitter::originOfGeneratorFunction(FunctionID function) const
{
    return m_plan.functionKind(m_program, conventions().closureEntryOf(function)).isInRuntimeModule ? GeneratorFunctionOrigin::Library : GeneratorFunctionOrigin::Program;
}

CellPointer SavedFrameEmitter::allocateGenerator(std::optional<CellPointer> closure, GeneratorFunctionOrigin origin, LBasicBlock slowPath)
{
    std::optional<Word32> structureID;
    std::optional<Pointer> structure;
    if (closure && origin == GeneratorFunctionOrigin::Library) {
        branchUnless(equal(loadStructureID(*closure), loadImmutable32(AOT_INSTANCE_OFFSET(closures.closureHeaders) + closureStructureOfGeneratorFunctions(false) * sizeof(uint64_t))), slowPath);
        structureID = Word32(m_out.load32(mutableInstanceField(AOT_INSTANCE_OFFSET(coroutines.structureIDsOfGeneratorsOfLibrary))));
        branchUnless(isNotZero(*structureID), slowPath);
        structure = decodeStructureID(*structureID);
    } else if (closure) {
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
    join.add(allocateGenerator(closure, originOfGeneratorFunction(value->function()), slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewGenerator, { closure ? boxCell(*closure) : Boxed(m_out.int64Zero) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void SavedFrameEmitter::generateNewGeneratorOfLibrary(const ValueContext& value)
{
    CellPointer closure = in<CellPointer>(value->calleeOperand());
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateGenerator(closure, GeneratorFunctionOrigin::Library, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewGeneratorOfLibrary, { boxCell(closure) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void SavedFrameEmitter::generateNewAsyncGenerator(const ValueContext& value)
{
    define(value, CellPointer(callStub(value, AOT::newAsyncGenerator(originOfGeneratorFunction(value->function())), { in<CellPointer>(value->calleeOperand()).value() }).first));
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

static std::optional<StepConsumer> stepConsumerOf(CoroutineKind kind)
{
    switch (kind) {
    case CoroutineKind::AsyncFunction:
        return StepConsumer::AsyncFunction;
    case CoroutineKind::AsyncGenerator:
        return StepConsumer::AsyncGenerator;
    case CoroutineKind::Generator:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateStepAsyncGenerator(const ValueContext& value)
{
    ValueID driver = value->driverOperand();
    std::optional<StepConsumer> consumer = stepConsumerOf(kindOfFunction());
    if (!consumer || opcodeOf(driver) == Opcode::Absent) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    LValue awaitsCall = value->calledBuiltin() ? m_out.constInt64(consumerOfStepAwaitsCall) : m_out.int64Zero;
    if (ValueID goesByDriver = value->goesByDriverOperand())
        awaitsCall = m_out.select(in<Condition>(goesByDriver).value(), m_out.int64Zero, awaitsCall);
    defineValueAndWord(value, suspend(value, stepAsyncGenerator(*consumer), { in<CellPointer>(value->generatorOperand()).value(), cellOrZero(driver), in<Boxed>(value->valueOperand()).value(), awaitsCall }, driver));
}

void SavedFrameEmitter::generatePromiseAllOfElements(const ValueContext& value)
{
    static_assert(maxComputedElementsOfArrayLiteral <= SavedFrame::maxCombinatorElements);
    Vector<Boxed, 8> elements;
    for (ValueID element : value->elementsOperands())
        elements.append(in<Boxed>(element));
    Pointer buffer = argumentBuffer(elements.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::PromiseAll, { word32(static_cast<int32_t>(elements.size())), buffer, word32(static_cast<int32_t>(value->directCalls())) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
