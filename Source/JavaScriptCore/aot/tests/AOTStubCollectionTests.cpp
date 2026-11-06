/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTOperationData.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubLeafConvention.h"
#include "AOTStubRunner.h"
#include "AOTStubsCollections.h"
#include "AOTTableInImage.h"
#include "AOTVector.h"
#include "CompleteSubspace.h"
#include "HashMapHelper.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSString.h"
#include "MarkedBlock.h"
#include "MarkedBlockCage.h"
#include "MarkedSpace.h"
#include "PreciseAllocation.h"
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Width = CompactTable::Width;
using SlotKind = CompactTable::SlotKind;

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }
static unsigned hashOfBits(uint64_t bits) { return rapidHashMix64(bits); }

namespace {

struct FakeCell {
    uint32_t structureID;
    uint8_t indexingTypeAndMisc;
    uint8_t type;
    uint8_t flags;
    uint8_t cellState;
    uint64_t payload;
};
static_assert(sizeof(FakeCell) == MarkedBlock::atomSize && OBJECT_OFFSETOF(FakeCell, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeCell, cellState) == JSCell::cellStateOffset());
static_assert(OBJECT_OFFSETOF(FakeCell, payload) == JSString::offsetOfValue());

class FakeHeap {
    WTF_MAKE_NONCOPYABLE(FakeHeap);
public:
    FakeHeap()
    {
        constexpr uintptr_t firstOffsetInCage = static_cast<uintptr_t>(minCell32) << cell32Shift;
        m_memory.grow(2 * usableBytes + firstOffsetInCage + MarkedBlock::atomSize);
        uintptr_t start = WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(std::bit_cast<uintptr_t>(m_memory.mutableSpan().data()));
        uintptr_t offsetInCage = start - MarkedBlockCage::baseOf(std::bit_cast<void*>(start));
        if (offsetInCage < firstOffsetInCage)
            start += firstOffsetInCage - offsetInCage;
        else if (offsetInCage + usableBytes > MarkedBlockCage::maxSize)
            start += MarkedBlockCage::maxSize - offsetInCage + firstOffsetInCage;
        m_next = start;
        m_end = start + usableBytes;
        AOT_TEST_CHECK(m_end <= std::bit_cast<uintptr_t>(m_memory.mutableSpan().data()) + m_memory.size());
        AOT_TEST_CHECK(MarkedBlockCage::baseOf(std::bit_cast<void*>(m_next)) == MarkedBlockCage::baseOf(std::bit_cast<void*>(m_end - 1)));
    }

    uintptr_t cageBase() const { return MarkedBlockCage::baseOf(std::bit_cast<void*>(m_next)); }

    uint8_t* allocate(size_t size)
    {
        uintptr_t result = m_next;
        m_next += WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(size);
        AOT_TEST_CHECK(m_next <= m_end);
        zeroSpan(std::span(std::bit_cast<uint8_t*>(result), size));
        return std::bit_cast<uint8_t*>(result);
    }

    uint64_t createCell(JSType type, uint64_t payload = 0)
    {
        auto* cell = reinterpret_cast<FakeCell*>(allocate(sizeof(FakeCell)));
        cell->type = type;
        cell->cellState = static_cast<uint8_t>(CellState::DefinitelyWhite);
        cell->payload = payload;
        return std::bit_cast<uintptr_t>(cell);
    }

    uint64_t createString(const String& text)
    {
        m_texts.append(text);
        return createCell(StringType, std::bit_cast<uintptr_t>(text.impl()));
    }

    uint64_t createStringWithoutHash(const String& text)
    {
        String copy = text.isolatedCopy();
        AOT_TEST_CHECK(!copy.impl()->hasHash());
        return createString(copy);
    }

    uint64_t createRope() { return createCell(StringType, JSString::isRopeInPointer); }

private:
    static constexpr size_t usableBytes = MB;

    Vector<uint8_t> m_memory;
    Vector<String> m_texts;
    uintptr_t m_next { 0 };
    uintptr_t m_end { 0 };
};

class FakeTable {
public:
    FakeTable(FakeHeap& heap, uint32_t capacity, Width keyWidth, Width valueWidth, uint8_t* bytesOfCreatedTable = nullptr)
        : m_cageBase(heap.cageBase())
        , m_capacity(capacity)
        , m_indexBits(CompactTable::indexBitsFor(capacity))
        , m_keyWidth(keyWidth)
        , m_valueWidth(valueWidth)
    {
        uint8_t keysShift = CompactTable::keysShiftFor(m_indexBits, capacity);
        size_t sizeOfIndex = static_cast<size_t>(2) << keysShift;
        m_offsetOfKeys = CompactTable::offsetOfIndex() + sizeOfIndex;
        m_offsetOfValues = m_offsetOfKeys + static_cast<size_t>(capacity) * static_cast<size_t>(keyWidth);
        m_size = m_offsetOfValues + static_cast<size_t>(capacity) * static_cast<size_t>(valueWidth);
        if (bytesOfCreatedTable) {
            m_bytes = bytesOfCreatedTable;
            return;
        }
        m_bytes = heap.allocate(m_size);

        auto* cell = reinterpret_cast<FakeCell*>(m_bytes);
        cell->type = CellType;
        cell->cellState = static_cast<uint8_t>(CellState::DefinitelyWhite);
        at<uint32_t>(CompactTable::offsetOfCapacity()) = capacity;
        at<uint8_t>(CompactTable::offsetOfIndexBits()) = CompactTable::markedIndexBits(m_indexBits);
        at<uint8_t>(CompactTable::offsetOfKeysShift()) = keysShift | (keyWidth == Width::Wide ? CompactTable::keysAreWide : 0);
        at<uint8_t>(CompactTable::offsetOfValueWidth()) = static_cast<uint8_t>(valueWidth);
        at<uint8_t>(CompactTable::offsetOfStatus()) = static_cast<uint8_t>(CompactTable::Status::Live);
        if (m_indexBits)
            zeroSpan(std::span(index(), sizeOfIndex));
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(m_bytes); }
    SlotKind slotKind() const { return CompactTable::slotKindFor(m_capacity); }
    uint32_t used() const { return at<uint32_t>(CompactTable::offsetOfUsed()); }
    uint32_t alive() const { return at<uint32_t>(CompactTable::offsetOfAliveCount()); }
    uint64_t header() const { return at<uint64_t>(0); }
    uint64_t capacityAndKinds() const { return at<uint64_t>(CompactTable::offsetOfCapacity()); }
    size_t size() const { return m_size; }
    void markAsWalked() { at<uint8_t>(CompactTable::offsetOfKeysShift()) |= CompactTable::wasWalked; }
    void deleteLastEntry()
    {
        if (m_keyWidth == Width::Wide)
            at<uint64_t>(m_offsetOfKeys + (used() - 1) * sizeof(uint64_t)) = 0;
        else
            at<uint32_t>(m_offsetOfKeys + (used() - 1) * sizeof(uint32_t)) = 0;
        at<uint32_t>(CompactTable::offsetOfAliveCount())--;
    }
    Vector<uint8_t> copyOfBytes() const { return Vector<uint8_t>(std::span<const uint8_t>(m_bytes, m_size)); }
    // A search for a place to insert marks the home slot that it leaves, whoever inserts the key in the end. The mark says that a key may lie elsewhere, and nothing of the entries.
    Vector<uint8_t> copyOfBytesWithoutMarksOfHomeSlots() const
    {
        Vector<uint8_t> bytes = copyOfBytes();
        if (m_indexBits) {
            size_t offsetOfTags = index() - m_bytes;
            for (uint8_t& tag : bytes.mutableSpan().subspan(offsetOfTags, static_cast<size_t>(1) << m_indexBits))
                tag &= ~CompactTable::keyLeftHomeSlot;
        }
        return bytes;
    }
    void setCellState(CellState state) { reinterpret_cast<FakeCell*>(m_bytes)->cellState = static_cast<uint8_t>(state); }

    void append(uint64_t key, unsigned hash, uint64_t value)
    {
        uint32_t entry = used();
        AOT_TEST_CHECK(entry < m_capacity && hash <= CompactTable::hashMask);
        store(m_offsetOfKeys, m_keyWidth, entry, key);
        if (m_valueWidth != Width::None)
            store(m_offsetOfValues, m_valueWidth, entry, value);
        CompactTable::insertIntoIndex(slotKind(), index(), m_indexBits, entry, hash);
        at<uint32_t>(CompactTable::offsetOfUsed()) = entry + 1;
        at<uint32_t>(CompactTable::offsetOfAliveCount()) = entry + 1;
        AOT_TEST_CHECK(find(key, hash) == entry);
    }

    std::optional<uint32_t> find(uint64_t key, unsigned hash) const
    {
        return CompactTable::probeIndex(slotKind(), index(), m_indexBits, used(), hash, [&](uint32_t entry) {
            return load(m_offsetOfKeys, m_keyWidth, entry) == key;
        });
    }

    uint64_t keyAt(uint32_t entry) const { return load(m_offsetOfKeys, m_keyWidth, entry); }
    uint64_t valueAt(uint32_t entry) const { return load(m_offsetOfValues, m_valueWidth, entry); }

private:
    template<typename T> T& at(size_t offset) const { return *reinterpret_cast<T*>(m_bytes + offset); }
    uint8_t* index() const { return m_bytes + CompactTable::offsetOfIndex(); }

    void store(size_t offsetOfColumn, Width width, uint32_t entry, uint64_t value)
    {
        if (width == Width::Wide) {
            at<uint64_t>(offsetOfColumn + entry * sizeof(uint64_t)) = value;
            return;
        }
        bool isCell = JSValue::decode(static_cast<EncodedJSValue>(value)).isCell();
        AOT_TEST_CHECK(width == Width::Narrow && (isCell || value < minCell32));
        at<uint32_t>(offsetOfColumn + entry * sizeof(uint32_t)) = isCell ? MarkedBlockCage::encode(m_cageBase, std::bit_cast<void*>(static_cast<uintptr_t>(value))) : static_cast<uint32_t>(value);
    }

    uint64_t load(size_t offsetOfColumn, Width width, uint32_t entry) const
    {
        if (width == Width::Wide)
            return at<uint64_t>(offsetOfColumn + entry * sizeof(uint64_t));
        uint32_t word = at<uint32_t>(offsetOfColumn + entry * sizeof(uint32_t));
        return word < minCell32 ? word : std::bit_cast<uintptr_t>(MarkedBlockCage::decode(m_cageBase, word));
    }

    uintptr_t m_cageBase;
    uint32_t m_capacity;
    unsigned m_indexBits;
    Width m_keyWidth;
    Width m_valueWidth;
    size_t m_offsetOfKeys { 0 };
    size_t m_offsetOfValues { 0 };
    size_t m_size { 0 };
    uint8_t* m_bytes { nullptr };
};

struct TableSize {
    unsigned numberOfKeys;
    uint32_t capacity;
    SlotKind slotKind;
};

struct Collections {
    Collections()
    {
        runner.instance().cellCageBase = heap.cageBase();
        runner.instance().allocation.cellSpace = m_cellSpace.bytes.data();
        runner.instance().allocation.compactTableHeader = headerOfCreatedTables;
        runner.install(Entry::operationAOTStringImplsAreEqual, operationAOTStringImplsAreEqual);
        runner.install(Entry::operationAOTHashStringImpl, operationAOTHashStringImpl);
    }

    void setAllocator(size_t cellSize, FakeAllocator* allocator)
    {
        size_t index = MarkedSpace::sizeClassToIndex(cellSize);
        AOT_TEST_CHECK(index < numberOfSizeSteps);
        m_cellSpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + index * sizeof(void*)) = allocator;
    }

    uint64_t createOwner(JSType type, uint64_t storage)
    {
        uint8_t* owner = heap.allocate(sizeof(JSMap));
        auto* cell = reinterpret_cast<FakeCell*>(owner);
        cell->type = type;
        cell->cellState = static_cast<uint8_t>(CellState::DefinitelyWhite);
        *reinterpret_cast<uint64_t*>(owner + JSMap::offsetOfStorage()) = storage;
        return std::bit_cast<uintptr_t>(owner);
    }

    static constexpr uint64_t headerOfCreatedTables = static_cast<uint64_t>(CellState::DefinitelyWhite) << (8 * JSCell::cellStateOffset()) | static_cast<uint64_t>(CellType) << (8 * JSCell::typeInfoTypeOffset()) | 0x2040;

    StubRunner runner;
    FakeHeap heap;

private:
    static constexpr size_t numberOfSizeSteps = 128;

    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + numberOfSizeSteps * sizeof(void*)> m_cellSpace;
};

struct StringKey {
    String text;
    uint64_t cell;
    uint64_t value;
};

struct TableEntry {
    uint64_t key;
    unsigned hash;
    std::optional<uint64_t> value;
};

} // anonymous namespace

static_assert(sizeof(JSMap) == sizeof(JSSet));

static constexpr TableSize tableSizes[] = {
    { 3, 4, SlotKind::Tag },
    { 8, 8, SlotKind::Tag },
    { 100, 128, SlotKind::Short },
    { 300, 320, SlotKind::Long },
};

static constexpr std::array comparisonsOfStrings { KeyComparison::AnyValue, KeyComparison::String };
static constexpr std::array comparisonsOfBits { KeyComparison::AnyValue, KeyComparison::Bits };

static String createHashedCopy(const String& text, bool is16Bit)
{
    String copy = text.isolatedCopy();
    if (is16Bit)
        copy.convertTo16Bit();
    AOT_TEST_CHECK(copy.impl() != text.impl() && copy.is8Bit() == !is16Bit);
    AOT_TEST_CHECK(copy.impl()->hash() == text.impl()->hash());
    return copy;
}

static Vector<StringKey> appendStringKeys(Collections& collections, FakeTable& table, unsigned numberOfKeys, bool valuesAreCells)
{
    Vector<StringKey> keys;
    for (unsigned i = 0; i < numberOfKeys; ++i) {
        String text = makeString("key"_s, i);
        uint64_t cell = collections.heap.createString(text);
        uint64_t value = valuesAreCells ? collections.heap.createCell(FinalObjectType) : bitsOf(jsNumber(1000 + i));
        table.append(cell, text.impl()->hash(), value);
        keys.append({ text, cell, value });
    }
    return keys;
}

static void checkAnswer(Collections& collections, StubKey stub, uint64_t owner, uint64_t key, uint64_t expected)
{
    StubRun run = collections.runner.run(stub, { owner, key });
    AOT_TEST_CHECK(!run.calledOperation && run.result == expected);
}

static void checkLeftToOperation(Collections& collections, StubKey stub, Entry operation, uint64_t owner, uint64_t key)
{
    constexpr uint64_t answerOfOperation = 0x1234;
    collections.runner.recordCallsOf(operation);
    collections.runner.setResultOfRecordedOperation(answerOfOperation);
    StubRun run = collections.runner.run(stub, { owner, key });
    collections.runner.countCallsOf(operation);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == owner && run.recordedArguments[1] == key);
    AOT_TEST_CHECK(run.result == answerOfOperation);
}

static void checkStringLookups(Collections& collections, uint64_t map, uint64_t set, const StringKey& key)
{
    FakeHeap& heap = collections.heap;
    const uint64_t cellsWithSameText[] = {
        key.cell,
        heap.createString(key.text),
        heap.createString(createHashedCopy(key.text, false)),
        heap.createString(createHashedCopy(key.text, true)),
    };
    for (KeyComparison comparison : comparisonsOfStrings) {
        for (uint64_t cell : cellsWithSameText) {
            checkAnswer(collections, mapGet(comparison), map, cell, key.value);
            checkAnswer(collections, mapHas(comparison), map, cell, bitsOf(jsBoolean(true)));
            checkAnswer(collections, setHas(comparison), set, cell, bitsOf(jsBoolean(true)));
            checkAnswer(collections, setAdd(comparison), set, cell, set);
        }
    }
}

static void checkAbsentStrings(Collections& collections, uint64_t map, uint64_t set)
{
    FakeHeap& heap = collections.heap;
    String absentText = "absent"_s;
    absentText.impl()->hash();
    uint64_t absent = heap.createString(absentText);
    uint64_t rope = heap.createRope();

    for (KeyComparison comparison : comparisonsOfStrings) {
        checkAnswer(collections, mapGet(comparison), map, absent, bitsOf(jsUndefined()));
        checkAnswer(collections, mapHas(comparison), map, absent, bitsOf(jsBoolean(false)));
        checkAnswer(collections, setHas(comparison), set, absent, bitsOf(jsBoolean(false)));
        checkAnswer(collections, mapGet(comparison), map, heap.createStringWithoutHash(absentText), bitsOf(jsUndefined()));
        checkAnswer(collections, mapHas(comparison), map, heap.createStringWithoutHash(absentText), bitsOf(jsBoolean(false)));
        checkAnswer(collections, setHas(comparison), set, heap.createStringWithoutHash(absentText), bitsOf(jsBoolean(false)));
        checkLeftToOperation(collections, mapGet(comparison), Entry::operationAOTMapGet, map, rope);
        checkLeftToOperation(collections, mapHas(comparison), Entry::operationAOTMapHas, map, rope);
        checkLeftToOperation(collections, setHas(comparison), Entry::operationAOTSetHas, set, rope);
        checkLeftToOperation(collections, setAdd(comparison), Entry::operationAOTSetAdd, set, rope);
    }
}

static void checkStringsWithoutHash(Collections& collections, uint64_t map, uint64_t set, const StringKey& key)
{
    FakeHeap& heap = collections.heap;
    for (KeyComparison comparison : comparisonsOfStrings) {
        checkAnswer(collections, mapGet(comparison), map, heap.createStringWithoutHash(key.text), key.value);
        checkAnswer(collections, mapHas(comparison), map, heap.createStringWithoutHash(key.text), bitsOf(jsBoolean(true)));
        checkAnswer(collections, setHas(comparison), set, heap.createStringWithoutHash(key.text), bitsOf(jsBoolean(true)));
        checkAnswer(collections, setAdd(comparison), set, heap.createStringWithoutHash(key.text), set);
    }
}

static void testStringKeys()
{
    Collections collections;
    for (const TableSize& size : tableSizes) {
        for (bool valuesAreCells : { true, false }) {
            FakeTable mapTable(collections.heap, size.capacity, Width::Narrow, valuesAreCells ? Width::Narrow : Width::Wide);
            FakeTable setTable(collections.heap, size.capacity, Width::Narrow, Width::None);
            AOT_TEST_CHECK(mapTable.slotKind() == size.slotKind && setTable.slotKind() == size.slotKind);
            Vector<StringKey> keys = appendStringKeys(collections, mapTable, size.numberOfKeys, valuesAreCells);
            for (const StringKey& key : keys)
                setTable.append(key.cell, key.text.impl()->hash(), 0);

            uint64_t map = collections.createOwner(JSMapType, mapTable.address());
            uint64_t set = collections.createOwner(JSSetType, setTable.address());
            for (const StringKey& key : keys)
                checkStringLookups(collections, map, set, key);
            checkStringsWithoutHash(collections, map, set, keys.first());
            checkStringsWithoutHash(collections, map, set, keys.last());
            checkAbsentStrings(collections, map, set);
        }
    }
}

// On its way to each string the search comes by a key of another kind, or by the place of a deleted key, that has the hash of the string.
static void testStringsBesideOtherKeys()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    for (const TableSize& size : tableSizes) {
        for (Width keyWidth : { Width::Narrow, Width::Wide }) {
            FakeTable mapTable(heap, size.capacity, keyWidth, Width::Wide);
            FakeTable setTable(heap, size.capacity, keyWidth, Width::None);
            Vector<StringKey> keys;
            for (unsigned i = 0; 2 * i + 1 < size.numberOfKeys; ++i) {
                String text = makeString("key"_s, i);
                unsigned hash = text.impl()->hash();
                uint64_t other = !i ? bitsOf(jsUndefined()) : i == 1 ? bitsOf(jsBoolean(true)) : keyWidth == Width::Wide && i % 2 ? bitsOf(jsNumber(i)) : heap.createCell(FinalObjectType);
                mapTable.append(other, hash, bitsOf(jsNull()));
                setTable.append(other, hash, 0);
                if (i % 4 == 2) {
                    mapTable.deleteLastEntry();
                    setTable.deleteLastEntry();
                }
                uint64_t cell = heap.createString(text);
                uint64_t value = bitsOf(jsNumber(1000 + i));
                mapTable.append(cell, hash, value);
                setTable.append(cell, hash, 0);
                keys.append({ text, cell, value });
            }

            uint64_t map = collections.createOwner(JSMapType, mapTable.address());
            uint64_t set = collections.createOwner(JSSetType, setTable.address());
            for (const StringKey& key : keys)
                checkStringLookups(collections, map, set, key);
            checkStringsWithoutHash(collections, map, set, keys.first());
            checkStringsWithoutHash(collections, map, set, keys.last());
            checkAbsentStrings(collections, map, set);
        }
    }
}

static Vector<uint64_t> createKeysComparedByBits(FakeHeap& heap, unsigned numberOfKeys, bool hasNumbers)
{
    Vector<uint64_t> keys { bitsOf(jsUndefined()), bitsOf(jsBoolean(true)), bitsOf(jsNull()) };
    while (keys.size() < numberOfKeys) {
        switch (keys.size() % 3) {
        case 0:
            keys.append(heap.createCell(FinalObjectType));
            break;
        case 1:
            keys.append(heap.createCell(SymbolType));
            break;
        case 2:
            keys.append(hasNumbers ? bitsOf(jsNumber(static_cast<int32_t>(keys.size()) - 50)) : heap.createCell(JSFunctionType));
            break;
        }
    }
    return keys;
}

static void checkKeysComparedByBits(Collections& collections, const TableSize& size, Width keyWidth)
{
    FakeHeap& heap = collections.heap;
    FakeTable mapTable(heap, size.capacity, keyWidth, Width::Wide);
    FakeTable setTable(heap, size.capacity, keyWidth, Width::None);
    Vector<uint64_t> keys = createKeysComparedByBits(heap, size.numberOfKeys, keyWidth == Width::Wide);
    for (size_t i = 0; i < keys.size(); ++i) {
        unsigned hash = hashOfBits(keys[i]) & CompactTable::hashMask;
        mapTable.append(keys[i], hash, bitsOf(jsNumber(static_cast<int32_t>(i))));
        setTable.append(keys[i], hash, 0);
    }
    uint64_t map = collections.createOwner(JSMapType, mapTable.address());
    uint64_t set = collections.createOwner(JSSetType, setTable.address());
    const uint64_t absentKeys[] = { heap.createCell(FinalObjectType), heap.createCell(SymbolType), bitsOf(jsBoolean(false)), bitsOf(jsNumber(123456)) };

    for (KeyComparison comparison : comparisonsOfBits) {
        for (size_t i = 0; i < keys.size(); ++i) {
            checkAnswer(collections, mapGet(comparison), map, keys[i], bitsOf(jsNumber(static_cast<int32_t>(i))));
            checkAnswer(collections, mapHas(comparison), map, keys[i], bitsOf(jsBoolean(true)));
            checkAnswer(collections, setHas(comparison), set, keys[i], bitsOf(jsBoolean(true)));
            checkAnswer(collections, setAdd(comparison), set, keys[i], set);
        }
        for (uint64_t absent : absentKeys) {
            checkAnswer(collections, mapGet(comparison), map, absent, bitsOf(jsUndefined()));
            checkAnswer(collections, mapHas(comparison), map, absent, bitsOf(jsBoolean(false)));
            checkAnswer(collections, setHas(comparison), set, absent, bitsOf(jsBoolean(false)));
        }
    }
    for (uint64_t immediate : { bitsOf(jsUndefined()), bitsOf(jsNull()) })
        checkAnswer(collections, mapHas(KeyComparison::String), map, immediate, bitsOf(jsBoolean(true)));
}

static void testKeysComparedByBits()
{
    Collections collections;
    for (const TableSize& size : tableSizes) {
        checkKeysComparedByBits(collections, size, Width::Narrow);
        checkKeysComparedByBits(collections, size, Width::Wide);
    }
}

static void testKeysLeftToOperations()
{
    Collections collections;
    FakeTable table(collections.heap, 4, Width::Narrow, Width::Wide);
    uint64_t map = collections.createOwner(JSMapType, table.address());
    for (uint64_t key : { bitsOf(jsNumber(0.5)), bitsOf(jsNaN()), collections.heap.createCell(HeapBigIntType) }) {
        checkLeftToOperation(collections, mapGet(KeyComparison::AnyValue), Entry::operationAOTMapGet, map, key);
        checkLeftToOperation(collections, mapHas(KeyComparison::AnyValue), Entry::operationAOTMapHas, map, key);
    }
}

static void testOwnersWithoutStorage()
{
    Collections collections;
    uint64_t map = collections.createOwner(JSMapType, 0);
    uint64_t set = collections.createOwner(JSSetType, 0);
    String text = "key"_s;
    text.impl()->hash();
    uint64_t string = collections.heap.createString(text);
    uint64_t object = collections.heap.createCell(FinalObjectType);

    for (KeyComparison comparison : { KeyComparison::AnyValue, KeyComparison::String, KeyComparison::Bits }) {
        uint64_t key = comparison == KeyComparison::Bits ? object : string;
        checkAnswer(collections, mapGet(comparison), map, key, bitsOf(jsUndefined()));
        checkAnswer(collections, mapHas(comparison), map, key, bitsOf(jsBoolean(false)));
        checkAnswer(collections, setHas(comparison), set, key, bitsOf(jsBoolean(false)));
    }
    checkAnswer(collections, mapGet(KeyComparison::AnyValue), map, collections.heap.createRope(), bitsOf(jsUndefined()));
    String textWithoutHash = text.isolatedCopy();
    checkAnswer(collections, mapGet(KeyComparison::AnyValue), map, collections.heap.createString(textWithoutHash), bitsOf(jsUndefined()));
    AOT_TEST_CHECK(!textWithoutHash.impl()->hasHash());

    for (KeyComparison comparison : comparisonsOfBits)
        checkLeftToOperation(collections, setAdd(comparison), Entry::operationAOTSetAdd, set, object);
    for (KeyComparison comparison : comparisonsOfStrings)
        checkLeftToOperation(collections, setAdd(comparison), Entry::operationAOTSetAdd, set, string);
}

static void checkStoredValue(Collections& collections, StubKey stub, uint64_t map, FakeTable& table, uint64_t key, unsigned hash, uint64_t value)
{
    StubRun run = collections.runner.run(stub, { map, key, value });
    AOT_TEST_CHECK(!run.calledOperation && run.result == map);
    std::optional<uint32_t> entry = table.find(key, hash);
    AOT_TEST_CHECK(entry && table.valueAt(*entry) == value);
}

static void checkStoreLeftToOperation(Collections& collections, StubKey stub, uint64_t map, uint64_t key, uint64_t value)
{
    constexpr Entry operation = Entry::operationAOTMapSet;
    collections.runner.recordCallsOf(operation);
    collections.runner.setResultOfRecordedOperation(map);
    StubRun run = collections.runner.run(stub, { map, key, value });
    collections.runner.countCallsOf(operation);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == map);
    AOT_TEST_CHECK(run.recordedArguments[0] == map && run.recordedArguments[1] == key && run.recordedArguments[2] == value);
}

static void checkMapSet(Collections& collections, const TableSize& size, Width valueWidth)
{
    FakeHeap& heap = collections.heap;
    FakeTable table(heap, size.capacity, Width::Narrow, valueWidth);
    Vector<StringKey> keys = appendStringKeys(collections, table, size.numberOfKeys - 1, valueWidth == Width::Narrow);
    uint64_t object = heap.createCell(FinalObjectType);
    unsigned hashOfObject = hashOfBits(object) & CompactTable::hashMask;
    table.append(object, hashOfObject, bitsOf(jsUndefined()));
    uint64_t map = collections.createOwner(JSMapType, table.address());
    const uint64_t number = bitsOf(jsNumber(42));

    for (uint64_t value : { heap.createCell(FinalObjectType), bitsOf(jsUndefined()), bitsOf(jsBoolean(true)), bitsOf(jsNull()) }) {
        for (KeyComparison comparison : comparisonsOfStrings)
            checkStoredValue(collections, mapSet(comparison), map, table, keys.last().cell, keys.last().text.impl()->hash(), value);
        for (KeyComparison comparison : comparisonsOfBits)
            checkStoredValue(collections, mapSet(comparison), map, table, object, hashOfObject, value);
    }
    if (valueWidth == Width::Wide)
        checkStoredValue(collections, mapSet(KeyComparison::Bits), map, table, object, hashOfObject, number);
    else {
        checkStoreLeftToOperation(collections, mapSet(KeyComparison::Bits), map, object, number);
        AOT_TEST_CHECK(table.valueAt(*table.find(object, hashOfObject)) == bitsOf(jsNull()));
    }

}

static void testMapSet()
{
    Collections collections;
    for (const TableSize& size : tableSizes) {
        checkMapSet(collections, size, Width::Narrow);
        checkMapSet(collections, size, Width::Wide);
    }
}

static void testMapSetBarrier()
{
    Collections collections;
    FakeTable table(collections.heap, 4, Width::Narrow, Width::Narrow);
    uint64_t key = collections.heap.createCell(FinalObjectType);
    unsigned hash = hashOfBits(key) & CompactTable::hashMask;
    table.append(key, hash, bitsOf(jsUndefined()));
    uint64_t map = collections.createOwner(JSMapType, table.address());
    uint64_t value = collections.heap.createCell(FinalObjectType);

    checkStoredValue(collections, mapSet(KeyComparison::Bits), map, table, key, hash, value);

    table.setCellState(CellState::PossiblyBlack);
    collections.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
    StubRun run = collections.runner.run(mapSet(KeyComparison::Bits), { map, key, bitsOf(jsNull()) });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == table.address() && run.result == map);
    AOT_TEST_CHECK(table.valueAt(0) == bitsOf(jsNull()));
}

static StubRun runInsertion(Collections& collections, StubKey stub, uint64_t owner, uint64_t key, std::optional<uint64_t> value)
{
    if (value)
        return collections.runner.run(stub, { owner, key, *value });
    return collections.runner.run(stub, { owner, key });
}

static StubKey insertionStub(KeyComparison comparison, bool hasValues)
{
    return hasValues ? mapSet(comparison) : setAdd(comparison);
}

static void checkAppendedEntry(Collections& collections, KeyComparison comparison, uint64_t owner, FakeTable& table, const TableEntry& entry)
{
    uint32_t used = table.used();
    uint32_t alive = table.alive();
    StubRun run = runInsertion(collections, insertionStub(comparison, entry.value.has_value()), owner, entry.key, entry.value);
    AOT_TEST_CHECK(!run.calledOperation && run.result == owner);
    AOT_TEST_CHECK(table.used() == used + 1 && table.alive() == alive + 1);
    AOT_TEST_CHECK(table.find(entry.key, entry.hash) == used && table.keyAt(used) == entry.key);
    AOT_TEST_CHECK(!entry.value || table.valueAt(used) == *entry.value);
}

static void checkInsertionLeftToOperation(Collections& collections, KeyComparison comparison, uint64_t owner, const TableEntry& entry)
{
    Entry operation = entry.value ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd;
    collections.runner.recordCallsOf(operation);
    collections.runner.setResultOfRecordedOperation(owner);
    StubRun run = runInsertion(collections, insertionStub(comparison, entry.value.has_value()), owner, entry.key, entry.value);
    collections.runner.countCallsOf(operation);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == owner);
    AOT_TEST_CHECK(run.recordedArguments[0] == owner && run.recordedArguments[1] == entry.key);
    AOT_TEST_CHECK(!entry.value || run.recordedArguments[2] == *entry.value);
}

static void checkTableIsUnchanged(Collections& collections, KeyComparison comparison, uint64_t owner, const FakeTable& table, const TableEntry& entry)
{
    Vector<uint8_t> bytesBefore = table.copyOfBytesWithoutMarksOfHomeSlots();
    checkInsertionLeftToOperation(collections, comparison, owner, entry);
    AOT_TEST_CHECK(table.copyOfBytesWithoutMarksOfHomeSlots() == bytesBefore);
}

static TableEntry createStringEntry(FakeHeap& heap, const String& text, std::optional<uint64_t> value)
{
    return { heap.createString(text), text.impl()->hash(), value };
}

static TableEntry createEntryComparedByBits(uint64_t key, std::optional<uint64_t> value)
{
    return { key, hashOfBits(key) & CompactTable::hashMask, value };
}

static uint64_t createKeyComparedByBits(FakeHeap& heap, uint32_t number, Width keyWidth)
{
    if (!number)
        return bitsOf(jsUndefined());
    if (keyWidth == Width::Wide && number % 4 == 2)
        return bitsOf(jsNumber(static_cast<int32_t>(number)));
    return heap.createCell(number % 8 ? FinalObjectType : SymbolType);
}

static std::optional<uint64_t> createValue(FakeHeap& heap, uint32_t number, Width valueWidth)
{
    switch (valueWidth) {
    case Width::None:
        return std::nullopt;
    case Width::Wide:
        if (number % 2)
            return bitsOf(jsNumber(number + 0.5));
        break;
    case Width::Narrow:
        break;
    case Width::InImage:
        AOT_TEST_CHECK(false);
    }
    const uint64_t values[] = { heap.createCell(FinalObjectType), bitsOf(jsUndefined()), bitsOf(jsBoolean(true)), bitsOf(jsNull()) };
    return values[number / 2 % std::size(values)];
}

static void checkTableFilledByStubs(Collections& collections, const TableSize& size, Width keyWidth, Width valueWidth)
{
    FakeHeap& heap = collections.heap;
    bool hasValues = valueWidth != Width::None;
    FakeTable table(heap, size.capacity, keyWidth, valueWidth);
    AOT_TEST_CHECK(table.slotKind() == size.slotKind);
    uint64_t owner = collections.createOwner(hasValues ? JSMapType : JSSetType, table.address());

    Vector<TableEntry> entries;
    for (uint32_t number = 0; number <= size.capacity; ++number) {
        bool isString = number % 2;
        std::optional<uint64_t> value = createValue(heap, number, valueWidth);
        TableEntry entry = isString ? createStringEntry(heap, makeString("key"_s, number), value) : createEntryComparedByBits(createKeyComparedByBits(heap, number, keyWidth), value);
        KeyComparison comparison = number % 4 < 2 ? KeyComparison::AnyValue : isString ? KeyComparison::String : KeyComparison::Bits;
        if (number == size.capacity) {
            checkTableIsUnchanged(collections, comparison, owner, table, entry);
            break;
        }
        checkAppendedEntry(collections, comparison, owner, table, entry);
        entries.append(entry);
    }
    for (const TableEntry& entry : entries) {
        if (hasValues)
            checkAnswer(collections, mapGet(KeyComparison::AnyValue), owner, entry.key, *entry.value);
        else
            checkAnswer(collections, setHas(KeyComparison::AnyValue), owner, entry.key, bitsOf(jsBoolean(true)));
    }
}

static void testAppendedEntries()
{
    Collections collections;
    for (const TableSize& size : tableSizes) {
        for (Width keyWidth : { Width::Narrow, Width::Wide }) {
            for (Width valueWidth : { Width::None, Width::Narrow, Width::Wide })
                checkTableFilledByStubs(collections, size, keyWidth, valueWidth);
        }
    }
}

static uint64_t createCellOutsideMarkedBlocks(FakeHeap& heap)
{
    uint8_t* bytes = heap.allocate(2 * sizeof(FakeCell)) + PreciseAllocation::halfAlignment;
    auto* cell = reinterpret_cast<FakeCell*>(bytes);
    cell->type = FinalObjectType;
    cell->cellState = static_cast<uint8_t>(CellState::DefinitelyWhite);
    return std::bit_cast<uintptr_t>(bytes);
}

static void testEntriesWiderThanColumns()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    const uint64_t number = bitsOf(jsNumber(42));
    const uint64_t largeCell = createCellOutsideMarkedBlocks(heap);
    for (const TableSize& size : tableSizes) {
        if (size.numberOfKeys == size.capacity)
            continue;
        FakeTable mapTable(heap, size.capacity, Width::Narrow, Width::Narrow);
        FakeTable setTable(heap, size.capacity, Width::Narrow, Width::None);
        appendStringKeys(collections, mapTable, size.numberOfKeys, true);
        uint64_t map = collections.createOwner(JSMapType, mapTable.address());
        uint64_t set = collections.createOwner(JSSetType, setTable.address());
        uint64_t object = heap.createCell(FinalObjectType);

        for (uint64_t wideWord : { number, largeCell }) {
            for (KeyComparison comparison : comparisonsOfBits) {
                checkTableIsUnchanged(collections, comparison, set, setTable, createEntryComparedByBits(wideWord, std::nullopt));
                checkTableIsUnchanged(collections, comparison, map, mapTable, createEntryComparedByBits(wideWord, object));
                checkTableIsUnchanged(collections, comparison, map, mapTable, createEntryComparedByBits(object, wideWord));
            }
            for (KeyComparison comparison : comparisonsOfStrings)
                checkTableIsUnchanged(collections, comparison, map, mapTable, createStringEntry(heap, "absent"_s, wideWord));
        }
        checkAppendedEntry(collections, KeyComparison::Bits, map, mapTable, createEntryComparedByBits(object, object));
    }
}

static void testAppendBarrier()
{
    Collections collections;
    FakeTable table(collections.heap, 4, Width::Narrow, Width::None);
    uint64_t set = collections.createOwner(JSSetType, table.address());
    table.setCellState(CellState::PossiblyBlack);
    collections.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
    uint64_t key = collections.heap.createCell(FinalObjectType);
    StubRun run = collections.runner.run(setAdd(KeyComparison::Bits), { set, key });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == table.address() && run.result == set);
    AOT_TEST_CHECK(table.used() == 1 && table.keyAt(0) == key);
}

static uint64_t& storageOf(uint64_t owner)
{
    return *std::bit_cast<uint64_t*>(static_cast<uintptr_t>(owner) + JSMap::offsetOfStorage());
}

static void checkCreatedTable(Collections& collections, KeyComparison comparison, const TableEntry& entry, Width keyWidth, Width valueWidth)
{
    FakeHeap& heap = collections.heap;
    bool hasValues = entry.value.has_value();
    size_t size = CompactTable::initialSize(keyWidth, valueWidth);
    uint8_t* room = heap.allocate(size);
    memsetSpan(std::span(room, size), 0xab);
    FakeAllocator allocator(size);
    allocator.makeRoomAt(std::bit_cast<uintptr_t>(room), size);
    collections.setAllocator(size, &allocator);
    uint64_t owner = collections.createOwner(hasValues ? JSMapType : JSSetType, 0);

    StubRun run = runInsertion(collections, insertionStub(comparison, hasValues), owner, entry.key, entry.value);
    collections.setAllocator(size, nullptr);
    AOT_TEST_CHECK(!run.calledOperation && run.result == owner && !allocator.hasRoom());
    AOT_TEST_CHECK(storageOf(owner) == std::bit_cast<uintptr_t>(room));

    FakeTable table(heap, CompactTable::initialCapacity, keyWidth, valueWidth, room);
    AOT_TEST_CHECK(table.header() == Collections::headerOfCreatedTables && table.capacityAndKinds() == CompactTable::initialCapacityAndKinds(keyWidth, valueWidth));
    AOT_TEST_CHECK(table.used() == 1 && table.alive() == 1 && table.find(entry.key, entry.hash) == 0u);
    AOT_TEST_CHECK(table.keyAt(0) == entry.key && (!hasValues || table.valueAt(0) == *entry.value));
    AOT_TEST_CHECK(*reinterpret_cast<uint64_t*>(room + CompactTable::offsetOfIndex()) == entry.hash >> CompactTable::tagShiftOfTags);
    for (uint32_t unused = 1; unused < CompactTable::initialCapacity; ++unused)
        AOT_TEST_CHECK(!table.keyAt(unused) && (!hasValues || !table.valueAt(unused)));

    if (hasValues)
        checkAnswer(collections, mapGet(comparison), owner, entry.key, *entry.value);
    else
        checkAnswer(collections, setHas(comparison), owner, entry.key, bitsOf(jsBoolean(true)));
    checkAppendedEntry(collections, KeyComparison::Bits, owner, table, createEntryComparedByBits(bitsOf(jsBoolean(true)), hasValues ? std::optional(bitsOf(jsNull())) : std::nullopt));
}

static void testCreatedTables()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    const uint64_t number = bitsOf(jsNumber(7));
    const uint64_t object = heap.createCell(FinalObjectType);
    const std::pair<std::optional<uint64_t>, Width> values[] = { { std::nullopt, Width::None }, { object, Width::Narrow }, { bitsOf(jsBoolean(false)), Width::Narrow }, { number, Width::Wide }, { createCellOutsideMarkedBlocks(heap), Width::Wide } };
    for (auto [value, valueWidth] : values) {
        for (KeyComparison comparison : comparisonsOfStrings) {
            checkCreatedTable(collections, comparison, createStringEntry(heap, "first"_s, value), Width::Narrow, valueWidth);
            checkCreatedTable(collections, comparison, { heap.createStringWithoutHash("first"_s), String("first"_s).impl()->hash(), value }, Width::Narrow, valueWidth);
        }
        for (KeyComparison comparison : comparisonsOfBits) {
            checkCreatedTable(collections, comparison, createEntryComparedByBits(heap.createCell(SymbolType), value), Width::Narrow, valueWidth);
            checkCreatedTable(collections, comparison, createEntryComparedByBits(bitsOf(jsUndefined()), value), Width::Narrow, valueWidth);
            checkCreatedTable(collections, comparison, createEntryComparedByBits(number, value), Width::Wide, valueWidth);
        }
        checkCreatedTable(collections, KeyComparison::String, createEntryComparedByBits(bitsOf(jsNull()), value), Width::Narrow, valueWidth);
    }
}

static void testTablesNotCreatedByStubs()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    uint64_t object = heap.createCell(FinalObjectType);
    size_t size = CompactTable::initialSize(Width::Narrow, Width::Narrow);
    FakeAllocator allocatorWithoutRoom(size);

    for (FakeAllocator* allocator : { static_cast<FakeAllocator*>(nullptr), &allocatorWithoutRoom }) {
        collections.setAllocator(size, allocator);
        uint64_t map = collections.createOwner(JSMapType, 0);
        checkInsertionLeftToOperation(collections, KeyComparison::Bits, map, createEntryComparedByBits(object, object));
        checkInsertionLeftToOperation(collections, KeyComparison::String, map, createStringEntry(heap, "first"_s, object));
        AOT_TEST_CHECK(!storageOf(map));
    }

    uint8_t* room = heap.allocate(size);
    FakeAllocator allocator(size);
    allocator.makeRoomAt(std::bit_cast<uintptr_t>(room), size);
    collections.setAllocator(size, &allocator);
    uint64_t map = collections.createOwner(JSMapType, 0);
    for (uint64_t key : { heap.createRope(), bitsOf(jsNumber(0.5)), heap.createCell(HeapBigIntType) })
        checkInsertionLeftToOperation(collections, KeyComparison::AnyValue, map, { key, 0, object });
    AOT_TEST_CHECK(!storageOf(map) && allocator.hasRoom());
}

static void testCreatedTableBarrier()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    size_t size = CompactTable::initialSize(Width::Narrow, Width::None);
    uint8_t* room = heap.allocate(size);
    FakeAllocator allocator(size);
    allocator.makeRoomAt(std::bit_cast<uintptr_t>(room), size);
    collections.setAllocator(size, &allocator);
    uint64_t set = collections.createOwner(JSSetType, 0);
    std::bit_cast<FakeCell*>(static_cast<uintptr_t>(set))->cellState = static_cast<uint8_t>(CellState::PossiblyBlack);

    collections.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
    StubRun run = collections.runner.run(setAdd(KeyComparison::Bits), { set, heap.createCell(FinalObjectType) });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == set && run.result == set);
    AOT_TEST_CHECK(storageOf(set) == std::bit_cast<uintptr_t>(room));
}

using GrowthPlans = std::array<uint64_t, 3 * CompactTable::growthPlansPerValueWidth>;

static uint64_t& growthPlanFor(GrowthPlans& plans, Width valueWidth, uint32_t count)
{
    return plans[static_cast<unsigned>(valueWidth) / 4 * CompactTable::growthPlansPerValueWidth + count / 2];
}

static GrowthPlans createGrowthPlans(FakeHeap& heap)
{
    GrowthPlans plans { };
    for (Width valueWidth : { Width::None, Width::Narrow, Width::Wide }) {
        for (uint32_t count = 0; count < CompactTable::maxCountWithGrowthPlan; count += 2) {
            uint32_t capacity = WTF::roundUpToMultipleOf<2>(count + count / 2 + 1);
            FakeTable table(heap, capacity, Width::Narrow, valueWidth);
            growthPlanFor(plans, valueWidth, count) = table.capacityAndKinds() | static_cast<uint64_t>(MarkedSpace::sizeClassToIndex(table.size())) << CompactTable::sizeStepShiftOfGrowthPlan;
        }
    }
    return plans;
}

struct FullTable {
    FullTable(Collections& collections, uint32_t capacity, Width keyWidth, Width valueWidth, unsigned numberOfOtherKeys = 0, JSType typeOfOtherKeys = FinalObjectType)
        : table(collections.heap, capacity, keyWidth, valueWidth)
        , owner(collections.createOwner(valueWidth == Width::None ? JSSetType : JSMapType, table.address()))
    {
        FakeHeap& heap = collections.heap;
        for (uint32_t number = 0; number < capacity; ++number) {
            std::optional<uint64_t> value = createValue(heap, number, valueWidth);
            entries.append(number < numberOfOtherKeys ? createEntryComparedByBits(heap.createCell(typeOfOtherKeys), value) : createStringEntry(heap, makeString("key"_s, number), value));
            table.append(entries.last().key, entries.last().hash, value.value_or(0));
        }
    }

    FakeTable table;
    uint64_t owner;
    Vector<TableEntry> entries;
};

struct RoomForTable {
    RoomForTable(Collections& collections, size_t size)
        : collections(collections)
        , size(size)
        , bytes(collections.heap.allocate(size))
        , allocator(size)
    {
        memsetSpan(std::span(bytes, size), 0xab);
        allocator.makeRoomAt(std::bit_cast<uintptr_t>(bytes), size);
        collections.setAllocator(size, &allocator);
    }

    ~RoomForTable() { collections.setAllocator(size, nullptr); }

    Collections& collections;
    size_t size;
    uint8_t* bytes;
    FakeAllocator allocator;
};

static void checkGrownTable(Collections& collections, GrowthPlans& plans, KeyComparison comparison, uint32_t capacity, Width valueWidth, unsigned numberOfOtherKeys, bool wasWalked, std::optional<JSType> typeOfAddedCell = std::nullopt)
{
    FakeHeap& heap = collections.heap;
    FullTable full(collections, capacity, Width::Narrow, valueWidth, numberOfOtherKeys);
    if (wasWalked)
        full.table.markAsWalked();
    uint32_t largerCapacity = static_cast<uint32_t>(growthPlanFor(plans, valueWidth, capacity));
    FakeTable expected(heap, largerCapacity, Width::Narrow, valueWidth);
    if (wasWalked)
        expected.markAsWalked();
    std::optional<uint64_t> addedValue = createValue(heap, capacity, valueWidth);
    TableEntry added = typeOfAddedCell ? createEntryComparedByBits(heap.createCell(*typeOfAddedCell), addedValue) : createStringEntry(heap, "added"_s, addedValue);
    full.entries.append(added);
    for (const TableEntry& entry : full.entries)
        expected.append(entry.key, entry.hash, entry.value.value_or(0));

    RoomForTable room(collections, expected.size());
    StubRun run = runInsertion(collections, insertionStub(comparison, added.value.has_value()), full.owner, added.key, added.value);
    AOT_TEST_CHECK(!run.calledOperation && run.result == full.owner && !room.allocator.hasRoom());
    AOT_TEST_CHECK(storageOf(full.owner) == std::bit_cast<uintptr_t>(room.bytes));

    FakeTable larger(heap, largerCapacity, Width::Narrow, valueWidth, room.bytes);
    AOT_TEST_CHECK(larger.header() == Collections::headerOfCreatedTables);
    AOT_TEST_CHECK(equalSpans(larger.copyOfBytes().subspan(sizeof(uint64_t)), expected.copyOfBytes().subspan(sizeof(uint64_t))));

    uint8_t* retired = std::bit_cast<uint8_t*>(static_cast<uintptr_t>(full.table.address()));
    AOT_TEST_CHECK(retired[CompactTable::offsetOfStatus()] == static_cast<uint8_t>(CompactTable::Status::Replaced));
    AOT_TEST_CHECK(*reinterpret_cast<uint64_t*>(retired + CompactTable::offsetOfUsed()) == larger.address() && !*reinterpret_cast<uint32_t*>(retired + CompactTable::offsetOfCapacity()));
}

static void testGrownTables()
{
    for (Width valueWidth : { Width::None, Width::Narrow, Width::Wide }) {
        Collections collections;
        GrowthPlans plans = createGrowthPlans(collections.heap);
        collections.runner.instance().allocation.compactTableGrowthPlans = plans.data();
        for (uint32_t capacity = 2; capacity < CompactTable::maxCountWithGrowthPlan; capacity += 2) {
            for (KeyComparison comparison : comparisonsOfStrings)
                checkGrownTable(collections, plans, comparison, capacity, valueWidth, 0, capacity % 4);
            for (KeyComparison comparison : comparisonsOfBits)
                checkGrownTable(collections, plans, comparison, capacity, valueWidth, capacity, capacity % 4, FinalObjectType);
            checkGrownTable(collections, plans, KeyComparison::Bits, capacity, valueWidth, capacity / 2, capacity % 4, SymbolType);
            checkGrownTable(collections, plans, KeyComparison::AnyValue, capacity, valueWidth, capacity / 2, capacity % 4);
        }
    }
}

static void checkTableIsNotGrown(Collections& collections, GrowthPlans& plans, FullTable& full, const TableEntry& added, bool takesRoom = false)
{
    Width valueWidth = added.value ? static_cast<Width>(static_cast<uint8_t>(full.table.capacityAndKinds() >> 48)) : Width::None;
    uint32_t capacity = static_cast<uint32_t>(full.table.capacityAndKinds());
    uint32_t largerCapacity = capacity < CompactTable::maxCountWithGrowthPlan ? static_cast<uint32_t>(growthPlanFor(plans, valueWidth, capacity)) : 2 * capacity;
    RoomForTable room(collections, FakeTable(collections.heap, largerCapacity, Width::Narrow, valueWidth).size());
    KeyComparison comparison = KeyComparison::AnyValue;
    checkTableIsUnchanged(collections, comparison, full.owner, full.table, added);
    AOT_TEST_CHECK(storageOf(full.owner) == full.table.address() && room.allocator.hasRoom() == !takesRoom);
}

static void testTablesNotGrownByStubs()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    GrowthPlans plans = createGrowthPlans(heap);
    Instance& instance = collections.runner.instance();
    instance.allocation.compactTableGrowthPlans = plans.data();
    uint64_t object = heap.createCell(FinalObjectType);
    auto stringEntry = [&](std::optional<uint64_t> value) {
        return createStringEntry(heap, "added"_s, value);
    };

    {
        FullTable full(collections, 8, Width::Narrow, Width::Narrow);
        instance.allocation.compactTableGrowthPlans = nullptr;
        checkTableIsNotGrown(collections, plans, full, stringEntry(object));
        instance.allocation.compactTableGrowthPlans = plans.data();

        instance.mutatorShouldBeFenced = true;
        checkTableIsNotGrown(collections, plans, full, stringEntry(object));
        instance.mutatorShouldBeFenced = false;

        checkTableIsNotGrown(collections, plans, full, createEntryComparedByBits(bitsOf(jsNull()), object));
        checkTableIsNotGrown(collections, plans, full, createEntryComparedByBits(createCellOutsideMarkedBlocks(heap), object));
        checkTableIsNotGrown(collections, plans, full, stringEntry(bitsOf(jsNumber(42))));
        checkTableIsNotGrown(collections, plans, full, stringEntry(createCellOutsideMarkedBlocks(heap)));

        checkTableIsUnchanged(collections, KeyComparison::String, full.owner, full.table, stringEntry(object));
        FakeAllocator allocatorWithoutRoom(FakeTable(heap, static_cast<uint32_t>(growthPlanFor(plans, Width::Narrow, 8)), Width::Narrow, Width::Narrow).size());
        collections.setAllocator(allocatorWithoutRoom.at<uint32_t>(LocalAllocator::offsetOfCellSize()), &allocatorWithoutRoom);
        checkTableIsUnchanged(collections, KeyComparison::String, full.owner, full.table, stringEntry(object));
        collections.setAllocator(allocatorWithoutRoom.at<uint32_t>(LocalAllocator::offsetOfCellSize()), nullptr);

        full.table.deleteLastEntry();
        checkTableIsNotGrown(collections, plans, full, stringEntry(object));
    }
    {
        FullTable full(collections, 8, Width::Wide, Width::None);
        checkTableIsNotGrown(collections, plans, full, stringEntry(std::nullopt));
    }
    {
        FullTable full(collections, CompactTable::maxCountWithGrowthPlan, Width::Narrow, Width::None);
        checkTableIsNotGrown(collections, plans, full, stringEntry(std::nullopt));
    }
    for (Width valueWidth : { Width::None, Width::Narrow, Width::Wide }) {
        // The index is made anew, and the hash of a BigInt is neither in the cell nor made of its address.
        FullTable full(collections, 8, Width::Narrow, valueWidth, 1, HeapBigIntType);
        checkTableIsNotGrown(collections, plans, full, stringEntry(createValue(heap, 0, valueWidth)), true);
    }
}

static void testGrownTableBarriers()
{
    Collections collections;
    GrowthPlans plans = createGrowthPlans(collections.heap);
    collections.runner.instance().allocation.compactTableGrowthPlans = plans.data();
    for (bool tableIsBlack : { false, true }) {
        for (bool ownerIsBlack : { false, true }) {
            FullTable full(collections, 4, Width::Narrow, Width::None);
            RoomForTable room(collections, FakeTable(collections.heap, static_cast<uint32_t>(growthPlanFor(plans, Width::None, 4)), Width::Narrow, Width::None).size());
            if (tableIsBlack)
                full.table.setCellState(CellState::PossiblyBlack);
            if (ownerIsBlack)
                std::bit_cast<FakeCell*>(static_cast<uintptr_t>(full.owner))->cellState = static_cast<uint8_t>(CellState::PossiblyBlack);
            collections.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
            StubRun run = collections.runner.run(setAdd(KeyComparison::String), { full.owner, collections.heap.createString("added"_s) });
            AOT_TEST_CHECK(run.numberOfRecordedCalls == static_cast<unsigned>(tableIsBlack) + ownerIsBlack && run.result == full.owner);
            AOT_TEST_CHECK(!run.numberOfRecordedCalls || run.recordedArguments[0] == (ownerIsBlack ? full.owner : full.table.address()));
            AOT_TEST_CHECK(storageOf(full.owner) == std::bit_cast<uintptr_t>(room.bytes));
        }
    }
}

static StubKey deletionStub(KeyComparison comparison, bool hasValues)
{
    return hasValues ? mapDelete(comparison) : setDelete(comparison);
}

static void checkDeletedEntry(Collections& collections, KeyComparison comparison, uint64_t owner, FakeTable& table, const TableEntry& entry, uint64_t keyWithSameValue)
{
    bool hasValues = entry.value.has_value();
    uint32_t used = table.used();
    uint32_t alive = table.alive();
    std::optional<uint32_t> position = table.find(entry.key, entry.hash);
    AOT_TEST_CHECK(position.has_value());

    checkAnswer(collections, deletionStub(comparison, hasValues), owner, keyWithSameValue, bitsOf(jsBoolean(true)));
    AOT_TEST_CHECK(table.used() == used && table.alive() == alive - 1);
    AOT_TEST_CHECK(!table.keyAt(*position) && (!hasValues || !table.valueAt(*position)) && !table.find(entry.key, entry.hash));
    checkAnswer(collections, deletionStub(comparison, hasValues), owner, keyWithSameValue, bitsOf(jsBoolean(false)));
    checkAnswer(collections, hasValues ? mapHas(comparison) : setHas(comparison), owner, entry.key, bitsOf(jsBoolean(false)));
    AOT_TEST_CHECK(table.used() == used && table.alive() == alive - 1);
}

static void checkDeletions(Collections& collections, const TableSize& size, Width keyWidth, Width valueWidth)
{
    FakeHeap& heap = collections.heap;
    bool hasValues = valueWidth != Width::None;
    FakeTable table(heap, size.capacity, keyWidth, valueWidth);
    uint64_t owner = collections.createOwner(hasValues ? JSMapType : JSSetType, table.address());
    Vector<TableEntry> entries;
    Vector<String> texts;
    for (uint32_t number = 0; number + 1 < size.numberOfKeys; ++number) {
        bool isString = number % 2;
        texts.append(makeString("key"_s, number));
        std::optional<uint64_t> value = createValue(heap, number, valueWidth);
        entries.append(isString ? createStringEntry(heap, texts.last(), value) : createEntryComparedByBits(createKeyComparedByBits(heap, number, keyWidth), value));
        table.append(entries.last().key, entries.last().hash, value.value_or(0));
    }

    for (uint32_t number = 0; number < entries.size(); number += 3) {
        bool isString = number % 2;
        KeyComparison comparison = number % 4 < 2 ? KeyComparison::AnyValue : isString ? KeyComparison::String : KeyComparison::Bits;
        uint64_t keyWithSameValue = !isString ? entries[number].key : number % 12 < 6 ? heap.createStringWithoutHash(texts[number]) : heap.createString(createHashedCopy(texts[number], number % 8 < 4));
        checkDeletedEntry(collections, comparison, owner, table, entries[number], keyWithSameValue);
    }
    for (uint32_t number = 0; number < entries.size(); ++number)
        checkAnswer(collections, hasValues ? mapHas(KeyComparison::AnyValue) : setHas(KeyComparison::AnyValue), owner, entries[number].key, bitsOf(jsBoolean(!!(number % 3))));
    checkAppendedEntry(collections, KeyComparison::AnyValue, owner, table, entries[0]);
}

static void testDeletedEntries()
{
    Collections collections;
    for (const TableSize& size : tableSizes) {
        for (Width keyWidth : { Width::Narrow, Width::Wide }) {
            for (Width valueWidth : { Width::None, Width::Narrow, Width::Wide })
                checkDeletions(collections, size, keyWidth, valueWidth);
        }
    }
}

static void testDeletionsLeftToOperations()
{
    Collections collections;
    FakeHeap& heap = collections.heap;
    FakeTable table(heap, 4, Width::Narrow, Width::Narrow);
    Vector<StringKey> keys = appendStringKeys(collections, table, 2, true);
    uint64_t map = collections.createOwner(JSMapType, table.address());
    uint64_t set = collections.createOwner(JSSetType, table.address());
    Vector<uint8_t> bytesBefore = table.copyOfBytes();

    for (KeyComparison comparison : comparisonsOfStrings) {
        checkLeftToOperation(collections, mapDelete(comparison), Entry::operationAOTMapDelete, map, heap.createRope());
        checkLeftToOperation(collections, setDelete(comparison), Entry::operationAOTSetDelete, set, heap.createRope());
    }
    for (uint64_t key : { bitsOf(jsNumber(0.5)), heap.createCell(HeapBigIntType) })
        checkLeftToOperation(collections, mapDelete(KeyComparison::AnyValue), Entry::operationAOTMapDelete, map, key);
    AOT_TEST_CHECK(table.copyOfBytes() == bytesBefore);

    uint64_t mapWithoutStorage = collections.createOwner(JSMapType, 0);
    for (KeyComparison comparison : comparisonsOfStrings)
        checkAnswer(collections, mapDelete(comparison), mapWithoutStorage, keys[0].cell, bitsOf(jsBoolean(false)));
    checkAnswer(collections, mapDelete(KeyComparison::Bits), mapWithoutStorage, heap.createCell(FinalObjectType), bitsOf(jsBoolean(false)));
}

static uint64_t createTableInImage(FakeHeap& heap, const TableInImage& entries)
{
    uint8_t* table = heap.allocate(CompactTable::offsetOfIndex() + 2 * sizeof(void*));
    auto at = [&]<typename T>(ptrdiff_t offset, T value) {
        *reinterpret_cast<T*>(table + offset) = value;
    };
    reinterpret_cast<FakeCell*>(table)->type = CellType;
    at(CompactTable::offsetOfUsed(), entries.count);
    at(CompactTable::offsetOfAliveCount(), entries.count);
    at(CompactTable::offsetOfCapacity(), entries.capacity | CompactTable::capacityIsInImage);
    at(CompactTable::offsetOfIndexBits(), static_cast<uint8_t>((entries.indexBits & CompactTable::numberOfIndexBits) | CompactTable::indexIsInImage));
    at(CompactTable::offsetOfKeysShift(), static_cast<uint8_t>(entries.keysShift | CompactTable::keysAreInImage));
    at(CompactTable::offsetOfValueWidth(), static_cast<uint8_t>(entries.hasValues ? Width::InImage : Width::None));
    at(CompactTable::offsetOfIndex(), &entries);
    return std::bit_cast<uintptr_t>(table);
}

static std::optional<uint32_t> firstEntryWithTagOf(const TableInImage& entries, unsigned hash)
{
    return entries.findWithHash(hash, [](uint32_t) {
        return true;
    });
}

// The tags alone may say that a key is absent. A key that has the tag of an entry is left to the operation, which moves the table to the heap.
static void checkLookupInImage(Collections& collections, const TableInImage& entries, uint64_t map, const String& text)
{
    uint64_t key = collections.heap.createString(text);
    bool hasTagOfAnEntry = firstEntryWithTagOf(entries, text.impl()->hash()).has_value();
    for (KeyComparison comparison : comparisonsOfStrings) {
        if (hasTagOfAnEntry) {
            checkLeftToOperation(collections, mapGet(comparison), Entry::operationAOTMapGet, map, key);
            checkLeftToOperation(collections, mapHas(comparison), Entry::operationAOTMapHas, map, key);
            continue;
        }
        checkAnswer(collections, mapHas(comparison), map, key, bitsOf(jsBoolean(false)));
        checkAnswer(collections, mapGet(comparison), map, key, bitsOf(jsUndefined()));
    }
}

static void checkTableInImage(Collections& collections, unsigned numberOfKeys)
{
    Vector<String> texts;
    Vector<TableInImage::Key> keys;
    Vector<uint64_t> values;
    for (uint32_t i = 0; i < numberOfKeys; ++i) {
        texts.append(makeString("key"_s, i));
        keys.append({ i, texts.last().impl()->hash() });
        values.append(bitsOf(jsNumber(static_cast<int32_t>(i))));
    }
    std::optional<Vector<uint8_t>> bytes = TableInImage::tryEncode(keys.span(), values.span());
    AOT_TEST_CHECK(bytes.has_value());
    const TableInImage& entries = *reinterpret_cast<const TableInImage*>(bytes->span().data());
    uint64_t map = collections.createOwner(JSMapType, createTableInImage(collections.heap, entries));

    for (uint32_t i = 0; i < numberOfKeys; ++i)
        checkLookupInImage(collections, entries, map, createHashedCopy(texts[i], i % 2));
    String absentText = "absent"_s;
    absentText.impl()->hash();
    checkLookupInImage(collections, entries, map, absentText);

    uint64_t object = collections.heap.createCell(FinalObjectType);
    uint64_t presentKey = collections.heap.createString(createHashedCopy(texts[0], false));
    for (KeyComparison comparison : comparisonsOfBits) {
        checkAnswer(collections, mapGet(comparison), map, object, bitsOf(jsUndefined()));
        checkAnswer(collections, mapDelete(comparison), map, object, bitsOf(jsBoolean(false)));
        checkStoreLeftToOperation(collections, mapSet(comparison), map, object, bitsOf(jsNull()));
        checkLeftToOperation(collections, setAdd(comparison), Entry::operationAOTSetAdd, map, object);
    }
    for (KeyComparison comparison : comparisonsOfStrings) {
        checkStoreLeftToOperation(collections, mapSet(comparison), map, presentKey, bitsOf(jsNull()));
        checkLeftToOperation(collections, setAdd(comparison), Entry::operationAOTSetAdd, map, presentKey);
        checkLeftToOperation(collections, mapDelete(comparison), Entry::operationAOTMapDelete, map, presentKey);
    }
}

static void testTablesInImage()
{
    Collections collections;
    for (const TableSize& size : tableSizes)
        checkTableInImage(collections, size.numberOfKeys);
}

std::span<const NativeTest> stubCollectionTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/collections/tables-in-image"_s, ImageTests::whenExecutable<testTablesInImage> },
        { "stubs/collections/string-keys"_s, ImageTests::whenExecutable<testStringKeys> },
        { "stubs/collections/strings-beside-other-keys"_s, ImageTests::whenExecutable<testStringsBesideOtherKeys> },
        { "stubs/collections/keys-compared-by-bits"_s, ImageTests::whenExecutable<testKeysComparedByBits> },
        { "stubs/collections/keys-left-to-operations"_s, ImageTests::whenExecutable<testKeysLeftToOperations> },
        { "stubs/collections/owners-without-storage"_s, ImageTests::whenExecutable<testOwnersWithoutStorage> },
        { "stubs/collections/map-set"_s, ImageTests::whenExecutable<testMapSet> },
        { "stubs/collections/map-set-barrier"_s, ImageTests::whenExecutable<testMapSetBarrier> },
        { "stubs/collections/appended-entries"_s, ImageTests::whenExecutable<testAppendedEntries> },
        { "stubs/collections/entries-wider-than-columns"_s, ImageTests::whenExecutable<testEntriesWiderThanColumns> },
        { "stubs/collections/append-barrier"_s, ImageTests::whenExecutable<testAppendBarrier> },
        { "stubs/collections/created-tables"_s, ImageTests::whenExecutable<testCreatedTables> },
        { "stubs/collections/tables-not-created-by-stubs"_s, ImageTests::whenExecutable<testTablesNotCreatedByStubs> },
        { "stubs/collections/created-table-barrier"_s, ImageTests::whenExecutable<testCreatedTableBarrier> },
        { "stubs/collections/grown-tables"_s, ImageTests::whenExecutable<testGrownTables> },
        { "stubs/collections/tables-not-grown-by-stubs"_s, ImageTests::whenExecutable<testTablesNotGrownByStubs> },
        { "stubs/collections/grown-table-barriers"_s, ImageTests::whenExecutable<testGrownTableBarriers> },
        { "stubs/collections/deleted-entries"_s, ImageTests::whenExecutable<testDeletedEntries> },
        { "stubs/collections/deletions-left-to-operations"_s, ImageTests::whenExecutable<testDeletionsLeftToOperations> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
