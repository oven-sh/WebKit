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

// What is kept beside each set of several objects, so that a type's layouts, classes and traits are read and never walked for.
struct ObjectSetSummary {
    LayoutSetID layouts;
    ClassPartsID classes;
    OptionSet<ObjectTrait> traits;
};
inline constexpr uint32_t wordsOfObjectSetSummary = 3;

// Records that are interned by content and never move. Any thread may add while the table grows.
// Positions are dense and follow the order of arrival: the first record added has position 0. So what one thread adds alone is numbered by the program alone.
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
    // For whoever has just asked find() in vain. Another thread may have added the record since.
    uint32_t addAfterMiss(uint32_t hash, const Record&);
    // What findOrAdd() answers to one thread that asks for the candidates in turn, worked out on every thread: the positions follow from the candidates alone, at any number of threads.
    // `prepare` writes a candidate, whose memory holds nothing until then, and answers its hash. A candidate without a hash wants no record, and nothing more is heard of it.
    // `finish` hears the position of every other candidate. A candidate that is new is copied into the table behind the call, so this is where what operator== passes over is filled in.
    // Each is called once for a candidate, on any thread, and neither adds to this table. By the thread that may start a loop, while no other thread adds.
    using PrepareCandidate = ScopedLambda<std::optional<uint32_t>(uint32_t candidate, unsigned worker)>;
    using FinishCandidate = ScopedLambda<void(uint32_t candidate, uint32_t position, bool isNew, unsigned worker)>;
    void findOrAddInOrder(const ParallelLoops&, std::span<Record> candidates, NOESCAPE const PrepareCandidate&, NOESCAPE const FinishCandidate&);
    // By one thread, while no other thread is at the table.
    void freeOutgrownSlots();
    uint32_t size() const;
    size_t sizeInBytes() const;

private:
    // For a record that find() misses, at a position that the caller has taken, while nobody else adds.
    void addAt(uint32_t position, uint32_t hash, const Record&);

    struct Shard;
    std::unique_ptr<Shard[]> m_shards;
};

// The same for lists of words: the parts of classes.
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

uint32_t hashOf(const TypeParts&);
uint32_t hashOf(std::span<const uint32_t> words);
uint32_t hashOf(const PropertyKey&);

// The types of the program: what the reader, Facts and Plan made. They last for the build.
// Nothing is added here directly: every maker goes through a TypeArena.
//   while it grows    arenas with ArenaKeeps::InProgram add from any thread
//   once frozen       nobody adds, reads take no lock, and arenas with ArenaKeeps::UntilReset keep what is new to themselves
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
    static constexpr TypeID anyPrimitive = typeOfPrimitives(Primitive::All);
    // The constructor interns these first, in this order, without seeds: for the target of a test, a bound, the widest type of a representation.
    // A value that is unknown has seeds: TypeArena::anything(UnknownSeed).
    static constexpr TypeID function = idAt<TypeID>(IDHome::Program, 0);
    static constexpr TypeID object = idAt<TypeID>(IDHome::Program, 1);
    static constexpr TypeID anything = idAt<TypeID>(IDHome::Program, 2);
    static constexpr TypeID anythingOrAbsent = idAt<TypeID>(IDHome::Program, 3);
    // Strings, symbols, BigInts and objects. The second with undefined, null and absent beside them.
    static constexpr TypeID cell = idAt<TypeID>(IDHome::Program, 4);
    static constexpr TypeID cellOrMarker = idAt<TypeID>(IDHome::Program, 5);
    // The elements of an Int16Array and of a Uint16Array. No id holds these ranges by itself, and elementTypeOf() has no arena to make them in.
    static constexpr TypeID int16 = idAt<TypeID>(IDHome::Program, 6);
    static constexpr TypeID uint16 = idAt<TypeID>(IDHome::Program, 7);
    static constexpr uint32_t numberOfTypesInternedAtStart = 8;
    static constexpr TypeID valuesOf(TypeID held) { return held == anythingOrAbsent ? anything : held; }

    const BuiltinTable& builtins() const { return m_builtins; }
    const Program* program() const { return m_program; }

    // The reader makes the objects before it reads the first type.
    void setObjects(std::unique_ptr<ObjectTable>&&);
    ObjectTable& objects() { return *m_objects; }
    const ObjectTable& objects() const { return *m_objects; }
    // Once, by WireTypeReader::finishObjects(), when no layout will be added. Behind it the list of a set of one layout outlives the view it was read from.
    void reserveMembersOfLayoutSets(uint32_t numberOfLayouts) { m_layoutSets.reserveMembers(numberOfLayouts); }

    // All of these stop at an id of a thread's arena.
    // In line: whoever asks looks at a member or two of the 88 bytes.
    Type at(TypeID) const;
    const Type& typeKeptAt(TypeID) const;
    // The type whose one value is the text, where the table holds it. Null until freeze().
    TypeID typeOfLiteralString(StringID string) const { return string.index() < m_typesOfLiteralStrings.size() ? m_typesOfLiteralStrings[string.index()] : TypeID(); }
    SetView<LayoutID, LayoutSetID> at(LayoutSetID set) const { return SetView<LayoutID, LayoutSetID>(m_layoutSets.viewOf(set.bits())); }
    // Ascending. It lasts as long as the table. Of a wide set it is made when it is first asked for, so a set that the solve only passes through has none.
    std::span<const LayoutID> listOf(LayoutSetID set) const
    {
        static_assert(sizeof(LayoutID) == sizeof(uint32_t));
        std::span<const uint32_t> numbers = m_layoutSets.listOf(set.bits());
        return unsafeMakeSpan(reinterpret_cast<const LayoutID*>(numbers.data()), numbers.size());
    }
    SetView<ObjectID, ObjectSetID> at(ObjectSetID set) const { return SetView<ObjectID, ObjectSetID>(m_objectSets.viewOf(set.bits())); }
    // False: the solve of Facts has given the set back, and only a type that nothing holds still names it: IDSetTable::holds().
    bool holds(ObjectSetID set) const { return m_objectSets.holds(set.bits()); }
    SetView<StringID, StringSetID> at(StringSetID set) const { return SetView<StringID, StringSetID>(m_stringSets.viewOf(set.bits())); }
    SetView<PropertyKeyID, KeySetID> at(KeySetID set) const { return SetView<PropertyKeyID, KeySetID>(m_keySets.viewOf(set.bits())); }
    std::span<const ClassPart> at(ClassPartsID) const;
    const PropertyKey& at(PropertyKeyID) const;

    // A body names a type by its index in the wire's list. Generated cursors translate through this.
    TypeID typeOfWireIndex(uint32_t index) const { return m_typeOfWireIndex[index]; }
    std::span<const TypeID> typesOfWireIndexes() const LIFETIME_BOUND { return m_typeOfWireIndex.span(); }
    void setTypesOfWireIndexes(Vector<TypeID>&& types) { m_typeOfWireIndex = WTF::move(types); }

    // The records of types have the positions 0 to numberOfTypes() - 1 of IDHome::Program.
    uint32_t numberOfTypes() const { return m_types.size(); }

    // Property keys are numbered from zero in the order of the wire's list, then in the order in which the reader added its own. There endStableNumbersOfKeys() ends the stable numbers.
    // Behind them come the text keys of the other strings of the program, by StringID. What any thread adds later follows the order of arrival.
    unsigned numberOfPropertyKeys() const;
    // Once, by computeFacts(), in front of the scan of bodies. Until then every key has a stable number.
    void endStableNumbersOfPropertyKeys();
    void endStableNumbersOfPropertyKeys(const ParallelLoops&);
    // A text key for each of the strings 0 to numberOfStrings - 1. Those that have none get the next numbers, by StringID, at any number of threads. While nobody else is at the table.
    void addKeysOfTexts(uint32_t numberOfStrings, const ParallelLoops&);
    // The program names a property by the key, and the number depends on the program alone, so a hash that decides something or a sort may use it.
    bool hasStableNumber(PropertyKeyID key) const { return key.index() < m_numberOfKeysWithStableNumbers; }
    // 0 to 63, from what the key is and never from its number: the same in every program that holds the key.
    // Null for a key without a stable number, for a key of a thread's arena, for every key until endStableNumbersOfPropertyKeys(), and in a table without a program.
    std::optional<uint8_t> locationBitOf(PropertyKeyID key) const
    {
        if (key.index() >= m_locationBitsOfKeys.size())
            return std::nullopt;
        return m_locationBitsOfKeys[key.index()];
    }
    // By content, the same at any number of threads. For whoever must sort keys without stable numbers.
    std::strong_ordering compareByContent(PropertyKeyID first, PropertyKeyID second) const
    {
        // Every member of the record: with one more or one fewer this does not compile.
        auto contentOf = [](const PropertyKey& key) {
            const auto& [kind, text, owner, wellKnownSymbol, module, callNumber, site] = key;
            return std::tuple(kind, text, owner, wellKnownSymbol, module, callNumber, site);
        };
        return contentOf(at(first)) <=> contentOf(at(second));
    }
    std::optional<PropertyKeyID> find(const PropertyKey&) const;
    // What TypeArena::forObject() answers, where an arena that keeps InProgram has made it. It adds nothing. Any thread, while the table grows and once it is frozen.
    std::optional<TypeID> findTypeOfObject(ObjectID) const;

    // What holds of a position without a guard and without a fact: the written type of an established position, else anything, with absent where the written type has it.
    ProvenInEveryVersion provenTypeOf(const DeclaredType&) const;

    // From here to freeze() an arena that keeps InProgram leaves its memos to the next one, and they forget nothing. Behind the solve, by one thread.
    void beginKeepingMemosAtRest();
    // Where something that the relations of types lean on has changed: noteSubclassesOfBuiltinClasses(), noteLayoutsOfDictionaries(). An arena that lives on calls TypeArena::forgetResults() itself.
    void invalidateMemosAtRest();
    size_t sizeOfMemosAtRestInBytes() const;

    // In front of Compile, by one thread.
    void freeze();
    bool isFrozen() const { return m_isFrozen; }
    // Null until freeze(), which stands behind Plan. Any thread.
    SharedAnswers* sharedAnswers() const { return m_sharedAnswers.get(); }

    size_t sizeInBytes() const;

private:
    friend class TypeArena;
    friend class ArenasOfWorkers;

    // They speak of the program's records alone, so they hold for every arena that keeps InProgram.
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

    // A class alone, with nothing written of its contents, is ClassPartsID of IDHome::Value. Its part lies here.
    Vector<ClassPart> m_partOfClassAlone;

    const BuiltinTable& m_builtins;
    const Program* m_program;
    std::unique_ptr<ObjectTable> m_objects;
    bool m_isFrozen { false };
    std::unique_ptr<SharedAnswers> m_sharedAnswers;
    InternedRecords<Type> m_types;
    // By position, from freeze() on: the records never move. The walk asks for a type some twenty times an operation.
    Vector<const Type*> m_typesOnceFrozen;
    IDSetTable m_layoutSets { IDSetForm::ChunksWithLazyList, 0 };
    IDSetTable m_objectSets { IDSetForm::ListsAndChunks, wordsOfObjectSetSummary };
    IDSetTable m_stringSets { IDSetForm::ListsAndChunks, 0 };
    IDSetTable m_keySets { IDSetForm::ListsAndChunks, 0 };
    InternedWordLists m_classParts;
    InternedRecords<PropertyKey> m_propertyKeys;
    uint32_t m_numberOfKeysWithStableNumbers { UINT32_MAX };
    Vector<uint8_t> m_locationBitsOfKeys;
    Vector<TypeID> m_typeOfWireIndex;
    // By StringID.
    Vector<TypeID> m_typesOfLiteralStrings;
};

ALWAYS_INLINE const Type& TypeTable::typeKeptAt(TypeID id) const
{
    RELEASE_ASSERT(liesInProgram(id));
    uint32_t position = positionOf(id);
    if (position < m_typesOnceFrozen.size()) [[likely]]
        return *m_typesOnceFrozen[position];
    return m_types.at(position);
}

ALWAYS_INLINE Type TypeTable::at(TypeID id) const
{
    if (isItsOwnValue(id)) {
        Type type;
        type.primitives = primitivesInID(id);
        type.integers = integersInID(id);
        return type;
    }
    return typeKeptAt(id);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
