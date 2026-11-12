/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompiledCode.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeIDs.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

static constexpr size_t fewestWordsOfSiteCacheKey = 4;

static constexpr size_t numberOfAllocationKeyHeaderWords = 7;
static_assert(sizeof(Typed::PlacedAllocation) == 6 * sizeof(uint32_t));
static_assert(!layoutWordOfAllocationKey && constructorWordOfAllocationKey == 4);
static_assert(std::is_same_v<FieldIndex, uint32_t>);

static bool mentionWordIsNotThreadLocalID(uint32_t word)
{
    return Typed::homeOf(TypeID::fromBits(word)) != Typed::IDHome::Thread;
}

static bool keyOfCardOfWordHasNoThreadLocalIDs(std::span<const uint32_t> key);

static ALWAYS_INLINE bool mentionKeyHasNoThreadLocalIDs(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
        return key.size() == 1 && !!StringID::fromBits(key[0]);
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassRank:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Type:
    case NumberSpace::MethodSelector:
        return key.size() == 1 && mentionWordIsNotThreadLocalID(key[0]);
    case NumberSpace::GlobalBinding:
        return key.size() == 2 && mentionWordIsNotThreadLocalID(key[0]);
    case NumberSpace::SiteCache:
        return key.size() >= fewestWordsOfSiteCacheKey && !(key.size() % 2) && mentionWordIsNotThreadLocalID(key[key.size() - 2]);
    case NumberSpace::PropertyRun:
        return std::ranges::all_of(key, mentionWordIsNotThreadLocalID);
    case NumberSpace::ReadGroupCache:
        return key.size() == 3 && mentionWordIsNotThreadLocalID(key[0]);
    case NumberSpace::CardOfWord:
        return keyOfCardOfWordHasNoThreadLocalIDs(key);
    case NumberSpace::RequireEdge:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ClassGuard:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::RegExp:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NEVER_INLINE bool keyOfCardOfWordHasNoThreadLocalIDs(std::span<const uint32_t> key)
{
    return key.size() > 1 && key[0] < numberOfNumberSpaces && mentionKeyHasNoThreadLocalIDs(static_cast<NumberSpace>(key[0]), key.subspan(1));
}

static bool requestMentionHasExpectedWordCount(RequestKind kind, size_t numberOfWords)
{
    switch (kind) {
    case RequestKind::NewClass:
        return numberOfWords == 1;
    case RequestKind::TemplateObject:
        return !(numberOfWords % 2);
    case RequestKind::DirectEval:
    case RequestKind::Data:
    case RequestKind::CollectionData:
        return numberOfWords == 2;
    case RequestKind::FormatPattern:
        return !!numberOfWords;
    case RequestKind::ImportChecks:
        return numberOfWords && !(numberOfWords % 2);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE uint32_t hashOfMentionKey(NumberSpace space, std::span<const uint32_t> key, int32_t addend)
{
    constexpr uint32_t multiplier = 0x9e3779b1u;
    uint32_t hash = ((static_cast<uint32_t>(space) + 1) * multiplier) ^ static_cast<uint32_t>(addend);
    for (uint32_t word : key) {
        hash = (hash ^ word) * multiplier;
        hash ^= hash >> 15;
    }
    return hash;
}

static void insertIntoMentionTable(std::span<uint32_t> table, uint32_t hash, uint32_t indexPlusOne)
{
    size_t mask = table.size() - 1;
    size_t place = hash & mask;
    while (table[place])
        place = (place + 1) & mask;
    table[place] = indexPlusOne;
}

NEVER_INLINE void MentionList::growTable()
{
    m_table.fill(0, std::max(smallestTableSize, 4 * m_table.size()));
    m_mentions.reserveCapacity(m_table.size() / 2);
    m_words.reserveCapacity(m_table.size());
    std::span<uint32_t> table = m_table.mutableSpan();
    std::span<const uint32_t> words = m_words.span();
    for (uint32_t index = 0; index < m_mentions.size(); ++index) {
        const Mention& mention = m_mentions[index];
        insertIntoMentionTable(table, hashOfMentionKey(mention.space, keyOf(mention, words), mention.addend), index + 1);
    }
}

template<size_t extent>
ALWAYS_INLINE uint32_t MentionList::addKey(NumberSpace space, std::span<const uint32_t, extent> key, int32_t addend)
{
    uint8_t numberOfWords = key.size() < Mention::longKey ? static_cast<uint8_t>(key.size()) : Mention::longKey;
    if (2 * (m_mentions.size() + 1) > m_table.size())
        growTable();

    std::span<const Mention> mentions = m_mentions.span();
    std::span<const uint32_t> words = m_words.span();
    auto hasKey = [&](const Mention& mention) ALWAYS_INLINE_LAMBDA {
        if (mention.space != space || mention.numberOfWords != numberOfWords || mention.addend != addend)
            return false;
        if (numberOfWords == 1)
            return mention.firstWord == key[0];
        size_t firstWord = mention.firstWord;
        if (numberOfWords == Mention::longKey && words[firstWord++] != key.size())
            return false;
        std::span<const uint32_t> wordsOfMention = words.subspan(firstWord, key.size());
        for (size_t index = 0; index < key.size(); ++index) {
            if (wordsOfMention[index] != key[index])
                return false;
        }
        return true;
    };

    std::span<const uint32_t> table = m_table.span();
    size_t mask = table.size() - 1;
    size_t place = hashOfMentionKey(space, key, addend) & mask;
    for (; table[place]; place = (place + 1) & mask) {
        if (hasKey(mentions[table[place] - 1]))
            return table[place] - 1;
    }

    RELEASE_ASSERT(mentionKeyHasNoThreadLocalIDs(space, key));
    Mention mention { .space = space, .numberOfWords = numberOfWords, .firstWord = numberOfWords == 1 ? key[0] : static_cast<uint32_t>(words.size()), .addend = addend };
    if (numberOfWords == Mention::longKey)
        m_words.append(static_cast<uint32_t>(key.size()));
    if (numberOfWords != 1)
        m_words.append(key);
    m_mentions.append(mention);
    m_table[place] = static_cast<uint32_t>(m_mentions.size());
    return static_cast<uint32_t>(m_mentions.size()) - 1;
}

uint32_t MentionList::add(NumberSpace space, std::span<const uint32_t> key, int32_t addend)
{
    if (key.size() == fewestWordsOfSiteCacheKey)
        return addKey(space, key.first<fewestWordsOfSiteCacheKey>(), addend);
    return addKey(space, key, addend);
}

uint32_t MentionList::add(NumberSpace space, uint32_t key, int32_t addend)
{
    return addKey(space, std::span<const uint32_t, 1>(&key, 1), addend);
}

uint32_t MentionList::addPair(NumberSpace space, uint32_t first, uint32_t second, int32_t addend)
{
    const uint32_t key[] { first, second };
    return addKey(space, std::span<const uint32_t, 2>(key), addend);
}

uint32_t MentionList::allocation(const Typed::PlacedAllocation& allocation, std::span<const FieldIndex> initialFields, int32_t addend)
{
    const uint32_t words[numberOfAllocationKeyHeaderWords] {
        allocation.layout.bits(),
        allocation.shape,
        allocation.size,
        allocation.orderWord,
        allocation.constructedBy.bits(),
        allocation.numberOfCaptures,
        allocation.isBornUnfinished,
    };
    Vector<uint32_t, 24> key;
    key.append(std::span<const uint32_t>(words));
    key.append(initialFields);
    return add(NumberSpace::Allocation, key.span(), addend);
}

AllocationKey decodeAllocationKey(std::span<const uint32_t> key)
{
    RELEASE_ASSERT(key.size() >= numberOfAllocationKeyHeaderWords && key[5] <= UINT16_MAX && key[6] <= 1);
    Typed::PlacedAllocation allocation;
    allocation.layout = LayoutID::fromBits(key[0]);
    allocation.shape = key[1];
    allocation.size = key[2];
    allocation.orderWord = key[3];
    allocation.constructedBy = FunctionID::fromBits(key[4]);
    allocation.numberOfCaptures = static_cast<uint16_t>(key[5]);
    allocation.isBornUnfinished = !!key[6];
    return { allocation, key.subspan(numberOfAllocationKeyHeaderWords) };
}

uint32_t MentionList::request(RequestKind kind, std::span<const uint32_t> words)
{
    RELEASE_ASSERT(requestMentionHasExpectedWordCount(kind, words.size()));
    Vector<uint32_t, 8> key;
    key.append(static_cast<uint32_t>(kind));
    key.append(words);
    return add(NumberSpace::Request, key.span());
}

uint32_t MentionList::classGuardCount(std::span<const ClassGuardKey> guards)
{
    Vector<std::span<const uint32_t>, 16> ascending(guards.size(), [&](size_t index) {
        return guards[index].span();
    });
    AOT::sort(ascending, [](std::span<const uint32_t> first, std::span<const uint32_t> second) {
        return std::ranges::lexicographical_compare(first, second);
    });
    Vector<uint32_t, 64> key { firstWordOfClassGuardCountKey };
    size_t numberOfGuards = 0;
    for (size_t index = 0; index < ascending.size(); ++index) {
        if (index && std::ranges::equal(ascending[index - 1], ascending[index]))
            continue;
        key.append(static_cast<uint32_t>(ascending[index].size()));
        key.append(ascending[index]);
        ++numberOfGuards;
    }
    RELEASE_ASSERT(numberOfGuards > 1);
    return add(NumberSpace::ClassGuard, key.span());
}

uint32_t MentionList::rowOfClassGuardCounts(std::span<const Vector<ClassGuardKey, 4>> slots)
{
    RELEASE_ASSERT(!slots.empty());
    Vector<uint32_t, 256> key { firstWordOfRowOfClassGuardCountsKey };
    for (const Vector<ClassGuardKey, 4>& guards : slots) {
        Vector<std::span<const uint32_t>, 16> ascending(guards.size(), [&](size_t index) {
            return guards[index].span();
        });
        AOT::sort(ascending, [](std::span<const uint32_t> first, std::span<const uint32_t> second) {
            return std::ranges::lexicographical_compare(first, second);
        });
        size_t placeOfSize = key.size();
        key.append(0);
        for (size_t index = 0; index < ascending.size(); ++index) {
            if (index && std::ranges::equal(ascending[index - 1], ascending[index]))
                continue;
            key.append(static_cast<uint32_t>(ascending[index].size()));
            key.append(ascending[index]);
        }
        key[placeOfSize] = static_cast<uint32_t>(key.size() - placeOfSize - 1);
    }
    return add(NumberSpace::ClassGuard, key.span());
}

void MentionList::moveTo(CodeBody& body)
{
    m_table.shrink(0);
    body.mentions = WTF::move(m_mentions);
    body.mentionWords = WTF::move(m_words);
}

static constexpr bool mentionKeyBeginsWithText(NumberSpace space)
{
    switch (space) {
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::RegExp:
        return true;
    default:
        return false;
    }
}

static std::optional<size_t> placeOfTextInMentionKey(NumberSpace space, std::span<const uint32_t> key)
{
    if (mentionKeyBeginsWithText(space))
        return 0;
    switch (space) {
    case NumberSpace::RegExpLiteralCell:
        return key.size() == 4 ? std::optional<size_t>(2) : std::nullopt;
    case NumberSpace::CardOfWord: {
        if (key.size() < 2 || key[0] >= numberOfNumberSpaces)
            return std::nullopt;
        std::optional<size_t> place = placeOfTextInMentionKey(static_cast<NumberSpace>(key[0]), key.subspan(1));
        return place ? std::optional<size_t>(*place + 1) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

static void forEachMadeTextAmongMentions(CodeBody& body, NOESCAPE const Invocable<void(uint32_t&)> auto& functor)
{
    std::span<uint32_t> words = body.mentionWords.mutableSpan();
    for (Mention& mention : body.mentions) {
        size_t place = 0;
        if (!mentionKeyBeginsWithText(mention.space)) {
            if (mention.space != NumberSpace::RegExpLiteralCell && mention.space != NumberSpace::CardOfWord)
                continue;
            std::optional<size_t> placeInKey = placeOfTextInMentionKey(mention.space, keyOf(mention, words));
            if (!placeInKey)
                continue;
            place = *placeInKey;
        }
        size_t firstWordOfKey = mention.numberOfWords == Mention::longKey ? mention.firstWord + 1 : mention.firstWord;
        uint32_t& word = mention.numberOfWords == 1 ? mention.firstWord : words[firstWordOfKey + place];
        if (Typed::liesInThread(StringID::fromBits(word)))
            functor(word);
    }
}

void copyMadeTextsMentionedBy(CodeBody& body, const Typed::TypeArena& types)
{
    Vector<uint32_t, 8> placesInBodyPlusOne;
    Vector<StringID, 8> texts;
    forEachMadeTextAmongMentions(body, [&](uint32_t& word) {
        StringID text = StringID::fromBits(word);
        while (placesInBodyPlusOne.size() <= Typed::positionOf(text))
            placesInBodyPlusOne.append(0);
        uint32_t& placePlusOne = placesInBodyPlusOne[Typed::positionOf(text)];
        if (!placePlusOne) {
            texts.append(text);
            types.bytesOf(text);
            placePlusOne = static_cast<uint32_t>(texts.size());
        }
        word = Typed::idAt<StringID>(Typed::IDHome::Thread, placePlusOne - 1).bits();
    });
    if (texts.isEmpty())
        return;

    struct TextsAtAddress {
        size_t mostBytes { 0 };
        std::optional<uint32_t> firstInBody;
    };
    HashMap<const uint8_t*, TextsAtAddress> textsAtAddresses;
    for (StringID text : texts) {
        std::span<const uint8_t> bytes = types.bytesOf(text);
        size_t& mostBytes = textsAtAddresses.add(bytes.data(), TextsAtAddress { }).iterator->value.mostBytes;
        mostBytes = std::max(mostBytes, bytes.size());
    }
    RareListsOfBody& lists = body.rareLists.ensure();
    for (StringID text : texts) {
        std::span<const uint8_t> bytes = types.bytesOf(text);
        TextsAtAddress& atAddress = textsAtAddresses.find(bytes.data())->value;
        if (!atAddress.firstInBody) {
            atAddress.firstInBody = static_cast<uint32_t>(lists.bytesOfMadeTexts.size());
            lists.bytesOfMadeTexts.append(std::span<const uint8_t>(bytes.data(), atAddress.mostBytes));
        }
        lists.firstsOfMadeTexts.append(*atAddress.firstInBody);
        lists.endsOfMadeTexts.append(*atAddress.firstInBody + static_cast<uint32_t>(bytes.size()));
    }
}

void internTextsMadeByFunctions(Typed::Program& program, LinkedFunctions& functions, BuildContext& context)
{
    struct MadeText {
        std::span<const uint8_t> bytes;
        uint32_t number;
    };
    Vector<MadeText> texts;
    Vector<uint32_t> numbersOfFirstTexts(functions.size());
    for (size_t index = 0; index < functions.size(); ++index) {
        const RareListsOfBody& lists = *functions[index].body.rareLists;
        numbersOfFirstTexts[index] = static_cast<uint32_t>(texts.size());
        if (lists.endsOfMadeTexts.isEmpty())
            continue;
        std::span<const uint8_t> kept = program.strings.keep(lists.bytesOfMadeTexts.span());
        for (uint32_t place = 0; place < lists.endsOfMadeTexts.size(); ++place)
            texts.append({ kept.subspan(lists.firstsOfMadeTexts[place], lists.endsOfMadeTexts[place] - lists.firstsOfMadeTexts[place]), static_cast<uint32_t>(texts.size()) });
    }
    if (texts.isEmpty())
        return;

    const ParallelLoops& loops = context.parallelLoops();
    parallelSort(loops, texts.mutableSpan(), [](const MadeText& first, const MadeText& second) {
        if (first.bytes.size() != second.bytes.size())
            return first.bytes.size() < second.bytes.size();
        return first.bytes.data() != second.bytes.data() && std::ranges::lexicographical_compare(first.bytes, second.bytes);
    });
    Vector<StringID> strings(texts.size());
    Typed::StringTable::FrontsOfKeptTexts fronts;
    for (size_t place = 0; place < texts.size(); ++place) {
        std::span<const uint8_t> bytes = texts[place].bytes;
        std::span<const uint8_t> before = place ? texts[place - 1].bytes : std::span<const uint8_t>();
        bool isTextBefore = place && bytes.size() == before.size() && (bytes.data() == before.data() || equalSpans(bytes, before));
        strings[texts[place].number] = isTextBefore ? strings[texts[place - 1].number] : program.strings.internKept(bytes, fronts);
    }

    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            CodeBody& body = functions[index].body;
            RareListsOfBody* lists = body.rareLists.ifExists();
            if (!lists || lists->endsOfMadeTexts.isEmpty())
                continue;
            forEachMadeTextAmongMentions(body, [&](uint32_t& word) {
                word = strings[numbersOfFirstTexts[index] + Typed::positionOf(StringID::fromBits(word))].bits();
            });
            lists->bytesOfMadeTexts = { };
            lists->firstsOfMadeTexts = { };
            lists->endsOfMadeTexts = { };
        }
    });
}

static uint64_t hashOfNumberedKey(std::span<const uint32_t> key)
{
    uint64_t hash = key.size();
    for (uint32_t word : key)
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
    return hash;
}

void NumberedKeys::buildHashIndex(const ParallelLoops& loops)
{
    static constexpr uint32_t minSizeForHashIndex = 64;
    hashSlots = { };
    uint32_t count = size();
    if (count < minSizeForHashIndex)
        return;
    size_t numberOfSlots = std::bit_ceil(static_cast<size_t>(count) * 2);
    unsigned shift = 64 - std::countr_zero(numberOfSlots);
    hashSlots.grow(numberOfSlots);
    forEachRangeOfIndicesInParallel(loops, numberOfSlots, [&](size_t first, size_t end) {
        zeroSpan(hashSlots.mutableSpan().subspan(first, end - first));
    });
    forEachRangeOfIndicesInParallel(loops, count, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            for (size_t slot = hashOfNumberedKey(keyAt(static_cast<uint32_t>(index))) >> shift;; slot = (slot + 1) & (numberOfSlots - 1)) {
                uint32_t expected = 0;
                if (AtomicRef(hashSlots[slot]).compare_exchange_strong(expected, static_cast<uint32_t>(index) + 1, std::memory_order_relaxed))
                    break;
            }
        }
    });
}

static ALWAYS_INLINE bool areSameNumberedKeys(std::span<const uint32_t> first, std::span<const uint32_t> second)
{
    if (first.size() != second.size())
        return false;
    for (size_t index = 0; index < first.size(); ++index) {
        if (first[index] != second[index])
            return false;
    }
    return true;
}

std::optional<uint32_t> NumberedKeys::find(std::span<const uint32_t> key) const
{
    if (!hashSlots.isEmpty()) {
        std::span<const uint32_t> slots = hashSlots.span();
        size_t mask = slots.size() - 1;
        unsigned shift = 64 - std::countr_zero(slots.size());
        for (size_t slot = hashOfNumberedKey(key) >> shift;; slot = (slot + 1) & mask) {
            uint32_t indexPlusOne = slots[slot];
            if (!indexPlusOne)
                return std::nullopt;
            if (areSameNumberedKeys(keyAt(indexPlusOne - 1), key))
                return indexPlusOne - 1;
        }
    }

    uint32_t first = 0;
    uint32_t end = size();
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (std::ranges::lexicographical_compare(keyAt(middle), key))
            first = middle + 1;
        else
            end = middle;
    }
    if (first == size() || !std::ranges::equal(keyAt(first), key))
        return std::nullopt;
    return first;
}

namespace {

class UnitsBehindMention {
public:
    UnitsBehindMention(const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, const ScopedLambda<void(CodeUnit, MentionKind)>& functor, const AsksForUnitsOfClass* asksForUnitsOfClass)
        : m_program(program)
        , m_facts(facts)
        , m_plan(plan)
        , m_conventions(plan.conventions())
        , m_functor(functor)
        , m_asksForUnitsOfClass(asksForUnitsOfClass)
    {
    }

    void addClosureOf(FunctionID) const;
    ALWAYS_INLINE void addClosureIfPlanned(FunctionID function) const
    {
        if (hasPlannedEntry(function))
            addClosureOf(function);
    }
    void addClass(LayoutID) const;
    void addModule(ModuleID) const;
    void addAllocation(std::span<const uint32_t> key) const;

private:
    bool hasPlannedEntry(FunctionID function) const { return function && m_conventions.exists(m_conventions.closureEntryOf(function)); }
    void addEntriesOf(FunctionID) const;
    void addEntriesIfPlanned(FunctionID) const;

    const Typed::Program& m_program;
    const Typed::Facts& m_facts;
    const Typed::Plan& m_plan;
    const Typed::CallingConventions& m_conventions;
    const ScopedLambda<void(CodeUnit, MentionKind)>& m_functor;
    const AsksForUnitsOfClass* m_asksForUnitsOfClass;
};

void UnitsBehindMention::addEntriesOf(FunctionID function) const
{
    CodeUnit entry = m_conventions.closureEntryOf(function);
    m_functor(entry, MentionKind::ClosureWord);
    if (CodeUnit constructEntry = m_conventions.genericEntry(entry).constructEntry)
        m_functor(constructEntry, MentionKind::ClosureWord);
}

void UnitsBehindMention::addEntriesIfPlanned(FunctionID function) const
{
    if (hasPlannedEntry(function))
        addEntriesOf(function);
}

void UnitsBehindMention::addClass(LayoutID classLayout) const
{
    size_t numberOfLayouts = m_program.layouts.size();
    for (size_t steps = 0; classLayout && m_plan.hasClassRecord(classLayout) && steps < numberOfLayouts; ++steps) {
        if (!m_asksForUnitsOfClass || (*m_asksForUnitsOfClass)(classLayout)) {
            addEntriesIfPlanned(m_program.at(classLayout).constructor);
            std::span<const Typed::ClassMember> members = m_facts.membersOf(classLayout);
            const Vector<Typed::MemberCreation>& creations = m_plan.slots().classCreation(classLayout).members;
            for (size_t index = 0; index < members.size() && index < creations.size(); ++index) {
                switch (creations[index]) {
                case Typed::MemberCreation::AtDefinition:
                    break;
                case Typed::MemberCreation::ByClassRecord:
                    addEntriesIfPlanned(members[index].function);
                    break;
                }
            }
        }
        Typed::ClassFacts classFacts = m_facts.classOf(classLayout);
        classLayout = classFacts.baseState == Typed::BaseState::Layout ? classFacts.base : LayoutID();
    }
    if (m_asksForUnitsOfClass)
        (*m_asksForUnitsOfClass)(LayoutID());
}

void UnitsBehindMention::addClosureOf(FunctionID function) const
{
    addEntriesOf(function);
    addClass(m_program.at(function).constructedClass);
}

void UnitsBehindMention::addModule(ModuleID module) const
{
    const Typed::ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    if (evaluation.isEvaluatedByCodeAlone(module))
        return;
    bool hasStart = !!evaluation.findStart(module);
    if (!hasStart && !m_plan.moduleBodyIsEntered(module))
        return;
    FunctionID body = m_program.at(module).body;
    CodeUnit entered { body, hasStart ? Typed::CodeRole::ModuleStart : Typed::CodeRole::Body };
    if (!body || !m_conventions.exists(entered))
        return;
    if (!hasStart)
        m_functor(entered, MentionKind::Call);
    addEntriesIfPlanned(body);
}

void UnitsBehindMention::addAllocation(std::span<const uint32_t> key) const
{
    Typed::PlacedAllocation allocation = decodeAllocationKey(key).allocation;
    addClosureIfPlanned(allocation.constructedBy);
    for (const Typed::PlacedSlot& slot : m_plan.layout(allocation.layout).slots)
        addClosureIfPlanned(slot.deferredFunction);
}

} // anonymous namespace

static LayoutID classOfRequestMention(std::span<const uint32_t> key)
{
    RELEASE_ASSERT(!key.empty());
    switch (static_cast<RequestKind>(key[0])) {
    case RequestKind::NewClass:
        RELEASE_ASSERT(key.size() == 2);
        return LayoutID(key[1]);
    case RequestKind::TemplateObject:
    case RequestKind::DirectEval:
    case RequestKind::Data:
    case RequestKind::CollectionData:
    case RequestKind::FormatPattern:
    case RequestKind::ImportChecks:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachUnitBehind(const Mention& mention, std::span<const uint32_t> mentionWords, const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, NOESCAPE const ScopedLambda<void(CodeUnit)>& functor)
{
    forEachUnitBehind(mention, mentionWords, program, facts, plan, scopedLambda<void(CodeUnit, MentionKind)>([&](CodeUnit unit, MentionKind) {
        functor(unit);
    }));
}

void forEachUnitBehindSlowCase(const Mention& mention, std::span<const uint32_t> mentionWords, const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, NOESCAPE const ScopedLambda<void(CodeUnit, MentionKind)>& functor, const AsksForUnitsOfClass* asksForUnitsOfClass)
{
    UnitsBehindMention units(program, facts, plan, functor, asksForUnitsOfClass);
    std::span<const uint32_t> key = keyOf(mention, mentionWords);
    switch (mention.space) {
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
        units.addClosureOf(FunctionID(key[0]));
        return;
    case NumberSpace::ClassRank:
        units.addClass(LayoutID(key[0]));
        return;
    case NumberSpace::Request:
        units.addClass(classOfRequestMention(key));
        if (static_cast<RequestKind>(key[0]) == RequestKind::ImportChecks) {
            for (size_t index = 1; index < key.size(); index += 2)
                units.addClosureIfPlanned(plan.variable(VariableID(key[index])).function);
        }
        return;
    case NumberSpace::ModuleValue:
        units.addModule(ModuleID(key[0]));
        return;
    case NumberSpace::RequireEdge:
        RELEASE_ASSERT(key.size() == 3);
        units.addModule(ModuleID(key[1]));
        return;
    case NumberSpace::Allocation:
        units.addAllocation(key);
        return;
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
        units.addClosureIfPlanned(plan.variable(VariableID(key[0])).function);
        return;
    case NumberSpace::MethodSelector: {
        const Typed::CallingConventions& conventions = plan.conventions();
        const Typed::MethodSelector& selector = conventions.methodSelector(key[0]);
        const Typed::MethodFamily& family = conventions.familyOf(selector);
        for (size_t place = 0; place < family.methods.size() && !selector.everyMethodIsEnteredDirectly; ++place) {
            if (!selector.methodsEnteredDirectly.get(place))
                functor(conventions.unitEnteredThroughTable(program, selector, family.methods[place]), MentionKind::Call);
        }
        if (family.entersLookup())
            functor(conventions.tableLookupOf(selector), MentionKind::Call);
        return;
    }
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::GlobalBinding:
    case NumberSpace::SiteCache:
    case NumberSpace::CardOfWord:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::PropertyRun:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::Type:
    case NumberSpace::RegExp:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
