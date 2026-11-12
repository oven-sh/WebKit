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

constexpr StubKey equalsLiteral(ComparedOperand operand)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteral : StubFamily::StringEqualsLiteral);
}

constexpr StubKey equalsCharacters(ComparedOperand operand, unsigned length)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsCharacters : StubFamily::StringEqualsCharacters).withCount(length);
}

constexpr StubKey equalsLiteralOfLength(ComparedOperand operand, unsigned length, CallerFrame frame = CallerFrame::HasFrame)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteralOfLength : StubFamily::StringEqualsLiteralOfLength).withCount(length).withCallerFrame(frame);
}

constexpr StubKey stringSwitchCase(ComparedOperand operand) { return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueSwitchCase : StubFamily::StringSwitchCase); }

constexpr StubKey narrowCharactersOfString() { return StubKey(StubFamily::NarrowCharactersOfString); }

static_assert(bodyOf(equalsLiteral(ComparedOperand::Value)) == equalsLiteral(ComparedOperand::String));
static_assert(bodyOf(equalsCharacters(ComparedOperand::Value, 9)) == equalsCharacters(ComparedOperand::String, 9));
static_assert(exists(equalsCharacters(ComparedOperand::String, StubKey::maxCount)) && exists(equalsCharacters(ComparedOperand::String, 9)) && !exists(equalsCharacters(ComparedOperand::String, 8)));
static_assert(bodyOf(equalsLiteralOfLength(ComparedOperand::Value, 1)) == equalsLiteralOfLength(ComparedOperand::String, 1));
static_assert(exists(equalsLiteralOfLength(ComparedOperand::String, 0)) && exists(equalsLiteralOfLength(ComparedOperand::String, 8)) && !exists(equalsLiteralOfLength(ComparedOperand::String, 9)));
static_assert(exists(narrowCharactersOfString()));
static_assert(bodyOf(stringSwitchCase(ComparedOperand::Value)) == stringSwitchCase(ComparedOperand::String));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
