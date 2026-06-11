/*
 * Copyright (C) 2008-2023 Apple Inc. All rights reserved.
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

#include "ArityCheckMode.h"
#include "CallFrame.h"
#include "CodeOrigin.h"
#include "Intrinsic.h"
#include "JSCJSValue.h"
#include "MacroAssemblerCodeRef.h"
#include "RegisterAtOffsetList.h"
#include "RegisterSet.h"
#include <wtf/Atomics.h>


namespace JSC {

class PCToCodeOriginMap;

namespace DFG {
class CommonData;
class JITCode;
}
namespace FTL {
class ForOSREntryJITCode;
class JITCode;
}
namespace DOMJIT {
class Signature;
}

class TrackedReferences;
class VM;

struct PropertyInlineCacheIndex {
    explicit PropertyInlineCacheIndex(unsigned index)
        : m_index(index)
    { }

    unsigned m_index { 0 };
};

enum class JITType : uint8_t {
    None = 0b000,
    HostCallThunk = 0b001,
    InterpreterThunk = 0b010,
    BaselineJIT = 0b011,
    DFGJIT = 0b100,
    FTLJIT = 0b101,
};
static constexpr unsigned widthOfJITType = 3;
static_assert(WTF::getMSBSet(static_cast<std::underlying_type_t<JITType>>(JITType::FTLJIT)) + 1 == widthOfJITType);

#if CPU(ADDRESS64)
template<typename ByteSizedEnumType>
class JITConstant {
    static_assert(sizeof(ByteSizedEnumType) == 1);
    static constexpr uint64_t typeShift = 48;
    static constexpr uint64_t typeMask = 0xffull << typeShift;
    uint64_t m_encodedPointer;

    inline uint64_t encode(void* pointer, ByteSizedEnumType type)
    {
        uint64_t pointerBits = std::bit_cast<uint64_t>(pointer);
        return pointerBits | static_cast<uint64_t>(type) << 48;
    }
public:
    inline JITConstant()
        : m_encodedPointer(0)
    { }

    inline JITConstant(void* pointer, ByteSizedEnumType type)
        : m_encodedPointer(encode(pointer, type))
    { }

    template<typename OtherPointerType>
    JITConstant(CompactPointerTuple<OtherPointerType, ByteSizedEnumType> other)
        : m_encodedPointer(encode(other.pointer(), other.type()))
    { }

    inline uint32_t hash() const { return computeHash(m_encodedPointer); }
    inline void* pointer() const { return std::bit_cast<void*>(m_encodedPointer & ~typeMask); }
    void setPointer(void* pointer) { m_encodedPointer = encode(pointer, type()); }
    inline ByteSizedEnumType type() const { return static_cast<ByteSizedEnumType>((m_encodedPointer & typeMask) >> typeShift); }
    void setType(ByteSizedEnumType type) { m_encodedPointer = encode(pointer(), type); }

    friend bool operator==(const JITConstant&, const JITConstant&) = default;
};
#else
template<typename ByteSizedEnumType>
class JITConstant {
    void* m_pointer;
    ByteSizedEnumType m_type;
public:
    inline JITConstant()
        : m_pointer(nullptr)
    { }

    inline JITConstant(void* pointer, ByteSizedEnumType type)
        : m_pointer(pointer)
        , m_type(type)
    { }

    template<typename OtherPointerType>
    JITConstant(CompactPointerTuple<OtherPointerType, ByteSizedEnumType> other)
        : m_pointer(other.pointer())
        , m_type(other.type())
    { }

    inline uint32_t hash() const { return computeHash(m_pointer) * 31 + computeHash((unsigned)m_type); }
    inline void* pointer() const { return m_pointer; }
    void setPointer(void* pointer) { m_pointer = pointer; }
    inline ByteSizedEnumType type() const { return m_type; }
    void setType(ByteSizedEnumType type) { m_type = type; }

    friend bool operator==(const JITConstant&, const JITConstant&) = default;
};
#endif


class JITCode : public ThreadSafeRefCounted<JSC::JITCode> {
public:
    template<PtrTag tag> using CodeRef = MacroAssemblerCodeRef<tag>;

    static ASCIILiteral typeName(JITType);

    static JITType bottomTierJIT()
    {
        return JITType::BaselineJIT;
    }
    
    static JITType topTierJIT()
    {
        return JITType::FTLJIT;
    }
    
    static JITType nextTierJIT(JITType jitType)
    {
        switch (jitType) {
        case JITType::BaselineJIT:
            return JITType::DFGJIT;
        case JITType::DFGJIT:
            return JITType::FTLJIT;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return JITType::None;
        }
    }
    
    static bool isExecutableScript(JITType jitType)
    {
        switch (jitType) {
        case JITType::None:
        case JITType::HostCallThunk:
            return false;
        default:
            return true;
        }
    }
    
    static bool couldBeInterpreted(JITType jitType)
    {
        switch (jitType) {
        case JITType::InterpreterThunk:
        case JITType::BaselineJIT:
            return true;
        default:
            return false;
        }
    }
    
    static bool isJIT(JITType jitType)
    {
        switch (jitType) {
        case JITType::BaselineJIT:
        case JITType::DFGJIT:
        case JITType::FTLJIT:
            return true;
        default:
            return false;
        }
    }

    static bool isLowerTierPrecise(JITType expectedLower, JITType expectedHigher)
    {
        RELEASE_ASSERT(isExecutableScript(expectedLower));
        RELEASE_ASSERT(isExecutableScript(expectedHigher));
        return expectedLower < expectedHigher;
    }
    
    static bool isHigherTier(JITType expectedHigher, JITType expectedLower)
    {
        return isLowerTierPrecise(expectedLower, expectedHigher);
    }
    
    static bool isLowerOrSameTier(JITType expectedLower, JITType expectedHigher)
    {
        return !isHigherTier(expectedLower, expectedHigher);
    }
    
    static bool isHigherOrSameTier(JITType expectedHigher, JITType expectedLower)
    {
        return isLowerOrSameTier(expectedLower, expectedHigher);
    }
    
    static bool isOptimizingJIT(JITType jitType)
    {
        return jitType == JITType::DFGJIT || jitType == JITType::FTLJIT;
    }
    
    static bool isBaselineCode(JITType jitType)
    {
        return jitType == JITType::InterpreterThunk || jitType == JITType::BaselineJIT;
    }

    virtual const DOMJIT::Signature* signature() const { return nullptr; }

    virtual bool canSwapCodeRefForDebugger() const { return false; }
    virtual CodeRef<JSEntryPtrTag> swapCodeRefForDebugger(CodeRef<JSEntryPtrTag>);
    
    enum class ShareAttribute : uint8_t {
        NotShared,
        Shared
    };

protected:
    JITCode(JITType, CodePtr<JSEntryPtrTag> = nullptr, JITCode::ShareAttribute = JITCode::ShareAttribute::NotShared);
    
public:
    virtual ~JITCode();
    
    JITType jitType() const
    {
        return m_jitType;
    }

    bool NODELETE isUnlinked() const;
    
    template<typename PointerType>
    static JITType jitTypeFor(PointerType jitCode)
    {
        if (!jitCode)
            return JITType::None;
        return jitCode->jitType();
    }

    void* addressForCall() const { return m_addressForCall.taggedPtr(); }

    virtual CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) = 0;
    virtual void* executableAddressAtOffset(size_t offset) = 0;
    void* executableAddress() { return executableAddressAtOffset(0); }
    virtual void* dataAddressAtOffset(size_t offset) = 0;
    virtual unsigned offsetOf(void* pointerIntoCode) = 0;
    
    virtual DFG::CommonData* dfgCommon();
    virtual const DFG::CommonData* dfgCommon() const;
    virtual DFG::JITCode* dfg();
    virtual FTL::JITCode* ftl();
    virtual FTL::ForOSREntryJITCode* ftlForOSREntry();
    virtual void shrinkToFit();
    
    virtual void validateReferences(const TrackedReferences&);
    
    void* start() { return dataAddressAtOffset(0); }
    virtual size_t size() = 0;
    void* end() { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(start()) + size()); }
    
    virtual bool contains(void*) = 0;

#if ENABLE(JIT)
    virtual RegisterSet liveRegistersToPreserveAtExceptionHandlingCallSite(CodeBlock*, CallSiteIndex);
    virtual std::optional<CodeOrigin> findPC(CodeBlock*, void* pc) { UNUSED_PARAM(pc); return std::nullopt; }
#endif

    Intrinsic intrinsic() { return m_intrinsic; }

    bool isShared() const { return m_shareAttribute == ShareAttribute::Shared; }

    virtual PCToCodeOriginMap* pcToCodeOriginMap() { return nullptr; }

    const RegisterAtOffsetList* calleeSaveRegisters() const;

    static constexpr ptrdiff_t offsetOfJITType() { return OBJECT_OFFSETOF(JSC::JITCode, m_jitType); }

private:
    const JITType m_jitType;
    const ShareAttribute m_shareAttribute;
protected:
    Intrinsic m_intrinsic { NoIntrinsic }; // Effective only in NativeExecutable.
    CodePtr<JSEntryPtrTag> m_addressForCall;
};

// TSAN code-lifecycle family (docs/threads/TSAN-TRIAGE.md section 3.5; SPEC-jit
// section 4.x publish-one-pointer): GIL-off, JIT code is consumed lock-free
// through ONE published pointer (ExecutableBase::m_jitCodeFor{Call,Construct},
// CodeBlock::m_jitCode) while installs replace that pointer concurrently from
// another lite. Plain C++ accesses to those pointer words are UB; everything
// below makes each cross-thread access an atomic on the underlying word:
//
//  - publishes are RELEASE stores (or release exchanges), so a reader that
//    observes the new pointer also observes the fully-constructed JITCode
//    behind it (vptr, m_jitType, m_addressForCall). This is what retires the
//    ctor-vs-virtual-call (vptr) pairs at linkFor/prepareOSREntry and the
//    JITCode::JITCode x jitType pairs: construction happens-before the
//    release publish.
//  - consumes are RELAXED loads in production builds. Every deref is
//    address-dependent on the loaded pointer, and all supported targets
//    (x86-64 TSO; ARM64 address dependencies) order dependent loads after the
//    pointer load, so the consume side needs no barrier and the codegen is
//    the identical plain mov/ldr the field had before — flag-off semantics
//    and codegen are unchanged by construction. C++ cannot spell consume
//    ordering (memory_order_consume is specified-as-acquire and deprecated),
//    and TSAN's happens-before machinery does not propagate release clocks
//    through relaxed loads, so under TSAN ONLY the load is acquire: that
//    expresses to the checker exactly the dependency ordering the hardware
//    already guarantees, with zero effect on non-TSAN builds.
#if TSAN_ENABLED
inline constexpr std::memory_order JITCodePointerConsumeOrder = std::memory_order_acquire;
#else
inline constexpr std::memory_order JITCodePointerConsumeOrder = std::memory_order_relaxed;
#endif

// Concurrent accessors for the raw arity-check entrypoint mirrors
// (ExecutableBase::m_jitCodeFor{Call,Construct}WithArityCheck): single
// pointer-sized words, published/retracted by installCode and read lock-free
// by entrypointFor and the virtual-call thunk slow paths.
template<PtrTag tag>
ALWAYS_INLINE CodePtr<tag> concurrentCodePtrLoad(const CodePtr<tag>& slot)
{
    static_assert(sizeof(CodePtr<tag>) == sizeof(void*));
    void* bits = WTF::atomicLoad(const_cast<void**>(reinterpret_cast<void* const*>(&slot)), JITCodePointerConsumeOrder);
    return std::bit_cast<CodePtr<tag>>(bits);
}

template<PtrTag tag>
ALWAYS_INLINE void concurrentCodePtrStore(CodePtr<tag>& slot, CodePtr<tag> value)
{
    static_assert(sizeof(CodePtr<tag>) == sizeof(void*));
    WTF::atomicStore(reinterpret_cast<void**>(&slot), std::bit_cast<void*>(value), std::memory_order_release);
}

// The published jit-code pointer itself. Drop-in replacement for the
// RefPtr<JITCode> fields that are read by foreign lites: same single-pointer
// layout (OBJECT_OFFSETOF consumers and JIT-emitted loads are unaffected),
// but every load is a consume-ordered atomic and every replace is a release
// exchange. Refcounting discipline is identical to RefPtr; the displaced
// value is deref'd after the exchange. Lifetime of the loaded pointer is the
// code-lifecycle protocol's concern (epoch reclamation / isShared()
// immortality premise — see FunctionExecutable::codeBlockWithEntrypointFor),
// not this class's.
class ConcurrentJITCodePtr {
public:
    ConcurrentJITCodePtr()
    {
        // Relaxed atomic null-init: TSAN pairs this constructor store against
        // later concurrent atomic readers of the same word (the enclosing
        // object's publication supplies the ordering); making it atomic keeps
        // the access defined without codegen change.
        WTF::atomicStore(&m_ptr, static_cast<JITCode*>(nullptr), std::memory_order_relaxed);
    }
    ConcurrentJITCodePtr(const ConcurrentJITCodePtr&) = delete;
    ConcurrentJITCodePtr& operator=(const ConcurrentJITCodePtr&) = delete;

    ~ConcurrentJITCodePtr()
    {
        // Teardown is never concurrent with readers (the owning cell is
        // unreachable); the relaxed load keeps TSAN symmetry with racing
        // accesses earlier in the object's life.
        if (JITCode* pointer = WTF::atomicLoad(&m_ptr, std::memory_order_relaxed))
            pointer->deref();
    }

    JITCode* get() const { return loadConsume(); }
    JITCode* operator->() const { return loadConsume(); }
    JITCode& operator*() const { return *loadConsume(); }
    operator bool() const { return !!loadConsume(); }
    bool operator!() const { return !loadConsume(); }

    ConcurrentJITCodePtr& operator=(RefPtr<JITCode>&& newCode)
    {
        // Release publish: the JITCode must be fully constructed before this
        // store; the exchange is the publication point.
        JITCode* oldPointer = WTF::atomicExchange(&m_ptr, newCode.leakRef(), std::memory_order_release);
        if (oldPointer)
            oldPointer->deref();
        return *this;
    }

    ConcurrentJITCodePtr& operator=(Ref<JITCode>&& newCode) { return *this = RefPtr<JITCode>(WTF::move(newCode)); }
    ConcurrentJITCodePtr& operator=(std::nullptr_t) { return *this = RefPtr<JITCode>(); }

    // Retract-and-take, for handing the displaced code to retirement
    // (RetiredJITArtifacts) instead of dropping it.
    RefPtr<JITCode> take()
    {
        return adoptRef(WTF::atomicExchange(&m_ptr, static_cast<JITCode*>(nullptr), std::memory_order_release));
    }

private:
    JITCode* loadConsume() const { return WTF::atomicLoad(const_cast<JITCode**>(&m_ptr), JITCodePointerConsumeOrder); }

    JITCode* m_ptr; // Initialized in the constructor via relaxed atomic store.
};

static_assert(sizeof(ConcurrentJITCodePtr) == sizeof(JITCode*));

class JITCodeWithCodeRef : public JSC::JITCode {
protected:
    JITCodeWithCodeRef(JITType);
    JITCodeWithCodeRef(CodeRef<JSEntryPtrTag>, JITType, JITCode::ShareAttribute);

public:
    ~JITCodeWithCodeRef() override;

    void* executableAddressAtOffset(size_t offset) override;
    void* dataAddressAtOffset(size_t offset) override;
    unsigned offsetOf(void* pointerIntoCode) override;
    size_t size() override;
    bool contains(void*) override;

    CodeRef<JSEntryPtrTag> swapCodeRefForDebugger(CodeRef<JSEntryPtrTag>) override;

protected:
    RefPtr<ExecutableMemoryHandle> m_executableMemory;
};

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(DirectJITCode);
class DirectJITCode : public JITCodeWithCodeRef {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(DirectJITCode, DirectJITCode);
public:
    DirectJITCode(JITType);
    DirectJITCode(CodeRef<JSEntryPtrTag>, CodePtr<JSEntryPtrTag> withArityCheck, JITType, JITCode::ShareAttribute = JITCode::ShareAttribute::NotShared);
    DirectJITCode(CodeRef<JSEntryPtrTag>, CodePtr<JSEntryPtrTag> withArityCheck, JITType, Intrinsic, JITCode::ShareAttribute = JITCode::ShareAttribute::NotShared); // For generated thunk.
    ~DirectJITCode() override;
    
    CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) override;

protected:
    void initializeCodeRefForDFG(CodeRef<JSEntryPtrTag>, CodePtr<JSEntryPtrTag> withArityCheck);

private:
    CodePtr<JSEntryPtrTag> m_withArityCheck;
};

class NativeJITCode : public JITCodeWithCodeRef {
public:
    NativeJITCode(JITType);
    NativeJITCode(CodeRef<JSEntryPtrTag>, JITType, Intrinsic, JITCode::ShareAttribute = JITCode::ShareAttribute::NotShared);
    ~NativeJITCode() override;

    CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) override;

    bool canSwapCodeRefForDebugger() const override { return true; }
};

class NativeDOMJITCode final : public NativeJITCode {
public:
    NativeDOMJITCode(CodeRef<JSEntryPtrTag>, JITType, Intrinsic, const DOMJIT::Signature*);
    ~NativeDOMJITCode() final = default;

    const DOMJIT::Signature* signature() const final { return m_signature; }

private:
    const DOMJIT::Signature* m_signature;
};

} // namespace JSC

namespace WTF {

class PrintStream;
void printInternal(PrintStream&, JSC::JITType);

} // namespace WTF
