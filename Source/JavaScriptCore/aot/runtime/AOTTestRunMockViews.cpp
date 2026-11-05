/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSValue Modules::handedOnInPlaceOfNamespace(const ModuleExportData& moduleExport) const
{
    if (m_namespacesReplacedByMock.isEmpty()) [[likely]]
        return { };
    LoadedModule* handedOn = loadedIfExists(moduleExport.targetModule());
    return handedOn ? handedOn->handedOnInPlaceOfNamespace.get() : JSValue();
}

bool Modules::tryReplaceNamespaceHandedOn(ModuleID handedOn, JSValue value)
{
    if (!m_moduleWithMockBeingLaid)
        return false;
    LoadedModule& mocked = ensureLoaded(*m_moduleWithMockBeingLaid);
    LoadedModule& target = ensureLoaded(handedOn);
    if (!mocked.namespacesReplacedByMock.contains(handedOn))
        mocked.namespacesReplacedByMock.append(handedOn);
    // Another mock, over another module that hands the object on, may have given a value. The latest answers, and each goes with its own mock.
    target.mocksThatReplaceNamespace.removeFirst(*m_moduleWithMockBeingLaid);
    target.mocksThatReplaceNamespace.append(*m_moduleWithMockBeingLaid);
    if (!target.handedOnByMocks)
        target.handedOnByMocks.set(*this, createTableOfExports());
    storeValue(m_instance.globalObject, target.handedOnByMocks.get(), m_moduleWithMockBeingLaid->index(), value);
    target.handedOnInPlaceOfNamespace.set(*this, value);
    m_namespacesReplacedByMock.set(handedOn.index());
    didReplaceNamespaceHandedOn(handedOn);
    return true;
}

// The caches hold the object as a constant, under the Structure of the object of each module that hands it on.
void Modules::didReplaceNamespaceHandedOn(ModuleID handedOn)
{
    for (unsigned index = 0; index < m_table.size(); ++index) {
        JSObject* made = namespaceIfCreated(ModuleID(index), ModulePhase::Evaluation);
        // A mock that left the module alone has set its object aside, and whoever held it reads on.
        LoadedModule* loaded = loadedIfExists(ModuleID(index));
        JSObject* setAside = loaded ? loaded->namespaceBeforeMockInPlace.get() : nullptr;
        if (!made && !setAside)
            continue;
        bool handsItOn = std::ranges::any_of(m_table.exports(ModuleID(index)), [&](const ModuleExportData& entry) {
            return entry.kind() == ModuleExportKind::Namespace && entry.targetModule() == handedOn;
        });
        if (!handsItOn)
            continue;
        for (JSObject* object : { made, setAside }) {
            if (auto* filed = dynamicDowncast<ModuleNamespace>(object))
                filed->leaveStructureThatCachesKnow(*m_instance.vm);
        }
    }
}

// The mock over the module ends. Where another mock has given a value for the same object, that one answers again.
void Modules::giveBackNamespacesHandedOn(LoadedModule& mocked)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    for (ModuleID handedOn : std::exchange(mocked.namespacesReplacedByMock, { })) {
        LoadedModule& target = ensureLoaded(handedOn);
        target.mocksThatReplaceNamespace.removeFirstMatching([&](ModuleID each) {
            return loadedIfExists(each) == &mocked;
        });
        bool isLeft = !target.mocksThatReplaceNamespace.isEmpty();
        target.handedOnInPlaceOfNamespace.set(*this, isLeft ? storedValue(realm, target.handedOnByMocks, Identifier::from(vm, target.mocksThatReplaceNamespace.last().index())) : JSValue());
        if (!isLeft)
            target.handedOnByMocks.set(*this, nullptr);
        m_namespacesReplacedByMock.set(handedOn.index(), isLeft);
        didReplaceNamespaceHandedOn(handedOn);
    }
}

auto Modules::momentOfNow() -> MomentMockWasLaid
{
    VMState& state = m_instance.vm->aotState();
    // Also in a hook that a preload has registered, which runs for every test file.
    bool isInPreload = host().isInPreload ? host().isInPreload(m_instance.globalObject).value_or(false) : !m_hasBegunTestFile;
    return { ++state.momentOfLoads, state.momentOfTestFile, isInPreload };
}

void Modules::keepEndedMock(ModuleID fileOrModule, JSObject* mock, MomentMockWasLaid laid, uint32_t momentEnded)
{
    // No moment of a load is noted outside a test run.
    if (m_momentsOfLoads.isEmpty())
        return;
    VMState& state = m_instance.vm->aotState();
    if (!momentEnded)
        momentEnded = ++state.momentOfLoads;
    Vector<EndedMock, 2>& ended = m_endedMocks.add(fileOrModule, Vector<EndedMock, 2>()).iterator->value;
    // One moment, one mock: it stood again, and has ended again.
    if (!ended.isEmpty() && ended.last().momentLaid == laid.moment) {
        ended.last().momentEnded = momentEnded;
        ended.last().momentOfTestFile = state.momentOfTestFile;
        return;
    }
    LoadedModule& loaded = ensureLoaded(fileOrModule);
    if (!loaded.endedMocks)
        loaded.endedMocks.set(*this, createTableOfExports());
    storeValue(m_instance.globalObject, loaded.endedMocks.get(), static_cast<unsigned>(ended.size()), mock);
    ended.append({ laid.moment, momentEnded, state.momentOfTestFile, laid.isInPreload });
}

uint32_t Modules::momentLinked(ModuleID importer) const
{
    if (m_momentsOfLoads.isEmpty())
        return 0;
    uint32_t moment = m_momentsOfLoads[importer.index()];
    if (auto anew = m_momentsLoadedAnew.find(importer); anew != m_momentsLoadedAnew.end())
        moment = std::max(moment, anew->value.moment);
    return moment;
}

JSObject* Modules::endedMockSeenAt(ModuleID fileOrModule, uint32_t moment) const
{
    auto found = m_endedMocks.find(fileOrModule);
    if (!moment || found == m_endedMocks.end())
        return nullptr;
    uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
    const Vector<EndedMock, 2>& ended = found->value;
    for (unsigned index = 0; index < ended.size(); ++index) {
        if (moment < ended[index].momentLaid || moment >= ended[index].momentEnded || (ended[index].momentOfTestFile != momentOfTestFile && !ended[index].wasLaidInPreload))
            continue;
        JSValue mock = storedValue(m_instance.globalObject, loadedIfExists(fileOrModule)->endedMocks.get(), Identifier::from(*m_instance.vm, index));
        return mock ? asObject(mock) : nullptr;
    }
    return nullptr;
}

// As collectDynamicExports() goes through the stars, and past whatever mock stands in place of the module.
void Modules::appendNamesBehindStarsOf(ModuleID module, WTF::Vector<Identifier>& names)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 8> pending { module };
    while (!pending.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(pending.takeLast())) {
            ModuleID source = moduleLoadedBy(request);
            if (!request.isDynamicStarExport || seen.quickSet(source.index()))
                continue;
            const ModuleData& sourceData = data(source);
            if (sourceData.capturesExports()) {
                forEachCapturedExport(source, [&](const Identifier& name, JSValue) {
                    names.append(name);
                });
                RETURN_IF_EXCEPTION(scope, void());
            } else if (sourceData.isUncompiledCode()) {
                JSObject* namespaceObject = uncompiledNamespace(source, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
                RETURN_IF_EXCEPTION(scope, void());
                PropertyNameArrayBuilder listed(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                namespaceObject->methodTable()->getOwnPropertyNames(namespaceObject, realm, listed, DontEnumPropertiesMode::Include);
                RETURN_IF_EXCEPTION(scope, void());
                for (const Identifier& name : listed)
                    names.append(name);
            } else {
                for (const ModuleExportData& entry : m_table.exports(source))
                    names.append(identifier(entry.name));
                pending.append(source);
            }
        }
    }
}

bool Modules::hasNameBehindItsStars(ModuleID module, UniquedStringImpl* name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    WTF::Vector<Identifier> names;
    appendNamesBehindStarsOf(module, names);
    RETURN_IF_EXCEPTION(scope, false);
    return names.containsIf([&](const Identifier& each) { return each.impl() == name; });
}

JSObject* Modules::mockSeenBy(ModuleID mocked, ModuleID importer) const
{
    ModuleID fileOrModule = m_table.fileBehindFace(mocked).value_or(mocked);
    if (!m_endedMocks.isEmpty() && !m_importersThatGotOriginal.contains(static_cast<uint64_t>(fileOrModule.index()) << 32 | importer.index())) [[unlikely]] {
        if (JSObject* ended = endedMockSeenAt(fileOrModule, momentLinked(importer)))
            return ended;
    }
    JSObject* standing = mockInPlace(fileOrModule);
    return standing && seesMockOf(fileOrModule, importer) ? standing : nullptr;
}

// `bun test` takes whoever imports a mock out of its loader when the mock ends: a load that comes to such an importer makes it anew, linked to what stands then. Here its body has run: it is linked anew.
void Modules::linkAnewWhoeverReadsEndedMockBelow(ModuleID root)
{
    VMState& state = m_instance.vm->aotState();
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        // Nothing loads a module of a preload again.
        bool isLoaded = status(module) > ModuleStatus::Linked && !m_loadedInPreload.get(module.index());
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = moduleLoadedBy(request);
            if (isLoaded && endedMockSeenAt(m_table.fileBehindFace(target).value_or(target), momentLinked(module)))
                m_momentsLoadedAnew.set(module, MomentLoadedAnew { ++state.momentOfLoads, state.momentOfTestFile, root });
            if (!request.isHoisted && !request.isDeferred && !seen.quickSet(target.index()))
                work.append(target);
        }
    }
}

// Compiled code asks who sees what only where the word of the module holds something.
JSValue Modules::wordWhereNoMockStands(ModuleID fileOrModule) const
{
    const LoadedModule* loaded = m_endedMocks.contains(fileOrModule) ? loadedIfExists(fileOrModule) : nullptr;
    return loaded && loaded->endedMocks ? JSValue(loaded->endedMocks.get()) : JSValue();
}

void Modules::forgetEndedMocksOfEarlierTestFiles()
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    for (auto& entry : m_numbersOfNamespacesOfEndedMocks) {
        LoadedModule& held = ensureLoaded(entry.key);
        for (unsigned index = 0; index < entry.value; ++index)
            uncheckedDowncast<ModuleNamespace>(asObject(storedValue(realm, held.namespacesOfEndedMocks.get(), Identifier::from(vm, index))))->clearMockOfView();
        held.namespacesOfEndedMocks.set(*this, nullptr);
    }
    m_numbersOfNamespacesOfEndedMocks.clear();
    Vector<ModuleID, 8> emptied;
    for (auto& entry : m_endedMocks) {
        // The places in the list name the objects.
        Vector<EndedMock, 2> kept;
        JSObject* before = ensureLoaded(entry.key).endedMocks.get();
        JSObject* objects = nullptr;
        for (unsigned index = 0; index < entry.value.size(); ++index) {
            if (!entry.value[index].wasLaidInPreload)
                continue;
            if (!objects)
                objects = createTableOfExports();
            storeValue(realm, objects, static_cast<unsigned>(kept.size()), storedValue(realm, before, Identifier::from(vm, index)));
            kept.append(entry.value[index]);
        }
        if (kept.isEmpty()) {
            emptied.append(entry.key);
            continue;
        }
        ensureLoaded(entry.key).endedMocks.set(*this, objects);
        entry.value = WTF::move(kept);
    }
    for (ModuleID fileOrModule : emptied) {
        m_endedMocks.remove(fileOrModule);
        ensureLoaded(fileOrModule).endedMocks.set(*this, nullptr);
        if (std::optional<SlotData> word = m_table.wordOfMock(fileOrModule); word && !mockInPlace(fileOrModule))
            ProgramStatics::writeVariable(m_instance, *word, JSValue());
    }
}

bool Modules::seesMockOf(ModuleID mocked, ModuleID importer) const
{
    if (importsOriginalBesideMock(mocked, importer)) [[unlikely]]
        return false;
    if (m_mocksOverModulesThatHaveRun.isEmpty()) [[likely]]
        return true;
    auto mock = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(mocked).value_or(mocked));
    if (mock == m_mocksOverModulesThatHaveRun.end() || m_momentsOfLoads.isEmpty())
        return true;
    // Whoever comes to the face of the file has been asked.
    if (m_table.fileBehindFace(importer) == std::optional(mock->key))
        return true;
    if (auto anew = m_momentsLoadedAnew.find(importer); anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == mock->value.momentOfTestFile && anew->value.moment > mock->value.momentThisWasLaid) [[unlikely]]
        return true;
    uint32_t momentLoaded = m_momentsOfLoads[importer.index()];
    if (!momentLoaded || momentLoaded > mock->value.momentLaid)
        return true;
    // What an earlier test file left is loaded again. What `bun test` has loaded anew in this one was linked then, and is one of this file's importers.
    return !m_loadedInPreload.get(importer.index()) && momentLinked(importer) < mock->value.momentOfTestFile;
}

// A mock that comes into the place of one that stands reaches whoever reads that one, and goes from them again when it ends.
void Modules::noteMomentMockComesInPlace(ModuleID fileOrModule, JSObject* mock, bool stoodInPlace, bool leavesEarlierMockToItsImporters)
{
    std::optional<MomentMockWasLaid> waiting = m_momentsWaitingMocksWereLaid.takeOptional(fileOrModule);
    // Only over the mock of a preload that the preload has also loaded: `bun test` patches what a test file was the first to load, and a second vi.mock() of a test file reaches whoever reads its first.
    if (JSObject* earlier = leavesEarlierMockToItsImporters ? mockInPlace(fileOrModule) : nullptr; earlier && earlier != mock && earlier == ensureLoaded(fileOrModule).mockLaidInPreload.get() && m_loadedInPreload.get(fileOrModule.index())) {
        // Whoever was linked since the call of this mock waited for it.
        if (std::optional<MomentMockWasLaid> laid = m_momentsStandingMocksWereLaid.takeOptional(fileOrModule))
            keepEndedMock(fileOrModule, earlier, *laid, waiting ? waiting->moment : 0);
    }
    MomentMockWasLaid laid = waiting && !stoodInPlace ? *waiting : momentOfNow();
    // The mock of a preload comes back behind each test file that had one of its own. Whoever the preload linked to it was linked to it then.
    LoadedModule& held = ensureLoaded(fileOrModule);
    if (held.mockLaidInPreload.get() == mock) {
        laid = m_momentsMocksOfPreloadWereLaid.get(fileOrModule);
        // A vi.mock() of the test file that has ended left it to whoever the preload had linked to it: keepEndedMock(). It has not ended: they read what stands from now on, and so the mock.module() or jest.mock()
        // of a later test file. Until then they keep it, whatever else that test file lays.
        if (auto ended = m_endedMocks.find(fileOrModule); ended != m_endedMocks.end()) {
            for (EndedMock& each : ended->value) {
                if (each.wasLaidInPreload)
                    each.momentEnded = each.momentLaid;
            }
        }
    } else if (laid.isInPreload) {
        held.mockLaidInPreload.set(*this, mock);
        m_momentsMocksOfPreloadWereLaid.set(fileOrModule, laid);
    }
    m_momentsStandingMocksWereLaid.set(fileOrModule, laid);
}

// The mock that stood in place of the module is taken away. `module`: what importers see of `fileOrModule`.
void Modules::keepMockThatStoodInPlace(ModuleID fileOrModule, ModuleID module, JSObject* endedMock)
{
    // A mock that ended with its test file is let go at a load of the next one, where nobody is left who was linked to it but what a preload loaded.
    std::optional<uint32_t> testFileOfEnd = m_testFilesInWhichMocksEnded.takeOptional(fileOrModule);
    bool endedInThisTestFile = !testFileOfEnd || *testFileOfEnd == m_instance.vm->aotState().momentOfTestFile;
    std::optional<MomentMockWasLaid> laid = m_momentsStandingMocksWereLaid.takeOptional(fileOrModule);
    if (laid && (endedInThisTestFile || laid->isInPreload))
        keepEndedMock(fileOrModule, endedMock, *laid);
    // Whoever holds the object that was made under the mock keeps the mock until the test file ends.
    for (ModuleID each : { fileOrModule, module }) {
        auto* object = endedInThisTestFile && !m_momentsOfLoads.isEmpty() ? dynamicDowncast<ModuleNamespace>(namespaceIfCreated(each, ModulePhase::Evaluation)) : nullptr;
        if (!object)
            continue;
        object->setMockOfView(*m_instance.vm, endedMock);
        LoadedModule& held = ensureLoaded(fileOrModule);
        if (!held.namespacesOfEndedMocks)
            held.namespacesOfEndedMocks.set(*this, createTableOfExports());
        unsigned& number = m_numbersOfNamespacesOfEndedMocks.add(fileOrModule, 0).iterator->value;
        storeValue(m_instance.globalObject, held.namespacesOfEndedMocks.get(), number++, object);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
