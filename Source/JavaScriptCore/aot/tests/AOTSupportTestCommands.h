/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct CommonTestWords {
    unsigned threads { 1 };
    unsigned seed { 0 };
};

void printBadArgument(PrintStream&, StringView word);
void printMissingArgument(PrintStream&);
void printCheck(PrintStream&, ASCIILiteral name, bool passed);
ASCIILiteral yesOrNo(bool);
bool isOnlyWord(std::span<const String> words, ASCIILiteral word);
std::optional<unsigned> parseThreadCount(StringView);
bool takeCommonTestWord(StringView word, CommonTestWords&);
std::optional<CommonTestWords> parseCommonTestWords(PrintStream&, std::span<const String> words);

template<typename T>
std::optional<T> parseTestNumber(StringView text)
{
    if (text.startsWith('+'))
        return std::nullopt;
    return parseInteger<T>(text, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

template<typename T>
void shuffle(Vector<T>& list, unsigned seed)
{
    WeakRandom random(seed + 1);
    shuffleVector(list, [&](size_t limit) {
        return random.getUint32(static_cast<unsigned>(limit));
    });
}

void runDiagnosticsTestCommand(TestCommandContext&);
void runLossesTestCommand(TestCommandContext&);
void runParallelLoopsTestCommand(TestCommandContext&);
void runArenaMergeTestCommand(TestCommandContext&);
void runHashedIndexTestCommand(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
