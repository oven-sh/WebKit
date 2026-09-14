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

#if USE(BUN_JSC_ADDITIONS)

#include "BIR.h"
#include "FFIType.h"
#include "JSExportMacros.h"
#include <span>
#include <expected>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace FFI { namespace BIR {

constexpr uint32_t noValue = UINT32_MAX;

struct Parameter {
    ParamKind kind { ParamKind::Value };
    Type type { Type::I64 }; // The type of the parameter's value: ByValStack and IndirectResult are addresses.
    uint64_t size { 0 }; // ByValStack only.
    uint64_t alignment { 8 }; // ByValStack only.
    Exhausts exhausts { Exhausts::Nothing }; // ByValStack only.
};

struct Signature {
    Vector<Type, 1> results;
    bool isVariadic { false };
    Vector<Parameter> parameters;

    // True when a plain C call with scalar arguments and at most one scalar result describes it.
    bool isScalar() const
    {
        if (isVariadic || results.size() > 1)
            return false;
        if (!results.isEmpty() && results[0] == Type::V128)
            return false;
        for (const Parameter& parameter : parameters) {
            if (parameter.kind != ParamKind::Value || parameter.type == Type::V128)
                return false;
        }
        return true;
    }
    Type soleResultOrVoid() const { return results.isEmpty() ? Type::Void : results[0]; }
};

struct Extern {
    CString name;
    ExternKind kind { ExternKind::Function };
    bool isWeak { false };
    uint32_t signature { 0 };
};

struct Reloc {
    uint64_t offset { 0 };
    RelocKind kind { RelocKind::Data };
    uint64_t index { 0 };
    int64_t addend { 0 };
};

struct ThreadLocalData {
    uint64_t size { 0 };
    uint64_t alignment { 1 };
    Vector<uint8_t> initialized;
    Vector<Reloc> relocs;
};

struct Data {
    uint64_t size { 0 };
    uint64_t alignment { 1 };
    uint64_t readOnlySize { 0 };
    Vector<uint8_t> initialized;
    Vector<Reloc> relocs;
};

struct Slot {
    uint64_t size { 0 };
    uint64_t alignment { 1 };
};

// a/b/c are value ids or table indices depending on op (operand order of BIR.h); imm is the
// constant, memory offset, or float bits. Vector ops keep their lane (or VConvertKind) in aux and
// `signed | index << 8` in imm; ConstV128 and VShuffle keep their 16 bytes as two words in extra.
// Atomics keep their MemKind in aux and `order | failureOrder << 8 | op << 16` in imm. Calls, Ret and Switch keep their variable-length tail
// in Function::extra[extraOffset, extraOffset + extraCount): value ids for calls and Ret,
// (case, block) pairs for Switch. A call with n results defines values result..result + n - 1;
// resultType is the first one's.
struct Inst {
    Op op { Op::Unreachable };
    uint8_t aux { 0 };
    Type resultType { Type::Void };
    uint32_t result { noValue };
    uint8_t resultCount { 0 };
    bool isVolatile { false }; // Load and Store only.
    uint32_t a { 0 };
    uint32_t b { 0 };
    uint32_t c { 0 };
    int64_t imm { 0 };
    uint32_t extraOffset { 0 };
    uint32_t extraCount { 0 };
};

struct Block {
    uint32_t firstInst { 0 };
    uint32_t instCount { 0 };
};

struct Function {
    String name;
    uint32_t signature { 0 };
    bool isExported { false };
    uint32_t callSiteCount { 0 }; // Call instructions naming it, in the whole module.
    bool isAddressTaken { false }; // FuncAddr or a Func reloc: reachable by means other than Call.
    bool usesFrameAddress { false }; // Must keep a frame of its own: never inlined.
    bool isAlwaysInline { false }; // `__attribute__((always_inline))`: specialized by its callers' constant arguments.
    bool isNeverInline { false }; // `__attribute__((noinline))`.
    bool hasInlineHint { false }; // Declared with the `inline` keyword.
    bool callsReturnsTwice { false }; // Calls setjmp or the like: control can come back into its frame.
    Vector<Type> locals;
    Vector<Slot> slots;
    Vector<Block> blocks;
    Vector<Inst> insts;
    Vector<int64_t> extra;
    uint32_t valueCount { 0 };
    bool hasCalls { false }; // Call, CallExtern or CallIndirect: may re-enter JS.
    bool movesStackPointer { false }; // StackAlloc or StackRestore.
    bool hasBody { true }; // False once compiled, for a function too large to ever be inlined again.

    void releaseBody()
    {
        hasBody = false;
        locals = { };
        slots = { };
        blocks = { };
        insts = { };
        extra = { };
    }
};

struct Export {
    String name;
    uint32_t function { 0 };
    FFI::Type returnType { FFI::Type::Void };
    Vector<FFI::Type> arguments;
};

class Module {
    WTF_MAKE_TZONE_ALLOCATED(Module);
    WTF_MAKE_NONCOPYABLE(Module);
public:
    Module() = default;

    JS_EXPORT_PRIVATE static std::expected<std::unique_ptr<Module>, String> decode(std::span<const uint8_t>);

    Arch arch { Arch::X86_64 };
    OS os { OS::Linux };
    Vector<Signature> signatures;
    Vector<Extern> externs;
    Data data;
    ThreadLocalData tls;
    Vector<Function> functions;
    Vector<Export> exports;
    Vector<CString> libraries;
    Vector<uint32_t> constructors;
    Vector<uint32_t> destructors;
    bool usesVectors { false }; // Some value, local, parameter or result is a v128.
};

} } } // namespace JSC::FFI::BIR

#endif // USE(BUN_JSC_ADDITIONS)
