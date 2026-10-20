/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirForwardRegisterCopies.h"

#if ENABLE(B3_JIT)

#include "AirCFG.h"
#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include <wtf/GraphOrdering.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

constexpr unsigned numberOfRegisters = MacroAssembler::numberOfRegisters() + MacroAssembler::numberOfFPRegisters();
static_assert(numberOfRegisters <= 64);

constexpr uint64_t bit(Reg reg)
{
    return 1ULL << reg.index();
}

struct Copies {
    Reg sourceOf(Reg reg) const
    {
        uint8_t source = m_sourceOf[reg.index()];
        return source ? Reg::fromIndex(source - 1) : Reg();
    }

    void add(Reg copy, Reg source)
    {
        m_sourceOf[copy.index()] = source.index() + 1;
        m_inlinedCopies |= bit(copy);
        m_sources |= bit(source);
    }

    void forget(uint64_t changed)
    {
        m_zeroExtended &= ~changed;
        for (uint64_t copies = m_inlinedCopies & changed; copies; copies &= copies - 1)
            m_sourceOf[WTF::ctz(copies)] = 0;
        m_inlinedCopies &= ~changed;
        if (!(m_sources & changed))
            return;
        m_sources = 0;
        for (uint64_t copies = m_inlinedCopies; copies; copies &= copies - 1) {
            unsigned copy = WTF::ctz(copies);
            uint64_t source = 1ULL << (m_sourceOf[copy] - 1);
            if (source & changed) {
                m_sourceOf[copy] = 0;
                m_inlinedCopies &= ~(1ULL << copy);
            } else
                m_sources |= source;
        }
    }

    void meet(const Copies& other)
    {
        m_zeroExtended &= other.m_zeroExtended;
        uint64_t sources = 0;
        for (uint64_t copies = m_inlinedCopies; copies; copies &= copies - 1) {
            unsigned copy = WTF::ctz(copies);
            if (m_sourceOf[copy] != other.m_sourceOf[copy]) {
                m_sourceOf[copy] = 0;
                m_inlinedCopies &= ~(1ULL << copy);
            } else
                sources |= 1ULL << (m_sourceOf[copy] - 1);
        }
        m_sources = sources;
    }

    bool isZeroExtended(Reg reg) const { return m_zeroExtended & bit(reg); }
    void setZeroExtended(uint64_t registers) { m_zeroExtended |= registers; }
    uint64_t copies() const { return m_inlinedCopies; }
    uint64_t named() const { return m_inlinedCopies | m_sources | m_zeroExtended; }

private:
    std::array<uint8_t, numberOfRegisters> m_sourceOf { };
    uint64_t m_inlinedCopies { 0 };
    uint64_t m_sources { 0 };
    uint64_t m_zeroExtended { 0 };
};

} // anonymous namespace

void forwardRegisterCopies(Code& code)
{
    PhaseScope phaseScope(code, "forwardRegisterCopies"_s);

    Vector<BasicBlock*, 64> postOrder;
    appendNodesInOrder(code.cfg(), GraphOrder::PostOrder, postOrder);
    Vector<Copies> atTail(FillWith { }, code.size(), Copies { });
    BitVector isDone(code.size());

    for (unsigned i = postOrder.size(); i--;) {
        BasicBlock* block = postOrder[i];
        Copies copies;
        bool isFirst = true;
        for (BasicBlock* predecessor : block->predecessors()) {
            if (!isDone.quickGet(predecessor->index()) || block == code[0]) {
                copies = Copies { };
                break;
            }
            if (isFirst)
                copies = atTail[predecessor->index()];
            else
                copies.meet(atTail[predecessor->index()]);
            isFirst = false;
        }
        bool didRemove = false;
        for (Inst& inst : *block) {
            bool isBetweenRegisters = inst.args().size() == 2 && inst.args()[0].isReg() && inst.args()[1].isReg();
            if (isBetweenRegisters && (inst.kind.opcode == Move || inst.kind.opcode == MoveDouble || (inst.kind.opcode == Move32 && copies.isZeroExtended(inst.args()[0].reg())))) {
                Reg source = inst.args()[0].reg();
                Reg copy = inst.args()[1].reg();
                bool isZeroExtended = copies.isZeroExtended(source);
                if (Reg original = copies.sourceOf(source))
                    source = original;
                if (source == copy || copies.sourceOf(copy) == source) {
                    inst = Inst();
                    didRemove = true;
                    continue;
                }
                inst.args()[0] = Tmp(source);
                copies.forget(bit(copy));
                if (source != Reg(MacroAssembler::stackPointerRegister))
                    copies.add(copy, source);
                if (isZeroExtended)
                    copies.setZeroExtended(bit(copy));
                continue;
            }
            bool isPatch = inst.kind.opcode == Patch;
            if (!isPatch && !mayZeroExtendWord(inst.kind.opcode)) {
                uint64_t named = 0;
                inst.forEachTmpFast([&](Tmp& tmp) {
                    named |= bit(tmp.reg());
                });
                if (!(named & copies.named()))
                    continue;
            }
            uint64_t uses = 0;
            uint64_t definedEarly = 0;
            uint64_t defined = 0;
            uint64_t zeroExtended = 0;
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank bank, Width width) {
                if (Arg::isAnyUse(role))
                    uses |= bit(tmp.reg());
                if (Arg::isEarlyDef(role))
                    definedEarly |= bit(tmp.reg());
                if (Arg::isAnyDef(role)) {
                    defined |= bit(tmp.reg());
                    if (Arg::isZDef(role) && bank == GP && width <= Width32)
                        zeroExtended |= bit(tmp.reg());
                }
            });
            if (isPatch) {
                zeroExtended = 0;
                inst.extraEarlyClobberedRegs().forEach([&](Reg reg) {
                    defined |= bit(reg);
                    definedEarly |= bit(reg);
                });
                inst.extraClobberedRegs().forEach([&](Reg reg) {
                    defined |= bit(reg);
                });
                uint64_t kept = 0;
                uint64_t fixed = 0;
                bool isLoweredFromOrigin = inst.origin && !inst.clobbersAtBlockEntry();
                if (auto* patchpoint = isLoweredFromOrigin ? inst.origin->as<PatchpointValue>() : nullptr) {
                    unsigned first = 1 + code.proc().resultCount(patchpoint->type());
                    for (unsigned child = 0; child < patchpoint->numChildren(); ++child) {
                        Arg& arg = inst.args()[first + child];
                        if (!arg.isReg())
                            continue;
                        switch (patchpoint->constrainedChild(child).rep().kind()) {
                        case ValueRep::SomeRegister:
                        case ValueRep::WarmAny:
                        case ValueRep::ColdAny:
                            kept |= bit(arg.reg());
                            if (Reg source = copies.sourceOf(arg.reg()); source && !(definedEarly & bit(source)))
                                arg = Tmp(source);
                            kept |= bit(arg.reg());
                            break;
                        case ValueRep::SomeLateRegister:
                        case ValueRep::LateColdAny:
                            kept |= bit(arg.reg());
                            break;
                        default:
                            fixed |= bit(arg.reg());
                            break;
                        }
                    }
                }
                defined |= uses & ~(kept & ~fixed);
            } else if ((uses & copies.copies()) && !isReturn(inst.kind.opcode)) {
                Inst original = inst;
                bool didChange = false;
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (role != Arg::Use && role != Arg::ColdUse)
                        return;
                    Reg source = copies.sourceOf(tmp.reg());
                    if (!source || (definedEarly & bit(source)))
                        return;
                    tmp = Tmp(source);
                    didChange = true;
                });
                if (didChange && !inst.isValidForm(code.targetFeatures()))
                    inst = original;
            }
            copies.forget(defined);
            copies.setZeroExtended(zeroExtended);
        }
        if (didRemove)
            block->insts().removeAllMatching([](const Inst& inst) { return !inst; });
        atTail[block->index()] = copies;
        isDone.quickSet(block->index());
    }
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
