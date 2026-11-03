/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "JSWeakMapInlines.h"
#include "PyAbstract.h"
#include "PyCodeCache.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyExec.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyVirtualFiles.h"
#include "TopExceptionScope.h"
#include <wtf/FileSystem.h>

namespace JSC {
namespace Python {

// The import statement. (Python/import.c and Lib/importlib/_bootstrap.py in CPython are the reference.)

// ---- Modules written in C++ ----

static std::span<const NativeModuleDef> s_hostNativeModules;

void setHostNativeModules(std::span<const NativeModuleDef> modules)
{
    s_hostNativeModules = modules;
}

std::span<const NativeModuleDef> hostNativeModules()
{
    return s_hostNativeModules;
}

const NativeModuleDef* findNativeModule(StringView name)
{
    for (auto& def : nativeModules()) {
        if (name == def.name)
            return &def;
    }
    for (auto& def : s_hostNativeModules) {
        if (name == def.name)
            return &def;
    }
    return nullptr;
}

// ---- Names ----

static JSValue raiseModuleNotFound(JSGlobalObject* globalObject, const String& message, JSString* name, bool fromNone = false)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = newException(globalObject, rt.ModuleNotFoundError(), message);
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::importErrorName, name);
    exception->setSuppressContext(fromNone);
    return raiseException(globalObject, scope, exception);
}

// The absolute name of what `from ...name import x` names, given the globals of the importing code. Null: an exception.
static JSString* resolveRelativeName(JSGlobalObject* globalObject, JSString* name, JSValue globalsValue, int level)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!globalsValue || isNone(globalsValue)) {
        raiseKeyError(globalObject, scope, makeStr(vm, "'__name__' not in globals"_s));
        return nullptr;
    }
    auto* globals = PyDict::tryFrom(globalsValue);
    if (!globals) {
        raiseTypeError(globalObject, scope, "globals must be a dict or a frozendict"_s);
        return nullptr;
    }

    JSValue package = globals->get(rt.names.dPackage());
    JSValue spec = globals->get(rt.names.dSpec());
    if (package && !isNone(package)) {
        if (!isStr(package)) {
            raiseTypeError(globalObject, scope, "package must be a string"_s);
            return nullptr;
        }
        if (spec && !isNone(spec)) {
            JSValue parent = getAttr(globalObject, spec, internString(vm, "parent"_s));
            RETURN_IF_EXCEPTION(scope, nullptr);
            bool isSame = richCompareBool(globalObject, package, parent, CompareOperator::Eq);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!isSame) {
                warn(globalObject, rt.DeprecationWarning(), "__package__ != __spec__.parent"_s);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
    } else if (spec && !isNone(spec)) {
        package = getAttr(globalObject, spec, internString(vm, "parent"_s));
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isStr(package)) {
            raiseTypeError(globalObject, scope, "__spec__.parent must be a string"_s);
            return nullptr;
        }
    } else {
        warn(globalObject, rt.ImportWarning(), "can't resolve package from __spec__ or __package__, falling back on __name__ and __path__"_s);
        RETURN_IF_EXCEPTION(scope, nullptr);
        package = globals->get(rt.names.dName());
        if (!package) {
            raiseKeyError(globalObject, scope, makeStr(vm, "'__name__' not in globals"_s));
            return nullptr;
        }
        if (!isStr(package)) {
            raiseTypeError(globalObject, scope, "__name__ must be a string"_s);
            return nullptr;
        }
        if (!globals->get(rt.names.dPath())) {
            String text = strValue(package)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            size_t dot = text.reverseFind('.');
            if (dot == notFound) {
                raiseImportError(globalObject, scope, "attempted relative import with no known parent package"_s);
                return nullptr;
            }
            package = makeStr(vm, text.left(dot));
        }
    }

    String base = strValue(package)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (base.isEmpty()) {
        raiseImportError(globalObject, scope, "attempted relative import with no known parent package"_s);
        return nullptr;
    }
    for (int i = 1; i < level; ++i) {
        size_t dot = base.reverseFind('.');
        if (dot == notFound) {
            raiseImportError(globalObject, scope, "attempted relative import beyond top-level package"_s);
            return nullptr;
        }
        base = base.left(dot);
    }
    String nameText = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (nameText.isEmpty())
        return internString(vm, base);
    return internString(vm, makeString(base, '.', nameText));
}

// ---- Finding and loading ----

// There is one of each, however often it is taken out of sys.modules and imported again: its types are made once.
JSValue nativeModuleInstance(JSGlobalObject* globalObject, JSString* name, const NativeModuleDef& def)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue instances = rt.slot(Runtime::Slot::NativeModules);
    if (!instances) {
        instances = PyDict::create(globalObject);
        rt.setSlot(Runtime::Slot::NativeModules, instances);
    }
    if (JSValue existing = PyDict::from(instances)->get(name))
        return existing;
    PyModule* module = PyModule::create(globalObject, name);
    module->dict()->set(vm, rt.names.dPackage(), rt.names.empty());
    PyDict::from(instances)->set(vm, name, module);
    def.initialize(globalObject, module);
    if (scope.exception()) {
        PyDict::from(instances)->remove(vm, name);
        return { };
    }
    return module;
}

static JSValue loadNativeModule(JSGlobalObject* globalObject, JSString* name, const NativeModuleDef& def)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = nativeModuleInstance(globalObject, name, def);
    RETURN_IF_EXCEPTION(scope, { });
    rt.modules()->set(vm, name, module);
    if (rt.modules()->get(internString(vm, "_frozen_importlib"_s))) {
        ensureNativeModuleSpec(globalObject, PyModule::from(module));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return module;
}

static JSValue loadSourceModule(JSGlobalObject* globalObject, JSString* name, const String& path, const String& packageDirectory)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto contents = readFile(path);
    if (!contents)
        return raiseOSError(globalObject, scope, errno, makeStr(vm, path));

    String nameText = stringOf(name);
    PyModule* module = PyModule::create(globalObject, name);
    PyDict* dict = module->dict();
    dict->set(vm, rt.names.dFile(), makeStr(vm, path));
    if (!packageDirectory.isNull()) {
        JSValue directory = makeStr(vm, packageDirectory);
        JSArray* searchPath = newList(globalObject, std::span<const JSValue> { &directory, 1 });
        RETURN_IF_EXCEPTION(scope, { });
        dict->set(vm, rt.names.dPath(), searchPath);
        dict->set(vm, rt.names.dPackage(), name);
    } else {
        size_t dot = nameText.reverseFind('.');
        dict->set(vm, rt.names.dPackage(), dot == notFound ? rt.names.empty() : makeStr(vm, nameText.left(dot)));
    }

    JSValue code = compileSource(globalObject, contents->span(), path, "exec"_s, 0, -1);
    RETURN_IF_EXCEPTION(scope, { });

    rt.modules()->set(vm, name, module);
    runCode(globalObject, code, dict, dict);
    if (scope.exception()) {
        rt.modules()->remove(vm, name);
        return { };
    }
    // A module may have put something else in its place.
    JSValue loaded = rt.modules()->get(name);
    if (!loaded)
        return raiseKeyError(globalObject, scope, name);
    return loaded;
}

// Looks in the directories for `tail`, the last part of the name. Empty with no exception: it is in none.
static JSValue findAndLoadOnPath(JSGlobalObject* globalObject, JSString* name, const String& tail, JSValue searchPath)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* directories = listFromIterable(globalObject, searchPath);
    RETURN_IF_EXCEPTION(scope, { });
    String namespaceDirectory;
    for (unsigned i = 0; i < listLength(directories); ++i) {
        JSValue entry = listGet(directories, i);
        if (!isStr(entry))
            continue;
        String directory = strValue(entry)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (directory.isEmpty())
            directory = "."_s;
        String base = FileSystem::pathByAppendingComponent(directory, tail);
        if (kindOfFile(base) == VirtualFileKind::Directory) {
            String init = FileSystem::pathByAppendingComponent(base, "__init__.py"_s);
            if (kindOfFile(init) == VirtualFileKind::File)
                RELEASE_AND_RETURN(scope, loadSourceModule(globalObject, name, init, base));
            if (namespaceDirectory.isNull())
                namespaceDirectory = base;
        }
        String file = makeString(base, ".py"_s);
        if (kindOfFile(file) == VirtualFileKind::File)
            RELEASE_AND_RETURN(scope, loadSourceModule(globalObject, name, file, { }));
    }
    if (namespaceDirectory.isNull())
        return { };

    // A directory without __init__.py is a package all the same.
    Runtime& rt = runtime(globalObject);
    PyModule* module = PyModule::create(globalObject, name);
    JSValue directory = makeStr(vm, namespaceDirectory);
    JSArray* path = newList(globalObject, std::span<const JSValue> { &directory, 1 });
    RETURN_IF_EXCEPTION(scope, { });
    module->dict()->set(vm, rt.names.dPath(), path);
    module->dict()->set(vm, rt.names.dPackage(), name);
    rt.modules()->set(vm, name, module);
    return module;
}

static JSValue importModuleOrNone(JSGlobalObject*, JSString* name, bool missingIsNone);

// ---- importlib ----
//
// The import system is importlib, unchanged. CPython has importlib._bootstrap and importlib._bootstrap_external compiled in
// ("frozen"), under other names; here what is above loads them from the standard library when the interpreter starts, and
// from then on only serves to find them. Without a standard library it remains the import system.

// Empty without an exception: there is no such file.
static JSValue loadFrozenModule(JSGlobalObject* globalObject, ASCIILiteral nameText, ASCIILiteral fileName, ASCIILiteral originalName)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = internString(vm, nameText);
    JSValue searchPath = rt.sysDict()->get(rt.names.path());
    if (!searchPath)
        return { };
    JSArray* directories = listFromIterable(globalObject, searchPath);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < listLength(directories); ++i) {
        JSValue entry = listGet(directories, i);
        if (!isStr(entry))
            continue;
        String directory = strValue(entry)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        // Tracebacks and warnings know the frames of the import system by the name that is shown.
        JSValue code = codeOfSourceFile(globalObject, FileSystem::pathByAppendingComponent(FileSystem::pathByAppendingComponent(directory, "importlib"_s), fileName), makeString("<frozen "_s, originalName, '>'));
        RETURN_IF_EXCEPTION(scope, { });
        if (!code)
            continue;
        PyModule* module = PyModule::create(globalObject, name);
        module->dict()->set(vm, rt.names.dPackage(), rt.names.empty());
        // For FrozenImporter._fix_up_module(), which makes the __spec__ of a frozen module that was loaded without it.
        module->dict()->set(vm, internString(vm, "__origname__"_s), makeStr(vm, originalName));
        rt.modules()->set(vm, name, module);
        runCode(globalObject, code, module->dict(), module->dict());
        if (scope.exception()) {
            rt.modules()->remove(vm, name);
            return { };
        }
        return module;
    }
    return { };
}

static JSValue loadImportlibBootstrap(JSGlobalObject* globalObject, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue imp = importModule(globalObject, "_imp"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue module = loadFrozenModule(globalObject, "_frozen_importlib"_s, "_bootstrap.py"_s, "importlib._bootstrap"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!module)
        return { };
    callMethod(globalObject, module, internString(vm, "_install"_s), rt.sysModule(), imp);
    if (scope.exception()) {
        rt.modules()->remove(vm, name);
        return { };
    }
    return module;
}

static void installImportlib(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue bootstrap = importModuleOrNone(globalObject, internString(vm, "_frozen_importlib"_s), true);
    RETURN_IF_EXCEPTION(scope, void());
    if (isNone(bootstrap))
        return;
    // From here on importlib does the importing: first of its other half, which is a frozen module (PyImp.cpp).
    JSValue findAndLoad = getAttr(globalObject, bootstrap, internString(vm, "_find_and_load"_s));
    RETURN_IF_EXCEPTION(scope, void());
    rt.setSlot(Runtime::Slot::FindAndLoad, findAndLoad);
    callMethod(globalObject, bootstrap, internString(vm, "_install_external_importers"_s));
    if (scope.exception()) [[unlikely]]
        rt.setSlot(Runtime::Slot::FindAndLoad, JSValue());
}

static ASCIILiteral s_importSystemModule;

void setImportSystemModule(ASCIILiteral name)
{
    s_importSystemModule = name;
}

JSValue importSystemFunction(JSGlobalObject* globalObject, ASCIILiteral name)
{
    if (s_importSystemModule.isNull())
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, s_importSystemModule);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttrIfPresent(globalObject, module, internString(vm, name)));
}

void startImportSystem(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    installImportlib(globalObject);
    bool isInstalled = !!rt.slot(Runtime::Slot::FindAndLoad);
    if (!scope.exception() && isInstalled && !s_importSystemModule.isNull())
        importModule(globalObject, s_importSystemModule);
    if (!scope.exception() && isInstalled)
        startMainInterpreter(globalObject);
    if (scope.exception()) [[unlikely]]
        printException(globalObject, takePendingException(globalObject));
}

// An exception that leaves the import system does not show its frames: all of them if it is an ImportError, else those
// up to a call of _call_with_frames_removed(), which is how importlib marks where the code of the module starts.
static void removeImportlibFrames(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* exception = PyException::tryFrom(pendingException(globalObject));
    if (!exception)
        return;
    bool alwaysTrims = exception->type()->isSubtypeOf(rt.ImportError());
    bool isInImportlib = false;
    // A link is the traceback whose `next` it is, or null for the exception's own.
    PyTraceback* previous = nullptr;
    PyTraceback* outer = nullptr;
    auto setLink = [&](PyTraceback* link, JSValue target) {
        if (link)
            link->setNext(vm, target);
        else
            exception->setTraceback(vm, target);
    };
    for (JSValue value = exception->traceback(); value;) {
        auto* traceback = PyTraceback::from(value);
        JSValue next = traceback->next();
        CodeInfo& info = traceback->frame()->function()->codeInfo();
        bool isNowInImportlib = info.filename == "<frozen importlib._bootstrap>"_s || info.filename == "<frozen importlib._bootstrap_external>"_s;
        if (isNowInImportlib && !isInImportlib)
            outer = previous;
        isInImportlib = isNowInImportlib;
        if (isInImportlib && (alwaysTrims || info.name.string() == "_call_with_frames_removed"_s)) {
            setLink(outer, next);
            previous = outer;
        } else
            previous = traceback;
        value = next;
    }
}

bool ensureNativeModuleSpec(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = module->dict();
    JSValue name = dict->get(rt.names.dName());
    if (!name || !name.isString())
        return true;
    if (module != rt.sysModule() && module != rt.builtinsModule()) {
        JSValue instances = rt.slot(Runtime::Slot::NativeModules);
        if (!instances || PyDict::from(instances)->get(asString(name)) != module)
            return true;
    }
    // Loading it gives one to every module there is. Without the standard library they go without.
    JSValue bootstrap = importModuleOrNone(globalObject, internString(vm, "_frozen_importlib"_s), true);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSValue spec = dict->get(rt.names.dSpec()); spec && !isNone(spec))
        return true;
    // It is given _imp once it has been loaded. Until then it cannot be asked, and then it sees to the modules there are.
    auto* bootstrapModule = PyModule::tryFrom(bootstrap);
    if (!bootstrapModule || !bootstrapModule->dict()->get(internString(vm, "_imp"_s)))
        return true;
    JSValue importer = getAttr(globalObject, bootstrap, internString(vm, "BuiltinImporter"_s));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue spec = callMethod(globalObject, importer, internString(vm, "find_spec"_s), name);
    RETURN_IF_EXCEPTION(scope, false);
    dict->set(vm, rt.names.dSpec(), spec);
    if (JSValue loader = dict->get(rt.names.dLoader()); !loader || isNone(loader))
        dict->set(vm, rt.names.dLoader(), importer);
    return true;
}

static void clearLazySubmodule(JSGlobalObject*, JSString* name, bool bind);

// The module is in sys.modules, where it is put before its code runs. If another thread is running that, this one waits.
static bool waitUntilModuleIsInitialized(JSGlobalObject* globalObject, JSValue module, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue spec = getAttrIfPresent(globalObject, module, rt.names.dSpec());
    RETURN_IF_EXCEPTION(scope, false);
    if (!spec || isNone(spec))
        return true;
    JSValue isInitializing = getAttrIfPresent(globalObject, spec, internString(vm, "_initializing"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!isInitializing)
        return true;
    bool waits = isTrue(globalObject, isInitializing);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue bootstrap = rt.modules()->get(internString(vm, "_frozen_importlib"_s));
    if (!waits || !bootstrap)
        return true;
    scope.release();
    return !!callMethod(globalObject, bootstrap, internString(vm, "_lock_unlock_module"_s), name);
}

// With `missingIsNone`, the answer for a module that is nowhere to be found is None.
static JSValue importModuleOrNone(JSGlobalObject* globalObject, JSString* name, bool missingIsNone)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    name = internString(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });

    if (JSValue module = rt.modules()->get(name)) {
        if (isNone(module))
            RELEASE_AND_RETURN(scope, raiseModuleNotFound(globalObject, makeString("import of "_s, stringOf(name), " halted; None in sys.modules"_s), name));
        bool isStillThere = true;
        if (rt.threads().isEnabled()) [[unlikely]] {
            bool isInitialized = waitUntilModuleIsInitialized(globalObject, module, name);
            EXCEPTION_ASSERT(!scope.exception() == isInitialized);
            if (!isInitialized)
                return { };
            // The import that another thread was busy with may have failed, which takes the module out again. Then this is
            // an import like any other, and fails, if it does, for reasons of its own.
            isStillThere = rt.modules()->get(name) == module;
        }
        if (isStillThere) [[likely]] {
            ImportTimeReport::wasCached(stringOf(name));
            return module;
        }
    }

    String text = stringOf(name);
    if (JSValue findAndLoad = rt.slot(Runtime::Slot::FindAndLoad)) {
        if (isAudited(globalObject)) [[unlikely]] {
            auto fromSys = [&](ASCIILiteral attribute) { return rt.sysDict()->get(internString(vm, attribute)); };
            if (!audit(globalObject, "import"_s, name, jsUndefined(), fromSys("path"_s), fromSys("meta_path"_s), fromSys("path_hooks"_s)))
                return { };
        }
        ImportTimeReport report(text);
        JSValue module = call(globalObject, findAndLoad, name, rt.defaultImportFunction());
        if (scope.exception()) [[unlikely]] {
            removeImportlibFrames(globalObject);
            if (!missingIsNone || !pendingExceptionMatches(globalObject, rt.ModuleNotFoundError()))
                return { };
            JSValue missing = PyException::from(pendingException(globalObject))->field(ExceptionField::importErrorName);
            if (!missing || !isStr(missing) || !WTF::equal(strValue(missing)->tryGetValueImpl(), name->tryGetValueImpl()))
                return { };
            takePendingException(globalObject);
            return jsUndefined();
        }
        didImportLazyModule(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        return module;
    }

    size_t dot = text.reverseFind('.');
    JSValue searchPath;
    JSValue parentModule;
    String tail = text;
    if (dot != notFound) {
        JSString* parent = internString(vm, text.left(dot));
        tail = text.substring(dot + 1);
        importModule(globalObject, parent);
        RETURN_IF_EXCEPTION(scope, { });
        // Importing the parent may have imported this.
        if (JSValue module = rt.modules()->get(name); module && !isNone(module))
            return module;
        parentModule = rt.modules()->get(parent);
        if (!parentModule)
            return raiseKeyError(globalObject, scope, parent);
        searchPath = getAttrIfPresent(globalObject, parentModule, rt.names.dPath());
        RETURN_IF_EXCEPTION(scope, { });
        if (!searchPath) {
            if (missingIsNone)
                return jsUndefined();
            RELEASE_AND_RETURN(scope, raiseModuleNotFound(globalObject, makeString("No module named "_s, strRepr(text), "; "_s, strRepr(stringOf(parent)), " is not a package"_s), name, true)); // importlib raises it `from None`.
        }
    }

    JSValue module;
    if (!parentModule) {
        if (auto* def = findNativeModule(text)) {
            module = loadNativeModule(globalObject, name, *def);
            RETURN_IF_EXCEPTION(scope, { });
        } else if (text == "_frozen_importlib"_s) {
            module = loadImportlibBootstrap(globalObject, name);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            searchPath = rt.sysDict()->get(rt.names.path());
    }
    if (!module && searchPath) {
        module = findAndLoadOnPath(globalObject, name, tail, searchPath);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!module) {
        if (missingIsNone)
            return jsUndefined();
        RELEASE_AND_RETURN(scope, raiseModuleNotFound(globalObject, makeString("No module named "_s, strRepr(text)), name));
    }

    if (parentModule) {
        setAttr(globalObject, parentModule, internString(vm, tail), module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    didImportLazyModule(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return module;
}

JSValue importModule(JSGlobalObject* globalObject, JSString* name)
{
    return importModuleOrNone(globalObject, name, false);
}

JSValue importModule(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return importModule(globalObject, internString(getVM(globalObject), name));
}

// from module import a, b: a name that is not an attribute of a package may be a submodule yet to be imported.
static void handleFromList(JSGlobalObject* globalObject, JSValue module, JSValue fromList, bool isAll)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue moduleName = getAttr(globalObject, module, rt.names.dName());
    RETURN_IF_EXCEPTION(scope, void());
    String prefix = isStr(moduleName) ? strValue(moduleName)->value(globalObject) : String();
    RETURN_IF_EXCEPTION(scope, void());

    forEach(globalObject, fromList, [&](JSValue item) {
        if (!isStr(item)) {
            raiseTypeError(globalObject, scope, "Item in "_s, isAll ? makeString(prefix, ".__all__"_s) : "``from list''"_str, " must be str, not "_s, typeName(globalObject, item));
            return false;
        }
        JSString* name = internString(globalObject, strValue(item));
        RETURN_IF_EXCEPTION(scope, false);
        if (stringOf(name) == "*"_s) {
            if (isAll)
                return true;
            JSValue all = getAttrIfPresent(globalObject, module, rt.names.dAll());
            RETURN_IF_EXCEPTION(scope, false);
            if (all)
                handleFromList(globalObject, module, all, true);
            return !scope.exception();
        }
        bool has = hasAttr(globalObject, module, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (has)
            return true;
        JSString* fullName = internString(vm, makeString(prefix, '.', stringOf(name)));
        importModule(globalObject, fullName);
        if (!scope.exception())
            return true;
        // That there is no such submodule is not an error yet: the `from` will say so.
        if (!pendingExceptionMatches(globalObject, rt.ModuleNotFoundError()))
            return false;
        JSValue missing = PyException::from(pendingException(globalObject))->field(ExceptionField::importErrorName);
        if (!missing || !isStr(missing) || !WTF::equal(strValue(missing)->tryGetValueImpl(), fullName->tryGetValueImpl()))
            return false;
        // None in sys.modules: it is not to be imported, which is something else.
        if (JSValue blocked = rt.modules()->get(fullName); blocked && isNone(blocked))
            return false;
        takePendingException(globalObject);
        return true;
    });
}

// Null: an exception.
static JSString* absoluteModuleName(JSGlobalObject* globalObject, JSString* name, JSValue globals, int level)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (level > 0)
        RELEASE_AND_RETURN(scope, resolveRelativeName(globalObject, name, globals, level));
    if (!name->length()) {
        raiseValueError(globalObject, scope, "Empty module name"_s);
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, internString(globalObject, name));
}

JSValue importName(JSGlobalObject* globalObject, JSString* name, JSValue globals, JSValue, JSValue fromList, int level)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (level < 0)
        return raiseValueError(globalObject, scope, "level must be >= 0"_s);
    String nameText = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    JSString* absoluteName = absoluteModuleName(globalObject, name, globals, level);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue cached = rt.modules()->get(absoluteName);
    JSValue module = importModule(globalObject, absoluteName);
    RETURN_IF_EXCEPTION(scope, { });
    if (cached == module) {
        clearLazySubmodule(globalObject, absoluteName, true);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool hasFromList = fromList && !isNone(fromList) && isTrue(globalObject, fromList);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasFromList) {
        JSValue path = getAttrIfPresent(globalObject, module, rt.names.dPath());
        RETURN_IF_EXCEPTION(scope, { });
        if (path) {
            handleFromList(globalObject, module, fromList, false);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return module;
    }

    // import a.b.c binds a.
    if (level && nameText.isEmpty())
        return module;
    size_t dot = nameText.find('.');
    if (dot == notFound)
        return module;
    if (!level)
        RELEASE_AND_RETURN(scope, importModule(globalObject, internString(vm, nameText.left(dot))));
    String absoluteText = absoluteName->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* front = internString(vm, absoluteText.left(absoluteText.length() - (nameText.length() - dot)));
    if (JSValue result = rt.modules()->get(front))
        return result;
    return raiseKeyError(globalObject, scope, makeStr(vm, makeString(strRepr(stringOf(front)), " not in sys.modules as expected"_s)));
}

static JSValue lazyImportFrom(JSGlobalObject*, PyRecord*, JSString* name);

// from module import name
JSValue importFrom(JSGlobalObject* globalObject, JSValue module, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyRecord* lazy = asLazyImport(globalObject, module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, lazyImportFrom(globalObject, lazy, name));
    JSValue value = getAttrIfPresent(globalObject, module, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;

    // In a circular import the submodule may be in sys.modules and not yet an attribute of the package.
    JSValue moduleName = getAttrIfPresent(globalObject, module, rt.names.dName());
    RETURN_IF_EXCEPTION(scope, { });
    String moduleText;
    if (moduleName && isStr(moduleName)) {
        moduleText = strValue(moduleName)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (JSValue submodule = rt.modules()->get(internString(vm, makeString(moduleText, '.', stringOf(name)))))
            return submodule;
    }

    if (moduleText.isNull())
        moduleName = { };
    String shownName = strRepr(moduleText.isNull() ? "<unknown module name>"_str : moduleText);
    String start = makeString("cannot import name "_s, strRepr(stringOf(name)), " from "_s);
    JSValue spec = getAttrIfPresent(globalObject, module, rt.names.dSpec());
    RETURN_IF_EXCEPTION(scope, { });
    ModuleDiagnosis diagnosis;
    String message;
    if (!spec)
        message = makeString(start, shownName, " (unknown location)"_s);
    else {
        if (!diagnoseModule(globalObject, spec, moduleName ? moduleName : JSValue(makeStr(vm, "<unknown module name>"_s)), diagnosis))
            return { };
        if (auto* object = PyModule::tryFrom(module); object && !diagnosis.origin) {
            if (JSValue file = object->dict()->get(rt.names.dFile()); file && isStr(file))
                diagnosis.origin = file;
        }
        String origin;
        if (diagnosis.origin) {
            origin = strValue(diagnosis.origin)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (diagnosis.hidesStandardLibrary)
            message = makeString(start, shownName, " (consider renaming "_s, strRepr(origin), " since it has the same name as the standard library module named "_s, shownName, " and prevents importing that standard library module)"_s);
        else {
            JSValue isInitializingValue = getAttrIfPresent(globalObject, spec, internString(vm, "_initializing"_s));
            RETURN_IF_EXCEPTION(scope, { });
            bool isInitializing = isInitializingValue && isTrue(globalObject, isInitializingValue);
            RETURN_IF_EXCEPTION(scope, { });
            if (isInitializing && diagnosis.mayHideAnother)
                message = makeString(start, shownName, " (consider renaming "_s, strRepr(origin), " if it has the same name as a library you intended to import)"_s);
            else if (isInitializing)
                message = makeString(start, "partially initialized module "_s, shownName, " (most likely due to a circular import)"_s, origin.isNull() ? ""_s : " ("_s, origin, origin.isNull() ? ""_s : ")"_s);
            else
                message = makeString(start, shownName, " ("_s, origin.isNull() ? "unknown location"_str : origin, ')');
        }
    }
    PyException* exception = newException(globalObject, rt.ImportError(), message);
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::importErrorName, moduleName ? moduleName : jsUndefined());
    exception->setField(vm, ExceptionField::importErrorPath, diagnosis.origin ? diagnosis.origin : jsUndefined());
    exception->setField(vm, ExceptionField::importErrorNameFrom, name);
    return raiseException(globalObject, scope, exception);
}

// from module import *
void importStar(JSGlobalObject* globalObject, JSValue module, JSValue locals)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool skipUnderscored = false;
    JSValue names = getAttrIfPresent(globalObject, module, rt.names.dAll());
    RETURN_IF_EXCEPTION(scope, void());
    if (!names) {
        JSValue dict = getAttrIfPresent(globalObject, module, rt.names.dDict());
        RETURN_IF_EXCEPTION(scope, void());
        if (!dict) {
            raiseImportError(globalObject, scope, "from-import-* object has no __dict__ and no __all__"_s);
            return;
        }
        names = listFromIterable(globalObject, dict);
        RETURN_IF_EXCEPTION(scope, void());
        skipUnderscored = true;
    }

    if (PyType* type = rt.typeOf(names); !type->lookup(Special::GetItem)) {
        raiseTypeError(globalObject, scope, '\'', type->name(), "' object does not support indexing"_s);
        return;
    }

    auto* localsDict = isDict(locals) && PyDict::from(locals)->type() == rt.dictType() ? PyDict::from(locals) : nullptr;
    // As a sequence, by index, until an IndexError.
    for (int64_t position = 0;; ++position) {
        JSValue item = getItem(globalObject, names, jsNumber(static_cast<double>(position)));
        if (!item) {
            clearPendingExceptionIfMatches(globalObject, rt.IndexError());
            return;
        }
        if (!isStr(item)) {
            JSValue moduleName = getAttr(globalObject, module, rt.names.dName());
            RETURN_IF_EXCEPTION(scope, void());
            String shown = reprForMessage(globalObject, moduleName);
            if (isStr(moduleName)) {
                shown = strValue(moduleName)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, void());
            }
            raiseTypeError(globalObject, scope, skipUnderscored ? "Key in "_s : "Item in "_s, shown, skipUnderscored ? ".__dict__"_s : ".__all__"_s, " must be str, not "_s, typeName(globalObject, item));
            return;
        }
        JSString* name = internString(globalObject, strValue(item));
        RETURN_IF_EXCEPTION(scope, void());
        if (skipUnderscored && stringOf(name).startsWith('_'))
            continue;
        JSValue value = getAttr(globalObject, module, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (localsDict)
            localsDict->set(vm, name, value);
        else {
            setItem(globalObject, locals, name, value);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

// ---- Lazy imports (PEP 810) ----
//
// `lazy import a` binds a to a placeholder, an instance of lazy_import. The import is carried out when the name is first looked
// up: loading a global or a name, or getting an attribute of a module, that finds a placeholder puts what it stands for in
// its place. Nothing else does: globals()["a"] is the placeholder. (Objects/lazyimportobject.c and Python/import.c in
// CPython are the reference.)
//
// There are two kinds. A root is what importing a module leaves: it has the absolute name of the module, and the `from` list.
// A projection is what `from` takes from another placeholder, as do the `.b.c` of `import a.b.c as d`: it has that
// placeholder, and the name of the attribute.

enum LazyImportField : unsigned {
    LazyImportBuiltins, // Of a root: the mapping that has __import__.
    LazyImportSource, // The name of the module, or the placeholder this is an attribute of.
    LazyImportAttribute, // Of a root: the `from` list, a tuple, or empty. Of a projection: the name.
    LazyImportFunction, // The code the statement is in, or empty. The `number` of the record is the line.
    LazyImportIsActive, // Of the root of `import a.b`: a has been used, so a.b is to behave as if it had been imported. Empty: no.
    LazyImportFieldCount
};

static PyType* lazyImportType(JSGlobalObject*);

PyRecord* asLazyImportSlow(JSGlobalObject* globalObject, JSCell* cell)
{
    return PyRecord::tryFrom(cell, lazyImportType(globalObject));
}

// By this thread, that is: to come upon it again is to have gone round in a circle. Another thread may well be resolving it at the
// same time, and both get what they want, because importlib imports a module once. So what a thread is resolving is kept with
// what else is the thread's own.
bool lazyImportIsBeingResolved(PyRecord* lazy)
{
    JSValue map = lazy->structure()->realm()->pythonRuntime()->threadLocals();
    return map && map.isCell() && uncheckedDowncast<JSWeakMap>(map.asCell())->has(lazy);
}

static PyRecord* lazyImportSource(JSGlobalObject* globalObject, PyRecord* lazy) { return asLazyImport(globalObject, lazy->get(LazyImportSource)); }

static bool lazyImportHasFromList(PyRecord* root)
{
    JSValue fromList = root->get(LazyImportAttribute);
    return fromList && PyTuple::from(fromList)->length();
}

static PyRecord* newLazyImport(JSGlobalObject* globalObject, JSValue builtins, JSValue source, JSValue attribute, PyFrame* frame)
{
    VM& vm = getVM(globalObject);
    PyRecord* lazy = PyRecord::create(globalObject, lazyImportType(globalObject), LazyImportFieldCount);
    lazy->set(vm, LazyImportBuiltins, builtins);
    lazy->set(vm, LazyImportSource, source);
    lazy->set(vm, LazyImportAttribute, attribute);
    if (frame) {
        lazy->set(vm, LazyImportFunction, frame->function());
        lazy->number = frame->lineNumber();
    }
    return lazy;
}

// The dotted name of what resolving it gives.
static String lazyImportPath(JSGlobalObject* globalObject, PyRecord* lazy)
{
    // (Not by recursion: there may be thousands of dots.)
    Vector<String, 8> attributes;
    while (PyRecord* source = lazyImportSource(globalObject, lazy)) {
        attributes.append(stringOf(asString(lazy->get(LazyImportAttribute))));
        lazy = source;
    }
    String name = stringOf(asString(lazy->get(LazyImportSource)));
    // __import__("a.b") returns a.
    if (size_t dot = name.find('.'); dot != notFound && !lazyImportHasFromList(lazy))
        name = name.left(dot);
    if (attributes.isEmpty())
        return name;
    StrBuilder builder;
    builder.append(name);
    for (size_t i = attributes.size(); i--;)
        builder.append('.', attributes[i]);
    return builder.toString();
}

// What messages call it.
static String lazyImportShownName(JSGlobalObject* globalObject, PyRecord* lazy)
{
    if (lazyImportSource(globalObject, lazy))
        return lazyImportPath(globalObject, lazy);
    String name = stringOf(asString(lazy->get(LazyImportSource)));
    return lazyImportHasFromList(lazy) ? makeString(name, "..."_s) : name;
}

// mapping[name], or an empty value and no exception if there is no such key.
static JSValue lazyImportOptionalItem(JSGlobalObject* globalObject, JSValue mapping, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    if (auto* dict = PyDict::tryFrom(mapping); dict && dict->type() == rt.dictType()) [[likely]]
        return dict->get(name);
    JSValue value = getItem(globalObject, mapping, name);
    if (!value)
        clearPendingExceptionIfMatches(globalObject, rt.KeyError());
    return value;
}

static bool moduleIsInitializing(JSGlobalObject* globalObject, JSValue module)
{
    // Not through getattr: that would have the __spec__ of a module written in C++ made.
    auto* object = PyModule::tryFrom(module);
    return object && specIsInitializing(globalObject, object->dict()->get(runtime(globalObject).names.dSpec()));
}

// An attribute of a module that has been imported and has finished running, found without running anything. Empty: none.
static JSValue loadedModuleAttribute(JSGlobalObject* globalObject, JSString* moduleName, JSString* attribute)
{
    auto* module = PyModule::tryFrom(runtime(globalObject).modules()->get(moduleName));
    if (!module || moduleIsInitializing(globalObject, module))
        return { };
    JSValue value = module->dict()->get(attribute);
    return asLazyImport(globalObject, value) ? JSValue() : value;
}

// -- What has been imported lazily and not yet in fact --

// Modules are imported before there is a sys to have made these.
static PySet* lazyModules(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (!rt.slot(Runtime::Slot::LazyModules))
        rt.setSlot(Runtime::Slot::LazyModules, PySet::create(globalObject));
    return PySet::from(rt.slot(Runtime::Slot::LazyModules));
}

static PyDict* lazyPendingSubmodules(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (!rt.slot(Runtime::Slot::LazyPendingSubmodules))
        rt.setSlot(Runtime::Slot::LazyPendingSubmodules, PyDict::create(globalObject));
    return PyDict::from(rt.slot(Runtime::Slot::LazyPendingSubmodules));
}

// Adds the name to sys.lazy_modules, unless the module is there already. `existing`: what sys.modules has for it.
static void addToLazyModules(JSGlobalObject* globalObject, JSString* name, JSValue* existing = nullptr)
{
    Runtime& rt = runtime(globalObject);
    JSValue module = rt.modules()->get(name);
    if (existing)
        *existing = module;
    bool isLoaded = module && !isNone(module);
    if (auto* object = isLoaded ? PyModule::tryFrom(module) : nullptr) {
        // Nothing of the program's is to run here. A spec that does not say in the usual way that it is done is taken not to be.
        if (JSValue spec = object->dict()->get(rt.names.dSpec()); spec && !isNone(spec)) {
            auto* instance = PyInstance::tryFrom(spec);
            JSValue isInitializing = instance ? instance->getAttribute(rt.vm(), internString(rt.vm(), "_initializing"_s)) : JSValue();
            isLoaded = instance && (!isInitializing || isInitializing == jsBoolean(false));
        }
    }
    if (!isLoaded)
        lazyModules(globalObject)->add(globalObject, name);
}

// Using the a of `lazy import a.b.c` imports a alone; a.b is imported when that attribute of a is first asked for and found
// missing, and so on. For that, the submodules that lazy imports have named are noted here:
// { "a": { "b": source }, "a.b": { "c": source } }. The source is the placeholder of the import, or None for a name of
// `lazy from a import b`, which may turn out to be a submodule, and for a submodule that was imported already.
static void registerLazySubmodules(JSGlobalObject* globalObject, const String& name, JSValue source)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* pending = lazyPendingSubmodules(globalObject);
    size_t end = name.length();
    for (size_t dot = name.reverseFind('.'); dot != notFound && dot; dot = name.reverseFind('.', dot - 1)) {
        JSString* parent = internString(vm, name.left(dot));
        JSString* child = internString(vm, name.substring(dot + 1, end - dot - 1));
        JSValue children = pending->get(parent);
        if (!children) {
            children = PyDict::create(globalObject);
            pending->set(vm, parent, children);
        }
        JSValue cached = rt.modules()->get(internString(vm, name.left(end)));
        JSValue value = cached && !isNone(cached) ? jsUndefined() : source;
        if (!isNone(source) || !PyDict::from(children)->get(child))
            PyDict::from(children)->set(vm, child, value);
        end = dot;
    }
}

// The submodule has been imported: it is no longer waited for. With `bind`, it is made an attribute of its parent if that
// has none of the name, as importing it would have.
static void clearLazySubmodule(JSGlobalObject* globalObject, JSString* name, bool bind)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* pending = lazyPendingSubmodules(globalObject);
    if (!pending->size())
        return;
    String text = stringOf(name);
    size_t dot = text.reverseFind('.');
    if (dot == notFound)
        return;
    JSString* parent = internString(vm, text.left(dot));
    JSString* child = internString(vm, text.substring(dot + 1));
    JSValue childrenValue = pending->get(parent);
    if (!childrenValue || !PyDict::from(childrenValue)->get(child))
        return;
    PyDict* children = PyDict::from(childrenValue);
    JSValue module = rt.modules()->get(name);
    if (module && moduleIsInitializing(globalObject, module))
        return;
    // While the parent may yet fail, the name is kept.
    JSValue parentModule = rt.modules()->get(parent);
    if (parentModule && moduleIsInitializing(globalObject, parentModule)) {
        children->set(vm, child, jsUndefined());
        return;
    }
    if (auto* object = PyModule::tryFrom(parentModule); bind && object && object->type() == rt.moduleType() && module && !isNone(module)) {
        if (!object->dict()->get(child))
            object->dict()->set(vm, child, module);
    }
    children->remove(vm, child);
}

void didImportLazyModule(JSGlobalObject* globalObject, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* modules = lazyModules(globalObject);
    if (modules->size()) {
        modules->remove(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
    }
    clearLazySubmodule(globalObject, name, false);
}

// -- Making placeholders --

bool isInLazyModules(JSGlobalObject* globalObject, JSString* name, PyDict* globals, int level)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue names = globals->get(internString(vm, "__lazy_modules__"_s));
    if (!names)
        return false;
    JSString* absoluteName = absoluteModuleName(globalObject, name, globals, level);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, contains(globalObject, names, absoluteName));
}

JSValue lazyImportName(JSGlobalObject* globalObject, JSValue nameValue, JSValue builtins, JSValue globals, JSValue locals, JSValue fromList, int level)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "module name must be a string, got "_s, typeNameForMessage(globalObject, rt.typeOf(nameValue)));
    if (level < 0)
        return raiseValueError(globalObject, scope, "level must be >= 0"_s);
    JSString* name = strValue(nameValue);
    JSString* absoluteName = absoluteModuleName(globalObject, name, globals, level);
    RETURN_IF_EXCEPTION(scope, { });

    PyFrame* frame = currentFrame(globalObject);
    if (!frame || currentLocals(globalObject) != JSValue(frame->function()->globals()))
        return raise(globalObject, scope, rt.SyntaxError(), "'lazy import' is only allowed at module level"_s);

    if (!fromList)
        fromList = jsUndefined();
    else if (isStr(fromList))
        fromList = PyTuple::create(globalObject, std::span<const JSValue> { &fromList, 1 });

    if (JSValue filter = rt.slot(Runtime::Slot::LazyImportsFilter); filter && !isNone(filter)) {
        JSValue importer = PyDict::from(globals)->get(rt.names.dName());
        JSValue decision = call(globalObject, filter, importer ? importer : jsUndefined(), absoluteName, fromList);
        RETURN_IF_EXCEPTION(scope, { });
        bool mayBeLazy = isTrue(globalObject, decision);
        RETURN_IF_EXCEPTION(scope, { });
        if (!mayBeLazy)
            RELEASE_AND_RETURN(scope, importName(globalObject, name, globals, locals, fromList, level));
    }

    auto* names = PyTuple::tryFrom(fromList);
    if (names) {
        for (unsigned i = 0; i < names->length(); ++i) {
            if (!isStr(names->at(i)))
                return raiseTypeError(globalObject, scope, "Item in ``from list'' must be str, not "_s, typeName(globalObject, names->at(i)));
        }
    } else if (!isNone(fromList))
        return raiseTypeError(globalObject, scope, "lazy_import: fromlist must be None, a string, or a tuple"_s);

    PyRecord* lazy = newLazyImport(globalObject, builtins, absoluteName, names ? JSValue(names) : JSValue(), frame);
    JSValue existing;
    addToLazyModules(globalObject, absoluteName, &existing);
    RETURN_IF_EXCEPTION(scope, { });
    String absoluteText = stringOf(absoluteName);
    if (!names || !names->length()) {
        registerLazySubmodules(globalObject, absoluteText, lazy);
        return lazy;
    }
    auto* existingModule = PyModule::tryFrom(existing);
    for (unsigned i = 0; i < names->length(); ++i) {
        JSString* item = internString(globalObject, strValue(names->at(i)));
        RETURN_IF_EXCEPTION(scope, { });
        // What the module already has is not a submodule yet to be imported.
        if (existingModule && existingModule->dict()->get(item))
            continue;
        String submodule = makeString(absoluteText, '.', stringOf(item));
        addToLazyModules(globalObject, internString(vm, submodule));
        RETURN_IF_EXCEPTION(scope, { });
        registerLazySubmodules(globalObject, submodule, jsUndefined());
    }
    return lazy;
}

static JSValue lazyImportFrom(JSGlobalObject* globalObject, PyRecord* lazy, JSString* name)
{
    // `from a import b` can take b from an a that has been imported. `import a.b as c` has to import a.b first.
    if (!lazyImportSource(globalObject, lazy) && lazyImportHasFromList(lazy)) {
        if (JSValue value = loadedModuleAttribute(globalObject, asString(lazy->get(LazyImportSource)), name))
            return value;
    }
    return newLazyImport(globalObject, { }, lazy, name, currentFrame(globalObject));
}

// -- Resolving them --

static JSValue resolveLazyImport(JSGlobalObject*, PyRecord*, JSValue* importedModule);

static JSValue resolvedIfLazyImport(JSGlobalObject* globalObject, JSValue value)
{
    PyRecord* lazy = asLazyImport(globalObject, value);
    return lazy ? resolveLazyImport(globalObject, lazy, nullptr) : value;
}

// Gets from the module, which the import of the root gave, the attributes on the way from the root to the placeholder.
static JSValue replayLazyImportFrom(JSGlobalObject* globalObject, JSValue module, PyRecord* lazy)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // There is one for each dot of `lazy import a.b.c as x`, and there may be thousands.
    MarkedArgumentBuffer attributes;
    for (PyRecord* step = lazy; lazyImportSource(globalObject, step); step = lazyImportSource(globalObject, step))
        attributes.append(step->get(LazyImportAttribute));
    JSValue value = module;
    for (size_t i = attributes.size(); i--;) {
        value = importFrom(globalObject, value, asString(attributes.at(i)));
        RETURN_IF_EXCEPTION(scope, { });
        value = resolvedIfLazyImport(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

// `lazy import a.b.c` binds a, and using it imports a alone (see registerLazySubmodules()): the answer is "a". Unless that
// could be told from what `import a.b.c` does: when they have all been imported, or one on the way that has been is not a
// package known by that name. Then it is the whole name.
static JSString* firstModuleOfLazyImport(JSGlobalObject* globalObject, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    String text = stringOf(name);
    size_t firstDot = text.find('.');
    if (firstDot == notFound)
        return name;
    bool allAreImported = true;
    for (size_t dot = firstDot; dot != notFound; dot = text.find('.', dot + 1)) {
        JSString* package = internString(vm, text.left(dot));
        JSValue knownAs = loadedModuleAttribute(globalObject, package, rt.names.dName());
        if (!knownAs) {
            allAreImported = false;
            break;
        }
        if (!loadedModuleAttribute(globalObject, package, rt.names.dPath()) || !knownAs.isString() || stringOf(asString(knownAs)) != stringOf(package))
            return name;
    }
    if (allAreImported && loadedModuleAttribute(globalObject, name, rt.names.dName()))
        return name;
    return internString(vm, text.left(firstDot));
}

static JSValue carryOutLazyImport(JSGlobalObject* globalObject, PyRecord* lazy, JSValue* importedModule)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Back to what importing the module left, and to what was first taken from that.
    PyRecord* root = lazy;
    PyRecord* first = nullptr;
    while (PyRecord* source = lazyImportSource(globalObject, root)) {
        first = root;
        root = source;
    }
    JSString* rootName = asString(root->get(LazyImportSource));
    JSValue builtins = root->get(LazyImportBuiltins);

    // Of `from a import b, c`, only the name being resolved is asked for.
    JSValue fromList = root->get(LazyImportAttribute);
    bool hasFromList = lazyImportHasFromList(root);
    if (first && hasFromList) {
        JSValue only = first->get(LazyImportAttribute);
        fromList = PyTuple::create(globalObject, std::span<const JSValue> { &only, 1 });
    } else if (!fromList)
        fromList = jsUndefined();

    PyDict* currentDict = currentGlobals(globalObject);
    JSValue globals = currentDict ? JSValue(currentDict) : jsUndefined();
    JSValue importFunction = lazyImportOptionalItem(globalObject, builtins, rt.names.dImport());
    RETURN_IF_EXCEPTION(scope, { });
    if (!importFunction)
        return raiseImportError(globalObject, scope, "__import__ not found"_s);
    bool isDefaultImport = importFunction == rt.defaultImportFunction();
    auto import = [&](JSString* name) -> JSValue {
        if (isDefaultImport)
            return importName(globalObject, name, globals, globals, fromList, 0);
        return call(globalObject, importFunction, name, globals, globals, fromList, jsNumber(0));
    };

    JSString* name = rootName;
    if (!first && !hasFromList && isDefaultImport && builtins == JSValue(rt.builtins()))
        name = firstModuleOfLazyImport(globalObject, rootName);
    JSValue result = import(name);
    RETURN_IF_EXCEPTION(scope, { });
    if (name != rootName) {
        root->set(vm, LazyImportIsActive, jsBoolean(true));
        // What is not a package has no submodules, as importing them all will say.
        auto* module = PyModule::tryFrom(result);
        if (!module || module->type() != rt.moduleType() || !module->dict()->get(rt.names.dPath())) {
            result = import(rootName);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else if (isDefaultImport)
        clearLazySubmodule(globalObject, name, false);

    // An __import__ of the program's may return a placeholder.
    result = resolvedIfLazyImport(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (importedModule && isDefaultImport && PyModule::tryFrom(result))
        *importedModule = result;
    RELEASE_AND_RETURN(scope, replayLazyImportFrom(globalObject, result, lazy));
}

// The exception that resolving it raised is made to tell where the import statement is.
static void addLazyImportToPendingException(JSGlobalObject* globalObject, PyRecord* lazy)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue functionValue = lazy->get(LazyImportFunction);
    if (!functionValue)
        return;
    auto* function = uncheckedDowncast<PyFunction>(functionValue.asCell());
    JSValue raised = takePendingException(globalObject);
    auto* exception = PyException::tryFrom(raised);
    if (!exception) {
        throwPythonException(globalObject, scope, raised);
        return;
    }
    String name = lazyImportShownName(globalObject, lazy);
    CodeInfo& info = function->codeInfo();
    int line = static_cast<int>(lazy->number);

    if (exception->cause() || exception->context() || exception->suppressContext()) {
        // It has a history of its own, which stays as it is.
        JSValue note = makeStr(vm, makeString("lazy import of '"_s, name, "' declared in "_s, info.name.string(), " at "_s, info.filename, ':', line));
        JSValue notes = getAttrIfPresent(globalObject, exception, rt.names.dNotes());
        bool isThere = !scope.exception() && notes && contains(globalObject, notes, note);
        if (!scope.exception() && !isThere)
            callMethod(globalObject, exception, internString(vm, "add_note"_s), note);
    } else if (PyException* cause = newException(globalObject, rt.ImportError(), makeString("lazy import of '"_s, name, "' raised an exception during resolution"_s))) {
        PyFrame* frame = PyFrame::createSuspended(globalObject, function, 0);
        frame->setLineNumber(line);
        cause->setTraceback(vm, PyTraceback::create(globalObject, { }, frame, line, 0));
        exception->setCause(vm, cause);
        exception->setSuppressContext(true);
    }
    if (scope.exception())
        takePendingException(globalObject);
    throwPythonException(globalObject, scope, raised);
}

static JSValue resolveLazyImport(JSGlobalObject* globalObject, PyRecord* lazy, JSValue* importedModule)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (lazyImportIsBeingResolved(lazy)) {
        PyRecord* root = lazy;
        while (PyRecord* source = lazyImportSource(globalObject, root))
            root = source;
        PyException* exception = newException(globalObject, rt.ImportCycleError(), makeString("cannot import name "_s, strRepr(lazyImportShownName(globalObject, lazy)), " (most likely due to a circular import)"_s));
        RETURN_IF_EXCEPTION(scope, { });
        exception->setField(vm, ExceptionField::importErrorName, root->get(LazyImportSource));
        return raiseException(globalObject, scope, exception);
    }
    if (!rt.tryEnterFrame())
        return { };
    if (!rt.threadLocals() || !rt.threadLocals().isCell())
        rt.setThreadLocals(JSWeakMap::create(vm, globalObject->weakMapStructure()));
    uncheckedDowncast<JSWeakMap>(rt.threadLocals().asCell())->set(vm, lazy, jsBoolean(true));
    JSValue result = carryOutLazyImport(globalObject, lazy, importedModule);
    uncheckedDowncast<JSWeakMap>(rt.threadLocals().asCell())->remove(lazy);
    rt.leaveFrame();
    if (scope.exception()) {
        addLazyImportToPendingException(globalObject, lazy);
        return { };
    }
    lazyModules(globalObject)->remove(globalObject, internString(vm, lazyImportPath(globalObject, lazy)));
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// In pkg/__init__.py, `lazy from .child import child`: importing pkg.child made the module an attribute of pkg, in the place of
// the placeholder. As after the import that is not lazy, the name is to be what was imported from the module.
static void replaceSubmoduleWithLazyImport(JSGlobalObject* globalObject, PyRecord* lazy, JSString* name, PyDict* names, JSValue submodule, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* root = lazyImportSource(globalObject, lazy);
    if (!root)
        return;
    while (PyRecord* source = lazyImportSource(globalObject, root))
        root = source;
    String rootName = stringOf(asString(root->get(LazyImportSource)));
    size_t dot = rootName.reverseFind('.');
    if (dot == notFound || StringView(rootName).substring(dot + 1) != stringOf(name))
        return;
    auto* parent = PyModule::tryFrom(rt.modules()->get(internString(vm, rootName.left(dot))));
    if (parent && parent->dict() == names && names->get(name) == submodule)
        names->set(vm, name, value);
}

JSValue reifyLazyImport(JSGlobalObject* globalObject, PyRecord* lazy, JSString* name, JSValue mapping)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue importedModule;
    JSValue value = resolveLazyImport(globalObject, lazy, &importedModule);
    RETURN_IF_EXCEPTION(scope, { });

    if (auto* dict = PyDict::tryFrom(mapping); dict && dict->type() == rt.dictType()) {
        if (dict->get(name) == JSValue(lazy))
            dict->set(vm, name, value);
        else if (importedModule)
            replaceSubmoduleWithLazyImport(globalObject, lazy, name, dict, importedModule, value);
        return value;
    }
    if (!rt.typeOf(mapping)->lookup(Special::SetItem))
        return value;
    JSValue current = lazyImportOptionalItem(globalObject, mapping, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (current == JSValue(lazy)) {
        setItem(globalObject, mapping, name, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

// Imports the submodule as `import a.b as x` would, on behalf of the `lazy import` that named it.
static JSValue loadLazyChild(JSGlobalObject* globalObject, PyRecord* declaration, const String& name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* lazy = newLazyImport(globalObject, declaration->get(LazyImportBuiltins), internString(vm, name), { }, nullptr);
    for (size_t dot = name.find('.'); dot != notFound;) {
        size_t start = dot + 1;
        dot = name.find('.', start);
        lazy = newLazyImport(globalObject, { }, lazy, internString(vm, name.substring(start, dot == notFound ? name.length() - start : dot - start)), nullptr);
    }
    lazy->set(vm, LazyImportFunction, declaration->get(LazyImportFunction));
    lazy->number = declaration->number;
    JSValue result = resolveLazyImport(globalObject, lazy, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    // If it is no package, what the import names further on does not exist, which has to be said.
    String declared = stringOf(asString(declaration->get(LazyImportSource)));
    if (auto* module = PyModule::tryFrom(result); module && declared != name && !module->dict()->get(rt.names.dPath()))
        RELEASE_AND_RETURN(scope, loadLazyChild(globalObject, declaration, declared));
    return result;
}

JSValue loadLazySubmodule(JSGlobalObject* globalObject, PyModule* module, JSString* attribute, bool suppress, bool& lookAgain)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    lookAgain = false;
    PyDict* pending = lazyPendingSubmodules(globalObject);
    if (!pending->size()) [[likely]]
        return { };
    PyDict* dict = module->dict();
    JSValue moduleName = dict->get(rt.names.dName());
    if (!moduleName || !moduleName.isString())
        return { };
    // A program may have given the module a name it put together.
    if (asString(moduleName)->isRope()) {
        asString(moduleName)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue childrenValue = pending->get(asString(moduleName));
    if (!childrenValue)
        return { };
    lookAgain = true;
    PyDict* children = PyDict::from(childrenValue);
    JSValue source = children->get(attribute);
    if (!source)
        return { };

    String fullText = makeString(stringOf(asString(moduleName)), '.', stringOf(attribute));
    JSString* fullName = internString(vm, fullText);
    JSValue loaded;
    PyRecord* declaration = asLazyImport(globalObject, source);
    if (declaration && !suppress && (declaration->get(LazyImportIsActive) || !dict->get(rt.name(Special::GetAttr))))
        loaded = loadLazyChild(globalObject, declaration, fullText);
    else {
        loaded = rt.modules()->get(fullName);
        if (!loaded || isNone(loaded))
            loaded = importModuleOrNone(globalObject, fullName, true);
    }
    if (scope.exception()) {
        // A submodule that was imported, when what followed failed, is waited for again.
        JSValue imported = rt.modules()->get(fullName);
        if (imported && !isNone(imported)) {
            if (dict->get(attribute) == imported)
                dict->remove(vm, attribute);
            if (!children->get(attribute))
                children->set(vm, attribute, source);
        }
        return { };
    }
    // One that is missing goes on being waited for: it may be there another time.
    if (isNone(loaded))
        return { };
    if (!moduleIsInitializing(globalObject, loaded)) {
        dict->set(vm, attribute, loaded);
        children->remove(vm, attribute);
    }
    return loaded;
}

// -- The type --

static JSValue lazyImportRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(getVM(globalObject), makeString("<lazy_import '"_s, lazyImportShownName(globalObject, PyRecord::from(self)), "'>"_s));
}

static JSValue lazyImportGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = genericGetAttr(globalObject, self, name, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    return raiseAttributeError(globalObject, scope, self, name, makeString("cannot access attribute "_s, strRepr(stringOf(name)), " on unresolved lazy import "_s, strRepr(lazyImportShownName(globalObject, PyRecord::from(self)))));
}

static JSValue lazyImportResolve(JSGlobalObject* globalObject, JSValue self) { return resolveLazyImport(globalObject, PyRecord::from(self), nullptr); }

static constexpr NativeDef lazyImportMethods[] = {
    PY_SLOT("__repr__", lazyImportRepr, "($self, /)"),
    PY_SLOT("__getattribute__", lazyImportGetAttribute, "($self, name, /)"),
    PY_METHOD("resolve", lazyImportResolve, "($self, /)"),
};

static constexpr TypeDef lazyImportTypeDef { "lazy_import"_s, { }, lazyImportMethods, { }, "What a lazy import binds a name to until the name is first used."_s };

static PyType* lazyImportType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(lazyImportTypeDef); }
PyType* typeOfLazyImports(JSGlobalObject* globalObject) { return lazyImportType(globalObject); }

} // namespace Python
} // namespace JSC
