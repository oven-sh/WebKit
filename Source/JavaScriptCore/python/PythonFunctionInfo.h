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

#include "Identifier.h"
#include "ImplementationVisibility.h"
#include "VirtualRegister.h"
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC { namespace Python {

// What kind of thing a piece of Python code is the code of. All of it is function code to JavaScriptCore.
enum class CodeKind : uint8_t {
    Module, // A file, or what is given to exec().
    Expression, // What is given to eval().
    Interactive, // What is typed at a prompt: compile(..., 'single').
    Function, // def
    Lambda,
    Class, // The body of a class statement. It is called once, with the namespace to fill in.
    GeneratorExpression,
};

// What is only known about a piece of code once it has been compiled, and is only wanted by what looks into it: a code object, and locals().
struct CodeDetails {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CodeDetails);
    Vector<Identifier> variableNames; // co_varnames: the parameters, and then the other local variables as they are first used.
    Vector<Identifier> names; // co_names: the globals and attributes, as they are first used.
    Vector<Identifier> cellVariables; // co_cellvars

    // What a frame object needs.
    struct FrameVariable {
        Identifier name;
        VirtualRegister location; // Not valid if it is in an environment, because an inner function uses it or it is an outer function's.
    };
    Vector<FrameVariable> frameVariables; // In the order of co_varnames, co_cellvars and co_freevars.
    VirtualRegister frameObjectRegister; // Where the frame object is, if there is one. Not valid for a generator, which keeps it itself.
    VirtualRegister scopeRegister;
    // Where what was written begins. Before it the arguments are being given to the parameters, and to Python there is no frame yet: what
    // is raised there is raised by the call.
    unsigned firstTraceableOffset { 0 };
};

// What has to be known about a piece of Python code before it is compiled, to call it and to compile it. It is worked out when what
// the code is in is compiled, and does not change.
struct FunctionInfo : ThreadSafeRefCounted<FunctionInfo> {
    CodeKind kind { CodeKind::Module };
    bool isGenerator { false };
    bool isCoroutine { false };
    bool isGeneratorBody { false }; // Of the two functions that a generator is made of, the one that is resumed.
    bool hasVariadic { false };
    bool hasKeywordVariadic { false };
    // Its names are looked up in a mapping that it is called with, before the globals. So it is for the body of a class, and for what
    // compile() makes, since exec() can be given any mapping for the local variables.
    bool usesNamespace { false };
    bool isNested { false }; // In a function.
    bool isMethod { false }; // Directly in a class.
    bool hasDocstring { false };
    unsigned futureFeatures { 0 };
    // Private for what comes with the engine and stands for what in CPython is written in C: it is in no traceback and no stack trace.
    ImplementationVisibility visibility { ImplementationVisibility::Public };
    unsigned line { 1 }; // That the source of it begins on.

    Identifier name;
    String qualifiedName;
    String docstring; // Null if it has none.
    Identifier privateName; // The class that names like __x are mangled for, if it is in one.

    // Its names, and those of what is in it, that are variables of functions it is in. All other names that it does not bind are global.
    Vector<Identifier> freeVariables;

    // First those that can be given by position, then those that can only be given by keyword, then *args, then **kwargs. That is
    // the order of the parameters of the JavaScript function.
    Vector<Identifier> parameterNames;
    unsigned positionalOnlyCount { 0 };
    unsigned positionalCount { 0 }; // Including those.
    unsigned keywordOnlyCount { 0 };

    // Filled in when it is compiled.
    mutable std::unique_ptr<CodeDetails> details;

    Ref<FunctionInfo> copy() const
    {
        auto result = adoptRef(*new FunctionInfo);
        result->kind = kind;
        result->isGenerator = isGenerator;
        result->isCoroutine = isCoroutine;
        result->isGeneratorBody = isGeneratorBody;
        result->hasVariadic = hasVariadic;
        result->hasKeywordVariadic = hasKeywordVariadic;
        result->usesNamespace = usesNamespace;
        result->isNested = isNested;
        result->isMethod = isMethod;
        result->hasDocstring = hasDocstring;
        result->futureFeatures = futureFeatures;
        result->visibility = visibility;
        result->line = line;
        result->name = name;
        result->qualifiedName = qualifiedName;
        result->docstring = docstring;
        result->privateName = privateName;
        result->freeVariables = freeVariables;
        result->parameterNames = parameterNames;
        result->positionalOnlyCount = positionalOnlyCount;
        result->positionalCount = positionalCount;
        result->keywordOnlyCount = keywordOnlyCount;
        return result;
    }

    unsigned parameterCount() const { return positionalCount + keywordOnlyCount + hasVariadic + hasKeywordVariadic; }
    unsigned variadicIndex() const { return positionalCount + keywordOnlyCount; }
    unsigned keywordVariadicIndex() const { return positionalCount + keywordOnlyCount + hasVariadic; }
};

} } // namespace JSC::Python
