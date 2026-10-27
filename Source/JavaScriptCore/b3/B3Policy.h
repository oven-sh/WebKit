/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirPolicy.h"
#include "B3HeapRange.h"
#include "B3Limits.h"
#include "B3Origin.h"
#include "GPRInfo.h"
#include "CPUFeatureSet.h"
#include "Reg.h"
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>
#include <wtf/SharedTask.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class CCallHelpers;

namespace B3 {

class MemoryValue;
struct MovedLoad;
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
    bool logsMovedLoads { false };
    bool keepsOriginsOfAccessesThatTakeTraps { false };
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
    bool mergesBlocksAfterForwardingJumps { false };
    // A Phi that is made for a demoted value has the origin of that value, for a client whose phases tell values apart by their origins.
    bool keepsOriginsOfDemotedValues { false };
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
    // The fast version of a loop whose counter stays below the length of an array, a view or a string does not poll.
    bool omitsPollsFromLoopsThatEndWithinData { false };
    bool unrolls { false };
    double minFrequencyOfUnrolledLoop { 1 };
    bool hoistsOnlyFromLikelyPaths { false };
    bool hoistsImmutableLoadsPastWrites { false };
};

struct SwitchLoweringPolicy {
    bool usesBitTests { false };
    bool usesValueTables { false };
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
    UnrollLoops,
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
    OmitFrameRecord,
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

static constexpr unsigned numberOfSwitches = 41;
static_assert(static_cast<unsigned>(PolicySwitch::FoldTrapsIntoAccesses) + 1 == numberOfSwitches);
static_assert(numberOfSwitches <= 64);

constexpr ASCIILiteral nameOf(PolicySwitch policySwitch) { return enumName(policySwitch); }

// Only the ahead-of-time compiler sets these. Without it they are constants, so the phases that hang on them are not in the binary.
#if ENABLE(AOT)
#define B3_POLICY_OF_CLIENT(Type, name) Type name
#else
#define B3_POLICY_OF_CLIENT(Type, name) static constexpr Type name { }
#endif

struct Policy {
    B3_POLICY_OF_CLIENT(AddressingPolicy, addressing);
    B3_POLICY_OF_CLIENT(AllocationPolicy, allocation);
    UpstreamPolicy upstream;
    B3_POLICY_OF_CLIENT(OptimizationPolicy, optimization);
    B3_POLICY_OF_CLIENT(SelectionPolicy, selection);
    B3_POLICY_OF_CLIENT(LoopPolicy, loops);
    B3_POLICY_OF_CLIENT(SwitchLoweringPolicy, switchLowering);
    B3_POLICY_OF_CLIENT(KnownPropertyPolicy, knownProperties);
    B3_POLICY_OF_CLIENT(ValidationPolicy, validation);
    LimitValues limits;
    CPUFeatureSet targetFeatures { CPUFeatureSet::host() };
    Air::Policy air;
    bool collectsRemarks { false };
    Vector<ClientPhase, 4> clientPhases;
    // False: the load is not logged. The collector's threads write some words, and some writes change no operation's result.
    RefPtr<SharedTask<bool(const MemoryValue&, MovedLoad&)>> saysWhatCountsOfMovedLoad;

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
