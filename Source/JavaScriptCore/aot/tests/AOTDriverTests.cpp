/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
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

static void testPrintWireRejectsNamesOfBuiltinsThatStringsDoNotSpell()
{
    BuildContext build(makeDefaultCompileOptions());
    Vector<uint8_t> bytes = encodeProgramText(makeStringByReplacingAll(sampleProgram, "name=\"sum\""_s, "name=\"slice\""_s), build);
    AOT_TEST_CHECK(bytes.size() >= sizeof(Wire::Header));
    Wire::Section section = spanReinterpretCast<const Wire::Header>(bytes.span().first(sizeof(Wire::Header)))[0].sections[static_cast<size_t>(ProgramList::Strings)];
    auto stringsOf = [&](Vector<uint8_t>& program) {
        return spanReinterpretCast<Wire::String>(program.mutableSpan().subspan(section.offsetInBytes, section.count * sizeof(Wire::String)));
    };
    auto carriesName = [](const Wire::String& string) {
        return string.builtinName != Wire::none;
    };
    std::span<Wire::String> strings = stringsOf(bytes);
    size_t named = std::ranges::find_if(strings, carriesName) - strings.begin();
    size_t plain = std::ranges::find_if_not(strings, carriesName) - strings.begin();
    AOT_TEST_CHECK(named < strings.size() && plain < strings.size());
    uint32_t name = strings[named].builtinName;
    TemporaryFile wholeProgram(".bin"_s, bytes.span());
    expectExitCode(runPrintWire(wholeProgram), 0);

    struct Damage {
        size_t string;
        uint32_t builtinName;
    };
    for (Damage damage : { Damage { plain, name }, Damage { plain, BuiltinTable::singleton().textNameCount() } }) {
        Vector<uint8_t> damaged = bytes;
        stringsOf(damaged)[damage.string].builtinName = damage.builtinName;
        TemporaryFile damagedProgram(".bin"_s, damaged.span());
        expectRejectedWith(runPrintWire(damagedProgram), InternalErrorKind::NameClassMismatch);
    }
}

std::span<const NativeTest> driverTests()
{
    static constexpr NativeTest tests[] = {
        { "subcommands/print-wire-rejects-names-of-builtins-that-strings-do-not-spell"_s, testPrintWireRejectsNamesOfBuiltinsThatStringsDoNotSpell },
        { "subcommands/print-wire-rejects"_s, testPrintWireRejects },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
