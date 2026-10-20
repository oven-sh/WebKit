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
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsHashTables.h"
#include "CompleteSubspace.h"
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

static void emitLoadValueOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg result = jit.result();
    emitLoadValuesOfCompactTable(jit, registers, registers.keys);
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

static void emitLoadValueOfTableInImage(StubAssembler& jit, const HashTableRegisters& registers, JumpList& unknown)
{
    static_assert(!DataWord::stringConstantTag && !DataWord::hole && InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
    constexpr int32_t offsetOfCellBeforeFirst = InstanceLayout::offsetOfStringCells - InstanceLayout::sizeOfStringCell;
    ASSERT(!jit.options().useWideReferences);
    const GPRReg result = jit.result();
    // On ARM64 the result register holds the key, which the unknown exit passes on.
    const GPRReg word = registers.candidate;

    emitLoadValuesOfCompactTable(jit, registers, registers.keys);
    jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), word);
    Jump isNotImmediate = jit.branchIfCell(word);
    jit.move(word, result);
    jit.returnToCaller();

    isNotImmediate.link(&jit);
    unknown.append(jit.branchTest64(CCallHelpers::NonZero, word, TrustedImm32(static_cast<int32_t>(DataWord::tagMask))));
    jit.urshift64(TrustedImm32(DataWord::operandShift), word);
    unknown.append(jit.branchTest64(CCallHelpers::Zero, word));
    jit.load32(BaseIndex(instanceGPR, word, CCallHelpers::TimesFour, offsetOfCellBeforeFirst), word);
    unknown.append(jit.branchTest32(CCallHelpers::Zero, word));
    decodeCell32(jit, word, result);
    jit.returnToCaller();
}

static void emitStoreValueOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers, GPRReg value, JumpList* doesNotFit)
{
    const GPRReg values = registers.keys;
    emitLoadValuesOfCompactTable(jit, registers, values);
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

// Nothing is stored before the last exit to cannotAppend: a table without room, or a column that is too narrow for the key or the value.
static void emitAppendToCompactTable(StubAssembler& jit, const HashTableRegisters& registers, InsertionPointExits& insertionPoints, std::optional<GPRReg> value, JumpList& cannotAppend)
{
    const GPRReg table = registers.table;
    const GPRReg entry = registers.entry;
    const bool keyFitsEveryColumn = keyComparisonOf(jit) == KeyComparison::String;
    auto storeKeyAt = [&](InsertionPoint point, JumpList& jumps) {
        jumps.link(&jit);
        jit.load32(Address(table, CompactTable::offsetOfUsed()), entry);
        cannotAppend.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, CompactTable::offsetOfCapacity())));
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
    emitAppendToCompactTable(jit, registers, exits.insertionPoints, value, exits.unknown);
    if (value)
        checkStubOperands(newCompactTable(owner), { registers.owner, registers.key, registers.hash, *value });
    else
        checkStubOperands(newCompactTable(owner), { registers.owner, registers.key, registers.hash });
    jit.linkToStub(exits.hasNoStorage, newCompactTable(owner));
    emitAtExit(jit, exits.unknown, [&] {
        emitCallTableOperation(jit, registers, owner == CompactTableOwner::Map ? Entry::operationAOTMapSet : Entry::operationAOTSetAdd, value);
    });
}

static void emitHas(StubAssembler& jit, Entry entry)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = true }, exits);
    exits.foundInCompactTable.append(exits.foundInTableInImage);
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

void generateMapGet(StubAssembler& jit)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = true }, exits);
    emitAtExit(jit, exits.foundInCompactTable, [&] {
        emitLoadValueOfCompactTable(jit, registers);
    });
    emitAtExit(jit, exits.foundInTableInImage, [&] {
        emitLoadValueOfTableInImage(jit, registers, exits.unknown);
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
        emitStoreValueOfCompactTable(jit, registers, value, &exits.unknown);
        emitBarrierAndReturnOwner(jit, registers, registers.table);
    });
    emitInsertAbsentKey(jit, registers, exits, CompactTableOwner::Map, value);
}

void generateSetAdd(StubAssembler& jit)
{
    const HashTableRegisters registers = HashTableRegisters::take(jit);
    HashTableExits exits;
    emitFindInHashTable(jit, registers, { .comparison = keyComparisonOf(jit), .searchesTablesInImage = false, .findsInsertionPoint = true }, exits);
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

void generateNewMap(StubAssembler& jit)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::Map);
    const GPRReg result = jit.result();
    const GPRReg allocator = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    JumpList slowCases;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, result, allocator, scratch, slowCases);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(result));
    for (unsigned word = 1; word < sizeof(JSMap) / sizeof(uint64_t); ++word)
        jit.store64(TrustedImm32(0), Address(result, word * sizeof(uint64_t)));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slowCases.link(&jit);
    jit.tailCallOperation(Entry::operationAOTNewMap, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
