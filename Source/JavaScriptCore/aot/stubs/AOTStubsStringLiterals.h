/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class ComparedOperand : uint8_t { Value, String };

constexpr StubRegister comparedSubjectRegister = StubRegister::T11;
constexpr StubRegister comparedTextRegister = StubRegister::T9;

constexpr StubKey equalsLiteral(ComparedOperand operand, unsigned length = 0)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteral : StubFamily::StringEqualsLiteral).withCount(length <= StubKey::maxCount ? length : 0);
}

constexpr StubKey equalsLiteralOfLength(ComparedOperand operand, unsigned length, CallerFrame frame = CallerFrame::HasFrame)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteralOfLength : StubFamily::StringEqualsLiteralOfLength).withCount(length).withCallerFrame(frame);
}

constexpr StubKey narrowCharactersOfString() { return StubKey(StubFamily::NarrowCharactersOfString); }

static_assert(bodyOf(equalsLiteral(ComparedOperand::Value)) == equalsLiteral(ComparedOperand::String));
static_assert(bodyOf(equalsLiteral(ComparedOperand::Value, 9)) == equalsLiteral(ComparedOperand::String, 9));
static_assert(exists(equalsLiteral(ComparedOperand::String, StubKey::maxCount)) && equalsLiteral(ComparedOperand::String, StubKey::maxCount + 1) == equalsLiteral(ComparedOperand::String));
static_assert(bodyOf(equalsLiteralOfLength(ComparedOperand::Value, 1)) == equalsLiteralOfLength(ComparedOperand::String, 1));
static_assert(exists(equalsLiteralOfLength(ComparedOperand::String, 0)) && exists(equalsLiteralOfLength(ComparedOperand::String, 8)) && !exists(equalsLiteralOfLength(ComparedOperand::String, 9)));
static_assert(exists(narrowCharactersOfString()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
