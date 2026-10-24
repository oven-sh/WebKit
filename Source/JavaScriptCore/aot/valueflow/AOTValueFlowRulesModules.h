/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// What the host's body holds beside the first values of the variables: the nodes of ValueFlowHostNode, the names of the module objects that the host can make, and one statement of each kind that runs for the host.
void describeHost(const Program&, ValueFlowBody&);

// EvaluateModule, RequireModule, RequireBuiltinModule, RequireDynamic, ImportModule, ImportDynamic, ResolveDynamic, ModuleNamespace, DirectEval, and a Builtin that has a statement: one that names the object of a compiled module.
bool isModuleOperation(Opcode);
void runModuleOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
// A statement of kind ModuleEvaluation lies in the body of its module and runs once that body is reached: what evaluating the module does beside running the body.
void runModuleEvaluation(const ValueFlowStatement&, ValueFlowRuleContext&);

// The statements of the host's body.
void runHostRoots(const ValueFlowStatement&, ValueFlowRuleContext&);
void runUsedBuiltins(const ValueFlowStatement&, ValueFlowRuleContext&);
void runUnseenCode(const ValueFlowStatement&, ValueFlowRuleContext&);
void runModulesLoadedByName(const ValueFlowStatement&, ValueFlowRuleContext&);

// A value that comes from several places.
struct ValueFlowSources {
    Vector<ValueFlowNodeID, 2> nodes;
    ClimbingSet constant;
};

void connect(ValueFlowRuleContext&, const ValueFlowSources&, ValueFlowNodeID target);
ClimbingSet valueOf(ValueFlowRuleContext&, const ValueFlowSources&);
// What a spread or Object.assign has copied onto an object is written nowhere on it: it lies on the objects that it was copied from, and on those that something was copied from onto them.
ClimbingSet withSourcesOfCopies(ValueFlowRuleContext&, ClimbingSet knownObjects);

// An import alone does not evaluate a module without side effects: a function of the module that is reached does, and so does whoever names one of its variables or holds its namespace object.
bool awaitsUse(const Program&, ModuleID);
void useModule(ValueFlowRuleContext&, ModuleID);
// None for a module of the run time that the program does not hold.
ModuleID moduleOf(const Program&, RuntimeModuleID);
// Reaches the body, and the bodies of what the module imports. What it requires is evaluated by the operations in its code.
void evaluateModule(ValueFlowRuleContext&, ModuleID);
// What require() of the module answers. Of a CommonJS file: what the variable holds that stands for `module.exports`.
ValueFlowSources requiredValueOf(ValueFlowRuleContext&, ModuleID);
// AOTValueFlowRulesHost.cpp.
bool isGetterFoundWithoutItsName(const BuiltinTable&, BuiltinID);
// The namespace object, with its properties tied to the variables that the module exports. Where the run time takes the names from the value of the module, the object finds what that value holds.
ValueFlowSources namespaceOf(ValueFlowRuleContext&, ModuleID, ImportPhase = ImportPhase::Evaluation);
// What the evaluation of the module may throw at whoever asked for it.
ValueFlowSources evaluationErrorsOf(ValueFlowRuleContext&, ModuleID);

// Only what seen code does is an event.
bool isInSeenCode(ValueFlowRuleContext&, FunctionID);
// The statement that runs stores under the name into a receiver that may be the unknown object.
// The function asks for the module: by an import of its module, or by an operation.
void noteRequest(ValueFlowRuleContext&, FunctionID requester, ModuleID);
// For an operation that calls what its first operand holds.
void applyEventsOfCall(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet callees);

// For whatever stores, defines or deletes under the name, or under a key nobody knows where there is no name.
void applyEventsOfStore(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet receivers, PropertyKeyID name);
// A member that holds a table or a function of the loader: `require.cache`, `Module._cache`, `Module._load`.
bool isLocationOfLoader(BuiltinID canonicalMember);
// `require.main`, `module.children` and `module.parent`: the members of the objects of one module that lead to the module object of another.
bool isWayBetweenModuleObjects(BuiltinID canonicalMember);
// What an entry answers whose type names the module object.
void appendModuleObjectsAnsweredBy(ValueFlowRuleContext&, BuiltinID canonicalEntry, Vector<ValueFlowObjectID, 32>&);
// The host brings the list behind `module.children` up to date through a function of the library, (list, child), as the row of the entry says. What the program has stored in the list's place gets the children the same way, and nobody follows it there.
void handChildrenToWhatReplacesTheirList(ValueFlowRuleContext&, ValueSetID children);
// A location of the loader, or process.mainModule: a way to the module objects of the program.
bool isFoundByNameAlone(BuiltinID canonicalMember);

// What a call of the built-in by the program lets in. Nothing for a built-in that runs no code but its own and its operands', and nothing for an evaluator of the engine in a build that excludes evaluated text.
std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable&, BuiltinID, bool refusesEvaluatedText);

// Native code calls the function with operands nobody knows and keeps what it answers.
void enterFromNativeCode(ValueFlowRuleContext&, FunctionID);
// Native code calls the functions with what the annotations of their own parameters say. Whoever asks connects `this`, what they answer and what they throw.
void passWhatAnnotationsSay(ValueFlowRuleContext&, ClimbingSet callees);
// The running statement calls native code that no entry describes.
void letUndeclaredNativeCodeRun(ValueFlowRuleContext&);
// A built-in of the host calls something of the program with operands of its own choice: what native code reads from the start-up modules is among them.
void letHostChooseOperands(ValueFlowRuleContext&);
// Code that the build has not seen can run from here on: evaluated text, a module that stays outside the image.
enum class UnseenCodeReach : uint8_t {
    GlobalScope,
    Loader,
    Realm,
};
void letUnseenCodeRun(ValueFlowRuleContext&, UnseenCodeReach);
// Whoever holds what has escaped may read it.
void letUnknownCodeWrite(ValueFlowRuleContext&);
// Unseen code writes too.
bool mayUnknownCodeWrite(ValueFlowRuleContext&);
// Reading a member is not calling it.
void letWhateverHasEscapedBeCalled(ValueFlowRuleContext&);
bool mayCallWhateverHasEscaped(ValueFlowRuleContext&);
// The unknown value lies under every name, in every slot and above each of the objects, and they have every mark.
void letAnythingHappenTo(ValueFlowRuleContext&, ClimbingSet objects);
// Some load names its module at run time: an entry point may be the one, a module of the library that reached code names by a constant, or a native module.
void letModulesBeLoadedByName(ValueFlowRuleContext&, ValueFlowHostNode answers);
// Whether such a name may find the module, should it be in the image: no file of the program but an entry point, and nothing that the bundler imports on its own.
bool mayAnswerToName(const Program&, ModuleID);
// The functions of the library that the host runs for a load that the run time hands on to it.
void enterLoaderOfHost(ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
