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

void generateCallCompiledMethodOfArray(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg name = jit.immediate();
    RELEASE_ASSERT(noOverlap(name, registers.word, registers.scratch, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), instanceGPR));

    // Not DerivedArrayType: that is Array.prototype.
    JumpList looksUp;
    looksUp.append(jit.branchIfNotCell(thisGPR));
    looksUp.append(jit.branchIfNotType(thisGPR, ArrayType));
    looksUp.append(branchIfNotPristine(jit, thisGPR));
    // A list of another realm finds the function of that realm, which makes its errors and its results there.
    loadStructure(jit, thisGPR, registers.scratch);
    looksUp.append(branchIfNotOfThisRealm(jit, registers.scratch));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs.wordsOfCompiledMethodsOfArray) + static_cast<ptrdiff_t>(jit.key().form() * sizeof(uint64_t))), registers.word);
    looksUp.append(jit.branchTest64(CCallHelpers::Zero, registers.word));
    // From here on the name is not needed: its register is that of the count. The function may be called, and whatever it declares, a register beyond the arguments holds undefined, as emitEnterClosure() leaves it.
    emitFillMissingArgumentRegisters(jit, count);
    jit.move(TrustedImm32(count), countGPR);
    toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
    jit.farJump(registers.word, JSEntryPtrTag);
    jit.linkToStub(looksUp, callMethodById(count));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
