/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "AOTMachineNumbering.h"
#include <wtf/Function.h>

namespace JSC { namespace AOT { namespace Machine {

struct SwitchCase {
    int64_t caseValue;
    Edge target;
};

struct ScopeFork {
    ScopeMark mark;
    Block* block { nullptr };
};

class Builder {
    WTF_MAKE_NONCOPYABLE(Builder);
public:
    JS_EXPORT_PRIVATE explicit Builder(Procedure&);

    Procedure& procedure() { return m_proc; }

    void setOrigin(Origin origin) { m_origin = origin; }
    Origin origin() const { return m_origin; }

    Block* newBlock() { return m_proc.addBlock(); }

    JS_EXPORT_PRIVATE void appendTo(Block*);
    JS_EXPORT_PRIVATE void appendToContinuation(Block*);
    JS_EXPORT_PRIVATE ScopeFork forkScope();
    JS_EXPORT_PRIVATE void appendToArm(Block*, const ScopeFork&);
    JS_EXPORT_PRIVATE void appendToJoin(Block*, const ScopeFork&);
    JS_EXPORT_PRIVATE void appendToSuccessor(Block*, const ScopeFork&);

    JS_EXPORT_PRIVATE Operation* const32(int32_t);
    JS_EXPORT_PRIVATE Operation* const64(int64_t);
    JS_EXPORT_PRIVATE Operation* constFloat(float);
    JS_EXPORT_PRIVATE Operation* constDouble(double);
    Operation* constantOfType(Type type, int64_t value) { return type == Int32 ? const32(static_cast<int32_t>(value)) : const64(value); }

    JS_EXPORT_PRIVATE Operation* unary(Opcode, Operation*);
    JS_EXPORT_PRIVATE Operation* binary(Opcode, Operation*, Operation*);
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
    JS_EXPORT_PRIVATE Operation* numberOf(CodeSymbol);
    JS_EXPORT_PRIVATE Operation* instanceAddressOf(CodeSymbol);

    enum class Trapping : bool { No, Yes };
    JS_EXPORT_PRIVATE Operation* load(Opcode, Type, Operation* base, ptrdiff_t offset, const AbstractHeap&, Trapping = Trapping::No);
    JS_EXPORT_PRIVATE Operation* store(Opcode, Operation* value, Operation* base, ptrdiff_t offset, const AbstractHeap&);
    JS_EXPORT_PRIVATE Operation* reload(Operation* load);
    JS_EXPORT_PRIVATE void noteIntegerAddedTo(Operation* load);
    JS_EXPORT_PRIVATE void fence();
    JS_EXPORT_PRIVATE static std::optional<int64_t> valueKnownToBeReadThrough(const Operation* base);

    JS_EXPORT_PRIVATE void verifyInheritedLoads(HeapIndex writtenByOtherThreads, Function<void(Operation* readSameBits)>&&);

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
    JS_EXPORT_PRIVATE Operation* upsilon(Operation* value, Operation* phi = nullptr);
    JS_EXPORT_PRIVATE void setPhi(Operation* upsilon, Operation* phi);
    JS_EXPORT_PRIVATE Operation* upsilonOfCompletePhi(Operation* value, Operation* phi);

    JS_EXPORT_PRIVATE PatchOperation* patch(Type);
    JS_EXPORT_PRIVATE PatchOperation* check(Operation* predicate);
    JS_EXPORT_PRIVATE Operation* findPurePatch(const PureFunction&, Type, std::span<Operation* const> operands);
    JS_EXPORT_PRIVATE void recordPurePatch(PatchOperation*);
    JS_EXPORT_PRIVATE void recordInPlaceOfPurePatch(const PureFunction&, Operation* phi);

    JS_EXPORT_PRIVATE void jump(Edge);
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
    Operation* duplicateIfFusable(Operation* operand)
    {
        if (!wasFoundInInheritedScope(operand)) [[likely]]
            return operand;
        return duplicateInheritedFindIfFusable(operand);
    }
    Operation* duplicateInheritedFindIfFusable(Operation*);
    bool wasFoundInInheritedScope(const Operation* operation) const
    {
        uint32_t index = operation->index();
        return index < m_epochOfLastInheritedFind.size() && m_epochOfLastInheritedFind[index] == m_scopeEpoch;
    }
    Operation* appendLoadLike(const Operation* load, Operation* base);
    void verifyInheritedLoad(Operation* load);
    template<unsigned numberOfChildren, bool isComputation> Operation* appendNewWithChildren(Opcode, Type, Operation* first, Operation* second, Operation* third);
    Operation* appendNew(Opcode, Type);
    Operation* appendNew(Opcode, Type, Operation*);
    Operation* appendNew(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*, Operation*);
    Operation* createWithoutPosition(Opcode, Type, uint64_t payload);
    Operation* createNewWithoutPosition(Opcode, Type, uint64_t payload, uint32_t hash);
    Operation* addWithoutPosition(Operation* base, int64_t constant);
    Operation* lowerForTarget(Opcode, Type, Operation*, Operation*);
    void append(Operation*, bool mayEndBlock);
    void sealPendingPatch()
    {
        if (m_pendingPatch) [[unlikely]]
            sealPatch();
    }
    void sealPatch();

    void checkAccess(const AbstractHeap& heap, bool isStore)
    {
        if (m_hasDeclaredAccesses) [[unlikely]]
            checkDeclaredAccess(heap, isStore);
    }
    void checkDeclaredAccess(const AbstractHeap&, bool isStore);

    Numbering m_numbering;
    ArenaVector<uint32_t> m_epochOfLastInheritedFind;
    uint32_t m_scopeEpoch { 0 };
    bool m_isDuplicating { false };
    bool m_isVerifyingInheritedLoad { false };
    HeapIndex m_heapWrittenByOtherThreads { rootHeapIndex };
    Function<void(Operation*)> m_verifierOfInheritedLoads;
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

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
