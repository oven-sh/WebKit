/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTOperationData.h"
#include "AOTStringEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StringBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit StringBuiltinEmitter(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    void generateStringRepeat(const InlineCall&);
    void generateStringPadStart(const InlineCall& call) { generatePad(call, true); }
    void generateStringPadEnd(const InlineCall& call) { generatePad(call, false); }
    void generateStringIsWellFormed(const InlineCall&);
    void generateStringToWellFormed(const InlineCall&);
    void generateStringNormalize(const InlineCall& call) { generateStringToWellFormed(call); }
    void generateStringFromCodePoint(const InlineCall&);
    void generateCallStringConstructor(const InlineCall&);
    void generateStringUTF8ByteLength(const InlineCall&);
    void generateStringLength(const InlineCall&);
    void generateStringAt(const InlineCall& call) { generateIndexedRead(call, InlineBuiltin::StringAt); }
    void generateStringCharAt(const InlineCall& call) { generateIndexedRead(call, InlineBuiltin::StringCharAt); }
    void generateStringCharCodeAt(const InlineCall& call) { generateIndexedRead(call, InlineBuiltin::StringCharCodeAt); }
    void generateStringCodePointAt(const InlineCall& call) { generateIndexedRead(call, InlineBuiltin::StringCodePointAt); }
    void generateStringSlice(const InlineCall& call) { generateRange(call, StringRangeMethod::Slice); }
    void generateStringSubstring(const InlineCall& call) { generateRange(call, StringRangeMethod::Substring); }
    void generateStringSubstr(const InlineCall& call) { generateRange(call, StringRangeMethod::Substr); }
    void generateStringStartsWith(const InlineCall& call) { generateHasAt(call, LiteralEnd::Start); }
    void generateStringEndsWith(const InlineCall& call) { generateHasAt(call, LiteralEnd::End); }
    void generateStringIncludes(const InlineCall& call) { generateSearch(call, SearchDirection::Forward, true); }
    void generateStringIndexOf(const InlineCall& call) { generateSearch(call, SearchDirection::Forward, false); }
    void generateStringLastIndexOf(const InlineCall& call) { generateSearch(call, SearchDirection::Backward, false); }
    void generateStringToLowerCase(const InlineCall& call) { generateCase(call, RuntimeOperation::StringToLowerCase); }
    void generateStringToUpperCase(const InlineCall& call) { generateCase(call, RuntimeOperation::StringToUpperCase); }
    void generateStringTrim(const InlineCall& call) { generateTrim(call, TrimmedEnds::Both); }
    void generateStringTrimStart(const InlineCall& call) { generateTrim(call, TrimmedEnds::Start); }
    void generateStringTrimEnd(const InlineCall& call) { generateTrim(call, TrimmedEnds::End); }
    void generateStringLocaleCompare(const InlineCall&);
    void generateStringConcat(const InlineCall&);
    void generateStringFromCharCode(const InlineCall&);
    void generateStringSplitPrefix(const InlineCall&);

private:
    StringEmitter& strings() { return emitter<StringEmitter>(); }
    CellPointer stringOperand(ValueID string) { return cellOperand(string); }
    // Where the operand may be undefined at run time it counts as `whenUndefined`. NaN is what the conversion of undefined to a number gives: position 0.
    std::optional<Number> numberArgument(const InlineCall&, unsigned index, double whenUndefined = PNaN);
    std::optional<Condition> isUndefinedArgument(const InlineCall&, unsigned index);
    Word32 positionArgument(const InlineCall&, unsigned index, Word32 length, Word32 whenAbsent);
    Condition is8Bit(CellPointer string);
    void continueIf(Condition condition, LBasicBlock otherwise) { strings().continueIf(condition, unlikelyEdge(otherwise)); }
    void finish(const InlineCall&, Operand naturalResult);

    void generateIndexedRead(const InlineCall&, InlineBuiltin);
    void generateRange(const InlineCall&, StringRangeMethod);
    void generateHasAt(const InlineCall&, LiteralEnd);
    Condition hasCharactersAt(const InlineCall&, CellPointer string, CellPointer other, Integer at);
    void generateSearch(const InlineCall&, SearchDirection, bool resultIsBoolean);
    void generateTrim(const InlineCall&, TrimmedEnds);
    void generateCase(const InlineCall&, RuntimeOperation);
    void generatePad(const InlineCall&, bool isAtStart);

    struct Separator {
        Word32 codeUnitOrText;
        unsigned length;
    };
    void splitAtLiteral(const InlineCall&, CellPointer string, StringID literal, unsigned count);
    // Writes where the first `wanted` separators are and returns how many of them it has found. Where none is wanted it counts them all.
    Integer searchSeparators(const CharacterView&, const Separator&, Pointer positions = { }, unsigned wanted = 0);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
