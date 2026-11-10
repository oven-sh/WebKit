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

namespace JSC { namespace AOT { namespace Machine {

class TestProcedure {
    WTF_MAKE_NONCOPYABLE(TestProcedure);
public:
    explicit TestProcedure(ProcedureTraits = { });
    ~TestProcedure();

    Procedure& procedure() { return *m_procedure; }
    Output& out() { return *m_out; }
    const AOTHeaps& heaps() const;

    LValue argumentGPR(unsigned index) { return out().argumentRegister(GPRInfo::toArgumentRegister(index)); }
    LValue argumentFPR(unsigned index) { return out().argumentRegister(FPRInfo::toArgumentRegister(index)); }

    bool prepare();
    bool compile(void* instance = nullptr, std::optional<uint32_t> branchReach = std::nullopt);

    template<typename Result, typename... Arguments>
    Result call(Arguments... arguments)
    {
        return reinterpret_cast<Result (SYSV_ABI *)(Arguments...)>(entry())(arguments...);
    }

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

struct RandomProcedureOptions {
    uint64_t seed { 0 };
    unsigned numberOfBlocks { 12 };
    unsigned operationsPerBlock { 8 };
    bool withCalls { true };
    bool withRareRegions { true };
    bool withLandingPads { true };
    bool withNumbers { true };
    bool withScaledSums { false };
    bool withCombinedOperands { false };
    bool withArmsThatReturn { false };
};
struct RandomProgram;
std::unique_ptr<RandomProgram, void (*)(RandomProgram*)> buildRandomProcedure(TestProcedure&, const RandomProcedureOptions&);
int64_t interpret(const RandomProgram&, int64_t a, int64_t b);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
