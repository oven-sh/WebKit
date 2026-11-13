/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "AOTMachineNumbering.h"
#include <array>
#include <wtf/Function.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Machine {

struct SwitchCase {
    int64_t caseValue;
    Edge target;
};

struct ScopeFork {
    ScopeMark mark;
    Block* block { nullptr };
};

enum class KindOfAppendedOperation : uint8_t {
    Computation,
    MemoryAccess,
    Any,
};

class Builder {
    WTF_MAKE_NONCOPYABLE(Builder);
public:
    JS_EXPORT_PRIVATE explicit Builder(Procedure&);

    Procedure& procedure() { return m_proc; }

    void setOrigin(Origin origin) { m_origin = origin; }
    Origin origin() const { return m_origin; }

    Block* newBlock() { return m_proc.addBlock(); }

    ALWAYS_INLINE void begin(Block* block)
    {
        if (!block->numberOfIncomingEdges()) [[unlikely]]
            m_proc.m_blocksBegunBeforeAnythingLedToThem.append(m_proc.arena(), block);
        m_block = block;
    }

    NEVER_INLINE void appendTo(Block* block)
    {
        ASSERT(block);
        sealPendingPatch();
        if (block == m_block)
            return;
        if (m_block)
            m_numbering.switchBlocks(m_block, block);
        begin(block);
    }
    ALWAYS_INLINE void appendToContinuation(Block* block)
    {
        sealPendingPatch();
        ASSERT(m_block->isTerminated());
        ASSERT(block->numberOfIncomingEdges() <= 1);
        if (block->numberOfIncomingEdges()) {
            ASSERT(hasEdgeBetween(m_block, block));
            block->m_hasOnlyOnePredecessor = true;
            m_numbering.continueIn(m_block, block);
        }
        begin(block);
    }
    ALWAYS_INLINE ScopeFork forkScope()
    {
        sealPendingPatch();
        return { m_numbering.mark(), m_block };
    }
    NEVER_INLINE void appendToArm(Block* block, const ScopeFork& fork)
    {
        sealPendingPatch();
        ASSERT(block->numberOfIncomingEdges() <= 1);
        if (block->numberOfIncomingEdges()) {
            ASSERT(hasEdgeBetween(fork.block, block));
            block->m_hasOnlyOnePredecessor = true;
        }
        m_numbering.restore(fork.mark);
        begin(block);
    }
    bool canRestoreScope(const ScopeFork& fork) const { return m_numbering.canRestore(fork.mark); }
    NEVER_INLINE void appendToJoin(Block* block, const ScopeFork& fork)
    {
        sealPendingPatch();
        m_numbering.restore(fork.mark);
        begin(block);
    }
    ALWAYS_INLINE void appendToSuccessor(Block* block, const ScopeFork& fork)
    {
        sealPendingPatch();
        bool comesOutOfFork = fork.block && block->singlePredecessor() == fork.block && block->classOfSingleIncomingEdge() != EdgeClass::Rare;
        if (!comesOutOfFork || block->isLandingPad() || !m_numbering.canRestore(fork.mark)) {
            appendTo(block);
            return;
        }
        block->m_hasOnlyOnePredecessor = true;
        m_numbering.inheritScope(fork.mark);
        ++m_scopeEpoch;
        m_hasInheritedFindsInEpoch = false;
        m_block = block;
    }

    Operation* const32(int32_t value)
    {
        if (static_cast<uint32_t>(value) < numberOfSmallConstantsKept) {
            if (Operation* kept = m_smallConstants32[static_cast<uint32_t>(value)])
                return kept;
        }
        return const32Slow(value);
    }
    Operation* const64(int64_t value)
    {
        if (static_cast<uint64_t>(value) < numberOfSmallConstantsKept) {
            if (Operation* kept = m_smallConstants64[static_cast<uint64_t>(value)])
                return kept;
        }
        return const64Slow(value);
    }
    JS_EXPORT_PRIVATE Operation* const32Slow(int32_t);
    JS_EXPORT_PRIVATE Operation* const64Slow(int64_t);
    JS_EXPORT_PRIVATE Operation* constFloat(float);
    JS_EXPORT_PRIVATE Operation* constDouble(double);
    Operation* constantOfType(Type type, int64_t value) { return type == Int32 ? const32(static_cast<int32_t>(value)) : const64(value); }

    JS_EXPORT_PRIVATE Operation* unary(Opcode, Operation*);
    JS_EXPORT_PRIVATE Operation* binary(Opcode, Operation*, Operation*);
    JS_EXPORT_PRIVATE bool findsAgain(Operation* sumOrProduct);
    bool isVisibleComputation(Operation*);
    struct LowWords {
        struct Cost {
            int conversions { 0 };
            unsigned operations { 0 };
        };
        UncheckedKeyHashMap<Operation*, Cost> costs;
        UncheckedKeyHashMap<Operation*, Operation*> made;
    };
    LowWords& lowWords() LIFETIME_BOUND { return m_lowWords; }
    JS_EXPORT_PRIVATE Operation* lowWord(Operation*);
    JS_EXPORT_PRIVATE Operation* extendWordThatIsNotNegative(Operation*);
    JS_EXPORT_PRIVATE Operation* chill(Opcode, Operation*, Operation*);
    JS_EXPORT_PRIVATE Operation* bitwiseCast(Operation*, Type);
    JS_EXPORT_PRIVATE Operation* select(Operation* predicate, Operation* whenNotZero, Operation* whenZero);
    JS_EXPORT_PRIVATE Operation* opaque(Operation*);
    JS_EXPORT_PRIVATE Operation* extract(Operation* tuple, unsigned index);

    JS_EXPORT_PRIVATE Operation* argumentRegister(Reg);
    JS_EXPORT_PRIVATE Operation* framePointer();
    JS_EXPORT_PRIVATE Operation* slotBase(StackSlot*);
    JS_EXPORT_PRIVATE Operation* addressOf(CodeSymbol);
    JS_EXPORT_PRIVATE Operation* numberOf(CodeSymbol, Type = Int32);
    JS_EXPORT_PRIVATE Operation* instanceAddressOf(CodeSymbol);

    enum class Trapping : bool { No, Yes };
    JS_EXPORT_PRIVATE Operation* load(Opcode, Type, Operation* base, ptrdiff_t offset, const AbstractHeap&, Trapping = Trapping::No);
    JS_EXPORT_PRIVATE Operation* store(Opcode, Operation* value, Operation* base, ptrdiff_t offset, const AbstractHeap&);
    JS_EXPORT_PRIVATE Operation* reload(Operation* load);
    JS_EXPORT_PRIVATE void forgetLoadsFrom(const AbstractHeap&);
    JS_EXPORT_PRIVATE void noteIntegerAddedTo(Operation* load);
    JS_EXPORT_PRIVATE void fence();
    JS_EXPORT_PRIVATE static std::optional<int64_t> valueKnownToBeReadThrough(const Operation* base);

    JS_EXPORT_PRIVATE void verifyReusedLoads(std::initializer_list<HeapIndex> writtenByOtherThreads, Function<void(Operation* readSameBits)>&&);

    JS_EXPORT_PRIVATE void setDeclaredAccesses(const HeapSet& reads, const HeapSet& writes);
    JS_EXPORT_PRIVATE void clearDeclaredAccesses();
    void suspendDeclaredAccesses()
    {
        if (!m_suspensionsOfDeclaredAccesses++) {
            m_hadDeclaredAccessesWhenSuspended = m_hasDeclaredAccesses;
            m_hasDeclaredAccesses = false;
        }
    }
    void resumeDeclaredAccesses()
    {
        ASSERT(m_suspensionsOfDeclaredAccesses);
        if (!--m_suspensionsOfDeclaredAccesses)
            m_hasDeclaredAccesses = m_hadDeclaredAccessesWhenSuspended;
    }

    Operation* phi(Type type) { return phiIn(m_block, type); }
    JS_EXPORT_PRIVATE Operation* phiIn(Block*, Type);
    void retypePhi(Operation* phi, Type type)
    {
        RELEASE_ASSERT(phi->opcode() == Phi && type.isNumeric());
        phi->m_type = type;
    }
    JS_EXPORT_PRIVATE Operation* upsilon(Operation* value, Operation* phi = nullptr);
    JS_EXPORT_PRIVATE void setPhi(Operation* upsilon, Operation* phi);

    JS_EXPORT_PRIVATE PatchOperation* patch(Type);
    JS_EXPORT_PRIVATE PatchOperation* check(Operation* predicate);
    JS_EXPORT_PRIVATE Operation* findPurePatch(const PureFunction&, Type, std::span<Operation* const> operands);
    JS_EXPORT_PRIVATE void recordPurePatch(PatchOperation*);
    JS_EXPORT_PRIVATE void recordInPlaceOfPurePatch(const PureFunction&, Operation* phi);
    JS_EXPORT_PRIVATE void recordInPlaceOfReadOfMemory(const PureFunction&, Operation* phi, Operation* operand, const AbstractHeap&);

    JS_EXPORT_PRIVATE void jump(Edge);
    JS_EXPORT_PRIVATE bool tryToLeadBranchPastEmptyBlock(Edge);
    JS_EXPORT_PRIVATE void branch(Operation* predicate, Edge whenNotZero, Edge whenZero);
    JS_EXPORT_PRIVATE void switchOn(Operation*, std::span<const SwitchCase>, Edge otherwise);
    JS_EXPORT_PRIVATE void ret(Operation*);
    JS_EXPORT_PRIVATE void retVoid();
    JS_EXPORT_PRIVATE void oops();

    Block* m_block { nullptr };

protected:
    Procedure& m_proc;

private:
    static Type typeOfResult(Opcode, Operation* first);

    Operation* findNumbered(const NumberingKey&, uint32_t hash);
    Operation* findNumberedLoad(const NumberingKey&, uint32_t hash);
    Operation* noteIfInherited(const Numbering::Found&);
    void noteInheritedFind(const Operation*);
    ALWAYS_INLINE Operation* duplicateIfFusable(Operation* operand)
    {
        if (!wasFoundInInheritedScope(operand)) [[likely]]
            return operand;
        return duplicateInheritedFindIfFusable(operand);
    }
    Operation* duplicateInheritedFindIfFusable(Operation*);
    ALWAYS_INLINE bool wasFoundInInheritedScope(const Operation* operation) const
    {
        if (!m_hasInheritedFindsInEpoch) [[likely]]
            return false;
        uint32_t index = operation->index();
        return index < m_epochOfLastInheritedFind.size() && m_epochOfLastInheritedFind[index] == m_scopeEpoch;
    }
    Operation* appendLoadLike(const Operation* load, Operation* base);
    Operation* makeSumOfStore(Operation* sum, const Operation* stored);
    Operation* makeOwnShiftOfSum(Operation* shift);
    void noteComparisonOfSum(Operation* comparison);
    Operation* compareTermOfSum(Operation* sum);
#if CPU(ARM64)
    Operation* sharedSumFor(Operation* sum);
#else
    Operation* sharedSumFor(Operation* sum) { return sum; }
#endif
    void verifyReusedLoad(Operation* load, Operation* baseAsked, int32_t offsetAsked);
    void noteNumberedLoadForStores(Operation* load);
    void stopIfLoadOfStoredWordStands(const Operation* store);
    bool isWrittenByOtherThreads(HeapIndex) const;
    template<unsigned numberOfChildren, KindOfAppendedOperation, bool hasRoom> Operation* appendNewWithChildren(Opcode, Type, Operation* first, Operation* second, Operation* third);
    Operation* appendNew(Opcode, Type);
    Operation* appendNew(Opcode, Type, Operation*);
    Operation* appendNew(Opcode, Type, Operation*, Operation*);
    Operation* appendNewMemoryAccess(Opcode, Type, Operation*);
    Operation* appendNewMemoryAccess(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*, Operation*);
    Operation* createWithoutPosition(Opcode, Type, uint64_t payload);
    template<bool hasRoom> Operation* createNewWithoutPosition(Opcode, Type, uint64_t payload, uint32_t hash);
    Operation* createNewWithoutPositionWhereRoomMayLack(Opcode, Type, uint64_t payload, uint32_t hash);
    Operation* addWithoutPosition(Operation* base, int64_t constant);
    Operation* lowerForTarget(Opcode, Type, Operation*, Operation*);
    bool hasRoomToCreate(size_t bytes) const;
    bool hasRoomToAppendTo(const Block*, size_t bytes) const;
    template<bool hasRoom> void didCreate(Operation*);
    template<bool hasRoom> void append(Operation*, bool mayEndBlock);
    template<bool hasRoom> Operation* appendUpsilon(Operation* value, Operation* phi);
    Operation* upsilonWhereRoomMayLack(Operation* value, Operation* phi);
    static Operation* countChildrenOfValueHandedOn(Operation* value, Operation* upsilon);
    template<bool hasRoom> PatchOperation* appendPatch(Type);
    PatchOperation* patchWhereRoomMayLack(Type);
    void jumpWhereRoomMayLack(Edge);
    void sealPendingPatch()
    {
        if (m_pendingPatch) [[unlikely]]
            sealPatch();
    }
    JS_EXPORT_PRIVATE void sealPatch();
#if ASSERT_ENABLED
    static bool hasEdgeBetween(const Block* from, const Block* to)
    {
        for (const Edge& edge : from->successors()) {
            if (edge.block() == to)
                return true;
        }
        return false;
    }
#endif

    void checkAccess(const AbstractHeap& heap, bool isStore)
    {
        if (m_hasDeclaredAccesses) [[unlikely]]
            checkDeclaredAccess(heap, isStore);
    }
    void checkDeclaredAccess(const AbstractHeap&, bool isStore);

    static constexpr uint32_t numberOfSmallConstantsKept = 16;

    Numbering m_numbering;
    std::array<Operation*, numberOfSmallConstantsKept> m_smallConstants32 { };
    std::array<Operation*, numberOfSmallConstantsKept> m_smallConstants64 { };
    ArenaVector<uint32_t> m_epochOfLastInheritedFind;
    uint32_t m_scopeEpoch { 0 };
#if CPU(ARM64)
    std::array<Operation*, 16> m_lastSharedSums { };
#endif
    bool m_hasInheritedFindsInEpoch { false };
    bool m_isDuplicating { false };
    bool m_isVerifyingReusedLoad { false };
    Vector<HeapIndex, 2> m_heapsWrittenByOtherThreads;
    UncheckedKeyHashMap<uint64_t, Vector<Operation*, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numberedLoadsByPlace;
    Function<void(Operation*)> m_verifierOfReusedLoads;
    LowWords m_lowWords;
    Origin m_origin;
    PatchOperation* m_pendingPatch { nullptr };
    bool m_hasDeclaredAccesses { false };
    bool m_hadDeclaredAccessesWhenSuspended { false };
    unsigned m_suspensionsOfDeclaredAccesses { 0 };
    HeapSet m_declaredReads;
    HeapSet m_declaredWrites;
};

class DeclaredAccessesSuspension {
    WTF_MAKE_NONCOPYABLE(DeclaredAccessesSuspension);
public:
    explicit DeclaredAccessesSuspension(Builder& builder)
        : m_builder(builder)
    {
        m_builder.suspendDeclaredAccesses();
    }
    ~DeclaredAccessesSuspension() { m_builder.resumeDeclaredAccesses(); }

private:
    Builder& m_builder;
};

PatchOperation* appendWithFixedRegisters(Builder&, Opcode, Type, Operation*, Operation*);
#if CPU(X86_64)
JS_EXPORT_PRIVATE Operation* appendFoldedProduct(Builder&, Operation*, Operation*);
#endif

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
