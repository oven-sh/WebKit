/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class MathBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit MathBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_MATH_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)
    Number stringToNumber(const InlineCall&, CellPointer string);

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
