/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "JSInternalFieldObjectImpl.h"
#include "LinkTimeConstant.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfRuntimeIntrinsics()
{
    return { Opcode::RequireBuiltinModule, Opcode::NativeBinding, Opcode::LinkTimeConstant, Opcode::HasJSType, Opcode::FindsDeclaredMember, Opcode::GetInternalField, Opcode::PutInternalField };
}

static CCallHelpers::Address internalFieldInStraightForm(GPRReg object, const BodyValue& value)
{
    return CCallHelpers::Address(object, static_cast<int32_t>(JSInternalFieldObjectImpl<>::offsetOfInternalField(value.index())));
}

static void generateHasJSType(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    JSType first = toJSType(value.first());
    JSType last = toJSType(value.last());
    jit.move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), StraightEmitter::resultGPR);
    if (first <= last) {
        GPRReg tested = StraightEmitter::temporary(1);
        emitter.load(value.valueOperand(), tested);
        CCallHelpers::JumpList hasOtherType;
        hasOtherType.append(jit.branchIfNotCell(tested));
        hasOtherType.append(jit.branchIfNotType(tested, JSTypeRange { first, last }));
        jit.move(CCallHelpers::TrustedImm32(JSValue::ValueTrue), StraightEmitter::resultGPR);
        hasOtherType.link(&jit);
    }
    emitter.define(value);
}

static void generatePutInternalField(StraightEmitter& emitter, const BodyValue& value)
{
    GPRReg object = StraightEmitter::temporary(0);
    GPRReg stored = StraightEmitter::temporary(1);
    emitter.load(value.objectOperand(), object);
    emitter.load(value.valueOperand(), stored);
    emitter.jit().store64(stored, internalFieldInStraightForm(object, value));
    if (!emitter.isConstant(value.valueOperand()))
        emitter.barrierAfterStore(object);
    emitter.defineSameAs(value, value.valueOperand());
}

void StraightEmitter::generateRuntimeIntrinsic(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::RequireBuiltinModule:
        callOperation(value, RuntimeOperation::RequireBuiltinModule, { StraightArgument::word(value.runtimeModule().index()) });
        define(value);
        return;
    case Opcode::NativeBinding:
        callOperation(value, RuntimeOperation::NativeBinding, { StraightArgument::word(BuiltinBindings::hostNumberOf(value.builtin()).value_or(0)) });
        define(value);
        return;
    case Opcode::LinkTimeConstant:
        callOperation(value, RuntimeOperation::LinkTimeConstant, { StraightArgument::word(static_cast<uint32_t>(toLinkTimeConstant(value.constant()))) });
        define(value);
        return;
    case Opcode::HasJSType:
        generateHasJSType(*this, value);
        return;
    case Opcode::FindsDeclaredMember:
        jit().move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), resultGPR);
        define(value);
        return;
    case Opcode::GetInternalField:
        load(value.objectOperand(), resultGPR);
        jit().load64(internalFieldInStraightForm(resultGPR, value), resultGPR);
        define(value);
        return;
    case Opcode::PutInternalField:
        generatePutInternalField(*this, value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
