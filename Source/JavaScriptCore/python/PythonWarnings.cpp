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

#include "PyDict.h"
#include "PyFrame.h"
#include "PyTuple.h"
#include "PythonContextVars.h"
#include "PythonSequences.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>

// _warnings: Python/_warnings.c of CPython, function for function. It is what decides whether a warning is shown, raised or passed over. warnings.py, if it has been imported, has the filters and shows
// what is to be shown, and this goes by what that has.

namespace JSC { namespace Python {

static WarningsState& stateOf(JSGlobalObject* globalObject) { return globalObject->pyRealm()->warnings(); }

// init_filters(), and _PyWarnings_InitState()
void initializeWarnings(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    WarningsState& state = stateOf(globalObject);
    MarkedArgumentBuffer filters;
    auto add = [&] (PyType* category, ASCIILiteral action, ASCIILiteral module) {
        filters.append(PyTuple::create(globalObject, { jsString(vm, String(action)), jsUndefined(), category, module.isNull() ? jsUndefined() : JSValue(jsString(vm, String(module))), jsNumber(0) }));
    };
    add(realm->typeDeprecationWarning(), "default"_s, "__main__"_s);
    add(realm->typeDeprecationWarning(), "ignore"_s, { });
    add(realm->typePendingDeprecationWarning(), "ignore"_s, { });
    add(realm->typeImportWarning(), "ignore"_s, { });
    add(realm->typeResourceWarning(), "ignore"_s, { });
    state.filters.set(vm, realm, constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), filters));
    state.onceRegistry.set(vm, realm, PyDict::create(globalObject));
    state.defaultAction.set(vm, realm, jsString(vm, String("default"_s)));
    state.context.set(vm, realm, newContextVariable(globalObject, jsString(vm, String("_warnings_context"_s))));
}

// What Argument Clinic does for a Py_ssize_t
static std::optional<int64_t> toSsize(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto result = toIndex(globalObject, value);
    if (scope.exception() && catchException(globalObject, BuiltinType::IndexError))
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C ssize_t"_s);
    return result;
}

// check_matched()
static bool checkMatched(JSGlobalObject* globalObject, JSValue pattern, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(pattern))
        return true;
    // What the filters that there are to begin with have has to be the whole of it.
    if (pattern.isString()) {
        if (!argument.isString()) {
            raiseTypeError(globalObject, scope, makeString("Can't compare "_s, typeName(globalObject, pattern), " and "_s, typeName(globalObject, argument)));
            return false;
        }
        bool isSame = asString(pattern)->equal(globalObject, asString(argument));
        RETURN_IF_EXCEPTION(scope, false);
        return isSame;
    }
    // Otherwise it is taken to be a regular expression.
    JSValue match = getAttribute(globalObject, pattern, Identifier::fromString(vm, "match"_s));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue result = call(globalObject, match, argument);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

// get_warnings_attr(): something of warnings.py's. Empty if there is no such thing, and if it raises.
static JSValue warningsAttribute(JSGlobalObject* globalObject, ASCIILiteral name, bool tryImport)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module;
    if (tryImport) {
        module = importModule(globalObject, nullptr, "warnings"_s, jsUndefined(), 0, false);
        if (scope.exception()) {
            // What is written here will do.
            catchException(globalObject, BuiltinType::ImportError);
            return { };
        }
    } else {
        module = uncheckedDowncast<PyDict>(globalObject->pyRealm()->modules())->getString(globalObject, "warnings"_s);
        if (!module)
            return { };
    }
    RELEASE_AND_RETURN(scope, getAttributeIfPresent(globalObject, module, Identifier::fromString(vm, name)));
}

// get_warnings_context_filters(): None if there is no context. Empty if it raises.
static JSValue contextFilters(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue context = contextVariableValue(globalObject, stateOf(globalObject).context.get());
    if (!context || isNone(context))
        return jsUndefined();
    JSValue filters = getAttribute(globalObject, context, Identifier::fromString(vm, "_filters"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isList(filters))
        return raiseValueError(globalObject, scope, "_filters of warnings._warnings_context must be a list"_s);
    return filters;
}

// get_warnings_filters()
static JSArray* globalFilters(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue filters = warningsAttribute(globalObject, "filters"_s, false);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!filters)
        return stateOf(globalObject).filters.get();
    if (!isList(filters)) {
        raiseValueError(globalObject, scope, "_warnings.filters must be a list"_s);
        return nullptr;
    }
    stateOf(globalObject).filters.set(vm, globalObject->pyRealm(), asList(filters));
    return asList(filters);
}

// get_once_registry()
static PyDict* onceRegistry(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue registry = warningsAttribute(globalObject, "onceregistry"_s, false);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!registry)
        return stateOf(globalObject).onceRegistry.get();
    if (!isDict(registry)) {
        raiseTypeError(globalObject, scope, makeString("_warnings.onceregistry must be a dict, not '"_s, typeName(globalObject, registry), '\''));
        return nullptr;
    }
    stateOf(globalObject).onceRegistry.set(vm, globalObject->pyRealm(), asDict(registry));
    return asDict(registry);
}

// get_default_action()
static JSString* defaultAction(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue action = warningsAttribute(globalObject, "defaultaction"_s, false);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!action)
        return stateOf(globalObject).defaultAction.get();
    if (!action.isString()) {
        raiseTypeError(globalObject, scope, makeString("_warnings.defaultaction must be a string, not '"_s, typeName(globalObject, action), '\''));
        return nullptr;
    }
    stateOf(globalObject).defaultAction.set(vm, globalObject->pyRealm(), asString(action));
    return asString(action);
}

namespace {

struct Filter {
    JSValue action; // Empty if none matches.
    JSValue item { jsUndefined() };
};

} // anonymous namespace

// filter_search()
static Filter searchFilters(JSGlobalObject* globalObject, JSValue category, JSValue text, int64_t line, JSValue module, ASCIILiteral listName, JSArray* filters)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < filters->length(); ++i) {
        JSValue item = listGet(globalObject, filters, i);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isTuple(item) || asTuple(item)->length() != 5) {
            raiseValueError(globalObject, scope, makeString("warnings."_s, listName, " item "_s, i, " isn't a 5-tuple"_s));
            return { };
        }
        PyTuple* tuple = asTuple(item);
        JSValue action = tuple->at(0);
        if (!isInstance(globalObject, action, globalObject->pyRealm()->typeStr())) {
            raiseTypeError(globalObject, scope, makeString("action must be a string, not '"_s, typeName(globalObject, action), '\''));
            return { };
        }
        bool isGoodMessage = checkMatched(globalObject, tuple->at(1), text);
        RETURN_IF_EXCEPTION(scope, { });
        bool isGoodModule = checkMatched(globalObject, tuple->at(3), module);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSubclass = isSubclassOf(globalObject, category, tuple->at(2));
        RETURN_IF_EXCEPTION(scope, { });
        // PyLong_AsSsize_t(), which will have nothing but an int.
        if (!isInstance(globalObject, tuple->at(4), globalObject->pyRealm()->typeInt())) {
            raiseTypeError(globalObject, scope, "an integer is required"_s);
            return { };
        }
        auto filterLine = toSsize(globalObject, tuple->at(4));
        RETURN_IF_EXCEPTION(scope, { });
        if (isGoodMessage && isSubclass && isGoodModule && (!*filterLine || line == *filterLine))
            return { action, item };
    }
    return { };
}

// get_filter()
static Filter getFilter(JSGlobalObject* globalObject, JSValue category, JSValue text, int64_t line, JSValue module)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue ofContext = contextFilters(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Filter found;
    if (isNone(ofContext)) {
        JSArray* filters = globalFilters(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        found = searchFilters(globalObject, category, text, line, module, "filters"_s, filters);
    } else
        found = searchFilters(globalObject, category, text, line, module, "_warnings_context _filters"_s, asList(ofContext));
    RETURN_IF_EXCEPTION(scope, { });
    if (found.action)
        return found;
    JSString* action = defaultAction(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return { action, jsUndefined() };
}

// already_warned()
static bool alreadyWarned(JSGlobalObject* globalObject, PyDict* registry, JSValue key, bool shouldSet)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    long version = stateOf(globalObject).filtersVersion;
    JSValue recorded = registry->getString(globalObject, "version"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (!recorded || !recorded.isInt32() || recorded.asInt32() != version) {
        registry->clear(globalObject);
        registry->setString(globalObject, "version"_s, jsNumber(static_cast<int32_t>(version)));
        RETURN_IF_EXCEPTION(scope, false);
    } else {
        JSValue warned = registry->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        if (warned) {
            bool wasWarned = isTrue(globalObject, warned);
            RETURN_IF_EXCEPTION(scope, false);
            if (wasWarned)
                return true;
        }
    }
    if (shouldSet) {
        registry->set(globalObject, key, jsBoolean(true));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return false;
}

// normalize_module()
static String normalizeModule(const String& filename)
{
    if (filename.isEmpty())
        return "<unknown>"_s;
    return filename.endsWith(".py"_s) ? filename.left(filename.length() - 3) : filename;
}

// _Py_DisplaySourceLine(): a line of a file, without what it is indented by. Null if there is no such line, or no such file.
static String sourceLine(JSGlobalObject* globalObject, const String& filename, int64_t line)
{
    if (line <= 0 || filename.isEmpty())
        return { };
    SourceCode source = readSourceIfPresent(globalObject, filename);
    if (source.isNull()) {
        // _Py_FindSourceFile(): a file of that name in one of the directories that modules are looked for in.
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        size_t slash = filename.reverseFind('/');
        String tail = slash == notFound ? filename : filename.substring(slash + 1);
        JSValue path = sysAttribute(globalObject, "path"_s);
        if (!path || !isList(path))
            return { };
        for (unsigned i = 0; i < asList(path)->length() && source.isNull(); ++i) {
            JSValue directory = listGet(globalObject, asList(path), i);
            if (scope.exception()) {
                scope.clearException();
                return { };
            }
            if (!directory.isString())
                continue;
            String prefix = asString(directory)->value(globalObject);
            source = readSourceIfPresent(globalObject, makeString(prefix, prefix.isEmpty() || prefix.endsWith('/') ? ""_s : "/"_s, tail));
        }
        if (source.isNull())
            return { };
    }
    StringView text = source.view();
    size_t start = 0;
    for (int64_t i = 1; i < line; ++i) {
        start = text.find('\n', start);
        if (start == notFound)
            return { };
        ++start;
    }
    if (start >= text.length())
        return { };
    size_t end = text.find('\n', start);
    StringView result = text.substring(start, end == notFound ? text.length() - start : end - start);
    while (!result.isEmpty() && (result[0] == ' ' || result[0] == '\t' || result[0] == '\f'))
        result = result.substring(1);
    while (!result.isEmpty() && result[result.length() - 1] == '\r')
        result = result.left(result.length() - 1);
    return result.toString();
}

// show_warning(): what is done with it if there is no warnings.py
static void showWarning(JSGlobalObject* globalObject, JSValue filename, int64_t line, JSValue text, JSValue category, JSValue givenSourceLine)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto finish = makeScopeExit([&] {
        scope.clearException();
    });
    JSValue name = getAttribute(globalObject, category, vm.pythonNames().dunder_name);
    if (scope.exception())
        return;
    JSValue file = sysAttribute(globalObject, "stderr"_s);
    if (!file) {
        dataLogLn("lost sys.stderr");
        return;
    }
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    if (scope.exception())
        return;
    // False if it raised.
    auto writeString = [&] (const String& string) {
        call(globalObject, write, jsString(vm, string));
        return !scope.exception();
    };
    auto writeObject = [&] (JSValue value) {
        String string = str(globalObject, value);
        return !scope.exception() && writeString(string);
    };
    if (!writeObject(filename) || !writeString(makeString(':', line, ": "_s)) || !writeObject(name) || !writeString(": "_s) || !writeObject(text) || !writeString("\n"_s))
        return;
    // It works out what the line is without what it is indented by, and writes it as it was.
    if (givenSourceLine) {
        if (writeObject(givenSourceLine))
            writeString("\n"_s);
        return;
    }
    if (!filename.isString())
        return;
    String found = sourceLine(globalObject, asString(filename)->value(globalObject), line);
    if (!found.isNull())
        writeString(makeString("  "_s, found, '\n'));
}

// call_show_warning()
static bool callShowWarning(JSGlobalObject* globalObject, JSValue category, JSValue text, JSValue message, JSValue filename, int64_t line, JSValue givenSourceLine, JSValue source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue show = warningsAttribute(globalObject, "_showwarnmsg"_s, true);
    RETURN_IF_EXCEPTION(scope, false);
    if (!show) {
        showWarning(globalObject, filename, line, text, category, givenSourceLine);
        return true;
    }
    if (!isCallable(globalObject, show)) {
        raiseTypeError(globalObject, scope, "warnings._showwarnmsg() must be set to a callable"_s);
        return false;
    }
    JSValue messageClass = warningsAttribute(globalObject, "WarningMessage"_s, false);
    RETURN_IF_EXCEPTION(scope, false);
    if (!messageClass) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "unable to get warnings.WarningMessage"_s);
        return false;
    }
    MarkedArgumentBuffer arguments;
    for (JSValue argument : { message, category, filename, jsNumber(line), jsUndefined(), jsUndefined() })
        arguments.append(argument);
    if (source)
        arguments.append(source);
    JSValue made = call(globalObject, messageClass, arguments);
    RETURN_IF_EXCEPTION(scope, false);
    call(globalObject, show, made);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// warn_explicit(). `module` and `registry` are empty if they were not given. False if it raised, which it does if that is what is to be done with the warning.
static bool warnExplicit(JSGlobalObject* globalObject, JSValue category, JSValue message, JSValue filename, int64_t line, JSValue module, JSValue registry, JSValue givenSourceLine, JSValue source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (module && isNone(module))
        return true;
    if (registry && !isDict(registry) && !isNone(registry)) {
        raiseTypeError(globalObject, scope, "'registry' must be a dict or None"_s);
        return false;
    }
    PyDict* registryDict = registry && !isNone(registry) ? asDict(registry) : nullptr;
    if (!module) {
        String name = asString(filename)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        module = jsString(vm, normalizeModule(name));
    }

    JSValue text;
    if (isInstance(globalObject, message, realm->typeWarning())) {
        String string = str(globalObject, message);
        RETURN_IF_EXCEPTION(scope, false);
        text = jsString(vm, string);
        category = typeOf(globalObject, message);
    } else {
        text = message;
        message = call(globalObject, category, message);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (source && isNone(source))
        source = { };

    JSValue key = PyTuple::create(globalObject, { text, category, jsNumber(line) });
    if (registryDict) {
        bool wasWarned = alreadyWarned(globalObject, registryDict, key, false);
        RETURN_IF_EXCEPTION(scope, false);
        if (wasWarned)
            return true;
    }

    Filter filter = getFilter(globalObject, category, text, line, module);
    RETURN_IF_EXCEPTION(scope, false);
    String action = str(globalObject, filter.action);
    RETURN_IF_EXCEPTION(scope, false);
    if (action == "error"_s) {
        throwException(globalObject, scope, message);
        return false;
    }
    if (action == "ignore"_s)
        return true;

    // That it has been here is kept, unless it is to be shown every time.
    bool wasWarned = false;
    if (action != "always"_s && action != "all"_s) {
        if (registryDict) {
            registryDict->set(globalObject, key, jsBoolean(true));
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (action == "once"_s) {
            if (!registryDict) {
                registryDict = onceRegistry(globalObject);
                RETURN_IF_EXCEPTION(scope, false);
            }
            wasWarned = alreadyWarned(globalObject, registryDict, PyTuple::create(globalObject, { text, category }), true);
        } else if (action == "module"_s) {
            if (registryDict)
                wasWarned = alreadyWarned(globalObject, registryDict, PyTuple::create(globalObject, { text, category }), true);
        } else if (action != "default"_s) {
            String shownAction = repr(globalObject, filter.action);
            RETURN_IF_EXCEPTION(scope, false);
            String shownItem = repr(globalObject, filter.item);
            RETURN_IF_EXCEPTION(scope, false);
            raise(globalObject, scope, BuiltinType::RuntimeError, makeString("Unrecognized action ("_s, shownAction, ") in warnings.filters:\n "_s, shownItem));
            return false;
        }
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (wasWarned)
        return true;
    RELEASE_AND_RETURN(scope, callShowWarning(globalObject, category, text, message, filename, line, givenSourceLine, source));
}

// ---- Whose fault it is

static String filenameOf(CallFrame* frame) { return frame->codeBlock()->ownerExecutable()->source().provider()->sourceURL(); }

// is_internal_filename()
static bool isInternalFilename(const String& filename) { return filename.contains("importlib"_s) && filename.contains("_bootstrap"_s); }

// is_filename_to_skip()
static bool isFilenameToSkip(JSGlobalObject* globalObject, const String& filename, PyTuple* prefixes)
{
    if (!prefixes)
        return false;
    for (auto& prefix : prefixes->span()) {
        if (filename.startsWith(asString(prefix.get())->value(globalObject).data))
            return true;
    }
    return false;
}

// next_external_frame()
static CallFrame* nextExternalFrame(JSGlobalObject* globalObject, CallFrame* frame, PyTuple* prefixes)
{
    do
        frame = callerOf(frame);
    while (frame && (isInternalFilename(filenameOf(frame)) || isFilenameToSkip(globalObject, filenameOf(frame), prefixes)));
    return frame;
}

// setup_context() and do_warn(). `frame` is the innermost frame of Python's.
static bool doWarn(JSGlobalObject* globalObject, CallFrame* frame, JSValue message, JSValue category, int64_t stackLevel, JSValue source, PyTuple* prefixes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (prefixes) {
        for (auto& prefix : prefixes->span()) {
            if (!isInstance(globalObject, prefix.get(), globalObject->pyRealm()->typeStr())) {
                raiseTypeError(globalObject, scope, makeString("Found non-str '"_s, typeName(globalObject, prefix.get()), "' in skip_file_prefixes."_s));
                return false;
            }
        }
    }
    if (stackLevel <= 0 || (frame && isInternalFilename(filenameOf(frame)))) {
        while (--stackLevel > 0 && frame)
            frame = callerOf(frame);
    } else {
        while (--stackLevel > 0 && frame)
            frame = nextExternalFrame(globalObject, frame, prefixes);
    }

    JSObject* globals;
    String filename;
    int64_t line;
    if (frame) {
        globals = globalsOfFrame(globalObject, frame);
        filename = filenameOf(frame);
        line = PyFrame::forCallFrame(vm, frame)->line(vm);
    } else {
        globals = asObject(uncheckedDowncast<PyDict>(globalObject->pyRealm()->modules())->getString(globalObject, "sys"_s));
        filename = "<sys>"_s;
        line = 0;
    }

    PyDict* globalsDict = PyDict::backedBy(globalObject, globals);
    JSValue registry = globalsDict->getString(globalObject, "__warningregistry__"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (!registry) {
        registry = PyDict::create(globalObject);
        globalsDict->setString(globalObject, "__warningregistry__"_s, registry);
        RETURN_IF_EXCEPTION(scope, false);
    }
    JSValue module = getStoredAttribute(vm, globals, names.dunder_name);
    if (!module || (!isNone(module) && !isInstance(globalObject, module, globalObject->pyRealm()->typeStr())))
        module = jsString(vm, String("<string>"_s));
    RELEASE_AND_RETURN(scope, warnExplicit(globalObject, category, message, jsString(vm, filename), line, module, registry, { }, source));
}

// get_category()
static JSValue getCategory(JSGlobalObject* globalObject, JSValue message, JSValue category)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (isInstance(globalObject, message, realm->typeWarning()))
        category = typeOf(globalObject, message);
    else if (!category || isNone(category))
        category = realm->typeUserWarning();
    // Whatever goes wrong in finding out is taken for no.
    bool isWarning = [&] {
        auto inner = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        bool result = isSubclassOf(globalObject, category, realm->typeWarning());
        if (!inner.exception())
            return result;
        inner.clearException();
        return false;
    }();
    if (!isWarning)
        return raiseTypeError(globalObject, scope, makeString("category must be a Warning subclass, not '"_s, typeName(globalObject, category), '\''));
    return category;
}

// ---- For what is written in C++

static CallFrame* innermostFrame(VM& vm)
{
    CallFrame* top = vm.topCallFrame;
    if (!top)
        return nullptr;
    return isFrameToPython(top, top->bytecodeIndex()) ? top : callerOf(top);
}

// PyErr_WarnEx()
bool warn(JSGlobalObject* globalObject, BuiltinType category, const String& message, int64_t stackLevel, JSValue source)
{
    VM& vm = globalObject->vm();
    return doWarn(globalObject, innermostFrame(vm), jsString(vm, message), globalObject->pyRealm()->type(category), stackLevel, source, nullptr);
}

bool warnIfOfStrictSubclass(JSGlobalObject* globalObject, JSValue result, BuiltinType type, const String& before, ASCIILiteral className)
{
    if (typeOf(globalObject, result) == globalObject->pyRealm()->type(type)) [[likely]]
        return true;
    return warn(globalObject, BuiltinType::DeprecationWarning, makeString(before, " (type "_s, typeName(globalObject, result), ").  The ability to return an instance of a strict subclass of "_s, className,
        " is deprecated, and may be removed in a future version of Python."_s));
}

// PyErr_WarnExplicitObject()
bool warnExplicit(JSGlobalObject* globalObject, BuiltinType category, const String& message, const String& filename, int64_t line)
{
    VM& vm = globalObject->vm();
    return warnExplicit(globalObject, globalObject->pyRealm()->type(category), jsString(vm, message), jsString(vm, filename), line, { }, { }, { }, { });
}

// ---- The module

PYTHON_NATIVE(warningsWarn)
{
    NATIVE_PROLOGUE();
    JSValue message = args.at(0);
    int64_t stackLevel = 1;
    if (JSValue given = args.at(2)) {
        auto index = toSsize(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        stackLevel = *index;
    }
    JSValue prefixesValue = args.at(4);
    if (prefixesValue && !isTuple(prefixesValue))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("warn() argument 'skip_file_prefixes' must be tuple, not "_s, isNone(prefixesValue) ? String("None"_s) : typeName(globalObject, prefixesValue))));
    JSValue category = getCategory(globalObject, message, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* prefixes = prefixesValue && asTuple(prefixesValue)->length() ? asTuple(prefixesValue) : nullptr;
    if (prefixes)
        stackLevel = std::max<int64_t>(stackLevel, 2);
    scope.release();
    doWarn(globalObject, callerOf(callFrame), message, category, stackLevel, args.at(3), prefixes);
    RETURN_NONE();
}

// _bless_my_loader() of importlib._bootstrap_external: what loaded the module that these are the globals of. None if they do not say.
// FIXME: It has a DeprecationWarning for globals whose __spec__ does not say what their __loader__ does, which comes with importlib.
static JSValue loaderOf(JSGlobalObject* globalObject, PyDict* globals)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loader = globals->getString(globalObject, "__loader__"_s);
    RETURN_IF_EXCEPTION(scope, { });
    bool hasLoader = loader && !isNone(loader);
    JSValue spec = globals->getString(globalObject, "__spec__"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasLoader) {
        if (!spec)
            return jsUndefined();
        if (isNone(spec))
            return raiseValueError(globalObject, scope, "Module globals is missing a __spec__.loader"_s);
    }
    JSValue specLoader = spec ? getAttributeIfPresent(globalObject, spec, Identifier::fromString(vm, "loader"_s)) : JSValue();
    RETURN_IF_EXCEPTION(scope, { });
    if (!specLoader || isNone(specLoader)) {
        if (!hasLoader)
            return raise(globalObject, scope, specLoader ? BuiltinType::ValueError : BuiltinType::AttributeError, "Module globals is missing a __spec__.loader"_s);
        return loader;
    }
    return hasLoader ? loader : specLoader;
}

// get_source_line(). Empty if there is none, and if it raises.
static JSValue sourceLineFromLoader(JSGlobalObject* globalObject, PyDict* globals, int64_t line)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue loader = loaderOf(globalObject, globals);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue moduleName = globals->getString(globalObject, "__name__"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!moduleName)
        return { };
    // What goes wrong in looking for it is left as it is, for what asked to find.
    JSValue getSource = getAttributeIfPresent(globalObject, loader, Identifier::fromString(vm, "get_source"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!getSource)
        return { };
    JSValue source = call(globalObject, getSource, moduleName);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(source))
        return { };
    JSValue splitLines = getAttribute(globalObject, source, Identifier::fromString(vm, "splitlines"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue lines = call(globalObject, splitLines);
    RETURN_IF_EXCEPTION(scope, { });
    // PyList_GetItem(), which does not count from the end.
    if (!isList(lines) || line < 1 || line > asList(lines)->length())
        return raise(globalObject, scope, BuiltinType::IndexError, "list index out of range"_s);
    RELEASE_AND_RETURN(scope, listGet(globalObject, asList(lines), static_cast<unsigned>(line - 1)));
}

PYTHON_NATIVE(warningsWarnExplicit)
{
    NATIVE_PROLOGUE();
    JSValue filename = args.at(2);
    if (!isInstance(globalObject, filename, realm->typeStr()))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("warn_explicit() argument 'filename' must be str, not "_s, isNone(filename) ? String("None"_s) : typeName(globalObject, filename))));
    auto line = toCInt(globalObject, args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue givenSourceLine;
    if (JSValue globals = args.at(6); globals && !isNone(globals)) {
        if (!isDict(globals))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("module_globals must be a dict, not '"_s, typeName(globalObject, globals), '\'')));
        givenSourceLine = sourceLineFromLoader(globalObject, asDict(globals), *line);
        RETURN_IF_EXCEPTION(scope, { });
    }
    scope.release();
    warnExplicit(globalObject, args.at(1), args.at(0), filename, *line, args.at(4), args.at(5), givenSourceLine, args.at(7));
    RETURN_NONE();
}

PYTHON_NATIVE(warningsAcquireLock)
{
    ++stateOf(globalObject).lockDepth;
    RETURN_NONE();
}

PYTHON_NATIVE(warningsReleaseLock)
{
    NATIVE_PROLOGUE();
    if (!stateOf(globalObject).lockDepth)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot release un-acquired lock"_s));
    --stateOf(globalObject).lockDepth;
    RETURN_NONE();
}

PYTHON_NATIVE(warningsFiltersMutated)
{
    NATIVE_PROLOGUE();
    if (!stateOf(globalObject).lockDepth)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "warnings lock is not held"_s));
    ++stateOf(globalObject).filtersVersion;
    RETURN_NONE();
}

JSObject* createWarningsModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    WarningsState& state = stateOf(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "_warnings"_s);
    addFunction(globalObject, module, "warn"_s, warningsWarn);
    addFunction(globalObject, module, "warn_explicit"_s, warningsWarnExplicit);
    addFunction(globalObject, module, "_acquire_lock"_s, warningsAcquireLock);
    addFunction(globalObject, module, "_release_lock"_s, warningsReleaseLock);
    addFunction(globalObject, module, "_filters_mutated_lock_held"_s, warningsFiltersMutated);
    module->putDirect(vm, Identifier::fromString(vm, "filters"_s), state.filters.get());
    module->putDirect(vm, Identifier::fromString(vm, "_onceregistry"_s), state.onceRegistry.get());
    module->putDirect(vm, Identifier::fromString(vm, "_defaultaction"_s), state.defaultAction.get());
    module->putDirect(vm, Identifier::fromString(vm, "_warnings_context"_s), state.context.get());
    return module;
}

} } // namespace JSC::Python
