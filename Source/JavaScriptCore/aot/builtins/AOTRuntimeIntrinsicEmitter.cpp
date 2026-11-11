/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTConstantEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTInstance.h"
#include "AOTTypeTable.h"
#include "LinkTimeConstant.h"

namespace JSC { namespace AOT { namespace Typed {

RuntimeIntrinsicEmitter::RuntimeIntrinsicEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void RuntimeIntrinsicEmitter::generateRequireBuiltinModule(const ValueContext& value)
{
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::RequireBuiltinModule, { word32(value->runtimeModule().index()) }), typeOf(value.id()));
}

void RuntimeIntrinsicEmitter::generateNativeBinding(const ValueContext& value)
{
    defineAs(value, callOperation<Boxed>(value, RuntimeOperation::NativeBinding, { word32(BuiltinBindings::hostNumberOf(value->builtin()).value_or(0)) }), typeOf(value.id()));
}

void RuntimeIntrinsicEmitter::generateLinkTimeConstant(const ValueContext& value)
{
    uint32_t constant = static_cast<uint32_t>(toLinkTimeConstant(value->constant()));
    Integer fetched(m_out.load64(instanceField(m_heaps.AOTInstance_fieldsFilledOnce, AOT_INSTANCE_OFFSET(realm.linkTimeConstants) + constant * sizeof(EncodedJSValue))));
    defineAs(value, boxCell(either<CellPointer>(isNotZero(fetched), [&] {
        return CellPointer(fetched.value());
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::LinkTimeConstant, { word32(constant) }, { .isOnRarePath = true });
    })), typeOf(value.id()));
}

Condition RuntimeIntrinsicEmitter::hasJSType(const Operation& value, CellPointer cell)
{
    JSType first = toJSType(value.first());
    JSType last = toJSType(value.last());
    if (first > last)
        return boolean(false);
    return hasCellTypeIn(cell, { first, last });
}

void RuntimeIntrinsicEmitter::generateHasJSType(const ValueContext& value)
{
    Boxed operand = boxedOperand(value, value->valueOperand());
    if (value.isBranchCondition()) {
        const BranchTargets& targets = value.branchTargets();
        LBasicBlock cellCase = newBlock();
        branch(isCell(operand), { normalEdge(cellCase), targets.notTaken });
        m_out.appendTo(cellCase);
        branch(hasJSType(value.value(), unboxCell(operand)), targets);
        return;
    }

    defineAs(value, either<Condition>(isCell(operand), [&] {
        return hasJSType(value.value(), unboxCell(operand));
    }, [&] {
        return boolean(false);
    }), m_program.at(TypeTable::boolean));
}

void RuntimeIntrinsicEmitter::generateFindsDeclaredMember(const ValueContext& value)
{
    if (value->receivers()) {
        emitter<GuardEmitter>().defineWhetherDeclaredMemberIsFound(value);
        return;
    }
    if (value.isBranchCondition()) {
        branch(boolean(false), value.branchTargets());
        return;
    }
    defineAs(value, boolean(false), m_program.at(TypeTable::boolean));
}

void RuntimeIntrinsicEmitter::generateFindsNoMember(const ValueContext& value)
{
    if (value->receivers() || value->asksObjectByInquiry()) {
        emitter<GuardEmitter>().defineWhetherNoMemberIsFound(value);
        return;
    }
    if (value.isBranchCondition()) {
        branch(boolean(false), value.branchTargets());
        return;
    }
    defineAs(value, boolean(false), m_program.at(TypeTable::boolean));
}

void RuntimeIntrinsicEmitter::generateIsPlainHeirOf(const ValueContext& value)
{
    if (value.isBranchCondition()) {
        branch(boolean(false), value.branchTargets());
        return;
    }
    defineAs(value, boolean(false), m_program.at(TypeTable::boolean));
}

void RuntimeIntrinsicEmitter::generateIsPrototypeChainPlain(const ValueContext& value)
{
    if (!value->conditions().isEmpty()) {
        emitter<GuardEmitter>().defineWhetherPrototypeChainIsPlain(value);
        return;
    }
    generateIsPlainHeirOf(value);
}

void RuntimeIntrinsicEmitter::generateIsAsBorn(const ValueContext& value)
{
    generateIsPlainHeirOf(value);
}

void RuntimeIntrinsicEmitter::generateIsAsBornExceptForAddedNames(const ValueContext& value)
{
    generateIsPlainHeirOf(value);
}

void RuntimeIntrinsicEmitter::generateIsPlainInstanceOf(const ValueContext& value)
{
    if (value->receivers()) {
        emitter<GuardEmitter>().defineWhetherObjectIsPlainInstance(value);
        return;
    }
    if (value.isBranchCondition()) {
        branch(boolean(false), value.branchTargets());
        return;
    }
    defineAs(value, boolean(false), m_program.at(TypeTable::boolean));
}

TypedPointer RuntimeIntrinsicEmitter::internalField(const Operation& value)
{
    CellPointer object = cellOperand(value.objectOperand());
    static_assert(JSPromiseType + 1 == JSMapType && JSMapType + 1 == JSSetType);
    if (build().options().recorded.validates) [[unlikely]] {
        m_out.check(hasCellTypeIn(object, { JSPromiseType, JSSetType }).value())->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters&) {
            jit.breakpoint();
        });
    }
    return m_out.address(object.value(), m_heaps.JSInternalFieldObjectImpl_internalFields[value.index()]);
}

void RuntimeIntrinsicEmitter::generateGetInternalField(const ValueContext& value)
{
    defineAs(value, Boxed(m_out.load64(internalField(value.value()))), typeOf(value.id()));
}

void RuntimeIntrinsicEmitter::generatePutInternalField(const ValueContext& value)
{
    ValueID stored = value->valueOperand();
    Boxed boxedValue = boxedOperand(value, stored);
    m_out.store64(boxedValue.value(), internalField(value.value()));
    CellPointer object = cellOperand(value->objectOperand());
    barrierAfterStore(value, object, typeOf(stored));
    if (m_code.useCountOf(value.id()))
        defineAs(value, get(stored), typeOf(stored));
    else
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
