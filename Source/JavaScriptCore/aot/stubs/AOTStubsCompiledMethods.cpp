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
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"
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
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs.wordsOfCompiledMethodsOfArray) + static_cast<ptrdiff_t>(jit.key().form() * sizeof(uint64_t))), registers.word);
    looksUp.append(lentArguments ? branchUnlessWordBorrows(jit, registers.word, registers.scratch, lentArguments) : jit.branchTest64(CCallHelpers::Zero, registers.word));
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
