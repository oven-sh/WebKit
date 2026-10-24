/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleHost.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTImage.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTModuleTable.h"
#include "AOTModules.h"
#include "AOTRuntimeReporter.h"
#include "Error.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include <array>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
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

namespace {

struct PathsNotInImage {
    Lock lock;
    std::array<std::pair<const Image*, String>, 32> paths WTF_GUARDED_BY_LOCK(lock);
};

// Static strings: every thread may hold them.
struct PathsOfModules {
    Lock lock;
    UncheckedKeyHashMap<uint64_t, StringImpl*> byImageAndModule WTF_GUARDED_BY_LOCK(lock);
};

} // anonymous namespace

std::optional<ModuleID> findModule(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return std::nullopt;
    static NeverDestroyed<PathsNotInImage> pathsNotInImage;
    PathsNotInImage& notFound = pathsNotInImage.get();
    const Image* image = Image::current();
    size_t place = publicPath.hash() % notFound.paths.size();
    {
        Locker locker { notFound.lock };
        if (notFound.paths[place].first == image && notFound.paths[place].second == publicPath)
            return std::nullopt;
    }
    std::optional<ModuleID> found = table->findByPath(publicPath);
    if (!found) {
        String copy = publicPath.toString().isolatedCopy();
        Locker locker { notFound.lock };
        notFound.paths[place] = { image, WTF::move(copy) };
    }
    return found;
}

// Each frame of each stack trace asks.
String modulePath(ModuleID module)
{
    static NeverDestroyed<PathsOfModules> pathsOfModules;
    PathsOfModules& paths = pathsOfModules.get();
    Locker locker { paths.lock };
    uint64_t key = static_cast<uint64_t>(Image::current()->number()) << 32 | (module.index() + 1);
    return paths.byImageAndModule.ensure(key, [&] {
        std::optional<ModuleTable> table = currentModuleTable();
        RELEASE_ASSERT(table);
        String path = table->pathText(module);
        return &(path.is8Bit() ? StringImpl::createStaticStringImpl(path.span8()) : StringImpl::createStaticStringImpl(path.span16())).leakRef();
    }).iterator->value;
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
    // The build folds what loads code at run time out of the host's modules unless it admits the inspector or names an external module.
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::Inspector);
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

bool refusesHostObjectsOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return refusesCodeOutsideImage(realm) && !(modules && modules->isAskingHost());
}

JSObject* mainModuleObject(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && Image::current()->showsWaysBetweenModuleObjects() ? modules->mainModuleObject() : nullptr;
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->commonJSExports(module) : jsUndefined();
}

void setModuleExports(JSGlobalObject* realm, ModuleID module, JSValue exports)
{
    if (Modules* modules = modulesOf(realm))
        modules->setCommonJSExports(module, exports);
}

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && Image::current()->showsWaysBetweenModuleObjects() ? modules->parentModuleObject(module) : nullptr;
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    Modules* modules = modulesOf(realm);
    return modules && Image::current()->showsWaysBetweenModuleObjects() && modules->forEachNewChild(module, functor);
}

bool recordRequire(JSGlobalObject* realm, ModuleID target, ModuleID referrer)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    modules->recordRequire(target, referrer);
    return modules->data(target).kind == ModuleKind::CommonJs;
}

void willLoadByName(JSGlobalObject* realm, JSValue specifier, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    // Code from outside the image has a loader of its own: its names pass, and one that leads to a file of the image is refused there.
    if (modules ? modules->refusesLoadsByName() && modules->refusesCodeOutsideImage() : !Image::current()->loadsModulesByName()) {
        String name = specifier.isString() ? asString(specifier)->value(realm) : String();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, throwLoadByNameRefused(realm, name, form));
    }
    if (modules)
        RELEASE_AND_RETURN(scope, modules->admitCodeNamedBy(specifier));
}

static Modules* modulesOrThrow(JSGlobalObject* realm, ModuleID module, LoadByName form = LoadByName::Import)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->refusesRequestFromHost(module)) [[likely]]
        return modules;
    if (modules) {
        throwLoadByNameRefused(realm, modulePath(module), form);
        return nullptr;
    }
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
#if ENABLE(SHADOW_REALM)
    throwException(realm, scope, createModuleInShadowRealmError(realm, modulePath(module)));
#else
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
#endif
    return nullptr;
}

void willAnswerWithModule(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    modulesOrThrow(realm, module, form);
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

JSValue requireModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesForEvaluation(realm, module);
    return modules ? modules->require(module) : JSValue();
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

bool hasFileWithExtension(StringView extension)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->hasFileWithExtension(extension);
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
