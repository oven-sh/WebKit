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
#include "AOTTrailingZeros.h"
#include "AOTVector.h"
#include "JSMap.h"
#include "JSSet.h"
#include "JSString.h"
#include <wtf/MathExtras.h>
#include <wtf/text/RapidHash.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(CompactTable::maxCapacityWithTags == sizeof(uint64_t));
static_assert(CompactTable::hashBits + StringImpl::s_flagCount == 32);
static_assert(CompactTable::probeStartFor(CompactTable::hashBits, CompactTable::hashMask) == CompactTable::hashMask && CompactTable::probeStartFor(CompactTable::hashBits + 2, 1) == 4);

static constexpr uint8_t hugeTableSignBit = 0x80;
static_assert(!CompactTable::markedIndexBits(0) && CompactTable::markedIndexBits(CompactTable::hashBits) == CompactTable::hashBits);
static_assert(CompactTable::markedIndexBits(CompactTable::hashBits + 1) & hugeTableSignBit);
static_assert((CompactTable::markedIndexBits(CompactTable::hashBits + 1) & CompactTable::numberOfIndexBits) == CompactTable::hashBits + 1);

struct IndexSlotFormat {
    unsigned entryBytes;
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

enum class KeyWidth : uint8_t { Either, Narrow, Wide };

struct CompactTableSearch {
    bool comparesStrings;
    KeyWidth keyWidth;
    bool findsInsertionPoint;
    JumpList needsCharacters;
    bool isInImage { false };
    bool asksForPresenceAlone { false };
    std::optional<GPRReg> lengthOfCharactersSought { };
    JumpList comparesCharactersInImage { };
    bool keyIsSubstring { false };
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

void emitHashOfKeyBits(StubAssembler& jit, GPRReg key, GPRReg hash, GPRReg scratch)
{
    ASSERT(noOverlap(key, hash, scratch));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.keyMultiplierOfTables)), hash);
#if CPU(ARM64)
    jit.mul64(key, hash, scratch);
    jit.uMulHigh64(key, hash, hash);
#else
    ASSERT(hash == X86Registers::eax && scratch == X86Registers::edx);
    jit.m_assembler.mulq_r(key);
#endif
    jit.xor64(scratch, hash);
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
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg tag = ownedScratch;
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

static void emitReloadTable(StubAssembler& jit, const HashTableRegisters& registers)
{
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), registers.table);
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

void emitLoadKeysOfCompactTable(StubAssembler& jit, const HashTableRegisters& registers)
{
    emitLoadKeys(jit, registers);
}

static void emitCompareWithEntry(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, JumpList& next, HashTableExits& exits, const ScopedLambda<void()>* emitFound = nullptr)
{
    const GPRReg key = registers.key;
    const GPRReg table = registers.table;
    const GPRReg candidate = registers.candidate;
    if (search.isInImage) {
        JumpList& found = search.asksForPresenceAlone ? exits.foundInCompactTable : exits.foundInImage;
        jit.load32(Address(table, CompactTable::offsetOfCapacity()), candidate);
        jit.add32(registers.entry, candidate);
        jit.load32(BaseIndex(registers.keys, candidate, CCallHelpers::TimesFour), candidate);
        static_assert(InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
        jit.load32(BaseIndex(instanceGPR, candidate, CCallHelpers::TimesFour, InstanceLayout::offsetOfStringCells), candidate);
        search.needsCharacters.append(jit.branchTest32(CCallHelpers::Zero, candidate));
        decodeCell32(jit, candidate, candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
        found.append(jit.branchPtr(CCallHelpers::Equal, candidate, Address(key, JSString::offsetOfValue())));
        search.comparesCharactersInImage.append(jit.jump());
        return;
    }

    if (!search.comparesStrings) {
        ASSERT(search.keyWidth == KeyWidth::Either);
        Jump isWide = jit.branchTest8(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
        jit.load32(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesFour), candidate);
        Jump isImmediate = jit.branch32(CCallHelpers::Below, candidate, TrustedImm32(minCell32));
        decodeCell32(jit, candidate, candidate);
        isImmediate.link(&jit);
        exits.foundAmongNarrowKeys.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        next.append(jit.jump());

        isWide.link(&jit);
        jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), candidate);
        exits.foundAmongWideKeys.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        return;
    }

    if (search.lengthOfCharactersSought) {
        ASSERT(search.keyWidth == KeyWidth::Narrow);
        jit.load32(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesFour), candidate);
        next.append(jit.branch32(CCallHelpers::Below, candidate, TrustedImm32(minCell32)));
        decodeCell32(jit, candidate, candidate);
        next.append(jit.branchIfNotType(candidate, StringType));
        jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
        search.needsCharacters.append(jit.branch32(CCallHelpers::Equal, *search.lengthOfCharactersSought, Address(candidate, StringImpl::lengthMemoryOffset())));
        return;
    }

    ASSERT(search.keyWidth != KeyWidth::Either);
    JumpList& found = search.keyWidth == KeyWidth::Narrow ? exits.foundAmongNarrowKeys : exits.foundAmongWideKeys;
    if (search.keyWidth == KeyWidth::Narrow) {
        const BaseIndex word(registers.keys, registers.entry, CCallHelpers::TimesFour);
        jit.load32(word, candidate);
        decodeCell32(jit, candidate, candidate);
        if (emitFound) {
            Jump isOtherCell = jit.branch64(CCallHelpers::NotEqual, candidate, key);
            (*emitFound)();
            isOtherCell.link(&jit);
        } else
            found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        next.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32)));
    } else {
        jit.load64(BaseIndex(registers.keys, registers.entry, CCallHelpers::TimesEight), candidate);
        found.append(jit.branch64(CCallHelpers::Equal, candidate, key));
        next.append(jit.branchTest64(CCallHelpers::Zero, candidate));
        next.append(jit.branchIfNotCell(candidate));
    }
    next.append(jit.branchIfNotType(candidate, StringType));
    jit.loadPtr(Address(candidate, JSString::offsetOfValue()), candidate);
    if (search.keyIsSubstring) {
        jit.load32(Address(key, JSRopeString::offsetOfFiber0() + static_cast<ptrdiff_t>(sizeof(uint32_t))), table);
        jit.lshift32(TrustedImm32(StringImpl::s_flagCount), table);
    } else {
        found.append(jit.branchPtr(CCallHelpers::Equal, candidate, Address(key, JSString::offsetOfValue())));
        jit.loadPtr(Address(key, JSString::offsetOfValue()), table);
        jit.load32(Address(table, StringImpl::flagsOffset()), table);
    }
    jit.xor32(Address(candidate, StringImpl::flagsOffset()), table);
    search.needsCharacters.append(jit.branch32(CCallHelpers::Below, table, TrustedImm32(1 << StringImpl::s_flagCount)));
    emitReloadTable(jit, registers);
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

static void emitLoadCharactersOfSubstring(StubAssembler& jit, GPRReg substring, GPRReg characters, GPRReg scratch)
{
    ASSERT(noOverlap(substring, characters, scratch));
    static_assert(JSRopeString::offsetOfFiber1Lower() + static_cast<ptrdiff_t>(sizeof(uint32_t)) == JSRopeString::offsetOfFiber2());
    jit.load16(Address(substring, JSRopeString::offsetOfFiber2()), characters);
    jit.load32(Address(substring, JSRopeString::offsetOfFiber1Lower()), scratch);
    jit.lshift64(TrustedImm32(32), characters);
    jit.or64(scratch, characters);
    jit.loadPtr(Address(characters, JSString::offsetOfValue()), characters);
    Jump baseIsRope = jit.branchIfRopeStringImpl(characters);
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        baseIsRope.link(&jit);
        jit.breakpoint();
    });
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.load64(Address(substring, JSRopeString::offsetOfFiber2()), scratch);
    jit.urshift64(TrustedImm32(16), scratch);
    jit.add64(scratch, characters);
}

static void emitRecordSubstringHit(StubAssembler& jit, GPRReg substring)
{
    jit.add32(TrustedImm32(static_cast<int32_t>(CompactTable::firstSubstringHit)), Address(substring, JSRopeString::offsetOfFiber0() + static_cast<ptrdiff_t>(sizeof(uint32_t))));
}

static void emitCompareCharactersOfSubstring(StubAssembler& jit, const HashTableRegisters& registers, JumpList& needsCharacters, JumpList& found, HashTableExits& exits)
{
    const GPRReg candidate = registers.candidate;
    const GPRReg length = registers.table;
    JumpList areSame;
    needsCharacters.link(&jit);
    jit.load32(Address(candidate, StringImpl::lengthMemoryOffset()), length);
    exits.unknown.append(jit.branch32(CCallHelpers::NotEqual, length, Address(registers.key, JSRopeString::offsetOfLength())));
    exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, Address(candidate, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(candidate, StringImpl::dataOffset()), candidate);
    emitLoadCharactersOfSubstring(jit, registers.key, registers.hash, registers.cursor);
    emitCompareLatin1Characters(jit, candidate, registers.hash, length, registers.cursor, registers.keys, areSame, exits.unknown);
    areSame.link(&jit);
    emitRecordSubstringHit(jit, registers.key);
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), registers.table);
    emitLoadKeys(jit, registers);
    found.append(jit.jump());
}

static void emitCompareCharactersOfKey(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, JumpList& needsCharacters, JumpList& found, HashTableExits& exits)
{
    if (lookup.keyIsSubstring) {
        emitCompareCharactersOfSubstring(jit, registers, needsCharacters, found, exits);
        return;
    }
    const bool keepsSlot = lookup.keepsSlot;
    const GPRReg impl = registers.hash;
    const GPRReg candidate = registers.candidate;
    const GPRReg length = registers.table;
    needsCharacters.link(&jit);
    if (keepsSlot) {
        jit.lshift64(TrustedImm32(32), registers.cursor);
        jit.or64(registers.cursor, registers.entry);
    }
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
    if (keepsSlot) {
        jit.urshift64(registers.entry, TrustedImm32(32), registers.cursor);
        jit.zeroExtend32ToWord(registers.entry, registers.entry);
    }
    found.append(jit.jump());
}

static void emitCompareCharactersInImage(StubAssembler& jit, const HashTableRegisters& registers, bool keyIsSubstring, JumpList& comparesCharacters, JumpList& found, HashTableExits& exits)
{
    const GPRReg impl = registers.hash;
    const GPRReg candidate = registers.candidate;
    const GPRReg length = registers.cursor;
    comparesCharacters.link(&jit);
    JumpList hasWideCharacters;
    jit.load32(Address(candidate, StringImpl::lengthMemoryOffset()), length);
    if (keyIsSubstring) {
        exits.unknown.append(jit.branch32(CCallHelpers::NotEqual, length, Address(registers.key, JSRopeString::offsetOfLength())));
        exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, Address(candidate, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    } else {
        jit.loadPtr(Address(registers.key, JSString::offsetOfValue()), impl);
        exits.unknown.append(jit.branch32(CCallHelpers::NotEqual, length, Address(impl, StringImpl::lengthMemoryOffset())));
        hasWideCharacters.append(jit.branchTest32(CCallHelpers::Zero, Address(candidate, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        hasWideCharacters.append(jit.branchTest32(CCallHelpers::Zero, Address(impl, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
        jit.loadPtr(Address(impl, StringImpl::dataOffset()), impl);
    }
    jit.loadPtr(Address(candidate, StringImpl::dataOffset()), candidate);
    jit.preserving({ registers.table, registers.keys }, [&] {
        if (keyIsSubstring)
            emitLoadCharactersOfSubstring(jit, registers.key, impl, registers.table);
        JumpList areSame;
        JumpList differ;
        emitCompareLatin1Characters(jit, candidate, impl, length, registers.table, registers.keys, areSame, differ);
        differ.link(&jit);
        jit.move(TrustedImm32(0), length);
        Jump isCompared = jit.jump();
        areSame.link(&jit);
        jit.move(TrustedImm32(1), length);
        isCompared.link(&jit);
    });
    exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, length));
    if (keyIsSubstring)
        emitRecordSubstringHit(jit, registers.key);
    found.append(jit.jump());
    if (keyIsSubstring)
        return;

    hasWideCharacters.link(&jit);
    RELEASE_ASSERT(noOverlap(leafFunctionGPR, registers.table, registers.keys));
    jit.preserving({ registers.table, registers.keys }, [&] {
        emitCallStringLeaf(jit, registers, Entry::operationAOTStringImplsAreEqual, candidate, impl);
    });
    exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, leafFunctionGPR));
    found.append(jit.jump());
}

static void emitProbeTags(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    JumpList& absent = search.findsInsertionPoint ? exits.insertionPoints.amongTags : exits.absent;
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
        jit.addPtr(TrustedImm32(index + CompactTable::maxCapacityWithTags), table, keys);
        absent.append(jit.branchTest64(CCallHelpers::Zero, matches));
    } else {
#if CPU(X86_64)
        absent.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionE)));
#else
        absent.append(jit.branchTest64(CCallHelpers::Zero, matches));
#endif
        jit.addPtr(TrustedImm32(index + CompactTable::maxCapacityWithTags), table, keys);
    }

    Label nextMatch = jit.label();
    countTrailingZerosOfNonZero(jit, matches, entry);
    jit.urshift32(TrustedImm32(3), entry);
    absent.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, Address(table, CompactTable::offsetOfUsed())));
    JumpList next;
    emitCompareWithEntry(jit, registers, search, next, exits, exits.emitFoundAmongNarrowKeys);
    next.link(&jit);
    jit.sub64(matches, TrustedImm32(1), candidate);
    jit.and64(candidate, matches);
    jit.branchTest64(CCallHelpers::NonZero, matches).linkTo(nextMatch, &jit);
    absent.append(jit.jump());
}

static void emitCompareWithFewKeys(StubAssembler& jit, const HashTableRegisters& registers, JumpList& hasMoreKeys, HashTableExits& exits)
{
    using Width = CompactTable::Width;
    constexpr uint32_t few = CompactTable::initialCapacity;
    constexpr int32_t offsetOfKeys = CompactTable::offsetOfIndex() + CompactTable::maxCapacityWithTags;
    const GPRReg key = registers.key;
    const GPRReg table = registers.table;
    const GPRReg narrowKey = registers.candidate;
    const Address used(table, CompactTable::offsetOfUsed());
    auto compareWithEach = [&](Width width, JumpList& found) {
        auto finishFound = [&](uint32_t entry) {
            if (width == Width::Narrow)
                exits.absent.append(jit.branchTest64(CCallHelpers::NonZero, key, TrustedImm32(MarkedBlock::atomSize - 1)));
            jit.move(TrustedImm32(entry), registers.entry);
            jit.addPtr(TrustedImm32(offsetOfKeys), table, registers.keys);
            found.append(jit.branch32(CCallHelpers::Above, used, TrustedImm32(entry)));
            exits.absent.append(jit.jump());
        };
        std::array<Jump, few - 1> isSame;
        for (uint32_t entry = 0; entry < few; ++entry) {
            if (entry && !(entry % 2))
                exits.absent.append(jit.branch32(CCallHelpers::BelowOrEqual, used, TrustedImm32(entry)));
            const auto relation = entry < isSame.size() ? CCallHelpers::Equal : CCallHelpers::NotEqual;
            const Address word(table, offsetOfKeys + static_cast<int32_t>(entry * static_cast<uint32_t>(width)));
            Jump jump = width == Width::Wide ? jit.branch64(relation, word, key) : jit.branch32(relation, word, narrowKey);
            if (entry < isSame.size())
                isSame[entry] = jump;
            else
                exits.absent.append(jump);
        }
        finishFound(few - 1);
        for (uint32_t entry = 0; entry < isSame.size(); ++entry) {
            isSame[entry].link(&jit);
            finishFound(entry);
        }
    };

    hasMoreKeys.append(jit.branch32(CCallHelpers::Above, used, TrustedImm32(few)));
    Jump keysAreNarrow = jit.branchTest8(CCallHelpers::Zero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide));
    compareWithEach(Width::Wide, exits.foundAmongWideKeys);

    keysAreNarrow.link(&jit);
    Jump notCell = jit.branchIfNotCell(key);
    encodeCellOfProgram(jit, key, narrowKey);
    compareWithEach(Width::Narrow, exits.foundAmongNarrowKeys);

    notCell.link(&jit);
    exits.absent.append(jit.branch64(CCallHelpers::AboveOrEqual, key, TrustedImm32(minCell32)));
    hasMoreKeys.append(jit.jump());
}

static void emitLoadTagToSearchFor(StubAssembler& jit, const CompactTableSearch& search, GPRReg hash)
{
    if (search.findsInsertionPoint) {
        emitLoadTagOfSlot(jit, hash);
        return;
    }
    jit.urshift32(TrustedImm32(CompactTable::tagShiftOfSlots - 1), hash);
    jit.or32(TrustedImm32(CompactTable::takenSlot | CompactTable::keyLeftHomeSlot), hash);
}

static void emitLoadHomeSlot(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg slot = registers.cursor;
    jit.sub32(registers.keys, TrustedImm32(1), slot);
    jit.and32(registers.hash, slot);
}

static void emitLoadKeysBehindSlots(StubAssembler& jit, const HashTableRegisters& registers, CCallHelpers::Scale bytesOfSlot)
{
    jit.getEffectiveAddress(BaseIndex(registers.table, registers.keys, bytesOfSlot, CompactTable::offsetOfIndex()), registers.keys);
}

static void emitLoadKeysBehindSlots(StubAssembler& jit, const HashTableRegisters& registers)
{
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    emitLoadKeysBehindSlots(jit, registers, shortSlotFormat.bytesOfSlot);
    Label isLoaded = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        hasLongSlots.link(&jit);
        Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
        emitLoadKeysBehindSlots(jit, registers, longSlotFormat.bytesOfSlot);
        jit.jump().linkTo(isLoaded, &jit);
        hasHugeSlots.link(&jit);
        emitLoadKeysBehindSlots(jit, registers, CCallHelpers::TimesEight);
        jit.jump().linkTo(isLoaded, &jit);
    });
}

static void emitLoadNumberOfSlots(StubAssembler& jit, const HashTableRegisters& registers)
{
    const GPRReg slots = registers.keys;
    jit.sub64(registers.keys, registers.table, slots);
    jit.sub64(TrustedImm32(CompactTable::offsetOfIndex()), slots);
    Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
    jit.urshift64(TrustedImm32(1), slots);
    Label isLoaded = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        hasLongSlots.link(&jit);
        Jump hasHugeSlots = jit.branch32(CCallHelpers::Above, Address(registers.table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithLongSlots));
        jit.urshift64(TrustedImm32(2), slots);
        jit.jump().linkTo(isLoaded, &jit);
        hasHugeSlots.link(&jit);
        jit.urshift64(TrustedImm32(3), slots);
        jit.jump().linkTo(isLoaded, &jit);
    });
}

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

static void emitCompareEightTags(StubAssembler& jit, const HashTableRegisters& registers, GPRReg tags, GPRReg matches, GPRReg taken)
{
    jit.xor64(registers.hash, tags, taken);
    jit.move(TrustedImm64(-static_cast<int64_t>(CompactTable::belowDifferenceOfOtherTags)), matches);
    jit.add64(taken, matches);
    jit.not64(taken);
    jit.and64(taken, matches);
    jit.and64(TrustedImm64(CompactTable::onesOfTags << 7), matches);
}

static void emitProbeHomeSlot(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, JumpList& searchesGroups, HashTableExits& exits)
{
    constexpr int32_t index = CompactTable::offsetOfIndex();
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    const BaseIndex tagAtHome(table, cursor, CCallHelpers::TimesOne, index);
    jit.move(TrustedImm32(1), keys);
    jit.lshift32(cursor, keys);
    emitLoadHomeSlot(jit, registers);
    emitLoadTagToSearchFor(jit, search, hash);

#if CPU(X86_64)
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
    JumpList hasOtherKey;
    if (search.comparesStrings || search.isInImage || search.findsInsertionPoint) {
        emitLoadEntryNumberAndKeys(jit, registers, false);
        emitCompareWithEntry(jit, registers, search, hasOtherKey, exits, exits.emitFoundAmongNarrowKeys);
    } else {
        Jump hasLongSlots = jit.branch32(CCallHelpers::Above, Address(table, CompactTable::offsetOfCapacity()), TrustedImm32(CompactTable::maxCapacityWithShortSlots));
        emitLoadEntryNumber(jit, registers, shortSlotFormat);
        emitLoadKeysBehindSlots(jit, registers, shortSlotFormat.bytesOfSlot);
        emitCompareWithEntry(jit, registers, search, hasOtherKey, exits);
        hasOtherKey.append(jit.jump());
        hasLongSlots.link(&jit);
        emitLoadEntryNumber(jit, registers, longSlotFormat);
        emitLoadKeysBehindSlots(jit, registers, longSlotFormat.bytesOfSlot);
        emitCompareWithEntry(jit, registers, search, hasOtherKey, exits);
    }
    hasOtherKey.link(&jit);
    restoreTag();
    if (!search.findsInsertionPoint)
        exits.absent.append(jit.branchTest8(CCallHelpers::Zero, tagAtHome, TrustedImm32(CompactTable::keyLeftHomeSlot)));
    emitLoadNumberOfSlots(jit, registers);
    searchesGroups.append(jit.jump());

    isNotAtHome.link(&jit);
    if (search.findsInsertionPoint) {
        Jump homeIsTaken = jit.branchTest8(CCallHelpers::NonZero, tagAtHome);
        emitLoadKeysBehindSlots(jit, registers);
        exits.insertionPoints.atSlot.append(jit.jump());
        homeIsTaken.link(&jit);
        restoreTag();
    } else if (exits.emitAbsent) {
        Jump mayBeElsewhere = jit.branchTest32(CCallHelpers::Zero, difference, TrustedImm32(CompactTable::keyLeftHomeSlot));
        (*exits.emitAbsent)();
        mayBeElsewhere.link(&jit);
        restoreTag();
    } else {
        exits.absent.append(jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(CompactTable::keyLeftHomeSlot)));
        restoreTag();
    }
    searchesGroups.append(jit.jump());
}

static void emitSearchGroups(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits)
{
    constexpr uint64_t tops = CompactTable::onesOfTags << 7;
    auto [owner, key, hash, table, cursor, keys, entry, candidate] = registers;
    const BaseIndex eightTags(table, cursor, CCallHelpers::TimesOne, CompactTable::offsetOfIndex());
    if (search.findsInsertionPoint)
        jit.move(cursor, candidate);
    jit.and32(TrustedImm32(~static_cast<int32_t>(CompactTable::slotsPerGroup - 1)), cursor);
    jit.load64(eightTags, entry);
    if (search.findsInsertionPoint)
        emitMarkThatKeyLeftHomeSlot(jit, BaseIndex(table, candidate, CCallHelpers::TimesOne, CompactTable::offsetOfIndex()));
    if (!search.comparesStrings && !search.findsInsertionPoint)
        jit.and32(TrustedImm32(0xff), hash);
    jit.move(TrustedImm64(CompactTable::onesOfTags), candidate);
    jit.mul64(candidate, hash);

    Label compareTags = jit.label();
    emitCompareEightTags(jit, registers, entry, candidate, entry);
#if CPU(X86_64)
    Jump hasNoMatch = Jump(jit.m_assembler.jCC(X86Assembler::ConditionE));
#else
    Jump hasNoMatch = jit.branchTest64(CCallHelpers::Zero, candidate);
#endif

    Label nextMatch = jit.label();
    countTrailingZerosOfNonZero(jit, candidate, candidate);
    jit.urshift32(TrustedImm32(3), candidate);
    jit.add32(candidate, cursor);
    emitLoadEntryNumberAndKeys(jit, registers, true);
    JumpList next;
    emitCompareWithEntry(jit, registers, search, next, exits);

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
    emitReloadTable(jit, registers);
    jit.branchTest64(CCallHelpers::NonZero, candidate).linkTo(nextMatch, &jit);

    hasNoMatch.link(&jit);
    jit.not64(entry);
    if (search.findsInsertionPoint) {
        jit.and64(TrustedImm64(tops), entry);
        Jump isFull = jit.branchTest64(CCallHelpers::Zero, entry);
        countTrailingZerosOfNonZero(jit, entry, candidate);
        jit.urshift32(TrustedImm32(3), candidate);
        jit.add32(candidate, cursor);
        emitLoadKeysBehindSlots(jit, registers);
        exits.insertionPoints.atSlot.append(jit.jump());
        isFull.link(&jit);
    } else
        exits.absent.append(jit.branchTest64(CCallHelpers::NonZero, entry, TrustedImm64(tops)));
    jit.add32(TrustedImm32(CompactTable::slotsPerGroup), cursor);
    jit.sub32(keys, TrustedImm32(1), entry);
    jit.and32(entry, cursor);
    jit.load64(eightTags, entry);
    jit.jump().linkTo(compareTags, &jit);
}

static void emitSearchCompactTable(StubAssembler& jit, const HashTableRegisters& registers, CompactTableSearch& search, HashTableExits& exits, Label* entryOfHugeTable = nullptr, JumpList* isInImage = nullptr)
{
    const GPRReg cursor = registers.cursor;
    const GPRReg hash = registers.hash;
    const bool isByBits = !search.comparesStrings && !search.findsInsertionPoint;
    const Address indexBits(registers.table, CompactTable::offsetOfIndexBits());
    Jump hasNoSlots;
    if (!entryOfHugeTable) {
        jit.load8SignedExtendTo32(indexBits, cursor);
        hasNoSlots = jit.branch32(CCallHelpers::LessThanOrEqual, cursor, TrustedImm32(0));
    } else {
#if CPU(X86_64)
        hasNoSlots = jit.branch8(CCallHelpers::Equal, indexBits, TrustedImm32(0));
        jit.load8(indexBits, cursor);
#else
        jit.load8SignedExtendTo32(indexBits, cursor);
        hasNoSlots = jit.branchTest32(CCallHelpers::Zero, cursor);
#endif
    }
    if (isByBits)
        emitHashOfKeyBits(jit, registers.key, hash, registers.candidate);
    JumpList searchesGroups;
    emitProbeHomeSlot(jit, registers, search, searchesGroups, exits);

    Label hugeTable = jit.label();
    if (isInImage)
        isInImage->append(jit.branchTest8(CCallHelpers::NonZero, Address(registers.table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreInImage)));
    if (entryOfHugeTable) {
        *entryOfHugeTable = jit.label();
        jit.load8(indexBits, cursor);
    }
    {
        constexpr int32_t index = CompactTable::offsetOfIndex();
        const GPRReg keys = registers.keys;
        const GPRReg entry = registers.entry;
        const GPRReg table = registers.table;
        const BaseIndex tagAtHome(table, cursor, CCallHelpers::TimesOne, index);
        if (isByBits) {
            emitHashOfKeyBits(jit, registers.key, hash, registers.candidate);
            jit.and32(TrustedImm32(CompactTable::hashMask), hash);
        }
        jit.and32(TrustedImm32(CompactTable::numberOfIndexBits), cursor);
        jit.move(TrustedImm32(1), keys);
        jit.lshift64(cursor, keys);
        jit.move(hash, entry);
        jit.sub32(TrustedImm32(CompactTable::hashBits), cursor);
        jit.lshift32(cursor, entry);
        jit.sub32(keys, TrustedImm32(1), cursor);
        jit.and32(entry, cursor);
        emitLoadTagToSearchFor(jit, search, hash);
        if (search.findsInsertionPoint) {
            Jump homeIsTaken = jit.branchTest8(CCallHelpers::NonZero, tagAtHome);
            emitLoadKeysBehindSlots(jit, registers, CCallHelpers::TimesEight);
            exits.insertionPoints.atSlot.append(jit.jump());
            homeIsTaken.link(&jit);
        } else
            exits.absent.append(jit.branchTest8(CCallHelpers::Zero, tagAtHome));
    }

    searchesGroups.link(&jit);
    emitSearchGroups(jit, registers, search, exits);

    hasNoSlots.link(&jit);
    if (!entryOfHugeTable) {
#if CPU(X86_64)
        Jump(jit.m_assembler.jCC(X86Assembler::ConditionS)).linkTo(hugeTable, &jit);
#else
        jit.branchTest32(CCallHelpers::Signed, cursor).linkTo(hugeTable, &jit);
#endif
    }
    if (isByBits) {
        JumpList hasMoreKeys;
        emitCompareWithFewKeys(jit, registers, hasMoreKeys, exits);
        hasMoreKeys.link(&jit);
        emitHashOfKeyBits(jit, registers.key, hash, registers.candidate);
        jit.and32(TrustedImm32(CompactTable::hashMask), hash);
    }
    emitProbeTags(jit, registers, search, exits);
}

static void emitFindBitsInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, HashTableExits& exits)
{
    CompactTableSearch search { false, KeyWidth::Either, lookup.findsInsertionPoint, { } };
    emitSearchCompactTable(jit, registers, search, exits, nullptr, &(lookup.findsInsertionPoint ? exits.unknown : exits.absent));
}

static void emitFindStringInCompactTable(StubAssembler& jit, const HashTableRegisters& registers, HashTableLookup lookup, JumpList& isLeftToFormForAnyValue, HashTableExits& exits)
{
    const GPRReg table = registers.table;
    const Address flags(table, CompactTable::offsetOfKeysShift());
    static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfIndexBits() + 1);
    Jump isOtherTable = jit.branchTest32(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfIndexBits()), TrustedImm32(hugeTableSignBit | CompactTable::keysAreWide << 8));
    CompactTableSearch narrowSearch { true, KeyWidth::Narrow, lookup.findsInsertionPoint, { } };
    narrowSearch.keyIsSubstring = lookup.keyIsSubstring;
    Label entryOfHugeNarrowTable;
    emitSearchCompactTable(jit, registers, narrowSearch, exits, &entryOfHugeNarrowTable);
    JumpList needsCharacters = narrowSearch.needsCharacters;

    const bool searchesTablesInImage = lookup.searchesTablesInImage && !lookup.findsInsertionPoint;
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
        CompactTableSearch wideSearch { true, KeyWidth::Wide, lookup.findsInsertionPoint, { } };
        wideSearch.keyIsSubstring = lookup.keyIsSubstring;
        emitSearchCompactTable(jit, registers, wideSearch, exits);
        needsCharacters.append(wideSearch.needsCharacters);
    }

    if (searchesTablesInImage) {
        isInImage.link(&jit);
        jit.loadPtr(Address(table, CompactTable::offsetOfIndex()), table);
        CompactTableSearch imageSearch { true, KeyWidth::Either, false, { }, true, lookup.asksForPresenceAlone };
        emitSearchCompactTable(jit, registers, imageSearch, exits);
        exits.unknown.append(imageSearch.needsCharacters);
        emitCompareCharactersInImage(jit, registers, lookup.keyIsSubstring, imageSearch.comparesCharactersInImage, lookup.asksForPresenceAlone ? exits.foundInCompactTable : exits.foundInImage, exits);
    } else
        exits.unknown.append(isInImage);

    emitCompareCharactersOfKey(jit, registers, lookup, needsCharacters, lookup.comparison == KeyComparison::String ? exits.foundAmongNarrowKeys : exits.foundInCompactTable, exits);
}

static void emitClassifyOtherKeys(StubAssembler& jit, const HashTableRegisters& registers, Jump notCell, Jump notString, JumpList& isComparedByBits, HashTableExits& exits)
{
    const GPRReg key = registers.key;
    notCell.link(&jit);
    isComparedByBits.append(jit.branchIfNotNumber(key));
    isComparedByBits.append(jit.branchIfInt32(key));
    const FPRReg number = jit.allocateScratch(FPRInfo::argumentFPR0);
    const FPRReg rounded = jit.allocateScratch(FPRInfo::argumentFPR1);
    unboxDouble(jit, key, registers.entry, number);
    jit.truncateDoubleToInt32(number, registers.entry);
    jit.convertInt32ToDouble(registers.entry, rounded);
    isComparedByBits.append(jit.branchDouble(CCallHelpers::DoubleNotEqualAndOrdered, number, rounded));
    exits.unknown.append(jit.jump());

    notString.link(&jit);
    exits.unknown.append(jit.branchIfType(key, HeapBigIntType));
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
#if CPU(ADDRESS64)
    if (lookup.findsHashedSubstrings && !lookup.findsInsertionPoint && !lookup.keepsSlot) {
        exits.unknown.append(jit.branchTestPtr(CCallHelpers::Zero, impl, TrustedImm32(JSRopeString::isSubstringInPointer)));
        exits.unknown.append(jit.branchTestPtr(CCallHelpers::Zero, impl, TrustedImm32(JSRopeString::is8BitInPointer)));
        jit.urshift64(impl, TrustedImm32(JSRopeString::substringHashShift), hash);
        exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, hash));
        exits.unknown.append(jit.branchTest32(CCallHelpers::NonZero, hash, TrustedImm32(static_cast<int32_t>(CompactTable::secondSubstringHit))));
        jit.and32(TrustedImm32(CompactTable::hashMask), hash);
        if (lookup.comparison == KeyComparison::String) {
            isLeftToFormForAnyValue.append(jit.jump());
            return;
        }
        HashTableLookup lookupOfSubstring = lookup;
        lookupOfSubstring.keyIsSubstring = true;
        emitReloadTable(jit, registers);
        emitFindStringInCompactTable(jit, registers, lookupOfSubstring, isLeftToFormForAnyValue, exits);
        return;
    }
#endif
    exits.unknown.append(jit.jump());
}

static void emitMultiplyInto(StubAssembler& jit, GPRReg low, GPRReg high, GPRReg scratch)
{
#if CPU(ARM64)
    jit.mul64(low, high, scratch);
    jit.uMulHigh64(low, high, high);
    jit.move(scratch, low);
#else
    UNUSED_PARAM(scratch);
    RELEASE_ASSERT(low == X86Registers::eax && high == X86Registers::edx);
    jit.m_assembler.mulq_r(high);
#endif
}

static consteval uint64_t mixOfHashOfCharacters(uint64_t a, uint64_t b)
{
    UInt128 product = static_cast<UInt128>(a) * b;
    return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
}

void emitFindLatin1CharactersInHashTable(StubAssembler& jit, const HashTableRegisters& registers, GPRReg length, HashTableExits& exits)
{
    constexpr std::array<uint64_t, 3> secret = RapidHash::secret;
    constexpr uint32_t mostCharactersHashedHere = 48;
    const GPRReg characters = registers.key;
    const GPRReg a = registers.hash;
    const GPRReg b = registers.candidate;
    const GPRReg first = registers.cursor;
    const GPRReg second = registers.entry;
    const GPRReg seed = registers.table;

    jit.zeroExtend32ToWord(length, length);
    if (jit.options().useWideReferences) {
        exits.unknown.append(jit.jump());
        return;
    }

    jit.move(TrustedImm64(mixOfHashOfCharacters(secret[0], secret[1])), seed);
    jit.xor64(length, seed);
    Jump isLong = jit.branch32(CCallHelpers::Above, length, TrustedImm32(16));
    Jump isShort = jit.branch32(CCallHelpers::Below, length, TrustedImm32(4));
    jit.compare32(CCallHelpers::AboveOrEqual, length, TrustedImm32(8), first);
    jit.lshift32(TrustedImm32(2), first);
    jit.load32(Address(characters), a);
    jit.lshift64(TrustedImm32(32), a);
    jit.sub32(length, TrustedImm32(4), second);
    jit.load32(BaseIndex(characters, second, CCallHelpers::TimesOne), b);
    jit.or64(b, a);
    jit.sub32(first, second);
    jit.load32(BaseIndex(characters, first, CCallHelpers::TimesOne), b);
    jit.lshift64(TrustedImm32(32), b);
    jit.load32(BaseIndex(characters, second, CCallHelpers::TimesOne), second);
    jit.or64(second, b);

    Label isRead = jit.label();
    jit.xor64(seed, b);
    jit.move(TrustedImm64(secret[1]), seed);
    jit.xor64(seed, a);
    emitMultiplyInto(jit, a, b, second);
    jit.xor64(seed, b);
    jit.move(TrustedImm64(secret[0]), seed);
    jit.xor64(length, seed);
    jit.xor64(seed, a);
    emitMultiplyInto(jit, a, b, second);
    jit.xor32(b, a);
    jit.and32(TrustedImm32(CompactTable::hashMask), a);
    Jump isZero = jit.branchTest32(CCallHelpers::Zero, a);
    Label isHashed = jit.label();

    const GPRReg table = registers.table;
    jit.loadPtr(Address(registers.owner, offsetOfHashTableStorage()), table);
    exits.absent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfIndexBits() + 1);
    Jump isOtherTable = jit.branchTest32(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfIndexBits()), TrustedImm32(hugeTableSignBit | (CompactTable::keysAreWide | CompactTable::keysAreInImage) << 8));
    CompactTableSearch search { true, KeyWidth::Narrow, false, { } };
    search.lengthOfCharactersSought = length;
    Label entryOfHugeTable;
    emitSearchCompactTable(jit, registers, search, exits, &entryOfHugeTable);

    isOtherTable.link(&jit);
    exits.unknown.append(jit.branchTest8(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfKeysShift()), TrustedImm32(CompactTable::keysAreWide | CompactTable::keysAreInImage)));
    jit.jump().linkTo(entryOfHugeTable, &jit);

    const GPRReg other = registers.candidate;
    JumpList areSame;
    search.needsCharacters.link(&jit);
    exits.unknown.append(jit.branchTest32(CCallHelpers::Zero, Address(other, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.loadPtr(Address(other, StringImpl::dataOffset()), other);
    jit.move(length, table);
    emitCompareLatin1Characters(jit, other, characters, table, registers.hash, registers.cursor, areSame, exits.unknown);
    areSame.link(&jit);
    emitReloadTable(jit, registers);
    exits.foundAmongNarrowKeys.append(jit.jump());

    isZero.link(&jit);
    jit.move(TrustedImm32(1 << (CompactTable::hashBits - 1)), a);
    jit.jump().linkTo(isHashed, &jit);

    isShort.link(&jit);
    jit.move(TrustedImm32(0), a);
    jit.move(TrustedImm32(0), b);
    jit.branchTest32(CCallHelpers::Zero, length).linkTo(isRead, &jit);
    jit.load8(Address(characters), a);
    jit.lshift64(TrustedImm32(56), a);
    jit.urshift32(length, TrustedImm32(1), first);
    jit.load8(BaseIndex(characters, first, CCallHelpers::TimesOne), first);
    jit.lshift64(TrustedImm32(32), first);
    jit.or64(first, a);
    jit.load8(BaseIndex(characters, length, CCallHelpers::TimesOne, -1), first);
    jit.or64(first, a);
    jit.jump().linkTo(isRead, &jit);

    isLong.link(&jit);
    exits.unknown.append(jit.branch32(CCallHelpers::Above, length, TrustedImm32(mostCharactersHashedHere)));
    jit.move(TrustedImm64(secret[2]), first);
    jit.load64(Address(characters), a);
    jit.xor64(first, a);
    jit.load64(Address(characters, 8), b);
    jit.xor64(seed, b);
    jit.move(TrustedImm64(secret[1]), seed);
    jit.xor64(seed, b);
    emitMultiplyInto(jit, a, b, second);
    jit.xor64(a, b, seed);
    Jump isRest = jit.branch32(CCallHelpers::BelowOrEqual, length, TrustedImm32(32));
    jit.load64(Address(characters, 16), a);
    jit.xor64(first, a);
    jit.load64(Address(characters, 24), b);
    jit.xor64(seed, b);
    emitMultiplyInto(jit, a, b, second);
    jit.xor64(a, b, seed);
    isRest.link(&jit);
    jit.load64(BaseIndex(characters, length, CCallHelpers::TimesOne, -16), a);
    jit.load64(BaseIndex(characters, length, CCallHelpers::TimesOne, -8), b);
    jit.jump().linkTo(isRead, &jit);
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
        isLeftToFormForAnyValue.append(jit.branchIfNotCell(registers.key));
        emitFindStringKey(jit, registers, lookup, isLeftToFormForAnyValue, exits);
        isLeftToFormForAnyValue.link(&jit);
        const StubKey formForAnyValue = formForAnyValueOf(jit.key());
        Vector<GPRReg, 4> operands;
        for (Reg operand : std::span(jit.signature().operands).first(jit.signature().numberOfOperands))
            operands.append(operand.gpr());
        checkStubOperands(formForAnyValue, operands.span());
        jit.jumpToStub(formForAnyValue);
        return;
    }
    case KeyComparison::AnyValue: {
        Jump notCell = jit.branchIfNotCell(registers.key);
        Jump notString = jit.branchIfNotType(registers.key, StringType);
        emitFindStringKey(jit, registers, lookup, isLeftToFormForAnyValue, exits);
        RELEASE_ASSERT(isLeftToFormForAnyValue.empty());
        emitClassifyOtherKeys(jit, registers, notCell, notString, isComparedByBits, exits);
        break;
    }
    }

    isComparedByBits.link(&jit);
    if (lookup.findsInsertionPoint) {
        emitHashOfKeyBits(jit, registers.key, registers.hash, registers.candidate);
        jit.and32(TrustedImm32(CompactTable::hashMask), registers.hash);
    }
    emitLoadTable(jit, registers, lookup, exits);
    emitFindBitsInCompactTable(jit, registers, lookup, exits);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
