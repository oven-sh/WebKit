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

struct OperatorTypes {
    TypeID left;
    TypeID right;
};

struct OperatorChoice {
    OperatorForm form { OperatorForm::Generic };
    TypeID result;
    Effects effects;
};

bool hasOperatorForm(Opcode);
OperatorChoice chooseOperatorForm(const Program&, TypeArena&, Opcode, OperatorTypes);
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
