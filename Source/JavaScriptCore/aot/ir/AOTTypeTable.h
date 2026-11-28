/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIDSets.h"
#include "AOTObjects.h"
#include "AOTSchemaRecords.h"
#include "AOTSharedAnswers.h"
#include "AOTType.h"
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct DeclaredType;

struct ObjectSetSummary {
    LayoutSetID layouts;
    ClassPartsID classes;
    OptionSet<ObjectTrait> traits;
};
inline constexpr uint32_t wordsOfObjectSetSummary = 3;

inline constexpr uint32_t log2OfFirstInternedSegment = 8;
inline constexpr uint32_t sizeOfFirstInternedSegment = 1u << log2OfFirstInternedSegment;

constexpr uint32_t internedSegmentOf(uint32_t position)
{
    return static_cast<uint32_t>(std::bit_width(position + sizeOfFirstInternedSegment)) - 1 - log2OfFirstInternedSegment;
}

constexpr uint32_t firstPositionOfInternedSegment(uint32_t segment)
{
    return (sizeOfFirstInternedSegment << segment) - sizeOfFirstInternedSegment;
}

inline constexpr uint32_t numberOfInternedSegments = internedSegmentOf(largestIDPosition) + 1;

template<typename Record>
class InternedRecords {
    WTF_MAKE_NONCOPYABLE(InternedRecords);
public:
    static constexpr uint32_t numberOfShards = 64;

    InternedRecords();
    ~InternedRecords();

    const Record& at(uint32_t position) const;
    std::optional<uint32_t> find(uint32_t hash, const Record&) const;
    uint32_t findOrAdd(uint32_t hash, const Record&);
    uint32_t addAfterMiss(uint32_t hash, const Record&);
    using PrepareCandidate = ScopedLambda<std::optional<uint32_t>(uint32_t candidate, unsigned worker)>;
    using FinishCandidate = ScopedLambda<void(uint32_t candidate, uint32_t position, bool isNew, unsigned worker)>;
    void findOrAddInOrder(const ParallelLoops&, std::span<Record> candidates, NOESCAPE const PrepareCandidate&, NOESCAPE const FinishCandidate&);
    void freeOutgrownSlots();
    uint32_t size() const;
    size_t sizeInBytes() const;

private:
    void addAt(uint32_t position, uint32_t hash, const Record&);

    struct Shard;
    std::unique_ptr<Shard[]> m_shards;
};

class InternedWordLists {
    WTF_MAKE_NONCOPYABLE(InternedWordLists);
public:
    InternedWordLists();
    ~InternedWordLists();

    std::span<const uint32_t> at(uint32_t position) const;
    std::optional<uint32_t> find(uint32_t hash, std::span<const uint32_t>) const;
    uint32_t findOrAdd(uint32_t hash, std::span<const uint32_t>);
    void freeOutgrownSlots();
    size_t sizeInBytes() const;

private:
    struct Shard;
    std::unique_ptr<Shard[]> m_shards;
};

constexpr uint64_t addToInternedHash(uint64_t hash, uint64_t bits)
{
    hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
    return hash ^ hash >> 32;
}

constexpr uint64_t addToInternedHash(uint64_t hash, uint32_t first, uint32_t second)
{
    return addToInternedHash(hash, static_cast<uint64_t>(first) << 32 | second);
}

constexpr uint32_t finishInternedHash(uint64_t hash)
{
    return static_cast<uint32_t>(addToInternedHash(hash, 0));
}

ALWAYS_INLINE uint32_t hashOf(const TypeParts& parts)
{
    uint64_t flags = parts.primitives
        | static_cast<uint64_t>(parts.seeds.toRaw()) << 16
        | static_cast<uint64_t>(parts.anyObject) << 32
        | static_cast<uint64_t>(parts.anyFunction) << 33
        | (parts.opaque ? static_cast<uint64_t>(*parts.opaque) + 1 : 0) << 34;
    uint64_t hash = addToInternedHash(0, flags);
    if (!parts.integers.isEmpty()) {
        hash = addToInternedHash(hash, static_cast<uint64_t>(parts.integers.minimum));
        hash = addToInternedHash(hash, static_cast<uint64_t>(parts.integers.maximum));
    }
    if (!parts.boundsOfFractions.isEmpty()) {
        hash = addToInternedHash(hash, static_cast<uint64_t>(parts.boundsOfFractions.minimum));
        hash = addToInternedHash(hash, static_cast<uint64_t>(parts.boundsOfFractions.maximum));
    }
    hash = addToInternedHash(hash, parts.environment.bits(), parts.stringLiterals.bits());
    hash = addToInternedHash(hash, parts.symbolLiterals.bits(), parts.objects.bits());
    hash = addToInternedHash(hash, parts.unnamedLayouts.bits(), parts.unnamedClasses.bits());
    return finishInternedHash(hash);
}

uint32_t hashOf(std::span<const uint32_t> words);
uint32_t hashOf(const PropertyKey&);

class TypeTable {
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    explicit TypeTable(const BuiltinTable&, const Program* = nullptr);
    ~TypeTable();

    static constexpr TypeID never = typeOfPrimitives(0);
    static constexpr TypeID undefined = typeOfPrimitives(Primitive::Undefined);
    static constexpr TypeID null = typeOfPrimitives(Primitive::Null);
    static constexpr TypeID nullish = typeOfPrimitives(Primitive::UndefinedOrNull);
    static constexpr TypeID absent = typeOfPrimitives(Primitive::Absent);
    static constexpr TypeID boolean = typeOfPrimitives(Primitive::Boolean);
    static constexpr TypeID int32 = *tryMakeTypeOfPrimitives(Primitive::Integer, Range::int32());
    static constexpr TypeID uint32 = *tryMakeTypeOfPrimitives(Primitive::Integer, Range::uint32());
    static constexpr TypeID length = *tryMakeTypeOfPrimitives(Primitive::Integer, Range::length());
    static constexpr TypeID integer = typeOfPrimitives(Primitive::Integer);
    static constexpr TypeID number = typeOfPrimitives(Primitive::Number);
    static constexpr TypeID string = typeOfPrimitives(Primitive::String);
    static constexpr TypeID symbol = typeOfPrimitives(Primitive::Symbol);
    static constexpr TypeID bigInt = typeOfPrimitives(Primitive::BigInt);
    // The constructor interns these first, in this order, without seeds: for the target of a test, a bound, the widest type of a representation.
    static constexpr TypeID function = idAt<TypeID>(IDHome::Program, 0);
    static constexpr TypeID object = idAt<TypeID>(IDHome::Program, 1);
    static constexpr TypeID anything = idAt<TypeID>(IDHome::Program, 2);
    static constexpr TypeID anythingOrAbsent = idAt<TypeID>(IDHome::Program, 3);
    static constexpr TypeID cell = idAt<TypeID>(IDHome::Program, 4);
    static constexpr TypeID cellOrMarker = idAt<TypeID>(IDHome::Program, 5);
    static constexpr TypeID int16 = idAt<TypeID>(IDHome::Program, 6);
    static constexpr TypeID uint16 = idAt<TypeID>(IDHome::Program, 7);
    static constexpr TypeID anythingButUndefined = idAt<TypeID>(IDHome::Program, 8);
    static constexpr TypeID anythingButNull = idAt<TypeID>(IDHome::Program, 9);
    static constexpr TypeID anythingButNullish = idAt<TypeID>(IDHome::Program, 10);
    static constexpr TypeID anythingButString = idAt<TypeID>(IDHome::Program, 11);
    static constexpr TypeID anythingButNumber = idAt<TypeID>(IDHome::Program, 12);
    static constexpr TypeID anythingButBoolean = idAt<TypeID>(IDHome::Program, 13);
    static constexpr uint32_t numberOfTypesInternedAtStart = 14;
    static constexpr bool passesEveryObject(TypeID id) { return id == anything || (id.bits() >= anythingButUndefined.bits() && id.bits() <= anythingButBoolean.bits()); }
    static constexpr bool isNonNullishTop(TypeID id) { return id == anythingButUndefined || id == anythingButNull || id == anythingButNullish; }
    static constexpr TypeID anythingBut(TypeID primitives)
    {
        if (primitives == undefined)
            return anythingButUndefined;
        if (primitives == null)
            return anythingButNull;
        if (primitives == nullish)
            return anythingButNullish;
        if (primitives == string)
            return anythingButString;
        if (primitives == number)
            return anythingButNumber;
        if (primitives == boolean)
            return anythingButBoolean;
        return { };
    }
    static constexpr TypeID valuesOf(TypeID held) { return held == anythingOrAbsent ? anything : held; }

    const BuiltinTable& builtins() const { return m_builtins; }
    const Program* program() const { return m_program; }

    void setObjects(std::unique_ptr<ObjectTable>&&);
    ObjectTable& objects() { return *m_objects; }
    const ObjectTable& objects() const { return *m_objects; }
    void reserveMembersOfLayoutSets(uint32_t numberOfLayouts) { m_layoutSets.reserveMembers(numberOfLayouts); }

    Type at(TypeID) const;
    const Type& typeKeptAt(TypeID) const;
    JS_EXPORT_PRIVATE const Type& typeKeptBeforeFreezeAt(TypeID) const;
    static Type typeInID(TypeID);
    static constexpr uint32_t positionInProgramOf(TypeID id) { return id.bits() - (static_cast<uint32_t>(IDHome::Program) << idHomeShift); }
    TypeID typeOfLiteralString(StringID string) const { return string.index() < m_typesOfLiteralStrings.size() ? m_typesOfLiteralStrings[string.index()] : TypeID(); }
    SetView<LayoutID, LayoutSetID> at(LayoutSetID set) const { return SetView<LayoutID, LayoutSetID>(m_layoutSets.viewOf(set.bits())); }
    std::span<const LayoutID> listOf(LayoutSetID set) const
    {
        static_assert(sizeof(LayoutID) == sizeof(uint32_t));
        std::span<const uint32_t> numbers = m_layoutSets.listOf(set.bits());
        return unsafeMakeSpan(reinterpret_cast<const LayoutID*>(numbers.data()), numbers.size());
    }
    SetView<ObjectID, ObjectSetID> at(ObjectSetID set) const { return SetView<ObjectID, ObjectSetID>(m_objectSets.viewOf(set.bits())); }
    SetView<StringID, StringSetID> at(StringSetID set) const { return SetView<StringID, StringSetID>(m_stringSets.viewOf(set.bits())); }
    SetView<PropertyKeyID, KeySetID> at(KeySetID set) const { return SetView<PropertyKeyID, KeySetID>(m_keySets.viewOf(set.bits())); }
    std::span<const ClassPart> at(ClassPartsID) const;
    const PropertyKey& at(PropertyKeyID) const;
    JS_EXPORT_PRIVATE void keepAddressesOfPropertyKeys();
    JS_EXPORT_PRIVATE void keepAddressesOfTypes();

    TypeID typeOfWireIndex(uint32_t index) const { return m_typeOfWireIndex[index]; }
    std::span<const TypeID> typesOfWireIndexes() const LIFETIME_BOUND { return m_typeOfWireIndex.span(); }
    void setTypesOfWireIndexes(Vector<TypeID>&& types) { m_typeOfWireIndex = WTF::move(types); }

    uint32_t numberOfTypes() const { return m_types.size(); }

    unsigned numberOfPropertyKeys() const;
    void endStableNumbersOfPropertyKeys();
    void endStableNumbersOfPropertyKeys(const ParallelLoops&);
    void addKeysOfTexts(uint32_t numberOfStrings, const ParallelLoops&);
    bool hasStableNumber(PropertyKeyID key) const { return key.index() < m_numberOfKeysWithStableNumbers; }
    bool isLookedUpByName(PropertyKeyID key) const { return key.index() < m_keysLookedUpByName.size() && m_keysLookedUpByName[key.index()]; }
    std::optional<uint8_t> locationBitOf(PropertyKeyID key) const
    {
        if (key.index() >= m_locationBitsOfKeys.size())
            return std::nullopt;
        return m_locationBitsOfKeys[key.index()];
    }
    std::strong_ordering compareByContent(PropertyKeyID first, PropertyKeyID second) const
    {
        auto contentOf = [](const PropertyKey& key) {
            const auto& [kind, text, owner, wellKnownSymbol, module, callNumber] = key;
            return std::tuple(kind, text, owner, wellKnownSymbol, module, callNumber);
        };
        return contentOf(at(first)) <=> contentOf(at(second));
    }
    std::optional<PropertyKeyID> find(const PropertyKey&) const;

    ALWAYS_INLINE ProvenInEveryVersion provenTypeOf(const DeclaredType& declared) const
    {
        if (std::optional<ProvenInEveryVersion> established = declared.established())
            return *established;
        return provenTypeOfWhatIsOnlyExpected(declared);
    }
    ProvenInEveryVersion provenTypeOfWhatIsOnlyExpected(const DeclaredType&) const;

    void beginKeepingMemosAtRest();
    void invalidateMemosAtRest();

    void freeze();
    bool isFrozen() const { return m_isFrozen; }
    class RoundWithoutAdditions {
        WTF_MAKE_NONCOPYABLE(RoundWithoutAdditions);
    public:
        explicit RoundWithoutAdditions(TypeTable& table)
            : m_table(table)
        {
            m_table.m_numberOfRoundsWithoutAdditions.fetch_add(1, std::memory_order_relaxed);
            for (IDSetTable* sets : { &m_table.m_layoutSets, &m_table.m_objectSets, &m_table.m_stringSets, &m_table.m_keySets })
                sets->beginRoundWithoutAdditions();
        }
        ~RoundWithoutAdditions()
        {
            for (IDSetTable* sets : { &m_table.m_layoutSets, &m_table.m_objectSets, &m_table.m_stringSets, &m_table.m_keySets })
                sets->endRoundWithoutAdditions();
            m_table.m_numberOfRoundsWithoutAdditions.fetch_sub(1, std::memory_order_relaxed);
        }

    private:
        TypeTable& m_table;
    };
    bool takesNoAdditions() const { return m_isFrozen || m_numberOfRoundsWithoutAdditions.load(std::memory_order_relaxed); }
    SharedAnswers* sharedAnswers() const { return m_sharedAnswers.get(); }

    size_t sizeInBytes() const;

private:
    friend class TypeArena;
    friend class ArenasOfWorkers;

    std::span<const ClassPart> classPartsKeptAt(ClassPartsID) const;
    const PropertyKey& propertyKeyKeptAt(PropertyKeyID) const;
    const PropertyKey& propertyKeyFoundByAddressAt(uint32_t position) const;

    struct MemosOfArena {
        IDPairMemo types;
        IDPairMemo layoutSets;
        IDPairMemo objectSets;
        IDPairMemo stringSets;
        IDPairMemo keySets;
        IDListMemo unionsOfMany;
    };
    mutable Lock m_memosAtRestLock;
    bool m_keepsMemosAtRest WTF_GUARDED_BY_LOCK(m_memosAtRestLock) { false };
    Vector<MemosOfArena> m_memosAtRest WTF_GUARDED_BY_LOCK(m_memosAtRestLock);

    Vector<ClassPart> m_partOfClassAlone;

    const BuiltinTable& m_builtins;
    const Program* m_program;
    std::unique_ptr<ObjectTable> m_objects;
    bool m_isFrozen { false };
    std::unique_ptr<SharedAnswers> m_sharedAnswers;
    InternedRecords<Type> m_types;
    Vector<const Type*> m_typesOnceFrozen;
    uint32_t m_numberOfTypesFoundByAddress { 0 };
    std::array<uintptr_t, numberOfInternedSegments> m_addressesOfPositionZeroInTypeSegments { };
    std::atomic<unsigned> m_numberOfRoundsWithoutAdditions { 0 };
    IDSetTable m_layoutSets { IDSetForm::ChunksWithLazyList, 0 };
    IDSetTable m_objectSets { IDSetForm::ListsAndChunks, wordsOfObjectSetSummary };
    IDSetTable m_stringSets { IDSetForm::ListsAndChunks, 0 };
    IDSetTable m_keySets { IDSetForm::ListsAndChunks, 0 };
    InternedWordLists m_classParts;
    InternedRecords<PropertyKey> m_propertyKeys;
    uint32_t m_numberOfPropertyKeysFoundByAddress { 0 };
    std::array<uintptr_t, numberOfInternedSegments> m_addressesOfPositionZeroInKeySegments { };
    uint32_t m_numberOfKeysWithStableNumbers { UINT32_MAX };
    Vector<uint8_t> m_locationBitsOfKeys;
    Vector<uint8_t> m_keysLookedUpByName;
    Vector<TypeID> m_typeOfWireIndex;
    Vector<TypeID> m_typesOfLiteralStrings;
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE const Type& TypeTable::typeKeptAt(TypeID id) const
{
    uint32_t position = positionInProgramOf(id);
    if (position < m_typesOnceFrozen.size()) [[likely]]
        return *m_typesOnceFrozen[position];
    if (position < m_numberOfTypesFoundByAddress) [[likely]]
        return *reinterpret_cast<const Type*>(m_addressesOfPositionZeroInTypeSegments.data()[internedSegmentOf(position)] + static_cast<uintptr_t>(position) * sizeof(Type));
    return typeKeptBeforeFreezeAt(id);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

ALWAYS_INLINE Type TypeTable::typeInID(TypeID id)
{
    Type type;
    type.primitives = primitivesInID(id);
    type.integers = integersInID(id);
    return type;
}

ALWAYS_INLINE Type TypeTable::at(TypeID id) const
{
    if (isItsOwnValue(id))
        return typeInID(id);
    return typeKeptAt(id);
}

inline std::span<const ClassPart> TypeTable::at(ClassPartsID id) const
{
    if (holdsOneMember(id))
        return std::span<const ClassPart>(&m_partOfClassAlone[positionOf(id)], 1);
    return classPartsKeptAt(id);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE const PropertyKey& TypeTable::propertyKeyFoundByAddressAt(uint32_t position) const
{
    return *reinterpret_cast<const PropertyKey*>(m_addressesOfPositionZeroInKeySegments.data()[internedSegmentOf(position)] + static_cast<uintptr_t>(position) * sizeof(PropertyKey));
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

ALWAYS_INLINE const PropertyKey& TypeTable::at(PropertyKeyID key) const
{
    if (key.bits() < m_numberOfPropertyKeysFoundByAddress) [[likely]]
        return propertyKeyFoundByAddressAt(key.bits());
    return propertyKeyKeptAt(key);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
