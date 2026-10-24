/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTVMState.h"
#include "JSCJSValueInlines.h"
#include "RegExpObject.h"
#include "StackManager.h"
#include "VM.h"
#include "YarrFlags.h"
#include "YarrJIT.h"
#include "YarrMatchingContextHolder.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/NotFound.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

static_assert(sizeof(std::unique_ptr<Yarr::YarrCodeBlock>) == sizeof(void*));
static_assert(sizeof(Yarr::MatchingContextHolder) == 4 * sizeof(void*));
static_assert(sizeof(String) == sizeof(void*));

inline constexpr unsigned maxSavedOperandsOfMatch = 3;
inline constexpr unsigned offsetOfSavedOperandsOfMatch = roundUpToMultipleOf<sizeof(void*)>(sizeof(Yarr::MatchingContextHolder));
inline constexpr unsigned sizeOfMatchFrame = roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfSavedOperandsOfMatch + maxSavedOperandsOfMatch * sizeof(void*));
// What StubAssembler::callLeaf wants to know inside the frame.
inline constexpr unsigned bytesPushedInMatchFrame = sizeOfMatchFrame + (isX86_64() ? 1 : 2) * sizeof(CPURegister);

inline constexpr int64_t noMatch = static_cast<int64_t>(notFound);

inline constexpr int32_t globalFlag = static_cast<int32_t>(Yarr::Flags::Global);
inline constexpr int32_t stickyFlag = static_cast<int32_t>(Yarr::Flags::Sticky);
inline constexpr int32_t hasIndicesFlag = static_cast<int32_t>(Yarr::Flags::HasIndices);
inline constexpr int32_t unicodeFlags = static_cast<int32_t>(Yarr::Flags::Unicode) | static_cast<int32_t>(Yarr::Flags::UnicodeSets);

inline Address savedOperandOfMatch(size_t index)
{
    return Address(CCallHelpers::stackPointerRegister, offsetOfSavedOperandsOfMatch + index * sizeof(void*));
}

inline void pushMatchFrame(StubAssembler& jit, std::span<const GPRReg> savedOperands)
{
    RELEASE_ASSERT(savedOperands.size() <= maxSavedOperandsOfMatch);
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeOfMatchFrame), CCallHelpers::stackPointerRegister);
    for (size_t index = 0; index < savedOperands.size(); ++index)
        jit.storePtr(savedOperands[index], savedOperandOfMatch(index));
}

inline void popMatchFrame(StubAssembler& jit, GPRReg answer, std::span<const GPRReg> savedOperands)
{
    jit.move(GPRInfo::returnValueGPR, answer);
    for (size_t index = 0; index < savedOperands.size(); ++index)
        jit.loadPtr(savedOperandOfMatch(index), savedOperands[index]);
    jit.emitFunctionEpilogue();
}

// The holder is at the stack pointer of the match frame.
inline void storeMatchingContext(StubAssembler& jit, GPRReg scratch)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit()), scratch);
    jit.storePtr(scratch, Address(sp, Yarr::MatchingContextHolder::offsetOfStackLimit()));
    jit.store64(TrustedImm32(0), Address(sp, Yarr::MatchingContextHolder::offsetOfFreeList()));
}

inline void loadMatchOutput(StubAssembler& jit, GPRReg output)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), output);
    jit.loadPtr(Address(output, VM::offsetOfAOTState() + VMState::offsetOfRegExpMatchOutput()), output);
}

// The characters are in A0. Without a start the match starts at 0. loadOutput(output) loads the address of the offsets, call() calls the code, whose answer goes to answer.
inline void callMatchingCodeInFrame(StubAssembler& jit, std::span<const GPRReg> savedOperands, GPRReg subjectLength, std::optional<GPRReg> start, GPRReg answer, const auto& loadOutput, const auto& call)
{
    const GPRReg startArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg lengthArgument = jit.reuseScratch(StubRegister::A2);
    const GPRReg output = jit.reuseScratch(StubRegister::A3);
    const GPRReg context = jit.reuseScratch(StubRegister::A4);
    pushMatchFrame(jit, savedOperands);
    jit.move(subjectLength, lengthArgument);
    // start may share its register with output.
    if (start)
        jit.move(*start, startArgument);
    else
        jit.move(TrustedImm32(0), startArgument);
    loadOutput(output);
    storeMatchingContext(jit, context);
    jit.move(CCallHelpers::stackPointerRegister, context);
    call();
    popMatchFrame(jit, answer, savedOperands);
}

inline Address matchOutputEntry(GPRReg output, unsigned index)
{
    return Address(output, index * sizeof(int));
}

[[nodiscard]] inline Jump branchIfReadOnlyLastIndex(StubAssembler& jit, GPRReg regExpObject)
{
    return jit.branchTest8(CCallHelpers::NonZero, Address(regExpObject, RegExpObject::offsetOfRegExpAndFlags()), TrustedImm32(RegExpObject::lastIndexIsNotWritableFlag));
}

inline void resetLastIndex(StubAssembler& jit, GPRReg regExpObject)
{
    jit.store64(TrustedImm64(JSValue::encode(jsNumber(0))), Address(regExpObject, RegExpObject::offsetOfLastIndex()));
}

inline void storeEndOfMatchInLastIndex(StubAssembler& jit, GPRReg regExpObject, GPRReg offsets, GPRReg scratch)
{
    jit.load32(matchOutputEntry(offsets, 1), scratch);
    boxInt32(jit, scratch, scratch);
    jit.store64(scratch, Address(regExpObject, RegExpObject::offsetOfLastIndex()));
}

inline void loadRegExp(StubAssembler& jit, GPRReg regExpObject, GPRReg regExp)
{
    jit.load64(Address(regExpObject, RegExpObject::offsetOfRegExpAndFlags()), regExp);
    jit.and64(TrustedImm32(static_cast<int32_t>(RegExpObject::regExpMask)), regExp);
}

struct MatchRecordRegisters {
    GPRReg regExp;
    GPRReg string;
    GPRReg offsets;
    GPRReg globalObject;
    GPRReg scratch;
};

// Leaves the match where RegExp.lastMatch and the other legacy static properties read it. Of the five registers only string is intact afterwards, and the result register is overwritten.
void recordMatch(StubAssembler&, const MatchRecordRegisters&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
