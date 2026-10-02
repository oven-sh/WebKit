/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThunks.h"

#if ENABLE(AOT)

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

// A thunk has no frame of its own: the frame pointer is that of the stub that called it in place of the operation, and the return
// address is still in the link register. It may use the caller-saved registers that hold no argument of its operation.
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

constexpr GPRReg cacheGPR = GPRInfo::argumentGPR7; // No operation with a thunk in front of it takes that many arguments.

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

// Emitted after an object has been initialized, before it becomes visible to other threads.
void mutatorFence(CCallHelpers& jit, GPRReg scratch)
{
    loadVM(jit, scratch);
    Jump notNeeded = jit.branchTest8(CCallHelpers::Zero, Address(scratch, VM::offsetOfHeapMutatorShouldBeFenced()));
    jit.storeFence();
    notNeeded.link(&jit);
}

// Returns a value the way an operation that did not throw does.
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

// Loads one of the program's identifiers. Null: nothing has asked for it yet (ProgramOfVM::identifier()).
void loadIdentifier(CCallHelpers& jit, GPRReg index, GPRReg result)
{
    jit.loadPtr(Address(instanceGPR, Instance::offsetOfIdentifiersOfProgram()), result);
    jit.zeroExtend32ToWord(index, scratch4);
    jit.loadPtr(BaseIndex(result, scratch4, CCallHelpers::TimesEight), result);
}

// Loads the property name for a value, if it is an atom string. Those are the only names in the megamorphic cache.
void loadAtomName(CCallHelpers& jit, GPRReg value, GPRReg result, JumpList& slowCases)
{
    slowCases.append(jit.branchIfNotCell(value));
    slowCases.append(jit.branchIfNotString(value));
    jit.loadPtr(Address(value, JSString::offsetOfValue()), result);
    slowCases.append(jit.branchIfRopeStringImpl(result));
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, Address(result, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIsAtom())));
}

// ---- Properties: the VM's megamorphic cache, for sites whose own inline cache is ineffective because they see many structures.
// The operations fill it (AOTInlineCaches.cpp), and make sure that it has no entries for names that it must not be used for.

void emitMegamorphicLoad(CCallHelpers& jit, GPRReg base, GPRReg uid, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    slowCases.append(jit.loadMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, scratch1, scratch2, scratch3, scratch4));
    returnValue(jit, scratch1);
}

// The entries for a structure and a name come from ordinary puts that turned out to be plain stores to the base. For those, a
// direct put has the same effect.
void emitMegamorphicStore(CCallHelpers& jit, GPRReg base, GPRReg uid, GPRReg value, JumpList& slowCases)
{
    loadEntry(jit, Entry::MegamorphicCache, cacheGPR);
    auto [notFound, reallocating] = jit.storeMegamorphicProperty(CCallHelpers::MegamorphicCacheLocation(cacheGPR), base, uid, nullptr, value, scratch1, scratch2, scratch3);
    slowCases.append(notFound);

    // The barrier is needed whatever the value is: the object may have a new structure, which nothing else may be keeping alive.
    loadVM(jit, scratch2);
    jit.load8(Address(base, JSCell::cellStateOffset()), scratch1);
    Jump noBarrier = jit.branch32(CCallHelpers::Above, scratch1, Address(scratch2, VM::offsetOfHeapBarrierThreshold()));
    jit.move(base, argument1);
    jit.move(scratch2, argument0);
    tailCall(jit, Entry::operationAOTWriteBarrierAfterPut);
    noBarrier.link(&jit);
    returnVoid(jit);

    // The object needs more out-of-line storage first. The last scratch register holds the cache entry, which says how much.
    reallocating.link(&jit);
    jit.move(value, scratch1);
    jit.move(base, argument1);
    jit.move(scratch1, argument2);
    jit.move(scratch3, argument3);
    loadVM(jit, argument0);
    tailCall(jit, Entry::operationAOTPutByIdReallocating);
}

} // anonymous namespace

// A site whose own slot is still empty has to reach its operation, which fills the slot. If it used an entry that another site left
// in the megamorphic cache, its slot would never be filled. Any other site loses nothing by trying the megamorphic cache first: its
// slot is for another structure, so it sees more than one; or it has given up on the slot; or the slot is shared (SharedData) and
// is never filled.
static void branchIfSlotIsStillOfUse(CCallHelpers& jit, GPRReg slot, JumpList& slowCases)
{
    jit.load32(Address(slot, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::attemptsMask), scratch0);
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
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
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
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
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

// ---- Equality of two values that are not both int32s, and whose bits differ unless they are numbers.

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

    // From here on, the values are only equal if they have the same type and equal contents or, for ==, after a conversion.
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
    // Atoms are unique, so two different atoms are not equal.
    jit.load32(Address(scratch0, StringImpl::flagsOffset()), scratch2);
    jit.load32(Address(scratch1, StringImpl::flagsOffset()), scratch3);
    jit.and32(scratch3, scratch2);
    isFalse.append(jit.branchTest32(CCallHelpers::NonZero, scratch2, TrustedImm32(StringImpl::flagIsAtom())));
    slowCases.append(jit.jump());

    leftIsNotString.link(&jit);
    rightIsNotString.link(&jit);
    if (strict) {
        // The only remaining values that compare by content are two BigInts.
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch0, TrustedImm32(HeapBigIntType)));
        isFalse.append(jit.branch32(CCallHelpers::NotEqual, scratch1, TrustedImm32(HeapBigIntType)));
        slowCases.append(jit.jump());
    } else {
        // Two objects are compared without conversion.
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
        // undefined and null are equal to each other, to objects that masquerade as undefined, and to nothing else.
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

// ---- Allocation, once the operation has filled the site's slots (fillAllocationCache()).

// Allocates a cell and initializes only its header.
static void emitAllocateFromCache(CCallHelpers& jit, GPRReg cache, GPRReg result, JumpList& slowCases)
{
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, structureID)), scratch0);
    slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch0));
    jit.loadPtr(Address(cache, sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), scratch1);
    jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load64(Address(cache, sizeof(Slot)), scratch1); // Its low half is zero.
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

// Allocates an object with the cached structure, whose properties are all inline, in order from the first slot.
// Clobbers count.
static void emitAllocateWithProperties(CCallHelpers& jit, GPRReg cache, GPRReg values, GPRReg count, JumpList& slowCases)
{
    emitAllocateFromCache(jit, cache, scratch3, slowCases);
    jit.load32(Address(cache, OBJECT_OFFSETOF(Slot, offset)), scratch0);
    jit.and32(TrustedImm32(Slot::offsetMask), scratch0);
    emitFillAndReturnObject(jit, values, count);
}

// scratch3: an object with an initialized header. scratch0: its inline capacity. Clobbers count.
static void emitFillAndReturnObject(CCallHelpers& jit, GPRReg values, GPRReg count)
{
    jit.storePtr(TrustedImmPtr(nullptr), Address(scratch3, JSObject::butterflyOffset()));
    // From the end: first clear the unused capacity, then store the values.
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
    // The callee is a function, then. Check that its allocation profile still has the cached structure.
    jit.loadPtr(Address(argument1, JSFunction::offsetOfExecutableOrRareData()), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0, TrustedImm32(JSFunction::rareDataTag)));
    jit.loadPtr(Address(scratch0, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfStructure() - JSFunction::rareDataTag), scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
    jit.load32(Address(argument4, 2 * sizeof(Slot) + OBJECT_OFFSETOF(Slot, structureID)), scratch1);
    slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch0, scratch1));
    emitAllocateWithProperties(jit, argument4, argument2, argument3, slowCases);

    // The site has cached one function, and this is another. See MegamorphicCache::ConstructionEntry.
    isAnotherFunction.link(&jit);
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
    // (The allocation size comes from the initial structure. The final structure has the same inline capacity.)
    jit.emitAllocateWithNonNullAllocator(scratch3, JITAllocator::variable(), scratch1, scratch2, slowCases, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
    jit.load32(Address(scratch4, ConstructionEntry::offsetOfLastStructureID()), scratch4);
    loadEntry(jit, Entry::StructureIDBase, scratch0);
    jit.addPtr(scratch0, scratch4);
    jit.emitStoreStructureWithTypeInfo(scratch4, scratch3, scratch1);
    jit.load8(Address(scratch4, Structure::inlineCapacityOffset()), scratch0);
    emitFillAndReturnObject(jit, argument2, argument3);
    slowCases.link(&jit);
    tailCall(jit, Entry::RawCreateThisWithProperties);
}

// (globalObject, callee, inlineCapacity). Once the callee has created an instance, its rare data has what is needed to allocate
// more.
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

// (globalObject, base, identifierIndex). Like AssemblyHelpers::hasMegamorphicProperty(), except that it only probes the primary
// table.
void generateFrontEndInById(CCallHelpers& jit)
{
    using HasEntry = MegamorphicCache::HasEntry;
    JumpList slowCases;
    branchIfNotObjectValue(jit, argument1, slowCases);
    loadIdentifier(jit, argument2, scratch0);
    slowCases.append(jit.branchTestPtr(CCallHelpers::Zero, scratch0));
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
    entries[static_cast<unsigned>(Entry::MegamorphicCache)] = &vm.ensureMegamorphicCache();
    AOT_INSTALL_FRONT_END(GetById)
    AOT_INSTALL_FRONT_END(GetByVal)
    AOT_INSTALL_FRONT_END(PutById)
    AOT_INSTALL_FRONT_END(PutByVal)
    AOT_INSTALL_FRONT_END(InById)
    AOT_INSTALL_FRONT_END(NewObject)
    AOT_INSTALL_FRONT_END(NewObjectLiteral)
    AOT_INSTALL_FRONT_END(CreateThis)
    AOT_INSTALL_FRONT_END(CreateThisWithProperties)
    AOT_INSTALL_FRONT_END(NewFunction)
#define AOT_INSTALL_HELPER(name, operation) install(Entry::operation, Entry::Behind##name, Stub::AheadOf##name);
    FOR_EACH_AOT_OPERATION_BEHIND_HELPER(AOT_INSTALL_HELPER)
#undef AOT_INSTALL_HELPER
    AOT_INSTALL_FRONT_END(CompareStrictEq)
    AOT_INSTALL_FRONT_END(CompareEq)
#undef AOT_INSTALL_FRONT_END
}

#else

void installOperationFrontEnds(VM&, void**) { }

#endif // CPU(ARM64)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
