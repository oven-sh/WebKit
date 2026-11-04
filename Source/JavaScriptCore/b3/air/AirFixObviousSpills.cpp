/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AirFixObviousSpills.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirCFG.h"
#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "Options.h"
#include "RegisterSet.h"
#include <wtf/GraphOrdering.h>
#include <wtf/IndexMap.h>
#include <wtf/ListDump.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

namespace AirFixObviousSpillsInternal {
static constexpr bool verbose = false;
}

class FixObviousSpills {
public:
    FixObviousSpills(Code& code)
        : m_code(code)
        , m_atHead(code.size(), State(SlotAliases::levelsFor(code.stackSlots().size())))
        , m_state(SlotAliases::levelsFor(code.stackSlots().size()))
    {
    }

    void run()
    {
        dataLogLnIf(AirFixObviousSpillsInternal::verbose, "Code before fixObviousSpills:\n", m_code);
        computeAliases();
        fixCode();
    }

private:
    void computeAliases()
    {
        Vector<BasicBlock*, 32> reversePostOrder;
        appendNodesInOrder(m_code.cfg(), GraphOrder::PostOrder, reversePostOrder);
        reversePostOrder.reverse();

        size_t numInOrder = reversePostOrder.size();
        IndexMap<BasicBlock*, unsigned> rpoNumber(m_code.size());
        for (size_t i = 0; i < numInOrder; ++i)
            rpoNumber[reversePostOrder[i]] = i;

        // shouldVisit is keyed by RPO position here (not block index). Position
        // 0 is the entry block, which is where the dataflow seeds.
        BitVector shouldVisit(m_code.size());
        BitVector notBottom(m_code.size());
        shouldVisit.quickSet(0);
        notBottom.quickSet(reversePostOrder[0]->index());

        unsigned cursor = 0;
        while ((cursor = shouldVisit.findBit(cursor, true)) < numInOrder) {
            shouldVisit.quickClear(cursor);
            BasicBlock* block = reversePostOrder[cursor];
            ASSERT(notBottom.quickGet(block->index()));
            m_block = block;
            m_state = m_atHead[block];

            dataLogLnIf(AirFixObviousSpillsInternal::verbose, "Executing block ", *m_block, ": ", m_state);
            for (m_instIndex = 0; m_instIndex < block->size(); ++m_instIndex) {
                dataLogLnIf(AirFixObviousSpillsInternal::verbose, "    Executing ", m_block->at(m_instIndex), ": ", m_state);
                if (!m_state.isEmpty())
                    clobberAllDefs();
                addInstAliases();
            }

            // Before we call merge we must make sure that the two states are sorted.
            m_state.sort();

            // Default to advancing forward. If a back-edge re-activates an earlier
            // block, rewind the cursor so findBit picks it up on the next step.
            unsigned nextCursor = cursor + 1;
            for (BasicBlock* successor : block->successorBlocks()) {
                unsigned successorIndex = successor->index();
                unsigned successorPosition = rpoNumber[successor];
                State& toState = m_atHead[successor];
                if (notBottom.quickGet(successorIndex)) {
                    bool changedAtSuccessorHead = toState.merge(m_state);
                    if (changedAtSuccessorHead) {
                        shouldVisit.quickSet(successorPosition);
                        nextCursor = std::min(nextCursor, successorPosition);
                    }
                } else { // The state at head of successor is bottom
                    toState = m_state;
                    notBottom.quickSet(successorIndex);
                    shouldVisit.quickSet(successorPosition);
                    nextCursor = std::min(nextCursor, successorPosition);
                }
            }
            cursor = nextCursor;
        }
    }

    void fixCode()
    {
        for (BasicBlock* block : m_code) {
            m_block = block;
            m_state.takeFrom(m_atHead[block]);

            for (m_instIndex = 0; m_instIndex < block->size(); ++m_instIndex) {
                if (m_state.isEmpty()) {
                    addInstAliases();
                    continue;
                }
                clobberEarlyDefs();
                fixInst();
                clobberLateDefs();
                addInstAliases();
            }
        }
    }

    template<typename Func>
    void forAllAliases(NOESCAPE const Func& func)
    {
        Inst& inst = m_block->at(m_instIndex);

        switch (inst.kind.opcode) {
        case Move:
            if (inst.args()[0].isSomeImm()) {
                if (inst.args()[1].isReg())
                    func(RegConst(inst.args()[1].reg(), inst.args()[0].value()));
                else if (isSpillSlot(inst.args()[1]))
                    func(SlotConst(inst.args()[1].stackSlot(), inst.args()[0].value()));
            } else if (isSpillSlot(inst.args()[0]) && inst.args()[1].isReg()) {
                if (std::optional<int64_t> constant = m_state.constantFor(inst.args()[0]))
                    func(RegConst(inst.args()[1].reg(), *constant));
                func(RegSlot(inst.args()[1].reg(), inst.args()[0].stackSlot(), RegSlot::AllBits));
            } else if (inst.args()[0].isReg() && isSpillSlot(inst.args()[1])) {
                if (std::optional<int64_t> constant = m_state.constantFor(inst.args()[0]))
                    func(SlotConst(inst.args()[1].stackSlot(), *constant));
                func(RegSlot(inst.args()[0].reg(), inst.args()[1].stackSlot(), RegSlot::AllBits));
            }
            break;

        case Move32:
            if (inst.args()[0].isSomeImm()) {
                if (inst.args()[1].isReg())
                    func(RegConst(inst.args()[1].reg(), static_cast<uint32_t>(inst.args()[0].value())));
                else if (isSpillSlot(inst.args()[1]))
                    func(SlotConst(inst.args()[1].stackSlot(), static_cast<uint32_t>(inst.args()[0].value())));
            } else if (isSpillSlot(inst.args()[0]) && inst.args()[1].isReg()) {
                if (std::optional<int64_t> constant = m_state.constantFor(inst.args()[0]))
                    func(RegConst(inst.args()[1].reg(), static_cast<uint32_t>(*constant)));
                func(RegSlot(inst.args()[1].reg(), inst.args()[0].stackSlot(), RegSlot::ZExt32));
            } else if (inst.args()[0].isReg() && isSpillSlot(inst.args()[1])) {
                if (std::optional<int64_t> constant = m_state.constantFor(inst.args()[0]))
                    func(SlotConst(inst.args()[1].stackSlot(), static_cast<int32_t>(*constant)));
                func(RegSlot(inst.args()[0].reg(), inst.args()[1].stackSlot(), RegSlot::Match32));
            }
            break;

        case MoveFloat:
            if (isSpillSlot(inst.args()[0]) && inst.args()[1].isReg())
                func(RegSlot(inst.args()[1].reg(), inst.args()[0].stackSlot(), RegSlot::Match32));
            else if (inst.args()[0].isReg() && isSpillSlot(inst.args()[1]))
                func(RegSlot(inst.args()[0].reg(), inst.args()[1].stackSlot(), RegSlot::Match32));
            break;

        case MoveDouble:
            if (isSpillSlot(inst.args()[0]) && inst.args()[1].isReg())
                func(RegSlot(inst.args()[1].reg(), inst.args()[0].stackSlot(), RegSlot::AllBits));
            else if (inst.args()[0].isReg() && isSpillSlot(inst.args()[1]))
                func(RegSlot(inst.args()[0].reg(), inst.args()[1].stackSlot(), RegSlot::AllBits));
            break;
            
        default:
            break;
        }
    }

    template<typename IsWhichDef>
    void clobberDefTmps(Arg& arg, Arg::Role role, Bank bank, Width width, NOESCAPE const IsWhichDef& isWhichDef)
    {
        auto mayReportDefTmp = [](const Arg& arg, bool argRoleIsDef) {
            return argRoleIsDef || arg.isPreIndex() || arg.isPostIndex();
        };

        if (!mayReportDefTmp(arg, isWhichDef(role))) {
            if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
                arg.forEachTmp(role, bank, width,
                    [&](Tmp& tmp, Arg::Role refinedRole, Bank, Width) {
                        RELEASE_ASSERT(!(isWhichDef(refinedRole) && tmp.isReg()));
                    });
            }
            return;
        }
        if (m_state.hasNoRegAlias())
            return;
        arg.forEachTmp(role, bank, width,
            [&](Tmp& tmp, Arg::Role refinedRole, Bank, Width) {
                if (isWhichDef(refinedRole) && tmp.isReg()) {
                    dataLogLnIf(AirFixObviousSpillsInternal::verbose, "        Clobbering ", tmp.reg());
                    m_state.clobber(tmp.reg());
                }
            });
    }

    void clobberDefs(Inst* prevInst, Inst* nextInst)
    {
        auto walk = [&] (Inst* inst, auto isWhichDef) {
            if (!inst)
                return;
            inst->forEachArg([&](Arg& arg, Arg::Role role, Bank bank, Width width) {
                // Only clobber spilled StackSlot since this phase's State only cares spilled StackSlots.
                if (isWhichDef(role) && arg.isStack() && arg.stackSlot()->isSpill()) {
                    dataLogLnIf(AirFixObviousSpillsInternal::verbose, "        Clobbering ", *arg.stackSlot());
                    m_state.clobber(arg.stackSlot());
                }
                clobberDefTmps(arg, role, bank, width, isWhichDef);
            });
        };
        walk(prevInst, [] (Arg::Role role) { return Arg::isLateDef(role); });
        walk(nextInst, [] (Arg::Role role) { return Arg::isEarlyDef(role); });

        // Patch's extra-clobbered registers aren't represented as args. Late
        // extras live on prevInst, early extras on nextInst — matching
        // forEachDefWithExtraClobberedRegs.
        if (prevInst && prevInst->kind.opcode == Patch) [[unlikely]] {
            prevInst->extraClobberedRegs().forEachWithWidthAndPreserved(
                [&](Reg reg, Width, PreservedWidth) { m_state.clobber(reg); });
        }
        if (nextInst && nextInst->kind.opcode == Patch) [[unlikely]] {
            nextInst->extraEarlyClobberedRegs().forEachWithWidthAndPreserved(
                [&](Reg reg, Width, PreservedWidth) { m_state.clobber(reg); });
        }
    }

    void clobberEarlyDefs()
    {
        clobberDefs(nullptr, &m_block->at(m_instIndex));
    }

    void clobberLateDefs()
    {
        clobberDefs(&m_block->at(m_instIndex), nullptr);
    }

    void clobberAllDefs()
    {
        Inst& inst = m_block->at(m_instIndex);
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank bank, Width width) {
            if (Arg::isAnyDef(role) && arg.isStack() && arg.stackSlot()->isSpill())
                m_state.clobber(arg.stackSlot());
            clobberDefTmps(arg, role, bank, width, [](Arg::Role role) { return Arg::isAnyDef(role); });
        });

        // Patch ops have extra-clobbered registers that aren't represented as
        // args. Both early and late extras clobber the same state here.
        if (inst.kind.opcode == Patch) [[unlikely]] {
            auto reportReg = [&](Reg reg, Width, PreservedWidth) { m_state.clobber(reg); };
            inst.extraEarlyClobberedRegs().forEachWithWidthAndPreserved(reportReg);
            inst.extraClobberedRegs().forEachWithWidthAndPreserved(reportReg);
        }
    }

    void addInstAliases()
    {
        forAllAliases(
            [&] (const auto& alias) {
                m_state.addAlias(alias);
            });
    }

    void fixInst()
    {
        Inst& inst = m_block->at(m_instIndex);
        dataLogLnIf(AirFixObviousSpillsInternal::verbose, "Fixing inst ", inst, ": ", m_state);
        
        // Check if alias analysis says that this is unnecessary.
        bool shouldLive = true;
        forAllAliases(
            [&] (const auto& alias) {
                shouldLive &= !m_state.contains(alias);
            });
        if (!shouldLive) {
            inst = Inst();
            return;
        }
        
        // First handle some special instructions.
        switch (inst.kind.opcode) {
        case Move: {
            if (inst.args()[0].isBigImm() && inst.args()[1].isReg()
                && isValidForm(m_code.targetFeatures(), Add64, Arg::Imm, Arg::Tmp, Arg::Tmp)) {
                // BigImm materializations are super expensive on both x86 and ARM. Let's try to
                // materialize this bad boy using math instead. Note that we use unsigned math here
                // since it's more deterministic.
                uint64_t myValue = inst.args()[0].value();
                Reg myDest = inst.args()[1].reg();
                for (Reg reg : m_state.constRegs) {
                    uint64_t otherValue = m_state.constants[reg.index()];

                    // Let's try add. That's the only thing that works on all platforms, since it's
                    // the only cheap arithmetic op that x86 does in three operands. Long term, we
                    // should add fancier materializations here for ARM if the BigImm is yuge.
                    uint64_t delta = myValue - otherValue;

                    if (Arg::isValidImmForm(delta)) {
                        if (delta) {
                            inst.kind = Add64;
                            inst.setArgs(Arg::imm(delta), Tmp(reg), Tmp(myDest));
                        } else {
                            inst.kind = Move;
                            inst.setArgs(Tmp(reg), Tmp(myDest));
                        }
                        return;
                    }
                }
                return;
            }
            break;
        }
            
        default:
            break;
        }
        
        // FIXME: This code should be taught how to simplify the spill-to-spill move
        // instruction. Basically it needs to know to remove the scratch arg.
        // https://bugs.webkit.org/show_bug.cgi?id=171133

        // Substitution below only ever replaces a spill slot argument, and it can only do that from
        // a RegSlot or a SlotConst, so without either there is nothing to look for.
        if (m_state.hasNoSlotAlias())
            return;

        // It also needs the instruction to mention a spill slot at all. Deciding that does not need
        // the Arg roles, and iterating args() can only over-approximate what forEachArg reports, so
        // this cannot skip an instruction the scan below would have changed. Worth doing before the
        // Inst copy, which is otherwise paid by every instruction that gets this far.
        bool mentionsSpillSlot = false;
        for (Arg& arg : inst.args()) {
            if (isSpillSlot(arg)) {
                mentionsSpillSlot = true;
                break;
            }
        }
        if (!mentionsSpillSlot)
            return;

        // Create a copy in case we invalidate the instruction. That doesn't happen often.
        Inst instCopy = inst;

        // The goal is to replace references to stack slots. We only care about early uses. We can't
        // handle UseDefs. We could teach this to handle UseDefs if we inserted a store instruction
        // after and we proved that the register aliased to the stack slot dies here. We can get that
        // information from the liveness analysis. We also can't handle late uses, because we don't
        // look at late clobbers when doing this.
        bool didThings = false;
        auto handleArg = [&] (Arg& arg, Arg::Role role, Bank, Width width) {
            if (!isSpillSlot(arg))
                return;
            if (!Arg::isEarlyUse(role))
                return;
            if (Arg::isAnyDef(role))
                return;
            
            // Try to get a register if at all possible.
            if (const RegSlot* alias = m_state.getRegSlot(arg.stackSlot())) {
                switch (width) {
                case Width64:
                    if (alias->mode != RegSlot::AllBits)
                        return;
                    dataLogLnIf(AirFixObviousSpillsInternal::verbose, "    Replacing ", arg, " with ", alias->reg);
                    arg = Tmp(alias->reg);
                    didThings = true;
                    return;
                case Width32:
                    dataLogLnIf(AirFixObviousSpillsInternal::verbose, "    Replacing ", arg, " with ", alias->reg, " (subwidth case)");
                    arg = Tmp(alias->reg);
                    didThings = true;
                    return;
                default:
                    return;
                }
            }

            // Revert to immediate if that didn't work.
            if (const SlotConst* alias = m_state.getSlotConst(arg.stackSlot())) {
                dataLogLnIf(AirFixObviousSpillsInternal::verbose, "    Replacing ", arg, " with constant ", alias->constant);
                if (Arg::isValidImmForm(alias->constant))
                    arg = Arg::imm(alias->constant);
                else
                    arg = Arg::bigImm(alias->constant);
                didThings = true;
                return;
            }
        };
        
        inst.forEachArg(handleArg);
        if (!didThings || inst.isValidForm(m_code.targetFeatures()))
            return;
        
        // We introduced something invalid along the way. Back up and carefully handle each argument.
        inst = instCopy;
        ASSERT(inst.isValidForm(m_code.targetFeatures()));
        inst.forEachArg(
            [&] (Arg& arg, Arg::Role role, Bank bank, Width width) {
                Arg argCopy = arg;
                handleArg(arg, role, bank, width);
                if (!inst.isValidForm(m_code.targetFeatures()))
                    arg = argCopy;
            });
    }
    
    static bool LEGACY_NODELETE isSpillSlot(const Arg& arg)
    {
        return arg.isStack() && arg.stackSlot()->isSpill();
    }

    struct RegConst {
        RegConst(Reg reg, int64_t constant)
            : reg(reg)
            , constant(constant)
        {
        }

        Reg reg;
        int64_t constant { 0 };
    };

    struct RegSlot {
        enum Mode : int8_t {
            AllBits,
            ZExt32, // Register contains zero-extended contents of stack slot.
            Match32 // Low 32 bits of register match low 32 bits of stack slot.
        };
        
        RegSlot()
        {
        }

        RegSlot(Reg reg, StackSlot* slot, Mode mode)
            : slot(slot)
            , reg(reg)
            , mode(mode)
        {
        }

        explicit operator bool() const
        {
            return slot && reg;
        }
        
        friend bool LEGACY_NODELETE operator==(const RegSlot&, const RegSlot&) = default;

        bool LEGACY_NODELETE operator<(const RegSlot& other) const
        {
            // We ignore `mode` on purpose, see merge() for how we deal with it.
            return slot < other.slot || (slot == other.slot && reg < other.reg);
        }

        void dump(PrintStream& out) const
        {
            out.print(pointerDump(slot), "->", reg);
            switch (mode) {
            case AllBits:
                out.print("(AllBits)");
                break;
            case ZExt32:
                out.print("(ZExt32)");
                break;
            case Match32:
                out.print("(Match32)");
                break;
            }
        }
        
        StackSlot* slot { nullptr };
        Reg reg;
        Mode mode { AllBits };
    };

    struct SlotConst {
        SlotConst()
        {
        }

        SlotConst(StackSlot* slot, int64_t constant)
            : slot(slot)
            , constant(constant)
        {
        }

        explicit operator bool() const
        {
            return slot;
        }

        friend bool LEGACY_NODELETE operator==(const SlotConst&, const SlotConst&) = default;

        bool LEGACY_NODELETE operator<(const SlotConst& other) const
        {
            return slot < other.slot || (slot == other.slot && constant < other.constant);
        }

        void dump(PrintStream& out) const
        {
            out.print(pointerDump(slot), "->", constant);
        }
        
        StackSlot* slot { nullptr };
        int64_t constant { 0 };
    };

    // The aliases of some spill slots. Those of one slot are in the order in which getRegSlot() looks at them: sorted up to the
    // end of the last block, then as they were added.
    struct AliasLists {
        bool isEmpty() const { return slotConst.isEmpty() && regSlot.isEmpty(); }

        ScalarRegisterSet regs() const
        {
            ScalarRegisterSet result;
            for (const RegSlot& alias : regSlot)
                result.add(alias.reg);
            return result;
        }

        // Returns whether an alias of a register went.
        bool clobber(StackSlot* slot)
        {
            slotConst.removeAllMatching([&](const SlotConst& alias) { return alias.slot == slot; });
            return regSlot.removeAllMatching([&](const RegSlot& alias) { return alias.slot == slot; });
        }

        void clobber(Reg reg)
        {
            regSlot.removeAllMatching([&](const RegSlot& alias) { return alias.reg == reg; });
        }

        void sort()
        {
            auto isLess = [](const auto& a, const auto& b) { return a < b; };
            std::ranges::sort(slotConst, isLess);
            std::ranges::sort(regSlot, isLess);
        }

        // Takes two sorted vectors, for each element in the first, it looks for the first element in the second which is not smaller.
        // If such an element exist, call f on both the element from the first vector and this element.
        // Remove the element from the first vector unless f returned true (so f says whether to keep the element)
        // Returns true if any element has been removed.
        template<typename T, typename Func>
        static bool filterVectorAgainst(Vector<T>& own, const Vector<T>& other, Func f)
        {
            const T* it = other.begin();
            const T* end = other.end();
            return !!own.removeAllMatching(
                [&] (T& alias) {
                    it = std::find_if_not(it, end, [&] (const T& otherAlias) {
                        return otherAlias < alias;
                    });
                    if (it == end)
                        return true;
                    return !f(alias, *it);
            });
        }

        // Keeps what both have. Both are sorted. Returns whether anything changed.
        bool merge(const AliasLists& other)
        {
            bool changed = false;
            changed |= filterVectorAgainst(slotConst, other.slotConst, [](SlotConst& a, const SlotConst& b) { return a == b; });
            changed |= filterVectorAgainst(regSlot, other.regSlot, [&](RegSlot& alias, const RegSlot& otherAlias) {
                if (alias.reg != otherAlias.reg || alias.slot != otherAlias.slot)
                    return false;
                if (alias.mode != RegSlot::Match32 && alias.mode != otherAlias.mode) {
                    alias.mode = RegSlot::Match32;
                    changed = true;
                }
                return true;
            });
            return changed;
        }

        friend bool operator==(const AliasLists&, const AliasLists&) = default;

        Vector<SlotConst> slotConst;
        Vector<RegSlot> regSlot;
    };

    // The aliases of all spill slots. Where the code has few slots they are in one pair of lists. Otherwise they are in a tree by
    // the index of the slot, whose leaves have the aliases of slotsPerLeaf slots. Copies share nodes and a change copies the nodes
    // on the way to one leaf, so the states at the heads of all blocks together take room for what changes from block to block.
    class SlotAliases {
    public:
        static constexpr unsigned bitsPerLevel = 4;
        static constexpr unsigned fanOut = 1 << bitsPerLevel;
        static constexpr unsigned slotsPerLeaf = fanOut;
        static constexpr unsigned maxSlotsInOnePairOfLists = 256;

        // 0 for one pair of lists.
        static unsigned levelsFor(size_t numberOfSlots)
        {
            if (numberOfSlots <= maxSlotsInOnePairOfLists)
                return 0;
            unsigned levels = 1;
            while (numberOfSlots > static_cast<size_t>(slotsPerLeaf) << levels * bitsPerLevel)
                ++levels;
            return levels;
        }

        explicit SlotAliases(unsigned levels)
            : m_levels(levels)
        {
        }

        SlotAliases(const SlotAliases&) = default;

        // All the states of one run have the same number of levels, and what is copied is sorted.
        SlotAliases& operator=(const SlotAliases& other)
        {
            ASSERT(m_levels == other.m_levels && other.m_isSorted);
            m_regs = other.m_regs;
            m_isEmpty = other.m_isEmpty;
            m_isSorted = true;
            if (m_levels) {
                m_root = other.m_root;
                m_unsortedSlots.shrink(0);
            } else
                m_lists = other.m_lists;
            return *this;
        }

        // The other one is not looked at again.
        void takeFrom(SlotAliases& other)
        {
            if (m_levels) {
                m_root = nullptr;
                *this = other;
                other.m_root = nullptr;
            } else
                *this = other;
        }

        bool isEmpty() const { return m_isEmpty; }
        bool hasAliasOf(Reg reg) const { return m_regs.contains(reg); }
        bool hasAliasOfAnyReg() const { return !m_regs.isEmpty(); }

        void add(const RegSlot& alias)
        {
            ownListsOf(alias.slot, alias.reg).regSlot.append(alias);
            m_regs.add(alias.reg);
            m_isEmpty = false;
        }

        void add(const SlotConst& alias)
        {
            ownListsOf(alias.slot, Reg()).slotConst.append(alias);
            m_isEmpty = false;
        }

        bool contains(const RegSlot& alias) const
        {
            if (!hasAliasOf(alias.reg))
                return false;
            const AliasLists* lists = listsOf(alias.slot);
            return lists && lists->regSlot.contains(alias);
        }

        bool contains(const SlotConst& alias) const
        {
            const AliasLists* lists = listsOf(alias.slot);
            return lists && lists->slotConst.contains(alias);
        }

        const RegSlot* getRegSlot(StackSlot* slot) const
        {
            if (const AliasLists* lists = listsOf(slot)) {
                for (const RegSlot& alias : lists->regSlot) {
                    if (alias.slot == slot)
                        return &alias;
                }
            }
            return nullptr;
        }

        const SlotConst* getSlotConst(StackSlot* slot) const
        {
            if (const AliasLists* lists = listsOf(slot)) {
                for (const SlotConst& alias : lists->slotConst) {
                    if (alias.slot == slot)
                        return &alias;
                }
            }
            return nullptr;
        }

        void clobber(Reg reg)
        {
            if (!hasAliasOf(reg))
                return;
            if (m_levels)
                clobber(m_root, m_levels, reg);
            else
                m_lists.clobber(reg);
            m_regs.remove(reg);
            updateIsEmpty();
        }

        void clobber(StackSlot* slot)
        {
            if (m_isEmpty)
                return;
            if (!m_levels) {
                if (m_lists.clobber(slot))
                    m_regs = m_lists.regs();
                updateIsEmpty();
                return;
            }
            const AliasLists* lists = listsOf(slot);
            if (!lists)
                return;
            auto isOfSlot = [&](const auto& alias) { return alias.slot == slot; };
            if (!lists->regSlot.containsIf(isOfSlot) && !lists->slotConst.containsIf(isOfSlot))
                return;
            clobber(m_root, m_levels, slot);
            updateRegs();
        }

        void sort()
        {
            if (m_isSorted)
                return;
            m_isSorted = true;
            if (!m_levels) {
                m_lists.sort();
                return;
            }
            for (StackSlot* slot : m_unsortedSlots) {
                Leaf* leaf = leafOf(slot);
                if (!leaf || leaf->isSorted)
                    continue;
                Leaf& ownLeaf = ownLeafOf(slot, Reg());
                ownLeaf.lists.sort();
                ownLeaf.isSorted = true;
            }
            m_unsortedSlots.shrink(0);
        }

        // Keeps what both have. Both are sorted. Returns whether anything changed.
        bool merge(const SlotAliases& other)
        {
            ASSERT(m_isSorted && other.m_isSorted);
            if (!(m_levels ? merge(m_root, other.m_root, m_levels) : m_lists.merge(other.m_lists)))
                return false;
            updateRegs();
            return true;
        }

        void dump(PrintStream& out) const
        {
            CommaPrinter comma;
            if (m_levels)
                dump(out, comma, m_root.get(), m_levels);
            else
                dump(out, comma, m_lists);
        }

    private:
        struct Node : public RefCounted<Node> {
            WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Node);
        public:
            Node() = default;
            Node(const Node& other)
                : RefCounted<Node>()
                , regs(other.regs)
            {
            }
            virtual ~Node() = default;

            // The registers that the aliases below name.
            ScalarRegisterSet regs;
        };

        struct Leaf final : public Node {
            AliasLists lists;
            bool isSorted { true };
        };

        struct Branch final : public Node {
            std::array<RefPtr<Node>, fanOut> children;
        };

        // The root is at m_levels and a leaf at 0.
        static Leaf& asLeaf(Node& node) { return static_cast<Leaf&>(node); }
        static Branch& asBranch(Node& node) { return static_cast<Branch&>(node); }
        static unsigned childIndex(StackSlot* slot, unsigned level) { return slot->index() >> level * bitsPerLevel & (fanOut - 1); }

        void updateIsEmpty() { m_isEmpty = m_levels ? !m_root : m_lists.isEmpty(); }

        void updateRegs()
        {
            updateIsEmpty();
            if (m_levels)
                m_regs = m_root ? m_root->regs : ScalarRegisterSet();
            else
                m_regs = m_lists.regs();
        }

        Leaf* leafOf(StackSlot* slot) const
        {
            Node* node = m_root.get();
            for (unsigned level = m_levels; node && level; --level)
                node = asBranch(*node).children[childIndex(slot, level)].get();
            return static_cast<Leaf*>(node);
        }

        const AliasLists* listsOf(StackSlot* slot) const
        {
            if (!m_levels)
                return &m_lists;
            const Leaf* leaf = leafOf(slot);
            return leaf ? &leaf->lists : nullptr;
        }

        // Afterwards nothing else sees a change to the node.
        static void makeOwn(RefPtr<Node>& node, unsigned level)
        {
            if (node && node->hasOneRef())
                return;
            if (level)
                node = node ? adoptRef(*new Branch(asBranch(*node))) : adoptRef(*new Branch);
            else
                node = node ? adoptRef(*new Leaf(asLeaf(*node))) : adoptRef(*new Leaf);
        }

        // After a removal.
        static void didEdit(RefPtr<Node>& node, unsigned level)
        {
            bool isEmpty = true;
            if (level) {
                node->regs = { };
                for (RefPtr<Node>& child : asBranch(*node).children) {
                    if (!child)
                        continue;
                    isEmpty = false;
                    node->regs.merge(child->regs);
                }
            } else {
                isEmpty = asLeaf(*node).lists.isEmpty();
                node->regs = asLeaf(*node).lists.regs();
            }
            if (isEmpty)
                node = nullptr;
        }

        // For an addition. The alias to be added names the register, if one is given.
        Leaf& ownLeafOf(StackSlot* slot, Reg reg)
        {
            RefPtr<Node>* node = &m_root;
            for (unsigned level = m_levels;; --level) {
                makeOwn(*node, level);
                if (reg)
                    (*node)->regs.add(reg);
                if (!level)
                    return asLeaf(**node);
                node = &asBranch(**node).children[childIndex(slot, level)];
            }
        }

        AliasLists& ownListsOf(StackSlot* slot, Reg reg)
        {
            m_isSorted = false;
            if (!m_levels)
                return m_lists;
            Leaf& leaf = ownLeafOf(slot, reg);
            if (std::exchange(leaf.isSorted, false))
                m_unsortedSlots.append(slot);
            return leaf.lists;
        }

        static void clobber(RefPtr<Node>& node, unsigned level, StackSlot* slot)
        {
            makeOwn(node, level);
            if (level)
                clobber(asBranch(*node).children[childIndex(slot, level)], level - 1, slot);
            else
                asLeaf(*node).lists.clobber(slot);
            didEdit(node, level);
        }

        static void clobber(RefPtr<Node>& node, unsigned level, Reg reg)
        {
            makeOwn(node, level);
            if (level) {
                for (RefPtr<Node>& child : asBranch(*node).children) {
                    if (child && child->regs.contains(reg))
                        clobber(child, level - 1, reg);
                }
            } else
                asLeaf(*node).lists.clobber(reg);
            didEdit(node, level);
        }

        static bool merge(RefPtr<Node>& own, const RefPtr<Node>& other, unsigned level)
        {
            if (own == other || !own)
                return false;
            if (!other) {
                own = nullptr;
                return true;
            }

            bool changed = false;
            if (level) {
                const Branch& otherBranch = asBranch(*other);
                bool isSame = true;
                for (unsigned index = 0; index < fanOut; ++index) {
                    if (asBranch(*own).children[index] == otherBranch.children[index])
                        continue;
                    makeOwn(own, level);
                    RefPtr<Node>& child = asBranch(*own).children[index];
                    changed |= merge(child, otherBranch.children[index], level - 1);
                    isSame &= child == otherBranch.children[index];
                }
                if (isSame) {
                    own = other;
                    return changed;
                }
            } else {
                if (asLeaf(*own).lists == asLeaf(*other).lists) {
                    own = other;
                    return false;
                }
                makeOwn(own, level);
                changed = asLeaf(*own).lists.merge(asLeaf(*other).lists);
            }
            if (changed)
                didEdit(own, level);
            return changed;
        }

        static void dump(PrintStream& out, CommaPrinter& comma, const AliasLists& lists)
        {
            for (const SlotConst& alias : lists.slotConst)
                out.print(comma, alias);
            for (const RegSlot& alias : lists.regSlot)
                out.print(comma, alias);
        }

        static void dump(PrintStream& out, CommaPrinter& comma, Node* node, unsigned level)
        {
            if (!node)
                return;
            if (!level) {
                dump(out, comma, asLeaf(*node).lists);
                return;
            }
            for (RefPtr<Node>& child : asBranch(*node).children)
                dump(out, comma, child.get(), level - 1);
        }

        AliasLists m_lists;
        RefPtr<Node> m_root;
        // The registers that the aliases name.
        ScalarRegisterSet m_regs;
        // One of each leaf that is not sorted.
        Vector<StackSlot*> m_unsortedSlots;
        unsigned m_levels;
        bool m_isEmpty { true };
        bool m_isSorted { true };
    };

    struct State {
        explicit State(unsigned levels)
            : slotAliases(levels)
        {
        }

        State(const State& other)
            : constRegs(other.constRegs)
            , slotAliases(other.slotAliases)
        {
            copyConstants(other);
        }

        State& operator=(const State& other)
        {
            constRegs = other.constRegs;
            copyConstants(other);
            slotAliases = other.slotAliases;
            return *this;
        }

        // The other one is not looked at again.
        void takeFrom(State& other)
        {
            constRegs = other.constRegs;
            copyConstants(other);
            slotAliases.takeFrom(other.slotAliases);
        }

        bool LEGACY_NODELETE isEmpty() const { return constRegs.isEmpty() && slotAliases.isEmpty(); }

        bool LEGACY_NODELETE hasNoSlotAlias() const { return slotAliases.isEmpty(); }

        bool LEGACY_NODELETE hasNoRegAlias() const { return constRegs.isEmpty() && !slotAliases.hasAliasOfAnyReg(); }

        void addAlias(const RegConst& newAlias)
        {
            constRegs.add(newAlias.reg);
            constants[newAlias.reg.index()] = newAlias.constant;
        }
        void addAlias(const RegSlot& newAlias) { slotAliases.add(newAlias); }
        void addAlias(const SlotConst& newAlias) { slotAliases.add(newAlias); }

        bool contains(const RegConst& alias) const
        {
            return constantForReg(alias.reg) == alias.constant;
        }
        bool contains(const RegSlot& alias) const { return slotAliases.contains(alias); }
        bool contains(const SlotConst& alias) const { return slotAliases.contains(alias); }

        std::optional<int64_t> LEGACY_NODELETE constantForReg(Reg reg) const
        {
            if (!constRegs.contains(reg))
                return std::nullopt;
            return constants[reg.index()];
        }

        const RegSlot* LEGACY_NODELETE getRegSlot(StackSlot* slot) const { return slotAliases.getRegSlot(slot); }

        const SlotConst* LEGACY_NODELETE getSlotConst(StackSlot* slot) const { return slotAliases.getSlotConst(slot); }

        std::optional<int64_t> LEGACY_NODELETE constantFor(const Arg& arg)
        {
            if (arg.isReg())
                return constantForReg(arg.reg());
            if (arg.isStack()) {
                if (const SlotConst* alias = getSlotConst(arg.stackSlot()))
                    return alias->constant;
                return std::nullopt;
            }
            return std::nullopt;
        }

        void clobber(Reg reg)
        {
            constRegs.remove(reg);
            slotAliases.clobber(reg);
        }

        void clobber(StackSlot* slot) { slotAliases.clobber(slot); }

        void sort() { slotAliases.sort(); }

        bool merge(const State& other)
        {
            bool changed = false;

            ScalarRegisterSet survivingConstRegs;
            for (Reg reg : constRegs) {
                if (other.constantForReg(reg) == constants[reg.index()])
                    survivingConstRegs.add(reg);
            }
            if (survivingConstRegs != constRegs) {
                constRegs = survivingConstRegs;
                changed = true;
            }

            changed |= slotAliases.merge(other.slotAliases);
            return changed;
        }

        void dump(PrintStream& out) const
        {
            out.print("{regConst = [");
            CommaPrinter comma;
            for (Reg reg : constRegs)
                out.print(comma, reg, "->", constants[reg.index()]);
            out.print("], aliases of slots = [", slotAliases, "]}");
        }

        void copyConstants(const State& other)
        {
            if (!other.constRegs.isEmpty())
                constants = other.constants;
        }

        ScalarRegisterSet constRegs;
        // Only those of constRegs mean anything.
        std::array<int64_t, Reg::maxIndex() + 1> constants;
        SlotAliases slotAliases;
    };

    Code& m_code;
    IndexMap<BasicBlock*, State> m_atHead;
    State m_state;
    BasicBlock* m_block { nullptr };
    unsigned m_instIndex { 0 };
};

} // anonymous namespace

void fixObviousSpills(Code& code)
{
    PhaseScope phaseScope(code, "fixObviousSpills"_s);

    FixObviousSpills fixObviousSpills(code);
    fixObviousSpills.run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

