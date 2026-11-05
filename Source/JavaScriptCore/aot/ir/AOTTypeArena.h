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

// Which side's written contents of a class an intersection keeps.
enum class DeclaredParts : uint8_t { FromEither, FromSecond };

// The numbers of TypeArena::rememberAnswer(): the first of each asker, which counts on from it up to the next. Whoever takes a number writes it here.
enum class AnswerKeptInArena : uint8_t {
    // walk/AOTWalk.h: AnswerKeptByWalk.
    FirstOfWalk = 0,
    // walk/AOTAccessPlanOfFields.cpp, four numbers: what Facts says of the cells of a receiver's objects under a name.
    CellsOfFieldArm = 57,
    // ir/AOTTypeIntersection.cpp: the set of objects whose members SharedAnswers has under a number.
    ObjectSetOfList = 61,
    // walk/AOTMemberPartition.cpp, the last two of the walk's: the number that SharedAnswers has for the members of a set of layouts, and the set of such a number.
    NumberOfLayoutList = 62,
    LayoutSetOfList = 63,
    // facts/AOTFlowRulesReads.cpp.
    FirstOfAppliedReads = 64,
    // facts/AOTFlowRulesWrites.cpp.
    FirstOfAppliedWrites = 72,
    // facts/AOTFactsOfCells.cpp.
    TypeInCells = 78,
    TypeFound = 79,
    // facts/AOTFlowRulesWrites.cpp, of a set of layouts and of the layouts of a set of objects.
    ReadOrAssignmentRunsNoCodeOnLayouts = 80,
    LayoutsAreFoundByShapeTest = 81,
    ReadRunsNoCodeOnLayoutsOfObjects = 82,
    // ir/AOTTypeArena.cpp: the number that SharedAnswers has for the members of a set of objects. The copy of facts/ that lane 2 builds counts up to 83: AOTSummaryApply.cpp.
    NumberOfObjectList = 84,
    // walk/AOTWalkSlotReads.cpp, sixteen numbers: which of the two words of a round, which half of an accessor is read, and what cannot happen to the receiver.
    FieldsOfSlotRead = 85,
    // walk/AOTWalkTypedAccesses.cpp: what a filled slot of the objects of a type holds under a name.
    TypeInFilledSlot = 101,
    // walk/AOTWalkGuesses.cpp, two numbers: the type and the layouts that the test for an expected type asks for.
    TestOfGuess = 102,
};

// One thread's door to the types: it makes them, computes with them and remembers results by pair of ids. It is never shared between threads.
//
// ArenaKeeps::InProgram    the reader, Facts, Plan. Everything made lasts for the build.
// ArenaKeeps::UntilReset   Compile. The walk calls reset() in front of each function it compiles.
//                          Every id with liesInThread() dies there. Nothing that outlives the function may hold one: CompiledCode names layouts, keys and classes by content.
//
// Every operation answers at once where one side is never and where both sides are primitives alone, and every one but differenceOf() where the ids are equal:
// a test may refuse a value that it names, so equal records go to subtractTypes().
// Otherwise it asks the memo, then computes. An entry whose three ids all lie outside the thread outlives reset(): IDPairMemo.
// An arena that keeps InProgram takes that memo, and those of its sets, from an arena that has ended, and leaves them to the next: TypeTable::beginKeepingMemosAtRest().
class TypeArena final : private IDSetAnnotator {
    WTF_MAKE_NONCOPYABLE(TypeArena);
public:
    TypeArena(TypeTable&, ArenaKeeps);
    ~TypeArena();

    void reset();
    // With the scratch of its sets, which lasts as long as they do.
    size_t sizeOfMemosInBytes() const;

    const TypeTable& table() const { return m_table; }
    const BuiltinTable& builtins() const { return m_table.builtins(); }
    const ObjectTable& objects() const { return m_table.objects(); }
    const Program* program() const { return m_table.program(); }
    ArenaKeeps keeps() const { return m_keeps; }
    // Whatever lies in the thread: types, parts of classes, keys, texts, sets. It grows with each thing that is made and that the program's table lacks.
    size_t numberMadeAndKeptToItself() const
    {
        return m_types.size() + m_firstWordOfClassParts.size() + m_propertyKeys.size() + m_madeTexts.size() + m_layoutSets.untyped().numberMadeAndKeptToItself() + m_objectSets.untyped().numberMadeAndKeptToItself()
            + m_stringSets.untyped().numberMadeAndKeptToItself() + m_keySets.untyped().numberMadeAndKeptToItself();
    }

    Type at(TypeID) const;
    // NOTHING IS SAID OF A VALUE OF THE TYPE: no type . every primitive and no object: what is left of `anything` where no object is seen . objects that nobody describes, beside whatever else.
    // No test tells it, and no typed form, fold or row of a built-in leans on it. It copies no Type.
    bool saysNothing(TypeID) const;
    // Of a type that does not holdOnlyPrimitives(), for whoever looks at a member or two. It stands until the arena makes its next type.
    const Type& recordOf(TypeID id) const LIFETIME_BOUND { return liesInThread(id) ? m_types[positionOf(id)] : m_table.typeKeptAt(id); }
    SetView<LayoutID, LayoutSetID> at(LayoutSetID set) const { return m_layoutSets.at(set); }
    // Ascending. It lasts as long as the set. Making the list of a wide set changes no answer of the arena.
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

    // At most one part a class, in ascending order of class. Trailing contents that are `anything` are dropped.
    ClassPartsID internClassParts(std::span<const ClassPart>);
    static constexpr ClassPartsID partsOf(BuiltinClassID builtinClass) { return idAt<ClassPartsID>(IDHome::Value, builtinClass.index()); }

    // Makes the parts canonical first:
    //   bits outside Primitive::Defined go; the range is cut to Range::full() and is empty exactly where Integer is absent
    //   bounds of fractions that leave room for none take FractionalNumber away; bounds that every fraction lies in are emptied, and so are those of a type without fractions
    //   String swallows the string literals and Symbol the symbol literals; a literal bit stands exactly where its set has members
    //   anyObject sets anyFunction and swallows unnamedLayouts and unnamedClasses. It never swallows a named object
    //   seeds stay only while hasUnknown()
    //   an opaque value or an environment mixed with anything but Absent and Unspecified becomes anything
    TypeID intern(const TypeParts&);

    static constexpr TypeID fromPrimitives(uint16_t primitives) { return typeOfPrimitives(primitives); }
    TypeID integer(Range);
    // The integers of the range and the numbers with a fraction between its ends. Neither NaN nor an infinity nor -0.
    TypeID numbersWithin(Range);
    TypeID number(double);
    // A text that liesInThread() can be no member of a set: the answer is TypeTable::string.
    TypeID stringLiteral(StringID);
    // A key that liesInThread() can be no member of a set: the answer is TypeTable::symbol.
    TypeID symbolLiteral(PropertyKeyID);
    TypeID forObject(ObjectID);
    TypeID forObjects(ObjectSetID);
    TypeID forFunction(FunctionID function) { return forObject(objects().closureOf(function)); }
    TypeID forBuiltin(BuiltinID builtin) { return forObject(objects().objectOf(builtin)); }
    // Some object of the layout, nobody knows which: `unknown` within it.
    TypeID forLayout(LayoutID);
    TypeID forLayouts(std::span<const LayoutID> unsorted);
    TypeID forLayoutSet(LayoutSetID);
    TypeID builtinClass(BuiltinClassID, std::span<const TypeID> writtenContents = { });
    TypeID forEnvironment(LayoutID);
    TypeID forOpaque(OpaqueKind);
    TypeID anything(UnknownSeed);
    TypeID object(UnknownSeed);
    TypeID function(UnknownSeed);

    // Every part is united. It grows with either side, so the solve may lean on it: unionOf(a, b) == a says that b brought nothing.
    TypeID unionOf(TypeID, TypeID);
    // The same answer as uniting them one by one, in any order. The result alone is interned, and so is each of its sets.
    TypeID unionOf(std::span<const TypeID> types) { return unionOf(types, { }); }
    // The same for values that have grown, which is how Facts takes what comes over an edge: the value as it stands, beside what its target was sent of it before.
    // earlierStates is empty or has one entry a type: what that value held once, all of which it still holds and others of the types hold too. A null id says nothing. IDSetArena::unionOf() says what it saves.
    TypeID unionOf(std::span<const TypeID> types, std::span<const TypeID> earlierStates);
    // IDSetArena::forgetEarlierStates() of each kind of set.
    void forgetEarlierStates();
    // What may be both. This is also what a filter lets through, a passed test proves and a guarded position holds:
    //   a named object stays, named, if the other side names it or its shape may fit the other side's `unknown`
    //   a merged form needs more where the answer holds all of the other side's `unknown`: the other side names it, or its shape surely fits. That `unknown` covers the rest of what it stands for
    //   `unknown` against a named closure or built-in gives that object; against layouts and classes it becomes `unknown` within them
    //   the seeds of both sides stay
    TypeID intersectionOf(TypeID, TypeID, DeclaredParts = DeclaredParts::FromEither);
    TypeID castTo(TypeID value, TypeID castType) { return intersectionOf(value, castType, DeclaredParts::FromSecond); }
    // What is left of `value` where a test for `removed` has failed: only what would surely have passed goes.
    TypeID differenceOf(TypeID value, TypeID removed);
    // Every value of the first is one of the second. Seeds are not looked at. A named object lies inside `unknown` if its shape surely does.
    bool isSubset(TypeID, TypeID of);
    bool overlap(TypeID, TypeID);
    bool mayBeSameObject(TypeID, TypeID);
    // The members of the first set that the second does not name and may hold all the same, under another number: ObjectSetAliasIndex::namesAliasOf(). Where either set is wide it is worked out once for the pair of sets, whichever types hold them.
    ObjectSetID membersWithAliasIn(ObjectSetID, ObjectSetID namedByOther);

    TypeID withPrimitives(TypeID, uint16_t added);
    TypeID withoutPrimitives(TypeID, uint16_t removed);
    TypeID withoutNullish(TypeID type) { return withoutPrimitives(type, Primitive::UndefinedOrNull); }
    TypeID withoutObjects(TypeID);
    TypeID withOnlyObjects(TypeID);
    TypeID withIntegers(TypeID, Range);
    // The code of a test asks whether a number has a fraction and never where it lies.
    TypeID withoutBoundsOfFractions(TypeID);
    TypeID withLiteralStringsWidened(TypeID);
    TypeID withSeeds(TypeID, UnknownSeeds added);

    // The type in the words of tests, which is how an expected type speaks: each named object that no comparison of pointers finds gives way to its layout or class,
    // a closure stays, and one without a shape makes it anyObject or anyFunction. For the guess that merged members beside `unknown` make, hand in withOnlyNamedObjects().
    TypeID describedByShape(TypeID);
    // No test finds a built-in that each module has one of, and none tells the instances of a class without an instance test: both widen to any object or any function.
    // Primitives stay as they are, a range of integers among them: TypeTest of an Int64 against int32 is how an overflow is found. Bounds of fractions go.
    TypeID testablePartOf(TypeID);

    // The walk's loops: a bound that moved since `previous` goes to the next landmark, so a range settles in a few steps.
    TypeID widenedFrom(TypeID now, TypeID previous);

    // What somebody outside the arena has worked out from two words, where nothing that the answer depends on changes while the arena lives: what Compile asks of Facts.
    // It is kept as the arena keeps its own answers: past reset() where no word lies in the thread, and until another answer takes its place. `question` is the asker's own number.
    std::optional<uint32_t> rememberedAnswer(uint8_t question, uint32_t first, uint32_t second);
    void rememberAnswer(uint8_t question, uint32_t first, uint32_t second, uint32_t answer);
    // What a round over the members of a set has found: up to four words, some of them types. It is kept as rememberAnswer() keeps a word, under `question` and the numbers behind it.
    // A set may name everything of its kind in the program, as many functions may ask, and what is found is seldom a type of the program: kept in the arena alone it would die with each function.
    // So the round over a wide set is kept for the build too (SharedAnswers), a type by its parts, and whichever thread asks next makes the types of them.
    static constexpr size_t mostWordsOfRound = 4;
    using WordsOfRound = std::array<uint32_t, mostWordsOfRound>;
    // The word that the question has for the set: its id, or for a wide set that lies in the thread the number of its members, which is the same in every thread.
    struct SetOfRound {
        uint32_t word { 0 };
        bool isKeptForBuild { false };
    };
    SetOfRound setOfRound(LayoutSetID);
    SetOfRound setOfRound(ObjectSetID);
    std::optional<WordsOfRound> rememberedRound(uint8_t question, size_t numberOfWords, SetOfRound, uint32_t second);
    // Bit n of `typesAmongWords`: word n is a type.
    void rememberRound(uint8_t question, size_t numberOfWords, uint8_t typesAmongWords, SetOfRound, uint32_t second, const WordsOfRound&);

    // By content, the same at any number of threads. For whoever must sort types.
    std::strong_ordering compareByContent(TypeID, TypeID) const;

    PropertyKeyID intern(const PropertyKey&);
    PropertyKeyID textKey(StringID);
    PropertyKeyID wellKnownSymbolKey(WellKnownSymbol);
    PropertyKeyID registeredSymbolKey(StringID description);

    // A text that Compile makes. Its id liesInThread() and dies at reset(), and equal texts get one id. Only for ArenaKeeps::UntilReset, and never for the empty text, which the program's table always has.
    // Whether that table holds an equal text is not asked: what a function's code is must not follow what the rest of the program spells. So a made text and a text of the program may be one text under two ids.
    // Whoever compares two texts of which one is made asks spellSameText(), and whoever reads one asks bytesOf() or viewOf(). Nothing that goes by the id of a text takes a made one:
    // it is no member of a set (stringLiteral() answers TypeTable::string), no text of a property key, and has no word among the literal strings.
    // Code mentions it as it mentions a text of the program, and copyMadeTextsMentionedBy() hands its bytes on to Image, which gives it a number.
    StringID internText(std::span<const uint8_t> programString);
    // The text of `left` with the text of `right` behind it, as internText() would make it. Neither is empty. A made text costs the same to join however long it is:
    // the new text is kept as its two pieces, and its bytes are laid down when somebody first asks for them. So `a + b + c + ..` costs what its last text holds, and not that of every text on the way.
    // Where somebody asks for each text on the way, each is laid down behind the one that it adds to while there is room: they begin at one address, and all of them together cost what the last one holds.
    StringID internJoinedTexts(StringID left, StringID right);
    // False: the one ends in the first half of a pair and the other begins with the second, so the joined text spells one character where the two spell two halves. It lays down no bytes of a made text.
    bool joinedTextIsSpelledAsItsPieces(StringID left, StringID right) const;
    // The canonical program string, of a text of either home. It stays where it is until reset().
    std::span<const uint8_t> bytesOf(StringID) const;
    // As of bytesOf(), without laying down the bytes of a made text.
    size_t numberOfBytesOf(StringID) const;
    // isWideProgramString() of bytesOf(), likewise.
    bool isWideText(StringID) const;
    StringView viewOf(StringID, Vector<char16_t>& bufferForNonASCII) const;
    bool spellSameText(StringID a, StringID b) const { return a == b || (liesInThread(a) != liesInThread(b) && numberOfBytesOf(a) == numberOfBytesOf(b) && std::ranges::equal(bytesOf(a), bytesOf(b))); }

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

    std::optional<uint32_t> remembered(Operation, uint32_t first, uint32_t second);
    void remember(Operation, uint32_t first, uint32_t second, uint32_t result);
    void forgetUnionsOfMany();
    Type complete(const TypeParts& canonical) const;
    TypeID internCanonical(const TypeParts&);

    TypeTable& m_table;
    const ArenaKeeps m_keeps;
    uint32_t m_epoch { 1 };
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
    // The buffers that m_classPartWords and m_propertyKeys have outgrown. What at(ClassPartsID) and at(PropertyKeyID) handed out points into them until reset().
    Vector<Vector<uint32_t>> m_outgrownClassPartWords;
    Vector<Vector<PropertyKey>> m_outgrownPropertyKeys;
    // What is known of a text without its bytes, and follows for a joined text from the same of its pieces.
    struct FactsOfText {
        uint32_t numberOfBytes { 0 };
        bool isWide { false };
        // Half a pair at the end of one text and the other half at the start of the next are one character of the joined text.
        bool beginsWithTrailSurrogate { false };
        bool endsWithLeadSurrogate { false };
        // The bytes as the digits of a number, and what a digit in front of them all would count.
        uint64_t hash { 0 };
        uint64_t weightInFront { 1 };
    };
    struct MadeText : FactsOfText {
        static constexpr uint32_t notLaidDown = UINT32_MAX;
        // Null: it was made of bytes.
        StringID left;
        StringID right;
        // In m_bytesOfTexts.
        uint32_t firstByte { notLaidDown };
    };
    static FactsOfText factsOfText(std::span<const uint8_t>);
    FactsOfText factsOf(StringID) const;
    std::span<const uint8_t> bytesOfMadeText(uint32_t position) const;
    StringID addMadeText(const MadeText&);

    // Asking for the bytes of a made text lays them down, which changes no answer of the arena.
    mutable Vector<uint8_t> m_bytesOfTexts;
    // Where a text ends that nothing has been added to: the bytes behind it that are left for whatever adds to it.
    mutable HashMap<uint32_t, uint32_t> m_roomBehindTexts;
    mutable Vector<MadeText> m_madeTexts;
    Vector<uint32_t> m_textIndex;
    // `left` above `right`: the position of what internJoinedTexts() has answered. Two arms of a guess walk one chain of joins.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_joinedTexts;
    // As above, for what bytesOf() handed out.
    mutable Vector<Vector<uint8_t>> m_outgrownBytesOfTexts;
    // By the bit of the seed: the walk asks at every operation of which nothing is known.
    std::array<TypeID, 16> m_anythingOfSeed { };
    IDPairMemo m_lastingMemo;
    IDListMemo m_unionsOfMany;
    Vector<MemoEntry> m_memoUntilReset;
};

// An arena that keeps InProgram for each thread of the loops, and what those threads intern together.
class ArenasOfWorkers {
    WTF_MAKE_NONCOPYABLE(ArenasOfWorkers);
public:
    ArenasOfWorkers(TypeTable&, const ParallelLoops&);
    ~ArenasOfWorkers();

    // It is made when it is first asked for. Only the thread that has the number asks.
    TypeArena& of(unsigned worker);

    // ids[index] becomes what TypeArena::intern(partsOf(index, arena)) answers to one thread that asks for 0, 1, 2 and so on.
    // So the positions of the types follow from the parts alone, at any number of threads. The ids of the sets and of the lists of class parts that a new type brings follow arrival, as ever.
    // `partsOf` is called once for an index, on any thread, with that thread's arena, and interns no type. By the thread that may start a loop, while nobody else interns a type.
    void internInOrder(std::span<TypeID> ids, NOESCAPE const ScopedLambda<TypeParts(uint32_t index, TypeArena&)>& partsOf);
    // The same of TypeArena::intern(keyOf(index)), while nobody else interns a key.
    void internInOrder(std::span<PropertyKeyID> ids, NOESCAPE const ScopedLambda<PropertyKey(uint32_t index)>& keyOf);

private:
    TypeTable& m_table;
    const ParallelLoops& m_loops;
    Vector<std::unique_ptr<TypeArena>> m_arenas;
    // Kept from one call to the next: its pages are touched once.
    MallocSpan<Type> m_candidates;
};

// Defined in AOTTypeUnion.cpp and AOTTypeIntersection.cpp. Only TypeArena calls them, behind its shortcuts and its memo.
TypeParts uniteTypes(TypeArena&, const Type&, const Type&);
TypeParts intersectTypes(TypeArena&, const Type&, const Type&, DeclaredParts);
TypeParts subtractTypes(TypeArena&, const Type& value, const Type& removed);

inline TypeReader::TypeReader(const TypeArena& arena)
    : m_table(&arena.table())
    , m_arena(&arena)
{
}

ALWAYS_INLINE Type TypeArena::at(TypeID id) const
{
    if (liesInThread(id))
        return m_types[positionOf(id)];
    return m_table.at(id);
}

ALWAYS_INLINE bool TypeArena::saysNothing(TypeID id) const
{
    if (!id || isNeverType(id))
        return true;
    if (holdsOnlyPrimitives(id))
        return (primitivesInID(id) & Primitive::All) == Primitive::All;
    const Type& type = liesInThread(id) ? m_types[positionOf(id)] : m_table.typeKeptAt(id);
    return type.hasUnboundedUnknown() || (!type.hasObjects() && (type.primitives & Primitive::All) == Primitive::All);
}

inline Type TypeReader::at(TypeID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline const Type& TypeReader::recordOf(TypeID id) const { return m_arena ? m_arena->recordOf(id) : m_table->typeKeptAt(id); }
inline SetView<LayoutID, LayoutSetID> TypeReader::at(LayoutSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline std::span<const LayoutID> TypeReader::listOf(LayoutSetID id) const { return m_arena ? m_arena->listOf(id) : m_table->listOf(id); }
inline SetView<ObjectID, ObjectSetID> TypeReader::at(ObjectSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline SetView<StringID, StringSetID> TypeReader::at(StringSetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline SetView<PropertyKeyID, KeySetID> TypeReader::at(KeySetID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline std::span<const ClassPart> TypeReader::at(ClassPartsID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline const PropertyKey& TypeReader::at(PropertyKeyID id) const { return m_arena ? m_arena->at(id) : m_table->at(id); }
inline const ObjectRecord& TypeReader::at(ObjectID id) const { return m_table->objects().at(id); }
inline const ObjectTable& TypeReader::objects() const { return m_table->objects(); }
inline const BuiltinTable& TypeReader::builtins() const { return m_table->builtins(); }
inline const Program* TypeReader::program() const { return m_table->program(); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
