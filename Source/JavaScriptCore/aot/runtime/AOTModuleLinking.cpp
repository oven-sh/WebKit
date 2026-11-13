/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCellButterfly.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "SyntheticModuleRecord.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/Scope.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using ModulePhase = AbstractModuleRecord::ModulePhase;
using StarExportResolution = AbstractModuleRecord::Resolution;

JSValue Modules::storedValue(JSGlobalObject* realm, JSObject* table, const Identifier& name)
{
    if (std::optional<uint32_t> index = parseIndex(name))
        return table->getDirectIndex(realm, *index);
    return table->getDirect(realm->vm(), name);
}

void Modules::storeValue(JSGlobalObject* realm, JSObject* table, unsigned number, JSValue value)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realm->vm());
    table->putDirectMayBeIndex(realm, Identifier::from(realm->vm(), number), value);
    scope.assertNoExceptionExceptTermination();
}

void Modules::removeValue(JSGlobalObject* realm, JSObject* table, unsigned number)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realm->vm());
    JSObject::deletePropertyByIndex(table, realm, number);
    scope.assertNoExceptionExceptTermination();
}

void Modules::forEachCapturedExport(ModuleID module, const ScopedLambda<void(const Identifier&, JSValue)>& functor)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(captured, m_instance.globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        JSValue value = storedValue(m_instance.globalObject, captured, name);
        functor(name, value == JSValue(captured) ? JSValue() : value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static bool findsNamesThroughRecords(const ModuleData& exporter)
{
    return exporter.isUncompiledCode() || (exporter.hasDynamicStarExport() && !exporter.capturesExports());
}

StarExportResolution::Type Modules::resolveRequiredName(ModuleID exporter, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& exporterData = data(exporter);

    if (findsNamesThroughRecords(exporterData)) {
        AbstractModuleRecord* record = recordForResolution(exporter);
        RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
        RELEASE_AND_RETURN(scope, record->resolveExport(m_instance.globalObject, name).type);
    }

    RELEASE_ASSERT(exporterData.capturesExports() || exporterData.hasExportTable());
    bool hasExport = exporterData.capturesExports() ? hasCapturedExport(exporter, name) : !!m_table.findExport(exporter, name.impl());
    RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
    if (hasExport)
        return StarExportResolution::Type::Resolved;
    return m_table.isAmbiguousExport(exporter, name.string()) ? StarExportResolution::Type::Ambiguous : StarExportResolution::Type::NotFound;
}

ReexportedNames& Modules::reexportedNames()
{
    std::unique_ptr<ReexportedNames>& names = m_instance.vm->aotState().reexportedNames;
    if (!names)
        names = makeUnique<ReexportedNames>(m_table);
    return *names;
}

bool Modules::reexportsFromMock(ModuleID exporter)
{
    ReexportersOfMocks state { m_activeMockCount.timesWritten(), m_timesMocked.size(), m_removedMocksInUse.size(), m_exportsLeftOut.size() };
    if (state == ReexportersOfMocks { }) [[likely]]
        return false;
    if (state != m_stateOfReexportersOfMocks) {
        Vector<ModuleID, 16> modules;
        auto append = [&](ModuleID module) {
            modules.append(module);
            modules.append(fileOf(module));
            modules.append(moduleWithExportsOf(fileOf(module)));
        };
        for (ModuleID module : m_timesMocked.keys())
            append(module);
        for (ModuleID module : m_removedMocksInUse.keys())
            append(module);
        for (ModuleID module : m_exportsLeftOut.keys())
            append(module);
        for (size_t module : m_modulesWithActiveMock)
            append(ModuleID(module));
        m_reexportersOfMocks = reexportedNames().transitiveReexportersOf(WTF::move(modules));
        m_stateOfReexportersOfMocks = state;
    }
    return m_reexportersOfMocks.quickGet(exporter.index());
}

std::optional<StarExportResolution::Type> Modules::tryResolveRequiredNameInTables(const RequiredNameData& requiredName)
{
    ModuleID exporter = requiredName.exporterModule();
    if (!findsNamesThroughRecords(data(exporter)) || !m_originalsBeingAsked.isEmpty() || identifier(requiredName.name) == m_instance.vm->propertyNames->defaultKeyword)
        return std::nullopt;
    if (reexportsFromMock(exporter))
        return std::nullopt;
    switch (reexportedNames().resolve(m_table, requiredName)) {
    case ReexportedNames::Answer::Unknown:
        return std::nullopt;
    case ReexportedNames::Answer::Found:
        return StarExportResolution::Type::Resolved;
    case ReexportedNames::Answer::NotFound:
        return StarExportResolution::Type::NotFound;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Modules::isMockedThroughStarExportsSlow(ModuleID module, MockFilter which)
{
    auto counts = [&](ModuleID each, ModuleID importer) {
        if (!isMocked(each))
            return false;
        if (which == MockFilter::AddingNames && importer && !seesMockOf(each, importer))
            return false;
        const LoadedModule* loaded = which == MockFilter::AddingNames && data(each).isESModuleOfProgram() ? loadedIfExists(each) : nullptr;
        return !loaded || !loaded->isPatchedByMock;
    };
    auto isMockedAsSeenBy = [&](ModuleID asked, ModuleID importer) {
        return !data(asked).isUncompiledCode() && (counts(fileOf(asked), importer) || counts(asked, importer));
    };
    if (isMockedAsSeenBy(module, ModuleID()))
        return true;
    BitVector seen(m_table.size());
    Vector<ModuleID, 8> toAsk { module };
    while (!toAsk.isEmpty()) {
        ModuleID asked = toAsk.takeLast();
        if (seen.quickSet(asked.index()))
            continue;
        if (data(asked).isUncompiledCode() || !data(asked).hasDynamicStarExport())
            continue;
        for (const ModuleRequestData& request : m_table.requests(asked)) {
            if (!request.isDynamicStarExport)
                continue;
            if (isMockedAsSeenBy(request.targetModule(), asked))
                return true;
            toAsk.append(request.targetModule());
        }
    }
    return false;
}

void Modules::checkRequiredNamesOfModulesLoadedAnewBelow(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector seen(m_table.size());
    Vector<std::pair<ModuleID, bool>, 32> work { { root, false } };
    while (!work.isEmpty()) {
        auto [module, hasAskedBelow] = work.takeLast();
        if (hasAskedBelow) {
            if (!m_modulesLoadedAnewWhoseNamesAreOwed.get(module.index()))
                continue;
            noteLoadedAnew(module, root);
            checkRequiredNames(std::span(&module, 1), ModulesAsked::ThoseLoadedAnew);
            if (scope.exception()) [[unlikely]] {
                for (auto [importer, isAboveRefusal] : work) {
                    if (isAboveRefusal)
                        m_modulesLoadedAnewWhoseNamesAreOwed.set(importer.index());
                }
                m_modulesWithNoNamesOwedBelow.clearAll();
                return;
            }
            m_modulesLoadedAnewWhoseNamesAreOwed.clear(module.index());
            continue;
        }
        if (seen.quickSet(module.index()))
            continue;
        bool standsBehindMock = isMocked(module) || activeMock(module);
        if (standsBehindMock && !isPatchedByMock(module))
            continue;
        work.append({ module, true });
        if (standsBehindMock)
            continue;
        size_t firstBelow = work.size();
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                work.append({ moduleLoadedBy(request), false });
        }
        std::ranges::reverse(work.mutableSpan().subspan(firstBelow));
    }
    m_modulesWithNoNamesOwedBelow.set(root.index());
}

void Modules::checkRequiredNames(std::span<const ModuleID> modules, ModulesAsked asked)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    for (ModuleID module : modules) {
        m_moduleWithCheckedNames = module;
        if (asked == ModulesAsked::ThoseNotLinked && status(module) != ModuleStatus::Unlinked)
            continue;
        for (const RequiredNameData& requiredName : m_table.requiredNames(module)) {
            ModuleID exporter = requiredName.exporterModule();
            if (m_mocks && (isCommonJSModuleOrFacade(exporter) || (data(exporter).isESModuleOfProgram() && data(exporter).hasDynamicStarExport())) && !m_originalsEvaluatedWhenMocked.get(fileOf(exporter).index()) && (activeMock(exporter) ? !seesMockOf(exporter, module) : isMocked(fileOf(exporter)) && importsOriginalDespiteMock(exporter, module))) [[unlikely]] {
                if (ModuleID file = fileOf(exporter); activeMock(file) && !hasIntactOriginal(file) && moduleWithExportsOf(file) != file) {
                    runFileBesideMock(file);
                    RETURN_IF_EXCEPTION(scope, void());
                }
                if (m_modulesAskedForOwnNames.get(exporter.index()))
                    continue;
                m_modulesAskedForOwnNames.set(exporter.index());
                JSObject* own = originalNamespace(exporter);
                m_modulesAskedForOwnNames.clear(exporter.index());
                RETURN_IF_EXCEPTION(scope, void());
                bool isExported = own->hasProperty(m_instance.globalObject, identifier(requiredName.name));
                RETURN_IF_EXCEPTION(scope, void());
                if (isExported || isRequestedByStarExport(requiredName))
                    continue;
                StarExportResolution::Type resolution = resolveRequiredName(exporter, identifier(requiredName.name));
                RETURN_IF_EXCEPTION(scope, void());
                if (resolution != StarExportResolution::Type::Ambiguous && dropExportThatFindsNothing(module, requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, resolution == StarExportResolution::Type::Ambiguous ? throwExportAmbiguous(requiredName) : throwExportNotFound(requiredName));
            }
            if ((activeMock(exporter) && mayGetNameFromMock(exporter, module)) || (asked == ModulesAsked::ThoseLoadedAnew && isMocked(exporter) && !wasLoadedByTestFile(exporter))) [[unlikely]] {
                bool patchesFileThatHasRun = activeMock(exporter) && hasIntactOriginal(exporter) && !isDisplacedByMock(fileOf(exporter));
                if (requiredName.mayBeAbsent || !patchesFileThatHasRun || !m_table.isAmbiguousExport(exporter, identifier(requiredName.name).string()))
                    continue;
            }
            if (m_mocks) [[unlikely]]
                noteLoadBehindMock(exporter, false);
            if (isFoundInTextOfFile(requiredName))
                continue;
            if (data(exporter).hasExportTable()) {
                if (!m_exportsLeftOut.isEmpty() && reexportsFromMock(exporter)) [[unlikely]] {
                    std::optional<unsigned> index = m_table.findExport(exporter, identifier(requiredName.name).impl());
                    if (index && isLeftOut(exporter, *index, TablesBehindStars::HaveName)) {
                        if (isMocked(exporter) || findMockForReexport(exporter, m_table.exports(exporter)[*index]) || dropExportThatFindsNothing(module, requiredName))
                            continue;
                        RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
                    }
                }
                if (requiredName.mayBeAbsent && !data(exporter).hasDynamicStarExport())
                    continue;
            }
            if (data(exporter).kind == ModuleKind::CommonJs && !data(exporter).capturesExports()) {
                bool isDefault = identifier(requiredName.name) == m_instance.vm->propertyNames->defaultKeyword;
                bool isExported = (isDefault && !ensureLoaded(exporter).didDropError) || ensureLoaded(exporter).exportsWhatMockNamesAlone;
                if (isExported || dropExportThatFindsNothing(module, requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
            }
            std::optional<StarExportResolution::Type> inTables = tryResolveRequiredNameInTables(requiredName);
            StarExportResolution::Type resolution = inTables ? *inTables : resolveRequiredName(exporter, identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            switch (resolution) {
            case StarExportResolution::Type::Resolved:
                continue;
            case StarExportResolution::Type::Ambiguous:
                if (isRequestedByStarExport(requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, throwExportAmbiguous(requiredName));
            case StarExportResolution::Type::NotFound:
            case StarExportResolution::Type::Error:
                if (requiredName.mayBeAbsent && data(exporter).exportRule != SyntheticExportRule::RuntimeModule)
                    continue;
                if (isMockedThroughStarExports(exporter, MockFilter::AddingNames) && mayGetNameFromMock(exporter, module))
                    continue;
                if (resolution == StarExportResolution::Type::NotFound && data(exporter).isOfLibrary() && exportsWhatMockNamesAlone(exporter))
                    continue;
                if (isCommonJSModuleOrFacade(exporter) && identifier(requiredName.name) == m_instance.vm->propertyNames->defaultKeyword && !ensureLoaded(exporter).didDropError && !ensureLoaded(fileOf(exporter)).didDropError && !ensureLoaded(exporter).exportsWhatMockNamesAlone)
                    continue;
                if (resolution == StarExportResolution::Type::NotFound && dropExportThatFindsNothing(module, requiredName))
                    continue;
                if (reachesDisplacedRun(exporter)) [[unlikely]]
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
            }
        }
    }
}

bool Modules::isNativeModule(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    if (moduleData.kind != ModuleKind::Runtime || !host().captureNativeModuleExports)
        return false;
    return !BuiltinTable::singleton().at(RuntimeModuleID(*moduleData.registryID())).flags.contains(RuntimeModuleFlag::HasSource);
}

static SyntheticExportRule captureRuleOf(const ModuleData& data)
{
    if (data.exportRule == SyntheticExportRule::None && data.kind == ModuleKind::CommonJs)
        return SyntheticExportRule::CommonJs;
    return data.exportRule;
}

static void checkSafeToRecurseWhileLinking(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwException(realm, scope, createModuleGraphTooDeepError(realm));
}

static bool hasUnvisitedRequests(AbstractModuleRecord* record, const VisitedModuleRecords& visited)
{
    auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record);
    return cyclic && cyclic->status() == CyclicModuleRecord::Status::Unlinked && !visited.contains(record);
}

ALWAYS_INLINE_INTO_OTHER_FILES void ModuleLinkWalk::append(ModuleID module, StepKind kind, bool resolvesNames)
{
    if (m_steps.size() == m_steps.capacity()) [[unlikely]]
        m_steps.reserveCapacity(m_steps.capacity() * 2);
    m_steps.unsafeAppendWithoutCapacityCheck(Step { module.index(), static_cast<uint32_t>(kind), resolvesNames, 0 });
}

ModuleLinkWalk::ModuleLinkWalk(const Modules& modules, ModuleID root, RefPtr<LinkWalks>&& earlier)
    : m_root(root)
    , m_earlier(WTF::move(earlier))
    , m_numberOfEarlierRoots(m_earlier ? m_earlier->roots.size() : 0)
    , m_linkEpoch(modules.linkEpoch())
{
    Vector<uint64_t> passedOver;
    if (m_earlier) {
        passedOver = modules.modulesSettledForLinks();
        for (size_t index = 0; index < passedOver.size(); ++index)
            passedOver[index] |= m_earlier->modulesWalked[index];
    }
    struct World {
        const ModuleData& data(uint32_t module) const { return table.data(ModuleID(module)); }
        bool isUnlinked(uint32_t module) const { return modules.status(ModuleID(module)) == ModuleStatus::Unlinked; }
        bool isSettledForLinks(uint32_t module) const { return modules.isSettledForLinks(ModuleID(module)); }
        std::span<const uint64_t> modulesSettledForLinks() const { return passedOver.empty() ? modules.modulesSettledForLinks() : passedOver; }
        bool loadsModulesIndividually() const { return modules.loadsModulesIndividually(); }
        bool goesByAllRequests(uint32_t module) const { return modules.linkGoesByAllRequestsOf(ModuleID(module)); }
        void didGetStateOfItsOwn(uint32_t module) const { modules.didGetStateOfItsOwn(ModuleID(module)); }
        ModuleNumbers linkRequests() const { return table.allLinkRequests(); }
        uint32_t endOfLinkRequests(uint32_t module) const { return table.endOfLinkRequests(ModuleID(module)); }
        std::span<const ModuleRequestData> requests() const { return table.allRequests(); }
        uint32_t endOfRequests(uint32_t module) const { return table.endOfRequests(ModuleID(module)); }
        bool resolvesNames(uint32_t module) const { return table.resolvesNames(ModuleID(module)); }
        const Modules& modules;
        const ModuleTable& table;
        std::span<const uint64_t> passedOver;
    };
    struct Steps {
        uint32_t size() const { return static_cast<uint32_t>(walk.m_steps.size()); }
        ALWAYS_INLINE void append(uint32_t module, StepKind kind, bool resolvesNames) { walk.append(ModuleID(module), kind, resolvesNames); }
        uint32_t moduleAt(uint32_t step) const { return walk.m_steps[step].module; }
        ALWAYS_INLINE void leave(uint32_t enter, bool resolvesNames)
        {
            walk.m_steps[enter].leave = size();
            append(moduleAt(enter), StepKind::Leave, resolvesNames);
        }
        ALWAYS_INLINE void onlyBecomesLinked(uint32_t enter)
        {
            onlyBecomesLinkedAlone(moduleAt(enter));
            walk.m_steps.shrink(enter);
        }
        ALWAYS_INLINE void onlyBecomesLinkedAlone(uint32_t module) { walk.m_modulesThatOnlyBecomeLinked.append(module); }
        ModuleLinkWalk& walk;
    };

    const ModuleTable& table = modules.table();
    if (table.data(root).hasLinkPlan()) {
        if (modules.status(root) == ModuleStatus::Unlinked)
            m_plan = table.entryPointLinkPlan();
        return;
    }
    Steps steps { *this };
    walkModulesToLink(World { modules, table, passedOver.span() }, root.index(), steps);
    m_linkEpoch = modules.linkEpoch();
    m_numberOfModulesLinkedAlone = modules.modulesLinkedAlone().size();
}

bool ModuleLinkWalk::goesPastModulesLinkedAlone(const Modules& modules, std::span<const ModuleID> linkedAlone) const
{
    const ModuleTable& table = modules.table();
    BitVector entered(table.size());
    forEachModuleOfProgram([&](ModuleID module) {
        entered.quickSet(module.index());
    });
    return std::ranges::all_of(linkedAlone, [&](ModuleID module) {
        if (!entered.quickGet(table.leaderInLink(module).index()))
            return true;
        return modules.status(module) != ModuleStatus::Unlinked && std::ranges::all_of(table.requests(module), [&](const ModuleRequestData& request) {
            return modules.isSettledForLinks(request.targetModule());
        });
    });
}

bool ModuleLinkWalk::bringUpToDate(const Modules& modules)
{
    if (m_linkEpoch != modules.linkEpoch())
        return false;
    for (size_t index = 0; index < m_numberOfEarlierRoots; ++index) {
        if (modules.status(m_earlier->roots[index]) == ModuleStatus::Unlinked)
            return false;
    }
    if (hasPlan())
        return modules.status(m_root) == ModuleStatus::Unlinked;
    if (std::span<const ModuleID> linkedAlone = modules.modulesLinkedAlone(); linkedAlone.size() != m_numberOfModulesLinkedAlone) {
        if (!goesPastModulesLinkedAlone(modules, linkedAlone.subspan(m_numberOfModulesLinkedAlone)))
            return false;
        m_numberOfModulesLinkedAlone = linkedAlone.size();
    }
    std::span<Step> steps = m_steps.mutableSpan();
    for (size_t index = 0; index < steps.size(); ++index) {
        const Step& enter = steps[index];
        if (static_cast<StepKind>(enter.kind) != StepKind::Enter || modules.status(ModuleID(enter.module)) == ModuleStatus::Unlinked)
            continue;
        if (!index)
            return false;
        size_t leave = enter.leave;
        bool isFollowedByOwnCapture = static_cast<StepKind>(steps[index + 1].kind) == StepKind::CapturesExports && steps[index + 1].module == enter.module;
        size_t firstBelow = index + 1 + isFollowedByOwnCapture;
        for (const Step& below : steps.subspan(firstBelow, leave - firstBelow)) {
            if (static_cast<StepKind>(below.kind) != StepKind::Leave && !modules.isSettledForLinks(ModuleID(below.module)))
                return false;
        }
        if (isFollowedByOwnCapture)
            std::swap(steps[index], steps[index + 1]);
        index = leave;
    }
    return true;
}

bool Modules::isLinkedWithLeader(ModuleID module) const
{
    if (m_loadsModulesIndividually)
        return false;
    ModuleStatus statusOfLeader = status(m_table.leaderInLink(module));
    return statusOfLeader != ModuleStatus::Unlinked && statusOfLeader != ModuleStatus::Linking;
}

void Modules::didGetStateOfItsOwn(ModuleID module) const
{
    if (m_loadsModulesIndividually || m_isLinkedAlone.set(module.index()))
        return;
    m_leadersWhoseLinkGoesByAllRequests.set(m_table.leaderInLink(module).index());
    m_modulesLinkedAlone.append(module);
}

bool Modules::checkIsSettledForLinks(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    bool isSettled = !moduleData.needsLink();
    if (!isSettled && moduleData.capturesExports()) {
        LoadedModule* loaded = status(module) == ModuleStatus::Evaluated ? loadedIfExists(module) : nullptr;
        isSettled = loaded && (loaded->capturedExports || loaded->capturedFields) && !loaded->bindsEarlierCapture && !loaded->isBeingLoadedByLoader;
    } else if (!isSettled)
        isSettled = !moduleData.isUncompiledCode() && status(module) != ModuleStatus::Unlinked;
    if (isSettled)
        m_modulesSettledForLinks[module.index() / 64] |= 1ull << (module.index() % 64);
    return isSettled;
}

static void forEachModuleRequestedBy(const ModuleTable& table, ModuleID module, NOESCAPE const Invocable<void(ModuleID)> auto& functor)
{
    for (const ModuleRequestData& request : table.requests(module)) {
        if (!request.isHoisted)
            functor(request.targetModule());
    }
}

static void createHub(const ModuleTable& table, MockedModules& mocks)
{
    mocks.hub.emplace();
    std::optional<uint32_t> module;
    auto numberOfImporters = [&](uint32_t imported) {
        return mocks.importers->firstImporterOf[imported + 1] - mocks.importers->firstImporterOf[imported];
    };
    for (uint32_t candidate : mocks.importers->modulesOfLargestCycle) {
        if (!mocks.modules.quickGet(candidate) && (!module || numberOfImporters(candidate) > numberOfImporters(*module)))
            module = candidate;
    }
    if (!module)
        return;
    MockedModules::Hub& hub = *mocks.hub;
    hub.reached = BitVector(table.size());
    hub.comeToHub = BitVector(table.size());
    hub.reachedBelow = BitVector(mocks.numberBelow);
    Vector<ModuleID, 32> work;
    auto reach = [&](ModuleID reached) {
        if (hub.reached.quickSet(reached.index()))
            return;
        if (std::optional<uint32_t> position = mocks.positionBelow(reached))
            hub.reachedBelow.quickSet(*position);
        if (!mocks.modules.quickGet(reached.index()))
            work.append(reached);
    };
    reach(ModuleID(*module));
    while (!work.isEmpty())
        forEachModuleRequestedBy(table, work.takeLast(), reach);
    Vector<uint32_t, 32> above { *module };
    hub.comeToHub.quickSet(*module);
    while (!above.isEmpty()) {
        uint32_t imported = above.takeLast();
        for (uint32_t importer : mocks.importers->importers.span().subspan(mocks.importers->firstImporterOf[imported], numberOfImporters(imported))) {
            if (!mocks.modules.quickGet(importer) && !hub.comeToHub.quickSet(importer))
                above.append(importer);
        }
    }
}

static Box<MockedModules> createMockedModules(const ModuleTable& table, const ImportersOfModules& importers, BitVector&& modules, const MockedModules* earlier, ModuleID added)
{
    Box<MockedModules> mocks = Box<MockedModules>::create(importers);
    mocks->modules = WTF::move(modules);
    if (earlier && earlier->positionsBelow) {
        mocks->positionsBelow = earlier->positionsBelow;
        mocks->numberBelow = earlier->numberBelow;
        mocks->addModulesBelow(table, { added });
    }
    if (earlier && earlier->comeToMock) {
        mocks->comeToMock = earlier->comeToMock;
        importers.addModulesAbove({ added }, *mocks->comeToMock);
    }
    return mocks;
}

void MockedModules::ensurePositionsBelow(const ModuleTable& table)
{
    if (positionsBelow) [[likely]]
        return;
    positionsBelow = Box<Vector<uint32_t>>::create();
    positionsBelow->fill(notBelow, table.size());
    Vector<ModuleID, 32> work;
    for (size_t module : modules)
        work.append(ModuleID(module));
    addModulesBelow(table, WTF::move(work));
}

void MockedModules::addModulesBelow(const ModuleTable& table, Vector<ModuleID, 32>&& work)
{
    Vector<uint32_t>& positions = *positionsBelow;
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        uint32_t& position = positions[module.index()];
        if (position != notBelow)
            continue;
        position = numberBelow++;
        forEachModuleRequestedBy(table, module, [&](ModuleID requested) {
            if (positions[requested.index()] == notBelow)
                work.append(requested);
        });
    }
}

const ImportersOfModules& Modules::importersOfModules() const
{
    std::unique_ptr<ImportersOfModules>& importers = m_instance.vm->aotState().importersOfModules;
    if (!importers)
        importers = makeUnique<ImportersOfModules>(m_table);
    return *importers;
}

const StatementImporters& Modules::statementImporters() const
{
    std::unique_ptr<StatementImporters>& importers = m_instance.vm->aotState().statementImporters;
    if (!importers)
        importers = makeUnique<StatementImporters>(m_table, StatementImporters::Imports::Statements);
    return *importers;
}

const StatementImporters& Modules::importersOfEveryImport() const
{
    std::unique_ptr<StatementImporters>& importers = m_instance.vm->aotState().importersOfEveryImport;
    if (!importers)
        importers = makeUnique<StatementImporters>(m_table, StatementImporters::Imports::AllRequests);
    return *importers;
}

const Vector<ModuleID>& Modules::uncompiledModules() const
{
    if (!m_uncompiledModules) {
        m_uncompiledModules.emplace();
        for (uint32_t index = 0; index < m_table.size(); ++index) {
            if (data(ModuleID(index)).isUncompiledCode())
                m_uncompiledModules->append(ModuleID(index));
        }
    }
    return *m_uncompiledModules;
}

void Modules::willMock(ModuleID mocked)
{
    forgetLoadsRefusedByCheckOfNames(mocked);
    m_modulesToReleaseAfterEvaluation.clearAll();
    m_modulesEvictedOnceLoaded.clear(mocked.index());
    if (isMocked(mocked))
        return;
    startLoadingModulesIndividually();
    m_importersAskedAboutOriginal.clear();
    didChangeLinkState();
    m_modulesWithNoNamesOwedBelow.clearAll();
    if (hasRunBesideMockAlone(mocked)) [[unlikely]] {
        m_evictedDataOriginals.set(mocked.index(), data(mocked).kind == ModuleKind::Data);
        restoreUnmockedInstance(mocked);
        bool isEvicted = hasInstances(mocked) && !currentInstance(mocked);
        m_originalsEvaluatedUnderMock.set(mocked.index(), !isEvicted);
        m_modulesOwedToImportersThatRan.clear(mocked.index());
        if (isEvicted) [[unlikely]]
            m_modulesThatRanBesideMockAlone.clear(mocked.index());
        else if (status(mocked) <= ModuleStatus::Linked)
            setStatus(mocked, ModuleStatus::Evaluated);
    } else if (LoadedModule* loaded = m_modulesThatRanBesideMockAlone.get(mocked.index()) ? loadedIfExists(mocked) : nullptr)
        loaded->errorOfModuleThatRanBesideMock.set(*this, JSValue());
    m_originalsEvaluatedWhenMocked.set(mocked.index(), status(mocked) == ModuleStatus::Evaluated && (!activeMock(mocked) || wasOriginalEvaluated(mocked)));
    if (data(mocked).kind == ModuleKind::Data && !activeMock(mocked)) {
        const LoadedModule* loaded = status(mocked) == ModuleStatus::Evaluated ? loadedIfExists(mocked) : nullptr;
        JSValue required = loaded ? loaded->requiredValue.get() : JSValue();
        m_originalsEvaluatedByRequire.set(mocked.index(), required && !dynamicDowncast<ModuleNamespace>(required));
    }
    momentOfFirstMockInTestFile(mocked);
    ++m_timesMocked.add(mocked, 0u).iterator->value;
    BitVector modules = m_mocks ? m_mocks->modules : BitVector(m_table.size());
    modules.quickSet(mocked.index());
    m_mocks = createMockedModules(m_table, importersOfModules(), WTF::move(modules), m_mocks.get(), mocked);
    didChangeLinkState();
}

void Modules::didRemoveMock(ModuleID mocked)
{
    if (wasVariantEvictedByMock(mocked))
        markImportersEvictedWithVariant(mocked);
    leaveImportersAbove(mocked);
    m_modulesRetainedByInFlightLoads.clear(mocked.index());
    m_modulesWithNoNamesOwedBelow.clearAll();
    for (DetachedLoad& left : m_detachedLoads) {
        left.wasAwaitedMockRemoved |= left.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == mocked; });
        if (left.wait->status() == JSPromise::Status::Pending && !left.mocksRemovedSince.contains(mocked) && staticImportClosure(left.required, MockTraversal::Stop).get(mocked.index()))
            left.mocksRemovedSince.append(mocked);
    }
    if (!isMocked(mocked))
        return;
    if (!activeMock(mocked) && !tryDisplaceFailedOriginal(mocked))
        bringBackErrorOfModuleThatRanBesideMock(mocked);
    m_importersAskedAboutOriginal.clear();
    didChangeLinkState();
    resetChecksOfNames();
    BitVector others = m_mocks->modules;
    others.quickClear(mocked.index());
    if (others.isEmpty()) {
        m_mocks = nullptr;
        return;
    }
    BitVector withoutMoment = others;
    if (m_testFileOfMomentsOfMocks == m_instance.vm->aotState().momentOfTestFile)
        withoutMoment.exclude(m_modulesWithMomentOfMock);
    for (size_t module : withoutMoment)
        momentOfFirstMockInTestFile(ModuleID(module));
    m_mocks = Box<MockedModules>::create(importersOfModules());
    m_mocks->modules = WTF::move(others);
}

bool Modules::MocksInGraph::rootComesToMock() const
{
    if (!m_mocks->comeToMock) {
        Vector<ModuleID, 32> mocked;
        for (size_t module : m_mocks->modules)
            mocked.append(ModuleID(module));
        m_mocks->comeToMock = BitVector(m_table.size());
        m_mocks->importers->addModulesAbove(WTF::move(mocked), *m_mocks->comeToMock);
    }
    return m_mocks->comeToMock->quickGet(m_root.index());
}

const BitVector& Modules::MocksInGraph::reachedFromRoot() const
{
    if (m_reached)
        return *m_reached;
    auto reach = [&] {
        const std::optional<MockedModules::Hub>& hub = m_mocks->hub;
        bool reachesHub = false;
        Box<BitVector> reachedBelow = Box<BitVector>::create(m_mocks->numberBelow);
        BitVector reached(m_table.size());
        Vector<ModuleID, 32> work;
        auto reachModule = [&](ModuleID module, bool isMocked) {
            if ((reachesHub && hub->reached.quickGet(module.index())) || reached.quickSet(module.index()))
                return;
            if (std::optional<uint32_t> position = m_mocks->positionBelow(module))
                reachedBelow->quickSet(*position);
            if (!isMocked)
                work.append(module);
        };
        reachModule(m_root, m_mocks->modules.quickGet(m_root.index()) && !m_loadsRootOriginal);
        while (!work.isEmpty()) {
            ModuleID module = work.takeLast();
            if (!hub && m_mocks->importers->isInLargestCycle.quickGet(module.index()))
                createHub(m_table, *m_mocks);
            if (!reachesHub && hub && hub->comeToHub.get(module.index())) {
                reachesHub = true;
                reachedBelow->merge(hub->reachedBelow);
            }
            if (reachesHub && hub->reached.quickGet(module.index()))
                continue;
            forEachModuleRequestedBy(m_table, module, [&](ModuleID requested) {
                reachModule(requested, m_mocks->modules.quickGet(requested.index()));
            });
        }
        return reachedBelow;
    };
    m_reached = m_loadsRootOriginal ? reach() : m_mocks->reachedFromRoots.ensure(m_root, reach).iterator->value;
    return *m_reached;
}

bool Modules::loadRuntimeModules(ModuleID root)
{
    return loadRuntimeModules(ModuleLinkWalk(*this, root));
}

bool Modules::loadRuntimeModules(const ModuleLinkWalk& walk, NativeModuleLoading nativeModuleLoading)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool didRunModule = false;
    MocksInGraph mocks = mocksInGraph(walk.root());
    walk.replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf != ModuleLinkWalk::Leaf::CapturesExports || data(module).exportRule != SyntheticExportRule::RuntimeModule)
            return;
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (isNativeModule(module)) {
            if (nativeModuleLoading == NativeModuleLoading::Eager && !didCaptureExports(module)) {
                didRunModule = true;
                captureExports(module);
            }
            return;
        }
        if (exportsWhatMockNamesAlone(module))
            return;
        LoadedModule* loaded = loadedIfExists(module);
        if (status(module) != ModuleStatus::Evaluated || (loaded && loaded->cachedLoadError)) {
            didRunModule = true;
            executeForLoader(module);
        }
    }, [](ModuleID, bool) { });
    return didRunModule;
}

void Modules::link(ModuleID root)
{
    if (!data(root).needsLink() || (data(root).linkFollowsRequests() && status(root) != ModuleStatus::Unlinked))
        return;
    if (std::unique_ptr<ModuleLinkWalk> kept = takeKeptLinkWalk(root)) {
        link(*kept);
        return;
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModuleLinkWalksCreated);
    link(ModuleLinkWalk(*this, root));
}

void Modules::link(const ModuleLinkWalk& walk)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (m_moduleWhoseOwnNamespaceIsMade != walk.root()) [[likely]] {
        runMocksInGraph(walk.root());
        RETURN_IF_EXCEPTION(scope, void());
    }

    VisitedModuleRecords visited;
    captureSyntheticExports(walk, visited);
    RETURN_IF_EXCEPTION(scope, void());

    ModuleLinkingStack stack;
    LinkContext context { stack };
    finishLink(walk, context);
    if (scope.exception()) [[unlikely]] {
        for (CyclicModuleRecord* record : stack)
            record->setStatus(CyclicModuleRecord::Status::Unlinked);
        return;
    }
    ASSERT(stack.isEmpty());
}

void Modules::captureSyntheticExports(ModuleID root, VisitedModuleRecords& visited)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(root).linkFollowsRequests() && status(root) == ModuleStatus::Unlinked) {
        if (ModuleRecord* record = recordIfExists(root); record && visited.contains(record))
            return;
    }
    std::unique_ptr<ModuleLinkWalk> walk = takeKeptLinkWalk(root);
    if (!walk) {
        countEvent(*m_instance.vm, RuntimeCounterID::ModuleLinkWalksCreated);
        RefPtr earlier = m_walksOfLink && m_walksOfLink->visited == &visited && !m_walksOfLink->roots.isEmpty() ? m_walksOfLink : nullptr;
        bool skipsEarlierWalks = !!earlier;
        walk = makeUnique<ModuleLinkWalk>(*this, root, WTF::move(earlier));
        if (skipsEarlierWalks && hasFileCapturedAfterEvaluation(*walk) && !resolvesNamesOutsideLibrary(*walk, scope))
            walk = makeUnique<ModuleLinkWalk>(*this, root);
    }
    captureSyntheticExports(*walk, visited);
    RETURN_IF_EXCEPTION(scope, void());
    keepLinkWalk(WTF::move(walk));
}

bool Modules::hasFileCapturedAfterEvaluation(const ModuleLinkWalk& walk) const
{
    return walk.hasPlan() || walk.hasCaptureAfterEvaluation(*this);
}

bool Modules::resolvesNamesOutsideLibrary(const ModuleLinkWalk& walk, ThrowScope& scope) const
{
    bool resolvesNames = false;
    walk.replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [](ModuleLinkWalk::Leaf, ModuleID) { }, [&](ModuleID module, bool moduleResolvesNames) {
        resolvesNames = resolvesNames || (moduleResolvesNames && !findsItsNamesInLibraryAlone(module));
    });
    return resolvesNames;
}

void Modules::captureSyntheticExports(const ModuleLinkWalk& walk, VisitedModuleRecords& visited)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool resolvesNames = hasFileCapturedAfterEvaluation(walk) && resolvesNamesOutsideLibrary(walk, scope);
    MocksInGraph mocks = mocksInGraph(walk.root());
    bool needsPath = data(walk.root()).reachesUncompiledCode();
    RefPtr<LinkWalks> walks = m_walksOfLink;
    if (walks && walks->visited != &visited)
        walks = nullptr;
    if (!walks && needsPath)
        walks = adoptRef(*new LinkWalks(visited));
    SetForScope walksOfLink(m_walksOfLink, walks);
    Vector<ModuleID, 16> path;
    size_t visitedPathLength = 0;

    bool didEnterRoot = walk.replay(scope, [&](ModuleID module) {
        if (status(module) != ModuleStatus::Unlinked)
            return false;
        ModuleRecord* record = recordIfExists(module);
        if (record && !visited.add(record).isNewEntry)
            return false;
        if (needsPath) [[unlikely]]
            path.append(module);
        return true;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            if (isDisplacedRunKept(module, walk.root())) [[unlikely]]
                return;
            if (module != walk.root()) {
                if (!activeMock(fileOf(module))) {
                    takeCopiesAnewWhereMockCallAloneTookThem(module);
                    RETURN_IF_EXCEPTION(scope, void());
                }
                bindLatestCapture(module);
                RETURN_IF_EXCEPTION(scope, void());
            }
            if (data(module).capturesExportsBehindEvaluation() && (!resolvesNames || module == walk.root()))
                captureExportsBehindEvaluation(module);
            else {
                captureExports(module);
                if (LoadedModule* loaded = loadedIfExists(module))
                    loaded->isBeingLoadedByLoader = false;
            }
            return;
        }
        ASSERT(needsPath);
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurseWhileLinking(realm, scope);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasUnvisitedRequests(record, visited)) {
            for (; visitedPathLength < path.size(); ++visitedPathLength)
                visited.add(ensureRecord(path[visitedPathLength]));
        }
        record->generateDeferredSyntheticModules(realm, visited);
    }, [&](ModuleID, bool) {
        if (!needsPath) [[likely]]
            return;
        path.removeLast();
        visitedPathLength = std::min(visitedPathLength, path.size());
    });
    RETURN_IF_EXCEPTION(scope, void());
    if (!walks || !didEnterRoot)
        return;
    if (walks->modulesWalked.isEmpty())
        walks->modulesWalked.fill(0, m_modulesSettledForLinks.size());
    walk.forEachModuleOfProgram([&](ModuleID module) {
        if (!mocks.hides(module))
            walks->modulesWalked[module.index() / 64] |= 1ull << (module.index() % 64);
    });
    walks->roots.append(walk.root());
}

void Modules::finishLinkForLoader(ModuleID root, LinkContext& context)
{
    SetForScope rootLinkedByLoader(m_rootLinkedByLoader, isReplacedByMock(root) ? std::nullopt : std::optional(root));
    finishLink(root, context);
}

void Modules::finishLink(ModuleID root, LinkContext& context)
{
    finishLink(*takeLinkWalk(root), context);
}

void Modules::finishLink(const ModuleLinkWalk& walk, LinkContext& context)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto isUnlinked = [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    };

    Vector<ModuleID, 16> modulesThatResolveNames;
    MocksInGraph mocks = mocksInGraph(walk.root());
    bool didEnterRoot = walk.replay(scope, isUnlinked, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            if (isDisplacedRunKept(module, walk.root())) [[unlikely]]
                return;
            runFileThatWaitsForItsTurn(module);
            RETURN_IF_EXCEPTION(scope, void());
            if (!data(module).capturesExportsBehindEvaluation())
                captureExports(module);
            return;
        }
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurseWhileLinking(realm, scope);
        RETURN_IF_EXCEPTION(scope, void());
        unsigned index = record->innerModuleLinking(realm, context.stack, context.index, context.scriptFetcher);
        RETURN_IF_EXCEPTION(scope, void());
        context.index = index;
        JSObject* namespaceObject = uncompiledNamespace(module, ModulePhase::Evaluation, data(module).isUncompiledCodeImportedAsNamespace() ? NamespaceUse::IsShownToProgram : NamespaceUse::CarriesBindings);
        RETURN_IF_EXCEPTION(scope, void());
        setValue(module, namespaceObject);
    }, [&](ModuleID module, bool resolvesNames) {
        bool runsItself = !isMocked(module) || (module == walk.root() && mocks.loadsRootOriginal());
        if (resolvesNames && runsItself && !mocks.hides(module))
            modulesThatResolveNames.append(module);
    });
    RETURN_IF_EXCEPTION(scope, void());
    if (!didEnterRoot) {
        if (m_rootLinkedByLoader == walk.root()) {
            m_instancesLinkedByLoader.appendVector(linkInstances(walk.root()));
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }

    KeepsAnswers keepsAnswers(*this);
    for (const ModuleID& module : modulesThatResolveNames) {
        if (!m_displacedRunImporters.isEmpty() && keepsDisplacedRun(module)) [[unlikely]]
            continue;
        checkRequiredNames(std::span(&module, 1));
        if (scope.exception()) [[unlikely]] {
            m_linkFailures.set(walk.root(), module);
            m_failedLoadModules.set(module.index());
            if (m_table.anyModuleHasInstances())
                noteLinkFailure(walk.root(), m_moduleWithCheckedNames);
            return;
        }
    }
    if (!m_linkFailures.isEmpty()) [[unlikely]]
        m_linkFailures.remove(walk.root());

    for (ModuleID module : modulesThatResolveNames) {
        if (!isUnlinked(module))
            continue;
        fillDeferredExports(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (m_rootLinkedByLoader == walk.root()) {
        m_instancesLinkedByLoader.appendVector(linkInstances(walk.root()));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (data(walk.root()).hasLinkPlan()) {
        didLinkEntryPoint(walk.root(), mocks);
        return;
    }
    walk.replay(scope, isUnlinked, [](ModuleLinkWalk::Leaf, ModuleID) { }, [&](ModuleID module, bool) {
        if (!mocks.hides(module))
            setStatus(module, ModuleStatus::Linked);
    });
    for (uint32_t index : walk.modulesThatOnlyBecomeLinked()) {
        ModuleID module(index);
        if (!isUnlinked(module) || mocks.hides(module))
            continue;
        if (storedStatus(module) == StoredModuleStatus::InRecord)
            setStatus(module, ModuleStatus::Linked);
        else
            setStoredStatus(module, StoredModuleStatus::Linked);
        m_modulesSettledForLinks[index / 64] |= 1ull << (index % 64);
    }
    linkRecordsWithLeaders();
}

void Modules::linkRecordsWithLeaders()
{
    m_unlinkedRecordsBelowLeaders.removeAllMatching([&](ModuleID module) {
        if (status(module) != ModuleStatus::Unlinked)
            return true;
        if (!isLinkedWithLeader(module))
            return false;
        setStatus(module, ModuleStatus::Linked);
        return true;
    });
}

bool Modules::didCaptureExports(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && (loaded->capturedExports || loaded->capturedFields);
}

void Modules::captureExportsBehindEvaluation(ModuleID module)
{
    if (didCaptureExports(module))
        return;
    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linking:
    case ModuleStatus::Linked:
        ensureRecord(module);
        return;
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Evaluated:
        captureExports(module);
        return;
    case ModuleStatus::Failed:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::captureExportsOfLibraryModules(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::unique_ptr<ModuleLinkWalk> walk = takeLinkWalk(root);
    walk->replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf != ModuleLinkWalk::Leaf::CapturesExports || data(module).exportRule != SyntheticExportRule::RuntimeModule || isNativeModule(module))
            return;
        if (!didCaptureExports(module))
            ensureLoaded(module).isBeingLoadedByLoader = true;
        captureExports(module);
    }, [](ModuleID, bool) { });
    RETURN_IF_EXCEPTION(scope, void());
    keepLinkWalk(WTF::move(walk));
}

JSValue Modules::executeForLoader(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue error = ensureLoaded(module).cachedLoadError) {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    JSValue value = execute(module);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception)) [[unlikely]]
        ensureLoaded(module).cachedLoadError.set(*this, exception->value());
    return value;
}

JSValue Modules::evaluateForCapture(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope<CollectionOfNamesBehindStars*> nobodyCollectsNamesBehindStars(m_collectionOfNamesBehindStars, nullptr);
    if (!data(module).hasExportTable())
        RELEASE_AND_RETURN(scope, data(module).kind == ModuleKind::RuntimeSource ? executeForLoader(module) : execute(module));

    std::optional<ModuleID> file = m_table.moduleBehindFacade(module);
    RELEASE_ASSERT(file);
    bool isModuleEvaluatingBeforeFacade = status(*file) == ModuleStatus::Evaluating && (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked);
    bool isReadPastMockThatWaits = m_moduleWhoseOwnNamespaceIsMade == module && status(*file) == ModuleStatus::Evaluated && m_modulesWithPendingMock.get(file->index());
    bool areEvaluated = data(*file).isReloadable() && status(*file) == ModuleStatus::Evaluated && status(module) == ModuleStatus::Evaluated;
    if (!isModuleEvaluatingBeforeFacade && !isReadPastMockThatWaits && !areEvaluated) {
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, exportsSeenByLoader(*file));
}

JSObject* Modules::generateNativeModuleExports(ModuleID module, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (status(module) == ModuleStatus::Failed) {
        scope.release();
        throwEvaluationError(module);
        return nullptr;
    }
    JSObject* deferredExportsSource = host().captureNativeModuleExports(m_instance.globalObject, *data(module).registryID(), names, values);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (!vm.isTerminationException(exception) && !didCaptureExports(module))
            setFailed(module, exception->value());
        return nullptr;
    }
    if (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked)
        setStatus(module, ModuleStatus::Evaluated);
    size_t defaultExport = names.find(vm.propertyNames->defaultKeyword);
    if (defaultExport != notFound && values.at(defaultExport).isObject())
        m_state.builtinCells->didLoadNativeModule(RuntimeModuleID(*data(module).registryID()), asObject(values.at(defaultExport)));
    return deferredExportsSource;
}

void Modules::captureExports(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);

    if (loaded.capturedExports || loaded.capturedFields)
        return;
    checkSafeToRecurseWhileLinking(realm, scope);
    RETURN_IF_EXCEPTION(scope, void());
    if (m_table.anyModuleHasInstances() && data(module).isOfLibrary()) [[unlikely]]
        noteLibraryModuleLoaded(module);

    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    JSObject* deferredExportsSource = nullptr;
    if (isNativeModule(module)) {
        deferredExportsSource = generateNativeModuleExports(module, names, values);
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        if (!m_displacedRunImporters.isEmpty() && m_displacedRunImporters.contains(fileOf(module).index()) && status(fileOf(module)) <= ModuleStatus::Linked) [[unlikely]]
            tellOfKeptRun(fileOf(module));
        JSValue value = evaluateForCapture(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (loaded.capturedExports || loaded.capturedFields)
            return;
        bool hasCapturedFields = tryCaptureFields(module, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasCapturedFields)
            RELEASE_AND_RETURN(scope, fillCapturedExports(module));

        JSObject* standIn = m_copiesThatGettersGave && m_facadeReusingGetterValues == module ? standInWhoseGettersAreTheirCopies(value, m_copiesThatGettersGave) : nullptr;
        RETURN_IF_EXCEPTION(scope, void());
        if (standIn) {
            MarkedArgumentBuffer read;
            host().captureSyntheticExports(realm, captureRuleOf(data(module)), standIn, names, read);
            RETURN_IF_EXCEPTION(scope, void());
            for (unsigned i = 0; i < read.size(); ++i)
                values.append(read.at(i) == JSValue(standIn) ? value : read.at(i));
        } else
            deferredExportsSource = host().captureSyntheticExports(realm, captureRuleOf(data(module)), value, names, values);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(realm, scope);
        return;
    }
    RELEASE_ASSERT(names.size() == values.size());
    if (loaded.capturedExports || loaded.capturedFields)
        return;

    JSObject* captured = createTableOfExports();
    for (unsigned i = 0; i < names.size(); ++i) {
        JSValue exported = values.at(i);
        RELEASE_ASSERT(exported || deferredExportsSource);
        captured->putDirectMayBeIndex(realm, names[i], exported ? exported : JSValue(captured));
        RETURN_IF_EXCEPTION(scope, void());
    }
    loaded.capturedExports.set(*this, captured);
    loaded.deferredExportsSource.set(*this, deferredExportsSource);
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

void Modules::copyExportsOfFinishedFile(ModuleID facade)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::optional<ModuleID> file = m_table.moduleBehindFacade(facade);
    if (!file || !ensureLoaded(facade).capturedExports)
        return;
    if (loader()->isMakingModuleInline(moduleKey(facade)) || loader()->isMakingModuleInline(moduleKey(*file)))
        return;
    JSValue value = exportsSeenByLoader(*file);
    RETURN_IF_EXCEPTION(scope, void());
    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    scope.release();
    host().captureSyntheticExports(m_instance.globalObject, captureRuleOf(data(facade)), value, names, values);
}

void Modules::takeCopiesAnewWhereMockCallAloneTookThem(ModuleID facade)
{
    ModuleID file = fileOf(facade);
    if (isMocked(file) || !m_facadesCapturedForMock.get(facade.index()) || isRequiredModulePatchPending(facade))
        return;
    bool wasImported = activeMock(file) || wasLoadedByImport(file) || namespaceIfCreated(facade, ModulePhase::Evaluation);
    m_facadesCapturedForMock.clear(facade.index());
    if (!wasImported)
        captureExportsAgainKeepingWhatGettersGave(facade);
}

void Modules::captureExportsAgainKeepingWhatGettersGave(ModuleID facade)
{
    JSObject* copies = ensureLoaded(facade).capturedExports.get();
    EnsureStillAliveScope keepsCopies(copies);
    SetForScope copiesThatGettersGave(m_copiesThatGettersGave, copies);
    SetForScope facadeReusingGetterValues(m_facadeReusingGetterValues, std::optional(facade));
    recaptureExports(facade);
}

JSObject* Modules::standInWhoseGettersAreTheirCopies(JSValue exports, JSObject* copies)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = exports.getObject();
    if (!object || object->type() == ProxyObjectType)
        return nullptr;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, realm, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* standIn = constructEmptyObject(realm);
    bool standsInForGetter = false;
    for (const Identifier& name : names) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool hasName = object->methodTable()->getOwnPropertySlot(object, realm, name, slot);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!hasName)
            continue;
        if (slot.isTaintedByOpaqueObject())
            return nullptr;
        JSValue copy = slot.isAccessor() ? storedValue(realm, copies, name) : JSValue();
        bool getterWasRead = copy && copy != JSValue(copies);
        standsInForGetter |= getterWasRead;
        JSValue value = getterWasRead ? copy : object->get(realm, name);
        RETURN_IF_EXCEPTION(scope, nullptr);
        unsigned attributes = slot.attributes() & PropertyAttribute::DontEnum;
        if (std::optional<uint32_t> index = parseIndex(name))
            standIn->putDirectIndex(realm, *index, value, attributes, PutDirectIndexLikePutDirect);
        else
            standIn->putDirect(vm, name, value, attributes);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return standsInForGetter ? standIn : nullptr;
}

void Modules::recaptureExports(ModuleID moduleOrFacade)
{
    if (forgetCapturedExports(moduleOrFacade))
        captureExports(m_table.facadeOf(moduleOrFacade).value_or(moduleOrFacade));
}

bool Modules::forgetCapturedExports(ModuleID moduleOrFacade)
{
    ModuleID module = m_table.facadeOf(moduleOrFacade).value_or(moduleOrFacade);
    if (!isCommonJSModuleOrFacade(module) || !didCaptureExports(module))
        return false;
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.didDropError || loaded.exportsWhatMockNamesAlone)
        return false;
    loaded.capturedExports.set(*this, nullptr);
    loaded.deferredExportsSource.set(*this, nullptr);
    loaded.capturedFields.set(*this, nullptr);
    loaded.sourceOfCapturedFields.set(*this, nullptr);
    loaded.bindsEarlierCapture = true;
    unsettleForLinks(module);
    if (namespaceIfCreated(module, ModulePhase::Evaluation)) {
        if (data(module).holdsNamespaceInValue())
            m_modulesWithEarlierNamespaceInValue.set(module.index());
        loaded.namespaceObject.set(*this, nullptr);
    }
    return true;
}

void Modules::endMockOfLibraryModule(ModuleID module)
{
    if (!m_libraryModulesWithMock.get(module.index()))
        return;
    m_libraryModulesWithMock.clear(module.index());
    bool stoodInPlaceOfModule = m_libraryModulesThatMockReplaces.clear(module.index());
    if (stoodInPlaceOfModule)
        m_momentsMocksInPlaceOfLibraryModulesEnded.set(module, ++m_instance.vm->aotState().momentOfLoads);
    auto discardNamespaceOnExit = makeScopeExit([&] {
        if (stoodInPlaceOfModule)
            discardNamespace(module);
    });
    LoadedModule* found = loadedIfExists(module);
    if (!found)
        return;
    LoadedModule& loaded = *found;
    releaseMockInRequireCache(module);
    loaded.exportsOfMock.set(*this, nullptr);
    bool restoresOriginal = isLibraryOriginalDisplaced(module);
    if (restoresOriginal) {
        restoreLibraryOriginal(module, loaded.exportsWhatMockNamesAlone ? LibraryMock::Replaces : LibraryMock::Patches);
        discardNamespaceOnExit.release();
        loaded.exportsWhatMockNamesAlone = false;
        loaded.takesNamesThatMockAdds = false;
        loaded.isYetToBeRequiredBehindMock = false;
        loaded.wasLoadedBehindMock = false;
        loaded.wasRequiredBehindMock = false;
    }
    if (!loaded.exportsWhatMockNamesAlone) {
        VM& vm = *m_instance.vm;
        JSGlobalObject* realm = m_instance.globalObject;
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSObject* before = loaded.exportsBeforeMock;
        loaded.exportsBeforeMock.set(*this, nullptr);
        if (!before)
            return;
        PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        JSObject::getOwnPropertyNames(before, realm, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
        for (const Identifier& name : names) {
            JSValue value = storedValue(realm, before, name);
            if (value == JSValue(before)) {
                JSObject* source = loaded.deferredExportsSource;
                value = source ? source->get(realm, name) : jsUndefined();
                RETURN_IF_EXCEPTION(scope, void());
            }
            tryReplaceCapturedExport(module, name.impl(), value);
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
    loaded.exportsBeforeMock.set(*this, nullptr);
    loaded.exportsWhatMockNamesAlone = false;
    loaded.takesNamesThatMockAdds = false;
    loaded.isYetToBeRequiredBehindMock = false;
    loaded.wasLoadedBehindMock = false;
    loaded.wasRequiredBehindMock = false;
    if (!didCaptureExports(module) && !namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    loaded.capturedExports.set(*this, nullptr);
    loaded.deferredExportsSource.set(*this, nullptr);
    loaded.capturedFields.set(*this, nullptr);
    loaded.sourceOfCapturedFields.set(*this, nullptr);
    unsettleForLinks(module);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        const Identifier& name = identifier(syntheticExport.name);
        if (!syntheticExport.hasVariable || !syntheticExport.isBoundByRequiredName || !isDeferredExport(loaded, name))
            continue;
        JSValue found = capturedValue(module, name);
        RETURN_IF_EXCEPTION(scope, void());
        ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found ? found : jsUndefined());
    }
    if (auto* held = dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation))) {
        ModuleNamespace::DynamicExports exportsOfModule;
        createDynamicExports(module, exportsOfModule.names, exportsOfModule.capturedValues, exportsOfModule.liveSources);
        RETURN_IF_EXCEPTION(scope, void());
        held->replaceDynamicExports(*m_instance.vm, exportsOfModule);
    }
    RELEASE_AND_RETURN(scope, takeNamesBehindFollowedStarsAnew());
}

void Modules::takeNamesBehindFollowedStarsAnew()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        auto* held = data(module).hasDynamicStarExport() ? dynamicDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation)) : nullptr;
        if (!held)
            continue;
        ModuleNamespace::DynamicExports exportsOfModule;
        createDynamicExports(module, exportsOfModule.names, exportsOfModule.capturedValues, exportsOfModule.liveSources);
        RETURN_IF_EXCEPTION(scope, void());
        held->replaceDynamicExports(*m_instance.vm, exportsOfModule);
    }
}

static Vector<ModuleID, 4> modulesThatKeepNamesHandedOnBy(Modules& modules, ModuleID file)
{
    const ModuleTable& table = modules.table();
    Vector<ModuleID, 4> holders;
    BitVector isHolder(table.size());
    BitVector seen(table.size());
    Vector<ModuleID, 8> pending { file };
    while (!pending.isEmpty()) {
        ModuleID module = pending.takeLast();
        if (seen.quickSet(module.index()))
            continue;
        const ModuleData& moduleData = table.data(module);
        if (!moduleData.hasExportTable() || moduleData.capturesExports()) {
            if (!isHolder.quickSet(module.index()))
                holders.append(module);
            continue;
        }
        for (const ModuleExportData& moduleExport : table.exports(module)) {
            switch (moduleExport.kind()) {
            case ModuleExportKind::Variable:
                if (const Modules::SyntheticExportPlace* origin = modules.syntheticExportKeptIn(moduleExport.variable); origin && !isHolder.quickSet(origin->module.index()))
                    holders.append(origin->module);
                break;
            case ModuleExportKind::Field:
            case ModuleExportKind::Namespace:
                break;
            case ModuleExportKind::Indirect:
                pending.append(table.indirectExport(moduleExport).targetModule());
                break;
            }
        }
        for (const ModuleRequestData& request : table.requests(module)) {
            if (request.isDynamicStarExport)
                pending.append(request.targetModule());
        }
    }
    return holders;
}

bool Modules::isFailedESModule(ModuleID file) const
{
    return data(file).isESModuleOfProgram() && !data(file).isFacade() && status(file) == ModuleStatus::Failed;
}

void Modules::noteWhetherFileHasThrownWhereMockIsMade(ModuleID file, bool displacesEarlierMock, bool displacesModule)
{
    if (displacesEarlierMock && !displacesModule)
        return;
    LoadedModule& loaded = ensureLoaded(file);
    loaded.hadThrownWhenMockWasMade = !displacesModule && isFailedESModule(file) && !wasReleasedByLoader(file) && !loaded.holdsValueOfMockThatEnded;
    loaded.wasDisplacedAfterFailure |= displacesModule && isFailedESModule(file) && !wasLoadedWithImporter(file);
}

void Modules::saveExportsBeforePatch(ModuleID file)
{
    m_moduleBeingPatched = file;
    LoadedModule& loaded = ensureLoaded(file);
    bool isFirstPatch = !std::exchange(loaded.isPatchedByMock, true);
    if (isFirstPatch) {
        loaded.wasEvaluatedWhenPatched = status(file) == ModuleStatus::Evaluated;
        loaded.hadThrownWhenPatched = loaded.hadThrownWhenMockWasMade;
    }
    if (loaded.valueBeforeMock || (!isFirstPatch && data(file).kind != ModuleKind::CommonJs))
        return;
    if (JSValue value = data(file).kind == ModuleKind::CommonJs ? commonJSExports(file) : JSValue(loaded.requiredValue))
        loaded.valueBeforeMock.set(*this, value);
}

void Modules::restoreCapturedExports(ModuleID holder)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule* held = loadedIfExists(holder);
    JSObject* before = held ? held->exportsBeforeMock.get() : nullptr;
    if (!before)
        return;
    held->exportsBeforeMock.set(*this, nullptr);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(before, realm, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        JSValue value = storedValue(realm, before, name);
        if (value == JSValue(before)) {
            JSObject* source = held->deferredExportsSource;
            value = source ? source->get(realm, name) : jsUndefined();
            RETURN_IF_EXCEPTION(scope, void());
        }
        tryReplaceCapturedExport(holder, name.impl(), value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::restorePatchedExports(ModuleID file)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(file);
    if (data(file).hasExportTable()) {
        for (ModuleID holder : modulesThatKeepNamesHandedOnBy(*this, file)) {
            if (m_libraryModulesWithMock.get(holder.index()) || isPatchedByMock(fileOf(holder)))
                continue;
            restoreCapturedExports(holder);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    giveBackNamespacesHandedOn(loaded);
    if (JSObject* variables = loaded.variablesBeforeMock) {
        loaded.variablesBeforeMock.set(*this, nullptr);
        PropertyNameArrayBuilder places(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        JSObject::getOwnPropertyNames(variables, realm, places, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
        for (const Identifier& place : places) {
            JSValue kept = storedValue(realm, variables, place);
            loaded.holdsValueOfMockThatEnded |= kept == JSValue(variables);
            if (std::optional<uint32_t> encoded = parseIndex(place); encoded && kept != JSValue(variables))
                ProgramStatics::writeVariable(m_instance, SlotData::decode(*encoded), kept);
        }
        for (auto& [field, kept] : std::exchange(loaded.fieldsBeforeMock, { })) {
            loaded.holdsValueOfMockThatEnded |= kept == JSValue(variables);
            if (kept != JSValue(variables))
                writeField(field.first, SlotData::decode(field.second), kept);
        }
    }
}

void Modules::endMockLaidOverFile(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_moduleBeingPatched = std::nullopt;
    if (ModuleID exporting = moduleWithExportsOf(file); status(exporting) == ModuleStatus::Evaluated && !namespaceIfCreated(exporting, ModulePhase::Evaluation))
        restoreNamespaceOfOriginal(exporting);
    LoadedModule* found = loadedIfExists(file);
    if (!found || !std::exchange(found->isPatchedByMock, false))
        return;
    LoadedModule& loaded = *found;
    bool wasImportedUnderPatch = endImportedPatch(file);
    didRemoveMock(file);
    loaded.exportsOfMock.set(*this, nullptr);
    restorePatchedExports(file);
    RETURN_IF_EXCEPTION(scope, void());
    if (JSValue value = loaded.valueBeforeMock; value && !hasInstances(file)) {
        loaded.valueBeforeMock.set(*this, JSValue());
        if (data(file).kind == ModuleKind::CommonJs)
            setCommonJSExports(file, value);
        else if (loaded.requiredValue) {
            loaded.requiredValue.set(*this, value);
            if (JSObject* entry = loaded.moduleObjectFromRequire) {
                entry->putDirect(vm, vm.propertyNames->exports, value);
                loaded.exportsGivenToObjectMadeByRequire.set(*this, value);
            }
        }
    } else if (data(file).kind != ModuleKind::CommonJs && loaded.requiredValue) {
        loaded.valueBeforeMock.set(*this, JSValue());
        loaded.requiredValue.set(*this, JSValue());
        if (hasInstances(file)) {
            loaded.moduleObjectFromRequire.set(*this, nullptr);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
        }
    }
    ModuleID module = moduleWithExportsOf(file);
    LoadedModule* withExports = loadedIfExists(module);
    if (withExports && withExports->exportsWhatMockNamesAlone) {
        bool wasImported = wasImportedUnderPatch || withExports->wasLoadedBehindMock || withExports->hasLinkedImporter || namespaceIfCreated(module, ModulePhase::Evaluation) || status(module) >= ModuleStatus::Evaluating;
        withExports->exportsBeforeMock.set(*this, nullptr);
        withExports->exportsWhatMockNamesAlone = false;
        withExports->takesNamesThatMockAdds = false;
        withExports->wasLoadedBehindMock = false;
        withExports->wasRequiredBehindMock = false;
        loaded.isYetToBeRequiredBehindMock = false;
        withExports->capturedExports.set(*this, nullptr);
        withExports->deferredExportsSource.set(*this, nullptr);
        withExports->capturedFields.set(*this, nullptr);
        withExports->capturedStars.set(*this, nullptr);
        withExports->sourceOfCapturedFields.set(*this, nullptr);
        unsettleForLinks(module);
        if (namespaceIfCreated(module, ModulePhase::Evaluation)) {
            if (data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
                setValue(module, JSValue());
            withExports->namespaceObject.set(*this, nullptr);
        }
        if (module != file && (status(file) < ModuleStatus::Evaluating || !wasImported))
            return;
        if (module != file && status(module) < ModuleStatus::Evaluating)
            m_facadesCapturedForMock.set(module.index());
        RELEASE_AND_RETURN(scope, captureExports(module));
    }
    RELEASE_AND_RETURN(scope, restoreCapturedExports(module));
}

void Modules::releaseNamespaceBeforeMock(ModuleID file)
{
    if (!m_modulesEvaluatedBeforeMock.get(file.index()))
        return;
    m_modulesEvaluatedBeforeMock.clear(file.index());
    LoadedModule* exporting = loadedIfExists(moduleWithExportsOf(file));
    if (exporting && !activeMock(file))
        exporting->namespaceBeforeMockInPlace.set(*this, nullptr);
}

JSObject* Modules::namespaceBeforeMock(ModuleID file)
{
    ModuleID facade = moduleWithExportsOf(file);
    bool isImported = facade != file && status(facade) >= ModuleStatus::Evaluating && !m_facadesCapturedForMock.get(facade.index());
    if (data(file).kind == ModuleKind::CommonJs && isImported && hasIntactOriginal(file) && status(file) == ModuleStatus::Evaluated && !delegatesWithoutPlugins(file))
        return originalNamespace(facade);
    if (data(file).kind != ModuleKind::CommonJs || !m_modulesEvaluatedBeforeMock.get(file.index()) || status(file) != ModuleStatus::Evaluated || delegatesWithoutPlugins(file))
        return nullptr;
    LoadedModule* loaded = loadedIfExists(file);
    if (loaded && loaded->isPatchedByMock)
        return nullptr;
    LoadedModule* exporting = loadedIfExists(moduleWithExportsOf(file));
    bool wasMadeWhileMockWaited = exporting && exporting->namespaceBeforeMockInPlace;
    if (!wasMadeWhileMockWaited && (!m_modulesWithPendingMock.get(file.index()) || hasIntactOriginal(file) || activeMock(file)))
        return nullptr;
    stopWhereExportsLeaveCompiledCode(file);
    return namespaceOfModuleThatRanBesideMock(moduleWithExportsOf(file));
}

JSValue Modules::exportsBeforeMock(ModuleID file, bool evenIfEvaluationFailed, OwedBelowOriginal owedBelowOriginal, bool* didExportReadThrow)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isCommonJSFile = data(file).kind == ModuleKind::CommonJs;
    if (hasIntactOriginal(file) && !delegatesWithoutPlugins(file)) {
        if (isCommonJSFile) {
            m_facadesCapturedForMock.clear(moduleWithExportsOf(file).index());
            JSValue original = ensureLoaded(file).exportsOfOriginalBesideMock.get();
            return original ? original : commonJSExports(file);
        }
        JSValue error = ensureLoaded(file).errorOfModuleThatRanBesideMock.get();
        if (ModuleRecord* record = error ? nullptr : displacedRecord(file))
            error = record->evaluationError();
        if (error) [[unlikely]] {
            reportRetainedFailure(file);
            throwException(realm, scope, error);
            return { };
        }
        if (owedBelowOriginal == OwedBelowOriginal::IsPaid) {
            evaluateDueDependencies(file);
            RETURN_IF_EXCEPTION(scope, { });
        }
        RELEASE_AND_RETURN(scope, originalNamespace(file));
    }
    if (JSValue original = isCommonJSFile && activeMock(file) ? ensureLoaded(file).exportsOfOriginalBesideMock.get() : JSValue())
        return original;
    if (LoadedModule* loaded = isCommonJSFile && isMocked(file) ? loadedIfExists(file) : nullptr; loaded && !loaded->isPatchedByMock) {
        if (JSValue original = loaded->exportsFromOwnFactory.get())
            return original;
    }
    if (!isCommonJSFile && isMocked(file) && m_originalsEvaluatedUnderMock.get(file.index()) && !m_modulesRetainedByInFlightLoads.get(file.index()) && status(file) == ModuleStatus::Evaluated && !delegatesWithoutPlugins(file)) {
        if (JSValue error = ensureLoaded(file).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
            throwException(realm, scope, error);
            return { };
        }
        if (owedBelowOriginal == OwedBelowOriginal::IsPaid) {
            evaluateDueDependencies(file);
            RETURN_IF_EXCEPTION(scope, { });
        }
        RELEASE_AND_RETURN(scope, originalNamespace(file));
    }
    if (activeMock(file) || delegatesWithoutPlugins(file))
        return { };
    LoadedModule* loaded = loadedIfExists(file);
    bool isLaidOver = loaded && loaded->isPatchedByMock;
    bool hasThrown = evenIfEvaluationFailed && loaded && (isLaidOver ? loaded->hadThrownWhenPatched : loaded->hadThrownWhenMockWasMade && !isMocked(file) && isFailedESModule(file));
    bool wasEvaluatedWithoutMock = m_modulesEvaluatedBeforeMock.get(file.index()) || m_filesEvaluatedByOwnFactory.get(file.index());
    bool hadRun = isLaidOver ? hasThrown || loaded->wasEvaluatedWhenPatched : hasThrown || ((!isMocked(file) || wasEvaluatedWithoutMock) && status(file) == ModuleStatus::Evaluated);
    if (!hadRun)
        return { };
    if (isCommonJSFile) {
        JSValue kept = isLaidOver ? loaded->valueBeforeMock.get() : JSValue();
        if (kept)
            return kept;
        stopWhereExportsLeaveCompiledCode(file);
        RELEASE_AND_RETURN(scope, exportsSeenByLoader(file));
    }
    if (!data(file).hasExportTable())
        return { };
    JSObject* variables = isLaidOver ? loaded->variablesBeforeMock.get() : nullptr;

    auto keptBy = [&](ModuleID holder, const Identifier& name) -> JSValue {
        LoadedModule* held = isLaidOver ? loadedIfExists(holder) : nullptr;
        JSObject* before = held ? held->exportsBeforeMock.get() : nullptr;
        if (!before)
            return { };
        JSValue value = storedValue(realm, before, name);
        if (value != JSValue(before))
            return value;
        JSObject* source = held->deferredExportsSource;
        RELEASE_AND_RETURN(scope, source ? source->get(realm, name) : jsUndefined());
    };
    BitVector seenBehindStars;
    auto keptValueOf = [&](auto& self, ModuleID module, UniquedStringImpl* name) -> JSValue {
        for (unsigned steps = 0; steps <= m_table.size(); ++steps) {
            std::optional<unsigned> index = m_table.findExport(module, name);
            if (!index) {
                if (!isLaidOver)
                    return { };
                JSValue value = keptBy(module, Identifier::fromUid(vm, name));
                RETURN_IF_EXCEPTION(scope, { });
                if (value || !data(module).hasDynamicStarExport() || standsForWholeModule(module))
                    return value;
                if (name == vm.propertyNames->defaultKeyword.impl() || seenBehindStars.set(module.index()))
                    return { };
                for (ModuleRequestData request : m_table.dynamicStars(module)) {
                    value = self(self, request.targetModule(), name);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (value)
                        return value;
                }
                return { };
            }
            const ModuleExportData& moduleExport = m_table.exports(module)[*index];
            switch (moduleExport.kind()) {
            case ModuleExportKind::Variable:
                if (const SyntheticExportPlace* origin = syntheticExportKeptIn(moduleExport.variable))
                    return keptBy(origin->module, identifier(origin->name));
                return variables ? storedValue(realm, variables, Identifier::from(vm, moduleExport.variable.encoded())) : JSValue();
            case ModuleExportKind::Field: {
                ResolvedField field = variables ? resolveField(module, *index, nullptr) : ResolvedField { };
                return field.instance ? loaded->fieldsBeforeMock.get(std::pair { field.instance, field.slot.encoded() }) : JSValue();
            }
            case ModuleExportKind::Namespace:
                return { };
            case ModuleExportKind::Indirect: {
                const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
                module = moduleLoadedUnder(indirectExport.targetModule());
                name = identifier(indirectExport.importName).impl();
                break;
            }
            }
        }
        return { };
    };

    JSObject* namespaceObject = moduleNamespace(file, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    namespaceObject->methodTable()->getOwnPropertyNames(namespaceObject, realm, names, DontEnumPropertiesMode::Exclude);
    if (scope.exception()) [[unlikely]] {
        if (didExportReadThrow)
            *didExportReadThrow = true;
        return { };
    }
    JSObject* exports = constructEmptyObject(realm);
    for (const Identifier& name : names) {
        if (!m_table.findExport(file, name.impl()) && (!data(file).hasDynamicStarExport() || isAddedByMock(file, name)))
            continue;
        seenBehindStars.clearAll();
        JSValue value = keptValueOf(keptValueOf, file, name.impl());
        RETURN_IF_EXCEPTION(scope, { });
        if (variables && value == JSValue(variables))
            continue;
        if (!value) {
            value = namespaceObject->get(realm, name);
            if (scope.exception()) [[unlikely]] {
                if (didExportReadThrow)
                    *didExportReadThrow = true;
                return { };
            }
        }
        exports->putDirectMayBeIndex(realm, name, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return exports;
}

void Modules::exportWhatMockNamesAlone(ModuleID file, bool hasRun)
{
    ModuleID module = moduleWithExportsOf(file);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.exportsWhatMockNamesAlone) {
        if (loaded.wasLoadedBehindMock) {
            ensureLoaded(file).isYetToBeRequiredBehindMock = !loaded.wasRequiredBehindMock;
            return;
        }
        if (data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
            setValue(module, JSValue());
        loaded.namespaceObject.set(*this, nullptr);
        LoadedModule* loadedFile = loadedIfExists(file);
        hasRun = loadedFile && loadedFile->hadRunAheadOfMocksThatNameAlone;
    } else if (namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    else if (didCaptureExports(module)) {
        if (!m_facadesCapturedForMock.get(module.index()) || loaded.loadWord || loaded.hasLinkedImporter || wasLoadedByImport(file))
            return;
        forgetCapturedExports(module);
        loaded.bindsEarlierCapture = false;
        m_facadesCapturedForMock.clear(module.index());
        if (status(module) == ModuleStatus::Evaluated) {
            startLoadingModulesIndividually();
            forgetLoadOfEntry(module);
        }
        hasRun = true;
    }
    loaded.capturedExports.set(*this, createTableOfExports());
    loaded.exportsWhatMockNamesAlone = true;
    ensureLoaded(file).isYetToBeRequiredBehindMock = !hasRun;
    ensureLoaded(file).hadRunAheadOfMocksThatNameAlone = hasRun;
}

bool Modules::wasRequiredUnderActiveMock(ModuleID file) const
{
    LoadedModule* loaded = loadedIfExists(file);
    return loaded && loaded->wasRequiredUnderActiveMock;
}

void Modules::noteLoadBehindMock(ModuleID moduleOrFacade, bool isRequired)
{
    LoadedModule* loaded = loadedIfExists(moduleWithExportsOf(fileOf(moduleOrFacade)));
    if (!loaded || !loaded->exportsWhatMockNamesAlone)
        return;
    loaded->wasLoadedBehindMock = true;
    loaded->wasRequiredBehindMock |= isRequired;
}

bool Modules::exportsWhatMockNamesAlone(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsWhatMockNamesAlone;
}

void Modules::fillNamesThatMockLacks(ModuleID file)
{
    ModuleID module = moduleWithExportsOf(file);
    if (exportsWhatMockNamesAlone(module))
        fillCapturedExports(module);
}

void Modules::answerFirstRequireBehindMock(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || !std::exchange(loaded->isYetToBeRequiredBehindMock, false))
        return;
    ModuleID module = moduleWithExportsOf(file);
    JSValue marker = capturedValue(module, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, void());
    if (!marker || !marker.toBoolean(m_instance.globalObject))
        return;
    JSValue answer = capturedValue(module, vm.propertyNames->defaultKeyword);
    RETURN_IF_EXCEPTION(scope, void());
    if (answer && !answer.isUndefined())
        setCommonJSExports(file, answer);
}

void Modules::bindLatestCapture(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->bindsEarlierCapture) [[likely]]
        return;
    loaded->bindsEarlierCapture = false;
    if (m_modulesWithEarlierNamespaceInValue.get(module.index())) {
        JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, void());
        m_modulesWithEarlierNamespaceInValue.clear(module.index());
        setNamespace(module, ModulePhase::Evaluation, namespaceObject);
        loaded->namespaceObject.set(*this, nullptr);
    }
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

bool Modules::tryCaptureFields(ModuleID module, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    SyntheticExportRule rule = captureRuleOf(data(module));
    if (rule == SyntheticExportRule::None)
        return false;
    Object* exports = compiledObjectOrNull(value);
    if (!exports || (exports->hasExtraProperties() && !hasPendingStars(*exports)))
        return false;
    const LayoutData& layout = exports->layout();
    if (layout.kind() != LayoutKind::Record)
        return false;
    RuntimeLayoutTable& layouts = exports->state().objects->layouts;
    for (const FieldData& field : layout.fields()) {
        if ((field.isAccessor() && exports->isInSlot(field)) || (layouts.hasStringKey(field) && parseIndex(*layouts.nameOf(vm, field))))
            return false;
    }

    bool isMarkedAsESModule = false;
    if (rule == SyntheticExportRule::CommonJs) {
        PropertySlot slot(exports, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool hasMark = exports->getPropertySlot(realm, vm.propertyNames->__esModule, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (hasMark) {
            JSValue mark = slot.getValue(realm, vm.propertyNames->__esModule);
            TRY_CLEAR_EXCEPTION(scope, false);
            isMarkedAsESModule = mark && !mark.isUndefinedOrNull() && mark.pureToBoolean() == TriState::True;
        }
    }

    JSCellButterfly* stars = nullptr;
    if (exports->hasExtraProperties()) {
        stars = snapshotOfPendingStars(vm, *exports);
        if (!stars)
            return false;
    }
    JSCellButterfly* fields = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, layout.numberOfFields);
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate())
            continue;
        JSValue held = exports->readInline(field);
        if (!held)
            held = exports->read(field);
        if (held)
            fields->setIndex(vm, layout.indexOf(field), held);
    }
    LoadedModule& loaded = ensureLoaded(module);
    loaded.capturedFields.set(*this, fields);
    loaded.capturedStars.set(*this, stars);
    loaded.sourceOfCapturedFields.set(*this, exports);
    loaded.capturedFieldsAreOfESModule = isMarkedAsESModule;
    loaded.capturedFieldsAreOfRuntimeModule = rule == SyntheticExportRule::RuntimeModule;
    return true;
}

JSValue Modules::capturedField(const LoadedModule& loaded, UniquedStringImpl* name) const
{
    VM& vm = *m_instance.vm;
    const CommonIdentifiers& names = *vm.propertyNames;
    bool isMarked = loaded.capturedFieldsAreOfESModule;
    bool isDefault = name == names.defaultKeyword.impl();
    bool isOfRuntimeModule = loaded.capturedFieldsAreOfRuntimeModule;
    JSCellButterfly* stars = isOfRuntimeModule ? nullptr : loaded.capturedStars.get();
    Object& source = *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get());
    if (!isOfRuntimeModule && isDefault && !isMarked)
        return &source;
    if (!isOfRuntimeModule && ((isMarked && name == names.__esModule.impl()) || (stars && name == names.constructor.impl())))
        return { };
    const LayoutData& layout = source.layout();
    const FieldData* field = source.state().objects->layouts.field(vm, layout, name);
    JSValue held = field && !field->isPrivate() ? loaded.capturedFields->get(layout.indexOf(*field)) : JSValue();
    if (held && (isOfRuntimeModule || (stars && isMarked)) && field->flags & fieldIsNotEnumerable)
        held = JSValue();
    if (!held && stars)
        held = valueInSnapshotOfPendingStars(vm, source, stars, name);
    return !held && isDefault ? JSValue(&source) : held;
}

void Modules::makeTableOfCapturedFields(LoadedModule& loaded)
{
    VM& vm = *m_instance.vm;
    Object& source = *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get());
    RuntimeLayoutTable& layouts = source.state().objects->layouts;
    const LayoutData& layout = source.layout();
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();

    JSObject* captured = createTableOfExports();
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate() || !layouts.hasStringKey(field))
            continue;
        UniquedStringImpl* name = layouts.nameOf(vm, field);
        JSValue held = name == defaultName ? JSValue() : capturedField(loaded, name);
        if (held)
            captured->putDirect(vm, Identifier::fromUid(vm, name), held);
    }
    if (loaded.capturedStars) {
        forEachBindingInSnapshotOfPendingStars(vm, source, loaded.capturedStars, [&](UniquedStringImpl* name, JSValue value) {
            if (name != vm.propertyNames->constructor.impl())
                captured->putDirect(vm, Identifier::fromUid(vm, name), value);
        });
    }
    captured->putDirect(vm, vm.propertyNames->defaultKeyword, capturedField(loaded, defaultName));
    loaded.capturedExports.set(*this, captured);
}

void Modules::fillCapturedExports(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.bindsEarlierCapture || hasIntactOriginal(module) || isLibraryOriginalDisplaced(module)) [[unlikely]]
        return;

    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        if (!syntheticExport.hasVariable)
            continue;
        const Identifier& name = identifier(syntheticExport.name);
        if (syntheticExport.isBoundByRequiredName && isDeferredExport(loaded, name))
            continue;
        JSValue found = capturedValue(module, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (found || syntheticExport.absentIsUndefined || loaded.exportsWhatMockNamesAlone)
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found ? found : jsUndefined());
        else if (loaded.holdsValuesOfMock) [[unlikely]]
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, JSValue());
    }
    loaded.holdsValuesOfMock = loaded.exportsWhatMockNamesAlone;
}

bool Modules::isDeferredExport(const LoadedModule& loaded, const Identifier& name) const
{
    JSObject* captured = loaded.capturedExports;
    return captured && storedValue(m_instance.globalObject, captured, name) == JSValue(captured);
}

bool Modules::findsItsNamesInLibraryAlone(ModuleID module) const
{
    if (!m_table.syntheticExports(module).empty())
        return false;
    return std::ranges::all_of(m_table.requiredNames(module), [&](const RequiredNameData& requiredName) {
        if (isFoundInTextOfFile(requiredName)) {
            LoadedModule* exporter = loadedIfExists(requiredName.exporterModule());
            return !exporter || !exporter->exportsWhatMockNamesAlone;
        }
        const ModuleData& exporter = data(requiredName.exporterModule());
        if (requiredName.mayBeAbsent && exporter.hasExportTable() && !exporter.hasDynamicStarExport())
            return true;
        return exporter.exportRule == SyntheticExportRule::RuntimeModule;
    });
}

static void fillDeferredExportFoundThroughRecords(JSGlobalObject* realm, AbstractModuleRecord* exporter, const Identifier& name)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    AbstractModuleRecord::Resolution resolution = exporter->resolveExport(realm, name);
    RETURN_IF_EXCEPTION(scope, void());
    if (resolution.type != AbstractModuleRecord::Resolution::Type::Resolved || resolution.localName == vm.propertyNames->starNamespacePrivateName)
        return;
    if (resolution.isLiveExport(vm))
        return;
    auto* compiled = dynamicDowncast<ModuleRecord>(resolution.moduleRecord);
    if (!compiled)
        RELEASE_AND_RETURN(scope, SyntheticModuleRecord::materializeLazyExport(realm, resolution.moduleRecord, resolution.localName));
    if (resolution.compiledExportIndex || compiled->isReplacedByMock())
        return;
    scope.release();
    compiled->readExport(realm, resolution.localName, std::nullopt);
}

void Modules::fillDeferredExports(ModuleID importer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    for (const RequiredNameData& requiredName : m_table.requiredNames(importer)) {
        ModuleID exporter = requiredName.exporterModule();
        if (findsNamesThroughRecords(data(exporter))) {
            LoadedModule* loaded = loadedIfExists(exporter);
            if (data(exporter).isUncompiledCode() && (!loaded || !loaded->uncompiledRecord))
                continue;
            if (tryResolveRequiredNameInTables(requiredName))
                continue;
            AbstractModuleRecord* record = recordForResolution(exporter);
            RETURN_IF_EXCEPTION(scope, void());
            fillDeferredExportFoundThroughRecords(m_instance.globalObject, record, identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        if (data(exporter).exportRule != SyntheticExportRule::RuntimeModule)
            continue;
        for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(exporter)) {
            if (syntheticExport.name != requiredName.name)
                continue;
            if (!syntheticExport.hasVariable || ProgramStatics::readVariableWithoutAllocating(m_instance, syntheticExport.variable))
                break;
            JSValue found = capturedValue(exporter, identifier(syntheticExport.name), MockMode::Bypass);
            RETURN_IF_EXCEPTION(scope, void());
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found ? found : jsUndefined());
            break;
        }
    }
}

auto Modules::collectDynamicExports(ModuleID module) -> Vector<DynamicExport>
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    Vector<DynamicExport> result;
    auto sortByName = [](Vector<DynamicExport>& exports) {
        AOT::sort(exports, [](const DynamicExport& a, const DynamicExport& b) {
            return codePointCompare(StringView(*a.name.impl()), StringView(*b.name.impl())) < 0;
        });
    };

    if (moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) {
        forEachCapturedExport(module, [&](const Identifier& name, JSValue value) {
            result.append({ name, value });
        });
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->exportsAddedByMock && !m_moduleWhoseOwnNamespaceIsMade) {
            JSObject* added = loaded->exportsAddedByMock;
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            JSObject::getOwnPropertyNames(added, realm, names, DontEnumPropertiesMode::Include);
            RETURN_IF_EXCEPTION(scope, { });
            for (const Identifier& name : names)
                result.append({ name, storedValue(realm, added, name) });
        }
        bool isOfModuleItself = m_moduleWhoseOwnNamespaceIsMade == module;
        if (!moduleData.hasDynamicStarExport() || (standsForWholeModule(module) && !isOfModuleItself)) {
            sortByName(result);
            return result;
        }
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(realm, scope);
            return result;
        }

        CollectionOfNamesBehindStars collection;
        SetForScope collectsNamesBehindStars(m_collectionOfNamesBehindStars, &collection);
        AbstractModuleRecord* record = isOfModuleItself && activeMock(module) ? ensureDisplacedRecord(module) : recordForResolution(module);
        IdentifierSet listedNames;
        BitVector entriesThatYieldToStars;
        UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, uint32_t, IdentifierRepHash> whereHeldNamesLand;
        bool mockMaySettleNames = hasAnyActiveMock() || keepsSomeEndedMock();
        IdentifierSet namesPassedByWhileHidden;
        Vector<Identifier> namesToSearchFor;
        std::optional<Identifier> nameSearchedFor;
        auto mayBeListedNow = [&](const Identifier& name) {
            return nameSearchedFor ? name == *nameSearchedFor : !namesPassedByWhileHidden.contains(name.impl());
        };
        auto isExportedThroughStar = [&](const Identifier& name, std::optional<ModuleID> source = std::nullopt) {
            if (!mayBeListedNow(name))
                return false;
            std::optional<unsigned> own = m_table.findExport(module, name.impl());
            bool isHeldByTable = own && !isLeftOut(module, *own, TablesBehindStars::MayLackName);
            if (isHeldByTable && mockMaySettleNames && source && data(*source).capturesExports() && m_table.exports(module)[*own].isFromStarExport && !listedNames.contains(name.impl())) [[unlikely]] {
                uint32_t landsOn = whereHeldNamesLand.ensure(name.impl(), [&]() -> uint32_t {
                    bool nothingIsKept = false;
                    SetForScope asksAnew(m_answersKept ? m_answersKept->isPassedBy : nothingIsKept, true);
                    StarExportResolution resolution = record->resolveExport(realm, name);
                    auto* provider = resolution.type == StarExportResolution::Type::Resolved && resolution.localName == name ? dynamicDowncast<ModuleRecord>(resolution.moduleRecord) : nullptr;
                    return provider ? provider->module().index() + 1 : 0;
                }).iterator->value;
                if (landsOn != source->index() + 1)
                    return false;
                listedNames.add(name.impl());
                entriesThatYieldToStars.set(*own);
                return true;
            }
            if (name == vm.propertyNames->defaultKeyword || isHeldByTable || !listedNames.add(name.impl()).isNewEntry)
                return false;
            return record->resolveExport(realm, name).type == StarExportResolution::Type::Resolved;
        };
        Vector<std::pair<ModuleID, unsigned>, 8> pending { { module, 0 } };
        UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, Vector<ModuleID, 2>, IdentifierRepHash> filesThatTakeNameBack;
        Vector<Identifier> namesTakenBack;
        auto filesThatTakeBack = [&](const Identifier& name) -> Vector<ModuleID, 2>& {
            auto added = filesThatTakeNameBack.add(name.impl(), Vector<ModuleID, 2>());
            if (added.isNewEntry)
                namesTakenBack.append(name);
            return added.iterator->value;
        };
        auto isTakenBackByFileOnPath = [&](const Identifier& name) {
            auto taken = filesThatTakeNameBack.find(name.impl());
            return taken != filesThatTakeNameBack.end() && std::ranges::any_of(taken->value, [&](ModuleID file) {
                return std::ranges::contains(pending, file, &std::pair<ModuleID, unsigned>::first);
            });
        };
        auto appendLiveExports = [&](ModuleID source, auto&& names, JSObject* namespaceObject = nullptr) {
            for (const Identifier& name : names) {
                if (!mayBeListedNow(name))
                    continue;
                if (isTakenBackByFileOnPath(name)) {
                    if (!nameSearchedFor && !listedNames.contains(name.impl()) && namesPassedByWhileHidden.add(name.impl()).isNewEntry)
                        namesToSearchFor.append(name);
                    continue;
                }
                bool isExported = isExportedThroughStar(name, source);
                RETURN_IF_EXCEPTION(scope, void());
                if (!isExported)
                    continue;
                if (!namespaceObject) {
                    auto beingMade = std::ranges::find(m_namespacesBeingMade, source, &std::pair<ModuleID, JSObject*>::first);
                    namespaceObject = beingMade != m_namespacesBeingMade.end() ? beingMade->second : moduleNamespace(source, ModulePhase::Evaluation);
                    RETURN_IF_EXCEPTION(scope, void());
                }
                result.append({ name, JSValue(), namespaceObject });
            }
        };

        BitVector seen(m_table.size());
        seen.quickSet(module.index());
        struct NamesInOrder {
            WTF::Vector<Identifier> inOrder;
            IdentifierSet all;
        };
        auto namesAskedAmong = [&](NamesInOrder& names) -> std::span<const Identifier> {
            if (!nameSearchedFor)
                return names.inOrder.span();
            if (names.all.isEmpty()) {
                for (const Identifier& name : names.inOrder)
                    names.all.add(name.impl());
            }
            return names.all.contains(nameSearchedFor->impl()) ? std::span<const Identifier>(&*nameSearchedFor, 1) : std::span<const Identifier>();
        };
        UncheckedKeyHashMap<ModuleID, NamesInOrder> namesBehindStarsOfMocks;
        std::array<UncheckedKeyHashMap<JSObject*, NamesInOrder>, 2> namesOfObjects;
        auto namesAskedOf = [&](JSObject* object, DontEnumPropertiesMode mode) -> std::span<const Identifier> {
            auto& known = namesOfObjects[mode == DontEnumPropertiesMode::Include];
            auto found = known.find(object);
            if (found == known.end()) {
                PropertyNameArrayBuilder own(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                object->methodTable()->getOwnPropertyNames(object, realm, own, mode);
                RETURN_IF_EXCEPTION(scope, { });
                NamesInOrder names;
                for (const Identifier& name : own)
                    names.inOrder.append(name);
                found = known.add(object, WTF::move(names)).iterator;
            }
            return namesAskedAmong(found->value);
        };
        auto beginsSearchForNextName = [&] {
            namesPassedByWhileHidden.clear();
            nameSearchedFor = namesToSearchFor.isEmpty() ? std::nullopt : std::optional(namesToSearchFor.takeLast());
            if (!nameSearchedFor)
                return false;
            pending.shrink(0);
            pending.append({ module, 0 });
            seen.clearAll();
            seen.quickSet(module.index());
            return true;
        };
        auto isStillWalking = [&] {
            return !pending.isEmpty() && !(nameSearchedFor && listedNames.contains(nameSearchedFor->impl()));
        };
        auto takesBackNameSearchedFor = [&](ModuleID source) {
            auto taken = nameSearchedFor ? filesThatTakeNameBack.find(nameSearchedFor->impl()) : filesThatTakeNameBack.end();
            return taken != filesThatTakeNameBack.end() && taken->value.contains(source);
        };
        auto listAskersBut = [&](ModuleRecord* recordOfMock) {
            Vector<ModuleRecord*, 8> askers;
            auto appendAsker = [&](AbstractModuleRecord* asker) {
                auto* compiled = dynamicDowncast<ModuleRecord>(asker);
                if (compiled && compiled != recordOfMock && !askers.contains(compiled))
                    askers.append(compiled);
            };
            appendAsker(record);
            for (auto& onPath : pending) {
                appendAsker(recordForResolution(onPath.first));
                RETURN_IF_EXCEPTION(scope, askers);
            }
            for (auto& beingMade : m_namespacesBeingMade)
                appendAsker(m_records.get(beingMade.first));
            return askers;
        };
        auto findsNothingUnderMock = [&](ModuleID source, const ModuleExportData& listed) {
            if (!m_mocks || listed.kind() != ModuleExportKind::Indirect)
                return false;
            AbstractModuleRecord* recordOfSource = recordForResolution(source);
            RETURN_IF_EXCEPTION(scope, false);
            StarExportResolution found = recordOfSource->resolveExport(realm, identifier(listed.name));
            RETURN_IF_EXCEPTION(scope, false);
            bool isFound = found.type == StarExportResolution::Type::Resolved;
            if (auto* provider = isFound ? dynamicDowncast<ModuleRecord>(found.moduleRecord) : nullptr; provider && provider->isReplacedByMock()) {
                JSObject* mock = activeMock(provider->module());
                isFound = mock && mock->hasProperty(realm, found.localName);
                RETURN_IF_EXCEPTION(scope, false);
                if (isFound && isMockedByFactoryThatTakesOriginal(provider->module())) {
                    Vector<ModuleRecord*, 8> askers = listAskersBut(provider);
                    RETURN_IF_EXCEPTION(scope, false);
                    isFound = provider->originalShows(realm, found.localName, askers.span());
                    RETURN_IF_EXCEPTION(scope, false);
                }
            }
            if (!isFound)
                filesThatTakeBack(identifier(listed.name));
            return !isFound;
        };
        auto takesNameBackFromPath = [&](ModuleID source, const ModuleExportData& listed) {
            const ModuleExportData* taken = &listed;
            for (size_t steps = 0; steps <= m_table.size() && taken->kind() == ModuleExportKind::Indirect; ++steps) {
                const IndirectExportData& asked = m_table.indirectExport(*taken);
                if (std::ranges::contains(pending, asked.targetModule(), &std::pair<ModuleID, unsigned>::first)) {
                    if (identifier(asked.importName) != identifier(listed.name))
                        return false;
                    Vector<ModuleID, 2>& files = filesThatTakeBack(identifier(listed.name));
                    if (!listed.isFromStarExport)
                        files.appendIfNotContains(source);
                    return true;
                }
                std::optional<unsigned> next = m_table.findExport(asked.targetModule(), identifier(asked.importName).impl());
                if (!next)
                    return false;
                taken = &m_table.exports(asked.targetModule())[*next];
            }
            return false;
        };
        while (isStillWalking() || beginsSearchForNextName()) {
            ModuleID importer = pending.last().first;
            DynamicStars stars = m_table.dynamicStars(importer);
            if (pending.last().second == stars.numberOfPlaces()) {
                pending.removeLast();
                continue;
            }
            if (std::optional<ModuleRequestData> request = stars.at(pending.last().second++)) {
                ModuleID source = request->targetModule();
                if (source == module)
                    continue;
                bool wasSeen = seen.quickSet(source.index());
                if (wasSeen && !m_mocks && m_removedMocksInUse.isEmpty())
                    continue;
                const ModuleData& sourceData = data(source);
                JSObject* ended = endedMockSeenBy(source, importer);
                if (ended && !wasSeen)
                    seen.quickClear(source.index());
                if (ended) [[unlikely]] {
                    std::span<const Identifier> names = namesAskedOf(ended, DontEnumPropertiesMode::Exclude);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names, namespaceSeenBy(source, importer));
                } else if (activeMock(source) ? !seesMockOf(source, importer) : isMocked(fileOf(source)) && importsOriginalDespiteMock(source, importer)) [[unlikely]] {
                    auto beingMade = std::ranges::find(m_namespacesBeingMade, source, &std::pair<ModuleID, JSObject*>::first);
                    if (beingMade == m_namespacesBeingMade.end() && sourceData.capturesExports() && isCommonJSModuleOrFacade(source)) {
                        evaluatePastPendingMock(source);
                        RETURN_IF_EXCEPTION(scope, { });
                        captureExports(source);
                        RETURN_IF_EXCEPTION(scope, { });
                        runOriginalsOwedTo(importer);
                        RETURN_IF_EXCEPTION(scope, { });
                    }
                    JSObject* own = beingMade != m_namespacesBeingMade.end() ? beingMade->second : originalNamespace(source);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (beingMade != m_namespacesBeingMade.end())
                        m_outermostNamespaceReadWhileBeingMade = std::min<size_t>(m_outermostNamespaceReadWhileBeingMade, beingMade - m_namespacesBeingMade.begin());
                    else if (auto lacking = std::ranges::find(m_namespacesThatLackNames, own, &NamespaceThatLacksNames::object); lacking != m_namespacesThatLackNames.end())
                        m_outermostNamespaceReadWhileBeingMade = std::min(m_outermostNamespaceReadWhileBeingMade, lacking->outermostRead);
                    std::span<const Identifier> names = namesAskedOf(own, DontEnumPropertiesMode::Include);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names, own);
                } else if (sourceData.capturesExports()) {
                    Vector<Identifier, 8> names;
                    if (!nameSearchedFor) {
                        forEachCapturedExport(source, [&](const Identifier& name, JSValue) {
                            names.append(name);
                        });
                    } else if (hasCapturedExport(source, *nameSearchedFor))
                        names.append(*nameSearchedFor);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names);
                } else if (sourceData.isUncompiledCode()) {
                    JSObject* namespaceObject = uncompiledNamespace(source, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (auto* shown = dynamicDowncast<JSModuleNamespaceObject>(namespaceObject); shown && shown->moduleRecord()->hasLiveExports()) {
                        WTF::Vector<Identifier> names = namesOfRecord(*shown->moduleRecord());
                        if (nameSearchedFor)
                            names.removeAllMatching([&](const Identifier& name) { return name != *nameSearchedFor; });
                        appendLiveExports(source, names);
                        RETURN_IF_EXCEPTION(scope, { });
                        continue;
                    }
                    std::span<const Identifier> names = namesAskedOf(namespaceObject, DontEnumPropertiesMode::Include);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names);
                } else {
                    RELEASE_ASSERT(sourceData.hasExportTable());
                    std::span<const ModuleExportData> entriesOfSource = m_table.exports(source);
                    std::span<const ModuleExportData> entriesAsked = entriesOfSource;
                    if (nameSearchedFor) {
                        std::optional<unsigned> asked = m_table.findExport(source, nameSearchedFor->impl());
                        entriesAsked = asked ? entriesOfSource.subspan(*asked, 1) : entriesOfSource.first(0);
                    }
                    bool seesMock = activeMock(source);
                    appendLiveExports(source, entriesAsked | std::views::filter([&](const ModuleExportData& entry) {
                        return !isLeftOut(source, &entry - entriesOfSource.data(), TablesBehindStars::HaveName) && (seesMock || (!takesNameBackFromPath(source, entry) && !findsNothingUnderMock(source, entry)));
                    }) | std::views::transform([&](const ModuleExportData& entry) {
                        return identifier(entry.name);
                    }));
                    RETURN_IF_EXCEPTION(scope, { });
                    WTF::Vector<Identifier> addedByMock;
                    if (seesMock && isMockedByFactoryThatTakesOriginal(source)) {
                        auto* recordOfMock = dynamicDowncast<ModuleRecord>(recordForResolution(source));
                        Vector<ModuleRecord*, 8> askers = listAskersBut(recordOfMock);
                        RETURN_IF_EXCEPTION(scope, { });
                        auto originalShows = [&](const Identifier& name) {
                            return recordOfMock ? recordOfMock->originalShows(realm, name, askers.span()) : ensureDisplacedRecord(source)->resolveExport(realm, name).type == StarExportResolution::Type::Resolved;
                        };
                        for (const ModuleExportData& entry : entriesAsked) {
                            if (!isLeftOut(source, &entry - entriesOfSource.data(), TablesBehindStars::HaveName))
                                continue;
                            Identifier name = identifier(entry.name);
                            if (namesPassedByWhileHidden.contains(name.impl()) && isTakenBackByFileOnPath(name))
                                continue;
                            bool isLackedByOriginal = entry.isFromStarExport && !originalShows(name);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (isLackedByOriginal)
                                continue;
                            bool isExported = isExportedThroughStar(name);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (isExported)
                                result.append({ name, jsUndefined() });
                        }
                        auto behindStars = namesBehindStarsOfMocks.find(source);
                        if (behindStars == namesBehindStarsOfMocks.end()) {
                            NamesInOrder names;
                            appendNamesBehindStarsOf(source, names.inOrder);
                            RETURN_IF_EXCEPTION(scope, { });
                            behindStars = namesBehindStarsOfMocks.add(source, WTF::move(names)).iterator;
                        }
                        std::span<const Identifier> namesAskedBehindStars = namesAskedAmong(behindStars->value);
                        JSObject* lackedByTable = ensureLoaded(source).exportsAddedByMock;
                        for (const Identifier& name : namesAskedBehindStars) {
                            if (m_table.findExport(source, name.impl()))
                                continue;
                            bool isShownByOriginal = originalShows(name);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (!isShownByOriginal)
                                continue;
                            if (lackedByTable && storedValue(realm, lackedByTable, name)) {
                                addedByMock.append(name);
                                continue;
                            }
                            bool isExported = isExportedThroughStar(name);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (isExported)
                                result.append({ name, jsUndefined() });
                        }
                    } else if (seesMock)
                        appendNamesAddedByMock(source, addedByMock);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, addedByMock);
                    if (!wasSeen && !standsForWholeModule(source) && !takesBackNameSearchedFor(source))
                        pending.append({ source, 0 });
                }
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        for (const Identifier& name : namesTakenBack) {
            bool isExported = isExportedThroughStar(name);
            RETURN_IF_EXCEPTION(scope, { });
            if (isExported)
                result.append({ name, jsUndefined() });
        }

        BitVector& ambiguousExports = ensureLoaded(module).ambiguousExports;
        ambiguousExports.clearAll();
        ambiguousExports.merge(entriesThatYieldToStars);
        std::span<const ModuleExportData> exports = m_table.exports(module);
        for (unsigned index = 0; index < exports.size(); ++index) {
            if (!exports[index].isFromStarExport)
                continue;
            StarExportResolution resolution = record->resolveExport(realm, identifier(exports[index].name));
            RETURN_IF_EXCEPTION(scope, { });
            if (resolution.type == StarExportResolution::Type::Ambiguous)
                ambiguousExports.set(index);
        }
    }

    sortByName(result);
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
