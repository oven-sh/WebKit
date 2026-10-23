/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinearScanInternals.h"

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include "AOTLinearScanOperations.h"
#include "AOTRelocation.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3ExtractValue.h"
#include "B3MemoryValue.h"
#include "B3RelocatableValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <cstring>

namespace JSC { namespace AOT { namespace Typed {

using namespace LinearScan;

namespace {

constexpr unsigned maxBlocks = 60000;

std::optional<int64_t> foldBinary(B3::Opcode opcode, bool isWide, int64_t a, int64_t b)
{
    uint64_t ua = static_cast<uint64_t>(a);
    uint64_t ub = static_cast<uint64_t>(b);
    uint64_t result;
    switch (opcode) {
    case B3::Add:
        result = ua + ub;
        break;
    case B3::Sub:
        result = ua - ub;
        break;
    case B3::Mul:
        result = ua * ub;
        break;
    case B3::BitAnd:
        result = ua & ub;
        break;
    case B3::BitOr:
        result = ua | ub;
        break;
    case B3::BitXor:
        result = ua ^ ub;
        break;
    case B3::Shl:
        result = isWide ? ua << (ub & 63) : static_cast<uint64_t>(static_cast<uint32_t>(ua) << (ub & 31));
        break;
    case B3::SShr:
        result = isWide ? static_cast<uint64_t>(a >> (ub & 63)) : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a) >> (ub & 31)));
        break;
    case B3::ZShr:
        result = isWide ? ua >> (ub & 63) : static_cast<uint64_t>(static_cast<uint32_t>(ua) >> (ub & 31));
        break;
    default:
        return std::nullopt;
    }
    if (!isWide)
        return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(result)));
    return static_cast<int64_t>(result);
}

std::optional<bool> foldComparison(B3::Opcode opcode, bool isWide, int64_t a, int64_t b)
{
    if (!isWide) {
        a = static_cast<int32_t>(a);
        b = static_cast<int32_t>(b);
    }
    uint64_t ua = isWide ? static_cast<uint64_t>(a) : static_cast<uint32_t>(a);
    uint64_t ub = isWide ? static_cast<uint64_t>(b) : static_cast<uint32_t>(b);
    switch (opcode) {
    case B3::Equal:
        return a == b;
    case B3::NotEqual:
        return a != b;
    case B3::LessThan:
        return a < b;
    case B3::GreaterThan:
        return a > b;
    case B3::LessEqual:
        return a <= b;
    case B3::GreaterEqual:
        return a >= b;
    case B3::Above:
        return ua > ub;
    case B3::Below:
        return ua < ub;
    case B3::AboveEqual:
        return ua >= ub;
    case B3::BelowEqual:
        return ua <= ub;
    default:
        return std::nullopt;
    }
}

std::optional<B3::Opcode> invertedComparison(B3::Opcode opcode, bool comparesDoubles)
{
    switch (opcode) {
    case B3::Equal:
        return B3::NotEqual;
    case B3::NotEqual:
        return B3::Equal;
    default:
        break;
    }
    if (comparesDoubles)
        return std::nullopt;
    switch (opcode) {
    case B3::LessThan:
        return B3::GreaterEqual;
    case B3::GreaterThan:
        return B3::LessEqual;
    case B3::LessEqual:
        return B3::GreaterThan;
    case B3::GreaterEqual:
        return B3::LessThan;
    case B3::Above:
        return B3::BelowEqual;
    case B3::Below:
        return B3::AboveEqual;
    case B3::AboveEqual:
        return B3::Below;
    case B3::BelowEqual:
        return B3::Above;
    default:
        return std::nullopt;
    }
}

std::optional<int64_t> valueGivenPhi(const B3::Value* value, const B3::Value* phi, int64_t arriving, unsigned depth = 0)
{
    constexpr unsigned maxDepth = 4;
    if (value == phi)
        return arriving;
    if (value->hasInt())
        return value->asInt();
    if (depth == maxDepth || value->numChildren() != 2 || !value->child(0)->type().isInt() || !value->child(1)->type().isInt())
        return std::nullopt;
    std::optional<int64_t> left = valueGivenPhi(value->child(0), phi, arriving, depth + 1);
    std::optional<int64_t> right = left ? valueGivenPhi(value->child(1), phi, arriving, depth + 1) : std::nullopt;
    if (!right)
        return std::nullopt;
    if (std::optional<bool> truth = foldComparison(value->opcode(), value->child(0)->type() == B3::Int64, *left, *right))
        return *truth;
    return foldBinary(value->opcode(), value->type() == B3::Int64, *left, *right);
}

std::optional<NodeType> nodeTypeOf(B3::Type type)
{
    switch (type.kind()) {
    case B3::Void:
        return NodeType::Void;
    case B3::Int32:
        return NodeType::Int32;
    case B3::Int64:
        return NodeType::Int64;
    case B3::Double:
        return NodeType::Double;
    case B3::Tuple:
        return NodeType::Tuple;
    default:
        return std::nullopt;
    }
}

} // anonymous namespace

void LinearScanCodeGenerator::setUpRegisters()
{
    uint64_t excluded = bitsOf(RegisterSet::macroClobberedGPRs()) | bitsOf(RegisterSet::stackRegisters()) | bitOf(Reg(MacroAssembler::fpTempRegister));
    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    calleeSaves.exclude(m_code.unsavedCalleeSaves());
    calleeSaves.merge(m_code.policy().registers.savedByCallee);
    m_calleeSaves = bitsOf(calleeSaves) & bitsOf(m_code.mutableRegs());
    for (Reg reg : m_code.regsInPriorityOrder(B3::GP)) {
        if (excluded & bitOf(reg))
            continue;
        m_registerOrder.append(static_cast<uint8_t>(reg.index()));
        if (!(m_calleeSaves & bitOf(reg)))
            m_generalRegisters |= bitOf(reg);
    }
    for (Reg reg : m_code.regsInPriorityOrder(B3::FP)) {
        if ((excluded | m_calleeSaves) & bitOf(reg))
            continue;
        m_registerOrder.append(static_cast<uint8_t>(reg.index()));
        m_doubleRegisters |= bitOf(reg);
    }
    std::ranges::stable_sort(m_registerOrder, { }, [&](uint8_t reg) {
        return !!(m_calleeSaves >> reg & 1);
    });
    const B3::Air::LayoutPolicy& layout = m_code.policy().layout;
    m_prefersSmallCode = layout.makesGapsForRareBlocks || layout.placesRareBlocksAfterGuards;
    if (isX86_64() && m_prefersSmallCode)
        m_layout = layout.placesRareBlocksAfterGuards ? Layout::AsControlFlows : Layout::RareCodeInGaps;
}

static bool truthAtMerge(const B3::BasicBlock* merge, int64_t arriving)
{
    const B3::Value* condition = merge->last()->child(0);
    int64_t value = *valueGivenPhi(condition, merge->at(0), arriving);
    return condition->type() == B3::Int64 ? !!value : !!static_cast<int32_t>(value);
}

void LinearScanCodeGenerator::findMergesOnlyBranchedOn()
{
    constexpr unsigned maxValuesInMerge = 6;
    BitVector watched;
    Vector<std::pair<const B3::Value*, const B3::BasicBlock*>, 16> owners;
    for (B3::BasicBlock* block : m_procedure) {
        if (block->size() > maxValuesInMerge || block->at(0)->opcode() != B3::Phi || block->last()->opcode() != B3::Branch)
            continue;
        const B3::Value* phi = block->at(0);
        if (phi->type() != B3::Int32 && phi->type() != B3::Int64)
            continue;
        bool fits = true;
        for (unsigned index = 1; index + 1 < block->size() && fits; ++index)
            fits = valueGivenPhi(block->at(index), phi, 0).has_value();
        if (!fits || !valueGivenPhi(block->last()->child(0), phi, 0))
            continue;
        m_hasMergesOnlyBranchedOn = true;
        m_mergesOnlyBranchedOn.set(block->index());
        for (unsigned index = 0; index + 1 < block->size(); ++index) {
            watched.set(block->at(index)->index());
            owners.append({ block->at(index), block });
        }
    }
    if (!m_hasMergesOnlyBranchedOn)
        return;
    for (B3::BasicBlock* block : m_procedure) {
        for (B3::Value* value : *block) {
            for (B3::Value* child : value->children()) {
                if (!watched.get(child->index()))
                    continue;
                for (auto [watchedValue, owner] : owners) {
                    if (watchedValue == child && owner != block)
                        m_mergesOnlyBranchedOn.clear(owner->index());
                }
            }
        }
    }
}

static const B3::Value* valueSentTo(const B3::Value* phi, const B3::BasicBlock* sender)
{
    constexpr unsigned maxValuesLookedAt = 16;
    unsigned size = sender->size();
    for (unsigned back = 1; back <= size && back <= maxValuesLookedAt; ++back) {
        const B3::Value* value = sender->at(size - back);
        if (value->opcode() == B3::Upsilon && value->as<B3::UpsilonValue>()->phi() == phi)
            return value->child(0);
    }
    return nullptr;
}

static const B3::Value* constantSentTo(const B3::Value* phi, const B3::BasicBlock* sender)
{
    const B3::Value* sent = valueSentTo(phi, sender);
    return sent && sent->hasInt() ? sent : nullptr;
}

void LinearScanCodeGenerator::findBranchesToTakeOver()
{
    if (!m_hasMergesOnlyBranchedOn)
        return;
    for (B3::BasicBlock* block : m_procedure) {
        if (block->last()->opcode() != B3::Jump)
            continue;
        B3::BasicBlock* merge = block->successorBlock(0);
        for (unsigned hops = 0; hops < 8 && merge->size() == 1 && merge->last()->opcode() == B3::Jump; ++hops)
            merge = merge->successorBlock(0);
        if (!m_mergesOnlyBranchedOn.get(merge->index()) || merge == block)
            continue;
        const B3::Value* phi = merge->at(0);
        const B3::Value* sent = valueSentTo(phi, block);
        if (!sent || sent->hasInt())
            continue;
        bool isNegated = false;
        const B3::Value* condition = merge->last()->child(0);
        while (condition != phi) {
            bool testsZero = (condition->opcode() == B3::Equal || condition->opcode() == B3::NotEqual) && condition->child(1)->hasInt() && !condition->child(1)->asInt();
            bool negatesTruth = condition->opcode() == B3::BitXor && condition->child(1)->hasInt() && condition->child(1)->asInt() == 1 && isComparison(condition->child(0)->opcode());
            if (!testsZero && !negatesTruth)
                break;
            isNegated ^= negatesTruth || condition->opcode() == B3::Equal;
            condition = condition->child(0);
        }
        if (condition != phi)
            continue;
        if (m_branchesTakenOver.isEmpty())
            m_branchesTakenOver.fill(0, m_procedure.size());
        m_branchesTakenOver[block->index()] = (merge->index() + 1) << 1 | isNegated;
    }
}

B3::BasicBlock* LinearScanCodeGenerator::threadedTarget(B3::BasicBlock* source, B3::BasicBlock* target, B3::FrequencyClass* rarestEdge) const
{
    for (unsigned hops = 0; hops < 8; ++hops) {
        bool endsInJump = target->last()->opcode() == B3::Jump;
        if (endsInJump && target->size() == 1) {
            target = target->successorBlock(0);
            continue;
        }
        if (!m_hasMergesOnlyBranchedOn)
            break;
        B3::BasicBlock* sender = source;
        B3::BasicBlock* merge = target;
        if (endsInJump && target->size() == 2 && target->at(0)->opcode() == B3::Upsilon) {
            sender = target;
            merge = target->successorBlock(0);
        }
        if (!m_mergesOnlyBranchedOn.get(merge->index()))
            break;
        const B3::Value* constant = constantSentTo(merge->at(0), sender);
        if (!constant)
            break;
        const B3::FrequentedBlock& edge = merge->successor(truthAtMerge(merge, constant->asInt()) ? 0 : 1);
        if (rarestEdge)
            *rarestEdge = B3::rarerOf(*rarestEdge, edge.frequency());
        target = edge.block();
    }
    return target;
}

bool LinearScanCodeGenerator::jumpedStraightTo(const B3::BasicBlock* source, const B3::BasicBlock* merge) const
{
    for (unsigned index = 0; index < source->numSuccessors(); ++index) {
        const B3::BasicBlock* successor = source->successorBlock(index);
        for (unsigned hops = 0; hops < 8 && successor != merge && successor->size() == 1 && successor->last()->opcode() == B3::Jump; ++hops)
            successor = successor->successorBlock(0);
        if (successor == merge)
            return true;
    }
    return false;
}

bool LinearScanCodeGenerator::orderBlocks()
{
    unsigned numberOfBlocks = m_procedure.size();
    if (numberOfBlocks > maxBlocks)
        return refuse();
    constexpr uint32_t unseen = UINT32_MAX;
    constexpr uint32_t seen = UINT32_MAX - 1;
    Vector<uint32_t, 64> indexInOrder;
    indexInOrder.fill(unseen, numberOfBlocks);

    // An edge as it leaves a block that is reached, and where it leads past the blocks that only hand on. Worked out once, when the walk enters the block.
    struct Exit {
        uint32_t target;
        B3::FrequencyClass frequency;
        B3::FrequencyClass rarest;
    };
    struct ExitsOfBlock {
        uint32_t first;
        uint32_t count;
    };
    struct Frame {
        uint32_t block;
        uint32_t next;
    };
    Vector<Exit, 128> exits;
    Vector<ExitsOfBlock, 64> exitsOfBlocks;
    exitsOfBlocks.grow(numberOfBlocks);
    Vector<Frame, 64> stack;
    Vector<uint32_t, 64> postOrder;
    auto rankOf = [](B3::FrequencyClass frequency) -> unsigned {
        switch (frequency) {
        case B3::FrequencyClass::Normal:
            return 0;
        case B3::FrequencyClass::Unlikely:
            return 1;
        case B3::FrequencyClass::Rare:
            return 2;
        }
        return 0;
    };

    findMergesOnlyBranchedOn();
    findBranchesToTakeOver();
    auto enter = [&](uint32_t index) {
        B3::BasicBlock* block = m_procedure.at(index);
        uint32_t takenOver = m_branchesTakenOver.isEmpty() ? 0 : m_branchesTakenOver[index];
        const B3::BasicBlock* branching = takenOver ? m_procedure.at((takenOver >> 1) - 1) : block;
        unsigned count = takenOver ? 2 : block->numSuccessors();
        exitsOfBlocks[index] = { static_cast<uint32_t>(exits.size()), count };
        for (unsigned exit = 0; exit < count; ++exit) {
            const B3::FrequentedBlock& edge = branching->successor(takenOver & 1 ? 1 - exit : exit);
            B3::FrequencyClass rarest = edge.frequency();
            B3::BasicBlock* target = threadedTarget(block, edge.block(), &rarest);
            exits.append(Exit { target->index(), edge.frequency(), rarest });
        }
        indexInOrder[index] = seen;
        stack.append({ index, 0 });
    };
    auto exitsOf = [&](uint32_t index) -> std::span<const Exit> {
        return exits.span().subspan(exitsOfBlocks[index].first, exitsOfBlocks[index].count);
    };
    // The rarer an edge, the earlier it is walked, so that what it leads to comes late in the order.
    auto exitInVisitOrder = [&](std::span<const Exit> leaving, unsigned position) -> unsigned {
        unsigned count = leaving.size();
        if (count == 1)
            return 0;
        if (count == 2) {
            bool firstIsVisitedFirst = rankOf(leaving[0].frequency) > rankOf(leaving[1].frequency);
            return firstIsVisitedFirst ? position : 1 - position;
        }
        if (count == 3) {
            std::array<unsigned, 3> order { 2, 1, 0 };
            auto putInOrder = [&](unsigned& earlier, unsigned& later) {
                if (rankOf(leaving[earlier].frequency) < rankOf(leaving[later].frequency))
                    std::swap(earlier, later);
            };
            putInOrder(order[1], order[2]);
            putInOrder(order[0], order[1]);
            putInOrder(order[1], order[2]);
            return order[position];
        }
        return count - 1 - position;
    };
    enter(0);
    while (!stack.isEmpty()) {
        uint32_t block = stack.last().block;
        uint32_t next = stack.last().next;
        std::span<const Exit> leaving = exitsOf(block);
        if (next == leaving.size()) {
            postOrder.append(block);
            stack.removeLast();
            continue;
        }
        ++stack.last().next;
        uint32_t successor = leaving[exitInVisitOrder(leaving, next)].target;
        if (indexInOrder[successor] == unseen)
            enter(successor);
    }

    unsigned count = postOrder.size();
    auto blockAtIndex = [&](unsigned index) {
        return postOrder[count - 1 - index];
    };
    for (unsigned index = 0; index < count; ++index)
        indexInOrder[blockAtIndex(index)] = index;

    Vector<uint8_t, 64> isReachedNormally;
    Vector<uint8_t, 64> isReachedWithoutRareEdge;
    isReachedNormally.fill(0, count);
    isReachedWithoutRareEdge.fill(0, count);
    isReachedNormally[0] = 1;
    isReachedWithoutRareEdge[0] = 1;
    for (unsigned index = 0; index < count; ++index) {
        if (!isReachedWithoutRareEdge[index])
            continue;
        std::span<const Exit> leaving = exitsOf(blockAtIndex(index));
        B3::FrequencyClass likeliest = B3::FrequencyClass::Rare;
        for (const Exit& exit : leaving)
            likeliest = B3::maxFrequency(likeliest, exit.frequency);
        for (const Exit& exit : leaving) {
            if (exit.rarest == B3::FrequencyClass::Rare)
                continue;
            uint32_t target = indexInOrder[exit.target];
            isReachedWithoutRareEdge[target] = 1;
            if (isReachedNormally[index] && (exit.rarest == B3::FrequencyClass::Normal || (exit.frequency == likeliest && exit.rarest == exit.frequency)))
                isReachedNormally[target] = 1;
        }
    }

    unsigned numberOfRareBlocks = 0;
    for (unsigned index = 0; index < count; ++index)
        numberOfRareBlocks += !isReachedNormally[index];

    Vector<uint32_t, 64> linearOrder;
    linearOrder.reserveInitialCapacity(count);
    for (unsigned index = 0; index < count; ++index) {
        if (isReachedNormally[index])
            linearOrder.append(index);
    }
    // Rare blocks that lead to one another are one region. The regions come in the order of their first blocks, the blocks of a region in the order of the walk.
    Vector<uint32_t, 64> leader;
    auto find = [&](uint32_t index) {
        while (leader[index] != index) {
            leader[index] = leader[leader[index]];
            index = leader[index];
        }
        return index;
    };
    if (numberOfRareBlocks) {
        leader.grow(count);
        for (unsigned index = 0; index < count; ++index)
            leader[index] = index;
        for (unsigned index = 0; index < count; ++index) {
            if (isReachedNormally[index])
                continue;
            for (const Exit& exit : exitsOf(blockAtIndex(index))) {
                uint32_t target = indexInOrder[exit.target];
                if (isReachedNormally[target])
                    continue;
                uint32_t a = find(index);
                uint32_t b = find(target);
                leader[std::max(a, b)] = std::min(a, b);
            }
        }
        constexpr uint32_t none = UINT32_MAX;
        Vector<uint32_t, 64> nextInRegion;
        Vector<uint32_t, 64> lastInRegion;
        nextInRegion.fill(none, count);
        lastInRegion.fill(none, count);
        for (unsigned index = 0; index < count; ++index) {
            if (isReachedNormally[index])
                continue;
            uint32_t region = find(index);
            if (lastInRegion[region] != none)
                nextInRegion[lastInRegion[region]] = index;
            lastInRegion[region] = index;
        }
        for (unsigned index = 0; index < count; ++index) {
            if (isReachedNormally[index] || leader[index] != index)
                continue;
            for (uint32_t member = index; member != none; member = nextInRegion[member])
                linearOrder.append(member);
        }
    }

    m_blocks.grow(count);
    m_linearOf.fill(unseen, numberOfBlocks);
    m_reversePostOrder.grow(count);
    uint32_t lastRegion = 0;
    uint32_t leaderOfLastRegion = unseen;
    for (unsigned order = 0; order < count; ++order) {
        uint32_t index = linearOrder[order];
        Block& block = m_blocks[order];
        block.block = m_procedure.at(blockAtIndex(index));
        block.isReachedNormally = isReachedWithoutRareEdge[index];
        block.isRare = !isReachedNormally[index];
        if (block.isRare) {
            if (find(index) != leaderOfLastRegion) {
                leaderOfLastRegion = find(index);
                ++lastRegion;
            }
            block.region = lastRegion;
        }
        block.reversePostOrderIndex = index;
        m_linearOf[blockAtIndex(index)] = order;
        m_reversePostOrder[index] = order;
    }

    auto isReturnBlock = [](B3::BasicBlock* candidate) {
        if (candidate->last()->opcode() != B3::Return)
            return false;
        if (candidate->size() == 1)
            return true;
        return candidate->size() == 2 && candidate->at(0)->opcode() == B3::Phi && candidate->last()->numChildren() == 1 && candidate->last()->child(0) == candidate->at(0);
    };
    const bool duplicatesReturns = m_code.policy().layout.duplicatesReturns;
    m_successors.reserveCapacity(exits.size());
    m_edgeClasses.reserveCapacity(exits.size());
    for (unsigned order = 0; order < count; ++order) {
        Block& block = m_blocks[order];
        block.firstSuccessor = m_successors.size();
        uint32_t indexOfBlock = block.block->index();
        std::span<const Exit> leaving = exitsOf(indexOfBlock);
        if (uint32_t takenOver = m_branchesTakenOver.isEmpty() ? 0 : m_branchesTakenOver[indexOfBlock])
            block.branchTakenFrom = m_procedure.at((takenOver >> 1) - 1);
        // Compact code has one copy of a return that gives the same value whoever reaches it.
        if (duplicatesReturns && !block.branchTakenFrom && block.block->last()->opcode() == B3::Jump && isReturnBlock(m_procedure.at(leaving[0].target)) && !(m_prefersSmallCode && m_procedure.at(leaving[0].target)->size() == 1)) {
            block.returnBlock = m_procedure.at(leaving[0].target);
            continue;
        }
        block.numberOfSuccessors = leaving.size();
        for (const Exit& exit : leaving) {
            uint32_t target = m_linearOf[exit.target];
            m_successors.append(target);
            m_edgeClasses.append(exit.frequency);
            ++m_blocks[target].numberOfPredecessors;
            if (target <= order) {
                m_blocks[target].hasLaterPredecessor = true;
                if (m_blocks[target].region == block.region)
                    m_backEdges.append({ target, order });
            }
            if (indexInOrder[exit.target] <= block.reversePostOrderIndex) {
                m_hasCycles = true;
                m_blocks[target].hasRetreatingPredecessor = true;
            }
        }
    }
    if (m_blocks[0].numberOfPredecessors)
        return refuse();

    uint32_t total = 0;
    for (Block& block : m_blocks) {
        block.firstPredecessor = total;
        total += std::exchange(block.numberOfPredecessors, 0);
    }
    m_predecessors.grow(total);
    m_edgeOfPredecessor.grow(total);
    for (unsigned order = 0; order < count; ++order) {
        const Block& block = m_blocks[order];
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            Block& target = m_blocks[successorOf(block, index)];
            m_predecessors[target.firstPredecessor + target.numberOfPredecessors] = order;
            m_edgeOfPredecessor[target.firstPredecessor + target.numberOfPredecessors] = block.firstSuccessor + index;
            ++target.numberOfPredecessors;
            target.predecessor = order;
            target.incomingEdge = block.firstSuccessor + index;
        }
    }
    return true;
}

uint32_t LinearScanCodeGenerator::commonDominator(uint32_t a, uint32_t b) const
{
    while (a != b) {
        while (m_blocks[a].reversePostOrderIndex > m_blocks[b].reversePostOrderIndex)
            a = m_blocks[a].dominator;
        while (m_blocks[b].reversePostOrderIndex > m_blocks[a].reversePostOrderIndex)
            b = m_blocks[b].dominator;
    }
    return a;
}

void LinearScanCodeGenerator::computeDominatorsOverAllEdges()
{
    constexpr uint32_t unknown = UINT32_MAX;
    for (Block& block : m_blocks)
        block.dominator = unknown;
    m_blocks[0].dominator = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t index = 1; index < m_reversePostOrder.size(); ++index) {
            Block& block = m_blocks[m_reversePostOrder[index]];
            uint32_t dominator = unknown;
            for (unsigned predecessor = 0; predecessor < block.numberOfPredecessors; ++predecessor) {
                uint32_t source = m_predecessors[block.firstPredecessor + predecessor];
                if (m_blocks[source].dominator != unknown)
                    dominator = dominator == unknown ? source : commonDominator(dominator, source);
            }
            if (dominator != block.dominator) {
                block.dominator = dominator;
                changed = true;
            }
        }
    }
}

bool LinearScanCodeGenerator::dominates(uint32_t dominator, uint32_t block) const
{
    constexpr unsigned maxSteps = 64;
    if (dominator > block)
        return false;
    uint32_t index = m_blocks[dominator].reversePostOrderIndex;
    for (unsigned steps = 0; m_blocks[block].reversePostOrderIndex > index; ++steps) {
        if (steps == maxSteps)
            return false;
        block = m_blocks[block].dominator;
    }
    return block == dominator;
}

bool LinearScanCodeGenerator::enterBlock(uint32_t order, uint32_t& epoch, uint32_t& memoryEpoch)
{
    Block& block = m_blocks[order];
    epoch = 0;
    memoryEpoch = 0;
    bool epochsAgree = true;
    bool memoryEpochsAgree = true;
    uint32_t dominator = UINT32_MAX;
    uint32_t numberOfLivePredecessors = 0;
    for (unsigned index = 0; index < block.numberOfPredecessors; ++index) {
        uint32_t source = m_predecessors[block.firstPredecessor + index];
        const Block& predecessor = m_blocks[source];
        if (predecessor.reversePostOrderIndex >= block.reversePostOrderIndex || !predecessor.isReachable)
            continue;
        if (predecessor.onlySuccessor && predecessor.firstSuccessor + predecessor.onlySuccessor - 1 != m_edgeOfPredecessor[block.firstPredecessor + index])
            continue;
        ++numberOfLivePredecessors;
        block.livePredecessor = source;
        dominator = dominator == UINT32_MAX ? source : commonDominator(dominator, source);
        memoryEpochsAgree &= !memoryEpoch || memoryEpoch == predecessor.memoryEpochAtEnd;
        memoryEpoch = predecessor.memoryEpochAtEnd;
        if (predecessor.isRare && !block.isRare)
            continue;
        epochsAgree &= !epoch || epoch == predecessor.epochAtEnd;
        epoch = predecessor.epochAtEnd;
    }
    if (block.hasRetreatingPredecessor || !order) {
        block.numberOfLivePredecessors = UINT32_MAX;
        epoch = ++m_lastEpoch;
        memoryEpoch = ++m_lastMemoryEpoch;
        return true;
    }
    if (!numberOfLivePredecessors)
        return false;
    block.dominator = dominator;
    block.numberOfLivePredecessors = numberOfLivePredecessors;
    if (!epoch || !epochsAgree)
        epoch = ++m_lastEpoch;
    if (!memoryEpochsAgree)
        memoryEpoch = ++m_lastMemoryEpoch;
    return true;
}

void LinearScanCodeGenerator::findLoops()
{
    if (m_backEdges.isEmpty())
        return;
    std::ranges::sort(m_backEdges, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    });
    Vector<uint32_t, 8> open;
    for (auto [first, last] : m_backEdges) {
        while (!open.isEmpty() && m_loops[open.last() - 1].lastBlock < first)
            open.removeLast();
        if (!open.isEmpty() && m_loops[open.last() - 1].firstBlock == first)
            continue;
        for (uint32_t enclosing : open) {
            if (m_loops[enclosing - 1].lastBlock < last)
                m_loops[enclosing - 1].lastBlock = last;
        }
        m_loops.append(Loop { first, last, open.isEmpty() ? 0 : open.last() });
        open.append(m_loops.size());
    }
    for (uint32_t index = 0; index < m_loops.size(); ++index) {
        const Loop& loop = m_loops[index];
        for (uint32_t order = loop.firstBlock; order <= loop.lastBlock; ++order)
            m_blocks[order].loop = index + 1;
    }
}

void LinearScanCodeGenerator::estimateFrequencies()
{
    constexpr double rareEdgeShare = 1.0 / 1024;
    constexpr double unlikelyEdgeShare = 1.0 / 16;
    constexpr double returningBlockWeight = 0.25;
    constexpr double iterationsPerLoop = 10;
    constexpr double minFrequency = 1e-6;
    constexpr double maxFrequency = 1e6;
    unsigned count = m_blocks.size();
    Vector<double, 64> weights;
    Vector<double, 64> incoming;
    weights.fill(1, count);
    incoming.fill(0, count);
    auto edgeWeight = [&](const Block& block, unsigned index) {
        double weight = weights[successorOf(block, index)];
        switch (m_edgeClasses[block.firstSuccessor + index]) {
        case B3::FrequencyClass::Normal:
            return weight;
        case B3::FrequencyClass::Unlikely:
            return std::max(rareEdgeShare, weight * unlikelyEdgeShare);
        case B3::FrequencyClass::Rare:
            return rareEdgeShare;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (uint32_t order = count; order--;) {
        const Block& block = m_blocks[order];
        double weight = block.block->last()->opcode() == B3::Oops ? rareEdgeShare : 1;
        if (block.numberOfSuccessors) {
            double likeliest = 0;
            for (unsigned index = 0; index < block.numberOfSuccessors; ++index)
                likeliest = std::max(likeliest, edgeWeight(block, index));
            weight = std::max(rareEdgeShare, std::min(weight, likeliest));
        }
        weights[order] = weight;
    }
    Vector<bool, 64> leadsNowhere;
    leadsNowhere.fill(false, count);
    for (uint32_t order = 0; order < count; ++order)
        leadsNowhere[order] = weights[order] <= rareEdgeShare;
    if (m_procedure.policy().optimization.returnsAreUnlikely) {
        for (uint32_t order = 0; order < count; ++order) {
            if (m_blocks[order].block->last()->opcode() == B3::Return)
                weights[order] = std::max(rareEdgeShare, std::min(weights[order], returningBlockWeight));
        }
    }
    incoming[0] = 1;
    for (uint32_t order = 0; order < count; ++order) {
        Block& block = m_blocks[order];
        double flow = incoming[order];
        double frequency = flow;
        for (uint32_t loop = block.loop; loop; loop = m_loops[loop - 1].parent)
            frequency *= iterationsPerLoop;
        block.frequency = std::clamp(frequency, minFrequency, maxFrequency);
        if (block.onlySuccessor || block.numberOfSuccessors == 1) {
            uint32_t edge = block.firstSuccessor + (block.onlySuccessor ? block.onlySuccessor - 1 : 0);
            uint32_t successor = m_successors[edge];
            if (!m_successorDecidedOnEdge.isEmpty() && m_successorDecidedOnEdge[edge])
                successor = successorOf(m_blocks[successor], m_successorDecidedOnEdge[edge] - 1);
            if (successor > order)
                incoming[successor] += flow;
            continue;
        }
        double weightOfOthers = 0;
        double rare = 0;
        auto isRareEdge = [&](unsigned index, uint32_t successor) {
            return m_edgeClasses[block.firstSuccessor + index] == B3::FrequencyClass::Rare || (leadsNowhere[successor] && !leadsNowhere[order]);
        };
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            uint32_t successor = successorOf(block, index);
            if (successor <= order)
                continue;
            if (isRareEdge(index, successor))
                rare += rareEdgeShare;
            else
                weightOfOthers += edgeWeight(block, index);
        }
        rare = std::min(rare, 0.5);
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            uint32_t successor = successorOf(block, index);
            if (successor <= order)
                continue;
            incoming[successor] += flow * (isRareEdge(index, successor) ? rareEdgeShare : (1 - rare) * edgeWeight(block, index) / weightOfOthers);
        }
    }
}

void LinearScanCodeGenerator::dropRawUse(NodeID id)
{
    uint16_t& rawUses = node(id).rawUses;
    if (rawUses && rawUses != UINT16_MAX)
        --rawUses;
}

void LinearScanCodeGenerator::makeAlias(Node& alias, NodeID target, bool targetIsChild)
{
    alias.form = Form::Alias;
    alias.a = target;
    if (targetIsChild)
        dropRawUse(target);
}

void LinearScanCodeGenerator::releaseRawUse(NodeID id)
{
    Node& released = node(id);
    if (!m_allUsesAreCounted) {
        dropRawUse(id);
        return;
    }
    if (released.form != Form::Plain || !released.rawUses || released.rawUses == UINT16_MAX)
        return;
    if (--released.rawUses)
        return;
    if (!isPureOperation(released.opcode) && !(B3::isLoad(released.opcode) && !released.has(Traps)))
        return;
    if (released.a)
        releaseRawUse(released.a);
    if (released.b)
        releaseRawUse(released.b);
    if (released.c)
        releaseRawUse(released.c);
}

bool LinearScanCodeGenerator::isSameValue(NodeID a, NodeID b, unsigned depth) const
{
    if (a == b)
        return true;
    const Node& first = node(a);
    const Node& second = node(b);
    if (first.type != second.type)
        return false;
    if (first.form == Form::Constant)
        return second.form == Form::Constant && first.constant == second.constant;
    if (depth >= 3 || first.opcode != second.opcode || first.operandType != second.operandType || !isPureOperation(first.opcode))
        return false;
    if (first.form != Form::Plain || second.form != Form::Plain)
        return false;
    if (!first.a != !second.a || !first.b != !second.b)
        return false;
    if (first.a && !isSameValue(first.a, second.a, depth + 1))
        return false;
    return !first.b || isSameValue(first.b, second.b, depth + 1);
}

std::pair<NodeID, bool> LinearScanCodeGenerator::truthSourceOf(NodeID condition) const
{
    bool isNegated = false;
    for (;;) {
        const Node& n = node(condition);
        if (n.form != Form::Plain)
            return { condition, isNegated };
        if (n.opcode == B3::BitXor && isConstant(n.b) && node(n.b).constant == 1 && isComparison(node(n.a).opcode) && node(n.a).form == Form::Plain) {
            isNegated = !isNegated;
            condition = n.a;
            continue;
        }
        if ((n.opcode == B3::Equal || n.opcode == B3::NotEqual) && n.operandType != NodeType::Double && isZero(n.b) && node(n.a).type == n.operandType) {
            if (n.opcode == B3::Equal)
                isNegated = !isNegated;
            condition = n.a;
            continue;
        }
        return { condition, isNegated };
    }
}

std::optional<bool> LinearScanCodeGenerator::knownTruthOf(uint32_t order, NodeID condition)
{
    constexpr unsigned maxBlocksLookedAt = 24;
    auto [wanted, wantedIsNegated] = truthSourceOf(condition);
    for (unsigned steps = 0; order; ++steps) {
        steps += m_blocks[order].stepsToBlockThatKnowsTruth;
        order = m_blocks[order].nearestBlockThatKnowsTruth;
        if (!order || steps >= maxBlocksLookedAt)
            break;
        const Block& block = m_blocks[order];
        if (isSameValue(block.truthKnownAtEntry, wanted, 0))
            return block.truthKnownAtEntryHolds != wantedIsNegated;
        order = block.dominator;
    }
    return std::nullopt;
}

// The blocks up the dominators were entered before this one, and nothing that is read here changes while the values are imported.
void LinearScanCodeGenerator::noteTruthKnownAtEntry(uint32_t order)
{
    Block& block = m_blocks[order];
    if (!order)
        return;
    if (block.numberOfLivePredecessors == 1) {
        const Block& parent = m_blocks[block.livePredecessor];
        const Node& last = node(idOf(parent.block->last()));
        if (last.opcode == B3::Branch && !parent.onlySuccessor && node(last.a).type == last.operandType) {
            bool isTaken = successorOf(parent, 0) == order;
            bool isNotTaken = successorOf(parent, 1) == order;
            if (isTaken != isNotTaken) {
                auto [known, knownIsNegated] = truthSourceOf(last.a);
                block.truthKnownAtEntry = known;
                block.truthKnownAtEntryHolds = isTaken != knownIsNegated;
                block.nearestBlockThatKnowsTruth = order;
                return;
            }
        }
    }
    const Block& dominator = m_blocks[block.dominator];
    block.nearestBlockThatKnowsTruth = dominator.nearestBlockThatKnowsTruth;
    block.stepsToBlockThatKnowsTruth = dominator.stepsToBlockThatKnowsTruth + 1;
}

bool LinearScanCodeGenerator::isSameOperand(NodeID a, NodeID b) const
{
    if (a == b)
        return true;
    const Node& first = node(a);
    const Node& second = node(b);
    if (first.form != second.form || first.type != second.type)
        return false;
    switch (first.form) {
    case Form::Constant:
        return first.constant == second.constant;
    case Form::FixedRegister:
        return first.base == second.base;
    case Form::Rematerialized:
        return first.opcode == B3::Add && second.opcode == B3::Add && first.base == second.base && first.constant == second.constant;
    default:
        return false;
    }
}

uint64_t LinearScanCodeGenerator::operandKeyOf(NodeID id) const
{
    const Node& n = node(id);
    switch (n.form) {
    case Form::Constant:
        return static_cast<uint64_t>(n.constant) * 0x9e3779b97f4a7c15ULL + 1;
    case Form::FixedRegister:
        return 0x517cc1b727220a95ULL + n.base;
    case Form::Rematerialized:
        if (n.opcode == B3::Add)
            return (static_cast<uint64_t>(n.constant) + n.base) * 0xff51afd7ed558ccdULL + 2;
        return id;
    default:
        return id;
    }
}

static bool isValueNumbered(const Node& n)
{
    switch (n.opcode) {
    case B3::PopCount:
    case B3::Clz:
    case B3::Neg:
    case B3::BitwiseCast:
    case B3::IToD:
    case B3::Select:
    case B3::Abs:
    case B3::Sqrt:
    case B3::Floor:
    case B3::Ceil:
    case B3::FTrunc:
    case B3::FMax:
    case B3::FMin:
        return true;
    case B3::Div:
        return n.isDouble();
    default:
        return isPureOperation(n.opcode);
    }
}

bool LinearScanCodeGenerator::tryReuseValue(NodeID id)
{
    Node& n = node(id);
    if (!isValueNumbered(n))
        return false;
    uint64_t first = operandKeyOf(n.a);
    uint64_t second = n.b ? operandKeyOf(n.b) : 0;
    bool commutes = n.isDouble() ? n.opcode == B3::Add || n.opcode == B3::Mul : isCommutative(n.opcode) || n.opcode == B3::Equal || n.opcode == B3::NotEqual;
    if (commutes && first > second)
        std::swap(first, second);
    uint64_t hash = (first * 31 + second) * 31 + (n.c ? operandKeyOf(n.c) : 0);
    hash = (hash * 31 + static_cast<uint64_t>(n.opcode)) * 0x9e3779b97f4a7c15ULL;
    NodeID& known = m_knownValues[(hash >> 32) & (m_knownValues.size() - 1)];
    if (known) {
        const Node& earlier = node(known);
        bool isSameOperation = earlier.form == Form::Plain && earlier.opcode == n.opcode && earlier.type == n.type && earlier.operandType == n.operandType && !earlier.b == !n.b && !earlier.c == !n.c;
        if (isSameOperation && earlier.epoch == n.epoch) {
            bool hasSameOperands = isSameOperand(earlier.a, n.a) && (!n.b || isSameOperand(earlier.b, n.b));
            if (!hasSameOperands && commutes)
                hasSameOperands = isSameOperand(earlier.a, n.b) && isSameOperand(earlier.b, n.a);
            if (hasSameOperands && (!n.c || isSameOperand(earlier.c, n.c)) && dominates(earlier.block, n.block)) {
                releaseRawUse(n.a);
                if (n.b)
                    releaseRawUse(n.b);
                if (n.c)
                    releaseRawUse(n.c);
                makeAlias(n, known, false);
                return true;
            }
        }
    }
    known = id;
    return false;
}

bool LinearScanCodeGenerator::tryReuseLoad(B3::Value* value, NodeID id, uint32_t memoryEpoch)
{
    Node& load = node(id);
    uint64_t hash = ((operandKeyOf(load.a) * 31 + static_cast<uint64_t>(load.offset())) * 31 + static_cast<uint64_t>(load.opcode)) * 0x9e3779b97f4a7c15ULL;
    KnownLoad& known = m_knownLoads[(hash >> 32) & (m_knownLoads.size() - 1)];
    if (known.load && !load.has(Traps)) {
        const Node& earlier = node(known.load);
        bool isSameAccess = earlier.form == Form::Plain && earlier.opcode == load.opcode && earlier.type == load.type && earlier.constant == load.constant && isSameOperand(earlier.a, load.a);
        bool isUnchanged = known.memoryEpoch == memoryEpoch || (value->as<B3::MemoryValue>()->readsMutability() == B3::Mutability::Immutable && earlier.epoch == load.epoch);
        if (isSameAccess && isUnchanged && dominates(earlier.block, load.block)) {
            NodeID base = load.a;
            makeAlias(load, known.load, false);
            releaseRawUse(base);
            return true;
        }
    }
    known.load = id;
    known.memoryEpoch = memoryEpoch;
    return false;
}

bool LinearScanCodeGenerator::tryReusePureCall(const B3::PatchpointValue* patchpoint, NodeID id)
{
    if (patchpoint->type().isTuple() || patchpoint->type() == B3::Void || patchpoint->effects.terminal)
        return false;
    size_t hash = static_cast<size_t>(patchpoint->pureFunction->id * 0x9e3779b97f4a7c15ULL >> 32);
    if (patchpoint->pureFunction->dependsOnChildren) {
        for (const B3::Value* child : patchpoint->children())
            hash = hash * 31 + resolved(child);
    }
    NodeID& known = m_knownPureCalls[hash % m_knownPureCalls.size()];
    auto canonicalChild = [&](const B3::Value* child) {
        return resolved(child);
    };
    if (known && B3::isSamePureCall(b3ValueOf(known)->as<B3::PatchpointValue>(), patchpoint, canonicalChild) && node(known).form == Form::Plain && dominates(node(known).block, node(id).block)) {
        for (const B3::Value* child : patchpoint->children())
            dropRawUse(resolved(child));
        makeAlias(node(id), known, false);
        return true;
    }
    known = id;
    return false;
}

void LinearScanCodeGenerator::placeStrayUpsilons(NodeID phiID, uint32_t order)
{
    constexpr uint32_t nowhere = UINT32_MAX;
    const Block& merge = m_blocks[order];
    Vector<std::pair<NodeID, uint32_t>, 4> placed;
    bool areAllPlaced = true;
    auto sendsToPhi = [&](const Block& block) {
        for (uint32_t record = block.firstUpsilon; record; record = m_upsilons[record - 1].next) {
            const Node& upsilon = node(m_upsilons[record - 1].upsilon);
            if (upsilon.b == phiID && upsilon.form == Form::Plain)
                return true;
        }
        return false;
    };
    for (NodeID stray : m_strayUpsilons) {
        if (node(stray).b != phiID)
            continue;
        uint32_t from = node(stray).block;
        uint32_t home = nowhere;
        for (unsigned index = 0; index < merge.numberOfPredecessors && areAllPlaced; ++index) {
            uint32_t sender = m_predecessors[merge.firstPredecessor + index];
            const Block& predecessor = m_blocks[sender];
            if (!predecessor.isReachable || (predecessor.onlySuccessor && predecessor.firstSuccessor + predecessor.onlySuccessor - 1 != m_edgeOfPredecessor[merge.firstPredecessor + index]))
                continue;
            if (!dominates(from, sender))
                continue;
            bool isOverwritten = false;
            for (uint32_t between = sender; !isOverwritten; between = m_blocks[between].dominator) {
                isOverwritten = sendsToPhi(m_blocks[between]);
                if (between == from)
                    break;
            }
            if (isOverwritten)
                continue;
            areAllPlaced &= home == nowhere;
            home = sender;
        }
        areAllPlaced &= home != nowhere && !placed.containsIf([&](const auto& other) { return other.second == home; });
        placed.append({ stray, home });
    }
    if (placed.isEmpty())
        return;
    for (auto [stray, home] : placed) {
        Node& upsilon = node(stray);
        if (areAllPlaced) {
            upsilon.block = home;
            upsilon.next = m_blocks[home].firstUpsilonMovedIn;
            m_blocks[home].firstUpsilonMovedIn = stray;
        }
        Block& block = m_blocks[upsilon.block];
        m_upsilons.append(UpsilonRecord { stray, block.firstUpsilon });
        block.firstUpsilon = m_upsilons.size();
    }
    if (!areAllPlaced)
        node(phiID).flags |= ArrivesInSlot;
    m_strayUpsilons.removeAllMatching([&](NodeID stray) {
        return node(stray).b == phiID;
    });
}

void LinearScanCodeGenerator::importIntegerBinary(Node& n)
{
    bool isWide = n.isWide();
    B3::Opcode opcode = n.opcode;
    NodeID left = n.a;
    NodeID right = n.b;
    const Node& leftNode = node(left);
    const Node& rightNode = node(right);
    bool leftIsConstant = leftNode.form == Form::Constant;
    bool rightIsConstant = rightNode.form == Form::Constant;
    if (leftIsConstant && rightIsConstant) {
        if (std::optional<int64_t> folded = foldBinary(opcode, isWide, leftNode.constant, rightNode.constant)) {
            makeConstant(n, *folded);
            return;
        }
    }
    if (left == right && !leftIsConstant) {
        switch (opcode) {
        case B3::Sub:
        case B3::BitXor:
            dropRawUse(left);
            dropRawUse(left);
            makeConstant(n, 0);
            return;
        case B3::BitAnd:
        case B3::BitOr:
            dropRawUse(left);
            makeAlias(n, left);
            return;
        default:
            break;
        }
    }
    auto isIdentity = [&](int64_t constant) {
        switch (opcode) {
        case B3::Add:
        case B3::Sub:
        case B3::BitOr:
        case B3::BitXor:
        case B3::Shl:
        case B3::SShr:
        case B3::ZShr:
            return !constant;
        case B3::Mul:
            return constant == 1;
        case B3::BitAnd:
            return constant == -1;
        default:
            return false;
        }
    };
    auto standsFor = [&](const Node& operand) {
        return operand.type == n.type || (isWide && operand.type == NodeType::Int32 && operand.form == Form::Plain && operand.has(IsZeroExtended) && operand.has(IsReadWide));
    };
    if (rightIsConstant && isIdentity(rightNode.constant) && standsFor(leftNode)) {
        makeAlias(n, left);
        return;
    }
    if (leftIsConstant && isCommutative(opcode) && isIdentity(leftNode.constant) && standsFor(rightNode)) {
        makeAlias(n, right);
        return;
    }
    if (opcode == B3::Add && isWide && (leftIsConstant || rightIsConstant)) {
        const Node& base = rightIsConstant ? leftNode : rightNode;
        int64_t constant = rightIsConstant ? rightNode.constant : leftNode.constant;
        bool isSumOffFixedRegister = base.form == Form::Rematerialized && base.opcode == B3::Add;
        if (base.form == Form::FixedRegister || isSumOffFixedRegister) {
            int64_t offset = constant + (isSumOffFixedRegister ? base.constant : 0);
            if (fitsInt32(offset)) {
                n.form = Form::Rematerialized;
                n.constant = offset;
                n.base = base.base;
                return;
            }
        }
    }
    if (!isWide)
        n.flags |= IsZeroExtended;
}

bool LinearScanCodeGenerator::import(B3::Value* value, NodeID id, uint32_t order, uint32_t& epoch, uint32_t& memoryEpoch)
{
    Node& n = node(id);
    B3::Opcode opcode = value->opcode();
    bool isDouble = n.isDouble();
    auto zeroExtendsNarrowResult = [&] {
        if (!n.isWide())
            n.flags |= IsZeroExtended;
    };

    switch (opcode) {
    case B3::Nop:
        n.form = Form::Dead;
        return true;
    case B3::Identity:
    case B3::Opaque:
        makeAlias(n, n.a);
        return true;
    case B3::Const32:
        makeConstant(n, value->asInt32());
        return true;
    case B3::Const64:
        makeConstant(n, value->asInt64());
        return true;
    case B3::ConstDouble:
        makeConstant(n, std::bit_cast<int64_t>(value->asDouble()));
        return true;
    case B3::Set:
    case B3::Get:
        return true;
    case B3::SlotBase:
        n.form = Form::Rematerialized;
        return true;
    case B3::Relocatable: {
        auto space = static_cast<RelocationSpace>(value->as<B3::RelocatableValue>()->symbol().space);
        if (space != RelocationSpace::SiteCaches && space != RelocationSpace::Constants && space != RelocationSpace::JumpTables)
            return refuse();
        n.form = Form::Rematerialized;
        return true;
    }
    case B3::FramePointer:
        n.form = Form::FixedRegister;
        n.base = encode(Reg(MacroAssembler::framePointerRegister));
        return true;
    case B3::ArgumentReg: {
        Reg reg = value->as<B3::ArgumentRegValue>()->argumentReg();
        if (m_code.isPinned(reg)) {
            n.form = Form::FixedRegister;
            n.base = encode(reg);
            return true;
        }
        if (order)
            return refuse();
        if (!((m_generalRegisters | m_doubleRegisters) & bitOf(reg)))
            return refuse();
        if (NodeID first = m_argumentNodes[reg.index()]) {
            makeAlias(n, first, false);
            return true;
        }
        m_argumentNodes[reg.index()] = id;
        n.base = encode(reg);
        return true;
    }
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
        if (!isDouble)
            importIntegerBinary(n);
        return true;
    case B3::Div:
        if (isDouble)
            return true;
        [[fallthrough]];
    case B3::Mod:
    case B3::UDiv:
    case B3::UMod:
    case B3::UMulHigh:
    case B3::MulHigh:
        if (isDouble)
            return refuse();
        zeroExtendsNarrowResult();
        return true;
    case B3::FMax:
    case B3::FMin:
    case B3::Ceil:
    case B3::Floor:
    case B3::FTrunc:
    case B3::Sqrt:
    case B3::Abs:
        return isDouble ? true : refuse();
    case B3::PopCount:
    case B3::Clz:
        zeroExtendsNarrowResult();
        return true;
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        if (isDouble)
            return refuse();
        if (opcode == B3::BitXor && isConstant(n.b) && node(n.b).constant == 1) {
            const Node& negated = node(n.a);
            std::optional<B3::Opcode> inverted = negated.form == Form::Plain && negated.type == n.type ? invertedComparison(negated.opcode, negated.operandType == NodeType::Double) : std::nullopt;
            if (inverted) {
                NodeID comparison = n.a;
                n.opcode = *inverted;
                n.operandType = negated.operandType;
                n.a = negated.a;
                n.b = negated.b;
                n.flags |= IsZeroExtended;
                for (NodeID operand : { n.a, n.b }) {
                    uint16_t& rawUses = node(operand).rawUses;
                    if (rawUses != UINT16_MAX)
                        ++rawUses;
                }
                releaseRawUse(comparison);
                return true;
            }
        }
        importIntegerBinary(n);
        return true;
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
        importIntegerBinary(n);
        return true;
    case B3::Neg:
        if (isDouble)
            return isX86_64() && !pinnedConstantFor(std::numeric_limits<int64_t>::min()) ? refuse() : true;
        zeroExtendsNarrowResult();
        return true;
    case B3::BitwiseCast:
        if (n.operandType != NodeType::Int64 && n.operandType != NodeType::Double)
            return refuse();
        return true;
    case B3::IToD: {
        int64_t constant;
        if (constantOf(n.a, constant))
            makeConstant(n, std::bit_cast<int64_t>(static_cast<double>(n.operandType == NodeType::Int64 ? constant : static_cast<int64_t>(static_cast<int32_t>(constant)))));
        return true;
    }
    case B3::SExt8:
    case B3::SExt16:
        n.flags |= IsZeroExtended;
        return true;
    case B3::SExt32: {
        int64_t constant;
        if (constantOf(n.a, constant))
            makeConstant(n, static_cast<int32_t>(constant));
        return true;
    }
    case B3::ZExt32: {
        Node& child = node(n.a);
        if (child.form == Form::Constant)
            makeConstant(n, static_cast<int64_t>(static_cast<uint32_t>(child.constant)));
        else if (child.form == Form::Plain && child.has(IsZeroExtended)) {
            child.flags |= IsReadWide;
            makeAlias(n, n.a);
        }
        return true;
    }
    case B3::Trunc: {
        const Node& child = node(n.a);
        if (child.type != NodeType::Int64 && child.type != NodeType::Int32)
            return refuse();
        if (child.form == Form::Constant)
            makeConstant(n, static_cast<int32_t>(child.constant));
        else if (child.form == Form::Plain)
            makeAlias(n, n.a);
        return true;
    }
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual: {
        n.flags |= IsZeroExtended;
        int64_t left;
        int64_t right;
        if (n.operandType != NodeType::Double && constantOf(n.a, left) && constantOf(n.b, right))
            makeConstant(n, *foldComparison(opcode, n.operandType == NodeType::Int64, left, right));
        return true;
    }
    case B3::EqualOrUnordered:
        if (n.operandType != NodeType::Double)
            return refuse();
        n.flags |= IsZeroExtended;
        return true;
    case B3::Select: {
        std::optional<bool> truth;
        int64_t constant;
        if (constantOf(n.a, constant))
            truth = n.operandType == NodeType::Int32 ? !!static_cast<int32_t>(constant) : !!constant;
        else if (node(n.a).type == n.operandType)
            truth = knownTruthOf(order, n.a);
        if (truth) {
            releaseRawUse(n.a);
            releaseRawUse(*truth ? n.c : n.b);
            makeAlias(n, *truth ? n.b : n.c);
            return true;
        }
        auto hasNoHighBits = [&](NodeID source) {
            return node(source).form == Form::Constant ? node(source).constant >= 0 : node(source).form == Form::Plain && node(source).has(IsZeroExtended);
        };
        if (n.type == NodeType::Int32 && hasNoHighBits(n.b) && hasNoHighBits(n.c))
            n.flags |= IsZeroExtended;
        return true;
    }
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
        if (value->as<B3::MemoryValue>()->hasFence())
            return refuse();
        n.constant = value->as<B3::MemoryValue>()->offset();
        if (n.type == NodeType::Int32)
            n.flags |= IsZeroExtended;
        m_hasLoadsThatOnlyTrap |= n.has(Traps);
        tryReuseLoad(value, id, memoryEpoch);
        return true;
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
        if (value->as<B3::MemoryValue>()->hasFence())
            return refuse();
        if (n.operandType != NodeType::Int32 && n.operandType != NodeType::Int64 && n.operandType != NodeType::Double)
            return refuse();
        n.constant = value->as<B3::MemoryValue>()->offset();
        memoryEpoch = ++m_lastMemoryEpoch;
        return true;
    case B3::Fence:
        memoryEpoch = ++m_lastMemoryEpoch;
        return true;
    case B3::Patchpoint: {
        B3::PatchpointValue* patchpoint = value->as<B3::PatchpointValue>();
        n.a = n.b = n.c = 0;
        if (patchpoint->pureFunction && tryReusePureCall(patchpoint, id))
            return true;
        if (patchpoint->effects.terminal)
            n.flags |= IsTerminal;
        if (patchpoint->effects.writes || patchpoint->effects.fence || patchpoint->effects.writesPinned)
            memoryEpoch = ++m_lastMemoryEpoch;
        uint64_t clobbered = bitsOf(patchpoint->earlyClobbered()) | bitsOf(patchpoint->lateClobbered());
        for (unsigned index = 0; index < patchpoint->numberOfSuccessorClobbers(); ++index)
            clobbered |= bitsOf(patchpoint->lateClobberedOnSuccessor(index));
        m_clobberedRegisters |= clobbered;
        if (std::popcount(m_generalRegisters & ~clobbered) <= 2)
            epoch = ++m_lastEpoch;
        for (const B3::ValueRep& constraint : patchpoint->resultConstraints) {
            switch (constraint.kind()) {
            case B3::ValueRep::WarmAny:
            case B3::ValueRep::SomeRegister:
            case B3::ValueRep::SomeEarlyRegister:
                break;
            case B3::ValueRep::Register:
                m_clobberedRegisters |= bitOf(constraint.reg());
                break;
            default:
                return refuse();
            }
        }
        return true;
    }
    case B3::Extract: {
        Node& tuple = node(idOf(value->child(0)));
        if (tuple.opcode != B3::Patchpoint || tuple.form != Form::Plain)
            return refuse();
        n.a = idOf(value->child(0));
        n.block = tuple.block;
        n.constant = value->as<B3::ExtractValue>()->index();
        if (tuple.has(IsTerminal))
            n.flags |= IsTerminal;
        for (NodeID other = tuple.c; other; other = node(other).next) {
            if (node(other).constant == n.constant) {
                makeAlias(n, other, false);
                return true;
            }
        }
        n.next = tuple.c;
        tuple.c = id;
        return true;
    }
    case B3::Check:
        n.b = n.c = 0;
        return true;
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
        n.c = 0;
        zeroExtendsNarrowResult();
        return true;
    case B3::Upsilon: {
        n.b = idOf(value->as<B3::UpsilonValue>()->phi());
        Node& phi = node(n.b);
        if (!phi.has(IsPlaced) || (m_blocks[order].branchTakenFrom && m_blocks[order].branchTakenFrom->at(0) == value->as<B3::UpsilonValue>()->phi())) {
            n.form = Form::Dead;
            releaseRawUse(n.a);
            return true;
        }
        if (phi.form == Form::Alias)
            return true;
        bool isStray = false;
        {
            const Block& source = m_blocks[order];
            bool leadsToPhi = source.returnBlock && m_linearOf[source.returnBlock->index()] == phi.block;
            for (unsigned index = 0; index < source.numberOfSuccessors; ++index)
                leadsToPhi |= successorOf(source, index) == phi.block;
            if (!leadsToPhi) {
                const B3::BasicBlock* merge = m_blocks[phi.block].block;
                if (m_hasMergesOnlyBranchedOn && m_mergesOnlyBranchedOn.get(merge->index()) && node(n.a).form == Form::Constant && jumpedStraightTo(source.block, merge)) {
                    n.form = Form::Dead;
                    return true;
                }
                const Block& home = m_blocks[phi.block];
                if (home.reversePostOrderIndex > source.reversePostOrderIndex && !home.hasRetreatingPredecessor && !phi.has(ArrivesInSlot))
                    isStray = true;
                else
                    phi.flags |= ArrivesInSlot;
            }
        }
        if (!phi.c)
            phi.a = n.a;
        else if (phi.a != n.a)
            phi.flags |= HasSeveralSources;
        ++phi.c;
        if (node(n.a).form != Form::Constant && !node(n.a).has(IsZeroExtended))
            phi.flags |= HasSourceWithHighBits;
        if (isStray) {
            m_strayUpsilons.append(id);
            return true;
        }
        Block& block = m_blocks[order];
        for (uint32_t record = block.firstUpsilon; record; record = m_upsilons[record - 1].next) {
            Node& earlier = node(m_upsilons[record - 1].upsilon);
            if (earlier.b == n.b && earlier.form == Form::Plain) {
                earlier.form = Form::Dead;
                releaseRawUse(earlier.a);
            }
        }
        m_upsilons.append(UpsilonRecord { id, block.firstUpsilon });
        block.firstUpsilon = m_upsilons.size();
        return true;
    }
    case B3::Phi: {
        Block& block = m_blocks[order];
        if (!m_strayUpsilons.isEmpty())
            placeStrayUpsilons(id, order);
        uint32_t numberOfUpsilons = std::exchange(n.c, 0);
        NodeID source = std::exchange(n.a, 0);
        bool sourceComesFirst = node(source).form != Form::Plain || node(source).block <= order;
        if (numberOfUpsilons && sourceComesFirst && !block.hasRetreatingPredecessor && !n.has(HasSeveralSources) && node(source).type == n.type) {
            makeAlias(n, source, false);
            uint16_t& rawUses = node(source).rawUses;
            if (rawUses != UINT16_MAX)
                rawUses -= std::min<uint32_t>(rawUses, numberOfUpsilons);
            return true;
        }
        if (numberOfUpsilons && !block.hasRetreatingPredecessor && n.type == NodeType::Int32 && !n.has(HasSourceWithHighBits))
            n.flags |= IsZeroExtended;
        n.next = block.firstPhi;
        block.firstPhi = id;
        return true;
    }
    case B3::Jump: {
        if (const B3::BasicBlock* merge = m_blocks[order].branchTakenFrom) {
            n.opcode = B3::Branch;
            n.a = resolved(valueSentTo(merge->at(0), m_blocks[order].block));
            n.operandType = merge->at(0)->type() == B3::Int64 ? NodeType::Int64 : NodeType::Int32;
            uint16_t& rawUses = node(n.a).rawUses;
            if (rawUses != UINT16_MAX)
                ++rawUses;
            return true;
        }
        B3::BasicBlock* returnBlock = m_blocks[order].returnBlock;
        if (!returnBlock)
            return true;
        n.opcode = B3::Return;
        B3::Value* returned = returnBlock->last();
        if (!returned->numChildren())
            return true;
        std::optional<NodeType> type = nodeTypeOf(returned->child(0)->type());
        if (!type)
            return refuse();
        n.operandType = *type;
        if (returnBlock->size() == 1) {
            n.a = resolved(returned->child(0));
            uint16_t& rawUses = node(n.a).rawUses;
            if (rawUses != UINT16_MAX)
                ++rawUses;
            return true;
        }
        NodeID phi = idOf(returned->child(0));
        for (uint32_t record = m_blocks[order].firstUpsilon; record; record = m_upsilons[record - 1].next) {
            Node& upsilon = node(m_upsilons[record - 1].upsilon);
            if (upsilon.b == phi && upsilon.form == Form::Plain) {
                upsilon.form = Form::Dead;
                n.a = upsilon.a;
                return true;
            }
        }
        return refuse();
    }
    case B3::Branch:
    case B3::Switch:
    case B3::Return:
    case B3::Oops:
        return true;
    default:
        return refuse();
    }
}

std::optional<CaseTest> LinearScanCodeGenerator::caseTestOf(const Node& branch, Vector<NodeID, 4>& chain) const
{
    NodeID condition = branch.a;
    bool isNegated = false;
    if (node(condition).type != branch.operandType)
        return std::nullopt;
    for (;;) {
        const Node& n = node(condition);
        if (n.form != Form::Plain)
            return std::nullopt;
        if (n.opcode == B3::BitXor && isConstant(n.b) && node(n.b).constant == 1) {
            chain.append(condition);
            isNegated = !isNegated;
            condition = n.a;
            continue;
        }
        if ((n.opcode != B3::Equal && n.opcode != B3::NotEqual) || n.operandType == NodeType::Double)
            return std::nullopt;
        int64_t constant;
        NodeID subject = n.a;
        if (!constantOf(n.b, constant)) {
            if (!constantOf(n.a, constant))
                return std::nullopt;
            subject = n.b;
        }
        if (node(subject).form != Form::Plain)
            return std::nullopt;
        chain.append(condition);
        if (n.operandType == NodeType::Int32)
            constant = static_cast<int32_t>(constant);
        return CaseTest { subject, constant, n.operandType, (n.opcode == B3::Equal) != isNegated };
    }
}

bool LinearScanCodeGenerator::holdsOnlyItsTest(const Block& block, const Vector<NodeID, 4>& chain) const
{
    for (B3::Value* value : *block.block) {
        NodeID id = idOf(value);
        const Node& n = node(id);
        if (n.form != Form::Plain || value == block.block->last())
            continue;
        if (!isPureOperation(n.opcode))
            return false;
        if (n.rawUses > (chain.contains(id) ? 1 : 0))
            return false;
    }
    return true;
}

void LinearScanCodeGenerator::takeOverChain(uint32_t order, std::span<const LinkOfChain> links, uint32_t otherwise, Vector<uint32_t, 16>& successorOfLink)
{
    constexpr uint32_t dropped = UINT32_MAX;
    Vector<uint32_t, 16> targets;
    Vector<B3::FrequencyClass, 16> classes;
    uint32_t firstSuccessor = m_successors.size();
    auto entryOf = [&](Block& target, uint32_t edge) {
        for (unsigned index = 0; index < target.numberOfPredecessors; ++index) {
            if (m_edgeOfPredecessor[target.firstPredecessor + index] == edge)
                return index;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto dropEntry = [&](Block& target, unsigned index) {
        unsigned last = --target.numberOfPredecessors;
        m_predecessors[target.firstPredecessor + index] = m_predecessors[target.firstPredecessor + last];
        m_edgeOfPredecessor[target.firstPredecessor + index] = m_edgeOfPredecessor[target.firstPredecessor + last];
    };
    successorOfLink.shrink(0);
    for (const LinkOfChain& link : links) {
        uint32_t edge = m_blocks[link.block].firstSuccessor + link.leavingIndex;
        uint32_t targetOrder = m_successors[edge];
        Block& target = m_blocks[targetOrder];
        unsigned entry = entryOf(target, edge);
        if (targetOrder == otherwise) {
            dropEntry(target, entry);
            successorOfLink.append(dropped);
            continue;
        }
        size_t position = targets.find(targetOrder);
        if (position != notFound) {
            dropEntry(target, entry);
            classes[position] = B3::maxFrequency(classes[position], m_edgeClasses[edge]);
        } else {
            position = targets.size();
            targets.append(targetOrder);
            classes.append(m_edgeClasses[edge]);
            m_predecessors[target.firstPredecessor + entry] = order;
            m_edgeOfPredecessor[target.firstPredecessor + entry] = firstSuccessor + position;
        }
        successorOfLink.append(static_cast<uint32_t>(position));
    }
    {
        const LinkOfChain& last = links.back();
        uint32_t edge = m_blocks[last.block].firstSuccessor + (1 - last.leavingIndex);
        Block& target = m_blocks[otherwise];
        unsigned entry = entryOf(target, edge);
        m_predecessors[target.firstPredecessor + entry] = order;
        m_edgeOfPredecessor[target.firstPredecessor + entry] = firstSuccessor + targets.size();
        targets.append(otherwise);
        classes.append(m_edgeClasses[edge]);
    }
    for (unsigned index = 0; index < targets.size(); ++index) {
        m_successors.append(targets[index]);
        m_edgeClasses.append(classes[index]);
        Block& target = m_blocks[targets[index]];
        target.predecessor = m_predecessors[target.firstPredecessor];
        target.incomingEdge = m_edgeOfPredecessor[target.firstPredecessor];
    }
    for (unsigned index = 0; index < links.size(); ++index) {
        Block& block = m_blocks[links[index].block];
        releaseRawUse(node(idOf(block.block->last())).a);
        if (!index)
            continue;
        block.isReachable = false;
        for (B3::Value* value : *block.block)
            node(idOf(value)).form = Form::Dead;
    }
    Block& head = m_blocks[order];
    head.firstSuccessor = firstSuccessor;
    head.numberOfSuccessors = targets.size();
}

void LinearScanCodeGenerator::inferSwitches()
{
    constexpr unsigned minCases = 4;
    Vector<LinkOfChain, 16> links;
    Vector<int64_t, 16> constants;
    Vector<NodeID, 4> chain;
    Vector<uint32_t, 16> successorOfLink;
    for (uint32_t order : m_reversePostOrder) {
        if (!m_blocks[order].isReachable)
            continue;
        links.shrink(0);
        constants.shrink(0);
        NodeID subject = 0;
        NodeType type = NodeType::Void;
        uint32_t current = order;
        for (;;) {
            const Block& block = m_blocks[current];
            const Node& branch = node(idOf(block.block->last()));
            if (branch.opcode != B3::Branch || block.onlySuccessor || block.numberOfSuccessors != 2)
                break;
            chain.shrink(0);
            std::optional<CaseTest> test = caseTestOf(branch, chain);
            if (!test || (subject && (test->subject != subject || test->type != type)))
                break;
            if (current != order && (block.numberOfLivePredecessors != 1 || block.region != m_blocks[order].region || !holdsOnlyItsTest(block, chain)))
                break;
            if (constants.contains(test->constant))
                break;
            subject = test->subject;
            type = test->type;
            links.append(LinkOfChain { current, test->equalIsTaken ? 0u : 1u });
            constants.append(test->constant);
            current = successorOf(block, test->equalIsTaken ? 1 : 0);
        }
        if (links.size() < minCases)
            continue;

        uint32_t firstCase = m_casesOfSwitches.size();
        takeOverChain(order, links.span(), current, successorOfLink);
        for (unsigned index = 0; index < links.size(); ++index) {
            if (successorOfLink[index] != UINT32_MAX)
                m_casesOfSwitches.append(CaseOfSwitch { constants[index], successorOfLink[index] });
        }
        Node& terminal = node(idOf(m_blocks[order].block->last()));
        terminal.opcode = B3::Switch;
        terminal.a = subject;
        terminal.operandType = type;
        m_inferredSwitches.append(InferredSwitch { firstCase, static_cast<uint32_t>(m_casesOfSwitches.size() - firstCase) });
        terminal.constant = m_inferredSwitches.size();
        uint16_t& rawUses = node(subject).rawUses;
        if (rawUses != UINT16_MAX)
            ++rawUses;
    }
}

std::optional<std::pair<NodeID, LeavingTest>> LinearScanCodeGenerator::leavingTestOf(const Block& block, unsigned leavingIndex, Vector<NodeID, 4>& chain) const
{
    const Node& branch = node(idOf(block.block->last()));
    NodeID condition = branch.a;
    const Node& n = node(condition);
    if (n.type != branch.operandType || n.form != Form::Plain || n.isDouble())
        return std::nullopt;
    bool leavesWhenTaken = !leavingIndex;
    auto fits = [](int64_t constant) {
        return fitsInt32(constant);
    };
    int64_t constant;
    if (isComparison(n.opcode)) {
        if (n.operandType == NodeType::Double || !constantOf(n.b, constant) || !fits(constant) || node(n.a).form != Form::Plain)
            return std::nullopt;
        chain.append(condition);
        const Node& left = node(n.a);
        int64_t mask;
        if ((n.opcode == B3::Equal || n.opcode == B3::NotEqual) && !constant && left.opcode == B3::BitAnd && left.type == n.operandType && constantOf(left.b, mask) && fits(mask) && node(left.a).form == Form::Plain && (n.opcode == B3::NotEqual) == leavesWhenTaken) {
            chain.append(n.a);
            return std::pair { left.a, LeavingTest { B3::BitAnd, true, n.operandType, mask, 0 } };
        }
        return std::pair { n.a, LeavingTest { n.opcode, leavesWhenTaken, n.operandType, constant, 0 } };
    }
    if (n.opcode == B3::BitAnd && leavesWhenTaken && constantOf(n.b, constant) && fits(constant) && node(n.a).form == Form::Plain) {
        chain.append(condition);
        return std::pair { n.a, LeavingTest { B3::BitAnd, true, n.type, constant, 0 } };
    }
    return std::pair { condition, LeavingTest { B3::NotEqual, leavesWhenTaken, n.type, 0, 0 } };
}

void LinearScanCodeGenerator::inferLeavingTests()
{
    Vector<LinkOfChain, 16> links;
    Vector<LeavingTest, 16> tests;
    Vector<NodeID, 4> chain;
    Vector<uint32_t, 16> successorOfLink;
    auto isLikely = [&](const Block& block, unsigned index) {
        return m_edgeClasses[block.firstSuccessor + index] == B3::FrequencyClass::Normal;
    };
    auto isTwoWayBranch = [&](const Block& block) {
        return node(idOf(block.block->last())).opcode == B3::Branch && !block.onlySuccessor && block.numberOfSuccessors == 2 && successorOf(block, 0) != successorOf(block, 1);
    };
    auto onlyLeavingValueOf = [](const LeavingTest& test) -> std::optional<int64_t> {
        if (test.relation != (test.leavesWhenItHolds ? B3::Equal : B3::NotEqual))
            return std::nullopt;
        return test.constant;
    };
    auto leavesFor = [](const LeavingTest& test, int64_t value) {
        return *foldComparison(test.relation, test.type == NodeType::Int64, value, test.constant) == test.leavesWhenItHolds;
    };
    for (uint32_t order : m_reversePostOrder) {
        const Block& head = m_blocks[order];
        if (!head.isReachable || !isTwoWayBranch(head) || isLikely(head, 0) == isLikely(head, 1))
            continue;
        unsigned leavingIndex = isLikely(head, 0) ? 1 : 0;
        chain.shrink(0);
        auto first = leavingTestOf(head, leavingIndex, chain);
        if (!first)
            continue;
        NodeID subject = first->first;
        bool testsBits = first->second.relation == B3::BitAnd;
        if (!testsBits && !onlyLeavingValueOf(first->second))
            continue;
        links.shrink(0);
        tests.shrink(0);
        links.append(LinkOfChain { order, leavingIndex });
        tests.append(first->second);
        uint32_t current = successorOf(head, 1 - leavingIndex);
        bool isClosed = false;
        while (!isClosed) {
            const Block& block = m_blocks[current];
            if (block.numberOfLivePredecessors != 1 || block.region != head.region || !isTwoWayBranch(block))
                break;
            bool isTaken = false;
            for (unsigned leaving = 0; leaving < 2 && !isTaken; ++leaving) {
                if (!isLikely(block, 1 - leaving))
                    continue;
                chain.shrink(0);
                auto next = leavingTestOf(block, leaving, chain);
                if (!next || next->first != subject || next->second.type != tests[0].type || (next->second.relation == B3::BitAnd) != testsBits || !holdsOnlyItsTest(block, chain))
                    continue;
                if (!testsBits) {
                    bool isImplied = true;
                    for (const LeavingTest& earlier : tests)
                        isImplied &= leavesFor(next->second, *onlyLeavingValueOf(earlier));
                    if (!isImplied)
                        continue;
                    isClosed = !onlyLeavingValueOf(next->second);
                }
                links.append(LinkOfChain { current, leaving });
                tests.append(next->second);
                current = successorOf(block, 1 - leaving);
                isTaken = true;
            }
            if (!isTaken)
                break;
        }
        if (links.size() < 2 || (!testsBits && !isClosed))
            continue;
        bool leavesForContinuation = false;
        for (const LinkOfChain& link : links)
            leavesForContinuation |= successorOf(m_blocks[link.block], link.leavingIndex) == current;
        if (leavesForContinuation)
            continue;

        takeOverChain(order, links.span(), current, successorOfLink);
        uint32_t firstTest = m_leavingTests.size();
        for (unsigned index = 0; index < tests.size(); ++index) {
            tests[index].successor = successorOfLink[index];
            m_leavingTests.append(tests[index]);
        }
        Node& terminal = node(idOf(m_blocks[order].block->last()));
        terminal.opcode = B3::EntrySwitch;
        terminal.a = subject;
        terminal.operandType = tests[0].type;
        m_inferredSwitches.append(InferredSwitch { firstTest, static_cast<uint32_t>(tests.size()) });
        terminal.constant = m_inferredSwitches.size();
        uint16_t& rawUses = node(subject).rawUses;
        if (rawUses != UINT16_MAX)
            ++rawUses;
    }
}

std::optional<int64_t> LinearScanCodeGenerator::valueIfPhiIs(NodeID value, NodeID phi, int64_t arriving, unsigned depth) const
{
    constexpr unsigned maxDepth = 4;
    if (value == phi)
        return arriving;
    const Node& n = node(value);
    if (n.form == Form::Constant)
        return n.constant;
    if (depth == maxDepth || n.form != Form::Plain || n.block != node(phi).block || !n.a || !n.b || n.c || n.operandType == NodeType::Double || n.isDouble())
        return std::nullopt;
    std::optional<int64_t> left = valueIfPhiIs(n.a, phi, arriving, depth + 1);
    std::optional<int64_t> right = left ? valueIfPhiIs(n.b, phi, arriving, depth + 1) : std::nullopt;
    if (!right)
        return std::nullopt;
    if (std::optional<bool> truth = foldComparison(n.opcode, n.operandType == NodeType::Int64, *left, *right))
        return *truth;
    return foldBinary(n.opcode, n.isWide(), *left, *right);
}

void LinearScanCodeGenerator::findEdgesThatDecideTests()
{
    for (uint32_t order = 1; order < m_blocks.size(); ++order) {
        Block& merge = m_blocks[order];
        if (!merge.isReachable || merge.hasRetreatingPredecessor || merge.onlySuccessor || merge.numberOfSuccessors != 2 || !merge.firstPhi || merge.numberOfPredecessors < 2)
            continue;
        const Node& branch = node(idOf(merge.block->last()));
        if (branch.opcode != B3::Branch || node(branch.a).type != branch.operandType)
            continue;
        NodeID tested = 0;
        for (NodeID phi = merge.firstPhi; phi && !tested; phi = node(phi).next) {
            const Node& candidate = node(phi);
            if (candidate.form == Form::Plain && candidate.rawUses == 1 && !candidate.isDouble() && !candidate.has(ArrivesInSlot) && valueIfPhiIs(branch.a, phi, 0))
                tested = phi;
        }
        if (!tested)
            continue;
        bool holdsOnlyTheTest = true;
        for (B3::Value* value : *merge.block) {
            const Node& n = node(idOf(value));
            if (n.form != Form::Plain || value == merge.block->last())
                continue;
            if (n.opcode == B3::Phi) {
                holdsOnlyTheTest &= !n.has(ArrivesInSlot);
                continue;
            }
            holdsOnlyTheTest &= isPureOperation(n.opcode) && n.rawUses == 1 && valueIfPhiIs(idOf(value), tested, 0);
        }
        if (!holdsOnlyTheTest)
            continue;
        for (unsigned index = 0; index < merge.numberOfPredecessors; ++index) {
            const Block& sender = m_blocks[m_predecessors[merge.firstPredecessor + index]];
            uint32_t edge = m_edgeOfPredecessor[merge.firstPredecessor + index];
            if (!sender.isReachable || !(sender.numberOfSuccessors == 1 || (sender.onlySuccessor && sender.firstSuccessor + sender.onlySuccessor - 1 == edge)))
                continue;
            for (uint32_t record = sender.firstUpsilon; record; record = m_upsilons[record - 1].next) {
                const Node& upsilon = node(m_upsilons[record - 1].upsilon);
                if (upsilon.b != tested || upsilon.form != Form::Plain)
                    continue;
                if (node(upsilon.a).form != Form::Constant)
                    break;
                int64_t value = *valueIfPhiIs(branch.a, tested, node(upsilon.a).constant);
                bool truth = branch.operandType == NodeType::Int64 ? !!value : !!static_cast<int32_t>(value);
                if (m_successorDecidedOnEdge.isEmpty())
                    m_successorDecidedOnEdge.fill(0, m_successors.size());
                m_successorDecidedOnEdge[edge] = truth ? 1 : 2;
                merge.testedPhi = tested;
                break;
            }
        }
    }
}

bool LinearScanCodeGenerator::canRunBeforeTrap(NodeID id) const
{
    const Node& n = node(id);
    if (n.form != Form::Plain || n.opcode == B3::Upsilon || n.opcode == B3::Phi)
        return true;
    if (B3::isLoad(n.opcode))
        return !n.has(Traps) && !n.has(TakesTrap) && !b3ValueOf(id)->effects().controlDependent;
    return isValueNumbered(n);
}

void LinearScanCodeGenerator::foldTrapsIntoAccesses()
{
    int64_t reach = m_code.policy().peepholes.trapReach;
    if (!reach || !m_hasLoadsThatOnlyTrap)
        return;
    auto widthOf = [](const Node& load) -> int64_t {
        switch (load.opcode) {
        case B3::Load8Z:
        case B3::Load8S:
            return 1;
        case B3::Load16Z:
        case B3::Load16S:
            return 2;
        default:
            return load.type == NodeType::Int32 ? 4 : 8;
        }
    };
    for (const Block& block : m_blocks) {
        for (uint32_t slot = block.firstSlot; slot < block.endSlot; ++slot) {
            Node& trap = node(m_sequence[slot]);
            if (trap.form != Form::Plain || !trap.has(Traps) || trap.rawUses || !B3::isLoad(trap.opcode))
                continue;
            AddressPlan reference = planAddress(trap.a, trap.offset(), trap);
            bool isNarrow = reference.index && reference.fixedBase == cellCageBaseGPR && reference.shift == cell32Shift;
            if (!isNarrow && (reference.index || !reference.base || node(reference.base).form != Form::Plain))
                continue;
            uint32_t taker = slot + 1;
            bool isFound = false;
            for (; taker < block.endSlot; ++taker) {
                const Node& access = node(m_sequence[taker]);
                if (access.form == Form::Plain && B3::isLoad(access.opcode) && !access.has(Traps) && !access.has(TakesTrap)) {
                    AddressPlan plan = planAddress(access.a, access.offset(), access);
                    isFound = plan.base == reference.base && plan.index == reference.index && plan.shift == reference.shift && plan.fixedBase == reference.fixedBase && !plan.slot && plan.offset >= 0 && plan.offset + widthOf(access) <= reach;
                    if (isFound)
                        break;
                }
                if (!canRunBeforeTrap(m_sequence[taker]))
                    break;
            }
            if (!isFound)
                continue;
            uint32_t limit = taker + 1;
            while (limit < block.endSlot && canRunBeforeTrap(m_sequence[limit]))
                ++limit;
            Node& access = node(m_sequence[taker]);
            access.flags |= TakesTrap;
            access.c = m_sequence[slot];
            access.next = limit;
            trap.form = Form::Dead;
            releaseRawUse(trap.a);
        }
    }
}

NodeType LinearScanCodeGenerator::typeOfChild(const B3::Value* child) const
{
    return nodeTypeOf(child->type()).value_or(NodeType::Void);
}

bool LinearScanCodeGenerator::importValues()
{
    unsigned numberOfValues = m_procedure.values().size();
    m_nodes.grow(numberOfValues + 1);
    std::memset(static_cast<void*>(m_nodes.mutableSpan().data()), 0, (numberOfValues + 1) * sizeof(Node));
    m_nodes[0].form = Form::Dead;
    m_sequence.reserveInitialCapacity(numberOfValues);
    m_blockOfSlot.reserveInitialCapacity(numberOfValues);
    m_virtualRegisters.append(VirtualRegister());
    m_knownValues.fill(0, std::bit_ceil(std::clamp<size_t>(numberOfValues / 2, 64, 4096)));
    m_knownLoads.fill(KnownLoad(), std::bit_ceil(std::clamp<size_t>(numberOfValues / 8, 64, 1024)));

    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        for (B3::Value* value : *m_blocks[order].block) {
            if (value->opcode() == B3::Phi) {
                Node& phi = node(idOf(value));
                phi.block = order;
                phi.flags |= IsPlaced;
            }
        }
    }
    if (m_hasCycles)
        computeDominatorsOverAllEdges();

    for (uint32_t order : m_reversePostOrder) {
        Block& block = m_blocks[order];
        uint32_t epoch;
        uint32_t memoryEpoch;
        block.isReachable = enterBlock(order, epoch, memoryEpoch);
        if (!block.isReachable) {
            for (B3::Value* value : *block.block)
                node(idOf(value)).form = Form::Dead;
            continue;
        }
        noteTruthKnownAtEntry(order);
        for (B3::Value* value : *block.block) {
            NodeID id = idOf(value);
            Node& n = node(id);
            std::optional<NodeType> type = nodeTypeOf(value->type());
            if (!type || (*type == NodeType::Tuple && value->opcode() != B3::Patchpoint))
                return refuse();
            n.opcode = value->opcode();
            n.type = *type;
            n.block = order;
            n.epoch = epoch;
            n.memoryEpoch = memoryEpoch;
            if (value->traps())
                n.flags |= Traps;
            if (value->isChill())
                n.flags |= IsChill;
            auto children = value->children();
            unsigned numberOfChildren = children.end() - children.begin();
            if (numberOfChildren) {
                std::optional<NodeType> operandType = nodeTypeOf(children.begin()[0]->type());
                if (!operandType)
                    return refuse();
                n.operandType = *operandType;
                for (unsigned index = 0; index < numberOfChildren; ++index) {
                    NodeID child = resolved(children.begin()[index]);
                    uint16_t& rawUses = node(child).rawUses;
                    if (rawUses != UINT16_MAX)
                        ++rawUses;
                    if (!index)
                        n.a = child;
                    else if (index == 1)
                        n.b = child;
                    else if (index == 2)
                        n.c = child;
                }
            }
            if (!import(value, id, order, epoch, memoryEpoch))
                return false;
            if (n.form == Form::Plain && numberOfChildren)
                tryReuseValue(id);
        }
        block.epochAtEnd = epoch;
        block.memoryEpochAtEnd = memoryEpoch;

        const Node& last = node(idOf(block.block->last()));
        if (last.opcode == B3::Branch) {
            std::optional<bool> truth;
            int64_t constant;
            if (constantOf(last.a, constant))
                truth = last.operandType == NodeType::Int32 ? !!static_cast<int32_t>(constant) : !!constant;
            else if (node(last.a).type == last.operandType)
                truth = knownTruthOf(order, last.a);
            if (truth) {
                block.onlySuccessor = *truth ? 1 : 2;
                releaseRawUse(last.a);
            }
        }
    }

    for (uint32_t index = m_reversePostOrder.size(); index--;) {
        const Block& block = m_blocks[m_reversePostOrder[index]];
        if (!block.isReachable)
            continue;
        for (unsigned position = block.block->size(); position--;) {
            Node& n = node(idOf(block.block->at(position)));
            if (n.form != Form::Plain || n.rawUses || !(isValueNumbered(n) || (B3::isLoad(n.opcode) && !n.has(Traps))))
                continue;
            n.form = Form::Dead;
            for (NodeID operand : { n.a, n.b, n.c }) {
                if (operand)
                    dropRawUse(operand);
            }
        }
    }
    m_allUsesAreCounted = true;
    inferSwitches();
    inferLeavingTests();
    findEdgesThatDecideTests();

    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        Block& block = m_blocks[order];
        block.firstSlot = m_sequence.size();
        if (block.isReachable) {
            for (B3::Value* value : *block.block) {
                NodeID id = idOf(value);
                Node& n = node(id);
                if (n.form != Form::Plain || (n.opcode == B3::Upsilon && n.block != order))
                    continue;
                if (value == block.block->last()) {
                    for (NodeID moved = block.firstUpsilonMovedIn; moved; moved = node(moved).next) {
                        if (node(moved).form != Form::Plain)
                            continue;
                        node(moved).slot = m_sequence.size();
                        m_sequence.append(moved);
                        m_blockOfSlot.append(order);
                    }
                }
                n.slot = m_sequence.size();
                m_sequence.append(id);
                m_blockOfSlot.append(order);
            }
        }
        block.endSlot = m_sequence.size();
    }
    foldTrapsIntoAccesses();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
