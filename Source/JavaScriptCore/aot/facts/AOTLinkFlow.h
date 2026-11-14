/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinsInReach.h"
#include "AOTIRProgram.h"
#include "AOTLargeList.h"
#include "AOTLinkTables.h"
#include "AOTLinkKeyOrders.h"
#include "AOTNameClass.h"
#include "AOTTestRuns.h"
#include "AOTThreads.h"
#include "AOTUnitedFamilies.h"
#include <array>
#include <bit>
#include <memory>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/SpinBackoff.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

struct BuiltinSlot;
enum class BuiltinWordOfCell : uint8_t;

namespace Typed {

using SiteID = ID<struct SiteIDTag>;

struct LinkType {
    enum class Objects : uint8_t {
        None,
        Layout,
        Set,
        Prototype,
        Prototypes,
        Family,
        Lost,
    };
    enum class Functions : uint8_t {
        None,
        Closure,
        Set,
        Constructors,
        Lost,
    };
    enum class Counts : uint8_t {
        BelowTwoTo31 = 0,
        BelowTwoTo32 = 1,
        ExactFromZeroUp = 2,
        WholeFromZeroUp = 3,
        Int32 = 4,
        FromInt32MinBelowTwoTo32 = 5,
        Exact = 6,
        Unknown = 7,
    };
    static constexpr bool startsAtZero(Counts counts) { return counts <= Counts::WholeFromZeroUp; }
    static constexpr bool areExact(Counts counts) { return (static_cast<uint8_t>(counts) & 3) != 3; }
    static constexpr bool areBelowTwoTo32(Counts counts) { return !(static_cast<uint8_t>(counts) & 2); }
    enum class Classes : uint8_t {
        None,
        Site,
        Mixed,
    };

    static constexpr uint16_t int32 = 1 << 14;
    static_assert(!(int32 & Primitive::Defined));
    static constexpr uint16_t oneSymbol = 1 << 15;
    static_assert(!(oneSymbol & Primitive::Defined));
    static constexpr uint16_t literalString = Primitive::LiteralString;
    static constexpr uint16_t closed(uint16_t primitives)
    {
        if (primitives & Primitive::String)
            primitives |= literalString;
        if (primitives & Primitive::Integer)
            primitives |= int32;
        if (primitives & Primitive::Symbol)
            primitives |= oneSymbol;
        return primitives;
    }
    static constexpr uint16_t withoutLiterals(uint16_t primitives)
    {
        uint32_t widened = primitives & (Primitive::Defined | int32 | oneSymbol) & ~Primitive::LiteralSymbol;
        if (primitives & Primitive::LiteralSymbol)
            widened |= Primitive::Symbol;
        return closed(static_cast<uint16_t>(widened));
    }
    static constexpr bool holdsOneSymbol(uint16_t primitives) { return (primitives & (Primitive::Symbol | oneSymbol)) == oneSymbol; }
    static constexpr uint16_t primitivesOf(uint16_t primitivesOfType, Range integers)
    {
        if (primitivesOfType & Primitive::LiteralString)
            primitivesOfType |= Primitive::String;
        if (!(primitivesOfType & Primitive::Integer))
            return primitivesOfType;
        return static_cast<uint16_t>(integers.isSubsetOf(Range::int32()) ? (primitivesOfType & ~Primitive::Integer) | int32 : primitivesOfType | int32);
    }
    static constexpr uint16_t inNamesOfPrimitive(uint16_t primitives)
    {
        if (primitives & Primitive::WholeNumber)
            primitives |= int32;
        if (primitives & int32)
            primitives = static_cast<uint16_t>((primitives & ~int32) | Primitive::Integer);
        if (primitives & oneSymbol)
            primitives = static_cast<uint16_t>((primitives & ~oneSymbol) | Primitive::Symbol);
        if (primitives & literalString)
            primitives = static_cast<uint16_t>((primitives & ~literalString) | Primitive::String);
        return primitives;
    }
    static constexpr Range integersOf(uint16_t primitives)
    {
        if (primitives & (Primitive::Integer | Primitive::WholeNumber))
            return Range::full();
        return primitives & int32 ? Range::int32() : Range::empty();
    }
    TypeID primitivesAsType() const { return *tryMakeTypeOfPrimitives(inNamesOfPrimitive(primitives), integersHeldAmong(primitives)); }
    TypeID primitivesWithoutLiteralsAsType() const
    {
        uint16_t widened = withoutLiterals(primitives);
        return *tryMakeTypeOfPrimitives(inNamesOfPrimitive(widened), integersHeldAmong(widened));
    }

    enum Mark : uint8_t { BuiltinsReadByName = 1 << 0, BuiltinsLostAtJoins = 1 << 1 };
    static constexpr uint8_t everyMark = BuiltinsReadByName | BuiltinsLostAtJoins;

    uint16_t primitives { 0 };
    Objects objects : 3 { Objects::None };
    Functions functions : 3 { Functions::None };
    Classes classes : 2 { Classes::None };
    bool hasUnknownObjects : 1 { false };
    bool holdsLostBuiltins : 1 { false };
    bool isUnknownEscapeOnly : 1 { false };
    uint8_t marks : 2 { 0 };
    Counts counts : 3 { Counts::Unknown };
    uint16_t escapeOnlyPrimitives { 0 };
    uint16_t directPrimitives() const { return primitives & ~escapeOnlyPrimitives; }
    bool hasNumbers() const { return primitives & closed(Primitive::Number); }
    Range integersHeldAmong(uint16_t primitivesHeld) const
    {
        if (!areExact(counts) || !hasNumbers() || primitivesHeld & Primitive::Double)
            return integersOf(primitivesHeld);
        if (!(primitivesHeld & Primitive::Integer))
            return startsAtZero(counts) ? Range::length() : integersOf(primitivesHeld);
        switch (counts) {
        case Counts::BelowTwoTo31:
            return Range::length();
        case Counts::BelowTwoTo32:
            return { 0, UINT32_MAX };
        case Counts::ExactFromZeroUp:
            return { 0, maxExactInteger };
        case Counts::Int32:
            return Range::int32();
        case Counts::FromInt32MinBelowTwoTo32:
            return { INT32_MIN, UINT32_MAX };
        default:
            return Range::exact();
        }
    }
    static constexpr Counts countsOf(uint16_t primitivesOfType, Range integers)
    {
        if (!(primitivesOfType & Primitive::Number))
            return Counts::BelowTwoTo31;
        if (!(primitivesOfType & Primitive::Integer) || primitivesOfType & Primitive::Double || integers.isEmpty())
            return Counts::Unknown;
        bool startsAtZero = integers.minimum >= 0;
        if (integers.isSubsetOf(Range::int32()))
            return startsAtZero ? Counts::BelowTwoTo31 : Counts::Int32;
        if (integers.minimum >= INT32_MIN && integers.maximum <= UINT32_MAX)
            return startsAtZero ? Counts::BelowTwoTo32 : Counts::FromInt32MinBelowTwoTo32;
        if (integers.isSubsetOf(Range::exact()))
            return startsAtZero ? Counts::ExactFromZeroUp : Counts::Exact;
        return startsAtZero ? Counts::WholeFromZeroUp : Counts::Unknown;
    }
    bool hasDirectUnknownObjects() const { return hasUnknownObjects && !isUnknownEscapeOnly; }
    LinkType asPassed() const
    {
        LinkType passed = *this;
        bool isExactPast32Bits = counts == Counts::ExactFromZeroUp || counts == Counts::FromInt32MinBelowTwoTo32 || counts == Counts::Exact;
        if (isExactPast32Bits && !(primitives & Primitive::FractionalNumber))
            passed.primitives &= static_cast<uint16_t>(~Primitive::WholeNumber);
        return passed;
    }
    LinkType withoutBoundPast32Bits() const
    {
        LinkType unbounded = *this;
        if (counts > Counts::BelowTwoTo32 && counts != Counts::Int32)
            unbounded.counts = Counts::Unknown;
        return unbounded;
    }
    LinkType asSlotHolds(bool keepsExactIntegers) const
    {
        bool isExactPast32Bits = counts == Counts::ExactFromZeroUp || counts == Counts::FromInt32MinBelowTwoTo32 || counts == Counts::Exact;
        if (keepsExactIntegers && isExactPast32Bits && !(primitives & Primitive::FractionalNumber))
            return asPassed();
        LinkType held = withoutBoundPast32Bits();
        if (held.primitives & Primitive::Double)
            held.primitives |= closed(Primitive::Number);
        return held;
    }
    LinkType asVariableHolds() const
    {
        bool isExactPast32Bits = counts == Counts::ExactFromZeroUp || counts == Counts::FromInt32MinBelowTwoTo32 || counts == Counts::Exact;
        return isExactPast32Bits && !(primitives & Primitive::FractionalNumber) ? asPassed() : withoutBoundPast32Bits();
    }
    bool hasSameShadowsAs(const LinkType& other) const { return directPrimitives() == other.directPrimitives() && hasDirectUnknownObjects() == other.hasDirectUnknownObjects() && marks == other.marks && counts == other.counts; }
    LinkType directPart() const
    {
        LinkType seen = *this;
        seen.primitives = directPrimitives();
        seen.hasUnknownObjects = hasDirectUnknownObjects();
        seen.escapeOnlyPrimitives = 0;
        seen.isUnknownEscapeOnly = false;
        seen.counts = Counts::Unknown;
        return seen;
    }
    LinkType asEscapeOnly() const
    {
        LinkType marked = *this;
        marked.escapeOnlyPrimitives = primitives;
        marked.isUnknownEscapeOnly = hasUnknownObjects;
        return marked;
    }
    BuiltinClassID builtinClass;
    BuiltinID builtin;
    uint16_t setOfClasses { 0 };
    LayoutID layout;
    FamilyID family;
    FunctionID function;
    FamilyID constructorsOf;
    SiteID site;

    void dump(PrintStream&) const;

    void dropInstances()
    {
        classes = Classes::None;
        builtinClass = { };
        setOfClasses = 0;
        site = { };
    }
    LinkType instancesAlone() const
    {
        LinkType instances;
        instances.classes = classes;
        instances.builtinClass = builtinClass;
        instances.setOfClasses = setOfClasses;
        instances.site = site;
        return instances;
    }
    bool holdsSameInstancesAs(const LinkType& other) const { return classes == other.classes && builtinClass == other.builtinClass && setOfClasses == other.setOfClasses && site == other.site; }
    ALWAYS_INLINE bool isNever() const { return holdsSameAs(LinkType { }); }
    static LinkType anything()
    {
        LinkType type;
        type.primitives = closed(Primitive::All);
        type.hasUnknownObjects = true;
        return type;
    }
    LinkType without(uint8_t marksToTakeOff) const
    {
        LinkType unmarked = *this;
        unmarked.marks = marks & ~marksToTakeOff;
        return unmarked;
    }
    static LinkType anythingWithBuiltinsReadByName()
    {
        LinkType type = anything();
        type.marks = BuiltinsReadByName;
        return type;
    }
    ALWAYS_INLINE bool isAnything() const { return holdsSameAs(anything()); }
    bool holdsPrimitivesAlone() const { return objects == Objects::None && functions == Functions::None && classes == Classes::None && !builtin && !hasUnknownObjects; }

    using Words = std::array<uint64_t, 4>;
    ALWAYS_INLINE Words words() const { return std::bit_cast<Words>(*this); }
    using References = std::array<uint64_t, 3>;
    ALWAYS_INLINE uint64_t head() const { return words()[0]; }
    ALWAYS_INLINE References references() const
    {
        Words held = words();
        return { held[1], held[2], held[3] };
    }
    static ALWAYS_INLINE References noReferences() { return LinkType { }.references(); }
    static ALWAYS_INLINE bool areSameReferences(const References& first, const References& second) { return !((first[0] ^ second[0]) | (first[1] ^ second[1]) | (first[2] ^ second[2])); }
    static ALWAYS_INLINE LinkType of(uint64_t head, const References& references) { return std::bit_cast<LinkType>(Words { head, references[0], references[1], references[2] }); }
    static ALWAYS_INLINE LinkType ofHead(uint64_t head) { return of(head, noReferences()); }
    static ALWAYS_INLINE Words wordsOfShadows()
    {
        LinkType marked;
        marked.isUnknownEscapeOnly = true;
        marked.marks = everyMark;
        marked.escapeOnlyPrimitives = UINT16_MAX;
        marked.counts = Counts::BelowTwoTo31;
        Words shadows = marked.words();
        Words nothing = LinkType { }.words();
        for (size_t index = 0; index < shadows.size(); ++index)
            shadows[index] ^= nothing[index];
        return shadows;
    }
    static ALWAYS_INLINE uint64_t wordOfNames()
    {
        LinkType named;
        named.objects = static_cast<Objects>(7);
        named.functions = static_cast<Functions>(7);
        named.classes = static_cast<Classes>(3);
        named.builtinClass = BuiltinClassID(0);
        return named.head() ^ LinkType { }.head();
    }
    static ALWAYS_INLINE uint64_t wordOfMarks()
    {
        LinkType marked;
        marked.marks = everyMark;
        return marked.head() ^ LinkType { }.head();
    }
    static ALWAYS_INLINE uint64_t wordOfLostBuiltins()
    {
        LinkType lost;
        lost.holdsLostBuiltins = true;
        return lost.head() ^ LinkType { }.head();
    }
    static ALWAYS_INLINE uint64_t wordOfPrimitives(uint16_t primitives)
    {
        LinkType held;
        held.primitives = primitives;
        return held.head() ^ LinkType { }.head();
    }
    static ALWAYS_INLINE bool namesBuiltinOrClasses(const References& references) { return static_cast<uint32_t>(references[0]) != static_cast<uint32_t>(noReferences()[0]); }
    static ALWAYS_INLINE uint64_t wordOfFlagsThatOnlyRise()
    {
        LinkType raised;
        raised.hasUnknownObjects = true;
        raised.marks = everyMark;
        raised.counts = Counts::BelowTwoTo31;
        return raised.words()[0] ^ LinkType { }.words()[0];
    }
    ALWAYS_INLINE bool raisesNoFlagOver(const LinkType& held) const { return !(words()[0] & ~held.words()[0] & wordOfFlagsThatOnlyRise()); }
    ALWAYS_INLINE bool isSameAs(const LinkType& other) const { return head() == other.head() && areSameReferences(references(), other.references()); }
    ALWAYS_INLINE bool holdsSameWithShadowsAs(const LinkType& other) const { return isSameAs(other) || (holdsSameAs(other) && hasSameShadowsAs(other)); }
    ALWAYS_INLINE bool tellsUnseenNothing() const { return namesNothing() && !holdsLostBuiltins && !marks; }
    ALWAYS_INLINE bool hasNoReferences() const { return areSameReferences(references(), noReferences()); }
    ALWAYS_INLINE bool holdsOneSymbolInAnyPart() const { return (primitives & oneSymbol) && (holdsOneSymbol(primitives) || holdsOneSymbol(directPrimitives())); }
    ALWAYS_INLINE bool namesNothing() const { return objects == Objects::None && functions == Functions::None && classes == Classes::None && !builtin; }
    ALWAYS_INLINE Words wordsWithoutShadows() const
    {
        Words held = words();
        Words shadows = wordsOfShadows();
        for (size_t index = 0; index < held.size(); ++index)
            held[index] &= ~shadows[index];
        return held;
    }

    ALWAYS_INLINE bool holdsSameAs(const LinkType& other) const
    {
        if (primitives != other.primitives)
            return false;
        Words own = wordsWithoutShadows();
        Words ofOther = other.wordsWithoutShadows();
        uint64_t difference = 0;
        for (size_t index = 0; index < own.size(); ++index)
            difference |= own[index] ^ ofOther[index];
        return !difference;
    }

    friend ALWAYS_INLINE bool operator==(const LinkType& first, const LinkType& second) { return first.holdsSameAs(second); }
};
static_assert(std::has_unique_object_representations_v<LinkType>);

template<typename MemberID, size_t maxSetMembers>
class SmallSetsInLinkFlow {
    WTF_MAKE_NONCOPYABLE(SmallSetsInLinkFlow);
public:
    static constexpr size_t mostMembers = maxSetMembers;

    SmallSetsInLinkFlow() = default;
    ~SmallSetsInLinkFlow()
    {
        for (auto& part : m_parts)
            delete[] part.load(std::memory_order_relaxed);
    }

    std::span<const MemberID> membersOf(uint32_t set) const
    {
        const Members& members = recordOf(set);
        size_t size = 2;
        while (size < mostMembers && members[size])
            ++size;
        return std::span<const MemberID>(members).first(size);
    }
    uint32_t numberOf(std::span<const MemberID> sortedMembers)
    {
        RELEASE_ASSERT(sortedMembers.size() >= 2 && sortedMembers.size() <= mostMembers);
        Members members { };
        std::ranges::copy(sortedMembers, members.begin());
        auto words = [&](size_t first) -> Words {
            return { static_cast<uint64_t>(members[first].bits()) << 32 | members[first + 1].bits(), static_cast<uint64_t>(members[first + 2].bits()) << 32 | members[first + 3].bits() };
        };
        Key key;
        if constexpr (mostMembers == 4)
            key = words(0);
        else
            key = { words(0), words(4) };
        while (m_isLocked.load(std::memory_order_relaxed) || m_isLocked.exchange(true, std::memory_order_acquire))
            simde_mm_pause();
        uint32_t next = m_size.load(std::memory_order_relaxed);
        auto added = m_numbers.add(key, next);
        if (added.isNewEntry) {
            auto [part, position] = placeOf(next);
            Members* records = m_parts[part].load(std::memory_order_relaxed);
            if (!records) {
                records = new Members[static_cast<size_t>(1) << (part + log2OfSetsInFirstPart)] { };
                m_parts[part].store(records, std::memory_order_release);
            }
            records[position] = members;
            m_size.store(next + 1, std::memory_order_release);
        }
        uint32_t number = added.iterator->value;
        m_isLocked.store(false, std::memory_order_release);
        return number;
    }
    uint32_t size() const { return m_size.load(std::memory_order_acquire); }

private:
    using Members = std::array<MemberID, mostMembers>;
    using Words = std::pair<uint64_t, uint64_t>;
    static_assert(mostMembers == 4 || mostMembers == 8);
    using Key = std::conditional_t<mostMembers == 4, Words, std::pair<Words, Words>>;
    static constexpr unsigned log2OfSetsInFirstPart = 10;
    static constexpr size_t numberOfParts = 32 - log2OfSetsInFirstPart;

    static std::pair<unsigned, uint32_t> placeOf(uint32_t set)
    {
        uint32_t stretch = (set >> log2OfSetsInFirstPart) + 1;
        unsigned part = 31 - std::countl_zero(stretch);
        return { part, set - (((1u << part) - 1) << log2OfSetsInFirstPart) };
    }
    const Members& recordOf(uint32_t set) const
    {
        auto [part, position] = placeOf(set);
        return m_parts[part].load(std::memory_order_acquire)[position];
    }

    std::array<std::atomic<Members*>, numberOfParts> m_parts { };
    UncheckedKeyHashMap<Key, uint32_t> m_numbers;
    std::atomic<uint32_t> m_size { 0 };
    std::atomic<bool> m_isLocked { false };
};
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

class LinkReferenceTable {
    WTF_MAKE_NONCOPYABLE(LinkReferenceTable);
public:
    using References = LinkType::References;

    JS_EXPORT_PRIVATE LinkReferenceTable();
    JS_EXPORT_PRIVATE ~LinkReferenceTable();

    ALWAYS_INLINE const References& at(uint32_t number) const { return m_records[number]; }

    struct Recent {
        References references { LinkType::noReferences() };
        uint32_t number { 0 };
    };
    using RecentNumbers = std::array<Recent, 1024>;
    JS_EXPORT_PRIVATE uint32_t numberOf(const References&, RecentNumbers&);

    ALWAYS_INLINE void makeRoomFor(size_t numberOfReferences)
    {
        size_t needed = m_nextNumber.load(std::memory_order_relaxed) + numberOfReferences + numberOfShards * recordBatchSize;
        if (needed > m_room) [[unlikely]]
            grow(needed);
    }

private:
    static constexpr unsigned numberOfShards = 128;
    static constexpr uint32_t recordBatchSize = 64;

    struct Shard {
        Vector<uint32_t> slots;
        uint32_t size { 0 };
        uint32_t next { 0 };
        uint32_t end { 0 };
        std::atomic<bool> isLocked { false };
    };

    static ALWAYS_INLINE uint64_t hashOf(const References& references)
    {
        uint64_t hash = references[0] * 0x9e3779b97f4a7c15ull;
        hash = (std::rotl(hash, 29) ^ references[1]) * 0xbf58476d1ce4e5b9ull;
        hash = (std::rotl(hash, 31) ^ references[2]) * 0x94d049bb133111ebull;
        return hash ^ hash >> 32;
    }
    uint32_t findOrAdd(const References&, uint64_t hash);
    uint32_t takeNumber(Shard&);
    void growSlots(Shard&);

    void grow(size_t needed);

    References* m_records { nullptr };
    size_t m_room { 0 };
    std::array<Shard, numberOfShards> m_shards;
    std::atomic<uint32_t> m_nextNumber { recordBatchSize };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

using SetsOfLayoutsInLinkFlow = SmallSetsInLinkFlow<LayoutID, 8>;
using SetsOfFunctionsInLinkFlow = SmallSetsInLinkFlow<FunctionID, 8>;

class SetsOfClassesInLinkFlow {
    WTF_MAKE_NONCOPYABLE(SetsOfClassesInLinkFlow);
public:
    SetsOfClassesInLinkFlow() = default;
    ~SetsOfClassesInLinkFlow()
    {
        for (uint32_t set = 0, size = m_size.load(std::memory_order_relaxed); set < size; ++set)
            delete[] recordOf(set).members.load(std::memory_order_relaxed);
        for (auto& part : m_parts)
            delete[] part.load(std::memory_order_relaxed);
    }

    std::span<const BuiltinClassID> membersOf(uint16_t set) const
    {
        const Record& record = recordOf(set - 1u);
        const BuiltinClassID* members = record.members.load(std::memory_order_acquire);
        RELEASE_ASSERT(members);
        return { members, record.size };
    }
    uint16_t numberOf(std::span<const BuiltinClassID> sortedMembers)
    {
        RELEASE_ASSERT(sortedMembers.size() >= 2);
        uint32_t hash = 2166136261u;
        for (BuiltinClassID member : sortedMembers)
            hash = (hash ^ member.index()) * 16777619u;
        hash >>= 1;
        while (m_isLocked.load(std::memory_order_relaxed) || m_isLocked.exchange(true, std::memory_order_acquire))
            simde_mm_pause();
        Vector<uint16_t, 1>& withSameHash = m_numbers.add(hash, Vector<uint16_t, 1> { }).iterator->value;
        uint16_t number = 0;
        for (uint16_t candidate : withSameHash) {
            if (std::ranges::equal(membersOf(candidate), sortedMembers))
                number = candidate;
        }
        uint32_t next = m_size.load(std::memory_order_relaxed);
        if (!number && next < std::numeric_limits<uint16_t>::max()) {
            auto [part, position] = placeOf(next);
            Record* records = m_parts[part].load(std::memory_order_relaxed);
            if (!records) {
                records = new Record[static_cast<size_t>(1) << (part + log2OfSetsInFirstPart)] { };
                m_parts[part].store(records, std::memory_order_release);
            }
            BuiltinClassID* members = new BuiltinClassID[sortedMembers.size()];
            std::ranges::copy(sortedMembers, members);
            records[position].size = sortedMembers.size();
            records[position].members.store(members, std::memory_order_release);
            m_size.store(next + 1, std::memory_order_release);
            number = static_cast<uint16_t>(next + 1);
            withSameHash.append(number);
        }
        m_isLocked.store(false, std::memory_order_release);
        return number;
    }

private:
    struct Record {
        std::atomic<const BuiltinClassID*> members { nullptr };
        size_t size { 0 };
    };
    static constexpr unsigned log2OfSetsInFirstPart = 4;
    static constexpr size_t numberOfParts = 17 - log2OfSetsInFirstPart;

    static std::pair<unsigned, uint32_t> placeOf(uint32_t set)
    {
        uint32_t stretch = (set >> log2OfSetsInFirstPart) + 1;
        unsigned part = 31 - std::countl_zero(stretch);
        return { part, set - (((1u << part) - 1) << log2OfSetsInFirstPart) };
    }
    const Record& recordOf(uint32_t set) const
    {
        auto [part, position] = placeOf(set);
        return m_parts[part].load(std::memory_order_acquire)[position];
    }

    std::array<std::atomic<Record*>, numberOfParts> m_parts { };
    UncheckedKeyHashMap<uint32_t, Vector<uint16_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numbers;
    std::atomic<uint32_t> m_size { 0 };
    std::atomic<bool> m_isLocked { false };
};

using FlowNodeID = ID<struct FlowNodeIDTag>;

struct FlowObserver {
    enum class Kind : uint8_t { SubjectOfCall, CalleeOfCall, ReceiverOfCall, SubjectOfAccess, KeyOfAccess, SubjectOfChange, OtherOfChange, OperandOfOperator, Inspected, MemberOfSharedCall, MemberOfSharedAccess, MemberOfAccess, SymbolOfPreparation, PassedToBeNamed, PassedToBeLinked, OperandOnlyLookedAt, KeysAreListed, FieldOfAccessorLaid, FieldIsOnlyAsked };

    Kind kind { Kind::SubjectOfCall };
    FunctionID function;
    uint32_t record { 0 };

    void dump(PrintStream&) const;

    friend bool operator==(const FlowObserver&, const FlowObserver&) = default;
};

struct BreachOfClosure {
    FlowNodeID node;
    LinkType before;
    LinkType after;
    ASCIILiteral set;
    LayoutID layout;
    OptionSet<LayoutChange> changes;
    FlowObserver told;
    FlowNodeID goneOver;
};

class StateOfLinkReach;
struct KeyAffixes;
struct DeleterOfStateOfLinkReach {
    void operator()(StateOfLinkReach*) const;
};

class StateOfLinkCalls;
struct DeleterOfStateOfLinkCalls {
    void operator()(StateOfLinkCalls*) const;
};

class StateOfLinkAccesses;
struct DeleterOfStateOfLinkAccesses {
    void operator()(StateOfLinkAccesses*) const;
};

class StateOfLinkOperators;
struct DeleterOfStateOfLinkOperators {
    void operator()(StateOfLinkOperators*) const;
};

class WriterOfLinkLayouts;
struct DeleterOfWriterOfLinkLayouts {
    void operator()(WriterOfLinkLayouts*) const;
};

ALWAYS_INLINE bool isPrivateNameInLink(const Program& program, PropertyKeyID key) { return key && program.at(key).kind == PropertyKeyKind::PrivateName; }
FunctionID parentConstructorOf(const Program&, FunctionID);

constexpr bool keepsExactIntegersInFields(LayoutKind kind) { return kind == LayoutKind::Record || kind == LayoutKind::Instance; }

class LinkFlow {
    WTF_MAKE_NONCOPYABLE(LinkFlow);
public:
    LinkFlow(Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&, OrderOfWorklist = OrderOfWorklist::NewestFirst);
    ~LinkFlow();

    OrderOfWorklist orderOfWorklist() const { return m_order; }
    Program& program;
    const ThinSummaries& summaries;
    const Families& families;
    const Families* familiesBehindFlow { nullptr };
    const Vector<DeclaredSetForFamilies>* setsDeclaredInProgram { nullptr };
    bool leavesOutDirectEvals { false };
    bool listsStoresOfWrappedBodies { false };
    const LooksAtBodies& looks;
    Mechanisms mechanisms;
    BuildContext& build;
    const ParallelLoops& loops;
    bool isGivenMore { false };

    FlowNodeID parameter(FunctionID function, uint32_t entryParameter) const
    {
        const NodesOfFunction& nodes = m_nodesOfFunctions[function.index()];
        RELEASE_ASSERT(entryParameter < nodes.firstResult - nodes.firstParameter);
        return FlowNodeID(nodes.firstParameter + entryParameter);
    }
    FlowNodeID result(FunctionID function, uint32_t index) const
    {
        const NodesOfFunction& nodes = m_nodesOfFunctions[function.index()];
        RELEASE_ASSERT(index < nodes.thrown - nodes.firstResult);
        return FlowNodeID(nodes.firstResult + index);
    }
    bool listsReturns(FunctionID id) const { return m_functionsListingReturns.get(id.index()); }
    bool isResultAdded(FunctionID id) const { return m_functionsWithAddedResults.get(id.index()); }
    void layResult(FunctionID);
    FlowNodeID resultToFollow(FunctionID function, uint32_t index)
    {
        layResult(function);
        return result(function, index);
    }
    LinkType passedByTest(const LinkType&, const SummaryParameterTest&) const;
    FlowNodeID thrown(FunctionID) const;
    FlowNodeID capture(FunctionID, uint32_t index) const;
    FlowNodeID variable(VariableID) const;
    ALWAYS_INLINE FlowNodeID field(FieldReference reference) const
    {
        uint32_t first = m_firstFieldOfLayout[reference.layout.index()];
        RELEASE_ASSERT(reference.field < m_firstFieldOfLayout[reference.layout.index() + 1] - first);
        return FlowNodeID(m_nodeOfField[first + reference.field]);
    }
    FlowNodeID member(LayoutID classLayout, ClassSide side, uint32_t index) const
    {
        size_t sideIndex = 2 * static_cast<size_t>(classLayout.index()) + static_cast<size_t>(side);
        uint32_t first = m_firstMemberNodeOfSide[sideIndex];
        if (index >= m_firstMemberNodeOfSide[sideIndex + 1] - first)
            return unseen();
        return FlowNodeID(first + index);
    }
    enum class MakerOfSite : uint8_t {
        PartOfRow,
        WrapperAtCall,
        PairAtCall,
        ListOfArguments,
    };
    SiteID siteOf(MakerOfSite, uint32_t number, uint32_t part, BuiltinClassID, bool onlyWaitRightBehindSeesIt = false, bool isOfClassItself = true, bool isCreatedWithoutHoles = true, bool isCreatedWithoutEngineNames = false);
    void noteListIsCopied(SiteID);
    bool isCreatedWithoutEngineNames(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].isCreatedWithoutEngineNames; }
    bool isCreatedWithoutHoles(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].isCreatedWithoutHoles; }
    bool hasNoHoles(const LinkType&) const;
    bool isOfClassItself(SiteID site) const { return m_sites[m_familiesOfSites.representativeWithoutShortening(site.index())].isOfClassItself; }
    SiteID siteOfSummary(FunctionID, uint32_t rank, BuiltinClassID);
    uint32_t numberOfSitesOfSummary(FunctionID function) const { return m_firstSiteOfFunction[function.index() + 1] - m_firstSiteOfFunction[function.index()]; }
    SiteID listOfArgumentsOf(FunctionID);
    enum class CallerOfGatherer : uint8_t { InSight, UnseenByEscapeAlone };
    FlowNodeID argumentGatheredAt(FunctionID, uint32_t place);
    FlowNodeID argumentsGatheredAtAnyPlace(FunctionID);
    FlowNodeID elementsOfAppliedArguments(FunctionID);
    void takeElementsOfAppliedArguments(FunctionID);
    void noteNumberOfArgumentsGathered(FunctionID, uint32_t number, CallerOfGatherer = CallerOfGatherer::InSight);
    void gatherWhatListsThatArriveHold(FunctionID);
    void passOnGatheredArgument(FunctionID gatherer, uint32_t place, FlowNodeID to);
    void passOnGatheredArguments(FunctionID gatherer, uint32_t firstPlace, FunctionID gathererBehind);
    BuiltinClassID classOf(SiteID site) const
    {
        return m_sites[site.index()].builtinClass;
    }
    BuiltinLayoutID layoutOfRecordsOfFamilyOf(SiteID site) const
    {
        // Without the lock: a site never moves; it is written right behind siteOf(), by whoever has made the site, and where families merge, which is never while several threads are at work.
        return m_sites[m_familiesOfSites.representativeOf(site.index())].layoutOfRecords;
    }
    void noteRecordsAreMadeAt(SiteID, BuiltinLayoutID);
    void noteLengthIsWritten(SiteID, uint32_t length);
    void noteListIsWritten(SiteID, FunctionID, uint32_t firstPlace, uint32_t numberOfPlaces);
    FlowNodeID whatIsWrittenAt(SiteID, uint32_t place);
    uint32_t numberOfPlacesWrittenOf(SiteID) const;
    void tellWrittenListIsChanged(const LinkType&);
    uint32_t writtenLengthOf(SiteID site) const { return m_sites[m_familiesOfSites.representativeWithoutShortening(site.index())].writtenLength; }
    uint32_t leastLengthOfViewsIn(const LinkType&) const;
    void noteReadsOffViewMadeAtSite(FunctionID function)
    {
        if (!m_functionsThatReadOffViewsMadeAtSites.at(function.index()))
            m_functionsThatReadOffViewsMadeAtSites.set(function.index(), 1);
    }
    bool readsOffViewMadeAtSite(FunctionID function) const { return m_functionsThatReadOffViewsMadeAtSites.at(function.index()); }
    bool mayHoldCellsOfCompiledLayouts(SiteID site) const
    {
        return m_sites[m_familiesOfSites.representativeOf(site.index())].mayHoldCellsOfCompiledLayouts;
    }
    size_t numberOfSites() const { return m_sites.size(); }
    struct ClassesOfSites {
        BitVector haveEscaped;
        BitVector areClimbedFrom;
    };
    ClassesOfSites findClassesOfSitesThatHaveEscapedOrAreClimbedFrom() const;
    static LinkType typeOf(SiteID, BuiltinClassID);
    LinkType typeOf(SiteID site) const { return typeOf(site, classOf(site)); }
    ALWAYS_INLINE FlowNodeID contents(SiteID member, SlotOfContents slot)
    {
        RELEASE_ASSERT(slot);
        if (m_severalThreadsTell)
            return contentsFoundOrMadeByLane(member, slot);
        return contentsFoundOrMadeByOneThread(member, slot);
    }
    void takeAsContents(SiteID, SlotOfContents, FlowNodeID);
    FlowNodeID contentsOf(const LinkType& type, SlotOfContents slot) { return type.classes == LinkType::Classes::None ? FlowNodeID() : contents(type.site, slot); }
    bool isMarked(SiteID site, SlotOfContents mark) const
    {
        FlowNodeID held = findContents(m_familiesOfSites.representativeOf(site.index()), mark);
        return held && (held == unseen() || !typeAt(held).isNever());
    }
    void markSite(SiteID site, SlotOfContents mark)
    {
        if (!isMarked(site, mark))
            arrive(contents(site, mark), fromTypeID(TypeTable::undefined));
    }
    FlowNodeID findContentsOf(const LinkType& type, SlotOfContents slot) const { return type.classes == LinkType::Classes::None ? FlowNodeID() : findContents(m_familiesOfSites.representativeOf(type.site.index()), slot); }
    static SlotOfContents slotOf(const BuiltinSlot&);
    SlotOfContents slotOf(BuiltinSlotID) const;
    BuiltinWordOfCell wordOfCell(SlotOfContents) const;
    Vector<SlotOfContents, 2> slotsBehind(BuiltinClassID, SlotOfContents) const;
    void unite(SiteID, SiteID);
    void escape(SiteID);
    bool hasEscaped(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].hasEscaped; }
    SiteID familyOf(SiteID site) const { return SiteID(m_familiesOfSites.representativeOf(site.index())); }
    FlowNodeID unseen() const { return FlowNodeID(0u); }
    Vector<FlowNodeID, 1> nodesOfWhatFunctionAnswers(FunctionID);
    FlowNodeID nodeOf(FunctionID function, uint32_t nodeInSummary) const
    {
        std::span<const NodesOfFunction> nodesOfFunctions = m_nodesOfFunctions.span();
        size_t index = function.index();
        RELEASE_ASSERT(index + 1 < nodesOfFunctions.size());
        uint32_t firstPlace = nodesOfFunctions[index].firstSummaryNode;
        RELEASE_ASSERT(nodeInSummary < nodesOfFunctions[index + 1].firstSummaryNode - firstPlace);
        return FlowNodeID(m_nodesOfSummaries[firstPlace + nodeInSummary]);
    }
    std::span<const uint32_t> nodesOfSummaryOf(FunctionID function) const LIFETIME_BOUND
    {
        uint32_t firstPlace = m_nodesOfFunctions[function.index()].firstSummaryNode;
        return m_nodesOfSummaries.span().subspan(firstPlace, m_nodesOfFunctions[function.index() + 1].firstSummaryNode - firstPlace);
    }
    void prefetchNodesOf(FunctionID) const;

    struct FoundMember {
        LayoutID classLayout;
        uint32_t index { 0 };
        MethodKind kind { MethodKind::Method };
    };
    class FoundMembers {
    public:
        const FoundMember* begin() const LIFETIME_BOUND { return m_members.begin(); }
        const FoundMember* end() const LIFETIME_BOUND { return m_members.end(); }
        size_t size() const { return m_members.size(); }
        const FoundMember& operator[](size_t index) const LIFETIME_BOUND { return m_members[index]; }
        bool mayAnswer() const { return !m_members.isEmpty(); }
        bool lookupGoesOnAboveClasses() const { return m_lookupGoesOnAboveClasses; }

    private:
        friend class LinkFlow;
        Vector<FoundMember, 2> m_members;
        bool m_lookupGoesOnAboveClasses { true };
    };
    FoundMembers findMembers(LayoutID, ClassSide, PropertyKeyID) const;
    bool namesMayBeIncomplete(LayoutID, ClassSide) const;
    bool hasComputedKeysOfItsOwn(LayoutID, ClassSide) const;
    bool staticNamesMayBeIncompleteBesideConstructorFunctions(LayoutID layout) const { return m_staticSidesBelowIncompleteNamesBesideConstructorFunctions.quickGet(layout.index()); }
    bool extendsAloneLinksClassObjects(LayoutID below, LayoutID above) const;
    bool findsPrototypeLinkOfSubject(LayoutID layout, ClassSide side, PropertyKeyID key, bool hasOtherReceiver) const { return !hasOtherReceiver && !findMembers(layout, side, key).mayAnswer(); }
    bool classesAnswerReadsUnderNumbers(LayoutID layout) const { return m_layoutsBelowMembersUnderNumbers.get(layout.index()) || namesMayBeIncomplete(layout, ClassSide::Prototype); }
    bool isAbsentFromObjectPrototype(PropertyKeyID) const;
    bool computeAbsence(PropertyKeyID) const;
    enum class Absence : uint8_t { NotAsked, IsAbsent, IsPresent };
    mutable Vector<Absence> m_absencesFromObjectPrototype;
    LayoutID classOfConstructor(FunctionID) const;
    NameClass nameClassOf(PropertyKeyID key) const { return m_nameClassesOfKeys[key.index()]; }

    BitVector findClassesWithFollowedPrototypes();
    bool instancesMayHoldName(LayoutID, PropertyKeyID) const;
    LayoutID layoutOfConstructorFunction(FunctionID) const;
    bool isOfConstructorFunction(LayoutID layout) const { return m_layoutsOfConstructorFunctions.get(layout.index()); }
    bool followsNamesOnPrototypeOf(LayoutID layout) const { return m_layoutsWithFollowedPrototypes.get(layout.index()); }
    FlowNodeID storedOnPrototypeOf(LayoutID, PropertyKeyID, bool isLaidByBodyOfFile, bool isDefinition = false, bool isByDescriptor = false);
    void noteInstancesLaidAsPrototype(const LinkType&);
    bool fillsNewObjects(FunctionID function) const { return m_functionsFillingNewObjects.get(function.index()); }
    bool mayHoldAccessorOnPrototype(LayoutID, PropertyKeyID) const;
    bool prototypeKeepsConstructor(LayoutID) const;
    void notePrototypeIsReplaced(LayoutID);
    bool mayBeOnClosuresOf(FunctionID, PropertyKeyID) const;
    void addConstructedPrototype(FunctionID function, LayoutID below, FlowNodeID instance, std::optional<uint32_t> copyingCall = std::nullopt);
    bool isConstructedPrototype(LayoutID) const;
    enum class AccessorsAbove : bool { AreEnteredByNobody, AreEnteredByCaller };
    FlowNodeID foundOnPrototypesAbove(LayoutID, PropertyKeyID, AccessorsAbove = AccessorsAbove::AreEnteredByNobody);
    FlowNodeID foundOnPrototypesAboveUnderNumbers(LayoutID);
    void lookForSetterOnPrototypesAbove(LayoutID, PropertyKeyID, AccessorsAbove = AccessorsAbove::AreEnteredByNobody);
    bool isDefinedByDescriptorSomewhere(PropertyKeyID) const;
    static bool holdsSymbolsAlone(const LinkType& key) { return key.holdsPrimitivesAlone() && !key.holdsLostBuiltins && key.primitives && !(key.primitives & ~(Primitive::Symbol | LinkType::oneSymbol)); }
    PropertyKeyID keyHeldAt(FunctionID, uint32_t keyNode) const;
    PropertyKeyID keyHeldAt(FunctionID, FlowNodeID) const;
    void layAccessorOnPrototypeOf(LayoutID, PropertyKeyID, FunctionID getter, FunctionID setter);
    void letAccessorsOnPrototypesAboveEscape(LayoutID, PropertyKeyID);
    Vector<FlowNodeID, 2> halvesOfAccessorsOnPrototypesAbove(LayoutID, PropertyKeyID, bool ofSetters);
    bool isBelowConstructorFunction(LayoutID layout) const { return m_layoutsBelowConstructorFunctions.get(layout.index()); }
    bool isBelowFollowedPrototype(LayoutID layout) const { return m_layoutsBelowFollowedPrototypes.get(layout.index()); }
    bool mayNotBeLinkedYetBelow(LayoutID from, LayoutID classAbove) const;
    bool namesParentOfPrototype(const LinkType& parent, LayoutID classLayout) const;
    bool namesParentOfFunction(const LinkType& parent, FunctionID child) const;
    bool answersNothing(FunctionID) const;
    bool answersOwnThis(FunctionID) const;
    void noteConstructionLeavesParent(FunctionID);
    void linkFunctionBelow(FunctionID child, FunctionID parent);

    struct PrototypeHelper {
        struct Operand {
            enum class Kind : uint8_t { Other, Parameter, PrototypeOfParameter, Unknown };

            Kind kind { Kind::Other };
            uint32_t parameter { 0 };

            bool isOther() const { return kind == Kind::Other; }
            friend bool operator==(const Operand&, const Operand&) = default;
        };
        struct Step {
            enum class Kind : uint8_t { LaysPrototype, LinksParent, StoresName, CopiesNames, AsksOwnName, CallsFunction };

            Kind kind { Kind::LaysPrototype };
            Operand object;
            Operand value;
            PropertyKeyID key;
            FlowNodeID stored;
            uint32_t call { 0 };
            bool resultIsUsed { false };
        };
        enum class Result : uint8_t { IsOther, IsParameter, IsUnknown };

        Vector<Step, 6> steps;
        Vector<uint32_t, 4> parameters;
        Vector<FunctionID, 1> surrogates;
        Result result { Result::IsOther };
        uint32_t returnedParameter { 0 };
        bool linksOperands { false };
        bool isOne { false };
    };
    struct PrototypeHelperStep {
        PrototypeHelper::Step::Kind kind { PrototypeHelper::Step::Kind::LaysPrototype };
        FunctionID writer;
        FunctionID object;
        FunctionID value;
        bool objectIsPrototype { false };
        bool valueIsPrototype { false };
        PropertyKeyID key;
        FlowNodeID stored;
    };
    struct PrototypeHelperCall {
        enum class State : uint8_t { Waits, IsLinked, IsGivenUp };

        FunctionID function;
        Vector<FlowNodeID, 4> arguments;
        uint32_t call { 0 };
        State state { State::Waits };
    };
    bool followsCopyingCall(uint32_t call);
    static bool onlyCopiesNames(const PrototypeHelper&);
    bool mayBePrototypeHelper(FunctionID function) const { return m_prototypeHelperCandidates.quickGet(function.index()); }
    const PrototypeHelper* findPrototypeHelper(FunctionID);
    void followPrototypeHelperCall(uint32_t call, FunctionID helper, Vector<FlowNodeID, 4>&& arguments);
    Vector<PrototypeHelperStep> prototypeHelperSteps() const;
    FlowNodeID objectsMadeForClassBelowConstructorFunction(LayoutID);
    FlowNodeID objectsMadeThroughConstructorOf(LayoutID);
    FlowNodeID objectsMadeBySuperCallsOf(LayoutID);
    enum class UnknownKey : bool { IsAnyKey, IsSomeSymbol };
    void defineOnPrototypeOf(LayoutID, PropertyKeyID, UnknownKey = UnknownKey::IsAnyKey);
    void openPrototypeOf(LayoutID);
    struct NameOnPrototype {
        LayoutID layout;
        PropertyKeyID key;
        FlowNodeID stored;
        FlowNodeID storedInSight;
        bool isStoredInto { false };
        bool isStoredIntoFromOutside { false };
        bool isPassedByLookups { false };
        bool isDefinedByDescriptor { false };
        bool isReadUnderAnyKey { false };
        uint32_t nextOfLayout { UINT32_MAX };
        bool isAccessorAsWritten { false };
        bool isPassedByLookupsThatEnterNoAccessor { false };
        bool isDefinedUnderSomeSymbol { false };
        bool isDefinedInSight { false };
        bool mayInterceptStores { false };
        bool hasAppendedField { false };
        FlowNodeID getter;
        FlowNodeID setter;

        bool mayHoldAccessor() const { return isDefinedByDescriptor || isDefinedUnderSomeSymbol || isAccessorAsWritten; }
    };
    bool hasOnlyPrimitives(const NameOnPrototype&) const;
    bool hasNoFunctionsInSight(const NameOnPrototype&) const;
    std::span<const NameOnPrototype> namesOnPrototypes() const LIFETIME_BOUND { return m_namesOnPrototypes.span(); }
    LayoutID findClassNamedAt(FunctionID asker, const SummaryNode&) const;

    struct NameOnClosures {
        FunctionID function;
        PropertyKeyID key;
        FlowNodeID laid;
        uint32_t next { UINT32_MAX };
    };
    bool followsNamesOnClosuresOf(FunctionID, PropertyKeyID) const;
    bool mayHoldOtherClassPrototype(FunctionID) const;
    bool isNameFollowedOnClosures(PropertyKeyID) const;
    FunctionID classRowNameHolder(LayoutID, PropertyKeyID) const;
    std::span<const LayoutID> rootsOfClassRows() const LIFETIME_BOUND { return m_rootsOfClassRows.span(); }
    FlowNodeID laidOnClosuresOf(FunctionID, PropertyKeyID);
    void tieNamesOnLostClosuresOf(FunctionID);
    void letLeaveWithClassObject(LayoutID classLayout, FlowNodeID node);
    void openWhatLeavesWithClassObject(LayoutID classLayout);
    std::span<const NameOnClosures> namesOnClosures() const LIFETIME_BOUND { return m_namesOnClosures.span(); }
    const NameOnPrototype* findNameOnPrototype(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    bool hasNameOnPrototypeOf(LayoutID layout) const { return m_namesOfPrototypes.contains(layout.index()); }
    enum class PrimitivesOnPrototypes : uint8_t { Count, AreIgnored, AreIgnoredWithObjects };
    bool mayLieOnPrototypeAbove(LayoutID, PropertyKeyID, bool countsKeysThatNobodyKnows = true, PrimitivesOnPrototypes = PrimitivesOnPrototypes::Count) const;

    FlowNodeID extraPropertyReadNode(LayoutID);
    FlowNodeID extraPropertyStoreNode(LayoutID);
    FlowNodeID joinedExtraProperties(LayoutID);
    void noteCopyOfAppendedFields(FunctionID, uint32_t record);
    void noteNamesOfRecordsRead(const FlowObserver&, FlowNodeID records, size_t numberOfNames);
    template<typename Functor> void forEachAppendedField(LayoutID layout, const Functor& functor) const
    {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        for (uint32_t place = m_newestAppendedFieldOfLayout[layout.index()]; place != UINT32_MAX;) {
            AppendedField appended = m_appendedFields[place];
            place = appended.next;
            functor(appended.key, appended.node);
        }
    }
    template<typename Functor> void forEachAppendedName(LayoutID layout, const Functor& functor) const
    {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        if (auto names = m_appendedNamesOfLayouts.find(layout.index()); names != m_appendedNamesOfLayouts.end()) {
            for (uint32_t place : names->value.places)
                functor(m_appendedFields[place].key, m_appendedFields[place].node);
            return;
        }
        for (uint32_t place = m_newestAppendedFieldOfLayout[layout.index()]; place != UINT32_MAX; place = m_appendedFields[place].next) {
            if (isAppendedName(place))
                functor(m_appendedFields[place].key, m_appendedFields[place].node);
        }
    }
    void trackAppendedNames(LayoutID);
    FlowNodeID unknownKeyStoreNode(LayoutID, bool keyIsNumber);
    FlowNodeID nonNumericKeyReadNode(LayoutID);
    FlowNodeID unseenReplacementNode(LayoutID);
    FlowNodeID storedWhereAccessorStandsAbove(LayoutID);
    bool tieReadUnderAffixes(FlowNodeID value, LayoutID, const KeyAffixes&);
    FlowNodeID foundBesideFieldsFor(LayoutID layout, PropertyKeyID key) { return mayBeNamedByNumber(key) ? extraPropertyReadNode(layout) : nonNumericKeyReadNode(layout); }
    bool hasUnknownKeyStores(LayoutID layout) const { return m_unknownKeyStoreNodes[layout.index()] != UINT32_MAX || m_numericKeyStoreNodes[layout.index()] != UINT32_MAX; }
    Vector<FlowNodeID, 8> nodesWithOwnValuesOf(LayoutID);
    Vector<FlowNodeID, 4> cellsOfWrittenPrivateNamesOf(LayoutID);
    bool isInspectedByComparisonOfPrivateFields(FlowNodeID) const;
    struct OwnValue {
        PropertyKeyID key;
        FlowNodeID node;
    };
    std::span<const OwnValue> ownValuesOf(LayoutID) LIFETIME_BOUND;
    void appendAppendedNames(LayoutID, Vector<OwnValue, 16>&) const;
    bool appendExportsOf(LayoutID, Vector<OwnValue, 8>&);
    std::span<const OwnValue> findOwnValues(LayoutID layout) const LIFETIME_BOUND { return layout.index() < m_ownValuesOfLayouts.size() ? m_ownValuesOfLayouts[layout.index()].span() : std::span<const OwnValue>(); }
    static constexpr size_t numberOfOwnValuesBesideFields = 2;
    std::span<const OwnValue> ownValuesThatMayHaveNoKeyOf(LayoutID) LIFETIME_BOUND;
    BitVector layoutsWhoseOwnFieldsListAccessor;
    FlowNodeID foundBesideFieldsUnder(PropertyKeyID);
    bool listMembersThatMayAnswerUnder(FamilyID, PropertyKeyID, Vector<LayoutID>&);
    struct FieldBornOnMember {
        LayoutID member;
        FlowNodeID node;
    };
    Vector<FieldBornOnMember> letKeeperHearOfFieldsBornUnder(FamilyID, PropertyKeyID, const FlowObserver& keeper);
    Vector<FieldBornOnMember> fieldsBornOnMembersUnder(FamilyID, PropertyKeyID);
    void letKeeperWatch(const FlowObserver& keeper, FlowNodeID);
    bool tellPorterThatMemberIsLost(FamilyID, LayoutID);
    bool hasMembersToResolve() const;
    void wakeEverySleeperToSeeThatNothingGrows();
    String describeSleeperThatMayFindSomething();
    std::span<const LayoutID> instancesOfClassesIn(FamilyID);
    enum class WhatSilentMemberFinds : uint8_t { Nothing, NothingThatCanBeCalled, TheUnknownAlone, SomethingOfItsOwn };
    WhatSilentMemberFinds whatSilentMemberFinds(LayoutID, PropertyKeyID calledName);
    WhatSilentMemberFinds whatPorterTakesMemberToFind(const FlowObserver& porter, LayoutID, PropertyKeyID calledName);
    FlowNodeID appendedField(LayoutID, PropertyKeyID);
    FlowNodeID appendedStoreNode(LayoutID, PropertyKeyID);
    FlowNodeID findAppendedStoreNode(LayoutID, PropertyKeyID) const;
    FlowNodeID findAppendedField(LayoutID, PropertyKeyID) const;
    bool somethingIsStoredBesideFieldsOf(LayoutID);
    void forEachAppendedField(const Invocable<void(PropertyKeyID, FlowNodeID)> auto& functor) const
    {
        for (const AppendedField& field : m_appendedFields)
            functor(field.key, field.node);
    }
    FlowNodeID foundOnClosuresOf(FunctionID);
    bool closuresInheritFunctionPrototype(FunctionID) const;
    FlowNodeID foundOnClassObjectsFrom(LayoutID);
    FlowNodeID constructorChainStoreNode(LayoutID, PropertyKeyID);
    LayoutID nearestClassWithStaticFieldThatMayMatch(LayoutID from, PropertyKeyID);
    void openClosuresOf(FunctionID);

    FlowNodeID fieldReadThrough(FamilyID, PropertyKeyID);
    enum class Lays : uint8_t { Assigns, AssignsInConstruction, DefinesInConstruction, Defines };
    FlowNodeID fieldStoredThrough(FamilyID, PropertyKeyID, Lays);
    FlowNodeID fieldReadThroughLost(LayoutID member, PropertyKeyID);
    FlowNodeID fieldStoredThroughLost(LayoutID member, PropertyKeyID, Lays);
    void letLostFamilyEscape(LayoutID member);
    FlowNodeID anyFieldReadThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber = false);
    bool hasPristinePrototypes(LayoutID) const;
    bool hasFollowedPrototypes(LayoutID) const;
    FlowNodeID literalKeyReadOf(LayoutID);
    FlowNodeID anyFieldStoredThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber = false);
    FlowNodeID elementsSeenOn(LayoutID);
    FlowNodeID anyFieldDefinedOn(LayoutID);
    FlowNodeID whatStandsAbove(LayoutID layout) { return nodeOnDemand(NodeOnDemand::WhatStandsAbove, layout.index(), { }).first; }
    FlowNodeID whatStandsAbove(SiteID site) { return nodeOnDemand(NodeOnDemand::WhatStandsAbove, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }).first; }
    FlowNodeID findWhatStandsAbove(LayoutID layout) const { return findNodeOnDemand(NodeOnDemand::WhatStandsAbove, layout.index(), { }); }
    FlowNodeID findWhatStandsAbove(SiteID site) const { return findNodeOnDemand(NodeOnDemand::WhatStandsAbove, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }); }
    FlowNodeID whatStandsBelow(LayoutID layout) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, layout.index(), { }).first; }
    FlowNodeID findWhatStandsBelow(LayoutID layout) const { return findNodeOnDemand(NodeOnDemand::WhatStandsBelow, layout.index(), { }); }
    FlowNodeID whatStandsBelow(SiteID site) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }).first; }
    static constexpr unsigned mostStepsAboveCreatedObject = 8;
    FlowNodeID findWhatConstructorLaysAbove(FunctionID, const SummaryChange&) const;
    FlowNodeID heirsThatRun(FunctionID member) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, member.index() | functionAmongNodesOnDemand, { }).first; }
    FlowNodeID findHeirsThatRun(FunctionID member) const { return findNodeOnDemand(NodeOnDemand::WhatStandsBelow, member.index() | functionAmongNodesOnDemand, { }); }
    FlowNodeID laidByDescriptorsOn(LayoutID layout, PropertyKeyID key) { return nodeOnDemand(NodeOnDemand::AnyFieldDefinedOnLayout, layout.index(), key).first; }
    FlowNodeID findLaidByDescriptorsOn(LayoutID layout, PropertyKeyID key) const { return findNodeOnDemand(NodeOnDemand::AnyFieldDefinedOnLayout, layout.index(), key); }
    bool hasSetterWritten(FieldReference field) const { return m_fieldsWithSettersWritten.get(m_firstFieldOfLayout[field.layout.index()] + field.field); }
    bool hasAccessorWritten(LayoutID layout) const { return m_layoutsWithAccessorsWritten.get(layout.index()); }
    FlowNodeID parameterOfLostFunctions(const LinkType& callee, uint32_t positionalArgument);
    FlowNodeID receiverOfLostFunctions(const LinkType& callee);
    FlowNodeID resultOfLostFunctions(const LinkType& callee);
    LinkType whatLiesAboveClasses(LayoutID, PropertyKeyID) const;
    FlowNodeID laidOnLostClosures(const LinkType& callee);
    bool lostFunctionsAreAtUnseen(const LinkType& callee) const;
    void letFamilyOfLostFunctionsEscape(FunctionID member) const;
    FlowNodeID madeByLostFunctions(const LinkType& callee);
    void noteCallOfLostFunctions(const LinkType& callee);

    ALWAYS_INLINE LinkType typeAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size())
            return { };
        LinkType type = typeOfNode(id.index());
        return m_flowIsAtRest ? typeAtRest(type) : type;
    }
    ALWAYS_INLINE bool holdsNothingAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size())
            return true;
        if (m_flowIsAtRest) [[unlikely]]
            return typeAt(id).isNever();
        return isNeverAt(id.index());
    }
    LinkType typeAtRest(const LinkType& type) const
    {
        if (ViewAtWork::current())
            return type.directPart();
        if (type.hasUnknownObjects && m_typesAreSettled && (type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None))
            return settled(type);
        return type;
    }
    void prefetchTypeAt(FlowNodeID id) const { __builtin_prefetch(&m_nodes[id.index()]); }
    bool holdsInstancesOfOneSiteAt(FlowNodeID id) const { return id.index() < m_nodes.size() && headOfNode(id.index()).classes == LinkType::Classes::Site; }
    bool holdsInstancesAt(FlowNodeID id) const { return id.index() < m_nodes.size() && headOfNode(id.index()).classes != LinkType::Classes::None; }
    bool isAccessedThroughFamilyUnder(FlowNodeID, PropertyKeyID) const;
    bool holdsLayoutsAt(FlowNodeID id) const
    {
        LinkType::Objects objects = id.index() < m_nodes.size() ? headOfNode(id.index()).objects : LinkType::Objects::None;
        return objects == LinkType::Objects::Layout || objects == LinkType::Objects::Set;
    }
    bool mayHoldListedAccessorsAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size())
            return false;
        LinkType head = headOfNode(id.index());
        if (head.objects == LinkType::Objects::None)
            return head.classes != LinkType::Classes::None || head.functions == LinkType::Functions::Closure;
        return head.objects == LinkType::Objects::Layout || head.objects == LinkType::Objects::Set || head.objects == LinkType::Objects::Family;
    }
    bool hasUnknownObjectsAt(FlowNodeID id) const { return id.index() < m_nodes.size() && headOfNode(id.index()).hasUnknownObjects; }
    FunctionID closureAloneAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size() || headOfNode(id.index()).functions != LinkType::Functions::Closure)
            return { };
        return typeAt(id).function;
    }
    void arrive(FlowNodeID, const LinkType&, PropertyKeyID symbol = { });
    PropertyKeyID symbolAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).primitives) ? symbolNotedAt(node) : PropertyKeyID(); }
    PropertyKeyID symbolSeenAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).directPrimitives()) ? symbolNotedAt(node) : PropertyKeyID(); }
    PropertyKeyID keySeenOf(FunctionID, const SummaryAccess&) const;
    static bool isSymbolAsFarAsIsSeen(const LinkType& key)
    {
        if (key.objects != LinkType::Objects::None || key.functions != LinkType::Functions::None || key.classes != LinkType::Classes::None || key.builtin || key.holdsLostBuiltins || key.hasDirectUnknownObjects())
            return false;
        return (key.directPrimitives() & LinkType::oneSymbol) && !(key.directPrimitives() & LinkType::closed(Primitive::All & ~Primitive::UndefinedOrNull) & ~LinkType::oneSymbol);
    }
    void addEdge(FlowNodeID from, FlowNodeID to, TypeID passes = { });
    void addValueEdge(FlowNodeID from, FlowNodeID to) { addEdge(from, to, TypeTable::anything); }
    void observe(FlowNodeID, const FlowObserver&);
    void run();
    void settleTypes();
    void countAllocatedMembersOfFamilies();
    FamilyID familyWhoseAllocatedMembersAre(std::span<const LayoutID>) const;
    LinkType settled(LinkType) const;
    LinkType withoutObjectsThatHaveEscaped(LinkType) const;
    Vector<BreachOfClosure> findBreachesOfClosure();
    void describe(PrintStream&, FlowNodeID) const;
    void describeLazyNode(PrintStream&, FlowNodeID) const;
    bool describeSharedCallNode(PrintStream&, FlowNodeID) const;
    size_t numberOfNodesWithLists() const { return m_listsOfNodes.size(); }
    struct NodeSubject {
        enum class Kind : uint8_t { Other, Variable, Field, Parameter, Result, Thrown, SummaryNode };
        Kind kind { Kind::Other };
        uint32_t first { 0 };
        uint32_t second { 0 };
    };
    NodeSubject subjectOfNode(FlowNodeID) const;
    size_t numberOfNodes() const { return m_nodes.size(); }
    bool flowsIntoUnseen(FlowNodeID) const;
    void forEachEdgeOutOf(FlowNodeID, NOESCAPE const ScopedLambda<void(FlowNodeID to, TypeID passes)>&) const;
    void forEachContents(NOESCAPE const ScopedLambda<void(SiteID, SlotOfContents, FlowNodeID)>&) const;
    struct SiteMaker {
        FunctionID function;
        ValueID value;
    };
    Vector<SiteMaker> findSiteMakers() const;
    Vector<SiteMaker> findMakersOf(std::span<const SiteID>) const;
    static std::optional<ValueID> valueThatMakes(const SummaryNode& classInstance);
    Vector<FlowNodeID> endsOfEdgesOutOf(FlowNodeID) const;
    Vector<FlowObserver> observersOf(FlowNodeID) const;
    SizeOfLinkFlow size() const;
    void forgetWaysOfFlow();
    class UnknownByEscapeAlone {
        WTF_MAKE_NONCOPYABLE(UnknownByEscapeAlone);
    public:
        enum class Marks : uint8_t { Nothing, OfRule, OfEscape };
        explicit UnknownByEscapeAlone(bool isSo, Marks marks = Marks::OfEscape)
            : m_outer(std::exchange(s_marksAtWork, isSo ? marks : Marks::Nothing))
        {
        }
        ~UnknownByEscapeAlone() { s_marksAtWork = m_outer; }
        static Marks marksAtWork() { return s_marksAtWork; }
    private:
        static constinit thread_local Marks s_marksAtWork;
        Marks m_outer;
    };
    static bool isUnknownByEscapeAloneIfAtAll(const LinkType& type) { return !type.hasDirectUnknownObjects(); }

    mutable std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;

    std::span<const LayoutID> layoutsOfSet(const LinkType& type) const LIFETIME_BOUND
    {
        ASSERT(type.objects == LinkType::Objects::Set);
        return m_setsOfLayouts.membersOf(type.layout.index());
    }
    std::span<const LayoutID> layoutsOfFamily(FamilyID) const LIFETIME_BOUND;
    LinkType typeOfLayouts(std::span<const LayoutID> sortedMembers) const;
    enum class LayoutsNamed : uint8_t { Exactly, AtMost };
    LayoutsNamed forEachLayoutOf(const LinkType& type, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        switch (type.objects) {
        case LinkType::Objects::None:
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Layout:
            functor(type.layout);
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Set:
            for (LayoutID member : layoutsOfSet(type))
                functor(member);
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Family:
            for (LayoutID member : layoutsOfFamily(type.family))
                functor(member);
            return LayoutsNamed::AtMost;
        case LinkType::Objects::Lost:
            if (type.layout) {
                for (LayoutID member : membersOfLostFamily(unitedLayouts.representativeWithoutShortening(type.layout.index())))
                    functor(member);
            }
            return LayoutsNamed::AtMost;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    uint32_t numberOfSetsOfLayouts() const { return m_setsOfLayouts.size(); }
    std::span<const FunctionID> functionsOfSet(const LinkType& type) const LIFETIME_BOUND
    {
        ASSERT(type.functions == LinkType::Functions::Set);
        return m_setsOfFunctions.membersOf(type.function.index());
    }
    LinkType typeOfFunctions(std::span<const FunctionID> sortedMembers) const;
    void loseFunctionsOfSet(const LinkType&) const;
    bool needsPrimitive(const FlowObserver&) const;
    bool asksEachFunction(const FlowObserver&) const;
    bool isPrototypeReadByInstanceOf(FunctionID, const SummaryAccess&) const;
    bool readsEachFunction(const SummaryAccess&) const;

    bool namesClassesOf(const LinkType& type) const { return type.classes == LinkType::Classes::Site || (type.classes == LinkType::Classes::Mixed && type.setOfClasses); }
    void forEachClassOf(const LinkType& type, NOESCAPE const Invocable<void(const LinkType& instancesOfOneClass)> auto& functor) const
    {
        ASSERT(namesClassesOf(type));
        LinkType instances = type.instancesAlone();
        if (type.classes == LinkType::Classes::Site) {
            functor(instances);
            return;
        }
        instances.classes = LinkType::Classes::Site;
        instances.setOfClasses = 0;
        for (BuiltinClassID member : m_setsOfClasses.membersOf(type.setOfClasses)) {
            instances.builtinClass = member;
            functor(instances);
        }
    }

    enum class LossAtJoin : bool { IsNotedAtOnce, WaitsForUse };
    LinkType joinOf(const LinkType&, const LinkType&, LossAtJoin = LossAtJoin::IsNotedAtOnce);
    void noteUseOf(const LinkType& type)
    {
        if (type.marks & LinkType::BuiltinsLostAtJoins)
            note(Note::BuiltinsLostAtJoinsAreUsed, 0);
    }
    ALWAYS_INLINE LinkType passedBy(const LinkType& type, TypeID test) const
    {
        if (!test)
            return type;
        return passedBySlowCase(type, test);
    }
    LinkType fromTypeID(TypeID) const;
    TypeID toTypeID(TypeArena&, const LinkType&) const;
    void nameLayoutsThatMet(TypeArena&);
    void forgetNamesThatAreNoFamilies(const Families& behindFlow);
    std::span<const LayoutSetID> layoutsThatMet() const LIFETIME_BOUND { return m_layoutsThatMet.span(); }
    bool tablesMayName(FunctionID function) const
    {
        const BitVector* functions = ViewAtWork::current() ? ViewAtWork::current()->functionsThatTablesMayName : nullptr;
        return !functions || functions->get(function.index());
    }

    void reach(FunctionID);
    void becomeLost(LayoutID);
    void becomeLost(FamilyID);
    void becomeLost(FunctionID);
    void escape(LayoutID);
    void escape(FunctionID);
    void escapeAllOf(const LinkType&);
    void arriveFromSource(FlowNodeID, LayoutID);
    void arriveFromSource(FlowNodeID, FamilyID);
    void arriveFromSource(FlowNodeID, const LinkType&);
    void arriveObjectsFromSource(FlowNodeID, const LinkType&);
    void arriveFromSource(FlowNodeID, TypeID);
    void noteAllocated(LayoutID);
    void noteConstructed(LayoutID);
    void noteAllocatedWithRowAbove(LayoutID);
    void reachEntries();
    bool enterLibraryInReach();
    void stopUnlessRecordsHandedOutFrozenStayAsBorn();
    BuiltinsInReach& builtinsInReach();
    bool takeForConstant(BuiltinID member);
    void noteLockedAbsentNames();
    class StillNothing {
    public:
        bool isOf(BuiltinID canonicalHolder, PropertyKeyID key) const { return m_holder == canonicalHolder && m_key == key; }

    private:
        friend class LinkFlow;
        StillNothing(BuiltinID canonicalHolder, PropertyKeyID key)
            : m_holder(canonicalHolder)
            , m_key(key)
        {
        }

        BuiltinID m_holder;
        PropertyKeyID m_key;
    };
    enum class LockAbsentName : bool { No, IfPossible };
    std::optional<StillNothing> stillNothing(BuiltinID canonicalHolder, PropertyKeyID key, NOESCAPE const Invocable<bool(FlowNodeID)> auto& tellAgainAt, LockAbsentName lock = LockAbsentName::No)
    {
        bool isLocked = lock == LockAbsentName::IfPossible && canLockAbsentMember(canonicalHolder, key);
        if (!isLocked && isOutOfSight(canonicalHolder) && !staysAbsentUnseen(canonicalHolder, key))
            return std::nullopt;
        for (FlowNodeID laid : { laidOnBuiltin(canonicalHolder, key), laidOnBuiltin(canonicalHolder, { }) }) {
            if (!tellAgainAt(laid) || !holdsNothingAt(laid))
                return std::nullopt;
        }
        takeForUnlaid(canonicalHolder, key, isLocked);
        noteHolderPassedInSight(canonicalHolder, key);
        return StillNothing(canonicalHolder, key);
    }
    bool followsGlobalObjectUnder(PropertyKeyID key, NOESCAPE const Invocable<bool(FlowNodeID)> auto& tellAgainAt) { return followsBuiltinUnder(globalObject(), key, tellAgainAt); }
    bool followsBuiltinUnder(BuiltinID holder, PropertyKeyID key, NOESCAPE const Invocable<bool(FlowNodeID)> auto& tellAgainAt)
    {
        if (isOutOfSight(holder))
            return false;
        FlowNodeID definedUnderAnyKey = laidOnBuiltin(holder, { });
        if (!tellAgainAt(definedUnderAnyKey) || !holdsNothingAt(definedUnderAnyKey))
            return false;
        takeForUnlaid(holder, key, false);
        noteHolderPassedInSight(holder, key);
        return true;
    }
    Vector<String> findWhatWasTakenAndNeitherHoldsNorWasTold(LinkFlow& flowThatIsAsked);
    uint32_t numberOfSymbolsThatGoByValue() const { return m_symbolsThatGoByValue.size() + m_everySymbolOfLibraryMayHavePreparation; }
    bool gathersWhatLookupsTake() const;
    const BuiltinCondition* lightFormOf(BuiltinID canonical) const;
    bool isAskedForOtherNames(BuiltinID canonical) const;
    bool isWorthFollowingToWrites(BuiltinID canonical) const;

    void assertHasWhatIsSharedToItself() const { RELEASE_ASSERT((!m_severalThreadsTell && !s_laneAtWork) || s_depthOfLockOfWhatIsShared); }
    template<typename T>
    class Shared {
    public:
        T& of(const LinkFlow& flow)
        {
            flow.assertHasWhatIsSharedToItself();
            return m_table;
        }

    private:
        T m_table;
    };
    class LockerOfWhatIsShared {
        WTF_MAKE_NONCOPYABLE(LockerOfWhatIsShared);
    public:
        explicit LockerOfWhatIsShared(const LinkFlow& flow) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
            : m_flow(flow.m_severalThreadsTell ? &flow : nullptr)
        {
            if (m_flow && !s_depthOfLockOfWhatIsShared++)
                m_flow->lockWhatIsShared();
        }
        ~LockerOfWhatIsShared() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            if (m_flow && !--s_depthOfLockOfWhatIsShared)
                m_flow->m_whatIsSharedIsLocked.store(false, std::memory_order_release);
        }

    private:
        const LinkFlow* m_flow;
    };
    enum class MadeOnDemand : uint8_t { InstanceLocks, LostName, Contents, ContentsOfFamily, TakenForConstant, FieldReadThroughLost, FieldStoredThroughLost, FieldReadThrough, FieldStoredThrough, FoundOnPrototypesAbove, FoundBesideFieldsUnder, FoundWhereFieldIsEmpty, AppendedField, AppendedStoreNode, FoundOnClosuresOf, FoundOnClassObjectsFrom, LaidOnClosuresOf, WrittenAtMember, WhereLossIsTold, ParameterOfLostFunctions, ReceiverOfLostFunctions, ResultOfLostFunctions, LaidOnLostClosures, MadeByLostFunctions, NameAddedByTests, FunctionOnObjectPrototype, AnyFieldDefinedOn, AnyFieldReadThrough, AnyFieldStoredThrough, LiteralKeyRead, ResultOfOperator };
    static constexpr size_t numberOfKindsMadeOnDemand = static_cast<size_t>(MadeOnDemand::ResultOfOperator) + 1;
    uint32_t askedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<uint32_t()> auto& make) const
    {
        if (!m_severalThreadsTell)
            return make();
        if (std::optional<uint32_t> known = findWhatLaneWasGiven(kind, words))
            return *known;
        if (std::optional<uint32_t> known = findWhatSomeLaneWasGiven(kind, words)) {
            keepWhatLaneWasGiven(kind, words, *known);
            return *known;
        }
        uint32_t made;
        {
            LockerOfWhatIsShared locker(*this);
            made = make();
        }
        keepWhatLaneWasGiven(kind, words, made);
        keepWhatSomeLaneWasGiven(kind, words, made);
        return made;
    }
    FlowNodeID nodeAskedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<FlowNodeID()> auto& make) const
    {
        return FlowNodeID::fromBits(askedOnceByLane(kind, words, [&]() -> uint32_t {
            return make().bits();
        }));
    }
    std::optional<uint32_t> findWhatLaneWasGiven(MadeOnDemand, uint64_t words) const;
    void keepWhatLaneWasGiven(MadeOnDemand, uint64_t words, uint32_t) const;
    std::optional<uint32_t> findWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words) const;
    void keepWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words, uint32_t) const;
    void forgetWhatSomeLaneWasGiven(MadeOnDemand);
    enum class Note : uint8_t { NameRead, NameCalled, ReadUnderKeyNobodyKnows, ReadUnderKeyNobodyKnowsOffHolder, ReadUnderNameThatLoopGave, BuiltinRead, BuiltinCalled, BuiltinCalledOutOfSight, FieldWrittenAfterConstruction, LayoutAllocated, ClassConstructed, ChangeOfLayout, WriteOnBuiltin, BuiltinHandedWhatIsNoTextNumberOrBoolean, WriteThroughLostBuiltins, ReadThroughLostBuiltins, WriteThroughUnseenByProgram, NameWrittenThroughUnseenByLibrary, WriteThroughUnseenInSight, OptionThatRunsCodePassed, OperandMayMeetCondition, ObjectHandedToFormatter, TypeHandedToHandler, BuiltinReadThroughAggregate, ExportIsShown, BuiltinCalledAtSite, LostFunctionCalled, LostFunctionsCalledByFamily, EveryLostFunctionCalled, LayoutGainsWhatUnseenCodeGaveSources, LayoutDefinedOnUnderWrittenKeys, LayoutDefinedOnUnderKeyThatIsNotWritten, LayoutGivenAccessorByDescriptorInSight, FieldGivenAccessorByDescriptorInSight, FieldWithUnseenAccessor, LayoutGetsNullAbove, LayoutDeletedUnderUnknownKey, FieldDeletedByName, PrototypeLaidAboveOtherObjects, FieldMayBeEmpty, PrototypeObjectRead, NameReadWithAnswerInSight, BuiltinsReadByNameAreUsed, BuiltinsLostAtJoinsAreUsed, InstanceMadeOutOfSight, InstanceHandedToFormatter, RowApplied, CallLeftToUnseen, InstancesLoseTheirConstructor, BuiltinUsedViaFlow, ReadOfProgramUnderKeyNobodyKnows, BuiltinStandsAboveClass, BuiltinFoundWhereFlowBroughtHolder, BuiltinCalledWithOperandsNobodyRead, NameCalledByNativeCodeOffUnseenObjects };
    void note(Note, uint32_t number, uint32_t more = 0);
    void noteFieldGivenAccessorByDescriptorInSight(FieldReference);
    void noteFieldWithUnseenAccessor(FieldReference);
    void noteDeletion(LayoutID, PropertyKeyID);
    void takeOverCallsWhoseRowsLinkTells();
    static constexpr uint32_t byCode = 1;
    static constexpr uint32_t byRowThatMayRunCode = 2;
    static constexpr uint32_t asConstructor = 4;
    static constexpr uint32_t withoutRow = 1;
    static constexpr uint32_t spelledByProgram = 1;
    static constexpr uint32_t passesNoOperand = 2;
    static constexpr uint32_t walkedByFormatter = 0;
    static constexpr uint32_t printedByCodeOfItsClass = 1;
    static constexpr uint32_t onlyDeletes = 1 << 16;
    unsigned numberOfLanes() const { return static_cast<unsigned>(m_lanes.size()); }
    bool bringAliveOnLanes(std::span<const FunctionID>, const ScopedLambda<void(FunctionID)>& bringAlive);
    bool takeStepsOfReachOnLanes(size_t numberOfSteps, const ScopedLambda<void(unsigned lane)>& takeSteps);
    bool severalThreadsTell() const { return m_severalThreadsTell; }
    bool lanesTakeStepsOfReach() const { return m_lanesTakeStepsOfReach && m_severalThreadsTell; }
    struct LeftToReach {
        uint8_t what;
        uint32_t index;
    };
    void leaveToReach(uint8_t what, uint32_t index);
    Vector<LeftToReach> takeWhatLaneLeftToReach(unsigned lane);
    bool listsSourceOfObjectsBehindPass(FlowNodeID, LayoutID);
    bool tellsBuiltinsInReachBehindPass(FunctionID);
    void listSourceOfObjects(FlowNodeID, LayoutID);
    bool letsReachWait() const { return m_letsReachWait; }
    unsigned laneOfObserversOf(FunctionID function) const { return laneIndexOf(FlowNodeID(m_nodesOfFunctions[function.index()].firstParameter)); }
    unsigned indexOfLaneAtWork() const;
    const LinkType& typeObservedBefore() const LIFETIME_BOUND;
    class CodeAtWork {
        WTF_MAKE_NONCOPYABLE(CodeAtWork);
    public:
        CodeAtWork(LinkFlow& flow, FunctionID function)
            : m_flow(flow)
            , m_wasOfLibrary(flow.setIsInLibraryCode(flow.m_isFunctionOfLibrary.quickGet(function.index())))
        {
        }
        ~CodeAtWork() { m_flow.setIsInLibraryCode(m_wasOfLibrary); }

    private:
        LinkFlow& m_flow;
        bool m_wasOfLibrary;
    };
    bool setIsInLibraryCode(bool);
    bool isInLibraryCode() const;
    bool rowsSpeakFor(FunctionID function) const { return m_isFunctionThatRowsSpeakFor.quickGet(function.index()); }
    bool rowsSpeakOfKeyFor(FunctionID function, PropertyKeyID key) const { return rowsSpeakFor(function) && !m_keysThatCodeTellsOfItself.contains(key); }
    bool objectsReachUnseen() const { return m_objectsReachUnseen; }
    void noteRowPassesWhatNobodyFollows() { raise(m_rowPassesWhatNobodyFollows); }
    bool rowPassesWhatNobodyFollows() const { return m_rowPassesWhatNobodyFollows; }
    void noteListenerMayReceivePromises() { raise(m_listenerMayReceivePromises); }
    bool listenerMayReceivePromises() const { return m_listenerMayReceivePromises; }
    bool letPromisesBeHandedOut();
    void noteLossOf(BuiltinID builtin) const
    {
        if (builtin)
            noteLossOfBuiltin(builtin);
    }
    void noteLossOfObjectOfBuiltinModule(ModuleID) const;
    void noteLossOfExportOfBuiltinModule(ModuleID, StringID name) const;
    void noteKeysBecomeTexts(LayoutID layout)
    {
        if (m_keysOfLayoutAreTold.at(layout.index()) != nothingIsLeftToTell) [[unlikely]]
            tellKeysThatBecomeTexts(layout);
    }
    void noteKeysBecomeTextsAmong(const LinkType& type)
    {
        if (type.objects == LinkType::Objects::Layout)
            noteKeysBecomeTexts(type.layout);
        else if (type.objects == LinkType::Objects::Set) [[unlikely]]
            noteKeysBecomeTextsAmongSet(type);
    }
    void noteKeysBecomeTextsAmongSet(const LinkType&);
    void noteNameBecomesText(FunctionID function)
    {
        if (m_nameOfFunctionIsTold.at(function.index()) != nothingIsLeftToTell) [[unlikely]]
            tellNameThatBecomesText(function);
    }
    void noteNamesOfLostFunctionsBecomeTexts();
    void noteLossOfLightFormAmong(const LinkType& type) const
    {
        if (type.builtin && lightFormOf(type.builtin))
            noteLossOfBuiltin(type.builtin);
    }
    Vector<BuiltinID> takeBuiltinsLost()
    {
        if (hasBuiltinsLostAtJoinsToLetGo())
            m_builtinsLost.appendVector(std::exchange(m_builtinsLostAtJoins, { }));
        return std::exchange(m_builtinsLost, { });
    }
    bool hasBuiltinsLostAtJoinsToLetGo() const { return (m_marksThatAreUsed | m_marksThatReachUnseen) & LinkType::BuiltinsLostAtJoins && !m_builtinsLostAtJoins.isEmpty(); }
    Vector<BuiltinID> takeBuiltinsLostInSight() { return std::exchange(m_builtinsLostInSight, { }); }
    Vector<BuiltinID> takeBuiltinsAtUnseen() { return std::exchange(m_builtinsAtUnseen, { }); }
    void handToAccessorsLaidAbove(FlowNodeID receiver);
    bool letGoWhoWaitsForAccessorsLaidAbove();
    void layAbovePrototype(FlowNodeID operand);
    bool hasSomethingLaidAbovePrototype() const { return !m_operandsLaidAbovePrototype.isEmpty(); }
    void noteWhatIsLaidAbovePrototype();
    bool everythingLaidAbovePrototypeMayWait(std::span<const BuiltinID> holdersLinkedAnew) const;
    void letGoWhatIsLaidAbovePrototype();
    bool lostBuiltinsReachUnseen() const { return m_lostBuiltinsReachUnseen; }
    bool builtinsReadByNameReachUnseen() const { return m_marksThatReachUnseen & LinkType::BuiltinsReadByName; }
    bool builtinsReadByNameAreUsed() const { return m_marksThatAreUsed & LinkType::BuiltinsReadByName; }
    bool builtinsLostAtJoinsReachUnseen() const { return m_marksThatReachUnseen & LinkType::BuiltinsLostAtJoins; }
    bool everyMarkIsUsed() const { return m_marksThatAreUsed == LinkType::everyMark; }
    bool answerOfReadStaysInSight(PropertyKeyID) const;
    void noteAccessorMayBeLaidOn(const LinkType& subject, PropertyKeyID, FunctionID writer);
    LinkType anythingBeside(BuiltinID canonical, FunctionID reader) const;
    LinkType unseenObjectsBelow(BuiltinID canonicalPrototype, FunctionID maker) const;
    enum class WhatIsStored : uint8_t { Anything, TextsNumbersAndBooleans, Nothing };
    void noteWriteOn(const LinkType& subject, PropertyKeyID, FunctionID writer, WhatIsStored = WhatIsStored::Anything, bool subjectIsMadeInBody = false, bool isStoreByName = false);
    BuiltinID globalObject() const;
    bool isGlobalObject(BuiltinID) const;
    void noteWhatIsStoredOn(const LinkType& subject, WhatIsStored);
    void noteWhatMayMakeThenableOf(const LinkType& subject, FunctionID writer, std::optional<PropertyKeyID> writtenUnder, bool subjectMayBeWhatNobodyKnows = true, bool subjectIsMadeInBody = false);
    FlowNodeID whereInstancesAreMadeThenables();
    FlowNodeID whereInstancesLoseTheirConstructor();
    void noteWhatNativeCodeHandsToWhatIsLaidOn(BuiltinID written, PropertyKeyID) const;
    void findWhatNativeCodeHandsToLibrary();
    Vector<std::pair<BuiltinID, BuiltinID>> m_holdersAtOrAboveWhatNativeCodeHandsToLibrary;
    struct HolderPassedInSight {
        BuiltinID holder;
        PropertyKeyID key;

        friend bool operator==(const HolderPassedInSight&, const HolderPassedInSight&) = default;
    };
    Vector<HolderPassedInSight> m_holdersPassedInSight;
    BitVector m_membersThatLookupsTookForConstants;
    void noteSiteOfWrite(FunctionID writer, ValueID site, FlowNodeID subject);
    void noteSiteOfCopy(FunctionID writer, ValueID site, FlowNodeID subject, FlowNodeID source);
    uint32_t sourceOfCopySpelledByLoop(FunctionID, ValueID site) const;
    bool followsCopySpelledByLoop(const LinkType& target, const LinkType& source) const;
    bool holdsOnlyPlainRecords(const LinkType&) const;
    bool findsPlainProperties(FunctionID constructor) const;
    bool assignsNamesOfRecords(const LinkType& target, const LinkType& source);
    bool assignedNamesOfRecords(const LinkType& target, const LinkType& source) const;
    void noteLateFieldStore(FieldReference field) { note(Note::FieldWrittenAfterConstruction, m_firstFieldOfLayout[field.layout.index()] + field.field); }
    Vector<WriteFollowedToBuiltin> takeWritesFollowedToBuiltins();
    bool appendNamesOfObjectsAmong(const LinkType&, Vector<PropertyKeyID>&) const;
    LinkType builtinsAbove(const LinkType&, FunctionID asker);
    BuiltinID prototypeOfInstancesOf(BuiltinClassID) const;
    bool isClimbedFrom(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].isClimbedFrom; }
    FlowNodeID writtenAtMember(BuiltinID member);
    FlowNodeID whereLossIsTold(LayoutID);
    FlowNodeID heldByGlobalObject(PropertyKeyID key) { return nodeOnDemand(NodeOnDemand::FoundBesideFieldsUnderKey, globalObjectAmongNodesOnDemand, key).first; }
    FlowNodeID heldByBuiltin(BuiltinID holder, PropertyKeyID key) { return isGlobalObject(holder) ? heldByGlobalObject(key) : nodeOnDemand(NodeOnDemand::FoundBesideFieldsUnderKey, holder.index() | builtinAmongNodesOnDemand, key).first; }
    FlowNodeID laidOnBuiltin(BuiltinID holder, PropertyKeyID key) { return nodeOnDemand(NodeOnDemand::LaidOnClosuresUnderKey, holder.index() | builtinAmongNodesOnDemand, key).first; }
    FlowNodeID findLaidOnBuiltin(BuiltinID holder, PropertyKeyID key) const { return findNodeOnDemand(NodeOnDemand::LaidOnClosuresUnderKey, holder.index() | builtinAmongNodesOnDemand, key); }
    FlowNodeID findWrittenAtMember(BuiltinID member) const;
    TestRunsInLink& testRuns() { return *m_testRuns; }
    const TestRunsInLink& testRuns() const { return *m_testRuns; }

    BitVector reached;
    enum class FoundByCalls : uint8_t { IsIterationCallback = 1 << 0, IsRepeatedCallback = 1 << 1, IsTrapOfProxy = 1 << 2, MakesProxies = 1 << 3 };
    void noteFunctionFoundByCalls(FunctionID function, FoundByCalls found) { m_foundByCalls.add(function.index(), static_cast<uint8_t>(found)); }
    OptionSet<FoundByCalls> foundByCalls(FunctionID function) const { return OptionSet<FoundByCalls>::fromRaw(m_foundByCalls.at(function.index())); }
    BitVector allocatedLayouts;
    BitVector constructedClasses;
    BitVector lostLayouts;
    UnitedFamilies unitedLayouts;
    BitVector lostFunctions;
    mutable UnitedFamilies unitedFunctions;
    BitVector escapedLayouts;
    BitVector prototypesLaidAboveOtherObjects;
    Vector<TaggedMaker> taggedMakers;

    struct CopyLookedAt {
        LinkType subject;
        LinkType other;
        bool waitsForNothingBesideFields { false };
        bool waitsForWhatIsSeenAlone { false };
    };
    struct CopiesObserved {
        HashMap<uint64_t, CopyLookedAt, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> lookedAt;
        HashSet<std::pair<uint64_t, uint32_t>> toldOfWhatLiesBesideFields;
        HashSet<std::pair<uint64_t, uint64_t>, DefaultHash<std::pair<uint64_t, uint64_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> namesStoredOnPrototypes;
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> accessorsThatWait;
        std::array<HashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>, 4> layoutsTiedByCopies;
    };
    CopiesObserved& copiesObservedIn(FunctionID);
    bool storesCopiedNamesOnPrototype(const FlowObserver&, const LinkType& target, const LinkType& source, CopiesObserved&);
    bool tellsObserversOnceMore() const { return m_breachesOfClosure; }
    void noteRenotifiedObserver(const FlowObserver& observer) { m_observerToldOnceMore = observer; }

    void noteAccessorLaid(LayoutID, const AccessorLaidInField&);
    BitVector escapedFunctions;
    mutable BitVector layoutsMetInFamilies;
    bool mayBeKnownByFamily(LayoutID layout) const { return layoutsMetInFamilies.get(layout.index()) || lostLayouts.get(layout.index()); }
    void noteSomethingBefell(FamilyID family) const { m_outermostFamiliesWhereSomethingBefell.quickSet(m_outermostFamilies[family.index()].index()); }
    BitVector escapedVariables;
    BitVector variablesStoredFirst;
    Vector<LinkedVariableStoredFirst> firstReadersOfVariablesStoredFirst;
    Vector<SummaryClassInVariable> classesInVariables;
    Vector<SummaryUnitInVariable> unitsInVariables;
    Vector<LinkedMethodOnPrototype> methodsOnPrototypes;
    std::optional<BitVector> m_membersOfProgramObjects;
    Vector<LinkedMethodStoredFirst> methodsStoredFirst;
    Vector<SummaryStoreOnPrototype> storesBeforeInstances;
    BitVector libraryFunctionsEntered;
    BitVector exportsShown;
    BitVector modulesWhoseExportHostTakesByCallInSight;
    void showExport(ModuleID, uint32_t index);
    void noteExportRead(ModuleID, uint32_t index);

    void callObserved(const FlowObserver&, const LinkType& told, const LinkType& before);
    bool resolveNotifiedCalls();
    void inspectionObserved(const FlowObserver&, const LinkType&);
    void accessObserved(const FlowObserver&, const LinkType&);
    PropertyKeyID keyOf(FunctionID, const SummaryAccess&) const;
    struct ComputedStoreNames {
        Vector<PropertyKeyID, 8> names;
        bool areAllOwnNames { false };
    };
    ComputedStoreNames findComputedStoreNames(FunctionID, ValueID site) const;
    bool appendCallbackKeys(FunctionID callback, const BodyIndex&, ValueID key, Vector<StringID, 8>& keys) const;
    bool isPlainCallback(FunctionID callback, const BodyIndex&) const;
    Vector<PropertyKeyID, 8> findNamesOfStore(FunctionID, const SummaryAccess&) const;
    Vector<PropertyKeyID, 8> findAffixedNamesOfStore(FunctionID, const SummaryAccess&) const;
    bool appendTextsOfKey(FunctionID, const BodyIndex&, ValueID key, Vector<StringID, 8>&) const;
    bool appendArrayParameterStrings(FunctionID, const BodyIndex&, ValueID list, Vector<StringID, 8>&) const;
    struct CallsInSight {
        FunctionID caller;
        Vector<std::pair<ValueID, uint32_t>> calls;
    };
    struct CallsInBody {
        HashMap<uint32_t, Vector<ValueID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sitesOfOnlyCalled;
    };
    using ClosuresInBody = Vector<std::pair<FunctionID, ValueID>>;
    FunctionID functionKeptBy(VariableID) const;
    enum class SitesFound : uint8_t { All, None, MustBeSought };
    SitesFound appendOnlySites(const BodyIndex&, FunctionID callee, ValueID closure, Vector<ValueID, 8>&) const;
    std::unique_ptr<CallsInBody> listCallsInBody(FunctionID caller) const;
    const CallsInBody& callsInBody(FunctionID caller) const LIFETIME_BOUND;
    std::unique_ptr<ClosuresInBody> listClosuresInBody(FunctionID maker) const;
    const ClosuresInBody& closuresInBody(FunctionID maker) const LIFETIME_BOUND;
    std::span<const BlockID> blocksBehindTestsByCalls(FunctionID, const BodyIndex&) const LIFETIME_BOUND;
    bool isExportedVariable(VariableID) const;
    std::unique_ptr<CallsInSight> listCallsInSight(FunctionID) const;
    const CallsInSight& findCallsInSight(FunctionID) const LIFETIME_BOUND;
    std::optional<uint32_t> findPassedPosition(FunctionID, const BodyIndex&, ValueID) const;
    StringID findPassedText(const CallsInSight&, size_t call, uint32_t position) const;
    FlowNodeID findPassedNode(FunctionID callee, const CallsInSight&, size_t call, uint32_t position) const;
    bool appendTextsOfParameter(FunctionID, const BodyIndex&, ValueID key, Vector<StringID, 8>&) const;
    bool readsUnderIndex(FunctionID, BodyValue element) const;
    bool appendTextsOfElement(FunctionID, const BodyIndex&, BodyValue element, Vector<StringID, 8>&) const;
    bool onlyReadsList(FunctionID, ValueID list, VariableID) const;
    bool isListVariableOnlyRead(VariableID) const;
    void listNamersOfVariables();
    bool appendReadListTexts(FunctionID, const BodyIndex&, ValueID list, Vector<StringID, 8>&) const;
    struct ReadList {
        FunctionID owner;
        ValueID list;
    };
    ReadList findReadList(FunctionID, const BodyIndex&, ValueID list) const;
    ReadList findReadListSlow(FunctionID, const BodyIndex&, VariableID, ValueID list) const;
    ValueID findCallbackReadList(FunctionID maker, const BodyIndex&, ValueID site, ValueID closure) const;
    bool appendNamesOfTexts(std::span<const StringID>, Vector<PropertyKeyID, 8>&) const;
    ValueID findCallbackList(FunctionID callback, FunctionID& maker) const;
    ValueID findCallbackListSlow(FunctionID callback, FunctionID& maker) const;
    std::optional<uint32_t> findCallbackParameterArray(FunctionID, uint32_t callbackParameter) const;
    struct ListedObject {
        FunctionID function;
        ValueID object;
        uint32_t node { 0 };
        bool givesPairs { false };
        bool isLoop { false };
    };
    std::optional<ListedObject> findListedObject(FunctionID, const BodyIndex&, ValueID key) const;
    bool onlyReadsKeys(FunctionID, const BodyIndex&, ValueID list) const;
    bool storesUnderListedNames(FunctionID, ValueID site) const;
    enum class PrototypeNames : bool { Excluded, Included };
    bool appendPrototypeNames(LayoutID, Vector<PropertyKeyID, 16>&) const;
    bool appendListedNames(const LinkType& objects, PrototypeNames, Vector<PropertyKeyID, 16>&, size_t* numberOfNames = nullptr) const;
    bool isBehindOwnNameTest(FunctionID, const BodyIndex&, ValueID site, ValueID object, ValueID name, const LinkType& objects, Vector<FlowNodeID, 4>& watched) const;
    Vector<PropertyKeyID, 16> findListedNamesOfStore(FunctionID, const SummaryAccess&) const;
    bool iteratesListsAsBuilt() const;
    FunctionID findOnlyReferrer(FunctionID) const;
    bool everySymbolOfLibraryMayHavePreparation() const { return m_everySymbolOfLibraryMayHavePreparation; }
    Vector<PropertyKeyID> symbolsWithPreparation() const;
    void noteSymbolHandedToPreparation(FunctionID caller, FlowNodeID symbol, FlowNodeID function);
    enum class UnknownSymbolIs : uint8_t { UnknownInSightToo, UnknownByEscapeAlone, UnknownInSightAlone };
    void noteUnknownSymbolHandedToPreparation(UnknownSymbolIs = UnknownSymbolIs::UnknownInSightToo);
    void preparationObserved(const FlowObserver&, const LinkType&);
    void noteKeyOfAccessInLibrary(FunctionID, uint32_t record, const SummaryAccess&);
    void changeObserved(const FlowObserver&, const LinkType&);
    void passedToBeNamedObserved(const FlowObserver&, const LinkType&);
    void prototypeHelperCallObserved(const FlowObserver&);
    bool isPlainClosureMadeWith(FunctionID, PropertyKeyID) const;
    bool isClassObjectMadeWith(FunctionID, PropertyKeyID) const;
    bool isWatchedForFirstTime(FlowNodeID, const FlowObserver&);
    bool isDeferringUpdates() const { return m_isDeferringUpdates; }
    bool arrivalOfUnseenObjectsInSightBringsNothingTo(FlowNodeID node) const { return m_isDeferringUpdates && node != unseen() && typeAt(node).hasDirectUnknownObjects(); }
    bool arrivalOfAnythingInSightBringsNothingTo(FlowNodeID) const;
    bool descriptorsInSightLayValuesAlone(const LinkType& descriptors, const FlowObserver* toldAgain, Vector<FlowNodeID, 4>& values);
    bool isPlainClosureOrConstructorFunction(FunctionID) const;
    void operatorObserved(const FlowObserver&, const LinkType&);
    std::optional<uint16_t> primitivesThatOperatorGivesWhateverArrives(FunctionID, uint32_t record);

    struct ListsOfCalleesOnHeap {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ListsOfCalleesOnHeap);
        Vector<FunctionID, 4> listed;
        Vector<BuiltinID, 4> builtins;
        Vector<uint32_t, 2> shared;
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> listedToLookUp;
    };
    struct InlineRoomOfCalleesOfCall {
        union {
            std::array<FunctionID, 2> inlineListed { };
            ListsOfCalleesOnHeap* listsOnHeap;
        };
        std::array<uint32_t, 1> inlineShared { };
        std::array<BuiltinID, 1> inlineBuiltins { };
        bool mayEnterLost : 1 { false };
        bool mayEnterUnseen : 1 { false };
        bool liesOnHeap : 1 { false };
        bool followsWholeResults : 1 { false };
        uint8_t numberOfInlineListed : 2 { 0 };
        uint8_t numberOfInlineBuiltins : 1 { 0 };
        uint8_t numberOfInlineShared : 1 { 0 };
    };
    class CalleesOfCall : private InlineRoomOfCalleesOfCall {
    public:
        template<typename Member>
        class List {
        public:
            explicit List(std::span<const Member> members)
                : m_members(members)
            {
            }

            auto begin() const { return m_members.begin(); }
            auto end() const { return m_members.end(); }
            size_t size() const { return m_members.size(); }
            bool isEmpty() const { return m_members.empty(); }
            bool contains(Member member) const { return std::ranges::contains(m_members, member); }
            Member operator[](size_t index) const
            {
                RELEASE_ASSERT(index < m_members.size());
                return m_members[index];
            }
            std::span<const Member> span() const { return m_members; }

        private:
            std::span<const Member> m_members;
        };

        using InlineRoomOfCalleesOfCall::mayEnterLost;
        using InlineRoomOfCalleesOfCall::followsWholeResults;
        using InlineRoomOfCalleesOfCall::mayEnterUnseen;

        CalleesOfCall() = default;
        CalleesOfCall(const CalleesOfCall& other)
            : InlineRoomOfCalleesOfCall(other)
        {
            if (liesOnHeap)
                listsOnHeap = new ListsOfCalleesOnHeap(*other.listsOnHeap);
        }
        CalleesOfCall(CalleesOfCall&& other)
            : InlineRoomOfCalleesOfCall(std::exchange<InlineRoomOfCalleesOfCall>(other, { }))
        {
        }
        ~CalleesOfCall()
        {
            if (liesOnHeap)
                delete listsOnHeap;
        }
        CalleesOfCall& operator=(CalleesOfCall other)
        {
            std::swap<InlineRoomOfCalleesOfCall>(*this, other);
            return *this;
        }

        List<FunctionID> listed() const LIFETIME_BOUND { return List<FunctionID>(liesOnHeap ? listsOnHeap->listed.span() : std::span<const FunctionID>(inlineListed).first(numberOfInlineListed)); }
        List<BuiltinID> builtins() const LIFETIME_BOUND { return List<BuiltinID>(liesOnHeap ? listsOnHeap->builtins.span() : std::span<const BuiltinID>(inlineBuiltins).first(numberOfInlineBuiltins)); }
        List<uint32_t> shared() const LIFETIME_BOUND { return List<uint32_t>(liesOnHeap ? listsOnHeap->shared.span() : std::span<const uint32_t>(inlineShared).first(numberOfInlineShared)); }

        bool lists(FunctionID callee) const
        {
            if (liesOnHeap && !listsOnHeap->listedToLookUp.isEmpty())
                return listsOnHeap->listedToLookUp.contains(callee.index());
            return listed().contains(callee);
        }
        void list(FunctionID callee)
        {
            if (!liesOnHeap && numberOfInlineListed < inlineListed.size()) {
                inlineListed[numberOfInlineListed++] = callee;
                return;
            }
            ListsOfCalleesOnHeap& lists = ensureListsOnHeap();
            lists.listed.append(callee);
            if (!lists.listedToLookUp.isEmpty())
                lists.listedToLookUp.add(callee.index());
            else
                lookUpListedOnceTheyAreMany(lists);
        }
        void add(BuiltinID builtin)
        {
            if (builtins().contains(builtin))
                return;
            if (!liesOnHeap && numberOfInlineBuiltins < inlineBuiltins.size()) {
                inlineBuiltins[numberOfInlineBuiltins++] = builtin;
                return;
            }
            ensureListsOnHeap().builtins.append(builtin);
        }
        void addShared(uint32_t index)
        {
            if (shared().contains(index))
                return;
            if (!liesOnHeap && numberOfInlineShared < inlineShared.size()) {
                inlineShared[numberOfInlineShared++] = index;
                return;
            }
            ensureListsOnHeap().shared.append(index);
        }

        void removeListedMatching(NOESCAPE const Invocable<bool(FunctionID)> auto& matches)
        {
            if (liesOnHeap) {
                listsOnHeap->listed.removeAllMatching(matches);
                listsOnHeap->listedToLookUp.clear();
                lookUpListedOnceTheyAreMany(*listsOnHeap);
                return;
            }
            uint8_t numberKept = 0;
            for (FunctionID callee : listed()) {
                if (!matches(callee))
                    inlineListed[numberKept++] = callee;
            }
            numberOfInlineListed = numberKept;
        }
        void clearBuiltins()
        {
            if (liesOnHeap)
                listsOnHeap->builtins.clear();
            numberOfInlineBuiltins = 0;
        }
        void clearShared()
        {
            if (liesOnHeap)
                listsOnHeap->shared.clear();
            numberOfInlineShared = 0;
        }
        void replaceShared(std::span<const uint32_t> indexes)
        {
            clearShared();
            for (uint32_t index : indexes)
                addShared(index);
        }
        ALWAYS_INLINE void sortListedAndBuiltins()
        {
            if (liesOnHeap || numberOfInlineListed > 1)
                sortListedAndBuiltinsSlowCase();
        }

        NEVER_INLINE void sortListedAndBuiltinsSlowCase()
        {
            if (liesOnHeap) {
                std::ranges::sort(listsOnHeap->listed);
                std::ranges::sort(listsOnHeap->builtins);
                return;
            }
            if (numberOfInlineListed > 1)
                std::ranges::sort(std::span(inlineListed).first(numberOfInlineListed));
        }

    private:
        static constexpr size_t fewestListedThatAreLookedUp = 16;
        static void lookUpListedOnceTheyAreMany(ListsOfCalleesOnHeap& lists)
        {
            if (lists.listed.size() < fewestListedThatAreLookedUp)
                return;
            for (FunctionID callee : lists.listed)
                lists.listedToLookUp.add(callee.index());
        }
        NEVER_INLINE ListsOfCalleesOnHeap& ensureListsOnHeap()
        {
            if (liesOnHeap)
                return *listsOnHeap;
            auto* lists = new ListsOfCalleesOnHeap;
            lists->listed.append(listed().span());
            lists->builtins.append(builtins().span());
            lists->shared.append(shared().span());
            numberOfInlineListed = 0;
            numberOfInlineBuiltins = 0;
            numberOfInlineShared = 0;
            listsOnHeap = lists;
            liesOnHeap = true;
            return *lists;
        }
    };
    static_assert(sizeof(CalleesOfCall) == 16);
    Vector<uint32_t> firstCallOfFunction;
    LargeList<CalleesOfCall> calleesOfCalls;
    Vector<CalleesOfCall> calleesOfSharedCalls;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> callsOfBuiltinsTold;
    struct ViewOfHeldReading {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ViewOfHeldReading);
        ~ViewOfHeldReading();
        BitVector escapedLayouts;
        std::span<CalleesOfCall> calleesOfCalls;
        Vector<CalleesOfCall> calleesOfSharedCalls;
        const BitVector* functionsThatTablesMayName { nullptr };
        std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;
    };
    std::unique_ptr<ViewOfHeldReading> makeViewOfHeldReading();
    class ViewAtWork {
        WTF_MAKE_NONCOPYABLE(ViewAtWork);
    public:
        explicit ViewAtWork(ViewOfHeldReading* view)
            : m_outer(static_cast<ViewOfHeldReading*>(std::exchange(t_whatHelpersInherit, view)))
        {
        }
        ~ViewAtWork() { t_whatHelpersInherit = m_outer; }
        static ViewOfHeldReading* current() { return static_cast<ViewOfHeldReading*>(t_whatHelpersInherit); }
    private:
        ViewOfHeldReading* m_outer;
    };
    const ParallelLoops& loopsOfReading() const { return loops.ofCurrentThread(); }
    WhatUnseenCodeDoes reading() const { return ViewAtWork::current() ? WhatUnseenCodeDoes::OnlyReadsAndCalls : WhatUnseenCodeDoes::Anything; }
    const BitVector& escapedLayoutsAsRead() const { return ViewAtWork::current() ? ViewAtWork::current()->escapedLayouts : escapedLayouts; }
    std::span<const CalleesOfCall> calleesOfCallsAsSettled() const { return ViewAtWork::current() ? std::span<const CalleesOfCall>(ViewAtWork::current()->calleesOfCalls) : calleesOfCalls.span(); }
    const Vector<CalleesOfCall>& calleesOfSharedCallsAsSettled() const { return ViewAtWork::current() ? ViewAtWork::current()->calleesOfSharedCalls : calleesOfSharedCalls; }
    std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts>& writerOfLayoutsOfReading() const { return ViewAtWork::current() ? ViewAtWork::current()->writerOfLayouts : writerOfLayouts; }
    struct SubjectOfSharedCall {
        FamilyID family;
        PropertyKeyID key;
        uint32_t form { 0 };

        friend constexpr auto operator<=>(const SubjectOfSharedCall&, const SubjectOfSharedCall&) = default;
    };
    Vector<SubjectOfSharedCall> subjectsOfSharedCalls;
    FlowNodeID makeSharedNode() { return makeNode(); }
    void settleSharedCalls() { settleSharedCallsAs(WhatUnseenCodeDoes::Anything); }
    void settleSharedCallsAs(WhatUnseenCodeDoes);
    void enter(FunctionID caller, uint32_t callInSummary, FunctionID callee);

    Vector<OptionSet<LayoutChange>> possibleChanges;
    Vector<OptionSet<LayoutChange>> changesByMoreThanStoresInSight;
    static constexpr uint32_t changeIsByStoreInSight = 1u << 8;
    Vector<AccessorLaidInField> accessorsLaidInFields;
    BitVector functionsEnteredAsAccessors;
    void noteEnteredAsAccessor(FunctionID);
    BitVector layoutsWithKnownAccessors;
    BitVector keysFilledOnObjectsBeingMade;
    BitVector layoutsDefinedOnUnderWrittenKeys;
    BitVector layoutsDefinedOnUnderKeysThatAreNotWritten;
    BitVector layoutsGivenAccessorsByDescriptorsInSight;
    BitVector layoutsThatGetNullAbove;
    BitVector layoutsDeletedUnderUnknownKeys;
    BitVector fieldsDeletedByName;
    BitVector layoutsWhoseInstancesAreLaidAsPrototypes;
    BitVector functionsThatReadPrototypeObjects;
    Vector<FieldReference> fieldsWithKnownAccessors;
    BitVector fieldsGivenAccessorsByDescriptorsInSight;
    BitVector fieldsWithUnseenAccessors;
    BitVector fieldsWrittenAfterConstruction;
    BitVector layoutsThatUnseenCodeMayChange;
    BitVector modulesWhoseValueStaysInSight;
    BitVector modulesWhoseRecordIsHandedOutFrozen;
    BitVector layoutsThatGainPropertiesThroughLost;
    BitVector layoutsThatGainWhatUnseenCodeGaveSources;
    BitVector fieldsThatUnseenCodeMayWrite;
    OptionSet<LayoutChange> possibleChangesOf(size_t layout) const
    {
        OptionSet<LayoutChange> changes = layout < possibleChanges.size() ? possibleChanges[layout] : OptionSet<LayoutChange> { };
        if (reading() != WhatUnseenCodeDoes::Anything)
            return changes;
        if (layoutsThatUnseenCodeMayChange.get(layout))
            changes.add({ LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        if (layoutsThatGainPropertiesThroughLost.get(layout) || layoutsThatGainWhatUnseenCodeGaveSources.get(layout))
            changes.add(LayoutChange::GainsProperty);
        return changes;
    }
    bool isWrittenAfterConstruction(size_t cell) const { return fieldsWrittenAfterConstruction.get(cell) || (reading() == WhatUnseenCodeDoes::Anything && fieldsThatUnseenCodeMayWrite.get(cell)); }

    BitVector fieldsThatMayBeEmpty;
    BitVector fieldsFilledAfterConstructions;
    BitVector fieldsNotOfBirth;
    BitVector gettersThatMayBeAbsent;
    BitVector settersThatMayBeAbsent;
    void noteBirthsOf(FunctionID);
    HashMap<uint32_t, Vector<uint32_t>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsOfBirthSoFar;
    Vector<uint32_t>& fieldsThatLaneAtWorkHasNotNotedEmpty(LayoutID) LIFETIME_BOUND;
    void noteConstructionsOf(FunctionID);
    FlowNodeID foundWhereFieldIsEmpty(FieldReference);
    bool hasLockableInstances(LayoutID classLayout) const { return m_classesWithLockableInstances.get(classLayout.index()); }
    void noteLockableInstances(LayoutID);
    FlowNodeID instanceLockNode(LayoutID classLayout);
    FlowNodeID lostNameNode(LayoutID);
    bool mayReadInFrontOfDefinition(FunctionID reader, const SummaryAccess&, LayoutID) const;
    struct InstanceOfGuardedRead {
        ValueID site;
        ValueID test;
        LayoutID tested;
    };
    struct InstanceOfGuardedReads {
        Vector<InstanceOfGuardedRead> bySite;
        Vector<std::pair<ValueID, uint32_t>> accessesByTest;
    };
    static InstanceOfGuardedReads findInstanceOfGuardedReads(const Program&, const BodyIndex&, const ThinSummary&);
    const InstanceOfGuardedReads* instanceOfGuardedReads(FunctionID);
    struct GuardedFamilyRead {
        FlowNodeID fields;
        FlowNodeID besideFields;
    };
    std::optional<GuardedFamilyRead> findGuardedFamilyPropertyRead(FamilyID, PropertyKeyID, LayoutID tested);
    bool superCallMayAnswerWithOlderObject(LayoutID id) const { return m_layoutsWhoseSuperCallMayAnswerWithOlderObject.get(id.index()); }
    bool constructionMayAnswerWithOlderObject(LayoutID id) const { return m_layoutsWhoseConstructionMayAnswerWithOlderObject.get(id.index()); }
    bool everyMemberStandsAtOrBelow(FamilyID family, LayoutID above) const { return m_stretchesAtAndBelowLayouts[above.index()].holds(m_stretchesOfFamilies[family.index()]); }
    LayoutID topOfRow(LayoutID id) const { return m_topsOfRows[id.index()]; }
    bool standsAtOrBelow(LayoutID id, LayoutID above) const
    {
        uint32_t place = m_stretchesAtAndBelowLayouts[id.index()].first;
        return m_stretchesAtAndBelowLayouts[above.index()].holds({ place, place + 1 });
    }
    LayoutID classUnderConstruction(FunctionID writer) const;
    bool mayStoreIntoOlderObjectThatSuperCallAnswered(FunctionID writer, const SummaryAccess&) const;
    void forEachNodeAnsweredBesideObjectOfConstruction(FunctionID constructor, NOESCAPE const ScopedLambda<void(uint32_t nodeInSummary)>&) const;
    bool isPartOfConstructionOf(LayoutID, FunctionID writer, const SummaryAccess&) const;
    bool isPartOfConstructionOfAllOf(FamilyID, FunctionID writer, const SummaryAccess&) const;

private:
    void takeForUnlaid(BuiltinID canonicalHolder, PropertyKeyID, bool isLocked);
    bool canLockAbsentMember(BuiltinID canonicalHolder, PropertyKeyID);
    bool isMemberOfProgramObjects(PropertyKeyID);
    void noteHolderPassedInSight(BuiltinID canonicalHolder, PropertyKeyID);
    bool isOutOfSight(BuiltinID canonical) const;
    bool staysAbsentUnseen(BuiltinID canonicalHolder, PropertyKeyID) const;

    struct Edge {
        FlowNodeID to;
        TypeID passes;
        uint32_t next { UINT32_MAX };
    };

    struct Node {
        uint64_t head { LinkType { }.head() };
        uint32_t references { 0 };
        TypeID typeOfPosition;
    };
    static_assert(sizeof(Node) == 16);
    struct ListsOfNode {
        uint32_t firstEdge { UINT32_MAX };
        uint32_t firstObserver { UINT32_MAX };
    };
    LargeList<Node> m_nodes;
    LinkReferenceTable m_references;
    LinkReferenceTable::RecentNumbers m_recentNumbers;
    uint32_t numberOfReferences(const LinkType::References&);
    static ALWAYS_INLINE LinkType storedNodeHead(const Node& node) { return LinkType::ofHead(node.head); }
    static ALWAYS_INLINE bool bringsOwnReferences(const Node& node, const LinkType& held, const LinkType& arriving)
    {
        return node.references && LinkType::areSameReferences(arriving.references(), held.references())
            && arriving.objects == held.objects && arriving.functions == held.functions && arriving.classes == held.classes && arriving.builtinClass == held.builtinClass
            && (!held.hasUnknownObjects || (!held.builtin && !held.setOfClasses)) && admitsEveryName(node);
    }
    static ALWAYS_INLINE bool admitsEveryName(const Node& node) { return !node.typeOfPosition || node.typeOfPosition == TypeTable::anything; }
    static ALWAYS_INLINE bool isLostThroughout(const LinkType& type) { return type.objects == LinkType::Objects::Lost && type.functions == LinkType::Functions::Lost && type.classes == LinkType::Classes::Mixed; }
    bool isUnitedWith(const LinkType& held, const LinkType& arriving) const;
    ALWAYS_INLINE LinkType storedNodeType(const Node& node) const { return LinkType::of(node.head, m_references.at(node.references)); }
    static constexpr uint32_t unknownReferences = UINT32_MAX;
    ALWAYS_INLINE uint32_t numberOfReferencesOfArrival(const LinkType::References& references, uint32_t referencesOfSender)
    {
        if (referencesOfSender != unknownReferences && LinkType::areSameReferences(references, m_references.at(referencesOfSender)))
            return referencesOfSender;
        return LinkType::areSameReferences(references, LinkType::noReferences()) ? 0 : numberOfReferences(references);
    }
    ALWAYS_INLINE void storeNodeType(Node& node, const LinkType& type, const LinkType& before, uint32_t referencesOfSender = unknownReferences)
    {
        node.head = type.head();
        LinkType::References references = type.references();
        if (!LinkType::areSameReferences(references, before.references()))
            node.references = numberOfReferencesOfArrival(references, referencesOfSender);
    }
    ALWAYS_INLINE LinkType headOfNode(size_t node) const { return storedNodeHead(m_nodes[node]); }
    ALWAYS_INLINE LinkType typeOfNode(size_t node) const { return storedNodeType(m_nodes[node]); }
    ALWAYS_INLINE void setTypeOfNode(size_t node, const LinkType& type, const LinkType& before, uint32_t referencesOfSender = unknownReferences) { storeNodeType(m_nodes[node], type, before, referencesOfSender); }
    ALWAYS_INLINE bool isNeverAt(size_t node) const
    {
        const Node& held = m_nodes[node];
        return !held.references && storedNodeHead(held).isNever();
    }
    using SetOfNumbers = HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    bool hasPreparation(PropertyKeyID symbol) const { return m_everySymbolOfLibraryMayHavePreparation || m_symbolsWithPreparation.contains(symbol.bits()); }
    struct Preparation {
        PropertyKeyID symbol;
        FunctionID function;
        friend bool operator==(const Preparation&, const Preparation&) = default;
    };
    static bool letSymbolsThatShareFunctionGoByValue(const Vector<Preparation>&, const SetOfNumbers& besideOtherKeys, SetOfNumbers& goByValue);
    struct SymbolsWithPreparationInSight {
        Vector<PropertyKeyID> notedBesideOtherKeys;
        Vector<Preparation> notedWithPreparation;
        bool unknownSymbolWasNoted { false };
        Vector<Preparation> preparations;
        bool everySymbolOfLibraryMayHaveOne { false };
        SetOfNumbers goByValue;
        SetOfNumbers besideOtherKeys;
        bool goesByValue(PropertyKeyID symbol) const { return everySymbolOfLibraryMayHaveOne || goByValue.contains(symbol.bits()); }
    };
    SymbolsWithPreparationInSight m_symbolsInSight;
    bool takeInWhatWasNotedOfSymbols();
    Vector<PropertyKeyID> m_symbolsNotedBesideOtherKeys;
    Vector<Preparation> m_preparationsNoted;
    bool m_unknownSymbolWasNotedWithPreparation { false };
    void tellAccessesUnderSymbolsWithPreparation();
    Shared<HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_nodesOfFunctionsHandedWithSymbolAt;
    SetOfNumbers m_symbolsWithPreparation;
    bool m_everySymbolOfLibraryMayHavePreparation { false };
    Vector<Preparation> m_preparations;
    SetOfNumbers m_symbolsThatGoByValue;
    bool goesByValue(PropertyKeyID symbol) const { return m_everySymbolOfLibraryMayHavePreparation || m_symbolsThatGoByValue.contains(symbol.bits()); }
    SetOfNumbers m_symbolsOfLibraryBesideOtherKeys;
    Vector<std::pair<PropertyKeyID, FlowObserver>> m_accessesUnderSymbolsOfLibrary;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessesUnderSymbolsOfLibraryListed;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_symbolsAtNodes;
    SetOfNumbers m_symbolsThatLostTheirNames;
    Vector<PropertyKeyID> m_symbolsNotedWithoutTheirNames;
    void noteSymbolThatLosesItsName(PropertyKeyID);
    LargeList<ListsOfNode> m_listsOfNodes;
    std::optional<SizeOfLinkFlow> m_sizeWhenWaysWereForgotten;
    BitVector m_flowsIntoUnseen;
    BitVector m_accessorsWaitAt;
    BitVector m_hasEdgesInSet;
    struct ArrivalHeldBack {
        FlowNodeID node;
        PropertyKeyID symbol;
        LinkType type;
        uint32_t referencesOfSender { unknownReferences };
    };
    struct EdgeLaid {
        FlowNodeID from;
        FlowNodeID to;
        TypeID passes;
    };
    struct ObserverPut {
        FlowNodeID node;
        FlowObserver observer;
    };
    struct Lane;
    Vector<std::unique_ptr<Lane>> m_lanes;
    unsigned m_log2OfNodesInBlock { 12 };
    unsigned m_maskOfLanes { 0 };
    std::atomic<uint32_t> m_notingLanes { 0 };
    unsigned laneIndexOf(FlowNodeID node) const { return (node.index() >> m_log2OfNodesInBlock) & m_maskOfLanes; }
    Lane& laneOf(FlowNodeID node) { return *m_lanes[laneIndexOf(node)]; }
    static constinit thread_local Lane* s_laneAtWork;
    bool m_flowIsAtRest { false };
    bool m_typesAreSettled { false };
    BitVector m_familiesWhoseConstructorsHaveEscaped;
    BitVector m_sitesThatHaveEscaped;
    bool m_isDeferringUpdates { false };
    bool m_tellsObservers { false };
    bool m_severalThreadsTell { false };
    bool m_lanesTakeStepsOfReach { false };
    alignas(128) mutable std::atomic<bool> m_whatIsSharedIsLocked { false };
    static void raise(bool& flag)
    {
        if (!WTF::atomicLoad<MemoryOrder::Relaxed>(&flag))
            WTF::atomicStore<MemoryOrder::Relaxed>(&flag, true);
    }
    ALWAYS_INLINE static FlowNodeID findNodeWithoutLock(uint32_t& node)
    {
        uint32_t known = WTF::atomicLoad<MemoryOrder::Acquire>(&node);
        return known == UINT32_MAX ? FlowNodeID() : FlowNodeID(known);
    }
    void lockWhatIsShared() const
    {
        static constexpr unsigned pausesBeforeGivingWay = 4096;
        unsigned pauses = 0;
        while (m_whatIsSharedIsLocked.load(std::memory_order_relaxed) || m_whatIsSharedIsLocked.exchange(true, std::memory_order_acquire)) {
            if (++pauses < pausesBeforeGivingWay)
                simde_mm_pause();
            else
                Thread::yield();
        }
    }
    static constinit thread_local unsigned s_depthOfLockOfWhatIsShared;
    struct alignas(128) PartOfWhatSomeLaneWasGiven {
        std::atomic<bool> isLocked { false };
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> given;
    };
    static constexpr size_t numberOfPartsOfWhatSomeLaneWasGiven = 16;
    mutable std::array<std::array<PartOfWhatSomeLaneWasGiven, numberOfPartsOfWhatSomeLaneWasGiven>, numberOfKindsMadeOnDemand> m_givenToSomeLane;
    PartOfWhatSomeLaneWasGiven& lockPartOfWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words) const;
    static const LinkType s_nothing;
    std::atomic<size_t> m_numberOfNodesMade { 0 };
    FlowNodeID contentsFoundOrMadeByLane(SiteID, SlotOfContents);
    void makeContentsInLane(uint32_t family, SlotOfContents, FlowNodeID);
    void listContentsThatLanesMade();
    void listContentsThatLanesMadeIn(unsigned part);
    static bool isToldByOneThread(FlowObserver::Kind);
    void tellOnLanes();
    void prepareAccessesForSeveralThreads();
    void prepareOperatorsForSeveralThreads();
    void sortTellsOfLane(unsigned lane);
    void tellInLane(unsigned lane);
    void takeOverWhatLanesDidWhileTelling();
    void takeOverNewObserversOfLanes();
    void noteFunctionToReach(FunctionID);
    void reachAll(std::span<const FunctionID>);
public:
    void noteLayoutToTie(LayoutID layout) { m_layoutsToTie.append(layout); }
    void tellKeysThatBecomeTexts(LayoutID);
    void tellNameThatBecomesText(FunctionID);
    static constexpr uint8_t nothingIsLeftToTell = 1;
    static constexpr uint8_t waitsForProgramToAsk = 2;
    BytesSharedByThreads m_foundByCalls;
    BytesSharedByThreads m_keysOfLayoutAreTold;
    BytesSharedByThreads m_nameOfFunctionIsTold;
    BytesSharedByThreads m_functionsThatReadOffViewsMadeAtSites;
    BytesSharedByThreads m_namesOfLostFunctionsAreTold { 1 };
    BitVector m_keysThatNameEvents;
    BitVector m_textsThatNameEvents;
private:
    void runWithDeferredUpdates(const ScopedLambda<void()>&);
    void applyNote(Note, uint32_t number, uint32_t more);
    void flushFirstLaneNotes();
    bool resolveNotifiedCalls(unsigned lane);
    bool m_setsMeetingsAside { false };
    Lane* m_firstLane { nullptr };
    ALWAYS_INLINE Lane& deferringLane() const
    {
        Lane* lane = s_laneAtWork;
        return lane ? *lane : *m_firstLane;
    }
    BitVector m_isToBeTold;
    uint64_t m_numberOfRounds { 0 };
    size_t m_mostSentSinceFree { 0 };
    unsigned m_quietPasses { 0 };
    void flow();
    void goAlongEdges();
    void freeIdleRoomOfLanes(size_t mostSent);
    void sendFromLane(unsigned lane);
    void receiveInLane(unsigned lane);
    void foldObjects(LinkType& into, std::span<const LinkType> met) const;
    void foldObjectsTakenApart(LinkType& into, std::span<const LinkType> met) const;
    void foldFunctions(LinkType& into, std::span<const LinkType> met) const;
    bool severalThreadsPayFor(size_t numberOfSteps) const;
    void wakeDeferredUpdateWaiters();
    void applyDeferredUpdates();
    void applyDeferredUpdates(unsigned lane);
    void gatherWhatLanesNoted();
    void runOnLanes(const Invocable<void(unsigned lane)> auto&);
    void runOnLanes(std::span<const uint32_t> workPerLane, const Invocable<void(unsigned lane)> auto&);
    bool lanesMayWorkForReach(size_t numberOfSteps) const;
    void workOnLanesForReach(const Invocable<void(unsigned lane)> auto&);
    bool layEdge(Lane&, FlowNodeID from, FlowNodeID to, TypeID passes);
    bool isEdgeLaid(Lane&, FlowNodeID from, FlowNodeID to, TypeID passes);
    bool addEdgeOfBusyNode(Lane&, FlowNodeID from, FlowNodeID to, TypeID passes);
    void addEdgeAtOnce(FlowNodeID from, FlowNodeID to, TypeID passes);
    void putObserver(Lane&, FlowNodeID, const FlowObserver&);
    bool isObservedAlready(FlowNodeID, const FlowObserver&);
    void send(Lane& sender, FlowNodeID to, const LinkType&, PropertyKeyID symbol, uint32_t referencesOfSender);
    void receive(FlowNodeID, const LinkType&, PropertyKeyID symbol, uint32_t referencesOfSender = unknownReferences);
    void noteClimb(Lane&, FlowNodeID, uint64_t headBefore, uint32_t referencesBefore);
    void joinHeads(FlowNodeID, Node&, const LinkType& before, const LinkType& arriving);
    bool tryJoinByHeads(Lane&, const ArrivalHeldBack&);
    void receiveWithoutMarksThatAreUsed(FlowNodeID, const LinkType&, PropertyKeyID symbol);
    void arriveAtOnce(FlowNodeID, const LinkType&, PropertyKeyID symbol);
    void wakeAccessorsThatWaitAt(Lane&, FlowNodeID);
    bool isAskedByMoreThanAccessorsThatWait(Lane&, FlowNodeID);
    void putObserverThatWaitsUntilNodeIsAsked(Lane&, FlowNodeID, const FlowObserver&);
    bool isAskedForTheFirstTime(Lane&, FlowNodeID);
    bool hasListsToItself(FlowNodeID) const;
    bool wouldNameSomethingNewAtUnseen(const LinkType&) const;
    void noteLossOfBuiltin(BuiltinID) const;
public:
    bool isSpelledInBody(FunctionID, uint32_t summaryNode) const;
private:
    void noteMetInFamily(LayoutID) const;
    bool hasEscapedAsFamily(FamilyID) const;
    void catchUpWithFamiliesAbove(LayoutID) const;
    Vector<FamilyID> m_outermostFamilies;
    mutable BitVector m_outermostFamiliesWhereSomethingBefell;
    PropertyKeyID symbolNotedAt(FlowNodeID) const;
    size_t placeOfNextNode(Lane&);
    void removeNextNode(Lane&);
    std::pair<FlowObserver, FlowNodeID> takeNewObserver();
    uint64_t nextScatteredNumber();
    mutable Lock m_lockOfSymbolsAtNodes;
    const OrderOfWorklist m_order;
    size_t m_firstOfNewObservers { 0 };
    uint64_t m_scatteredNumber { 88172645463325252ull };
    Vector<BreachOfClosure>* m_breachesOfClosure { nullptr };
    FlowObserver m_observerToldOnceMore;
    FlowNodeID m_nodeGoneOver;
    void noteBreachOfClosure(BreachOfClosure&&);
    BitVector m_familiesThatHaveEscaped;
    BitVector m_lostFamiliesThatHaveEscaped;
    Vector<LayoutSetID> m_layoutsThatMet;
    UncheckedKeyHashMap<uint32_t, LayoutSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsThatMetByRepresentative;
    BitVector m_isOnWorklist;
    uint64_t m_numberOfObserversTold { 0 };
    struct NodesOfFunction {
        uint32_t firstParameter;
        uint32_t firstResult;
        uint32_t thrown;
        uint32_t base;
        uint32_t firstSummaryNode;
    };
    Vector<NodesOfFunction> m_nodesOfFunctions;
    Vector<uint32_t> m_nodesOfSummaries;
    Vector<NameClass> m_nameClassesOfKeys;

    StateOfLinkReach& stateOfLinkReach();
    bool fillParametersThatUnseenCodeAlonePasses();
    std::unique_ptr<StateOfLinkReach, DeleterOfStateOfLinkReach> m_stateOfLinkReach;

    StateOfLinkCalls& stateOfLinkCalls();
    std::unique_ptr<StateOfLinkCalls, DeleterOfStateOfLinkCalls> m_stateOfLinkCalls;

    ALWAYS_INLINE StateOfLinkAccesses& stateOfLinkAccesses()
    {
        if (m_stateOfLinkAccesses) [[likely]]
            return *m_stateOfLinkAccesses;
        return makeStateOfLinkAccesses();
    }
    StateOfLinkAccesses& makeStateOfLinkAccesses();
    std::unique_ptr<StateOfLinkAccesses, DeleterOfStateOfLinkAccesses> m_stateOfLinkAccesses;

    std::unique_ptr<StateOfLinkOperators, DeleterOfStateOfLinkOperators> m_stateOfLinkOperators;

    std::unique_ptr<TestRunsInLink> m_testRuns;

    const LinkType* m_typeObservedBefore { &s_nothing };
    Vector<uint32_t> m_firstFieldOfLayout;

    void noteFieldMayBeEmpty(FieldReference);
    Vector<uint32_t> m_nodesFoundWhereFieldsAreEmpty;
    BitVector m_classesWithLockableInstances;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_instanceLockNodes;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lostNameNodes;

    void indexConstructorFunctions();
    void findFunctionsListingReturns();
    uint32_t recordOfNameOnPrototype(LayoutID, PropertyKeyID);
    uint32_t firstNameOnPrototypeOf(LayoutID) const;
    void forEachPrototypePassedAbove(LayoutID, PropertyKeyID, const Invocable<void(uint32_t record)> auto&);
    void tiePrototypesAboveLostLayout(LayoutID);
    bool isPooledNameOnPrototype(uint32_t record) const;
    void poolNameOnPrototype(uint32_t record);
    bool letPrototypesFoundThroughLostEscape();
    bool callsMethodsOfNewObject(LayoutID made) const;
    bool noteFieldsEmptiedByMethods();
    void listConstructions();
    bool isFilledAfterConstructions(LayoutID made) const;
    Vector<LayoutID> m_layoutsFilledByMethods;
    struct ConstructionInSummary {
        FunctionID caller;
        uint32_t call { 0 };
    };
    Vector<ConstructionInSummary> m_constructions;
    BitVector m_constructorsBeforeMethods;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namesConstructedBeforeMethods;
    Vector<LayoutID> m_layoutsTestedByConstructors;
    BitVector m_functionsFillingNewObjects;

    struct ConstructedPrototype {
        FunctionID function;
        LayoutID below;
        FlowNodeID instance;
        LayoutID parent;
        bool isAbandoned { false };
        std::optional<uint32_t> copyingCall;
    };
    PrototypeHelperCall* findCopyingCall(uint32_t call) const;
    std::optional<bool> copiesNamesOfRecord(const PrototypeHelperCall&) const;
    Vector<ConstructedPrototype> m_constructedPrototypes;
    BitVector m_layoutsWithReplacedPrototypes;
    bool layConstructedPrototypes();
    bool addAppendedFieldsToPrototype(LayoutID below, LayoutID parent);
    void noteNameMayInterceptStores(uint32_t record);
    bool noteNamesThatMayInterceptStores();
    LayoutID findConstructedParent(const ConstructedPrototype&) const;
    bool constructionKeepsThis(LayoutID) const;
    bool ignoresThis(FunctionID) const;
    std::optional<bool> onlyForwardsThis(LayoutID) const;
    class CopiesOfMembers;
    bool layMembersThatCopiesBring();
    bool copyAppendedFields();
    size_t countNamesOfRecords(FlowNodeID) const;
    size_t countPrototypeNames(LayoutID) const;
    FlowNodeID findExtraPropertyStoreNode(LayoutID) const;
    bool tellAccessesUnderNewNames();
    class PrototypeHelperReader;
    enum class HelperProgress : uint8_t { Waits, IsDone, GivesUp };
    struct HelperEntered {
        const PrototypeHelper* helper { nullptr };
        FunctionID function;
        Vector<FlowNodeID, 4> arguments;
    };
    const PrototypeHelper* knownPrototypeHelper(FunctionID) const;
    HelperEntered enteredBy(const PrototypeHelperCall&) const;
    HelperProgress visitPrototypeHelper(const HelperEntered&, Vector<HelperEntered, 4>& entered, const Invocable<bool(const PrototypeHelperStep&)> auto& visit, const Invocable<void(FlowNodeID)> auto& watch) const;
    bool takePrototypeHelperStep(const PrototypeHelperStep&);
    bool everyFunctionAboveIsConstructorFunction(FunctionID) const;
    bool findsOwnNameBuiltin(const PrototypeHelperStep&) const;
    bool someBodyStoresHasOwnProperty() const;
    mutable std::optional<bool> m_someBodyStoresHasOwnProperty;
    void storeOnClassHolder(FunctionID writer, LayoutID, ClassSide, PropertyKeyID, FlowNodeID value);
    void abandonHelperCall(PrototypeHelperCall&);
    void readCalledHelpers(FunctionID);
    LayoutID classOfHolder(FunctionID) const;
    BitVector m_prototypeHelperCandidates;
    HashMap<uint32_t, std::unique_ptr<PrototypeHelper>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_prototypeHelpers;
    Vector<std::unique_ptr<PrototypeHelperCall>> m_prototypeHelperCalls;
    HashSet<std::pair<uint32_t, uint32_t>> m_prototypeHelperCallsListed;
    HashSet<std::pair<uint64_t, uint64_t>> m_namesStoredByHelpers;
    bool markFieldsSkippedByCopies();
    std::optional<Vector<FunctionID>> m_copyingFunctions;
    BitVector m_plainFieldLayouts;
    bool canCopySkipField(LayoutID) const;
    BitVector m_parserAllocatedLayouts;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_edgesLaidByCopiesOfMembers;
    BitVector m_layoutsOfConstructorFunctions;
    BitVector m_layoutsBelowConstructorFunctions;
    BitVector m_layoutsWithFollowedPrototypes;
    BitVector m_layoutsBelowFollowedPrototypes;
    BitVector m_namesStoredOnObjects;
    Vector<LayoutID> m_nearestConstructorFunctionsFrom;
    BitVector m_prototypesPassedByLookups;
    BitVector m_prototypesWithDefinitions;
    BitVector m_keysDefinedByDescriptors;
    bool m_someAccessorIsWrittenUnderComputedKey { false };
    bool mayBeDefinedUnderUnknownKeyOnPrototypeOf(LayoutID, PropertyKeyID) const;
    void findKeysDefinedByDescriptors();
    bool letAccessorOnPrototypeEscape(uint32_t record);
    void noteConstructionsMayLeaveFieldEmpty(FieldReference);
    void noteFieldsThatAccessorAboveMayLeaveEmpty(LayoutID, PropertyKeyID);
    BitVector m_layoutsWhoseConstructorsMeetAccessors;
    BitVector m_prototypesAboveLostLayouts;
    Vector<NameOnPrototype> m_namesOnPrototypes;
    struct NamesOfPrototype {
        uint32_t first { UINT32_MAX };
        uint32_t last { UINT32_MAX };
    };
    HashMap<uint32_t, NamesOfPrototype, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namesOfPrototypes;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfNamesOnPrototypes;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesFoundOnPrototypesAbove;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeForClassesBelow;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeThroughConstructors;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeBySuperCalls;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfMembersWritten;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesWhereLossIsTold;

    enum class NodeOnDemand : uint8_t { ReadThroughFamily, LaidThroughFamily, AnyFieldReadOfLayout, AnyFieldStoredOfLayout, AnyFieldReadOfFamily, AnyFieldStoredOfFamily, AnyFieldReadOfSet, ElementReadOfSet, AnyFieldStoredOfSet, ElementStoredOfSet, FoundBesideFieldsUnderKey, LaidOnClosuresUnderKey, AnyFieldDefinedOnLayout, WhatStandsAbove, WhatStandsBelow, ConstructorChainStoreNode };
    enum class AggregateAccess : uint8_t { Read, Store };

    struct MemberOfClass {
        LayoutID classLayout;
        ClassSide side { ClassSide::Prototype };
        PropertyKeyID key;
        MethodKind kind { MethodKind::Method };
        uint32_t index { 0 };
        FunctionID function;
    };

    struct Deferred {
        enum class Kind : uint8_t { LayoutIsLost, FamilyIsLost, LayoutsAreUnited, LostFamilyEscapes, FunctionIsLost, FunctionsAreUnited, PoolsOfLostFunctionsAreUnited, FamilyOfFunctionsEscapes, ConstructorsAreLost, SitesAreUnited, LayoutEscapes, FunctionEscapes, ConstructorsEscape, PrototypesEscape, SiteEscapes, TypeEscapes };

        Kind kind { Kind::TypeEscapes };
        uint32_t number { 0 };
        LinkType type;
        uint32_t other { 0 };
    };

    struct LostKey {
        PropertyKeyID key;
        uint32_t read { UINT32_MAX };
        uint32_t stored { UINT32_MAX };
        uint32_t defined { UINT32_MAX };
        uint32_t firstHolder { UINT32_MAX };
        uint32_t lastHolder { UINT32_MAX };
        uint32_t family { 0 };
        uint32_t next { UINT32_MAX };
        bool tiesEveryMember { false };
        bool isListedAsTyingEveryMember { false };
        bool hasBeenMerged { false };
    };

    struct LostFamily {
        uint32_t foundBesideFields { UINT32_MAX };
        uint32_t storedBesideFields { UINT32_MAX };
        uint32_t anyFieldRead { UINT32_MAX };
        uint32_t anyFieldReadUnderMarksOfRule { UINT32_MAX };
        uint32_t firstKey { UINT32_MAX };
        Vector<uint32_t> keysThatTieEveryMember;
        bool reachesUnseen { false };
        std::set<uint32_t> placesInForestOfFamilies;
    };

    struct HolderOfLostKey {
        LayoutID layout;
        uint32_t next { UINT32_MAX };
    };

    Vector<uint32_t> indexMembersOfClasses();
    void numberNodes(Vector<uint32_t>&& numbersOfMemberNodesOfSides);
    void makeTablesThatNeedNoNodes();
    FlowNodeID findSharedNodeOf(FunctionID, uint32_t nodeInSummary) const;
    FlowNodeID makeNode();
    std::span<const MemberOfClass> membersOf(LayoutID, ClassSide) const LIFETIME_BOUND;
    std::span<const MemberOfClass> membersFoundUnder(LayoutID, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    bool lookupGoesOnAboveClasses(LayoutID from, std::span<const MemberOfClass> members) const { return members.empty() || mayNotBeLinkedYetBelow(from, members[0].classLayout); }

    LinkType joined(const LinkType&, const LinkType&, LossAtJoin = LossAtJoin::IsNotedAtOnce) const;
    bool loseInSight(BuiltinID canonical) const;
    Vector<BuiltinID>& builtinsLostInSightByThread() const;
    bool isNodeOfLibrary(size_t index) const { return index < m_isNodeOfLibrary.size() && m_isNodeOfLibrary.quickGet(index); }
    ALWAYS_INLINE LinkType passedByTestOfEdge(const LinkType& type, TypeID test)
    {
        if (!test)
            return type;
        if (test == TypeTable::anything) {
            LinkType passed = type;
            passed.primitives &= LinkType::closed(Primitive::All);
            return passed;
        }
        return passedByTestOfEdgeSlowCase(type, test);
    }
    ALWAYS_INLINE void joinObjects(LinkType& into, const LinkType& other) const
    {
        if (other.objects == LinkType::Objects::None)
            return;
        if (into.objects == other.objects && into.layout == other.layout && into.family == other.family)
            return;
        if (into.objects == LinkType::Objects::None) {
            into.objects = other.objects;
            into.layout = other.layout;
            into.family = other.family;
            return;
        }
        joinObjectsSlowCase(into, other);
    }
    void joinFunctions(LinkType& into, const LinkType&) const;
    ALWAYS_INLINE void joinClasses(LinkType& into, const LinkType& other) const
    {
        if (other.classes == LinkType::Classes::None)
            return;
        joinClassesSlowCase(into, other);
    }
    FlowNodeID contentsFoundOrMadeByOneThread(SiteID, SlotOfContents);
    LinkType passedBySlowCase(const LinkType&, TypeID test) const;
    LinkType passedByTestOfEdgeSlowCase(const LinkType&, TypeID test);
    void joinObjectsSlowCase(LinkType& into, const LinkType&) const;
    void joinObjectsOfOtherKinds(LinkType& into, const LinkType&) const;
    void joinClassesSlowCase(LinkType& into, const LinkType&) const;
    void joinClassesThatDifferOrLieAtOtherSites(LinkType& into, const LinkType&) const;
    bool standsOutsideFamilies(const LinkType&) const;
    bool namesOnlyLayoutsOfSet(const LinkType&, const LinkType& set) const;
    void deferLoss(LayoutID) const;
    void deferUnion(LayoutID, LayoutID) const;
    LayoutID memberStandingFor(const LinkType&) const;
    mutable SetsOfLayoutsInLinkFlow m_setsOfLayouts;
    mutable SetsOfFunctionsInLinkFlow m_setsOfFunctions;
    mutable SetsOfClassesInLinkFlow m_setsOfClasses;
    bool mayStandInOneSet(std::span<const FunctionID> sortedMembers) const;
    bool addListedReturnResults();
    BitVector m_functionsListingReturns;
    BitVector m_functionsWithAddedResults;
    HashMap<uint32_t, LinkType, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_resultsGivenWithoutFunctions;
    bool namesOnlyFunctionsOfSet(const LinkType&, const LinkType& set) const;
    void foldFunctionsTakenApart(LinkType& into, std::span<const LinkType> met) const;
    void uniteLost(LayoutID, LayoutID);
    void loseFamiliesOfMembersThatMeet(uint32_t kept, uint32_t absorbed);
    struct TiesToLay {
        enum class Members : uint8_t { HoldersOfLostKey, LostFamily, DeclaredFamily, Listed };
        enum class Access : uint8_t { Read, Assign, AssignInConstruction, DefineInConstruction, Define, ReadOfAnyField, ReadOfAnyFieldUnderNumbers };
        Members members;
        Access access;
        FlowNodeID aggregate;
        uint32_t number;
        PropertyKeyID key;
        uint32_t end { 0 };
        UnknownByEscapeAlone::Marks marks { UnknownByEscapeAlone::Marks::Nothing };
        bool isInLibraryCode { false };
    };
    bool leavesTiesToLanes(TiesToLay);
    void layTiesThatLanesLeft();
    bool settlingLeavesTies() const { return m_isSettling && m_isDeferringUpdates && !m_severalThreadsTell && !s_laneAtWork && !m_breachesOfClosure; }
    void layTiesThatSettlingLeft();
    Vector<TiesToLay> m_tiesLeftBySettling;
    Vector<LayoutID> m_membersOfTiesLeftBySettling;
    void layTie(const TiesToLay&, LayoutID member);
    uint32_t placeInForestOfFamilies(LayoutID);
    Vector<uint32_t> m_placesInForestOfFamilies;
    Vector<LayoutID> m_layoutsByPlaceInForestOfFamilies;
    void mergeLostFamilies(uint32_t kept, uint32_t absorbed);
    Vector<LayoutID> membersOfLostFamily(uint32_t representative) const;
    void deferLoss(FamilyID) const;
    void deferLoss(FunctionID) const;
    void deferUnion(SiteID, SiteID) const;
    void deferLossOfConstructors(FamilyID) const;
    void loseFamilyThatHoldsLostConstructorsWith(FamilyID, LayoutID onlyMemberLost);
    struct LostConstructorsOfTree {
        FamilyID nearestFamily;
        LayoutID onlyMember;
    };
    HashMap<uint32_t, LostConstructorsOfTree, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lostConstructorsOfTrees;
    bool deferEscape(LayoutID) const;
    bool deferEscape(FunctionID) const;
    void deferEscape(SiteID) const;
    void noteReachesUnseen(const LinkType&) const;
    void settle()
    {
        if (!m_isSettling && !m_tellsObservers && !m_deferred.isEmpty())
            settleWhatWasDeferred();
    }
    void settleWhatWasDeferred();
    void tell(const FlowObserver&, const LinkType& now, const LinkType& before);
    bool onlyLooksAtBuiltinsReadByName(const FlowObserver&) const;
    bool onlyLooksAtBuiltinsLostAtJoins(const FlowObserver&) const;

    bool tieInWhatGrew();
    bool tieInLostLayouts();
    void tieInLostLayout(LayoutID);
    bool tieInLostFunctions();
    void tieInLostFunction(FunctionID);
    void open(FlowNodeID contents);
    bool isWordOfCell(SlotOfContents) const;
    void openContents(uint32_t family, SlotOfContents, FlowNodeID contents);
    FlowNodeID findContents(uint32_t family, SlotOfContents) const;
    void addContents(uint32_t family, SlotOfContents, FlowNodeID);
    void listContents(uint32_t site, SlotOfContents, FlowNodeID);
    bool openLayoutsThatChanged();
    bool tieStoresUnderUnknownNames();
    bool letMembersOfLostEscape();
    bool tieAppendedFieldsOfLostLayouts();
    bool poolAppendedFields();
    void noteLostLayoutsGainingProperties();

    static uint64_t wordOfNodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID);
    std::pair<FlowNodeID, bool> nodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID);
    uint64_t wordOfAnyFieldThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber) const;
    FlowNodeID findOrMakeAnyFieldReadThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber);
    FlowNodeID findOrMakeAnyFieldStoredThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber);
    Vector<NameOnClosures> m_namesOnClosures;
    mutable Lock m_lockOfOnlyReferrers;
    mutable Vector<FunctionID> m_onlyReferrers WTF_GUARDED_BY_LOCK(m_lockOfOnlyReferrers);
    mutable Lock m_lockOfLists;
    mutable HashMap<uint64_t, ReadList, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_readLists WTF_GUARDED_BY_LOCK(m_lockOfLists);
    mutable HashMap<uint32_t, std::pair<FunctionID, ValueID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callbackLists WTF_GUARDED_BY_LOCK(m_lockOfLists);
    Vector<uint32_t> m_firstNamerOfVariable;
    Vector<FunctionID> m_namersOfVariables;
    BytesSharedByThreads m_modulesWithDirectEval;
    UncheckedKeyHashMap<uint32_t, Vector<FlowNodeID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesThatLeaveWithClassObjects;
    BitVector m_classObjectsThatAreOpen;
    BitVector m_layoutsWithNotedBases;
    BitVector& layoutsWithNotedBases();
    Vector<uint32_t> m_newestNameOnClosuresOf;
    Vector<LayoutID> m_rootsOfClassRows;
    void indexRootsOfClassRows();
    void ensureExtraPropertyNodes(LayoutID);
    FlowNodeID extraPropertyNode(LayoutID, size_t place);
    void open(LayoutID);
    bool tieRead(FlowNodeID aggregate, LayoutID, PropertyKeyID);
    bool tieStore(FlowNodeID aggregate, LayoutID, PropertyKeyID, Lays);
    enum class InheritedMembers : uint8_t { Any, None };
    void tieReadOfAnyField(FlowNodeID aggregate, LayoutID, bool keyIsNumber = false, InheritedMembers = InheritedMembers::Any);
    void tieStoreOfAnyField(FlowNodeID aggregate, LayoutID, bool keyIsNumber = false);
    void tieAccessor(FlowNodeID aggregate, LayoutID receiver, const MemberOfClass&, AggregateAccess);
    void tieAccessorThatNobodyKnows(FlowNodeID aggregate, LayoutID receiver, FlowNodeID holder, AggregateAccess);
    void tieToUnseen(FlowNodeID aggregate, LayoutID, AggregateAccess);
    uint32_t recordOfLostKey(uint32_t representative, PropertyKeyID);
    FlowNodeID fieldThroughLost(LayoutID member, PropertyKeyID, TiesToLay::Access);
    static constexpr uint32_t anyLostFunction = UINT32_MAX;
    static constexpr uint32_t poolOfAllLostFunctions = 0;
    struct PoolOfLostFunctions {
        uint32_t receiver { UINT32_MAX };
        uint32_t result { UINT32_MAX };
        uint32_t laidOnClosures { UINT32_MAX };
        uint32_t made { UINT32_MAX };
        uint32_t gathered { UINT32_MAX };
        uint32_t firstArgumentGathered { UINT32_MAX };
        uint32_t keeper { UINT32_MAX };
        Vector<uint32_t> arguments;
        Vector<FunctionID> functionsTied;
    };
    uint32_t familyOfLostFunctions(const LinkType& callee) const;
    uint32_t poolOfLostFunctions(uint32_t family);
    FlowNodeID argumentOfLostFunctions(uint32_t pool, uint32_t positionalArgument);
    FlowNodeID gatheredByLostFunctions(uint32_t pool);
    FlowNodeID laidOnClosuresOfPool(uint32_t pool);
    FlowNodeID madeByPool(uint32_t pool);
    void tieThisToMade(FunctionID, FlowNodeID made);
    void tieWhatIsLaidOnClosuresOf(FunctionID, FlowNodeID laidOnClosuresOfPool);
    void gatherArgumentsOfLostFunctionsFrom(uint32_t pool, uint32_t firstGathered);
    void tiePoolsOfLostFunctions(uint32_t kept, uint32_t absorbed);
    void deferUnion(FunctionID, FunctionID) const;
    void unite(FunctionID, FunctionID) const;
    void noteFamilyOfFunctionsAtUnseen(FunctionID member) const;
    void noteCallOfFamilyMembers(FunctionID member) const;
    struct ArgumentsGathered {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ArgumentsGathered);
        struct Taker {
            FlowNodeID node;
            uint32_t place { 0 };
        };
        struct GathererBehind {
            FunctionID function;
            uint32_t firstPlace { 0 };
        };
        uint32_t fewestInSight { UINT32_MAX };
        uint32_t fewestByEscapeAlone { UINT32_MAX };
        FlowNodeID atAnyPlace;
        FlowNodeID elementsApplied;
        bool takesElementsApplied { false };
        Vector<FlowNodeID> atPlaces;
        Vector<Taker> takers;
        Vector<GathererBehind> gatherersBehind;
    };
    ArgumentsGathered& argumentsGatheredBy(FunctionID);
    Shared<HashMap<uint32_t, std::unique_ptr<ArgumentsGathered>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_argumentsGathered;
    Shared<HashSet<std::pair<uint64_t, uint32_t>, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>>> m_takersOfGatheredArgumentsAtPlaces;

    Vector<std::pair<FlowObserver, FlowNodeID>> m_newObservers;
    size_t m_numberOfNewObserversLeftWithLanes { 0 };
    size_t numberOfNewObservers() const { return m_newObservers.size() + m_numberOfNewObserversLeftWithLanes; }
    mutable Vector<Deferred> m_deferred;
    bool m_isSettling { false };
    bool m_letsReachWait { false };
    void bringAliveWhatSettlingReached();

    uint32_t m_firstVariable { 0 };
    Vector<uint32_t> m_nodeOfField;
    Vector<uint32_t> m_firstDeclaredFieldOfLayout;
    uint32_t m_firstNodeOfFields { 0 };
    BitVector m_fieldsWhoseSlotsHaveDoors;
    BitVector m_fieldsThatKeepExactIntegers;
    Vector<uint32_t> m_firstMemberNodeOfSide;
    Vector<uint32_t> m_firstMemberOfSide;
    Vector<MemberOfClass> m_membersOfClasses;
    std::array<BitVector, 2> m_keysOfMembers;
    BitVector m_sidesWithComputedKeys;
    BitVector m_fieldsWithSettersWritten;
    BitVector m_layoutsWithAccessorsWritten;
    static constexpr uint32_t siteAmongNodesOnDemand = 1u << 27;
    static constexpr uint32_t builtinAmongNodesOnDemand = 1u << 27;
    static constexpr uint32_t globalObjectAmongNodesOnDemand = 1;
    static constexpr uint32_t functionAmongNodesOnDemand = 1u << 26;
    static constexpr uint32_t underMarksOfRuleAmongNodesOnDemand = 1u << 27;
    static constexpr uint32_t literalKeyNodeBit = 1u << 26;
    BitVector m_sidesBelowIncompleteNames;
    BitVector m_staticSidesBelowIncompleteNamesBesideConstructorFunctions;
    void findSidesBelowIncompleteNames();
    BitVector m_layoutsBelowMembersUnderNumbers;
    void findLayoutsBelowMembersUnderNumbers();
    BitVector m_layoutsWithFieldsUnderComputedKeys;
    struct KeysThatRowOfConstructorsFills {
        uint32_t untilFirstSight { 0 };
        uint32_t untilEnds { 0 };
        uint32_t leftEmptyUntilFirstSight { UINT32_MAX };
        uint32_t leftEmptyUntilEnds { UINT32_MAX };
        uint32_t lookedUpUntilFirstSight { 0 };
        bool testsObject { false };
    };
    Vector<KeysThatRowOfConstructorsFills> m_keysThatRowsOfConstructorsFill;
    Vector<Vector<PropertyKeyID>> m_listsOfKeysThatRowsOfConstructorsFill;
    Vector<Vector<uint32_t>> m_listsOfFieldsThatRowsOfConstructorsLeaveEmpty;
    void forEachFieldLeftEmptyByRowOfConstructors(LayoutID, uint32_t placeOfKeysFilled, uint32_t placeOfFieldsLeftEmpty, const Invocable<void(uint32_t)> auto&) const;
    void findWhatRowsOfConstructorsFill();
    void findSuperCallsThatMayAnswerWithOlderObject();
    bool answersOnlyWithObjectOfItsConstruction(FunctionID constructor) const;
    void numberTreeOfBases();
    struct StretchInTreeOfBases {
        bool holds(const StretchInTreeOfBases& other) const { return first <= other.first && other.end <= end; }
        uint32_t first { UINT32_MAX };
        uint32_t end { 0 };
    };
    BitVector m_layoutsWhoseSuperCallMayAnswerWithOlderObject;
    HashMap<uint32_t, LayoutID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_classesOfInitializers;
    bool m_someSuperCallMayAnswerWithOlderObject { false };
    BitVector m_layoutsWhoseConstructionMayAnswerWithOlderObject;
    Vector<uint32_t> m_numbersOfAllocatedMembersOfFamilies;
    Vector<StretchInTreeOfBases> m_stretchesAtAndBelowLayouts;
    Vector<StretchInTreeOfBases> m_stretchesOfFamilies;
    Vector<LayoutID> m_topsOfRows;

    mutable BitVector m_layoutsToldLost;
    mutable BitVector m_familiesToldLost;
    mutable BitVector m_functionsToldLost;
    mutable BitVector m_layoutsToldToEscape;
    mutable BitVector m_functionsToldToEscape;
    mutable BitVector m_layoutsAtUnseen;
    mutable BitVector m_prototypesAtUnseen;
    mutable BitVector m_familiesAtUnseen;
    mutable BitVector m_functionsAtUnseen;
    mutable BitVector m_constructorsToldLost;
    mutable BitVector m_constructorsAtUnseen;
    mutable BitVector m_prototypesOfFamiliesAtUnseen;
    mutable BitVector m_familiesOfFunctionsAtUnseen;
    mutable Vector<FunctionID> m_membersOfFamiliesNewlyAtUnseen;
    mutable bool m_objectsReachUnseen { false };
    bool m_rowPassesWhatNobodyFollows { false };
    bool m_listenerMayReceivePromises { false };
    Vector<PropertyKeyID, 2> m_keysThatCodeTellsOfItself;
    bool m_promisesMayBeHandedOut { false };
    mutable Vector<BuiltinID> m_builtinsLost;
    mutable Vector<BuiltinID> m_builtinsLostAtJoins;
    mutable Vector<BuiltinID> m_builtinsLostInSight;
    struct SiteOfWrite {
        FunctionID writer;
        ValueID site;
        FlowNodeID subject;
        FlowNodeID source { };
    };
    Shared<Vector<SiteOfWrite>> m_sitesOfWritesOnBuiltins;
    mutable Vector<BuiltinID> m_builtinsAtUnseen;
    Vector<FlowNodeID> m_receiversThatWaitForAccessorsLaidAbove;
    bool m_operandIsLaidAbovePrototype { false };
    Vector<FlowNodeID> m_operandsLaidAbovePrototype;
    BitVector m_isNodeOfLibrary;
    BitVector m_isFunctionOfLibrary;
    bool m_isInLibraryCode { false };
    BitVector m_isFunctionThatRowsSpeakFor;
    mutable bool m_lostBuiltinsReachUnseen { false };
    mutable uint8_t m_marksThatReachUnseen { 0 };
    uint8_t m_marksThatAreUsed { 0 };
    Vector<PropertyKeyID, 2> m_namesReadWithAnswersInSight;

    struct TestOfEdge {
        enum class Passes : uint8_t { Everything, PrimitivesAlone, EveryObject, SomeObjects };

        Passes passes { Passes::SomeObjects };
        uint16_t primitives { 0 };
    };
    HashMap<uint32_t, TestOfEdge, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_testsOfEdges;
    HashMap<uint32_t, std::unique_ptr<InstanceOfGuardedReads>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_instanceOfGuardedReads;

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesOnDemand;
    Vector<uint32_t> m_extraPropertyNodes;
    Vector<uint32_t> m_joinedPropertyNodes;
    struct CopyOfAppendedFields {
        FunctionID function;
        uint32_t record { 0 };
        LinkType subject;
        LinkType other;
        size_t numberOfAppendedFields { 0 };
    };
    Vector<CopyOfAppendedFields> m_copiesOfAppendedFields;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_notedAppendedFieldCopies;
    HashSet<std::pair<uint64_t, uint64_t>, DefaultHash<std::pair<uint64_t, uint64_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_appendedFieldsCopied;
    BitVector m_layoutsWithJoinedProperties;
    void joinExtraProperties(LayoutID, FlowNodeID joined);
    Vector<uint32_t> m_unknownKeyStoreNodes;
    Vector<uint32_t> m_numericKeyStoreNodes;
    Vector<uint32_t> m_nonNumericKeyReadNodes;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_unseenReplacementNodes;
    Vector<uint32_t> m_nodesStoredWhereAccessorStandsAbove;
    Vector<Vector<OwnValue>> m_ownValuesOfLayouts;
    BitVector m_layoutsWithListedFieldWithoutKey;
    BitVector m_layoutsWhoseOwnValuesAreListed;
    Vector<OwnValue, 8> listOwnValuesOf(LayoutID);
    Vector<LayoutID> m_extraPropertyNodeLayouts;
    static constexpr uint32_t keyOfStoreToTieIsNumber = 1u << 31;
    Vector<std::pair<uint32_t, uint32_t>> m_storesUnderUnknownNamesToTie;
    struct alignas(128) PartOfNodesThatDefinitionsWatch {
        std::atomic<bool> isLocked { false };
        HashSet<std::pair<uint64_t, uint64_t>> watched;
    };
    static constexpr unsigned log2OfPartsOfNodesThatDefinitionsWatch = 6;
    std::array<PartOfNodesThatDefinitionsWatch, 1 << log2OfPartsOfNodesThatDefinitionsWatch> m_nodesOfDescriptorsThatDefinitionsWatch;
    BitVector m_openLayouts;
    struct AppendedField {
        PropertyKeyID key;
        FlowNodeID node;
        uint32_t next { UINT32_MAX };
        FlowNodeID layoutStoreNode { };
    };
    Vector<AppendedField> m_appendedFields;
    struct AccessUnderNamesOfRecords {
        FlowObserver access;
        FlowNodeID records;
        size_t numberOfNames;
    };
    Vector<AccessUnderNamesOfRecords> m_accessesUnderNamesOfRecords;
    size_t m_namesWhenAccessesWereTold { 0 };
    struct AppendedNames {
        Vector<uint32_t> places;
        size_t numberOfHeldNames { 0 };
    };
    HashMap<uint32_t, AppendedNames, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_appendedNamesOfLayouts;
    size_t countAppendedNames(LayoutID layout, const AppendedNames& names) const { return program.at(layout).kind == LayoutKind::Instance ? names.numberOfHeldNames : names.places.size(); }
    bool isAppendedName(uint32_t place) const { return m_appendedFields[place].layoutStoreNode || !typeAt(m_appendedFields[place].node).isNever(); }
    void collectAppendedNames(LayoutID, AppendedNames&) const;
    size_t m_numberOfPrototypeStores { 0 };
    mutable Lock m_lockOfCallsInSight;
    mutable HashMap<uint32_t, std::unique_ptr<CallsInSight>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callsInSight WTF_GUARDED_BY_LOCK(m_lockOfCallsInSight);
    mutable HashMap<uint32_t, std::unique_ptr<CallsInBody>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callsInBodies WTF_GUARDED_BY_LOCK(m_lockOfCallsInSight);
    mutable HashMap<uint32_t, std::unique_ptr<ClosuresInBody>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closuresInBodies WTF_GUARDED_BY_LOCK(m_lockOfCallsInSight);
    mutable HashMap<uint32_t, Vector<BlockID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksBehindTestsByCalls WTF_GUARDED_BY_LOCK(m_lockOfCallsInSight);
    mutable std::once_flag m_exportedVariablesAreListed;
    mutable BitVector m_exportedVariables;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAccessesUnderNames;
    struct ReadThroughFamily {
        PropertyKeyID key;
        FlowNodeID aggregate;
    };
    struct SortedMembersOfFamily {
        Vector<LayoutID> tiedOneByOne;
        Vector<LayoutID> instancesOfClasses;
        Vector<LayoutID> topmostInstancesOfClasses;
        Vector<LayoutID> instancesOfClassesThatIndexOfNamesLeavesOut;
        Vector<LayoutID> withNothingAbove;
        HashMap<uint32_t, Vector<LayoutID, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> heirsOfMembers;
        bool someInstanceOfClassMayHaveAppendedFields { false };
        HashMap<uint32_t, Vector<LayoutID, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersWithNothingAboveByKey;
        uint32_t numberWithNothingAbove { 0 };
        FlowNodeID foundBesideTheirFields;
        struct MembersListingName {
            Vector<uint32_t> places;
            FlowNodeID fields;
        };
        HashMap<uint32_t, MembersListingName, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersListingName;
        std::optional<bool> hasOnlyWrittenClasses;
        Vector<uint32_t> placesOfMembers;
        uint32_t numberLost { 0 };
        Vector<ReadThroughFamily> readsWithoutLostMember;
    };
    bool hasNothingAbove(LayoutID);
    BitVector m_layoutsWithNothingAbove;
    bool m_hasListedLayoutsWithNothingAbove { false };
    HashMap<uint64_t, Vector<FlowNodeID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_appendedFieldsThatWaitForRead;
    SortedMembersOfFamily& sortedMembersOf(FamilyID);
    bool noteLostMemberWithNothingAbove(FamilyID, LayoutID);
    void tieReadThroughSortedMembers(FlowNodeID aggregate, FamilyID, PropertyKeyID);
    bool tieReadThroughClassesThatDeclare(const SortedMembersOfFamily&, FlowNodeID aggregate, FamilyID, PropertyKeyID);
    bool isMemberOf(FamilyID, LayoutID) const;
    struct DeclarerOfMember {
        uint32_t key { 0 };
        LayoutID classLayout;
        MethodKind kind { MethodKind::Method };
    };
    std::span<const DeclarerOfMember> classesThatDeclareOnPrototype(PropertyKeyID) LIFETIME_BOUND;
    Vector<DeclarerOfMember> m_declarersOnPrototypesByKey;
    bool m_hasListedDeclarersOnPrototypes { false };
    struct FieldBornBesideFields {
        LayoutID layout;
        FlowNodeID node;
    };
    HashMap<uint32_t, Vector<FieldBornBesideFields, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsBornBelowClassesByKey;
    HashMap<uint64_t, Vector<FlowObserver, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keepersOfSleepersByName;
    FlowNodeID findNodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID) const;
    HashMap<uint32_t, std::unique_ptr<SortedMembersOfFamily>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sortedMembersOfFamilies;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAppendedFields;
    Vector<uint32_t> m_newestAppendedFieldOfLayout;
    BitVector m_lostLayoutsWhoseAppendedFieldsAreTied;
    BitVector m_layoutsWithPooledAppendedFields;
    struct Site {
        BuiltinClassID builtinClass;
        BuiltinLayoutID layoutOfRecords;
        bool hasEscaped { false };
        bool isToldToEscape { false };
        bool isClimbedFrom { false };
        bool mayHoldCellsOfCompiledLayouts { false };
        bool onlyWaitRightBehindSeesIt : 1 { false };
        bool holdsFunctionsAlone : 1 { false };
        bool isOfClassItself { true };
        bool isCreatedWithoutHoles { true };
        bool isCreatedWithoutEngineNames { true };
        uint32_t writtenLength { 0 };
        uint32_t newestContents { UINT32_MAX };
        uint32_t numberOfContents { 0 };
    };
    struct ContentsOfSite {
        SlotOfContents slot;
        FlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    // SITES ARE MADE WHILE SEVERAL THREADS TELL OBSERVERS, WITHOUT THE LOCK OF WHAT IS SHARED, and are read without any. So a site never moves: the blocks double, and there are as many as a number has bits.
    class ListOfSites {
        WTF_MAKE_NONCOPYABLE(ListOfSites);
    public:
        ListOfSites() = default;
        ~ListOfSites()
        {
            for (auto& block : m_blocks)
                delete[] block.load(std::memory_order_relaxed);
        }
        size_t size() const { return m_size.load(std::memory_order_relaxed); }
        Site& operator[](size_t index)
        {
            size_t position = index + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            return m_blocks[block].load(std::memory_order_acquire)[position - (static_cast<size_t>(1) << (block + log2OfFirstBlock))];
        }
        template<typename Functor>
        void forEachIn(size_t first, size_t end, const Functor& functor)
        {
            while (first < end) {
                size_t sizeOfBlock = std::bit_floor(first + (static_cast<size_t>(1) << log2OfFirstBlock));
                size_t firstOfBlock = sizeOfBlock - (static_cast<size_t>(1) << log2OfFirstBlock);
                std::span<Site> sites = unsafeMakeSpan(&(*this)[firstOfBlock], sizeOfBlock).first(std::min(sizeOfBlock, end - firstOfBlock));
                for (Site& site : sites.subspan(first - firstOfBlock))
                    functor(first++, site);
            }
        }
        uint32_t append(const Site& site)
        {
            size_t index = m_size.fetch_add(1, std::memory_order_relaxed);
            RELEASE_ASSERT(index < UINT32_MAX);
            ensureBlockOf(index);
            (*this)[index] = site;
            return static_cast<uint32_t>(index);
        }
        void growTo(size_t size)
        {
            if (size <= this->size())
                return;
            for (size_t index = 0; index < size; index = 2 * index + (static_cast<size_t>(1) << log2OfFirstBlock))
                ensureBlockOf(index);
            m_size.store(size, std::memory_order_relaxed);
        }

    private:
        void ensureBlockOf(size_t index)
        {
            size_t position = index + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            if (m_blocks[block].load(std::memory_order_acquire))
                return;
            while (m_blockIsLaidOut.exchange(true, std::memory_order_acquire))
                simde_mm_pause();
            if (!m_blocks[block].load(std::memory_order_relaxed))
                m_blocks[block].store(new Site[static_cast<size_t>(1) << (block + log2OfFirstBlock)], std::memory_order_release);
            m_blockIsLaidOut.store(false, std::memory_order_release);
        }

        static constexpr unsigned log2OfFirstBlock = 10;
        std::array<std::atomic<Site*>, 33 - log2OfFirstBlock> m_blocks { };
        std::atomic<size_t> m_size { 0 };
        std::atomic<bool> m_blockIsLaidOut { false };
    };
    mutable ListOfSites m_sites;
    struct WrittenList {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WrittenList);
        FunctionID function;
        uint32_t firstPlace { 0 };
        uint32_t numberOfPlaces { 0 };
        WrittenList* next { nullptr };
        WrittenList* lastOfChain { nullptr };
        uint32_t numberOfPlacesOfFamily { 0 };
    };
    Vector<std::unique_ptr<WrittenList>> m_writtenLists;
    HashMap<uint32_t, WrittenList*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_writtenListsOfFamilies;
    void layWhatIsWrittenAt(const WrittenList&, uint32_t place, FlowNodeID);
    void layWhatListsFromWriteAt(const WrittenList* first, SlotOfContents, FlowNodeID);
    UnitedFamilies m_familiesOfSites;
    struct PartOfContentsOfSites {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PartOfContentsOfSites);
        Vector<ContentsOfSite> listed;
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> ofFamiliesWithManySlots;
    };
    Vector<std::unique_ptr<PartOfContentsOfSites>> m_contentsOfSites;
    unsigned partOfContentsOf(uint32_t site) const { return (site >> 6) & m_maskOfLanes; }
    Vector<ContentsOfSite>& contentsListedOf(uint32_t site) const { return m_contentsOfSites[partOfContentsOf(site)]->listed; }
    struct alignas(128) PartOfSitesOfMakers {
        std::atomic<bool> isLocked { false };
        HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> sites;
    };
    static constexpr unsigned log2OfPartsOfSitesOfMakers = 8;
    std::unique_ptr<std::array<PartOfSitesOfMakers, 1 << log2OfPartsOfSitesOfMakers>> m_sitesOfMakers { makeUniqueWithoutFastMallocCheck<std::array<PartOfSitesOfMakers, 1 << log2OfPartsOfSitesOfMakers>>() };
    Vector<uint32_t> m_firstSiteOfFunction;
    void fillSiteJustMade(SiteID);
    Vector<uint32_t> m_nodesFoundOnClosures;
    Vector<uint32_t> m_nodesFoundOnClassObjectsFrom;
    bool m_hasStoresThroughLost { false };

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfLostKeys;
    Vector<LostFamily> m_lostFamilies;
    bool m_anyFieldIsReadThroughLost { false };
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysLookedUpThroughLost;
    Vector<LostKey> m_lostKeys;
    mutable HashMap<uint32_t, BuiltinID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsOnObjectPrototype;
    Vector<HolderOfLostKey> m_holdersOfLostKeys;
    Vector<LayoutID> m_layoutsToTie;
    bool m_lostLayoutsMayHaveGrown { false };

    Vector<PoolOfLostFunctions> m_poolsOfLostFunctions;
    mutable Vector<uint32_t> m_poolOfFamilyOfFunctions;
    mutable Vector<std::pair<uint32_t, uint32_t>> m_poolsOfLostFunctionsToTie;
    size_t m_numberOfPoolsTiedToPoolOfAll { 1 };
    TiesOfPoolsInLinkFlow m_tiesOfPools;
    mutable BitVector m_calledFunctionFamilies;
    std::atomic<uint32_t> m_whereInstancesAreMadeThenables { UINT32_MAX };
    std::atomic<uint32_t> m_whereInstancesLoseTheirConstructor { UINT32_MAX };
    BitVector m_lostFunctionsTied;
    size_t m_numberOfLostFunctionsTied { 0 };
    bool m_lostFunctionsAreCalled { false };
    mutable BitVector m_lostFunctionsThatCallsName;
    mutable bool m_someCallNamesLostFunctions { false };

    bool mayBeNamedByNumber(PropertyKeyID key) const { return !key || mayBeElementKey(nameClassOf(key)); }
    FlowNodeID foundBesideFieldsUnderAnyNumber();
    std::pair<FlowNodeID, bool> aggregateOfReadsUnderNumbers(Vector<uint32_t>& nodes, size_t numberOfHolders, uint32_t holder);
    void passPrototypesAboveUnderNumbers(LayoutID);
    uint32_t m_foundBesideFieldsUnderAnyNumber { UINT32_MAX };
    Vector<uint32_t> m_elementsReadThroughLayouts;
    Vector<uint32_t> m_elementsReadThroughFamilies;
    Vector<uint32_t> m_elementsReadThroughLostFamilies;
    Vector<uint32_t> m_elementsStoredThroughLayouts;
    Vector<uint32_t> m_elementsStoredThroughFamilies;
    Vector<uint32_t> m_elementsSeenOnLayouts;
    BitVector m_prototypesPassedByNumbers;
};

class LayoutsByNames;

struct NamesAskedOfContentsOfFields {
    Vector<uint64_t> cells;
    Vector<std::pair<uint32_t, uint32_t>> cellsOfGroups;
    Vector<std::pair<uint32_t, uint32_t>> namesOfGroups;
    Vector<std::pair<uint32_t, uint64_t>> valuesOfGroups;
    struct NamesReadFromField {
        FieldReference field;
        Vector<PropertyKeyID, 4> names;
    };
    Vector<NamesReadFromField> fields;
};

struct TurnsOfReadings {
    LinkTables held;
    const LinkTables* proving { nullptr };
    BinarySemaphore provingTablesAreWritten;
    std::unique_ptr<LinkFlow> provingFlow;
};
LinkTables link(std::unique_ptr<LinkFlow>&&, TurnsOfReadings* = nullptr);
enum class WrappedBodies : bool { StoreLikeModuleBodies, AreFunctions };
LinkTables linkBeforeEvaluation(const LinkFlow& flowAtStart, WrappedBodies);
void finishReadingsOfLink(Program&, Mechanisms, TurnsOfReadings&, LinkTables& proven, LinkTables& held);
NamesAskedOfContentsOfFields gatherNamesAskedOfContentsOfFields(const Program&, const LinkFlow&, const LinkTables& held);
void expectContentsOfFieldsByNamesAsked(Program&, const LinkFlow&, const NamesAskedOfContentsOfFields&, const LayoutsByNames&, LinkTables& held);
void printBreachesOfClosureOfLink(PrintStream&, Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&);
void summarizeCallees(const LinkFlow&, LinkTables&);
void writePrototypeObjectsThatAreSettled(const LinkFlow&, LinkTables&);
void layReferrersOfHeldReadingOver(LinkTables& proven, LinkTables& held, const ParallelLoops&);
void rankReferrersFirst(LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
