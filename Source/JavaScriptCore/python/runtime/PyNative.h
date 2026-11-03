/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CallFrame.h"
#include "InternalFunction.h"
#include "PyTuple.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// How a call passes its arguments: the positional ones, then the values of the keyword ones, as the arguments of a JS call.
// `this` is a tuple of the names of the keywords (a cell of type PyKwNamesType), or anything else when there are none.

class CallArgs {
public:
    CallArgs() = default;
    CallArgs(const JSValue* values, unsigned positionalCount, PyTuple* keywordNames = nullptr)
        : m_values(values)
        , m_count(positionalCount)
        , m_keywordNames(keywordNames)
    {
    }
    CallArgs(std::span<const JSValue> positional)
        : m_values(positional.data())
        , m_count(static_cast<unsigned>(positional.size()))
    {
    }
    CallArgs(std::initializer_list<JSValue> positional)
        : m_values(positional.begin())
        , m_count(static_cast<unsigned>(positional.size()))
    {
    }
    explicit CallArgs(CallFrame* callFrame)
        : m_values(reinterpret_cast<const JSValue*>(callFrame->addressOfArgumentsStart()))
        , m_count(static_cast<unsigned>(callFrame->argumentCount()))
    {
        JSValue thisValue = callFrame->thisValue();
        if (thisValue.isCell() && thisValue.asCell()->type() == PyKwNamesType) [[unlikely]] {
            m_keywordNames = uncheckedDowncast<PyTuple>(thisValue.asCell());
            ASSERT(m_count >= m_keywordNames->length());
            m_count -= m_keywordNames->length();
        }
    }

    // The positional arguments.
    unsigned size() const { return m_count; }
    bool isEmpty() const { return !m_count; }
    JSValue operator[](unsigned index) const
    {
        ASSERT(index < m_count);
        return m_values[index];
    }
    JSValue get(unsigned index) const { return index < m_count ? m_values[index] : JSValue(); }
    std::span<const JSValue> positional() const { return { m_values, m_count }; }

    bool hasKeywords() const { return m_keywordNames; }
    unsigned keywordCount() const { return m_keywordNames ? m_keywordNames->length() : 0; }
    JSString* keywordName(unsigned index) const { return asString(m_keywordNames->at(index)); }
    JSValue keywordValue(unsigned index) const { return m_values[m_count + index]; }
    PyTuple* keywordNames() const { return m_keywordNames; }
    // Empty: not given.
    JSValue keyword(JSString* name) const;
    JSValue keyword(ASCIILiteral name) const;

    const JSValue* values() const { return m_values; }
    unsigned totalCount() const { return m_count + keywordCount(); }

    CallArgs withoutFirst() const
    {
        ASSERT(m_count);
        return { m_values + 1, m_count - 1, m_keywordNames };
    }

private:
    const JSValue* m_values { nullptr };
    unsigned m_count { 0 };
    PyTuple* m_keywordNames { nullptr };
};

// ---- Functions written in C++ ----
//
// One is described by its name, a signature in the notation of __text_signature__, and a C++ function. For
//
//     "($self, sep=None, maxsplit=-1, /)"
//
// the function is JSValue f(JSGlobalObject*, JSValue self, JSValue sep, JSValue maxsplit): one parameter for each in the
// signature, which is empty if the caller left it out. The caller's arguments are checked against the signature.
// With *args or **kwargs in the signature, it is JSValue f(JSGlobalObject*, JSValue self, const CallArgs&), and nothing is checked.
// (`self` is there only if the signature starts with a $ parameter.)
// What throws returns an empty value.

struct NativeSignature {
    uint8_t parameterCount { 0 }; // Not counting $self.
    uint8_t minPositional { 0 };
    uint8_t maxPositional { 0 };
    uint8_t positionalOnlyCount { 0 };
    bool hasSelf { false };
    bool isVariadic { false };
    bool takesKeywords { false };
    bool hasRequiredKeywordOnly { false };
    bool isValid { true };
};

constexpr NativeSignature parseNativeSignature(const char* text, size_t length)
{
    NativeSignature result;
    if (length < 2 || text[0] != '(' || text[length - 1] != ')') {
        result.isValid = false;
        return result;
    }
    size_t i = 1;
    size_t end = length - 1;
    bool first = true;
    bool keywordOnly = false;
    bool sawSlash = false;
    bool sawDefault = false;
    while (i < end) {
        size_t start = i;
        unsigned depth = 0;
        bool hasDefault = false;
        bool inString = false;
        while (i < end) {
            char c = text[i];
            if (inString) {
                if (c == '\'')
                    inString = false;
            } else if (c == '\'')
                inString = true;
            else if (c == '(' || c == '[' || c == '{')
                ++depth;
            else if (c == ')' || c == ']' || c == '}')
                --depth;
            else if (c == '=' && !depth)
                hasDefault = true;
            else if (c == ',' && !depth)
                break;
            ++i;
        }
        size_t tokenLength = i - start;
        if (i < end) {
            ++i;
            if (i < end && text[i] == ' ')
                ++i;
        }
        if (!tokenLength) {
            result.isValid = false;
            return result;
        }
        if (text[start] == '$') {
            if (!first)
                result.isValid = false;
            result.hasSelf = true;
        } else if (text[start] == '/' && tokenLength == 1) {
            sawSlash = true;
            result.positionalOnlyCount = result.parameterCount;
        } else if (text[start] == '*') {
            if (tokenLength == 1)
                keywordOnly = true;
            else
                result.isVariadic = true;
        } else {
            ++result.parameterCount;
            if (!keywordOnly) {
                ++result.maxPositional;
                if (!hasDefault) {
                    if (sawDefault)
                        result.isValid = false;
                    ++result.minPositional;
                } else
                    sawDefault = true;
            } else if (!hasDefault)
                result.hasRequiredKeywordOnly = true;
        }
        first = false;
    }
    UNUSED_VARIABLE(sawSlash);
    result.takesKeywords = result.positionalOnlyCount < result.parameterCount;
    return result;
}

enum class NativeKind : uint8_t {
    Function, // builtin_function_or_method: of a module, or bound to an object.
    Method, // method_descriptor
    SlotWrapper, // wrapper_descriptor: a method that stands for an operation, as int.__add__. Bound, it is a method-wrapper.
    ClassMethod, // classmethod_descriptor
    StaticMethod, // A Function in a staticmethod.
    New, // __new__: a Function bound to the type.
};

enum NativeFlag : uint8_t {
    // list.__add__, tuple.__mul__ and the like: for `a + b`, the methods of numbers are tried on both operands first.
    SequenceOperation = 1 << 0,
    // __get__, which refuses (None, None). Set by the name.
    DescriptorGet = 1 << 1,
    // See wipeStackBelow().
    OnWipedStack = 1 << 2,
};

class NativeDef {
public:
    static constexpr unsigned maxArity = 8;

    using F0 = JSValue (*)(JSGlobalObject*);
    using F1 = JSValue (*)(JSGlobalObject*, JSValue);
    using F2 = JSValue (*)(JSGlobalObject*, JSValue, JSValue);
    using F3 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue);
    using F4 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue);
    using F5 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue, JSValue);
    using F6 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue);
    using F7 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue);
    using F8 = JSValue (*)(JSGlobalObject*, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue, JSValue);
    using Variadic = JSValue (*)(JSGlobalObject*, const CallArgs&);
    using VariadicWithSelf = JSValue (*)(JSGlobalObject*, JSValue, const CallArgs&);

    union Pointer {
        F0 f0;
        F1 f1;
        F2 f2;
        F3 f3;
        F4 f4;
        F5 f5;
        F6 f6;
        F7 f7;
        F8 f8;
        Variadic variadic;
        VariadicWithSelf variadicWithSelf;

        constexpr Pointer(F0 f) : f0(f) { }
        constexpr Pointer(F1 f) : f1(f) { }
        constexpr Pointer(F2 f) : f2(f) { }
        constexpr Pointer(F3 f) : f3(f) { }
        constexpr Pointer(F4 f) : f4(f) { }
        constexpr Pointer(F5 f) : f5(f) { }
        constexpr Pointer(F6 f) : f6(f) { }
        constexpr Pointer(F7 f) : f7(f) { }
        constexpr Pointer(F8 f) : f8(f) { }
        constexpr Pointer(Variadic f) : variadic(f) { }
        constexpr Pointer(VariadicWithSelf f) : variadicWithSelf(f) { }
    };

    template<typename... Parameters>
    static constexpr int arityOf(JSValue (*)(JSGlobalObject*, Parameters...)) { return sizeof...(Parameters); }
    static constexpr int arityOf(Variadic) { return -1; }
    static constexpr int arityOf(VariadicWithSelf) { return -2; }

    template<typename Function>
    constexpr NativeDef(NativeKind kind, ASCIILiteral name, Function function, ASCIILiteral signature, uint8_t flags = 0)
        : name(name)
        , signatureText(signature)
        , pointer(function)
        , signature(parseNativeSignature(signature.characters(), signature.length()))
        , kind(kind)
        , flags(flags | (kind == NativeKind::SlotWrapper && name.length() == 7 && name[2] == 'g' && name[3] == 'e' && name[4] == 't' ? DescriptorGet : 0))
    {
        int arity = arityOf(function);
        bool matches = this->signature.isValid
            && (this->signature.isVariadic ? arity == (this->signature.hasSelf ? -2 : -1) : arity == this->signature.parameterCount + this->signature.hasSelf);
        if (!matches)
            signatureDoesNotMatchFunction();
    }

    ASCIILiteral name;
    ASCIILiteral signatureText;
    Pointer pointer;
    NativeSignature signature;
    NativeKind kind;
    uint8_t flags;

private:
    // Not constexpr, so that a table with a mistake in it does not compile.
    static void signatureDoesNotMatchFunction();
};

#define PY_FUNCTION(name, function, signature) NativeDef(NativeKind::Function, name ""_s, function, signature ""_s)
#define PY_METHOD(name, function, signature) NativeDef(NativeKind::Method, name ""_s, function, signature ""_s)
#define PY_SLOT(name, function, signature) NativeDef(NativeKind::SlotWrapper, name ""_s, function, signature ""_s)
#define PY_SEQUENCE_SLOT(name, function, signature) NativeDef(NativeKind::SlotWrapper, name ""_s, function, signature ""_s, SequenceOperation)
#define PY_CLASSMETHOD(name, function, signature) NativeDef(NativeKind::ClassMethod, name ""_s, function, signature ""_s)
#define PY_STATICMETHOD(name, function, signature) NativeDef(NativeKind::StaticMethod, name ""_s, function, signature ""_s)
#define PY_NEW(function, signature) NativeDef(NativeKind::New, "__new__"_s, function, signature ""_s)

// An attribute computed in C++: getset_descriptor.
struct NativeGetSet {
    using Getter = JSValue (*)(JSGlobalObject*, JSValue self);
    // An empty value deletes. What throws says so through the VM.
    using Setter = void (*)(JSGlobalObject*, JSValue self, JSValue value);

    ASCIILiteral name;
    Getter getter;
    Setter setter { nullptr };
    // What CPython has as a field of the object's struct (a PyMemberDef) shows: it is a member_descriptor, and without a
    // setter it is a "readonly attribute".
    bool isMember { false };
    ASCIILiteral doc { };
};

#define PY_MEMBER(name, getter) { name ""_s, getter, nullptr, true }
#define PY_WRITABLE_MEMBER(name, getter, setter) { name ""_s, getter, setter, true }

JSC_DECLARE_HOST_FUNCTION(callNativeFunction);

class PyNativeFunction final : public InternalFunction {
public:
    using Base = InternalFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | jsViewStructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    PY_DECLARE_JS_VIEW;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
    }

    // `kind` is that of the definition, or Function for one bound to `self`. `owner`: the type it is a method of, or the
    // name of the module it is a function of, or empty.
    static PyNativeFunction* create(JSGlobalObject*, const NativeDef&, NativeKind, JSValue self, JSValue owner);
    // The method bound to `self`.
    static PyNativeFunction* createBound(JSGlobalObject*, PyNativeFunction* method, JSValue self);

    static bool is(JSCell* cell) { return cell->type() == InternalFunctionType && uncheckedDowncast<InternalFunction>(cell)->nativeFunctionFor(CodeSpecializationKind::CodeForCall) == TaggedNativeFunction(callNativeFunction); }
    static bool is(JSValue value) { return value && value.isCell() && is(value.asCell()); }
    static PyNativeFunction* tryFrom(JSValue value) { return is(value) ? uncheckedDowncast<PyNativeFunction>(value.asCell()) : nullptr; }

    const NativeDef& def() const { return *m_def; }
    NativeKind kind() const { return m_kind; }
    JSValue self() const { return m_self.get(); } // Empty: not bound.
    JSValue owner() const { return m_owner.get(); }
    PyType* ownerType() const;
    bool isDescriptor() const { return m_kind == NativeKind::Method || m_kind == NativeKind::SlotWrapper || m_kind == NativeKind::ClassMethod; }

    JSValue invoke(JSGlobalObject*, const CallArgs&);
    // For a Method or a SlotWrapper, when `self` is known to be an instance of the owner.
    JSValue invokeWithSelf(JSGlobalObject*, JSValue self, const CallArgs&);

private:
    PyNativeFunction(VM&, Structure*, const NativeDef&, NativeKind);

    JSValue invokeNew(JSGlobalObject*, const CallArgs&);

    const NativeDef* m_def;
    NativeKind m_kind;
    WriteBarrier<Unknown> m_self;
    WriteBarrier<Unknown> m_owner;
};

// Calls the C++ function, having matched the arguments with its signature. `self` is ignored if the signature has none.
JSValue invokeNative(JSGlobalObject*, const NativeDef&, JSValue self, const CallArgs&);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
