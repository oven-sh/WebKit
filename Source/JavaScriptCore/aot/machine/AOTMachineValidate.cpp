/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineChecker.h"

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "AOTMachineTarget.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace JSC { namespace AOT { namespace Machine {

namespace {

class MachineValidator {
    WTF_MAKE_NONCOPYABLE(MachineValidator);
public:
    MachineValidator(Procedure& procedure, bool isBehindBlockPasses)
        : m_proc(procedure)
        , m_arena(procedure.arena())
        , m_isBehindBlockPasses(isBehindBlockPasses)
    {
    }

    bool run();
    bool runCheckOfDominanceAlone() { return checkDominance(); }

private:
    static constexpr uint32_t notReached = UINT32_MAX;

    enum class Side : uint8_t { Operand, Result };

    struct Visit {
        const Block* block;
        unsigned nextSuccessor;
    };

    bool fail(const Operation* = nullptr, PlaceOfCheck = PlaceOfCheck::here());
    bool failAtEndOf(const Block*, PlaceOfCheck = PlaceOfCheck::here());

    bool isInProcedure(const Operation*) const;
    bool isInProcedure(const Block*) const;
    static std::span<Operation* const> listHolding(const Block*, Opcode);
    bool listsItsOwnOperations(const Block*, std::span<Operation* const>);

    bool findWhatIsLeft();
    bool isLeft(const Block* block) const { return !m_isBehindBlockPasses || m_blockIsLeft.get(block->index()); }
    ALWAYS_INLINE bool isLeft(const Operation* operation) const { return !m_isBehindBlockPasses || !operation->owner() || m_operationIsLeft.get(operation->index()); }

    bool checkMembership();
    bool checkEdges();
    bool checkTerminators();
    bool checkOperations();
    bool checkUpsilons();
    bool checkDominance();

    static bool terminatorHasExpectedSuccessorCount(const Block*);
    bool hasLegalChildren(const Operation*) const;
    bool hasLegalPosition(const Operation*) const;
    bool hasLegalTypes(const Operation*) const;
    bool hasLegalAddress(const Operation*) const;
    bool hasLegalTupleIndex(Type) const;
    bool isLegalPatch(const PatchOperation*) const;
    static bool isLegalConstraint(const ValueRep&, Type, Side);
    static bool hasLegalFlags(const Operation*);
    static bool hasInstructionOnTarget(const Operation*);

    void computeDominators();
    bool dominates(const Block* dominator, const Block*) const;

    Procedure& m_proc;
    Arena& m_arena;
    const bool m_isBehindBlockPasses;

    BitSpan m_blockIsLeft;
    BitSpan m_operationIsLeft;

    ArenaVector<uint32_t> m_numberOfIncomingEdges;
    BitSpan m_isEntrypoint;
    ArenaVector<uint32_t> m_placeOf;

    ArenaVector<uint32_t> m_preorder;
    ArenaVector<uint32_t> m_sizeOfSubtree;
};

bool MachineValidator::run()
{
    return (!m_isBehindBlockPasses || findWhatIsLeft()) && checkMembership() && checkEdges() && checkTerminators() && checkOperations() && checkUpsilons() && checkDominance();
}

bool MachineValidator::findWhatIsLeft()
{
    m_blockIsLeft = BitSpan::create(m_arena, m_proc.numberOfBlocks());
    m_operationIsLeft = BitSpan::create(m_arena, m_proc.numberOfOperations());
    ArenaVector<const Block*> reached;
    auto reach = [&](const Block* block) {
        if (!isInProcedure(block))
            return false;
        if (!m_blockIsLeft.get(block->index())) {
            m_blockIsLeft.set(block->index());
            reached.append(m_arena, block);
        }
        return true;
    };
    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        if (!reach(entrypoint.block))
            return fail();
    }
    while (!reached.isEmpty()) {
        const Block* block = reached.takeLast();
        for (std::span<Operation* const> list : { block->phis(), block->operations(), block->upsilons() }) {
            for (const Operation* operation : list) {
                if (!isInProcedure(operation))
                    return fail();
                m_operationIsLeft.set(operation->index());
            }
        }
        for (const Edge& edge : block->successors()) {
            if (!reach(edge.block()))
                return fail();
        }
    }
    return true;
}

bool MachineValidator::fail(const Operation* operation, PlaceOfCheck check)
{
    m_proc.fail(FailureKind::MalformedProcedure, operation, check);
    return false;
}

bool MachineValidator::failAtEndOf(const Block* block, PlaceOfCheck check)
{
    return fail(block->isEmpty() ? nullptr : block->last(), check);
}

bool MachineValidator::isInProcedure(const Operation* operation) const
{
    return operation && operation->index() < m_proc.numberOfOperations() && m_proc.operationAt(operation->index()) == operation;
}

bool MachineValidator::isInProcedure(const Block* block) const
{
    return block && block->index() < m_proc.numberOfBlocks() && m_proc.blockAt(block->index()) == block;
}

std::span<Operation* const> MachineValidator::listHolding(const Block* block, Opcode opcode)
{
    if (opcode == Phi)
        return block->phis();
    if (opcode == Upsilon)
        return block->upsilons();
    return block->operations();
}

bool MachineValidator::listsItsOwnOperations(const Block* block, std::span<Operation* const> list)
{
    for (size_t place = 0; place < list.size(); ++place) {
        const Operation* operation = list[place];
        if (!isInProcedure(operation))
            return fail();
        if (operation->owner() != block || operation->indexInBlock() != place || listHolding(block, operation->opcode()).data() != list.data())
            return fail(operation);
    }
    return true;
}

bool MachineValidator::checkMembership()
{
    for (uint32_t index = 0; index < m_proc.numberOfOperations(); ++index) {
        const Operation* operation = m_proc.operationAt(index);
        if (!operation || operation->index() != index)
            return fail();
        if (static_cast<unsigned>(operation->opcode()) >= numberOfOpcodes)
            return fail(operation);
        const Block* owner = operation->owner();
        if (!owner)
            continue;
        if (!isInProcedure(owner))
            return fail(operation);
        std::span<Operation* const> list = listHolding(owner, operation->opcode());
        if (isLeft(operation) && (operation->indexInBlock() >= list.size() || list[operation->indexInBlock()] != operation))
            return fail(operation);
    }

    for (uint32_t index = 0; index < m_proc.numberOfBlocks(); ++index) {
        const Block* block = m_proc.blockAt(index);
        if (!block || block->index() != index)
            return fail();
        if (!isLeft(block))
            continue;
        if (!listsItsOwnOperations(block, block->phis()) || !listsItsOwnOperations(block, block->operations()) || !listsItsOwnOperations(block, block->upsilons()))
            return false;
    }
    return true;
}

bool MachineValidator::checkEdges()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    m_numberOfIncomingEdges.fill(m_arena, numberOfBlocks, 0);
    m_isEntrypoint = BitSpan::create(m_arena, numberOfBlocks);

    if (m_proc.entrypoints().empty())
        return fail();
    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        if (!isInProcedure(entrypoint.block))
            return fail();
        m_isEntrypoint.set(entrypoint.block->index());
    }

    for (const Block* block : m_proc.blocks()) {
        if (!isLeft(block))
            continue;
        for (const Edge& edge : block->successors()) {
            if (!isInProcedure(edge.block()) || m_isEntrypoint.get(edge.block()->index()))
                return failAtEndOf(block);
            ++m_numberOfIncomingEdges[edge.block()->index()];
        }
    }

    for (const Block* block : m_proc.blocks()) {
        if (!m_isBehindBlockPasses && block->numberOfIncomingEdges() != m_numberOfIncomingEdges[block->index()])
            return failAtEndOf(block);
    }

    for (const Block* block : m_proc.blocks()) {
        for (const Edge& edge : block->successors()) {
            if (edge.block()->numberOfIncomingEdges() == 1 && (edge.block()->singlePredecessor() != block || edge.block()->classOfSingleIncomingEdge() != edge.edgeClass()))
                return failAtEndOf(block);
        }
    }

    for (const Block* block : m_proc.blocks()) {
        if (isLeft(block) && block->hasOnlyOnePredecessor() && m_numberOfIncomingEdges[block->index()] != 1)
            return failAtEndOf(block);
    }
    return true;
}

bool MachineValidator::terminatorHasExpectedSuccessorCount(const Block* block)
{
    const Operation* terminator = block->last();
    unsigned numberOfSuccessors = block->numSuccessors();
    if (terminator->opcode() == Patch)
        return true;
    if (terminator->opcode() == Jump)
        return numberOfSuccessors == 1;
    if (terminator->opcode() == Branch)
        return numberOfSuccessors == 2;
    if (terminator->opcode() == Switch)
        return numberOfSuccessors == terminator->switchData().caseValues.size() + 1;
    return !numberOfSuccessors;
}

bool MachineValidator::checkTerminators()
{
    for (const Block* block : m_proc.blocks()) {
        if (!isLeft(block))
            continue;
        std::span<Operation* const> operations = block->operations();
        for (size_t index = 0; index + 1 < operations.size(); ++index) {
            if (operations[index]->endsBlock())
                return fail(operations[index]);
        }
        if (!block->isTerminated()) {
            if (m_numberOfIncomingEdges[block->index()] || m_isEntrypoint.get(block->index()))
                return failAtEndOf(block);
            continue;
        }
        if (!terminatorHasExpectedSuccessorCount(block))
            return failAtEndOf(block);
    }
    return true;
}

bool MachineValidator::hasLegalChildren(const Operation* operation) const
{
    Opcode opcode = operation->opcode();
    unsigned numberOfChildren = operation->numChildren();
    uint8_t numberWanted = descriptionOf(opcode).numberOfChildren;
    if (numberWanted != variableChildren ? numberOfChildren != numberWanted : opcode == Return && numberOfChildren > 1)
        return false;

    for (const Operation* child : operation->children()) {
        if (!isInProcedure(child))
            return false;
        if (opcode == Extract ? !child->type().isTuple() : !child->type().isNumeric())
            return false;
    }
    return true;
}

bool MachineValidator::hasLegalPosition(const Operation* operation) const
{
    Opcode opcode = operation->opcode();
    const Block* owner = operation->owner();
    if (hasNoPosition(opcode))
        return !owner;
    if (opcode == ArgumentReg) {
        Reg reg = operation->argumentReg();
        if (!reg.isGPR() && !reg.isFPR())
            return false;
        return owner ? m_isEntrypoint.get(owner->index()) : isPinned(reg);
    }
    if (owner)
        return true;

    if (opcode != Add)
        return false;
    const Operation* base = operation->child(0);
    bool baseIsRegisterOrFrameAddress = base->opcode() == ArgumentReg || base->opcode() == FramePointer || base->opcode() == SlotBase;
    return baseIsRegisterOrFrameAddress && base->hasNoPosition() && operation->child(1)->hasInt64();
}

bool MachineValidator::hasLegalAddress(const Operation* access) const
{
    const Operation* base = access->lastChild();
    if (base->type() != Int64 || access->heap() >= m_proc.heapTree().size())
        return false;
    if (base->opcode() != SlotBase)
        return true;
    const StackSlot* slot = base->slot();
    int64_t begin = access->offset();
    return slot && begin >= 0 && begin + bytesForWidth(access->accessWidth()) <= slot->byteSize();
}

bool MachineValidator::hasLegalTupleIndex(Type type) const
{
    return !type.isTuple() || type.tupleIndex() < m_proc.numberOfTuples();
}

bool MachineValidator::isLegalConstraint(const ValueRep& constraint, Type type, Side side)
{
    switch (constraint.kind()) {
    case ValueRep::WarmAny:
    case ValueRep::ColdAny:
    case ValueRep::LateColdAny:
    case ValueRep::SomeLateRegister:
        return side == Side::Operand;
    case ValueRep::SomeEarlyRegister:
        return side == Side::Result;
    case ValueRep::SomeRegister:
    case ValueRep::StackArgument:
        return true;
    case ValueRep::Register:
        return type.isFloat() ? constraint.isFPR() : constraint.isGPR();
    case ValueRep::Stack:
    case ValueRep::Constant:
        return false;
    }
    return false;
}

bool MachineValidator::isLegalPatch(const PatchOperation* patch) const
{
    Type type = patch->type();
    unsigned numberOfChildren = patch->numChildren();
    if (patch->opcode() == Check && (type != Void || !numberOfChildren || !patch->child(0)->type().isInt()))
        return false;
    if (patch->failsWhenPredicateIsZero() && (patch->opcode() != Check || !isComparison(patch->child(0)->opcode()) || patch->child(0)->child(0)->type().isInt()))
        return false;
    if (!hasLegalTupleIndex(type))
        return false;

    unsigned numberOfResults = m_proc.resultCount(type);
    if (patch->resultConstraints.size() != numberOfResults)
        return false;
    for (unsigned index = 0; index < numberOfResults; ++index) {
        Type result = m_proc.resultType(type, index);
        if (!result.isNumeric() || !isLegalConstraint(patch->resultConstraints[index], result, Side::Result))
            return false;
    }
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        if (!isLegalConstraint(patch->constraintOf(index), patch->child(index)->type(), Side::Operand))
            return false;
    }

    if (patch->onlyKeepsChildrenAlive ? type != Void : !patch->generator())
        return false;

    if (patch->endsBlock())
        return true;
    for (unsigned index = 0; index < patch->owner()->numSuccessors(); ++index) {
        if (patch->lateClobberedOnSuccessor(index))
            return false;
    }
    return true;
}

bool MachineValidator::hasLegalTypes(const Operation* operation) const
{
    Type type = operation->type();
    auto typeOfChild = [&](unsigned index) {
        return operation->child(index)->type();
    };

    switch (operation->opcode()) {
    case Nop:
    case Fence:
    case Upsilon:
    case Jump:
    case Return:
    case Oops:
        return type == Void;
    case Opaque:
    case Neg:
        return type == typeOfChild(0);
    case Const32:
        return type == Int32;
    case Const64:
    case FramePointer:
        return type == Int64;
    case ConstFloat:
        return type == Float;
    case ConstDouble:
        return type == Double;
    case SlotBase: {
        const StackSlot* slot = operation->slot();
        std::span<StackSlot* const> slots = m_proc.stackSlots();
        return type == Int64 && slot && slot->index() < slots.size() && slots[slot->index()] == slot;
    }
    case AddressOf: {
        SymbolSpace space = operation->symbol().space;
        return type == Int64 && space != SymbolSpace::Number && space != SymbolSpace::InstanceWord;
    }
    case NumberOf:
        return (type == Int32 || type == Int64) && operation->symbol().space == SymbolSpace::Number;
    case InstanceAddressOf:
        return type == Int64 && operation->symbol().space == SymbolSpace::InstanceWord;
    case ArgumentReg:
        return type == (operation->argumentReg().isGPR() ? Int64 : Double);
    case Add:
    case Sub:
    case Mul:
    case Div:
        return type == typeOfChild(0) && type == typeOfChild(1);
    case MulHigh:
    case UMulHigh:
    case UDiv:
    case Mod:
    case UMod:
    case BitAnd:
    case BitOr:
    case BitXor:
        return type.isInt() && type == typeOfChild(0) && type == typeOfChild(1);
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
        return type.isInt() && type == typeOfChild(0) && typeOfChild(1) == Int32;
    case Clz:
    case PopCount:
        return type.isInt() && type == typeOfChild(0);
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
        return type.isFloat() && type == typeOfChild(0);
    case FMin:
    case FMax:
        return type.isFloat() && type == typeOfChild(0) && type == typeOfChild(1);
    case BitwiseCast:
        return type.isNumeric() && type.isInt() != typeOfChild(0).isInt() && widthOf(type) == widthOf(typeOfChild(0));
    case SExt8:
    case SExt16:
        return type == Int32 && typeOfChild(0) == Int32;
    case SExt32:
    case ZExt32:
        return type == Int64 && typeOfChild(0) == Int32;
    case Trunc:
        return type == Int32 && typeOfChild(0) == Int64;
    case IToD:
        return type == Double && typeOfChild(0).isInt();
    case FloatToDouble:
        return type == Double && typeOfChild(0) == Float;
    case DoubleToFloat:
        return type == Float && typeOfChild(0) == Double;
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
        return type == Int32 && typeOfChild(0) == typeOfChild(1);
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
        return type == Int32 && typeOfChild(0).isInt() && typeOfChild(0) == typeOfChild(1);
    case EqualOrUnordered:
        return type == Int32 && typeOfChild(0).isFloat() && typeOfChild(0) == typeOfChild(1);
    case Select:
        return typeOfChild(0).isInt() && type == typeOfChild(1) && type == typeOfChild(2);
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
        return type == Int32 && hasLegalAddress(operation);
    case Load:
        return type.isNumeric() && hasLegalAddress(operation);
    case Store8:
    case Store16:
        return type == Void && typeOfChild(0) == Int32 && hasLegalAddress(operation);
    case Store:
        return type == Void && hasLegalAddress(operation);
    case Phi:
        return type.isNumeric();
    case Extract: {
        if (!hasLegalTupleIndex(typeOfChild(0)))
            return false;
        std::span<const Type> members = m_proc.tupleOf(typeOfChild(0));
        unsigned index = operation->extractedIndex();
        return index < members.size() && type == members[index];
    }
    case Patch:
    case Check:
        return isLegalPatch(operation->as<PatchOperation>());
    case Branch:
        return type == Void && typeOfChild(0).isInt();
    case Switch: {
        if (type != Void || !typeOfChild(0).isInt())
            return false;
        bool isNarrow = typeOfChild(0) == Int32;
        std::span<const int64_t> values = operation->switchData().caseValues;
        for (size_t index = 0; index < values.size(); ++index) {
            if (index && values[index - 1] >= values[index])
                return false;
            if (isNarrow && values[index] != static_cast<int32_t>(values[index]))
                return false;
        }
        return true;
    }
    }
    return false;
}

bool MachineValidator::hasLegalFlags(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (operation->traps() && !isLoad(opcode))
        return false;
    if (operation->isChill() && !((opcode == Div || opcode == Mod) && operation->type().isInt()))
        return false;
    if (operation->isNarrowReference() && operation->type() != Int32)
        return false;
    return !operation->has(OperationFlag::IsInnerPointer) || operation->numChildren();
}

bool MachineValidator::hasInstructionOnTarget(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (!operation->type().isInt())
        return true;
#if CPU(X86_64)
    if (opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR)
        return operation->child(1)->hasInt32();
    return opcode != Div && opcode != UDiv && opcode != Mod && opcode != UMod && opcode != MulHigh && opcode != UMulHigh;
#else
    return opcode != Mod && opcode != UMod;
#endif
}

bool MachineValidator::checkOperations()
{
    uint32_t numberOfOperations = m_proc.numberOfOperations();
    ArenaVector<uint32_t> numberOfUses;
    numberOfUses.fill(m_arena, numberOfOperations, 0);

    for (uint32_t index = 0; index < numberOfOperations; ++index) {
        const Operation* operation = m_proc.operationAt(index);
        if (!isLeft(operation))
            continue;
        if (!hasLegalChildren(operation) || !hasLegalPosition(operation) || !hasLegalTypes(operation) || !hasLegalFlags(operation) || !hasInstructionOnTarget(operation))
            return fail(operation);
        if (countsChildrenAtFirstUse(operation->opcode()) && !operation->useCount())
            continue;
        std::span<Operation* const> children = operation->children();
        for (unsigned childIndex = 0; childIndex < children.size(); ++childIndex) {
            const Operation* child = children[childIndex];
            ++numberOfUses[child->index()];
            if (operation->opcode() != Trunc && !child->has(OperationFlag::UpperHalfIsRead))
                return fail(operation);
            if (!readsZeroExtendedWord(operation->opcode(), childIndex) && !child->has(OperationFlag::HasOpaqueReader))
                return fail(operation);
        }
    }

    for (uint32_t index = 0; index < numberOfOperations; ++index) {
        const Operation* operation = m_proc.operationAt(index);
        if (operation->useCount() < std::min<uint32_t>(numberOfUses[index], std::numeric_limits<uint16_t>::max()))
            return fail(operation);
    }
    return true;
}

bool MachineValidator::checkUpsilons()
{
    ArenaVector<uint32_t> lastBlockLeadingTo;
    ArenaVector<uint32_t> lastBlockHandingTo;
    lastBlockLeadingTo.fill(m_arena, m_proc.numberOfBlocks(), 0);
    lastBlockHandingTo.fill(m_arena, m_proc.numberOfOperations(), 0);

    for (const Block* block : m_proc.blocks()) {
        if (!block->isTerminated() || !isLeft(block))
            continue;
        uint32_t stamp = block->index() + 1;
        for (const Edge& edge : block->successors())
            lastBlockLeadingTo[edge.block()->index()] = stamp;

        bool mayHaveUpsilonsForUntakenEdges = block->last()->opcode() == Jump;
        for (const Operation* upsilon : block->upsilons()) {
            const Operation* phi = upsilon->phi();
            if (!isInProcedure(phi) || phi->opcode() != Phi || phi->type() != upsilon->child(0)->type())
                return fail(upsilon);
            if (lastBlockLeadingTo[phi->owner()->index()] != stamp && !mayHaveUpsilonsForUntakenEdges)
                return fail(upsilon);
            if (std::exchange(lastBlockHandingTo[phi->index()], stamp) == stamp)
                return fail(upsilon);
        }

        for (const Edge& edge : block->successors()) {
            for (const Operation* phi : edge.block()->phis()) {
                if (lastBlockHandingTo[phi->index()] != stamp)
                    return failAtEndOf(block);
            }
        }
    }
    return true;
}

void MachineValidator::computeDominators()
{
    m_placeOf.fill(m_arena, m_proc.numberOfBlocks(), notReached);

    ArenaVector<const Block*> postorder;
    ArenaVector<Visit> path;
    auto enter = [&](const Block* block) {
        m_placeOf[block->index()] = 0;
        path.append(m_arena, Visit { block, 0 });
    };
    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        if (m_placeOf[entrypoint.block->index()] != notReached)
            continue;
        enter(entrypoint.block);
        while (!path.isEmpty()) {
            Visit& visit = path.last();
            if (visit.nextSuccessor == visit.block->numSuccessors()) {
                postorder.append(m_arena, path.takeLast().block);
                continue;
            }
            const Block* successor = visit.block->successor(visit.nextSuccessor++).block();
            if (m_placeOf[successor->index()] == notReached)
                enter(successor);
        }
    }

    uint32_t numberReached = postorder.size();
    auto blockAt = [&](uint32_t place) {
        return postorder[numberReached - place];
    };
    for (uint32_t place = 1; place <= numberReached; ++place)
        m_placeOf[blockAt(place)->index()] = place;

    ArenaVector<uint32_t> dominator;
    dominator.fill(m_arena, numberReached + 1, notReached);
    dominator[0] = 0;
    for (const Entrypoint& entrypoint : m_proc.entrypoints())
        dominator[m_placeOf[entrypoint.block->index()]] = 0;

    ArenaVector<uint32_t> lastMergeAt;
    lastMergeAt.fill(m_arena, numberReached + 1, 0);
    uint32_t merge = 0;
    auto intersect = [&](uint32_t common, uint32_t from) {
        while (common != from) {
            if (common > from) {
                lastMergeAt[common] = merge;
                common = dominator[common];
                continue;
            }
            if (lastMergeAt[from] == merge)
                break;
            lastMergeAt[from] = merge;
            from = dominator[from];
        }
        return common;
    };

    ArenaVector<uint32_t> firstPredecessorOf;
    firstPredecessorOf.fill(m_arena, numberReached + 2, 0);
    for (const Entrypoint& entrypoint : m_proc.entrypoints())
        ++firstPredecessorOf[m_placeOf[entrypoint.block->index()] + 1];
    for (uint32_t place = 1; place <= numberReached; ++place) {
        for (const Edge& edge : blockAt(place)->successors())
            ++firstPredecessorOf[m_placeOf[edge.block()->index()] + 1];
    }
    for (uint32_t place = 1; place <= numberReached + 1; ++place)
        firstPredecessorOf[place] += firstPredecessorOf[place - 1];
    ArenaVector<uint32_t> predecessors;
    predecessors.fill(m_arena, firstPredecessorOf[numberReached + 1], 0);
    ArenaVector<uint32_t> nextFree;
    nextFree.fill(m_arena, numberReached + 1, 0);
    for (uint32_t place = 1; place <= numberReached; ++place)
        nextFree[place] = firstPredecessorOf[place];
    for (const Entrypoint& entrypoint : m_proc.entrypoints())
        predecessors[nextFree[m_placeOf[entrypoint.block->index()]]++] = 0;
    for (uint32_t place = 1; place <= numberReached; ++place) {
        for (const Edge& edge : blockAt(place)->successors())
            predecessors[nextFree[m_placeOf[edge.block()->index()]]++] = place;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t place = 1; place <= numberReached; ++place) {
            uint32_t next = notReached;
            ++merge;
            for (uint32_t index = firstPredecessorOf[place + 1]; index-- > firstPredecessorOf[place];) {
                uint32_t from = predecessors[index];
                if (dominator[from] == notReached)
                    continue;
                next = next == notReached ? from : intersect(next, from);
            }
            changed |= next != dominator[place];
            dominator[place] = next;
        }
    }

    m_sizeOfSubtree.fill(m_arena, numberReached + 1, 1);
    for (uint32_t place = numberReached; place; --place)
        m_sizeOfSubtree[dominator[place]] += m_sizeOfSubtree[place];

    m_preorder.fill(m_arena, numberReached + 1, 0);
    ArenaVector<uint32_t> nextNumberBelow;
    nextNumberBelow.fill(m_arena, numberReached + 1, 1);
    for (uint32_t place = 1; place <= numberReached; ++place) {
        uint32_t& next = nextNumberBelow[dominator[place]];
        m_preorder[place] = next;
        next += m_sizeOfSubtree[place];
        nextNumberBelow[place] = m_preorder[place] + 1;
    }
}

bool MachineValidator::dominates(const Block* dominator, const Block* block) const
{
    uint32_t above = m_placeOf[dominator->index()];
    uint32_t below = m_placeOf[block->index()];
    if (above == notReached || below == notReached)
        return false;
    return m_preorder[below] - m_preorder[above] < m_sizeOfSubtree[above];
}

bool MachineValidator::checkDominance()
{
    computeDominators();

    for (uint32_t index = 0; index < m_proc.numberOfOperations(); ++index) {
        const Operation* user = m_proc.operationAt(index);
        const Block* block = user->owner();
        if (!block || !isLeft(user))
            continue;
        if (m_isBehindBlockPasses && countsChildrenAtFirstUse(user->opcode()) && !user->useCount())
            continue;
        bool isReached = m_placeOf[block->index()] != notReached;
        for (const Operation* child : user->children()) {
            const Block* home = child->owner();
            if (!home)
                continue;
            if (home == block) {
                if (child->opcode() != Phi && user->opcode() != Upsilon && child->indexInBlock() >= user->indexInBlock())
                    return fail(user);
                continue;
            }
            if (isReached && !dominates(home, block))
                return fail(user);
        }
    }
    return true;
}

} // anonymous namespace

bool validate(Procedure& procedure)
{
    return MachineValidator(procedure, false).run();
}

bool validateBehindBlockPasses(Procedure& procedure)
{
    return MachineValidator(procedure, true).run();
}

bool checkThatDefinitionsDominateUses(Procedure& procedure)
{
    return MachineValidator(procedure, false).runCheckOfDominanceAlone();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
