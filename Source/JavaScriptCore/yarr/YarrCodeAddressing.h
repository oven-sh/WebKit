/*
 * Copyright (C) 2009-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
#include "JITThunks.h"
#include "LinkBuffer.h"
#include "VM.h"
#include "YarrCanonicalize.h"
#include "YarrClassTable.h"
#include "YarrJIT.h"
#include "YarrSharedRoutines.h"
#include "YarrSharedTables.h"
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace JSC { namespace Yarr {

enum class CodeSizePolicy : uint8_t {
    Fast,
    Compact,
    MostCompact,
};

struct CodeGenerationPolicy {
    CodeSizePolicy sizePolicy { CodeSizePolicy::Fast };
    unsigned maximumCodeSize { 16 * MB };
    unsigned maxInlineLiteralLengthInDispatch { 32 };
    bool usesAlternationDispatch { true };
    bool compilesLookbehinds { true };
};

// Each CodeAddressing sets the member it links and leaves the other unset.
struct ReturnAddressSite {
    MacroAssembler::DataLabelPtr patchedPointer;
    MacroAssembler::Label pcRelativeAddress;
};

struct ReturnAddressRecord {
    ReturnAddressSite site;
    MacroAssembler::Label target;
};

class CodeAddressing {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CodeAddressing);
    WTF_MAKE_NONCOPYABLE(CodeAddressing);
public:
    virtual ~CodeAddressing() = default;

    virtual void moveAddress(SharedTableID, GPRReg dest) = 0;
    virtual void moveAddress(const ClassTable&, GPRReg dest) = 0;
    // The code may embed the address, so the bytes must outlive the code.
    virtual void moveAddressOfData(std::span<const uint8_t>, GPRReg dest) = 0;
    virtual MacroAssembler::Jump branchTestBuiltInClassTable(MacroAssembler::ResultCondition, GPRReg character, CharacterClass::Table) = 0;
    virtual MacroAssembler::Jump branchTestLatin1Table(MacroAssembler::ResultCondition, GPRReg character, const CharacterClass::ByteTable&) = 0;
    virtual void canonicalizeLatin1(GPRReg character) = 0;
    virtual bool tryLoadConstant(uint64_t, GPRReg dest) = 0;
    virtual void callRoutine(const SharedRoutine&) = 0;
    virtual ReturnAddressSite storeReturnAddress(MacroAssembler::Address slot) = 0;
    virtual void linkReturnAddresses(Vector<ReturnAddressRecord>&&) = 0;
    virtual void emitLocalRoutines() = 0;

    std::optional<JITFailureReason> failureReason() const { return m_failureReason; }

protected:
    explicit CodeAddressing(CCallHelpers& jit)
        : m_jit(jit)
    {
    }

    void fail(JITFailureReason reason)
    {
        if (!m_failureReason)
            m_failureReason = reason;
    }

    CCallHelpers& m_jit;

private:
    std::optional<JITFailureReason> m_failureReason;
};

} } // namespace JSC::Yarr

#endif // ENABLE(YARR_JIT)
