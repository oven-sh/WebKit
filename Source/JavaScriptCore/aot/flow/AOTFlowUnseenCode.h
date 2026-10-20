/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCalls.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowUnseenCodeState {
    FlowInstanceID hostInstance;
    FlowStatementReference nativeCodeStatement;
    Vector<FlowObjectID> holdersWithoutMembers;
    Vector<FlowObjectID> holdersOfRefusedMembers;
    // Members that native code's own statements write. Start-up refuses those writes until unknown JavaScript can run.
    BitVector membersWithRefusedWrites;
    bool membersFollowHolders { false };
    bool startupModulesAreExposed { false };
    bool everyUseIsAppliedToUnknownObject { false };
    // A function of the library that only native code's own statements lead to is entered for calls that seen code may make and nobody has seen.
    BitVector libraryInstancesCalledBySeenCode;
    HashMap<FlowInstanceID, Vector<FlowInstanceID>> calleesOfInstancesAwaitingSeenCaller;
    HashMap<FlowInstanceID, Vector<FlowStatementSite>> statementsAwaitingSeenCaller;
    Vector<std::pair<FlowNodeID, Witness>> nodesHandedToUnknownRecipients;
    Vector<std::pair<FlowObjectID, Witness>> objectsHandedToUnknownRecipients;
    // By the index of the class.
    Vector<Vector<FlowNodeID>> nativeCellsReachedThroughClass;
    BitVector classesWithInstanceInUnknownJavaScript;
};

constexpr bool admitsUnknownJavaScript(UnseenCodeCause cause)
{
    switch (cause) {
    case UnseenCodeCause::EvaluatesText:
    case UnseenCodeCause::ImportsUncompiledModule:
    case UnseenCodeCause::RunsUnseenCode:
    case UnseenCodeCause::OptionAdmitsCode:
        return true;
    case UnseenCodeCause::Unknown:
    case UnseenCodeCause::LoadsModuleByName:
    case UnseenCodeCause::LimitReached:
    case UnseenCodeCause::FlowResultDisabled:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// Unseen code opens what it can reach. Only the option that lets any code in, at any place, opens the whole program.
constexpr bool opensWholeProgram(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::OptionAdmitsCode;
}

// A module that stays outside the image is unseen code with a loader of its own. It names the modules of the library, and gets the one instance that the image holds. The run time refuses it the files of the image.
constexpr bool requiresLibraryModules(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::ImportsUncompiledModule;
}

// The text of a function or a module of the library requires modules of the library too.
constexpr bool admitsLibraryText(UnseenCodeCause cause)
{
    return requiresLibraryModules(cause) || opensWholeProgram(cause);
}

constexpr bool loadsModulesByName(UnseenCodeCause cause)
{
    return cause == UnseenCodeCause::LoadsModuleByName || opensWholeProgram(cause);
}

// didReplaceLoaderAnswers names the module cache in its record. No other load by name does: the cache is not callable.
inline bool replacesLoaderAnswers(const UnseenCodeRecord& record)
{
    return opensWholeProgram(record.cause) || (record.cause == UnseenCodeCause::LoadsModuleByName && record.builtin == Builtins::ModuleCache.index());
}

FlowNodeID unknownValueNode(FlowSolver&);
void didLinkCall(FlowSolver&, FlowInstanceID caller, FlowInstanceID callee);
// JavaScript reaches a cell of native memory only through the entries that name it, and each of those takes or answers an instance of a class.
// Code that holds such an instance applies them unseen: it takes out what the cell holds and puts in what it likes.
void noteNativeCellReachedThrough(FlowSolver&, BuiltinClassID, FlowNodeID cell);
FlowNodeID nativeCellNode(FlowSolver&, BuiltinName cell);
// The body of a module of the library is seen code once seen code imports or requires the module. One that only native code requires is entered like a function that only native code calls.
void didRequestModule(FlowSolver&, FlowInstanceID requester, ModuleID);
void didRequestModuleOfProgram(FlowSolver&, ModuleID);
// Every primitive, and the objects that seen code has handed to a function or an object nobody knows.
FlowNodeID valuesHandedToUnknownRecipients(FlowSolver&);

void enterFromNativeCode(FlowSolver&, FunctionID, FlowObjectID closure, const Witness&);
void passDeclaredTypes(FlowSolver&, FlowInstanceID, const Witness&);
void passOtherNewTargetToEnteredInstances(FlowSolver&, const Witness&);
// Whoever may do everything with a constructor constructs it for any new.target: the target of Reflect.construct with a third operand.
void passOtherNewTarget(FlowSolver&, FlowObjectID closure, const Witness&);
// super() under a heritage nobody knows, Reflect.construct of a callee nobody knows: the callee may be any function that has reached native code, now or later.
void constructEscapedFunctionsForOtherNewTarget(FlowSolver&, const Witness&);

void exposeNodesOf(FlowSolver&, FlowObjectID);
void exposePrototypesOf(FlowSolver&, FlowObjectID, const Witness&);
void exposeClosure(FlowSolver&, FlowObjectID, const Witness&);
void exposeBuiltin(FlowSolver&, FlowObjectID, const Witness&);
void exposeNamespace(FlowSolver&, FlowObjectID, const Witness&);
void exposeCapturedNameSources(FlowSolver&, ModuleID root, const Witness&);
void exposeCoroutineResult(FlowSolver&, FlowObjectID, const Witness&);
void openLocation(FlowSolver&, FlowObjectID, FlowNodeID, const Witness&);
void openUnnamedParts(FlowSolver&, FlowObjectID, const Witness&);
Witness reachWitnessOf(const FlowSolver&, FlowObjectID, ObjectMutation);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
