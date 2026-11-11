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
    return { Opcode::RequireBuiltinModule, Opcode::NativeBinding, Opcode::LinkTimeConstant, Opcode::HasJSType, Opcode::FindsDeclaredMember, Opcode::FindsNoMember, Opcode::IsPlainInstanceOf, Opcode::IsPlainHeirOf, Opcode::IsAsBorn, Opcode::IsAsBornExceptForAddedNames, Opcode::IsPrototypeChainPlain, Opcode::GetInternalField, Opcode::PutInternalField };
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
    GPRReg result = emitter.takeResultRegister();
    jit.move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), result);
    if (first <= last) {
        GPRReg tested = StraightEmitter::temporary(1);
        emitter.load(value.valueOperand(), tested);
        CCallHelpers::JumpList hasOtherType;
        hasOtherType.append(jit.branchIfNotCell(tested));
        hasOtherType.append(jit.branchIfNotType(tested, JSTypeRange { first, last }));
        jit.move(CCallHelpers::TrustedImm32(JSValue::ValueTrue), result);
        hasOtherType.link(&jit);
    }
    emitter.define(value, result);
}

static void generatePutInternalField(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg object = StraightEmitter::temporary(0);
    GPRReg stored = StraightEmitter::temporary(1);
    emitter.load(value.objectOperand(), object);
    emitter.load(value.valueOperand(), stored);
    jit.store64(stored, internalFieldInStraightForm(object, value));
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
    case Opcode::FindsNoMember:
    case Opcode::IsPlainInstanceOf:
    case Opcode::IsPlainHeirOf:
    case Opcode::IsAsBorn:
    case Opcode::IsAsBornExceptForAddedNames:
    case Opcode::IsPrototypeChainPlain:
        jit().move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), takeResultRegister());
        define(value);
        return;
    case Opcode::GetInternalField: {
        GPRReg result = loadIntoResultRegister(value.objectOperand());
        jit().load64(internalFieldInStraightForm(result, value), result);
        define(value, result);
        return;
    }
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
