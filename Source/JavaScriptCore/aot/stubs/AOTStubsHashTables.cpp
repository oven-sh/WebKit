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
    unsigned entryBytes;
    // The index has a tag byte and an entry number for each slot.
    CCallHelpers::Scale bytesOfSlot;
};

template<CompactTable::SlotKind kind>
static constexpr IndexSlotFormat indexSlotFormatOf()
{
    constexpr unsigned entryBytes = CompactTable::SlotFormat<kind>::entryBytes;
    static_assert(entryBytes == 1 || entryBytes == 3);
    return { entryBytes, entryBytes == 1 ? CCallHelpers::TimesTwo : CCallHelpers::TimesFour };
}

static constexpr IndexSlotFormat shortSlotFormat = indexSlotFormatOf<CompactTable::SlotKind::Short>();
static constexpr IndexSlotFormat longSlotFormat = indexSlotFormatOf<CompactTable::SlotKind::Long>();
static_assert(4 * static_cast<uint64_t>(CompactTable::maxCapacityWithLongSlots) <= 3 * (static_cast<uint64_t>(1) << CompactTable::hashBits));

// What a search knows of the keys before it looks at one.
enum class KeyWidth : uint8_t { Either, Narrow, Wide };

struct CompactTableSearch {
    bool isInImage;
    bool comparesStrings;
    KeyWidth keyWidth;
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

void emitLoadValuesOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers, GPRReg values, JumpList* keysAreNarrow, JumpList* keysAreWide, bool everyoneKnowsTheWidth)
{
    const GPRReg capacity = registers.cursor;
    const GPRReg keys = registers.keys;
    const Address capacityOfTable(registers.table, CompactTable::offsetOfCapacity());
    JumpList isWide = keysAreWide ? std::exchange(*keysAreWide, { }) : JumpList();
    if (!everyoneKnowsTheWidth)
        isWide.append(jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide)));
    if (keysAreNarrow)
        keysAreNarrow->link(&jit);
    jit.load32(capacityOfTable, capacity);
    jit.getEffectiveAddress(BaseIndex(keys, capacity, CCallHelpers::TimesFour), values);
    if (isWide.empty())
        return;
    Label isLoaded = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        isWide.link(&jit);
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

    if (!search.comparesStrings) {
        ASSERT(search.keyWidth == KeyWidth::Either);
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
    }

    ASSERT(search.keyWidth != KeyWidth::Either);
    JumpList& found = search.keyWidth == KeyWidth::Narrow ? exits.foundAmongNarrowKeys : exits.foundAmongWideKeys;
    if (search.keyWidth == KeyWidth::Narrow) {
        const BaseIndex word(registers.keys, registers.entry, CCallHelpers::TimesFour);
        jit.load32(word, candidate);
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        // A word that is no reference is an empty place or a value that is no cell: where it points no cell lies, so it was not taken for the key.
        next.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32)));
    } else {
        jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
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

// One comparison serves every search of a heap table in a stub.
static void emitCompareCharactersOfHeapKey(StubAssembler& jit, const HashTableRegisters& registers, JumpList& needsCharacters, JumpList& found, HashTableExits& exits)
{
    const GPRReg impl = registers.hash;
    const GPRReg candidate = registers.candidate;
    const GPRReg length = registers.table;
    needsCharacters.link(&jit);
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
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), registers.table);
    emitLoadKeys(jit, registers);
    found.append(jit.jump());
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

// cursor = CompactTable::probeStartFor(cursor, hash), for an index with no more bits than the hash. keys is 1 << cursor.
static void emitLoadHomeSlot(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg slot = registers.cursor;
    jit.sub32(registers.keys, TrustedImm32(1), slot);
    jit.and32(registers.hash, slot);
}

// tags = where the tags begin: as many bytes in front of the keys as there are slots.
static void emitLoadTags(StubAssembler& jit, const HashTableRegisters& registers, GPRReg tags)
{
    jit.sub64(registers.keys, registers.table, tags);
    jit.sub64(TrustedImm32(CompactTable::offsetOfIndex()), tags);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    jit.urshift64(TrustedImm32(1), tags);
    Jump isShifted = jit.jump();
    hasLongSlots.link(&jit);
    Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
    jit.urshift64(TrustedImm32(2), tags);
    Jump isShiftedByTwo = jit.jump();
    hasHugeSlots.link(&jit);
    jit.urshift64(TrustedImm32(3), tags);
    isShifted.link(&jit);
    isShiftedByTwo.link(&jit);
    jit.sub64(registers.keys, tags, tags);
}

// entry = the number in the slot.
static void emitLoadEntryNumber(StubAssembler& jit, const HashTableRegisters& registers, const IndexSlotFormat& format, GPRReg slot, GPRReg entry)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    ASSERT(slot != entry);
    if (format.entryBytes == 1) {
        jit.load8(BaseIndex(registers.table, slot, CCallHelpers::TimesOne, index), entry);
        return;
    }
    jit.getEffectiveAddress(BaseIndex(slot, slot, CCallHelpers::TimesTwo), entry);
    jit.load32(BaseIndex(registers.table, entry, CCallHelpers::TimesOne, index), entry);
    jit.and32(TrustedImm32(0xffffff), entry);
}

static void emitCountTrailingZerosOfNonZero(StubAssembler& jit, GPRReg source, GPRReg count)
{
#if CPU(X86_64)
    jit.m_assembler.bsfq_rr(source, count);
#else
    jit.countTrailingZeros64(source, count);
#endif
}

// tags holds eight tags, and every byte of hash is the tag that is looked for.
// matches = a set bit at the top of each tag that may be that one. taken = the complement of the difference: its top bit of a tag is set where the slot is taken.
static void emitCompareEightTags(StubAssembler& jit, const HashTableRegisters& registers, GPRReg tags, GPRReg matches, GPRReg taken)
{
    jit.xor64(registers.hash, tags, taken);
    jit.move(TrustedImm64(-static_cast<int64_t>(CompactTable::onesOfTags)), matches);
    jit.add64(taken, matches);
    jit.not64(taken);
    jit.and64(taken, matches);
    jit.and64(TrustedImm64(CompactTable::onesOfTags << 7), matches);
}

// Behind this search of the home slot all kinds of slots share emitSearchGroups(): candidate is where the tags begin, and hash the tag.
static void emitProbeHomeSlot(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, const IndexSlotFormat& format, JumpList& searchesGroups, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    JumpList& absent = search.findsInsertionPoint ? exits.insertionPoints.atSlot : exits.absent;
    jit.move(TrustedImm32(1), keys);
    jit.lshift32(cursor, keys);
    emitLoadHomeSlot(jit, registers);
    jit.urshift32(TrustedImm32(CompactTable::tagShiftOfSlots), hash);
    jit.or32(TrustedImm32(CompactTable::takenSlot), hash);
    // The tags begin behind 1 or 3 bytes for each slot, and the keys behind one more.
    if (format.entryBytes == 1) {
        jit.getEffectiveAddress(BaseIndex(table, keys, CCallHelpers::TimesOne, index), candidate);
        jit.getEffectiveAddress(BaseIndex(table, keys, CCallHelpers::TimesTwo, index), keys);
    } else {
        jit.getEffectiveAddress(BaseIndex(table, keys, format.bytesOfSlot, index), candidate);
        jit.sub64(keys, candidate);
        jit.add64(candidate, keys);
    }

    // Most keys are in their home slot.
    jit.load8(BaseIndex(candidate, cursor, CCallHelpers::TimesOne), entry);
    Jump isNotAtHome = jit.branch32(CCallHelpers::NotEqual, entry, hash);
    emitLoadEntryNumber(jit, registers, format, cursor, entry);
    JumpList hasOtherKey;
    emitCompareWithEntry(jit, registers, search, hasOtherKey, exits);
    hasOtherKey.link(&jit);
    emitLoadTags(jit, registers, candidate);
    searchesGroups.append(jit.jump());

    isNotAtHome.link(&jit);
    absent.append(jit.branchTest32(CCallHelpers::Zero, entry));
    searchesGroups.append(jit.jump());
}

// cursor is the number of a free slot at an insertion point, and the low byte of hash the tag.
static void emitSearchGroups(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    constexpr uint64_t tops = CompactTable::onesOfTags << 7;
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    jit.and32(TrustedImm32(~static_cast<int32_t>(CompactTable::slotsPerGroup - 1)), cursor);
    jit.load64(BaseIndex(candidate, cursor, CCallHelpers::TimesOne), entry);
    jit.move(TrustedImm64(CompactTable::onesOfTags), candidate);
    jit.mul64(candidate, hash);

    Label compareTags = jit.label();
    emitCompareEightTags(jit, registers, entry, candidate, entry);
#if CPU(X86_64)
    // The last conjunction has set the flags.
    Jump hasNoMatch = Jump(jit.m_assembler.jCC(X86Assembler::ConditionE));
#else
    Jump hasNoMatch = jit.branchTest64(CCallHelpers::Zero, candidate);
#endif

    Label nextMatch = jit.label();
    emitCountTrailingZerosOfNonZero(jit, candidate, candidate);
    jit.urshift32(TrustedImm32(3), candidate);
    jit.add32(candidate, cursor);
    {
        Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
        emitLoadEntryNumber(jit, registers, shortSlotFormat, cursor, entry);
        Jump isLoaded = jit.jump();
        hasLongSlots.link(&jit);
        Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
        emitLoadEntryNumber(jit, registers, longSlotFormat, cursor, entry);
        Jump isLoadedFromLong = jit.jump();
        hasHugeSlots.link(&jit);
        jit.lshift64(cursor, TrustedImm32(3), entry);
        jit.sub64(cursor, entry);
        jit.load32(BaseIndex(table, entry, CCallHelpers::TimesOne, CompactTable::offsetOfIndex()), entry);
        isLoaded.link(&jit);
        isLoadedFromLong.link(&jit);
    }
    JumpList next;
    emitCompareWithEntry(jit, registers, search, next, exits);

    // The matches above that slot. table holds the shift meanwhile.
    next.link(&jit);
    emitLoadTags(jit, registers, entry);
    jit.and32(TrustedImm32(CompactTable::slotsPerGroup - 1), cursor, table);
    jit.sub32(table, cursor);
    jit.load64(BaseIndex(entry, cursor, CCallHelpers::TimesOne), entry);
    jit.lshift32(TrustedImm32(3), table);
    jit.add32(TrustedImm32(7), table);
    emitCompareEightTags(jit, registers, entry, candidate, entry);
    jit.urshift64(table, candidate);
    jit.urshift64(TrustedImm32(1), candidate);
    jit.lshift64(TrustedImm32(1), candidate);
    jit.lshift64(table, candidate);
    emitReloadTable(jit, registers, search);
    jit.branchTest64(CCallHelpers::NonZero, candidate).linkTo(nextMatch, &jit);

    // A free slot among the eight ends the search.
    hasNoMatch.link(&jit);
    jit.not64(entry);
    if (search.findsInsertionPoint) {
        jit.and64(TrustedImm64(tops), entry);
        Jump isFull = jit.branchTest64(CCallHelpers::Zero, entry);
        emitCountTrailingZerosOfNonZero(jit, entry, candidate);
        jit.urshift32(TrustedImm32(3), candidate);
        jit.add32(candidate, cursor);
        exits.insertionPoints.atSlot.append(jit.jump());
        isFull.link(&jit);
    } else
        exits.absent.append(jit.branchTest64(CCallHelpers::NonZero, entry, TrustedImm64(tops)));
    // The next eight. The number of slots is what lies between the tags and the keys.
    emitLoadTags(jit, registers, candidate);
    jit.sub64(keys, candidate, entry);
    jit.add32(TrustedImm32(CompactTable::slotsPerGroup), cursor);
    jit.sub32(TrustedImm32(1), entry);
    jit.and32(entry, cursor);
    jit.load64(BaseIndex(candidate, cursor, CCallHelpers::TimesOne), entry);
    jit.jump().linkTo(compareTags, &jit);
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
    JumpList searchesGroups;
    Jump hasLongOrHugeSlots = jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    emitProbeHomeSlot(jit, registers, search, shortSlotFormat, searchesGroups, exits);

    hasLongOrHugeSlots.link(&jit);
    Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, capacity, TrustedImm32(CompactTable::maxCapacityWithLongSlots));
    emitProbeHomeSlot(jit, registers, search, longSlotFormat, searchesGroups, exits);

    // A table of more than twelve million entries. The search of the first eight tags finds a key in its home slot too.
    hasHugeSlots.link(&jit);
    {
        constexpr int32_t index = CompactTable::offsetOfIndex();
        const GPRReg hash = registers.hash;
        const GPRReg keys = registers.keys;
        const GPRReg entry = registers.entry;
        const GPRReg table = registers.table;
        const GPRReg tags = registers.candidate;
        jit.move(TrustedImm32(1), keys);
        jit.lshift64(cursor, keys);
        jit.move(hash, entry);
        Jump hashCoversIndex = jit.branch32(CCallHelpers::BelowOrEqual, cursor, TrustedImm32(CompactTable::hashBits));
        jit.sub32(TrustedImm32(CompactTable::hashBits), cursor);
        jit.lshift32(cursor, entry);
        hashCoversIndex.link(&jit);
        jit.sub32(keys, TrustedImm32(1), cursor);
        jit.and32(entry, cursor);
        jit.urshift32(TrustedImm32(CompactTable::tagShiftOfSlots), hash);
        jit.or32(TrustedImm32(CompactTable::takenSlot), hash);
        jit.getEffectiveAddress(BaseIndex(table, keys, CCallHelpers::TimesEight, index), tags);
        jit.sub64(keys, tags);
        jit.add64(tags, keys);
        (search.findsInsertionPoint ? exits.insertionPoints.atSlot : exits.absent).append(jit.branchTest8(CCallHelpers::Zero, BaseIndex(tags, cursor, CCallHelpers::TimesOne)));
    }

    searchesGroups.link(&jit);
    emitSearchGroups(jit, registers, search, exits);
}

static void emitFindBitsInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    Jump isInImage = jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreInImage));
    CompactTableSearch search { false, false, KeyWidth::Either, lookup.findsInsertionPoint, { } };
    emitSearchCompactTable(jit, registers, search, exits);
    (lookup.findsInsertionPoint ? exits.unknown : exits.absent).append(isInImage);
}

static void emitFindStringInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, JumpList& isLeftToFormForAnyValue, HashTableExits& exits)
{
    const GPRReg table = registers.table;
    const Address flags(table, CompactTable::offsetOfKeysShift());
    const bool searchesTablesInImage = lookup.searchesTablesInImage && !lookup.findsInsertionPoint;
    // Most tables that hold strings have narrow keys. One test of the width here takes the place of one for each key that is looked at.
    Jump isOtherTable = jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreInImage | CompactTable::keysAreWide));
    CompactTableSearch narrowSearch { false, true, KeyWidth::Narrow, lookup.findsInsertionPoint, { } };
    emitSearchCompactTable(jit, registers, narrowSearch, exits);
    JumpList needsCharacters = narrowSearch.needsCharacters;

    JumpList isInImage;
    if (lookup.comparison == KeyComparison::String) {
        if (searchesTablesInImage) {
            isOtherTable.link(&jit);
            isLeftToFormForAnyValue.append(jit.branchTest8(CCallHelpers::Zero, flags, TrustedImm32(CompactTable::keysAreInImage)));
        } else
            isLeftToFormForAnyValue.append(isOtherTable);
    } else {
        isOtherTable.link(&jit);
        isInImage.append(jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreInImage)));
        CompactTableSearch wideSearch { false, true, KeyWidth::Wide, lookup.findsInsertionPoint, { } };
        emitSearchCompactTable(jit, registers, wideSearch, exits);
        needsCharacters.append(wideSearch.needsCharacters);
    }

    if (searchesTablesInImage) {
        isInImage.link(&jit);
        jit.loadPtr(Address(table, CompactTable::offsetOfIndex()), table);
        CompactTableSearch imageSearch { true, true, KeyWidth::Either, false, { } };
        emitSearchCompactTable(jit, registers, imageSearch, exits);
        exits.unknown.append(imageSearch.needsCharacters);
    } else
        exits.unknown.append(isInImage);

    emitCompareCharactersOfHeapKey(jit, registers, needsCharacters, lookup.comparison == KeyComparison::String ? exits.foundAmongNarrowKeys : exits.foundInCompactTable, exits);
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

static void emitFindStringKey(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, JumpList& isLeftToFormForAnyValue, HashTableExits& exits)
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
    emitFindStringInCompactTable(jit, registers, lookup, isLeftToFormForAnyValue, exits);

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
    JumpList isLeftToFormForAnyValue;
    switch (lookup.comparison) {
    case KeyComparison::Bits:
        break;
    case KeyComparison::String: {
        // undefined, null, and a table that holds numbers beside its strings are rare here. The form for any value, which every program has, knows what to do with them.
        isLeftToFormForAnyValue.append(jit.branchIfNotCell(registers.key));
        emitFindStringKey(jit, registers, lookup, isLeftToFormForAnyValue, exits);
        isLeftToFormForAnyValue.link(&jit);
        const StubKey formForAnyValue = jit.key().withForm(static_cast<uint8_t>(KeyComparison::AnyValue));
        if constexpr (hashTableRegisterPlan.copyOfOwner.has_value())
            jit.move(registers.owner, registers.hash);
        Vector<GPRReg, 4> operands;
        for (Reg operand : std::span(jit.signature().operands).first(jit.signature().numberOfOperands))
            operands.append(operand.gpr());
        checkStubOperands(formForAnyValue, operands.span());
        jit.jumpToStub(formForAnyValue);
        return;
    }
    case KeyComparison::AnyValue:
        emitClassifyAnyValue(jit, registers.key, isComparedByBits, exits);
        emitFindStringKey(jit, registers, lookup, isLeftToFormForAnyValue, exits);
        RELEASE_ASSERT(isLeftToFormForAnyValue.empty());
        break;
    }

    isComparedByBits.link(&jit);
    emitHashOfBits(jit, registers.key, registers.hash, registers.table, registers.candidate);
    jit.and32(TrustedImm32(CompactTable::hashMask), registers.hash);
    emitLoadTable(jit, registers, lookup, exits);
    emitFindBitsInCompactTable(jit, registers, lookup, exits);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
