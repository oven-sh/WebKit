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

// What iterating over it comes to is what is in it, and nobody can tell whether it was iterated over.
static bool canBeCopiedFrom(JSValue value)
{
    if (!value.isCell())
        return false;
    if (value.asCell()->type() == JSCellButterflyType)
        return true;
    auto* array = dynamicDowncast<JSArray>(value.asCell());
    return array && array->isIteratorProtocolFastAndNonObservable();
}

static unsigned lengthOfWhatCanBeCopiedFrom(JSValue value)
{
    if (auto* butterfly = dynamicDowncast<JSCellButterfly>(value.asCell()))
        return butterfly->length();
    return uncheckedDowncast<JSArray>(value.asCell())->length();
}

// Stub::CallVarargs: how many there are, and then all of them, one after the other. See ListDescriptor.
JSC_DEFINE_JIT_OPERATION(operationAOTSizeOfVarargs, size_t, (JSGlobalObject* globalObject, EncodedJSValue listOrItems, uint32_t descriptorBits))
{
    AOT_OPERATION_BEGIN_FOR_NOBODY(globalObject);
    ListDescriptor descriptor { descriptorBits };
    uint64_t length = 0;
    if (!descriptor.isOfItems()) {
        length = sizeOfVarargs(globalObject, JSValue::decode(listOrItems), descriptor.firstVarArg());
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    } else {
        auto* items = std::bit_cast<EncodedJSValue*>(listOrItems);
        bool allCanBeCopiedFrom = true;
        auto forEachSpread = [&](const auto& functor) {
            unsigned word = 0;
            for (unsigned i = 0; i < descriptor.numberOfItems(); ++i) {
                if (descriptor.kindOf(i) == ListDescriptor::Spread)
                    functor(items[word], i);
                word += descriptor.kindOf(i) == ListDescriptor::Passed ? 2 : 1;
            }
        };
        forEachSpread([&](EncodedJSValue& item, unsigned) {
            allCanBeCopiedFrom &= canBeCopiedFrom(JSValue::decode(item));
        });
        if (!allCanBeCopiedFrom) [[unlikely]] {
            // Then code runs, which may do anything to the others: each is gone through when it is its turn, and what it came to is kept.
            bool threw = false;
            const void* returnAddress = removeCodePtrTag(callFrame->rawReturnPC());
            FunctionRef function = caller(globalObject, callFrame);
            uint32_t callSite = callSiteAt(*function.info().function(), whatIsAt(returnAddress).offset);
            forEachSpread([&](EncodedJSValue& item, unsigned index) {
                if (threw)
                    return;
                std::optional<SiteInPlaceOfCallSite> where;
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
                length += lengthOfWhatCanBeCopiedFrom(JSValue::decode(items[word++]));
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
    AOT_OPERATION_BEGIN_FOR_NOBODY(globalObject);
    ListDescriptor descriptor { descriptorBits };
    if (!descriptor.isOfItems()) {
        loadVarargs(globalObject, std::bit_cast<JSValue*>(where), JSValue::decode(listOrItems), descriptor.firstVarArg(), length);
        OPERATION_RETURN(scope);
    }
    // (Nothing has run since they were counted.)
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
            // (That takes other code, and this is all there is.)
            if (executable->isShortForm()) {
                throwSyntaxError(function->realm(), scope, makeString("The function "_s, executable->ecmaName().string(), " was compiled ahead of time for a module that is not linked the way it was then, and there is no other code for it"_s));
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

// Stub::LinkFunction. Whoever called the function knew what it was calling: code of a module that is linked as compiled, and so then are
// the modules it imports from, and theirs (JSModuleRecord::isLinkedAsInImage()). There is nothing that could be in the way.
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
    RELEASE_ASSERT(Data::create(*instance, executable, executable->unlinkedExecutable()->codeBlockIfThereIsOne(kind), code.get()));
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwStackOverflowError(instance->globalObject, scope);
}

// Empty if the callee is not eval after all.
JSC_DEFINE_JIT_OPERATION(operationAOTCallDirectEval, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue callee, uint32_t count, EncodedJSValue firstArgument, JSScope* callerScopeChain, EncodedJSValue thisValue, uint32_t bytecodeIndexBits, uint32_t lexicallyScopedFeatures))
{
    AOT_OPERATION_BEGIN(globalObject);
    // What eval() looks at of the frame that a call would have been made with.
    // It makes that the last frame that the VM knows of, so it has to lead somewhere: to where this was called from.
    // (A Register starts out as whatever was there.)
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
