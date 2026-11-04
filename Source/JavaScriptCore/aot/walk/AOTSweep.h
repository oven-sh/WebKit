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

// One block that reads a value. `reader`: the last operation of the block that reads it. Null: the end of the block does. `previous`: of the same value, in the list of all.
struct ReadOfSweptValue {
    BlockID block;
    ValueID reader;
    uint32_t previous { UINT32_MAX };
};

struct SweepTotals {
    // Visits, not records: it goes up each time a loop of the sweep visits a record that Code::append() made, so an extra pass over Code shows up in it. Parameters and constants are not counted.
    uint32_t operationsSwept { 0 };
    uint32_t operationsDropped { 0 };
};

// DESIGN.md section 11. One backward pass over Code, which changes no record: it adds marks, the order of each block, use counts and what is live at suspensions.
// A thread has one and sweeps one unit after the other with it, in the same storage.
class Sweep {
    WTF_MAKE_NONCOPYABLE(Sweep);
public:
    Sweep() = default;

    // Behind it Code::isSwept().
    SweepTotals run(const SweepInputs&, Code&);

private:
    // AOTSweep.cpp
    void listIncomingEdges();
    // From the block along the edges of terminators. Each block is scanned for its roots when it is reached, once.
    void reach(BlockID);
    void scanRootsOf(BlockID);
    void need(ValueID);
    void needArgumentsFor(ValueID parameter);
    // The edge has just begun to be taken.
    void needArgumentsOn(EdgeID);
    void needArgumentsOn(const Edge&);
    // It leaves a reached block, and is no handler edge of a block in which nothing needed has been found to throw.
    bool isTaken(EdgeID) const;
    bool isRoot(const Operation&) const;
    void forgetEdgesThatAreNotTaken();
    void forgetHandlersThatAreGone();
    void sortFactUses();
    // One way backward through the reached blocks, each behind the blocks that its terminator leads forward to. Users stand behind what they use, so where the way comes by an operation it mostly knows whether the operation is needed,
    // and does there all that is to be done for it: marks, order, use counts, handlers, barriers, fences, and what liveness asks of it.
    // What is found needed behind the way (round a loop, by a handler block, a store whose object is read in front of it) goes through the worklist and is visited once more, alone.
    void sweepBackward();
    void visitOperationsOf(BlockID);
    // Once for each needed operation, where the way comes by it or when it leaves the worklist.
    void noteNeededOperation(ValueID, const Operation&);
    void settleWhatIsNeeded();
    bool mayBeKeptForItsOperand(const Operation&) const;
    // Whether the operand is needed is known when the way has come by the operand, which stands in front. Until then the operation stands in the order of its block. False: it never stays.
    bool noteOperationKeptForItsOperand(BlockID, ValueID, const Operation&);
    void settleOperationsKeptForTheirOperands();
    void putOperationsNeededLateInOrder();
    void needOperandsOfWhatWayDoesNotComeBy();
    void countUse(ValueID used, ValueID user);
    void countUsesAtEndsOfBlocks();

    // AOTSweepStores.cpp
    // A store into an object that nothing needs but its own stores is dropped with the object. Into a closure only a PropSet that isPlainStoreToClosure() is such a store, and no other store by name or by key is one: it may end in a setter.
    bool isStoreIntoOwnObject(const Operation&, ValueID& object);
    void holdBackUntilNeeded(ValueID store, ValueID object);
    void needStoresHeldBackFor(ValueID object);

    // AOTSweepHandlers.cpp
    // A block gets its handler edge back when something that stays in it may throw.
    void giveUpHandlerOf(BlockID);
    void keepHandlerOf(BlockID);
    void keepHandlerIfThrows(const Operation& needed);
    // The handler block takes what was thrown and throws it on out of the unit, and on the way there stands nothing that stays by itself. A block that throws to it needs no handler, so it keeps none, and the handler block is not reached.
    // It goes by the records alone, and looks at each handler block once.
    bool handlerOnlyThrowsOutOfUnit(BlockID handler);
    // A barrier that a later barrier covers. visitOperationsOf() calls these on its way backward through each block, every block behind the blocks that its terminator leads forward to.
    bool beginLookingForCoveredBarriers();
    void beginCoverageAtEndOf(BlockID);
    void lookForCoveredBarrier(ValueID, const Operation&);
    void endCoverageAtHeadOf(BlockID);
    void strikeCoveredOwner(ValueID);
    void strikeCoveredOwners();
    void dropBarriersThatNoStoreWaitsFor();
    // The fence of an allocation that the fence of a later allocation covers. Called like those above, on the same way through the blocks.
    bool beginLookingForCoveredFences();
    void beginFenceCoverageAtEndOf(BlockID);
    void lookForCoveredFence(ValueID, const Operation&);
    void endFenceCoverageAtHeadOf(BlockID);
    // Whether the operation stays changes what lookForCoveredBarrier() or lookForCoveredFence() finds for some other one, or for itself.
    bool mayChangeWhatIsCovered(const Operation&) const;
    // Over the order of each block, once that is final: something of which mayChangeWhatIsCovered() says yes was found needed behind the way, or was dropped behind it.
    void lookForCoverageAgain();

    // AOTSweepLiveness.cpp
    // Last uses for ReleaseHints and ReleaseEnvironments, and the live values at each needed operation that suspends.
    // What a hint behind a suspension names, and what flows into a join that only hints read, is followed once more, from the hint back to where it was last read: Code::releasesBehindLastReadsIn().
    // sweepBackward() notes on its way who reads what in which block, and what suspends.
    // Behind it findLiveSets() works out of those notes what is live behind each hint and each suspension, and visits no operation again.
    void beginFollowingLiveness();
    void noteNeededOperationForLiveness(ValueID, const Operation&);
    void noteReadForLiveness(ValueID read, BlockID, ValueID reader);
    void noteValueHeldByCopiedBody(ValueID);
    void followIfEnvironmentIsHandedBack(ValueID needed);
    void followIfHoldsEnvironmentInFrame(ValueID needed);
    void finishLiveness();
    // What finishLiveness() asks, answered block by block in place of value by value: one way through the blocks, each behind the blocks that its edges lead forward to, with the live set as a SweepValueSets::Set.
    // A block costs what it reads and defines, and a suspension what is live there. What is live at the head of a loop lives to the loop's end: no edge is followed back.
    // `releaseHints`, by value: the first hint that names the value, and by hint the next one that names the same value. `valuesToFollow`, by value. Null: every value whose reads are listed.
    void findLiveSets(std::span<const ValueID> releaseHints, const SweepBits* valuesToFollow);
    SweepValueSets::Set liveThroughLoopsAround(BlockID) const;
    void markReleasesBehindLastUsesBySets();
    bool markReleasesBehindLastUsesByValue(std::span<const ValueID> releaseHints);
    void listLiveValuesAtSuspensionsBySets();

    const SweepInputs* m_inputs { nullptr };
    Code* m_code { nullptr };
    SweepTotals m_totals;
    Vector<ValueID> m_worklist;
    // Handler blocks that something needed may throw to.
    Vector<BlockID> m_blocksToReach;
    // By block: handlerOnlyThrowsOutOfUnit() has looked, and what it found.
    SweepBits m_handlersLookedAt;
    SweepBits m_handlersThatOnlyThrowOutOfUnit;
    // By parameter: the edges that bring its arguments are found through the block's incoming edges.
    Vector<uint32_t> m_firstIncomingEdge;
    Vector<EdgeID> m_incomingEdges;
    bool m_hasListedIncomingEdges { false };
    bool m_hasGivenUpHandlers { false };
    SweepBits m_blocksThatHandOverArguments;
    // One search after the other, each in postorder. A later search begins at a handler block and ends where an earlier one has been.
    Vector<BlockID> m_reachedInPostorder;
    SweepBits m_heldBack;
    bool m_holdsBackStores { false };
    std::array<uint8_t, numberOfEnumerators<Opcode>> m_operandsWrittenByHeldBackStores { };
    // At an allocation, the last store that waits for it. At such a store, the store that waits for the same allocation in front of it.
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
    // In the order of the way, so those of one block stand in a row. A ReleaseHint that never stays is listed too.
    Vector<OperationKeptForOperand> m_operationsKeptForOperands;
    uint32_t m_numberOfOperationsInOrder { 0 };
    bool m_looksForCoveredBarriers { false };
    bool m_looksForCoveredFences { false };
    bool m_coverageIsOutOfDate { false };
    // Each needed value that has the type of an environment whose cells are handed back, or holds one without setting its byte, and each ReleaseEnvironment, in the order of the layouts.
    struct EnvironmentHandedBack {
        LayoutID layout;
        ValueID value;
    };
    Vector<EnvironmentHandedBack> m_environmentsHandedBack;
    // From beginFollowingLiveness() to finishLiveness().
    bool m_followsLiveness { false };
    bool m_mayHoldHints { false };
    bool m_mayHandBackEnvironments { false };
    bool m_looksForLastReads { false };
    bool m_hasInteriorPointers { false };
    bool m_holdsHintsBehindCopiedBodies { false };
    // By value: what may be a cell that a ReleaseEnvironment names.
    SweepBits m_environmentsFollowed;
    SweepBits m_blocksThatSuspend;
    SweepBits m_blocksWithHints;
    // By block: the last needed operation that may suspend.
    Vector<ValueID> m_lastSuspensions;
    SweepBits m_valuesLetGo;
    SweepBits m_valuesHeldByCopiedBodies;
    struct OtherNameOfValue {
        ValueID valueNamedByHint;
        ValueID name;
    };
    Vector<OtherNameOfValue> m_otherNames;
    // By value, in m_reads.
    Vector<uint32_t> m_lastReads;
    Vector<ReadOfSweptValue> m_reads;
    SweepLivenessStorage m_liveness;
    // By value: the last operation of the block that defines the value which reads it.
    Vector<ValueID> m_lastReadersInOwnBlocks;
    // By block: the owners that a barrier remembers on every way on from the head, before anything can exit sideways or collect. Nothing for a block that has not been visited.
    Vector<ValueID> m_coveredAtHeads;
    Vector<uint32_t> m_firstCoveredAtHead;
    Vector<uint32_t> m_numberCoveredAtHead;
    // Where the sweep stands: the owners that a later barrier remembers, as a list and by value.
    Vector<ValueID> m_coveredOwners;
    SweepBits m_ownersCovered;
    // Each RememberOwner that no later barrier covers, and the owner of each store that waits for a later barrier.
    Vector<ValueID> m_barriersBehindStores;
    Vector<ValueID> m_ownersOfCoveredStores;
    // Every way on from where the sweep stands comes to a fence before a cell is handed on to where the collector finds it. By block: the same of its head. Clear for a block that has not been visited.
    bool m_laterFenceCovers { false };
    bool m_laterFenceCoversAtSomeHead { false };
    SweepBits m_laterFenceCoversAtHeads;
};

// AOTSweep.cpp
// The sweep keeps the record whether or not anything uses it: it may throw or run code, it has no result, or it is one of those that are never dropped, a LoopHint among them.
// The walk asks it of what it appends while a loop is open: a loop that holds nothing of the kind but its polls only turns.
bool staysInSweptCodeByItself(const Program&, const Operation&);

// AOTSweepHandlers.cpp
// The record that has made the cell which `cell` names, behind the operations that hand on the cell they are given.
ValueID rootOfCellInSweptCode(const Code&, ValueID cell);
extern const OpcodeSet sweptOperationsWithOwnerBarriers;
extern const OpcodeSet sweptAllocationsWithFences;

// Whatever the allocation meets at run time, its code fences behind the stores that fill a new cell of the heap, and hands no cell on in front of that fence.
// Each is answered beside the emitter that it speaks of.
// AOTAllocationEmitter.cpp
bool newObjectFencesBeforeHandingCellOn(const Program&, const Plan&, const Operation& newObject);
// AOTElementEmitterAllocation.cpp
bool newArrayFencesBeforeHandingCellOn(const Operation& newArray);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
