/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHeaps.h"
#include "AOTOutput.h"
#include "MacroAssemblerCodeRef.h"
#include <memory>
#include <wtf/text/WTFString.h>

// For tests in the manner of testb3: operations are built through Output, compiled, and run in the test process.

namespace JSC { namespace AOT { namespace Machine {

class TestProcedure {
    WTF_MAKE_NONCOPYABLE(TestProcedure);
public:
    // Checks are on whatever the traits say.
    explicit TestProcedure(ProcedureTraits = { });
    ~TestProcedure();

    Procedure& procedure() { return *m_procedure; }
    Output& out() { return *m_out; }
    const AOTHeaps& heaps() const;
    // Output::m_block is the block of the primary entrypoint at first.

    // The arguments of the calling convention that call() goes by: the C one, which on x86-64 is System V's, on Windows too.
    LValue argumentGPR(unsigned index) { return out().argumentRegister(GPRInfo::toArgumentRegister(index)); }
    LValue argumentFPR(unsigned index) { return out().argumentRegister(FPRInfo::toArgumentRegister(index)); }

    // prepareForGeneration(). False after a failure: procedure().failure().
    bool prepare();
    // prepare(), generate(), link into executable memory. Relocations against SymbolSpace::Constant and JumpTable are resolved against copies that live as long as this object. Any other space fails the test.
    // A wrapper around the code saves the registers that compiled code takes for its own and the convention of call() wants kept, and sets the pinned registers: the instance register to `instance`, the tags, the cage base to zero.
    bool compile(void* instance = nullptr);

    template<typename Result, typename... Arguments>
    Result call(Arguments... arguments)
    {
        return reinterpret_cast<Result (SYSV_ABI *)(Arguments...)>(entry())(arguments...);
    }

    // The bytes that compile() has put into executable memory, as they are linked.
    std::span<const uint8_t> linkedCode() const;

    String printedProcedure() const;
    String printedOrder() const;
    String printedLiveness() const;
    String printedAllocation() const;
    String printedFrame() const;

private:
    void* entry() const;

    struct Storage;
    std::unique_ptr<Storage> m_storage;
    Procedure* m_procedure { nullptr };
    Output* m_out { nullptr };
};

// Seeded, so a failure can be run again. Builds a procedure with random reducible control flow, loops, rare arms, landing pads, Patches with random constraints and clobbers, in both banks,
// together with an interpreter's answer for it. AOTMachineRandomProcedure.cpp.
struct RandomProcedureOptions {
    uint64_t seed { 0 };
    unsigned numberOfBlocks { 12 };
    unsigned operationsPerBlock { 8 };
    bool withCalls { true };
    bool withRareRegions { true };
    bool withLandingPads { true };
    bool withNumbers { true };
    // base + (index << amount), mostly by 1 to 3, and the choice between that and the index by a test of the index. Without them a seed builds what it built before there were any.
    bool withScaledSums { false };
    // Most arms outside rare regions end in a Return, most of those behind a Patch, which is a call more often than not: what a later arm reads is dead there. Without them a seed builds what it built before there were any.
    bool withArmsThatReturn { false };
};
// Fills the procedure, which takes two Int64 arguments and returns an Int64. interpret() says what it has to answer.
struct RandomProgram;
std::unique_ptr<RandomProgram, void (*)(RandomProgram*)> buildRandomProcedure(TestProcedure&, const RandomProcedureOptions&);
int64_t interpret(const RandomProgram&, int64_t a, int64_t b);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
