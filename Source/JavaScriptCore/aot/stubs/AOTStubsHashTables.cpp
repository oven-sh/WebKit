/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsHashTables.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsCollections.h"
#include "AOTVector.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSString.h"
#include <wtf/MathExtras.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(CompactTable::maxCapacityWithTags == sizeof(uint64_t));
static_assert(CompactTable::hashBits + StringImpl::s_flagCount == 32);
static_assert(CompactTable::probeStartFor(CompactTable::hashBits, CompactTable::hashMask) == CompactTable::hashMask && CompactTable::probeStartFor(CompactTable::hashBits + 2, 1) == 4);

struct IndexSlotFormat {
    CCallHelpers::Scale scale;
    unsigned tagShift;
    unsigned entryBits;
    uint32_t tagMask;
    uint32_t freeSlot;
};

template<CompactTable::SlotKind kind>
static constexpr IndexSlotFormat indexSlotFormatOf()
{
    using Format = CompactTable::SlotFormat<kind>;
    using Slot = typename Format::Slot;
    return {
        sizeof(Slot) == sizeof(uint16_t) ? CCallHelpers::TimesTwo : CCallHelpers::TimesFour,
        Format::tagShift,
        Format::entryBits,
        static_cast<Slot>(~0u << Format::entryBits),
        Format::freeSlot,
    };
}

static constexpr IndexSlotFormat shortSlotFormat = indexSlotFormatOf<CompactTable::SlotKind::Short>();
static constexpr IndexSlotFormat longSlotFormat = indexSlotFormatOf<CompactTable::SlotKind::Long>();
static constexpr IndexSlotFormat hugeSlotFormat = indexSlotFormatOf<CompactTable::SlotKind::Huge>();

struct CompactTableSearch {
    bool isInImage;
    bool comparesStrings;
    // Nothing but strings is looked for, so the table has seen nothing but strings unless the program got around its types: its keys are narrow.
    bool keysAreNarrow;
    bool findsInsertionPoint;
    JumpList needsCharacters;
};

ptrdiff_t offsetOfHashTableStorage()
{
    RELEASE_ASSERT(JSMap::offsetOfStorage() == JSSet::offsetOfStorage());
    return JSMap::offsetOfStorage();
}

HashTableRegisters HashTableRegisters::take(StubAssembler& jit)
{
    constexpr HashTableRegisterPlan plan = hashTableRegisterPlan;
    GPRReg key = jit.operand(1);
    GPRReg owner;
    GPRReg hash;
    if constexpr (plan.copyOfOwner.has_value()) {
        hash = jit.clobberOperand(0);
        RELEASE_ASSERT(hash == gprOf(plan.hash));
        owner = jit.allocateScratch(*plan.copyOfOwner);
        jit.move(hash, owner);
    } else {
        owner = jit.operand(0);
        hash = jit.allocateScratch(plan.hash);
    }
    GPRReg table = jit.allocateScratch(plan.table);
    GPRReg cursor = jit.allocateScratch(plan.cursor);
    GPRReg keys = jit.allocateScratch(plan.keys);
    GPRReg entry = jit.allocateScratch(plan.entry);
    GPRReg candidate = jit.allocateScratch(plan.candidate);
    return { owner, key, hash, table, cursor, keys, entry, candidate };
}

void emitHashOfBits(StubAssembler& jit, GPRReg key, GPRReg hash, GPRReg scratch, GPRReg scratch2)
{
    constexpr int32_t offsetOfHashSecrets = AOT_INSTANCE_OFFSET(numbers.hashSecrets);
    jit.load64(Address(instanceGPR, offsetOfHashSecrets), hash);
    jit.xor64(key, hash);
    jit.load64(Address(instanceGPR, offsetOfHashSecrets + sizeof(uint64_t)), scratch);
    jit.xor64(key, scratch);
#if CPU(ARM64)
    jit.mul64(hash, scratch, scratch2);
    jit.uMulHigh64(hash, scratch, hash);
    jit.xor64(scratch2, hash);
#else
    UNUSED_PARAM(scratch2);
    ASSERT(hash == X86Registers::eax && scratch == X86Registers::edx);
    jit.m_assembler.mulq_r(scratch);
    jit.xor64(scratch, hash);
#endif
}

void emitLoadValuesOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers, GPRReg values, JumpList* keysAreNarrow, JumpList* keysAreWide)
{
    const GPRReg capacity = registers.cursor;
    const GPRReg keys = registers.keys;
    const Address capacityOfTable(registers.table, CompactTable::offsetOfCapacity());
    Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
    if (keysAreNarrow)
        keysAreNarrow->link(&jit);
    jit.load32(capacityOfTable, capacity);
    jit.getEffectiveAddress(BaseIndex(keys, capacity, CCallHelpers::TimesFour), values);
    Label isLoaded = jit.label();
    JumpList knownToBeWide = keysAreWide ? std::exchange(*keysAreWide, { }) : JumpList();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        isWide.link(&jit);
        knownToBeWide.link(&jit);
        jit.load32(capacityOfTable, capacity);
        jit.getEffectiveAddress(BaseIndex(keys, capacity, CCallHelpers::TimesEight), values);
        jit.jump().linkTo(isLoaded, &jit);
    });
}

static Vector<GPRReg, 8> registersToSaveForLeafCall(const StubAssembler& jit, const HashTableRegisters& registers)
{
    RegisterSet lost = registersClobberedByPreserveMostLeaves();
    lost.add(leafArgumentGPR(0));
    lost.add(leafArgumentGPR(1));

    RegisterSet live(registers.owner, registers.key, registers.entry);
    const StubSignature& signature = jit.signature();
    for (Reg operand : std::span(signature.operands).first(signature.numberOfOperands))
        live.add(operand, IgnoreVectors);
    live.filter(lost);

    Vector<GPRReg, 8> saved;
    live.forEach([&](Reg reg) {
        saved.append(reg.gpr());
    });
    return saved;
}

// The answer is in leafFunctionGPR. owner, key, entry and the operands of the stub are intact afterward. hash, table, cursor, keys and candidate are not.
static void emitCallStringLeaf(StubAssembler& jit, const HashTableRegisters& registers, Entry leaf, GPRReg first, std::optional<GPRReg> second)
{
    constexpr GPRReg scratch = leafFunctionGPR;
    RELEASE_ASSERT(second != scratch);
    const Vector<GPRReg, 8> saved = registersToSaveForLeafCall(jit, registers);
    jit.preserving(saved.span(), [&] {
        jit.recordClobbers(RegisterSet { leafArgumentGPR(0), leafArgumentGPR(1) });
        jit.move(first, scratch);
        if (second)
            jit.move(*second, leafArgumentGPR(1));
        jit.move(scratch, leafArgumentGPR(0));
        jit.callLeaf(leaf, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(saved.size() * sizeof(CPURegister)));
        jit.move(GPRInfo::returnValueGPR, scratch);
    });
}

static void emitReloadTable(StubAssembler& jit, const HashTableRegisters& registers, const CompactTableSearch& search)
{
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), registers.table);
    if (search.isInImage)
        jit.loadPtr(Address(registers.table, CompactTable::offsetOfIndex()), registers.table);
}

static void emitLoadKeys(StubAssembler& jit, const HashTableRegisters& registers)
{
    jit.load8(Address(registers.table, CompactTable::offsetOfKeysShift()), registers.cursor);
    jit.and32(TrustedImm32(CompactTable::keysShiftMask), registers.cursor);
    jit.move(TrustedImm32(2), registers.keys);
    jit.lshift64(registers.cursor, registers.keys);
    jit.add64(registers.table, registers.keys);
    jit.addPtr(TrustedImm32(CompactTable::offsetOfIndex()), registers.keys);
}

static void emitLoadCandidate(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg candidate = registers.candidate;
    Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
    jit.load32(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesFour), candidate);
    Jump isImmediate = jit.branch32(CCallHelpers::Below, candidate, TrustedImm32(minCell32));
    decodeCell32(jit, candidate, candidate);
    Jump isDecoded = jit.jump();

    isWide.link(&jit);
    jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), candidate);
    isImmediate.link(&jit);
    isDecoded.link(&jit);
}

// Falls through or jumps to next when the entry holds another key, with table intact.
static void emitCompareWithEntry(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, JumpList& next, HashTableExits& exits)
{
    const GPRReg key = registers.key;
    const GPRReg table = registers.table;
    const GPRReg candidate = registers.candidate;
    if (search.isInImage) {
        // The operation looks at every entry whose tag matches.
        search.needsCharacters.append(jit.jump());
        return;
    }

    JumpList& found = search.keysAreNarrow ? exits.foundAmongNarrowKeys : exits.foundInCompactTable;
    if (search.keysAreNarrow) {
        const BaseIndex word(registers.keys, registers.entry, CCallHelpers::TimesFour);
        jit.load32(word, candidate);
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        // A word that is no reference is an empty place or a value that is no cell: where it points no cell lies, so it was not taken for the key.
        next.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32)));
    } else if (!search.comparesStrings) {
        Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
        jit.load32(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesFour), candidate);
        Jump isImmediate = jit.branch32(CCallHelpers::Below, candidate, TrustedImm32(minCell32));
        decodeCell32(jit, candidate, candidate);
        isImmediate.link(&jit);
        exits.foundInCompactTable.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        next.append(jit.jump());

        isWide.link(&jit);
        jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), candidate);
        exits.foundAmongWideKeys.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        return;
    } else {
        emitLoadCandidate(jit, registers);
        exits.foundInCompactTable.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        next.append(jit.branchTest64(CCallHelpers::Zero, candidate));
        next.append(jit.branchIfNotCell(candidate));
    }
    next.append(jit.branchIfNotType(candidate, StringType));
    jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
    found.append(jit.branchPtr(CCallHelpers::Equal, candidate, Address(key, JSString::offsetOfValue())));

    jit.loadPtr(Address(key, JSString::offsetOfValue()), table);
    jit.load32(Address(table, StringImpl::flagsOffset()), table);
    jit.xor32(Address(candidate, StringImpl::flagsOffset()), table);
    search.needsCharacters.append(jit.branch32(CCallHelpers::Below, table, TrustedImm32(1 << StringImpl::s_flagCount)));
    emitReloadTable(jit, registers, search);
}

static void emitCompareLatin1Characters(StubAssembler& jit, GPRReg first, GPRReg second, GPRReg length, GPRReg scratch, GPRReg scratch2, JumpList& areSame, JumpList& differ)
{
    Jump isBelow8 = jit.branch32(CCallHelpers::Below, length, TrustedImm32(8));
    Label nextWord = jit.label();
    jit.sub32(TrustedImm32(8), length);
    jit.load64(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load64(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    differ.append(jit.branch64(CCallHelpers::NotEqual, scratch, scratch2));
    jit.branch32(CCallHelpers::AboveOrEqual, length, TrustedImm32(8)).linkTo(nextWord, &jit);
    jit.load64(Address(first), scratch);
    jit.load64(Address(second), scratch2);
    areSame.append(jit.branch64(CCallHelpers::Equal, scratch, scratch2));
    differ.append(jit.jump());

    isBelow8.link(&jit);
    Jump isBelow4 = jit.branch32(CCallHelpers::Below, length, TrustedImm32(4));
    jit.load32(Address(first), scratch);
    jit.load32(Address(second), scratch2);
    differ.append(jit.branch32(CCallHelpers::NotEqual, scratch, scratch2));
    jit.sub32(TrustedImm32(4), length);
    jit.load32(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load32(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    areSame.append(jit.branch32(CCallHelpers::Equal, scratch, scratch2));
    differ.append(jit.jump());

    isBelow4.link(&jit);
    Label nextCharacter = jit.label();
    areSame.append(jit.branchTest32(CCallHelpers::Zero, length));
    jit.sub32(TrustedImm32(1), length);
    jit.load8(BaseIndex(first, length, CCallHelpers::TimesOne), scratch);
    jit.load8(BaseIndex(second, length, CCallHelpers::TimesOne), scratch2);
    jit.branch32(CCallHelpers::Equal, scratch, scratch2).linkTo(nextCharacter, &jit);
    differ.append(jit.jump());
}

static void emitCompareCharactersOfHeapKey(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    const GPRReg impl = registers.hash;
    const GPRReg candidate = registers.candidate;
    const GPRReg length = registers.table;
    search.needsCharacters.link(&jit);
    jit.loadPtr(Address(registers.key, JSString::offsetOfValue()), impl);
    JumpList areSame;
    JumpList hasWideCharacters;
    jit.load32(Address(candidate, StringImpl::lengthMemoryOffset()), length);
    exits.unknown.append(jit.branch32(CCallHelpers::NotEqual, length, Address(impl, StringImpl::lengthMemoryOffset())));
    hasWideCharacters.append(jit.branchTest32(CCallHelpers::Zero, Address(candidate, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    hasWideCharacters.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(candidate, StringImpl::dataOffset()), candidate);
    jit.loadPtr(Address(impl, StringImpl::dataOffset()), impl);
    emitCompareLatin1Characters(jit, candidate, impl, length, registers.cursor, registers.keys, areSame, exits.unknown);

    hasWideCharacters.link(&jit);
    emitCallStringLeaf(jit, registers, Entry::operationAOTStringImplsAreEqual, candidate, impl);
    areSame.append(jit.branchTest32(CCallHelpers::NonZero, leafFunctionGPR));
    exits.unknown.append(jit.jump());

    areSame.link(&jit);
    emitReloadTable(jit, registers, search);
    emitLoadKeys(jit, registers);
    (search.keysAreNarrow ? exits.foundAmongNarrowKeys : exits.foundInCompactTable).append(jit.jump());
}

static void emitProbeTags(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    JumpList& absent = search.findsInsertionPoint ? exits.insertionPoints.amongTags : exits.absent;
    // An insertion stores the tag, so it keeps the hash.
    const GPRReg matches = search.findsInsertionPoint ? cursor : hash;
    jit.urshift32(hash, TrustedImm32(CompactTable::tagShiftOfTags), matches);
    jit.move(TrustedImm64(0x0101010101010101), candidate);
    jit.mul64(candidate, matches);
    jit.xor64(Address(table, index), matches);
    jit.sub64(matches, candidate, entry);
    jit.not64(matches);
    jit.and64(entry, matches);
    jit.and64(TrustedImm64(0x8080808080808080), matches);
    if (search.findsInsertionPoint) {
        // An insertion wants the keys where the key is absent too.
        jit.addPtr(TrustedImm32(index + CompactTable::maxCapacityWithTags), table, keys);
        absent.append(jit.branchTest64(CCallHelpers::Zero, matches));
    } else {
#if CPU(X86_64)
        // The conjunction has set the flags.
        absent.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionE)));
#else
        absent.append(jit.branchTest64(CCallHelpers::Zero, matches));
#endif
        jit.addPtr(TrustedImm32(index + CompactTable::maxCapacityWithTags), table, keys);
    }

    Label nextMatch = jit.label();
#if CPU(X86_64)
    // matches is not zero, so the count needs no test for that.
    jit.m_assembler.bsfq_rr(matches, entry);
#else
    jit.countTrailingZeros64(matches, entry);
#endif
    jit.urshift32(TrustedImm32(3), entry);
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, CompactTable::offsetOfUsed())));
    JumpList next;
    emitCompareWithEntry(jit, registers, search, next, exits);
    next.link(&jit);
    jit.sub64(matches, TrustedImm32(1), candidate);
    jit.and64(candidate, matches);
    jit.branchTest64(CCallHelpers::NonZero, matches).linkTo(nextMatch, &jit);
    absent.append(jit.jump());
}

static void emitMoveTagIntoSlotPosition(StubAssembler& jit, GPRReg hash, const IndexSlotFormat& format)
{
    if (format.tagShift >= format.entryBits)
        jit.urshift32(TrustedImm32(format.tagShift - format.entryBits), hash);
    else
        jit.lshift32(TrustedImm32(format.entryBits - format.tagShift), hash);
    jit.and32(TrustedImm32(static_cast<int32_t>(format.tagMask)), hash);
}

// candidate = CompactTable::probeStartFor(cursor, hash). keys is 1 << cursor. Overwrites cursor and entry.
static void emitLoadProbeStart(StubAssembler& jit, const HashTableRegisters& registers, const IndexSlotFormat& format)
{
    const GPRReg indexBits = registers.cursor;
    const GPRReg spreadHash = registers.entry;
    const GPRReg start = registers.candidate;

    jit.sub32(registers.keys, TrustedImm32(1), start);
    if (format.entryBits < CompactTable::hashBits) {
        jit.and32(registers.hash, start);
        return;
    }
    jit.move(registers.hash, spreadHash);
    Jump hashCoversIndex = jit.branch32(CCallHelpers::BelowOrEqual, indexBits, TrustedImm32(CompactTable::hashBits));
    jit.sub32(TrustedImm32(CompactTable::hashBits), indexBits);
    jit.lshift32(indexBits, spreadHash);
    hashCoversIndex.link(&jit);
    jit.and32(spreadHash, start);
}

static void emitProbeSlots(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, const IndexSlotFormat& format, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    const bool isShort = format.scale == CCallHelpers::TimesTwo;
    JumpList& atFreeSlot = isShort ? exits.insertionPoints.atShortSlot : exits.insertionPoints.atLongSlot;
    JumpList& absent = search.findsInsertionPoint ? atFreeSlot : exits.absent;
    jit.move(TrustedImm32(1), keys);
    jit.lshift32(cursor, keys);
    emitLoadProbeStart(jit, registers, format);
    jit.getEffectiveAddress(BaseIndex(table, candidate, format.scale, index), cursor);
    jit.getEffectiveAddress(BaseIndex(table, keys, format.scale, index), keys);
    emitMoveTagIntoSlotPosition(jit, hash, format);

    Label loop = jit.label();
    if (isShort)
        jit.load16(Address(cursor), entry);
    else
        jit.load32(Address(cursor), entry);
    absent.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(static_cast<int32_t>(format.freeSlot))));
    jit.xor32(hash, entry);
    JumpList next;
    next.append(jit.branch32(CCallHelpers::Above, entry, TrustedImm32((1 << format.entryBits) - 1)));
    emitCompareWithEntry(jit, registers, search, next, exits);
    next.link(&jit);
    jit.addPtr(TrustedImm32(1 << format.scale), cursor);
    jit.branchPtr(CCallHelpers::NotEqual, cursor, keys).linkTo(loop, &jit);
    jit.addPtr(TrustedImm32(index), table, cursor);
    jit.jump().linkTo(loop, &jit);
}

static void emitSearchCompactTable(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    const GPRReg cursor = registers.cursor;
    const Address capacity(registers.table, CompactTable::offsetOfCapacity());

    const Address indexBits(registers.table, CompactTable::offsetOfIndexBits());
#if CPU(X86_64)
    Jump hasSlots = jit.branch8(CCallHelpers::NotEqual, indexBits, TrustedImm32(0));
#else
    jit.load8(indexBits, cursor);
    Jump hasSlots = jit.branchTest32(CCallHelpers::NonZero, cursor);
#endif
    emitProbeTags(jit, registers, search, exits);

    hasSlots.link(&jit);
#if CPU(X86_64)
    jit.load8(indexBits, cursor);
#endif
    Jump hasLongOrHugeSlots = jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    emitProbeSlots(jit, registers, search, shortSlotFormat, exits);

    hasLongOrHugeSlots.link(&jit);
    Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(CompactTable::maxCapacityWithLongSlots));
    emitProbeSlots(jit, registers, search, longSlotFormat, exits);

    hasHugeSlots.link(&jit);
    emitProbeSlots(jit, registers, search, hugeSlotFormat, exits);

    if (!search.comparesStrings)
        return;
    if (search.isInImage)
        exits.unknown.append(search.needsCharacters);
    else
        emitCompareCharactersOfHeapKey(jit, registers, search, exits);
}

static void emitFindInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, bool comparesStrings, HashTableExits& exits)
{
    const bool keysAreNarrow = comparesStrings && lookup.comparison == KeyComparison::String;
    const GPRReg table = registers.table;
    // A table that holds strings beside keys that need 64 bits is left to the operation: one test here in place of one for each key that is looked at.
    const int32_t otherTables = CompactTable::keysAreInImage | (keysAreNarrow ? CompactTable::keysAreWide : 0);
    Jump isInImage = jit.branchTest8(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(otherTables));
    CompactTableSearch heapSearch { false, comparesStrings, keysAreNarrow, lookup.findsInsertionPoint, { } };
    emitSearchCompactTable(jit, registers, heapSearch, exits);

    if (lookup.findsInsertionPoint)
        exits.unknown.append(isInImage);
    else if (!comparesStrings)
        exits.absent.append(isInImage);
    else if (lookup.searchesTablesInImage) {
        isInImage.link(&jit);
        if (keysAreNarrow)
            exits.unknown.append(jit.branchTest8(CCallHelpers::Zero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreInImage)));
        jit.loadPtr(Address(table, CompactTable::offsetOfIndex()), table);
        CompactTableSearch imageSearch { true, true, false, false, { } };
        emitSearchCompactTable(jit, registers, imageSearch, exits);
    } else
        exits.unknown.append(isInImage);
}

static void emitClassifyAnyValue(StubAssembler& jit, GPRReg key, JumpList& isComparedByBits, HashTableExits& exits)
{
    Jump notCell = jit.branchIfNotCell(key);
    Jump isString = jit.branchIfType(key, StringType);
    exits.unknown.append(jit.branchIfType(key, HeapBigIntType));
    isComparedByBits.append(jit.jump());

    notCell.link(&jit);
    isComparedByBits.append(jit.branchIfNotNumber(key));
    isComparedByBits.append(jit.branchIfInt32(key));
    exits.unknown.append(jit.jump());

    isString.link(&jit);
}

static void emitLoadTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    JumpList& hasNoStorage = lookup.findsInsertionPoint ? exits.hasNoStorage : exits.absent;
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), registers.table);
    hasNoStorage.append(jit.branchTestPtr(CCallHelpers::Zero, registers.table));
}

static void emitFindStringKey(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    const GPRReg hash = registers.hash;
    const GPRReg impl = registers.candidate;
    auto branchToAbsentIfNoStorage = [&] {
        if (!lookup.findsInsertionPoint)
            exits.absent.append(jit.branchTestPtr(CCallHelpers::Zero, Address(registers.owner, offsetOfHashTableStorage())));
    };

    jit.loadPtr(Address(registers.key, JSString::offsetOfValue()), impl);
    Jump isRope = jit.branchTestPtr(CCallHelpers::NonZero, impl, TrustedImm32(JSString::isRopeInPointer));
    jit.load32(Address(impl, StringImpl::flagsOffset()), hash);
    jit.urshift32(TrustedImm32(StringImpl::s_flagCount), hash);
#if CPU(X86_64)
    // The shift has set the flags.
    Jump isNotHashed = Jump(jit.m_assembler.jCC(X86Assembler::ConditionE));
#else
    Jump isNotHashed = jit.branchTest32(CCallHelpers::Zero, hash);
#endif

    Label isHashed = jit.label();
    emitLoadTable(jit, registers, lookup, exits);
    emitFindInCompactTable(jit, registers, lookup, true, exits);

    isNotHashed.link(&jit);
    branchToAbsentIfNoStorage();
    emitCallStringLeaf(jit, registers, Entry::operationAOTHashStringImpl, impl, std::nullopt);
    jit.move(leafFunctionGPR, hash);
    jit.jump().linkTo(isHashed, &jit);

    isRope.link(&jit);
    branchToAbsentIfNoStorage();
    exits.unknown.append(jit.jump());
}

void emitFindInHashTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    if (jit.options().useWideReferences) {
        if (!lookup.findsInsertionPoint)
            emitLoadTable(jit, registers, lookup, exits);
        exits.unknown.append(jit.jump());
        return;
    }

    JumpList isComparedByBits;
    switch (lookup.comparison) {
    case KeyComparison::Bits:
        break;
    case KeyComparison::String:
        isComparedByBits.append(jit.branchIfNotCell(registers.key));
        emitFindStringKey(jit, registers, lookup, exits);
        break;
    case KeyComparison::AnyValue:
        emitClassifyAnyValue(jit, registers.key, isComparedByBits, exits);
        emitFindStringKey(jit, registers, lookup, exits);
        break;
    }

    isComparedByBits.link(&jit);
    emitHashOfBits(jit, registers.key, registers.hash, registers.table, registers.candidate);
    jit.and32(TrustedImm32(CompactTable::hashMask), registers.hash);
    emitLoadTable(jit, registers, lookup, exits);
    emitFindInCompactTable(jit, registers, lookup, false, exits);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
