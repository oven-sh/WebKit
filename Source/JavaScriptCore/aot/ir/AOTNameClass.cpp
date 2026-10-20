/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNameClass.h"

#if ENABLE(AOT)

#include "Identifier.h"
#include "JSGlobalObjectFunctions.h"
#include "PropertyName.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/dtoa.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxCanonicalNumericStringLength = 32;

static consteval size_t maxWordLength()
{
    size_t longest = 0;
    for (NameClass nameClass : allMembersOf<NameClass>) {
        size_t length = 0;
        for (char character : wordsOf(nameClass).span()) {
            length = character == ' ' ? 0 : length + 1;
            longest = std::max(longest, length);
        }
    }
    return longest;
}

static bool isCanonicalNumericString(std::span<const uint8_t> bytes)
{
    if (bytes.size() > maxCanonicalNumericStringLength)
        return false;
    auto characters = byteCast<Latin1Character>(bytes);
    if (auto fastResult = fastIsCanonicalNumericIndexString(characters))
        return *fastResult;
    double number = jsToNumber(StringView(characters));
    NumberToStringBuffer buffer;
    return equalSpans(WTF::numberToStringAndSize(number, buffer), bytes);
}

static NameClass classifyWord(std::span<const uint8_t> text)
{
    if (text.size() > maxWordLength())
        return NameClass::Ordinary;
    for (NameClass nameClass : allMembersOf<NameClass>) {
        std::span<const char> words = wordsOf(nameClass).span();
        while (!words.empty()) {
            size_t length = std::ranges::find(words, ' ') - words.begin();
            if (equalSpans(words.first(length), text))
                return nameClass;
            words = words.subspan(std::min(length + 1, words.size()));
        }
    }
    return NameClass::Ordinary;
}

NameClass classifyName(std::span<const uint8_t> text)
{
    if (text.empty())
        return NameClass::Ordinary;
    if (parseIndex(byteCast<Latin1Character>(text)))
        return NameClass::Index;
    if (isCanonicalNumericString(text))
        return NameClass::CanonicalNumericString;
    return classifyWord(text);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
