/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCollectionIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTResultRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTVector.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSSet.h"
#include "VM.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OBJECT_OFFSETOF(JSMap, m_storage) == OBJECT_OFFSETOF(JSSet, m_storage));
static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
static_assert(JSMap::Helper::dataTableStartIndex(JSMap::Helper::LargeCapacity) == JSMap::Helper::hashTableStartIndex() + JSMap::Helper::LargeCapacity);
static_assert(static_cast<unsigned>(SetNextResult::HasEntry) == static_cast<unsigned>(MapNextResult::HasEntry) && static_cast<unsigned>(SetNextResult::Entry) == static_cast<unsigned>(MapNextResult::Entry) && static_cast<unsigned>(SetNextResult::Element) == static_cast<unsigned>(MapNextResult::Key));
static_assert(static_cast<unsigned>(MapNextResult::Value) == numberOfEnumerators<SetNextResult> && numberOfEnumerators<MapNextResult> == numberOfEnumerators<SetNextResult> + 1);

namespace {

struct TableEntry {
    Boxed key;
    Boxed value;
};

class StepJoin {
public:
    explicit StepJoin(Output& out)
        : m_out(out)
        , m_continuation(out.newBlock())
    {
    }

    void add(Condition hasEntry, Word32 entryIndex, const TableEntry& entry)
    {
        m_hasEntry.append(m_out.anchor(hasEntry.value()));
        m_entryIndexes.append(m_out.anchor(entryIndex.value()));
        m_keys.append(m_out.anchor(entry.key.value()));
        m_values.append(m_out.anchor(entry.value.value()));
        m_out.jump(m_continuation);
    }

    std::array<Operand, numberOfEnumerators<MapNextResult>> finish()
    {
        m_out.appendTo(m_continuation);
        std::array<Operand, numberOfEnumerators<MapNextResult>> results;
        results[static_cast<size_t>(MapNextResult::HasEntry)] = Condition(m_out.phi(B3::Int32, m_hasEntry));
        results[static_cast<size_t>(MapNextResult::Entry)] = Word32(m_out.phi(B3::Int32, m_entryIndexes));
        results[static_cast<size_t>(MapNextResult::Key)] = Boxed(m_out.phi(B3::Int64, m_keys));
        results[static_cast<size_t>(MapNextResult::Value)] = Boxed(m_out.phi(B3::Int64, m_values));
        return results;
    }

private:
    Output& m_out;
    LBasicBlock m_continuation;
    Vector<ValueFromBlock, 4> m_hasEntry;
    Vector<ValueFromBlock, 4> m_entryIndexes;
    Vector<ValueFromBlock, 4> m_keys;
    Vector<ValueFromBlock, 4> m_values;
};

} // anonymous namespace

class CollectionIntrinsicEmitter::TableReader final : public OperationEmitter {
public:
    explicit TableReader(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    LValue storageOf(CellPointer owner)
    {
        return m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), owner.value(), JSMap::offsetOfStorage()));
    }

    TypedPointer tableField(LValue table, ptrdiff_t offset)
    {
        return m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), table, offset);
    }

    LValue keysOf(LValue table)
    {
        LValue keysShift = m_out.bitAnd(m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfKeysShift())), m_out.constInt32(CompactTable::keysShiftMask));
        LValue sizeOfIndex = m_out.shl(m_out.constInt32(2), keysShift);
        return m_out.add(table, m_out.add(m_out.zeroExtPtr(sizeOfIndex), m_out.constIntPtr(CompactTable::offsetOfIndex())));
    }

    TypedPointer elementAt(LValue base, LValue index, unsigned shift)
    {
        return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), m_out.add(base, m_out.shl(index, m_out.constInt32(shift))));
    }

    TypedPointer dataAt(LValue base, ptrdiff_t index)
    {
        return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), base, index * sizeof(EncodedJSValue));
    }

    LValue dataOf(LValue storage)
    {
        return m_out.add(storage, m_out.constIntPtr(JSCellButterfly::offsetOfData()));
    }

    LValue entriesOf(LValue data)
    {
        LValue capacity = m_out.zeroExtPtr(m_out.load32(dataAt(data, JSMap::Helper::capacityIndex())));
        return m_out.add(data, m_out.shl(m_out.add(capacity, m_out.constIntPtr(JSMap::Helper::hashTableStartIndex())), m_out.constInt32(3)));
    }

    LValue entryAt(Collection kind, LValue entries, LValue entryIndex)
    {
        return m_out.add(entries, m_out.mul(entryIndex, m_out.constIntPtr((kind == Collection::Map ? JSMap::Helper::EntrySize : JSSet::Helper::EntrySize) * sizeof(EncodedJSValue))));
    }

    Boxed decodeNarrow(NarrowReference word)
    {
        Condition isReference = isCellReferenceAmongAllWords(word);
        Boxed cell = boxCell(decode(word));
        Boxed immediate(m_out.zeroExtPtr(word.value()));
        return select(isReference, cell, immediate);
    }

    template<typename Functor>
    LValue firstAliveEntry(LValue start, LValue used, LBasicBlock hasNoEntry, const Functor& isAlive)
    {
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock isUsed = m_out.newBlock();
        LBasicBlock isDeleted = m_out.newBlock();
        LBasicBlock found = m_out.newBlock();
        ValueFromBlock first = m_out.anchor(start);
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue entryIndex = m_out.phi(B3::Int64, first);
        m_out.branch(m_out.below(entryIndex, used), normalEdge(isUsed), unlikelyEdge(hasNoEntry));

        m_out.appendTo(isUsed);
        m_out.branch(isAlive(entryIndex), normalEdge(found), unlikelyEdge(isDeleted));

        m_out.appendTo(isDeleted);
        m_out.addIncomingToPhi(entryIndex, m_out.anchor(m_out.add(entryIndex, m_out.constInt64(1))));
        m_out.jump(loop);

        m_out.appendTo(found);
        return entryIndex;
    }

    LValue isAliveInCompactTable(CompactTable::Width keyWidth, LValue keys, LValue entryIndex)
    {
        if (keyWidth == CompactTable::Width::Wide)
            return m_out.notZero64(m_out.load64(elementAt(keys, entryIndex, 3)));
        return m_out.notZero32(m_out.load32(elementAt(keys, entryIndex, 2)));
    }

    TableEntry compactEntryAt(Collection kind, CompactTable::Width keyWidth, LValue table, LValue keys, LValue entryIndex)
    {
        const bool keysAreWide = keyWidth == CompactTable::Width::Wide;
        Boxed key = keysAreWide ? Boxed(m_out.load64(elementAt(keys, entryIndex, 3))) : decodeNarrow(NarrowReference(m_out.load32(elementAt(keys, entryIndex, 2))));
        if (kind == Collection::Set)
            return { key, undefined() };
        LBasicBlock isNarrow = m_out.newBlock();
        LBasicBlock isWide = m_out.newBlock();
        Join<Boxed> join(m_out);
        LValue values = m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(tableField(table, CompactTable::offsetOfCapacity()))), m_out.constInt32(keysAreWide ? 3 : 2)));
        LValue width = m_out.load8ZeroExt32(tableField(table, CompactTable::offsetOfValueWidth()));
        m_out.branch(m_out.equal(width, m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow))), normalEdge(isNarrow), unlikelyEdge(isWide));

        m_out.appendTo(isNarrow);
        join.add(decodeNarrow(NarrowReference(m_out.load32(elementAt(values, entryIndex, 2)))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(elementAt(values, entryIndex, 3))));
        return { key, join.finish() };
    }

    TableEntry storageEntryAt(Collection kind, LValue entries, LValue entryIndex)
    {
        LValue words = entryAt(kind, entries, entryIndex);
        Boxed key(m_out.load64(dataAt(words, 0)));
        return { key, kind == Collection::Map ? Boxed(m_out.load64(dataAt(words, 1))) : undefined() };
    }
};

CollectionIntrinsicEmitter::CollectionIntrinsicEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void CollectionIntrinsicEmitter::generateNext(Collection kind, const ValueContext& value)
{
    CellPointer owner = in<CellPointer>(kind == Collection::Map ? value->mapOperand() : value->setOperand());
    Boxed table = in<Boxed>(value->tableOperand());
    Integer start = integerOf(value->entryOperand());
    const size_t numberOfResults = kind == Collection::Map ? numberOfEnumerators<MapNextResult> : numberOfEnumerators<SetNextResult>;
    // A stub stands in front of the operation and takes the step in a table that is still the owner's.
    auto addStepByOperation = [&](StepJoin& join) {
        RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
        Pointer buffer = calls.uninitializedArgumentBuffer(2);
        std::array<Operand, 4> arguments { owner, table, low32(start), buffer };
        StubCallResult step = calls.callOperationReturningPair(value, kind == Collection::Map ? RuntimeOperation::MapNext : RuntimeOperation::SetNext, arguments);
        auto resultInBuffer = [&](unsigned index) {
            return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, buffer.value(), index * sizeof(EncodedJSValue))));
        };
        TableEntry entryFromOperation { resultInBuffer(0), kind == Collection::Map ? resultInBuffer(1) : undefined() };
        Condition hasEntry(m_out.notZero64(step.first));
        join.add(hasEntry, Word32(m_out.castToInt32(step.second)), entryFromOperation);
    };
    if (value.shape() != CodeShape::Fast) {
        StepJoin join(m_out);
        addStepByOperation(join);
        auto results = join.finish();
        defineResults(value, std::span<const Operand>(results).first(numberOfResults));
        return;
    }

    TableReader reader(generator());
    LBasicBlock isCurrent = newBlock();
    LBasicBlock isChanged = newBlock();
    LBasicBlock isCompact = newBlock();
    LBasicBlock isOrdinary = newBlock();
    LBasicBlock hasNoEntry = newBlock();
    LBasicBlock slowPath = newBlock();
    StepJoin join(m_out);
    LValue storage = reader.storageOf(owner);
    m_out.branch(m_out.equal(storage, table.value()), normalEdge(isCurrent), unlikelyEdge(isChanged));

    m_out.appendTo(isChanged);
    m_out.branch(m_out.isNull(storage), normalEdge(hasNoEntry), rareEdge(slowPath));

    m_out.appendTo(isCurrent);
    m_out.branch(hasCellType(CellPointer(storage), JSCellButterflyType).value(), unlikelyEdge(isOrdinary), normalEdge(isCompact));

    m_out.appendTo(isCompact);
    // A number never fits a short key, and a table with one wide key has wide keys only: the type of the keys says which of the two widths is read in line. The other one is the stub's.
    Type keys = typeOfContentsRead({ m_program, m_function, types(), TypeLevel::Expected }, typeOf(kind == Collection::Map ? value->mapOperand() : value->setOperand()), kind == Collection::Map ? ContentsSlot::Key : ContentsSlot::Element);
    const CompactTable::Width keyWidth = keys.withoutPrimitives(Primitive::Undefined | Primitive::Null | Primitive::Boolean).isNumber() ? CompactTable::Width::Wide : CompactTable::Width::Narrow;
    if (keyWidth == CompactTable::Width::Wide) {
        // The bit for wide keys is the sign of the byte and the bit for the image the one below it.
        static_assert(CompactTable::keysAreWide == 0x80 && CompactTable::keysAreInImage == 0x40);
        branchUnless(Condition(m_out.lessThan(m_out.load8SignExt32(reader.tableField(storage, CompactTable::offsetOfKeysShift())), m_out.constInt32(static_cast<int8_t>(CompactTable::keysAreWide | CompactTable::keysAreInImage)))), slowPath);
    } else {
        Word32 flags(m_out.load8ZeroExt32(reader.tableField(storage, CompactTable::offsetOfKeysShift())));
        branchUnless(hasNoBits(flags, CompactTable::keysAreWide | CompactTable::keysAreInImage), slowPath);
    }
    LValue used = m_out.zeroExtPtr(m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed())));
    LValue column = reader.keysOf(storage);
    LValue entryIndex = reader.firstAliveEntry(start.value(), used, hasNoEntry, [&](LValue candidate) {
        return reader.isAliveInCompactTable(keyWidth, column, candidate);
    });
    // From here on the walk stands behind an entry. The mark is bookkeeping that only clear() reads, which comes after a step as a write of the table comes after a read. The other bits of the byte never change.
    LBasicBlock marks = newBlock();
    LBasicBlock isMarked = newBlock();
    TypedPointer byteWithMark = m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), storage, CompactTable::offsetOfKeysShift());
    Word32 flagsAndMark(m_out.load8ZeroExt32(byteWithMark));
    m_out.branch(hasNoBits(flagsAndMark, CompactTable::wasWalked).value(), unlikelyEdge(marks), normalEdge(isMarked));

    m_out.appendTo(marks);
    m_out.store32As8(m_out.bitOr(flagsAndMark.value(), m_out.constInt32(CompactTable::wasWalked)), byteWithMark);
    m_out.jump(isMarked);

    m_out.appendTo(isMarked);
    TableEntry entry = reader.compactEntryAt(kind, keyWidth, storage, column, entryIndex);
    join.add(boolean(true), low32(Integer(entryIndex)), entry);

    m_out.appendTo(isOrdinary);
    LValue data = reader.dataOf(storage);
    LValue usedInStorage = m_out.zeroExtPtr(m_out.add(m_out.load32(reader.dataAt(data, JSMap::Helper::aliveEntryCountIndex())), m_out.load32(reader.dataAt(data, JSMap::Helper::deletedEntryCountIndex()))));
    LValue entries = reader.entriesOf(data);
    LValue deletedValue = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, vm().value(), VM::offsetOfOrderedHashTableDeletedValue()));
    LValue entryIndexInStorage = reader.firstAliveEntry(start.value(), usedInStorage, hasNoEntry, [&](LValue candidate) {
        return m_out.notEqual(m_out.load64(reader.dataAt(reader.entryAt(kind, entries, candidate), 0)), deletedValue);
    });
    TableEntry entryInStorage = reader.storageEntryAt(kind, entries, entryIndexInStorage);
    join.add(boolean(true), low32(Integer(entryIndexInStorage)), entryInStorage);

    m_out.appendTo(hasNoEntry);
    join.add(boolean(false), word32(0), { undefined(), undefined() });

    m_out.appendTo(slowPath);
    addStepByOperation(join);

    auto results = join.finish();
    defineResults(value, std::span<const Operand>(results).first(numberOfResults));
}

void CollectionIntrinsicEmitter::generateMapNext(const ValueContext& value)
{
    generateNext(Collection::Map, value);
}

void CollectionIntrinsicEmitter::generateSetNext(const ValueContext& value)
{
    generateNext(Collection::Set, value);
}

void CollectionIntrinsicEmitter::generateCollectionTable(const ValueContext& value)
{
    LValue storage = TableReader(generator()).storageOf(in<CellPointer>(value->collectionOperand()));
    define(value, Boxed(m_out.select(m_out.notNull(storage), storage, m_out.constInt64(JSValue::encode(jsNull())))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
