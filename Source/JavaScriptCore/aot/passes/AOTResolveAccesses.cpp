/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isCalleeOperand(Opcode user, uint32_t operandIndex)
{
    return (user == Opcode::Call && operandIndex == OperandIndex::Call::callee)
        || (user == Opcode::CallList && operandIndex == OperandIndex::CallList::callee);
}

static Effects effectsOfMethodClosure(const PropertyAccessPlan& plan)
{
    Effects effects = declaredBoundOf(Opcode::NewClosure);
    if (plan.capturesReceiver)
        effects.merge(declaredBoundOf(Opcode::Claim));
    return effects;
}

static Effects effectsOfVariableAccess(const Program& program, const PropertyAccessPlan& plan)
{
    Value access(plan.operation);
    access.setVariable(plan.member.variable);
    Effects effects = declaredEffectsOf(program, access);
    if (plan.operation == Opcode::GetModuleVariable)
        effects.merge(declaredBoundOf(Opcode::CheckInitialized));
    return effects;
}

static AccessOperands operandsOfElementAccess(const Value& access)
{
    return { access.objectOperand(), OperandPosition::value(access.opcode) ? access.valueOperand() : ValueID(), { } };
}

static bool canMarkIntegralKey(const Value& access, const ElementAccessPlan& plan)
{
    return plan.keyIsIntegral && !plan.testsKeyIsNumber && FlagMask::keyIsIntegral(access.opcode) && !access.keyIsIntegral();
}

static ElementAccessPlan createIntegralKeyPlan()
{
    ElementAccessPlan byKey;
    byKey.keyIsIntegral = true;
    return byKey;
}

std::optional<NamedAccessSite> planNamedAccessSite(FunctionPassContext& context, const ValueSite& site)
{
    const Value& access = context.function.at(site.value);
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
    if (!query)
        return std::nullopt;

    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    if (access.opcode == Opcode::PropGet)
        query->isOnlyCalled = isOnlyCalled(context.analyses, site.value);

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    return NamedAccessSite { site, WTF::move(*query), WTF::move(plan) };
}

std::optional<ElementAccessSite> planElementAccessSite(FunctionPassContext& context, const ValueSite& site)
{
    std::optional<ElementAccessPlan> plan = planElementAccessOf(context.program, context.function, context.function.at(site.value));
    if (!plan)
        return std::nullopt;
    return ElementAccessSite { site, WTF::move(*plan) };
}

bool isOnlyCalled(FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    const UseLists& uses = analyses.useLists();

    bool isCalled = false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : uses.usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;

            Opcode user = function.at(use.user).opcode;
            if (isCast(user) && !use.operandIndex) {
                worklist.append(use.user);
                continue;
            }
            if (!isCalleeOperand(user, use.operandIndex))
                return false;
            isCalled = true;
        }
    }
    return isCalled;
}

AccessOperands operandsOfNamedAccess(const Value& access)
{
    std::optional<uint8_t> stored = storedOf(access.opcode);
    return { access.operand(*receiverOf(access.opcode)), stored ? access.operand(*stored) : ValueID(), { } };
}

SlotStamp createSlotStamp(FunctionPassContext& context, const Value& access, const PropertyAccessPlan& plan)
{
    return {
        keyOfNamedAccess(context.program, access),
        context.types.completeLayoutSet(plan.receivers.span()),
        plan.level,
        PayloadWord::accessSite(access.opcode) ? access.accessSite() : AccessSiteID(),
    };
}

Effects effectsAfterRewrite(FunctionPassContext& context, const PropertyAccessPlan& plan, const SlotStamp& stamp)
{
    switch (plan.form) {
    case AccessForm::Slot:
        return effectsOfAccessForm(context.program, plan.operation, stamp.key, stamp.receivers);
    case AccessForm::Constant:
        return Effects::none();
    case AccessForm::Builtin:
        return declaredBoundOf(Opcode::Builtin);
    case AccessForm::Method:
        return effectsOfMethodClosure(plan);
    case AccessForm::Variable:
        return effectsOfVariableAccess(context.program, plan);
    case AccessForm::Namespace:
        return declaredBoundOf(Opcode::ModuleNamespace);
    case AccessForm::ByName:
    case AccessForm::Call:
    case AccessForm::Split:
        return Effects();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Effects effectsAfterRewrite(const ElementAccessPlan& plan, const Type& key)
{
    if (!plan.isResolved() || plan.form == ElementAccessForm::NamedKey)
        return Effects();

    // The plan proves that the key is a number. Only the key's type can prove that it is an array index.
    Effects effects = declaredBoundOf(plan.operation);
    refineElementEffects(plan.operation, key, plan.testsInBounds || plan.isInBounds ? ElementPosition::InsideLength : ElementPosition::Anywhere, effects);
    return effects;
}

AccessRewriter::AccessRewriter(FunctionPassContext& context, const InsertValue& insert)
    : m_context(context)
    , m_insert(insert)
{
}

ValueID AccessRewriter::insert(Value&& value, Origin origin)
{
    value.setOrigin(origin);
    return m_insert(WTF::move(value));
}

void AccessRewriter::finish(const ValueSite& site, const ReplacedEffects& replaced)
{
    Value& value = valueAt(site);
    value.type = resultTypeOf(m_context.program, m_context.function, value, m_context.types);
    value.setWritten(writtenTypeOf(m_context.program, m_context.function, value, m_context.types));
    value.setPrediction(predictionOf(m_context.program, m_context.function, value, m_context.types));
    checkRewrite(m_context, site, replaced);
}

void AccessRewriter::rewrite(const NamedAccessSite& site, const AccessOperands& operands)
{
    switch (site.plan.form) {
    case AccessForm::Slot:
        becomeSlotOperation(site, operands);
        break;
    case AccessForm::Constant:
        becomeConstant(site);
        break;
    case AccessForm::Builtin:
        becomeBuiltin(site);
        break;
    case AccessForm::Method:
        becomeMethodClosure(site, operands);
        break;
    case AccessForm::Variable:
        becomeVariableAccess(site, operands);
        break;
    case AccessForm::Namespace:
        becomeNamespace(site);
        break;
    case AccessForm::ByName:
    case AccessForm::Call:
    case AccessForm::Split:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void AccessRewriter::rewrite(const ElementAccessSite& site, const AccessOperands& operands)
{
    switch (site.plan.form) {
    case ElementAccessForm::NamedKey:
        becomeNamedAccess(site);
        return;
    case ElementAccessForm::Array:
        becomeArrayAccess(site, operands);
        return;
    case ElementAccessForm::TypedArray:
        becomeTypedArrayAccess(site, operands);
        return;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::String:
    case ElementAccessForm::Split:
        valueAt(site.site).setKeyIsIntegral(true);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AccessResolver::AccessResolver(FunctionPassContext& context)
    : m_context(context)
    , m_builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, context.function, context.types, &context.analyses)
{
}

PassOutcome AccessResolver::run()
{
    planSites();
    if (m_namedSites.isEmpty() && m_elementSites.isEmpty() && m_readsOfFieldsThatMayBeEmpty.isEmpty())
        return PassOutcome::Unchanged;
    Vector<ValueID> rewritten = rewriteSites();
    expandReadsOfFieldsThatMayBeEmpty();
    // Narrowing may renumber the values, and the planned sites name them.
    if (!rewritten.isEmpty())
        narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_context.function, m_context.analyses, m_context.types, m_context.build }, rewritten.span());
    return PassOutcome::Changed;
}

// Nothing but the state of the slot is open, and either state has its own read: no lookup by name from the receiver is left.
static bool canGoOnAtPrototype(const PropertyAccessPlan& plan)
{
    return plan.emptySlotMeansNoOwnProperty && plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty() && plan.requiredProofs == ProofSet { Proof::SlotFilled };
}

static bool answersWithoutLookup(AccessForm form)
{
    return form == AccessForm::Constant || form == AccessForm::Builtin || form == AccessForm::Method;
}

void AccessResolver::planSites()
{
    forEachValueSite(m_context.analyses, m_context.function, [&](const ValueSite& site) {
        if (std::optional<NamedAccessSite> named = planNamedAccessSite(m_context, site)) {
            if (answersWithoutLookup(named->plan.form) && isTestedByValidation(m_context.function, m_context.analyses.useLists(), site.value))
                return;
            if (named->plan.isResolved() && named->plan.needsNoTest() && named->plan.form != AccessForm::Call)
                m_namedSites.append(WTF::move(*named));
            else if (canGoOnAtPrototype(named->plan))
                m_readsOfFieldsThatMayBeEmpty.append(WTF::move(*named));
            else
                recordLoss(*named);
            return;
        }

        std::optional<ElementAccessSite> element = planElementAccessSite(m_context, site);
        if (!element)
            return;
        if (element->plan.isResolved() && element->plan.needsNoTest()) {
            m_elementSites.append(WTF::move(*element));
            return;
        }
        // A form that needs tests is not rewritten here: the access stays by key and learns what the type of its key proves.
        if (canMarkIntegralKey(m_context.function.at(site.value), element->plan))
            m_elementSites.append(ElementAccessSite { site, createIntegralKeyPlan() });
    });
}

Vector<ValueID> AccessResolver::rewriteSites()
{
    Function& function = m_context.function;
    ProgramPoint current;
    // ScopedLambda passes its arguments on as lvalues.
    auto insertBeforeCurrent = [&](auto&& value) -> ValueID {
        m_builder.setOrigin(value.origin());
        return m_builder.insertBefore(current, WTF::move(value));
    };
    InsertValue insert = insertBeforeCurrent;
    AccessRewriter rewriter(m_context, insert);

    Vector<ValueID> changed;
    changed.reserveInitialCapacity(m_namedSites.size() + m_elementSites.size());
    for (const NamedAccessSite& site : m_namedSites) {
        current = site.site.point();
        rewriter.rewrite(site, operandsOfNamedAccess(function.at(site.site.value)));
        changed.append(site.site.value);
    }
    for (const ElementAccessSite& site : m_elementSites) {
        current = site.site.point();
        rewriter.rewrite(site, operandsOfElementAccess(function.at(site.site.value)));
        if (site.plan.isResolved())
            changed.append(site.site.value);
    }

    m_builder.applyInsertions();
    m_context.analyses.didChangeValues();
    return changed;
}

void AccessResolver::expandReadsOfFieldsThatMayBeEmpty()
{
    if (m_readsOfFieldsThatMayBeEmpty.isEmpty())
        return;

    // The rewrites have moved the reads, and an expansion moves what stands behind it.
    const UseLists& useLists = m_context.analyses.useLists();
    for (NamedAccessSite& read : m_readsOfFieldsThatMayBeEmpty) {
        ValueLocation location = useLists.locationOf(read.site.value);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        read.site.block = location.block;
        read.site.index = location.index;
    }
    std::ranges::sort(m_readsOfFieldsThatMayBeEmpty, std::ranges::greater { }, [](const NamedAccessSite& read) {
        return std::pair { read.site.block, read.site.index };
    });

    SiteExpansion expansion(m_context, m_builder);
    for (const NamedAccessSite& read : m_readsOfFieldsThatMayBeEmpty) {
        SlotStamp stamp = createSlotStamp(m_context, m_context.function.at(read.site.value), read.plan);
        expansion.begin(read.site);
        ValueID object = expansion.original().objectOperand();
        ValueID isFilled = m_builder.append(makeObjectTest(Proof::SlotFilled, object, stamp));
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(isFilled, OperandIndex::IsSlotFilled::object, expansion.frequency(), expansion.frequency());

        m_builder.moveToEndOf(branch.taken);
        Value ownRead = IRBuilder::make(Opcode::GetSlot, { object, branch.provenOnTaken });
        setSlotStamp(ownRead, stamp);
        expansion.finishArm(expansion.append(WTF::move(ownRead)));

        m_builder.moveToEndOf(branch.notTaken);
        Value prototype = IRBuilder::make(Opcode::GetSuperBase, { object });
        prototype.setHomeKind(HomeObjectKind::Object);
        // SuperGet throws where there is no object to look in, and this lookup ends there.
        ValueID hasPrototype = m_builder.append(makeTypeTest(expansion.append(WTF::move(prototype)), TypeTable::object));
        IRBuilder::ProvenBranch chain = m_builder.emitBranchWithProof(hasPrototype, testedOperandIndex, expansion.frequency(), BlockFrequency::Unlikely);

        m_builder.moveToEndOf(chain.taken);
        Value inheritedRead = IRBuilder::make(Opcode::SuperGet, { chain.provenOnTaken, object });
        inheritedRead.setKey(stamp.key);
        expansion.finishArm(expansion.append(WTF::move(inheritedRead)));

        m_builder.moveToEndOf(chain.notTaken);
        expansion.finishArm(m_builder.appendUndefined());
        expansion.end();
    }
    m_context.analyses.didChangeValues();
}

void AccessResolver::recordLoss(const NamedAccessSite& site)
{
    if (site.plan.limit != LimitID::None)
        m_context.gaveUp(site.plan.limit, site.site.value);
}

PassOutcome resolveAccesses(FunctionPassContext& context)
{
    return AccessResolver(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
