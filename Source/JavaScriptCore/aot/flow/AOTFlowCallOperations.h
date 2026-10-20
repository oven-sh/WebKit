/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfProtocols.h"
#include "AOTFlowSolution.h"
#include <memory>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct FlowSummaryInputs {
    FlowSummaryInputs(const Program&, BuildContext&);

    const Program& program;
    BuildContext& build;
    // Whether an import may leave a module unevaluated, so that a function has to say which modules it uses.
    bool hasModulesWithoutSideEffects;
};

FlowSummary buildFlowSummary(const FlowSummaryInputs&, FunctionID);
FlowSummary buildHostSummary(const FlowSummaryInputs&);

std::unique_ptr<FlowCallState, FlowCallStateDeleter> createFlowCallState(FlowSolver&);

void addRoots(FlowSolver&);

struct FlowCallOperands {
    FlowNodeID thisValue;
    FlowObjectID thisObject;
    FlowNodeID newTarget;
    FlowObjectID newTargetObject;
    std::span<const FlowNodeID> arguments;
    FlowNodeID argumentList;
    FlowNodeID result;
    FlowNodeID thrown;
};

struct FlowCallRequest {
    FlowStatementSite site;
    ValueID origin;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Once };
    bool isInCycle { false };
    FlowCallOperands operands;
};

// isInCycle is about the operation at the site, times about the code that the operation runs.
inline bool mayRunRepeatedly(const FlowCallRequest& request)
{
    return request.isInCycle || request.times != BuiltinCallbackTimes::Once;
}

// An operand passed by node follows the node: it reaches a compiled callee over an edge, and a built-in or a bound function is a statement of its own that waits on the node.
// No call runs the callee before it returns, so what the callee adds to the result arrives later.
void callObjects(FlowSolver&, const FlowCallRequest&, const FlowNewObjects& callees);
// Calls what the node holds and what arrives in it later.
void callValuesOf(FlowSolver&, const FlowCallRequest&, FlowNodeID callees);
void callBuiltin(FlowSolver&, const FlowCallRequest&, BuiltinID, BuiltinCallMode);
// Code that nobody knows may run at the operation: what it calls, or an accessor that its lookup may find.
void noteUnknownCallee(FlowSolver&, FlowStatementSite, ValueID origin, FlowCallLinkKind);

struct FlowProtocolRequest {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID operand;
    Protocols protocols;
    std::span<const BuiltinName> namedMembers;
    FlowNodeID result;
    FlowNodeID thrown;
    NamedMembers namedMemberUse { NamedMembers::AreOnlyRead };
    std::span<const BuiltinName> calledMembers { };
    // What the protocols find goes back into the operand and nowhere else.
    bool resultStaysInOperand { false };
    // The receivers are what a toJSON method returned, and they are written like the values that ToJSON is run on.
    bool isPartOfSerialization { false };
    // A function among the receivers is kept and called. What is read under namedMembers is converted whatever it is.
    bool convertsFunctionsUnderNamesOnly { false };
    std::span<const BuiltinName> convertedMembers { };
};

void runProtocols(FlowSolver&, const FlowProtocolRequest&);

void didReachNativeCode(FlowSolver&, FlowObjectID);
void didReachUnknownJavaScript(FlowSolver&, FlowObjectID);
void exposeLocation(FlowSolver&, FlowObjectID, FlowNodeID);
// From a read under a key nobody can name through the unknown object on, the unknown value also stands for the members of the built-in objects in nativeCode.
void exposeMembersOfEscapedBuiltins(FlowSolver&, const Witness&);

void raiseUnseenCode(FlowSolver&, UnseenCodeCause, BuiltinID, const Witness&);
// Whether applying the built-in is an event.
bool letsInUnseenCode(const BuiltinTable&, BuiltinID);
// The table does not describe what the object holds: what a native binding answers, and an object that lets unseen code in. Its flow object stands for all of that, functions and what they return.
bool standsForWhatItHolds(const BuiltinTable&, BuiltinID);
// What seen code passes to a function, or stores into an object, that nobody knows. It reaches native code. Once unseen code can run, the function or the object may be that code's own.
void handToUnknownRecipient(FlowSolver&, FlowNodeID, const Witness&);
void handToUnknownRecipient(FlowSolver&, FlowObjectID, const Witness&);
// What seen code puts where only unseen code looks on its own. Nobody but seen code reads it until unseen code can run.
void handToUnseenCode(FlowSolver&, FlowNodeID, const Witness&);
// Native code requires a start-up module where it hands a function of that module to code of the program: a built-in of the host that seen code calls finds a member of an operand
// by name and calls it with operands of its own choice. Until then nobody reads what the module makes.
void exposeStartupModules(FlowSolver&, const Witness&);
// Only a call that seen code makes is an event. The statements of native code raise none, and neither does a function of the library that only they lead to:
// the run time refuses every such entry unless the build has seen an event. A statement that gets the answer no runs again once seen code calls its function.
bool isInSeenCode(FlowSolver&, FlowStatementSite);
// A name that one of the host's own files computes comes from outside the program (a configuration file, the command line, text typed at a prompt) and names code outside the image.
bool isInApplication(FlowSolver&, FlowStatementSite);
void noteBuiltinUse(FlowSolver&, BuiltinID, OptionSet<BuiltinUse>, const Witness&);
// The uses of an entry that enter the native code behind it, and with that code the features that the entry names.
OptionSet<BuiltinUse> usesEnteringCode(const BuiltinTable&, BuiltinID);
// Native code calls what the entries of a class name for any instance of the class or of one below it, whoever made the instance: it reports a promise that is rejected, it calls the listeners of an event target.
void noteInstanceOf(FlowSolver&, BuiltinClassID, const Witness&);
// A native binding without a signature, an overload of the host without a declaration: nothing says what the native code behind the call calls, so it calls what the table lists for such code. Only a call that seen code makes counts, as for an event.
// A callee nobody knows is no such call: an object of the host that the table does not describe comes out of one of these.
void didCallUndeclaredNativeCode(FlowSolver&, FlowStatementSite, const Witness&);
void letUndeclaredNativeCodeRun(FlowSolver&, const Witness&);
bool conditionMayHold(const Program&, const FlowValueTable&, const FlowValue& operand, const BuiltinCondition&);
void evaluateModule(FlowSolver&, ModuleID, const Witness&);

void runCall(FlowSolver&, FlowStatementSite, const FlowCall&);
void runDirectCall(FlowSolver&, FlowStatementSite, const FlowDirectCall&);
void runAwait(FlowSolver&, FlowStatementSite, const FlowAwait&);
void runIteration(FlowSolver&, FlowStatementSite, const FlowIteration&);
void runOperandUse(FlowSolver&, FlowStatementSite, const FlowOperandUse&);
void runModuleOperation(FlowSolver&, FlowStatementSite, const FlowModuleOperation&);
void runUnseenCodeEvent(FlowSolver&, FlowStatementSite, const FlowUnseenCodeEvent&);
void runBuiltinCall(FlowSolver&, FlowStatementSite);

// The operands reach unknownJavaScript, the results are unknown, and the callee reaches nativeCode, which calls it with unknown operands.
void abandonBuiltinCall(FlowSolver&, FlowStatementSite);
void abandonBuiltinCallsOf(FlowSolver&, FlowInstanceID);
// Null for a call of a bound function.
BuiltinID builtinCalledAt(FlowSolver&, FlowStatementSite);
// A built-in of the host finds a member of an operand by name and calls it with operands of its own choice, which nobody described. So does each statement that its call leads to.
// A built-in of the engine passes what the language says: its operands and what it has made of them, and no function of the host.
bool passesOperandsOfHost(FlowSolver&, FlowStatementSite caller, const FlowCallRequest&);

FlowProgress refineExecutionOrder(FlowSolver&);
FlowProgress refineEscapedInheritors(FlowSolver&);

enum class FlowModuleOrdering : uint8_t {
    Unknown,
    CompletesBefore,
};

FlowModuleOrdering moduleOrderingOf(const FlowSolution&, ModuleID first, ModuleID second);

enum class FlowCacheEntryState : uint8_t {
    Unknown,
    NeverRemoved,
};

FlowCacheEntryState cacheEntryStateOf(const FlowSolution&, ModuleID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
