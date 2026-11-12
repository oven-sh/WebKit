/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCompiledMethods.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "JSType.h"

namespace JSC { namespace AOT {

std::optional<CompiledMethodOfArray> compiledMethodOfArray(BuiltinID builtin)
{
    struct Row {
        NamedBuiltin builtin;
        CompiledMethodOfArray method;
    };
    static constexpr Row methods[] {
        { Builtins::ArrayPrototypeAt, CompiledMethodOfArray::At },
        { Builtins::ArrayPrototypeEvery, CompiledMethodOfArray::Every },
        { Builtins::ArrayPrototypeFilter, CompiledMethodOfArray::Filter },
        { Builtins::ArrayPrototypeFind, CompiledMethodOfArray::Find },
        { Builtins::ArrayPrototypeFindIndex, CompiledMethodOfArray::FindIndex },
        { Builtins::ArrayPrototypeFindLast, CompiledMethodOfArray::FindLast },
        { Builtins::ArrayPrototypeFindLastIndex, CompiledMethodOfArray::FindLastIndex },
        { Builtins::ArrayPrototypeFlatMap, CompiledMethodOfArray::FlatMap },
        { Builtins::ArrayPrototypeForEach, CompiledMethodOfArray::ForEach },
        { Builtins::ArrayPrototypeMap, CompiledMethodOfArray::Map },
        { Builtins::ArrayPrototypeReduce, CompiledMethodOfArray::Reduce },
        { Builtins::ArrayPrototypeReduceRight, CompiledMethodOfArray::ReduceRight },
        { Builtins::ArrayPrototypeSome, CompiledMethodOfArray::Some },
    };
    static_assert(std::size(methods) == numberOfCompiledMethodsOfArray);
    for (const Row& row : methods) {
        if (static_cast<BuiltinID>(row.builtin) == builtin)
            return row.method;
    }
    return std::nullopt;
}

StubSignature signatureOfCallCompiledMethodOfArray(StubKey key) { return signatureOf(callMethodById(key.count())); }
StubSignature signatureOfCallCompiledMethodOfArrayLending(StubKey key) { return signatureOf(callMethodById(key.count())); }

static void emitReturnElementAt(StubAssembler& jit, const CallRegisters& registers, JumpList& callsMethod)
{
    const GPRReg position = argumentGPR(0);
    const GPRReg index = argumentGPR(1);
    const GPRReg scratch = argumentGPR(2);
    const GPRReg butterfly = registers.word;
    const GPRReg kind = registers.scratch;
    const GPRReg element = registers.scratch;
    RELEASE_ASSERT(noOverlap(position, index, scratch, butterfly, kind, thisGPR, instanceGPR));
    JumpList callsOperation;
    Jump isNoInt32 = jit.branchIfNotInt32(position);
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(IndexingShapeMask)));
    jit.and32(TrustedImm32(IndexingShapeMask), kind);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.zeroExtend32ToWord(position, index);
    Jump countsFromStart = jit.branch32(CCallHelpers::GreaterThanOrEqual, index, TrustedImm32(0));
    jit.add32(Address(butterfly, Butterfly::offsetOfPublicLength()), index);
    countsFromStart.link(&jit);
    Jump isOutside = jit.branch32(CCallHelpers::AboveOrEqual, index, Address(butterfly, Butterfly::offsetOfPublicLength()));
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    Jump holdsNoIntegers = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape));
    holdsValues.link(&jit);
    jit.load64(BaseIndex(butterfly, index, CCallHelpers::TimesEight), element);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, element));
    jit.move(element, jit.result());
    jit.returnToCaller();

    isOutside.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsUndefined())), jit.result());
    jit.returnToCaller();

    holdsNoIntegers.link(&jit);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));
    jit.load64(BaseIndex(butterfly, index, CCallHelpers::TimesEight), element);
    boxDoubleBits(jit, PureDoubleBits::branchIfNotFinite(jit, element, scratch, callsOperation));
    jit.move(element, jit.result());
    jit.returnToCaller();

    isNoInt32.link(&jit);
    callsMethod.append(jit.branchIfNotNumber(position));
    callsOperation.link(&jit);
    static_assert(noOverlap(thisGPR, argumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2)));
    jit.move(position, operationArgumentGPR(2));
    jit.move(thisGPR, operationArgumentGPR(1));
    jit.jumpToCallOfOperation(Entry::operationAOTArrayAtGeneric);
}

enum class EmptyArrayResult : uint8_t { CallsMethod, Constant, InitialValue, NewArray };

struct EmptyArrayMethodResult {
    EmptyArrayResult kind { EmptyArrayResult::CallsMethod };
    EncodedJSValue constant { 0 };
};

static EmptyArrayMethodResult emptyArrayMethodResult(CompiledMethodOfArray method, unsigned count)
{
    if (!count)
        return { };
    switch (method) {
    case CompiledMethodOfArray::At:
        return { };
    case CompiledMethodOfArray::Filter:
    case CompiledMethodOfArray::FlatMap:
    case CompiledMethodOfArray::Map:
        return { EmptyArrayResult::NewArray };
    case CompiledMethodOfArray::Every:
        return { EmptyArrayResult::Constant, JSValue::encode(jsBoolean(true)) };
    case CompiledMethodOfArray::Some:
        return { EmptyArrayResult::Constant, JSValue::encode(jsBoolean(false)) };
    case CompiledMethodOfArray::FindIndex:
    case CompiledMethodOfArray::FindLastIndex:
        return { EmptyArrayResult::Constant, JSValue::encode(jsNumber(-1)) };
    case CompiledMethodOfArray::Find:
    case CompiledMethodOfArray::FindLast:
    case CompiledMethodOfArray::ForEach:
        return { EmptyArrayResult::Constant, JSValue::encode(jsUndefined()) };
    case CompiledMethodOfArray::Reduce:
    case CompiledMethodOfArray::ReduceRight:
        return { count > 1 ? EmptyArrayResult::InitialValue : EmptyArrayResult::CallsMethod };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitReturnForEmptyArray(StubAssembler& jit, const CallRegisters& registers, EmptyArrayMethodResult result)
{
    const GPRReg callback = argumentGPR(0);
    const GPRReg butterfly = registers.scratch;
    JumpList callsMethod;
    callsMethod.append(jit.branchTest8(CCallHelpers::Zero, Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    callsMethod.append(jit.branchTest32(CCallHelpers::NonZero, Address(butterfly, Butterfly::offsetOfPublicLength())));
    callsMethod.append(jit.branchIfNotCell(callback));
    callsMethod.append(branchIfNotFunctionType(jit, callback, registers.scratch));
    switch (result.kind) {
    case EmptyArrayResult::CallsMethod:
        RELEASE_ASSERT_NOT_REACHED();
    case EmptyArrayResult::Constant:
        jit.move(TrustedImm64(result.constant), jit.result());
        jit.returnToCaller();
        break;
    case EmptyArrayResult::InitialValue:
        jit.move(argumentGPR(1), jit.result());
        jit.returnToCaller();
        break;
    case EmptyArrayResult::NewArray:
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), registers.scratch);
        callsMethod.append(branchIfWatchpointSetIsNotWatched(jit, Address(registers.scratch, OBJECT_OFFSETOF(JSGlobalObject, m_arraySpeciesWatchpointSet)), registers.scratch));
        jit.jumpToStub(newEmptyArray(EmptyArray::Int32Capacity1));
        break;
    }
    callsMethod.link(&jit);
}

static void emitCallCompiledMethodOfArray(StubAssembler& jit, unsigned lentArguments)
{
    const unsigned count = jit.key().count();
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg name = jit.immediate();
    RELEASE_ASSERT(noOverlap(name, registers.word, registers.scratch, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), instanceGPR));

    JumpList looksUp;
    looksUp.append(jit.branchIfNotCell(thisGPR));
    looksUp.append(jit.branchIfNotType(thisGPR, ArrayType));
    looksUp.append(branchIfNotPristine(jit, thisGPR));
    loadStructure(jit, thisGPR, registers.scratch);
    looksUp.append(branchIfNotOfThisRealm(jit, registers.scratch));
    if (static_cast<CompiledMethodOfArray>(jit.key().form()) == CompiledMethodOfArray::At && count == 1 && !lentArguments) {
        JumpList callsMethod;
        emitReturnElementAt(jit, registers, callsMethod);
        callsMethod.link(&jit);
    }
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs.wordsOfCompiledMethodsOfArray) + static_cast<ptrdiff_t>(jit.key().form() * sizeof(uint64_t))), registers.word);
    looksUp.append(lentArguments ? branchUnlessWordBorrows(jit, registers.word, registers.scratch, lentArguments) : jit.branchTest64(CCallHelpers::Zero, registers.word));
    if (EmptyArrayMethodResult result = emptyArrayMethodResult(static_cast<CompiledMethodOfArray>(jit.key().form()), count); result.kind != EmptyArrayResult::CallsMethod)
        emitReturnForEmptyArray(jit, registers, result);
    emitFillMissingArgumentRegisters(jit, count);
    jit.move(TrustedImm32(count), countGPR);
    toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
    jit.farJump(registers.word, JSEntryPtrTag);
    jit.linkToStub(looksUp, callMethodById(count, lentArguments));
}

void generateCallCompiledMethodOfArray(StubAssembler& jit)
{
    emitCallCompiledMethodOfArray(jit, 0);
}

void generateCallCompiledMethodOfArrayLending(StubAssembler& jit)
{
    emitCallCompiledMethodOfArray(jit, onlyFirstArgumentIsLent);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
