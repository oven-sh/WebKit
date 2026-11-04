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

// The scope at a branch, for the blocks that the branch dominates.
struct ScopeFork {
    ScopeMark mark;
    Block* block { nullptr };
};

// Step 1: append. Each request meets one chain: fold, find by number, lower for the target, append. What a fold builds goes through the same chain at once.
// Output (codegen/AOTOutput.h) gives these the names that the emitters call.
class Builder {
    WTF_MAKE_NONCOPYABLE(Builder);
public:
    JS_EXPORT_PRIVATE explicit Builder(Procedure&);

    Procedure& procedure() { return m_proc; }

    void setOrigin(Origin origin) { m_origin = origin; }
    Origin origin() const { return m_origin; }

    Block* newBlock() { return m_proc.addBlock(); }

    // The block starts with an empty scope, unless the emitters come back to a block they left: Numbering::switchBlocks().
    JS_EXPORT_PRIVATE void appendTo(Block*);
    // The current block has just been ended with an edge to this one, and no other edge will ever lead to it. It goes on in the same scope.
    JS_EXPORT_PRIVATE void appendToContinuation(Block*);
    // Taken in the block that is about to branch, behind the last operation in front of the branch.
    JS_EXPORT_PRIVATE ScopeFork forkScope();
    // The only edge to the block comes from the block of the fork. It sees what the fork saw.
    JS_EXPORT_PRIVATE void appendToArm(Block*, const ScopeFork&);
    // Every way to the block passes the fork, and every block between them has been filled. It sees what the fork saw, less the loads that a write since has ended.
    JS_EXPORT_PRIVATE void appendToJoin(Block*, const ScopeFork&);
    // All edges into the block exist. With exactly one, not rare, out of the fork's block, it inherits that scope. Otherwise, and if it is a landing pad, it starts empty.
    JS_EXPORT_PRIVATE void appendToSuccessor(Block*, const ScopeFork&);

    JS_EXPORT_PRIVATE Operation* const32(int32_t);
    JS_EXPORT_PRIVATE Operation* const64(int64_t);
    JS_EXPORT_PRIVATE Operation* constFloat(float);
    JS_EXPORT_PRIVATE Operation* constDouble(double);
    Operation* constantOfType(Type type, int64_t value) { return type == Int32 ? const32(static_cast<int32_t>(value)) : const64(value); }

    // The type of the result follows from the opcode and the operands, but for the three that name it.
    JS_EXPORT_PRIVATE Operation* unary(Opcode, Operation*);
    JS_EXPORT_PRIVATE Operation* binary(Opcode, Operation*, Operation*);
    // The low word of an Int64 of which the asker reads no more and takes nobody else to: `(a + b) | 0`, where the sum may have left int32 and nothing tests it.
    // Where that takes fewer operations it is made of the low words of what the Int64 is made of, and nothing is left to read the Int64. Otherwise it is a Trunc.
    JS_EXPORT_PRIVATE Operation* lowWord(Operation*);
    // A ZExt32 of an Int32 that the emitter has proof is no less than zero. The proof holds where the request is made, and so wherever the result is read.
    // It is another value than a ZExt32 of the word that was asked for without the proof, which may stand where the proof does not hold.
    JS_EXPORT_PRIVATE Operation* extendWordThatIsNotNegative(Operation*);
    // Div and Mod that answer where the machine would trap.
    JS_EXPORT_PRIVATE Operation* chill(Opcode, Operation*, Operation*);
    JS_EXPORT_PRIVATE Operation* bitwiseCast(Operation*, Type);
    JS_EXPORT_PRIVATE Operation* select(Operation* predicate, Operation* whenNotZero, Operation* whenZero);
    JS_EXPORT_PRIVATE Operation* opaque(Operation*);
    // The Extracts of a Patch stand right behind it, at the head of its first successor where it ends its block, with nothing between them, and a member has one. That successor has no other predecessor. Otherwise: FailureKind::MalformedProcedure.
    JS_EXPORT_PRIVATE Operation* extract(Operation* tuple, unsigned index);

    // Of a pinned register: one operation without position. Of any other: the value that the register holds where the current block, an entrypoint, is entered.
    JS_EXPORT_PRIVATE Operation* argumentRegister(Reg);
    JS_EXPORT_PRIVATE Operation* framePointer();
    JS_EXPORT_PRIVATE Operation* slotBase(StackSlot*);
    JS_EXPORT_PRIVATE Operation* addressOf(CodeSymbol);
    // SymbolSpace::Number.
    JS_EXPORT_PRIVATE Operation* numberOf(CodeSymbol);
    // SymbolSpace::InstanceWord. As the base of a load or a store with the offset zero it costs no register.
    JS_EXPORT_PRIVATE Operation* instanceAddressOf(CodeSymbol);

    enum class Trapping : bool { No, Yes };
    // An Add of a constant in the base moves into the offset. A load that traps asserts that valueKnownToBeReadThrough() knows nothing: the emitter throws there.
    JS_EXPORT_PRIVATE Operation* load(Opcode, Type, Operation* base, ptrdiff_t offset, const AbstractHeap&, Trapping = Trapping::No);
    // OperationFlag::ReadsZeroOffNoCell. Only where readsOffNoCellFault.
    JS_EXPORT_PRIVATE Operation* load32OffWhatMayBeNoCell(Operation* base, ptrdiff_t offset, const AbstractHeap&);
    JS_EXPORT_PRIVATE Operation* store(Opcode, Operation* value, Operation* base, ptrdiff_t offset, const AbstractHeap&);
    // `load` is what load() has found, and nothing has written its heap since. The same load once more among the block's own entries, or the one that is there already. From here on it is the one that is found.
    JS_EXPORT_PRIVATE Operation* reload(Operation* load);
    // The emitter has proof that the word which this Load of 64 bits reads is an integer from minIntegerAddedTo to maxIntegerAddedTo. The proof holds where the Load stands, and so wherever its result is read.
    JS_EXPORT_PRIVATE void noteIntegerAddedTo(Operation* load);
    // Orders the stores in front of it before those behind it for other threads. It writes no heap, so it ends no numbered load.
    JS_EXPORT_PRIVATE void fence();
    // The word that an access through this base is certain to read through: a constant base, or the constant that is added to the cage base register.
    JS_EXPORT_PRIVATE static std::optional<int64_t> valueKnownToBeReadThrough(const Operation* base);

    // Validating builds. A load from a mutable heap that a block finds in the scope it inherited is made once more where it is found, and the verifier is handed one if both read the same bits, else zero.
    // The verifier appends what stops the program at zero, and begins no block. A heap at or below `writtenByOtherThreads` is exempt.
    JS_EXPORT_PRIVATE void verifyInheritedLoads(HeapIndex writtenByOtherThreads, Function<void(Operation* readSameBits)>&&);

    // Checking builds. Until the next call, the heap of each load lies at or below a heap of `reads` or `writes`, and that of each store at or below a heap of `writes`.
    // A heap with HeapTrait::Immutable or HeapTrait::IsOutsideDeclaredEffects is exempt. Otherwise: FailureKind::AccessOutsideDeclaredEffects.
    JS_EXPORT_PRIVATE void setDeclaredAccesses(const HeapSet& reads, const HeapSet& writes);
    JS_EXPORT_PRIVATE void clearDeclaredAccesses();
    // Checking builds. Between the two calls no load and no store is held against the declared accesses. The calls nest.
    // Neither setDeclaredAccesses() nor clearDeclaredAccesses() comes between them.
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
    // In the current block. Without a Phi it waits for setPhi(), which comes once.
    JS_EXPORT_PRIVATE Operation* upsilon(Operation* value, Operation* phi = nullptr);
    JS_EXPORT_PRIVATE void setPhi(Operation* upsilon, Operation* phi);
    // `value` is a Phi that no further Upsilon will feed, so what it says of the upper half of its register stays as it is and goes on to `phi`.
    JS_EXPORT_PRIVATE Operation* upsilonOfCompletePhi(Operation* value, Operation* phi);

    // The emitter sets it up after this call. Its effects are read once, when it is sealed, and what it writes ends numbered loads from then on.
    // Every request seals it but those that neither append to the operations of a block nor touch the scope: a constant, framePointer(), slotBase(), addressOf(), numberOf(), instanceAddressOf(),
    // argumentRegister() of a pinned register, phiIn(), setPhi() and newBlock(). Emitters make those while they set up a Patch.
    JS_EXPORT_PRIVATE PatchOperation* patch(Type);
    // A predicate that is the constant zero gives an operation that belongs to no block and is never encoded.
    JS_EXPORT_PRIVATE PatchOperation* check(Operation* predicate);
    // A Patch is in its block before the emitter has said what it computes, so a pure one is looked for ahead of patch() and recorded once it is set up.
    JS_EXPORT_PRIVATE Operation* findPurePatch(const PureFunction&, Type, std::span<Operation* const> operands);
    // Numbers the result. Whether the Patch may be dropped is asked of its effects.
    JS_EXPORT_PRIVATE void recordPurePatch(PatchOperation*);
    // A Phi of the current block holds what a Patch of the function, which depends on nothing, would give here. findPurePatch() finds it as long as the scope lasts, and no further than the next suspension.
    JS_EXPORT_PRIVATE void recordInPlaceOfPurePatch(const PureFunction&, Operation* phi);

    JS_EXPORT_PRIVATE void jump(Edge);
    // With one way left it is a jump, along a normal edge whatever the class of that way was.
    JS_EXPORT_PRIVATE void branch(Operation* predicate, Edge whenNotZero, Edge whenZero);
    // With one way left it is a jump, along a normal edge whatever the class of that way was.
    JS_EXPORT_PRIVATE void switchOn(Operation*, std::span<const SwitchCase>, Edge otherwise);
    JS_EXPORT_PRIVATE void ret(Operation*);
    JS_EXPORT_PRIVATE void retVoid();
    JS_EXPORT_PRIVATE void oops();

    // The block that operations are appended to. Emitters read it. Only the appendTo family writes it.
    Block* m_block { nullptr };

protected:
    Procedure& m_proc;

private:
    static Type typeOfResult(Opcode, Operation* first);

    Operation* findNumbered(const NumberingKey&, uint32_t hash);
    Operation* findNumberedLoad(const NumberingKey&, uint32_t hash);
    Operation* noteIfInherited(const Numbering::Found&);
    void noteInheritedFind(const Operation*);
    // For an operand of an operation that is about to be appended and could take it in: a test, an access, a computation.
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
    // Not numbered.
    Operation* appendLoadLike(const Operation* load, Operation* base);
    void verifyInheritedLoad(Operation* load);
    // Behind folding and numbering.
    template<unsigned numberOfChildren, bool isComputation> Operation* appendNewWithChildren(Opcode, Type, Operation* first, Operation* second, Operation* third);
    Operation* appendNew(Opcode, Type);
    Operation* appendNew(Opcode, Type, Operation*);
    Operation* appendNew(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*);
    Operation* appendNewComputation(Opcode, Type, Operation*, Operation*, Operation*);
    Operation* createWithoutPosition(Opcode, Type, uint64_t payload);
    Operation* createNewWithoutPosition(Opcode, Type, uint64_t payload, uint32_t hash);
    // Add(base, constant), where the base is a pinned register, the FramePointer or a SlotBase. It is made again at each use, so it lives across nothing.
    Operation* addWithoutPosition(Operation* base, int64_t constant);
    Operation* lowerForTarget(Opcode, Type, Operation*, Operation*);
    void append(Operation*, bool mayEndBlock);
    // Reads the effects of the Patch that was appended last.
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
    // By the index of the operation: m_scopeEpoch when a request last found it among the inherited entries. Zero: never.
    ArenaVector<uint32_t> m_epochOfLastInheritedFind;
    // Rises with each block that inherits a scope.
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

// On x86-64: Div, UDiv, Mod, UMod, MulHigh, UMulHigh and the shifts by an amount in a register, as pure Patches with the registers that the instructions name.
// AOTMachineFixedRegistersX86_64.cpp. Elsewhere it is not called.
PatchOperation* appendWithFixedRegisters(Builder&, Opcode, Type, Operation*, Operation*);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
