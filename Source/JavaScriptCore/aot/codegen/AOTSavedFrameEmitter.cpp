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
        AwaitedOperand form = value->takesExtraTurn() ? AwaitedOperand::ValueAfterTurn : m_plan.moduleEvaluation().isBodyResumedByJobOfLoader(function()) ? AwaitedOperand::ValueInModuleBody : AwaitedOperand::Value;
        define(value, Boxed(suspend(value, awaitBySavingFrame(form), { in<Boxed>(awaited).value() }, driver, value->takesExtraTurn() ? RunsCodeOfAwaited::InFrameOfFunction : RunsCodeOfAwaited::BehindFunction).first));
        return;
    }
    case CoroutineKind::AsyncGenerator:
        if (value->takesExtraTurn()) {
            fail(value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
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

void SavedFrameEmitter::generateNewHeapWords(const ValueContext& value)
{
    uint32_t numberOfWords = value->numberOfWords();
    RELEASE_ASSERT(numberOfWords && numberOfWords <= SavedFrame::maxNumberOfWords);
    if (numberOfWords <= SavedFrame::maxWordsAllocatedInStub) {
        define(value, boxCell(CellPointer(callStub(value, AOT::newHeapFrame(), { m_out.constInt64(numberOfWords), m_out.intPtrZero }).first)));
        return;
    }
    CellPointer frame = callOperation<CellPointer>(value, RuntimeOperation::NewHeapFrame, { word32(static_cast<int32_t>(numberOfWords)), Pointer(m_out.intPtrZero) });
    define(value, boxCell(frame));
}

void SavedFrameEmitter::generateGetHeapFrameWord(const ValueContext& value)
{
    LValue frame = in<Boxed>(value->frameOperand()).value();
    defineNarrowed(value, Boxed(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame, offsetOfWordInHeapFrame(value->index())))));
}

void SavedFrameEmitter::generatePutHeapFrameWord(const ValueContext& value)
{
    CellPointer frame = unboxCell(in<Boxed>(value->frameOperand()));
    ValueID operand = value->valueOperand();
    const Type& storedType = typeOf(operand);
    LValue stored = in<Boxed>(operand).value();
    if (m_cellCageBase && storedType.mayBeUnspecified())
        stored = m_out.select(m_out.equal(stored, m_cellCageBase), m_out.int64Zero, stored);
    m_out.store64(stored, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), frame.value(), offsetOfWordInHeapFrame(value->index())));
    if (value->remembersFrame())
        emitter<BarrierEmitter>().barrierAfterStore(value, frame, storedType);
}

static constexpr uint32_t maxWordsOfHeapFrameMadeInLine = 16;
static_assert(maxWordsOfHeapFrameMadeInLine <= SavedFrame::maxWordsAllocatedInStub);

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
    std::optional<StepConsumer> consumer = stepConsumerOf(kindOfFunction(), value->awaitsCall());
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
