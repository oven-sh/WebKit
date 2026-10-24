/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class BinaryOperator : uint8_t { Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq };
constexpr uint8_t numberOfBinaryOperators = static_cast<uint8_t>(BinaryOperator::GreaterEq) + 1;

constexpr bool isComparison(BinaryOperator operation) { return operation >= BinaryOperator::Less; }
constexpr bool isOnBits(BinaryOperator operation) { return operation >= BinaryOperator::BitAnd && operation <= BinaryOperator::URShift; }
constexpr bool hasDoubleCase(BinaryOperator operation) { return !isOnBits(operation); }
constexpr bool hasFormAfterInt32Check(BinaryOperator operation) { return operation != BinaryOperator::Div && operation != BinaryOperator::Mod; }

// Every operator, then in the same order every operator that has a form after the check.
constexpr uint8_t binaryOperationForm(BinaryOperator operation, bool isAfterInt32Check)
{
    if (!isAfterInt32Check)
        return static_cast<uint8_t>(operation);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(hasFormAfterInt32Check(operation));
    return numberOfBinaryOperators + static_cast<uint8_t>(operation) - (operation > BinaryOperator::Div) - (operation > BinaryOperator::Mod);
}

enum class BinaryWithDoubleForm : uint8_t { Add, Mul, Sub, SubWithNumberOnRight, Div, DivWithNumberOnRight };

constexpr BinaryWithDoubleForm binaryWithDoubleForm(BinaryOperator operation, bool numberIsOnRight)
{
    switch (operation) {
    case BinaryOperator::Add:
        return BinaryWithDoubleForm::Add;
    case BinaryOperator::Mul:
        return BinaryWithDoubleForm::Mul;
    case BinaryOperator::Sub:
        return numberIsOnRight ? BinaryWithDoubleForm::SubWithNumberOnRight : BinaryWithDoubleForm::Sub;
    case BinaryOperator::Div:
        return numberIsOnRight ? BinaryWithDoubleForm::DivWithNumberOnRight : BinaryWithDoubleForm::Div;
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class UnaryOperationForm : uint8_t { Negate, Increment, Decrement, BitNot, ToNumeric };
enum class BoxedNumberForm : uint8_t { Double, OptionalDouble, Integer, OptionalInteger };

constexpr StubKey binaryOperation(BinaryOperator operation, bool isAfterInt32Check) { return StubKey(StubFamily::BinaryOperation).withForm(binaryOperationForm(operation, isAfterInt32Check)); }
constexpr StubKey binaryWithDouble(BinaryOperator operation, bool numberIsOnRight) { return StubKey(StubFamily::BinaryWithDouble).withForm(static_cast<uint8_t>(binaryWithDoubleForm(operation, numberIsOnRight))); }
constexpr StubKey unaryOperation(UnaryOperationForm form) { return StubKey(StubFamily::UnaryOperation).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey boxNumber(BoxedNumberForm form) { return StubKey(StubFamily::BoxNumber).withForm(static_cast<uint8_t>(form)); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
