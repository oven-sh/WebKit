/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTCompiledCall.h"
#include "AOTEffectsNet.h"
#include "AOTEnvironment.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotAccess.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "DeferGCInlines.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "ModuleRegistryEntry.h"
#include "ObjectConstructor.h"
#include "ProxyObject.h"
#include "PropertyNameArray.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static AbstractModuleRecord::ModulePhase phaseOfNamespaceExport(const ModuleExportData& moduleExport)
{
    return moduleExport.isDeferred ? AbstractModuleRecord::ModulePhase::Defer : AbstractModuleRecord::ModulePhase::Evaluation;
}

Environment* Modules::currentInstance(ModuleID module) const
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return nullptr;
    std::optional<SlotData> guard = m_table.instanceGuard(module);
    if (!guard || !ProgramStatics::readVariableWithoutAllocating(m_instance, *guard).isTrue())
        return nullptr;
    return uncheckedDowncast<Environment>(ProgramStatics::readVariableWithoutAllocating(m_instance, *m_table.instanceVariable(module)).asCell());
}

Environment* Modules::lastInstance(ModuleID module) const
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return nullptr;
    std::optional<SlotData> guard = m_table.instanceGuard(module);
    if (!guard || ProgramStatics::readVariableWithoutAllocating(m_instance, *guard).isUndefined())
        return nullptr;
    return uncheckedDowncast<Environment>(ProgramStatics::readVariableWithoutAllocating(m_instance, *m_table.instanceVariable(module)).asCell());
}

void Modules::callInstanceAllocator(ModuleID module)
{
    callCompiledCode(m_instance.globalObject, ProgramStatics::readVariable(m_instance, *m_table.instanceAllocator(module)).asCell(), jsUndefined(), ArgList());
}

Environment* Modules::ensureInstance(ModuleID module)
{
    if (Environment* current = currentInstance(module))
        return current;
    if (!hasInstances(module))
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ASSERT(!m_linkedInstances.get(module.index()));
    didChangeLinkState();
    if (lastInstance(module)) {
        ProgramStatics::writeVariable(m_instance, *m_table.severalInstancesVariable(module), jsBoolean(true));
        reportImportersWithoutInstances(module);
    }
    callInstanceAllocator(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return currentInstance(module);
}

bool Modules::isLinkedInstance(ModuleID module, const Environment* instance) const
{
    return instance && (instance != currentInstance(module) || m_linkedInstances.get(module.index()));
}

JSValue Modules::readField(const Environment* instance, SlotData field) const
{
    return instance ? readSlot(field.representation(), &instance->at<uint8_t>(field.offset), m_instance) : JSValue();
}

void Modules::writeField(Environment* instance, SlotData field, JSValue value)
{
    if (readField(instance, field) != value)
        m_state.countWriteForEffectsNet(Typed::CountInEffectsNet::WordsOfStaticsWritten);
    bool didWrite = tryWriteSlot(m_instance, field.representation(), &instance->at<uint8_t>(field.offset), value);
    RELEASE_ASSERT(didWrite);
    m_instance.vm->writeBarrier(instance, value);
}

Environment* Modules::readLink(const Environment* instance, SlotData link) const
{
    JSValue linked = readField(instance, link);
    return linked && linked.isCell() ? uncheckedDowncast<Environment>(linked.asCell()) : nullptr;
}

const ModuleLinkData* Modules::findModuleLink(ModuleID module, ModuleID target) const
{
    ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
    std::span<const ModuleLinkData> links = m_table.moduleLinks(module);
    auto found = std::ranges::partition_point(links, [&](const ModuleLinkData& link) {
        return !link.isResolved && link.target < file.index();
    });
    return found != links.end() && !found->isResolved && found->targetModule() == file ? &*found : nullptr;
}

ModuleNamespace* Modules::mockWord(const ModuleLinkData& link, const Environment* instance) const
{
    JSValue word = link.hasMock ? readField(instance, link.mock) : JSValue();
    auto* object = word ? dynamicDowncast<ModuleNamespace>(word) : nullptr;
    return object && object->linkedExports() ? object : nullptr;
}

bool Modules::isMockPending(ModuleID file) const
{
    return m_modulesWithPendingMock.get(file.index()) || m_modulesWithRunningFactory.get(file.index()) || hasPendingMock(file);
}

bool Modules::isReplacedByMock(ModuleID root) const
{
    return (isMocked(root) && !isPatchedByMock(root)) || activeMock(root) || isMockPending(m_table.moduleBehindFacade(root).value_or(root));
}

bool Modules::isMockSeenByLoad(ModuleID target, ModuleID importer) const
{
    ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
    if (m_table.moduleBehindFacade(importer) == std::optional(file))
        return false;
    bool hasActiveMock = activeMock(file) && !hasRemovedMock(file);
    bool isPending = isMockPending(file);
    if (!hasActiveMock && !isPending)
        return false;
    if (isPending && std::ranges::contains(m_factoryLoadedImporters, std::pair { importer, file }))
        return false;
    if (!isMocked(file) && isPending && isOfPackage(importer) && host().shouldBypassMock && data(file).isESModuleOfProgram() && m_table.hasFilePath(file))
        return !host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(file), m_table.fullPath(importer), true);
    return !importsOriginalDespiteMock(target, importer);
}

static JSC_DECLARE_HOST_FUNCTION(readUninitializedMock);
static JSC_DECLARE_HOST_FUNCTION(uninitializedMockHasName);

JSC_DEFINE_HOST_FUNCTION(readUninitializedMock, (JSGlobalObject* realm, CallFrame* callFrame))
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (callFrame->argument(1).isSymbol())
        return JSValue::encode(jsUndefined());
    String name = callFrame->argument(1).toWTFString(realm);
    RETURN_IF_EXCEPTION(scope, { });
    return throwVMError(realm, scope, createTDZError(realm, name));
}

JSC_DEFINE_HOST_FUNCTION(uninitializedMockHasName, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(!callFrame->argument(1).isSymbol()));
}

void Modules::ensureUninitializedMock()
{
    if (m_uninitializedMock)
        return;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* handler = constructEmptyObject(realm);
    handler->putDirect(vm, vm.propertyNames->get, JSFunction::create(vm, realm, 3, String(), readUninitializedMock, ImplementationVisibility::Private));
    handler->putDirect(vm, Identifier::fromString(vm, "has"_s), JSFunction::create(vm, realm, 2, String(), uninitializedMockHasName, ImplementationVisibility::Private));
    m_uninitializedMock.set(vm, ProxyObject::create(realm, constructEmptyObject(realm), handler));
    scope.assertNoException();
}

ModuleNamespace* Modules::mockNamespaceForLoad(ModuleID target)
{
    VM& vm = *m_instance.vm;
    ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
    ModuleID module = moduleWithExportsOf(file);
    LoadedModule& held = ensureLoaded(file);
    bool isPending = isMockPending(file) && !(activeMock(file) && isMockLingering(file));
    JSObject* standing = isPending ? nullptr : activeMock(file);
    Reference<JSObject*>& stored = isPending ? held.pendingMockWord : held.mockWord;
    if (JSObject* word = stored)
        return uncheckedDowncast<ModuleNamespace>(word);
    ModuleNamespace* word = nullptr;
    if (standing) {
        auto* created = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation));
        if (created && !created->keepsToModule() && created->activeMock() == standing)
            word = created;
    }
    if (!word)
        word = ModuleNamespace::create(vm, namespaceStructure(true), module, ModulePhase::Evaluation, { });
    if (standing) {
        word->setMockOfView(vm, standing);
        word->setRestoredAfterPatch(standing == held.preloadMock.get() && m_restoredPreloadMocks.get(file.index()));
    } else if (cyclesOfImports().find(file) && !m_filesVisibleToOwnFactories.get(file.index())) {
        word->setUninitializedMock(true);
        ensureUninitializedMock();
    }
    word->setMockWord();
    stored.set(*this, word);
    return word;
}

ModuleNamespace* Modules::ensureLoadWord(ModuleID target)
{
    ModuleID file = fileOf(target);
    ModuleID facade = moduleWithExportsOf(file);
    if (!data(file).isReloadable() || facade == file || activeMock(file) || isEvicted(file))
        return nullptr;
    LoadedModule& held = ensureLoaded(facade);
    if (JSObject* word = held.loadWord)
        return uncheckedDowncast<ModuleNamespace>(word);
    auto* word = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(facade, ModulePhase::Evaluation));
    if (!word)
        word = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), facade, ModulePhase::Evaluation, { });
    word->setLoadWord();
    held.loadWord.set(*this, word);
    return word;
}

void Modules::setImportedPatch(ModuleID file, JSObject* mock)
{
    ModuleID facade = moduleWithExportsOf(file);
    if (facade == file || !exportsWhatMockNamesAlone(facade) || !isPatchedByMock(file))
        return;
    LoadedModule& held = ensureLoaded(facade);
    held.importedPatch.set(*this, mock);
    if (auto* word = static_cast<ModuleNamespace*>(held.patchWord.get())) {
        word->leaveStructureThatCachesKnow(*m_instance.vm);
        word->setMockOfView(*m_instance.vm, mock);
    }
    didChangeLinkState();
}

ModuleNamespace* Modules::ensurePatchWord(ModuleID target)
{
    ModuleID file = fileOf(target);
    ModuleID facade = moduleWithExportsOf(file);
    LoadedModule* held = loadedIfExists(facade);
    JSObject* mock = held ? held->importedPatch.get() : nullptr;
    if (!mock || activeMock(file) || isEvicted(file)) [[likely]]
        return nullptr;
    if (JSObject* word = held->patchWord)
        return uncheckedDowncast<ModuleNamespace>(word);
    auto* word = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(facade, ModulePhase::Evaluation));
    if (!word)
        word = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), facade, ModulePhase::Evaluation, { });
    word->leaveStructureThatCachesKnow(*m_instance.vm);
    word->setMockOfView(*m_instance.vm, mock);
    word->setMockWord();
    held->patchWord.set(*this, word);
    return word;
}

bool Modules::endImportedPatch(ModuleID file)
{
    ModuleID facade = moduleWithExportsOf(file);
    LoadedModule* held = loadedIfExists(facade);
    if (!held || !held->importedPatch) [[likely]]
        return false;
    held->importedPatch.set(*this, nullptr);
    bool wasImported = held->patchWord;
    if (wasImported) {
        held->patchWord.set(*this, nullptr);
        m_hasMockEnded = true;
        if (status(facade) == ModuleStatus::Evaluated) {
            startLoadingModulesIndividually();
            forgetLoadOfEntry(facade);
        }
    }
    didChangeLinkState();
    return wasImported;
}

void Modules::forgetLoadOfEntry(ModuleID fileOrFacade)
{
    m_loadRoots.clear(fileOrFacade.index());
    forgetLoad(fileOrFacade);
    if (ModuleRecord* record = m_records.get(fileOrFacade)) {
        loader()->removeEntriesForRecord(record);
        takeOutRecord(fileOrFacade);
    }
    setStoredStatus(fileOrFacade, StoredModuleStatus::Unlinked);
    m_modulesLinkedAfterEntryPoint.set(fileOrFacade.index());
    if (LoadedModule* loaded = loadedIfExists(fileOrFacade))
        loaded->hasLinkedImporter = false;
}

ModuleNamespace* Modules::wordForLoad(ModuleID target, bool seesMock)
{
    if (seesMock)
        return mockNamespaceForLoad(target);
    if (ModuleNamespace* word = m_mocks ? ensurePatchWord(target) : nullptr) [[unlikely]]
        return word;
    ModuleNamespace* word = ensureLoadWord(target);
    if (data(fileOf(target)).kind == ModuleKind::CommonJs)
        ensureLoaded(moduleWithExportsOf(fileOf(target))).hasLinkedImporter = true;
    if (!word && data(fileOf(target)).isReloadable())
        ensureLoaded(fileOf(target)).isLinkedWithoutNotice = true;
    return word;
}

void Modules::didReplaceActiveMock(ModuleID file, JSObject* previous, JSObject* current, bool isSameEntry)
{
    if (previous && (!current || !isSameEntry))
        forgetRequireEdges(file, true);
    LoadedModule* held = loadedIfExists(file);
    if (!held)
        return;
    if (held->preloadMock && m_hasReloadableModules && data(file).kind == ModuleKind::CommonJs && previous != current) {
        if (previous == held->preloadMock.get() && !isSameEntry) {
            if (auto parent = forgetEntryParent(file))
                m_parentsOfPreloadMocks.set(file, *parent);
        } else if (current == held->preloadMock.get()) {
            forgetEntryParent(file);
            if (auto parent = m_parentsOfPreloadMocks.takeOptional(file)) {
                m_modulesWithKnownParent.set(file.index());
                m_entryParents.set(file, *parent);
            }
        }
    }
    VM& vm = *m_instance.vm;
    auto show = [&](JSObject* word, JSObject* mock) {
        auto* object = uncheckedDowncast<ModuleNamespace>(word);
        object->leaveStructureThatCachesKnow(vm);
        object->setMockOfView(vm, mock);
        object->setKeepsOriginalNames(isMockedByFactoryThatTakesOriginal(file));
    };
    auto isRestored = [&](JSObject* word) {
        return m_preloadedMocks.get(file.index()) || uncheckedDowncast<ModuleNamespace>(word)->isRestoredAfterPatch();
    };
    if (previous && previous == current) {
        if (data(file).kind != ModuleKind::CommonJs)
            return;
        m_restoredPreloadMocks.set(file.index());
        if (JSObject* word = held->mockWord; word && !isRestored(word)) {
            held->earlierPreloadMockState.set(*this, word);
            held->mockWord.set(*this, nullptr);
        }
        return;
    }
    if (JSObject* word = held->earlierPreloadMockState; word && previous && current && isSameEntry && held->preloadMock.get() == previous) {
        show(word, current);
    }
    if (!current || (previous && !isSameEntry))
        held->earlierPreloadMockState.set(*this, nullptr);
    if (JSObject* word = held->mockWord) {
        if (previous && current && isSameEntry) {
            if (held->preloadMock.get() == previous) {
                held->preloadMockWord.set(*this, word);
                held->isPreloadMockPatched = true;
            }
            show(word, current);
        } else if (previous) {
            if (held->preloadMock.get() == previous)
                held->preloadMockWord.set(*this, word);
            else if (!current && held->preloadMock && word == held->preloadMockWord.get() && isRestored(word))
                show(word, held->preloadMock);
            held->mockWord.set(*this, nullptr);
        }
    }
    if (previous && current && isSameEntry) {
        auto* registered = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(moduleWithExportsOf(file), ModulePhase::Evaluation));
        if (registered && registered->mockOfView() == previous)
            show(registered, current);
    }
    m_hasMockEnded |= previous && (!current || !isSameEntry);
    if (!current) {
        if (JSObject* word = held->pendingMockWord; word && previous)
            show(word, previous);
        return;
    }
    if (JSObject* word = held->pendingMockWord) {
        show(word, current);
        held->pendingMockWord.set(*this, nullptr);
        if (!held->mockWord)
            held->mockWord.set(*this, word);
    }
    if (!held->mockWord && held->preloadMock.get() == current && held->preloadMockWord) {
        bool wasPatched = std::exchange(held->isPreloadMockPatched, false);
        JSObject* word = held->preloadMockWord;
        if (!wasPatched || m_preloadedMocks.get(file.index())) {
            show(word, current);
            held->mockWord.set(*this, word);
        }
        held->preloadMockWord.set(*this, nullptr);
    }
}

void Modules::recordPreloadedMocks()
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return;
    m_preloadedMocks.ensureSize(m_table.size());
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        const LoadedModule* held = loadedIfExists(ModuleID(index));
        if (held && held->activeMock && (held->mockWord || m_loadedInPreload.get(moduleWithExportsOf(ModuleID(index)).index())))
            m_preloadedMocks.quickSet(index);
    }
}

void Modules::clearPendingMockState(ModuleID file)
{
    LoadedModule* held = loadedIfExists(file);
    JSObject* word = held ? held->pendingMockWord.get() : nullptr;
    if (!word)
        return;
    auto* object = uncheckedDowncast<ModuleNamespace>(word);
    Vector<ModuleID, 4> linkedByFailedLoads;
    for (size_t index : m_linkedByFailedLoads) {
        ModuleID module(index);
        const ModuleLinkData* link = isLinkedAndUnevaluated(module) ? findModuleLink(module, file) : nullptr;
        if (link && link->hasMock && readField(currentInstance(module), link->mock) == JSValue(word))
            linkedByFailedLoads.append(module);
    }
    if (JSObject* mock = object->activeMock())
        object->setMockOfView(*m_instance.vm, mock);
    else {
        object->setUninitializedMock(false);
        object->setKeepsToModule(true);
    }
    held->pendingMockWord.set(*this, nullptr);
    m_hasMockEnded = true;
    for (ModuleID module : linkedByFailedLoads)
        releaseModuleWithImporters(module);
}

Vector<ModuleID, 16> Modules::collectUnlinkedInstances(ModuleID root)
{
    Vector<ModuleID, 16> found;
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if ((module != root && isEvicted(module)) || m_linkedInstances.get(module.index()) || (!hasInstances(module) && !data(module).isESModuleOfProgram()))
            continue;
        found.append(module);
        auto visit = [&](ModuleID target) {
            if (!seen.get(target.index()) && !isMockSeenByLoad(target, module)) {
                seen.quickSet(target.index());
                work.append(target);
            }
        };
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isRequire && !request.isHoisted)
                visit(request.targetModule());
        }
        for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
            if (!link.isResolved)
                visit(link.targetModule());
        }
    }
    return found;
}

Vector<ModuleID, 16> Modules::linkInstances(ModuleID root)
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return { };
    if (m_linkedInstances.get(root.index())) [[likely]] {
        noteLinkedModules(root);
        return { };
    }
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    linkFailedLoads();
    RETURN_IF_EXCEPTION(scope, { });
    noteLinkedModules(root);
    Vector<ModuleID, 16> modules = collectUnlinkedInstances(root);
    linkInstances(modules.span());
    RETURN_IF_EXCEPTION(scope, { });
    return modules;
}

void Modules::resolveLinks(std::span<const ModuleID> modules)
{
    EndsOfLinkPaths known;
    for (ModuleID module : modules) {
        Environment* instance = currentInstance(module);
        for (const ModuleLinkData& resolved : m_table.moduleLinks(module)) {
            if (!resolved.isResolved)
                continue;
            EndOfLinkPath end = followLinkPath(module, instance, resolved.firstStep, known);
            if (resolved.hasLink) {
                if (end.instance)
                    writeField(instance, resolved.link, end.instance);
                continue;
            }
            const ModuleLinkData* link = end.instance ? findModuleLink(end.module, resolved.targetModule()) : nullptr;
            JSValue word = link && link->hasMock ? readField(end.instance, link->mock) : JSValue();
            writeField(instance, resolved.mock, word ? word : jsUndefined());
        }
    }
    didChangeLinkState();
}

auto Modules::followLinkPath(ModuleID module, Environment* instance, uint32_t step, EndsOfLinkPaths& known) const -> EndOfLinkPath
{
    Vector<std::pair<Environment*, uint32_t>, 8> passed;
    EndOfLinkPath end { instance, module };
    while (end.instance && step != LinkPathStepData::noStep) {
        if (auto answer = known.find({ end.instance, step }); answer != known.end()) {
            end = answer->value;
            break;
        }
        passed.append({ end.instance, step });
        const LinkPathStepData& stepData = m_table.linkPathStep(step);
        const ModuleLinkData* link = findModuleLink(end.module, stepData.moduleOfStep());
        end = { link && link->hasLink && !mockWord(*link, end.instance) ? readLink(end.instance, link->link) : nullptr, stepData.moduleOfStep() };
        step = stepData.nextStep;
    }
    for (auto& each : passed)
        known.set(each, end);
    return end;
}

JSObject* Modules::absentModuleNamespace(ModuleID external)
{
    AbstractModuleRecord* record = nullptr;
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
        const Identifier& key = resolvedKey(external);
        if (scope.exception()) [[unlikely]] {
            (void)scope.tryClearException();
            return nullptr;
        }
        ModuleRegistryEntry* entry = registryEntry(key, fetchParametersOf(external).get());
        record = entry ? entry->record() : nullptr;
    }
    if (!record || !record->hasLiveExports())
        return nullptr;
    ensureLoaded(external).uncompiledRecord.set(*this, record);
    m_modulesWithUncompiledRecord.set(external.index());
    return uncompiledNamespace(external, ModulePhase::Evaluation, data(external).isUncompiledCodeImportedAsNamespace() ? NamespaceUse::IsShownToProgram : NamespaceUse::CarriesBindings);
}

bool Modules::releaseAbsentModuleImporters(const String& key, ReleasedImporters which)
{
    if (m_table.modulesOfUncompiledCode().empty()) [[likely]]
        return false;
    bool hasImporters = false;
    for (const UncompiledModuleData& entry : m_table.modulesOfUncompiledCode()) {
        ModuleID external(entry.module);
        const LoadedModule* loaded = loadedIfExists(external);
        if (!loaded || loaded->keyOfLatestLoad.isNull() || loaded->keyOfLatestLoad.string() != key)
            continue;
        std::optional<ModuleID> importer = data(external).referrerModule();
        if (!importer || !findModuleLink(*importer, external))
            continue;
        releaseImportersOf(external, which);
        hasImporters = true;
    }
    return hasImporters;
}

void Modules::tellIfOriginalImports(ModuleID mocked, ModuleID importer)
{
    if (!host().didRunOriginal || m_originalsThatImportTheirImporters.get(mocked.index()))
        return;
    const CyclesOfImports::Member* original = cyclesOfImports().find(mocked);
    const CyclesOfImports::Member* above = original ? cyclesOfImports().find(importer) : nullptr;
    if (!above || above->cycle != original->cycle)
        return;
    m_originalsThatImportTheirImporters.set(mocked.index());
    host().didRunOriginal(m_instance.globalObject, m_table.fullPath(fileOf(mocked)), OriginalRun::LoadsItsImporter);
}

void Modules::tellOfFailedLoad(ModuleID module, ModuleID target)
{
    auto failed = m_failedLoadTestFiles.find(module);
    if (failed == m_failedLoadTestFiles.end() || failed->value != m_instance.vm->aotState().momentOfTestFile || !m_failedLoadTestFiles.contains(target) || !m_modulesBeforeLinkFailures.get(target.index()))
        return;
    if (m_table.hasFilePath(target) && host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(target), OriginalRun::LoadFailsToLink);
}

void Modules::linkInstances(std::span<const ModuleID> modules)
{
    if (modules.empty())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto didLink = makeScopeExit([&] {
        didChangeLinkState();
    });
    for (ModuleID module : modules) {
        ensureInstance(module);
        RETURN_IF_EXCEPTION(scope, void());
        for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
            if (!link.hasLink)
                continue;
            ensureInstance(link.targetModule());
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (hasInstances(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ensureInstance(request.targetModule());
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    for (ModuleID module : modules) {
        Environment* instance = currentInstance(module);
        for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
            ModuleID target = link.targetModule();
            if (link.hasLink)
                writeField(instance, link.link, currentInstance(target));
            if (!link.hasMock || link.isResolved)
                continue;
            if (data(target).isUncompiledCode()) [[unlikely]] {
                JSObject* namespaceObject = absentModuleNamespace(target);
                RETURN_IF_EXCEPTION(scope, void());
                writeField(instance, link.mock, namespaceObject ? JSValue(namespaceObject) : jsUndefined());
                continue;
            }
            bool seesMock = isMockSeenByLoad(target, module);
            ModuleNamespace* word = wordForLoad(target, seesMock);
            if (seesMock && m_mocks && isMocked(target) && !activeMock(target)) [[unlikely]]
                tellIfOriginalImports(target, module);
            if (!m_failedLoadTestFiles.isEmpty() && word && !word->isLoadWord()) [[unlikely]]
                tellOfFailedLoad(module, target);
            if (!word)
                didLoadDataFile(target, LoadKind::Import);
            if (data(target).isOfLibrary())
                noteLibraryModuleLoaded(target);
            if (!word && isReloadedLibraryModule(target)) [[unlikely]]
                word = reloadedLibraryWord(target);
            if (word && !m_table.hasFilePath(target) && wasActiveMockEntryRemoved(target))
                noteAskUnderMockInPlace(target);
            writeField(instance, link.mock, word ? JSValue(word) : jsUndefined());
            if (word && word->linkedExports() && !word->namesOfMock()) {
                fixMockNames(word);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
    }
    resolveLinks(modules);
    if (m_linkMoments.isEmpty()) {
        m_linkMoments.fill(0, m_table.size());
        m_linkTestFiles.fill(0, m_table.size());
    }
    bool isInPreloadHook = m_hasBegunTestFile && host().isInPreload && host().isInPreload(m_instance.globalObject).value_or(false);
    for (ModuleID module : modules) {
        m_loadedByPreloadHooks.set(module.index(), isInPreloadHook && !m_linkTestFiles[module.index()]);
        if (m_releasedLoadsOfPreloadHooks.clear(module.index())) [[unlikely]]
            tellOfKeptRun(fileOf(module));
        m_linkedInstances.set(module.index());
        m_linkMoments[module.index()] = m_numberOfReleases;
        m_linkTestFiles[module.index()] = m_instance.vm->aotState().momentOfTestFile;
    }
}

BitVector Modules::findModulesLinkedBefore(ModuleID root, ModuleID importer) const
{
    struct Step {
        ModuleID module;
        size_t nextRequest;
        uint32_t place;
        uint32_t earliestPlace;
    };
    BitVector linked(m_table.size());
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> places;
    BitVector isOpen(m_table.size());
    Vector<ModuleID, 16> open { root };
    Vector<Step, 16> path { { root, 0, 0, 0 } };
    places.add(root.index(), 0);
    isOpen.quickSet(root.index());
    while (!path.isEmpty()) {
        Step& step = path.last();
        auto requests = m_table.requests(step.module);
        if (step.nextRequest == requests.size() || (isMocked(step.module) && step.module != root)) {
            if (step.module == importer)
                return linked;
            Step done = path.takeLast();
            if (done.earliestPlace == done.place) {
                for (bool isHead = false; !isHead;) {
                    ModuleID module = open.takeLast();
                    isOpen.quickClear(module.index());
                    linked.quickSet(module.index());
                    isHead = module == done.module;
                }
            }
            if (!path.isEmpty())
                path.last().earliestPlace = std::min(path.last().earliestPlace, done.earliestPlace);
            continue;
        }
        const ModuleRequestData& request = requests[step.nextRequest++];
        if (request.isHoisted || request.isRequire)
            continue;
        ModuleID target = request.targetModule();
        uint32_t place = places.size();
        if (auto entered = places.add(target.index(), place); !entered.isNewEntry) {
            if (isOpen.quickGet(target.index()))
                step.earliestPlace = std::min(step.earliestPlace, entered.iterator->value);
            continue;
        }
        isOpen.quickSet(target.index());
        open.append(target);
        path.append({ target, 0, place, place });
    }
    return linked;
}

void Modules::noteLinkFailure(ModuleID root, ModuleID importer)
{
    for (size_t index : staticImportClosure(root, MockTraversal::Stop)) {
        ModuleID module(index);
        if (hasInstances(module) && status(module) <= ModuleStatus::Linked && !isMocked(module) && !wasLoadedWithGraph(module))
            m_failedLoadTestFiles.add(module, m_instance.vm->aotState().momentOfTestFile);
    }
    BitVector linkedBefore = findModulesLinkedBefore(root, importer);
    m_modulesBeforeLinkFailures.merge(linkedBefore);
    for (size_t index : linkedBefore) {
        ModuleID module(index);
        if (hasInstances(module) && !m_linkedInstances.get(index) && !isEvicted(module) && !isMocked(module))
            m_modulesLinkedBeforeFailures.append(module);
    }
}

void Modules::unlinkInstances(ModuleID root, std::span<const ModuleID> modules)
{
    didChangeLinkState();
    BitVector linked = findModulesLinkedBefore(root, m_lastLinkFailureImporter);
    m_modulesBeforeLinkFailures.merge(linked);
    for (ModuleID module : modules) {
        if (hasInstances(module) && status(module) <= ModuleStatus::Linked && !isMocked(module))
            m_failedLoadTestFiles.add(module, m_instance.vm->aotState().momentOfTestFile);
        if (ModuleStatus current = status(module); current != ModuleStatus::Evaluated && current != ModuleStatus::EvaluatingAsync && !linked.quickGet(module.index()))
            m_linkedInstances.clear(module.index());
    }
}

bool Modules::wasLinkedBeforeTestFile(ModuleID module) const
{
    return isLinkedAndUnevaluated(module) && !wasLoadedByTestFile(module);
}

void Modules::linkFailedLoads()
{
    if (m_modulesLinkedBeforeFailures.isEmpty()) [[likely]]
        return;
    Vector<ModuleID, 4> modules = std::exchange(m_modulesLinkedBeforeFailures, { });
    modules.removeAllMatching([&](ModuleID module) {
        return m_linkedInstances.get(module.index()) || status(module) > ModuleStatus::Linked || isEvicted(module) || isMocked(module);
    });
    for (ModuleID module : modules)
        m_linkedByFailedLoads.set(module.index());
    linkInstances(modules.span());
}

void Modules::tryLinkFailedLoads()
{
    if (m_modulesLinkedBeforeFailures.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    linkFailedLoads();
    if (scope.exception()) [[unlikely]]
        scope.tryClearException();
}

void Modules::noteFailedEvaluation(ModuleID root, ModuleID failed)
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return;
    BitVector reached = findModulesLinkedBefore(root, failed);
    for (size_t index : staticImportClosure(root, MockTraversal::Stop)) {
        if (isLinkedAndUnevaluated(ModuleID(index)) && (data(ModuleID(index)).needsEvaluation() || !reached.quickGet(index)))
            m_linkedByFailedLoads.set(index);
    }
}

void Modules::noteFailedLoaderLink()
{
    for (ModuleID module : std::exchange(m_instancesLinkedByLoader, { })) {
        if (isLinkedAndUnevaluated(module))
            m_linkedByFailedLoads.set(module.index());
    }
}

void Modules::noteLinkedModules(ModuleID root)
{
    if (m_linkedByFailedLoads.isEmpty()) [[likely]]
        return;
    m_linkedByFailedLoads.exclude(staticImportClosure(root, MockTraversal::Stop));
}

auto Modules::resolveField(ModuleID module, unsigned index, Environment* instance) const -> ResolvedField
{
    const ExportedFieldData& field = m_table.exportedField(m_table.exports(module)[index]);
    if (!instance)
        instance = currentInstance(module);
    if (field.module == module.index())
        return { instance, field.slot };
    const ReexportedFieldData* reexported = m_table.findReexportedField(module, index);
    if (!reexported || !isLinkedInstance(module, instance))
        return { lastInstance(field.declaringModule()), field.slot, nullptr, 0, true };
    MockForExport mock = findMockForField(module, index, instance);
    if (mock.word)
        return { nullptr, field.slot, mock.word, mock.nameInMock, false, mock.isFromStarExport };
    if (auto reexport = mock.exportWithoutLinks; reexport && findMockForReexport(reexport->first, m_table.exports(reexport->first)[reexport->second], MockMode::Bypass)) [[unlikely]]
        return { readLink(instance, reexported->link), field.slot, nullptr, 0, true, false, reexport };
    return { readLink(instance, reexported->link), field.slot };
}

#if ASSERT_ENABLED
template<typename Function>
auto Modules::answerAnew(const Function& function) const
{
    SetForScope isAnsweringAnew(m_isAnsweringAnew, true);
    AnswersByLinks& answers = answersByLinks();
    AnswersByLinks kept = std::exchange(answers, AnswersByLinks { answers.state });
    auto answer = function();
    answers = WTF::move(kept);
    return answer;
}
#endif

auto Modules::findMockForField(ModuleID module, unsigned index, Environment* instance) const -> MockForExport
{
    const ExportedFieldData& field = m_table.exportedField(m_table.exports(module)[index]);
    auto& known = answersByLinks().mocksBehindFields;
    Vector<std::pair<Environment*, unsigned>, 8> passed;
    MockForExport found;
    while (instance && field.module != module.index()) {
        if (auto answer = known.find({ instance, index }); answer != known.end()) {
            found = answer->value;
            ASSERT(m_isAnsweringAnew || found == answerAnew([&] { return findMockForField(module, index, instance); }));
            break;
        }
        const ReexportedFieldData* reexported = m_table.findReexportedField(module, index);
        if (!reexported || !isLinkedInstance(module, instance))
            break;
        ModuleID named = reexported->namedModule();
        const ModuleLinkData* link = findModuleLink(module, named);
        bool hasNoLinks = !hasInstances(named) && data(named).isESModuleOfProgram();
        if (!link && !hasNoLinks)
            break;
        passed.append({ instance, index });
        if (passed.size() > m_table.size()) [[unlikely]]
            break;
        if (ModuleNamespace* word = link ? mockWord(*link, instance) : nullptr) {
            found = { word, reexported->importName, m_table.exports(module)[index].isFromStarExport };
            break;
        }
        std::optional<unsigned> next = hasNoLinks || link->hasLink ? m_table.findExport(named, identifier(reexported->importName).impl()) : std::nullopt;
        if (!next)
            break;
        const ModuleExportData& entry = m_table.exports(named)[*next];
        if (entry.kind() != ModuleExportKind::Field || &m_table.exportedField(entry) != &field)
            break;
        if (hasNoLinks) {
            found.exportWithoutLinks = std::pair { named, *next };
            break;
        }
        instance = readLink(instance, link->link);
        module = named;
        index = *next;
    }
    for (auto& request : passed)
        known.set(request, found);
    return found;
}

auto Modules::answersByLinks() const -> AnswersByLinks&
{
    auto& state = m_instance.vm->aotState();
    LinkState current { m_linkStateVersion, m_activeMockCount.timesWritten(), m_linkEpoch, state.momentOfLoads, state.momentOfTestFile };
    AnswersByLinks& answers = m_answersByLinks[m_isBypassingMocks];
    if (answers.state != current)
        answers = { current };
    return answers;
}

auto Modules::resolveNamespace(ModuleID module, unsigned index, Environment* instance) const -> ResolvedNamespace
{
    ModuleID target = m_table.exports(module)[index].targetModule();
    if (!instance)
        instance = currentInstance(module);
    NamespaceBehindExport found = followNamespace(module, index, instance);
    using End = NamespaceBehindExport::End;
    Environment* holder = found.holder.value_or(nullptr);
    if (found.absent) [[unlikely]]
        return { target, nullptr, nullptr, std::nullopt, false, std::nullopt, found.absent };
    switch (found.end) {
    case End::Mock:
        return { target, nullptr, found.word, found.nameInMock, found.isFromStarExport };
    case End::Instance:
        return { target, found.instance };
    case End::WithoutInstances:
        return { found.isFollowed || found.mayBeMocked ? std::optional(target) : std::nullopt };
    case End::WithoutLink:
        if (found.isFollowed && !holder)
            return { target };
        break;
    case End::Original:
        if (!holder)
            return { target, nullptr, nullptr, std::nullopt, false, found.original };
        break;
    case End::Broken:
        break;
    }
    if (holder)
        return { target, holder };
    return { };
}

auto Modules::followNamespace(ModuleID module, unsigned index, Environment* instance) const -> NamespaceBehindExport
{
    using End = NamespaceBehindExport::End;
    struct Step {
        std::pair<Environment*, unsigned> request;
        std::optional<Environment*> holder;
    };
    ModuleID target = m_table.exports(module)[index].targetModule();
    auto& known = answersByLinks().namespaces;
    Vector<Step, 8> passed;
    NamespaceBehindExport found;
    while (passed.size() <= m_table.size() && isLinkedInstance(module, instance)) {
        if (auto answer = known.find({ instance, index }); answer != known.end()) {
            found = answer->value;
            ASSERT(m_isAnsweringAnew || found == answerAnew([&] { return followNamespace(module, index, instance); }));
            break;
        }
        const ReexportedFieldData* reexported = m_table.findReexportedField(module, index);
        Step step { { instance, index }, std::nullopt };
        if (reexported && reexported->hasLink)
            step.holder = readLink(instance, reexported->link);
        passed.append(step);
        ModuleID named = reexported ? reexported->namedModule() : target;
        const ModuleLinkData* link = findModuleLink(module, named);
        if (!link) {
            found.end = reexported ? End::Broken : End::WithoutLink;
            break;
        }
        bool isDeferred = !reexported && phaseOfNamespaceExport(m_table.exports(module)[index]) == ModulePhase::Defer;
        if (ModuleNamespace* word = isDeferred ? nullptr : mockWord(*link, instance)) {
            found.end = End::Mock;
            found.word = word;
            found.nameInMock = reexported ? std::optional(reexported->importName) : std::nullopt;
            found.isFromStarExport = m_table.exports(module)[index].isFromStarExport;
            break;
        }
        if (!reexported) {
            found.end = link->hasLink ? End::Instance : End::WithoutInstances;
            found.instance = link->hasLink ? readLink(instance, link->link) : nullptr;
            found.mayBeMocked = link->hasMock;
            if (JSValue absent = link->hasMock && data(named).isUncompiledCode() ? readField(instance, link->mock) : JSValue(); absent && absent.isObject()) [[unlikely]]
                found.absent = asObject(absent);
            break;
        }
        std::optional<unsigned> next = m_table.findExport(named, identifier(reexported->importName).impl());
        bool isNamespace = next && m_table.exports(named)[*next].kind() == ModuleExportKind::Namespace && m_table.exports(named)[*next].targetModule() == target;
        if (!isNamespace) {
            Environment* exporter = link->hasLink ? readLink(instance, link->link) : nullptr;
            const ModuleLinkData* further = exporter ? findModuleLink(named, target) : nullptr;
            if (ModuleNamespace* word = further ? mockWord(*further, exporter) : nullptr) {
                found.end = End::Mock;
                found.word = word;
            }
            break;
        }
        if (!link->hasLink) {
            found.end = End::Original;
            found.original = { named, *next };
            break;
        }
        instance = readLink(instance, link->link);
        module = named;
        index = *next;
    }
    bool isEndAtLast = passed.size() && passed.last().request.first == instance && passed.last().request.second == index;
    for (size_t place = passed.size(); place--;) {
        Step& step = passed[place];
        if (step.holder)
            found.holder = step.holder;
        if (!(isEndAtLast && place + 1 == passed.size()))
            found.isFollowed = true;
        known.set(step.request, found);
    }
    return found;
}

auto Modules::resolveVariable(ModuleID module, unsigned index, Environment* instance) const -> ResolvedVariable
{
    ResolvedVariable resolved;
    SlotData variable = m_table.exports(module)[index].variable;
    auto& known = answersByLinks().variables;
    Vector<std::pair<Environment*, unsigned>, 8> passed;
    if (!instance)
        instance = currentInstance(module);
    while (instance && passed.size() <= m_table.size()) {
        if (auto answer = known.find({ instance, index }); answer != known.end()) {
            resolved = answer->value;
            ASSERT(m_isAnsweringAnew || resolved == answerAnew([&] { return resolveVariable(module, index, instance); }));
            break;
        }
        const ReexportedFieldData* reexported = m_table.findReexportedField(module, index);
        const ModuleLinkData* link = reexported && isLinkedInstance(module, instance) ? findModuleLink(module, reexported->namedModule()) : nullptr;
        if (!link)
            break;
        passed.append({ instance, index });
        resolved.isFollowed = true;
        if (ModuleNamespace* word = mockWord(*link, instance)) {
            resolved = { true, word, reexported->importName, m_table.exports(module)[index].isFromStarExport };
            break;
        }
        std::optional<unsigned> next = m_table.findExport(link->targetModule(), identifier(reexported->importName).impl());
        if (!next && link->hasLink && data(link->targetModule()).hasDynamicStarExport()) {
            resolved.starHolder = readLink(instance, link->link);
            resolved.holderModule = link->targetModule();
            resolved.nameInHolder = reexported->importName;
        }
        if (!next)
            break;
        const ModuleExportData& entry = m_table.exports(link->targetModule())[*next];
        if (entry.kind() != ModuleExportKind::Variable || entry.variable.encoded() != variable.encoded())
            break;
        if (!link->hasLink) {
            resolved.original = std::pair { link->targetModule(), *next };
            break;
        }
        instance = readLink(instance, link->link);
        module = link->targetModule();
        index = *next;
    }
    for (auto& request : passed)
        known.set(request, resolved);
    return resolved;
}

JSValue Modules::readLinkedMock(ModuleNamespace* word, const Identifier& key)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* mock = word->linkedExports();
    if (JSObject* names = word->namesOfMock()) {
        JSValue stored = storedValue(realm, names, key);
        if (stored && stored.isFalse())
            return jsUndefined();
    }
    bool hasName = mock->hasProperty(realm, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasName)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, mock->get(realm, key));
}

BitVector Modules::modulesOnImportCycle(ModuleID module) const
{
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>> importers;
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { module };
    seen.quickSet(module.index());
    while (!work.isEmpty()) {
        ModuleID importer = work.takeLast();
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID target = request.targetModule();
            importers.add(target, Vector<ModuleID, 2>()).iterator->value.append(importer);
            if (!seen.quickSet(target.index()))
                work.append(target);
        }
    }
    BitVector onCycle(m_table.size());
    work.append(module);
    while (!work.isEmpty()) {
        auto found = importers.find(work.takeLast());
        if (found == importers.end())
            continue;
        for (ModuleID importer : found->value) {
            if (!onCycle.quickSet(importer.index()))
                work.append(importer);
        }
    }
    return onCycle;
}

bool Modules::addStarExportNames(ModuleID module, const BitVector& onCycle, PropertyNameArrayBuilder& names, BitVector& seen)
{
    if (seen.quickSet(module.index()))
        return true;
    bool areKnown = true;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSGlobalObject* realm = m_instance.globalObject;
    auto addNamesOf = [&](JSObject* object) {
        PropertyNameArrayBuilder found(*m_instance.vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        object->methodTable()->getOwnPropertyNames(object, realm, found, DontEnumPropertiesMode::Include);
        for (const Identifier& name : found) {
            if (name != m_instance.vm->propertyNames->defaultKeyword)
                names.add(name);
        }
    };
    for (ModuleRequestData star : m_table.dynamicStars(module)) {
        ModuleID target = star.targetModule();
        if (ModuleNamespace* word = isMockSeenByLoad(target, module) ? mockNamespaceForLoad(target) : nullptr; word && word->activeMock()) {
            if (!word->namesOfMock()) {
                fixMockNames(word);
                RETURN_IF_EXCEPTION(scope, false);
            }
            addNamesOf(word->namesOfMock());
            RETURN_IF_EXCEPTION(scope, false);
            continue;
        }
        if (onCycle.get(target.index()))
            continue;
        for (const ModuleExportData& moduleExport : m_table.exports(target)) {
            if (Identifier name = identifier(moduleExport.name); name != m_instance.vm->propertyNames->defaultKeyword)
                names.add(name);
        }
        if (data(target).isESModuleOfProgram() && !data(target).isFacade()) {
            areKnown &= addStarExportNames(target, onCycle, names, seen);
            RETURN_IF_EXCEPTION(scope, false);
            continue;
        }
        if (status(fileOf(target)) != ModuleStatus::Evaluated) {
            areKnown = false;
            continue;
        }
        JSObject* source = activeMock(target) ? originalNamespace(target) : moduleNamespace(moduleLoadedBy(star), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, false);
        addNamesOf(source);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return areKnown;
}

void Modules::fixMockNames(ModuleNamespace* word)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* names = createTableOfExports();
    PropertyNameArrayBuilder found(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    LoadedModule* loaded = loadedIfExists(word->module());
    if (loaded && !word->isLoadWord() && (loaded->wasImportedUnderActiveMock || activeMock(word->module()) == word->activeMock())) {
        BitVector leftOut = exportsLeftOutOf(word->module());
        std::span<const ModuleExportData> exports = m_table.exports(word->module());
        for (unsigned index = 0; index < exports.size(); ++index) {
            if (!leftOut.get(index))
                found.add(identifier(exports[index].name));
        }
        if (JSObject* added = loaded->exportsWhatMockNamesAlone ? loaded->capturedExports.get() : loaded->exportsAddedByMock.get())
            JSObject::getOwnPropertyNames(added, m_instance.globalObject, found, DontEnumPropertiesMode::Include);
    } else {
        JSObject* mock = word->linkedExports();
        mock->methodTable()->getOwnPropertyNames(mock, m_instance.globalObject, found, DontEnumPropertiesMode::Exclude);
    }
    RETURN_IF_EXCEPTION(scope, void());
    ModuleID module = word->module();
    bool hasNamesOfOriginal = keepsOriginalNames(*word) && data(module).hasExportTable() && !data(module).capturesExports();
    if (!hasNamesOfOriginal) {
        for (const Identifier& name : found) {
            names->putDirectMayBeIndex(m_instance.globalObject, name, jsBoolean(true));
            RETURN_IF_EXCEPTION(scope, void());
        }
        word->setMockNames(vm, names);
        return;
    }
    IdentifierSet namesInMock;
    JSObject* provisional = createTableOfExports();
    for (const Identifier& name : found) {
        namesInMock.add(name.impl());
        provisional->putDirectMayBeIndex(m_instance.globalObject, name, jsBoolean(true));
        RETURN_IF_EXCEPTION(scope, void());
    }
    word->setMockNames(vm, provisional);
    PropertyNameArrayBuilder ofOriginal(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    for (const ModuleExportData& moduleExport : m_table.exports(module))
        ofOriginal.add(identifier(moduleExport.name));
    BitVector onCycle;
    if (data(module).hasDynamicStarExport())
        onCycle = modulesOnImportCycle(module);
    bool isOnImportCycle = false;
    for (ModuleRequestData star : m_table.dynamicStars(module))
        isOnImportCycle |= onCycle.get(star.targetModule().index());
    bool areNamesKnown = true;
    IdentifierSet namesOutsideCycles;
    bool isOriginalLinked = status(module) == ModuleStatus::Evaluated && (!hasInstances(module) || isLinkedInstance(module, currentInstance(module)));
    if (isOnImportCycle || (data(module).hasDynamicStarExport() && !isOriginalLinked)) {
        PropertyNameArrayBuilder behindStars(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        BitVector seen(m_table.size());
        areNamesKnown = addStarExportNames(module, onCycle, behindStars, seen);
        RETURN_IF_EXCEPTION(scope, void());
        for (const ModuleExportData& moduleExport : m_table.exports(module)) {
            if (!moduleExport.isFromStarExport)
                namesOutsideCycles.add(identifier(moduleExport.name).impl());
        }
        for (const Identifier& name : behindStars) {
            namesOutsideCycles.add(name.impl());
            ofOriginal.add(name);
        }
    }
    if (data(module).hasDynamicStarExport() && isOriginalLinked) {
        JSObject* original = originalNamespace(module);
        RETURN_IF_EXCEPTION(scope, void());
        original->methodTable()->getOwnPropertyNames(original, m_instance.globalObject, ofOriginal, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!areNamesKnown || isOnImportCycle) {
        for (const Identifier& name : found)
            ofOriginal.add(name);
    }
    for (const Identifier& name : ofOriginal) {
        bool isInMock = namesInMock.contains(name.impl());
        bool isUncertain = (isOnImportCycle || !areNamesKnown) && !namesOutsideCycles.contains(name.impl());
        names->putDirectMayBeIndex(m_instance.globalObject, name, isUncertain ? jsNumber(isInMock) : jsBoolean(isInMock));
        RETURN_IF_EXCEPTION(scope, void());
    }
    word->setMockNames(vm, names);
}

bool Modules::hasNameInMock(ModuleNamespace* word, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (word->isUninitializedMock()) [[unlikely]]
        return true;
    if (!word->namesOfMock()) {
        fixMockNames(word);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (JSObject* names = word->namesOfMock())
        RELEASE_AND_RETURN(scope, !!storedValue(m_instance.globalObject, names, name));
    RELEASE_AND_RETURN(scope, word->linkedExports()->hasProperty(m_instance.globalObject, name));
}

bool Modules::hasNameInMockOutsideCycles(ModuleNamespace* word, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (word->isUninitializedMock()) [[unlikely]]
        return false;
    bool hasName = hasNameInMock(word, name);
    RETURN_IF_EXCEPTION(scope, false);
    return hasName && storedValue(m_instance.globalObject, word->namesOfMock(), name).isBoolean();
}

bool Modules::hasOutdatedStarLink(ModuleID module, Environment* instance) const
{
    if (!data(module).hasDynamicStarExport() || !isLinkedInstance(module, instance))
        return false;
    auto& known = answersByLinks().hasOutdatedStarLink;
    if (auto answer = known.find(instance); answer != known.end()) {
        bool isOutdated = answer->value;
        ASSERT(m_isAnsweringAnew || isOutdated == answerAnew([&] { return hasOutdatedStarLink(module, instance); }));
        return isOutdated;
    }
    OutdatedStarSearch search;
    unsigned outermost = 0;
    bool isOutdated = findOutdatedStarLink(module, instance, search, 0, outermost);
    auto& answers = answersByLinks().hasOutdatedStarLink;
    if (!isOutdated && !search.hasOtherInstances) {
        for (auto& visit : search.visits.values())
            answers.add(visit.instance, false);
    }
    answers.set(instance, isOutdated);
    return isOutdated;
}

bool Modules::findOutdatedStarLink(ModuleID module, Environment* instance, OutdatedStarSearch& search, unsigned depth, unsigned& outermost) const
{
    if (!data(module).hasDynamicStarExport() || !isLinkedInstance(module, instance))
        return false;
    auto& known = answersByLinks().hasOutdatedStarLink;
    if (auto answer = known.find(instance); answer != known.end())
        return answer->value;
    if (auto visit = search.visits.add(module, OutdatedStarSearch::Visit { instance, depth }); !visit.isNewEntry) {
        bool isOther = visit.iterator->value.instance != instance;
        search.hasOtherInstances |= isOther;
        outermost = isOther ? 0 : std::min(outermost, visit.iterator->value.outermost);
        return false;
    }
    unsigned outermostBelow = depth;
    bool isCurrent = instance == currentInstance(module);
    for (ModuleRequestData star : m_table.dynamicStars(module)) {
        const ModuleLinkData* link = findModuleLink(module, star.targetModule());
        if (!link)
            continue;
        bool seesOriginal = isCurrent && !mockWord(*link, instance) && (!link->hasLink || readLink(instance, link->link) == currentInstance(link->targetModule())) && importsOriginalDespiteMock(star.targetModule(), module);
        bool isOutdated = !isCurrent || (!seesOriginal && !isLinkedToCurrentEntry(module, star.targetModule()).value_or(true));
        if (!isOutdated && !mockWord(*link, instance) && link->hasLink)
            isOutdated = findOutdatedStarLink(link->targetModule(), readLink(instance, link->link), search, depth + 1, outermostBelow);
        if (isOutdated) {
            answersByLinks().hasOutdatedStarLink.set(instance, true);
            return true;
        }
    }
    if (outermostBelow >= depth)
        answersByLinks().hasOutdatedStarLink.set(instance, false);
    else
        search.visits.find(module)->value.outermost = outermostBelow;
    outermost = std::min(outermost, outermostBelow);
    return false;
}

bool Modules::reexportsNameOf(ModuleID module, const Identifier& name, ModuleID source) const
{
    Identifier exportName = name;
    for (size_t steps = 0; steps <= m_table.size(); ++steps) {
        std::optional<unsigned> index = m_table.findExport(module, exportName.impl());
        if (!index || m_table.exports(module)[*index].isFromStarExport)
            return false;
        const ModuleExportData& entry = m_table.exports(module)[*index];
        if (entry.kind() == ModuleExportKind::Indirect) {
            const IndirectExportData& indirect = m_table.indirectExport(entry);
            module = indirect.targetModule();
            exportName = identifier(indirect.importName);
        } else {
            const ReexportedFieldData* reexported = hasInstances(module) ? m_table.findReexportedField(module, *index) : nullptr;
            if (!reexported)
                return false;
            module = reexported->namedModule();
            exportName = identifier(reexported->importName);
        }
        if (module == source && exportName == name)
            return true;
    }
    return false;
}

auto Modules::bindingOfExport(ModuleID module, unsigned index, Environment* instance, BindingSearch& search) -> LinkedBinding
{
    using Kind = LinkedBinding::Kind;
    using Type = LinkedBinding::Type;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleExportData& entry = m_table.exports(module)[index];
    auto follow = [&](ModuleID requested, uint32_t importName) -> LinkedBinding {
        const ModuleLinkData* link = isLinkedInstance(module, instance) ? findModuleLink(module, requested) : nullptr;
        if (ModuleNamespace* word = link ? mockWord(*link, instance) : nullptr)
            return { Type::Resolved, Kind::Name, true, word, 0, identifier(importName).impl() };
        if (isCommonJSModuleOrFacade(requested))
            return { Type::Resolved, Kind::Name, false, nullptr, fileOf(requested).index(), identifier(importName).impl() };
        if (!link || !link->hasLink)
            return { };
        return resolveBinding(link->targetModule(), identifier(importName), readLink(instance, link->link), search);
    };
    size_t revisitsBefore = search.numberOfRevisits;
    if (entry.kind() == ModuleExportKind::Indirect) {
        const IndirectExportData& indirect = m_table.indirectExport(entry);
        LinkedBinding binding = follow(indirect.targetModule(), indirect.importName);
        RETURN_IF_EXCEPTION(scope, { });
        search.numberOfOrderedResults += search.numberOfRevisits != revisitsBefore;
        if (binding.type == Type::NotFound && !binding.isLackedByMock && search.numberOfRevisits == revisitsBefore)
            return { Type::Resolved, Kind::Name, false, nullptr, indirect.module, identifier(indirect.importName).impl() };
        return binding;
    }
    bool isLackedByMock = false;
    if (const ReexportedFieldData* reexported = entry.isFromStarExport || entry.kind() == ModuleExportKind::Namespace ? nullptr : m_table.findReexportedField(module, index)) {
        LinkedBinding binding = follow(reexported->namedModule(), reexported->importName);
        RETURN_IF_EXCEPTION(scope, { });
        search.numberOfOrderedResults += search.numberOfRevisits != revisitsBefore;
        if (binding.type != Type::NotFound || search.numberOfRevisits != revisitsBefore)
            return binding;
        isLackedByMock = binding.isLackedByMock;
    }
    auto bindingOfMock = [&](ModuleNamespace* word, uint32_t nameInMock, bool isFromStarExport) -> LinkedBinding {
        bool hasName = !isLackedByMock || !isFromStarExport || hasNameInMock(word, identifier(nameInMock));
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasName)
            return { .isLackedByMock = true };
        return { Type::Resolved, Kind::Name, true, word, 0, identifier(nameInMock).impl() };
    };
    switch (entry.kind()) {
    case ModuleExportKind::Field: {
        ResolvedField resolved = resolveField(module, index, instance);
        if (resolved.mockWord)
            RELEASE_AND_RETURN(scope, bindingOfMock(resolved.mockWord, resolved.nameInMock, resolved.isFromStarExport));
        return { Type::Resolved, Kind::Field, false, resolved.instance, resolved.slot.encoded() };
    }
    case ModuleExportKind::Variable: {
        ResolvedVariable resolved = resolveVariable(module, index, instance);
        if (resolved.mockWord)
            RELEASE_AND_RETURN(scope, bindingOfMock(resolved.mockWord, resolved.nameInMock, resolved.isFromStarExport));
        return { Type::Resolved, Kind::Variable, false, nullptr, entry.variable.encoded() };
    }
    case ModuleExportKind::Namespace: {
        ResolvedNamespace resolved = resolveNamespace(module, index, instance);
        if (resolved.mockWord && resolved.nameInMock)
            RELEASE_AND_RETURN(scope, bindingOfMock(resolved.mockWord, *resolved.nameInMock, resolved.isFromStarExport));
        if (resolved.mockWord)
            return { Type::Resolved, Kind::Namespace, false, resolved.mockWord };
        return { Type::Resolved, Kind::Namespace, false, resolved.instance, entry.module };
    }
    case ModuleExportKind::Indirect:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto Modules::resolveBindingAmongStars(ModuleID module, const Identifier& name, Environment* instance, BindingSearch& search) -> LinkedBinding
{
    using Kind = LinkedBinding::Kind;
    using Type = LinkedBinding::Type;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LinkedBinding binding;
    bool isLackedByMock = false;
    auto merge = [&](const LinkedBinding& found) {
        search.numberOfOrderedResults += found.isOfMock;
        isLackedByMock |= found.isLackedByMock;
        if (found.type == Type::NotFound)
            return true;
        if (found.type == Type::Ambiguous) {
            binding = found;
            return false;
        }
        if (binding.type == Type::NotFound)
            binding = found;
        else if (!binding.isOfMock && !found.isOfMock && !binding.isSameBinding(found)) {
            binding = { Type::Ambiguous };
            return false;
        }
        return true;
    };
    const StarTargetsByName& stars = starTargetsByName(module);
    stars.visitCandidates(name.impl(), [&](unsigned place) {
        ModuleID target = stars.targets[place].module;
        const ModuleLinkData* link = findModuleLink(module, target);
        if (ModuleNamespace* word = link ? mockWord(*link, instance) : nullptr) {
            bool hasName = hasNameInMock(word, name);
            RETURN_IF_EXCEPTION(scope, false);
            isLackedByMock |= !hasName;
            return !hasName || merge({ Type::Resolved, Kind::Name, true, word, 0, name.impl() });
        }
        if (JSObject* mock = !hasInstances(module) && someMockIsSeen() ? mockSeenBy(target, module) : nullptr) [[unlikely]] {
            bool hasName = mock->hasProperty(m_instance.globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            return !hasName || merge({ Type::Resolved, Kind::Name, true, nullptr, (static_cast<uint64_t>(module.index()) << 32) | target.index(), name.impl() });
        }
        if (data(target).isESModuleOfProgram() && !data(target).isFacade()) {
            LinkedBinding found = resolveBinding(target, name, link && link->hasLink ? readLink(instance, link->link) : currentInstance(target), search);
            RETURN_IF_EXCEPTION(scope, false);
            return merge(found);
        }
        JSObject* source = activeMock(target) ? originalNamespace(target) : moduleNamespace(moduleLoadedBy(ModuleRequestData::create(target.index(), false, true)), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, false);
        bool hasName = source->hasProperty(m_instance.globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        return !hasName || merge({ Type::Resolved, Kind::Name, false, nullptr, fileOf(target).index(), name.impl() });
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (binding.type == Type::NotFound)
        binding.isLackedByMock = isLackedByMock;
    return binding;
}

bool Modules::isProvidedByDynamicStar(ModuleID module, unsigned index) const
{
    const ReexportedFieldData* reexported = m_table.findReexportedField(module, index);
    const Vector<PlaceOfExport, 1>& providers = starExportProviders(module)[index];
    if (!reexported && providers.isEmpty())
        return false;
    ModuleID provider = reexported ? reexported->namedModule() : providers.first().module;
    for (ModuleRequestData star : m_table.dynamicStars(module)) {
        if (star.targetModule() == provider)
            return true;
    }
    return false;
}

auto Modules::resolveBinding(ModuleID module, const Identifier& name, Environment* instance, BindingSearch& search) -> LinkedBinding
{
    using Type = LinkedBinding::Type;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwStackOverflowError(m_instance.globalObject, scope);
        return { };
    }
    BindingSearch::Query query { module.index() + 1, instance, name.impl() };
    auto& known = answersByLinks().bindings;
    if (auto kept = known.find({ instance, name.impl() }); instance && kept != known.end()) {
        LinkedBinding binding = kept->value.binding;
#if ASSERT_ENABLED
        if (!m_isAnsweringAnew && search.open.isEmpty()) {
            BindingSearch searchAnew;
            LinkedBinding bindingAnew = answerAnew([&] { return resolveBinding(module, name, instance, searchAnew); });
            RETURN_IF_EXCEPTION(scope, { });
            ASSERT(binding.type == bindingAnew.type);
            ASSERT(binding.type != Type::Resolved || (binding.isOfMock == bindingAnew.isOfMock && binding.isSameBinding(bindingAnew)));
        }
#endif
        return binding;
    }
    size_t place = search.open.size();
    if (auto visited = search.visited.add(query, place); !visited.isNewEntry) {
        ++search.numberOfRevisits;
        search.outermost = std::min(search.outermost, visited.iterator->value);
        return { };
    }
    search.open.append(query);
    size_t outermostBefore = std::exchange(search.outermost, BindingSearch::noQuery);
    size_t revisitsBefore = search.numberOfRevisits;
    size_t orderedBefore = search.numberOfOrderedResults;
    LinkedBinding binding;
    std::optional<unsigned> index = m_table.findExport(module, name.impl());
    bool isFromStarExport = !index || m_table.exports(module)[*index].isFromStarExport;
    if (!isFromStarExport) {
        binding = bindingOfExport(module, *index, instance, search);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (binding.type == Type::NotFound && name != vm.propertyNames->defaultKeyword) {
        bool isLackedByMock = binding.isLackedByMock;
        binding = resolveBindingAmongStars(module, name, instance, search);
        RETURN_IF_EXCEPTION(scope, { });
        if (binding.type == Type::NotFound)
            binding.isLackedByMock |= isLackedByMock;
        if (binding.type == Type::NotFound && index && isFromStarExport && !isProvidedByDynamicStar(module, *index)) {
            search.numberOfOrderedResults += search.numberOfRevisits != revisitsBefore;
            binding = bindingOfExport(module, *index, instance, search);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    size_t outermost = std::exchange(search.outermost, outermostBefore);
    if (outermost < place) {
        search.outermost = std::min(outermostBefore, outermost);
        search.visited.find(query)->value = outermost;
        return binding;
    }
    bool isSameForCycle = search.numberOfOrderedResults == orderedBefore;
    for (const BindingSearch::Query& each : search.open.span().subspan(place)) {
        if (each != query && !isSameForCycle) {
            search.visited.find(each)->value = 0;
            continue;
        }
        if (const Environment* instanceOfQuery = std::get<1>(each))
            answersByLinks().bindings.set({ instanceOfQuery, std::get<2>(each) }, AnswersByLinks::KeptBinding { binding, binding.name });
    }
    search.open.shrink(place);
    return binding;
}

bool Modules::isAmbiguousInLinks(ModuleID module, const Identifier& name, Environment* instance, BindingSearch& search)
{
    if (!isLinkedInstance(module, instance))
        return false;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    search = { };
    LinkedBinding binding = resolveBinding(module, name, instance, search);
    RETURN_IF_EXCEPTION(scope, false);
    return binding.type == LinkedBinding::Type::Ambiguous;
}

auto Modules::collectStarExports(ModuleID module, Environment* instance) -> Vector<DynamicExport>
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<DynamicExport> result;
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwStackOverflowError(realm, scope);
        return result;
    }
    if (!isLinkedInstance(module, instance))
        return result;
    Vector<std::pair<Identifier, JSObject*>> found;
    for (ModuleRequestData star : m_table.dynamicStars(module)) {
        const ModuleLinkData* link = findModuleLink(module, star.targetModule());
        ModuleNamespace* word = link ? mockWord(*link, instance) : nullptr;
        JSObject* source = word;
        if (!source && link && link->hasLink) {
            source = instanceNamespace(link->targetModule(), readLink(instance, link->link), ModulePhase::Evaluation);
        } else if (!source)
            source = activeMock(star.targetModule()) ? originalNamespace(star.targetModule()) : moduleNamespace(moduleLoadedBy(star), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        JSObject* namesOf = word ? (word->namesOfMock() ? word->namesOfMock() : word->linkedExports()) : source;
        PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        auto* listing = dynamicDowncast<ModuleNamespace>(namesOf);
        bool hasOnlyEnumerableNames = listing ? !isNativeModule(listing->module()) : namesOf->inherits<JSModuleNamespaceObject>();
        namesOf->methodTable()->getOwnPropertyNames(namesOf, realm, names, hasOnlyEnumerableNames ? DontEnumPropertiesMode::Include : DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, { });
        for (const Identifier& name : names) {
            if (name == vm.propertyNames->defaultKeyword)
                continue;
            std::optional<unsigned> own = m_table.findExport(module, name.impl());
            bool isOwn = own && !(m_table.exports(module)[*own].isHiddenUntilInitialized && isLeftOut(module, *own, TablesBehindStars::MayLackName));
            if (!isOwn && !(link && reexportsNameOf(link->targetModule(), name, module)))
                found.append({ name, source });
        }
    }
    IdentifierSet listed;
    BindingSearch search;
    for (auto& [name, source] : found) {
        if (!listed.add(name.impl()).isNewEntry)
            continue;
        bool isAmbiguous = isAmbiguousInLinks(module, name, instance, search);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isAmbiguous)
            result.append({ name, JSValue(), source });
    }
    AOT::sort(result, [](const DynamicExport& a, const DynamicExport& b) {
        return codePointCompare(StringView(*a.name.impl()), StringView(*b.name.impl())) < 0;
    });
    return result;
}

std::optional<Modules::StarResolution> Modules::resolveThroughStars(ModuleID module, const Identifier& name, Environment* instance, BitVector& seen)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!isLinkedInstance(module, instance) || seen.quickSet(module.index()))
        return std::nullopt;
    for (ModuleRequestData star : m_table.dynamicStars(module)) {
        const ModuleLinkData* link = findModuleLink(module, star.targetModule());
        if (!link) {
            if (data(star.targetModule()).isESModuleOfProgram() && !data(star.targetModule()).isFacade() && !data(star.targetModule()).hasDynamicStarExport())
                continue;
            JSObject* source = moduleNamespace(moduleLoadedBy(star), ModulePhase::Evaluation);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            bool hasName = source->hasProperty(m_instance.globalObject, name);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (hasName)
                return StarResolution { source };
            continue;
        }
        if (ModuleNamespace* word = mockWord(*link, instance)) {
            bool hasName = hasNameInMock(word, name);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (hasName)
                return StarResolution { nullptr, word };
            continue;
        }
        ModuleID target = link->targetModule();
        std::optional<unsigned> index = m_table.findExport(target, name.impl());
        Environment* linked = link->hasLink ? readLink(instance, link->link) : nullptr;
        if (!index) {
            if (linked) {
                std::optional<StarResolution> further = resolveThroughStars(target, name, linked, seen);
                RETURN_IF_EXCEPTION(scope, std::nullopt);
                if (further)
                    return further;
                continue;
            }
            JSObject* source = activeMock(target) ? originalNamespace(target) : moduleNamespace(moduleLoadedBy(star), ModulePhase::Evaluation);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            bool hasName = source->hasProperty(m_instance.globalObject, name);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (hasName)
                return StarResolution { source };
            continue;
        }
        if (reexportsNameOf(target, name, module))
            continue;
        const ModuleExportData& entry = m_table.exports(target)[*index];
        ModuleNamespace* word = nullptr;
        if (linked && entry.isFromStarExport && entry.kind() == ModuleExportKind::Field)
            word = resolveField(target, *index, linked).mockWord;
        else if (linked && entry.isFromStarExport && entry.kind() == ModuleExportKind::Variable)
            word = resolveVariable(target, *index, linked).mockWord;
        bool isLacking = word && !hasNameInMock(word, name);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (linked && !entry.isFromStarExport && (entry.kind() == ModuleExportKind::Indirect || m_table.findReexportedField(target, *index))) {
            BindingSearch search;
            LinkedBinding binding = resolveBinding(target, name, linked, search);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (binding.type == LinkedBinding::Type::NotFound && binding.isLackedByMock)
                continue;
        }
        if (!isLacking)
            return StarResolution { nullptr, nullptr, target, *index, linked };
        std::optional<StarResolution> further = resolveThroughStars(target, name, linked, seen);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (further)
            return further;
    }
    return std::nullopt;
}

void Modules::fixHiddenExports(ModuleNamespace& namespaceObject)
{
    ModuleID module = namespaceObject.module();
    Environment* instance = namespaceObject.boundModuleInstance();
    if (!isLinkedInstance(module, instance) || (namespaceObject.isAnsweredByMock() && m_moduleWhoseOwnNamespaceIsMade != module))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    KeepsAnswers keepsAnswers(*this);
    Vector<uint32_t, 8> hidden;
    bool isDecidedByLinks = !m_timesMocked.isEmpty() && hasOutdatedStarLink(module, instance);
    BindingSearch search;
    std::span<const ModuleExportData> exports = m_table.exports(module);
    for (unsigned index = 0; index < exports.size(); ++index) {
        if (!exports[index].isFromStarExport)
            continue;
        bool isHidden = isExportAmbiguous(module, index);
        bool isAmbiguous = isHidden;
        if (isDecidedByLinks) {
            isAmbiguous = isAmbiguousInLinks(module, identifier(exports[index].name), instance, search);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (isAmbiguous) {
            hidden.append(index);
            continue;
        }
        if (!isHidden && exports[index].kind() == ModuleExportKind::Namespace && m_table.findReexportedField(module, index)) {
            ResolvedNamespace resolved = resolveNamespace(module, index, instance);
            isHidden = resolved.mockWord && resolved.nameInMock && resolved.isFromStarExport && !hasNameInMock(resolved.mockWord, identifier(*resolved.nameInMock));
            RETURN_IF_EXCEPTION(scope, void());
        } else if (!isHidden && exports[index].kind() == ModuleExportKind::Variable && m_table.findReexportedField(module, index)) {
            ResolvedVariable resolved = resolveVariable(module, index, instance);
            isHidden = resolved.mockWord && resolved.isFromStarExport && !hasNameInMock(resolved.mockWord, identifier(resolved.nameInMock));
            RETURN_IF_EXCEPTION(scope, void());
            if (exports[index].isHiddenUntilInitialized && !resolved.mockWord)
                isHidden = isLeftOut(module, index, TablesBehindStars::MayLackName);
            else if (resolved.original)
                isHidden = isLeftOut(resolved.original->first, resolved.original->second, TablesBehindStars::MayLackName);
            else if (resolved.starHolder) {
                BitVector seen(m_table.size());
                isHidden = !resolveThroughStars(resolved.holderModule, identifier(resolved.nameInHolder), resolved.starHolder, seen);
                RETURN_IF_EXCEPTION(scope, void());
            } else if (!resolved.isFollowed)
                isHidden = isLeftOut(module, index, TablesBehindStars::MayLackName);
        } else if (!isHidden && exports[index].kind() != ModuleExportKind::Field)
            isHidden = isLeftOut(module, index, TablesBehindStars::MayLackName);
        else if (!isHidden && !m_timesMocked.isEmpty()) {
            ResolvedField resolved = resolveField(module, index, instance);
            isHidden = resolved.mockWord && resolved.isFromStarExport && !hasNameInMock(resolved.mockWord, identifier(resolved.nameInMock));
            RETURN_IF_EXCEPTION(scope, void());
            if (resolved.exportWithoutLinks)
                isHidden = isLeftOut(resolved.exportWithoutLinks->first, resolved.exportWithoutLinks->second, TablesBehindStars::MayLackName);
        }
        if (isHidden) {
            search = { };
            isHidden = resolveBinding(module, identifier(exports[index].name), instance, search).type != LinkedBinding::Type::Resolved;
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (isHidden)
            hidden.append(index);
    }
    JSCellButterfly* indexes = hidden.isEmpty() ? nullptr : JSCellButterfly::create(*m_instance.vm, CopyOnWriteArrayWithContiguous, hidden.size());
    for (size_t place = 0; place < hidden.size(); ++place)
        indexes->setIndex(*m_instance.vm, place, jsNumber(static_cast<int32_t>(hidden[place])));
    namespaceObject.setHiddenExports(*m_instance.vm, indexes);
}

JSObject* Modules::instanceNamespace(ModuleID module, Environment* instance, ModulePhase phase)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleInstanceData& record = m_table.instanceData(module);
    SlotData field = phase == ModulePhase::Defer ? record.deferredNamespaceField : record.namespaceField;
    if (JSValue created = readField(instance, field))
        return asObject(created);
    for (size_t place = 0; phase == ModulePhase::Evaluation && place < m_namespacesBeingMade.size(); ++place) {
        auto* beingMade = m_namespacesBeingMade[place].first == module ? dynamicDowncast<ModuleNamespace>(m_namespacesBeingMade[place].second) : nullptr;
        if (!beingMade || beingMade->boundModuleInstance() != instance)
            continue;
        m_outermostNamespaceReadWhileBeingMade = std::min(m_outermostNamespaceReadWhileBeingMade, place);
        return beingMade;
    }
    JSObject* object = nullptr;
    if (instance == currentInstance(module)) {
        bool willBePatched = !activeMock(module) && status(module) == ModuleStatus::Evaluated && !wasOriginalEvaluated(module) && !hasIntactOriginal(module);
        bool isUnderMock = phase == ModulePhase::Evaluation && (activeMock(module) || (isMocked(module) && !willBePatched));
        object = isUnderMock ? originalNamespace(module) : moduleNamespace(module, phase);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } else {
        ModuleNamespace* ofReleased = ModuleNamespace::create(vm, namespaceStructure(true), module, phase, { }, instance);
        ModuleNamespace::DynamicExports exports;
        size_t place = m_namespacesBeingMade.size();
        size_t outermostReadBefore = std::exchange(m_outermostNamespaceReadWhileBeingMade, notFound);
        m_namespacesBeingMade.append({ module, ofReleased });
        createDynamicExports(module, exports.names, exports.capturedValues, exports.liveSources, instance);
        m_namespacesBeingMade.removeLast();
        bool lacksNames = m_outermostNamespaceReadWhileBeingMade < place;
        m_outermostNamespaceReadWhileBeingMade = std::min(outermostReadBefore, m_outermostNamespaceReadWhileBeingMade);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ofReleased->replaceDynamicExports(vm, exports);
        fixHiddenExports(*ofReleased);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ofReleased->setKeepsToModule(true);
        if (lacksNames)
            return ofReleased;
        object = ofReleased;
    }
    auto* created = dynamicDowncast<ModuleNamespace>(object);
    auto lacking = std::ranges::find(m_namespacesThatLackNames, object, &NamespaceThatLacksNames::object);
    if (lacking != m_namespacesThatLackNames.end())
        m_outermostNamespaceReadWhileBeingMade = std::min(m_outermostNamespaceReadWhileBeingMade, lacking->outermostRead);
    else if (created && created->boundModuleInstance() == instance && !(phase == ModulePhase::Defer && isMocked(module) && instance == currentInstance(module) && !m_linkedInstances.get(module.index()))) {
        writeField(instance, field, object);
        created->setBelongsToInstance();
    }
    return object;
}

JSObject* Modules::instanceImportMeta(ModuleID module, Environment* instance)
{
    auto originals = m_originalsByFile.find(fileOf(module).index());
    if (originals != m_originalsByFile.end()) [[unlikely]] {
        for (const OriginalInstance& original : originals->value) {
            if (original.instance.get() == instance)
                return createImportMeta(jsString(*m_instance.vm, original.key.string()));
        }
    }
    if (instance != currentInstance(module))
        return createImportMeta(pathString(module));
    bool isOriginal = m_moduleEvaluatedAsOriginal == module || (isMocked(module) && (m_originalsDueUnderMock.get(module.index()) || m_originalsEvaluatedUnderMock.get(module.index())));
    return isOriginal ? createImportMeta(jsString(*m_instance.vm, actualKey(fileOf(module)).string())) : importMeta(module);
}

JSObject* Modules::namespaceInInstance(ModuleID module, ModulePhase phase) const
{
    Environment* instance = currentInstance(module);
    if (!instance)
        return nullptr;
    const ModuleInstanceData& record = m_table.instanceData(module);
    JSValue created = readField(instance, phase == ModulePhase::Defer ? record.deferredNamespaceField : record.namespaceField);
    return created ? asObject(created) : nullptr;
}

void Modules::storeInInstance(ModuleNamespace& object)
{
    ModuleID module = object.module();
    Environment* instance = object.boundModuleInstance();
    if (!instance || instance != currentInstance(module) || !m_linkedInstances.get(module.index()) || object.isAnsweredByMock())
        return;
    const ModuleInstanceData& record = m_table.instanceData(module);
    SlotData field = object.isDeferred() ? record.deferredNamespaceField : record.namespaceField;
    if (readField(instance, field))
        return;
    writeField(instance, field, &object);
    object.setBelongsToInstance();
}

JSValue Modules::readInitializedLinkedExport(ModuleID module, unsigned index, Environment* instance) const
{
    const ModuleExportData& moduleExport = m_table.exports(module)[index];
    if (moduleExport.kind() == ModuleExportKind::Field) {
        ResolvedField resolved = resolveField(module, index, instance);
        return resolved.exportWithoutLinks ? JSValue() : readField(resolved.instance, resolved.slot);
    }
    if (!instance)
        instance = currentInstance(module);
    if (moduleExport.kind() != ModuleExportKind::Namespace)
        return { };
    ResolvedNamespace resolved = resolveNamespace(module, index, instance);
    if (resolved.mockWord)
        return resolved.nameInMock ? JSValue() : JSValue(resolved.mockWord);
    if (!resolved.instance || handedOnInPlaceOfNamespace(moduleExport))
        return { };
    const ModuleInstanceData& target = m_table.instanceData(*resolved.target);
    return readField(resolved.instance, phaseOfNamespaceExport(moduleExport) == ModulePhase::Defer ? target.deferredNamespaceField : target.namespaceField);
}

bool Modules::hasOutdatedLinkForName(ModuleID module, const Identifier& name, Environment* instance) const
{
    struct Step {
        ModuleID module;
        Identifier name;
        Environment* instance;
    };
    UncheckedKeyHashSet<std::pair<uint32_t, UniquedStringImpl*>> seen;
    Vector<Step, 8> work { { module, name, instance } };
    while (!work.isEmpty()) {
        Step step = work.takeLast();
        if (!seen.add({ step.module.index() + 1, step.name.impl() }).isNewEntry)
            continue;
        if (step.instance != currentInstance(step.module) || !isLinkedInstance(step.module, step.instance))
            return true;
        bool isOutdated = false;
        auto follow = [&](ModuleID target, const Identifier& targetName) {
            const ModuleLinkData* link = findModuleLink(step.module, target);
            if (!link)
                return;
            ModuleNamespace* word = mockWord(*link, step.instance);
            Environment* linked = link->hasLink ? readLink(step.instance, link->link) : nullptr;
            bool seesOriginal = !word && linked == (link->hasLink ? currentInstance(link->targetModule()) : nullptr) && importsOriginalDespiteMock(target, step.module);
            isOutdated |= !seesOriginal && !isLinkedToCurrentEntry(step.module, target).value_or(true);
            if (!word && linked)
                work.append({ link->targetModule(), targetName, linked });
        };
        std::optional<unsigned> index = m_table.findExport(step.module, step.name.impl());
        if (index) {
            const ModuleExportData& entry = m_table.exports(step.module)[*index];
            if (entry.kind() == ModuleExportKind::Indirect) {
                const IndirectExportData& indirect = m_table.indirectExport(entry);
                follow(indirect.targetModule(), identifier(indirect.importName));
            } else if (const ReexportedFieldData* reexported = m_table.findReexportedField(step.module, *index))
                follow(reexported->namedModule(), identifier(reexported->importName));
        }
        if (!index || m_table.exports(step.module)[*index].isFromStarExport) {
            for (ModuleRequestData star : m_table.dynamicStars(step.module))
                follow(star.targetModule(), step.name);
        }
        if (isOutdated)
            return true;
    }
    return false;
}

bool Modules::isReadThroughStars(ModuleID module, unsigned index, Environment* instance) const
{
    if (m_timesMocked.isEmpty() || !m_table.exports(module)[index].isFromStarExport) [[likely]]
        return false;
    return hasOutdatedStarLink(module, instance);
}

JSValue Modules::readThroughStars(ModuleID module, uint32_t name, Environment* instance)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwStackOverflowError(realm, scope);
        return { };
    }
    std::optional<StarResolution> resolution = followStars(module, identifier(name), instance);
    RETURN_IF_EXCEPTION(scope, { });
    if (!resolution)
        return { };
    if (resolution->source)
        RELEASE_AND_RETURN(scope, resolution->source->get(realm, identifier(name)));
    if (resolution->mockWord)
        RELEASE_AND_RETURN(scope, readLinkedMock(resolution->mockWord, name));
    if (resolution->isInStarCycle) {
        JSValue value;
        bool isAnswered = tryReadLinkedExport(resolution->module, resolution->exportIndex, resolution->instance, value, StarMode::Ignore);
        RETURN_IF_EXCEPTION(scope, { });
        if (isAnswered)
            return value;
    }
    RELEASE_AND_RETURN(scope, readExport(resolution->module, resolution->exportIndex, MockMode::Bypass, resolution->instance));
}

auto Modules::followStars(ModuleID module, const Identifier& name, Environment* instance) -> std::optional<StarResolution>
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::optional<StarResolution> resolution;
    Vector<Environment*, 8> passed;
    UncheckedKeyHashSet<Environment*> seenInstances;
    bool isInStarCycle = false;
    while (true) {
        if (auto known = answersByLinks().starResolutions.find({ instance, name.impl() }); known != answersByLinks().starResolutions.end()) {
            resolution = known->value;
#if ASSERT_ENABLED
            if (!m_isAnsweringAnew) {
                std::optional<StarResolution> resolutionAnew = answerAnew([&] { return followStars(module, name, instance); });
                RETURN_IF_EXCEPTION(scope, std::nullopt);
                ASSERT(resolutionAnew && resolution->source == resolutionAnew->source && resolution->mockWord == resolutionAnew->mockWord);
                ASSERT(resolution->mockWord || (resolution->module == resolutionAnew->module && resolution->exportIndex == resolutionAnew->exportIndex && resolution->instance == resolutionAnew->instance));
            }
#endif
            break;
        }
        BitVector seen(m_table.size());
        std::optional<StarResolution> next = resolveThroughStars(module, name, instance, seen);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!next)
            break;
        passed.append(instance);
        seenInstances.add(instance);
        resolution = next;
        if (next->source || next->mockWord || !next->instance || !isReadThroughStars(next->module, next->exportIndex, next->instance))
            break;
        isInStarCycle = seenInstances.contains(next->instance);
        if (isInStarCycle) {
            resolution->isInStarCycle = true;
            break;
        }
        module = next->module;
        instance = next->instance;
    }
    if (resolution && !resolution->source && !isInStarCycle) {
        for (Environment* each : passed)
            answersByLinks().starResolutions.set({ each, name.impl() }, *resolution);
    }
    return resolution;
}

JSValue Modules::readBinding(const LinkedBinding& binding, const Identifier& name)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Kind = LinkedBinding::Kind;
    JSValue value;
    switch (binding.kind) {
    case Kind::Field:
        value = readField(static_cast<const Environment*>(binding.holder), SlotData::decode(binding.place));
        break;
    case Kind::Variable:
        value = ProgramStatics::readVariable(m_instance, SlotData::decode(binding.place));
        break;
    case Kind::Name: {
        Identifier nameInHolder = Identifier::fromUid(vm, binding.name);
        if (binding.isOfMock && binding.holder)
            RELEASE_AND_RETURN(scope, readLinkedMock(static_cast<ModuleNamespace*>(const_cast<void*>(binding.holder)), nameInHolder));
        if (binding.isOfMock) {
            JSObject* mock = mockSeenBy(ModuleID(static_cast<uint32_t>(binding.place)), ModuleID(static_cast<uint32_t>(binding.place >> 32)));
            if (!mock)
                return { };
            RELEASE_AND_RETURN(scope, mock->get(realm, nameInHolder));
        }
        JSObject* source = moduleNamespace(moduleWithExportsOf(ModuleID(static_cast<uint32_t>(binding.place))), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, source->get(realm, nameInHolder));
    }
    case Kind::Namespace:
        return { };
    }
    if (!value) [[unlikely]]
        throwException(realm, scope, createTDZError(realm, StringView(name.string())));
    return value;
}

JSValue Modules::readMockOrStar(ModuleID module, unsigned index, Environment* instance, ModuleNamespace* word, uint32_t nameInMock, bool isFromStarExport)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (isFromStarExport) {
        bool hasName = hasNameInMock(word, identifier(nameInMock));
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasName) {
            JSValue value = readThroughStars(module, m_table.exports(module)[index].name, instance);
            RETURN_IF_EXCEPTION(scope, { });
            if (value)
                return value;
            BindingSearch search;
            Identifier name = identifier(m_table.exports(module)[index].name);
            LinkedBinding binding = resolveBinding(module, name, instance, search);
            RETURN_IF_EXCEPTION(scope, { });
            if (binding.type == LinkedBinding::Type::Resolved) {
                value = readBinding(binding, name);
                RETURN_IF_EXCEPTION(scope, { });
                if (value)
                    return value;
            }
        }
    }
    RELEASE_AND_RETURN(scope, readLinkedMock(word, nameInMock));
}

bool Modules::tryReadLinkedExport(ModuleID module, unsigned index, Environment* instance, JSValue& value, StarMode starMode)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!instance)
        instance = currentInstance(module);
    const ModuleExportData& moduleExport = m_table.exports(module)[index];
    if (starMode == StarMode::Follow && isReadThroughStars(module, index, instance)) [[unlikely]] {
        value = readThroughStars(module, moduleExport.name, instance);
        RETURN_IF_EXCEPTION(scope, true);
        if (value)
            return true;
    }
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        ResolvedVariable resolved = resolveVariable(module, index, instance);
        BitVector seen(m_table.size());
        std::optional<StarResolution> resolution = resolved.starHolder ? resolveThroughStars(resolved.holderModule, identifier(resolved.nameInHolder), resolved.starHolder, seen) : std::nullopt;
        RETURN_IF_EXCEPTION(scope, true);
        if (resolution && resolution->mockWord)
            value = readLinkedMock(resolution->mockWord, resolved.nameInHolder);
        else if (moduleExport.isHiddenUntilInitialized && !resolved.mockWord) {
            return false;
        } else if (resolved.mockWord)
            value = readMockOrStar(module, index, instance, resolved.mockWord, resolved.nameInMock, resolved.isFromStarExport);
        else if (resolved.original)
            value = readExportOfModuleItself(resolved.original->first, resolved.original->second);
        else if (resolved.isFollowed) {
            value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
            if (!value) [[unlikely]] {
                DeferGCForAWhile deferGC(*m_instance.vm);
                throwException(realm, scope, createTDZError(realm, StringView(identifier(moduleExport.name).string())));
                return true;
            }
        } else
            return false;
        RETURN_IF_EXCEPTION(scope, true);
        return true;
    }
    case ModuleExportKind::Field: {
        ResolvedField resolved = resolveField(module, index, instance);
        if (resolved.mockWord) [[unlikely]] {
            value = readMockOrStar(module, index, instance, resolved.mockWord, resolved.nameInMock, resolved.isFromStarExport);
            RETURN_IF_EXCEPTION(scope, true);
            return true;
        }
        if (resolved.exportWithoutLinks) [[unlikely]] {
            value = readExportOfModuleItself(resolved.exportWithoutLinks->first, resolved.exportWithoutLinks->second);
            RETURN_IF_EXCEPTION(scope, true);
            return true;
        }
        value = readField(resolved.instance, resolved.slot);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(*m_instance.vm);
            throwException(realm, scope, createTDZError(realm, StringView(identifier(moduleExport.name).string())));
        }
        return true;
    }
    case ModuleExportKind::Namespace: {
        ResolvedNamespace resolved = resolveNamespace(module, index, instance);
        ModulePhase phase = phaseOfNamespaceExport(moduleExport);
        if (resolved.mockWord && resolved.nameInMock)
            value = readMockOrStar(module, index, instance, resolved.mockWord, *resolved.nameInMock, resolved.isFromStarExport);
        else if (resolved.mockWord)
            value = resolved.mockWord;
        else if (resolved.absent)
            value = resolved.absent;
        else if (handedOnInPlaceOfNamespace(moduleExport))
            return false;
        else if (resolved.original)
            value = readExportOfModuleItself(resolved.original->first, resolved.original->second);
        else if (resolved.instance)
            value = instanceNamespace(*resolved.target, resolved.instance, phase);
        else if (resolved.target && phase == ModulePhase::Evaluation)
            value = originalNamespace(*resolved.target);
        else
            return false;
        RETURN_IF_EXCEPTION(scope, true);
        return true;
    }
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirect = m_table.indirectExport(moduleExport);
        const ModuleLinkData* link = isLinkedInstance(module, instance) ? findModuleLink(module, indirect.targetModule()) : nullptr;
        if (link && data(link->targetModule()).isUncompiledCode()) {
            JSValue absent = link->hasMock ? readField(instance, link->mock) : JSValue();
            if (!absent || !absent.isObject())
                return false;
            value = asObject(absent)->get(realm, identifier(indirect.importName));
            RETURN_IF_EXCEPTION(scope, true);
            return true;
        }
        bool isBeingMade = std::ranges::contains(m_namespacesBeingMade, indirect.targetModule(), &std::pair<ModuleID, JSObject*>::first);
        if (!link || (!isBeingMade && ((m_timesMocked.isEmpty() && !m_hasReleasedInstances) || !hasOutdatedLinkForName(module, identifier(moduleExport.name), instance))))
            return false;
        if (ModuleNamespace* word = mockWord(*link, instance)) {
            value = readLinkedMock(word, indirect.importName);
            RETURN_IF_EXCEPTION(scope, true);
            return true;
        }
        if (!link->hasLink)
            return false;
        JSObject* namespaceObject = instanceNamespace(link->targetModule(), readLink(instance, link->link), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, true);
        value = namespaceObject->get(realm, identifier(indirect.importName));
        RETURN_IF_EXCEPTION(scope, true);
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Modules::isLinkedToReleasedInstance(ModuleID importer, ModuleID target) const
{
    if (!m_hasReleasedInstances) [[likely]]
        return false;
    Environment* instance = currentInstance(importer);
    const ModuleLinkData* link = instance && m_linkedInstances.get(importer.index()) ? findModuleLink(importer, target) : nullptr;
    if (!link)
        return false;
    if (link->hasLink)
        return readLink(instance, link->link) != currentInstance(target);
    JSValue stored = link->hasMock ? readField(instance, link->mock) : JSValue();
    auto* word = stored ? dynamicDowncast<ModuleNamespace>(stored) : nullptr;
    return word && word->isReleasedLoad();
}

bool Modules::isEvaluatedWithOriginal(ModuleID importer, ModuleID mocked) const
{
    Environment* instance = m_linkedInstances.get(importer.index()) ? currentInstance(importer) : nullptr;
    const ModuleLinkData* link = instance && status(importer) == ModuleStatus::Evaluated ? findModuleLink(importer, mocked) : nullptr;
    if (!link || !link->hasLink || !link->hasMock)
        return false;
    JSValue word = readField(instance, link->mock);
    return word.isUndefined() || uncheckedDowncast<ModuleNamespace>(word)->isLoadWord();
}

void Modules::noteReleasedFailure(ModuleID module, ModuleRecord* record)
{
    LoadedModule& loaded = ensureLoaded(module);
    loaded.releasedFailedRecord.set(*this, record);
    loaded.failureLinkMoment = m_linkMoments.isEmpty() ? 0 : m_linkMoments[module.index()];
    loaded.failureReleaseMoment = m_numberOfReleases;
}

JSValue Modules::releasedInstanceError(ModuleID importer, ModuleID target) const
{
    if (!m_hasReleasedInstances || !m_linkedInstances.get(importer.index())) [[likely]]
        return { };
    const LoadedModule* loaded = loadedIfExists(target);
    auto* record = loaded ? dynamicDowncast<ModuleRecord>(loaded->releasedFailedRecord.get()) : nullptr;
    if (!record)
        return { };
    Environment* instance = currentInstance(importer);
    const ModuleLinkData* link = instance ? findModuleLink(importer, target) : nullptr;
    if (link && (!link->hasLink || mockWord(*link, instance)))
        return { };
    uint32_t moment = m_linkMoments[importer.index()];
    bool isLinked = link ? readLink(instance, link->link) == record->moduleInstance() : moment >= loaded->failureLinkMoment && moment < loaded->failureReleaseMoment;
    return isLinked ? record->evaluationError() : JSValue();
}

bool Modules::reexportsFrom(ModuleID module, ModuleID importer, ModuleID imported) const
{
    for (ModuleRequestData star : m_table.dynamicStars(importer)) {
        if (star.targetModule() == imported)
            return true;
    }
    if (!data(importer).hasExportTable())
        return false;
    return std::ranges::any_of(m_table.exports(importer), [&](const ModuleExportData& entry) {
        switch (entry.kind()) {
        case ModuleExportKind::Field:
            return m_table.exportedField(entry).declaringModule() == module;
        case ModuleExportKind::Namespace:
            return entry.targetModule() == module;
        case ModuleExportKind::Indirect:
            return m_table.indirectExport(entry).targetModule() == imported;
        case ModuleExportKind::Variable:
            return false;
        }
        return false;
    });
}

bool Modules::areReadByImportersWithoutInstances(std::span<const ModuleID> modules)
{
    listImportersAndRequirers();
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work;
    for (ModuleID module : modules) {
        if (!seen.quickSet(module.index()))
            work.append(module);
    }
    while (!work.isEmpty()) {
        for (uint32_t index : m_importersByStatement[work.takeLast().index()]) {
            ModuleID importer(index);
            if (seen.quickSet(index) || status(importer) != ModuleStatus::Evaluated)
                continue;
            if (hasInstances(importer))
                work.append(importer);
            else if (!m_instance.vm->aotState().filesStartedByHost.get(index))
                return true;
        }
    }
    return false;
}

bool Modules::hasEvaluatedImporterWithoutInstances(ModuleID module)
{
    listImportersAndRequirers();
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 8> work { module };
    while (!work.isEmpty()) {
        ModuleID imported = work.takeLast();
        for (uint32_t index : m_importersByStatement[imported.index()]) {
            ModuleID importer(index);
            if (status(importer) != ModuleStatus::Evaluated || seen.quickSet(index))
                continue;
            if (hasInstances(importer)) {
                if (reexportsFrom(module, importer, imported))
                    work.append(importer);
            } else if (!m_instance.vm->aotState().filesStartedByHost.get(index))
                return true;
        }
    }
    return false;
}

bool Modules::isActiveMockRegistered(ModuleID module) const
{
    ModuleID file = m_table.moduleBehindFacade(module).value_or(module);
    const LoadedModule* held = loadedIfExists(file);
    const LoadedModule* exporting = loadedIfExists(moduleWithExportsOf(file));
    return activeMock(file) && held && held->mockWord && !(exporting && exporting->nextRequestReloadsActiveMock);
}

std::optional<bool> Modules::isLinkedToCurrentEntry(ModuleID importer, ModuleID target) const
{
    return isLinkedToCurrentEntry(importer, target, m_linkedInstances.get(importer.index()) ? currentInstance(importer) : nullptr);
}

std::optional<bool> Modules::isLinkedToCurrentEntry(ModuleID importer, ModuleID target, Environment* instance) const
{
    const ModuleLinkData* link = instance ? findModuleLink(importer, target) : nullptr;
    if (!link || data(target).isUncompiledCode())
        return std::nullopt;
    ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
    const LoadedModule* held = loadedIfExists(file);
    JSValue word = link->hasMock ? readField(instance, link->mock) : jsUndefined();
    if (activeMock(file)) {
        const LoadedModule* exporting = loadedIfExists(moduleWithExportsOf(file));
        bool isRegistered = !(exporting && exporting->nextRequestReloadsActiveMock);
        auto isWord = [&](JSObject* ofEntry) {
            return ofEntry && word == JSValue(ofEntry);
        };
        return isRegistered && held && (isWord(held->mockWord) || isWord(held->earlierPreloadMockState));
    }
    if (hasIntactOriginal(file))
        return false;
    if (held && held->reloadedWord && word == JSValue(held->reloadedWord.get()))
        return true;
    if (isReloadedLibraryModule(file))
        return false;
    if (const LoadedModule* patched = word.isUndefined() ? nullptr : loadedIfExists(moduleWithExportsOf(file)); patched && patched->patchWord && word == JSValue(patched->patchWord.get()))
        return true;
    auto* object = word ? dynamicDowncast<ModuleNamespace>(word) : nullptr;
    bool isLinkedToLoad = object && object->isLoadWord();
    const LoadedModule* exporting = isLinkedToLoad ? loadedIfExists(moduleWithExportsOf(file)) : nullptr;
    if (isLinkedToLoad ? !exporting || exporting->loadWord.get() != object : !word.isUndefined())
        return false;
    if (!link->hasLink && (isEvicted(file) || m_mocksWhoseEntryProgramDeleted.get(file.index())))
        return false;
    return !link->hasLink || readLink(instance, link->link) == currentInstance(link->targetModule());
}

void Modules::releaseEndedMockImporters()
{
    if (!std::exchange(m_hasMockEnded, false)) [[likely]]
        return;
    Vector<ModuleID, 16> modules;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        Environment* instance = currentInstance(module);
        if (!instance || !m_linkedInstances.get(index) || m_loadedInPreload.get(index))
            continue;
        bool isLinkedToEndedMock = std::ranges::any_of(m_table.moduleLinks(module), [&](const ModuleLinkData& link) {
            JSValue stored = !link.isResolved && link.hasMock ? readField(instance, link.mock) : JSValue();
            auto* word = stored ? dynamicDowncast<ModuleNamespace>(stored) : nullptr;
            const LoadedModule* held = word ? loadedIfExists(link.targetModule()) : nullptr;
            bool isReloadedLibraryState = word && word->keepsToModule() && data(link.targetModule()).isOfLibrary();
            const LoadedModule* patched = word ? loadedIfExists(moduleWithExportsOf(link.targetModule())) : nullptr;
            return word && !(patched && patched->patchWord.get() == word) && !word->isLoadWord() && !isReloadedLibraryState && !(held && ((held->mockWord.get() == word && activeMock(link.targetModule())) || held->pendingMockWord.get() == word));
        });
        if (isLinkedToEndedMock)
            modules.append(module);
    }
    for (ModuleID module : modules)
        releaseModuleWithImporters(module);
}

bool Modules::isLinkedAndUnevaluated(ModuleID module) const
{
    return hasInstances(module) && m_linkedInstances.get(module.index()) && status(module) <= ModuleStatus::Linked && !hasRunBesideMockAlone(module);
}

void Modules::resetLinkedInstances(const WTF::Function<bool(const String&)>& isKept)
{
    if (!m_table.anyModuleHasInstances()) [[likely]]
        return;
    m_modulesLinkedBeforeFailures.clear();
    m_modulesBeforeLinkFailures.clearAll();
    m_modulesWithPrivateCopy.clearAll();
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        if (currentInstance(module) && m_linkedInstances.get(index) && status(module) <= ModuleStatus::Linked && !isKept(m_table.fullPath(module)))
            tryReleaseModule(module);
    }
}

void Modules::releaseImportersOfEvictedMock(ModuleID module)
{
    releaseImportersOf(module, ReleasedImporters::All);
    if (LoadedModule* loaded = loadedIfExists(moduleWithExportsOf(module)))
        loaded->wasImportedUnderActiveMock = false;
}

bool Modules::releaseModuleWithImporters(ModuleID module)
{
    bool isPatched = isPatchedInPreload(module);
    if (!isPatched && !mayReleaseModule(module))
        return false;
    releaseImportersOf(module, ReleasedImporters::All);
    if (!isPatched)
        return tryReleaseModule(module);
    forgetPatch(module);
    releaseLoadedModule(module);
    return true;
}

void Modules::releaseInstance(ModuleID module)
{
    Environment* instance = currentInstance(module);
    if (!instance)
        return;
    const ModuleInstanceData& record = m_table.instanceData(module);
    for (ModulePhase phase : { ModulePhase::Evaluation, ModulePhase::Defer }) {
        auto* object = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, phase));
        if (!object)
            continue;
        object->leaveStructureThatCachesKnow(*m_instance.vm);
        if (object->boundModuleInstance() == instance && !object->isAnsweredByMock()) {
            writeField(instance, phase == ModulePhase::Defer ? record.deferredNamespaceField : record.namespaceField, object);
            object->setBelongsToInstance();
            object->setKeepsToModule(true);
        }
    }
    if (LoadedModule* loaded = loadedIfExists(module)) {
        auto* own = dynamicDowncast<ModuleNamespace>(loaded->namespaceBeforeMockInPlace.get());
        if (own && own->boundModuleInstance() == instance) {
            writeField(instance, record.namespaceField, own);
            own->setBelongsToInstance();
            own->setKeepsToModule(true);
            loaded->namespaceBeforeMockInPlace.set(*this, nullptr);
        }
        loaded->deferredNamespaceObject.set(*this, nullptr);
    }
    discardNamespace(module);
    ProgramStatics::writeVariable(m_instance, *m_table.instanceGuard(module), jsBoolean(false));
    ++m_numberOfReleases;
    didChangeLinkState();
    m_linkedInstances.clear(module.index());
    m_linkedByFailedLoads.clear(module.index());
    m_failedLoadTestFiles.remove(module);
    m_hasReleasedInstances = true;
    m_modulesLinkedAfterEntryPoint.set(module.index());
    for (size_t reexporter : std::exchange(m_modulesCachingInstances, { })) {
        if (auto* object = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(ModuleID(reexporter), ModulePhase::Evaluation)))
            object->leaveStructureThatCachesKnow(*m_instance.vm);
    }
}

void Modules::noteImportInFlight(JSPromise* importPromise, ModuleID module)
{
    Environment* instance = hasInstances(module) && status(module) == ModuleStatus::Evaluated ? currentInstance(module) : nullptr;
    if (!instance || activeMock(module))
        return;
    if (m_importsInFlight.add(importPromise, ImportInFlight { module, instance, m_instance.vm->aotState().momentOfLoads }).isNewEntry)
        m_importsBeforeFirstStep.add(module);
    m_storedReferences.recordAll();
}

bool Modules::hasMockRegisteredAfter(ModuleID module, uint32_t moment) const
{
    if (!m_modulesWithPendingMock.get(module.index()))
        return false;
    auto call = m_pendingMockCalls.find(module);
    return call != m_pendingMockCalls.end() && call->value.moment > moment;
}

static JSC_DECLARE_HOST_FUNCTION(hostImportStep);

JSC_DEFINE_HOST_FUNCTION(hostImportStep, (JSGlobalObject* realm, CallFrame* callFrame))
{
    if (Modules* modules = modulesOf(realm))
        modules->didTakeFirstHostImportStep(uncheckedDowncast<JSPromise>(callFrame->argument(1)));
    return JSValue::encode(jsUndefined());
}

void noteHostImportInFlight(JSGlobalObject* realm, ModuleID module, JSPromise* importPromise, bool isFromOwnFactory)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteHostImportInFlight(importPromise, module, isFromOwnFactory);
}

void Modules::noteHostImportInFlight(JSPromise* importPromise, ModuleID module, bool isFromOwnFactory)
{
    bool joinsImportCall = m_importsBeforeFirstStep.count(module) > m_hostImportsBeforeFirstStep.count(module);
    noteImportInFlight(importPromise, module);
    auto found = m_importsInFlight.find(importPromise);
    if (found == m_importsInFlight.end())
        return;
    found->value.isFromOwnFactory = isFromOwnFactory;
    found->value.joinsImportCall = joinsImportCall;
    m_hostImportsBeforeFirstStep.add(module);
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSPromise* tick = JSPromise::create(vm, realm->promiseStructure());
    tick->fulfill(vm, jsUndefined());
    JSFunction* didStep = JSFunction::create(vm, realm, 2, String(), hostImportStep, ImplementationVisibility::Private);
    tick->performPromiseThenWithContext(vm, realm, didStep, didStep, jsUndefined(), importPromise);
}

void Modules::didTakeFirstHostImportStep(JSPromise* importPromise)
{
    auto found = m_importsInFlight.find(importPromise);
    if (found == m_importsInFlight.end())
        return;
    ImportInFlight import = found->value;
    m_importsInFlight.remove(found);
    m_importsBeforeFirstStep.remove(import.module);
    m_hostImportsBeforeFirstStep.remove(import.module);
    if (import.joinsImportCall && import.instance != currentInstance(import.module) && (isMocked(import.module) || isMockUnseen(import.module)) && awaitedMocksBelow(import.module, false).isEmpty() && host().didRunOriginal) [[unlikely]]
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(import.module), OriginalRun::MockedDuringImport);
    if (import.isFromOwnFactory)
        return;
    if (import.instance != currentInstance(import.module)) [[unlikely]]
        forgetMockRegisteredAfter(import.module, import.moment);
    else if (hasMockRegisteredAfter(import.module, import.moment)) [[unlikely]]
        tellThatFileDidNotRunAgain(import.module);
}

bool Modules::tryRestartReleasedImport(JSPromise* importPromise)
{
    if (m_importsInFlight.isEmpty()) [[likely]]
        return false;
    auto found = m_importsInFlight.find(importPromise);
    if (found == m_importsInFlight.end())
        return false;
    ModuleID module = found->value.module;
    m_importsBeforeFirstStep.remove(module);
    if (found->value.instance == currentInstance(module)) {
        if (hasMockRegisteredAfter(module, found->value.moment)) [[unlikely]]
            tellThatFileDidNotRunAgain(module);
        return false;
    }
    SetForScope callSequenceNumber(m_resumingCallSequenceNumber, std::optional(found->value.moment));
    m_importsInFlight.remove(found);
    tellOfFactoriesInCycle(module);
    startImportWhoseTextHasCome(importPromise, module, std::nullopt, importPromise);
    return true;
}

JSObject* Modules::takeReleasedImportNamespace(JSPromise* importPromise, ModuleID module)
{
    if (m_importsInFlight.isEmpty()) [[likely]]
        return nullptr;
    auto found = m_importsInFlight.find(importPromise);
    if (found == m_importsInFlight.end())
        return nullptr;
    Environment* instance = found->value.instance;
    m_importsInFlight.remove(found);
    if (instance == currentInstance(module))
        return nullptr;
    return instanceNamespace(module, instance, ModulePhase::Evaluation);
}

bool Modules::mayReleaseModule(ModuleID module)
{
    if (data(fileOf(module)).isReloadable())
        return mayReleaseCommonJSFile(fileOf(module));
    if (!hasInstances(module) || isEvicted(module) || isMocked(module) || activeMock(module) || hasIntactOriginal(module) || isPatchedByMock(module))
        return false;
    ModuleStatus current = status(module);
    if (!currentInstance(module))
        return current == ModuleStatus::Failed;
    if (current <= ModuleStatus::Linked)
        return m_linkedInstances.get(module.index()) && (!data(module).needsEvaluation() || m_linkedByFailedLoads.get(module.index()) || (!isSomeLoadUnderWay() && !isGraphRunning()));
    return current == ModuleStatus::Evaluated || current == ModuleStatus::Failed;
}

bool Modules::mayReleaseCommonJSFile(ModuleID file)
{
    if (!data(file).isReloadable() || status(file) != ModuleStatus::Evaluated || isEvicted(file) || isMocked(file) || activeMock(file) || hasIntactOriginal(file) || isPatchedByMock(file))
        return false;
    ModuleID facade = moduleWithExportsOf(file);
    if (m_filesThatRanInPreload.get(file.index()) || m_loadedInPreload.get(file.index()) || m_loadedInPreload.get(facade.index()) || isAwaitedByImport(file))
        return false;
    if (const LoadedModule* loaded = loadedIfExists(file); loaded && loaded->isLinkedWithoutNotice)
        return false;
    if (facade == file)
        return true;
    if (isReexportedByUnlistedName(facade))
        return false;
    ModuleStatus statusOfFacade = status(facade);
    if (statusOfFacade == ModuleStatus::Linking || statusOfFacade == ModuleStatus::Evaluating || statusOfFacade == ModuleStatus::EvaluatingAsync)
        return false;
    listImportersAndRequirers();
    if (!m_importersByStatement[facade.index()].isEmpty() && hasEvaluatedImporterWithoutInstances(facade))
        return false;
    const LoadedModule* exporting = loadedIfExists(facade);
    if (!exporting)
        return true;
    if (exporting->didDropError || exporting->exportsWhatMockNamesAlone || exporting->deferredExportsSource)
        return false;
    auto* word = static_cast<ModuleNamespace*>(exporting->loadWord.get());
    if (!word || word->isFilled())
        return !word || word == namespaceIfCreated(facade, ModulePhase::Evaluation) || !namespaceIfCreated(facade, ModulePhase::Evaluation);
    return !namespaceIfCreated(facade, ModulePhase::Evaluation) && didCaptureExports(facade) && !exporting->bindsEarlierCapture;
}

bool Modules::isHandedOnOverDynamicStar(ModuleID module, uint32_t name) const
{
    for (uint32_t step = 0; step < m_table.size(); ++step) {
        std::optional<unsigned> index = m_table.findExport(module, identifier(name).impl());
        if (!index)
            return step ? data(module).hasDynamicStarExport() : !data(module).hasDynamicStarExport();
        if (m_table.exports(module)[*index].isFromStarExport)
            return step && data(module).hasDynamicStarExport();
        const ReexportedFieldData* reexported = m_table.findReexportedField(module, *index);
        if (!reexported)
            return false;
        module = reexported->namedModule();
        name = reexported->importName;
    }
    return false;
}

bool Modules::isReexportedByUnlistedName(ModuleID facade)
{
    if (!m_variablesReexportedByUnlistedNames) {
        m_variablesReexportedByUnlistedNames.emplace();
        for (uint32_t module = 0; module < m_table.size(); ++module) {
            if (!hasInstances(ModuleID(module)) || !data(ModuleID(module)).hasExportTable())
                continue;
            std::span<const ModuleExportData> exports = m_table.exports(ModuleID(module));
            for (unsigned index = 0; index < exports.size(); ++index) {
                const ReexportedFieldData* reexported = exports[index].kind() == ModuleExportKind::Variable && exports[index].isFromStarExport ? m_table.findReexportedField(ModuleID(module), index) : nullptr;
                if (reexported && isHandedOnOverDynamicStar(reexported->namedModule(), reexported->importName))
                    m_variablesReexportedByUnlistedNames->add(exports[index].variable.encoded() + 1ull);
            }
        }
    }
    if (m_variablesReexportedByUnlistedNames->isEmpty()) [[likely]]
        return false;
    return std::ranges::any_of(m_table.syntheticExports(facade), [&](const SyntheticExportData& syntheticExport) {
        return syntheticExport.hasVariable && m_variablesReexportedByUnlistedNames->contains(syntheticExport.variable.encoded() + 1ull);
    });
}

void Modules::releaseExportsOfLoad(ModuleID facade)
{
    LoadedModule* exporting = loadedIfExists(facade);
    if (!exporting)
        return;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* word = static_cast<ModuleNamespace*>(exporting->loadWord.get());
    if (!word)
        word = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(facade, ModulePhase::Evaluation));
    if (word) {
        if (!word->isFilled()) {
            ModuleNamespace::DynamicExports exports;
            createDynamicExports(facade, exports.names, exports.capturedValues, exports.liveSources);
            scope.assertNoExceptionExceptTermination();
            word->replaceDynamicExports(vm, exports);
            word->setFilled();
        }
        word->releaseLoad(vm, createTableOfExports());
    }
    exporting->loadWord.set(*this, nullptr);
    exporting->bindsEarlierCapture = false;
    m_exportsLeftOut.remove(facade);
    discardNamespace(facade);
    exporting->namespaceToRestore.set(*this, nullptr);
    exporting->capturedExports.set(*this, nullptr);
    exporting->deferredExportsSource.set(*this, nullptr);
    exporting->capturedFields.set(*this, nullptr);
    exporting->capturedStars.set(*this, nullptr);
    exporting->sourceOfCapturedFields.set(*this, nullptr);
    exporting->earlyCapturedExports.set(*this, nullptr);
    exporting->earlyDeferredExportsSource.set(*this, nullptr);
    exporting->earlyCapturedFields.set(*this, nullptr);
    exporting->earlySourceOfCapturedFields.set(*this, nullptr);
    unsettleForLinks(facade);
    m_facadesCapturedForMock.clear(facade.index());
    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(facade)) {
        if (syntheticExport.hasVariable)
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, syntheticExport.absentIsUndefined ? jsUndefined() : JSValue());
    }
}

void Modules::releaseCommonJSFile(ModuleID file)
{
    startLoadingModulesIndividually();
    ModuleID facade = moduleWithExportsOf(file);
    auto isOwedNotice = [&](const BitVector& owed) {
        return owed.get(file.index()) || owed.get(facade.index());
    };
    if ((isOwedNotice(m_importersRetainedAboveMock) || isOwedNotice(m_modulesReleasedByLoader)) && runsTestFilesAgain()) [[unlikely]]
        m_requirersThatRunAgain.set(file.index());
    if (facade != file)
        releaseExportsOfLoad(facade);
    discardFailedEvaluation(file);
    Vector<ModuleID, 2> keys { file };
    if (facade != file)
        keys.append(facade);
    for (ModuleID each : keys)
        forgetLoadOfEntry(each);
    m_mockedFilesWithReleasedRequirers.add(file, false);
    forgetRequireEdges(file, false);
    forgetEdgesOfRequirer(file.index());
    m_runsLoadedUnderActualKey.clear(file.index());
    m_filesThatRanBeforeTestFile.clear(file.index());
    ++m_numberOfReleases;
    m_hasReleasedInstances = true;
    didChangeLinkState();
    m_modulesReleasedSincePass.set(file.index());
}

bool Modules::keepCommonJSFileDroppedByLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!data(file).isReloadable() || !m_modulesReleasedByLoader.clear(file.index()))
        return false;
    tellOfReleaseWhereTestFilesRunAgain(file);
    m_importersOfMockThatEnded.clear(file.index());
    m_originalsOfReplacedMocks.set(file.index());
    return true;
}

bool Modules::tryReleaseCommonJSFileDroppedByLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!data(file).isReloadable() || !m_modulesReleasedByLoader.get(file.index()))
        return false;
    tellOfReleaseWhereTestFilesRunAgain(file);
    if (!tryReleaseModule(file))
        return false;
    m_modulesReleasedByLoader.clear(file.index());
    m_importersOfMockThatEnded.clear(file.index());
    return true;
}

bool Modules::requiresOriginalUnderMock(ModuleID file, ModuleID requirer) const
{
    if ((!activeMock(file) && !isMockPending(file)) || !isOfPackage(requirer) || !host().shouldBypassMock || !m_table.hasFilePath(file))
        return false;
    return host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(file), m_table.fullPath(requirer), false);
}

uint32_t Modules::requirerIndex(ModuleID requirer) const
{
    if (m_testFilesOfCommonJSOriginals.isEmpty()) [[likely]]
        return requirer.index();
    ModuleID file = fileOf(requirer);
    if (!m_testFilesOfCommonJSOriginals.contains(file.index()))
        return requirer.index();
    bool isBehindMock = activeMock(file) && !hasIntactOriginal(file) && !isPatchedByMock(file);
    bool isOfOriginal = m_commonJSOriginal == file || isBehindMock || status(file) <= ModuleStatus::Linked;
    return isOfOriginal ? requirerIndexOfOriginal(file) : requirer.index();
}

void Modules::noteRequireEdge(ModuleID target, ModuleID requirer)
{
    ModuleID file = fileOf(target);
    uint32_t index = requirerIndex(requirer);
    bool isPending = isMockPending(file);
    if (const ModuleIndexSet* known = isPending ? nullptr : requirersOfEntry(file); known && known->contains(index))
        return;
    if (requiresOriginalUnderMock(file, requirer))
        return noteRequireOfOriginal(file, requirer);
    if (isPending) {
        m_originalsByRequirer.add(index, ModuleIndexSet()).iterator->value.add(file.index());
        return;
    }
    if (auto originals = m_originalsByRequirer.find(index); originals != m_originalsByRequirer.end())
        originals->value.remove(file.index());
    Requirers& requirers = m_requirersByFile.add(file.index(), Requirers()).iterator->value;
    (activeMock(file) ? requirers.ofMock : requirers.ofLoad).add(index);
    m_filesByRequirer.add(index, ModuleIndexSet()).iterator->value.add(file.index());
}

void Modules::forgetRequireEdge(ModuleID target, ModuleID requirer)
{
    ModuleID file = fileOf(target);
    uint32_t index = requirerIndex(requirer);
    if (auto requirers = m_requirersByFile.find(file.index()); requirers != m_requirersByFile.end()) {
        requirers->value.ofLoad.remove(index);
        requirers->value.ofMock.remove(index);
    }
    if (auto originals = m_originalsByRequirer.find(index); originals != m_originalsByRequirer.end())
        originals->value.remove(file.index());
}

auto Modules::requirersOfEntry(ModuleID file) const -> const ModuleIndexSet*
{
    auto requirers = m_requirersByFile.find(file.index());
    if (requirers == m_requirersByFile.end() || (!activeMock(file) && hasIntactOriginal(file)))
        return nullptr;
    return activeMock(file) ? &requirers->value.ofMock : &requirers->value.ofLoad;
}

void Modules::forgetEdgesOfRequirer(uint32_t requirerIndex)
{
    if (requirerIndex < m_table.size() && m_requirersUnderPendingMocks.clear(requirerIndex) && !m_originalsRestoredAfterPass && m_table.hasFilePath(ModuleID(requirerIndex)) && host().didRunOriginal) [[unlikely]]
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(ModuleID(requirerIndex)), OriginalRun::RunsAmongLoads);
    for (uint32_t file : m_filesByRequirer.take(requirerIndex)) {
        if (auto requirers = m_requirersByFile.find(file); requirers != m_requirersByFile.end()) {
            requirers->value.ofLoad.remove(requirerIndex);
            requirers->value.ofMock.remove(requirerIndex);
        }
    }
}

void Modules::didEvaluateAsOriginal(ModuleID file)
{
    auto required = m_filesByRequirer.find(requirerIndexOfOriginal(file));
    if (required == m_filesByRequirer.end())
        return;
    for (uint32_t each : copyToVector(required->value))
        noteRequireEdge(ModuleID(each), file);
}

void Modules::forgetCommonJSOriginal(ModuleID file)
{
    if (!m_testFilesOfCommonJSOriginals.remove(file.index()))
        return;
    uint32_t index = requirerIndexOfOriginal(file);
    forgetEdgesOfRequirer(index);
    m_originalsByRequirer.remove(index);
}

void Modules::noteDisplacedRunImporters(ModuleID file)
{
    listImportersAndRequirers();
    ModuleID facade = moduleWithExportsOf(file);
    ModuleIndexSet importers;
    for (uint32_t index : m_importersByStatement[facade.index()]) {
        ModuleID importer(index);
        Environment* instance = m_linkedInstances.get(index) ? currentInstance(importer) : nullptr;
        const ModuleLinkData* link = instance ? findModuleLink(importer, facade) : nullptr;
        if (!link || status(importer) != ModuleStatus::Evaluated || m_loadedInPreload.get(index) || isStartedByHost(importer))
            continue;
        JSValue word = link->hasMock ? readField(instance, link->mock) : jsUndefined();
        auto* object = word ? dynamicDowncast<ModuleNamespace>(word) : nullptr;
        if (word.isUndefined() || (object && object->isLoadWord()))
            importers.add(index);
    }
    if (importers.isEmpty())
        return;
    m_displacedRunImporters.set(file.index(), WTF::move(importers));
    if (runsTestFilesAgain())
        tellOfKeptRun(file);
}

bool Modules::isDisplacedRunImporter(ModuleID file, ModuleID importer) const
{
    if (m_displacedRunImporters.isEmpty()) [[likely]]
        return false;
    auto importers = m_displacedRunImporters.find(file.index());
    return importers != m_displacedRunImporters.end() && importers->value.contains(importer.index());
}

bool Modules::keepsDisplacedRun(ModuleID importer) const
{
    return std::ranges::any_of(m_displacedRunImporters.values(), [&](const ModuleIndexSet& importers) {
        return importers.contains(importer.index());
    });
}

bool Modules::reachesDisplacedRun(ModuleID module) const
{
    if (m_displacedRunImporters.isEmpty()) [[likely]]
        return false;
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 16> toVisit { module };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (keepsDisplacedRun(importer))
            return true;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (!request.isHoisted && !request.isRequire && !seen.quickSet(request.target()))
                toVisit.append(request.targetModule());
        }
    }
    return false;
}

void Modules::releaseDisplacedRunImporters(ModuleID file)
{
    for (uint32_t importer : m_displacedRunImporters.take(file.index()))
        releaseModuleWithImporters(ModuleID(importer));
}

bool Modules::hasRequiredOriginal(ModuleID requirer, ModuleID file) const
{
    auto originals = m_originalsByRequirer.find(requirer.index());
    return originals != m_originalsByRequirer.end() && originals->value.contains(file.index());
}

void Modules::noteAnsweredRequire(ModuleID target, ModuleID requirer, JSValue answered)
{
    ModuleID file = fileOf(target);
    if (activeMock(file) && answered != answerOfActiveMock(file)) {
        noteRequireOfOriginal(file, requirer);
        if (m_entryParents.getOptional(target) == requirer)
            forgetEntryParent(target);
    } else
        noteRequireEdge(file, requirer);
}

bool Modules::isStartedByHost(ModuleID module) const
{
    return m_instance.vm->aotState().filesStartedByHost.get(module.index());
}

void Modules::noteRequestOfActiveMock(ModuleID target)
{
    ModuleID file = fileOf(target);
    const LoadedModule* exporting = activeMock(file) ? loadedIfExists(moduleWithExportsOf(file)) : nullptr;
    if (!exporting || !exporting->nextRequestReloadsActiveMock)
        return;
    noteAskUnderMockInPlace(target);
    noteRequireOfMockLaidInPreload(target);
}

void Modules::tellOfKeptRun(ModuleID file)
{
    if (m_table.hasFilePath(file) && !m_instance.vm->aotState().filesReportedAsKept.set(file.index()))
        notifyRejectedChangeListener(m_instance.globalObject, makeString("Kept the run of '"_s, m_table.fullPath(file), '\''));
}

void Modules::noteRequireDuringPatch(ModuleID file)
{
    if (status(file) != ModuleStatus::Evaluated) {
        if (m_filesRequiredBesidePatch.clear(file.index()))
            tellOfKeptRun(file);
        return;
    }
    if (!host().isPatchPending || activeMock(file) || hasIntactOriginal(file) || m_modulesEvaluatedBeforeMock.get(file.index()) || !isPatchPending(file))
        return;
    tellOfKeptRun(file);
    m_filesRequiredBesidePatch.set(file.index());
}

bool Modules::isOnCycleOfLoads(ModuleID file)
{
    listImportersAndRequirers();
    BitVector seen(m_table.size());
    Vector<uint32_t, 16> work { file.index() };
    while (!work.isEmpty()) {
        uint32_t module = work.takeLast();
        auto follow = [&](uint32_t index) {
            if (!seen.quickSet(index))
                work.append(index);
        };
        for (uint32_t index : m_requirers[module])
            follow(index);
        for (uint32_t index : m_importersByStatement[module])
            follow(index);
        if (ModuleID facade = moduleWithExportsOf(ModuleID(module)); facade.index() != module)
            follow(facade.index());
    }
    return seen.quickGet(file.index());
}

void Modules::tellIfMockedFileWasKept(ModuleID target)
{
    ModuleID file = fileOf(target);
    auto released = m_mockedFilesWithReleasedRequirers.find(file);
    if (released == m_mockedFilesWithReleasedRequirers.end() || status(file) != ModuleStatus::Evaluated)
        return;
    bool isKept = released->value && (m_filesThatRanInPreload.get(file.index()) || !data(file).isReloadable());
    if (!isKept && (!host().isPatchPending || !isPatchPending(file)))
        return;
    m_mockedFilesWithReleasedRequirers.remove(released);
    tellThatFileDidNotRunAgain(file);
}

void Modules::noteRefusedOriginal(ModuleID module)
{
    uint32_t moment = m_instance.vm->aotState().momentOfTestFile;
    uint32_t first = m_refusedOriginals.add(module, moment).iterator->value;
    if (first != moment && m_table.hasFilePath(module) && !m_instance.vm->aotState().filesReportedAsKept.set(module.index()))
        notifyRejectedChangeListener(m_instance.globalObject, makeString("Kept the run of '"_s, m_table.fullPath(module), '\''));
}

JSValue Modules::answerOfActiveMock(ModuleID file) const
{
    if (data(file).kind == ModuleKind::CommonJs)
        return commonJSExports(file);
    const LoadedModule* loaded = loadedIfExists(file);
    return loaded ? loaded->requiredValue.get() : JSValue();
}

void Modules::noteRequireOfOriginal(ModuleID target, ModuleID requirer)
{
    ModuleID file = fileOf(target);
    uint32_t index = requirerIndex(requirer);
    if (auto requirers = m_requirersByFile.find(file.index()); requirers != m_requirersByFile.end())
        requirers->value.ofMock.remove(index);
    m_originalsByRequirer.add(index, ModuleIndexSet()).iterator->value.add(file.index());
}

void Modules::didDeleteEntry(ModuleID module)
{
    if (m_mocks && status(fileOf(module)) == ModuleStatus::Evaluated) {
        for (size_t index : m_mocks->modules) {
            ModuleID mocked(index);
            if (fileOf(mocked) != fileOf(module) && status(mocked) <= ModuleStatus::Linked && importsStatically(mocked, module))
                tellThatFileDidNotRunAgain(fileOf(module));
        }
    }
    forgetRequireEdges(module, !!activeMock(fileOf(module)));
    m_entryParents.remove(fileOf(module));
    m_modulesWithKnownParent.clear(fileOf(module).index());
}

void Modules::forgetRequireEdges(ModuleID target, bool areToMock)
{
    if (auto requirers = m_requirersByFile.find(fileOf(target).index()); requirers != m_requirersByFile.end())
        (areToMock ? requirers->value.ofMock : requirers->value.ofLoad).clear();
}

void Modules::tellIfReexportedFileWasKept(ModuleID module)
{
    if (ModuleID file = fileOf(module); data(file).isReloadable() && status(file) == ModuleStatus::Evaluated && isReexportedByUnlistedName(moduleWithExportsOf(file))) [[unlikely]]
        tellThatFileDidNotRunAgain(file);
}

bool Modules::tryReleaseModule(ModuleID module)
{
    if (!mayReleaseModule(module)) {
        tellIfReexportedFileWasKept(module);
        return false;
    }
    if (data(fileOf(module)).isReloadable())
        releaseCommonJSFile(fileOf(module));
    else
        releaseLoadedModule(module);
    if (host().deferPatchToNextLoad && !isReloadedByPendingImport(module))
        host().deferPatchToNextLoad(m_instance.globalObject, module, true);
    return true;
}

void Modules::oweToEvaluatedImporters(ModuleID module)
{
    if (!hasEvaluatedImporterWithoutInstances(module)) [[likely]]
        return;
    m_modulesOwedToImportersThatRan.set(module.index());
    m_modulesWithNothingOwedBelow.clearAll();
    m_mockedModulesPassedOverByWalks.clear();
    unsettleForLinks(module);
}

void Modules::dropPatch(ModuleID module)
{
    if (!isPatchedByMock(module))
        return;
    forgetPatch(module);
    ensureLoaded(module).isReleasedPatch = false;
}

bool Modules::takeReleaseUnderPendingPatch(ModuleID module)
{
    bool isOwed = m_modulesReloadedUnderPatch.remove(module.index());
    isOwed |= m_modulesReloadedUnderPatch.remove(fileOf(module).index());
    if (isOwed)
        m_loadsInFlight.removeSettledIf([&](const LoadInFlight& load) { return m_rootAwaitingLoader != load.root; });
    return isOwed;
}

bool Modules::isReloadedByPendingImport(ModuleID module)
{
    return m_importsBeforeFirstStep.contains(module) && m_modulesOwedToImportersThatRan.isEmpty() && !hasEvaluatedImporterWithoutInstances(module);
}

bool Modules::isImportedByOwnFactory(ModuleID module) const
{
    return std::ranges::any_of(m_importsInFlight.values(), [&](const ImportInFlight& import) { return import.module == module && import.isFromOwnFactory; });
}

bool Modules::mayReleasePatchedModule(ModuleID module)
{
    if (!isPatchedByMock(module) || !currentInstance(module) || isEvicted(module) || activeMock(module) || hasIntactOriginal(module) || isMockPending(module))
        return false;
    return status(module) == ModuleStatus::Evaluated && ensureLoaded(module).wasEvaluatedWhenPatched;
}

bool Modules::tryReleasePatchedModule(ModuleID module)
{
    if (!mayReleasePatchedModule(module) || !host().deferPatchToNextLoad)
        return false;
    forgetPatch(module);
    releaseLoadedModule(module);
    if (!host().deferPatchToNextLoad(m_instance.globalObject, module, false))
        tellThatFileDidNotRunAgain(module);
    return true;
}

void Modules::releasePatchedFileAtMockEnd(ModuleID file)
{
    ModuleID facade = moduleWithExportsOf(file);
    if (data(file).kind != ModuleKind::CommonJs || !data(file).isReloadable() || status(file) != ModuleStatus::Evaluated || !isPatchedByMock(file) || isEvicted(file) || activeMock(file) || hasIntactOriginal(file) || m_filesThatRanInPreload.get(file.index()) || m_loadedInPreload.get(file.index()) || m_loadedInPreload.get(facade.index()))
        return;
    m_modulesReloadedUnderPatch.add(file.index());
}

void Modules::releasePatchedModuleOfCycle(ModuleID module, bool isFactoryPending)
{
    if (std::exchange(m_patchedModuleOfCycle, std::nullopt) != module)
        return;
    if (isFactoryPending && host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::RunsAmongLoads);
    if (!tryReleaseModule(module) && !tryReleasePatchedModule(module))
        tellThatFileDidNotRunAgain(module);
}

void releasePatchedModuleOfCycle(JSGlobalObject* realm, ModuleID module, bool isFactoryPending)
{
    if (Modules* modules = modulesOf(realm))
        modules->releasePatchedModuleOfCycle(module, isFactoryPending);
}

bool Modules::isPatchedInPreload(ModuleID module)
{
    return m_loadedInPreload.get(module.index()) && mayReleasePatchedModule(module);
}

void Modules::forgetPatch(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    loaded.isPatchedByMock = false;
    loaded.isReleasedPatch = true;
    loaded.exportsOfMock.set(*this, nullptr);
    loaded.valueBeforeMock.set(*this, JSValue());
}

void Modules::restoreReleasedPatch(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !std::exchange(loaded->isReleasedPatch, false)) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    restorePatchedExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, restoreCapturedExports(moduleWithExportsOf(module)));
}

void Modules::releaseLoadedModule(ModuleID module)
{
    startLoadingModulesIndividually();
    m_loadRoots.clear(module.index());
    for (const ModuleRequestData& request : m_table.imports(module)) {
        ModuleID target = request.targetModule();
        if (!data(target).isOfLibrary() || !wasImporterLoaded(module))
            continue;
        uint32_t& last = m_lastLinksOfReleasedImporters.add(target, 0).iterator->value;
        last = std::max(last, momentLinked(module));
    }
    if (m_loadedByPreloadHooks.clear(module.index())) [[unlikely]]
        m_releasedLoadsOfPreloadHooks.set(module.index());
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.instanceBesideMock.get())
        m_modulesThatRanBesideMockAlone.set(module.index());
    if (!loaded.preloadMock) {
        if (m_loadedInPreload.get(module.index()) && status(module) != ModuleStatus::Failed && !m_modulesWithPreloadInstance.set(module.index()))
            loaded.preloadInstance.set(*this, currentInstance(module));
        m_loadedInPreload.clear(module.index());
    }
    loaded.requiredValue.set(*this, JSValue());
    loaded.moduleObjectFromRequire.set(*this, nullptr);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    loaded.isKeptByRequire = false;
    loaded.cachedLoadError.set(*this, JSValue());
    m_originalsEvaluatedUnderMock.clear(module.index());
    if (!currentInstance(module))
        forgetLoad(module);
    makeESModuleReadyToRunAgain(module);
    if (storedStatus(module) == StoredModuleStatus::Linked)
        setStoredStatus(module, StoredModuleStatus::Unlinked);
    m_modulesReleasedSincePass.set(module.index());
    if (m_unseenMocks.clear(module.index())) [[unlikely]] {
        m_modulesWithPendingMock.set(module.index());
        m_modulesWithDeferredMock.set(module.index());
        willMock(module);
        noteCallOfMock(module, false);
    }
}

bool Modules::isReachedByBlockedLoad(ModuleID module, ModuleID mocked)
{
    auto isReachedFrom = [&](ModuleID root) {
        BitVector closure = staticImportClosure(root, MockTraversal::Stop);
        if (!closure.get(module.index()))
            return false;
        for (size_t index : closure) {
            for (const ModuleRequestData& request : m_table.imports(ModuleID(index))) {
                if (ModuleID file = fileOf(request.targetModule()); file != fileOf(mocked) && isMockPending(file))
                    return true;
            }
        }
        return false;
    };
    for (LoadInFlight& load : m_loadsInFlight) {
        if (!load.isLoaded && isReachedFrom(load.root))
            return true;
    }
    return std::ranges::any_of(m_factoryBlockedImportRoots, isReachedFrom);
}

Vector<String> Modules::takeModulesReleasedSincePass()
{
    Vector<String> paths;
    for (size_t index : std::exchange(m_modulesReleasedSincePass, { })) {
        ModuleID module(index);
        if (!m_linkedInstances.get(index) && !loader()->registryEntry(moduleKey(module)))
            paths.append(m_table.fullPath(module));
    }
    return paths;
}

void Modules::forgetLoad(ModuleID module)
{
    if (!m_momentsOfLoads.isEmpty())
        didGiveUpMomentLinked(std::exchange(m_momentsOfLoads[module.index()], 0));
    if (auto anew = m_momentsLoadedAnew.find(module); anew != m_momentsLoadedAnew.end()) {
        didGiveUpMomentLinked(anew->value.moment);
        m_momentsLoadedAnew.remove(anew);
    }
    m_earlierMomentsLoadedAnew.remove(module);
    auto isOfImporter = [&](uint64_t pair) {
        return static_cast<uint32_t>(pair) == module.index();
    };
    m_importersLinkedToMock.removeIf([&](auto& entry) { return isOfImporter(entry.key); });
    m_importersLinkedToOriginal.removeIf([&](auto& entry) { return isOfImporter(entry.key); });
    m_importersAskedAboutOriginal.removeIf([&](auto& entry) { return isOfImporter(entry.key); });
    for (auto& importers : m_displacedRunImporters.values())
        importers.remove(module.index());
    forgetEntryParent(module);
    m_modulesReleasedByLoader.clear(module.index());
    m_modulesWithoutTestFile.clear(module.index());
    m_importersOfMockThatEnded.clear(module.index());
    m_modulesLoadedAnewWhoseNamesAreOwed.clear(module.index());
    m_modulesOwedToImportersThatRan.clear(module.index());
    m_importersRetainedAboveMock.clear(module.index());
    m_originalsKeptUnderMock.clear(fileOf(module).index());
    m_modulesLoadedBesideGroups.clear(module.index());
    m_modulesWithNothingOwedBelow.clearAll();
    m_modulesWithNoNamesOwedBelow.clearAll();
    followImportsAnewAtNextQuestion();
    ++m_linkEpoch;
}

auto Modules::exchangeCurrentInstance(ModuleID module, const CurrentInstance& current) -> CurrentInstance
{
    SlotData guard = *m_table.instanceGuard(module);
    CurrentInstance previous { lastInstance(module), m_linkedInstances.get(module.index()), m_linkMoments.isEmpty() ? 0 : m_linkMoments[module.index()], ProgramStatics::readVariableWithoutAllocating(m_instance, guard) };
    if (current.instance)
        ProgramStatics::writeVariable(m_instance, *m_table.instanceVariable(module), current.instance);
    ProgramStatics::writeVariable(m_instance, guard, current.instance ? current.guard : jsUndefined());
    m_linkedInstances.set(module.index(), current.isLinked);
    didChangeLinkState();
    if (!m_linkMoments.isEmpty())
        m_linkMoments[module.index()] = current.linkMoment;
    return previous;
}

Environment* Modules::originalBesideMock(ModuleID module) const
{
    if (!hasInstances(module))
        return nullptr;
    if (m_originalsEvaluatedUnderMock.get(module.index()))
        return m_linkedInstances.get(module.index()) ? currentInstance(module) : nullptr;
    const LoadedModule* loaded = hasRunBesideMockAlone(module) ? loadedIfExists(module) : nullptr;
    JSValue instance = loaded ? loaded->instanceBesideMock.get() : JSValue();
    return instance ? uncheckedDowncast<Environment>(instance.asCell()) : nullptr;
}

void Modules::forgetUnmockedOriginal(ModuleID module)
{
    m_modulesWithPrivateCopy.clearAll();
    if (m_originalsEvaluatedUnderMock.clear(module.index()))
        releaseInstance(module);
    LoadedModule& loaded = ensureLoaded(module);
    loaded.instanceBesideMock.set(*this, JSValue());
    loaded.errorOfModuleThatRanBesideMock.set(*this, JSValue());
    m_modulesThatRanBesideMockAlone.clear(module.index());
}

bool Modules::tryDisplaceFailedOriginal(ModuleID module)
{
    Environment* instance = errorOfModuleThatRanBesideMock(module) && m_originalsEvaluatedUnderMock.get(module.index()) ? originalBesideMock(module) : nullptr;
    if (!instance)
        return false;
    ensureLoaded(module).instanceBesideMock.set(*this, instance);
    releaseInstance(module);
    m_originalsEvaluatedUnderMock.clear(module.index());
    m_modulesThatRanBesideMockAlone.set(module.index());
    setStatus(module, ModuleStatus::Unlinked);
    return true;
}

void Modules::restoreInstance(ModuleID module, Environment* instance)
{
    releaseInstance(module);
    ProgramStatics::writeVariable(m_instance, *m_table.instanceVariable(module), instance);
    ProgramStatics::writeVariable(m_instance, *m_table.instanceGuard(module), jsBoolean(true));
    m_linkedInstances.set(module.index());
    didChangeLinkState();
    const ModuleInstanceData& record = m_table.instanceData(module);
    for (ModulePhase phase : { ModulePhase::Evaluation, ModulePhase::Defer }) {
        JSValue created = readField(instance, phase == ModulePhase::Defer ? record.deferredNamespaceField : record.namespaceField);
        if (!created)
            continue;
        uncheckedDowncast<ModuleNamespace>(created.asCell())->setKeepsToModule(false);
        setNamespace(module, phase, asObject(created));
    }
}

void Modules::restoreUnmockedInstance(ModuleID module)
{
    LoadedModule* loaded = hasInstances(module) ? loadedIfExists(module) : nullptr;
    JSValue instance = loaded ? loaded->instanceBesideMock.get() : JSValue();
    if (!instance)
        return;
    loaded->instanceBesideMock.set(*this, JSValue());
    restoreInstance(module, uncheckedDowncast<Environment>(instance.asCell()));
}

void Modules::restoreOriginalLeftByEarlierMock(ModuleID module)
{
    LoadedModule* loaded = hasInstances(module) && isOfPackage(module) ? loadedIfExists(module) : nullptr;
    if (!loaded || !loaded->instanceBesideMock || m_failedInstancesBesideMocks.get(module.index()) || status(module) > ModuleStatus::Linked || isMocked(module) || activeMock(module) || isMockPending(module) || hasIntactOriginal(module))
        return;
    restoreUnmockedInstance(module);
    m_modulesThatRanBesideMockAlone.clear(module.index());
    startLoadingModulesIndividually();
    setStatus(module, ModuleStatus::Evaluated);
    m_modulesLoadedBesideGroups.clear(module.index());
    followImportsAnewAtNextQuestion();
    ++m_linkEpoch;
}

void Modules::displaceUnmockedInstance(ModuleID module)
{
    if (m_originalsEvaluatedUnderMock.get(module.index()) && !m_originalsRestoredAtUnmock.get(module.index()) && !hasRunBesideMockAlone(module) && !activeMock(module) && tryLeaveUnderActualKey(module)) {
        m_originalsEvaluatedUnderMock.clear(module.index());
        return;
    }
    if (m_originalsRestoredAtUnmock.get(module.index()) && isOfPackage(module) && hasInstances(module) && !hasRunBesideMockAlone(module) && !activeMock(module))
        m_originalsEvaluatedUnderMock.clear(module.index());
    Environment* instance = hasRunBesideMockAlone(module) && hasInstances(module) && !activeMock(module) ? currentInstance(module) : nullptr;
    if (!instance || status(module) != ModuleStatus::Evaluated)
        return;
    ensureLoaded(module).instanceBesideMock.set(*this, instance);
    releaseInstance(module);
    m_originalsEvaluatedUnderMock.clear(module.index());
    setStatus(module, ModuleStatus::Unlinked);
}

bool Modules::tryLeaveUnderActualKey(ModuleID module)
{
    Environment* instance = hasInstances(module) ? currentInstance(module) : nullptr;
    if (!instance || status(module) != ModuleStatus::Evaluated || m_loadedInPreload.get(module.index()) || !tryReleaseModule(module))
        return false;
    ensureLoaded(module).instanceBesideMock.set(*this, instance);
    m_modulesThatRanBesideMockAlone.set(module.index());
    return true;
}

void Modules::clearUnmockedInstance(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->instanceBesideMock.get() && !currentInstance(module)) {
        loaded->instanceBesideMock.set(*this, JSValue());
        m_modulesThatRanBesideMockAlone.clear(module.index());
    }
}

bool Modules::tryEvictRequirerOfOriginal(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!hasInstances(file) || !activeMock(file) || !m_requirersOfOwnOriginals.clear(file.index()))
        return false;
    releaseImportersOf(file, ReleasedImporters::All);
    forgetUnmockedOriginal(file);
    didRemoveActiveMockEntry(file);
    return true;
}

bool Modules::tryEvictUnderActualKey(ModuleID module)
{
    m_originalsRequiredByFactories.clear(module.index());
    if (!m_displacedRunImporters.isEmpty()) [[unlikely]]
        releaseDisplacedRunImporters(fileOf(module));
    LoadedModule* loaded = hasInstances(module) ? loadedIfExists(module) : nullptr;
    JSValue earlier = loaded ? loaded->instanceBesideMock.get() : JSValue();
    if (!earlier || earlier == JSValue(currentInstance(module)))
        return false;
    loaded->instanceBesideMock.set(*this, JSValue());
    setIsUnderActualKey(module, false);
    releaseImportersOfInstance(module, earlier);
    if (m_requirersOfOwnOriginals.clear(module.index()) && hasIntactOriginal(module) && currentInstance(module) && status(module) == ModuleStatus::Evaluated) {
        m_evictedOriginals.set(module.index());
        releaseImportersOfInstance(module, currentInstance(module));
    }
    return true;
}

void Modules::releaseImportersOfInstance(ModuleID module, JSValue original)
{
    listImportersAndRequirers();
    for (uint32_t index : m_importersByStatement[module.index()]) {
        ModuleID importer(index);
        Environment* instance = m_linkedInstances.get(index) && !m_loadedInPreload.get(index) ? currentInstance(importer) : nullptr;
        const ModuleLinkData* link = instance ? findModuleLink(importer, module) : nullptr;
        if (link && link->hasLink && JSValue(readLink(instance, link->link)) == original)
            releaseModuleWithImporters(importer);
    }
}

void Modules::restorePreloadInstances()
{
    if (m_modulesWithPreloadInstance.isEmpty()) [[likely]]
        return;
    for (size_t index : std::exchange(m_modulesWithPreloadInstance, { })) {
        ModuleID module(index);
        if (currentInstance(module) && status(module) > ModuleStatus::Linked) {
            if (!m_restoredModules.get(index))
                releaseImportersOf(module, ReleasedImporters::All);
            if (!tryReleaseModule(module)) {
                m_modulesWithPreloadInstance.set(index);
                continue;
            }
        }
        LoadedModule& loaded = ensureLoaded(module);
        auto* instance = uncheckedDowncast<Environment>(loaded.preloadInstance.get().asCell());
        loaded.preloadInstance.set(*this, JSValue());
        restoreInstance(module, instance);
        setStoredStatus(module, StoredModuleStatus::Evaluated);
        m_loadedInPreload.set(index);
        m_modulesLoadedBesideGroups.set(index);
        if (!m_momentsOfLoads.isEmpty())
            m_momentsOfLoads[index] = 1;
        followImportsAnewAtNextQuestion();
    }
}

bool Modules::keepsOriginalNames(const ModuleNamespace& word) const
{
    ModuleID file = m_table.moduleBehindFacade(word.module()).value_or(word.module());
    const LoadedModule* held = loadedIfExists(file);
    return held && held->mockWord.get() == &word ? isMockedByFactoryThatTakesOriginal(file) : word.keepsOriginalNames();
}

void Modules::registerEvaluatedModule(ModuleID module)
{
    if (!currentInstance(module) || status(module) != ModuleStatus::Evaluated || isMocked(module) || activeMock(module) || loader()->registryEntry(moduleKey(module)))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = registerWithLoader(module, RecordHolder::Loader);
    RETURN_IF_EXCEPTION(scope, void());
    if (ModuleRegistryEntry* entry = loader()->registryEntry(record->moduleKey()); entry && entry->record() == record)
        entry->markLoaded();
}

void Modules::reportImportersWithoutInstances(ModuleID module)
{
    if (!hasEvaluatedImporterWithoutInstances(module)) [[likely]]
        return;
    for (uint32_t index : m_importersByStatement[module.index()]) {
        ModuleID importer(index);
        if (!hasInstances(importer) && status(importer) == ModuleStatus::Evaluated && !m_instance.vm->aotState().filesStartedByHost.get(index))
            tellThatFileDidNotRunAgain(importer);
    }
}

bool Modules::isEvaluatedUnderPendingPatch(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!host().isPatchPending || status(file) != ModuleStatus::Evaluated || !m_table.hasFilePath(file)) [[likely]]
        return false;
    if (!isPatchPending(file)) [[likely]]
        return false;
    if (activeMock(file) || hasIntactOriginal(file) || isPatchedByMock(file) || m_originalsEvaluatedUnderMock.get(file.index()) || hasRunBesideMockAlone(file))
        return false;
    return data(file).kind == ModuleKind::CommonJs ? wasLoadedByImport(file) : !!currentInstance(file);
}

void Modules::tellIfFactoryIsRunning(ModuleID requested, LoadKind kind)
{
    if (!host().didRunOriginal || isMocked(requested) || !data(requested).isESModuleOfProgram() || status(requested) > ModuleStatus::Linked || (kind == LoadKind::Import) == !!m_loadCallingFactory)
        return;
    BitVector reached = modulesInRequireGraph(requested);
    if (m_loadCallingFactory)
        reached.filter(modulesInLoadGraph(*m_loadCallingFactory));
    for (size_t index : reached) {
        ModuleID importer(index);
        if (isMocked(importer))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID mocked = moduleLoadedBy(request);
            if (!m_modulesWithRunningFactory.get(mocked.index()) || !m_table.hasFilePath(mocked))
                continue;
            if (kind == LoadKind::Import)
                m_mocksImportedByFactories.set(mocked.index());
            else
                host().didRunOriginal(m_instance.globalObject, m_table.fullPath(mocked), OriginalRun::RunsAmongLoads);
        }
    }
}

bool Modules::isRequireUnderPendingMock(ModuleID required, ModuleID requirer)
{
    ModuleID file = fileOf(required);
    if (data(fileOf(requirer)).kind != ModuleKind::CommonJs || !data(file).isESModuleOfProgram() || !m_table.hasFilePath(file))
        return false;
    return isMockPending(file) || ((m_activeMockCount || isPatchedByMock(file)) && host().hasPendingPatch && host().hasPendingPatch(m_instance.globalObject, file));
}

void Modules::tellIfPatchIsPending(ModuleID required)
{
    ModuleID file = fileOf(required);
    if (host().didRunOriginal && host().hasPendingPatch && data(file).isESModuleOfProgram() && m_table.hasFilePath(file) && (activeMock(file) || isMocked(file) || isPatchedByMock(file)) && host().hasPendingPatch(m_instance.globalObject, file))
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(file), OriginalRun::RunsAmongLoads);
}

bool Modules::requiresPatch(ModuleID module, JSObject*& exports) const
{
    ModuleID file = fileOf(module);
    const LoadedModule* loaded = loadedIfExists(module);
    if (!host().requiresPatch || (loaded && (loaded->requiredValue || loaded->wasRequiredByHost)) || !data(file).isESModuleOfProgram() || data(file).isFacade() || !m_table.hasFilePath(file))
        return false;
    return host().requiresPatch(m_instance.globalObject, scopedLambda<String()>([&] {
        return m_table.fullPath(file);
    }), exports);
}

void Modules::reportKeptImporters(ModuleID module)
{
    if (!currentInstance(module))
        return;
    listImportersAndRequirers();
    for (uint32_t index : m_importersByStatement[module.index()]) {
        ModuleID importer(index);
        Environment* instance = currentInstance(importer);
        const ModuleLinkData* link = instance && m_linkedInstances.get(index) ? findModuleLink(importer, module) : nullptr;
        if (link && link->hasLink && !mockWord(*link, instance) && readLink(instance, link->link) == currentInstance(module))
            tellThatFileDidNotRunAgain(importer);
    }
}

void reportKeptImporters(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->reportKeptImporters(module);
}

void setModuleWithSettledFactory(JSGlobalObject* realm, std::optional<ModuleID> module)
{
    if (Modules* modules = modulesOf(realm))
        modules->setModuleWithSettledFactory(module);
}

void registerEvaluatedModule(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->registerEvaluatedModule(module);
}

bool isActiveMockRegistered(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isActiveMockRegistered(module);
}

bool isMockLoadedInPreload(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isMockLoadedInPreload(module);
}

bool wasLoadedInPreload(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLoadedInPreload(module);
}

bool isReleased(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasInstances(module) && !modules->currentInstance(module);
}

bool isReloadableImport(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isReloadable(module) && modules->wasLoadedByImport(module);
}

void tellIfMockedBelowImport(JSGlobalObject* realm, ModuleID file, std::optional<ModuleID> caller)
{
    if (Modules* modules = modulesOf(realm))
        modules->tellIfMockedBelowImport(file, caller);
}

bool hasInstances(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasInstances(module);
}

void releaseCommonJSFile(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->data(file).isReloadable())
        modules->releaseModuleWithImporters(file);
}

void releaseModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->data(module).isReloadable() && !modules->hasEvaluatedImporterWithoutInstances(module) && !modules->tryLeaveUnderActualKey(module))
        modules->tryReleaseModule(module);
}

bool keepCommonJSFileDroppedByLoader(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->keepCommonJSFileDroppedByLoader(module);
}

bool tryReleaseCommonJSFileDroppedByLoader(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->tryReleaseCommonJSFileDroppedByLoader(module);
}

bool tryEvictUnderActualKey(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->tryEvictUnderActualKey(module);
}

void setReplacedTestFile(JSGlobalObject* realm, std::optional<ModuleID> testFile)
{
    if (Modules* modules = modulesOf(realm))
        modules->setReplacedTestFile(testFile);
}

bool releaseModuleAndImporters(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> asking)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->hasInstances(module) && !modules->mayReleaseModule(module)) {
        modules->tellIfReexportedFileWasKept(module);
        modules->releaseImportersOf(module, Modules::ReleasedImporters::WithInstances);
        modules->forgetEntryParent(module);
        modules->didEvictDataFile(module);
        return false;
    }
    if (!modules || (!modules->mayReleaseModule(module) && !modules->isPatchedInPreload(module)) || (modules->isGraphRunning() && modules->hasUnevaluatedLinkedImporter(module, asking)))
        return false;
    if (modules->hasEvaluatedImporterWithoutInstances(module) || modules->isAwaitedByDetachedLoad(module))
        return false;
    if (modules->wasOriginalEvaluatedUnderMock(module))
        return false;
    return modules->releaseModuleWithImporters(module);
}

void releaseEndedMockImporters(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->releaseEndedMockImporters();
}

void restoreOriginalLeftByEarlierMock(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->restoreOriginalLeftByEarlierMock(module);
}

void releaseImporters(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (modules && (modules->currentInstance(module) || (modules->data(module).isReloadable() && modules->status(module) == ModuleStatus::Evaluated)))
        modules->releaseImportersOf(module, Modules::ReleasedImporters::All);
}

void releaseMockImporters(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->releaseImportersOfEvictedMock(module);
}

void releaseEarlierImporters(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleData& data = modules->data(module);
    bool hasLoad = data.isOfLibrary() ? modules->table().anyModuleHasInstances() : !modules->hasInstances(module);
    if (!modules->currentInstance(module) && !hasLoad)
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modules->detachImportersLinkedBeforeReset(module);
    RETURN_IF_EXCEPTION(scope, void());
    modules->releaseImportersOf(module, Modules::ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule);
}

void restorePreloadInstances(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->restorePreloadInstances();
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceNamespace, EncodedJSValue, (Instance* instance, JSCell* moduleInstance, uint32_t module, uint32_t phase))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceNamespace);
    Modules::ModulePhase requested = static_cast<ImportPhase>(phase) == ImportPhase::Defer ? Modules::ModulePhase::Defer : Modules::ModulePhase::Evaluation;
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).instanceNamespace(ModuleID(module), uncheckedDowncast<Environment>(moduleInstance), requested)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceImportMeta, EncodedJSValue, (Instance* instance, JSCell* moduleInstance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceImportMeta);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).instanceImportMeta(ModuleID(module), uncheckedDowncast<Environment>(moduleInstance))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkedMock, EncodedJSValue, (Instance* instance, EncodedJSValue word))
{
    auto* object = dynamicDowncast<ModuleNamespace>(JSValue::decode(word));
    JSObject* mock = object ? object->linkedExports() : nullptr;
    if (object && object->isUninitializedMock()) [[unlikely]]
        mock = modulesOf(*instance).uninitializedMock();
    return JSValue::encode(mock ? JSValue(mock) : jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasNameInMock, size_t, (Instance* instance, EncodedJSValue encodedWord, uint32_t key, uint32_t outsideCycles))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasNameInMock);
    auto* word = uncheckedDowncast<ModuleNamespace>(JSValue::decode(encodedWord));
    const Identifier& name = identifierAt(instance, key);
    OPERATION_RETURN(scope, outsideCycles ? modulesOf(*instance).hasNameInMockOutsideCycles(word, name) : modulesOf(*instance).hasNameInMock(word, name));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTKeepsOriginalNames, size_t, (Instance* instance, EncodedJSValue word))
{
    auto* object = dynamicDowncast<ModuleNamespace>(JSValue::decode(word));
    return object && object->activeMock() && modulesOf(*instance).keepsOriginalNames(*object);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
