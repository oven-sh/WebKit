/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTStatistics.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueID findLiteralList(const Function& function, const UseLists& useLists, const Value& construct)
{
    ValueID id = construct.operand(OperandIndex::SuperConstructList::list);
    while (useLists.singleUse(id) && function.at(id).opcode == Opcode::Upcast)
        id = function.at(id).operand(OperandIndex::Upcast::value);

    const Value& list = function.at(id);
    if (!useLists.singleUse(id) || list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand())
        return { };
    return id;
}

static Vector<ValueID, 8> collectConstructArguments(const Function& function, const Value& construct)
{
    if (construct.opcode != Opcode::SuperConstructList)
        return Vector<ValueID, 8>(construct.variadicOperands());

    const Value& list = function.at(definitionThroughUpcasts(function, construct.operand(OperandIndex::SuperConstructList::list)));
    RELEASE_ASSERT(list.opcode == Opcode::NewArray);
    return Vector<ValueID, 8>(list.variadicOperands());
}

static bool allocatesOwnThis(const Function& constructor)
{
    switch (constructor.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return !constructor.has(FunctionFlags::HasThis);
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The run time reads the prototype property at the first allocation of a site. An assigned prototype object is stored before every read of the property, and a construction is one.
static bool hasOnePrototypeObject(const Program& program, LayoutID layout)
{
    const Layout& constructed = program.at(layout);
    const ClassFlow& classFlow = constructed.classFlow;
    return isEvaluatedOnce(program, constructed) && (classFlow.prototypeProperty == MemberMutability::Fixed || classFlow.hasAssignedPrototype);
}

static DirectCallRequest createConstructionRequest(FunctionID constructor, ValueID newTarget, std::span<const ValueID> arguments)
{
    DirectCallRequest request;
    request.callee = constructor;
    request.newTarget = newTarget;
    request.arguments = arguments;
    request.isConstruction = true;
    return request;
}

// The layout of a constructor function is allocated for that function as new.target alone, and the run time keeps it under whatever prototype the function has.
static bool allocatesExactClass(const Program& program, const ConstructArm& arm)
{
    return !arm.readsConstructPrototype
        || program.at(arm.call.callee).role == FunctionRole::AllocatedThisVersion
        || program.at(declaredLayoutOf(program, arm.allocatedClass)).has(LayoutFlags::IsConstructorFunction);
}

static ValueID appendThis(IRBuilder& builder, const ConstructArm& arm, ValueID newTarget)
{
    const Program& program = builder.context().program;
    ValueID prototype;
    if (arm.readsConstructPrototype) {
        Value read = IRBuilder::make(Opcode::GetConstructPrototype, { newTarget });
        read.setLayout(declaredLayoutOf(program, arm.allocatedClass));
        prototype = builder.append(WTF::move(read));
    }

    Value object = IRBuilder::make(Opcode::NewObject, { prototype });
    object.setLayout(arm.allocatedClass);
    const Vector<AllocationClassID>& allocationClasses = program.at(arm.allocatedClass).flow.allocationClasses;
    if (allocationClasses.size() == 1)
        object.setAllocationClass(allocationClasses.first());
    object.setKeepsShape(allocatesExactClass(program, arm) && receiverStateOf(program, arm.allocatedClass).cannot(ObjectMutation::LoseShape));
    return builder.append(WTF::move(object));
}

static bool isClassConstructor(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

// new.target selects what is allocated, and the allocation is handed its prototype object alone. That object names a class, whose prototype property nobody can store to.
// What a function has as its prototype names nothing, so a function may be new.target here only where its own layout is allocated.
static bool isAlwaysClass(const Program& program, const Type& newTarget)
{
    if (!newTarget.isObject() || !newTarget.hasOnlyKnownCallees() || newTarget.layouts || !newTarget.builtins.isEmpty())
        return false;
    return std::ranges::all_of(newTarget.functions, [&](FunctionID function) {
        return isClassConstructor(program.at(function));
    });
}

// The run time exchanges the object for an instance of the class that new.target selects, so the class that holds the super call stands for all of them.
// A class that nobody constructs itself has no placement: the one class below it that has instances is allocated in its place, and with several the run time chooses.
static LayoutID findClassAllocatedBySuperCall(const Program& program, LayoutID holder)
{
    if (program.at(holder).flow.allocation != AllocationState::NeverAllocated)
        return holder;
    std::span<const LayoutID> allocatedBelow = program.layoutsBelow(holder);
    return allocatedBelow.size() == 1 ? allocatedBelow[0] : LayoutID();
}

static uint32_t constructorOperandIndexOf(Opcode construct)
{
    return construct == Opcode::Construct ? *OperandPosition::callee(construct) : *OperandPosition::parent(construct);
}

PlanOrDecline<ConstructArm> CallResolver::planConstructArm(const CallSite& site, const CalleeCandidates& candidates, FunctionID member)
{
    const Value& construct = m_function.at(site.position.value);
    bool isSuperCall = site.opcode != Opcode::Construct;
    ValueID callee = construct.operand(constructorOperandIndexOf(site.opcode));

    ConstructArm arm;
    arm.call = createDirectCallArm(m_program, m_flow, member, construct.callees());
    arm.call.closureCreation = findClosureCreation(m_function, candidates.value, member);

    ValueID newTarget = construct.newTargetOperand();
    bool isOwnNewTarget = !isSuperCall && (!newTarget || areSameObject(newTarget, callee));
    LayoutID constructed = m_program.at(member).constructedClass;
    if (!allocatesOwnThis(m_program.at(arm.call.callee))) {
        if (!(isSuperCall ? construct.layout() : constructed))
            return std::unexpected(CallDeclineReason::CalleeNotCallable);
        if (!isOwnNewTarget && !(isSuperCall && isAlwaysClass(m_program, typeOf(newTarget))))
            return std::unexpected(CallDeclineReason::None);
        arm.allocatedClass = isSuperCall ? findClassAllocatedBySuperCall(m_program, construct.layout()) : constructed;
        if (!arm.allocatedClass)
            return std::unexpected(CallDeclineReason::None);
        if (LayoutID derived = isSuperCall ? LayoutID() : construct.allocatedLayout()) {
            RELEASE_ASSERT(declaredLayoutOf(m_program, derived) == constructed);
            arm.allocatedClass = derived;
        }
    } else if (isOwnNewTarget && canTakeAllocatedThis(m_program, arm.call.callee)) {
        arm.allocatedClass = layoutAllocatedBy(m_program, arm.call.callee);
        arm.call.callee = m_context.derivedFunction(derivedFunctionKey(FunctionRole::AllocatedThisVersion, arm.call.callee));
    }
    arm.readsConstructPrototype = arm.allocatedClass && !(isOwnNewTarget && hasOnePrototypeObject(m_program, declaredLayoutOf(m_program, arm.allocatedClass)));
    return arm;
}

PlanOrDecline<ConstructPlan> CallResolver::planConstruct(const CallSite& site)
{
    const Value& construct = m_function.at(site.position.value);
    if (site.opcode == Opcode::SuperConstructList && !findLiteralList(m_function, m_context.analyses.useLists(), construct))
        return std::unexpected(CallDeclineReason::None);

    ValueID callee = construct.operand(constructorOperandIndexOf(site.opcode));
    CalleeCandidates candidates = calleeCandidates(m_program, m_flow, m_function, callee, construct.callees(), construct.accessSite());
    requireArmsForCallSiteVersions(m_program, candidates, construct.callees());
    if (LayoutID named = site.opcode == Opcode::Construct ? construct.allocatedLayout() : LayoutID())
        requireArmFor(candidates, m_program.at(declaredLayoutOf(m_program, named)).constructor);
    if (!candidates.isProven())
        return std::unexpected(declineOfUnprovenCallee(candidates));

    PlanOrDecline<Vector<FunctionID, 4>> members = chooseDispatchedCallees(site, candidates, [&](FunctionID member) {
        return canConstructDirectly(m_program, member);
    });
    if (!members)
        return std::unexpected(members.error());

    // A constructor without an arm is constructed by the original operation, with whatever else the value may be.
    ConstructPlan plan;
    Vector<FunctionID, 4> armConstructors;
    CallDeclineReason decline = CallDeclineReason::None;
    for (FunctionID member : *members) {
        PlanOrDecline<ConstructArm> arm = planConstructArm(site, candidates, member);
        if (!arm) {
            decline = arm.error();
            continue;
        }
        plan.arms.append(WTF::move(*arm));
        armConstructors.append(member);
    }
    if (plan.arms.isEmpty())
        return std::unexpected(decline);

    plan.split = createCalleeSplit(site, candidates, armConstructors.span(), construct.callees());
    return plan;
}

void CallResolver::emitConstructArm(const ConstructArm& arm, ValueID closure, std::span<const ValueID> arguments)
{
    const Value& original = m_expansion.original();
    ValueID newTarget = original.newTargetOperand() ? original.newTargetOperand() : closure;
    ValueID thisValue = arm.allocatedClass ? appendThis(m_builder, arm, newTarget) : ValueID();
    Vector<ValueID, 8> captures = capturesOf(arm.call, closure, { });

    DirectCallRequest request = createConstructionRequest(arm.call.callee, newTarget, arguments);
    request.captures = captures.span();
    request.thisValue = thisValue;
    if (m_program.at(arm.call.callee).has(FunctionFlags::TakesCallee))
        request.calleeValue = closure;
    request.accessSite = original.accessSite();
    request.resultCheck = arm.call.resultCheck;
    ValueID returned = appendResultCheck(m_builder, appendDirectCall(m_builder, request));

    if (arm.allocatedClass)
        finishConstruction(returned, thisValue);
    else if (typeOf(returned).isNever())
        m_expansion.finishArmAsUnreachable();
    else
        m_expansion.finishArm(returned);
}

void CallResolver::rewriteConstruct(const CallSite& site, const ConstructPlan& plan)
{
    m_expansion.begin(site.position);
    Vector<ValueID, 8> arguments = collectConstructArguments(m_function, m_expansion.original());
    LayoutID namedLayout = site.opcode == Opcode::Construct ? m_expansion.original().allocatedLayout() : LayoutID();
    bool namedLayoutHasArm = namedLayout && std::ranges::any_of(plan.arms, [&](const ConstructArm& arm) {
        return arm.allocatedClass == namedLayout;
    });
    emitSplit(m_expansion, plan.split.split,
        [&](uint32_t armIndex, ValueID narrowed) {
            emitConstructArm(plan.arms[armIndex], narrowed, arguments.span());
        },
        [&](ValueID narrowed) {
            ValueID rest = m_expansion.appendOriginal({ constructorOperandIndexOf(site.opcode), narrowed, { }, plan.split.restCallees });
            // The constructor of the layout is not among what is left.
            if (namedLayoutHasArm)
                m_function.at(rest).setAllocatedLayout({ });
            m_expansion.finishArm(rest);
        });
    m_expansion.end();
    m_context.build.statistics().count(StatisticID::ConstructsResolved);
}

void CallResolver::finishConstruction(ValueID returned, ValueID thisValue)
{
    const Type& returnedType = typeOf(returned);
    if (returnedType.isNever()) {
        m_expansion.finishArmAsUnreachable();
        return;
    }
    if (!returnedType.hasObjects()) {
        m_expansion.finishArm(thisValue);
        return;
    }

    BlockFrequency frequency = m_expansion.frequency();
    ValueID isObject = m_builder.appendTypeTest(returned, Type::object());
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(isObject, 0, rarerOf(frequency, BlockFrequency::Unlikely), frequency);
    m_expansion.finishArm(branch.provenOnTaken);
    m_builder.moveToEndOf(branch.notTaken);
    m_expansion.finishArm(thisValue);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
