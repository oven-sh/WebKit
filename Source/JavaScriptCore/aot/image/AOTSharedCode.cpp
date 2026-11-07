/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTPhaseTimes.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct HashedArgumentMoves {
    uint64_t hash;
    KeptArgumentMoves moves;
};
static_assert(!VectorTraits<HashedArgumentMoves>::needsInitialization && !VectorTraits<KeptArgumentMoves>::needsInitialization, "the loops that fill a list of them are the first to touch its memory");

} // anonymous namespace

static bool comesFirstAmongArgumentMoves(KeptArgumentMoves a, KeptArgumentMoves b)
{
    return std::ranges::lexicographical_compare(a.span(), b.span());
}

static bool areSameArgumentMoves(KeptArgumentMoves a, KeptArgumentMoves b)
{
    std::span<const uint64_t> wordsOfA = a.span();
    std::span<const uint64_t> wordsOfB = b.span();
    if (wordsOfA.size() != wordsOfB.size())
        return false;
    for (size_t index = 0; index < wordsOfA.size(); ++index) {
        if (wordsOfA[index] != wordsOfB[index])
            return false;
    }
    return true;
}

static uint64_t hashOfArgumentMoves(std::span<const uint64_t> moves)
{
    uint64_t hash = moves.size();
    for (uint64_t word : moves)
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
    return hash;
}

static Vector<uint32_t> removeDuplicateArgumentMoves(const ParallelLoops& loops, const Vector<HashedArgumentMoves>& lists, Vector<uint32_t>& listKeptFor)
{
    size_t numberOfSlots = std::bit_ceil(lists.size() * 2 + 1);
    unsigned shift = 64 - std::countr_zero(numberOfSlots);
    Vector<uint32_t> slots;
    slots.grow(numberOfSlots);
    forEachRangeOfIndicesInParallel(loops, numberOfSlots, [&](size_t first, size_t end) {
        zeroSpan(slots.mutableSpan().subspan(first, end - first));
    });
    listKeptFor.grow(lists.size());
    return collectInParallel<uint32_t>(loops, static_cast<unsigned>(lists.size()), [&](unsigned index, Vector<uint32_t>& kept) {
        const HashedArgumentMoves& list = lists[index];
        for (size_t slot = list.hash >> shift;; slot = (slot + 1) & (numberOfSlots - 1)) {
            AtomicRef occupant(slots[slot]);
            uint32_t indexPlusOne = occupant.load(std::memory_order_relaxed);
            if (!indexPlusOne && occupant.compare_exchange_strong(indexPlusOne, index + 1, std::memory_order_relaxed)) {
                kept.append(index);
                listKeptFor[index] = index;
                return;
            }
            const HashedArgumentMoves& other = lists[indexPlusOne - 1];
            if (other.hash == list.hash && areSameArgumentMoves(other.moves, list.moves)) {
                listKeptFor[index] = indexPlusOne - 1;
                return;
            }
        }
    });
}

namespace {

struct ArgumentMovesWithFirstWords {
    std::array<uint64_t, 4> firstWords;
    uint32_t list;
    uint32_t unused;
};
static_assert(!VectorTraits<ArgumentMovesWithFirstWords>::needsInitialization, "the loops that fill a list of them are the first to touch its memory");

} // anonymous namespace

static Vector<KeptArgumentMoves> sortArgumentMovesByWords(const ParallelLoops& loops, const Vector<HashedArgumentMoves>& lists, const Vector<uint32_t>& kept, Vector<uint32_t>& placeOfKept, Vector<StubKey>& stubKeys)
{
    Vector<ArgumentMovesWithFirstWords> withFirstWords;
    withFirstWords.grow(kept.size());
    forEachRangeOfIndicesInParallel(loops, kept.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            std::span<const uint64_t> words = lists[kept[index]].moves.span();
            ArgumentMovesWithFirstWords& entry = withFirstWords[index];
            entry.list = kept[index];
            entry.unused = 0;
            for (size_t word = 0; word < entry.firstWords.size(); ++word)
                entry.firstWords[word] = word < words.size() ? words[word] : 0;
        }
    });
    parallelSort(loops, withFirstWords.mutableSpan(), [&](const ArgumentMovesWithFirstWords& a, const ArgumentMovesWithFirstWords& b) {
        for (size_t word = 0; word < a.firstWords.size(); ++word) {
            if (a.firstWords[word] != b.firstWords[word])
                return a.firstWords[word] < b.firstWords[word];
        }
        return comesFirstAmongArgumentMoves(lists[a.list].moves, lists[b.list].moves);
    });

    Vector<KeptArgumentMoves> sorted;
    sorted.grow(withFirstWords.size());
    placeOfKept.grow(lists.size());
    forEachRangeOfIndicesInParallel(loops, withFirstWords.size(), [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place) {
            const ArgumentMovesWithFirstWords& entry = withFirstWords[place];
            sorted[place] = lists[entry.list].moves;
            placeOfKept[entry.list] = static_cast<uint32_t>(place);
            RELEASE_ASSERT(!place || entry.firstWords != withFirstWords[place - 1].firstWords || !areSameArgumentMoves(lists[withFirstWords[place - 1].list].moves, sorted[place]));
        }
    });
    for (const ArgumentMovesWithFirstWords& entry : withFirstWords) {
        StubKey key = StubKey::fromBits(entry.firstWords[0]);
        if (stubKeys.isEmpty() || stubKeys.last() != key)
            stubKeys.append(key);
    }
    return sorted;
}

static String describeStubForSharedCode(StubKey key)
{
    if (!key.hasFamily())
        return makeString("of no family, with the bits "_s, key.bits());
    String immediate = key.immediate() ? makeString(", immediate "_s, *key.immediate()) : emptyString();
    return makeString(descriptorOf(key.family()).name, ", form "_s, static_cast<unsigned>(key.form()), ", count "_s, key.count(), ", caller frame "_s, static_cast<unsigned>(key.callerFrame()), immediate);
}

namespace {

template<typename T, typename Traits>
class DistinctElements {
public:
    uint32_t& slotOf(const T& element)
    {
        if (m_elements.size() * 2 >= m_slots.size())
            grow();
        size_t mask = m_slots.size() - 1;
        for (size_t slot = Traits::hash(element) >> m_shift;; slot = (slot + 1) & mask) {
            uint32_t& indexPlusOne = m_slots[slot];
            if (!indexPlusOne || Traits::equal(m_elements[indexPlusOne - 1], element))
                return indexPlusOne;
        }
    }

    void addAt(uint32_t& emptySlot, const T& element)
    {
        m_elements.append(element);
        emptySlot = static_cast<uint32_t>(m_elements.size());
    }

    void add(const T& element)
    {
        uint32_t& indexPlusOne = slotOf(element);
        if (!indexPlusOne)
            addAt(indexPlusOne, element);
    }

    Vector<T> take() { return WTF::move(m_elements); }

private:
    void grow()
    {
        size_t numberOfSlots = std::max<size_t>(256, m_slots.size() * 4);
        m_shift = 64 - std::countr_zero(numberOfSlots);
        m_slots.fill(0, numberOfSlots);
        for (size_t index = 0; index < m_elements.size(); ++index) {
            size_t slot = Traits::hash(m_elements[index]) >> m_shift;
            while (m_slots[slot])
                slot = (slot + 1) & (numberOfSlots - 1);
            m_slots[slot] = static_cast<uint32_t>(index) + 1;
        }
    }

    Vector<T> m_elements;
    Vector<uint32_t> m_slots;
    unsigned m_shift { 64 };
};

struct StubKeyTraits {
    static uint64_t hash(StubKey key) { return key.bits() * 0x9e3779b97f4a7c15ULL; }
    static bool equal(StubKey a, StubKey b) { return a == b; }
};

struct HashedArgumentMovesTraits {
    static uint64_t hash(const HashedArgumentMoves& hashed) { return hashed.hash; }
    static bool equal(const HashedArgumentMoves& a, const HashedArgumentMoves& b) { return a.hash == b.hash && areSameArgumentMoves(a.moves, b.moves); }
};

using DistinctStubKeys = DistinctElements<StubKey, StubKeyTraits>;
using DistinctArgumentMoves = DistinctElements<HashedArgumentMoves, HashedArgumentMovesTraits>;

struct SharedCodeCalledByChunk {
    DistinctStubKeys stubKeys;
    DistinctStubKeys stubKeysOfArgumentMoves;
    DistinctArgumentMoves argumentMoves;
};

} // anonymous namespace

[[nodiscard]] static bool addSharedCodeCalledBy(const CompiledCode& function, SharedCodeCalledByChunk& called, std::span<uint32_t> placesOfArgumentMoves, BuildContext& context)
{
    bool everyStubExists = true;
    auto existsOrIsReported = [&](StubKey key) {
        if (exists(key))
            return true;
        everyStubExists = false;
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::CallToStubNotGenerated,
            .function = function.unit.function,
            .detail = makeString("the "_s, nameOf(function.unit.role), " names the stub "_s, describeStubForSharedCode(key), ", and there is no such stub"_s),
        });
        return false;
    };
    for (StubKey key : function.body.stubKeys) {
        uint32_t& indexPlusOne = called.stubKeys.slotOf(key);
        if (!indexPlusOne && existsOrIsReported(key))
            called.stubKeys.addAt(indexPlusOne, key);
    }
    for (size_t list = 0; list < placesOfArgumentMoves.size(); ++list) {
        std::span<const uint64_t> moves = function.body.argumentMoves[list];
        RELEASE_ASSERT(moves.size() >= numberOfHeaderWords && !((moves.size() - numberOfHeaderWords) % 2));
        HashedArgumentMoves hashed { hashOfArgumentMoves(moves), moves };
        uint32_t& indexPlusOne = called.argumentMoves.slotOf(hashed);
        if (!indexPlusOne) {
            StubKey key = StubKey::fromBits(moves[0]);
            uint32_t& indexOfKeyPlusOne = called.stubKeysOfArgumentMoves.slotOf(key);
            if (!indexOfKeyPlusOne && existsOrIsReported(key))
                called.stubKeysOfArgumentMoves.addAt(indexOfKeyPlusOne, key);
            if (indexOfKeyPlusOne)
                called.argumentMoves.addAt(indexPlusOne, hashed);
        }
        placesOfArgumentMoves[list] = indexPlusOne - 1;
    }
    return everyStubExists;
}

std::optional<CalledSharedCode> collectCalledSharedCode(const LinkedFunctions& functions, BuildContext& context)
{
    StepTimes times(context, "image: shared code"_s);
    const ParallelLoops& loops = context.parallelLoops();
    const size_t numberOfChunks = numberOfRangesOfIndices(safeCast<unsigned>(functions.size()));
    Vector<Vector<StubKey>> stubKeysOfChunks(numberOfChunks);
    Vector<Vector<HashedArgumentMoves>> argumentMovesOfChunks(numberOfChunks);
    CalledSharedCode called;
    called.firstArgumentMovesOfBody.grow(functions.size() + 1);
    Vector<uint64_t> firstListOfChunk(FillWith { }, numberOfChunks + 1, 0);
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        uint64_t numberOfListsInChunk = 0;
        for (size_t position = first; position < end; ++position)
            numberOfListsInChunk += functions[position].body.argumentMoves.size();
        firstListOfChunk[first / indicesInRangeOfLoop + 1] = numberOfListsInChunk;
    });
    for (size_t chunk = 0; chunk < numberOfChunks; ++chunk)
        firstListOfChunk[chunk + 1] += firstListOfChunk[chunk];
    uint64_t numberOfLists = firstListOfChunk.last();
    called.firstArgumentMovesOfBody[functions.size()] = safeCast<uint32_t>(numberOfLists);
    called.thunksOfArgumentMoves.grow(numberOfLists);
    std::span<const uint32_t> firstListOfBody = called.firstArgumentMovesOfBody.span();
    std::span<uint32_t> thunksOfLists = called.thunksOfArgumentMoves.mutableSpan();
    std::atomic<bool> everyStubExists { true };
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        SharedCodeCalledByChunk calledByChunk;
        auto firstList = static_cast<uint32_t>(firstListOfChunk[first / indicesInRangeOfLoop]);
        for (size_t position = first; position < end; ++position) {
            called.firstArgumentMovesOfBody[position] = firstList;
            std::span<uint32_t> placesOfArgumentMoves = thunksOfLists.subspan(firstList, functions[position].body.argumentMoves.size());
            firstList += static_cast<uint32_t>(placesOfArgumentMoves.size());
            if (!addSharedCodeCalledBy(functions[position], calledByChunk, placesOfArgumentMoves, context))
                everyStubExists.store(false, std::memory_order_relaxed);
        }
        stubKeysOfChunks[first / indicesInRangeOfLoop] = calledByChunk.stubKeys.take();
        argumentMovesOfChunks[first / indicesInRangeOfLoop] = calledByChunk.argumentMoves.take();
    });
    times.didFinish("collect by chunk"_s);
    if (!everyStubExists.load(std::memory_order_relaxed))
        return std::nullopt;

    Vector<uint32_t> firstListKeptByChunk(numberOfChunks);
    size_t numberOfListsKeptByChunks = 0;
    for (size_t chunk = 0; chunk < numberOfChunks; ++chunk) {
        firstListKeptByChunk[chunk] = static_cast<uint32_t>(numberOfListsKeptByChunks);
        numberOfListsKeptByChunks += argumentMovesOfChunks[chunk].size();
    }
    Vector<HashedArgumentMoves> hashedMoves;
    hashedMoves.grow(numberOfListsKeptByChunks);
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfChunks), [&](unsigned chunk) {
        std::ranges::copy(argumentMovesOfChunks[chunk], hashedMoves.mutableSpan().subspan(firstListKeptByChunk[chunk]).begin());
        argumentMovesOfChunks[chunk] = { };
    });
    Vector<uint32_t> listKeptFor;
    Vector<uint32_t> kept = removeDuplicateArgumentMoves(loops, hashedMoves, listKeptFor);
    times.didFinish("remove duplicate argument moves"_s);

    called.stubKeys = joinChunks<StubKey>(loops, WTF::move(stubKeysOfChunks));
    Vector<uint32_t> placeOfKept;
    called.argumentMoves = sortArgumentMovesByWords(loops, hashedMoves, kept, placeOfKept, called.stubKeysOfArgumentMoves);
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        uint32_t firstListKept = firstListKeptByChunk[first / indicesInRangeOfLoop];
        for (uint32_t& thunk : thunksOfLists.subspan(firstListOfBody[first], firstListOfBody[end] - firstListOfBody[first]))
            thunk = placeOfKept[listKeptFor[firstListKept + thunk]];
    });
    times.didFinish("sort argument moves"_s);
    return called;
}

static bool checksSharedCodeWhileBuilding()
{
    return ASSERT_ENABLED || ASAN_ENABLED;
}

static bool isSameStubSummary(const StubSummary& a, const StubSummary& b)
{
    return a.signature.operands == b.signature.operands
        && a.signature.numberOfOperands == b.signature.numberOfOperands
        && a.signature.usesCallConvention == b.signature.usesCallConvention
        && a.signature.result == b.signature.result
        && a.signature.firstResult == b.signature.firstResult
        && a.signature.secondResult == b.signature.secondResult
        && a.signature.immediate == b.signature.immediate
        && a.signature.trueCondition == b.signature.trueCondition
        && a.clobbered == b.clobbered
        && a.clobberedInline == b.clobberedInline;
}

static void checkStubSummariesMatchCompile(const StubCatalog& catalogOfImage, std::span<const StubKey> calledKeys)
{
    StubOptions optionsOfCompile = catalogOfImage.options();
    optionsOfCompile.maxNameInFieldTable = RowDisplacementTable::maxColumn;
    if (optionsOfCompile.maxNameInFieldTable == catalogOfImage.options().maxNameInFieldTable)
        return;
    const StubCatalog catalogOfCompile(optionsOfCompile);
    for (StubKey key : calledKeys) {
        bool matchesCompile = isSameStubSummary(catalogOfCompile.summaryOf(key), catalogOfImage.summaryOf(key))
            && catalogOfCompile.effectsOf(key) == catalogOfImage.effectsOf(key)
            && catalogOfCompile.isLeaf(key) == catalogOfImage.isLeaf(key);
        if (matchesCompile)
            continue;
        dataLogLn("AOT: the registers or the effects of the stub "_s, describeStubForSharedCode(key), " depend on StubOptions::maxNameInFieldTable, so its callers were compiled for a different stub than the image has"_s);
        CRASH();
    }
}

uint32_t SharedCode::thunkOffset(std::span<const uint64_t> moves) const
{
    auto found = std::ranges::lower_bound(argumentMoves, KeptArgumentMoves(moves), comesFirstAmongArgumentMoves);
    RELEASE_ASSERT(found != argumentMoves.end() && areSameArgumentMoves(*found, moves));
    return argumentThunks.offsets[found - argumentMoves.begin()];
}

SharedCode generateSharedCode(CalledSharedCode&& called, const StubCatalog& catalog, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext& context)
{
    StepTimes times(context, "image: shared code"_s);
    SharedCode result;
    result.argumentMoves = WTF::move(called.argumentMoves);
    result.firstArgumentMovesOfBody = WTF::move(called.firstArgumentMovesOfBody);
    result.thunksOfArgumentMoves = WTF::move(called.thunksOfArgumentMoves);

    DistinctStubKeys distinctKeys;
    for (StubKey key : called.stubKeys)
        distinctKeys.add(key);
    for (StubKey key : called.stubKeysOfArgumentMoves)
        distinctKeys.add(key);
    Vector<StubKey> calledKeys = distinctKeys.take();
    sortAndRemoveDuplicates(context.parallelLoops(), calledKeys);
    if (checksSharedCodeWhileBuilding())
        checkStubSummariesMatchCompile(catalog, calledKeys.span());

    std::expected<ArgumentThunks, LinkFailure> argumentThunks;
    std::expected<StubBlob, LinkFailure> blob;
    runLanes(context.parallelLoops(), {
        [&] {
            argumentThunks = generateArgumentThunks(context, catalog, result.argumentMoves);
            times.didFinish("generate argument thunks"_s);
        },
        [&] {
            StepTimes timesOfLane(context, "image: shared code"_s);
            blob = buildStubBlob(context, catalog, calledKeys.span(), entriesUsedByFunctions);
            timesOfLane.didFinish("generate stubs"_s);
        },
    });
    if (!argumentThunks) {
        reportLinkFailure(argumentThunks.error(), { }, context);
        return { };
    }
    result.argumentThunks = WTF::move(*argumentThunks);
    RELEASE_ASSERT(result.argumentThunks.offsets.size() == result.argumentMoves.size());
    RELEASE_ASSERT(result.argumentThunks.stubKeys == called.stubKeysOfArgumentMoves);
    if (!blob) {
        reportLinkFailure(blob.error(), { }, context);
        return { };
    }
    result.blob = WTF::move(*blob);

    uint64_t blobSize = result.blob.code.bytes.size();
    uint64_t argumentThunksSize = result.argumentThunks.code.bytes.size();
    uint64_t argumentThunksOffset = WTF::roundUpToMultipleOf<imageStubsAlignment>(blobSize);
    uint64_t veneersOffset = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(argumentThunksSize ? argumentThunksOffset + argumentThunksSize : blobSize);
    if (veneersOffset > std::min<uint64_t>(context.limit(LimitID::CodeBytes), std::numeric_limits<uint32_t>::max())) {
        context.reportLimitExceeded(LimitID::CodeBytes, veneersOffset);
        return { };
    }
    result.argumentThunksOffset = static_cast<uint32_t>(argumentThunksOffset);
    result.veneersOffset = static_cast<uint32_t>(veneersOffset);
    context.statistics().count(StatisticID::StubBytes, blobSize);
    context.statistics().count(StatisticID::ArgumentThunkBytes, argumentThunksSize);
    return result;
}

SharedCode selectSharedCode(const LinkedFunctions& functions, const StubCatalog& catalog, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext& context)
{
    std::optional<CalledSharedCode> called = collectCalledSharedCode(functions, context);
    if (!called)
        return { };
    return generateSharedCode(WTF::move(*called), catalog, entriesUsedByFunctions, context);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
