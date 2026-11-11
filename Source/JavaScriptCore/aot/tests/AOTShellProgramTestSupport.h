/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTNativeTestReasons.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

inline std::unique_ptr<CompileOutput> compileForShellOnThisThread(ASCIILiteral programText, OptionWords optionWords, ASCIILiteral way = ""_s)
{
    Wire::CompileOptions options = makeOptionsOfTestedProgram();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), build);
    if (!program)
        failBecauseTextIsNoProgram(__FILE__, __LINE__, build, optionWordsOfReason(way, optionWords));

    std::unique_ptr<CompileOutput> output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfWay(way));
    if (!output->hasImage())
        failBecauseThereIsNoImage(__FILE__, __LINE__, *output, optionWordsOfReason(way, optionWords));
    AOT_TEST_CHECK(!output->section(Wire::ResultSection::EntryModuleIds).empty());
    return output;
}

inline String outputOfProgramInShell(ASCIILiteral programText, OptionWords optionWords, ASCIILiteral statementsBeforeProgram = ""_s, OptionWords shellOptions = { }, ASCIILiteral way = ""_s)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programText, optionWords, way);
    });

    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()) };
    for (ASCIILiteral option : shellOptions)
        arguments.append(String(option));
    arguments.append(String("-e"_s));
    arguments.append(makeString("globalThis.id = value => value; "_s, statementsBeforeProgram, " runCompiledEntryPoint("_s, output->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ").catch(error => print(\"the program threw \" + error));"_s));

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    checkThatChildSucceeded(__FILE__, __LINE__, optionWordsOfReason(way, optionWords), result, EXIT_SUCCESS);
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

inline void checkOutputOfProgramInShell(ASCIILiteral programText, ASCIILiteral expectedOutput, OptionWords optionWords, ASCIILiteral statementsBeforeProgram = ""_s, OptionWords shellOptions = { })
{
    for (ASCIILiteral way : waysToCompilePrograms) {
        String output = outputOfProgramInShell(programText, optionWords, statementsBeforeProgram, shellOptions, way);
        checkPrintedLines(__FILE__, __LINE__, optionWordsOfReason(way, optionWords), output, expectedOutput);
    }
}

#define COUNTED_CALLS(NAMES_OF_OPERATIONS) "const countedOperations = [" NAMES_OF_OPERATIONS "];" R"JS(
const countedCalls = () => {
    let sum = 0;
    for (const name of countedOperations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
let countsCalls = aotOperationCount("operationAOTGetById") !== null;
const servedBy = (before, accesses, most = 3) => {
    const made = countedCalls() - before;
    return !countsCalls ? "" : ", " + (made <= most ? "cached" : made <= 8 && accesses >= 400 ? "the object answers" : made >= accesses ? "not cached" : made + " calls");
};
)JS"

struct WordOfService {
    char16_t letter;
    ASCIILiteral word;
};
inline constexpr WordOfService wordsOfCaches[] { { 'c', ", cached"_s }, { 'o', ", the object answers"_s }, { 'n', ", not cached"_s } };
inline constexpr WordOfService wordsOfLookups[] { { 'o', "objects"_s }, { 'f', "a few operations"_s }, { 'n', "operations"_s } };

inline void checkServicesInShell(const char* file, int line, ASCIILiteral programText, ASCIILiteral output, ASCIILiteral statements, std::span<const WordOfService> words, ASCIILiteral lettersOfLines, OptionWords optionWords = { }, OptionWords shellOptions = { "--aotCountsPath=/dev/null"_s })
{
    constexpr ASCIILiteral way = waysToCompilePrograms[0];
    constexpr ASCIILiteral answer = "right"_s;
    StringView letters { lettersOfLines };
    StringBuilder expected;
    unsigned next = 0;
    for (StringView text : StringView(output).split('\n')) {
        AOT_TEST_CHECK(next < letters.length());
        bool isGroup = letters[next] == '(';
        unsigned end = isGroup ? static_cast<unsigned>(letters.find(')', next)) : next + 1;
        StringView ofLine = isGroup ? letters.substring(next + 1, end - next - 1) : letters.substring(next, 1);
        next = isGroup ? end + 1 : end;
        auto wordAt = [&](unsigned index) {
            char16_t letter = ofLine[ofLine.length() == 1 ? 0 : index];
            auto found = std::ranges::find(words, letter, &WordOfService::letter);
            AOT_TEST_CHECK(found != words.end());
            return found->word;
        };
        unsigned numberOfAnswers = 0;
        unsigned begin = 0;
        if (ofLine[0] != '.') {
            for (size_t place; (place = text.find(answer, begin)) != notFound; begin = static_cast<unsigned>(place + answer.length()))
                expected.append(text.substring(begin, static_cast<unsigned>(place) - begin), wordAt(numberOfAnswers++));
        }
        expected.append(text.substring(begin));
        if (ofLine[0] != '.' && !numberOfAnswers)
            expected.append(wordAt(0));
        expected.append('\n');
    }
    AOT_TEST_CHECK(next == letters.length());
    String printed = outputOfProgramInShell(programText, optionWords, statements, shellOptions, way);
    checkPrintedLines(file, line, optionWordsOfReason(way, optionWords), printed, expected.toString());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
