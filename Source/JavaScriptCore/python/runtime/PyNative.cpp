/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyNative.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyJSView.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

JSValue CallArgs::keyword(JSString* name) const
{
    for (unsigned i = 0; i < keywordCount(); ++i) {
        JSString* candidate = keywordName(i);
        if (candidate == name || WTF::equal(candidate->tryGetValueImpl(), name->tryGetValueImpl()))
            return keywordValue(i);
    }
    return { };
}

JSValue CallArgs::keyword(ASCIILiteral name) const
{
    for (unsigned i = 0; i < keywordCount(); ++i) {
        if (WTF::equal(keywordName(i)->tryGetValueImpl(), name.span8()))
            return keywordValue(i);
    }
    return { };
}

// ---- Matching arguments with a signature ----

namespace {

struct NativeParameter {
    std::span<const Latin1Character> name;
    bool hasDefault;
};

}

static void parseParameters(const NativeDef& def, Vector<NativeParameter, NativeDef::maxArity>& parameters)
{
    auto text = def.signatureText.span8();
    size_t i = 1;
    size_t end = text.size() - 1;
    bool inOptionalGroup = false;
    while (i < end) {
        size_t start = i;
        size_t nameEnd = 0;
        size_t bracket = 0;
        bool isOptional = inOptionalGroup;
        unsigned depth = 0;
        bool inString = false;
        while (i < end) {
            Latin1Character ch = text[i];
            if (inString) {
                if (ch == '\'')
                    inString = false;
            } else if (ch == '\'')
                inString = true;
            else if ((ch == '[' || ch == ']') && !depth && !nameEnd) {
                // An optional group: see parseNativeSignature().
                if (!bracket)
                    bracket = i;
                if (ch == '[')
                    inOptionalGroup = true;
            } else if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
                --depth;
            else if (ch == '=' && !depth && !nameEnd)
                nameEnd = i;
            else if (ch == ',' && !depth)
                break;
            ++i;
        }
        size_t tokenEnd = i;
        if (i < end) {
            ++i;
            if (i < end && text[i] == ' ')
                ++i;
        }
        Latin1Character first = text[start];
        if (first == '$' || first == '/' || first == '*')
            continue;
        parameters.append({ text.subspan(start, (nameEnd ? nameEnd : bracket ? bracket : tokenEnd) - start), nameEnd || isOptional });
    }
}

// As in messages: "list.append()", "math.sqrt()", "len()".
static String describeFunction(JSGlobalObject* globalObject, const NativeDef& def, PyNativeFunction* function)
{
    if (function) {
        JSValue owner = function->owner();
        // One that is bound goes by what it is bound to, as its __qualname__ does: True.bit_length is bool.bit_length.
        if (JSValue bound = function->self(); bound && function->ownerType() && def.kind != NativeKind::New) {
            PyType* type = PyType::is(bound) ? PyType::from(bound) : typeOf(globalObject, bound);
            // What is bound to a module is taken for a function of the module.
            if (type->isSubtypeOf(runtime(globalObject).moduleType()))
                return makeString(def.name, "()"_s);
            return makeString(stringOf(type->qualname()), '.', def.name, "()"_s);
        }
        if (PyType* type = function->ownerType())
            return makeString(type->name(), '.', def.name, "()"_s);
        if (owner && owner.isString()) {
            String module = asString(owner)->value(globalObject);
            if (module != "builtins"_s)
                return makeString(module, '.', def.name, "()"_s);
        }
    }
    return makeString(def.name, "()"_s);
}

static ASCIILiteral plural(unsigned count) { return count == 1 ? ""_s : "s"_s; }

// A call that is in order, with keywords or with parameters left out, takes no reading of the signature's text: where the names
// are in it is remembered for the functions called last. False: the call may not be in order, and `values` are as they were.
static ALWAYS_INLINE bool tryBindNativeArguments(const NativeDef& def, const CallArgs& args, JSValue* values)
{
    struct Entry {
        const NativeDef* def { nullptr };
        struct {
            uint16_t offset;
            uint8_t length;
            bool hasDefault;
        } parameters[NativeDef::maxArity];
    };
    static constexpr unsigned cacheSize = 64;
    static thread_local std::array<Entry, cacheSize> cache;

    auto& signature = def.signature;
    unsigned total = signature.parameterCount;
    unsigned count = args.size();
    unsigned keywordCount = args.keywordCount();
    if (!signature.takesKeywords || count + keywordCount > total || count > signature.maxPositional)
        return false;

    auto text = def.signatureText.span8();
    Entry& entry = cache[(reinterpret_cast<uintptr_t>(&def) / sizeof(NativeDef)) % cacheSize];
    if (entry.def != &def) [[unlikely]] {
        Vector<NativeParameter, NativeDef::maxArity> parameters;
        parseParameters(def, parameters);
        ASSERT(parameters.size() == total);
        entry.def = nullptr;
        for (unsigned i = 0; i < total; ++i) {
            size_t offset = parameters[i].name.data() - text.data();
            if (offset > UINT16_MAX || parameters[i].name.size() > UINT8_MAX)
                return false;
            entry.parameters[i] = { static_cast<uint16_t>(offset), static_cast<uint8_t>(parameters[i].name.size()), parameters[i].hasDefault };
        }
        entry.def = &def;
    }

    for (unsigned i = 0; i < count; ++i)
        values[i] = args[i];
    bool isInOrder = true;
    for (unsigned k = 0; k < keywordCount && isInOrder; ++k) {
        StringImpl* name = args.keywordName(k)->tryGetValueImpl();
        unsigned index = signature.positionalOnlyCount;
        while (index < total && !WTF::equal(name, text.subspan(entry.parameters[index].offset, entry.parameters[index].length)))
            ++index;
        if (index < total && !values[index])
            values[index] = args.keywordValue(k);
        else
            isInOrder = false;
    }
    for (unsigned i = 0; i < total && isInOrder; ++i)
        isInOrder = values[i] || entry.parameters[i].hasDefault;
    if (isInOrder)
        return true;
    for (unsigned i = 0; i < total; ++i)
        values[i] = JSValue();
    return false;
}

static NEVER_INLINE bool bindNativeArgumentsSlow(JSGlobalObject* globalObject, const NativeDef& def, PyNativeFunction* function, const CallArgs& args, JSValue* values)
{
    if (tryBindNativeArguments(def, args, values)) [[likely]]
        return true;

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& signature = def.signature;
    unsigned count = args.size();
    unsigned keywordCount = args.keywordCount();
    unsigned min = signature.minPositional;
    unsigned max = signature.maxPositional;

    // What makes an instance goes by the name of the type: "property() takes at most 4 arguments (5 given)".
    bool isConstructor = function && function->ownerType() && (def.name == "__init__"_s || def.name == "__new__"_s);
    bool isSlotWrapper = def.kind == NativeKind::SlotWrapper && !isConstructor;
    String name = isConstructor ? function->ownerType()->name() : String(def.name);

    if (!signature.takesKeywords) {
        if (keywordCount) {
            if (isConstructor)
                raiseTypeError(globalObject, scope, name, "() takes no keyword arguments"_s);
            else if (isSlotWrapper)
                raiseTypeError(globalObject, scope, "wrapper "_s, name, "() takes no keyword arguments"_s);
            else
                raiseTypeError(globalObject, scope, describeFunction(globalObject, def, function), " takes no keyword arguments"_s);
            return false;
        }
        if (isSlotWrapper && min == max && max <= 1)
            raiseTypeError(globalObject, scope, "expected "_s, min, " argument"_s, plural(min), ", got "_s, count);
        else if (isSlotWrapper && min == 1 && max == 2 && def.name != "__get__"_s)
            raiseTypeError(globalObject, scope, "expected 1 or 2 arguments, got "_s, count);
        else if (!max && !isConstructor)
            raiseTypeError(globalObject, scope, describeFunction(globalObject, def, function), " takes no arguments ("_s, count, " given)"_s);
        else if (min == 1 && max == 1 && !isConstructor)
            raiseTypeError(globalObject, scope, describeFunction(globalObject, def, function), " takes exactly one argument ("_s, count, " given)"_s);
        else if (min == max)
            raiseTypeError(globalObject, scope, name, " expected "_s, min, " argument"_s, plural(min), ", got "_s, count);
        else if (count < min)
            raiseTypeError(globalObject, scope, name, " expected at least "_s, min, " argument"_s, plural(min), ", got "_s, count);
        else
            raiseTypeError(globalObject, scope, name, " expected at most "_s, max, " argument"_s, plural(max), ", got "_s, count);
        return false;
    }

    Vector<NativeParameter, NativeDef::maxArity> parameters;
    parseParameters(def, parameters);
    unsigned total = parameters.size();
    ASSERT(total == signature.parameterCount);

    if (count + keywordCount > total) {
        raiseTypeError(globalObject, scope, name, "() takes at most "_s, total, ' ', !count ? "keyword "_s : ""_s, "argument"_s, plural(total), " ("_s, count + keywordCount, " given)"_s);
        return false;
    }
    if (count > max) {
        if (!max)
            raiseTypeError(globalObject, scope, name, "() takes no positional arguments"_s);
        else
            raiseTypeError(globalObject, scope, name, "() takes "_s, min == max ? "exactly "_s : "at most "_s, max, " positional argument"_s, plural(max), " ("_s, count, " given)"_s);
        return false;
    }
    unsigned requiredPositionalOnly = std::min<unsigned>(min, signature.positionalOnlyCount);
    if (count < requiredPositionalOnly) {
        raiseTypeError(globalObject, scope, name, "() takes "_s, requiredPositionalOnly == max ? "exactly "_s : "at least "_s, requiredPositionalOnly, " positional argument"_s, plural(requiredPositionalOnly), " ("_s, count, " given)"_s);
        return false;
    }

    for (unsigned i = 0; i < count; ++i)
        values[i] = args[i];
    auto parameterOf = [&](unsigned keyword) {
        StringImpl* name = args.keywordName(keyword)->tryGetValueImpl();
        unsigned index = signature.positionalOnlyCount;
        while (index < total && !WTF::equal(name, parameters[index].name))
            ++index;
        return index;
    };
    bool allKeywordsFit = true;
    for (unsigned k = 0; k < keywordCount; ++k) {
        unsigned index = parameterOf(k);
        if (index < total && !values[index])
            values[index] = args.keywordValue(k);
        else
            allKeywordsFit = false;
    }

    // A parameter left out is complained of first, then one given twice, then a keyword that is no parameter.
    for (unsigned i = 0; i < total; ++i) {
        if (!values[i] && !parameters[i].hasDefault) {
            raiseTypeError(globalObject, scope, name, "() missing required argument '"_s, StringView(parameters[i].name), "' (pos "_s, i + 1, ')');
            return false;
        }
    }
    if (allKeywordsFit)
        return true;
    unsigned firstGivenTwice = total;
    for (unsigned k = 0; k < keywordCount; ++k)
        firstGivenTwice = std::min(firstGivenTwice, parameterOf(k) < count ? parameterOf(k) : total);
    if (firstGivenTwice < total) {
        raiseTypeError(globalObject, scope, "argument for "_s, name, "() given by name ('"_s, StringView(parameters[firstGivenTwice].name), "') and position ("_s, firstGivenTwice + 1, ')');
        return false;
    }
    unsigned unexpected = 0;
    while (parameterOf(unexpected) < total)
        ++unexpected;
    String keyword = stringOf(args.keywordName(unexpected));
    Vector<String> candidates;
    for (unsigned i = signature.positionalOnlyCount; i < total; ++i)
        candidates.append(String(parameters[i].name));
    String suggestion = calculateSuggestion(candidates, keyword);
    if (suggestion.isNull())
        raiseTypeError(globalObject, scope, name, "() got an unexpected keyword argument '"_s, keyword, '\'');
    else
        raiseTypeError(globalObject, scope, name, "() got an unexpected keyword argument '"_s, keyword, "'. Did you mean '"_s, suggestion, "'?"_s);
    return false;
}

JSValue raiseUnexpectedKeyword(JSGlobalObject* globalObject, StringView function, const String& keyword, std::span<const ASCIILiteral> names)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<String> candidates;
    for (ASCIILiteral name : names)
        candidates.append(String(name));
    String suggestion = calculateSuggestion(candidates, keyword);
    if (suggestion.isNull())
        return raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, keyword, '\'');
    return raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, keyword, "'. Did you mean '"_s, suggestion, "'?"_s);
}

static NEVER_INLINE JSValue raiseNativeStackOverflow(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
}

static ALWAYS_INLINE JSValue invokeNativeImpl(JSGlobalObject* globalObject, const NativeDef& def, PyNativeFunction* function, JSValue self, const CallArgs& args)
{
    // Whatever recursion there is among functions written in C++ (the hash of a tuple of tuples...) comes through here.
    if (!getVM(globalObject).isSafeToRecurseSoft()) [[unlikely]]
        return raiseNativeStackOverflow(globalObject);

    auto& signature = def.signature;
    if (signature.isVariadic)
        return signature.hasSelf ? def.pointer.variadicWithSelf(globalObject, self, args) : def.pointer.variadic(globalObject, args);

    JSValue values[NativeDef::maxArity + 1];
    unsigned offset = signature.hasSelf;
    values[0] = self;
    unsigned count = args.size();
    if (!args.hasKeywords() && count >= signature.minPositional && count <= signature.maxPositional && !signature.hasRequiredKeywordOnly) [[likely]] {
        for (unsigned i = 0; i < count; ++i)
            values[offset + i] = args[i];
    } else if (!bindNativeArgumentsSlow(globalObject, def, function, args, values + offset))
        return { };
    if ((def.flags & DescriptorGet) && isNone(values[1]) && (!values[2] || isNone(values[2]))) [[unlikely]] {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s);
    }

    switch (signature.parameterCount + offset) {
    case 0:
        return def.pointer.f0(globalObject);
    case 1:
        return def.pointer.f1(globalObject, values[0]);
    case 2:
        return def.pointer.f2(globalObject, values[0], values[1]);
    case 3:
        return def.pointer.f3(globalObject, values[0], values[1], values[2]);
    case 4:
        return def.pointer.f4(globalObject, values[0], values[1], values[2], values[3]);
    case 5:
        return def.pointer.f5(globalObject, values[0], values[1], values[2], values[3], values[4]);
    case 6:
        return def.pointer.f6(globalObject, values[0], values[1], values[2], values[3], values[4], values[5]);
    case 7:
        return def.pointer.f7(globalObject, values[0], values[1], values[2], values[3], values[4], values[5], values[6]);
    case 8:
        return def.pointer.f8(globalObject, values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue invokeNative(JSGlobalObject* globalObject, const NativeDef& def, JSValue self, const CallArgs& args)
{
    return invokeNativeImpl(globalObject, def, nullptr, self, args);
}

// ---- PyNativeFunction ----

const ClassInfo PyNativeFunction::s_info = { "PyNativeFunction"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyNativeFunction) };

static JSC_DECLARE_HOST_FUNCTION(callNativeFunctionWithFrame);
static JSC_DECLARE_HOST_FUNCTION(callNativeFunctionAgainOnWipedStack);

// This one has no frame to speak of, so that all the frames of the call come after the wiping.
JSC_DEFINE_HOST_FUNCTION(callNativeFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    if (auto flags = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->def().flags; flags & (OnWipedStack | MayWait)) [[unlikely]] {
        if (flags & OnWipedStack)
            wipeStackBelow(getVM(globalObject));
        else
            runtime(globalObject).threads().willCallFunctionThatMayWait(callFrame);
    }
    [[clang::musttail]] return callNativeFunctionWithFrame(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION_WITH_ATTRIBUTES(callNativeFunctionAgainOnWipedStack, static NEVER_INLINE, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    wipeStackBelow(getVM(globalObject), stackBytesWipedForWaiting);
    [[clang::musttail]] return callNativeFunctionWithFrame(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION_WITH_ATTRIBUTES(callNativeFunctionWithFrame, static NEVER_INLINE, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* function = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee());
    CallArgs args(callFrame);
    if (!args.hasKeywords() && hasKeywordMarker(globalObject, callFrame)) [[unlikely]]
        return JSValue::encode(resultForCaller(globalObject, callFrame, callWithKeywordMarker(globalObject, callFrame)));
    VM& vm = getVM(globalObject);
    JSValue result = function->invoke(globalObject, args);
    // The frame of this function is gone when that one runs.
    if (!result && (function->def().flags & MayWait) && runtime(globalObject).threads().takeRequestToCallAgain(globalObject)) [[unlikely]]
        [[clang::musttail]] return callNativeFunctionAgainOnWipedStack(globalObject, callFrame);
    // CPython looks for signals after every call. Code counts on it: os.kill(os.getpid(), n), and the handler has run.
    // (Only signals. A finalizer between next(counter) and queue.put() in queue.put(next(counter)) is more than code is used to.)
    if (vm.traps().needHandling(VMTraps::NeedPythonCalls) && result && !checkSignals(globalObject)) [[unlikely]]
        return { };
    return JSValue::encode(resultForCaller(globalObject, callFrame, result));
}

PY_DEFINE_JS_VIEW(PyNativeFunction)

PyNativeFunction::PyNativeFunction(VM& vm, Structure* structure, const NativeDef& def, NativeKind kind)
    : Base(vm, structure, callNativeFunction, nullptr)
    , m_def(&def)
    , m_kind(kind)
{
}

PyNativeFunction* PyNativeFunction::create(JSGlobalObject* globalObject, const NativeDef& def, NativeKind kind, JSValue self, JSValue owner)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* function = new (NotNull, allocateCell<PyNativeFunction>(vm)) PyNativeFunction(vm, rt.nativeFunctionStructure(), def, kind);
    function->finishCreation(vm);
    // JSC takes every InternalFunction to have one: for stack traces, for Function.prototype.toString().
    function->m_originalName.set(vm, function, jsString(vm, String(def.name)));
    function->m_self.set(vm, function, self);
    function->m_owner.set(vm, function, owner);
    return function;
}

PyNativeFunction* PyNativeFunction::createBound(JSGlobalObject* globalObject, PyNativeFunction* method, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* function = new (NotNull, allocateCell<PyNativeFunction>(vm)) PyNativeFunction(vm, rt.nativeFunctionStructure(), method->def(), NativeKind::Function);
    function->finishCreation(vm);
    function->m_originalName.set(vm, function, method->m_originalName.get());
    function->m_self.set(vm, function, self);
    function->m_owner.set(vm, function, method->owner());
    return function;
}

template<typename Visitor>
void PyNativeFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyNativeFunction>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_self);
    visitor.append(thisObject->m_owner);
}

DEFINE_VISIT_CHILDREN(PyNativeFunction);

PyType* PyNativeFunction::ownerType() const
{
    return PyType::tryFrom(m_owner.get());
}

JSValue PyNativeFunction::invokeWithSelf(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    return invokeNativeImpl(globalObject, *m_def, this, self, args);
}

// T.__new__(S, ...)
JSValue PyNativeFunction::invokeNew(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = ownerType();
    if (args.isEmpty())
        return raiseTypeError(globalObject, scope, type->nameInMessages(), ".__new__(): not enough arguments"_s);
    PyType* subtype = PyType::tryFrom(args[0]);
    if (!subtype)
        return raiseTypeError(globalObject, scope, type->nameInMessages(), ".__new__(X): X is not a type object ("_s, typeName(globalObject, args[0]), ')');
    if (!subtype->isSubtypeOf(type))
        return raiseTypeError(globalObject, scope, type->nameInMessages(), ".__new__("_s, subtype->nameInMessages(), "): "_s, subtype->nameInMessages(), " is not a subtype of "_s, type->nameInMessages());
    // object.__new__(dict) would make a dict that dict.__new__ had no say in.
    PyType* staticBase = subtype;
    while (staticBase && staticBase->isHeapType())
        staticBase = staticBase->base();
    if (staticBase && staticBase->has(TypeFlag::DisallowInstantiation))
        return raiseTypeError(globalObject, scope, "cannot create '"_s, subtype->nameInMessages(), "' instances"_s);
    // The types that share a function, as the exceptions do, may stand in for each other.
    auto isSameFunction = [&](JSValue other) {
        auto* native = PyNativeFunction::tryFrom(other);
        return native && !memcmp(&native->def().pointer, &m_def->pointer, sizeof(m_def->pointer));
    };
    if (staticBase && !isSameFunction(staticBase->lookup(Special::New))) {
        // The first whose __new__ is not written in Python has the one to use.
        PyType* user = subtype;
        while (user != staticBase && !PyNativeFunction::tryFrom(user->lookup(Special::New)))
            user = user->base();
        return raiseTypeError(globalObject, scope, type->nameInMessages(), ".__new__("_s, subtype->nameInMessages(), ") is not safe, use "_s, user->nameInMessages(), ".__new__()"_s);
    }
    JSValue object = invokeNativeImpl(globalObject, *m_def, this, subtype, args.withoutFirst());
    RETURN_IF_EXCEPTION(scope, { });
    // typeCall() sees to this for what is made by calling the class. copy and pickle make objects this way.
    if (subtype->isHeapType() && subtype->lookup(Special::Del) && object.isCell() && typeOf(globalObject, object) == subtype) [[unlikely]]
        runtime(globalObject).registerFinalizable(object.asCell());
    return object;
}

JSValue PyNativeFunction::invoke(JSGlobalObject* globalObject, const CallArgs& args)
{
    switch (m_kind) {
    case NativeKind::Function:
    case NativeKind::StaticMethod:
        return invokeNativeImpl(globalObject, *m_def, this, m_self.get(), args);
    case NativeKind::New:
        return invokeNew(globalObject, args);
    case NativeKind::Method:
    case NativeKind::SlotWrapper:
    case NativeKind::ClassMethod:
        break;
    }

    // Called through the type: list.append(x, 1)
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* owner = ownerType();
    if (args.isEmpty()) {
        if (m_kind == NativeKind::Method)
            return raiseTypeError(globalObject, scope, "unbound method "_s, owner->name(), '.', m_def->name, "() needs an argument"_s);
        return raiseTypeError(globalObject, scope, "descriptor '"_s, m_def->name, "' of '"_s, owner->nameInMessages(), "' object needs an argument"_s);
    }
    JSValue self = args[0];
    if (m_kind == NativeKind::ClassMethod) {
        PyType* type = PyType::tryFrom(self);
        if (!type)
            return raiseTypeError(globalObject, scope, "descriptor '"_s, m_def->name, "' for type '"_s, owner->nameInMessages(), "' needs a type, not a '"_s, typeName(globalObject, self), "' as arg 2"_s);
        if (!type->isSubtypeOf(owner))
            return raiseTypeError(globalObject, scope, "descriptor '"_s, m_def->name, "' requires a subtype of '"_s, owner->nameInMessages(), "' but received '"_s, type->nameInMessages(), '\'');
    } else if (!typeOf(globalObject, self)->isSubtypeOf(owner)) {
        if (m_kind == NativeKind::SlotWrapper)
            return raiseTypeError(globalObject, scope, "descriptor '"_s, m_def->name, "' requires a '"_s, owner->nameInMessages(), "' object but received a '"_s, typeName(globalObject, self), '\'');
        return raiseTypeError(globalObject, scope, "descriptor '"_s, m_def->name, "' for '"_s, owner->nameInMessages(), "' objects doesn't apply to a '"_s, typeName(globalObject, self), "' object"_s);
    }
    RELEASE_AND_RETURN(scope, invokeNativeImpl(globalObject, *m_def, this, self, args.withoutFirst()));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
