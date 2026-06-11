/*
 * Copyright (C) 2007-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Cameron Zwarich (cwzwarich@uwaterloo.ca)
 * Copyright (C) 2024 Sosuke Suzuki <aosukeke@gmail.com>.
 * Copyright (C) 2024 Tetsuharu Ohzeki <tetsuharu.ohzeki@gmail.com>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSGlobalObject.h"

#include "AggregateError.h"
#include "SuppressedError.h"
#include "ThreadObject.h"
#include "InternalFieldTuple.h"
#include "AggregateErrorConstructorInlines.h"
#include "SuppressedErrorConstructorInlines.h"
#include "AggregateErrorPrototypeInlines.h"
#include "ArrayConstructor.h"
#include "SuppressedErrorPrototypeInlines.h"
#include "ArrayConstructorInlines.h"
#include "ArrayIteratorPrototypeInlines.h"
#include "ArrayPrototypeInlines.h"
#include "AsyncDisposableStackConstructor.h"
#include "AsyncDisposableStackPrototype.h"
#include "AsyncDisposableStackPrototypeInlines.h"
#include "AsyncFromSyncIteratorPrototypeInlines.h"
#include "AsyncFunctionConstructorInlines.h"
#include "AsyncFunctionPrototypeInlines.h"
#include "AsyncGeneratorFunctionConstructor.h"
#include "AsyncGeneratorFunctionPrototypeInlines.h"
#include "AsyncGeneratorPrototypeInlines.h"
#include "AsyncIteratorPrototypeInlines.h"
#include "AtomicsObject.h"
#include "BigIntConstructorInlines.h"
#include "BigIntObjectInlines.h"
#include "BigIntPrototypeInlines.h"
#include "BooleanConstructorInlines.h"
#include "BooleanObjectInlines.h"
#include "BooleanPrototypeInlines.h"
#include "BuiltinNames.h"
#include "ChainedWatchpoint.h"
#include "ClonedArguments.h"
#include "CodeBlock.h"
#include "CodeBlockSetInlines.h"
#include "ConsoleClient.h"
#include "ConsoleObjectInlines.h"
#include "CrossTaskToken.h"
#include "DateConstructorInlines.h"
#include "DateInstanceInlines.h"
#include "DatePrototypeInlines.h"
#include "Debugger.h"
#include "DebuggerScope.h"
#include "DeferTermination.h"
#include "DirectArguments.h"
#include "DisposableStackConstructor.h"
#include "DisposableStackPrototype.h"
#include "DisposableStackPrototypeInlines.h"
#include "ErrorConstructorInlines.h"
#include "ErrorInstanceInlines.h"
#include "ErrorPrototypeInlines.h"
#include "FinalizationRegistryConstructorInlines.h"
#include "FinalizationRegistryPrototypeInlines.h"
#include "FunctionConstructorInlines.h"
#include "FunctionPrototypeInlines.h"
#include "GeneratorFunctionConstructorInlines.h"
#include "GeneratorFunctionPrototypeInlines.h"
#include "GeneratorPrototypeInlines.h"
#include "GetterSetter.h"
#include "GlobalObjectMethodTable.h"
#include "HeapIterationScope.h"
#include "ImportMap.h"
#include "IntlCollator.h"
#include "IntlCollatorPrototype.h"
#include "IntlDateTimeFormat.h"
#include "IntlDateTimeFormatConstructor.h"
#include "IntlDateTimeFormatPrototype.h"
#include "IntlDisplayNames.h"
#include "IntlDisplayNamesPrototype.h"
#include "IntlDurationFormat.h"
#include "IntlDurationFormatPrototype.h"
#include "IntlListFormat.h"
#include "IntlListFormatPrototype.h"
#include "IntlLocale.h"
#include "IntlLocalePrototype.h"
#include "IntlNumberFormat.h"
#include "IntlNumberFormatConstructor.h"
#include "IntlNumberFormatPrototype.h"
#include "IntlObject.h"
#include "IntlPartObject.h"
#include "IntlPluralRules.h"
#include "IntlPluralRulesPrototype.h"
#include "IntlRelativeTimeFormat.h"
#include "IntlRelativeTimeFormatPrototype.h"
#include "IntlSegmentDataObject.h"
#include "IntlSegmentIterator.h"
#include "IntlSegmentIteratorPrototype.h"
#include "IntlSegmenter.h"
#include "IntlSegmenterPrototype.h"
#include "IntlSegments.h"
#include "IntlSegmentsPrototype.h"
#include "JSAPIWrapperObject.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferConstructor.h"
#include "JSArrayBufferPrototype.h"
#include "JSArrayIterator.h"
#include "JSAsyncDisposableStack.h"
#include "JSAsyncDisposableStackInlines.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncFromSyncIteratorInlines.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncFunctionInlines.h"
#include "JSAsyncGenerator.h"
#include "JSAsyncGeneratorFunctionInlines.h"
#include "JSBoundFunctionInlines.h"
#include "JSCallbackConstructor.h"
#include "JSCallbackFunction.h"
#include "JSCallbackObject.h"
#include "JSCalleeInlines.h"
#include "JSCast.h"
#include "JSCustomGetterFunctionInlines.h"
#include "JSCustomSetterFunctionInlines.h"
#include "JSDataView.h"
#include "JSDataViewPrototype.h"
#include "JSDisposableStack.h"
#include "JSDisposableStackInlines.h"
#include "JSDollarVM.h"
#include "JSFinalizationRegistry.h"
#include "JSFunction.h"
#include "JSFunctionWithFields.h"
#include "JSGenerator.h"
#include "JSGeneratorFunctionInlines.h"
#include "JSGenericTypedArrayViewConstructorInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSGenericTypedArrayViewPrototypeInlines.h"
#include "JSGlobalLexicalEnvironmentInlines.h"
#include "JSGlobalObjectFunctions.h"
#include "JSGlobalObjectInlines.h"
#include "JSGlobalProxyInlines.h"
#include "JSIterator.h"
#include "JSIteratorConstructor.h"
#include "JSIteratorHelper.h"
#include "JSIteratorHelperPrototypeInlines.h"
#include "JSIteratorPrototype.h"
#include "JSIteratorPrototypeInlines.h"
#include "JSLexicalEnvironmentInlines.h"
#include "JSMapInlines.h"
#include "JSMapIteratorInlines.h"
#include "JSMicrotask.h"
#include "JSMicrotaskDispatcher.h"
#include "JSModuleEnvironmentInlines.h"
#include "JSModuleLoaderInlines.h"
#include "JSModuleNamespaceObjectInlines.h"
#include "JSModuleRecord.h"
#include "JSModuleRecordInlines.h"
#include "JSNativeStdFunctionInlines.h"
#include "JSONObjectInlines.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsContextInlines.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseConstructor.h"
#include "JSPromisePrototype.h"
#include "JSPromiseReaction.h"
#include "JSRawJSONObject.h"
#include "JSRegExpStringIteratorInlines.h"
#include "JSRemoteFunctionInlines.h"
#include "JSSetInlines.h"
#include "JSSetIteratorInlines.h"
#include "JSStringIteratorInlines.h"
#include "JSThreadsSafepoint.h"
#include "JSTypedArrayConstructors.h"
#include "JSTypedArrayPrototypes.h"
#include "JSTypedArrayViewConstructor.h"
#include "JSTypedArrayViewPrototype.h"
#include "JSTypedArrays.h"
#include "JSWeakMapInlines.h"
#include "JSWeakObjectRefInlines.h"
#include "JSWeakSetInlines.h"
#include "JSWebAssembly.h"
#include "JSWebAssemblyArrayInlines.h"
#include "JSWebAssemblyCompileError.h"
#include "JSWebAssemblyException.h"
#include "JSWebAssemblyGlobal.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyLinkError.h"
#include "JSWebAssemblyMemory.h"
#include "JSWebAssemblyModule.h"
#include "JSWebAssemblyRuntimeError.h"
#include "JSWebAssemblyStruct.h"
#include "JSWebAssemblyTable.h"
#include "JSWebAssemblyTag.h"
#include "JSWithScope.h"
#include "JSWrapForValidIteratorInlines.h"
#include "LazyClassStructureInlines.h"
#include "LazyPropertyInlines.h"
#include "LinkTimeConstant.h"
#include "MapConstructorInlines.h"
#include "MapIteratorPrototypeInlines.h"
#include "MapPrototypeInlines.h"
#include "MarkedSpaceInlines.h"
#include "MathObjectInlines.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "NativeErrorConstructorInlines.h"
#include "NativeErrorPrototypeInlines.h"
#include "NullGetterFunctionInlines.h"
#include "NullSetterFunctionInlines.h"
#include "NumberConstructorInlines.h"
#include "NumberPrototypeInlines.h"
#include "ObjCCallbackFunction.h"
#include "ObjectAdaptiveStructureWatchpoint.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPropertyChangeAdaptiveWatchpoint.h"
#include "ObjectPropertyConditionSet.h"
#include "ObjectPrototypeInlines.h"
#include "PinballCompletion.h"
#include "ProfilerSupport.h"
#include "ProxyConstructorInlines.h"
#include "ProxyObjectInlines.h"
#include "ProxyRevokeInlines.h"
#include "ReflectObjectInlines.h"
#include "RegExpConstructorInlines.h"
#include "RegExpGlobalDataInlines.h"
#include "RegExpMatchesArray.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototypeInlines.h"
#include "RegExpStringIteratorPrototypeInlines.h"
#include "SamplingProfiler.h"
#include "ScopedArguments.h"
#include "SetConstructorInlines.h"
#include "SetIteratorPrototypeInlines.h"
#include "SetPrototypeInlines.h"
#include "ShadowRealmConstructorInlines.h"
#include "ShadowRealmObjectInlines.h"
#include "ShadowRealmPrototypeInlines.h"
#include "SourceCodeKey.h"
#include "StrictEvalActivationInlines.h"
#include "StringConstructorInlines.h"
#include "StringIteratorPrototypeInlines.h"
#include "StringObjectInlines.h"
#include "StringPrototypeInlines.h"
#include "StructureInlines.h"
#include "SuppressedError.h"
#include "SuppressedErrorConstructorInlines.h"
#include "SuppressedErrorPrototypeInlines.h"
#include "SymbolConstructorInlines.h"
#include "SymbolObjectInlines.h"
#include "SymbolPrototypeInlines.h"
#include "SyntheticModuleRecord.h"
#include "TemporalCalendar.h"
#include "TemporalDuration.h"
#include "TemporalDurationPrototype.h"
#include "TemporalInstant.h"
#include "TemporalInstantPrototype.h"
#include "TemporalObject.h"
#include "TemporalPlainDate.h"
#include "TemporalPlainDatePrototype.h"
#include "TemporalPlainDateTime.h"
#include "TemporalPlainDateTimePrototype.h"
#include "TemporalPlainMonthDay.h"
#include "TemporalPlainMonthDayPrototype.h"
#include "TemporalPlainTime.h"
#include "TemporalPlainTimePrototype.h"
#include "TemporalPlainYearMonth.h"
#include "TemporalPlainYearMonthPrototype.h"
#include "TemporalTimeZone.h"
#include "TemporalTimeZonePrototype.h"
#include "TopExceptionScope.h"
#include "VMLite.h"
#include "VMLiteInlines.h" // UNGIL §E.1/I11 (U-T9): per-lite microtask enqueue reroute.
#include "VMTrapsInlines.h"
#include "WaiterListManager.h"
#include "WasmCapabilities.h"
#include "WeakMapConstructorInlines.h"
#include "WeakMapPrototypeInlines.h"
#include "WeakObjectRefConstructorInlines.h"
#include "WeakObjectRefPrototypeInlines.h"
#include "WeakSetConstructorInlines.h"
#include "WeakSetPrototypeInlines.h"
#include "WebAssemblyArrayConstructor.h"
#include "WebAssemblyArrayPrototype.h"
#include "WebAssemblyCompileErrorConstructor.h"
#include "WebAssemblyCompileErrorPrototype.h"
#include "WebAssemblyExceptionConstructor.h"
#include "WebAssemblyExceptionPrototype.h"
#include "WebAssemblyFunction.h"
#include "WebAssemblyGlobalConstructor.h"
#include "WebAssemblyGlobalPrototype.h"
#include "WebAssemblyInstanceConstructor.h"
#include "WebAssemblyInstancePrototype.h"
#include "WebAssemblyLinkErrorConstructor.h"
#include "WebAssemblyLinkErrorPrototype.h"
#include "WebAssemblyMemoryConstructor.h"
#include "WebAssemblyMemoryPrototype.h"
#include "WebAssemblyModuleConstructor.h"
#include "WebAssemblyModulePrototype.h"
#include "WebAssemblyModuleRecord.h"
#include "WebAssemblyRuntimeErrorConstructor.h"
#include "WebAssemblyRuntimeErrorPrototype.h"
#include "WebAssemblyStructConstructor.h"
#include "WebAssemblyStructPrototype.h"
#include "WebAssemblySuspendErrorConstructor.h"
#include "WebAssemblySuspendErrorPrototype.h"
#include "WebAssemblySuspendingConstructor.h"
#include "WebAssemblySuspendingPrototype.h"
#include "WebAssemblyTableConstructor.h"
#include "WebAssemblyTablePrototype.h"
#include "WebAssemblyTagConstructor.h"
#include "WebAssemblyTagPrototype.h"
#include "WrapForValidIteratorPrototypeInlines.h"
#include "runtime/VM.h"
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SystemTracing.h>
#include <wtf/text/MakeString.h>

#if ENABLE(REMOTE_INSPECTOR)
#include "JSGlobalObjectDebuggable.h"
#include "JSGlobalObjectInspectorController.h"
#endif

#ifdef JSC_GLIB_API_ENABLED
#include "JSCCallbackFunction.h"
#include "JSCWrapperMap.h"
#endif

namespace JSC {

MicrotaskQueue& JSGlobalObject::microtaskQueue() const { return m_microtaskQueue.get(); }

#define CHECK_FEATURE_FLAG_TYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) \
static_assert(std::is_same_v<std::remove_cv_t<decltype(featureFlag)>, bool> || std::is_same_v<std::remove_cv_t<decltype(featureFlag)>, bool&>);

FOR_EACH_SIMPLE_BUILTIN_TYPE(CHECK_FEATURE_FLAG_TYPE)
FOR_EACH_BUILTIN_DERIVED_ITERATOR_TYPE(CHECK_FEATURE_FLAG_TYPE)
FOR_EACH_LAZY_BUILTIN_TYPE(CHECK_FEATURE_FLAG_TYPE)

static JSC_DECLARE_HOST_FUNCTION(createPrivateSymbol);
static JSC_DECLARE_HOST_FUNCTION(jsonParse);
static JSC_DECLARE_HOST_FUNCTION(jsonStringify);
static JSC_DECLARE_HOST_FUNCTION(enableSuperSampler);
static JSC_DECLARE_HOST_FUNCTION(disableSuperSampler);
static JSC_DECLARE_HOST_FUNCTION(resolvePromise);
static JSC_DECLARE_HOST_FUNCTION(rejectPromise);
static JSC_DECLARE_HOST_FUNCTION(fulfillPromise);
static JSC_DECLARE_HOST_FUNCTION(markPromiseAsHandledHostFunction);
static JSC_DECLARE_HOST_FUNCTION(isPromiseStatePending);
static JSC_DECLARE_HOST_FUNCTION(claimGeneratorResume);
static JSC_DECLARE_HOST_FUNCTION(publishGeneratorResume);
static JSC_DECLARE_HOST_FUNCTION(resolvePromiseWithFirstResolvingFunctionCallCheck);
static JSC_DECLARE_HOST_FUNCTION(rejectPromiseWithFirstResolvingFunctionCallCheck);
static JSC_DECLARE_HOST_FUNCTION(fulfillPromiseWithFirstResolvingFunctionCallCheck);
static JSC_DECLARE_HOST_FUNCTION(newResolvedPromise);
static JSC_DECLARE_HOST_FUNCTION(newRejectedPromise);
static JSC_DECLARE_HOST_FUNCTION(resolveWithInternalMicrotaskForAsyncAwait);
static JSC_DECLARE_HOST_FUNCTION(driveAsyncFunction);
static JSC_DECLARE_HOST_FUNCTION(newHandledRejectedPromise);
static JSC_DECLARE_HOST_FUNCTION(promiseReturnUndefinedOnFulfilled);
static JSC_DECLARE_HOST_FUNCTION(promiseResolve);
static JSC_DECLARE_HOST_FUNCTION(promiseReject);
#if USE(BUN_JSC_ADDITIONS)
static JSC_DECLARE_HOST_FUNCTION(promiseResolveWithThen);
#endif
static JSC_DECLARE_HOST_FUNCTION(performPromiseThen);
static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorQueueEnqueue);
static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorQueueDequeueResolve);
static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorQueueDequeueReject);
#if ASSERT_ENABLED
static JSC_DECLARE_HOST_FUNCTION(assertCall);
#endif
#if ENABLE(SAMPLING_PROFILER)
static JSC_DECLARE_HOST_FUNCTION(enableSamplingProfiler);
static JSC_DECLARE_HOST_FUNCTION(disableSamplingProfiler);
static JSC_DECLARE_HOST_FUNCTION(dumpAndClearSamplingProfilerSamples);
#endif

static JSC_DECLARE_HOST_FUNCTION(tracePointStart);
static JSC_DECLARE_HOST_FUNCTION(tracePointStop);
static JSC_DECLARE_HOST_FUNCTION(signpostStart);
static JSC_DECLARE_HOST_FUNCTION(signpostStop);

static JSValue initializeEvalFunction(VM&, JSObject* object)
{
    return uncheckedDowncast<JSGlobalObject>(object)->evalFunction();
}

static JSValue createProxyProperty(VM& vm, JSObject* object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return ProxyConstructor::create(vm, ProxyConstructor::createStructure(vm, global, global->functionPrototype()));
}

static JSValue createJSONProperty(VM& vm, JSObject* object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return JSONObject::create(vm, global, JSONObject::createStructure(vm, global, global->objectPrototype()));
}

static JSValue createMathProperty(VM& vm, JSObject* object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return MathObject::create(vm, global, MathObject::createStructure(vm, global, global->objectPrototype()));
}

static JSValue createReflectProperty(VM& vm, JSObject* object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return ReflectObject::create(vm, global, ReflectObject::createStructure(vm, global, global->objectPrototype()));
}

static JSValue createAtomicsProperty(VM& vm, JSObject *object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return AtomicsObject::create(vm, global, AtomicsObject::createStructure(vm, global, global->objectPrototype()));
}

static JSValue createConsoleProperty(VM& vm, JSObject* object)
{
    JSGlobalObject* global = uncheckedDowncast<JSGlobalObject>(object);
    return ConsoleObject::create(vm, global, ConsoleObject::createStructure(vm, global, constructEmptyObject(global)));
}

// FIXME: use a bytecode or intrinsic for creating a private symbol.
// https://bugs.webkit.org/show_bug.cgi?id=212782
JSC_DEFINE_HOST_FUNCTION(createPrivateSymbol, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto description = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    return JSValue::encode(Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get()));
}

JSC_DEFINE_HOST_FUNCTION(jsonParse, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto json = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    return JSValue::encode(JSONParse(globalObject, json));
}

JSC_DEFINE_HOST_FUNCTION(jsonStringify, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto result = JSONStringify(globalObject, callFrame->argument(0), callFrame->argument(1));
    return JSValue::encode(jsString(vm, result));
}

#if ASSERT_ENABLED
JSC_DEFINE_HOST_FUNCTION(assertCall, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    RELEASE_ASSERT(callFrame->argument(0).isBoolean());
    if (callFrame->argument(0).asBoolean())
        return JSValue::encode(jsUndefined());

    bool iteratedOnce = false;
    CodeBlock* codeBlock = nullptr;
    LineColumn lineColumn;
    StackVisitor::visit(callFrame, globalObject->vm(), [&] (StackVisitor& visitor) {
        if (!iteratedOnce) {
            iteratedOnce = true;
            return IterationStatus::Continue;
        }

        RELEASE_ASSERT(visitor->hasLineAndColumnInfo());
        lineColumn = visitor->computeLineAndColumn();
        codeBlock = visitor->codeBlock();
        return IterationStatus::Done;
    });
    RELEASE_ASSERT(!!codeBlock);
    RELEASE_ASSERT_WITH_MESSAGE(false, "JS assertion failed at line %u in:\n%s\n", lineColumn.line, codeBlock->sourceCodeForTools().data());
    return JSValue::encode(jsUndefined());
}
#endif // ASSERT_ENABLED

#if ENABLE(SAMPLING_PROFILER)
JSC_DEFINE_HOST_FUNCTION(enableSamplingProfiler, (JSGlobalObject* globalObject, CallFrame*))
{
    globalObject->vm().enableSamplingProfiler();
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(disableSamplingProfiler, (JSGlobalObject* globalObject, CallFrame*))
{
    globalObject->vm().disableSamplingProfiler();
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(dumpAndClearSamplingProfilerSamples, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue argument = callFrame->argument(0);
    auto filenamePrefix = emptyString();
    if (!argument.isUndefinedOrNull()) {
        filenamePrefix = argument.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }

    auto json = vm.takeSamplingProfilerSamplesAsJSON();
    if (!json) [[unlikely]]
        return JSValue::encode(jsUndefined());

    auto jsonData = json->toJSONString();
    {
        auto [tempFilePath, fileHandle] = FileSystem::openTemporaryFile(filenamePrefix);
        if (!fileHandle) {
            dataLogLn("Dumping sampling profiler samples failed to open temporary file");
            return JSValue::encode(jsUndefined());
        }

        CString utf8String = jsonData.utf8();

        fileHandle.write(byteCast<uint8_t>(utf8String.span()));
        dataLogLn("Dumped sampling profiler samples to ", tempFilePath);
    }

    return JSValue::encode(jsUndefined());
}
#endif

static uint64_t asTracePointInt(JSGlobalObject* globalObject, JSValue v)
{
    if (v.isUndefined())
        return 0;
    return v.toNumber(globalObject);
}

JSC_DEFINE_HOST_FUNCTION(tracePointStart, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto getValue = [&] (unsigned arg) {
        JSValue v = callFrame->argument(arg);
        return asTracePointInt(globalObject, v);
    };

    uint64_t one = getValue(0);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t two = getValue(1);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t three = getValue(2);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t four = getValue(3);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());

    tracePoint(TracePointCode::FromJSStart, one, two, three, four);

    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(tracePointStop, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto getValue = [&] (unsigned arg) {
        JSValue v = callFrame->argument(arg);
        return asTracePointInt(globalObject, v);
    };

    uint64_t one = getValue(0);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t two = getValue(1);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t three = getValue(2);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());
    uint64_t four = getValue(3);
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());

    tracePoint(TracePointCode::FromJSStop, one, two, three, four);
    return JSValue::encode(jsUndefined());
}

std::atomic<unsigned> activeJSGlobalObjectSignpostIntervalCount { 0 };

static String asSignpostString(JSGlobalObject* globalObject, JSValue v)
{
    if (v.isUndefined())
        return emptyString();
    return v.toWTFString(globalObject);
}

JSC_DEFINE_HOST_FUNCTION(signpostStart, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto message = asSignpostString(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());

    globalObject->startSignpost(WTF::move(message));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(signpostStop, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto message = asSignpostString(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, EncodedJSValue());

    globalObject->stopSignpost(WTF::move(message));
    return JSValue::encode(jsUndefined());
}

void JSGlobalObject::startSignpost(String&& message)
{
    ++activeJSGlobalObjectSignpostIntervalCount;
    auto* identifier = std::bit_cast<void*>(static_cast<uintptr_t>(m_signposts.ensure(message, [] {
        return JSCJSGlobalObjectSignpostIdentifier::generate();
    }).iterator->value.toUInt64()));
    UNUSED_VARIABLE(identifier);
    auto string = message.ascii();
    WTFBeginSignpostAlways(identifier, JSCJSGlobalObject, "%" PUBLIC_LOG_STRING, string.data());
    ProfilerSupport::markStart(identifier, ProfilerSupport::Category::JSGlobalObjectSignpost, WTF::move(string));
}

void JSGlobalObject::stopSignpost(String&& message)
{
    void* identifier = std::bit_cast<void*>(this);
    if (auto stored = m_signposts.takeOptional(message))
        identifier = std::bit_cast<void*>(static_cast<uintptr_t>(stored->toUInt64()));
    UNUSED_VARIABLE(identifier);
    auto string = message.ascii();
    WTFEndSignpostAlways(identifier, JSCJSGlobalObject, "%" PUBLIC_LOG_STRING, string.data());
    ProfilerSupport::markEnd(identifier, ProfilerSupport::Category::JSGlobalObjectSignpost, WTF::move(string));
    --activeJSGlobalObjectSignpostIntervalCount;
}

JSC_DEFINE_HOST_FUNCTION(enableSuperSampler, (JSGlobalObject*, CallFrame*))
{
    enableSuperSampler();
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(disableSuperSampler, (JSGlobalObject*, CallFrame*))
{
    disableSuperSampler();
    return JSValue::encode(jsUndefined());
}

} // namespace JSC

#include "JSGlobalObject.lut.h"

namespace JSC {

const ClassInfo JSGlobalObject::s_info = { "GlobalObject"_s, &Base::s_info, &globalObjectTable, nullptr, CREATE_METHOD_TABLE(JSGlobalObject) };

// =============================================================================
// UNGIL §E.1b.4 (U-T8e): host-hook disposition table — EVERY
// globalObjectMethodTable / host-callback slot JS-reachable on a spawned TS,
// with its GIL-off disposition. This block is the doc-of-record enumeration
// (transcribed to INTEGRATE-ungil.md §(v) at landing); GIL-on/flag-off
// (useJSThreads=false) behavior is unchanged for every row.
//
// Disposition vocabulary: {inline-safe, carrier-queued, refused-with-error,
// unreachable-on-spawned (proof)}. "inline-safe" = the hook runs on the
// acting (possibly spawned) thread under that thread's entry token; for
// embedder-INSTALLED hooks an inline-safe row additionally imposes the
// §F.6 embedder contract (hook must be thread-safe / pure; Bun-side audit
// U-T14). "carrier-queued" = the SD15 mechanism (VM.cpp, this change).
// "ENFORCEMENT OPEN (U-T9, <site>)" marks refused rows whose call sites are
// outside U-T8e's owned files; this table gates U-T9, which lands them.
//
// --- GlobalObjectMethodTable slots (GlobalObjectMethodTable.h:58-90) -------
//
// supportsRichSourceInfo — INLINE-SAFE. Pure const predicate (default
//   JSGlobalObject.h:1228 returns true). Spawned-reachable via error-stack
//   capture and the JSONP path (Interpreter.cpp:1056/:1059). Installed
//   overrides: §F.6 pure/thread-safe contract.
// shouldInterruptScript — UNREACHABLE-ON-SPAWNED. Proof: NO in-tree consumer
//   in the JSCOnly port (grep: only the table initializers in jsc.cpp /
//   JSAPIGlobalObject.cpp reference it); the WebCore-era consumer is the
//   watchdog client, and watchdog enforcement is carrier-only by SD14
//   (W1 carrier service episode; W3 timer-thread arm skips the callback).
// javaScriptRuntimeFlags — INLINE-SAFE (pure; default JSGlobalObject.h:1234).
//   Consulted only at global-object finishCreation (this file, both
//   JSGlobalObject::finishCreation overloads);
//   on a spawned TS that is reachable only through
//   deriveShadowRealmGlobalObject, itself REFUSED below.
// shouldInterruptScriptBeforeTimeout — UNREACHABLE-ON-SPAWNED (same proof as
//   shouldInterruptScript).
// moduleLoaderImportModule — REFUSED-WITH-ERROR (v1 ruling): dynamic import()
//   evaluated on a spawned TS rejects with a TypeError before the hook is
//   consulted. Rationale: installed loaders (jsc shell, Bun) drive
//   embedder-side fetch/IO with main-loop affinity and per-VM loader maps;
//   no SPEC-ungil section audits a spawned-initiated module graph, and the
//   spawned entry point is a plain function (api §5.2), so refusal loses no
//   chartered capability. ENFORCEMENT OPEN (U-T9, JSModuleLoader.cpp:492 —
//   importModule's hook branch; reject the internal promise when
//   vm.gilOff() && ThreadManager::isJSThreadCurrent()).
// moduleLoaderResolve — UNREACHABLE-ON-SPAWNED GIVEN the importModule
//   refusal. Proof: loader pipeline steps (resolve/fetch/meta/evaluate,
//   JSModuleLoader.cpp:508/:534/:548/:557) run only inside a module graph
//   load, which a spawned TS can no longer initiate; pipeline microtasks of
//   carrier-initiated graphs are registered on the carrier and their loader
//   promises are settled by the embedder — a spawned-thread settle of an
//   embedder loader promise would migrate the continuation (SD10) onto the
//   spawned TS, which the §F.6(b) embedder contract forbids for loader
//   promises (recorded as an F.6 checklist row; Bun audit U-T14).
// moduleLoaderFetch — UNREACHABLE-ON-SPAWNED (same proof).
// moduleLoaderCreateImportMetaProperties — UNREACHABLE-ON-SPAWNED (same
//   proof; import.meta materializes during carrier-driven evaluation).
// moduleLoaderEvaluate — UNREACHABLE-ON-SPAWNED (same proof).
// promiseRejectionTracker — CARRIER-QUEUED (SD15; the §E.1b.4 BINDING
//   ruling). Machinery landed by THIS task in VM.cpp: spawned Reject/Handle
//   events append {promise Strong, operation} records to a leaf-lock handoff
//   queue (no JS, no allocation beyond the record); records are flushed and
//   EXECUTED at the §F.1 carrier drain point (VM::didExhaustMicrotaskQueue)
//   under the carrier's token; ordering vs carrier-side events unspecified;
//   never lost while the carrier drains; no-carrier-ever-drains leaks are
//   declared (bounded by ~VM purge). The DEFAULT hook below is gated TODAY
//   through VM::promiseRejected; the four installed-hook call sites
//   (JSPromise.cpp:405/:464/:502/:637) are re-pointed by U-T9 at
//   notifyPromiseRejectionTrackerCrossThreadAware (VM.cpp seam).
// reportUncaughtExceptionAtEventLoop — INLINE-SAFE for the default (no-op,
//   below at JSGlobalObject::reportUncaughtExceptionAtEventLoop). Spawned-
//   reachable: a spawned drain's microtask throw reports on the draining
//   thread (MicrotaskQueue.cpp:66) — SD10-consistent (continuations and
//   their failures surface on the settling thread). The DWT site
//   (DeferredWorkTimer.cpp:157) is registrant-routed per §E.7.5 and so runs
//   on the registrant. Installed hooks: §F.6 thread-safe contract (Bun
//   routing audit U-T14; if Bun requires carrier affinity it reuses the SD15
//   record/flush shape — mechanism generalizes, no new spec needed).
// currentScriptExecutionOwner — INLINE-SAFE (default JSGlobalObject.h:1137
//   returns the global; pure). Spawned-reachable sites:
//   DeferredWorkTimer.cpp:197 (addPendingWork, spawned internal arm §E.7),
//   JSFinalizationRegistry.cpp:62.
// scriptExecutionStatus — INLINE-SAFE (default JSGlobalObject.h:1138 returns
//   Running; pure). Spawned-reachable at DWT dispatch
//   (DeferredWorkTimer.cpp:129). Installed overrides: §F.6 contract.
// reportViolationForUnsafeEval — INLINE-SAFE (default no-op,
//   JSGlobalObject.h:1139). Spawned-reachable via eval()/Function()
//   (Interpreter.cpp:159, FunctionConstructor.cpp:204,
//   DirectEvalExecutable.cpp:44, IndirectEvalExecutable.cpp:45,
//   JSGlobalObjectFunctions.cpp:491). Installed (CSP/trusted-types): §F.6.
// defaultLanguage — INLINE-SAFE (base slot null => JSC fallback). Spawned-
//   reachable via Intl (IntlObject.cpp:844). Installed overrides must return
//   an isolated String (§F.6).
// compileStreaming / instantiateStreaming — INLINE on the acting thread when
//   installed (presence-gated, JSWebAssembly.cpp:124/:126; invoked from the
//   draining thread's microtask, JSMicrotask.cpp:1499-1500/:1512-1513, so
//   spawned-reachable under SD10). The streaming result settles a promise
//   whose DWT ticket follows §E.7.5 REGISTRANT routing — a spawned
//   registrant's settlement never routes via carrier drains. Base table /
//   Bun: slot null => UNREACHABLE (the non-streaming fallback runs instead).
//   jsc-shell impls enqueue onto the Wasm worklist (thread-safe).
// deriveShadowRealmGlobalObject — REFUSED-WITH-ERROR (v1 ruling): `new
//   ShadowRealm()` on a spawned TS throws a TypeError before the hook runs.
//   Rationale: fresh-realm creation (JSGlobalObject::init) mutates VM-wide
//   caches/watchpoint sets whose §K/§N rulings audited EXISTING-global
//   access, not off-carrier realm BOOT; deferring to v1 keeps the audit
//   honest (earlier enablement is always legal post-audit). ENFORCEMENT OPEN
//   (U-T9, ShadowRealmObject.cpp:65 — gate ahead of the hook call).
// codeForEval — INLINE-SAFE (default JSGlobalObject.h:1140 nullString; pure).
//   Spawned-reachable sites Interpreter.cpp:135,
//   JSGlobalObjectFunctions.cpp:469. Installed (trusted types): §F.6.
// canCompileStrings — INLINE-SAFE (default JSGlobalObject.h:1141 true; pure).
//   Sites Interpreter.cpp:149, FunctionConstructor.cpp:192,
//   JSGlobalObjectFunctions.cpp:482. Installed: §F.6.
// trustedScriptStructure — INLINE-SAFE (default JSGlobalObject.h:1142 null).
//   Consulted once at realm init (this file, JSGlobalObject::init's
//   m_trustedScriptStructure stamp), carrier-side given the
//   shadow-realm refusal; the cached read afterwards is an ordinary GC slot.
//
// --- Adjacent host-callback slots (not in the method table, same audit) ----
//
// ConsoleClient (m_consoleClient, per-global WeakPtr) — INLINE on the acting
//   thread. Spawned-reachable via console.*; base default null => console
//   falls back to dataLog (serialized internally). Installed clients
//   (inspector, Bun): §F.6 thread-safe contract; Bun audit U-T14.
// Debugger (m_debugger) — UNREACHABLE-ON-SPAWNED by SD13 (spawned
//   breakpoints no-op GIL-off; §A.2.7 walks happen under a §A.3 stop).
// CrossTaskToken::createMicrotaskDispatcher / JSMicrotaskDispatcher
//   (JSGlobalObject::queueMicrotask, this file) — INLINE at enqueue on the
//   acting thread; the per-lite queue reroute itself is §E.1b/I11 (U-T9).
// VM Bun hooks m_onComputeErrorInfo / m_onComputeErrorInfoJSValue /
//   m_computeLineColumnWithSourcemap (VM.h) — INLINE on the throwing thread
//   (Error.stack capture is spawned-reachable); §F.6 contract, Bun audit
//   U-T14.
// VM::m_onEachMicrotaskTick (VM.h:1538) — INLINE on the draining thread
//   (spawned drains included); §F.6 contract, Bun audit U-T14.
// DeferredWorkTimer onAddPendingWork/onScheduleWorkSoon/onCancelPendingWork/
//   onCrossThreadWorkEnqueued — ruled by §E.7 (hookManaged: main/embedder
//   registrants only; spawned tickets always take the internal arm), owned
//   by U-T9; listed here for enumeration closure only.
// Watchdog embedder callback — carrier-only by SD14 (W1/W3); owned by U-T2's
//   traps/watchdog work; listed for closure.
// =============================================================================
const GlobalObjectMethodTable* JSGlobalObject::baseGlobalObjectMethodTable()
{
    static constexpr GlobalObjectMethodTable table = {
        &supportsRichSourceInfo,
        &shouldInterruptScript,
        &javaScriptRuntimeFlags,
        &shouldInterruptScriptBeforeTimeout,
        nullptr, // moduleLoaderImportModule
        nullptr, // moduleLoaderResolve
        nullptr, // moduleLoaderFetch
        nullptr, // moduleLoaderCreateImportMetaProperties
        nullptr, // moduleLoaderEvaluate
        &promiseRejectionTracker,
        &reportUncaughtExceptionAtEventLoop,
        &currentScriptExecutionOwner,
        &scriptExecutionStatus,
        &reportViolationForUnsafeEval,
        nullptr, // defaultLanguage
        nullptr, // compileStreaming
        nullptr, // instantiateStreaming
        &deriveShadowRealmGlobalObject,
        &codeForEval,
        &canCompileStrings,
        &trustedScriptStructure,
    };
    return &table;
};

/* Source for JSGlobalObject.lut.h
@begin globalObjectTable
  isNaN                 globalFuncIsNaN                              DontEnum|Function 1         GlobalIsNaNIntrinsic
  isFinite              globalFuncIsFinite                           DontEnum|Function 1         GlobalIsFiniteIntrinsic
  escape                globalFuncEscape                             DontEnum|Function 1
  unescape              globalFuncUnescape                           DontEnum|Function 1
  decodeURI             globalFuncDecodeURI                          DontEnum|Function 1
  decodeURIComponent    globalFuncDecodeURIComponent                 DontEnum|Function 1
  encodeURI             globalFuncEncodeURI                          DontEnum|Function 1
  encodeURIComponent    globalFuncEncodeURIComponent                 DontEnum|Function 1
  eval                  initializeEvalFunction                       DontEnum|PropertyCallback
  globalThis            JSGlobalObject::m_globalThis                 DontEnum|CellProperty
  parseInt              JSGlobalObject::m_parseIntFunction           DontEnum|CellProperty
  parseFloat            JSGlobalObject::m_parseFloatFunction         DontEnum|CellProperty
  ArrayBuffer           JSGlobalObject::m_arrayBufferStructure       DontEnum|ClassStructure
  EvalError             JSGlobalObject::m_evalErrorStructure         DontEnum|ClassStructure
  RangeError            JSGlobalObject::m_rangeErrorStructure        DontEnum|ClassStructure
  ReferenceError        JSGlobalObject::m_referenceErrorStructure    DontEnum|ClassStructure
  SyntaxError           JSGlobalObject::m_syntaxErrorStructure       DontEnum|ClassStructure
  TypeError             JSGlobalObject::m_typeErrorStructure         DontEnum|ClassStructure
  URIError              JSGlobalObject::m_URIErrorStructure          DontEnum|ClassStructure
  AggregateError        JSGlobalObject::m_aggregateErrorStructure    DontEnum|ClassStructure
  SuppressedError       JSGlobalObject::m_suppressedErrorStructure   DontEnum|ClassStructure
  Proxy                 createProxyProperty                          DontEnum|PropertyCallback
  Reflect               createReflectProperty                        DontEnum|PropertyCallback
  JSON                  createJSONProperty                           DontEnum|PropertyCallback
  Math                  createMathProperty                           DontEnum|PropertyCallback
  Atomics               createAtomicsProperty                        DontEnum|PropertyCallback
  console               createConsoleProperty                        DontEnum|PropertyCallback
  Int8Array             JSGlobalObject::m_typedArrayInt8             DontEnum|ClassStructure
  Int16Array            JSGlobalObject::m_typedArrayInt16            DontEnum|ClassStructure
  Int32Array            JSGlobalObject::m_typedArrayInt32            DontEnum|ClassStructure
  Uint8Array            JSGlobalObject::m_typedArrayUint8            DontEnum|ClassStructure
  Uint8ClampedArray     JSGlobalObject::m_typedArrayUint8Clamped     DontEnum|ClassStructure
  Uint16Array           JSGlobalObject::m_typedArrayUint16           DontEnum|ClassStructure
  Uint32Array           JSGlobalObject::m_typedArrayUint32           DontEnum|ClassStructure
  Float16Array          JSGlobalObject::m_typedArrayFloat16          DontEnum|ClassStructure
  Float32Array          JSGlobalObject::m_typedArrayFloat32          DontEnum|ClassStructure
  Float64Array          JSGlobalObject::m_typedArrayFloat64          DontEnum|ClassStructure
  BigInt64Array         JSGlobalObject::m_typedArrayBigInt64         DontEnum|ClassStructure
  BigUint64Array        JSGlobalObject::m_typedArrayBigUint64        DontEnum|ClassStructure
  DataView              JSGlobalObject::m_typedArrayDataView         DontEnum|ClassStructure
  Date                  JSGlobalObject::m_dateStructure              DontEnum|ClassStructure
  Error                 JSGlobalObject::m_errorStructure             DontEnum|ClassStructure
  Boolean               JSGlobalObject::m_booleanObjectStructure     DontEnum|ClassStructure
  Map                   JSGlobalObject::m_mapStructure               DontEnum|ClassStructure
  Number                JSGlobalObject::m_numberObjectStructure      DontEnum|ClassStructure
  Set                   JSGlobalObject::m_setStructure               DontEnum|ClassStructure
  WeakMap               JSGlobalObject::m_weakMapStructure           DontEnum|ClassStructure
  WeakSet               JSGlobalObject::m_weakSetStructure           DontEnum|ClassStructure
@end
*/

#if USE(BUN_JSC_ADDITIONS)
JSC_DEFINE_HOST_FUNCTION(enqueueJob, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* job = uncheckedDowncast<JSFunction>(callFrame->argument(0));
    ASSERT(job->realm() == globalObject);
    JSValue argument0 = callFrame->argument(1);
    JSValue argument1 = callFrame->argument(2);
    JSValue argument2 = callFrame->argument(3);
    // maxMicrotaskArguments=4: job + 3 user arguments
    JSC::QueuedTask task { nullptr, JSC::InternalMicrotask::BunInvokeJobWithArguments, 0, globalObject, job, argument0, argument1, argument2 };
    globalObject->vm().queueMicrotask(WTF::move(task));
    return encodedJSUndefined();
}
#endif

JSC_DEFINE_HOST_FUNCTION(resolvePromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->resolvePromise(globalObject, globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(rejectPromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->rejectPromise(globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(fulfillPromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->fulfillPromise(globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(markPromiseAsHandledHostFunction, (JSGlobalObject*, CallFrame* callFrame))
{
    auto* promise = dynamicDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    promise->markAsHandled();
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(isPromiseStatePending, (JSGlobalObject*, CallFrame* callFrame))
{
    auto* promise = dynamicDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    return JSValue::encode(jsBoolean(promise->status() == JSPromise::Status::Pending));
}

// SPEC-ungil §N.5 (BINDING; annex N7 row R7): GIL-off resume-claim twin
// primitives for builtins/GeneratorPrototype.js + JSIteratorHelperPrototype.js
// (the §N.5 @atomicInternalFieldClaim / @atomicInternalFieldPublish pair,
// landed as host hooks behind the @gilOffProcess constant branch; flag-off
// and GIL-on keep the landed inline sequence verbatim).
//
// SHAPE NOTE (supersession recorded in SPEC-ungil-history.md, "§N.5 LANDED
// SHAPE" entry): this is NOT the r17 F5 lowering shape. r17 F5 prescribes
// uniform bytecode (intrinsics emitted unconditionally in all modes) with
// the LOWERING keyed at LLInt/Baseline on the JSCConfig gilOffProcess byte
// and at DFG/FTL via AtomicInternalFieldClaim/Publish NODES lowering to an
// inline CAS / release store. What landed instead keys the EMISSION (the
// @gilOffProcess bytecode constant branch) and reaches these host hooks as
// ordinary calls in every tier. Flag-off identity holds (the constant-false
// branch keeps GIL-on/flag-off on the landed inline sequence); the gilOff
// host-call cost is recorded-not-gated (§B.5), with the r17 F5 intrinsic/
// node form the named perf contingency. Mode-keyed bytecode is sound only
// because the derivation is process-immutable AND the disk bytecode cache
// version is partitioned on it (JSCBytecodeCacheVersion.cpp).
//
// The landed resume head's check-then-store (read state; throw on Executing;
// store Executing) is two separate accesses — GIL-off, two threads racing
// .next() can both pass the check and both resume into one suspended frame
// (the MC-PRIM P5 / MC-TEAR S6 hit). The ruling makes the whole head ONE
// claim: a single-word CAS SuspendedX -> ClaimToken on the State internal
// field. Claim FAILURE dispatches on the RE-READ (no SD): any executing
// observation is returned as the canonical Executing so the caller throws
// the existing "Generator is executing" TypeError (§N.5: NOT an SD);
// Completed is terminal, never claimed, dispatched read-only by the caller.
//
// The claim word carries OWNER IDENTITY (a per-thread token < Executing),
// not the plain Executing sentinel, because the resume epilogue must decide
// "did MY resume run to completion?" by CASing ITS OWN token -> Completed:
// after the body unclaims at a yield (the suspend-state store, ordered after
// the frame save by BytecodeGeneratorification's gilOffProcess reorder), a
// second thread can immediately claim — a plain "state == Executing" re-read
// would confuse the rival's claim with our own completion and fabricate a
// done:true carrying the yielded value (the torn-completion arm of
// mc-prim-generator-resume-claim).
//
// Memory order: the claim's acquire pairs with the previous resumer's frame
// publication — the release half is real in every tier: gilOffProcess,
// op_put_internal_field emits a store-store fence before the field store
// (r15 F1 "UNCLAIM transitions are store-RELEASE in ALL tiers"; LLInt
// LowLevelInterpreter64.asm, Baseline JITPropertyAccess.cpp, DFG/FTL
// compilePutInternalField), so the yield-side SuspendedX store publishes the
// OpPutToScope frame saves on weak-memory targets. The winner therefore
// reads a fully published frame; at-most-one-claimant then keeps every
// interior store while claimed plain (§N.5).
// States and tokens are int32 JSValues — no cell is ever stored, so no write
// barrier is needed.
static ALWAYS_INLINE EncodedJSValue generatorClaimTokenForCurrentThread()
{
    // Tokens occupy (-inf, Executing): claimGeneratorResume only ever claims
    // values >= 0 (Init / suspend points), so a foreign token can never be
    // claimed, and any value <= Executing reads as "executing". WTF::Thread
    // uids are unique for the process lifetime (never recycled); the 2^30
    // truncation can only collide two SIMULTANEOUSLY-LIVE threads whose uids
    // differ by a multiple of 2^30 — over a billion thread creations with
    // both endpoints alive — accepted (and the collision outcome is the
    // pre-token done-ambiguity, not a new unsafety class).
    int32_t token = static_cast<int32_t>(JSGenerator::State::Executing) - 1 - static_cast<int32_t>(Thread::currentSingleton().uid() & 0x3FFFFFFFu);
    return JSValue::encode(jsNumber(token));
}

JSC_DEFINE_HOST_FUNCTION(claimGeneratorResume, (JSGlobalObject*, CallFrame* callFrame))
{
    // The builtin callers gate on @isGenerator / the iterator-helper
    // generator field before calling.
    auto* generator = uncheckedDowncast<JSGenerator>(asObject(callFrame->uncheckedArgument(0)));
    auto& slot = generator->internalField(static_cast<unsigned>(JSGenerator::Field::State));
    auto* word = std::bit_cast<Atomic<EncodedJSValue>*>(&slot);
    static const EncodedJSValue executingBits = JSValue::encode(jsNumber(static_cast<int32_t>(JSGenerator::State::Executing)));
    static const EncodedJSValue completedBits = JSValue::encode(jsNumber(static_cast<int32_t>(JSGenerator::State::Completed)));
    const EncodedJSValue tokenBits = generatorClaimTokenForCurrentThread();
    for (;;) {
        EncodedJSValue observed = word->load(std::memory_order_acquire);
        if (observed == completedBits)
            return observed;
        // The State field only ever holds int32 jsNumbers (states + tokens).
        int32_t observedState = JSValue::decode(observed).asInt32();
        if (observedState <= static_cast<int32_t>(JSGenerator::State::Executing))
            return executingBits; // Executing or a claim token: report the canonical Executing.
        // SuspendedX (Init or a suspend point): claim it with our token.
        // compareExchangeWeak may fail spuriously; re-read and re-dispatch (a
        // racing claimant may have won — the next iteration bails).
        if (word->compareExchangeWeak(observed, tokenBits, std::memory_order_acq_rel))
            return observed;
    }
}

// The §N.5 UNCLAIM/publish half for the resume epilogue: CAS
// ourToken -> Completed. Success means the body never unclaimed (no suspend
// store overwrote our token), i.e. THIS resume ran the generator to
// completion (or threw) => done:true. Failure means the body suspended (the
// yield's post-save state store was the unclaim; the field now holds
// SuspendedY, a rival's token, or Completed) => done:false for OUR resume.
// The release on success orders the body's result publication before any
// later claimant's acquire.
JSC_DEFINE_HOST_FUNCTION(publishGeneratorResume, (JSGlobalObject*, CallFrame* callFrame))
{
    auto* generator = uncheckedDowncast<JSGenerator>(asObject(callFrame->uncheckedArgument(0)));
    auto& slot = generator->internalField(static_cast<unsigned>(JSGenerator::Field::State));
    auto* word = std::bit_cast<Atomic<EncodedJSValue>*>(&slot);
    static const EncodedJSValue completedBits = JSValue::encode(jsNumber(static_cast<int32_t>(JSGenerator::State::Completed)));
    const EncodedJSValue tokenBits = generatorClaimTokenForCurrentThread();
    EncodedJSValue witnessed = word->compareExchangeStrong(tokenBits, completedBits, std::memory_order_acq_rel);
    return JSValue::encode(jsBoolean(witnessed == tokenBits));
}

JSC_DEFINE_HOST_FUNCTION(resolvePromiseWithFirstResolvingFunctionCallCheck, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->resolve(globalObject, globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(rejectPromiseWithFirstResolvingFunctionCallCheck, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->reject(globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(fulfillPromiseWithFirstResolvingFunctionCallCheck, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    promise->fulfill(globalObject->vm(), argument);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(newResolvedPromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    JSValue argument = callFrame->uncheckedArgument(0);
    auto* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->resolve(globalObject, vm, argument);
    return JSValue::encode(promise);
}

JSC_DEFINE_HOST_FUNCTION(newRejectedPromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue argument = callFrame->uncheckedArgument(0);
    return JSValue::encode(JSPromise::rejectedPromise(globalObject, argument));
}

JSC_DEFINE_HOST_FUNCTION(resolveWithInternalMicrotaskForAsyncAwait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    JSValue resolution = callFrame->uncheckedArgument(0);
    auto task = static_cast<InternalMicrotask>(callFrame->uncheckedArgument(1).asUInt32AsAnyInt());
    JSValue context = callFrame->uncheckedArgument(2);
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, resolution, task, context);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(driveAsyncFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    JSValue resolution = callFrame->uncheckedArgument(0);
    JSValue context = callFrame->uncheckedArgument(1);
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, resolution, InternalMicrotask::AsyncFunctionResume, context);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(newHandledRejectedPromise, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue argument = callFrame->uncheckedArgument(0);
    auto* promise = JSPromise::rejectedPromise(globalObject, argument);
    promise->markAsHandled();
    return JSValue::encode(promise);
}

JSC_DEFINE_HOST_FUNCTION(promiseReturnUndefinedOnFulfilled, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(promiseResolve, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    ASSERT(callFrame->argumentCount() == 2);
    JSObject* constructor = uncheckedDowncast<JSObject>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    return JSValue::encode(JSPromise::promiseResolve(globalObject, constructor, argument));
}

JSC_DEFINE_HOST_FUNCTION(promiseReject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    ASSERT(callFrame->argumentCount() == 2);
    JSObject* constructor = uncheckedDowncast<JSObject>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);
    return JSValue::encode(JSPromise::promiseReject(globalObject, constructor, argument));
}

#if USE(BUN_JSC_ADDITIONS)
// Same as promiseResolve, but sets the @then property on the returned promise.
// This is needed because Bun's builtins use promise.@then() pattern which requires
// the @then property to be set directly on the promise object, not just on the prototype.
// Additionally, this function "shields" InternalPromise by wrapping it in a regular Promise,
// ensuring that internal promises are not exposed to user code.
JSC_DEFINE_HOST_FUNCTION(promiseResolveWithThen, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    ASSERT(callFrame->argumentCount() == 2);
    JSObject* constructor = uncheckedDowncast<JSObject>(callFrame->uncheckedArgument(0));
    JSValue argument = callFrame->uncheckedArgument(1);

    JSObject* promise = JSPromise::promiseResolve(globalObject, constructor, argument);
    if (promise) [[likely]] {
        // Set @then property on the promise if it doesn't already have one
        auto thenPrivateName = vm.propertyNames->builtinNames().thenPrivateName();
        if (!promise->hasOwnProperty(globalObject, thenPrivateName))
            promise->putDirect(vm, thenPrivateName, globalObject->promiseProtoThenFunction(), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    }
    return JSValue::encode(promise);
}
#endif

JSC_DEFINE_HOST_FUNCTION(performPromiseThen, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(0));
    JSValue onFulfilled = callFrame->uncheckedArgument(1);
    JSValue onRejected = callFrame->uncheckedArgument(2);
    JSValue promiseOrCapability = callFrame->uncheckedArgument(3);
#if USE(BUN_JSC_ADDITIONS)
    if (callFrame->argumentCount() > 4) {
        JSValue context = callFrame->uncheckedArgument(4);
        promise->performPromiseThenWithContext(globalObject->vm(), globalObject, onFulfilled, onRejected, promiseOrCapability, context);
        return encodedJSUndefined();
    }
#endif
    promise->performPromiseThen(globalObject->vm(), globalObject, onFulfilled, onRejected, promiseOrCapability);
    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorQueueEnqueue, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    JSAsyncGenerator* generator = dynamicDowncast<JSAsyncGenerator>(callFrame->uncheckedArgument(0));
    JSValue value = callFrame->uncheckedArgument(1);
    int32_t resumeMode = callFrame->uncheckedArgument(2).asInt32();
    JSPromise* promise = uncheckedDowncast<JSPromise>(callFrame->uncheckedArgument(3));

    if (!generator) [[unlikely]] {
        promise->reject(vm, createTypeError(globalObject, "|this| should be an async generator"_s));
        return JSValue::encode(jsNumber(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorResumeMode::Empty)));
    }

    generator->enqueue(vm, value, resumeMode, promise);

    if (generator->isExecutionState())
        return JSValue::encode(jsNumber(static_cast<int32_t>(JSAsyncGenerator::AsyncGeneratorResumeMode::Empty)));
    return JSValue::encode(jsNumber(generator->resumeMode()));
}

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorQueueDequeueResolve, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    JSAsyncGenerator* generator = uncheckedDowncast<JSAsyncGenerator>(callFrame->uncheckedArgument(0));
    JSValue resolution = callFrame->uncheckedArgument(1);

    auto [value, resumeMode, promise] = generator->dequeue(vm);

    promise->resolve(globalObject, vm, resolution);

    return JSValue::encode(jsNumber(generator->resumeMode()));
}

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorQueueDequeueReject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();

    JSAsyncGenerator* generator = uncheckedDowncast<JSAsyncGenerator>(callFrame->uncheckedArgument(0));
    JSValue error = callFrame->uncheckedArgument(1);

    auto [value, resumeMode, promise] = generator->dequeue(vm);

    promise->reject(vm, error);

    return JSValue::encode(jsNumber(generator->resumeMode()));
}

JS_GLOBAL_OBJECT_ADDITIONS_2;

JSGlobalObject::JSGlobalObject(VM& vm, Structure* structure, const GlobalObjectMethodTable* globalObjectMethodTable)
    : Base(vm, structure, nullptr)
    , m_vm(&vm)
    , m_microtaskQueue(vm.defaultMicrotaskQueue())
    , m_linkTimeConstants(numberOfLinkTimeConstants)
    , m_structureCache(vm)
    , m_symbolTableCache(vm)
    , m_masqueradesAsUndefinedWatchpointSet(WatchpointSet::create(IsWatched))
    , m_havingABadTimeWatchpointSet(WatchpointSet::create(IsWatched))
    , m_varInjectionWatchpointSet(WatchpointSet::create(IsWatched))
    , m_varReadOnlyWatchpointSet(WatchpointSet::create(IsWatched))
    , m_regExpRecompiledWatchpointSet(WatchpointSet::create(IsWatched))
    , m_arrayBufferDetachWatchpointSet(WatchpointSet::create(IsWatched))
    , m_weakRandom(Options::forceWeakRandomSeed() ? Options::forcedWeakRandomSeed() : cryptographicallyRandomNumber<uint32_t>())
    , m_runtimeFlags()
    , m_stackTraceLimit(Options::defaultErrorStackTraceLimit())
    , m_customGetterFunctionSet(vm)
    , m_customSetterFunctionSet(vm)
    , m_importMap(ImportMap::create())
    , m_globalObjectMethodTable(globalObjectMethodTable ? globalObjectMethodTable : baseGlobalObjectMethodTable())
{
}

// =============================================================================
// UNGIL §K.1 per-lite realm duplicates (U-T8b; ANNEX AUD1.K2/SD19 + ALS1.3).
//
// Two JSGlobalObject-resident members are ruled §K.1 per-lite (annex K4):
//
//  - m_regExpGlobalData (AUD1.K2, K4 §0 U2, N7 RESOLVED-7; SD19): multi-word
//    RegExp legacy-statics cache rewritten on every global-flag match,
//    DFG/FTL-inlined (RecordRegExpCachedResult). GIL-off each entered thread
//    owns a PRIVATE stream; RegExp.$1-$9/lastMatch/leftContext/rightContext/
//    input observe only the CURRENT thread's matches (SD19, GIL-off only).
//  - m_asyncContextData (ALS1.3, PRE-RULED class 1): the ALS cursor is
//    swap-written by every job run and by Bun's enter/exit hooks; "current
//    async context" is thread-local by definition, so the cursor reroutes
//    per-lite. (The REROUTE of the JSPromise.cpp/JSMicrotask.cpp/
//    JSPromisePrototype.cpp capture/restore sites is U-T9's ALS1 work; this
//    block lands the storage + accessor + rooting + teardown those sites
//    consume. Captured-tuple carry across threads is ALS1.1/SD10 —
//    unaffected.)
//
// STORAGE: VMLite carries no L2 slot for either yet (VMLite.h is outside
// every U-T8-wave writable set — see the A16-extension activation checklist
// in VMLite.cpp; the JIT-addressable slot lands with the emission slice).
// Until then the per-lite copies live in this leaf-locked side table keyed
// (global, lite). That satisfies the three §K.1 structural duties NOW:
//  - REGISTRY-WALK ROOTING (§A.1.3 GC-roots rule; K4 binding consequence 3):
//    visitChildrenImpl walks this global's entries under the table lock (the
//    M11 precedent — markers hold no other lock there) and visits the
//    cell-holding copies.
//  - ~VM / LITE-TEARDOWN WALK: every lite teardown path funnels through
//    ~VMLite (walk-freed, TLS-destructor-freed, deferred — see VM.cpp's
//    EXIT1.9 machinery), which calls purgePerLiteRealmStateForLite below —
//    per-lite copies die with their lite, and the EXIT1.9 ~VM fence
//    guarantees all non-main lites are gone before VM members die.
//    Per-global entries die in ~JSGlobalObject.
//  - CURRENT-LITE ACCESSORS: threadRegExpGlobalData / threadAsyncContextData
//    route gilOff non-main-carrier threads to their copy; everything else
//    (flag-off, GIL-on, the main carrier) keeps the in-object member
//    BYTE-IDENTICALLY — the main carrier's stream IS the in-object one,
//    matching AUD1.K2's "flag-off keeps the baked global-object-relative
//    address" (the future lite slot for the main carrier aliases the
//    member).
//
// Lock discipline: the table lock is a §LK.7 leaf; ALL cell allocation and
// record()/create() initialization happens OUTSIDE it (alloc-outside shape,
// §E.1b / WS1.2); losers' buffers are destroyed after release.
//
// RE-POINT STATUS: the AUD1.K2 runtime-consumer re-point LANDED (TSAN wave
// 1) — every C++ consumer of the match-result stream (RegExpConstructor.cpp,
// RegExpPrototype.cpp, RegExpObject paths, StringPrototype paths,
// RegExpSubstringGlobalAtomCache.cpp, DFGOperations.cpp slow paths) calls
// threadRegExpGlobalData (declared in JSGlobalObject.h). JIT SIDE: gilOff
// compilations can no longer emit the shared-stream stores —
// DFGStrengthReductionPhase refuses the RecordRegExpCachedResult /
// RegExpTestInline conversions when gilOff (the generic nodes lower to the
// re-pointed operations) and DFG + FTL both fail-stop if such a node is ever
// reached gilOff. The residual was a MEMORY-SAFETY gap (torn multi-word
// record -> OOB substring in leftContext()), not stale legacy statics — see
// RegExpCachedResult.h's banner. STILL OPEN (perf only): the A16-ext jit
// slice re-points the inline emission at the lite-resident copy so gilOff
// regains the inline fast path. ALS1.3 STATUS (U-T9): the CAPTURE-side
// re-point LANDED (JSPromise.cpp's five registration sites call
// threadAsyncContextData) but the accessor's per-lite routing is GATED OFF
// (perLiteAsyncContextCursorEnabled below) until the RESTORE-side brackets
// (JSMicrotask.cpp save/write/run/write-back) and the then() prototype
// fast-path capture (JSPromisePrototype.cpp) — both TUs outside U-T9's owned
// set — land their re-point; a capture-only reroute would split the cursor
// regime (see the gate comment). Single flip enables the whole annex.
// =============================================================================

void jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(VM*); // Defined in VMLite.cpp (K4 §VIII machinery); identical self-declaration.
void purgePerLiteRealmStateForLite(VMLite&); // Defined below; ~VMLite (VMLite.cpp) self-declares and calls it.

namespace {

struct PerLiteRealmState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PerLiteRealmState);
    std::unique_ptr<RegExpGlobalData> regExpGlobalData;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<InternalFieldTuple> asyncContextData;
#endif
};

struct PerLiteRealmTable {
    Lock lock; // §LK.7 leaf: nothing is acquired under it; markers take it bare (M11 shape).
    HashMap<std::pair<JSGlobalObject*, VMLite*>, std::unique_ptr<PerLiteRealmState>> map WTF_GUARDED_BY_LOCK(lock);
};

PerLiteRealmTable& perLiteRealmTable()
{
    static NeverDestroyed<PerLiteRealmTable> table;
    return table;
}

// Routing predicate: non-null iff the CURRENT thread must use a per-lite
// copy — gilOff VM, an installed same-VM lite that is not the main carrier.
// Everything else (flag-off, GIL-on, main carrier, unentered probes) keeps
// the in-object member, preserving flag-off identity.
//
// MAIN-CARRIER KEY (GIL-removal review round): GIL-off, m_mainVMLite is
// NEVER installed (A36 — every thread, the main one included, gets a
// per-(thread,VM) carrier from JSLock's TLS maps), so `lite ==
// vm.mainVMLite()` alone never matched and the banner's "the main carrier's
// stream IS the in-object one" claim was unsatisfiable. The gilOff main
// carrier is the MAIN THREAD's carrier — the ownerHasNoTlsDtor==true lite
// (A36 r32 registration clause; it also borrows &vm.clientHeap, F1B) — and
// it keeps the in-object member, matching VM::queueMicrotask/
// drainMicrotasks' identical re-key.
ALWAYS_INLINE VMLite* perLiteRealmRoutingLite(VM& vm)
{
    if (!vm.gilOff()) [[likely]]
        return nullptr;
    VMLite* lite = VMLite::currentIfExists();
    if (!lite || lite == vm.mainVMLite() || lite->ownerHasNoTlsDtor || lite->vm != &vm)
        return nullptr;
    return lite;
}

} // anonymous namespace

// Declared in JSGlobalObject.h; consumers call the ALWAYS_INLINE
// threadRegExpGlobalData() wrapper (RegExpGlobalDataInlines.h), which routes
// here only when gilOffWithProcessGate() is true (the AUD1.K2 consumer
// re-point; flag-off match paths stay call-free).
RegExpGlobalData& threadRegExpGlobalDataSlow(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    VMLite* lite = perLiteRealmRoutingLite(vm);
    if (!lite) [[likely]]
        return globalObject->regExpGlobalData();

    auto& table = perLiteRealmTable();
    std::pair<JSGlobalObject*, VMLite*> key { globalObject, lite };
    {
        Locker locker { table.lock };
        auto it = table.map.find(key);
        if (it != table.map.end() && it->value->regExpGlobalData)
            return *it->value->regExpGlobalData;
    }
    // Alloc-outside: mirror the ctor-time seeding of the in-object stream
    // (init()'s cachedResult().record with the empty string) so $1-$9 read
    // as empty strings, not garbage, before this thread's first match.
    // jsEmptyString is smallStrings — allocation-free — but record() runs
    // barriers; none of it may run under the leaf lock.
    auto fresh = makeUniqueWithoutFastMallocCheck<RegExpGlobalData>();
    fresh->cachedResult().record(vm, globalObject, nullptr, jsEmptyString(vm), MatchResult(0, 0), /* oneCharacterMatch */ false);
    Locker locker { table.lock };
    auto result = table.map.ensure(key, [] { return makeUnique<PerLiteRealmState>(); });
    auto& state = *result.iterator->value;
    if (!state.regExpGlobalData)
        state.regExpGlobalData = WTF::move(fresh);
    return *state.regExpGlobalData; // A losing `fresh` dies at scope exit, after the locker releases.
}

#if USE(BUN_JSC_ADDITIONS)
InternalFieldTuple* threadAsyncContextData(JSGlobalObject*); // Self-declaration (U-T9/ALS1.3 lifts it beside m_asyncContextData's consumers).

// ALS1.3 STAGING GATE (U-T9): the per-lite reroute below stays DISABLED until
// the RESTORE-side brackets (JSMicrotask.cpp save/write/run/write-back) and
// the then() fast-path capture (JSPromisePrototype.cpp) — both TUs outside
// U-T9's owned file set — are re-pointed at this accessor. Enabling only the
// CAPTURE half would SPLIT the regime: a spawned thread's job-run brackets
// swap-write the SHARED in-object cursor while its registration-time captures
// read a never-bracket-written per-lite copy — the bracket value is missed
// entirely (ALS1.4 cannot pass) on top of the cross-thread clobber ALS1.3
// exists to fix. Until the restore slice lands, EVERY thread coherently uses
// the shared cursor (the landed pre-ALS1 regime; the documented clobber class
// remains, unamplified). Flip this constant WITH the restore-side re-point.
static constexpr bool perLiteAsyncContextCursorEnabled = false;

InternalFieldTuple* threadAsyncContextData(JSGlobalObject* globalObject)
{
    if constexpr (!perLiteAsyncContextCursorEnabled)
        return globalObject->m_asyncContextData.get();

    VM& vm = globalObject->vm();
    VMLite* lite = perLiteRealmRoutingLite(vm);
    if (!lite) [[likely]]
        return globalObject->m_asyncContextData.get();

    auto& table = perLiteRealmTable();
    std::pair<JSGlobalObject*, VMLite*> key { globalObject, lite };
    {
        Locker locker { table.lock };
        auto it = table.map.find(key);
        if (it != table.map.end() && it->value->asyncContextData)
            return it->value->asyncContextData.get();
    }
    // A fresh thread starts with the EMPTY async context (undefined cursor)
    // — ALS1.3 per-lite semantics. Allocated outside the leaf lock; kept
    // alive across the gap by this frame's conservative root.
    InternalFieldTuple* tuple = InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), jsUndefined(), jsUndefined());
    Locker locker { table.lock };
    auto result = table.map.ensure(key, [] { return makeUnique<PerLiteRealmState>(); });
    auto& state = *result.iterator->value;
    if (!state.asyncContextData)
        state.asyncContextData.set(vm, globalObject, tuple); // Loser's tuple is unreferenced garbage; the GC reclaims it.
    return state.asyncContextData.get();
}
#endif // USE(BUN_JSC_ADDITIONS)

// Lite-teardown half of the ~VM walk (K4 binding consequence 3): called from
// ~VMLite for EVERY lite (all teardown paths funnel there). Entries are
// detached under the leaf lock and destroyed after release.
void purgePerLiteRealmStateForLite(VMLite& lite)
{
    auto& table = perLiteRealmTable();
    Vector<std::unique_ptr<PerLiteRealmState>, 4> doomed;
    {
        Locker locker { table.lock };
        table.map.removeIf([&](auto& entry) {
            if (entry.key.second != &lite)
                return false;
            doomed.append(WTF::move(entry.value));
            return true;
        });
    }
    // `doomed` dies here, outside the lock (WS1.2 destroy-after-release shape).
}

// Global-death half: a global can die (GC sweep) while its VM and lites live
// on; its per-lite entries must not outlive it (their barriers name it as
// owner and visitChildren stops visiting them).
static void purgePerLiteRealmStateForGlobal(JSGlobalObject* globalObject)
{
    auto& table = perLiteRealmTable();
    Vector<std::unique_ptr<PerLiteRealmState>, 4> doomed;
    {
        Locker locker { table.lock };
        table.map.removeIf([&](auto& entry) {
            if (entry.key.first != globalObject)
                return false;
            doomed.append(WTF::move(entry.value));
            return true;
        });
    }
}

JSGlobalObject::~JSGlobalObject()
{
    // UNGIL §K.1 (U-T8b): drop this global's per-lite realm duplicates
    // before the member they shadow (m_regExpGlobalData) is torn down.
    purgePerLiteRealmStateForGlobal(this);

    clearWeakTickets();
#if ENABLE(REMOTE_INSPECTOR)
    protect(inspectorController())->globalObjectDestroyed();
    m_inspectorDebuggable->globalObjectDestroyed();
#endif

    if (m_debugger)
        m_debugger->detach(this, Debugger::GlobalObjectIsDestructing);
}

void JSGlobalObject::destroy(JSCell* cell)
{
    static_cast<JSGlobalObject*>(cell)->JSGlobalObject::~JSGlobalObject();
}

void JSGlobalObject::setGlobalThis(VM& vm, JSObject* globalThis)
{
    // UNGIL annex K4 §VIII.9 (U-T8b): m_globalThis is immutable-after-init —
    // written by finishCreation/resetPrototype before the global is shared
    // across threads. Debug fail-stop on a write after the VM's first
    // cross-thread entry (no-op flag-off/GIL-on and in release builds).
    jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(&vm);
    m_globalThis.set(vm, this, globalThis);
}

static GetterSetter* getGetterById(JSGlobalObject* globalObject, JSObject* base, const Identifier& ident)
{
    VM& vm = globalObject->vm();
    JSValue baseValue = JSValue(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::VMInquiry, &vm);
    baseValue.getPropertySlot(globalObject, ident, slot);
    return uncheckedDowncast<GetterSetter>(slot.getPureResult());
}

static ObjectPropertyCondition setupAdaptiveWatchpoint(JSGlobalObject* globalObject, JSObject* base, const Identifier& ident)
{
    // Performing these gets should not throw.
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferScope(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool result = base->getOwnPropertySlot(base, globalObject, ident, slot);
    ASSERT_UNUSED(result, result);
    catchScope.assertNoException();
    RELEASE_ASSERT(slot.isCacheableValue() || slot.isCacheableGetter());
    JSValue functionValue = slot.isCacheableValue() ? slot.getValue(globalObject, ident) : slot.getterSetter();
    catchScope.assertNoException();
    ASSERT(is<JSFunction>(functionValue) || is<GetterSetter>(functionValue));

    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, base, ident.impl());
    RELEASE_ASSERT(condition.requiredValue() == functionValue);

    bool isWatchable = condition.isWatchable(PropertyCondition::EnsureWatchability);
    RELEASE_ASSERT(isWatchable); // We allow this to install the necessary watchpoints.

    return condition;
}

static ObjectPropertyCondition setupAbsenceAdaptiveWatchpoint(JSGlobalObject* globalObject, JSObject* base, PropertyName propertyName, JSObject* prototype)
{
    // Performing these gets should not throw.
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferScope(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool result = base->getOwnPropertySlot(base, globalObject, propertyName, slot);
    RELEASE_ASSERT(!result);
    catchScope.assertNoException();
    RELEASE_ASSERT(slot.isUnset());
    RELEASE_ASSERT(base->getPrototypeDirect() == (prototype ? JSValue(prototype) : jsNull()));
    ObjectPropertyCondition condition = ObjectPropertyCondition::absence(vm, globalObject, base, propertyName.uid(), prototype);

    bool isWatchable = condition.isWatchable(PropertyCondition::EnsureWatchability);
    RELEASE_ASSERT(isWatchable); // We allow this to install the necessary watchpoints.

    return condition;
}

template<ErrorType errorType>
void JSGlobalObject::initializeErrorConstructor(LazyClassStructure::Initializer& init)
{
    init.setPrototype(NativeErrorPrototype::create(init.vm, NativeErrorPrototype::createStructure(init.vm, this, m_errorStructure.prototype(this)), errorTypeName(errorType)));
    init.setStructure(ErrorInstance::createStructure(init.vm, this, init.prototype));
    init.setConstructor(NativeErrorConstructor<errorType>::create(init.vm, NativeErrorConstructor<errorType>::createStructure(init.vm, this, m_errorStructure.constructor(this)), uncheckedDowncast<NativeErrorPrototype>(init.prototype)));
}

void JSGlobalObject::initializeAggregateErrorConstructor(LazyClassStructure::Initializer& init)
{
    init.setPrototype(AggregateErrorPrototype::create(init.vm, AggregateErrorPrototype::createStructure(init.vm, this, m_errorStructure.prototype(this))));
    init.setStructure(ErrorInstance::createStructure(init.vm, this, init.prototype));
    init.setConstructor(AggregateErrorConstructor::create(init.vm, AggregateErrorConstructor::createStructure(init.vm, this, m_errorStructure.constructor(this)), uncheckedDowncast<AggregateErrorPrototype>(init.prototype)));
}

void JSGlobalObject::initializeSuppressedErrorConstructor(LazyClassStructure::Initializer& init)
{
    init.setPrototype(SuppressedErrorPrototype::create(init.vm, SuppressedErrorPrototype::createStructure(init.vm, this, m_errorStructure.prototype(this))));
    init.setStructure(ErrorInstance::createStructure(init.vm, this, init.prototype));
    init.setConstructor(SuppressedErrorConstructor::create(init.vm, SuppressedErrorConstructor::createStructure(init.vm, this, m_errorStructure.constructor(this)), uncheckedDowncast<SuppressedErrorPrototype>(init.prototype)));
}

SUPPRESS_ASAN inline void JSGlobalObject::initStaticGlobals(VM& vm)
{
    GlobalPropertyInfo staticGlobals[] = {
        GlobalPropertyInfo(vm.propertyNames->NaN, jsNaN(), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly),
        GlobalPropertyInfo(vm.propertyNames->Infinity, jsNumber(std::numeric_limits<double>::infinity()), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly),
        GlobalPropertyInfo(vm.propertyNames->undefinedKeyword, jsUndefined(), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly),
#if ASSERT_ENABLED
        GlobalPropertyInfo(vm.propertyNames->builtinNames().assertPrivateName(), JSFunction::create(vm, this, 1, String(), assertCall, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly),
#endif
    };
    addStaticGlobals(staticGlobals);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

void JSGlobalObject::init(VM& vm)
{
    ASSERT(vm.trapsForCurrentThread().isDeferringTermination()); // Per-thread deferral keying (DeferTermination.h).
    ASSERT(vm.currentThreadIsHoldingAPILock());
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    convertToDictionary(vm);

    m_debugger = nullptr;
    updateCanFastQueueMicrotask();

#if ENABLE(REMOTE_INSPECTOR)
    m_inspectorController = makeUnique<Inspector::JSGlobalObjectInspectorController>(*this);
    m_inspectorDebuggable = JSGlobalObjectDebuggable::create(*this);
    m_inspectorDebuggable->init();
    m_consoleClient = protect(inspectorController())->consoleClient().get();
#endif

    m_functionPrototype.set(vm, this, FunctionPrototype::create(vm, FunctionPrototype::createStructure(vm, this, jsNull()))); // The real prototype will be set once ObjectPrototype is created.
    m_calleeStructure.set(vm, this, JSCallee::createStructure(vm, this, jsNull()));

    m_globalLexicalEnvironment.set(vm, this, JSGlobalLexicalEnvironment::create(vm, JSGlobalLexicalEnvironment::createStructure(vm, this), this));

    // Need to create the callee structure (above) before creating the callee.
    JSCallee* globalCallee = JSCallee::create(vm, this, globalScope());
    m_globalCallee.set(vm, this, globalCallee);

    JSCallee* evalCallee = JSCallee::create(vm, this, globalScope());
    m_evalCallee.set(vm, this, evalCallee);

    m_zombieFrameCallee.set(vm, this, JSCallee::create(vm, this, globalScope()));

    m_hostFunctionStructure.set(vm, this, JSFunction::createStructure(vm, this, m_functionPrototype.get()));

    auto initFunctionStructures = [&] (FunctionStructures& structures) {
        structures.strictFunctionStructure.set(vm, this, JSStrictFunction::createStructure(vm, this, m_functionPrototype.get()));
        structures.strictMethodStructure.set(vm, this, JSStrictFunction::createStructure(vm, this, m_functionPrototype.get()));
        structures.sloppyFunctionStructure.set(vm, this, JSSloppyFunction::createStructure(vm, this, m_functionPrototype.get()));
        structures.sloppyMethodStructure.set(vm, this, JSSloppyFunction::createStructure(vm, this, m_functionPrototype.get()));
        structures.arrowFunctionStructure.set(vm, this, JSArrowFunction::createStructure(vm, this, m_functionPrototype.get()));
    };
    initFunctionStructures(m_builtinFunctions);
    initFunctionStructures(m_ordinaryFunctions);
    m_boundFunctionStructure.set(vm, this, JSBoundFunction::createStructure(vm, this, m_functionPrototype.get()));

    m_customGetterFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCustomGetterFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_customSetterFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCustomSetterFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_nativeStdFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSNativeStdFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_remoteFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSRemoteFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    JSFunction* callFunction = nullptr;
    JSFunction* applyFunction = nullptr;
    JSFunction* hasInstanceSymbolFunction = nullptr;
    m_functionPrototype->addFunctionProperties(vm, this, &callFunction, &applyFunction, &hasInstanceSymbolFunction);
    m_objectProtoToStringFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, init.vm.propertyNames->toString.string(), objectProtoFuncToString, ImplementationVisibility::Public, ObjectToStringIntrinsic));
        });
    m_arrayProtoToStringFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, init.vm.propertyNames->toString.string(), arrayProtoFuncToString, ImplementationVisibility::Public));
        });
    m_arrayProtoValuesFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, init.vm.propertyNames->builtinNames().valuesPublicName().string(), arrayProtoFuncValues, ImplementationVisibility::Public, ArrayValuesIntrinsic));
        });
    m_mapProtoEntriesFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, init.vm.propertyNames->builtinNames().entriesPublicName().string(), mapProtoFuncEntries, ImplementationVisibility::Public, JSMapEntriesIntrinsic));
        });
    m_setProtoValuesFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, init.vm.propertyNames->builtinNames().valuesPublicName().string(), setProtoFuncValues, ImplementationVisibility::Public, JSSetValuesIntrinsic));
        });
    m_stringProtoSymbolIteratorFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "[Symbol.iterator]"_s, stringProtoFuncIterator, ImplementationVisibility::Public, JSStringIteratorIntrinsic));
        });

    m_iteratorProtoSymbolIteratorFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "[Symbol.iterator]"_s, iteratorProtoFuncIterator, ImplementationVisibility::Public, IteratorIntrinsic));
        });

    m_numberProtoToStringFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, init.vm.propertyNames->toString.string(), numberProtoFuncToString, ImplementationVisibility::Public, NumberPrototypeToStringIntrinsic));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::stringSubstring)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "substring"_s, stringProtoFuncSubstring, ImplementationVisibility::Public, StringPrototypeSubstringIntrinsic));
        });

    m_functionProtoHasInstanceSymbolFunction.set(vm, this, hasInstanceSymbolFunction);
    m_nullGetterFunction.set(vm, this, NullGetterFunction::create(vm, NullGetterFunction::createStructure(vm, this, m_functionPrototype.get())));
    Structure* nullSetterFunctionStructure = NullSetterFunction::createStructure(vm, this, m_functionPrototype.get());
    m_nullSetterFunction.set(vm, this, NullSetterFunction::create(vm, nullSetterFunctionStructure, ECMAMode::sloppy()));
    m_nullSetterStrictFunction.set(vm, this, NullSetterFunction::create(vm, nullSetterFunctionStructure, ECMAMode::strict()));
    m_objectPrototype.set(vm, this, ObjectPrototype::create(vm, this, ObjectPrototype::createStructure(vm, this, jsNull())));
    // We have to manually set this here because we make it a prototype without transition below.
    m_objectPrototype.get()->didBecomePrototype(vm);
    GetterSetter* protoAccessor = GetterSetter::create(vm, this,
        JSFunction::create(vm, this, 0, makeString("get "_s, vm.propertyNames->underscoreProto.string()), globalFuncProtoGetter, ImplementationVisibility::Public, UnderscoreProtoIntrinsic),
        JSFunction::create(vm, this, 0, makeString("set "_s, vm.propertyNames->underscoreProto.string()), globalFuncProtoSetter, ImplementationVisibility::Public));
    m_objectPrototype->putDirectNonIndexAccessorWithoutTransition(vm, vm.propertyNames->underscoreProto, protoAccessor, PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
    m_functionPrototype->structure()->setPrototypeWithoutTransition(vm, m_objectPrototype.get());
    m_objectStructureForObjectConstructor.set(vm, this, m_structureCache.emptyObjectStructureForPrototype(this, m_objectPrototype.get(), JSFinalObject::defaultInlineCapacity));
    m_objectProtoValueOfFunction.set(vm, this, uncheckedDowncast<JSFunction>(objectPrototype()->getDirect(vm, vm.propertyNames->valueOf)));

    JS_GLOBAL_OBJECT_ADDITIONS_3;

    m_arraySpeciesGetterSetter.set(vm, this, GetterSetter::create(vm, this, JSFunction::create(vm, this, 0, "get [Symbol.species]"_s, globalFuncSpeciesGetter, ImplementationVisibility::Public, SpeciesGetterIntrinsic), nullptr));
    m_typedArraySpeciesGetterSetter.set(vm, this, GetterSetter::create(vm, this, JSFunction::create(vm, this, 0, "get [Symbol.species]"_s, globalFuncSpeciesGetter, ImplementationVisibility::Public, SpeciesGetterIntrinsic), nullptr));
    m_arrayBufferSpeciesGetterSetter.set(vm, this, GetterSetter::create(vm, this, JSFunction::create(vm, this, 0, "get [Symbol.species]"_s, globalFuncSpeciesGetter, ImplementationVisibility::Public, SpeciesGetterIntrinsic), nullptr));
    m_sharedArrayBufferSpeciesGetterSetter.set(vm, this, GetterSetter::create(vm, this, JSFunction::create(vm, this, 0, "get [Symbol.species]"_s, globalFuncSpeciesGetter, ImplementationVisibility::Public, SpeciesGetterIntrinsic), nullptr));
    m_promiseSpeciesGetterSetter.set(vm, this, GetterSetter::create(vm, this, JSFunction::create(vm, this, 0, "get [Symbol.species]"_s, globalFuncSpeciesGetter, ImplementationVisibility::Public, SpeciesGetterIntrinsic), nullptr));

    m_throwTypeErrorArgumentsCalleeGetterSetter.initLater(
        [] (const Initializer<GetterSetter>& init) {
            JSFunction* thrower = JSFunction::create(init.vm, init.owner, 0, emptyString(), globalFuncThrowTypeErrorArgumentsCalleeAndCaller, ImplementationVisibility::Public);
            thrower->freeze(init.vm);
            init.set(GetterSetter::create(init.vm, init.owner, thrower, thrower));
        });
    m_typedArrayProto.initLater(
        [] (const Initializer<JSTypedArrayViewPrototype>& init) {
            init.set(JSTypedArrayViewPrototype::create(init.vm, init.owner, JSTypedArrayViewPrototype::createStructure(init.vm, init.owner, init.owner->m_objectPrototype.get())));

            // Make sure that the constructor gets initialized, too.
            init.owner->m_typedArraySuperConstructor.get(init.owner);
        });
    m_typedArraySuperConstructor.initLater(
        [] (const Initializer<JSTypedArrayViewConstructor>& init) {
            JSTypedArrayViewPrototype* prototype = init.owner->m_typedArrayProto.get(init.owner);
            JSTypedArrayViewConstructor* constructor = JSTypedArrayViewConstructor::create(init.vm, init.owner, JSTypedArrayViewConstructor::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()), prototype);
            prototype->putDirectWithoutTransition(init.vm, init.vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
            init.set(constructor);
        });

#define INIT_TYPED_ARRAY_LATER(type) \
    m_typedArray ## type.initLater( \
        [] (LazyClassStructure::Initializer& init) { \
            init.setPrototype(JS ## type ## ArrayPrototype::create(init.vm, init.global, JS ## type ## ArrayPrototype::createStructure(init.vm, init.global, init.global->m_typedArrayProto.get(init.global)))); \
            init.setStructure(JS ## type ## Array::createStructure(init.vm, init.global, init.prototype)); \
            init.setConstructor(JS ## type ## ArrayConstructor::create(init.vm, init.global, JS ## type ## ArrayConstructor::createStructure(init.vm, init.global, init.global->m_typedArraySuperConstructor.get(init.global)), init.prototype, #type "Array"_s)); \
            init.global->typedArrayStructure(Type##type, /* isResizableOrGrowableShared */ true); /* Initialize resizable Structure too */ \
        }); \
    m_resizableOrGrowableSharedTypedArray ## type ## Structure.initLater( \
        [] (const Initializer<Structure>& init) { \
            init.set(JSResizableOrGrowableShared ## type ## Array::createStructure(init.vm, init.owner, init.owner->typedArrayPrototype(Type##type))); \
            init.owner->typedArrayStructure(Type##type, /* isResizableOrGrowableShared */ false); /* Initialize non-resizable Structure too */ \
        }); \
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::type##Array)].initLater([](const Initializer<JSCell>& init) { \
            init.set(init.owner->typedArrayConstructor(TypedArrayType::Type##type)); \
        });
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(INIT_TYPED_ARRAY_LATER)
#undef INIT_TYPED_ARRAY_LATER

    m_typedArrayDataView.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.setPrototype(JSDataViewPrototype::create(init.vm, init.global, JSDataViewPrototype::createStructure(init.vm, init.global, init.global->m_objectPrototype.get())));
            init.setStructure(JSDataView::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(JSDataViewConstructor::create(init.vm, init.global, JSDataViewConstructor::createStructure(init.vm, init.global, init.global->m_functionPrototype.get()), init.prototype, "DataView"_s));
            init.global->typedArrayStructure(TypeDataView, /* isResizableOrGrowableShared */ true); /* Initialize resizable Structure too */
        });
    m_resizableOrGrowableSharedTypedArrayDataViewStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSResizableOrGrowableSharedDataView::createStructure(init.vm, init.owner, init.owner->typedArrayPrototype(TypeDataView)));
            init.owner->typedArrayStructure(TypeDataView, /* isResizableOrGrowableShared */ false); /* Initialize non-resizable Structure too */
        });

    m_lexicalEnvironmentStructure.set(vm, this, JSLexicalEnvironment::createStructure(vm, this));
    m_moduleEnvironmentStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSModuleEnvironment::createStructure(init.vm, init.owner));
        });
    m_strictEvalActivationStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(StrictEvalActivation::createStructure(init.vm, init.owner, jsNull()));
        });
    m_debuggerScopeStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(DebuggerScope::createStructure(init.vm, init.owner));
        });
    m_withScopeStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSWithScope::createStructure(init.vm, init.owner, jsNull()));
        });

    m_nullPrototypeObjectStructure.set(vm, this, JSFinalObject::createStructure(vm, this, jsNull(), JSFinalObject::defaultInlineCapacity));

    m_callbackFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCallbackFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_directArgumentsStructure.set(vm, this, DirectArguments::createStructure(vm, this, m_objectPrototype.get()));
    m_scopedArgumentsStructure.set(vm, this, ScopedArguments::createStructure(vm, this, m_objectPrototype.get()));
    m_clonedArgumentsStructure.set(vm, this, ClonedArguments::createStructure(vm, this, m_objectPrototype.get()));
    m_callbackConstructorStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCallbackConstructor::createStructure(init.vm, init.owner, init.owner->m_objectPrototype.get()));
        });
    m_callbackObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCallbackObject<JSNonFinalObject>::createStructure(init.vm, init.owner, init.owner->m_objectPrototype.get()));
        });
    m_rawJSONObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSRawJSONObject::createStructure(init.vm, init.owner, jsNull()));
        });

#if JSC_OBJC_API_ENABLED
    m_objcCallbackFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(ObjCCallbackFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_objcWrapperObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCallbackObject<JSAPIWrapperObject>::createStructure(init.vm, init.owner, init.owner->m_objectPrototype.get()));
        });
#endif
#ifdef JSC_GLIB_API_ENABLED
    m_glibCallbackFunctionStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCCallbackFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });
    m_glibWrapperObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSCallbackObject<JSAPIWrapperObject>::createStructure(init.vm, init.owner, init.owner->m_objectPrototype.get()));
        });
#endif
    m_arrayPrototype.set(vm, this, ArrayPrototype::create(vm, this, ArrayPrototype::createStructure(vm, this, m_objectPrototype.get())));

    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(UndecidedShape)].set(vm, this, JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithUndecided));
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(Int32Shape)].set(vm, this, JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithInt32));

    Structure* arrayWithContiguousStructure = JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithContiguous);
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(DoubleShape)].set(vm, this,
        Options::allowDoubleShape() ? JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithDouble) : arrayWithContiguousStructure);
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(ContiguousShape)].set(vm, this, arrayWithContiguousStructure);

    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(ArrayStorageShape)].set(vm, this, JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithArrayStorage));
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(SlowPutArrayStorageShape)].set(vm, this, JSArray::createStructure(vm, this, m_arrayPrototype.get(), ArrayWithSlowPutArrayStorage));
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(CopyOnWriteArrayWithInt32)].set(vm, this, JSArray::createStructure(vm, this, m_arrayPrototype.get(), CopyOnWriteArrayWithInt32));

    Structure* copyOnWriteArrayWithContiguous = JSArray::createStructure(vm, this, m_arrayPrototype.get(), CopyOnWriteArrayWithContiguous);
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(CopyOnWriteArrayWithDouble)].set(vm, this,
        Options::allowDoubleShape() ? JSArray::createStructure(vm, this, m_arrayPrototype.get(), CopyOnWriteArrayWithDouble) : copyOnWriteArrayWithContiguous);
    m_originalArrayStructureForIndexingShape[arrayIndexFromIndexingType(CopyOnWriteArrayWithContiguous)].set(vm, this, copyOnWriteArrayWithContiguous);

    for (unsigned i = 0; i < NumberOfArrayIndexingModes; ++i)
        m_arrayStructureForIndexingShapeDuringAllocation[i] = m_originalArrayStructureForIndexingShape[i];

    m_shadowRealmPrototype.set(vm, this, ShadowRealmPrototype::create(vm, ShadowRealmPrototype::createStructure(vm, this, m_objectPrototype.get())));
    m_shadowRealmObjectStructure.set(vm, this, ShadowRealmObject::createStructure(vm, this, m_shadowRealmPrototype.get()));

    m_regExpPrototype.set(vm, this, RegExpPrototype::create(vm, this, RegExpPrototype::createStructure(vm, this, m_objectPrototype.get())));
    m_regExpStructure.set(vm, this, RegExpObject::createStructure(vm, this, m_regExpPrototype.get()));
    m_regExpMatchesArrayStructure.set(vm, this, createRegExpMatchesArrayStructure(vm, this));
    m_regExpMatchesArrayWithIndicesStructure.set(vm, this, createRegExpMatchesArrayWithIndicesStructure(vm, this));
    m_regExpMatchesIndicesArrayStructure.set(vm, this, createRegExpMatchesIndicesArrayStructure(vm, this));

    m_trustedScriptStructure.setMayBeNull(vm, this, globalObjectMethodTable()->trustedScriptStructure(this));

    m_moduleRecordStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSModuleRecord::createStructure(init.vm, init.owner, jsNull()));
        });
    m_syntheticModuleRecordStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(SyntheticModuleRecord::createStructure(init.vm, init.owner, jsNull()));
        });
    m_moduleNamespaceObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(JSModuleNamespaceObject::createStructure(init.vm, init.owner, jsNull()));
        });
    m_proxyObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            bool isCallable = false;
            init.set(ProxyObject::createStructure(init.vm, init.owner, jsNull(), isCallable));
        });
    m_callableProxyObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            bool isCallable = true;
            init.set(ProxyObject::createStructure(init.vm, init.owner, jsNull(), isCallable));
        });
    m_proxyRevokeStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(ProxyRevoke::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
        });

    m_parseIntFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, init.vm.propertyNames->parseInt.string(), globalFuncParseInt, ImplementationVisibility::Public, ParseIntIntrinsic));
        });
    m_parseFloatFunction.initLater(
        [] (const Initializer<JSFunction>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, init.vm.propertyNames->parseFloat.string(), globalFuncParseFloat, ImplementationVisibility::Public));
        });

    m_sharedArrayBufferStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.setPrototype(JSArrayBufferPrototype::create(init.vm, init.global, JSArrayBufferPrototype::createStructure(init.vm, init.global, init.global->m_objectPrototype.get()), ArrayBufferSharingMode::Shared));
            init.setStructure(JSArrayBuffer::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(JSSharedArrayBufferConstructor::create(init.vm, JSSharedArrayBufferConstructor::createStructure(init.vm, init.global, init.global->m_functionPrototype.get()), uncheckedDowncast<JSArrayBufferPrototype>(init.prototype)));
        });

    m_disposableStackStructure.initLater(
        [] (LazyClassStructure::Initializer& init) -> void {
            init.setPrototype(DisposableStackPrototype::create(init.vm, init.global, DisposableStackPrototype::createStructure(init.vm, init.global, init.global->m_objectPrototype.get())));
            init.setStructure(JSDisposableStack::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(DisposableStackConstructor::create(init.vm, init.global, DisposableStackConstructor::createStructure(init.vm, init.global, init.global->m_functionPrototype.get()), uncheckedDowncast<DisposableStackPrototype>(init.prototype)));
        });
    m_asyncDisposableStackStructure.initLater(
        [] (LazyClassStructure::Initializer& init) -> void {
            init.setPrototype(AsyncDisposableStackPrototype::create(init.vm, init.global, AsyncDisposableStackPrototype::createStructure(init.vm, init.global, init.global->m_objectPrototype.get())));
            init.setStructure(JSAsyncDisposableStack::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(AsyncDisposableStackConstructor::create(init.vm, init.global, AsyncDisposableStackConstructor::createStructure(init.vm, init.global, init.global->m_functionPrototype.get()), uncheckedDowncast<AsyncDisposableStackPrototype>(init.prototype)));
        });

    m_iteratorPrototype.set(vm, this, JSIteratorPrototype::create(vm, this, JSIteratorPrototype::createStructure(vm, this, m_objectPrototype.get())));

    m_iteratorStructure.set(vm, this, JSIterator::createStructure(vm, this, m_iteratorPrototype.get()));

    m_iteratorHelperPrototype.set(vm, this, JSIteratorHelperPrototype::create(vm, this, JSIteratorHelperPrototype::createStructure(vm, this, m_iteratorPrototype.get())));
    m_iteratorHelperStructure.set(vm, this, JSIteratorHelper::createStructure(vm, this, m_iteratorHelperPrototype.get()));

    m_asyncIteratorPrototype.set(vm, this, AsyncIteratorPrototype::create(vm, this, AsyncIteratorPrototype::createStructure(vm, this, m_objectPrototype.get())));

    m_generatorPrototype.set(vm, this, GeneratorPrototype::create(vm, this, GeneratorPrototype::createStructure(vm, this, m_iteratorPrototype.get())));
    m_asyncGeneratorPrototype.set(vm, this, AsyncGeneratorPrototype::create(vm, this, AsyncGeneratorPrototype::createStructure(vm, this, m_asyncIteratorPrototype.get())));

    auto* arrayIteratorPrototype = ArrayIteratorPrototype::create(vm, this, ArrayIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_arrayIteratorPrototype.set(vm, this, arrayIteratorPrototype);
    m_arrayIteratorStructure.set(vm, this, JSArrayIterator::createStructure(vm, this, arrayIteratorPrototype));

    auto* mapIteratorPrototype = MapIteratorPrototype::create(vm, this, MapIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_mapIteratorPrototype.set(vm, this, mapIteratorPrototype);
    m_mapIteratorStructure.set(vm, this, JSMapIterator::createStructure(vm, this, mapIteratorPrototype));

    auto* setIteratorPrototype = SetIteratorPrototype::create(vm, this, SetIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_setIteratorPrototype.set(vm, this, setIteratorPrototype);
    m_setIteratorStructure.set(vm, this, JSSetIterator::createStructure(vm, this, setIteratorPrototype));

    auto* wrapForValidIteratorPrototype = WrapForValidIteratorPrototype::create(vm, this, WrapForValidIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_wrapForValidIteratorStructure.set(vm, this, JSWrapForValidIterator::createStructure(vm, this, wrapForValidIteratorPrototype));

    auto* asyncFromSyncIteratorPrototype = AsyncFromSyncIteratorPrototype::create(vm, this, AsyncFromSyncIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_asyncFromSyncIteratorStructure.set(vm, this, JSAsyncFromSyncIterator::createStructure(vm, this, asyncFromSyncIteratorPrototype));

    auto* regExpStringIteratorPrototype = RegExpStringIteratorPrototype::create(vm, this, RegExpStringIteratorPrototype::createStructure(vm, this, m_iteratorPrototype.get()));
    m_regExpStringIteratorStructure.set(vm, this, JSRegExpStringIterator::createStructure(vm, this, regExpStringIteratorPrototype));

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::sentinelString)].set(vm, this, vm.smallStrings.sentinelString());

    JSFunction* defaultPromiseThen = JSFunction::create(vm, this, 2, vm.propertyNames->then.impl(), promiseProtoFuncThen, ImplementationVisibility::Public, PromisePrototypeThenIntrinsic);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::defaultPromiseThen)].set(vm, this, defaultPromiseThen);

#define CREATE_PROTOTYPE_FOR_SIMPLE_TYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) if (featureFlag) { \
        m_ ## lowerName ## Prototype.set(vm, this, capitalName##Prototype::create(vm, this, capitalName##Prototype::createStructure(vm, this, m_ ## prototypeBase ## Prototype.get()))); \
        m_ ## properName ## Structure.set(vm, this, instanceType::createStructure(vm, this, m_ ## lowerName ## Prototype.get())); \
    }

    FOR_EACH_SIMPLE_BUILTIN_TYPE(CREATE_PROTOTYPE_FOR_SIMPLE_TYPE)
    FOR_EACH_BUILTIN_DERIVED_ITERATOR_TYPE(CREATE_PROTOTYPE_FOR_SIMPLE_TYPE)

#undef CREATE_PROTOTYPE_FOR_SIMPLE_TYPE

#define CREATE_PROTOTYPE_FOR_LAZY_TYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) if (featureFlag) {  \
    m_ ## properName ## Structure.initLater(\
        [] (LazyClassStructure::Initializer& init) { \
            init.setPrototype(capitalName##Prototype::create(init.vm, init.global, capitalName##Prototype::createStructure(init.vm, init.global, init.global->m_ ## prototypeBase ## Prototype.get()))); \
            init.setStructure(instanceType::createStructure(init.vm, init.global, init.prototype)); \
            init.setConstructor(capitalName ## Constructor::create(init.vm, capitalName ## Constructor::createStructure(init.vm, init.global, init.global->m_functionPrototype.get()), uncheckedDowncast<capitalName ## Prototype>(init.prototype))); \
        }); \
    }

    FOR_EACH_LAZY_BUILTIN_TYPE(CREATE_PROTOTYPE_FOR_LAZY_TYPE)

#undef CREATE_PROTOTYPE_FOR_LAZY_TYPE

    // Constructors

    ObjectConstructor* objectConstructor = ObjectConstructor::create(vm, this, ObjectConstructor::createStructure(vm, this, m_functionPrototype.get()), m_objectPrototype.get());
    m_objectConstructor.set(vm, this, objectConstructor);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Object)].set(vm, this, objectConstructor);

    JSFunction* throwTypeErrorFunction = JSFunction::create(vm, this, 0, String(), globalFuncThrowTypeError, ImplementationVisibility::Public);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::throwTypeErrorFunction)].set(vm, this, throwTypeErrorFunction);

    FunctionConstructor* functionConstructor = FunctionConstructor::create(vm, FunctionConstructor::createStructure(vm, this, m_functionPrototype.get()), m_functionPrototype.get());
    m_functionConstructor.set(vm, this, functionConstructor);

    ArrayConstructor* arrayConstructor = ArrayConstructor::create(vm, this, ArrayConstructor::createStructure(vm, this, m_functionPrototype.get()), m_arrayPrototype.get());
    m_arrayConstructor.set(vm, this, arrayConstructor);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Array)].set(vm, this, arrayConstructor);

    ShadowRealmConstructor* shadowRealmConstructor = ShadowRealmConstructor::create(vm, ShadowRealmConstructor::createStructure(vm, this, m_functionPrototype.get()), m_shadowRealmPrototype.get());
    m_shadowRealmConstructor.set(vm, this, shadowRealmConstructor);

    RegExpConstructor* regExpConstructor = RegExpConstructor::create(vm, RegExpConstructor::createStructure(vm, this, m_functionPrototype.get()), m_regExpPrototype.get());
    m_regExpConstructor.set(vm, this, regExpConstructor);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::RegExp)].set(vm, this, regExpConstructor);
    m_regExpGlobalData.cachedResult().record(vm, this, nullptr, jsEmptyString(vm), MatchResult(0, 0), /*oneCharacterMatch */ false);

#define CREATE_CONSTRUCTOR_FOR_SIMPLE_TYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) \
capitalName ## Constructor* lowerName ## Constructor = featureFlag ? capitalName ## Constructor::create(vm, capitalName ## Constructor::createStructure(vm, this, m_functionPrototype.get()), m_ ## lowerName ## Prototype.get()) : nullptr; \
    if (featureFlag) \
        m_ ## lowerName ## Prototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, lowerName ## Constructor, static_cast<unsigned>(PropertyAttribute::DontEnum)); \

    FOR_EACH_SIMPLE_BUILTIN_TYPE(CREATE_CONSTRUCTOR_FOR_SIMPLE_TYPE)

#undef CREATE_CONSTRUCTOR_FOR_SIMPLE_TYPE

    m_promiseConstructor.set(vm, this, promiseConstructor);
    m_stringConstructor.set(vm, this, stringConstructor);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Promise)].set(vm, this, promiseConstructor);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::String)].set(vm, this, stringConstructor);

    m_evalErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::EvalError>(init);
        });
    m_rangeErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::RangeError>(init);
        });
    m_referenceErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::ReferenceError>(init);
        });
    m_syntaxErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::SyntaxError>(init);
        });
    m_typeErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::TypeError>(init);
        });
    m_URIErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeErrorConstructor<ErrorType::URIError>(init);
        });
    m_aggregateErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeAggregateErrorConstructor(init);
        });
    m_suppressedErrorStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.global->initializeSuppressedErrorConstructor(init);
        });

    m_generatorFunctionPrototype.set(vm, this, GeneratorFunctionPrototype::create(vm, GeneratorFunctionPrototype::createStructure(vm, this, m_functionPrototype.get())));
    GeneratorFunctionConstructor* generatorFunctionConstructor = GeneratorFunctionConstructor::create(vm, GeneratorFunctionConstructor::createStructure(vm, this, functionConstructor), m_generatorFunctionPrototype.get());
    m_generatorFunctionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, generatorFunctionConstructor, PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_generatorFunctionStructure.set(vm, this, JSGeneratorFunction::createStructure(vm, this, m_generatorFunctionPrototype.get()));

    m_generatorPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, m_generatorFunctionPrototype.get(), PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_generatorFunctionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->prototype, m_generatorPrototype.get(), PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_generatorStructure.set(vm, this, JSGenerator::createStructure(vm, this, m_generatorPrototype.get()));
    m_asyncFunctionGeneratorStructure.set(vm, this, JSAsyncFunctionGenerator::createStructure(vm, this, m_generatorPrototype.get()));

    m_asyncFunctionPrototype.set(vm, this, AsyncFunctionPrototype::create(vm, AsyncFunctionPrototype::createStructure(vm, this, m_functionPrototype.get())));
    AsyncFunctionConstructor* asyncFunctionConstructor = AsyncFunctionConstructor::create(vm, AsyncFunctionConstructor::createStructure(vm, this, functionConstructor), m_asyncFunctionPrototype.get());
    m_asyncFunctionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, asyncFunctionConstructor, PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_asyncFunctionStructure.set(vm, this, JSAsyncFunction::createStructure(vm, this, m_asyncFunctionPrototype.get()));
    m_functionWithFieldsStructure.set(vm, this, JSFunctionWithFields::createStructure(vm, this, m_functionPrototype.get()));

    m_asyncGeneratorFunctionPrototype.set(vm, this, AsyncGeneratorFunctionPrototype::create(vm, AsyncGeneratorFunctionPrototype::createStructure(vm, this, m_functionPrototype.get())));
    AsyncGeneratorFunctionConstructor* asyncGeneratorFunctionConstructor = AsyncGeneratorFunctionConstructor::create(vm, AsyncGeneratorFunctionConstructor::createStructure(vm, this, functionConstructor), m_asyncGeneratorFunctionPrototype.get());
    m_asyncGeneratorFunctionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, asyncGeneratorFunctionConstructor, PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_asyncGeneratorFunctionStructure.set(vm, this, JSAsyncGeneratorFunction::createStructure(vm, this, m_asyncGeneratorFunctionPrototype.get()));

    m_asyncGeneratorPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, m_asyncGeneratorFunctionPrototype.get(), PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_asyncGeneratorFunctionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->prototype, m_asyncGeneratorPrototype.get(), PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
    m_asyncGeneratorStructure.set(vm, this, JSAsyncGenerator::createStructure(vm, this, m_asyncGeneratorPrototype.get()));

    m_objectPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, objectConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    m_functionPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, functionConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    m_arrayPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, arrayConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    m_regExpPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, regExpConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    m_shadowRealmPrototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, shadowRealmConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));

    putDirectWithoutTransition(vm, vm.propertyNames->Object, objectConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, vm.propertyNames->Function, functionConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, vm.propertyNames->Array, arrayConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectWithoutTransition(vm, vm.propertyNames->RegExp, regExpConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));

    JSIteratorConstructor* iteratorConstructor = JSIteratorConstructor::create(vm, this, JSIteratorConstructor::createStructure(vm, this, m_functionPrototype.get()), m_iteratorPrototype.get());
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Iterator)].set(vm, this, iteratorConstructor);
    m_iteratorConstructor.set(vm, this, iteratorConstructor);
    putDirectWithoutTransition(vm, vm.propertyNames->Iterator, iteratorConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));

    if (Options::useSharedArrayBuffer())
        putDirectWithoutTransition(vm, vm.propertyNames->SharedArrayBuffer, m_sharedArrayBufferStructure.constructor(this), static_cast<unsigned>(PropertyAttribute::DontEnum));

    if (Options::useJSThreads()) {
        // Shared-memory Thread API (docs/threads/SPEC-api.md 9.2-2).
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "Thread"_s), createThreadProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "Lock"_s), createLockProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "Condition"_s), createConditionProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "ThreadLocal"_s), createThreadLocalProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "ConcurrentAccessError"_s), createConcurrentAccessErrorProperty(vm, this), static_cast<unsigned>(PropertyAttribute::DontEnum));

        // THREADS (AUD1.N3 / §K.3 LZ1 interim): the ClonedArguments and
        // DirectArguments slow paths consult these two lazy properties from
        // ANY thread (strict-callee poison accessor and @@iterator
        // materialization). Until the park-capable LZ1 waiter lands in
        // LazyPropertyInlines.h, LazyProperty::get() can return null to a
        // thread that catches a foreign mutator mid-first-touch — so force
        // the first touch HERE, on the owning thread, before this global can
        // escape to a Thread(). Every later get() is then a plain non-null
        // load (no wait loop, no livelock risk), and the values become
        // per-realm singletons every racing arguments materializer agrees on.
        m_arrayProtoValuesFunction.get(this);
        m_throwTypeErrorArgumentsCalleeGetterSetter.get(this);
    }

    if (Options::useExplicitResourceManagement()) {
        putDirectWithoutTransition(vm, vm.propertyNames->SuppressedError, m_suppressedErrorStructure.constructor(this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, vm.propertyNames->DisposableStack, m_disposableStackStructure.constructor(this), static_cast<unsigned>(PropertyAttribute::DontEnum));
        putDirectWithoutTransition(vm, vm.propertyNames->AsyncDisposableStack, m_asyncDisposableStackStructure.constructor(this), static_cast<unsigned>(PropertyAttribute::DontEnum));
    }

#define PUT_CONSTRUCTOR_FOR_SIMPLE_TYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) \
    if (featureFlag) \
        putDirectWithoutTransition(vm, vm.propertyNames-> jsName, lowerName ## Constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));


    FOR_EACH_SIMPLE_BUILTIN_TYPE_WITH_CONSTRUCTOR(PUT_CONSTRUCTOR_FOR_SIMPLE_TYPE)

#undef PUT_CONSTRUCTOR_FOR_SIMPLE_TYPE
    m_iteratorResultObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createIteratorResultObjectStructure(init.vm, *init.owner));
        });
    m_dataPropertyDescriptorObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createDataPropertyDescriptorObjectStructure(init.vm, *init.owner));
        });
    m_accessorPropertyDescriptorObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createAccessorPropertyDescriptorObjectStructure(init.vm, *init.owner));
        });
    m_promiseCapabilityObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createPromiseCapabilityObjectStructure(init.vm, *init.owner));
        });
    m_promiseAllSettledFulfilledResultStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createPromiseAllSettledFulfilledResultStructure(init.vm, *init.owner));
        });
    m_promiseAllSettledRejectedResultStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createPromiseAllSettledRejectedResultStructure(init.vm, *init.owner));
        });

    m_collatorStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlCollatorPrototype* collatorPrototype = IntlCollatorPrototype::create(init.vm, globalObject, IntlCollatorPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlCollator::createStructure(init.vm, globalObject, collatorPrototype));
        });
    m_displayNamesStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlDisplayNamesPrototype* displayNamesPrototype = IntlDisplayNamesPrototype::create(init.vm, IntlDisplayNamesPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlDisplayNames::createStructure(init.vm, globalObject, displayNamesPrototype));
        });
    m_durationFormatStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlDurationFormatPrototype* durationFormatPrototype = IntlDurationFormatPrototype::create(init.vm, IntlDurationFormatPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlDurationFormat::createStructure(init.vm, globalObject, durationFormatPrototype));
        });
    m_listFormatStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlListFormatPrototype* listFormatPrototype = IntlListFormatPrototype::create(init.vm, IntlListFormatPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlListFormat::createStructure(init.vm, globalObject, listFormatPrototype));
        });
    m_localeStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlLocalePrototype* localePrototype = IntlLocalePrototype::create(init.vm, IntlLocalePrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlLocale::createStructure(init.vm, globalObject, localePrototype));
        });
    m_pluralRulesStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlPluralRulesPrototype* pluralRulesPrototype = IntlPluralRulesPrototype::create(init.vm, globalObject, IntlPluralRulesPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlPluralRules::createStructure(init.vm, globalObject, pluralRulesPrototype));
        });
    m_relativeTimeFormatStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlRelativeTimeFormatPrototype* relativeTimeFormatPrototype = IntlRelativeTimeFormatPrototype::create(init.vm, IntlRelativeTimeFormatPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlRelativeTimeFormat::createStructure(init.vm, globalObject, relativeTimeFormatPrototype));
        });
    m_segmentIteratorStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlSegmentIteratorPrototype* segmentIteratorPrototype = IntlSegmentIteratorPrototype::create(init.vm, IntlSegmentIteratorPrototype::createStructure(init.vm, globalObject, globalObject->iteratorPrototype()));
            init.set(IntlSegmentIterator::createStructure(init.vm, globalObject, segmentIteratorPrototype));
        });
    m_segmenterStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlSegmenterPrototype* segmenterPrototype = IntlSegmenterPrototype::create(init.vm, IntlSegmenterPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlSegmenter::createStructure(init.vm, globalObject, segmenterPrototype));
        });
    m_segmentsStructure.initLater(
        [] (const Initializer<Structure>& init) {
            JSGlobalObject* globalObject = init.owner;
            IntlSegmentsPrototype* segmentsPrototype = IntlSegmentsPrototype::create(init.vm, globalObject, IntlSegmentsPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
            init.set(IntlSegments::createStructure(init.vm, globalObject, segmentsPrototype));
        });
    m_segmentDataObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createSegmentDataObjectStructure(init.vm, *init.owner));
        });
    m_segmentDataObjectWithIsWordLikeStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createSegmentDataObjectWithIsWordLikeStructure(init.vm, *init.owner));
        });
    m_intlPartObjectStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createIntlPartObjectStructure(init.vm, *init.owner));
        });
    m_intlPartObjectWithSourceStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createIntlPartObjectWithSourceStructure(init.vm, *init.owner));
        });
    m_intlPartObjectWithUnitStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createIntlPartObjectWithUnitStructure(init.vm, *init.owner));
        });
    m_intlPartObjectWithUnitAndSourceStructure.initLater(
        [] (const Initializer<Structure>& init) {
            init.set(createIntlPartObjectWithUnitAndSourceStructure(init.vm, *init.owner));
        });

    m_dateTimeFormatStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.setPrototype(IntlDateTimeFormatPrototype::create(init.vm, init.global, IntlDateTimeFormatPrototype::createStructure(init.vm, init.global, init.global->objectPrototype())));
            init.setStructure(IntlDateTimeFormat::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(IntlDateTimeFormatConstructor::create(init.vm, IntlDateTimeFormatConstructor::createStructure(init.vm, init.global, init.global->functionPrototype()), uncheckedDowncast<IntlDateTimeFormatPrototype>(init.prototype)));
        });
    m_numberFormatStructure.initLater(
        [] (LazyClassStructure::Initializer& init) {
            init.setPrototype(IntlNumberFormatPrototype::create(init.vm, init.global, IntlNumberFormatPrototype::createStructure(init.vm, init.global, init.global->objectPrototype())));
            init.setStructure(IntlNumberFormat::createStructure(init.vm, init.global, init.prototype));
            init.setConstructor(IntlNumberFormatConstructor::create(init.vm, IntlNumberFormatConstructor::createStructure(init.vm, init.global, init.global->functionPrototype()), uncheckedDowncast<IntlNumberFormatPrototype>(init.prototype)));
        });

    m_defaultCollator.initLater(
        [] (const Initializer<IntlCollator>& init) {
            JSGlobalObject* globalObject = init.owner;
            VM& vm = init.vm;
            auto scope = DECLARE_THROW_SCOPE(vm);
            IntlCollator* collator = IntlCollator::create(vm, globalObject->collatorStructure());
            collator->initializeCollator(globalObject, jsUndefined(), jsUndefined());
            RETURN_IF_EXCEPTION(scope, void());
            init.set(collator);
            globalObject->m_canDoASCIIUCADUCETLocaleCompare = collator->canDoASCIIUCADUCETComparison();
        });

    m_defaultDateTimeFormat.initLater(
        [] (const Initializer<IntlDateTimeFormat>& init) {
            JSGlobalObject* globalObject = init.owner;
            VM& vm = init.vm;
            auto scope = DECLARE_THROW_SCOPE(vm);
            auto* dateTimeFormat = IntlDateTimeFormat::create(vm, globalObject->dateTimeFormatStructure());
            dateTimeFormat->initializeDateTimeFormat(globalObject, jsUndefined(), jsUndefined(), IntlDateTimeFormat::RequiredComponent::Any, IntlDateTimeFormat::Defaults::All);
            RETURN_IF_EXCEPTION(scope, void());
            init.set(dateTimeFormat);
        });

    m_defaultDateFormat.initLater(
        [] (const Initializer<IntlDateTimeFormat>& init) {
            JSGlobalObject* globalObject = init.owner;
            VM& vm = init.vm;
            auto scope = DECLARE_THROW_SCOPE(vm);
            auto* dateTimeFormat = IntlDateTimeFormat::create(vm, globalObject->dateTimeFormatStructure());
            dateTimeFormat->initializeDateTimeFormat(globalObject, jsUndefined(), jsUndefined(), IntlDateTimeFormat::RequiredComponent::Date, IntlDateTimeFormat::Defaults::Date);
            RETURN_IF_EXCEPTION(scope, void());
            init.set(dateTimeFormat);
        });

    m_defaultTimeFormat.initLater(
        [] (const Initializer<IntlDateTimeFormat>& init) {
            JSGlobalObject* globalObject = init.owner;
            VM& vm = init.vm;
            auto scope = DECLARE_THROW_SCOPE(vm);
            auto* dateTimeFormat = IntlDateTimeFormat::create(vm, globalObject->dateTimeFormatStructure());
            dateTimeFormat->initializeDateTimeFormat(globalObject, jsUndefined(), jsUndefined(), IntlDateTimeFormat::RequiredComponent::Time, IntlDateTimeFormat::Defaults::Time);
            RETURN_IF_EXCEPTION(scope, void());
            init.set(dateTimeFormat);
        });

    m_defaultNumberFormat.initLater(
        [] (const Initializer<IntlNumberFormat>& init) {
            JSGlobalObject* globalObject = init.owner;
            VM& vm = init.vm;
            auto scope = DECLARE_THROW_SCOPE(vm);
            auto* numberFormat = IntlNumberFormat::create(vm, globalObject->numberFormatStructure());
            numberFormat->initializeNumberFormat(globalObject, jsUndefined(), jsUndefined());
            RETURN_IF_EXCEPTION(scope, void());
            init.set(numberFormat);
        });

    IntlObject* intl = IntlObject::create(vm, this, IntlObject::createStructure(vm, this, m_objectPrototype.get()));
    putDirectWithoutTransition(vm, vm.propertyNames->Intl, intl, static_cast<unsigned>(PropertyAttribute::DontEnum));

    if (Options::useTemporal()) {
        m_durationStructure.initLater(
            [] (const Initializer<Structure>& init) {
                JSGlobalObject* globalObject = init.owner;
                TemporalDurationPrototype* durationPrototype = TemporalDurationPrototype::create(init.vm, TemporalDurationPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalDuration::createStructure(init.vm, globalObject, durationPrototype));
            });

        m_instantStructure.initLater(
            [] (const Initializer<Structure>& init) {
                JSGlobalObject* globalObject = init.owner;
                TemporalInstantPrototype* instantPrototype = TemporalInstantPrototype::create(init.vm, TemporalInstantPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalInstant::createStructure(init.vm, globalObject, instantPrototype));
            });

        m_plainDateStructure.initLater(
            [] (const Initializer<Structure>& init) {
                auto* globalObject = init.owner;
                auto* plainDatePrototype = TemporalPlainDatePrototype::create(init.vm, globalObject, TemporalPlainDatePrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalPlainDate::createStructure(init.vm, globalObject, plainDatePrototype));
            });

        m_plainDateTimeStructure.initLater(
            [] (const Initializer<Structure>& init) {
                auto* globalObject = init.owner;
                auto* plainDateTimePrototype = TemporalPlainDateTimePrototype::create(init.vm, globalObject, TemporalPlainDateTimePrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalPlainDateTime::createStructure(init.vm, globalObject, plainDateTimePrototype));
            });

        m_plainMonthDayStructure.initLater(
            [] (const Initializer<Structure>& init) {
                auto* globalObject = init.owner;
                auto* plainMonthDayPrototype = TemporalPlainMonthDayPrototype::create(init.vm, globalObject, TemporalPlainMonthDayPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalPlainMonthDay::createStructure(init.vm, globalObject, plainMonthDayPrototype));
            });

        m_plainTimeStructure.initLater(
            [] (const Initializer<Structure>& init) {
                auto* globalObject = init.owner;
                auto* plainTimePrototype = TemporalPlainTimePrototype::create(init.vm, globalObject, TemporalPlainTimePrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalPlainTime::createStructure(init.vm, globalObject, plainTimePrototype));
            });

        m_plainYearMonthStructure.initLater(
            [] (const Initializer<Structure>& init) {
                auto* globalObject = init.owner;
                auto* plainYearMonthPrototype = TemporalPlainYearMonthPrototype::create(init.vm, globalObject, TemporalPlainYearMonthPrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalPlainYearMonth::createStructure(init.vm, globalObject, plainYearMonthPrototype));
            });

        m_timeZoneStructure.initLater(
            [] (const Initializer<Structure>& init) {
                JSGlobalObject* globalObject = init.owner;
                TemporalTimeZonePrototype* timeZonePrototype = TemporalTimeZonePrototype::create(init.vm, globalObject, TemporalTimeZonePrototype::createStructure(init.vm, globalObject, globalObject->objectPrototype()));
                init.set(TemporalTimeZone::createStructure(init.vm, globalObject, timeZonePrototype));
            });

        TemporalObject* temporal = TemporalObject::create(vm, TemporalObject::createStructure(vm, this));
        putDirectWithoutTransition(vm, vm.propertyNames->Temporal, temporal, static_cast<unsigned>(PropertyAttribute::DontEnum));
    }
    if (Options::useShadowRealm())
        putDirectWithoutTransition(vm, vm.propertyNames->ShadowRealm, shadowRealmConstructor, static_cast<unsigned>(PropertyAttribute::DontEnum));

    m_moduleLoader.initLater(
        [] (const Initializer<JSModuleLoader>& init) {
            auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(init.vm);
            init.set(JSModuleLoader::create(init.owner, init.vm));
            catchScope.releaseAssertNoException();
        });
    if (Options::exposeInternalModuleLoader())
        putDirectWithoutTransition(vm, vm.propertyNames->Loader, moduleLoader(), static_cast<unsigned>(PropertyAttribute::DontEnum));

    GetterSetter* regExpProtoFlagsGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->flags);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoFlagsGetter)].set(vm, this, regExpProtoFlagsGetter);
    GetterSetter* regExpProtoHasIndicesGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->hasIndices);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoHasIndicesGetter)].set(vm, this, regExpProtoHasIndicesGetter);
    GetterSetter* regExpProtoGlobalGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->global);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoGlobalGetter)].set(vm, this, regExpProtoGlobalGetter);
    GetterSetter* regExpProtoIgnoreCaseGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->ignoreCase);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoIgnoreCaseGetter)].set(vm, this, regExpProtoIgnoreCaseGetter);
    GetterSetter* regExpProtoMultilineGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->multiline);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoMultilineGetter)].set(vm, this, regExpProtoMultilineGetter);
    GetterSetter* regExpProtoSourceGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->source);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoSourceGetter)].set(vm, this, regExpProtoSourceGetter);
    GetterSetter* regExpProtoStickyGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->sticky);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoStickyGetter)].set(vm, this, regExpProtoStickyGetter);
    GetterSetter* regExpProtoUnicodeGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->unicode);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoUnicodeGetter)].set(vm, this, regExpProtoUnicodeGetter);
    GetterSetter* regExpProtoDotAllGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->dotAll);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoDotAllGetter)].set(vm, this, regExpProtoDotAllGetter);
    GetterSetter* regExpProtoUnicodeSetsGetter = getGetterById(this, m_regExpPrototype.get(), vm.propertyNames->unicodeSets);
    catchScope.assertNoException();
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpProtoUnicodeSetsGetter)].set(vm, this, regExpProtoUnicodeSetsGetter);
    JSFunction* regExpSymbolReplace = uncheckedDowncast<JSFunction>(m_regExpPrototype->getDirect(vm, vm.propertyNames->replaceSymbol));
    m_regExpProtoSymbolReplace.set(vm, this, regExpSymbolReplace);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpBuiltinExec)].set(vm, this, uncheckedDowncast<JSFunction>(m_regExpPrototype->getDirect(vm, vm.propertyNames->exec)));
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpPrototypeSymbolMatch)].set(vm, this, m_regExpPrototype->getDirect(vm, vm.propertyNames->matchSymbol).asCell());
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpPrototypeSymbolMatchAll)].set(vm, this, m_regExpPrototype->getDirect(vm, vm.propertyNames->matchAllSymbol).asCell());
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpPrototypeSymbolReplace)].set(vm, this, m_regExpPrototype->getDirect(vm, vm.propertyNames->replaceSymbol).asCell());

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isArray)].initLater([] (const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 1, "isArray"_s, arrayConstructorIsArray, ImplementationVisibility::Public, ArrayIsArrayIntrinsic));
    });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::callFunction)].set(vm, this, callFunction);
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::applyFunction)].set(vm, this, applyFunction);

    {
        JSValue hasOwnPropertyFunction = uncheckedDowncast<JSFunction>(objectPrototype()->get(this, vm.propertyNames->hasOwnProperty));
        catchScope.assertNoException();
        RELEASE_ASSERT(is<JSFunction>(hasOwnPropertyFunction));
        m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::hasOwnPropertyFunction)].set(vm, this, uncheckedDowncast<JSFunction>(hasOwnPropertyFunction));
    }

#define INIT_PRIVATE_GLOBAL(funcName, code) \
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::funcName)].initLater([] (const Initializer<JSCell>& init) { \
            JSGlobalObject* globalObject = init.owner; \
            init.set(JSFunction::create(init.vm, globalObject, code ## CodeGenerator(init.vm), globalObject)); \
        });
    JSC_FOREACH_BUILTIN_LINK_TIME_CONSTANT(INIT_PRIVATE_GLOBAL)
#undef INIT_PRIVATE_GLOBAL

    // AsyncFromSyncIterator Helpers
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::asyncFromSyncIteratorCreate)].initLater([](const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 2, "asyncFromSyncIteratorCreate"_s, asyncFromSyncIteratorPrivateFuncCreate, ImplementationVisibility::Private, AsyncFromSyncIteratorCreateIntrinsic));
    });

    // RegExpStringIteratorHelpers
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpStringIteratorCreate)].initLater([](const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 4, "regExpStringIteratorCreate"_s, regExpStringIteratorPrivateFuncCreate, ImplementationVisibility::Private, RegExpStringIteratorCreateIntrinsic));
    });

    // WrapForValidIterator Helpers
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::wrapForValidIteratorCreate)].initLater([](const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 2, "wrapForValidIteratorCreate"_s, wrapForValidIteratorPrivateFuncCreate, ImplementationVisibility::Private, WrapForValidIteratorCreateIntrinsic));
    });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::iteratorHelperCreate)].initLater([](const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 2, "iteratorHelperCreate"_s, iteratorHelperPrivateFuncCreate, ImplementationVisibility::Private, IteratorHelperCreateIntrinsic));
    });

    // Global object and function helpers.
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isFinite)].initLater([] (const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 1, "isFinite"_s, globalFuncIsFinite, ImplementationVisibility::Private, GlobalIsFiniteIntrinsic));
    });

    // Map and Set helpers.
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Set)].initLater([] (const Initializer<JSCell>& init) {
            init.set(init.owner->setConstructor());
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::Map)].initLater([] (const Initializer<JSCell>& init) {
            init.set(init.owner->mapConstructor());
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIterationNext)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIterationNext"_s, mapPrivateFuncMapIterationNext, ImplementationVisibility::Private, JSMapIterationNextIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIterationEntry)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIterationEntry"_s, mapPrivateFuncMapIterationEntry, ImplementationVisibility::Private, JSMapIterationEntryIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapStorage)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapStorage"_s, mapPrivateFuncMapStorage, ImplementationVisibility::Private, JSMapStorageIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIteratorNext)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIteratorNext"_s, mapIteratorPrivateFuncMapIteratorNext, ImplementationVisibility::Private, JSMapIteratorNextIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIteratorKey)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIteratorKey"_s, mapIteratorPrivateFuncMapIteratorKey, ImplementationVisibility::Private, JSMapIteratorKeyIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIteratorValue)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIteratorValue"_s, mapIteratorPrivateFuncMapIteratorValue, ImplementationVisibility::Private, JSMapIteratorValueIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIterationEntryKey)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIterationEntryKey"_s, mapPrivateFuncMapIterationEntryKey, ImplementationVisibility::Private, JSMapIterationEntryKeyIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::mapIterationEntryValue)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "mapIterationEntryValue"_s, mapPrivateFuncMapIterationEntryValue, ImplementationVisibility::Private, JSMapIterationEntryValueIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setIterationNext)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setIterationNext"_s, setPrivateFuncSetIterationNext, ImplementationVisibility::Private, JSSetIterationNextIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setIterationEntry)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setIterationEntry"_s, setPrivateFuncSetIterationEntry, ImplementationVisibility::Private, JSSetIterationEntryIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setIterationEntryKey)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setIterationEntryKey"_s, setPrivateFuncSetIterationEntryKey, ImplementationVisibility::Private, JSSetIterationEntryKeyIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setIteratorNext)].initLater([](const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setIteratorNext"_s, setIteratorPrivateFuncSetIteratorNext, ImplementationVisibility::Private, JSSetIteratorNextIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setIteratorKey)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setIteratorKey"_s, setIteratorPrivateFuncSetIteratorKey, ImplementationVisibility::Private, JSSetIteratorKeyIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setStorage)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "setStorage"_s, setPrivateFuncSetStorage, ImplementationVisibility::Private, JSSetStorageIntrinsic));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::importModule)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "importModule"_s, globalFuncImportModule, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::copyDataProperties)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "copyDataProperties"_s, globalFuncCopyDataProperties, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::cloneObject)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "cloneObject"_s, globalFuncCloneObject, ImplementationVisibility::Private));
        });
#if USE(BUN_JSC_ADDITIONS)
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::enqueueJob)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "enqueueJob"_s, enqueueJob, ImplementationVisibility::Public));
        });
#endif
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::resolvePromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "resolvePromise"_s, resolvePromise, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::rejectPromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "rejectPromise"_s, rejectPromise, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::fulfillPromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "fulfillPromise"_s, fulfillPromise, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::markPromiseAsHandled)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "markPromiseAsHandled"_s, markPromiseAsHandledHostFunction, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isPromiseStatePending)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "isPromiseStatePending"_s, isPromiseStatePending, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::claimGeneratorResume)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "claimGeneratorResume"_s, claimGeneratorResume, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::publishGeneratorResume)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "publishGeneratorResume"_s, publishGeneratorResume, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::resolvePromiseWithFirstResolvingFunctionCallCheck)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "resolvePromiseWithFirstResolvingFunctionCallCheck"_s, resolvePromiseWithFirstResolvingFunctionCallCheck, ImplementationVisibility::Private, ResolvePromiseWithFirstResolvingFunctionCallCheckIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::rejectPromiseWithFirstResolvingFunctionCallCheck)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "rejectPromiseWithFirstResolvingFunctionCallCheck"_s, rejectPromiseWithFirstResolvingFunctionCallCheck, ImplementationVisibility::Private, RejectPromiseWithFirstResolvingFunctionCallCheckIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::fulfillPromiseWithFirstResolvingFunctionCallCheck)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "fulfillPromiseWithFirstResolvingFunctionCallCheck"_s, fulfillPromiseWithFirstResolvingFunctionCallCheck, ImplementationVisibility::Private, FulfillPromiseWithFirstResolvingFunctionCallCheckIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::newResolvedPromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "newResolvedPromise"_s, newResolvedPromise, ImplementationVisibility::Private, NewResolvedPromiseIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::newRejectedPromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "newRejectedPromise"_s, newRejectedPromise, ImplementationVisibility::Private, NewRejectedPromiseIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::resolveWithInternalMicrotaskForAsyncAwait)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 3, "resolveWithInternalMicrotaskForAsyncAwait"_s, resolveWithInternalMicrotaskForAsyncAwait, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::asyncGeneratorQueueEnqueue)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 4, "asyncGeneratorQueueEnqueue"_s, asyncGeneratorQueueEnqueue, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::asyncGeneratorQueueDequeueResolve)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "asyncGeneratorQueueDequeueResolve"_s, asyncGeneratorQueueDequeueResolve, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::asyncGeneratorQueueDequeueReject)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "asyncGeneratorQueueDequeueReject"_s, asyncGeneratorQueueDequeueReject, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::driveAsyncFunction)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "driveAsyncFunction"_s, driveAsyncFunction, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::newHandledRejectedPromise)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "newHandledRejectedPromise"_s, newHandledRejectedPromise, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::promiseReturnUndefinedOnFulfilled)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "promiseReturnUndefinedOnFulfilled"_s, promiseReturnUndefinedOnFulfilled, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::promiseResolve)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "promiseResolve"_s, promiseResolve, ImplementationVisibility::Private, PromiseResolveIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::promiseReject)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "promiseReject"_s, promiseReject, ImplementationVisibility::Private, PromiseRejectIntrinsic));
        });
#if USE(BUN_JSC_ADDITIONS)
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::promiseResolveWithThen)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "promiseResolveWithThen"_s, promiseResolveWithThen, ImplementationVisibility::Private));
        });
#endif
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::performPromiseThen)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 4, "performPromiseThen"_s, performPromiseThen, ImplementationVisibility::Private));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::makeTypeError)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "makeTypeError"_s, globalFuncMakeTypeError, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::AggregateError)].initLater([] (const Initializer<JSCell>& init) {
            JSGlobalObject* globalObject = init.owner;
            init.set(globalObject->m_aggregateErrorStructure.constructor(globalObject));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::ReferenceError)].initLater([] (const Initializer<JSCell>& init) {
        JSGlobalObject* globalObject = init.owner;
        init.set(globalObject->m_referenceErrorStructure.constructor(globalObject));
    });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::SuppressedError)].initLater([] (const Initializer<JSCell>& init) {
        JSGlobalObject* globalObject = init.owner;
        init.set(globalObject->m_suppressedErrorStructure.constructor(globalObject));
    });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::DisposableStack)].initLater([] (const Initializer<JSCell>& init) {
        JSGlobalObject* globalObject = init.owner;
        init.set(globalObject->m_disposableStackStructure.constructor(globalObject));
    });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::AsyncDisposableStack)].initLater([] (const Initializer<JSCell>& init) {
        JSGlobalObject* globalObject = init.owner;
        init.set(globalObject->m_asyncDisposableStackStructure.constructor(globalObject));
    });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::typedArrayLength)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "typedArrayViewLength"_s, typedArrayViewPrivateFuncLength, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isTypedArrayView)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "typedArrayViewIsTypedArrayView"_s, typedArrayViewPrivateFuncIsTypedArrayView, ImplementationVisibility::Private, IsTypedArrayViewIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isSharedTypedArrayView)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "typedArrayViewIsSharedTypedArrayView"_s, typedArrayViewPrivateFuncIsSharedTypedArrayView, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isResizableOrGrowableSharedTypedArrayView)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "typedArrayViewPrivateFuncIsResizableOrGrowableSharedTypedArrayView"_s, typedArrayViewPrivateFuncIsResizableOrGrowableSharedTypedArrayView, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::typedArrayFromFast)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "typedArrayViewTypedArrayFromFast"_s, typedArrayViewPrivateFuncTypedArrayFromFast, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::arrayFromFastWithoutMapFn)].initLater([] (const Initializer<JSCell>& init) {
        init.set(JSFunction::create(init.vm, init.owner, 2, "arrayFromFastWithoutMapFn"_s, arrayConstructorPrivateFromFastWithoutMapFn, ImplementationVisibility::Private));
    });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isDetached)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "typedArrayViewIsDetached"_s, typedArrayViewPrivateFuncIsDetached, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::instanceOf)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "instanceOf"_s, objectPrivateFuncInstanceOf, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::BuiltinLog)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "BuiltinLog"_s, globalFuncBuiltinLog, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::BuiltinDescribe)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "BuiltinDescribe"_s, globalFuncBuiltinDescribe, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::min)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "min"_s, mathProtoFuncMin, ImplementationVisibility::Private, MinIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::repeatCharacter)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "repeatCharacter"_s, stringProtoFuncRepeatCharacter, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::importInRealm)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "importInRealm"_s, importInRealm, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::evalFunction)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, init.vm.propertyNames->eval.string(), globalFuncEval, ImplementationVisibility::Public));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::evalInRealm)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "evalInRealm"_s, evalInRealm, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::moveFunctionToRealm)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "moveFunctionToRealm"_s, moveFunctionToRealm, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::sameValue)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "is"_s, objectConstructorIs, ImplementationVisibility::Private, ObjectIsIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setPrototypeDirect)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "setPrototypeDirect"_s, globalFuncSetPrototypeDirect, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::setPrototypeDirectOrThrow)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "setPrototypeDirectOrThrow"_s, globalFuncSetPrototypeDirectOrThrow, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::toIntegerOrInfinity)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "toIntegerOrInfinity"_s, globalFuncToIntegerOrInfinity, ImplementationVisibility::Private, ToIntegerOrInfinityIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::toLength)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "toLength"_s, globalFuncToLength, ImplementationVisibility::Private, ToLengthIntrinsic));
        });

    // RegExp.prototype helpers.
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpCreate)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "esSpecRegExpCreate"_s, esSpecRegExpCreate, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isRegExp)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "esSpecIsRegExp"_s, esSpecIsRegExp, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpMatchFast)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "regExpMatchFast"_s, regExpProtoFuncMatchFast, ImplementationVisibility::Private, RegExpMatchFastIntrinsic));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::regExpSplitFast)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "regExpSplitFast"_s, regExpProtoFuncSplitFast, ImplementationVisibility::Private));
        });

    // String.prototype helpers.
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::stringIncludesInternal)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "stringIncludesInternal"_s, builtinStringIncludesInternal, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::stringIndexOfInternal)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "stringIndexOfInternal"_s, builtinStringIndexOfInternal, ImplementationVisibility::Private));
        });
    // Proxy helpers.
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::handleNegativeProxyHasTrapResult)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "handleNegativeProxyHasTrapResult"_s, globalFuncHandleNegativeProxyHasTrapResult, ImplementationVisibility::Private));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::handleProxyGetTrapResult)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 3, "handleProxyGetTrapResult"_s, globalFuncHandleProxyGetTrapResult, ImplementationVisibility::Private));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::handlePositiveProxySetTrapResult)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 3, "handlePositiveProxySetTrapResult"_s, globalFuncHandlePositiveProxySetTrapResult, ImplementationVisibility::Private));
        });

    // PrivateSymbols / PrivateNames
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::createPrivateSymbol)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "createPrivateSymbol"_s, createPrivateSymbol, ImplementationVisibility::Private));
        });

    // JSON helpers
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::jsonParse)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 1, "parse"_s, jsonParse, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::jsonStringify)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 2, "stringify"_s, jsonStringify, ImplementationVisibility::Private));
        });

    // ShadowRealms
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::createRemoteFunction)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "createRemoteFunction"_s, createRemoteFunction, ImplementationVisibility::Private));
        });
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::isRemoteFunction)].initLater([] (const Initializer<JSCell>& init) {
            init.set(JSFunction::create(init.vm, init.owner, 0, "isRemoteFunction"_s, isRemoteFunction, ImplementationVisibility::Private));
        });

    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::emptyPropertyNameEnumerator)].initLater([] (const Initializer<JSCell>& init) {
        init.set(init.vm.emptyPropertyNameEnumerator());
    });

#if USE(BUN_JSC_ADDITIONS)
    // Link Time Constant would be faster, but it seems OpGetInternalField does not expect having a linkTimeConstant,
    // so `@getInternalField(@asyncContext, 0)` will crash. If we can read/write to the internal field from JS in a better way, that could improve perf.
    // m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::asyncContext)].initLater([](const Initializer<JSCell>& init) {
    //     auto* globalObject = uncheckedDowncast<JSGlobalObject>(init.owner);
    //     init.set(AsyncContext::create(init.vm, AsyncContext::createStructure(init.vm, globalObject, globalObject->objectPrototype())));
    // });
    m_internalFieldTupleStructure.set(vm, this, InternalFieldTuple::createStructure(vm, this));

    InternalFieldTuple* asyncContext = InternalFieldTuple::create(vm, internalFieldTupleStructure(), jsUndefined(), jsUndefined());
    putDirectWithoutTransition(
        vm, vm.propertyNames->builtinNames().asyncContextPrivateName(),
        asyncContext, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    m_asyncContextData.set(vm, this, asyncContext);
#endif

    m_performProxyObjectHasFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectHas)));
    m_performProxyObjectHasByValFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectHasByVal)));
    m_performProxyObjectGetFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectGet)));
    m_performProxyObjectGetByValFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectGetByVal)));
    m_performProxyObjectSetStrictFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectSetStrict)));
    m_performProxyObjectSetSloppyFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectSetSloppy)));
    m_performProxyObjectSetByValStrictFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectSetByValStrict)));
    m_performProxyObjectSetByValSloppyFunction.set(vm, this, uncheckedDowncast<JSFunction>(linkTimeConstant(LinkTimeConstant::performProxyObjectSetByValSloppy)));

    if (Options::exposeProfilersOnGlobalObject()) {
#if ENABLE(SAMPLING_PROFILER)
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__enableSamplingProfiler"_s), JSFunction::create(vm, this, 1, "enableSamplingProfiler"_s, enableSamplingProfiler, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__disableSamplingProfiler"_s), JSFunction::create(vm, this, 1, "disableSamplingProfiler"_s, disableSamplingProfiler, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__dumpAndClearSamplingProfilerSamples"_s), JSFunction::create(vm, this, 1, "dumpAndClearSamplingProfilerSamples"_s, dumpAndClearSamplingProfilerSamples, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
#endif
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__enableSuperSampler"_s), JSFunction::create(vm, this, 1, "enableSuperSampler"_s, enableSuperSampler, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__disableSuperSampler"_s), JSFunction::create(vm, this, 1, "disableSuperSampler"_s, disableSuperSampler, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);

        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__tracePointStart"_s), JSFunction::create(vm, this, 4, "tracePointStart"_s, tracePointStart, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__tracePointStop"_s), JSFunction::create(vm, this, 4, "tracePointStop"_s, tracePointStop, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__signpostStart"_s), JSFunction::create(vm, this, 1, "signpostStart"_s, signpostStart, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "__signpostStop"_s), JSFunction::create(vm, this, 1, "signpostStop"_s, signpostStop, ImplementationVisibility::Public), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    }

    initStaticGlobals(vm);

    if (Options::useDollarVM()) [[unlikely]]
        exposeDollarVM(vm);

#if ENABLE(WEBASSEMBLY)
    if (Wasm::isSupported()) {
        m_webAssemblyModuleRecordStructure.initLater(
            [] (const Initializer<Structure>& init) {
                init.set(WebAssemblyModuleRecord::createStructure(init.vm, init.owner, jsNull()));
            });
        m_webAssemblyFunctionStructure.initLater(
            [] (const Initializer<Structure>& init) {
                init.set(WebAssemblyFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
            });
        m_webAssemblyWrapperFunctionStructure.initLater(
            [] (const Initializer<Structure>& init) {
                init.set(WebAssemblyWrapperFunction::createStructure(init.vm, init.owner, init.owner->m_functionPrototype.get()));
            });
        m_webAssemblyJSTag.initLater(
            [] (const Initializer<JSWebAssemblyTag>& init) {
                init.set(JSWebAssemblyTag::create(init.vm, init.owner, init.owner->webAssemblyTagStructure(), Wasm::Tag::jsExceptionTag()));
            });
        auto* webAssembly = JSWebAssembly::create(vm, this, JSWebAssembly::createStructure(vm, this, m_objectPrototype.get()));
        putDirectWithoutTransition(vm, Identifier::fromString(vm, "WebAssembly"_s), webAssembly, static_cast<unsigned>(PropertyAttribute::DontEnum));

#define CREATE_WEBASSEMBLY_PROTOTYPE(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) \
    if (featureFlag) {\
        m_ ## properName ## Structure.initLater(\
            [] (LazyClassStructure::Initializer& init) { \
                init.setPrototype(capitalName##Prototype::create(init.vm, init.global, capitalName##Prototype::createStructure(init.vm, init.global, init.global->prototypeBase ## Prototype()))); \
                init.setStructure(instanceType::createStructure(init.vm, init.global, init.prototype)); \
                auto* constructorPrototype = strcmp(#prototypeBase, "error") == 0 ? init.global->m_errorStructure.constructor(init.global) : init.global->functionPrototype(); \
                init.setConstructor(capitalName ## Constructor::create(init.vm, capitalName ## Constructor::createStructure(init.vm, init.global, constructorPrototype), uncheckedDowncast<capitalName ## Prototype>(init.prototype))); \
            }); \
    }

        FOR_EACH_WEBASSEMBLY_CONSTRUCTOR_TYPE(CREATE_WEBASSEMBLY_PROTOTYPE)

#undef CREATE_WEBASSEMBLY_PROTOTYPE

        if (Options::useJSPI()) {
            webAssembly->putDirectWithoutTransition(vm, Identifier::fromString(vm, "Suspending"_s), webAssemblySuspendingConstructor(), static_cast<unsigned>(PropertyAttribute::DontEnum));
            webAssembly->putDirectWithoutTransition(vm, Identifier::fromString(vm, "SuspendError"_s), webAssemblySuspendErrorConstructor(), static_cast<unsigned>(PropertyAttribute::DontEnum));
        }
    }
#endif // ENABLE(WEBASSEMBLY)

    // Detect property change.
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, arrayIteratorPrototype, vm.propertyNames->next), m_arrayIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, this->arrayPrototype(), vm.propertyNames->iteratorSymbol), m_arrayIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, this->arrayPrototype(), vm.propertyNames->join), m_arrayJoinWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, this->arrayPrototype(), vm.propertyNames->toString), m_arrayToStringWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, mapIteratorPrototype, vm.propertyNames->next), m_mapIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, setIteratorPrototype, vm.propertyNames->next), m_setIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_stringIteratorPrototype.get(), vm.propertyNames->next), m_stringIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->iteratorSymbol), m_stringIteratorProtocolWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->toString), m_stringToStringWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->valueOf), m_stringValueOfWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->valueOf), m_objectPrototypeValueOfWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_arrayPrototype.get(), vm.propertyNames->valueOf, objectPrototype()), m_arrayPrototypeValueOfWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->exec), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->flags), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->dotAll), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->global), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->hasIndices), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->ignoreCase), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->multiline), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->sticky), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->unicode), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->unicodeSets), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->replaceSymbol), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->matchSymbol), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->searchSymbol), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->matchAllSymbol), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_regExpPrototype.get(), vm.propertyNames->splitSymbol), m_regExpPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, jsSetPrototype(), vm.propertyNames->has), m_setPrimordialPropertiesWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, jsSetPrototype(), vm.propertyNames->keys), m_setPrimordialPropertiesWatchpointSet);

    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, promisePrototype(), vm.propertyNames->then), m_promiseThenWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->then, nullptr), m_promiseThenWatchpointSet);
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, m_promiseConstructor.get(), vm.propertyNames->resolve), m_promiseResolveWatchpointSet);

    // Detect property absence.
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->matchSymbol, objectPrototype()), m_stringSymbolMatchWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->matchSymbol, nullptr), m_stringSymbolMatchWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->searchSymbol, objectPrototype()), m_stringSymbolSearchWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->searchSymbol, nullptr), m_stringSymbolSearchWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->matchAllSymbol, objectPrototype()), m_stringSymbolMatchAllWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->matchAllSymbol, nullptr), m_stringSymbolMatchAllWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->replaceSymbol, objectPrototype()), m_stringSymbolReplaceWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->replaceSymbol, nullptr), m_stringSymbolReplaceWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->splitSymbol, objectPrototype()), m_stringSymbolSplitWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->splitSymbol, nullptr), m_stringSymbolSplitWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringPrototype.get(), vm.propertyNames->toPrimitiveSymbol, objectPrototype()), m_stringSymbolToPrimitiveWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->toPrimitiveSymbol, nullptr), m_stringSymbolToPrimitiveWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_arrayPrototype.get(), vm.propertyNames->toPrimitiveSymbol, objectPrototype()), m_arraySymbolToPrimitiveWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->toPrimitiveSymbol, nullptr), m_arraySymbolToPrimitiveWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_arrayPrototype.get(), vm.propertyNames->negativeOneIdentifier, objectPrototype()), m_arrayNegativeOneWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->negativeOneIdentifier, nullptr), m_arrayNegativeOneWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_arrayPrototype.get(), vm.propertyNames->isConcatSpreadableSymbol, objectPrototype()), m_arrayIsConcatSpreadableWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->isConcatSpreadableSymbol, nullptr), m_arrayIsConcatSpreadableWatchpointSet);

    // The iterator protocol fast paths assume that IteratorClose is unobservable, so they must be
    // invalidated when a "return" property appears anywhere on the iterator's prototype chain.
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, arrayIteratorPrototype, vm.propertyNames->returnKeyword, m_iteratorPrototype.get()), m_arrayIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, mapIteratorPrototype, vm.propertyNames->returnKeyword, m_iteratorPrototype.get()), m_mapIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, setIteratorPrototype, vm.propertyNames->returnKeyword, m_iteratorPrototype.get()), m_setIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_stringIteratorPrototype.get(), vm.propertyNames->returnKeyword, m_iteratorPrototype.get()), m_stringIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_iteratorPrototype.get(), vm.propertyNames->returnKeyword, objectPrototype()), m_arrayIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_iteratorPrototype.get(), vm.propertyNames->returnKeyword, objectPrototype()), m_mapIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_iteratorPrototype.get(), vm.propertyNames->returnKeyword, objectPrototype()), m_setIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_iteratorPrototype.get(), vm.propertyNames->returnKeyword, objectPrototype()), m_stringIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->returnKeyword, nullptr), m_arrayIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->returnKeyword, nullptr), m_mapIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->returnKeyword, nullptr), m_setIteratorProtocolWatchpointSet);
    installObjectAdaptiveStructureWatchpoint(setupAbsenceAdaptiveWatchpoint(this, m_objectPrototype.get(), vm.propertyNames->returnKeyword, nullptr), m_stringIteratorProtocolWatchpointSet);

    // Array Species watchpoint.
    {
        RELEASE_ASSERT(!m_arrayPrototypeConstructorWatchpoint);
        RELEASE_ASSERT(!m_arrayConstructorSpeciesWatchpoint);
        tryInstallSpeciesWatchpoint(this->arrayPrototype(), arrayConstructor, m_arrayPrototypeConstructorWatchpoint, m_arrayConstructorSpeciesWatchpoint, m_arraySpeciesWatchpointSet, HasSpeciesProperty::Yes, arraySpeciesGetterSetter());
        catchScope.assertNoException();
    }
    {
        tryInstallSpeciesWatchpoint(this->promisePrototype(), promiseConstructor, m_promisePrototypeConstructorWatchpoint, m_promiseConstructorSpeciesWatchpoint, m_promiseSpeciesWatchpointSet, HasSpeciesProperty::Yes, promiseSpeciesGetterSetter());
        catchScope.assertNoException();
    }
    {
        // RegExp Species watchpoint: validates that RegExp[Symbol.species] hasn't been
        // overridden so the C++ split fast path can skip the species-construction step
        // performed by the JS regExpPrototypeSplit builtin.
        RegExpConstructor* regExpConstructor = m_regExpConstructor.get();
        PropertySlot speciesSlot(regExpConstructor, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool found = regExpConstructor->getOwnPropertySlot(regExpConstructor, this, vm.propertyNames->speciesSymbol, speciesSlot);
        speciesSlot.disallowVMEntry.reset();
        if (found && speciesSlot.isAccessor()) {
            GetterSetter* speciesGetterSetter = speciesSlot.getterSetter();
            tryInstallSpeciesWatchpoint(m_regExpPrototype.get(), regExpConstructor, m_regExpPrototypeConstructorWatchpoint, m_regExpConstructorSpeciesWatchpoint, m_regExpSpeciesWatchpointSet, HasSpeciesProperty::Yes, speciesGetterSetter);
        }
        catchScope.assertNoException();
    }

    installSaneChainWatchpoints();

    // Unfortunately, the prototype objects of the builtin objects can be touched from concurrent compilers. So eagerly initialize them only if we use JIT.
    if (Options::useJIT()) {
        this->booleanPrototype();
        this->numberPrototype();
        this->symbolPrototype();
    }

    fixupPrototypeChainWithObjectPrototype(vm);

    if (Options::alwaysHaveABadTime()) [[unlikely]]
        this->haveABadTime(vm);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

bool JSGlobalObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* thisObject = uncheckedDowncast<JSGlobalObject>(cell);
    ASSERT(!Heap::heap(value) || Heap::heap(value) == Heap::heap(thisObject));

    if (isThisValueAltered(slot, thisObject)) [[unlikely]] {
        SymbolTableEntry::Fast entry = thisObject->symbolTable()->get(propertyName.uid());
        if (!entry.isNull()) {
            if (entry.isReadOnly())
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot));
        }
        RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
    }

    bool shouldThrowReadOnlyError = slot.isStrictMode();
    bool ignoreReadOnlyErrors = false;
    bool putResult = false;
    bool done = symbolTablePutTouchWatchpointSet(thisObject, globalObject, propertyName, value, shouldThrowReadOnlyError, ignoreReadOnlyErrors, putResult);
    EXCEPTION_ASSERT((!!scope.exception() == (done && !putResult)) || !shouldThrowReadOnlyError);
    if (done)
        return putResult;
    RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
}

bool JSGlobalObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* thisObject = uncheckedDowncast<JSGlobalObject>(object);

    SymbolTableEntry entry;
    PropertyDescriptor currentDescriptor;
    if (symbolTableGet(thisObject, propertyName, entry, currentDescriptor)) {
        bool isExtensible = false; // ignored since current descriptor is present
        bool isCurrentDefined = true;
        bool isCompatibleDescriptor = validateAndApplyPropertyDescriptor(globalObject, nullptr, propertyName, isExtensible, descriptor, isCurrentDefined, currentDescriptor, shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isCompatibleDescriptor)
            return false;

        if (descriptor.value()) {
            bool ignoreReadOnlyErrors = true;
            bool putResult = false;
            if (symbolTablePutTouchWatchpointSet(thisObject, globalObject, propertyName, descriptor.value(), shouldThrow, ignoreReadOnlyErrors, putResult))
                ASSERT(putResult);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (descriptor.writablePresent() && !descriptor.writable() && !entry.isReadOnly()) {
            entry.setReadOnly();
            thisObject->symbolTable()->set(propertyName.uid(), entry);
            thisObject->varReadOnlyWatchpointSet().fireAll(vm, "GlobalVar was redefined as ReadOnly");
        }
        return true;
    }

    RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow));
}

// https://tc39.es/ecma262/#sec-candeclareglobalfunction
bool JSGlobalObject::canDeclareGlobalFunction(const Identifier& ident)
{
    auto scope = DECLARE_THROW_SCOPE(vm());

    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = getOwnPropertySlot(this, this, ident, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasProperty) [[likely]]
        return isStructureExtensible();

    bool isConfigurable = !(slot.attributes() & PropertyAttribute::DontDelete);
    if (isConfigurable)
        return true;
    bool isDataDescriptor = !(slot.attributes() & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor));
    bool isWritableAndEnumerable = !(slot.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum));
    return isDataDescriptor && isWritableAndEnumerable;
}

// https://tc39.es/ecma262/#sec-createglobalfunctionbinding
template<BindingCreationContext context>
void JSGlobalObject::createGlobalFunctionBinding(const Identifier& ident)
{
    VM& vm = this->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = getOwnPropertySlot(this, this, ident, slot);
    RETURN_IF_EXCEPTION(scope, void());
    if (hasProperty) [[unlikely]] {
        if (slot.attributes() & PropertyAttribute::DontDelete) {
            ASSERT(!(slot.attributes() & PropertyAttribute::ReadOnly));
            // Nothing to do here: there is either a symbol table entry or non-configurable writable property
            // on the structure that will be updated with real function by put_to_scope.
        } else {
            unsigned newAttributes = 0;
            if constexpr (context == BindingCreationContext::Global)
                newAttributes |= PropertyAttribute::DontDelete;
            putDirect(vm, ident, jsUndefined(), newAttributes);
        }
    } else {
        ASSERT(isStructureExtensible());
        if constexpr (context == BindingCreationContext::Global)
            addSymbolTableEntry(ident);
        else
            putDirect(vm, ident, jsUndefined());
    }
}

template void JSGlobalObject::createGlobalFunctionBinding<BindingCreationContext::Global>(const Identifier&);
template void JSGlobalObject::createGlobalFunctionBinding<BindingCreationContext::Eval>(const Identifier&);

void JSGlobalObject::addSymbolTableEntry(const Identifier& ident)
{
    ConcurrentJSLocker locker(symbolTable()->m_lock);
    ASSERT(!symbolTable()->contains(locker, ident.impl()));

    ScopeOffset offset = symbolTable()->takeNextScopeOffset(locker);
    SymbolTableEntry newEntry(VarOffset(offset), 0);
    newEntry.prepareToWatch();
    symbolTable()->add(locker, ident.impl(), WTF::move(newEntry));

    ScopeOffset offsetForAssert = addVariables(1, jsUndefined());
    RELEASE_ASSERT(offsetForAssert == offset);
}

void JSGlobalObject::setGlobalScopeExtension(JSScope* scope)
{
    m_globalScopeExtension.set(vm(), this, scope);
}

void JSGlobalObject::clearGlobalScopeExtension()
{
    m_globalScopeExtension.clear();
}

void JSGlobalObject::notifyArrayBufferDetachingSlow()
{
    m_arrayBufferDetachWatchpointSet->fireAll(vm(), "ArrayBuffer detached");
}

static inline JSObject* NODELETE lastInPrototypeChain(JSObject* object)
{
    JSObject* o = object;
    while (o->getPrototypeDirect().isObject())
        o = asObject(o->getPrototypeDirect());
    return o;
}

// Private namespace for helpers for JSGlobalObject::haveABadTime()
namespace {

class GlobalObjectDependencyFinder : public MarkedBlock::VoidFunctor {
public:
    GlobalObjectDependencyFinder() = default;

    IterationStatus operator()(HeapCell*, HeapCell::Kind) const;

    void addDependency(JSGlobalObject* key, JSGlobalObject* dependent);
    UncheckedKeyHashSet<JSGlobalObject*>* dependentsFor(JSGlobalObject* key);

private:
    void visit(JSObject*);

    UncheckedKeyHashMap<JSGlobalObject*, UncheckedKeyHashSet<JSGlobalObject*>> m_dependencies;
};

inline void GlobalObjectDependencyFinder::addDependency(JSGlobalObject* key, JSGlobalObject* dependent)
{
    auto keyResult = m_dependencies.add(key, UncheckedKeyHashSet<JSGlobalObject*>());
    keyResult.iterator->value.add(dependent);
}

inline UncheckedKeyHashSet<JSGlobalObject*>* GlobalObjectDependencyFinder::dependentsFor(JSGlobalObject* key)
{
    auto iterator = m_dependencies.find(key);
    if (iterator == m_dependencies.end())
        return nullptr;
    return &iterator->value;
}

inline void GlobalObjectDependencyFinder::visit(JSObject* object)
{
    if (!object->mayBePrototype())
        return;

    JSObject* current = object;
    JSGlobalObject* objectGlobalObject = object->realmMayBeNull();
    if (!objectGlobalObject) {
        ASSERT(object->getPrototypeDirect().isNull());
        return;
    }

    do {
        JSValue prototypeValue = current->getPrototypeDirect();
        if (prototypeValue.isNull())
            return;
        current = asObject(prototypeValue);

        JSGlobalObject* protoGlobalObject = current->realmMayBeNull();
        if (protoGlobalObject && protoGlobalObject != objectGlobalObject)
            addDependency(protoGlobalObject, objectGlobalObject);
    } while (true);
}

IterationStatus GlobalObjectDependencyFinder::operator()(HeapCell* cell, HeapCell::Kind kind) const
{
    if (isJSCellKind(kind) && static_cast<JSCell*>(cell)->isObject()) {
        // FIXME: This const_cast exists because this isn't a C++ lambda.
        // https://bugs.webkit.org/show_bug.cgi?id=159644
        const_cast<GlobalObjectDependencyFinder*>(this)->visit(uncheckedDowncast<JSObject>(static_cast<JSCell*>(cell)));
    }
    return IterationStatus::Continue;
}

enum class BadTimeFinderMode {
    SingleGlobal,
    MultipleGlobals
};

template<BadTimeFinderMode mode>
class ObjectsWithBrokenIndexingFinder : public MarkedBlock::VoidFunctor {
public:
    ObjectsWithBrokenIndexingFinder(Vector<JSObject*>&, JSGlobalObject*);
    ObjectsWithBrokenIndexingFinder(Vector<JSObject*>&, UncheckedKeyHashSet<JSGlobalObject*>&);

    bool NODELETE needsMultiGlobalsScan() const { return m_needsMultiGlobalsScan; }
    IterationStatus operator()(HeapCell*, HeapCell::Kind) const;

private:
    IterationStatus visit(JSObject*);

    Vector<JSObject*>& m_foundObjects;
    JSGlobalObject* const m_globalObject { nullptr }; // Only used for SingleBadTimeGlobal mode.
    UncheckedKeyHashSet<JSGlobalObject*>* m_globalObjects { nullptr }; // Only used for BadTimeGlobalGraph mode;
    bool m_needsMultiGlobalsScan { false };
};

template<>
ObjectsWithBrokenIndexingFinder<BadTimeFinderMode::SingleGlobal>::ObjectsWithBrokenIndexingFinder(Vector<JSObject*>& foundObjects, JSGlobalObject* globalObject)
    : m_foundObjects(foundObjects)
    , m_globalObject(globalObject)
{
}

template<>
ObjectsWithBrokenIndexingFinder<BadTimeFinderMode::MultipleGlobals>::ObjectsWithBrokenIndexingFinder(Vector<JSObject*>& foundObjects, UncheckedKeyHashSet<JSGlobalObject*>& globalObjects)
    : m_foundObjects(foundObjects)
    , m_globalObjects(&globalObjects)
{
}

inline bool NODELETE hasBrokenIndexing(IndexingType type)
{
    return type && !hasSlowPutArrayStorage(type);
}

inline bool NODELETE hasBrokenIndexing(JSObject* object)
{
    IndexingType type = object->indexingType();
    return hasBrokenIndexing(type);
}

template<BadTimeFinderMode mode>
inline IterationStatus ObjectsWithBrokenIndexingFinder<mode>::visit(JSObject* object)
{
    // We only want to have a bad time in the affected global object, not in the entire
    // VM. But we have to be careful, since there may be objects that claim to belong to
    // a different global object that have prototypes from our global object.
    auto isInAffectedGlobalObject = [&] (JSObject* object) {
        JSGlobalObject* objectGlobalObject { nullptr };
        bool objectMayBePrototype { false };

        if (mode == BadTimeFinderMode::SingleGlobal) {
            objectGlobalObject = object->realmMayBeNull();
            if (objectGlobalObject == m_globalObject)
                return true;

            objectMayBePrototype = object->mayBePrototype();
        }

        for (JSObject* current = object; ;) {
            JSGlobalObject* currentGlobalObject = current->realmMayBeNull();
            if (mode == BadTimeFinderMode::SingleGlobal) {
                if (objectMayBePrototype && currentGlobalObject != objectGlobalObject)
                    m_needsMultiGlobalsScan = true;
                if (currentGlobalObject == m_globalObject)
                    return true;
            } else {
                if (currentGlobalObject && m_globalObjects->contains(currentGlobalObject))
                    return true;
            }

            JSValue prototypeValue = current->getPrototypeDirect();
            if (prototypeValue.isNull())
                return false;
            current = asObject(prototypeValue);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto checkStructureHasRelevantGlobalObject = [&](Structure* structure) -> bool {
        if (hasBrokenIndexing(structure->indexingType())) {
            bool isRelevantGlobalObject =
                (mode == BadTimeFinderMode::SingleGlobal
                    ? m_globalObject == structure->realm()
                    : m_globalObjects->contains(structure->realm()))
                || (structure->hasMonoProto() && !structure->storedPrototype().isNull() && isInAffectedGlobalObject(asObject(structure->storedPrototype())));
            return isRelevantGlobalObject;
        }
        return false;
    };

    if (object->inherits<JSFunction>()) {
        JSFunction* function = uncheckedDowncast<JSFunction>(object);
        if (FunctionRareData* rareData = function->rareData()) {
            // We only use this to cache JSFinalObjects. They do not start off with a broken indexing type.
            ASSERT(!(rareData->objectAllocationStructure() && hasBrokenIndexing(rareData->objectAllocationStructure()->indexingType())));

            if (Structure* structure = rareData->internalFunctionAllocationStructure()) {
                bool isRelevantGlobalObject = checkStructureHasRelevantGlobalObject(structure);
                if (mode == BadTimeFinderMode::SingleGlobal && m_needsMultiGlobalsScan)
                    return IterationStatus::Done; // Bailing early and let the MultipleGlobals path handle everything.
                if (isRelevantGlobalObject)
                    rareData->clearInternalFunctionAllocationProfile(function->vm(), "have a bad time breaking internal function allocation");
            }
        }
    }

    if (object->inherits<JSGlobalObject>()) {
        JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(object);
        // If this globalObject is already having a bad time, then structures in its StructureCache
        // does not affect on this new JSGlobalObject's haveABadTime since they are already slow mode.
        if (!globalObject->isHavingABadTime()) {
            VM& vm = globalObject->vm();
            ASSERT(vm.heap.isDeferred());
            bool willClear = false;
            globalObject->structureCache().forEach([&](Structure* structure) {
                bool isRelevantGlobalObject = checkStructureHasRelevantGlobalObject(structure);
                if (mode == BadTimeFinderMode::SingleGlobal && m_needsMultiGlobalsScan)
                    return IterationStatus::Done;
                if (isRelevantGlobalObject)
                    willClear = true;
                return IterationStatus::Continue;
            });
            if (mode == BadTimeFinderMode::SingleGlobal && m_needsMultiGlobalsScan)
                return IterationStatus::Done; // Bailing early and let the MultipleGlobals path handle everything.

            // StructureCache contains Structures which is no longer valid after relevant JSGlobalObject's haveABadTime.
            // We do not make such a JSGlobalObject status haveABadTime since still its own objects are intact.
            if (willClear)
                globalObject->clearStructureCache(vm);
        }
    }

    // Run this filter first, since it's cheap, and ought to filter out a lot of objects.
    if (!hasBrokenIndexing(object))
        return IterationStatus::Continue;

    if (isInAffectedGlobalObject(object))
        m_foundObjects.append(object);

    if (mode == BadTimeFinderMode::SingleGlobal && m_needsMultiGlobalsScan)
        return IterationStatus::Done; // Bailing early and let the MultipleGlobals path handle everything.

    return IterationStatus::Continue;
}

template<BadTimeFinderMode mode>
IterationStatus ObjectsWithBrokenIndexingFinder<mode>::operator()(HeapCell* cell, HeapCell::Kind kind) const
{
    if (isJSCellKind(kind) && static_cast<JSCell*>(cell)->isObject()) {
        // FIXME: This const_cast exists because this isn't a C++ lambda.
        // https://bugs.webkit.org/show_bug.cgi?id=159644
        return const_cast<ObjectsWithBrokenIndexingFinder*>(this)->visit(uncheckedDowncast<JSObject>(static_cast<JSCell*>(cell)));
    }
    return IterationStatus::Continue;
}

} // end private namespace for helpers for JSGlobalObject::haveABadTime()

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

void JSGlobalObject::fireWatchpointAndMakeAllArrayStructuresSlowPut(VM& vm)
{
    if (isHavingABadTime())
        return;

    // This must happen first, because the compiler thread may race with haveABadTime.
    // Let R_BT, W_BT <- Read/Fire the watchpoint, R_SC, W_SC <- Read/clear the structure cache.
    // The possible interleavings are:
    // R_BT, R_SC, W_SC, W_BT: Compiler thread installs a watchpoint, and the code is discarded.
    // R_BT, W_SC, R_SC, W_BT: ^ Same
    // R_BT, W_SC, W_BT, W_SC: ^ Same
    // W_SC, R_BT, R_SC, W_BT: ^ Same
    // W_SC, R_BT, W_BT, R_SC: ^ Same
    // W_SC, W_BT, R_BT, R_SC: No watchpoint is installed, but we could not see old structures from the cache.
    clearStructureCache(vm);

    // Make sure that all JSArray allocations that load the appropriate structure from
    // this object now load a structure that uses SlowPut.
    for (unsigned i = 0; i < NumberOfArrayIndexingModes; ++i)
        m_arrayStructureForIndexingShapeDuringAllocation[i].set(vm, this, originalArrayStructureForIndexingType(ArrayWithSlowPutArrayStorage));

    // Same for any special array structures.
    Structure* slowPutStructure;
    slowPutStructure = createRegExpMatchesArraySlowPutStructure(vm, this);
    m_regExpMatchesArrayStructure.set(vm, this, slowPutStructure);
    slowPutStructure = createRegExpMatchesArrayWithIndicesSlowPutStructure(vm, this);
    m_regExpMatchesArrayWithIndicesStructure.set(vm, this, slowPutStructure);
    slowPutStructure = createRegExpMatchesIndicesArraySlowPutStructure(vm, this);
    m_regExpMatchesIndicesArrayStructure.set(vm, this, slowPutStructure);
    slowPutStructure = ClonedArguments::createSlowPutStructure(vm, this, m_objectPrototype.get());
    m_clonedArgumentsStructure.set(vm, this, slowPutStructure);

    // Make sure that all allocations or indexed storage transitions that are inlining
    // the assumption that it's safe to transition to a non-SlowPut array storage don't
    // do so anymore.
    // Note: we are deliberately firing the watchpoint here at the end only after
    // making all the array structures SlowPut. This ensures that the concurrent
    // JIT threads will always get the SlowPut versions of the structures if
    // isHavingABadTime() returns true. The concurrent JIT relies on this.
    m_havingABadTimeWatchpointSet->fireAll(vm, "Having a bad time");
    ASSERT(isHavingABadTime()); // The watchpoint is what tells us that we're having a bad time.
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

void JSGlobalObject::clearStructureCache(VM& vm)
{
    m_structureCache.clear(); // We may be caching array structures in here.
    m_structureCacheClearedWatchpointSet.fireAll(vm, "Clearing StructureCache");
}

void JSGlobalObject::haveABadTime(VM& vm)
{
    ASSERT(&vm == &this->vm());

    if (isHavingABadTime())
        return;

    // UNGIL SPEC-ungil §K.5 class-4 (ANNEX HBT as amended by HBT2-HBT4) —
    // AB-10 CLOSED: GIL-off, the WHOLE body from here to the end of the
    // conversion walk runs as ONE §A.3 thread-granular stop with the calling
    // mutator as conductor. It fires the HaveABadTime watchpoint and then
    // iterates and rewrites OTHER threads' live objects (forEachLiveCell +
    // SlowPutArrayStorage conversion) — silent heap/structure corruption if
    // sibling mutators run (a sibling could allocate/store through a fast
    // indexing mode after the watchpoint fired but before the walk saw its
    // array). Routing through JSThreadsSafepoint::stopTheWorldAndRun gives
    // the HBT4-ordered conductor sequence (release access -> §A.3.3
    // arbitration -> GCL -> fan -> predicate wait), the HBT3.2 in-window
    // re-acquisition of the conductor's OWN client access (which is what
    // licenses the Class-4 allocating body: ArrayStorage conversion, Vector
    // growth, slow-put structure creation), and the HBT2.2 no-GC-in-window
    // I14 bracket (GC initiation inside the window defers; the function-
    // scoped DeferGC below dies inside the closure and its exit check
    // re-runs on the conductor after resume).
    //
    // HBT item 1/2: arbitration losers park and retry; the post-arbitration
    // isHavingABadTime() re-check inside the closure makes double entry for
    // the SAME global idempotent (another thread may have completed the same
    // transition while we waited); DIFFERENT globals' calls serialize through
    // the same arbitration, each running its own complete stop. A nested call
    // from inside an already-open window (R1.h) runs inline under the
    // conductor's witness. The CONCURRENT-COMPILER half is unchanged: the
    // jit I2/R1 watchpoint/jettison protocol covers compiler threads, which
    // do not park under §A.3.
    //
    // GIL-on/flag-off: unchanged — the GIL is the serializer; no stop is
    // requested (one predicted-false byte test).
    if (vm.gilOff()) [[unlikely]] {
        JSThreadsSafepoint::stopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            // ANNEX HBT item 2: conductor re-checks isHavingABadTime() after
            // winning arbitration — a sibling may have completed this
            // global's transition while we waited.
            if (isHavingABadTime())
                return;
            haveABadTimeImpl(vm);
        }));
        return;
    }

    haveABadTimeImpl(vm);
}

void JSGlobalObject::haveABadTimeImpl(VM& vm)
{
    ASSERT(!isHavingABadTime());

    DeferGC deferGC(vm);

    // Consider the following objects and prototype chains:
    //    O (of global G1) -> A (of global G1)
    //    B (of global G2) where G2 has a bad time
    //
    // If we set B as the prototype of A, G1 will need to have a bad time.
    // See comments in Structure::mayInterceptIndexedAccesses() for why.
    //
    // Now, consider the following objects and prototype chains:
    //    O1 (of global G1) -> A1 (of global G1) -> B1 (of global G2)
    //    O2 (of global G2) -> A2 (of global G2)
    //    B2 (of global G3) where G3 has a bad time.
    //
    // G1 and G2 does not have a bad time, but G3 already has a bad time.
    // If we set B2 as the prototype of A2, then G2 needs to have a bad time.
    // Note that by induction, G1 also now needs to have a bad time because of
    // O1 -> A1 -> B1.
    //
    // We describe this as global G1 being affected by global G2, and G2 by G3.
    // Similarly, we say that G1 is dependent on G2, and G2 on G3.
    // Hence, when G3 has a bad time, we need to ensure that all globals that
    // are transitively dependent on it also have a bad time (G2 and G1 in this
    // example).
    //
    // Apart from clearing the VM structure cache above, there are 2 more things
    // that we have to do when globals have a bad time:
    // 1. For each affected global:
    //    a. Fire its HaveABadTime watchpoint.
    //    b. Convert all of its array structures to SlowPutArrayStorage.
    // 2. Make sure that all affected objects  switch to the slow kind of
    //    indexed storage. An object is considered to be affected if it has
    //    indexed storage and has a prototype object which may have indexed
    //    accessors. If the prototype object belongs to a global having a bad
    //    time, then the prototype object is considered to possibly have indexed
    //    accessors. See comments in Structure::mayInterceptIndexedAccesses()
    //    for details.
    //
    // Note: step 1 must be completed before step 2 because step 2 relies on
    // the HaveABadTime watchpoint having already been fired on all affected
    // globals.
    //
    // In the common case, only this global will start having a bad time here,
    // and no other globals are affected by it. So, we first proceed on this assumption
    // with a simpler ObjectsWithBrokenIndexingFinder scan to find heap objects
    // affected by this global that need to be converted to SlowPutArrayStorage.
    // We'll also have the finder check for the presence of other global objects
    // depending on this one.
    //
    // If we do discover other globals depending on this one, we'll abort this
    // first ObjectsWithBrokenIndexingFinder scan because it will be insufficient
    // to find all affected objects that need to be converted to SlowPutArrayStorage.
    // It also does not make dependent globals have a bad time. Instead, we'll
    // take a more comprehensive approach of first creating a dependency graph
    // between globals, and then using that graph to determine all affected
    // globals and objects. With that, we can make all affected globals have a
    // bad time, and convert all affected objects to SlowPutArrayStorage.

    fireWatchpointAndMakeAllArrayStructuresSlowPut(vm); // Step 1 above.

    Vector<JSObject*> foundObjects;
    ObjectsWithBrokenIndexingFinder<BadTimeFinderMode::SingleGlobal> finder(foundObjects, this);
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, finder); // Attempt step 2 above.
    }

    if (finder.needsMultiGlobalsScan()) {
        foundObjects.clear();

        // Find all globals that will also have a bad time as a side effect of
        // this global having a bad time.
        GlobalObjectDependencyFinder dependencies;
        {
            HeapIterationScope iterationScope(vm.heap);
            vm.heap.objectSpace().forEachLiveCell(iterationScope, dependencies);
        }

        UncheckedKeyHashSet<JSGlobalObject*> globalsHavingABadTime;
        Deque<JSGlobalObject*> globals;

        globals.append(this);
        while (!globals.isEmpty()) {
            JSGlobalObject* global = globals.takeFirst();
            global->fireWatchpointAndMakeAllArrayStructuresSlowPut(vm); // Step 1 above.
            auto result = globalsHavingABadTime.add(global);
            if (result.isNewEntry) {
                if (UncheckedKeyHashSet<JSGlobalObject*>* dependents = dependencies.dependentsFor(global)) {
                    for (JSGlobalObject* dependentGlobal : *dependents)
                        globals.append(dependentGlobal);
                }
            }
        }

        ObjectsWithBrokenIndexingFinder<BadTimeFinderMode::MultipleGlobals> finder(foundObjects, globalsHavingABadTime);
        {
            HeapIterationScope iterationScope(vm.heap);
            vm.heap.objectSpace().forEachLiveCell(iterationScope, finder); // Step 2 above.
        }
    }

    while (!foundObjects.isEmpty()) {
        JSObject* object = asObject(foundObjects.last());
        foundObjects.removeLast();
        ASSERT(hasBrokenIndexing(object));
        object->switchToSlowPutArrayStorage(vm);
    }
}

void JSGlobalObject::fixupPrototypeChainWithObjectPrototype(VM& vm)
{
    JSObject* oldLastInPrototypeChain = lastInPrototypeChain(this);
    JSObject* objectPrototype = m_objectPrototype.get();
    if (oldLastInPrototypeChain != objectPrototype)
        oldLastInPrototypeChain->setPrototypeDirect(vm, objectPrototype);
}

// Set prototype, and also insert the object prototype at the end of the chain.
void JSGlobalObject::resetPrototype(VM& vm, JSValue prototype)
{
    if (getPrototypeDirect() == prototype)
        return;
    setPrototypeDirect(vm, prototype);
    fixupPrototypeChainWithObjectPrototype(vm);
    // Whenever we change the prototype of the global object, we need to create a new JSGlobalProxy with the correct prototype.
    setGlobalThis(vm, JSGlobalProxy::create(vm, JSGlobalProxy::createStructure(vm, this, prototype), this));
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename Visitor>
void JSGlobalObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSGlobalObject* thisObject = uncheckedDowncast<JSGlobalObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    visitor.append(thisObject->m_globalThis);

#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_asyncContextData);
    visitor.append(thisObject->m_internalFieldTupleStructure);
#endif

    visitor.append(thisObject->m_globalLexicalEnvironment);
    visitor.append(thisObject->m_globalScopeExtension);
    visitor.append(thisObject->m_globalCallee);
    visitor.append(thisObject->m_evalCallee);
    visitor.append(thisObject->m_zombieFrameCallee);
    JS_GLOBAL_OBJECT_ADDITIONS_4;
    thisObject->m_evalErrorStructure.visit(visitor);
    thisObject->m_rangeErrorStructure.visit(visitor);
    thisObject->m_referenceErrorStructure.visit(visitor);
    thisObject->m_syntaxErrorStructure.visit(visitor);
    thisObject->m_typeErrorStructure.visit(visitor);
    thisObject->m_URIErrorStructure.visit(visitor);
    thisObject->m_aggregateErrorStructure.visit(visitor);
    thisObject->m_suppressedErrorStructure.visit(visitor);
    visitor.append(thisObject->m_arrayConstructor);
    visitor.append(thisObject->m_shadowRealmConstructor);
    visitor.append(thisObject->m_regExpConstructor);
    visitor.append(thisObject->m_objectConstructor);
    visitor.append(thisObject->m_functionConstructor);
    visitor.append(thisObject->m_iteratorConstructor);
    visitor.append(thisObject->m_promiseConstructor);
    visitor.append(thisObject->m_stringConstructor);

    thisObject->m_defaultCollator.visit(visitor);
    thisObject->m_defaultDateTimeFormat.visit(visitor);
    thisObject->m_defaultDateFormat.visit(visitor);
    thisObject->m_defaultTimeFormat.visit(visitor);
    thisObject->m_defaultNumberFormat.visit(visitor);
    thisObject->m_collatorStructure.visit(visitor);
    thisObject->m_displayNamesStructure.visit(visitor);
    thisObject->m_durationFormatStructure.visit(visitor);
    thisObject->m_listFormatStructure.visit(visitor);
    thisObject->m_localeStructure.visit(visitor);
    thisObject->m_pluralRulesStructure.visit(visitor);
    thisObject->m_relativeTimeFormatStructure.visit(visitor);
    thisObject->m_segmentIteratorStructure.visit(visitor);
    thisObject->m_segmenterStructure.visit(visitor);
    thisObject->m_segmentsStructure.visit(visitor);
    thisObject->m_segmentDataObjectStructure.visit(visitor);
    thisObject->m_segmentDataObjectWithIsWordLikeStructure.visit(visitor);
    thisObject->m_intlPartObjectStructure.visit(visitor);
    thisObject->m_intlPartObjectWithSourceStructure.visit(visitor);
    thisObject->m_intlPartObjectWithUnitStructure.visit(visitor);
    thisObject->m_intlPartObjectWithUnitAndSourceStructure.visit(visitor);
    thisObject->m_dateTimeFormatStructure.visit(visitor);
    thisObject->m_numberFormatStructure.visit(visitor);

    thisObject->m_durationStructure.visit(visitor);
    thisObject->m_instantStructure.visit(visitor);
    thisObject->m_plainDateStructure.visit(visitor);
    thisObject->m_plainDateTimeStructure.visit(visitor);
    thisObject->m_plainMonthDayStructure.visit(visitor);
    thisObject->m_plainTimeStructure.visit(visitor);
    thisObject->m_plainYearMonthStructure.visit(visitor);
    thisObject->m_timeZoneStructure.visit(visitor);

    visitor.append(thisObject->m_nullGetterFunction);
    visitor.append(thisObject->m_nullSetterFunction);
    visitor.append(thisObject->m_nullSetterStrictFunction);

    thisObject->m_parseIntFunction.visit(visitor);
    thisObject->m_parseFloatFunction.visit(visitor);
    thisObject->m_objectProtoToStringFunction.visit(visitor);
    thisObject->m_arrayProtoToStringFunction.visit(visitor);
    thisObject->m_arrayProtoValuesFunction.visit(visitor);
    thisObject->m_mapProtoEntriesFunction.visit(visitor);
    thisObject->m_setProtoValuesFunction.visit(visitor);
    thisObject->m_stringProtoSymbolIteratorFunction.visit(visitor);
    visitor.append(thisObject->m_objectProtoValueOfFunction);
    thisObject->m_iteratorProtoSymbolIteratorFunction.visit(visitor);
    thisObject->m_numberProtoToStringFunction.visit(visitor);
    visitor.append(thisObject->m_functionProtoHasInstanceSymbolFunction);
    visitor.append(thisObject->m_performProxyObjectHasFunction);
    visitor.append(thisObject->m_performProxyObjectHasByValFunction);
    visitor.append(thisObject->m_performProxyObjectGetFunction);
    visitor.append(thisObject->m_performProxyObjectGetByValFunction);
    visitor.append(thisObject->m_performProxyObjectSetStrictFunction);
    visitor.append(thisObject->m_performProxyObjectSetSloppyFunction);
    visitor.append(thisObject->m_performProxyObjectSetByValStrictFunction);
    visitor.append(thisObject->m_performProxyObjectSetByValSloppyFunction);
    visitor.append(thisObject->m_regExpProtoSymbolReplace);
    thisObject->m_throwTypeErrorArgumentsCalleeGetterSetter.visit(visitor);
    thisObject->m_moduleLoader.visit(visitor);

    visitor.append(thisObject->m_objectPrototype);
    visitor.append(thisObject->m_functionPrototype);
    visitor.append(thisObject->m_arrayPrototype);
    visitor.append(thisObject->m_iteratorPrototype);
    visitor.append(thisObject->m_iteratorHelperPrototype);
    visitor.append(thisObject->m_generatorFunctionPrototype);
    visitor.append(thisObject->m_generatorPrototype);
    visitor.append(thisObject->m_arrayIteratorPrototype);
    visitor.append(thisObject->m_mapIteratorPrototype);
    visitor.append(thisObject->m_setIteratorPrototype);
    visitor.append(thisObject->m_asyncFunctionPrototype);
    visitor.append(thisObject->m_asyncGeneratorPrototype);
    visitor.append(thisObject->m_asyncIteratorPrototype);
    visitor.append(thisObject->m_asyncGeneratorFunctionPrototype);

    thisObject->m_debuggerScopeStructure.visit(visitor);
    thisObject->m_withScopeStructure.visit(visitor);
    thisObject->m_strictEvalActivationStructure.visit(visitor);
    visitor.append(thisObject->m_lexicalEnvironmentStructure);
    thisObject->m_moduleEnvironmentStructure.visit(visitor);
    visitor.append(thisObject->m_directArgumentsStructure);
    visitor.append(thisObject->m_scopedArgumentsStructure);
    visitor.append(thisObject->m_clonedArgumentsStructure);
    visitor.append(thisObject->m_objectStructureForObjectConstructor);
    for (unsigned i = 0; i < NumberOfArrayIndexingModes; ++i)
        visitor.append(thisObject->m_originalArrayStructureForIndexingShape[i]);
    for (unsigned i = 0; i < NumberOfArrayIndexingModes; ++i)
        visitor.append(thisObject->m_arrayStructureForIndexingShapeDuringAllocation[i]);
    thisObject->m_callbackConstructorStructure.visit(visitor);
    thisObject->m_callbackFunctionStructure.visit(visitor);
    thisObject->m_callbackObjectStructure.visit(visitor);
    thisObject->m_rawJSONObjectStructure.visit(visitor);
#if JSC_OBJC_API_ENABLED
    thisObject->m_objcCallbackFunctionStructure.visit(visitor);
    thisObject->m_objcWrapperObjectStructure.visit(visitor);
#endif
#ifdef JSC_GLIB_API_ENABLED
    thisObject->m_glibCallbackFunctionStructure.visit(visitor);
    thisObject->m_glibWrapperObjectStructure.visit(visitor);
#endif
    visitor.append(thisObject->m_nullPrototypeObjectStructure);
    visitor.append(thisObject->m_calleeStructure);

    visitor.append(thisObject->m_hostFunctionStructure);
    auto visitFunctionStructures = [&] (FunctionStructures& structures) {
        visitor.append(structures.arrowFunctionStructure);
        visitor.append(structures.sloppyFunctionStructure);
        visitor.append(structures.sloppyMethodStructure);
        visitor.append(structures.strictFunctionStructure);
        visitor.append(structures.strictMethodStructure);
    };
    visitFunctionStructures(thisObject->m_builtinFunctions);
    visitFunctionStructures(thisObject->m_ordinaryFunctions);
    visitor.append(thisObject->m_boundFunctionStructure);
    visitor.append(thisObject->m_trustedScriptStructure);

    thisObject->m_customGetterFunctionStructure.visit(visitor);
    thisObject->m_customSetterFunctionStructure.visit(visitor);
    thisObject->m_nativeStdFunctionStructure.visit(visitor);
    thisObject->m_remoteFunctionStructure.visit(visitor);
    visitor.append(thisObject->m_shadowRealmObjectStructure);
    visitor.append(thisObject->m_regExpStructure);
    visitor.append(thisObject->m_generatorFunctionStructure);
    visitor.append(thisObject->m_asyncFunctionStructure);
    visitor.append(thisObject->m_asyncGeneratorFunctionStructure);
    visitor.append(thisObject->m_generatorStructure);
    visitor.append(thisObject->m_asyncFunctionGeneratorStructure);
    visitor.append(thisObject->m_asyncGeneratorStructure);
    visitor.append(thisObject->m_functionWithFieldsStructure);
    visitor.append(thisObject->m_iteratorStructure);
    visitor.append(thisObject->m_iteratorHelperStructure);
    visitor.append(thisObject->m_arrayIteratorStructure);
    visitor.append(thisObject->m_mapIteratorStructure);
    visitor.append(thisObject->m_setIteratorStructure);
    visitor.append(thisObject->m_wrapForValidIteratorStructure);
    visitor.append(thisObject->m_asyncFromSyncIteratorStructure);
    visitor.append(thisObject->m_regExpStringIteratorStructure);
    thisObject->m_iteratorResultObjectStructure.visit(visitor);
    thisObject->m_dataPropertyDescriptorObjectStructure.visit(visitor);
    thisObject->m_accessorPropertyDescriptorObjectStructure.visit(visitor);
    thisObject->m_promiseCapabilityObjectStructure.visit(visitor);
    thisObject->m_promiseAllSettledFulfilledResultStructure.visit(visitor);
    thisObject->m_promiseAllSettledRejectedResultStructure.visit(visitor);
    visitor.append(thisObject->m_regExpMatchesArrayStructure);
    visitor.append(thisObject->m_regExpMatchesArrayWithIndicesStructure);
    visitor.append(thisObject->m_regExpMatchesIndicesArrayStructure);
    thisObject->m_moduleRecordStructure.visit(visitor);
    thisObject->m_syntheticModuleRecordStructure.visit(visitor);
    thisObject->m_moduleNamespaceObjectStructure.visit(visitor);
    thisObject->m_proxyObjectStructure.visit(visitor);
    thisObject->m_callableProxyObjectStructure.visit(visitor);
    thisObject->m_proxyRevokeStructure.visit(visitor);
    thisObject->m_sharedArrayBufferStructure.visit(visitor);
    thisObject->m_disposableStackStructure.visit(visitor);
    thisObject->m_asyncDisposableStackStructure.visit(visitor);

    for (auto& property : thisObject->m_linkTimeConstants)
        property.visit(visitor);

#define VISIT_SIMPLE_TYPE_PROTOTYPE(CapitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) if (featureFlag) \
        visitor.append(thisObject->m_ ## lowerName ## Prototype); \

#define VISIT_SIMPLE_TYPE_STRUCTURE(CapitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) if (featureFlag) \
        visitor.append(thisObject->m_ ## properName ## Structure); \

    FOR_EACH_SIMPLE_BUILTIN_TYPE(VISIT_SIMPLE_TYPE_STRUCTURE)
    FOR_EACH_BUILTIN_DERIVED_ITERATOR_TYPE(VISIT_SIMPLE_TYPE_STRUCTURE)
    FOR_EACH_SIMPLE_BUILTIN_TYPE(VISIT_SIMPLE_TYPE_PROTOTYPE)
    FOR_EACH_BUILTIN_DERIVED_ITERATOR_TYPE(VISIT_SIMPLE_TYPE_PROTOTYPE)

#define VISIT_LAZY_TYPE(CapitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) if (featureFlag) \
        thisObject->m_ ## properName ## Structure.visit(visitor);

    FOR_EACH_LAZY_BUILTIN_TYPE(VISIT_LAZY_TYPE)

#if ENABLE(WEBASSEMBLY)
    thisObject->m_webAssemblyModuleRecordStructure.visit(visitor);
    thisObject->m_webAssemblyFunctionStructure.visit(visitor);
    thisObject->m_webAssemblyWrapperFunctionStructure.visit(visitor);
    thisObject->m_webAssemblyJSTag.visit(visitor);
    FOR_EACH_WEBASSEMBLY_CONSTRUCTOR_TYPE(VISIT_LAZY_TYPE)
#endif // ENABLE(WEBASSEMBLY)

#undef VISIT_SIMPLE_TYPE
#undef VISIT_LAZY_TYPE

    for (unsigned i = NumberOfTypedArrayTypes; i--;) {
        thisObject->lazyTypedArrayStructure(indexToTypedArrayType(i)).visit(visitor);
        thisObject->lazyResizableOrGrowableSharedTypedArrayStructure(indexToTypedArrayType(i)).visit(visitor);
    }

    visitor.append(thisObject->m_arraySpeciesGetterSetter);
    visitor.append(thisObject->m_typedArraySpeciesGetterSetter);
    visitor.append(thisObject->m_arrayBufferSpeciesGetterSetter);
    visitor.append(thisObject->m_sharedArrayBufferSpeciesGetterSetter);
    visitor.append(thisObject->m_promiseSpeciesGetterSetter);
    visitor.append(thisObject->m_unhandledRejectionCallback);

    thisObject->m_typedArrayProto.visit(visitor);
    thisObject->m_typedArraySuperConstructor.visit(visitor);
    thisObject->m_regExpGlobalData.visitAggregate(visitor);

    {
        // UNGIL §K.1 registry-walk rooting (U-T8b; AUD1.K2 + ALS1.3): the
        // per-lite duplicates of m_regExpGlobalData / m_asyncContextData hold
        // cells owned by this global and MUST be scanned whenever the global
        // is (K4 binding consequence 3). Leaf lock; markers hold no other
        // lock here (the M11 m_microtaskQueues precedent). Entries for other
        // globals are skipped — they are visited with their own owner.
        auto& table = perLiteRealmTable();
        Locker locker { table.lock };
        for (auto& entry : table.map) {
            if (entry.key.first != thisObject)
                continue;
            if (entry.value->regExpGlobalData)
                entry.value->regExpGlobalData->visitAggregate(visitor);
#if USE(BUN_JSC_ADDITIONS)
            visitor.append(entry.value->asyncContextData);
#endif
        }
    }

    {
        if (thisObject->m_weakTickets) {
            Locker locker { thisObject->cellLock() };
            for (Ref<DeferredWorkTimer::TicketData> ticket : *thisObject->m_weakTickets) {
                // FIXME: This seems like it should remove the cancelled ticket? Although, it would likely have to deal with deadlocking somehow.
                if (ticket->isCancelled())
                    continue;
                visitor.appendUnbarriered(ticket->scriptExecutionOwner());
                // The check above is just an optimization since between the check and here the mutator could cancel the ticket.
                constexpr bool mayBeCancelled = true;
                for (auto& dependency : ticket->dependencies(mayBeCancelled))
                    visitor.appendUnbarriered(dependency);
            }
        }
    }
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSGlobalObject);

SUPPRESS_ASAN void JSGlobalObject::exposeDollarVM(VM& vm)
{
    RELEASE_ASSERT(g_jscConfig.restrictedOptionsEnabled && Options::useDollarVM());
    PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (getOwnPropertySlot(this, this, vm.propertyNames->builtinNames().dollarVMPrivateName(), slot))
        return;

    JSDollarVM* dollarVM = JSDollarVM::create(vm, JSDollarVM::createStructure(vm, this, m_objectPrototype.get()));

    GlobalPropertyInfo extraStaticGlobals[] = {
        GlobalPropertyInfo(vm.propertyNames->builtinNames().dollarVMPrivateName(), dollarVM, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly),
    };
    addStaticGlobals(extraStaticGlobals);

    putDirect(vm, Identifier::fromString(vm, "$vm"_s), dollarVM, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void JSGlobalObject::addStaticGlobals(std::span<GlobalPropertyInfo> globals)
{
    ScopeOffset startOffset = addVariables(globals.size(), jsUndefined());

    for (auto [i, global] : indexedRange(globals)) {
        // This `configurable = false` is necessary condition for static globals,
        // otherwise lexical bindings can change the result of GlobalVar queries too.
        // We won't be able to declare a global lexical variable with the sanem name to
        // the static globals because configurable = false.
        ASSERT(global.attributes & PropertyAttribute::DontDelete);

        WatchpointSet* watchpointSet = nullptr;
        WriteBarrierBase<Unknown>* variable = nullptr;
        {
            ConcurrentJSLocker locker(symbolTable()->m_lock);
            ScopeOffset offset = symbolTable()->takeNextScopeOffset(locker);
            RELEASE_ASSERT(offset == startOffset + i);
            SymbolTableEntry newEntry(VarOffset(offset), global.attributes);
            newEntry.prepareToWatch();
            watchpointSet = newEntry.watchpointSet();
            symbolTable()->add(locker, global.identifier.impl(), WTF::move(newEntry));
            variable = &variableAt(offset);
        }
        symbolTablePutTouchWatchpointSet(vm(), this, global.identifier, global.value, variable, watchpointSet);
    }
}

bool JSGlobalObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    if (Base::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    return symbolTableGet(uncheckedDowncast<JSGlobalObject>(object), propertyName, slot);
}

void JSGlobalObject::clearRareData(JSCell* cell)
{
    uncheckedDowncast<JSGlobalObject>(cell)->m_rareData = nullptr;
}

template<typename SpeciesWatchpoint>
void JSGlobalObject::tryInstallSpeciesWatchpoint(JSObject* prototype, JSObject* constructor, std::unique_ptr<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>& constructorWatchpoint, std::unique_ptr<SpeciesWatchpoint>& speciesWatchpoint, InlineWatchpointSet& speciesWatchpointSet, HasSpeciesProperty hasSpeciesProperty, GetterSetter* speciesGetterSetter)
{
    RELEASE_ASSERT(!constructorWatchpoint);
    RELEASE_ASSERT(!speciesWatchpoint);

    VM& vm = this->vm();
    DeferTerminationForAWhile deferScope(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // First we need to make sure that the %prototype%.constructor property points to a %constructor%
    // and that %constructor%[Symbol.species] is the primordial GetterSetter.

    // We only initialize once so flattening the structures does not have any real cost.
    Structure* prototypeStructure = prototype->structure();
    if (prototypeStructure->isDictionary())
        prototypeStructure = prototypeStructure->flattenDictionaryStructure(vm, prototype);
    RELEASE_ASSERT(!prototypeStructure->isDictionary());

    auto invalidateWatchpoint = [&] {
        speciesWatchpointSet.invalidate(vm, StringFireDetail("Was not able to set up species watchpoint."));
    };

    PropertySlot constructorSlot(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
    prototype->getOwnPropertySlot(prototype, this, vm.propertyNames->constructor, constructorSlot);
    scope.assertNoException();
    if (constructorSlot.slotBase() != prototype
        || !constructorSlot.isCacheableValue()
        || constructorSlot.getValue(this, vm.propertyNames->constructor) != constructor) {
        invalidateWatchpoint();
        return;
    }

    Structure* constructorStructure = constructor->structure();
    if (constructorStructure->isDictionary())
        constructorStructure = constructorStructure->flattenDictionaryStructure(vm, constructor);

    PropertySlot speciesSlot(constructor, PropertySlot::InternalMethodType::VMInquiry, &vm);
    constructor->getOwnPropertySlot(constructor, this, vm.propertyNames->speciesSymbol, speciesSlot);
    scope.assertNoException();
    switch (hasSpeciesProperty) {
    case HasSpeciesProperty::Yes: {
        if (speciesSlot.slotBase() != constructor
            || !speciesSlot.isCacheableGetter()
            || speciesSlot.getterSetter() != speciesGetterSetter) {
            invalidateWatchpoint();
            return;
        }
        break;
    }
    case HasSpeciesProperty::No: {
        if (!speciesSlot.isUnset()) {
            invalidateWatchpoint();
            return;
        }
        break;
    }
    }

    // Now we need to setup the watchpoints to make sure these conditions remain valid.

    prototypeStructure->startWatchingPropertyForReplacements(vm, constructorSlot.cachedOffset());
    switch (hasSpeciesProperty) {
    case HasSpeciesProperty::Yes:
        constructorStructure->startWatchingPropertyForReplacements(vm, speciesSlot.cachedOffset());
        break;
    case HasSpeciesProperty::No:
        break;
    }

    ObjectPropertyCondition constructorCondition = ObjectPropertyCondition::equivalence(vm, this, prototype, vm.propertyNames->constructor.impl(), constructor);
    ObjectPropertyCondition speciesCondition;
    switch (hasSpeciesProperty) {
    case HasSpeciesProperty::Yes:
        speciesCondition = ObjectPropertyCondition::equivalence(vm, this, constructor, vm.propertyNames->speciesSymbol.impl(), speciesGetterSetter);
        break;
    case HasSpeciesProperty::No:
        speciesCondition = ObjectPropertyCondition::absence(vm, this, constructor, vm.propertyNames->speciesSymbol.impl(), dynamicDowncast<JSObject>(constructor->getPrototypeDirect()));
        break;
    }

    if (!constructorCondition.isWatchable(PropertyCondition::MakeNoChanges) || !speciesCondition.isWatchable(PropertyCondition::MakeNoChanges)) {
        invalidateWatchpoint();
        return;
    }

    // We only watch this from the DFG, and the DFG makes sure to only start watching if the watchpoint is in the IsWatched state.
    RELEASE_ASSERT(!speciesWatchpointSet.isBeingWatched());
    speciesWatchpointSet.touch(vm, "Set up species watchpoint.");

    constructorWatchpoint = makeUnique<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>(this, constructorCondition, speciesWatchpointSet);
    constructorWatchpoint->install(vm);

    speciesWatchpoint = makeUnique<SpeciesWatchpoint>(this, speciesCondition, speciesWatchpointSet);
    speciesWatchpoint->install(vm);
}

void JSGlobalObject::installSaneChainWatchpoints()
{
    ASSERT(!arrayPrototype()->structure()->mayInterceptIndexedAccesses());
    ASSERT(!arrayPrototype()->structure()->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero());
    ASSERT(!arrayPrototype()->structure()->hasPolyProto());
    ASSERT(arrayPrototype()->structure()->storedPrototype() == objectPrototype());
    ASSERT(!hasIndexedProperties(arrayPrototype()->structure()->indexingType()));
    {
        auto result = ObjectPropertyCondition::absenceOfIndexedProperties(*m_vm, this, arrayPrototype(), objectPrototype());
        ASSERT(result.isWatchable(PropertyCondition::MakeNoChanges));
        installObjectAdaptiveStructureWatchpoint(result, m_arrayPrototypeChainIsSaneWatchpointSet);
    }

    ASSERT(!stringPrototype()->structure()->mayInterceptIndexedAccesses());
    ASSERT(!stringPrototype()->structure()->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero());
    ASSERT(!stringPrototype()->structure()->hasPolyProto());
    ASSERT(stringPrototype()->structure()->storedPrototype() == objectPrototype());
    ASSERT(!hasIndexedProperties(stringPrototype()->structure()->indexingType()));
    {
        auto result = ObjectPropertyCondition::absenceOfIndexedProperties(*m_vm, this, stringPrototype(), objectPrototype());
        ASSERT(result.isWatchable(PropertyCondition::MakeNoChanges));
        installObjectAdaptiveStructureWatchpoint(result, m_stringPrototypeChainIsSaneWatchpointSet);
    }

    ASSERT(!promisePrototype()->structure()->mayInterceptIndexedAccesses());
    ASSERT(!promisePrototype()->structure()->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero());
    ASSERT(!promisePrototype()->structure()->hasPolyProto());
    ASSERT(promisePrototype()->structure()->storedPrototype() == objectPrototype());
    ASSERT(!hasIndexedProperties(promisePrototype()->structure()->indexingType()));
    {
        auto result = ObjectPropertyCondition::absenceOfIndexedProperties(*m_vm, this, promisePrototype(), objectPrototype());
        ASSERT(result.isWatchable(PropertyCondition::MakeNoChanges));
        installObjectAdaptiveStructureWatchpoint(result, m_promisePrototypeChainIsSaneWatchpointSet);
    }

    ASSERT(!objectPrototype()->structure()->mayInterceptIndexedAccesses());
    ASSERT(!objectPrototype()->structure()->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero());
    ASSERT(!objectPrototype()->structure()->hasPolyProto());
    ASSERT(objectPrototype()->structure()->storedPrototype() == jsNull());
    ASSERT(!hasIndexedProperties(objectPrototype()->structure()->indexingType()));
    {
        auto result = ObjectPropertyCondition::absenceOfIndexedProperties(*m_vm, this, objectPrototype(), nullptr);
        ASSERT(result.isWatchable(PropertyCondition::MakeNoChanges));
        installObjectAdaptiveStructureWatchpoint(result, m_objectPrototypeChainIsSaneWatchpointSet);
    }
    installChainedWatchpoint(m_objectPrototypeChainIsSaneWatchpointSet, m_arrayPrototypeChainIsSaneWatchpointSet);
    installChainedWatchpoint(m_objectPrototypeChainIsSaneWatchpointSet, m_stringPrototypeChainIsSaneWatchpointSet);
    installChainedWatchpoint(m_objectPrototypeChainIsSaneWatchpointSet, m_promisePrototypeChainIsSaneWatchpointSet);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

void JSGlobalObject::tryInstallArrayBufferSpeciesWatchpoint(ArrayBufferSharingMode sharingMode)
{
    static_assert(static_cast<unsigned>(ArrayBufferSharingMode::Default) == 0);
    static_assert(static_cast<unsigned>(ArrayBufferSharingMode::Shared) == 1);
    unsigned index = static_cast<unsigned>(sharingMode);
    tryInstallSpeciesWatchpoint(arrayBufferPrototype(sharingMode), arrayBufferConstructor(sharingMode), m_arrayBufferPrototypeConstructorWatchpoints[index], m_arrayBufferConstructorSpeciesWatchpoints[index], arrayBufferSpeciesWatchpointSet(sharingMode), HasSpeciesProperty::Yes, arrayBufferSpeciesGetterSetter(sharingMode));
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

inline std::unique_ptr<ObjectAdaptiveStructureWatchpoint>& JSGlobalObject::typedArrayConstructorSpeciesAbsenceWatchpoint(TypedArrayType type)
{
    switch (type) {
    case NotTypedArray:
        RELEASE_ASSERT_NOT_REACHED();
        return m_typedArrayInt8ConstructorSpeciesAbsenceWatchpoint;
#define TYPED_ARRAY_TYPE_CASE(name) case Type ## name: return m_typedArray ## name ## ConstructorSpeciesAbsenceWatchpoint;
        FOR_EACH_TYPED_ARRAY_TYPE(TYPED_ARRAY_TYPE_CASE)
#undef TYPED_ARRAY_TYPE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
    return m_typedArrayInt8ConstructorSpeciesAbsenceWatchpoint;
}

inline std::unique_ptr<ObjectAdaptiveStructureWatchpoint>& JSGlobalObject::typedArrayPrototypeSymbolIteratorAbsenceWatchpoint(TypedArrayType type)
{
    switch (type) {
    case NotTypedArray:
        RELEASE_ASSERT_NOT_REACHED();
        return m_typedArrayInt8PrototypeSymbolIteratorAbsenceWatchpoint;
#define TYPED_ARRAY_TYPE_CASE(name) case Type ## name: return m_typedArray ## name ## PrototypeSymbolIteratorAbsenceWatchpoint;
        FOR_EACH_TYPED_ARRAY_TYPE(TYPED_ARRAY_TYPE_CASE)
#undef TYPED_ARRAY_TYPE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
    return m_typedArrayInt8PrototypeSymbolIteratorAbsenceWatchpoint;
}

inline std::unique_ptr<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>& JSGlobalObject::typedArrayPrototypeConstructorWatchpoint(TypedArrayType type)
{
    switch (type) {
    case NotTypedArray:
        RELEASE_ASSERT_NOT_REACHED();
        return m_typedArrayInt8PrototypeConstructorWatchpoint;
#define TYPED_ARRAY_TYPE_CASE(name) case Type ## name: return m_typedArray ## name ## PrototypeConstructorWatchpoint;
        FOR_EACH_TYPED_ARRAY_TYPE(TYPED_ARRAY_TYPE_CASE)
#undef TYPED_ARRAY_TYPE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
    return m_typedArrayInt8PrototypeConstructorWatchpoint;
}

void JSGlobalObject::tryInstallTypedArraySpeciesWatchpoint(TypedArrayType type)
{
    VM& vm = this->vm();
    auto* prototype = typedArrayPrototype(type);
    auto* constructor = typedArrayConstructor(type);
    auto& watchpointSet = typedArraySpeciesWatchpointSet(type);
    ASSERT(m_typedArrayConstructorSpeciesWatchpoint);
    if (constructor->getPrototypeDirect() != m_typedArraySuperConstructor.get(this)) {
        watchpointSet.invalidate(vm, StringFireDetail("Was not able to set up species watchpoint."));
        return;
    }
    tryInstallSpeciesWatchpoint(prototype, constructor, typedArrayPrototypeConstructorWatchpoint(type), typedArrayConstructorSpeciesAbsenceWatchpoint(type), watchpointSet, HasSpeciesProperty::No, nullptr);
}

void JSGlobalObject::installTypedArrayConstructorSpeciesWatchpoint(JSTypedArrayViewConstructor* constructor)
{
    VM& vm = this->vm();
    PropertySlot slot(constructor, PropertySlot::InternalMethodType::VMInquiry, &vm);
    constructor->getOwnPropertySlot(constructor, this, vm.propertyNames->speciesSymbol.impl(), slot);
    constructor->structure()->startWatchingPropertyForReplacements(vm, slot.cachedOffset());
    ObjectPropertyCondition speciesCondition = ObjectPropertyCondition::equivalence(vm, nullptr, constructor, vm.propertyNames->speciesSymbol.impl(), typedArraySpeciesGetterSetter());
    m_typedArrayConstructorSpeciesWatchpoint = makeUnique<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>(this, speciesCondition, m_typedArrayConstructorSpeciesWatchpointSet);
    m_typedArrayConstructorSpeciesWatchpoint->install(vm);
}

void JSGlobalObject::installTypedArrayIteratorProtocolWatchpoint(JSObject* base, TypedArrayType typedArrayType)
{
    VM& vm = this->vm();

    DeferTerminationForAWhile deferScope(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    auto absenceCondition = [&](PropertyName propertyName) {
        PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool result = base->getOwnPropertySlot(base, this, propertyName, slot);
        RELEASE_ASSERT(!result);
        catchScope.assertNoException();
        RELEASE_ASSERT(slot.isUnset());
        RELEASE_ASSERT(base->getPrototypeDirect() == m_typedArrayProto.get(this));
        return ObjectPropertyCondition::absence(vm, this, base, propertyName.uid(), m_typedArrayProto.get(this));
    };

    ObjectPropertyCondition iteratorCondition = absenceCondition(vm.propertyNames->iteratorSymbol);

    if (!iteratorCondition.isWatchable(PropertyCondition::EnsureWatchability)) {
        typedArrayIteratorProtocolWatchpointSet(typedArrayType).invalidate(vm, StringFireDetail("Was not able to set up iterator protocol watchpoint."));
        return;
    }

    RELEASE_ASSERT(!typedArrayIteratorProtocolWatchpointSet(typedArrayType).isBeingWatched());
    typedArrayIteratorProtocolWatchpointSet(typedArrayType).touch(vm, "Set up iterator protocol watchpoint.");

    typedArrayPrototypeSymbolIteratorAbsenceWatchpoint(typedArrayType) = makeUnique<ObjectAdaptiveStructureWatchpoint>(this, iteratorCondition, typedArrayIteratorProtocolWatchpointSet(typedArrayType));
    typedArrayPrototypeSymbolIteratorAbsenceWatchpoint(typedArrayType)->install(vm);
}

void JSGlobalObject::installTypedArrayPrototypeIteratorProtocolWatchpoint(JSTypedArrayViewPrototype* prototype)
{
    VM& vm = this->vm();
    ObjectPropertyCondition condition = setupAdaptiveWatchpoint(this, prototype, vm.propertyNames->iteratorSymbol);
    m_typedArrayPrototypeSymbolIteratorWatchpoint = makeUnique<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>(this, condition, m_typedArrayPrototypeIteratorProtocolWatchpointSet);
    m_typedArrayPrototypeSymbolIteratorWatchpoint->install(vm);
}

void JSGlobalObject::installNumberPrototypeWatchpoint(NumberPrototype* numberPrototype)
{
    VM& vm = this->vm();
    ASSERT(m_numberToStringWatchpointSet.isStillValid());
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, numberPrototype, vm.propertyNames->toString), m_numberToStringWatchpointSet);
}

void JSGlobalObject::installMapPrototypeWatchpoint(MapPrototype* mapPrototype)
{
    VM& vm = this->vm();
    if (m_mapIteratorProtocolWatchpointSet.isStillValid())
        installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, mapPrototype, vm.propertyNames->iteratorSymbol), m_mapIteratorProtocolWatchpointSet);
    ASSERT(m_mapSetWatchpointSet.isStillValid());
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, mapPrototype, vm.propertyNames->set), m_mapSetWatchpointSet);
}

void JSGlobalObject::installSetPrototypeWatchpoint(SetPrototype* setPrototype)
{
    VM& vm = this->vm();
    if (m_setIteratorProtocolWatchpointSet.isStillValid())
        installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, setPrototype, vm.propertyNames->iteratorSymbol), m_setIteratorProtocolWatchpointSet);
    ASSERT(m_setAddWatchpointSet.isStillValid());
    installObjectPropertyChangeAdaptiveWatchpoint(setupAdaptiveWatchpoint(this, setPrototype, vm.propertyNames->add), m_setAddWatchpointSet);
}

void JSGlobalObject::installObjectAdaptiveStructureWatchpoint(const ObjectPropertyCondition& key, InlineWatchpointSet& watchpointSet)
{
    auto watchpoint = makeUniqueRef<ObjectAdaptiveStructureWatchpoint>(this, key, watchpointSet);
    watchpoint->install(*m_vm);
    m_installedObjectAdaptiveStructureWatchpoints.append(WTF::move(watchpoint));
}

void JSGlobalObject::installObjectPropertyChangeAdaptiveWatchpoint(const ObjectPropertyCondition& key, InlineWatchpointSet& watchpointSet)
{
    auto watchpoint = makeUniqueRef<ObjectPropertyChangeAdaptiveWatchpoint<InlineWatchpointSet>>(this, key, watchpointSet);
    watchpoint->install(*m_vm);
    m_installedObjectPropertyChangeAdaptiveWatchpoints.append(WTF::move(watchpoint));
}

void JSGlobalObject::installChainedWatchpoint(InlineWatchpointSet& from, InlineWatchpointSet& to)
{
    auto watchpoint = makeUniqueRef<ChainedWatchpoint>(this, to);
    watchpoint->install(from, *m_vm);
    m_installedChainedWatchpoints.append(WTF::move(watchpoint));
}

void JSGlobalObject::tryInstallPropertyDescriptorFastPathWatchpoint()
{
    VM& vm = this->vm();

    DeferTerminationForAWhile deferScope(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    auto invalidate = [&]() {
        m_propertyDescriptorFastPathWatchpointSet.invalidate(vm, StringFireDetail("Was not able to set up property descriptor related names watchpoint set."));
    };

    auto absenceCondition = [&](JSObject* base, PropertyName propertyName) -> std::optional<ObjectPropertyCondition> {
        PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool result = base->getOwnPropertySlot(base, this, propertyName, slot);
        if (result)
            return std::nullopt;
        catchScope.assertNoException();
        RELEASE_ASSERT(slot.isUnset());
        return ObjectPropertyCondition::absence(vm, this, base, propertyName.uid(), nullptr);
    };

    if (!objectPrototypeChainIsSane()) {
        invalidate();
        return;
    }

    Vector<ObjectPropertyCondition, 8> conditions;
    {
        auto condition = absenceCondition(objectPrototype(), vm.propertyNames->get);
        if (!condition) {
            invalidate();
            return;
        }
        conditions.append(condition.value());
    }
    {
        auto condition = absenceCondition(objectPrototype(), vm.propertyNames->set);
        if (!condition) {
            invalidate();
            return;
        }
        conditions.append(condition.value());
    }
    {
        auto condition = absenceCondition(objectPrototype(), vm.propertyNames->enumerable);
        if (!condition) {
            invalidate();
            return;
        }
        conditions.append(condition.value());
    }
    {
        auto condition = absenceCondition(objectPrototype(), vm.propertyNames->configurable);
        if (!condition) {
            invalidate();
            return;
        }
        conditions.append(condition.value());
    }
    {
        auto condition = absenceCondition(objectPrototype(), vm.propertyNames->writable);
        if (!condition) {
            invalidate();
            return;
        }
        conditions.append(condition.value());
    }

    for (auto& condition : conditions) {
        if (!condition.isWatchable(PropertyCondition::EnsureWatchability)) {
            invalidate();
            return;
        }
    }

    RELEASE_ASSERT(!m_propertyDescriptorFastPathWatchpointSet.isBeingWatched());
    m_propertyDescriptorFastPathWatchpointSet.touch(vm, "Set up property descriptor fast path watchpoint set.");
    for (auto& condition : conditions)
        installObjectAdaptiveStructureWatchpoint(condition, m_propertyDescriptorFastPathWatchpointSet);
}

void NODELETE slowValidateCell(JSGlobalObject* globalObject)
{
    RELEASE_ASSERT(globalObject->isGlobalObject());
    ASSERT_GC_OBJECT_INHERITS(globalObject, JSGlobalObject::info());
}

void JSGlobalObject::setInspectable(bool inspectable)
{
#if ENABLE(REMOTE_INSPECTOR)
    // FIXME: <http://webkit.org/b/246237> Local inspection should be controlled by `inspectable` API.
    m_inspectorDebuggable->setInspectable(inspectable);
#else
    UNUSED_PARAM(inspectable);
#endif
}

bool JSGlobalObject::inspectable() const
{
#if ENABLE(REMOTE_INSPECTOR)
    // FIXME: <http://webkit.org/b/246237> Local inspection should be controlled by `inspectable` API.
    return m_inspectorDebuggable->inspectable();
#else
    return false;
#endif
}

void JSGlobalObject::setIsITML()
{
#if ENABLE(REMOTE_INSPECTOR)
    m_inspectorDebuggable->setIsITML();
#endif
}

void JSGlobalObject::setName(const String& name)
{
    // UNGIL annex K4 §VIII.9 (U-T8b): m_name is immutable-after-init —
    // embedder configuration written before the global is shared across
    // threads. A post-entry write would race foreign readers on a
    // non-atomic two-word refcounted String (torn pointer + non-atomic
    // ref). Debug fail-stop after the VM's first cross-thread entry
    // (no-op flag-off/GIL-on and in release builds), same wiring as
    // setGlobalThis.
    jsThreadsAssertNoWriteAfterFirstCrossThreadEntry(&vm());
    m_name = name;

#if ENABLE(REMOTE_INSPECTOR)
    m_inspectorDebuggable->update();
#endif
}

void JSGlobalObject::bumpGlobalLexicalBindingEpoch(VM& vm)
{
    if (++m_globalLexicalBindingEpoch == Options::thresholdForGlobalLexicalBindingEpoch()) {
        // Since the epoch overflows, we should rewrite all the CodeBlock to adjust to the newly started generation.
        m_globalLexicalBindingEpoch = 1;
        vm.heap.codeBlockSet().iterate([&] (CodeBlock* codeBlock) {
            if (codeBlock->globalObject() != this)
                return;
            codeBlock->notifyLexicalBindingUpdate();
        });
    }
}

void JSGlobalObject::queueMicrotaskToEventLoop(JSC::JSGlobalObject& globalObject, JSC::QueuedTask&& task)
{
    globalObject.queueMicrotask(globalObject.vm(), WTF::move(task));
}

static bool incumbentRealmIs(VM& vm, JSGlobalObject* target)
{
    bool result = false;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (visitor->isNativeCalleeFrame())
            return IterationStatus::Continue;
        if (auto* codeBlock = visitor->codeBlock()) {
            if (auto* functionExecutable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable()); functionExecutable && functionExecutable->isBuiltinFunction())
                return IterationStatus::Continue;
            if (codeBlock->globalObject() == target) {
                result = true;
                return IterationStatus::Done;
            }
        }
        return IterationStatus::Continue;
    });
    return result;
}

void JSGlobalObject::queueMicrotask(VM& vm, QueuedTask&& task)
{
    if (!m_canFastQueueMicrotask || vm.crossTaskToken()) [[unlikely]] {
        queueMicrotaskSlow(vm, WTF::move(task));
        return;
    }
    // UNGIL §E.1/I11 (U-T9): GIL-off, a SPAWNED thread's enqueue is ALWAYS
    // per-lite — the realm-bound m_microtaskQueue aliases the VM default
    // queue, which only the carrier may drain; routing there would let two
    // threads enqueue/drain one queue (I11/U22 break, r22). The X1.7 host
    // hook (queueMicrotaskToEventLoop) is consulted only on carrier paths;
    // its default forwards here and reroutes identically. Flag-off/GIL-on/
    // main carrier: the landed enqueue, byte-identical.
    if (VMLite* lite = perLiteRealmRoutingLite(vm)) [[unlikely]] {
        lite->enqueueMicrotaskToDefaultQueue(WTF::move(task));
        return;
    }
    // UNGIL §E.1/§E.4 (TSAN family 30, wave-2 amendment): mirror
    // VM::queueMicrotask's third arm. perLiteRealmRoutingLite() returns null
    // not only for the owning main carrier but also when the calling thread
    // has NO current lite (pre-carrier window) or a FOREIGN lite
    // (lite->vm != &vm, cross-VM reentry) — and gilOff the realm-bound queue
    // aliases the VM default queue, which only the main thread's carrier may
    // touch plainly. Such an enqueue must go through the queue's
    // lock-guarded foreign inbox (release(enqueuer)/acquire(drain splice)
    // publishes the task's words before the carrier dequeues/runs/frees
    // them); a plain enqueue here is the corruption-grade unsynchronized
    // Deque write racing the carrier's drain. Flag-off/GIL-on: gilOff() is
    // false, branch not taken, landed enqueue byte-identical.
    if (vm.gilOff() && !WTF::isMainThread()) [[unlikely]] {
        microtaskQueue().enqueueFromForeignThread(WTF::move(task));
        return;
    }
    microtaskQueue().enqueue(WTF::move(task));
}

void JSGlobalObject::queueMicrotask(VM& vm, InternalMicrotask job, uint8_t payload, JSValue argument0, JSValue argument1, JSValue argument2)
{
    queueMicrotask(vm, QueuedTask { nullptr, job, payload, this, argument0, argument1, argument2 });
}

void JSGlobalObject::queueMicrotaskSlow(VM& vm, QueuedTask&& task)
{
    ([&] ALWAYS_INLINE_LAMBDA {
        if (auto* crossTaskToken = vm.crossTaskToken(); crossTaskToken && crossTaskToken->shouldPropagateToMicroTask()) [[unlikely]] {
            if (auto dispatcher = crossTaskToken->createMicrotaskDispatcher(vm, this)) {
                task.setDispatcher(JSMicrotaskDispatcher::create(vm, dispatcher.releaseNonNull(), this));
                return;
            }
        }

        if (debugger()) [[unlikely]] {
            task.setDispatcher(JSMicrotaskDispatcher::create(vm, DebuggableMicrotaskDispatcher::create(), this));
            return;
        }
    }());
    if (!m_associatedContextIsFullyActive) [[unlikely]] {
        if (microtaskQueue().isPerformingMicrotaskCheckpoint() && incumbentRealmIs(vm, this)) [[unlikely]]
            return;
    }
    // UNGIL §E.1/I11 (U-T9): the slow path's dispatcher decoration ran
    // INLINE on the acting thread (the U-T8e CrossTaskToken disposition);
    // the storage routing is the same per-lite reroute as the fast path.
    if (VMLite* lite = perLiteRealmRoutingLite(vm)) [[unlikely]] {
        lite->enqueueMicrotaskToDefaultQueue(WTF::move(task));
        return;
    }
    // Same foreign-inbox arm as the fast path above (UNGIL §E.1/§E.4,
    // family 30 wave-2 amendment): a gilOff enqueue from a non-owner thread
    // (no-lite window or foreign-VM lite) must not touch the owner's plain
    // Deque.
    if (vm.gilOff() && !WTF::isMainThread()) [[unlikely]] {
        microtaskQueue().enqueueFromForeignThread(WTF::move(task));
        return;
    }
    microtaskQueue().enqueue(WTF::move(task));
}

#if USE(BUN_JSC_ADDITIONS)
void JSGlobalObject::queueMicrotask(VM& vm, InternalMicrotask job, uint8_t payload, JSValue argument0, JSValue argument1, JSValue argument2, JSValue argument3)
{
    queueMicrotask(vm, QueuedTask { nullptr, job, payload, this, argument0, argument1, argument2, argument3 });
}
#endif


void JSGlobalObject::setMicrotaskQueue(Ref<MicrotaskQueue>&& queue)
{
    m_microtaskQueue = WTF::move(queue);
}

// UNGIL §E.1b.4/SD15 (U-T8e): the DEFAULT tracker. Disposition CARRIER-QUEUED
// — but the gating lives in VM::promiseRejected (VM.cpp), the hook's only
// effectful arm: a spawned caller's Reject event becomes a handoff record
// there, and the carrier flush re-enters this hook on the carrier, where
// promiseRejected appends to the carrier-confined
// m_aboutToBeNotifiedRejectedPromises vector. Handle is a no-op for the
// default tracker on any thread (didExhaustMicrotaskQueue re-checks
// isHandled() at report time, so a late Handle is absorbed). This function
// therefore stays bit-identical on every thread and in flag-off mode.
void JSGlobalObject::promiseRejectionTracker(JSGlobalObject* globalObject, JSPromise* promise, JSPromiseRejectionOperation operation)
{
    switch (operation) {
    case JSC::JSPromiseRejectionOperation::Handle: {
        break;
    }
    case JSC::JSPromiseRejectionOperation::Reject: {
        globalObject->vm().promiseRejected(promise);
        break;
    }
    }
}

// UNGIL §E.1b.4 (U-T8e): disposition INLINE-SAFE — the default is a no-op on
// any thread. Spawned drains report on the draining thread (SD10-consistent);
// installed hooks carry the §F.6 thread-safe contract (see the disposition
// table at baseGlobalObjectMethodTable).
void JSGlobalObject::reportUncaughtExceptionAtEventLoop(JSGlobalObject*, Exception*)
{
}

void JSGlobalObject::setConsoleClient(WeakPtr<ConsoleClient>&& consoleClient)
{
    m_consoleClient = WTF::move(consoleClient);
}

CheckedPtr<ConsoleClient> JSGlobalObject::consoleClient() const
{
    return m_consoleClient.get();
}

void JSGlobalObject::setDebugger(Debugger* debugger)
{
    m_debugger = debugger;
    updateCanFastQueueMicrotask();
    if (debugger)
        vm().ensureShadowChicken();
}

bool JSGlobalObject::hasInteractiveDebugger() const
{
    return m_debugger && m_debugger->isInteractivelyDebugging();
}

#if ENABLE(DFG_JIT)
WatchpointSet* JSGlobalObject::getReferencedPropertyWatchpointSet(UniquedStringImpl* uid)
{
    ConcurrentJSLocker locker(m_referencedGlobalPropertyWatchpointSetsLock);
    return m_referencedGlobalPropertyWatchpointSets.get(uid);
}

WatchpointSet& JSGlobalObject::ensureReferencedPropertyWatchpointSet(UniquedStringImpl* uid)
{
    ConcurrentJSLocker locker(m_referencedGlobalPropertyWatchpointSetsLock);
    return m_referencedGlobalPropertyWatchpointSets.ensure(uid, [] {
        return WatchpointSet::create(IsWatched);
    }).iterator->value.get();
}
#endif

JSGlobalObject* JSGlobalObject::create(VM& vm, Structure* structure)
{
    JSGlobalObject* globalObject = new (NotNull, allocateCell<JSGlobalObject>(vm)) JSGlobalObject(vm, structure);
    globalObject->finishCreation(vm);
    return globalObject;
}

JSGlobalObject* JSGlobalObject::createWithCustomMethodTable(VM& vm, Structure* structure, const GlobalObjectMethodTable* methodTable)
{
    JSGlobalObject* globalObject = new (NotNull, allocateCell<JSGlobalObject>(vm)) JSGlobalObject(vm, structure, methodTable);
    globalObject->finishCreation(vm);
    return globalObject;
}

void JSGlobalObject::finishCreation(VM& vm)
{
    DeferTermination deferTermination(vm);
    Base::finishCreation(vm);
    structure()->setRealm(vm, this);
    m_runtimeFlags = m_globalObjectMethodTable->javaScriptRuntimeFlags(this);
    init(vm);
    setGlobalThis(vm, JSGlobalProxy::create(vm, JSGlobalProxy::createStructure(vm, this, getPrototypeDirect()), this));
    ASSERT(type() == GlobalObjectType);
}

void JSGlobalObject::finishCreation(VM& vm, JSObject* thisValue)
{
    DeferTermination deferTermination(vm);
    Base::finishCreation(vm);
    structure()->setRealm(vm, this);
    m_runtimeFlags = m_globalObjectMethodTable->javaScriptRuntimeFlags(this);
    init(vm);
    setGlobalThis(vm, thisValue);
    ASSERT(type() == GlobalObjectType);
}

#ifdef JSC_GLIB_API_ENABLED
void JSGlobalObject::setWrapperMap(std::unique_ptr<WrapperMap>&& map)
{
    m_wrapperMap = WTF::move(map);
}
#endif

void JSGlobalObject::addWeakTicket(DeferredWorkTimer::Ticket ticket)
{
    Locker locker { cellLock() };
    if (!m_weakTickets) {
        auto weakTickets = makeUnique<ThreadSafeWeakHashSet<DeferredWorkTimer::TicketData>>();
        WTF::storeStoreFence();
        m_weakTickets = WTF::move(weakTickets);
    }
    m_weakTickets->add(*ticket);
    vm().writeBarrier(this);
}
void JSGlobalObject::clearWeakTickets()
{
    if (!m_weakTickets)
        return;

    WaiterListManager::singleton().unregister(this);
    // Clear the rest tickets safely.
    vm().deferredWorkTimer->cancelPendingWorkSafe(this);
}

FunctionExecutable* JSGlobalObject::tryGetCachedFunctionExecutableForFunctionConstructor(const Identifier& name, StringView program, const SourceOrigin& sourceOrigin, SourceTaintedOrigin sourceTaintedOrigin, const String& sourceURL, const TextPosition& startPosition, LexicallyScopedFeatures lexicallyScopedFeatures, FunctionConstructionMode functionConstructionMode)
{
    if (!defaultCodeGenerationMode().isEmpty())
        return nullptr;

    // Copy the cell pointer out of the Weak slot under the leaf lock: a concurrent
    // cachedFunctionExecutableForFunctionConstructor() performs Weak::set, which swaps and
    // deallocates the previous WeakImpl; a lock-free Weak::get here could dereference that
    // freed WeakImpl (TSAN triage §8.36). Once copied to the stack the cell itself is kept
    // alive by conservative scanning, so all further inspection happens outside the lock.
    FunctionExecutable* executable;
    {
        Locker locker { m_functionConstructorExecutableCacheLock };
        executable = m_executableForCachedFunctionExecutableForFunctionConstructor.get();
    }
    if (!executable)
        return nullptr;

    auto* unlinkedExecutable = executable->unlinkedExecutable();
    if (name != unlinkedExecutable->name())
        return nullptr;

    if (lexicallyScopedFeatures != unlinkedExecutable->lexicallyScopedFeatures())
        return nullptr;

    auto storedSource = executable->source();
    if (OrdinalNumber { } != storedSource.firstLine())
        return nullptr;

    int offset = functionConstructorPrefix(functionConstructionMode).length() + name.length();
    if (offset != storedSource.startColumn().zeroBasedInt())
        return nullptr;

    if (program.substring(offset) != storedSource.view())
        return nullptr;

    RefPtr storedProvider = executable->source().provider();
    if (storedProvider->startPosition() != startPosition)
        return nullptr;

    if (storedProvider->sourceOrigin() != sourceOrigin)
        return nullptr;

    if (storedProvider->sourceURL() != sourceURL)
        return nullptr;

    if (storedProvider->sourceTaintedOrigin() != sourceTaintedOrigin)
        return nullptr;

    return executable;
}

void JSGlobalObject::cachedFunctionExecutableForFunctionConstructor(FunctionExecutable* executable)
{
    if (!defaultCodeGenerationMode().isEmpty())
        return;
    if (executable->source().provider()->couldBeTainted())
        return;
    auto* unlinkedExecutable = executable->unlinkedExecutable();
    if (unlinkedExecutable->features() & NoEvalCacheFeature)
        return;
    // SPEC-ungil §LK WS row (i): Weak CREATION (MSPL under ISS) is forbidden
    // under leaf locks — the WeakImpl allocation can hit a heap slow path
    // that parks at a safepoint, and a second mutator blocked on this lock
    // would then be safepoint-blind (the ab17b watchdog-timeout family).
    // Construct the Weak BEFORE the lock, publish under it with a pointer
    // swap only, and let the displaced Weak's WeakSet::deallocate run AFTER
    // the lock is released.
    Weak<FunctionExecutable> newWeak(executable);
    {
        Locker locker { m_functionConstructorExecutableCacheLock };
        m_executableForCachedFunctionExecutableForFunctionConstructor.swap(newWeak);
    }
    // `newWeak` now holds the displaced previous Weak (possibly empty) and
    // deallocates it here, outside the lock.
}

#if ENABLE(REMOTE_INSPECTOR)
Inspector::JSGlobalObjectInspectorController& JSGlobalObject::inspectorController() const
{
    return *m_inspectorController.get();
}
#endif

} // namespace JSC
