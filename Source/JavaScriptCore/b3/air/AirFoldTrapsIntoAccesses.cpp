/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirFoldTrapsIntoAccesses.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"
#include "AirTmpInlines.h"
#include "AirTmpMap.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3Value.h"
#include <algorithm>

namespace JSC { namespace B3 { namespace Air {

namespace {

class TrapFolder {
public:
    explicit TrapFolder(Code& code)
        : m_code(code)
        , m_reach(code.policy().peepholes.trapReach)
    {
    }

    bool run()
    {
        if (!canReadOrigins() || !hasTrappingLoad())
            return false;

        countMentions();
        bool changed = false;
        for (BasicBlock* block : m_code)
            changed |= foldTrapsIn(block);
        return changed;
    }

private:
    bool canReadOrigins() const
    {
        return m_code.shouldPreserveB3Origins() || m_code.proc().policy().allocation.keepsValuesAfterLowering;
    }

    // What the offset of an access is added to: `[base + offset]`, `[base + scale * index + offset]`.
    struct Reference {
        Tmp base;
        Tmp index;
        unsigned scale { 0 };

        static Reference of(const Arg& arg)
        {
            if (arg.isIndex())
                return { arg.base(), arg.index(), arg.scale() };
            return { arg.base(), Tmp(), 0 };
        }

        bool isMadeFrom(const Arg& arg) const { return arg == Arg(base) || (index && arg == Arg(index)); }

        friend bool operator==(const Reference&, const Reference&) = default;
    };

    bool isInReach(const Arg& arg, Width width) const
    {
        return (arg.isAddr() || arg.isIndex()) && arg.offset() >= 0 && arg.offset() + static_cast<int64_t>(bytesForWidth(width)) <= m_reach;
    }

    bool isTrappingLoad(Inst& inst) const
    {
        if (!inst.kind.effects || !inst.origin || !inst.origin->traps() || inst.args().size() != 2)
            return false;
        bool loadsIntoTmp = true;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (arg.isMemory())
                loadsIntoTmp &= role == Arg::Use && isInReach(arg, width);
            else
                loadsIntoTmp &= arg.isTmp() && !arg.isReg() && Arg::isAnyDef(role) && !Arg::isAnyUse(role);
        });
        return loadsIntoTmp && inst.args()[0].isMemory();
    }

    bool hasTrappingLoad()
    {
        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                if (isTrappingLoad(inst))
                    return true;
            }
        }
        return false;
    }

    void countMentions()
    {
        constexpr uint8_t several = 2;
        m_numberOfMentions = TmpMap<uint8_t>(m_code, static_cast<uint8_t>(0));
        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                inst.forEachTmpFast([&](Tmp& tmp) {
                    uint8_t& count = m_numberOfMentions[tmp];
                    count = std::min<uint8_t>(count + 1, several);
                });
            }
        }
    }

    bool checksAddressOnly(Inst& inst) const
    {
        return isTrappingLoad(inst) && m_numberOfMentions[inst.args()[1].tmp()] == 1;
    }

    bool canTakeTrap(Inst& inst, const Reference& reference) const
    {
        if (inst.kind.effects || !accessesMemoryFirst(inst.kind.opcode))
            return false;
        unsigned numberOfMemoryArgs = 0;
        bool faultsWithTrap = false;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (!arg.isMemory())
                return;
            ++numberOfMemoryArgs;
            faultsWithTrap = role != Arg::UseAddr && isInReach(arg, width) && Reference::of(arg) == reference;
        });
        return numberOfMemoryArgs == 1 && faultsWithTrap;
    }

    bool isControlIndependentLoad(Inst& inst) const
    {
        auto* load = inst.origin ? inst.origin->as<MemoryValue>() : nullptr;
        return load && load->isLoad() && inst.args().size() == 2 && !load->effects().controlDependent;
    }

    bool canRunBeforeTrap(Inst& inst, const Reference& reference) const
    {
        if (inst.hasNonArgEffects() || mayFault(inst.kind.opcode))
            return false;
        bool mayReadMemory = isControlIndependentLoad(inst);
        bool result = true;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (arg.isMemory() && (!mayReadMemory || role != Arg::Use))
                result = false;
            if (Arg::isAnyDef(role) && reference.isMadeFrom(arg))
                result = false;
        });
        return result;
    }

    bool onlyChoosesSuccessor(Inst& inst, const Reference& reference) const
    {
        if (!inst.isTerminal() || inst.kind.effects || inst.kind.opcode == Patch || mayFault(inst.kind.opcode))
            return false;
        bool result = true;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (arg.isMemory() || (Arg::isAnyDef(role) && reference.isMadeFrom(arg)))
                result = false;
        });
        return result;
    }

    struct ScanResult {
        Inst* access { nullptr };
        bool reachedEnd { false };
    };

    ScanResult scan(BasicBlock* block, unsigned begin, const Reference& reference) const
    {
        for (unsigned index = begin; index < block->size(); ++index) {
            Inst& inst = block->at(index);
            if (canTakeTrap(inst, reference))
                return { &inst, false };
            if (index + 1 == block->size() && onlyChoosesSuccessor(inst, reference))
                return { nullptr, true };
            if (!canRunBeforeTrap(inst, reference))
                return { };
        }
        return { };
    }

    BasicBlock* findJoinOfAllPathsFrom(BasicBlock* block, const Reference& reference) const
    {
        BasicBlock* join = nullptr;
        for (BasicBlock* successor : block->successorBlocks()) {
            BasicBlock* target = successor;
            if (successor->numPredecessors() == 1 && successor->numSuccessors() == 1) {
                ScanResult arm = scan(successor, 0, reference);
                if (!arm.reachedEnd)
                    return nullptr;
                target = successor->successorBlock(0);
            }
            if (join && join != target)
                return nullptr;
            join = target;
        }
        if (!join || join == block || join->numPredecessors() != block->numSuccessors())
            return nullptr;
        return join;
    }

    Inst* findAccessTakingTrap(BasicBlock* block, unsigned trapIndex) const
    {
        constexpr unsigned maxNumberOfJoins = 4;
        Reference reference = Reference::of(block->at(trapIndex).args()[0]);
        unsigned begin = trapIndex + 1;
        for (unsigned numberOfJoins = 0; block && numberOfJoins <= maxNumberOfJoins; ++numberOfJoins) {
            ScanResult result = scan(block, begin, reference);
            if (!result.reachedEnd)
                return result.access;
            block = findJoinOfAllPathsFrom(block, reference);
            begin = 0;
        }
        return nullptr;
    }

    bool foldTrapsIn(BasicBlock* block)
    {
        bool changed = false;
        for (unsigned index = 0; index < block->size(); ++index) {
            Inst& trap = block->at(index);
            if (!checksAddressOnly(trap))
                continue;
            Inst* access = findAccessTakingTrap(block, index);
            if (!access)
                continue;
            access->kind.effects = true;
            access->origin = trap.origin;
            trap = Inst();
            m_code.proc().count(Counter::TrapsFoldedIntoAccesses);
            changed = true;
        }
        if (changed) {
            block->insts().removeAllMatching([](const Inst& inst) {
                return !inst;
            });
        }
        return changed;
    }

    Code& m_code;
    int64_t m_reach;
    TmpMap<uint8_t> m_numberOfMentions;
};

} // anonymous namespace

bool foldTrapsIntoAccesses(Code& code)
{
    PhaseScope phaseScope(code, "foldTrapsIntoAccesses"_s);
    return TrapFolder(code).run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
