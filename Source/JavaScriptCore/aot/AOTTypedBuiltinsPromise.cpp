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
#include "JSAsyncFromSyncIterator.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "JSSentinel.h"
#include "MicrotaskQueueInlines.h"

namespace JSC { namespace AOT {

// The family aot/builtins/promise.def.

// As JSPromise::resolveWithInternalMicrotaskForAsyncAwait(), which looks nothing up for the first two either.
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
    vm.writeBarrier(uncheckedDowncast<Typed::Closure>(resumeClosure)->environment());
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, resumeClosure);
    OPERATION_RETURN(scope);
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAsyncGeneratorYield, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtYield();
    resumeWhenSettled(globalObject, vm, JSValue::decode(operand), InternalMicrotask::AOTAsyncGeneratorResume, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetIteratorNext, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterator = JSValue::decode(encodedIterator);
    if (auto* generator = dynamicDowncast<Typed::Generator>(iterator); generator && generator->iteratesAsMade(globalObject))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    OPERATION_RETURN(scope, JSValue::encode(iterator.get(globalObject, vm.propertyNames->next)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGeneratorResume, EncodedJSValue, (Instance* instance, JSCell* generator, EncodedJSValue mode, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* inner = uncheckedDowncast<Typed::Generator>(generator);
    auto resumeMode = static_cast<Typed::Generator::ResumeMode>(JSValue::decode(mode).asInt32());
    if (resumeMode != Typed::Generator::ResumeMode::Next && !inner->iteratesAsMade(globalObject))
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    bool isDone;
    OPERATION_RETURN(scope, JSValue::encode(inner->resume(globalObject, resumeMode, JSValue::decode(value), isDone)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedGeneratorIsDone, size_t, (Instance*, JSCell* generator))
{
    return uncheckedDowncast<Typed::Generator>(generator)->isCompleted();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetAsyncIteratorNext, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterator = JSValue::decode(encodedIterator);
    if (iterator.isCell() && iterator.asCell()->type() == JSAsyncFromSyncIteratorType)
        OPERATION_RETURN(scope, JSValue::encode(vm.fastAsyncGeneratorSentinel()));
    if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(iterator); generator && generator->iteratesAsMade(globalObject))
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
    if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(iterator))
        OPERATION_RETURN(scope, JSValue::encode(generator->enqueueDriver(globalObject, sent ? sent : jsUndefined())));
    auto* wrapper = uncheckedDowncast<JSAsyncFromSyncIterator>(iterator);
    if (!sent || wrapper->iterationMode() != IterationMode::Generic)
        OPERATION_RETURN(scope, encodedIterator);
    OPERATION_RETURN(scope, JSValue::encode(asyncFromSyncIteratorNext(globalObject, wrapper, sent)));
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlinePromise(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    // A promise that is not as the realm makes them is asked for its `constructor`, any other object for `then`.
    case InlineBuiltin::PromiseResolve:
        return !arguments.empty() && arguments[0].hasObjects() ? Writes::Heap : Writes::RuntimeState;
    // The receiver finds the constructor and the species the realm started with.
    case InlineBuiltin::PromiseThen:
    case InlineBuiltin::PromiseCatch:
        return Writes::RuntimeState;
    case InlineBuiltin::AwaitMayContinue:
        return Writes::Nothing;
    case InlineBuiltin::AwaitedValue:
        return Writes::RuntimeState;
    // Reading `constructor` or `then` of what is awaited runs code.
    case InlineBuiltin::SuspendAsyncFunction:
    case InlineBuiltin::GetAsyncIteratorNext:
    case InlineBuiltin::AsyncIteratorNext:
    case InlineBuiltin::GetIteratorNext:
    case InlineBuiltin::GeneratorResume:
    case InlineBuiltin::AsyncGeneratorYield:
        return Writes::Heap;
    case InlineBuiltin::IsAsyncIteratorDriven:
    case InlineBuiltin::GeneratorIsDone:
        return Writes::Nothing;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::lacksThenUnlessMarked(const Type& type) const
{
    auto then = m_program.find("then"_s.span8());
    return then && !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.views.isEmpty() && std::ranges::all_of(type.views, [&](LayoutID view) {
        return isObjectLayout(m_program.at(view).kind) && onlyExtraPropertiesSupply(m_program, view, *then);
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
    if (!m_didFindWhatIsUsedOtherwiseThanAwaited) {
        m_didFindWhatIsUsedOtherwiseThanAwaited = true;
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
                        m_isUsedOtherwiseThanAwaited.set(child.index());
                }
                for (unsigned i = 0; i < user.arguments.size(); ++i) {
                    if (i != awaitedArgument)
                        m_isUsedOtherwiseThanAwaited.set(user.arguments[i].index());
                }
            }
            if (block.terminator.value)
                m_isUsedOtherwiseThanAwaited.set(block.terminator.value.index());
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    m_isUsedOtherwiseThanAwaited.set(argument.index());
            }
        }
    }
    return !m_isUsedOtherwiseThanAwaited.get(awaited.index());
}

CellPointer Lowering::objectAwaitedOrElse(ValueID awaited, LBasicBlock isNoObject)
{
    Operand operand = get(awaited);
    CellPointer cell;
    if (operand.rep == Rep::Cell)
        cell = operand.as<CellPointer>();
    else {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isCellCase = m_out.newBlock();
        m_out.branch(isCell(boxed, typeOf(awaited)).value(), unsure(isCellCase), unsure(isNoObject));
        m_out.appendTo(isCellCase);
        cell = asCell(boxed);
    }
    if (typeOf(awaited).primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt)) {
        LBasicBlock isObject = m_out.newBlock();
        m_out.branch(below(typeOfCell(cell), word32(ObjectType)).value(), unsure(isNoObject), unsure(isObject));
        m_out.appendTo(isObject);
    }
    return cell;
}

void Lowering::branchOnAwaitMayContinue(const Value& value, BlockID goesOn, LBasicBlock mayContinue, LBasicBlock mustSuspend)
{
    static constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    ValueID awaited = value.arguments[0];
    Rep rep = get(awaited).rep;
    if (!Options::validateAOTTypedFacts() && (rep == Rep::Cell || rep == Rep::JSValue) && numberOfPredecessorsOf(goesOn) == 1 && !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id)) {
        static_assert(static_cast<unsigned>(Stub::AwaitMayContinue) + 1 == static_cast<unsigned>(Stub::LeafAwaitMayContinue));
        LValue answer = callAllocationStub(Stub::AwaitMayContinue, { { in<Boxed>(awaited).value(), GPRInfo::argumentGPR1 }, { in<Integer>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, { Writes::RuntimeState, false }, false);
        m_valuesAwaitedInPlace.set(&m_function.at(goesOn), std::pair { awaited, answer });
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
        AOT_TYPED_OPERATION(FactDoesNotHold, undefined(), word32(m_id.index()), word32(m_currentBlock->terminator.value.index()));
        m_out.jump(mustSuspend);
        m_out.appendTo(holds);
    }
    if (rep != Rep::Cell && rep != Rep::JSValue) {
        m_out.jump(mayContinue);
        return;
    }
    bool bringsValue = numberOfPredecessorsOf(goesOn) == 1;
    LBasicBlock isNoObject = bringsValue ? m_out.newBlock() : mayContinue;
    CellPointer promise = objectAwaitedOrElse(awaited, isNoObject);
    if (typeOf(awaited).onlyEngineClass() != &m_builtins.promiseClass() || !isOnlyAwaited(awaited))
        require(equal(loadField<Word32>(promise, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value());
    LValue packed = m_out.load64(promise.value(), m_heaps.JSPromise_packed);
    static_assert(!static_cast<unsigned>(JSPromise::Status::Pending) && static_cast<unsigned>(JSPromise::Status::Fulfilled) == 1 && static_cast<unsigned>(JSPromise::Status::Rejected) == 2 && JSPromise::stateMask == 3);
    require(m_out.testNonZero64(packed, m_out.constInt64(static_cast<int64_t>(JSPromise::Status::Fulfilled) << flagsShift)));
    if (!bringsValue) {
        m_out.jump(mayContinue);
        return;
    }
    Join join(m_out, Rep::JSValue, Int64);
    didEmitWrite(Writes::RuntimeState);
    m_out.store64(m_out.bitOr(packed, m_out.constInt64(static_cast<int64_t>(JSPromise::isHandledFlag) << flagsShift)), promise.value(), m_heaps.JSPromise_packed);
    join.add(Boxed(m_out.load64(promise.value(), m_heaps.JSPromise_slot)));
    m_out.appendTo(isNoObject);
    join.add(in<Boxed>(awaited));
    m_valuesAwaitedInPlace.set(&m_function.at(goesOn), std::pair { awaited, join.finish().value });
    m_out.jump(mayContinue);
}

std::optional<Operand> Lowering::lowerInlinePromise(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    static constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    auto& type = typeOf(result);
    if (which == InlineBuiltin::SuspendAsyncFunction) {
        CellPointer closure = in<CellPointer>(value.arguments[0]);
        Boxed awaited = in<Boxed>(value.arguments[1]);
        if (m_environmentAwaitingBarrier && m_environmentAwaitingBarrier == m_function.at(value.arguments[0]).child(0)) {
            m_environmentAwaitingBarrier = ValueID();
            AOT_TYPED_OPERATION_WITH_EFFECTS(ParkAsyncFunction, effectsOfBuiltinCall(value), closure, awaited);
        } else
            AOT_TYPED_OPERATION_WITH_EFFECTS(SuspendAsyncFunction, effectsOfBuiltinCall(value), closure, awaited);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::AsyncGeneratorYield) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(AsyncGeneratorYield, effectsOfBuiltinCall(value), in<CellPointer>(value.arguments[0]), in<Boxed>(value.arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (which == InlineBuiltin::GetIteratorNext)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetIteratorNext, effectsOfBuiltinCall(value), in<Boxed>(value.arguments[0])), type);
    if (which == InlineBuiltin::GeneratorResume)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GeneratorResume, effectsOfBuiltinCall(value), asCell(in<Boxed>(value.arguments[0])), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2])), type);
    if (which == InlineBuiltin::GeneratorIsDone)
        return yieldBoolean(AOT_TYPED_OPERATION(GeneratorIsDone, asCell(in<Boxed>(value.arguments[0]))), type);
    if (which == InlineBuiltin::GetAsyncIteratorNext)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetAsyncIteratorNext, effectsOfBuiltinCall(value), in<Boxed>(value.arguments[0])), type);
    if (which == InlineBuiltin::AsyncIteratorNext)
        return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(AsyncIteratorNext, effectsOfBuiltinCall(value), in<Boxed>(value.arguments[0]), value.arguments.size() > 1 ? in<Boxed>(value.arguments[1]) : Boxed(m_out.int64Zero)), type);
    if (which == InlineBuiltin::IsAsyncIteratorDriven) {
        Boxed next = in<Boxed>(value.arguments[0]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isNoCell = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(isCell(next).value(), unsure(isCellCase), unsure(isNoCell));
        m_out.appendTo(isCellCase);
        join.add(equal(typeOfCell(asCell(next)), word32(SentinelType)));
        m_out.appendTo(isNoCell);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
    auto argumentOrUndefined = [&](unsigned index) {
        return index < value.arguments.size() ? in<Boxed>(value.arguments[index]) : undefined();
    };
    Type promiseType = Type::engineClass(m_builtins.promiseClass());
    if (which == InlineBuiltin::PromiseThen) {
        CellPointer promise = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        Boxed onFulfilled = argumentOrUndefined(0);
        Boxed onRejected = argumentOrUndefined(1);
        CellPointer derived = lowerNewPromise();
        AOT_TYPED_OPERATION(PromiseThen, promise, onFulfilled, onRejected, derived);
        return ensure(derived, promiseType, type);
    }
    if (which == InlineBuiltin::PromiseCatch) {
        CellPointer promise = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        return ensure(AOT_TYPED_OPERATION(PromiseCatch, promise, argumentOrUndefined(0)), promiseType, type);
    }
    if (which == InlineBuiltin::PromiseResolve) {
        Boxed resolution = argumentOrUndefined(0);
        bool mayBeObject = !value.arguments.isEmpty() && typeOf(value.arguments[0]).hasObjects();
        LBasicBlock isNoObject = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        if (mayBeObject) {
            LBasicBlock isCellCase = m_out.newBlock();
            LBasicBlock isObject = m_out.newBlock();
            LBasicBlock otherwise = newBlockForGeneralCall(value);
            m_out.branch(isCell(resolution).value(), unsure(isCellCase), unsure(isNoObject));
            m_out.appendTo(isCellCase);
            CellPointer cell = asCell(resolution);
            m_out.branch(below(typeOfCell(cell), word32(ObjectType)).value(), unsure(isNoObject), unsure(isObject));
            m_out.appendTo(isObject);
            if (lacksThenUnlessMarked(typeOf(value.arguments[0])))
                m_out.branch(lacksBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(isNoObject), rarely(otherwise));
            else {
                LBasicBlock isOriginalPromise = m_out.newBlock();
                m_out.branch(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value(), unsure(isOriginalPromise), rarely(otherwise));
                m_out.appendTo(isOriginalPromise);
                join.add(cell);
            }
            if (!isGeneralArmOnOffer(otherwise)) {
                m_out.appendTo(otherwise);
                join.add(asCell(callBuiltinFunction(value, effectsOfBuiltinCall(value))));
            }
        } else
            m_out.jump(isNoObject);
        m_out.appendTo(isNoObject);
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
    auto objectOrElse = [&](LBasicBlock isNoObject) {
        return objectAwaitedOrElse(awaited, isNoObject);
    };
    switch (which) {
    // `await` enqueues one job and returns to the job loop, which has nothing else to run and nothing to do in between: the job is next.
    // The operand is no object, or a fulfilled promise that finds what the realm started with, which is immutable.
    case InlineBuiltin::AwaitMayContinue: {
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
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
            if (typeOf(awaited).onlyEngineClass() != &m_builtins.promiseClass() || !isOnlyAwaited(awaited))
                require(equal(loadField<Word32>(object, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value());
            LValue flags = m_out.lShr(m_out.load64(object.value(), m_heaps.JSPromise_packed), m_out.constInt32(flagsShift));
            require(m_out.equal(m_out.bitAnd(flags, m_out.constInt64(JSPromise::stateMask)), m_out.constInt64(static_cast<int64_t>(JSPromise::Status::Fulfilled))));
        }
        m_out.jump(mayContinue);
        m_out.appendTo(mayContinue);
        join.add(boolean(true));
        m_out.appendTo(mustSuspend);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
    // Behind AwaitMayContinue.
    case InlineBuiltin::AwaitedValue: {
        if (!mayBeCell)
            return yieldValue(awaited, type);
        if (auto brought = m_valuesAwaitedInPlace.take(m_currentBlock); brought.second && brought.first == awaited)
            return ensure(Boxed(brought.second), type);
        LBasicBlock isNoObject = m_out.newBlock();
        Join join(m_out, Rep::JSValue, Int64);
        CellPointer promise = objectOrElse(isNoObject);
        didEmitWrite(Writes::RuntimeState);
        m_out.store64(m_out.bitOr(m_out.load64(promise.value(), m_heaps.JSPromise_packed), m_out.constInt64(static_cast<int64_t>(JSPromise::isHandledFlag) << flagsShift)), promise.value(), m_heaps.JSPromise_packed);
        join.add(Boxed(m_out.load64(promise.value(), m_heaps.JSPromise_slot)));
        m_out.appendTo(isNoObject);
        join.add(in<Boxed>(awaited));
        return ensure(join.finish().as<Boxed>(), type);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
