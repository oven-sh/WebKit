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
#include <array>
#include <wtf/StdLibExtras.h>
#include <wtf/dtoa.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxCanonicalNumericStringLength = 32;

struct ClassifiedWord {
    std::span<const char> text;
    NameClass nameClass { NameClass::Ordinary };
};

static consteval void forEachClassifiedWord(auto&& functor)
{
    for (NameClass nameClass : allMembersOf<NameClass>) {
        std::span<const char> words = wordsOf(nameClass).span();
        while (!words.empty()) {
            size_t length = std::ranges::find(words, ' ') - words.begin();
            functor(ClassifiedWord { words.first(length), nameClass });
            words = words.subspan(std::min(length + 1, words.size()));
        }
    }
}

static consteval size_t countClassifiedWords()
{
    size_t count = 0;
    forEachClassifiedWord([&](ClassifiedWord) {
        ++count;
    });
    return count;
}

static consteval size_t maxWordLength()
{
    size_t longest = 0;
    forEachClassifiedWord([&](ClassifiedWord word) {
        longest = std::max(longest, word.text.size());
    });
    return longest;
}

struct ClassifiedWordsByLength {
    std::array<ClassifiedWord, countClassifiedWords()> words;
    std::array<uint8_t, maxWordLength() + 2> firstOfLength { };
};

static consteval ClassifiedWordsByLength sortClassifiedWordsByLength()
{
    ClassifiedWordsByLength sorted;
    size_t count = 0;
    for (size_t length = 0; length <= maxWordLength(); ++length) {
        sorted.firstOfLength[length] = count;
        forEachClassifiedWord([&](ClassifiedWord word) {
            if (word.text.size() == length)
                sorted.words[count++] = word;
        });
    }
    sorted.firstOfLength[maxWordLength() + 1] = count;
    return sorted;
}

static constexpr ClassifiedWordsByLength classifiedWords = sortClassifiedWordsByLength();

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
    for (size_t index = classifiedWords.firstOfLength[text.size()]; index < classifiedWords.firstOfLength[text.size() + 1]; ++index) {
        if (equalSpans(classifiedWords.words[index].text, text))
            return classifiedWords.words[index].nameClass;
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
