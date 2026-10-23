/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "LinkBuffer.h"
#include <wtf/StringPrintStream.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>

inline B3::Opcode relationWithSwappedOperands(B3::Opcode relation)
{
    switch (relation) {
    case LessThan:
        return GreaterThan;
    case LessEqual:
        return GreaterEqual;
    case GreaterThan:
        return LessThan;
    case GreaterEqual:
        return LessEqual;
    case Below:
        return Above;
    case BelowEqual:
        return AboveEqual;
    case Above:
        return Below;
    case AboveEqual:
        return BelowEqual;
    default:
        return relation;
    }
}

inline B3::Opcode negatedRelation(B3::Opcode relation)
{
    switch (relation) {
    case Equal:
        return NotEqual;
    case NotEqual:
        return Equal;
    case LessThan:
        return GreaterEqual;
    case LessEqual:
        return GreaterThan;
    case GreaterThan:
        return LessEqual;
    case GreaterEqual:
        return LessThan;
    case Below:
        return AboveEqual;
    case BelowEqual:
        return Above;
    case Above:
        return BelowEqual;
    case AboveEqual:
        return Below;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return relation;
    }
}

struct RandomProgramState {
    PollState poll;
    int64_t effectHash { 0 };
    std::array<int64_t, 3> failedCheckOperands { };
};

inline int64_t JIT_OPERATION_ATTRIBUTES randomProgramCallee(RandomProgramState* state, int64_t operand)
{
    state->effectHash = static_cast<int64_t>(static_cast<uint64_t>(state->effectHash) * 3 + 1000 + static_cast<uint64_t>(operand));
    return static_cast<int64_t>(static_cast<uint64_t>(operand) + 5);
}

struct PatchpointModel {
    enum class Kind : uint8_t { Nothing, Constant, AddConstant, LoadWord, IncrementWord, TruncateDouble, Observable, Poll };

    Kind kind { Kind::Nothing };
    int64_t immediate { 0 };
};

using PatchpointModels = HashMap<Value*, PatchpointModel>;

inline bool isUncloneable(Value* value)
{
    return value->kind().isCloningForbidden();
}

inline constexpr uint32_t randomProgramSpace = 1;
inline constexpr uint32_t testDataSectionSpace = 2;
inline constexpr uint32_t testJumpTableSpace = 3;

enum class RandomProgramSymbol : uint32_t {
    Callee,
    State,
};

inline constexpr uint64_t conversionFunctionID = 200;
inline constexpr uint64_t addressFunctionID = 300;

class RandomProgramRelocationLowering final : public RelocationLowering {
public:
    explicit RandomProgramRelocationLowering(RandomProgramState& state)
        : m_state(state)
    {
    }

    Value* lower(Procedure&, InsertionSet&, size_t index, const RelocatableValue&) final;

private:
    Value* lowerProgramSymbol(InsertionSet&, size_t index, const RelocatableValue&);

    RandomProgramState& m_state;
};

inline Value* RandomProgramRelocationLowering::lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    switch (symbol.space) {
    case randomProgramSpace:
        return lowerProgramSymbol(insertionSet, index, value);
    case testDataSectionSpace: {
        int64_t address = std::bit_cast<int64_t>(proc.dataSections()[symbol.index].data) + value.addend();
        return insertionSet.insert<Const64Value>(index, value.origin(), address);
    }
    case testJumpTableSpace:
    default:
        return nullptr;
    }
}

inline Value* RandomProgramRelocationLowering::lowerProgramSymbol(InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    void* base = nullptr;
    switch (static_cast<RandomProgramSymbol>(symbol.index)) {
    case RandomProgramSymbol::Callee:
        base = std::bit_cast<void*>(tagCFunction<OperationPtrTag>(randomProgramCallee));
        break;
    case RandomProgramSymbol::State:
        base = &m_state;
        break;
    }
    RELEASE_ASSERT(base);
    void* address = static_cast<uint8_t*>(base) + value.addend();

    PatchpointValue* result = insertionSet.insert<PatchpointValue>(index, pointerType(), value.origin());
    result->effects = Effects::none();
    if (!value.addend())
        result->pureFunction = PureFunction { addressFunctionID + symbol.index };
    result->setGenerator([address](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(CCallHelpers::TrustedImmPtr(address), params[0].gpr());
    });
    return result;
}

class ReproducibleRelocationLowering final : public RelocationLowering {
public:
    explicit ReproducibleRelocationLowering(RandomProgramState& state)
        : m_programLowering(state)
    {
    }

    Value* lower(Procedure&, InsertionSet&, size_t index, const RelocatableValue&) final;

private:
    static constexpr int64_t placeholderBytesPerDataSection = 1ll << 32;

    RandomProgramRelocationLowering m_programLowering;
};

inline Value* ReproducibleRelocationLowering::lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    if (symbol.space != testDataSectionSpace)
        return m_programLowering.lower(proc, insertionSet, index, value);
    int64_t placeholder = (static_cast<int64_t>(symbol.index) + 1) * placeholderBytesPerDataSection + value.addend();
    return insertionSet.insert<Const64Value>(index, value.origin(), placeholder);
}

inline Policy randomProgramPolicy(RelocationLowering& lowering)
{
    Policy policy = optimizingPolicy();

    policy.allocation.keepsValuesAfterLowering = true;

    policy.addressing.isPositionIndependent = true;
    policy.addressing.namesPinnedRegistersDirectly = true;
    policy.addressing.dataSectionSpace = testDataSectionSpace;
    policy.addressing.jumpTableSpace = testJumpTableSpace;
    policy.addressing.relocationLowering = &lowering;

    policy.switchLowering.minCasesForJumpTable = std::numeric_limits<unsigned>::max();

    policy.air.registers.chargesForCalleeSaves = true;
    policy.air.registers.discountsPreferredRegisterMoves = true;
    policy.air.registers.hintsTwoAddressOperands = true;
    policy.air.registers.usesSecondPreferredRegister = true;
    policy.air.registers.evictsHolderOfPreferredRegister = true;

    policy.air.splitting.splitsAroundClobbersEarly = true;
    policy.air.splitting.spansStretchesOfClobbers = true;
    policy.air.splitting.limitsClobberSitesPerSplit = true;
    policy.air.splitting.splitsIntoClustersOverBlocks = true;
    policy.air.splitting.splitsAroundFrame = true;

    policy.air.frame.ordersSlotsByUse = true;
    policy.air.frame.prefersStackPointerAddresses = true;

    policy.air.layout.movesBlocksWithoutSuccessorsToEnd = true;
    policy.air.layout.placesRareBlocksInGaps = true;
    policy.air.layout.ordersBlocksByEdgeFrequency = true;
    policy.air.layout.recordsRareCodeStart = true;
    policy.air.layout.mergesIdenticalBlocks = true;

    policy.air.peepholes.forwardsRegisterCopies = true;
    policy.air.peepholes.reusesConditionFlags = true;
    policy.air.peepholes.pairsMemoryAccesses = true;
    policy.air.peepholes.trapReach = 4096;
    return policy;
}

inline std::unique_ptr<Compilation> generateAndLink(Procedure& proc, Vector<uint8_t>* bytes = nullptr)
{
    CHECK(!proc.report().failure);
    CCallHelpers jit;
    generate(proc, jit);
    if (bytes) {
        AssemblerBuffer& buffer = jit.m_assembler.buffer();
        *bytes = Vector<uint8_t>(unsafeMakeSpan(static_cast<const uint8_t*>(buffer.readOnlyData()), buffer.codeSize()));
    }
    LinkBuffer linkBuffer(jit, nullptr);
    auto code = FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "Compilation");
    CHECK(!proc.report().failure);
    return makeUnique<Compilation>(WTF::move(code), proc.releaseByproducts());
}

inline std::unique_ptr<Compilation> compileAndCheckReport(Procedure& proc, unsigned optLevel)
{
    proc.setOptLevel(optLevel);
    if (g_dumpB3AfterGeneration)
        proc.setShouldDumpIR();
    prepareForGeneration(proc);
    return generateAndLink(proc);
}


#endif // ENABLE(B3_JIT)
