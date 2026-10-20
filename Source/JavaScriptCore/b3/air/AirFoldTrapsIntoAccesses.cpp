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

    bool isInReach(const Arg& arg, Width width) const
    {
        return arg.isAddr() && arg.offset() >= 0 && arg.offset() + static_cast<int64_t>(bytesForWidth(width)) <= m_reach;
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

    bool canTakeTrap(Inst& inst, Tmp base) const
    {
        if (inst.kind.effects || !accessesMemoryFirst(inst.kind.opcode))
            return false;
        unsigned numberOfMemoryArgs = 0;
        bool faultsWithTrap = false;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (!arg.isMemory())
                return;
            ++numberOfMemoryArgs;
            faultsWithTrap = role != Arg::UseAddr && isInReach(arg, width) && arg.base() == base;
        });
        return numberOfMemoryArgs == 1 && faultsWithTrap;
    }

    bool isControlIndependentLoad(Inst& inst) const
    {
        auto* load = inst.origin ? inst.origin->as<MemoryValue>() : nullptr;
        return load && load->isLoad() && inst.args().size() == 2 && !m_code.proc().effectsOf(load).controlDependent;
    }

    bool canRunBeforeTrap(Inst& inst, Tmp base) const
    {
        if (inst.hasNonArgEffects() || mayFault(inst.kind.opcode))
            return false;
        bool mayReadMemory = isControlIndependentLoad(inst);
        bool result = true;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (arg.isMemory() && (!mayReadMemory || role != Arg::Use))
                result = false;
            if (Arg::isAnyDef(role) && arg == Arg(base))
                result = false;
        });
        return result;
    }

    Inst* findAccessTakingTrap(BasicBlock* block, unsigned trapIndex) const
    {
        Tmp base = block->at(trapIndex).args()[0].base();
        for (unsigned index = trapIndex + 1; index < block->size(); ++index) {
            Inst& inst = block->at(index);
            if (canTakeTrap(inst, base))
                return &inst;
            if (!canRunBeforeTrap(inst, base))
                return nullptr;
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
