/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTCompactTable.h"
#include "AOTDriver.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTOperationCallScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProcess.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "ErrorInstance.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <limits>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/PageBlock.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#if OS(LINUX)
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> compactTableTests();

using Width = CompactTable::Width;
using Status = CompactTable::Status;

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static void runWithReferences(ASCIILiteral testName, ASCIILiteral referencesWord, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText, { referencesWord }).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, image.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image with ", referencesWord, " ", result.description());
        failNativeTest(__FILE__, __LINE__, testName.characters());
    }
}

namespace {

class Collection {
public:
    enum class Kind : bool { Set, Map };

    Collection(LoadedProgram& program, Kind kind)
        : m_program(program)
        , m_kind(kind)
    {
        VM& vm = program.vm();
        JSGlobalObject* realm = program.realm();
        m_owner.set(vm, kind == Kind::Set ? static_cast<JSObject*>(JSSet::create(vm, realm->setStructure())) : static_cast<JSObject*>(JSMap::create(vm, realm->mapStructure())));
    }

    JSObject* owner() { return m_owner.get(); }
    CompactTable* table() { return CompactTable::tableOf(owner()); }

    void add(JSValue key, JSValue value = { })
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_program.vm());
        if (m_kind == Kind::Set)
            uncheckedDowncast<JSSet>(owner())->add(m_program.realm(), key);
        else
            uncheckedDowncast<JSMap>(owner())->set(m_program.realm(), key, value);
        AOT_TEST_CHECK(!scope.exception());
    }

    bool remove(JSValue key)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_program.vm());
        bool result = m_kind == Kind::Set ? uncheckedDowncast<JSSet>(owner())->remove(m_program.realm(), key) : uncheckedDowncast<JSMap>(owner())->remove(m_program.realm(), key);
        AOT_TEST_CHECK(!scope.exception());
        return result;
    }

    std::optional<uint32_t> find(JSValue key)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_program.vm());
        std::optional<uint32_t> entry = table()->find(m_program.realm(), key);
        AOT_TEST_CHECK(!scope.exception());
        return entry;
    }

    bool has(JSValue key) { return !!find(key); }

    JSValue get(JSValue key)
    {
        std::optional<uint32_t> entry = find(key);
        return entry ? table()->valueAt(*entry) : JSValue();
    }

    Vector<JSValue> keysInOrder()
    {
        Vector<JSValue> keys;
        for (CompactStorage::Position position = CompactTable::next(m_program.vm(), table(), 0); position.storage; position = CompactTable::next(m_program.vm(), position.storage, position.entry + 1))
            keys.append(position.key);
        return keys;
    }

private:
    LoadedProgram& m_program;
    Kind m_kind;
    Strong<JSObject> m_owner;
};

} // anonymous namespace

static JSString* numberedString(LoadedProgram& program, ASCIILiteral prefix, unsigned number)
{
    return jsString(program.vm(), makeString(prefix, number));
}

static String textOf(JSValue string)
{
    return asString(string)->tryGetValue().data;
}

static bool usesWideReferences()
{
    return Image::current()->header().options.useWideReferences();
}

static void testDeletionOfHalfAndGrowth(LoadedProgram& program)
{
    constexpr unsigned count = 1000;
    Collection map(program, Collection::Kind::Map);
    for (unsigned number = 0; number < count; ++number)
        map.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, number));
    CompactTable* before = map.table();
    for (unsigned number = 0; number < count; number += 2)
        AOT_TEST_CHECK(map.remove(numberedString(program, "key"_s, number)));
    AOT_TEST_CHECK(!map.remove(numberedString(program, "key"_s, 0)));
    AOT_TEST_CHECK(map.table() == before && before->size() == count / 2 && before->used() == count);
    for (unsigned number = 0; number < count; ++number)
        AOT_TEST_CHECK(map.has(numberedString(program, "key"_s, number)) == !!(number % 2));

    for (unsigned number = count; number < 3 * count; ++number)
        map.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, number));
    AOT_TEST_CHECK(map.table() != before && before->status() == Status::Replaced);
    AOT_TEST_CHECK(map.table()->size() == count / 2 + 2 * count);

    Vector<JSValue> keys = map.keysInOrder();
    AOT_TEST_CHECK(keys.size() == count / 2 + 2 * count);
    for (unsigned index = 0; index < keys.size(); ++index) {
        unsigned number = index < count / 2 ? 2 * index + 1 : index + count / 2;
        AOT_TEST_CHECK(textOf(keys[index]) == makeString("key"_s, number));
        AOT_TEST_CHECK(textOf(map.get(keys[index])) == makeString("value"_s, number));
    }
    for (unsigned number = 0; number < count; number += 2)
        AOT_TEST_CHECK(!map.has(numberedString(program, "key"_s, number)));

    map.add(numberedString(program, "key"_s, 0), jsNull());
    AOT_TEST_CHECK(textOf(map.keysInOrder().last()) == "key0"_s);
}

#if OS(LINUX)
static size_t numberOfPagesInMemory(CompactTable* table)
{
    uintptr_t start = roundUpToMultipleOf(pageSize(), std::bit_cast<uintptr_t>(table));
    uintptr_t end = (std::bit_cast<uintptr_t>(table) + table->cellSize()) & ~(pageSize() - 1);
    Vector<unsigned char> pages((end - start) / pageSize());
    AOT_TEST_CHECK(!mincore(std::bit_cast<void*>(start), end - start, pages.mutableSpan().data()));
    return std::ranges::count_if(pages, [](unsigned char page) { return page & 1; });
}
#endif

static void testATableThatWasReplacedGivesItsPagesBack(LoadedProgram& program)
{
    constexpr unsigned count = 200000;
    constexpr unsigned standsAt = 100000;
    constexpr unsigned deletedOnEachSide = 3000;
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < count; ++number)
        set.add(numberedString(program, "key"_s, number));
    CompactTable* before = set.table();
    AOT_TEST_CHECK(before->isPreciseAllocation());
    CompactStorage::Position position = CompactTable::next(program.vm(), before, standsAt);
    AOT_TEST_CHECK(position.storage == before && position.entry == standsAt);
    for (unsigned number = 0; number < deletedOnEachSide; ++number) {
        AOT_TEST_CHECK(set.remove(numberedString(program, "key"_s, number * 2)));
        AOT_TEST_CHECK(set.remove(numberedString(program, "key"_s, count - 1 - number * 2)));
    }
#if OS(LINUX)
    size_t pagesOfTheList = 2 * deletedOnEachSide * sizeof(uint32_t) / pageSize() + 1;
    AOT_TEST_CHECK(numberOfPagesInMemory(before) > 4 * pagesOfTheList);
#endif

    unsigned added = count;
    while (set.table() == before)
        set.add(numberedString(program, "key"_s, added++));
    AOT_TEST_CHECK(before->status() == Status::Replaced);
#if OS(LINUX)
    AOT_TEST_CHECK(numberOfPagesInMemory(before) <= pagesOfTheList);
#endif

    unsigned expected = standsAt + 1;
    for (position = CompactTable::next(program.vm(), before, standsAt + 1); position.storage; position = CompactTable::next(program.vm(), position.storage, position.entry + 1)) {
        while (expected < count && expected > count - 2 * deletedOnEachSide && !((count - 1 - expected) % 2))
            ++expected;
        AOT_TEST_CHECK(textOf(position.key) == makeString("key"_s, expected++));
    }
    AOT_TEST_CHECK(expected == added);
    ensureStillAliveHere(before);
}

static void takeFirstEntry(LoadedProgram& program, Collection& collection)
{
    AOT_TEST_CHECK(CompactTable::next(program.vm(), collection.table(), 0).storage == collection.table());
}

static void testClearingATableThatNobodyHasWalkedEmptiesIt(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSValue value = numberedString(program, "value"_s, 0);
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        for (unsigned count : { 1u, 3u, 4u, 7u, 8u, 9u, 16u, 20u }) {
            Collection collection(program, kind);
            for (unsigned number = 0; number < count; ++number)
                collection.add(numberedString(program, "key"_s, number), value);
            CompactTable* table = collection.table();
            uint32_t capacity = table->capacity();
            AOT_TEST_CHECK(capacity <= CompactTable::maxCapacityEmptiedByClear);
            for (unsigned round = 0; round < 3; ++round) {
                CompactTable::clear(program.realm(), collection.owner());
                AOT_TEST_CHECK(collection.table() == table && table->isLive() && !table->used() && !table->size() && table->capacity() == capacity);
                for (unsigned number = 0; number < count; ++number)
                    AOT_TEST_CHECK(!collection.has(numberedString(program, "key"_s, number)) && !collection.has(numberedString(program, "other"_s, number)));
                for (unsigned number = capacity; number--;)
                    collection.add(numberedString(program, round % 2 ? "key"_s : "other"_s, number), value);
                AOT_TEST_CHECK(collection.table() == table && table->used() == capacity && table->size() == capacity);
                for (unsigned number = 0; number < capacity; ++number) {
                    AOT_TEST_CHECK(collection.find(numberedString(program, round % 2 ? "key"_s : "other"_s, number)) == capacity - 1 - number);
                    AOT_TEST_CHECK(!collection.has(numberedString(program, round % 2 ? "other"_s : "key"_s, number)));
                }
            }

            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(!CompactTable::next(vm, table, 0).storage);
            collection.add(numberedString(program, "later"_s, 0), value);
            CompactStorage::Position position = CompactTable::next(vm, table, 0);
            AOT_TEST_CHECK(position.storage == table && !position.entry && textOf(position.key) == "later0"_s);

            collection.add(numberedString(program, "later"_s, 1), value);
            collection.add(numberedString(program, "later"_s, 2), value);
            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(collection.table() != table ? table->status() == Status::Cleared : table->used() == 3 && !table->size());
            collection.add(numberedString(program, "last"_s, 0), value);
            position = CompactTable::next(vm, table, 1);
            AOT_TEST_CHECK(position.storage == collection.table() && textOf(position.key) == "last0"_s);
        }

        Collection grown(program, kind);
        for (unsigned number = 0; number < CompactTable::initialCapacity; ++number)
            grown.add(numberedString(program, "key"_s, number), value);
        CompactTable* smaller = grown.table();
        takeFirstEntry(program, grown);
        grown.add(numberedString(program, "key"_s, CompactTable::initialCapacity), value);
        CompactTable* larger = grown.table();
        AOT_TEST_CHECK(larger != smaller && smaller->status() == Status::Replaced);
        CompactTable::clear(program.realm(), grown.owner());
        AOT_TEST_CHECK(larger->status() == Status::Cleared && grown.table() != larger);
        grown.add(numberedString(program, "later"_s, 0), value);
        CompactStorage::Position position = CompactTable::next(vm, smaller, 1);
        AOT_TEST_CHECK(position.storage == grown.table() && !position.entry && textOf(position.key) == "later0"_s);

        Collection large(program, kind);
        while (!large.table() || large.table()->capacity() <= CompactTable::maxCapacityEmptiedByClear)
            large.add(numberedString(program, "key"_s, large.table() ? large.table()->used() : 0), value);
        CompactTable* largeTable = large.table();
        CompactTable::clear(program.realm(), large.owner());
        AOT_TEST_CHECK(largeTable->status() == Status::Cleared && large.table()->capacity() <= CompactTable::maxCapacityWithTags);
    }
}

static void testClearingATableThatNothingWasAddedToKeepsIt(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        Collection collection(program, kind);
        for (unsigned number = 0; number < CompactTable::initialCapacity; ++number)
            collection.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, number));
        CompactTable* filled = collection.table();
        takeFirstEntry(program, collection);
        CompactTable::clear(program.realm(), collection.owner());
        CompactTable* empty = collection.table();
        AOT_TEST_CHECK(filled->status() == Status::Cleared && empty != filled && empty->isLive() && !empty->used());

        CompactTable::clear(program.realm(), collection.owner());
        CompactTable::clear(program.realm(), collection.owner());
        AOT_TEST_CHECK(collection.table() == empty && empty->isLive() && !empty->used() && !CompactTable::next(vm, empty, 0).storage);

        collection.add(numberedString(program, "later"_s, 0), numberedString(program, "value"_s, 1));
        AOT_TEST_CHECK(collection.table() == empty);
        for (JSCell* storage : { static_cast<JSCell*>(filled), static_cast<JSCell*>(empty) }) {
            CompactStorage::Position position = CompactTable::next(vm, storage, 0);
            AOT_TEST_CHECK(position.storage == empty && !position.entry && textOf(position.key) == "later0"_s);
        }
    }
}

static void testAClearedTableNamesItsOwner(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        Collection collection(program, kind);
        Vector<CompactTable*> cleared;
        for (unsigned round = 0; round < 5; ++round) {
            unsigned count = CompactTable::initialCapacity * (round % 2 ? 6 : 1);
            for (unsigned number = 0; number < count; ++number)
                collection.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, round));
            cleared.append(collection.table());
            takeFirstEntry(program, collection);
            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(cleared.last()->status() == Status::Cleared && collection.table() != cleared.last());
        }
        for (CompactTable* table : cleared) {
            AOT_TEST_CHECK(table->successorOfObsoleteTable() == collection.owner());
            AOT_TEST_CHECK(!CompactTable::next(vm, table, 0).storage && !CompactTable::next(vm, table, 3).storage);
        }
        collection.add(numberedString(program, "later"_s, 0), numberedString(program, "value"_s, 9));
        for (CompactTable* table : cleared) {
            CompactStorage::Position position = CompactTable::next(vm, table, 2);
            AOT_TEST_CHECK(position.storage == collection.table() && !position.entry && textOf(position.key) == "later0"_s);
        }
    }
}

static void testAFullTableGrowsOnceToWhatALoopStillAdds(LoadedProgram& program)
{
    using Width = CompactTable::Width;
    JSValue text = numberedString(program, "text"_s, 0);
    auto fill = [&](Collection& collection, uint32_t count, uint32_t distinct, uint32_t period, JSValue held) {
        unsigned tables = 0;
        CompactTable* last = collection.table();
        uint32_t sizeAtEntry = last ? last->size() : 0;
        for (uint32_t index = 0; index < count; ++index) {
            if (index % period)
                continue;
            if (index)
                CompactTable::reserveForAdditions(program.realm(), collection.owner(), count - index, index, sizeAtEntry);
            collection.add(numberedString(program, "key"_s, index % distinct), held);
            if (collection.table() != last) {
                last = collection.table();
                ++tables;
            }
        }
        return tables;
    };
    auto capacityFor = [&](const CompactTable* table, uint32_t count) {
        return CompactTable::tryCreate(program.realm(), table->keyWidth(), table->valueWidth(), std::max(count, CompactTable::initialCapacity))->capacity();
    };
    for (uint32_t count : { 1u, 4u, 5u, 9u, 20u, 100u, 300u, 5000u }) {
        for (JSValue held : { text, JSValue(jsNumber(0.5)) }) {
            Collection map(program, Collection::Kind::Map);
            unsigned tables = fill(map, count, count, 1, held);
            CompactTable* table = map.table();
            unsigned steps = 1;
            for (uint64_t capacity = CompactTable::initialCapacity; capacity < count; capacity *= CompactTable::largestStepInLoop)
                ++steps;
            AOT_TEST_CHECK(tables <= steps && table->used() == count && table->size() == count && table->capacity() == capacityFor(table, count));
            AOT_TEST_CHECK(table->keyWidth() == Width::Narrow && table->valueWidth() == (held.isNumber() ? Width::Wide : Width::Narrow));
            for (uint32_t index = 0; index < count; ++index)
                AOT_TEST_CHECK(map.find(numberedString(program, "key"_s, index)) == index);
            AOT_TEST_CHECK(!map.has(numberedString(program, "key"_s, count)));

            Collection more(program, Collection::Kind::Map);
            for (uint32_t index = 0; index < count; ++index)
                more.add(numberedString(program, "before"_s, index), held);
            bool isFullAtEntry = more.table()->used() == more.table()->capacity();
            unsigned tablesOfSecondLoop = fill(more, 200, 200, 1, held);
            AOT_TEST_CHECK(tablesOfSecondLoop <= 4 && more.table()->size() == count + 200);
            AOT_TEST_CHECK(!tablesOfSecondLoop || (tablesOfSecondLoop == 1 && isFullAtEntry) || more.table()->capacity() == capacityFor(more.table(), count + 200));
        }
    }

    for (uint32_t distinct : { 1u, 3u, 5u, 6u, 30u }) {
        Collection map(program, Collection::Kind::Map);
        fill(map, 6000, distinct, 1, text);
        AOT_TEST_CHECK(map.table()->size() == distinct && map.table()->capacity() <= capacityFor(map.table(), distinct * CompactTable::largestStepInLoop));
    }
    for (uint32_t period : { 2u, 3u, 10u, 100u }) {
        Collection map(program, Collection::Kind::Map);
        unsigned tables = fill(map, 6000, 6000, period, text);
        uint32_t size = (6000 + period - 1) / period;
        AOT_TEST_CHECK(map.table()->size() == size && map.table()->capacity() <= capacityFor(map.table(), 2 * size) && tables <= 6);
    }

    for (bool repeatsKey : { false, true }) {
        constexpr uint32_t turns = 3000;
        Collection map(program, Collection::Kind::Map);
        unsigned tables = 0;
        CompactTable* last = nullptr;
        for (uint32_t turn = 0; turn < turns; ++turn) {
            for (ASCIILiteral prefix : { "first"_s, repeatsKey ? "first"_s : "second"_s }) {
                if (turn)
                    CompactTable::reserveForAdditions(program.realm(), map.owner(), turns - turn, turn, 0);
                map.add(numberedString(program, prefix, turn), text);
                if (map.table() != std::exchange(last, map.table()))
                    ++tables;
            }
        }
        uint32_t size = repeatsKey ? turns : 2 * turns;
        AOT_TEST_CHECK(map.table()->size() == size && tables <= 6);
        AOT_TEST_CHECK(map.table()->capacity() <= capacityFor(map.table(), size + 2 * CompactTable::largestStepInLoop));
    }

    Collection set(program, Collection::Kind::Set);
    for (uint32_t index = 0; index < CompactTable::initialCapacity; ++index)
        set.add(numberedString(program, "key"_s, index));
    CompactTable::reserveForAdditions(program.realm(), set.owner(), 24, CompactTable::initialCapacity, 0);
    CompactTable* narrowTable = set.table();
    uint32_t reserved = narrowTable->capacity();
    AOT_TEST_CHECK(narrowTable->keyWidth() == Width::Narrow && reserved >= 28 && narrowTable->used() == CompactTable::initialCapacity);
    set.add(jsNumber(1.5));
    CompactTable* wideTable = set.table();
    AOT_TEST_CHECK(wideTable != narrowTable && !narrowTable->isLive() && wideTable->keyWidth() == Width::Wide && wideTable->capacity() >= reserved);
    for (uint32_t index = 1; index < 24; ++index)
        set.add(jsNumber(index + 1.5));
    AOT_TEST_CHECK(set.table() == wideTable && set.has(jsNumber(24.5)) && set.has(numberedString(program, "key"_s, 0)));

    Collection untouched(program, Collection::Kind::Map);
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, 1, 0);
    AOT_TEST_CHECK(!untouched.table());
    untouched.add(text, text);
    CompactTable* initial = untouched.table();
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, 1, 0);
    AOT_TEST_CHECK(untouched.table() == initial);
    for (uint32_t index = 1; index < CompactTable::initialCapacity; ++index)
        untouched.add(numberedString(program, "key"_s, index), text);
    AOT_TEST_CHECK(untouched.table() == initial);

    for (int64_t belowZero : { int64_t { -1 }, std::numeric_limits<int64_t>::min() }) {
        CompactTable::reserveForAdditions(program.realm(), untouched.owner(), static_cast<uint64_t>(belowZero), CompactTable::initialCapacity, 0);
        CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, static_cast<uint64_t>(belowZero), 0);
        AOT_TEST_CHECK(untouched.table() == initial);
    }

    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), std::numeric_limits<int64_t>::max(), CompactTable::initialCapacity, 0);
    AOT_TEST_CHECK(untouched.table() != initial && untouched.table()->size() == CompactTable::initialCapacity && untouched.table()->capacity() <= capacityFor(untouched.table(), CompactTable::initialCapacity * CompactTable::largestStepInLoop));
    AOT_TEST_CHECK(untouched.find(text) == 0u && untouched.find(numberedString(program, "key"_s, CompactTable::initialCapacity - 1)) == CompactTable::initialCapacity - 1);
}

static void testALoopThatRunsManyTimesKeepsGrowthAmortized(LoadedProgram& program)
{
    JSValue text = numberedString(program, "text"_s, 0);
    struct Moves {
        unsigned tables { 0 };
        uint64_t entries { 0 };
    };
    auto runLoop = [&](Collection& collection, uint32_t runs, uint32_t turns, uint32_t additions) {
        Moves moves;
        CompactTable* last = nullptr;
        auto noteTable = [&] {
            if (collection.table() == std::exchange(last, collection.table()))
                return;
            ++moves.tables;
            moves.entries += last->size();
        };
        for (uint32_t run = 0; run < runs; ++run) {
            CompactTable::reserveForAdditions(program.realm(), collection.owner(), turns, 0, 0);
            noteTable();
            for (uint32_t turn = 0; turn < additions; ++turn) {
                collection.add(jsNumber(run * additions + turn), text);
                noteTable();
            }
        }
        return moves;
    };
    auto stepsOfGrowthTo = [](uint64_t count) {
        unsigned steps = 0;
        for (uint64_t capacity = CompactTable::initialCapacity; capacity < count; capacity *= 2)
            ++steps;
        return steps;
    };
    constexpr uint32_t turns = 1000;
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        for (uint32_t additions : { turns, 1u }) {
            uint32_t runs = additions == turns ? 100 : 2000;
            uint32_t total = runs * additions;
            Collection collection(program, kind);
            Moves moves = runLoop(collection, runs, turns, additions);
            AOT_TEST_CHECK(moves.tables <= stepsOfGrowthTo(total) + 1 && moves.entries <= 2 * static_cast<uint64_t>(total));
            AOT_TEST_CHECK(collection.table()->size() == total && collection.table()->used() == total);
            for (uint32_t number = 0; number < total; number += 97)
                AOT_TEST_CHECK(collection.find(jsNumber(number)) == number);
            AOT_TEST_CHECK(!collection.has(jsNumber(total)));
        }
    }
}

static JSArray* listForNewCollection(LoadedProgram& program, std::initializer_list<JSValue> elements)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSArray* list = constructEmptyArray(program.realm(), nullptr);
    unsigned index = 0;
    for (JSValue element : elements) {
        if (element)
            list->putDirectIndex(program.realm(), index, element);
        ++index;
    }
    AOT_TEST_CHECK(!scope.exception());
    return list;
}

template<typename Operation>
static CompactTable* tableOfCollectionCreatedFrom(LoadedProgram& program, Operation operation, JSArray* list)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    auto created = callOperationFromTest(program.vm(), operation, &program.instance(), JSValue::encode(list));
    AOT_TEST_CHECK(!scope.exception() && !created.exception);
    return created.value ? CompactTable::tableOf(asObject(created.value)) : nullptr;
}

static void testASetIsFilledFromTheNumbersOfAnArray(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    auto shapeOf = [](JSArray* list) { return list->indexingType() & IndexingShapeMask; };
    auto listOfNumbers = [&](unsigned length, auto numberAt) {
        JSArray* list = constructEmptyArray(realm, nullptr);
        for (unsigned index = 0; index < length; ++index)
            list->putDirectIndex(realm, index, jsNumber(numberAt(index)));
        AOT_TEST_CHECK(!scope.exception());
        return list;
    };

    for (bool areDoubles : { false, true }) {
        for (unsigned length : { 2u, 8u, 10u, 100u, 254u, 1000u }) {
            auto numberAt = [&](unsigned index) { return index / 2 + (areDoubles ? 0.5 : 0.0); };
            JSArray* list = listOfNumbers(length, numberAt);
            AOT_TEST_CHECK(shapeOf(list) == (areDoubles ? DoubleShape : Int32Shape));
            CompactTable* table = tableOfCollectionCreatedFrom(program, operationAOTTryNewSetFromIterable, list);
            AOT_TEST_CHECK(table && table->size() == length / 2 && table->used() == length / 2 && table->capacity() >= length);
            AOT_TEST_CHECK(table->keyWidth() == Width::Wide && table->valueWidth() == Width::None);
            for (unsigned entry = 0; entry < length / 2; ++entry)
                AOT_TEST_CHECK(table->keyAt(entry) == jsNumber(numberAt(2 * entry)) && table->find(realm, jsNumber(numberAt(2 * entry))) == entry);
            AOT_TEST_CHECK(!table->find(realm, jsNumber(numberAt(length))) && !table->find(realm, jsNumber(-1)));
        }
    }

    CompactTable* few = tableOfCollectionCreatedFrom(program, operationAOTTryNewSetFromIterable, listOfNumbers(1000, [](unsigned index) { return index % 10; }));
    AOT_TEST_CHECK(few && few->size() == 10 && few->used() == 10 && few->capacity() < 250);
    for (unsigned entry = 0; entry < 10; ++entry)
        AOT_TEST_CHECK(few->keyAt(entry) == jsNumber(entry) && few->find(realm, jsNumber(entry)) == entry);

    JSArray* doubles = listForNewCollection(program, { jsNumber(0.5), jsNumber(-0.0), jsNumber(0), jsDoubleNumber(3.0), jsNumber(3), jsNumber(0.5), jsNumber(1e300) });
    AOT_TEST_CHECK(shapeOf(doubles) == DoubleShape);
    CompactTable* normalized = tableOfCollectionCreatedFrom(program, operationAOTTryNewSetFromIterable, doubles);
    AOT_TEST_CHECK(normalized && normalized->size() == 4);
    AOT_TEST_CHECK(normalized->keyAt(0) == jsNumber(0.5) && normalized->keyAt(1) == jsNumber(0) && normalized->keyAt(2) == jsNumber(3) && normalized->keyAt(3) == jsNumber(1e300));
    AOT_TEST_CHECK(normalized->find(realm, jsNumber(-0.0)) == 1u && normalized->find(realm, jsDoubleNumber(3.0)) == 2u);

    for (JSValue number : { jsNumber(1), jsNumber(1.5) }) {
        JSArray* withHole = listForNewCollection(program, { number, JSValue(), number, jsNumber(7) });
        AOT_TEST_CHECK(shapeOf(withHole) == (number.isInt32() ? Int32Shape : DoubleShape));
        if (CompactTable* table = tableOfCollectionCreatedFrom(program, operationAOTTryNewSetFromIterable, withHole))
            AOT_TEST_CHECK(table->size() == 3 && table->keyAt(0) == number && table->keyAt(1) == jsUndefined() && table->keyAt(2) == jsNumber(7));
    }
}

static void testAMapIsFilledFromPairsWithStringKeys(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Width width = usesWideReferences() ? Width::Wide : Width::Narrow;

    for (bool valuesAreNumbers : { false, true }) {
        for (unsigned length : { 2u, 8u, 10u, 100u, 254u, 1000u }) {
            JSArray* pairs = constructEmptyArray(realm, nullptr);
            for (unsigned index = 0; index < length; ++index)
                pairs->putDirectIndex(realm, index, listForNewCollection(program, { numberedString(program, "key"_s, index / 2), valuesAreNumbers ? jsNumber(index + 0.5) : numberedString(program, "value"_s, index) }));
            AOT_TEST_CHECK(!scope.exception());
            CompactTable* table = tableOfCollectionCreatedFrom(program, operationAOTTryNewMapFromEntries, pairs);
            AOT_TEST_CHECK(table && table->size() == length / 2 && table->used() == length / 2);
            AOT_TEST_CHECK(table->keyWidth() == width && table->valueWidth() == (valuesAreNumbers ? Width::Wide : width));
            AOT_TEST_CHECK(usesWideReferences() || table->capacity() >= length);
            for (unsigned entry = 0; entry < length / 2; ++entry) {
                AOT_TEST_CHECK(textOf(table->keyAt(entry)) == makeString("key"_s, entry) && table->find(realm, numberedString(program, "key"_s, entry)) == entry);
                JSValue value = table->valueAt(entry);
                AOT_TEST_CHECK(valuesAreNumbers ? value == jsNumber(2 * entry + 1.5) : textOf(value) == makeString("value"_s, 2 * entry + 1));
            }
            AOT_TEST_CHECK(!table->find(realm, numberedString(program, "key"_s, length / 2)));
        }
    }

    JSString* rope = jsString(realm, jsString(vm, String("ke"_s)), jsString(vm, String("y0"_s)));
    AOT_TEST_CHECK(rope->isRope());
    JSArray* mixed = listForNewCollection(program, {
        listForNewCollection(program, { numberedString(program, "key"_s, 0), jsNull() }),
        listForNewCollection(program, { jsNumber(-0.0), jsBoolean(true) }),
        listForNewCollection(program, { rope, jsBoolean(false) }),
        listForNewCollection(program, { jsNumber(0), jsUndefined() }),
    });
    CompactTable* table = tableOfCollectionCreatedFrom(program, operationAOTTryNewMapFromEntries, mixed);
    AOT_TEST_CHECK(table && table->size() == 2 && textOf(table->keyAt(0)) == "key0"_s && table->valueAt(0) == jsBoolean(false) && table->keyAt(1) == jsNumber(0) && table->valueAt(1) == jsUndefined());

    for (JSArray* pair : { listForNewCollection(program, { numberedString(program, "key"_s, 0) }), listForNewCollection(program, { numberedString(program, "key"_s, 0), JSValue(), jsNull() }) })
        AOT_TEST_CHECK(!tableOfCollectionCreatedFrom(program, operationAOTTryNewMapFromEntries, listForNewCollection(program, { pair })));
}

static void testFindByCharacters(LoadedProgram& program)
{
    VM& vm = program.vm();
    Collection set(program, Collection::Kind::Set);
    set.add(jsString(vm, String("alpha"_s)));
    set.add(Symbol::create(vm));
    set.add(jsString(vm, String("beta"_s)));
    set.add(constructEmptyObject(program.realm()));
    static constexpr char16_t wideCharacters[] = { 'g', 'a', 'm', 'm', 'a' };
    set.add(jsString(vm, String(std::span(wideCharacters))));

    auto partOf = [&](ASCIILiteral text, unsigned offset, unsigned length) {
        JSString* part = jsSubstring(program.realm(), jsString(vm, String(text)), offset, length);
        AOT_TEST_CHECK(part->isSubstring());
        return part;
    };
    AOT_TEST_CHECK(set.find(partOf("the alpha version"_s, 4, 5)) == 0u && set.find(partOf("the beta version"_s, 4, 4)) == 2u && set.find(partOf("the gamma version"_s, 4, 5)) == 4u);
    AOT_TEST_CHECK(!set.has(partOf("the alpha version"_s, 4, 4)) && !set.has(partOf("the alphaa version"_s, 4, 6)) && !set.has(partOf("the delta version"_s, 4, 5)));

#if CPU(ADDRESS64)
    for (bool isMember : { true, false }) {
        auto* part = uncheckedDowncast<JSRopeString>(isMember ? partOf("the alpha version"_s, 4, 5) : partOf("the delta version"_s, 4, 5));
        unsigned hash = isMember ? String("alpha"_s).hash() : String("delta"_s).hash();
        AOT_TEST_CHECK(!part->hashOfSubstring());
        for (unsigned round = 0; round < 2; ++round) {
            auto answer = callOperationFromTest(vm, operationAOTMapHas, &program.instance(), set.owner(), JSValue::encode(part));
            AOT_TEST_CHECK(!answer.exception && JSValue::decode(answer.value) == jsBoolean(isMember));
            AOT_TEST_CHECK(part->isSubstring() && part->hashOfSubstring() == hash);
        }
        vm.heap.collectSync(CollectionScope::Full);
        AOT_TEST_CHECK(part->isSubstring() && part->hashOfSubstring() == hash && part->substringOffset() == 4 && part->length() == 5);
        AOT_TEST_CHECK(part->getString(program.realm()) == String(isMember ? "alpha"_s : "delta"_s));
    }
#endif

    JSString* rope = jsString(program.realm(), jsString(vm, String("al"_s)), jsString(vm, String("pha"_s)));
    AOT_TEST_CHECK(rope->isRope() && set.find(rope) == 0u);
    AOT_TEST_CHECK(!set.has(jsString(program.realm(), jsString(vm, String("al"_s)), jsString(vm, String("ph"_s)))));
}

static void testTheCountOfMovedEntriesRisesWithEveryDeedThatMovesOne(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto risesIn = [&](auto&& deed) {
        uint64_t before = vm.aotState().movesOfEntriesInCollections;
        deed();
        return vm.aotState().movesOfEntriesInCollections > before;
    };
    auto keyAt = [&](unsigned number) { return numberedString(program, "key"_s, number); };
    auto filled = [&](Collection& map, unsigned count, JSValue value) {
        for (unsigned number = 0; number < count; ++number)
            map.add(keyAt(number), value);
        return map.table();
    };

    Collection map(program, Collection::Kind::Map);
    CompactTable* table = filled(map, 1, jsNumber(0));
    AOT_TEST_CHECK(table->capacity() >= 3);
    AOT_TEST_CHECK(!risesIn([&] { map.add(keyAt(1), jsNumber(1)); }) && map.table() == table && table->used() == 2);
    AOT_TEST_CHECK(!risesIn([&] { map.add(keyAt(1), jsNumber(2)); }) && table->used() == 2);
    AOT_TEST_CHECK(!risesIn([&] { AOT_TEST_CHECK(map.get(keyAt(1)) == jsNumber(2) && !map.remove(keyAt(9))); }));
    AOT_TEST_CHECK(risesIn([&] { AOT_TEST_CHECK(map.remove(keyAt(1))); }) && map.table() == table);
    AOT_TEST_CHECK(risesIn([&] {
        for (unsigned number = 2; map.table() == table; ++number)
            map.add(keyAt(number), jsNumber(number));
    }) && table->status() == Status::Replaced);

    if (!usesWideReferences()) {
        Collection texts(program, Collection::Kind::Map);
        table = filled(texts, 1, keyAt(0));
        AOT_TEST_CHECK(table->valueWidth() == Width::Narrow);
        AOT_TEST_CHECK(risesIn([&] { texts.add(keyAt(1), jsNumber(1)); }) && texts.table() != table && texts.table()->valueWidth() == Width::Wide);

        Collection numbers(program, Collection::Kind::Map);
        table = filled(numbers, 1, jsNumber(0));
        AOT_TEST_CHECK(table->keyWidth() == Width::Narrow);
        AOT_TEST_CHECK(risesIn([&] { numbers.add(jsNumber(1.5), jsNumber(1)); }) && numbers.table() != table && numbers.table()->keyWidth() == Width::Wide);
    }

    Collection emptied(program, Collection::Kind::Map);
    table = filled(emptied, 3, jsNumber(0));
    AOT_TEST_CHECK(risesIn([&] { CompactTable::clear(program.realm(), emptied.owner()); }) && emptied.table() == table && !table->used());

    Collection replaced(program, Collection::Kind::Map);
    table = filled(replaced, CompactTable::initialCapacity, jsNumber(0));
    takeFirstEntry(program, replaced);
    AOT_TEST_CHECK(risesIn([&] { CompactTable::clear(program.realm(), replaced.owner()); }) && table->status() == Status::Cleared);

    Collection walked(program, Collection::Kind::Map);
    filled(walked, CompactTable::initialCapacity + 1, jsNumber(0));
    CompactTable::clear(program.realm(), walked.owner());
    table = filled(walked, 2, jsNumber(0));
    AOT_TEST_CHECK(table->capacity() <= CompactTable::maxCapacityWithTags && table->capacity() >= 4);
    takeFirstEntry(program, walked);
    AOT_TEST_CHECK(risesIn([&] { CompactTable::clear(program.realm(), walked.owner()); }) && walked.table() == table && table->used() == 2 && !table->size());

    Collection large(program, Collection::Kind::Map);
    table = filled(large, CompactTable::maxCapacityEmptiedByClear + 1, jsNumber(0));
    AOT_TEST_CHECK(risesIn([&] { CompactTable::clear(program.realm(), large.owner()); }) && table->status() == Status::Cleared);

    Collection shrunk(program, Collection::Kind::Map);
    table = filled(shrunk, 1000, jsNumber(0));
    uint32_t capacityWhenFull = table->capacity();
    for (unsigned number = 8; number < 1000; ++number) {
        CompactTable* held = shrunk.table();
        uint64_t before = vm.aotState().movesOfEntriesInCollections;
        AOT_TEST_CHECK(shrunk.remove(keyAt(number)));
        AOT_TEST_CHECK(vm.aotState().movesOfEntriesInCollections >= before + (shrunk.table() == held ? 1 : 2));
    }
    AOT_TEST_CHECK(table->status() == Status::Replaced && shrunk.table()->capacity() < capacityWhenFull);
}

static void testAKeyThatIsDeletedAndAddedAgainKeepsOneSlot(LoadedProgram& program)
{
    for (bool keysAreNumbers : { false, true }) {
        auto keyAt = [&](unsigned number) { return keysAreNumbers ? jsNumber(number) : numberedString(program, "key"_s, number); };
        Collection map(program, Collection::Kind::Map);
        constexpr unsigned count = 1000;
        for (unsigned number = 0; number < count; ++number)
            map.add(keyAt(number), jsNumber(number));
        CompactTable* table = map.table();
        AOT_TEST_CHECK(table->numberOfTakenSlots() == count && table->capacity() > count + 100);
        for (unsigned turn = 0; turn < 100; ++turn) {
            AOT_TEST_CHECK(map.remove(keyAt(7)) && !map.has(keyAt(7)));
            map.add(keyAt(7), jsNumber(turn));
            AOT_TEST_CHECK(map.table() == table && table->numberOfTakenSlots() <= count + 2 && map.get(keyAt(7)) == jsNumber(turn));
        }
        for (unsigned number = 0; number < count; ++number)
            AOT_TEST_CHECK(map.has(keyAt(number)));
        AOT_TEST_CHECK(map.keysInOrder().last() == keyAt(7) || textOf(map.keysInOrder().last()) == "key7"_s);

        for (unsigned round = 0; round < 3; ++round) {
            for (unsigned number = round; number < count; number += 3)
                AOT_TEST_CHECK(map.remove(keyAt(number)));
            for (unsigned number = 0; number < count; ++number)
                map.add(keyAt(number), jsNumber(number + round));
            AOT_TEST_CHECK(map.table()->size() == count && map.table()->numberOfTakenSlots() <= count + count / 3);
            for (unsigned number = 0; number < count; ++number)
                AOT_TEST_CHECK(map.get(keyAt(number)) == jsNumber(number + round));
        }
    }
}

static void testAnOrdinaryTableHasNoDeletedPrefix(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        Collection collection(program, kind);
        {
            SetForScope asBesideNoCompiledCode(vm.aotState().hasCompiledCode, false);
            collection.add(jsNumber(0), jsNumber(0));
        }
        auto hasCompactStorage = [&] {
            return kind == Collection::Kind::Set ? uncheckedDowncast<JSSet>(collection.owner())->hasCompactStorage() : uncheckedDowncast<JSMap>(collection.owner())->hasCompactStorage();
        };
        auto firstKey = [&] {
            EncodedJSValue results[2];
            UGPRPair answer = callOperationFromTest(vm, kind == Collection::Kind::Set ? operationAOTSetNext : operationAOTMapNext, &program.instance(), collection.owner(), JSValue::encode(jsNull()), 0, results);
            size_t hasEntry;
            size_t entry;
            decodeResult(answer, hasEntry, entry);
            return hasEntry ? JSValue::decode(results[0]) : JSValue();
        };
        AOT_TEST_CHECK(!hasCompactStorage());
        unsigned next = 1;
        while (next < 100)
            collection.add(jsNumber(next++), jsNumber(0));
        for (unsigned oldest = 0; oldest < 2000; ++oldest) {
            AOT_TEST_CHECK(firstKey() == jsNumber(oldest) && !CompactTable::deletedPrefixOf(collection.owner()));
            AOT_TEST_CHECK(collection.remove(jsNumber(oldest)));
            collection.add(jsNumber(next++), jsNumber(0));
        }
        for (unsigned oldest = 2000; oldest + 1 < next; ++oldest) {
            AOT_TEST_CHECK(firstKey() == jsNumber(oldest));
            AOT_TEST_CHECK(collection.remove(jsNumber(oldest)));
        }
        AOT_TEST_CHECK(firstKey() == jsNumber(next - 1) && !hasCompactStorage() && !CompactTable::deletedPrefixOf(collection.owner()));
    }
}

static bool takeOutOfMemoryError(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    if (!exception)
        return false;
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    bool isOutOfMemoryError = error && error->isOutOfMemoryError();
    scope.clearException();
    return isOutOfMemoryError;
}

static void testOutOfMemoryThrowsAndLeavesTheTableUsable(LoadedProgram& program)
{
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < 10; ++number)
        set.add(numberedString(program, "key"_s, number));
    CompactTable* table = set.table();

    AOT_TEST_CHECK(!CompactTable::tryCreate(program.realm(), Width::Narrow, Width::None, CompactTable::maxCapacity + 1));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));
    AOT_TEST_CHECK(!CompactTable::tryCreate(program.realm(), Width::Wide, Width::Wide, UINT32_MAX));
    AOT_TEST_CHECK(takeOutOfMemoryError(program));

    AOT_TEST_CHECK(set.table() == table && table->isLive() && table->size() == 10);
    for (unsigned number = 0; number < 10; ++number)
        AOT_TEST_CHECK(set.find(numberedString(program, "key"_s, number)) == number);
    for (unsigned number = 10; number < 100; ++number)
        set.add(numberedString(program, "key"_s, number));
    AOT_TEST_CHECK(set.table()->size() == 100 && set.find(numberedString(program, "key"_s, 99)) == 99u);
}

static constexpr ASCIILiteral programThatProbesSet = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = NewClosure function=f1
    PropSet v1, v2 key="countPresent"
    Return
}
function f1 name="countPresent" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Check v0 expectedType=i32
    v4 = Check v1 expectedType=i32
    v5 = Check v2 expectedType=i32
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="Set"
    v8 = Construct v7, -
    v9 = Integer integer=0
    v10 = Integer integer=1
    Jump -> next b1(v9)
  b1(v11):
    LoopHint
    v13 = Less v11, v3
    Branch v13 -> taken b2(), notTaken b3()
  b2():
    v14 = PropGet v8 key="add"
    v15 = Call v14, v8, v11
    v16 = Add v11, v10
    Jump -> next b1(v16)
  b3():
    Jump -> next b4(v4, v9)
  b4(v17, v18):
    LoopHint
    v20 = Less v17, v5
    Branch v20 -> taken b5(), notTaken b6()
  b5():
    v21 = PropGet v8 key="has"
    v22 = Call v21, v8, v17
    v23 = ToBoolean v22
    Branch v23 -> taken b7(), notTaken b8()
  b6():
    Return v18
  b7():
    v24 = Add v18, v10
    Jump -> next b9(v24)
  b8():
    Jump -> next b9(v18)
  b9(v25):
    v26 = Add v17, v10
    Jump -> next b4(v26, v25)
}
)IR"_s;

static void testIndexWiderThanTheHashInCompiledCode()
{
    for (ASCIILiteral way : waysToCompilePrograms) {
        for (ASCIILiteral referencesWord : { "references=narrow"_s, "references=wide"_s }) {
            std::unique_ptr<CompileOutput> output;
            runOnBuildThread([&] {
                output = compileForShellOnThisThread(programThatProbesSet, { referencesWord }, way);
            });
            std::span<const uint32_t> entryModuleIDs = output->records<uint32_t>(Wire::ResultSection::EntryModuleIds);
            AOT_TEST_CHECK(entryModuleIDs.size() == 1);
            TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
            Vector<String> arguments {
                makeString("--aotImagePath="_s, image.path()),
                String("-e"_s),
                makeString("globalThis.registry = { }; runCompiledEntryPoint("_s, entryModuleIDs[0], R"js().then(() => {
    let count = 12582912 + 1000;
    print(registry.countPresent(10, 5, 15));
    print(registry.countPresent(300, -10, 310));
    print(registry.countPresent(count, -50000, count + 50000));
}).catch(error => print("the script threw " + error));)js"_s),
            };
            ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
            String printed = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
            if (!result.exitedWith(EXIT_SUCCESS) || printed != "5\n300\n12583912\n"_s)
                dataLog("with ", way, " ", referencesWord, " the shell ", result.description(), " and printed\n", printed);
            AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS) && printed == "5\n300\n12583912\n"_s);
        }
    }
}

template<const ASCIILiteral& referencesWord, LoadedProgramTest body>
static void runNativeTestWithReferences()
{
    for (const NativeTest& test : compactTableTests()) {
        if (test.run == &runNativeTestWithReferences<referencesWord, body>) {
            runWithReferences(test.name, referencesWord, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ASCIILiteral narrowReferences = "references=narrow"_s;
static constexpr ASCIILiteral wideReferences = "references=wide"_s;
template<LoadedProgramTest body> static constexpr void (*narrow)() = runNativeTestWithReferences<narrowReferences, body>;
template<LoadedProgramTest body> static constexpr void (*wide)() = runNativeTestWithReferences<wideReferences, body>;

std::span<const NativeTest> compactTableTests()
{
    static constexpr NativeTest tests[] = {
        { "compact-table/deletion-of-half-and-growth"_s, narrow<testDeletionOfHalfAndGrowth> },
        { "compact-table/clearing-a-table-that-nothing-was-added-to-keeps-it"_s, narrow<testClearingATableThatNothingWasAddedToKeepsIt> },
        { "compact-table/a-cleared-table-names-its-owner"_s, narrow<testAClearedTableNamesItsOwner> },
        { "compact-table/a-full-table-grows-once-to-what-a-loop-still-adds"_s, narrow<testAFullTableGrowsOnceToWhatALoopStillAdds> },
        { "compact-table/wide-references/loop-that-runs-many-times"_s, wide<testALoopThatRunsManyTimesKeepsGrowthAmortized> },
        { "compact-table/set-from-numbers"_s, narrow<testASetIsFilledFromTheNumbersOfAnArray> },
        { "compact-table/wide-references/map-from-pairs"_s, wide<testAMapIsFilledFromPairsWithStringKeys> },
        { "compact-table/clearing-a-table-that-nobody-has-walked-empties-it"_s, narrow<testClearingATableThatNobodyHasWalkedEmptiesIt> },
        { "compact-table/find-by-characters"_s, narrow<testFindByCharacters> },
        { "compact-table/ordinary-table-has-no-prefix"_s, narrow<testAnOrdinaryTableHasNoDeletedPrefix> },
        { "compact-table/readded-key-keeps-one-slot"_s, narrow<testAKeyThatIsDeletedAndAddedAgainKeepsOneSlot> },
        { "compact-table/the-count-of-moved-entries-rises-with-every-deed-that-moves-one"_s, narrow<testTheCountOfMovedEntriesRisesWithEveryDeedThatMovesOne> },
        { "compact-table/wide-references/the-count-of-moved-entries-rises-with-every-deed-that-moves-one"_s, wide<testTheCountOfMovedEntriesRisesWithEveryDeedThatMovesOne> },
        { "compact-table/out-of-memory-throws-and-leaves-the-table-usable"_s, narrow<testOutOfMemoryThrowsAndLeavesTheTableUsable> },
        { "compact-table/wide-references/deletion-of-half-and-growth"_s, wide<testDeletionOfHalfAndGrowth> },
        { "compact-table/a-table-that-was-replaced-gives-its-pages-back"_s, narrow<testATableThatWasReplacedGivesItsPagesBack> },
        { "slow/compact-table/compiled-code-probes-index-wider-than-the-hash"_s, testIndexWiderThanTheHashInCompiledCode },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
