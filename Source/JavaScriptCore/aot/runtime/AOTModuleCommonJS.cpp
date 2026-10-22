/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <utility>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

// Compiled code comes here for what it does not evaluate by itself: a module of the library, data, code outside the image, a module with work for a link below it,
// a module that waits for a promise, and a module whose export "module.exports" is what require() answers.
JSValue Modules::require(ModuleID target)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& targetData = data(target);

    JSValue executed;
    switch (targetData.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        if (status(target) != ModuleStatus::Evaluated) {
            LoadedModule& loaded = ensureLoaded(target);
            if (loaded.isBeingRequired) {
                if (!loaded.exportsWhileRequired)
                    loaded.exportsWhileRequired.set(*this, constructEmptyObject(m_instance.globalObject));
                return loaded.exportsWhileRequired.get();
            }
            // An evaluation that an import has begun shows the namespace object, with its bindings as they are.
            loaded.isBeingRequired = status(target) != ModuleStatus::Evaluating;
            evaluateSynchronously(target, SynchronousEvaluationReason::Require);
            loaded.isBeingRequired = false;
            RETURN_IF_EXCEPTION(scope, { });
        }
        break;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        executed = execute(target);
        RETURN_IF_EXCEPTION(scope, { });
        break;
    case ModuleKind::External:
        switch (targetData.externalKind) {
        case ExternalKind::HostValue:
            executed = execute(target);
            RETURN_IF_EXCEPTION(scope, { });
            break;
        case ExternalKind::UncompiledCode:
            RELEASE_AND_RETURN(scope, requireThroughHost(pathString(target), *targetData.referrerModule()));
        }
        break;
    }

    if (!targetData.hasExportTable()) {
        RELEASE_ASSERT(executed);
        return executed;
    }

    switch (targetData.requireResult) {
    case RequireResult::ModuleExports:
        break;
    case RequireResult::DefaultExport: {
        LoadedModule& loaded = ensureLoaded(target);
        if (JSValue answered = loaded.requiredValue)
            return answered;
        std::optional<unsigned> index = m_table.findExport(target, vm.propertyNames->defaultKeyword.impl());
        RELEASE_ASSERT(index);
        JSValue defaultExport = readExport(target, *index);
        RETURN_IF_EXCEPTION(scope, { });
        loaded.requiredValue.set(*this, defaultExport);
        return defaultExport;
    }
    case RequireResult::Namespace:
        RELEASE_AND_RETURN(scope, namespaceForRequire(target));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A CommonJS module of the image is a function without parameters that runs at most once. A module that is required again while its body runs has exported what it has exported by then.
void Modules::evaluateCommonJS(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(module);
    ASSERT(moduleData.kind == ModuleKind::CommonJs);

    switch (status(module)) {
    case ModuleStatus::Evaluating:
    case ModuleStatus::Evaluated:
        return;
    case ModuleStatus::Failed:
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    case ModuleStatus::Unlinked:
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
        break;
    case ModuleStatus::Linked:
        break;
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
        RELEASE_ASSERT_NOT_REACHED();
    }

    ModuleRecord* record = recordIfExists(module);
    if (!record && moduleData.evaluatesItself())
        RELEASE_AND_RETURN(scope, evaluateByCallingBody(module));

    // The body of a module that nothing of the image calls stores no status, and whoever gave a module a record goes by the status in the record.
    setStatus(module, ModuleStatus::Evaluating);
    if (moduleData.evaluatesItself())
        evaluateByCallingBody(module);
    else
        callBodyOfModuleEnteredByLoader(module);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        didThrowInBody(module, exception->value());
        return;
    }
    setStatus(module, ModuleStatus::Evaluated);
    if (!record)
        return;

    // A link has given the module the record so that the evaluation comes by here.
    if (moduleData.capturesExports()) {
        captureExports(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!record->isRetained())
        retireRecord(record);
}

JSValue Modules::commonJSExports(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    ASSERT(moduleData.kind == ModuleKind::CommonJs);
    if (!moduleData.hasValue())
        return jsUndefined();
    JSValue exports = ProgramStatics::readVariable(m_instance, moduleData.value);
    return exports ? exports : jsUndefined();
}

JSValue Modules::exportsSeenByLoader(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->requirer) [[likely]]
        return commonJSExports(module);
    return loaded->requirer->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

void Modules::setCommonJSExports(ModuleID module, JSValue exports)
{
    ASSERT(data(module).kind == ModuleKind::CommonJs);
    setValue(module, exports);
}

// `bun run` gives the id "." to the first CommonJS file that its loader of ES modules fetches: the entry point, or the first such file in the graph of an entry point that is an ES module.
// The CommonJS files of a graph stand in front of everything else that its root requests.
std::optional<ModuleID> Modules::mainCommonJSModule() const
{
    if (!m_firstModuleRequestedByHost)
        return std::nullopt;
    ModuleID entryPoint = *m_firstModuleRequestedByHost;
    if (data(entryPoint).kind == ModuleKind::CommonJs)
        return entryPoint;
    auto imports = m_table.imports(entryPoint);
    auto firstImport = imports.begin();
    if (firstImport == imports.end())
        return std::nullopt;
    ModuleID first = (*firstImport).targetModule();
    return data(first).kind == ModuleKind::CommonJs ? std::optional(first) : m_table.fileBehindFace(first);
}

// No module has a module object or a require function until the program uses one as a value.
JSObject* Modules::moduleObject(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    LoadedModule* existing = loadedIfExists(module);
    if (existing && existing->requirer)
        return existing->requirer;

    const ModuleData& moduleData = data(module);
    ModuleObjects objects;
    if (moduleData.kind == ModuleKind::CommonJs) {
        OptionSet<ModuleObjectTrait> traits;
        if (moduleData.hasValue())
            traits.add(ModuleObjectTrait::HasExportsVariable);
        if (mainCommonJSModule() == module)
            traits.add(ModuleObjectTrait::IsMain);
        host().createModuleObject(realm, pathString(module), module, traits, objects);
    } else {
        JSObject* meta = importMeta(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        host().createRequirer(realm, meta, objects);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);

    LoadedModule& loaded = ensureLoaded(module);
    loaded.requirer.set(*this, objects.module);
    loaded.requireFunction.set(*this, objects.require);
    return objects.module;
}

// Every module whose module object a read of the program can reach exports an object. The module object of any other module is handed to nobody.
bool Modules::mayHandOutModuleObject(ModuleID module) const
{
    return data(module).kind != ModuleKind::CommonJs || data(module).hasValue();
}

JSObject* Modules::mainModuleObject()
{
    if (!m_firstModuleRequestedByHost || data(*m_firstModuleRequestedByHost).kind != ModuleKind::CommonJs || !mayHandOutModuleObject(*m_firstModuleRequestedByHost))
        return nullptr;
    return moduleObject(*m_firstModuleRequestedByHost);
}

JSValue Modules::requireFunction(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    LoadedModule* existing = loadedIfExists(module);
    if (existing && existing->requireFunction)
        return existing->requireFunction;
    moduleObject(module);
    RETURN_IF_EXCEPTION(scope, { });
    return ensureLoaded(module).requireFunction;
}

// What only time knows, for a program that looks: which require() came to a module first, and which of the calls in functions and branches have run.
// The objects that show it are made when somebody reads them, so nothing is allocated in the heap here.
void Modules::recordRequire(ModuleID target, ModuleID referrer)
{
    if (data(target).kind != ModuleKind::CommonJs) {
        markNamespaceAsRequired(target);
        return;
    }
    // A require() is recorded in front of the evaluation and behind it. The module has its parent from the first record on. It is a child from the second on, which a call that throws does not reach.
    uint64_t pair = (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
    auto recorded = m_recordedRequires.add(pair, false);
    if (recorded.isNewEntry) {
        LoadedModule& required = ensureLoaded(target);
        if (!required.firstRequirer)
            required.firstRequirer = referrer;
        return;
    }
    // The record in front of another require() of a module that has thrown.
    bool& isChild = recorded.iterator->value;
    if (isChild || status(target) == ModuleStatus::Failed)
        return;
    isChild = true;
    ensureLoaded(referrer).requiredModules.append(target);
}

JSObject* Modules::parentModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->firstRequirer || mainCommonJSModule() == module || !mayHandOutModuleObject(*loaded->firstRequirer))
        return nullptr;
    return moduleObject(*loaded->firstRequirer);
}

bool Modules::forEachNewChild(ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded)
        return true;
    for (ModuleID child : loaded->requiredModules) {
        if (!mayHandOutModuleObject(child))
            return false;
    }
    // The functor may run code of the program, which may require more.
    while (loaded->numberOfChildrenShown < loaded->requiredModules.size()) {
        ModuleID child = loaded->requiredModules[loaded->numberOfChildrenShown++];
        JSObject* childObject = moduleObject(child);
        RETURN_IF_EXCEPTION(scope, true);
        functor(childObject);
        RETURN_IF_EXCEPTION(scope, true);
    }
    return true;
}

JSValue Modules::requireDynamic(JSValue specifier, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    willLoadByName(m_instance.globalObject, specifier);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* requirer = moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, host().requireSpecifier(m_instance.globalObject, requirer, specifier));
}

JSValue Modules::requireThroughHost(JSValue specifier, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    admitCodeNamedBy(specifier);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* requirer = moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    RequestToHostScope requestToHost(*this);
    RELEASE_AND_RETURN(scope, host().requireSpecifier(m_instance.globalObject, requirer, specifier));
}

JSValue Modules::resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm form)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    JSObject* holder = nullptr;
    switch (form) {
    case ResolvedSpecifierForm::Path:
    case ResolvedSpecifierForm::PathForRequire:
        holder = moduleObject(referrer);
        break;
    case ResolvedSpecifierForm::URL:
        holder = importMeta(referrer);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, host().resolveSpecifier(m_instance.globalObject, holder, specifier, form));
}

// From the first require() on, the namespace object of a module without an export `__esModule` reads `true` under that name, for everybody who holds it.
void Modules::markNamespaceAsRequired(ModuleID module)
{
    if (!data(module).hasExportTable() || m_modulesWithMarkOfRequire.get(module.index()))
        return;
    m_modulesWithMarkOfRequire.set(module.index());
    JSObject* created = namespaceIfCreated(module, ModulePhase::Evaluation);
    if (!created)
        return;
    auto* namespaceObject = uncheckedDowncast<ModuleNamespace>(created);
    if (!namespaceObject->hasExport(m_instance.vm->propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
}

JSValue Modules::namespaceForRequire(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (LoadedModule* loaded = loadedIfExists(module)) {
        if (JSValue answered = loaded->requiredValue)
            return answered;
    }

    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    if (m_moduleExportsName.isNull())
        m_moduleExportsName = Identifier::fromString(vm, "module.exports"_s);

    JSValue esModule = readExportForRequire(namespaceObject, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue moduleExports;
    if (esModule) {
        moduleExports = readExportForRequire(namespaceObject, m_moduleExportsName);
        RETURN_IF_EXCEPTION(scope, { });
    }
    markNamespaceAsRequired(module);

    JSValue result = !moduleExports || moduleExports.isUndefinedOrNull() ? JSValue(namespaceObject) : moduleExports;
    ensureLoaded(module).requiredValue.set(*this, result);
    return result;
}

// An empty result says that the read threw, as it does for a binding in its dead zone.
JSValue Modules::readExportForRequire(JSObject* namespaceObject, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto* compiledNamespace = uncheckedDowncast<ModuleNamespace>(namespaceObject);
    std::optional<unsigned> index = compiledNamespace->find(name.impl());
    if (!index && !compiledNamespace->hasExport(name.impl()))
        return jsUndefined();

    JSValue exported = index ? readExport(compiledNamespace->module(), *index) : namespaceObject->get(m_instance.globalObject, name);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, { });
        return { };
    }
    return exported;
}

void Modules::throwLoaderChangeRefused(LoaderChange change, StringView changed)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(realm, scope, createLoaderChangeRefusedError(realm, change, changed));
}

// The host's require() takes no entry that is falsy.
void Modules::willSetRequireCacheEntry(ModuleID module, JSValue entry)
{
    if (entry.toBoolean(m_instance.globalObject))
        throwLoaderChangeRefused(LoaderChange::ReplaceInRequireCache, m_table.pathText(module));
}

static ASCIILiteral nameOf(RequireOverride location)
{
    switch (location) {
    case RequireOverride::ResolveFilename:
        return "Module._resolveFilename"_s;
    case RequireOverride::RequireFunction:
        return "Module.prototype.require"_s;
    case RequireOverride::Extensions:
        return "a loader in Module._extensions"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A request that is resolved at run time asks the two functions, as under plain Bun. A `require` with a constant name is the module that the build has found, and asks nobody.
// A loader for an extension would have to be handed the text of a module of the image, which is not there.
void Modules::willChangeRequireOverride(RequireOverride location, bool isOverridden)
{
    if (isOverridden && location == RequireOverride::Extensions)
        throwLoaderChangeRefused(LoaderChange::ReplaceLoaderFunction, nameOf(location));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
