/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCalls.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowModuleState {
    Vector<ModuleID> moduleOfRuntimeModule;
    Vector<ModuleID> modulesToEvaluate;
    BitVector hasCapturedExports;
    // The modules without side effects that export a variable of another module, by the variable. Filled at the first question.
    std::optional<HashMap<VariableID, Vector<ModuleID, 1>>> modulesExportingAgain;
    bool isEvaluatingModules { false };
    bool everyModuleIsExposed { false };
    bool libraryModulesAreOpenToUnseenCode { false };
    bool everyModuleObjectIsExposed { false };
};

struct FlowModuleLoad {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID result;
    FlowNodeID thrown;
};

Vector<ModuleID> indexRuntimeModules(const Program&);
// The object of a module of the host that is written in JavaScript is what the body of the module makes. The table only lists its names.
RuntimeModuleID findCompiledModuleWithObject(const Program&, BuiltinID);
ModuleID moduleOf(const FlowCallState&, RuntimeModuleID);

void evaluateOneModule(FlowSolver&, ModuleID, const Witness&);
// Evaluates a module that an import alone does not evaluate.
void useModule(FlowSolver&, ModuleID, const Witness&);
void useModulesBehindVariable(FlowSolver&, VariableID, const Witness&);
void useModuleBehindExport(FlowSolver&, const ModuleExport&, const Witness&);
void useModulesBehindNamespace(FlowSolver&, ModuleID, const Witness&);
void enterModuleBody(FlowSolver&, ModuleID, const Witness&);
void prepareCommonJSObjects(FlowSolver&, ModuleID, const Witness&);
void fillExportsOfLoader(FlowSolver&, ModuleID, const Witness&);
void captureExportsForImporter(FlowSolver&, ModuleID imported);

FlowNodeID moduleExportsNode(FlowSolver&, ModuleID);
void didCreateGetterOfModuleObject(FlowSolver&, FlowObjectID moduleObject, PropertyKeyID, FlowNodeID getter);
FlowNodeID commonJSModuleObjectsNode(FlowSolver&);
void connectNamespaceOf(FlowSolver&, ModuleID, FlowNodeID target, const Witness&);
void connectNamespaceOf(FlowSolver&, ModuleID, ImportPhase, FlowNodeID target, const Witness&);
void connectRequiredValueOf(FlowSolver&, ModuleID, FlowNodeID target, const Witness&);
void connectDefaultExportOf(FlowSolver&, ModuleID, FlowNodeID target, const Witness&);

void requireModule(FlowSolver&, const FlowModuleLoad&, ModuleID);
void importModule(FlowSolver&, const FlowModuleLoad&, std::span<const ModuleID>, bool mayLoadOtherModules);
void requireRuntimeModule(FlowSolver&, const FlowModuleLoad&, RuntimeModuleID);
enum class FlowModuleAnswer : uint8_t {
    RequiredValue = 1 << 0,
    Namespace = 1 << 1,
};

void loadByUnknownName(FlowSolver&, const FlowModuleLoad&, BuiltinID loader, OptionSet<FlowModuleAnswer>);
// What a load by a name nobody knows can answer: what each module of the image exports. The statement that loads is seen code, so what it does with the answer is followed like any other value.
FlowNodeID answersOfModulesLoadedByName(FlowSolver&, FlowModuleAnswer);
void exposeEveryModule(FlowSolver&, const Witness&);
void openLibraryModulesToUnseenCode(FlowSolver&, const Witness&);
void exposeEveryModuleObject(FlowSolver&, const Witness&);
// The program reads, or writes, a location through which the run time answers a require with a constant name.
void didUseLoaderLocation(FlowSolver&, BuiltinID location, OptionSet<BuiltinUse> added, const Witness&);
void didReplaceLoaderAnswers(FlowSolver&, const Witness&);
// The module cache and the table of loaders by file extension: what the program stores into one of them answers a later require.
bool holdsLoaderAnswers(BuiltinID object);
// A store to one of these locations changes what a later require answers.
bool isLoaderLocation(BuiltinID location);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
