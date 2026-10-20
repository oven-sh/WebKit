/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirPolicy.h"
#include "B3Limits.h"
#include "CPUFeatureSet.h"
#include "Reg.h"
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>
#include <wtf/SharedTask.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace B3 {

class Procedure;
class RelocationLowering;

enum class ValidationLevel : uint8_t {
    FromOptions,
    AtBoundaries,
    AtEachPhase,
};

struct ValidationPolicy {
    ValidationLevel level { ValidationLevel::FromOptions };
    bool checksKnownPropertiesAtRunTime { false };
};

enum class RedundancyElimination : uint8_t {
    CommonSubexpressions,
    MemoryNumbering,
};

struct PinnedConstant {
    Reg reg;
    std::optional<int32_t> offset;
    bool replacesImmediates { false };
};

using PinnedConstantFinder = std::optional<PinnedConstant> (*)(uint64_t bits);

struct AddressingPolicy {
    bool isPositionIndependent { false };
    bool namesPinnedRegistersDirectly { false };
    uint32_t dataSectionSpace { 0 };
    uint32_t jumpTableSpace { 0 };
    RelocationLowering* relocationLowering { nullptr };
    PinnedConstantFinder findPinnedConstant { nullptr };
};

struct AllocationPolicy {
    bool usesArena { false };
    bool keepsValuesAfterLowering { false };
};

struct UpstreamPolicy {
    unsigned optLevel { 2 };
    unsigned maxTailDuplicationBlockSize { 3 };
    unsigned maxTailDuplicationBlockSuccessors { 3 };
    unsigned maxWasmGCEpochSnapshotEntries { 1024 * 1024 };
    bool specializesSelects { true };
    bool duplicatesTails { true };
    bool canonicalizesIncrements { false };
    bool hoistsLoopInvariantValues { true };
    bool eliminatesWasmGCAllocations { true };
    bool usesWasmFaultSignalHandler { true };
    bool allowsSIMD { false };
    bool forcesSIMD { false };
};

struct OptimizationPolicy {
    RedundancyElimination redundancyElimination { RedundancyElimination::CommonSubexpressions };
    bool splitsBranchConditions { false };
    bool estimatesFrequencies { false };
    bool returnsAreUnlikely { false };
    bool checksVariablesAreSet { false };
    bool threadsJumps { false };
    bool foldsImpliedComparisons { false };
    bool reducesWithRanges { false };
    bool narrowsIntegers { false };
    bool chainsAssociativeOperations { false };
    bool stopsSwitchInferenceAtRareEdges { false };
    bool convertsBranchesToSelects { false };
    bool mergesAdjacentConstantStores { false };
    bool sinksValues { false };
    bool placesConstantsByFrequency { false };
};

struct SelectionPolicy {
    bool extendsInstructionSelection { false };
    bool attributesTrapsToAccesses { false };
};

struct LoopPolicy {
    bool rotates { false };
    bool foldsComparisonsWithBounds { false };
    bool versions { false };
    bool clampsLoopBounds { false };
    bool stripMinesPolls { false };
    bool hoistsOnlyFromLikelyPaths { false };
};

struct SwitchLoweringPolicy {
    bool usesBitTests { false };
    unsigned minCasesForJumpTable { 7 };
};

struct KnownPropertyPolicy {
    bool trustsKnownRanges { false };
    bool trustsKnownDereferenceableBytes { false };
    bool trustsKnownInductions { false };
};

struct ClientPhase {
    enum class Point : uint8_t {
        BeforeOptimization,
        AfterOptimization,
        // No later phase moves a value over another.
        AfterSinking,
        BeforeLowerToAir,
    };

    Point point;
    ASCIILiteral name;
    RefPtr<SharedTask<bool(Procedure&)>> run;
};

enum class PolicySwitch : uint8_t {
    MemoryNumbering,
    SplitBranchConditions,
    EstimateFrequencies,
    ThreadJumps,
    FoldImpliedComparisons,
    ReduceWithRanges,
    NarrowIntegers,
    ChainAssociativeOperations,
    ConvertBranchesToSelects,
    MergeAdjacentConstantStores,
    SinkValues,
    PlaceConstantsByFrequency,
    ExtendInstructionSelection,
    PinnedConstants,
    HoistLoopInvariantValues,
    RotateLoops,
    FoldComparisonsWithBounds,
    VersionLoops,
    ClampLoopBounds,
    StripMinePolls,
    BitTests,
    KnownRanges,
    KnownDereferenceableBytes,
    KnownInductions,
    ChargeForCalleeSaves,
    PreferredRegisters,
    SplitLiveRanges,
    SplitLiveRangesAroundFrame,
    ShrinkWrapFrame,
    FrameLayout,
    MoveBlocksWithoutSuccessorsToEnd,
    RareBlockLayout,
    MergeIdenticalBlocks,
    DuplicateReturns,
    ForwardRegisterCopies,
    HoistCommonMoves,
    ReuseConditionFlags,
    PairMemoryAccesses,
    ResequenceMovesBeforePatches,
    FoldTrapsIntoAccesses,
};

static constexpr unsigned numberOfSwitches = 40;
static_assert(static_cast<unsigned>(PolicySwitch::FoldTrapsIntoAccesses) + 1 == numberOfSwitches);
static_assert(numberOfSwitches <= 64);

constexpr ASCIILiteral nameOf(PolicySwitch policySwitch) { return enumName(policySwitch); }

struct Policy {
    AddressingPolicy addressing;
    AllocationPolicy allocation;
    UpstreamPolicy upstream;
    OptimizationPolicy optimization;
    SelectionPolicy selection;
    LoopPolicy loops;
    SwitchLoweringPolicy switchLowering;
    KnownPropertyPolicy knownProperties;
    ValidationPolicy validation;
    LimitValues limits;
    CPUFeatureSet targetFeatures { CPUFeatureSet::host() };
    Air::Policy air;
    bool collectsRemarks { false };
    Vector<ClientPhase, 4> clientPhases;

    JS_EXPORT_PRIVATE static Policy fromProcessOptions();

    JS_EXPORT_PRIVATE void disable(PolicySwitch);
    bool isDisabled(PolicySwitch policySwitch) const { return m_disabledSwitches & maskOf(policySwitch); }
    uint64_t disabledSwitches() const { return m_disabledSwitches; }
    JS_EXPORT_PRIVATE void applyDisabledSwitches();

    JS_EXPORT_PRIVATE bool supportsFloatingPointRounding() const;
    JS_EXPORT_PRIVATE bool supportsRoundingFloatToIntegerFloat() const;
    JS_EXPORT_PRIVATE bool supportsAtomicMemoryOperations() const;
    JS_EXPORT_PRIVATE bool supportsSHA3() const;

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    static constexpr uint64_t maskOf(PolicySwitch policySwitch) { return 1ull << static_cast<unsigned>(policySwitch); }

    uint64_t m_disabledSwitches { 0 };
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
