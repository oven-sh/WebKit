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

#include <wtf/FastMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class CallFrame;
class JSGlobalObject;

namespace Python {

// What is built into Python and written in C says what its arguments are, in __text_signature__, and what it is for, in __doc__. In CPython the code
// that takes the arguments apart is generated from the first. Here nothing is generated: a function that is written in C++ has the same signature, as
// data, and its arguments are checked against that before it is called. So it can rely on having those that are required, and can get any of them by
// its position however it was given, and what is said when they are wrong is said in one place.
//
// The signatures and docstrings of what CPython has too are taken from CPython: see lib/dump-builtin-descriptions.py.

struct BuiltinDescription {
    // The type that CPython has for it.
    enum class Kind : uint8_t {
        Type,
        Module,
        WrapperDescriptor,
        MethodDescriptor,
        ClassMethodDescriptor,
        BuiltinFunction,
        StaticMethod,
        GetSetDescriptor,
        MemberDescriptor,
    };

    ASCIILiteral key; // "type", "type.attribute", "module:" or "module:function"
    Kind kind;
    ASCIILiteral signature; // Null if it has none.
    ASCIILiteral doc; // Likewise.
};

// An attribute of a module that is something to read, and is not its __doc__. It need not be ASCII.
struct BuiltinText {
    ASCIILiteral key; // "module:attribute"
    const char* utf8;
};

// What CPython says of how instances of a built-in class are laid out. The numbers mean nothing here, but programs can see them, and whether they are
// zero decides what a class derived from it can have.
struct BuiltinTypeLayout {
    ASCIILiteral name;
    int basicSize; // type.__basicsize__
    int itemSize; // type.__itemsize__: not zero if instances are not all of a size, and then they cannot be given __slots__.
    int dictOffset; // type.__dictoffset__: not zero if instances have a __dict__.
    int weakReferenceOffset; // type.__weakrefoffset__: not zero if there can be weak references to instances.
    unsigned long flags; // type.__flags__
};

// Null if CPython has no such thing.
const BuiltinTypeLayout* findTypeLayout(StringView type);
const BuiltinDescription* findTypeDescription(StringView type);
const BuiltinDescription* findAttributeDescription(StringView type, StringView attribute);
const BuiltinDescription* findModuleDescription(StringView module);
const BuiltinDescription* findFunctionDescription(StringView module, StringView function);
String findModuleText(ASCIILiteral key); // "module:attribute". There is to be one.

// A signature, taken apart.
class NativeSignature {
    WTF_MAKE_TZONE_ALLOCATED(NativeSignature);
public:
    // "($self, value, start=0, stop=sys.maxsize, /)"
    explicit NativeSignature(ASCIILiteral text);

    // How CPython would call a function with such a signature, which is what decides how it puts it when the arguments are wrong.
    enum class Family : uint8_t {
        Unchecked, // (*args, **kwargs): it sees to them itself.
        NoArguments,
        OneArgument,
        Positional, // None of them can be given by name.
        Keywords,
    };

    ASCIILiteral text() const { return m_text; }
    // What the function is called when its arguments are wrong, if that is neither its name nor its class's: "typevar(name, ...)". Null otherwise.
    const String& functionName() const { return m_functionName; }
    Family family() const { return m_family; }
    // Whether it begins with $self, $type or $module.
    bool hasImplicitFirst() const { return m_hasImplicitFirst; }
    // The names of all that have one, without the implicit first: those that can be given by position, and then those that cannot.
    const Vector<String>& names() const { return m_names; }
    unsigned positionalOnlyCount() const { return m_positionalOnlyCount; }
    unsigned requiredPositionalCount() const { return m_requiredPositionalCount; }
    unsigned positionalCount() const { return m_positionalCount; }
    unsigned requiredKeywordOnlyCount() const { return m_requiredKeywordOnlyCount; }
    bool hasVarPositional() const { return m_hasVarPositional; }
    bool hasVarKeywords() const { return m_hasVarKeywords; }

private:
    ASCIILiteral m_text;
    String m_functionName;
    Vector<String> m_names;
    unsigned m_positionalOnlyCount { 0 };
    unsigned m_requiredPositionalCount { 0 };
    unsigned m_positionalCount { 0 };
    unsigned m_requiredKeywordOnlyCount { 0 };
    bool m_hasImplicitFirst { false };
    bool m_hasVarPositional { false };
    bool m_hasVarKeywords { false };
    Family m_family { Family::Unchecked };
};

// Whether the arguments that a function written in C++ has been called with are ones that it can be called with. If not, TypeError has been raised.
// This is for when a look at how many there are was not enough to tell.
bool checkArgumentsSlow(JSGlobalObject*, CallFrame*);

// The one of `candidates` that `name` is most likely a slip for. Null if none is near enough.
String calculateSuggestion(const Vector<String>& candidates, const String& name);

} } // namespace JSC::Python
