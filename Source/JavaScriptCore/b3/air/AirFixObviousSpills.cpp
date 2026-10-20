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
#include "AirFormTraits.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "Options.h"
#include "RegisterSet.h"
#include <wtf/GraphOrdering.h>
#include <wtf/IndexMap.h>
#include <wtf/ListDump.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

namespace {

namespace AirFixObviousSpillsInternal {
static constexpr bool verbose = false;
}

class FixObviousSpills {
public:
    FixObviousSpills(Code& code)
        : m_code(code)
        , m_atHead(code.size())
        , m_firstInstOfBlock(code.size())
    {
    }

    void run()
    {
        dataLogLnIf(AirFixObviousSpillsInternal::verbose, "Code before fixObviousSpills:\n", m_code);
        decode();
        computeAliases();
        fixCode();
    }

private:
    using Mentions = Greedy::Mentions;

    static constexpr uint8_t definesSpillSlot = 1;
    static constexpr uint8_t mayAddAlias = 2;
    static constexpr uint8_t namesSpillSlot = 4;

    struct DefinedRegs {
        uint64_t early;
        uint64_t late;
    };

    ALWAYS_INLINE static void noteArg(Arg& arg, unsigned traits, DefinedRegs& regs, uint8_t& flags)
    {
        if (isSpillSlot(arg)) {
            flags |= traits & Mentions::anyDef ? namesSpillSlot | definesSpillSlot : namesSpillSlot;
            return;
        }
        forEachTmpWithTraits(arg, traits,
            [&](Tmp tmp, unsigned tmpTraits) ALWAYS_INLINE_LAMBDA {
                if (!tmp.isReg())
                    return;
                uint64_t bit = static_cast<uint64_t>(1) << tmp.reg().index();
                regs.early |= tmpTraits & Mentions::earlyDef ? bit : 0;
                regs.late |= tmpTraits & Mentions::lateDef ? bit : 0;
            });
    }

    NEVER_INLINE static uint8_t decodeSlowly(Inst& inst, DefinedRegs& regs)
    {
        uint8_t flags = 0;
        inst.forEachArg(
            [&](Arg& arg, Arg::Role role, Bank bank, Width width) {
                noteArg(arg, Mentions::traitsOf(role, bank, width), regs, flags);
            });
        return flags;
    }

    void decode()
    {
        struct Clobbers {
            RegisterSet set;
            uint64_t regs { 0 };

            void update(const RegisterSet& newSet)
            {
                if (newSet == set) [[likely]]
                    return;
                set = newSet;
                regs = 0;
                set.forEachWithWidthAndPreserved(
                    [&](Reg reg, Width, PreservedWidth) {
                        regs |= static_cast<uint64_t>(1) << reg.index();
                    });
            }
        };
        Clobbers early;
        Clobbers late;

        size_t numberOfInsts = 0;
        for (BasicBlock* block : m_code)
            numberOfInsts += block->size();
        m_definedRegs.grow(numberOfInsts);
        m_flags.grow(numberOfInsts);
        DefinedRegs* nextDefinedRegs = m_definedRegs.mutableSpan().data();
        uint8_t* nextFlags = m_flags.mutableSpan().data();
        unsigned ordinal = 0;

        for (BasicBlock* block : m_code) {
            m_firstInstOfBlock[block] = ordinal;
            ordinal += block->size();
            for (Inst& inst : *block) {
                Arg* args = inst.args().data();
                const unsigned numberOfArgs = inst.args().size();
                DefinedRegs regs { 0, 0 };
                uint8_t flags = 0;
                unsigned argIndex = 0;
                if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
                    for (; argIndex < numberOfArgs; ++argIndex)
                        noteArg(args[argIndex], Greedy::formTraits[forms[argIndex]], regs, flags);
                } else
                    flags = decodeSlowly(inst, regs);

                switch (inst.kind.opcode) {
                case Move:
                case Move32:
                    if (args[0].isSomeImm() || (flags & namesSpillSlot))
                        flags |= mayAddAlias;
                    break;
                case MoveFloat:
                case MoveDouble:
                    if (flags & namesSpillSlot)
                        flags |= mayAddAlias;
                    break;
                case Patch:
                    early.update(inst.extraEarlyClobberedRegs());
                    late.update(inst.extraClobberedRegs());
                    regs.early |= early.regs;
                    regs.late |= late.regs;
                    break;
                default:
                    break;
                }
                *nextDefinedRegs++ = regs;
                *nextFlags++ = flags;
            }
        }
    }

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
            const DefinedRegs* definedRegs = m_definedRegs.span().data() + m_firstInstOfBlock[block];
            const uint8_t* flags = m_flags.span().data() + m_firstInstOfBlock[block];
            unsigned blockSize = block->size();
            for (m_instIndex = 0; m_instIndex < blockSize; ++m_instIndex) {
                uint8_t instFlags = flags[m_instIndex];
                if (instFlags & definesSpillSlot) [[unlikely]]
                    clobberDefinedSlots([](Arg::Role role) { return Arg::isAnyDef(role); });
                m_state.clobber(definedRegs[m_instIndex].early | definedRegs[m_instIndex].late);
                if (instFlags & mayAddAlias)
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
            m_state = m_atHead[block];

            const DefinedRegs* definedRegs = m_definedRegs.span().data() + m_firstInstOfBlock[block];
            const uint8_t* flags = m_flags.span().data() + m_firstInstOfBlock[block];
            unsigned blockSize = block->size();
            bool removedAny = false;
            for (m_instIndex = 0; m_instIndex < blockSize; ++m_instIndex) {
                uint8_t instFlags = flags[m_instIndex];
                if (!instFlags) {
                    m_state.clobber(definedRegs[m_instIndex].early | definedRegs[m_instIndex].late);
                    continue;
                }
                if (m_state.isEmpty()) {
                    addInstAliases();
                    continue;
                }
                if (instFlags & definesSpillSlot) [[unlikely]]
                    clobberDefinedSlots([](Arg::Role role) { return Arg::isEarlyDef(role); });
                m_state.clobber(definedRegs[m_instIndex].early);
                fixInst();
                if (!block->at(m_instIndex)) {
                    removedAny = true;
                    continue;
                }
                if (instFlags & definesSpillSlot) [[unlikely]]
                    clobberDefinedSlots([](Arg::Role role) { return Arg::isLateDef(role); });
                m_state.clobber(definedRegs[m_instIndex].late);
                addInstAliases();
            }

            if (removedAny) {
                block->insts().removeAllMatching(
                    [](const Inst& inst) {
                        return !inst;
                    });
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
    void clobberDefinedSlots(NOESCAPE const IsWhichDef& isWhichDef)
    {
        m_block->at(m_instIndex).forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (isWhichDef(role) && isSpillSlot(arg)) {
                dataLogLnIf(AirFixObviousSpillsInternal::verbose, "        Clobbering ", *arg.stackSlot());
                m_state.clobber(arg.stackSlot());
            }
        });
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
                for (uint64_t regs = m_state.constRegs; regs; regs &= regs - 1) {
                    Reg reg = Reg::fromIndex(std::countr_zero(regs));
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

    struct State {
        State() = default;

        State(const State& other)
        {
            *this = other;
        }

        State& operator=(const State& other)
        {
            constRegs = other.constRegs;
            for (uint64_t regs = constRegs; regs; regs &= regs - 1) {
                unsigned index = std::countr_zero(regs);
                constants[index] = other.constants[index];
            }
            if (!slotConst.isEmpty() || !other.slotConst.isEmpty())
                slotConst = other.slotConst;
            if (!regSlot.isEmpty() || !other.regSlot.isEmpty())
                regSlot = other.regSlot;
            regSlotRegs = other.regSlotRegs;
#if ASSERT_ENABLED
            m_isSorted = other.m_isSorted;
#endif
            return *this;
        }

        static uint64_t bitFor(Reg reg) { return static_cast<uint64_t>(1) << reg.index(); }

        bool LEGACY_NODELETE isEmpty() const { return !constRegs && slotConst.isEmpty() && regSlot.isEmpty(); }

        bool LEGACY_NODELETE hasNoSlotAlias() const { return slotConst.isEmpty() && regSlot.isEmpty(); }

        void addAlias(const RegConst& newAlias)
        {
            constRegs |= bitFor(newAlias.reg);
            constants[newAlias.reg.index()] = newAlias.constant;
        }
        void addAlias(const RegSlot& newAlias)
        {
            regSlot.append(newAlias);
            regSlotRegs |= bitFor(newAlias.reg);
#if ASSERT_ENABLED
            m_isSorted = false;
#endif
        }
        void addAlias(const SlotConst& newAlias)
        {
            slotConst.append(newAlias);
#if ASSERT_ENABLED
            m_isSorted = false;
#endif
        }

        bool contains(const RegConst& alias) const
        {
            return constantForReg(alias.reg) == alias.constant;
        }
        bool contains(const RegSlot& alias)
        {
            if (!(regSlotRegs & bitFor(alias.reg)))
                return false;
            return regSlot.contains(alias);
        }
        bool contains(const SlotConst& alias)
        {
            return slotConst.contains(alias);
        }

        std::optional<int64_t> LEGACY_NODELETE constantForReg(Reg reg) const
        {
            if (!(constRegs & bitFor(reg)))
                return std::nullopt;
            return constants[reg.index()];
        }

        const RegSlot* LEGACY_NODELETE getRegSlot(StackSlot* slot) const
        {
            for (const RegSlot& alias : regSlot) {
                if (alias.slot == slot)
                    return &alias;
            }
            return nullptr;
        }

        const SlotConst* LEGACY_NODELETE getSlotConst(StackSlot* slot) const
        {
            for (const SlotConst& alias : slotConst) {
                if (alias.slot == slot)
                    return &alias;
            }
            return nullptr;
        }

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

        void clobber(uint64_t regs)
        {
            constRegs &= ~regs;
            if (!(regSlotRegs & regs)) [[likely]]
                return;
            unsigned matchCount = regSlot.removeAllMatching(
                [&] (const RegSlot& alias) -> bool {
                    return regs & bitFor(alias.reg);
                });
            ASSERT_UNUSED(matchCount, matchCount);
            regSlotRegs &= ~regs;
        }

        void clobber(StackSlot* slot)
        {
            slotConst.removeAllMatching(
                [&] (const SlotConst& alias) -> bool {
                    return alias.slot == slot;
                });
            if (regSlot.removeAllMatching([&] (const RegSlot& alias) -> bool { return alias.slot == slot; }))
                rebuildRegSlotRegs();
        }

        void sort()
        {
            static constexpr size_t minSizeToSortTailSeparately = 64;
            if (slotConst.size() >= minSizeToSortTailSeparately) {
                auto isLess = [](const auto& a, const auto& b) {
                    return a < b;
                };
                auto endOfSorted = std::ranges::is_sorted_until(slotConst, isLess);
                if (endOfSorted != slotConst.end()) {
                    std::ranges::sort(endOfSorted, slotConst.end(), isLess);
                    std::ranges::inplace_merge(slotConst.begin(), endOfSorted, slotConst.end(), isLess);
                }
            } else if (slotConst.size() > 1) {
                std::ranges::sort(slotConst, [](const auto& a, const auto& b) {
                    return a < b;
                });
            }
            if (regSlot.size() > 1) {
                std::ranges::sort(regSlot, [](const auto& a, const auto& b) {
                    return a < b;
                });
            }
#if ASSERT_ENABLED
            m_isSorted = true;
#endif
        }

        // Takes two sorted vectors, for each element in the first, it looks for the first element in the second which is not smaller.
        // If such an element exist, call f on both the element from the first vector and this element.
        // Remove the element from the first vector unless f returned true (so f says whether to keep the element)
        // Returns true if any element has been removed.
        template<typename T, typename Func>
        static bool filterVectorAgainst(Vector<T>& own, const Vector<T>& other, Func f)
        {
            if (own.isEmpty())
                return false;
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

        bool merge(const State& other)
        {
            ASSERT(m_isSorted);
            ASSERT(other.m_isSorted);
            bool changed = false;

            uint64_t survivingConstRegs = 0;
            for (uint64_t regs = constRegs & other.constRegs; regs; regs &= regs - 1) {
                unsigned index = std::countr_zero(regs);
                if (other.constants[index] == constants[index])
                    survivingConstRegs |= static_cast<uint64_t>(1) << index;
            }
            if (survivingConstRegs != constRegs) {
                constRegs = survivingConstRegs;
                changed = true;
            }

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

            if (changed)
                rebuildRegSlotRegs();

            return changed;
        }

        void rebuildRegSlotRegs()
        {
            regSlotRegs = 0;
            for (const RegSlot& alias : regSlot)
                regSlotRegs |= bitFor(alias.reg);
        }

        void dump(PrintStream& out) const
        {
            out.print("{regConst = [");
            CommaPrinter comma;
            for (uint64_t regs = constRegs; regs; regs &= regs - 1)
                out.print(comma, Reg::fromIndex(std::countr_zero(regs)), "->", constants[std::countr_zero(regs)]);
            out.print(
                "], slotConst = [", listDump(slotConst),
                "], regSlot = [", listDump(regSlot), "]}");
        }

        uint64_t constRegs { 0 };
        std::array<int64_t, Reg::maxIndex() + 1> constants;
        Vector<SlotConst> slotConst;
        Vector<RegSlot> regSlot;
        uint64_t regSlotRegs { 0 };
#if ASSERT_ENABLED
        bool m_isSorted { true };
#endif
    };

    Code& m_code;
    IndexMap<BasicBlock*, State> m_atHead;
    IndexMap<BasicBlock*, unsigned> m_firstInstOfBlock;
    Vector<DefinedRegs> m_definedRegs;
    Vector<uint8_t> m_flags;
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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)

