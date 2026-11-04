/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t wordsPerClassPartInArena = 1 + mostContentsOfClass;
static constexpr size_t fewestSlotsOfTypeArenaIndex = 64;
static constexpr size_t fewestRecordsOfTypeArenaBuffer = 64;
static constexpr size_t slotsOfTypeMemoUntilReset = 1 << 13;
static constexpr uint32_t bitsOfOperationInTypeMemo = 8;
static constexpr uint32_t largestEpochOfTypeMemo = (1u << (32 - bitsOfOperationInTypeMemo)) - 1;

// The words of a list of parts are read back as the parts themselves, here and in TypeTable::at(ClassPartsID).
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(ClassPart) == wordsPerClassPartInArena * sizeof(uint32_t));
static_assert(alignof(ClassPart) == alignof(uint32_t));
static_assert(offsetof(ClassPart, writtenContents) == sizeof(uint32_t));

using LayoutsOfNamedObjects = Vector<LayoutID, 64>;
using ClassPartsBeingMade = Vector<ClassPart, 8>;

static bool typeArenaAddsToProgram(const TypeTable& table, ArenaKeeps keeps)
{
    return keeps == ArenaKeeps::InProgram && !table.isFrozen();
}

template<typename Table, typename Record>
static std::optional<uint32_t> findInTypeTableOrAdd(Table& table, bool adds, uint32_t hash, const Record& record)
{
    if (adds)
        return table.findOrAdd(hash, record);
    return table.find(hash, record);
}

// An index of the arena has open addressing. A slot holds the position of a record plus one, and zero while it is free.
static std::optional<uint32_t> findInTypeArenaIndex(const Vector<uint32_t>& index, uint32_t hash, NOESCAPE const Invocable<bool(uint32_t)> auto& matches)
{
    if (index.isEmpty())
        return std::nullopt;
    size_t mask = index.size() - 1;
    for (size_t slot = hash & mask; index[slot]; slot = (slot + 1) & mask) {
        if (matches(index[slot] - 1))
            return index[slot] - 1;
    }
    return std::nullopt;
}

static void placeInTypeArenaIndex(Vector<uint32_t>& index, uint32_t hash, uint32_t position)
{
    size_t mask = index.size() - 1;
    size_t slot = hash & mask;
    while (index[slot])
        slot = (slot + 1) & mask;
    index[slot] = position + 1;
}

// `position` is also the number of records that the index holds so far.
static void addToTypeArenaIndex(Vector<uint32_t>& index, uint32_t hash, uint32_t position, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& hashOfRecordAt)
{
    if ((static_cast<size_t>(position) + 1) * 2 > index.size()) {
        index.fill(0, std::max(fewestSlotsOfTypeArenaIndex, index.size() * 2));
        for (uint32_t earlier = 0; earlier < position; ++earlier)
            placeInTypeArenaIndex(index, hashOfRecordAt(earlier), earlier);
    }
    placeInTypeArenaIndex(index, hash, position);
}

// It goes back to its smallest size, so that what reset() costs does not follow the largest function seen.
static void emptyTypeArenaIndex(Vector<uint32_t>& index)
{
    index.fill(0, std::min(fewestSlotsOfTypeArenaIndex, index.size()));
}

// What was handed out of `live` stays where it is: a buffer that is full is kept as it stands until reset(), and a larger copy takes its place.
template<typename Record>
static void makeRoomInTypeArena(Vector<Record>& live, Vector<Vector<Record>>& outgrown, size_t added)
{
    size_t needed = live.size() + added;
    if (needed <= live.capacity())
        return;
    size_t capacity = std::max(fewestRecordsOfTypeArenaBuffer, needed * 2);
    if (live.isEmpty()) {
        live.reserveCapacity(capacity);
        return;
    }
    Vector<Record> larger;
    larger.reserveInitialCapacity(capacity);
    larger.appendVector(live);
    outgrown.append(std::exchange(live, WTF::move(larger)));
}

static std::span<const uint32_t> wordsOfClassPartsInThread(const Vector<uint32_t>& words, const Vector<uint32_t>& firstWords, uint32_t position)
{
    size_t first = firstWords[position];
    size_t end = position + 1 < firstWords.size() ? firstWords[position + 1] : words.size();
    return words.subspan(first, end - first);
}

static void uniteWrittenContentsOfClassPart(TypeArena& arena, ClassPart& part, const ClassPart& other)
{
    for (unsigned position = 0; position < mostContentsOfClass; ++position) {
        TypeID& contents = part.writtenContents[position];
        TypeID otherContents = other.writtenContents[position];
        contents = contents && otherContents ? arena.unionOf(contents, otherContents) : TypeID();
    }
}

static void makeWrittenContentsCanonical(ClassPart& part)
{
    unsigned numberWritten = mostContentsOfClass;
    while (numberWritten && (!part.writtenContents[numberWritten - 1] || part.writtenContents[numberWritten - 1] == TypeTable::anything))
        part.writtenContents[--numberWritten] = TypeID();
    for (unsigned position = 0; position < numberWritten; ++position) {
        if (!part.writtenContents[position])
            part.writtenContents[position] = TypeTable::anything;
    }
}

// A named object says nothing of its contents. So a class that both lists name keeps what `written` says of them.
static ClassPartsID addClassesOfNamedObjects(TypeArena& arena, ClassPartsID written, ClassPartsID ofNamedObjects)
{
    if (!ofNamedObjects || written == ofNamedObjects)
        return written;
    if (!written)
        return ofNamedObjects;

    std::span<const ClassPart> first = arena.at(written);
    std::span<const ClassPart> second = arena.at(ofNamedObjects);
    ClassPartsBeingMade merged;
    size_t indexInFirst = 0;
    size_t indexInSecond = 0;
    while (indexInFirst < first.size() && indexInSecond < second.size()) {
        if (first[indexInFirst].builtinClass < second[indexInSecond].builtinClass)
            merged.append(first[indexInFirst++]);
        else if (second[indexInSecond].builtinClass < first[indexInFirst].builtinClass)
            merged.append(second[indexInSecond++]);
        else {
            merged.append(first[indexInFirst++]);
            ++indexInSecond;
        }
    }
    merged.append(first.subspan(indexInFirst));
    merged.append(second.subspan(indexInSecond));
    if (merged.size() == first.size())
        return written;
    return arena.internClassParts(merged.span());
}

static void addClassOfNamedObject(ClassPartsBeingMade& classes, BuiltinClassID builtinClass)
{
    auto place = std::ranges::lower_bound(classes, builtinClass, { }, &ClassPart::builtinClass);
    if (place != classes.end() && place->builtinClass == builtinClass)
        return;
    classes.insert(place - classes.begin(), ClassPart { builtinClass, { } });
}

// Whatever escapes merges into the form of the layout that its record names, with whatever layout `new.target` gave it.
static bool namedObjectMayHaveLayoutBelow(const ObjectTable& objects, const ObjectRecord& record)
{
    if (record.has(ObjectTrait::MayHaveLayoutBelow))
        return true;
    return record.kind == ObjectKind::EscapedOfLayout && objects.at(objects.instanceOf(record.layout)).has(ObjectTrait::MayHaveLayoutBelow);
}

// Type::layouts are the shapes of all objects of a type that hasOnlyLayouts(). So an object whose layout `new.target` selects brings every layout written below its own.
// The constructor of a tagged family makes an object of the member that its argument names, and the record names the layout of the maker: it brings the members, with or without the trait.
// anyPossibleLayoutOf() of AOTTypeRelations.cpp and addObjectToDescribedShapes() of AOTTypeShapes.cpp go over the same layouts, and isSubset(type, forLayoutSet(type.layouts)) rests on that: whoever changes one changes all three.
static void appendLayoutsOfNamedObject(const Program* program, const ObjectRecord& record, bool mayHaveLayoutBelow, LayoutsOfNamedObjects& layouts)
{
    RELEASE_ASSERT(record.layout);
    layouts.append(record.layout);
    if (!program)
        return;

    if (mayHaveLayoutBelow) {
        std::span<const LayoutID> below = program->layoutsExpectedBelow(record.layout);
        layouts.append(below);
        // A member has its maker as base, so the members of every family at or below the layout are among these. Below a layout in a ring of bases nothing is listed.
        if (!below.empty())
            return;
    }
    for (const TaggedFamily& family : std::ranges::equal_range(program->taggedFamilies, record.layout, { }, &TaggedFamily::maker)) {
        for (const TaggedLayout& member : family.members)
            layouts.append(member.layout);
    }
}

static LayoutSetID internLayoutsOfNamedObjects(LayoutSets& sets, LayoutsOfNamedObjects& layouts)
{
    if (layouts.size() > 1) {
        std::ranges::sort(layouts);
        removeRepeatedElements(layouts);
    }
    if (layouts.isEmpty())
        return { };
    if (layouts.size() == 1)
        return LayoutSets::of(layouts[0]);
    return sets.ofSorted(layouts.span());
}

static bool typePartsHoldUnknown(const TypeParts& parts)
{
    return parts.anyObject || parts.anyFunction || parts.unnamedLayouts || parts.unnamedClasses;
}

static void makeLiteralsOfTypePartsCanonical(uint16_t& primitives, auto& literals, uint16_t anyBit, uint16_t literalBit)
{
    // A literal bit without a list says that one comes and not which.
    if (primitives & anyBit || (!literals && primitives & literalBit)) {
        literals = { };
        primitives = (primitives & ~literalBit) | anyBit;
        return;
    }
    if (literals)
        primitives |= literalBit;
}

static void makeTypePartsCanonical(TypeParts& parts)
{
    constexpr uint16_t markers = Primitive::Absent | Primitive::Unspecified;
    parts.primitives &= Primitive::Defined;

    if (parts.opaque || parts.environment) {
        bool isMixed = (parts.opaque && parts.environment) || typePartsHoldUnknown(parts) || parts.objects || parts.stringLiterals || parts.symbolLiterals || parts.primitives & ~markers;
        if (!isMixed) {
            TypeParts internal;
            internal.primitives = parts.primitives;
            internal.opaque = parts.opaque;
            internal.environment = parts.environment;
            parts = internal;
            return;
        }
        parts.primitives |= Primitive::All;
        parts.integers = Range::full();
        parts.boundsOfFractions = Range::empty();
        parts.anyObject = true;
        parts.opaque = std::nullopt;
        parts.environment = { };
    }

    if (parts.primitives & Primitive::Integer) {
        parts.integers = parts.integers.intersectWith(Range::full());
        if (parts.integers.isEmpty())
            parts.primitives &= ~Primitive::Integer;
    }
    if (!(parts.primitives & Primitive::Integer))
        parts.integers = Range::empty();

    if (parts.primitives & Primitive::FractionalNumber && !parts.boundsOfFractions.isEmpty()) {
        parts.boundsOfFractions = parts.boundsOfFractions.intersectWith(Range::aroundEveryFraction());
        // No fraction lies between an integer and itself.
        if (parts.boundsOfFractions.minimum >= parts.boundsOfFractions.maximum)
            parts.primitives &= ~Primitive::FractionalNumber;
    }
    if (!(parts.primitives & Primitive::FractionalNumber) || parts.boundsOfFractions.isEmpty() || parts.boundsOfFractions == Range::aroundEveryFraction())
        parts.boundsOfFractions = Range::empty();

    makeLiteralsOfTypePartsCanonical(parts.primitives, parts.stringLiterals, Primitive::String, Primitive::LiteralString);
    makeLiteralsOfTypePartsCanonical(parts.primitives, parts.symbolLiterals, Primitive::Symbol, Primitive::LiteralSymbol);

    // `unknown` stands for what has escaped. A named object need not have, so nothing swallows it.
    if (parts.anyObject) {
        parts.anyFunction = true;
        parts.unnamedLayouts = { };
        parts.unnamedClasses = { };
    }
    if (!typePartsHoldUnknown(parts))
        parts.seeds = { };
}

static constexpr bool wordOfTypeMemoLiesInThread(uint32_t bits)
{
    return liesInThread(TypeID::fromBits(bits));
}

static constexpr uint32_t stampInTypeMemo(uint32_t operation, uint32_t epoch)
{
    return epoch << bitsOfOperationInTypeMemo | operation;
}

static size_t slotInTypeMemo(uint32_t operation, uint32_t first, uint32_t second, size_t numberOfSlots)
{
    uint64_t mixed = (static_cast<uint64_t>(first) << 32 | second) * 0x9e3779b97f4a7c15ull;
    mixed = ((mixed ^ (mixed >> 32)) + operation) * 0xbf58476d1ce4e5b9ull;
    return static_cast<size_t>(mixed >> 32) & (numberOfSlots - 1);
}

TypeArena::TypeArena(TypeTable& table, ArenaKeeps keeps)
    : m_table(table)
    , m_keeps(keeps)
    , m_layoutSets(table.m_layoutSets, keeps)
    , m_objectSets(table.m_objectSets, keeps, this)
    , m_stringSets(table.m_stringSets, keeps)
    , m_keySets(table.m_keySets, keeps)
{
    if (keeps != ArenaKeeps::InProgram)
        return;
    Locker locker { table.m_memosAtRestLock };
    if (!table.m_keepsMemosAtRest)
        return;
    if (!table.m_memosAtRest.isEmpty()) {
        TypeTable::MemosOfArena memos = table.m_memosAtRest.takeLast();
        m_lastingMemo = WTF::move(memos.types);
        m_layoutSets.untyped().setLastingMemo(WTF::move(memos.layoutSets));
        m_objectSets.untyped().setLastingMemo(WTF::move(memos.objectSets));
        m_stringSets.untyped().setLastingMemo(WTF::move(memos.stringSets));
        m_keySets.untyped().setLastingMemo(WTF::move(memos.keySets));
        m_unionsOfMany = WTF::move(memos.unionsOfMany);
    }
    m_lastingMemo.keepEverything();
    m_layoutSets.untyped().lastingMemo().keepEverything();
    m_objectSets.untyped().lastingMemo().keepEverything();
    m_stringSets.untyped().lastingMemo().keepEverything();
    m_keySets.untyped().lastingMemo().keepEverything();
    m_unionsOfMany.keepEverything();
}

TypeArena::~TypeArena()
{
    if (m_keeps != ArenaKeeps::InProgram)
        return;
    Locker locker { m_table.m_memosAtRestLock };
    if (!m_table.m_keepsMemosAtRest)
        return;
    m_table.m_memosAtRest.append(TypeTable::MemosOfArena { WTF::move(m_lastingMemo), m_layoutSets.untyped().takeLastingMemo(), m_objectSets.untyped().takeLastingMemo(), m_stringSets.untyped().takeLastingMemo(), m_keySets.untyped().takeLastingMemo(), WTF::move(m_unionsOfMany) });
}

void TypeArena::reset()
{
    m_layoutSets.reset();
    m_objectSets.reset();
    m_stringSets.reset();
    m_keySets.reset();

    m_types.shrink(0);
    emptyTypeArenaIndex(m_typeIndex);
    m_anythingOfSeed.fill(TypeID());

    m_classPartWords.shrink(0);
    m_firstWordOfClassParts.shrink(0);
    emptyTypeArenaIndex(m_classPartsIndex);
    m_outgrownClassPartWords.shrink(0);

    m_propertyKeys.shrink(0);
    emptyTypeArenaIndex(m_propertyKeyIndex);
    m_outgrownPropertyKeys.shrink(0);

    m_bytesOfTexts.shrink(0);
    m_roomBehindTexts.clear();
    m_madeTexts.shrink(0);
    m_joinedTexts.clear();
    emptyTypeArenaIndex(m_textIndex);
    m_outgrownBytesOfTexts.shrink(0);

    m_answersUntilReset.shrink(0);
    m_wordsOfAnswersUntilReset.shrink(0);
    emptyTypeArenaIndex(m_indexOfAnswersUntilReset);

    if (m_epoch == largestEpochOfTypeMemo) {
        m_memoUntilReset.fill(MemoEntry { });
        m_epoch = 0;
    }
    ++m_epoch;
    forgetUnionsOfMany();
}

void TypeArena::forgetResults()
{
    m_lastingMemo.clear();
    m_memoUntilReset.fill(MemoEntry { });
    forgetUnionsOfMany();
}

void TypeArena::forgetResultsOfTypesAndSets()
{
    forgetResults();
    m_layoutSets.untyped().lastingMemo().clear();
    m_objectSets.untyped().lastingMemo().clear();
    m_stringSets.untyped().lastingMemo().clear();
    m_keySets.untyped().lastingMemo().clear();
}

void TypeArena::forgetUnionsOfMany()
{
    m_unionsOfMany.clear();
}

size_t TypeArena::sizeOfMemosInBytes() const
{
    size_t size = m_lastingMemo.sizeInBytes() + m_unionsOfMany.sizeInBytes() + m_memoUntilReset.capacity() * sizeof(MemoEntry);
    for (const IDSetArena* sets : { &m_layoutSets.untyped(), &m_objectSets.untyped(), &m_stringSets.untyped(), &m_keySets.untyped() })
        size += sets->sizeOfMemosInBytes() + sets->sizeOfScratchInBytes();
    return size;
}

std::span<const ClassPart> TypeArena::at(ClassPartsID id) const
{
    if (!id)
        return { };
    if (!liesInThread(id))
        return m_table.at(id);
    std::span<const uint32_t> words = wordsOfClassPartsInThread(m_classPartWords, m_firstWordOfClassParts, positionOf(id));
    return unsafeMakeSpan(reinterpret_cast<const ClassPart*>(words.data()), words.size() / wordsPerClassPartInArena);
}

const PropertyKey& TypeArena::at(PropertyKeyID key) const
{
    if (liesInThread(key))
        return m_propertyKeys[positionOf(key)];
    return m_table.at(key);
}

ObjectSetSummary TypeArena::summaryOf(ObjectSetID set) const
{
    if (!set)
        return { };

    if (!holdsOneMember(set)) {
        std::span<const uint32_t> words = at(set).untyped().attachedWords();
        RELEASE_ASSERT(words.size() == wordsOfObjectSetSummary);
        return { LayoutSetID::fromBits(words[0]), ClassPartsID::fromBits(words[1]), OptionSet<ObjectTrait>::fromRaw(static_cast<uint16_t>(words[2])) };
    }

    const ObjectRecord& record = at(ObjectID(positionOf(set)));
    ObjectSetSummary summary;
    summary.traits = record.traits;
    if (record.has(ObjectTrait::HasLayout)) {
        LayoutsOfNamedObjects layouts;
        appendLayoutsOfNamedObject(program(), record, namedObjectMayHaveLayoutBelow(objects(), record), layouts);
        // Interning a set changes no answer of the arena.
        summary.layouts = internLayoutsOfNamedObjects(const_cast<TypeArena&>(*this).m_layoutSets, layouts);
    }
    if (record.has(ObjectTrait::HasClass))
        summary.classes = partsOf(record.builtinClass);
    return summary;
}

void TypeArena::annotate(const IDSetView& members, std::optional<IDSetOperation> operation, std::span<const uint32_t> operands, std::span<uint32_t> attachedWords)
{
    RELEASE_ASSERT(attachedWords.size() == wordsOfObjectSetSummary);

    ObjectSetSummary summary;
    if (operation == IDSetOperation::Union) {
        Vector<LayoutSetID, 16> layouts;
        for (uint32_t operand : operands) {
            ObjectSetSummary ofOperand = summaryOf(ObjectSetID::fromBits(operand));
            if (ofOperand.layouts && (layouts.isEmpty() || layouts.last() != ofOperand.layouts))
                layouts.append(ofOperand.layouts);
            summary.classes = addClassesOfNamedObjects(*this, summary.classes, ofOperand.classes);
            summary.traits.add(ofOperand.traits);
        }
        summary.layouts = m_layoutSets.unionOf(layouts.span());
    } else {
        LayoutsOfNamedObjects layouts;
        ClassPartsBeingMade classes;
        LayoutID previousLayout;
        bool previousMayHaveLayoutBelow = false;
        members.forEach([&](uint32_t member) {
            const ObjectRecord& record = at(ObjectID(member));
            summary.traits.add(record.traits);
            if (record.has(ObjectTrait::HasClass))
                addClassOfNamedObject(classes, record.builtinClass);
            if (!record.has(ObjectTrait::HasLayout))
                return;
            bool mayHaveLayoutBelow = namedObjectMayHaveLayoutBelow(objects(), record);
            if (record.layout == previousLayout && mayHaveLayoutBelow == previousMayHaveLayoutBelow)
                return;
            appendLayoutsOfNamedObject(program(), record, mayHaveLayoutBelow, layouts);
            previousLayout = record.layout;
            previousMayHaveLayoutBelow = mayHaveLayoutBelow;
        });
        summary.layouts = internLayoutsOfNamedObjects(m_layoutSets, layouts);
        summary.classes = internClassParts(classes.span());
    }

    attachedWords[0] = summary.layouts.bits();
    attachedWords[1] = summary.classes.bits();
    attachedWords[2] = summary.traits.toRaw();
}

ClassPartsID TypeArena::internClassParts(std::span<const ClassPart> parts)
{
    // A copy, since `parts` may lie in this arena and uniting contents may add to it.
    ClassPartsBeingMade canonical(parts);
    auto isOutOfOrder = [](const ClassPart& a, const ClassPart& b) {
        return a.builtinClass >= b.builtinClass;
    };
    if (std::ranges::adjacent_find(canonical, isOutOfOrder) != canonical.end()) {
        AOT::stableSort(canonical, { }, &ClassPart::builtinClass);
        size_t size = 1;
        for (size_t index = 1; index < canonical.size(); ++index) {
            if (canonical[index].builtinClass == canonical[size - 1].builtinClass)
                uniteWrittenContentsOfClassPart(*this, canonical[size - 1], canonical[index]);
            else
                canonical[size++] = canonical[index];
        }
        canonical.shrink(size);
    }
    for (ClassPart& part : canonical) {
        ASSERT(part.builtinClass);
        makeWrittenContentsCanonical(part);
    }

    if (canonical.isEmpty())
        return { };
    if (canonical.size() == 1 && !canonical[0].hasWrittenContents())
        return partsOf(canonical[0].builtinClass);

    Vector<uint32_t, 8 * wordsPerClassPartInArena> words;
    for (const ClassPart& part : canonical) {
        words.append(part.builtinClass.index());
        for (TypeID contents : part.writtenContents)
            words.append(contents.bits());
    }

    uint32_t hash = hashOf(words.span());
    if (std::optional<uint32_t> position = findInTypeTableOrAdd(m_table.m_classParts, typeArenaAddsToProgram(m_table, m_keeps), hash, words.span()))
        return idAt<ClassPartsID>(IDHome::Program, *position);
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset);

    auto wordsAt = [&](uint32_t position) {
        return wordsOfClassPartsInThread(m_classPartWords, m_firstWordOfClassParts, position);
    };
    std::optional<uint32_t> known = findInTypeArenaIndex(m_classPartsIndex, hash, [&](uint32_t candidate) {
        return std::ranges::equal(wordsAt(candidate), words.span());
    });
    if (known)
        return idAt<ClassPartsID>(IDHome::Thread, *known);

    uint32_t position = static_cast<uint32_t>(m_firstWordOfClassParts.size());
    makeRoomInTypeArena(m_classPartWords, m_outgrownClassPartWords, words.size());
    m_firstWordOfClassParts.append(static_cast<uint32_t>(m_classPartWords.size()));
    m_classPartWords.append(words.span());
    addToTypeArenaIndex(m_classPartsIndex, hash, position, [&](uint32_t earlier) {
        return hashOf(wordsAt(earlier));
    });
    return idAt<ClassPartsID>(IDHome::Thread, position);
}

TypeID TypeArena::intern(const TypeParts& parts)
{
    TypeParts canonical = parts;
    makeTypePartsCanonical(canonical);
    return internCanonical(canonical);
}

// While types are united, one set stands for every set of its kind that has members: uniting two types then unites no sets and still sees which side has some.
template<typename SetID>
struct SetsOfTypesBeingUnited {
    void take(SetID& set, SetID earlierState)
    {
        if (!set)
            return;
        sets.append(set);
        earlierStates.append(earlierState);
        set = sets.first();
    }

    Vector<SetID, 16> sets;
    Vector<SetID, 16> earlierStates;
};

TypeID TypeArena::unionOf(std::span<const TypeID> types, std::span<const TypeID> earlierStates)
{
    RELEASE_ASSERT(earlierStates.empty() || earlierStates.size() == types.size());
    // By type. The earlier state of a type stands in the lower half.
    Vector<uint64_t, 16> pairs;
    for (size_t index = 0; index < types.size(); ++index) {
        ASSERT(types[index]);
        if (isNeverType(types[index]))
            continue;
        TypeID earlierState = earlierStates.empty() ? TypeID() : earlierStates[index];
        // No set of it has chunks.
        if (earlierState && holdsOnlyPrimitives(earlierState))
            earlierState = { };
        pairs.append(static_cast<uint64_t>(types[index].bits()) << 32 | earlierState.bits());
    }
    // Where a type comes several times the greatest number stays, which is that of the null id wherever one of them says nothing: the type may be the very one that holds the earlier states of the others.
    static_assert(TypeID().bits() == UINT32_MAX);
    std::ranges::sort(pairs, std::greater<>());
    Vector<TypeID, 16> distinct;
    Vector<TypeID, 16> earlierStatesOfDistinct;
    bool someTypeHasEarlierState = false;
    for (uint64_t pair : pairs) {
        TypeID type = TypeID::fromBits(static_cast<uint32_t>(pair >> 32));
        if (!distinct.isEmpty() && distinct.last() == type)
            continue;
        distinct.append(type);
        earlierStatesOfDistinct.append(TypeID::fromBits(static_cast<uint32_t>(pair)));
        someTypeHasEarlierState |= !!earlierStatesOfDistinct.last();
    }
    if (distinct.isEmpty())
        return TypeTable::never;
    if (distinct.size() == 1)
        return distinct[0];
    bool isPair = distinct.size() == 2;
    if (isPair && !someTypeHasEarlierState)
        return unionOf(distinct[0], distinct[1]);
    // A copy: it holds what its source held, and the source holds that and more.
    if (isPair && earlierStatesOfDistinct[0] == distinct[1])
        return distinct[0];
    if (isPair && earlierStatesOfDistinct[1] == distinct[0])
        return distinct[1];

    // An earlier state changes no answer, so no memo asks about it.
    static_assert(sizeof(TypeID) == sizeof(uint32_t));
    std::span<const uint32_t> operands = unsafeMakeSpan(reinterpret_cast<const uint32_t*>(distinct.span().data()), distinct.size());
    if (std::optional<uint32_t> known = isPair ? remembered(Operation::Union, operands[1], operands[0]) : m_unionsOfMany.find(operands))
        return TypeID::fromBits(*known);

    SetsOfTypesBeingUnited<StringSetID> stringLiterals;
    SetsOfTypesBeingUnited<KeySetID> symbolLiterals;
    SetsOfTypesBeingUnited<ObjectSetID> objects;
    SetsOfTypesBeingUnited<LayoutSetID> unnamedLayouts;
    Type united;
    for (size_t index = 0; index < distinct.size(); ++index) {
        TypeID id = distinct[index];
        Type type = at(id);
        Type earlierState = earlierStatesOfDistinct[index] ? at(earlierStatesOfDistinct[index]) : Type();
        stringLiterals.take(type.stringLiterals, earlierState.stringLiterals);
        symbolLiterals.take(type.symbolLiterals, earlierState.symbolLiterals);
        objects.take(type.objects, earlierState.objects);
        unnamedLayouts.take(type.unnamedLayouts, earlierState.unnamedLayouts);
        if (id == distinct.first()) {
            united = type;
            continue;
        }
        // What at() of the interned union would answer, as far as uniteTypes() reads it.
        TypeParts parts = uniteTypes(*this, united, type);
        makeTypePartsCanonical(parts);
        static_cast<TypeParts&>(united) = parts;
    }

    // A set that something has swallowed on the way stays swallowed.
    if (united.stringLiterals)
        united.stringLiterals = m_stringSets.unionOf(stringLiterals.sets.span(), stringLiterals.earlierStates.span());
    if (united.symbolLiterals)
        united.symbolLiterals = m_keySets.unionOf(symbolLiterals.sets.span(), symbolLiterals.earlierStates.span());
    if (united.objects)
        united.objects = m_objectSets.unionOf(objects.sets.span(), objects.earlierStates.span());
    if (united.unnamedLayouts)
        united.unnamedLayouts = m_layoutSets.unionOf(unnamedLayouts.sets.span(), unnamedLayouts.earlierStates.span());
    TypeID result = intern(united);

    if (isPair)
        remember(Operation::Union, operands[1], operands[0], result.bits());
    else
        m_unionsOfMany.add(operands, result.bits());
    return result;
}

void TypeArena::forgetEarlierStates()
{
    m_layoutSets.untyped().forgetEarlierStates();
    m_objectSets.untyped().forgetEarlierStates();
    m_stringSets.untyped().forgetEarlierStates();
    m_keySets.untyped().forgetEarlierStates();
}

// The id of a type that needs no record.
static std::optional<TypeID> tryMakeTypeOfCanonicalParts(const TypeParts& parts)
{
    bool holdsPrimitivesAlone = !typePartsHoldUnknown(parts) && !parts.objects && !parts.opaque && !parts.environment;
    if (holdsPrimitivesAlone && parts.boundsOfFractions.isEmpty())
        return tryMakeTypeOfPrimitives(parts.primitives, parts.integers);
    return std::nullopt;
}

TypeID TypeArena::internCanonical(const TypeParts& parts)
{
    if (std::optional<TypeID> id = tryMakeTypeOfCanonicalParts(parts))
        return *id;

    ASSERT(m_keeps == ArenaKeeps::UntilReset || !(liesInThread(parts.stringLiterals) || liesInThread(parts.symbolLiterals) || liesInThread(parts.objects) || liesInThread(parts.unnamedLayouts) || liesInThread(parts.unnamedClasses)));
    uint32_t hash = hashOf(parts);

    // Two types are compared by their parts alone. So what follows from the parts, which may be a long list of layouts, is worked out only for a type that is new.
    Type wanted;
    static_cast<TypeParts&>(wanted) = parts;
    if (std::optional<uint32_t> position = m_table.m_types.find(hash, wanted))
        return idAt<TypeID>(IDHome::Program, *position);
    if (typeArenaAddsToProgram(m_table, m_keeps))
        return idAt<TypeID>(IDHome::Program, m_table.m_types.addAfterMiss(hash, complete(parts)));
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset);

    std::optional<uint32_t> known = findInTypeArenaIndex(m_typeIndex, hash, [&](uint32_t candidate) {
        return static_cast<const TypeParts&>(m_types[candidate]) == parts;
    });
    if (known)
        return idAt<TypeID>(IDHome::Thread, *known);

    Type type = complete(parts);
    uint32_t position = static_cast<uint32_t>(m_types.size());
    m_types.append(type);
    addToTypeArenaIndex(m_typeIndex, hash, position, [&](uint32_t earlier) {
        return hashOf(static_cast<const TypeParts&>(m_types[earlier]));
    });
    return idAt<TypeID>(IDHome::Thread, position);
}

Type TypeArena::complete(const TypeParts& canonical) const
{
    Type type;
    static_cast<TypeParts&>(type) = canonical;
    type.layouts = canonical.unnamedLayouts;
    type.classes = canonical.unnamedClasses;
    if (!canonical.objects)
        return type;

    // Interning a set or a list of parts changes no answer of the arena.
    TypeArena& arena = const_cast<TypeArena&>(*this);
    ObjectSetSummary summary = summaryOf(canonical.objects);
    type.layouts = arena.m_layoutSets.unionOf(canonical.unnamedLayouts, summary.layouts);
    type.classes = addClassesOfNamedObjects(arena, canonical.unnamedClasses, summary.classes);
    type.traitsOfNamed = summary.traits;
    return type;
}

ArenasOfWorkers::ArenasOfWorkers(TypeTable& table, const ParallelLoops& loops)
    : m_table(table)
    , m_loops(loops)
    , m_arenas(loops.width())
{
}

// Each has memos of its own to free. One thread is through a few megabytes before the others have woken up.
ArenasOfWorkers::~ArenasOfWorkers()
{
    constexpr size_t fewestBytesFreedOnEveryThread = 16 * MB;
    m_arenas.removeAllMatching([](const std::unique_ptr<TypeArena>& arena) {
        return !arena;
    });
    size_t sizeOfMemos = 0;
    for (const std::unique_ptr<TypeArena>& arena : m_arenas)
        sizeOfMemos += arena->sizeOfMemosInBytes();
    if (sizeOfMemos < fewestBytesFreedOnEveryThread)
        return;
    forEachIndexInParallel(m_loops, static_cast<unsigned>(m_arenas.size()), [&](unsigned index) {
        m_arenas[index] = nullptr;
    });
}

TypeArena& ArenasOfWorkers::of(unsigned worker)
{
    std::unique_ptr<TypeArena>& arena = m_arenas[worker];
    if (!arena)
        arena = makeUniqueWithoutFastMallocCheck<TypeArena>(m_table, ArenaKeeps::InProgram);
    return *arena;
}

// Step by step what intern() and internCanonical() do for an arena that adds to the program.
void ArenasOfWorkers::internInOrder(std::span<TypeID> ids, NOESCAPE const ScopedLambda<TypeParts(uint32_t index, TypeArena&)>& partsOf)
{
    RELEASE_ASSERT(!m_table.isFrozen());
    if (m_candidates.span().size() < ids.size())
        m_candidates = MallocSpan<Type>::malloc(ids.size() * sizeof(Type));
    std::span<Type> candidates = m_candidates.mutableSpan().first(ids.size());

    m_table.m_types.findOrAddInOrder(m_loops, candidates, [&](uint32_t index, unsigned worker) -> std::optional<uint32_t> {
        TypeParts parts = partsOf(index, of(worker));
        makeTypePartsCanonical(parts);
        if (std::optional<TypeID> id = tryMakeTypeOfCanonicalParts(parts)) {
            ids[index] = *id;
            return std::nullopt;
        }
        static_cast<TypeParts&>(*new (NotNull, &candidates[index]) Type) = parts;
        return hashOf(parts);
    }, [&](uint32_t index, uint32_t position, bool isNew, unsigned worker) {
        if (isNew)
            candidates[index] = of(worker).complete(candidates[index]);
        ids[index] = idAt<TypeID>(IDHome::Program, position);
    });
}

void ArenasOfWorkers::internInOrder(std::span<PropertyKeyID> ids, NOESCAPE const ScopedLambda<PropertyKey(uint32_t index)>& keyOf)
{
    RELEASE_ASSERT(!m_table.isFrozen());
    auto candidates = MallocSpan<PropertyKey>::malloc(ids.size() * sizeof(PropertyKey));
    m_table.m_propertyKeys.findOrAddInOrder(m_loops, candidates.mutableSpan(), [&](uint32_t index, unsigned) -> std::optional<uint32_t> {
        return hashOf(*new (NotNull, &candidates[index]) PropertyKey(keyOf(index)));
    }, [&](uint32_t index, uint32_t position, bool, unsigned) {
        ids[index] = idAt<PropertyKeyID>(IDHome::Value, position);
    });
}

std::optional<uint32_t> TypeArena::remembered(Operation operation, uint32_t first, uint32_t second)
{
    uint32_t operationWord = static_cast<uint32_t>(operation);
    if (!wordOfTypeMemoLiesInThread(first) && !wordOfTypeMemoLiesInThread(second)) {
        if (std::optional<uint32_t> result = m_lastingMemo.find(operationWord, first, second))
            return result;
    }
    if (m_memoUntilReset.isEmpty())
        return std::nullopt;
    const MemoEntry& entry = m_memoUntilReset[slotInTypeMemo(operationWord, first, second, m_memoUntilReset.size())];
    if (entry.first != first || entry.second != second || entry.operationAndEpoch != stampInTypeMemo(operationWord, m_epoch))
        return std::nullopt;
    return entry.result;
}

void TypeArena::remember(Operation operation, uint32_t first, uint32_t second, uint32_t result)
{
    uint32_t operationWord = static_cast<uint32_t>(operation);
    if (!wordOfTypeMemoLiesInThread(first) && !wordOfTypeMemoLiesInThread(second) && !wordOfTypeMemoLiesInThread(result)) {
        m_lastingMemo.add(operationWord, first, second, result);
        return;
    }
    if (m_memoUntilReset.isEmpty())
        m_memoUntilReset.fill(MemoEntry { }, slotsOfTypeMemoUntilReset);
    m_memoUntilReset[slotInTypeMemo(operationWord, first, second, m_memoUntilReset.size())] = { first, second, result, stampInTypeMemo(operationWord, m_epoch) };
}

static constexpr uint8_t firstQuestionFromOutsideInTypeMemo = 128;

std::optional<uint32_t> TypeArena::rememberedAnswer(uint8_t question, uint32_t first, uint32_t second)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset && question < firstQuestionFromOutsideInTypeMemo);
    return remembered(static_cast<Operation>(firstQuestionFromOutsideInTypeMemo + question), first, second);
}

void TypeArena::rememberAnswer(uint8_t question, uint32_t first, uint32_t second, uint32_t answer)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset && question < firstQuestionFromOutsideInTypeMemo);
    remember(static_cast<Operation>(firstQuestionFromOutsideInTypeMemo + question), first, second, answer);
}

namespace {

// A type without an id of an arena.
struct PartsOfTypeKeptForBuild {
    static constexpr uint32_t noList = UINT32_MAX;

    size_t sizeInBytes() const
    {
        size_t size = sizeof(PartsOfTypeKeptForBuild) + unnamedClasses.capacity() * sizeof(ClassPart) + writtenContents.capacity() * sizeof(WrittenContents);
        for (const WrittenContents& contents : writtenContents)
            size += contents.type->sizeInBytes();
        return size;
    }

    // Without the sets that lay in the thread.
    TypeParts parts;
    // The numbers that SharedAnswers has for the members of those.
    uint32_t stringLiterals { noList };
    uint32_t symbolLiterals { noList };
    uint32_t objects { noList };
    uint32_t unnamedLayouts { noList };
    // Where these lay in the thread. Written contents that did are null here.
    Vector<ClassPart> unnamedClasses;
    struct WrittenContents {
        uint32_t classPart { 0 };
        uint32_t position { 0 };
        std::unique_ptr<PartsOfTypeKeptForBuild> type;
    };
    Vector<WrittenContents> writtenContents;
};

class RoundKeptForBuild final : public SharedAnswers::Answer {
public:
    size_t sizeInBytes() const final
    {
        size_t size = sizeof(RoundKeptForBuild);
        for (const std::unique_ptr<PartsOfTypeKeptForBuild>& type : types)
            size += type ? type->sizeInBytes() : 0;
        return size;
    }

    TypeArena::WordsOfRound words { };
    // In place of a word that was a type of the thread.
    std::array<std::unique_ptr<PartsOfTypeKeptForBuild>, TypeArena::mostWordsOfRound> types;
};

} // anonymous namespace

template<typename SetID>
static uint32_t numberOfMembersInSharedAnswers(TypeArena& arena, SharedAnswers& answers, SetID set)
{
    auto view = arena.at(set);
    Vector<uint32_t, 64> members;
    members.reserveInitialCapacity(view.size());
    view.forEach([&](auto member) {
        members.append(member.index());
    });
    return answers.internList(members.span());
}

template<typename SetID>
static uint32_t takeSetOutOfThread(TypeArena& arena, SharedAnswers& answers, SetID& set)
{
    if (!liesInThread(set))
        return PartsOfTypeKeptForBuild::noList;
    return numberOfMembersInSharedAnswers(arena, answers, std::exchange(set, { }));
}

static std::unique_ptr<PartsOfTypeKeptForBuild> partsOfTypeInThread(TypeArena& arena, SharedAnswers& answers, TypeID type)
{
    auto kept = makeUniqueWithoutFastMallocCheck<PartsOfTypeKeptForBuild>();
    kept->parts = arena.at(type);
    kept->stringLiterals = takeSetOutOfThread(arena, answers, kept->parts.stringLiterals);
    kept->symbolLiterals = takeSetOutOfThread(arena, answers, kept->parts.symbolLiterals);
    kept->objects = takeSetOutOfThread(arena, answers, kept->parts.objects);
    kept->unnamedLayouts = takeSetOutOfThread(arena, answers, kept->parts.unnamedLayouts);
    if (!liesInThread(kept->parts.unnamedClasses))
        return kept;

    kept->unnamedClasses = Vector<ClassPart>(arena.at(std::exchange(kept->parts.unnamedClasses, { })));
    for (uint32_t classPart = 0; classPart < kept->unnamedClasses.size(); ++classPart) {
        for (uint32_t position = 0; position < mostContentsOfClass; ++position) {
            TypeID& written = kept->unnamedClasses[classPart].writtenContents[position];
            if (liesInThread(written))
                kept->writtenContents.append({ classPart, position, partsOfTypeInThread(arena, answers, std::exchange(written, { })) });
        }
    }
    return kept;
}

// Equal sets have one id in an arena, and so have equal types: what is made here is what whoever makes the round in this arena would have got.
template<typename MemberID, typename SetID>
static SetID setOfMembersInSharedAnswers(SetsOf<MemberID, SetID>& sets, SharedAnswers& answers, uint32_t number)
{
    std::span<const uint32_t> members = answers.listAt(number);
    RELEASE_ASSERT(members.size() > 1);
    static_assert(sizeof(MemberID) == sizeof(uint32_t));
    return sets.ofSorted(unsafeMakeSpan(reinterpret_cast<const MemberID*>(members.data()), members.size()));
}

static TypeID typeOfPartsKeptForBuild(TypeArena& arena, SharedAnswers& answers, const PartsOfTypeKeptForBuild& kept)
{
    TypeParts parts = kept.parts;
    if (kept.stringLiterals != PartsOfTypeKeptForBuild::noList)
        parts.stringLiterals = setOfMembersInSharedAnswers(arena.stringSets(), answers, kept.stringLiterals);
    if (kept.symbolLiterals != PartsOfTypeKeptForBuild::noList)
        parts.symbolLiterals = setOfMembersInSharedAnswers(arena.keySets(), answers, kept.symbolLiterals);
    if (kept.objects != PartsOfTypeKeptForBuild::noList)
        parts.objects = setOfMembersInSharedAnswers(arena.objectSets(), answers, kept.objects);
    if (kept.unnamedLayouts != PartsOfTypeKeptForBuild::noList)
        parts.unnamedLayouts = setOfMembersInSharedAnswers(arena.layoutSets(), answers, kept.unnamedLayouts);
    if (!kept.unnamedClasses.isEmpty()) {
        Vector<ClassPart, 8> unnamedClasses(kept.unnamedClasses.span());
        for (const PartsOfTypeKeptForBuild::WrittenContents& contents : kept.writtenContents)
            unnamedClasses[contents.classPart].writtenContents[contents.position] = typeOfPartsKeptForBuild(arena, answers, *contents.type);
        parts.unnamedClasses = arena.internClassParts(unnamedClasses.span());
    }
    return arena.intern(parts);
}

// A round over fewer members costs less than finding what is kept of it.
static constexpr uint32_t fewestMembersOfSetWhoseRoundsAreKeptForBuild = 8;
// No set has an id of IDHome::SmallRange, and the number of a list is less than 1 << 30: walk/AOTMemberPartition.cpp has the same word for a set of layouts.
static constexpr uint32_t homeOfListInWordOfSet = static_cast<uint32_t>(IDHome::SmallRange);

static SharedAnswers* answersThatKeepRounds(TypeArena& arena)
{
    return arena.keeps() != ArenaKeeps::UntilReset ? nullptr : arena.table().sharedAnswers();
}

template<typename SetID>
static TypeArena::SetOfRound setOfRoundIn(TypeArena& arena, SetID set, AnswerKeptInArena numberOfList, AnswerKeptInArena setOfList)
{
    SharedAnswers* answers = answersThatKeepRounds(arena);
    if (!answers || !set || holdsOneMember(set) || arena.at(set).size() < fewestMembersOfSetWhoseRoundsAreKeptForBuild)
        return { set.bits(), false };
    if (!liesInThread(set))
        return { set.bits(), true };
    std::optional<uint32_t> number = arena.rememberedAnswer(static_cast<uint8_t>(numberOfList), set.bits(), 0);
    if (!number) {
        number = numberOfMembersInSharedAnswers(arena, *answers, set);
        arena.rememberAnswer(static_cast<uint8_t>(numberOfList), set.bits(), 0, *number);
        arena.rememberAnswer(static_cast<uint8_t>(setOfList), *number, 0, set.bits());
    }
    return { homeOfListInWordOfSet << idHomeShift | *number, true };
}

TypeArena::SetOfRound TypeArena::setOfRound(LayoutSetID set)
{
    return setOfRoundIn(*this, set, AnswerKeptInArena::NumberOfLayoutList, AnswerKeptInArena::LayoutSetOfList);
}

TypeArena::SetOfRound TypeArena::setOfRound(ObjectSetID set)
{
    return setOfRoundIn(*this, set, AnswerKeptInArena::NumberOfObjectList, AnswerKeptInArena::ObjectSetOfList);
}

TypeArena::SetOfRound TypeArena::numberOfRound(uint32_t number)
{
    RELEASE_ASSERT(!wordOfTypeMemoLiesInThread(number));
    return { number, !!answersThatKeepRounds(*this) };
}

std::optional<TypeArena::WordsOfRound> TypeArena::rememberedRound(uint8_t question, size_t numberOfWords, SetOfRound set, uint32_t second)
{
    RELEASE_ASSERT(numberOfWords && numberOfWords <= mostWordsOfRound);
    WordsOfRound words { };
    size_t numberOfWordsKept = 0;
    for (; numberOfWordsKept < numberOfWords; ++numberOfWordsKept) {
        std::optional<uint32_t> kept = rememberedAnswer(static_cast<uint8_t>(question + numberOfWordsKept), set.word, second);
        if (!kept)
            break;
        words[numberOfWordsKept] = *kept;
    }
    if (numberOfWordsKept == numberOfWords)
        return words;
    if (!set.isKeptForBuild || wordOfTypeMemoLiesInThread(second))
        return std::nullopt;

    SharedAnswers& answers = *m_table.sharedAnswers();
    const SharedAnswers::Answer* found = answers.find({ SharedQuestion::RoundOverSet, question, set.word, second });
    if (!found)
        return std::nullopt;
    const auto& round = static_cast<const RoundKeptForBuild&>(*found);
    for (size_t index = 0; index < numberOfWords; ++index) {
        words[index] = round.types[index] ? typeOfPartsKeptForBuild(*this, answers, *round.types[index]).bits() : round.words[index];
        rememberAnswer(static_cast<uint8_t>(question + index), set.word, second, words[index]);
    }
    return words;
}

void TypeArena::rememberRound(uint8_t question, size_t numberOfWords, uint8_t typesAmongWords, SetOfRound set, uint32_t second, const WordsOfRound& words)
{
    RELEASE_ASSERT(numberOfWords && numberOfWords <= mostWordsOfRound);
    for (size_t index = 0; index < numberOfWords; ++index)
        rememberAnswer(static_cast<uint8_t>(question + index), set.word, second, words[index]);
    if (!set.isKeptForBuild || wordOfTypeMemoLiesInThread(second))
        return;

    SharedAnswers& answers = *m_table.sharedAnswers();
    auto round = makeUniqueWithoutFastMallocCheck<RoundKeptForBuild>();
    round->words = words;
    for (size_t index = 0; index < numberOfWords; ++index) {
        if (typesAmongWords >> index & 1 && wordOfTypeMemoLiesInThread(words[index]))
            round->types[index] = partsOfTypeInThread(*this, answers, TypeID::fromBits(words[index]));
    }
    answers.add({ SharedQuestion::RoundOverSet, question, set.word, second }, WTF::move(round));
}

static uint32_t hashOfQuestionKeptUntilReset(std::span<const uint32_t> question)
{
    uint64_t hash = 0xcbf29ce484222325ull ^ question.size();
    for (uint32_t word : question) {
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    return static_cast<uint32_t>(hash >> 32);
}

const SharedAnswers::Answer* TypeArena::answerKeptUntilReset(std::span<const uint32_t> question) const
{
    uint32_t hash = hashOfQuestionKeptUntilReset(question);
    std::optional<uint32_t> known = findInTypeArenaIndex(m_indexOfAnswersUntilReset, hash, [&](uint32_t candidate) {
        const AnswerUntilReset& kept = m_answersUntilReset[candidate];
        return kept.hash == hash && std::ranges::equal(m_wordsOfAnswersUntilReset.subspan(kept.firstWord, kept.numberOfWords), question);
    });
    return known ? m_answersUntilReset[*known].answer.get() : nullptr;
}

const SharedAnswers::Answer& TypeArena::keepAnswerUntilReset(std::span<const uint32_t> question, std::unique_ptr<SharedAnswers::Answer> answer)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset && answer);
    ASSERT(!answerKeptUntilReset(question));
    uint32_t hash = hashOfQuestionKeptUntilReset(question);
    uint32_t position = m_answersUntilReset.size();
    m_answersUntilReset.append(AnswerUntilReset { hash, static_cast<uint32_t>(m_wordsOfAnswersUntilReset.size()), static_cast<uint32_t>(question.size()), WTF::move(answer) });
    m_wordsOfAnswersUntilReset.append(question);
    addToTypeArenaIndex(m_indexOfAnswersUntilReset, hash, position, [&](uint32_t earlier) {
        return m_answersUntilReset[earlier].hash;
    });
    return *m_answersUntilReset.last().answer;
}

PropertyKeyID TypeArena::intern(const PropertyKey& key)
{
    uint32_t hash = hashOf(key);
    // A key of the program is its number, so that it can be a member of a set. A key of the thread cannot.
    if (std::optional<uint32_t> position = findInTypeTableOrAdd(m_table.m_propertyKeys, typeArenaAddsToProgram(m_table, m_keeps), hash, key))
        return idAt<PropertyKeyID>(IDHome::Value, *position);
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset);

    std::optional<uint32_t> known = findInTypeArenaIndex(m_propertyKeyIndex, hash, [&](uint32_t candidate) {
        return m_propertyKeys[candidate] == key;
    });
    if (known)
        return idAt<PropertyKeyID>(IDHome::Thread, *known);

    uint32_t position = static_cast<uint32_t>(m_propertyKeys.size());
    makeRoomInTypeArena(m_propertyKeys, m_outgrownPropertyKeys, 1);
    m_propertyKeys.append(key);
    addToTypeArenaIndex(m_propertyKeyIndex, hash, position, [&](uint32_t earlier) {
        return hashOf(m_propertyKeys[earlier]);
    });
    return idAt<PropertyKeyID>(IDHome::Thread, position);
}

// Two keys are one name exactly where their texts have one id, which does not hold of a text that Compile made.
PropertyKeyID TypeArena::textKey(StringID text)
{
    RELEASE_ASSERT(!liesInThread(text));
    return intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
}

PropertyKeyID TypeArena::wellKnownSymbolKey(WellKnownSymbol symbol)
{
    return intern(PropertyKey { .kind = PropertyKeyKind::WellKnownSymbol, .symbol = symbol });
}

PropertyKeyID TypeArena::registeredSymbolKey(StringID description)
{
    RELEASE_ASSERT(!liesInThread(description));
    return intern(PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = description });
}

// The hash of a joined text follows from those of its pieces: the digits of the left piece count for more by the weight in front of the right one.
static constexpr uint64_t baseOfTextHashInTypeArena = 0x9e3779b97f4a7c15ull;

static uint32_t slotHashOfTextInTypeArena(uint64_t hash)
{
    return static_cast<uint32_t>((hash ^ hash >> 32) * 0xff51afd7ed558ccdull >> 32);
}

TypeArena::FactsOfText TypeArena::factsOfText(std::span<const uint8_t> bytes)
{
    // A half of a pair takes three bytes: ed, then a0 to af for a lead and b0 to bf for a trail.
    auto isHalfOfPairAt = [&](size_t place, uint8_t firstOfSecondByte) {
        return bytes.size() >= 3 && bytes[place] == 0xed && (bytes[place + 1] & 0xf0) == firstOfSecondByte;
    };
    FactsOfText facts;
    facts.numberOfBytes = static_cast<uint32_t>(bytes.size());
    facts.isWide = isWideProgramString(bytes);
    facts.beginsWithTrailSurrogate = isHalfOfPairAt(0, 0xb0);
    facts.endsWithLeadSurrogate = isHalfOfPairAt(bytes.size() - 3, 0xa0);
    for (uint8_t byte : bytes) {
        facts.hash = facts.hash * baseOfTextHashInTypeArena + byte + 1;
        facts.weightInFront *= baseOfTextHashInTypeArena;
    }
    return facts;
}

TypeArena::FactsOfText TypeArena::factsOf(StringID text) const
{
    if (liesInThread(text))
        return m_madeTexts[positionOf(text)];
    return factsOfText(bytesOf(text));
}

StringID TypeArena::addMadeText(const MadeText& text)
{
    uint32_t position = static_cast<uint32_t>(m_madeTexts.size());
    m_madeTexts.append(text);
    addToTypeArenaIndex(m_textIndex, slotHashOfTextInTypeArena(text.hash), position, [&](uint32_t earlier) {
        return slotHashOfTextInTypeArena(m_madeTexts[earlier].hash);
    });
    return idAt<StringID>(IDHome::Thread, position);
}

StringID TypeArena::internText(std::span<const uint8_t> programString)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset && !programString.empty());
    Vector<uint8_t> canonicalBytes;
    if (!isCanonicalProgramString(programString)) {
        canonicalBytes = encodeProgramString(decodeProgramString(programString).span());
        programString = canonicalBytes.span();
    }

    MadeText text { factsOfText(programString), { }, { }, MadeText::notLaidDown };
    std::optional<uint32_t> known = findInTypeArenaIndex(m_textIndex, slotHashOfTextInTypeArena(text.hash), [&](uint32_t candidate) {
        return m_madeTexts[candidate].hash == text.hash && m_madeTexts[candidate].numberOfBytes == text.numberOfBytes && std::ranges::equal(bytesOfMadeText(candidate), programString);
    });
    if (known)
        return idAt<StringID>(IDHome::Thread, *known);

    // A copy, since the bytes may lie in this arena.
    Vector<uint8_t> added(programString);
    makeRoomInTypeArena(m_bytesOfTexts, m_outgrownBytesOfTexts, added.size());
    text.firstByte = static_cast<uint32_t>(m_bytesOfTexts.size());
    m_bytesOfTexts.append(added.span());
    return addMadeText(text);
}

bool TypeArena::joinedTextIsSpelledAsItsPieces(StringID left, StringID right) const
{
    return !factsOf(left).endsWithLeadSurrogate || !factsOf(right).beginsWithTrailSurrogate;
}

StringID TypeArena::internJoinedTexts(StringID left, StringID right)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset);
    uint64_t pieces = static_cast<uint64_t>(left.bits()) << 32 | right.bits();
    if (auto joined = m_joinedTexts.find(pieces); joined != m_joinedTexts.end())
        return idAt<StringID>(IDHome::Thread, joined->value);

    FactsOfText factsOfLeft = factsOf(left);
    FactsOfText factsOfRight = factsOf(right);
    RELEASE_ASSERT(factsOfLeft.numberOfBytes && factsOfRight.numberOfBytes);
    StringID answer;
    if (factsOfLeft.endsWithLeadSurrogate && factsOfRight.beginsWithTrailSurrogate) {
        Vector<uint8_t> bytes(bytesOf(left));
        bytes.append(bytesOf(right));
        answer = internText(bytes.span());
    } else {
        MadeText text;
        text.numberOfBytes = factsOfLeft.numberOfBytes + factsOfRight.numberOfBytes;
        RELEASE_ASSERT(text.numberOfBytes > factsOfLeft.numberOfBytes);
        text.isWide = factsOfLeft.isWide || factsOfRight.isWide;
        text.beginsWithTrailSurrogate = factsOfLeft.beginsWithTrailSurrogate;
        text.endsWithLeadSurrogate = factsOfRight.endsWithLeadSurrogate;
        text.hash = factsOfLeft.hash * factsOfRight.weightInFront + factsOfRight.hash;
        text.weightInFront = factsOfLeft.weightInFront * factsOfRight.weightInFront;
        text.left = left;
        text.right = right;
        std::optional<uint32_t> known = findInTypeArenaIndex(m_textIndex, slotHashOfTextInTypeArena(text.hash), [&](uint32_t candidate) {
            if (m_madeTexts[candidate].hash != text.hash || m_madeTexts[candidate].numberOfBytes != text.numberOfBytes)
                return false;
            std::span<const uint8_t> bytes = bytesOfMadeText(candidate);
            return std::ranges::equal(bytes.first(factsOfLeft.numberOfBytes), bytesOf(left)) && std::ranges::equal(bytes.subspan(factsOfLeft.numberOfBytes), bytesOf(right));
        });
        answer = known ? idAt<StringID>(IDHome::Thread, *known) : addMadeText(text);
    }
    m_joinedTexts.add(pieces, positionOf(answer));
    return answer;
}

// From the left end to the right end, over the pieces of pieces that were never laid down themselves. No piece is gone into by a call: a chain is as deep as it is long.
// A TEXT THAT ADDS TO ONE THAT IS LAID DOWN IS LAID DOWN BEHIND IT, AND THE TWO BEGIN AT ONE BYTE. For that a text leaves as much room behind itself as it takes, for the first text that adds to it.
// So a row of texts that each add to the one before, each of them read, costs what its last text holds, whatever else is laid down between them.
std::span<const uint8_t> TypeArena::bytesOfMadeText(uint32_t position) const
{
    if (m_madeTexts[position].firstByte == MadeText::notLaidDown) {
        uint32_t numberOfBytes = m_madeTexts[position].numberOfBytes;
        std::optional<uint32_t> firstByte;
        uint32_t end = 0;
        uint32_t endOfRoom = 0;
        auto takeRoomBehindAllTexts = [&] {
            size_t room = 2 * static_cast<size_t>(numberOfBytes);
            makeRoomInTypeArena(m_bytesOfTexts, m_outgrownBytesOfTexts, room);
            end = static_cast<uint32_t>(m_bytesOfTexts.size());
            firstByte = end;
            m_bytesOfTexts.grow(end + room);
            endOfRoom = static_cast<uint32_t>(m_bytesOfTexts.size());
        };
        auto layDown = [&](std::span<const uint8_t> bytes) {
            memcpySpan(m_bytesOfTexts.mutableSpan().subspan(end, bytes.size()), bytes);
            end += static_cast<uint32_t>(bytes.size());
        };
        Vector<StringID, 16> piecesLeft { m_madeTexts[position].right, m_madeTexts[position].left };
        while (!piecesLeft.isEmpty()) {
            StringID piece = piecesLeft.takeLast();
            if (!liesInThread(piece)) {
                if (!firstByte)
                    takeRoomBehindAllTexts();
                layDown(program()->string(piece));
                continue;
            }
            const MadeText& made = m_madeTexts[positionOf(piece)];
            if (made.firstByte == MadeText::notLaidDown) {
                piecesLeft.append(made.right);
                piecesLeft.append(made.left);
                continue;
            }
            if (!firstByte) {
                auto room = m_roomBehindTexts.find(made.firstByte + made.numberOfBytes);
                if (room != m_roomBehindTexts.end() && room->value >= numberOfBytes - made.numberOfBytes) {
                    firstByte = made.firstByte;
                    end = room->key;
                    endOfRoom = end + room->value;
                    m_roomBehindTexts.remove(room);
                    continue;
                }
                takeRoomBehindAllTexts();
            }
            layDown(m_bytesOfTexts.subspan(made.firstByte, made.numberOfBytes));
        }
        RELEASE_ASSERT(end - *firstByte == numberOfBytes && end <= endOfRoom);
        if (end < endOfRoom)
            m_roomBehindTexts.add(end, endOfRoom - end);
        m_madeTexts[position].firstByte = *firstByte;
    }
    return m_bytesOfTexts.subspan(m_madeTexts[position].firstByte, m_madeTexts[position].numberOfBytes);
}

std::span<const uint8_t> TypeArena::bytesOf(StringID text) const
{
    if (liesInThread(text))
        return bytesOfMadeText(positionOf(text));
    RELEASE_ASSERT(program());
    return program()->string(text);
}

size_t TypeArena::numberOfBytesOf(StringID text) const
{
    return liesInThread(text) ? m_madeTexts[positionOf(text)].numberOfBytes : bytesOf(text).size();
}

bool TypeArena::isWideText(StringID text) const
{
    return liesInThread(text) ? m_madeTexts[positionOf(text)].isWide : isWideProgramString(bytesOf(text));
}

StringView TypeArena::viewOf(StringID text, Vector<char16_t>& bufferForNonASCII) const
{
    if (!liesInThread(text)) {
        RELEASE_ASSERT(program());
        return program()->strings.viewOf(text, bufferForNonASCII);
    }
    std::span<const uint8_t> bytes = bytesOf(text);
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return StringView(byteCast<Latin1Character>(bytes));
    bufferForNonASCII = decodeProgramString(bytes);
    return StringView(bufferForNonASCII.span());
}

std::span<const uint8_t> TypeReader::bytesOf(StringID text) const
{
    if (m_arena)
        return m_arena->bytesOf(text);
    RELEASE_ASSERT(program() && !liesInThread(text));
    return program()->string(text);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
