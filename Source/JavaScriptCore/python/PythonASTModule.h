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

#include "PythonASDL.h"
#include "WriteBarrier.h"
#include <array>

namespace JSC {

class JSGlobalObject;
class JSObject;
class PyType;

namespace Python {

struct Module;

// What _ast has for each realm: struct ast_state of CPython's pycore_ast_state.h. It is made when it is first wanted.
struct ASTState {
    std::array<WriteBarrier<PyType>, numberOfASTClasses> classes;
    // Load(), Add() and the like. What comes of parsing has the same one wherever there is one.
    std::array<WriteBarrier<JSObject>, numberOfASTClasses> singletons;

    PyType* classFor(ASTClass astClass) const { return classes[static_cast<unsigned>(astClass)].get(); }
    JSObject* singletonFor(ASTClass astClass) const { return singletons[static_cast<unsigned>(astClass)].get(); }

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        for (auto& astClass : classes)
            visitor.append(astClass);
        for (auto& singleton : singletons)
            visitor.append(singleton);
    }
};

// Null if it raised.
ASTState* astState(JSGlobalObject*);

// PyAST_mod2obj()
JSValue objectFromAST(JSGlobalObject*, Module&);
// PyAST_Check()
bool isAST(JSGlobalObject*, JSValue);

} } // namespace JSC::Python
