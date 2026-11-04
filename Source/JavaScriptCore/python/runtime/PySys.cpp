/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyCodeCache.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyGenerator.h"
#include "PyIO.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyNamespace.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStdlibModuleNames.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyVirtualFiles.h"
#include "TopExceptionScope.h"
#include <unistd.h>
#include <wtf/FileSystem.h>

namespace JSC {
namespace Python {

// The module sys. (Python/sysmodule.c in CPython is the reference.)

static constexpr int versionMajor = 3;
static constexpr int versionMinor = 15;
static constexpr int versionMicro = 2;
static constexpr int versionHex = (versionMajor << 24) | (versionMinor << 16) | (versionMicro << 8) | 0xf0;

// ---- The standard streams ----

static void addStandardStreams(JSGlobalObject* globalObject, PyModule* module)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    auto add = [&](int fd, ASCIILiteral attribute, ASCIILiteral original, ASCIILiteral name, ASCIILiteral errors) {
        JSValue stream = makeStandardStream(globalObject, fd, fd != STDIN_FILENO, name, errors);
        if (failed(scope, stream)) {
            // A directory, say.
            scope.clearException();
            stream = jsUndefined();
        }
        module->add(globalObject, attribute, stream);
        module->add(globalObject, original, stream);
    };
    // As in the UTF-8 mode of CPython, which is the default.
    add(STDIN_FILENO, "stdin"_s, "__stdin__"_s, "<stdin>"_s, "surrogateescape"_s);
    add(STDOUT_FILENO, "stdout"_s, "__stdout__"_s, "<stdout>"_s, "surrogateescape"_s);
    add(STDERR_FILENO, "stderr"_s, "__stderr__"_s, "<stderr>"_s, "backslashreplace"_s);
}

// ---- Functions ----

static JSValue sysExit(JSGlobalObject* globalObject, JSValue status)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!status || isNone(status))
        return raiseWithArgs(globalObject, scope, rt.SystemExit(), { });
    // A tuple is the arguments of the exception, as for any `raise SystemExit, value`.
    if (auto* tuple = PyTuple::tryFrom(status)) {
        PyException* exception = newException(globalObject, rt.SystemExit(), tuple->span());
        RETURN_IF_EXCEPTION(scope, { });
        return raiseException(globalObject, scope, exception);
    }
    return raiseWithArgs(globalObject, scope, rt.SystemExit(), { status });
}

static JSValue sysException(JSGlobalObject* globalObject)
{
    JSValue exception = runtime(globalObject).handledException();
    return exception ? exception : jsUndefined();
}

static JSValue sysExcInfo(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    JSValue exception = rt.handledException();
    if (!exception)
        return PyTuple::create(globalObject, { jsUndefined(), jsUndefined(), jsUndefined() });
    JSValue traceback = PyException::from(exception)->traceback();
    return PyTuple::create(globalObject, { rt.typeOf(exception), exception, traceback ? traceback : jsUndefined() });
}

static JSValue sysExceptHook(JSGlobalObject* globalObject, JSValue, JSValue value, JSValue traceback)
{
    VM& vm = getVM(globalObject);
    if (auto* exception = PyException::tryFrom(value); exception && !exception->traceback() && PyTraceback::is(traceback))
        exception->setTraceback(vm, traceback);
    printException(globalObject, value);
    return jsUndefined();
}

static JSValue sysDisplayHook(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(value))
        return jsUndefined();
    JSString* underscore = internString(vm, "_"_s);
    rt.builtins()->set(vm, underscore, jsUndefined());
    JSValue output = rt.sysDict()->get(rt.names.stdout_());
    if (!output || isNone(output))
        return raiseRuntimeError(globalObject, scope, "lost sys.stdout"_s);
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    writeToFile(globalObject, output, text);
    if (scope.exception()) [[unlikely]] {
        // What the encoding of the stream has no way to write is shown escaped.
        if (!clearPendingExceptionIfMatches(globalObject, rt.UnicodeEncodeError()))
            return { };
        JSValue encoding = getAttr(globalObject, output, internString(vm, "encoding"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isStr(encoding))
            return raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
        JSValue encoded = strEncode(globalObject, text, strValue(encoding), makeStr(vm, "backslashreplace"_s));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue buffer = getAttrIfPresent(globalObject, output, internString(vm, "buffer"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (buffer)
            callMethod(globalObject, buffer, rt.names.write(), encoded);
        else {
            JSValue escaped = callMethod(globalObject, encoded, internString(vm, "decode"_s), encoding, makeStr(vm, "strict"_s));
            RETURN_IF_EXCEPTION(scope, { });
            callMethod(globalObject, output, rt.names.write(), escaped);
        }
        RETURN_IF_EXCEPTION(scope, { });
    }
    writeToFile(globalObject, output, jsSingleCharacterString(vm, '\n'));
    RETURN_IF_EXCEPTION(scope, { });
    rt.builtins()->set(vm, underscore, value);
    return jsUndefined();
}

static JSValue sysUnraisableHook(JSGlobalObject* globalObject, JSValue unraisable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue argumentsType = runtime(globalObject).sysDict()->get(internString(vm, "__unraisable_hook_args_type__"_s));
    if (!argumentsType || JSValue(typeOf(globalObject, unraisable)) != argumentsType)
        return raiseTypeError(globalObject, scope, "sys.unraisablehook argument type must be UnraisableHookArgs"_s);
    auto attribute = [&](ASCIILiteral name) { return getAttr(globalObject, unraisable, internString(vm, name)); };
    JSValue message = attribute("err_msg"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue object = attribute("object"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue exception = attribute("exc_value"_s);
    RETURN_IF_EXCEPTION(scope, { });
    StrBuilder text;
    if (!isNone(message)) {
        JSString* string = str(globalObject, message);
        RETURN_IF_EXCEPTION(scope, { });
        text.append(string->value(globalObject).data);
    } else if (!isNone(object))
        text.append("Exception ignored in"_s);
    if (!isNone(object))
        text.append(": "_s, reprForMessage(globalObject, object), '\n');
    else if (!isNone(message))
        text.append(":\n"_s);
    JSValue file = runtime(globalObject).sysDict()->get(runtime(globalObject).names.stderr_());
    if (!file || isNone(file))
        return jsUndefined();
    writeToFile(globalObject, file, makeStr(globalObject, text.toString()));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(exception))
        printException(globalObject, exception, file, true);
    return jsUndefined();
}

static JSValue sysBreakpointHook(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const char* variable = configuration().useEnvironment ? getenv("PYTHONBREAKPOINT") : nullptr;
    String path = variable && *variable ? String::fromUTF8(variable) : String("pdb.set_trace"_s);
    if (path == "0"_s)
        return jsUndefined();

    // "module.callable", or the name of a built-in function.
    size_t dot = path.reverseFind('.');
    JSValue hook;
    if (dot) {
        JSValue module = importModule(globalObject, dot == notFound ? rt.names.builtins() : makeStr(vm, path.left(dot)));
        if (scope.exception() && !clearPendingExceptionIfMatches(globalObject, rt.ImportError()))
            return { };
        if (module) {
            hook = getAttr(globalObject, module, makeStr(vm, dot == notFound ? path : path.substring(dot + 1)));
            if (scope.exception() && !clearPendingExceptionIfMatches(globalObject, rt.AttributeError()))
                return { };
        }
    }
    if (!hook) {
        warn(globalObject, rt.RuntimeWarning(), makeString("Ignoring unimportable $PYTHONBREAKPOINT: \""_s, path, '"'), 0);
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    RELEASE_AND_RETURN(scope, call(globalObject, hook, args));
}

static JSValue sysGetFrame(JSGlobalObject* globalObject, JSValue depthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int depth = depthValue ? toCInt(globalObject, depthValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    PyFrame* frame = currentFrame(globalObject, depth > 0 ? depth : 0);
    if (!frame)
        return raiseValueError(globalObject, scope, "call stack is not deep enough"_s);
    if (failed(scope, audit(globalObject, "sys._getframe"_s, frame)))
        return { };
    return frame;
}

static JSValue sysGetFrameModuleName(JSGlobalObject* globalObject, JSValue depthValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int depth = depthValue ? toCInt(globalObject, depthValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, audit(globalObject, "sys._getframemodulename"_s, jsNumber(depth))))
        return { };
    PyFrame* frame = currentFrame(globalObject, depth > 0 ? depth : 0);
    if (!frame)
        return jsUndefined();
    JSValue module = frame->function()->codeOwner()->module();
    return module ? module : jsUndefined();
}

static JSValue sysIntern(JSGlobalObject* globalObject, JSValue string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!string.isString()) {
        if (isStr(string))
            return raiseTypeError(globalObject, scope, "can't intern "_s, typeName(globalObject, string));
        return raiseTypeError(globalObject, scope, "intern() argument must be str, not "_s, typeNameOfArgument(globalObject, string));
    }
    // The argument stays the object it is: if an equal str has been interned before, that one is the result.
    String text = asString(string)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    AtomString atom(text);
    if (atom.impl() == text.impl())
        return string;
    return jsString(vm, atom.string());
}

static JSValue sysIsInterned(JSGlobalObject* globalObject, JSValue string)
{
    if (!isStr(string)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "_is_interned() argument must be str, not "_s, typeNameOfArgument(globalObject, string));
    }
    const StringImpl* impl = string.isString() ? asString(string)->tryGetValueImpl() : nullptr;
    return jsBoolean(impl && impl->isAtom());
}

static JSValue sysGetRecursionLimit(JSGlobalObject* globalObject) { return jsNumber(runtime(globalObject).recursionLimit); }

static JSValue sysSetRecursionLimit(JSGlobalObject* globalObject, JSValue limitValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int limit = toCInt(globalObject, limitValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (limit < 1)
        return raiseValueError(globalObject, scope, "recursion limit must be greater or equal than 1"_s);
    Runtime& rt = runtime(globalObject);
    if (rt.frameDepth >= limit)
        return raiseRecursionError(globalObject, scope, "cannot set the recursion limit to "_s, limit, " at the recursion depth "_s, rt.frameDepth, ": the limit is too low"_s);
    rt.recursionLimit = limit;
    return jsUndefined();
}

static JSValue sysGetIntMaxStrDigits(JSGlobalObject* globalObject) { return jsNumber(runtime(globalObject).maxStrDigits); }

static JSValue sysSetIntMaxStrDigits(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int digits = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (digits && digits < 640)
        return raiseValueError(globalObject, scope, "maxdigits must be >= 640 or 0 for unlimited"_s);
    runtime(globalObject).maxStrDigits = digits;
    return jsUndefined();
}

// There are no reference counts. Programs that ask mostly want to know that the number is small.
static JSValue sysGetRefCount(JSGlobalObject*, JSValue) { return jsNumber(2); }

static JSValue sysGetSizeOf(JSGlobalObject* globalObject, JSValue object, JSValue defaultValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = rt.typeOf(object)->lookup(Special::SizeOf);
    if (!method) {
        if (defaultValue)
            return defaultValue;
        return raiseTypeError(globalObject, scope, "Type "_s, typeName(globalObject, object), " doesn't define __sizeof__"_s);
    }
    JSValue size = callSpecial(globalObject, method, object);
    EXCEPTION_ASSERT(!!scope.exception() == !size);
    if (size && !isInt(size)) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        size = JSValue();
    }
    if (!size) {
        if (defaultValue && clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return defaultValue;
        return { };
    }
    size = intValue(size);
    if (!tryInt64(size))
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    if (intSign(size) < 0)
        return raiseValueError(globalObject, scope, "__sizeof__() should return >= 0"_s);
    return size;
}

// ---- Audit hooks ----

static bool callAuditHooks(JSGlobalObject* globalObject, JSValue event, std::span<const JSValue> arguments)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* hooks = asList(rt.slot(Runtime::Slot::AuditHooks));
    PyTuple* tuple = PyTuple::create(globalObject, arguments);
    // A hook may add another, which then hears of this event too.
    for (unsigned i = 0; i < listLength(hooks); ++i) {
        JSValue hook = listGet(hooks, i);
        // What a hook does is not traced, unless it says it may be.
        JSValue canTrace = getAttrIfPresent(globalObject, hook, internString(vm, "__cantrace__"_s));
        RETURN_IF_EXCEPTION(scope, false);
        bool isTraced = canTrace && isTrue(globalObject, canTrace);
        RETURN_IF_EXCEPTION(scope, false);
        rt.tracingDepth += !isTraced;
        call(globalObject, hook, event, tuple);
        rt.tracingDepth -= !isTraced;
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

bool auditWithHooks(JSGlobalObject* globalObject, ASCIILiteral event, std::span<const JSValue> arguments)
{
    Vector<JSValue, 8> values;
    for (JSValue argument : arguments)
        values.append(argument ? argument : jsUndefined());
    return callAuditHooks(globalObject, makeStr(getVM(globalObject), event), values.span());
}

static JSValue sysAudit(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "sys.audit() takes no keyword arguments"_s);
    if (!args.size())
        return raiseTypeError(globalObject, scope, "audit expected at least 1 argument, got 0"_s);
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "audit() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]));
    if (isAudited(globalObject)) {
        callAuditHooks(globalObject, args[0], args.positional().subspan(1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue sysAddAuditHook(JSGlobalObject* globalObject, JSValue hook)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    if (failed(scope, audit(globalObject, "sys.addaudithook"_s))) {
        // The hooks there are may refuse another, and then nothing is said about it.
        if (clearPendingExceptionIfMatches(globalObject, rt.Exception()))
            return jsUndefined();
        return { };
    }
    if (!isAudited(globalObject))
        rt.setSlot(Runtime::Slot::AuditHooks, newList(globalObject));
    listAppend(globalObject, asList(rt.slot(Runtime::Slot::AuditHooks)), hook);
    return jsUndefined();
}

static JSValue sysGetLazyImports(JSGlobalObject* globalObject)
{
    JSValue mode = runtime(globalObject).slot(Runtime::Slot::LazyImportsMode);
    return mode ? mode : JSValue(makeStr(getVM(globalObject), "normal"_s));
}

static JSValue sysSetLazyImports(JSGlobalObject* globalObject, JSValue mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(mode))
        return raiseTypeError(globalObject, scope, "mode must be a string: 'normal' or 'all'"_s);
    String text = strValue(mode)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text != "normal"_s && text != "all"_s)
        return raiseValueError(globalObject, scope, "mode must be 'normal' or 'all'"_s);
    runtime(globalObject).setSlot(Runtime::Slot::LazyImportsMode, makeStr(vm, text));
    return jsUndefined();
}

static JSValue sysGetLazyImportsFilter(JSGlobalObject* globalObject)
{
    JSValue filter = runtime(globalObject).slot(Runtime::Slot::LazyImportsFilter);
    return filter ? filter : jsUndefined();
}

static JSValue sysSetLazyImportsFilter(JSGlobalObject* globalObject, JSValue filter)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isNone(filter) && !isCallable(globalObject, filter))
        return raiseValueError(globalObject, scope, "filter provided but is not callable"_s);
    runtime(globalObject).setSlot(Runtime::Slot::LazyImportsFilter, filter);
    return jsUndefined();
}

static JSValue sysGetDlopenFlags(JSGlobalObject* globalObject)
{
    JSValue flags = runtime(globalObject).slot(Runtime::Slot::DlopenFlags);
    return flags ? flags : jsNumber(2); // RTLD_NOW
}

static JSValue sysSetDlopenFlags(JSGlobalObject* globalObject, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int value = toCInt(globalObject, flags);
    RETURN_IF_EXCEPTION(scope, { });
    runtime(globalObject).setSlot(Runtime::Slot::DlopenFlags, jsNumber(value));
    return jsUndefined();
}

static JSValue sysGetDefaultEncoding(JSGlobalObject* globalObject) { return makeStr(getVM(globalObject), "utf-8"_s); }
static JSValue sysGetFilesystemEncodeErrors(JSGlobalObject* globalObject) { return makeStr(getVM(globalObject), "surrogateescape"_s); }
static JSValue sysReturnNone(JSGlobalObject*) { return jsUndefined(); }
static JSValue sysReturnFalse(JSGlobalObject*) { return jsBoolean(false); }
static JSValue sysReturnZero(JSGlobalObject*) { return jsNumber(0); }

static JSValue sysGetCoroutineOriginTrackingDepth(JSGlobalObject* globalObject) { return jsNumber(runtime(globalObject).coroutineOriginTrackingDepth); }

static JSValue sysSetCoroutineOriginTrackingDepth(JSGlobalObject* globalObject, JSValue depthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int depth = toCInt(globalObject, depthValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (depth < 0)
        return raiseValueError(globalObject, scope, "depth must be >= 0"_s);
    runtime(globalObject).coroutineOriginTrackingDepth = depth;
    return jsUndefined();
}

static JSValue sysClearTypeCache(JSGlobalObject* globalObject)
{
    if (!warn(globalObject, runtime(globalObject).DeprecationWarning(), "sys._clear_type_cache() is deprecated and scheduled for removal in a future version. Use sys._clear_internal_caches() instead."_s))
        return { };
    return jsUndefined();
}

// -X cpu_count, which nothing sets: os.cpu_count() is to ask the system.
static JSValue sysGetCpuCountConfig(JSGlobalObject*) { return jsNumber(configuration().cpuCount); }
static JSValue sysGetSwitchInterval(JSGlobalObject* globalObject) { return makeFloat(globalObject, static_cast<double>(runtime(globalObject).threads().switchInterval()) * 1e-9); }

static JSValue sysSetSwitchInterval(JSGlobalObject* globalObject, JSValue interval)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double seconds = toDouble(globalObject, interval);
    RETURN_IF_EXCEPTION(scope, { });
    if (!(seconds > 0))
        return raiseValueError(globalObject, scope, "switch interval must be strictly positive"_s);
    // It is kept in microseconds, in 32 bits.
    double microseconds = seconds * 1e6;
    if (microseconds >= 4294967295.0)
        return raiseOverflowError(globalObject, scope, "switch interval is too large"_s);
    runtime(globalObject).threads().setSwitchInterval(static_cast<int64_t>(microseconds) * 1000);
    return jsUndefined();
}

static JSValue sysIsFinalizing(JSGlobalObject* globalObject) { return jsBoolean(runtime(globalObject).threads().isFinalized()); }

// { ident: the frame the thread is in }
static JSValue sysCurrentFrames(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, audit(globalObject, "sys._current_frames"_s)))
        return { };
    PyDict* frames = PyDict::create(globalObject);
    if (PyFrame* frame = currentFrame(globalObject))
        frames->set(globalObject, makeInt(globalObject, currentThreadIdent()), frame);
    rt.threads().forEachThread([&](ThreadState& thread) {
        if (!thread.parked)
            return;
        if (PyFrame* frame = topFrameOf(globalObject, *thread.parked))
            frames->set(globalObject, makeInt(globalObject, thread.ident), frame);
    });
    UNUSED_PARAM(vm);
    return frames;
}

// { ident: the exception the thread is handling, or None }
static JSValue sysCurrentExceptions(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    if (failed(scope, audit(globalObject, "sys._current_exceptions"_s)))
        return { };
    PyDict* exceptions = PyDict::create(globalObject);
    JSValue mine = rt.handledException();
    exceptions->set(globalObject, makeInt(globalObject, currentThreadIdent()), mine ? mine : jsUndefined());
    rt.threads().forEachThread([&](ThreadState& thread) {
        if (!thread.saved)
            return;
        for (auto& entry : thread.saved->entries) {
            if (entry.runtime != &rt)
                continue;
            JSValue exception = entry.state.handledException;
            for (PyGenerator* generator = entry.state.runningGenerator; generator; generator = generator->resumedBy()) {
                if (JSValue value = generator->handledException()) {
                    exception = value;
                    break;
                }
            }
            exceptions->set(globalObject, makeInt(globalObject, thread.ident), exception ? exception : jsUndefined());
        }
    });
    return exceptions;
}
static JSValue sysIsGILEnabled(JSGlobalObject*) { return jsBoolean(true); }
static JSValue sysCallTracing(JSGlobalObject* globalObject, JSValue function, JSValue args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* tuple = PyTuple::tryFrom(args);
    if (!tuple)
        return raiseTypeError(globalObject, scope, "call_tracing() argument 2 must be tuple, not "_s, typeName(globalObject, args));
    // It is for a debugger to call, from its trace function, what is to be traced in its turn.
    Runtime& rt = runtime(globalObject);
    SetForScope tracingDepth(rt.tracingDepth, 0);
    rt.monitoring().toolIsInterrupted();
    RELEASE_AND_RETURN(scope, call(globalObject, function, CallArgs(tuple->span())));
}

static constexpr ASCIILiteral asyncGenHooksFields[] = { "firstiter"_s, "finalizer"_s };
static constexpr StructSeqDef asyncGenHooksDef { "asyncgen_hooks"_s, asyncGenHooksFields, 2, true, true };

static JSValue sysGetAsyncGenHooks(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSString* key = internString(vm, "__asyncgen_hooks_type__"_s);
    JSValue type = rt.sysDict()->get(key);
    if (!type) {
        type = makeStructSeqType(globalObject, asyncGenHooksDef);
        rt.sysDict()->set(vm, key, type);
    }
    JSValue firstIter = rt.asyncGenFirstIter();
    JSValue finalizer = rt.asyncGenFinalizer();
    return makeStructSeq(globalObject, PyType::from(type), { firstIter ? firstIter : jsUndefined(), finalizer ? finalizer : jsUndefined() });
}

static JSValue sysSetAsyncGenHooks(JSGlobalObject* globalObject, JSValue firstIter, JSValue finalizer)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (finalizer && !isNone(finalizer) && !isCallable(globalObject, finalizer))
        return raiseTypeError(globalObject, scope, "callable finalizer expected, got "_s, typeName(globalObject, finalizer));
    if (firstIter && !isNone(firstIter) && !isCallable(globalObject, firstIter))
        return raiseTypeError(globalObject, scope, "callable firstiter expected, got "_s, typeName(globalObject, firstIter));
    if (finalizer) {
        if (failed(scope, audit(globalObject, "sys.set_asyncgen_hook_finalizer"_s)))
            return { };
        rt.setAsyncGenFinalizer(isNone(finalizer) ? JSValue() : finalizer);
    }
    if (firstIter) {
        if (failed(scope, audit(globalObject, "sys.set_asyncgen_hook_firstiter"_s)))
            return { };
        rt.setAsyncGenFirstIter(isNone(firstIter) ? JSValue() : firstIter);
    }
    return jsUndefined();
}

// The interactive interpreter without line editing, which _pyrepl falls back on.
static JSValue sysBaseRepl(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    auto status = runBasicInteractiveLoop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (status)
        return raiseWithArgs(globalObject, scope, rt.SystemExit(), { jsNumber(*status) });
    return jsUndefined();
}

static constexpr NativeDef sysFunctions[] = {
    PY_FUNCTION("_clear_internal_caches", sysReturnNone, "()"),
    PY_FUNCTION("_clear_type_cache", sysClearTypeCache, "()"),
    PY_FUNCTION("_get_cpu_count_config", sysGetCpuCountConfig, "()"),
    PY_FUNCTION("_current_exceptions", sysCurrentExceptions, "()"),
    PY_FUNCTION("_current_frames", sysCurrentFrames, "()"),
    PY_FUNCTION("_getframe", sysGetFrame, "(depth=0, /)"),
    PY_FUNCTION("_getframemodulename", sysGetFrameModuleName, "(depth=0)"),
    PY_FUNCTION("_is_gil_enabled", sysIsGILEnabled, "()"),
    PY_FUNCTION("_is_interned", sysIsInterned, "(string, /)"),
    PY_FUNCTION("addaudithook", sysAddAuditHook, "(hook)"),
    PY_FUNCTION("audit", sysAudit, "(event, /, *args)"),
    PY_FUNCTION("breakpointhook", sysBreakpointHook, "(*args, **kws)"),
    PY_FUNCTION("call_tracing", sysCallTracing, "(func, args, /)"),
    PY_FUNCTION("displayhook", sysDisplayHook, "(object, /)"),
    PY_FUNCTION("exc_info", sysExcInfo, "()"),
    PY_FUNCTION("excepthook", sysExceptHook, "(exctype, value, traceback, /)"),
    PY_FUNCTION("exception", sysException, "()"),
    PY_FUNCTION("exit", sysExit, "(status=None, /)"),
    PY_FUNCTION("get_asyncgen_hooks", sysGetAsyncGenHooks, "()"),
    PY_FUNCTION("get_coroutine_origin_tracking_depth", sysGetCoroutineOriginTrackingDepth, "()"),
    PY_FUNCTION("get_int_max_str_digits", sysGetIntMaxStrDigits, "()"),
    PY_FUNCTION("get_lazy_imports", sysGetLazyImports, "()"),
    PY_FUNCTION("get_lazy_imports_filter", sysGetLazyImportsFilter, "()"),
    PY_FUNCTION("getallocatedblocks", sysReturnZero, "()"),
    PY_FUNCTION("getdefaultencoding", sysGetDefaultEncoding, "()"),
    PY_FUNCTION("getdlopenflags", sysGetDlopenFlags, "()"),
    PY_FUNCTION("getfilesystemencodeerrors", sysGetFilesystemEncodeErrors, "()"),
    PY_FUNCTION("getfilesystemencoding", sysGetDefaultEncoding, "()"),
    PY_FUNCTION("getprofile", sysGetProfile, "()"),
    PY_FUNCTION("getrecursionlimit", sysGetRecursionLimit, "()"),
    PY_FUNCTION("getrefcount", sysGetRefCount, "(object, /)"),
    PY_FUNCTION("getsizeof", sysGetSizeOf, "(object, default=<unrepresentable>)"),
    PY_FUNCTION("getswitchinterval", sysGetSwitchInterval, "()"),
    PY_FUNCTION("gettrace", sysGetTrace, "()"),
    PY_FUNCTION("getunicodeinternedsize", sysReturnZero, "()"),
    PY_FUNCTION("intern", sysIntern, "(string, /)"),
    PY_FUNCTION("is_finalizing", sysIsFinalizing, "()"),
    PY_FUNCTION("is_remote_debug_enabled", sysReturnFalse, "()"),
    PY_FUNCTION("is_stack_trampoline_active", sysReturnFalse, "()"),
    PY_FUNCTION("set_asyncgen_hooks", sysSetAsyncGenHooks, "(firstiter=<unrepresentable>, finalizer=<unrepresentable>)"),
    PY_FUNCTION("set_coroutine_origin_tracking_depth", sysSetCoroutineOriginTrackingDepth, "(depth)"),
    PY_FUNCTION("set_int_max_str_digits", sysSetIntMaxStrDigits, "(maxdigits)"),
    PY_FUNCTION("set_lazy_imports", sysSetLazyImports, "(mode)"),
    PY_FUNCTION("set_lazy_imports_filter", sysSetLazyImportsFilter, "(filter)"),
    PY_FUNCTION("setdlopenflags", sysSetDlopenFlags, "(flags, /)"),
    PY_FUNCTION("setprofile", sysSetProfile, "(function, /)"),
    PY_FUNCTION("setrecursionlimit", sysSetRecursionLimit, "(limit, /)"),
    PY_FUNCTION("setswitchinterval", sysSetSwitchInterval, "(interval, /)"),
    PY_FUNCTION("settrace", sysSetTrace, "(function, /)"),
    PY_FUNCTION("_setprofileallthreads", sysSetProfileAllThreads, "(function, /)"),
    PY_FUNCTION("_settraceallthreads", sysSetTraceAllThreads, "(function, /)"),
    PY_FUNCTION("unraisablehook", sysUnraisableHook, "(unraisable, /)"),
    PY_FUNCTION("_baserepl", sysBaseRepl, "()"),
};

// ---- Data ----

static constexpr ASCIILiteral versionInfoFields[] = { "major"_s, "minor"_s, "micro"_s, "releaselevel"_s, "serial"_s };
static constexpr StructSeqDef versionInfoDef { "sys.version_info"_s, versionInfoFields, 5, false, true };

static constexpr ASCIILiteral flagsFields[] = {
    "debug"_s, "inspect"_s, "interactive"_s, "optimize"_s, "dont_write_bytecode"_s, "no_user_site"_s, "no_site"_s, "ignore_environment"_s, "verbose"_s,
    "bytes_warning"_s, "quiet"_s, "hash_randomization"_s, "isolated"_s, "dev_mode"_s, "utf8_mode"_s, "warn_default_encoding"_s, "safe_path"_s, "int_max_str_digits"_s,
    "gil"_s, "thread_inherit_context"_s, "context_aware_warnings"_s, "lazy_imports"_s,
};
static constexpr StructSeqDef flagsDef { "sys.flags"_s, flagsFields, 18, false, true };

static constexpr ASCIILiteral floatInfoFields[] = { "max"_s, "max_exp"_s, "max_10_exp"_s, "min"_s, "min_exp"_s, "min_10_exp"_s, "dig"_s, "mant_dig"_s, "epsilon"_s, "radix"_s, "rounds"_s };
static constexpr StructSeqDef floatInfoDef { "sys.float_info"_s, floatInfoFields, 11, true, true };

static constexpr ASCIILiteral intInfoFields[] = { "bits_per_digit"_s, "sizeof_digit"_s, "default_max_str_digits"_s, "str_digits_check_threshold"_s };
static constexpr StructSeqDef intInfoDef { "sys.int_info"_s, intInfoFields, 4, true, true };

static constexpr ASCIILiteral hashInfoFields[] = { "width"_s, "modulus"_s, "inf"_s, "nan"_s, "imag"_s, "algorithm"_s, "hash_bits"_s, "seed_bits"_s, "cutoff"_s };
static constexpr StructSeqDef hashInfoDef { "sys.hash_info"_s, hashInfoFields, 9, true, true };

static constexpr ASCIILiteral threadInfoFields[] = { "name"_s, "lock"_s, "version"_s };
static constexpr StructSeqDef threadInfoDef { "sys.thread_info"_s, threadInfoFields, 3, true, true };

// Where the standard library is: <prefix>/lib/python3.15, where the prefix is $PYTHONHOME, or else what the host has it under.
String standardLibraryDirectory()
{
    String prefix;
    if (!configuration().home.isNull())
        prefix = configuration().home;
    else if (const char* directory = getenv("BUN_PYTHON_STDLIB"))
        return String::fromUTF8(directory);
    else if (auto* fileSystem = virtualFileSystem())
        prefix = fileSystem->prefix;
    else
        return { };
    return makeString(prefix, "/lib/python"_s, versionMajor, '.', versionMinor);
}

// python3._pth beside the program, as it was started or where it really is: its lines are all there is to sys.path, and neither
// the environment nor, unless a line says `import site`, the site module has any say. (Modules/getpath.py in CPython.)
// The directory the file is in, which is the prefix then. Null: there is none.
static String readPathFile(Configuration& configuration, Vector<String>& searchPath)
{
    if (configuration.executable.isEmpty())
        return { };
    for (const String& program : { configuration.executable, realPath(configuration.executable) }) {
        if (program.isEmpty())
            continue;
        auto contents = readFile(makeString(program, "._pth"_s));
        if (!contents)
            continue;
        String directory = FileSystem::parentPath(program);
        configuration.isolated = true;
        configuration.useEnvironment = false;
        configuration.siteImport = false;
        configuration.userSiteDirectory = false;
        configuration.safePath = true;
        configuration.home = { };
        configuration.pythonPath = { };
        String text = decodeFilesystem(contents->span());
        for (auto whole : StringView(text).splitAllowingEmptyEntries('\n')) {
            size_t comment = whole.find('#');
            StringView line = (comment == notFound ? whole : whole.left(comment)).trim(isASCIIWhitespace<char16_t>);
            if (line.isEmpty())
                continue;
            if (line == "import site"_s)
                configuration.siteImport = true;
            else if (line.startsWith("import "_s))
                fputs("unsupported 'import' line in ._pth file\n", stderr);
            else
                searchPath.append(absolutePath(line.startsWith('/') ? line.toString() : makeString(directory, '/', line)));
        }
        return directory;
    }
    return { };
}

String shortVersionText()
{
    return makeString(versionMajor, '.', versionMinor, '.', versionMicro);
}

String versionText()
{
    return makeString(shortVersionText(), " (bun, " __DATE__ ", " __TIME__ ") [JavaScriptCore]"_s);
}

ASCIILiteral platformText()
{
#if OS(DARWIN)
    return "darwin"_s;
#elif OS(WINDOWS)
    return "win32"_s;
#else
    return "linux"_s;
#endif
}

void updateSysFlags(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    const Configuration& configuration = Python::configuration();
    JSString* name = internString(vm, "flags"_s);
    JSValue existing = rt.sysDict()->get(name);
    PyType* type = existing ? rt.typeOf(existing) : makeStructSeqType(globalObject, flagsDef);
    rt.sysDict()->set(vm, name, makeStructSeq(globalObject, type, {
        jsNumber(!!configuration.parserDebug), jsNumber(!!configuration.inspect), jsNumber(!!configuration.interactive), jsNumber(configuration.optimizationLevel),
        jsNumber(!configuration.writeBytecode), jsNumber(!configuration.userSiteDirectory), jsNumber(!configuration.siteImport), jsNumber(!configuration.useEnvironment),
        jsNumber(configuration.verbose), jsNumber(configuration.bytesWarning), jsNumber(!!configuration.quiet), jsNumber(configuration.hashRandomization),
        jsNumber(configuration.isolated), jsBoolean(configuration.devMode), jsNumber(configuration.utf8Mode), jsNumber(configuration.warnDefaultEncoding),
        jsBoolean(configuration.safePath), jsNumber(configuration.intMaxStrDigits),
        jsNumber(1), jsNumber(configuration.threadInheritContext), jsNumber(configuration.contextAwareWarnings), jsNumber(configuration.lazyImports) }));
}

void initializeSysModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<String> pathFromFile;
    String directoryOfPathFile = readPathFile(Python::configuration(), pathFromFile);
    const Configuration& configuration = Python::configuration();
    module->addFunctions(globalObject, sysFunctions);
    addSysExtras(globalObject, module);
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "__doc__"_s, docstringFor(globalObject, "module sys"_s));
    module->add(globalObject, "__package__"_s, rt.names.empty());
    PyDict* dict = module->dict();
    auto text = [&](const String& string) -> JSValue { return makeStr(vm, string); };
    auto alias = [&](ASCIILiteral name, ASCIILiteral original) { module->add(globalObject, name, dict->get(internString(vm, original))); };
    alias("__breakpointhook__"_s, "breakpointhook"_s);
    alias("__displayhook__"_s, "displayhook"_s);
    alias("__excepthook__"_s, "excepthook"_s);
    alias("__unraisablehook__"_s, "unraisablehook"_s);

    addStandardStreams(globalObject, module);

    module->add(globalObject, "modules"_s, rt.modules());
    auto listOf = [&](const Vector<String>& strings) {
        JSArray* list = newList(globalObject);
        for (auto& string : strings) {
            listAppend(globalObject, list, text(string));
            scope.assertNoException();
        }
        return list;
    };
    module->add(globalObject, "argv"_s, listOf(configuration.arguments));
    module->add(globalObject, "orig_argv"_s, listOf(configuration.originalArguments));
    module->add(globalObject, "warnoptions"_s, listOf(configuration.warnOptions));
    PyDict* xOptions = PyDict::create(globalObject);
    for (auto& option : configuration.xOptions) {
        size_t separator = option.find('=');
        if (separator == notFound)
            xOptions->set(vm, internString(vm, option), jsBoolean(true));
        else
            xOptions->set(vm, internString(vm, option.left(separator)), text(option.substring(separator + 1)));
    }
    module->add(globalObject, "_xoptions"_s, xOptions);
    module->add(globalObject, "meta_path"_s, newList(globalObject));
    module->add(globalObject, "path_hooks"_s, newList(globalObject));
    module->add(globalObject, "path_importer_cache"_s, PyDict::create(globalObject));
    if (!rt.slot(Runtime::Slot::LazyModules))
        rt.setSlot(Runtime::Slot::LazyModules, PySet::create(globalObject));
    module->add(globalObject, "lazy_modules"_s, rt.slot(Runtime::Slot::LazyModules));

    JSArray* path = newList(globalObject);
    String library = standardLibraryDirectory();
    if (!directoryOfPathFile.isNull()) {
        for (auto& part : pathFromFile)
            listAppend(globalObject, path, text(part));
    } else {
        if (!configuration.pythonPath.isNull()) {
            for (auto& part : configuration.pythonPath.splitAllowingEmptyEntries(':')) {
                listAppend(globalObject, path, text(absolutePath(part)));
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
        if (!library.isNull()) {
            listAppend(globalObject, path, text(library));
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    module->add(globalObject, "path"_s, path);
    module->add(globalObject, "_stdlib_dir"_s, library.isNull() ? jsUndefined() : text(library));

    String prefix = !directoryOfPathFile.isNull() ? directoryOfPathFile : library.isNull() ? emptyString() : FileSystem::parentPath(FileSystem::parentPath(library));
    // A virtual environment is where there is a pyvenv.cfg beside the executable or one directory up. `site` does the rest.
    String environment = prefix;
    String baseExecutable = !configuration.baseExecutable.isNull() ? configuration.baseExecutable : configuration.executable.isNull() ? emptyString() : configuration.executable;
    if (!configuration.executable.isEmpty()) {
        String directory = FileSystem::parentPath(configuration.executable);
        for (const String& candidate : { directory, FileSystem::parentPath(directory) }) {
            if (kindOfFile(makeString(candidate, "/pyvenv.cfg"_s)) == VirtualFileKind::File) {
                environment = candidate;
                break;
            }
        }
    }
    if (environment != prefix) {
        // The interpreter the environment was made with is in the directory that pyvenv.cfg calls `home`, under the name this one
        // was started by, or else under one of the usual names. (Modules/getpath.py in CPython.)
        String home;
        if (auto contents = readFile(makeString(environment, "/pyvenv.cfg"_s))) {
            for (auto line : String::fromUTF8ReplacingInvalidSequences(contents->span()).split('\n')) {
                size_t separator = line.find('=');
                if (separator != notFound && equalLettersIgnoringASCIICase(line.left(separator).trim(isASCIIWhitespace<char16_t>), "home"_s)) {
                    home = line.substring(separator + 1).trim(isASCIIWhitespace<char16_t>);
                    break;
                }
            }
        }
        if (!home.isEmpty()) {
            for (const String& name : { FileSystem::pathFileName(configuration.executable), String("python3"_s), makeString("python"_s, versionMajor, '.', versionMinor) }) {
                String candidate = makeString(home, '/', name);
                if (kindOfFile(candidate) == VirtualFileKind::File) {
                    baseExecutable = candidate;
                    break;
                }
            }
        }
    }
    module->add(globalObject, "prefix"_s, text(environment));
    module->add(globalObject, "exec_prefix"_s, text(environment));
    module->add(globalObject, "base_prefix"_s, text(prefix));
    module->add(globalObject, "base_exec_prefix"_s, text(prefix));
    module->add(globalObject, "platlibdir"_s, text(configuration.platlibdir));
    module->add(globalObject, "executable"_s, text(configuration.executable.isNull() ? emptyString() : configuration.executable));
    module->add(globalObject, "_base_executable"_s, text(baseExecutable));
    module->add(globalObject, "_home"_s, jsUndefined());
    module->add(globalObject, "_framework"_s, text(emptyString()));
    module->add(globalObject, "pycache_prefix"_s, configuration.pycachePrefix.isNull() ? jsUndefined() : text(configuration.pycachePrefix));
    module->add(globalObject, "dont_write_bytecode"_s, jsBoolean(!configuration.writeBytecode));

    JSValue versionInfo = makeStructSeq(globalObject, makeStructSeqType(globalObject, versionInfoDef), { jsNumber(versionMajor), jsNumber(versionMinor), jsNumber(versionMicro), text("final"_s), jsNumber(0) });
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "version_info"_s, versionInfo);
    module->add(globalObject, "hexversion"_s, jsNumber(versionHex));
    module->add(globalObject, "version"_s, text(versionText()));
    module->add(globalObject, "api_version"_s, jsNumber(1013));
    module->add(globalObject, "abiflags"_s, text(emptyString()));
    module->add(globalObject, "copyright"_s, text("Copyright (c) 2001 Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) 1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."_s));
    module->add(globalObject, "_git"_s, PyTuple::create(globalObject, { text("Bun"_s), text(emptyString()), text(emptyString()) }));

    PyDict* implementation = PyDict::create(globalObject);
    implementation->set(vm, rt.names.name(), text("bun"_s));
    implementation->set(vm, internString(vm, "cache_tag"_s), text(codeCacheTag()));
    implementation->set(vm, internString(vm, "supports_isolated_interpreters"_s), jsBoolean(false));
    implementation->set(vm, internString(vm, "version"_s), versionInfo);
    implementation->set(vm, internString(vm, "hexversion"_s), jsNumber(versionHex));
    module->add(globalObject, "implementation"_s, makeSimpleNamespace(globalObject, implementation));

    module->add(globalObject, "platform"_s, text(platformText()));
    module->add(globalObject, "byteorder"_s, text("little"_s));
    module->add(globalObject, "maxsize"_s, makeInt(globalObject, std::numeric_limits<int64_t>::max()));
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "maxunicode"_s, jsNumber(0x10ffff));
    module->add(globalObject, "float_repr_style"_s, text("short"_s));

    updateSysFlags(globalObject);
    if (configuration.lazyImports > 0)
        rt.setSlot(Runtime::Slot::LazyImportsMode, text("all"_s));
    module->add(globalObject, "float_info"_s, makeStructSeq(globalObject, makeStructSeqType(globalObject, floatInfoDef), {
        makeFloat(globalObject, std::numeric_limits<double>::max()), jsNumber(1024), jsNumber(308), makeFloat(globalObject, std::numeric_limits<double>::min()), jsNumber(-1021), jsNumber(-307),
        jsNumber(15), jsNumber(53), makeFloat(globalObject, std::numeric_limits<double>::epsilon()), jsNumber(2), jsNumber(1) }));
    module->add(globalObject, "int_info"_s, makeStructSeq(globalObject, makeStructSeqType(globalObject, intInfoDef), { jsNumber(30), jsNumber(4), jsNumber(4300), jsNumber(640) }));
    module->add(globalObject, "hash_info"_s, makeStructSeq(globalObject, makeStructSeqType(globalObject, hashInfoDef), {
        jsNumber(64), makeInt(globalObject, static_cast<int64_t>(2305843009213693951)), jsNumber(314159), jsNumber(0), jsNumber(1000003), text("wtf"_s), jsNumber(64), jsNumber(0), jsNumber(0) }));
    {
        JSValue version = jsUndefined();
#if defined(_CS_GNU_LIBPTHREAD_VERSION)
        char buffer[255];
        if (size_t length = confstr(_CS_GNU_LIBPTHREAD_VERSION, buffer, sizeof(buffer)); length > 1 && length < sizeof(buffer))
            version = makeStr(vm, String::fromLatin1(buffer));
#endif
#if OS(WINDOWS)
        JSValue name = text("nt"_s);
#else
        JSValue name = text("pthread"_s);
#endif
        module->add(globalObject, "thread_info"_s, makeStructSeq(globalObject, makeStructSeqType(globalObject, threadInfoDef), { name, text("pymutex"_s), version }));
        RETURN_IF_EXCEPTION(scope, void());
    }

    MarkedArgumentBuffer names;
    names.append(rt.names.builtins());
    names.append(rt.names.sys());
    for (auto& def : nativeModules())
        names.append(internString(vm, def.name));
    for (auto& def : hostNativeModules())
        names.append(internString(vm, def.name));
    module->add(globalObject, "builtin_module_names"_s, PyTuple::create(globalObject, spanOf(names)));
    PySet* stdlibNames = PySet::createFrozen(globalObject);
    for (ASCIILiteral name : stdlibModuleNames) {
        stdlibNames->add(globalObject, internString(vm, name));
        RETURN_IF_EXCEPTION(scope, void());
    }
    module->add(globalObject, "stdlib_module_names"_s, stdlibNames);
}

#if !OS(WINDOWS)

// What sysconfig knows, on POSIX, it has from this module, which the build of CPython writes from its Makefile and pyconfig.h.
// Here it has the variables that mean something without either. There are no extension modules yet. SOABI and EXT_SUFFIX say what
// they would be called: packaging.tags, and so pip, makes the name of the ABI from them and gives up without.
void initializeSysconfigDataModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* variables = PyDict::create(globalObject);
    auto set = [&](ASCIILiteral name, JSValue value) { variables->set(vm, internString(vm, name), value); };
    auto text = [&](const String& string) -> JSValue { return makeStr(vm, string); };
    auto fromSys = [&](ASCIILiteral name) -> String {
        JSValue value = rt.sysDict()->get(internString(vm, name));
        return value && isStr(value) ? String(strValue(value)->value(globalObject)) : emptyString();
    };
    String prefix = fromSys("base_prefix"_s);
    RETURN_IF_EXCEPTION(scope, void());
    String library = fromSys("_stdlib_dir"_s);
    RETURN_IF_EXCEPTION(scope, void());
    String version = makeString(versionMajor, '.', versionMinor);
    String include = makeString(prefix, "/include/python"_s, version);
    set("prefix"_s, text(prefix));
    set("exec_prefix"_s, text(prefix));
    set("BINDIR"_s, text(makeString(prefix, "/bin"_s)));
    set("LIBDIR"_s, text(makeString(prefix, "/lib"_s)));
    set("LIBDEST"_s, text(library));
    set("BINLIBDEST"_s, text(library));
    set("INCLUDEPY"_s, text(include));
    set("CONFINCLUDEPY"_s, text(include));
    set("PLATLIBDIR"_s, text("lib"_s));
    set("VERSION"_s, text(version));
    set("LDVERSION"_s, text(version));
    set("ABIFLAGS"_s, text(emptyString()));
    set("MULTIARCH"_s, text(emptyString()));
#if OS(DARWIN)
    String soabi = makeString(codeCacheTag(), "-darwin"_s);
#elif CPU(ARM64)
    String soabi = makeString(codeCacheTag(), "-aarch64-linux-gnu"_s);
#else
    String soabi = makeString(codeCacheTag(), "-x86_64-linux-gnu"_s);
#endif
    set("SOABI"_s, text(soabi));
    set("EXT_SUFFIX"_s, text(makeString('.', soabi, ".so"_s)));
    set("SHLIB_SUFFIX"_s, text(".so"_s));
    // How an extension module would be built: with the compiler of the system. setuptools insists on these. There is no Python.h, so
    // the compiler fails, which is what the many packages whose extension module is optional are prepared for: they go on without.
    set("CC"_s, text("cc"_s));
    set("CXX"_s, text("c++"_s));
    set("CFLAGS"_s, text(emptyString()));
    set("CPPFLAGS"_s, text(emptyString()));
    set("LDFLAGS"_s, text(emptyString()));
    set("OPT"_s, text(emptyString()));
    set("CCSHARED"_s, text("-fPIC"_s));
#if OS(DARWIN)
    set("LDSHARED"_s, text("cc -bundle -undefined dynamic_lookup"_s));
    set("LDCXXSHARED"_s, text("c++ -bundle -undefined dynamic_lookup"_s));
#else
    set("LDSHARED"_s, text("cc -shared"_s));
    set("LDCXXSHARED"_s, text("c++ -shared"_s));
#endif
    set("AR"_s, text("ar"_s));
    set("ARFLAGS"_s, text("rcs"_s));
    set("MACHDEP"_s, text(fromSys("platform"_s)));
    set("EXE"_s, text(emptyString()));
    set("Py_DEBUG"_s, jsNumber(0));
    set("Py_GIL_DISABLED"_s, jsNumber(0));
    set("Py_ENABLE_SHARED"_s, jsNumber(0));
    set("Py_HASH_ALGORITHM"_s, jsNumber(0)); // None of CPython's: see sys.hash_info.algorithm.
    set("WITH_DOC_STRINGS"_s, jsNumber(1));
    // os.urandom() needs no file descriptor.
    set("HAVE_GETENTROPY"_s, jsNumber(1));
#if OS(LINUX)
    set("HAVE_GETRANDOM"_s, jsNumber(1));
    set("HAVE_GETRANDOM_SYSCALL"_s, jsNumber(1));
#endif
    set("SIZEOF_SHORT"_s, jsNumber(sizeof(short)));
    set("SIZEOF_INT"_s, jsNumber(sizeof(int)));
    set("SIZEOF_LONG"_s, jsNumber(sizeof(long)));
    set("SIZEOF_LONG_LONG"_s, jsNumber(sizeof(long long)));
    set("SIZEOF_FLOAT"_s, jsNumber(sizeof(float)));
    set("SIZEOF_DOUBLE"_s, jsNumber(sizeof(double)));
    set("SIZEOF_VOID_P"_s, jsNumber(sizeof(void*)));
    set("SIZEOF_SIZE_T"_s, jsNumber(sizeof(size_t)));
    set("SIZEOF_UINTPTR_T"_s, jsNumber(sizeof(uintptr_t)));
    set("SIZEOF_WCHAR_T"_s, jsNumber(sizeof(wchar_t)));
    set("SIZEOF_TIME_T"_s, jsNumber(sizeof(time_t)));
    set("SIZEOF_OFF_T"_s, jsNumber(sizeof(off_t)));
    set("SIZEOF_PID_T"_s, jsNumber(sizeof(pid_t)));
    // Where zoneinfo looks for the time zones of the system.
    set("TZPATH"_s, text("/usr/share/zoneinfo:/usr/lib/zoneinfo:/usr/share/lib/zoneinfo:/etc/zoneinfo"_s));
    module->add(globalObject, "build_time_vars"_s, variables);
}

#endif // !OS(WINDOWS)

} // namespace Python
} // namespace JSC
