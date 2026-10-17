/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTTypedObject.h"
#include "AsyncContextSwapScope.h"
#include "AsyncGeneratorPrototype.h"
#include "B3StackmapGenerationParams.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSSentinel.h"
#include "MicrotaskQueueInlines.h"

namespace JSC { namespace AOT {

static ALWAYS_INLINE void resumeWhenSettled(JSGlobalObject* globalObject, VM& vm, JSValue awaited, InternalMicrotask task, JSCell* resumeClosure)
{
    if (!awaited.isObject()) {
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), awaited, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    if (awaited.asCell()->structureID() == globalObject->promiseStructure()->id()) {
        uncheckedDowncast<JSPromise>(awaited)->performPromiseThenWithInternalMicrotask(vm, task, nullptr, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    if (Typed::driveAsyncIterator(globalObject, vm, awaited.asCell(), asObject(resumeClosure)))
        return;
    if (awaited.asCell()->type() != JSPromiseType && isDefinitelyNonThenable(asObject(awaited), globalObject)) {
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), awaited, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        return;
    }
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, awaited, task, resumeClosure);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSuspendAsyncFunction, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue awaited))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedParkAsyncFunction, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue awaited))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.writeBarrier(*reinterpret_cast<JSCell* const*>(reinterpret_cast<const uint8_t*>(resumeClosure) + Typed::Closure::offsetOfCaptures()));
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSuspendAtFirstAwait, void, (Instance* instance, JSCell* frame, EncodedJSValue awaited, uint64_t request))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto word = [](JSCell* cell, ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + offset);
    };
    word(frame, ResumeClosureRequest::frameWordOf(request) * sizeof(uint64_t)) = 0;
    JSCell* resumeClosure = instance->typed->allocate(vm, ClosureRequest::allocationOf(request));
    word(resumeClosure, JSObject::butterflyOffset()) = 0;
    word(resumeClosure, JSCallee::offsetOfScopeChain()) = std::bit_cast<uint64_t>(instance->globalObject);
    word(resumeClosure, JSFunction::offsetOfExecutableOrRareData()) = std::bit_cast<uint64_t>(instance->typedExecutables[ResumeClosureRequest::executableOf(request)]);
    word(resumeClosure, Typed::Closure::offsetOfCaptures()) = std::bit_cast<uint64_t>(frame);
    WTF::storeStoreFence();
    word(resumeClosure, Typed::Closure::offsetOfEntry()) = instance->typedFunctions[ClosureRequest::functionOf(request)] | 1ULL << ClosureWord::isNotCallableBit;
    WTF::storeStoreFence();
    word(frame, ResumeClosureRequest::frameWordOf(request) * sizeof(uint64_t)) = std::bit_cast<uint64_t>(resumeClosure);
    vm.writeBarrier(frame);
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSaveFrameAndAwait, JSCell*, (Instance* instance, EncodedJSValue awaited, uint64_t frame, uint64_t bottom, uint64_t resumeAddress, uint64_t extra))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Typed::SavedFrame* saved = Typed::SavedFrame::save(vm, *instance, std::bit_cast<uint64_t*>(frame), std::bit_cast<const uint64_t*>(bottom), std::bit_cast<void*>(resumeAddress), std::bit_cast<const uint64_t*>(extra));
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, saved);
    if (scope.exception()) [[unlikely]]
        saved->willBeRestoredToStack();
    OPERATION_RETURN(scope, saved->promise());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAwaitWithSavedFrame, JSCell*, (Instance* instance, EncodedJSValue awaited, JSCell* saved))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.writeBarrier(saved);
    if (!awaited)
        OPERATION_RETURN(scope, uncheckedDowncast<Typed::SavedFrame>(saved)->promise());
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, saved);
    if (scope.exception()) [[unlikely]]
        uncheckedDowncast<Typed::SavedFrame>(saved)->willBeRestoredToStack();
    OPERATION_RETURN(scope, uncheckedDowncast<Typed::SavedFrame>(saved)->promise());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStepOrAwait, EncodedJSValue, (Instance* instance, EncodedJSValue awaited, JSCell* saved))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSObject* result = driveAsyncFromSyncIterator(instance->globalObject, uncheckedDowncast<JSAsyncFromSyncIterator>(JSValue::decode(awaited)), asObject(saved));
    return result ? JSValue::encode(result) : JSValue::encode(JSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPromiseThen, void, (Instance* instance, JSCell* promise, EncodedJSValue onFulfilled, EncodedJSValue onRejected, JSCell* derived))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, JSValue::decode(onFulfilled), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPromiseCatch, JSCell*, (Instance* instance, JSCell* promise, EncodedJSValue onRejected))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSPromise* derived = JSPromise::create(vm, globalObject->promiseStructure());
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, jsUndefined(), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope, derived);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryNewPromiseSubclass, EncodedJSValue, (Instance* instance, EncodedJSValue parent, EncodedJSValue newTarget, EncodedJSValue executor))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSValue::decode(parent) != globalObject->promiseConstructor() || !JSValue::decode(newTarget).isObject() || !JSValue::decode(executor).isCallable())
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, promiseStructure, asObject(JSValue::decode(newTarget)), globalObject->promiseConstructor());
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(JSPromise::create(vm, structure)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAsyncGeneratorYield, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->tryToYieldInPlace(*instance, vm, JSValue::decode(operand)))
        OPERATION_RETURN(scope);
    uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtYield();
    resumeWhenSettled(globalObject, vm, JSValue::decode(operand), InternalMicrotask::AOTAsyncGeneratorResume, resumeClosure);
    uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtAwait();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetIteratorNext, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterator = JSValue::decode(encodedIterator);
    if (auto* generator = dynamicDowncast<Typed::Generator>(iterator); generator && generator->hasUnmodifiedIteration(globalObject))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    OPERATION_RETURN(scope, JSValue::encode(iterator.get(globalObject, vm.propertyNames->next)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGeneratorResume, EncodedJSValue, (Instance* instance, JSCell* generator, EncodedJSValue mode, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* inner = uncheckedDowncast<Typed::Generator>(generator);
    auto resumeMode = static_cast<Typed::Generator::ResumeMode>(JSValue::decode(mode).asInt32());
    if (resumeMode != Typed::Generator::ResumeMode::Next && !inner->hasUnmodifiedIteration(globalObject))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    bool isDone;
    OPERATION_RETURN(scope, JSValue::encode(inner->resume(globalObject, resumeMode, JSValue::decode(value), isDone)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetAsyncIteratorNext, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterator = JSValue::decode(encodedIterator);
    if (iterator.isCell() && iterator.asCell()->type() == JSAsyncFromSyncIteratorType)
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(iterator); generator && generator->hasUnmodifiedIteration(globalObject))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    JSValue next = iterator.get(globalObject, vm.propertyNames->next);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (iterator.inherits<Typed::AsyncGenerator>() && next == globalObject->asyncGeneratorPrototype()->getDirect(vm, vm.propertyNames->next))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    OPERATION_RETURN(scope, JSValue::encode(next));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAsyncIteratorNext, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator, EncodedJSValue encodedSent))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterator = JSValue::decode(encodedIterator);
    JSValue sent = JSValue::decode(encodedSent);
    if (iterator.asCell()->type() != JSAsyncFromSyncIteratorType) {
        bool driverCanContinueImmediately = !instance->awaitMustSuspend && instance->microtaskQueue->isEmpty() && Typed::SavedFrame::isEnteredByJobLoop(*instance, std::bit_cast<const uint64_t*>(callFrame->callerFrame()));
        OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<Typed::AsyncGenerator>(iterator)->enqueueDriver(globalObject, sent ? sent : jsUndefined(), driverCanContinueImmediately)));
    }
    auto* wrapper = uncheckedDowncast<JSAsyncFromSyncIterator>(iterator);
    if (!instance->awaitMustSuspend && instance->microtaskQueue->isEmpty() && Typed::SavedFrame::isEnteredByJobLoop(*instance, std::bit_cast<const uint64_t*>(callFrame->callerFrame()))) {
        JSPromise*& immediateStep = instance->typed->asyncGeneratorImmediateStep;
        if (!immediateStep) [[unlikely]] {
            immediateStep = JSPromise::create(vm, globalObject->promiseStructure());
            immediateStep->fulfill(vm, createIteratorResultObject(globalObject, jsUndefined(), false));
        }
        OPERATION_RETURN(scope, JSValue::encode(asyncFromSyncIteratorNext(globalObject, wrapper, sent, immediateStep)));
    }
    if (!sent || wrapper->iterationMode() != IterationMode::Generic)
        OPERATION_RETURN(scope, encodedIterator);
    OPERATION_RETURN(scope, JSValue::encode(asyncFromSyncIteratorNext(globalObject, wrapper, sent)));
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlinePromise(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::PromiseResolve:
        return !arguments.empty() && arguments[0].hasObjects() ? Writes::Heap : Writes::RuntimeState;
    case InlineBuiltin::PromiseThen:
    case InlineBuiltin::PromiseCatch:
        return Writes::RuntimeState;
    case InlineBuiltin::AwaitMayContinue:
        return Writes::Nothing;
    case InlineBuiltin::AwaitedValue:
        return Writes::RuntimeState;
    case InlineBuiltin::SuspendAsyncFunction:
    case InlineBuiltin::AwaitBySavingFrame:
    case InlineBuiltin::GetAsyncIteratorNext:
    case InlineBuiltin::AsyncIteratorNext:
    case InlineBuiltin::GetIteratorNext:
    case InlineBuiltin::GeneratorResume:
    case InlineBuiltin::AsyncGeneratorYield:
    case InlineBuiltin::ClearAsyncIteratorResult:
    case InlineBuiltin::TryNewPromiseSubclass:
        return Writes::Heap;
    case InlineBuiltin::IsAsyncIteratorDriven:
    case InlineBuiltin::GeneratorIsDone:
        return Writes::Nothing;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::isNotThenableWithoutExtraProperties(const Type& type) const
{
    auto then = m_program.find(asBytes("then"_s.span8()));
    return then && !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.objectLayouts.isEmpty() && std::ranges::all_of(type.objectLayouts, [&](LayoutID view) {
        return isObjectLayout(m_program.at(view).kind) && isOnlyInExtraProperties(m_program, view, *then);
    });
}

bool Lowering::isAwaitMayContinue(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitMayContinue;
}

bool Lowering::isOnlyAwaited(ValueID awaited)
{
    auto& made = m_function.at(awaited);
    if (made.opcode != Opcode::CallDirect || made.isJumpToContinuation())
        return false;
    auto& callee = m_program.at(made.function());
    if (!callee.resumeFunction || (callee.declaredAs & (functionIsAsync | functionIsGenerator)) != functionIsAsync)
        return false;
    if (!m_didFindNonAwaitUses) {
        m_didFindNonAwaitUses = true;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = m_function.at(id);
                unsigned awaitedArgument = UINT_MAX;
                if (user.opcode == Opcode::CallBuiltin) {
                    auto& implementation = m_builtins.at(user.builtin()).implementation;
                    if (implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::AwaitMayContinue || implementation.inlined == InlineBuiltin::AwaitedValue))
                        awaitedArgument = 0;
                    else if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::SuspendAsyncFunction)
                        awaitedArgument = 1;
                }
                for (ValueID child : user.children) {
                    if (child)
                        m_hasNonAwaitUse.set(child.index());
                }
                for (unsigned i = 0; i < user.arguments.size(); ++i) {
                    if (i != awaitedArgument)
                        m_hasNonAwaitUse.set(user.arguments[i].index());
                }
            }
            if (block.terminator.value)
                m_hasNonAwaitUse.set(block.terminator.value.index());
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    m_hasNonAwaitUse.set(argument.index());
            }
        }
    }
    return !m_hasNonAwaitUse.get(awaited.index());
}

CellPointer Lowering::awaitedObjectOrElse(ValueID awaited, LBasicBlock notObject)
{
    Operand operand = get(awaited);
    CellPointer cell;
    if (operand.rep == Rep::Cell)
        cell = operand.as<CellPointer>();
    else {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isCellCase = m_out.newBlock();
        m_out.branch(isCell(boxed, typeOf(awaited)).value(), unsure(isCellCase), unsure(notObject));
        m_out.appendTo(isCellCase);
        cell = asCell(boxed);
    }
    if (typeOf(awaited).primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt)) {
        LBasicBlock isObject = m_out.newBlock();
        m_out.branch(below(loadCellType(cell), word32(ObjectType)).value(), unsure(notObject), unsure(isObject));
        m_out.appendTo(isObject);
    }
    return cell;
}

void Lowering::branchOnAwaitMayContinue(const Value& value, BlockID goesOn, LBasicBlock mayContinue, LBasicBlock mustSuspend)
{
    static constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    ValueID awaited = value.arguments[0];
    Rep rep = get(awaited).rep;
    if (!Options::validateAOTTypedFacts() && (rep == Rep::Cell || rep == Rep::JSValue) && numberOfPredecessorsOf(goesOn) == 1 && !m_isInLoop && !m_program.isHotOutsideLoops(m_id)) {
        static_assert(static_cast<unsigned>(Stub::AwaitMayContinue) + 1 == static_cast<unsigned>(Stub::LeafAwaitMayContinue));
        LValue answer = callAllocationStub(Stub::AwaitMayContinue, { { in<Boxed>(awaited).value(), GPRInfo::argumentGPR1 }, { in<Integer>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, { Writes::RuntimeState, false }, false);
        m_inlineAwaitedValues.set(&m_function.at(goesOn), std::pair { awaited, answer });
        m_out.branch(m_out.notZero64(answer), unsure(mayContinue), unsure(mustSuspend));
        return;
    }
    auto require = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, unsure(next), unsure(mustSuspend));
        m_out.appendTo(next);
    };
    auto stateAt = [&](LValue base, ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTTyped_runtimeState, base, offset);
    };
    require(m_out.testNonZero64(in<Integer>(value.arguments[1]).value(), m_out.constInt64(CoroutineFrame::isEnteredByJobLoop)));
    LValue queue = m_out.loadPtr(stateAt(m_instance, Instance::offsetOfMicrotaskQueue()));
    require(m_out.equal(m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfStart())), m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfEnd()))));
    require(m_out.isZero32(m_out.load8ZeroExt32(stateAt(m_instance, Instance::offsetOfAwaitMustSuspend()))));
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        LValue globalObject = fixedPointer(Instance::offsetOfGlobalObject());
        LValue mustSuspendByFlags = m_out.bitOr(m_out.isZero32(m_out.load8ZeroExt32(stateAt(globalObject, JSGlobalObject::offsetOfCanFastQueueMicrotask()))), m_out.notZero32(m_out.load8ZeroExt32(stateAt(fixedPointer(Instance::offsetOfVM()), VM::offsetOfHostObservesMicrotaskBoundaries()))));
        LBasicBlock doesNotHold = newColdBlock();
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(m_out.bitAnd(m_out.isZero32(mustSuspendByFlags), m_out.equal(queue, m_out.loadPtr(stateAt(globalObject, JSGlobalObject::offsetOfMicrotaskQueue())))), usually(holds), rarely(doesNotHold));
        m_out.appendTo(doesNotHold);
        AOT_TYPED_OPERATION(FactViolated, undefined(), word32(m_id.index()), word32(m_currentBlock->terminator.value.index()));
        m_out.jump(mustSuspend);
        m_out.appendTo(holds);
    }
    if (rep != Rep::Cell && rep != Rep::JSValue) {
        m_out.jump(mayContinue);
        return;
    }
    bool carriesValue = numberOfPredecessorsOf(goesOn) == 1;
    LBasicBlock notObject = carriesValue ? m_out.newBlock() : mayContinue;
    CellPointer promise = awaitedObjectOrElse(awaited, notObject);
    if (typeOf(awaited).singleEngineClass() != &m_builtins.promiseClass() || !isOnlyAwaited(awaited))
        require(equal(loadField<Word32>(promise, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value());
    LValue packed = m_out.load64(promise.value(), m_heaps.JSPromise_packed);
    static_assert(!static_cast<unsigned>(JSPromise::Status::Pending) && static_cast<unsigned>(JSPromise::Status::Fulfilled) == 1 && static_cast<unsigned>(JSPromise::Status::Rejected) == 2 && JSPromise::stateMask == 3);
    require(m_out.testNonZero64(packed, m_out.constInt64(static_cast<int64_t>(JSPromise::Status::Fulfilled) << flagsShift)));
    if (!carriesValue) {
        m_out.jump(mayContinue);
        return;
    }
    Join join(m_out, Rep::JSValue, Int64);
    didEmitWrite(Writes::RuntimeState);
    m_out.store64(m_out.bitOr(packed, m_out.constInt64(static_cast<int64_t>(JSPromise::isHandledFlag) << flagsShift)), promise.value(), m_heaps.JSPromise_packed);
    join.add(Boxed(m_out.load64(promise.value(), m_heaps.JSPromise_slot)));
    m_out.appendTo(notObject);
    join.add(in<Boxed>(awaited));
    m_inlineAwaitedValues.set(&m_function.at(goesOn), std::pair { awaited, join.finish().value });
    m_out.jump(mayContinue);
}

bool Lowering::isResumeClosureCreatedByStub(size_t index)
{
    if (!isResumeClosureCreatedAtSuspend(index))
        return false;
    auto& values = m_currentBlock->values;
    auto& resume = m_program.at(m_function.at(values[index]).function());
    auto& store = m_function.at(values[index + 1]);
    ValueID awaited = m_function.at(values[index + 2]).arguments[1];
    return FunctionTables::executableKind(static_cast<uint8_t>(resume.kind) | resume.declaredAs, false) == FunctionTables::executableKind(static_cast<uint8_t>(FunctionKind::Plain), false)
        && m_program.at(store.layout()).fields[store.field()].offset == ResumeClosureRequest::stubFrameWord * sizeof(uint64_t)
        && m_program.closureAllocation(1) <= ResumeClosureRequest::maxAllocationForStub
        && !isAllocatedByFirstUse(awaited) && !isLazilyFetched(awaited);
}

std::optional<Operand> Lowering::lowerInlinePromise(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    static constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    auto& type = typeOf(result);
    if (which == InlineBuiltin::AwaitBySavingFrame) {
        RELEASE_ASSERT(m_function.savesFrameOnSuspend && value.arguments.size() >= 2);
        m_context.emitsCalls = true;
        m_context.savesFrameOnSuspend = true;
        Boxed awaited = in<Boxed>(value.arguments[0]);
        Vector<LValue, 16> kept;
        kept.append(in<CellPointer>(value.arguments[1]).value());
        for (unsigned i = 2; i < value.arguments.size(); ++i) {
            Operand operand = get(value.arguments[i]);
            if (holdsCells(operand.rep))
                kept.append(operand.value);
        }
        PatchpointValue* patchpoint = m_out.patchpoint(Int64);
        applyEffects(patchpoint, { Writes::Heap, true });
        patchpoint->effects.writes = HeapRange::top();
        patchpoint->append(ConstrainedValue(awaited.value(), ValueRep::reg(GPRInfo::argumentGPR1)));
        for (LValue cell : kept)
            patchpoint->append(ConstrainedValue(cell, ValueRep::LateColdAny));
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
        patchpoint->clobberLate(m_proc.code().calleeSavesClobberedAtSuspensions());
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        CallSite site { nextSite() };
        patchpoint->setGenerator([context = &m_context, site, keptCount = kept.size()](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            Vector<uint32_t, 8> places { 0u };
            uint32_t promiseLocation = 0;
            for (unsigned i = 0; i < keptCount; ++i) {
                const ValueRep& rep = params[2 + i];
                uint32_t place = SavedFrame::locationInRegister;
                if (rep.isStack())
                    place = SavedFrame::locationInFrame(rep.offsetFromFP());
                else if (rep.isGPR()) {
                    if (rep.gpr() == GPRInfo::numberTagRegister || rep.gpr() == GPRInfo::notCellMaskRegister)
                        continue;
                    RELEASE_ASSERT(rep.gpr() == SavedFrame::preservedRegister);
                } else {
                    RELEASE_ASSERT(rep.isConstant() && i);
                    continue;
                }
                if (!i)
                    promiseLocation = place;
                while (places.size() <= 1 + place / 32)
                    places.append(0);
                places[1 + place / 32] |= 1u << (place % 32);
            }
            places[0] = places.size() - 1;
            uint32_t row = context->plans.size();
            for (uint32_t earlier : context->savedFramePlanOffsets) {
                if (equalSpans(context->plans.subspan(earlier, std::min<size_t>(places.size(), context->plans.size() - earlier)), places.span())) {
                    row = earlier;
                    break;
                }
            }
            if (row == context->plans.size()) {
                context->savedFramePlanOffsets.append(row);
                context->plans.appendVector(places);
            }
            RegisterAtOffsetList saves = params.code().calleeSaveRegisterAtOffsetList();
#if CPU(ARM64)
            RELEASE_ASSERT(!(saves.registerCount() % 2) && saves.registerCount() <= 6);
            for (size_t i = 0; i < saves.registerCount(); ++i) {
                size_t fromLast = saves.registerCount() - i;
                RELEASE_ASSERT(saves.at(i).reg().isGPR() && static_cast<size_t>(saves.at(i).reg().gpr()) + fromLast == static_cast<size_t>(ARM64Registers::x24) + 1 && saves.at(i).offset() == -static_cast<ptrdiff_t>(fromLast * sizeof(CPURegister)));
            }
            auto word = SavedFrame::siteWord(row, promiseLocation, saves.registerCount() / 2);
#else
            uint32_t savedRegisters = 0;
            RELEASE_ASSERT(isX86_64() || !saves.registerCount());
#if CPU(X86_64)
            for (size_t i = 0; i < saves.registerCount(); ++i) {
                RELEASE_ASSERT(saves.at(i).reg().isGPR() && saves.at(i).offset() == -static_cast<ptrdiff_t>((saves.registerCount() - i) * sizeof(CPURegister)));
                GPRReg reg = saves.at(i).reg().gpr();
                RELEASE_ASSERT(reg == SavedFrame::preservedRegister || reg == X86Registers::r15);
                savedRegisters |= reg == SavedFrame::preservedRegister ? 1 : 2;
            }
#endif
            auto word = SavedFrame::siteWord(row, promiseLocation, savedRegisters);
#endif
            RELEASE_ASSERT(word);
            if (*word >> 32)
                jit.move(CCallHelpers::TrustedImm64(*word), stubImmediateGPR);
            else
                jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(*word)), stubImmediateGPR);
            context->stubCalls.call(jit, Stub::AwaitBySavingFrame, site);
        });
        unwindToLandingPad(patchpoint, site);
        return ensure(Boxed(patchpoint), type);
    }
    if (which == InlineBuiltin::SuspendAsyncFunction && m_currentIndexInBlock >= 2 && isResumeClosureCreatedAtSuspend(m_currentIndexInBlock - 2)) {
        auto& made = m_function.at(value.arguments[0]);
        auto& resume = m_program.at(made.function());
        auto& store = m_function.at(m_currentBlock->values[m_currentIndexInBlock - 1]);
        auto& field = m_program.at(store.layout()).fields[store.field()];
        unsigned executable = FunctionTables::executableKind(static_cast<uint8_t>(resume.kind) | resume.declaredAs, false);
        uint32_t allocation = m_program.closureAllocation(1);
        if (isResumeClosureCreatedByStub(m_currentIndexInBlock - 2))
            callICStub(Stub::SuspendAtFirstAwait, Void, { { in<CellPointer>(made.arguments[0]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, std::pair { stubImmediateGPR, ResumeClosureRequest::encodeForStub(made.function().index(), allocation) }, builtinCallEffects(value));
        else
            AOT_TYPED_OPERATION_WITH_EFFECTS(SuspendAtFirstAwait, builtinCallEffects(value), in<CellPointer>(made.arguments[0]), in<Boxed>(value.arguments[1]), integer(static_cast<int64_t>(ResumeClosureRequest::encode(made.function().index(), executable, allocation, field.offset / sizeof(uint64_t)))));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::SuspendAsyncFunction) {
        CellPointer closure = in<CellPointer>(value.arguments[0]);
        Boxed awaited = in<Boxed>(value.arguments[1]);
        if (m_environmentAwaitingBarrier && m_environmentAwaitingBarrier == m_function.at(value.arguments[0]).child(0)) {
            m_environmentAwaitingBarrier = ValueID();
            AOT_TYPED_OPERATION_WITH_EFFECTS(ParkAsyncFunction, builtinCallEffects(value), closure, awaited);
        } else
            AOT_TYPED_OPERATION_WITH_EFFECTS(SuspendAsyncFunction, builtinCallEffects(value), closure, awaited);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::AsyncGeneratorYield) {
        callICStub(Stub::AsyncGeneratorYield, Void, { { in<CellPointer>(value.arguments[0]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::GetIteratorNext)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetIteratorNext, builtinCallEffects(value), in<Boxed>(value.arguments[0])), type);
    if (which == InlineBuiltin::TryNewPromiseSubclass)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(TryNewPromiseSubclass, builtinCallEffects(value), in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2])), type);
    if (which == InlineBuiltin::GeneratorResume)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GeneratorResume, builtinCallEffects(value), asCell(in<Boxed>(value.arguments[0])), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2])), type);
    if (which == InlineBuiltin::GeneratorIsDone) {
        auto at = [&](LValue base, ptrdiff_t offset) {
            return TypedPointer(m_heaps.root, m_out.add(base, m_out.constIntPtr(offset)));
        };
        LValue generator = asCell(in<Boxed>(value.arguments[0])).value();
        LValue frame = m_out.loadPtr(at(m_out.loadPtr(at(generator, Generator::offsetOfResumeClosure())), Closure::offsetOfCaptures()));
        LValue state = m_out.load32(TypedPointer(m_heaps.root, m_out.add(frame, m_out.zeroExtPtr(m_out.load32(at(generator, Generator::offsetOfOffsetOfState()))))));
        return booleanResult(Word32(m_out.equal(state, m_out.constInt32(CoroutineFrame::completedState))), type);
    }
    if (which == InlineBuiltin::GetAsyncIteratorNext)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetAsyncIteratorNext, builtinCallEffects(value), in<Boxed>(value.arguments[0])), type);
    if (which == InlineBuiltin::AsyncIteratorNext)
        return ensure(Boxed(callICStub(Stub::AsyncIteratorNext, Int64, { { in<Boxed>(value.arguments[0]).value(), GPRInfo::argumentGPR1 }, { value.arguments.size() > 1 ? in<Boxed>(value.arguments[1]).value() : m_out.int64Zero, GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value))), type);
    if (which == InlineBuiltin::ClearAsyncIteratorResult) {
        Boxed next = in<Boxed>(value.arguments[0]);
        Boxed iteratorResult = in<Boxed>(value.arguments[1]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isDriven = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(next).value(), unsure(isCellCase), unsure(continuation));
        m_out.appendTo(isCellCase);
        m_out.branch(equal(loadCellType(asCell(next)), word32(SentinelType)).value(), unsure(isDriven), unsure(continuation));
        m_out.appendTo(isDriven);
        didEmitWrite(Writes::Heap);
        m_out.store64(m_out.constInt64(JSValue::encode(jsUndefined())), m_out.address(m_heaps.properties.atAnyNumber(), asCell(iteratorResult).value(), JSObject::offsetOfInlineStorage() + iteratorResultObjectValuePropertyOffset * sizeof(EncodedJSValue)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::IsAsyncIteratorDriven) {
        Boxed next = in<Boxed>(value.arguments[0]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock notCell = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(isCell(next).value(), unsure(isCellCase), unsure(notCell));
        m_out.appendTo(isCellCase);
        join.add(equal(loadCellType(asCell(next)), word32(SentinelType)));
        m_out.appendTo(notCell);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    auto argumentOrUndefined = [&](unsigned index) {
        return index < value.arguments.size() ? in<Boxed>(value.arguments[index]) : undefined();
    };
    Type promiseType = Type::engineClass(m_builtins.promiseClass());
    if (which == InlineBuiltin::PromiseThen) {
        CellPointer promise = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        Boxed onFulfilled = argumentOrUndefined(0);
        Boxed onRejected = argumentOrUndefined(1);
        CellPointer derived = lowerNewPromise();
        AOT_TYPED_OPERATION(PromiseThen, promise, onFulfilled, onRejected, derived);
        return ensure(derived, promiseType, type);
    }
    if (which == InlineBuiltin::PromiseCatch) {
        CellPointer promise = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        return ensure(AOT_TYPED_OPERATION(PromiseCatch, promise, argumentOrUndefined(0)), promiseType, type);
    }
    if (which == InlineBuiltin::PromiseResolve) {
        Boxed resolution = argumentOrUndefined(0);
        bool mayBeObject = !value.arguments.isEmpty() && typeOf(value.arguments[0]).hasObjects();
        LBasicBlock notObject = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        if (mayBeObject) {
            LBasicBlock isCellCase = m_out.newBlock();
            LBasicBlock isObject = m_out.newBlock();
            LBasicBlock otherwise = newBlockForGenericCall(value);
            m_out.branch(isCell(resolution).value(), unsure(isCellCase), unsure(notObject));
            m_out.appendTo(isCellCase);
            CellPointer cell = asCell(resolution);
            m_out.branch(below(loadCellType(cell), word32(ObjectType)).value(), unsure(notObject), unsure(isObject));
            m_out.appendTo(isObject);
            if (isNotThenableWithoutExtraProperties(typeOf(value.arguments[0])))
                m_out.branch(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(notObject), rarely(otherwise));
            else {
                LBasicBlock isOriginalPromise = m_out.newBlock();
                m_out.branch(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value(), unsure(isOriginalPromise), rarely(otherwise));
                m_out.appendTo(isOriginalPromise);
                join.add(cell);
            }
            if (!isSharedGenericPath(otherwise)) {
                m_out.appendTo(otherwise);
                join.add(asCell(callBuiltinFunction(value, builtinCallEffects(value))));
            }
        } else
            m_out.jump(notObject);
        m_out.appendTo(notObject);
        CellPointer promise = lowerNewPromise();
        didEmitWrite(Writes::RuntimeState);
        storeField(promise, m_heaps.JSPromise_slot, resolution);
        storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << flagsShift));
        join.add(promise);
        return ensure(join.finish().as<CellPointer>(), promiseType, type);
    }
    ValueID awaited = value.arguments[0];
    Operand operand = get(awaited);
    bool mayBeCell = operand.rep == Rep::Cell || operand.rep == Rep::JSValue;
    auto objectOrElse = [&](LBasicBlock notObject) {
        return awaitedObjectOrElse(awaited, notObject);
    };
    switch (which) {
    case InlineBuiltin::AwaitMayContinue: {
        if (m_isFusedWithBranch.get(result.index()))
            return booleanResult(boolean(false), type);
        LBasicBlock mayContinue = m_out.newBlock();
        LBasicBlock mustSuspend = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        auto require = [&](LValue condition) {
            LBasicBlock next = m_out.newBlock();
            m_out.branch(condition, unsure(next), unsure(mustSuspend));
            m_out.appendTo(next);
        };
        auto stateAt = [&](LValue base, ptrdiff_t offset) {
            return m_out.address(m_heaps.AOTTyped_runtimeState, base, offset);
        };
        require(m_out.testNonZero64(in<Integer>(value.arguments[1]).value(), m_out.constInt64(CoroutineFrame::isEnteredByJobLoop)));
        LValue queue = m_out.loadPtr(stateAt(m_instance, Instance::offsetOfMicrotaskQueue()));
        require(m_out.equal(m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfStart())), m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfEnd()))));
        require(m_out.isZero32(m_out.load8ZeroExt32(stateAt(m_instance, Instance::offsetOfAwaitMustSuspend()))));
        if (mayBeCell) {
            CellPointer object = objectOrElse(mayContinue);
            if (typeOf(awaited).singleEngineClass() != &m_builtins.promiseClass() || !isOnlyAwaited(awaited))
                require(equal(loadField<Word32>(object, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value());
            LValue flags = m_out.lShr(m_out.load64(object.value(), m_heaps.JSPromise_packed), m_out.constInt32(flagsShift));
            require(m_out.equal(m_out.bitAnd(flags, m_out.constInt64(JSPromise::stateMask)), m_out.constInt64(static_cast<int64_t>(JSPromise::Status::Fulfilled))));
        }
        m_out.jump(mayContinue);
        m_out.appendTo(mayContinue);
        join.add(boolean(true));
        m_out.appendTo(mustSuspend);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::AwaitedValue: {
        if (!mayBeCell)
            return valueResult(awaited, type);
        if (auto brought = m_inlineAwaitedValues.take(m_currentBlock); brought.second && brought.first == awaited)
            return ensure(Boxed(brought.second), type);
        LBasicBlock notObject = m_out.newBlock();
        Join join(m_out, Rep::JSValue, Int64);
        CellPointer promise = objectOrElse(notObject);
        didEmitWrite(Writes::RuntimeState);
        m_out.store64(m_out.bitOr(m_out.load64(promise.value(), m_heaps.JSPromise_packed), m_out.constInt64(static_cast<int64_t>(JSPromise::isHandledFlag) << flagsShift)), promise.value(), m_heaps.JSPromise_packed);
        join.add(Boxed(m_out.load64(promise.value(), m_heaps.JSPromise_slot)));
        m_out.appendTo(notObject);
        join.add(in<Boxed>(awaited));
        return ensure(join.finish().as<Boxed>(), type);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
