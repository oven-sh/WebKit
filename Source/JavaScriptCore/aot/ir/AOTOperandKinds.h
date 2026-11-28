/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

TypeID widestTypeOf(TypeArena&, OperandKind);

bool hasKind(TypeArena&, TypeID proven, OperandKind, LayoutID layoutOfOperation = { });

struct IllTypedOperand {
    InternalErrorKind error { InternalErrorKind::OperandTypeMismatch };
    uint32_t operandIndex { 0 };
    TypeID proven;
    TypeID required;
};

template<typename Operation> std::optional<IllTypedOperand> findIllTypedOperand(const TypingContext&, const Operation&, OperandTypes proven);
template<typename Terminator> std::optional<IllTypedOperand> findIllTypedOperandOfTerminator(const TypingContext&, const Terminator&, OperandTypes proven, FunctionID returningFunction);

template<typename Operation> bool handsOperandOn(const TypingContext&, const Operation&, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
