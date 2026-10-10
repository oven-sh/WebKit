/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AsyncContextSwapScope.h"
#include "JSCInlines.h"
#include "JSPromise.h"
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
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, awaited, task, resumeClosure);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSuspendAsyncFunction, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue awaited))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    resumeWhenSettled(globalObject, vm, JSValue::decode(awaited), InternalMicrotask::AOTAsyncFunctionResume, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPromiseThen, void, (Instance* instance, JSCell* promise, EncodedJSValue onFulfilled, EncodedJSValue onRejected, JSCell* derived))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSPromise>(promise)->performPromiseThen(vm, globalObject, JSValue::decode(onFulfilled), JSValue::decode(onRejected), derived);
    OPERATION_RETURN(scope);
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
        return Writes::RuntimeState;
    case InlineBuiltin::AwaitMayContinue:
        return Writes::Nothing;
    case InlineBuiltin::AwaitedValue:
        return Writes::RuntimeState;
    // Reading `constructor` or `then` of what is awaited runs code.
    case InlineBuiltin::SuspendAsyncFunction:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlinePromise(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    static constexpr unsigned flagsShift = CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer;
    auto& type = typeOf(result);
    if (which == InlineBuiltin::SuspendAsyncFunction) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(SuspendAsyncFunction, effectsOfBuiltinCall(value), in<CellPointer>(value.arguments[0]), in<Boxed>(value.arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
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
    if (which == InlineBuiltin::PromiseResolve) {
        Boxed resolution = argumentOrUndefined(0);
        bool mayBeObject = !value.arguments.isEmpty() && typeOf(value.arguments[0]).hasObjects();
        LBasicBlock isNoObject = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        if (mayBeObject) {
            LBasicBlock isCellCase = m_out.newBlock();
            LBasicBlock isObject = m_out.newBlock();
            LBasicBlock isOriginalPromise = m_out.newBlock();
            LBasicBlock otherwise = newColdBlock();
            m_out.branch(isCell(resolution).value(), unsure(isCellCase), unsure(isNoObject));
            m_out.appendTo(isCellCase);
            CellPointer cell = asCell(resolution);
            m_out.branch(below(typeOfCell(cell), word32(ObjectType)).value(), unsure(isNoObject), unsure(isObject));
            m_out.appendTo(isObject);
            m_out.branch(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(fixed32(Instance::offsetOfHeaderFor(Instance::InlineAllocation::Promise)))).value(), unsure(isOriginalPromise), rarely(otherwise));
            m_out.appendTo(isOriginalPromise);
            join.add(cell);
            m_out.appendTo(otherwise);
            join.add(asCell(callBuiltinFunction(value, effectsOfBuiltinCall(value))));
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
    bool mayBePrimitiveCell = typeOf(awaited).primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt);
    // Goes on where the operand is an object.
    auto objectOrElse = [&](LBasicBlock isNoObject) {
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
        if (mayBePrimitiveCell) {
            LBasicBlock isObject = m_out.newBlock();
            m_out.branch(below(typeOfCell(cell), word32(ObjectType)).value(), unsure(isNoObject), unsure(isObject));
            m_out.appendTo(isObject);
        }
        return cell;
    };
    switch (which) {
    // `await` enqueues one job and returns to the job loop, which has nothing else to run and nothing to do in between: the job is next.
    // The operand is no object, or a fulfilled promise that finds what the realm started with, which is immutable.
    case InlineBuiltin::AwaitMayContinue: {
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
        LValue globalObject = fixedPointer(Instance::offsetOfGlobalObject());
        LValue queue = m_out.loadPtr(stateAt(globalObject, JSGlobalObject::offsetOfMicrotaskQueue()));
        require(m_out.equal(m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfStart())), m_out.load64(stateAt(queue, MicrotaskQueue::offsetOfEnd()))));
        require(m_out.notZero32(m_out.load8ZeroExt32(stateAt(globalObject, JSGlobalObject::offsetOfCanFastQueueMicrotask()))));
        require(m_out.isZero32(m_out.load8ZeroExt32(stateAt(fixedPointer(Instance::offsetOfVM()), VM::offsetOfHostObservesMicrotaskBoundaries()))));
        if (mayBeCell) {
            CellPointer object = objectOrElse(mayContinue);
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
