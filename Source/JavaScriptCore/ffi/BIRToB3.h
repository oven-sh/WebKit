/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)

#include "B3Origin.h"
#include "B3Type.h"
#include "BIRModule.h"
#include <span>
#include <wtf/Function.h>
#include <wtf/Vector.h>

namespace JSC {

namespace B3 {
class BasicBlock;
class Procedure;
class Value;
class Variable;
namespace Air {
class StackSlot;
} // namespace Air
} // namespace B3

namespace FFI {

// Where a loaded module's pieces live. functionTable has one entry per BIR function and is
// filled in as functions are compiled; calls between BIR functions load their target from it.
struct BIRLinkEnvironment {
    uint8_t* dataBase { nullptr };
    void* const* functionTable { nullptr };
    std::span<void* const> externAddresses;
    // Passed to threadLocalBase to find (or create) the calling thread's copy of the module's tls.
    void* threadLocalContext { nullptr };
    void* (SYSV_ABI *threadLocalBase)(void* context) { nullptr };
};

class BIRToB3 {
public:
    BIRToB3(const BIR::Module&, const BIRLinkEnvironment&, B3::Procedure&);

    // Fills the (empty) procedure with `functionIndex` as a function callable through the
    // host C ABI.
    void lowerFunction(unsigned functionIndex);

    // Emits the body of `functionIndex` starting at `block`, reading its parameters from
    // `arguments`. Control continues in `continuation`; `result` is null for void functions.
    struct Inlined {
        B3::BasicBlock* continuation { nullptr };
        B3::Value* result { nullptr }; // The first of `results`.
        Vector<B3::Value*, 1> results;
    };
    Inlined lowerInline(unsigned functionIndex, B3::BasicBlock* block, std::span<B3::Value* const> arguments, B3::Origin);

    // Lets a client that keeps its own block order (the FTL) create the blocks.
    void setBlockFactory(Function<B3::BasicBlock*()>&& factory) { m_blockFactory = WTF::move(factory); }

    // Functions whose entry the emitted code reads from the function table (a call that was
    // not inlined, or FuncAddr). They need machine code of their own.
    const Vector<unsigned>& referencedFunctions() const { return m_referencedFunctions; }

    static B3::Type toB3(BIR::Type);

private:
    struct ReturnTarget {
        B3::BasicBlock* continuation { nullptr };
        Vector<B3::Variable*, 1> results;
    };

    void emitBody(const BIR::Function&, B3::BasicBlock* entry, std::span<B3::Value* const> arguments, const ReturnTarget*);
    void emitInst(const BIR::Function&, const BIR::Inst&);
    Vector<B3::Value*, 1> emitCall(const BIR::Function&, const BIR::Inst&, const BIR::Signature&, B3::Value* target);
    Vector<B3::Value*, 1> emitPatchpointCall(const BIR::Signature&, B3::Value* target, const Vector<B3::Value*>& arguments);
    void emitReturn(const BIR::Function&, const BIR::Inst&);
    bool shouldInlineCallee(unsigned functionIndex, bool hasConstantArgument) const;
    B3::Value* emitFloatToInt(BIR::Type result, B3::Value*, bool isSigned);
    B3::Value* emitIntToFloat(BIR::Type result, B3::Value*, bool isSigned);
    B3::Value* emitBitOperation(BIR::Op, B3::Value*);
    B3::Value* emitVectorOperation(const BIR::Function&, const BIR::Inst&);
    B3::Value* emitVectorConversion(BIR::VConvertKind, B3::Value*);
    B3::Value* emitLaneWise(BIR::Lane, bool isSigned, B3::Value* a, B3::Value* b, const Function<B3::Value*(B3::Value*, B3::Value*)>&);
    B3::Value* emitAtomic(const BIR::Inst&);
    void emitVariadicEntry(const BIR::Signature&, B3::BasicBlock* entry);
    void emitVaStart(B3::Value* vaList);
    B3::Value* emitStackOperation(const BIR::Inst&);
    B3::Value* emitVolatileAccess(const BIR::Inst&);
    void emitInlineAssembly(const BIR::Function&, const BIR::Inst&);
    bool emitSmallMemoryCopy(B3::Value* destination, B3::Value* source, B3::Value* size);
    bool emitSmallMemoryFill(B3::Value* destination, B3::Value* byte, B3::Value* size);
    B3::Value* constant(B3::Type, int64_t);
    B3::Value* pointer(const void*);
    B3::BasicBlock* newBlock();

    const BIR::Module& m_module;
    const BIRLinkEnvironment& m_environment;
    B3::Procedure& m_proc;
    B3::Origin m_origin;
    Function<B3::BasicBlock*()> m_blockFactory;

    B3::BasicBlock* m_block { nullptr };

    // State of the function body being emitted; swapped out while a callee is inlined into it.
    struct Body {
        Vector<B3::Value*> values;
        Vector<B3::BasicBlock*> blocks;
        Vector<B3::Variable*> locals;
        Vector<B3::Air::StackSlot*> slots;
        const ReturnTarget* returnTarget { nullptr };
    };
    Body m_body;
    Vector<unsigned, 4> m_inlineStack;
    Vector<unsigned> m_referencedFunctions;

    // Set up at the entry of a variadic function for VaStart to describe.
    struct VariadicFrame {
        B3::Air::StackSlot* registerSaveArea { nullptr };
        unsigned namedGPRs { 0 };
        unsigned namedFPRs { 0 };
        unsigned namedStackBytes { 0 };
        unsigned namedArguments { 0 };
    };
    VariadicFrame m_variadicFrame;
    unsigned m_inlinedInstructionBudget { 0 };
    unsigned m_alwaysInlineInstructionBudget { 200000 };
};

} // namespace FFI

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)
