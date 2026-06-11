/*
 * Copyright (C) 2009-2023 Apple Inc. All rights reserved.
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
#include "CallData.h"
#include "CodeBlockHash.h"
#include "CodeSpecializationKind.h"
#include "JITCode.h"
#include "JSCConfig.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC {

class CodeBlock;
class EvalCodeBlock;
class FunctionCodeBlock;
class JSScope;
class JSWebAssemblyModule;
class LLIntOffsetsExtractor;
class ModuleProgramCodeBlock;
class ProgramCodeBlock;

enum class CompilationKind { FirstCompilation, OptimizingCompilation };

inline bool isCall(CodeSpecializationKind kind)
{
    if (kind == CodeSpecializationKind::CodeForCall)
        return true;
    ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
    return false;
}

class ExecutableBase : public JSCell {
    friend class JIT;
    friend class LLIntOffsetsExtractor;
    friend MacroAssemblerCodeRef<JSEntryPtrTag> boundFunctionCallGenerator(VM*);
    using Base = JSCell;

protected:
    ExecutableBase(VM& vm, Structure* structure)
        : JSCell(vm, structure)
    {
    }

    DECLARE_DEFAULT_FINISH_CREATION;

public:
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);
    
    // Force subclasses to override this.
    template<typename, SubspaceAccess>
    static void subspaceFor(VM&) { }
        
    CodeBlockHash hashFor(CodeSpecializationKind) const;

    bool isEvalExecutable() const
    {
        return type() == EvalExecutableType;
    }
    bool isFunctionExecutable() const
    {
        return type() == FunctionExecutableType;
    }
    bool isProgramExecutable() const
    {
        return type() == ProgramExecutableType;
    }
    bool isModuleProgramExecutable()
    {
        return type() == ModuleProgramExecutableType;
    }
    bool isHostFunction() const
    {
        return type() == NativeExecutableType;
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_EXPORT_INFO;

public:
    // TSAN code-lifecycle (TSAN-TRIAGE.md section 3.5): m_jitCodeForCall /
    // m_jitCodeForConstruct are the published jit-code pointers — replaced by
    // a concurrent installCode while read lock-free here. All accesses go
    // through ConcurrentJITCodePtr (consume-ordered load / release publish;
    // see jit/JITCode.h). The single-load-then-deref shape below is load
    // bearing: one atomic load, everything after is address-dependent.
    Ref<JSC::JITCode> generatedJITCodeForCall() const
    {
        JSC::JITCode* jitCode = m_jitCodeForCall.get();
        ASSERT(jitCode);
        return *jitCode;
    }

    Ref<JSC::JITCode> generatedJITCodeForConstruct() const
    {
        JSC::JITCode* jitCode = m_jitCodeForConstruct.get();
        ASSERT(jitCode);
        return *jitCode;
    }

    void* generatedJITCodeAddressForCall() const
    {
        JSC::JITCode* jitCode = m_jitCodeForCall.get();
        ASSERT(jitCode);
        return jitCode->addressForCall();
    }

    Ref<JSC::JITCode> generatedJITCodeFor(CodeSpecializationKind kind) const
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return generatedJITCodeForCall();
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return generatedJITCodeForConstruct();
    }

    CodePtr<JSEntryPtrTag> generatedJITCodeWithArityCheckForCall() const
    {
        return concurrentCodePtrLoad(m_jitCodeForCallWithArityCheck);
    }

    CodePtr<JSEntryPtrTag> generatedJITCodeWithArityCheckForConstruct() const
    {
        return concurrentCodePtrLoad(m_jitCodeForConstructWithArityCheck);
    }

    CodePtr<JSEntryPtrTag> generatedJITCodeWithArityCheckFor(CodeSpecializationKind kind) const
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return generatedJITCodeWithArityCheckForCall();
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return generatedJITCodeWithArityCheckForConstruct();
    }

    CodePtr<JSEntryPtrTag> entrypointFor(CodeSpecializationKind kind, ArityCheckMode arity)
    {
        // Check if we have a cached result. We only have it for arity check because we use the
        // no-arity entrypoint in non-virtual calls, which will "cache" this value directly in
        // machine code.
        if (arity == ArityCheckMode::MustCheckArity) {
            switch (kind) {
            case CodeSpecializationKind::CodeForCall:
                if (CodePtr<JSEntryPtrTag> result = concurrentCodePtrLoad(m_jitCodeForCallWithArityCheck))
                    return result;
                break;
            case CodeSpecializationKind::CodeForConstruct:
                if (CodePtr<JSEntryPtrTag> result = concurrentCodePtrLoad(m_jitCodeForConstructWithArityCheck))
                    return result;
                break;
            }
        }
        // AB17d (ANNEX CBI item 3, amended): gilOff-process, a concurrent
        // ScriptExecutable::installCode retracts m_jitCodeFor* FIRST
        // (retract-first store order), so (a) the unconditional
        // generatedJITCodeFor deref below can hit the transient null even
        // when a pre-retraction hasJITCodeForCall() gate passed, and (b) the
        // lazy arity-cache refill below could store an entrypoint derived
        // from the OLD jit code AFTER the install retracted the slot — a
        // STABLE stale value that the thunks' slot-recompare revalidation
        // cannot distinguish from a fresh one (value-recurrence/ABA: arity
        // entrypoints are addresses inside long-lived JITCode objects and
        // recur across jettison/reinstall of the same CodeBlock). Under the
        // process gate we therefore NEVER write the lazy cache and
        // null-check the racy jit-code read: script executables keep a
        // permanently-null arity mirror gilOff (installCode/clearCode only
        // ever store null to it), so the virtual-call thunks' fast path
        // simply never engages for them and every virtual call derives a
        // matched (entrypoint, CodeBlock) pair through the CodeBlock
        // snapshot in the C++ slow path (RepatchInlines.h linkFor /
        // virtualForWithFunction). Host executables publish both mirrors
        // once at construction (NativeExecutable.cpp) and never retract, so
        // they keep the thunk fast path with no torn pair possible.
        // Flag-off: one predicted-false byte test on the read-only Config
        // page; byte-identical behavior otherwise.
        if (g_jscConfig.gilOffProcess) [[unlikely]] {
            JSC::JITCode* jitCode = (kind == CodeSpecializationKind::CodeForCall ? m_jitCodeForCall : m_jitCodeForConstruct).get();
            if (!jitCode)
                return nullptr;
            return jitCode->addressForCall(arity);
        }
        CodePtr<JSEntryPtrTag> result = generatedJITCodeFor(kind)->addressForCall(arity);
        if (arity == ArityCheckMode::MustCheckArity) {
            // Cache the result; this is necessary for the JIT's virtual call optimizations.
            switch (kind) {
            case CodeSpecializationKind::CodeForCall:
                concurrentCodePtrStore(m_jitCodeForCallWithArityCheck, result);
                break;
            case CodeSpecializationKind::CodeForConstruct:
                concurrentCodePtrStore(m_jitCodeForConstructWithArityCheck, result);
                break;
            }
        }
        return result;
    }

    static constexpr ptrdiff_t offsetOfJITCodeWithArityCheckFor(
        CodeSpecializationKind kind)
    {
        switch (kind) {
        case CodeSpecializationKind::CodeForCall:
            return OBJECT_OFFSETOF(ExecutableBase, m_jitCodeForCallWithArityCheck);
        case CodeSpecializationKind::CodeForConstruct:
            return OBJECT_OFFSETOF(ExecutableBase, m_jitCodeForConstructWithArityCheck);
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
    
    bool hasJITCodeForCall() const;
    bool hasJITCodeForConstruct() const;

    bool hasJITCodeFor(CodeSpecializationKind kind) const
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return hasJITCodeForCall();
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return hasJITCodeForConstruct();
    }

    // Intrinsics are only for calls, currently.
    inline Intrinsic intrinsic() const;
        
    inline Intrinsic intrinsicFor(CodeSpecializationKind) const;

    ImplementationVisibility implementationVisibility() const;
    InlineAttribute inlineAttribute() const;

    CodePtr<JSEntryPtrTag> swapGeneratedJITCodeWithArityCheckForDebugger(CodeSpecializationKind kind, CodePtr<JSEntryPtrTag> jitCodeWithArityCheck)
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return swapGeneratedJITCodeForCallWithArityCheckForDebugger(jitCodeWithArityCheck);
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return swapGeneratedJITCodeForConstructWithArityCheckForDebugger(jitCodeWithArityCheck);
    }

    CodePtr<JSEntryPtrTag> swapGeneratedJITCodeForCallWithArityCheckForDebugger(CodePtr<JSEntryPtrTag> jitCodeForCallWithArityCheck)
    {
        auto old = concurrentCodePtrLoad(m_jitCodeForCallWithArityCheck);
        concurrentCodePtrStore(m_jitCodeForCallWithArityCheck, jitCodeForCallWithArityCheck);
        return old;
    }

    CodePtr<JSEntryPtrTag> swapGeneratedJITCodeForConstructWithArityCheckForDebugger(CodePtr<JSEntryPtrTag> jitCodeForConstructWithArityCheck)
    {
        auto old = concurrentCodePtrLoad(m_jitCodeForConstructWithArityCheck);
        concurrentCodePtrStore(m_jitCodeForConstructWithArityCheck, jitCodeForConstructWithArityCheck);
        return old;
    }
    
    void dump(PrintStream&) const;
        
protected:
    // TSAN code-lifecycle (section 3.5): published jit-code pointers, replaced
    // concurrently by installCode while read lock-free — all accesses are
    // atomics via ConcurrentJITCodePtr / concurrentCodePtr{Load,Store}
    // (jit/JITCode.h). Same single-pointer layout as the RefPtr/CodePtr these
    // used to be, so offsetOfJITCodeWithArityCheckFor consumers and
    // JIT-emitted loads are unaffected.
    ConcurrentJITCodePtr m_jitCodeForCall;
    ConcurrentJITCodePtr m_jitCodeForConstruct;
    CodePtr<JSEntryPtrTag> m_jitCodeForCallWithArityCheck;
    CodePtr<JSEntryPtrTag> m_jitCodeForConstructWithArityCheck;
};

} // namespace JSC
