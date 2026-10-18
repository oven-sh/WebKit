/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3VersionLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3Dominators.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3FixSSA.h"
#include "B3InsertionSetInlines.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValueInlines.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3VariableValue.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/RangeSet.h>

namespace JSC { namespace B3 {

namespace {

class VersionLoops {
public:
    VersionLoops(Procedure& proc, Vector<BasicBlock*>& topTestedVersionHeaders)
        : m_proc(proc)
        , m_topTestedVersionHeaders(topTestedVersionHeaders)
        , m_upsilons(proc)
    {
    }

    bool run()
    {
        ensureLoopPreHeaders(m_proc);
        NaturalLoops& loops = m_proc.naturalLoops();
        m_proc.resetValueOwners();
        m_numberOfUses.resize(m_proc.values().size());
        for (Value* value : m_proc.values()) {
            for (Value* child : value->children())
                m_numberOfUses[child]++;
        }
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;)
            considerLoop(loops, loops.loop(loopIndex));
        if (m_versions.isEmpty())
            return false;

        for (auto& version : m_versions) {
            for (BasicBlock* block : version.hotBlocks)
                m_originalFrequencies.add(block, block->frequency());
            for (BasicBlock* block : version.excursionBlocks)
                m_originalFrequencies.add(block, block->frequency());
        }
        for (unsigned i = m_versions.size(); i--;)
            hoist(m_versions[i]);
        demoteValuesEscapingHotBlocks();
        for (auto& version : m_versions)
            cloneHotBlocks(version);
        splitEdgesIntoSlowPaths();
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned maxNumberOfClonedValues = 48;
    static constexpr unsigned maxClonedValuesWithHiddenRanges = 100;
    static constexpr unsigned maxUnswitchingClonedValues = 64;
    static constexpr unsigned maxExcursionClonedValues = 128;
    static constexpr unsigned minNumberOfHoistedValues = 2;
    static constexpr double originalLoopFrequencyScale = 1.0 / 1024;

    struct Poll {
        BasicBlock* block;
        Value* limit;
        Value* condition;
        BasicBlock* continuation;
    };

    struct IndexLimit {
        Value* limit;
        int64_t minOffset;
        int64_t maxOffset;
    };

    struct Version {
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        Vector<BasicBlock*, 8> hotBlocks;
        Vector<Value*, 8> hoistedValues;
        IndexSet<Value*> hoistedValueSet;
        IndexSet<Value*> omittedValues;
        unsigned numberOfClonedValues { 0 };
        unsigned numberOfHoistedValues { 0 };
        unsigned numberOfOmittedValues { 0 };
        bool mayExitEveryIteration { false };
        unsigned numberOfValuesHoistedPastGuards { 0 };
        BasicBlock* firstIterationSlowPath { nullptr };
        std::optional<unsigned> nextVersionIndex;
        Value* unswitchedTerminal { nullptr };
        BasicBlock* nextVersionSuccessor { nullptr };
        bool isEnteredFromPreHeader { true };
        UncheckedKeyHashMap<Value*, Value*> repeatedValues;
        UncheckedKeyHashMap<Value*, Value*> hoistedClones;
        UncheckedKeyHashMap<Value*, BasicBlock*> hoistedGuardLikelySuccessors;
        std::optional<CountedLoop> countedLoop;
        bool isTopTested { false };
        UncheckedKeyHashMap<Value*, bool> comparisonResults;
        std::optional<Poll> poll;
        Vector<IndexLimit, 4> indexLimits;
        Variable* clampedBound { nullptr };
        Variable* bound { nullptr };
        Variable* stripEnd { nullptr };
        BasicBlock* fastVersionFirstPreHeader { nullptr };
        BasicBlock* fastVersionLastPreHeader { nullptr };
        bool hidesRanges { false };
        Vector<std::pair<Value*, Opcode>, 2> phisNarrowedOnEntry;
        Vector<BasicBlock*, 8> excursionBlocks;
        IndexSet<Value*> hoistedValuesValidInExcursion;
        Vector<BasicBlock*, 8> slowPathBlocks;
    };

    static BasicBlock* guardLikelySuccessor(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block)
    {
        if (block->numSuccessors() != 2 || (block->last()->opcode() != Branch && block->last()->opcode() != Patchpoint))
            return nullptr;
        bool firstIsUnlikely = isRare(block->successor(0), block);
        bool secondIsUnlikely = isRare(block->successor(1), block);
        if (!firstIsUnlikely && !secondIsUnlikely) {
            firstIsUnlikely = !loops.belongsTo(block->successorBlock(0), loop);
            secondIsUnlikely = !loops.belongsTo(block->successorBlock(1), loop);
        }
        if (firstIsUnlikely == secondIsUnlikely)
            return nullptr;
        return block->successorBlock(firstIsUnlikely ? 1 : 0);
    }

    struct UnswitchCandidate {
        BasicBlock* block { nullptr };
        std::array<BasicBlock*, 2> successors { };
    };

    static void findUnswitchCandidate(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block, UnswitchCandidate& result)
    {
        if (block->last()->opcode() != Branch && block->last()->opcode() != Switch)
            return;
        unsigned numberOfLikelySuccessors = 0;
        for (const FrequentedBlock& successor : block->successors()) {
            if (isRare(successor, block) || !loops.belongsTo(successor.block(), loop))
                continue;
            if (successor.block() == loop.header())
                return;
            if (numberOfLikelySuccessors && (result.successors[0] == successor.block() || (numberOfLikelySuccessors == 2 && result.successors[1] == successor.block())))
                continue;
            if (numberOfLikelySuccessors == 2)
                return;
            result.successors[numberOfLikelySuccessors++] = successor.block();
        }
        if (numberOfLikelySuccessors == 2)
            result.block = block;
    }

    bool endsWithPoll(BasicBlock* block) const
    {
        if (!m_proc.loopPollGenerator() || block->last()->opcode() != Branch)
            return false;
        for (Value* child : block->last()->child(0)->children()) {
            auto* patchpoint = child->as<PatchpointValue>();
            if (patchpoint && patchpoint->generator() == m_proc.loopPollGenerator())
                return true;
        }
        return false;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        for (unsigned i = loop.size(); i--;) {
            if (loops.innerMostLoopOf(loop.at(i)) != &loop)
                return;
        }
        BasicBlock* preHeader = nullptr;
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (loops.belongsTo(predecessor, loop))
                continue;
            if (preHeader)
                return;
            preHeader = predecessor;
        }
        if (!preHeader || preHeader->last()->opcode() != Jump)
            return;

        auto addVersion = [&](Version&& version) {
            findExcursion(loops, loop, version);
            if (version.firstIterationSlowPath && !version.excursionBlocks.contains(version.firstIterationSlowPath) && !version.unswitchedTerminal && version.isEnteredFromPreHeader)
                return;
            for (BasicBlock* block : version.hotBlocks)
                m_hotBlocks.add(block);
            for (BasicBlock* block : version.excursionBlocks)
                m_hotBlocks.add(block);
            m_versions.append(WTF::move(version));
        };
        m_resolvesSingleInputPhis = true;
        std::optional<Version> topTestedVersion = tryCreateVersion(loops, loop, preHeader, nullptr, nullptr, nullptr);
        m_resolvesSingleInputPhis = false;
        UnswitchCandidate candidate;
        std::optional<Version> version = tryCreateVersion(loops, loop, preHeader, nullptr, nullptr, &candidate);
        if (topTestedVersion) {
            bool countsOmittedValues = !topTestedVersion->mayExitEveryIteration;
            unsigned stays = topTestedVersion->numberOfClonedValues - topTestedVersion->numberOfHoistedValues - (countsOmittedValues ? topTestedVersion->numberOfOmittedValues : 0) + (countsOmittedValues ? 0 : topTestedVersion->numberOfValuesHoistedPastGuards);
            unsigned staysOnOtherPath = version ? version->numberOfClonedValues - version->numberOfHoistedValues - (countsOmittedValues ? version->numberOfOmittedValues : 0) : topTestedVersion->numberOfClonedValues;
            if (topTestedVersion->isTopTested ? stays * 4 <= staysOnOtherPath * 3 : stays < staysOnOtherPath) {
                addVersion(WTF::move(*topTestedVersion));
                return;
            }
        }
        if (candidate.block) {
            std::optional<Version> first = tryCreateVersion(loops, loop, preHeader, candidate.block, candidate.successors[0], nullptr);
            std::optional<Version> second = first ? tryCreateVersion(loops, loop, preHeader, candidate.block, candidate.successors[1], nullptr) : std::nullopt;
            if (second && first->numberOfClonedValues + second->numberOfClonedValues <= maxUnswitchingClonedValues) {
                first->nextVersionIndex = m_versions.size() + 1;
                first->unswitchedTerminal = candidate.block->last();
                first->nextVersionSuccessor = candidate.successors[1];
                second->isEnteredFromPreHeader = false;
                addVersion(WTF::move(*first));
                addVersion(WTF::move(*second));
                return;
            }
        }
        if (version)
            addVersion(WTF::move(*version));
    }

    void findExcursion(NaturalLoops& loops, const NaturalLoop& loop, Version& version)
    {
        IndexSet<BasicBlock*> hotBlocks;
        for (BasicBlock* block : version.hotBlocks)
            hotBlocks.add(block);
        RangeSet<HeapRange> hoistedValueReads;
        bool hoistedValuesReadPinned = false;
        IndexSet<Value*> holdInFastVersionOnly;
        for (Value* value : version.hoistedValues) {
            Effects effects = value->effects();
            if (effects.readsMutability != Mutability::Immutable)
                hoistedValueReads.add(effects.reads);
            hoistedValuesReadPinned |= effects.readsPinned;
            bool holdsInFastVersionOnly = value->opcode() == Phi;
            for (Value* child : value->children())
                holdsInFastVersionOnly |= holdInFastVersionOnly.contains(child);
            if (holdsInFastVersionOnly)
                holdInFastVersionOnly.add(value);
            else
                version.hoistedValuesValidInExcursion.add(value);
        }

        IndexSet<BasicBlock*> comeBack;
        IndexSet<BasicBlock*> asked;
        Vector<BasicBlock*, 16> worklist { loop.header() };
        while (!worklist.isEmpty()) {
            for (BasicBlock* predecessor : worklist.takeLast()->predecessors()) {
                if (predecessor == loop.header() || !loops.belongsTo(predecessor, loop) || !asked.add(predecessor))
                    continue;
                bool canBeCloned = true;
                if (!hotBlocks.contains(predecessor)) {
                    for (Value* value : *predecessor) {
                        Effects effects = value->effects();
                        canBeCloned &= canCloneForLoop(value) && !isCall(value) && !effects.fence && !(effects.writesPinned && hoistedValuesReadPinned) && !hoistedValueReads.overlaps(effects.writes);
                    }
                }
                if (!canBeCloned)
                    continue;
                comeBack.add(predecessor);
                worklist.append(predecessor);
            }
        }

        IndexSet<BasicBlock*> reached;
        auto add = [&](BasicBlock* block) {
            if (comeBack.contains(block) && reached.add(block))
                version.excursionBlocks.append(block);
        };
        for (BasicBlock* block : version.hotBlocks) {
            if (version.hoistedValueSet.contains(block->last()))
                continue;
            for (BasicBlock* successor : block->successorBlocks()) {
                if (!hotBlocks.contains(successor))
                    add(successor);
            }
        }
        unsigned numberOfClonedValues = 0;
        for (unsigned i = 0; i < version.excursionBlocks.size(); ++i) {
            BasicBlock* block = version.excursionBlocks[i];
            for (Value* value : *block)
                numberOfClonedValues += emitsInstructions(value) && !version.hoistedValuesValidInExcursion.contains(value);
            if (version.hoistedValuesValidInExcursion.contains(block->last())) {
                add(version.hoistedGuardLikelySuccessors.get(block->last()));
                continue;
            }
            for (BasicBlock* successor : block->successorBlocks())
                add(successor);
        }
        if (numberOfClonedValues > maxExcursionClonedValues)
            version.excursionBlocks.clear();
    }

    static bool loopsBackThroughInvariantTests(NaturalLoops& loops, const NaturalLoop& loop)
    {
        for (BasicBlock* block : loop.header()->predecessors()) {
            if (!loops.belongsTo(block, loop))
                continue;
            while (block != loop.header() && block->last()->opcode() == Jump && block->numPredecessors() == 1)
                block = block->predecessor(0);
            if (block->last()->opcode() != Branch || loops.belongsTo(block->last()->child(0)->owner, loop))
                return false;
        }
        return true;
    }

    std::optional<Version> tryCreateVersion(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* preHeader, BasicBlock* unswitchedBlock, BasicBlock* takenSuccessor, UnswitchCandidate* candidate)
    {
        Version version;
        version.header = loop.header();
        version.preHeader = preHeader;

        IndexSet<BasicBlock*> hotBlocks;
        hotBlocks.add(loop.header());
        version.hotBlocks.append(loop.header());
        bool hasBackEdge = false;
        for (unsigned i = 0; i < version.hotBlocks.size(); ++i) {
            for (const FrequentedBlock& successor : version.hotBlocks[i]->successors()) {
                if (isRare(successor, version.hotBlocks[i]) || !loops.belongsTo(successor.block(), loop))
                    continue;
                if (version.hotBlocks[i] == unswitchedBlock && successor.block() != takenSuccessor)
                    continue;
                hasBackEdge |= successor.block() == loop.header();
                if (hotBlocks.add(successor.block()))
                    version.hotBlocks.append(successor.block());
            }
        }
        if (!hasBackEdge || loopsBackThroughInvariantTests(loops, loop))
            return std::nullopt;
        for (unsigned i = loop.size(); i--;) {
            if (!hotBlocks.contains(loop.at(i)))
                version.slowPathBlocks.append(loop.at(i));
        }

        RangeSet<HeapRange> writes;
        bool writesPinned = false;
        unsigned numberOfClonedValues = 0;
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || isCall(value))
                    return std::nullopt;
                numberOfClonedValues += emitsInstructions(value);
                Effects effects = value->effects();
                if (effects.fence)
                    return std::nullopt;
                writes.add(effects.writes);
                writesPinned |= effects.writesPinned;
            }
        }
        auto isExtendedWordInFastVersion = [&](Value* value) -> Opcode {
            for (unsigned depth = 0; depth < 4; ++depth) {
                if (value->opcode() == SExt32 || value->opcode() == ZExt32)
                    return value->opcode();
                if (value->opcode() != Phi || value->owner == loop.header() || !hotBlocks.contains(value->owner))
                    return Oops;
                auto* upsilons = m_upsilons.find(value);
                if (!upsilons)
                    return Oops;
                Value* only = nullptr;
                for (UpsilonValue* upsilon : *upsilons) {
                    if (!hotBlocks.contains(upsilon->owner))
                        continue;
                    if (only && only != upsilon->child(0))
                        return Oops;
                    only = upsilon->child(0);
                }
                if (!only)
                    return Oops;
                value = only;
            }
            return Oops;
        };
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                if (value->opcode() != Phi || value->type() != Int64)
                    continue;
                auto* upsilons = m_upsilons.find(value);
                if (!upsilons)
                    continue;
                if (block != loop.header()) {
                    bool hasMarker = false;
                    for (UpsilonValue* upsilon : *upsilons) {
                        Value* source = upsilon->child(0);
                        hasMarker |= !hotBlocks.contains(upsilon->owner) && source->hasInt64() && source->asInt64() != static_cast<int32_t>(source->asInt64()) && source->asInt64() != static_cast<int64_t>(static_cast<uint32_t>(source->asInt64()));
                    }
                    version.hidesRanges |= hasMarker && isExtendedWordInFastVersion(value) != Oops;
                    continue;
                }
                Opcode backEdgeValue = Oops;
                bool hasOtherBackEdgeValue = false;
                Value* entering = nullptr;
                for (UpsilonValue* upsilon : *upsilons) {
                    if (!loops.belongsTo(upsilon->owner, loop))
                        entering = upsilon->child(0);
                    if (!hotBlocks.contains(upsilon->owner))
                        continue;
                    Opcode extension = isExtendedWordInFastVersion(upsilon->child(0));
                    if (extension == Oops || (backEdgeValue != Oops && backEdgeValue != extension))
                        hasOtherBackEdgeValue = true;
                    else
                        backEdgeValue = extension;
                }
                if (backEdgeValue != Oops && !hasOtherBackEdgeValue && entering && entering->opcode() != backEdgeValue && !entering->hasInt64())
                    version.phisNarrowedOnEntry.append({ value, backEdgeValue });
            }
        }
        if (!version.hidesRanges)
            version.phisNarrowedOnEntry.clear();
        if (numberOfClonedValues > maxClonedValuesWithHiddenRanges)
            return std::nullopt;
        version.numberOfClonedValues = numberOfClonedValues;

        auto isInvariant = [&](Value* value) {
            return !loops.belongsTo(value->owner, loop) || version.hoistedValueSet.contains(value);
        };
        auto hasInvariantChildren = [&](Value* value) {
            for (Value* child : value->children()) {
                if (!isInvariant(child))
                    return false;
            }
            return true;
        };
        Vector<Value*, 16> valuesBeforeFastLoop;
        {
            IndexSet<Value*> seen;
            Vector<Value*, 16> worklist;
            auto addChildrenBeforeLoop = [&](Value* value) {
                for (Value* child : value->children()) {
                    if (emitsInstructions(child) && !loops.belongsTo(child->owner, loop) && seen.add(child))
                        worklist.append(child);
                }
            };
            for (BasicBlock* block : version.hotBlocks) {
                for (Value* value : *block)
                    addChildrenBeforeLoop(value);
            }
            static constexpr unsigned maxNumberOfBlocks = 4;
            BasicBlock* precedingBlock = preHeader;
            for (unsigned i = 0; i < maxNumberOfBlocks; ++i) {
                for (Value* value : *precedingBlock) {
                    if (emitsInstructions(value) && value->type() != Void && seen.add(value))
                        worklist.append(value);
                }
                if (precedingBlock->numPredecessors() != 1)
                    break;
                precedingBlock = precedingBlock->predecessor(0);
            }
            auto isUnwrittenSince = [&](Value* load, HeapRange reads) {
                BasicBlock* block = preHeader;
                for (unsigned i = 0; i < maxNumberOfBlocks; ++i) {
                    for (unsigned index = block->size(); index--;) {
                        Value* value = block->at(index);
                        if (value == load)
                            return true;
                        Effects effects = value->effects();
                        if (effects.fence || effects.writes.overlaps(reads))
                            return false;
                    }
                    if (block->numPredecessors() != 1)
                        return false;
                    block = block->predecessor(0);
                }
                return false;
            };
            while (!worklist.isEmpty()) {
                Value* value = worklist.takeLast();
                Effects effects = value->effects();
                if (effects.mustExecute() || effects.readsLocalState || effects.readsPinned)
                    continue;
                if (effects.reads && effects.readsMutability != Mutability::Immutable && !(value->as<MemoryValue>() && isUnwrittenSince(value, effects.reads)))
                    continue;
                valuesBeforeFastLoop.append(value);
                addChildrenBeforeLoop(value);
            }
        }
        UncheckedKeyHashMap<Value*, Value*> repeatedLoads;
        auto& repeatedValues = m_resolvesSingleInputPhis ? version.repeatedValues : repeatedLoads;
        auto withoutRepetition = [&](Value* value) {
            Value* repeated = repeatedValues.get(value);
            return repeated ? repeated : value;
        };
        auto isSameValue = [&](Value* a, Value* b) {
            a = withoutRepetition(a);
            b = withoutRepetition(b);
            return a == b || (a->hasInt() && b->hasInt() && a->type() == b->type() && a->asInt() == b->asInt());
        };
        auto repeatsHoistedLoad = [&](Value* value) {
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad())
                return false;
            for (Value* hoisted : valuesBeforeFastLoop) {
                auto* other = hoisted->as<MemoryValue>();
                if (!other || other->opcode() != load->opcode() || other->type() != load->type() || other->offset() != load->offset())
                    continue;
                if (withoutRepetition(other->lastChild()) != withoutRepetition(load->lastChild()))
                    continue;
                repeatedValues.add(value, withoutRepetition(hoisted));
                return true;
            }
            return false;
        };
        auto repeatsHoistedComputation = [&](Value* value) {
            switch (value->opcode()) {
            case Add:
            case Sub:
            case BitAnd:
            case BitOr:
            case BitXor:
            case Shl:
            case SShr:
            case ZShr:
            case Trunc:
            case SExt32:
            case ZExt32:
            case IToD:
            case Equal:
            case NotEqual:
            case LessThan:
            case GreaterThan:
            case LessEqual:
            case GreaterEqual:
            case Above:
            case Below:
            case AboveEqual:
            case BelowEqual:
                break;
            default:
                return false;
            }
            for (Value* hoisted : valuesBeforeFastLoop) {
                if (hoisted->kind() != value->kind() || hoisted->type() != value->type() || version.repeatedValues.contains(hoisted))
                    continue;
                bool hasSameChildren = true;
                for (unsigned i = value->numChildren(); i--;)
                    hasSameChildren &= isSameValue(hoisted->child(i), value->child(i));
                if (!hasSameChildren)
                    continue;
                version.repeatedValues.add(value, hoisted);
                return true;
            }
            return false;
        };
        auto singleInputInFastVersion = [&](Value* phi) -> Value* {
            auto* upsilons = m_upsilons.find(phi);
            if (!upsilons)
                return nullptr;
            Value* result = nullptr;
            for (UpsilonValue* upsilon : *upsilons) {
                if (loops.belongsTo(upsilon->owner, loop) && !hotBlocks.contains(upsilon->owner))
                    continue;
                Value* child = withoutRepetition(upsilon->child(0));
                if (result && result != child)
                    return nullptr;
                result = child;
            }
            return result;
        };
        auto isBetweenHoistedLoads = [&](Value* value) {
            static constexpr int64_t minPageSize = 4096;
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad())
                return false;
            int64_t begin = load->offset();
            int64_t end = begin + load->accessByteSize();
            std::optional<int64_t> beginOfLower;
            std::optional<int64_t> endOfUpper;
            for (Value* hoisted : valuesBeforeFastLoop) {
                auto* other = hoisted->as<MemoryValue>();
                if (!other || !other->isLoad() || version.repeatedValues.contains(hoisted) || withoutRepetition(other->lastChild()) != withoutRepetition(load->lastChild()))
                    continue;
                int64_t beginOfOther = other->offset();
                int64_t endOfOther = beginOfOther + other->accessByteSize();
                if (beginOfOther <= begin && (!beginOfLower || beginOfOther > *beginOfLower))
                    beginOfLower = beginOfOther;
                if (endOfOther >= end && (!endOfUpper || endOfOther < *endOfUpper))
                    endOfUpper = endOfOther;
            }
            return beginOfLower && endOfUpper && *endOfUpper - *beginOfLower < minPageSize;
        };
        auto visitTransitiveInputs = [&](Value* root, unsigned limit, const auto& visit) {
            IndexSet<Value*> seen;
            Vector<Value*, 16> worklist { root };
            while (!worklist.isEmpty()) {
                Value* value = worklist.takeLast();
                if (value->isConstant() || !seen.add(value))
                    continue;
                if (!limit-- || !visit(value))
                    return false;
                for (Value* child : value->children())
                    worklist.append(child);
                if (value->opcode() != Phi)
                    continue;
                if (auto* upsilons = m_upsilons.find(value)) {
                    for (UpsilonValue* upsilon : *upsilons)
                        worklist.append(upsilon->child(0));
                }
            }
            return true;
        };
        static constexpr unsigned maxGuardInputs = 48;
        static constexpr unsigned maxAddressInputs = 12;
        IndexSet<Value*> remainingGuardInputs;
        bool remainingGuardInputsAreKnown = true;
        auto isIndependentOfRemainingGuards = [&](Value* value) {
            auto* load = value->as<MemoryValue>();
            return load && load->isLoad() && remainingGuardInputsAreKnown && visitTransitiveInputs(load->lastChild(), maxAddressInputs, [&](Value* source) {
                return !remainingGuardInputs.contains(source);
            });
        };
        auto sharesAlignedHeaderWithHoistedLoad = [&](Value* value) {
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad() || !load->isInAlignedHeader())
                return false;
            for (Value* hoisted : valuesBeforeFastLoop) {
                auto* other = hoisted->as<MemoryValue>();
                if (other && other->isLoad() && other->isInAlignedHeader() && withoutRepetition(other->lastChild()) == withoutRepetition(load->lastChild()))
                    return true;
            }
            static constexpr unsigned maxDominatorsToScan = 24;
            unsigned valueBudget = 512;
            Dominators& dominators = m_proc.dominators();
            BasicBlock* block = preHeader;
            for (unsigned count = 0; block && count < maxDominatorsToScan; ++count, block = dominators.idom(block)) {
                for (Value* before : *block) {
                    if (!valueBudget--)
                        return false;
                    auto* other = before->as<MemoryValue>();
                    if (other && other->isLoad() && other->isInAlignedHeader() && other->lastChild() == withoutRepetition(load->lastChild()))
                        return true;
                }
            }
            return false;
        };
        auto matchesValueBeforeLoop = [&](auto& self, Value* inside, Value* before) -> bool {
            if (inside == before)
                return !loops.belongsTo(inside->owner, loop);
            if (!hotBlocks.contains(inside->owner) || loops.belongsTo(before->owner, loop) || inside->kind() != before->kind() || inside->type() != before->type())
                return false;
            if (inside->opcode() == ZExt32 || inside->opcode() == SExt32)
                return self(self, inside->child(0), before->child(0));
            auto* load = inside->as<MemoryValue>();
            if (!load || !load->isLoad() || load->offset() != before->as<MemoryValue>()->offset() || !valuesBeforeFastLoop.contains(before))
                return false;
            Effects effects = load->effects();
            if (effects.readsMutability != Mutability::Immutable && writes.overlaps(effects.reads))
                return false;
            return self(self, load->lastChild(), before->lastChild());
        };
        auto invariantValueOfHeaderPhi = [&](Value* phi) -> Value* {
            auto* upsilons = m_upsilons.find(phi);
            if (!upsilons)
                return nullptr;
            Value* entering = nullptr;
            for (UpsilonValue* upsilon : *upsilons) {
                if (loops.belongsTo(upsilon->owner, loop))
                    continue;
                if (entering)
                    return nullptr;
                entering = upsilon->child(0);
            }
            if (!entering || entering->isConstant())
                return nullptr;
            for (UpsilonValue* upsilon : *upsilons) {
                if (loops.belongsTo(upsilon->owner, loop) && !matchesValueBeforeLoop(matchesValueBeforeLoop, upsilon->child(0), entering))
                    return nullptr;
            }
            return entering;
        };
        IndexSet<Value*> hoistedPastRemainingGuards;
        UncheckedKeyHashMap<Value*, bool> remainingGuardConditions;
        std::optional<IndexSet<BasicBlock*>> rejoiningBlocks;
        auto slowPathRejoinsLoop = [&](BasicBlock* asked) {
            if (!rejoiningBlocks) {
                rejoiningBlocks.emplace();
                IndexSet<BasicBlock*> seen;
                Vector<BasicBlock*, 16> worklist { loop.header() };
                while (!worklist.isEmpty()) {
                    for (BasicBlock* predecessor : worklist.takeLast()->predecessors()) {
                        if (predecessor == loop.header() || !loops.belongsTo(predecessor, loop) || !seen.add(predecessor))
                            continue;
                        bool canBeCloned = true;
                        if (!hotBlocks.contains(predecessor)) {
                            for (Value* value : *predecessor)
                                canBeCloned &= canCloneForLoop(value) && !isCall(value) && !value->effects().fence;
                        }
                        if (!canBeCloned)
                            continue;
                        rejoiningBlocks->add(predecessor);
                        worklist.append(predecessor);
                    }
                }
            }
            return rejoiningBlocks->contains(asked);
        };
        auto shouldKeepGuardInLoop = [&](BasicBlock* block, BasicBlock* likelySuccessor) {
            bool usesValueHoistedPastGuards = false;
            for (Value* child : block->last()->children())
                usesValueHoistedPastGuards |= hoistedPastRemainingGuards.contains(child);
            if (!usesValueHoistedPastGuards)
                return false;
            for (BasicBlock* successor : block->successorBlocks()) {
                if (successor != likelySuccessor && slowPathRejoinsLoop(successor))
                    return true;
            }
            return false;
        };
        UncheckedKeyHashMap<Value*, bool> hoistedGuardConditions;
        BasicBlock* topTest = nullptr;
        IndexSet<BasicBlock*> blocksReachedWithoutTopTest;
        bool allGuardsSoFarAreHoisted = true;
        bool hoistsGuard = false;
        unsigned numberOfHoistedValues = 0;
        auto hoist = [&](Value* value) {
            version.hoistedValues.append(value);
            version.hoistedValueSet.add(value);
            valuesBeforeFastLoop.append(value);
            numberOfHoistedValues += emitsInstructions(value);
        };
        Vector<Value*, 8> variantGuards;
        IndexSet<BasicBlock*> visited;
        for (BasicBlock* block = loop.header(); block && hotBlocks.contains(block) && visited.add(block);) {
            for (Value* value : *block) {
                if (value->opcode() == Phi) {
                    Value* only = !m_resolvesSingleInputPhis ? nullptr : block == loop.header() ? invariantValueOfHeaderPhi(value) : singleInputInFastVersion(value);
                    if (only && isInvariant(only)) {
                        if (block == loop.header() || hoistedPastRemainingGuards.contains(only))
                            hoistedPastRemainingGuards.add(value);
                        version.repeatedValues.add(value, only);
                        hoist(value);
                    }
                    continue;
                }
                if (value == block->last() || value->type() == Void || !hasInvariantChildren(value))
                    continue;
                Effects effects = value->effects();
                if (effects.mustExecute() || effects.readsLocalState || (effects.readsPinned && writesPinned))
                    continue;
                if (effects.reads && effects.readsMutability != Mutability::Immutable && writes.overlaps(effects.reads))
                    continue;
                if (!m_resolvesSingleInputPhis) {
                    if (effects.controlDependent && !allGuardsSoFarAreHoisted && !repeatsHoistedLoad(value))
                        continue;
                    hoist(value);
                    continue;
                }
                bool isPastRemainingGuards = false;
                if (!repeatsHoistedLoad(value) && effects.controlDependent && !allGuardsSoFarAreHoisted && !isBetweenHoistedLoads(value)) {
                    if (!sharesAlignedHeaderWithHoistedLoad(value) && !isIndependentOfRemainingGuards(value))
                        continue;
                    isPastRemainingGuards = true;
                }
                isPastRemainingGuards |= hoistedPastRemainingGuards.contains(withoutRepetition(value));
                for (Value* child : value->children())
                    isPastRemainingGuards |= hoistedPastRemainingGuards.contains(child);
                if (isPastRemainingGuards && hoistedPastRemainingGuards.add(value))
                    version.numberOfValuesHoistedPastGuards += emitsInstructions(value);
                repeatsHoistedComputation(value);
                if (value->opcode() == Select) {
                    if (auto known = hoistedGuardConditions.find(withoutRepetition(value->child(0))); known != hoistedGuardConditions.end())
                        version.repeatedValues.add(value, withoutRepetition(value->child(known->value ? 1 : 2)));
                }
                hoist(value);
            }
            if (block->last()->opcode() == Jump) {
                block = block->successorBlock(0);
                continue;
            }
            BasicBlock* likelySuccessor = block == unswitchedBlock ? takenSuccessor : guardLikelySuccessor(loops, loop, block);
            if (!likelySuccessor) {
                if (candidate && hasInvariantChildren(block->last()))
                    findUnswitchCandidate(loops, loop, block, *candidate);
                break;
            }
            if (block == unswitchedBlock && !hasInvariantChildren(block->last()))
                return std::nullopt;
            if (hasInvariantChildren(block->last()) && !shouldKeepGuardInLoop(block, likelySuccessor)) {
                hoist(block->last());
                hoistsGuard = true;
                version.hoistedGuardLikelySuccessors.add(block->last(), likelySuccessor);
                if (block->last()->opcode() == Branch)
                    hoistedGuardConditions.add(withoutRepetition(block->last()->child(0)), likelySuccessor == block->successorBlock(0));
            } else if (!endsWithPoll(block)) {
                if (!version.firstIterationSlowPath && block->last()->opcode() == Branch) {
                    auto valueOnFirstIteration = [&](auto& self, Value* value) -> std::optional<int64_t> {
                        if (value->hasInt64())
                            return value->asInt64();
                        if (value->opcode() == Add && value->type() == Int64) {
                            auto left = self(self, value->child(0));
                            auto right = left ? self(self, value->child(1)) : std::nullopt;
                            if (!right)
                                return std::nullopt;
                            return static_cast<int64_t>(static_cast<uint64_t>(*left) + static_cast<uint64_t>(*right));
                        }
                        if (value->opcode() != Phi || value->owner != loop.header())
                            return std::nullopt;
                        std::optional<int64_t> result;
                        if (auto* upsilons = m_upsilons.find(value)) {
                            for (UpsilonValue* upsilon : *upsilons) {
                                if (loops.belongsTo(upsilon->owner, loop))
                                    continue;
                                if (result || !upsilon->child(0)->hasInt64())
                                    return std::nullopt;
                                result = upsilon->child(0)->asInt64();
                            }
                        }
                        return result;
                    };
                    Value* condition = block->last()->child(0);
                    bool indexIsFirst = condition->opcode() == Below || condition->opcode() == AboveEqual;
                    if (indexIsFirst || condition->opcode() == Above || condition->opcode() == BelowEqual) {
                        bool isTrueInBounds = condition->opcode() == Below || condition->opcode() == Above;
                        BasicBlock* inBoundsSuccessor = block->successorBlock(isTrueInBounds ? 0 : 1);
                        auto index = valueOnFirstIteration(valueOnFirstIteration, condition->child(indexIsFirst ? 0 : 1));
                        auto isZeroExtendedLength = [&](Value* value) {
                            if (value->opcode() == ZExt32)
                                return true;
                            auto* upsilons = value->opcode() == Phi ? m_upsilons.find(value) : nullptr;
                            if (!upsilons)
                                return false;
                            for (UpsilonValue* upsilon : *upsilons) {
                                if (upsilon->child(0)->opcode() != ZExt32)
                                    return false;
                            }
                            return true;
                        };
                        if (index && *index < 0 && isZeroExtendedLength(condition->child(indexIsFirst ? 1 : 0)) && inBoundsSuccessor == likelySuccessor)
                            version.firstIterationSlowPath = block->successorBlock(isTrueInBounds ? 1 : 0);
                    }
                }
                if (block->last()->opcode() == Branch) {
                    bool isTaken = likelySuccessor == block->successorBlock(0);
                    auto known = remainingGuardConditions.add(withoutRepetition(block->last()->child(0)), isTaken);
                    if (!known.isNewEntry && known.iterator->value == isTaken) {
                        version.hoistedValueSet.add(block->last());
                        version.hoistedGuardLikelySuccessors.add(block->last(), likelySuccessor);
                        ++numberOfHoistedValues;
                        block = likelySuccessor;
                        continue;
                    }
                }
                allGuardsSoFarAreHoisted = false;
                remainingGuardInputsAreKnown = remainingGuardInputsAreKnown && visitTransitiveInputs(block->last(), maxGuardInputs, [&](Value* source) {
                    remainingGuardInputs.add(source);
                    return true;
                });
                bool slowPathStaysInLoop = false;
                for (BasicBlock* successor : block->successorBlocks())
                    slowPathStaysInLoop |= successor != likelySuccessor && loops.belongsTo(successor, loop);
                if (slowPathStaysInLoop) {
                    static constexpr unsigned maxNumberOfSources = 16;
                    bool variesAcrossIterations = false;
                    IndexSet<Value*> seen;
                    Vector<Value*, 16> worklist { block->last() };
                    for (unsigned count = 0; !worklist.isEmpty() && !variesAcrossIterations; ++count) {
                        Value* value = worklist.takeLast();
                        if (isInvariant(value) || !seen.add(value))
                            continue;
                        variesAcrossIterations = value->opcode() == Phi || count >= maxNumberOfSources;
                        for (Value* child : value->children())
                            worklist.append(child);
                    }
                    version.mayExitEveryIteration |= !variesAcrossIterations;
                    if (variesAcrossIterations)
                        variantGuards.append(block->last());
                }
                if (!topTest && block != unswitchedBlock && block->last()->opcode() == Branch) {
                    const FrequentedBlock& other = block->successor(block->successorBlock(0) == likelySuccessor ? 1 : 0);
                    if (!loops.belongsTo(other.block(), loop) && !isRare(other, block))
                        topTest = block;
                }
            }
            block = likelySuccessor;
        }
        if ((!version.hidesRanges && !hoistsGuard && numberOfHoistedValues < minNumberOfHoistedValues) || (unswitchedBlock && !version.hoistedValueSet.contains(unswitchedBlock->last())))
            return std::nullopt;
        if (!version.hidesRanges && numberOfClonedValues - numberOfHoistedValues > maxNumberOfClonedValues)
            return std::nullopt;
        if (numberOfClonedValues <= maxNumberOfClonedValues && numberOfHoistedValues >= minNumberOfHoistedValues)
            version.phisNarrowedOnEntry.clear();
        version.numberOfHoistedValues = numberOfHoistedValues;
        version.countedLoop = analyzeCountedLoop(loops, loop, m_upsilons, isInvariant);
        if (!version.countedLoop && topTest && m_resolvesSingleInputPhis) {
            Value* condition = topTest->last()->child(0);
            if (condition->opcode() == Phi && condition->owner != loop.header())
                condition = singleInputInFastVersion(condition);
            if (condition) {
                version.countedLoop = analyzeTopTestedCountedLoop(loops, loop, preHeader, topTest, condition, m_upsilons, isInvariant, [&](BasicBlock* block) {
                    return !hotBlocks.contains(block);
                });
            }
            if (version.countedLoop) {
                BasicBlock* staying = version.countedLoop->latch;
                Vector<BasicBlock*, 8> worklist { loop.header() };
                blocksReachedWithoutTopTest.add(loop.header());
                while (!worklist.isEmpty()) {
                    BasicBlock* block = worklist.takeLast();
                    for (Value* value : *block) {
                        Effects effects = value->effects();
                        auto* upsilon = value->as<UpsilonValue>();
                        if (effects.writes || effects.writesPinned || effects.exitsSideways || effects.fence || value->opcode() == Set || (upsilon && upsilon->phi()->owner == loop.header()))
                            version.countedLoop = std::nullopt;
                    }
                    for (BasicBlock* successor : block->successorBlocks()) {
                        if (!hotBlocks.contains(successor) || (block == topTest && successor == staying))
                            continue;
                        if (blocksReachedWithoutTopTest.add(successor))
                            worklist.append(successor);
                    }
                }
            }
            version.isTopTested = !!version.countedLoop;
        }
        if (version.countedLoop) {
            Value* bound = version.countedLoop->bound;
            auto isBound = [&](Value* value) {
                Value* other = bound;
                while (true) {
                    value = withoutRepetition(value);
                    other = withoutRepetition(other);
                    if (value == other)
                        return true;
                    if (value->opcode() != other->opcode() || value->type() != other->type())
                        return false;
                    if (value->opcode() != ZExt32 && value->opcode() != SExt32) {
                        auto* load = value->as<MemoryValue>();
                        if (!load || !load->isLoad() || load->offset() != other->as<MemoryValue>()->offset())
                            return false;
                        if (!version.hoistedValueSet.contains(value) || !version.hoistedValueSet.contains(other))
                            return false;
                    }
                    value = value->lastChild();
                    other = other->lastChild();
                }
            };
            for (BasicBlock* block : version.hotBlocks) {
                if (version.isTopTested && blocksReachedWithoutTopTest.contains(block))
                    continue;
                for (Value* value : *block) {
                    if (value->numChildren() != 2)
                        continue;
                    Value* left = isBound(value->child(0)) ? bound : value->child(0);
                    Value* right = isBound(value->child(1)) ? bound : value->child(1);
                    if (auto outcome = evaluateComparisonWithBound(*version.countedLoop, value->opcode(), left, right, bound))
                        version.comparisonResults.add(value, *outcome);
                }
            }
            if (!version.isTopTested && !relationHoldsOnEntry(version))
                version.comparisonResults.clear();
        }
        if (version.countedLoop && hotBlocks.contains(version.countedLoop->test) && hotBlocks.contains(version.countedLoop->latch))
            version.poll = findPoll(version);
        if (version.poll) {
            version.omittedValues.add(version.poll->limit);
            version.omittedValues.add(version.poll->condition);
            version.hoistedValueSet.add(version.poll->block->last());
            version.hoistedGuardLikelySuccessors.add(version.poll->block->last(), version.poll->continuation);
            version.numberOfOmittedValues += emitsInstructions(version.poll->limit) + emitsInstructions(version.poll->condition) + 1;
        }
        bool advancesOnEveryPath = version.poll && !version.countedLoop->saturation && version.countedLoop->increases();
        if (advancesOnEveryPath) {
            for (UpsilonValue* upsilon : *m_upsilons.find(version.countedLoop->inductionVariable))
                advancesOnEveryPath &= upsilon->owner == preHeader || upsilon->child(0) == version.countedLoop->next;
        }
        auto lagsOneIteration = [&](Value* phi) {
            const CountedLoop& counted = *version.countedLoop;
            if (phi->opcode() != Phi || phi->owner != loop.header() || phi == counted.inductionVariable)
                return false;
            auto* upsilons = m_upsilons.find(phi);
            if (!upsilons)
                return false;
            auto isStartMinusStep = [&](Value* value) {
                Value* start = counted.start;
                if (start->hasInt64())
                    return value->hasInt64() && value->asInt64() == start->asInt64() - counted.step;
                return start->opcode() == Add && start->child(1)->hasInt64() && start->child(1)->asInt64() == counted.step && isSameValue(start->child(0), value);
            };
            for (UpsilonValue* upsilon : *upsilons) {
                if (upsilon->owner == preHeader ? !isStartMinusStep(upsilon->child(0)) : upsilon->child(0) != counted.inductionVariable)
                    return false;
            }
            return true;
        };
        auto isStoredInLoop = [&](Value* limit) {
            while (limit->opcode() == ZExt32 || limit->opcode() == SExt32)
                limit = limit->child(0);
            auto* load = limit->as<MemoryValue>();
            if (!load || !load->isLoad())
                return false;
            for (unsigned i = loop.size(); i--;) {
                for (Value* value : *loop.at(i)) {
                    auto* store = value->as<MemoryValue>();
                    if (store && store->isStore() && store->range().overlaps(load->range()))
                        return true;
                }
            }
            return false;
        };
        if (advancesOnEveryPath) {
            static constexpr unsigned maxNumberOfLimits = 4;
            std::optional<int64_t> knownStart;
            if (Value* start = version.countedLoop->start; start->hasInt64())
                knownStart = start->asInt64();
            else if (start->opcode() == Add && start->child(0)->hasInt64() && start->child(1)->hasInt64())
                knownStart = static_cast<int64_t>(static_cast<uint64_t>(start->child(0)->asInt64()) + static_cast<uint64_t>(start->child(1)->asInt64()));
            for (BasicBlock* block : version.hotBlocks) {
                if (version.isTopTested && blocksReachedWithoutTopTest.contains(block))
                    continue;
                for (Value* value : *block) {
                    bool indexIsFirst = value->opcode() == Below || value->opcode() == AboveEqual;
                    if ((!indexIsFirst && value->opcode() != Above && value->opcode() != BelowEqual) || version.comparisonResults.contains(value) || version.hoistedValueSet.contains(value))
                        continue;
                    Value* index = value->child(indexIsFirst ? 0 : 1);
                    Value* limit = value->child(indexIsFirst ? 1 : 0);
                    if (index->type() != Int64 || !isInvariant(limit) || value == version.countedLoop->test->last()->child(0))
                        continue;
                    int64_t offset = 0;
                    if (lagsOneIteration(index))
                        offset = -version.countedLoop->step;
                    else if (index != version.countedLoop->inductionVariable) {
                        if (index->opcode() != Add || index->child(0) != version.countedLoop->inductionVariable || !index->child(1)->hasInt64())
                            continue;
                        offset = index->child(1)->asInt64();
                        if (offset < -maxIterationsPerStrip || offset > maxIterationsPerStrip)
                            continue;
                    }
                    if (knownStart && *knownStart + offset < 0)
                        continue;
                    if (isStoredInLoop(limit))
                        continue;
                    limit = withoutRepetition(limit);
                    size_t found = version.indexLimits.findIf([&](auto& indexLimit) { return indexLimit.limit == limit; });
                    if (found == notFound) {
                        if (version.indexLimits.size() == maxNumberOfLimits)
                            continue;
                        found = version.indexLimits.size();
                        version.indexLimits.append({ limit, offset, offset });
                    }
                    version.indexLimits[found].minOffset = std::min(version.indexLimits[found].minOffset, offset);
                    version.indexLimits[found].maxOffset = std::max(version.indexLimits[found].maxOffset, offset);
                    version.comparisonResults.add(value, value->opcode() == Below || value->opcode() == Above);
                }
            }
        }
        for (auto& entry : version.comparisonResults) {
            Value* comparison = entry.key;
            if (version.hoistedValueSet.contains(comparison))
                continue;
            Value* last = comparison->owner->last();
            version.numberOfOmittedValues += emitsInstructions(comparison) + (last->opcode() == Branch && last->child(0) == comparison && m_numberOfUses[comparison] == 1);
        }
        if (!version.hidesRanges && numberOfClonedValues - numberOfHoistedValues + version.numberOfValuesHoistedPastGuards > maxNumberOfClonedValues) {
            for (Value* guard : variantGuards) {
                if (guard->opcode() != Branch || !version.comparisonResults.contains(guard->child(0)))
                    return std::nullopt;
            }
        }
        return version;
    }

    static bool relationHoldsOnEntry(const Version& version)
    {
        CountedLoop loop = *version.countedLoop;
        if (Value* repeated = version.repeatedValues.get(loop.bound))
            loop.bound = repeated;
        return B3::relationHoldsOnEntry(loop);
    }

    std::optional<Poll> findPoll(const Version& version)
    {
        Poll result;
        result.limit = nullptr;
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                auto* patchpoint = value->as<PatchpointValue>();
                if (!patchpoint || !m_proc.loopPollGenerator() || patchpoint->generator() != m_proc.loopPollGenerator())
                    continue;
                if (result.limit)
                    return std::nullopt;
                result.limit = value;
            }
        }
        if (!result.limit || m_numberOfUses[result.limit] != 1)
            return std::nullopt;
        result.block = result.limit->owner;
        if (result.block->last()->opcode() != Branch || version.hoistedValueSet.contains(result.block->last()))
            return std::nullopt;
        result.condition = result.block->last()->child(0);
        if (result.condition->owner != result.block || m_numberOfUses[result.condition] != 1 || result.condition->numChildren() != 2)
            return std::nullopt;
        unsigned indexOfLimit = result.condition->child(0) == result.limit ? 0 : 1;
        if (result.condition->child(indexOfLimit) != result.limit || !result.condition->child(1 - indexOfLimit)->hasInt64())
            return std::nullopt;
        unsigned indexOfSlowPath = result.block->successor(0).frequency() == FrequencyClass::Rare ? 0 : 1;
        if (result.block->successor(indexOfSlowPath).frequency() != FrequencyClass::Rare)
            return std::nullopt;
        result.continuation = result.block->successorBlock(1 - indexOfSlowPath);
        return result;
    }

    void hoist(Version& version)
    {
        double frequency = version.preHeader->frequency();
        BasicBlock* block = m_proc.addBlock(frequency);
        version.fastVersionFirstPreHeader = block;
        for (Value* value : version.hoistedValues) {
            if (Value* repeated = version.repeatedValues.get(value)) {
                Value* repeatedClone = version.hoistedClones.get(repeated);
                version.hoistedClones.add(value, repeatedClone ? repeatedClone : repeated);
                continue;
            }
            Value* clone = m_proc.clone(value);
            for (Value*& child : clone->children()) {
                if (Value* replacement = version.hoistedClones.get(child))
                    child = replacement;
            }
            block->append(clone);
            if (value != value->owner->last()) {
                version.hoistedClones.add(value, clone);
                continue;
            }
            BasicBlock* next = m_proc.addBlock(frequency);
            block->successors() = value->owner->successors();
            BasicBlock* likelySuccessor = version.hoistedGuardLikelySuccessors.get(value);
            for (FrequentedBlock& successor : block->successors()) {
                if (successor.block() == likelySuccessor)
                    successor = FrequentedBlock(next);
                else if (value == version.unswitchedTerminal && successor.block() == version.nextVersionSuccessor)
                    successor = FrequentedBlock(m_versions[*version.nextVersionIndex].fastVersionFirstPreHeader);
                else
                    successor = FrequentedBlock(version.header, FrequencyClass::Rare);
            }
            block = next;
        }
        version.fastVersionLastPreHeader = block;
        if (!version.poll)
            return;
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        Value* hoistedBound = version.hoistedClones.get(loop.bound);
        Value* bound = hoistedBound ? hoistedBound : loop.bound;
        auto emitEntryGuard = [&](Opcode relation, Value* left, Value* right) {
            BasicBlock* next = m_proc.addBlock(frequency);
            block->appendNew<Value>(m_proc, Branch, origin, block->appendNew<Value>(m_proc, relation, origin, left, right));
            block->setSuccessors(FrequentedBlock(next), FrequentedBlock(version.header, FrequencyClass::Rare));
            block = next;
            version.fastVersionLastPreHeader = block;
        };
        auto constant = [&](int64_t value) {
            return block->appendNew<Const64Value>(m_proc, origin, value);
        };
        static constexpr int64_t maxLimit = 1ll << 62;
        if (loop.wrapsAroundInt32 && (version.isTopTested || !version.indexLimits.isEmpty())) {
            if (loop.increases())
                emitEntryGuard(LessEqual, bound, constant(std::numeric_limits<int32_t>::max() - maxIterationsPerStrip));
            else
                emitEntryGuard(GreaterEqual, bound, constant(std::numeric_limits<int32_t>::min() + maxIterationsPerStrip));
        }
        Value* clampedBound = bound;
        if (!version.indexLimits.isEmpty()) {
            if (loop.isUnsigned())
                emitEntryGuard(BelowEqual, bound, constant(maxLimit));
            for (auto& indexLimit : version.indexLimits) {
                Value* hoistedIndexLimit = version.hoistedClones.get(indexLimit.limit);
                Value* limit = hoistedIndexLimit ? hoistedIndexLimit : indexLimit.limit;
                emitEntryGuard(GreaterEqual, loop.start, constant(-indexLimit.minOffset));
                emitEntryGuard(BelowEqual, limit, constant(maxLimit));
                Value* last = block->appendNew<Value>(m_proc, Sub, origin, limit, constant(indexLimit.maxOffset + loop.includesEqual()));
                emitEntryGuard(GreaterEqual, last, constant(0));
                clampedBound = block->appendNew<Value>(m_proc, Select, origin, block->appendNew<Value>(m_proc, LessThan, origin, last, clampedBound), last, clampedBound);
            }
            if (!version.isTopTested)
                emitEntryGuard(loop.relation, loop.start, clampedBound);
            version.clampedBound = m_proc.addVariable(Int64);
            block->appendNew<VariableValue>(m_proc, Set, origin, version.clampedBound, clampedBound);
        }
        version.bound = m_proc.addVariable(Int64);
        version.stripEnd = m_proc.addVariable(Int64);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.bound, bound);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.stripEnd, appendStripEnd(m_proc, block, loop, loop.start, clampedBound, version.clampedBound ? version.isTopTested : !relationHoldsOnEntry(version)));
    }

    void splitEdgesIntoSlowPaths()
    {
        for (auto& version : m_versions) {
            for (BasicBlock* block : version.slowPathBlocks)
                m_slowPathBlocks.add(block);
        }
        unsigned numberOfBlocks = m_proc.size();
        for (unsigned i = 0; i < numberOfBlocks; ++i) {
            BasicBlock* block = m_proc[i];
            if (!block || block->numSuccessors() < 2 || m_slowPathBlocks.contains(block))
                continue;
            for (FrequentedBlock& successor : block->successors()) {
                if (!m_slowPathBlocks.contains(successor.block()))
                    continue;
                BasicBlock* edge = m_proc.addBlock(std::min(block->frequency() * (successor.frequency() == FrequencyClass::Rare ? originalLoopFrequencyScale : 1), successor.block()->frequency()));
                edge->appendNew<Value>(m_proc, Jump, block->last()->origin());
                edge->setSuccessors(FrequentedBlock(successor.block()));
                successor.block() = edge;
            }
        }
    }

    void demoteValuesEscapingHotBlocks()
    {
        m_proc.resetValueOwners();
        IndexSet<Value*> valuesToDemote;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Phi && m_hotBlocks.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && m_hotBlocks.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
        demoteValues(m_proc, valuesToDemote);
    }

    using Clones = UncheckedKeyHashMap<BasicBlock*, BasicBlock*>;

    void fillHotBlockClones(Version& version, const Clones& clones, BasicBlock* nextIterationHeader, Variable* inductionVariableNext, const Clones* excursion = nullptr)
    {
        fillClones(version, version.hotBlocks, clones, nextIterationHeader, inductionVariableNext, excursion, false);
    }

    void fillClones(Version& version, const Vector<BasicBlock*, 8>& blocks, const Clones& clones, BasicBlock* nextIterationHeader, Variable* inductionVariableNext, const Clones* excursion, bool isExcursion)
    {
        const IndexSet<Value*>& hoistedValueSet = isExcursion ? version.hoistedValuesValidInExcursion : version.hoistedValueSet;
        for (BasicBlock* block : blocks) {
            BasicBlock* blockClone = clones.get(block);
            UncheckedKeyHashMap<Value*, Value*> map;
            for (Value* value : *block) {
                if (!isExcursion && version.omittedValues.contains(value))
                    continue;
                if (hoistedValueSet.contains(value)) {
                    if (Value* hoisted = version.hoistedClones.get(value))
                        map.add(value, hoisted);
                    else
                        blockClone->appendNew<Value>(m_proc, Jump, value->origin());
                    continue;
                }
                if (auto outcome = version.comparisonResults.find(value); !isExcursion && outcome != version.comparisonResults.end()) {
                    map.add(value, blockClone->appendNew<Const32Value>(m_proc, value->origin(), outcome->value));
                    continue;
                }
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
                }
                if (value->type() != Void)
                    map.add(value, clone);
                blockClone->append(clone);
                if (inductionVariableNext && value == (version.isTopTested ? version.countedLoop->inductionVariable : version.countedLoop->next))
                    blockClone->appendNew<VariableValue>(m_proc, Set, value->origin(), inductionVariableNext, clone);
            }
            if (hoistedValueSet.contains(block->last()))
                blockClone->setSuccessors(FrequentedBlock(version.hoistedGuardLikelySuccessors.get(block->last())));
            else
                blockClone->successors() = block->successors();
            for (BasicBlock*& successor : blockClone->successorBlocks()) {
                if (successor == version.header)
                    successor = nextIterationHeader;
                else if (BasicBlock* successorClone = clones.get(successor))
                    successor = successorClone;
                else if (BasicBlock* successorClone = excursion ? excursion->get(successor) : nullptr)
                    successor = successorClone;
            }
        }
    }

    void cloneHotBlocks(Version& version)
    {
        Clones clones;
        for (BasicBlock* block : version.hotBlocks)
            clones.add(block, m_proc.addBlock(m_originalFrequencies.get(block)));
        Variable* inductionVariableNext = version.poll ? m_proc.addVariable(Int64) : nullptr;
        Clones excursion;
        BasicBlock* excursionEnd = nullptr;
        if (!version.excursionBlocks.isEmpty()) {
            double frequency = 0;
            for (BasicBlock* block : version.excursionBlocks) {
                if (!clones.contains(block))
                    frequency = std::max(frequency, m_originalFrequencies.get(block));
            }
            for (BasicBlock* block : version.excursionBlocks)
                excursion.add(block, m_proc.addBlock(std::min(frequency, m_originalFrequencies.get(block))));
            excursionEnd = m_proc.addBlock(frequency);
            fillClones(version, version.excursionBlocks, excursion, excursionEnd, inductionVariableNext, nullptr, true);
        }
        fillHotBlockClones(version, clones, clones.get(version.header), inductionVariableNext, &excursion);
        BasicBlock* narrowingFailureBlock = version.header;
        if (!version.phisNarrowedOnEntry.isEmpty()) {
            Clones firstIteration;
            for (BasicBlock* block : version.hotBlocks)
                firstIteration.add(block, m_proc.addBlock(m_originalFrequencies.get(block) * originalLoopFrequencyScale));
            fillHotBlockClones(version, firstIteration, clones.get(version.header), nullptr);
            narrowingFailureBlock = firstIteration.get(version.header);
        }
        for (BasicBlock* block : version.hotBlocks)
            block->setFrequency(m_originalFrequencies.get(block) * originalLoopFrequencyScale);
        for (BasicBlock* block : version.excursionBlocks)
            block->setFrequency(m_originalFrequencies.get(block) * originalLoopFrequencyScale);
        if (version.poll)
            stripMine(version, clones, excursion, inductionVariableNext);
        auto narrowPhis = [&](BasicBlock* block) {
            for (auto [phi, extension] : version.phisNarrowedOnEntry) {
                auto* read = phi->opcode() == Identity ? phi->child(0)->as<VariableValue>() : nullptr;
                if (!read || read->opcode() != Get)
                    continue;
                BasicBlock* next = m_proc.addBlock(block->frequency());
                Value* entered = block->appendNew<VariableValue>(m_proc, Get, phi->origin(), read->variable());
                Value* narrowed = block->appendNew<Value>(m_proc, extension, phi->origin(), block->appendNew<Value>(m_proc, Trunc, phi->origin(), entered));
                block->appendNew<Value>(m_proc, Branch, phi->origin(), block->appendNew<Value>(m_proc, Equal, phi->origin(), narrowed, entered));
                block->setSuccessors(FrequentedBlock(next), FrequentedBlock(narrowingFailureBlock, FrequencyClass::Rare));
                next->appendNew<VariableValue>(m_proc, Set, phi->origin(), read->variable(), narrowed);
                block = next;
            }
            return block;
        };
        version.fastVersionLastPreHeader = narrowPhis(version.fastVersionLastPreHeader);
        version.fastVersionLastPreHeader->appendNew<Value>(m_proc, Jump, version.header->at(0)->origin());
        version.fastVersionLastPreHeader->setSuccessors(FrequentedBlock(clones.get(version.header)));
        if (excursionEnd) {
            excursionEnd = narrowPhis(excursionEnd);
            excursionEnd->appendNew<Value>(m_proc, Jump, version.header->at(0)->origin());
            excursionEnd->setSuccessors(FrequentedBlock(clones.get(version.header)));
        }
        if (version.isTopTested)
            m_topTestedVersionHeaders.append(clones.get(version.header));
        if (version.isEnteredFromPreHeader)
            version.preHeader->replaceSuccessor(version.header, version.fastVersionFirstPreHeader);
    }

    void stripMine(Version& version, const Clones& clones, const Clones& excursion, Variable* inductionVariableNext)
    {
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        double frequency = loop.header->frequency() * originalLoopFrequencyScale;
        BasicBlock* test = clones.get(loop.test);
        BasicBlock* latch = clones.get(loop.latch);
        BasicBlock* realTest = m_proc.addBlock(version.preHeader->frequency());
        BasicBlock* stripPoll = m_proc.addBlock(frequency);
        BasicBlock* nextStrip = m_proc.addBlock(frequency);
        BasicBlock* originalLoopLatch = version.isTopTested ? version.header : m_proc.addBlock(frequency);
        auto get = [&](BasicBlock* block, Variable* variable) {
            return block->appendNew<VariableValue>(m_proc, Get, origin, variable);
        };

        BasicBlock* exit = test->successorBlock(1 - loop.latchSuccessorIndex);
        auto testStripEnd = [&](BasicBlock* test, BasicBlock* latch) {
            test->removeLast(m_proc);
            Value* nextAtTest = get(test, inductionVariableNext);
            test->appendNew<Value>(m_proc, Branch, origin, test->appendNew<Value>(m_proc, loop.relation, origin, nextAtTest, get(test, version.stripEnd)));
            test->setSuccessors(FrequentedBlock(latch), FrequentedBlock(realTest, version.isTopTested ? FrequencyClass::Rare : FrequencyClass::Normal));
        };
        testStripEnd(test, latch);
        if (!version.isTopTested && excursion.contains(loop.test) && excursion.contains(loop.latch))
            testStripEnd(excursion.get(loop.test), excursion.get(loop.latch));

        Value* nextAtRealTest = get(realTest, inductionVariableNext);
        realTest->appendNew<Value>(m_proc, Branch, origin, realTest->appendNew<Value>(m_proc, loop.relation, origin, nextAtRealTest, get(realTest, version.bound)));
        realTest->setSuccessors(FrequentedBlock(stripPoll, FrequencyClass::Rare), FrequentedBlock(exit));
        if (version.clampedBound) {
            BasicBlock* clampedBoundTest = m_proc.addBlock(frequency);
            realTest->replaceSuccessor(stripPoll, clampedBoundTest);
            Value* nextAtClampedBoundTest = get(clampedBoundTest, inductionVariableNext);
            clampedBoundTest->appendNew<Value>(m_proc, Branch, origin, clampedBoundTest->appendNew<Value>(m_proc, loop.relation, origin, nextAtClampedBoundTest, get(clampedBoundTest, version.clampedBound)));
            clampedBoundTest->setSuccessors(FrequentedBlock(stripPoll), FrequentedBlock(originalLoopLatch, FrequencyClass::Rare));
        }

        Value* limit = m_proc.clone(version.poll->limit);
        limit->as<PatchpointValue>()->setGenerator(RefPtr { m_proc.loopPollGenerator() });
        stripPoll->append(limit);
        version.poll->limit->as<PatchpointValue>()->setGenerator([generator = RefPtr { m_proc.loopPollGenerator() }](CCallHelpers& jit, const StackmapGenerationParams& params) {
            generator->run(jit, params);
        });
        Value* condition = m_proc.clone(version.poll->condition);
        for (Value*& child : condition->children())
            child = child == version.poll->limit ? limit : stripPoll->appendNew<Const64Value>(m_proc, origin, child->asInt64());
        stripPoll->append(condition);
        stripPoll->appendNew<Value>(m_proc, Branch, origin, condition);
        stripPoll->successors() = version.poll->block->successors();
        for (FrequentedBlock& successor : stripPoll->successors())
            successor = successor.block() == version.poll->continuation ? FrequentedBlock(nextStrip) : FrequentedBlock(originalLoopLatch, FrequencyClass::Rare);

        Value* nextAtNextStrip = get(nextStrip, inductionVariableNext);
        nextStrip->appendNew<VariableValue>(m_proc, Set, origin, version.stripEnd, appendStripEnd(m_proc, nextStrip, loop, nextAtNextStrip, get(nextStrip, version.clampedBound ? version.clampedBound : version.bound), false));
        nextStrip->appendNew<Value>(m_proc, Jump, origin);
        nextStrip->setSuccessors(FrequentedBlock(version.isTopTested ? clones.get(version.header) : latch));
        if (version.isTopTested)
            return;

        UncheckedKeyHashMap<Value*, Value*> map;
        for (Value* value : *loop.latch) {
            Value* clone = m_proc.clone(value);
            for (Value*& child : clone->children()) {
                if (Value* replacement = map.get(child))
                    child = replacement;
            }
            if (value->type() != Void)
                map.add(value, clone);
            originalLoopLatch->append(clone);
        }
        originalLoopLatch->setSuccessors(FrequentedBlock(loop.header));
    }

    Procedure& m_proc;
    Vector<BasicBlock*>& m_topTestedVersionHeaders;
    bool m_resolvesSingleInputPhis { false };
    UpsilonsByPhi m_upsilons;
    IndexMap<Value*, unsigned> m_numberOfUses;
    Vector<Version> m_versions;
    IndexSet<BasicBlock*> m_hotBlocks;
    UncheckedKeyHashMap<BasicBlock*, double> m_originalFrequencies;
    IndexSet<BasicBlock*> m_slowPathBlocks;
};

} // anonymous namespace

bool versionLoops(Procedure& proc, Vector<BasicBlock*>& topTestedVersionHeaders)
{
    PhaseScope phaseScope(proc, "versionLoops"_s);
    VersionLoops versionLoops(proc, topTestedVersionHeaders);
    return versionLoops.run();
}

bool foldOutOfRangeComparisons(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldOutOfRangeComparisons"_s);
    InsertionSet insertionSet(proc);
    bool changed = false;
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            Value* value = block->at(index);
            if (value->opcode() != Equal && value->opcode() != NotEqual)
                continue;
            for (unsigned i = 0; i < 2; ++i) {
                Value* extended = value->child(i);
                if (!value->child(1 - i)->hasInt64())
                    continue;
                int64_t constant = value->child(1 - i)->asInt64();
                bool isOutOfRange = extended->opcode() == SExt32 ? constant != static_cast<int32_t>(constant) : extended->opcode() == ZExt32 && constant != static_cast<int64_t>(static_cast<uint32_t>(constant));
                if (!isOutOfRange)
                    continue;
                value->replaceWithIdentity(insertionSet.insertIntConstant(index, value, value->opcode() == NotEqual));
                changed = true;
                break;
            }
        }
        insertionSet.execute(block);
    }
    return changed;
}

static bool narrowExtendedWordPhis(Procedure& proc, IndexSet<Value*>& narrowPhiExtensions)
{
    enum class Sources : uint8_t { ConstantsOnly, ExtendedWords, ZeroExtendedWords, Other };
    UncheckedKeyHashMap<Value*, Sources> sourcesOf;
    UncheckedKeyHashMap<Value*, std::pair<int64_t, int64_t>> constantsOf;
    bool isFirstRound = narrowPhiExtensions.isEmpty();
    IndexSet<Value*> phisFedNarrowPhiFromRareBlock;
    for (Value* value : proc.values()) {
        auto* upsilon = value->as<UpsilonValue>();
        if (!upsilon || upsilon->phi()->type() != Int64)
            continue;
        if (!isFirstRound)
            upsilon->performSubstitution();
        Value* source = upsilon->child(0);
        auto& sources = sourcesOf.add(upsilon->phi(), Sources::ConstantsOnly).iterator->value;
        if (narrowPhiExtensions.contains(source)) {
            if (source->owner->frequency() * maxRatioOfHeaderToLikelyBlock < upsilon->phi()->owner->frequency())
                phisFedNarrowPhiFromRareBlock.add(upsilon->phi());
            else
                sources = Sources::Other;
        }
        if (source->opcode() == SExt32)
            sources = sources == Sources::ZeroExtendedWords ? Sources::Other : std::max(sources, Sources::ExtendedWords);
        else if (source->opcode() == ZExt32)
            sources = sources == Sources::ExtendedWords ? Sources::Other : std::max(sources, Sources::ZeroExtendedWords);
        else if (source->hasInt64()) {
            auto& constants = constantsOf.add(upsilon->phi(), std::pair { source->asInt64(), source->asInt64() }).iterator->value;
            constants = { std::min(constants.first, source->asInt64()), std::max(constants.second, source->asInt64()) };
        } else
            sources = Sources::Other;
    }
    for (auto& [phi, constants] : constantsOf) {
        auto& sources = sourcesOf.find(phi)->value;
        if (sources == Sources::ExtendedWords && (constants.first < std::numeric_limits<int32_t>::min() || constants.second > std::numeric_limits<int32_t>::max()))
            sources = Sources::Other;
        if (sources == Sources::ZeroExtendedWords && (constants.first < 0 || constants.second > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())))
            sources = Sources::Other;
    }
    UncheckedKeyHashMap<Value*, Value*> narrowPhis;
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            Value* phi = block->at(index);
            if (phi->opcode() != Phi)
                continue;
            auto found = sourcesOf.find(phi);
            if (found == sourcesOf.end() || (found->value != Sources::ExtendedWords && found->value != Sources::ZeroExtendedWords))
                continue;
            if (!isFirstRound && !phisFedNarrowPhiFromRareBlock.contains(phi))
                continue;
            Value* narrow = insertionSet.insert<Value>(index, Phi, Int32, phi->origin());
            narrowPhis.add(phi, narrow);
            Value* extension = insertionSet.insert<Value>(index, found->value == Sources::ExtendedWords ? SExt32 : ZExt32, phi->origin(), narrow);
            extension->owner = block;
            narrowPhiExtensions.add(extension);
            phi->replaceWithIdentity(extension);
        }
        insertionSet.execute(block);
    }
    if (narrowPhis.isEmpty())
        return false;
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* upsilon = block->at(index)->as<UpsilonValue>();
            Value* narrow = upsilon ? narrowPhis.get(upsilon->phi()) : nullptr;
            if (!narrow)
                continue;
            Value* source = upsilon->child(0);
            Value* word = source->opcode() == SExt32 || source->opcode() == ZExt32 ? source->child(0) : insertionSet.insert<Const32Value>(index, upsilon->origin(), static_cast<int32_t>(source->asInt64()));
            insertionSet.insert<UpsilonValue>(index, upsilon->origin(), word, narrow);
            upsilon->replaceWithNop();
        }
        insertionSet.execute(block);
    }
    return true;
}

bool narrowExtendedWordPhis(Procedure& proc)
{
    PhaseScope phaseScope(proc, "narrowExtendedWordPhis"_s);
    proc.resetValueOwners();
    IndexSet<Value*> narrowPhiExtensions;
    bool changed = false;
    while (narrowExtendedWordPhis(proc, narrowPhiExtensions))
        changed = true;
    return changed;
}

bool eliminateRedundantPhiCycles(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateRedundantPhiCycles"_s);
    PhiChildren phiChildren(proc);
    const auto& phis = phiChildren.phis();
    if (phis.isEmpty())
        return false;

    IndexMap<Value*, Value*> representatives(proc.values().size());
    auto settle = [&] {
        bool changed = true;
        while (changed) {
            changed = false;
            for (Value* phi : phis) {
                if (representatives[phi] == phi)
                    continue;
                Value* result = nullptr;
                for (Value* child : phiChildren[phi].values()) {
                    Value* value = child->opcode() == Phi ? representatives[child] : child;
                    if (!value || value == phi)
                        continue;
                    if (!result) {
                        result = value;
                        continue;
                    }
                    if (result != value) {
                        result = phi;
                        break;
                    }
                }
                if (result && representatives[phi] != result) {
                    representatives[phi] = result;
                    changed = true;
                }
            }
        }
    };

    settle();
    bool mayChange = false;
    for (Value* phi : phis)
        mayChange |= representatives[phi] && representatives[phi] != phi;
    if (!mayChange)
        return false;
    proc.resetValueOwners();
    Dominators& dominators = proc.dominators();
    while (true) {
        settle();
        bool gaveUp = false;
        for (Value* phi : phis) {
            Value* representative = representatives[phi];
            if (representative == phi)
                continue;
            if (!representative || !dominators.strictlyDominates(representative->owner, phi->owner)) {
                representatives[phi] = phi;
                gaveUp = true;
            }
        }
        if (!gaveUp)
            break;
    }

    bool changed = false;
    for (Value* phi : phis) {
        Value* representative = representatives[phi];
        if (representative == phi)
            continue;
        for (Value* upsilon : phiChildren[phi])
            upsilon->replaceWithNop();
        phi->replaceWithIdentity(representative);
        changed = true;
    }
    if (!changed)
        return false;
    for (Value* value : proc.values())
        value->performSubstitution();
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
