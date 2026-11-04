/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class Equality : uint8_t { Loose, Strict };

constexpr StubKey toBoolean() { return StubKey(StubFamily::ToBoolean); }
constexpr StubKey valueIsTruthy() { return StubKey(StubFamily::ValueIsTruthy); }
// Takes any value but the empty one.
constexpr StubKey valueIsCallable() { return StubKey(StubFamily::ValueIsCallable); }
constexpr StubKey throwUnlessCallable() { return StubKey(StubFamily::ThrowUnlessCallable); }
constexpr StubKey throwUnlessForwardedCalleeIsCallable() { return StubKey(StubFamily::ThrowUnlessForwardedCalleeIsCallable); }
constexpr StubKey equal(bool isStrict) { return StubKey(StubFamily::Equal).withForm(static_cast<uint8_t>(isStrict ? Equality::Strict : Equality::Loose)); }

static_assert(exists(toBoolean()) && exists(valueIsTruthy()) && exists(valueIsCallable()) && exists(throwUnlessCallable()) && exists(throwUnlessForwardedCalleeIsCallable()));
static_assert(exists(equal(false)) && exists(equal(true)));
static_assert(!exists(equal(true).withForm(static_cast<uint8_t>(Equality::Strict) + 1)));

constexpr StubSignature signatureWithFlagsResult(std::initializer_list<Reg> operandRegisters, MacroAssembler::ResultCondition trueCondition)
{
    StubSignature signature = StubSignature::withoutResult(operandRegisters);
    signature.result = StubResult::Flags;
    signature.trueCondition = trueCondition;
    return signature;
}

void callBinaryOperation(StubAssembler&, Entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
