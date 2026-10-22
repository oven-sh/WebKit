/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

#include "AOTNarrowFieldTypes.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_STAMPED_SLOT_OPERATION(v) \
    v(GetSlot) \
    v(GetSlotOrUndefined) \
    v(GetSlotGetter) \
    v(GetFlattenedSlot) \
    v(GetFlattenedSlotOrUndefined) \
    v(GetDeferredClosureSlot) \
    v(GetDeferredClosureSlotOrUndefined) \
    v(IsSlotFilled) \
    v(PutSlot) \
    v(PutSlotOrAdd)

#define AOT_CHECK_SLOT_OPERAND_ORDER(operation) \
    static_assert(OperandIndex::operation::object == 0); \
    static_assert(OperandIndex::operation::guard == 1);
FOR_EACH_AOT_STAMPED_SLOT_OPERATION(AOT_CHECK_SLOT_OPERAND_ORDER)
#undef AOT_CHECK_SLOT_OPERAND_ORDER

static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);

#define AOT_LIST_SLOT_OPERATION(operation) Opcode::operation,
static_assert(Value::opcodesWithFieldAccess == OpcodeSet { FOR_EACH_AOT_STAMPED_SLOT_OPERATION(AOT_LIST_SLOT_OPERATION) });
#undef AOT_LIST_SLOT_OPERATION

#undef FOR_EACH_AOT_STAMPED_SLOT_OPERATION

#if ASSERT_ENABLED
static bool isInsideStoredType(const Program& program, const Type& stored, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    ScratchTypeArena scratch(program.types);
    bool slotsHoldUndefinedAndNull = std::ranges::all_of(receivers, [&](LayoutID receiver) {
        std::optional<FieldReference> field = findField(program, receiver, key, BaseChain::Proven);
        return field && slotHoldsUndefinedAndNull(program, *field);
    });
    return (slotsHoldUndefinedAndNull ? stored.withoutNullish() : stored).isSubsetOf(storedTypeOf(program, receivers, key, scratch), program);
}
#endif

void AccessRewriter::becomeSlotOperation(const NamedAccessSite& site, const AccessOperands& operands)
{
    const Program& program = m_context.program;
    const Function& function = m_context.function;
    const PropertyAccessPlan& plan = site.plan;
    RELEASE_ASSERT(Value::opcodesWithFieldAccess.contains(plan.operation));

    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    if (m_checksEffects)
        replaced.add({ program, m_context.inputs.effects }, function, value);
    SlotStamp stamp = createSlotStamp(m_context, value, plan);

    ASSERT(program.typeOf(function, operands.receiver).isSubsetOf(Type::object(), program));
    ASSERT(operands.guard || program.typeOf(function, operands.receiver).hasOnlyLayouts(program));
    ASSERT(!plan.stores() || isInsideStoredType(program, program.typeOf(function, operands.stored), plan.receivers.span(), stamp.key));

    value.become(plan.operation);
    value.setOperand(OperandIndex::GetSlot::object, operands.receiver);
    value.setOperand(OperandIndex::GetSlot::guard, operands.guard);
    if (plan.stores())
        value.setOperand(OperandIndex::PutSlot::value, operands.stored);
    setSlotStamp(value, stamp);
    if (plan.operation == Opcode::GetSlot || plan.operation == Opcode::GetDeferredClosureSlot)
        value.setHoldsDeclaredType(plan.testsDeclaredType && (plan.requiredProofs.contains(Proof::SlotFilled) || plan.requiredProofs.contains(Proof::BirthFieldsFilled)));
    finish(site.site, replaced);
}

void AccessRewriter::becomeCallOfGetterInSlot(const NamedAccessSite& site, const AccessOperands& operands)
{
    const Program& program = m_context.program;
    const PropertyAccessPlan& plan = site.plan;
    RELEASE_ASSERT(plan.operation == Opcode::GetSlotGetter);

    ReplacedEffects replaced;
    if (m_checksEffects)
        replaced.add({ program, m_context.inputs.effects }, m_context.function, valueAt(site.site));
    SlotStamp stamp = createSlotStamp(m_context, valueAt(site.site), plan);

    Value read = IRBuilder::make(Opcode::GetSlotGetter, { operands.receiver, operands.guard });
    setSlotStamp(read, { stamp.key, stamp.receivers, stamp.level, { } });
    ValueID getter = insert(WTF::move(read), valueAt(site.site).origin());

    Value& call = valueAt(site.site);
    call.become(Opcode::Call);
    call.setCalleeOperand(getter);
    call.setThisValueOperand(operands.receiver);
    call.setAccessSite(stamp.accessSite);
    finish(site.site, replaced);
}

void AccessRewriter::becomeConstant(const NamedAccessSite& site)
{
    const PropertyAccessPlan& plan = site.plan;
    RELEASE_ASSERT(plan.operation == Opcode::Boolean || plan.operation == Opcode::Undefined);

    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    if (m_checksEffects)
        replaced.add({ m_context.program, m_context.inputs.effects }, m_context.function, value);

    value.become(plan.operation);
    if (plan.operation == Opcode::Boolean)
        value.setBoolean(plan.booleanConstant);
    finish(site.site, replaced);
}

void AccessRewriter::becomeBuiltin(const NamedAccessSite& site)
{
    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    if (m_checksEffects)
        replaced.add({ m_context.program, m_context.inputs.effects }, m_context.function, value);

    value.become(Opcode::Builtin);
    value.setBuiltin(site.plan.member.builtin);
    finish(site.site, replaced);
}

void AccessRewriter::becomeMethodClosure(const NamedAccessSite& site, const AccessOperands& operands)
{
    const Program& program = m_context.program;
    const PropertyAccessPlan& plan = site.plan;
    FunctionID method = plan.member.method;
    const Vector<DeclaredType>& captures = program.at(method).captures;
    RELEASE_ASSERT(captures.size() == (plan.capturesReceiver ? 1u : 0u));

    ReplacedEffects replaced;
    if (m_checksEffects)
        replaced.add({ program, m_context.inputs.effects }, m_context.function, valueAt(site.site));

    ValueID capturedReceiver;
    if (plan.capturesReceiver) {
        AnalysisContext analysisContext { m_context.build, program, &m_context.inputs.flow, &m_context.inputs.effects, m_context.id };
        ValueInsertion insertion { analysisContext, m_context.function, m_context.types, valueAt(site.site).origin(), m_insert };
        capturedReceiver = convertTo(insertion, operands.receiver, captures[0]);
    }

    Value& value = valueAt(site.site);
    value.become(Opcode::NewClosure);
    value.setFunction(method);
    // A closure that is only called is nobody's object. Any other use may compare it with what another read of the member gave.
    value.setIsHeldByClosureTable(plan.member.kind == MemberKind::Method && !site.query.isOnlyCalled);
    if (capturedReceiver)
        value.appendVariadicOperand(capturedReceiver);
    finish(site.site, replaced);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
