/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirReleaseDeadValues.h"

#if ENABLE(B3_JIT)

#include "AirCFG.h"
#include "AirCode.h"
#include "AirDominators.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "AirStackColoring.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include <wtf/BitVector.h>
#include <wtf/GraphOrdering.h>
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
        m_code.proc().count(Counter::StepsToReleaseDeadValues, static_cast<uint32_t>(std::min<uint64_t>(m_steps, std::numeric_limits<uint32_t>::max())));
    }

private:
    struct Hint {
        BasicBlock* block;
        unsigned instIndex;
        // The places that hold the value and that are worth clearing.
        BitVector places;
        // The callee lets go. The places are cleared in front of the call.
        bool isCall { false };
        Vector<unsigned, 1> named { };
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

    static constexpr unsigned noPlace = UINT_MAX;

    template<typename Func>
    void forEachWrite(Inst& inst, const Func& func) const
    {
        if (inst.kind.opcode == Move && inst.args().size() >= 2 && placeOf(inst.args()[1])) {
            func(*placeOf(inst.args()[1]), placeOf(inst.args()[0]).value_or(noPlace));
            if (inst.args().size() == 3) {
                if (auto scratch = placeOf(inst.args()[2]))
                    func(*scratch, noPlace);
            }
            return;
        }
        forEachClobberedPlace(inst, [&](unsigned place) {
            func(place, noPlace);
        });
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (!Arg::isAnyDef(role))
                return;
            if (auto place = placeOf(arg))
                func(*place, noPlace);
            forEachWordTouchedInPart(arg, [&](unsigned place) {
                func(place, noPlace);
            });
        });
    }

    struct Write {
        unsigned instIndex;
        unsigned place;
        unsigned source;
    };

    struct Join {
        unsigned block;
        unsigned place;
    };

    unsigned nameOfWrite(size_t index) const { return m_numberOfPlaces + index; }
    unsigned nameOfJoin(size_t index) const { return m_numberOfPlaces + m_writes.size() + index; }
    unsigned numberOfNames() const { return nameOfJoin(m_joins.size()); }

    unsigned placeOfName(unsigned name) const
    {
        if (name < m_numberOfPlaces)
            return name;
        name -= m_numberOfPlaces;
        return name < m_writes.size() ? m_writes[name].place : m_joins[name - m_writes.size()].place;
    }

    struct Lists {
        Vector<unsigned> first;
        Vector<unsigned> members;

        std::span<const unsigned> of(unsigned key) const LIFETIME_BOUND { return members.span().subspan(first[key], first[key + 1] - first[key]); }

        template<typename Pairs, typename KeyOf, typename MemberOf>
        void fill(unsigned count, const Pairs& pairs, const KeyOf& keyOf, const MemberOf& memberOf)
        {
            first.fill(0, count + 1);
            for (auto& pair : pairs)
                ++first[keyOf(pair) + 1];
            for (unsigned key = 0; key < count; ++key)
                first[key + 1] += first[key];
            members.grow(pairs.size());
            Vector<unsigned> next = first;
            for (auto& pair : pairs)
                members[next[keyOf(pair)]++] = memberOf(pair);
        }
    };

    void listWrites()
    {
        m_firstWriteOfBlock.fill(0, m_code.size() + 1);
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            m_firstWriteOfBlock[blockIndex] = m_writes.size();
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                forEachWrite(block->at(instIndex), [&](unsigned place, unsigned source) {
                    m_writes.append({ instIndex, place, source });
                });
            }
        }
        m_firstWriteOfBlock[m_code.size()] = m_writes.size();
    }

    using HandedOn = std::pair<unsigned, unsigned>;

    void placeJoins(Dominators& dominators)
    {
        Vector<Vector<unsigned, 2>> frontierOf(m_code.size());
        for (BasicBlock* block : m_code) {
            if (block->numPredecessors() < (block == m_code[0] ? 1u : 2u))
                continue;
            BasicBlock* above = dominators.idom(block);
            for (BasicBlock* predecessor : block->predecessors()) {
                for (BasicBlock* runner = predecessor; runner && runner != above; runner = dominators.idom(runner)) {
                    auto& frontier = frontierOf[runner->index()];
                    if (!frontier.isEmpty() && frontier.last() == block->index())
                        break;
                    frontier.append(block->index());
                }
            }
        }

        Vector<std::pair<unsigned, unsigned>> placesAndBlocks;
        Vector<unsigned> lastBlockWriting(FillWith { }, m_numberOfPlaces, UINT_MAX);
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            for (unsigned write = m_firstWriteOfBlock[blockIndex]; write < m_firstWriteOfBlock[blockIndex + 1]; ++write) {
                if (std::exchange(lastBlockWriting[m_writes[write].place], blockIndex) != blockIndex)
                    placesAndBlocks.append({ m_writes[write].place, blockIndex });
            }
        }
        Lists blocksWriting;
        blocksWriting.fill(m_numberOfPlaces, placesAndBlocks, [](auto& pair) { return pair.first; }, [](auto& pair) { return pair.second; });

        Vector<unsigned, 16> givers;
        Vector<unsigned> placeWrittenIn(FillWith { }, m_code.size(), noPlace);
        Vector<unsigned> placeJoinedAt(FillWith { }, m_code.size(), noPlace);
        for (unsigned place = 0; place < m_numberOfPlaces; ++place) {
            for (unsigned block : blocksWriting.of(place)) {
                placeWrittenIn[block] = place;
                givers.append(block);
            }
            while (!givers.isEmpty()) {
                for (unsigned block : frontierOf[givers.takeLast()]) {
                    if (std::exchange(placeJoinedAt[block], place) == place)
                        continue;
                    m_joins.append({ block, place });
                    if (placeWrittenIn[block] != place)
                        givers.append(block);
                }
            }
        }
        Vector<unsigned> joins(m_joins.size(), [](size_t index) { return static_cast<unsigned>(index); });
        m_joinsOfBlock.fill(m_code.size(), joins, [&](unsigned index) { return m_joins[index].block; }, [](unsigned index) { return index; });
    }

    void give(unsigned place, unsigned name)
    {
        m_formerNames.append({ place, m_nameIn[place] });
        m_nameIn[place] = name;
        if (m_joinsAreBeingFilled)
            m_placesWithOtherNames.append(place);
    }

    void walkWithNames(Dominators& dominators, const Invocable<void(BasicBlock*)> auto& atHead, const Invocable<void(BasicBlock*)> auto& atBlock)
    {
        m_nameIn = Vector<unsigned>(m_numberOfPlaces, [](size_t place) { return static_cast<unsigned>(place); });
        Vector<unsigned> marks;
        dominators.forAllBlocksInDominatorTreePreOrder<DominatorTreeChildOrder::ReversePostOrder>([&](BasicBlock* block) {
            marks.append(m_formerNames.size());
            atHead(block);
            for (unsigned join : m_joinsOfBlock.of(block->index()))
                give(m_joins[join].place, nameOfJoin(join));
            atBlock(block);
        }, [&](BasicBlock*) {
            for (unsigned mark = marks.takeLast(); m_formerNames.size() > mark;) {
                auto [place, name] = m_formerNames.takeLast();
                m_nameIn[place] = name;
                if (m_joinsAreBeingFilled)
                    m_placesWithOtherNames.append(place);
            }
        });
    }

    void nameWhatPlacesHold()
    {
        Dominators dominators(m_code);
        Vector<HandedOn> handedOn;
        listWrites();
        placeJoins(dominators);
        Vector<unsigned> numberSeenFor(FillWith { }, m_code.size(), UINT_MAX);
        walkWithNames(dominators, [](BasicBlock*) { }, [&](BasicBlock* block) {
            for (unsigned write = m_firstWriteOfBlock[block->index()]; write < m_firstWriteOfBlock[block->index() + 1]; ++write) {
                if (m_writes[write].source != noPlace)
                    handedOn.append({ m_nameIn[m_writes[write].source], nameOfWrite(write) });
                give(m_writes[write].place, nameOfWrite(write));
            }
            for (BasicBlock* successor : block->successorBlocks()) {
                std::span<const unsigned> joins = m_joinsOfBlock.of(successor->index());
                if (joins.empty())
                    continue;
                unsigned numberSeen = std::exchange(numberSeenFor[successor->index()], m_placesWithOtherNames.size());
#if ENABLE(AOT)
                if (numberSeen != UINT_MAX && m_placesWithOtherNames.size() - numberSeen >= joins.size())
                    numberSeen = UINT_MAX;
#endif
                if (numberSeen == UINT_MAX) {
                    m_steps += joins.size();
                    for (unsigned join : joins)
                        handedOn.append({ m_nameIn[m_joins[join].place], nameOfJoin(join) });
                    continue;
                }
                m_steps += m_placesWithOtherNames.size() - numberSeen;
                for (unsigned place : m_placesWithOtherNames.span().subspan(numberSeen)) {
                    auto join = std::ranges::lower_bound(joins, place, { }, [&](unsigned join) { return m_joins[join].place; });
                    if (join != joins.end() && m_joins[*join].place == place)
                        handedOn.append({ m_nameIn[place], nameOfJoin(*join) });
                }
            }
        });
        m_joinsAreBeingFilled = false;
        m_placesWithOtherNames.clear();
        m_takenFrom.fill(numberOfNames(), handedOn, [](const HandedOn& pair) { return pair.second; }, [](const HandedOn& pair) { return pair.first; });
        m_handedTo.fill(numberOfNames(), handedOn, [](const HandedOn& pair) { return pair.first; }, [](const HandedOn& pair) { return pair.second; });
        m_steps += m_writes.size() + m_joins.size() + handedOn.size();

        m_hintThatCameBackTo.fill(0, numberOfNames());
        m_hintThatWentOnTo.fill(0, numberOfNames());
        walkWithNames(dominators, [](BasicBlock*) { }, [&](BasicBlock* block) {
            unsigned write = m_firstWriteOfBlock[block->index()];
            auto giveNamesUpTo = [&](unsigned instIndex) {
                for (; write < m_firstWriteOfBlock[block->index() + 1] && m_writes[write].instIndex < instIndex; ++write)
                    give(m_writes[write].place, nameOfWrite(write));
            };
            for (unsigned hint = m_firstHintOfBlock[block->index()]; hint < m_firstHintOfBlock[block->index() + 1]; ++hint) {
                giveNamesUpTo(m_hints[hint].instIndex);
                for (unsigned place : m_hints[hint].named)
                    addPlacesHoldingValueOf(place, m_hints[hint].places);
            }
            giveNamesUpTo(UINT_MAX);
        });
    }

    void addPlacesHoldingValueOf(unsigned named, BitVector& places)
    {
        ++m_numberOfQuestions;
        Vector<unsigned, 16> toGoBackFrom;
        Vector<unsigned, 16> toGoOnFrom;
        auto goBackFrom = [&](unsigned name) {
            if (std::exchange(m_hintThatCameBackTo[name], m_numberOfQuestions) != m_numberOfQuestions)
                toGoBackFrom.append(name);
        };
        auto goOnFrom = [&](unsigned name) {
            if (std::exchange(m_hintThatWentOnTo[name], m_numberOfQuestions) == m_numberOfQuestions)
                return;
            toGoOnFrom.append(name);
            unsigned place = placeOfName(name);
            if (m_nameIn[place] == name && !places.quickSet(place))
                goBackFrom(name);
        };
        places.quickSet(named);
        goBackFrom(m_nameIn[named]);
        while (!toGoBackFrom.isEmpty() || !toGoOnFrom.isEmpty()) {
            ++m_steps;
            if (!toGoBackFrom.isEmpty()) {
                unsigned name = toGoBackFrom.takeLast();
                goOnFrom(name);
                for (unsigned giver : m_takenFrom.of(name))
                    goBackFrom(giver);
                continue;
            }
            for (unsigned taker : m_handedTo.of(toGoOnFrom.takeLast()))
                goOnFrom(taker);
        }
    }

    void findPlacesOfValues()
    {
        m_watchedIndexOfPlace.fill(notWatched, m_numberOfPlaces);
        m_firstHintOfBlock.fill(0, m_code.size() + 1);
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            m_firstHintOfBlock[blockIndex] = m_hints.size();
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                Hint hint { block, instIndex, BitVector(m_numberOfPlaces) };
                if (PatchpointValue* call = callWhoseCalleeLetsGo(inst)) {
                    hint.isCall = true;
                    unsigned firstChild = 1 + m_code.proc().resultCount(call->type());
                    for (unsigned child : call->childrenLetGoByCallee()) {
                        if (auto place = firstChild + child < inst.args().size() ? placeOf(inst.args()[firstChild + child]) : std::nullopt)
                            hint.named.append(*place);
                    }
                } else if (!hintOf(inst))
                    continue;
                // The value may have become a constant.
                else if (auto place = inst.args().size() == 2 ? placeOf(inst.args()[1]) : std::nullopt)
                    hint.named.append(*place);
                m_hints.append(WTF::move(hint));
            }
        }
        m_firstHintOfBlock[m_code.size()] = m_hints.size();
        if (m_hints.isEmpty())
            return;

        nameWhatPlacesHold();
        for (Hint& hint : m_hints) {
            hint.places.exclude(m_neverCleared);
            for (unsigned place : hint.places) {
                if (m_watchedIndexOfPlace[place] != notWatched)
                    continue;
                m_watchedIndexOfPlace[place] = m_watchedPlaces.size();
                m_watchedPlaces.append(place);
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
    };

    void forEachAccessOfWatchedPlace(Inst& inst, const Invocable<void(unsigned)> auto& write, const Invocable<void(unsigned)> auto& read) const
    {
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
                read(*index);
            forEachWordTouchedInPart(arg, [&](unsigned place) {
                if (m_watchedIndexOfPlace[place] != notWatched)
                    read(m_watchedIndexOfPlace[place]);
            });
        });
        // This way the registers that addresses are made of count too.
        inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
            if (auto index = watchedIndexOf(Arg(tmp)); index && Arg::isAnyUse(role))
                read(*index);
        });
    }

    void stepBackward(Inst& inst, Backward& state) const
    {
        forEachAccessOfWatchedPlace(inst, [&](unsigned index) {
            state.live.quickClear(index);
            state.meetsCollection.quickClear(index);
        }, [&](unsigned index) {
            state.live.quickSet(index);
        });
        if (mayCollect(inst))
            state.meetsCollection = m_everyWatchedPlace;
    }

    Backward emptyState() const
    {
        unsigned size = m_watchedPlaces.size();
        return { BitVector(size), BitVector(size) };
    }

    void solveBackward()
    {
        m_everyWatchedPlace.ensureSize(m_watchedPlaces.size());
        for (unsigned index = 0; index < m_watchedPlaces.size(); ++index)
            m_everyWatchedPlace.quickSet(index);

        struct Effect {
            unsigned block;
            unsigned index;
            bool isLive;
            bool isWritten;
            bool meetsCollection;
        };
        Vector<Effect> effects;
        Vector<unsigned> effectOfIndex(FillWith { }, m_watchedPlaces.size(), UINT_MAX);
        BitVector blocksThatMayCollect(m_code.size());
        for (BasicBlock* block : m_code) {
            size_t firstEffect = effects.size();
            auto effectOf = [&](unsigned index) -> Effect& {
                if (effectOfIndex[index] == UINT_MAX || effectOfIndex[index] < firstEffect) {
                    effectOfIndex[index] = effects.size();
                    effects.append({ block->index(), index, false, false, false });
                }
                return effects[effectOfIndex[index]];
            };
            Vector<unsigned, 8> written;
            for (Inst& inst : *block) {
                if (mayCollect(inst))
                    blocksThatMayCollect.quickSet(block->index());
                written.shrink(0);
                forEachAccessOfWatchedPlace(inst, [&](unsigned index) {
                    written.append(index);
                }, [&](unsigned index) {
                    Effect& effect = effectOf(index);
                    effect.isLive |= !effect.isWritten;
                });
                for (unsigned index : written) {
                    Effect& effect = effectOf(index);
                    if (!std::exchange(effect.isWritten, true))
                        effect.meetsCollection = blocksThatMayCollect.quickGet(block->index());
                }
            }
        }

        m_atTail = IndexMap<BasicBlock*, Backward>(m_code.size());
        auto forEachBlockWithHints = [&](const Invocable<void(BasicBlock*)> auto& func) {
            BasicBlock* last = nullptr;
            for (const Hint& hint : m_hints) {
                if (std::exchange(last, hint.block) != hint.block)
                    func(hint.block);
            }
        };
        forEachBlockWithHints([&](BasicBlock* block) {
            m_atTail[block] = emptyState();
        });

        Vector<unsigned> blocksInPostOrder;
        BitVector isInOrder(m_code.size());
        appendNodeIndicesInOrder(m_code.cfg(), GraphOrder::PostOrder, isInOrder, blocksInPostOrder);

        constexpr unsigned unnumbered = UINT_MAX;
        Vector<unsigned> leastNumberFrom(FillWith { }, m_code.size(), unnumbered);
        Vector<unsigned> numberOf(FillWith { }, m_code.size(), unnumbered);
        for (unsigned turn = 0; turn < blocksInPostOrder.size(); ++turn)
            numberOf[blocksInPostOrder[turn]] = leastNumberFrom[blocksInPostOrder[turn]] = blocksInPostOrder.size() - turn;
        BitVector leadsToCollection = blocksThatMayCollect;
        for (bool changed = true; changed;) {
            changed = false;
            m_steps += blocksInPostOrder.size();
            for (unsigned blockIndex : blocksInPostOrder) {
                for (BasicBlock* successor : m_code[blockIndex]->successorBlocks()) {
                    if (leastNumberFrom[successor->index()] < leastNumberFrom[blockIndex]) {
                        leastNumberFrom[blockIndex] = leastNumberFrom[successor->index()];
                        changed = true;
                    }
                    if (leadsToCollection.quickGet(successor->index()))
                        changed |= !leadsToCollection.quickSet(blockIndex);
                }
            }
        }
        Vector<unsigned> greatestNumberTouching(FillWith { }, m_watchedPlaces.size(), 0u);
        for (const Effect& effect : effects) {
            if (numberOf[effect.block] != unnumbered)
                greatestNumberTouching[effect.index] = std::max(greatestNumberTouching[effect.index], numberOf[effect.block]);
        }
        constexpr unsigned isSettled = UINT_MAX;
        Vector<unsigned> positionInRounds(FillWith { }, m_watchedPlaces.size(), isSettled);
        Vector<unsigned> placesInRounds;
        for (const Hint& hint : m_hints) {
            unsigned leastNumberBehind = unnumbered;
            bool collectionIsBehind = false;
            for (BasicBlock* successor : hint.block->successorBlocks()) {
                leastNumberBehind = std::min(leastNumberBehind, leastNumberFrom[successor->index()]);
                collectionIsBehind |= leadsToCollection.quickGet(successor->index());
            }
            for (unsigned place : hint.places) {
                unsigned index = m_watchedIndexOfPlace[place];
                if (greatestNumberTouching[index] < leastNumberBehind) {
                    if (collectionIsBehind)
                        m_atTail[hint.block].meetsCollection.quickSet(index);
                    continue;
                }
                if (positionInRounds[index] == isSettled) {
                    positionInRounds[index] = placesInRounds.size();
                    placesInRounds.append(index);
                }
            }
        }

        constexpr unsigned placesAtOnce = 64;
        unsigned numberOfRounds = (placesInRounds.size() + placesAtOnce - 1) / placesAtOnce;
        Lists effectsOfRound;
        Vector<unsigned> indices;
        for (unsigned effect = 0; effect < effects.size(); ++effect) {
            if (positionInRounds[effects[effect].index] != isSettled)
                indices.append(effect);
        }
        effectsOfRound.fill(numberOfRounds, indices, [&](unsigned effect) { return positionInRounds[effects[effect].index] / placesAtOnce; }, [](unsigned effect) { return effect; });
        struct Words {
            uint64_t isLive { 0 };
            uint64_t isWritten { 0 };
            uint64_t meetsCollection { 0 };
            uint64_t liveAtTail { 0 };
            uint64_t meetsCollectionAtTail { 0 };
        };
        Vector<Words> words;
        for (unsigned round = 0; round < numberOfRounds; ++round) {
            words.fill(Words { }, m_code.size());
            for (unsigned index : effectsOfRound.of(round)) {
                const Effect& effect = effects[index];
                uint64_t bit = 1ull << (positionInRounds[effect.index] % placesAtOnce);
                Words& ofBlock = words[effect.block];
                ofBlock.isLive |= effect.isLive ? bit : 0;
                ofBlock.isWritten |= effect.isWritten ? bit : 0;
                ofBlock.meetsCollection |= effect.meetsCollection ? bit : 0;
            }
            for (bool changed = true; changed;) {
                changed = false;
                m_steps += blocksInPostOrder.size();
                for (unsigned blockIndex : blocksInPostOrder) {
                    BasicBlock* block = m_code[blockIndex];
                    const Words& ofBlock = words[blockIndex];
                    uint64_t live = (ofBlock.liveAtTail & ~ofBlock.isWritten) | ofBlock.isLive;
                    uint64_t behindBlock = blocksThatMayCollect.quickGet(blockIndex) ? ~0ull : ofBlock.meetsCollectionAtTail;
                    uint64_t meetsCollection = (behindBlock & ~ofBlock.isWritten) | ofBlock.meetsCollection;
                    for (BasicBlock* predecessor : block->predecessors()) {
                        Words& above = words[predecessor->index()];
                        changed |= (live & ~above.liveAtTail) || (meetsCollection & ~above.meetsCollectionAtTail);
                        above.liveAtTail |= live;
                        above.meetsCollectionAtTail |= meetsCollection;
                    }
                }
            }
            unsigned end = std::min<unsigned>(placesAtOnce, placesInRounds.size() - round * placesAtOnce);
            forEachBlockWithHints([&](BasicBlock* block) {
                const Words& ofBlock = words[block->index()];
                for (unsigned bit = 0; bit < end; ++bit) {
                    if (ofBlock.liveAtTail >> bit & 1)
                        m_atTail[block].live.quickSet(placesInRounds[round * placesAtOnce + bit]);
                    if (ofBlock.meetsCollectionAtTail >> bit & 1)
                        m_atTail[block].meetsCollection.quickSet(placesInRounds[round * placesAtOnce + bit]);
                }
            });
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
    Vector<unsigned> m_firstHintOfBlock;
    Vector<Write> m_writes;
    Vector<unsigned> m_firstWriteOfBlock;
    Vector<Join> m_joins;
    Lists m_joinsOfBlock;
    Lists m_takenFrom;
    Lists m_handedTo;
    Vector<unsigned> m_nameIn;
    Vector<std::pair<unsigned, unsigned>> m_formerNames;
    Vector<unsigned> m_placesWithOtherNames;
    bool m_joinsAreBeingFilled { true };
    Vector<unsigned> m_hintThatCameBackTo;
    Vector<unsigned> m_hintThatWentOnTo;
    unsigned m_numberOfQuestions { 0 };
    uint64_t m_steps { 0 };
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
