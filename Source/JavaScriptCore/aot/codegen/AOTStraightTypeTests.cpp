/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static uint16_t primitivesOfTypeInStraightTest(const TypeArena& types, TypeID tested)
{
    return holdsOnlyPrimitives(tested) ? primitivesInID(tested) : types.recordOf(tested).primitives;
}

// The masks that the word alone answers in no more room than a call takes. It may write `boxed`.
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

// Image gives no record to a type of a thread's arena, and the record of a type asks for its TypeArena::testablePartOf().
bool StraightEmitter::canTestType(TypeID tested)
{
    if (!tested || liesInThread(tested))
        return false;
    if (primitivesOfTypeInStraightTest(m_types, tested) & Primitive::Unspecified)
        return false;
    if (holdsOnlyPrimitives(tested))
        return true;
    return m_types.recordOf(tested).isJavaScriptValue() && m_types.testablePartOf(tested) == tested;
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

    // No record holds Absent, and RuntimeLayoutTable::valueHasType() turns the empty word away.
    bool absentPasses = primitivesOfTypeInStraightTest(m_types, tested) & Primitive::Absent;
    CCallHelpers::Jump isAbsent;
    if (absentPasses)
        isAbsent = jit.branchTest64(CCallHelpers::Zero, boxed);
    StraightArgument arguments[] { StraightArgument::inRegister(boxed), StraightArgument::number(mentions().add(NumberSpace::Type, tested.bits())) };
    callOperation(Origin { }, RuntimeOperation::ValueHasType, arguments, { });
    jit.move(resultOfCall(), zeroOrOne);
    if (!absentPasses)
        return;
    CCallHelpers::Jump isTested = jit.jump();
    isAbsent.link(&jit);
    jit.move(CCallHelpers::TrustedImm32(1), zeroOrOne);
    isTested.link(&jit);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
