/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTSplitUnions.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct StringRangeBounds {
    ValueID start;
    ValueID end;
};

struct StringRangeSource {
    ValueID string;
    std::optional<StringRangeBounds> bounds;
    ValueID first;
    ValueID second;
    StringRangeMethod method { StringRangeMethod::Slice };
    bool mayBeUndefined { false };

    bool isSingleUnit() const { return !bounds && (method == StringRangeMethod::SingleIndex || method == StringRangeMethod::SingleRelativeIndex); }
};

constexpr InlineBuiltin inlineBuiltinOf(StringRangeMethod method)
{
    switch (method) {
    case StringRangeMethod::Slice:
        return InlineBuiltin::StringSlice;
    case StringRangeMethod::Substring:
        return InlineBuiltin::StringSubstring;
    case StringRangeMethod::Substr:
        return InlineBuiltin::StringSubstr;
    case StringRangeMethod::SingleIndex:
        return InlineBuiltin::StringCharAt;
    case StringRangeMethod::SingleRelativeIndex:
        return InlineBuiltin::StringAt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

std::optional<StringRangeMethod> rangeMethodOf(InlineBuiltin);
std::optional<StringRangeSource> stringRangeSourceOf(const Program&, const Function&, const Value&);
StringRangeBounds insertStringRange(IRBuilder&, ProgramPoint, const StringRangeSource&, std::optional<StringRangeBounds> enclosing);

#define AOT_CHECK_STRING_RANGE_READ_OPERANDS(name) \
    static_assert(OperandIndex::name::string == OperandIndex::StringRangeCodeUnitOrNaN::string); \
    static_assert(OperandIndex::name::start == OperandIndex::StringRangeCodeUnitOrNaN::start); \
    static_assert(OperandIndex::name::end == OperandIndex::StringRangeCodeUnitOrNaN::end);
FOR_EACH_AOT_OPCODE_IN_STRING_RANGE_READ(AOT_CHECK_STRING_RANGE_READ_OPERANDS)
#undef AOT_CHECK_STRING_RANGE_READ_OPERANDS
static_assert(OperandIndex::StringRangeCodeUnitOrMinusOne::position == OperandIndex::StringRangeCodeUnitOrNaN::position);
static_assert(OperandIndex::StringRangeCodePointOrUndefined::position == OperandIndex::StringRangeCodeUnitOrNaN::position);
static_assert(OperandIndex::StringRangeCharacterOrEmpty::position == OperandIndex::StringRangeCodeUnitOrNaN::position);
static_assert(OperandIndex::StringRangeCharacterOrUndefined::position == OperandIndex::StringRangeCodeUnitOrNaN::position);

Value makeStringRangeRead(Opcode, ValueID string, std::optional<StringRangeBounds>, ValueID position);

// The calls of built-ins in a function, in the order of forEachValueSite().
using BuiltinCallSites = Vector<ValueSite, 16>;
BuiltinCallSites collectBuiltinCallSites(FunctionPassContext&);

PassOutcome expandStringRanges(FunctionPassContext&, const BuiltinCallSites&);
PassOutcome expandAppendedStringRanges(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
