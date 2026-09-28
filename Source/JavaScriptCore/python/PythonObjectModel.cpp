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
#include "PythonOperations.h"

#include "CodeBlock.h"
#include "ErrorInstance.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "ObjectConstructor.h"
#include "PythonNumbers.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionExecutable.h"

// The object model: what type a value has, exceptions, attributes and calls.

namespace JSC { namespace Python {

// ---- Types

bool isList(JSValue value)
{
    return value.isCell() && isJSArray(value.asCell());
}

static PyType* typeOfError(PyRealm* realm, ErrorInstance* error)
{
    switch (error->errorType()) {
    case ErrorType::TypeError:
        return realm->typeTypeError();
    case ErrorType::RangeError:
        return error->isStackOverflowError() ? realm->typeRecursionError() : realm->typeValueError();
    case ErrorType::ReferenceError:
        return realm->typeNameError();
    case ErrorType::SyntaxError:
        return realm->typeSyntaxError();
    default:
        return realm->typeJSError();
    }
}

PyType* typeOf(JSGlobalObject* globalObject, JSValue value)
{
    PyRealm* realm = globalObject->pyRealm();
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        switch (cell->type()) {
        case StringType:
            return realm->typeStr();
        case HeapBigIntType:
            return realm->typeInt();
        case SymbolType:
            return realm->typeJSSymbol();
        case PyTypeType:
            return uncheckedDowncast<PyType>(cell)->metatype();
        case JSFunctionType:
            if (auto* native = dynamicDowncast<PyNativeFunction>(cell)) {
                switch (native->kind()) {
                case PyNativeFunction::Kind::Function:
                case PyNativeFunction::Kind::New:
                    return realm->typeBuiltinFunction();
                case PyNativeFunction::Kind::Method:
                    return realm->typeMethodDescriptor();
                case PyNativeFunction::Kind::ClassMethod:
                    return realm->typeClassMethodDescriptor();
                }
            }
            return realm->typeFunction();
        default:
            break;
        }
        if (!cell->isObject())
            return realm->typeJSObject();
        // Anything whose prototype is a class is an instance of it.
        JSValue prototype = cell->structure()->storedPrototype(asObject(cell));
        if (isType(prototype))
            return uncheckedDowncast<PyType>(prototype.asCell());
        if (isJSArray(cell))
            return realm->typeList();
        if (cell->type() == Uint8ArrayType)
            return realm->typeByteArray();
        if (cell->type() == JSGeneratorType)
            return realm->typeGenerator();
        if (cell->type() == ErrorInstanceType)
            return typeOfError(realm, uncheckedDowncast<ErrorInstance>(cell));
        return realm->typeJSObject();
    }
    if (value.isNumber())
        return taggedInteger(value) ? realm->typeInt() : realm->typeFloat();
    if (value.isBoolean())
        return realm->typeBool();
    ASSERT(value.isUndefinedOrNull());
    return realm->typeNoneType();
}

bool isInstance(JSGlobalObject* globalObject, JSValue value, PyType* type)
{
    return typeOf(globalObject, value)->isSubtypeOf(type);
}

String typeName(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value)->nameString(globalObject);
}

JSValue getMember(JSGlobalObject* globalObject, JSObject* object, const String& name)
{
    VM& vm = globalObject->vm();
    return object->getDirect(vm, Identifier::fromString(vm, makeString('.', name)));
}

void setMember(JSGlobalObject* globalObject, JSObject* object, const String& name, JSValue value)
{
    VM& vm = globalObject->vm();
    object->putDirect(vm, Identifier::fromString(vm, makeString('.', name)), value, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// ---- Exceptions

JSObject* createException(JSGlobalObject* globalObject, PyType* type, JSValue argument)
{
    VM& vm = globalObject->vm();
    PyInstance* exception = PyInstance::create(vm, type->instanceStructure());
    exception->putDirect(vm, vm.pythonNames().private_args, argument ? PyTuple::create(globalObject, { argument }) : PyTuple::create(globalObject, 0));
    // What its __init__ would have done.
    if (argument && type == globalObject->pyRealm()->typeStopIteration())
        setMember(globalObject, exception, "value"_s, argument);
    return exception;
}

JSObject* createException(JSGlobalObject* globalObject, PyType* type, const String& message)
{
    return createException(globalObject, type, jsString(globalObject->vm(), message));
}

JSValue raise(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type, JSValue argument)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* exception = createException(globalObject, realm->type(type), argument);
    UNUSED_PARAM(vm);
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

void setContext(JSGlobalObject* globalObject, JSObject* exception)
{
    VM& vm = globalObject->vm();
    auto& name = vm.pythonNames().private_context;
    JSValue handled = globalObject->pyRealm()->handledException();
    if (!handled || !handled.isObject() || handled == JSValue(exception))
        return;
    // If it is already somewhere in the chain of what is being handled, it is taken out, or the chain would go round.
    for (JSObject* link = asObject(handled);;) {
        JSValue next = link->getDirect(vm, name);
        if (!next || !next.isObject())
            break;
        if (next == JSValue(exception)) {
            link->putDirect(vm, name, jsUndefined());
            break;
        }
        link = asObject(next);
    }
    exception->putDirect(vm, name, handled);
}

JSValue raise(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type, const String& message)
{
    return raise(globalObject, scope, type, jsString(globalObject->vm(), message));
}

JSValue raiseNameError(JSGlobalObject* globalObject, ThrowScope& scope, const String& name)
{
    JSObject* exception = createException(globalObject, globalObject->pyRealm()->typeNameError(), makeString("name '"_s, name, "' is not defined"_s));
    setMember(globalObject, exception, "name"_s, jsString(globalObject->vm(), name));
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

void throwUnboundVariable(JSGlobalObject* globalObject, CodeBlock* codeBlock, JSString* name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String string = name->value(globalObject);
    bool isFree = false;
    if (auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable())) {
        if (auto* info = executable->unlinkedExecutable()->pythonInfo()) {
            for (auto& free : info->freeVariables)
                isFree |= free.string() == string;
        }
    }
    if (isFree)
        raise(globalObject, scope, BuiltinType::NameError, makeString("cannot access free variable '"_s, string, "' where it is not associated with a value in enclosing scope"_s));
    else
        raise(globalObject, scope, BuiltinType::UnboundLocalError, makeString("cannot access local variable '"_s, string, "' where it is not associated with a value"_s));
}

bool catchException(JSGlobalObject* globalObject, BuiltinType type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    if (!exception || vm.isTerminationException(exception))
        return false;
    if (!isInstance(globalObject, exception->value(), globalObject->pyRealm()->type(type)))
        return false;
    scope.clearException();
    return true;
}

JSValue exceptionValue(JSGlobalObject*, JSValue thrown)
{
    return thrown;
}

// ---- Descriptors

namespace {

// What an attribute found in a class does when it is got from an instance.
enum class DescriptorKind : uint8_t {
    Plain, // Nothing: it is the value.
    Function, // Becomes a bound method.
    NativeClassMethod,
    GetSet,
    Property,
    StaticMethod,
    ClassMethod,
    Member, // One of __slots__.
    General, // Its own class has __get__.
};

struct Descriptor {
    DescriptorKind kind { DescriptorKind::Plain };
    bool isData { false }; // It has __set__ or __delete__, and so comes before what the instance itself has.
    JSValue getter; // __get__, if General.
};

Descriptor classifyDescriptor(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell())
        return { };
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSCell* cell = value.asCell();
    switch (cell->type()) {
    case JSFunctionType:
        if (auto* native = dynamicDowncast<PyNativeFunction>(cell)) {
            switch (native->kind()) {
            case PyNativeFunction::Kind::Function:
            case PyNativeFunction::Kind::New:
                return { };
            case PyNativeFunction::Kind::Method:
                return { DescriptorKind::Function, false, { } };
            case PyNativeFunction::Kind::ClassMethod:
                return { DescriptorKind::NativeClassMethod, false, { } };
            }
        }
        return { DescriptorKind::Function, false, { } };
    case StringType:
    case HeapBigIntType:
    case SymbolType:
    case PyTupleType:
    case PyDictType:
    case PySetType:
    case PyTypeType:
    case PyBoundMethodType:
    case PyModuleType:
        return { };
    default:
        break;
    }
    if (!cell->isObject())
        return { };
    if (cell->inherits<PyGetSetDescriptor>())
        return { DescriptorKind::GetSet, true, { } };

    PyType* type = typeOf(globalObject, value);
    if (type == realm->typeProperty())
        return { DescriptorKind::Property, true, { } };
    if (type == realm->typeStaticMethod())
        return { DescriptorKind::StaticMethod, false, { } };
    if (type == realm->typeClassMethod())
        return { DescriptorKind::ClassMethod, false, { } };
    if (type == realm->typeMemberDescriptor())
        return { DescriptorKind::Member, true, { } };
    if (type == realm->typeJSObject() || type == realm->typeList())
        return { };

    auto& names = vm.pythonNames();
    JSValue getter = type->lookup(vm, names.dunder_get);
    bool isData = type->lookup(vm, names.dunder_set) || type->lookup(vm, names.dunder_delete);
    if (!getter && !isData)
        return { };
    return { DescriptorKind::General, isData, getter };
}

JSValue bind(JSGlobalObject* globalObject, const Descriptor& descriptor, JSValue value, JSValue instance, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (descriptor.kind) {
    case DescriptorKind::Plain:
        return value;
    case DescriptorKind::Function:
        if (!instance)
            return value;
        return PyBoundMethod::create(globalObject, value, instance);
    case DescriptorKind::NativeClassMethod:
        return PyBoundMethod::create(globalObject, value, type);
    case DescriptorKind::GetSet:
        if (!instance)
            return value;
        RELEASE_AND_RETURN(scope, uncheckedDowncast<PyGetSetDescriptor>(value.asCell())->getter()(globalObject, instance));
    case DescriptorKind::Property: {
        if (!instance)
            return value;
        JSValue getter = uncheckedDowncast<PyNativeObject>(value.asCell())->field(0);
        if (isNone(getter) || !getter)
            return raise(globalObject, scope, BuiltinType::AttributeError, "property has no getter"_s);
        RELEASE_AND_RETURN(scope, call(globalObject, getter, instance));
    }
    case DescriptorKind::StaticMethod:
        return uncheckedDowncast<PyNativeObject>(value.asCell())->field(0);
    case DescriptorKind::ClassMethod: {
        JSValue function = uncheckedDowncast<PyNativeObject>(value.asCell())->field(0);
        return PyBoundMethod::create(globalObject, function, type);
    }
    case DescriptorKind::Member: {
        if (!instance)
            return value;
        auto* member = uncheckedDowncast<PyNativeObject>(value.asCell());
        String name = asString(member->field(0))->value(globalObject);
        if (JSValue stored = getMember(globalObject, asObject(instance), name))
            return stored;
        // One of a built-in type has a value from the start.
        if (JSValue initial = member->field(2))
            return initial;
        return raise(globalObject, scope, BuiltinType::AttributeError, makeString('\'', type->nameString(globalObject), "' object has no attribute '"_s, name, '\''));
    }
    case DescriptorKind::General:
        if (!descriptor.getter)
            return value;
        RELEASE_AND_RETURN(scope, call(globalObject, descriptor.getter, value, instance ? instance : jsUndefined(), type));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Where an object's own attributes are, if it has any. They are the properties of this.
JSObject* attributeStorage(JSGlobalObject* globalObject, JSValue value, PyType* type)
{
    if (!value.isObject())
        return nullptr;
    JSObject* object = asObject(value);
    switch (object->type()) {
    case PyInstanceType:
        return type->hasFlag(PyType::HasNoInstanceDict) ? nullptr : object;
    case PyModuleType:
        return uncheckedDowncast<PyModule>(object)->namespaceObject();
    case JSFunctionType:
        return object->inherits<PyNativeFunction>() ? nullptr : object;
    case PyTypeType:
        return nullptr;
    default:
        break;
    }
    // An instance of a class that is written in Python and derived from a built-in one.
    if (type->hasFlag(PyType::IsHeapType) && !type->hasFlag(PyType::HasNoInstanceDict))
        return object;
    UNUSED_PARAM(globalObject);
    return nullptr;
}

bool isJavaScriptObject(JSGlobalObject* globalObject, PyType* type)
{
    PyRealm* realm = globalObject->pyRealm();
    return type == realm->typeJSObject() || type == realm->typeJSError();
}

JSValue nameAsString(VM& vm, PropertyName name)
{
    return jsString(vm, String(name.uid()));
}

// type.__getattribute__. Empty, with nothing raised, if there is no such attribute.
JSValue getTypeAttribute(JSGlobalObject* globalObject, PyType* type, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* metatype = type->metatype();
    JSValue metaAttribute = metatype->lookup(vm, name);
    Descriptor metaDescriptor;
    if (metaAttribute) {
        metaDescriptor = classifyDescriptor(globalObject, metaAttribute);
        if (metaDescriptor.isData)
            RELEASE_AND_RETURN(scope, bind(globalObject, metaDescriptor, metaAttribute, type, metatype));
    }
    if (JSValue attribute = type->lookup(vm, name))
        RELEASE_AND_RETURN(scope, bind(globalObject, classifyDescriptor(globalObject, attribute), attribute, JSValue(), type));
    if (metaAttribute)
        RELEASE_AND_RETURN(scope, bind(globalObject, metaDescriptor, metaAttribute, type, metatype));
    return { };
}

// object.__getattribute__, likewise.
JSValue getObjectAttribute(JSGlobalObject* globalObject, JSValue value, PyType* type, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue attribute = type->lookup(vm, name);
    Descriptor descriptor;
    if (attribute) {
        descriptor = classifyDescriptor(globalObject, attribute);
        if (descriptor.isData)
            RELEASE_AND_RETURN(scope, bind(globalObject, descriptor, attribute, value, type));
    }
    if (JSObject* storage = attributeStorage(globalObject, value, type)) {
        if (JSValue own = storage->getDirect(vm, name))
            return own;
    } else if (isJavaScriptObject(globalObject, type) && value.isObject()) {
        // An attribute of a JavaScript object is a property of it, as JavaScript finds it.
        PropertySlot slot(value, PropertySlot::InternalMethodType::Get);
        bool found = asObject(value)->getPropertySlot(globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, { });
        if (found)
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, name));
    }
    if (attribute)
        RELEASE_AND_RETURN(scope, bind(globalObject, descriptor, attribute, value, type));
    return { };
}

JSValue raiseNoAttribute(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    StringView attribute { name.uid() };
    String message;
    if (isType(value))
        message = makeString("type object '"_s, uncheckedDowncast<PyType>(value.asCell())->nameString(globalObject), "' has no attribute '"_s, attribute, '\'');
    else if (auto* module = tryModule(value)) {
        JSValue moduleName = module->namespaceObject()->getDirect(vm, vm.pythonNames().dunder_name);
        if (moduleName && moduleName.isString())
            message = makeString("module '"_s, asString(moduleName)->value(globalObject).data, "' has no attribute '"_s, attribute, '\'');
    }
    if (message.isNull())
        message = makeString('\'', typeName(globalObject, value), "' object has no attribute '"_s, attribute, '\'');
    JSObject* exception = createException(globalObject, globalObject->pyRealm()->typeAttributeError(), message);
    setMember(globalObject, exception, "name"_s, jsString(vm, attribute.toString()));
    setMember(globalObject, exception, "obj"_s, value);
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

} // anonymous namespace

JSValue bindDescriptor(JSGlobalObject* globalObject, JSValue descriptor, JSValue instance, PyType* type)
{
    return bind(globalObject, classifyDescriptor(globalObject, descriptor), descriptor, instance, type);
}

// ---- Attributes

JSValue genericGetAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    if (isType(value))
        return getTypeAttribute(globalObject, uncheckedDowncast<PyType>(value.asCell()), name);
    return getObjectAttribute(globalObject, value, typeOf(globalObject, value), name);
}

JSValue getAttributeIfPresent(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = typeOf(globalObject, value);
    unsigned hooks = type->hooks(globalObject);

    JSValue result;
    if (hooks & PyType::HasCustomGetAttribute) [[unlikely]] {
        result = call(globalObject, type->lookup(vm, names.dunder_getattribute), value, nameAsString(vm, name));
        if (scope.exception() && !catchException(globalObject, BuiltinType::AttributeError))
            return { };
    } else {
        result = genericGetAttribute(globalObject, value, name);
        // A property's getter may say there is no such attribute, and __getattr__ gets its say then too.
        if (scope.exception() && (!(hooks & PyType::HasGetAttr) || !catchException(globalObject, BuiltinType::AttributeError)))
            return { };
    }
    if (result || !(hooks & PyType::HasGetAttr))
        return result;

    result = call(globalObject, type->lookup(vm, names.dunder_getattr), value, nameAsString(vm, name));
    if (scope.exception())
        catchException(globalObject, BuiltinType::AttributeError);
    return result;
}

JSValue getAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = typeOf(globalObject, value);
    unsigned hooks = type->hooks(globalObject);

    JSValue result;
    if (hooks & PyType::HasCustomGetAttribute) [[unlikely]]
        result = call(globalObject, type->lookup(vm, names.dunder_getattribute), value, nameAsString(vm, name));
    else
        result = genericGetAttribute(globalObject, value, name);

    if (scope.exception()) [[unlikely]] {
        if (!(hooks & PyType::HasGetAttr) || !catchException(globalObject, BuiltinType::AttributeError))
            return { };
        result = { };
    }
    if (result) [[likely]]
        return result;
    if (hooks & PyType::HasGetAttr)
        RELEASE_AND_RETURN(scope, call(globalObject, type->lookup(vm, names.dunder_getattr), value, nameAsString(vm, name)));
    return raiseNoAttribute(globalObject, scope, value, name);
}

// object.__setattr__ and object.__delattr__, and type's. `newValue` is empty to delete.
void genericSetAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = typeOf(globalObject, value);
    StringView attribute { name.uid() };

    JSValue found = type->lookup(vm, name);
    if (found) {
        Descriptor descriptor = classifyDescriptor(globalObject, found);
        switch (descriptor.kind) {
        case DescriptorKind::GetSet: {
            auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(found.asCell());
            if (!getSet->setter()) {
                if (getSet->isMember())
                    raise(globalObject, scope, BuiltinType::AttributeError, "readonly attribute"_s);
                else
                    raise(globalObject, scope, BuiltinType::AttributeError, makeString("attribute '"_s, attribute, "' of '"_s, getSet->owner()->nameString(globalObject), "' objects is not writable"_s));
                return;
            }
            RELEASE_AND_RETURN(scope, getSet->setter()(globalObject, value, newValue));
        }
        case DescriptorKind::Property: {
            JSValue function = uncheckedDowncast<PyNativeObject>(found.asCell())->field(newValue ? 1 : 2);
            if (!function || isNone(function)) {
                raise(globalObject, scope, BuiltinType::AttributeError, makeString("property '"_s, attribute, "' of '"_s, type->nameString(globalObject), "' object has no "_s, newValue ? "setter"_s : "deleter"_s));
                return;
            }
            scope.release();
            if (newValue)
                call(globalObject, function, value, newValue);
            else
                call(globalObject, function, value);
            return;
        }
        case DescriptorKind::Member:
            if (newValue) {
                setMember(globalObject, asObject(value), attribute.toString(), newValue);
                return;
            }
            if (!getMember(globalObject, asObject(value), attribute.toString())) {
                // Deleting an empty slot says only which.
                raise(globalObject, scope, BuiltinType::AttributeError, attribute.toString());
                return;
            }
            scope.release();
            asObject(value)->deleteProperty(globalObject, Identifier::fromString(vm, makeString('.', attribute)));
            return;
        case DescriptorKind::General:
            if (descriptor.isData) {
                JSValue function = typeOf(globalObject, found)->lookup(vm, newValue ? names.dunder_set : names.dunder_delete);
                if (!function) {
                    raise(globalObject, scope, BuiltinType::AttributeError, newValue ? "__set__"_s : "__delete__"_s);
                    return;
                }
                scope.release();
                if (newValue)
                    call(globalObject, function, found, value, newValue);
                else
                    call(globalObject, function, found, value);
                return;
            }
            break;
        default:
            break;
        }
    }

    if (isType(value)) {
        auto* target = uncheckedDowncast<PyType>(value.asCell());
        if (!target->hasFlag(PyType::IsHeapType)) {
            raiseTypeError(globalObject, scope, makeString("cannot set '"_s, attribute, "' attribute of immutable type '"_s, target->nameString(globalObject), '\''));
            return;
        }
        if (newValue) {
            target->setAttribute(vm, name, newValue);
            return;
        }
        bool deleted = target->deleteAttribute(vm, globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (!deleted)
            raise(globalObject, scope, BuiltinType::AttributeError, makeString("type object '"_s, target->nameString(globalObject), "' has no attribute '"_s, attribute, '\''));
        return;
    }

    if (JSObject* storage = attributeStorage(globalObject, value, type)) {
        if (newValue) {
            storage->putDirect(vm, name, newValue);
            return;
        }
        if (!storage->getDirect(vm, name)) {
            raiseNoAttribute(globalObject, scope, value, name);
            return;
        }
        scope.release();
        storage->deleteProperty(globalObject, name);
        return;
    }

    if (isJavaScriptObject(globalObject, type) && value.isObject()) {
        scope.release();
        if (newValue) {
            PutPropertySlot slot(value, true);
            asObject(value)->methodTable()->put(asObject(value), globalObject, name, newValue, slot);
        } else
            JSCell::deleteProperty(asObject(value), globalObject, name);
        return;
    }

    if (found) {
        raise(globalObject, scope, BuiltinType::AttributeError, makeString('\'', type->nameString(globalObject), "' object attribute '"_s, attribute, "' is read-only"_s));
        return;
    }
    if (newValue) {
        raise(globalObject, scope, BuiltinType::AttributeError, makeString('\'', type->nameString(globalObject), "' object has no attribute '"_s, attribute, "' and no __dict__ for setting new attributes"_s));
        return;
    }
    raiseNoAttribute(globalObject, scope, value, name);
}

void setAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue newValue)
{
    VM& vm = globalObject->vm();
    PyType* type = typeOf(globalObject, value);
    if (type->hooks(globalObject) & PyType::HasCustomSetAttr) [[unlikely]] {
        call(globalObject, type->lookup(vm, vm.pythonNames().dunder_setattr), value, nameAsString(vm, name), newValue);
        return;
    }
    genericSetAttribute(globalObject, value, name, newValue);
}

void deleteAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    PyType* type = typeOf(globalObject, value);
    if (type->hooks(globalObject) & PyType::HasCustomSetAttr) [[unlikely]] {
        call(globalObject, type->lookup(vm, vm.pythonNames().dunder_delattr), value, nameAsString(vm, name));
        return;
    }
    genericSetAttribute(globalObject, value, name, JSValue());
}

JSValue loadMethod(JSGlobalObject* globalObject, JSValue base, PropertyName name, JSValue& self)
{
    VM& vm = globalObject->vm();
    self = { };
    PyType* type = typeOf(globalObject, base);
    if (type == globalObject->pyRealm()->typeSuper()) {
        // super().method(...): the function from further along, and the instance that super() was made for.
        auto* object = uncheckedDowncast<PyNativeObject>(base.asCell());
        JSValue start = object->field(2);
        JSValue instance = object->field(1);
        if (start && instance != start) {
            JSValue attribute = asType(start)->lookupAfter(vm, asType(object->field(0)), name);
            if (attribute && classifyDescriptor(globalObject, attribute).kind == DescriptorKind::Function) {
                self = instance;
                return attribute;
            }
        }
    }
    if (isType(base) || (type->hooks(globalObject) & PyType::HasCustomGetAttribute))
        return getAttribute(globalObject, base, name);
    JSValue attribute = type->lookup(vm, name);
    if (!attribute || classifyDescriptor(globalObject, attribute).kind != DescriptorKind::Function)
        return getAttribute(globalObject, base, name);
    // What the instance itself has by that name comes first.
    if (JSObject* storage = attributeStorage(globalObject, base, type)) {
        if (JSValue own = storage->getDirect(vm, name))
            return own;
    }
    self = base;
    return attribute;
}

JSValue lookupSpecial(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue& self)
{
    VM& vm = globalObject->vm();
    self = { };
    PyType* type = typeOf(globalObject, value);
    JSValue attribute = type->lookup(vm, name);
    if (!attribute)
        return { };
    Descriptor descriptor = classifyDescriptor(globalObject, attribute);
    if (descriptor.kind == DescriptorKind::Function) {
        self = value;
        return attribute;
    }
    return bind(globalObject, descriptor, attribute, value, type);
}

// ---- Calls

static const FunctionInfo* pythonInfoOf(JSValue callable)
{
    auto* function = dynamicDowncast<JSFunction>(callable);
    if (!function || function->isHostOrBuiltinFunction())
        return nullptr;
    return function->jsExecutable()->unlinkedExecutable()->pythonInfo();
}

bool isCallable(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell())
        return false;
    if (value.asCell()->type() == PyInstanceType)
        return !!typeOf(globalObject, value)->lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_call);
    return value.isCallable();
}

JSObject* createNotCallableError(JSGlobalObject* globalObject, JSValue callable)
{
    return createException(globalObject, globalObject->pyRealm()->typeTypeError(), makeString('\'', typeName(globalObject, callable), "' object is not callable"_s));
}

static JSValue raiseNotCallable(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callable)
{
    throwException(globalObject, scope, createNotCallableError(globalObject, callable));
    return { };
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]]
        return raiseNotCallable(globalObject, scope, callable);
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, jsUndefined(), arguments));
}

JSValue call(JSGlobalObject* globalObject, JSValue callable)
{
    return call(globalObject, callable, ArgList());
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, JSValue a)
{
    MarkedArgumentBuffer arguments;
    arguments.append(a);
    return call(globalObject, callable, arguments);
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, JSValue a, JSValue b)
{
    MarkedArgumentBuffer arguments;
    arguments.append(a);
    arguments.append(b);
    return call(globalObject, callable, arguments);
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, JSValue a, JSValue b, JSValue c)
{
    MarkedArgumentBuffer arguments;
    arguments.append(a);
    arguments.append(b);
    arguments.append(c);
    return call(globalObject, callable, arguments);
}

JSValue callMethod(JSGlobalObject* globalObject, JSValue function, JSValue self, const ArgList& arguments)
{
    if (!self)
        return call(globalObject, function, arguments);
    MarkedArgumentBuffer all;
    all.append(self);
    for (unsigned i = 0; i < arguments.size(); ++i)
        all.append(arguments.at(i));
    return call(globalObject, function, all);
}

JSValue callMethod(JSGlobalObject* globalObject, JSValue function, JSValue self)
{
    return self ? call(globalObject, function, self) : call(globalObject, function);
}

JSValue callMethod(JSGlobalObject* globalObject, JSValue function, JSValue self, JSValue a)
{
    return self ? call(globalObject, function, self, a) : call(globalObject, function, a);
}

JSValue callMethod(JSGlobalObject* globalObject, JSValue function, JSValue self, JSValue a, JSValue b)
{
    return self ? call(globalObject, function, self, a, b) : call(globalObject, function, a, b);
}

static String joinNames(const Vector<String>& names)
{
    // 'a', 'b' and 'c'
    StringBuilder builder;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i)
            builder.append(i + 1 == names.size() ? (names.size() > 2 ? ", and "_s : " and "_s) : ", "_s);
        builder.append('\'', names[i], '\'');
    }
    return builder.toString();
}

// Works out the value of each parameter of a function written in Python, as Python/ceval.c of CPython does, and in its words if it
// cannot be done. The values of the keywords are the last of `arguments`.
bool bindArguments(JSGlobalObject* globalObject, JSFunction* function, const FunctionInfo& info, const ArgList& arguments, KeywordNames* keywordNames, MarkedArgumentBuffer& result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    unsigned keywordCount = keywordNames ? keywordNames->length() : 0;
    unsigned given = arguments.size() - keywordCount;
    unsigned positionalCount = info.positionalCount;
    unsigned namedCount = positionalCount + info.keywordOnlyCount;
    String functionName = nameOfFunction(globalObject, function, true);

    // Everything that goes in here is also in `arguments`, in the function's defaults, or in a local variable.
    Vector<JSValue, 16> bound(info.parameterCount());

    PyDict* extraKeywords = nullptr;
    if (info.hasKeywordVariadic) {
        extraKeywords = PyDict::create(globalObject);
        bound[info.keywordVariadicIndex()] = extraKeywords;
    }

    unsigned taken = std::min(given, positionalCount);
    for (unsigned i = 0; i < taken; ++i)
        bound[i] = arguments.at(i);
    if (info.hasVariadic) {
        PyTuple* rest = PyTuple::create(globalObject, given - taken);
        for (unsigned i = taken; i < given; ++i)
            rest->initializeAt(vm, i - taken, arguments.at(i));
        bound[info.variadicIndex()] = rest;
    }

    Vector<String> positionalOnlyGivenByKeyword;
    for (unsigned k = 0; k < keywordCount; ++k) {
        JSString* keyword = asString(keywordNames->get(k));
        String keywordString = keyword->value(globalObject);
        JSValue value = arguments.at(given + k);
        bool found = false;
        for (unsigned i = info.positionalOnlyCount; i < namedCount; ++i) {
            if (info.parameterNames[i].string() != keywordString)
                continue;
            if (bound[i]) {
                raiseTypeError(globalObject, scope, makeString(functionName, "() got multiple values for argument '"_s, keywordString, '\''));
                return false;
            }
            bound[i] = value;
            found = true;
            break;
        }
        if (found)
            continue;
        if (extraKeywords) {
            extraKeywords->set(globalObject, keyword, value);
            RETURN_IF_EXCEPTION(scope, false);
            continue;
        }
        bool isPositionalOnly = false;
        for (unsigned i = 0; i < info.positionalOnlyCount; ++i)
            isPositionalOnly |= info.parameterNames[i].string() == keywordString;
        if (isPositionalOnly) {
            positionalOnlyGivenByKeyword.append(keywordString);
            continue;
        }
        raiseTypeError(globalObject, scope, makeString(functionName, "() got an unexpected keyword argument '"_s, keywordString, '\''));
        return false;
    }
    if (!positionalOnlyGivenByKeyword.isEmpty()) {
        StringBuilder list;
        for (size_t i = 0; i < positionalOnlyGivenByKeyword.size(); ++i)
            list.append(i ? ", "_s : ""_s, positionalOnlyGivenByKeyword[i]);
        raiseTypeError(globalObject, scope, makeString(functionName, "() got some positional-only arguments passed as keyword arguments: '"_s, list.toString(), '\''));
        return false;
    }

    JSValue defaultsValue = function->getDirect(vm, names.private_defaults);
    PyTuple* defaults = defaultsValue ? uncheckedDowncast<PyTuple>(defaultsValue.asCell()) : nullptr;
    unsigned defaultCount = defaults ? defaults->length() : 0;

    if (given > positionalCount && !info.hasVariadic) {
        unsigned keywordOnlyGiven = 0;
        for (unsigned i = positionalCount; i < namedCount; ++i)
            keywordOnlyGiven += !!bound[i];
        StringBuilder message;
        message.append(functionName, "() takes "_s);
        if (defaultCount)
            message.append("from "_s, positionalCount - defaultCount, " to "_s, positionalCount, " positional arguments"_s);
        else
            message.append(positionalCount, " positional argument"_s, positionalCount == 1 ? ""_s : "s"_s);
        message.append(" but "_s, given);
        if (keywordOnlyGiven)
            message.append(" positional argument"_s, given == 1 ? ""_s : "s"_s, " (and "_s, keywordOnlyGiven, " keyword-only argument"_s, keywordOnlyGiven == 1 ? ""_s : "s"_s, ')');
        message.append(given == 1 && !keywordOnlyGiven ? " was given"_s : " were given"_s);
        raiseTypeError(globalObject, scope, message.toString());
        return false;
    }

    Vector<String> missing;
    unsigned firstDefault = positionalCount - defaultCount;
    for (unsigned i = 0; i < positionalCount; ++i) {
        if (bound[i])
            continue;
        if (i >= firstDefault)
            bound[i] = defaults->at(i - firstDefault);
        else
            missing.append(info.parameterNames[i].string());
    }
    if (!missing.isEmpty()) {
        raiseTypeError(globalObject, scope, makeString(functionName, "() missing "_s, missing.size(), " required positional argument"_s, missing.size() == 1 ? ""_s : "s"_s, ": "_s, joinNames(missing)));
        return false;
    }

    if (info.keywordOnlyCount) {
        JSValue keywordDefaultsValue = function->getDirect(vm, names.private_kwdefaults);
        PyDict* keywordDefaults = keywordDefaultsValue ? uncheckedDowncast<PyDict>(keywordDefaultsValue.asCell()) : nullptr;
        for (unsigned i = positionalCount; i < namedCount; ++i) {
            if (bound[i])
                continue;
            JSValue value = keywordDefaults ? keywordDefaults->getString(globalObject, info.parameterNames[i].string()) : JSValue();
            if (value)
                bound[i] = value;
            else
                missing.append(info.parameterNames[i].string());
        }
        if (!missing.isEmpty()) {
            raiseTypeError(globalObject, scope, makeString(functionName, "() missing "_s, missing.size(), " required keyword-only argument"_s, missing.size() == 1 ? ""_s : "s"_s, ": "_s, joinNames(missing)));
            return false;
        }
    }
    for (JSValue value : bound)
        result.append(value);
    return true;
}

JSValue callWithKeywords(JSGlobalObject* globalObject, JSValue callable, const ArgList& arguments, KeywordNames* keywordNames)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!keywordNames || !keywordNames->length())
        RELEASE_AND_RETURN(scope, call(globalObject, callable, arguments));

    auto callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]]
        return raiseNotCallable(globalObject, scope, callable);

    if (const FunctionInfo* info = pythonInfoOf(callable)) {
        MarkedArgumentBuffer bound;
        bool ok = bindArguments(globalObject, uncheckedDowncast<JSFunction>(callable.asCell()), *info, arguments, keywordNames, bound);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, globalObject->pyRealm()->boundArgumentsMarker(), bound));
    }

    JSCell* cell = callable.asCell();
    bool understandsKeywords = cell->inherits<PyNativeFunction>() || cell->type() == PyTypeType || cell->type() == PyBoundMethodType || cell->type() == PyInstanceType;
    if (understandsKeywords)
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, keywordNames, arguments));

    // A JavaScript function gets them as an object, after the rest.
    unsigned positional = arguments.size() - keywordNames->length();
    MarkedArgumentBuffer converted;
    for (unsigned i = 0; i < positional; ++i)
        converted.append(arguments.at(i));
    JSObject* options = constructEmptyObject(globalObject);
    for (unsigned i = 0; i < keywordNames->length(); ++i) {
        auto name = asString(keywordNames->get(i))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        options->putDirect(vm, name, arguments.at(positional + i));
    }
    converted.append(options);
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, jsUndefined(), converted));
}

JSValue instantiate(JSGlobalObject* globalObject, PyType* type, const ArgList& arguments, KeywordNames* keywordNames)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    // type(x)
    if (type == realm->typeType() && arguments.size() == 1 && !keywordNames)
        return typeOf(globalObject, arguments.at(0));

    JSValue constructor = type->lookup(vm, names.dunder_new);
    ASSERT(constructor);
    // It is a static method, whether or not it says so.
    if (auto* wrapper = tryNativeObject(constructor); wrapper && typeOf(globalObject, constructor) == realm->typeStaticMethod())
        constructor = wrapper->field(0);

    auto prepend = [&] (MarkedArgumentBuffer& buffer, JSValue first) {
        buffer.append(first);
        for (unsigned i = 0; i < arguments.size(); ++i)
            buffer.append(arguments.at(i));
    };
    MarkedArgumentBuffer withType;
    prepend(withType, type);
    JSValue instance = callWithKeywords(globalObject, constructor, withType, keywordNames);
    RETURN_IF_EXCEPTION(scope, { });

    PyType* instanceType = typeOf(globalObject, instance);
    if (!instanceType->isSubtypeOf(type))
        return instance;
    JSValue initializer = instanceType->lookup(vm, names.dunder_init);
    if (!initializer)
        return instance;
    MarkedArgumentBuffer withInstance;
    prepend(withInstance, instance);
    JSValue result = callWithKeywords(globalObject, initializer, withInstance, keywordNames);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(result))
        return raiseTypeError(globalObject, scope, makeString("__init__() should return None, not '"_s, typeName(globalObject, result), '\''));
    return instance;
}

// ---- The arguments of a function written in C++

JSValue NativeArguments::keyword(JSGlobalObject* globalObject, ASCIILiteral name) const
{
    for (unsigned i = 0; i < keywordCount(); ++i) {
        if (keywordName(i)->value(globalObject).data == name)
            return keywordValue(i);
    }
    return { };
}

bool NativeArguments::checkNoKeywords(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral functionName) const
{
    if (!keywordCount())
        return true;
    raiseTypeError(globalObject, scope, makeString(functionName, "() takes no keyword arguments"_s));
    return false;
}

bool NativeArguments::check(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral functionName, unsigned minimum, unsigned maximum) const
{
    if (!checkNoKeywords(globalObject, scope, functionName))
        return false;
    if (size() >= minimum && size() <= maximum)
        return true;
    if (minimum == maximum) {
        if (!minimum)
            raiseTypeError(globalObject, scope, makeString(functionName, "() takes no arguments ("_s, size(), " given)"_s));
        else if (minimum == 1)
            raiseTypeError(globalObject, scope, makeString(functionName, "() takes exactly one argument ("_s, size(), " given)"_s));
        else
            raiseTypeError(globalObject, scope, makeString(functionName, " expected "_s, minimum, " arguments, got "_s, size()));
        return false;
    }
    if (size() < minimum)
        raiseTypeError(globalObject, scope, makeString(functionName, " expected at least "_s, minimum, " argument"_s, minimum == 1 ? ""_s : "s"_s, ", got "_s, size()));
    else
        raiseTypeError(globalObject, scope, makeString(functionName, " expected at most "_s, maximum, " argument"_s, maximum == 1 ? ""_s : "s"_s, ", got "_s, size()));
    return false;
}

} } // namespace JSC::Python
