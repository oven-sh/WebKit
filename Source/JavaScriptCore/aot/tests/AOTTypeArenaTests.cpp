/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRTestSupport.h"
#include "AOTProvisionalFunctions.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RecordsOfTask {
    PropertyKeyID key;
    LayoutSetID layouts;
    CalleeSetID callees;
    TypeID range;
    TypeID array;
    TypeID object;

    friend bool operator==(const RecordsOfTask&, const RecordsOfTask&) = default;
};

RecordsOfTask internRecordsOfTask(TypeArena& arena, unsigned task)
{
    RecordsOfTask records;
    records.key = arena.textKey(StringID(task % 4));
    records.layouts = arena.completeLayoutSet(std::array<LayoutID, 2> { LayoutID(task % 5), LayoutID(task % 3) });
    records.callees = arena.intern(CalleeSet { { FunctionID(task % 6), FunctionID(task % 2) }, { }, true });
    records.range = arena.intern(Type::integer(Range { 0, 100 + task % 7 }));
    const TypeID contents[] { records.range };
    records.array = arena.intern(Type::builtinClass(BuiltinClasses::Array, contents));
    records.object = arena.unionOf(arena.intern(Type::forLayoutSet(records.layouts)), records.array);
    return records;
}

Vector<RecordsOfTask> internInParallel(TypeTable& types, unsigned numberOfThreads, unsigned numberOfTasks)
{
    ParallelLoops loops(numberOfThreads);
    TypeInterningPhase phase(types, numberOfTasks);
    Vector<RecordsOfTask> records(numberOfTasks);
    forEachIndexInParallel(loops, numberOfTasks, [&](unsigned task) {
        TypeArena arena(phase, task);
        records[task] = internRecordsOfTask(arena, task);
    });
    TypeRemap remap = phase.finish(StringRemap());
    for (RecordsOfTask& record : records) {
        remap.apply(record.key);
        remap.apply(record.layouts);
        remap.apply(record.callees);
        remap.apply(record.range);
        remap.apply(record.array);
        remap.apply(record.object);
    }
    return records;
}

TypeID internNestedLists(TypeTable& types, const ParallelLoops* loops, unsigned depth)
{
    constexpr unsigned numberOfOtherTypes = 5000;
    TypeInterningPhase phase(types, 1);
    TypeID list;
    {
        TypeArena arena(phase, 0);
        for (unsigned index = 0; index < numberOfOtherTypes; ++index)
            arena.intern(Type::integer(Range { 0, 1000 + index }));
        list = arena.intern(Type::integer(Range { 0, 7 }));
        for (unsigned level = 0; level < depth; ++level) {
            const TypeID contents[] { list };
            list = arena.intern(Type::builtinClass(BuiltinClasses::Array, contents));
        }
    }
    return phase.finish(StringRemap(), loops)(list);
}

unsigned nestingDepthOf(const TypeTable& types, TypeID list)
{
    unsigned depth = 0;
    for (; !types.at(list).classes.isEmpty(); list = types.at(list).classes[0].contents[0])
        ++depth;
    return depth;
}

template<typename IDType>
bool haveEqualRecords(const TypeTable& first, const TypeTable& second, unsigned count)
{
    for (unsigned index = 0; index < count; ++index) {
        if (first.at(IDType(index)) != second.at(IDType(index)))
            return false;
    }
    return true;
}

} // anonymous namespace

static void internedTablesDoNotDependOnThreads()
{
    constexpr unsigned numberOfTasks = 500;
    TypeTable serialTypes(BuiltinTable::singleton());
    TypeTable parallelTypes(BuiltinTable::singleton());
    unsigned initialSize = serialTypes.size();
    Vector<RecordsOfTask> serialRecords = internInParallel(serialTypes, 1, numberOfTasks);
    Vector<RecordsOfTask> parallelRecords = internInParallel(parallelTypes, 8, numberOfTasks);
    AOT_TEST_CHECK(serialRecords == parallelRecords);

    AOT_TEST_CHECK(serialTypes.size() > initialSize && serialTypes.size() == parallelTypes.size());
    AOT_TEST_CHECK(serialTypes.numberOfPropertyKeys() == 4 && parallelTypes.numberOfPropertyKeys() == 4);
    AOT_TEST_CHECK(serialTypes.numberOfLayoutSets() == parallelTypes.numberOfLayoutSets() && serialTypes.numberOfCalleeSets() == parallelTypes.numberOfCalleeSets());
    AOT_TEST_CHECK(haveEqualRecords<TypeID>(serialTypes, parallelTypes, serialTypes.size()));
    AOT_TEST_CHECK(haveEqualRecords<PropertyKeyID>(serialTypes, parallelTypes, serialTypes.numberOfPropertyKeys()));
    AOT_TEST_CHECK(haveEqualRecords<LayoutSetID>(serialTypes, parallelTypes, serialTypes.numberOfLayoutSets()));
    AOT_TEST_CHECK(haveEqualRecords<CalleeSetID>(serialTypes, parallelTypes, serialTypes.numberOfCalleeSets()));
    AOT_TEST_CHECK(!serialTypes.hasOpenPhase() && !parallelTypes.hasOpenPhase());

    TypeTable tableWithoutPhase(BuiltinTable::singleton());
    TypeArena serialArena(tableWithoutPhase);
    for (unsigned task = 0; task < numberOfTasks; ++task)
        AOT_TEST_CHECK(internRecordsOfTask(serialArena, task) == serialRecords[task]);
}

static void listsNestedToAnyDepthAreMergedAlikeByOneThreadAndByMany()
{
    ParallelLoops loops(8);
    for (unsigned depth : { 3u, 253u, 254u, 255u, 256u, 257u, 300u, 1000u }) {
        TypeTable serialTypes(BuiltinTable::singleton());
        TypeTable parallelTypes(BuiltinTable::singleton());
        TypeID serialList = internNestedLists(serialTypes, nullptr, depth);
        TypeID parallelList = internNestedLists(parallelTypes, &loops, depth);
        AOT_TEST_CHECK(serialList == parallelList && serialTypes.size() == parallelTypes.size());
        AOT_TEST_CHECK(haveEqualRecords<TypeID>(serialTypes, parallelTypes, serialTypes.size()));
        AOT_TEST_CHECK(nestingDepthOf(parallelTypes, parallelList) == depth);
    }
}

static void provisionalStringLiteralsAreSortedAfterRemap()
{
    TestProgram test;
    StringInterningPhase stringPhase(test.program.strings, 2);
    TypeInterningPhase typePhase(test.program.types, 2);
    {
        StringArena strings(stringPhase, 1);
        AOT_TEST_CHECK(StringTable::isProvisional(strings.intern(StringView("literal of both tasks"_s))));
    }
    StringID ownLiteral;
    StringID sharedLiteral;
    TypeID provisionalType;
    {
        StringArena strings(stringPhase, 0);
        TypeArena types(typePhase, 0);
        ownLiteral = strings.intern(StringView("literal of the first task"_s));
        sharedLiteral = strings.intern(StringView("literal of both tasks"_s));
        AOT_TEST_CHECK(sharedLiteral < ownLiteral);
        provisionalType = types.intern(Type::stringLiteral(ownLiteral).unionWith(Type::stringLiteral(sharedLiteral), types));
        AOT_TEST_CHECK(TypeTable::isProvisional(provisionalType) && hasElements(types.at(provisionalType).stringLiterals, { sharedLiteral, ownLiteral }));
    }
    StringRemap stringRemap = stringPhase.finish();
    TypeRemap typeRemap = typePhase.finish(stringRemap);
    AOT_TEST_CHECK(stringRemap(ownLiteral) < stringRemap(sharedLiteral) && !StringTable::isProvisional(stringRemap(sharedLiteral)));

    TypeID finalType = typeRemap(provisionalType);
    AOT_TEST_CHECK(hasElements(test.program.at(finalType).stringLiterals, { stringRemap(ownLiteral), stringRemap(sharedLiteral) }));
    TypeArena serialArena(test.program.types);
    Type sameType = Type::stringLiteral(stringRemap(sharedLiteral)).unionWith(Type::stringLiteral(stringRemap(ownLiteral)), serialArena);
    AOT_TEST_CHECK(serialArena.intern(sameType) == finalType);
}

static void scratchArenaInOpenPhaseLeavesNoRecord()
{
    TypeTable types(BuiltinTable::singleton());
    unsigned initialSize = types.size();
    Type scratchType = Type::integer(Range { 0, 1000 });
    Type committedType = Type::integer(Range { 0, 2000 });
    TypeInterningPhase phase(types, 1);
    {
        ScratchTypeArena scratch(types);
        TypeID id = scratch.arena().intern(scratchType);
        AOT_TEST_CHECK(TypeTable::isProvisional(id) && types.at(id) == scratchType);
        AOT_TEST_CHECK(TypeTable::isProvisional(scratch.arena().completeLayoutSet(std::array<LayoutID, 1> { LayoutID(0u) })));
        AOT_TEST_CHECK(scratch.arena().intern(Type::anything()) == TypeTable::anything);
    }
    TypeID committed;
    {
        TypeArena arena(phase, 0);
        committed = arena.intern(committedType);
    }
    AOT_TEST_CHECK(types.hasOpenPhase() && types.size() == initialSize);
    TypeRemap remap = phase.finish(StringRemap());
    AOT_TEST_CHECK(types.size() == initialSize + 1 && !types.numberOfLayoutSets());
    AOT_TEST_CHECK(!types.find(scratchType) && types.find(committedType) == remap(committed));
}

static void scratchArenaWithoutOpenPhaseLeavesNoRecord()
{
    TypeTable types(BuiltinTable::singleton());
    unsigned initialSize = types.size();
    Type scratchType = Type::integer(Range { 0, 1000 });
    {
        ScratchTypeArena scratch(types);
        TypeID id = scratch.arena().intern(scratchType);
        AOT_TEST_CHECK(!types.hasOpenPhase() && TypeTable::isProvisional(id) && types.at(id) == scratchType);
    }
    AOT_TEST_CHECK(!types.hasOpenPhase() && types.size() == initialSize && !types.find(scratchType));

    TypeInterningPhase phase(types, 1);
    TypeID committed;
    {
        TypeArena arena(phase, 0);
        committed = arena.intern(scratchType);
    }
    TypeRemap remap = phase.finish(StringRemap());
    AOT_TEST_CHECK(types.size() == initialSize + 1 && types.find(scratchType) == remap(committed));
}

static void scratchArenasWithoutOpenPhaseOnManyThreads()
{
    constexpr unsigned numberOfArenas = 20000;
    TypeTable types(BuiltinTable::singleton());
    unsigned initialSize = types.size();
    ParallelLoops loops(8);
    for (unsigned round = 0; round < 3; ++round) {
        forEachIndexInParallel(loops, numberOfArenas, [&](unsigned index) {
            ScratchTypeArena scratch(types);
            RecordsOfTask records = internRecordsOfTask(scratch.arena(), index);
            AOT_TEST_CHECK(TypeTable::isProvisional(records.range) && types.at(records.range) == Type::integer(Range { 0, 100 + index % 7 }));
            AOT_TEST_CHECK(internRecordsOfTask(scratch.arena(), index) == records);
        });
        AOT_TEST_CHECK(!types.hasOpenPhase() && types.size() == initialSize && !types.numberOfLayoutSets());
        TypeInterningPhase phase(types, 0);
        phase.discard();
    }
}

static void layoutSetOfTwoTasksGetsOneID()
{
    TypeTable types(BuiltinTable::singleton());
    TypeInterningPhase phase(types, 3);
    std::array<LayoutSetID, 2> sets;
    {
        TypeArena first(phase, 0);
        TypeArena second(phase, 2);
        sets[0] = first.completeLayoutSet(std::array<LayoutID, 2> { LayoutID(1u), LayoutID(0u) });
        sets[1] = second.completeLayoutSet(std::array<LayoutID, 2> { LayoutID(0u), LayoutID(1u) });
        AOT_TEST_CHECK(TypeTable::isProvisional(sets[0]) && TypeTable::isProvisional(sets[1]) && sets[0] != sets[1]);
    }
    TypeRemap remap = phase.finish(StringRemap());
    AOT_TEST_CHECK(remap(sets[0]) == remap(sets[1]) && types.numberOfLayoutSets() == 1);
    AOT_TEST_CHECK(hasElements(types.at(remap(sets[0])).members, { LayoutID(0u), LayoutID(1u) }));
    AOT_TEST_CHECK(!remap.isIdentity() && !remap.isIdentityFor(0) && remap.isIdentityFor(1) && !remap.isIdentityFor(2));
}

static void tasksCommitInTheListedOrder()
{
    TypeTable types(BuiltinTable::singleton());
    unsigned initialSize = types.size();
    TypeInterningPhase phase(types, 1);
    AOT_TEST_CHECK(phase.addTasks(2) == 1);
    std::array<TypeID, 3> ids;
    for (unsigned task = 0; task < ids.size(); ++task) {
        TypeArena arena(phase, task);
        ids[task] = arena.intern(Type::forFunction(ProvisionalFunctions::idOf(task)));
    }
    FunctionRemap functions(Vector<FunctionID> { FunctionID(7u), FunctionID(8u), FunctionID(9u) });
    TypeRemap remap = phase.finish(std::array<unsigned, 3> { 2, 0, 1 }, functions, StringRemap());
    AOT_TEST_CHECK(remap(ids[2]) == TypeID(initialSize) && remap(ids[0]) == TypeID(initialSize + 1) && remap(ids[1]) == TypeID(initialSize + 2));
    for (unsigned task = 0; task < ids.size(); ++task)
        AOT_TEST_CHECK(types.at(remap(ids[task])).singleFunction() == FunctionID(7 + task));
}

static void discardedPhaseLeavesNoRecord()
{
    TypeTable types(BuiltinTable::singleton());
    unsigned initialSize = types.size();
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        AOT_TEST_CHECK(TypeTable::isProvisional(arena.intern(Type::integer(Range { 0, 1000 }))));
        AOT_TEST_CHECK(arena.unionOf(TypeTable::string, TypeTable::string) == TypeTable::string && arena.unionOf(TypeTable::never, TypeTable::number) == TypeTable::number);
    }
    phase.discard();
    AOT_TEST_CHECK(!types.hasOpenPhase() && types.size() == initialSize);
}

static SharedTypeMemo::Key sharedKeyAt(unsigned index)
{
    if (index % 2)
        return SharedTypeMemo::keyOf(SharedTypeMemo::Relation::Union, TypeID(index), TypeID(index + 1));
    return SharedTypeMemo::keyOf(index * 0x9e3779b97f4a7c15ull, static_cast<uint64_t>(index) << 40);
}

static void sharedMemoIsThreadSafe()
{
    constexpr unsigned numberOfKeys = 20000;
    SharedTypeMemo memo(false, true);
    ParallelLoops loops(8);
    forEachIndexInParallel(loops, numberOfKeys * 4, [&](unsigned position) {
        unsigned index = position % numberOfKeys;
        TypeID known = memo.find(sharedKeyAt(index));
        AOT_TEST_CHECK(!known || known == TypeID(index));
        if (!known)
            memo.add(sharedKeyAt(index), TypeID(index));
    });
    for (unsigned index = 0; index < numberOfKeys; ++index)
        AOT_TEST_CHECK(memo.find(sharedKeyAt(index)) == TypeID(index));
    AOT_TEST_CHECK(!memo.find(SharedTypeMemo::keyOf(SharedTypeMemo::Relation::Intersection, TypeID(1u), TypeID(2u))));
    AOT_TEST_CHECK(!memo.find(sharedKeyAt(numberOfKeys)) && !memo.find(sharedKeyAt(numberOfKeys + 1)));

    SharedTypeMemo::Counts counts = memo.counts();
    AOT_TEST_CHECK(counts.entries == numberOfKeys && !counts.fullShards);
    AOT_TEST_CHECK(counts.found + counts.notFound == numberOfKeys * 5 + 3 && counts.notFound >= numberOfKeys + 3);
}

static void fullSharedMemoRejectsNewEntries()
{
    constexpr unsigned numberOfKeys = 1200000;
    constexpr size_t maxSlotsInShard = 1 << 14;
    SharedTypeMemo memo(false, false, maxSlotsInShard);
    for (unsigned index = 0; index < numberOfKeys; ++index)
        memo.add(sharedKeyAt(index), TypeID(index));
    uint64_t numberOfKeysFound = 0;
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        TypeID known = memo.find(sharedKeyAt(index));
        AOT_TEST_CHECK(!known || known == TypeID(index));
        numberOfKeysFound += !!known;
    }
    SharedTypeMemo::Counts counts = memo.counts();
    AOT_TEST_CHECK(counts.fullShards && counts.entries == numberOfKeysFound && numberOfKeysFound < numberOfKeys);
}

static void arenasShareOnlySideEffectFreeAnswers()
{
    TypeTable types(BuiltinTable::singleton());
    SharedTypeMemo aboutTypes(false, true);
    SharedTypeMemo aboutProgram(false, true);
    types.setSharedMemos(&aboutTypes, &aboutProgram);
    SharedTypeMemo::Key keyOfUnion = SharedTypeMemo::keyOf(SharedTypeMemo::Relation::Union, TypeTable::string, TypeTable::boolean);

    TypeID stringOrBoolean;
    {
        TypeInterningPhase phase(types, 1);
        {
            TypeArena arena(phase, 0);
            stringOrBoolean = arena.unionOf(TypeTable::string, TypeTable::boolean);
            AOT_TEST_CHECK(TypeTable::isProvisional(stringOrBoolean) && !aboutTypes.find(keyOfUnion));
            AOT_TEST_CHECK(!TypeArena::sharedKeyOf(SharedTypeMemo::Relation::Union, stringOrBoolean, TypeTable::null));
        }
        stringOrBoolean = phase.finish(StringRemap())(stringOrBoolean);
    }

    SharedTypeMemo::Key firstQuestion = SharedTypeMemo::keyOf(1, 1);
    SharedTypeMemo::Key secondQuestion = SharedTypeMemo::keyOf(2, 2);
    SharedTypeMemo::Key thirdQuestion = SharedTypeMemo::keyOf(3, 3);
    TypeInterningPhase phase(types, 3);
    {
        TypeArena arena(phase, 0);
        AOT_TEST_CHECK(arena.unionOf(TypeTable::string, TypeTable::boolean) == stringOrBoolean && aboutTypes.find(keyOfUnion) == stringOrBoolean);

        // The arena answers the union from its own memo now.
        TypeID answer = arena.sharedAnswer(SharedAnswers::AboutProgram, firstQuestion, [&] {
            return arena.unionOf(TypeTable::string, TypeTable::boolean);
        });
        AOT_TEST_CHECK(answer == stringOrBoolean && !aboutProgram.find(firstQuestion));

        answer = arena.sharedAnswer(SharedAnswers::AboutProgram, secondQuestion, [&] {
            arena.intern(Type::integer(Range { 0, 1000 }));
            return TypeTable::number;
        });
        AOT_TEST_CHECK(answer == TypeTable::number && !aboutProgram.find(secondQuestion));

        answer = arena.sharedAnswer(SharedAnswers::AboutProgram, thirdQuestion, [&] {
            arena.markCurrentAnswersUnshareable();
            return TypeTable::number;
        });
        AOT_TEST_CHECK(answer == TypeTable::number && !aboutProgram.find(thirdQuestion));
    }
    {
        TypeArena arena(phase, 1);
        TypeID answer = arena.sharedAnswer(SharedAnswers::AboutProgram, firstQuestion, [&] {
            return arena.unionOf(TypeTable::string, TypeTable::boolean);
        });
        AOT_TEST_CHECK(answer == stringOrBoolean && aboutProgram.find(firstQuestion) == stringOrBoolean && !aboutTypes.find(firstQuestion));
    }
    {
        TypeArena arena(phase, 2);
        TypeID answer = arena.sharedAnswer(SharedAnswers::AboutProgram, firstQuestion, [&]() -> TypeID {
            RELEASE_ASSERT_NOT_REACHED();
        });
        AOT_TEST_CHECK(answer == stringOrBoolean);
        answer = arena.sharedAnswer(SharedAnswers::AboutProgram, std::nullopt, [&] {
            return TypeTable::object;
        });
        AOT_TEST_CHECK(answer == TypeTable::object);
    }
    TypeRemap remap = phase.finish(StringRemap());
    AOT_TEST_CHECK(!remap.isIdentityFor(0) && remap.isIdentityFor(1) && remap.isIdentityFor(2));
    types.setSharedMemos(nullptr, nullptr);
}

static void verifyingSharedMemoRecomputesAnswers()
{
    TypeTable types(BuiltinTable::singleton());
    SharedTypeMemo aboutTypes(true, false);
    SharedTypeMemo aboutProgram(true, false);
    types.setSharedMemos(&aboutTypes, &aboutProgram);
    SharedTypeMemo::Key question = SharedTypeMemo::keyOf(1, 1);
    unsigned numberOfComputations = 0;
    for (unsigned round = 0; round < 3; ++round) {
        ScratchTypeArena scratch(types);
        TypeID answer = scratch.arena().sharedAnswer(SharedAnswers::AboutTypes, question, [&] {
            ++numberOfComputations;
            return TypeTable::string;
        });
        AOT_TEST_CHECK(answer == TypeTable::string && aboutTypes.find(question) == TypeTable::string);
    }
    AOT_TEST_CHECK(numberOfComputations == 3);
    types.setSharedMemos(nullptr, nullptr);
}

static void internedTablesDoNotDependOnSharedAnswers()
{
    constexpr unsigned numberOfTasks = 500;
    TypeTable plainTypes(BuiltinTable::singleton());
    TypeTable sharingTypes(BuiltinTable::singleton());
    SharedTypeMemo aboutTypes(false, true);
    SharedTypeMemo aboutProgram(false, true);
    sharingTypes.setSharedMemos(&aboutTypes, &aboutProgram);
    for (unsigned round = 0; round < 3; ++round) {
        Vector<RecordsOfTask> plainRecords = internInParallel(plainTypes, 1, numberOfTasks);
        Vector<RecordsOfTask> sharedRecords = internInParallel(sharingTypes, 8, numberOfTasks);
        AOT_TEST_CHECK(plainRecords == sharedRecords);
        AOT_TEST_CHECK((aboutTypes.counts().entries > 0) == (round > 0));
    }
    sharingTypes.setSharedMemos(nullptr, nullptr);

    AOT_TEST_CHECK(aboutTypes.counts().found && !aboutProgram.counts().entries);
    AOT_TEST_CHECK(plainTypes.size() == sharingTypes.size() && plainTypes.numberOfLayoutSets() == sharingTypes.numberOfLayoutSets());
    AOT_TEST_CHECK(haveEqualRecords<TypeID>(plainTypes, sharingTypes, plainTypes.size()));
    AOT_TEST_CHECK(haveEqualRecords<LayoutSetID>(plainTypes, sharingTypes, plainTypes.numberOfLayoutSets()));
}

#define FOR_EACH_AOT_TYPE_ARENA_TEST(v) \
    v(internedTablesDoNotDependOnThreads) v(listsNestedToAnyDepthAreMergedAlikeByOneThreadAndByMany) v(provisionalStringLiteralsAreSortedAfterRemap) v(scratchArenaInOpenPhaseLeavesNoRecord) \
    v(scratchArenaWithoutOpenPhaseLeavesNoRecord) v(scratchArenasWithoutOpenPhaseOnManyThreads) v(layoutSetOfTwoTasksGetsOneID) \
    v(tasksCommitInTheListedOrder) v(discardedPhaseLeavesNoRecord) v(sharedMemoIsThreadSafe) v(fullSharedMemoRejectsNewEntries) \
    v(arenasShareOnlySideEffectFreeAnswers) v(verifyingSharedMemoRecomputesAnswers) v(internedTablesDoNotDependOnSharedAnswers)

std::span<const NativeTest> typeArenaTests()
{
#define AOT_TYPE_ARENA_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_TYPE_ARENA_TEST(AOT_TYPE_ARENA_TEST_ENTRY) };
#undef AOT_TYPE_ARENA_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
