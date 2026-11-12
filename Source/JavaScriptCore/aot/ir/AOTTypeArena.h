/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeTable.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/MallocSpan.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

enum class DeclaredParts : uint8_t { FromEither, FromSecond };

enum class AnswerKeptInArena : uint8_t {
    FirstOfWalk,
    LastOfWalk = FirstOfWalk + 54,
    ObjectSetOfList,
    NumberOfLayoutList,
    LayoutSetOfList,
    NumberOfObjectList,
    TypeInFilledSlot,
    TestOfGuess,
    LayoutsOfGuessTest,
    ShapesOfLayouts,
    PartWhoseClassesHaveGuards,
    PartWithInheritedMembers,
    PartOfWrittenSetThatFactoriesFill,
    PartThatHasNameAsked,
    ConstantMemberOfBuiltin,
    RecordsThatFileWrites,
    WrittenRecordCoverage,
    UnitMayMentionFunctions,
    LiteralsWithoutNames,
    ReadFindsFieldWithoutWrittenType,
    LayoutsThatStayWithCompiledCode,
    FieldOfLayoutsIsStable,
    DeclaredFieldOfLayoutsIsStable,
    ResultOfBuiltinCall,
    FitInBuiltinType,
    NumberOfAnswers,
};

enum class MakerOfType : uint8_t {
    ForObject = 1,
    ForLayoutSet,
    BuiltinClass,
    StringLiteral,
    Integers,
    WithSeeds,
    TruthyPart,
    FalsyPart,
    WithoutLayoutsThatTestMayRefuse,
    WithPrimitives,
    BuiltinClassWithOneContent,
    NarrowedByTestThatPassed,
    NarrowedByTestThatFailed,
    FirstNarrowedByComparison,
};

inline constexpr uint32_t bitsOfOperationInTypeMemo = 8;
inline constexpr uint8_t firstQuestionFromOutsideInTypeMemo = 128;
static_assert(static_cast<unsigned>(AnswerKeptInArena::NumberOfAnswers) <= firstQuestionFromOutsideInTypeMemo);

constexpr bool wordOfTypeMemoLiesInThread(uint32_t bits)
{
    return liesInThread(TypeID::fromBits(bits));
}

constexpr uint32_t stampInTypeMemo(uint32_t operation, uint32_t epoch)
{
    return epoch << bitsOfOperationInTypeMemo | operation;
}

constexpr size_t slotInTypeMemo(uint32_t operation, uint32_t first, uint32_t second, size_t numberOfSlots)
{
    return slotOfMemoHash(hashOfMemoQuestion(operation, first, second), numberOfSlots);
}

class TypeArena final : private IDSetAnnotator {
    WTF_MAKE_NONCOPYABLE(TypeArena);
public:
    TypeArena(TypeTable&, ArenaKeeps);
    ~TypeArena();

    void reset();
    size_t sizeOfMemosInBytes() const;

    const TypeTable& table() const { return m_table; }
    const BuiltinTable& builtins() const { return m_table.builtins(); }
    const ObjectTable& objects() const { return m_table.objects(); }
    const Program* program() const { return m_table.program(); }
    ArenaKeeps keeps() const { return m_keeps; }
    struct TypeTakenAtEntry {
        uint32_t function { UINT32_MAX };
        uint32_t entryParameter { 0 };
        uint32_t known { 0 };
        uint32_t epoch { 0 };
        TypeID taken;
    };
    TypeTakenAtEntry& typeTakenLastAtEntry(uint32_t function, uint32_t entryParameter) LIFETIME_BOUND { return m_typesTakenLastAtEntries[(function * 8 + entryParameter) % m_typesTakenLastAtEntries.size()]; }
    uint32_t epoch() const { return m_epoch; }
    size_t numberMadeAndKeptToItself() const
    {
        return m_types.size() + m_firstWordOfClassParts.size() + m_propertyKeys.size() + m_madeTexts.size() + m_layoutSets.untyped().numberMadeAndKeptToItself() + m_objectSets.untyped().numberMadeAndKeptToItself()
            + m_stringSets.untyped().numberMadeAndKeptToItself() + m_keySets.untyped().numberMadeAndKeptToItself();
    }

    Type at(TypeID) const;
    bool isUninformative(TypeID) const;
    const Type& recordOf(TypeID) const LIFETIME_BOUND;
    SetView<LayoutID, LayoutSetID> at(LayoutSetID set) const { return m_layoutSets.at(set); }
    std::span<const LayoutID> listOf(LayoutSetID set) const { return const_cast<TypeArena&>(*this).m_layoutSets.listOf(set); }
    SetView<ObjectID, ObjectSetID> at(ObjectSetID set) const { return m_objectSets.at(set); }
    SetView<StringID, StringSetID> at(StringSetID set) const { return m_stringSets.at(set); }
    SetView<PropertyKeyID, KeySetID> at(KeySetID set) const { return m_keySets.at(set); }
    std::span<const ClassPart> at(ClassPartsID) const;
    const PropertyKey& at(PropertyKeyID) const;
    const ObjectRecord& at(ObjectID object) const { return objects().at(object); }
    ObjectSetSummary summaryOf(ObjectSetID) const;

    LayoutSets& layoutSets() { return m_layoutSets; }
    ObjectSets& objectSets() { return m_objectSets; }
    StringSets& stringSets() { return m_stringSets; }
    KeySets& keySets() { return m_keySets; }

    ClassPartsID internClassParts(std::span<const ClassPart>);
    static constexpr ClassPartsID partsOf(BuiltinClassID builtinClass) { return idAt<ClassPartsID>(IDHome::Value, builtinClass.index()); }

    TypeID intern(const TypeParts&);
    TypeID internPartsThatAreCanonical(const TypeParts&);

    static constexpr TypeID fromPrimitives(uint16_t primitives) { return typeOfPrimitives(primitives); }
    TypeID integer(Range);
    TypeID numbersWithin(Range);
    TypeID number(double);
    TypeID stringLiteral(StringID);
    TypeID symbolLiteral(PropertyKeyID);
    TypeID forObject(ObjectID);
    TypeID forObjects(ObjectSetID);
    TypeID forFunction(FunctionID function) { return forObject(objects().closureOf(function)); }
    TypeID forBuiltin(BuiltinID builtin) { return forObject(objects().objectOf(builtin)); }
    TypeID forLayout(LayoutID);
    TypeID forLayouts(std::span<const LayoutID> unsorted);
    TypeID forLayoutSet(LayoutSetID);
    TypeID builtinClass(BuiltinClassID, std::span<const TypeID> writtenContents = { });
    TypeID forEnvironment(LayoutID);
    TypeID forOpaque(OpaqueKind);
    TypeID anything(UnknownSeed);
    TypeID object(UnknownSeed);
    TypeID function(UnknownSeed);

    TypeID unionOf(TypeID, TypeID);
    TypeID unionOf(std::span<const TypeID> types) { return unionOf(types, { }); }
    TypeID unionOf(std::span<const TypeID> types, std::span<const TypeID> earlierStates);
    void forgetEarlierStates();
    TypeID intersectionOf(TypeID, TypeID, DeclaredParts = DeclaredParts::FromEither);
    TypeID castTo(TypeID value, TypeID castType) { return intersectionOf(value, castType, DeclaredParts::FromSecond); }
    TypeID differenceOf(TypeID value, TypeID removed);
    bool isSubset(TypeID, TypeID of);
    bool overlap(TypeID, TypeID);
    bool mayBeSameObject(TypeID, TypeID);
    ObjectSetID membersWithAliasIn(ObjectSetID, ObjectSetID namedByOther);

    TypeID withPrimitives(TypeID, uint16_t added);
    TypeID withoutPrimitives(TypeID, uint16_t removed);
    TypeID wholeKindsAndBuiltinClassesOf(TypeID);
    TypeID numbersAskedBehindCallOfBuiltin(TypeID);
    TypeID withoutNullish(TypeID type) { return withoutPrimitives(type, Primitive::UndefinedOrNull); }
    TypeID withoutObjects(TypeID);
    TypeID withOnlyObjects(TypeID);
    TypeID withIntegers(TypeID, Range);
    TypeID withoutBoundsOfFractions(TypeID);
    TypeID withLiteralStringsWidened(TypeID);
    TypeID withSeeds(TypeID, UnknownSeeds added);

    TypeID describedByShape(TypeID);
    TypeID testablePartOf(TypeID);


    std::optional<uint32_t> rememberedAnswer(uint8_t question, uint32_t first, uint32_t second);
    void rememberAnswer(uint8_t question, uint32_t first, uint32_t second, uint32_t answer);
    uint32_t numberOfOperandList(uint32_t list, TypeID lastOperand);
    static constexpr size_t mostWordsOfRound = 4;
    using WordsOfRound = std::array<uint32_t, mostWordsOfRound>;
    struct SetOfRound {
        uint32_t word { 0 };
        bool isKeptForBuild { false };
    };
    SetOfRound setOfRound(LayoutSetID set) { return !set || holdsOneMember(set) ? SetOfRound { set.bits(), false } : setOfRoundOverSeveral(set); }
    SetOfRound setOfRound(ObjectSetID set) { return !set || holdsOneMember(set) ? SetOfRound { set.bits(), false } : setOfRoundOverSeveral(set); }
    std::optional<WordsOfRound> rememberedRound(uint8_t question, size_t numberOfWords, SetOfRound, uint32_t second);
    void rememberRound(uint8_t question, size_t numberOfWords, uint8_t typesAmongWords, SetOfRound, uint32_t second, const WordsOfRound&);
    static constexpr size_t fewestElementsOfArrayWhoseContentsAreKeptForBuild = 8;
    std::optional<TypeID> rememberedContentsOfArrayInData(DataID);
    void rememberContentsOfArrayInData(DataID, TypeID);

    TypeID typeMadeBefore(MakerOfType, uint32_t first, uint32_t second) const;
    void noteTypeMade(MakerOfType, uint32_t first, uint32_t second, TypeID made, bool internedNothingInThread);

    std::strong_ordering compareByContent(TypeID, TypeID) const;

    PropertyKeyID intern(const PropertyKey&);
    PropertyKeyID textKey(StringID);
    PropertyKeyID wellKnownSymbolKey(WellKnownSymbol);
    PropertyKeyID registeredSymbolKey(StringID description);

    StringID internText(std::span<const uint8_t> programString);
    StringID internJoinedTexts(StringID left, StringID right);
    std::span<const uint8_t> bytesOf(StringID) const;
    size_t numberOfBytesOf(StringID) const;
    bool isWideText(StringID) const;
    StringView viewOf(StringID, Vector<char16_t>& bufferForNonASCII) const;
    bool spellSameText(StringID a, StringID b) const { return a == b || (liesInThread(a) != liesInThread(b) && numberOfBytesOf(a) == numberOfBytesOf(b) && std::ranges::equal(bytesOf(a), bytesOf(b))); }
    bool hasMadeTexts() const { return !m_madeTexts.isEmpty(); }

private:
    friend class ArenasOfWorkers;

    enum class Operation : uint8_t { Union, Intersection, Cast, Difference, Subset, Overlap, SameObject, DescribedByShape, TestablePart, MembersWithAlias, WithoutPrimitives };

    struct MemoEntry {
        uint32_t first { UINT32_MAX };
        uint32_t second { UINT32_MAX };
        uint32_t result { UINT32_MAX };
        uint32_t operationAndEpoch { 0 };
    };

    void annotate(const IDSetView&, std::optional<IDSetOperation>, std::span<const uint32_t> operands, std::span<uint32_t> attachedWords) final;

    static const MemoEntry& entryInMemo(const Vector<MemoEntry>&, uint32_t operation, uint32_t first, uint32_t second);
    std::optional<uint32_t> remembered(Operation, uint32_t first, uint32_t second);
    std::optional<uint32_t> rememberedUntilReset(uint32_t operationWord, uint32_t first, uint32_t second) const;
    std::optional<uint32_t> rememberedAtOnce(Operation, uint32_t first, uint32_t second);
    std::optional<uint32_t> rememberedElsewhere(Operation, uint32_t first, uint32_t second);
    void remember(Operation, uint32_t first, uint32_t second, uint32_t result);
    void rememberInFirstMemoUntilReset(Operation, uint32_t first, uint32_t second, uint32_t result);
    bool makeRoomForTypesMade();
    void forgetUnionsOfMany();
    Type complete(const TypeParts& canonical) const;
    void completeInPlace(Type&) const;
    void addWhatNamedObjectsBring(Type&) const;
    TypeID internCanonical(Type&);

    TypeID unionOfDifferentTypes(TypeID, TypeID);
    TypeID intersectionOfDifferentTypes(TypeID, TypeID, DeclaredParts);
    bool isSubsetOfDifferentType(TypeID, TypeID of);
    bool overlapOfDifferentTypes(TypeID, TypeID);
    TypeID differenceOfTypesWithValues(TypeID value, TypeID removed);
    TypeID withoutPrimitivesHeld(TypeID, uint16_t removed);
    TypeID takePrimitivesAwayAndRemember(TypeID, uint16_t removed);
    TypeID withoutObjectsOfRecord(TypeID);
    TypeID withoutBoundsOfFractionsOfRecord(TypeID);
    TypeID withSeedsOnRecord(TypeID, UnknownSeeds added);
    TypeID makeAnything(UnknownSeed);
    SetOfRound setOfRoundOverSeveral(LayoutSetID);
    SetOfRound setOfRoundOverSeveral(ObjectSetID);
    std::span<const ClassPart> classPartsInThreadAt(ClassPartsID) const;
    TypeID uniteAndRemember(TypeID, TypeID);
    TypeID intersectAndRemember(TypeID, TypeID, DeclaredParts);
    TypeID subtractAndRemember(TypeID value, TypeID removed);
    bool workOutSubsetAndRemember(TypeID, TypeID of);
    bool workOutOverlapAndRemember(TypeID, TypeID);

    TypeTable& m_table;
    const ArenaKeeps m_keeps;
    std::array<TypeTakenAtEntry, 64> m_typesTakenLastAtEntries;
    bool m_keepsTypesMade { false };
    bool m_someAnythingOfSeedLiesInThread { false };
    uint32_t m_epoch { 1 };
    const std::span<const Type* const> m_typesOfFrozenTable;
    LayoutSets m_layoutSets;
    ObjectSets m_objectSets;
    StringSets m_stringSets;
    KeySets m_keySets;
    Vector<Type> m_types;
    Vector<uint32_t> m_typeIndex;
    Vector<uint32_t> m_classPartWords;
    Vector<uint32_t> m_firstWordOfClassParts;
    Vector<uint32_t> m_classPartsIndex;
    Vector<PropertyKey> m_propertyKeys;
    Vector<uint32_t> m_propertyKeyIndex;
    Vector<Vector<uint32_t>> m_outgrownClassPartWords;
    Vector<Vector<PropertyKey>> m_outgrownPropertyKeys;
    struct FactsOfText {
        uint32_t numberOfBytes { 0 };
        bool isWide { false };
        bool beginsWithTrailSurrogate { false };
        bool endsWithLeadSurrogate { false };
        uint64_t hash { 0 };
        uint64_t weightInFront { 1 };
    };
    struct MadeText : FactsOfText {
        static constexpr uint32_t notLaidDown = UINT32_MAX;
        StringID left;
        StringID right;
        uint32_t firstByte { notLaidDown };
    };
    static FactsOfText factsOfText(std::span<const uint8_t>);
    FactsOfText factsOf(StringID) const;
    std::span<const uint8_t> bytesOfMadeText(uint32_t position) const;
    StringID addMadeText(const MadeText&);

    mutable Vector<uint8_t> m_bytesOfTexts;
    mutable HashMap<uint32_t, uint32_t> m_roomBehindTexts;
    mutable Vector<MadeText> m_madeTexts;
    Vector<uint32_t> m_textIndex;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_joinedTexts;
    mutable Vector<Vector<uint8_t>> m_outgrownBytesOfTexts;
    std::array<TypeID, 16> m_anythingOfSeed { };
    IDPairMemo m_lastingMemo;
    using NumbersOfOperandLists = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    uint32_t m_numberOfOperandLists { 0 };
    NumbersOfOperandLists m_numbersOfOperandLists;
    NumbersOfOperandLists m_numbersOfOperandListsUntilReset;
    IDListMemo m_unionsOfMany;
    Vector<MemoEntry> m_memoUntilReset;
    Vector<MemoEntry> m_typesMade;
};

class ArenasOfWorkers {
    WTF_MAKE_NONCOPYABLE(ArenasOfWorkers);
public:
    ArenasOfWorkers(TypeTable&, const ParallelLoops&);
    ~ArenasOfWorkers();

    TypeArena& of(unsigned worker);

    void internInOrder(std::span<TypeID> ids, NOESCAPE const ScopedLambda<TypeParts(uint32_t index, TypeArena&)>& partsOf);
    void internInOrder(std::span<PropertyKeyID> ids, NOESCAPE const ScopedLambda<PropertyKey(uint32_t index)>& keyOf);

private:
    TypeTable& m_table;
    const ParallelLoops& m_loops;
    Vector<std::unique_ptr<TypeArena>> m_arenas;
    MallocSpan<Type> m_candidates;
};

TypeParts uniteTypes(TypeArena&, const Type&, const Type&);
TypeParts intersectTypes(TypeArena&, const Type&, const Type&, DeclaredParts);
TypeParts subtractTypes(TypeArena&, const Type& value, const Type& removed);

inline TypeReader::TypeReader(const TypeArena& arena)
    : m_table(&arena.table())
    , m_arena(&arena)
{
}

ALWAYS_INLINE const Type& TypeArena::recordOf(TypeID id) const
{
    uint32_t positionInProgram = TypeTable::positionInProgramOf(id);
    if (positionInProgram < m_typesOfFrozenTable.size()) [[likely]]
        return *m_typesOfFrozenTable[positionInProgram];
    uint32_t positionInThread = id.bits() - (static_cast<uint32_t>(IDHome::Thread) << idHomeShift);
    if (positionInThread < m_types.size())
        return m_types[positionInThread];
    return m_table.typeKeptAt(id);
}

ALWAYS_INLINE Type TypeArena::at(TypeID id) const
{
    if (isItsOwnValue(id))
        return TypeTable::typeInID(id);
    return recordOf(id);
}

ALWAYS_INLINE bool TypeArena::isUninformative(TypeID id) const
{
    if (!id || isNeverType(id))
        return true;
    if (holdsOnlyPrimitives(id))
        return (primitivesInID(id) & Primitive::All) == Primitive::All;
    const Type& type = recordOf(id);
    return type.hasUnboundedUnknown() || (!type.hasObjects() && (type.primitives & Primitive::All) == Primitive::All);
}

inline std::span<const ClassPart> TypeArena::at(ClassPartsID id) const
{
    if (liesInThread(id))
        return classPartsInThreadAt(id);
    return m_table.at(id);
}

ALWAYS_INLINE const PropertyKey& TypeArena::at(PropertyKeyID key) const
{
    if (key.bits() < m_table.m_numberOfPropertyKeysFoundByAddress) [[likely]]
        return m_table.propertyKeyFoundByAddressAt(key.bits());
    if (liesInThread(key))
        return m_propertyKeys[positionOf(key)];
    return m_table.at(key);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE const TypeArena::MemoEntry& TypeArena::entryInMemo(const Vector<MemoEntry>& memo, uint32_t operation, uint32_t first, uint32_t second)
{
    return memo.span().data()[slotInTypeMemo(operation, first, second, memo.size())];
}

ALWAYS_INLINE void TypeArena::noteTypeMade(MakerOfType maker, uint32_t first, uint32_t second, TypeID made, bool internedNothingInThread)
{
    if (m_typesMade.isEmpty() && !makeRoomForTypesMade())
        return;
    uint32_t makerWord = static_cast<uint32_t>(maker);
    m_typesMade.mutableSpan().data()[slotInTypeMemo(makerWord, first, second, m_typesMade.size())] = { first, second, made.bits(), stampInTypeMemo(makerWord, internedNothingInThread ? 0 : m_epoch) };
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

ALWAYS_INLINE std::optional<uint32_t> TypeArena::rememberedUntilReset(uint32_t operationWord, uint32_t first, uint32_t second) const
{
    if (m_memoUntilReset.isEmpty())
        return std::nullopt;
    const MemoEntry& entry = entryInMemo(m_memoUntilReset, operationWord, first, second);
    if (entry.first != first || entry.second != second || entry.operationAndEpoch != stampInTypeMemo(operationWord, m_epoch))
        return std::nullopt;
    return entry.result;
}

ALWAYS_INLINE std::optional<uint32_t> TypeArena::remembered(Operation operation, uint32_t first, uint32_t second)
{
    uint32_t operationWord = static_cast<uint32_t>(operation);
    if (!wordOfTypeMemoLiesInThread(first) && !wordOfTypeMemoLiesInThread(second)) {
        if (std::optional<uint32_t> result = m_lastingMemo.find(operationWord, first, second))
            return result;
    }
    return rememberedUntilReset(operationWord, first, second);
}

ALWAYS_INLINE std::optional<uint32_t> TypeArena::rememberedAtOnce(Operation operation, uint32_t first, uint32_t second)
{
    uint32_t operationWord = static_cast<uint32_t>(operation);
    if (!wordOfTypeMemoLiesInThread(first) && !wordOfTypeMemoLiesInThread(second))
        return m_lastingMemo.findInRecentGeneration(operationWord, first, second);
    return rememberedUntilReset(operationWord, first, second);
}

ALWAYS_INLINE std::optional<uint32_t> TypeArena::rememberedElsewhere(Operation operation, uint32_t first, uint32_t second)
{
    uint32_t operationWord = static_cast<uint32_t>(operation);
    if (wordOfTypeMemoLiesInThread(first) || wordOfTypeMemoLiesInThread(second))
        return std::nullopt;
    if (std::optional<uint32_t> result = m_lastingMemo.findInOlderGeneration(operationWord, first, second))
        return result;
    return rememberedUntilReset(operationWord, first, second);
}

ALWAYS_INLINE std::optional<uint32_t> TypeArena::rememberedAnswer(uint8_t question, uint32_t first, uint32_t second)
{
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset && question < firstQuestionFromOutsideInTypeMemo);
    return remembered(static_cast<Operation>(firstQuestionFromOutsideInTypeMemo + question), first, second);
}

ALWAYS_INLINE TypeID TypeArena::typeMadeBefore(MakerOfType maker, uint32_t first, uint32_t second) const
{
    if (m_typesMade.isEmpty())
        return { };
    uint32_t makerWord = static_cast<uint32_t>(maker);
    const MemoEntry& entry = entryInMemo(m_typesMade, makerWord, first, second);
    if (entry.first != first || entry.second != second)
        return { };
    if (entry.operationAndEpoch != stampInTypeMemo(makerWord, m_epoch) && entry.operationAndEpoch != stampInTypeMemo(makerWord, 0))
        return { };
    return TypeID::fromBits(entry.result);
}

inline TypeID TypeArena::anything(UnknownSeed seed)
{
    TypeID known = m_anythingOfSeed[std::countr_zero(static_cast<uint16_t>(seed))];
    return known ? known : makeAnything(seed);
}

inline TypeID TypeArena::unionOf(TypeID a, TypeID b)
{
    ASSERT(a && b);
    if (a == b || isNeverType(b))
        return a;
    if (isNeverType(a))
        return b;
    return unionOfDifferentTypes(a, b);
}

inline TypeID TypeArena::intersectionOf(TypeID a, TypeID b, DeclaredParts declared)
{
    ASSERT(a && b);
    if (a == b)
        return a;
    if (isNeverType(a) || isNeverType(b))
        return TypeTable::never;
    return intersectionOfDifferentTypes(a, b, declared);
}

inline TypeID TypeArena::differenceOf(TypeID value, TypeID removed)
{
    ASSERT(value && removed);
    if (isNeverType(value) || isNeverType(removed))
        return value;
    return differenceOfTypesWithValues(value, removed);
}

inline bool TypeArena::isSubset(TypeID type, TypeID of)
{
    ASSERT(type && of);
    if (type == of || isNeverType(type))
        return true;
    if (isNeverType(of))
        return false;
    return isSubsetOfDifferentType(type, of);
}

inline bool TypeArena::overlap(TypeID a, TypeID b)
{
    ASSERT(a && b);
    if (isNeverType(a) || isNeverType(b))
        return false;
    if (a == b)
        return true;
    return overlapOfDifferentTypes(a, b);
}

inline TypeID TypeArena::withoutPrimitives(TypeID type, uint16_t removed)
{
    uint16_t held = holdsOnlyPrimitives(type) ? primitivesInID(type) : recordOf(type).primitives;
    if (!(held & removed))
        return type;
    return withoutPrimitivesHeld(type, removed);
}

inline TypeID TypeArena::withoutObjects(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return type;
    return withoutObjectsOfRecord(type);
}

inline TypeID TypeArena::withoutBoundsOfFractions(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return type;
    return withoutBoundsOfFractionsOfRecord(type);
}

inline TypeID TypeArena::withSeeds(TypeID type, UnknownSeeds added)
{
    if (holdsOnlyPrimitives(type))
        return type;
    return withSeedsOnRecord(type, added);
}

ALWAYS_INLINE __attribute__((used)) const Type& TypeReader::recordOf(TypeID id) const { return m_arena ? m_arena->recordOf(id) : m_table->typeKeptAt(id); }
ALWAYS_INLINE __attribute__((used)) Type TypeReader::at(TypeID id) const
{
    if (isItsOwnValue(id))
        return TypeTable::typeInID(id);
    return recordOf(id);
}
inline SetView<LayoutID, LayoutSetID> TypeReader::at(LayoutSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
ALWAYS_INLINE __attribute__((used)) std::span<const LayoutID> TypeReader::listOf(LayoutSetID id) const { return m_arena ? m_arena->listOf(id) : m_table->listOf(id); }
inline SetView<ObjectID, ObjectSetID> TypeReader::at(ObjectSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline SetView<StringID, StringSetID> TypeReader::at(StringSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline SetView<PropertyKeyID, KeySetID> TypeReader::at(KeySetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
ALWAYS_INLINE __attribute__((used)) std::span<const ClassPart> TypeReader::at(ClassPartsID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline const PropertyKey& TypeReader::at(PropertyKeyID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline const ObjectRecord& TypeReader::at(ObjectID id) const { return m_table->objects().at(id); }
inline const ObjectTable& TypeReader::objects() const { return m_table->objects(); }
inline const BuiltinTable& TypeReader::builtins() const { return m_table->builtins(); }
inline const Program* TypeReader::program() const { return m_table->program(); }

ALWAYS_INLINE __attribute__((used)) std::span<const LayoutID> membersOf(TypeReader reader, LayoutSetID set)
{
    return reader.listOf(set);
}

ALWAYS_INLINE __attribute__((used)) std::span<const LayoutID> membersOf(TypeReader reader, const Type& type)
{
    return reader.listOf(type.layouts);
}

ALWAYS_INLINE __attribute__((used)) std::span<const ClassPart> classPartsOf(TypeReader reader, const Type& type)
{
    if (!type.classes)
        return { };
    return reader.at(type.classes);
}

ALWAYS_INLINE __attribute__((used)) const ClassPart* singleBuiltinClassOf(TypeReader reader, const Type& type)
{
    if (!type.hasOnlyClassObjects())
        return nullptr;
    std::span<const ClassPart> parts = classPartsOf(reader, type);
    return parts.size() == 1 ? parts.data() : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
