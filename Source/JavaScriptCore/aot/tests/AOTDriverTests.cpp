/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCheckLines.h"
#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTSchemaEnums.h"
#include "AOTTemporaryFile.h"
#include "AOTTestFile.h"
#include "AOTTestHarness.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "Options.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using namespace Typed;

namespace {

} // anonymous namespace

static constexpr ASCIILiteral sampleProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"sum\" parameters=[{type=number}, {type=number}] results=[{type=number}] {\n"
    "  b0(v0: number, v1: number):\n"
    "    v2 = Add v0, v1\n"
    "    v3 = Mul v2, v2\n"
    "    v4 = Sub v3, v0\n"
    "    Return v4\n"
    "}\n"
    "entry\n"_s;

static ChildProcessResult runPrintWire(const TemporaryFile& program, std::initializer_list<ASCIILiteral> otherArguments = { })
{
    Vector<String> arguments { "print"_s, program.path(), "--wire"_s };
    for (ASCIILiteral argument : otherArguments)
        arguments.append(argument);
    return ChildProcessLauncher(String(programPath()), { }).run(arguments.span());
}

static void expectExitCode(const ChildProcessResult& result, int exitCode)
{
    if (!result.exitedWith(exitCode))
        dataLogLn("testaot ", result.description(), ":\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(exitCode));
}

static StringView lastLineOf(StringView output)
{
    StringView lastLine;
    for (StringView line : output.split('\n'))
        lastLine = line;
    return lastLine;
}

static void expectRejectedWith(const ChildProcessResult& result, InternalErrorKind kind)
{
    expectExitCode(result, exitCodeForFailure);
    AOT_TEST_CHECK(lastLineOf(result.output).startsWith(makeString(nameOf(kind), ": "_s)));
}

static void expectWireText(StringView text, std::span<const uint8_t> bytes)
{
    BuildContext build(makeDefaultCompileOptions());
    AOT_TEST_CHECK(equalSpans(encodeProgramText(text, build).span(), bytes));
}

static void testPrintWireAccepts()
{
    BuildContext build(makeDefaultCompileOptions());
    Vector<uint8_t> bytes = encodeProgramText(sampleProgram, build);
    AOT_TEST_CHECK(programKindOf(bytes.span()) == ProgramFile::Kind::Bytes);
    TemporaryFile program(".bin"_s, bytes.span());
    ChildProcessResult result = runPrintWire(program);
    expectExitCode(result, exitCodeForSuccess);
    expectWireText(result.output, bytes.span());
}

static void testPrintWireRejects()
{
    BuildContext build(makeDefaultCompileOptions());
    Vector<uint8_t> bytes = encodeProgramText(sampleProgram, build);
    AOT_TEST_CHECK(bytes.size() >= sizeof(Wire::Header));
    bytes[offsetof(Wire::Header, schemaHash)] ^= 0xff;
    TemporaryFile damagedProgram(".bin"_s, bytes.span());
    expectRejectedWith(runPrintWire(damagedProgram), InternalErrorKind::SchemaMismatch);

    String withoutEntry = makeStringByReplacingAll(sampleProgram, "entry\n"_s, ""_s);
    CString withoutEntryText = withoutEntry.utf8();
    TemporaryFile programWithoutEntry(".ir"_s, byteCast<uint8_t>(withoutEntryText.span()));
    expectRejectedWith(runPrintWire(programWithoutEntry), InternalErrorKind::NoEntryPoint);
}

static void testPrintWireTakesNoOtherFlag()
{
    TemporaryFile program(".ir"_s, byteCast<uint8_t>(sampleProgram.span8()));
    expectExitCode(runPrintWire(program, { "no-such-word"_s }), exitCodeForInvalidCommandLine);
}

std::span<const NativeTest> driverTests()
{
    static constexpr NativeTest tests[] = {
        { "subcommands/print-wire-accepts"_s, testPrintWireAccepts },
        { "subcommands/print-wire-rejects"_s, testPrintWireRejects },
        { "subcommands/print-wire-takes-no-other-flag"_s, testPrintWireTakesNoOtherFlag },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
