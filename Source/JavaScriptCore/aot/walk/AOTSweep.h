/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTSweepLiveness.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class Plan;
class Program;

struct SweepInputs {
    const Program& program;
    const Plan& plan;
    TypeArena& types;
};

struct ReadOfSweptValue {
    BlockID block;
    ValueID reader;
    uint32_t previous { UINT32_MAX };
};

struct SweepTotals {
    uint32_t operationsSwept { 0 };
    uint32_t operationsDropped { 0 };
};

inline constexpr uint8_t neededMarkOfSweep = static_cast<uint8_t>(SweepMark::IsNeeded);
inline constexpr uint8_t passedByMarkOfSweep = 1 << 6;

class Sweep {
    WTF_MAKE_NONCOPYABLE(Sweep);
public:
    Sweep() = default;

    SweepTotals run(const SweepInputs&, Code&);

private:
    struct ArraysByValue {
        uint8_t* marks;
        uint8_t* useCounts;
        ValueID* onlyUsers;
    };

    void listIncomingEdges();
    void listEdgesThatAreTaken();
    void ensureEdgesThatAreTakenAreListed();
    bool edgesAscendInto(BlockID, const Block&);
    void reach(BlockID);
    void scanRootsOf(const ArraysByValue&, BlockID, const Block&);
    void need(ValueID);
    void needArgumentsFor(ValueID parameter, const Operation&);
    void needArgumentsOn(EdgeID);
    void needArgumentsOn(const Edge&);
    bool isTaken(EdgeID, const Edge&) const;
    bool isRoot(const Operation&) const;
    void forgetEdgesThatAreNotTaken();
    void forgetHandlersThatAreGone();
    void sortFactUses();
    void sweepBackward();
    void visitOperationsOf(BlockID);
    void noteNeededOperation(const ArraysByValue&, const uint8_t& stateOfBlock, ValueID, const Operation&);
    void settleWhatIsNeeded();
    bool mayBeKeptForItsOperand(const Operation&) const;
    bool noteOperationKeptForItsOperand(BlockID, ValueID, const Operation&);
    void settleOperationsKeptForTheirOperands();
    void putOperationsNeededLateInOrder();
    void needOperandsOfWhatWayDoesNotComeBy();
    void countUse(ValueID used, ValueID user);
    void countUsesAtEndsOfBlocks();

    bool isStoreIntoOwnObject(const Operation&, ValueID& object);
    void holdBackUntilNeeded(ValueID store, ValueID object);
    void needStoresHeldBackFor(ValueID object);

    void giveUpHandlerOf(BlockID, const Block&);
    void keepHandlerOf(BlockID);
    void keepHandlerIfThrows(const Operation& needed);
    bool handlerOnlyThrowsOutOfUnit(BlockID handler);
    bool beginLookingForCoveredBarriers();
    void beginCoverageAtEndOf(BlockID);
    void lookForCoveredBarrier(ValueID, const Operation&);
    void endCoverageAtHeadOf(BlockID);
    void strikeCoveredOwner(ValueID);
    void strikeCoveredOwners();
    void lookForBirthOfOwner(ValueID, const Operation&);
    void forgetStoresThatAwaitBirth();
    bool beginLookingForCoveredFences();
    void beginFenceCoverageAtEndOf(BlockID);
    void lookForCoveredFence(ValueID, const Operation&);
    void endFenceCoverageAtHeadOf(BlockID);
    bool mayChangeWhatIsCovered(const Operation&) const;
    void lookForCoverageAgain();

    void beginFollowingLiveness();
    void noteNeededOperationForLiveness(ValueID, const Operation&);
    void noteReadForLiveness(ValueID read, BlockID, ValueID reader);
    void noteValueHeldByCopiedBody(ValueID);
    void followIfEnvironmentIsHandedBack(ValueID needed);
    void followIfHoldsEnvironmentInFrame(ValueID needed);
    void finishLiveness();
    void findLiveSets(std::span<const ValueID> releaseHints, const SweepBits* valuesToFollow);
    SweepValueSets::Set liveThroughLoopsAround(BlockID) const;
    void markReleasesBehindLastUsesBySets();
    bool markReleasesBehindLastUsesByValue(std::span<const ValueID> releaseHints);
    void listLiveValuesAtSuspensionsBySets();

    const SweepInputs* m_inputs { nullptr };
    Code* m_code { nullptr };
    SweepTotals m_totals;
    ArraysByValue m_arrays { };
    Vector<uint8_t> m_statesOfBlocks;
    uint8_t* m_states { nullptr };
    Vector<ValueID> m_worklist;
    Vector<BlockID> m_blocksToReach;
    Vector<uint32_t> m_firstIncomingEdge;
    Vector<EdgeID> m_incomingEdges;
    bool m_hasListedIncomingEdges { false };
    bool m_hasForgottenEdgesThatAreNotTaken { false };
    bool m_someBlockHandsOverArguments { false };
    BlockID m_blockThatEdgesAscendInto;
    struct BlockOnSearchPath {
        const Block* block;
        uint32_t index;
        uint32_t nextEdge;
    };
    Vector<BlockOnSearchPath> m_searchPath;
    Vector<BlockID> m_reachedInPostorder;
    SweepBits m_heldBack;
    bool m_holdsBackStores { false };
    std::array<uint8_t, numberOfEnumerators<Opcode>> m_operandsWrittenByHeldBackStores { };
    std::array<uint8_t, numberOfEnumerators<Opcode>> m_opcodesOfAllocationsWhoseStoresAreHeldBack { };
    Vector<ValueID> m_waitingStores;

    struct OperationInBlock {
        BlockID block;
        ValueID operation;
    };
    Vector<OperationInBlock> m_operationsNeededLate;
    struct OperationKeptForOperand {
        BlockID block;
        ValueID operation;
        ValueID operand;
        Opcode opcode { Opcode::Undefined };
        bool standsInOrder { false };
        bool isBehindCopiedBody { false };
    };
    Vector<OperationKeptForOperand> m_operationsKeptForOperands;
    uint32_t m_numberOfOperationsInOrder { 0 };
    uint32_t m_firstInOrderSoFar { 0 };
    bool m_looksForCoveredBarriers { false };
    bool m_looksForCoveredFences { false };
    bool m_coverageIsOutOfDate { false };
    struct EnvironmentHandedBack {
        LayoutID layout;
        ValueID value;
    };
    Vector<EnvironmentHandedBack> m_environmentsHandedBack;
    bool m_followsLiveness { false };
    bool m_mayHoldHints { false };
    bool m_mayHandBackEnvironments { false };
    bool m_looksForLastReads { false };
    bool m_hasInteriorPointers { false };
    bool m_holdsHintsBehindCopiedBodies { false };
    SweepBits m_environmentsFollowed;
    SweepBits m_blocksThatSuspend;
    SweepBits m_blocksWithHints;
    Vector<ValueID> m_lastSuspensions;
    SweepBits m_valuesLetGo;
    SweepBits m_valuesHeldByCopiedBodies;
    struct OtherNameOfValue {
        ValueID valueNamedByHint;
        ValueID name;
    };
    Vector<OtherNameOfValue> m_otherNames;
    Vector<uint32_t> m_lastReads;
    Vector<ReadOfSweptValue> m_reads;
    SweepLivenessStorage m_liveness;
    Vector<ValueID> m_lastReadersInOwnBlocks;
    Vector<ValueID> m_coveredAtHeads;
    Vector<uint32_t> m_firstCoveredAtHead;
    Vector<uint32_t> m_numberCoveredAtHead;
    Vector<ValueID> m_coveredOwners;
    SweepBits m_ownersCovered;
    ValueID m_storeWhoseOwnerIsKnown;
    ValueID m_ownerOfThatStore;
    struct StoreThatAwaitsBirth {
        ValueID store;
        ValueID owner;
    };
    Vector<StoreThatAwaitsBirth, 8> m_storesThatAwaitBirth;
    SweepBits m_birthsAwaited;
    bool m_laterFenceCovers { false };
    bool m_laterFenceCoversAtSomeHead { false };
    SweepBits m_laterFenceCoversAtHeads;
};

bool staysInSweptCodeByItself(const Program&, const Operation&);

inline ValueID rootOfCellInSweptCode(const Code& code, ValueID cell)
{
    for (;;) {
        const Operation& operation = code.at(cell);
        if (!opcodesThatGiveSameCell.contains(operation.opcode))
            return cell;
        ASSERT(operation.operandUses(0).contains(OperandUse::Returns));
        cell = operation.operand(0);
    }
}

inline constexpr OpcodeSet sweptOperationsWithOwnerBarriers = opcodesWhere([](Opcode opcode) {
    return runsOwnerBarrierOf(opcode) && remembersOwnerOf(opcode);
});
inline constexpr OpcodeSet sweptAllocationsWithFences { Opcode::NewObject, Opcode::NewClosure, Opcode::NewArray, Opcode::CreateRest };

bool newObjectFencesBeforeHandingCellOn(const Program&, const Plan&, const Operation& newObject);
bool newArrayFencesBeforeHandingCellOn(const Operation& newArray);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
