/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "Error.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "ModuleRegistryEntry.h"
#include "ScriptFetchParameters.h"
#include <wtf/OrderedHashSet.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

const Identifier& Modules::resolvedKey(ModuleID external)
{
    ASSERT(data(external).isUncompiledCode());
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_admitsCodeOutsideImage = true;
    LoadedModule& loaded = ensureLoaded(external);
    if (!loaded.resolvedKey.isNull())
        return loaded.resolvedKey;
    Identifier key = loader()->resolve(realm, pathString(external), pathString(*data(external).referrerModule()), nullptr, true);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    loaded.resolvedKey = WTF::move(key);
    return loaded.resolvedKey;
}

void Modules::admitCodeNamedBy(JSValue specifier)
{
    if (m_admitsCodeOutsideImage || !specifier.isString())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto text = asString(specifier)->view(m_instance.globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    // A large program asks for every name that it loads at run time, and has thousands of modules, few or none of them outside the image: the build has listed those.
    m_admitsCodeOutsideImage = std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](uint32_t module) {
        String name = m_table.pathText(ModuleID(module));
        return text == name || (text->startsWith(name) && text[name.length()] == '/');
    });
}

void Modules::appendUncompiledRequests(ModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    OrderedHashSet<UniquedStringImpl*> keys;
    ModuleRequestWalk walk(m_table, record->module());
    for (ModuleRequestWalk::Step step = walk.next(); step.kind != ModuleRequestWalk::StepKind::Done; step = walk.next()) {
        if (step.kind == ModuleRequestWalk::StepKind::Leave)
            continue;
        ModuleID target = step.request->targetModule();
        const ModuleData& targetData = data(target);
        if (!targetData.reachesUncompiledCode())
            continue;
        if (targetData.isUncompiledCode()) {
            const Identifier& key = resolvedKey(target);
            RETURN_IF_EXCEPTION(scope, void());
            keys.add(key.impl());
            continue;
        }
        if (!walk.hasVisited(target) && status(target) == ModuleStatus::Unlinked)
            walk.enter(target);
    }
    for (UniquedStringImpl* key : keys)
        record->appendRequestedModule(Identifier::fromUid(vm, key), nullptr, ModulePhase::Evaluation);
}

ModuleRecord* Modules::exposeRecord(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    loadRuntimeModules(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    if (record->status() != CyclicModuleRecord::Status::New || !record->requestedModules().isEmpty())
        return record;
    appendUncompiledRequests(record);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return record;
}

ModuleRecord* Modules::registerWithLoader(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleRecord* record = exposeRecord(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = loader()->ensureRegistered(m_instance.globalObject, record->moduleKey(), ScriptFetchParameters::Type::JavaScript);
    if (!entry->record() && entry->status() == ModuleRegistryEntry::Status::New)
        entry->provideModule(vm, record);
    return record;
}

JSPromise* Modules::importThroughLoader(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = registerWithLoader(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    JSPromise* promise = loader()->requestImportModuleForKey(realm, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1));
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    return promise;
}

JSPromise* Modules::evaluateThroughLoaderSynchronously(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = registerWithLoader(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, loader()->loadModuleSync(m_instance.globalObject, record->moduleKey(), nullptr, nullptr));
}

AbstractModuleRecord* Modules::uncompiledRecord(ModuleID external)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(external);
    if (loaded.uncompiledRecord)
        return loaded.uncompiledRecord;
    const Identifier& key = resolvedKey(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = loader()->registryEntry(key);
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    if (!record) {
        throwException(m_instance.globalObject, scope, createExternalModuleNotLoadedError(m_instance.globalObject, m_table.pathText(external)));
        return nullptr;
    }
    loaded.uncompiledRecord.set(*this, record);
    return record;
}

JSObject* Modules::uncompiledNamespace(ModuleID external, ModulePhase phase)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    AbstractModuleRecord* record = uncompiledRecord(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, record->moduleNamespaceObject(m_instance.globalObject, phase));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
