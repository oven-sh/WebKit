/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// What the host's body holds beside the first values of the variables: the nodes of ValueFlowHostNode, the operands of the bodies of CommonJS modules, and one statement of each kind that runs for the host.
void describeHost(const Program&, ValueFlowBody&);

// RequireModule, RequireBuiltinModule, RequireDynamic, ImportModule, ImportDynamic, ResolveDynamic, ModuleNamespace, DirectEval, and a Builtin that has a statement: one that names the object of a compiled module.
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

// The table lists the object that a module of the host answers as a built-in. Where the program holds that module as compiled code, the object is what its body makes. None for any other built-in.
ModuleID compiledModuleWithObject(const Program&, BuiltinID);
// An import alone does not evaluate a module without side effects: a function of the module that is reached does, and so does whoever names one of its variables or holds its namespace object.
// A module without code has nothing to wait with: the loader fills its variables.
bool awaitsUse(const Program&, ModuleID);
void useModule(ValueFlowRuleContext&, ModuleID);
// None for a module of the run time that the program does not hold.
ModuleID moduleOf(const Program&, RuntimeModuleID);
// Reaches the body, and the bodies of what the module requests.
void evaluateModule(ValueFlowRuleContext&, ModuleID);
// What require() of the module answers.
ValueFlowSources requiredValueOf(ValueFlowRuleContext&, ModuleID);
// The namespace object, with its properties tied to the variables that the module exports.
ValueFlowSources namespaceOf(ValueFlowRuleContext&, ModuleID, ImportPhase = ImportPhase::Evaluation);
// What the evaluation of the module may throw at whoever asked for it.
ValueFlowSources evaluationErrorsOf(ValueFlowRuleContext&, ModuleID);

// Only what seen code does is an event. The program's own functions are seen code, and so are the functions of a module of the library that seen code asks for.
// A function of the library that only native code leads to stands for calls that nobody has seen, and the run time refuses those what they would let in.
bool isInSeenCode(ValueFlowRuleContext&, FunctionID);
// The function asks for the module: by a request of its module, an import or a require.
void noteRequest(ValueFlowRuleContext&, FunctionID requester, ModuleID);
// For an operation that calls what its first operand holds.
// A statement without a function stands for calls of statements that nobody can name here: accessors, protocols and callbacks of built-ins. Seen code may be among them.
void applyEventsOfCall(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet callees);

// For whatever stores, defines or deletes under the name, or under a key nobody knows where there is no name.
void applyEventsOfStore(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet receivers, PropertyKeyID name);

// What a call of the built-in by the program lets in. Nothing for a built-in that runs no code but its own and its operands'.
std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable&, BuiltinID);

// Native code calls the function with operands nobody knows and keeps what it answers.
void enterFromNativeCode(ValueFlowRuleContext&, FunctionID);
// The running statement calls native code that no entry describes.
void letUndeclaredNativeCodeRun(ValueFlowRuleContext&);
// A built-in of the host calls something of the program with operands of its own choice: what native code reads from the start-up modules is among them.
void letHostChooseOperands(ValueFlowRuleContext&);
// Code that the build has not seen can run from here on: evaluated text, a module that stays outside the image.
void letUnseenCodeRun(ValueFlowRuleContext&);
// From here on everything may happen to what has escaped: an object has been handed to native code of which the table says nothing, or something is called that nobody knows,
// which may be a built-in that stores into its operands, redefines them or gives them another prototype.
void openEscapedObjects(ValueFlowRuleContext&);
// Unseen code opens them too.
bool areEscapedObjectsOpen(ValueFlowRuleContext&);
// The unknown value lies under every name, in every slot and above each of the objects, and they have every mark.
void letAnythingHappenTo(ValueFlowRuleContext&, ClimbingSet objects);
// Some load names its module at run time: every module that the image holds may be the one. The node is the one that holds what such a load answers.
void letModulesBeLoadedByName(ValueFlowRuleContext&, ValueFlowHostNode answers);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
