/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTDriver.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTTestHarness.h"
#include "AOTVector.h"
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

inline String joinLinesOfReason(StringView text)
{
    StringBuilder line;
    for (StringView part : text.split('\n'))
        line.append(line.isEmpty() ? ""_s : " | "_s, part);
    return line.toString();
}

[[noreturn]] inline void failNativeTestBecause(const char* file, int line, const String& reason)
{
    CString text = joinLinesOfReason(reason).utf8();
    failNativeTest(file, line, byteCast<char>(text.data()));
}

inline String optionWordsOfReason(OptionWords optionWords)
{
    StringBuilder words;
    for (ASCIILiteral word : optionWords)
        words.append(words.isEmpty() ? ""_s : " "_s, word);
    return words.isEmpty() ? String("no option word"_s) : words.toString();
}

inline String optionWordsOfReason(ASCIILiteral firstWord, OptionWords optionWords)
{
    if (firstWord.isEmpty())
        return optionWordsOfReason(optionWords);
    if (!optionWords.size())
        return firstWord;
    return makeString(firstWord, ' ', optionWordsOfReason(optionWords));
}

[[noreturn]] inline void failBecauseTextIsNoProgram(const char* file, int line, BuildContext& build, const String& optionWords)
{
    build.diagnostics().finish(nullptr);
    StringPrintStream diagnostics;
    Typed::printDiagnostics(build.diagnostics(), diagnostics);
    failNativeTestBecause(file, line, makeString("the IR text is no program ("_s, optionWords, "): "_s, diagnostics.toString()));
}

[[noreturn]] inline void failBecauseThereIsNoImage(const char* file, int line, const CompileOutput& output, const String& optionWords)
{
    StringPrintStream diagnostics;
    Typed::printDiagnosticsOf(output, diagnostics);
    failNativeTestBecause(file, line, makeString("the compiler made no image ("_s, optionWords, "): "_s, diagnostics.toString()));
}

inline String differenceOfPrintedLines(StringView printed, StringView expected)
{
    Vector<StringView> printedLines;
    for (StringView line : printed.splitAllowingEmptyEntries('\n'))
        printedLines.append(line);
    Vector<StringView> expectedLines;
    for (StringView line : expected.splitAllowingEmptyEntries('\n'))
        expectedLines.append(line);

    size_t index = 0;
    while (index < printedLines.size() && index < expectedLines.size() && printedLines[index] == expectedLines[index])
        ++index;
    auto lineAt = [&](const Vector<StringView>& lines) {
        return index < lines.size() ? makeString('`', lines[index], '`') : String("missing"_s);
    };
    return makeString("line "_s, index + 1, " is "_s, lineAt(printedLines), ", expected "_s, lineAt(expectedLines), "; printed: "_s, joinLinesOfReason(printed), "; expected: "_s, joinLinesOfReason(expected));
}

inline void checkPrintedLines(const char* file, int line, const String& optionWords, StringView printed, StringView expected)
{
    if (printed != expected) [[unlikely]]
        failNativeTestBecause(file, line, makeString('(', optionWords, ") "_s, differenceOfPrintedLines(printed, expected)));
}

inline void checkThatChildSucceeded(const char* file, int line, const String& optionWords, const ChildProcessResult& result, int exitCodeOfSuccess)
{
    if (!result.exitedWith(exitCodeOfSuccess)) [[unlikely]]
        failNativeTestBecause(file, line, makeString('(', optionWords, ") the process "_s, result.description(), "; printed: "_s, result.output));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
