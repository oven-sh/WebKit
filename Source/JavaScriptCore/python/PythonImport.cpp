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
#include "PythonImport.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCompiler.h"
#include "PythonConfiguration.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "TopExceptionScope.h"

namespace JSC { namespace Python {

ImportState& importState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->importState(); }
static Identifier identifier(VM& vm, ASCIILiteral name) { return Identifier::fromString(vm, name); }

// ---- sys.modules

// It is the one that there was to begin with, whatever sys.modules is now.
static JSValue importedModules(JSGlobalObject* globalObject) { return globalObject->pyRealm()->modules(); }

// PyMapping_GetOptionalItem()
static JSValue getOptionalItem(JSGlobalObject* globalObject, JSValue mapping, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (typeOf(globalObject, mapping) == globalObject->pyRealm()->typeDict())
        RELEASE_AND_RETURN(scope, uncheckedDowncast<PyDict>(mapping.asCell())->get(globalObject, key));
    JSValue value = getItem(globalObject, mapping, key);
    if (scope.exception()) [[unlikely]] {
        catchException(globalObject, BuiltinType::KeyError);
        return { };
    }
    return value;
}

JSValue moduleIfImported(JSGlobalObject* globalObject, JSValue name)
{
    return getOptionalItem(globalObject, importedModules(globalObject), name);
}

std::optional<bool> isSpecInitializing(JSGlobalObject* globalObject, JSValue spec)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!spec)
        return false;
    JSValue value = getAttributeIfPresent(globalObject, spec, identifier(vm, "_initializing"_s));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!value)
        return false;
    bool result = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return result;
}

std::optional<bool> isUninitializedSubmodule(JSGlobalObject* globalObject, JSValue spec, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!spec)
        return false;
    JSValue value = getAttributeIfPresent(globalObject, spec, identifier(vm, "_uninitialized_submodules"_s));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!value)
        return false;
    bool result = contains(globalObject, value, name);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return result;
}

JSValue fileOriginOfSpec(JSGlobalObject* globalObject, JSValue spec)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hasLocation = getAttributeIfPresent(globalObject, spec, identifier(vm, "has_location"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasLocation)
        return { };
    bool isLocated = isTrue(globalObject, hasLocation);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isLocated)
        return { };
    JSValue origin = getAttributeIfPresent(globalObject, spec, identifier(vm, "origin"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return origin && stringIn(origin) ? origin : JSValue();
}

bool isPossiblyShadowing(JSGlobalObject* globalObject, JSValue origin)
{
    // root = os.path.dirname(origin.removesuffix(os.sep + "__init__.py"))
    // return not sys.flags.safe_path and root == (sys.path[0] or os.getcwd())
    if (!origin)
        return false;
    auto& configuration = globalObject->pyRealm()->configuration();
    String root = stringIn(origin)->value(globalObject);
    size_t separator = root.reverseFind('/');
    if (separator == notFound)
        return false;
    // If it is a package, it is the directory that the package is in.
    if (StringView(root).substring(separator + 1) == "__init__.py"_s) {
        root = root.left(separator);
        separator = root.reverseFind('/');
        if (separator == notFound)
            return false;
    }
    root = root.left(separator);
    if (configuration.firstSearchPath.isNull())
        return false;
    String first = configuration.firstSearchPath;
    if (first.isEmpty()) {
        auto* files = configuration.files;
        if (!files)
            return false;
        auto directory = files->currentDirectory();
        if (!directory)
            return false;
        first = String::fromUTF8(directory->span());
    }
    return first == root;
}

std::optional<bool> isShadowingStandardLibrary(JSGlobalObject* globalObject, JSValue moduleName)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue names = sysAttribute(globalObject, "stdlib_module_names"_s);
    if (!names || !isSet(names))
        return false;
    bool result = contains(globalObject, names, moduleName);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return result;
}

// import_ensure_initialized(). False if it raised.
static bool ensureIsInitialized(JSGlobalObject* globalObject, JSValue module, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // _lock_unlock_module() is only called if __spec__._initializing is true, which is why that is set before the module is put in sys.modules.
    JSValue spec = getAttributeIfPresent(globalObject, module, vm.pythonNames().dunder_spec);
    RETURN_IF_EXCEPTION(scope, false);
    auto isInitializing = isSpecInitializing(globalObject, spec);
    RETURN_IF_EXCEPTION(scope, false);
    if (!*isInitializing)
        return true;
    // Until it has been run to its end.
    callMethodNamed(globalObject, importState(globalObject).importlib.get(), identifier(vm, "_lock_unlock_module"_s), name);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

JSValue getImportedModule(JSGlobalObject* globalObject, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue module = moduleIfImported(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!module || isNone(module))
        return module;
    auto check = [&] () -> JSValue {
        if (!ensureIsInitialized(globalObject, module, name))
            return { };
        // It may have been taken out, as it is when running it fails.
        JSValue now = moduleIfImported(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        return now == module ? module : JSValue();
    };
    JSValue result = check();
    if (scope.exception()) [[unlikely]] {
        scope.release();
        removeImportlibFrames(globalObject);
        return { };
    }
    return result;
}

JSObject* addModule(JSGlobalObject* globalObject, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue modules = importedModules(globalObject);
    JSValue module = getOptionalItem(globalObject, modules, name);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (module) {
        if (JSObject* object = tryModule(globalObject, module))
            return object;
    }
    String text = stringIn(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* created = newModule(globalObject, text);
    setItem(globalObject, modules, name, created);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return created;
}

// remove_module(): out of sys.modules, if it is there. It is called with an exception raised, which is the one that is raised afterwards, unless this fails.
static void removeModule(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue modules = importedModules(globalObject);
    if (typeOf(globalObject, modules) == globalObject->pyRealm()->typeDict())
        uncheckedDowncast<PyDict>(modules.asCell())->remove(globalObject, name);
    else {
        deleteItem(globalObject, modules, name);
        if (scope.exception())
            catchException(globalObject, BuiltinType::KeyError);
    }
    scope.release();
    chainRaisedExceptions(globalObject, raised);
}

// ---- The modules that are written in C++

// `_PyImport_Inittab`. One that has nothing to make it with was made with the realm, and cannot be made again.
static constexpr BuiltinModule s_builtinModules[] = {
    { "_ast"_s, createASTModule },
    { "_codecs"_s, createCodecsModule },
    { "_collections"_s, createCollectionsModule },
    { "_contextvars"_s, createContextVarsModule },
    { "_frame"_s, createFrameModule },
    { "_imp"_s, createImpModule },
    { "_io"_s, createIOModule },
    { "_thread"_s, createThreadModule },
    { "_typing"_s, createTypingModule },
    { "_warnings"_s, createWarningsModule },
    { "_weakref"_s, createWeakrefModule },
    { "builtins"_s, nullptr },
    { "errno"_s, createErrnoModule },
    { "itertools"_s, createItertoolsModule },
    { "marshal"_s, createMarshalModule },
    { "math"_s, createMathModule },
    { "sys"_s, nullptr },
    { "time"_s, createTimeModule },
};

template<typename Function>
static void forEachBuiltinModule(JSGlobalObject* globalObject, const Function& function)
{
    for (auto& module : s_builtinModules) {
        if (function(module) == IterationStatus::Done)
            return;
    }
    for (auto& module : globalObject->pyRealm()->configuration().builtinModules) {
        if (function(module) == IterationStatus::Done)
            return;
    }
}

static const BuiltinModule* findBuiltinModule(JSGlobalObject* globalObject, const String& name)
{
    const BuiltinModule* found = nullptr;
    forEachBuiltinModule(globalObject, [&] (const BuiltinModule& module) {
        if (name != module.name)
            return IterationStatus::Continue;
        found = &module;
        return IterationStatus::Done;
    });
    return found;
}

int isBuiltinModule(JSGlobalObject* globalObject, const String& name)
{
    auto* module = findBuiltinModule(globalObject, name);
    return !module ? 0 : module->create ? 1 : -1;
}

JSValue createBuiltinModule(JSGlobalObject* globalObject, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = stringIn(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto* module = findBuiltinModule(globalObject, text);
    if (!module)
        return jsUndefined();
    if (!module->create)
        RELEASE_AND_RETURN(scope, addModule(globalObject, name));
    RELEASE_AND_RETURN(scope, module->create(globalObject));
}

JSValue builtinModuleNames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    Vector<String> names;
    forEachBuiltinModule(globalObject, [&] (const BuiltinModule& module) {
        names.append(module.name);
        return IterationStatus::Continue;
    });
    std::ranges::sort(names, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
    PyTuple* result = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i)
        result->initializeAt(vm, i, jsString(vm, names[i]));
    return result;
}

// ---- Running the code of a module

// module_dict_for_exec(): the namespace of the module of that name, which is made if there is none. Null if it raised.
static JSObject* namespaceForRunning(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // If the module is being loaded again, it is the same one, and what is in it is still there.
    JSObject* module = addModule(globalObject, name);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Identifier builtins = identifier(vm, "__builtins__"_s);
    if (!getStoredAttribute(vm, module, builtins))
        putStoredAttribute(vm, module, builtins, PyDict::backedBy(globalObject, globalObject->pyRealm()->builtinsModule()));
    return module;
}

// exec_code_in_module(). Empty if it raised.
static JSValue runCodeInModule(JSGlobalObject* globalObject, JSValue name, JSObject* namespaceObject, FunctionExecutable* code)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    call(globalObject, bindToGlobals(globalObject, code, namespaceObject));
    if (scope.exception()) [[unlikely]] {
        scope.release();
        removeModule(globalObject, name);
        return { };
    }
    JSValue module = moduleIfImported(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!module) {
        String shown = repr(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        return raise(globalObject, scope, BuiltinType::ImportError, concatenate("Loaded module "_s, shown, " not found in sys.modules"_s));
    }
    return module;
}

// ---- import

// resolve_name(): the whole name of what is named from where the importing is done. Null if it raised.
static String resolveName(JSGlobalObject* globalObject, const String& name, JSValue globals, int level)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    auto fail = [&] (BuiltinType type, ASCIILiteral message) {
        raise(globalObject, scope, type, message);
        return String();
    };
    auto noParent = [&] { return fail(BuiltinType::ImportError, "attempted relative import with no known parent package"_s); };
    if (!globals)
        return fail(BuiltinType::KeyError, "'__name__' not in globals"_s);
    if (!isDict(globals))
        return fail(BuiltinType::TypeError, "globals must be a dict"_s);
    auto* dict = uncheckedDowncast<PyDict>(globals.asCell());
    auto item = [&] (const Identifier& key) { return dict->get(globalObject, jsString(vm, key.string())); };

    JSValue package = item(names.dunder_package);
    RETURN_IF_EXCEPTION(scope, { });
    if (package && isNone(package))
        package = { };
    JSValue spec = item(names.dunder_spec);
    RETURN_IF_EXCEPTION(scope, { });
    bool hasSpec = spec && !isNone(spec);

    if (package) {
        if (!stringIn(package))
            return fail(BuiltinType::TypeError, "package must be a string"_s);
        if (hasSpec) {
            JSValue parent = getAttribute(globalObject, spec, identifier(vm, "parent"_s));
            RETURN_IF_EXCEPTION(scope, { });
            bool isSame = isEqual(globalObject, package, parent);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isSame && !warn(globalObject, BuiltinType::DeprecationWarning, "__package__ != __spec__.parent"_s))
                return { };
        }
    } else if (hasSpec) {
        package = getAttribute(globalObject, spec, identifier(vm, "parent"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!stringIn(package))
            return fail(BuiltinType::TypeError, "__spec__.parent must be a string"_s);
    } else {
        if (!warn(globalObject, BuiltinType::ImportWarning, "can't resolve package from __spec__ or __package__, falling back on __name__ and __path__"_s))
            return { };
        package = item(names.dunder_name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!package)
            return fail(BuiltinType::KeyError, "'__name__' not in globals"_s);
        if (!stringIn(package))
            return fail(BuiltinType::TypeError, "__name__ must be a string"_s);
        JSValue path = item(names.dunder_path);
        RETURN_IF_EXCEPTION(scope, { });
        if (!path) {
            String text = stringIn(package)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            size_t dot = text.reverseFind('.');
            if (dot == notFound)
                return noParent();
            package = jsString(vm, text.left(dot));
        }
    }

    String text = stringIn(package)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.isEmpty())
        return noParent();
    size_t lastDot = text.length();
    for (int levelUp = 1; levelUp < level; ++levelUp) {
        lastDot = lastDot ? text.reverseFind('.', lastDot - 1) : notFound;
        if (lastDot == notFound)
            return fail(BuiltinType::ImportError, "attempted relative import beyond top-level package"_s);
    }
    String base = text.left(lastDot);
    if (name.isEmpty())
        return base.isNull() ? emptyString() : base;
    return concatenate(base, '.', name);
}

// import_find_and_load(). Empty if it raised.
static JSValue findAndLoad(JSGlobalObject* globalObject, JSValue absoluteName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto orNone = [] (JSValue value) { return value ? value : jsUndefined(); };
    if (!audit(globalObject, "import"_s, absoluteName, jsUndefined(), orNone(sysAttribute(globalObject, "path"_s)), orNone(sysAttribute(globalObject, "meta_path"_s)), orNone(sysAttribute(globalObject, "path_hooks"_s))))
        return { };
    auto& state = importState(globalObject);
    RELEASE_AND_RETURN(scope, callMethodNamed(globalObject, state.importlib.get(), identifier(vm, "_find_and_load"_s), absoluteName, state.importFunction.get()));
}

static JSValue importModuleLevelWithFrames(JSGlobalObject* globalObject, JSValue name, JSValue globals, JSValue fromList, int level)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What follows is importlib.__import__() and _gcd_import(), which CPython has in C so that they are quick.
    if (!name)
        return raiseValueError(globalObject, scope, "Empty module name"_s);
    if (!stringIn(name))
        return raiseTypeError(globalObject, scope, "module name must be a string"_s);
    if (level < 0)
        return raiseValueError(globalObject, scope, "level must be >= 0"_s);
    String nameText = stringIn(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue absoluteName = name;
    String absoluteText = nameText;
    if (level > 0) {
        absoluteText = resolveName(globalObject, nameText, globals, level);
        RETURN_IF_EXCEPTION(scope, { });
        absoluteName = jsString(vm, absoluteText);
    } else if (nameText.isEmpty())
        return raiseValueError(globalObject, scope, "Empty module name"_s);

    JSValue module = moduleIfImported(globalObject, absoluteName);
    RETURN_IF_EXCEPTION(scope, { });
    bool isLoaded = false;
    if (module && !isNone(module)) {
        if (!ensureIsInitialized(globalObject, module, absoluteName))
            return { };
        // It may have been taken out meanwhile, because running it failed. It is tried again then, so that what is raised is what went wrong.
        JSValue now = moduleIfImported(globalObject, absoluteName);
        RETURN_IF_EXCEPTION(scope, { });
        isLoaded = now == module;
    }
    if (!isLoaded) {
        module = findAndLoad(globalObject, absoluteName);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool hasFrom = false;
    if (fromList && !isNone(fromList)) {
        hasFrom = isTrue(globalObject, fromList);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!hasFrom) {
        if (level && nameText.isEmpty())
            return module;
        size_t dot = nameText.find('.');
        if (dot == notFound)
            return module;
        // import a.b.c gives a.
        if (!level)
            RELEASE_AND_RETURN(scope, importModuleLevel(globalObject, jsString(vm, nameText.left(dot)), JSValue(), JSValue(), JSValue(), 0));
        JSValue toReturn = jsString(vm, absoluteText.left(absoluteText.length() - (nameText.length() - dot)));
        JSValue front = moduleIfImported(globalObject, toReturn);
        RETURN_IF_EXCEPTION(scope, { });
        if (!front) {
            String shown = repr(globalObject, toReturn);
            RETURN_IF_EXCEPTION(scope, { });
            return raise(globalObject, scope, BuiltinType::KeyError, concatenate(shown, " not in sys.modules as expected"_s));
        }
        return front;
    }
    JSValue path = getAttributeIfPresent(globalObject, module, vm.pythonNames().dunder_path);
    RETURN_IF_EXCEPTION(scope, { });
    if (!path)
        return module;
    auto& state = importState(globalObject);
    MarkedArgumentBuffer arguments;
    arguments.append(module);
    arguments.append(fromList);
    arguments.append(state.importFunction.get());
    JSValue handle = getAttribute(globalObject, state.importlib.get(), identifier(vm, "_handle_fromlist"_s));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, handle, arguments));
}

JSValue importModuleLevel(JSGlobalObject* globalObject, JSValue name, JSValue globals, JSValue, JSValue fromList, int level)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = importModuleLevelWithFrames(globalObject, name, globals, fromList, level);
    if (scope.exception()) [[unlikely]] {
        scope.release();
        removeImportlibFrames(globalObject);
        return { };
    }
    return result;
}

// PyImport_Import()
static JSValue importByStatement(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    Identifier importName = identifier(vm, "__import__"_s);
    // The __import__ of where this was called from, so that whatever a program has put in the way of importing is gone through.
    JSValue globals;
    JSValue import;
    // PyEval_GetGlobals(): those of the Python that is running, which is at the top unless it has called something that is written in C++.
    JSObject* namespaceObject = nullptr;
    if (CallFrame* top = vm.topCallFrame) {
        namespaceObject = globalsOfFrame(globalObject, top);
        if (!namespaceObject) {
            if (CallFrame* caller = callerOf(top))
                namespaceObject = globalsOfFrame(globalObject, caller);
        }
    }
    if (namespaceObject) {
        globals = PyDict::backedBy(globalObject, namespaceObject);
        JSValue builtins = getItem(globalObject, globals, jsNontrivialString(vm, "__builtins__"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (isDict(builtins)) {
            import = getItem(globalObject, builtins, jsString(vm, importName.string()));
            if (scope.exception()) [[unlikely]] {
                if (!scope.tryClearException())
                    return { };
                return raise(globalObject, scope, BuiltinType::KeyError, jsString(vm, importName.string()));
            }
        } else {
            import = getAttribute(globalObject, builtins, importName);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else {
        auto* dict = PyDict::create(globalObject);
        dict->setString(globalObject, "__builtins__"_s, realm->builtinsModule());
        globals = dict;
        import = getAttribute(globalObject, realm->builtinsModule(), importName);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // It is called for what it does. What is wanted is the module that was named, and what it returns is the package that that is in.
    MarkedArgumentBuffer arguments;
    arguments.append(name);
    arguments.append(globals);
    arguments.append(globals);
    arguments.append(newList(globalObject, MarkedArgumentBuffer()));
    arguments.append(jsNumber(0));
    call(globalObject, import, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue module = moduleIfImported(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!module)
        return raise(globalObject, scope, BuiltinType::KeyError, name);
    return module;
}

JSValue importModule(JSGlobalObject* globalObject, const String& name)
{
    return importByStatement(globalObject, jsString(globalObject->vm(), name));
}

JSValue importModuleAttribute(JSGlobalObject* globalObject, const String& moduleName, ASCIILiteral attribute)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, moduleName);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, module, identifier(vm, attribute)));
}

JSValue pathImporterFor(JSGlobalObject* globalObject, JSValue path)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto required = [&] (ASCIILiteral name) -> JSValue {
        JSValue value = sysAttribute(globalObject, name);
        if (!value)
            return raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("lost sys."_s, name));
        return value;
    };
    JSValue cache = required("path_importer_cache"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue hooks = required("path_hooks"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, hooks, realm->typeList()))
        return raise(globalObject, scope, BuiltinType::RuntimeError, "sys.path_hooks is not a list"_s);
    if (!isDict(cache))
        return raise(globalObject, scope, BuiltinType::RuntimeError, "sys.path_importer_cache is not a dict"_s);
    auto* dict = uncheckedDowncast<PyDict>(cache.asCell());
    JSValue importer = dict->get(globalObject, path);
    RETURN_IF_EXCEPTION(scope, { });
    if (importer)
        return importer;
    // So that asking does not come round to asking again.
    dict->set(globalObject, path, jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = asList(hooks);
    unsigned count = list->length();
    for (unsigned i = 0; i < count && i < list->length(); ++i) {
        importer = call(globalObject, list->getIndexQuickly(i), path);
        if (!scope.exception())
            break;
        importer = { };
        if (!catchException(globalObject, BuiltinType::ImportError))
            return { };
    }
    if (!importer)
        return jsUndefined();
    dict->set(globalObject, path, importer);
    RETURN_IF_EXCEPTION(scope, { });
    return importer;
}

// ---- The statement

JSValue importName(JSGlobalObject* globalObject, JSObject* builtins, JSValue globals, JSValue locals, JSValue name, JSValue fromList, JSValue level)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue import = getStoredAttribute(vm, builtins, identifier(vm, "__import__"_s));
    if (!import)
        return raise(globalObject, scope, BuiltinType::ImportError, "__import__ not found"_s);
    // If it is the one that there was to begin with, there is no need to call it.
    if (import == importState(globalObject).importFunction.get())
        RELEASE_AND_RETURN(scope, importModuleLevel(globalObject, name, globals, locals, fromList, level.asInt32()));
    MarkedArgumentBuffer arguments;
    arguments.append(name);
    arguments.append(globals);
    arguments.append(locals);
    arguments.append(fromList);
    arguments.append(level);
    RELEASE_AND_RETURN(scope, call(globalObject, import, arguments));
}

JSValue importFrom(JSGlobalObject* globalObject, JSValue module, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    auto attribute = asString(name)->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = getAttributeIfPresent(globalObject, module, attribute);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    // A module of the package that has not been made an attribute of it yet, as when the modules of a package import each other.
    JSValue moduleName = getAttributeIfPresent(globalObject, module, names.dunder_name);
    RETURN_IF_EXCEPTION(scope, { });
    if (moduleName && !stringIn(moduleName))
        moduleName = { };
    if (moduleName) {
        String package = stringIn(moduleName)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        value = getImportedModule(globalObject, jsString(vm, concatenate(package, '.', attribute.string())));
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }

    // The rest is all about what to say.
    JSValue nameOrUnknown = moduleName ? moduleName : JSValue(jsNontrivialString(vm, "<unknown module name>"_s));
    String shownName = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    String shownModule = repr(globalObject, nameOrUnknown);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue origin;
    String message;
    JSValue spec = getAttributeIfPresent(globalObject, module, names.dunder_spec);
    RETURN_IF_EXCEPTION(scope, { });
    if (!spec)
        message = concatenate("cannot import name "_s, shownName, " from "_s, shownModule, " (unknown location)"_s);
    else {
        origin = fileOriginOfSpec(globalObject, spec);
        RETURN_IF_EXCEPTION(scope, { });
        bool isShadowing = isPossiblyShadowing(globalObject, origin);
        bool isShadowingLibrary = false;
        if (isShadowing) {
            auto answer = isShadowingStandardLibrary(globalObject, nameOrUnknown);
            RETURN_IF_EXCEPTION(scope, { });
            isShadowingLibrary = *answer;
        }
        // For want of anything better, where the module says that it is from: PyModule_GetFilenameObject()
        if (!origin) {
            if (JSObject* object = tryModule(globalObject, module)) {
                origin = getStoredAttribute(vm, object, names.dunder_file);
                if (origin && !stringIn(origin))
                    origin = { };
            }
        }
        String shownOrigin;
        String originText;
        if (origin) {
            shownOrigin = repr(globalObject, origin);
            RETURN_IF_EXCEPTION(scope, { });
            originText = str(globalObject, origin);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (isShadowingLibrary)
            message = concatenate("cannot import name "_s, shownName, " from "_s, shownModule, " (consider renaming "_s, shownOrigin, " since it has the same name as the standard library module named "_s, shownModule, " and prevents importing that standard library module)"_s);
        else {
            auto isInitializing = isSpecInitializing(globalObject, spec);
            RETURN_IF_EXCEPTION(scope, { });
            if (*isInitializing) {
                // Of what is not the standard library's, it is only said that it may be in the way of something if it has not been run to its end.
                if (isShadowing)
                    message = concatenate("cannot import name "_s, shownName, " from "_s, shownModule, " (consider renaming "_s, shownOrigin, " if it has the same name as a library you intended to import)"_s);
                else if (origin)
                    message = concatenate("cannot import name "_s, shownName, " from partially initialized module "_s, shownModule, " (most likely due to a circular import) ("_s, originText, ')');
                else
                    message = concatenate("cannot import name "_s, shownName, " from partially initialized module "_s, shownModule, " (most likely due to a circular import)"_s);
            } else if (origin)
                message = concatenate("cannot import name "_s, shownName, " from "_s, shownModule, " ("_s, originText, ')');
            else
                message = concatenate("cannot import name "_s, shownName, " from "_s, shownModule, " (unknown location)"_s);
        }
    }

    // _PyErr_SetImportErrorWithNameFrom()
    JSValue text = strOrMemoryError(globalObject, message);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue error = call(globalObject, realm->typeImportError(), text);
    RETURN_IF_EXCEPTION(scope, { });
    asObject(error)->putDirect(vm, names.field_name, moduleName ? moduleName : jsUndefined());
    asObject(error)->putDirect(vm, names.field_path, origin ? origin : jsUndefined());
    asObject(error)->putDirect(vm, names.field_nameFrom, name);
    throwException(globalObject, scope, error);
    return { };
}

// PySequence_GetItem()
static JSValue sequenceItem(JSGlobalObject* globalObject, JSValue sequence, int64_t index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, sequence);
    if (isDict(sequence) || type == globalObject->pyRealm()->typeMappingProxy())
        return raiseTypeError(globalObject, scope, concatenate(typeName(globalObject, sequence), " is not a sequence"_s));
    if (!type->lookup(vm, vm.pythonNames().dunder_getitem))
        return raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, sequence), "' object does not support indexing"_s));
    RELEASE_AND_RETURN(scope, getItem(globalObject, sequence, intFromInt64(globalObject, index)));
}

void importAllFrom(JSGlobalObject* globalObject, JSValue locals, JSValue module)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    bool skipsLeadingUnderscores = false;
    JSValue all = getAttributeIfPresent(globalObject, module, names.dunder_all);
    RETURN_IF_EXCEPTION(scope, void());
    if (!all) {
        JSValue dict = getAttributeIfPresent(globalObject, module, names.dunder_dict);
        RETURN_IF_EXCEPTION(scope, void());
        if (!dict) {
            raise(globalObject, scope, BuiltinType::ImportError, "from-import-* object has no __dict__ and no __all__"_s);
            return;
        }
        // PyMapping_Keys()
        JSValue keys = callMethodNamed(globalObject, dict, identifier(vm, "keys"_s));
        RETURN_IF_EXCEPTION(scope, void());
        all = call(globalObject, globalObject->pyRealm()->typeList(), keys);
        RETURN_IF_EXCEPTION(scope, void());
        skipsLeadingUnderscores = true;
    }
    for (int64_t position = 0; ; ++position) {
        JSValue name = sequenceItem(globalObject, all, position);
        if (scope.exception()) [[unlikely]] {
            catchException(globalObject, BuiltinType::IndexError);
            return;
        }
        JSString* string = stringIn(name);
        if (!string) {
            JSValue moduleName = getAttribute(globalObject, module, names.dunder_name);
            RETURN_IF_EXCEPTION(scope, void());
            if (!stringIn(moduleName)) {
                raiseTypeError(globalObject, scope, concatenate("module __name__ must be a string, not "_s, typeName(globalObject, moduleName)));
                return;
            }
            String moduleText = stringIn(moduleName)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            raiseTypeError(globalObject, scope, concatenate(skipsLeadingUnderscores ? "Key"_s : "Item"_s, " in "_s, moduleText, '.', skipsLeadingUnderscores ? "__dict__"_s : "__all__"_s, " must be str, not "_s, typeName(globalObject, name)));
            return;
        }
        auto attribute = string->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (skipsLeadingUnderscores && attribute.string().startsWith('_'))
            continue;
        JSValue value = getAttribute(globalObject, module, attribute);
        RETURN_IF_EXCEPTION(scope, void());
        setItem(globalObject, locals, name, value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// ---- The modules that are written in Python and are not in files

#include "PythonLibrarySources.h"

// `_PyImport_FrozenBootstrap`: what importing is done with, which there is no doing without.
static constexpr FrozenModule s_frozenBootstrap[] = {
    { "_frozen_importlib"_s, s_librarySource__bootstrap, false, "importlib._bootstrap"_s },
    { "_frozen_importlib_external"_s, s_librarySource__bootstrap_external, false, "importlib._bootstrap_external"_s },
};

// `_PyImport_FrozenStdlib`
static constexpr FrozenModule s_frozenLibrary[] = {
    { "_framelocals"_s, s_librarySource__framelocals, false, { }, ImplementationVisibility::Private },
};

// use_frozen()
static bool usesFrozenModules(JSGlobalObject* globalObject)
{
    int override = importState(globalObject).overrideOfFrozenModules;
    return override ? override > 0 : globalObject->pyRealm()->configuration().usesFrozenModules;
}

JSValue frozenModuleNames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    Vector<ASCIILiteral> names;
    for (auto& module : s_frozenBootstrap)
        names.append(module.name);
    if (usesFrozenModules(globalObject)) {
        for (auto& module : s_frozenLibrary)
            names.append(module.name);
    }
    for (auto& module : globalObject->pyRealm()->configuration().frozenModules) {
        if (!names.containsIf([&] (ASCIILiteral name) { return equalSpans(name.span(), module.name.span()); }))
            names.append(module.name);
    }
    MarkedArgumentBuffer values;
    for (ASCIILiteral name : names)
        values.append(jsString(vm, String(name)));
    return newList(globalObject, values);
}

// look_up_frozen()
static const FrozenModule* lookUpFrozen(JSGlobalObject* globalObject, const String& name)
{
    for (auto& module : s_frozenBootstrap) {
        if (name == module.name)
            return &module;
    }
    // The host's come before the library's, so that it can put its own in place of one of those, or say that there is to be none.
    for (auto& module : globalObject->pyRealm()->configuration().frozenModules) {
        if (name == module.name)
            return &module;
    }
    if (usesFrozenModules(globalObject)) {
        for (auto& module : s_frozenLibrary) {
            if (name == module.name)
                return &module;
        }
    }
    return nullptr;
}

FrozenStatus findFrozen(JSGlobalObject* globalObject, JSValue name, FrozenInfo& info)
{
    info = { };
    if (!name || isNone(name))
        return FrozenStatus::BadName;
    String text = stringIn(name)->value(globalObject);
    if (text.contains(static_cast<char16_t>(0)))
        return FrozenStatus::BadName;
    const FrozenModule* module = lookUpFrozen(globalObject, text);
    if (!module)
        return FrozenStatus::NotFound;
    info.module = module;
    info.originalName = module->originalName.isNull() ? String(module->name) : String(module->originalName);
    if (!module->source.data())
        return FrozenStatus::Excluded;
    if (module->source.empty())
        return FrozenStatus::Invalid;
    return FrozenStatus::Okay;
}

void raiseFrozenError(JSGlobalObject* globalObject, FrozenStatus status, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, void());
    String message;
    switch (status) {
    case FrozenStatus::BadName:
    case FrozenStatus::NotFound:
        message = concatenate("No such frozen object named "_s, shown);
        break;
    case FrozenStatus::Disabled:
        message = concatenate("Frozen modules are disabled and the frozen object named "_s, shown, " is not essential"_s);
        break;
    case FrozenStatus::Excluded:
        message = concatenate("Excluded frozen object named "_s, shown);
        break;
    case FrozenStatus::Invalid:
        message = concatenate("Frozen object named "_s, shown, " is invalid"_s);
        break;
    case FrozenStatus::Okay:
        return;
    }
    // PyErr_SetImportError()
    JSValue error = call(globalObject, globalObject->pyRealm()->typeImportError(), jsString(vm, message));
    RETURN_IF_EXCEPTION(scope, void());
    asObject(error)->putDirect(vm, vm.pythonNames().field_name, name);
    throwException(globalObject, scope, error);
}

// What unmarshal_frozen_code() is, where it is the source that there is.
FunctionExecutable* compileFrozen(JSGlobalObject* globalObject, const FrozenInfo& info)
{
    SourceCode source = makeSource(String::fromUTF8(byteCast<char8_t>(info.module->source)), SourceOrigin(), concatenate("<frozen "_s, info.originalName, '>'));
    return compileSource(globalObject, source, CodeKind::Module, false, 0, info.module->visibility);
}

std::optional<bool> importFrozenModule(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    FrozenInfo info;
    FrozenStatus status = findFrozen(globalObject, name, info);
    if (status == FrozenStatus::NotFound || status == FrozenStatus::Disabled || status == FrozenStatus::BadName)
        return false;
    if (status != FrozenStatus::Okay) {
        scope.release();
        raiseFrozenError(globalObject, status, name);
        return std::nullopt;
    }
    FunctionExecutable* code = compileFrozen(globalObject, info);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (info.module->isPackage) {
        JSObject* module = addModule(globalObject, name);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        putStoredAttribute(vm, module, vm.pythonNames().dunder_path, newList(globalObject, MarkedArgumentBuffer()));
    }
    JSObject* namespaceObject = namespaceForRunning(globalObject, name);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    runCodeInModule(globalObject, name, namespaceObject, code);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    // For FrozenImporter._setup_module()
    putStoredAttribute(vm, namespaceObject, identifier(vm, "__origname__"_s), jsString(vm, info.originalName));
    return true;
}

// ---- When a realm is made

// What cannot go wrong has, and there is no going on.
[[noreturn]] static void failToStart(JSGlobalObject* globalObject, ASCIILiteral what)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    dataLogLn("Fatal Python error: ", what);
    if (Exception* exception = scope.exception()) {
        JSValue value = exception->value();
        scope.clearException();
        reportUncaughtException(globalObject, value);
    }
    CRASH();
}

void initializeImport(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = importState(globalObject);
    // _PyImport_InitDefaultImportFunc()
    state.importFunction.set(vm, realm, getStoredAttribute(vm, realm->builtinsModule(), identifier(vm, "__import__"_s)));

    // init_importlib()
    JSValue name = jsNontrivialString(vm, "_frozen_importlib"_s);
    auto wasFound = importFrozenModule(globalObject, name);
    if (scope.exception() || !*wasFound)
        failToStart(globalObject, "failed to initialize importlib"_s);
    JSObject* importlib = addModule(globalObject, name);
    state.importlib.set(vm, realm, importlib);
    // importlib needs _imp, and importing anything needs importlib. So this one is made by hand: bootstrap_imp()
    JSObject* imp = createImpModule(globalObject);
    registerModule(globalObject, "_imp"_s, imp);
    callMethodNamed(globalObject, importlib, identifier(vm, "_install"_s), moduleIfImported(globalObject, jsNontrivialString(vm, "sys"_s)), imp);
    if (scope.exception())
        failToStart(globalObject, "failed to initialize importlib"_s);
}

void initializeExternalImport(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // init_importlib_external()
    callMethodNamed(globalObject, importState(globalObject).importlib.get(), identifier(vm, "_install_external_importers"_s));
    RETURN_IF_EXCEPTION(scope, void());

    // init_zipimport()
    JSValue hooks = sysAttribute(globalObject, "path_hooks"_s);
    if (!hooks) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.path_hooks"_s);
        return;
    }
    JSValue importer = importModuleAttribute(globalObject, "zipimport"_s, "zipimporter"_s);
    if (scope.exception()) {
        // It can be done without.
        scope.tryClearException();
        return;
    }
    callMethodNamed(globalObject, hooks, identifier(vm, "insert"_s), jsNumber(0), importer);
}


} } // namespace JSC::Python
