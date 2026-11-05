/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/ASCIICType.h>
#include <wtf/Vector.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

namespace JSC {
namespace Python {

// 'a%sb%5r' % (x, y) is f'a{x!s}b{y!r:>5}' to CPython's compiler, if the format is a literal with nothing in it but %s, %r, %a and %%, with or without flags, a width and a
// precision of at most two digits, and the tuple is written out, without a star, and has as many items. It shows in the tree that ast.parse(optimize=1) gives, in the
// constants of the code, and in its lines: the pieces of text are on no line.

// A piece of text, or what becomes of the next item.
struct PartOfFormat {
    String text; // Or the format spec; null: there is none.
    int conversion { 0 }; // 0: text.
};

// False: it is left as it is.
inline bool takeFormatApart(StringView format, unsigned itemCount, Vector<PartOfFormat, 8>& parts)
{
    unsigned length = format.length();
    unsigned position = 0;
    unsigned used = 0;
    static constexpr unsigned maxDigits = 2;
    // False: there are too many digits.
    auto number = [&](int& result) {
        unsigned digits = 0;
        for (result = 0; position < length && isASCIIDigit(format[position]); ++position) {
            if (++digits > maxDigits)
                return false;
            result = result * 10 + (format[position] - '0');
        }
        return true;
    };
    while (true) {
        StringBuilder text;
        for (; position < length; ++position) {
            if (format[position] == '%') {
                if (position + 1 == length || format[position + 1] != '%')
                    break;
                ++position;
            }
            text.append(format[position]);
        }
        if (!text.isEmpty())
            parts.append({ text.toString(), 0 });
        if (position == length)
            return used == itemCount;
        if (used++ == itemCount)
            return false;
        ++position;
        bool isOnTheLeft = false;
        for (; position < length && (format[position] == '-' || format[position] == '+' || format[position] == ' ' || format[position] == '#' || format[position] == '0'); ++position)
            isOnTheLeft |= format[position] == '-';
        int width = -1;
        int precision = -1;
        if (position < length && isASCIIDigit(format[position]) && !number(width))
            return false;
        if (position < length && format[position] == '.') {
            ++position;
            if (!number(precision))
                return false;
        }
        if (position == length || (format[position] != 's' && format[position] != 'r' && format[position] != 'a'))
            return false;
        StringBuilder spec;
        if (!isOnTheLeft && width > 0)
            spec.append('>');
        if (width >= 0)
            spec.append(width);
        if (precision >= 0)
            spec.append('.', precision);
        parts.append({ spec.isEmpty() ? String() : spec.toString(), format[position++] });
    }
}

} // namespace Python
} // namespace JSC
