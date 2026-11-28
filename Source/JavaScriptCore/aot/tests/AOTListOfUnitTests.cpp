/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompileQueue.h"
#include "AOTIRParser.h"
#include "AOTListOfUnit.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTStages.h"
#include "AOTTestHarness.h"
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> listOfUnitTests();

namespace {

struct ElementWithList {
    ListOfUnit<uint32_t, 2> words;
    uint32_t tag { 0 };
};

void forEachListOfUnitIn(ElementWithList& element, auto& visit)
{
    visit(element.words);
}

} // anonymous namespace

template<>
inline constexpr bool holdsListsOfUnit<ElementWithList> = true;

namespace {

struct RarePartOfRecord {
    ListOfUnit<uint32_t> words;
    ListOfUnit<ElementWithList> elements;
    uint32_t number { 0 };
};

void forEachListOfUnitIn(RarePartOfRecord& part, auto& visit)
{
    visit(part.words);
    visit(part.elements);
}

} // anonymous namespace

template<>
inline constexpr bool holdsListsOfUnit<RarePartOfRecord> = true;

namespace {

struct RecordWithRarePart {
    ListOfUnit<uint8_t> bytes;
    RarePartOfUnit<RarePartOfRecord> rarePart;
};

void forEachListOfUnitIn(RecordWithRarePart& record, auto& visit)
{
    visit(record.bytes);
    visit(record.rarePart);
}

struct RecordWithLists {
    ListOfUnit<uint8_t> bytes;
    ListOfUnit<uint64_t, 1> few;
    ListOfUnit<ElementWithList> elements;
    ListOfUnit<uint16_t> nothing;
    uint32_t number { 0 };
};

void forEachListOfUnitIn(RecordWithLists& record, auto& visit)
{
    visit(record.bytes);
    visit(record.few);
    visit(record.elements);
    visit(record.nothing);
}

struct HeapFinder {
    template<typename T, size_t inlineCapacity>
    void operator()(ListOfUnit<T, inlineCapacity>& list)
    {
        ++numberOfLists;
        numberOnHeap += list.liesOnHeap();
        numberInBlocks += list.liesInBlock();
        if constexpr (holdsListsOfUnit<T>) {
            for (T& element : list)
                forEachListOfUnitIn(element, *this);
        }
    }

    template<typename Part>
    void operator()(RarePartOfUnit<Part>& rarePart)
    {
        numberOnHeap += rarePart.liesOnHeap();
        numberInBlocks += rarePart.liesInBlock();
        if (Part* part = rarePart.ifExists())
            forEachListOfUnitIn(*part, *this);
    }

    size_t numberOfLists { 0 };
    size_t numberOnHeap { 0 };
    size_t numberInBlocks { 0 };
};

using ListOfThree = ListOfUnit<uint32_t, 3>;
using ListOfTwo = ListOfUnit<uint32_t, 2>;

} // anonymous namespace

static RecordWithLists makeRecordWithLists(uint32_t number, size_t numberOfBytes)
{
    RecordWithLists record;
    record.number = number;
    for (size_t index = 0; index < numberOfBytes; ++index)
        record.bytes.append(static_cast<uint8_t>(number + index));
    record.few.append(number);
    for (uint32_t tag = 0; tag < 3; ++tag) {
        ElementWithList element;
        element.tag = tag;
        for (uint32_t word = 0; word < 2 * tag; ++word)
            element.words.append(number + 10 * tag + word);
        record.elements.append(WTF::move(element));
    }
    record.nothing.reserveCapacity(8);
    return record;
}

template<typename T>
static const uint8_t* addressOf(const T* pointer) { return reinterpret_cast<const uint8_t*>(pointer); }

static void testListGrowsInItselfThenOnHeap()
{
    ListOfThree list;
    AOT_TEST_CHECK(list.isEmpty() && list.capacity() == 3);
    for (uint32_t value = 0; value < 3; ++value)
        list.append(value);
    AOT_TEST_CHECK(!list.liesOnHeap() && !list.liesInBlock() && addressOf(list.begin()) >= addressOf(&list) && addressOf(list.end()) <= addressOf(&list + 1));

    ListOfThree movedWhileInItself = WTF::move(list);
    AOT_TEST_CHECK(list.isEmpty() && movedWhileInItself == ListOfThree({ 0, 1, 2 }));
    AOT_TEST_CHECK(addressOf(movedWhileInItself.begin()) >= addressOf(&movedWhileInItself) && addressOf(movedWhileInItself.end()) <= addressOf(&movedWhileInItself + 1));

    movedWhileInItself.append(movedWhileInItself[1]);
    AOT_TEST_CHECK(movedWhileInItself.liesOnHeap() && movedWhileInItself == ListOfThree({ 0, 1, 2, 1 }));

    const uint32_t* buffer = movedWhileInItself.begin();
    ListOfThree movedWhileOnHeap = WTF::move(movedWhileInItself);
    AOT_TEST_CHECK(movedWhileInItself.isEmpty() && !movedWhileInItself.liesOnHeap() && movedWhileOnHeap.begin() == buffer && movedWhileOnHeap.size() == 4);

    ListOfThree copy = movedWhileOnHeap;
    AOT_TEST_CHECK(copy == movedWhileOnHeap && copy.begin() != buffer);
    copy.removeAt(1);
    AOT_TEST_CHECK(copy == ListOfThree({ 0, 2, 1 }) && copy.takeLast() == 1 && copy.last() == 2);
    copy.insertFill(1, 7, 2);
    AOT_TEST_CHECK(copy == ListOfThree({ 0, 7, 7, 2 }) && copy.find(2u) == 3 && !copy.contains(1u));
}

static void testLargeListThatFillsItsMemoryStaysWhereItLies()
{
    RecordWithLists record = makeRecordWithLists(1, 0);
    record.bytes.grow(BlocksOfUnits::mostBytesBesideOthers + 1);
    record.bytes.last() = 0xee;
    const uint8_t* memory = record.bytes.begin();

    BlocksOfUnits blocks;
    RecordWithLists* kept = blocks.keep(WTF::move(record));
    AOT_TEST_CHECK(kept->bytes.begin() == memory && kept->bytes.liesInBlock() && !kept->bytes.liesOnHeap() && kept->bytes.last() == 0xee);
    AOT_TEST_CHECK(blocks.numberOfBlocks() == 2 && blocks.sizeInBytes() == BlocksOfUnits::sizeOfBlock + BlocksOfUnits::mostBytesBesideOthers + 1);
    AOT_TEST_CHECK(addressOf(kept->elements.begin()) == addressOf(kept + 1));
    kept->~RecordWithLists();
}

static void testListInBlockIsWrittenInPlaceAndOutgrowsIt()
{
    constexpr size_t numberOfBytes = 40;
    BlocksOfUnits blocks;
    RecordWithLists* kept = blocks.keep(makeRecordWithLists(9, numberOfBytes));
    const uint8_t* room = kept->bytes.begin();

    kept->bytes.shrink(30);
    kept->bytes.grow(34);
    kept->bytes[33] = 0xcc;
    AOT_TEST_CHECK(kept->bytes.liesInBlock() && kept->bytes.begin() == room && kept->bytes.size() == 34 && kept->bytes[29] == static_cast<uint8_t>(9 + 29));
    kept->bytes.removeAt(0);
    AOT_TEST_CHECK(kept->bytes.takeLast() == 0xcc && kept->bytes.liesInBlock() && kept->bytes.begin() == room && kept->bytes[0] == 10);
    kept->bytes.grow(numberOfBytes);
    AOT_TEST_CHECK(kept->bytes.liesInBlock() && kept->bytes.begin() == room);

    kept->bytes.append(0xdd);
    AOT_TEST_CHECK(kept->bytes.liesOnHeap() && kept->bytes.size() == numberOfBytes + 1 && kept->bytes[0] == 10 && kept->bytes.last() == 0xdd);

    ListOfTwo& words = kept->elements[2].words;
    AOT_TEST_CHECK(words.liesInBlock());
    words.append(1);
    AOT_TEST_CHECK(words.liesOnHeap() && words.size() == 5 && words[0] == 9 + 20 && words[4] == 1);

    kept->elements.clear();
    AOT_TEST_CHECK(kept->elements.isEmpty() && !kept->elements.liesInBlock());
    kept->~RecordWithLists();
}

static void testRarePartIsAbsentUntilEnsuredAndMovesIntoBlock()
{
    RecordWithRarePart without;
    without.bytes.append(1);
    AOT_TEST_CHECK(without.rarePart.isEmpty() && !without.rarePart.ifExists() && !without.rarePart.liesOnHeap() && !without.rarePart.liesInBlock());
    AOT_TEST_CHECK(without.rarePart->words.isEmpty() && without.rarePart->elements.isEmpty() && !without.rarePart->number && without.rarePart.isEmpty());

    RecordWithRarePart with;
    with.bytes.append(2);
    RarePartOfRecord& part = with.rarePart.ensure();
    part.number = 7;
    for (uint32_t word = 0; word < 5; ++word)
        part.words.append(word);
    ElementWithList element;
    for (uint32_t word = 0; word < 4; ++word)
        element.words.append(10 + word);
    part.elements.append(WTF::move(element));
    AOT_TEST_CHECK(with.rarePart.liesOnHeap() && &with.rarePart.ensure() == &part && with.rarePart.ifExists() == &part && &*with.rarePart == &part);

    RecordWithRarePart copy = with;
    AOT_TEST_CHECK(copy.rarePart.liesOnHeap() && copy.rarePart.ifExists() != &part && copy.rarePart->number == 7 && copy.rarePart->words == part.words && copy.rarePart->words.begin() != part.words.begin());
    copy = without;
    AOT_TEST_CHECK(copy.rarePart.isEmpty());

    BlocksOfUnits blocks;
    RecordWithRarePart* keptWithout = blocks.keep(WTF::move(without));
    RecordWithRarePart* kept = blocks.keep(WTF::move(with));
    AOT_TEST_CHECK(keptWithout->rarePart.isEmpty() && with.rarePart.isEmpty());
    AOT_TEST_CHECK(kept->rarePart.liesInBlock() && !kept->rarePart.liesOnHeap() && addressOf(kept->rarePart.ifExists()) > addressOf(kept) && addressOf(kept->rarePart.ifExists()) < addressOf(kept) + BlocksOfUnits::sizeOfBlock);
    AOT_TEST_CHECK(kept->rarePart->number == 7 && kept->rarePart->words.liesInBlock() && kept->rarePart->words == ListOfUnit<uint32_t>({ 0, 1, 2, 3, 4 }));
    AOT_TEST_CHECK(kept->rarePart->elements.liesInBlock() && kept->rarePart->elements[0].words.liesInBlock() && kept->rarePart->elements[0].words == ListOfTwo({ 10, 11, 12, 13 }));
    HeapFinder finder;
    forEachListOfUnitIn(*kept, finder);
    AOT_TEST_CHECK(!finder.numberOnHeap && finder.numberInBlocks == 5);

    RecordWithRarePart moved = WTF::move(*kept);
    AOT_TEST_CHECK(kept->rarePart.isEmpty() && moved.rarePart.liesInBlock() && moved.rarePart->number == 7);
    kept->~RecordWithRarePart();
    keptWithout->~RecordWithRarePart();
}

static constexpr ASCIILiteral programWhoseUnitsLeaveLists = R"IR(layout L0 kind=Instance name="Box" module=m0 constructor=f1
module m0 path="main.js" displayPath="main.js" body=f0 namespaceLayout=L(m0)
entry module=m0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = NewClosure function=f2
    DefineMethod v1, v2 layout=L0 key="size"
    v4 = FinishClass v1 layout=L0
    v5 = Construct v4
    v6 = PropGet v5 key="size"
    v7 = Call v6, v5 calleeKey="size"
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="sink" [isFreeName]
    v10 = Call v9, -, v7, v4 calleeName="sink"
    Return v10
}
function f1 name="Box" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="size" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    Return v0
}
)IR"_s;

static void testNoRecordOfStageOwnsMemory()
{
    for (unsigned numberOfThreads : { 1u, 2u, 16u }) {
        runOnBuildThread([&] {
            Wire::CompileOptions options = makeOptionsOfTestedProgram();
            options.threadCount = numberOfThreads;
            BuildContext build(options);
            std::unique_ptr<Typed::Program> parsed = Typed::parseProgram(Typed::withStandInLibrary(String(programWhoseUnitsLeaveLists)), build);
            AOT_TEST_CHECK(parsed);
            Typed::StagedBuild stages(WTF::move(parsed), build);
            const PreparedProgram* prepared = stages.prepared();
            AOT_TEST_CHECK(prepared);

            CompiledProgram compiled = compileQueuedUnits(*prepared->compileInputs, stages.program().types, build);
            AOT_TEST_CHECK(compiled.succeeded && compiled.functions.size() >= 2);

            HeapFinder finder;
            uint32_t placeOfPrevious = 0;
            for (size_t position = 0; position < compiled.functions.size(); ++position) {
                CompiledCode& code = compiled.functions[position];
                AOT_TEST_CHECK(!position || placeOfPrevious < code.placeInCode);
                placeOfPrevious = code.placeInCode;
                AOT_TEST_CHECK(compiled.functions.positionOfCodeAt(code.placeInCode) == std::optional<uint32_t>(static_cast<uint32_t>(position)));
                AOT_TEST_CHECK(code.body.bytes.liesInBlock() && code.body.bytes.begin() == addressOf(&code + 1));
                forEachListOfUnitIn(code, finder);
            }
            AOT_TEST_CHECK(!finder.numberOnHeap && finder.numberInBlocks >= compiled.functions.size());

            size_t numberOfBlocks = 0;
            for (const BlocksOfUnits& blocks : compiled.functions.blocks())
                numberOfBlocks += blocks.numberOfBlocks();
            AOT_TEST_CHECK(numberOfBlocks);

            compiled.functions.clear(build.parallelLoops());
            AOT_TEST_CHECK(compiled.functions.isEmpty() && compiled.functions.blocks().empty());
        });
    }
}

std::span<const NativeTest> listOfUnitTests()
{
    static constexpr NativeTest tests[] {
        { "list-of-unit/grows-in-itself-then-on-the-heap"_s, testListGrowsInItselfThenOnHeap },
        { "list-of-unit/large-list-that-fills-its-memory-stays-where-it-lies"_s, testLargeListThatFillsItsMemoryStaysWhereItLies },
        { "list-of-unit/list-in-a-block-is-written-in-place-and-outgrows-it"_s, testListInBlockIsWrittenInPlaceAndOutgrowsIt },
        { "list-of-unit/rare-part-is-absent-until-ensured-and-moves-into-the-block"_s, testRarePartIsAbsentUntilEnsuredAndMovesIntoBlock },
        { "list-of-unit/no-record-of-the-stage-owns-memory"_s, testNoRecordOfStageOwnsMemory },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
