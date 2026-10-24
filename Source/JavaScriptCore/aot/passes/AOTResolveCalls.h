/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTMethodDispatch.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTRefusedContents.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTVector.h"
#include <expected>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/Variant.h>

namespace JSC { namespace AOT {

struct BuiltinOverload;

namespace Typed {

// The operations that CallResolver looks at.
inline constexpr OpcodeSet callSiteOpcodes {
    Opcode::Call,
    Opcode::CallList,
    Opcode::Construct,
    Opcode::ConstructList,
    Opcode::SuperConstruct,
    Opcode::SuperConstructList,
    Opcode::PropGet,
    Opcode::PropSet,
    Opcode::SuperGet,
    Opcode::SuperSet,
    Opcode::InstanceOf,
    Opcode::CallBuiltin,
};

bool canTakeAllocatedThis(const Program&, FunctionID constructor);
// The class of the constructor, or the layout derived from it that this version of the constructor allocates.
LayoutID layoutAllocatedBy(const Program&, FunctionID constructor);
// A function that takes `this` can have a copy that `new` enters where the function is its own new.target. The copy takes no `this`: it allocates the object and returns what was constructed.
bool canHaveConstructBody(const Program&, FunctionID constructor);
bool isConstructorFunction(const Program&, const Function&);
// What `new` hands a constructor function that is its own new.target: an object with the layout of the function. `closure` is that function.
ValueID appendThisOfConstructorFunction(IRBuilder&, FunctionID constructor, ValueID closure);
// Ends the block: the object is returned unless the constructor has returned another.
void emitReturnOfConstruction(IRBuilder&, ValueID returned, ValueID object);

struct MemberRead {
    Vector<ValueSite, 2> links;
    // What a validating build looks at the links with. It goes with them.
    Vector<ValueID, 2> validations;
    bool canSinkToCall { false };

    bool isEmpty() const { return links.isEmpty(); }
    const ValueSite& read() const LIFETIME_BOUND { return links.first(); }
};

// `F.prototype.m`, read or called with a `this` that the caller names: the lookup starts at the prototype object of the class of F.
struct PrototypeMemberRead {
    LayoutID classLayout;
    ValueID classObject;
    PropertyKeyID key;
    // The read of `prototype` and, of a call, the read of the member, each with its casts. Nothing else uses them.
    MemberRead reads;

    explicit operator bool() const { return !!classLayout; }
};

struct CallSite {
    ValueSite position;
    Opcode opcode { Opcode::Undefined };
    MemberRead memberRead;
    PrototypeMemberRead prototypeMemberRead;
};

MemberRead findMemberRead(FunctionPassContext&, const ValueSite& call);

enum class CallArmKind : uint8_t { OriginalOperations, DirectCall, Undefined };

struct CallArm {
    CallArmKind kind { CallArmKind::OriginalOperations };
    FunctionID closureFunction;
    FunctionID callee;
    ValueID closureCreation;
    bool capturesReceiver { false };
    LayoutSetID receivers;
    MemberConditions conditions;
};

struct ReceiverDispatchPlan {
    MethodKind kind { MethodKind::Method };
    ValueID receiver;
    uint32_t receiverOperandIndex { 0 };
    PropertyKeyID key;
    ValueSplit split;
    Vector<CallArm, 4> arms;
    LayoutSetID restReceivers;
    bool retiresMemberRead { false };
    bool checksReceiverAtRead { false };
};

struct CalleeDispatchPlan {
    ValueID callee;
    CalleeProof proof { CalleeProof::Unknown };
    ValueSplit split;
    Vector<CallArm, 4> arms;
};

// A call of the one function that its callee can be, whose result is what the call gives: no test, no arm, no block.
struct OnlyCalleePlan {
    ValueID callee;
    FunctionID function;
    ValueID closureCreation;
};

struct HolderMemberPlan {
    CallArm arm;
    ValueID thisValue;
    ValueID storedValue;
    bool passesCallArguments { false };
    bool retiresMemberRead { false };
};

struct PrototypeMemberPlan {
    FunctionID method;
    MemberConditions conditions;
};

struct ConstructArm {
    CallArm call;
    LayoutID allocatedClass;
    bool readsConstructPrototype { false };
};

struct ConstructPlan {
    ValueID callee;
    CalleeProof proof { CalleeProof::Unknown };
    ValueSplit split;
    Vector<ConstructArm, 2> arms;
};

struct InstanceOfPlan {
    ValueID subject;
    LayoutSetID instances;
    MemberConditions conditions;
    bool failedTestMeansFalse { false };
    bool isAlwaysFalse { false };
    bool testsPrototypeOfInstance { false };
};

struct BindPlan {
    CallArm arm;
    ValueID closure;
    FunctionID boundFunction;
    uint32_t numberOfCapturedArguments { 0 };
    bool capturesThis { false };
    bool capturesTargetClosure { false };
    // Nobody has said that the closure has its name and its length as it was created with them: the bind asks, and a closure that has not is bound by the engine.
    bool testsThatClosureIsAsCreated { false };
};

struct ListCallPlan {
    unsigned numberOfArguments { 0 };
};

// f(a, b, ...rest): see planRestHandover().
struct RestHandoverPlan {
    CallArm arm;
    ValueID callee;
    CalleeProof proof { CalleeProof::Unknown };
    ValueID rest;
    // The name under which it is spread, which may say more of it.
    ValueID spreadName;
    bool isRestProvenPristine { false };
    // The literal in front of the spread, and whatever names it but the call: casts, checks of a validating build, and what appends the rest array.
    ValueID list;
    Vector<ValueID, 4> operationsOnList;
    // The one operation that appends. None where a test of the rest array chooses between two.
    ValueID append;
};

struct TableCallPlan {
    ValueID receiver;
    MethodSelector selector;
    LayoutSetID receivers;
    // The layouts of the family that find something else under the name than a method that the table can enter: a field, an accessor, nothing.
    LayoutSetID otherReceivers;
    Vector<FunctionID, 4> methods;
    bool checksReceiverAtRead { false };
};

using CallPlan = Variant<ReceiverDispatchPlan, CalleeDispatchPlan, OnlyCalleePlan, HolderMemberPlan, PrototypeMemberPlan, ConstructPlan, InstanceOfPlan, ForwardedCallPlan, BindPlan, ListCallPlan, RestHandoverPlan, TableCallPlan>;

struct PlannedCall {
    CallSite site;
    CallPlan plan;
};

template<typename Plan> using PlanOrDecline = std::expected<Plan, CallDeclineReason>;

struct HomeObject {
    MemberStart start { MemberStart::ClassPrototype };
    LayoutID classLayout;

    explicit operator bool() const { return !!classLayout; }
};

HomeObject findHomeObject(const Program&, const Function&, const Value& superAccess);
MemberAnswer findSuperMember(const PropertyAccessContext&, const HomeObject&, PropertyKeyID, Access);

CallArm createDirectCallArm(FunctionID closureFunction);

// What `value instanceof C` is under tests.
struct TestedInstanceOf {
    bool walksChain { false };
    AccessTests tests;
};

// The forms in the order in which they are worth trying. None where the operation is not known well enough.
Vector<TestedInstanceOf, 2> planTestedInstanceOf(FunctionPassContext&, const Value& instanceOf);
LayoutID classTestedBy(const Program&, const Function&, const Value& instanceOf);
// Of the layouts that the tests have established, those whose objects are instances. Nothing where the tests are about the class object alone.
std::optional<LayoutList> instancesAmongTestedLayouts(const Program&, const Function&, const Value& instanceOf, std::span<const Value> tests, LayoutList& testedLayouts);

class CallResolver {
    WTF_MAKE_NONCOPYABLE(CallResolver);
public:
    // MethodTable: only the calls of methods that would need a test of member conditions or more arms than a dispatch has. They go through the method table.
    enum class Resolution : uint8_t { Everything, MethodTable };

    CallResolver(FunctionPassContext&, Resolution);

    PassOutcome run();

private:
    Vector<CallSite> collectSites();
    bool mayHavePlan(const Value&) const;
    Vector<PlannedCall> planSites(std::span<CallSite>);
    std::optional<SiteContent> contentOf(const CallSite&) const;
    PlanOrDecline<CallPlan> plan(const CallSite&);
    void rewrite(const PlannedCall&);
    void reportDecline(const CallSite&, CallDeclineReason);
    bool reportsDeclinesIn(BlockID);
    CallDeclineReason declineOfUnprovenCallee(const CalleeCandidates&) const;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    PropertyAccessContext accessContext() const { return { m_program, &m_flow }; }
    bool areSameObject(ValueID, ValueID) const;
    const Value* findMethodRead(const CallSite&, Opcode readOpcode) const;
    bool canRetireMemberRead(const CallSite&, FunctionID callee) const;

    void retireMemberRead(const CallSite&, bool checksReceiver, PropertyKeyID);
    void retireReads(const MemberRead&, bool checksReceiver, PropertyKeyID, uint32_t operandIndexInOriginal);
    ValueID appendCall(const CallArm&, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments);
    Vector<ValueID, 8> capturesOf(const CallArm&, ValueID closure, ValueID receiver);
    // Goes on where the conditions hold. Where they do not, the arm ends with the operations as they were.
    void branchOnConditions(std::span<const MemberCondition>, const OriginalCopy&, LayoutID classWhoseGuardsBegin = { });

    PlanOrDecline<CallPlan> planMemberCall(const CallSite&);
    PlanOrDecline<ReceiverDispatchPlan> planReceiverDispatch(const CallSite&, MethodKind);
    std::optional<CallArm> planReceiverArm(const CallSite&, const MemberArm&, MethodKind) const;
    void rewriteReceiverDispatch(const CallSite&, const ReceiverDispatchPlan&);
    void emitReceiverArm(const ReceiverDispatchPlan&, const CallArm&, ValueID receiver, std::span<const ValueID> arguments);

    PlanOrDecline<HolderMemberPlan> planClassObjectMember(const CallSite&);
    PlanOrDecline<HolderMemberPlan> planSuperMember(const CallSite&);
    MemberAnswer findMemberOnPrototypesOfHome(const Value& superAccess, Access);
    PlanOrDecline<HolderMemberPlan> planHolderMember(const CallSite&, const MemberAnswer&, ValueID thisValue);
    void rewriteHolderMember(const CallSite&, const HolderMemberPlan&);
    PlanOrDecline<PrototypeMemberPlan> planPrototypeMember(const CallSite&);
    void rewritePrototypeMember(const CallSite&, const PrototypeMemberPlan&);

    using CanEnterCallee = ScopedLambda<bool(FunctionID)>;
    PlanOrDecline<Vector<FunctionID, 4>> chooseDispatchedCallees(const CallSite&, const CalleeCandidates&, NOESCAPE const CanEnterCallee&);
    ValueSplit createCalleeSplit(const CallSite&, const CalleeCandidates&, std::span<const FunctionID> dispatchedCallees);
    CallArm createCalleeArm(const CalleeCandidates&, FunctionID member) const;

    PlanOrDecline<CalleeDispatchPlan> planCalleeDispatch(const CallSite&);
    std::optional<OnlyCalleePlan> planCallOfOnlyCallee(const CallSite&);
    void rewriteCallOfOnlyCallee(const CallSite&, const OnlyCalleePlan&);
    void rewriteCalleeDispatch(const CallSite&, const CalleeDispatchPlan&);

    PlanOrDecline<ConstructPlan> planConstruct(const CallSite&);
    PlanOrDecline<ConstructArm> planConstructArm(const CallSite&, const CalleeCandidates&, FunctionID constructor);
    void rewriteConstruct(const CallSite&, const ConstructPlan&);
    void emitConstructArm(const ConstructArm&, ValueID closure, std::span<const ValueID> arguments);
    void finishConstruction(ValueID returned, ValueID thisValue);

    PlanOrDecline<InstanceOfPlan> planInstanceOf(const CallSite&);
    void rewriteInstanceOf(const CallSite&, const InstanceOfPlan&);
    void passProofOnBranches(ValueID test, ValueID subject);

    PlanOrDecline<CallPlan> planFunctionMethod(const CallSite&);
    PlanOrDecline<ForwardedCallPlan> planForwardedCall(const CallSite&);
    PlanOrDecline<BindPlan> planBind(const CallSite&, const BuiltinOverload&);
    void rewriteForwardedCall(const CallSite&, const ForwardedCallPlan&);
    void rewriteBind(const CallSite&, const BindPlan&);

    PlanOrDecline<ListCallPlan> planListCall(const CallSite&);
    void rewriteListCall(const CallSite&, const ListCallPlan&);
    PlanOrDecline<RestHandoverPlan> planRestHandover(const CallSite&);
    void rewriteRestHandover(const CallSite&, const RestHandoverPlan&);
    bool findSpreadOfRest(const CallSite&, RestHandoverPlan&);
    bool isRestArrayOfThisFrame(ValueID) const;
    bool isSeenByNobodyElse(const CallSite&, const RestHandoverPlan&);
    bool liesOnEveryLikelyWay(BlockID);
    bool leadsInLineTo(ProgramPoint from, ProgramPoint to, std::optional<std::span<const ValueID>> retired);

    bool mayCallThroughTable(const ValueSite&) const;
    PlanOrDecline<TableCallPlan> planTableCall(const CallSite&);
    void rewriteTableCall(const CallSite&, const TableCallPlan&);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    const FlowResults& m_flow;
    IRBuilder m_builder;
    SiteExpansion m_expansion;
    Resolution m_resolution;
    unsigned m_numberOfResolvedSites { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
