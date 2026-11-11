/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"
#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Typed {

static uint16_t primitivesOfTypeInStraightTest(const TypeArena& types, TypeID tested)
{
    return holdsOnlyPrimitives(tested) ? primitivesInID(tested) : types.recordOf(tested).primitives;
}

static bool tryTestWordInStraightForm(CCallHelpers& jit, GPRReg boxed, TypeID tested, GPRReg zeroOrOne)
{
    if (!holdsOnlyPrimitives(tested))
        return false;
    auto compareWith = [&](int64_t word) {
        jit.compare64(CCallHelpers::Equal, boxed, CCallHelpers::TrustedImm32(static_cast<int32_t>(word)), zeroOrOne);
        return true;
    };
    switch (primitivesInID(tested)) {
    case Primitive::Undefined:
        return compareWith(JSValue::ValueUndefined);
    case Primitive::Null:
        return compareWith(JSValue::ValueNull);
    case Primitive::False:
        return compareWith(JSValue::ValueFalse);
    case Primitive::True:
        return compareWith(JSValue::ValueTrue);
    case Primitive::Absent:
        return compareWith(JSValue::ValueEmpty);
    case Primitive::UndefinedOrNull:
        jit.and64(CCallHelpers::TrustedImm32(~JSValue::UndefinedTag), boxed);
        return compareWith(JSValue::ValueNull);
    case Primitive::Boolean:
        jit.and64(CCallHelpers::TrustedImm32(~1), boxed);
        return compareWith(JSValue::ValueFalse);
    case Primitive::Number:
        if (integersInID(tested) != Range::full())
            return false;
        jit.withNumberTag([&](auto tag) {
            jit.test64(CCallHelpers::NonZero, tag, boxed, zeroOrOne);
        });
        return true;
    default:
        return false;
    }
}

std::optional<StraightTest> StraightEmitter::testOfWordAlone(TypeID tested)
{
    if (!tested || !holdsOnlyPrimitives(tested))
        return std::nullopt;
    switch (primitivesInID(tested)) {
    case Primitive::Undefined:
        return StraightTest { 0, JSValue::ValueUndefined };
    case Primitive::Null:
        return StraightTest { 0, JSValue::ValueNull };
    case Primitive::False:
        return StraightTest { 0, JSValue::ValueFalse };
    case Primitive::True:
        return StraightTest { 0, JSValue::ValueTrue };
    case Primitive::Absent:
        return StraightTest { 0, JSValue::ValueEmpty };
    case Primitive::UndefinedOrNull:
        return StraightTest { ~JSValue::UndefinedTag, JSValue::ValueNull };
    case Primitive::Boolean:
        return StraightTest { ~1, JSValue::ValueFalse };
    default:
        return std::nullopt;
    }
}

bool StraightEmitter::canTestType(TypeID tested)
{
    return recordInTablesTellsType(m_types, tested);
}

void StraightEmitter::testType(GPRReg boxed, TypeID tested, GPRReg zeroOrOne)
{
    if (!canTestType(tested)) {
        fail(Opcode::TypeTest);
        return;
    }
    storeWhatWaitsInRegister();
    CCallHelpers& jit = *m_jit;
    if (tryTestWordInStraightForm(jit, boxed, tested, zeroOrOne))
        return;

    bool absentPasses = primitivesOfTypeInStraightTest(m_types, tested) & Primitive::Absent;
    CCallHelpers::Jump isAbsent;
    if (absentPasses)
        isAbsent = jit.branchTest64(CCallHelpers::Zero, boxed);
    CCallHelpers::JumpList isTested;
    if (m_types.isSubset(TypeTable::number, tested)) {
        jit.withNumberTag([&](auto tag) {
            jit.test64(CCallHelpers::NonZero, tag, boxed, zeroOrOne);
        });
        isTested.append(jit.branchTest32(CCallHelpers::NonZero, zeroOrOne));
        ++m_numberOfNumbersToldByTheirWord;
    }
    StraightArgument arguments[] { StraightArgument::inRegister(boxed), StraightArgument::number(mentions().add(NumberSpace::Type, tested.bits())) };
    callOperation(Origin { }, RuntimeOperation::ValueHasType, arguments, { });
    jit.move(resultOfCall(), zeroOrOne);
    if (absentPasses) {
        isTested.append(jit.jump());
        isAbsent.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(1), zeroOrOne);
    }
    isTested.link(&jit);
}

void StraightEmitter::trapUnlessOfType(ValueID value, TypeID tested, ProofSource source, Origin origin)
{
    if (m_types.isSubset(TypeTable::anything, tested))
        return;
    uint16_t primitives = primitivesOfTypeInStraightTest(m_types, tested);
    if (std::optional<JSValue> constant = value ? constantOf(value) : std::optional<JSValue>(jsUndefined())) {
        if ((constant->isUndefined() && (primitives & Primitive::Undefined)) || (constant->isNull() && (primitives & Primitive::Null)) || (constant->isBoolean() && (primitives & (constant->asBoolean() ? Primitive::True : Primitive::False))))
            return;
    }
    if (value && !isConstant(value) && !m_index.isParameter(rootOf(value))) {
        BodyValue maker = m_index.value(rootOf(value));
        if (maker.opcode() == Opcode::NewObject && m_types.isSubset(m_types.forLayout(maker.as<Opcode::NewObject>().layout()), tested))
            return;
    }
    origin.isTailCall = false;
    GPRReg boxed = temporary(1);
    auto stopByName = [&] {
        AllowMacroScratchRegisterUsage allowScratch(*m_jit);
        emitStraightThrowSite(*m_jit, m_records, ThrowSite { .kind = ThrowKind::ErrorWithMessage, .a = ThrowSiteWord::none, .b = std::nullopt, .sourceOfFalseProof = source, .expectedProofType = FalseProofWord::summarize(primitives, !holdsOnlyPrimitives(tested) && m_types.recordOf(tested).hasObjects()) }, boxed, 0, addSite(origin, { }));
        m_jit->breakpoint();
    };
    storeWhatWaitsInRegister();
    if (!value) {
        m_jit->move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), boxed);
        stopByName();
        return;
    }
    load(value, boxed);
    GPRReg passes = takeResultRegister();
    testType(boxed, tested, passes);
    if (m_hasFailed)
        return;
    CCallHelpers::Jump isOfType = m_jit->branchTest32(CCallHelpers::NonZero, passes);
    load(value, boxed);
    stopByName();
    isOfType.link(m_jit);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
