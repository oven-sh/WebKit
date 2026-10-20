/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static_assert(PayloadWord::mode(Opcode::ElemSet) && PayloadWord::mode(Opcode::PutArrayElement) && !PayloadWord::mode(Opcode::ElemDefine));

static std::optional<ECMAMode> findMode(const Value& access)
{
    if (!PayloadWord::mode(access.opcode))
        return std::nullopt;
    return access.mode();
}

void AccessRewriter::becomeNamedAccess(const ElementAccessSite& site)
{
    const ElementAccessPlan& plan = site.plan;
    Value& value = valueAt(site.site);
    RELEASE_ASSERT(plan.constantKey && plan.operation == namedOperationFor(value.opcode));
    Typed::becomeNamedAccess(value, internKey(m_context.types, plan.constantKey));
    finish(site.site, { });
}

void AccessRewriter::becomeArrayAccess(const ElementAccessSite& site, const AccessOperands& operands)
{
    const ElementAccessPlan& plan = site.plan;
    RELEASE_ASSERT(operands.guard || plan.needsNoTest());

    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    replaced.add({ m_context.program, m_context.inputs.effects }, m_context.function, value);

    ValueID position = value.propertyOperand();
    AccessSiteID accessSite = value.accessSite();
    // A definition throws when it fails, as a strict store does.
    ECMAMode mode = findMode(value).value_or(ECMAMode::Strict);

    value.become(plan.operation);
    value.setArrayOperand(operands.receiver);
    value.setPositionOperand(position);
    value.setGuardOperand(operands.guard);
    value.setArrayStorage(plan.arrayStorage);
    value.setKeyIsIntegral(plan.keyIsIntegral);
    if (Value::opcodesWithIsInBounds.contains(plan.operation))
        value.setIsInBounds(plan.isInBounds);
    if (OperandPosition::value(plan.operation))
        value.setValueOperand(operands.stored);
    if (PayloadWord::mode(plan.operation))
        value.setMode(mode);
    value.setLevel(plan.level);
    value.setAccessSite(accessSite);
    finish(site.site, replaced);
}

void AccessRewriter::becomeTypedArrayAccess(const ElementAccessSite& site, const AccessOperands& operands)
{
    const ElementAccessPlan& plan = site.plan;
    RELEASE_ASSERT(isTypedView(plan.typedArrayType));

    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    replaced.add({ m_context.program, m_context.inputs.effects }, m_context.function, value);

    ValueID position = value.propertyOperand();
    AccessSiteID accessSite = value.accessSite();

    value.become(plan.operation);
    value.setViewOperand(operands.receiver);
    value.setPositionOperand(position);
    value.setTypedArrayType(plan.typedArrayType);
    value.setKeyIsIntegral(plan.keyIsIntegral);
    value.setLevel(plan.level);
    value.setAccessSite(accessSite);
    if (OperandPosition::value(plan.operation))
        value.setValueOperand(operands.stored);
    finish(site.site, replaced);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
