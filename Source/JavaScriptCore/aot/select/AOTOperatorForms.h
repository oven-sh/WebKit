/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct OperatorSite {
    const TypingContext& typing;
    const EffectContext& effects;
    CodeShape shape { CodeShape::Balanced };
};

struct OperatorChoice {
    OperatorForm form { OperatorForm::Generic };
    Type result;
    EffectsOnEdges effects;
};

constexpr bool hasOperatorForm(Opcode opcode) { return Value::opcodesWithForm.contains(opcode); }

OperatorChoice chooseOperatorForm(const OperatorSite&, const Value&);

OperatorForm chooseArithmeticForm(const OperatorSite&, const Value&);
OperatorForm chooseComparisonForm(const OperatorSite&, const Value&);
OperatorForm chooseConversionForm(const OperatorSite&, const Value&);

struct OperatorTypes {
    const Type& left;
    const Type& right;
    const Type& result;
};

OperatorChoice chooseOperatorFormFor(const OperatorSite&, Opcode, std::span<const Type> operandTypes);

OperatorForm chooseArithmeticForm(const OperatorSite&, Opcode, const OperatorTypes&);
OperatorForm chooseComparisonForm(const OperatorSite&, Opcode, const OperatorTypes&);
OperatorForm chooseConversionForm(const OperatorSite&, Opcode, const OperatorTypes&);

bool isLegalOperatorForm(Opcode, OperatorForm);
bool mayCallRuntime(Opcode, OperatorForm);
Effects effectsBoundOf(Opcode, OperatorForm);

bool isNumericOperand(const Type&);
bool canCompareBits(const Type& left, const Type& right);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
