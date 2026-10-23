/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNaturalLoops.h"
#include "AOTRegionCloner.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class SSARepair;
struct FunctionPassContext;

// Loop headers, handler blocks, and the points after an operation that may call unknown code and the reads of its results.
Vector<ProgramPoint> findRejoinPoints(FunctionAnalyses&, NOESCAPE const ScopedLambda<bool(ValueID)>& mayCallUnknownCode, NOESCAPE const ScopedLambda<bool(ProgramPoint)>& mayRejoinAt);

// A copy that comes back in front of the tests it has failed closes a cycle through them. The tested code never goes around it, so it is no loop to move a test out of.
bool isClosedOnlyByWaysBack(FunctionAnalyses&, const NaturalLoop&);

struct ValuesNarrowedByTests {
    std::span<const ValueID> asExpected;
    std::span<const ValueID> inUnknownWay;
    std::span<const ValueID> knownBeyondTheirTypes;
};

BitVector findValuesTypedCodeKnowsMoreAbout(FunctionAnalyses&, const TypeArena&, const ValuesNarrowedByTests&);

class GenericCopy {
    WTF_MAKE_NONCOPYABLE(GenericCopy);
public:
    GenericCopy(FunctionPassContext&, IRBuilder&);

    // Step 1, once the caller has split its blocks.
    void addCopiedBlock(BlockID);
    void addStart(BlockID);
    void addEntry(BlockID);
    void addRejoinBlock(BlockID);
    void addRejoinBlockWithoutTests(BlockID);
    void addValueToBeNarrowed(ValueID);
    void addValueNarrowedInUnknownWay(ValueID);
    void addValueKnownBeyondItsType(ValueID);
    // For a caller that narrows its values after clone(): a rejoin block that is rarer than the starts is typed as the copy is, so the copy leaves for it whatever is live there and does not copy it once more.
    void leaveAtRarerRejoinBlocks() { m_leavesAtRarerRejoinBlocks = true; }
    enum class Extent : uint8_t { NotCopied, OnlyCopiedBlocks, ToEndOfFunction, ToWaysBack };
    bool wasCutShortByLimit() const { return m_wasCutShortByLimit; }
    // Outside loops the copy then comes back only where that takes no test.
    void testOnlyWaysBackIntoLoops() { m_testsOnlyWaysBackIntoLoops = true; }
    Extent plan(uint64_t maxBlocks);
    // A copy without starts asks the given liveness, which has to track the values that the copied blocks define, at the heads of the entries.
    Extent plan(uint64_t maxBlocks, NOESCAPE const RegionCloner::LivenessSource& livenessInsideCopiedBlocks);
    std::span<const BlockID> plannedBlocks() const LIFETIME_BOUND { return m_region.span(); }

    // Step 2.
    void clone(BlockFrequency (*frequencyOfCopy)(BlockFrequency));
    BlockID copyOf(BlockID) const;
    ValueID copyOf(ValueID) const;
    std::span<const BlockID> copiedBlocks() const;

    // Step 3, once the caller has emitted its tests and rewritten the typed code, before SSA is repaired.
    enum class TypesTestedOnWayBack : uint8_t { Proven, Expected };
    void addWaysBack(std::span<const ValueID> changedValues, TypesTestedOnWayBack = TypesTestedOnWayBack::Proven);
    unsigned numberOfWaysBack() const { return m_numberOfWaysBack; }

    // Step 4. addDefinition() may be called any time after clone(), with a value made after clone().
    void addVariablesTo(SSARepair&);
    void addDefinition(ValueID inTypedCode, ValueID otherDefinition);

private:
    struct TestedRejoin {
        BlockID block;
        unsigned depthOfLoop { 0 };
        Vector<ValueID, 4> narrowedLiveValues;
        Vector<ValueID, 4> valuesThatMayBeUnspecified;
    };

    struct RejoinTest {
        std::optional<uint32_t> parameterIndex;
        ValueID value;
        TypeID testedType;
    };

    struct RejoinPlan {
        BlockID block;
        unsigned depthOfLoop { 0 };
        Vector<Edge, 2> edges;
        Vector<RejoinTest, 2> tests;
        Vector<TypeID, 4> carriedTypes;
        std::optional<size_t> preheaderIndex;
        BlockID testBlock;
        Arguments carriedValues;
    };

    struct PreheaderPlan {
        BlockID header;
        Vector<Edge, 4> edges;
        BlockID preheader;
        Arguments parameters;
    };

    struct RejoinTypes {
        Vector<TypeID> ignoringCopy;
        Vector<TypeID> ofOtherDefinitions;
    };

    struct OtherDefinition {
        ValueID inTypedCode;
        ValueID definition;
    };

    struct AnswersByBlock {
        BitVector isKnown;
        BitVector answer;
    };

    bool isInRegion(BlockID block) const { return m_isInRegion.get(block.index()); }
    bool isRare(BlockID) const;

    void computeBlocksReachingNonRareCode();
    void computeNarrowedValues();
    bool hasValueAtHead(BlockID, const BitVector& values, AnswersByBlock&) const;
    bool hasNarrowedValueAtHead(BlockID) const;
    bool knowsMoreInTypedCodeAtHead(BlockID) const;
    void computeRegion(Extent);
    bool addsLoopEntry(BlockID) const;
    bool mergesAllocationWithItsCopyAt(BlockID) const;
    bool mergesAllocationThatSinksWithItsCopyAt(BlockID) const;
    bool canLeaveCopyAt(BlockID) const;
    bool forceCopyOfUnusableExits();
    bool tryPlan(Extent, uint64_t maxBlocks, const RegionCloner::LivenessSource&);
    void recordTestedRejoins();

    void addPlaceholders();
    ValueID nameInCopy(ValueID inTypedCode) const;

    Vector<TypeID> computeTypesIgnoringCopy(std::span<const ValueID> changedValues);
    Vector<TypeID> collectTypesOfOtherDefinitions() const;
    TypeID typeBoundInCopy(ValueID inTypedCode, ValueID inCopy, const RejoinTypes&) const;
    TypeID typeAssumedInTypedCode(ValueID, TypeID, const BitVector& isCopy);
    std::optional<RejoinPlan> planRejoin(const TestedRejoin&, const RejoinTypes&, const BitVector& isCopy);
    unsigned depthOfLoopEnteredAt(BlockID) const;
    void keepWaysBackWithinSizeOfCopy(Vector<RejoinPlan, 4>&) const;
    std::optional<PreheaderPlan> planPreheader(BlockID header, bool isTestedRejoin, const BitVector& isCopy);
    void addRejoinBlocks(Vector<RejoinPlan, 4>&, Vector<PreheaderPlan, 4>&, const RejoinTypes&);
    void emitRejoin(const RejoinPlan&, BlockID target);
    void emitPreheader(const PreheaderPlan&);

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder& m_builder;
    std::optional<RegionCloner> m_cloner;

    Vector<BlockID, 8> m_requiredBlocks;
    Vector<BlockID, 8> m_starts;
    Vector<BlockID, 8> m_entries;
    Vector<ValueID, 8> m_valuesToBeNarrowed;
    Vector<ValueID, 8> m_valuesNarrowedInUnknownWay;
    Vector<ValueID, 8> m_valuesKnownBeyondTheirTypes;
    BitVector m_isRejoinBlock;
    BitVector m_isRejoinBlockWithoutTests;

    BitVector m_reachesNonRareCode;
    BitVector m_mayBeNarrowed;
    BitVector m_isKnownBetterInTypedCode;
    mutable AnswersByBlock m_hasNarrowedValueAtHead;
    mutable AnswersByBlock m_knowsMoreInTypedCodeAtHead;
    BitVector m_mustBeCopied;
    BitVector m_keepsAllocationsApart;

    Vector<BlockID> m_region;
    BitVector m_isInRegion;
    uint64_t m_numberOfBlocksFromStarts { 0 };
    bool m_wasCutShortByLimit { false };
    bool m_testsOnlyWaysBackIntoLoops { false };
    Vector<BlockID, 8> m_exits;
    Vector<BlockID, 4> m_untestedRejoins;
    Vector<BlockID, 4> m_testedRejoinBlocks;
    mutable std::optional<BitVector> m_allocationsThatSink;
    Vector<TestedRejoin, 4> m_testedRejoins;

    BitVector m_hasPlaceholder;
    Vector<OtherDefinition, 8> m_otherDefinitions;
    size_t m_numberOfValuesAfterCloning { 0 };
    unsigned m_numberOfWaysBack { 0 };
    TypesTestedOnWayBack m_typesTestedOnWayBack { TypesTestedOnWayBack::Proven };
    bool m_leavesAtRarerRejoinBlocks { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
