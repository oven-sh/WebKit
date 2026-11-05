/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDeclaredType.h"
#include "AOTEffects.h"
#include "AOTOpcode.h"
#include "AOTRepresentationRules.h"
#include "AOTType.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Program;

// AOTOperatorForms.cpp: pure functions of an opcode and the proven types of its operands. One form for each opcode and state of knowledge; where the operation stands does not matter.

struct OperatorTypes {
    TypeID left;
    // Empty for an operator with one operand.
    TypeID right;
};

struct OperatorChoice {
    OperatorForm form { OperatorForm::Generic };
    // By resultTypeOfOperator(), whatever the form: the type that append() gives the record.
    TypeID result;
    Effects effects;
};

// The opcodes that the schema gives a `form` field.
bool hasOperatorForm(Opcode);
OperatorChoice chooseOperatorForm(const Program&, TypeArena&, Opcode, OperatorTypes);
// Its `form` alone: no effects are worked out, and the type of the result only for arithmetic, whose form hangs on it.
OperatorForm chooseFormOfOperator(const Program&, TypeArena&, Opcode, OperatorTypes);
OperatorForm chooseArithmeticForm(const Program&, const TypeArena&, Opcode, OperatorTypes, TypeID result);
OperatorForm chooseComparisonForm(const Program&, const TypeArena&, Opcode, OperatorTypes);
OperatorForm chooseConversionForm(const Program&, const TypeArena&, Opcode, TypeID operand);
bool isLegalOperatorForm(Opcode, OperatorForm);
bool mayCallRuntime(Opcode, OperatorForm);
Effects effectsBoundOf(Opcode, OperatorForm);
bool canCompareBits(const TypeArena&, TypeID left, TypeID right);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
