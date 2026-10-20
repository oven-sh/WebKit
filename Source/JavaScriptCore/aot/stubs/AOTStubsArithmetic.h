/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class BinaryOperator : uint8_t { Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq };

constexpr bool isComparison(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        return true;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Mod:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool hasDoubleCase(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        return true;
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool isOnBits(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
        return true;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool hasFormAfterInt32Check(BinaryOperator operation)
{
    switch (operation) {
    case BinaryOperator::Div:
    case BinaryOperator::Mod:
        return false;
    case BinaryOperator::Add:
    case BinaryOperator::Sub:
    case BinaryOperator::Mul:
    case BinaryOperator::BitAnd:
    case BinaryOperator::BitOr:
    case BinaryOperator::BitXor:
    case BinaryOperator::LShift:
    case BinaryOperator::RShift:
    case BinaryOperator::URShift:
    case BinaryOperator::Less:
    case BinaryOperator::LessEq:
    case BinaryOperator::Greater:
    case BinaryOperator::GreaterEq:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class BinaryOperationForm : uint8_t {
    Add, Sub, Mul, Div, BitAnd, BitOr, BitXor, LShift, RShift, URShift, Mod, Less, LessEq, Greater, GreaterEq,
    AddAfterInt32Check, SubAfterInt32Check, MulAfterInt32Check, BitAndAfterInt32Check, BitOrAfterInt32Check, BitXorAfterInt32Check, LShiftAfterInt32Check, RShiftAfterInt32Check, URShiftAfterInt32Check,
    LessAfterInt32Check, LessEqAfterInt32Check, GreaterAfterInt32Check, GreaterEqAfterInt32Check
};

struct BinaryOperationShape {
    BinaryOperator operation;
    bool isAfterInt32Check;
};

constexpr BinaryOperationForm binaryOperationForm(BinaryOperator operation, bool isAfterInt32Check)
{
    if (isAfterInt32Check) {
        switch (operation) {
        case BinaryOperator::Add: return BinaryOperationForm::AddAfterInt32Check;
        case BinaryOperator::Sub: return BinaryOperationForm::SubAfterInt32Check;
        case BinaryOperator::Mul: return BinaryOperationForm::MulAfterInt32Check;
        case BinaryOperator::BitAnd: return BinaryOperationForm::BitAndAfterInt32Check;
        case BinaryOperator::BitOr: return BinaryOperationForm::BitOrAfterInt32Check;
        case BinaryOperator::BitXor: return BinaryOperationForm::BitXorAfterInt32Check;
        case BinaryOperator::LShift: return BinaryOperationForm::LShiftAfterInt32Check;
        case BinaryOperator::RShift: return BinaryOperationForm::RShiftAfterInt32Check;
        case BinaryOperator::URShift: return BinaryOperationForm::URShiftAfterInt32Check;
        case BinaryOperator::Less: return BinaryOperationForm::LessAfterInt32Check;
        case BinaryOperator::LessEq: return BinaryOperationForm::LessEqAfterInt32Check;
        case BinaryOperator::Greater: return BinaryOperationForm::GreaterAfterInt32Check;
        case BinaryOperator::GreaterEq: return BinaryOperationForm::GreaterEqAfterInt32Check;
        case BinaryOperator::Div:
        case BinaryOperator::Mod:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
    switch (operation) {
    case BinaryOperator::Add: return BinaryOperationForm::Add;
    case BinaryOperator::Sub: return BinaryOperationForm::Sub;
    case BinaryOperator::Mul: return BinaryOperationForm::Mul;
    case BinaryOperator::Div: return BinaryOperationForm::Div;
    case BinaryOperator::BitAnd: return BinaryOperationForm::BitAnd;
    case BinaryOperator::BitOr: return BinaryOperationForm::BitOr;
    case BinaryOperator::BitXor: return BinaryOperationForm::BitXor;
    case BinaryOperator::LShift: return BinaryOperationForm::LShift;
    case BinaryOperator::RShift: return BinaryOperationForm::RShift;
    case BinaryOperator::URShift: return BinaryOperationForm::URShift;
    case BinaryOperator::Mod: return BinaryOperationForm::Mod;
    case BinaryOperator::Less: return BinaryOperationForm::Less;
    case BinaryOperator::LessEq: return BinaryOperationForm::LessEq;
    case BinaryOperator::Greater: return BinaryOperationForm::Greater;
    case BinaryOperator::GreaterEq: return BinaryOperationForm::GreaterEq;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr BinaryOperationShape shapeOf(BinaryOperationForm form)
{
    switch (form) {
    case BinaryOperationForm::Add: return { BinaryOperator::Add, false };
    case BinaryOperationForm::Sub: return { BinaryOperator::Sub, false };
    case BinaryOperationForm::Mul: return { BinaryOperator::Mul, false };
    case BinaryOperationForm::Div: return { BinaryOperator::Div, false };
    case BinaryOperationForm::BitAnd: return { BinaryOperator::BitAnd, false };
    case BinaryOperationForm::BitOr: return { BinaryOperator::BitOr, false };
    case BinaryOperationForm::BitXor: return { BinaryOperator::BitXor, false };
    case BinaryOperationForm::LShift: return { BinaryOperator::LShift, false };
    case BinaryOperationForm::RShift: return { BinaryOperator::RShift, false };
    case BinaryOperationForm::URShift: return { BinaryOperator::URShift, false };
    case BinaryOperationForm::Mod: return { BinaryOperator::Mod, false };
    case BinaryOperationForm::Less: return { BinaryOperator::Less, false };
    case BinaryOperationForm::LessEq: return { BinaryOperator::LessEq, false };
    case BinaryOperationForm::Greater: return { BinaryOperator::Greater, false };
    case BinaryOperationForm::GreaterEq: return { BinaryOperator::GreaterEq, false };
    case BinaryOperationForm::AddAfterInt32Check: return { BinaryOperator::Add, true };
    case BinaryOperationForm::SubAfterInt32Check: return { BinaryOperator::Sub, true };
    case BinaryOperationForm::MulAfterInt32Check: return { BinaryOperator::Mul, true };
    case BinaryOperationForm::BitAndAfterInt32Check: return { BinaryOperator::BitAnd, true };
    case BinaryOperationForm::BitOrAfterInt32Check: return { BinaryOperator::BitOr, true };
    case BinaryOperationForm::BitXorAfterInt32Check: return { BinaryOperator::BitXor, true };
    case BinaryOperationForm::LShiftAfterInt32Check: return { BinaryOperator::LShift, true };
    case BinaryOperationForm::RShiftAfterInt32Check: return { BinaryOperator::RShift, true };
    case BinaryOperationForm::URShiftAfterInt32Check: return { BinaryOperator::URShift, true };
    case BinaryOperationForm::LessAfterInt32Check: return { BinaryOperator::Less, true };
    case BinaryOperationForm::LessEqAfterInt32Check: return { BinaryOperator::LessEq, true };
    case BinaryOperationForm::GreaterAfterInt32Check: return { BinaryOperator::Greater, true };
    case BinaryOperationForm::GreaterEqAfterInt32Check: return { BinaryOperator::GreaterEq, true };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
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

constexpr StubKey binaryOperation(BinaryOperator operation, bool isAfterInt32Check) { return StubKey(StubFamily::BinaryOperation).withForm(static_cast<uint8_t>(binaryOperationForm(operation, isAfterInt32Check))); }
constexpr StubKey binaryWithDouble(BinaryOperator operation, bool numberIsOnRight) { return StubKey(StubFamily::BinaryWithDouble).withForm(static_cast<uint8_t>(binaryWithDoubleForm(operation, numberIsOnRight))); }
constexpr StubKey unaryOperation(UnaryOperationForm form) { return StubKey(StubFamily::UnaryOperation).withForm(static_cast<uint8_t>(form)); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
