/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLiveness.h"
#include "AOTNaturalLoops.h"
#include "AOTRegionCloner.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class SSARepair;
struct FunctionPassContext;

// With requestedBlocks, returns only the points inside those blocks, and may omit points at block heads.
Vector<ProgramPoint> findRejoinPoints(FunctionAnalyses&, NOESCAPE const ScopedLambda<bool(ValueID)>& mayCallUnknownCode, NOESCAPE const ScopedLambda<bool(ProgramPoint)>& mayRejoinAt, const BitVector* requestedBlocks = nullptr);

Expected<bool> isClosedOnlyByWaysBack(FunctionAnalyses&, const NaturalLoop&);

Vector<TypeID, 4> typesOfCountersOnWayBack(FunctionAnalyses&, TypeArena&, BlockID head);

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

    void addCopiedBlock(BlockID block) { m_requiredBlocks.append(block); }
    void addStart(BlockID block) { m_starts.append(block); }
    void addEntry(BlockID block) { m_entries.append(block); }
    void addRejoinBlock(BlockID block) { m_isRejoinBlock.set(block.index()); }
    void addRejoinBlockWithoutTests(BlockID block) { m_isRejoinBlockWithoutTests.set(block.index()); }
    // The copy comes back here even where the tested code knows more about a live value. Behind the block that value has the type of both.
    void addRequiredRejoinBlock(BlockID block) { m_isRequiredRejoinBlock.set(block.index()); }
    void addValueToBeNarrowed(ValueID value) { m_valuesToBeNarrowed.append(value); }
    void addValueNarrowedInUnknownWay(ValueID value) { m_valuesNarrowedInUnknownWay.append(value); }
    void addValueKnownBeyondItsType(ValueID value) { m_valuesKnownBeyondTheirTypes.append(value); }
    void leaveAtRarerRejoinBlocks() { m_leavesAtRarerRejoinBlocks = true; }
    enum class Extent : uint8_t { NotCopied, OnlyCopiedBlocks, ToEndOfFunction, ToWaysBack };
    bool wasCutShortByLimit() const { return m_wasCutShortByLimit; }
    void testOnlyWaysBackIntoLoops() { m_testsOnlyWaysBackIntoLoops = true; }
    Extent plan(uint64_t maxBlocks);
    Extent plan(uint64_t maxBlocks, NOESCAPE const RegionCloner::LivenessSource& livenessInsideCopiedBlocks);
    std::span<const BlockID> plannedBlocks() const LIFETIME_BOUND { return m_region.span(); }

    void clone(Expected<BlockFrequency> (*frequencyOfCopy)(Expected<BlockFrequency>));
    BlockID copyOf(BlockID block) const { return m_cloner ? m_cloner->cloneOf(block) : BlockID(); }
    ValueID copyOf(ValueID value) const { return m_hasPlaceholder.get(value.index()) ? ValueID() : nameInCopy(value); }
    std::span<const BlockID> copiedBlocks() const { return m_cloner ? m_cloner->clonedBlocks() : std::span<const BlockID> { }; }

    enum class TypesTestedOnWayBack : uint8_t { Proven, Expected };
    void addWaysBack(std::span<const ValueID> changedValues, TypesTestedOnWayBack = TypesTestedOnWayBack::Proven);
    unsigned numberOfWaysBack() const { return m_numberOfWaysBack; }

    void addVariablesTo(SSARepair&);
    void addDefinition(ValueID inTypedCode, ValueID otherDefinition);

private:
    struct TestedRejoin {
        BlockID block;
        unsigned depthOfLoop { 0 };
        Vector<ValueID, 4> narrowedLiveValues;
        Vector<ValueID, 4> valuesThatMayBeUnspecified;
        Vector<TypeID, 4> typesOfCounters;
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
        Liveness::AnswersAboutValues aboutLiveValues;
    };

    bool isInRegion(BlockID block) const { return m_isInRegion.get(block.index()); }
    Expected<bool> isRare(BlockID block) const { return m_function.at(block).frequency == BlockFrequency::Rare; }

    void computeBlocksReachingNonRareCode();
    void computeNarrowedValues();
    bool hasValueAtHead(BlockID, const BitVector& values, AnswersByBlock&) const;
    void computeRegion(Extent);
    bool addsLoopEntry(BlockID) const;
    bool mergesAllocationWithItsCopyAt(BlockID) const;
    bool mergesAllocationThatSinksWithItsCopyAt(BlockID) const;
    bool mergesGeneratorWithItsCopyAt(BlockID) const;
    bool canLeaveCopyAt(BlockID) const;
    bool handsOverTooMuchAtExits() const;
    bool tryPlan(Extent, uint64_t maxBlocks, const RegionCloner::LivenessSource&);
    void recordTestedRejoins();

    ValueID nameInCopy(ValueID inTypedCode) const { return m_cloner ? m_cloner->cloneOf(inTypedCode) : ValueID(); }

    Vector<TypeID> computeTypesIgnoringCopy(std::span<const ValueID> changedValues);
    TypeID typeBoundInCopy(ValueID inTypedCode, ValueID inCopy, const RejoinTypes&) const;
    TypeID typeAssumedInTypedCode(ValueID, TypeID, const BitVector& isCopy);
    std::optional<RejoinPlan> planRejoin(const TestedRejoin&, const RejoinTypes&, const BitVector& isCopy);
    void keepWaysBackWithinSizeOfCopy(Vector<RejoinPlan, 4>&) const;
    std::optional<PreheaderPlan> planPreheader(BlockID header, bool isTestedRejoin, const BitVector& isCopy);

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
    BitVector m_isRequiredRejoinBlock;

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
