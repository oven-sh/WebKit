/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPrimitives.h"
#include "AOTRuntimeTable.h"
#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class Equality : uint8_t { Loose, Strict, StrictInFlags };

constexpr StubKey toBoolean() { return StubKey(StubFamily::ToBoolean); }
constexpr StubKey valueIsTruthy() { return StubKey(StubFamily::ValueIsTruthy); }
constexpr StubKey largeValueIsTruthy() { return StubKey(StubFamily::LargeValueIsTruthy); }
constexpr StubKey isCallableCell() { return StubKey(StubFamily::IsCallableCell); }
constexpr StubKey typeofString() { return StubKey(StubFamily::TypeOf); }
constexpr StubKey throwUnlessCallable() { return StubKey(StubFamily::ThrowUnlessCallable); }
constexpr StubKey checkProofOfKinds() { return StubKey(StubFamily::CheckProofOfKinds); }
constexpr StubKey checkProofOfType() { return StubKey(StubFamily::CheckProofOfType); }
constexpr StubKey throwUnlessForwardedCallCanBeMade() { return StubKey(StubFamily::ThrowUnlessForwardedCallCanBeMade); }
constexpr StubKey equal(bool isStrict) { return StubKey(StubFamily::Equal).withForm(static_cast<uint8_t>(isStrict ? Equality::Strict : Equality::Loose)); }
constexpr StubKey strictEqualInFlags() { return StubKey(StubFamily::Equal).withForm(static_cast<uint8_t>(Equality::StrictInFlags)); }

static_assert(exists(toBoolean()) && exists(valueIsTruthy()) && bodyOf(largeValueIsTruthy()) == valueIsTruthy() && exists(isCallableCell()) && exists(typeofString()) && exists(throwUnlessCallable()) && exists(throwUnlessForwardedCallCanBeMade()) && exists(checkProofOfKinds()) && exists(checkProofOfType()));
static_assert(exists(equal(false)) && exists(equal(true)) && exists(strictEqualInFlags()));
static_assert(!exists(equal(true).withForm(static_cast<uint8_t>(Equality::StrictInFlags) + 1)));

constexpr StubSignature signatureWithFlagsResult(std::initializer_list<Reg> operandRegisters, MacroAssembler::ResultCondition trueCondition)
{
    StubSignature signature = StubSignature::withoutResult(operandRegisters);
    signature.result = StubResult::Flags;
    signature.trueCondition = trueCondition;
    return signature;
}

struct KindsOfProof {
    static constexpr uint32_t everyObject = 1u << 16;
    static constexpr uint32_t everyNumber = Typed::Primitive::Integer;
    static constexpr uint32_t primitives = Typed::Primitive::UndefinedOrNull | Typed::Primitive::Boolean | everyNumber | Typed::Primitive::String | Typed::Primitive::Symbol | Typed::Primitive::BigInt | Typed::Primitive::Absent;
    static constexpr uint32_t all = primitives | everyObject;
    static constexpr unsigned shiftOfSource = 17;
    static constexpr unsigned shiftOfNumber = 32;

    static constexpr uint32_t withSource(uint32_t kinds, Typed::ProofSource source) { return kinds | static_cast<uint32_t>(source) << shiftOfSource; }
};
static_assert(!(KindsOfProof::primitives & KindsOfProof::everyObject) && KindsOfProof::all < 1u << KindsOfProof::shiftOfSource);
static_assert(numberOfEnumerators<Typed::ProofSource> <= 1u << (KindsOfProof::shiftOfNumber - KindsOfProof::shiftOfSource));

void callBinaryOperation(StubAssembler&, Entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
