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
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(receiver, getterSetter, getter, scratch) && jit.result() == GPRInfo::returnValueGPR);

    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    cannot.append(branchIfNotFunctionType(jit, getter, scratch));
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true }, [&] {
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.callStub(callValue(0));
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

struct RecordedAccessorCall {
    GPRReg receiver;
    GPRReg record;
    GPRReg uid;
    std::optional<GPRReg> storedValue;
};

// A0 is set last: on ARM64 it is the receiver's register.
static void callRecordedAccessorAndReturn(StubAssembler& jit, const RecordedAccessorCall& call)
{
    const GPRReg record = call.record;
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::HostFunction, .returnsValue = !call.storedValue, .checksException = true }, [&] {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg function = jit.reuseScratch(StubRegister::T11);
        RELEASE_ASSERT(noOverlap(record, A0, A1, A2, function) && noOverlap(call.receiver, record, A1, A2, function));

        if (call.storedValue) {
            const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
            RELEASE_ASSERT(noOverlap(A3, record, call.receiver));
            jit.shuffleRegisters<GPRReg, 2>({ *call.storedValue, call.uid }, { A2, A3 });
        } else
            jit.move(call.uid, A2);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, slotBase)), A1);
        jit.moveConditionallyTest64(CCallHelpers::Zero, A1, A1, call.receiver, A1);
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
    callRecordedAccessorAndReturn(jit, { receiver, record, uid, std::nullopt });
}

void callCustomSetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg value, GPRReg accessorIndex, GPRReg uid)
{
    const GPRReg record = jit.clobberRegister(accessorIndex);
    RELEASE_ASSERT(noOverlap(receiver, value, record, uid));
    loadCustomAccessorRecord(jit, record);
    callRecordedAccessorAndReturn(jit, { receiver, record, uid, value });
}

static uint32_t encodeAccessorKind(MethodKind kind)
{
    FunctionKindData data { };
    data.accessorKind = static_cast<unsigned>(kind);
    return std::bit_cast<uint16_t>(data);
}

static uint32_t encodeCallableKind(CallableKind kind)
{
    FunctionKindData data { };
    data.callableKind = static_cast<unsigned>(kind);
    return std::bit_cast<uint16_t>(data);
}

struct NamePrefixTest {
    bool hasPrefix(uint32_t functionKind) const { return (functionKind & accessorKindMask) != methodAccessorKind || (functionKind & callableKindMask) == boundCallableKind; }

    uint32_t accessorKindMask { 0 };
    uint32_t callableKindMask { 0 };
    uint32_t methodAccessorKind { 0 };
    uint32_t boundCallableKind { 0 };
};

static NamePrefixTest createNamePrefixTest()
{
    constexpr CallableKind callableKinds[] = { CallableKind::Plain, CallableKind::Arrow, CallableKind::Method, CallableKind::Bound };

    NamePrefixTest test;
    test.methodAccessorKind = encodeAccessorKind(MethodKind::Method);
    test.boundCallableKind = encodeCallableKind(CallableKind::Bound);
    for (MethodKind accessorKind : allMembersOf<MethodKind>)
        test.accessorKindMask |= encodeAccessorKind(accessorKind);
    for (CallableKind callableKind : callableKinds)
        test.callableKindMask |= encodeCallableKind(callableKind);
    RELEASE_ASSERT(!(test.accessorKindMask & test.callableKindMask));

    for (MethodKind accessorKind : allMembersOf<MethodKind>) {
        for (CallableKind callableKind : callableKinds) {
            FunctionKindData data { };
            data.accessorKind = static_cast<unsigned>(accessorKind);
            data.callableKind = static_cast<unsigned>(callableKind);
            RELEASE_ASSERT(test.hasPrefix(std::bit_cast<uint16_t>(data)) == data.hasInitialNamePrefix());
        }
    }
    return test;
}

// functionNames has no "get ", "set " or "bound ".
static void branchIfNameHasPrefix(StubAssembler& jit, GPRReg tables, GPRReg function, GPRReg kind, JumpList& hasPrefix)
{
    const NamePrefixTest test = createNamePrefixTest();
    RELEASE_ASSERT(noOverlap(tables, function, kind));
    loadProgramTable(jit, tables, ProgramTableIndex::functionKinds, kind);
    jit.load16(BaseIndex(kind, function, CCallHelpers::TimesTwo), kind);
    jit.xor32(TrustedImm32(test.methodAccessorKind | test.boundCallableKind), kind);
    hasPrefix.append(jit.branchTest32(CCallHelpers::NonZero, kind, TrustedImm32(test.accessorKindMask)));
    hasPrefix.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(test.callableKindMask)));
}

static void loadSlimClosureFunction(StubAssembler& jit, GPRReg value, GPRReg function, JumpList& isNotSlimClosure)
{
    isNotSlimClosure.append(jit.branchIfNotCell(value));
    isNotSlimClosure.append(jit.branch8(CCallHelpers::NotEqual, Address(value, JSCell::typeInfoTypeOffset()), TrustedImm32(SlimClosureType)));
    jit.load64(Address(value, offsetOfSlimClosureWord), function);
    extractBits(jit, function, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, function);
}

// C++ reifies the closure at the second read of length or name.
static void markLazyPropertyRead(StubAssembler& jit, GPRReg closure, GPRReg flags, JumpList& wasReadBefore)
{
    jit.load8(Address(closure, JSCell::typeInfoFlagsOffset()), flags);
    wasReadBefore.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(TypeInfoPerCellBit)));
    jit.or32(TrustedImm32(TypeInfoPerCellBit), flags);
    jit.store8(flags, Address(closure, JSCell::typeInfoFlagsOffset()));
}

void readClosureLengthAndReturn(StubAssembler& jit, GPRReg closure, JumpList& miss)
{
    const GPRReg index = jit.reuseScratch(StubRegister::T10);
    const GPRReg tables = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(closure, index, tables, scratch));

    loadSlimClosureFunction(jit, closure, index, miss);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    jit.sub32(Address(tables, OBJECT_OFFSETOF(ProgramTables, closureFunctions) + OBJECT_OFFSETOF(ClosureFunctionRangeData, first)), index);
    miss.append(jit.branch32(CCallHelpers::AboveOrEqual, index, programTableCount(tables, ProgramTableIndex::functionLengths)));
    markLazyPropertyRead(jit, closure, scratch, miss);
    loadProgramTable(jit, tables, ProgramTableIndex::functionLengths, scratch);
    jit.load16(BaseIndex(scratch, index, CCallHelpers::TimesTwo), scratch);
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();
}

void readClosureNameAndReturn(StubAssembler& jit, GPRReg closure, JumpList& miss)
{
    constexpr StubKey createName = createConstant(CreatedConstant::NewString);
    const GPRReg function = jit.reuseScratch(StubRegister::T10);
    const GPRReg tables = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(closure, function, tables, scratch));

    loadSlimClosureFunction(jit, closure, function, miss);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    branchIfNameHasPrefix(jit, tables, function, scratch, miss);
    loadProgramTable(jit, tables, ProgramTableIndex::functionNames, scratch);
    const GPRReg name = function;
    jit.load32(BaseIndex(scratch, function, CCallHelpers::TimesFour), name);
    // An obfuscated image has 0 for every function, and string 0 is not its name.
    miss.append(jit.branchTest32(CCallHelpers::Zero, name));
    markLazyPropertyRead(jit, closure, scratch, miss);
    jit.move(name, jit.clobberRegister(*signatureOfCreateConstant(createName).immediate));
    jit.jumpToStub(createName);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
