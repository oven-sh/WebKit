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
        return type() == FunctionExecutableType || type() == ShortFunctionExecutableType;
    }
    bool hasAOTEntry() const { return m_aotEntry[0] || m_aotEntry[1]; } // It is AOT::ProgramOfVM's.
    bool isShortFunctionExecutable() const // See FunctionExecutable.
    {
        return type() == ShortFunctionExecutableType;
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
    Ref<JSC::JITCode> generatedJITCodeForCall() const
    {
        ASSERT(m_jitCodeForCall);
        return *m_jitCodeForCall;
    }

    Ref<JSC::JITCode> generatedJITCodeForConstruct() const
    {
        ASSERT(m_jitCodeForConstruct);
        return *m_jitCodeForConstruct;
    }

    // To keep hold of while it runs. Code that was compiled when the program was built stays, and has none.
    JSC::JITCode* jitCodeIfAnyFor(CodeSpecializationKind kind) const { return isShortFunctionExecutable() ? nullptr : (kind == CodeSpecializationKind::CodeForCall ? WTF::opaque(this)->m_jitCodeForCall : WTF::opaque(this)->m_jitCodeForConstruct).get(); } // (opaque(): or it may be read before it is known to be there.)

    void* generatedJITCodeAddressForCall() const
    {
        ASSERT(m_jitCodeForCall);
        return m_jitCodeForCall->addressForCall();
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
        if (!isShortFunctionExecutable()) [[likely]] {
            if (CodePtr<JSEntryPtrTag> result = WTF::opaque(this)->m_jitCodeForCallWithArityCheck) [[likely]]
                return result;
        }
        return entrypointOfStaticCode(CodeSpecializationKind::CodeForCall);
    }

    CodePtr<JSEntryPtrTag> generatedJITCodeWithArityCheckForConstruct() const
    {
        if (!isShortFunctionExecutable()) [[likely]] {
            if (CodePtr<JSEntryPtrTag> result = WTF::opaque(this)->m_jitCodeForConstructWithArityCheck) [[likely]]
                return result;
        }
        return entrypointOfStaticCode(CodeSpecializationKind::CodeForConstruct);
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
        if (m_aotEntry[static_cast<unsigned>(kind)] || isShortFunctionExecutable()) [[unlikely]]
            return entrypointOfStaticCode(kind);
        ExecutableBase* inFull = WTF::opaque(this);
        if (arity == ArityCheckMode::MustCheckArity) {
            switch (kind) {
            case CodeSpecializationKind::CodeForCall:
                if (CodePtr<JSEntryPtrTag> result = inFull->m_jitCodeForCallWithArityCheck)
                    return result;
                break;
            case CodeSpecializationKind::CodeForConstruct:
                if (CodePtr<JSEntryPtrTag> result = inFull->m_jitCodeForConstructWithArityCheck)
                    return result;
                break;
            }
        }
        CodePtr<JSEntryPtrTag> result = generatedJITCodeFor(kind)->addressForCall(arity);
        if (arity == ArityCheckMode::MustCheckArity) {
            // Cache the result; this is necessary for the JIT's virtual call optimizations.
            switch (kind) {
            case CodeSpecializationKind::CodeForCall:
                inFull->m_jitCodeForCallWithArityCheck = result;
                break;
            case CodeSpecializationKind::CodeForConstruct:
                inFull->m_jitCodeForConstructWithArityCheck = result;
                break;
            }
        }
        return result;
    }

    static constexpr ptrdiff_t offsetOfJITCodeFor(CodeSpecializationKind kind)
    {
        switch (kind) {
        case CodeSpecializationKind::CodeForCall:
            return OBJECT_OFFSETOF(ExecutableBase, m_jitCodeForCall);
        case CodeSpecializationKind::CodeForConstruct:
            return OBJECT_OFFSETOF(ExecutableBase, m_jitCodeForConstruct);
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
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
        RELEASE_ASSERT(!isShortFunctionExecutable());
        if (kind == CodeSpecializationKind::CodeForCall)
            return swapGeneratedJITCodeForCallWithArityCheckForDebugger(jitCodeWithArityCheck);
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return swapGeneratedJITCodeForConstructWithArityCheckForDebugger(jitCodeWithArityCheck);
    }

    CodePtr<JSEntryPtrTag> swapGeneratedJITCodeForCallWithArityCheckForDebugger(CodePtr<JSEntryPtrTag> jitCodeForCallWithArityCheck)
    {
        auto old = m_jitCodeForCallWithArityCheck;
        m_jitCodeForCallWithArityCheck = jitCodeForCallWithArityCheck;
        return old;
    }

    CodePtr<JSEntryPtrTag> swapGeneratedJITCodeForConstructWithArityCheckForDebugger(CodePtr<JSEntryPtrTag> jitCodeForConstructWithArityCheck)
    {
        auto old = m_jitCodeForConstructWithArityCheck;
        m_jitCodeForConstructWithArityCheck = jitCodeForConstructWithArityCheck;
        return old;
    }
    
    void dump(PrintStream&) const;
        
protected:
    // What m_jitCodeFor*WithArityCheck would be, of a FunctionExecutable of a program that was compiled ahead of time. It does not say so itself: in the
    // short form it has no room to, and in full it could only do so with an address, and the code is wherever it was mapped.
    JS_EXPORT_PRIVATE CodePtr<JSEntryPtrTag> entrypointOfStaticCode(CodeSpecializationKind) const;

    // Of a FunctionExecutable: see aotEntryFor().
    uint64_t m_aotEntry[2] { }; // AOT::EntryWord
    uint32_t m_aotIndex[2] { };
    // The short form of a FunctionExecutable ends here (FunctionExecutable::sizeOfShortForm), so the fields below must not be
    // accessed on one. (On other executables they are accessed through WTF::opaque(this). Otherwise the compiler could hoist the
    // load above the check for the short form.)
    CodePtr<JSEntryPtrTag> m_jitCodeForCallWithArityCheck;
    CodePtr<JSEntryPtrTag> m_jitCodeForConstructWithArityCheck;
    RefPtr<JSC::JITCode> m_jitCodeForCall;
    RefPtr<JSC::JITCode> m_jitCodeForConstruct;
};

} // namespace JSC
