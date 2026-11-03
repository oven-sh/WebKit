/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PyNative.h"
#include "PyNumbers.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include "PyType.h"
#include "TopExceptionScope.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _warnings: whether a warning is shown, ignored or raised. It is what the runtime itself warns through, before
// Lib/warnings.py has been imported as well as after, and that module takes warn(), warn_explicit() and the filters from here.
// How a warning is shown is left to warnings._showwarnmsg(), if the module can be had. (Python/_warnings.c in CPython is the
// reference.)

enum WarningsStateField : unsigned {
    WarningsFilters, // list
    WarningsOnceRegistry, // dict
    WarningsDefaultAction, // str
    WarningsContext, // ContextVar
    WarningsFiltersVersion, // int
    WarningsStateFieldCount
};

static JSArray* warningsState(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue state = rt.slot(Runtime::Slot::Warnings))
        return asList(state);

    auto filter = [&](PyType* category, ASCIILiteral action, JSValue module) -> JSValue {
        return PyTuple::create(globalObject, { internString(vm, action), jsUndefined(), category, module, jsNumber(0) });
    };
    JSArray* filters = newList(globalObject);
    listAppend(globalObject, filters, filter(rt.DeprecationWarning(), "default"_s, rt.names.dMain()));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, filters, filter(rt.DeprecationWarning(), "ignore"_s, jsUndefined()));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, filters, filter(rt.PendingDeprecationWarning(), "ignore"_s, jsUndefined()));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, filters, filter(rt.ImportWarning(), "ignore"_s, jsUndefined()));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, filters, filter(rt.ResourceWarning(), "ignore"_s, jsUndefined()));
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSArray* state = newList(globalObject);
    listAppend(globalObject, state, filters);
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, state, PyDict::create(globalObject));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, state, internString(vm, "default"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, state, jsUndefined());
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, state, jsNumber(0));
    rt.setSlot(Runtime::Slot::Warnings, state);
    return state;
}

// Null: an exception.
static JSValue warningsContextVariable(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* state = warningsState(globalObject);
    JSValue variable = listGet(state, WarningsContext);
    if (!isNone(variable))
        return variable;
    JSValue module = importModule(globalObject, "_contextvars"_s);
    RETURN_IF_EXCEPTION(scope, { });
    variable = callMethod(globalObject, module, internString(vm, "ContextVar"_s), makeStr(vm, "_warnings_context"_s));
    RETURN_IF_EXCEPTION(scope, { });
    listSet(globalObject, state, WarningsContext, variable);
    return variable;
}

class WarningsLocker {
public:
    explicit WarningsLocker(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_isHeld(runtime(globalObject).threads().warningsLock.acquire(globalObject))
    {
    }
    ~WarningsLocker()
    {
        if (m_isHeld)
            runtime(m_globalObject).threads().warningsLock.release(m_globalObject);
    }
    // False: an exception.
    explicit operator bool() const { return m_isHeld; }

private:
    JSGlobalObject* m_globalObject;
    bool m_isHeld;
};

// warnings.name. Empty, perhaps with an exception: there is no such thing.
static JSValue warningsAttribute(JSGlobalObject* globalObject, ASCIILiteral name, bool tryImport)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module;
    if (tryImport && !rt.threads().isFinalized()) {
        module = importModule(globalObject, "warnings"_s);
        if (scope.exception()) [[unlikely]] {
            clearPendingExceptionIfMatches(globalObject, rt.ImportError());
            return { };
        }
    } else {
        module = rt.modules()->get(internString(vm, "warnings"_s));
        if (!module)
            return { };
    }
    RELEASE_AND_RETURN(scope, getAttrIfPresent(globalObject, module, internString(vm, name)));
}

enum class Matched : uint8_t { No, Yes, Error };

static Matched warningsCheckMatched(JSGlobalObject* globalObject, JSValue pattern, JSValue argument, JSValue filename)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(pattern))
        return Matched::Yes;
    // The filters this module starts with have plain text, which must match exactly.
    if (pattern.isString()) {
        if (!argument)
            return Matched::No;
        bool isSame = isEqual(globalObject, pattern, argument);
        RETURN_IF_EXCEPTION(scope, Matched::Error);
        return isSame ? Matched::Yes : Matched::No;
    }
    JSValue result;
    if (argument)
        result = callMethod(globalObject, pattern, internString(vm, "match"_s), argument);
    else {
        JSValue module = importModule(globalObject, "_py_warnings"_s);
        RETURN_IF_EXCEPTION(scope, Matched::Error);
        result = callMethod(globalObject, module, internString(vm, "_match_filename"_s), pattern, filename);
    }
    RETURN_IF_EXCEPTION(scope, Matched::Error);
    bool isMatch = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, Matched::Error);
    return isMatch ? Matched::Yes : Matched::No;
}

// False: an exception. An empty action: no filter matches.
static bool warningsSearchFilters(JSGlobalObject* globalObject, JSValue category, JSValue text, int64_t lineno, JSValue module, JSValue filename, ASCIILiteral listName, JSArray* filters, JSValue& item, JSValue& action)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    action = { };
    for (unsigned i = 0; i < listLength(filters); ++i) {
        auto* filter = PyTuple::tryFrom(listGet(filters, i));
        if (!filter || filter->length() != 5) {
            raiseValueError(globalObject, scope, "warnings."_s, listName, " item "_s, i, " isn't a 5-tuple"_s);
            return false;
        }
        if (!isStr(filter->at(0))) {
            raiseTypeError(globalObject, scope, "action must be a string, not '"_s, typeName(globalObject, filter->at(0)), '\'');
            return false;
        }
        Matched message = warningsCheckMatched(globalObject, filter->at(1), text, { });
        EXCEPTION_ASSERT(!!scope.exception() == (message == Matched::Error));
        if (message == Matched::Error)
            return false;
        Matched moduleMatch = warningsCheckMatched(globalObject, filter->at(3), module, filename);
        EXCEPTION_ASSERT(!!scope.exception() == (moduleMatch == Matched::Error));
        if (moduleMatch == Matched::Error)
            return false;
        bool categoryMatches = isSubclass(globalObject, category, filter->at(2));
        RETURN_IF_EXCEPTION(scope, false);
        int64_t line = toSsize(globalObject, filter->at(4), runtime(globalObject).OverflowError());
        RETURN_IF_EXCEPTION(scope, false);
        if (message == Matched::Yes && categoryMatches && moduleMatch == Matched::Yes && (!line || line == lineno)) {
            item = filter;
            action = filter->at(0);
            return true;
        }
    }
    return true;
}

// The action to take. Empty: an exception.
static JSValue warningsGetFilter(JSGlobalObject* globalObject, JSValue category, JSValue text, int64_t lineno, JSValue module, JSValue filename, JSValue& item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* state = warningsState(globalObject);

    // The filters of the context (warnings.catch_warnings(), when sys.flags.context_aware_warnings is set) stand in for the others.
    JSValue variable = warningsContextVariable(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue context = callMethod(globalObject, variable, internString(vm, "get"_s), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue action;
    if (!isNone(context)) {
        JSValue contextFilters = getAttr(globalObject, context, internString(vm, "_filters"_s));
        RETURN_IF_EXCEPTION(scope, { });
        JSArray* list = tryAsList(contextFilters);
        if (!list)
            return raiseValueError(globalObject, scope, "_filters of warnings._warnings_context must be a list"_s);
        if (!warningsSearchFilters(globalObject, category, text, lineno, module, filename, "_warnings_context _filters"_s, list, item, action))
            return { };
        if (action)
            return action;
    } else {
        JSValue filters = warningsAttribute(globalObject, "filters"_s, false);
        RETURN_IF_EXCEPTION(scope, { });
        if (filters)
            listSet(globalObject, state, WarningsFilters, filters);
        JSArray* list = tryAsList(listGet(state, WarningsFilters));
        if (!list)
            return raiseValueError(globalObject, scope, "_warnings.filters must be a list"_s);
        if (failed(scope, warningsSearchFilters(globalObject, category, text, lineno, module, filename, "filters"_s, list, item, action)))
            return { };
        if (action)
            return action;
    }

    item = jsUndefined();
    JSValue defaultAction = warningsAttribute(globalObject, "defaultaction"_s, false);
    RETURN_IF_EXCEPTION(scope, { });
    if (!defaultAction)
        return listGet(state, WarningsDefaultAction);
    if (!isStr(defaultAction))
        return raiseTypeError(globalObject, scope, "_warnings.defaultaction must be a string, not '"_s, typeName(globalObject, defaultAction), '\'');
    listSet(globalObject, state, WarningsDefaultAction, defaultAction);
    return defaultAction;
}

// 1: the registry has the key. 0: it has not (and has it now, if it is to be set). -1: an exception.
static int warningsAlreadyWarned(JSGlobalObject* globalObject, PyDict* registry, JSValue key, bool shouldSet)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue currentVersion = listGet(warningsState(globalObject), WarningsFiltersVersion);
    JSString* versionKey = internString(vm, "version"_s);
    JSValue version = registry->get(versionKey);
    // What was noted under other filters no longer counts.
    if (!version || !version.isNumber() || version.asNumber() != currentVersion.asNumber()) {
        registry->clear(vm);
        registry->set(vm, versionKey, currentVersion);
    } else {
        JSValue seen = registry->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, -1);
        if (seen) {
            bool isSeen = isTrue(globalObject, seen);
            RETURN_IF_EXCEPTION(scope, -1);
            if (isSeen)
                return 1;
        }
    }
    if (shouldSet) {
        registry->set(globalObject, key, jsBoolean(true));
        RETURN_IF_EXCEPTION(scope, -1);
    }
    return 0;
}

// Without warnings.py: "filename:lineno: category: text", and the line of the source.
static void warningsShowPlainly(JSGlobalObject* globalObject, JSValue filename, int lineno, JSValue text, JSValue category, JSValue sourceLine)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto show = [&] {
        JSValue name = getAttr(globalObject, category, rt.names.dName());
        if (scope.exception())
            return;
        JSValue file = rt.sysDict()->get(rt.names.stderr_());
        if (!file || isNone(file))
            return;
        JSString* filenameText = str(globalObject, filename);
        if (scope.exception())
            return;
        JSString* nameText = str(globalObject, name);
        if (scope.exception())
            return;
        JSString* messageText = str(globalObject, text);
        if (scope.exception())
            return;
        StrBuilder line;
        line.append(String(filenameText->value(globalObject)), ':', lineno, ": "_s, String(nameText->value(globalObject)), ": "_s, String(messageText->value(globalObject)), '\n');
        if (sourceLine && isStr(sourceLine))
            line.append(String(strValue(sourceLine)->value(globalObject)), '\n');
        writeToFile(globalObject, file, makeStr(vm, line.toString()));
    };
    show();
    if (scope.exception())
        scope.clearException();
}

static bool warningsCallShow(JSGlobalObject* globalObject, JSValue category, JSValue text, JSValue message, JSValue filename, int lineno, JSValue sourceLine, JSValue source, JSValue module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue show = warningsAttribute(globalObject, "_showwarnmsg"_s, true);
    RETURN_IF_EXCEPTION(scope, false);
    if (!show) {
        warningsShowPlainly(globalObject, filename, lineno, text, category, sourceLine);
        return true;
    }
    if (!isCallable(globalObject, show)) {
        raiseTypeError(globalObject, scope, "warnings._showwarnmsg() must be set to a callable"_s);
        return false;
    }
    JSValue messageClass = warningsAttribute(globalObject, "WarningMessage"_s, false);
    RETURN_IF_EXCEPTION(scope, false);
    if (!messageClass) {
        raiseRuntimeError(globalObject, scope, "unable to get warnings.WarningMessage"_s);
        return false;
    }
    JSValue arguments[] = { message, category, filename, jsNumber(lineno), jsUndefined(), sourceLine ? sourceLine : jsUndefined(), source ? source : jsUndefined(), module ? module : jsUndefined() };
    JSValue record = call(globalObject, messageClass, CallArgs(arguments, std::size(arguments)));
    RETURN_IF_EXCEPTION(scope, false);
    call(globalObject, show, record);
    return !scope.exception();
}

static bool warningsActionIs(JSGlobalObject* globalObject, JSValue action, ASCIILiteral name)
{
    return String(strValue(action)->value(globalObject)) == name;
}

// With the lock held. `module` may be empty (the filters then go by the file name), `registry`, `sourceLine` and `source` too.
static bool warningsWarnExplicit(JSGlobalObject* globalObject, JSValue category, JSValue message, JSValue filename, int lineno, JSValue module, JSValue registryValue, JSValue sourceLine, JSValue source)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // So late that the modules are being taken apart: there are no filters to go by any more.
    if (module && isNone(module))
        return true;
    if (registryValue && isNone(registryValue))
        registryValue = { };
    PyDict* registry = nullptr;
    if (registryValue) {
        if (!isDict(registryValue)) {
            raiseTypeError(globalObject, scope, "'registry' must be a dict or None"_s);
            return false;
        }
        registry = PyDict::from(registryValue);
    }

    JSValue text;
    if (rt.typeOf(message)->isSubtypeOf(rt.Warning())) {
        text = str(globalObject, message);
        RETURN_IF_EXCEPTION(scope, false);
        category = rt.typeOf(message);
    } else {
        text = message;
        message = call(globalObject, category, message);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (source && isNone(source))
        source = { };

    JSValue key = PyTuple::create(globalObject, { text, category, jsNumber(lineno) });
    if (registry) {
        int seen = warningsAlreadyWarned(globalObject, registry, key, false);
        EXCEPTION_ASSERT(!!scope.exception() == (seen < 0));
        if (seen < 0)
            return false;
        if (seen)
            return true;
    }

    JSValue item;
    JSValue action = warningsGetFilter(globalObject, category, text, lineno, module, filename, item);
    RETURN_IF_EXCEPTION(scope, false);
    if (warningsActionIs(globalObject, action, "error"_s)) {
        raiseException(globalObject, scope, message);
        return false;
    }
    if (warningsActionIs(globalObject, action, "ignore"_s))
        return true;

    int seen = 0;
    if (!warningsActionIs(globalObject, action, "always"_s) && !warningsActionIs(globalObject, action, "all"_s)) {
        if (registry) {
            registry->set(globalObject, key, jsBoolean(true));
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (warningsActionIs(globalObject, action, "once"_s)) {
            if (!registry) {
                JSArray* state = warningsState(globalObject);
                JSValue once = warningsAttribute(globalObject, "onceregistry"_s, false);
                RETURN_IF_EXCEPTION(scope, false);
                if (once) {
                    if (!isDict(once)) {
                        raiseTypeError(globalObject, scope, "_warnings.onceregistry must be a dict, not '"_s, typeName(globalObject, once), '\'');
                        return false;
                    }
                    listSet(globalObject, state, WarningsOnceRegistry, once);
                }
                registry = PyDict::from(listGet(state, WarningsOnceRegistry));
            }
            seen = warningsAlreadyWarned(globalObject, registry, PyTuple::create(globalObject, { text, category }), true);
        } else if (warningsActionIs(globalObject, action, "module"_s)) {
            if (registry)
                seen = warningsAlreadyWarned(globalObject, registry, PyTuple::create(globalObject, { text, category }), true);
        } else if (!warningsActionIs(globalObject, action, "default"_s)) {
            String actionText = reprForMessage(globalObject, action);
            RETURN_IF_EXCEPTION(scope, false);
            String itemText = reprForMessage(globalObject, item);
            RETURN_IF_EXCEPTION(scope, false);
            raiseRuntimeError(globalObject, scope, "Unrecognized action ("_s, actionText, ") in warnings.filters:\n "_s, itemText);
            return false;
        }
    }
    if (seen < 0)
        return false;
    if (seen)
        return true;
    RELEASE_AND_RETURN(scope, warningsCallShow(globalObject, category, text, message, filename, lineno, sourceLine, source, module));
}

static bool warningsIsInternalFile(const String& filename)
{
    return filename.contains("importlib"_s) && filename.contains("_bootstrap"_s);
}

static bool warningsIsFileToSkip(JSGlobalObject* globalObject, const String& filename, PyTuple* prefixes)
{
    for (unsigned i = 0; prefixes && i < prefixes->length(); ++i) {
        if (filename.startsWith(String(strValue(prefixes->at(i))->value(globalObject))))
            return true;
    }
    return false;
}

static bool warningsDoWarn(JSGlobalObject* globalObject, JSValue message, JSValue category, int64_t stackLevel, JSValue source, PyTuple* skipFilePrefixes)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; skipFilePrefixes && i < skipFilePrefixes->length(); ++i) {
        if (!isStr(skipFilePrefixes->at(i))) {
            raiseTypeError(globalObject, scope, "Found non-str '"_s, typeName(globalObject, skipFilePrefixes->at(i)), "' in skip_file_prefixes."_s);
            return false;
        }
    }

    auto filenameOf = [](PyFrame* frame) -> const String& { return frame->function()->codeInfo().filename; };
    unsigned depth = 0;
    PyFrame* frame = currentFrame(globalObject);
    if (stackLevel <= 0 || (frame && warningsIsInternalFile(filenameOf(frame)))) {
        while (--stackLevel > 0 && frame)
            frame = currentFrame(globalObject, ++depth);
    } else {
        // The machinery of import does not count.
        while (--stackLevel > 0 && frame) {
            do {
                frame = currentFrame(globalObject, ++depth);
            } while (frame && (warningsIsInternalFile(filenameOf(frame)) || warningsIsFileToSkip(globalObject, filenameOf(frame), skipFilePrefixes)));
        }
    }

    PyDict* globals = frame ? frame->function()->globals() : rt.sysDict();
    JSValue filename = makeStr(vm, frame ? filenameOf(frame) : String("<sys>"_s));
    int lineno = frame ? frame->lineNumber() : 0;

    JSString* registryName = internString(vm, "__warningregistry__"_s);
    JSValue registry = globals->get(registryName);
    if (!registry) {
        registry = PyDict::create(globalObject);
        globals->set(vm, registryName, registry);
    }
    JSValue module = globals->get(rt.names.dName());
    if (!module || (!isNone(module) && !isStr(module)))
        module = makeStr(vm, "<string>"_s);

    WarningsLocker locker(globalObject);
    if (!locker)
        return false;
    RELEASE_AND_RETURN(scope, warningsWarnExplicit(globalObject, category, message, filename, lineno, module, registry, { }, source));
}

// ---- For the runtime ----

bool warn(JSGlobalObject* globalObject, PyType* category, const String& message, int stackLevel)
{
    return warningsDoWarn(globalObject, makeStr(getVM(globalObject), message), category, stackLevel, { }, nullptr);
}

bool warnAbout(JSGlobalObject* globalObject, PyType* category, const String& message, int stackLevel, JSValue source)
{
    return warningsDoWarn(globalObject, makeStr(getVM(globalObject), message), category, stackLevel, source, nullptr);
}

bool warnExplicit(JSGlobalObject* globalObject, PyType* category, const String& message, const String& filename, int lineno, JSValue module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    WarningsLocker locker(globalObject);
    if (!locker)
        return false;
    RELEASE_AND_RETURN(scope, warningsWarnExplicit(globalObject, category, makeStr(vm, message), makeStr(vm, filename), lineno, module && !isNone(module) ? module : JSValue(), { }, { }, { }));
}

// ---- The module ----

static JSValue warningsWarn(JSGlobalObject* globalObject, JSValue message, JSValue category, JSValue stackLevelValue, JSValue source, JSValue skipFilePrefixes)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t stackLevel = 1;
    if (stackLevelValue) {
        JSValue index = toIndex(globalObject, stackLevelValue);
        RETURN_IF_EXCEPTION(scope, { });
        auto converted = tryInt64(index);
        if (!converted)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        stackLevel = *converted;
    }
    PyTuple* prefixes = nullptr;
    if (skipFilePrefixes) {
        prefixes = PyTuple::tryFrom(skipFilePrefixes);
        if (!prefixes)
            return raiseTypeError(globalObject, scope, "warn() argument 'skip_file_prefixes' must be tuple, not "_s, typeNameOfArgument(globalObject, skipFilePrefixes));
    }

    if (rt.typeOf(message)->isSubtypeOf(rt.Warning()))
        category = rt.typeOf(message);
    else if (!category || isNone(category))
        category = rt.UserWarning();
    else if (auto* type = PyType::tryFrom(category); !type)
        return raiseTypeError(globalObject, scope, "category must be a Warning subclass, not '"_s, typeNameForMessage(globalObject, rt.typeOf(category)), '\'');
    else if (!type->isSubtypeOf(rt.Warning()))
        return raiseTypeError(globalObject, scope, "category must be a Warning subclass, not class '"_s, type->name(), '\'');

    if (prefixes) {
        if (prefixes->length())
            stackLevel = std::max<int64_t>(stackLevel, 2);
        else
            prefixes = nullptr;
    }
    warningsDoWarn(globalObject, message, category, stackLevel, source, prefixes);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

// The line of the module, as its loader has it. Empty, perhaps with an exception: it is not to be had.
static JSValue warningsSourceLine(JSGlobalObject* globalObject, PyDict* moduleGlobals, int lineno)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue external = importModule(globalObject, "_frozen_importlib_external"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue loader = callMethod(globalObject, external, internString(vm, "_bless_my_loader"_s), moduleGlobals);
    RETURN_IF_EXCEPTION(scope, { });

    // A module that runs as a script is called __main__, which its loader knows nothing of.
    JSValue moduleName;
    if (JSValue spec = moduleGlobals->get(rt.names.dSpec())) {
        moduleName = getAttrIfPresent(globalObject, spec, internString(vm, "name"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (moduleName && isNone(moduleName))
            moduleName = { };
    }
    if (!moduleName) {
        moduleName = moduleGlobals->get(rt.names.dName());
        if (!moduleName)
            return { };
    }
    JSValue getSource = getAttrIfPresent(globalObject, loader, internString(vm, "get_source"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!getSource)
        return { };
    JSValue source = call(globalObject, getSource, moduleName);
    if (scope.exception()) [[unlikely]] {
        clearPendingExceptionIfMatches(globalObject, rt.ImportError());
        return { };
    }
    if (isNone(source))
        return { };
    JSValue lines = callMethod(globalObject, source, internString(vm, "splitlines"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = tryAsList(lines);
    if (!list || lineno < 1 || static_cast<unsigned>(lineno) > listLength(list))
        return { };
    return listGet(list, lineno - 1);
}

static JSValue warningsWarnExplicitFunction(JSGlobalObject* globalObject, JSValue message, JSValue category, JSValue filename, JSValue linenoValue, JSValue module, JSValue registry, JSValue moduleGlobals, JSValue source)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(filename))
        return raiseTypeError(globalObject, scope, "warn_explicit() argument 'filename' must be str, not "_s, typeNameOfArgument(globalObject, filename));
    int lineno = toCInt(globalObject, linenoValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue sourceLine;
    if (moduleGlobals && !isNone(moduleGlobals)) {
        if (!PyDict::is(moduleGlobals))
            return raiseTypeError(globalObject, scope, "module_globals must be a dict or a frozendict, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, moduleGlobals)));
        sourceLine = warningsSourceLine(globalObject, PyDict::from(moduleGlobals), lineno);
        RETURN_IF_EXCEPTION(scope, { });
    }
    WarningsLocker locker(globalObject);
    if (!locker)
        return { };
    warningsWarnExplicit(globalObject, category, message, filename, lineno, module, registry, sourceLine, source);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue warningsFiltersMutated(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!runtime(globalObject).threads().warningsLock.isHeld())
        return raiseRuntimeError(globalObject, scope, "warnings lock is not held"_s);
    JSArray* state = warningsState(globalObject);
    listSet(globalObject, state, WarningsFiltersVersion, jsNumber(listGet(state, WarningsFiltersVersion).asNumber() + 1));
    return jsUndefined();
}

static JSValue warningsAcquireLock(JSGlobalObject* globalObject)
{
    if (!runtime(globalObject).threads().warningsLock.acquire(globalObject))
        return { };
    return jsUndefined();
}

static JSValue warningsReleaseLock(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ThreadLock& lock = runtime(globalObject).threads().warningsLock;
    if (!lock.isHeldByThisThread(globalObject))
        return raiseRuntimeError(globalObject, scope, "cannot release un-acquired lock"_s);
    lock.release(globalObject);
    return jsUndefined();
}

static constexpr NativeDef warningsFunctions[] = {
    PY_FUNCTION("warn", warningsWarn, "(message, category=None, stacklevel=1, source=None, *, skip_file_prefixes=())"),
    PY_FUNCTION("warn_explicit", warningsWarnExplicitFunction, "(message, category, filename, lineno, module=<unrepresentable>, registry=None, module_globals=None, source=None)"),
    PY_FUNCTION("_filters_mutated_lock_held", warningsFiltersMutated, "()"),
    PY_FUNCTION("_acquire_lock", warningsAcquireLock, "()"),
    PY_FUNCTION("_release_lock", warningsReleaseLock, "()"),
};

void initializeWarningsModule(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->addFunctions(globalObject, warningsFunctions);
    JSArray* state = warningsState(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "filters"_s, listGet(state, WarningsFilters));
    module->add(globalObject, "_onceregistry"_s, listGet(state, WarningsOnceRegistry));
    module->add(globalObject, "_defaultaction"_s, listGet(state, WarningsDefaultAction));
    JSValue variable = warningsContextVariable(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (variable)
        module->add(globalObject, "_warnings_context"_s, variable);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
