/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
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
    auto fetchByOperation = [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::LinkTimeConstant, { word32(constant) }, options);
    };
    if (value.shape() == CodeShape::Compact) {
        defineAs(value, boxCell(fetchByOperation({ })), typeOf(value.id()));
        return;
    }

    Integer fetched(m_out.load64(mutableInstanceField(AOT_INSTANCE_OFFSET(realm.linkTimeConstants) + constant * sizeof(EncodedJSValue))));
    defineAs(value, boxCell(either<CellPointer>(isNotZero(fetched), [&] {
        return CellPointer(fetched.value());
    }, [&] {
        return fetchByOperation({ .isOnRarePath = true });
    })), typeOf(value.id()));
}

Condition RuntimeIntrinsicEmitter::hasJSType(const Value& value, CellPointer cell)
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

// Where a pass knew the member, tests of the class stand in the place of the operation. What is left is a place where nobody knows.
void RuntimeIntrinsicEmitter::generateFindsDeclaredMember(const ValueContext& value)
{
    if (value.isBranchCondition()) {
        branch(boolean(false), value.branchTargets());
        return;
    }
    defineAs(value, boolean(false), m_program.at(TypeTable::boolean));
}

TypedPointer RuntimeIntrinsicEmitter::internalField(const Value& value)
{
    return m_out.address(cellOperand(value.objectOperand()).value(), m_heaps.JSInternalFieldObjectImpl_internalFields[value.index()]);
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
    barrierAfterStore(value, cellOperand(value->objectOperand()), get(stored), typeOf(stored));
    defineAs(value, get(stored), typeOf(stored));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
