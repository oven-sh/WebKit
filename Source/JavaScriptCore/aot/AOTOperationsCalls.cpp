/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsObjects.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "FunctionExecutableInlines.h"
#include "Interpreter.h"
#include "InterpreterInlines.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSArrayIteratorInlines.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncGenerator.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSMicrotask.h"
#include "JSSentinel.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "JSStringIteratorInlines.h"
#include "LLIntSlowPaths.h"
#include "ScriptExecutableInlines.h"
#include "StaticHeap.h"

namespace JSC { namespace AOT {

// ---- for-of

// What op_iterator_open does before it resorts to calling iterable[Symbol.iterator]: an iterable of a kind the engine knows how
// to walk, whose protocol nobody has touched, gets an iterator and a marker for a next method, which is how op_iterator_next
// knows. Every kind is welcome at every site: what the other tiers ration is the code they emit per kind, and here it is one call.
JSC_DEFINE_JIT_OPERATION(operationAOTIteratorOpenTryFast, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedIterable, EncodedJSValue encodedSymbolIterator, EncodedJSValue* next))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue iterable = JSValue::decode(encodedIterable);
    JSValue symbolIterator = JSValue::decode(encodedSymbolIterator);
    auto finish = [&](JSValue iterator, JSValue nextValue) {
        *next = JSValue::encode(nextValue);
        return JSValue::encode(iterator);
    };

    switch (getIterationMode(vm, globalObject, iterable, symbolIterator)) {
    case IterationMode::FastArray:
        if (Options::useUnboxedFastArrayIteration()) {
            // No iterator object: the array stays in the iterable operand, and the index of the next element is the "next method".
            OPERATION_RETURN(scope, finish(vm.fastArrayUnboxedSentinel(), jsNumber(0)));
        }
        OPERATION_RETURN(scope, finish(JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), asObject(iterable), IterationKind::Values), vm.fastArrayValuesSentinel()));
    case IterationMode::FastArrayValues:
        OPERATION_RETURN(scope, finish(iterable, vm.fastArrayValuesSentinel()));
    case IterationMode::FastArrayKeys:
        OPERATION_RETURN(scope, finish(iterable, vm.fastArrayKeysSentinel()));
    case IterationMode::FastArrayEntries:
        OPERATION_RETURN(scope, finish(iterable, vm.fastArrayEntriesSentinel()));
    case IterationMode::FastMap:
        OPERATION_RETURN(scope, finish(JSMapIterator::create(vm, globalObject->mapIteratorStructure(), uncheckedDowncast<JSMap>(iterable), IterationKind::Entries), vm.fastMapEntriesSentinel()));
    case IterationMode::FastMapKeys:
        OPERATION_RETURN(scope, finish(iterable, vm.fastMapKeysSentinel()));
    case IterationMode::FastMapValues:
        OPERATION_RETURN(scope, finish(iterable, vm.fastMapValuesSentinel()));
    case IterationMode::FastMapEntries:
        OPERATION_RETURN(scope, finish(iterable, vm.fastMapEntriesSentinel()));
    case IterationMode::FastSet:
        OPERATION_RETURN(scope, finish(JSSetIterator::create(vm, globalObject->setIteratorStructure(), uncheckedDowncast<JSSet>(iterable), IterationKind::Values), vm.fastSetValuesSentinel()));
    case IterationMode::FastSetValues:
        OPERATION_RETURN(scope, finish(iterable, vm.fastSetValuesSentinel()));
    case IterationMode::FastSetEntries:
        OPERATION_RETURN(scope, finish(iterable, vm.fastSetEntriesSentinel()));
    case IterationMode::FastString:
        OPERATION_RETURN(scope, finish(JSStringIterator::create(vm, globalObject->stringIteratorStructure(), asString(iterable)), vm.fastStringValuesSentinel()));
    case IterationMode::FastAsyncGenerator:
    case IterationMode::AsyncFromSync:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    case IterationMode::Generic:
        break;
    }

    auto validationResult = validateIterable(vm, iterable, symbolIterator);
    if (validationResult != IterableValidationResult::Valid) [[unlikely]]
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validationResult, iterable));
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTAsyncIteratorOpenTryFast, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedIterable, EncodedJSValue encodedSymbolIterator, EncodedJSValue* next))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue iterable = JSValue::decode(encodedIterable);
    JSValue symbolIterator = JSValue::decode(encodedSymbolIterator);
    bool promiseSpeciesIsPrimordial = globalObject->promiseSpeciesWatchpointSet().state() == IsWatched;

    // No @@asyncIterator: the sync iterator, wrapped. The marker fuses the consumer's await, which is only unobservable while
    // the species is primordial.
    if (symbolIterator.isUndefinedOrNull()) {
        JSAsyncFromSyncIterator* wrapper = createAsyncFromSyncIteratorForIterable(globalObject, iterable);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        *next = JSValue::encode(promiseSpeciesIsPrimordial ? JSValue(vm.fastAsyncGeneratorSentinel()) : JSValue(globalObject->asyncFromSyncIteratorPrototypeNextFunction()));
        OPERATION_RETURN(scope, JSValue::encode(wrapper));
    }

    // A genuine async generator whose @@asyncIterator and next are the primordial ones is driven directly.
    if (promiseSpeciesIsPrimordial && iterable.isCell() && iterable.asCell()->type() == JSAsyncGeneratorType
        && symbolIterator == globalObject->linkTimeConstant(LinkTimeConstant::asyncIteratorPrototypeSymbolAsyncIterator)) {
        JSObject* iterableObject = asObject(iterable);
        PropertySlot slot(iterableObject, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool found = iterableObject->getPropertySlot(globalObject, vm.propertyNames->next, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (found && slot.isValue() && slot.getValue(globalObject, vm.propertyNames->next) == globalObject->linkTimeConstant(LinkTimeConstant::asyncGeneratorPrototypeNext)) {
            *next = JSValue::encode(vm.fastAsyncGeneratorSentinel());
            OPERATION_RETURN(scope, encodedIterable);
        }
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

// For an iterator that came with a marker. The value, or empty when the iteration is done.
JSC_DEFINE_JIT_OPERATION(operationAOTIteratorNextTryFast, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* iterator))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (auto* arrayIterator = dynamicDowncast<JSArrayIterator>(iterator)) {
        JSValue value;
        bool hasNext = arrayIterator->next(globalObject, value);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, hasNext ? JSValue::encode(value) : encodedJSValue());
    }

    if (auto* mapIterator = dynamicDowncast<JSMapIterator>(iterator)) {
        auto result = mapIterator->nextWithAdvance(vm);
        if (result.key.isEmpty())
            OPERATION_RETURN(scope, encodedJSValue());
        switch (mapIterator->kind()) {
        case IterationKind::Keys:
            OPERATION_RETURN(scope, JSValue::encode(result.key));
        case IterationKind::Values:
            OPERATION_RETURN(scope, JSValue::encode(result.value));
        case IterationKind::Entries:
            OPERATION_RETURN(scope, JSValue::encode(constructArrayPair(globalObject, result.key, result.value)));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (auto* setIterator = dynamicDowncast<JSSetIterator>(iterator)) {
        JSValue key = setIterator->nextWithAdvance(vm);
        if (key.isEmpty())
            OPERATION_RETURN(scope, encodedJSValue());
        if (setIterator->kind() == IterationKind::Entries)
            OPERATION_RETURN(scope, JSValue::encode(constructArrayPair(globalObject, key, key)));
        OPERATION_RETURN(scope, JSValue::encode(key));
    }

    auto* stringIterator = uncheckedDowncast<JSStringIterator>(iterator);
    RELEASE_ASSERT(iterator->inherits<JSStringIterator>());
    JSString* value = stringIterator->nextWithAdvance(globalObject, vm);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, value ? JSValue::encode(value) : encodedJSValue());
}

// For an array that has no iterator object. index: in and out.
JSC_DEFINE_JIT_OPERATION(operationAOTIteratorNextWithIndex, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue iterable, EncodedJSValue* encodedIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue index = JSValue::decode(*encodedIndex);
    JSValue value;
    bool hasNext = JSArrayIterator::nextValueWithIndexInFrame(globalObject, JSValue::decode(iterable), index, value);
    *encodedIndex = JSValue::encode(index);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, hasNext ? JSValue::encode(value) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTAsyncIteratorNextWithDriver, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* iterator, JSObject* driver, EncodedJSValue resumeValue))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(asyncIteratorNextWithDriver(globalObject, iterator, driver, JSValue::decode(resumeValue), &vm.syncResumeCallCache())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMaterializeArrayIterator, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue iterable, EncodedJSValue index))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, static_cast<JSObject*>(materializeUnboxedFastArrayIterator(globalObject, JSValue::decode(iterable), JSValue::decode(index))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIteratorResultIsNotObject, void, (JSGlobalObject* globalObject))
{
    AOT_OPERATION_BEGIN(globalObject);
    throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
    OPERATION_RETURN(scope);
}

// ---- Calls

// The number of arguments, having made sure that there is stack for a frame that holds them below the caller's own
// numUsedStackSlots.
JSC_DEFINE_JIT_OPERATION(operationAOTSizeFrameForVarargs, size_t, (JSGlobalObject* globalObject, EncodedJSValue arguments, uint32_t numUsedStackSlots, uint32_t firstVarArgOffset))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, sizeFrameForVarargs(globalObject, callFrame, vm, JSValue::decode(arguments), numUsedStackSlots, firstVarArgOffset));
}

// Called with the stack pointer below newCallFrame.
JSC_DEFINE_JIT_OPERATION(operationAOTSetupVarargsFrame, CallFrame*, (JSGlobalObject* globalObject, CallFrame* newCallFrame, EncodedJSValue arguments, uint32_t firstVarArgOffset, uint32_t length))
{
    AOT_OPERATION_BEGIN(globalObject);
    setupVarargsFrame(globalObject, callFrame, newCallFrame, JSValue::decode(arguments), firstVarArgOffset, length);
    OPERATION_RETURN(scope, newCallFrame);
}

// A caller gives up its frame before it jumps to the callee, and from then on nobody can say on whose behalf code is being
// compiled or an error thrown. So whatever a call may need doing is done first: true means that the callee is a function that
// has code to jump to. False means that this had better be an ordinary call.
JSC_DEFINE_JIT_OPERATION(operationAOTPrepareTailCall, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedCallee))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue callee = JSValue::decode(encodedCallee);
    JSFunction* function = callee.isCell() ? dynamicDowncast<JSFunction>(callee.asCell()) : nullptr;
    if (!function)
        OPERATION_RETURN(scope, false);
    ExecutableBase* executable = function->executable();
    DeferTraps deferTraps(vm); // Nothing gets to throw away the code that is about to run.
    if (!executable->isHostFunction()) {
        CodeBlock* codeBlock = nullptr;
        uncheckedDowncast<FunctionExecutable>(executable)->prepareForExecution<FunctionExecutable>(vm, function, function->scopeUnchecked(), CodeSpecializationKind::CodeForCall, codeBlock);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
    }
    // Asking is what puts the answer where the thunk looks for it.
    executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
    OPERATION_RETURN(scope, true);
}

// A call that was compiled for one function in particular (which of the caller's known callees) is being made. If this callee is a
// closure of that function, and runs the code that the call goes straight to, then from now on this callee gets called that way.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkCall, void, (JSGlobalObject* globalObject, EncodedJSValue encodedCallee, uint32_t knownCallee, Slot* cache, uint32_t isConstruct))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    // (Not for a function that has nowhere to keep it.)
    if (SharedData::contains(cache))
        return;
    countAttemptToLinkCall(cache);

    Data* caller = callerData(callFrame);
    JITCode* code = caller->code;
    JSValue callee = JSValue::decode(encodedCallee);
    JSFunction* function = callee.isCell() ? dynamicDowncast<JSFunction>(callee.asCell()) : nullptr;
    if (!code->isFromImage() || !function || function->isHostFunction())
        return;
    CodeSpecializationKind kind = isConstruct ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
    FunctionExecutable* executable = function->jsExecutable();
    const ImageFunction* functionOfCallee;
    uint32_t indexOfCallee;
    if (isConstruct && executable->constructsByCalling())
        return;
    if (executable->aotEntryFor(kind)) {
        // The call is going to go straight to the code, which takes it that it has been run before.
        DeferGCForAWhile deferGC(vm);
        if (caller->instance != vm.m_aotInstanceOfProgram || !linkStaticFunction(vm, executable, kind, function->scope()))
            return;
        indexOfCallee = executable->aotIndexFor(kind);
        functionOfCallee = caller->instance->infos[indexOfCallee].function();
    } else {
        if (!executable->hasJITCodeFor(kind) || executable->generatedJITCodeFor(kind)->jitType() != JITType::AOTJIT)
            return;
        auto* codeOfCallee = static_cast<JITCode*>(executable->generatedJITCodeFor(kind).ptr());
        if (codeOfCallee->instance() != caller->instance || !codeOfCallee->isFromImage())
            return;
        indexOfCallee = codeOfCallee->header().index;
        functionOfCallee = codeOfCallee->imageFunction();
    }
    // (The number is one of the caller's image.)
    if (code->imageFunction()->knownCallees()[knownCallee] != indexOfCallee || &Image::of(*functionOfCallee) != &Image::of(*code->imageFunction()))
        return;
    fillCallCache(vm, caller, cache, function);
}

// llint_virtual_call(), which begins by asking for the CodeBlock of the caller, in case there is an error to report. Nearly always
// what there is to do is get a function that has not been called before its code.
extern "C" UGPRPair SYSV_ABI findCallTarget(CallFrame* calleeFrame, CallLinkInfo* callLinkInfo)
{
    JSValue callee = calleeFrame->guaranteedJSValueCallee();
    if (!callee.isCell())
        return LLInt::llint_virtual_call(calleeFrame, callLinkInfo);
    CodeSpecializationKind kind = callLinkInfo->specializationKind();
    VM& vm = callee.asCell()->vm();
    if (callee.inherits<InternalFunction>())
        return encodeResult(vm.getCTIInternalFunctionTrampolineFor(kind).taggedPtr(), nullptr);
    auto* function = dynamicDowncast<JSFunction>(callee.asCell());
    if (!function)
        return LLInt::llint_virtual_call(calleeFrame, callLinkInfo);
    // (It has its code from the start, and reports its own errors.)
    if (function->isHostFunction())
        return encodeResult(function->executable()->entrypointFor(kind, ArityCheckMode::MustCheckArity).taggedPtr(), nullptr);
    FunctionExecutable* executable = function->jsExecutable();
    if (!isCall(kind) && executable->constructAbility() == ConstructAbility::CannotConstruct)
        return LLInt::llint_virtual_call(calleeFrame, callLinkInfo);

    NativeCallFrameTracer tracer(vm, calleeFrame);
    sanitizeStackForVM(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    DeferTraps deferTraps(vm); // Nothing gets to throw away the code that is about to run.
    calleeFrame->setCodeBlock(nullptr);
    if (executable->aotEntryFor(kind)) {
        DeferGCForAWhile deferGC(vm);
        // (The executable is every VM's, and says what it says. It is the function that gets another.)
        if (!linkStaticFunction(vm, executable, kind, function->scopeUnchecked())) [[unlikely]] {
            executable = StaticHeap::standInFor(vm, executable);
            function->replaceExecutable(vm, executable);
        }
    }
    executable->prepareForExecution<FunctionExecutable>(vm, function, function->scopeUnchecked(), kind, *calleeFrame->addressOfCodeBlock());
    if (scope.exception()) [[unlikely]]
        return encodeResult(nullptr, std::bit_cast<void*>(&vm));
    return encodeResult(executable->entrypointFor(kind, ArityCheckMode::MustCheckArity).taggedPtr(), nullptr);
}

// Stub::LinkFunction. Whoever made the call is code of a module that is linked as compiled, and so then are the modules it imports
// from, and theirs (JSModuleRecord::isLinkedAsInImage()): there is nothing that could be in the way.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkFunction, void*, (CallFrame* calleeFrame, uint32_t index, uint32_t distanceOfEnvironment))
{
    auto* instance = std::bit_cast<Instance*>(calleeFrame->unsafeCodeBlock());
    VM& vm = *instance->vm;
    NativeCallFrameTracer tracer(vm, calleeFrame->callerFrame());
    DeferGCForAWhile deferGC(vm);
    DeferTraps deferTraps(vm);
    auto [executable, kind] = StaticHeap::executableOfFunction(index);
    auto* scope = std::bit_cast<JSScope*>(std::bit_cast<uint8_t*>(instance) - distanceOfEnvironment);
    RELEASE_ASSERT(executable->aotIndexFor(kind) == index);
    RELEASE_ASSERT(linkStaticFunction(vm, executable, kind, scope));
    const ImageFunction& function = *instance->infos[index].function();
    return tagCodePtr<JSEntryPtrTag>(const_cast<uint8_t*>(Image::of(function).codeFor(function)) + function.directEntryOffset);
}

// For a stub that is about to fill a slot: didFillSlot(), but for the epoch.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNoteFilled, void, (Data* data))
{
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}

// For a stub that has seen slots fail a function that has none of its own once too often.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGiveData, void, (Instance* instance, uint32_t index))
{
    instance->ensureData(index);
}

// The frame is that of a function that has found no room for it, and is not yet one that anybody could make sense of.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance, uint32_t index))
{
    Data* data = instance->ensureData(index);
    VM& vm = *data->instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    callFrame->convertToZombieFrame(vm, data->ensureCodeBlock());
    throwStackOverflowError(data->instance->globalObject, scope);
}

// calleeFrame: what a call to the callee would be made with, complete but for the return address. Empty if the callee is not
// eval after all.
JSC_DEFINE_JIT_OPERATION(operationAOTCallDirectEval, EncodedJSValue, (CallFrame* calleeFrame, JSScope* callerScopeChain, EncodedJSValue thisValue, uint32_t bytecodeIndexBits, uint32_t lexicallyScopedFeatures))
{
    CallFrame* callFrame = calleeFrame->callerFrame();
    VM& vm = callFrame->deprecatedVM();
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    calleeFrame->setCodeBlock(nullptr);
    OPERATION_RETURN(scope, JSValue::encode(eval(calleeFrame, JSValue::decode(thisValue), callerScopeChain, caller(callFrame).ensureData()->ensureCodeBlock(), BytecodeIndex::fromBits(bytecodeIndexBits), static_cast<LexicallyScopedFeatures>(lexicallyScopedFeatures))));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
