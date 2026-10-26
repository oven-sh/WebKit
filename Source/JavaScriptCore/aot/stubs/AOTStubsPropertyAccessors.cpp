/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyAccessors.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTFunctionData.h"
#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeRecords.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsCalls.h"
#include "AOTStubsConstants.h"
#include "GetterSetter.h"
#include "JSCell.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "VM.h"
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static constexpr unsigned customAccessorRecordShift = 5;
static_assert(sizeof(CustomAccessorRecord) == 1u << customAccessorRecordShift);
static_assert(exists(callValue(0)) && exists(callValue(1)));

void callGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getterSetter, JumpList& cannot)
{
    const GPRReg getter = jit.reuseScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(receiver, getterSetter, getter, gprOf(StubRegister::T14)));
    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    callGetterFunctionAndReturn(jit, receiver, getter, cannot);
}

void callGetterFunctionAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getter, JumpList& cannot)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(getter == jit.reuseScratch(StubRegister::T11) && noOverlap(receiver, getter, scratch) && jit.result() == GPRInfo::returnValueGPR);

#if CPU(X86_64)
    // A closure of this Instance that declares no parameter is entered from here: its Structure says what the test of the type and CallValue would find out.
    static_assert(ClosureWord::offsetUnit == 1 && ClosureWord::numberOfOffsetBits == 32);
    auto branchIfTopByteRefusesCall = [&](ptrdiff_t offsetOfWord) {
        return jit.branch8(CCallHelpers::LessThan, Address(getter, offsetOfWord + ClosureWord::topByteShift / 8), TrustedImm32(ClosureWord::minTopByteOfCalleeTaking(0)));
    };
    JumpList isNotCommonCase;
    JumpList isNotSlim;
    if (jit.options().hasOneInstance)
        isNotSlim.append(branchIfTopByteRefusesCall(offsetOfSlimClosureWord));
    else {
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
        isNotSlim.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
        isNotCommonCase.append(branchIfTopByteRefusesCall(offsetOfSlimClosureWord));
    }
    jit.load32(Address(getter, offsetOfSlimClosureWord), scratch);
    Jump hasOffsetOfCode = jit.jump();

    isNotSlim.link(&jit);
    if (jit.options().hasOneInstance)
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
    isNotCommonCase.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))));
    isNotCommonCase.append(branchIfTopByteRefusesCall(offsetOfClosureEntry));
    jit.load32(Address(getter, offsetOfClosureEntry), scratch);

    hasOffsetOfCode.link(&jit);
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true }, [&] {
        RELEASE_ASSERT(noOverlap(scratch, thisGPR, calleeGPR, countGPR));
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.move(TrustedImm32(0), countGPR);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), scratch);
        jit.call(scratch, JSEntryPtrTag);
    });
    isNotCommonCase.link(&jit);
#else
    // The same. The whole word is loaded, and compared as the top byte is there.
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    constexpr TrustedImm64 minWord(ClosureWord::minWordOfCalleeTaking(0));
    JumpList isNotCommonCase;
    JumpList isNotSlim;
    if (!jit.options().hasOneInstance) {
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
        isNotSlim.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
    }
    jit.load64(Address(getter, offsetOfSlimClosureWord), scratch);
    (jit.options().hasOneInstance ? isNotSlim : isNotCommonCase).append(jit.branch64(CCallHelpers::LessThan, scratch, minWord));
    Jump hasWord = jit.jump();

    isNotSlim.link(&jit);
    jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
    isNotCommonCase.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))));
    jit.load64(Address(getter, offsetOfClosureEntry), scratch);
    isNotCommonCase.append(jit.branch64(CCallHelpers::LessThan, scratch, minWord));

    hasWord.link(&jit);
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true }, [&] {
        RELEASE_ASSERT(noOverlap(scratch, thisGPR, calleeGPR, countGPR));
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.move(TrustedImm32(0), countGPR);
        toCodeAddress(jit, scratch, jit.scratchRegister(), StackIsTested::Yes);
        jit.call(scratch, JSEntryPtrTag);
    });
    isNotCommonCase.link(&jit);
#endif
    cannot.append(branchIfNotFunctionType(jit, getter, scratch));
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true }, [&] {
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.callStub(callValueCell(0));
    });
}

void callSetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg value, GPRReg getterSetter, JumpList& cannot)
{
    const GPRReg setter = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(receiver, value, getterSetter, setter, scratch));

    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfSetter()), setter);
    cannot.append(branchIfNotFunctionType(jit, setter, scratch));
    // The function that stands for an absent setter ignores the call. Strict code has to throw.
    cannot.append(jit.branchIfType(setter, NullSetterFunctionType));
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = false, .checksException = true }, [&] {
        jit.shuffleRegisters<GPRReg, 3>({ receiver, value, setter }, { thisGPR, argumentGPR(0), calleeGPR });
        jit.callStub(callValue(1));
    });
}

static void loadCustomAccessorRecord(StubAssembler& jit, GPRReg accessorIndexAndRecord)
{
    jit.lshift64(TrustedImm32(customAccessorRecordShift), accessorIndexAndRecord);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.customAccessors)), accessorIndexAndRecord);
}

// A0 is set last: on ARM64 it is the receiver's register.
static void callRecordedAccessorAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg record, GPRReg uid, std::optional<GPRReg> storedValue)
{
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::HostFunction, .returnsValue = !storedValue, .checksException = true }, [&] {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg function = jit.reuseScratch(StubRegister::T11);
        RELEASE_ASSERT(noOverlap(record, A0, A1, A2, function) && noOverlap(receiver, record, A1, A2, function));

        if (storedValue) {
            const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
            RELEASE_ASSERT(noOverlap(A3, record, receiver));
            jit.shuffleRegisters<GPRReg, 2>({ *storedValue, uid }, { A2, A3 });
        } else
            jit.move(uid, A2);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, slotBase)), A1);
        jit.moveConditionallyTest64(CCallHelpers::Zero, A1, A1, receiver, A1);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, function)), function);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, globalObject)), A0);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), record);
        jit.storePtr(GPRInfo::callFrameRegister, Address(record, VM::offsetOfTopCallFrame()));
        jit.call(function, CustomAccessorPtrTag);
    });
}

void callCustomGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg accessorIndex, GPRReg uid)
{
    const GPRReg cachedValue = jit.reuseScratch(StubRegister::T11);
    const GPRReg record = jit.clobberRegister(accessorIndex);
    RELEASE_ASSERT(noOverlap(receiver, record, uid, cachedValue));
    loadCustomAccessorRecord(jit, record);

    jit.load32(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, offsetOfCachedValue)), cachedValue);
    Jump hasNoCachedValue = jit.branchTest32(CCallHelpers::Zero, cachedValue);
    jit.load64(BaseIndex(receiver, cachedValue, CCallHelpers::TimesOne), cachedValue);
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, cachedValue);
    jit.move(cachedValue, jit.result());
    jit.returnToCaller();

    hasNoCachedValue.link(&jit);
    isEmpty.link(&jit);
    callRecordedAccessorAndReturn(jit, receiver, record, uid, std::nullopt);
}

void callCustomSetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg value, GPRReg accessorIndex, GPRReg uid)
{
    const GPRReg record = jit.clobberRegister(accessorIndex);
    RELEASE_ASSERT(noOverlap(receiver, value, record, uid));
    loadCustomAccessorRecord(jit, record);
    callRecordedAccessorAndReturn(jit, receiver, record, uid, value);
}

// functionNames has no "get ", "set " or "bound ". The run time notes a look that stack frames tell of.
static void branchUnlessNameIsReadFromTable(StubAssembler& jit, GPRReg function, GPRReg kind, JumpList& hasPrefix)
{
    constexpr CallableKind callableKinds[] = { CallableKind::Plain, CallableKind::Arrow, CallableKind::Method, CallableKind::Bound };
    auto encode = [](unsigned accessorKind, unsigned callableKind) -> uint32_t {
        FunctionKindData data { };
        data.accessorKind = accessorKind;
        data.callableKind = callableKind;
        return std::bit_cast<uint16_t>(data);
    };
    const uint32_t methodAccessorKind = encode(static_cast<unsigned>(MethodKind::Method), 0);
    const uint32_t boundCallableKind = encode(0, static_cast<unsigned>(CallableKind::Bound));
    uint32_t accessorKindMask = 0;
    uint32_t callableKindMask = 0;
    for (MethodKind accessorKind : allMembersOf<MethodKind>)
        accessorKindMask |= encode(static_cast<unsigned>(accessorKind), 0);
    for (CallableKind callableKind : callableKinds)
        callableKindMask |= encode(0, static_cast<unsigned>(callableKind));
    RELEASE_ASSERT(!(accessorKindMask & callableKindMask));
    for (unsigned accessorKind = 0; accessorKind <= methodThatNotesLooksForName; ++accessorKind) {
        for (CallableKind callableKind : callableKinds) {
            const uint32_t functionKind = encode(accessorKind, static_cast<unsigned>(callableKind));
            const bool testLeaves = (functionKind & accessorKindMask) != methodAccessorKind || (functionKind & callableKindMask) == boundCallableKind;
            const FunctionKindData data = std::bit_cast<FunctionKindData>(static_cast<uint16_t>(functionKind));
            RELEASE_ASSERT(testLeaves == (data.hasInitialNamePrefix() || data.notesLooksForName()));
        }
    }

    loadKindOfClosureFunction(jit, function, kind);
    jit.xor32(TrustedImm32(methodAccessorKind | boundCallableKind), kind);
    hasPrefix.append(jit.branchTest32(CCallHelpers::NonZero, kind, TrustedImm32(accessorKindMask)));
    hasPrefix.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(callableKindMask)));
}

static void loadClosureFunction(StubAssembler& jit, GPRReg value, GPRReg function, JumpList& isNotClosure)
{
    isNotClosure.append(jit.branchIfNotCell(value));
    Jump isSlim = jit.branch8(CCallHelpers::Equal, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(SlimClosureType));
    isNotClosure.append(jit.branch8(CCallHelpers::NotEqual, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledClosureType)));
    jit.load64(Address(value, offsetOfClosureEntry), function);
    Jump hasWord = jit.jump();
    isSlim.link(&jit);
    jit.load64(Address(value, offsetOfSlimClosureWord), function);
    hasWord.link(&jit);
    extractBits(jit, function, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, function);
}

void readClosureLengthAndReturn(StubAssembler& jit, GPRReg closure, JumpList& miss)
{
    const GPRReg index = jit.reuseScratch(StubRegister::T10);
    const GPRReg tables = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(closure, index, tables, scratch));

    loadClosureFunction(jit, closure, index, miss);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    jit.sub32(Address(tables, OBJECT_OFFSETOF(ProgramTables, closureFunctions) + OBJECT_OFFSETOF(ClosureFunctionRangeData, first)), index);
    miss.append(jit.branch32(CCallHelpers::AboveOrEqual, index, programTableCount(tables, ProgramTableIndex::functionLengths)));
    loadProgramTable(jit, tables, ProgramTableIndex::functionLengths, scratch);
    jit.load8(BaseIndex(scratch, index, CCallHelpers::TimesOne), scratch);
    miss.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(countsAreListed)));
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();
}

// Changes table. index and result may be one register.
static void loadNumber24(StubAssembler& jit, GPRReg table, GPRReg index, GPRReg result)
{
    static_assert(sizeof(Number24) == 3);
    jit.getEffectiveAddress(BaseIndex(index, index, CCallHelpers::TimesTwo), result);
    jit.addPtr(result, table);
    jit.load16(Address(table), result);
    jit.load8(Address(table, 2), table);
    jit.lshift32(TrustedImm32(16), table);
    jit.or32(table, result);
}

void readClosureNameAndReturn(StubAssembler& jit, GPRReg closure, JumpList& miss)
{
    constexpr StubKey createName = createConstant(CreatedConstant::FunctionName);
    const GPRReg function = jit.reuseScratch(StubRegister::T10);
    const GPRReg tables = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(closure, function, tables, scratch));

    if (jit.options().useWideReferences) {
        miss.append(jit.jump());
        return;
    }
    loadClosureFunction(jit, closure, function, miss);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    branchUnlessNameIsReadFromTable(jit, function, scratch, miss);
    // ProgramTables::nameOfFunction(): functionSources begins at the first form of another function, and the groups of names end there.
    auto loadNumberOfFunctionsInFrontOfForms = [&] {
        jit.load32(programTableCount(tables, ProgramTableIndex::functionRecordNumbers), scratch);
        jit.sub32(programTableCount(tables, ProgramTableIndex::functionSources), scratch);
    };
    loadNumberOfFunctionsInFrontOfForms();
    Jump isListed = jit.branch32(CCallHelpers::Below, function, scratch);
    jit.sub32(scratch, function);
    loadProgramTable(jit, tables, ProgramTableIndex::functionSources, scratch);
    loadNumber24(jit, scratch, function, function);
    loadNumberOfFunctionsInFrontOfForms();
    miss.append(jit.branch32(CCallHelpers::AboveOrEqual, function, scratch));
    isListed.link(&jit);
    loadProgramTable(jit, tables, ProgramTableIndex::functionNameGroups, scratch);
    const GPRReg haveName = tables;
    jit.urshift32(function, TrustedImm32(5), haveName);
    jit.getEffectiveAddress(BaseIndex(scratch, haveName, CCallHelpers::TimesEight), scratch);
    jit.load32(Address(scratch), haveName);
    const GPRReg position = scratch;
    jit.load32(Address(scratch, sizeof(uint32_t)), position);
    // The bit of the function goes to the top of the word, the bits of the functions in front of it stand below it.
    jit.and32(TrustedImm32(31), function);
    jit.xor32(TrustedImm32(63), function);
    jit.lshift64(function, haveName);
    Jump hasNoEntry = jit.branch64(CCallHelpers::GreaterThanOrEqual, haveName, TrustedImm32(0));
    jit.lshift64(TrustedImm32(1), haveName);
    jit.countPopulation64(haveName, haveName, CCallHelpers::fpTempRegister);
    // ProgramStrings::writtenNameOfFunction(), which makes the string for whoever asks first.
    const GPRReg name = function;
    jit.add32(haveName, position, name);
    const GPRReg place = tables;
    static_assert(hasOneBitSet(ConstantCellWords::numberOfCachedFunctionNames));
    jit.move(name, place);
    jit.and32(TrustedImm32(ConstantCellWords::numberOfCachedFunctionNames - 1), place);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.functionNameCache)), scratch);
    jit.load64(BaseIndex(scratch, place, CCallHelpers::TimesEight), scratch);
    jit.add32(TrustedImm32(1), name);
    Jump isNotCached = jit.branch32(CCallHelpers::NotEqual, scratch, name);
    jit.urshift64(TrustedImm32(32), scratch);
    decodeCell32(jit, scratch, jit.result());
    jit.returnToCaller();

    isNotCached.link(&jit);
    jit.sub32(TrustedImm32(1), name);
    jit.move(name, jit.clobberRegister(*signatureOfCreateConstant(createName).immediate));
    jit.jumpToStub(createName);

    // Its name is the string with the number 0, which need not be the empty one.
    hasNoEntry.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.nameOfFunctionsWithoutEntry)), scratch);
    miss.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    jit.move(scratch, jit.result());
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
