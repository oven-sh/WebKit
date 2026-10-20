/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTObjectModel.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static std::span<uint8_t> requireEdgeStates(Instance& instance, const ModuleTable& table)
{
    return unsafeMakeSpan(instance.modules.requireEdges, table.numberOfRequireEdges());
}

template<typename LoadedModule>
static void setModuleBuiltins(LoadedModule& loaded, JSObject* requirer, const CommonJSModuleObjects& objects)
{
    loaded.requirer = requirer;
    loaded.requireFunction = objects.require;
    loaded.initialExports = objects.exports;
}

void Modules::armRequireEdge(uint32_t edge)
{
    requireEdgeStates(m_instance, m_table)[edge] = static_cast<uint8_t>(RequireEdgeState::Ready);
}

void Modules::disarmRequireEdges()
{
    zeroSpan(requireEdgeStates(m_instance, m_table));
}

JSValue Modules::requireFrom(ModuleID target, ModuleID referrer)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<uint32_t> edge = m_table.findRequireEdge(referrer, target);
    auto isAnsweredByHost = [&] {
        LoadedModule* loaded = loadedIfExists(target);
        return requireIsOverridden() || (loaded && loaded->isOverriddenInRequireCache);
    };

    if (isAnsweredByHost()) [[unlikely]] {
        std::optional<StringID> writtenSpecifier = requireIsOverridden() && edge ? m_table.requireEdge(*edge).specifierString() : std::nullopt;
        JSString* specifier = writtenSpecifier ? m_state.strings->string(vm, *writtenSpecifier) : pathString(target);
        RELEASE_AND_RETURN(scope, requireThroughHost(specifier, referrer));
    }

    JSObject* parent = nullptr;
    if (data(target).kind == ModuleKind::CommonJs) {
        parent = moduleObject(referrer);
        RETURN_IF_EXCEPTION(scope, { });
    }

    ChildLink link = ChildLink::Recorded;
    JSValue result = requireWithLink(target, parent, nullptr, link);
    RETURN_IF_EXCEPTION(scope, { });
    if (edge && link == ChildLink::Recorded && status(target) == ModuleStatus::Evaluated && !isAnsweredByHost())
        armRequireEdge(*edge);
    return result;
}

JSValue Modules::require(ModuleID target, JSObject* parent, JSObject* adoptedModule)
{
    ChildLink link = ChildLink::Recorded;
    return requireWithLink(target, parent, adoptedModule, link);
}

JSValue Modules::requireWithLink(ModuleID target, JSObject* parent, JSObject* adoptedModule, ChildLink& link)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& targetData = data(target);

    JSValue executed;
    switch (targetData.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        if (status(target) != ModuleStatus::Evaluated) {
            evaluateSynchronously(target, SynchronousEvaluationReason::Require);
            RETURN_IF_EXCEPTION(scope, { });
        }
        break;
    case ModuleKind::CommonJs:
        executed = executeCommonJS(target, parent, adoptedModule, link);
        RETURN_IF_EXCEPTION(scope, { });
        break;
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

    switch (targetData.requireResult) {
    case RequireResult::ModuleExports:
        RELEASE_ASSERT(executed);
        return executed;
    case RequireResult::DefaultExport: {
        if (!targetData.hasExportTable())
            return executed;
        if (JSValue cached = value(target))
            return cached;
        std::optional<unsigned> index = m_table.findExport(target, vm.propertyNames->defaultKeyword.impl());
        RELEASE_ASSERT(index);
        JSValue defaultExport = readExport(target, *index);
        RETURN_IF_EXCEPTION(scope, { });
        setValue(target, defaultExport);
        return defaultExport;
    }
    case RequireResult::Namespace:
        RELEASE_AND_RETURN(scope, namespaceForRequire(target));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ChildLink Modules::addChild(JSObject* parent, JSObject* child)
{
    return host().addChild(m_instance.globalObject, parent, child);
}

JSValue Modules::executeCommonJS(ModuleID module, JSObject* parent, JSObject* adoptedModule, ChildLink& link)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool shouldAddChild = parent && !adoptedModule;
    link = ChildLink::Recorded;

    bool isCached = false;
    switch (status(module)) {
    case ModuleStatus::Evaluated:
        isCached = true;
        break;
    case ModuleStatus::Evaluating:
        isCached = !ensureLoaded(module).wasEvictedWhileEvaluating;
        break;
    case ModuleStatus::Failed:
        recordIfExists(module)->setEvaluationError(vm, JSValue());
        setStatus(module, ModuleStatus::Linked);
        break;
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        break;
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (isCached) {
        if (shouldAddChild) {
            JSObject* child = value(module).getObject();
            link = addChild(parent, child);
            RETURN_IF_EXCEPTION(scope, { });
            if (link == ChildLink::ChildrenAreObservable) {
                host().pushObservableChild(realm, parent, child);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        RELEASE_AND_RETURN(scope, commonJSExports(module));
    }

    JSString* path = pathString(module);
    CommonJSModuleObjects objects;
    if (adoptedModule)
        adoptCommonJSModule(module, adoptedModule, objects);
    else
        host().createCommonJSModule(realm, path, parent, objects);
    RETURN_IF_EXCEPTION(scope, { });

    LoadedModule& loaded = ensureLoaded(module);
    // The edges that an earlier run armed recorded their children on the module object of that run.
    if (loaded.requireFunction)
        disarmRequireEdges();
    setValue(module, objects.module);
    setModuleBuiltins(loaded, nullptr, objects);
    loaded.wasEvictedWhileEvaluating = false;
    setStatus(module, ModuleStatus::Evaluating);

    if (shouldAddChild)
        link = addChild(parent, objects.module);
    if (!scope.exception()) [[likely]]
        callCommonJSBody(module, objects);

    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        // After an eviction, a require() inside the body runs the body again, and that run owns the state from then on.
        if (value(module) == JSValue(objects.module)) {
            if (loaded.wasEvictedWhileEvaluating) {
                setValue(module, JSValue());
                setModuleBuiltins(loaded, nullptr, { });
                setStatus(module, ModuleStatus::Linked);
            } else
                setStatus(module, ModuleStatus::Evaluated);
        }
        // The body may have read the children of its parent, so the caller gets what the next require() would find.
        if (link == ChildLink::ChildrenAreObservable)
            host().pushObservableChild(realm, parent, objects.module);
        else if (shouldAddChild)
            link = addChild(parent, objects.module);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, objects.module->get(realm, vm.propertyNames->exports));
    }

    setValue(module, JSValue());
    setModuleBuiltins(loaded, nullptr, { });
    setStatus(module, ModuleStatus::Linked);
    disarmRequireEdges();
    if (adoptedModule)
        return { };
    TRY_CLEAR_EXCEPTION(scope, { });
    host().removeFromRequireCache(realm, path);
    TRY_CLEAR_EXCEPTION(scope, { });
    throwException(realm, scope, exception);
    return { };
}

void Modules::adoptCommonJSModule(ModuleID module, JSObject* commonJSModule, CommonJSModuleObjects& objects)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    ModuleAdoption adoption = host().adoptCommonJSModule(realm, pathString(module), commonJSModule, objects);
    RETURN_IF_EXCEPTION(scope, void());
    switch (adoption) {
    case ModuleAdoption::Adopted:
        return;
    case ModuleAdoption::Rejected:
        throwTypeError(realm, scope, withAOTErrorNote(makeString("The module "_s, m_table.path(module), " was compiled ahead of time and runs only with a module object and an exports object that the module loader created"_s)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Compiled code reads the module object, its exports and its require function from cells of the instance, and each run has its own.
void Modules::resetBeforeNextRun(ModuleID module)
{
    for (const ModuleResetSlotData& entry : m_table.resetSlots(module))
        ProgramStatics::writeVariable(m_instance, entry.slot, JSValue());
}

void Modules::callCommonJSBody(ModuleID module, const CommonJSModuleObjects& objects)
{
    RELEASE_ASSERT(objects.filename.isString() && objects.dirname.isString());
    if (std::exchange(ensureLoaded(module).hasEnteredBody, true))
        resetBeforeNextRun(module);
    MarkedArgumentBuffer arguments;
    for (CommonJsParameter parameter : allMembersOf<CommonJsParameter>) {
        switch (parameter) {
        case CommonJsParameter::Exports:
            arguments.append(objects.exports);
            break;
        case CommonJsParameter::Require:
            arguments.append(objects.require);
            break;
        case CommonJsParameter::Module:
            arguments.append(objects.module);
            break;
        case CommonJsParameter::Filename:
            arguments.append(objects.filename);
            break;
        case CommonJsParameter::Dirname:
            arguments.append(objects.dirname);
            break;
        }
    }
    callBody(module, objects.exports, arguments);
}

void Modules::evaluateCommonJSBody(ModuleID module, JSObject* commonJSModule)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    RELEASE_ASSERT(data(module).kind == ModuleKind::CommonJs && commonJSModule);

    CommonJSModuleObjects objects;
    adoptCommonJSModule(module, commonJSModule, objects);
    RETURN_IF_EXCEPTION(scope, void());

    LoadedModule& loaded = ensureLoaded(module);
    CommonJSModuleObjects previous { .module = loaded.requirer, .exports = loaded.initialExports, .require = loaded.requireFunction };
    setModuleBuiltins(loaded, objects.module, objects);
    disarmRequireEdges();
    scope.release();
    callCommonJSBody(module, objects);
    // A require() inside the body may have run the body for require.cache, and that run owns the state from then on.
    if (loaded.requirer == objects.module) {
        setModuleBuiltins(loaded, previous.module, previous);
        disarmRequireEdges();
    }
}

JSValue Modules::commonJSExports(ModuleID module)
{
    JSObject* commonJSModule = value(module).getObject();
    ASSERT(commonJSModule);
    return commonJSModule->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

JSObject* Modules::moduleObject(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    LoadedModule* existing = loadedIfExists(module);
    if (existing && existing->requirer)
        return existing->requirer;
    if (data(module).kind == ModuleKind::CommonJs) {
        if (JSValue commonJSModule = value(module))
            return asObject(commonJSModule);
    }

    JSObject* meta = importMeta(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    CommonJSModuleObjects objects;
    host().createRequirer(m_instance.globalObject, meta, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);

    setModuleBuiltins(ensureLoaded(module), objects.module, objects);
    return objects.module;
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
        holder = moduleObject(referrer);
        break;
    case ResolvedSpecifierForm::URL:
        holder = importMeta(referrer);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, host().resolveSpecifier(m_instance.globalObject, holder, specifier, form));
}

JSValue Modules::resolveFrom(ModuleID target, ModuleID referrer, StringID writtenSpecifier, ResolvedSpecifierForm form)
{
    if (requireIsOverridden() && form == ResolvedSpecifierForm::Path) [[unlikely]]
        return resolveDynamic(m_state.strings->string(*m_instance.vm, writtenSpecifier), referrer, form);
    return resolve(target, form);
}

JSValue Modules::namespaceForRequire(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (JSValue cached = value(module))
        return cached;

    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    if (m_moduleExportsName.isNull())
        m_moduleExportsName = Identifier::fromString(vm, "module.exports"_s);

    const Identifier& esModuleName = vm.propertyNames->__esModule;
    JSValue esModule = readExportForRequire(namespaceObject, esModuleName);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue moduleExports;
    if (esModule) {
        moduleExports = readExportForRequire(namespaceObject, m_moduleExportsName);
        RETURN_IF_EXCEPTION(scope, { });
    }

    auto* compiledNamespace = uncheckedDowncast<ModuleNamespace>(namespaceObject);
    if (!compiledNamespace->hasExport(esModuleName.impl()))
        compiledNamespace->setESModuleMarker(true);

    JSValue result = !moduleExports || moduleExports.isUndefinedOrNull() ? JSValue(namespaceObject) : moduleExports;
    setValue(module, result);
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

void Modules::throwLoaderChangeRefused(const String& change)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(realm, scope, createError(realm, withAOTErrorNote(makeString("Cannot "_s, change, ": this executable was compiled without support for changing at run time what a module name loads. Compile with --admit=runtime-code"_s))));
}

void Modules::evict(ModuleID module)
{
    if (data(module).isEvaluatedAtMostOnce()) {
        throwLoaderChangeRefused(makeString("remove '"_s, m_table.path(module), "' from require.cache"_s));
        return;
    }

    LoadedModule* loaded = loadedIfExists(module);
    if (loaded)
        loaded->isOverriddenInRequireCache = false;

    switch (data(module).kind) {
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        break;
    case ModuleKind::SourceText:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return;
    }

    switch (status(module)) {
    case ModuleStatus::Evaluated:
    case ModuleStatus::Failed:
        disarmRequireEdges();
        setValue(module, JSValue());
        if (loaded && !loaded->requirer)
            setModuleBuiltins(*loaded, nullptr, { });
        if (ModuleRecord* record = recordIfExists(module))
            record->setEvaluationError(*m_instance.vm, JSValue());
        setStatus(module, ModuleStatus::Linked);
        return;
    case ModuleStatus::Evaluating:
        ensureLoaded(module).wasEvictedWhileEvaluating = true;
        return;
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linking:
    case ModuleStatus::Linked:
    case ModuleStatus::EvaluatingAsync:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::willSetRequireCacheEntry(ModuleID module, JSValue entry)
{
    // The host's require() loads the module again when the entry is falsy.
    if (!entry.toBoolean(m_instance.globalObject)) {
        evict(module);
        return;
    }
    bool isOverridden = entry != value(module);
    LoadedModule* loaded = loadedIfExists(module);
    if (isOverridden == (loaded && loaded->isOverriddenInRequireCache))
        return;
    if (isOverridden && !m_replacesLoaderAnswers) {
        throwLoaderChangeRefused(makeString("replace '"_s, m_table.path(module), "' in require.cache"_s));
        return;
    }
    ensureLoaded(module).isOverriddenInRequireCache = isOverridden;
    disarmRequireEdges();
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

void Modules::willChangeRequireOverride(RequireOverride location, bool isOverridden)
{
    if (isOverridden && !m_replacesLoaderAnswers) {
        throwLoaderChangeRefused(makeString("replace "_s, nameOf(location)));
        return;
    }
    m_requireOverrides.set(location, isOverridden);
    if (isOverridden)
        disarmRequireEdges();
}

void Modules::didObserveModuleChildren()
{
    disarmRequireEdges();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
