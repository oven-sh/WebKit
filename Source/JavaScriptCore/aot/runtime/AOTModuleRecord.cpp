/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleRecord.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleStarExports.h"
#include "AOTModules.h"
#include "Error.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSPromise.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

static constexpr unsigned invalidIndex = static_cast<unsigned>(-1);

const ClassInfo ModuleRecord::s_info = { "CompiledModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleRecord) };

Structure* ModuleRecord::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

ModuleRecord::ModuleRecord(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
    : Base(*instance.vm, structure, loader, moduleKey, SourceProviderSourceType::Module)
    , m_instance(instance)
    , m_module(module)
{
}

ModuleRecord* ModuleRecord::create(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
{
    VM& vm = *instance.vm;
    auto* record = new (NotNull, allocateCell<ModuleRecord>(vm)) ModuleRecord(instance, structure, loader, moduleKey, module);
    record->finishCreation(instance.globalObject, vm);
    return record;
}

void ModuleRecord::finishCreation(JSGlobalObject* globalObject, VM& vm)
{
    Base::finishCreation(globalObject, vm);
    ASSERT(inherits(info()));
}

void ModuleRecord::destroy(JSCell* cell)
{
    static_cast<ModuleRecord*>(cell)->ModuleRecord::~ModuleRecord();
}

template<typename Visitor>
void ModuleRecord::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleRecord>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(ModuleRecord);

JSGlobalObject* ModuleRecord::instanceRealm() const
{
    return m_instance.globalObject;
}

Modules& ModuleRecord::modules() const
{
    return modulesOf(m_instance);
}

unsigned ModuleRecord::innerModuleEvaluation(JSGlobalObject* globalObject, ModuleEvaluationStack& stack, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    EvaluationContext context { stack, index, referrerAsyncOrder < 0 ? std::nullopt : std::optional(referrerAsyncOrder), dynamicImportPromise };
    modules().innerModuleEvaluation(m_module, context);
    RETURN_IF_EXCEPTION(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords& visited)
{
    modules().captureSyntheticExports(m_module, visited);
}

unsigned ModuleRecord::innerModuleLinking(JSGlobalObject* globalObject, ModuleLinkingStack& stack, unsigned index, RefPtr<ScriptFetcher> scriptFetcher)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    LinkContext context { stack, index, WTF::move(scriptFetcher) };
    modules().finishLink(m_module, context);
    RETURN_IF_EXCEPTION(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::execute(JSGlobalObject*, JSPromise* capability)
{
    RELEASE_ASSERT(!capability);
    modules().callBodyOfModuleEnteredByLoader(m_module);
}

void ModuleRecord::executeAsync(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(status() == Status::Evaluating || status() == Status::EvaluatingAsync);
    ASSERT(hasTLA());

    JSValue result = modules().callBodyOfModuleEnteredByLoader(m_module);
    RETURN_IF_EXCEPTION(scope, void());
    auto* promise = dynamicDowncast<JSPromise>(result);
    if (!promise) {
        promise = JSPromise::resolvedPromise(globalObject, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncModuleExecutionDone, nullptr, this);
}

static AbstractModuleRecord::Resolution absentExport(VM& vm, const Identifier& exportName)
{
    using Resolution = AbstractModuleRecord::Resolution;
    return exportName == vm.propertyNames->defaultKeyword ? Resolution::error() : Resolution::notFound();
}

auto ModuleRecord::resolveCapturedExport(JSGlobalObject* globalObject, const Identifier& exportName) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasExport = modules().hasCapturedExport(m_module, exportName);
    RETURN_IF_EXCEPTION(scope, Resolution::error());
    if (!hasExport)
        return absentExport(vm, exportName);
    return { Resolution::Type::Resolved, this, exportName };
}

auto ModuleRecord::resolveExportEntry(JSGlobalObject* globalObject, const Identifier& exportName, unsigned index) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleExportData& entry = modules.table().exports(m_module)[index];

    switch (entry.kind()) {
    case ModuleExportKind::Variable:
        return { Resolution::Type::Resolved, this, exportName, index };
    case ModuleExportKind::Namespace: {
        // A Resolution has no phase, and its readers take *namespace* as the namespace object of phase Evaluation.
        if (entry.isDeferred)
            return { Resolution::Type::Resolved, this, exportName, index };
        ModuleID target = entry.targetModule();
        if (modules.data(target).isUncompiledCode()) {
            AbstractModuleRecord* record = modules.uncompiledRecord(target);
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            return { Resolution::Type::Resolved, record, vm.propertyNames->starNamespacePrivateName };
        }
        ModuleRecord* record = modules.ensureRecord(target);
        record->retain();
        return { Resolution::Type::Resolved, record, vm.propertyNames->starNamespacePrivateName };
    }
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = modules.table().indirectExport(entry);
        AbstractModuleRecord* record = modules.uncompiledRecord(indirectExport.targetModule());
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            throwException(globalObject, scope, createModuleGraphTooDeepError(globalObject));
            return Resolution::error();
        }
        RELEASE_AND_RETURN(scope, record->resolveExport(globalObject, modules.identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto ModuleRecord::resolveExport(JSGlobalObject* globalObject, const Identifier& exportName) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);

    retain();
    if (data.capturesExports())
        RELEASE_AND_RETURN(scope, resolveCapturedExport(globalObject, exportName));

    // An uncompiled module starts another search with a resolveSet of its own, so a circular request is found here.
    if (m_resolveSet.contains(exportName.impl()))
        return Resolution::notFound();
    m_resolveSet.append(exportName.impl());
    auto removeFromResolveSet = makeScopeExit([&] {
        m_resolveSet.removeLast();
    });

    std::optional<unsigned> index = modules.table().findExport(m_module, exportName.impl());
    bool needsResolver = data.hasDynamicStarExport() && (!index || modules.table().exports(m_module)[*index].isFromStarExport);
    StarExportResolver resolver(modules);
    const StarCandidate* candidate = nullptr;
    if (needsResolver) {
        candidate = resolver.find(m_module, exportName.impl());
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (candidate && candidate->isAmbiguous)
            return Resolution::ambiguous();
    }

    if (index)
        RELEASE_AND_RETURN(scope, resolveExportEntry(globalObject, exportName, *index));
    if (!candidate)
        return absentExport(vm, exportName);
    if (auto* uncompiled = std::get_if<UncompiledBinding>(&candidate->binding))
        return { Resolution::Type::Resolved, uncompiled->record, Identifier::fromUid(vm, uncompiled->localName) };
    return { Resolution::Type::Resolved, this, exportName };
}

void ModuleRecord::collectExportedNames(JSGlobalObject* globalObject, Vector<Identifier>& names)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);

    if (!data.hasDynamicStarExport() && !data.capturesExports()) {
        for (const ModuleExportData& entry : modules.table().exports(m_module))
            names.append(modules.identifier(entry.name));
        return;
    }

    // An uncompiled module starts another walk with an exportStarSet of its own, so an export * cycle is found here.
    if (m_isInExportStarSet)
        return;
    SetForScope exportStarSetScope(m_isInExportStarSet, true);

    StarExportResolver resolver(modules);
    const StarCandidates* candidates = resolver.candidatesOf(m_module);
    RETURN_IF_EXCEPTION(scope, void());
    // An ambiguous name is listed, as in GetExportedNames: it is ambiguous in every module that has export * from this one.
    for (auto& name : candidates->keys())
        names.append(Identifier::fromUid(vm, name.get()));
}

JSValue ModuleRecord::readExport(JSGlobalObject* globalObject, const Identifier& exportName, std::optional<unsigned> exportIndex)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (exportIndex)
        RELEASE_AND_RETURN(scope, modules().readExport(m_module, *exportIndex));
    // The namespace object holds the same value, and to make it reads every deferred export.
    if (modules().data(m_module).capturesExports()) {
        JSValue value = modules().capturedValue(m_module, exportName);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    JSObject* namespaceObject = modules().moduleNamespace(m_module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, exportName));
}

JSObject* ModuleRecord::moduleNamespaceObject(JSGlobalObject*, ModulePhase phase)
{
    return modules().moduleNamespace(m_module, phase);
}

static std::optional<StarBinding> bindingOf(Modules& modules, StarExportResolver& resolver, const AbstractModuleRecord::Resolution& resolution)
{
    ModuleID module = uncheckedDowncast<ModuleRecord>(resolution.moduleRecord)->module();
    if (!resolution.compiledExportIndex) {
        const StarCandidate* candidate = resolver.find(module, resolution.localName.impl());
        return candidate ? std::optional(candidate->binding) : std::nullopt;
    }
    return resolver.bindingOf(modules.table().exports(module)[*resolution.compiledExportIndex]);
}

bool ModuleRecord::isSameBinding(const Resolution& first, const Resolution& second)
{
    auto* firstRecord = uncheckedDowncast<ModuleRecord>(first.moduleRecord);
    auto* secondRecord = uncheckedDowncast<ModuleRecord>(second.moduleRecord);
    VM& vm = firstRecord->vm();
    // Resolution::isSameBinding() has no caller that checks for an exception.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    const Identifier& starNamespace = vm.propertyNames->starNamespacePrivateName;
    if (first.localName == starNamespace || second.localName == starNamespace)
        return false;
    if (&firstRecord->m_instance != &secondRecord->m_instance)
        return false;

    Modules& modules = firstRecord->modules();
    StarExportResolver resolver(modules);
    std::optional<StarBinding> firstBinding = bindingOf(modules, resolver, first);
    if (!scope.exception()) [[likely]] {
        std::optional<StarBinding> secondBinding = bindingOf(modules, resolver, second);
        if (!scope.exception()) [[likely]]
            return firstBinding && secondBinding && *firstBinding == *secondBinding;
    }
    scope.clearExceptionExceptTermination();
    return false;
}

void ModuleRecord::gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen)
{
    modules().gatherAsynchronousTransitiveDependencies(m_module, result, seen);
}

bool ModuleRecord::readyForSyncExecution(VisitedModuleRecords& seen)
{
    return modules().readyForSyncExecution(m_module, seen);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
