/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

// Value: any boxed value, and the stub tests for a string first. String: a string cell.
enum class ComparedOperand : uint8_t { Value, String };

constexpr StubRegister comparedSubjectRegister = StubRegister::T11;
constexpr StubRegister comparedTextRegister = StubRegister::T9;

// The immediate of equalsLiteral and looseEqualsLiteral is the index of the literal's StringID, not the number of a string constant.
constexpr StubKey equalsLiteral(ComparedOperand operand)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteral : StubFamily::StringEqualsLiteral);
}

// The second operand is the text as packStringIntoRegister packs it. The result is in the flags.
constexpr StubKey equalsLiteralOfLength(ComparedOperand operand, unsigned length, CallerFrame frame = CallerFrame::HasFrame)
{
    return StubKey(operand == ComparedOperand::Value ? StubFamily::ValueEqualsLiteralOfLength : StubFamily::StringEqualsLiteralOfLength).withCount(length).withCallerFrame(frame);
}

// ToNumber of the literal is NaN, so no number, boolean or BigInt is loosely equal to it. The stub does not test that.
constexpr StubKey looseEqualsLiteral() { return StubKey(StubFamily::LooseEqualsLiteral); }
constexpr StubKey literalStringToString() { return StubKey(StubFamily::LiteralStringToString); }
// The operand is a string cell or no cell. The stub does not test the type of a cell.
constexpr StubKey toLiteralString() { return StubKey(StubFamily::ToLiteralString); }
constexpr StubKey shortStringOfValue() { return StubKey(StubFamily::ShortStringOfValue); }

static_assert(bodyOf(equalsLiteral(ComparedOperand::Value)) == equalsLiteral(ComparedOperand::String));
static_assert(bodyOf(equalsLiteralOfLength(ComparedOperand::Value, 1)) == equalsLiteralOfLength(ComparedOperand::String, 1));
static_assert(!exists(equalsLiteralOfLength(ComparedOperand::String, 0)) && exists(equalsLiteralOfLength(ComparedOperand::String, 8)) && !exists(equalsLiteralOfLength(ComparedOperand::String, 9)));
static_assert(exists(looseEqualsLiteral()) && exists(literalStringToString()) && exists(toLiteralString()) && exists(shortStringOfValue()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
