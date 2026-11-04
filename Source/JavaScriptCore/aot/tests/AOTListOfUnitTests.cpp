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
        // No word, two words in the element, four words outside it.
        for (uint32_t word = 0; word < 2 * tag; ++word)
            element.words.append(number + 10 * tag + word);
        record.elements.append(WTF::move(element));
    }
    record.nothing.reserveCapacity(8);
    return record;
}

static void checkRecordWithLists(const RecordWithLists& record, uint32_t number, size_t numberOfBytes)
{
    AOT_TEST_CHECK(record.number == number && record.bytes.size() == numberOfBytes && record.few.size() == 1 && record.few[0] == number && record.elements.size() == 3 && record.nothing.isEmpty());
    for (size_t index = 0; index < numberOfBytes; ++index)
        AOT_TEST_CHECK(record.bytes[index] == static_cast<uint8_t>(number + index));
    for (uint32_t tag = 0; tag < 3; ++tag) {
        const ElementWithList& element = record.elements[tag];
        AOT_TEST_CHECK(element.tag == tag && element.words.size() == 2 * tag);
        for (uint32_t word = 0; word < 2 * tag; ++word)
            AOT_TEST_CHECK(element.words[word] == number + 10 * tag + word);
    }
}

template<typename T>
static const uint8_t* addressOf(const T* pointer) { return reinterpret_cast<const uint8_t*>(pointer); }

static const uint8_t* alignedFor(const uint8_t* address, size_t alignment)
{
    return reinterpret_cast<const uint8_t*>(WTF::roundUpToMultipleOf(alignment, reinterpret_cast<uintptr_t>(address)));
}

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

    // The value is an element of the list that has to move.
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

static void testListTakesBufferOfVector()
{
    Vector<uint32_t> longVector;
    for (uint32_t value = 0; value < 100; ++value)
        longVector.append(value);
    const uint32_t* buffer = longVector.span().data();
    ListOfThree taken = WTF::move(longVector);
    AOT_TEST_CHECK(longVector.isEmpty() && taken.begin() == buffer && taken.liesOnHeap() && taken.size() == 100 && taken[99] == 99);

    Vector<uint32_t> shortVector { 4, 5 };
    ListOfThree copiedIntoItself = WTF::move(shortVector);
    AOT_TEST_CHECK(!copiedIntoItself.liesOnHeap() && copiedIntoItself == Vector<uint32_t>({ 4, 5 }));

    Vector<uint32_t, 8> vectorThatHoldsItsElements { 1, 2, 3, 4, 5 };
    ListOfThree copiedToHeap = WTF::move(vectorThatHoldsItsElements);
    AOT_TEST_CHECK(copiedToHeap.liesOnHeap() && copiedToHeap == Vector<uint32_t>({ 1, 2, 3, 4, 5 }));

    Vector<ElementWithList> elements;
    for (uint32_t tag = 0; tag < 4; ++tag)
        elements.append(ElementWithList { { tag, tag, tag }, tag });
    ListOfUnit<ElementWithList> moved = WTF::move(elements);
    AOT_TEST_CHECK(elements.isEmpty() && moved.size() == 4 && moved[3].tag == 3 && moved[3].words == ListOfTwo({ 3, 3, 3 }));
}

static void testRecordAndItsListsAreOneStretch()
{
    constexpr size_t numberOfBytes = 13;
    BlocksOfUnits blocks;
    RecordWithLists* kept = blocks.keep(makeRecordWithLists(5, numberOfBytes));
    checkRecordWithLists(*kept, 5, numberOfBytes);

    HeapFinder finder;
    forEachListOfUnitIn(*kept, finder);
    // The bytes, the elements, and the four words of the last element. What fits in a list stays in it, and room that was reserved for nothing is let go.
    AOT_TEST_CHECK(finder.numberOfLists == 7 && !finder.numberOnHeap && finder.numberInBlocks == 3);
    AOT_TEST_CHECK(!kept->few.liesInBlock() && !kept->nothing.liesInBlock() && !kept->nothing.capacity() && !kept->elements[1].words.liesInBlock());

    AOT_TEST_CHECK(kept->bytes.begin() == addressOf(kept + 1));
    AOT_TEST_CHECK(addressOf(kept->elements.begin()) == alignedFor(kept->bytes.end(), alignof(ElementWithList)));
    AOT_TEST_CHECK(addressOf(kept->elements[2].words.begin()) == addressOf(kept->elements.end()));

    RecordWithLists* next = blocks.keep(makeRecordWithLists(6, numberOfBytes));
    AOT_TEST_CHECK(addressOf(next) == alignedFor(addressOf(kept->elements[2].words.end()), alignof(uint64_t)));
    checkRecordWithLists(*kept, 5, numberOfBytes);
    checkRecordWithLists(*next, 6, numberOfBytes);
    next->~RecordWithLists();
    kept->~RecordWithLists();
}

static void testHundredsOfRecordsShareOneBlock()
{
    constexpr uint32_t numberOfRecords = 1000;
    BlocksOfUnits blocks;
    Vector<RecordWithLists*> records;
    for (uint32_t number = 0; number < numberOfRecords; ++number)
        records.append(blocks.keep(makeRecordWithLists(number, number % 200)));
    AOT_TEST_CHECK(blocks.numberOfBlocks() == 1 && blocks.sizeInBytes() == BlocksOfUnits::sizeOfBlock);

    BlocksOfUnits movedBlocks = WTF::move(blocks);
    AOT_TEST_CHECK(!blocks.numberOfBlocks() && movedBlocks.numberOfBlocks() == 1);
    for (uint32_t number = 0; number < numberOfRecords; ++number) {
        checkRecordWithLists(*records[number], number, number % 200);
        records[number]->~RecordWithLists();
    }
}

static void testLargeRecordHasBlockOfItsOwn()
{
    constexpr size_t numberOfBytesOfLargeRecord = BlocksOfUnits::mostBytesBesideOthers + 1;
    BlocksOfUnits blocks;
    RecordWithLists* first = blocks.keep(makeRecordWithLists(1, 8));
    RecordWithLists* large = blocks.keep(makeRecordWithLists(2, numberOfBytesOfLargeRecord));
    RecordWithLists* second = blocks.keep(makeRecordWithLists(3, 8));
    AOT_TEST_CHECK(blocks.numberOfBlocks() == 2 && blocks.sizeInBytes() < BlocksOfUnits::sizeOfBlock + numberOfBytesOfLargeRecord + KB);
    // The block that the small ones share goes on behind the first.
    AOT_TEST_CHECK(addressOf(second) == alignedFor(addressOf(first->elements[2].words.end()), alignof(uint64_t)));
    checkRecordWithLists(*first, 1, 8);
    checkRecordWithLists(*large, 2, numberOfBytesOfLargeRecord);
    checkRecordWithLists(*second, 3, 8);

    // A block that is full is followed by another.
    Vector<RecordWithLists*> records { first, large, second };
    while (blocks.numberOfBlocks() == 2)
        records.append(blocks.keep(makeRecordWithLists(4, BlocksOfUnits::mostBytesBesideOthers / 2)));
    AOT_TEST_CHECK(blocks.numberOfBlocks() == 3 && records.size() >= 10);
    checkRecordWithLists(*records.last(), 4, BlocksOfUnits::mostBytesBesideOthers / 2);
    for (RecordWithLists* record : records)
        record->~RecordWithLists();
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
    // The rest lies behind the record as ever.
    AOT_TEST_CHECK(addressOf(kept->elements.begin()) == addressOf(kept + 1));
    kept->~RecordWithLists();
}

static void testListInBlockIsWrittenInPlaceAndOutgrowsIt()
{
    constexpr size_t numberOfBytes = 40;
    BlocksOfUnits blocks;
    RecordWithLists* kept = blocks.keep(makeRecordWithLists(9, numberOfBytes));
    const uint8_t* room = kept->bytes.begin();

    // What removeFinalJumps() does to the bytes of a unit.
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

    // A list of an element in a block, too.
    ListOfTwo& words = kept->elements[2].words;
    AOT_TEST_CHECK(words.liesInBlock());
    words.append(1);
    AOT_TEST_CHECK(words.liesOnHeap() && words.size() == 5 && words[0] == 9 + 20 && words[4] == 1);

    kept->elements.clear();
    AOT_TEST_CHECK(kept->elements.isEmpty() && !kept->elements.liesInBlock());
    kept->~RecordWithLists();
}

static void testHeapThatFitsGoesBackIntoList()
{
    RecordWithLists record;
    for (uint64_t value = 0; value < 6; ++value)
        record.few.append(value);
    record.few.shrink(1);
    AOT_TEST_CHECK(record.few.liesOnHeap());

    BlocksOfUnits blocks;
    RecordWithLists* kept = blocks.keep(WTF::move(record));
    AOT_TEST_CHECK(!kept->few.liesOnHeap() && !kept->few.liesInBlock() && kept->few.size() == 1 && !kept->few[0]);
    kept->~RecordWithLists();
}

// class Box { size() { return 1 } }  sink(new Box().size(), Box)
// The class object is handed on: a class that nobody asks for is not made, and then its constructor and its members have no code.
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
                // The bytes lie right behind the record.
                AOT_TEST_CHECK(code.body.bytes.liesInBlock() && code.body.bytes.begin() == addressOf(&code + 1));
                AOT_TEST_CHECK(code.calleesOfIndirectCalls.lists.isEmpty() && !code.calleesOfIndirectCalls.lists.capacity() && !code.calleesOfIndirectCalls.functions.capacity());
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
        { "list-of-unit/takes-the-buffer-of-a-vector"_s, testListTakesBufferOfVector },
        { "list-of-unit/record-and-its-lists-are-one-stretch"_s, testRecordAndItsListsAreOneStretch },
        { "list-of-unit/hundreds-of-records-share-one-block"_s, testHundredsOfRecordsShareOneBlock },
        { "list-of-unit/large-record-has-a-block-of-its-own"_s, testLargeRecordHasBlockOfItsOwn },
        { "list-of-unit/large-list-that-fills-its-memory-stays-where-it-lies"_s, testLargeListThatFillsItsMemoryStaysWhereItLies },
        { "list-of-unit/list-in-a-block-is-written-in-place-and-outgrows-it"_s, testListInBlockIsWrittenInPlaceAndOutgrowsIt },
        { "list-of-unit/heap-that-fits-goes-back-into-the-list"_s, testHeapThatFitsGoesBackIntoList },
        { "list-of-unit/no-record-of-the-stage-owns-memory"_s, testNoRecordOfStageOwnsMemory },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
