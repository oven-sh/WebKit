/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTSchemaInlineBuiltins.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class MathBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit MathBuiltinEmitter(CodeGenerator&);

    void generateMathMinOfArray(const InlineCall&);
    void generateMathMaxOfArray(const InlineCall&);
    void generateCallNumberConstructor(const InlineCall&);
    Number stringToNumber(const InlineCall&, CellPointer string);
    void generateNumberToString(const InlineCall&);
    void generateNumberToStringAppend(const InlineCall&);
    void generateParseInt(const InlineCall&);
    void generateMathAbs(const InlineCall&);
    void generateMathFloor(const InlineCall&);
    void generateMathCeil(const InlineCall&);
    void generateMathTrunc(const InlineCall&);
    void generateMathRound(const InlineCall&);
    void generateMathFround(const InlineCall&);
    void generateMathF16Round(const InlineCall&);
    void generateMathSqrt(const InlineCall&);
    void generateMathPow(const InlineCall&);
    void generateMathMin(const InlineCall&);
    void generateMathMax(const InlineCall&);
    void generateMathImul(const InlineCall&);
    void generateMathClz32(const InlineCall&);
    void generateMathSign(const InlineCall&);
    void generateMathFunction(const InlineCall&);
    void generateMathATan2(const InlineCall&);
    void generateMathHypot(const InlineCall&);
    void generateIsNaN(const InlineCall&);
    void generateIsFinite(const InlineCall&);
    void generateNumberIsNaN(const InlineCall&);
    void generateNumberIsFinite(const InlineCall&);
    void generateNumberIsInteger(const InlineCall&);
    void generateNumberIsSafeInteger(const InlineCall&);
    void generateMathRandom(const InlineCall&);

private:
    enum class Rounding : uint8_t { Floor, Ceil, Trunc, Round };
    enum class Extremum : uint8_t { Min, Max };
    enum class NumberTest : uint8_t { IsNaN, IsFinite, IsInteger, IsSafeInteger };

    void round(const InlineCall&, Rounding);
    void extremum(const InlineCall&, Extremum);
    void extremumOfArray(const InlineCall&, Extremum);
    Number extremumOfElements(Extremum, CellPointer array, Pointer butterfly, Integer length, LBasicBlock otherwise);
    void testNumber(const InlineCall&, NumberTest);
    Condition testNumber(Number, NumberTest);
    Condition isFinite(Number);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
