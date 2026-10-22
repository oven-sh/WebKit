/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3Opcode.h"
#include "MacroAssembler.h"

namespace JSC { namespace AOT { namespace Typed { namespace LinearScan {

inline bool isComparison(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        return true;
    default:
        return false;
    }
}

inline bool isPureOperation(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
    case B3::ZExt32:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::Trunc:
        return true;
    default:
        return isComparison(opcode);
    }
}

inline bool isCommutative(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Add:
    case B3::Mul:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        return true;
    default:
        return false;
    }
}

inline MacroAssembler::RelationalCondition relationalConditionOf(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
        return MacroAssembler::Equal;
    case B3::NotEqual:
        return MacroAssembler::NotEqual;
    case B3::LessThan:
        return MacroAssembler::LessThan;
    case B3::GreaterThan:
        return MacroAssembler::GreaterThan;
    case B3::LessEqual:
        return MacroAssembler::LessThanOrEqual;
    case B3::GreaterEqual:
        return MacroAssembler::GreaterThanOrEqual;
    case B3::Above:
        return MacroAssembler::Above;
    case B3::Below:
        return MacroAssembler::Below;
    case B3::AboveEqual:
        return MacroAssembler::AboveOrEqual;
    case B3::BelowEqual:
        return MacroAssembler::BelowOrEqual;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline MacroAssembler::DoubleCondition doubleConditionOf(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
        return MacroAssembler::DoubleEqualAndOrdered;
    case B3::EqualOrUnordered:
        return MacroAssembler::DoubleEqualOrUnordered;
    case B3::NotEqual:
        return MacroAssembler::DoubleNotEqualOrUnordered;
    case B3::LessThan:
        return MacroAssembler::DoubleLessThanAndOrdered;
    case B3::GreaterThan:
        return MacroAssembler::DoubleGreaterThanAndOrdered;
    case B3::LessEqual:
        return MacroAssembler::DoubleLessThanOrEqualAndOrdered;
    case B3::GreaterEqual:
        return MacroAssembler::DoubleGreaterThanOrEqualAndOrdered;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline bool comparesWithoutSign(MacroAssembler::RelationalCondition condition)
{
    switch (condition) {
    case MacroAssembler::Equal:
    case MacroAssembler::NotEqual:
    case MacroAssembler::Above:
    case MacroAssembler::AboveOrEqual:
    case MacroAssembler::Below:
    case MacroAssembler::BelowOrEqual:
        return true;
    default:
        return false;
    }
}

} } } } // namespace JSC::AOT::Typed::LinearScan

#endif // ENABLE(AOT)
