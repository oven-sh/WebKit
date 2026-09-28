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
    Comprehension, // [x for ...] and its like, where it is not part of the code that it is in.
    Annotations, // What evaluates the annotations of a function, a class or a module, when they are asked for: its __annotate__.
    TypeParameters, // What makes the type parameters of a generic function, class or alias, and then that. It is called at once.
    Evaluator, // What evaluates, when it is asked for, the bound or the default of a type parameter, or what an alias is an alias of.
};

// Whether its variables are its own, and not the items of a namespace.
inline bool isFunctionKind(CodeKind kind)
{
    return kind == CodeKind::Function || kind == CodeKind::Lambda || kind == CodeKind::GeneratorExpression || kind == CodeKind::Annotations || kind == CodeKind::TypeParameters || kind == CodeKind::Evaluator;
}

struct FunctionInfo;
// What evaluates the annotations of a module is made before anything else in the module is done. But CPython generates its code when it has been through the module, and puts what makes it at the beginning
// afterwards. So it is the last of the module's constants, and what is wrong with it is found after whatever else is wrong.
bool isGeneratedLast(const FunctionInfo&);

// The last three have no source of their own. They are compiled from the source of what they belong to, which is one of these.
enum class OwnerKind : uint8_t {
    None,
    Function,
    Class,
    TypeAlias,
    Module,
    Interactive,
};

// What an Evaluator evaluates.
enum class Evaluates : uint8_t {
    Bound, // T: bound, and T: (a, b)
    Default, // T = default
    Value, // type A = value
};

// What is only known about a piece of code once it has been compiled, and is only wanted by what looks into it: a code object, and locals().
struct CodeDetails {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CodeDetails);
    Vector<Identifier> variableNames; // co_varnames: the parameters, and then the other local variables as they are first used.
    Vector<Identifier> names; // co_names: the globals and attributes, as they are first used.
    Vector<Identifier> cellVariables; // co_cellvars

    // co_consts: what is written out in the source, and the code of what is defined in it, as they are first come to.
    struct Constant {
        enum class Kind : uint8_t { None, True, False, Ellipsis, Integer, BigInteger, Float, Imaginary, String, Bytes, Code, Complex, Tuple, FrozenSet };
        Kind kind { Kind::None };
        uint8_t radix { 10 };
        bool isNegative { false };
        uint64_t bits { 0 }; // The integer, the bits of the double, or which of the functions that the code makes.
        String text; // The digits, the string, or a character for each byte.
        uint64_t imaginaryBits { 0 }; // Of Complex, whose real part is in `bits`.
        Vector<Constant> elements;
        friend bool operator==(const Constant&, const Constant&) = default;
    };
    Vector<Constant> constants;

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
    // Where op_py_enter is. What is thrown from before it is thrown from a frame that has not been counted.
    unsigned enterOffset { 0 };
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
    // It is in a class, though it is no part of the body of one, and what it does not find among its own names it looks for there first.
    bool canSeeClassScope { false };
    OwnerKind owner { OwnerKind::None };
    Evaluates evaluates { Evaluates::Value };
    unsigned typeParameterIndex { 0 }; // For an Evaluator of a bound or a default: of which.
    unsigned futureFeatures { 0 };
    // compile(optimize=...). From 1, __debug__ is False and there are no assert statements. From 2, there are no docstrings.
    uint8_t optimizationLevel { 0 };
    // Private for what comes with the engine and stands for what in CPython is written in C: it is in no traceback and no stack trace.
    ImplementationVisibility visibility { ImplementationVisibility::Public };
    unsigned line { 1 }; // That the source of it begins on.
    unsigned firstLine { 1 }; // That it says it begins on, co_firstlineno: the same, or that of the first decorator, which is no part of the source of it.
    // How much further on it says its lines are: code.replace(co_firstlineno=...). What is defined in it says what it did.
    int lineDelta { 0 };
    // A generator that can be awaited: types.coroutine(), which sets CO_ITERABLE_COROUTINE with code.replace(co_flags=...).
    bool isIterableCoroutine { false };

    Identifier name;
    String qualifiedName;
    // What that was in the source, if it has been changed: code.replace(co_qualname=...). What is defined in it is named after this.
    String qualifiedNameInSource;
    // For the three kinds that belong to something else. What is in one of them is named as if it were where that is.
    String qualifiedNamePrefix;
    String docstring; // Null if it has none.
    Identifier privateName; // The class that names like __x are mangled for, if it is in one.

    // Its names, and those of what is in it, that are variables of functions it is in. All other names that it does not bind are global.
    Vector<Identifier> freeVariables;
    // Those of them that it is given as cells, which it looks into, and not as variables of the environments that it is in. So it is for a function that a program
    // made out of a code object and a closure, function(code, globals, closure=...), and for what is defined in one. An environment cannot have a variable that is
    // another's, and a cell can be anyone's.
    Vector<Identifier> variablesGivenAsCells;

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
        result->canSeeClassScope = canSeeClassScope;
        result->owner = owner;
        result->evaluates = evaluates;
        result->typeParameterIndex = typeParameterIndex;
        result->futureFeatures = futureFeatures;
        result->optimizationLevel = optimizationLevel;
        result->visibility = visibility;
        result->line = line;
        result->firstLine = firstLine;
        result->lineDelta = lineDelta;
        result->isIterableCoroutine = isIterableCoroutine;
        result->name = name;
        result->qualifiedName = qualifiedName;
        result->qualifiedNameInSource = qualifiedNameInSource;
        result->qualifiedNamePrefix = qualifiedNamePrefix;
        result->docstring = docstring;
        result->privateName = privateName;
        result->freeVariables = freeVariables;
        result->variablesGivenAsCells = variablesGivenAsCells;
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
