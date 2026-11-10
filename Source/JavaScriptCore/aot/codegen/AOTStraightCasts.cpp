/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTTypeTable.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfCasts()
{
    return inputIdentityOpcodes | OpcodeSet { Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject, Opcode::TypeTest };
}

static ThrowKind throwKindOfStraightForm(NullishAccess access)
{
    switch (access) {
    case NullishAccess::Read:
        return ThrowKind::NullishRead;
    case NullishAccess::Write:
        return ThrowKind::NullishWrite;
    case NullishAccess::Iterate:
        return ThrowKind::NullishIterate;
    case NullishAccess::Spread:
        return ThrowKind::NullishSpread;
    case NullishAccess::Update:
        return ThrowKind::NullishUpdate;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void generateCheckInitialized(StraightEmitter& emitter, const BodyValue& value)
{
    ValueID checked = value.valueOperand();
    if (std::optional<JSValue> constant = emitter.constantOf(checked)) {
        if (!*constant)
            emitter.throwAt(value.origin(), emitter.throwSiteWithName(ThrowKind::UninitializedBinding, value.name()));
        emitter.defineSameAs(value, checked);
        return;
    }
    emitter.throwIfEmpty(value, checked, emitter.throwSiteWithName(ThrowKind::UninitializedBinding, value.name()));
}

static void generateRequireObjectCoercible(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    ValueID required = value.valueOperand();
    GPRReg boxed = StraightEmitter::temporary(1);
    GPRReg withoutUndefinedTag = StraightEmitter::temporary(2);
    emitter.load(required, boxed);
    jit.and64(CCallHelpers::TrustedImm32(~JSValue::UndefinedTag), boxed, withoutUndefinedTag);
    CCallHelpers::Jump isCoercible = jit.branch64(CCallHelpers::NotEqual, withoutUndefinedTag, CCallHelpers::TrustedImm32(JSValue::ValueNull));
    emitter.throwAt(value.origin(), emitter.throwSiteWithName(throwKindOfStraightForm(value.nullishAccess()), value.name()), boxed);
    isCoercible.link(&jit);
    emitter.defineSameAs(value, required);
}

static void generateRequireObject(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    ValueID required = value.valueOperand();
    GPRReg boxed = StraightEmitter::temporary(1);
    emitter.load(required, boxed);
    GPRReg result = emitter.takeResultRegister();
    emitter.testType(boxed, TypeTable::object, result);
    CCallHelpers::Jump isObject = jit.branchTest32(CCallHelpers::NonZero, result);
    emitter.callOperation(value, RuntimeOperation::ThrowRuntimeError, { StraightArgument::word(static_cast<uint32_t>(value.runtimeError())) });
    isObject.link(&jit);
    emitter.defineSameAs(value, required);
}

static void generateTypeTest(StraightEmitter& emitter, const BodyValue& value)
{
    if (std::optional<StraightTest> test = StraightEmitter::testOfWordAlone(value.testedType()); test && emitter.isReadByBranchAlone(value)) {
        emitter.loadIntoResultRegister(value.valueOperand());
        emitter.defineTest(value, *test);
        return;
    }
    GPRReg boxed = StraightEmitter::temporary(1);
    emitter.load(value.valueOperand(), boxed);
    emitter.testType(boxed, value.testedType(), emitter.takeResultRegister());
    emitter.defineBoolean(value);
}

void StraightEmitter::generateCast(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::CheckInitialized:
        generateCheckInitialized(*this, value);
        return;
    case Opcode::RequireObjectCoercible:
        generateRequireObjectCoercible(*this, value);
        return;
    case Opcode::RequireObject:
        generateRequireObject(*this, value);
        return;
    case Opcode::TypeTest:
        generateTypeTest(*this, value);
        return;
    default:
        break;
    }
    if (value.opcode() == Opcode::Convert) {
        DeclaredType declared = value.as<Opcode::Convert>().declared();
        if (declared.has(PositionFlags::IsInt32Promise) || (declared.has(PositionFlags::IsClaim) && !m_index.isUnused(value.id()))) {
            fail(value);
            return;
        }
    }
    if (inputIdentityOpcodes.contains(value.opcode())) {
        defineSameAs(value, value.operand(0));
        return;
    }
    fail(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
