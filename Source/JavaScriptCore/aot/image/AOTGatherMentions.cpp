/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTCompiledCode.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include "AOTTypeIDs.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr std::array gatheredSpacesThatCountFunctions { NumberSpace::Name, NumberSpace::NameOfText };
static_assert(std::ranges::all_of(gatheredSpacesThatCountFunctions, hasOneIDAsKey));

static constexpr std::array gatheredSpacesThatKeepFirstFunction { NumberSpace::Name, NumberSpace::NameOfText, NumberSpace::String, NumberSpace::StringCell, NumberSpace::KeyCell };
static_assert(std::ranges::all_of(gatheredSpacesThatKeepFirstFunction, hasOneIDAsKey));
static_assert(std::ranges::all_of(gatheredSpacesThatCountFunctions, [](NumberSpace space) { return std::ranges::contains(gatheredSpacesThatKeepFirstFunction, space); }), "functions are counted unit by unit");
static_assert(FunctionByModule { .position = UINT32_MAX, .function = FunctionID(0u) } < FunctionByModule { }, "a minimum over functions starts at no function");
static_assert(FunctionByModule { .module = ModuleID(0u), .position = 1, .function = FunctionID(1u) } < FunctionByModule { .module = ModuleID(1u), .position = 0, .function = FunctionID(0u) });
static_assert(FunctionByModule { .module = ModuleID(0u), .position = 0, .function = FunctionID(1u) } < FunctionByModule { .module = ModuleID(0u), .position = 1, .function = FunctionID(0u) });

static constexpr uint32_t largestGatheredID = Typed::StringID::largestIndex;

FunctionByModule FunctionByModule::forFunction(const Typed::Program& program, FunctionID id)
{
    const Typed::Function& function = program.at(id);
    return { function.module, function.position, id };
}

static void removeRepeatedGatheredIDs(Vector<uint32_t>& ids, size_t first)
{
    std::span<uint32_t> added = ids.mutableSpan().subspan(first);
    if (added.size() < 2)
        return;
    AOT::sortInLine(added);
    auto repeats = std::ranges::unique(added);
    ids.shrink(ids.size() - repeats.size());
}

namespace {

struct GatheredIDsOfUnit {
    FunctionByModule function;
    size_t end;
    bool codeRunsAtMostOnce;
};

struct GatheredChunk {
    Vector<uint32_t>& wordsOf(NumberSpace space) { return words[static_cast<unsigned>(space)]; }
    const Vector<uint32_t>& wordsOf(NumberSpace space) const { return words[static_cast<unsigned>(space)]; }
    Vector<GatheredIDsOfUnit>& idsOfUnitsOf(NumberSpace space) { return idsOfUnits[static_cast<unsigned>(space)]; }
    const Vector<GatheredIDsOfUnit>& idsOfUnitsOf(NumberSpace space) const { return idsOfUnits[static_cast<unsigned>(space)]; }

    std::array<Vector<uint32_t>, numberOfNumberSpaces> words;
    std::array<Vector<GatheredIDsOfUnit>, numberOfNumberSpaces> idsOfUnits;
    std::array<uint32_t, numberOfNumberSpaces> endOfIDs { };
    std::array<uint32_t, numberOfNumberSpaces> numberOfKeys { };
    uint64_t numberOfMentions { 0 };
};

struct DistinctKeysOfRange {
    size_t numberOfKeys { 0 };
    size_t numberOfWords { 0 };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

struct GatheredKey {
    std::span<const uint32_t> words() const { return unsafeMakeSpan(sizeAndWords + 1, *sizeAndWords); }

    uint64_t firstWords;
    const uint32_t* sizeAndWords;
};
static_assert(sizeof(GatheredKey) == sizeof(std::span<const uint32_t>));

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

class MentionGatherer {
    WTF_MAKE_NONCOPYABLE(MentionGatherer);
public:
    MentionGatherer(const Typed::Program& program, const Typed::Facts& facts, BuildContext& context, GatheredChunk& chunk)
        : m_program(program)
        , m_facts(facts)
        , m_context(context)
        , m_chunk(chunk)
    {
    }

    void gather(const CompiledCode& code)
    {
        std::array<size_t, gatheredSpacesThatCountFunctions.size()> firstIDOfFunction { };
        for (size_t index = 0; index < firstIDOfFunction.size(); ++index)
            firstIDOfFunction[index] = m_chunk.wordsOf(gatheredSpacesThatCountFunctions[index]).size();

        m_code = &code;
        std::span<const Mention> mentions = code.body.mentions.span();
        std::span<const uint32_t> mentionWords = code.body.mentionWords.span();
        for (m_indexOfMention = 0; m_indexOfMention < mentions.size(); ++m_indexOfMention) {
            const Mention& mention = mentions[m_indexOfMention];
            add(mention.space, keyOf(mention, mentionWords));
        }
        m_chunk.numberOfMentions += mentions.size();

        for (size_t index = 0; index < firstIDOfFunction.size(); ++index)
            removeRepeatedGatheredIDs(m_chunk.wordsOf(gatheredSpacesThatCountFunctions[index]), firstIDOfFunction[index]);

        for (NumberSpace space : gatheredSpacesThatKeepFirstFunction) {
            Vector<GatheredIDsOfUnit>& idsOfUnits = m_chunk.idsOfUnitsOf(space);
            size_t end = m_chunk.wordsOf(space).size();
            if (end == (idsOfUnits.isEmpty() ? 0 : idsOfUnits.last().end))
                continue;
            idsOfUnits.append(GatheredIDsOfUnit { .function = FunctionByModule::forFunction(m_program, code.unit.function), .end = end, .codeRunsAtMostOnce = runsAtMostOnce(code) });
        }
    }

private:
    bool runsAtMostOnce(const CompiledCode& code) const
    {
        FunctionID function = code.unit.function;
        return m_program.at(function).isModuleBody() && !m_facts.hasLoops(function) && code.sites.rareLists->pollSites.isEmpty();
    }

    static constexpr bool impliesOtherMentions(NumberSpace space)
    {
        switch (space) {
        case NumberSpace::CardOfVariable:
        case NumberSpace::CardOfWord:
        case NumberSpace::Request:
        case NumberSpace::ClassGuard:
        case NumberSpace::SiteCache:
            return true;
        default:
            return false;
        }
    }

    ALWAYS_INLINE void add(NumberSpace space, std::span<const uint32_t> key)
    {
        Vector<uint32_t>& words = m_chunk.wordsOf(space);
        if (hasOneIDAsKey(space)) {
            if (key.size() != 1 || key[0] > largestGatheredID) {
                reportMalformedMention("the key of a space that goes by ids is not one id"_s);
                return;
            }
            if (Typed::liesInThread(StringID::fromBits(key[0]))) {
                reportMalformedMention("the key is an id of a thread: internTextsMadeByFunctions() comes first"_s);
                return;
            }
            uint32_t& endOfIDs = m_chunk.endOfIDs[static_cast<unsigned>(space)];
            endOfIDs = std::max(endOfIDs, key[0] + 1);
            words.append(key[0]);
        } else {
            words.append(static_cast<uint32_t>(key.size()));
            ++m_chunk.numberOfKeys[static_cast<unsigned>(space)];
            words.append(key);
        }
        if (impliesOtherMentions(space))
            addImpliedMentions(space, key);
    }

    NEVER_INLINE void addImpliedMentions(NumberSpace space, std::span<const uint32_t> key)
    {
        switch (space) {
        case NumberSpace::CardOfVariable:
            add(NumberSpace::Variable, key);
            return;
        case NumberSpace::CardOfWord:
            if (key.size() < 2 || key[0] >= numberOfNumberSpaces) {
                reportMalformedMention("the card of a word names no word"_s);
                return;
            }
            add(static_cast<NumberSpace>(key[0]), key.subspan(1));
            return;
        case NumberSpace::Request:
            addMentionsImpliedByRequest(key);
            return;
        case NumberSpace::ClassGuard:
            if (isRowOfClassGuardCountsKey(key)) {
                bool everySlotListsGuards = true;
                bool hasSlots = forEachSlotInRowOfClassGuardCountsKey(key, [&](uint32_t, std::span<const uint32_t> slot) {
                    everySlotListsGuards &= forEachGuardInSlotOfRowOfClassGuardCounts(slot, [&](std::span<const uint32_t> guard) {
                        add(NumberSpace::ClassGuard, guard);
                    });
                });
                if (!hasSlots || !everySlotListsGuards)
                    reportMalformedMention("the row of counts of guards of classes is no row of lists of guards"_s);
                return;
            }
            if (isClassGuardCountKey(key)) {
                bool listsGuards = forEachGuardInClassGuardCountKey(key, [&](std::span<const uint32_t> guard) {
                    add(NumberSpace::ClassGuard, guard);
                });
                if (!listsGuards)
                    reportMalformedMention("the count of guards of classes does not list two guards or more"_s);
                return;
            }
            if (key.size() < 2 || key.size() % 2) {
                reportMalformedMention("the guard of a name of a class names no name"_s);
                return;
            }
            for (std::span<const uint32_t> names = key.subspan(2); !names.empty(); names = names.subspan(2)) {
                if (names[0] != static_cast<uint32_t>(NumberSpace::Name) && names[0] != static_cast<uint32_t>(NumberSpace::NameOfText)) {
                    reportMalformedMention("the guard of a name of a class names no name"_s);
                    return;
                }
                add(static_cast<NumberSpace>(names[0]), names.subspan(1, 1));
            }
            return;
        case NumberSpace::SiteCache:
            if (key.size() < 2) {
                reportMalformedMention("the site names no name"_s);
                return;
            }
            add(NumberSpace::Name, key.subspan(key.size() - 2, 1));
            return;
        case NumberSpace::ShownString:
        case NumberSpace::AtomOfString:
        case NumberSpace::Name:
        case NumberSpace::NameOfText:
        case NumberSpace::String:
        case NumberSpace::StringCell:
        case NumberSpace::KeyCell:
        case NumberSpace::BuiltinCell:
        case NumberSpace::BigInt:
        case NumberSpace::GlobalBinding:
        case NumberSpace::RequireEdge:
        case NumberSpace::Variable:
        case NumberSpace::ModuleValue:
        case NumberSpace::ModuleNamespace:
        case NumberSpace::ModuleBuiltinCell:
        case NumberSpace::DataStorage:
        case NumberSpace::DataElements:
        case NumberSpace::RegExpLiteralCell:
        case NumberSpace::TemplateObjectCell:
        case NumberSpace::ArrayProfile:
        case NumberSpace::ReadGroupCache:
        case NumberSpace::EnvironmentHandedBack:
        case NumberSpace::ClassRank:
        case NumberSpace::HeirLookedAt:
        case NumberSpace::ClosureFunction:
        case NumberSpace::ClosureCell:
        case NumberSpace::Allocation:
        case NumberSpace::PropertyRun:
        case NumberSpace::Type:
        case NumberSpace::MethodSelector:
        case NumberSpace::RegExp:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void addMentionsImpliedByRequest(std::span<const uint32_t> request)
    {
        if (request.empty()) {
            reportMalformedMention("the request has no kind"_s);
            return;
        }
        switch (static_cast<RequestKind>(request[0])) {
        case RequestKind::NewClass:
        case RequestKind::TemplateObject:
        case RequestKind::CollectionData:
        case RequestKind::FormatPattern:
            return;
        case RequestKind::DirectEval:
            addMentionsImpliedByDirectEvalRequest(request.subspan(1));
            return;
        case RequestKind::Data:
            addMentionsImpliedByDataRequest(request.subspan(1));
            return;
        }
        reportMalformedMention("the request has no kind"_s);
    }

    void addMentionsImpliedByDirectEvalRequest(std::span<const uint32_t> scopeAndModule)
    {
        if (scopeAndModule.size() != 2 || scopeAndModule[0] >= m_program.evalScopes.size()) {
            reportMalformedMention("the request for a direct eval does not name a scope of the program and a module"_s);
            return;
        }
        if (Typed::EvalScopeID outer = m_program.evalScopes[scopeAndModule[0]].outer) {
            uint32_t request[] { static_cast<uint32_t>(RequestKind::DirectEval), outer.index(), scopeAndModule[1] };
            add(NumberSpace::Request, request);
        }
    }

    void addMentionsImpliedByDataRequest(std::span<const uint32_t> dataAndEncoding)
    {
        if (dataAndEncoding.size() != 2 || dataAndEncoding[0] >= m_program.data.size() || dataAndEncoding[1] >= numberOfEnumerators<Typed::DataEncoding>) {
            reportMalformedMention("the request for data does not name data of the program and an encoding"_s);
            return;
        }
        std::span<const uint32_t> data = dataAndEncoding.first(1);
        switch (static_cast<Typed::DataEncoding>(dataAndEncoding[1])) {
        case Typed::DataEncoding::ReadOnlyTree:
        case Typed::DataEncoding::TreeWithOwnStorage:
            return;
        case Typed::DataEncoding::CopyOnWriteArray: {
            const auto* array = m_program.at(DataID(data[0])).root.tryAs<Typed::DataNode::Array>();
            if (!array)
                break;
            if (!Typed::isStorageOfArray(*array))
                add(NumberSpace::DataStorage, data);
            return;
        }
        case Typed::DataEncoding::Unselected:
            break;
        }
        reportMalformedMention("the request for data names an encoding in which the data has no record"_s);
    }

    NEVER_INLINE void reportMalformedMention(ASCIILiteral rule)
    {
        m_context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .function = m_code->unit.function,
            .count = m_indexOfMention,
            .detail = makeString("mention "_s, m_indexOfMention, " of the "_s, nameOf(m_code->unit.role), ": "_s, rule),
        });
    }

    const Typed::Program& m_program;
    const Typed::Facts& m_facts;
    BuildContext& m_context;
    GatheredChunk& m_chunk;
    const CompiledCode* m_code { nullptr };
    uint32_t m_indexOfMention { 0 };
};

} // anonymous namespace

static GatheredMentions::ByID mergeGatheredIDs(NumberSpace space, std::span<const GatheredChunk> chunks)
{
    uint32_t endOfIDs = 0;
    for (const GatheredChunk& chunk : chunks)
        endOfIDs = std::max(endOfIDs, chunk.endOfIDs[static_cast<unsigned>(space)]);

    GatheredMentions::ByID merged;
    merged.isMentioned.ensureSize(endOfIDs);
    if (!std::ranges::contains(gatheredSpacesThatKeepFirstFunction, space)) {
        for (const GatheredChunk& chunk : chunks) {
            for (uint32_t id : chunk.wordsOf(space))
                merged.isMentioned.quickSet(id);
        }
        return merged;
    }

    bool countsFunctions = std::ranges::contains(gatheredSpacesThatCountFunctions, space);
    if (countsFunctions)
        merged.numberOfFunctions.fill(0, endOfIDs);
    merged.firstFunction.fill(FunctionByModule { }, endOfIDs);
    for (const GatheredChunk& chunk : chunks) {
        std::span<const uint32_t> ids = chunk.wordsOf(space).span();
        size_t first = 0;
        for (const GatheredIDsOfUnit& idsOfUnit : chunk.idsOfUnitsOf(space)) {
            bool isCounted = countsFunctions && !idsOfUnit.codeRunsAtMostOnce;
            for (uint32_t id : ids.subspan(first, idsOfUnit.end - first)) {
                merged.isMentioned.quickSet(id);
                if (isCounted)
                    ++merged.numberOfFunctions[id];
                merged.firstFunction[id] = std::min(merged.firstFunction[id], idsOfUnit.function);
            }
            first = idsOfUnit.end;
        }
        RELEASE_ASSERT(first == ids.size());
    }
    return merged;
}

static uint64_t firstWordsOfGatheredKey(std::span<const uint32_t> key)
{
    uint64_t first = key.empty() ? 0 : key[0];
    uint64_t second = key.size() < 2 ? 0 : key[1];
    return first << 32 | second;
}

static NumberedKeys mergeGatheredKeys(NumberSpace space, std::span<const GatheredChunk> chunks, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    NumberedKeys merged;
    Vector<size_t> firstKeyOfChunk(chunks.size());
    size_t numberOfKeys = 0;
    for (size_t chunk = 0; chunk < chunks.size(); ++chunk) {
        firstKeyOfChunk[chunk] = numberOfKeys;
        numberOfKeys += chunks[chunk].numberOfKeys[static_cast<unsigned>(space)];
    }
    if (!numberOfKeys) {
        merged.firstWord.append(0);
        return merged;
    }
    if (!checkListSize(numberOfKeys, sizeof(GatheredKey), context)) {
        merged.firstWord.append(0);
        return merged;
    }

    Vector<GatheredKey> keys;
    keys.grow(numberOfKeys);
    forEachIndexInParallel(loops, static_cast<unsigned>(chunks.size()), [&](unsigned chunk) {
        size_t next = firstKeyOfChunk[chunk];
        std::span<const uint32_t> rest = chunks[chunk].wordsOf(space).span();
        while (!rest.empty()) {
            size_t sizeOfKey = rest[0];
            keys[next++] = { firstWordsOfGatheredKey(rest.subspan(1, sizeOfKey)), rest.data() };
            rest = rest.subspan(1 + sizeOfKey);
        }
    });
    auto firstWordsOf = [](const GatheredKey& key) {
        return key.firstWords;
    };
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, keys.mutableSpan(), firstWordsOf);
    sortRunsOfEqualWordsOnPool(loops, keys.mutableSpan(), firstWordsOf, [](const GatheredKey& a, const GatheredKey& b) {
        return std::ranges::lexicographical_compare(a.words(), b.words());
    });

    auto forEachDistinctKeyInRange = [&](size_t first, size_t end, const auto& functor) {
        for (size_t index = first; index < end; ++index) {
            if (!index || keys[index - 1].firstWords != keys[index].firstWords || !std::ranges::equal(keys[index - 1].words(), keys[index].words()))
                functor(keys[index].words());
        }
    };
    Vector<DistinctKeysOfRange> ranges(numberOfRangesOfIndices(numberOfKeys));
    forEachRangeOfIndicesInParallel(loops, numberOfKeys, [&](size_t first, size_t end) {
        DistinctKeysOfRange found;
        forEachDistinctKeyInRange(first, end, [&](std::span<const uint32_t> key) {
            ++found.numberOfKeys;
            found.numberOfWords += key.size();
        });
        ranges[first / indicesInRangeOfLoop] = found;
    });
    DistinctKeysOfRange all;
    for (DistinctKeysOfRange& range : ranges) {
        DistinctKeysOfRange inFront = all;
        all.numberOfKeys += range.numberOfKeys;
        all.numberOfWords += range.numberOfWords;
        range = inFront;
    }
    if (!checkListSize(all.numberOfWords, sizeof(uint32_t), context)) {
        merged.firstWord.append(0);
        return merged;
    }

    merged.words.grow(all.numberOfWords);
    merged.firstWord.grow(all.numberOfKeys + 1);
    merged.firstWord[all.numberOfKeys] = static_cast<uint32_t>(all.numberOfWords);
    forEachRangeOfIndicesInParallel(loops, numberOfKeys, [&](size_t first, size_t end) {
        DistinctKeysOfRange next = ranges[first / indicesInRangeOfLoop];
        forEachDistinctKeyInRange(first, end, [&](std::span<const uint32_t> key) {
            merged.firstWord[next.numberOfKeys++] = static_cast<uint32_t>(next.numberOfWords);
            memcpySpan(merged.words.mutableSpan().subspan(next.numberOfWords), key);
            next.numberOfWords += key.size();
        });
    });
    merged.buildHashIndex(loops);
    return merged;
}

static void addClassGuardsUnderMethodTableRows(const Typed::Plan& plan, std::span<const GatheredChunk> chunks, GatheredChunk& implied)
{
    const Typed::CallingConventions& conventions = plan.conventions();
    std::span<const Typed::MethodFamily> families = conventions.methodFamilies();
    BitVector familiesWithMentionedSelector(families.size());
    for (const GatheredChunk& chunk : chunks) {
        for (uint32_t selector : chunk.wordsOf(NumberSpace::MethodSelector))
            familiesWithMentionedSelector.set(conventions.methodSelector(selector).family);
    }
    Vector<uint32_t>& guards = implied.wordsOf(NumberSpace::ClassGuard);
    for (size_t index : familiesWithMentionedSelector) {
        const Typed::MethodFamily& family = families[index];
        if (!family.entersLookup())
            continue;
        implied.wordsOf(NumberSpace::Name).append(family.key.index());
        uint32_t& endOfNames = implied.endOfIDs[static_cast<unsigned>(NumberSpace::Name)];
        endOfNames = std::max(endOfNames, family.key.index() + 1);
        for (const Typed::MethodTableRow& row : family.rows) {
            if (!row.guardedClass)
                continue;
            std::array<uint32_t, 4> guard = keyOfClassGuardUnderMethodTableRow(family, row);
            guards.append(static_cast<uint32_t>(guard.size()));
            guards.append(std::span<const uint32_t>(guard));
            ++implied.numberOfKeys[static_cast<unsigned>(NumberSpace::ClassGuard)];
        }
    }
    if (size_t numberOfNames = implied.wordsOf(NumberSpace::Name).size())
        implied.idsOfUnitsOf(NumberSpace::Name).append(GatheredIDsOfUnit { .function = { }, .end = numberOfNames, .codeRunsAtMostOnce = false });
}

GatheredMentions gatherMentions(const NumberingInputs& inputs, BuildContext& context)
{
    StepTimes times(context, "image: gather mentions"_s);
    const ParallelLoops& loops = context.parallelLoops();
    const LinkedFunctions& functions = inputs.functions;

    size_t numberOfChunksOfFunctions = numberOfRangesOfIndices(functions.size());
    Vector<GatheredChunk> chunks(numberOfChunksOfFunctions + 1);
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        MentionGatherer gatherer(inputs.program, inputs.facts, context, chunks[first / indicesInRangeOfLoop]);
        for (size_t index = first; index < end; ++index)
            gatherer.gather(functions[index]);
    });
    addClassGuardsUnderMethodTableRows(inputs.plan, chunks.span().first(numberOfChunksOfFunctions), chunks.last());
    times.didFinish("gather"_s);

    GatheredMentions gathered;
    runLanes(loops, {
        [&] {
            StepTimes timesOfLane(context, "image: gather mentions"_s);
            runLanes(context.parallelLoops(), numberOfNumberSpaces, [&](unsigned index, const ParallelLoops&) {
                auto space = static_cast<NumberSpace>(index);
                if (!hasOneIDAsKey(space))
                    gathered.keySpaces[index] = mergeGatheredKeys(space, chunks.span(), context);
            });
            timesOfLane.didFinish("merge keys"_s);
        },
        [&] {
            StepTimes timesOfLane(context, "image: gather mentions"_s);
            forEachIndexInParallel(context.parallelLoops(), numberOfNumberSpaces, [&](unsigned index) {
                auto space = static_cast<NumberSpace>(index);
                if (hasOneIDAsKey(space))
                    gathered.idSpaces[index] = mergeGatheredIDs(space, chunks.span());
            });
            timesOfLane.didFinish("merge ids"_s);
        },
    });
    times.didFinish("merge keys beside ids"_s);

    uint64_t numberOfMentions = 0;
    for (const GatheredChunk& chunk : chunks)
        numberOfMentions += chunk.numberOfMentions;
    context.statistics().count(StatisticID::ThingsMentioned, numberOfMentions);
    forEachIndexInParallel(loops, static_cast<unsigned>(chunks.size()), [&](unsigned chunk) {
        chunks[chunk] = { };
    });
    times.didFinish("free the chunks"_s);
    return gathered;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
