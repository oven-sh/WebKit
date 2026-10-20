/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleHost.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTModuleTable.h"
#include "AOTModules.h"
#include "AOTRuntimeReporter.h"
#include "Error.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

const ModuleHost& moduleHostOf(JSGlobalObject* realm)
{
    const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost;
    RELEASE_ASSERT(host);
    return *host;
}

static std::optional<ModuleTable> currentModuleTable()
{
    const Image* image = Image::current();
    if (!image)
        return std::nullopt;
    return ModuleTable(image->programTables());
}

std::optional<ModuleID> findModule(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return std::nullopt;
    return table->findByPath(publicPath);
}

StringView modulePath(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    RELEASE_ASSERT(table);
    return table->path(module);
}

bool isCommonJS(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    RELEASE_ASSERT(table);
    return table->data(module).kind == ModuleKind::CommonJs;
}

bool entryPointIsCommonJS(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    std::optional<ModuleID> entryPoint = modules->firstModuleRequestedByHost();
    return entryPoint && modules->data(*entryPoint).kind == ModuleKind::CommonJs;
}

void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>& functor)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return;
    for (unsigned index = 0; index < table->size(); ++index) {
        ModuleID module(index);
        if (table->hasFilePath(module) && functor(module) == IterationStatus::Done)
            return;
    }
}

bool refusesCodeOutsideImage(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        return modules->refusesCodeOutsideImage();
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::RuntimeCode);
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createError(realm, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable was compiled without support for loading a module by a name that is computed at run time. Compile with --admit=runtime-code"_s))));
}

void willLoadByName(JSGlobalObject* realm, JSValue specifier)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    // Code from outside the image has a loader of its own: its names pass, and one that leads to a file of the image is refused there.
    if (modules ? modules->refusesLoadsByName() && modules->refusesCodeOutsideImage() : !Image::current()->loadsModulesByName()) {
        String name = specifier.isString() ? asString(specifier)->value(realm) : String();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, throwLoadByNameRefused(realm, name));
    }
    if (modules)
        RELEASE_AND_RETURN(scope, modules->admitCodeNamedBy(specifier));
}

static Modules* modulesOrThrow(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->refusesRequestFromHost(module)) [[likely]]
        return modules;
    if (modules) {
        throwLoadByNameRefused(realm, modulePath(module));
        return nullptr;
    }
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwSyntaxError(realm, scope, withAOTErrorNote(makeString("The module "_s, modulePath(module), " was compiled ahead of time and cannot be loaded in a realm that was created at run time (ShadowRealm)"_s)));
    return nullptr;
}

static bool s_isVerbose;

void setVerbose(bool isVerbose)
{
    s_isVerbose = isVerbose;
}

static void logEvaluation(ModuleID module)
{
    if (s_isVerbose) [[unlikely]]
        writeToStandardError(makeString("[aot] module: "_s, modulePath(module), '\n'));
}

static bool hasStartedEvaluation(ModuleStatus status)
{
    switch (status) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linking:
    case ModuleStatus::Linked:
        return false;
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Evaluated:
    case ModuleStatus::Failed:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Modules* modulesForEvaluation(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    if (!modules)
        return nullptr;
    modules->didRequestFromHost(module);
    if (!hasStartedEvaluation(modules->status(module)))
        logEvaluation(module);
    return modules;
}

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->exposeRecord(module) : nullptr;
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->importFromHost(module, referrerAsyncOrder) : nullptr;
}

void evaluateModuleSynchronously(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesForEvaluation(realm, module))
        modules->evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}

JSObject* moduleNamespace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    return modules ? modules->moduleNamespace(module, Modules::ModulePhase::Evaluation) : nullptr;
}

JSValue requireModule(JSGlobalObject* realm, ModuleID module, JSObject* commonJSModule)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->require(module, nullptr, commonJSModule) : JSValue();
}

void evaluateCommonJSBody(JSGlobalObject* realm, ModuleID module, JSObject* commonJSModule)
{
    Modules* modules = modulesOrThrow(realm, module);
    if (!modules)
        return;
    logEvaluation(module);
    modules->evaluateCommonJSBody(module, commonJSModule);
}

static bool isEvaluated(const Modules& modules, ModuleID module)
{
    return modules.status(module) == ModuleStatus::Evaluated && modules.table().hasFilePath(module);
}

bool isEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && isEvaluated(*modules, module);
}

JSObject* namespaceIfEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).kind != ModuleKind::SourceText || modules->refusesRequestFromHost(module))
        return nullptr;
    if (modules->status(module) != ModuleStatus::Evaluated)
        return nullptr;
    return modules->moduleNamespace(module, Modules::ModulePhase::Evaluation);
}

void forEachEvaluatedModule(JSGlobalObject* realm, const ScopedLambda<void(ModuleID)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    unsigned numberOfModules = modules->table().size();
    for (unsigned index = 0; index < numberOfModules; ++index) {
        ModuleID module(index);
        if (isEvaluated(*modules, module))
            functor(module);
    }
}

void evictModule(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->evict(module);
}

void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID module, JSValue entry)
{
    if (Modules* modules = modulesOf(realm))
        modules->willSetRequireCacheEntry(module, entry);
}

void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    if (Modules* modules = modulesOf(realm))
        modules->willChangeRequireOverride(location, isOverridden);
}

void didObserveModuleChildren(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->didObserveModuleChildren();
}

bool isModuleNamespace(JSCell* cell)
{
    return cell->inherits<ModuleNamespace>();
}

bool hasESModuleMarker(JSCell* moduleNamespace)
{
    return uncheckedDowncast<ModuleNamespace>(moduleNamespace)->hasESModuleMarker();
}

void setESModuleMarker(JSCell* moduleNamespace, bool hasMarker)
{
    uncheckedDowncast<ModuleNamespace>(moduleNamespace)->setESModuleMarker(hasMarker);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
