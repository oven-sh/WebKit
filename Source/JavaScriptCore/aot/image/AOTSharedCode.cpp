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
    const ArgumentMoves* moves;
};

} // anonymous namespace

static bool comesFirstAmongArgumentMoves(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return std::ranges::lexicographical_compare(*a, *b);
}

static bool areSameArgumentMoves(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return *a == *b;
}

static uint64_t hashOfArgumentMoves(const ArgumentMoves& moves)
{
    uint64_t hash = moves.size();
    for (uint64_t word : moves)
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
    return hash;
}

// Which of several equal lists stays depends on the threads. Nothing that reaches the image does: the caller sorts what stays by its words.
static Vector<const ArgumentMoves*> removeDuplicateArgumentMoves(const ParallelLoops& loops, const Vector<HashedArgumentMoves>& lists)
{
    size_t numberOfSlots = std::bit_ceil(lists.size() * 2 + 1);
    unsigned shift = 64 - std::countr_zero(numberOfSlots);
    // index + 1, linear probing.
    Vector<uint32_t> slots;
    slots.grow(numberOfSlots);
    forEachRangeOfIndicesInParallel(loops, numberOfSlots, [&](size_t first, size_t end) {
        zeroSpan(slots.mutableSpan().subspan(first, end - first));
    });
    return collectInParallel<const ArgumentMoves*>(loops, static_cast<unsigned>(lists.size()), [&](unsigned index, Vector<const ArgumentMoves*>& distinct) {
        const HashedArgumentMoves& list = lists[index];
        for (size_t slot = list.hash >> shift;; slot = (slot + 1) & (numberOfSlots - 1)) {
            AtomicRef occupant(slots[slot]);
            uint32_t indexPlusOne = occupant.load(std::memory_order_relaxed);
            if (!indexPlusOne && occupant.compare_exchange_strong(indexPlusOne, index + 1, std::memory_order_relaxed)) {
                distinct.append(list.moves);
                return;
            }
            const HashedArgumentMoves& other = lists[indexPlusOne - 1];
            if (other.hash == list.hash && *other.moves == *list.moves)
                return;
        }
    });
}

namespace {

// A list that is shorter has zeros behind it, which order it in front of every list that it begins.
struct ArgumentMovesWithFirstWords {
    std::array<uint64_t, 4> firstWords;
    const ArgumentMoves* moves;
};

} // anonymous namespace

static void sortArgumentMovesByWords(const ParallelLoops& loops, Vector<const ArgumentMoves*>& lists)
{
    Vector<ArgumentMovesWithFirstWords> withFirstWords;
    withFirstWords.grow(lists.size());
    forEachRangeOfIndicesInParallel(loops, lists.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            std::span<const uint64_t> words = lists[index]->span();
            ArgumentMovesWithFirstWords& entry = withFirstWords[index];
            entry.moves = lists[index];
            for (size_t word = 0; word < entry.firstWords.size(); ++word)
                entry.firstWords[word] = word < words.size() ? words[word] : 0;
        }
    });
    parallelSort(loops, withFirstWords.mutableSpan(), [](const ArgumentMovesWithFirstWords& a, const ArgumentMovesWithFirstWords& b) {
        if (a.firstWords != b.firstWords)
            return a.firstWords < b.firstWords;
        return comesFirstAmongArgumentMoves(a.moves, b.moves);
    });
    forEachRangeOfIndicesInParallel(loops, lists.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            lists[index] = withFirstWords[index].moves;
    });
}

static String describeStubForSharedCode(StubKey key)
{
    if (!key.hasFamily())
        return makeString("of no family, with the bits "_s, key.bits());
    String immediate = key.immediate() ? makeString(", immediate "_s, *key.immediate()) : emptyString();
    return makeString(descriptorOf(key.family()).name, ", form "_s, static_cast<unsigned>(key.form()), ", count "_s, key.count(), ", caller frame "_s, static_cast<unsigned>(key.callerFrame()), immediate);
}

namespace {

// Keeps the first of equal elements, in the order in which they were added.
template<typename T, typename Traits>
class DistinctElements {
public:
    void add(const T& element)
    {
        if (m_elements.size() * 2 >= m_slots.size())
            grow();
        size_t mask = m_slots.size() - 1;
        for (size_t slot = Traits::hash(element) >> m_shift;; slot = (slot + 1) & mask) {
            uint32_t indexPlusOne = m_slots[slot];
            if (!indexPlusOne) {
                m_elements.append(element);
                m_slots[slot] = static_cast<uint32_t>(m_elements.size());
                return;
            }
            if (Traits::equal(m_elements[indexPlusOne - 1], element))
                return;
        }
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
    static bool equal(const HashedArgumentMoves& a, const HashedArgumentMoves& b) { return a.hash == b.hash && *a.moves == *b.moves; }
};

using DistinctStubKeys = DistinctElements<StubKey, StubKeyTraits>;
using DistinctArgumentMoves = DistinctElements<HashedArgumentMoves, HashedArgumentMovesTraits>;

} // anonymous namespace

// False where the function names a stub that does not exist.
[[nodiscard]] static bool addSharedCodeCalledBy(const CompiledCode& function, DistinctStubKeys& stubKeys, DistinctArgumentMoves& argumentMoves, BuildContext& context)
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
        if (existsOrIsReported(key))
            stubKeys.add(key);
    }
    for (const ArgumentMoves& moves : function.body.argumentMoves) {
        RELEASE_ASSERT(moves.size() >= numberOfHeaderWords && !((moves.size() - numberOfHeaderWords) % 2));
        if (existsOrIsReported(StubKey::fromBits(moves[0])))
            argumentMoves.add({ hashOfArgumentMoves(moves), &moves });
    }
    return everyStubExists;
}

// Runs after applyNumbersToFunctions(), so a key whose immediate is a number given by Image already has it, and so has a throw word that a thunk moves.
// StubCalls adds a key or a list of moves only along with a relocation that names it, so what a body lists is what it calls.
std::optional<CalledSharedCode> collectCalledSharedCode(const LinkedFunctions& functions, BuildContext& context)
{
    StepTimes times(context, "image: shared code"_s);
    const ParallelLoops& loops = context.parallelLoops();
    const ChunksOfIndices chunks = divideIntoChunks(safeCast<unsigned>(functions.size()));
    Vector<Vector<StubKey>> stubKeysOfChunks(chunks.size());
    Vector<Vector<HashedArgumentMoves>> argumentMovesOfChunks(chunks.size());
    std::atomic<bool> everyStubExists { true };
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        DistinctStubKeys stubKeys;
        DistinctArgumentMoves argumentMoves;
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position) {
            if (!addSharedCodeCalledBy(functions[position], stubKeys, argumentMoves, context))
                everyStubExists.store(false, std::memory_order_relaxed);
        }
        stubKeysOfChunks[chunk] = stubKeys.take();
        argumentMovesOfChunks[chunk] = argumentMoves.take();
    });
    times.didFinish("collect by chunk"_s);
    if (!everyStubExists.load(std::memory_order_relaxed))
        return std::nullopt;

    Vector<HashedArgumentMoves> hashedMoves = joinChunks<HashedArgumentMoves>(loops, WTF::move(argumentMovesOfChunks));
    CalledSharedCode called;
    called.argumentMoves = removeDuplicateArgumentMoves(loops, hashedMoves);
    times.didFinish("remove duplicate argument moves"_s);

    called.stubKeys = joinChunks<StubKey>(loops, WTF::move(stubKeysOfChunks));
    sortArgumentMovesByWords(loops, called.argumentMoves);
    removeRepeatedElements(called.argumentMoves, areSameArgumentMoves);
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

// The functions were compiled against a catalog that did not know the last column of the field table. What it said about a stub has to hold for the stub in the image.
// StubOptions::from() takes the last column of the front tables for maxNameInFieldTable, and the options of the two catalogs agree in everything else.
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

void SharedCode::buildArgumentMovesHashIndex()
{
    size_t numberOfSlots = std::bit_ceil(argumentMoves.size() * 2 + 1);
    unsigned shift = 64 - std::countr_zero(numberOfSlots);
    argumentMovesHashSlots.fill(0, numberOfSlots);
    for (size_t index = 0; index < argumentMoves.size(); ++index) {
        size_t slot = hashOfArgumentMoves(*argumentMoves[index]) >> shift;
        while (argumentMovesHashSlots[slot])
            slot = (slot + 1) & (numberOfSlots - 1);
        argumentMovesHashSlots[slot] = static_cast<uint32_t>(index) + 1;
    }
}

uint32_t SharedCode::thunkOffset(const ArgumentMoves& moves) const
{
    if (!argumentMovesHashSlots.isEmpty()) {
        size_t mask = argumentMovesHashSlots.size() - 1;
        unsigned shift = 64 - std::countr_zero(argumentMovesHashSlots.size());
        for (size_t slot = hashOfArgumentMoves(moves) >> shift;; slot = (slot + 1) & mask) {
            uint32_t indexPlusOne = argumentMovesHashSlots[slot];
            RELEASE_ASSERT(indexPlusOne);
            if (*argumentMoves[indexPlusOne - 1] == moves)
                return argumentThunks.offsets[indexPlusOne - 1];
        }
    }

    auto found = std::ranges::lower_bound(argumentMoves, &moves, comesFirstAmongArgumentMoves);
    RELEASE_ASSERT(found != argumentMoves.end() && **found == moves);
    return argumentThunks.offsets[found - argumentMoves.begin()];
}

SharedCode generateSharedCode(CalledSharedCode&& called, const StubCatalog& catalog, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext& context)
{
    StepTimes times(context, "image: shared code"_s);
    SharedCode result;
    result.argumentMoves = WTF::move(called.argumentMoves);
    result.buildArgumentMovesHashIndex();
    std::expected<ArgumentThunks, LinkFailure> argumentThunks = generateArgumentThunks(context, catalog, result.argumentMoves);
    times.didFinish("generate argument thunks"_s);
    if (!argumentThunks) {
        reportLinkFailure(argumentThunks.error(), { }, context);
        return { };
    }
    result.argumentThunks = WTF::move(*argumentThunks);
    RELEASE_ASSERT(result.argumentThunks.offsets.size() == result.argumentMoves.size());

    Vector<StubKey> calledKeys = WTF::move(called.stubKeys);
    calledKeys.appendVector(result.argumentThunks.stubKeys);
    sortAndRemoveDuplicates(context.parallelLoops(), calledKeys);
    if (checksSharedCodeWhileBuilding())
        checkStubSummariesMatchCompile(catalog, calledKeys.span());

    std::expected<StubBlob, LinkFailure> blob = buildStubBlob(context, catalog, calledKeys.span(), entriesUsedByFunctions);
    times.didFinish("generate stubs"_s);
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
