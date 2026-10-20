/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool canObserveDeadZone(const Variable& variable)
{
    return variable.initialization == VariableInitialization::Uninitialized && variable.flow.initialization == InitializationState::Unknown;
}

static Value makeVariableRead(VariableID variable)
{
    Value read = IRBuilder::make(Opcode::GetModuleVariable);
    read.setVariable(variable);
    return read;
}

void AccessRewriter::becomeVariableAccess(const NamedAccessSite& site, const AccessOperands& operands)
{
    const Program& program = m_context.program;
    const PropertyAccessPlan& plan = site.plan;
    VariableID id = plan.member.variable;
    const Variable& variable = program.at(id);

    ReplacedEffects replaced;
    replaced.add({ program, m_context.inputs.effects }, m_context.function, valueAt(site.site));

    if (plan.operation == Opcode::PutModuleVariable) {
        ASSERT(program.typeOf(m_context.function, operands.stored).isSubsetOf(program.at(variable.declared.type), program));
        Value& store = valueAt(site.site);
        store.become(Opcode::PutModuleVariable);
        store.setVariable(id);
        store.setOperand(OperandIndex::PutModuleVariable::value, operands.stored);
        finish(site.site, replaced);
        return;
    }

    RELEASE_ASSERT(plan.operation == Opcode::GetModuleVariable);
    if (!canObserveDeadZone(variable)) {
        Value& read = valueAt(site.site);
        read.become(Opcode::GetModuleVariable);
        read.setVariable(id);
        finish(site.site, replaced);
        return;
    }

    RELEASE_ASSERT(!variable.owner);
    const PropertyKey& exportKey = program.at(site.query.key);
    RELEASE_ASSERT(exportKey.kind == PropertyKeyKind::Text);
    // The ReferenceError of a namespace object names the export that was read, not the binding the export resolves to.
    StringID nameInError = exportKey.text;
    ValueID read = insert(makeVariableRead(id), valueAt(site.site).origin());

    Value& check = valueAt(site.site);
    check.become(Opcode::CheckInitialized);
    check.setOperand(OperandIndex::CheckInitialized::value, read);
    check.setName(nameInError);
    finish(site.site, replaced);
}

void AccessRewriter::becomeNamespace(const NamedAccessSite& site)
{
    Value& value = valueAt(site.site);
    ReplacedEffects replaced;
    replaced.add({ m_context.program, m_context.inputs.effects }, m_context.function, value);

    value.become(Opcode::ModuleNamespace);
    value.setModule(site.plan.member.module);
    value.setPhase(ImportPhase::Evaluation);
    finish(site.site, replaced);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
