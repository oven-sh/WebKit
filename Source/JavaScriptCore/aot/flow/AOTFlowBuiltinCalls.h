/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowProtocols.h"
#include "AOTUseLists.h"
#include <memory>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SegmentedVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowFunctionUses {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowFunctionUses);

    explicit FlowFunctionUses(const Function& function)
        : cfg(function)
        , uses(function, cfg)
    {
    }

    CFG cfg;
    UseLists uses;
};

// One of a built-in, a bound function, whatever a node holds, and the members that protocols find on what a node holds.
struct FlowBuiltinCallee {
    BuiltinID builtin;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    ModuleID module;
    FlowObjectID boundFunction;
    FlowNodeID values;
    Protocols protocols;
    BuiltinName namedMember;

    friend bool operator==(const FlowBuiltinCallee&, const FlowBuiltinCallee&) = default;
};

inline void add(Hasher& hasher, const FlowBuiltinCallee& callee)
{
    add(hasher, callee.builtin, static_cast<uint8_t>(callee.mode), callee.module, callee.boundFunction, callee.values, callee.protocols.toRaw(), callee.namedMember);
}

// The first call of a callee under a statement of a summary, by the statement itself or by a call it led to, is a statement on the caller's own nodes,
// so the records of the usual call name them. Every other call of that callee under the statement of the summary feeds one statement with nodes of its own.
// Built-ins call each other in cycles and make nodes as they run: sharing is what bounds the number of statements.
enum class FlowBuiltinCallNodes : uint8_t {
    OfCaller,
    Own,
};

struct FlowBuiltinCallKey {
    FlowStatementSite summaryStatement;
    ValueID origin;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Once };
    FlowBuiltinCallee callee;
    FlowBuiltinCallNodes nodes { FlowBuiltinCallNodes::OfCaller };
    bool passesReceiver { false };
    bool passesNewTarget { false };
    bool takesResult { false };
    // The arguments are the choice of a built-in of the host, made in the statement that led to this one.
    bool passesOperandsOfHost { false };

    friend bool operator==(const FlowBuiltinCallKey&, const FlowBuiltinCallKey&) = default;
};

inline void add(Hasher& hasher, const FlowBuiltinCallKey& key)
{
    add(hasher, key.summaryStatement, key.origin, static_cast<uint8_t>(key.kind), static_cast<uint8_t>(key.times), key.callee, static_cast<uint8_t>(key.nodes), key.passesReceiver, key.passesNewTarget, key.takesResult, key.passesOperandsOfHost);
}

struct FlowBuiltinCallOperands {
    FlowNodeID receiver;
    FlowNodeID newTarget;
    FlowObjectID newTargetObject;
    FlowPassedArguments arguments;
    FlowNodeID result;
    FlowNodeID thrown;
    bool isInCycle { false };

    friend bool operator==(const FlowBuiltinCallOperands&, const FlowBuiltinCallOperands&) = default;
};

enum class FlowTypeMatch : uint8_t {
    NotReached,
    Never,
    Sometimes,
    Always,
};

// How the objects that have come out of the cursor of an operand so far lie in a type that the dispatch tests.
struct FlowMatchedObjects {
    FlowNodeID operand;
    BuiltinTypeID type;
    FlowTypeMatch match { FlowTypeMatch::NotReached };
};

// The keys that a node has named so far. The objects that came out of the cursors of the statement were accessed under these.
struct FlowKeysOfNode {
    FlowNodeID node;
    FlowKeySet keys;
};

struct FlowBuiltinCallStatement {
    FlowBuiltinCallKey key;
    FlowBuiltinCallOperands operands;
    Vector<FlowMatchedObjects, 2> matchedObjects;
    Vector<FlowKeysOfNode, 0> keysOfNodes;
    Vector<BuiltinOverloadID, 1> recordedOverloads;
    // Emptied when the statement gets other operand nodes.
    Vector<BuiltinOverloadID, 1> overloadsWithRecordedOperands;
    bool hasArgumentsPastLimit { false };
    bool isAbandoned { false };
};

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct HashTraits<JSC::AOT::Typed::FlowBuiltinCallKey> : GenericHashTraits<JSC::AOT::Typed::FlowBuiltinCallKey> {
    using FlowBuiltinCallKey = JSC::AOT::Typed::FlowBuiltinCallKey;
    using FlowStatementSite = JSC::AOT::Typed::FlowStatementSite;

    static void constructDeletedValue(FlowBuiltinCallKey& slot)
    {
        slot = FlowBuiltinCallKey { };
        HashTraits<FlowStatementSite>::constructDeletedValue(slot.summaryStatement);
    }

    static bool isDeletedValue(const FlowBuiltinCallKey& key) { return HashTraits<FlowStatementSite>::isDeletedValue(key.summaryStatement); }
};

} // namespace WTF

namespace JSC { namespace AOT { namespace Typed {

struct FlowBuiltinCallState {
    SegmentedVector<FlowBuiltinCallStatement, 256> statements;
    HashMap<FlowBuiltinCallKey, uint32_t> statementByKey;
    HashMap<FlowInstanceID, Vector<uint32_t>> statementsByInstance;
    HashSet<std::tuple<FlowStatementSite, BuiltinOverloadID, uint32_t, FlowNodeID>> recordedOperandValues;
    HashMap<std::pair<FlowInstanceID, ValueID>, TypeID> typedResultTypes;
    HashMap<FunctionID, std::unique_ptr<FlowFunctionUses>> usesOfFunctionsWithTypedResults;
    BitVector builtinsWithEnteredAttachments;
    // By the index of the feature: named by an entry of the host whose native code is entered.
    BitVector featuresEnteredByHost;
};

inline FlowBuiltinCallStatement& statementAt(FlowBuiltinCallState& state, FlowStatementSite site)
{
    RELEASE_ASSERT(site.statement.kind == FlowStatementKind::BuiltinCall);
    return state.statements[site.statement.index];
}

TypeID typedResultTypeOf(const FlowCallState&, FlowInstanceID caller, ValueID call);

struct FlowBuiltinCall {
    const FlowCallRequest& request;
    const FlowPassedArguments& arguments;
    BuiltinID builtin;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    ModuleID module;
    FlowNodeID receiver;
};

struct FlowOverloadCall {
    const FlowBuiltinCall& call;
    BuiltinOverloadID id;
    const BuiltinOverload& overload;
};

// These find or make the statement of the call and queue it. Only the solver runs it, so no built-in is ever applied inside the run of another statement.
void callBuiltinObject(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowObjectID callee);
void callBuiltinOfModule(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, BuiltinID, BuiltinCallMode, ModuleID);
void callBoundFunction(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowObjectID boundFunction);
void callValuesWith(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, FlowNodeID callees);
void runProtocolsInOwnStatement(FlowSolver&, const FlowProtocolRequest&);
FlowNodeID escapingReceiverNodeOf(FlowSolver&, const FlowBuiltinCall&);
Witness witnessOf(FlowSolver&, const FlowBuiltinCall&);
bool isAppliedBySeenCode(FlowSolver&, const FlowBuiltinCall&);

FlowNodeID operandNodeOf(const FlowBuiltinCall&, BuiltinOperand);
void forEachOperandNode(const FlowBuiltinCall&, BuiltinOperand, NOESCAPE const ScopedLambda<void(FlowNodeID)>&);
bool mayBePassed(const FlowBuiltinCall&, BuiltinOperand);
bool isAlwaysPassed(const FlowBuiltinCall&, BuiltinOperand);
bool takesOnlyAcceptedObjects(const BuiltinTable&, const BuiltinParameter&);

FlowTypeMatch matchOperand(FlowSolver&, const FlowBuiltinCall&, BuiltinOperand, BuiltinTypeID);
FlowTypeMatch matchPrimitives(const FlowSolver&, const FlowValue&, const BuiltinType&);
FlowTypeMatch matchObject(const FlowSolver&, FlowObjectID, const BuiltinType&);
FlowTypeMatch matchObjectWithPart(const FlowSolver&, FlowObjectID, const BuiltinTypePart&);
FlowTypeMatch unitedMatch(FlowTypeMatch, FlowTypeMatch);
uint16_t primitivesOf(const BuiltinType&);

void forEachSelectedOverload(FlowSolver&, const FlowBuiltinCall&, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>&);
void selectFrom(FlowSolver&, const FlowBuiltinCall&, BuiltinDispatchTarget, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>&);

void recordOverload(FlowSolver&, const FlowOverloadCall&);
void recordOperandValues(FlowSolver&, const FlowOverloadCall&);
void applyUndeclaredOverload(FlowSolver&, const FlowOverloadCall&);

enum class FlowEnteredImplementation : uint8_t {
    None,
    Entered,
};

FlowEnteredImplementation enterImplementation(FlowSolver&, const FlowOverloadCall&);
void enterFunctionCalledByValue(FlowSolver&, const FlowOverloadCall&);
RuntimeFunctionID analyzedFunctionOf(const BuiltinOverload&);
FunctionID functionOf(FlowSolver&, RuntimeFunctionID, BuiltinID user, const Witness&);

void applyParameters(FlowSolver&, const FlowOverloadCall&);
void runProtocolsOfParameters(FlowSolver&, const FlowOverloadCall&);
void applyParameter(FlowSolver&, const FlowOverloadCall&, const BuiltinParameter&);
void applyTransfers(FlowSolver&, const FlowOverloadCall&);
void applyCallbacks(FlowSolver&, const FlowOverloadCall&);
void applyHandlerCalls(FlowSolver&, const FlowOverloadCall&);
void applyCallback(FlowSolver&, const FlowOverloadCall&, const BuiltinCallback&);
FlowNodeID thrownNodeOf(FlowSolver&, const FlowOverloadCall&, const BuiltinCallback&);
FlowNodeID rejectionNodeOf(FlowSolver&, const FlowOverloadCall&);
FlowNodeID operandCodeThrownNodeOf(FlowSolver&, const FlowOverloadCall&);
FlowNodeID callbackResultsNode(FlowSolver&, const FlowOverloadCall&, BuiltinOperand callee);
void connectOperandPart(FlowSolver&, const FlowOverloadCall&, const BuiltinTypePart&, FlowNodeID target);
void applyPropertyActions(FlowSolver&, const FlowOverloadCall&);
void applyPropertyAction(FlowSolver&, const FlowOverloadCall&, const BuiltinPropertyAction&, uint32_t actionIndex);
FlowKeySet keysOf(FlowSolver&, const FlowOverloadCall&, const BuiltinKey&);

uint32_t packedPlace(const BuiltinPlace&);
uint32_t packedOperand(BuiltinOperand);
FlowNodeID sourceNodeOf(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&);
bool mayBeMissing(const FlowBuiltinCall&, const BuiltinPlace&);
FlowNodeID targetNodeOf(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&);
void connectSlotSources(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&, FlowNodeID hub);
void connectSlotTargets(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&, FlowNodeID hub);
void connectOwnValueSources(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&, FlowNodeID hub);
void connectMemberSources(FlowSolver&, const FlowOverloadCall&, const BuiltinPlace&, FlowNodeID hub);
void connectIntoSlot(FlowSolver&, const FlowOverloadCall&, FlowNodeID from, FlowObjectID, BuiltinSlotID);
bool holdsResolvedValues(const BuiltinTable&, BuiltinSlotID);

void applyResult(FlowSolver&, const FlowOverloadCall&);
void applyRangeRule(FlowSolver&, const FlowOverloadCall&);
void allocateExpectedResult(FlowSolver&, const FlowCallRequest&);
void forEachResultObject(FlowSolver&, const FlowOverloadCall&, NOESCAPE const ScopedLambda<void(FlowObjectID)>&);

// The table derives a transfer for each part that names an operand in a result type or in a slot of the result, and none for such a part further inside or in the type of a new value.
enum class FlowOperandParts : uint8_t {
    HaveTransfers,
    AreConnected,
};

struct FlowBuiltinValue {
    BuiltinTypeID type;
    FlowNodeID target;
    FlowSiteOrigin origin { FlowSiteOrigin::BuiltinResult };
    FlowOperandParts operandParts { FlowOperandParts::HaveTransfers };
};

using FlowBuiltinTypePath = Vector<BuiltinTypeID, 4>;

void joinValueOfType(FlowSolver&, const FlowOverloadCall&, const FlowBuiltinValue&, FlowBuiltinTypePath&);
void joinPrimitivesOfType(FlowSolver&, const BuiltinType&, FlowNodeID target, const Witness&);
void joinPart(FlowSolver&, const FlowOverloadCall&, const FlowBuiltinValue&, const BuiltinTypePart&, FlowBuiltinTypePath&);
FlowObjectID allocatePart(FlowSolver&, const FlowOverloadCall&, const FlowBuiltinValue&, const BuiltinTypePart&);
void fillAllocatedPart(FlowSolver&, const FlowOverloadCall&, const FlowBuiltinValue&, const BuiltinTypePart&, FlowObjectID, FlowBuiltinTypePath&);
void joinInstancesOfReceiverClass(FlowSolver&, const FlowOverloadCall&, const FlowBuiltinValue&, const BuiltinTypePart&);
void allocateForNewTargets(FlowSolver&, const FlowOverloadCall&, BuiltinClassID);

bool conditionMayHold(FlowSolver&, const FlowBuiltinCall&, const BuiltinCondition*);
void applyCallAttachments(FlowSolver&, const FlowBuiltinCall&);
void applyOverloadAttachments(FlowSolver&, const FlowOverloadCall&);
// For an overload that native code takes, at a call that seen code makes. Nothing says what the code behind one without a declaration calls, and one that runs code of its own and answers anything may answer a promise.
void noteUndescribedNativeCode(FlowSolver&, const FlowOverloadCall&);
void applyOperandRoles(FlowSolver&, const FlowOverloadCall&, const BuiltinParameter&);
void loadModulesNamedBy(FlowSolver&, const FlowOverloadCall&, FlowNodeID specifier);
void enterAttachmentsOf(FlowSolver&, BuiltinID, const Witness&);
void enterAttachmentsOfEscapedBuiltin(FlowSolver&, BuiltinID, const Witness&);
void loadRuntimeModule(FlowSolver&, const BuiltinModuleLoad&, const Witness&);

void callBuiltinFromNativeCode(FlowSolver&, FlowObjectID builtinObject);
void callBuiltinAccessorFromNativeCode(FlowSolver&, BuiltinID accessor, BuiltinCallMode);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
