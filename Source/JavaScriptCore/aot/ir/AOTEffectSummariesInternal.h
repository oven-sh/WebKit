/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBorrowedParameters.h"
#include "AOTCompactList.h"
#include "AOTEffectSummaries.h"
#include "AOTStronglyConnectedComponents.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/MallocPtr.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class PhaseTimes;

namespace Typed {

class BodyReferenceCollector;
class BodyReferences;

enum class SummaryPart : uint8_t { OnReturn, OnThrow, Parameter, UnknownCode };

struct SummaryNode {
    FunctionID function;
    uint32_t parameter : 30 { 0 };
    SummaryPart part : 2 { SummaryPart::UnknownCode };

    static SummaryNode onReturn(FunctionID function) { return { function, 0, SummaryPart::OnReturn }; }
    static SummaryNode onThrow(FunctionID function) { return { function, 0, SummaryPart::OnThrow }; }
    static SummaryNode parameterOf(FunctionID function, uint32_t parameter)
    {
        RELEASE_ASSERT(parameter < 1u << 30);
        return { function, parameter, SummaryPart::Parameter };
    }
    static SummaryNode unknownCode() { return { }; }

    friend bool operator==(const SummaryNode&, const SummaryNode&) = default;
};
static_assert(sizeof(SummaryNode) == 8);

struct SummaryDependency {
    SummaryNode source;
    SummaryNode target;

    friend bool operator==(const SummaryDependency&, const SummaryDependency&) = default;
};

struct BodySummary {
    Effects onReturn { Effects::none() };
    Effects onThrow { Effects::none() };
    Vector<ParameterSummary> parameters;
    Vector<SummaryDependency> dependencies;
    UnknownCodeWitness unknownCodeWitness;
    bool followsFieldsOfOperands { false };

    friend bool operator==(const BodySummary&, const BodySummary&) = default;
};

// A BodySummary whose masks are IDs of a table and whose lists share one allocation. Two of them compare only if they use the same table.
// The dependencies are grouped by the function of their source, ascending, unknown code first. A group is a word for the function (0 for unknown code, else its index + 1), the number of its dependencies,
// and for each of them the node of the source and the node of the target, where 0 is OnReturn, 1 is OnThrow and 2 + n is parameter n: two halves of a word, or two words if one body has a node past 65535.
class StoredBodySummary {
    WTF_MAKE_NONCOPYABLE(StoredBodySummary);
public:
    StoredBodySummary() = default;
    StoredBodySummary(StoredBodySummary&&) = default;
    StoredBodySummary& operator=(StoredBodySummary&&) = default;
    StoredBodySummary(InternedLocationMasks&, FunctionID, const BodySummary&);

    // The dependencies whose sources are nodes of one function.
    class DependencyGroup {
    public:
        DependencyGroup(std::span<const uint32_t> words, bool isWide)
            : m_words(words)
            , m_isWide(isWide)
        {
        }

        void forEach(NOESCAPE const Invocable<void(uint32_t sourceNode, uint32_t targetNode)> auto& functor) const
        {
            if (m_isWide) {
                for (size_t position = 0; position < m_words.size(); position += 2)
                    functor(m_words[position], m_words[position + 1]);
                return;
            }
            for (uint32_t word : m_words)
                functor(word >> 16, word & 0xffff);
        }

    private:
        std::span<const uint32_t> m_words;
        bool m_isWide;
    };

    CompactEffects onReturn() const { return m_onReturn; }
    CompactEffects onThrow() const { return m_onThrow; }
    void setOnReturn(CompactEffects effects) { m_onReturn = effects; }
    void setOnThrow(CompactEffects effects) { m_onThrow = effects; }
    uint32_t numberOfParameters() const { return m_numberOfParameters; }
    uint32_t numberOfDependencies() const { return m_numberOfDependencies; }
    bool followsFieldsOfOperands() const { return m_followsFieldsOfOperands; }
    UnknownCodeWitness unknownCodeWitness(FunctionID function) const { return m_hasWitness ? UnknownCodeWitness { function, m_witness } : UnknownCodeWitness(); }

    std::span<const StoredParameterSummary> parameters() const LIFETIME_BOUND { return { m_lists.get(), m_numberOfParameters }; }
    std::span<StoredParameterSummary> mutableParameters() LIFETIME_BOUND { return { m_lists.get(), m_numberOfParameters }; }

    // Unknown code has no function and comes first. The functions ascend.
    void forEachGroupOfDependencies(NOESCAPE const Invocable<void(FunctionID source, const DependencyGroup&)> auto& functor) const
    {
        std::span<const uint32_t> words = wordsOfDependencies();
        unsigned wordsInDependency = m_hasWideDependencies ? 2 : 1;
        for (size_t position = 0; position < words.size();) {
            uint32_t source = words[position];
            size_t size = static_cast<size_t>(words[position + 1]) * wordsInDependency;
            functor(source ? FunctionID(source - 1) : FunctionID(), DependencyGroup(words.subspan(position + 2, size), m_hasWideDependencies));
            position += 2 + size;
        }
    }

    // The functions that the sources of the dependencies name: ascending, each once, without the function itself.
    void forEachFunctionInDependencies(FunctionID function, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
    {
        forEachGroupOfDependencies([&](FunctionID source, const DependencyGroup&) {
            if (source && source != function)
                functor(source);
        });
    }

    friend bool operator==(const StoredBodySummary&, const StoredBodySummary&);

private:
    std::span<const uint32_t> wordsOfDependencies() const LIFETIME_BOUND { return { reinterpret_cast<const uint32_t*>(m_lists.get() + m_numberOfParameters), m_numberOfWordsOfDependencies }; }
    size_t sizeOfLists() const { return m_numberOfParameters * sizeof(StoredParameterSummary) + m_numberOfWordsOfDependencies * sizeof(uint32_t); }

    CompactEffects m_onReturn;
    CompactEffects m_onThrow;
    // The parameters, then the words of the dependencies.
    MallocPtr<StoredParameterSummary> m_lists;
    uint32_t m_numberOfParameters : 30 { 0 };
    uint32_t m_hasWitness : 1 { 0 };
    uint32_t m_followsFieldsOfOperands : 1 { 0 };
    uint32_t m_numberOfDependencies { 0 };
    uint32_t m_numberOfWordsOfDependencies : 31 { 0 };
    uint32_t m_hasWideDependencies : 1 { 0 };
    ValueID m_witness;
};
static_assert(sizeof(StoredBodySummary) == 40);

struct StoredAddedEffects {
    FunctionID function;
    CompactEffects effects;
};

// What is kept from one computation of the summaries to the next: what each body does by itself, and the solution.
// The summary of a body reads the body, the records of the functions that its dependencies name, and records that all functions share.
// Whoever keeps this says which functions changed, among them those that name a shared record that changed.
// A body also reads what the seed says, which follows bodies any number of calls away. The summarizer keeps the seed and compares.
class BodySummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodySummaries);
    WTF_MAKE_NONCOPYABLE(BodySummaries);
public:
    BodySummaries() = default;

    // SayNoLess: what the records of the functions say of them to a caller holds as before, or more does.
    enum class Records : uint8_t { MaySayLess, SayNoLess };
    // OnlySetStorage: all that the edits wrote is where the cells that the functions allocate lie, which neither a summary nor the table of borrowed parameters reads.
    // What is kept of a body that was read as it stands is what reading it again gives. A body that was lowered since it was read, before the edits or behind them, counts as changed.
    enum class Edits : uint8_t { Any, OnlySetStorage };
    void didChange(std::span<const FunctionID>, Records = Records::MaySayLess, Edits = Edits::Any);
    // The functions do no more than before. What is kept of them stays, as an upper bound.
    void didLower(std::span<const FunctionID>);
    // The same of functions that the table of borrowed parameters still reads: it looks at them again.
    void didChangeDoingNoMore(std::span<const FunctionID>, Edits = Edits::Any);
    // The functions got operations that do this, somewhere, to any object they are handed.
    void didAddEffects(std::span<const AddedEffects>);
    // What is kept of the body also came from the records of layouts that the body does not name: those that the fields of an operand hold, and so on.
    bool followsFieldsOfOperands(FunctionID id) const { return id.index() < m_bodies.size() && m_bodies[id.index()].followsFieldsOfOperands(); }
    // False where all that is kept of the function is out of date already, so that to report it as changed once more, in whichever way, changes nothing.
    bool keepsSomethingOf(FunctionID id) const { return id.index() < m_isChanged.size() && !(m_isChanged[id.index()] && m_isChangedForTable[id.index()] && m_recordSays[id.index()] == RecordSays::MaybeLess); }

    // Whether no function was added or reported as changed since the summaries were brought up to date. Then the table and the seed are what a computation from nothing gives.
    bool describesFunctionsAsTheyAre(size_t numberOfFunctions) const { return m_bodies.size() == numberOfFunctions && !m_isChangedForTable.contains(1) && !m_isChangedForTableIfLowered.contains(1); }
    const BorrowedParameters& borrowedParameters() const LIFETIME_BOUND { return m_borrowed; }
    // The table of the functions as they are now, from the bodies that were reported as changed or made since the summaries were brought up to date, and what is kept of the others.
    BorrowedParameters computeBorrowedParametersOfChangedBodies(const Program&, BuildContext&);
    const EffectSummaries& seed() const LIFETIME_BOUND { return m_seed; }

private:
    friend class EffectSummarizer;

    static constexpr unsigned functionsInChunk = 256;

    void countLoweredBodiesWithNewStorageAsChanged();
    BorrowedParameters computeBorrowedParametersFromLatestTable(const Program&, BuildContext&, std::span<const uint64_t> stampsOfRecords, std::span<const uint8_t> recordLooksDifferent);
    void didTouch(FunctionID id)
    {
        if (id.index() < m_wasTouchedSinceLatestTable.size())
            m_wasTouchedSinceLatestTable[id.index()] = 1;
    }

    const Ref<InternedLocationMasks> m_masks { InternedLocationMasks::create() };
    Vector<StoredBodySummary> m_bodies;
    Vector<CompactList<FunctionID>> m_functionsOfClosuresMade;
    Vector<uint8_t> m_isChanged;
    // Those, and the functions that have changed and do no more than before.
    Vector<uint8_t> m_isChangedForTable;
    // The functions that edits which only set storage have kept out of the two lists above. One joins its list if it was lowered since it was summarized, or since it was scanned for the table.
    Vector<uint8_t> m_isChangedIfLowered;
    Vector<uint8_t> m_isChangedForTableIfLowered;
    enum class RecordSays : uint8_t { WhatItSaid, NoLess, MaybeLess };
    Vector<RecordSays> m_recordSays;
    Vector<uint8_t> m_wasLoweredSinceSummarized;
    bool m_hasUpperBounds { false };
    Vector<StoredAddedEffects> m_addedEffects;
    Vector<uint64_t> m_stampsOfRecords;
    Vector<uint64_t> m_stampsOfEntries;
    BorrowingBodies m_borrowingBodies;
    // Empty unless computeBorrowedParametersOfChangedBodies() has looked at the changed bodies since the summaries were brought up to date.
    // A body that nobody has touched since, and that reads no record that looks different since, is not looked at once more.
    Vector<uint8_t> m_wasTouchedSinceLatestTable;
    Vector<uint64_t> m_stampsOfRecordsAtLatestTable;
    BorrowedParameters m_borrowed;
    Vector<uint64_t> m_parametersWhoseContentsAreCalled;
    EffectSummaries m_seed { Ref { m_masks }, EffectSummaries::Contents::SayNothingOfBodies };

    // The solution. A node has what it takes in other than through unknown code, and remembers whether it takes in unknown code.
    bool m_isSolved { false };
    Vector<uint8_t> m_isEnteredByUnknownCode;
    Vector<uint32_t> m_firstNode;
    Vector<uint32_t> m_numberOfNodesOf;
    uint32_t m_numberOfNodes { 0 };
    Vector<CompactEffects> m_effects;
    Vector<uint8_t> m_takesInUnknownCode;
    // The functions whose dependencies name a function: as of the latest listUsers(), and those that came since. A function that no longer names it may stay listed.
    Vector<uint32_t> m_firstUser;
    Vector<FunctionID> m_users;
    UncheckedKeyHashMap<uint32_t, Vector<FunctionID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_usersAddedSince;
    // What the functions of a chunk that unknown code may enter do.
    Vector<Effects> m_effectsEnteredInChunk;
    Vector<uint8_t> m_takesInUnknownCodeInChunk;
    Effects m_unknownCode { Effects::unknownCode() };
};

enum class ReasonToSummarize : uint8_t { None, IsNew, HasChanged, HasNewSeed, NamesFunctionThatLooksDifferent, MakesClosureWithNewSeed };
inline constexpr unsigned numberOfReasonsToSummarize = static_cast<unsigned>(ReasonToSummarize::MakesClosureWithNewSeed) + 1;

struct ExitReachability {
    BitVector reachesReturn;
    BitVector reachesThrow;
};

// What the role of a function says it does, whatever its body looks like. False for a role that says nothing.
bool describeByRole(const Program&, const Function&, EffectDescription&);
// The rows of a serializer or a parser say more than the built-in they stand for: they name no operand types, and they write what the function has just allocated.
// Its summary is the one of the built-in before and after the summarizer sees the body, so a caller that has placed its tests by one of them is right by the other.
constexpr bool isSummarizedByRole(FunctionRole role) { return role == FunctionRole::Serializer || role == FunctionRole::Parser || role == FunctionRole::ValidatingParser; }

// What the seed takes from the types of the parameters. Bit i: the function borrows entry parameter i, the table names a call, and no value of the type of the parameter can be called.
// The seed says the same of a function as long as flow, the word of the table and this stay as they are.
uint64_t parametersWhoseContentsAreCalled(const Program&, FunctionID, const BorrowedParameters&);

ExitReachability computeExitReachability(const Function&);
// The collector is told the body where the summarizer walks it, which it does not do for every function.
BodySummary summarizeBodyLocally(const EffectContext&, FunctionID, FunctionAnalyses&, BodyReferenceCollector* = nullptr);
FunctionSummary createFunctionSummary(Effects onReturn, Effects onThrow, Vector<ParameterSummary>&&, UnknownCodeWitness);

FunctionSummary createSummaryFromFlow(const Program&, const FlowResults&, const BorrowedParameters&, BuildContext&, FunctionID);

class EffectSummarizer {
public:
    enum class Witnesses : uint8_t { Omitted, Found };

    EffectSummarizer(const Program&, const FlowResults&, BuildContext&, BodySummaries&, Witnesses);

    void setIsOnlyCheck() { m_isOnlyCheck = true; }
    // What the bodies that are summarized name goes there, so that the next call graph need not read them.
    void giveReferencesTo(BodyReferences* references) { m_references = references; }
    void update(EffectSummaries&);
    // The two steps of update(). Behind the first, BodySummaries::seed() and borrowedParameters() are those of the program as it is, and the second leaves them alone.
    void bringSeedUpToDate(EffectSummaries&);
    void finishUpdate(EffectSummaries&);

private:
    void findRecordsThatLookDifferent();
    void updateSeed(BorrowedParameters&&);
    Vector<ReasonToSummarize> findReasonsToSummarize() const;
    void summarizeBodies();
    using CountsByReason = std::array<uint64_t, numberOfReasonsToSummarize>;
    void logReasons(std::span<const ReasonToSummarize>, const CountsByReason& numberOfBodiesSummarizedAsBefore) const;
    void findFunctionsEnteredByUnknownCode();

    bool solveAgain(EffectSummaries&);
    size_t maxIncrementalSolveSize() const;
    void forEachUser(uint32_t function, NOESCAPE const Invocable<void(FunctionID)> auto&) const;
    void forEachSourceFunction(FunctionID user, NOESCAPE const Invocable<void(FunctionID)> auto&) const;
    bool hasEdgeFrom(FunctionID user, FunctionID function) const;
    Vector<uint8_t> markTransitiveUsers(std::span<const uint32_t> functions) const;
    std::optional<Vector<uint32_t>> addAffectedSources(std::span<const uint32_t> functions, Vector<uint8_t>& states, size_t limit) const;
    void numberNodesOfChangedFunctions();
    Vector<uint32_t> solveFunctions(std::span<const uint32_t> functions, Vector<uint32_t>& firstPositionOf);

    void solveAll(EffectSummaries&, PhaseTimes&);
    void numberAllNodes();
    void createNodes();
    void propagateEffects(PhaseTimes&);
    void takeInSources(uint32_t node);
    Vector<uint32_t> resolveNodesBelowCycles();
    void resolveNodesInAndAboveCycles(std::span<const uint32_t> nodes);
    void checkEffectsAgainstPlainSolve(std::span<const Effects> effectsOfNodesAlone) const;
    void propagateWitnesses();
    void listUsers();

    void uniteEffectsEnteredInChunk(unsigned chunk);
    bool updateEffectsOfUnknownCode();
    void prepareToAssemble();
    CompactEffects compactSolvedEffectsOf(uint32_t node);
    void assemble(EffectSummaries&, FunctionID);
    void checkAgainstSummariesFromNothing(const EffectSummaries&) const;
    void checkCoversSummariesFromNothing(const EffectSummaries&) const;

    Effects effectsOfUnknownCodeAlone() const;
    Effects solvedEffectsOf(uint32_t node) const;
    uint32_t indexOf(const SummaryNode&) const;
    uint32_t indexOf(FunctionID, uint32_t nodeOfFunction) const;
    uint32_t unknownCodeNode() const { return m_kept.m_numberOfNodes; }
    std::span<const uint32_t> sourcesOf(unsigned node) const LIFETIME_BOUND;

    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    BodySummaries& m_kept;
    const Witnesses m_witnessesWanted;
    bool m_isOnlyCheck { false };
    BodyReferences* m_references { nullptr };
    unsigned m_numberOfFunctionsBefore { 0 };
    Vector<uint8_t> m_recordLooksDifferent;
    // The record looks different in a way that may take something away from what whoever names the function has read in it.
    Vector<uint8_t> m_recordLooksDifferentToCallers;
    Vector<uint8_t> m_hasNewSeed;
    // Ascending. Their bodies do something else than before, or unknown code may enter them now and could not before, or the other way around.
    Vector<uint32_t> m_changedFunctions;
    // Ascending. The functions whose isEnteredByUnknownCode flipped.
    Vector<uint32_t> m_entryFlippedFunctions;
    // Ascending. The changed functions whose nodes were renumbered, so their previous solution is gone.
    Vector<uint32_t> m_renumberedFunctions;

    // An ID of the table for each ID that the table had when the memo was reset. Whoever computes an entry computes the same.
    class MemoByMasks {
    public:
        void reset(size_t size);
        uint32_t get(uint32_t id, NOESCAPE const Invocable<uint32_t()> auto& compute);

    private:
        Vector<std::atomic<uint32_t>> m_resultsPlusOne;
    };
    MemoByMasks m_readsWithUnknownCode;
    MemoByMasks m_writesWithUnknownCode;
    MemoByMasks m_masksOfPerObjectHeaps;
    CompactEffects m_compactUnknownCode;

    Vector<UnknownCodeWitness> m_witnesses;
    Vector<uint32_t> m_firstSource;
    Vector<uint32_t> m_endOfSources;
    Vector<uint32_t> m_sources;
    Vector<uint8_t> m_isResolved;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
