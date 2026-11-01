/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3PromoteLoopMemory.h"

#if ENABLE(B3_JIT)

#include "B3BackwardsDominators.h"
#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValueInlines.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include <wtf/RangeSet.h>

namespace JSC { namespace B3 {

namespace {

class PromoteLoopMemory {
public:
    PromoteLoopMemory(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        Vector<const NaturalLoop*> fromOutermost;
        for (unsigned index = 0; index < loops.numLoops(); ++index)
            fromOutermost.append(&loops.loop(index));
        std::ranges::stable_sort(fromOutermost, { }, [&](const NaturalLoop* loop) {
            return loops.loopDepth(loop->header());
        });
        for (const NaturalLoop* loop : fromOutermost)
            considerLoop(loops, *loop);
        if (m_words.isEmpty())
            return false;
        carryWords();
        return true;
    }

private:
    struct Word {
        BasicBlock* preHeader { nullptr };
        Value* loadInFront { nullptr };
        Variable* variable { nullptr };
    };

    static bool accessesWholeWord(const MemoryValue* memory)
    {
        if (memory->opcode() != Load && memory->opcode() != Store)
            return false;
        if (memory->isExotic() || memory->traps())
            return false;
        return memory->accessType().isNumeric() && !memory->accessType().isVector();
    }

    static bool accessSameWord(const MemoryValue* first, const MemoryValue* second)
    {
        return first->lastChild() == second->lastChild() && first->offset() == second->offset() && first->accessType() == second->accessType();
    }

    enum class MadeOf : uint8_t { WhatStaysTheSame, WhatIsReadInLoop, WhatChangesWithTurns };

    static Counter reasonThatAddressIsMadeInLoop(const NaturalLoops& loops, const NaturalLoop& loop, Value* address, UncheckedKeyHashMap<Value*, MadeOf>& madeOf)
    {
        Vector<Value*, 16> path { address };
        while (!path.isEmpty()) {
            Value* value = path.last();
            if (madeOf.contains(value)) {
                path.removeLast();
                continue;
            }
            MadeOf result = MadeOf::WhatStaysTheSame;
            bool waitsForChild = false;
            if (value->opcode() == Phi)
                result = MadeOf::WhatChangesWithTurns;
            else {
                Effects effects = value->effects();
                if (effects.reads || effects.writes || effects.readsLocalState || effects.readsPinned)
                    result = MadeOf::WhatIsReadInLoop;
                for (Value* child : value->children()) {
                    if (!loops.belongsTo(child->owner, loop))
                        continue;
                    auto found = madeOf.find(child);
                    if (found == madeOf.end()) {
                        path.append(child);
                        waitsForChild = true;
                    } else
                        result = std::max(result, found->value);
                }
            }
            if (waitsForChild)
                continue;
            madeOf.add(value, result);
            path.removeLast();
        }
        switch (madeOf.get(address)) {
        case MadeOf::WhatStaysTheSame:
            return Counter::WordsLeftForAddressNotMovedOutOfLoop;
        case MadeOf::WhatIsReadInLoop:
            return Counter::WordsLeftForAddressReadInLoop;
        case MadeOf::WhatChangesWithTurns:
            return Counter::WordsLeftForAddressThatChangesWithTurns;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        m_proc.count(Counter::LoopsSearchedForWordsToCarry);
        BasicBlock* preHeader = findPreHeader(loops, loop);
        if (!preHeader || preHeader->last()->opcode() != Jump) {
            m_proc.count(Counter::LoopsWithNoBlockAheadToReadWordsIn);
            return;
        }

        Vector<MemoryValue*, 16> accesses;
        Vector<uint64_t, 16> beginsOfStores;
        Vector<uint64_t, 16> endsOfStores;
        RangeSet<HeapRange> otherWritesOnLikelyPaths;
        RangeSet<HeapRange> otherWritesOnRarePaths;
        IndexSet<BasicBlock*> likelyBlocks = findLikelyBlocks(loops, loop);
        bool leavesSideways = false;
        bool callsOnLikelyPaths = false;
        for (unsigned index = loop.size(); index--;) {
            bool isLikely = likelyBlocks.contains(loop.at(index));
            RangeSet<HeapRange>& otherWrites = isLikely ? otherWritesOnLikelyPaths : otherWritesOnRarePaths;
            for (Value* value : *loop.at(index)) {
                Effects effects = value->effects();
                if (effects.fence) {
                    m_proc.count(Counter::LoopsWithFencesSearchedForWords);
                    return;
                }
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && accessesWholeWord(memory)) {
                    accesses.append(memory);
                    if (memory->isStore()) {
                        beginsOfStores.append(memory->range().begin());
                        endsOfStores.append(memory->range().end());
                    }
                    continue;
                }
                effects.writes.addTo(otherWrites);
                leavesSideways |= effects.exitsSideways && !isLoopPoll(value);
                callsOnLikelyPaths |= isLikely && isCall(value) && !isLoopPoll(value);
            }
        }
        if (beginsOfStores.isEmpty())
            return;
        m_proc.count(Counter::AccessesReadForWordsCarriedAroundLoops, accesses.size());
        std::ranges::sort(beginsOfStores);
        std::ranges::sort(endsOfStores);
        auto numberThatOverlap = [](const auto& sortedBegins, const auto& sortedEnds, const HeapRange& range) {
            size_t numberThatEndInFront = std::ranges::upper_bound(sortedEnds, range.begin()) - sortedEnds.begin();
            size_t numberThatBeginBehind = sortedBegins.end() - std::ranges::lower_bound(sortedBegins, range.end());
            return sortedBegins.size() - numberThatEndInFront - numberThatBeginBehind;
        };
        std::ranges::sort(accesses, { }, [](const MemoryValue* access) {
            return std::tuple { access->lastChild()->index(), access->offset(), access->accessType().kind(), access->range().begin(), access->range().end(), access->index() };
        });

        Dominators& dominators = m_proc.dominators();
        auto isCertainToRun = [&](const MemoryValue* access) {
            Effects effects = access->effects();
            if (!effects.controlDependent || !m_proc.effectsOf(access).controlDependent)
                return true;
            return !leavesSideways && m_proc.backwardsDominators().dominates(access->owner, preHeader);
        };
        bool overlooksOtherWrites = m_proc.policy().validation.overlooksWritesToWordsCarriedAroundLoops;
        UncheckedKeyHashMap<Value*, MadeOf> madeOf;

        for (size_t first = 0, next = 0; first < accesses.size(); first = next) {
            while (next < accesses.size() && accessSameWord(accesses[next], accesses[first]))
                ++next;
            std::span<MemoryValue*> accessesToWord = accesses.mutableSpan().subspan(first, next - first);
            MemoryValue* load = nullptr;
            Vector<uint64_t, 4> beginsOfStoresToWord;
            Vector<uint64_t, 4> endsOfStoresToWord;
            bool isOrdinary = true;
            bool isTouchedByWhoeverEnters = false;
            for (MemoryValue* access : accessesToWord) {
                isOrdinary &= !m_wordOf.contains(access) && access->range();
                if (access->isStore()) {
                    beginsOfStoresToWord.append(access->range().begin());
                    endsOfStoresToWord.append(access->range().end());
                } else {
                    isOrdinary &= access->readsMutability() != Mutability::Immutable;
                    if (!load || access->index() < load->index())
                        load = access;
                }
            }
            bool isTakenByLoopAround = m_wordOf.contains(accessesToWord[0]);
            if (load && !isTakenByLoopAround)
                m_proc.count(beginsOfStoresToWord.isEmpty() ? Counter::WordsOnlyReadInLoops : Counter::WordsReadAndWrittenInLoops);
            if (!load || beginsOfStoresToWord.isEmpty())
                continue;
            if (!isOrdinary) {
                if (!isTakenByLoopAround)
                    m_proc.count(Counter::WordsLeftThatAreSaidNeverToChange);
                continue;
            }
            if (auto* isTakenOutOfLoopsByClient = m_proc.policy().loops.isTakenOutOfLoopsByClient; isTakenOutOfLoopsByClient && isTakenOutOfLoopsByClient(m_proc, *load)) {
                m_proc.count(Counter::WordsLeftToPhasesOfClient);
                continue;
            }
            if (!dominators.dominates(load->lastChild()->owner, preHeader)) {
                m_proc.count(reasonThatAddressIsMadeInLoop(loops, loop, load->lastChild(), madeOf));
                continue;
            }
            std::ranges::sort(endsOfStoresToWord);
            enum class Writers : uint8_t { StoresToWordAlone, OthersOnRarePaths, StoresUnderOtherAddresses, OthersOnLikelyPaths };
            auto writersOf = [&](const HeapRange& range) {
                if (HeapSet(range).overlaps(otherWritesOnLikelyPaths))
                    return Writers::OthersOnLikelyPaths;
                if (numberThatOverlap(beginsOfStores, endsOfStores, range) != numberThatOverlap(beginsOfStoresToWord, endsOfStoresToWord, range))
                    return Writers::StoresUnderOtherAddresses;
                return HeapSet(range).overlaps(otherWritesOnRarePaths) ? Writers::OthersOnRarePaths : Writers::StoresToWordAlone;
            };
            Writers writers = Writers::StoresToWordAlone;
            HeapRange heapsNamedForWord = accessesToWord[0]->range();
            HeapRange lastHeapAsked;
            for (MemoryValue* access : accessesToWord) {
                heapsNamedForWord = HeapRange(std::min(heapsNamedForWord.begin(), access->range().begin()), std::max(heapsNamedForWord.end(), access->range().end()));
                if (overlooksOtherWrites || !access->isLoad() || access->range() == lastHeapAsked)
                    continue;
                lastHeapAsked = access->range();
                writers = std::max(writers, writersOf(lastHeapAsked));
            }
            if (writers != Writers::StoresToWordAlone) {
                m_proc.count(writers == Writers::OthersOnRarePaths ? Counter::WordsLeftForWritesOnRarePaths : writers == Writers::StoresUnderOtherAddresses ? Counter::WordsLeftForStoresUnderOtherAddresses : Counter::WordsLeftForWritesOnLikelyPaths);
                continue;
            }
            m_proc.count(Counter::AccessesReadForWordsCarriedAroundLoops, accessesToWord.size());
            for (MemoryValue* access : accessesToWord)
                isTouchedByWhoeverEnters = isTouchedByWhoeverEnters || isCertainToRun(access);
            if (!isTouchedByWhoeverEnters) {
                m_proc.count(Counter::WordsLeftThatMayNotBeTouched);
                continue;
            }
            if (callsOnLikelyPaths) {
                m_proc.count(Counter::WordsLeftBesideCallsOnLikelyPaths);
                continue;
            }

            unsigned word = m_words.size();
            MemoryValue* loadInFront = m_proc.clone(load)->as<MemoryValue>();
            loadInFront->setRange(heapsNamedForWord);
            m_words.append({ preHeader, loadInFront, m_proc.addVariable(load->type()) });
            for (MemoryValue* access : accessesToWord)
                RELEASE_ASSERT(m_wordOf.add(access, word).isNewEntry);
            m_proc.count(Counter::WordsCarriedAroundLoops);
        }
    }

    void carryWords()
    {
        for (const Word& word : m_words) {
            word.preHeader->appendNonTerminal(word.loadInFront);
            word.preHeader->appendNonTerminal(m_proc.add<VariableValue>(Set, word.loadInFront->origin(), word.variable, word.loadInFront));
        }
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                auto found = m_wordOf.find(value);
                if (found == m_wordOf.end())
                    continue;
                Variable* variable = m_words[found->value].variable;
                if (value->opcode() == Store) {
                    m_insertionSet.insert<VariableValue>(index + 1, Set, value->origin(), variable, value->child(0));
                    continue;
                }
                Value* carried = m_insertionSet.insert<VariableValue>(index, Get, value->origin(), variable);
                if (std::optional<MovedLoad> moved = MovedLoad::of(m_proc, value))
                    m_proc.movedLoadLog().add(*moved, block, index, value->lastChild(), carried);
                value->replaceWithIdentity(carried);
            }
            m_insertionSet.execute(block);
        }
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    Vector<Word> m_words;
    UncheckedKeyHashMap<Value*, unsigned> m_wordOf;
};

} // anonymous namespace

bool promoteLoopMemory(Procedure& proc)
{
    PhaseScope phaseScope(proc, "promoteLoopMemory"_s);
    PromoteLoopMemory promoteLoopMemory(proc);
    return promoteLoopMemory.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
