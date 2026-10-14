/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThunks.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperations.h"
#include "AOTRuntime.h"
#include "CCallHelpers.h"
#include "FunctionRareData.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "LinkBuffer.h"
#include "MegamorphicCache.h"

namespace JSC { namespace AOT {

#if CPU(ARM64) || CPU(X86_64)

namespace {

using Jump = CCallHelpers::Jump;
using JumpList = CCallHelpers::JumpList;
using Address = CCallHelpers::Address;
using BaseIndex = CCallHelpers::BaseIndex;
using TrustedImm32 = CCallHelpers::TrustedImm32;
using TrustedImm64 = CCallHelpers::TrustedImm64;
using TrustedImmPtr = CCallHelpers::TrustedImmPtr;

constexpr GPRReg argument0 = GPRInfo::argumentGPR0;
constexpr GPRReg argument1 = GPRInfo::argumentGPR1;
constexpr GPRReg argument2 = GPRInfo::argumentGPR2;
constexpr GPRReg argument3 = GPRInfo::argumentGPR3;
constexpr GPRReg argument4 = GPRInfo::argumentGPR4;
constexpr GPRReg argument5 = GPRInfo::argumentGPR5;
#if CPU(X86_64)
constexpr GPRReg scratch0 = X86Registers::eax;
constexpr GPRReg scratch1 = X86Registers::r10;
constexpr GPRReg scratch2 = X86Registers::ebx;
constexpr GPRReg scratch3 = X86Registers::r12;
constexpr GPRReg scratch4 = argument0;
constexpr GPRReg cacheGPR = argument5;
#else
constexpr GPRReg scratch0 = GPRInfo::regT9;
constexpr GPRReg scratch1 = GPRInfo::regT10;
constexpr GPRReg scratch2 = GPRInfo::regT11;
constexpr GPRReg scratch3 = GPRInfo::regT12;
constexpr GPRReg scratch4 = GPRInfo::regT13;

constexpr GPRReg cacheGPR = GPRInfo::argumentGPR7;
#endif

enum class Restores : uint8_t { CalleeSaves, CalleeSavesAndArguments };

void enter(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.push(scratch2);
    jit.push(scratch3);
    jit.push(scratch4);
    jit.push(cacheGPR);
#else
    UNUSED_PARAM(jit);
#endif
}

void leave(CCallHelpers& jit, Restores restores)
{
#if CPU(X86_64)
    if (restores == Restores::CalleeSavesAndArguments) {
        jit.pop(cacheGPR);
        jit.pop(scratch4);
    } else
        jit.addPtr(TrustedImm32(2 * sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
    jit.pop(scratch3);
    jit.pop(scratch2);
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(restores);
#endif
}

void loadInstance(CCallHelpers& jit, GPRReg result)
{
    jit.move(instanceGPR, result);
}

void loadVM(CCallHelpers& jit, GPRReg result)
{
    loadInstance(jit, result);
    jit.loadPtr(Address(result, Instance::offsetOfVM()), result);
}

void loadEntry(CCallHelpers& jit, Entry entry, GPRReg result)
{
    loadInstance(jit, result);
    jit.loadPtr(Address(result, Instance::offsetOfRuntimeTable()), result);
    jit.loadPtr(Address(result, static_cast<unsigned>(entry) * sizeof(void*)), result);
}

void tailCall(CCallHelpers& jit, Entry operation, Restores restores = Restores::CalleeSavesAndArguments)
{
    leave(jit, restores);
    loadEntry(jit, operation, GPRInfo::nonArgGPR0);
    jit.farJump(GPRInfo::nonArgGPR0, OperationPtrTag);
}

void mutatorFence(CCallHelpers& jit, GPRReg scratch)
{
#if CPU(X86_64)
    UNUSED_PARAM(jit);
    UNUSED_PARAM(scratch);
#else
    loadVM(jit, scratch);
    Jump notNeeded = jit.branchTest8(CCallHelpers::Zero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.storeFence();
    notNeeded.link(&jit);
#endif
}

void returnValue(CCallHelpers& jit, GPRReg value)
{
    jit.move(value, GPRInfo::returnValueGPR);
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    leave(jit, Restores::CalleeSaves);
    jit.ret();
}

void returnBoolean(CCallHelpers& jit, bool value)
{
    jit.move(TrustedImm32(value), GPRInfo::returnValueGPR);
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    leave(jit, Restores::CalleeSaves);
    jit.ret();
}

void returnVoid(CCallHelpers& jit)
{
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR);
    leave(jit, Restores::CalleeSaves);
    jit.ret();
}

void branchIfNotObjectValue(CCallHelpers& jit, GPRReg value, JumpList& slowCases)
{
    slowCases.append(jit.branchIfNotCell(value));
    slowCases.append(jit.branchIfNotObject(value));
}

void loadIdentifier(CCallHelpers& jit, GPRReg index, GPRReg result)
{
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifierPlaces()), result);
    jit.zeroExtend32ToWord(index, scratch4);
    jit.load16(BaseIndex(result, scratch4, CCallHelpers::TimesTwo), scratch4);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfProgramIdentifiers()), result);
    jit.loadPtr(BaseIndex(result, scratch4, CCallHelpers::TimesEight), result);
}

void loadAtomName(CCallHelpers& jit, GPRReg value, GPRReg result, JumpList& slowCases)
{
    slowCases.append(jit.branchIfNotCell(value));
    Jump isSymbol = jit.branchIfSymbol(value);
    slowCases.append(jit.branchIfNotString(value));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), result);
    slowCases.append(jit.branchIfRopeStringImpl(result));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(result, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
    Jump isReady = jit.jump();
    isSymbol.link(&jit);
    jit.loadPtr(Address(value, Symbol::offsetOfSymbolImpl()), result);
    isReady.link(&jit);
}

void emitMegamorphicLoad(CCallHelpers& jit, GPRReg base, GPRReg uid, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    slowCases.append(jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, scratch1, scratch2, scratch3, scratch4));
    returnValue(jit, scratch1);
}

void emitMegamorphicStore(CCallHelpers& jit, GPRReg base, GPRReg uid, GPRReg value, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    auto [notFound, reallocating] = jit.storeMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, value, scratch1, scratch2, scratch3);
    slowCases.append(notFound);

    loadVM(jit, scratch2);
    jit.load8(Address(base, JSCell::cellStateOffset()), scratch1);
    Jump noBarrier = jit.branch32(CCallHelpers::Above, scratch1, Address(scratch2, VM::offsetOfHeapBarrierThreshold()));
    jit.move(base, argument1);
    jit.move(scratch2, argument0);
    tailCall(jit, Entry::operationAOTWriteBarrierAfterPut, Restores::CalleeSaves);
    noBarrier.link(&jit);
    returnVoid(jit);

    reallocating.link(&jit);
    slowCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch3, MegamorphicCache::StoreEntry::offsetOfReallocating()), TrustedImm32(MegamorphicCache::StoreEntry::addsLazyClosure)));
    jit.move(value, scratch1);
    jit.move(base, argument1);
    jit.move(scratch1, argument2);
    jit.move(scratch3, argument3);
    loadVM(jit, argument0);
    tailCall(jit, Entry::operationAOTPutByIdReallocating, Restores::CalleeSaves);
}

} // anonymous namespace

static void branchIfSlotIsLive(CCallHelpers& jit, GPRReg slot, JumpList& slowCases)
{
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::attemptsMask), scratch0);
    Jump hasGivenUp = jit.branch32(CCallHelpers::Equal, scratch0, TrustedImm32(Slot::attemptsMask));
    Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(slot, OBJECT_OFFSETOF(Slot, structureID)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), scratch0);
    jit.subPtr(slot, scratch0, scratch0);
    jit.addPtr(TrustedImm32(BiasedDataPointer::bias), scratch0);
    slowCases.append(jit.branchPtr(CCallHelpers::AboveOrEqual, scratch0, CCallHelpers::TrustedImmPtr(SharedData::size)));
    hasGivenUp.link(&jit);
    isTaken.link(&jit);
}

void generateFrontEndGetById(CCallHelpers& jit)
{
    JumpList slowCases;
    enter(jit);
    branchIfSlotIsLive(jit, argument3, slowCases);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument2, scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
    emitMegamorphicLoad(jit, argument1, scratch0, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawGetById);
}

void generateFrontEndGetByVal(CCallHelpers& jit)
{
    JumpList slowCases;
    enter(jit);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadAtomName(jit, argument2, scratch0, slowCases);
    emitMegamorphicLoad(jit, argument1, scratch0, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawGetByVal);
}

void generateFrontEndPutById(CCallHelpers& jit)
{
    constexpr GPRReg flags = argument5;
    constexpr uint32_t isDirect = 1;
    JumpList slowCases;
    enter(jit);
    slowCases.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(isDirect)));
    branchIfSlotIsLive(jit, argument4, slowCases);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument3, scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
    emitMegamorphicStore(jit, argument1, scratch0, argument2, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawPutById);
}

void generateFrontEndPutByVal(CCallHelpers& jit)
{
    JumpList slowCases;
    enter(jit);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadAtomName(jit, argument2, scratch0, slowCases);
    emitMegamorphicStore(jit, argument1, scratch0, argument3, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawPutByVal);
}

static void generateCompareEq(CCallHelpers& jit, bool strict)
{
    constexpr GPRReg left = argument1;
    constexpr GPRReg right = argument2;
    JumpList slowCases;
    JumpList isFalse;
    JumpList isTrue;

    enter(jit);
    jit.or64(left, right, scratch0);
    Jump notBothCells = jit.branchIfNotCell(scratch0);

    jit.load8(Address(left, JSCell::typeInfoTypeOffset()), scratch0);
    jit.load8(Address(right, JSCell::typeInfoTypeOffset()), scratch1);
    Jump leftIsNotString = jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(StringType));
    Jump rightIsNotString = jit.branch32(CCallHelpers::NotEqual, scratch1, TrustedImm32(StringType));

    jit.loadPtr(Address(left, JSString::offsetOfValue()), scratch0);
    jit.loadPtr(Address(right, JSString::offsetOfValue()), scratch1);
    slowCases.append(jit.branchIfRopeStringImpl(scratch0));
    slowCases.append(jit.branchIfRopeStringImpl(scratch1));
    isTrue.append(jit.branchPtr(CCallHelpers::Equal, scratch0, scratch1));
    jit.load32(Address(scratch0, StringImpl::lengthMemoryOffset()), scratch2);
    jit.load32(Address(scratch1, StringImpl::lengthMemoryOffset()), scratch3);
    isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch2, scratch3));
    jit.load32(Address(scratch0, StringImpl::flagsOffset()), scratch2);
    jit.load32(Address(scratch1, StringImpl::flagsOffset()), scratch3);
    jit.and32(scratch3, scratch2);
    isFalse.append(jit.branchTest32(CCallHelpers::NonZero, scratch2, TrustedImm32(StringImpl::flagIsAtom())));
    slowCases.append(jit.jump());

    leftIsNotString.link(&jit);
    rightIsNotString.link(&jit);
    if (strict) {
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(HeapBigIntType)));
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch1, TrustedImm32(HeapBigIntType)));
        slowCases.append(jit.jump());
    } else {
        slowCases.append(jit.branch32(CCallHelpers::Below, scratch0, TrustedImm32(ObjectType)));
        slowCases.append(jit.branch32(CCallHelpers::Below, scratch1, TrustedImm32(ObjectType)));
        isFalse.append(jit.jump());
    }

    notBothCells.link(&jit);
    JumpList notBothNumbers;
    notBothNumbers.append(jit.branchIfNotNumber(left));
    notBothNumbers.append(jit.branchIfNotNumber(right));
    auto toDouble = [&](GPRReg value, FPRReg result) {
        Jump isInt32 = jit.branchIfInt32(value);
        jit.unboxDoubleWithoutAssertions(value, scratch0, result);
        Jump done = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(value, result);
        done.link(&jit);
    };
    toDouble(left, FPRInfo::fpRegT0);
    toDouble(right, FPRInfo::fpRegT1);
    isTrue.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, FPRInfo::fpRegT0, FPRInfo::fpRegT1));
    isFalse.append(jit.jump());

    notBothNumbers.link(&jit);
    if (strict)
        isFalse.append(jit.jump());
    else {
        auto compareWithOther = [&](GPRReg value) {
            isTrue.append(jit.branchIfOther(value, scratch0));
            isFalse.append(jit.branchIfNotCell(value));
            isFalse.append(jit.branchTest8(CCallHelpers::Zero, Address(value, JSCell::typeInfoFlagsOffset()), TrustedImm32(MasqueradesAsUndefined)));
            slowCases.append(jit.jump());
        };
        Jump leftIsNotOther = jit.branchIfNotOther(left, scratch0);
        compareWithOther(right);
        leftIsNotOther.link(&jit);
        slowCases.append(jit.branchIfNotOther(right, scratch0));
        compareWithOther(left);
    }

    isTrue.link(&jit);
    returnBoolean(jit, true);
    isFalse.link(&jit);
    returnBoolean(jit, false);

    slowCases.link(&jit);
    tailCall(jit, strict ? Entry::RawCompareStrictEq : Entry::RawCompareEq);
}

void generateFrontEndCompareStrictEq(CCallHelpers& jit) { generateCompareEq(jit, true); }
void generateFrontEndCompareEq(CCallHelpers& jit) { generateCompareEq(jit, false); }

static void emitAllocateFromCache(CCallHelpers& jit, GPRReg cache, GPRReg result, JumpList& slowCases)
{
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, structureID)), scratch0);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch0));
    jit.loadPtr(Address(cache, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), scratch1);
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(cache, sizeof(Slot)), scratch1);
    jit.or64(scratch1, scratch0);
    jit.store64(scratch0, Address(result, 0));
}

static void emitFillAndReturnObject(CCallHelpers&, GPRReg values, GPRReg count);

static void emitAllocateWithProperties(CCallHelpers& jit, GPRReg cache, GPRReg values, GPRReg count, JumpList& slowCases)
{
    emitAllocateFromCache(jit, cache, scratch3, slowCases);
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::offsetMask), scratch0);
    emitFillAndReturnObject(jit, values, count);
}

static void emitFillAndReturnObject(CCallHelpers& jit, GPRReg values, GPRReg count)
{
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    auto clear = jit.label();
    Jump cleared = jit.branch32(CCallHelpers::BelowOrEqual, scratch0, count);
    jit.sub32(TrustedImm32(1), scratch0);
    jit.store64(TrustedImm32(0), BaseIndex(scratch3, scratch0, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.jump().linkTo(clear, &jit);
    cleared.link(&jit);
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        Jump isNotEmpty = jit.branchTest32(CCallHelpers::NonZero, count);
        jit.breakpoint();
        isNotEmpty.link(&jit);
    }
    auto copy = jit.label();
    jit.sub32(TrustedImm32(1), count);
    jit.load64(BaseIndex(values, count, CCallHelpers::TimesEight), scratch0);
    jit.store64(scratch0, BaseIndex(scratch3, count, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.branchTest32(CCallHelpers::NonZero, count).linkTo(copy, &jit);
    mutatorFence(jit, scratch0);
    returnValue(jit, scratch3);
}

void generateFrontEndNewObjectLiteral(CCallHelpers& jit)
{
    JumpList slowCases;
    enter(jit);
    emitAllocateWithProperties(jit, argument3, argument1, argument2, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawNewObjectLiteral);
}

void installOperationFrontEnds(VM& vm, void** entries)
{
    auto install = [&](Entry entry, Entry raw, Stub stub) {
        entries[static_cast<unsigned>(raw)] = entries[static_cast<unsigned>(entry)];
        entries[static_cast<unsigned>(entry)] = tagCodePtr<OperationPtrTag>(stubAddress(stub));
    };
#define AOT_INSTALL_FRONT_END(name) install(Entry::operationAOT##name, Entry::Raw##name, Stub::FrontEnd##name);
    entries[static_cast<unsigned>(Entry::MegamorphicCache)] = &vm.ensureMegamorphicCache();
    entries[static_cast<unsigned>(Entry::UndefinedHolder)] = const_cast<EncodedJSValue*>(undefinedValueHolder());
    AOT_INSTALL_FRONT_END(GetById)
    AOT_INSTALL_FRONT_END(GetByVal)
    AOT_INSTALL_FRONT_END(PutById)
    AOT_INSTALL_FRONT_END(PutByVal)
    AOT_INSTALL_FRONT_END(NewObjectLiteral)
#define AOT_INSTALL_HELPER(name, operation) install(Entry::operation, Entry::name##SlowPath, Stub::name##WithFastPath);
    FOR_EACH_AOT_OPERATION_BEHIND_HELPER(AOT_INSTALL_HELPER)
#undef AOT_INSTALL_HELPER
    AOT_INSTALL_FRONT_END(CompareStrictEq)
    AOT_INSTALL_FRONT_END(CompareEq)
#undef AOT_INSTALL_FRONT_END
}

#else

void installOperationFrontEnds(VM&, void**) { }

#endif // CPU(ARM64) || CPU(X86_64)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
