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
        , m_writtenAtHead(code.size())
        , m_liveAtTail(code.size())
        , m_reachesCallAtTail(code.size())
    {
    }

    void run()
    {
        numberPlaces();
        solveBackward();
        solveForward();
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
        if (inst.kind.opcode == CCall)
            return true;
        if (inst.kind.opcode != Patch || hintOf(inst))
            return false;
        auto* stackmap = inst.origin ? inst.origin->as<StackmapValue>() : nullptr;
        return !stackmap || !stackmap->runsWithoutFrame();
    }

    void numberPlaces()
    {
        m_registers = RegisterSet::calleeSaveRegisters();
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
        // What is not a spill slot may be read through its address, or by whoever walks the stack.
        m_wordsReadUnseen.ensureSize(m_numberOfPlaces);
        for (StackSlot* slot : m_code.stackSlots()) {
            if (slot->isSpill() || slot->index() >= m_coloring.unitsOfSlot.size())
                continue;
            if (auto units = m_coloring.unitsOfSlot[slot->index()]) {
                for (unsigned word = units->first / StackColoring::unitsPerWord; word <= units->last / StackColoring::unitsPerWord; ++word)
                    m_wordsReadUnseen.quickSet(m_firstSlot + word);
            }
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

    static bool storesConstant(const Inst& inst)
    {
        return (inst.kind.opcode == Move || inst.kind.opcode == Move32) && inst.args().size() == 2 && (inst.args()[0].isSomeImm() || inst.args()[0].isZeroReg());
    }

    // `written`: the places that may hold a whole word which this function has put there and which is no constant.
    void stepForward(Inst& inst, BitVector& written)
    {
        forEachClobberedPlace(inst, [&](unsigned place) {
            written.quickClear(place);
        });
        bool copiesWord = inst.kind.opcode == Move && inst.args().size() == 2;
        std::optional<unsigned> source = copiesWord ? placeOf(inst.args()[0]) : std::nullopt;
        bool mayNameCell = !storesConstant(inst) && (!source || written.quickGet(*source));
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank bank, Width width) {
            forEachWordTouchedInPart(arg, [&](unsigned word) {
                written.quickClear(word);
            });
            auto place = placeOf(arg);
            if (!place || !Arg::isAnyDef(role))
                return;
            written.quickSet(*place, mayNameCell && bank == GP && width == Width64);
            if (arg.isStack())
                m_slotOfWord[*place - m_firstSlot] = arg.stackSlot();
        });
    }

    // What the hint at that place of the block leaves alone: what is read again, and what is written again before anything is called.
    BitVector placesKeptAt(BasicBlock* block, unsigned hintIndex) const
    {
        BitVector live = m_liveAtTail[block];
        BitVector reachesCall = m_reachesCallAtTail[block];
        for (unsigned instIndex = block->size(); --instIndex > hintIndex;)
            stepBackward(block->at(instIndex), live, reachesCall);
        BitVector kept(m_numberOfPlaces);
        for (unsigned place = 0; place < m_numberOfPlaces; ++place)
            kept.quickSet(place, live.quickGet(place) || !reachesCall.quickGet(place));
        return kept;
    }

    template<typename Func>
    void walkForward(BasicBlock* block, BitVector& written, const Func& atHint)
    {
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);
            if (!hintOf(inst)) {
                stepForward(inst, written);
                continue;
            }
            BitVector kept = placesKeptAt(block, instIndex);
            BitVector released = written;
            released.exclude(kept);
            released.exclude(m_wordsReadUnseen);
            written.filter(kept);
            atHint(instIndex, released);
        }
    }

    void solveForward()
    {
        for (BasicBlock* block : m_code)
            m_writtenAtHead[block].ensureSize(m_numberOfPlaces);
        bool changed = true;
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_code) {
                BitVector written = m_writtenAtHead[block];
                walkForward(block, written, [](unsigned, const BitVector&) { });
                for (BasicBlock* successor : block->successorBlocks())
                    changed |= mergeInto(m_writtenAtHead[successor], written);
            }
        }
    }

    void rewrite()
    {
        const Arg zero = isX86_64() ? Arg::imm(0) : Arg::zeroReg();
        InsertionSet insertionSet(m_code);
        for (BasicBlock* block : m_code) {
            BitVector written = m_writtenAtHead[block];
            bool removesHint = false;
            walkForward(block, written, [&](unsigned instIndex, const BitVector& released) {
                Inst& inst = block->at(instIndex);
                RegisterSet registers;
                for (unsigned place : released) {
                    if (place < m_firstSlot)
                        registers.add(Reg::fromIndex(place), IgnoreVectors);
                    else if (place < m_firstCallArg)
                        insertionSet.insert(instIndex + 1, Move, inst.origin, zero, Arg::stack(m_slotOfWord[place - m_firstSlot]));
                    else
                        insertionSet.insert(instIndex + 1, Move, inst.origin, zero, Arg::callArg(static_cast<int32_t>((place - m_firstCallArg) * sizeof(uint64_t))));
                }
                hintOf(inst)->setRegistersToRelease(registers);
                hintOf(inst)->clobberLate(registers);
                if (registers.isEmpty()) {
                    inst = Inst();
                    removesHint = true;
                }
            });
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
    BitVector m_wordsReadUnseen;
    IndexMap<BasicBlock*, BitVector> m_writtenAtHead;
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
