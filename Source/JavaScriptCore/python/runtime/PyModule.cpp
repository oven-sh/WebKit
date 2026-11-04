/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTypes.h"
#include <unistd.h>

namespace JSC {
namespace Python {

// Module objects. (Objects/moduleobject.c in CPython is the reference.)

PY_DEFINE_CELL(PyModule);

PyModule::PyModule(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyModule* PyModule::create(VM& vm, Structure* structure, PyType* type)
{
    auto* module = new (NotNull, allocateCell<PyModule>(vm)) PyModule(vm, structure, type);
    module->finishCreation(vm);
    return module;
}

static void initializeModuleDict(JSGlobalObject* globalObject, PyModule* module, JSValue name, JSValue doc)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* dict = module->dict();
    dict->set(vm, rt.names.dName(), name);
    dict->set(vm, rt.names.dDoc(), doc);
    dict->set(vm, rt.names.dPackage(), jsUndefined());
    dict->set(vm, rt.names.dLoader(), jsUndefined());
    dict->set(vm, rt.names.dSpec(), jsUndefined());
}

PyModule* PyModule::create(JSGlobalObject* globalObject, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyModule* module = create(vm, rt.structure<PyModule>(), rt.moduleType());
    module->setDict(vm, PyDict::create(globalObject));
    initializeModuleDict(globalObject, module, name, jsUndefined());
    return module;
}

PyModule* PyModule::create(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return create(globalObject, internString(getVM(globalObject), name));
}

template<typename Visitor>
void PyModule::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyModule>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_dict);
}

DEFINE_VISIT_CHILDREN(PyModule);

void PyModule::add(JSGlobalObject* globalObject, ASCIILiteral name, JSValue value)
{
    VM& vm = getVM(globalObject);
    dict()->set(vm, internString(vm, name), value);
}

void PyModule::addFunctions(JSGlobalObject* globalObject, std::span<const NativeDef> defs)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue moduleName = dict()->get(rt.names.dName());
    for (auto& def : defs) {
        ASSERT(def.kind == NativeKind::Function);
        dict()->set(vm, internString(vm, def.name), PyNativeFunction::create(globalObject, def, NativeKind::Function, { }, moduleName));
    }
}

// ---- The type ----

static JSValue moduleNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyModule* module = PyModule::create(vm, rt.structure<PyModule>(), PyType::from(cls));
    module->setDict(vm, PyDict::create(globalObject));
    return module;
}

static JSValue moduleInit(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue doc)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "module() argument 'name' must be str, not "_s, typeNameOfArgument(globalObject, name));
    initializeModuleDict(globalObject, PyModule::from(self), name, doc ? doc : jsUndefined());
    return jsUndefined();
}

// importlib._bootstrap._module_repr() says. What follows that is for when there is no importlib.
static JSValue moduleRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* findAndLoad = PyFunction::tryFrom(rt.slot(Runtime::Slot::FindAndLoad))) {
        if (JSValue function = findAndLoad->globals()->get(internString(vm, "_module_repr"_s)))
            RELEASE_AND_RETURN(scope, call(globalObject, function, self));
    }
    JSValue loader = getAttrIfPresent(globalObject, self, rt.names.dLoader());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue spec = getAttrIfPresent(globalObject, self, rt.names.dSpec());
    RETURN_IF_EXCEPTION(scope, { });
    bool hasSpec = spec && isTrue(globalObject, spec);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasSpec) {
        // <module 'x' from 'origin'>, <module 'x' (origin)> or <module 'x' (loader)>
        String nameRepr = "'?'"_s;
        JSValue specName = getAttr(globalObject, spec, rt.names.name());
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(specName))
            nameRepr = reprForMessage(globalObject, specName);
        JSValue origin = getAttrIfPresent(globalObject, spec, internString(vm, "origin"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!origin || isNone(origin)) {
            JSValue specLoader = getAttrIfPresent(globalObject, spec, internString(vm, "loader"_s));
            RETURN_IF_EXCEPTION(scope, { });
            if (!specLoader || isNone(specLoader))
                return makeStr(vm, makeString("<module "_s, nameRepr, '>'));
            RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<module "_s, nameRepr, " ("_s, reprForMessage(globalObject, specLoader), ")>"_s)));
        }
        JSValue hasLocation = getAttrIfPresent(globalObject, spec, internString(vm, "has_location"_s));
        RETURN_IF_EXCEPTION(scope, { });
        bool isLocation = hasLocation && isTrue(globalObject, hasLocation);
        RETURN_IF_EXCEPTION(scope, { });
        if (isLocation)
            RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<module "_s, nameRepr, " from "_s, reprForMessage(globalObject, origin), '>')));
        JSString* originText = str(globalObject, origin);
        RETURN_IF_EXCEPTION(scope, { });
        String text = originText->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return makeStr(vm, makeString("<module "_s, nameRepr, " ("_s, text, ")>"_s));
    }

    JSValue name = getAttrIfPresent(globalObject, self, rt.names.dName());
    RETURN_IF_EXCEPTION(scope, { });
    String nameRepr = name ? reprForMessage(globalObject, name) : "'?'"_str;
    JSValue file = getAttrIfPresent(globalObject, self, rt.names.dFile());
    RETURN_IF_EXCEPTION(scope, { });
    if (file)
        RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<module "_s, nameRepr, " from "_s, reprForMessage(globalObject, file), '>')));
    if (loader && !isNone(loader))
        RELEASE_AND_RETURN(scope, makeStr(vm, makeString("<module "_s, nameRepr, " ("_s, reprForMessage(globalObject, loader), ")>"_s)));
    return makeStr(vm, makeString("<module "_s, nameRepr, '>'));
}

bool specIsInitializing(JSGlobalObject* globalObject, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!spec || isNone(spec))
        return false;
    JSValue value = getAttrIfPresent(globalObject, spec, internString(vm, "_initializing"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, value));
}

// ---- What to say of a module that lacks a name ----

// spec.origin, if the spec says that that is where the module is from, and it is a str.
static JSValue fileOriginOfSpec(JSGlobalObject* globalObject, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hasLocation = getAttrIfPresent(globalObject, spec, internString(vm, "has_location"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasLocation)
        return { };
    bool isLocation = isTrue(globalObject, hasLocation);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isLocation)
        return { };
    JSValue origin = getAttrIfPresent(globalObject, spec, internString(vm, "origin"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return origin && isStr(origin) ? origin : JSValue();
}

// Whether a module in that file may stand in the way of one of the same name further down the path: it is in the directory
// that was put first on sys.path, that of the script.
static bool mayHideAnotherModule(const String& origin)
{
    const Configuration& configuration = Python::configuration();
    if (configuration.safePath || configuration.firstSearchPath.isNull())
        return false;
    size_t slash = origin.reverseFind('/');
    if (slash == notFound)
        return false;
    if (StringView(origin).substring(slash + 1) == "__init__.py"_s) {
        if (!slash)
            return false;
        slash = origin.reverseFind('/', slash - 1);
        if (slash == notFound)
            return false;
    }
    StringView directory = StringView(origin).left(slash);
    if (!configuration.firstSearchPath.isEmpty())
        return directory == configuration.firstSearchPath;
    char buffer[PATH_MAX];
    return getcwd(buffer, sizeof(buffer)) && directory == String::fromUTF8(buffer);
}

bool diagnoseModule(JSGlobalObject* globalObject, JSValue spec, JSValue moduleName, ModuleDiagnosis& diagnosis)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    diagnosis.origin = fileOriginOfSpec(globalObject, spec);
    RETURN_IF_EXCEPTION(scope, false);
    if (!diagnosis.origin)
        return true;
    String origin = strValue(diagnosis.origin)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    diagnosis.mayHideAnother = mayHideAnotherModule(origin);
    if (!diagnosis.mayHideAnother)
        return true;
    if (auto* names = PySet::tryFrom(rt.sysDict()->get(internString(vm, "stdlib_module_names"_s)))) {
        diagnosis.hidesStandardLibrary = names->contains(globalObject, moduleName);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// Which attribute of which object it was is for whoever asked to say: getAttr().
static JSValue raiseModuleHasNoAttribute(JSGlobalObject* globalObject, PyModule* module, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = module->dict();
    String attribute = stringOf(name);
    JSValue moduleNameValue = dict->get(rt.names.dName());
    if (!moduleNameValue || !isStr(moduleNameValue))
        return raise(globalObject, scope, rt.AttributeError(), makeString("module has no attribute '"_s, attribute, '\''));
    String moduleName = strValue(moduleNameValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String plain = makeString("module '"_s, moduleName, "' has no attribute '"_s, attribute, '\'');
    JSValue spec = dict->get(rt.names.dSpec());
    if (!spec)
        return raise(globalObject, scope, rt.AttributeError(), plain);

    ModuleDiagnosis diagnosis;
    if (failed(scope, diagnoseModule(globalObject, spec, moduleNameValue, diagnosis)))
        return { };
    String origin;
    if (diagnosis.origin) {
        origin = strValue(diagnosis.origin)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (diagnosis.hidesStandardLibrary)
        return raise(globalObject, scope, rt.AttributeError(), makeString(plain, " (consider renaming '"_s, origin, "' since it has the same name as the standard library module named '"_s, moduleName, "' and prevents importing that standard library module)"_s));

    JSValue isInitializingValue = getAttrIfPresent(globalObject, spec, internString(vm, "_initializing"_s));
    RETURN_IF_EXCEPTION(scope, { });
    bool isInitializing = isInitializingValue && isTrue(globalObject, isInitializingValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (isInitializing) {
        if (diagnosis.mayHideAnother)
            return raise(globalObject, scope, rt.AttributeError(), makeString(plain, " (consider renaming '"_s, origin, "' if it has the same name as a library you intended to import)"_s));
        return raise(globalObject, scope, rt.AttributeError(), makeString("partially initialized module '"_s, moduleName, '\'', origin.isNull() ? ""_s : " from '"_s, origin, origin.isNull() ? ""_s : "'"_s, " has no attribute '"_s, attribute, "' (most likely due to a circular import)"_s));
    }
    JSValue submodules = getAttrIfPresent(globalObject, spec, internString(vm, "_uninitialized_submodules"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (submodules) {
        bool isSubmodule = contains(globalObject, submodules, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSubmodule)
            return raise(globalObject, scope, rt.AttributeError(), makeString("cannot access submodule '"_s, attribute, "' of module '"_s, moduleName, "' (most likely due to a circular import)"_s));
    }
    return raise(globalObject, scope, rt.AttributeError(), plain);
}

JSValue moduleGetAttr(JSGlobalObject* globalObject, PyModule* module, JSString* name, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Nearly always the name is in the dict, and nothing on the type comes before that but a data descriptor.
    PyDict* dict = module->dict();
    JSValue value;
    if (module->type() == rt.moduleType()) [[likely]] {
        value = dict->get(name);
        if (value && rt.moduleType()->lookup(name)) [[unlikely]]
            value = { };
    }
    if (!value) {
        value = genericGetAttr(globalObject, module, name, true);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!value) {
        bool lookAgain;
        value = loadLazySubmodule(globalObject, module, name, suppress, lookAgain);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value && lookAgain)
            value = dict->get(name);
    }
    if (value) {
        if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]] {
            // Asked whether it has the attribute while that is being imported, the answer is no.
            if (suppress && lazyImportIsBeingResolved(lazy))
                return { };
            RELEASE_AND_RETURN(scope, reifyLazyImport(globalObject, lazy, name, dict));
        }
        // Modules written in C++ are given these two when they are first asked for.
        if (value.isUndefined() && (name->tryGetValueImpl() == rt.names.dSpec()->tryGetValueImpl() || name->tryGetValueImpl() == rt.names.dLoader()->tryGetValueImpl())) [[unlikely]] {
            ensureNativeModuleSpec(globalObject, module);
            RETURN_IF_EXCEPTION(scope, { });
            if (JSValue made = dict->get(name))
                return made;
        }
        return value;
    }

    JSValue getter = dict->get(rt.name(Special::GetAttr));
    if (PyRecord* lazy = asLazyImport(globalObject, getter)) [[unlikely]] {
        getter = { };
        if (!lazyImportIsBeingResolved(lazy)) {
            getter = reifyLazyImport(globalObject, lazy, rt.name(Special::GetAttr), dict);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (getter) {
        JSValue result = call(globalObject, getter, name);
        EXCEPTION_ASSERT(!!scope.exception() == !result);
        if (!result && suppress)
            clearPendingExceptionIfMatches(globalObject, rt.AttributeError());
        return result;
    }
    if (suppress)
        return { };
    RELEASE_AND_RETURN(scope, raiseModuleHasNoAttribute(globalObject, module, name));
}

JSValue moduleGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    JSString* name = attributeName(globalObject, nameValue);
    if (!name)
        return { };
    return moduleGetAttr(globalObject, PyModule::from(self), name, false);
}

static JSValue moduleDir(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dictValue = getAttr(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    auto* dict = PyDict::tryFrom(dictValue);
    if (!dict)
        return raiseTypeError(globalObject, scope, "<module>.__dict__ is not a dictionary"_s);
    if (JSValue dirFunction = dict->get(rt.name(Special::Dir)))
        RELEASE_AND_RETURN(scope, call(globalObject, dirFunction));
    RELEASE_AND_RETURN(scope, listFromIterable(globalObject, dict));
}

static JSValue moduleGetDict(JSGlobalObject*, JSValue self) { return PyModule::from(self)->dict(); }

static JSValue moduleGetAnnotate(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    PyDict* dict = PyModule::from(self)->dict();
    if (JSValue annotate = dict->get(rt.names.dAnnotate()))
        return annotate;
    dict->set(rt.vm(), rt.names.dAnnotate(), jsUndefined());
    return jsUndefined();
}

static void moduleSetAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyModule::from(self)->dict();
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete __annotate__ attribute"_s);
        return;
    }
    if (!isNone(value) && !isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "__annotate__ must be callable or None"_s);
        return;
    }
    dict->set(vm, rt.names.dAnnotate(), value);
    if (!isNone(value))
        dict->remove(vm, rt.names.dAnnotations());
}

static JSValue moduleGetAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyModule::from(self)->dict();
    if (JSValue annotations = dict->get(rt.names.dAnnotations()))
        return annotations;
    bool isInitializing = specIsInitializing(globalObject, dict->get(rt.names.dSpec()));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue annotations;
    JSValue annotate = dict->get(rt.names.dAnnotate());
    if (annotate && !isNone(annotate)) {
        annotations = call(globalObject, annotate, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isDict(annotations))
            return raiseTypeError(globalObject, scope, "__annotate__() must return a dict, not "_s, typeName(globalObject, annotations));
    } else
        annotations = PyDict::create(globalObject);
    // What a module that is still running has annotated so far is not the last word.
    if (!isInitializing)
        dict->set(vm, rt.names.dAnnotations(), annotations);
    return annotations;
}

static void moduleSetAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyModule::from(self)->dict();
    if (value)
        dict->set(vm, rt.names.dAnnotations(), value);
    else if (!dict->remove(vm, rt.names.dAnnotations())) {
        raise(globalObject, scope, rt.AttributeError(), "__annotations__"_s);
        return;
    }
    dict->remove(vm, rt.names.dAnnotate());
}

static constexpr NativeDef moduleMethods[] = {
    PY_NEW(moduleNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", moduleInit, "($self, /, name, doc=None)"),
    PY_SLOT("__repr__", moduleRepr, "($self, /)"),
    PY_SLOT("__getattribute__", moduleGetAttribute, "($self, name, /)"),
    PY_METHOD("__dir__", moduleDir, "($self, /)"),
};

static constexpr NativeGetSet moduleGetSets[] = {
    PY_MEMBER("__dict__", moduleGetDict),
    { "__annotations__"_s, moduleGetAnnotations, moduleSetAnnotations },
    { "__annotate__"_s, moduleGetAnnotate, moduleSetAnnotate },
};

void initializeModuleTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.moduleType()->initializeBuiltin(globalObject, { "module"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, moduleMethods, moduleGetSets, { } }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC
