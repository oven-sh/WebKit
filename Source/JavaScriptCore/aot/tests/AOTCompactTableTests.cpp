/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCompactTable.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProcess.h"
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
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> compactTableTests();

using Width = CompactTable::Width;
using SlotKind = CompactTable::SlotKind;
using Status = CompactTable::Status;

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static Vector<uint8_t> compileWithReferences(ASCIILiteral referencesWord)
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static void runWithReferences(ASCIILiteral testName, ASCIILiteral referencesWord, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }

    TemporaryFile image(".image"_s, compileWithReferences(referencesWord).span());
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

static void testTableStartsSmall(LoadedProgram& program)
{
    AOT_TEST_CHECK(!usesWideReferences());
    Collection set(program, Collection::Kind::Set);
    AOT_TEST_CHECK(!set.table());
    set.add(numberedString(program, "key"_s, 0));
    CompactTable* table = set.table();
    AOT_TEST_CHECK(table && table->isLive() && table->isHeapTable() && !table->isInImage());
    AOT_TEST_CHECK(table->structure() == CompactTable::structureOf(program.vm()));
    AOT_TEST_CHECK(table->size() == 1 && table->used() == 1);
    AOT_TEST_CHECK(table->capacity() >= CompactTable::initialCapacity && CompactTable::slotKindFor(table->capacity()) == SlotKind::Tag);
    AOT_TEST_CHECK(table->keyWidth() == Width::Narrow && table->valueWidth() == Width::None && !table->hasValues());

    Collection map(program, Collection::Kind::Map);
    map.add(numberedString(program, "key"_s, 0), numberedString(program, "value"_s, 0));
    AOT_TEST_CHECK(map.table()->keyWidth() == Width::Narrow && map.table()->valueWidth() == Width::Narrow && map.table()->hasValues());
}

static void testWideReferencesStartWide(LoadedProgram& program)
{
    AOT_TEST_CHECK(usesWideReferences());
    Collection set(program, Collection::Kind::Set);
    set.add(numberedString(program, "key"_s, 0));
    AOT_TEST_CHECK(set.table()->keyWidth() == Width::Wide && set.table()->valueWidth() == Width::None);
    Collection map(program, Collection::Kind::Map);
    map.add(jsNumber(1.5), jsNumber(2.5));
    AOT_TEST_CHECK(map.table()->keyWidth() == Width::Wide && map.table()->valueWidth() == Width::Wide);
    AOT_TEST_CHECK(map.get(jsNumber(1.5)) == jsNumber(2.5) && !map.has(jsNumber(2.5)));
}

static void testKeysThatAreNoNumbersStayNarrow(LoadedProgram& program)
{
    VM& vm = program.vm();
    constexpr unsigned numberOfStrings = 100000;
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < numberOfStrings; ++number)
        set.add(numberedString(program, "key"_s, number));
    CompactTable* table = set.table();
    AOT_TEST_CHECK(table->size() == numberOfStrings && table->keyWidth() == Width::Narrow);
    AOT_TEST_CHECK(table->used() + 5 <= table->capacity());
    uint32_t capacity = table->capacity();

    const JSValue others[] = { jsUndefined(), jsNull(), jsBoolean(true), Symbol::create(vm), constructEmptyObject(program.realm()) };
    for (unsigned i = 0; i < std::size(others); ++i) {
        AOT_TEST_CHECK(!set.has(others[i]));
        set.add(others[i]);
        AOT_TEST_CHECK(set.table() == table && table->isLive());
        AOT_TEST_CHECK(table->capacity() == capacity && table->keyWidth() == Width::Narrow);
        AOT_TEST_CHECK(table->used() == numberOfStrings + i + 1 && table->size() == numberOfStrings + i + 1);
        AOT_TEST_CHECK(set.find(others[i]) == numberOfStrings + i && table->keyAt(numberOfStrings + i) == others[i]);
    }
    for (JSValue absent : { jsBoolean(false), JSValue(Symbol::create(vm)), JSValue(constructEmptyObject(program.realm())), jsNumber(1), JSValue(jsString(vm, String("undefined"_s))) })
        AOT_TEST_CHECK(!set.has(absent));

    set.add(jsNumber(1.5));
    CompactTable* wider = set.table();
    AOT_TEST_CHECK(wider != table && table->status() == Status::Replaced);
    AOT_TEST_CHECK(wider->keyWidth() == Width::Wide && wider->size() == numberOfStrings + 6);
    AOT_TEST_CHECK(set.find(jsNumber(1.5)) == numberOfStrings + 5);
    for (unsigned i = 0; i < std::size(others); ++i)
        AOT_TEST_CHECK(set.find(others[i]) == numberOfStrings + i);
    for (unsigned number = 0; number < numberOfStrings; number += 997)
        AOT_TEST_CHECK(set.find(numberedString(program, "key"_s, number)) == number);
    AOT_TEST_CHECK(!set.has(numberedString(program, "key"_s, numberOfStrings)) && !set.has(jsNumber(2.5)));
}

static void testNumberWidensTheKeyColumnAndKeepsTheOrder(LoadedProgram& program)
{
    for (JSValue number : { jsNumber(1.5), jsNumber(7), jsNumber(-0.0), jsDoubleNumber(PNaN) }) {
        Collection map(program, Collection::Kind::Map);
        Vector<JSValue> expected;
        MarkedArgumentBuffer roots;
        for (unsigned i = 0; i < 20; ++i) {
            JSString* key = numberedString(program, "key"_s, i);
            roots.append(key);
            expected.append(key);
            map.add(key, key);
        }
        AOT_TEST_CHECK(map.table()->keyWidth() == Width::Narrow && map.table()->valueWidth() == Width::Narrow);
        map.add(number, expected[0]);
        AOT_TEST_CHECK(map.table()->keyWidth() == Width::Wide && map.table()->valueWidth() == Width::Narrow);

        Vector<JSValue> keys = map.keysInOrder();
        AOT_TEST_CHECK(keys.size() == 21);
        for (unsigned i = 0; i < 20; ++i)
            AOT_TEST_CHECK(keys[i] == expected[i] && map.get(expected[i]) == expected[i]);
        AOT_TEST_CHECK(keys[20].isNumber() && map.get(number) == expected[0]);
    }
}

static void testNumberWidensTheValueColumn(LoadedProgram& program)
{
    Collection map(program, Collection::Kind::Map);
    MarkedArgumentBuffer keys;
    for (unsigned i = 0; i < 10; ++i) {
        keys.append(numberedString(program, "key"_s, i));
        map.add(keys.at(i), i % 2 ? jsUndefined() : keys.at(i));
    }
    AOT_TEST_CHECK(map.remove(keys.at(1)) && map.remove(keys.at(4)));
    AOT_TEST_CHECK(map.table()->valueWidth() == Width::Narrow);

    map.add(keys.at(6), jsNumber(42));
    AOT_TEST_CHECK(map.table()->valueWidth() == Width::Wide && map.table()->keyWidth() == Width::Narrow);
    AOT_TEST_CHECK(map.table()->size() == 8);
    for (unsigned i = 0; i < 10; ++i) {
        if (i == 1 || i == 4)
            AOT_TEST_CHECK(!map.has(keys.at(i)));
        else if (i == 6)
            AOT_TEST_CHECK(map.get(keys.at(i)) == jsNumber(42));
        else
            AOT_TEST_CHECK(map.get(keys.at(i)) == (i % 2 ? jsUndefined() : keys.at(i)));
    }

    map.add(numberedString(program, "key"_s, 10), jsNumber(1.5));
    AOT_TEST_CHECK(map.get(numberedString(program, "key"_s, 10)) == jsNumber(1.5) && map.table()->size() == 9);
}

static JSValue evaluate(LoadedProgram& program, ASCIILiteral source)
{
    return program.realms().evaluate(program.realms().plain(), source);
}

static void testKeysFindEachOtherAsSameValueZeroSays(LoadedProgram& program)
{
    VM& vm = program.vm();
    Collection map(program, Collection::Kind::Map);
    JSValue one = evaluate(program, "1n"_s);
    JSValue large = evaluate(program, "2n ** 70n"_s);
    map.add(jsNumber(-0.0), jsString(vm, String("zero"_s)));
    map.add(jsDoubleNumber(PNaN), jsString(vm, String("nan"_s)));
    map.add(one, jsString(vm, String("one"_s)));
    map.add(large, jsString(vm, String("large"_s)));
    AOT_TEST_CHECK(map.table()->size() == 4);

    AOT_TEST_CHECK(map.find(jsNumber(0)) == 0u && map.find(jsNumber(-0.0)) == 0u && map.find(jsDoubleNumber(0.0)) == 0u);
    AOT_TEST_CHECK(map.table()->keyAt(0) == jsNumber(0));
    AOT_TEST_CHECK(map.find(jsDoubleNumber(PNaN)) == 1u && map.find(evaluate(program, "0 / 0"_s)) == 1u && map.find(evaluate(program, "-(0 / 0)"_s)) == 1u);
    AOT_TEST_CHECK(map.find(evaluate(program, "2n - 1n"_s)) == 2u && map.find(evaluate(program, "BigInt(1)"_s)) == 2u);
    JSValue otherLarge = evaluate(program, "2n ** 69n * 2n"_s);
    AOT_TEST_CHECK(otherLarge != large && map.find(otherLarge) == 3u);

    for (JSValue absent : { jsNumber(1), evaluate(program, "'1'"_s), evaluate(program, "'0'"_s), evaluate(program, "0n"_s), evaluate(program, "-1n"_s), evaluate(program, "2n ** 70n + 1n"_s), evaluate(program, "-(2n ** 70n)"_s), evaluate(program, "2 ** 70"_s), evaluate(program, "'NaN'"_s), jsUndefined() })
        AOT_TEST_CHECK(!map.has(absent));

    map.add(jsNumber(0), jsString(vm, String("zero again"_s)));
    map.add(otherLarge, jsString(vm, String("large again"_s)));
    AOT_TEST_CHECK(map.table()->size() == 4);
    AOT_TEST_CHECK(map.remove(jsNumber(-0.0)) && !map.has(jsNumber(0)) && map.remove(evaluate(program, "1n"_s)) && !map.has(one));
    AOT_TEST_CHECK(map.table()->size() == 2);

    AOT_TEST_CHECK(map.find(jsNumber(1.0)) == map.find(jsDoubleNumber(1.0)));
    map.add(jsDoubleNumber(5.0), jsNull());
    AOT_TEST_CHECK(map.has(jsNumber(5)) && map.find(jsNumber(5)) == map.find(jsDoubleNumber(5.0)));
}

static void testBigIntsStayNarrow(LoadedProgram& program)
{
    Collection set(program, Collection::Kind::Set);
    set.add(evaluate(program, "2n ** 70n"_s));
    set.add(evaluate(program, "2n ** 71n"_s));
    AOT_TEST_CHECK(set.table()->keyWidth() == Width::Narrow && set.table()->size() == 2);
    AOT_TEST_CHECK(set.find(evaluate(program, "2n ** 71n"_s)) == 1u && !set.has(evaluate(program, "2n ** 72n"_s)));
}

static void checkEveryKeyAt(LoadedProgram& program, unsigned count)
{
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < count; ++number)
        set.add(numberedString(program, "key"_s, number));
    CompactTable* table = set.table();
    AOT_TEST_CHECK(table->size() == count && table->used() == count && table->capacity() >= count);
    for (unsigned number = 0; number < count; ++number)
        AOT_TEST_CHECK(set.find(numberedString(program, "key"_s, number)) == number);
    for (unsigned number = count; number < count + 300; ++number)
        AOT_TEST_CHECK(!set.has(numberedString(program, "key"_s, number)));
}

static void testEverySlotKindOfSmallTables(LoadedProgram& program)
{
    static_assert(CompactTable::slotKindFor(8) == SlotKind::Tag && CompactTable::slotKindFor(9) == SlotKind::Short);
    static_assert(CompactTable::slotKindFor(254) == SlotKind::Short && CompactTable::slotKindFor(255) == SlotKind::Long);
    static_assert(CompactTable::slotKindFor(3 << 22) == SlotKind::Long && CompactTable::slotKindFor((3 << 22) + 1) == SlotKind::Huge);
    for (unsigned count : { 1u, 4u, 5u, 8u, 9u, 63u, 64u, 65u, 253u, 254u, 255u, 256u, 1000u })
        checkEveryKeyAt(program, count);
}

static void testTablesCreatedAtTheSlotKindBoundaries(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (uint32_t minCapacity : { 8u, 9u, 254u, 255u }) {
        CompactTable* table = CompactTable::tryCreate(program.realm(), Width::Narrow, Width::None, minCapacity);
        AOT_TEST_CHECK(table && !scope.exception());
        AOT_TEST_CHECK(table->capacity() >= minCapacity && !table->size() && !table->used());
        AOT_TEST_CHECK(!table->find(program.realm(), numberedString(program, "key"_s, 0)));
    }
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

static void checkReplacedTables(LoadedProgram& program, Collection::Kind kind, bool keysAreNumbers, bool valuesAreNumbers, unsigned deletedStride)
{
    constexpr unsigned count = 700;
    auto keyAt = [&](unsigned number) {
        return keysAreNumbers ? jsNumber(number) : numberedString(program, "key"_s, number);
    };
    auto valueAt = [&](unsigned number) {
        return valuesAreNumbers ? jsNumber(number + 0.5) : numberedString(program, "value"_s, number);
    };
    auto isDeleted = [&](unsigned number) {
        return deletedStride && !(number % deletedStride);
    };

    Collection collection(program, kind);
    unsigned numberOfTables = 0;
    CompactTable* last = nullptr;
    for (unsigned number = 0; number < count; ++number) {
        collection.add(keyAt(number), valueAt(number));
        if (isDeleted(number))
            AOT_TEST_CHECK(collection.remove(keyAt(number)));
        if (collection.table() != std::exchange(last, collection.table()))
            ++numberOfTables;
    }
    AOT_TEST_CHECK(numberOfTables > 8);
    AOT_TEST_CHECK(collection.table()->keyWidth() == (keysAreNumbers ? Width::Wide : Width::Narrow));
    AOT_TEST_CHECK(collection.table()->valueWidth() == (kind == Collection::Kind::Set ? Width::None : valuesAreNumbers ? Width::Wide : Width::Narrow));

    Vector<JSValue> keys = collection.keysInOrder();
    unsigned index = 0;
    for (unsigned number = 0; number < count; ++number) {
        AOT_TEST_CHECK(collection.has(keyAt(number)) == !isDeleted(number));
        if (isDeleted(number))
            continue;
        AOT_TEST_CHECK(index < keys.size() && collection.find(keys[index++]) == collection.find(keyAt(number)));
        if (kind == Collection::Kind::Set)
            continue;
        JSValue value = collection.get(keyAt(number));
        AOT_TEST_CHECK(valuesAreNumbers ? value == valueAt(number) : textOf(value) == makeString("value"_s, number));
    }
    AOT_TEST_CHECK(index == keys.size() && collection.table()->size() == keys.size());
}

static void testReplacedTablesKeepEntriesOfEveryWidth(LoadedProgram& program)
{
    for (unsigned deletedStride : { 0u, 2u, 3u, 7u }) {
        for (bool keysAreNumbers : { false, true }) {
            checkReplacedTables(program, Collection::Kind::Set, keysAreNumbers, false, deletedStride);
            for (bool valuesAreNumbers : { false, true })
                checkReplacedTables(program, Collection::Kind::Map, keysAreNumbers, valuesAreNumbers, deletedStride);
        }
    }
}

static void testPositionInReplacedTableLeadsToTheSuccessor(LoadedProgram& program)
{
    VM& vm = program.vm();
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < 8; ++number)
        set.add(numberedString(program, "key"_s, number));
    CompactTable* first = set.table();
    AOT_TEST_CHECK(set.remove(numberedString(program, "key"_s, 1)) && set.remove(numberedString(program, "key"_s, 2)) && set.remove(numberedString(program, "key"_s, 6)));
    for (unsigned number = 8; set.table() == first; ++number)
        set.add(numberedString(program, "key"_s, number));
    AOT_TEST_CHECK(first->status() == Status::Replaced);

    auto keyFrom = [&](uint32_t entry) {
        CompactStorage::Position position = CompactTable::next(vm, first, entry);
        AOT_TEST_CHECK(position.storage == set.table());
        return textOf(position.key);
    };
    AOT_TEST_CHECK(keyFrom(0) == "key0"_s);
    AOT_TEST_CHECK(keyFrom(1) == "key3"_s && keyFrom(2) == "key3"_s && keyFrom(3) == "key3"_s);
    AOT_TEST_CHECK(keyFrom(4) == "key4"_s && keyFrom(5) == "key5"_s);
    AOT_TEST_CHECK(keyFrom(6) == "key7"_s && keyFrom(7) == "key7"_s && keyFrom(8) == "key8"_s);

    CompactTable* second = set.table();
    CompactTable::clear(program.realm(), set.owner());
    AOT_TEST_CHECK(second->status() == Status::Cleared && set.table() != second && !set.table()->size());
    AOT_TEST_CHECK(!CompactTable::next(vm, first, 3).storage && !CompactTable::next(vm, second, 0).storage);
    set.add(numberedString(program, "later"_s, 0));
    for (JSCell* storage : { static_cast<JSCell*>(first), static_cast<JSCell*>(second) }) {
        CompactStorage::Position position = CompactTable::next(vm, storage, 5);
        AOT_TEST_CHECK(position.storage == set.table() && !position.entry && textOf(position.key) == "later0"_s);
    }
}

// Somebody takes the first entry, and may stand behind it from then on.
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
                // As many entries fit as before, under other keys and in another order.
                for (unsigned number = capacity; number--;)
                    collection.add(numberedString(program, round % 2 ? "key"_s : "other"_s, number), value);
                AOT_TEST_CHECK(collection.table() == table && table->used() == capacity && table->size() == capacity);
                for (unsigned number = 0; number < capacity; ++number) {
                    AOT_TEST_CHECK(collection.find(numberedString(program, round % 2 ? "key"_s : "other"_s, number)) == capacity - 1 - number);
                    AOT_TEST_CHECK(!collection.has(numberedString(program, round % 2 ? "other"_s : "key"_s, number)));
                }
            }

            // Whoever holds the table and has taken nothing finds what is added after the clear, from its start.
            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(!CompactTable::next(vm, table, 0).storage);
            collection.add(numberedString(program, "later"_s, 0), value);
            CompactStorage::Position position = CompactTable::next(vm, table, 0);
            AOT_TEST_CHECK(position.storage == table && !position.entry && textOf(position.key) == "later0"_s);

            // That was a step: from now on a clear leaves the entries' numbers alone.
            collection.add(numberedString(program, "later"_s, 1), value);
            collection.add(numberedString(program, "later"_s, 2), value);
            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(collection.table() != table ? table->status() == Status::Cleared : table->used() == 3 && !table->size());
            collection.add(numberedString(program, "last"_s, 0), value);
            position = CompactTable::next(vm, table, 1);
            AOT_TEST_CHECK(position.storage == collection.table() && textOf(position.key) == "last0"_s);
        }

        // The table that takes the place of a walked one is walked: whoever stood in the old one goes on at the same entry of the new one.
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

        // A large table is let go.
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

static void testClearingASmallTableWithRoomDeletesItsEntries(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        Collection collection(program, kind);
        collection.add(numberedString(program, "key"_s, 0), numberedString(program, "value"_s, 0));
        CompactTable* table = collection.table();
        uint32_t capacity = table->capacity();
        AOT_TEST_CHECK(capacity >= CompactTable::initialCapacity);
        takeFirstEntry(program, collection);

        // Whoever stands in front of the entry or behind it finds what is added later, and the key that was there is found no more until it is added again.
        CompactTable::clear(program.realm(), collection.owner());
        AOT_TEST_CHECK(collection.table() == table && table->isLive() && !table->size() && table->used() == 1);
        AOT_TEST_CHECK(!collection.has(numberedString(program, "key"_s, 0)) && !CompactTable::next(vm, table, 0).storage && !CompactTable::next(vm, table, 1).storage);
        collection.add(numberedString(program, "key"_s, 0), numberedString(program, "value"_s, 1));
        AOT_TEST_CHECK(collection.table() == table && table->size() == 1 && table->used() == 2 && collection.find(numberedString(program, "key"_s, 0)) == 1u);
        for (uint32_t entry : { 0u, 1u }) {
            CompactStorage::Position position = CompactTable::next(vm, table, entry);
            AOT_TEST_CHECK(position.storage == table && position.entry == 1 && textOf(position.key) == "key0"_s);
        }

        // More than half is used: the next round may not fit, so a table takes its place.
        while (table->used() * 2 <= capacity)
            collection.add(numberedString(program, "more"_s, table->used()), numberedString(program, "value"_s, 2));
        AOT_TEST_CHECK(collection.table() == table);
        CompactTable::clear(program.realm(), collection.owner());
        AOT_TEST_CHECK(table->status() == Status::Cleared && collection.table() != table && !collection.table()->used());
        collection.add(numberedString(program, "later"_s, 0), numberedString(program, "value"_s, 3));
        CompactStorage::Position position = CompactTable::next(vm, table, 2);
        AOT_TEST_CHECK(position.storage == collection.table() && !position.entry && textOf(position.key) == "later0"_s);
    }
}

static void testAClearedTableNamesItsOwner(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        Collection collection(program, kind);
        Vector<CompactTable*> cleared;
        for (unsigned round = 0; round < 5; ++round) {
            // Some rounds outgrow the first table, so tables that were replaced stand between those that were cleared.
            unsigned count = CompactTable::initialCapacity * (round % 2 ? 6 : 1);
            for (unsigned number = 0; number < count; ++number)
                collection.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, round));
            cleared.append(collection.table());
            takeFirstEntry(program, collection);
            CompactTable::clear(program.realm(), collection.owner());
            AOT_TEST_CHECK(cleared.last()->status() == Status::Cleared && collection.table() != cleared.last());
        }
        // Whoever holds one of them holds the collection and what it has now, and none of the tables between.
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

static void testTheFirstTableHoldsTheFirstEntry(LoadedProgram& program)
{
    using Width = CompactTable::Width;
    JSValue text = numberedString(program, "text"_s, 0);
    JSValue number = jsNumber(1.5);
    struct Case {
        JSValue key;
        JSValue value;
        Width keyWidth;
        Width valueWidth;
    };
    for (const Case& first : { Case { text, text, Width::Narrow, Width::Narrow }, Case { number, text, Width::Wide, Width::Narrow }, Case { text, number, Width::Narrow, Width::Wide }, Case { number, number, Width::Wide, Width::Wide } }) {
        Collection map(program, Collection::Kind::Map);
        AOT_TEST_CHECK(!map.table());
        map.add(first.key, first.value);
        CompactTable* table = map.table();
        AOT_TEST_CHECK(table->isLive() && table->used() == 1 && table->keyWidth() == first.keyWidth && table->valueWidth() == first.valueWidth);
        AOT_TEST_CHECK(table->capacity() >= CompactTable::initialCapacity && map.find(first.key) == 0u);

        // The table that takes the place of a cleared one has the same columns.
        for (unsigned index = 1; table->used() * 2 <= table->capacity(); ++index)
            map.add(numberedString(program, "more"_s, index), text);
        AOT_TEST_CHECK(map.table() == table);
        takeFirstEntry(program, map);
        CompactTable::clear(program.realm(), map.owner());
        AOT_TEST_CHECK(map.table() != table && !map.table()->used() && map.table()->keyWidth() == first.keyWidth && map.table()->valueWidth() == first.valueWidth);
        CompactTable* fresh = map.table();
        map.add(first.key, first.value);
        AOT_TEST_CHECK(map.table() == fresh && map.find(first.key) == 0u);
    }
    for (const Case& first : { Case { text, { }, Width::Narrow, Width::None }, Case { number, { }, Width::Wide, Width::None } }) {
        Collection set(program, Collection::Kind::Set);
        set.add(first.key);
        AOT_TEST_CHECK(set.table()->used() == 1 && set.table()->keyWidth() == first.keyWidth && set.table()->valueWidth() == Width::None && set.has(first.key));
    }
}

static void testAFullTableGrowsOnceToWhatALoopStillAdds(LoadedProgram& program)
{
    using Width = CompactTable::Width;
    JSValue text = numberedString(program, "text"_s, 0);
    // Of every `period` iterations one comes to the addition, and the keys repeat after `distinct`.
    auto fill = [&](Collection& collection, uint32_t count, uint32_t distinct, uint32_t period, JSValue held) {
        unsigned tables = 0;
        CompactTable* last = collection.table();
        uint32_t sizeAtEntry = last ? last->size() : 0;
        for (uint32_t index = 0; index < count; ++index) {
            if (index % period)
                continue;
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

            // A second loop over a collection that has entries: the usual step while nothing is known, then one to the end.
            Collection more(program, Collection::Kind::Map);
            for (uint32_t index = 0; index < count; ++index)
                more.add(numberedString(program, "before"_s, index), held);
            bool isFullAtEntry = more.table()->used() == more.table()->capacity();
            unsigned tablesOfSecondLoop = fill(more, 200, 200, 1, held);
            AOT_TEST_CHECK(tablesOfSecondLoop <= 4 && more.table()->size() == count + 200);
            AOT_TEST_CHECK(!tablesOfSecondLoop || (tablesOfSecondLoop == 1 && isFullAtEntry) || more.table()->capacity() == capacityFor(more.table(), count + 200));
        }
    }

    // Keys that repeat and iterations that skip the addition: the table has room for about as many entries as it gets.
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

    // A number that comes while there is room widens the column and keeps the room.
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

    // Nothing is done for a collection without a table, for a table with room, in the first iteration, and past the limit.
    Collection untouched(program, Collection::Kind::Map);
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, 1, 0);
    AOT_TEST_CHECK(!untouched.table());
    untouched.add(text, text);
    CompactTable* initial = untouched.table();
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, 1, 0);
    AOT_TEST_CHECK(untouched.table() == initial);
    for (uint32_t index = 1; index < CompactTable::initialCapacity; ++index)
        untouched.add(numberedString(program, "key"_s, index), text);
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), 100, 0, CompactTable::initialCapacity);
    CompactTable::reserveForAdditions(program.realm(), untouched.owner(), uint64_t { 1 } << 40, CompactTable::initialCapacity, 0);
    AOT_TEST_CHECK(untouched.table() == initial);
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
    CompactTable* table = set.table();
    AOT_TEST_CHECK(table->find("alpha"_span8) == 0u && table->find("beta"_span8) == 2u && table->find("gamma"_span8) == 4u);
    AOT_TEST_CHECK(!table->find("alph"_span8) && !table->find("alphaa"_span8) && !table->find(""_span8) && !table->find("delta"_span8));

    JSString* rope = jsString(program.realm(), jsString(vm, String("al"_s)), jsString(vm, String("pha"_s)));
    JSString* substring = jsSubstring(program.realm(), jsString(vm, String("the beta version"_s)), 4, 4);
    AOT_TEST_CHECK(rope->isRope() && substring->isSubstring());
    AOT_TEST_CHECK(set.find(rope) == 0u && set.find(substring) == 2u);
    AOT_TEST_CHECK(!set.has(jsString(program.realm(), jsString(vm, String("al"_s)), jsString(vm, String("ph"_s)))));
}

static void testATableThatIsMostlyDeletedIsLetGo(LoadedProgram& program)
{
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        for (unsigned count : { CompactTable::minUsedToShrink, 100u, 5000u }) {
            Collection collection(program, kind);
            for (unsigned number = 0; number < count; ++number)
                collection.add(jsNumber(number), jsNumber(number));
            CompactTable* full = collection.table();
            unsigned removed = 0;
            while ((count - removed) * 4 >= count) {
                AOT_TEST_CHECK(collection.table() == full && full->isLive());
                AOT_TEST_CHECK(collection.remove(jsNumber(removed++)));
            }
            AOT_TEST_CHECK(collection.table() != full && !full->isLive());
            AOT_TEST_CHECK(collection.table()->size() == count - removed && collection.table()->capacity() < count && collection.table()->capacity() >= 2 * (count - removed));
            Vector<JSValue> keys = collection.keysInOrder();
            AOT_TEST_CHECK(keys.size() == count - removed);
            for (unsigned number = 0; number < count; ++number)
                AOT_TEST_CHECK(collection.has(jsNumber(number)) == (number >= removed) && (number < removed || keys[number - removed] == jsNumber(number)));
            // Whoever stands in the table that was let go goes on at the same entry.
            CompactStorage::Position position = CompactTable::next(program.vm(), full, removed + 1);
            AOT_TEST_CHECK(position.storage == collection.table() && position.key == jsNumber(removed + 1));
            while (removed < count)
                AOT_TEST_CHECK(collection.remove(jsNumber(removed++)));
            AOT_TEST_CHECK(!collection.table()->size() && collection.table()->capacity() < 2 * CompactTable::minUsedToShrink);
        }
    }
}

static void testACollectionThatSwingsAboutOneSizeKeepsItsTable(LoadedProgram& program)
{
    for (unsigned size : { 8u, 9u, 40u, 1000u }) {
        Collection map(program, Collection::Kind::Map);
        for (unsigned number = 0; number < 4 * size; ++number)
            map.add(jsNumber(number), jsNumber(number));
        unsigned first = 0;
        while (first < 3 * size)
            AOT_TEST_CHECK(map.remove(jsNumber(first++)));
        unsigned numberOfTables = 0;
        constexpr unsigned turnsForEachEntry = 50;
        CompactTable* last = map.table();
        for (unsigned number = 4 * size; number < (4 + turnsForEachEntry) * size; ++number) {
            AOT_TEST_CHECK(map.remove(jsNumber(first++)));
            map.add(jsNumber(number), jsNumber(number));
            if (map.table() != std::exchange(last, map.table()))
                ++numberOfTables;
        }
        AOT_TEST_CHECK(map.table()->size() == size && numberOfTables <= 3 * turnsForEachEntry);
    }
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
            // One slot may stay behind for good: the one that a key has left, or the last of its eight.
            AOT_TEST_CHECK(map.table() == table && table->numberOfTakenSlots() <= count + 2 && map.get(keyAt(7)) == jsNumber(turn));
        }
        for (unsigned number = 0; number < count; ++number)
            AOT_TEST_CHECK(map.has(keyAt(number)));
        AOT_TEST_CHECK(map.keysInOrder().last() == keyAt(7) || textOf(map.keysInOrder().last()) == "key7"_s);

        // Every third key goes and comes back, then every key is set again: all are found, and no slot is taken twice.
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

static bool deletedPrefixIsDeleted(Collection& collection)
{
    CompactTable* table = collection.table();
    uint32_t deletedPrefix = CompactTable::deletedPrefixOf(collection.owner());
    if (deletedPrefix > table->used())
        return false;
    for (uint32_t entry = 0; entry < deletedPrefix; ++entry) {
        if (table->keyAt(entry))
            return false;
    }
    return true;
}

// What a walk from the front writes down, as compiled code and operationAOTMapNext do.
static void noteDeletedPrefix(Collection& collection)
{
    CompactTable* table = collection.table();
    uint32_t& deletedPrefix = CompactTable::deletedPrefixOf(collection.owner());
    while (deletedPrefix < table->used() && !table->keyAt(deletedPrefix))
        ++deletedPrefix;
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
            UGPRPair answer = kind == Collection::Kind::Set ? operationAOTSetNext(&program.instance(), collection.owner(), JSValue::encode(jsNull()), 0, results) : operationAOTMapNext(&program.instance(), collection.owner(), JSValue::encode(jsNull()), 0, results);
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

static void testTheDeletedPrefixEndsWithItsTable(LoadedProgram& program)
{
    for (Collection::Kind kind : { Collection::Kind::Set, Collection::Kind::Map }) {
        for (bool keysAreNumbers : { false, true }) {
            auto keyAt = [&](unsigned number) { return keysAreNumbers ? jsNumber(number) : numberedString(program, "key"_s, number); };
            // A cache at a steady size: the oldest entry goes, a new one comes, through many tables.
            Collection cache(program, kind);
            constexpr unsigned size = 100;
            unsigned next = 0;
            while (next < size)
                cache.add(keyAt(next++), jsNumber(0));
            unsigned numberOfTables = 0;
            unsigned longestPrefix = 0;
            CompactTable* last = cache.table();
            for (unsigned turn = 0; turn < 2000; ++turn) {
                noteDeletedPrefix(cache);
                longestPrefix = std::max(longestPrefix, CompactTable::deletedPrefixOf(cache.owner()));
                AOT_TEST_CHECK(cache.table()->keyAt(CompactTable::deletedPrefixOf(cache.owner())) == keyAt(next - size) || !keysAreNumbers);
                AOT_TEST_CHECK(cache.remove(keyAt(next - size)) && deletedPrefixIsDeleted(cache));
                cache.add(keyAt(next++), jsNumber(turn));
                AOT_TEST_CHECK(deletedPrefixIsDeleted(cache));
                if (cache.table() != std::exchange(last, cache.table())) {
                    ++numberOfTables;
                    AOT_TEST_CHECK(!CompactTable::deletedPrefixOf(cache.owner()));
                }
            }
            AOT_TEST_CHECK(numberOfTables > 5 && longestPrefix > size / 2 && cache.keysInOrder().size() == size);

            // Let go because it is mostly deleted entries.
            for (unsigned number = next - size; number + 1 < next; ++number) {
                noteDeletedPrefix(cache);
                AOT_TEST_CHECK(cache.remove(keyAt(number)) && deletedPrefixIsDeleted(cache));
            }
            AOT_TEST_CHECK(cache.keysInOrder().size() == 1);

            // Cleared, with and without somebody who has walked it.
            for (bool isWalked : { false, true }) {
                for (unsigned count : { 4u, 60u }) {
                    Collection cleared(program, kind);
                    for (unsigned number = 0; number < count; ++number)
                        cleared.add(keyAt(number), jsNumber(number));
                    for (unsigned number = 0; number < count / 2; ++number)
                        AOT_TEST_CHECK(cleared.remove(keyAt(number)));
                    noteDeletedPrefix(cleared);
                    AOT_TEST_CHECK(CompactTable::deletedPrefixOf(cleared.owner()) == count / 2);
                    if (isWalked)
                        CompactTable::next(program.vm(), cleared.table(), 0);
                    CompactTable::clear(program.realm(), cleared.owner());
                    AOT_TEST_CHECK(deletedPrefixIsDeleted(cleared));
                    cleared.add(keyAt(1), jsNumber(1));
                    AOT_TEST_CHECK(deletedPrefixIsDeleted(cleared) && cleared.keysInOrder().size() == 1);
                }
            }
        }
    }
}

static void testCopyIsIndependent(LoadedProgram& program)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    Collection set(program, Collection::Kind::Set);
    for (unsigned number = 0; number < 20; ++number)
        set.add(numberedString(program, "key"_s, number));
    AOT_TEST_CHECK(set.remove(numberedString(program, "key"_s, 3)));
    CompactTable* copy = set.table()->tryCopy(program.realm());
    AOT_TEST_CHECK(copy && !scope.exception() && copy != set.table());
    AOT_TEST_CHECK(copy->size() == 19 && copy->isLive());
    set.add(numberedString(program, "key"_s, 3));
    AOT_TEST_CHECK(set.remove(numberedString(program, "key"_s, 5)));
    AOT_TEST_CHECK(!copy->find(program.realm(), numberedString(program, "key"_s, 3)) && copy->find(program.realm(), numberedString(program, "key"_s, 5)));
    AOT_TEST_CHECK(copy->size() == 19);
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

static void testKeysSurviveCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    Collection map(program, Collection::Kind::Map);
    for (unsigned number = 0; number < 500; ++number)
        map.add(numberedString(program, "key"_s, number), numberedString(program, "value"_s, number));
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        for (unsigned number = 0; number < 500; ++number) {
            JSValue value = map.get(numberedString(program, "key"_s, number));
            AOT_TEST_CHECK(value && vm.heap.isMarked(value.asCell()) && textOf(value) == makeString("value"_s, number));
        }
        map.add(numberedString(program, "young"_s, static_cast<unsigned>(scope)), numberedString(program, "young value"_s, 0));
    }
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(textOf(map.get(numberedString(program, "young"_s, static_cast<unsigned>(CollectionScope::Full)))) == "young value0"_s);
}

static void checkIntegerKeys(LoadedProgram& program, uint32_t count, SlotKind expectedSlotKind)
{
    Collection set(program, Collection::Kind::Set);
    for (uint32_t number = 0; number < count; ++number)
        set.add(jsNumber(number));
    CompactTable* table = set.table();
    AOT_TEST_CHECK(table->size() == count && table->used() == count);
    AOT_TEST_CHECK(CompactTable::slotKindFor(table->capacity()) == expectedSlotKind);
    uint32_t step = std::max<uint32_t>(count / 200000, 1);
    for (uint32_t number = 0; number < count; number += step)
        AOT_TEST_CHECK(set.find(jsNumber(number)) == number);
    for (uint32_t number : { count - 3, count - 2, count - 1 })
        AOT_TEST_CHECK(set.find(jsNumber(number)) == number);
    for (uint32_t number = count; number < count + 200000; ++number)
        AOT_TEST_CHECK(!set.has(jsNumber(number)));
    AOT_TEST_CHECK(!set.has(jsNumber(-1)) && !set.has(jsNumber(0.5)));
}

static void testTableCreatedWithTheLargestLongSlots(LoadedProgram& program)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    for (uint32_t count : { CompactTable::maxCapacityWithLongSlots, CompactTable::maxCapacityWithLongSlots + 1 }) {
        AOT_TEST_CHECK(count == 12582912 || count == 12582913);
        CompactTable* table = CompactTable::tryCreate(program.realm(), Width::Wide, Width::None, count);
        AOT_TEST_CHECK(table && !scope.exception() && table->capacity() >= count);
    }
    checkIntegerKeys(program, 1048574, SlotKind::Long);
    checkIntegerKeys(program, 1048575, SlotKind::Long);
}

static void testLongSlotsBelowTheBoundary(LoadedProgram& program)
{
    checkIntegerKeys(program, 500000, SlotKind::Long);
}

static void testIndexWiderThanTheHash(LoadedProgram& program)
{
    constexpr uint32_t count = 12582912 + 1000;
    static_assert(CompactTable::probeStartFor(24, 0xabcdef) == 0xabcdef && CompactTable::probeStartFor(25, 0xabcdef) == 0xabcdef << 1);
    checkIntegerKeys(program, count, SlotKind::Huge);
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
    for (ASCIILiteral referencesWord : { "references=narrow"_s, "references=wide"_s }) {
        std::unique_ptr<CompileOutput> output;
        runOnBuildThread([&] {
            Wire::CompileOptions options = makeDefaultCompileOptions();
            AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
            BuildContext build(options);
            std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programThatProbesSet)), Typed::IRForm::AsRead, build);
            AOT_TEST_CHECK(program);
            output = compileParsedProgram(WTF::move(program), build);
            AOT_TEST_CHECK(!output->image.isEmpty() && output->entryModuleIDs.size() == 1);
        });
        TemporaryFile image(".image"_s, output->image.span());
        Vector<String> arguments {
            makeString("--aotImagePath="_s, image.path()),
            String("-e"_s),
            makeString("globalThis.registry = { }; runCompiledEntryPoint("_s, output->entryModuleIDs[0], R"js().then(() => {
    let count = 12582912 + 1000;
    print(registry.countPresent(10, 5, 15));
    print(registry.countPresent(300, -10, 310));
    print(registry.countPresent(count, -50000, count + 50000));
}).catch(error => print("the script threw " + error));)js"_s),
        };
        ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
        String printed = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
        if (!result.exitedWith(EXIT_SUCCESS) || printed != "5\n300\n12583912\n"_s)
            dataLog("with ", referencesWord, " the shell ", result.description(), " and printed\n", printed);
        AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS) && printed == "5\n300\n12583912\n"_s);
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
        { "compact-table/table-starts-small"_s, narrow<testTableStartsSmall> },
        { "compact-table/keys-that-are-no-numbers-stay-narrow"_s, narrow<testKeysThatAreNoNumbersStayNarrow> },
        { "compact-table/number-widens-the-key-column-and-keeps-the-order"_s, narrow<testNumberWidensTheKeyColumnAndKeepsTheOrder> },
        { "compact-table/number-widens-the-value-column"_s, narrow<testNumberWidensTheValueColumn> },
        { "compact-table/keys-find-each-other-as-same-value-zero-says"_s, narrow<testKeysFindEachOtherAsSameValueZeroSays> },
        { "compact-table/big-ints-stay-narrow"_s, narrow<testBigIntsStayNarrow> },
        { "compact-table/every-slot-kind-of-small-tables"_s, narrow<testEverySlotKindOfSmallTables> },
        { "compact-table/tables-created-at-the-slot-kind-boundaries"_s, narrow<testTablesCreatedAtTheSlotKindBoundaries> },
        { "compact-table/deletion-of-half-and-growth"_s, narrow<testDeletionOfHalfAndGrowth> },
        { "compact-table/replaced-tables-keep-entries-of-every-width"_s, narrow<testReplacedTablesKeepEntriesOfEveryWidth> },
        { "compact-table/position-in-replaced-table-leads-to-the-successor"_s, narrow<testPositionInReplacedTableLeadsToTheSuccessor> },
        { "compact-table/clearing-a-table-that-nothing-was-added-to-keeps-it"_s, narrow<testClearingATableThatNothingWasAddedToKeepsIt> },
        { "compact-table/clearing-a-small-table-with-room-deletes-its-entries"_s, narrow<testClearingASmallTableWithRoomDeletesItsEntries> },
        { "compact-table/a-cleared-table-names-its-owner"_s, narrow<testAClearedTableNamesItsOwner> },
        { "compact-table/the-first-table-holds-the-first-entry"_s, narrow<testTheFirstTableHoldsTheFirstEntry> },
        { "compact-table/a-full-table-grows-once-to-what-a-loop-still-adds"_s, narrow<testAFullTableGrowsOnceToWhatALoopStillAdds> },
        { "compact-table/clearing-a-table-that-nobody-has-walked-empties-it"_s, narrow<testClearingATableThatNobodyHasWalkedEmptiesIt> },
        { "compact-table/clearing-a-table-that-nobody-has-walked-empties-it-wide"_s, wide<testClearingATableThatNobodyHasWalkedEmptiesIt> },
        { "compact-table/find-by-characters"_s, narrow<testFindByCharacters> },
        { "compact-table/copy-is-independent"_s, narrow<testCopyIsIndependent> },
        { "compact-table/mostly-deleted-is-let-go"_s, narrow<testATableThatIsMostlyDeletedIsLetGo> },
        { "compact-table/wide-references/mostly-deleted-is-let-go"_s, wide<testATableThatIsMostlyDeletedIsLetGo> },
        { "compact-table/swing-keeps-its-table"_s, narrow<testACollectionThatSwingsAboutOneSizeKeepsItsTable> },
        { "compact-table/deleted-prefix"_s, narrow<testTheDeletedPrefixEndsWithItsTable> },
        { "compact-table/ordinary-table-has-no-prefix"_s, narrow<testAnOrdinaryTableHasNoDeletedPrefix> },
        { "compact-table/wide-references/deleted-prefix"_s, wide<testTheDeletedPrefixEndsWithItsTable> },
        { "compact-table/readded-key-keeps-one-slot"_s, narrow<testAKeyThatIsDeletedAndAddedAgainKeepsOneSlot> },
        { "compact-table/wide-references/readded-key-keeps-one-slot"_s, wide<testAKeyThatIsDeletedAndAddedAgainKeepsOneSlot> },
        { "compact-table/out-of-memory-throws-and-leaves-the-table-usable"_s, narrow<testOutOfMemoryThrowsAndLeavesTheTableUsable> },
        { "compact-table/keys-survive-collections"_s, narrow<testKeysSurviveCollections> },
        { "compact-table/wide-references/start-wide"_s, wide<testWideReferencesStartWide> },
        { "compact-table/wide-references/keys-find-each-other-as-same-value-zero-says"_s, wide<testKeysFindEachOtherAsSameValueZeroSays> },
        { "compact-table/wide-references/every-slot-kind-of-small-tables"_s, wide<testEverySlotKindOfSmallTables> },
        { "compact-table/wide-references/deletion-of-half-and-growth"_s, wide<testDeletionOfHalfAndGrowth> },
        { "compact-table/wide-references/position-in-replaced-table-leads-to-the-successor"_s, wide<testPositionInReplacedTableLeadsToTheSuccessor> },
        { "compact-table/wide-references/clearing-a-small-table-with-room-deletes-its-entries"_s, wide<testClearingASmallTableWithRoomDeletesItsEntries> },
        { "compact-table/wide-references/keys-survive-collections"_s, wide<testKeysSurviveCollections> },
        { "slow/compact-table/long-slots-below-the-boundary"_s, narrow<testLongSlotsBelowTheBoundary> },
        { "slow/compact-table/table-created-with-the-largest-long-slots"_s, narrow<testTableCreatedWithTheLargestLongSlots> },
        { "slow/compact-table/index-wider-than-the-hash"_s, narrow<testIndexWiderThanTheHash> },
        { "slow/compact-table/compiled-code-probes-index-wider-than-the-hash"_s, testIndexWiderThanTheHashInCompiledCode },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
