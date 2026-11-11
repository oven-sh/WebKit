/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTestSupport.h"

#if ENABLE(AOT)

#include "AOTMachineTarget.h"
#include "AOTTestHarness.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <initializer_list>
#include <optional>
#include <wtf/FilePrintStream.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Machine {

namespace {

struct FuzzArguments {
    int64_t a;
    int64_t b;
};

enum class FuzzProgress : bool { Silent, NamesEachSeedFirst };

} // anonymous namespace

static constexpr uint64_t scrambleFuzzSeed(uint64_t bits)
{
    bits += 0x9e3779b97f4a7c15ull;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ull;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebull;
    return bits ^ (bits >> 31);
}

static std::array<FuzzArguments, 3> argumentsOfFuzzSeed(uint64_t seed)
{
    uint64_t a = scrambleFuzzSeed(seed);
    uint64_t b = scrambleFuzzSeed(a);
    return { { { 0, 0 }, { 1, -1 }, { static_cast<int64_t>(a), static_cast<int64_t>(b) } } };
}

static constexpr uint64_t firstFuzzSeedWithScaledSums = 400001;
static constexpr uint64_t firstFuzzSeedWithArmsThatReturn = 500001;

static RandomProcedureOptions optionsOfFuzzSeed(uint64_t seed, unsigned numberOfBlocks)
{
    RandomProcedureOptions options;
    options.seed = seed;
    options.numberOfBlocks = numberOfBlocks;
    options.withScaledSums = seed >= firstFuzzSeedWithScaledSums;
    options.withArmsThatReturn = seed >= firstFuzzSeedWithArmsThatReturn;
    return options;
}

ProcedureTraits traitsOfSeed(uint64_t seed)
{
    ProcedureTraits traits;
    traits.hasHandlers = !(seed % 4);
    traits.savesFrameOnSuspend = !(seed % 8);
    traits.chargesForCalleeSaves = seed & 2;
    traits.mayOmitFrameRecord = seed % 3;
    traits.checksAllocation = true;
    traits.shouldUseFallbackBlockOrder = !(seed % 5);
    return traits;
}

static void fillWordOfFuzzInstance(TestInstance& instance, uint64_t bits)
{
    std::optional<PinnedConstant> pinned = findPinnedConstant(bits);
    if (!pinned || !pinned->offset)
        return;

    constexpr int32_t bytesOfWord = sizeof(uint64_t);
    int32_t word = *pinned->offset / bytesOfWord;
    int32_t numberOfWordsBelow = static_cast<int32_t>(instance.wordsBelow.size());
    AOT_TEST_CHECK(pinned->reg == instanceGPR && !(*pinned->offset % bytesOfWord));
    AOT_TEST_CHECK(word >= -numberOfWordsBelow && word < static_cast<int32_t>(instance.words.size()));
    if (word < 0)
        instance.wordsBelow[static_cast<size_t>(numberOfWordsBelow + word)] = bits;
    else
        instance.words[static_cast<size_t>(word)] = bits;
}

static void fillFuzzInstance(TestInstance& instance, const Procedure& procedure)
{
    for (uint32_t index = 0; index < procedure.numberOfOperations(); ++index) {
        const Operation* operation = procedure.operationAt(index);
        if (operation->isConstant())
            fillWordOfFuzzInstance(instance, static_cast<uint64_t>(operation->constantBits()));
    }
    for (uint64_t bits : { std::bit_cast<uint64_t>(1.0), PNaNAsBits, uint64_t { 1 } << 63 })
        fillWordOfFuzzInstance(instance, bits);
}

static String describeFailureOfFuzzedProcedure(ASCIILiteral step, const Procedure& procedure)
{
    RELEASE_ASSERT(procedure.failure());
    const Failure& failure = *procedure.failure();
    StringPrintStream out;
    out.print(step, " fails with ", failure.kind);
    if (failure.operation < procedure.numberOfOperations()) {
        out.print(" at ");
        printOperation(out, procedure, procedure.operationAt(failure.operation));
    }
    return out.toString();
}

String fuzzOneProcedure(uint64_t seed, unsigned numberOfBlocks)
{
    TestProcedure test(traitsOfSeed(seed));
    auto program = buildRandomProcedure(test, optionsOfFuzzSeed(seed, numberOfBlocks));
    if (!test.prepare())
        return describeFailureOfFuzzedProcedure("prepare()"_s, test.procedure());

    TestInstance instance;
    fillFuzzInstance(instance, test.procedure());
    if (!test.compile(instance.pointer()))
        return describeFailureOfFuzzedProcedure("compile()"_s, test.procedure());

    for (auto [a, b] : argumentsOfFuzzSeed(seed)) {
        int64_t expected = interpret(*program, a, b);
        if (test.call<int64_t>(a, b) != expected)
            return WTF::toString("called with (", a, ", ", b, ") the code does not answer ", expected);
    }
    return { };
}

static void printTextOfFuzzedProcedure(PrintStream& out, ASCIILiteral heading, const String& text)
{
    out.println("---- ", heading);
    if (text.isNull()) {
        out.println("not reached");
        return;
    }
    out.print(text);
    if (!StringView(text).endsWith('\n'))
        out.println();
}

static unsigned showFuzzedProcedure(uint64_t seed, unsigned numberOfBlocks, PrintStream& out)
{
    ProcedureTraits traits = traitsOfSeed(seed);
    TestProcedure test(traits);
    auto program = buildRandomProcedure(test, optionsOfFuzzSeed(seed, numberOfBlocks));
    out.println("seed ", seed, ": ", numberOfBlocks, " blocks", traits.hasHandlers ? ", has handlers" : "", traits.savesFrameOnSuspend ? ", saves its frame on suspension" : "", traits.shouldUseFallbackBlockOrder ? ", is ordered as when ordered again" : "");

    printTextOfFuzzedProcedure(out, "procedure"_s, test.printedProcedure());
    out.flush();

    bool isPrepared = test.prepare();
    printTextOfFuzzedProcedure(out, "order"_s, test.printedOrder());
    printTextOfFuzzedProcedure(out, "liveness"_s, test.printedLiveness());
    printTextOfFuzzedProcedure(out, "allocation"_s, test.printedAllocation());
    printTextOfFuzzedProcedure(out, "frame"_s, test.printedFrame());
    if (isPrepared)
        return 0;
    out.println("FAIL seed ", seed, ": ", describeFailureOfFuzzedProcedure("prepare()"_s, test.procedure()));
    return 1;
}

static unsigned fuzzSeedsInTurn(const AllocationFuzzRequest& request, FuzzProgress progress, PrintStream& out)
{
    if (request.seedToShow)
        return showFuzzedProcedure(*request.seedToShow, request.numberOfBlocks, out);

    unsigned numberOfFailures = 0;
    for (unsigned index = 0; index < request.count; ++index) {
        uint64_t seed = request.seed + index;
        if (progress == FuzzProgress::NamesEachSeedFirst) {
            out.println("seed ", seed);
            out.flush();
        }
        String reason = fuzzOneProcedure(seed, request.numberOfBlocks);
        if (reason.isNull())
            continue;
        out.println("FAIL seed ", seed, " --blocks=", request.numberOfBlocks, ": ", reason);
        out.flush();
        ++numberOfFailures;
    }
    return numberOfFailures;
}

unsigned fuzzAllocation(const AllocationFuzzRequest& request, PrintStream& out)
{
    return fuzzSeedsInTurn(request, FuzzProgress::Silent, out);
}

} // namespace Machine

namespace Typed {

static constexpr ASCIILiteral fuzzSeedFlag = "--seed"_s;
static constexpr ASCIILiteral fuzzCountFlag = "--count"_s;
static constexpr ASCIILiteral fuzzBlocksFlag = "--blocks"_s;
static constexpr ASCIILiteral fuzzShowFlag = "--show"_s;

template<typename... Types>
static int refuseFuzzAllocationArguments(const Types&... sentence)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);
    errors.println("testaot fuzz-allocation: ", sentence...);
    return exitCodeForInvalidCommandLine;
}

template<typename Number>
static ASCIILiteral parseFuzzFlagValue(StringView text, std::optional<Number>& number)
{
    if (number)
        return "may be specified only once"_s;
    number = parseInteger<Number>(text);
    return number ? ASCIILiteral() : "takes a decimal number"_s;
}

int runFuzzAllocationSubcommand(std::span<const String> arguments)
{
    Machine::AllocationFuzzRequest request;
    std::optional<uint64_t> seed;
    std::optional<unsigned> count;
    std::optional<unsigned> numberOfBlocks;
    for (StringView argument : arguments) {
        size_t equalsSign = argument.find('=');
        StringView name = equalsSign == notFound ? argument : argument.left(equalsSign);
        StringView value = equalsSign == notFound ? StringView() : argument.substring(equalsSign + 1);

        ASCIILiteral complaint;
        if (name == fuzzSeedFlag)
            complaint = parseFuzzFlagValue(value, seed);
        else if (name == fuzzCountFlag)
            complaint = parseFuzzFlagValue(value, count);
        else if (name == fuzzBlocksFlag)
            complaint = parseFuzzFlagValue(value, numberOfBlocks);
        else if (name == fuzzShowFlag)
            complaint = parseFuzzFlagValue(value, request.seedToShow);
        else
            return refuseFuzzAllocationArguments("unknown argument ", argument);
        if (!complaint.isNull())
            return refuseFuzzAllocationArguments(name, " ", complaint);
    }
    if (count && !*count)
        return refuseFuzzAllocationArguments(fuzzCountFlag, " takes a number above 0");
    request.seed = seed.value_or(request.seed);
    request.count = count.value_or(request.count);
    request.numberOfBlocks = numberOfBlocks.value_or(request.numberOfBlocks);

    static constexpr unsigned usualNumbersOfBlocks[] { 12, 16, 24 };
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    unsigned numberOfFailures = 0;
    unsigned numberRun = 0;
    for (unsigned usual : usualNumbersOfBlocks) {
        if (numberOfBlocks || request.seedToShow) {
            if (usual != usualNumbersOfBlocks[0])
                break;
        } else
            request.numberOfBlocks = usual;
        numberOfFailures += Machine::fuzzSeedsInTurn(request, Machine::FuzzProgress::NamesEachSeedFirst, out);
        numberRun += request.count;
    }
    if (!request.seedToShow)
        out.println(numberRun - numberOfFailures, " passed, ", numberOfFailures, " failed, 0 todo");
    out.flush();
    return numberOfFailures ? exitCodeForFailure : exitCodeForSuccess;
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
