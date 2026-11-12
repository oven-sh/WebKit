/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineExecutionFuzzer.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTTestHarness.h"
#include <optional>
#include <wtf/FilePrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

#if OS(LINUX) && !ASAN_ENABLED

namespace Machine {

static constexpr unsigned numberOfSeedsPerTest = ASSERT_ENABLED ? 200 : 4000;

static void runExecutionFuzzer(uint64_t seed, unsigned count, std::optional<bool> checksAllocation = std::nullopt)
{
    ExecutionFuzzRequest request;
    request.seed = seed;
    request.count = count;
    request.checksAllocation = checksAllocation;
    FilePrintStream out(stderr, FilePrintStream::Borrow);
    AOT_TEST_CHECK(!fuzzExecution(request, out));
}

template<uint64_t firstSeed>
static void testExecutionOfSeeds()
{
    runExecutionFuzzer(firstSeed, numberOfSeedsPerTest);
}

template<uint64_t firstSeed>
static void testExecutionOfCheckedSeeds()
{
    runExecutionFuzzer(firstSeed, numberOfSeedsPerTest / 4, true);
}

static void testExecutionOfPastFailures()
{
    static constexpr uint64_t seeds[] = { 347, 705867513, 705867800, 723742500, 736226774, 755987727, 788890195, 7019485, 8002881, 10055588, 20914882, 30037465, 30597012, 50014391, 1002937411, 1803356743, 3873219 };
    for (uint64_t seed : seeds)
        runExecutionFuzzer(seed, 1, true);
}

} // namespace Machine

std::span<const NativeTest> machineExecutionFuzzerTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/execution-fuzzer/past-failures"_s, Machine::testExecutionOfPastFailures },
        { "machine/execution-fuzzer/first-seeds"_s, Machine::testExecutionOfSeeds<1> },
        { "machine/execution-fuzzer/numbered-reads"_s, Machine::testExecutionOfSeeds<Machine::ExecutionFuzzer::firstSeedWithNumberedReads> },
        { "machine/execution-fuzzer/test-chains"_s, Machine::testExecutionOfSeeds<1050000000> },
        { "machine/execution-fuzzer/checked-allocation"_s, Machine::testExecutionOfCheckedSeeds<2000000> },
    };
    return tests;
}

namespace Typed {

int runFuzzExecutionSubcommand(std::span<const String> arguments)
{
    auto refuse = [](const auto&... sentence) {
        FilePrintStream errors(stderr, FilePrintStream::Borrow);
        errors.println("testaot fuzz-execution: ", sentence...);
        return exitCodeForInvalidCommandLine;
    };

    Machine::ExecutionFuzzRequest request;
    for (StringView argument : arguments) {
        if (argument == "--checked"_s || argument == "--unchecked"_s) {
            request.checksAllocation = argument == "--checked"_s;
            continue;
        }
        size_t equalsSign = argument.find('=');
        StringView name = equalsSign == notFound ? argument : argument.left(equalsSign);
        std::optional<uint64_t> number = equalsSign == notFound ? std::nullopt : parseInteger<uint64_t>(argument.substring(equalsSign + 1));
        if (!number)
            return refuse(argument, " is no flag with a decimal number");
        if (name == "--seed"_s)
            request.seed = *number;
        else if (name == "--count"_s)
            request.count = static_cast<unsigned>(*number);
        else if (name == "--show"_s)
            request.seedToShow = *number;
        else
            return refuse("unknown argument ", argument);
    }

    FilePrintStream out(stdout, FilePrintStream::Borrow);
    unsigned numberOfFailures = Machine::fuzzExecution(request, out);
    if (!request.seedToShow)
        out.println(request.count - numberOfFailures, " passed, ", numberOfFailures, " failed, 0 todo");
    out.flush();
    return numberOfFailures ? exitCodeForFailure : exitCodeForSuccess;
}

} // namespace Typed

#else

std::span<const NativeTest> machineExecutionFuzzerTests() { return { }; }

namespace Typed {

int runFuzzExecutionSubcommand(std::span<const String>)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);
    errors.println("testaot fuzz-execution: only on Linux, without AddressSanitizer");
    return exitCodeForInvalidCommandLine;
}

} // namespace Typed

#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
