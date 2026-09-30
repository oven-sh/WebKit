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


#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"

// The module _operator: Modules/_operator.c of CPython. There is the same in Python, in operator.py, for where there is no _operator, but a function that is written in Python is a method of what it is an attribute of, and one
// that is built in is not: `concat_path = operator.add` in a class, which glob.py has, is only right of the latter.

namespace JSC { namespace Python {

namespace {

struct OperatorModuleState final : NativeState {
    PYTHON_NATIVE_STATE(OperatorModuleState);
    WriteBarrier<PyType> itemGetter;
    WriteBarrier<PyType> attributeGetter;
    WriteBarrier<PyType> methodCaller;
};

template<typename Visitor>
void OperatorModuleState::visit(Visitor& visitor)
{
    visitor.append(itemGetter);
    visitor.append(attributeGetter);
    visitor.append(methodCaller);
}

OperatorModuleState& operatorModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<OperatorModuleState>(); }

} // anonymous namespace

// ---- What stands for an operator

PYTHON_NATIVE(operatorBinary)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    auto inPlace = unpack<bool>(callFrame, 1);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, op, inPlace, args[0], args[1])));
}

PYTHON_NATIVE(operatorUnary)
{
    auto op = unpack<UnaryOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(unaryOperation(globalObject, op, args[0])));
}

PYTHON_NATIVE(operatorCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, args[0], args[1])));
}

PYTHON_NATIVE(operatorAbs)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(absolute(globalObject, args[0])));
}

// truth(a) and not_(a)
PYTHON_NATIVE(operatorTruth)
{
    auto isNegated = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    bool truth = isTrue(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(truth != isNegated));
}

PYTHON_NATIVE(operatorIndex)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(toInt(globalObject, args[0])));
}

// is_(a, b) and is_not(a, b)
PYTHON_NATIVE(operatorIs)
{
    auto isNegated = unpack<bool>(callFrame, 0);
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsBoolean(isIdentical(args[0], args[1]) != isNegated));
}

// is_none(a) and is_not_none(a)
PYTHON_NATIVE(operatorIsNone)
{
    auto isNegated = unpack<bool>(callFrame, 0);
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsBoolean(isNone(args[0]) != isNegated));
}

// ---- Sequences

// concat(a, b) and iconcat(a, b)
PYTHON_NATIVE(operatorConcat)
{
    auto inPlace = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(sequenceConcatenate(globalObject, inPlace, args[0], args[1])));
}

PYTHON_NATIVE(operatorContains)
{
    NATIVE_PROLOGUE();
    bool found = contains(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(found));
}

// indexOf(a, b) and countOf(a, b): _PySequence_IterSearch() of Objects/abstract.c
PYTHON_NATIVE(operatorSearch)
{
    auto isCount = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue iterator = getIterator(globalObject, args[0]);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::TypeError))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("argument of type '"_s, typeName(globalObject, args[0]), "' is not iterable"_s)));
        return { };
    }
    int64_t count = 0;
    for (int64_t index = 0;; ++index) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        bool isSame = isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isSame)
            continue;
        if (!isCount)
            return JSValue::encode(intFromInt64(globalObject, index));
        ++count;
    }
    if (!isCount)
        return JSValue::encode(raiseValueError(globalObject, scope, "sequence.index(x): x not in sequence"_s));
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(operatorGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, args[0], args[1])));
}

PYTHON_NATIVE(operatorSetItem)
{
    NATIVE_PROLOGUE();
    setItem(globalObject, args[0], args[1], args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(operatorDeleteItem)
{
    NATIVE_PROLOGUE();
    deleteItem(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// length_hint(obj, default=0, /)
PYTHON_NATIVE(operatorLengthHint)
{
    NATIVE_PROLOGUE();
    int64_t defaultValue = 0;
    if (args.size() > 1) {
        auto given = toSsize(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        defaultValue = *given;
    }
    auto hint = lengthHint(globalObject, args[0], defaultValue);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, *hint));
}

// ---- _compare_digest(a, b)

// _tscmp(): how long it takes is to depend on how long `b` is and on nothing that is in either. What is volatile is so that the compiler has no chance to find a shorter way.
static bool compareInConstantTime(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    volatile size_t length = b.size();
    volatile const uint8_t* left = nullptr;
    volatile const uint8_t* right = b.data();
    volatile uint8_t result = 0;
    // No `else`, so that as much is done either way.
    if (a.size() == length) {
        left = a.data();
        result = 0;
    }
    if (a.size() != length) {
        left = b.data();
        result = 1;
    }
    for (size_t i = 0; i < length; ++i)
        result = result | (*left++ ^ *right++);
    return !result;
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

// _hashlib has it too, under another name.
PYTHON_SHARED_NATIVE(operatorCompareDigest)
{
    NATIVE_PROLOGUE();
    JSString* a = stringIn(args[0]);
    JSString* b = stringIn(args[1]);
    if (a && b) {
        String left = a->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        String right = b->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!left.containsOnlyASCII() || !right.containsOnlyASCII())
            return JSValue::encode(raiseTypeError(globalObject, scope, "comparing strings with non-ASCII characters is not supported"_s));
        // What has only such characters in it may all the same have two bytes for each, if JavaScript made it.
        CString leftBytes = left.ascii();
        CString rightBytes = right.ascii();
        return JSValue::encode(jsBoolean(compareInConstantTime(byteCast<uint8_t>(leftBytes.span()), byteCast<uint8_t>(rightBytes.span()))));
    }
    if (!hasBuffer(globalObject, args[0]) && !hasBuffer(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("unsupported operand types(s) or combination of types: '"_s, typeName(globalObject, args[0]), "' and '"_s, typeName(globalObject, args[1]), '\'')));
    auto left = bufferOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto right = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(compareInConstantTime(*left, *right)));
}

// call(obj, /, *args, **kwargs)
PYTHON_NATIVE(operatorCall)
{
    NATIVE_PROLOGUE();
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, "call expected at least 1 argument, got 0"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, args[0], args.allFrom(1), args.keywordNames())));
}

// ---- What the three classes have in common

namespace {

// _PyArg_NoKeywords() and _PyArg_CheckPositional(name, count, 1, 1), of what one of them is called with. False if it raised.
bool checkOneArgument(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral name)
{
    if (!args.checkNoKeywords(globalObject, scope, name))
        return false;
    if (args.size() == 2)
        return true;
    raiseTypeError(globalObject, scope, concatenate(name, " expected 1 argument, got "_s, args.size() - 1));
    return false;
}

JSValue textSignature(JSGlobalObject* globalObject, JSValue) { return jsNontrivialString(globalObject->vm(), "(obj, /)"_s); }

// ---- itemgetter

struct ItemGetter final : NativeState {
    PYTHON_NATIVE_STATE(ItemGetter);
    // What to get, or a tuple of them if there is more than one.
    WriteBarrier<Unknown> item;
    unsigned count { 0 };
};

template<typename Visitor> void ItemGetter::visit(Visitor& visitor) { visitor.append(item); }

} // anonymous namespace

PYTHON_NATIVE(itemGetterNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "itemgetter"_s))
        return { };
    unsigned count = args.size() - 1;
    if (!count)
        return JSValue::encode(raiseTypeError(globalObject, scope, "itemgetter expected 1 argument, got 0"_s));
    JSValue item = args[1];
    if (count > 1) {
        PyTuple* items = PyTuple::create(globalObject, count);
        for (unsigned i = 0; i < count; ++i)
            items->initializeAt(vm, i, args[i + 1]);
        item = items;
    }
    // Of the class itself, whatever class it was asked for.
    auto* object = PyStateObject::create(vm, operatorModuleState(globalObject).itemGetter->instanceStructure(), makeUnique<ItemGetter>());
    auto& state = object->state<ItemGetter>();
    state.item.set(vm, object, item);
    state.count = count;
    return JSValue::encode(object);
}

PYTHON_NATIVE(itemGetterCall)
{
    NATIVE_PROLOGUE();
    if (!checkOneArgument(globalObject, scope, args, "itemgetter"_s))
        return { };
    auto& state = stateOf<ItemGetter>(args[0]);
    JSValue object = args[1];
    if (state.count == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, object, state.item.get())));
    PyTuple* items = asTuple(state.item.get());
    MarkedArgumentBuffer values;
    for (unsigned i = 0; i < state.count; ++i) {
        values.append(getItem(globalObject, object, items->at(i)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(PyTuple::createFromArguments(globalObject, values));
}

PYTHON_NATIVE(itemGetterRepr)
{
    NATIVE_PROLOGUE();
    String name = typeOf(globalObject, args[0])->nameString(globalObject);
    ReprGuard guard(globalObject, args[0].asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsString(vm, concatenate(name, "(...)"_s)));
    auto& state = stateOf<ItemGetter>(args[0]);
    String shown = repr(globalObject, state.item.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, state.count == 1 ? concatenate(name, '(', shown, ')') : concatenate(name, shown))));
}

PYTHON_NATIVE(itemGetterReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& state = stateOf<ItemGetter>(args[0]);
    JSValue arguments = state.count == 1 ? JSValue(PyTuple::create(globalObject, { state.item.get() })) : state.item.get();
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), arguments }));
}

// ---- attrgetter

namespace {

struct AttributeGetter final : NativeState {
    PYTHON_NATIVE_STATE(AttributeGetter);
    // One for each thing to get: its name, or if there are dots in that, a tuple of the names between them.
    WriteBarrier<PyTuple> attributes;
};

template<typename Visitor> void AttributeGetter::visit(Visitor& visitor) { visitor.append(attributes); }

JSValue getAttributeNamed(JSGlobalObject* globalObject, JSValue object, JSValue name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Identifier identifier = stringIn(name)->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, object, identifier));
}

// dotted_getattr()
JSValue getDottedAttribute(JSGlobalObject* globalObject, JSValue object, JSValue attribute)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isTuple(attribute))
        RELEASE_AND_RETURN(scope, getAttributeNamed(globalObject, object, attribute));
    for (auto& name : asTuple(attribute)->span()) {
        object = getAttributeNamed(globalObject, object, name.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    return object;
}

// dotjoinattr()
JSValue joinWithDots(JSGlobalObject* globalObject, JSValue attribute)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isTuple(attribute))
        return attribute;
    TextBuilder builder;
    bool isFirst = true;
    for (auto& name : asTuple(attribute)->span()) {
        if (!isFirst)
            builder.append('.');
        isFirst = false;
        String text = asString(name.get())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(text);
    }
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, builder.tryFinish()));
}

// attrgetter_args(): what it was made from. Null if it raised.
PyTuple* argumentsOf(JSGlobalObject* globalObject, AttributeGetter& state)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer names;
    for (auto& attribute : state.attributes->span()) {
        names.append(joinWithDots(globalObject, attribute.get()));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return PyTuple::createFromArguments(globalObject, names);
}

} // anonymous namespace

PYTHON_NATIVE(attributeGetterNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "attrgetter"_s))
        return { };
    unsigned count = args.size() - 1;
    if (!count)
        return JSValue::encode(raiseTypeError(globalObject, scope, "attrgetter expected 1 argument, got 0"_s));
    MarkedArgumentBuffer attributes;
    for (unsigned i = 0; i < count; ++i) {
        JSString* item = stringIn(args[i + 1]);
        if (!item)
            return JSValue::encode(raiseTypeError(globalObject, scope, "attribute name must be a string"_s));
        String text = item->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!text.contains('.')) {
            attributes.append(args[i + 1]);
            continue;
        }
        MarkedArgumentBuffer chain;
        for (auto part : StringView(text).splitAllowingEmptyEntries('.'))
            chain.append(jsString(vm, part.toString()));
        attributes.append(PyTuple::createFromArguments(globalObject, chain));
    }
    auto* object = PyStateObject::create(vm, operatorModuleState(globalObject).attributeGetter->instanceStructure(), makeUnique<AttributeGetter>());
    object->state<AttributeGetter>().attributes.set(vm, object, PyTuple::createFromArguments(globalObject, attributes));
    return JSValue::encode(object);
}

PYTHON_NATIVE(attributeGetterCall)
{
    NATIVE_PROLOGUE();
    if (!checkOneArgument(globalObject, scope, args, "attrgetter"_s))
        return { };
    PyTuple* attributes = stateOf<AttributeGetter>(args[0]).attributes.get();
    if (attributes->length() == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(getDottedAttribute(globalObject, args[1], attributes->at(0))));
    MarkedArgumentBuffer values;
    for (auto& attribute : attributes->span()) {
        values.append(getDottedAttribute(globalObject, args[1], attribute.get()));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(PyTuple::createFromArguments(globalObject, values));
}

PYTHON_NATIVE(attributeGetterRepr)
{
    NATIVE_PROLOGUE();
    String name = typeOf(globalObject, args[0])->nameString(globalObject);
    ReprGuard guard(globalObject, args[0].asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsString(vm, concatenate(name, "(...)"_s)));
    auto& state = stateOf<AttributeGetter>(args[0]);
    PyTuple* arguments = argumentsOf(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, arguments->length() == 1 ? arguments->at(0) : JSValue(arguments));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, arguments->length() == 1 ? concatenate(name, '(', shown, ')') : concatenate(name, shown))));
}

PYTHON_NATIVE(attributeGetterReduce)
{
    NATIVE_PROLOGUE();
    PyTuple* arguments = argumentsOf(globalObject, stateOf<AttributeGetter>(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), arguments }));
}

// ---- methodcaller

namespace {

struct MethodCaller final : NativeState {
    PYTHON_NATIVE_STATE(MethodCaller);
    WriteBarrier<Unknown> name;
    WriteBarrier<PyTuple> arguments;
    // Null if there were none.
    WriteBarrier<PyDict> keywords;
};

template<typename Visitor>
void MethodCaller::visit(Visitor& visitor)
{
    visitor.append(name);
    visitor.append(arguments);
    visitor.append(keywords);
}

} // anonymous namespace

PYTHON_NATIVE(methodCallerNew)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "methodcaller needs at least one argument, the method name"_s));
    if (!stringIn(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "method name must be a string"_s));
    PyTuple* arguments = PyTuple::create(globalObject, args.size() - 2);
    for (unsigned i = 2; i < args.size(); ++i)
        arguments->initializeAt(vm, i - 2, args[i]);
    PyDict* keywords = nullptr;
    if (args.keywordCount()) {
        keywords = PyDict::create(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            keywords->set(globalObject, args.keywordName(i), args.keywordValue(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    auto* object = PyStateObject::create(vm, operatorModuleState(globalObject).methodCaller->instanceStructure(), makeUnique<MethodCaller>());
    auto& state = object->state<MethodCaller>();
    state.name.set(vm, object, args[1]);
    state.arguments.set(vm, object, arguments);
    state.keywords.setMayBeNull(vm, object, keywords);
    return JSValue::encode(object);
}

PYTHON_NATIVE(methodCallerCall)
{
    NATIVE_PROLOGUE();
    // Which of the two is looked at first depends on how it is called, which depends on how much it was given: methodcaller_vectorcall() and methodcaller_call().
    auto& state = stateOf<MethodCaller>(args[0]);
    constexpr unsigned maximumForVectorcall = 8;
    bool countIsFirst = 1 + state.arguments->length() + (state.keywords ? state.keywords->size() : 0) < maximumForVectorcall;
    if (countIsFirst && args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("methodcaller expected 1 argument, got "_s, args.size() - 1)));
    if (!checkOneArgument(globalObject, scope, args, "methodcaller"_s))
        return { };
    JSValue method = getAttributeNamed(globalObject, args[1], state.name.get());
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (auto& argument : state.arguments->span())
        arguments.append(argument.get());
    if (!state.keywords)
        RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, method, arguments)));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywordDict(globalObject, method, arguments, state.keywords.get())));
}

PYTHON_NATIVE(methodCallerRepr)
{
    NATIVE_PROLOGUE();
    String name = typeOf(globalObject, args[0])->nameString(globalObject);
    ReprGuard guard(globalObject, args[0].asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsString(vm, concatenate(name, "(...)"_s)));
    auto& state = stateOf<MethodCaller>(args[0]);
    TextBuilder builder;
    String shownName = repr(globalObject, state.name.get());
    RETURN_IF_EXCEPTION(scope, { });
    builder.append(name, '(', shownName);
    for (auto& argument : state.arguments->span()) {
        String shown = repr(globalObject, argument.get());
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(", "_s, shown);
    }
    if (state.keywords) {
        MarkedArgumentBuffer pairs;
        state.keywords->forEach(globalObject, [&] (JSValue key, JSValue value) {
            pairs.append(key);
            pairs.append(value);
            return true;
        });
        for (size_t i = 0; i < pairs.size(); i += 2) {
            String key = asString(pairs.at(i))->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            String shown = repr(globalObject, pairs.at(i + 1));
            RETURN_IF_EXCEPTION(scope, { });
            builder.append(", "_s, key, '=', shown);
        }
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, builder.tryFinish())));
}

PYTHON_NATIVE(methodCallerReduce)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<MethodCaller>(args[0]);
    JSValue type = typeOf(globalObject, args[0])->object();
    if (!state.keywords || !state.keywords->size()) {
        MarkedArgumentBuffer arguments;
        arguments.append(state.name.get());
        for (auto& argument : state.arguments->span())
            arguments.append(argument.get());
        return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::createFromArguments(globalObject, arguments) }));
    }
    // There is no saying what keywords a class is to be called with, so what is to be called is something that has them already.
    JSValue partial = importModuleAttribute(globalObject, "functools"_s, "partial"_s);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(type);
    arguments.append(state.name.get());
    JSValue constructor = callWithKeywordDict(globalObject, partial, arguments, state.keywords.get());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { constructor, state.arguments.get() }));
}

// ---- The module

JSObject* createOperatorModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = operatorModuleState(globalObject);
    if (!state.itemGetter) {
        auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, NativeFunction create, NativeFunction call, NativeFunction show, NativeFunction reduce) {
            PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, 0);
            type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
            slot.set(vm, realm, type);
            addMethods(globalObject, type, {
                { "__new__"_s, create, Kind::New, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
                { "__call__"_s, call, Kind::Wrapper, 0, "($self, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
                { "__repr__"_s, show, Kind::Wrapper },
                { "__reduce__"_s, reduce },
            });
            addGenericGetAttribute(globalObject, type);
            addGetSet(globalObject, type, "__text_signature__"_s, textSignature);
            // In CPython this tells the class where in the struct the pointer is by which an instance is called. It is there for a program to see as well, and what it gives of an instance is that pointer, as a number. There
            // is none here.
            addMember(globalObject, type, "__vectorcalloffset__"_s, [] (JSGlobalObject*, JSValue) -> JSValue { return jsNumber(0); });
        };
        make(state.attributeGetter, "operator.attrgetter"_s, attributeGetterNew, attributeGetterCall, attributeGetterRepr, attributeGetterReduce);
        make(state.itemGetter, "operator.itemgetter"_s, itemGetterNew, itemGetterCall, itemGetterRepr, itemGetterReduce);
        make(state.methodCaller, "operator.methodcaller"_s, methodCallerNew, methodCallerCall, methodCallerRepr, methodCallerReduce);
    }

    JSObject* module = newBuiltinModule(globalObject, "_operator"_s);
    addFunction(globalObject, module, "truth"_s, operatorTruth, pack(false));
    addFunction(globalObject, module, "contains"_s, operatorContains);
    addFunction(globalObject, module, "indexOf"_s, operatorSearch, pack(false));
    addFunction(globalObject, module, "countOf"_s, operatorSearch, pack(true));
    addFunction(globalObject, module, "is_"_s, operatorIs, pack(false));
    addFunction(globalObject, module, "is_not"_s, operatorIs, pack(true));
    addFunction(globalObject, module, "is_none"_s, operatorIsNone, pack(false));
    addFunction(globalObject, module, "is_not_none"_s, operatorIsNone, pack(true));
    addFunction(globalObject, module, "index"_s, operatorIndex);
    static constexpr std::tuple<ASCIILiteral, ASCIILiteral, BinaryOperator> binary[] = {
        { "add"_s, "iadd"_s, BinaryOperator::Add }, { "sub"_s, "isub"_s, BinaryOperator::Sub }, { "mul"_s, "imul"_s, BinaryOperator::Mult }, { "matmul"_s, "imatmul"_s, BinaryOperator::MatMult },
        { "floordiv"_s, "ifloordiv"_s, BinaryOperator::FloorDiv }, { "truediv"_s, "itruediv"_s, BinaryOperator::Div }, { "mod"_s, "imod"_s, BinaryOperator::Mod }, { "lshift"_s, "ilshift"_s, BinaryOperator::LShift },
        { "rshift"_s, "irshift"_s, BinaryOperator::RShift }, { "and_"_s, "iand"_s, BinaryOperator::BitAnd }, { "xor"_s, "ixor"_s, BinaryOperator::BitXor }, { "or_"_s, "ior"_s, BinaryOperator::BitOr },
        { "pow"_s, "ipow"_s, BinaryOperator::Pow },
    };
    for (auto& [name, inPlaceName, op] : binary) {
        addFunction(globalObject, module, name, operatorBinary, pack(op, false));
        addFunction(globalObject, module, inPlaceName, operatorBinary, pack(op, true));
    }
    addFunction(globalObject, module, "neg"_s, operatorUnary, pack(UnaryOperator::USub));
    addFunction(globalObject, module, "pos"_s, operatorUnary, pack(UnaryOperator::UAdd));
    addFunction(globalObject, module, "abs"_s, operatorAbs);
    addFunction(globalObject, module, "inv"_s, operatorUnary, pack(UnaryOperator::Invert));
    addFunction(globalObject, module, "invert"_s, operatorUnary, pack(UnaryOperator::Invert));
    addFunction(globalObject, module, "not_"_s, operatorTruth, pack(true));
    addFunction(globalObject, module, "concat"_s, operatorConcat, pack(false));
    addFunction(globalObject, module, "iconcat"_s, operatorConcat, pack(true));
    addFunction(globalObject, module, "getitem"_s, operatorGetItem);
    addFunction(globalObject, module, "setitem"_s, operatorSetItem);
    addFunction(globalObject, module, "delitem"_s, operatorDeleteItem);
    static constexpr std::pair<ASCIILiteral, ComparisonOperator> comparisons[] = {
        { "eq"_s, ComparisonOperator::Eq }, { "ne"_s, ComparisonOperator::NotEq }, { "lt"_s, ComparisonOperator::Lt }, { "le"_s, ComparisonOperator::LtE }, { "gt"_s, ComparisonOperator::Gt }, { "ge"_s, ComparisonOperator::GtE },
    };
    for (auto& [name, op] : comparisons)
        addFunction(globalObject, module, name, operatorCompare, pack(op));
    addFunction(globalObject, module, "_compare_digest"_s, operatorCompareDigest);
    addFunction(globalObject, module, "length_hint"_s, operatorLengthHint);
    addFunction(globalObject, module, "call"_s, operatorCall, 0, { }, Arguments::AreNotChecked);
    module->putDirect(vm, Identifier::fromString(vm, "attrgetter"_s), state.attributeGetter->object());
    module->putDirect(vm, Identifier::fromString(vm, "itemgetter"_s), state.itemGetter->object());
    module->putDirect(vm, Identifier::fromString(vm, "methodcaller"_s), state.methodCaller->object());
    return module;
}

} } // namespace JSC::Python
