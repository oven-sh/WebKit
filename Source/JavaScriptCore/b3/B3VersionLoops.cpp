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
    VersionLoops(Procedure& proc, Vector<BasicBlock*>& headersOfVersionsTestedAtTheTop)
        : m_proc(proc)
        , m_headersOfVersionsTestedAtTheTop(headersOfVersionsTestedAtTheTop)
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
            for (BasicBlock* block : version.blocksOfExcursion)
                m_originalFrequencies.add(block, block->frequency());
        }
        // Reverse order: a version's pre-header branches to the next version's.
        for (unsigned i = m_versions.size(); i--;)
            hoist(m_versions[i]);
        demoteValuesThatLeaveHotBlocks();
        for (auto& version : m_versions)
            cloneHotBlocks(version);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned maxNumberOfClonedValues = 48;
    static constexpr unsigned maxNumberOfClonedValuesWhereRangesAreHidden = 100;
    static constexpr unsigned maxNumberOfClonedValuesWhenUnswitching = 64;
    static constexpr unsigned maxNumberOfClonedValuesOfExcursion = 128;
    static constexpr unsigned minNumberOfHoistedValues = 2;
    static constexpr double frequencyScaleOfOriginalLoop = 1.0 / 1024;

    // The poll of a counted loop, which the fast version makes once per strip. See B3StripMineLoops.h.
    struct Poll {
        // Ends in the Branch on the condition, which compares the limit with a constant.
        BasicBlock* block;
        Value* limit;
        Value* condition;
        BasicBlock* continuation;
    };

    struct Version {
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        Vector<BasicBlock*, 8> hotBlocks;
        // In the order in which they execute. A terminal stands for a guard.
        Vector<Value*, 8> hoistedValues;
        IndexSet<Value*> hoistedValueSet;
        IndexSet<Value*> omittedValues;
        unsigned numberOfClonedValues { 0 };
        unsigned numberOfHoistedValues { 0 };
        std::optional<unsigned> nextVersionIndex;
        Value* unswitchedTerminal { nullptr };
        BasicBlock* successorOfNextVersion { nullptr };
        bool isEnteredFromPreHeader { true };
        UncheckedKeyHashMap<Value*, Value*> repeatedValues;
        UncheckedKeyHashMap<Value*, Value*> hoistedClones;
        UncheckedKeyHashMap<Value*, BasicBlock*> likelySuccessorsOfHoistedGuards;
        std::optional<CountedLoop> countedLoop;
        bool isTestedAtTheTop { false };
        UncheckedKeyHashMap<Value*, bool> outcomesOfComparisons;
        std::optional<Poll> poll;
        // With a poll: set in the last pre-header of the fast version.
        Variable* bound { nullptr };
        Variable* endOfStrip { nullptr };
        BasicBlock* firstPreHeaderOfFastVersion { nullptr };
        BasicBlock* lastPreHeaderOfFastVersion { nullptr };
        bool hidesRanges { false };
        Vector<std::pair<Value*, Opcode>, 2> phisNarrowedOnEntry;
        Vector<BasicBlock*, 8> blocksOfExcursion;
        IndexSet<Value*> hoistedValuesThatHoldInExcursion;
    };

    // A guard: a Branch, or a patchpoint that branches, with one successor that is rare or, if none is, leaves the loop.
    static BasicBlock* likelySuccessorOfGuard(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block)
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

    // Nothing depends on how a poll comes out.
    bool endsInPoll(BasicBlock* block) const
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
            for (BasicBlock* block : version.hotBlocks)
                m_hotBlocks.add(block);
            for (BasicBlock* block : version.blocksOfExcursion)
                m_hotBlocks.add(block);
            m_versions.append(WTF::move(version));
        };
        m_seesWhatIsOneValue = true;
        std::optional<Version> versionTestedAtTheTop = tryCreateVersion(loops, loop, preHeader, nullptr, nullptr, nullptr);
        m_seesWhatIsOneValue = false;
        UnswitchCandidate candidate;
        std::optional<Version> version = tryCreateVersion(loops, loop, preHeader, nullptr, nullptr, &candidate);
        if (versionTestedAtTheTop) {
            unsigned stays = versionTestedAtTheTop->numberOfClonedValues - versionTestedAtTheTop->numberOfHoistedValues;
            unsigned staysOtherwise = version ? version->numberOfClonedValues - version->numberOfHoistedValues : versionTestedAtTheTop->numberOfClonedValues;
            if (versionTestedAtTheTop->isTestedAtTheTop ? stays * 4 <= staysOtherwise * 3 : stays < staysOtherwise) {
                addVersion(WTF::move(*versionTestedAtTheTop));
                return;
            }
        }
        if (candidate.block) {
            std::optional<Version> first = tryCreateVersion(loops, loop, preHeader, candidate.block, candidate.successors[0], nullptr);
            std::optional<Version> second = first ? tryCreateVersion(loops, loop, preHeader, candidate.block, candidate.successors[1], nullptr) : std::nullopt;
            if (second && first->numberOfClonedValues + second->numberOfClonedValues <= maxNumberOfClonedValuesWhenUnswitching) {
                first->nextVersionIndex = m_versions.size() + 1;
                first->unswitchedTerminal = candidate.block->last();
                first->successorOfNextVersion = candidate.successors[1];
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
        RangeSet<HeapRange> readsOfHoistedValues;
        bool hoistedValuesReadPinned = false;
        IndexSet<Value*> holdInFastVersionOnly;
        for (Value* value : version.hoistedValues) {
            Effects effects = value->effects();
            if (effects.readsMutability != Mutability::Immutable)
                readsOfHoistedValues.add(effects.reads);
            hoistedValuesReadPinned |= effects.readsPinned;
            bool holdsInFastVersionOnly = value->opcode() == Phi;
            for (Value* child : value->children())
                holdsInFastVersionOnly |= holdInFastVersionOnly.contains(child);
            if (holdsInFastVersionOnly)
                holdInFastVersionOnly.add(value);
            else
                version.hoistedValuesThatHoldInExcursion.add(value);
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
                        canBeCloned &= canCloneForLoop(value) && !isCall(value) && !effects.fence && !(effects.writesPinned && hoistedValuesReadPinned) && !readsOfHoistedValues.overlaps(effects.writes);
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
                version.blocksOfExcursion.append(block);
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
        for (unsigned i = 0; i < version.blocksOfExcursion.size(); ++i) {
            BasicBlock* block = version.blocksOfExcursion[i];
            for (Value* value : *block)
                numberOfClonedValues += takesInstructions(value) && !version.hoistedValuesThatHoldInExcursion.contains(value);
            if (version.hoistedValuesThatHoldInExcursion.contains(block->last())) {
                add(version.likelySuccessorsOfHoistedGuards.get(block->last()));
                continue;
            }
            for (BasicBlock* successor : block->successorBlocks())
                add(successor);
        }
        if (numberOfClonedValues > maxNumberOfClonedValuesOfExcursion)
            version.blocksOfExcursion.clear();
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
        if (!hasBackEdge)
            return std::nullopt;

        RangeSet<HeapRange> writes;
        bool writesPinned = false;
        unsigned numberOfClonedValues = 0;
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || isCall(value))
                    return std::nullopt;
                numberOfClonedValues += takesInstructions(value);
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
                Opcode comesRoundAs = Oops;
                bool comesRoundOtherwise = false;
                Value* entering = nullptr;
                for (UpsilonValue* upsilon : *upsilons) {
                    if (!loops.belongsTo(upsilon->owner, loop))
                        entering = upsilon->child(0);
                    if (!hotBlocks.contains(upsilon->owner))
                        continue;
                    Opcode extension = isExtendedWordInFastVersion(upsilon->child(0));
                    if (extension == Oops || (comesRoundAs != Oops && comesRoundAs != extension))
                        comesRoundOtherwise = true;
                    else
                        comesRoundAs = extension;
                }
                if (comesRoundAs != Oops && !comesRoundOtherwise && entering && entering->opcode() != comesRoundAs && !entering->hasInt64())
                    version.phisNarrowedOnEntry.append({ value, comesRoundAs });
            }
        }
        if (!version.hidesRanges)
            version.phisNarrowedOnEntry.clear();
        if (numberOfClonedValues > maxNumberOfClonedValuesWhereRangesAreHidden)
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
        Vector<Value*, 16> valuesAheadOfFastLoop;
        {
            IndexSet<Value*> seen;
            Vector<Value*, 16> worklist;
            auto addChildrenAheadOfLoop = [&](Value* value) {
                for (Value* child : value->children()) {
                    if (takesInstructions(child) && !loops.belongsTo(child->owner, loop) && seen.add(child))
                        worklist.append(child);
                }
            };
            for (BasicBlock* block : version.hotBlocks) {
                for (Value* value : *block)
                    addChildrenAheadOfLoop(value);
            }
            for (Value* value : *preHeader) {
                if (value->opcode() == Upsilon)
                    addChildrenAheadOfLoop(value);
            }
            auto nothingHasWrittenSince = [&](Value* load, HeapRange reads) {
                static constexpr unsigned maxNumberOfBlocks = 4;
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
                if (effects.reads && effects.readsMutability != Mutability::Immutable && !(value->as<MemoryValue>() && nothingHasWrittenSince(value, effects.reads)))
                    continue;
                valuesAheadOfFastLoop.append(value);
                addChildrenAheadOfLoop(value);
            }
        }
        // A load from where a hoisted load loads from is as safe as that one, whatever guards it. By the hoisted load it repeats, so that what hangs on the two is seen to repeat too.
        UncheckedKeyHashMap<Value*, Value*> repeatedLoads;
        auto& repeatedValues = m_seesWhatIsOneValue ? version.repeatedValues : repeatedLoads;
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
            for (Value* hoisted : valuesAheadOfFastLoop) {
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
            for (Value* hoisted : valuesAheadOfFastLoop) {
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
        auto onlyInputInFastVersion = [&](Value* phi) -> Value* {
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
        auto liesBetweenHoistedLoads = [&](Value* value) {
            static constexpr int64_t minPageSize = 4096;
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad())
                return false;
            int64_t begin = load->offset();
            int64_t end = begin + load->accessByteSize();
            std::optional<int64_t> beginOfLower;
            std::optional<int64_t> endOfUpper;
            for (Value* hoisted : valuesAheadOfFastLoop) {
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
        UncheckedKeyHashMap<Value*, bool> conditionsOfHoistedGuards;
        BasicBlock* testAtTheTop = nullptr;
        IndexSet<BasicBlock*> blocksReachedWithoutTestAtTheTop;
        bool everyGuardSoFarIsHoisted = true;
        bool hoistsGuard = false;
        unsigned numberOfHoistedValues = 0;
        auto hoist = [&](Value* value) {
            version.hoistedValues.append(value);
            version.hoistedValueSet.add(value);
            valuesAheadOfFastLoop.append(value);
            numberOfHoistedValues += takesInstructions(value);
        };
        IndexSet<BasicBlock*> visited;
        for (BasicBlock* block = loop.header(); block && hotBlocks.contains(block) && visited.add(block);) {
            for (Value* value : *block) {
                if (value->opcode() == Phi) {
                    Value* only = block == loop.header() || !m_seesWhatIsOneValue ? nullptr : onlyInputInFastVersion(value);
                    if (only && isInvariant(only)) {
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
                if (!m_seesWhatIsOneValue) {
                    if (effects.controlDependent && !everyGuardSoFarIsHoisted && !repeatsHoistedLoad(value))
                        continue;
                    hoist(value);
                    continue;
                }
                if (!repeatsHoistedLoad(value) && effects.controlDependent && !everyGuardSoFarIsHoisted && !liesBetweenHoistedLoads(value))
                    continue;
                repeatsHoistedComputation(value);
                if (value->opcode() == Select) {
                    if (auto known = conditionsOfHoistedGuards.find(withoutRepetition(value->child(0))); known != conditionsOfHoistedGuards.end())
                        version.repeatedValues.add(value, withoutRepetition(value->child(known->value ? 1 : 2)));
                }
                hoist(value);
            }
            if (block->last()->opcode() == Jump) {
                block = block->successorBlock(0);
                continue;
            }
            BasicBlock* likelySuccessor = block == unswitchedBlock ? takenSuccessor : likelySuccessorOfGuard(loops, loop, block);
            if (!likelySuccessor) {
                if (candidate && hasInvariantChildren(block->last()))
                    findUnswitchCandidate(loops, loop, block, *candidate);
                break;
            }
            if (block == unswitchedBlock && !hasInvariantChildren(block->last()))
                return std::nullopt;
            if (hasInvariantChildren(block->last())) {
                hoist(block->last());
                hoistsGuard = true;
                version.likelySuccessorsOfHoistedGuards.add(block->last(), likelySuccessor);
                if (block->last()->opcode() == Branch)
                    conditionsOfHoistedGuards.add(withoutRepetition(block->last()->child(0)), likelySuccessor == block->successorBlock(0));
            } else if (!endsInPoll(block)) {
                everyGuardSoFarIsHoisted = false;
                if (!testAtTheTop && block != unswitchedBlock && block->last()->opcode() == Branch) {
                    const FrequentedBlock& other = block->successor(block->successorBlock(0) == likelySuccessor ? 1 : 0);
                    if (!loops.belongsTo(other.block(), loop) && !isRare(other, block))
                        testAtTheTop = block;
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
        if (!version.countedLoop && testAtTheTop && m_seesWhatIsOneValue) {
            Value* condition = testAtTheTop->last()->child(0);
            if (condition->opcode() == Phi && condition->owner != loop.header())
                condition = onlyInputInFastVersion(condition);
            if (condition) {
                version.countedLoop = analyzeCountedLoopTestedAtTheTop(loops, loop, preHeader, testAtTheTop, condition, m_upsilons, isInvariant, [&](BasicBlock* block) {
                    return !hotBlocks.contains(block);
                });
            }
            if (version.countedLoop) {
                BasicBlock* staying = version.countedLoop->latch;
                Vector<BasicBlock*, 8> worklist { loop.header() };
                blocksReachedWithoutTestAtTheTop.add(loop.header());
                while (!worklist.isEmpty()) {
                    BasicBlock* block = worklist.takeLast();
                    for (Value* value : *block) {
                        Effects effects = value->effects();
                        auto* upsilon = value->as<UpsilonValue>();
                        if (effects.writes || effects.writesPinned || effects.exitsSideways || effects.fence || value->opcode() == Set || (upsilon && upsilon->phi()->owner == loop.header()))
                            version.countedLoop = std::nullopt;
                    }
                    for (BasicBlock* successor : block->successorBlocks()) {
                        if (!hotBlocks.contains(successor) || (block == testAtTheTop && successor == staying))
                            continue;
                        if (blocksReachedWithoutTestAtTheTop.add(successor))
                            worklist.append(successor);
                    }
                }
            }
            version.isTestedAtTheTop = !!version.countedLoop;
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
                if (version.isTestedAtTheTop && blocksReachedWithoutTestAtTheTop.contains(block))
                    continue;
                for (Value* value : *block) {
                    if (value->numChildren() != 2)
                        continue;
                    Value* left = isBound(value->child(0)) ? bound : value->child(0);
                    Value* right = isBound(value->child(1)) ? bound : value->child(1);
                    if (auto outcome = outcomeOfComparisonWithBound(*version.countedLoop, value->opcode(), left, right, bound))
                        version.outcomesOfComparisons.add(value, *outcome);
                }
            }
            if (!version.isTestedAtTheTop && !relationHoldsOnEntry(*version.countedLoop))
                version.outcomesOfComparisons.clear();
        }
        if (version.countedLoop && hotBlocks.contains(version.countedLoop->test) && hotBlocks.contains(version.countedLoop->latch))
            version.poll = findPoll(version);
        if (version.poll) {
            version.omittedValues.add(version.poll->limit);
            version.omittedValues.add(version.poll->condition);
            version.hoistedValueSet.add(version.poll->block->last());
            version.likelySuccessorsOfHoistedGuards.add(version.poll->block->last(), version.poll->continuation);
        }
        return version;
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
        version.firstPreHeaderOfFastVersion = block;
        for (Value* value : version.hoistedValues) {
            if (Value* repeated = version.repeatedValues.get(value)) {
                Value* cloneOfRepeated = version.hoistedClones.get(repeated);
                version.hoistedClones.add(value, cloneOfRepeated ? cloneOfRepeated : repeated);
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
            BasicBlock* likelySuccessor = version.likelySuccessorsOfHoistedGuards.get(value);
            for (FrequentedBlock& successor : block->successors()) {
                if (successor.block() == likelySuccessor)
                    successor = FrequentedBlock(next);
                else if (value == version.unswitchedTerminal && successor.block() == version.successorOfNextVersion)
                    successor = FrequentedBlock(m_versions[*version.nextVersionIndex].firstPreHeaderOfFastVersion);
                else
                    successor = FrequentedBlock(version.header, FrequencyClass::Rare);
            }
            block = next;
        }
        version.lastPreHeaderOfFastVersion = block;
        if (!version.poll)
            return;
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        Value* hoistedBound = version.hoistedClones.get(loop.bound);
        Value* bound = hoistedBound ? hoistedBound : loop.bound;
        version.bound = m_proc.addVariable(Int64);
        version.endOfStrip = m_proc.addVariable(Int64);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.bound, bound);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.endOfStrip, appendEndOfStrip(m_proc, block, loop, loop.start, bound, !relationHoldsOnEntry(loop)));
    }

    void demoteValuesThatLeaveHotBlocks()
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

    void fillClonesOfHotBlocks(Version& version, const Clones& clones, BasicBlock* headerOfNextTurn, Variable* nextOfInductionVariable, const Clones* excursion = nullptr)
    {
        fillClones(version, version.hotBlocks, clones, headerOfNextTurn, nextOfInductionVariable, excursion, false);
    }

    void fillClones(Version& version, const Vector<BasicBlock*, 8>& blocks, const Clones& clones, BasicBlock* headerOfNextTurn, Variable* nextOfInductionVariable, const Clones* excursion, bool isExcursion)
    {
        const IndexSet<Value*>& hoistedValueSet = isExcursion ? version.hoistedValuesThatHoldInExcursion : version.hoistedValueSet;
        for (BasicBlock* block : blocks) {
            BasicBlock* cloneOfBlock = clones.get(block);
            UncheckedKeyHashMap<Value*, Value*> map;
            for (Value* value : *block) {
                if (!isExcursion && version.omittedValues.contains(value))
                    continue;
                if (hoistedValueSet.contains(value)) {
                    if (Value* hoisted = version.hoistedClones.get(value))
                        map.add(value, hoisted);
                    else
                        cloneOfBlock->appendNew<Value>(m_proc, Jump, value->origin());
                    continue;
                }
                if (auto outcome = version.outcomesOfComparisons.find(value); !isExcursion && outcome != version.outcomesOfComparisons.end()) {
                    map.add(value, cloneOfBlock->appendNew<Const32Value>(m_proc, value->origin(), outcome->value));
                    continue;
                }
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
                }
                if (value->type() != Void)
                    map.add(value, clone);
                cloneOfBlock->append(clone);
                if (nextOfInductionVariable && value == (version.isTestedAtTheTop ? version.countedLoop->inductionVariable : version.countedLoop->next))
                    cloneOfBlock->appendNew<VariableValue>(m_proc, Set, value->origin(), nextOfInductionVariable, clone);
            }
            if (hoistedValueSet.contains(block->last()))
                cloneOfBlock->setSuccessors(FrequentedBlock(version.likelySuccessorsOfHoistedGuards.get(block->last())));
            else
                cloneOfBlock->successors() = block->successors();
            for (BasicBlock*& successor : cloneOfBlock->successorBlocks()) {
                if (successor == version.header)
                    successor = headerOfNextTurn;
                else if (BasicBlock* cloneOfSuccessor = clones.get(successor))
                    successor = cloneOfSuccessor;
                else if (BasicBlock* cloneOfSuccessor = excursion ? excursion->get(successor) : nullptr)
                    successor = cloneOfSuccessor;
            }
        }
    }

    void cloneHotBlocks(Version& version)
    {
        Clones clones;
        for (BasicBlock* block : version.hotBlocks)
            clones.add(block, m_proc.addBlock(m_originalFrequencies.get(block)));
        Variable* nextOfInductionVariable = version.poll ? m_proc.addVariable(Int64) : nullptr;
        Clones excursion;
        BasicBlock* endOfExcursion = nullptr;
        if (!version.blocksOfExcursion.isEmpty()) {
            double frequency = 0;
            for (BasicBlock* block : version.blocksOfExcursion) {
                if (!clones.contains(block))
                    frequency = std::max(frequency, m_originalFrequencies.get(block));
            }
            for (BasicBlock* block : version.blocksOfExcursion)
                excursion.add(block, m_proc.addBlock(std::min(frequency, m_originalFrequencies.get(block))));
            endOfExcursion = m_proc.addBlock(frequency);
            fillClones(version, version.blocksOfExcursion, excursion, endOfExcursion, nextOfInductionVariable, nullptr, true);
        }
        fillClonesOfHotBlocks(version, clones, clones.get(version.header), nextOfInductionVariable, &excursion);
        BasicBlock* whereNarrowingFails = version.header;
        if (!version.phisNarrowedOnEntry.isEmpty()) {
            Clones firstTurn;
            for (BasicBlock* block : version.hotBlocks)
                firstTurn.add(block, m_proc.addBlock(m_originalFrequencies.get(block) * frequencyScaleOfOriginalLoop));
            fillClonesOfHotBlocks(version, firstTurn, clones.get(version.header), nullptr);
            whereNarrowingFails = firstTurn.get(version.header);
        }
        for (BasicBlock* block : version.hotBlocks)
            block->setFrequency(m_originalFrequencies.get(block) * frequencyScaleOfOriginalLoop);
        for (BasicBlock* block : version.blocksOfExcursion)
            block->setFrequency(m_originalFrequencies.get(block) * frequencyScaleOfOriginalLoop);
        if (version.poll)
            stripMine(version, clones, excursion, nextOfInductionVariable);
        if (endOfExcursion) {
            Origin origin = version.header->at(0)->origin();
            BasicBlock* whereExcursionComesBack = m_proc.addBlock(version.lastPreHeaderOfFastVersion->frequency());
            version.lastPreHeaderOfFastVersion->appendNew<Value>(m_proc, Jump, origin);
            version.lastPreHeaderOfFastVersion->setSuccessors(FrequentedBlock(whereExcursionComesBack));
            version.lastPreHeaderOfFastVersion = whereExcursionComesBack;
            endOfExcursion->appendNew<Value>(m_proc, Jump, origin);
            endOfExcursion->setSuccessors(FrequentedBlock(whereExcursionComesBack));
        }
        for (auto [phi, extension] : version.phisNarrowedOnEntry) {
            auto* read = phi->opcode() == Identity ? phi->child(0)->as<VariableValue>() : nullptr;
            if (!read || read->opcode() != Get)
                continue;
            BasicBlock* block = version.lastPreHeaderOfFastVersion;
            BasicBlock* next = m_proc.addBlock(block->frequency());
            Value* entered = block->appendNew<VariableValue>(m_proc, Get, phi->origin(), read->variable());
            Value* narrowed = block->appendNew<Value>(m_proc, extension, phi->origin(), block->appendNew<Value>(m_proc, Trunc, phi->origin(), entered));
            block->appendNew<Value>(m_proc, Branch, phi->origin(), block->appendNew<Value>(m_proc, Equal, phi->origin(), narrowed, entered));
            block->setSuccessors(FrequentedBlock(next), FrequentedBlock(whereNarrowingFails, FrequencyClass::Rare));
            next->appendNew<VariableValue>(m_proc, Set, phi->origin(), read->variable(), narrowed);
            version.lastPreHeaderOfFastVersion = next;
        }
        version.lastPreHeaderOfFastVersion->appendNew<Value>(m_proc, Jump, version.header->at(0)->origin());
        version.lastPreHeaderOfFastVersion->setSuccessors(FrequentedBlock(clones.get(version.header)));
        if (version.isTestedAtTheTop)
            m_headersOfVersionsTestedAtTheTop.append(clones.get(version.header));
        if (version.isEnteredFromPreHeader)
            version.preHeader->replaceSuccessor(version.header, version.firstPreHeaderOfFastVersion);
    }

    // As B3::stripMineLoops() does it, on variables. Where a stop is requested the original loop goes on, which polls every time around.
    void stripMine(Version& version, const Clones& clones, const Clones& excursion, Variable* nextOfInductionVariable)
    {
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        double frequency = loop.header->frequency() * frequencyScaleOfOriginalLoop;
        BasicBlock* test = clones.get(loop.test);
        BasicBlock* latch = clones.get(loop.latch);
        BasicBlock* realTest = m_proc.addBlock(version.preHeader->frequency());
        BasicBlock* pollOfStrip = m_proc.addBlock(frequency);
        BasicBlock* nextStrip = m_proc.addBlock(frequency);
        BasicBlock* latchOfOriginalLoop = version.isTestedAtTheTop ? version.header : m_proc.addBlock(frequency);
        // By now a value of another block is only to be had from a variable.
        auto get = [&](BasicBlock* block, Variable* variable) {
            return block->appendNew<VariableValue>(m_proc, Get, origin, variable);
        };

        BasicBlock* exit = test->successorBlock(1 - loop.indexOfLatchInSuccessors);
        auto testEndOfStrip = [&](BasicBlock* test, BasicBlock* latch) {
            test->removeLast(m_proc);
            Value* nextAtTest = get(test, nextOfInductionVariable);
            test->appendNew<Value>(m_proc, Branch, origin, test->appendNew<Value>(m_proc, loop.relation, origin, nextAtTest, get(test, version.endOfStrip)));
            test->setSuccessors(FrequentedBlock(latch), FrequentedBlock(realTest, version.isTestedAtTheTop ? FrequencyClass::Rare : FrequencyClass::Normal));
        };
        testEndOfStrip(test, latch);
        if (!version.isTestedAtTheTop && excursion.contains(loop.test) && excursion.contains(loop.latch))
            testEndOfStrip(excursion.get(loop.test), excursion.get(loop.latch));

        Value* nextAtRealTest = get(realTest, nextOfInductionVariable);
        realTest->appendNew<Value>(m_proc, Branch, origin, realTest->appendNew<Value>(m_proc, loop.relation, origin, nextAtRealTest, get(realTest, version.bound)));
        realTest->setSuccessors(FrequentedBlock(pollOfStrip, FrequencyClass::Rare), FrequentedBlock(exit));

        Value* limit = m_proc.clone(version.poll->limit);
        limit->as<PatchpointValue>()->setGenerator(RefPtr { m_proc.loopPollGenerator() });
        pollOfStrip->append(limit);
        // A stop request is handed over to the original loop, which therefore has to go on polling every time around: stripMineLoops() must not know its poll.
        version.poll->limit->as<PatchpointValue>()->setGenerator([generator = RefPtr { m_proc.loopPollGenerator() }](CCallHelpers& jit, const StackmapGenerationParams& params) {
            generator->run(jit, params);
        });
        Value* condition = m_proc.clone(version.poll->condition);
        for (Value*& child : condition->children())
            child = child == version.poll->limit ? limit : pollOfStrip->appendNew<Const64Value>(m_proc, origin, child->asInt64());
        pollOfStrip->append(condition);
        pollOfStrip->appendNew<Value>(m_proc, Branch, origin, condition);
        pollOfStrip->successors() = version.poll->block->successors();
        for (FrequentedBlock& successor : pollOfStrip->successors())
            successor = successor.block() == version.poll->continuation ? FrequentedBlock(nextStrip) : FrequentedBlock(latchOfOriginalLoop, FrequencyClass::Rare);

        Value* nextAtNextStrip = get(nextStrip, nextOfInductionVariable);
        nextStrip->appendNew<VariableValue>(m_proc, Set, origin, version.endOfStrip, appendEndOfStrip(m_proc, nextStrip, loop, nextAtNextStrip, get(nextStrip, version.bound), false));
        nextStrip->appendNew<Value>(m_proc, Jump, origin);
        nextStrip->setSuccessors(FrequentedBlock(version.isTestedAtTheTop ? clones.get(version.header) : latch));
        if (version.isTestedAtTheTop)
            return;

        // After the demotion a block refers to no value of another block of the loop.
        UncheckedKeyHashMap<Value*, Value*> map;
        for (Value* value : *loop.latch) {
            Value* clone = m_proc.clone(value);
            for (Value*& child : clone->children()) {
                if (Value* replacement = map.get(child))
                    child = replacement;
            }
            if (value->type() != Void)
                map.add(value, clone);
            latchOfOriginalLoop->append(clone);
        }
        latchOfOriginalLoop->setSuccessors(FrequentedBlock(loop.header));
    }

    Procedure& m_proc;
    Vector<BasicBlock*>& m_headersOfVersionsTestedAtTheTop;
    bool m_seesWhatIsOneValue { false };
    UpsilonsByPhi m_upsilons;
    IndexMap<Value*, unsigned> m_numberOfUses;
    Vector<Version> m_versions;
    IndexSet<BasicBlock*> m_hotBlocks;
    UncheckedKeyHashMap<BasicBlock*, double> m_originalFrequencies;
};

} // anonymous namespace

bool versionLoops(Procedure& proc, Vector<BasicBlock*>& headersOfVersionsTestedAtTheTop)
{
    PhaseScope phaseScope(proc, "versionLoops"_s);
    VersionLoops versionLoops(proc, headersOfVersionsTestedAtTheTop);
    return versionLoops.run();
}

bool foldComparisonsWithWhatIsOutOfRange(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldComparisonsWithWhatIsOutOfRange"_s);
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

static bool narrowPhisOfExtendedWordsOnce(Procedure& proc)
{
    enum class Sources : uint8_t { ConstantsOnly, ExtendedWords, ZeroExtendedWords, Other };
    UncheckedKeyHashMap<Value*, Sources> sourcesOf;
    UncheckedKeyHashMap<Value*, std::pair<int64_t, int64_t>> constantsOf;
    for (Value* value : proc.values()) {
        auto* upsilon = value->as<UpsilonValue>();
        if (!upsilon || upsilon->phi()->type() != Int64)
            continue;
        Value* source = upsilon->child(0);
        auto& sources = sourcesOf.add(upsilon->phi(), Sources::ConstantsOnly).iterator->value;
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
            Value* narrow = insertionSet.insert<Value>(index, Phi, Int32, phi->origin());
            narrowPhis.add(phi, narrow);
            phi->replaceWithIdentity(insertionSet.insert<Value>(index, found->value == Sources::ExtendedWords ? SExt32 : ZExt32, phi->origin(), narrow));
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
    for (Value* value : proc.values())
        value->performSubstitution();
    return true;
}

bool narrowPhisOfExtendedWords(Procedure& proc)
{
    PhaseScope phaseScope(proc, "narrowPhisOfExtendedWords"_s);
    bool changed = false;
    while (narrowPhisOfExtendedWordsOnce(proc))
        changed = true;
    return changed;
}

bool eliminateRedundantPhiCycles(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateRedundantPhiCycles"_s);
    for (Value* value : proc.values())
        value->performSubstitution();
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
