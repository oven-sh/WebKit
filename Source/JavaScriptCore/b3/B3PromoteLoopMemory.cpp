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
        m_proc.resetValueOwners();
        NaturalLoops& loops = m_proc.naturalLoops();
        Vector<const NaturalLoop*> outermostFirst;
        for (unsigned index = 0; index < loops.numLoops(); ++index)
            outermostFirst.append(&loops.loop(index));
        std::ranges::stable_sort(outermostFirst, { }, [&](const NaturalLoop* loop) {
            return loops.loopDepth(loop->header());
        });
        for (const NaturalLoop* loop : outermostFirst)
            considerLoop(loops, *loop);
        if (m_promotedLocations.isEmpty())
            return false;
        promoteLocations();
        return true;
    }

private:
    struct PromotedLocation {
        BasicBlock* preheader { nullptr };
        Value* preheaderLoad { nullptr };
        Variable* variable { nullptr };
    };

    // A narrow store leaves part of the location unchanged, and a narrow load extends what it reads.
    static bool isPromotableAccess(const MemoryValue* memory)
    {
        if (memory->opcode() != Load && memory->opcode() != Store)
            return false;
        if (memory->isExotic() || memory->traps())
            return false;
        return memory->accessType().isNumeric() && !memory->accessType().isVector();
    }

    static bool accessSameLocation(const MemoryValue* first, const MemoryValue* second)
    {
        return first->lastChild() == second->lastChild() && first->offset() == second->offset() && first->accessType() == second->accessType();
    }

    enum class AddressKind : uint8_t { LoopInvariant, LoadedInLoop, LoopVariant };

    // Visits each value in the loop once, no matter how many addresses are computed from it.
    static Counter reasonAddressIsNotHoisted(const NaturalLoops& loops, const NaturalLoop& loop, Value* address, UncheckedKeyHashMap<Value*, AddressKind>& addressKinds)
    {
        Vector<Value*, 16> path { address };
        while (!path.isEmpty()) {
            Value* value = path.last();
            if (addressKinds.contains(value)) {
                path.removeLast();
                continue;
            }
            AddressKind result = AddressKind::LoopInvariant;
            bool hasUnvisitedChild = false;
            if (value->opcode() == Phi)
                result = AddressKind::LoopVariant;
            else {
                Effects effects = value->effects();
                if (effects.reads || effects.writes || effects.readsLocalState || effects.readsPinned)
                    result = AddressKind::LoadedInLoop;
                for (Value* child : value->children()) {
                    if (!loops.belongsTo(child->owner, loop))
                        continue;
                    auto found = addressKinds.find(child);
                    if (found == addressKinds.end()) {
                        path.append(child);
                        hasUnvisitedChild = true;
                    } else
                        result = std::max(result, found->value);
                }
            }
            if (hasUnvisitedChild)
                continue;
            addressKinds.add(value, result);
            path.removeLast();
        }
        switch (addressKinds.get(address)) {
        case AddressKind::LoopInvariant:
            return Counter::NotPromotedAddressNotHoisted;
        case AddressKind::LoadedInLoop:
            return Counter::NotPromotedAddressLoadedInLoop;
        case AddressKind::LoopVariant:
            return Counter::NotPromotedLoopVariantAddress;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        m_proc.count(Counter::LoopsConsideredForMemoryPromotion);
        BasicBlock* preheader = findPreHeader(loops, loop);
        if (!preheader || preheader->last()->opcode() != Jump) {
            m_proc.count(Counter::LoopsWithoutPreheader);
            return;
        }

        Vector<MemoryValue*, 16> accesses;
        Vector<uint64_t, 16> storeBegins;
        Vector<uint64_t, 16> storeEnds;
        RangeSet<HeapRange> clobbersOnLikelyPaths;
        RangeSet<HeapRange> clobbersOnRarePaths;
        IndexSet<BasicBlock*> likelyBlocks = findLikelyBlocks(loops, loop);
        bool hasSidewaysExit = false;
        bool callsOnLikelyPaths = false;
        for (unsigned index = loop.size(); index--;) {
            bool isLikely = likelyBlocks.contains(loop.at(index));
            RangeSet<HeapRange>& clobbers = isLikely ? clobbersOnLikelyPaths : clobbersOnRarePaths;
            for (Value* value : *loop.at(index)) {
                Effects effects = value->effects();
                if (effects.fence) {
                    m_proc.count(Counter::LoopsWithFences);
                    return;
                }
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && isPromotableAccess(memory)) {
                    accesses.append(memory);
                    if (memory->isStore()) {
                        storeBegins.append(memory->range().begin());
                        storeEnds.append(memory->range().end());
                    }
                    continue;
                }
                effects.writes.addTo(clobbers);
                hasSidewaysExit |= effects.exitsSideways && !isLoopPoll(value);
                callsOnLikelyPaths |= isLikely && isCall(value) && !isLoopPoll(value);
            }
        }
        if (storeBegins.isEmpty())
            return;
        m_proc.count(Counter::MemoryPromotionAccessesVisited, accesses.size());
        std::ranges::sort(storeBegins);
        std::ranges::sort(storeEnds);
        auto numberOverlapping = [](const auto& sortedBegins, const auto& sortedEnds, const HeapRange& range) {
            size_t numberEndingBefore = std::ranges::upper_bound(sortedEnds, range.begin()) - sortedEnds.begin();
            size_t numberBeginningAfter = sortedBegins.end() - std::ranges::lower_bound(sortedBegins, range.end());
            return sortedBegins.size() - numberEndingBefore - numberBeginningAfter;
        };
        // Groups the accesses to each location together.
        std::ranges::sort(accesses, { }, [](const MemoryValue* access) {
            return std::tuple { access->lastChild()->index(), access->offset(), access->accessType().kind(), access->range().begin(), access->range().end(), access->index() };
        });

        Dominators& dominators = m_proc.dominators();
        // Such an access executes whenever the loop is entered, so the preheader load reads nothing that the program would not have accessed anyway.
        auto alwaysExecutes = [&](const MemoryValue* access) {
            Effects effects = access->effects();
            if (!effects.controlDependent || !m_proc.effectsOf(access).controlDependent)
                return true;
            return !hasSidewaysExit && m_proc.backwardsDominators().dominates(access->owner, preheader);
        };
        bool ignoresClobbers = m_proc.policy().validation.ignoresWritesToPromotedLocations;
        UncheckedKeyHashMap<Value*, AddressKind> addressKinds;

        for (size_t first = 0, next = 0; first < accesses.size(); first = next) {
            while (next < accesses.size() && accessSameLocation(accesses[next], accesses[first]))
                ++next;
            std::span<MemoryValue*> accessesToLocation = accesses.mutableSpan().subspan(first, next - first);
            MemoryValue* load = nullptr;
            Vector<uint64_t, 4> ownStoreBegins;
            Vector<uint64_t, 4> ownStoreEnds;
            bool isPromotable = true;
            bool isGuaranteedToExecute = false;
            for (MemoryValue* access : accessesToLocation) {
                // An outer loop that promoted this location has already claimed all of its accesses.
                isPromotable &= !m_locationIndexOfAccess.contains(access) && access->range();
                if (access->isStore()) {
                    ownStoreBegins.append(access->range().begin());
                    ownStoreEnds.append(access->range().end());
                } else {
                    isPromotable &= access->readsMutability() != Mutability::Immutable;
                    if (!load || access->index() < load->index())
                        load = access;
                }
            }
            bool isPromotedInOuterLoop = m_locationIndexOfAccess.contains(accessesToLocation[0]);
            if (load && !isPromotedInOuterLoop)
                m_proc.count(ownStoreBegins.isEmpty() ? Counter::LocationsOnlyLoadedInLoops : Counter::LocationsLoadedAndStoredInLoops);
            if (!load || ownStoreBegins.isEmpty())
                continue;
            if (!isPromotable) {
                if (!isPromotedInOuterLoop)
                    m_proc.count(Counter::NotPromotedImmutable);
                continue;
            }
            if (auto* clientHoistsOutOfLoops = m_proc.policy().loops.clientHoistsOutOfLoops; clientHoistsOutOfLoops && clientHoistsOutOfLoops(m_proc, *load)) {
                m_proc.count(Counter::NotPromotedHandledByClient);
                continue;
            }
            if (!dominators.dominates(load->lastChild()->owner, preheader)) {
                m_proc.count(reasonAddressIsNotHoisted(loops, loop, load->lastChild(), addressKinds));
                continue;
            }
            // A load that executes cannot observe a write to a heap range disjoint from its own, so each replaced load is checked against its own range.
            // The range of another access proves nothing about this load: that access may never execute, in which case its range need not be accurate.
            std::ranges::sort(ownStoreEnds);
            enum class Clobber : uint8_t { None, EffectsOnRarePaths, MayAliasStores, EffectsOnLikelyPaths };
            auto clobberOf = [&](const HeapRange& range) {
                if (HeapSet(range).overlaps(clobbersOnLikelyPaths))
                    return Clobber::EffectsOnLikelyPaths;
                if (numberOverlapping(storeBegins, storeEnds, range) != numberOverlapping(ownStoreBegins, ownStoreEnds, range))
                    return Clobber::MayAliasStores;
                return HeapSet(range).overlaps(clobbersOnRarePaths) ? Clobber::EffectsOnRarePaths : Clobber::None;
            };
            Clobber clobber = Clobber::None;
            HeapRange unionOfRanges = accessesToLocation[0]->range();
            HeapRange lastRangeChecked;
            for (MemoryValue* access : accessesToLocation) {
                unionOfRanges = HeapRange(std::min(unionOfRanges.begin(), access->range().begin()), std::max(unionOfRanges.end(), access->range().end()));
                if (ignoresClobbers || !access->isLoad() || access->range() == lastRangeChecked)
                    continue;
                lastRangeChecked = access->range();
                clobber = std::max(clobber, clobberOf(lastRangeChecked));
            }
            if (clobber != Clobber::None) {
                m_proc.count(clobber == Clobber::EffectsOnRarePaths ? Counter::NotPromotedClobberedOnRarePaths : clobber == Clobber::MayAliasStores ? Counter::NotPromotedMayAliasStores : Counter::NotPromotedClobberedOnLikelyPaths);
                continue;
            }
            m_proc.count(Counter::MemoryPromotionAccessesVisited, accessesToLocation.size());
            for (MemoryValue* access : accessesToLocation)
                isGuaranteedToExecute = isGuaranteedToExecute || alwaysExecutes(access);
            if (!isGuaranteedToExecute) {
                m_proc.count(Counter::NotPromotedNotGuaranteedToExecute);
                continue;
            }
            // Next to a call a load is cheap, and the promoted value would have to stay live across the call on every iteration.
            if (callsOnLikelyPaths) {
                m_proc.count(Counter::NotPromotedCallsOnLikelyPaths);
                continue;
            }

            unsigned location = m_promotedLocations.size();
            // The preheader load executes no matter which of the accesses the iterations reach.
            MemoryValue* preheaderLoad = m_proc.clone(load)->as<MemoryValue>();
            preheaderLoad->setRange(unionOfRanges);
            m_promotedLocations.append({ preheader, preheaderLoad, m_proc.addVariable(load->type()) });
            for (MemoryValue* access : accessesToLocation)
                RELEASE_ASSERT(m_locationIndexOfAccess.add(access, location).isNewEntry);
            m_proc.count(Counter::LocationsPromotedInLoops);
        }
    }

    void promoteLocations()
    {
        for (const PromotedLocation& location : m_promotedLocations) {
            location.preheader->appendNonTerminal(location.preheaderLoad);
            location.preheader->appendNonTerminal(m_proc.add<VariableValue>(Set, location.preheaderLoad->origin(), location.variable, location.preheaderLoad));
        }
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                auto found = m_locationIndexOfAccess.find(value);
                if (found == m_locationIndexOfAccess.end())
                    continue;
                Variable* variable = m_promotedLocations[found->value].variable;
                if (value->opcode() == Store) {
                    m_insertionSet.insert<VariableValue>(index + 1, Set, value->origin(), variable, value->child(0));
                    continue;
                }
                Value* promoted = m_insertionSet.insert<VariableValue>(index, Get, value->origin(), variable);
                if (std::optional<MovedLoad> moved = MovedLoad::of(m_proc, value))
                    m_proc.movedLoadLog().add(*moved, block, index, value->lastChild(), promoted);
                value->replaceWithIdentity(promoted);
            }
            m_insertionSet.execute(block);
        }
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    Vector<PromotedLocation> m_promotedLocations;
    UncheckedKeyHashMap<Value*, unsigned> m_locationIndexOfAccess;
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
