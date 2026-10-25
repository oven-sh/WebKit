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
#include "AOTBuiltinIDs.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCoroutines.h"
#include "AOTFrameMapEmitter.h"
#include "AOTGenerator.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTTypeTestEmitter.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "MarkedSpace.h"
#include <array>
#include <optional>

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
    return coroutineOfBody(m_function).kind;
}

LValue SavedFrameEmitter::cellOrZero(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    Boxed boxed = in<Boxed>(driver);
    return m_out.select(isCell(boxed).value(), boxed.value(), m_out.int64Zero);
}

// The site word names the location of this word, and a constant has none, so nothing here is folded by type. Who reads the word tells a promise from undefined.
LValue SavedFrameEmitter::driverAsPassed(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    return in<Boxed>(driver).value();
}

StubCallResult SavedFrameEmitter::suspend(const ValueContext& value, StubKey key, std::initializer_list<LValue> operands, ValueID driverID)
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSummary& summary = catalog.summaryOf(key);
    const StubSignature& signature = summary.signature;
    RELEASE_ASSERT(operands.size() && signature.numberOfOperands == operands.size() + 1);
    Reg siteWordRegister = signature.operands[operands.size()];
    bool hasMode = signature.result == StubResult::TwoGPRs;
    RELEASE_ASSERT(hasMode || signature.result == StubResult::GPR);
    LType type = hasMode ? m_proc.addTuple({ B3::Int64, B3::Int64 }) : B3::Int64;

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
    B3::PatchpointValue* patchpoint = calls.createCallPatchpoint(value, request);
    patchpoint->setNeedsFrameRecord(true);
    unsigned operandIndex = 0;
    for (LValue operand : operands) {
        RELEASE_ASSERT(operand);
        patchpoint->append(B3::ConstrainedValue(operand, B3::ValueRep::reg(signature.operands[operandIndex++])));
    }
    unsigned driverChild = m_proc.resultCount(type) + patchpoint->numChildren();
    patchpoint->append(B3::ConstrainedValue(driver, B3::ValueRep::LateColdAny));
    FrameMapEmitter::Roots roots = emitter<FrameMapEmitter>().appendRoots(patchpoint, value);
    patchpoint->resultConstraints = { B3::ValueRep::reg(signature.firstResult) };
    if (hasMode)
        patchpoint->resultConstraints.append(B3::ValueRep::reg(signature.secondResult));

    uint32_t site = calls.addSite(value);
    patchpoint->setGenerator([records = &m_records, build = &build(), function = m_id, valueID = value.id(), key, site, roots, driverChild, siteWordGPR = siteWordRegister.gpr()](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        std::optional<uint64_t> word = FrameMapEmitter::siteWordFor(records->frameMaps, params, roots, driverChild);
        if (!word) {
            build->diagnostics().add(InternalError { .kind = InternalErrorKind::FrameMapNotEncodable, .function = function, .value = valueID });
            jit.breakpoint();
            return;
        }
        if (*word >> 32)
            jit.move(CCallHelpers::TrustedImm64(*word), siteWordGPR);
        else
            jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(*word)), siteWordGPR);
        records->stubCalls.call(jit, key, OptionalIndex { site });
    });
    calls.finishCallPatchpoint(value, patchpoint, site, request);

    if (!hasMode)
        return { patchpoint, nullptr };
    return { m_out.extract(patchpoint, 0), m_out.extract(patchpoint, 1) };
}

// B3 gives all FramePointer values of a function one value, and a frame that was saved goes on at another address. A suspension writes every heap, so nothing moves this read across one.
Pointer SavedFrameEmitter::framePointerAtThisPoint()
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::pointerType());
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.reads = B3::HeapRange::top();
    patchpoint->setNeedsFrameRecord(true);
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.move(GPRInfo::callFrameRegister, params[0].gpr());
    });
    return Pointer(patchpoint);
}

// The conditions on which the stub AwaitBySavingFrame goes on without saving the frame. Whatever queues a job calls the run time, so the words of the queue are run-time state.
void SavedFrameEmitter::generateAwaitMayContinue(const ValueContext& value)
{
    const B3::AbstractHeap& queueWords = m_heaps.entire(AbstractHeap::RuntimeState);
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isFrameOfLastJob = newBlock();
        Pointer frameThatMayContinue(m_out.loadPtr(mutableInstanceField(AOT_INSTANCE_OFFSET(coroutines.frameThatMayContinue))));
        branch(equal(frameThatMayContinue, framePointerAtThisPoint()), { B3::FrequentedBlock(isFrameOfLastJob, targets.taken.frequency()), targets.notTaken });

        m_out.appendTo(isFrameOfLastJob);
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
        AwaitedOperand form = value->takesExtraTurn() ? AwaitedOperand::ValueAfterTurn : m_function.isModuleBody() ? AwaitedOperand::ValueInModuleBody : AwaitedOperand::Value;
        define(value, Boxed(suspend(value, awaitBySavingFrame(form), { in<Boxed>(awaited).value() }, driver).first));
        return;
    }
    case CoroutineKind::AsyncGenerator:
        // The engine resumes an async generator, and none of its jobs waits a turn more.
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
    if (kindOfFunction() != CoroutineKind::AsyncFunction) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    Vector<Operand, 8> arguments;
    for (ValueID argument : value->argumentsOperands())
        arguments.append(get(argument));
    arguments.append(undefined());
    Boxed returned = emitter<CallEmitter>().callDirect(value, value->function(), arguments.span(), { .isTailCall = value->isDemotedTailCall() }).as<Boxed>();
    define(value, Boxed(suspend(value, awaitCallBySavingFrame(), { returned.value() }, value->promiseOperand()).first));
}

void SavedFrameEmitter::suspendGenerator(const ValueContext& value, GeneratorSuspension form)
{
    Boxed yielded = in<Boxed>(value->valueOperand());
    ValueID generator = value->generatorOperand();
    defineValueAndWord(value, suspend(value, AOT::suspendGenerator(form), { in<CellPointer>(generator).value(), yielded.value() }, generator));
}

// What a yield and a step of a generator answer.
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

// The Structure is the one that InternalFunction::createSubclassStructure answers without reading the property `prototype`: the function remembers it, and it was made for this class in this realm.
CellPointer SavedFrameEmitter::allocateGenerator(CellPointer closure, LBasicBlock slowPath)
{
    constexpr ptrdiff_t offsetOfStructureID = FunctionRareData::offsetOfInternalFunctionAllocationProfile() + InternalFunctionAllocationProfile::offsetOfStructureID();
    Pointer taggedRareData = loadField<Pointer>(closure, m_heaps.JSFunction_executableOrRareData);
    branchUnless(Condition(m_out.testNonZeroPtr(taggedRareData.value(), m_out.constIntPtr(JSFunction::rareDataTag))), slowPath);
    // A store to `prototype` makes the function forget the Structure, so this is a read of its named properties.
    Word32 structureID(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), taggedRareData.value(), offsetOfStructureID - static_cast<ptrdiff_t>(JSFunction::rareDataTag))));
    branchUnless(isNotZero(structureID), slowPath);

    Pointer structure = decodeStructureID(structureID);
    Pointer classInfos = loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.instanceClassInfos));
    Pointer classInfo(m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, classInfos.value(), static_cast<ptrdiff_t>(BuiltinClasses::Generator.index()) * sizeof(const ClassInfo*))));
    branchUnless(equal(loadField<Pointer>(structure, m_heaps.Structure_classInfo), classInfo), slowPath);
    branchUnless(equal(loadField<Pointer>(structure, m_heaps.Structure_realm), loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject))), slowPath);

    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    Pointer allocator = cells.allocatorForSize(loadImmutablePointer(AOT_INSTANCE_OFFSET(allocation.cellSpace)), sizeof(Generator));
    branchUnless(Condition(m_out.notNull(allocator.value())), slowPath);
    CellPointer result = cells.allocateHeapCell(allocator, sizeof(Generator), slowPath);
    {
        CodeGenerator::InitializationScope scope(generator());
        m_out.store32(structureID.value(), result.value(), m_heaps.JSCell_structureID);
        m_out.store32(m_out.load32(structure.value(), m_heaps.Structure_indexingModeIncludingHistory), result.value(), m_heaps.JSCell_usefulBytes);
        m_out.storePtr(m_out.intPtrZero, result.value(), m_heaps.JSObject_butterfly);
        m_out.store64(m_out.int64Zero, m_out.address(m_heaps.entire(AbstractHeap::InternalFields), result.value(), Generator::offsetOfSavedFrame()));
        m_out.store32(m_out.int32Zero, stateOf(result));
        m_out.store32(m_out.int32Zero, m_out.address(m_heaps.entire(AbstractHeap::PrototypeLinks), result.value(), Generator::offsetOfHasOtherPrototype()));
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    return result;
}

// The closure of a generator function has the full form, whoever calls it.
void SavedFrameEmitter::generateNewGenerator(const ValueContext& value)
{
    CellPointer closure = in<CellPointer>(value->calleeOperand());
    if (value.shape() != CodeShape::Fast) {
        define(value, CellPointer(callStub(value, AOT::newGenerator(NewGeneratorKind::Sync), { closure.value() }).first));
        return;
    }

    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    join.add(allocateGenerator(closure, slowPath));

    m_out.appendTo(slowPath);
    join.add(callOperation<CellPointer>(value, RuntimeOperation::NewGenerator, { boxCell(closure), word32(false) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void SavedFrameEmitter::generateNewAsyncGenerator(const ValueContext& value)
{
    define(value, CellPointer(callStub(value, AOT::newGenerator(NewGeneratorKind::Async), { in<CellPointer>(value->calleeOperand()).value() }).first));
}

void SavedFrameEmitter::generateTakeGeneratorPrototype(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::TakeGeneratorPrototype, { in<CellPointer>(value->generatorOperand()), boxCell(in<CellPointer>(value->calleeOperand())) });
}

TypedPointer SavedFrameEmitter::stateOf(CellPointer generator)
{
    return m_out.address(m_heaps.entire(AbstractHeap::InternalFields), generator.value(), Generator::offsetOfState());
}

// Only a generator whose start was left to its first step has this state and no frame.
void SavedFrameEmitter::generateIsGeneratorUnstarted(const ValueContext& value)
{
    CellPointer generator = in<CellPointer>(value->generatorOperand());
    Integer saved(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::InternalFields), generator.value(), Generator::offsetOfSavedFrame())));
    defineTest(value, logicalAnd(isZero(saved), isZero(Word32(m_out.load32(stateOf(generator))))));
}

void SavedFrameEmitter::generateIsGeneratorRunning(const ValueContext& value)
{
    Word32 state(m_out.load32(stateOf(in<CellPointer>(value->generatorOperand()))));
    defineTest(value, equal(state, word32(static_cast<int32_t>(Generator::State::Running))));
}

void SavedFrameEmitter::generateMarkGeneratorRunning(const ValueContext& value)
{
    m_out.store32(m_out.constInt32(static_cast<int32_t>(Generator::State::Running)), stateOf(in<CellPointer>(value->generatorOperand())));
}

// As the stub that steps a generator: a suspension stores its state, and a body that returned left the state as it was.
void SavedFrameEmitter::generateTakeGeneratorStepStatus(const ValueContext& value)
{
    CellPointer generator = in<CellPointer>(value->generatorOperand());
    Word32 state(m_out.load32(stateOf(generator)));
    Condition hasReturned = equal(state, word32(static_cast<int32_t>(Generator::State::Running)));
    m_out.store32(select(hasReturned, word32(static_cast<int32_t>(Generator::State::Completed)), state).value(), stateOf(generator));
    define(value, select(hasReturned, word32(static_cast<int32_t>(GeneratorStepStatus::Completed)), word32(static_cast<int32_t>(GeneratorStepStatus::Yielded))));
}

void SavedFrameEmitter::generateReturnAsyncResult(const ValueContext& value)
{
    Boxed promise = in<Boxed>(value->promiseOperand());
    Boxed result = in<Boxed>(value->valueOperand());
    define(value, Boxed(callStub(value, returnAsyncResult(), { promise.value(), result.value() }).first));
}

void SavedFrameEmitter::generatePromiseAllOfElements(const ValueContext& value)
{
    Vector<Boxed, 8> elements;
    for (ValueID element : value->elementsOperands())
        elements.append(in<Boxed>(element));
    Pointer buffer = argumentBuffer(elements.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::PromiseAll, { word32(static_cast<int32_t>(elements.size())), buffer, word32(static_cast<int32_t>(value->directCalls())) }));
}

void SavedFrameEmitter::generateStepGenerator(const ValueContext& value)
{
    Boxed generator = in<Boxed>(value->generatorOperand());
    Boxed sentValue = in<Boxed>(value->valueOperand());
    LValue mode = m_out.constInt64(static_cast<int64_t>(value->resumeMode()));
    defineValueAndWord(value, callStub(value, stepGenerator(value->check()), { generator.value(), mode, sentValue.value() }));
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
        // The empty value has no tag bits, so it passes isCell.
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
    case Representation::LiteralString:
    case Representation::RawWord:
        RELEASE_ASSERT_NOT_REACHED();
    }

    const ClassPart* classPart = type.singleBuiltinClass();
    if (!classPart || classPart->builtinClass != BuiltinClasses::Generator)
        branchUnless(emitter<TypeTestEmitter>().inheritsClassInfo(cell, BuiltinClasses::Generator), otherwise);

    // The stored prototype is only known to be a cell while the flag is clear, so test the flag first.
    Word32 hasOtherPrototype(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::PrototypeLinks), cell.value(), Generator::offsetOfHasOtherPrototype())));
    branchUnless(isZero(hasOtherPrototype), otherwise);
    branchUnless(Condition(m_out.isNull(m_out.loadPtr(cell.value(), m_heaps.JSObject_butterfly))), otherwise);
    CellPointer prototype(m_out.load64(loadStructure(cell).value(), m_heaps.Structure_prototype));
    branchUnless(equal(loadStructureID(prototype), loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.emptyGeneratorPrototypeStructureID))), otherwise);
}

void SavedFrameEmitter::generateIsUnmodifiedGenerator(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    defineTestByBranching(value, [&](BranchTargets targets) {
        branchUnlessUnmodifiedGenerator(get(operand), typeOf(operand), targets.notTaken.block());
        m_out.jump(targets.taken.block());
    });
}

void SavedFrameEmitter::generateIsUnmodifiedAsyncGenerator(const ValueContext& value)
{
    CellPointer generator = in<CellPointer>(value->valueOperand());
    const ClassPart* classPart = typeOf(value->valueOperand()).singleBuiltinClass();
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock otherwise = targets.notTaken.block();
        if (!classPart || classPart->builtinClass != BuiltinClasses::AsyncGenerator)
            branchUnless(hasCellType(generator, JSAsyncGeneratorType), otherwise);
        branchUnless(Condition(m_out.isNull(m_out.loadPtr(generator.value(), m_heaps.JSObject_butterfly))), otherwise);
        Boxed prototype(m_out.load64(loadStructure(generator).value(), m_heaps.Structure_prototype));
        branchUnless(isCell(prototype), otherwise);
        branchUnless(equal(loadStructureID(unboxCell(prototype)), loadImmutable32(AOT_INSTANCE_OFFSET(coroutines.emptyAsyncGeneratorPrototypeStructureID))), otherwise);
        m_out.jump(targets.taken.block());
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

// The frame may be saved at this site: where the iterator does not answer at once, the stub suspends the function here, and whoever finishes the step resumes it with both results.
void SavedFrameEmitter::generateStepAsyncGenerator(const ValueContext& value)
{
    // An awaited call of `next` names no driver.
    bool awaitsCall = m_function.at(value->driverOperand()).opcode == Opcode::Absent;
    std::optional<StepConsumer> consumer = stepConsumerOf(kindOfFunction(), awaitsCall);
    if (!consumer) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    ValueID driver = awaitsCall ? driverOf(m_function) : value->driverOperand();
    defineValueAndWord(value, suspend(value, stepAsyncGenerator(*consumer), { in<CellPointer>(value->generatorOperand()).value(), cellOrZero(driver), in<Boxed>(value->valueOperand()).value() }, driver));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
