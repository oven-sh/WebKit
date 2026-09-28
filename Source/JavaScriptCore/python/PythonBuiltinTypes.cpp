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
#include "JSGenerator.h"
#include "PythonGenerators.h"
#include "UnlinkedFunctionExecutable.h"

// object, type, and the types of functions, methods, descriptors, modules and generators.

namespace JSC { namespace Python {

void addMethods(JSGlobalObject* globalObject, PyType* type, std::initializer_list<MethodDefinition> methods)
{
    VM& vm = globalObject->vm();
    for (auto& method : methods)
        type->putDirect(vm, Identifier::fromString(vm, method.name), PyNativeFunction::create(vm, globalObject, 0, String(method.name), method.function, method.kind, type, method.data));
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

PyNativeFunction* addFunction(JSGlobalObject* globalObject, JSObject* namespaceObject, ASCIILiteral name, NativeFunction function, unsigned data)
{
    VM& vm = globalObject->vm();
    auto* native = PyNativeFunction::create(vm, globalObject, 0, String(name), function, PyNativeFunction::Kind::Function, nullptr, data);
    namespaceObject->putDirect(vm, Identifier::fromString(vm, name), native);
    return native;
}

JSValue boxIfDerived(JSGlobalObject* globalObject, PyType* type, PyType* builtin, JSValue value)
{
    if (type == builtin)
        return value;
    return PyBoxedValue::create(globalObject->vm(), type->instanceStructure(), value);
}

// ---- What is the same for every built-in type

JSC_DEFINE_HOST_FUNCTION(nativeRepr, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    String text = builtinRepr(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

JSC_DEFINE_HOST_FUNCTION(nativeHash, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    int64_t hash = builtinHash(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, hash)));
}

JSC_DEFINE_HOST_FUNCTION(nativeLen, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    int64_t length = builtinLength(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, length)));
}

JSC_DEFINE_HOST_FUNCTION(nativeGetItem, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__getitem__"_s, 2, 2))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(builtinGetItem(globalObject, args[0], args[1])));
}

JSC_DEFINE_HOST_FUNCTION(nativeSetItem, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__setitem__"_s, 3, 3))
        return { };
    scope.release();
    builtinSetItem(globalObject, args[0], args[1], args[2]);
    RETURN_NONE();
}

JSC_DEFINE_HOST_FUNCTION(nativeDelItem, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__delitem__"_s, 2, 2))
        return { };
    scope.release();
    builtinSetItem(globalObject, args[0], args[1], JSValue());
    RETURN_NONE();
}

JSC_DEFINE_HOST_FUNCTION(nativeContains, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__contains__"_s, 2, 2))
        return { };
    auto result = builtinContains(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result.value_or(false)));
}

JSC_DEFINE_HOST_FUNCTION(nativeIter, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(builtinGetIterator(globalObject, args.at(0)));
}

JSC_DEFINE_HOST_FUNCTION(nativeNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    NATIVE_PROLOGUE();
    JSValue value = iteratorNext(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(value);
}

JSC_DEFINE_HOST_FUNCTION(nativeSelf, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(callFrame->argument(0));
}

PYTHON_NATIVE(nativeCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("expected 1 argument, got "_s, args.size() ? args.size() - 1 : 0)));
    JSValue result = builtinCompare(globalObject, op, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(result);
}

void addComparisons(JSGlobalObject* globalObject, PyType* type, bool ordering)
{
    addMethods(globalObject, type, {
        { "__eq__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Eq) },
        { "__ne__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::NotEq) },
    });
    if (!ordering)
        return;
    addMethods(globalObject, type, {
        { "__lt__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Lt) },
        { "__le__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::LtE) },
        { "__gt__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::Gt) },
        { "__ge__"_s, nativeCompare, PyNativeFunction::Kind::Method, pack(ComparisonOperator::GtE) },
    });
}

enum class Form : uint8_t { Plain, Reflected, InPlace };

PYTHON_NATIVE(nativeBinary)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    auto form = unpack<Form>(callFrame, 1);
    NATIVE_PROLOGUE();
    if (args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("expected 1 argument, got "_s, args.size() ? args.size() - 1 : 0)));
    JSValue left = form == Form::Reflected ? args[1] : args[0];
    JSValue right = form == Form::Reflected ? args[0] : args[1];
    JSValue result = builtinBinaryOperation(globalObject, op, form == Form::InPlace, left, right);
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(result);
}

void addBinaryOperators(JSGlobalObject* globalObject, PyType* type, std::initializer_list<BinaryOperator> operators, bool reflected, bool inPlace)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    auto add = [&] (const Identifier& name, BinaryOperator op, Form form) {
        type->putDirect(vm, name, PyNativeFunction::create(vm, globalObject, 1, name.string(), nativeBinary, PyNativeFunction::Kind::Method, type, pack(op, form)));
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
    if (!args.size() || !isType(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "object.__new__(X): X is not a type object"_s));
    auto* type = uncheckedDowncast<PyType>(args[0].asCell());
    if (hasExcessArguments(args)) {
        if (type->lookup(vm, names.dunder_new).asCell() != realm->function(PyRealm::WellKnownFunction::ObjectNew))
            return JSValue::encode(raiseTypeError(globalObject, scope, "object.__new__() takes exactly one argument (the type to instantiate)"_s));
        if (type->lookup(vm, names.dunder_init).asCell() == realm->function(PyRealm::WellKnownFunction::ObjectInit))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString(type->nameString(globalObject), "() takes no arguments"_s)));
    }
    if (type->layout() != PyType::Layout::Object)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("object.__new__("_s, type->nameString(globalObject), ") is not safe, use "_s, type->base()->nameString(globalObject), ".__new__()"_s)));
    return JSValue::encode(PyInstance::create(vm, type->instanceStructure()));
}

PYTHON_NATIVE(objectInit)
{
    NATIVE_PROLOGUE();
    if (hasExcessArguments(args)) {
        PyType* type = typeOf(globalObject, args[0]);
        if (type->lookup(vm, names.dunder_init).asCell() != realm->function(PyRealm::WellKnownFunction::ObjectInit))
            return JSValue::encode(raiseTypeError(globalObject, scope, "object.__init__() takes exactly one argument (the instance to initialize)"_s));
        if (type->lookup(vm, names.dunder_new).asCell() == realm->function(PyRealm::WellKnownFunction::ObjectNew))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString(type->nameString(globalObject), "() takes no arguments"_s)));
    }
    RETURN_NONE();
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
    JSValue method = typeOf(globalObject, args.at(0))->lookup(vm, names.dunder_eq);
    JSValue result = call(globalObject, method, args.at(0), args.at(1));
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

static std::optional<Identifier> attributeName(JSGlobalObject* globalObject, ThrowScope& scope, JSValue name)
{
    if (!name.isString()) {
        raiseTypeError(globalObject, scope, makeString("attribute name must be string, not '"_s, typeName(globalObject, name), '\''));
        return std::nullopt;
    }
    return asString(name)->toIdentifier(globalObject);
}

PYTHON_NATIVE(objectGetAttribute)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__getattribute__"_s, 2, 2))
        return { };
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = genericGetAttribute(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    if (isType(args[0]))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, makeString("type object '"_s, uncheckedDowncast<PyType>(args[0].asCell())->nameString(globalObject), "' has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, makeString('\'', typeName(globalObject, args[0]), "' object has no attribute '"_s, name->string(), '\'')));
}

PYTHON_NATIVE(objectSetAttr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__setattr__"_s, 3, 3))
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
    if (!args.check(globalObject, scope, "__delattr__"_s, 2, 2))
        return { };
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    genericSetAttribute(globalObject, args[0], *name, JSValue());
    RETURN_NONE();
}

PYTHON_NATIVE(objectFormat)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "__format__"_s, 2, 2))
        return { };
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("__format__() argument must be str, not "_s, typeName(globalObject, args[1]))));
    if (asString(args[1])->length())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("unsupported format string passed to "_s, typeName(globalObject, args[0]), ".__format__"_s)));
    String text = str(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(objectInitSubclass)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount()) {
        String name = args.size() && isType(args[0]) ? uncheckedDowncast<PyType>(args[0].asCell())->nameString(globalObject) : "object"_str;
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, ".__init_subclass__() takes no keyword arguments"_s)));
    }
    RETURN_NONE();
}

static void collectAttributeNames(JSGlobalObject* globalObject, JSObject* object, PySet* into)
{
    VM& vm = globalObject->vm();
    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->getOwnNonIndexPropertyNames(globalObject, properties, DontEnumPropertiesMode::Exclude);
    for (auto& name : properties)
        into->add(globalObject, jsString(vm, name.string()));
}

PYTHON_NATIVE(objectDir)
{
    NATIVE_PROLOGUE();
    JSValue self = args.at(0);
    PySet* found = PySet::create(globalObject);
    PyType* type = isType(self) ? uncheckedDowncast<PyType>(self.asCell()) : typeOf(globalObject, self);
    if (auto* module = tryModule(self))
        collectAttributeNames(globalObject, module->namespaceObject(), found);
    else {
        if (self.isObject() && !isType(self) && (self.asCell()->type() == PyInstanceType || type->hasFlag(PyType::IsHeapType)))
            collectAttributeNames(globalObject, asObject(self), found);
        for (auto& entry : type->mro()->span())
            collectAttributeNames(globalObject, asObject(entry.get()), found);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(listFromIterable(globalObject, found)));
}

static JSValue getClass(JSGlobalObject* globalObject, JSValue self)
{
    return typeOf(globalObject, self);
}

static void setClass(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isType(value)) {
        raiseTypeError(globalObject, scope, makeString("__class__ must be set to a class, not '"_s, value ? typeName(globalObject, value) : "NULL"_str, "' object"_s));
        return;
    }
    auto* newType = uncheckedDowncast<PyType>(value.asCell());
    PyType* oldType = typeOf(globalObject, self);
    if (!self.isObject() || !oldType->hasFlag(PyType::IsHeapType) || !newType->hasFlag(PyType::IsHeapType) || oldType->layout() != newType->layout()) {
        raiseTypeError(globalObject, scope, makeString("__class__ assignment: '"_s, newType->nameString(globalObject), "' object layout differs from '"_s, oldType->nameString(globalObject), '\''));
        return;
    }
    asObject(self)->setPrototypeDirect(vm, newType);
}

static JSValue getInstanceDict(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, self);
    if (auto* module = tryModule(self))
        return PyDict::backedBy(globalObject, module->namespaceObject());
    bool hasDict = self.isObject() && !type->hasFlag(PyType::HasNoInstanceDict) && (type->hasFlag(PyType::IsHeapType) || type->isExceptionType() || (self.asCell()->type() == JSFunctionType && !self.asCell()->inherits<PyNativeFunction>()));
    if (!hasDict)
        return raise(globalObject, scope, BuiltinType::AttributeError, makeString('\'', type->nameString(globalObject), "' object has no attribute '__dict__'"_s));
    return PyDict::backedBy(globalObject, asObject(self));
}

// obj.__dict__ = mapping: that dict is the attributes from now on, and the dict that was is a dict like any other.
static void setInstanceDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isDict(value)) {
        raiseTypeError(globalObject, scope, makeString("__dict__ must be set to a dictionary, not a '"_s, value ? typeName(globalObject, value) : "NULL"_str, '\''));
        return;
    }
    JSValue current = getInstanceDict(globalObject, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (current == value)
        return;
    JSObject* object = asDict(current)->backing();
    asDict(current)->detach(globalObject);
    if (!asDict(value)->backing()) {
        asDict(value)->becomeBackedBy(globalObject, object);
        return;
    }
    // FIXME: Two objects cannot have the same dict, since it is one of them that holds what is in it. This one gets a copy.
    PyDict::backedBy(globalObject, object)->copyFrom(globalObject, *asDict(value));
}

// ---- type

PYTHON_NATIVE(typeNew)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isType(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "type.__new__(X): X is not a type object"_s));
    auto* metatype = uncheckedDowncast<PyType>(args[0].asCell());
    if (args.size() == 2 && !args.keywordCount() && metatype == realm->typeType())
        return JSValue::encode(typeOf(globalObject, args[1]));
    if (args.size() != 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type.__new__() argument 1 must be str, not "_s, typeName(globalObject, args[1]))));
    if (!isTuple(args[2]))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type.__new__() argument 2 must be tuple, not "_s, typeName(globalObject, args[2]))));
    if (!isDict(args[3]))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type.__new__() argument 3 must be dict, not "_s, typeName(globalObject, args[3]))));

    PyDict* keywords = nullptr;
    if (args.keywordCount()) {
        keywords = PyDict::create(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            keywords->set(globalObject, args.keywordName(i), args.keywordValue(i));
    }
    // The most derived metaclass among the bases' has the last word.
    auto* bases = uncheckedDowncast<PyTuple>(args[2].asCell());
    for (auto& base : bases->span()) {
        PyType* candidate = typeOf(globalObject, base.get());
        if (candidate != metatype && candidate->isSubtypeOf(metatype))
            metatype = candidate;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newType(globalObject, metatype, asString(args[1]), bases, uncheckedDowncast<PyDict>(args[3].asCell()), keywords)));
}

PYTHON_NATIVE(typeInit)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

PYTHON_NATIVE(typeCall)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isType(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "descriptor '__call__' requires a 'type' object"_s));
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(instantiate(globalObject, uncheckedDowncast<PyType>(args[0].asCell()), arguments, args.keywordNames())));
}

PYTHON_NATIVE(typeMro)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(listFromIterable(globalObject, uncheckedDowncast<PyType>(args.at(0).asCell())->mro())));
}

PYTHON_NATIVE(typeSubclasses)
{
    NATIVE_PROLOGUE();
    // FIXME: A class does not know what is derived from it.
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject)));
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
    return JSValue::encode(jsBoolean(isInstance(globalObject, args.at(1), uncheckedDowncast<PyType>(args.at(0).asCell()))));
}

PYTHON_NATIVE(typeSubclassCheck)
{
    NATIVE_PROLOGUE();
    if (!isType(args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s));
    return JSValue::encode(jsBoolean(uncheckedDowncast<PyType>(args[1].asCell())->isSubtypeOf(uncheckedDowncast<PyType>(args[0].asCell()))));
}

static JSValue getOwnOr(JSGlobalObject* globalObject, JSValue self, const Identifier& name, JSValue otherwise)
{
    JSValue value = asType(self)->lookupOwn(globalObject->vm(), name);
    return value ? value : otherwise;
}

// ---- Functions

static JSFunction* asFunction(JSValue value) { return uncheckedDowncast<JSFunction>(value.asCell()); }

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
    asFunction(self)->putDirect(vm, qualified ? vm.pythonNames().private_qualname : vm.pythonNames().private_name, value);
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

FUNCTION_PROPERTY(getFunctionDefaults, setFunctionDefaults, private_defaults)
FUNCTION_PROPERTY(getFunctionKeywordDefaults, setFunctionKeywordDefaults, private_kwdefaults)
FUNCTION_PROPERTY(getFunctionDoc, setFunctionDoc, private_doc)
FUNCTION_PROPERTY(getFunctionModule, setFunctionModule, private_module)

PYTHON_NATIVE(functionGet)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    // function.__get__(instance, owner)
    if (isNone(args.at(1)))
        return JSValue::encode(args.at(0));
    return JSValue::encode(PyBoundMethod::create(globalObject, args[0], args[1]));
}

PYTHON_NATIVE(callableCall)
{
    NATIVE_PROLOGUE();
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, args.at(0), arguments, args.keywordNames())));
}

// ---- Bound methods

static PyBoundMethod* asMethod(JSValue value) { return uncheckedDowncast<PyBoundMethod>(value.asCell()); }

PYTHON_NATIVE(methodNew)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "method"_s, 3, 3))
        return { };
    return JSValue::encode(PyBoundMethod::create(globalObject, args[1], args[2]));
}

PYTHON_NATIVE(methodEq)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* other = tryBoundMethod(args.at(1));
    if (!other)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(jsBoolean(isIdentical(asMethod(args[0])->function(), other->function()) && isIdentical(asMethod(args[0])->self(), other->self())));
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

static PyNativeObject* asNativeObject(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }

static JSValue allocateNativeObject(JSGlobalObject* globalObject, ThrowScope& scope, JSValue type)
{
    if (!isType(type))
        return raiseTypeError(globalObject, scope, "__new__(X): X is not a type object"_s);
    return PyNativeObject::create(globalObject->vm(), asType(type)->instanceStructure());
}

PYTHON_NATIVE(nativeObjectNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(allocateNativeObject(globalObject, scope, args.at(0))));
}

// property(fget=None, fset=None, fdel=None, doc=None)
PYTHON_NATIVE(propertyInit)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    static constexpr ASCIILiteral keywords[] = { "fget"_s, "fset"_s, "fdel"_s, "doc"_s };
    auto* property = asNativeObject(args[0]);
    for (unsigned i = 0; i < 4; ++i) {
        JSValue value = args.at(i + 1);
        if (!value)
            value = args.keyword(globalObject, keywords[i]);
        property->setField(vm, i, value ? value : jsUndefined());
    }
    RETURN_NONE();
}

// @x.setter and its like: a copy, with one function replaced.
PYTHON_NATIVE(propertyWith)
{
    auto field = unpack<unsigned>(callFrame, 0);
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* original = asNativeObject(args.at(0));
    auto* copy = PyNativeObject::create(vm, original->structure());
    for (unsigned i = 0; i < PyNativeObject::numberOfFields; ++i)
        copy->setField(vm, i, i == field ? args.at(1) : original->field(i));
    return JSValue::encode(copy);
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
    JSValue instance = args.at(1);
    JSValue owner = args.at(2);
    PyType* type = owner && isType(owner) ? asType(owner) : typeOf(globalObject, instance ? instance : jsUndefined());
    RELEASE_AND_RETURN(scope, JSValue::encode(bindDescriptor(globalObject, args.at(0), !instance || isNone(instance) ? JSValue() : instance, type)));
}

PYTHON_NATIVE(propertySet)
{
    NATIVE_PROLOGUE();
    JSValue setter = asNativeObject(args.at(0))->field(1);
    if (!setter || isNone(setter))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, "property has no setter"_s));
    scope.release();
    call(globalObject, setter, args.at(1), args.at(2));
    RETURN_NONE();
}

PYTHON_NATIVE(propertyDelete)
{
    NATIVE_PROLOGUE();
    JSValue deleter = asNativeObject(args.at(0))->field(2);
    if (!deleter || isNone(deleter))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, "property has no deleter"_s));
    scope.release();
    call(globalObject, deleter, args.at(1));
    RETURN_NONE();
}

// staticmethod(function) and classmethod(function)
PYTHON_NATIVE(wrapperInit)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "staticmethod"_s, 2, 2))
        return { };
    asNativeObject(args[0])->setField(vm, 0, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(staticMethodCall)
{
    NATIVE_PROLOGUE();
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, asNativeObject(args.at(0))->field(0), arguments, args.keywordNames())));
}

// ---- super

// super(type, object_or_type). With no arguments, the compiler has filled them in.
PYTHON_NATIVE(superInit)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "super"_s))
        return { };
    auto* object = asNativeObject(args[0]);
    if (args.size() == 1)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "super(): no arguments"_s));
    if (!isType(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("super() argument 1 must be a type, not "_s, typeName(globalObject, args[1]))));
    PyType* type = asType(args[1]);
    object->setField(vm, 0, type);
    if (args.size() == 2)
        RETURN_NONE();
    JSValue instance = args[2];
    PyType* start;
    if (isType(instance) && asType(instance)->isSubtypeOf(type))
        start = asType(instance);
    else if (isInstance(globalObject, instance, type))
        start = typeOf(globalObject, instance);
    else
        return JSValue::encode(raiseTypeError(globalObject, scope, "super(type, obj): obj must be an instance or subtype of type"_s));
    object->setField(vm, 1, instance);
    object->setField(vm, 2, start);
    RETURN_NONE();
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
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AttributeError, makeString("'super' object has no attribute '"_s, name->string(), '\'')));
    return JSValue::encode(value);
}

// ---- Modules

PYTHON_NATIVE(moduleNew)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2 || !args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "module() argument 'name' must be str"_s));
    return JSValue::encode(PyModule::create(globalObject, asString(args[1])->value(globalObject)));
}

// ---- Generators

static JSGenerator* asGenerator(JSValue value) { return uncheckedDowncast<JSGenerator>(value.asCell()); }

PYTHON_NATIVE(generatorSendMethod)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "send"_s, 2, 2))
        return { };
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
    if (!args.check(globalObject, scope, "throw"_s, 2, 4))
        return { };
    JSValue exception = args[1];
    if (isType(exception) && asType(exception)->isExceptionType()) {
        JSValue value = args.at(2);
        if (value && typeOf(globalObject, value)->isExceptionType())
            exception = value;
        else
            exception = value && !isNone(value) ? call(globalObject, exception, value) : call(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (!typeOf(globalObject, exception)->isExceptionType())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("exceptions must be classes or instances deriving from BaseException, not "_s, typeName(globalObject, exception))));
    RELEASE_AND_RETURN(scope, JSValue::encode(generatorThrow(globalObject, asGenerator(args[0]), exception)));
}

PYTHON_NATIVE(generatorCloseMethod)
{
    NATIVE_PROLOGUE();
    scope.release();
    generatorClose(globalObject, asGenerator(args.at(0)));
    RETURN_NONE();
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
        { "__new__"_s, objectNew, Kind::Function },
        { "__init__"_s, objectInit },
        { "__repr__"_s, nativeRepr },
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
    addGetSet(globalObject, object, "__dict__"_s, getInstanceDict, setInstanceDict);
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
        { "__new__"_s, typeNew, Kind::Function },
        { "__init__"_s, typeInit },
        { "__call__"_s, typeCall },
        { "__getattribute__"_s, objectGetAttribute },
        { "__setattr__"_s, objectSetAttr },
        { "__delattr__"_s, objectDelAttr },
        { "__repr__"_s, nativeRepr },
        { "__dir__"_s, objectDir },
        { "mro"_s, typeMro },
        { "__subclasses__"_s, typeSubclasses },
        { "__prepare__"_s, typePrepare, Kind::ClassMethod },
        { "__instancecheck__"_s, typeInstanceCheck },
        { "__subclasscheck__"_s, typeSubclassCheck },
    });
    remember(type, names.dunder_call, Function::TypeCall);
    remember(type, names.dunder_getattribute, Function::TypeGetAttribute);
    remember(type, names.dunder_setattr, Function::TypeSetAttr);
    remember(type, names.dunder_delattr, Function::TypeDelAttr);
    addGetSet(globalObject, type, "__name__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->name(); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        if (value && value.isString())
            asType(self)->setName(globalObject->vm(), asString(value));
    });
    addGetSet(globalObject, type, "__qualname__"_s, [] (JSGlobalObject* globalObject, JSValue self) { return getOwnOr(globalObject, self, globalObject->vm().pythonNames().dunder_qualname, asType(self)->name()); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        if (value)
            asType(self)->putDirect(globalObject->vm(), globalObject->vm().pythonNames().dunder_qualname, value);
    });
    addGetSet(globalObject, type, "__module__"_s, [] (JSGlobalObject* globalObject, JSValue self) { return getOwnOr(globalObject, self, globalObject->vm().pythonNames().dunder_module, jsNontrivialString(globalObject->vm(), "builtins"_s)); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        if (value)
            asType(self)->putDirect(globalObject->vm(), globalObject->vm().pythonNames().dunder_module, value);
    });
    addGetSet(globalObject, type, "__bases__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->bases(); });
    addGetSet(globalObject, type, "__base__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->base() ? JSValue(asType(self)->base()) : jsUndefined(); });
    addGetSet(globalObject, type, "__mro__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asType(self)->mro(); });
    addGetSet(globalObject, type, "__dict__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return PyNativeObject::create(globalObject, BuiltinType::MappingProxy, PyDict::backedBy(globalObject, asType(self))); });
    addGetSet(globalObject, type, "__doc__"_s, [] (JSGlobalObject* globalObject, JSValue self) { return getOwnOr(globalObject, self, globalObject->vm().pythonNames().dunder_doc, jsUndefined()); });

    addMethods(globalObject, realm->typeNoneType(), {
        { "__repr__"_s, nativeRepr },
        { "__bool__"_s, returnFalse },
        { "__hash__"_s, nativeHash },
    });
    addMethods(globalObject, realm->typeNotImplementedType(), { { "__repr__"_s, nativeRepr } });
    addMethods(globalObject, realm->typeEllipsis(), { { "__repr__"_s, nativeRepr } });
}

void initializeFunctionTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    // What the rest of the setting up is done with comes first.
    for (PyType* type : { realm->typeProperty(), realm->typeStaticMethod(), realm->typeClassMethod(), realm->typeSuper(), realm->typeMemberDescriptor(), realm->typeNotImplementedType(), realm->typeEllipsis(), realm->typeDictKeys(), realm->typeDictValues(), realm->typeDictItems(), realm->typeMappingProxy() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));
    realm->typeGetSetDescriptor()->setInstanceStructure(vm, PyGetSetDescriptor::createStructure(vm, globalObject, realm->typeGetSetDescriptor()));

    for (PyType* type : { realm->typeFunction(), realm->typeBuiltinFunction(), realm->typeMethodDescriptor(), realm->typeClassMethodDescriptor() }) {
        addMethods(globalObject, type, {
            { "__repr__"_s, nativeRepr },
            { "__call__"_s, callableCall },
        });
        addGetSet(globalObject, type, "__name__"_s, getFunctionName<false>, setFunctionName<false>);
        addGetSet(globalObject, type, "__qualname__"_s, getFunctionName<true>, setFunctionName<true>);
        addGetSet(globalObject, type, "__doc__"_s, getFunctionDoc, setFunctionDoc);
    }
    PyType* function = realm->typeFunction();
    addMethods(globalObject, function, { { "__get__"_s, functionGet } });
    addMethods(globalObject, realm->typeMethodDescriptor(), { { "__get__"_s, functionGet } });
    addGetSet(globalObject, function, "__defaults__"_s, getFunctionDefaults, setFunctionDefaults);
    addGetSet(globalObject, function, "__kwdefaults__"_s, getFunctionKeywordDefaults, setFunctionKeywordDefaults);
    addGetSet(globalObject, function, "__module__"_s, getFunctionModule, setFunctionModule);
    UNUSED_PARAM(infoOf);

    PyType* method = realm->typeMethod();
    method->setInstanceStructure(vm, PyBoundMethod::createStructure(vm, globalObject, method));
    addMethods(globalObject, method, {
        { "__new__"_s, methodNew, Kind::Function },
        { "__repr__"_s, nativeRepr },
        { "__call__"_s, callableCall },
        { "__eq__"_s, methodEq },
        { "__getattribute__"_s, methodGetAttribute },
    });
    addMember(globalObject, method, "__func__"_s, [] (JSGlobalObject*, JSValue self) { return asMethod(self)->function(); });
    addMember(globalObject, method, "__self__"_s, [] (JSGlobalObject*, JSValue self) { return asMethod(self)->self(); });

    PyType* property = realm->typeProperty();
    addMethods(globalObject, property, {
        { "__new__"_s, nativeObjectNew, Kind::Function },
        { "__init__"_s, propertyInit },
        { "__get__"_s, descriptorGet },
        { "__set__"_s, propertySet },
        { "__delete__"_s, propertyDelete },
        { "getter"_s, propertyWith, PyNativeFunction::Kind::Method, pack(0) },
        { "setter"_s, propertyWith, PyNativeFunction::Kind::Method, pack(1) },
        { "deleter"_s, propertyWith, PyNativeFunction::Kind::Method, pack(2) },
    });
    addMember(globalObject, property, "fget"_s, getField<0>);
    addMember(globalObject, property, "fset"_s, getField<1>);
    addMember(globalObject, property, "fdel"_s, getField<2>);
    addGetSet(globalObject, property, "__doc__"_s, getField<3>);

    for (PyType* type : { realm->typeStaticMethod(), realm->typeClassMethod() }) {
        addMethods(globalObject, type, {
            { "__new__"_s, nativeObjectNew, Kind::Function },
            { "__init__"_s, wrapperInit },
            { "__get__"_s, descriptorGet },
        });
        addMember(globalObject, type, "__func__"_s, getField<0>);
        addMember(globalObject, type, "__wrapped__"_s, getField<0>);
    }
    addMethods(globalObject, realm->typeStaticMethod(), { { "__call__"_s, staticMethodCall } });

    addMethods(globalObject, realm->typeSuper(), {
        { "__new__"_s, nativeObjectNew, Kind::Function },
        { "__init__"_s, superInit },
        { "__getattribute__"_s, superGetAttribute },
    });

    PyType* module = realm->typeModule();
    module->setInstanceStructure(vm, PyModule::createStructure(vm, globalObject, module));
    addMethods(globalObject, module, {
        { "__new__"_s, moduleNew, Kind::Function },
        { "__repr__"_s, nativeRepr },
        { "__dir__"_s, objectDir },
    });

    addMethods(globalObject, realm->typeGenerator(), {
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, generatorNextMethod },
        { "send"_s, generatorSendMethod },
        { "throw"_s, generatorThrowMethod },
        { "close"_s, generatorCloseMethod },
        { "__repr__"_s, nativeRepr },
    });
}

} } // namespace JSC::Python
