/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSort.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesThatWriteIntoOperandOfHeldBackStore {
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::ChangesIntegrity,
};

static constexpr OperandUses usesThatReachPastOperandsOfHeldBackStore {
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

static constexpr OperandUses usesThatWriteNoOperandOfHeldBackStore {
    OperandUse::Inspects,
    OperandUse::Returns,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
};

static_assert(everyOperandUseIsInExactlyOneOf({ usesThatWriteIntoOperandOfHeldBackStore, usesThatReachPastOperandsOfHeldBackStore, OperandUse::Retains, usesThatWriteNoOperandOfHeldBackStore }));

static constexpr OpcodeSet heldBackOpcodesThatWriteWordOfTheirCell { Opcode::FinishBirth, Opcode::TryFinishBirth };
static_assert(OperandIndex::FinishBirth::object == OperandIndex::TryFinishBirth::object);

static constexpr bool writesReceiverOfHeldBackStore(Access access)
{
    switch (access) {
    case Access::Store:
    case Access::Define:
        return true;
    case Access::None:
    case Access::Read:
    case Access::Has:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static std::optional<uint32_t> operandWrittenByHeldBackStore(Opcode opcode)
{
    if (heldBackOpcodesThatWriteWordOfTheirCell.contains(opcode))
        return OperandIndex::FinishBirth::object;

    if (resultRuleOf(opcode) == ResultRule::Allocation || opcode == Opcode::NewClosure)
        return std::nullopt;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    uint32_t numberOfDeclaredOperands = numberOfNamedOperands + hasVariadicOperand(opcode);
    std::optional<uint32_t> written;
    bool writesSeveralOperands = false;
    auto noteWrittenOperand = [&](uint32_t operandIndex) {
        writesSeveralOperands |= written && *written != operandIndex;
        written = operandIndex;
    };

    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.containsAny(usesThatReachPastOperandsOfHeldBackStore) || !info.protocols.isEmpty())
            return std::nullopt;
        if (info.uses.containsAny(usesThatWriteIntoOperandOfHeldBackStore))
            noteWrittenOperand(operandIndex);
        if (!info.uses.contains(OperandUse::Retains))
            continue;
        switch (info.retainedIn) {
        case RetainedIn::UnknownMemory:
        case RetainedIn::Result:
            return std::nullopt;
        case RetainedIn::Operand:
            if (!info.retainingOperand)
                return std::nullopt;
            noteWrittenOperand(*info.retainingOperand);
            break;
        }
    }
    if (std::optional<uint8_t> receiver = receiverOf(opcode); receiver && writesReceiverOfHeldBackStore(accessOf(opcode)))
        noteWrittenOperand(*receiver);

    if (!written || writesSeveralOperands || *written >= numberOfNamedOperands)
        return std::nullopt;
    return written;
}

static bool isCallOfBuiltinWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& call)
{
    const BuiltinOverload& overload = builtins.at(call.overload());
    bool answersAsRowSays = call.opcode == Opcode::CallBuiltin ? call.builtinCallForm() == BuiltinCallForm::Whole : overload.code.implementation.inlineBuiltin == call.inlineBuiltin();
    if (!answersAsRowSays || call.isDoneInPlace() || !builtins.contains(overload.result.type))
        return false;
    return builtins.at(overload.result.type).freshness == BuiltinFreshness::Yes;
}

static bool isOpcodeOfAllocationWhoseStoresAreHeldBack(Opcode opcode)
{
    if (opcode == Opcode::NewClosure || effectRuleOf(opcode) != EffectRule::Declared || boundToOf(opcode) == BoundTo::Site)
        return false;

    bool keepsOperandInResult = false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (info.uses.contains(OperandUse::Returns))
            return false;
        keepsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
    }
    return resultRuleOf(opcode) == ResultRule::Allocation || keepsOperandInResult || opcode == Opcode::Data;
}

static constexpr uint8_t opcodeOfHeldBackAllocationIsNotLookedUp = 0;
static constexpr uint8_t isNoOpcodeOfHeldBackAllocation = 1;
static constexpr uint8_t isOpcodeOfHeldBackAllocation = 2;

static bool isAllocationWhoseStoresAreHeldBack(const BuiltinTable& builtins, const Operation& made, std::array<uint8_t, numberOfEnumerators<Opcode>>& answersOfSchema)
{
    EffectTraits traits = made.traits();
    if (traits.mayWriteObservableState() || traits.mayRunOtherCode())
        return false;

    Opcode opcode = made.opcode;
    if (opcode == Opcode::CallBuiltin || opcode == Opcode::CallInline)
        return isCallOfBuiltinWhoseStoresAreHeldBack(builtins, made);
    uint8_t& answerOfSchema = answersOfSchema[static_cast<unsigned>(opcode)];
    if (answerOfSchema == opcodeOfHeldBackAllocationIsNotLookedUp)
        answerOfSchema = isOpcodeOfAllocationWhoseStoresAreHeldBack(opcode) ? isOpcodeOfHeldBackAllocation : isNoOpcodeOfHeldBackAllocation;
    return answerOfSchema != isNoOpcodeOfHeldBackAllocation;
}

static constexpr uint8_t operandWrittenByHeldBackStoreIsNotLookedUp = 0;
static constexpr uint8_t noOperandIsWrittenByHeldBackStore = 1;
static constexpr uint8_t firstOperandWrittenByHeldBackStore = 2;

bool Sweep::isStoreIntoOwnObject(const Operation& operation, ValueID& object)
{
    if (effectRuleOf(operation.opcode) == EffectRule::Access && accessOf(operation.opcode) == Access::Store)
        return false;

    EffectTraits traits = operation.traits();
    if (traits.mayThrowByRule() || traits.mayBeTerminated() || traits.mayRunOtherCode() || traits.mayWriteOutsideObjects())
        return false;

    uint8_t& written = m_operandsWrittenByHeldBackStores[static_cast<unsigned>(operation.opcode)];
    if (written == operandWrittenByHeldBackStoreIsNotLookedUp) {
        std::optional<uint32_t> found = operandWrittenByHeldBackStore(operation.opcode);
        written = found ? static_cast<uint8_t>(*found + firstOperandWrittenByHeldBackStore) : noOperandIsWrittenByHeldBackStore;
    }
    if (written == noOperandIsWrittenByHeldBackStore)
        return false;
    ValueID name = operation.child(written - firstOperandWrittenByHeldBackStore);
    if (!name)
        return false;

    ValueID allocation = underlyingCellOf(*m_code, name);
    const Operation& made = m_code->at(allocation);
    if (made.blockIndex == Operation::noBlock)
        return false;
    if (!isAllocationWhoseStoresAreHeldBack(m_inputs->program.builtins(), made, m_opcodesOfAllocationsWhoseStoresAreHeldBack))
        return false;

    object = allocation;
    return true;
}

void Sweep::holdBackUntilNeeded(ValueID store, ValueID object)
{
    Code& code = *m_code;
    if (code.isNeeded(object)) {
        need(store);
        return;
    }

    if (!m_holdsBackStores) {
        m_holdsBackStores = true;
        m_heldBack.clearAll();
        m_heldBack.ensureSize(code.numberOfValues());
        m_waitingStores.fill(ValueID(), code.numberOfValues());
    }
    ASSERT(!m_heldBack.quickGet(object.index()));
    m_heldBack.quickSet(store.index());
    m_waitingStores[store.index()] = m_waitingStores[object.index()];
    m_waitingStores[object.index()] = store;
}

void Sweep::needStoresHeldBackFor(ValueID object)
{
    if (!m_holdsBackStores || m_heldBack.quickGet(object.index()))
        return;
    for (ValueID store = m_waitingStores[object.index()]; store; store = m_waitingStores[store.index()])
        need(store);
}

static_assert(storesThatMoveWithBirthsInSweep.contains(Opcode::PutSlot) && !storesThatMoveWithBirthsInSweep.contains(Opcode::AppendElement));

static bool storeMayMoveWithBirthInSweep(const Operation& store)
{
    if (!storesThatMoveWithBirthsInSweep.contains(store.opcode))
        return false;
    return store.opcode != Opcode::PutSlotOrAdd || store.orderWordAfter();
}

namespace {

struct BirthThatMayMove {
    ValueID allocation;
    BlockID whereCellIsNeeded;
    BlockID whereItComesToStand;
    uint32_t numberOfUsesSeen { 0 };
    bool isFollowed { false };
    Vector<ValueID, 4> stores;
    Vector<ValueID, 4> birthsThatListIt;
};

struct BlockBelowBirth {
    BlockID above;
    BlockID leap;
    BlockID nearestFreeOfHandlers;
    BlockID nearestSideOfBranch;
    BlockID aboveEdgesSeen;
    uint32_t depth { 0 };
    uint32_t numberOfEdgesAwaited { 0 };
    uint32_t numberOfEdgesTakenInto { std::numeric_limits<uint32_t>::max() };
    uint32_t look { 0 };
    bool isLookedAt { false };
};

struct OperationThatMoves {
    BlockID to;
    ValueID operation;
};

} // anonymous namespace

static bool birthMayMoveInSweep(const Program& program, const Facts& facts, const Operation& made)
{
    if (!made.traits().isUnobservable() || made.storage() != AllocationStorage::Heap)
        return false;
    switch (made.opcode) {
    case Opcode::NewArray:
        return true;
    case Opcode::NewClosure:
        if (made.isHeldByClosureTable() || made.isMadeOnDemand() || (made.closureForm() != ClosureForm::Full && made.closureForm() != ClosureForm::Slim))
            return false;
        return !program.at(made.function()).constructedClass;
    case Opcode::NewObject: {
        LayoutID declared = declaredLayoutOf(program, made.layout());
        if (made.prototypeOperand() || !declared || program.at(declared).has(LayoutFlags::IsConstructorFunction))
            return false;
        switch (program.at(declared).kind) {
        case LayoutKind::Record:
        case LayoutKind::Environment:
            return true;
        case LayoutKind::Instance:
            return facts.isEvaluatedAtMostOnce(declared);
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

bool Sweep::beginLookingForBirthsToMove()
{
    const Code& code = *m_code;
    if (!code.opcodesOfValues().intersects(birthsThatSweepMayMove))
        return false;
    if (code.role() != CodeRole::Body || code.hasSuspensions())
        return false;
    return code.has(Mechanism::EscapeAnalysis, m_inputs->facts);
}

void Sweep::noteIfBirthMayMove(ValueID allocation, const Operation& made)
{
    const Code& code = *m_code;
    if (ValueID user = m_arrays.onlyUsers[allocation.index()]) {
        const Operation& only = code.at(user);
        if (only.blockIndex == made.blockIndex && !storesThatMoveWithBirthsInSweep.contains(only.opcode) && !birthsThatSweepMayMove.contains(only.opcode))
            return;
    }
    const Program& program = m_inputs->program;
    const Block& block = code.at(made.block());
    if (block.handler || !block.terminator.numberOfEdges || (!block.isInLoop && program.at(code.function()).isModuleBody()))
        return;
    if (birthMayMoveInSweep(program, m_inputs->facts, made))
        m_birthsThatMayMove.append(allocation);
}

SweepTotals Sweep::sweepAgainBehindMovedBirths(const SweepInputs& inputs, Code& code)
{
    m_liveness = nullptr;
    Sweep again(m_rooms);
    again.m_sweepsAgainBehindMovedBirths = true;
    SweepTotals totals = again.run(inputs, code);
    totals.operationsSwept += m_totals.operationsSwept;
    totals.allocationsSunk = m_totals.allocationsSunk;
    totals.branchesThatDecideNothing += m_totals.branchesThatDecideNothing;
    return totals;
}

bool Sweep::moveBirthsToWhereCellsAreNeeded()
{
    Code& code = *m_code;
    const uint32_t numberOfBlocks = code.numberOfBlocks();
    constexpr uint8_t usesAreNotCounted = std::numeric_limits<uint8_t>::max();

    SweepBits mayMove;
    mayMove.ensureSize(code.numberOfValues());
    uint32_t numberThatMayMove = 0;
    for (ValueID allocation : m_birthsThatMayMove) {
        if (code.useCountOf(allocation) == usesAreNotCounted)
            continue;
        mayMove.quickSet(allocation.index());
        ++numberThatMayMove;
    }
    uint32_t numberOfOperationsLookedAt = 0;

    auto namesBirth = [&](ValueID name) ALWAYS_INLINE_LAMBDA {
        return name && mayMove.quickGet(name.index());
    };
    auto forEachUseOfBirthIn = [&](BlockID id, uint32_t first, const auto& noteUse, const auto& noteBirth) ALWAYS_INLINE_LAMBDA {
        const Block& block = code.at(id);
        auto noteRead = [&](ValueID read) {
            if (namesBirth(read))
                noteUse(read, ValueID(), false);
        };
        forEachValueReadAtEndOfSweptBlock(block, noteRead);
        forEachArgumentCountedBySweepAtEndOf(code, m_arrays.marks, m_states[id.index()], block, [&](ValueID argument, ValueID) {
            noteRead(argument);
        });

        std::span<const ValueID> users = code.valuesOf(id);
        for (size_t index = users.size(); index--;) {
            ValueID user = users[index];
            if (user.index() < first)
                break;
            ++numberOfOperationsLookedAt;
            const Operation& operation = code.at(user);
            bool namesSomeBirth = false;
            forEachOperandNeededBySweep(operation, [&](ValueID operand) ALWAYS_INLINE_LAMBDA {
                namesSomeBirth |= namesBirth(operand);
            });
            if (namesSomeBirth) {
                ValueID filled;
                bool onlyFillsCell = storeMayMoveWithBirthInSweep(operation) && resultRuleOf(operation.opcode) == ResultRule::None && !testsItsOwnGuess(operation) && isStoreIntoOwnObject(operation, filled);
                forEachOperandNeededBySweep(operation, [&](ValueID operand) {
                    if (namesBirth(operand))
                        noteUse(operand, user, onlyFillsCell && operand == filled);
                });
            }
            if (namesBirth(user))
                noteBirth(user);
        }
    };

    for (size_t firstOfBlock = 0, end = 0; firstOfBlock < m_birthsThatMayMove.size(); firstOfBlock = end) {
        BlockID id = code.at(m_birthsThatMayMove[firstOfBlock]).block();
        while (end < m_birthsThatMayMove.size() && code.at(m_birthsThatMayMove[end]).block() == id)
            ++end;
        forEachUseOfBirthIn(id, m_birthsThatMayMove[end - 1].index() + 1, [&](ValueID birth, ValueID user, bool onlyFillsCell) {
            if (onlyFillsCell || code.at(birth).block() != id || namesBirth(user))
                return;
            mayMove.quickClear(birth.index());
            --numberThatMayMove;
        }, [](ValueID) { });
    }
    m_totals.operationsSwept += numberOfOperationsLookedAt;
    numberOfOperationsLookedAt = 0;
    if (!numberThatMayMove)
        return false;

    auto standsOnOneSideOfBranch = [&](BlockID id) {
        BlockID above;
        for (EdgeID edge = code.at(id).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
            if (!isSweptEdgeTaken(code, m_states, edge, code.at(edge)))
                continue;
            if (above)
                return false;
            above = code.at(edge).from;
        }
        if (!above)
            return false;
        const Terminator& end = code.at(above).terminator;
        for (uint32_t index = 0; index < end.numberOfEdges; ++index) {
            BlockID other = code.at(end.edge(index)).to;
            if (other != id && code.at(other).frequency <= code.at(id).frequency)
                return true;
        }
        return false;
    };

    Vector<BirthThatMayMove, 4> births;
    for (ValueID allocation : m_birthsThatMayMove) {
        if (!mayMove.quickGet(allocation.index()))
            continue;
        births.append(BirthThatMayMove { allocation, { }, { }, 0, false, { }, { } });
    }
    AOT::sort(births, { }, &BirthThatMayMove::allocation);
    auto recordOf = [&](ValueID allocation) -> BirthThatMayMove& {
        return *std::ranges::lower_bound(births, allocation, { }, &BirthThatMayMove::allocation);
    };

    Vector<BlockBelowBirth> blocksBelow;
    blocksBelow.fill(BlockBelowBirth { }, numberOfBlocks);
    auto blockAboveAtDepth = [&](BlockID id, uint32_t depth) {
        while (blocksBelow[id.index()].depth > depth) {
            BlockID leap = blocksBelow[id.index()].leap;
            id = blocksBelow[leap.index()].depth >= depth ? leap : blocksBelow[id.index()].above;
        }
        return id;
    };
    auto standsAtOrAbove = [&](BlockID upper, BlockID lower) {
        uint32_t depth = blocksBelow[upper.index()].depth;
        return depth <= blocksBelow[lower.index()].depth && blockAboveAtDepth(lower, depth) == upper;
    };
    auto nearestBlockAboveBoth = [&](BlockID first, BlockID second) {
        first = blockAboveAtDepth(first, blocksBelow[second.index()].depth);
        second = blockAboveAtDepth(second, blocksBelow[first.index()].depth);
        while (first != second) {
            bool leapsDiffer = blocksBelow[first.index()].leap != blocksBelow[second.index()].leap;
            first = leapsDiffer ? blocksBelow[first.index()].leap : blocksBelow[first.index()].above;
            second = leapsDiffer ? blocksBelow[second.index()].leap : blocksBelow[second.index()].above;
        }
        return first;
    };

    Vector<BlockID, 16> blocksToLookAt;
    Vector<uint32_t, 8> birthsFollowedInLook;
    uint32_t numberOfUsesAwaited = 0;
    uint32_t look = 0;
    auto layBelow = [&](BlockID id, BlockID above) {
        BlockBelowBirth& laid = blocksBelow[id.index()];
        laid.look = look;
        laid.numberOfEdgesAwaited = 0;
        laid.above = above;
        laid.leap = id;
        laid.depth = 0;
        laid.nearestFreeOfHandlers = BlockID();
        laid.nearestSideOfBranch = BlockID();
        if (above) {
            const BlockBelowBirth& upper = blocksBelow[above.index()];
            const BlockBelowBirth& leap = blocksBelow[upper.leap.index()];
            bool leapsAreOfOneLength = upper.depth - leap.depth == leap.depth - blocksBelow[leap.leap.index()].depth;
            laid.depth = upper.depth + 1;
            laid.leap = leapsAreOfOneLength ? leap.leap : above;
            laid.nearestFreeOfHandlers = upper.nearestFreeOfHandlers;
            laid.nearestSideOfBranch = standsOnOneSideOfBranch(id) ? id : upper.nearestSideOfBranch;
        }
        if (!code.at(id).handler && !code.at(id).isHandlerBlock() && !code.at(id).meetsStepWrittenByHand)
            laid.nearestFreeOfHandlers = id;
        blocksToLookAt.append(id);
    };
    auto noteEdgeOutOf = [&](BlockID from, const Edge& edge) {
        BlockID to = edge.to;
        BlockBelowBirth& below = blocksBelow[to.index()];
        if (below.isLookedAt || code.at(to).isEntry)
            return;
        if (below.look != look) {
            below.look = look;
            below.aboveEdgesSeen = from;
            if (below.numberOfEdgesTakenInto == std::numeric_limits<uint32_t>::max()) {
                below.numberOfEdgesTakenInto = 0;
                for (EdgeID into = code.at(to).firstEdgeIn(); into; into = code.at(into).nextInto())
                    below.numberOfEdgesTakenInto += isSweptEdgeTaken(code, m_states, into, code.at(into));
            }
            below.numberOfEdgesAwaited = below.numberOfEdgesTakenInto;
        } else
            below.aboveEdgesSeen = nearestBlockAboveBoth(below.aboveEdgesSeen, from);
        if (--below.numberOfEdgesAwaited)
            return;
        layBelow(to, below.aboveEdgesSeen);
    };

    const uint32_t firstBirth = births.first().allocation.index();
    for (size_t indexOfTop = 0; indexOfTop < births.size(); ++indexOfTop) {
        if (births[indexOfTop].isFollowed || !mayMove.quickGet(births[indexOfTop].allocation.index()))
            continue;
        ++look;
        layBelow(code.at(births[indexOfTop].allocation).block(), BlockID());
        do {
            BlockID id = blocksToLookAt.takeLast();
            blocksBelow[id.index()].isLookedAt = true;
            forEachUseOfBirthIn(id, firstBirth, [&](ValueID allocation, ValueID user, bool onlyFillsCell) {
                BirthThatMayMove& birth = recordOf(allocation);
                if (!birth.isFollowed && code.at(allocation).block() != id) {
                    mayMove.quickClear(allocation.index());
                    return;
                }
                ++birth.numberOfUsesSeen;
                RELEASE_ASSERT(birth.numberOfUsesSeen <= code.useCountOf(allocation));
                if (birth.isFollowed)
                    --numberOfUsesAwaited;
                if (onlyFillsCell || namesBirth(user)) {
                    auto& users = onlyFillsCell ? birth.stores : birth.birthsThatListIt;
                    if (users.isEmpty() || users.last() != user)
                        users.append(user);
                    return;
                }
                birth.whereCellIsNeeded = birth.whereCellIsNeeded ? nearestBlockAboveBoth(birth.whereCellIsNeeded, id) : id;
                BlockID from = code.at(allocation).block();
                BlockID side = blocksBelow[birth.whereCellIsNeeded.index()].nearestSideOfBranch;
                if (side && side != from && standsAtOrAbove(from, side))
                    return;
                mayMove.quickClear(allocation.index());
                if (birth.isFollowed)
                    numberOfUsesAwaited -= code.useCountOf(allocation) - birth.numberOfUsesSeen;
            }, [&](ValueID allocation) {
                BirthThatMayMove& birth = recordOf(allocation);
                birth.isFollowed = true;
                numberOfUsesAwaited += code.useCountOf(allocation) - birth.numberOfUsesSeen;
                birthsFollowedInLook.append(static_cast<uint32_t>(&birth - births.begin()));
            });
            if (!numberOfUsesAwaited)
                break;
            const Block& block = code.at(id);
            for (uint32_t index = 0; index < block.terminator.numberOfEdges; ++index)
                noteEdgeOutOf(id, code.at(block.terminator.edge(index)));
            if (EdgeID handler = handlerOfSweptBlock(block, m_states[id.index()]))
                noteEdgeOutOf(id, code.at(handler));
        } while (!blocksToLookAt.isEmpty());
        for (uint32_t index : birthsFollowedInLook) {
            if (births[index].numberOfUsesSeen != code.useCountOf(births[index].allocation))
                mayMove.quickClear(births[index].allocation.index());
        }
        birthsFollowedInLook.shrink(0);
        blocksToLookAt.shrink(0);
        numberOfUsesAwaited = 0;
    }
    m_totals.operationsSwept += numberOfOperationsLookedAt;

    Vector<OperationThatMoves, 8> moves;
    uint32_t numberOfAllocationsSunk = 0;
    for (size_t indexOfBirth = births.size(); indexOfBirth--;) {
        BirthThatMayMove& birth = births[indexOfBirth];
        const Operation& made = code.at(birth.allocation);
        BlockID from = made.block();
        birth.whereItComesToStand = from;
        if (!mayMove.quickGet(birth.allocation.index()))
            continue;
        auto standsBelowBirth = [&](BlockID id) {
            return id && id != from && standsAtOrAbove(from, id);
        };

        BlockID to = birth.whereCellIsNeeded;
        for (ValueID lister : birth.birthsThatListIt) {
            BlockID ofLister = recordOf(lister).whereItComesToStand;
            to = to ? nearestBlockAboveBoth(to, ofLister) : ofLister;
        }
        if (standsBelowBirth(to))
            to = blocksBelow[to.index()].nearestFreeOfHandlers;
        if (!standsBelowBirth(to) || !standsBelowBirth(blocksBelow[to.index()].nearestSideOfBranch))
            continue;

        size_t numberOfMovesBefore = moves.size();
        moves.append({ to, birth.allocation });
        bool everythingHasItsPlace = true;
        for (ValueID store : birth.stores) {
            BlockID at = code.at(store).block();
            if (standsAtOrAbove(to, at))
                continue;
            everythingHasItsPlace &= standsAtOrAbove(at, to) && !code.at(at).handler;
            moves.append({ to, store });
        }
        AOT::sort(moves.mutableSpan().subspan(numberOfMovesBefore), { }, &OperationThatMoves::operation);
        std::span<const ValueID> behind = code.valuesOf(to);
        for (size_t index = numberOfMovesBefore; index < moves.size() && everythingHasItsPlace; ++index) {
            ValueID moved = moves[index].operation;
            everythingHasItsPlace &= behind.empty() || moved < behind.front();
            everythingHasItsPlace &= index == numberOfMovesBefore || standsAtOrAbove(code.at(moves[index - 1].operation).block(), code.at(moved).block());
            forEachOperandNeededBySweep(code.at(moved), [&](ValueID operand) {
                everythingHasItsPlace &= !operand || operand < moved;
            });
        }
        if (!everythingHasItsPlace) {
            moves.shrink(numberOfMovesBefore);
            continue;
        }
        ++numberOfAllocationsSunk;
        birth.whereItComesToStand = to;
    }
    if (moves.isEmpty())
        return false;

    SweepBits isMoved;
    isMoved.ensureSize(code.numberOfValues());
    Vector<BlockID, 8> blocksLeft;
    for (const OperationThatMoves& move : moves) {
        isMoved.quickSet(move.operation.index());
        blocksLeft.append(code.at(move.operation).block());
    }
    AOT::sort(blocksLeft);
    blocksLeft.shrink(std::ranges::unique(blocksLeft).begin() - blocksLeft.begin());
    for (BlockID id : blocksLeft) {
        ValueID* link = &code.m_blocks[id.index()].m_last;
        while (*link) {
            Operation& record = code.m_operations[link->index()];
            if (isMoved.quickGet(link->index()))
                *link = record.previous;
            else
                link = &record.previous;
        }
    }

    AOT::sort(moves, { }, [](const OperationThatMoves& move) {
        return std::tuple(move.to, move.operation);
    });
    for (size_t first = 0, end = 0; first < moves.size(); first = end) {
        BlockID to = moves[first].to;
        while (end < moves.size() && moves[end].to == to)
            ++end;
        ValueID* link = &code.m_blocks[to.index()].m_last;
        while (*link)
            link = &code.m_operations[link->index()].previous;
        for (size_t index = end; index-- > first;) {
            Operation& record = code.m_operations[moves[index].operation.index()];
            *link = moves[index].operation;
            record.blockIndex = to.index();
            record.previous = ValueID();
            link = &record.previous;
        }
    }

    m_totals.allocationsSunk += numberOfAllocationsSunk;
    m_hasMovedBirths = true;
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
