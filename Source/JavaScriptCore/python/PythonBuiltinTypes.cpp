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

#include "FunctionExecutable.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSGenerator.h"
#include "PythonGenerators.h"
#include "PythonIO.h"
#include "UnlinkedFunctionExecutable.h"

// object, type, and the types of functions, methods, descriptors, modules and generators.

namespace JSC { namespace Python {

void addMethods(JSGlobalObject* globalObject, PyType* type, std::initializer_list<MethodDefinition> methods)
{
    VM& vm = globalObject->vm();
    for (auto& method : methods) {
        JSValue value = PyNativeFunction::create(vm, globalObject, 0, String(method.name), method.function, method.kind, type, method.data, ImplementationVisibility::Public, method.signature, method.arguments);
        if (method.kind == PyNativeFunction::Kind::Function) {
            auto* wrapper = PyNativeObject::create(globalObject, BuiltinType::StaticMethod);
            wrapper->setField(vm, 0, value);
            value = wrapper;
        }
        type->putDirect(vm, Identifier::fromString(vm, method.name), value);
    }
}

void addMethodsThatCPythonHas(JSGlobalObject* globalObject, PyType* type, std::initializer_list<MethodDefinition> methods)
{
    String typeName = type->nameWithoutModule(globalObject);
    for (auto& method : methods) {
        if (findAttributeDescription(typeName, method.name))
            addMethods(globalObject, type, { method });
    }
}

void addGetSet(JSGlobalObject* globalObject, PyType* type, ASCIILiteral name, PyGetSetDescriptor::Getter getter, PyGetSetDescriptor::Setter setter)
{
    VM& vm = globalObject->vm();
    type->putDirect(vm, Identifier::fromString(vm, name), PyGetSetDescriptor::create(globalObject, type, String(name), getter, setter));
}

void addMember(JSGlobalObject* globalObject, PyType* type, ASCIILiteral name, PyGetSetDescriptor::Getter getter, PyGetSetDescriptor::Setter setter)
{
    VM& vm = globalObject->vm();
    type->putDirect(vm, Identifier::fromString(vm, name), PyGetSetDescriptor::create(globalObject, type, String(name), getter, setter, true));
}

PyNativeFunction* addFunction(JSGlobalObject* globalObject, JSObject* namespaceObject, ASCIILiteral name, NativeFunction function, unsigned data, ASCIILiteral signature, PyNativeFunction::Arguments arguments)
{
    VM& vm = globalObject->vm();
    auto* native = PyNativeFunction::create(vm, globalObject, 0, String(name), function, PyNativeFunction::Kind::Function, namespaceObject, data, ImplementationVisibility::Public, signature, arguments);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, name), native);
    return native;
}

JSValue boxIfDerived(JSGlobalObject* globalObject, PyType* type, PyType* builtin, JSValue value)
{
    if (type == builtin)
        return value;
    return PyBoxedValue::create(globalObject->vm(), type->instanceStructure(), value);
}

bool checkArguments(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    auto* function = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee());
    // How many there are is enough to go by, unless some were given by name.
    auto checkAgainstSignature = [&] {
        if (function->takes(callFrame->argumentCount()) && !isKeywordNames(callFrame->thisValue())) [[likely]]
            return true;
        return !function->checksArguments() || checkArgumentsSlow(globalObject, callFrame);
    };
    PyNativeFunction::Kind kind = function->kind();
    if (!function->hasImplicitFirst())
        return checkAgainstSignature();
    auto* owner = uncheckedDowncast<PyType>(function->owner());
    NativeArguments args(callFrame);
    JSValue first = args.size() ? args[0] : JSValue();
    if (kind == PyNativeFunction::Kind::Method || kind == PyNativeFunction::Kind::Wrapper) {
        if (first) [[likely]] {
            PyType* type = typeOf(globalObject, first);
            if (type == owner || type->isSubtypeOf(owner)) [[likely]]
                return checkAgainstSignature();
            // A class of JavaScript's is a function of JavaScript's besides.
            if (owner == globalObject->pyRealm()->typeJSFunction() && first.isCell() && isJavaScriptClass(first.asCell()))
                return checkAgainstSignature();
        }
    } else if (first && isClass(first) && asType(first)->isSubtypeOf(owner))
        return checkAgainstSignature();

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String ownerName = owner->nameString(globalObject);
    String name = function->name(vm);
    switch (kind) {
    case PyNativeFunction::Kind::Wrapper:
        if (!first) {
            raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' of '"_s, ownerName, "' object needs an argument"_s));
            break;
        }
        raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' requires a '"_s, ownerName, "' object but received a '"_s, typeName(globalObject, first), '\''));
        break;
    case PyNativeFunction::Kind::Method:
        if (!first)
            raiseTypeError(globalObject, scope, concatenate("unbound method "_s, ownerName, '.', name, "() needs an argument"_s));
        else
            raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' for '"_s, ownerName, "' objects doesn't apply to a '"_s, typeName(globalObject, first), "' object"_s));
        break;
    case PyNativeFunction::Kind::ClassMethod:
        if (!first)
            raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' of '"_s, ownerName, "' object needs an argument"_s));
        else if (!isClass(first))
            raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' for type '"_s, ownerName, "' needs a type, not a '"_s, typeName(globalObject, first), "' as arg 2"_s));
        else
            raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name, "' requires a subtype of '"_s, ownerName, "' but received '"_s, asType(first)->nameString(globalObject), '\''));
        break;
    case PyNativeFunction::Kind::New:
        if (!first)
            raiseTypeError(globalObject, scope, concatenate(ownerName, ".__new__(): not enough arguments"_s));
        else if (!isClass(first))
            raiseTypeError(globalObject, scope, concatenate(ownerName, ".__new__(X): X is not a type object ("_s, typeName(globalObject, first), ')'));
        else
            raiseTypeError(globalObject, scope, concatenate(ownerName, ".__new__("_s, asType(first)->nameString(globalObject), "): "_s, asType(first)->nameString(globalObject), " is not a subtype of "_s, ownerName));
        break;
    case PyNativeFunction::Kind::Function:
    case PyNativeFunction::Kind::StaticMethod:
        break;
    }
    return false;
}

// ---- What is the same for every built-in type

PYTHON_SHARED_NATIVE(nativeRepr)
{
    NATIVE_PROLOGUE();
    String text = builtinRepr(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_SHARED_NATIVE(nativeHash)
{
    NATIVE_PROLOGUE();
    int64_t hash = builtinHash(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, hash)));
}

PYTHON_SHARED_NATIVE(nativeLen)
{
    NATIVE_PROLOGUE();
    int64_t length = builtinLength(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, length)));
}

PYTHON_SHARED_NATIVE(nativeGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(builtinGetItem(globalObject, args[0], args[1])));
}

PYTHON_SHARED_NATIVE(nativeSetItem)
{
    NATIVE_PROLOGUE();
    scope.release();
    builtinSetItem(globalObject, args[0], args[1], args[2]);
    RETURN_NONE();
}

PYTHON_SHARED_NATIVE(nativeDelItem)
{
    NATIVE_PROLOGUE();
    scope.release();
    builtinSetItem(globalObject, args[0], args[1], JSValue());
    RETURN_NONE();
}

PYTHON_SHARED_NATIVE(nativeContains)
{
    NATIVE_PROLOGUE();
    auto result = builtinContains(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result.value_or(false)));
}

PYTHON_SHARED_NATIVE(nativeIter)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(builtinGetIterator(globalObject, args.at(0)));
}

PYTHON_SHARED_NATIVE(nativeNext)
{
    NATIVE_PROLOGUE();
    // This is the built-in class's own, whatever a class derived from it may have.
    auto* native = tryIterator(args[0]);
    JSValue value = native ? native->next(globalObject) : iteratorNext(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(value);
}

PYTHON_SHARED_NATIVE(nativeSelf)
{
    UNUSED_PARAM(globalObject);
    return JSValue::encode(callFrame->argument(0));
}

PYTHON_NATIVE(nativeCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue result = builtinCompare(globalObject, op, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(result);
}

void addComparisons(JSGlobalObject* globalObject, PyType* type, NativeFunction function)
{
    // A class that compares in any way has them all, as in CPython, where they are one function. Those that mean nothing to it say NotImplemented.
    addMethods(globalObject, type, {
        { "__eq__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Eq) },
        { "__ne__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::NotEq) },
        { "__lt__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Lt) },
        { "__le__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::LtE) },
        { "__gt__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Gt) },
        { "__ge__"_s, function, PyNativeFunction::Kind::Method, pack(ComparisonOperator::GtE) },
    });
}

void addComparisons(JSGlobalObject* globalObject, PyType* type)
{
    addComparisons(globalObject, type, nativeCompare);
}

enum class Form : uint8_t { Plain, Reflected, InPlace };

PYTHON_NATIVE(nativeBinary)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    auto form = unpack<Form>(callFrame, 1);
    NATIVE_PROLOGUE();
    if (args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected 1 argument, got "_s, args.size() ? args.size() - 1 : 0)));
    JSValue left = form == Form::Reflected ? args[1] : args[0];
    JSValue right = form == Form::Reflected ? args[0] : args[1];
    JSValue result = builtinBinaryOperation(globalObject, op, form == Form::InPlace, left, right);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(result);
}

// A str that is of a class derived from str is kept in something else.
static JSValue withoutBox(JSValue value)
{
    if (auto* boxed = tryBoxedValue(value); boxed && boxed->value().isString())
        return boxed->value();
    return value;
}

// s + t and s += t, of a str, a list or a tuple: sq_concat and sq_inplace_concat. There is no asking anything else, so what will not do is an error.
PYTHON_NATIVE(nativeConcatenate)
{
    auto form = unpack<Form>(callFrame, 1);
    NATIVE_PROLOGUE();
    JSValue result = builtinBinaryOperation(globalObject, BinaryOperator::Add, form == Form::InPlace, withoutBox(args[0]), withoutBox(args[1]));
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return JSValue::encode(result);
    ASCIILiteral type = isList(args[0]) ? "list"_s : isTuple(args[0]) ? "tuple"_s : "str"_s;
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("can only concatenate "_s, type, " (not \""_s, typeName(globalObject, args[1]), "\") to "_s, type)));
}

// s * n, n * s and s *= n: sq_repeat and sq_inplace_repeat
PYTHON_NATIVE(nativeRepeat)
{
    auto form = unpack<Form>(callFrame, 1);
    NATIVE_PROLOGUE();
    auto count = toIndexOrOverflow(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(builtinBinaryOperation(globalObject, BinaryOperator::Mult, form == Form::InPlace, withoutBox(args[0]), intFromInt64(globalObject, *count))));
}

void addBinaryOperators(JSGlobalObject* globalObject, PyType* type, std::initializer_list<BinaryOperator> operators, bool reflected, bool inPlace)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    bool isSequence = type == realm->typeStr() || type == realm->typeList() || type == realm->typeTuple();
    auto add = [&] (const Identifier& name, BinaryOperator op, Form form) {
        NativeFunction function = isSequence && op == BinaryOperator::Add ? nativeConcatenate : isSequence && op == BinaryOperator::Mult ? nativeRepeat : nativeBinary;
        type->putDirect(vm, name, PyNativeFunction::create(vm, globalObject, 1, name.string(), function, PyNativeFunction::Kind::Method, type, pack(op, form)));
    };
    for (BinaryOperator op : operators) {
        add(names.method(op), op, Form::Plain);
        if (reflected)
            add(names.reflectedMethod(op), op, Form::Reflected);
        if (inPlace)
            add(names.inPlaceMethod(op), op, Form::InPlace);
    }
}

// ---- object

static bool hasExcessArguments(const NativeArguments& args)
{
    return args.size() > 1 || args.keywordCount();
}

PYTHON_NATIVE(objectNew)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "object.__new__(X): X is not a type object"_s));
    auto* type = asType(args[0]);
    // tp_new_wrapper(), which comes before anything that object_new() has to say.
    PyType::Allocator allocator = nullptr;
    if (type->layout() != PyType::Layout::Object) {
        PyType* builtin = type;
        while (builtin->hasFlag(PyType::IsHeapType))
            builtin = builtin->base();
        allocator = builtin->allocator();
    }
    if (type->layout() != PyType::Layout::Object && !allocator) {
        PyType* builtin = type;
        while (builtin->hasFlag(PyType::IsHeapType))
            builtin = builtin->base();
        // One of JavaScript's is made by JavaScript.
        if (!builtin->hasFlag(PyType::IsJavaScript) && builtin->cannotBeInstantiated(vm))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot create '"_s, type->nameString(globalObject), "' instances"_s)));
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("object.__new__("_s, type->nameString(globalObject), ") is not safe, use "_s, builtin->nameString(globalObject), ".__new__()"_s)));
    }
    if (hasExcessArguments(args)) {
        if (type->lookup(vm, names.dunder_new).asCell() != realm->function(PyRealm::WellKnownFunction::ObjectNew))
            return JSValue::encode(raiseTypeError(globalObject, scope, "object.__new__() takes exactly one argument (the type to instantiate)"_s));
        if (type->lookup(vm, names.dunder_init).asCell() == realm->function(PyRealm::WellKnownFunction::ObjectInit))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), "() takes no arguments"_s)));
    }
    if (type->hasFlag(PyType::IsAbstract)) {
        JSValue methods = type->lookupOwn(vm, names.dunder_abstractmethods);
        if (!methods)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, "__abstractmethods__"_s));
        MarkedArgumentBuffer unsorted;
        collect(globalObject, methods, unsorted);
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer sorted;
        sortValues(globalObject, unsorted, JSValue(), false, sorted);
        RETURN_IF_EXCEPTION(scope, { });
        TextBuilder joined;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (!sorted.at(i).isString())
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("sequence item "_s, i, ": expected str instance, "_s, typeName(globalObject, sorted.at(i)), " found"_s)));
            joined.append(i ? "', '"_s : ""_s, asString(sorted.at(i))->value(globalObject).data);
        }
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Can't instantiate abstract class "_s, type->nameString(globalObject), " without an implementation for abstract method"_s, sorted.size() > 1 ? "s"_s : ""_s, " '"_s, joined.tryFinish(), '\'')));
    }
    if (allocator)
        return JSValue::encode(allocator(vm, type->instanceStructure()));
    return JSValue::encode(PyInstance::create(vm, type->instanceStructure()));
}

PYTHON_NATIVE(notImplementedBool)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, "NotImplemented should not be used in a boolean context"_s));
}

// NoneType(), type(NotImplemented)() and type(...)(): the one that there is.
PYTHON_NATIVE(singletonNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    if (hasExcessArguments(args))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type == realm->typeEllipsis() ? "EllipsisType"_str : type->nameString(globalObject), " takes no arguments"_s)));
    if (type == realm->typeNotImplementedType())
        return JSValue::encode(realm->notImplemented());
    return JSValue::encode(type == realm->typeEllipsis() ? JSValue(realm->ellipsis()) : jsUndefined());
}

PYTHON_NATIVE(objectInit)
{
    NATIVE_PROLOGUE();
    if (hasExcessArguments(args)) {
        PyType* type = typeOf(globalObject, args[0]);
        if (type->lookup(vm, names.dunder_init).asCell() != realm->function(PyRealm::WellKnownFunction::ObjectInit))
            return JSValue::encode(raiseTypeError(globalObject, scope, "object.__init__() takes exactly one argument (the instance to initialize)"_s));
        if (!type->cannotBeInstantiated(vm) && type->lookup(vm, names.dunder_new).asCell() == realm->function(PyRealm::WellKnownFunction::ObjectNew))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), ".__init__() takes exactly one argument (the instance to initialize)"_s)));
    }
    RETURN_NONE();
}

PYTHON_NATIVE(objectRepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args.at(0);
    const void* address = self.isCell() ? static_cast<const void*>(self.asCell()) : std::bit_cast<const void*>(JSValue::encode(self));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', qualifiedNameOfType(globalObject, typeOf(globalObject, self)), " object at "_s, addressOf(address), '>'))));
}

PYTHON_NATIVE(objectStr)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(objectEq)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (isIdentical(args.at(0), args.at(1)))
        return JSValue::encode(jsBoolean(true));
    RETURN_NOT_IMPLEMENTED();
}

// By default, x != y is not x == y.
PYTHON_NATIVE(objectNe)
{
    NATIVE_PROLOGUE();
    PyType* type = typeOf(globalObject, args.at(0));
    JSValue result = callSpecial(globalObject, type, type->lookup(vm, names.dunder_eq), args.at(0), args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (result.isCell() && result.asCell() == realm->notImplemented())
        return JSValue::encode(result);
    bool truth = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(!truth));
}

PYTHON_NATIVE(returnFalse)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(false));
}

PYTHON_NATIVE(returnNotImplemented)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(globalObject->pyRealm()->notImplemented());
}

std::optional<Identifier> attributeName(JSGlobalObject* globalObject, ThrowScope& scope, JSValue name)
{
    JSString* string = stringIn(name);
    if (!string) {
        raiseTypeError(globalObject, scope, concatenate("attribute name must be string, not '"_s, typeName(globalObject, name), '\''));
        return std::nullopt;
    }
    return string->toIdentifier(globalObject);
}

// object.__getattribute__(x, name) is PyObject_GenericGetAttr(), whatever x is. A class is then an instance of its metaclass like any other, with what is in its own __dict__ for its attributes, as they are.
PYTHON_NATIVE(objectGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value;
    if (isClass(args[0])) {
        PyType* metatype = asType(args[0])->metatype();
        JSValue attribute = metatype->lookup(vm, *name);
        bool isData = attribute && isDataDescriptor(globalObject, attribute);
        // What a class that is built in has is not kept where this would look.
        if (!isData && asType(args[0])->hasFlag(PyType::IsHeapType))
            value = asType(args[0])->lookupOwn(vm, *name);
        if (!value && attribute) {
            value = bindDescriptor(globalObject, attribute, args[0], metatype);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else {
        value = genericGetAttribute(globalObject, args[0], *name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (value)
        return JSValue::encode(value);
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, args[0]), "' object has no attribute '"_s, name->string(), '\'')));
}

PYTHON_NATIVE(typeGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = genericGetAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    if (isClass(args[0]))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate("type object '"_s, asType(args[0])->nameString(globalObject), "' has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, args[0]), "' object has no attribute '"_s, name->string(), '\'')));
}

// hackcheck(): what is built in and has its own way of setting attributes is not to be got round with object's. That is classes.
static bool checkIsNotClass(JSGlobalObject* globalObject, ThrowScope& scope, JSValue self, ASCIILiteral what)
{
    if (!isClass(self))
        return true;
    raiseTypeError(globalObject, scope, concatenate("can't apply this "_s, what, " to "_s, typeName(globalObject, self), " object"_s));
    return false;
}

PYTHON_NATIVE(objectSetAttr)
{
    NATIVE_PROLOGUE();
    if (!checkIsNotClass(globalObject, scope, args[0], "__setattr__"_s))
        return { };
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    genericSetAttribute(globalObject, args[0], *name, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(objectDelAttr)
{
    NATIVE_PROLOGUE();
    if (!checkIsNotClass(globalObject, scope, args[0], "__delattr__"_s))
        return { };
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    genericSetAttribute(globalObject, args[0], *name, JSValue());
    RETURN_NONE();
}

PYTHON_NATIVE(typeSetAttr)
{
    NATIVE_PROLOGUE();
    if (asType(args[0])->isImmutable()) {
        String shown = repr(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot set "_s, shown, " attribute of immutable type '"_s, asType(args[0])->nameString(globalObject), '\'')));
    }
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    genericSetAttribute(globalObject, args[0], *name, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(typeDelAttr)
{
    NATIVE_PROLOGUE();
    if (asType(args[0])->isImmutable()) {
        String shown = repr(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot set "_s, shown, " attribute of immutable type '"_s, asType(args[0])->nameString(globalObject), '\'')));
    }
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    genericSetAttribute(globalObject, args[0], *name, JSValue());
    RETURN_NONE();
}

PYTHON_NATIVE(objectFormat)
{
    NATIVE_PROLOGUE();
    JSString* specification = stringIn(args[1]);
    if (!specification)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, args[1]))));
    if (specification->length())
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("unsupported format string passed to "_s, typeName(globalObject, args[0]), ".__format__"_s)));
    RELEASE_AND_RETURN(scope, JSValue::encode(strObject(globalObject, args[0])));
}

PYTHON_NATIVE(objectInitSubclass)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount()) {
        String name = args.size() && isClass(args[0]) ? asType(args[0])->nameString(globalObject) : "object"_str;
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(name, ".__init_subclass__() takes no keyword arguments"_s)));
    }
    RETURN_NONE();
}

// Adds the keys of the __dict__ of a class, and of those of its bases, and so on: merge_class_dict() of CPython's Objects/typeobject.c. It goes by
// what the attributes say, so that something that only makes itself out to be a class will do.
static bool mergeClassDict(JSGlobalObject* globalObject, PyDict* names, JSValue aClass)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue classDict = getAttributeIfPresent(globalObject, aClass, vm.pythonNames().dunder_dict);
    RETURN_IF_EXCEPTION(scope, false);
    if (classDict) {
        // PyDict_Update(). What a class has for its __dict__ is known, and only its keys are wanted. Anything else is asked for its keys(), and then for what it has for each.
        bool isKnown = isGoneThroughAsDict(globalObject, classDict) || typeOf(globalObject, classDict) == globalObject->pyRealm()->typeMappingProxy();
        JSValue keys = classDict;
        if (!isKnown) {
            JSValue method = getAttribute(globalObject, classDict, Identifier::fromString(vm, "keys"_s));
            RETURN_IF_EXCEPTION(scope, false);
            JSValue given = call(globalObject, method);
            RETURN_IF_EXCEPTION(scope, false);
            // method_output_as_list()
            keys = getIterator(globalObject, given);
            if (scope.exception()) {
                if (catchException(globalObject, BuiltinType::TypeError))
                    raiseTypeError(globalObject, scope, concatenate(typeName(globalObject, classDict), ".keys() returned a non-iterable (type "_s, typeName(globalObject, given), ')'));
                return false;
            }
        }
        MarkedArgumentBuffer each;
        collectAsList(globalObject, keys, each);
        RETURN_IF_EXCEPTION(scope, false);
        for (unsigned i = 0; i < each.size(); ++i) {
            if (!isKnown) {
                getItem(globalObject, classDict, each.at(i));
                RETURN_IF_EXCEPTION(scope, false);
            }
            names->set(globalObject, each.at(i), jsUndefined());
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    JSValue bases = getAttributeIfPresent(globalObject, aClass, vm.pythonNames().dunder_bases);
    RETURN_IF_EXCEPTION(scope, false);
    if (!bases)
        return true;
    // There is no saying that it is a tuple. It is asked how long it is, and then for each.
    int64_t count = length(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue base = getItem(globalObject, bases, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        if (!mergeClassDict(globalObject, names, base))
            return false;
    }
    return true;
}

static JSValue keysOf(JSGlobalObject* globalObject, PyDict* dict)
{
    MarkedArgumentBuffer keys;
    dict->forEach(globalObject, [&] (JSValue key, JSValue) {
        keys.append(key);
        return true;
    });
    return newList(globalObject, keys);
}

// object.__dir__(): what is in its __dict__, and in that of its class and the classes that that is derived from.
PYTHON_NATIVE(objectDir)
{
    NATIVE_PROLOGUE();
    PyDict* found = PyDict::create(globalObject);
    JSValue dict = getAttributeIfPresent(globalObject, args[0], names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, { });
    if (dict && isDict(dict)) {
        asDict(dict)->forEach(globalObject, [&] (JSValue key, JSValue) {
            found->set(globalObject, key, jsUndefined());
            return !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue itsClass = getAttributeIfPresent(globalObject, args[0], names.dunder_class);
    RETURN_IF_EXCEPTION(scope, { });
    if (itsClass && !mergeClassDict(globalObject, found, itsClass))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(keysOf(globalObject, found)));
}

// type.__dir__()
PYTHON_NATIVE(typeDir)
{
    NATIVE_PROLOGUE();
    PyDict* found = PyDict::create(globalObject);
    if (!mergeClassDict(globalObject, found, args[0]))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(keysOf(globalObject, found)));
}

// module.__dir__(): what its own __dir__() says, if it defines one (PEP 562), and otherwise what is in it.
PYTHON_NATIVE(moduleDir)
{
    NATIVE_PROLOGUE();
    JSValue dict = getAttribute(globalObject, args[0], names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(dict))
        return JSValue::encode(raiseTypeError(globalObject, scope, "<module>.__dict__ is not a dictionary"_s));
    if (JSValue function = asDict(dict)->getString(globalObject, "__dir__"_s))
        RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, function)));
    RELEASE_AND_RETURN(scope, JSValue::encode(keysOf(globalObject, asDict(dict))));
}

static JSValue getClass(JSGlobalObject* globalObject, JSValue self)
{
    return typeOf(globalObject, self)->object();
}

// type.__dict__
static JSValue getTypeDict(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = asType(self);
    if (!type->javaScriptConstructor())
        return PyNativeObject::create(globalObject, BuiltinType::MappingProxy, PyDict::backedBy(globalObject, type));
    // What a class of JavaScript's defines is in two objects, and is not all values. So this is how it is now, and does not keep up.
    PyDict* dict = PyDict::create(globalObject);
    auto add = [&] (JSObject* object, bool isConstructor) {
        PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        object->methodTable()->getOwnPropertyNames(object, globalObject, properties, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
        for (auto& property : properties) {
            if (property == vm.propertyNames->constructor || (isConstructor && (property == vm.propertyNames->prototype || property == vm.propertyNames->name || property == vm.propertyNames->length)))
                continue;
            JSValue value = object->getDirect(vm, property);
            if (!value)
                continue;
            // An accessor is what property() makes.
            if (auto* accessor = dynamicDowncast<GetterSetter>(value))
                value = PyNativeObject::create(globalObject, BuiltinType::Property, accessor->isGetterNull() ? jsUndefined() : JSValue(accessor->getter()), accessor->isSetterNull() ? jsUndefined() : JSValue(accessor->setter()), jsUndefined(), jsUndefined());
            else if (!value.isObject() && value.isCell() && !value.isString() && !value.isHeapBigInt() && !value.isSymbol())
                continue;
            dict->set(globalObject, jsString(vm, property.string()), value);
            RETURN_IF_EXCEPTION(scope, void());
        }
    };
    add(type, false);
    RETURN_IF_EXCEPTION(scope, { });
    add(type->javaScriptPrototype(), false);
    RETURN_IF_EXCEPTION(scope, { });
    add(type->javaScriptConstructor(), true);
    RETURN_IF_EXCEPTION(scope, { });
    return PyNativeObject::create(globalObject, BuiltinType::MappingProxy, dict);
}

// The order of resolution, as a program is to see it.
static PyTuple* orderOfResolution(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    PyTuple* order = type->mro();
    bool isAsKept = true;
    for (auto& entry : order->span())
        isAsKept &= !asType(entry.get())->javaScriptConstructor();
    if (isAsKept) [[likely]]
        return order;
    PyTuple* result = PyTuple::create(globalObject, order->length());
    for (unsigned i = 0; i < order->length(); ++i)
        result->initializeAt(vm, i, asType(order->at(i))->object());
    return result;
}

// Whether an instance of one class could as well be an instance of another: compatible_for_assignment() of CPython's Objects/typeobject.c, and what that
// calls. It is easier to tell of a class and its base than of any two, so each is followed up to the last that it is laid out like, and those are
// compared.
bool areLaidOutAlike(JSGlobalObject* globalObject, PyType* oldType, PyType* newType)
{
    VM& vm = globalObject->vm();
    constexpr unsigned long inlineValues = 1ul << 2;
    constexpr unsigned long preheader = 1ul << 3 | 1ul << 4;
    constexpr unsigned long haveGC = 1ul << 14;
    auto isLaidOutLikeItsBase = [] (PyType* child) {
        PyType* parent = child->base();
        return parent && child->basicSize() == parent->basicSize() && child->itemSize() == parent->itemSize() && child->dictOffset() == parent->dictOffset()
            && child->weakReferenceOffset() == parent->weakReferenceOffset() && (child->flagsForPython() & haveGC) == (parent->flagsForPython() & haveGC) && child->hasFlag(PyType::IsHeapType);
    };
    auto addTheSameSlots = [&] (PyType* a, PyType* b) {
        if (!a->hasFlag(PyType::IsHeapType) || !b->hasFlag(PyType::IsHeapType))
            return false;
        int size = a->base()->basicSize();
        JSValue slotsOfA = a->getDirect(vm, vm.pythonNames().private_slots);
        JSValue slotsOfB = b->getDirect(vm, vm.pythonNames().private_slots);
        if (slotsOfA && slotsOfB) {
            if (!isEqual(globalObject, slotsOfA, slotsOfB))
                return false;
            size += sizeof(void*) * uncheckedDowncast<PyTuple>(slotsOfA.asCell())->length();
        }
        return size == a->basicSize() && size == b->basicSize();
    };
    PyType* newBase = newType;
    PyType* oldBase = oldType;
    while (isLaidOutLikeItsBase(newBase))
        newBase = newBase->base();
    while (isLaidOutLikeItsBase(oldBase))
        oldBase = oldBase->base();
    if (newBase != oldBase && (newBase->base() != oldBase->base() || !addTheSameSlots(newBase, oldBase)))
        return false;
    return (oldType->flagsForPython() & (inlineValues | preheader)) == (newType->flagsForPython() & (inlineValues | preheader));
}

static void setClass(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete __class__ attribute"_s);
        return;
    }
    if (!isClass(value)) {
        raiseTypeError(globalObject, scope, concatenate("__class__ must be set to a class, not '"_s, typeName(globalObject, value), "' object"_s));
        return;
    }
    auto* newType = asType(value);
    PyType* oldType = typeOf(globalObject, self);
    bool areModules = newType->isSubtypeOf(realm->typeModule()) && oldType->isSubtypeOf(realm->typeModule());
    if (!areModules && (!oldType->hasFlag(PyType::IsHeapType) || !newType->hasFlag(PyType::IsHeapType))) {
        raiseTypeError(globalObject, scope, "__class__ assignment only supported for mutable types or ModuleType subclasses"_s);
        return;
    }
    // The class of a class is not its prototype, which is the class that it is derived from.
    if (isClass(self)) {
        if (!areLaidOutAlike(globalObject, oldType, newType) || !newType->hasFlag(PyType::IsTypeSubclass)) {
            raiseTypeError(globalObject, scope, concatenate("__class__ assignment: '"_s, newType->nameString(globalObject), "' object layout differs from '"_s, oldType->nameString(globalObject), '\''));
            return;
        }
        asType(self)->setMetatype(vm, newType);
        return;
    }
    // The last is what matters here, where it is a kind of cell that a class goes with. It follows from the rest, and is not left to.
    if (!areLaidOutAlike(globalObject, oldType, newType) || !self.isObject() || !newType->instanceStructure() || newType->instanceStructure()->classInfoForCells() != self.asCell()->classInfo()) {
        raiseTypeError(globalObject, scope, concatenate("__class__ assignment: '"_s, newType->nameString(globalObject), "' object layout differs from '"_s, oldType->nameString(globalObject), '\''));
        return;
    }
    asObject(self)->setPrototypeDirect(vm, newType);
}

JSValue getInstanceDict(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, self);
    JSObject* storage = attributeStorage(globalObject, self, type);
    if (!storage)
        return raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', type->nameString(globalObject), "' object has no attribute '__dict__'"_s));
    return PyDict::backedBy(globalObject, storage);
}

// obj.__dict__ = mapping: that dict is the attributes from now on, and the dict that was is a dict like any other. del obj.__dict__ leaves it with none.
void setInstanceDict(JSGlobalObject*, JSValue self, JSValue value);

// PyObject_GenericSetDict(), which is what those that are built in have. An instance of a class of a program's can do without one.
static void setInstanceDictOfBuiltin(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete __dict__"_s);
        return;
    }
    RELEASE_AND_RETURN(scope, setInstanceDict(globalObject, self, value));
}

void setInstanceDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (value && !isDict(value)) {
        raiseTypeError(globalObject, scope, concatenate("__dict__ must be set to a dictionary, not a '"_s, typeName(globalObject, value), '\''));
        return;
    }
    JSValue current = getInstanceDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (current == value)
        return;
    JSObject* object = asObject(self);
    // It parts with the one that it has, which keeps what is in it.
    if (object->getDirect(vm, names.private_foreignDict))
        deleteStoredAttribute(globalObject, object, names.private_foreignDict);
    else
        asDict(current)->detach(globalObject);
    if (!value)
        return;
    if (!asDict(value)->backing()) {
        asDict(value)->becomeBackedBy(globalObject, object);
        return;
    }
    // It is some other object's already.
    object->putDirect(vm, names.private_foreignDict, value);
    typeOf(globalObject, self)->setFlag(PyType::MayHaveForeignDict);
}

// obj.__weakref__: the first of the weak references to it. There is no making one yet, so there is none.
void addGenericGetAttribute(JSGlobalObject* globalObject, PyType* type)
{
    addMethods(globalObject, type, { { "__getattribute__"_s, objectGetAttribute } });
}

void addGenericSetAttribute(JSGlobalObject* globalObject, PyType* type)
{
    addMethods(globalObject, type, { { "__setattr__"_s, objectSetAttr }, { "__delattr__"_s, objectDelAttr } });
}

// ---- type

PYTHON_NATIVE(typeNew)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "type.__new__(X): X is not a type object"_s));
    auto* metatype = asType(args[0]);
    if (args.size() == 2 && !args.keywordCount() && metatype == realm->typeType())
        return JSValue::encode(typeOf(globalObject, args[1])->object());
    if (args.size() != 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("type.__new__() argument 1 must be str, not "_s, typeName(globalObject, args[1]))));
    if (!isTuple(args[2]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("type.__new__() argument 2 must be tuple, not "_s, typeName(globalObject, args[2]))));
    if (!isDict(args[3]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("type.__new__() argument 3 must be dict, not "_s, typeName(globalObject, args[3]))));

    // The most derived metaclass among the bases' has the last word, and if it has a __new__() of its own, the whole of it: type_new_get_bases()
    auto* bases = uncheckedDowncast<PyTuple>(args[2].asCell());
    for (auto& entry : bases->span()) {
        if (isClass(entry.get()))
            continue;
        // list[int] can be derived from, by a class statement, which asks it what to derive from in its place.
        JSValue standsForOthers = getAttributeIfPresent(globalObject, entry.get(), names.dunder_mro_entries);
        RETURN_IF_EXCEPTION(scope, { });
        if (standsForOthers)
            return JSValue::encode(raiseTypeError(globalObject, scope, "type() doesn't support MRO entry resolution; use types.new_class()"_s));
    }
    PyType* winner = calculateMetaclass(globalObject, metatype, bases);
    RETURN_IF_EXCEPTION(scope, { });
    if (winner != metatype) {
        JSValue constructor = winner->lookup(vm, names.dunder_new);
        if (constructor != realm->typeType()->lookup(vm, names.dunder_new)) {
            constructor = bindDescriptor(globalObject, constructor, JSValue(), winner);
            RETURN_IF_EXCEPTION(scope, { });
            MarkedArgumentBuffer arguments;
            arguments.append(winner->object());
            ArgList rest = args.allFrom(1);
            for (unsigned i = 0; i < rest.size(); ++i)
                arguments.append(rest.at(i));
            RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, constructor, arguments, args.keywordNames())));
        }
        metatype = winner;
    }

    PyDict* keywords = nullptr;
    if (args.keywordCount()) {
        keywords = PyDict::create(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            keywords->set(globalObject, args.keywordName(i), args.keywordValue(i));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newType(globalObject, metatype, asString(args[1]), bases, uncheckedDowncast<PyDict>(args[3].asCell()), keywords)));
}

PYTHON_NATIVE(typeInit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount() && args.size() == 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "type.__init__() takes no keyword arguments"_s));
    if (args.size() != 2 && args.size() != 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, "type.__init__() takes 1 or 3 arguments"_s));
    RETURN_NONE();
}

PYTHON_NATIVE(typeCall)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "descriptor '__call__' requires a 'type' object"_s));
    ArgList arguments = args.allFrom(1);
    RELEASE_AND_RETURN(scope, JSValue::encode(instantiate(globalObject, asType(args[0]), arguments, args.keywordNames())));
}

PYTHON_NATIVE(typeMro)
{
    NATIVE_PROLOGUE();
    // It is worked out, and not looked up: a metaclass whose mro() calls this is asking what it would be.
    PyTuple* order = defaultOrder(globalObject, asType(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer classes;
    for (auto& entry : order->span())
        classes.append(asType(entry.get())->object());
    return JSValue::encode(newList(globalObject, classes));
}

PYTHON_NATIVE(typeSubclasses)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MarkedArgumentBuffer subclasses;
    for (PyType* subclass : asType(args[0])->subclasses())
        subclasses.append(subclass->object());
    return JSValue::encode(newList(globalObject, subclasses));
}

PYTHON_NATIVE(typePrepare)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(PyDict::create(globalObject));
}

PYTHON_NATIVE(typeInstanceCheck)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsBoolean(isInstance(globalObject, args.at(1), asType(args.at(0)))));
}

PYTHON_NATIVE(typeSubclassCheck)
{
    NATIVE_PROLOGUE();
    if (!isClass(args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s));
    return JSValue::encode(jsBoolean(asType(args[1])->isSubtypeOf(asType(args[0]))));
}

static JSValue getOwnOr(JSGlobalObject* globalObject, JSValue self, const Identifier& name, JSValue otherwise)
{
    JSValue value = asType(self)->lookupOwn(globalObject->vm(), name);
    return value ? value : otherwise;
}

// ---- Functions


static const FunctionInfo* infoOf(JSValue function)
{
    JSFunction* f = asFunction(function);
    return f->isHostOrBuiltinFunction() ? nullptr : f->jsExecutable()->unlinkedExecutable()->pythonInfo();
}

template<bool qualified>
static JSValue getFunctionName(JSGlobalObject* globalObject, JSValue self)
{
    return jsString(globalObject->vm(), nameOfFunction(globalObject, asFunction(self), qualified));
}

template<bool qualified>
static void setFunctionName(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isString()) {
        raiseTypeError(globalObject, scope, qualified ? "__qualname__ must be set to a string object"_s : "__name__ must be set to a string object"_s);
        return;
    }
    if (qualified) {
        asFunction(self)->putDirect(vm, vm.pythonNames().private_qualname, value);
        return;
    }
    scope.release();
    PropertyDescriptor descriptor(value, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    asFunction(self)->methodTable()->defineOwnProperty(asFunction(self), globalObject, vm.propertyNames->name, descriptor, true);
}

// An attribute of a function that is kept in a property that Python cannot name, and is None until it is set.
#define FUNCTION_PROPERTY(getterName, setterName, privateName) \
    static JSValue getterName(JSGlobalObject* globalObject, JSValue self) \
    { \
        JSValue value = asFunction(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().privateName); \
        return value ? value : jsUndefined(); \
    } \
    static void setterName(JSGlobalObject* globalObject, JSValue self, JSValue value) \
    { \
        VM& vm = globalObject->vm(); \
        if (value && !isNone(value)) \
            asFunction(self)->putDirect(vm, vm.pythonNames().privateName, value); \
        else \
            JSCell::deleteProperty(asFunction(self), globalObject, vm.pythonNames().privateName); \
        JSCell::deleteProperty(asFunction(self), globalObject, vm.pythonNames().private_alignedDefaults); \
    }

FUNCTION_PROPERTY(getFunctionDefaults, setFunctionDefaultsUnchecked, private_defaults)
FUNCTION_PROPERTY(getFunctionKeywordDefaults, setFunctionKeywordDefaultsUnchecked, private_kwdefaults)

static void setFunctionDefaults(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value && !isNone(value) && !isTuple(value)) {
        raiseTypeError(globalObject, scope, "__defaults__ must be set to a tuple object"_s);
        return;
    }
    setFunctionDefaultsUnchecked(globalObject, self, value);
}

static void setFunctionKeywordDefaults(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value && !isNone(value) && !isDict(value)) {
        raiseTypeError(globalObject, scope, "__kwdefaults__ must be set to a dict object"_s);
        return;
    }
    setFunctionKeywordDefaultsUnchecked(globalObject, self, value);
}

// __doc__ and __module__ are what the source and the module say until they are set to something else, None included.
static JSValue getFunctionDoc(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    if (JSValue value = asFunction(self)->getDirect(vm, vm.pythonNames().private_doc))
        return value;
    const FunctionInfo* info = infoOf(self);
    return info && !info->docstring.isNull() ? JSValue(jsString(vm, info->docstring)) : jsUndefined();
}

static void setFunctionDoc(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    asFunction(self)->putDirect(vm, vm.pythonNames().private_doc, value ? value : jsUndefined());
}

static JSValue getFunctionModule(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    if (JSValue value = asFunction(self)->getDirect(vm, vm.pythonNames().private_module))
        return value;
    if (!infoOf(self))
        return jsUndefined();
    JSValue globals = getAttribute(globalObject, self, Identifier::fromString(vm, "__globals__"_s));
    JSValue name = asDict(globals)->getString(globalObject, "__name__"_s);
    return name ? name : jsUndefined();
}

static void setFunctionModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    asFunction(self)->putDirect(vm, vm.pythonNames().private_module, value ? value : jsUndefined());
}

// descriptor.__get__(instance, owner=None) is to be told one or the other, whatever the descriptor.
static bool checkDescriptorGet(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args)
{
    if (!isNone(args[1]) || (args.size() > 2 && !isNone(args[2])))
        return true;
    raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s);
    return false;
}

PYTHON_NATIVE(functionGet)
{
    NATIVE_PROLOGUE();
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    if (isNone(args[1]))
        return JSValue::encode(args[0]);
    return JSValue::encode(PyBoundMethod::create(globalObject, args[0], args[1]));
}

// method_get() and wrapperdescr_get(): it is a method only of an instance of the class that it is in.
PYTHON_NATIVE(methodDescriptorGet)
{
    NATIVE_PROLOGUE();
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    if (isNone(args[1]))
        return JSValue::encode(args[0]);
    auto* function = uncheckedDowncast<PyNativeFunction>(args[0].asCell());
    PyType* owner = asType(function->owner());
    if (!typeOf(globalObject, args[1])->isSubtypeOf(owner))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, function->name(vm), "' for '"_s, owner->nameString(globalObject), "' objects doesn't apply to a '"_s, typeName(globalObject, args[1]), "' object"_s)));
    return JSValue::encode(PyBoundMethod::create(globalObject, args[0], args[1]));
}

PYTHON_NATIVE(callableCall)
{
    NATIVE_PROLOGUE();
    ArgList arguments = args.allFrom(1);
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, args.at(0), arguments, args.keywordNames())));
}

// ---- Bound methods

static PyBoundMethod* asMethod(JSValue value) { return uncheckedDowncast<PyBoundMethod>(value.asCell()); }

// ---- What is written in C++

// The function, and what it is bound to: the instance or the class that it was got from, the class that a __new__ is of, or the module that a
// function is of. Empty if it is bound to nothing.
struct NativeCallable {
    PyNativeFunction* function;
    JSValue self;
};

static NativeCallable nativeCallableOf(JSValue value)
{
    if (auto* method = tryBoundMethod(value))
        return { uncheckedDowncast<PyNativeFunction>(method->function().asCell()), method->self() };
    auto* function = uncheckedDowncast<PyNativeFunction>(value.asCell());
    bool isBound = function->kind() == PyNativeFunction::Kind::Function || function->kind() == PyNativeFunction::Kind::New || function->kind() == PyNativeFunction::Kind::StaticMethod;
    JSObject* owner = function->owner();
    return { function, isBound && owner ? JSValue(isType(owner) ? asType(owner)->object() : owner) : JSValue() };
}

static JSValue getNativeName(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    return jsString(vm, nativeCallableOf(self).function->name(vm));
}

static JSValue getNativeQualifiedName(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto [function, bound] = nativeCallableOf(self);
    // A function of a module goes by its name alone.
    if (!function->owner() || !isType(function->owner()))
        return jsString(vm, function->name(vm));
    // One that is bound goes by the class that it was got by way of, and one that is not by the class that it is in.
    PyType* type = asType(function->owner());
    if (bound && typeOf(globalObject, self) == globalObject->pyRealm()->typeBuiltinFunction())
        type = isClass(bound) ? asType(bound) : typeOf(globalObject, bound);
    return strOrMemoryError(globalObject, concatenate(qualifiedNameWithoutModule(globalObject, type), '.', function->name(vm)));
}

static JSValue getNativeDoc(JSGlobalObject* globalObject, JSValue self)
{
    auto* description = nativeCallableOf(self).function->description();
    return description && !description->doc.isNull() ? JSValue(jsString(globalObject->vm(), String(description->doc))) : jsUndefined();
}

static JSValue getNativeTextSignature(JSGlobalObject* globalObject, JSValue self)
{
    // What it says is not always what its arguments are checked against.
    PyNativeFunction* function = nativeCallableOf(self).function;
    ASCIILiteral text = function->description() ? function->description()->signature : function->signature() ? function->signature()->text() : ASCIILiteral();
    return text.isNull() ? jsUndefined() : JSValue(jsString(globalObject->vm(), String(text)));
}

// The name of the module that a function is of. A method has none.
static JSValue getNativeModule(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    if (auto* method = tryBoundMethod(self)) {
        JSValue set = method->getDirect(vm, vm.pythonNames().private_module);
        return set ? set : jsUndefined();
    }
    auto* function = uncheckedDowncast<PyNativeFunction>(self.asCell());
    if (JSValue set = function->getDirect(vm, vm.pythonNames().private_module))
        return set;
    if (!function->owner() || isType(function->owner()))
        return jsUndefined();
    JSValue name = function->owner()->getDirect(vm, vm.pythonNames().dunder_name);
    return name ? name : jsUndefined();
}

static void setNativeModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    asObject(self)->putDirect(vm, vm.pythonNames().private_module, value ? value : jsUndefined());
}

PYTHON_NATIVE(nativeCallableCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (!isEquality(op) || typeOf(globalObject, args[1]) != typeOf(globalObject, args[0]))
        RETURN_NOT_IMPLEMENTED();
    auto a = nativeCallableOf(args[0]);
    auto b = nativeCallableOf(args[1]);
    bool areEqual = a.function == b.function && (a.self ? b.self && isIdentical(a.self, b.self) : !b.self);
    return JSValue::encode(jsBoolean(areEqual == (op == ComparisonOperator::Eq)));
}

PYTHON_NATIVE(nativeCallableHash)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto [function, bound] = nativeCallableOf(args[0]);
    int64_t result = (bound && bound.isCell() ? hashOfPointer(bound.asCell()) : bound ? static_cast<int64_t>(JSValue::encode(bound)) : 0) ^ hashOfPointer(function);
    return JSValue::encode(intFromInt64(globalObject, result == -1 ? -2 : result));
}

// How pickle is to find it again: by its name if it is a function of a module, and otherwise by getting it from what it was got from.
PYTHON_NATIVE(nativeCallableReduce)
{
    NATIVE_PROLOGUE();
    auto [function, bound] = nativeCallableOf(args[0]);
    JSString* name = jsString(vm, function->name(vm));
    if (function->kind() == PyNativeFunction::Kind::Function)
        return JSValue::encode(name);
    JSValue getattr = getStoredAttribute(vm, realm->builtinsModule(), Identifier::fromString(vm, "getattr"_s));
    UNUSED_PARAM(scope);
    return JSValue::encode(PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { bound ? bound : JSValue(function->owner()), name }) }));
}

// classmethod_descriptor.__get__(instance, owner=None)
PYTHON_NATIVE(classMethodDescriptorGet)
{
    NATIVE_PROLOGUE();
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    auto* function = uncheckedDowncast<PyNativeFunction>(args[0].asCell());
    JSValue type = args.at(2);
    if (!type || isNone(type))
        type = typeOf(globalObject, args[1])->object();
    if (!isClass(type))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, function->name(vm), "' for type '"_s, asType(function->owner())->nameString(globalObject), "' needs a type, not a '"_s, typeName(globalObject, type), "' as arg 2"_s)));
    if (!asType(type)->isSubtypeOf(asType(function->owner())))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, function->name(vm), "' requires a subtype of '"_s, asType(function->owner())->nameString(globalObject), "' but received '"_s, asType(type)->nameString(globalObject), '\'')));
    return JSValue::encode(PyBoundMethod::create(globalObject, function, type));
}

PYTHON_NATIVE(methodNew)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(PyBoundMethod::createMethod(globalObject, args[1], args[2]));
}

PYTHON_NATIVE(methodCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isEquality(op) || typeOf(globalObject, args[1]) != realm->typeMethod())
        RETURN_NOT_IMPLEMENTED();
    auto* other = asMethod(args[1]);
    // The functions are equal, and they are bound to the same object.
    bool areEqual = isEqual(globalObject, asMethod(args[0])->function(), other->function());
    RETURN_IF_EXCEPTION(scope, { });
    areEqual = areEqual && isIdentical(asMethod(args[0])->self(), other->self());
    return JSValue::encode(jsBoolean(areEqual == (op == ComparisonOperator::Eq)));
}

PYTHON_NATIVE(methodHash)
{
    NATIVE_PROLOGUE();
    JSValue self = asMethod(args[0])->self();
    int64_t function = hash(globalObject, asMethod(args[0])->function());
    RETURN_IF_EXCEPTION(scope, { });
    // By which object it is, whatever the object's own __hash__ says, since that is how they are compared.
    int64_t result = (self.isCell() ? hashOfPointer(self.asCell()) : static_cast<int64_t>(JSValue::encode(self))) ^ function;
    return JSValue::encode(jsNumber(result == -1 ? -2 : result));
}

// What a bound method does not have itself, the function has.
PYTHON_NATIVE(methodGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = genericGetAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, asMethod(args[0])->function(), *name)));
}

// ---- property, staticmethod, classmethod


static JSValue allocateNativeObject(JSGlobalObject* globalObject, ThrowScope& scope, JSValue type)
{
    if (!isClass(type))
        return raiseTypeError(globalObject, scope, "__new__(X): X is not a type object"_s);
    return PyNativeObject::create(globalObject->vm(), asType(type)->instanceStructure());
}

PYTHON_NATIVE(nativeObjectNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(allocateNativeObject(globalObject, scope, args.at(0))));
}

template<unsigned field>
static JSValue getField(JSGlobalObject*, JSValue self)
{
    JSValue value = asNativeObject(self)->field(field);
    return value ? value : jsUndefined();
}

PYTHON_NATIVE(descriptorGet)
{
    NATIVE_PROLOGUE();
    // descriptor.__get__(instance, owner=None)
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    JSValue instance = args.at(1);
    JSValue owner = args.at(2);
    if (owner && !isNone(owner) && !isClass(owner) && typeOf(globalObject, args[0])->isSubtypeOf(realm->typeClassMethod()))
        return JSValue::encode(PyBoundMethod::createMethod(globalObject, asNativeObject(args[0])->field(0), owner));
    PyType* type = owner && isClass(owner) ? asType(owner) : typeOf(globalObject, instance ? instance : jsUndefined());
    RELEASE_AND_RETURN(scope, JSValue::encode(bindDescriptor(globalObject, args.at(0), !instance || isNone(instance) ? JSValue() : instance, type)));
}

// The class that a descriptor written in C++ is an attribute of, and what it is called.
static std::pair<PyType*, JSString*> ownerAndNameOf(JSValue descriptor)
{
    auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(descriptor.asCell());
    return { getSet->owner(), getSet->name() };
}

// member.__get__(instance, owner=None), member.__set__(instance, value) and member.__delete__(instance), and the same of a getset_descriptor
enum class DescriptorOperation : uint8_t { Get, Set, Delete };
PYTHON_NATIVE(nativeDescriptorOperation)
{
    auto operation = unpack<DescriptorOperation>(callFrame, 0);
    NATIVE_PROLOGUE();
    ASCIILiteral method = operation == DescriptorOperation::Get ? "__get__"_s : operation == DescriptorOperation::Set ? "__set__"_s : "__delete__"_s;
    unsigned count = operation == DescriptorOperation::Set ? 3 : 2;
    auto [owner, name] = ownerAndNameOf(args[0]);
    JSValue instance = args[1];
    if (operation == DescriptorOperation::Get && isNone(instance)) {
        if (args.size() < 3 || isNone(args[2]))
            return JSValue::encode(raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s));
        return JSValue::encode(args[0]);
    }
    if (!typeOf(globalObject, instance)->isSubtypeOf(owner))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, name->value(globalObject).data, "' for '"_s, owner->nameString(globalObject), "' objects doesn't apply to a '"_s, typeName(globalObject, instance), "' object"_s)));
    if (operation == DescriptorOperation::Get)
        RELEASE_AND_RETURN(scope, JSValue::encode(bindDescriptor(globalObject, args[0], instance, typeOf(globalObject, instance))));
    setDescriptor(globalObject, args[0], instance, name->view(globalObject), operation == DescriptorOperation::Set ? args[2] : JSValue());
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// <member 'x' of 'A' objects>, <attribute '__dict__' of 'A' objects>
PYTHON_NATIVE(nativeDescriptorRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto [owner, name] = ownerAndNameOf(args[0]);
    auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(args[0].asCell());
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', getSet->isMember() ? "member"_s : "attribute"_s, " '"_s, name->value(globalObject).data, "' of '"_s, owner->nameString(globalObject), "' objects>"_s))));
}

// staticmethod(function) and classmethod(function)
PYTHON_NATIVE(wrapperInit)
{
    NATIVE_PROLOGUE();
    asNativeObject(args[0])->setField(vm, 0, args[1]);
    // It goes by the name of what it wraps: functools_wraps() of CPython's Objects/funcobject.c.
    for (const Identifier* name : { &names.dunder_module, &names.dunder_name, &names.dunder_qualname, &names.dunder_doc }) {
        JSValue value = getAttributeIfPresent(globalObject, args[1], *name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            continue;
        setAttribute(globalObject, args[0], *name, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(staticMethodCall)
{
    NATIVE_PROLOGUE();
    ArgList arguments = args.allFrom(1);
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, asNativeObject(args.at(0))->field(0), arguments, args.keywordNames())));
}

// ---- super

// super(type, object_or_type). With no arguments, the compiler has filled them in.
PYTHON_NATIVE(superInit)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "super"_s))
        return { };
    if (args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("super() expected at most 2 arguments, got "_s, args.size() - 1)));
    auto* object = asNativeObject(args[0]);
    if (args.size() == 1)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "super(): no arguments"_s));
    if (!isClass(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("super() argument 1 must be a type, not "_s, typeName(globalObject, args[1]))));
    PyType* type = asType(args[1]);
    JSValue instance = args.size() > 2 && !isNone(args[2]) ? args[2] : JSValue();
    PyType* start = nullptr;
    if (instance) {
        start = superCheck(globalObject, type, instance);
        RETURN_IF_EXCEPTION(scope, { });
    }
    object->setField(vm, 0, type);
    object->setField(vm, 1, instance);
    object->setField(vm, 2, start ? JSValue(start) : JSValue());
    RETURN_NONE();
}

PYTHON_NATIVE(superRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* object = asNativeObject(args[0]);
    String type = object->field(0) ? asType(object->field(0))->nameString(globalObject) : "NULL"_str;
    if (JSValue start = object->field(2))
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<super: <class '"_s, type, "'>, <"_s, asType(start)->nameString(globalObject), " object>>"_s))));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<super: <class '"_s, type, "'>, NULL>"_s))));
}

// super(C) as an attribute of a class: got from an instance, it is super(C, instance).
PYTHON_NATIVE(superGet)
{
    NATIVE_PROLOGUE();
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    auto* object = asNativeObject(args[0]);
    JSValue instance = args[1];
    if (isNone(instance) || object->field(1))
        return JSValue::encode(object);
    JSValue type = object->field(0) ? JSValue(asType(object->field(0))->object()) : jsUndefined();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, typeOf(globalObject, object)->object(), type, instance)));
}

// <staticmethod(<function f at 0x...>)>
PYTHON_NATIVE(wrapperRepr)
{
    NATIVE_PROLOGUE();
    JSValue wrapped = asNativeObject(args[0])->field(0);
    String text = repr(globalObject, wrapped ? wrapped : jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', isInstance(globalObject, args[0], realm->typeStaticMethod()) ? "staticmethod"_s : "classmethod"_s, '(', text, ")>"_s))));
}

// It is bound already.
PYTHON_NATIVE(methodGet)
{
    NATIVE_PROLOGUE();
    if (!checkDescriptorGet(globalObject, scope, args))
        return { };
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(superGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = getSuperAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value) {
        value = genericGetAttribute(globalObject, args[0], *name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, concatenate("'super' object has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(value);
}

// ---- Modules

// It has nothing in it until __init__() has named it.
PYTHON_NATIVE(moduleNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyInstance::create(vm, asType(args[0])->instanceStructure()));
}

// module(name, doc=None)
PYTHON_NATIVE(moduleInit)
{
    NATIVE_PROLOGUE();
    JSValue name = args.at(1);
    if (!stringIn(name))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("module() argument 'name' must be str, not "_s, isNone(name) ? "None"_s : typeName(globalObject, name))));
    JSObject* module = asObject(args[0]);
    JSValue doc = args.at(2);
    putStoredAttribute(vm, module, names.dunder_name, name);
    putStoredAttribute(vm, module, names.dunder_doc, doc ? doc : jsUndefined());
    putStoredAttribute(vm, module, names.dunder_package, jsUndefined());
    putStoredAttribute(vm, module, names.dunder_loader, jsUndefined());
    putStoredAttribute(vm, module, names.dunder_spec, jsUndefined());
    RETURN_NONE();
}

PYTHON_NATIVE(moduleGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(getModuleAttribute(globalObject, args[0], *name)));
}

// It is for importlib to say: _PyImport_ImportlibModuleRepr()
PYTHON_NATIVE(moduleRepr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, realm->importState().importlib.get(), Identifier::fromString(vm, "_module_repr"_s), args[0])));
}

// ---- Generators


PYTHON_NATIVE(generatorSendMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorSend(globalObject, asGenerator(args[0]), args[1])));
}

PYTHON_NATIVE(generatorNextMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorSend(globalObject, asGenerator(args.at(0)), jsUndefined())));
}

// generator.throw(exception), or the old way, throw(type, value, traceback)
PYTHON_NATIVE(generatorThrowMethod)
{
    NATIVE_PROLOGUE();
    if (!warnOfThrowSignature(globalObject, args, "throw"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorThrow(globalObject, asGenerator(args[0]), packThrowArguments(globalObject, args, 1))));
}

PYTHON_NATIVE(generatorCloseMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorClose(globalObject, asGenerator(args.at(0)))));
}

// ---- Setting them up

void initializeObjectAndType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Function = PyRealm::WellKnownFunction;
    auto remember = [&] (PyType* type, const Identifier& name, Function function) {
        realm->setFunction(vm, function, asObject(type->lookupOwn(vm, name)));
    };

    PyType* object = realm->typeObject();
    addMethods(globalObject, object, {
        { "__new__"_s, objectNew, Kind::New },
        { "__init__"_s, objectInit },
        { "__repr__"_s, objectRepr },
        { "__str__"_s, objectStr },
        { "__eq__"_s, objectEq },
        { "__ne__"_s, objectNe },
        { "__lt__"_s, returnNotImplemented },
        { "__le__"_s, returnNotImplemented },
        { "__gt__"_s, returnNotImplemented },
        { "__ge__"_s, returnNotImplemented },
        { "__hash__"_s, nativeHash },
        { "__getattribute__"_s, objectGetAttribute },
        { "__setattr__"_s, objectSetAttr },
        { "__delattr__"_s, objectDelAttr },
        { "__format__"_s, objectFormat },
        { "__dir__"_s, objectDir },
        { "__init_subclass__"_s, objectInitSubclass, Kind::ClassMethod },
        { "__subclasshook__"_s, returnNotImplemented, Kind::ClassMethod },
    });
    addGetSet(globalObject, object, "__class__"_s, getClass, setClass);
    remember(object, names.dunder_new, Function::ObjectNew);
    remember(object, names.dunder_init, Function::ObjectInit);
    remember(object, names.dunder_getattribute, Function::ObjectGetAttribute);
    remember(object, names.dunder_setattr, Function::ObjectSetAttr);
    remember(object, names.dunder_delattr, Function::ObjectDelAttr);
    remember(object, names.dunder_eq, Function::ObjectEq);
    remember(object, names.dunder_ne, Function::ObjectNe);
    remember(object, names.dunder_hash, Function::ObjectHash);
    remember(object, names.dunder_repr, Function::ObjectRepr);
    remember(object, names.dunder_str, Function::ObjectStr);
    remember(object, names.dunder_format, Function::ObjectFormat);

    PyType* type = realm->typeType();
    addMethods(globalObject, type, {
        { "__new__"_s, typeNew, Kind::New },
        { "__init__"_s, typeInit },
        { "__call__"_s, typeCall },
        { "__getattribute__"_s, typeGetAttribute },
        { "__setattr__"_s, typeSetAttr },
        { "__delattr__"_s, typeDelAttr },
        { "__repr__"_s, nativeRepr },
        { "__dir__"_s, typeDir },
        { "mro"_s, typeMro },
        { "__subclasses__"_s, typeSubclasses },
        { "__prepare__"_s, typePrepare, Kind::ClassMethod, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__instancecheck__"_s, typeInstanceCheck },
        { "__subclasscheck__"_s, typeSubclassCheck },
    });
    remember(type, names.dunder_call, Function::TypeCall);
    remember(type, names.dunder_getattribute, Function::TypeGetAttribute);
    remember(type, names.dunder_setattr, Function::TypeSetAttr);
    remember(type, names.dunder_delattr, Function::TypeDelAttr);
    // check_set_special_type_attr()
    static constexpr auto checkSetSpecial = [] (JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, JSValue value, ASCIILiteral name) {
        if (!type->isImmutable() && value)
            return audit(globalObject, "object.__setattr__"_s, type->object(), jsString(globalObject->vm(), String(name)), value);
        raiseTypeError(globalObject, scope, concatenate("cannot "_s, !type->isImmutable() ? "delete"_s : "set"_s, " '"_s, name, "' attribute of immutable type '"_s, type->nameString(globalObject), '\''));
        return false;
    };
    addGetSet(globalObject, type, "__name__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->name(); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        PyType* type = asType(self);
        if (!checkSetSpecial(globalObject, scope, type, value, "__name__"_s))
            return;
        JSString* name = stringIn(value);
        if (!name) {
            raiseTypeError(globalObject, scope, concatenate("can only assign string to "_s, type->nameString(globalObject), ".__name__, not '"_s, typeName(globalObject, value), '\''));
            return;
        }
        auto text = name->value(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (text->contains(static_cast<char16_t>(0))) {
            raiseValueError(globalObject, scope, "type name must not contain null characters"_s);
            return;
        }
        type->setName(globalObject->vm(), name);
    });
    addGetSet(globalObject, type, "__qualname__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), qualifiedNameWithoutModule(globalObject, asType(self))); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        PyType* type = asType(self);
        if (!checkSetSpecial(globalObject, scope, type, value, "__qualname__"_s))
            return;
        if (!value.isString()) {
            raiseTypeError(globalObject, scope, concatenate("can only assign string to "_s, type->nameString(globalObject), ".__qualname__, not '"_s, typeName(globalObject, value), '\''));
            return;
        }
        type->putDirect(vm, vm.pythonNames().private_qualname, value);
    });
    addMember(globalObject, type, "__basicsize__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(asType(self)->basicSize()); });
    addMember(globalObject, type, "__itemsize__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(asType(self)->itemSize()); });
    addMember(globalObject, type, "__dictoffset__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(asType(self)->dictOffset()); });
    addMember(globalObject, type, "__weakrefoffset__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(asType(self)->weakReferenceOffset()); });
    addMember(globalObject, type, "__flags__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, asType(self)->flagsForPython()); });
    addGetSet(globalObject, type, "__text_signature__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        // Only a built-in class says how it is called in this way.
        auto* description = asType(self)->hasFlag(PyType::IsHeapType) ? nullptr : findTypeDescription(asType(self)->nameWithoutModule(globalObject));
        return description && !description->signature.isNull() ? JSValue(jsString(globalObject->vm(), String(description->signature))) : jsUndefined();
    });
    addGetSet(globalObject, type, "__module__"_s, [] (JSGlobalObject* globalObject, JSValue self) {
        // A built-in class does not look in itself for it. `type` has there the very thing that is asking.
        if (!asType(self)->hasFlag(PyType::IsHeapType)) {
            // Unless it is one that can be added to, and has been told.
            if (JSValue own = asType(self)->isImmutable() ? JSValue() : asType(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().dunder_module))
                return own;
            String module = asType(self)->moduleOfBuiltin();
            return JSValue(module.isNull() ? jsNontrivialString(globalObject->vm(), "builtins"_s) : jsString(globalObject->vm(), module));
        }
        return getOwnOr(globalObject, self, globalObject->vm().pythonNames().dunder_module, jsNontrivialString(globalObject->vm(), "builtins"_s));
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        if (!checkSetSpecial(globalObject, scope, asType(self), value, "__module__"_s))
            return;
        asType(self)->putDirect(globalObject->vm(), globalObject->vm().pythonNames().dunder_module, value);
    });
    addGetSet(globalObject, type, "__abstractmethods__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        // `type` has there the very thing that is asking.
        JSValue methods = self == globalObject->pyRealm()->typeType() ? JSValue() : asType(self)->lookupOwn(vm, vm.pythonNames().dunder_abstractmethods);
        return methods ? methods : raise(globalObject, scope, BuiltinType::AttributeError, "__abstractmethods__"_s);
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        PyType* type = asType(self);
        bool isAbstract = false;
        if (value) {
            isAbstract = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, void());
            type->setAttribute(vm, vm.pythonNames().dunder_abstractmethods, value);
        } else {
            bool wasThere = type->deleteAttribute(vm, globalObject, vm.pythonNames().dunder_abstractmethods);
            RETURN_IF_EXCEPTION(scope, void());
            if (!wasThere) {
                raise(globalObject, scope, BuiltinType::AttributeError, "__abstractmethods__"_s);
                return;
            }
        }
        if (isAbstract)
            type->setFlag(PyType::IsAbstract);
        else
            type->clearFlag(PyType::IsAbstract);
    });
    addGetSet(globalObject, type, "__bases__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->bases(); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setBases(globalObject, asType(self), value); });
    addGetSet(globalObject, type, "__base__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->base() ? JSValue(asType(self)->base()->object()) : jsUndefined(); });
    addGetSet(globalObject, type, "__mro__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return orderOfResolution(globalObject, asType(self)); });
    addGetSet(globalObject, type, "__dict__"_s, getTypeDict);
    addGetSet(globalObject, type, "__doc__"_s, [] (JSGlobalObject* globalObject, JSValue self) {
        // Nor for this.
        if (self == globalObject->pyRealm()->typeType()) {
            auto* description = findTypeDescription("type"_s);
            return JSValue(jsString(globalObject->vm(), String(description->doc)));
        }
        // type_get_doc(): what the class itself has, and if that has a __get__() it is asked what it is for the class.
        JSValue own = getOwnOr(globalObject, self, globalObject->vm().pythonNames().dunder_doc, jsUndefined());
        return bindDescriptor(globalObject, own, JSValue(), asType(self));
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!checkSetSpecial(globalObject, scope, asType(self), value, "__doc__"_s))
            return;
        asType(self)->setAttribute(vm, vm.pythonNames().dunder_doc, value);
    });

    addMethods(globalObject, realm->typeNoneType(), {
        { "__eq__"_s, objectEq },
        { "__ne__"_s, objectNe },
        { "__lt__"_s, returnNotImplemented },
        { "__le__"_s, returnNotImplemented },
        { "__gt__"_s, returnNotImplemented },
        { "__ge__"_s, returnNotImplemented },
        { "__repr__"_s, nativeRepr },
        { "__bool__"_s, returnFalse },
        { "__hash__"_s, nativeHash },
    });
    addMethods(globalObject, realm->typeNotImplementedType(), { { "__repr__"_s, nativeRepr }, { "__bool__"_s, notImplementedBool } });
    addMethods(globalObject, realm->typeEllipsis(), { { "__repr__"_s, nativeRepr } });
}

void initializeFunctionTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    // What the rest of the setting up is done with comes first.
    for (PyType* type : { realm->typeProperty(), realm->typeStaticMethod(), realm->typeClassMethod(), realm->typeSuper(), realm->typeNotImplementedType(), realm->typeEllipsis(), realm->typeDictKeys(), realm->typeDictValues(), realm->typeDictItems(), realm->typeMappingProxy() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));
    realm->typeGetSetDescriptor()->setInstanceStructure(vm, PyGetSetDescriptor::createStructure(vm, globalObject, realm->typeGetSetDescriptor()));
    realm->typeMemberDescriptor()->setInstanceStructure(vm, PyGetSetDescriptor::createStructure(vm, globalObject, realm->typeMemberDescriptor()));

    // What JavaScript can do with a function it can do with one of these.
    for (PyType* type : { realm->typeFunction(), realm->typeMethod(), realm->typeBuiltinFunction(), realm->typeMethodDescriptor(), realm->typeWrapperDescriptor(), realm->typeClassMethodDescriptor(), realm->typeMethodWrapper() })
        type->setPrototypeDirect(vm, globalObject->functionPrototype());
    realm->typeFunction()->setInstanceStructure(vm, Structure::create(vm, globalObject, realm->typeFunction(), TypeInfo(JSFunctionType, JSFunction::StructureFlags | IsImmutablePrototypeExoticObject), JSFunction::info()));
    addMethods(globalObject, realm->typeFunction(), {
        { "__repr__"_s, nativeRepr },
        { "__call__"_s, callableCall },
    });
    addGetSet(globalObject, realm->typeFunction(), "__name__"_s, getFunctionName<false>, setFunctionName<false>);
    addGetSet(globalObject, realm->typeFunction(), "__qualname__"_s, getFunctionName<true>, setFunctionName<true>);
    addGetSet(globalObject, realm->typeFunction(), "__doc__"_s, getFunctionDoc, setFunctionDoc);

    // What is written in C++, as it is in a class, as it is got from an instance, and as it is when it is no method.
    for (PyType* type : { realm->typeBuiltinFunction(), realm->typeMethodWrapper() })
        type->setInstanceStructure(vm, PyBoundMethod::createStructure(vm, globalObject, type));
    for (PyType* type : { realm->typeBuiltinFunction(), realm->typeMethodDescriptor(), realm->typeClassMethodDescriptor(), realm->typeWrapperDescriptor(), realm->typeMethodWrapper() }) {
        addMethods(globalObject, type, {
            { "__repr__"_s, nativeRepr },
            { "__call__"_s, callableCall },
        });
        addMethodsThatCPythonHas(globalObject, type, { { "__reduce__"_s, nativeCallableReduce } });
        addGetSet(globalObject, type, "__name__"_s, getNativeName);
        addGetSet(globalObject, type, "__qualname__"_s, getNativeQualifiedName);
        addGetSet(globalObject, type, "__doc__"_s, getNativeDoc);
        addGetSet(globalObject, type, "__text_signature__"_s, getNativeTextSignature);
    }
    for (PyType* type : { realm->typeMethodDescriptor(), realm->typeClassMethodDescriptor(), realm->typeWrapperDescriptor(), realm->typeMethodWrapper() })
        addMember(globalObject, type, "__objclass__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(nativeCallableOf(self).function->owner())->object(); });
    for (PyType* type : { realm->typeBuiltinFunction(), realm->typeMethodWrapper() }) {
        addMethods(globalObject, type, {
            { "__hash__"_s, nativeCallableHash },
        });
        addComparisons(globalObject, type, nativeCallableCompare);
        addGetSet(globalObject, type, "__self__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
            // A static method is kept with its class and does not let on.
            auto [function, bound] = nativeCallableOf(self);
            return bound && function->kind() != PyNativeFunction::Kind::StaticMethod ? bound : jsUndefined();
        });
    }
    addGetSet(globalObject, realm->typeBuiltinFunction(), "__module__"_s, getNativeModule, setNativeModule);
    for (PyType* type : { realm->typeMethodDescriptor(), realm->typeWrapperDescriptor() })
        addMethods(globalObject, type, { { "__get__"_s, methodDescriptorGet } });
    addMethods(globalObject, realm->typeClassMethodDescriptor(), { { "__get__"_s, classMethodDescriptorGet } });
    for (PyType* type : { realm->typeMemberDescriptor(), realm->typeGetSetDescriptor() }) {
        addMethods(globalObject, type, {
            { "__get__"_s, nativeDescriptorOperation, Kind::Method, pack(DescriptorOperation::Get) },
            { "__set__"_s, nativeDescriptorOperation, Kind::Method, pack(DescriptorOperation::Set) },
            { "__delete__"_s, nativeDescriptorOperation, Kind::Method, pack(DescriptorOperation::Delete) },
            { "__repr__"_s, nativeDescriptorRepr },
        });
        addGetSet(globalObject, type, "__doc__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(self.asCell());
            return !getSet->doc().isNull() ? JSValue(jsString(globalObject->vm(), String(getSet->doc()))) : jsUndefined();
        });
        addMember(globalObject, type, "__name__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return ownerAndNameOf(self).second; });
        addMember(globalObject, type, "__objclass__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return ownerAndNameOf(self).first->object(); });
        addGetSet(globalObject, type, "__qualname__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            auto [owner, name] = ownerAndNameOf(self);
            return jsString(globalObject->vm(), concatenate(qualifiedNameWithoutModule(globalObject, owner), '.', name->value(globalObject).data));
        });
    }
    PyType* function = realm->typeFunction();
    addMethods(globalObject, function, { { "__get__"_s, functionGet } });
    addGetSet(globalObject, function, "__defaults__"_s, getFunctionDefaults, setFunctionDefaults);
    addGetSet(globalObject, function, "__kwdefaults__"_s, getFunctionKeywordDefaults, setFunctionKeywordDefaults);
    addGetSet(globalObject, function, "__module__"_s, getFunctionModule, setFunctionModule);

    for (PyType* type : { realm->typeFunction(), realm->typeStaticMethod(), realm->typeClassMethod(), realm->typeBaseException() })
        addGetSet(globalObject, type, "__dict__"_s, getInstanceDict, setInstanceDictOfBuiltin);
    addMember(globalObject, realm->typeModule(), "__dict__"_s, getInstanceDict);

    for (PyType* type : { realm->typeNoneType(), realm->typeNotImplementedType(), realm->typeEllipsis() })
        addMethods(globalObject, type, { { "__new__"_s, singletonNew, Kind::New } });

    PyType* method = realm->typeMethod();
    method->setInstanceStructure(vm, PyBoundMethod::createStructure(vm, globalObject, method));
    addMethods(globalObject, method, {
        { "__new__"_s, methodNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, nativeRepr },
        { "__call__"_s, callableCall },
        { "__hash__"_s, methodHash },
        { "__get__"_s, methodGet },
        { "__getattribute__"_s, methodGetAttribute },
    });
    addComparisons(globalObject, method, methodCompare);
    addGetSet(globalObject, method, "__doc__"_s, [] (JSGlobalObject* globalObject, JSValue self) { return getAttribute(globalObject, asMethod(self)->function(), globalObject->vm().pythonNames().dunder_doc); });
    addMember(globalObject, method, "__func__"_s, [] (JSGlobalObject*, JSValue self) { return asMethod(self)->function(); });
    addMember(globalObject, method, "__self__"_s, [] (JSGlobalObject*, JSValue self) { return asMethod(self)->self(); });

    PyType* property = realm->typeProperty();
    addMethods(globalObject, property, {
        { "__new__"_s, nativeObjectNew, Kind::New },
    });

    for (PyType* type : { realm->typeStaticMethod(), realm->typeClassMethod() }) {
        addMethods(globalObject, type, {
            { "__new__"_s, nativeObjectNew, Kind::New },
            { "__init__"_s, wrapperInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__get__"_s, descriptorGet },
        });
        addMember(globalObject, type, "__func__"_s, getField<0>);
        addMember(globalObject, type, "__wrapped__"_s, getField<0>);
    }
    addMethods(globalObject, realm->typeStaticMethod(), { { "__call__"_s, staticMethodCall }, { "__repr__"_s, wrapperRepr } });
    addMethods(globalObject, realm->typeClassMethod(), { { "__repr__"_s, wrapperRepr } });

    addMethods(globalObject, realm->typeSuper(), {
        { "__new__"_s, nativeObjectNew, Kind::New },
        { "__init__"_s, superInit },
        { "__getattribute__"_s, superGetAttribute },
        { "__repr__"_s, superRepr },
        { "__get__"_s, superGet },
    });
    addMember(globalObject, realm->typeSuper(), "__thisclass__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { JSValue type = asNativeObject(self)->field(0); return type ? JSValue(asType(type)->object()) : jsUndefined(); });
    addMember(globalObject, realm->typeSuper(), "__self__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { JSValue instance = asNativeObject(self)->field(1); return instance ? instance : jsUndefined(); });
    addMember(globalObject, realm->typeSuper(), "__self_class__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { JSValue type = asNativeObject(self)->field(2); return type ? JSValue(asType(type)->object()) : jsUndefined(); });

    PyType* module = realm->typeModule();
    addMethods(globalObject, module, {
        { "__new__"_s, moduleNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, moduleInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__getattribute__"_s, moduleGetAttribute },
        { "__repr__"_s, moduleRepr },
        { "__dir__"_s, moduleDir },
    });
    realm->setFunction(vm, PyRealm::WellKnownFunction::ModuleGetAttribute, asObject(module->lookupOwn(vm, vm.pythonNames().dunder_getattribute)));

    addMethods(globalObject, realm->typeGenerator(), {
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, generatorNextMethod },
        { "send"_s, generatorSendMethod },
        { "throw"_s, generatorThrowMethod, Kind::Method, 0, "($self, typ, val=None, tb=None, /)"_s },
        { "close"_s, generatorCloseMethod },
        { "__repr__"_s, nativeRepr },
    });
}

} } // namespace JSC::Python
