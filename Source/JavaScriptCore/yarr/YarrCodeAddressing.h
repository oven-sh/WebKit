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
    JS_EXPORT_PRIVATE static CodeGenerationPolicy fromProcessOptions();

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

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
MacroAssemblerCodeRef<JITThunkPtrTag> tryReadUnicodeCharSlowThunkGenerator(VM&);
#endif
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
MacroAssemblerCodeRef<JITThunkPtrTag> areCanonicallyEquivalentThunkGenerator(VM&);
#endif

class JITCodeAddressing final : public CodeAddressing {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(JITCodeAddressing);
public:
    JITCodeAddressing(CCallHelpers& jit, VM& vm, YarrBoyerMooreData& data)
        : CodeAddressing(jit)
        , m_vm(vm)
        , m_data(data)
    {
    }

    void moveAddress(SharedTableID id, GPRReg dest) final
    {
        m_jit.move(MacroAssembler::TrustedImmPtr(sharedTable(id).data()), dest);
    }

    void moveAddress(const ClassTable&, GPRReg dest) final
    {
        fail(JITFailureReason::UnlistedSharedTable);
        m_jit.move(MacroAssembler::TrustedImmPtr(nullptr), dest);
    }

    void moveAddressOfData(std::span<const uint8_t> bytes, GPRReg dest) final
    {
        m_jit.move(MacroAssembler::TrustedImmPtr(bytes.data()), dest);
    }

    MacroAssembler::Jump branchTestBuiltInClassTable(MacroAssembler::ResultCondition condition, GPRReg character, CharacterClass::Table table) final
    {
        return m_jit.branchTest8(condition, MacroAssembler::ExtendedAddress(character, reinterpret_cast<intptr_t>(table)));
    }

    MacroAssembler::Jump branchTestLatin1Table(MacroAssembler::ResultCondition condition, GPRReg character, const CharacterClass::ByteTable& table) final
    {
        const uint8_t* storedTable = m_data.addLatin1Table(table);
        return m_jit.branchTest8(condition, MacroAssembler::ExtendedAddress(character, reinterpret_cast<intptr_t>(storedTable)));
    }

    void canonicalizeLatin1(GPRReg character) final
    {
        m_jit.load16(MacroAssembler::ExtendedAddress(character, reinterpret_cast<intptr_t>(&latin1CanonicalizationTable)), character);
    }

    bool tryLoadConstant(uint64_t, GPRReg) final { return false; }

    void callRoutine(const SharedRoutine& routine) final
    {
        switch (routine.kind()) {
        case SharedRoutine::Kind::SlowCodePointRead:
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
            callThunk(tryReadUnicodeCharSlowThunkGenerator);
            return;
#else
            break;
#endif
        case SharedRoutine::Kind::CanonicalEquivalence:
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
            callThunk(areCanonicallyEquivalentThunkGenerator);
            return;
#else
            break;
#endif
        case SharedRoutine::Kind::MatchSet:
        case SharedRoutine::Kind::ReadCodePoint:
        case SharedRoutine::Kind::MatchCodePointAgainstClassTable:
        case SharedRoutine::Kind::ClearCaptures:
        case SharedRoutine::Kind::FindFirstOfClass:
            break;
        }

        if (!canGenerateSharedRoutine(routine)) {
            fail(JITFailureReason::UnlistedSharedTable);
            return;
        }
        ensureLocalRoutine(routine).calls.append(m_jit.nearCall());
    }

    ReturnAddressSite storeReturnAddress(MacroAssembler::Address slot) final
    {
        return { m_jit.storePtrWithPatch(MacroAssembler::TrustedImmPtr(nullptr), slot), { } };
    }

    void linkReturnAddresses(Vector<ReturnAddressRecord>&& records) final
    {
        if (records.isEmpty())
            return;
        m_jit.addLinkTask([records = WTF::move(records)](LinkBuffer& linkBuffer) {
            for (auto& record : records)
                linkBuffer.patch(record.site.patchedPointer, linkBuffer.locationOf<YarrBacktrackPtrTag>(record.target));
        });
    }

    void emitLocalRoutines() final
    {
        auto localRoutines = std::exchange(m_localRoutines, { });
        for (auto& localRoutine : localRoutines) {
            MacroAssembler::Label start = m_jit.label();
            generateSharedRoutine(m_jit, localRoutine.routine, *this);
            m_jit.addLinkTask([start, calls = WTF::move(localRoutine.calls)](LinkBuffer& linkBuffer) {
                for (auto& call : calls)
                    linkBuffer.link(call, linkBuffer.locationOf<NoPtrTag>(start));
            });
        }
        RELEASE_ASSERT(m_localRoutines.isEmpty());
        *m_didEmitLocalRoutines = true;
    }

private:
    struct LocalRoutine {
        SharedRoutine routine;
        Vector<MacroAssembler::Call, 2> calls;
    };

    void callThunk(ThunkGenerator generator)
    {
        m_jit.nearCallThunk(CodeLocationLabel { m_vm.getCTIStub(generator).retaggedCode<NoPtrTag>() });
    }

    LocalRoutine& ensureLocalRoutine(const SharedRoutine& routine)
    {
        for (auto& localRoutine : m_localRoutines) {
            if (localRoutine.routine == routine)
                return localRoutine;
        }
        RELEASE_ASSERT(!*m_didEmitLocalRoutines);
        if (m_localRoutines.isEmpty()) {
            m_jit.addLinkTask([didEmitLocalRoutines = m_didEmitLocalRoutines](LinkBuffer&) {
                RELEASE_ASSERT(*didEmitLocalRoutines);
            });
        }
        m_localRoutines.append(LocalRoutine { routine, { } });
        return m_localRoutines.last();
    }

    VM& m_vm;
    YarrBoyerMooreData& m_data;
    Vector<LocalRoutine, 2> m_localRoutines;
    std::shared_ptr<bool> m_didEmitLocalRoutines { std::make_shared<bool>(false) };
};

} } // namespace JSC::Yarr

#endif // ENABLE(YARR_JIT)
