/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "Nodes.h"
#include "PythonAST.h"
#include "PythonArena.h"
#include "PythonFunctionInfo.h"
#include "PythonSymbolTable.h"

namespace JSC { namespace Python {

// What BytecodeGenerator is given to generate the code of. To it, this is the root of a syntax tree, and it asks it to emit itself.
class ScopeNode final : public JSC::ScopeNode {
public:
    // `root` is a Module, a FunctionDef, a Lambda, a ClassDef or a GeneratorExp, by the kind that the info says.
    ScopeNode(ParserArena&, const SourceCode&, Arena&, SymbolTable&, Block&, const FunctionInfo&, void* root);

    // Of the JavaScript function, not counting `this`.
    unsigned parameterCount() const;

    void emitBytecode(BytecodeGenerator&, RegisterID* = nullptr) final;

    // Set if the code could not be generated.
    const SyntaxError& error() const { return m_error; }

private:
    Arena& m_arena;
    SymbolTable& m_symbolTable;
    Block& m_block;
    const FunctionInfo& m_info;
    void* m_root;
    SyntaxError m_error;
};

} } // namespace JSC::Python
