/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCollections.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTOperationData.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsHashTables.h"
#include "AOTVector.h"
#include "CompleteSubspace.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include "PreciseAllocation.h"
#include "WeakMapImpl.h"
#include <optional>

namespace JSC { namespace AOT {

static_assert(static_cast<int32_t>(CompactTable::Width::Wide) == 8 && !(static_cast<int32_t>(CompactTable::Width::Narrow) & 8));
static_assert((MarkedBlock::atomSize - 1) & PreciseAllocation::halfAlignment);
static_assert((1u << cell32Shift) <= MarkedBlock::atomSize);
static_assert(sizeof(JSMap) == 3 * sizeof(uint64_t));
static_assert(exists(mapGet(KeyComparison::Bits)) && exists(mapHas(KeyComparison::Bits)) && exists(setHas(KeyComparison::Bits)) && exists(mapSet(KeyComparison::Bits)) && exists(setAdd(KeyComparison::Bits)));
static_assert(!exists(mapGet(KeyComparison::Bits).withForm(static_cast<uint8_t>(KeyComparison::Bits) + 1)));
static_assert(exists(mapDelete(KeyComparison::Bits)) && exists(setDelete(KeyComparison::Bits)) && exists(newCompactTable(CompactTableOwner::Set)));
static_assert(!JSValue::ValueEmpty && !absentCell32);

enum class WeakTableLookup : uint8_t { MapGet, MapHas, SetHas };
enum class InsertionPoint : uint8_t { AmongTags, AtShortSlot, AtLongSlot };

struct ColumnEntry {
    GPRReg column;
    GPRReg entry;
};

static StubSignature signatureOfLookup()
{
    return StubSignature::withGPRResult({ thisGPR, argumentGPR(0) });
}

static KeyComparison keyComparisonOf(const StubAssembler& jit)
{
    return static_cast<KeyComparison>(jit.key().form());
}

template<typename Functor>
static void emitAtExit(StubAssembler& jit, JumpList& exit, const Functor& emitEnd)
{
    if (exit.empty())
        return;
    exit.link(&jit);
    emitEnd();
}

static void returnConstant(StubAssembler& jit, JSValue value)
{
    jit.move(TrustedImm64(JSValue::encode(value)), jit.result());
    jit.returnToCaller();
}

static void returnOwner(StubAssembler& jit, const HashTableRegisters& registers)
{
    jit.move(registers.owner, jit.result());
    jit.returnToCaller();
}

static void emitCallTableOperation(StubAssembler& jit, const HashTableRegisters& registers, Entry entry, std::optional<GPRReg> value)
{
    if (value)
        jit.move(*value, operationArgumentGPR(3));
    jit.move(registers.key, operationArgumentGPR(2));
    jit.move(registers.owner, operationArgumentGPR(1));
    jit.callOperationAndReturn(entry, { });
}

static void emitBarrierAndReturnOwner(StubAssembler& jit, const HashTableRegisters& registers, GPRReg cell)
{
    ASSERT(registers.owner != gprOf(StubRegister::T9));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, cell, registers.candidate);
    returnOwner(jit, registers);

    needsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.move(cell, gprOf(firstStubOperandRegister));
    jit.callStub(writeBarrier());
    jit.move(registers.owner, jit.result());
    jit.emitFunctionEpilogue();
    jit.ret();
}

static Jump branchIfKeysAreWide(StubAssembler& jit, const HashTableRegisters& registers)
{
    return jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
}

static Jump branchIfValuesAreWide(StubAssembler& jit, const HashTableRegisters& registers)
{
    return jit.branch8(CCallHelpers::NotEqual, Address(registers.table, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Narrow)));
}

// A narrow column holds cells of marked blocks, undefined, null and booleans.
static void emitBranchUnlessNarrowColumnHolds(StubAssembler& jit, GPRReg value, JumpList& doesNotHold)
{
    Jump isCell = jit.branchIfCell(value);
    doesNotHold.append(jit.branch64(CCallHelpers::AboveOrEqual, value, TrustedImm32(minCell32)));
    Jump isImmediate = jit.jump();
    isCell.link(&jit);
    doesNotHold.append(jit.branchTest64(CCallHelpers::NonZero, value, TrustedImm32(MarkedBlock::atomSize - 1)));
    isImmediate.link(&jit);
}

// Without a list the caller knows that a narrow column holds the value.
static void emitEncodeForNarrowColumn(StubAssembler& jit, GPRReg value, GPRReg word, JumpList* doesNotFit)
{
    jit.move(value, word);
    Jump isCell = jit.branchIfCell(word);
    if (doesNotFit)
        doesNotFit->append(jit.branch64(CCallHelpers::AboveOrEqual, word, TrustedImm32(minCell32)));
    Jump isEncoded = jit.jump();
    isCell.link(&jit);
    if (doesNotFit)
        doesNotFit->append(jit.branchTest64(CCallHelpers::NonZero, word, TrustedImm32(MarkedBlock::atomSize - 1)));
    encodeCell32(jit, word, word);
    isEncoded.link(&jit);
}

// isWide is taken for a column of 64-bit words. Nothing is stored when the value does not fit.
static void emitStoreInColumn(StubAssembler& jit, Jump isWide, ColumnEntry at, GPRReg value, GPRReg word, JumpList* doesNotFit)
{
    emitEncodeForNarrowColumn(jit, value, word, doesNotFit);
    jit.store32(word, BaseIndex(at.column, at.entry, CCallHelpers::TimesFour));
    Jump isStored = jit.jump();

    isWide.link(&jit);
    jit.store64(value, BaseIndex(at.column, at.entry, CCallHelpers::TimesEight));
    isStored.link(&jit);
}

static void emitClearInColumn(StubAssembler& jit, Jump isWide, ColumnEntry at)
{
    jit.store32(TrustedImm32(0), BaseIndex(at.column, at.entry, CCallHelpers::TimesFour));
    Jump isCleared = jit.jump();

    isWide.link(&jit);
    jit.store64(TrustedImm32(0), BaseIndex(at.column, at.entry, CCallHelpers::TimesEight));
    isCleared.link(&jit);
}

static void emitLoadValueOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers, JumpList& keysAreNarrow)
{
    const GPRReg result = jit.result();
    emitLoadValuesOfCompactTable(jit, registers, registers.keys, &keysAreNarrow);
    Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Wide)));
    jit.load32(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesFour), result);
    Jump isNotCell = jit.branch32(CCallHelpers::Below, result, TrustedImm32(minCell32));
    decodeCell32(jit, result, result);
    isNotCell.link(&jit);
    jit.returnToCaller();

    isWide.link(&jit);
    jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), result);
    jit.returnToCaller();
}

static void emitStoreValueOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers, GPRReg value, JumpList* doesNotFit, JumpList* keysAreNarrow = nullptr)
{
    const GPRReg values = registers.keys;
    emitLoadValuesOfCompactTable(jit, registers, values, keysAreNarrow);
    emitStoreInColumn(jit, branchIfValuesAreWide(jit, registers), { values, registers.entry }, value, registers.candidate, doesNotFit);
}

static void emitStoreInIndex(StubAssembler& jit, const HashTableRegisters& registers, InsertionPoint point)
{
    switch (point) {
    case InsertionPoint::AmongTags:
        jit.urshift32(TrustedImm32(CompactTable::tagShiftOfTags), registers.hash);
        jit.store8(registers.hash, BaseIndex(registers.table, registers.entry, CCallHelpers::TimesOne, CompactTable::offsetOfIndex()));
        return;
    case InsertionPoint::AtShortSlot:
        jit.or32(registers.entry, registers.hash);
        jit.store16(registers.hash, Address(registers.cursor));
        return;
    case InsertionPoint::AtLongSlot:
        jit.or32(registers.entry, registers.hash);
        jit.store32(registers.hash, Address(registers.cursor));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nothing is stored before the last exit to isFull, a table without room, or to cannotAppend, a column that is too narrow for the key or the value.
static void emitAppendToCompactTable(StubAssembler& jit, const HashTableRegisters& registers, InsertionPointExits& insertionPoints, std::optional<GPRReg> value, JumpList& isFull, JumpList& cannotAppend)
{
    const GPRReg table = registers.table;
    const GPRReg entry = registers.entry;
    const bool keyFitsEveryColumn = keyComparisonOf(jit) == KeyComparison::String;
    auto storeKeyAt = [&](InsertionPoint point, JumpList& jumps) {
        jumps.link(&jit);
        jit.load32(Address(table, CompactTable::offsetOfUsed()), entry);
        isFull.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, CompactTable::offsetOfCapacity())));
        if (value) {
            Jump valuesAreWide = branchIfValuesAreWide(jit, registers);
            emitBranchUnlessNarrowColumnHolds(jit, *value, cannotAppend);
            valuesAreWide.link(&jit);
        }
        emitStoreInColumn(jit, branchIfKeysAreWide(jit, registers), { registers.keys, entry }, registers.key, registers.candidate, keyFitsEveryColumn ? nullptr : &cannotAppend);
        emitStoreInIndex(jit, registers, point);
    };
    if (insertionPoints.amongTags.empty())
        return;

    JumpList isIndexed;
    storeKeyAt(InsertionPoint::AmongTags, insertionPoints.amongTags);
    isIndexed.append(jit.jump());
    storeKeyAt(InsertionPoint::AtLongSlot, insertionPoints.atLongSlot);
    isIndexed.append(jit.jump());
    storeKeyAt(InsertionPoint::AtShortSlot, insertionPoints.atShortSlot);

    isIndexed.link(&jit);
    if (value)
        emitStoreValueOfCompactTable(jit, registers, *value, nullptr);
    // A marker visits the entries below the count that it reads.
    fenceAfterAllocation(jit);
    jit.add32(TrustedImm32(1), entry);
    jit.store32(entry, Address(table, CompactTable::offsetOfUsed()));
    jit.add32(TrustedImm32(1), Address(table, CompactTable::offsetOfAliveCount()));
    emitBarrierAndReturnOwner(jit, registers, table);
}

static void emitInsertAbsentKey(StubAssembler& jit, const HashTableRegisters& registers, HashTableExits& exits, CompactTableOwner owner, std::optional<GPRReg> value)
{
    RELEASE_ASSERT(exits.absent.empty() && value.has_value() == (owner == CompactTableOwner::Map));
    JumpList isFull;
    emitAppendToCompactTable(jit, registers, exits.insertionPoints, value, isFull, exits.unknown);
    if (value) {
        checkStubOperands(newCompactTable(owner), { registers.owner, registers.key, registers.hash, *value });
        checkStubOperands(growCompactTable(owner), { registers.owner, registers.key, *value });
    } else {
        checkStubOperands(newCompactTable(owner), { registers.owner, registers.key, registers.hash });
        checkStubOperands(growCompactTable(owner), { registers.owner, registers.key });
    }
    jit.linkToStub(exits.hasNoStorage, newCompactTable(owner));
    jit.linkToStub(isFull, growCompactTable(owner));
    emitAtExit(jit, exits.unknown, [&] {
        emitCallTableOperation(jit, registers, owner == CompactTableOwner::Map ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd, value);
    });
}

static void emitHas(StubAssembler& jit, Entry entry)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = true }, exits);
    exits.foundInCompactTable.append(exits.foundAmongNarrowKeys);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        returnConstant(jit, jsBoolean(true));
    });
    emitAtExit(jit, exits.absent, [&] {
        returnConstant(jit, jsBoolean(false));
    });
    emitAtExit(jit, exits.unknown, [&] {
        emitCallTableOperation(jit, registers, entry, std::nullopt);
    });
}

static void emitLookupInWeakTable(StubAssembler& jit, WeakTableLookup lookup)
{
    using MapBucket = WeakMapBucket<WeakMapBucketDataKeyValue>;
    using SetBucket = WeakMapBucket<WeakMapBucketDataKey>;
    static_assert(sizeof(MapBucket) == 2 * sizeof(EncodedJSValue) && sizeof(SetBucket) == sizeof(EncodedJSValue));
    static_assert(JSWeakMap::offsetOfBuffer() == JSWeakSet::offsetOfBuffer() && JSWeakMap::offsetOfCapacity() == JSWeakSet::offsetOfCapacity() && MapBucket::offsetOfKey() == SetBucket::offsetOfKey());
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    const GPRReg result = jit.result();
    const GPRReg table = registers.owner;
    const GPRReg key = registers.key;
    const GPRReg hash = registers.hash;
    const GPRReg other = registers.table;
    const GPRReg buffer = registers.cursor;
    const GPRReg mask = registers.keys;
    const GPRReg bucket = registers.candidate;
    bool isMap = lookup != WeakTableLookup::SetHas;
    Jump notCell = jit.branchIfNotCell(key);
    emitHashOfBits(jit, key, hash, other, bucket);
    jit.loadPtr(Address(table, JSWeakMap::offsetOfBuffer()), buffer);
    jit.load32(Address(table, JSWeakMap::offsetOfCapacity()), mask);
    jit.sub32(TrustedImm32(1), mask);
    Label loop = jit.label();
    jit.and32(mask, hash);
    if (isMap)
        jit.lshift32(hash, TrustedImm32(1), bucket);
    else
        jit.move(hash, bucket);
    jit.loadPtr(BaseIndex(buffer, bucket, CCallHelpers::TimesEight, MapBucket::offsetOfKey()), other);
    Jump found = jit.branchPtr(CCallHelpers::Equal, other, key);
    jit.add32(TrustedImm32(1), hash);
    jit.branchTestPtr(CCallHelpers::NonZero, other).linkTo(loop, &jit);

    notCell.link(&jit);
    returnConstant(jit, lookup == WeakTableLookup::MapGet ? jsUndefined() : jsBoolean(false));

    found.link(&jit);
    if (lookup != WeakTableLookup::MapGet) {
        returnConstant(jit, jsBoolean(true));
        return;
    }
    jit.load64(BaseIndex(buffer, bucket, CCallHelpers::TimesEight, MapBucket::offsetOfValue()), result);
    jit.returnToCaller();
}

StubSignature signatureOfMapGet(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfMapHas(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfSetHas(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfSetAdd(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfMapDelete(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfSetDelete(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfWeakMapGet(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfWeakMapHas(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfWeakSetHas(StubKey) { return signatureOfLookup(); }
StubSignature signatureOfMapSet(StubKey) { return StubSignature::withGPRResult({ thisGPR, argumentGPR(0), argumentGPR(1) }); }
StubSignature signatureOfNewMap(StubKey) { return StubSignature::withGPRResult({ }); }
StubSignature signatureOfNewSet(StubKey) { return StubSignature::withGPRResult({ }); }

void generateMapGet(StubAssembler& jit)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = true }, exits);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        emitLoadValueOfCompactTable(jit, registers, exits.foundAmongNarrowKeys);
    });
    emitAtExit(jit, exits.absent, [&] {
        returnConstant(jit, jsUndefined());
    });
    emitAtExit(jit, exits.unknown, [&] {
        emitCallTableOperation(jit, registers, Entry::operationAOTMapGet, std::nullopt);
    });
}

void generateMapHas(StubAssembler& jit)
{
    emitHas(jit, Entry::operationAOTMapHas);
}

void generateSetHas(StubAssembler& jit)
{
    emitHas(jit, Entry::operationAOTSetHas);
}

void generateMapSet(StubAssembler& jit)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    const GPRReg value = jit.operand(2);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = false, .findsInsertionPoint = true }, exits);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        emitStoreValueOfCompactTable(jit, registers, value, &exits.unknown, &exits.foundAmongNarrowKeys);
        emitBarrierAndReturnOwner(jit, registers, registers.table);
    });
    emitInsertAbsentKey(jit, registers, exits, CompactTableOwner::Map, value);
}

void generateSetAdd(StubAssembler& jit)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = false, .findsInsertionPoint = true }, exits);
    exits.foundInCompactTable.append(exits.foundAmongNarrowKeys);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        returnOwner(jit, registers);
    });
    emitInsertAbsentKey(jit, registers, exits, CompactTableOwner::Set, std::nullopt);
}

static void emitDelete(StubAssembler& jit, CompactTableOwner owner)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = false }, exits);
    exits.foundInCompactTable.append(exits.foundAmongNarrowKeys);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        emitClearInColumn(jit, branchIfKeysAreWide(jit, registers), { registers.keys, registers.entry });
        if (owner == CompactTableOwner::Map) {
            const GPRReg values = registers.keys;
            emitLoadValuesOfCompactTable(jit, registers, values);
            emitClearInColumn(jit, branchIfValuesAreWide(jit, registers), { values, registers.entry });
        }
        jit.sub32(TrustedImm32(1), Address(registers.table, CompactTable::offsetOfAliveCount()));
        returnConstant(jit, jsBoolean(true));
    });
    emitAtExit(jit, exits.absent, [&] {
        returnConstant(jit, jsBoolean(false));
    });
    emitAtExit(jit, exits.unknown, [&] {
        emitCallTableOperation(jit, registers, owner == CompactTableOwner::Map ? Entry::operationAOTMapDelete : Entry::operationAOTSetDelete, std::nullopt);
    });
}

void generateMapDelete(StubAssembler& jit)
{
    emitDelete(jit, CompactTableOwner::Map);
}

void generateSetDelete(StubAssembler& jit)
{
    emitDelete(jit, CompactTableOwner::Set);
}

static void emitStoreFirstEntry(StubAssembler& jit, const HashTableRegisters& registers, CompactTable::Width width, ptrdiff_t offsetOfColumn, GPRReg value)
{
    const Address first(registers.table, static_cast<int32_t>(offsetOfColumn));
    switch (width) {
    case CompactTable::Width::Narrow:
        emitEncodeForNarrowColumn(jit, value, registers.candidate, nullptr);
        jit.store32(registers.candidate, first);
        return;
    case CompactTable::Width::Wide:
        jit.store64(value, first);
        return;
    case CompactTable::Width::None:
    case CompactTable::Width::InImage:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// hash has the tag of the key. Never falls through.
static void emitCreateTableWithEntry(StubAssembler& jit, const HashTableRegisters& registers, CompactTable::Width keyWidth, CompactTable::Width valueWidth, std::optional<GPRReg> value, JumpList& isCreated, JumpList& slowPath)
{
    constexpr uint64_t oneEntryUsedAndAlive = 1ull << 32 | 1;
    constexpr ptrdiff_t offsetOfKeys = CompactTable::offsetOfIndex() + CompactTable::maxCapacityWithTags;
    static_assert(CompactTable::offsetOfAliveCount() == CompactTable::offsetOfUsed() + 4 && !(CompactTable::offsetOfUsed() % sizeof(uint64_t)) && !(CompactTable::offsetOfCapacity() % sizeof(uint64_t)));
    const ptrdiff_t offsetOfValues = offsetOfKeys + CompactTable::initialCapacity * static_cast<ptrdiff_t>(keyWidth);
    const size_t size = CompactTable::initialSize(keyWidth, valueWidth);
    const GPRReg table = registers.table;
    const GPRReg allocator = registers.cursor;
    const GPRReg scratch = registers.keys;
    RELEASE_ASSERT(!(size % sizeof(uint64_t)) && value.has_value() == (valueWidth != CompactTable::Width::None));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(Address(allocator, CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(size) * sizeof(Allocator)), allocator);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, table, allocator, scratch, slowPath);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.compactTableHeader)), scratch);
    jit.store64(scratch, Address(table));
    jit.store64(TrustedImm64(oneEntryUsedAndAlive), Address(table, CompactTable::offsetOfUsed()));
    jit.store64(TrustedImm64(CompactTable::initialCapacityAndKinds(keyWidth, valueWidth)), Address(table, CompactTable::offsetOfCapacity()));
    jit.store64(registers.hash, Address(table, CompactTable::offsetOfIndex()));
    for (size_t offset = offsetOfKeys; offset < size; offset += sizeof(uint64_t))
        jit.store64(TrustedImm32(0), Address(table, static_cast<int32_t>(offset)));
    emitStoreFirstEntry(jit, registers, keyWidth, offsetOfKeys, registers.key);
    if (value)
        emitStoreFirstEntry(jit, registers, valueWidth, offsetOfValues, *value);
    isCreated.append(jit.jump());
}

static constexpr GPRReg ownerAfterLookup = hashTableRegisterPlan.copyOfOwner ? gprOf(*hashTableRegisterPlan.copyOfOwner) : thisGPR;

StubSignature signatureOfNewCompactTable(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ ownerAfterLookup, argumentGPR(0), gprOf(hashTableRegisterPlan.hash) });
    if (static_cast<CompactTableOwner>(key.form()) == CompactTableOwner::Map)
        signature.addOperand(argumentGPR(1));
    return signature;
}

// The columns of the first table are as wide as the first entry needs them.
void generateNewCompactTable(StubAssembler& jit)
{
    using Width = CompactTable::Width;
    constexpr HashTableRegisterPlan plan = hashTableRegisterPlan;
    const auto owner = static_cast<CompactTableOwner>(jit.key().form());
    const HashTableRegisters registers {
        .owner = jit.operand(0),
        .key = jit.operand(1),
        .hash = jit.clobberOperand(2),
        .table = jit.allocateScratch(plan.table),
        .cursor = jit.allocateScratch(plan.cursor),
        .keys = jit.allocateScratch(plan.keys),
        .entry = jit.allocateScratch(plan.entry),
        .candidate = jit.allocateScratch(plan.candidate),
    };
    JumpList isCreated;
    JumpList slowPath;
    JumpList keyIsWide;

    jit.urshift32(TrustedImm32(CompactTable::tagShiftOfTags), registers.hash);
    emitBranchUnlessNarrowColumnHolds(jit, registers.key, keyIsWide);
    std::optional<GPRReg> value;
    switch (owner) {
    case CompactTableOwner::Set:
        emitCreateTableWithEntry(jit, registers, Width::Narrow, Width::None, value, isCreated, slowPath);
        keyIsWide.link(&jit);
        emitCreateTableWithEntry(jit, registers, Width::Wide, Width::None, value, isCreated, slowPath);
        break;
    case CompactTableOwner::Map: {
        value = jit.operand(3);
        JumpList valueIsWide;
        emitBranchUnlessNarrowColumnHolds(jit, *value, valueIsWide);
        emitCreateTableWithEntry(jit, registers, Width::Narrow, Width::Narrow, value, isCreated, slowPath);
        valueIsWide.link(&jit);
        emitCreateTableWithEntry(jit, registers, Width::Narrow, Width::Wide, value, isCreated, slowPath);

        keyIsWide.link(&jit);
        JumpList valueOfWideKeyIsWide;
        emitBranchUnlessNarrowColumnHolds(jit, *value, valueOfWideKeyIsWide);
        emitCreateTableWithEntry(jit, registers, Width::Wide, Width::Narrow, value, isCreated, slowPath);
        valueOfWideKeyIsWide.link(&jit);
        emitCreateTableWithEntry(jit, registers, Width::Wide, Width::Wide, value, isCreated, slowPath);
        break;
    }
    }

    isCreated.link(&jit);
    fenceAfterAllocation(jit);
    jit.storePtr(registers.table, Address(registers.owner, offsetOfHashTableStorage()));
    emitBarrierAndReturnOwner(jit, registers, registers.owner);

    slowPath.link(&jit);
    emitCallTableOperation(jit, registers, owner == CompactTableOwner::Map ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd, value);
}

StubSignature signatureOfGrowCompactTable(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ ownerAfterLookup, argumentGPR(0) });
    if (static_cast<CompactTableOwner>(key.form()) == CompactTableOwner::Map)
        signature.addOperand(argumentGPR(1));
    return signature;
}

// from and to end behind what is copied. There is at least one word.
static void emitCopyWords(StubAssembler& jit, GPRReg from, GPRReg to, GPRReg endOfFrom, GPRReg scratch)
{
    Label loop = jit.label();
    jit.load64(Address(from), scratch);
    jit.store64(scratch, Address(to));
    jit.addPtr(TrustedImm32(sizeof(uint64_t)), from);
    jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
    jit.branchPtr(CCallHelpers::Below, from, endOfFrom).linkTo(loop, &jit);
}

static void emitFillWords(StubAssembler& jit, GPRReg to, GPRReg end, int32_t word)
{
    Jump isDone = jit.branchPtr(CCallHelpers::AboveOrEqual, to, end);
    Label loop = jit.label();
    jit.store64(TrustedImm32(word), Address(to));
    jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
    jit.branchPtr(CCallHelpers::Below, to, end).linkTo(loop, &jit);
    isDone.link(&jit);
}

// The owner's table is full and lacks the key.
void generateGrowCompactTable(StubAssembler& jit)
{
    using ShortSlots = CompactTable::SlotFormat<CompactTable::SlotKind::Short>;
    constexpr HashTableRegisterPlan plan = hashTableRegisterPlan;
    constexpr int32_t index = CompactTable::offsetOfIndex();
    static_assert(CompactTable::growthPlansPerValueWidth * sizeof(uint64_t) == static_cast<unsigned>(CompactTable::Width::Narrow) << 6);
    static_assert(sizeof(Allocator) == sizeof(uint64_t) && !static_cast<uint8_t>(CompactTable::Status::Live) && sizeof(ShortSlots::Slot) == 2);
    static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfCapacity() + 5);
    const auto kindOfOwner = static_cast<CompactTableOwner>(jit.key().form());
    const HashTableRegisters registers {
        .owner = jit.operand(0),
        .key = jit.operand(1),
        .hash = jit.allocateScratch(plan.hash),
        .table = jit.allocateScratch(plan.table),
        .cursor = jit.allocateScratch(plan.cursor),
        .keys = jit.allocateScratch(plan.keys),
        .entry = jit.allocateScratch(plan.entry),
        .candidate = jit.allocateScratch(plan.candidate),
    };
    const GPRReg owner = registers.owner;
    const GPRReg key = registers.key;
    const GPRReg table = registers.table;
    const GPRReg larger = registers.keys;
    const GPRReg count = registers.entry;
    const GPRReg to = registers.hash;
    const GPRReg scratch = registers.cursor;
    const GPRReg end = registers.candidate;
    // Once the index is written the full table is named by its owner alone.
    const GPRReg from = table;
    std::optional<GPRReg> value;
    if (kindOfOwner == CompactTableOwner::Map)
        value = jit.operand(2);
    const GPRReg slot = value ? *value : jit.allocateScratch(StubRegister::A1);
    JumpList givesUp;

    if (jit.options().useWideReferences) {
        emitCallTableOperation(jit, registers, value ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd, value);
        return;
    }

    {
        const GPRReg growthPlan = to;
        const GPRReg allocator = scratch;
        jit.loadPtr(Address(owner, offsetOfHashTableStorage()), table);
        givesUp.append(jit.branchIfNotCell(key));
        givesUp.append(jit.branchIfNotType(key, StringType));
        jit.load32(Address(table, CompactTable::offsetOfUsed()), count);
        givesUp.append(jit.branch32(CCallHelpers::NotEqual, count, Address(table, CompactTable::offsetOfAliveCount())));
        givesUp.append(jit.branch32(CCallHelpers::AboveOrEqual, count, TrustedImm32(CompactTable::maxCountWithGrowthPlan)));
        givesUp.append(branchIfKeysAreWide(jit, registers));
        // No marker reads a table meanwhile.
        givesUp.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))));
        if (value) {
            Jump valuesAreWide = branchIfValuesAreWide(jit, registers);
            emitBranchUnlessNarrowColumnHolds(jit, *value, givesUp);
            valuesAreWide.link(&jit);
        }
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.compactTableGrowthPlans)), scratch);
        givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
        jit.load8(Address(table, CompactTable::offsetOfValueWidth()), growthPlan);
        jit.lshift32(TrustedImm32(6), growthPlan);
        jit.add64(scratch, growthPlan);
        jit.load64(BaseIndex(growthPlan, count, CCallHelpers::TimesFour), growthPlan);

        jit.urshift64(growthPlan, TrustedImm32(CompactTable::sizeStepShiftOfGrowthPlan), allocator);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), end);
        jit.loadPtr(BaseIndex(end, allocator, CCallHelpers::TimesEight, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
        givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        allocateCell(jit, larger, allocator, end, givesUp);

        jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.compactTableHeader)), scratch);
        jit.store64(scratch, Address(larger));
        jit.store64(TrustedImm32(0), Address(larger, CompactTable::offsetOfUsed()));
        jit.lshift64(TrustedImm32(64 - CompactTable::sizeStepShiftOfGrowthPlan), growthPlan);
        jit.urshift64(TrustedImm32(64 - CompactTable::sizeStepShiftOfGrowthPlan), growthPlan);
        // Whoever stands in the full table goes on at the same entry of the larger one.
        jit.load8(Address(table, CompactTable::offsetOfKeysShift()), scratch);
        jit.and32(TrustedImm32(CompactTable::wasWalked), scratch);
        jit.lshift64(TrustedImm32(8 * 5), scratch);
        jit.or64(scratch, growthPlan);
        jit.store64(growthPlan, Address(larger, CompactTable::offsetOfCapacity()));
    }

    auto loadSizeOfIndex = [&](GPRReg shift, GPRReg size) {
        jit.move(TrustedImm32(2), size);
        jit.lshift64(shift, size);
    };
    JumpList hasIndex;
    jit.addPtr(TrustedImm32(index), larger, to);
    jit.load8(Address(larger, CompactTable::offsetOfIndexBits()), scratch);
    Jump getsSlots = jit.branchTest32(CCallHelpers::NonZero, scratch);
    jit.loadPtr(Address(key, JSString::offsetOfValue()), end);
    jit.load32(Address(end, StringImpl::flagsOffset()), end);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount + CompactTable::tagShiftOfTags), end);
    jit.lshift32(count, TrustedImm32(3), scratch);
    jit.lshift64(scratch, end);
    jit.load64(Address(table, index), scratch);
    jit.or64(scratch, end);
    jit.store64(end, Address(to));
    jit.addPtr(TrustedImm32(CompactTable::maxCapacityWithTags), to);
    jit.addPtr(TrustedImm32(index + CompactTable::maxCapacityWithTags), table, from);
    hasIndex.append(jit.jump());

    // An index of short slots has 2 << indexBits bytes. Entries keep their numbers, and an index of the same size has them at the same positions.
    getsSlots.link(&jit);
    jit.load8(Address(table, CompactTable::offsetOfIndexBits()), end);
    Jump keepsSlots = jit.branch32(CCallHelpers::Equal, end, scratch);
    loadSizeOfIndex(scratch, end);
    jit.add64(to, end);
    emitFillWords(jit, to, end, -1);
    jit.load8(Address(table, CompactTable::offsetOfKeysShift()), scratch);
    jit.and32(TrustedImm32(CompactTable::keysShiftMask), scratch);
    loadSizeOfIndex(scratch, end);
    jit.add64(end, table, from);
    jit.addPtr(TrustedImm32(index), from);
    hasIndex.append(jit.jump());

    keepsSlots.link(&jit);
    loadSizeOfIndex(scratch, end);
    jit.addPtr(TrustedImm32(index), table, from);
    jit.add64(from, end);
    emitCopyWords(jit, from, to, end, scratch);

    auto fillRestOfColumn = [&](CCallHelpers::Scale scale) {
        jit.load32(Address(larger, CompactTable::offsetOfCapacity()), scratch);
        jit.sub32(count, scratch);
        jit.getEffectiveAddress(BaseIndex(to, scratch, scale), end);
        jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
        emitFillWords(jit, to, end, 0);
    };
    hasIndex.link(&jit);
    jit.getEffectiveAddress(BaseIndex(from, count, CCallHelpers::TimesFour), end);
    emitCopyWords(jit, from, to, end, scratch);
    encodeCell32(jit, key, scratch);
    jit.zeroExtend32ToWord(scratch, scratch);
    jit.store64(scratch, Address(to));
    fillRestOfColumn(CCallHelpers::TimesFour);
    if (value) {
        Jump valuesAreWide = jit.branchTest8(CCallHelpers::NonZero, Address(larger, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Wide)));
        jit.getEffectiveAddress(BaseIndex(from, count, CCallHelpers::TimesFour), end);
        emitCopyWords(jit, from, to, end, scratch);
        emitEncodeForNarrowColumn(jit, *value, scratch, nullptr);
        jit.zeroExtend32ToWord(scratch, scratch);
        jit.store64(scratch, Address(to));
        fillRestOfColumn(CCallHelpers::TimesFour);
        Jump hasValues = jit.jump();

        valuesAreWide.link(&jit);
        jit.getEffectiveAddress(BaseIndex(from, count, CCallHelpers::TimesEight), end);
        emitCopyWords(jit, from, to, end, scratch);
        jit.store64(*value, Address(to));
        fillRestOfColumn(CCallHelpers::TimesEight);
        hasValues.link(&jit);
    }

    jit.loadPtr(Address(owner, offsetOfHashTableStorage()), table);
    jit.load8(Address(larger, CompactTable::offsetOfIndexBits()), scratch);
    Jump hasTags = jit.branchTest32(CCallHelpers::Zero, scratch);
    {
        const GPRReg entry = to;
        const GPRReg mask = end;
        const GPRReg keys = table;
        const GPRReg position = scratch;
        const GPRReg hash = key;
        jit.move(count, entry);
        jit.load8(Address(table, CompactTable::offsetOfIndexBits()), mask);
        Jump hasOtherEntries = jit.branch32(CCallHelpers::Equal, mask, scratch);
        jit.move(TrustedImm32(0), entry);
        hasOtherEntries.link(&jit);
        jit.move(TrustedImm32(1), mask);
        jit.lshift32(scratch, mask);
        jit.getEffectiveAddress(BaseIndex(larger, mask, CCallHelpers::TimesTwo, index), keys);
        jit.sub32(TrustedImm32(1), mask);

        Vector<GPRReg, 2> saved { key };
        if (value)
            saved.append(*value);
        jit.preserving(saved.span(), [&] {
            jit.recordClobbers(RegisterSet { hash, slot });
            JumpList isNotString;
            Label nextEntry = jit.label();
            jit.load32(BaseIndex(keys, entry, CCallHelpers::TimesFour), hash);
            isNotString.append(jit.branch32(CCallHelpers::Below, hash, TrustedImm32(minCell32)));
            decodeCell32(jit, hash, hash);
            isNotString.append(jit.branchIfNotType(hash, StringType));
            jit.loadPtr(Address(hash, JSString::offsetOfValue()), hash);
            jit.load32(Address(hash, StringImpl::flagsOffset()), hash);
            jit.urshift32(TrustedImm32(StringImpl::s_flagCount), hash);
            jit.and32(hash, mask, position);
            Label probe = jit.label();
            jit.load16(BaseIndex(larger, position, CCallHelpers::TimesTwo, index), slot);
            Jump isFree = jit.branch32(CCallHelpers::Equal, slot, TrustedImm32(ShortSlots::freeSlot));
            jit.add32(TrustedImm32(1), position);
            jit.and32(mask, position);
            jit.jump().linkTo(probe, &jit);

            isFree.link(&jit);
            jit.urshift32(TrustedImm32(ShortSlots::tagShift), hash);
            jit.lshift32(TrustedImm32(ShortSlots::entryBits), hash);
            jit.or32(entry, hash);
            jit.store16(hash, BaseIndex(larger, position, CCallHelpers::TimesTwo, index));
            jit.add32(TrustedImm32(1), entry);
            jit.branch32(CCallHelpers::BelowOrEqual, entry, count).linkTo(nextEntry, &jit);
            Jump isIndexed = jit.jump();

            // The larger table has no entry and nothing names it.
            isNotString.link(&jit);
            jit.move(TrustedImm32(0), count);
            isIndexed.link(&jit);
        });
        givesUp.append(jit.branchTest32(CCallHelpers::Zero, count));
        jit.loadPtr(Address(owner, offsetOfHashTableStorage()), table);
    }
    hasTags.link(&jit);

    jit.add32(TrustedImm32(1), count);
    jit.store32(count, Address(larger, CompactTable::offsetOfUsed()));
    jit.store32(count, Address(larger, CompactTable::offsetOfAliveCount()));
    jit.store8(TrustedImm32(static_cast<int32_t>(CompactTable::Status::Replaced)), Address(table, CompactTable::offsetOfStatus()));
    jit.storePtr(larger, Address(table, CompactTable::offsetOfUsed()));
    jit.store32(TrustedImm32(0), Address(table, CompactTable::offsetOfCapacity()));
    jit.storePtr(larger, Address(owner, offsetOfHashTableStorage()));
    Jump fullTableNeedsBarrier = branchIfBarrierIsNeeded(jit, table, registers.candidate);
    Label fullTableIsRemembered = jit.label();
    emitBarrierAndReturnOwner(jit, registers, owner);

    fullTableNeedsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.move(table, gprOf(firstStubOperandRegister));
    jit.callStub(writeBarrier());
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(fullTableIsRemembered, &jit);

    givesUp.link(&jit);
    emitCallTableOperation(jit, registers, value ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd, value);
}

StubSignature signatureOfMapNextFrontEnd(StubKey) { return operationFrontEndSignature(4); }
StubSignature signatureOfSetNextFrontEnd(StubKey) { return operationFrontEndSignature(4); }

// The operands are those of the operation: the owner, the table the walk stands in, the entry to go on at, and where the key and the value go.
static void emitNextFrontEnd(StubAssembler& jit, CompactTableOwner kind, Entry rawOperation)
{
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
    const bool hasValues = kind == CompactTableOwner::Map;
    const GPRReg owner = jit.operand(0);
    const GPRReg table = jit.operand(1);
    const GPRReg entry = jit.operand(2);
    const GPRReg results = jit.operand(3);
    const GPRReg storage = jit.allocateScratch(StubRegister::T9);
    const GPRReg column = jit.allocateScratch(StubRegister::T11);
    const GPRReg used = jit.allocateScratch(StubRegister::T12);
    const GPRReg word = jit.allocateScratch(isARM64() ? StubRegister::T13 : StubRegister::R0);
    RELEASE_ASSERT(noOverlap(owner, table, entry, results, storage, column, used, word));
    const Address flags(storage, CompactTable::offsetOfKeysShift());
    JumpList callsOperation;
    JumpList hasNoEntry;
    JumpList hasEntry;

    auto decodeNarrow = [&] {
        Jump isImmediate = jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32));
        decodeCell32(jit, word, word);
        isImmediate.link(&jit);
    };
    auto findUsedEntry = [&](NOESCAPE const Invocable<Jump()> auto& branchIfAlive) {
        Label loop = jit.label();
        hasNoEntry.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        Jump isAlive = branchIfAlive();
        jit.add32(TrustedImm32(1), entry);
        jit.jump().linkTo(loop, &jit);
        isAlive.link(&jit);
    };

    jit.zeroExtend32ToWord(entry, entry);
    jit.loadPtr(Address(owner, offsetOfHashTableStorage()), storage);
    Jump isCurrent = jit.branchPtr(CCallHelpers::Equal, storage, table);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, storage));
    hasNoEntry.append(jit.jump());

    isCurrent.link(&jit);
    Jump isOrdinary = jit.branch8(CCallHelpers::Equal, Address(storage, JSCell::typeInfoTypeOffset()), TrustedImm32(JSCellButterflyType));
    jit.load8(flags, word);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, word, TrustedImm32(CompactTable::keysAreInImage)));
    jit.and32(TrustedImm32(CompactTable::keysShiftMask), word);
    jit.move(TrustedImm32(2), column);
    jit.lshift64(word, column);
    jit.add64(storage, column);
    jit.addPtr(TrustedImm32(CompactTable::offsetOfIndex()), column);
    jit.load32(Address(storage, CompactTable::offsetOfUsed()), used);
    Jump keysAreWide = jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreWide));

    findUsedEntry([&] {
        jit.load32(BaseIndex(column, entry, CCallHelpers::TimesFour), word);
        return jit.branchTest32(CCallHelpers::NonZero, word);
    });
    decodeNarrow();
    if (hasValues) {
        jit.load32(Address(storage, CompactTable::offsetOfCapacity()), used);
        jit.getEffectiveAddress(BaseIndex(column, used, CCallHelpers::TimesFour), column);
    }
    Jump hasKey = jit.jump();

    keysAreWide.link(&jit);
    findUsedEntry([&] {
        jit.load64(BaseIndex(column, entry, CCallHelpers::TimesEight), word);
        return jit.branchTest64(CCallHelpers::NonZero, word);
    });
    if (hasValues) {
        jit.load32(Address(storage, CompactTable::offsetOfCapacity()), used);
        jit.getEffectiveAddress(BaseIndex(column, used, CCallHelpers::TimesEight), column);
    }

    hasKey.link(&jit);
    jit.store64(word, Address(results));
    // From here on the walk stands behind an entry, which clear() has to know.
    Jump isMarked = jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::wasWalked));
    jit.load8(flags, word);
    jit.or32(TrustedImm32(CompactTable::wasWalked), word);
    jit.store8(word, flags);
    isMarked.link(&jit);
    if (hasValues) {
        Jump valuesAreWide = jit.branchTest8(CCallHelpers::NonZero, Address(storage, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Wide)));
        jit.load32(BaseIndex(column, entry, CCallHelpers::TimesFour), word);
        decodeNarrow();
        jit.store64(word, Address(results, sizeof(EncodedJSValue)));
        hasEntry.append(jit.jump());

        valuesAreWide.link(&jit);
        jit.load64(BaseIndex(column, entry, CCallHelpers::TimesEight), word);
        jit.store64(word, Address(results, sizeof(EncodedJSValue)));
    }
    hasEntry.append(jit.jump());

    // Nothing leaves for the operation from here on, so the owner and the table make room.
    isOrdinary.link(&jit);
    const GPRReg deletedValue = owner;
    const GPRReg position = table;
    constexpr int32_t data = JSCellButterfly::offsetOfData();
    constexpr int32_t wordSize = sizeof(EncodedJSValue);
    const int32_t entrySize = hasValues ? JSMap::Helper::EntrySize : JSSet::Helper::EntrySize;
    jit.load32(Address(storage, data + JSMap::Helper::aliveEntryCountIndex() * wordSize), used);
    jit.load32(Address(storage, data + JSMap::Helper::deletedEntryCountIndex() * wordSize), word);
    jit.add32(word, used);
    jit.load32(Address(storage, data + JSMap::Helper::capacityIndex() * wordSize), word);
    jit.getEffectiveAddress(BaseIndex(storage, word, CCallHelpers::TimesEight, data + JSMap::Helper::hashTableStartIndex() * wordSize), column);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), deletedValue);
    jit.loadPtr(Address(deletedValue, VM::offsetOfOrderedHashTableDeletedValue()), deletedValue);
    findUsedEntry([&] {
        jit.mul32(TrustedImm32(entrySize), entry, position);
        jit.load64(BaseIndex(column, position, CCallHelpers::TimesEight), word);
        return jit.branch64(CCallHelpers::NotEqual, word, deletedValue);
    });
    jit.store64(word, Address(results));
    if (hasValues) {
        jit.load64(BaseIndex(column, position, CCallHelpers::TimesEight, wordSize), word);
        jit.store64(word, Address(results, wordSize));
    }

    hasEntry.link(&jit);
    jit.move(entry, jit.secondResult());
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();

    hasNoEntry.link(&jit);
    jit.store64(TrustedImm64(JSValue::encode(jsUndefined())), Address(results));
    if (hasValues)
        jit.store64(TrustedImm64(JSValue::encode(jsUndefined())), Address(results, wordSize));
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.tailCallOperation(rawOperation, OperationCall::FirstArgument::Instance);
}

void generateMapNextFrontEnd(StubAssembler& jit)
{
    emitNextFrontEnd(jit, CompactTableOwner::Map, Entry::RawMapNext);
}

void generateSetNextFrontEnd(StubAssembler& jit)
{
    emitNextFrontEnd(jit, CompactTableOwner::Set, Entry::RawSetNext);
}

void generateWeakMapGet(StubAssembler& jit)
{
    emitLookupInWeakTable(jit, WeakTableLookup::MapGet);
}

void generateWeakMapHas(StubAssembler& jit)
{
    emitLookupInWeakTable(jit, WeakTableLookup::MapHas);
}

void generateWeakSetHas(StubAssembler& jit)
{
    emitLookupInWeakTable(jit, WeakTableLookup::SetHas);
}

template<InlineAllocation allocation, typename Cell>
static void emitNewHashTable(StubAssembler& jit, Entry slowPath)
{
    constexpr unsigned kind = static_cast<unsigned>(allocation);
    const GPRReg result = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, result, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(result));
    for (unsigned word = 1; word < sizeof(Cell) / sizeof(uint64_t); ++word)
        jit.store64(TrustedImm32(0), Address(result, word * sizeof(uint64_t)));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slowCases.link(&jit);
    jit.tailCallOperation(slowPath, OperationCall::FirstArgument::Instance);
}

void generateNewMap(StubAssembler& jit)
{
    emitNewHashTable<InlineAllocation::Map, JSMap>(jit, Entry::operationAOTNewMap);
}

void generateNewSet(StubAssembler& jit)
{
    emitNewHashTable<InlineAllocation::Set, JSSet>(jit, Entry::operationAOTNewSet);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
