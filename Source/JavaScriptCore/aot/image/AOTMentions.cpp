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

namespace JSC { namespace AOT {

// ---- The list

// A power of two, like every size of the table.
static constexpr size_t minimumMentionTableSize = 64;
static constexpr size_t fewestMentionsWithTable = 16;
static_assert(2 * (fewestMentionsWithTable + 1) <= minimumMentionTableSize);
static constexpr size_t fewestWordsOfSiteCacheKey = 4;

static constexpr size_t numberOfAllocationKeyHeaderWords = 7;
// MentionList::allocation() and decodeAllocationKey() name every member.
static_assert(sizeof(Typed::PlacedAllocation) == 6 * sizeof(uint32_t));
static_assert(!layoutWordOfAllocationKey && constructorWordOfAllocationKey == 4);
static_assert(std::is_same_v<FieldIndex, uint32_t>);

// The null id has the bits of a thread-local id, so it is turned away too.
static bool mentionWordIsNotThreadLocalID(uint32_t word)
{
    return Typed::homeOf(TypeID::fromBits(word)) != Typed::IDHome::Thread;
}

static bool mentionKeyHasNoThreadLocalIDs(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    // A text that the walk has made is mentioned like a text of the program: copyMadeTextsMentionedBy().
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::AtomOfComparedText:
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
    case NumberSpace::FilledFiles:
    case NumberSpace::DataCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Function:
    case NumberSpace::Type:
    case NumberSpace::MethodSelector:
        return key.size() == 1 && mentionWordIsNotThreadLocalID(key[0]);
    case NumberSpace::GlobalBinding:
        return key.size() == 2 && mentionWordIsNotThreadLocalID(key[0]);
    case NumberSpace::SiteCache:
        return key.size() >= fewestWordsOfSiteCacheKey && !(key.size() % 2) && mentionWordIsNotThreadLocalID(key[key.size() - 2]);
    case NumberSpace::PropertyRun:
        return std::ranges::all_of(key, mentionWordIsNotThreadLocalID);
    case NumberSpace::CardOfWord:
        return key.size() > 1 && key[0] < numberOfNumberSpaces && mentionKeyHasNoThreadLocalIDs(static_cast<NumberSpace>(key[0]), key.subspan(1));
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

std::span<const uint32_t> keyOf(const Mention& mention, std::span<const uint32_t> mentionWords)
{
    if (mention.numberOfWords != Mention::longKey)
        return mentionWords.subspan(mention.firstWord, mention.numberOfWords);
    return mentionWords.subspan(mention.firstWord + 1, mentionWords[mention.firstWord]);
}

NEVER_INLINE void MentionList::growTable()
{
    m_table.fill(0, std::max(minimumMentionTableSize, 2 * m_table.size()));
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
    std::span<const Mention> mentions = m_mentions.span();
    std::span<const uint32_t> words = m_words.span();
    auto hasKey = [&](const Mention& mention) ALWAYS_INLINE_LAMBDA {
        if (mention.space != space || mention.numberOfWords != numberOfWords || mention.addend != addend)
            return false;
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

    bool usesTable = mentions.size() >= fewestMentionsWithTable;
    size_t place = 0;
    if (usesTable) {
        if (2 * (mentions.size() + 1) > m_table.size())
            growTable();

        std::span<const uint32_t> table = m_table.span();
        size_t mask = table.size() - 1;
        place = hashOfMentionKey(space, key, addend) & mask;
        for (; table[place]; place = (place + 1) & mask) {
            if (hasKey(mentions[table[place] - 1]))
                return table[place] - 1;
        }
    } else {
        for (size_t index = 0; index < mentions.size(); ++index) {
            if (hasKey(mentions[index]))
                return static_cast<uint32_t>(index);
        }
    }

    RELEASE_ASSERT(mentionKeyHasNoThreadLocalIDs(space, key));
    Mention mention { .space = space, .numberOfWords = numberOfWords, .firstWord = static_cast<uint32_t>(words.size()), .addend = addend };
    if (numberOfWords == Mention::longKey)
        m_words.append(static_cast<uint32_t>(key.size()));
    m_words.append(key);
    m_mentions.append(mention);
    if (usesTable)
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

// Rows lie in the order of their keys, and the run time looks for a record to share among the neighbors of a row that have its shape: the layout comes first, then the shape.
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

void MentionList::moveTo(CodeBody& body)
{
    m_table.shrink(0);
    body.mentions = WTF::move(m_mentions);
    body.mentionWords = WTF::move(m_words);
}

// ---- Texts that the walk has made

// The word of a key that names a text which the walk may have made. A name is never one: TypeArena::textKey().
static std::optional<size_t> placeOfTextInMentionKey(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::AtomOfComparedText:
    case NumberSpace::RegExp:
        return 0;
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
    for (const Mention& mention : body.mentions) {
        std::optional<size_t> place = placeOfTextInMentionKey(mention.space, keyOf(mention, words));
        if (!place)
            continue;
        size_t firstWordOfKey = mention.numberOfWords == Mention::longKey ? mention.firstWord + 1 : mention.firstWord;
        uint32_t& word = words[firstWordOfKey + *place];
        if (Typed::liesInThread(StringID::fromBits(word)))
            functor(word);
    }
}

static std::span<const uint8_t> madeTextOfCodeBody(const CodeBody& body, uint32_t place)
{
    uint32_t first = place ? body.endsOfMadeTexts[place - 1] : 0;
    return body.bytesOfMadeTexts.subspan(first, body.endsOfMadeTexts[place] - first);
}

void copyMadeTextsMentionedBy(CodeBody& body, const Typed::TypeArena& types)
{
    // [position in the arena]
    Vector<uint32_t, 8> placesInBodyPlusOne;
    forEachMadeTextAmongMentions(body, [&](uint32_t& word) {
        StringID text = StringID::fromBits(word);
        while (placesInBodyPlusOne.size() <= Typed::positionOf(text))
            placesInBodyPlusOne.append(0);
        uint32_t& placePlusOne = placesInBodyPlusOne[Typed::positionOf(text)];
        if (!placePlusOne) {
            body.bytesOfMadeTexts.append(types.bytesOf(text));
            body.endsOfMadeTexts.append(static_cast<uint32_t>(body.bytesOfMadeTexts.size()));
            placePlusOne = static_cast<uint32_t>(body.endsOfMadeTexts.size());
        }
        word = Typed::idAt<StringID>(Typed::IDHome::Thread, placePlusOne - 1).bits();
    });
}

void internTextsMadeByFunctions(Typed::Program& program, LinkedFunctions& functions, BuildContext& context)
{
    Vector<std::span<const uint8_t>> texts;
    for (const CompiledCode& code : functions) {
        for (uint32_t place = 0; place < code.body.endsOfMadeTexts.size(); ++place)
            texts.append(madeTextOfCodeBody(code.body, place));
    }
    if (texts.isEmpty())
        return;

    // A text that the table lacks gets its number in the order of the texts, whichever function made it.
    const ParallelLoops& loops = context.parallelLoops();
    sortAndRemoveDuplicates(loops, texts, [](std::span<const uint8_t> first, std::span<const uint8_t> second) {
        return std::ranges::lexicographical_compare(first, second);
    }, [](std::span<const uint8_t> first, std::span<const uint8_t> second) {
        return std::ranges::equal(first, second);
    });
    for (std::span<const uint8_t> text : texts)
        program.strings.intern(text);

    forEachIndexInParallel(loops, static_cast<unsigned>(functions.size()), [&](unsigned index) {
        CodeBody& body = functions[index].body;
        if (body.endsOfMadeTexts.isEmpty())
            return;
        forEachMadeTextAmongMentions(body, [&](uint32_t& word) {
            StringID string = program.strings.find(madeTextOfCodeBody(body, Typed::positionOf(StringID::fromBits(word))));
            RELEASE_ASSERT(string);
            word = string.bits();
        });
        body.bytesOfMadeTexts = { };
        body.endsOfMadeTexts = { };
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

std::optional<uint32_t> NumberedKeys::find(std::span<const uint32_t> key) const
{
    if (!hashSlots.isEmpty()) {
        size_t mask = hashSlots.size() - 1;
        unsigned shift = 64 - std::countr_zero(hashSlots.size());
        for (size_t slot = hashOfNumberedKey(key) >> shift;; slot = (slot + 1) & mask) {
            uint32_t indexPlusOne = hashSlots[slot];
            if (!indexPlusOne)
                return std::nullopt;
            if (std::ranges::equal(keyAt(indexPlusOne - 1), key))
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

// ---- The units behind a mention

namespace {

class UnitsBehindMention {
public:
    UnitsBehindMention(const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, const ScopedLambda<void(CodeUnit, MentionKind)>& functor)
        : m_program(program)
        , m_facts(facts)
        , m_plan(plan)
        , m_conventions(plan.conventions())
        , m_functor(functor)
    {
    }

    void addUnit(CodeUnit) const;
    // Code names the closure itself, so the plan has its entry: the queue stops the build where it has not.
    void addClosureOf(FunctionID) const;
    // A table of the image is going to name the closure. It stops the build where it needs an entry that has no code.
    void addClosureIfPlanned(FunctionID) const;
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

// The record of a class whose constructor has an entry names the entry of each member that it creates, called or not.
// No NewClass stands in the code for a deferred class: the run time makes it of its record when it makes the closure of the constructor, and first the classes above it of theirs.
void UnitsBehindMention::addClass(LayoutID classLayout) const
{
    size_t numberOfLayouts = m_program.layouts.size();
    for (size_t steps = 0; classLayout && m_plan.hasClassRecord(classLayout) && steps < numberOfLayouts; ++steps) {
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
        Typed::ClassFacts classFacts = m_facts.classOf(classLayout);
        classLayout = classFacts.baseState == Typed::BaseState::Layout ? classFacts.base : LayoutID();
    }
}

void UnitsBehindMention::addClosureOf(FunctionID function) const
{
    addEntriesOf(function);
    addClass(m_program.at(function).constructedClass);
}

void UnitsBehindMention::addClosureIfPlanned(FunctionID function) const
{
    if (hasPlannedEntry(function))
        addClosureOf(function);
}

// Once the entry for calls has code, the tables take the function for one that closures are made of, however the entry was named.
// The row of a bound function that holds no closure of its target names the entry of the target.
void UnitsBehindMention::addUnit(CodeUnit unit) const
{
    if (unit == m_conventions.closureEntryOf(unit.function)) {
        addClosureOf(unit.function);
        return;
    }
    m_functor(unit, MentionKind::Call);
    if (Typed::isBoundRole(unit.role))
        addClosureIfPlanned(unit.function);
}

// Nothing for a module that compiled code alone evaluates. The loader never enters it, so the table of modules names no entry for it, and the step that calls its body or its start names that unit in a relocation.
// The loader enters any other module in the generic convention: the table names the entry that the word of a closure would name. A body that Facts never reaches has neither unit.
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
    // A start has no code, and what its row calls is among unitsNamedOutsideCode().
    if (!hasStart)
        m_functor(entered, MentionKind::Call);
    addEntriesIfPlanned(body);
}

// The row of an allocation names the entry of the constructor, by which the run time finds the class of the object. The record of the layout names the entry of each closure that a slot defers.
void UnitsBehindMention::addAllocation(std::span<const uint32_t> key) const
{
    Typed::PlacedAllocation allocation = decodeAllocationKey(key).allocation;
    addClosureIfPlanned(allocation.constructedBy);
    for (const Typed::PlacedSlot& slot : m_plan.layout(allocation.layout).slots)
        addClosureIfPlanned(slot.deferredFunction);
}

} // anonymous namespace

// Null for a request that makes no class.
// A request for data brings no unit. Its objects have layouts of literals, which no function constructs, and none of their slots holds uncreatedDeferredClosure: only an allocation in code stores that, and it mentions its own row.
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

void forEachUnitBehind(const Mention& mention, std::span<const uint32_t> mentionWords, const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, NOESCAPE const ScopedLambda<void(CodeUnit, MentionKind)>& functor)
{
    UnitsBehindMention units(program, facts, plan, functor);
    std::span<const uint32_t> key = keyOf(mention, mentionWords);
    switch (mention.space) {
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
        units.addClosureOf(FunctionID(key[0]));
        return;
    case NumberSpace::Function:
        units.addUnit(plan.conventions().unitAt(key[0]));
        return;
    case NumberSpace::ClassRank:
        units.addClass(LayoutID(key[0]));
        return;
    case NumberSpace::Request:
        units.addClass(classOfRequestMention(key));
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
    // The table of variables that are born as functions names the entry for each of them that has a word, and a store alone gives a variable its word.
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
        units.addClosureIfPlanned(plan.variable(VariableID(key[0])).function);
        return;
    // The table holds a unit for each row of the family, whichever of them the code that mentions the selector may meet.
    case NumberSpace::MethodSelector: {
        const Typed::CallingConventions& conventions = plan.conventions();
        const Typed::MethodSelector& selector = conventions.methodSelector(key[0]);
        const Typed::MethodFamily& family = conventions.familyOf(selector);
        for (FunctionID method : family.methods)
            functor(conventions.unitEnteredThroughTable(program, selector, method), MentionKind::Call);
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
    case NumberSpace::FilledFiles:
    case NumberSpace::DataCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::PropertyRun:
    case NumberSpace::AtomOfComparedText:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::Type:
    case NumberSpace::RegExp:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
