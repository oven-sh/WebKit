/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirArg.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

enum class ImmediateForm : uint8_t {
    None,
    Imm,
    BitImm,
    BitImm64,
};

inline ImmediateForm immediateFormFor(const Value* user, const Value* child)
{
    if (user->numChildren() != 2 || child != user->child(1))
        return ImmediateForm::None;
    switch (user->opcode()) {
    case Add:
    case Sub:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case RotL:
        return ImmediateForm::Imm;
    case BitAnd:
    case BitOr:
    case BitXor:
        return user->type() == Int64 ? ImmediateForm::BitImm64 : ImmediateForm::BitImm;
    default:
        return ImmediateForm::None;
    }
}

inline bool fitsImmediateForm(ImmediateForm form, int64_t value)
{
    switch (form) {
    case ImmediateForm::None:
        return false;
    case ImmediateForm::Imm:
        return Air::Arg::isValidImmForm(value);
    case ImmediateForm::BitImm:
        return Air::Arg::isValidBitImmForm(value);
    case ImmediateForm::BitImm64:
        return Air::Arg::isValidBitImm64Form(value);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

inline bool isSelectedAsImmediate(const Value* user, const Value* child)
{
    if (!child->hasInt() || user->numChildren() != 2 || (user->child(0)->hasInt() && user->child(1)->hasInt()))
        return false;
    return fitsImmediateForm(immediateFormFor(user, child), child->asInt());
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
