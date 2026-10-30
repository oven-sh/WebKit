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
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 { namespace Air {

// The phase runs in few functions. Its own size counts for more than its speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace {

class DeadValueReleaser {
public:
    DeadValueReleaser(Code& code, const StackColoring& coloring)
        : m_code(code)
        , m_coloring(coloring)
    {
    }

    void run()
    {
        numberPlaces();
        findPlacesOfValues();
        if (!m_watchedPlaces.isEmpty())
            solveBackward();
        rewrite();
    }

private:
    struct Hint {
        BasicBlock* block;
        unsigned instIndex;
        // The places that hold the value and that are worth clearing.
        BitVector places;
        // The callee lets go. The places are cleared in front of the call.
        bool isCall { false };
    };

    static StackmapValue* hintOf(const Inst& inst)
    {
        if (inst.kind.opcode != Patch || !inst.origin)
            return nullptr;
        auto* stackmap = inst.origin->as<StackmapValue>();
        return stackmap && stackmap->releasesDeadValues() ? stackmap : nullptr;
    }

    static PatchpointValue* callWhoseCalleeLetsGo(const Inst& inst)
    {
        if (inst.kind.opcode != Patch || !inst.origin)
            return nullptr;
        auto* patchpoint = inst.origin->as<PatchpointValue>();
        return patchpoint && !patchpoint->childrenLetGoByCallee().isEmpty() ? patchpoint : nullptr;
    }

    // A stub that keeps nearly every register may still allocate, so the registers that a patchpoint clobbers say nothing.
    static bool mayCollect(const Inst& inst)
    {
        if (inst.kind.opcode == ColdCCall)
            return true;
        if (inst.kind.opcode != Patch || hintOf(inst))
            return false;
        // Also where the call takes the frame down first: the frame of the callee lies over the same words, and it need not write them all.
        if (callWhoseCalleeLetsGo(inst))
            return true;
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

    // Walking backward from the hint.
    struct Trail {
        // The places that hold the value here.
        BitVector holds;
        // The places that are written between here and the hint.
        BitVector rewritten;
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
            UncheckedKeyHashMap<unsigned, Trail, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> atTail;
            Vector<BasicBlock*, 8> worklist;
            auto walk = [&](BasicBlock* block, unsigned end, Trail trail) {
                for (unsigned instIndex = end; instIndex--;) {
                    if (trail.holds.isEmpty())
                        return;
                    grew |= stepBackward(block->at(instIndex), trail, found);
                }
                if (trail.holds.isEmpty())
                    return;
                for (BasicBlock* predecessor : block->predecessors()) {
                    auto added = atTail.add(predecessor->index(), trail);
                    if (!added.isNewEntry) {
                        Trail& known = added.iterator->value;
                        size_t before = known.rewritten.bitCount();
                        known.rewritten.filter(trail.rewritten);
                        if (!mergeInto(known.holds, trail.holds) && known.rewritten.bitCount() == before)
                            continue;
                    }
                    worklist.append(predecessor);
                }
            };
            walk(hintBlock, hintIndex, Trail { found, BitVector(m_numberOfPlaces) });
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                walk(block, block->size(), atTail.get(block->index()));
            }
        }
        return found;
    }

    void findPlacesOfValues()
    {
        m_watchedIndexOfPlace.fill(notWatched, m_numberOfPlaces);
        for (BasicBlock* block : m_code) {
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                Hint hint { block, instIndex, { } };
                if (PatchpointValue* call = callWhoseCalleeLetsGo(inst)) {
                    hint.isCall = true;
                    unsigned firstChild = 1 + m_code.proc().resultCount(call->type());
                    for (unsigned child : call->childrenLetGoByCallee()) {
                        if (auto first = firstChild + child < inst.args().size() ? placeOf(inst.args()[firstChild + child]) : std::nullopt)
                            hint.places.merge(placesHoldingValueAt(block, instIndex, *first));
                    }
                } else if (!hintOf(inst))
                    continue;
                // The value may have become a constant.
                else if (auto first = inst.args().size() == 2 ? placeOf(inst.args()[1]) : std::nullopt)
                    hint.places = placesHoldingValueAt(block, instIndex, *first);
                hint.places.exclude(m_neverCleared);
                for (unsigned place : hint.places) {
                    if (m_watchedIndexOfPlace[place] != notWatched)
                        continue;
                    m_watchedIndexOfPlace[place] = m_watchedPlaces.size();
                    m_watchedPlaces.append(place);
                }
                m_hints.append(WTF::move(hint));
            }
        }
    }

    // Liveness is asked only about the places that some hint would clear. They are few, whatever the size of the function, so a set of them is a word.
    std::optional<unsigned> watchedIndexOf(const Arg& arg) const
    {
        auto place = placeOf(arg);
        if (!place || m_watchedIndexOfPlace[*place] == notWatched)
            return std::nullopt;
        return m_watchedIndexOfPlace[*place];
    }

    struct Backward {
        // Read before it is written.
        BitVector live;
        // Something may collect before it is written.
        BitVector meetsCollection;
        BitVector written;
    };

    // From the state behind the instruction to the state in front of it.
    void stepBackward(Inst& inst, Backward& state) const
    {
        auto write = [&](unsigned index) {
            state.live.quickClear(index);
            state.meetsCollection.quickClear(index);
            state.written.quickSet(index);
        };
        forEachClobberedPlace(inst, [&](unsigned place) {
            if (m_watchedIndexOfPlace[place] != notWatched)
                write(m_watchedIndexOfPlace[place]);
        });
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (auto index = watchedIndexOf(arg); index && Arg::isAnyDef(role) && (arg.isTmp() || width == Width64))
                write(*index);
        });
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (auto index = watchedIndexOf(arg); index && !arg.isTmp() && (Arg::isAnyUse(role) || role == Arg::UseAddr))
                state.live.quickSet(*index);
            forEachWordTouchedInPart(arg, [&](unsigned place) {
                if (m_watchedIndexOfPlace[place] != notWatched)
                    state.live.quickSet(m_watchedIndexOfPlace[place]);
            });
        });
        // This way the registers that addresses are made of count too.
        inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
            if (auto index = watchedIndexOf(Arg(tmp)); index && Arg::isAnyUse(role))
                state.live.quickSet(*index);
        });
        if (mayCollect(inst))
            state.meetsCollection = m_everyWatchedPlace;
    }

    Backward emptyState() const
    {
        unsigned size = m_watchedPlaces.size();
        return { BitVector(size), BitVector(size), BitVector(size) };
    }

    static bool mergeInto(BitVector& target, const BitVector& source)
    {
        size_t before = target.bitCount();
        target.merge(source);
        return target.bitCount() != before;
    }

    void solveBackward()
    {
        m_everyWatchedPlace.ensureSize(m_watchedPlaces.size());
        for (unsigned index = 0; index < m_watchedPlaces.size(); ++index)
            m_everyWatchedPlace.quickSet(index);

        // Every instruction is looked at once. What a block does to the state behind it is three words.
        IndexMap<BasicBlock*, Backward> effectOf(m_code.size());
        m_atTail = IndexMap<BasicBlock*, Backward>(m_code.size());
        for (BasicBlock* block : m_code) {
            Backward effect = emptyState();
            for (unsigned instIndex = block->size(); instIndex--;)
                stepBackward(block->at(instIndex), effect);
            effectOf[block] = WTF::move(effect);
            m_atTail[block] = emptyState();
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned blockIndex = m_code.size(); blockIndex--;) {
                BasicBlock* block = m_code[blockIndex];
                if (!block)
                    continue;
                const Backward& effect = effectOf[block];
                BitVector live = m_atTail[block].live;
                live.exclude(effect.written);
                live.merge(effect.live);
                BitVector meetsCollection = m_atTail[block].meetsCollection;
                meetsCollection.exclude(effect.written);
                meetsCollection.merge(effect.meetsCollection);
                for (BasicBlock* predecessor : block->predecessors()) {
                    changed |= mergeInto(m_atTail[predecessor].live, live);
                    changed |= mergeInto(m_atTail[predecessor].meetsCollection, meetsCollection);
                }
            }
        }
    }

    void rewrite()
    {
        const Arg zero = isX86_64() ? Arg::imm(0) : Arg::zeroReg();
        InsertionSet insertionSet(m_code);
        for (unsigned first = 0; first < m_hints.size();) {
            BasicBlock* block = m_hints[first].block;
            unsigned end = first;
            while (end < m_hints.size() && m_hints[end].block == block)
                ++end;
            // The hints of a block from the last to the first, with the state behind each.
            Backward state = m_watchedPlaces.isEmpty() ? emptyState() : m_atTail[block];
            unsigned instIndex = block->size();
            for (unsigned hintIndex = end; hintIndex-- > first;) {
                Hint& hint = m_hints[hintIndex];
                while (--instIndex > hint.instIndex)
                    stepBackward(block->at(instIndex), state);
                Inst& inst = block->at(instIndex);
                // What the call reads itself stays.
                if (hint.isCall)
                    stepBackward(inst, state);
                unsigned clearedAt = hint.isCall ? instIndex : instIndex + 1;
                RegisterSet registers;
                for (unsigned place : hint.places) {
                    unsigned index = m_watchedIndexOfPlace[place];
                    if (state.live.quickGet(index) || !state.meetsCollection.quickGet(index))
                        continue;
                    // An earlier hint of the block need not clear it again.
                    state.meetsCollection.quickClear(index);
                    if (place >= m_firstCallArg)
                        insertionSet.insert(clearedAt, Move, inst.origin, zero, Arg::callArg(static_cast<int32_t>((place - m_firstCallArg) * sizeof(uint64_t))));
                    else if (place >= m_firstSlot)
                        insertionSet.insert(clearedAt, Move, inst.origin, zero, Arg::stack(m_slotOfWord[place - m_firstSlot]));
                    else
                        registers.add(Reg::fromIndex(place), IgnoreVectors);
                }
                // A move of zero into a register that nothing reads would not last: reportUsedRegisters() removes it.
                StackmapValue* stackmap = inst.origin->as<StackmapValue>();
                stackmap->setRegistersToRelease(registers);
                // A call that runs without the frame leaves the function: nothing behind it reads a register. The registers that the frame saved are the caller's again
                // by then, so it may not say that it clobbers them, and its generator clears only what belongs to nobody.
                if (!hint.isCall || !stackmap->runsWithoutFrame())
                    stackmap->clobberLate(registers);
                if (hint.isCall)
                    continue;
                stepBackward(inst, state);
                if (registers.isEmpty())
                    inst = Inst();
            }
            insertionSet.execute(block);
            block->insts().removeAllMatching([](const Inst& inst) {
                return !inst;
            });
            first = end;
        }
    }

    static constexpr unsigned notWatched = UINT_MAX;

    Code& m_code;
    const StackColoring& m_coloring;
    RegisterSet m_registers;
    Vector<StackSlot*> m_slotOfWord;
    unsigned m_firstSlot { 0 };
    unsigned m_firstCallArg { 0 };
    unsigned m_numberOfPlaces { 0 };
    BitVector m_neverCleared;
    Vector<Hint> m_hints;
    Vector<unsigned> m_watchedPlaces;
    Vector<unsigned> m_watchedIndexOfPlace;
    BitVector m_everyWatchedPlace;
    IndexMap<BasicBlock*, Backward> m_atTail { 0 };
};

} // anonymous namespace

void releaseDeadValues(Code& code, const StackColoring& coloring)
{
    PhaseScope phaseScope(code, "releaseDeadValues"_s);
    DeadValueReleaser(code, coloring).run();
}

#pragma clang attribute pop

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
