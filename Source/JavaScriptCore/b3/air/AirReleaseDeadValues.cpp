/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirReleaseDeadValues.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "AirStackColoring.h"
#include "B3StackmapValue.h"
#include <wtf/BitVector.h>
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

class DeadValueReleaser {
public:
    DeadValueReleaser(Code& code, const StackColoring& coloring)
        : m_code(code)
        , m_coloring(coloring)
        , m_liveAtTail(code.size())
        , m_reachesCallAtTail(code.size())
    {
    }

    void run()
    {
        numberPlaces();
        solveBackward();
        rewrite();
    }

private:
    static StackmapValue* hintOf(const Inst& inst)
    {
        if (inst.kind.opcode != Patch || !inst.origin)
            return nullptr;
        auto* stackmap = inst.origin->as<StackmapValue>();
        return stackmap && stackmap->releasesDeadValues() ? stackmap : nullptr;
    }

    // A stub that keeps nearly every register may still allocate, so the registers that a patchpoint clobbers say nothing.
    static bool mayCollect(const Inst& inst)
    {
        if (inst.kind.opcode == ColdCCall)
            return true;
        if (inst.kind.opcode != Patch || hintOf(inst))
            return false;
        auto* stackmap = inst.origin ? inst.origin->as<StackmapValue>() : nullptr;
        return !stackmap || !stackmap->runsWithoutFrame();
    }

    void numberPlaces()
    {
        // The value is followed through every register. Only one that a callee may store is worth clearing.
        m_registers = RegisterSet::allGPRs();
        m_registers.exclude(RegisterSet(m_code.pinnedRegisters()));
        m_registers.remove(MacroAssembler::framePointerRegister);
        m_registers.remove(MacroAssembler::stackPointerRegister);
        m_firstSlot = Reg::maxIndex() + 1;
        m_firstCallArg = m_firstSlot + m_coloring.numberOfWords();
        m_slotOfWord.fill(nullptr, m_coloring.numberOfWords());
        m_numberOfPlaces = m_firstCallArg + m_code.callArgAreaSizeInBytes() / sizeof(uint64_t);
        m_everyPlace.ensureSize(m_numberOfPlaces);
        for (unsigned place = 0; place < m_numberOfPlaces; ++place)
            m_everyPlace.quickSet(place);
        m_neverCleared.ensureSize(m_numberOfPlaces);
        m_registers.forEach([&](Reg reg) {
            if (!RegisterSet::calleeSaveRegisters().contains(reg, IgnoreVectors))
                m_neverCleared.quickSet(reg.index());
        });
        for (StackSlot* slot : m_code.stackSlots()) {
            if (slot->index() >= m_coloring.unitsOfSlot.size())
                continue;
            auto units = m_coloring.unitsOfSlot[slot->index()];
            if (!units)
                continue;
            if (auto place = placeOf(Arg::stack(slot))) {
                m_slotOfWord[*place - m_firstSlot] = slot;
                continue;
            }
            if (slot->isSpill())
                continue;
            // What is not a spill slot may be read through its address, or by whoever walks the stack.
            for (unsigned word = units->first / StackColoring::unitsPerWord; word <= units->last / StackColoring::unitsPerWord; ++word)
                m_neverCleared.quickSet(m_firstSlot + word);
        }
    }

    std::optional<unsigned> placeOf(const Arg& arg) const
    {
        if (arg.isTmp()) {
            if (!arg.tmp().isReg() || !arg.tmp().isGP() || !m_registers.contains(arg.tmp().reg(), IgnoreVectors))
                return std::nullopt;
            return arg.tmp().reg().index();
        }
        if (arg.isStack()) {
            // Slots that are never live together share their words, so a place is a word of the coloring.
            auto units = unitsOf(arg);
            if (!units || !arg.stackSlot()->isSpill() || units->first % StackColoring::unitsPerWord || units->last != units->first + 1 || arg.offset())
                return std::nullopt;
            return m_firstSlot + units->first / StackColoring::unitsPerWord;
        }
        if (arg.isCallArg()) {
            if (arg.offset() < 0 || arg.offset() % sizeof(uint64_t) || static_cast<uint64_t>(arg.offset()) >= m_code.callArgAreaSizeInBytes())
                return std::nullopt;
            return m_firstCallArg + static_cast<unsigned>(arg.offset() / sizeof(uint64_t));
        }
        return std::nullopt;
    }

    std::optional<StackColoring::UnitRange> unitsOf(const Arg& arg) const
    {
        unsigned index = arg.stackSlot()->index();
        return index < m_coloring.unitsOfSlot.size() ? m_coloring.unitsOfSlot[index] : std::nullopt;
    }

    // Of a slot that is no whole word by itself, or that is entered in the middle.
    template<typename Func>
    void forEachWordTouchedInPart(const Arg& arg, const Func& func) const
    {
        if (placeOf(arg))
            return;
        if (arg.isCallArg() && arg.offset() >= 0 && static_cast<uint64_t>(arg.offset()) < m_code.callArgAreaSizeInBytes())
            func(m_firstCallArg + static_cast<unsigned>(arg.offset() / sizeof(uint64_t)));
        if (!arg.isStack())
            return;
        if (auto units = unitsOf(arg)) {
            for (unsigned word = units->first / StackColoring::unitsPerWord; word <= units->last / StackColoring::unitsPerWord; ++word)
                func(m_firstSlot + word);
        }
    }

    template<typename Func>
    void forEachClobberedPlace(Inst& inst, const Func& func) const
    {
        if (inst.kind.opcode != Patch)
            return;
        auto visit = [&](Reg reg) {
            if (m_registers.contains(reg, IgnoreVectors))
                func(reg.index());
        };
        inst.extraEarlyClobberedRegs().forEach(visit);
        inst.extraClobberedRegs().forEach(visit);
    }

    // From the state behind the instruction to the state in front of it.
    void stepBackward(Inst& inst, BitVector& live, BitVector& reachesCall) const
    {
        auto define = [&](unsigned place) {
            live.quickClear(place);
            reachesCall.quickClear(place);
        };
        forEachClobberedPlace(inst, define);
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (auto place = placeOf(arg); place && Arg::isAnyDef(role) && (arg.isTmp() || width == Width64))
                define(*place);
        });
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (auto place = placeOf(arg); place && !arg.isTmp() && (Arg::isAnyUse(role) || role == Arg::UseAddr))
                live.quickSet(*place);
            forEachWordTouchedInPart(arg, [&](unsigned word) {
                live.quickSet(word);
            });
        });
        // This way the registers that addresses are made of count too.
        inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
            if (auto place = placeOf(Arg(tmp)); place && Arg::isAnyUse(role))
                live.quickSet(*place);
        });
        if (mayCollect(inst))
            reachesCall = m_everyPlace;
    }

    static bool mergeInto(BitVector& target, const BitVector& source)
    {
        size_t before = target.bitCount();
        target.merge(source);
        return target.bitCount() != before;
    }

    void solveBackward()
    {
        for (BasicBlock* block : m_code) {
            m_liveAtTail[block].ensureSize(m_numberOfPlaces);
            m_reachesCallAtTail[block].ensureSize(m_numberOfPlaces);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (unsigned blockIndex = m_code.size(); blockIndex--;) {
                BasicBlock* block = m_code[blockIndex];
                if (!block)
                    continue;
                BitVector live = m_liveAtTail[block];
                BitVector reachesCall = m_reachesCallAtTail[block];
                for (unsigned instIndex = block->size(); instIndex--;)
                    stepBackward(block->at(instIndex), live, reachesCall);
                for (BasicBlock* predecessor : block->predecessors()) {
                    changed |= mergeInto(m_liveAtTail[predecessor], live);
                    changed |= mergeInto(m_reachesCallAtTail[predecessor], reachesCall);
                }
            }
        }
    }

    // Walking backward from the hint.
    struct Trail {
        // The places that hold the value here.
        BitVector holds;
        // The places that are written between here and the hint.
        BitVector rewritten;
        bool isReached { true };
    };

    // `found`: the places that hold the value at the hint.
    bool stepBackward(Inst& inst, Trail& trail, BitVector& found) const
    {
        auto write = [&](unsigned place) {
            trail.holds.quickClear(place);
            trail.rewritten.quickSet(place);
        };
        bool grew = false;
        if (inst.kind.opcode == Move && inst.args().size() >= 2 && placeOf(inst.args()[1])) {
            std::optional<unsigned> source = placeOf(inst.args()[0]);
            unsigned target = *placeOf(inst.args()[1]);
            bool targetHolds = trail.holds.quickGet(target);
            if (!targetHolds && source && trail.holds.quickGet(*source) && !trail.rewritten.quickGet(target))
                grew |= !found.quickSet(target);
            write(target);
            if (inst.args().size() == 3) {
                if (auto scratch = placeOf(inst.args()[2]))
                    write(*scratch);
            }
            if (targetHolds && source) {
                trail.holds.quickSet(*source);
                if (!trail.rewritten.quickGet(*source))
                    grew |= !found.quickSet(*source);
            }
            return grew;
        }
        forEachClobberedPlace(inst, write);
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (!Arg::isAnyDef(role))
                return;
            if (auto place = placeOf(arg))
                write(*place);
            forEachWordTouchedInPart(arg, write);
        });
        return false;
    }

    // Every place that a move has put the value into, or taken it from, and that nothing has written since.
    BitVector placesHoldingValueAt(BasicBlock* hintBlock, unsigned hintIndex, unsigned first) const
    {
        BitVector found(m_numberOfPlaces);
        found.quickSet(first);
        // A copy of a copy shows only once the first copy is known to hold the value.
        for (bool grew = true; grew;) {
            grew = false;
            IndexMap<BasicBlock*, Trail> atTail(m_code.size(), Trail { { }, { }, false });
            Vector<BasicBlock*, 8> worklist;
            auto walk = [&](BasicBlock* block, unsigned end, Trail trail) {
                for (unsigned instIndex = end; instIndex--;) {
                    if (trail.holds.isEmpty())
                        return;
                    grew |= stepBackward(block->at(instIndex), trail, found);
                }
                for (BasicBlock* predecessor : block->predecessors()) {
                    Trail& known = atTail[predecessor];
                    if (!known.isReached)
                        known = trail;
                    else {
                        size_t before = known.rewritten.bitCount();
                        known.rewritten.filter(trail.rewritten);
                        if (!mergeInto(known.holds, trail.holds) && known.rewritten.bitCount() == before)
                            continue;
                    }
                    worklist.append(predecessor);
                }
            };
            walk(hintBlock, hintIndex, Trail { found, BitVector(m_numberOfPlaces), true });
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                walk(block, block->size(), atTail[block]);
            }
        }
        return found;
    }

    // What the hint at that place of the block leaves alone: what is read again, and what is written again before anything may collect.
    BitVector placesKeptAt(BasicBlock* block, unsigned hintIndex) const
    {
        BitVector live = m_liveAtTail[block];
        BitVector reachesCall = m_reachesCallAtTail[block];
        for (unsigned instIndex = block->size(); --instIndex > hintIndex;)
            stepBackward(block->at(instIndex), live, reachesCall);
        BitVector kept = m_neverCleared;
        for (unsigned place = 0; place < m_numberOfPlaces; ++place) {
            if (live.quickGet(place) || !reachesCall.quickGet(place))
                kept.quickSet(place);
        }
        return kept;
    }

    void rewrite()
    {
        const Arg zero = isX86_64() ? Arg::imm(0) : Arg::zeroReg();
        InsertionSet insertionSet(m_code);
        for (BasicBlock* block : m_code) {
            bool removesHint = false;
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                StackmapValue* hint = hintOf(inst);
                if (!hint)
                    continue;
                RegisterSet registers;
                // The value may have become a constant.
                if (auto first = inst.args().size() == 2 ? placeOf(inst.args()[1]) : std::nullopt) {
                    BitVector released = placesHoldingValueAt(block, instIndex, *first);
                    released.exclude(placesKeptAt(block, instIndex));
                    for (unsigned place : released) {
                        if (place < m_firstSlot)
                            registers.add(Reg::fromIndex(place), IgnoreVectors);
                        else if (place < m_firstCallArg)
                            insertionSet.insert(instIndex + 1, Move, inst.origin, zero, Arg::stack(m_slotOfWord[place - m_firstSlot]));
                        else
                            insertionSet.insert(instIndex + 1, Move, inst.origin, zero, Arg::callArg(static_cast<int32_t>((place - m_firstCallArg) * sizeof(uint64_t))));
                    }
                }
                hint->setRegistersToRelease(registers);
                hint->clobberLate(registers);
                if (registers.isEmpty()) {
                    inst = Inst();
                    removesHint = true;
                }
            }
            insertionSet.execute(block);
            if (removesHint) {
                block->insts().removeAllMatching([](const Inst& inst) {
                    return !inst;
                });
            }
        }
    }

    Code& m_code;
    const StackColoring& m_coloring;
    RegisterSet m_registers;
    Vector<StackSlot*> m_slotOfWord;
    unsigned m_firstSlot { 0 };
    unsigned m_firstCallArg { 0 };
    unsigned m_numberOfPlaces { 0 };
    BitVector m_everyPlace;
    BitVector m_neverCleared;
    IndexMap<BasicBlock*, BitVector> m_liveAtTail;
    IndexMap<BasicBlock*, BitVector> m_reachesCallAtTail;
};

} // anonymous namespace

void releaseDeadValues(Code& code, const StackColoring& coloring)
{
    PhaseScope phaseScope(code, "releaseDeadValues"_s);
    DeadValueReleaser(code, coloring).run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
