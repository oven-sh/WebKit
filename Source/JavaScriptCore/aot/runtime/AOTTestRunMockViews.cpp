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

std::span<const ModuleID> Modules::modulesThatHandOnNamespaceOf(ModuleID handedOn)
{
    if (!m_modulesThatHandOnNamespaces) {
        m_modulesThatHandOnNamespaces.emplace();
        for (unsigned index = 0; index < m_table.size(); ++index) {
            for (const ModuleExportData& entry : m_table.exports(ModuleID(index))) {
                if (entry.kind() != ModuleExportKind::Namespace)
                    continue;
                Vector<ModuleID, 2>& handers = m_modulesThatHandOnNamespaces->add(entry.targetModule(), Vector<ModuleID, 2>()).iterator->value;
                if (handers.isEmpty() || handers.last() != ModuleID(index))
                    handers.append(ModuleID(index));
            }
        }
    }
    auto found = m_modulesThatHandOnNamespaces->find(handedOn);
    return found == m_modulesThatHandOnNamespaces->end() ? std::span<const ModuleID>() : found->value.span();
}

void Modules::didReplaceNamespaceHandedOn(ModuleID handedOn)
{
    for (ModuleID hander : modulesThatHandOnNamespaceOf(handedOn)) {
        JSObject* made = namespaceIfCreated(hander, ModulePhase::Evaluation);
        LoadedModule* loaded = loadedIfExists(hander);
        JSObject* setAside = loaded ? loaded->namespaceBeforeMockInPlace.get() : nullptr;
        if (!made && !setAside)
            continue;
        for (JSObject* object : { made, setAside }) {
            if (auto* filed = dynamicDowncast<ModuleNamespace>(object))
                filed->leaveStructureThatCachesKnow(*m_instance.vm);
        }
    }
}

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
    bool isInPreload = host().isInPreload ? host().isInPreload(m_instance.globalObject).value_or(false) : !m_hasBegunTestFile;
    return { ++state.momentOfLoads, state.momentOfTestFile, isInPreload };
}

bool Modules::isSomeModuleLinkedBetween(uint32_t from, uint32_t until) const
{
    auto isBetween = [&](uint32_t moment) {
        return moment >= from && moment < until;
    };
    auto mayComeBack = [&](const LinksBeforeLoad& before) {
        return std::ranges::any_of(before.moments, [&](const auto& link) { return link.second && isBetween(link.second->moment); });
    };
    for (const LinksBeforeLoad* before = m_linksBeforeLoadInFlight; before; before = before->outer) {
        if (mayComeBack(*before))
            return true;
    }
    return std::ranges::any_of(m_momentsOfLoads, isBetween)
        || std::ranges::any_of(m_momentsLoadedAnew.values(), [&](const MomentLoadedAnew& anew) { return isBetween(anew.moment); })
        || std::ranges::any_of(m_momentsLinkedInRefusedLoads.values(), isBetween)
        || std::ranges::any_of(m_linksOfLoadsThatMayFail, [&](const LinksOfLoadThatMayFail& links) { return mayComeBack(links.before); });
}

bool Modules::mayStillBeSeen(const EndedMock& ended) const
{
    if (ended.wasLaidInPreload || ended.isKeptWhereImporterIsAskedForAgain || isSomeModuleLinkedBetween(ended.momentLaid, ended.momentEnded))
        return true;
    for (auto& [importer, earlier] : m_earlierMomentsLoadedAnew) {
        for (size_t place = 0; place < earlier.size(); ++place) {
            if (earlier[place] < ended.momentLaid || earlier[place] >= ended.momentEnded)
                continue;
            if (isSomeModuleLinkedBetween(earlier[place], place + 1 < earlier.size() ? earlier[place + 1] : m_momentsLoadedAnew.get(importer).moment))
                return true;
        }
    }
    return false;
}

void Modules::letGoOfEndedMocksNobodySees(ModuleID fileOrModule)
{
    auto entry = m_endedMocks.find(fileOrModule);
    if (entry == m_endedMocks.end())
        return;
    LoadedModule& held = ensureLoaded(fileOrModule);
    for (unsigned index = 0; index < entry->value.size(); ++index) {
        EndedMock& ended = entry->value[index];
        if (ended.momentEnded == ended.momentLaid || mayStillBeSeen(ended))
            continue;
        ended.momentEnded = ended.momentLaid;
        for (JSObject* table : { held.endedMocks.get(), held.namespacesOfEndedMocks.get() }) {
            if (table)
                removeValue(m_instance.globalObject, table, index);
        }
    }
}

std::optional<unsigned> Modules::keepEndedMock(ModuleID fileOrModule, JSObject* mock, MomentMockWasLaid laid, uint32_t momentEnded, BitVector&& exportsLeftOut)
{
    if (m_momentsOfLoads.isEmpty())
        return std::nullopt;
    VMState& state = m_instance.vm->aotState();
    if (!momentEnded)
        momentEnded = ++state.momentOfLoads;
    letGoOfEndedMocksNobodySees(fileOrModule);
    Vector<EndedMock, 2>& ended = m_endedMocks.add(fileOrModule, Vector<EndedMock, 2>()).iterator->value;
    if (!ended.isEmpty() && ended.last().momentLaid == laid.moment) {
        if (!endedMock(fileOrModule, ended.size() - 1))
            storeValue(m_instance.globalObject, ensureLoaded(fileOrModule).endedMocks.get(), static_cast<unsigned>(ended.size() - 1), mock);
        ended.last().momentEnded = momentEnded;
        ended.last().momentOfTestFile = state.momentOfTestFile;
        return ended.size() - 1;
    }
    LoadedModule& loaded = ensureLoaded(fileOrModule);
    if (!loaded.endedMocks)
        loaded.endedMocks.set(*this, createTableOfExports());
    storeValue(m_instance.globalObject, loaded.endedMocks.get(), static_cast<unsigned>(ended.size()), mock);
    if (!ended.isEmpty() && laid.moment < std::max(ended.last().momentLaid, ended.last().momentEnded))
        m_modulesWhoseEndedMocksOverlap.set(fileOrModule.index());
    ended.append({ laid.moment, momentEnded, state.momentOfTestFile, laid.isInPreload, WTF::move(exportsLeftOut) });
    return ended.size() - 1;
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

uint32_t Modules::momentLinkedAsOf(ModuleID importer, ModuleID reader) const
{
    uint32_t last = momentLinked(importer);
    uint32_t ofReader = reader && reader != importer ? momentLinked(reader) : 0;
    if (auto anew = ofReader ? m_momentsLoadedAnew.find(reader) : m_momentsLoadedAnew.end(); anew != m_momentsLoadedAnew.end() && anew->value.moment == ofReader)
        ofReader = std::max(ofReader, momentLinked(anew->value.rootOfLoad));
    if (!ofReader || last <= ofReader)
        return last;
    if (!m_momentsLoadedAnew.contains(importer))
        return last;
    uint32_t first = m_momentsOfLoads[importer.index()];
    auto found = m_earlierMomentsLoadedAnew.find(importer);
    if (found == m_earlierMomentsLoadedAnew.end())
        return first;
    const auto& earlier = found->value;
    auto younger = std::upper_bound(earlier.begin(), earlier.end(), ofReader);
    return younger == earlier.begin() ? first : std::max(first, *(younger - 1));
}

std::optional<unsigned> Modules::indexOfEndedMockSeenAt(ModuleID fileOrModule, uint32_t moment) const
{
    auto found = m_endedMocks.find(fileOrModule);
    if (!moment || found == m_endedMocks.end())
        return std::nullopt;
    uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
    const Vector<EndedMock, 2>& ended = found->value;
    auto wasSeen = [&](const EndedMock& mock) {
        return moment >= mock.momentLaid && moment < mock.momentEnded && (mock.momentOfTestFile == momentOfTestFile || mock.wasLaidInPreload);
    };
    if (m_modulesWhoseEndedMocksOverlap.get(fileOrModule.index())) [[unlikely]] {
        size_t index = ended.findIf(wasSeen);
        return index == notFound ? std::nullopt : std::optional(static_cast<unsigned>(index));
    }
    auto laidLater = std::ranges::upper_bound(ended, moment, { }, &EndedMock::momentLaid);
    if (laidLater == ended.begin() || !wasSeen(*std::prev(laidLater)))
        return std::nullopt;
    return static_cast<unsigned>(std::prev(laidLater) - ended.begin());
}

std::optional<unsigned> Modules::indexOfEndedMockSeenBy(ModuleID fileOrModule, ModuleID importer, ModuleID reader) const
{
    if (m_endedMocks.isEmpty()) [[likely]]
        return std::nullopt;
    if (auto got = m_importersThatGotOriginal.find(static_cast<uint64_t>(fileOrModule.index()) << 32 | importer.index()); got != m_importersThatGotOriginal.end()) {
        auto anew = m_momentsLoadedAnew.find(importer);
        if (!got->value.isGivenItNoMore || anew == m_momentsLoadedAnew.end() || anew->value.moment <= got->value.moment)
            return std::nullopt;
    }
    return indexOfEndedMockSeenAt(fileOrModule, momentLinkedAsOf(importer, reader));
}

JSObject* Modules::endedMock(ModuleID fileOrModule, std::optional<unsigned> index) const
{
    JSValue mock = index ? loadedIfExists(fileOrModule)->endedMocks->getDirectIndex(m_instance.globalObject, *index) : JSValue();
    return mock ? asObject(mock) : nullptr;
}

bool Modules::isNamedByEndedMockBelow(ModuleID root, const Identifier& name)
{
    if (m_endedMocks.isEmpty()) [[likely]]
        return false;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MarkedArgumentBuffer mocks;
    for (size_t index : reachedByStatements(root, AtMock::GoesOn)) {
        ModuleID importer(index);
        if (isEvicted(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (JSObject* mock = endedMockSeenBy(request.targetModule(), importer))
                mocks.append(mock);
        }
    }
    for (size_t index = 0; index < mocks.size(); ++index) {
        bool hasName = asObject(mocks.at(index))->hasOwnProperty(m_instance.globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (hasName)
            return true;
    }
    return false;
}

void Modules::appendNamesBehindStarsOf(ModuleID module, WTF::Vector<Identifier>& names)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 8> pending { module };
    while (!pending.isEmpty()) {
        ModuleID importer = pending.takeLast();
        for (ModuleRequestData request : m_table.dynamicStars(importer)) {
            ModuleID source = moduleLoadedBy(request);
            bool wasSeen = seen.quickSet(source.index());
            if (wasSeen && !m_mocks && m_endedMocks.isEmpty())
                continue;
            const ModuleData& sourceData = data(source);
            if (JSObject* ended = endedMockSeenBy(source, importer)) [[unlikely]] {
                PropertyNameArrayBuilder listed(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                ended->methodTable()->getOwnPropertyNames(ended, realm, listed, DontEnumPropertiesMode::Exclude);
                RETURN_IF_EXCEPTION(scope, void());
                for (const Identifier& name : listed)
                    names.append(name);
            } else if (mockInPlace(source) && !seesMockOf(source, importer)) [[unlikely]] {
                JSObject* own = namespaceOfModuleItself(source);
                RETURN_IF_EXCEPTION(scope, void());
                PropertyNameArrayBuilder listed(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                own->methodTable()->getOwnPropertyNames(own, realm, listed, DontEnumPropertiesMode::Include);
                RETURN_IF_EXCEPTION(scope, void());
                for (const Identifier& name : listed)
                    names.append(name);
            } else if (sourceData.capturesExports()) {
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
                if (!wasSeen)
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

JSObject* Modules::endedMockSeenBy(ModuleID mocked, ModuleID importer) const
{
    if (m_endedMocks.isEmpty()) [[likely]]
        return nullptr;
    ModuleID fileOrModule = m_table.fileBehindFace(mocked).value_or(mocked);
    JSObject* ended = endedMock(fileOrModule, indexOfEndedMockSeenBy(fileOrModule, importer));
    return ended != mockInPlace(fileOrModule) ? ended : nullptr;
}

bool Modules::keepsEndedMockOf(ModuleID module) const
{
    if (m_endedMocks.isEmpty()) [[likely]]
        return false;
    ModuleID named = moduleWithNamesOf(module);
    return m_endedMocks.contains(m_table.fileBehindFace(named).value_or(named));
}

std::optional<bool> Modules::endedMockHasNameAsSeenBy(ModuleID mocked, ModuleID holder, const Identifier& name)
{
    if (!m_collectionOfNamesBehindStars) [[likely]]
        return std::nullopt;
    JSObject* ended = endedMockSeenBy(mocked, holder);
    if (!ended)
        return std::nullopt;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& taken = m_collectionOfNamesBehindStars->namesOfEndedMocks;
    size_t index = taken.findIf([&](const auto& each) { return each.first == ended; });
    if (index == notFound) {
        PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        ended->methodTable()->getOwnPropertyNames(ended, m_instance.globalObject, names, DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        IdentifierSet own;
        for (const Identifier& each : names)
            own.add(each.impl());
        index = taken.size();
        taken.append({ ended, WTF::move(own) });
    }
    return taken[index].second.contains(name.impl());
}

JSObject* Modules::mockSeenBy(ModuleID mocked, ModuleID importer, ModuleID reader) const
{
    ModuleID fileOrModule = m_table.fileBehindFace(mocked).value_or(mocked);
    if (JSObject* ended = endedMock(fileOrModule, indexOfEndedMockSeenBy(fileOrModule, importer, reader))) [[unlikely]]
        return ended;
    JSObject* standing = mockInPlace(fileOrModule);
    return standing && seesMockOf(fileOrModule, importer) ? standing : nullptr;
}

std::optional<bool> Modules::isLeftOutInViewOf(ModuleID importer, ModuleID mocked, unsigned exportIndex) const
{
    JSObject* standing = mockInPlace(mocked);
    if (!standing && m_endedMocks.isEmpty()) [[likely]]
        return std::nullopt;
    ModuleID fileOrModule = m_table.fileBehindFace(mocked).value_or(mocked);
    if (std::optional<unsigned> ended = indexOfEndedMockSeenBy(fileOrModule, importer); ended && endedMock(fileOrModule, ended) != standing)
        return m_endedMocks.find(fileOrModule)->value[*ended].exportsLeftOut.get(exportIndex);
    if (standing && !seesMockOf(fileOrModule, importer))
        return false;
    return std::nullopt;
}

JSObject* Modules::namespaceSeenBy(ModuleID module, ModuleID importer, ModuleID reader)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    ModuleID fileOrModule = fileOf(module);
    std::optional<unsigned> ended = indexOfEndedMockSeenBy(fileOrModule, importer, reader);
    JSObject* seen = endedMock(fileOrModule, ended);
    if (!seen || (seen == mockInPlace(fileOrModule) && !m_endedMocks.find(fileOrModule)->value[*ended].isKeptWhereImporterIsAskedForAgain))
        return moduleNamespace(module, ModulePhase::Evaluation);
    LoadedModule& held = ensureLoaded(fileOrModule);
    if (JSValue kept = held.namespacesOfEndedMocks ? held.namespacesOfEndedMocks->getDirectIndex(realm, *ended) : JSValue())
        return asObject(kept);
    ModuleNamespace* object = ModuleNamespace::create(vm, namespaceStructure(true), moduleWithExportsOf(fileOrModule), ModulePhase::Evaluation, { });
    object->setMockOfView(vm, seen);
    if (!held.namespacesOfEndedMocks)
        held.namespacesOfEndedMocks.set(*this, createTableOfExports());
    storeValue(realm, held.namespacesOfEndedMocks.get(), *ended, object);
    return object;
}

void Modules::noteLoadedAnew(ModuleID module, ModuleID root)
{
    VMState& state = m_instance.vm->aotState();
    if (m_linksBeforeLoadInFlight) {
        auto before = m_momentsLoadedAnew.find(module);
        m_linksBeforeLoadInFlight->moments.append({ module, before == m_momentsLoadedAnew.end() ? std::nullopt : std::optional(before->value) });
    }
    auto refused = m_momentsLinkedInRefusedLoads.find(module);
    if (auto earlier = m_momentsLoadedAnew.find(module); earlier != m_momentsLoadedAnew.end())
        m_earlierMomentsLoadedAnew.add(module, Vector<uint32_t, 2>()).iterator->value.append(earlier->value.moment);
    m_momentsLoadedAnew.set(module, MomentLoadedAnew { refused != m_momentsLinkedInRefusedLoads.end() ? refused->value : ++state.momentOfLoads, state.momentOfTestFile, root });
    m_modulesLoadedAnewWhoseNamesAreOwed.set(module.index());
    m_modulesWithNoNamesOwedBelow.clearAll();
    if (data(module).hasDynamicStarExport())
        letGoOfNamespaceMadeUnderMock(module);
}

void Modules::bringBackLinks(const LinksBeforeLoad& before, LinksOfLoad links)
{
    for (size_t index = before.moments.size(); index--;) {
        auto& [module, moment] = before.moments[index];
        if (links == LinksOfLoad::AreMetByLaterLoads && !m_modulesLoadedAnewWhoseNamesAreOwed.get(module.index()))
            m_momentsLinkedInRefusedLoads.add(module, m_momentsLoadedAnew.get(module).moment);
        if (moment) {
            m_momentsLoadedAnew.set(module, *moment);
            m_earlierMomentsLoadedAnew.find(module)->value.removeLast();
        } else
            m_momentsLoadedAnew.remove(module);
    }
    m_modulesThatLoaderLetGo.merge(before.modulesThatLoaderLetGo);
    m_importersOfMockThatEnded.merge(before.importersOfMockThatEnded);
}

Modules::LinksOfLoadThatMayFail* Modules::linksOfLoadWalkedFor(ModuleID root)
{
    for (LinksOfLoadThatMayFail& links : m_linksOfLoadsThatMayFail) {
        if (!links.isHeldBy(links.root))
            continue;
        if (links.root == root)
            return &links;
        if (!isBeingEvaluated(links.root))
            continue;
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { links.root };
        while (!work.isEmpty()) {
            for (const ModuleRequestData& request : m_table.imports(work.takeLast())) {
                ModuleID imported = moduleLoadedBy(request);
                if (request.isHoisted || seen.quickSet(imported.index()))
                    continue;
                if (imported == root)
                    return &links;
                work.append(imported);
            }
        }
    }
    return nullptr;
}

bool Modules::reachesWhatLoadLinkedAnew(const LinksOfLoadThatMayFail& links, ModuleID root)
{
    BitVector isLinkedAnew(m_table.size());
    for (auto& [module, moment] : links.before.moments)
        isLinkedAnew.quickSet(module.index());
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (isLinkedAnew.quickGet(module.index()))
            return true;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID imported = moduleLoadedBy(request);
            if (!request.isHoisted && !seen.quickSet(imported.index()))
                work.append(imported);
        }
    }
    return false;
}

void Modules::meetLinksOfLoadsThatMayFail(ModuleID root)
{
    m_linksOfLoadsThatMayFail.removeAllMatching([&](const LinksOfLoadThatMayFail& links) {
        return links.loadsThatMayFail.containsIf([&](const LoadThatMayFail& load) {
            return !load.hadRun && status(load.root) == ModuleStatus::Evaluated;
        });
    });
    LinksOfLoadThatMayFail* own = linksOfLoadWalkedFor(root);
    ModuleID load = own ? own->root : root;
    for (LinksOfLoadThatMayFail& links : m_linksOfLoadsThatMayFail) {
        if (&links != own && !links.isHeldBy(load) && reachesWhatLoadLinkedAnew(links, root))
            links.loadsThatMayFail.append({ load, status(load) == ModuleStatus::Evaluated });
    }
}

bool Modules::holdsLinksOfLoadThatMayFail(ModuleID root) const
{
    return m_linksOfLoadsThatMayFail.containsIf([&](const LinksOfLoadThatMayFail& links) {
        return links.isHeldBy(root);
    });
}

void Modules::letLinksOfLoadStand(ModuleID root)
{
    m_linksOfLoadsThatMayFail.removeAllMatching([&](const LinksOfLoadThatMayFail& links) {
        return links.isHeldBy(root);
    });
}

void Modules::bringBackLinksOfLoadThatFailed(ModuleID root)
{
    if (status(root) >= ModuleStatus::Evaluating && status(root) != ModuleStatus::Failed)
        return letLinksOfLoadStand(root);
    for (size_t index = m_linksOfLoadsThatMayFail.size(); index--;) {
        LinksOfLoadThatMayFail& links = m_linksOfLoadsThatMayFail[index];
        if (!links.loadsThatMayFail.removeFirstMatching([&](const LoadThatMayFail& each) { return each.root == root; }))
            continue;
        bool isHeldByAnother = links.loadsThatMayFail.containsIf([&](const LoadThatMayFail& other) {
            return status(other.root) != ModuleStatus::Failed;
        });
        if (isHeldByAnother)
            continue;
        LinksBeforeLoad before = WTF::move(links.before);
        m_linksOfLoadsThatMayFail.removeAt(index);
        bringBackLinks(before, LinksOfLoad::AreForgotten);
    }
}

void Modules::forgetLinksInRefusedLoadsThatCameTo(ModuleID mocked)
{
    m_momentsLinkedInRefusedLoads.removeIf([&](auto& linked) {
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { linked.key };
        while (!work.isEmpty()) {
            ModuleID module = work.takeLast();
            if (module == mocked || m_table.fileBehindFace(module) == std::optional(mocked))
                return true;
            for (const ModuleRequestData& request : m_table.imports(module)) {
                if (!request.isHoisted && !seen.quickSet(moduleLoadedBy(request).index()))
                    work.append(moduleLoadedBy(request));
            }
        }
        return false;
    });
}

void Modules::linkAnewWhoeverReadsEndedMockBelow(ModuleID root)
{
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 32> work { root };
    auto wasLeftAloneBeforeEndOf = [&](ModuleID module, ModuleID mocked, unsigned ended) {
        auto over = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(module).value_or(module));
        return over != m_mocksOverModulesThatHaveRun.end() && !m_importersOfMockThatEnded.get(module.index()) && over->value.momentThisWasLaid < m_endedMocks.find(mocked)->value[ended].momentEnded;
    };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        bool isLoaded = status(module) > ModuleStatus::Linked && !m_loadedInPreload.get(module.index());
        bool loaderGoesBelow = !isLoaded || m_linksBeforeLoadInFlight->modulesThatLoaderLetGo.get(module.index());
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = moduleLoadedBy(request);
            ModuleID mocked = m_table.fileBehindFace(target).value_or(target);
            std::optional<unsigned> ended = isLoaded && !exportsBehindEntryOfProgram(module) ? indexOfEndedMockSeenBy(mocked, module) : std::nullopt;
            if (ended && !wasLeftAloneBeforeEndOf(module, mocked, *ended) && !keepsEndedMockWhereItIsAskedForAgain(mocked, module)) {
                noteLoadedAnew(module, root);
                loaderGoesBelow = true;
            }
        }
        if (!loaderGoesBelow)
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = moduleLoadedBy(request);
            if (!request.isHoisted && !request.isDeferred && !seen.quickSet(target.index()))
                work.append(target);
        }
    }
}

JSValue Modules::wordWhereNoMockStands(ModuleID fileOrModule) const
{
    const LoadedModule* loaded = m_endedMocks.contains(fileOrModule) ? loadedIfExists(fileOrModule) : nullptr;
    return loaded && loaded->endedMocks ? JSValue(loaded->endedMocks.get()) : JSValue();
}

void Modules::forgetEndedMocksOfEarlierTestFiles()
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    Vector<ModuleID, 8> emptied;
    for (auto& entry : m_endedMocks) {
        Vector<EndedMock, 2> kept;
        JSObject* before = ensureLoaded(entry.key).endedMocks.get();
        JSObject* namespacesBefore = ensureLoaded(entry.key).namespacesOfEndedMocks.get();
        JSObject* namespaces = nullptr;
        JSObject* objects = nullptr;
        for (unsigned index = 0; index < entry.value.size(); ++index) {
            if (!entry.value[index].wasLaidInPreload)
                continue;
            if (!objects)
                objects = createTableOfExports();
            storeValue(realm, objects, static_cast<unsigned>(kept.size()), storedValue(realm, before, Identifier::from(vm, index)));
            if (JSValue object = namespacesBefore ? namespacesBefore->getDirectIndex(realm, index) : JSValue()) {
                if (!namespaces)
                    namespaces = createTableOfExports();
                storeValue(realm, namespaces, static_cast<unsigned>(kept.size()), object);
            }
            kept.append(entry.value[index]);
            kept.last().isKeptWhereImporterIsAskedForAgain = false;
        }
        ensureLoaded(entry.key).namespacesOfEndedMocks.set(*this, namespaces);
        if (kept.isEmpty()) {
            emptied.append(entry.key);
            continue;
        }
        ensureLoaded(entry.key).endedMocks.set(*this, objects);
        entry.value = WTF::move(kept);
    }
    for (ModuleID fileOrModule : emptied) {
        m_endedMocks.remove(fileOrModule);
        m_modulesWhoseEndedMocksOverlap.clear(fileOrModule.index());
        ensureLoaded(fileOrModule).endedMocks.set(*this, nullptr);
        if (std::optional<SlotData> word = m_table.wordOfMock(fileOrModule); word && !mockInPlace(fileOrModule))
            ProgramStatics::writeVariable(m_instance, *word, JSValue());
    }
}

bool Modules::seesMockOf(ModuleID mocked, ModuleID importer) const
{
    if (importsOriginalBesideMock(mocked, importer) || keepsOriginalUnderLaterMock(mocked, importer)) [[unlikely]]
        return false;
    if (m_mocksOverModulesThatHaveRun.isEmpty()) [[likely]]
        return true;
    auto mock = m_mocksOverModulesThatHaveRun.find(m_table.fileBehindFace(mocked).value_or(mocked));
    if (mock == m_mocksOverModulesThatHaveRun.end() || m_momentsOfLoads.isEmpty())
        return true;
    if (m_table.fileBehindFace(importer) == std::optional(mock->key))
        return true;
    if (auto anew = m_momentsLoadedAnew.find(importer); anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == mock->value.momentOfTestFile && anew->value.moment > mock->value.momentThisWasLaid) [[unlikely]]
        return true;
    uint32_t momentLoaded = m_momentsOfLoads[importer.index()];
    if (!momentLoaded || momentLoaded > mock->value.momentLaid)
        return true;
    return !m_loadedInPreload.get(importer.index()) && !wasLetGoByLoader(importer) && momentLinked(importer) < mock->value.momentOfTestFile;
}

void Modules::noteImportUnderMockInPlace(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    loaded.wasImportedUnderMockInPlace = true;
    if (LoadedModule& held = ensureLoaded(fileOf(module)); held.mockInPlace && held.mockInPlace.get() == held.mockLaidInPreload.get())
        held.wasImportedUnderMockLaidInPreload = true;
    noteAskUnderMockInPlace(module);
    if (!loaded.momentOfTestFileThatLoadedItUnderMockInPlace)
        loaded.momentOfTestFileThatLoadedItUnderMockInPlace = m_instance.vm->aotState().momentOfTestFile;
}

bool Modules::wasLoadedUnderMockInPlaceByThisTestFile(ModuleID fileOrModule)
{
    ModuleID module = moduleWithExportsOf(fileOrModule);
    return !m_loadedInPreload.get(module.index()) && ensureLoaded(module).momentOfTestFileThatLoadedItUnderMockInPlace == m_instance.vm->aotState().momentOfTestFile;
}

bool Modules::keepsEndedMockWhereItIsAskedForAgain(ModuleID fileOrModule, ModuleID importer) const
{
    std::optional<unsigned> ended = indexOfEndedMockSeenAt(fileOrModule, momentLinked(importer));
    return ended && m_endedMocks.find(fileOrModule)->value[*ended].isKeptWhereImporterIsAskedForAgain;
}

bool Modules::seesEndedMockLaidInPreload(ModuleID fileOrModule, ModuleID importer) const
{
    uint32_t linked = momentLinked(importer);
    std::optional<unsigned> ended = linked < m_instance.vm->aotState().momentOfTestFile ? indexOfEndedMockSeenAt(fileOrModule, linked) : std::nullopt;
    return ended && m_endedMocks.find(fileOrModule)->value[*ended].wasLaidInPreload;
}

bool Modules::isImportedByModuleLinkedInThisTestFile(ModuleID fileOrModule, ModuleID module) const
{
    uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (momentLinked(importer) < momentOfTestFile)
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            ModuleID target = request.targetModule();
            if (target == fileOrModule || target == module || fileOf(target) == fileOrModule)
                return true;
        }
    }
    return false;
}

bool Modules::bringBackMockKeptByImportersOfThisTestFile(ModuleID fileOrModule)
{
    LoadedModule& held = ensureLoaded(fileOrModule);
    JSObject* standing = mockInPlace(fileOrModule);
    JSObject* ofPreload = held.mockLaidInPreload.get();
    if (!standing || !ofPreload || hasMockThatEnded(fileOrModule))
        return false;
    if (standing != ofPreload) {
        auto ended = m_endedMocks.find(fileOrModule);
        if (ended == m_endedMocks.end() || !ended->value.last().isKeptWhereImporterIsAskedForAgain || ended->value.last().momentOfTestFile != m_instance.vm->aotState().momentOfTestFile || endedMock(fileOrModule, ended->value.size() - 1) != ofPreload)
            return false;
        ModuleID module = moduleWithExportsOf(fileOrModule);
        keepMockThatStoodInPlace(fileOrModule, module, standing, exportsLeftOutOf(module));
        letGoOfNamespaceMadeUnderMock(module);
        setMockInPlace(fileOrModule, ofPreload, held.whatRequireAnswersUnderMockLaidInPreload.get());
    } else if (auto waiting = m_momentsWaitingMocksWereLaid.find(fileOrModule); !m_modulesWithMockToRun.get(fileOrModule.index()) || waiting == m_momentsWaitingMocksWereLaid.end() || !waiting->value.importersKeepMockThatStands)
        return false;
    forgetCallOfMock(fileOrModule);
    m_modulesWithMockToRun.clear(fileOrModule.index());
    m_modulesWithMockThatWaited.clear(fileOrModule.index());
    m_modulesLoadedBeforeMockThatWaited.clear(fileOrModule.index());
    dropErrorsOfLoadsEndedByMockOf(fileOrModule);
    return true;
}

LoadedUnderStandingMock Modules::loadedUnderStandingMockThatImportersKeep(ModuleID fileOrModule)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto noted = m_momentsWaitingMocksWereLaid.find(fileOrModule);
    if (!mockInPlace(fileOrModule) || noted == m_momentsWaitingMocksWereLaid.end() || !noted->value.importersKeepMockThatStands)
        return { };
    LoadedUnderStandingMock loaded;
    if (data(fileOrModule).kind != ModuleKind::CommonJs)
        loaded.requiredValue = ensureLoaded(fileOrModule).isKeptByRequire ? ensureLoaded(fileOrModule).requiredValue.get() : JSValue();
    else if (wasRequiredUnderMockInPlace(fileOrModule))
        loaded.requiredValue = commonJSExports(fileOrModule);
    if (ModuleID module = moduleWithExportsOf(fileOrModule); !loaded.requiredValue || ensureLoaded(module).wasImportedUnderMockInPlace) {
        loaded.namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return loaded;
}

bool Modules::keepsMockThatStandsWhileAnotherWaits(ModuleID mocked, ModuleID importer) const
{
    ModuleID fileOrModule = m_table.fileBehindFace(mocked).value_or(mocked);
    if (!m_modulesWithMockToRun.get(fileOrModule.index()) || !mockInPlace(fileOrModule) || m_table.fileBehindFace(importer) == std::optional(fileOrModule))
        return false;
    auto waiting = m_momentsWaitingMocksWereLaid.find(fileOrModule);
    uint32_t linked = momentLinked(importer);
    return waiting != m_momentsWaitingMocksWereLaid.end() && waiting->value.importersKeepMockThatStands && linked && linked < waiting->value.moment;
}

void Modules::noteMomentMockComesInPlace(ModuleID fileOrModule, JSObject* mock, bool stoodInPlace, bool leavesEarlierMockToItsImporters)
{
    std::optional<MomentMockWasLaid> waiting = m_momentsWaitingMocksWereLaid.takeOptional(fileOrModule);
    if (JSObject* earlier = leavesEarlierMockToItsImporters ? mockInPlace(fileOrModule) : nullptr; earlier && earlier != mock && earlier == ensureLoaded(fileOrModule).mockLaidInPreload.get()) {
        ModuleID module = moduleWithExportsOf(fileOrModule);
        bool isKeptByImportersOfThisTestFile = waiting && waiting->importersKeepMockThatStands;
        std::optional<MomentMockWasLaid> laid = m_loadedInPreload.get(module.index()) || isKeptByImportersOfThisTestFile ? m_momentsStandingMocksWereLaid.takeOptional(fileOrModule) : std::nullopt;
        std::optional<unsigned> ended = laid ? keepEndedMock(fileOrModule, earlier, *laid, waiting ? waiting->moment : 0, exportsLeftOutOf(module)) : std::nullopt;
        if (ended)
            m_endedMocks.find(fileOrModule)->value[*ended].isKeptWhereImporterIsAskedForAgain = isKeptByImportersOfThisTestFile;
        if (waiting && waiting->hadNamespace && (isKeptByImportersOfThisTestFile || waiting->moment == m_instance.vm->aotState().momentOfLoads)) {
            keepNamespacesMadeUnder(fileOrModule, module, earlier, ended);
            letGoOfNamespaceMadeUnderMock(module);
        }
    }
    MomentMockWasLaid laid = waiting && !stoodInPlace ? *waiting : momentOfNow();
    if (stoodInPlace && !leavesEarlierMockToItsImporters) {
        auto standing = m_momentsStandingMocksWereLaid.find(fileOrModule);
        if (standing != m_momentsStandingMocksWereLaid.end() && !standing->value.importsWrapWhatRequireMapHolds)
            laid.moment = standing->value.moment;
    }
    LoadedModule& held = ensureLoaded(fileOrModule);
    JSObject* ofPreload = held.mockLaidInPreload.get();
    bool liesOverMockOfPreload = mockInPlace(fileOrModule) == ofPreload ? m_loadedInPreload.get(moduleWithExportsOf(fileOrModule).index()) || m_libraryModulesAskedForInPreload.get(fileOrModule.index()) : m_modulesWhoseMockOfPreloadIsPatched.get(fileOrModule.index());
    m_modulesWhoseMockOfPreloadIsPatched.set(fileOrModule.index(), stoodInPlace && !leavesEarlierMockToItsImporters && ofPreload && mock != ofPreload && liesOverMockOfPreload);
    if (held.mockLaidInPreload.get() == mock) {
        MomentMockWasLaid& itsOwn = m_momentsMocksOfPreloadWereLaid.add(fileOrModule, MomentMockWasLaid { }).iterator->value;
        itsOwn.importsWrapWhatRequireMapHolds |= !held.wasImportedUnderMockLaidInPreload;
        laid = itsOwn;
        letGoOfEndedMocksLaidInPreload(fileOrModule);
        bringBackNamespaceKeptFor(fileOrModule, mock);
    } else if (laid.isInPreload) {
        held.mockLaidInPreload.set(*this, mock);
        m_momentsMocksOfPreloadWereLaid.set(fileOrModule, laid);
    } else if (auto itsOwn = m_momentsMocksOfPreloadWereLaid.find(fileOrModule); stoodInPlace && !leavesEarlierMockToItsImporters && itsOwn != m_momentsMocksOfPreloadWereLaid.end())
        itsOwn->value.importsWrapWhatRequireMapHolds = true;
    m_momentsStandingMocksWereLaid.set(fileOrModule, laid);
    m_keysWithoutEntryBehindMock.clear(fileOrModule.index());
}

void Modules::letGoOfEndedMocksLaidInPreload(ModuleID fileOrModule)
{
    auto ended = m_endedMocks.find(fileOrModule);
    if (ended == m_endedMocks.end())
        return;
    for (EndedMock& each : ended->value) {
        if (each.wasLaidInPreload)
            each.momentEnded = each.momentLaid;
    }
}

void Modules::keepMockThatStoodInPlace(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, BitVector&& exportsLeftOut)
{
    std::optional<uint32_t> testFileOfEnd = m_testFilesInWhichMocksEnded.takeOptional(fileOrModule);
    bool endedInThisTestFile = !testFileOfEnd || *testFileOfEnd == m_instance.vm->aotState().momentOfTestFile;
    std::optional<MomentMockWasLaid> laid = m_momentsStandingMocksWereLaid.takeOptional(fileOrModule);
    std::optional<unsigned> ended = laid && (endedInThisTestFile || laid->isInPreload) ? keepEndedMock(fileOrModule, endedMock, *laid, 0, WTF::move(exportsLeftOut)) : std::nullopt;
    if (!m_momentsOfLoads.isEmpty())
        keepNamespacesMadeUnder(fileOrModule, module, endedMock, ended);
}

void Modules::keepMockForWhoeverWasLinkedUnderIt(ModuleID fileOrModule)
{
    ModuleID file = fileOf(fileOrModule);
    ModuleID module = moduleWithExportsOf(file);
    JSObject* mock = mockInPlace(file);
    if (!mock || hasMockThatEnded(file))
        return;
    auto& moments = m_momentsStandingMocksWereLaid.contains(file) ? m_momentsStandingMocksWereLaid : m_momentsWaitingMocksWereLaid;
    std::optional<MomentMockWasLaid> laid = moments.takeOptional(file);
    if (!laid)
        return;
    std::optional<unsigned> ended = keepEndedMock(file, mock, *laid, 0, exportsLeftOutOf(module));
    if (ended)
        m_endedMocks.find(file)->value[*ended].isKeptWhereImporterIsAskedForAgain = true;
    keepNamespacesMadeUnder(file, module, mock, ended);
    letGoOfNamespaceMadeUnderMock(module);
    laid->moment = momentOfNow().moment;
    moments.set(file, *laid);
}

void Modules::bringBackNamespaceKeptFor(ModuleID fileOrModule, JSObject* mock)
{
    LoadedModule& held = ensureLoaded(fileOrModule);
    auto ended = m_endedMocks.find(fileOrModule);
    if (ended == m_endedMocks.end() || !held.namespacesOfEndedMocks)
        return;
    for (unsigned index = 0; index < ended->value.size(); ++index) {
        JSValue kept = endedMock(fileOrModule, index) == mock ? held.namespacesOfEndedMocks->getDirectIndex(m_instance.globalObject, index) : JSValue();
        auto* object = kept ? dynamicDowncast<ModuleNamespace>(kept) : nullptr;
        if (!object)
            continue;
        ModuleID module = object->module();
        auto* madeSince = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation));
        if (madeSince == object)
            return;
        if (madeSince) {
            madeSince->setKeepsToModule(true);
            if (!ensureLoaded(module).namespaceBeforeMockInPlace)
                ensureLoaded(module).namespaceBeforeMockInPlace.set(*this, madeSince);
        }
        object->clearMockOfView();
        setNamespace(module, ModulePhase::Evaluation, object);
        return;
    }
}

void Modules::keepNamespacesMadeUnder(ModuleID fileOrModule, ModuleID module, JSObject* endedMock, std::optional<unsigned> ended)
{
    for (ModuleID each : { fileOrModule, module }) {
        auto* object = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(each, ModulePhase::Evaluation));
        if (!object || object->keepsToModule())
            continue;
        object->setMockOfView(*m_instance.vm, endedMock);
        if (!ended)
            continue;
        LoadedModule& held = ensureLoaded(fileOrModule);
        if (!held.namespacesOfEndedMocks)
            held.namespacesOfEndedMocks.set(*this, createTableOfExports());
        storeValue(m_instance.globalObject, held.namespacesOfEndedMocks.get(), *ended, object);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
