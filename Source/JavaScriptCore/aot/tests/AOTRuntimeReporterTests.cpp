/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTHost.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTTemporaryFile.h"
#include "JSCInlines.h"
#include "Options.h"
#include <stdlib.h>
#include <tuple>
#include <wtf/DataLog.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral countsAtVMDestructionTest = "runtime-reporter/counts-are-written-at-vm-destruction"_s;
static constexpr ASCIILiteral countsAtExitTest = "runtime-reporter/counts-are-written-by-exit-hook"_s;
static constexpr ASCIILiteral countsOnceTest = "runtime-reporter/counts-are-written-once"_s;
static constexpr ASCIILiteral countsPathWithProcessIDTest = "runtime-reporter/counts-path-names-process"_s;
static constexpr ASCIILiteral noCountsPathTest = "runtime-reporter/nothing-is-written-without-counts-path"_s;

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static String countsPathOption()
{
    return String::fromUTF8(byteCast<char>(Options::aotCountsPath()));
}

static unsigned numberOfTimesCountsWereWritten(const String& path)
{
    constexpr ASCIILiteral lineOfEveryWrite = "\tcounter:FieldsLeftSlotByDelete\n"_s;
    AOT_TEST_CHECK(FileSystem::fileExists(path));
    auto bytes = FileSystem::readEntireFile(path);
    String text = bytes ? String::fromUTF8(bytes->span()) : emptyString();
    unsigned count = 0;
    for (size_t position = text.find(lineOfEveryWrite); position != notFound; position = text.find(lineOfEveryWrite, position + 1))
        ++count;
    return count;
}

static void runChildWithCountsPath(ASCIILiteral test, const String& imagePath, const String& countsPath)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(test),
        makeString("--aotImagePath="_s, imagePath), makeString("--aotCountsPath="_s, countsPath),
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that writes counts ", result.description());
        failNativeTest(__FILE__, __LINE__, test.characters());
    }
}

static void checkChildWritesCountsOnce(ASCIILiteral test, LoadedProgramTest bodyOfChild)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(test, programText, bodyOfChild);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    TemporaryFile counts(".counts"_s, { });
    runChildWithCountsPath(test, image.path(), counts.path());
    AOT_TEST_CHECK(numberOfTimesCountsWereWritten(counts.path()) == 1);
    AOT_TEST_CHECK(!FileSystem::fileExists(makeString(counts.path(), ".operations"_s)));
}

static void testCountsAreWrittenAtVMDestruction()
{
    checkChildWritesCountsOnce(countsAtVMDestructionTest, [](LoadedProgram&) {
        AOT_TEST_CHECK(!numberOfTimesCountsWereWritten(countsPathOption()));
    });
}

static void testCountsAreWrittenByExitHook()
{
    checkChildWritesCountsOnce(countsAtExitTest, [](LoadedProgram&) {
        AOT_TEST_CHECK(!numberOfTimesCountsWereWritten(countsPathOption()));
        exit(EXIT_SUCCESS);
    });
}

static void testCountsAreWrittenOnce()
{
    checkChildWritesCountsOnce(countsOnceTest, [](LoadedProgram& program) {
        RuntimeReporter& reporter = *program.vm().aotState().reporter;
        reporter.writeAtExit();
        AOT_TEST_CHECK(numberOfTimesCountsWereWritten(countsPathOption()) == 1);
        reporter.writeAtExit();
        AOT_TEST_CHECK(numberOfTimesCountsWereWritten(countsPathOption()) == 1);
    });
}

static void checkCountsPathNamesProcess(LoadedProgram& program)
{
    String path = makeStringByReplacingAll(countsPathOption(), "%p"_s, String::number(getCurrentProcessID()));
    AOT_TEST_CHECK(path != countsPathOption());
    AOT_TEST_CHECK(!FileSystem::fileExists(path));
    program.vm().aotState().reporter->writeAtExit();
    AOT_TEST_CHECK(numberOfTimesCountsWereWritten(path) == 1);
    AOT_TEST_CHECK(!FileSystem::fileExists(countsPathOption()));
    AOT_TEST_CHECK(FileSystem::deleteFile(path));
}

static void testCountsPathNamesProcess()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(countsPathWithProcessIDTest, programText, checkCountsPathNamesProcess);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    runChildWithCountsPath(countsPathWithProcessIDTest, image.path(), makeString(image.path(), ".%p.counts"_s));
}

static void testNothingIsWrittenWithoutCountsPath()
{
    runWithLoadedProgram(noCountsPathTest, programText, [](LoadedProgram& program) {
        AOT_TEST_CHECK(!Options::aotCountsPath());
        AOT_TEST_CHECK(!program.vm().aotState().runtimeTable->countsCalls());
        program.vm().aotState().reporter->writeAtExit();
    });
}

} } // namespace JSC::AOT

#if ENABLE(WEBASSEMBLY)

#include "AOTWasmInterface.h"

namespace JSC { namespace AOT {

using Wasm::InterpreterFallbackReason;

static constexpr ASCIILiteral fallbackWithoutLogTest = "runtime-reporter/interpreter-fallback-without-log"_s;

namespace {

enum class LogFormat : bool { Text, JSON };

class MismatchLog {
    WTF_MAKE_NONCOPYABLE(MismatchLog);
public:
    explicit MismatchLog(LogFormat format)
    {
        std::tie(m_path, m_file) = FileSystem::openTemporaryFile("testaot"_s);
        AOT_TEST_CHECK(m_file.isValid());
        setMismatchLog(descriptor(), format == LogFormat::JSON);
    }

    ~MismatchLog()
    {
        disable();
        m_file = { };
        FileSystem::deleteFile(m_path);
    }

    void disable() { setMismatchLog(-1, false); }

    String text()
    {
        AOT_TEST_CHECK(m_file.seek(0, FileSystem::FileSeekOrigin::Beginning).has_value());
        auto bytes = m_file.readAll();
        return bytes ? String::fromUTF8(bytes->span()) : emptyString();
    }

private:
    intptr_t descriptor() const
    {
#if OS(WINDOWS)
        return reinterpret_cast<intptr_t>(m_file.platformHandle());
#else
        return m_file.platformHandle();
#endif
    }

    String m_path;
    FileSystem::FileHandle m_file;
};

} // anonymous namespace

static void testInterpreterFallbackText()
{
    MismatchLog log(LogFormat::Text);
    reportInterpreterFallback(InterpreterFallbackReason::MemoryChecksBounds);
    AOT_TEST_CHECK(log.text() == "[aot] interpreter-fallback no-guard-pages: a compiled WebAssembly module is running in the interpreter because its memory has no guard pages\n"_s);
}

static void testInterpreterFallbackLinesAreAppended()
{
    MismatchLog log(LogFormat::Text);
    reportInterpreterFallback(InterpreterFallbackReason::MemoryChecksBounds);
    String firstLine = log.text();
    reportInterpreterFallback(InterpreterFallbackReason::VectorsAreDisabled);
    String text = log.text();
    AOT_TEST_CHECK(text.startsWith(firstLine));
    AOT_TEST_CHECK(StringView(text).substring(firstLine.length()) == "[aot] interpreter-fallback no-simd: a compiled WebAssembly module is running in the interpreter because SIMD isn't available on this CPU\n"_s);
}

static void testInterpreterFallbackJSON()
{
    MismatchLog log(LogFormat::JSON);
    reportInterpreterFallback(InterpreterFallbackReason::MemoryChecksBounds);
    reportInterpreterFallback(InterpreterFallbackReason::VectorsAreDisabled);
    AOT_TEST_CHECK(log.text() == "{\"aot\":\"interpreter-fallback\",\"reason\":\"no-guard-pages\"}\n{\"aot\":\"interpreter-fallback\",\"reason\":\"no-simd\"}\n"_s);
}

static void testDisabledLogGetsNoLine()
{
    MismatchLog log(LogFormat::Text);
    log.disable();
    reportInterpreterFallback(InterpreterFallbackReason::MemoryChecksBounds);
    AOT_TEST_CHECK(log.text().isEmpty());
}

static void testInterpreterFallbackWithoutLog()
{
    reportInterpreterFallback(InterpreterFallbackReason::MemoryChecksBounds);
}

static void testInterpreterFallbackIsSilentByDefault()
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(fallbackWithoutLogTest) };
    ChildProcessResult result = launcher.run(arguments.span());
    AOT_TEST_CHECK(result.exitedWith(0));
    AOT_TEST_CHECK(result.output.contains(fallbackWithoutLogTest));
    AOT_TEST_CHECK(!result.output.contains("[aot]"_s));
}

} } // namespace JSC::AOT

#endif // ENABLE(WEBASSEMBLY)

namespace JSC { namespace AOT {

std::span<const NativeTest> runtimeReporterTests()
{
    static constexpr NativeTest tests[] = {
#if ENABLE(WEBASSEMBLY)
        { fallbackWithoutLogTest, testInterpreterFallbackWithoutLog },
        { "runtime-reporter/interpreter-fallback-is-silent-by-default"_s, testInterpreterFallbackIsSilentByDefault },
        { "runtime-reporter/interpreter-fallback-text"_s, testInterpreterFallbackText },
        { "runtime-reporter/interpreter-fallback-lines-are-appended"_s, testInterpreterFallbackLinesAreAppended },
        { "runtime-reporter/interpreter-fallback-json"_s, testInterpreterFallbackJSON },
        { "runtime-reporter/disabled-log-gets-no-line"_s, testDisabledLogGetsNoLine },
#endif
        { countsAtVMDestructionTest, testCountsAreWrittenAtVMDestruction },
        { countsAtExitTest, testCountsAreWrittenByExitHook },
        { countsOnceTest, testCountsAreWrittenOnce },
        { countsPathWithProcessIDTest, testCountsPathNamesProcess },
        { noCountsPathTest, testNothingIsWrittenWithoutCountsPath },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
