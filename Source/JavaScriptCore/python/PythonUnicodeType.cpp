/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonUnicodeType.h"

namespace JSC { namespace Python { namespace Unicode {

namespace {

enum Flag : uint16_t {
    AlphaMask = 0x01,
    DecimalMask = 0x02,
    DigitMask = 0x04,
    LowerMask = 0x08,
    TitleMask = 0x40,
    UpperMask = 0x80,
    XIDStartMask = 0x100,
    XIDContinueMask = 0x200,
    PrintableMask = 0x400,
    NumericMask = 0x800,
    CaseIgnorableMask = 0x1000,
    CasedMask = 0x2000,
    ExtendedCaseMask = 0x4000,
};

struct TypeRecord {
    // Each is what to add to the character, or where to look in extendedCase.
    int32_t upper;
    int32_t lower;
    int32_t title;
    uint8_t decimal;
    uint8_t digit;
    uint16_t flags;
};

} // anonymous namespace

#include "PythonUnicodeTypeDatabase.h"

static const TypeRecord& typeRecordOf(char32_t code)
{
    if (code >= 0x110000)
        return typeRecords[0];
    unsigned index = index1[code >> shift];
    return typeRecords[index2[(index << shift) + (code & ((1 << shift) - 1))]];
}

static bool has(char32_t c, Flag flag) { return typeRecordOf(c).flags & flag; }

bool isAlpha(char32_t c) { return has(c, AlphaMask); }
bool isDecimalDigit(char32_t c) { return has(c, DecimalMask); }
bool isDigit(char32_t c) { return has(c, DigitMask); }
bool isNumeric(char32_t c) { return has(c, NumericMask); }
bool isLowercase(char32_t c) { return has(c, LowerMask); }
bool isUppercase(char32_t c) { return has(c, UpperMask); }
bool isTitlecase(char32_t c) { return has(c, TitleMask); }
bool isCased(char32_t c) { return has(c, CasedMask); }
bool isCaseIgnorable(char32_t c) { return has(c, CaseIgnorableMask); }
bool isPrintable(char32_t c) { return has(c, PrintableMask); }
bool isXIDStart(char32_t c) { return has(c, XIDStartMask); }
bool isXIDContinue(char32_t c) { return has(c, XIDContinueMask); }

int toDecimalDigit(char32_t c)
{
    auto& record = typeRecordOf(c);
    return record.flags & DecimalMask ? record.decimal : -1;
}

int toDigit(char32_t c)
{
    auto& record = typeRecordOf(c);
    return record.flags & DigitMask ? record.digit : -1;
}

static char32_t mapOne(char32_t c, int32_t TypeRecord::* member)
{
    auto& record = typeRecordOf(c);
    if (record.flags & ExtendedCaseMask)
        return extendedCase[record.*member & 0xFFFF];
    return c + record.*member;
}

char32_t toLowercase(char32_t c) { return mapOne(c, &TypeRecord::lower); }
char32_t toUppercase(char32_t c) { return mapOne(c, &TypeRecord::upper); }
char32_t toTitlecase(char32_t c) { return mapOne(c, &TypeRecord::title); }

static unsigned mapFull(char32_t c, int32_t TypeRecord::* member, Mapping& result)
{
    auto& record = typeRecordOf(c);
    if (record.flags & ExtendedCaseMask) {
        unsigned index = record.*member & 0xFFFF;
        unsigned count = record.*member >> 24;
        for (unsigned i = 0; i < count; ++i)
            result[i] = extendedCase[index + i];
        return count;
    }
    result[0] = c + record.*member;
    return 1;
}

unsigned toLowerFull(char32_t c, Mapping& result) { return mapFull(c, &TypeRecord::lower, result); }
unsigned toUpperFull(char32_t c, Mapping& result) { return mapFull(c, &TypeRecord::upper, result); }
unsigned toTitleFull(char32_t c, Mapping& result) { return mapFull(c, &TypeRecord::title, result); }

unsigned toFoldedFull(char32_t c, Mapping& result)
{
    auto& record = typeRecordOf(c);
    if (record.flags & ExtendedCaseMask && (record.lower >> 20) & 7) {
        unsigned index = (record.lower & 0xFFFF) + (record.lower >> 24);
        unsigned count = (record.lower >> 20) & 7;
        for (unsigned i = 0; i < count; ++i)
            result[i] = extendedCase[index + i];
        return count;
    }
    return toLowerFull(c, result);
}

} } } // namespace JSC::Python::Unicode
