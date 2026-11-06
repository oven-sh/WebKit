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
    GPRReg owner = jit.operand(0);
    GPRReg key = jit.operand(1);
    RELEASE_ASSERT(owner == ownerOfHashTableGPR);
    GPRReg hash = jit.allocateScratch(plan.hash);
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

void emitLoadTagOfSlot(StubAssembler& jit, GPRReg hash)
{
    static_assert(CompactTable::tagOfSlot(CompactTable::hashMask) == CompactTable::takenSlot + 2 * (CompactTable::hashMask >> CompactTable::tagShiftOfSlots));
    jit.urshift32(TrustedImm32(CompactTable::tagShiftOfSlots), hash);
    jit.getEffectiveAddress(BaseIndex(hash, hash, CCallHelpers::TimesOne, CompactTable::takenSlot), hash);
}

void emitMarkThatKeyLeftHomeSlot(StubAssembler& jit, BaseIndex tagAtHome)
{
#if CPU(X86_64)
    jit.or8(TrustedImm32(CompactTable::keyLeftHomeSlot), tagAtHome);
#else
    const GPRReg tag = jit.scratchRegister();
    jit.load8(tagAtHome, tag);
    jit.or32(TrustedImm32(CompactTable::keyLeftHomeSlot), tag);
    jit.store8(tag, tagAtHome);
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

// A tag in a slot is that of the key if the two differ in nothing but keyLeftHomeSlot. An insertion stores the tag, with that bit clear. A lookup has it set, which takes one instruction less:
// the difference then has the bit where no key has left the slot.
static void emitLoadTagToSearchFor(StubAssembler& jit, const CompactTableSearch& search, GPRReg hash)
{
    if (search.findsInsertionPoint) {
        emitLoadTagOfSlot(jit, hash);
        return;
    }
    jit.urshift32(TrustedImm32(CompactTable::tagShiftOfSlots - 1), hash);
    jit.or32(TrustedImm32(CompactTable::takenSlot | CompactTable::keyLeftHomeSlot), hash);
}

// cursor = CompactTable::probeStartFor(cursor, hash), for an index with no more bits than the hash. keys is 1 << cursor.
static void emitLoadHomeSlot(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg slot = registers.cursor;
    jit.sub32(registers.keys, TrustedImm32(1), slot);
    jit.and32(registers.hash, slot);
}

// keys goes from the number of slots to the address of the keys.
static void emitLoadKeysBehindSlots(StubAssembler& jit, const HashTableRegisters& registers, CCallHelpers::Scale bytesOfSlot)
{
    jit.getEffectiveAddress(BaseIndex(registers.table, registers.keys, bytesOfSlot, CompactTable::offsetOfIndex()), registers.keys);
}

static void emitLoadKeysBehindSlots(StubAssembler& jit, const HashTableRegisters& registers)
{
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    emitLoadKeysBehindSlots(jit, registers, shortSlotFormat.bytesOfSlot);
    Jump isShortDone = jit.jump();
    hasLongSlots.link(&jit);
    Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
    emitLoadKeysBehindSlots(jit, registers, longSlotFormat.bytesOfSlot);
    Jump isLongDone = jit.jump();
    hasHugeSlots.link(&jit);
    emitLoadKeysBehindSlots(jit, registers, CCallHelpers::TimesEight);
    isShortDone.link(&jit);
    isLongDone.link(&jit);
}

// keys goes from the address of the keys to the number of slots.
static void emitLoadNumberOfSlots(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg slots = registers.keys;
    jit.sub64(registers.keys, registers.table, slots);
    jit.sub64(TrustedImm32(CompactTable::offsetOfIndex()), slots);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    jit.urshift64(TrustedImm32(1), slots);
    Jump isShortDone = jit.jump();
    hasLongSlots.link(&jit);
    Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
    jit.urshift64(TrustedImm32(2), slots);
    Jump isLongDone = jit.jump();
    hasHugeSlots.link(&jit);
    jit.urshift64(TrustedImm32(3), slots);
    isShortDone.link(&jit);
    isLongDone.link(&jit);
}

// entry = the number in the slot cursor. keys is the number of slots: the numbers lie behind as many tags.
static void emitLoadEntryNumber(StubAssembler& jit, const HashTableRegisters& registers, const IndexSlotFormat& format)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    const GPRReg slot = registers.cursor;
    const GPRReg entry = registers.entry;
    if (format.entryBytes == 1) {
        jit.getEffectiveAddress(BaseIndex(registers.keys, slot, CCallHelpers::TimesOne), entry);
        jit.load8(BaseIndex(registers.table, entry, CCallHelpers::TimesOne, index), entry);
        return;
    }
    jit.getEffectiveAddress(BaseIndex(slot, slot, CCallHelpers::TimesTwo), entry);
    jit.add64(registers.keys, entry);
    jit.load32(BaseIndex(registers.table, entry, CCallHelpers::TimesOne, index), entry);
    jit.and32(TrustedImm32(0xffffff), entry);
}

// Behind a tag that matches, which is where the width of the numbers first matters: entry = the number in the slot cursor, and keys goes from the number of slots to the address of the keys.
static void emitLoadEntryNumberAndKeys(StubAssembler& jit, const HashTableRegisters& registers, bool mayHaveHugeSlots)
{
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    emitLoadEntryNumber(jit, registers, shortSlotFormat);
    emitLoadKeysBehindSlots(jit, registers, shortSlotFormat.bytesOfSlot);
    Label isLoaded = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        hasLongSlots.link(&jit);
        Jump hasHugeSlots;
        if (mayHaveHugeSlots)
            hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
        emitLoadEntryNumber(jit, registers, longSlotFormat);
        emitLoadKeysBehindSlots(jit, registers, longSlotFormat.bytesOfSlot);
        jit.jump().linkTo(isLoaded, &jit);
        if (mayHaveHugeSlots) {
            hasHugeSlots.link(&jit);
            jit.lshift64(registers.cursor, TrustedImm32(3), registers.entry);
            jit.sub64(registers.cursor, registers.entry);
            jit.add64(registers.keys, registers.entry);
            jit.load32(BaseIndex(registers.table, registers.entry, CCallHelpers::TimesOne, CompactTable::offsetOfIndex()), registers.entry);
            emitLoadKeysBehindSlots(jit, registers, CCallHelpers::TimesEight);
            jit.jump().linkTo(isLoaded, &jit);
        }
    });
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
    jit.move(TrustedImm64(-static_cast<int64_t>(CompactTable::belowDifferenceOfOtherTags)), matches);
    jit.add64(taken, matches);
    jit.not64(taken);
    jit.and64(taken, matches);
    jit.and64(TrustedImm64(CompactTable::onesOfTags << 7), matches);
}

// On entry cursor is the number of index bits, of no more bits than the hash has. Behind this search of the home slot all kinds of slots share emitSearchGroups(): keys is the number of slots, and hash the tag.
static void emitProbeHomeSlot(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, JumpList& searchesGroups, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    const BaseIndex tagAtHome(table, cursor, CCallHelpers::TimesOne, index);
    jit.move(TrustedImm32(1), keys);
    jit.lshift32(cursor, keys);
    emitLoadHomeSlot(jit, registers);
    emitLoadTagToSearchFor(jit, search, hash);

    // Most keys are in their home slot, and most that are absent find there that no key has left it.
    // difference: in its low byte, what the tag in the slot differs in from the tag that is searched for.
#if CPU(X86_64)
    // One instruction reads and compares. It takes three bytes behind the tag along: more tags, or the first numbers. The same instruction gives the tag back.
    const GPRReg difference = hash;
    auto compareWithTagAtHome = [&] { jit.xor32(tagAtHome, hash); };
    auto restoreTag = compareWithTagAtHome;
#else
    const GPRReg difference = entry;
    auto compareWithTagAtHome = [&] {
        jit.load8(tagAtHome, entry);
        jit.xor32(hash, entry);
    };
    auto restoreTag = [] { };
#endif
    compareWithTagAtHome();
    Jump isNotAtHome = jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(0xff & ~CompactTable::keyLeftHomeSlot));
    emitLoadEntryNumberAndKeys(jit, registers, false);
    JumpList hasOtherKey;
    emitCompareWithEntry(jit, registers, search, hasOtherKey, exits);
    hasOtherKey.link(&jit);
    restoreTag();
    if (search.findsInsertionPoint)
        emitMarkThatKeyLeftHomeSlot(jit, tagAtHome);
    else
        exits.absent.append(jit.branchTest8(CCallHelpers::Zero, tagAtHome, TrustedImm32(CompactTable::keyLeftHomeSlot)));
    emitLoadNumberOfSlots(jit, registers);
    searchesGroups.append(jit.jump());

    isNotAtHome.link(&jit);
    if (search.findsInsertionPoint) {
        // The difference from the tag of a free slot, which is zero, is the tag.
        Jump homeIsTaken = jit.branchTest8(CCallHelpers::NonZero, tagAtHome);
        emitLoadKeysBehindSlots(jit, registers);
        exits.insertionPoints.atSlot.append(jit.jump());
        homeIsTaken.link(&jit);
        restoreTag();
        emitMarkThatKeyLeftHomeSlot(jit, tagAtHome);
    } else {
        exits.absent.append(jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(CompactTable::keyLeftHomeSlot)));
        restoreTag();
    }
    searchesGroups.append(jit.jump());
}

// cursor is the number of a slot, keys the number of slots, and the low byte of hash the tag. At an insertion point cursor is the number of a free slot.
static void emitSearchGroups(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    constexpr uint64_t tops = CompactTable::onesOfTags << 7;
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    const BaseIndex eightTags(table, cursor, CCallHelpers::TimesOne, CompactTable::offsetOfIndex());
    jit.and32(TrustedImm32(~static_cast<int32_t>(CompactTable::slotsPerGroup - 1)), cursor);
    jit.load64(eightTags, entry);
    // A lookup of a number left the bits above the hash in place: nothing has asked for them until here.
    if (!search.comparesStrings && !search.findsInsertionPoint)
        jit.and32(TrustedImm32(0xff), hash);
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
    emitLoadEntryNumberAndKeys(jit, registers, true);
    JumpList next;
    emitCompareWithEntry(jit, registers, search, next, exits);

    // The matches above that slot. table holds the shift meanwhile.
    next.link(&jit);
    emitLoadNumberOfSlots(jit, registers);
    jit.and32(TrustedImm32(CompactTable::slotsPerGroup - 1), cursor, candidate);
    jit.sub32(candidate, cursor);
    jit.load64(eightTags, entry);
    jit.move(candidate, table);
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
        emitLoadKeysBehindSlots(jit, registers);
        exits.insertionPoints.atSlot.append(jit.jump());
        isFull.link(&jit);
    } else
        exits.absent.append(jit.branchTest64(CCallHelpers::NonZero, entry, TrustedImm64(tops)));
    // The next eight.
    jit.add32(TrustedImm32(CompactTable::slotsPerGroup), cursor);
    jit.sub32(keys, TrustedImm32(1), entry);
    jit.and32(entry, cursor);
    jit.load64(eightTags, entry);
    jit.jump().linkTo(compareTags, &jit);
}

// isInImage: for a table of which nobody has asked yet whether it lies in the image. entryOfHugeTable: for a search that is not entered with a huge table, and is given the label where such a table goes in.
static void emitSearchCompactTable(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits, JumpList* isInImage = nullptr, Label* entryOfHugeTable = nullptr)
{
    const GPRReg cursor = registers.cursor;
    const GPRReg hash = registers.hash;
    const bool mayHaveBitsAboveHash = !search.comparesStrings && !search.findsInsertionPoint;
    const Address indexBits(registers.table, CompactTable::offsetOfIndexBits());
    static_assert(CompactTable::indexIsInImage == 0x80);
    Jump hasSlots;
    Jump isHuge;
    if (isInImage) {
        // One read says all of it: the byte is negative for a table in the image or a huge one, and zero for one without slots.
        jit.load8SignedExtendTo32(indexBits, cursor);
        hasSlots = jit.branch32(CCallHelpers::GreaterThan, cursor, TrustedImm32(0));
#if CPU(X86_64)
        // The comparison has set the flags.
        isHuge = Jump(jit.m_assembler.jCC(X86Assembler::ConditionS));
#else
        isHuge = jit.branchTest32(CCallHelpers::Signed, cursor);
#endif
    } else {
#if CPU(X86_64)
        hasSlots = jit.branch8(CCallHelpers::NotEqual, indexBits, TrustedImm32(0));
#else
        jit.load8SignedExtendTo32(indexBits, cursor);
        hasSlots = jit.branchTest32(CCallHelpers::NonZero, cursor);
#endif
    }
    if (mayHaveBitsAboveHash)
        jit.and32(TrustedImm32(CompactTable::hashMask), hash);
    emitProbeTags(jit, registers, search, exits);

    hasSlots.link(&jit);
    if (!isInImage) {
#if CPU(X86_64)
        // The comparison has set the flags.
        if (!entryOfHugeTable)
            isHuge = Jump(jit.m_assembler.jCC(X86Assembler::ConditionS));
        jit.load8(indexBits, cursor);
#else
        if (!entryOfHugeTable)
            isHuge = jit.branchTest32(CCallHelpers::Signed, cursor);
#endif
    }
    JumpList searchesGroups;
    emitProbeHomeSlot(jit, registers, search, searchesGroups, exits);

    // A table with more slots than the hash has values: the home slots are spread over them. The search of the first eight tags finds a key in its home slot too.
    if (isHuge.isSet())
        isHuge.link(&jit);
    if (entryOfHugeTable) {
        *entryOfHugeTable = jit.label();
        jit.load8(indexBits, cursor);
    }
    if (isInImage)
        isInImage->append(jit.branchTest32(CCallHelpers::Zero, cursor, TrustedImm32(CompactTable::indexIsHuge)));
    {
        constexpr int32_t index = CompactTable::offsetOfIndex();
        const GPRReg keys = registers.keys;
        const GPRReg entry = registers.entry;
        const GPRReg table = registers.table;
        const BaseIndex tagAtHome(table, cursor, CCallHelpers::TimesOne, index);
        if (mayHaveBitsAboveHash)
            jit.and32(TrustedImm32(CompactTable::hashMask), hash);
        jit.and32(TrustedImm32(CompactTable::numberOfIndexBits), cursor);
        jit.move(TrustedImm32(1), keys);
        jit.lshift64(cursor, keys);
        jit.move(hash, entry);
        jit.sub32(TrustedImm32(CompactTable::hashBits), cursor);
        jit.lshift32(cursor, entry);
        jit.sub32(keys, TrustedImm32(1), cursor);
        jit.and32(entry, cursor);
        emitLoadTagToSearchFor(jit, search, hash);
        // The key may be the one in the slot. The mark then says too much, which costs a search and no answer.
        if (search.findsInsertionPoint) {
            Jump homeIsTaken = jit.branchTest8(CCallHelpers::NonZero, tagAtHome);
            emitLoadKeysBehindSlots(jit, registers, CCallHelpers::TimesEight);
            exits.insertionPoints.atSlot.append(jit.jump());
            homeIsTaken.link(&jit);
            emitMarkThatKeyLeftHomeSlot(jit, tagAtHome);
        } else
            exits.absent.append(jit.branchTest8(CCallHelpers::Zero, tagAtHome));
    }

    searchesGroups.link(&jit);
    emitSearchGroups(jit, registers, search, exits);
}

static void emitFindBitsInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    CompactTableSearch search { false, false, KeyWidth::Either, lookup.findsInsertionPoint, { } };
    emitSearchCompactTable(jit, registers, search, exits, &(lookup.findsInsertionPoint ? exits.unknown : exits.absent));
}

static void emitFindStringInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, JumpList& isLeftToFormForAnyValue, HashTableExits& exits)
{
    const GPRReg table = registers.table;
    const Address flags(table, CompactTable::offsetOfKeysShift());
    const bool searchesTablesInImage = lookup.searchesTablesInImage && !lookup.findsInsertionPoint;
    // Most tables that hold strings have narrow keys. One test of the width here takes the place of one for each key that is looked at. The word begins with the index bits, whose byte is negative for a huge table.
    static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfIndexBits() + 1 && CompactTable::indexIsHuge == 0x40 && CompactTable::indexIsInImage == 0x80);
    Jump isOtherTable = jit.branchTest32(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfIndexBits()), TrustedImm32(CompactTable::indexIsInImage | (CompactTable::keysAreInImage | CompactTable::keysAreWide) << 8));
    CompactTableSearch narrowSearch { false, true, KeyWidth::Narrow, lookup.findsInsertionPoint, { } };
    Label entryOfHugeNarrowTable;
    emitSearchCompactTable(jit, registers, narrowSearch, exits, nullptr, &entryOfHugeNarrowTable);
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
        jit.branchTest8(CCallHelpers::Zero, flags, TrustedImm32(CompactTable::keysAreWide)).linkTo(entryOfHugeNarrowTable, &jit);
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
    // A lookup in slots leaves out what lies above the bits of the hash: the mask of the slots and the tests of the tag do.
    if (lookup.findsInsertionPoint)
        jit.and32(TrustedImm32(CompactTable::hashMask), registers.hash);
    emitLoadTable(jit, registers, lookup, exits);
    emitFindBitsInCompactTable(jit, registers, lookup, exits);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
