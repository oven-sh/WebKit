/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonBuiltins.h"

#include "GlobalObjectMethodTable.h"
#include "PyFrame.h"
#include "PyInstance.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonConfiguration.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonSequences.h"
#include "PythonStandardLibraryNames.h"
#include "PythonStrings.h"
#include "TopExceptionScope.h"
#include <wtf/text/StringBuilder.h>

// The sys module: what a program sees, and can set, of the interpreter itself. This is CPython's Python/sysmodule.c, and the parts of Python/errors.c and
// Python/pythonrun.c that call what a program has put in it.

namespace JSC { namespace Python {

PYTHON_NATIVE(returnNone)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

PYTHON_NATIVE(returnFalse)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(false));
}

// ---- Writing to sys.stderr

// file.write(text), and nothing if that cannot be done.
static void writeTo(JSGlobalObject* globalObject, JSValue file, const String& text)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!file || isNone(file))
        return;
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    if (!scope.exception())
        call(globalObject, write, jsString(vm, text));
    scope.clearException();
}

static void writeToStandardError(JSGlobalObject* globalObject, const String& text)
{
    writeTo(globalObject, sysAttribute(globalObject, "stderr"_s), text);
}

// ---- Audit hooks

bool auditSlow(JSGlobalObject* globalObject, ASCIILiteral event, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* hooks = globalObject->pyRealm()->auditHooks();
    if (!hooks)
        return true;
    JSValue name = jsString(vm, String(event));
    JSValue tuple = PyTuple::createFromArguments(globalObject, arguments);
    // One that is added while they are being told is told too.
    for (unsigned i = 0; i < hooks->length(); ++i) {
        JSValue hook = listGet(globalObject, hooks, i);
        RETURN_IF_EXCEPTION(scope, false);
        // Nothing is told of what a hook does, by sys.settrace() and its like, unless the hook says that it may be.
        JSValue canBeTraced = getAttributeIfPresent(globalObject, hook, Identifier::fromString(vm, "__cantrace__"_s));
        RETURN_IF_EXCEPTION(scope, false);
        bool isTraced = canBeTraced && isTrue(globalObject, canBeTraced);
        RETURN_IF_EXCEPTION(scope, false);
        unsigned& callbackDepth = globalObject->pyRealm()->monitoring().callbackDepth;
        callbackDepth += !isTraced;
        call(globalObject, hook, name, tuple);
        callbackDepth -= !isTraced;
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

PYTHON_NATIVE(sysAddAuditHook)
{
    NATIVE_PROLOGUE();
    // Those that are there already can refuse it, which is not an error.
    audit(globalObject, "sys.addaudithook"_s);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::Exception))
            RETURN_NONE();
        return { };
    }
    JSArray* hooks = realm->auditHooks();
    if (!hooks) {
        hooks = newList(globalObject);
        realm->setAuditHooks(vm, hooks);
    }
    listAppend(globalObject, hooks, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// sys.audit(event, *args)
PYTHON_NATIVE(sysAudit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "sys.audit() takes no keyword arguments"_s));
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, "audit expected at least 1 argument, got 0"_s));
    if (!stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("audit() argument 1 must be str, not "_s, isNone(args[0]) ? "None"_s : typeName(globalObject, args[0]))));
    if (!realm->auditHooks())
        RETURN_NONE();
    MarkedArgumentBuffer rest;
    for (unsigned i = 1; i < args.size(); ++i)
        rest.append(args[i]);
    JSValue tuple = PyTuple::createFromArguments(globalObject, rest);
    JSArray* hooks = realm->auditHooks();
    for (unsigned i = 0; i < hooks->length(); ++i) {
        JSValue hook = listGet(globalObject, hooks, i);
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, hook, args[0], tuple);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- What is raised and not caught

// sys.excepthook(exctype, value, traceback)
PYTHON_NATIVE(sysExceptHook)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue value = args[1];
    if (isNone(value)) {
        writeToStandardError(globalObject, "NoneType: None\n"_s);
        RETURN_NONE();
    }
    if (!isInstance(globalObject, value, realm->typeBaseException())) {
        writeToStandardError(globalObject, concatenate("TypeError: print_exception(): Exception expected for value, "_s, typeName(globalObject, value), " found\n"_s));
        RETURN_NONE();
    }
    // One that has no traceback of its own is given the one that came with it.
    JSValue own = asObject(value)->getDirect(vm, names.private_traceback);
    if ((!own || isNone(own)) && typeOf(globalObject, args[2]) == realm->typeTraceback())
        asObject(value)->putDirect(vm, names.private_traceback, args[2]);
    writeToStandardError(globalObject, formatException(globalObject, value));
    RETURN_NONE();
}

void reportUncaughtException(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue type = typeOf(globalObject, exception)->object();
    JSValue traceback = exception.isObject() ? asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback) : JSValue();
    if (!traceback)
        traceback = jsUndefined();
    {
        JSObject* sys = globalObject->pyRealm()->sysModule();
        for (auto [name, value] : { std::pair { "last_exc"_s, exception }, std::pair { "last_type"_s, type }, std::pair { "last_value"_s, exception }, std::pair { "last_traceback"_s, traceback } })
            putStoredAttribute(vm, sys, Identifier::fromString(vm, name), value);
    }
    JSValue hook = sysAttribute(globalObject, "excepthook"_s);
    audit(globalObject, "sys.excepthook"_s, hook ? hook : jsUndefined(), type, exception, traceback);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::RuntimeError))
            return;
        reportUnraisable(globalObject, "Exception ignored in audit hook"_s);
    }
    if (!hook) {
        writeToStandardError(globalObject, "sys.excepthook is missing\n"_s);
        writeToStandardError(globalObject, formatException(globalObject, exception));
        return;
    }
    call(globalObject, hook, type, exception, traceback);
    Exception* second = scope.exception();
    if (!second)
        return;
    scope.clearException();
    writeToStandardError(globalObject, "Error in sys.excepthook:\n"_s);
    writeToStandardError(globalObject, formatException(globalObject, second->value()));
    writeToStandardError(globalObject, "\nOriginal exception was:\n"_s);
    writeToStandardError(globalObject, formatException(globalObject, exception));
}

// ---- What is raised where there is nobody to catch it

namespace UnraisableField {
enum Field : unsigned { Type, Value, Traceback, Message, Object };
}

// write_unraisable_exc_file() of CPython's Python/errors.c
static void writeUnraisable(JSGlobalObject* globalObject, JSValue type, JSValue value, JSValue traceback, JSValue message, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue file = sysAttribute(globalObject, "stderr"_s);
    if (!file || isNone(file))
        return;
    auto textOf = [&] (JSValue something, bool asRepr, ASCIILiteral ifItFails) -> String {
        String text = asRepr ? repr(globalObject, something) : str(globalObject, something);
        if (scope.exception()) {
            scope.clearException();
            return ifItFails;
        }
        return text;
    };
    TextBuilder builder;
    bool hasMessage = message && !isNone(message);
    if (object && !isNone(object)) {
        if (hasMessage)
            builder.append(textOf(message, false, ""_s), ": "_s);
        else
            builder.append("Exception ignored in: "_s);
        builder.append(textOf(object, true, "<object repr() failed>"_s), '\n');
    } else if (hasMessage)
        builder.append(textOf(message, false, ""_s), ":\n"_s);
    builder.append(formatTraceback(globalObject, traceback));
    if (!type || isNone(type)) {
        writeTo(globalObject, file, builder.tryFinish());
        return;
    }
    JSValue module = getAttributeIfPresent(globalObject, type, vm.pythonNames().dunder_module);
    scope.clearException();
    if (JSString* name = module ? stringIn(module) : nullptr) {
        auto text = name->value(globalObject);
        if (text.data != "builtins"_s && text.data != "__main__"_s)
            builder.append(text.data, '.');
    } else
        builder.append("<unknown>"_s);
    if (isClass(type))
        builder.append(qualifiedNameWithoutModule(globalObject, asType(type)));
    else
        builder.append("<unknown>"_s);
    if (value && !isNone(value))
        builder.append(": "_s, textOf(value, false, "<exception str() failed>"_s));
    builder.append('\n');
    writeTo(globalObject, file, builder.tryFinish());
}

// sys.unraisablehook(unraisable)
PYTHON_NATIVE(sysUnraisableHook)
{
    NATIVE_PROLOGUE();
    if (typeOf(globalObject, args[0]) != realm->typeUnraisableHookArgs())
        return JSValue::encode(raiseTypeError(globalObject, scope, "sys.unraisablehook argument type must be UnraisableHookArgs"_s));
    PyTuple* unraisable = asTuple(args[0]);
    writeUnraisable(globalObject, unraisable->at(UnraisableField::Type), unraisable->at(UnraisableField::Value), unraisable->at(UnraisableField::Traceback), unraisable->at(UnraisableField::Message), unraisable->at(UnraisableField::Object));
    // As it turns out in CPython, where this is taken for having failed and nothing has been raised to say how.
    if (isNone(unraisable->at(UnraisableField::Type)))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "<built-in function unraisablehook> returned NULL without setting an exception"_s));
    RETURN_NONE();
}

void reportUnraisable(JSGlobalObject* globalObject, const String& givenMessage, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    Exception* raised = scope.exception();
    if (!raised)
        return;
    scope.clearException();

    auto partsOf = [&] (JSValue exception, JSValue& type, JSValue& traceback) {
        type = typeOf(globalObject, exception)->object();
        traceback = exception.isObject() ? asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback) : JSValue();
        if (!traceback)
            traceback = jsUndefined();
    };
    JSValue exception = raised->value();
    JSValue type;
    JSValue traceback;
    partsOf(exception, type, traceback);
    JSValue message = givenMessage.isNull() ? jsUndefined() : JSValue(jsString(vm, givenMessage));
    if (!object)
        object = jsUndefined();

    // If telling the hook goes wrong, that is what is written, by what the hook is at first.
    auto failed = [&] (ASCIILiteral why, JSValue about) {
        exception = scope.exception()->value();
        scope.clearException();
        partsOf(exception, type, traceback);
        writeUnraisable(globalObject, type, exception, traceback, jsString(vm, String(why)), about);
    };
    MarkedArgumentBuffer fields;
    for (JSValue field : { type, exception, traceback, message, object })
        fields.append(field);
    JSValue unraisable = newStructSequence(globalObject, realm->typeUnraisableHookArgs(), fields);
    JSValue hook = sysAttribute(globalObject, "unraisablehook"_s);
    if (hook) {
        audit(globalObject, "sys.unraisablehook"_s, hook, unraisable);
        if (scope.exception())
            return failed("Exception ignored in audit hook"_s, jsUndefined());
    }
    if (!hook || isNone(hook)) {
        writeUnraisable(globalObject, type, exception, traceback, message, object);
        return;
    }
    call(globalObject, hook, unraisable);
    if (scope.exception())
        failed("Exception ignored in sys.unraisablehook"_s, hook);
}

void reportUnraisableShowing(JSGlobalObject* globalObject, ASCIILiteral message, JSValue shown)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!scope.exception() || vm.hasPendingTerminationException())
        return;
    // What is being said is about what was raised, which is kept meanwhile.
    Exception* raised = takeRaisedException(vm);
    String text = repr(globalObject, shown);
    if (scope.exception()) {
        if (vm.hasPendingTerminationException())
            return;
        scope.clearException();
        text = { };
    } else
        text = concatenate(message, ' ', text);
    restoreRaisedException(globalObject, raised);
    reportUnraisable(globalObject, text);
}

// ---- breakpoint()

// sys.breakpointhook(*args, **kws): what $PYTHONBREAKPOINT names is called, which is pdb.set_trace if it names nothing.
PYTHON_NATIVE(sysBreakpointHook)
{
    NATIVE_PROLOGUE();
    const char* variable = getenv("PYTHONBREAKPOINT");
    String named = variable && *variable ? String::fromUTF8(variable) : "pdb.set_trace"_str;
    if (named == "0"_s)
        RETURN_NONE();
    size_t lastDot = named.reverseFind('.');
    // What cannot be imported is warned of and passed over.
    auto ignore = [&] {
        warn(globalObject, BuiltinType::RuntimeWarning, concatenate("Ignoring unimportable $PYTHONBREAKPOINT: \""_s, named, '"'), 0);
        return JSValue::encode(jsUndefined());
    };
    if (!lastDot)
        return ignore();
    String moduleName = lastDot == notFound ? "builtins"_str : named.left(lastDot);
    String attribute = lastDot == notFound ? named : named.substring(lastDot + 1);
    JSValue module = importModule(globalObject, moduleName);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::ImportError))
            return ignore();
        return { };
    }
    JSValue hook = getAttribute(globalObject, module, Identifier::fromString(vm, attribute));
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::AttributeError))
            return ignore();
        return { };
    }
    ArgList arguments = args.allFrom(0);
    RELEASE_AND_RETURN(scope, JSValue::encode(args.keywordNames() ? callWithKeywords(globalObject, hook, arguments, args.keywordNames()) : call(globalObject, hook, arguments)));
}

// breakpoint(*args, **kws), of builtins
PYTHON_NATIVE(builtinBreakpoint)
{
    NATIVE_PROLOGUE();
    JSValue hook = sysAttribute(globalObject, "breakpointhook"_s);
    if (!hook)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.breakpointhook"_s));
    audit(globalObject, "builtins.breakpoint"_s, hook);
    RETURN_IF_EXCEPTION(scope, { });
    ArgList arguments = args.allFrom(0);
    RELEASE_AND_RETURN(scope, JSValue::encode(args.keywordNames() ? callWithKeywords(globalObject, hook, arguments, args.keywordNames()) : call(globalObject, hook, arguments)));
}

// ---- Objects

PYTHON_NATIVE(sysIntern)
{
    NATIVE_PROLOGUE();
    if (args[0].isString())
        RELEASE_AND_RETURN(scope, JSValue::encode(realm->intern(globalObject, asString(args[0]))));
    if (stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("can't intern "_s, typeName(globalObject, args[0]))));
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("intern() argument must be str, not "_s, isNone(args[0]) ? "None"_s : typeName(globalObject, args[0]))));
}

PYTHON_NATIVE(sysIsInterned)
{
    NATIVE_PROLOGUE();
    if (!stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_is_interned() argument must be str, not "_s, isNone(args[0]) ? "None"_s : typeName(globalObject, args[0]))));
    return JSValue::encode(jsBoolean(args[0].isString() && realm->isInterned(globalObject, asString(args[0]))));
}

// sys.getsizeof(object[, default]): what __sizeof__() says, and what CPython keeps in front of an object of the kind.
PYTHON_NATIVE(sysGetSizeOf)
{
    NATIVE_PROLOGUE();
    JSValue object = args.at(0);
    JSValue fallback = args.at(1);
    auto sizeOf = [&] () -> JSValue {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, object, Identifier::fromString(vm, "__sizeof__"_s), self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return raiseTypeError(globalObject, scope, concatenate("Type "_s, typeName(globalObject, object), " doesn't define __sizeof__"_s));
        JSValue result = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInstance(globalObject, result, realm->typeInt()))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        auto size = toSsize(globalObject, result);
        RETURN_IF_EXCEPTION(scope, { });
        if (*size < 0)
            return raiseValueError(globalObject, scope, "__sizeof__() should return >= 0"_s);
        constexpr unsigned long isCollected = 1ul << 14;
        constexpr unsigned long hasPreheader = (1ul << 3) | (1ul << 4);
        int64_t inFront = 0;
        if (!isType(object) || asType(object)->metatype() != realm->typeType() || asType(object)->hasFlag(PyType::IsHeapType)) {
            unsigned long flags = typeOf(globalObject, object)->flagsForPython();
            inFront = (flags & isCollected ? 16 : 0) + (flags & hasPreheader ? 16 : 0);
        }
        return intFromInt64(globalObject, *size + inFront);
    };
    JSValue size = sizeOf();
    if (scope.exception()) {
        if (fallback && catchException(globalObject, BuiltinType::TypeError))
            return JSValue::encode(fallback);
        return { };
    }
    return JSValue::encode(size);
}

// Nothing is counted here. What is said is what CPython says of something that only the caller refers to, or of what is never freed.
PYTHON_NATIVE(sysGetRefCount)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(args[0].isCell() && !args[0].isString() ? jsNumber(2) : intFromInt64(globalObject, int64_t { 3 } << 30));
}

PYTHON_NATIVE(sysIsImmortal)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsBoolean(!args[0].isCell()));
}

PYTHON_NATIVE(sysGetAllocatedBlocks)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(intFromInt64(globalObject, globalObject->vm().heap.objectCount()));
}

PYTHON_NATIVE(returnZero)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(0));
}

// ---- Limits and settings

PYTHON_NATIVE(sysGetRecursionLimit)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(globalObject->vm().pythonRecursionLimit()));
}

PYTHON_NATIVE(sysSetRecursionLimit)
{
    NATIVE_PROLOGUE();
    auto limit = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*limit < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "recursion limit must be greater or equal than 1"_s));
    // It is not to be made so low that where this is called from is already beyond it.
    if (vm.pythonDepth() >= static_cast<uint32_t>(*limit))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RecursionError, concatenate("cannot set the recursion limit to "_s, *limit, " at the recursion depth "_s, vm.pythonDepth(), ": the limit is too low"_s)));
    vm.setPythonRecursionLimit(*limit);
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetIntMaxStrDigits)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(globalObject->pyRealm()->maximumDigitsOfIntAsString));
}

PYTHON_NATIVE(sysSetIntMaxStrDigits)
{
    NATIVE_PROLOGUE();
    auto digits = toCInt(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    constexpr int threshold = 640;
    if (*digits && *digits < threshold)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("maxdigits must be >= "_s, threshold, " or 0 for unlimited"_s)));
    realm->maximumDigitsOfIntAsString = *digits;
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetSwitchInterval)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(floatFromDouble(globalObject->pyRealm()->switchInterval));
}

PYTHON_NATIVE(sysSetSwitchInterval)
{
    NATIVE_PROLOGUE();
    auto interval = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*interval <= 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "switch interval must be strictly positive"_s));
    realm->switchInterval = *interval;
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetCoroutineOriginTrackingDepth)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(globalObject->pyRealm()->coroutineOriginTrackingDepth));
}

PYTHON_NATIVE(sysSetCoroutineOriginTrackingDepth)
{
    NATIVE_PROLOGUE();
    auto depth = toCInt(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (*depth < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "depth must be >= 0"_s));
    realm->coroutineOriginTrackingDepth = *depth;
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetAsyncGeneratorHooks)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MarkedArgumentBuffer hooks;
    for (JSValue hook : { realm->asyncGeneratorFirstIterationHook(), realm->asyncGeneratorFinalizerHook() })
        hooks.append(hook ? hook : jsUndefined());
    return JSValue::encode(newStructSequence(globalObject, realm->typeSysAsyncGeneratorHooks(), hooks));
}

// sys.set_asyncgen_hooks([firstiter] [, finalizer]). One that is not given is left as it is, and None is for none.
PYTHON_NATIVE(sysSetAsyncGeneratorHooks)
{
    NATIVE_PROLOGUE();
    // It does not say what it is called.
    if (unsigned given = args.size() + args.keywordCount(); given > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("function takes at most 2 arguments ("_s, given, " given)"_s)));
    JSValue firstIteration = args.at(0);
    JSValue finalizer = args.at(1);
    if (finalizer && !isNone(finalizer) && !isCallable(globalObject, finalizer))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("callable finalizer expected, got "_s, typeName(globalObject, finalizer))));
    if (firstIteration && !isNone(firstIteration) && !isCallable(globalObject, firstIteration))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("callable firstiter expected, got "_s, typeName(globalObject, firstIteration))));
    if (finalizer) {
        audit(globalObject, "sys.set_asyncgen_hooks_finalizer"_s);
        RETURN_IF_EXCEPTION(scope, { });
        realm->setAsyncGeneratorFinalizerHook(vm, isNone(finalizer) ? JSValue() : finalizer);
    }
    if (firstIteration) {
        audit(globalObject, "sys.set_asyncgen_hooks_firstiter"_s);
        RETURN_IF_EXCEPTION(scope, { });
        realm->setAsyncGeneratorFirstIterationHook(vm, isNone(firstIteration) ? JSValue() : firstIteration);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(sysReturnText)
{
    static constexpr ASCIILiteral texts[] = { "utf-8"_s, "surrogateescape"_s };
    return JSValue::encode(jsNontrivialString(globalObject->vm(), texts[unpack<unsigned>(callFrame, 0)]));
}

// ---- Frames

// sys.call_tracing(func, args)
PYTHON_NATIVE(sysClearTypeCache)
{
    warn(globalObject, BuiltinType::DeprecationWarning, "sys._clear_type_cache() is deprecated and scheduled for removal in a future version. Use sys._clear_internal_caches() instead."_s);
    RETURN_NONE();
}

PYTHON_NATIVE(sysCallTracing)
{
    NATIVE_PROLOGUE();
    if (!isTuple(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("call_tracing() argument 2 must be tuple, not "_s, isNone(args[1]) ? "None"_s : typeName(globalObject, args[1]))));
    MarkedArgumentBuffer arguments;
    for (auto& argument : asTuple(args[1])->span())
        arguments.append(argument.get());
    // What is called is told of, though this be called by what is being told of something.
    SetForScope callbackDepth(realm->monitoring().callbackDepth, 0u);
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, args[0], arguments)));
}

// There is one thread, and this is what it goes by.
static JSValue identifierOfMainThread() { return jsNumber(1); }

PYTHON_NATIVE(sysCurrentFrames)
{
    NATIVE_PROLOGUE();
    audit(globalObject, "sys._current_frames"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue getFrame = sysAttribute(globalObject, "_getframe"_s);
    PyDict* result = PyDict::create(globalObject);
    JSValue frame = call(globalObject, getFrame);
    RETURN_IF_EXCEPTION(scope, { });
    result->set(globalObject, identifierOfMainThread(), frame);
    return JSValue::encode(result);
}

PYTHON_NATIVE(sysCurrentExceptions)
{
    NATIVE_PROLOGUE();
    audit(globalObject, "sys._current_exceptions"_s);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict* result = PyDict::create(globalObject);
    JSValue handled = realm->handledException();
    result->set(globalObject, identifierOfMainThread(), handled ? handled : jsUndefined());
    return JSValue::encode(result);
}

// ---- What has no meaning here, and says so as CPython does where it has none there

PYTHON_NATIVE(sysActivateStackTrampoline)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseValueError(globalObject, scope, "perf trampoline not available"_s));
}

PYTHON_NATIVE(sysGetCpuCountConfig)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(-1));
}

PYTHON_NATIVE(returnTrue)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(true));
}

PYTHON_NATIVE(sysExit)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemExit, args.at(0)));
}

PYTHON_NATIVE(sysException)
{
    UNUSED_PARAM(callFrame);
    JSValue handled = globalObject->pyRealm()->handledException();
    return JSValue::encode(handled ? handled : jsUndefined());
}

PYTHON_NATIVE(sysExcInfo)
{
    UNUSED_PARAM(callFrame);
    JSValue handled = globalObject->pyRealm()->handledException();
    if (!handled)
        return JSValue::encode(PyTuple::create(globalObject, { jsUndefined(), jsUndefined(), jsUndefined() }));
    JSValue traceback = handled.isObject() ? asObject(handled)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_traceback) : JSValue();
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, handled)->object(), handled, traceback ? traceback : jsUndefined() }));
}

// ---- The standard streams

// stream.write(text), for sys.stdout and sys.stderr. What is written goes straight to the host, as posix.write(). Whether to keep it for a while
// is up to the host, which has JavaScript's output to put it in order with.
PYTHON_NATIVE(standardStreamWrite)
{
    NATIVE_PROLOGUE();
    int descriptor = asObject(args[0])->getDirect(vm, names.private_descriptor).asInt32();
    JSValue text = args[1];
    if (!text.isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("write() argument must be str, not "_s, typeName(globalObject, text))));
    // What cannot be written is an error, but not in the middle of reporting one.
    auto bytes = encodeString(globalObject, text, "utf-8"_s, descriptor == 2 ? "backslashreplace"_s : "strict"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (const FileOperations* files = fileOperations(globalObject)) {
        auto rest = bytes->span();
        while (!rest.empty()) {
            int64_t written = files->write(descriptor, rest);
            if (written == -EINTR)
                continue;
            if (written < 0)
                return JSValue::encode(raiseOSError(globalObject, scope, static_cast<int>(-written)));
            rest = rest.subspan(static_cast<size_t>(written));
        }
    }
    return JSValue::encode(jsNumber(stringLength(globalObject, asString(text))));
}

static JSObject* createPreliminaryStream(JSGlobalObject* globalObject, int descriptor)
{
    VM& vm = globalObject->vm();
    PyType* type = globalObject->pyRealm()->typeStandardStream();
    if (!type->getDirect(vm, Identifier::fromString(vm, "write"_s))) {
        addMethods(globalObject, type, {
            { "write"_s, standardStreamWrite, PyNativeFunction::Kind::Method, 0, "($self, text, /)"_s },
            { "flush"_s, returnNone, PyNativeFunction::Kind::Method, 0, "($self, /)"_s },
        });
    }
    JSObject* stream = PyInstance::create(vm, type->instanceStructure());
    stream->putDirect(vm, vm.pythonNames().private_descriptor, jsNumber(descriptor));
    return stream;
}

// sys.displayhook(value): what is done with what an expression comes to at a prompt.
PYTHON_NATIVE(sysDisplayHook)
{
    NATIVE_PROLOGUE();
    if (isNone(args[0]))
        RETURN_NONE();
    // It is unset meanwhile, so that showing it cannot come back here with it.
    JSObject* builtins = realm->builtinsModule();
    auto underscore = Identifier::fromString(vm, "_"_s);
    builtins->putDirect(vm, underscore, jsUndefined());
    JSValue file = sysAttribute(globalObject, "stdout"_s);
    if (!file || isNone(file))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.stdout"_s));
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, { });
    call(globalObject, write, jsString(vm, text));
    RETURN_IF_EXCEPTION(scope, { });
    call(globalObject, write, vm.smallStrings.singleCharacterString('\n'));
    RETURN_IF_EXCEPTION(scope, { });
    builtins->putDirect(vm, underscore, args[0]);
    RETURN_NONE();
}

// ---- Setting it up

JSObject* createSysModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "sys"_s);
    auto set = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    auto text = [&] (ASCIILiteral value) -> JSValue { return jsString(vm, String(value)); };
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    // What a program can replace is there a second time, under a name that says that it is the original.
    auto addHook = [&] (ASCIILiteral name, ASCIILiteral original, NativeFunction function) {
        add(name, function);
        set(original, module->getDirect(vm, Identifier::fromString(vm, name)));
    };

    const Configuration& configuration = realm->configuration();
    auto listOf = [&] (const Vector<String>& strings) {
        MarkedArgumentBuffer values;
        for (auto& string : strings)
            values.append(jsString(vm, string));
        return newList(globalObject, values);
    };
    set("modules"_s, realm->modules());
    set("path"_s, listOf(configuration.moduleSearchPaths));
    set("meta_path"_s, newList(globalObject));
    set("path_hooks"_s, newList(globalObject));
    set("path_importer_cache"_s, PyDict::create(globalObject));
    set("builtin_module_names"_s, builtinModuleNames(globalObject));
    MarkedArgumentBuffer libraryNames;
    for (ASCIILiteral name : standardLibraryModuleNames)
        libraryNames.append(jsString(vm, String(name)));
    set("stdlib_module_names"_s, setFromIterable(globalObject, realm->typeFrozenSet()->instanceStructure(), newList(globalObject, libraryNames)));
    set("prefix"_s, jsString(vm, configuration.prefix));
    set("base_prefix"_s, jsString(vm, configuration.prefix));
    set("exec_prefix"_s, jsString(vm, configuration.executablePrefix));
    set("base_exec_prefix"_s, jsString(vm, configuration.executablePrefix));
    set("_stdlib_dir"_s, configuration.libraryDirectory.isNull() ? jsUndefined() : JSValue(jsString(vm, configuration.libraryDirectory)));
    set("_base_executable"_s, jsString(vm, configuration.executable));
    set("_home"_s, jsUndefined());
    set("_framework"_s, jsEmptyString(vm));
    set("argv"_s, listOf(configuration.arguments));
    set("orig_argv"_s, listOf(configuration.arguments));
    set("executable"_s, jsString(vm, configuration.executable));
    set("warnoptions"_s, newList(globalObject));
    set("_xoptions"_s, PyDict::create(globalObject));
    set("dont_write_bytecode"_s, jsBoolean(true));
    set("pycache_prefix"_s, jsUndefined());

    // ---- Which Python this is
    auto structOf = [&] (PyType* type, std::span<const ASCIILiteral> fields, unsigned countInSequence, std::initializer_list<JSValue> values) {
        makeStructSequenceType(globalObject, type, fields, countInSequence);
        MarkedArgumentBuffer buffer;
        for (JSValue value : values)
            buffer.append(value);
        return newStructSequence(globalObject, type, buffer);
    };
    constexpr int major = 3;
    constexpr int minor = 14;
    constexpr int micro = 7;
    constexpr int hexVersion = major << 24 | minor << 16 | micro << 8 | 0xF0;
    static constexpr ASCIILiteral versionFields[] = { "major"_s, "minor"_s, "micro"_s, "releaselevel"_s, "serial"_s };
    JSValue versionInfo = structOf(realm->typeSysVersionInfo(), versionFields, 5, { jsNumber(major), jsNumber(minor), jsNumber(micro), text("final"_s), jsNumber(0) });
    set("version_info"_s, versionInfo);
    set("hexversion"_s, jsNumber(hexVersion));
    set("version"_s, strOrMemoryError(globalObject, concatenate(major, '.', minor, '.', micro, " (JavaScriptCore)"_s)));
    set("api_version"_s, jsNumber(1013));
    set("abiflags"_s, jsEmptyString(vm));
    set("copyright"_s, text("Copyright (c) 2001 Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) 1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."_s));
    JSObject* implementation = newSimpleNamespace(globalObject);
    auto describe = [&] (ASCIILiteral name, JSValue value) { putStoredAttribute(vm, implementation, Identifier::fromString(vm, name), value); };
    describe("name"_s, jsString(vm, configuration.implementationName));
    // What goes in the name of a file of compiled code, so that each implementation and version has its own. None are written yet: see dont_write_bytecode.
    describe("cache_tag"_s, strOrMemoryError(globalObject, concatenate(configuration.implementationName, '-', major, minor)));
    describe("version"_s, versionInfo);
    describe("hexversion"_s, jsNumber(hexVersion));
    set("implementation"_s, implementation);
#if OS(DARWIN)
    set("platform"_s, text("darwin"_s));
#elif OS(WINDOWS)
    set("platform"_s, text("win32"_s));
#else
    set("platform"_s, text("linux"_s));
#endif
    set("platlibdir"_s, text("lib"_s));
    set("byteorder"_s, text("little"_s));
    set("maxsize"_s, intFromInt64(globalObject, std::numeric_limits<int64_t>::max()));
    set("maxunicode"_s, jsNumber(0x10FFFF));
    set("float_repr_style"_s, text("short"_s));

    static constexpr ASCIILiteral flagFields[] = { "debug"_s, "inspect"_s, "interactive"_s, "optimize"_s, "dont_write_bytecode"_s, "no_user_site"_s, "no_site"_s, "ignore_environment"_s, "verbose"_s,
        "bytes_warning"_s, "quiet"_s, "hash_randomization"_s, "isolated"_s, "dev_mode"_s, "utf8_mode"_s, "warn_default_encoding"_s, "safe_path"_s, "int_max_str_digits"_s, "gil"_s,
        "thread_inherit_context"_s, "context_aware_warnings"_s };
    JSValue zero = jsNumber(0);
    set("flags"_s, structOf(realm->typeSysFlags(), flagFields, 18, { zero, zero, zero, zero, jsNumber(1), zero, zero, zero, zero, zero, zero, zero, zero, jsBoolean(false), jsNumber(1), zero, jsBoolean(false),
        jsNumber(realm->maximumDigitsOfIntAsString), jsNumber(1), zero, zero }));
    static constexpr ASCIILiteral floatFields[] = { "max"_s, "max_exp"_s, "max_10_exp"_s, "min"_s, "min_exp"_s, "min_10_exp"_s, "dig"_s, "mant_dig"_s, "epsilon"_s, "radix"_s, "rounds"_s };
    using Limits = std::numeric_limits<double>;
    set("float_info"_s, structOf(realm->typeSysFloatInfo(), floatFields, 11, { floatFromDouble(Limits::max()), jsNumber(Limits::max_exponent), jsNumber(Limits::max_exponent10), floatFromDouble(Limits::min()),
        jsNumber(Limits::min_exponent), jsNumber(Limits::min_exponent10), jsNumber(Limits::digits10), jsNumber(Limits::digits), floatFromDouble(Limits::epsilon()), jsNumber(Limits::radix), jsNumber(1) }));
    // As CPython has an int, which is what int.__sizeof__() goes by. Here it is a number or a BigInt.
    static constexpr ASCIILiteral intFields[] = { "bits_per_digit"_s, "sizeof_digit"_s, "default_max_str_digits"_s, "str_digits_check_threshold"_s };
    set("int_info"_s, structOf(realm->typeSysIntInfo(), intFields, 4, { jsNumber(30), jsNumber(4), jsNumber(4300), jsNumber(640) }));
    // The hash of a number is CPython's. That of a str is the one that the engine keeps with the string.
    static constexpr ASCIILiteral hashFields[] = { "width"_s, "modulus"_s, "inf"_s, "nan"_s, "imag"_s, "algorithm"_s, "hash_bits"_s, "seed_bits"_s, "cutoff"_s };
    set("hash_info"_s, structOf(realm->typeSysHashInfo(), hashFields, 9, { jsNumber(64), intFromInt64(globalObject, (int64_t { 1 } << 61) - 1), jsNumber(314159), zero, jsNumber(1000003), text("wtf"_s), jsNumber(24), zero, zero }));
    static constexpr ASCIILiteral threadFields[] = { "name"_s, "lock"_s, "version"_s };
    set("thread_info"_s, structOf(realm->typeSysThreadInfo(), threadFields, 3, { text("pthread"_s), text("mutex+cond"_s), jsUndefined() }));
    static constexpr ASCIILiteral asyncGeneratorHookFields[] = { "firstiter"_s, "finalizer"_s };
    makeStructSequenceType(globalObject, realm->typeSysAsyncGeneratorHooks(), asyncGeneratorHookFields, 2);
    static constexpr ASCIILiteral unraisableFields[] = { "exc_type"_s, "exc_value"_s, "exc_traceback"_s, "err_msg"_s, "object"_s };
    makeStructSequenceType(globalObject, realm->typeUnraisableHookArgs(), unraisableFields, 5);

    // Somewhere to say what goes wrong until there is `io`, which is what the standard streams are made of: _PySys_SetPreliminaryStderr()
    JSObject* preliminary = createPreliminaryStream(globalObject, 2);
    set("stderr"_s, preliminary);
    set("__stderr__"_s, preliminary);

    // ---- Functions
    addHook("displayhook"_s, "__displayhook__"_s, sysDisplayHook);
    addHook("excepthook"_s, "__excepthook__"_s, sysExceptHook);
    addHook("unraisablehook"_s, "__unraisablehook__"_s, sysUnraisableHook);
    addFunction(globalObject, module, "breakpointhook"_s, sysBreakpointHook, 0, { }, PyNativeFunction::Arguments::AreNotChecked);
    set("__breakpointhook__"_s, module->getDirect(vm, Identifier::fromString(vm, "breakpointhook"_s)));
    addFunction(globalObject, realm->builtinsModule(), "breakpoint"_s, builtinBreakpoint, 0, { }, PyNativeFunction::Arguments::AreNotChecked);
    add("addaudithook"_s, sysAddAuditHook);
    addFunction(globalObject, module, "audit"_s, sysAudit, 0, { }, PyNativeFunction::Arguments::AreNotChecked);
    addFrameFunctions(globalObject, module);
    add("exit"_s, sysExit);
    add("exception"_s, sysException);
    add("exc_info"_s, sysExcInfo);
    add("intern"_s, sysIntern);
    add("_is_interned"_s, sysIsInterned);
    add("_is_immortal"_s, sysIsImmortal);
    addFunction(globalObject, module, "getsizeof"_s, sysGetSizeOf, 0, "(object, default=None)"_s);
    add("getrefcount"_s, sysGetRefCount);
    add("getallocatedblocks"_s, sysGetAllocatedBlocks);
    add("getunicodeinternedsize"_s, returnZero);
    add("getrecursionlimit"_s, sysGetRecursionLimit);
    add("setrecursionlimit"_s, sysSetRecursionLimit);
    add("get_int_max_str_digits"_s, sysGetIntMaxStrDigits);
    add("set_int_max_str_digits"_s, sysSetIntMaxStrDigits);
    add("getswitchinterval"_s, sysGetSwitchInterval);
    add("setswitchinterval"_s, sysSetSwitchInterval);
    add("get_coroutine_origin_tracking_depth"_s, sysGetCoroutineOriginTrackingDepth);
    add("set_coroutine_origin_tracking_depth"_s, sysSetCoroutineOriginTrackingDepth);
    add("get_asyncgen_hooks"_s, sysGetAsyncGeneratorHooks);
    addFunction(globalObject, module, "set_asyncgen_hooks"_s, sysSetAsyncGeneratorHooks, 0, "(firstiter=None, finalizer=None)"_s, PyNativeFunction::Arguments::AreNotChecked);
    add("getdefaultencoding"_s, sysReturnText, pack(0u));
    add("getfilesystemencoding"_s, sysReturnText, pack(0u));
    add("getfilesystemencodeerrors"_s, sysReturnText, pack(1u));
    add("call_tracing"_s, sysCallTracing);
    add("_current_frames"_s, sysCurrentFrames);
    add("_current_exceptions"_s, sysCurrentExceptions);
    add("is_finalizing"_s, returnFalse);
    add("activate_stack_trampoline"_s, sysActivateStackTrampoline);
    add("deactivate_stack_trampoline"_s, returnNone);
    add("is_stack_trampoline_active"_s, returnFalse);
    add("is_remote_debug_enabled"_s, returnFalse);
    add("_is_gil_enabled"_s, returnTrue);
    add("_get_cpu_count_config"_s, sysGetCpuCountConfig);
    add("_clear_type_cache"_s, sysClearTypeCache);
    add("_clear_internal_caches"_s, returnNone);
    add("_debugmallocstats"_s, returnNone);

    // The compiler that CPython means by this is not one that there is here.
    JSObject* jit = newBuiltinModule(globalObject, "sys._jit"_s);
    for (ASCIILiteral name : { "is_available"_s, "is_enabled"_s, "is_active"_s })
        addFunction(globalObject, jit, name, returnFalse);
    set("_jit"_s, jit);
    addMonitoring(globalObject, module);
    return module;
}

} } // namespace JSC::Python
