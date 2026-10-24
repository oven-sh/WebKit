/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class MemberIntent : uint8_t { Read, Store, Call };

struct MemberUse {
    CallArmKind kind { CallArmKind::OriginalOperations };
    FunctionID callee;
};

struct MemberLookup {
    ValueID firstHolder;
    PropertyKeyID key;
};

} // anonymous namespace

static MemberIntent intentOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropGet:
    case Opcode::SuperGet:
        return MemberIntent::Read;
    case Opcode::PropSet:
    case Opcode::SuperSet:
        return MemberIntent::Store;
    case Opcode::Call:
        return MemberIntent::Call;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool isClassConstructor(const Program& program, FunctionID function)
{
    if (!function)
        return false;
    FunctionKind kind = program.at(function).kind;
    return kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor;
}

static ClassSide sideOf(MemberStart start)
{
    switch (start) {
    case MemberStart::ClassPrototype:
        return ClassSide::Prototype;
    case MemberStart::ClassObject:
        return ClassSide::Static;
    case MemberStart::Instance:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberAnswer findBuiltinClassMember(const PropertyAccessContext& context, BuiltinClassID builtinClass, ClassSide side, PropertyKeyID key, Access access)
{
    switch (side) {
    case ClassSide::Prototype:
        return findBuiltinInstanceMember(context, builtinClass, key, access);
    case ClassSide::Static: {
        BuiltinID constructor = context.program.builtins().at(builtinClass).constructor;
        if (!constructor)
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        return findBuiltinObjectMember(context, constructor, key, access);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberUse accessorUseOf(MemberIntent intent, const MemberAnswer& answer)
{
    switch (intent) {
    case MemberIntent::Read:
        if (!answer.getter)
            return { CallArmKind::Undefined, { } };
        return { CallArmKind::DirectCall, answer.getter };
    case MemberIntent::Store:
        if (!answer.setter)
            return { };
        return { CallArmKind::DirectCall, answer.setter };
    case MemberIntent::Call:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberUse memberUseOf(Opcode opcode, const MemberAnswer& answer)
{
    MemberIntent intent = intentOf(opcode);
    switch (answer.kind) {
    case MemberKind::Method:
        if (intent != MemberIntent::Call)
            return { };
        return { CallArmKind::DirectCall, answer.method };
    case MemberKind::Accessor:
        return accessorUseOf(intent, answer);
    case MemberKind::Absent:
        if (opcode != Opcode::SuperGet)
            return { };
        return { CallArmKind::Undefined, { } };
    case MemberKind::Unknown:
    case MemberKind::Field:
    case MemberKind::Variable:
    case MemberKind::Namespace:
    case MemberKind::Builtin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What the class definitions say of a member holds while the bits of its conditions are set, and one test at the site asks for all of them.
static bool canTestConditions(FunctionPassContext& context, const CallSite& site, const MemberAnswer& answer)
{
    if (answer.needsNoTest())
        return true;
    if (!isMemberOfClass(answer) || !answer.receiverProofs.isEmpty())
        return false;
    // The failing side reads the member again, at the call.
    if (site.opcode == Opcode::Call && !site.memberRead.canSinkToCall && !site.prototypeMemberRead.reads.canSinkToCall)
        return false;

    // Rare code stays small: there the operations give way only to what takes their place without a branch.
    context.planAsksAboutPlace = true;
    FunctionAnalyses& analyses = context.analyses;
    BlockID block = site.position.block;
    if (analyses.blockFrequencies().classOf(block) == BlockFrequency::Rare)
        return false;
    // A bit that was cleared stays cleared, so the operations behind a failed test stay as they are.
    return analyses.findEarlierTest(makeConditionsTest(context.types, answer.conditions.span()), block) != EarlierTest::Failed;
}

static MemberLookup lookupOf(const Value& memberRead)
{
    switch (memberRead.opcode) {
    case Opcode::PropGet:
        return { memberRead.objectOperand(), memberRead.key() };
    case Opcode::SuperGet:
        return { memberRead.baseOperand(), memberRead.key() };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static void appendLookupValidation(IRBuilder& builder, FunctionID method, const MemberLookup& lookup)
{
    Value closure = IRBuilder::make(Opcode::NewClosure);
    closure.setFunction(method);

    Value validation = IRBuilder::make(Opcode::ValidateLookup, { lookup.firstHolder, builder.append(WTF::move(closure)) });
    validation.setKey(lookup.key);
    builder.append(WTF::move(validation));
}

HomeObject findHomeObject(const Program& program, const Function& function, const Value& superAccess)
{
    const Value& superBase = function.at(sameObjectThroughCasts(function, superAccess.baseOperand()));
    if (superBase.opcode != Opcode::GetSuperBase)
        return { };
    LayoutID classLayout = classOfClassObject(program, program.typeOf(function, superBase.homeOperand()));
    if (!classLayout)
        return { };

    switch (superBase.homeKind()) {
    case HomeObjectKind::Object:
        return { MemberStart::ClassObject, classLayout };
    case HomeObjectKind::ClassPrototype:
        if (!isClassConstructor(program, program.at(classLayout).constructor))
            return { };
        return { MemberStart::ClassPrototype, classLayout };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

MemberAnswer findSuperMember(const PropertyAccessContext& context, const HomeObject& home, PropertyKeyID key, Access access)
{
    const Program& program = context.program;
    if (keyLookupOf(program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    ClassSide side = sideOf(home.start);
    MemberAnswer walked;
    NextHolder next = nextHolderOf(program, home.classLayout, side, walked.conditions);

    MemberAnswer found;
    switch (next.step) {
    case ChainStep::Unknown:
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    case ChainStep::CompiledClass:
        found = findMember(context, { home.start, next.layout, key, access });
        break;
    case ChainStep::BuiltinClass:
        found = findBuiltinClassMember(context, next.builtinClass, side, key, access);
        break;
    case ChainStep::NoBase:
        found = findBuiltinInstanceMember(context, side == ClassSide::Static ? BuiltinClasses::Function : BuiltinClasses::Object, key, access);
        break;
    }
    return finishAtChainEnd(WTF::move(walked), WTF::move(found));
}

PlanOrDecline<HolderMemberPlan> CallResolver::planHolderMember(const CallSite& site, const MemberAnswer& answer, ValueID thisValue)
{
    if (!answer.isKnown())
        return std::unexpected(CallDeclineReason::UnknownMember);
    if (!canTestConditions(m_context, site, answer))
        return std::unexpected(CallDeclineReason::NeedsTest);

    HolderMemberPlan plan;
    plan.thisValue = thisValue;

    MemberUse use = memberUseOf(site.opcode, answer);
    switch (use.kind) {
    case CallArmKind::OriginalOperations:
        return std::unexpected(CallDeclineReason::None);
    case CallArmKind::Undefined:
        plan.arm.kind = CallArmKind::Undefined;
        plan.arm.conditions = answer.conditions;
        return plan;
    case CallArmKind::DirectCall:
        break;
    }

    if (!canCallWithoutClosure(m_program, use.callee))
        return std::unexpected(CallDeclineReason::CalleeNeedsClosure);

    switch (intentOf(site.opcode)) {
    case MemberIntent::Read:
        break;
    case MemberIntent::Store:
        plan.storedValue = m_function.at(site.position.value).valueOperand();
        break;
    case MemberIntent::Call:
        if (!canRetireMemberRead(site, use.callee))
            return std::unexpected(CallDeclineReason::None);
        plan.passesCallArguments = true;
        plan.retiresMemberRead = true;
        break;
    }

    plan.arm = createDirectCallArm(use.callee);
    plan.arm.conditions = answer.conditions;
    return plan;
}

PlanOrDecline<HolderMemberPlan> CallResolver::planClassObjectMember(const CallSite& site)
{
    const Value* access = site.opcode == Opcode::Call ? findMethodRead(site, Opcode::PropGet) : &m_function.at(site.position.value);
    if (!access)
        return std::unexpected(CallDeclineReason::None);

    ValueID classObject = access->objectOperand();
    const Type& classObjectType = typeOf(classObject);
    LayoutID classLayout = classOfClassObject(m_program, classObjectType);
    if (!classLayout || classObjectType.mayBeUndefinedOrNull())
        return std::unexpected(CallDeclineReason::None);

    MemberAnswer answer = findMember(accessContext(), { MemberStart::ClassObject, classLayout, access->key(), accessOf(access->opcode) });
    if (answer.kind == MemberKind::Variable)
        return std::unexpected(CallDeclineReason::None);
    return planHolderMember(site, answer, classObject);
}

// The lookup starts at the prototype of an object, not of a class: it finds what a lookup that misses on the object itself goes on to find.
MemberAnswer CallResolver::findMemberOnPrototypesOfHome(const Value& superAccess, Access access)
{
    const Value& superBase = m_function.at(m_context.analyses.identityOf(superAccess.baseOperand()));
    if (superBase.opcode != Opcode::GetSuperBase || superBase.homeKind() != HomeObjectKind::Object)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    ReceiverCandidates homes = receiverCandidates(m_program, m_function, superBase.homeOperand());
    if (!homes.isProven() || homes.mayBeNullish || homes.proven.size() > m_context.limit(LimitID::MaxAccessPlanMembers))
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    MemberArm arm;
    for (LayoutID layout : homes.proven) {
        MemberAnswer found = findMemberOnPrototypes(accessContext(), { MemberStart::Instance, layout, superAccess.key(), access });
        if (arm.layouts.isEmpty())
            arm.answer = found;
        else if (!belongToOneArm(arm.answer, found))
            return MemberAnswer::unknown(UnprovenCause::Unknown);
        mergeIntoArm(arm, layout, found);
    }
    return arm.answer;
}

PlanOrDecline<HolderMemberPlan> CallResolver::planSuperMember(const CallSite& site)
{
    const Value* access = site.opcode == Opcode::Call ? findMethodRead(site, Opcode::SuperGet) : &m_function.at(site.position.value);
    if (!access)
        return std::unexpected(CallDeclineReason::None);

    PropertyKeyID key = access->key();
    if (!key)
        return std::unexpected(CallDeclineReason::None);

    Access lookup = access->opcode == Opcode::SuperSet ? Access::Store : Access::Read;
    HomeObject home = findHomeObject(m_program, m_function, *access);
    MemberAnswer answer = home ? findSuperMember(accessContext(), home, key, lookup) : findMemberOnPrototypesOfHome(*access, lookup);
    if (!home && !answer.isKnown())
        return std::unexpected(CallDeclineReason::None);
    if (answer.kind == MemberKind::Field || answer.kind == MemberKind::Variable)
        return std::unexpected(CallDeclineReason::UnknownMember);
    return planHolderMember(site, answer, access->receiverOperand());
}

// While the condition holds, the property holds the prototype object of the class.
static void addConditionOfPrototypeProperty(const Program& program, LayoutID classLayout, MemberAnswer& answer)
{
    const Layout& layout = program.at(classLayout);
    // No code can change the property of a class.
    if (!layout.has(LayoutFlags::IsConstructorFunction) || layout.classFlow.prototypeProperty == MemberMutability::Fixed)
        return;
    addCondition(answer.conditions, { MemberCondition::Kind::MemberUnchanged, classLayout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    setLevel(answer);
}

PlanOrDecline<PrototypeMemberPlan> CallResolver::planPrototypeMember(const CallSite& site)
{
    const PrototypeMemberRead& read = site.prototypeMemberRead;
    // Both reads go to the side where the conditions fail.
    if (!read || !read.reads.canSinkToCall || !isEvaluatedOnce(m_program, m_program.at(read.classLayout)))
        return std::unexpected(CallDeclineReason::None);
    // The body of a module runs once, and it is where the prototype objects are filled: the class is not to be defined by a read in the middle of that.
    AccessSiteID accessSite = m_function.at(read.reads.read().value).accessSite();
    FunctionID writtenIn = m_flow.hasResults() && accessSite ? m_flow.at(accessSite).function : FunctionID();
    if (writtenIn && m_program.at(writtenIn).isModuleBody())
        return std::unexpected(CallDeclineReason::None);
    MemberAnswer answer = findMember(accessContext(), { MemberStart::ClassPrototype, read.classLayout, read.key, Access::Read });
    if (answer.kind != MemberKind::Method || !m_program.at(answer.method).captures.isEmpty())
        return std::unexpected(CallDeclineReason::None);
    addConditionOfPrototypeProperty(m_program, read.classLayout, answer);
    if (!canTestConditions(m_context, site, answer))
        return std::unexpected(CallDeclineReason::NeedsTest);

    if (site.opcode == Opcode::Call) {
        if (!m_function.at(site.position.value).thisValueOperand() || !canCallWithoutClosure(m_program, answer.method))
            return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
    } else if (!answer.holder || !isEvaluatedOnce(m_program, m_program.at(answer.holder)))
        return std::unexpected(CallDeclineReason::None);
    return PrototypeMemberPlan { answer.method, WTF::move(answer.conditions) };
}

void CallResolver::rewritePrototypeMember(const CallSite& site, const PrototypeMemberPlan& plan)
{
    const PrototypeMemberRead& read = site.prototypeMemberRead;
    bool isCall = site.opcode == Opcode::Call;

    m_expansion.begin(site.position);
    retireReads(read.reads, false, { }, isCall ? OperandIndex::Call::callee : OperandIndex::PropGet::object);
    // The guards of a constructor function begin with the first object that it makes, and many a base makes none.
    branchOnConditions(plan.conditions.span(), { }, m_program.at(read.classLayout).has(LayoutFlags::IsConstructorFunction) ? read.classLayout : LayoutID());

    if (m_context.build.validates()) {
        Value prototype = IRBuilder::make(Opcode::GetClassPrototype, { read.classObject });
        prototype.setLayout(read.classLayout);
        appendLookupValidation(m_builder, plan.method, { m_expansion.append(WTF::move(prototype)), read.key });
    }
    if (!isCall) {
        Value closure = IRBuilder::make(Opcode::NewClosure);
        closure.setFunction(plan.method);
        closure.setIsHeldByClosureTable(true);
        m_expansion.finishArm(m_expansion.append(WTF::move(closure)));
        m_expansion.end();
        return;
    }

    Vector<ValueID, 4> arguments(m_expansion.original().argumentsOperands());
    ValueID result = appendCall(createDirectCallArm(plan.method), m_expansion.original().thisValueOperand(), { }, arguments.span());
    if (typeOf(result).isNever())
        m_expansion.finishArmAsUnreachable();
    else
        m_expansion.finishArm(result);
    m_expansion.end();
}

void CallResolver::rewriteHolderMember(const CallSite& site, const HolderMemberPlan& plan)
{
    std::optional<MemberLookup> validatedLookup;
    if (plan.retiresMemberRead && m_context.build.validates())
        validatedLookup = lookupOf(m_function.at(site.memberRead.read().value));

    m_expansion.begin(site.position);
    if (plan.retiresMemberRead)
        retireMemberRead(site, false, { });
    branchOnConditions(plan.arm.conditions.span(), { });

    Vector<ValueID, 4> arguments;
    if (plan.passesCallArguments)
        arguments.append(m_expansion.original().argumentsOperands());
    else if (plan.storedValue)
        arguments.append(plan.storedValue);

    switch (plan.arm.kind) {
    case CallArmKind::Undefined:
        m_expansion.finishArm(m_builder.appendUndefined());
        break;
    case CallArmKind::DirectCall: {
        if (validatedLookup)
            appendLookupValidation(m_builder, plan.arm.closureFunction, *validatedLookup);
        ValueID result = appendCall(plan.arm, plan.thisValue, { }, arguments.span());
        if (typeOf(result).isNever())
            m_expansion.finishArmAsUnreachable();
        else if (plan.storedValue)
            m_expansion.finishArmWithoutResult();
        else
            m_expansion.finishArm(result);
        break;
    }
    case CallArmKind::OriginalOperations:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
