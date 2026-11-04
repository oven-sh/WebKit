/*
 * Copyright (C) 2009-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2019-2026 the V8 project authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(YARR_JIT)

#include "CCallHelpers.h"
#include "Options.h"
#include "ProbeContext.h"
#include "YarrCanonicalize.h"
#include "YarrJITRegisters.h"
#include "YarrPattern.h"
#include <wtf/Hasher.h>
#include <wtf/Variant.h>

namespace JSC { namespace Yarr {

class CodeAddressing;

inline constexpr int32_t errorCodePoint = -1;

inline constexpr MacroAssembler::TrustedImm32 surrogateTagMask = MacroAssembler::TrustedImm32(0xfc00fc00);
inline constexpr MacroAssembler::TrustedImm32 surrogatePairTags = MacroAssembler::TrustedImm32(0xdc00d800);
inline constexpr MacroAssembler::TrustedImm32 surrogateTagShiftedRight11 = MacroAssembler::TrustedImm32(0xd800 >> 11);

template<typename AddressType>
inline void emitVerifySubjectRead(CCallHelpers& jit, AddressType address, unsigned bytes, GPRReg inputReg, GPRReg lengthReg, unsigned unitShift, unsigned site)
{
    if (!Options::verifyRegExpJITReads()) [[likely]]
        return;
    jit.probeDebug([=](Probe::Context& context) {
        uintptr_t effective;
        if constexpr (std::is_same_v<AddressType, MacroAssembler::BaseIndex>)
            effective = context.gpr(address.base) + (static_cast<uintptr_t>(context.gpr(address.index)) << static_cast<unsigned>(address.scale)) + static_cast<intptr_t>(address.offset);
        else
            effective = context.gpr(address.base) + static_cast<intptr_t>(address.offset);
        uintptr_t start = context.gpr(inputReg);
        uintptr_t end = start + (static_cast<uintptr_t>(static_cast<uint32_t>(context.gpr(lengthReg))) << unitShift);
        if (effective < start || effective + bytes > end) {
            dataLogLn("RegExp JIT subject read out of bounds at site ", site, ": [", RawPointer(reinterpret_cast<void*>(effective)), ", +", bytes, ") vs subject [", RawPointer(reinterpret_cast<void*>(start)), ", ", RawPointer(reinterpret_cast<void*>(end)), ")");
            CRASH();
        }
    });
}

#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
// Since the generator areCanonicallyEquivalentThunkGenerator() needs to be static,
// we set the incoming argument registers to the thunk here and ASSERT at runtime
// that they match.
#if CPU(ARM64)
static constexpr GPRReg areCanonicallyEquivalentCharArgReg = ARM64Registers::x6;
static constexpr GPRReg areCanonicallyEquivalentPattCharArgReg = ARM64Registers::x7;
static constexpr GPRReg areCanonicallyEquivalentCanonicalModeArgReg = ARM64Registers::x10;
#elif CPU(X86_64)
static constexpr GPRReg areCanonicallyEquivalentCharArgReg = X86Registers::eax;
static constexpr GPRReg areCanonicallyEquivalentPattCharArgReg = X86Registers::r9;
static constexpr GPRReg areCanonicallyEquivalentCanonicalModeArgReg = X86Registers::r13;

// The thunk code assumes that we return the result to areCanonicallyEquivalentCharArgReg.
static_assert(areCanonicallyEquivalentCharArgReg == GPRInfo::returnValueGPR);
#endif
#endif

enum class SurrogatePairAdvance : uint8_t { ByCaller, ClaimsSecondUnit, ClaimsSecondUnitBeforeCallerAdvances };
enum class InputCheck : uint8_t { DoneByCaller, FailsIfIndexIsAboveLength, FailsIfIndexIsLength };
enum class ClassWords : uint8_t { Low, High, Both };

#if CPU(X86_64)
inline constexpr GPRReg classTableRegister = MacroAssembler::s_scratchRegister;
#elif CPU(ARM64)
inline constexpr GPRReg classTableRegister = MacroAssembler::dataTempRegister;
#endif

struct SharedRoutine {
    enum class Kind : uint8_t {
        MatchSet,
        ReadCodePoint,
        SlowCodePointRead,
        MatchCodePointAgainstClassTable,
        ClearCaptures,
        FindFirstOfClass,
        CanonicalEquivalence,
    };

    struct MatchSet {
        GPRReg character;
        GPRReg scratch;
        Vector<char32_t> matches;
        Vector<CharacterRange> ranges;

        friend bool operator==(const MatchSet&, const MatchSet&) = default;
    };

    struct ReadCodePoint {
        friend bool operator==(const ReadCodePoint&, const ReadCodePoint&) = default;
    };

    struct SlowCodePointRead {
        friend bool operator==(const SlowCodePointRead&, const SlowCodePointRead&) = default;
    };

    // Returns membership in the carry flag on x86-64 and in bit 0 of unicodeAndSubpatternIdTemp on ARM64.
    struct MatchCodePointAgainstClassTable {
        bool isInverted;
        SurrogatePairAdvance pairAdvance;
        InputCheck inputCheck;

        friend bool operator==(const MatchCodePointAgainstClassTable&, const MatchCodePointAgainstClassTable&) = default;
    };

    struct ClearCaptures {
        friend bool operator==(const ClearCaptures&, const ClearCaptures&) = default;
    };

    struct FindFirstOfClass {
        ClassWords words;
        bool matchesAllNonASCII;
        CharSize charSize;

        friend bool operator==(const FindFirstOfClass&, const FindFirstOfClass&) = default;
    };

    struct CanonicalEquivalence {
        CanonicalMode mode;

        friend bool operator==(const CanonicalEquivalence&, const CanonicalEquivalence&) = default;
    };

    using Payload = Variant<MatchSet, ReadCodePoint, SlowCodePointRead, MatchCodePointAgainstClassTable, ClearCaptures, FindFirstOfClass, CanonicalEquivalence>;

    Kind kind() const;

    friend bool operator==(const SharedRoutine&, const SharedRoutine&) = default;

    Payload payload;
};

void add(Hasher&, const SharedRoutine&);
bool canGenerateSharedRoutine(const SharedRoutine&);
void generateSharedRoutine(CCallHelpers&, const SharedRoutine&, CodeAddressing&);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
void emitReadSurrogatePair(CCallHelpers&, MacroAssembler::RegisterID resultReg, MacroAssembler::JumpList& slowCases);
void tryReadUnicodeCharSlowImpl(CCallHelpers&);
#endif

} } // namespace JSC::Yarr

#endif // ENABLE(YARR_JIT)
