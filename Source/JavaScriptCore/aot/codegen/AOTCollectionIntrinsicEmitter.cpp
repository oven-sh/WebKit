/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCollectionIntrinsicEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
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
        Condition isReference = isCellReference(word);
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

    TableEntry compactEntryAt(Collection kind, LValue table, LValue keys, LValue entryIndex)
    {
        Boxed key = decodeNarrow(NarrowReference(m_out.load32(elementAt(keys, entryIndex, 2))));
        if (kind == Collection::Set)
            return { key, undefined() };
        LBasicBlock isNarrow = m_out.newBlock();
        LBasicBlock isWide = m_out.newBlock();
        Join<Boxed> join(m_out);
        LValue values = m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(tableField(table, CompactTable::offsetOfCapacity()))), m_out.constInt32(2)));
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
    TableReader reader(generator());
    LBasicBlock isCurrent = newBlock();
    LBasicBlock isChanged = newBlock();
    LBasicBlock isCompact = newBlock();
    LBasicBlock isOrdinary = newBlock();
    LBasicBlock hasNoEntry = newBlock();
    LBasicBlock slowPath = newBlock();
    StepJoin join(m_out);
    CellPointer owner = in<CellPointer>(kind == Collection::Map ? value->mapOperand() : value->setOperand());
    Boxed table = in<Boxed>(value->tableOperand());
    Integer start = integerOf(value->entryOperand());
    LValue storage = reader.storageOf(owner);
    m_out.branch(m_out.equal(storage, table.value()), normalEdge(isCurrent), unlikelyEdge(isChanged));

    m_out.appendTo(isChanged);
    m_out.branch(m_out.isNull(storage), normalEdge(hasNoEntry), rareEdge(slowPath));

    m_out.appendTo(isCurrent);
    m_out.branch(hasCellType(CellPointer(storage), JSCellButterflyType).value(), unlikelyEdge(isOrdinary), normalEdge(isCompact));

    m_out.appendTo(isCompact);
    Word32 flags(m_out.load8ZeroExt32(reader.tableField(storage, CompactTable::offsetOfKeysShift())));
    branchUnless(hasNoBits(flags, CompactTable::keysAreWide | CompactTable::keysAreInImage), slowPath);
    LValue used = m_out.zeroExtPtr(m_out.load32(reader.tableField(storage, CompactTable::offsetOfUsed())));
    LValue keys = reader.keysOf(storage);
    LValue entryIndex = reader.firstAliveEntry(start.value(), used, hasNoEntry, [&](LValue candidate) {
        return m_out.notZero32(m_out.load32(reader.elementAt(keys, candidate, 2)));
    });
    TableEntry entry = reader.compactEntryAt(kind, storage, keys, entryIndex);
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

    auto results = join.finish();
    defineResults(value, std::span<const Operand>(results).first(kind == Collection::Map ? numberOfEnumerators<MapNextResult> : numberOfEnumerators<SetNextResult>));
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
