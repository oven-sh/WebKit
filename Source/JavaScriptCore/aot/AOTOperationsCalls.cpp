/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsObjects.h"

#if ENABLE(FTL_JIT)

#include "AOTOperations.h"
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
#include "JSBoundFunctionInlines.h"
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

// What op_iterator_open does before it falls back to calling iterable[Symbol.iterator]. An iterable of a kind that the engine can
// iterate directly, and whose iteration protocol is unmodified, gets an iterator and a sentinel in place of its `next` method,
// which is how op_iterator_next recognizes it. Every kind is accepted at every site: the other tiers limit the kinds per site to
// limit the code they emit, and here it is a single call.
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

// For an iterator that came with a sentinel. Returns the value, or empty when the iteration is done.
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

// For an array that is iterated without an iterator object. index: read and updated.
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

// Whether iterating over the value yields exactly its elements, and the iteration is not observable.
static bool isIterationUnobservable(JSValue value)
{
    if (!value.isCell())
        return false;
    if (value.asCell()->type() == JSCellButterflyType)
        return true;
    auto* array = dynamicDowncast<JSArray>(value.asCell());
    return array && array->isIteratorProtocolFastAndNonObservable();
}

static unsigned copyableLength(JSValue value)
{
    if (auto* butterfly = dynamicDowncast<JSCellButterfly>(value.asCell()))
        return butterfly->length();
    return uncheckedDowncast<JSArray>(value.asCell())->length();
}

// For Stub::CallVarargs: first the number of arguments, then the arguments themselves. See ListDescriptor.
JSC_DEFINE_JIT_OPERATION(operationAOTSizeOfVarargs, size_t, (JSGlobalObject* globalObject, EncodedJSValue listOrItems, uint32_t descriptorBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(globalObject);
    ListDescriptor descriptor { descriptorBits };
    uint64_t length = 0;
    if (!descriptor.isOfItems()) {
        length = sizeOfVarargs(globalObject, JSValue::decode(listOrItems), descriptor.firstVarArg());
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    } else {
        auto* items = std::bit_cast<EncodedJSValue*>(listOrItems);
        bool allIterationsAreUnobservable = true;
        auto forEachSpread = [&](const auto& functor) {
            unsigned word = 0;
            for (unsigned i = 0; i < descriptor.numberOfItems(); ++i) {
                if (descriptor.kindOf(i) == ListDescriptor::Spread)
                    functor(items[word], i);
                word += descriptor.kindOf(i) == ListDescriptor::Passed ? 2 : 1;
            }
        };
        forEachSpread([&](EncodedJSValue& item, unsigned) {
            allIterationsAreUnobservable &= isIterationUnobservable(JSValue::decode(item));
        });
        if (!allIterationsAreUnobservable) [[unlikely]] {
            // Iterating runs code, which may modify the other items. So each item is iterated in turn, and the results are kept.
            bool threw = false;
            const void* returnAddress = removeCodePtrTag(callFrame->rawReturnPC());
            FunctionRef function = caller(globalObject, callFrame);
            uint32_t callSite = callSiteAt(*function.info().function(), classifyAddress(returnAddress).offset);
            forEachSpread([&](EncodedJSValue& item, unsigned index) {
                if (threw)
                    return;
                std::optional<CallSiteOverride> where;
                if (auto site = siteOfSpread(*function.info().function(), callSite, index))
                    where.emplace(*function.instance, returnAddress, *site);
                JSCell* result = spread(globalObject, JSValue::decode(item));
                if (scope.exception()) [[unlikely]] {
                    threw = true;
                    return;
                }
                item = JSValue::encode(result);
            });
            OPERATION_RETURN_IF_EXCEPTION(scope, 0);
        }
        unsigned word = 0;
        for (unsigned i = 0; i < descriptor.numberOfItems(); ++i) {
            switch (descriptor.kindOf(i)) {
            case ListDescriptor::Value:
                ++length;
                ++word;
                break;
            case ListDescriptor::Spread:
                length += copyableLength(JSValue::decode(items[word++]));
                break;
            case ListDescriptor::Passed:
                length += static_cast<uint64_t>(items[word]);
                word += 2;
                break;
            }
        }
    }
    if (length > maxArguments) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, static_cast<size_t>(length));
}

JSC_DEFINE_JIT_OPERATION(operationAOTLoadVarargs, void, (JSGlobalObject* globalObject, EncodedJSValue* where, EncodedJSValue listOrItems, uint32_t descriptorBits, uint32_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(globalObject);
    ListDescriptor descriptor { descriptorBits };
    if (!descriptor.isOfItems()) {
        loadVarargs(globalObject, std::bit_cast<JSValue*>(where), JSValue::decode(listOrItems), descriptor.firstVarArg(), length);
        OPERATION_RETURN(scope);
    }
    // (No code has run since the items were counted.)
    auto* items = std::bit_cast<EncodedJSValue*>(listOrItems);
    unsigned word = 0;
    for (unsigned i = 0; i < descriptor.numberOfItems(); ++i) {
        switch (descriptor.kindOf(i)) {
        case ListDescriptor::Value:
            *where++ = items[word++];
            break;
        case ListDescriptor::Spread: {
            JSCell* cell = JSValue::decode(items[word++]).asCell();
            if (auto* butterfly = dynamicDowncast<JSCellButterfly>(cell)) {
                for (unsigned index = 0; index < butterfly->length(); ++index)
                    *where++ = JSValue::encode(butterfly->get(index));
                break;
            }
            auto* array = uncheckedDowncast<JSArray>(cell);
            for (unsigned index = 0, count = array->length(); index < count; ++index) {
                JSValue element = array->tryGetIndexQuickly(index);
                *where++ = JSValue::encode(element ? element : jsUndefined());
            }
            break;
        }
        case ListDescriptor::Passed: {
            size_t count = static_cast<size_t>(items[word]);
            memcpy(where, std::bit_cast<const EncodedJSValue*>(items[word + 1]), count * sizeof(EncodedJSValue));
            where += count;
            word += 2;
            break;
        }
        }
    }
    OPERATION_RETURN(scope);
}

// A replacement for llint_virtual_call(), which starts by looking up the caller's CodeBlock in case there is an error to report. In
// nearly all cases, all that is needed is to link a function that has not been called before.
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
    // (A host function always has its code, and reports its own errors.)
    if (function->isHostFunction())
        return encodeResult(function->executable()->entrypointFor(kind, ArityCheckMode::MustCheckArity).taggedPtr(), nullptr);
    FunctionExecutable* executable = function->jsExecutable();
    if (!isCall(kind) && executable->constructAbility() == ConstructAbility::CannotConstruct)
        return LLInt::llint_virtual_call(calleeFrame, callLinkInfo);

    NativeCallFrameTracer tracer(vm, calleeFrame);
    sanitizeStackForVM(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    DeferTraps deferTraps(vm); // So that nothing can jettison the code that is about to run.
    calleeFrame->setCodeBlock(nullptr);
    if (executable->aotEntryFor(kind)) {
        DeferGCForAWhile deferGC(vm);
        // (The executable is shared by every VM and is read-only, so it is the function that is given a different executable.)
        if (!linkStaticFunction(vm, executable, kind, function->scopeUnchecked())) [[unlikely]] {
            // (That requires other code for the function, and a function in the short form has only its AOT code.)
            if (executable->isShortForm()) {
                throwSyntaxError(function->realm(), scope, makeString("The function "_s, executable->ecmaName().string(), " was compiled ahead of time, but its module is linked differently at run time, and the executable has no other code for it"_s));
                return encodeResult(nullptr, std::bit_cast<void*>(&vm));
            }
            executable = StaticHeap::standInFor(vm, executable);
            function->replaceExecutable(vm, executable);
        }
    }
    executable->prepareForExecution<FunctionExecutable>(vm, function, function->scopeUnchecked(), kind, *calleeFrame->addressOfCodeBlock());
    if (scope.exception()) [[unlikely]]
        return encodeResult(nullptr, std::bit_cast<void*>(&vm));
    return encodeResult(executable->entrypointFor(kind, ArityCheckMode::MustCheckArity).taggedPtr(), nullptr);
}

// For Stub::LinkFunction. The caller called the function directly, so it belongs to a module that is linked as it was compiled, and
// so do the modules that it imports from, transitively (JSModuleRecord::isLinkedAsInImage()). So linking cannot fail.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkFunction, void, (Instance* instance, void* addressInFunction))
{
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    DeferTraps deferTraps(vm);
    uint32_t index = FunctionRef::at(instance, addressInFunction).index;
    RELEASE_ASSERT(!instance->isLinked(index));
    auto [executable, kind] = StaticHeap::executableOfFunction(index);
    RELEASE_ASSERT(executable->aotIndexFor(kind) == index);
    const ImageFunction* function = instance->infos[index].function();
    Ref<JITCode> code = codeOfFunctionFromImage({ &Image::of(*function), function }, kind);
    code->setInstance(*instance);
    RELEASE_ASSERT(Data::create(*instance, executable, executable->unlinkedExecutable()->codeBlockIfExists(kind), code.get()));
}

// For a stub that is about to fill a slot. The same as didFillSlot(), except that it does not change the epoch.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNoteFilled, void, (Data* data))
{
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}

// For a stub that has seen too many inline cache misses in a function that has no Data of its own.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureData, void, (Instance* instance, uint32_t index))
{
    instance->ensureData(index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwStackOverflowError(instance->globalObject, scope);
}

// Returns empty if the callee is not eval.
JSC_DEFINE_JIT_OPERATION(operationAOTCallDirectEval, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue callee, uint32_t count, EncodedJSValue firstArgument, JSScope* callerScopeChain, EncodedJSValue thisValue, uint32_t bytecodeIndexBits, uint32_t lexicallyScopedFeatures))
{
    AOT_OPERATION_BEGIN(globalObject);
    // A stand-in for the parts of the callee frame that eval() reads.
    // eval() makes it the VM's top call frame, so it has to link to a valid caller frame: the one that this was called from.
    // (A Register is not initialized by its constructor.)
    uint64_t words[CallFrame::headerSizeInRegisters + 2] { };
    Register* frame = std::bit_cast<Register*>(&words[0]);
    CallFrame* calleeFrame = CallFrame::create(frame);
    frame[0] = callFrame;
    *std::bit_cast<void**>(&frame[1]) = __builtin_return_address(0);
    frame[static_cast<int>(CallFrameSlot::callee)] = JSValue::decode(callee);
    frame[static_cast<int>(CallFrameSlot::argumentCountIncludingThis)].lowWord() = std::min(count, 1u) + 1;
    frame[static_cast<int>(CallFrameSlot::thisArgument)] = JSValue::decode(thisValue);
    frame[static_cast<int>(CallFrameSlot::firstArgument)] = JSValue::decode(firstArgument);
    OPERATION_RETURN(scope, JSValue::encode(eval(calleeFrame, JSValue::decode(thisValue), callerScopeChain, caller(globalObject, callFrame).ensureData()->ensureCodeBlock(), BytecodeIndex::fromBits(bytecodeIndexBits), static_cast<LexicallyScopedFeatures>(lexicallyScopedFeatures))));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
