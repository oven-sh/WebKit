/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTStringBuiltinOperationData.h"
#include "AOTStringEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

enum class StringCase : uint8_t { Lower, Upper };

class StringBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit StringBuiltinEmitter(CodeGenerator&);

    void generateStringRepeat(const InlineCall&);
    void generateStringPadStart(const InlineCall&);
    void generateStringPadEnd(const InlineCall&);
    void generateStringIsWellFormed(const InlineCall&);
    void generateStringToWellFormed(const InlineCall&);
    void generateStringNormalize(const InlineCall&);
    void generateStringFromCodePoint(const InlineCall&);
    void generateCallStringConstructor(const InlineCall&);
    void generateStringUTF8ByteLength(const InlineCall&);
    void generateStringLength(const InlineCall&);
    void generateStringAt(const InlineCall&);
    void generateStringCharAt(const InlineCall&);
    void generateStringCharCodeAt(const InlineCall&);
    void generateStringCodePointAt(const InlineCall&);
    void generateStringSlice(const InlineCall&);
    void generateStringSubstring(const InlineCall&);
    void generateStringSubstr(const InlineCall&);
    void generateStringStartsWith(const InlineCall&);
    void generateStringEndsWith(const InlineCall&);
    void generateStringIncludes(const InlineCall&);
    void generateStringIndexOf(const InlineCall&);
    void generateStringLastIndexOf(const InlineCall&);
    void generateStringToLowerCase(const InlineCall&);
    void generateStringToUpperCase(const InlineCall&);
    void generateStringTrim(const InlineCall&);
    void generateStringTrimStart(const InlineCall&);
    void generateStringTrimEnd(const InlineCall&);
    void generateStringLocaleCompare(const InlineCall&);
    void generateStringConcat(const InlineCall&);
    void generateStringFromCharCode(const InlineCall&);
    void generateStringSplitPrefix(const InlineCall&);

private:
    StringEmitter& strings() { return emitter<StringEmitter>(); }
    CellPointer stringOperand(ValueID string) { return cellOperand(string); }
    std::optional<Number> numberArgument(const InlineCall&, unsigned index);
    Word32 positionArgument(const InlineCall&, unsigned index, Word32 length, Word32 whenAbsent);
    Condition is8Bit(CellPointer string);
    void finish(const InlineCall&, Operand naturalResult);

    void generateIndexedRead(const InlineCall&, InlineBuiltin);
    void generateRange(const InlineCall&, StringRangeMethod);
    void generateHasAt(const InlineCall&, LiteralEnd);
    void generateSearch(const InlineCall&, SearchDirection, bool resultIsBoolean);
    void generateTrim(const InlineCall&, TrimmedEnds);
    void generateCase(const InlineCall&, StringCase);
    void generatePad(const InlineCall&, bool isAtStart);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
