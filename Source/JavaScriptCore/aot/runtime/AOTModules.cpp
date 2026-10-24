/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleStarExports.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DisallowVMEntry.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSModuleLoader.h"
#include "ObjectConstructor.h"
#include <wtf/ASCIICType.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Modules);

static bool isAbsoluteFileSystemPath(StringView path)
{
#if OS(WINDOWS)
    auto isSeparator = [](char16_t character) {
        return character == '/' || character == '\\';
    };
    if (path.isEmpty())
        return false;
    if (isSeparator(path[0]))
        return true;
    return path.length() >= 3 && isASCIIAlpha(path[0]) && path[1] == ':' && isSeparator(path[2]);
#else
    return path.startsWith('/');
#endif
}

static AbstractModuleRecord::ModulePhase targetPhaseOf(const ModuleExportData& moduleExport)
{
    return moduleExport.isDeferred ? AbstractModuleRecord::ModulePhase::Defer : AbstractModuleRecord::ModulePhase::Evaluation;
}

static StoredModuleStatus storedStatusFor(ModuleStatus status)
{
    switch (status) {
    case ModuleStatus::Unlinked:
        return StoredModuleStatus::Unlinked;
    case ModuleStatus::Linked:
        return StoredModuleStatus::Linked;
    case ModuleStatus::Evaluating:
        return StoredModuleStatus::Evaluating;
    case ModuleStatus::Evaluated:
        return StoredModuleStatus::Evaluated;
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CyclicModuleRecord::Status recordStatusFor(ModuleStatus status)
{
    switch (status) {
    case ModuleStatus::Unlinked:
        return CyclicModuleRecord::Status::Unlinked;
    case ModuleStatus::Linking:
        return CyclicModuleRecord::Status::Linking;
    case ModuleStatus::Linked:
        return CyclicModuleRecord::Status::Linked;
    case ModuleStatus::Evaluating:
        return CyclicModuleRecord::Status::Evaluating;
    case ModuleStatus::EvaluatingAsync:
        return CyclicModuleRecord::Status::EvaluatingAsync;
    case ModuleStatus::Evaluated:
        return CyclicModuleRecord::Status::Evaluated;
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ModuleStatus statusOf(const ModuleRecord& record)
{
    switch (record.status()) {
    case CyclicModuleRecord::Status::New:
    case CyclicModuleRecord::Status::Unlinked:
        return ModuleStatus::Unlinked;
    case CyclicModuleRecord::Status::Linking:
        return ModuleStatus::Linking;
    case CyclicModuleRecord::Status::Linked:
        return ModuleStatus::Linked;
    case CyclicModuleRecord::Status::Evaluating:
        return ModuleStatus::Evaluating;
    case CyclicModuleRecord::Status::EvaluatingAsync:
        return ModuleStatus::EvaluatingAsync;
    case CyclicModuleRecord::Status::Evaluated:
        return record.evaluationError() ? ModuleStatus::Failed : ModuleStatus::Evaluated;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CyclicModuleRecord::Status initialRecordStatus(ModuleStatus status, const ModuleData& moduleData)
{
    if (status == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode())
        return CyclicModuleRecord::Status::New;
    return recordStatusFor(status);
}

template<typename Visitor>
void Modules::visitRoots(Visitor& visitor)
{
    m_storedReferences.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (ModuleRecord* record : m_records.values())
            visitor.appendUnbarriered(record);
        for (auto& loaded : m_loaded.values()) {
            visitor.appendUnbarriered(loaded->namespaceObject.get());
            visitor.appendUnbarriered(loaded->deferredNamespaceObject.get());
            visitor.appendUnbarriered(loaded->importMeta.get());
            visitor.appendUnbarriered(loaded->requirer.get());
            visitor.appendUnbarriered(loaded->requireFunction.get());
            visitor.appendUnbarriered(loaded->requiredValue.get());
            visitor.appendUnbarriered(loaded->exportsWhileRequired.get());
            visitor.appendUnbarriered(loaded->capturedExports.get());
            visitor.appendUnbarriered(loaded->deferredExportsSource.get());
            visitor.appendUnbarriered(loaded->capturedFields.get());
            visitor.appendUnbarriered(loaded->capturedStars.get());
            visitor.appendUnbarriered(loaded->sourceOfCapturedFields.get());
            visitor.appendUnbarriered(loaded->uncompiledRecord.get());
        }
        visitor.appendUnbarriered(m_namespaceStructure);
    });
}

template void Modules::visitRoots(SlotVisitor&);
template void Modules::visitRoots(AbstractSlotVisitor&);

Modules::Modules(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_table(state.tables)
    , m_admitsCodeOutsideImage(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector))
    , m_admitsInspector(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector))
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    state.caches->add(CacheKind::StrongRoots, "modules"_s, *this, RootVolatility::GreyedByExecution);
}

Modules::~Modules() = default;

Modules& modulesOf(Instance& instance)
{
    return *instance.state->modules;
}

Modules* modulesOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance ? instance->state->modules.get() : nullptr;
}

const ModuleHost& Modules::host() const
{
    return moduleHostOf(m_instance.globalObject);
}

Identifier Modules::identifier(uint32_t number) const
{
    return m_instance.vm->aotState().identifiers->identifier(number);
}

JSModuleLoader* Modules::loader() const
{
    return m_instance.globalObject->moduleLoader();
}

ModuleStatus Modules::statusInRecord(ModuleID module) const
{
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record);
    return statusOf(*record);
}

void Modules::setStatus(ModuleID module, ModuleStatus status)
{
    ModuleRecord* record = recordIfExists(module);
    if (!record) {
        setStoredStatus(module, storedStatusFor(status));
        return;
    }
    record->setStatus(recordStatusFor(status));
    if (status == ModuleStatus::Evaluated && !record->cycleRoot())
        record->setCycleRoot(*m_instance.vm, record);
}

ModuleRecord* Modules::ensureRecord(ModuleID module)
{
    // While the body of a module with a record runs, the table has the status.
    if (ModuleRecord* record = m_records.get(module))
        return record;

    VM& vm = *m_instance.vm;
    const ModuleData& moduleData = data(module);
    ModuleRecord* record = ModuleRecord::create(m_instance, vm.aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->setHasTLA(moduleData.hasTopLevelAwait());
    record->setStatus(initialRecordStatus(status(module), moduleData));
    if (record->status() == CyclicModuleRecord::Status::Evaluated)
        record->setCycleRoot(vm, record);
    {
        Locker locker { m_state.caches->lock() };
        m_records.add(module, record);
        m_storedReferences.recordAll();
    }
    setStoredStatus(module, StoredModuleStatus::InRecord);
    return record;
}

// A record has a status of its own, which JavaScriptCore's loader reads.
void Modules::didLinkEntryPoint()
{
    m_entryPointIsLinked = true;
    for (ModuleRecord* record : m_records.values()) {
        if (data(record->module()).isLinkedWithEntryPoint() && statusOf(*record) == ModuleStatus::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
    }
}

void Modules::retireRecord(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(record->status() == CyclicModuleRecord::Status::Evaluated);
    ASSERT(!record->evaluationError());
    ASSERT(record->asyncEvaluationOrder().isUnset());
    ModuleID module = record->module();
    setStoredStatus(module, StoredModuleStatus::Evaluated);
    Locker locker { m_state.caches->lock() };
    m_records.remove(module);
}

void Modules::setFailed(ModuleID module, JSValue error)
{
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    record->setEvaluationError(*m_instance.vm, error);
    record->setStatus(CyclicModuleRecord::Status::Evaluated);
}

void Modules::throwEvaluationError(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record && record->evaluationError());
    if (JSPromise* capability = record->topLevelCapability())
        capability->markAsHandled();
    throwException(m_instance.globalObject, scope, record->evaluationError());
}

JSValue Modules::value(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    return moduleData.hasValue() ? ProgramStatics::readVariableWithoutAllocating(m_instance, moduleData.value) : JSValue();
}

void Modules::setValue(ModuleID module, JSValue value)
{
    RELEASE_ASSERT(data(module).hasValue());
    ProgramStatics::writeVariable(m_instance, data(module).value, value);
}

auto Modules::loadedIfExists(ModuleID module) const -> LoadedModule*
{
    return m_loaded.get(module);
}

auto Modules::ensureLoaded(ModuleID module) -> LoadedModule&
{
    if (LoadedModule* loaded = loadedIfExists(module))
        return *loaded;
    RELEASE_ASSERT(module.index() < m_table.size());
    auto created = makeUnique<LoadedModule>();
    LoadedModule& loaded = *created;
    Locker locker { m_state.caches->lock() };
    m_loaded.add(module, WTF::move(created));
    return loaded;
}

Identifier Modules::moduleKey(ModuleID module)
{
    return Identifier::fromString(*m_instance.vm, m_table.pathText(module));
}

JSString* Modules::pathString(ModuleID module)
{
    if (data(module).directory == Number24::max)
        return m_state.strings->string(*m_instance.vm, StringID(data(module).publicPathOrName));
    return jsString(*m_instance.vm, m_table.pathText(module));
}

void Modules::throwExportNotFound(ModuleID exporter, uint32_t nameIdentifier)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& name = identifier(nameIdentifier);
    if (name == vm.propertyNames->defaultKeyword) {
        throwException(globalObject, scope, createMissingDefaultExportError(globalObject, m_table.pathText(exporter)));
        return;
    }
    throwException(globalObject, scope, createExportNotFoundError(globalObject, name.string(), m_table.pathText(exporter)));
}

void Modules::throwExportAmbiguous(ModuleID exporter, uint32_t nameIdentifier)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(m_instance.globalObject, scope, createAmbiguousExportError(m_instance.globalObject, identifier(nameIdentifier).string(), m_table.pathText(exporter)));
}

JSPromise* Modules::rejectedPromiseWithPendingException(JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (JSPromise* promise = JSPromise::rejectedPromiseWithCaughtException(globalObject, scope))
        return promise;
    // A termination cannot be caught: it stays pending, and the promise is never settled.
    return JSPromise::create(globalObject->vm(), globalObject->promiseStructure());
}

JSValue Modules::readExportIfInitialized(ModuleID module, unsigned index) const
{
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable);
    case ModuleExportKind::Namespace: {
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
        return namespaceObject ? JSValue(namespaceObject) : JSValue();
    }
    case ModuleExportKind::Indirect:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Modules::readExport(ModuleID module, unsigned index)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    // The caches answer for the namespace object of an evaluated module only.
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        JSValue value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(vm);
            const Identifier& name = identifier(moduleExport.name);
            throwException(globalObject, scope, createTDZError(globalObject, StringView(name.string())));
        }
        return value;
    }
    case ModuleExportKind::Namespace:
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        JSObject* namespaceObject = moduleNamespace(indirectExport.targetModule(), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSObject* Modules::capturedExports(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && !loaded->capturedExports && loaded->capturedFields)
        const_cast<Modules*>(this)->makeTableOfCapturedFields(*loaded);
    RELEASE_ASSERT(loaded && loaded->capturedExports);
    return loaded->capturedExports;
}

JSValue Modules::capturedValue(ModuleID module, const Identifier& name)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* captured = loaded.capturedExports;
    if (!captured && loaded.capturedFields)
        return capturedField(loaded, name.impl());
    RELEASE_ASSERT(captured);
    JSValue value = StarExportResolver::storedValue(realm, captured, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value != JSValue(captured))
        return value;
    value = loaded.deferredExportsSource->get(realm, name);
    RETURN_IF_EXCEPTION(scope, { });
    // The getter may have come back here and filled the export. A variable filled in the meantime holds that value.
    JSValue filledMeanwhile = StarExportResolver::storedValue(realm, captured, name);
    if (filledMeanwhile != JSValue(captured))
        return filledMeanwhile;
    captured->putDirectMayBeIndex(realm, name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

bool Modules::hasCapturedExport(ModuleID module, const Identifier& name) const
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && !loaded->capturedExports && loaded->capturedFields)
        return !!capturedField(*loaded, name.impl());
    return !!StarExportResolver::storedValue(m_instance.globalObject, capturedExports(module), name);
}

bool Modules::isExportAmbiguous(ModuleID module, unsigned index) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->ambiguousExports.get(index);
}

Structure* Modules::namespaceStructure()
{
    if (!m_namespaceStructure) {
        JSGlobalObject* globalObject = m_instance.globalObject;
        m_namespaceStructure = ModuleNamespace::createStructure(*m_instance.vm, globalObject, globalObject->moduleNamespaceObjectStructure()->storedPrototype());
        m_storedReferences.recordAll();
    }
    return m_namespaceStructure;
}

// Its names are added one after the other and no code of the program sees it, so it has one Structure of its own in place of one for each name.
JSObject* Modules::createTableOfExports()
{
    VM& vm = *m_instance.vm;
    JSObject* table = constructEmptyObject(vm, m_instance.globalObject->nullPrototypeObjectStructure());
    table->convertToDictionary(vm);
    return table;
}

// Compiled code reads the word of a module that has one.
JSObject* Modules::namespaceIfCreated(ModuleID module, ModulePhase phase) const
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue()) {
        JSValue held = value(module);
        return held ? asObject(held) : nullptr;
    }
    LoadedModule* loaded = loadedIfExists(module);
    return loaded ? loaded->namespaceObjectFor(phase).get() : nullptr;
}

void Modules::setNamespace(ModuleID module, ModulePhase phase, JSObject* namespaceObject)
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue()) {
        setValue(module, namespaceObject);
        return;
    }
    ensureLoaded(module).namespaceObjectFor(phase).set(*this, namespaceObject);
}

JSObject* Modules::createNamespace(ModuleID module, ModulePhase phase)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.isUncompiledCode()) {
        JSObject* loaderNamespace = uncompiledNamespace(module, phase);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setNamespace(module, phase, loaderNamespace);
        return loaderNamespace;
    }

    bool ranCode = false;
    if (status(module) == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    // Whoever wants the object wants the names now, and the names are those of what the module exports: it is evaluated first.
    if ((moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) && !didCaptureExports(module)) {
        captureExports(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    if (ranCode) {
        if (JSObject* createdMeanwhile = namespaceIfCreated(module, phase))
            return createdMeanwhile;
    }

    Vector<DynamicExport> dynamicExports = collectDynamicExports(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleNamespace::DynamicExports exportObjects;
    if (!dynamicExports.isEmpty()) {
        exportObjects.capturedValues = createTableOfExports();
        exportObjects.names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, dynamicExports.size());
        for (unsigned i = 0; i < dynamicExports.size(); ++i) {
            const DynamicExport& dynamicExport = dynamicExports[i];
            if (dynamicExport.liveSource) {
                if (!exportObjects.liveSources)
                    exportObjects.liveSources = createTableOfExports();
                exportObjects.liveSources->putDirectMayBeIndex(globalObject, dynamicExport.name, dynamicExport.liveSource);
            } else if (dynamicExport.capturedValue)
                exportObjects.capturedValues->putDirectMayBeIndex(globalObject, dynamicExport.name, dynamicExport.capturedValue);
            RETURN_IF_EXCEPTION(scope, nullptr);
            JSString* text = jsString(vm, dynamicExport.name.string());
            text->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            exportObjects.names->setIndex(vm, i, text);
        }
    }

    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, namespaceStructure(), module, phase, exportObjects);
    if (phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index()) && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    setNamespace(module, phase, namespaceObject);
    return namespaceObject;
}

JSObject* Modules::moduleNamespace(ModuleID module, ModulePhase phase)
{
    if (JSObject* namespaceObject = namespaceIfCreated(module, phase))
        return namespaceObject;
    return createNamespace(module, phase);
}

JSObject* Modules::importMeta(ModuleID module)
{
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.importMeta)
        return loaded.importMeta;
    JSObject* meta = loader()->createImportMetaProperties(globalObject, pathString(module), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_ASSERT(meta);
    loaded.importMeta.set(*this, meta);
    return meta;
}

JSCell* Modules::builtinCell(BuiltinID builtin, ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const Builtin& entry = BuiltinTable::singleton().at(builtin);
    RELEASE_ASSERT(entry.scope == BuiltinScope::Module);

    if (!entry.holder) {
        if (builtin == Builtins::ModuleObject)
            RELEASE_AND_RETURN(scope, moduleObject(module));
        if (builtin == Builtins::ImportMeta)
            RELEASE_AND_RETURN(scope, importMeta(module));
        RELEASE_ASSERT(builtin == Builtins::ModuleRequire);
        JSValue function = requireFunction(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_ASSERT(function.isCell());
        return function.asCell();
    }

    JSCell* holder = builtinCell(entry.holder, module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!holder || !holder->isObject())
        return nullptr;
    RELEASE_AND_RETURN(scope, m_state.builtinCells->readMember(m_instance.globalObject, asObject(holder), builtin));
}

JSString* Modules::resolve(ModuleID module, ResolvedSpecifierForm form)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    bool isURL = form == ResolvedSpecifierForm::URL;
    switch (moduleData.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return isURL ? jsString(vm, URL::fileURLWithFileSystemPath(m_table.pathText(module)).string()) : pathString(module);
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return pathString(module);
    case ModuleKind::External: {
        JSString* specifier = pathString(module);
        Identifier key;
        {
            DisallowVMEntry disallowVMEntry(vm);
            key = loader()->resolve(globalObject, specifier, pathString(*moduleData.referrerModule()), nullptr, true);
        }
        if (scope.exception()) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, specifier);
            return specifier;
        }
        if (isURL && isAbsoluteFileSystemPath(key.string()))
            return jsString(vm, URL::fileURLWithFileSystemPath(key.string()).string());
        return jsString(vm, key.string());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const SourceOrigin& Modules::sourceOrigin(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.sourceOrigin)
        return *loaded.sourceOrigin;
    switch (data(module).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(URL::fileURLWithFileSystemPath(m_table.pathText(module)));
        break;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(URL(m_table.pathText(module)));
        break;
    }
    return *loaded.sourceOrigin;
}

void Modules::throwFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    StringID specifier = failure.specifierString();
    ModuleID importer = failure.importerModule();
    if (failure.isAbsentExport()) {
        throwException(globalObject, scope, createExportNotFoundError(globalObject, ProgramStrings::text(m_table.programTables(), specifier), m_table.pathText(importer)));
        return;
    }
    {
        DisallowVMEntry disallowVMEntry(vm);
        loader()->resolve(globalObject, m_state.strings->string(vm, specifier), pathString(importer), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    throwException(globalObject, scope, createModuleMissingAtBuildError(globalObject, ProgramStrings::text(m_table.programTables(), specifier), m_table.pathText(importer), isForRequire ? "MODULE_NOT_FOUND"_s : "ERR_MODULE_NOT_FOUND"_s));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
