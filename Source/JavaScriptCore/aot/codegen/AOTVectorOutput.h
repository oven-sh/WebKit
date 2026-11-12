/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTOutput.h"
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

class VectorOutput {
public:
    explicit VectorOutput(Output& out)
        : m_out(out)
    {
    }

    enum class Arithmetic : uint8_t { Add, Sub, Mul, Div };

    LValue splat(LValue number);
    LValue insert(LValue first, LValue second);
    LValue lane(LValue numbers, unsigned index);
    LValue compute(Arithmetic, LValue left, LValue right);
    LValue squareRoots(LValue numbers);

    LValue spreadBytes(LValue word);
    LValue toNumbers(LValue words, unsigned half);
    LValue widenToWords(LValue integers, TypedArrayType);
    LValue singlesToNumbers(LValue singles);
    LValue numbersToSingles(LValue numbers);

    LValue splatInteger(LValue word, unsigned lanes);
    LValue joinWords(LValue firstPair, LValue secondPair);
    LValue computeIntegers(Opcode, unsigned lanes, LValue left, LValue right);
    LValue shiftWords(bool isLeft, LValue words, unsigned amount);
    LValue reduceWords(Opcode, LValue words);
    LValue sumsOfPairs(LValue words);

private:
    template<typename Functor> LValue unary(LType, LValue operand, const Functor&);

    Output& m_out;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
