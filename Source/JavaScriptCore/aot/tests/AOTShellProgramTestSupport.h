/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using OptionWords = std::initializer_list<ASCIILiteral>;

inline std::unique_ptr<CompileOutput> compileForShellOnThisThread(ASCIILiteral programText, OptionWords optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    AOT_TEST_CHECK(!output->entryModuleIDs.isEmpty());
    return output;
}

inline String outputOfProgramInShell(ASCIILiteral programText, OptionWords optionWords, ASCIILiteral statementsBeforeProgram = ""_s, OptionWords shellOptions = { })
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programText, optionWords);
    });

    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()) };
    for (ASCIILiteral option : shellOptions)
        arguments.append(String(option));
    arguments.append(String("-e"_s));
    arguments.append(makeString("globalThis.id = value => value; "_s, statementsBeforeProgram, " runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").catch(error => print(\"the program threw \" + error));"_s));

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

inline void checkOutputOfProgramInShell(ASCIILiteral programText, ASCIILiteral expectedOutput, OptionWords optionWords, ASCIILiteral statementsBeforeProgram = ""_s, OptionWords shellOptions = { })
{
    String output = outputOfProgramInShell(programText, optionWords, statementsBeforeProgram, shellOptions);
    if (output != expectedOutput) {
        for (ASCIILiteral word : optionWords)
            dataLog(word, " ");
        dataLogLn("printed:\n", output);
    }
    AOT_TEST_CHECK(output == expectedOutput);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
