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
// A lookup goes there by itself. Whoever reads straight off known objects what they hold of their own reads off these.
ClimbingSet withSourcesOfCopies(ValueFlowRuleContext&, ClimbingSet knownObjects);

// An import alone does not evaluate a module without side effects: a function of the module that is reached does, and so does whoever names one of its variables or holds its namespace object.
// An importer may also have a name from a module that exports it again: such a module is used with the name, so what it imports for its side effects is evaluated.
// A module without code has nothing to wait with: the loader fills its variables.
bool awaitsUse(const Program&, ModuleID);
void useModule(ValueFlowRuleContext&, ModuleID);
// None for a module of the run time that the program does not hold.
ModuleID moduleOf(const Program&, RuntimeModuleID);
// Reaches the body, and the bodies of what the module imports. What it requires is evaluated by the operations in its code.
void evaluateModule(ValueFlowRuleContext&, ModuleID);
// What require() of the module answers. Of a CommonJS file: what the variable holds that stands for `module.exports`.
ValueFlowSources requiredValueOf(ValueFlowRuleContext&, ModuleID);
// AOTValueFlowRulesHost.cpp. Reading an accessor is calling its getter, and text that nobody has seen reads what it likes. Where such text may run, the function of the library behind an accessor is in the program
// if the table lists nothing else of the library that the accessor needs: it costs tens of bytes. In a program that nobody else gets into, it is entered by the reads that name it:
// entered from the start it would bring the unknown object into a program that has none (JSON.stringify of one object: 16 functions | 475).
// TODO: compiled `unknown[key]` names no member of the host, so it enters none: where it hits such an accessor the read is refused with the error that names it.
bool isGetterFoundWithoutItsName(const BuiltinTable&, BuiltinID);
// The namespace object, with its properties tied to the variables that the module exports. Where the run time takes the names from the value of the module, the object finds what that value holds.
ValueFlowSources namespaceOf(ValueFlowRuleContext&, ModuleID, ImportPhase = ImportPhase::Evaluation);
// What the evaluation of the module may throw at whoever asked for it.
ValueFlowSources evaluationErrorsOf(ValueFlowRuleContext&, ModuleID);

// Only what seen code does is an event. The program's own functions are seen code, and so are the functions of the library that seen code calls and the body of a module of the library that seen code asks for.
// A function of the library that only native code leads to stands for calls that nobody has seen, and the run time refuses those what they would let in.
bool isInSeenCode(ValueFlowRuleContext&, FunctionID);
// The statement that runs stores under the name into a receiver that may be the unknown object.
void noteStoreThroughUnknownObject(ValueFlowRuleContext&, ValueFlowName);
// The function asks for the module: by an import of its module, or by an operation.
void noteRequest(ValueFlowRuleContext&, FunctionID requester, ModuleID);
// For an operation that calls what its first operand holds. Its result takes what a loader among the callees answers.
// A statement without a function stands for calls of statements that nobody can name here: accessors, protocols and callbacks of built-ins. Seen code may be among them.
void applyEventsOfCall(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet callees);

// For whatever stores, defines or deletes under the name, or under a key nobody knows where there is no name.
// The module cache lists what the host has loaded from outside the image, and no module of the image. An entry that the program puts there answers a load under a name nobody knows.
void applyEventsOfStore(const ValueFlowStatement&, ValueFlowRuleContext&, ClimbingSet receivers, PropertyKeyID name);
// A member that holds a table or a function of the loader: `require.cache`, `Module._cache`, `Module._load`. The run time refuses whoever puts another function in the place of the loader's,
// so a constant name loads the module that it stood for in the build. `unknown._cache` is no read of one.
bool isLocationOfLoader(BuiltinID canonicalMember);
// `require.main`, `module.children` and `module.parent`: the members of the objects of one module that lead to the module object of another.
bool isWayBetweenModuleObjects(BuiltinID canonicalMember);
// What an entry answers whose type names the module object. `require.main` and process.mainModule answer that of an entry point. `children` answers those of the modules that a module requires
// whose own module object somebody has got hold of, and `parent` those of the modules that require such a module. Every other entry answers one that is no module's.
void appendModuleObjectsAnsweredBy(ValueFlowRuleContext&, BuiltinID canonicalEntry, Vector<ValueFlowObjectID, 32>&);
// A location of the loader, or process.mainModule: a way to the module objects of the program. Only a read that names it finds it. Under a key nobody knows, and through a placeholder,
// the run time answers as in an image whose code does not name it: the error of a function that was not compiled, or undefined.
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
// How far code reaches that the build has not seen.
// GlobalScope: the text that `Function`, its relatives and eval run. It is a function body or eval code, so it sees the global object, what hangs from it and what it is handed, and it declares nothing
// that a free name of the program finds. The run time refuses it, as it refuses native code, every change of what compiled code relies on in the objects of the realm.
// Loader: a module that stays outside the image. It has `require` and `module` besides, loads every module of the library and starts what it likes. Of the module objects of the program it sees none.
// The realm it finds as text does: an assignment to what compiled code relies on is refused, whoever makes it.
// Realm: anything else. It is started with the loader itself in its hands and supplies the text of modules: nothing about the realm is relied on.
enum class UnseenCodeReach : uint8_t {
    GlobalScope,
    Loader,
    Realm,
};
void letUnseenCodeRun(ValueFlowRuleContext&, UnseenCodeReach);
// Whoever holds what has escaped may read it. From here on it may also write: an object has been handed to native code of which the table says nothing, or the unknown object is called,
// which may be code that stores into its operands, redefines them or gives them another prototype.
void letUnknownCodeWrite(ValueFlowRuleContext&);
// Unseen code writes too.
bool mayUnknownCodeWrite(ValueFlowRuleContext&);
// Reading a member is not calling it. A built-in of the language that has escaped is held: it may be kept, compared and stored. The code behind it runs where a reached call may have it as its callee.
// `x.key(..)` with a callee nobody knows applies the members under the key: see runCall(). Any other call of a value nobody knows reaches whatever has escaped, and so does unseen code.
// Native code of which the table says nothing calls what it is handed and what that leads to, as the handler of a proxy.
void letWhateverHasEscapedBeCalled(ValueFlowRuleContext&);
bool mayCallWhateverHasEscaped(ValueFlowRuleContext&);
// The unknown value lies under every name, in every slot and above each of the objects, and they have every mark.
// But for the objects that the run time locks, and for the other built-in objects of the realm as long as no unseen code runs.
void letAnythingHappenTo(ValueFlowRuleContext&, ClimbingSet objects);
// Some load names its module at run time: an entry point may be the one, a module of the library that reached code names by a constant, or a native module. The load runs no module by itself.
// The node is the one that holds what such a load answers.
void letModulesBeLoadedByName(ValueFlowRuleContext&, ValueFlowHostNode answers);
// Whether such a name may find the module, should it be in the image: no file of the program but an entry point, and nothing that the bundler imports on its own.
bool mayAnswerToName(const Program&, ModuleID);
// The functions of the library that the host runs for a load that the run time hands on to it.
void enterLoaderOfHost(ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
