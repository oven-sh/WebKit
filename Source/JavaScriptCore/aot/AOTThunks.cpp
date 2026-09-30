/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThunks.h"

#if ENABLE(FTL_JIT)

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

#if CPU(ARM64)

namespace {

using Jump = CCallHelpers::Jump;
using JumpList = CCallHelpers::JumpList;
using Address = CCallHelpers::Address;
using BaseIndex = CCallHelpers::BaseIndex;
using TrustedImm32 = CCallHelpers::TrustedImm32;
using TrustedImm64 = CCallHelpers::TrustedImm64;
using TrustedImmPtr = CCallHelpers::TrustedImmPtr;

// A thunk has no frame: the frame pointer is that of the stub that called it in place of the operation, and the return address is where
// it was put by the call. It may use the registers that a C function need not preserve and that hold no argument of its operation.
constexpr GPRReg argument0 = GPRInfo::argumentGPR0;
constexpr GPRReg argument1 = GPRInfo::argumentGPR1;
constexpr GPRReg argument2 = GPRInfo::argumentGPR2;
constexpr GPRReg argument3 = GPRInfo::argumentGPR3;
constexpr GPRReg argument4 = GPRInfo::argumentGPR4;
constexpr GPRReg argument5 = GPRInfo::argumentGPR5;
constexpr GPRReg scratch0 = GPRInfo::regT9;
constexpr GPRReg scratch1 = GPRInfo::regT10;
constexpr GPRReg scratch2 = GPRInfo::regT11;
constexpr GPRReg scratch3 = GPRInfo::regT12;
constexpr GPRReg scratch4 = GPRInfo::regT13;

constexpr GPRReg cacheGPR = GPRInfo::argumentGPR7; // No operation that has one of these in front of it takes that many arguments.

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

void tailCall(CCallHelpers& jit, Entry operation)
{
    loadEntry(jit, operation, GPRInfo::nonArgGPR0);
    jit.farJump(GPRInfo::nonArgGPR0, OperationPtrTag);
}

// After an object has been filled in, before anyone else gets to see it.
void mutatorFence(CCallHelpers& jit, GPRReg scratch)
{
    loadVM(jit, scratch);
    Jump notNeeded = jit.branchTest8(CCallHelpers::Zero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.storeFence();
    notNeeded.link(&jit);
}

// What an operation that did not throw returns.
void returnValue(CCallHelpers& jit, GPRReg value)
{
    jit.move(value, GPRInfo::returnValueGPR);
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();
}

void returnBoolean(CCallHelpers& jit, bool value)
{
    jit.move(TrustedImm32(value), GPRInfo::returnValueGPR);
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();
}

void returnVoid(CCallHelpers& jit)
{
    jit.move(TrustedImm32(0), GPRInfo::returnValueGPR);
    jit.ret();
}

void branchIfNotObjectValue(CCallHelpers& jit, GPRReg value, JumpList& slowCases)
{
    slowCases.append(jit.branchIfNotCell(value));
    slowCases.append(jit.branchIfNotObject(value));
}

// One of the calling function's identifiers.
void loadIdentifier(CCallHelpers& jit, GPRReg index, GPRReg result)
{
    // Which function that is, is told from where the stub is to go back to.
    jit.loadPtr(Address(GPRInfo::callFrameRegister, CallFrame::returnPCOffset()), scratch4);
    loadIndexOfFunctionAt(jit, scratch4);
    static_assert(sizeof(FunctionInfo) == 32);
    jit.lshiftPtr(indexOfFunctionGPR, TrustedImm32(5), scratch4);
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfInfos()), result);
    jit.addPtr(scratch4, result);
    jit.loadPtr(Address(result, FunctionInfo::offsetOfIdentifiers()), result);
    jit.zeroExtend32ToWord(index, scratch4);
    jit.loadPtr(BaseIndex(result, scratch4, CCallHelpers::TimesEight), result);
}

// The name that a value is, if it is a string that is an atom: those are the names the megamorphic cache knows.
void loadAtomName(CCallHelpers& jit, GPRReg value, GPRReg result, JumpList& slowCases)
{
    slowCases.append(jit.branchIfNotCell(value));
    slowCases.append(jit.branchIfNotString(value));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), result);
    slowCases.append(jit.branchIfRopeStringImpl(result));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(result, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
}

// ---- Properties: the VM's megamorphic cache, for the sites whose own cache is of no use because they see many structures.
// The operations fill it (AOTInlineCaches.cpp), and see to it that there is nothing in it for the names it is not meant for.

void emitMegamorphicLoad(CCallHelpers& jit, GPRReg base, GPRReg uid, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    slowCases.append(jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, scratch1, scratch2, scratch3, scratch4));
    returnValue(jit, scratch1);
}

// What the cache knows about a structure and a name it learned from puts that were not direct and that turned out to be stores to
// the base. For those a direct put comes to the same.
void emitMegamorphicStore(CCallHelpers& jit, GPRReg base, GPRReg uid, GPRReg value, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    auto [notFound, reallocating] = jit.storeMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, value, scratch1, scratch2, scratch3);
    slowCases.append(notFound);

    // Whatever the value is: the object may have a new structure, that nothing else may be keeping alive.
    loadVM(jit, scratch2);
    jit.load8(Address(base, JSCell::cellStateOffset()), scratch1);
    Jump noBarrier = jit.branch32(CCallHelpers::Above, scratch1, Address(scratch2, VM::offsetOfHeapBarrierThreshold()));
    jit.move(base, argument1);
    jit.move(scratch2, argument0);
    tailCall(jit, Entry::operationAOTWriteBarrierAfterPut);
    noBarrier.link(&jit);
    returnVoid(jit);

    // The object needs more storage first. The entry that says how much is in the last scratch register.
    reallocating.link(&jit);
    jit.move(value, scratch1);
    jit.move(base, argument1);
    jit.move(scratch1, argument2);
    jit.move(scratch3, argument3);
    loadVM(jit, argument0);
    tailCall(jit, Entry::operationAOTPutByIdReallocating);
}

} // anonymous namespace

// A site with a slot of its own that has nothing in it yet has to get to its operation, which fills the slot: what another site has
// left in the megamorphic cache would keep it from ever having one. Any other has nothing to lose by looking there first: its
// slot is for another structure, so it sees more than one; or it has given up on the slot; or the slot is nobody's (SharedData), and
// is never filled.
static void branchIfSlotIsStillOfUse(CCallHelpers& jit, GPRReg slot, JumpList& slowCases)
{
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::attemptsMask), scratch0);
    if (!Options::aotLooksInMegamorphicCacheUnlessSlotIsEmpty()) {
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(Slot::attemptsMask)));
        return;
    }
    Jump hasGivenUp = jit.branch32(CCallHelpers::Equal, scratch0, TrustedImm32(Slot::attemptsMask));
    Jump isTaken = jit.branchTest32(CCallHelpers::NonZero, Address(slot, OBJECT_OFFSETOF(Slot, structureID)));
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfSharedData()), scratch0);
    jit.subPtr(slot, scratch0, scratch0);
    slowCases.append(jit.branchPtr(CCallHelpers::AboveOrEqual, scratch0, CCallHelpers::TrustedImmPtr(SharedData::size)));
    hasGivenUp.link(&jit);
    isTaken.link(&jit);
}

// (globalObject, base, identifierIndex, slot)
void generateFrontEndGetById(CCallHelpers& jit)
{
    JumpList slowCases;
    branchIfSlotIsStillOfUse(jit, argument3, slowCases);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument2, scratch0);
    emitMegamorphicLoad(jit, argument1, scratch0, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawGetById);
}

// (globalObject, base, property)
void generateFrontEndGetByVal(CCallHelpers& jit)
{
    JumpList slowCases;
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadAtomName(jit, argument2, scratch0, slowCases);
    emitMegamorphicLoad(jit, argument1, scratch0, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawGetByVal);
}

// (globalObject, base, value, identifierIndex, slot, flags)
void generateFrontEndPutById(CCallHelpers& jit)
{
    JumpList slowCases;
    branchIfSlotIsStillOfUse(jit, argument4, slowCases);
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument3, scratch0);
    emitMegamorphicStore(jit, argument1, scratch0, argument2, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawPutById);
}

// (globalObject, base, property, value, isStrict)
void generateFrontEndPutByVal(CCallHelpers& jit)
{
    JumpList slowCases;
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadAtomName(jit, argument2, scratch0, slowCases);
    emitMegamorphicStore(jit, argument1, scratch0, argument3, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawPutByVal);
}

// ---- Equality, of two values that are not both int32s, and that do not have the same bits unless they are numbers.

// (globalObject, left, right)
static void generateCompareEq(CCallHelpers& jit, bool strict)
{
    constexpr GPRReg left = argument1;
    constexpr GPRReg right = argument2;
    JumpList slowCases;
    JumpList isFalse;
    JumpList isTrue;

    jit.or64(left, right, scratch0);
    Jump notBothCells = jit.branchIfNotCell(scratch0);

    // From here on it takes two values of the same kind that are equal by content, or, for ==, a conversion.
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
    // There is only one atom for any content.
    jit.load32(Address(scratch0, StringImpl::flagsOffset()), scratch2);
    jit.load32(Address(scratch1, StringImpl::flagsOffset()), scratch3);
    jit.and32(scratch3, scratch2);
    isFalse.append(jit.branchTest32(CCallHelpers::NonZero, scratch2, TrustedImm32(StringImpl::flagIsAtom())));
    slowCases.append(jit.jump());

    leftIsNotString.link(&jit);
    rightIsNotString.link(&jit);
    if (strict) {
        // What is left that is equal by content is a pair of BigInts.
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(HeapBigIntType)));
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch1, TrustedImm32(HeapBigIntType)));
        slowCases.append(jit.jump());
    } else {
        // Two objects are not converted.
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
        // undefined and null are equal to each other, to objects that pass themselves off as undefined, and to nothing else.
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

// ---- Allocation, once the operation has left what it takes in the site's slots (fillAllocationCache()).

// A cell with its header, and nothing else, filled in.
static void emitAllocateFromCache(CCallHelpers& jit, GPRReg cache, GPRReg result, JumpList& slowCases)
{
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, structureID)), scratch0);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch0));
    jit.loadPtr(Address(cache, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), scratch1);
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(cache, sizeof(Slot)), scratch1); // There is nothing in its low half.
    jit.or64(scratch1, scratch0);
    jit.store64(scratch0, Address(result, 0));
}

// (globalObject, inlineCapacity, cache)
void generateFrontEndNewObject(CCallHelpers& jit)
{
    JumpList slowCases;
    emitAllocateFromCache(jit, argument2, scratch3, slowCases);
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    jit.load32(Address(argument2, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::offsetMask), scratch0);
    jit.emitInitializeInlineStorage(scratch3, scratch0);
    mutatorFence(jit, scratch0);
    returnValue(jit, scratch3);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawNewObject);
}

static void emitFillAndReturnObject(CCallHelpers&, GPRReg values, GPRReg count);

// An object of the structure the cache is for, in which the properties are in the object itself, one after the other from the start.
// Clobbers count.
static void emitAllocateWithProperties(CCallHelpers& jit, GPRReg cache, GPRReg values, GPRReg count, JumpList& slowCases)
{
    emitAllocateFromCache(jit, cache, scratch3, slowCases);
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::offsetMask), scratch0);
    emitFillAndReturnObject(jit, values, count);
}

// scratch3: an object with its header filled in. scratch0: how many properties there is room for in it. Clobbers count.
static void emitFillAndReturnObject(CCallHelpers& jit, GPRReg values, GPRReg count)
{
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    // From the end: what there is room for and nothing to put in, and then the values.
    auto clear = jit.label();
    Jump cleared = jit.branch32(CCallHelpers::BelowOrEqual, scratch0, count);
    jit.sub32(TrustedImm32(1), scratch0);
    jit.store64(TrustedImm32(0), BaseIndex(scratch3, scratch0, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.jump().linkTo(clear, &jit);
    cleared.link(&jit);
    auto copy = jit.label();
    jit.sub32(TrustedImm32(1), count);
    jit.load64(BaseIndex(values, count, CCallHelpers::TimesEight), scratch0);
    jit.store64(scratch0, BaseIndex(scratch3, count, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.branchTest32(CCallHelpers::NonZero, count).linkTo(copy, &jit);
    mutatorFence(jit, scratch0);
    returnValue(jit, scratch3);
}

// (globalObject, values, count, cache)
void generateFrontEndNewObjectLiteral(CCallHelpers& jit)
{
    JumpList slowCases;
    emitAllocateWithProperties(jit, argument3, argument1, argument2, slowCases);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawNewObjectLiteral);
}

// (globalObject, callee, values, count, cache). See operationAOTCreateThisWithProperties().
void generateFrontEndCreateThisWithProperties(CCallHelpers& jit)
{
    JumpList slowCases;
    jit.loadPtr(Address(argument4, OBJECT_OFFSETOF(Slot, pointer)), scratch0);
    Jump isAnotherFunction = jit.branchPtr(CCallHelpers::NotEqual, scratch0, argument1);
    // It is a function, then. What it makes its instances from is still what it was?
    jit.loadPtr(Address(argument1, JSFunction::offsetOfExecutableOrRareData()), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0, TrustedImm32(JSFunction::rareDataTag)));
    jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfStructure() - JSFunction::rareDataTag), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
    jit.load32(Address(argument4, 2 * sizeof(Slot) + OBJECT_OFFSETOF(Slot, structureID)), scratch1);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch0, scratch1));
    emitAllocateWithProperties(jit, argument4, argument2, argument3, slowCases);

    // The site knows one function, and this is another. See MegamorphicCache::ConstructionEntry.
    isAnotherFunction.link(&jit);
    if (Options::aotCachesConstructionForManyFunctions()) {
        using ConstructionEntry = MegamorphicCache::ConstructionEntry;
        slowCases.append(jit.branchIfNotFunction(argument1));
        jit.loadPtr(Address(argument1, JSFunction::offsetOfExecutableOrRareData()), scratch0);
        slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0, TrustedImm32(JSFunction::rareDataTag)));
        jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfAllocator() - JSFunction::rareDataTag), scratch1);
        jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfStructure() - JSFunction::rareDataTag), scratch0);
        slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
        loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
        jit.urshift32(scratch0, TrustedImm32(MegamorphicCache::constructionHashShift), scratch2);
        jit.urshiftPtr(argument4, TrustedImm32(MegamorphicCache::constructionHashShift), scratch4);
        jit.xor32(scratch4, scratch2);
        jit.and32(TrustedImm32(MegamorphicCache::constructionCacheMask), scratch2);
        static_assert(sizeof(ConstructionEntry) == 24);
        jit.getEffectiveAddress(BaseIndex(scratch2, scratch2, CCallHelpers::TimesTwo), scratch2);
        jit.getEffectiveAddress(BaseIndex(cacheGPR, scratch2, CCallHelpers::TimesEight, MegamorphicCache::offsetOfConstructionEntries()), scratch4);
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch0, Address(scratch4, ConstructionEntry::offsetOfFirstStructureID())));
        slowCases.append(jit.branchPtr(CCallHelpers::NotEqual, argument4, Address(scratch4, ConstructionEntry::offsetOfSite())));
        jit.load16(Address(scratch4, ConstructionEntry::offsetOfEpoch()), scratch2);
        jit.load16(Address(cacheGPR, MegamorphicCache::offsetOfEpoch()), scratch0);
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch0, scratch2));
        // (As big as what it starts out as: there is as much room for properties in the one as in the other.)
        jit.emitAllocateWithNonNullAllocator(scratch3, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        jit.load32(Address(scratch4, ConstructionEntry::offsetOfLastStructureID()), scratch4);
        loadEntry(jit, Entry::StructureIDBase, scratch0);
        jit.addPtr(scratch0, scratch4);
        jit.emitStoreStructureWithTypeInfo(scratch4, scratch3, scratch1);
        jit.load8(Address(scratch4, Structure::inlineCapacityOffset()), scratch0);
        emitFillAndReturnObject(jit, argument2, argument3);
    }
    slowCases.link(&jit);
    tailCall(jit, Entry::RawCreateThisWithProperties);
}

// (globalObject, callee, inlineCapacity). The callee keeps what it takes to allocate its instances, once it has made one.
void generateFrontEndCreateThis(CCallHelpers& jit)
{
    JumpList slowCases;
    slowCases.append(jit.branchIfNotFunction(argument1));
    jit.loadPtr(Address(argument1, JSFunction::offsetOfExecutableOrRareData()), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0, TrustedImm32(JSFunction::rareDataTag)));
    jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfAllocator() - JSFunction::rareDataTag), scratch1);
    jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfStructure() - JSFunction::rareDataTag), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch1));
    jit.emitAllocateWithNonNullAllocator(scratch3, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.emitStoreStructureWithTypeInfo(scratch0, scratch3, scratch1);
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    jit.load8(Address(scratch0, Structure::inlineCapacityOffset()), scratch1);
    jit.emitInitializeInlineStorage(scratch3, scratch1);
    mutatorFence(jit, scratch0);
    returnValue(jit, scratch3);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawCreateThis);
}

// (globalObject, scope, index, isExpression, kind, cache)
void generateFrontEndNewFunction(CCallHelpers& jit)
{
    JumpList slowCases;
    emitAllocateFromCache(jit, argument5, scratch3, slowCases);
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    jit.storePtr(argument1, Address(scratch3, JSCallee::offsetOfScopeChain()));
    jit.loadPtr(Address(argument5, OBJECT_OFFSETOF(Slot, pointer)), scratch0);
    jit.storePtr(scratch0, Address(scratch3, JSFunction::offsetOfExecutableOrRareData()));
    mutatorFence(jit, scratch0);
    returnValue(jit, scratch3);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawNewFunction);
}

// (globalObject, base, identifierIndex). As AssemblyHelpers::hasMegamorphicProperty() does it, but for looking in one place only.
void generateFrontEndInById(CCallHelpers& jit)
{
    using HasEntry = MegamorphicCache::HasEntry;
    JumpList slowCases;
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument2, scratch0);
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    jit.load32(Address(argument1, JSCell::structureIDOffset()), scratch1);
    jit.extractUnsignedBitfield32(scratch1, TrustedImm32(MegamorphicCache::structureIDHashShift1), TrustedImm32(32 - MegamorphicCache::structureIDHashShift1), scratch2);
    jit.xorUnsignedRightShift32(scratch2, scratch1, TrustedImm32(MegamorphicCache::structureIDHashShift6), scratch3);
    jit.load32(Address(scratch0, UniquedStringImpl::flagsOffset()), scratch2);
    jit.addUnsignedRightShift32(scratch3, scratch2, TrustedImm32(StringImpl::s_flagCount), scratch3);
    jit.and32(TrustedImm32(MegamorphicCache::hasCachePrimaryMask), scratch3);
    static_assert(hasOneBitSet(sizeof(HasEntry)));
    jit.lshift32(TrustedImm32(getLSBSet(sizeof(HasEntry))), scratch3);
    jit.addPtr(cacheGPR, scratch3);
    jit.addPtr(TrustedImm32(MegamorphicCache::offsetOfHasCachePrimaryEntries()), scratch3);
    jit.load16(Address(cacheGPR, MegamorphicCache::offsetOfEpoch()), scratch2);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch1, Address(scratch3, HasEntry::offsetOfStructureID())));
    slowCases.append(jit.branchPtr(CCallHelpers::NotEqual, Address(scratch3, HasEntry::offsetOfUid()), scratch0));
    slowCases.append(jit.branch32WithMemory16(CCallHelpers::NotEqual, Address(scratch3, HasEntry::offsetOfEpoch()), scratch2));
    jit.load16(Address(scratch3, HasEntry::offsetOfResult()), scratch2);
    returnValue(jit, scratch2);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawInById);
}

void installOperationFrontEnds(VM& vm, void** entries)
{
    auto install = [&](Entry entry, Entry raw, Stub stub) {
        entries[static_cast<unsigned>(raw)] = entries[static_cast<unsigned>(entry)];
        entries[static_cast<unsigned>(entry)] = tagCodePtr<OperationPtrTag>(addressOfStub(stub));
    };
#define AOT_INSTALL_FRONT_END(name) install(Entry::operationAOT##name, Entry::Raw##name, Stub::FrontEnd##name);
    unsigned disabled = Options::aotDisableFastPaths();
    if (!(disabled & 1024)) {
        entries[static_cast<unsigned>(Entry::MegamorphicCache)] = &vm.ensureMegamorphicCache();
        AOT_INSTALL_FRONT_END(GetById)
        AOT_INSTALL_FRONT_END(GetByVal)
        AOT_INSTALL_FRONT_END(PutById)
        AOT_INSTALL_FRONT_END(PutByVal)
        AOT_INSTALL_FRONT_END(InById)
    }
    if (!(disabled & 2048)) {
        AOT_INSTALL_FRONT_END(NewObject)
        AOT_INSTALL_FRONT_END(NewObjectLiteral)
        AOT_INSTALL_FRONT_END(CreateThis)
        AOT_INSTALL_FRONT_END(CreateThisWithProperties)
        AOT_INSTALL_FRONT_END(NewFunction)
#define AOT_INSTALL_HELPER(name, operation) install(Entry::operation, Entry::Behind##name, Stub::AheadOf##name);
        FOR_EACH_AOT_OPERATION_BEHIND_HELPER(AOT_INSTALL_HELPER)
#undef AOT_INSTALL_HELPER
    }
    if (!(disabled & 4096)) {
        AOT_INSTALL_FRONT_END(CompareStrictEq)
        AOT_INSTALL_FRONT_END(CompareEq)
    }
#undef AOT_INSTALL_FRONT_END
}

#else

void installOperationFrontEnds(VM&, void**) { }

#endif // CPU(ARM64)

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
