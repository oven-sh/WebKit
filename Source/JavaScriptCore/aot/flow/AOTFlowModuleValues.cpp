/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowModules.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowCoroutines.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

static const ModuleExport* findExport(const Program& program, const Module& module, BuiltinName name)
{
    RELEASE_ASSERT(name);
    auto found = std::ranges::find(module.exports, program.stringOf(name), &ModuleExport::name);
    return found == module.exports.end() ? nullptr : found;
}

static bool hasExportTable(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowInstanceID enterBodyOf(FlowSolver& solver, const Module& module)
{
    return module.body ? solver.enter(module.body, FlowContext::Proven) : FlowInstanceID();
}

static FlowNodeID slotOrNativeCode(const FlowGraph& graph, FlowObjectID object, BuiltinSlotID slot)
{
    FlowNodeID node = graph.slot(object, slot);
    return node ? node : graph.nativeCode();
}

static void connectLocation(FlowSolver& solver, FlowNodeID location, FlowNodeID target, const Witness& witness)
{
    solver.connect(location, target, { }, witness);
    solver.noteLocationEvent(location, FlowLocationEvent::ReadAsValue);
}

static void connectExport(FlowSolver& solver, const ModuleExport& entry, FlowNodeID target, const Witness& witness)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        useModulesBehindVariable(solver, entry.variable, witness);
        connectLocation(solver, solver.graph().variable(entry.variable), target, witness);
        return;
    case ModuleExportKind::Namespace:
        connectNamespaceOf(solver, entry.module, entry.phase, target, witness);
        return;
    case ModuleExportKind::Indirect:
        solver.joinUnknown(target, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void connectRuntimeNamespace(FlowSolver& solver, RuntimeModuleID runtimeModule, FlowNodeID target, const Witness& witness)
{
    BuiltinID namespaceObject = solver.builtins().namespaceOf(runtimeModule);
    if (!namespaceObject) {
        solver.joinUnknownObject(target, witness);
        return;
    }
    solver.joinObject(target, ensureBuiltinObject(solver, namespaceObject), witness);
}

static void connectBodyResult(FlowSolver& solver, const Module& module, FlowNodeID target, const Witness& witness)
{
    FlowInstanceID instance = enterBodyOf(solver, module);
    if (!instance || !solver.graph().summaryOf(instance).numberOfResults) {
        solver.joinUnknown(target, witness);
        return;
    }
    solver.connect(solver.node(instance, solver.graph().summaryOf(instance).result(0)), target, { }, witness);
}

static FlowNodeID exportsFromGetterNode(FlowSolver& solver, ModuleID id)
{
    return solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::ExportsFromGetter, id.index()));
}

// A module may define an accessor for module.exports. The loader reads the property, so the getter runs and require() answers what it returns.
void didCreateGetterOfModuleObject(FlowSolver& solver, FlowObjectID moduleObject, PropertyKeyID key, FlowNodeID getter)
{
    if (key != solver.program().keyOf(BuiltinNames::exports))
        return;

    FlowCallRequest request {
        nativeCodeSite(solver),
        ValueID(),
        FlowCallLinkKind::Accessor,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .thisObject = moduleObject, .result = exportsFromGetterNode(solver, solver.objects().at(moduleObject).module), .thrown = solver.graph().nativeCode() },
    };
    callValuesOf(solver, request, getter);
}

static void connectModuleExportsValue(FlowSolver& solver, ModuleID id, FlowNodeID target, const Witness& witness)
{
    const Module& module = solver.program().at(id);
    switch (module.kind) {
    case ModuleKind::CommonJs:
        connectLocation(solver, moduleExportsNode(solver, id), target, witness);
        solver.connect(exportsFromGetterNode(solver, id), target, { }, witness);
        return;
    case ModuleKind::RuntimeSource:
        connectBodyResult(solver, module, target, witness);
        return;
    case ModuleKind::SourceText:
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        solver.joinUnknown(target, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void connectEvaluationErrors(FlowSolver& solver, ModuleID id, FlowNodeID thrown, const Witness& witness)
{
    const Module& module = solver.program().at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
        return;
    case ModuleKind::External:
        solver.joinUnknown(thrown, witness);
        return;
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    if (FlowInstanceID instance = enterBodyOf(solver, module))
        solver.connect(solver.node(instance, solver.graph().summaryOf(instance).thrown()), thrown, { }, witness);
    // What the body of a requested module throws reached native code when that body was entered.
    if (!module.requests.isEmpty())
        solver.joinUnknown(thrown, witness);
}

FlowNodeID moduleExportsNode(FlowSolver& solver, ModuleID module)
{
    FlowObjectID moduleObject = ensureModuleBuiltinObject(solver, Builtins::ModuleObject, module);
    return solver.ensurePropertyValue(moduleObject, solver.program().keyOf(BuiltinNames::exports));
}

void connectNamespaceOf(FlowSolver& solver, ModuleID id, FlowNodeID target, const Witness& witness)
{
    if (!target)
        return;

    const Module& module = solver.program().at(id);
    switch (module.kind) {
    case ModuleKind::External:
        solver.joinUnknownObject(target, witness);
        return;
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        solver.joinObject(target, ensureNamespaceObject(solver, id), witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void connectNamespaceOf(FlowSolver& solver, ModuleID id, ImportPhase phase, FlowNodeID target, const Witness& witness)
{
    switch (phase) {
    case ImportPhase::Evaluation:
        connectNamespaceOf(solver, id, target, witness);
        return;
    case ImportPhase::Defer:
        // The deferred namespace object is another object, without the namespace layout: a read of it may evaluate the module. It is not tracked, so what it serves escapes.
        connectNamespaceOf(solver, id, solver.graph().nativeCode(), witness);
        solver.joinUnknownObject(target, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void connectRequiredValueOf(FlowSolver& solver, ModuleID id, FlowNodeID target, const Witness& witness)
{
    if (!target)
        return;

    const Program& program = solver.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
        connectRuntimeNamespace(solver, module.runtimeModule, target, witness);
        return;
    case ModuleKind::External:
        solver.joinUnknown(target, witness);
        return;
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    switch (module.requireResult) {
    case RequireResult::ModuleExports:
        connectModuleExportsValue(solver, id, target, witness);
        return;
    case RequireResult::DefaultExport:
        connectDefaultExportOf(solver, id, target, witness);
        return;
    case RequireResult::Namespace:
        // The run time answers the namespace object when this export holds undefined or null.
        connectNamespaceOf(solver, id, target, witness);
        if (const ModuleExport* entry = findExport(program, module, program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()))))
            connectExport(solver, *entry, target, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void connectDefaultExportOf(FlowSolver& solver, ModuleID id, FlowNodeID target, const Witness& witness)
{
    const Program& program = solver.program();
    const Module& module = program.at(id);
    if (!hasExportTable(module.kind)) {
        connectModuleExportsValue(solver, id, target, witness);
        return;
    }

    if (const ModuleExport* entry = findExport(program, module, BuiltinNames::defaultKeyword))
        connectExport(solver, *entry, target, witness);
    else
        solver.joinPrimitives(target, Primitive::Undefined, witness);
}

static FlowNodeID answersOfReplacedLoaderNode(FlowSolver& solver)
{
    return solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::AnswersOfReplacedLoader));
}

// An entry that the program stores into the module cache, and a function that it puts in the place of the loader's, answer every later require, also one with a constant name, with any module or any value that has escaped.
void didReplaceLoaderAnswers(FlowSolver& solver, const Witness& witness)
{
    FlowNodeID answers = answersOfReplacedLoaderNode(solver);
    if (!solver.valueOf(answers).isEmpty())
        return;
    solver.joinUnknown(answers, witness);
    solver.connect(answersOfModulesLoadedByName(solver, FlowModuleAnswer::RequiredValue), answers, { }, witness);
    raiseUnseenCode(solver, UnseenCodeCause::LoadsModuleByName, Builtins::ModuleCache, witness);
}

bool holdsLoaderAnswers(BuiltinID object)
{
    return object == Builtins::ModuleCache || object == Builtins::ModuleRequireExtensions || object == Builtins::NodeModule_extensions;
}

static constexpr BuiltinID locationsOfModuleCache[] { Builtins::ModuleRequireCache, Builtins::NodeModule_cache };
static constexpr BuiltinID locationsOfLoaders[] {
    Builtins::ModuleObjectRequire,
    Builtins::NodeModulePrototypeRequire,
    Builtins::NodeModuleWrap,
    Builtins::NodeModuleWrapper,
    Builtins::NodeModule_load,
    Builtins::NodeModule_resolveFilename,
};

bool isLoaderLocation(BuiltinID location)
{
    return std::ranges::contains(locationsOfModuleCache, location) || std::ranges::contains(locationsOfLoaders, location);
}

void didUseLoaderLocation(FlowSolver& solver, BuiltinID location, OptionSet<BuiltinUse> added, const Witness& witness)
{
    // The cache hands out the module object of every module that has been evaluated.
    if (!added.containsOnly(BuiltinUse::Tested) && std::ranges::contains(locationsOfModuleCache, location))
        exposeEveryModuleObject(solver, witness);
    if (added.contains(BuiltinUse::Written) && isLoaderLocation(location))
        didReplaceLoaderAnswers(solver, witness);
}

void requireModule(FlowSolver& solver, const FlowModuleLoad& load, ModuleID module)
{
    Witness witness = solver.witnessAt(load.site, load.origin);
    didRequestModule(solver, load.site.instance, module);
    evaluateModule(solver, module, witness);
    connectRequiredValueOf(solver, module, load.result, witness);
    solver.connect(answersOfReplacedLoaderNode(solver), load.result);
    connectEvaluationErrors(solver, module, thrownNodeOrNativeCode(solver, load.thrown), witness);
}

void importModule(FlowSolver& solver, const FlowModuleLoad& load, std::span<const ModuleID> modules, bool mayLoadOtherModules)
{
    Witness witness = solver.witnessAt(load.site, load.origin);
    FlowObjectID promise = allocateAtCallSite(solver, {
        load.site,
        { .kind = FlowSiteKind::Instance, .origin = FlowSiteOrigin::BuiltinResult, .value = load.origin, .builtinClass = BuiltinClasses::Promise },
    });
    solver.joinObject(load.result, promise, witness);

    FlowNodeID value = slotOrNativeCode(solver.graph(), promise, BuiltinSlots::PromiseValue);
    FlowNodeID reason = slotOrNativeCode(solver.graph(), promise, BuiltinSlots::PromiseReason);
    FlowNodeID namespaces = solver.temporaryFor(callTemporaryKey(load.site, FlowCallTemporaryRole::ImportedNamespaces));
    for (ModuleID module : modules) {
        didRequestModule(solver, load.site.instance, module);
        evaluateModule(solver, module, witness);
        captureExportsForImporter(solver, module);
        // No native module exports `then`, and the walk cannot tell: the table does not list what such a namespace object holds.
        connectNamespaceOf(solver, module, solver.program().at(module).kind == ModuleKind::Runtime ? value : namespaces, witness);
        connectEvaluationErrors(solver, module, reason, witness);
    }
    // The promise is resolved with the namespace object, so an export named `then` is called.
    resolvePromiseWith(solver, load.site, load.origin, promise, namespaces);
    solver.joinObject(reason, runtimeErrorObject(solver), witness);
    // The host reports a rejection that no handler takes.
    solver.connect(reason, solver.graph().nativeCode(), { }, witness);

    if (mayLoadOtherModules)
        loadByUnknownName(solver, { load.site, load.origin, value, reason }, BuiltinID(), FlowModuleAnswer::Namespace);
}

void requireRuntimeModule(FlowSolver& solver, const FlowModuleLoad& load, RuntimeModuleID runtimeModule)
{
    if (ModuleID module = moduleOf(solver.callState(), runtimeModule)) {
        requireModule(solver, load, module);
        return;
    }

    Witness witness = solver.witnessAt(load.site, load.origin);
    if (BuiltinID namespaceObject = solver.builtins().namespaceOf(runtimeModule))
        noteBuiltinUse(solver, namespaceObject, BuiltinUse::Read, witness);
    connectRuntimeNamespace(solver, runtimeModule, load.result, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
