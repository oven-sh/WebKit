/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineChecker.h"
#include "AOTMachinePrinter.h"
#include "AOTMachineTesting.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/text/WTFString.h>

// What the native tests of the back end share on top of machine/AOTMachineTesting.h, which has TestProcedure, buildRandomProcedure() and interpret().

namespace JSC { namespace AOT { namespace Machine {

// ---- AOTMachineTestPatches.cpp

// What the instance register points at while code of a test runs: `words`. The words below it stand for the prefix of an instance.
struct alignas(16) TestInstance {
    std::array<uint64_t, 64> wordsBelow { };
    std::array<uint64_t, 64> words { };

    void* pointer() { return words.data(); }
    // Writes each constant that code reads from a word of the instance into its word: Machine::findPinnedConstant().
    void fillPinnedConstants();
};

// TestProcedure::prepare() or compile(). Where it fails, the native test fails with the kind of the failure, the procedure and, if there is one, the allocation as text.
// Without an instance the code runs against one of compileOrFail()'s own, which has its pinned constants filled and lives as long as the process: the instance register is never null.
// All tests share that one, so a test whose code writes to the instance brings its own.
void prepareOrFail(TestProcedure&);
void compileOrFail(TestProcedure&, void* instance = nullptr);

template<typename Result, typename... Arguments>
Result compileAndRun(TestProcedure& test, Arguments... arguments)
{
    compileOrFail(test);
    return test.call<Result>(arguments...);
}

enum class TestPatchKind : uint8_t {
    // The result is the wrapping sum of the operands, read as 64-bit integers or as the bits of doubles. Its code writes `poison` to every register of its clobber sets and to its scratch registers.
    Sum,
    // Sum, through a real call of a C++ function: it needs a frame and clobbers what the C convention lets a callee write.
    SumThroughCall,
    // Ends its block. Takes the second successor, a landing pad, where the sum of the operands is odd. On that way it poisons every register that is not callee-saved.
    SumOrThrow,
};

inline constexpr uint64_t poison = 0xbadbeef0badbeef0ull;

struct TestPatch {
    TestPatchKind kind { TestPatchKind::Sum };
    Type result { Int64 };
    std::span<const ConstrainedValue> operands;
    RegisterMask earlyClobbers { 0 };
    RegisterMask lateClobbers { 0 };
    uint8_t numberOfGPScratchRegisters { 0 };
    uint8_t numberOfFPScratchRegisters { 0 };
    // Of SumOrThrow.
    Edge normalSuccessor;
    Edge landingPad;
};

// Appends it to the block that Output stands in.
PatchOperation* appendTestPatch(TestProcedure&, const TestPatch&);

// `count` values that are defined here and all read by the Sum that sumOf() appends: more of them than a bank has registers makes pressure.
Vector<Operation*> defineValues(TestProcedure&, Operation* seed, unsigned count, Type);
Operation* sumOf(TestProcedure&, std::span<Operation* const>);

// ---- The fuzzer of the allocator. AOTMachineFuzzer.cpp

// Every fourth seed has handlers, every eighth saves its frame on suspension. The rest of the shape is drawn by buildRandomProcedure() from the seed.
ProcedureTraits traitsOfSeed(uint64_t seed);

// Builds, compiles with every check of AOTMachineChecker.h on, runs on three pairs of arguments and compares with interpret(). Null where all agree, else one line.
String fuzzOneProcedure(uint64_t seed, unsigned numberOfBlocks);

struct AllocationFuzzRequest {
    uint64_t seed { 1 };
    unsigned count { 1000 };
    unsigned numberOfBlocks { 12 };
    // Prints the procedure, the order, the liveness, the allocation and the frame of that seed and runs nothing else.
    std::optional<uint64_t> seedToShow;
};

// One line `FAIL seed <n>: <why>` for each seed that fails. Answers how many.
unsigned fuzzAllocation(const AllocationFuzzRequest&, PrintStream&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
