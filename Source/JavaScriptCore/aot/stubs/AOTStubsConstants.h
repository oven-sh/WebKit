/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubHelpers.h"

namespace JSC { namespace AOT {

enum class CreatedConstant : uint8_t { String, Key };
// What a site has made that has read the word of the cell and found it empty. The stub fills the word, hands the cell back in the register of its key and keeps every other register.
// The number of the constant lies behind the call: the string constant's, the property key's, the built-in cell's, or the word of the literal. In the last form that word lies in the register of the result.
enum class ConstantAtSite : uint8_t { String, Key, BuiltinCell, LiteralString, LiteralStringInRegister };

// The immediate: the string constant's number, the StringID's index, the property key's number.
constexpr StubKey createConstant(CreatedConstant constant) { return StubKey(StubFamily::CreateConstant).withForm(static_cast<uint8_t>(constant)); }
// The immediate: the built-in cell's number.
constexpr StubKey builtinCell() { return StubKey(StubFamily::BuiltinCell); }
// The immediate: the word of the literal.
constexpr StubKey literalStringCell() { return StubKey(StubFamily::LiteralStringCell); }
constexpr StubKey createConstantAtSite(ConstantAtSite constant) { return StubKey(StubFamily::CreateConstantAtSite).withForm(static_cast<uint8_t>(constant)); }

constexpr bool readsNumberBehindCall(ConstantAtSite constant) { return constant != ConstantAtSite::LiteralStringInRegister; }
// The number lies there as a word of a throwing site does: ThrowSiteWord::opcodeInFrontOfWordBehindCall.
// On x86-64 the stub returns to that instruction, which writes the flags alone. On ARM64 it returns behind the word.
inline constexpr int32_t offsetOfNumberBehindCall = static_cast<int32_t>(ThrowSiteWord::sizeOfWordBehindCall - sizeof(uint32_t));

struct LazyHeaderRegisters {
    GPRReg number;
    GPRReg record;
    // Holds zero, which the table of offsets had for the number.
    GPRReg offset;
    // Holds the address of the headers.
    GPRReg lazyHeaders;
    GPRReg word;
};

// ImageText::createLazyHeader(). Leaves the offset of the header in r.offset. Keeps r.number and r.lazyHeaders.
void createLazyHeader(StubAssembler&, const LazyHeaderRegisters&, unsigned longestTextWithoutHeader, JumpList& cannot);

static_assert(exists(createConstant(CreatedConstant::Key)) && !exists(createConstant(CreatedConstant::Key).withForm(3)));
static_assert(exists(builtinCell()) && exists(builtinCell().withImmediate(1)) && !exists(createConstant(CreatedConstant::Key).withImmediate(1)));
static_assert(exists(createConstantAtSite(ConstantAtSite::LiteralStringInRegister)) && !exists(createConstantAtSite(ConstantAtSite::LiteralStringInRegister).withForm(5)));
static_assert([] {
    for (GPRReg reg : allocatableGPRs) {
        if (!exists(createConstantAtSite(ConstantAtSite::String).withOperandRegister(reg)))
            return false;
    }
    return true;
}());

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
