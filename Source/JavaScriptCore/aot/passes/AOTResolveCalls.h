/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTMethodDispatch.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include <expected>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct BuiltinOverload;

namespace Typed {

bool canTakeAllocatedThis(const Program&, FunctionID constructor);
// The class of the constructor, or the layout derived from it that this version of the constructor allocates.
LayoutID layoutAllocatedBy(const Program&, FunctionID constructor);

struct MemberRead {
    Vector<ValueSite, 2> links;
    bool canSinkToCall { false };

    bool isEmpty() const { return links.isEmpty(); }
    const ValueSite& read() const LIFETIME_BOUND { return links.first(); }
};

struct CallSite {
    ValueSite position;
    Opcode opcode { Opcode::Undefined };
    MemberRead memberRead;
};

MemberRead findMemberRead(FunctionPassContext&, const ValueSite& call);

enum class CallArmKind : uint8_t { OriginalOperations, DirectCall, Undefined };

struct CallArm {
    CallArmKind kind { CallArmKind::OriginalOperations };
    FunctionID closureFunction;
    FunctionID callee;
    ResultCheck resultCheck { ResultCheck::ByCallee };
    ValueID closureCreation;
    bool capturesReceiver { false };
    LayoutSetID receivers;
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

struct CalleeSplit {
    ValueSplit split;
    CalleeSetID restCallees;
};

struct CalleeDispatchPlan {
    ValueID callee;
    CalleeSplit split;
    Vector<CallArm, 4> arms;
};

struct HolderMemberPlan {
    CallArm arm;
    ValueID thisValue;
    ValueID storedValue;
    bool passesCallArguments { false };
    bool retiresMemberRead { false };
};

struct ConstructArm {
    CallArm call;
    LayoutID allocatedClass;
    bool readsConstructPrototype { false };
};

struct ConstructPlan {
    CalleeSplit split;
    Vector<ConstructArm, 2> arms;
};

struct InstanceOfPlan {
    ValueID subject;
    LayoutTestKind test { LayoutTestKind::Shape };
    LayoutSetID instances;
    bool failedTestMeansFalse { false };
    bool isAlwaysFalse { false };
};

struct BindPlan {
    CallArm arm;
    ValueID closure;
    FunctionID boundFunction;
    uint32_t numberOfCapturedArguments { 0 };
    bool capturesThis { false };
    bool capturesTargetClosure { false };
};

struct MethodPredictionPlan {
    FunctionID method;
};

struct ListCallPlan {
    unsigned numberOfArguments { 0 };
};

using CallPlan = Variant<ReceiverDispatchPlan, CalleeDispatchPlan, HolderMemberPlan, ConstructPlan, InstanceOfPlan, ForwardedCallPlan, BindPlan, MethodPredictionPlan, ListCallPlan>;

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

CallArm createDirectCallArm(const Program&, const FlowResults&, FunctionID closureFunction, CalleeSetID stamp);

// What `value instanceof C` is under tests. The tests of the class object alone leave the walk up the chain of the value. Where the value is also tested for the layouts it is expected to have,
// each under the prototype object of its class, the layout answers.
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
    explicit CallResolver(FunctionPassContext&);

    PassOutcome run();

private:
    Vector<CallSite> collectSites();
    Vector<PlannedCall> planSites(std::span<const CallSite>);
    PlanOrDecline<CallPlan> plan(const CallSite&);
    void rewrite(const PlannedCall&);
    void reportDecline(const CallSite&, CallDeclineReason);
    CallDeclineReason declineOfUnprovenCallee(const CalleeCandidates&) const;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    PropertyAccessContext accessContext() const { return { m_program, &m_flow }; }
    bool areSameObject(ValueID, ValueID) const;
    const Value* findMethodRead(const CallSite&, Opcode readOpcode) const;
    bool canRetireMemberRead(const CallSite&, FunctionID callee) const;

    void retireMemberRead(const CallSite&, bool checksReceiver, PropertyKeyID);
    ValueID appendCall(const CallArm&, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments);
    Vector<ValueID, 8> capturesOf(const CallArm&, ValueID closure, ValueID receiver);

    PlanOrDecline<CallPlan> planMemberCall(const CallSite&);
    PlanOrDecline<ReceiverDispatchPlan> planReceiverDispatch(const CallSite&, MethodKind);
    std::optional<CallArm> planReceiverArm(const CallSite&, const MemberArm&, MethodKind) const;
    void rewriteReceiverDispatch(const CallSite&, const ReceiverDispatchPlan&);
    void emitReceiverArm(const ReceiverDispatchPlan&, const CallArm&, ValueID receiver, std::span<const ValueID> arguments);
    void rewriteMethodPrediction(const CallSite&, const MethodPredictionPlan&);

    PlanOrDecline<HolderMemberPlan> planClassObjectMember(const CallSite&);
    PlanOrDecline<HolderMemberPlan> planSuperMember(const CallSite&);
    MemberAnswer findMemberOnPrototypesOfHome(const Value& superAccess, Access);
    PlanOrDecline<HolderMemberPlan> planHolderMember(const CallSite&, const MemberAnswer&, ValueID thisValue);
    void rewriteHolderMember(const CallSite&, const HolderMemberPlan&);

    using CanEnterCallee = ScopedLambda<bool(FunctionID)>;
    PlanOrDecline<Vector<FunctionID, 4>> chooseDispatchedCallees(const CallSite&, const CalleeCandidates&, NOESCAPE const CanEnterCallee&);
    CalleeSplit createCalleeSplit(const CallSite&, const CalleeCandidates&, std::span<const FunctionID> dispatchedCallees, CalleeSetID stamp);

    PlanOrDecline<CalleeDispatchPlan> planCalleeDispatch(const CallSite&);
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

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    const FlowResults& m_flow;
    IRBuilder m_builder;
    SiteExpansion m_expansion;
    unsigned m_numberOfResolvedSites { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
