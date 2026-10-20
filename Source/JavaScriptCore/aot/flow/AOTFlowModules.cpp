/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

Vector<ModuleID> indexRuntimeModules(const Program& program)
{
    Vector<ModuleID> index(program.builtins().moduleCount());
    for (uint32_t module = 0; module < program.modules.size(); ++module) {
        if (RuntimeModuleID runtimeModule = program.modules[module].runtimeModule)
            index[runtimeModule.index()] = ModuleID(module);
    }
    return index;
}

ModuleID moduleOf(const FlowCallState& state, RuntimeModuleID id)
{
    const Vector<ModuleID>& index = state.modules.moduleOfRuntimeModule;
    return id.index() < index.size() ? index[id.index()] : ModuleID();
}

static Witness importWitnessOf(const Program& program, const Module& external, const Witness& witness)
{
    if (hasFunction(witness) || !external.referrer)
        return witness;
    FunctionID importer = program.at(external.referrer).body;
    if (!importer)
        return witness;
    return { importer.index(), Wire::none, program.at(importer).declarationLocation() };
}

static void evaluateExternalModule(FlowSolver& solver, const Module& module, const Witness& witness)
{
    switch (module.externalKind) {
    case ExternalKind::UncompiledCode:
        raiseUnseenCode(solver, UnseenCodeCause::ImportsUncompiledModule, BuiltinID(), importWitnessOf(solver.program(), module, witness));
        return;
    case ExternalKind::HostValue:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void exposeModuleObject(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    if (solver.program().at(id).kind == ModuleKind::CommonJs)
        solver.joinObject(solver.graph().nativeCode(), ensureModuleBuiltinObject(solver, Builtins::ModuleObject, id), witness);
}

RuntimeModuleID findCompiledModuleWithObject(const Program& program, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (entry.holder || !isObjectKind(entry.kind) || entry.origin != BuiltinOrigin::Host)
        return { };
    for (const Module& module : program.modules) {
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        BuiltinID object = builtins.namespaceOf(module.runtimeModule);
        if (object && builtins.canonicalID(object) == builtin)
            return module.runtimeModule;
    }
    return { };
}

FlowNodeID answersOfModulesLoadedByName(FlowSolver& solver, FlowModuleAnswer answer)
{
    return solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::ModulesLoadedByName, static_cast<uint32_t>(answer)));
}

static void exposeModule(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    connectRequiredValueOf(solver, id, answersOfModulesLoadedByName(solver, FlowModuleAnswer::RequiredValue), witness);
    connectNamespaceOf(solver, id, answersOfModulesLoadedByName(solver, FlowModuleAnswer::Namespace), witness);
    if (solver.callState().modules.everyModuleObjectIsExposed)
        exposeModuleObject(solver, id, witness);
}

static void openLibraryModuleToUnseenCode(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    if (solver.program().at(id).kind != ModuleKind::RuntimeSource)
        return;
    FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();
    connectRequiredValueOf(solver, id, unknownJavaScript, witness);
    connectNamespaceOf(solver, id, unknownJavaScript, witness);
}

void openLibraryModulesToUnseenCode(FlowSolver& solver, const Witness& witness)
{
    if (std::exchange(solver.callState().modules.libraryModulesAreOpenToUnseenCode, true))
        return;
    // The module that requires it is its `module.parent`, and its own `require.cache` lists the module object of every CommonJS module that has run.
    solver.connect(commonJSModuleObjectsNode(solver), solver.graph().unknownJavaScript(), { }, witness);
    BitVector evaluatedModules = solver.records().evaluatedModules();
    for (size_t index : evaluatedModules)
        openLibraryModuleToUnseenCode(solver, ModuleID(static_cast<uint32_t>(index)), witness);
}

void evaluateModule(FlowSolver& solver, ModuleID module, const Witness& witness)
{
    FlowModuleState& state = solver.callState().modules;
    state.modulesToEvaluate.append(module);
    if (state.isEvaluatingModules)
        return;

    state.isEvaluatingModules = true;
    while (!state.modulesToEvaluate.isEmpty())
        evaluateOneModule(solver, state.modulesToEvaluate.takeLast(), witness);
    state.isEvaluatingModules = false;
}

void fillExportsOfLoader(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    for (const SyntheticExport& entry : solver.program().at(id).syntheticExports) {
        if (!entry.variable)
            continue;
        FlowNodeID variable = solver.graph().variable(entry.variable);
        solver.joinUnknown(variable, witness);
        solver.noteLocationEvent(variable, FlowLocationEvent::WrittenByUnknownCode);
        keepLowerWitness(solver.records().ensureLocationRecord(variable).firstLoss, witness);
    }
}

// When an ES module imports the module, the loader reads every own enumerable property of its value, whichever names the importer uses, so every getter runs.
static void captureExportsOf(FlowSolver& solver, ModuleID id)
{
    FlowStatementSite site = nativeCodeSite(solver);
    FlowNodeID value = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::CapturedExports, id.index()));
    connectRequiredValueOf(solver, id, value, solver.witnessAt(site, ValueID()));
    runProtocolsInOwnStatement(solver, { site, ValueID(), value, { Protocol::OwnKeys, Protocol::OwnValues }, { }, { }, solver.graph().nativeCode() });
}

void captureExportsForImporter(FlowSolver& solver, ModuleID imported)
{
    FlowModuleState& state = solver.callState().modules;
    if (solver.program().at(imported).exportRule == SyntheticExportRule::None || state.hasCapturedExports.set(imported.index()))
        return;
    captureExportsOf(solver, imported);
}

static bool awaitsUse(const Program& program, ModuleID module)
{
    return program.at(module).flags.contains(ModuleFlags::HasNoSideEffects);
}

void useModule(FlowSolver& solver, ModuleID module, const Witness& witness)
{
    if (!solver.records().evaluatedModules().get(module.index()))
        evaluateModule(solver, module, witness);
}

static const HashMap<VariableID, Vector<ModuleID, 1>>& modulesExportingAgain(FlowSolver& solver)
{
    auto& index = solver.callState().modules.modulesExportingAgain;
    if (index)
        return *index;

    index.emplace();
    const Program& program = solver.program();
    for (uint32_t module = 0; module < program.modules.size(); ++module) {
        if (!awaitsUse(program, ModuleID(module)))
            continue;
        for (const ModuleExport& entry : program.modules[module].exports) {
            if (entry.kind == ModuleExportKind::Variable && program.at(entry.variable).module != ModuleID(module))
                index->add(entry.variable, Vector<ModuleID, 1>()).iterator->value.append(ModuleID(module));
        }
    }
    return *index;
}

// The importer may have the name from a module that exports it again. Such a module is used with the name, so what it imports for its side effects is evaluated.
void useModulesBehindVariable(FlowSolver& solver, VariableID variable, const Witness& witness)
{
    useModule(solver, solver.program().at(variable).module, witness);
    const auto& index = modulesExportingAgain(solver);
    auto found = index.find(variable);
    if (found == index.end())
        return;
    for (ModuleID module : found->value)
        useModule(solver, module, witness);
}

void useModuleBehindExport(FlowSolver& solver, const ModuleExport& entry, const Witness& witness)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        useModulesBehindVariable(solver, entry.variable, witness);
        return;
    case ModuleExportKind::Namespace:
        return;
    case ModuleExportKind::Indirect:
        useModule(solver, entry.module, witness);
        captureExportsForImporter(solver, entry.module);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Code that nobody follows can read every export of a namespace object that it holds, whichever module the variable is in.
void useModulesBehindNamespace(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    useModule(solver, id, witness);
    captureExportsForImporter(solver, id);
    for (const ModuleExport& entry : solver.program().at(id).exports)
        useModuleBehindExport(solver, entry, witness);
}

void evaluateOneModule(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    if (!solver.records().addEvaluatedModule(id))
        return;

    FlowModuleState& state = solver.callState().modules;
    const Module& module = solver.program().at(id);
    if (module.runtimeModule) {
        if (BuiltinID namespaceObject = solver.builtins().namespaceOf(module.runtimeModule))
            noteBuiltinUse(solver, namespaceObject, BuiltinUse::Read, witness);
    }
    FlowInstanceID bodyInstance = module.body ? solver.enter(module.body, FlowContext::Proven) : FlowInstanceID();
    for (const ModuleRequest& request : module.requests) {
        // The names of a dynamic star export are those that the evaluated module has.
        if (awaitsUse(solver.program(), request.target) && !request.isDynamicStarExport)
            continue;
        state.modulesToEvaluate.append(request.target);
        captureExportsForImporter(solver, request.target);
        if (bodyInstance)
            didRequestModule(solver, bodyInstance, request.target);
    }

    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        enterModuleBody(solver, id, witness);
        break;
    case ModuleKind::CommonJs:
        prepareCommonJSObjects(solver, id, witness);
        enterModuleBody(solver, id, witness);
        break;
    case ModuleKind::Runtime:
        break;
    case ModuleKind::External:
        evaluateExternalModule(solver, module, witness);
        break;
    }

    if (!module.body)
        fillExportsOfLoader(solver, id, witness);
    if (state.libraryModulesAreOpenToUnseenCode)
        openLibraryModuleToUnseenCode(solver, id, witness);
    if (state.everyModuleIsExposed) {
        didRequestModuleOfProgram(solver, id);
        exposeModule(solver, id, witness);
    }
    else if (state.everyModuleObjectIsExposed)
        exposeModuleObject(solver, id, witness);
}

static void passCommonJSArguments(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, ModuleID id)
{
    uint32_t firstParameter = solver.program().at(call.function).firstParameterIndex();
    auto indexOf = [&](CommonJsParameter parameter) {
        return firstParameter + static_cast<uint32_t>(parameter);
    };
    auto passObject = [&](CommonJsParameter parameter, BuiltinID builtin) {
        passOperand(solver, call, linked, { { }, ensureModuleBuiltinObject(solver, builtin, id), indexOf(parameter) });
    };

    passObject(CommonJsParameter::Exports, Builtins::ModuleObjectExports);
    passObject(CommonJsParameter::Require, Builtins::ModuleRequire);
    passObject(CommonJsParameter::Module, Builtins::ModuleObject);
    passPrimitives(solver, linked, indexOf(CommonJsParameter::Filename), Primitive::String);
    passPrimitives(solver, linked, indexOf(CommonJsParameter::Dirname), Primitive::String);
}

void enterModuleBody(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    const Module& module = solver.program().at(id);
    if (!module.body)
        return;
    FlowInstanceID instance = solver.enter(module.body, FlowContext::Proven);
    if (!instance)
        return;

    bool isCommonJS = module.kind == ModuleKind::CommonJs;
    const FlowSummary& summary = solver.graph().summaryOf(instance);
    uint32_t numberOfArguments = solver.program().at(module.body).numberOfPositionalParameters();

    FlowCallRequest request { nativeCodeSite(solver), ValueID(), FlowCallLinkKind::Host, BuiltinCallbackTimes::Once, false, { } };
    if (isCommonJS)
        request.operands.thisObject = ensureModuleBuiltinObject(solver, Builtins::ModuleObjectExports, id);
    FlowPassedArguments arguments;
    arguments.count = Range::one(numberOfArguments);
    FlowFunctionCall call { request, arguments, module.body, FlowObjectID(), FlowContext::Proven, FlowInstancePolicy::BySummary };
    FlowLinkedCall linked { ensureCallLink(solver, request, instance), instance, hasTrackedOperands(solver.graph().at(instance).context) };

    passThisAndNewTarget(solver, call, linked);
    if (isCommonJS)
        passCommonJSArguments(solver, call, linked, id);
    if (summary.restSite)
        passRestArguments(solver, call, linked, numberOfArguments);

    connectThrown(solver, solver.node(instance, summary.thrown()), request.operands.thrown, witness);
    if (summary.coroutine.resultSite)
        solver.joinObject(solver.graph().nativeCode(), linkCoroutineResult(solver, request, instance), witness);

    // The body can hand its callee to any code (`arguments.callee`), which calls it with arguments of its own.
    if (solver.program().at(module.body).calleeParameterIndex())
        enterFromNativeCode(solver, module.body, FlowObjectID(), witness);
}

FlowNodeID commonJSModuleObjectsNode(FlowSolver& solver)
{
    return solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::ModuleObjects));
}

void prepareCommonJSObjects(FlowSolver& solver, ModuleID id, const Witness& witness)
{
    FlowObjectID exports = ensureModuleBuiltinObject(solver, Builtins::ModuleObjectExports, id);
    solver.joinObject(moduleExportsNode(solver, id), exports, witness);
    solver.joinObject(commonJSModuleObjectsNode(solver), ensureModuleBuiltinObject(solver, Builtins::ModuleObject, id), witness);
}

void runModuleOperation(FlowSolver& solver, FlowStatementSite site, const FlowModuleOperation& operation)
{
    FlowModuleLoad load { site, operation.origin, solver.node(site, operation.result), solver.node(site, operation.thrown) };

    switch (operation.kind) {
    case FlowModuleOperationKind::Namespace: {
        Witness witness = solver.witnessAt(site, operation.origin);
        didRequestModule(solver, site.instance, operation.module);
        evaluateModule(solver, operation.module, witness);
        connectNamespaceOf(solver, operation.module, solver.operationAt(site, operation.origin).phase(), load.result, witness);
        return;
    }
    case FlowModuleOperationKind::Import:
        importModule(solver, load, singleElementSpan(operation.module), false);
        return;
    case FlowModuleOperationKind::Require:
        requireModule(solver, load, operation.module);
        return;
    case FlowModuleOperationKind::ImportDynamic:
        importModule(solver, load, { }, true);
        return;
    case FlowModuleOperationKind::RequireDynamic:
        loadByUnknownName(solver, load, BuiltinID(), FlowModuleAnswer::RequiredValue);
        return;
    case FlowModuleOperationKind::RequireBuiltinModule:
        requireRuntimeModule(solver, load, operation.runtimeModule);
        return;
    case FlowModuleOperationKind::ResolveDynamic:
        noteBuiltinUse(solver, Builtins::ModuleRequireResolve, BuiltinUse::Called, solver.witnessAt(site, operation.origin));
        return;
    case FlowModuleOperationKind::Use: {
        Witness witness = solver.witnessAt(site, operation.origin);
        useModule(solver, operation.module, witness);
        if (!solver.graph().isHost(site.instance) && operation.module == solver.program().at(solver.graph().at(site.instance).function).module)
            return;
        // The variables that another module names are those that the loader fills when it links the importer.
        captureExportsForImporter(solver, operation.module);
        for (const FlowExternalNode& external : solver.graph().summaryOf(site.instance).externals) {
            if (external.kind() == FlowExternalKind::Variable && solver.program().at(external.variable()).module == operation.module)
                useModulesBehindVariable(solver, external.variable(), witness);
        }
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What a parser makes of the text of a file: a string, or objects and arrays that hold no function.
static void joinContentsOfDataFile(FlowSolver& solver, const FlowModuleLoad& load, const Witness& witness)
{
    FlowPlainDataObjects data = allocatePlainData(solver, load.site, load.origin);
    if (data.object == unknownFlowObject) {
        solver.joinUnknown(load.result, witness);
        return;
    }
    solver.join(load.result, solver.values().unknownValue().withoutObjects(), witness);
    solver.joinObject(load.result, data.object, witness);
    solver.joinObject(load.result, data.array, witness);
}

void loadByUnknownName(FlowSolver& solver, const FlowModuleLoad& load, BuiltinID loader, OptionSet<FlowModuleAnswer> answers)
{
    Witness witness = solver.witnessAt(load.site, load.origin);
    bool findsOnlyDataOutsideImage = false;
    if (isInApplication(solver, load.site)) {
        raiseUnseenCode(solver, UnseenCodeCause::LoadsModuleByName, loader, witness);
        // The run time refuses code that is not in the image unless the build admitted it. A module that the build left outside is among the modules below.
        if (solver.build().options().admitsRuntimeCode())
            raiseUnseenCode(solver, UnseenCodeCause::RunsUnseenCode, loader, witness);
        else {
            solver.records().addLoadByUnknownName(witness);
            findsOnlyDataOutsideImage = true;
        }
    }
    if (findsOnlyDataOutsideImage)
        joinContentsOfDataFile(solver, load, witness);
    else
        solver.joinUnknown(load.result, witness);
    for (FlowModuleAnswer answer : answers)
        solver.connect(answersOfModulesLoadedByName(solver, answer), load.result, { }, witness);
    throwUnknownValue(solver, load.thrown, witness);
}

void exposeEveryModule(FlowSolver& solver, const Witness& witness)
{
    if (std::exchange(solver.callState().modules.everyModuleIsExposed, true))
        return;

    FlowStatementSite site = nativeCodeSite(solver);
    BitVector modulesEvaluatedEarlier = solver.records().evaluatedModules();
    uint32_t numberOfModules = solver.program().modules.size();
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        solver.charge(site, 1);
        ModuleID module(index);
        if (modulesEvaluatedEarlier.get(index)) {
            didRequestModuleOfProgram(solver, module);
            exposeModule(solver, module, witness);
            continue;
        }
        // A module of the library that is written in JavaScript is in the image only if seen code names it somewhere. The run time refuses any other, as it refuses a file that was not compiled.
        if (solver.program().at(module).kind == ModuleKind::RuntimeSource)
            continue;
        didRequestModuleOfProgram(solver, module);
        evaluateModule(solver, module, witness);
    }

    for (RuntimeFunctionID loader : solver.builtins().moduleLoaderFunctions()) {
        if (const RuntimeFunctionEntry* entry = findRuntimeFunction(solver.program(), loader))
            enterFromNativeCode(solver, entry->function, FlowObjectID(), witness);
    }
}

// The host hands out what a module exports. Only JavaScript that the build does not see takes the module object itself and calls its members.
void exposeEveryModuleObject(FlowSolver& solver, const Witness& witness)
{
    if (std::exchange(solver.callState().modules.everyModuleObjectIsExposed, true))
        return;

    BitVector evaluatedModules = solver.records().evaluatedModules();
    for (size_t index : evaluatedModules)
        exposeModuleObject(solver, ModuleID(static_cast<uint32_t>(index)), witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
