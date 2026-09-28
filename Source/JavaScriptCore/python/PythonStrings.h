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

#pragma once

#include "PythonOperations.h"

namespace JSC { namespace Python {

// str.isidentifier()
bool isIdentifier(StringView);

JSValue stringGetItem(JSGlobalObject*, JSString*, JSValue key);
JSValue stringRepeat(JSGlobalObject*, JSString*, int64_t count);
// format % values
JSValue stringPercentFormat(JSGlobalObject*, JSValue format, JSValue values);
// The same of bytes. The result has a character for each byte, and is null if it raised.
String bytesPercentFormat(JSGlobalObject*, std::span<const uint8_t> format, JSValue values);
// What ascii() does to what repr() gives.
String escapeNonASCII(const String&);

// [[fill]align][sign][z][#][0][width][grouping][.precision][type], which is what comes after the colon in f"{x:>10.2f}".
struct FormatSpecification {
    char32_t fill { ' ' };
    char align { 0 }; // '<', '>', '=', '^', or 0 for what the type does by default.
    char sign { '-' };
    bool hasSign { false }; // It was given, even if it is the '-' that it would have been anyway.
    bool noNegativeZero { false };
    bool alternate { false };
    bool hasWidth { false };
    unsigned width { 0 };
    char grouping { 0 }; // ',' or '_'
    int precision { -1 };
    char fractionGrouping { 0 }; // The same, of the digits after the point.
    char32_t type { 0 };
    String typeName; // Of what it is for, which is said if it will not do.
};

// Nothing if it makes no sense, and then ValueError has been raised. `typeName` is what it was meant for.
// A string is put to the left of its field unless told otherwise, and that changes what a 0 before the width means.
std::optional<FormatSpecification> parseFormatSpecification(JSGlobalObject*, StringView, const String& typeName, bool isForString = false);

// What int.__format__, float.__format__ and str.__format__ do. Null if they raised.
String formatInt(JSGlobalObject*, JSValue, const FormatSpecification&);
String formatFloat(JSGlobalObject*, double, const FormatSpecification&);
String formatString(JSGlobalObject*, const String&, const FormatSpecification&);
String raiseUnknownFormatCode(JSGlobalObject*, ThrowScope&, char32_t code, const String& typeName);

// The digits of a float rounded to so many places after the point, correctly, with ties going to the even digit.
String fixedDigits(double, int precision);
// round(x, digits)
double roundToDigits(double, int digits);

} } // namespace JSC::Python
