/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyOperations.h"
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SegmentedVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowCallTemporaryRole : uint16_t {
    ProvenOperand,
    ArgumentLists,
    MixedBoundArguments,
    ReceiversOfSource,
    ReceiversOfGetter,
    ResultOfGetter,
    ReceiversOfBuiltin,
    ReceiversOfBuiltinGetter,
    ResultOfBuiltinGetter,
    PrimitiveReceiver,
    EscapingReceivers,
    NewTargets,
    PrototypeOfNewTarget,
    AwaitedObjects,
    ResolvedValue,
    RejectedValue,
    ThrownOrRejectedValue,
    PromisesSettledByThen,
    Iterables,
    IteratorStates,
    IteratorStateObjects,
    IteratorResults,
    AwaitedIteratorResults,
    IteratorResultObjects,
    AwaitedElements,
    ProtocolOperands,
    MemberValue,
    NamedMemberValues,
    ConvertedMemberValues,
    SpeciesConstructors,
    SpeciesValues,
    TransitiveContents,
    TransitiveObjects,
    OperandObjects,
    RetainerObjects,
    ParameterOperands,
    PlaceSource,
    PlaceSourceObjects,
    PlaceTarget,
    PlaceTargetObjects,
    CallbackResults,
    ActionObjects,
    KeyListObjects,
    KeyListElements,
    ReceiverClassObjects,
    ModuleObjects,
    CapturedExports,
    UnknownValue,
    EscapedInheritors,
    AnswersOfReplacedLoader,
    HandedToUnknownRecipients,
    ModulesLoadedByName,
    ReceiversOfValues,
    CalledValues,
    KnownResultOfBuiltinGetter,
    ContentsOfOperand,
    HoldersOfContents,
    EscapedBoundFunctions,
    ObjectsForNewTargets,
    NewTargetsOfConstructors,
    NewTargetsOfUnknownConstructor,
    ReceiversOutsideNativeCode,
    ContentsOutsideNativeCode,
    ObjectsOutsideNativeCode,
    MatchedObjects,
    IteratedContainers,
    MemberPathValues,
    MemberPathHolders,
    NativeCell,
    ImportedNamespaces,
    ExportsFromGetter,
    UncopiedValuesOfTypedResult,
    TypedResult,
    EscapedObjectsOfOperand,
    PredictedEntryParameter,
    RequiredValueReadByNativeCode,
    ExportsReadByNativeCode,
};

constexpr FlowTemporaryKey callTemporaryKey(FlowStatementSite site, FlowCallTemporaryRole role, uint32_t index = 0)
{
    return { site, FlowTemporaryOwner::Calls, static_cast<uint16_t>(role), index };
}

struct FlowPassedArguments {
    Vector<FlowNodeID, 8> positions;
    FlowNodeID rest;
    Range count { Range::one(0) };

    friend bool operator==(const FlowPassedArguments&, const FlowPassedArguments&) = default;

    bool mayPass(uint32_t position) const { return position < positions.size() || rest; }
    bool alwaysPasses(uint32_t position) const { return static_cast<int64_t>(position) < count.minimum; }
    FlowNodeID at(uint32_t position) const { return position < positions.size() ? positions[position] : rest; }
};

FlowPassedArguments expandArguments(FlowSolver&, const FlowCallRequest&);

enum class FlowInstancePolicy : uint8_t {
    BySummary,
    AlwaysAtCallSite,
};

struct FlowFunctionCall {
    const FlowCallRequest& request;
    const FlowPassedArguments& arguments;
    FunctionID function;
    FlowObjectID closure;
    FlowContext context { FlowContext::Generic };
    FlowInstancePolicy policy { FlowInstancePolicy::BySummary };
};

struct FlowLinkedCall {
    FlowCallLinkID link;
    FlowInstanceID callee;
    bool passesThroughGates { false };
};

// What a statement asks of each callee, and the callees it has asked. A statement asks the same at every run until it takes another operand or turns generic: that is another request.
// Which exact object is passed as the receiver is no part of it: a statement passes any number of them, one by one.
struct FlowLinkedRequest {
    FlowLinkedRequest(uint32_t index, const FlowCallRequest&, const FlowPassedArguments&, FlowContext, FlowInstancePolicy);

    bool isFor(const FlowCallRequest&, const FlowPassedArguments&, FlowContext, FlowInstancePolicy) const;
    // False for a callee that the request has.
    bool hasCallee(const FlowFunctionCall&) const;
    bool addCallee(const FlowFunctionCall&);

    uint32_t index;
    std::optional<uint32_t> previousOfSite;

    ValueID origin;
    FlowCallLinkKind kind;
    BuiltinCallbackTimes times;
    bool isInCycle;
    FlowContext context;
    FlowInstancePolicy policy;
    bool passesExactReceiver;
    FlowNodeID thisValue;
    FlowNodeID newTarget;
    FlowObjectID newTargetObject;
    FlowNodeID result;
    FlowNodeID thrown;
    FlowPassedArguments arguments;

    FlowObjectSet closures;
    Vector<FunctionID> functionsWithoutClosure;
    bool hasUnknownCallee { false };

    // Beside an unknown callee.
    FlowObjectSet predictedCallees;
    bool hasTooManyCalleesToPredict { false };

    // What the values of the new.target node have led to.
    bool allocatesThis { false };
    bool everyConstructorHasOtherNewTarget { false };
    FlowObjectID onlyNewTarget;
};

// new.target is the exact object if there is one, else what the node holds, else the callee itself.
inline bool constructsForNewTargetNode(const FlowCallRequest& request)
{
    return request.kind == FlowCallLinkKind::Construct && !request.operands.newTargetObject && request.operands.newTarget;
}

struct FlowListArguments {
    Vector<FlowNodeID, 8> positions;
    FlowNodeID rest;
};

struct FlowCallLinkState {
    HashMap<std::tuple<FlowInstanceID, ValueID, FlowInstanceID>, FlowCallLinkID> links;
    SegmentedVector<FlowLinkedRequest, 64> linkedRequests;
    HashMap<FlowStatementSite, uint32_t> lastLinkedRequestOfSite;
    HashMap<std::pair<FlowStatementSite, FlowNodeID>, FlowListArguments> listArguments;
    HashSet<FlowStatementSite> genericSites;
    HashSet<std::tuple<FlowInstanceID, ValueID, uint8_t>> recordedUnknownCallees;
    bool constructsUnknownCalleeForOtherNewTarget { false };
    BitVector instancesEnteredWithoutOtherNewTarget;
};

FlowContext contextOf(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, const FlowObjectSet& callees);
bool repeatsOnContextChain(const FlowGraph&, FlowInstanceID caller, FunctionID callee);
bool deservesCallSiteInstance(FlowSolver&, const FlowFunctionCall&);
FlowInstanceID chooseInstance(FlowSolver&, const FlowFunctionCall&);

FlowLinkedRequest* findLinkedRequest(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowContext, FlowInstancePolicy);
FlowLinkedRequest& linkedRequestOf(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowContext, FlowInstancePolicy);
void linkFunctionCall(FlowSolver&, const FlowFunctionCall&);
// For a loop over the callees of one request, which follows the request itself once the loop has ended.
void linkFunctionCall(FlowSolver&, const FlowFunctionCall&, FlowLinkedRequest&);
bool mayLinkWithTrackedOperands(FlowSolver&, const FlowFunctionCall&, const FlowLinkedRequest&);
// What a statement does for all the callees of a request together. Every run comes here, with or without a new callee.
void followLinkedRequest(FlowSolver&, const FlowCallRequest&, FlowLinkedRequest&);
void allocateThisForNewTargets(FlowSolver&, const FlowCallRequest&, const FlowLinkedRequest&);
FlowCallLinkID ensureCallLink(FlowSolver&, const FlowCallRequest&, FlowInstanceID callee);
FlowNodeID provenOperandNode(FlowSolver&, FlowStatementSite, FlowNodeID operand);

struct FlowPassedOperand {
    FlowNodeID from;
    FlowObjectID object;
    uint32_t entryParameter { 0 };
};

void passOperand(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&, const FlowPassedOperand&);
void passPrimitives(FlowSolver&, const FlowLinkedCall&, uint32_t entryParameter, uint16_t primitives);
void passCaptures(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&);
void passThisAndNewTarget(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&);
void passArguments(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&);
void passRestArguments(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&, uint32_t firstRestArgument);
void connectCallResults(FlowSolver&, const FlowFunctionCall&, const FlowLinkedCall&);
void connectPrototypeFromConstructor(FlowSolver&, const FlowCallRequest&, FlowObjectID created, FlowObjectID constructor, BuiltinID defaultPrototype);

void runBoundFunctionCall(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowObjectID boundFunction);
void runCallOfValues(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowNodeID callees);
void callUnknownCallee(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowLinkedRequest&);

void callMethods(FlowSolver&, const FlowCallRequest&, const FlowNewObjects& receivers, const FlowKeySet&);
void callMethodsOfPrimitives(FlowSolver&, const FlowCallRequest&, FlowPrimitiveReceiver, const FlowKeySet&);
void callMethodSource(FlowSolver&, const FlowCallRequest&, FlowObjectID receiver, uint16_t primitiveReceiver, const FlowPropertySource&);

FlowCallRequest requestOf(FlowSolver&, FlowStatementSite, const FlowCall&, std::span<const FlowNodeID> arguments);
FlowCallLinkKind linkKindOf(FlowCallMode);
BuiltinCallMode builtinCallModeOf(FlowCallLinkKind);

FlowStatementSite nativeCodeSite(FlowSolver&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
