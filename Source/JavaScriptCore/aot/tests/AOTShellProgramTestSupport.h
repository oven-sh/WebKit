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
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

// `way` is one of waysToCompilePrograms, or no word.
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
