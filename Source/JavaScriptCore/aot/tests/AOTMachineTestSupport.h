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

namespace JSC { namespace AOT { namespace Machine {

struct alignas(16) TestInstance {
    std::array<uint64_t, 64> wordsBelow { };
    std::array<uint64_t, 64> words { };

    void* pointer() { return words.data(); }
    void fillPinnedConstants();
};

void prepareOrFail(TestProcedure&);
void compileOrFail(TestProcedure&, void* instance = nullptr);

template<typename Result, typename... Arguments>
Result compileAndRun(TestProcedure& test, Arguments... arguments)
{
    compileOrFail(test);
    return test.call<Result>(arguments...);
}

enum class TestPatchKind : uint8_t {
    Sum,
    SumThroughCall,
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
    Edge normalSuccessor;
    Edge landingPad;
};

PatchOperation* appendTestPatch(TestProcedure&, const TestPatch&);

Vector<Operation*> defineValues(TestProcedure&, Operation* seed, unsigned count, Type);
Operation* sumOf(TestProcedure&, std::span<Operation* const>);

ProcedureTraits traitsOfSeed(uint64_t seed);

String fuzzOneProcedure(uint64_t seed, unsigned numberOfBlocks);

struct AllocationFuzzRequest {
    uint64_t seed { 1 };
    unsigned count { 1000 };
    unsigned numberOfBlocks { 12 };
    std::optional<uint64_t> seedToShow;
};

unsigned fuzzAllocation(const AllocationFuzzRequest&, PrintStream&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
