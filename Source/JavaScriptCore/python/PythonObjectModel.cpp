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
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSGlobalProxy.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSModuleNamespaceObject.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PyStateObject.h"
#include "PyWeakReference.h"
#include "ObjectConstructor.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionExecutable.h"

// The object model: what type a value has, exceptions, attributes and calls.

namespace JSC { namespace Python {

// ---- Types

bool isList(JSValue value)
{
    return value.isCell() && isListCell(value.asCell());
}

static PyType* typeOfError(PyRealm* realm, ErrorInstance* error)
{
    switch (error->errorType()) {
    case ErrorType::TypeError:
        return realm->typeTypeError();
    case ErrorType::RangeError:
        return error->isStackOverflowError() ? realm->typeRecursionError() : error->isOutOfMemoryError() ? realm->typeMemoryError() : realm->typeValueError();
    case ErrorType::ReferenceError:
        return realm->typeNameError();
    case ErrorType::SyntaxError:
        return realm->typeSyntaxError();
    default:
        return realm->typeJSError();
    }
}

bool isPythonFunction(JSFunction* function)
{
    return !function->isHostOrBuiltinFunction() && function->jsExecutable()->unlinkedExecutable()->pythonInfo();
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
        case ModuleNamespaceObjectType:
            return realm->typeModule();
        case JSFunctionType:
            if (auto* native = dynamicDowncast<PyNativeFunction>(cell)) {
                switch (native->kind()) {
                case PyNativeFunction::Kind::Function:
                case PyNativeFunction::Kind::New:
                case PyNativeFunction::Kind::StaticMethod:
                    return realm->typeBuiltinFunction();
                case PyNativeFunction::Kind::Method:
                    return realm->typeMethodDescriptor();
                case PyNativeFunction::Kind::Wrapper:
                    return realm->typeWrapperDescriptor();
                case PyNativeFunction::Kind::ClassMethod:
                    return realm->typeClassMethodDescriptor();
                }
            }
            if (isPythonFunction(uncheckedDowncast<JSFunction>(cell)))
                return realm->typeFunction();
            return isJavaScriptClass(cell) ? metatypeOfJavaScriptClass(globalObject, asObject(cell)) : realm->typeJSFunction();
        case InternalFunctionType:
            return isJavaScriptClass(cell) ? metatypeOfJavaScriptClass(globalObject, asObject(cell)) : realm->typeJSFunction();
        default:
            break;
        }
        if (!cell->isObject())
            return realm->typeJSObject();
        // What has a class for its prototype is an instance of it, if it is the kind of cell that its instances are. JavaScript can give anything any
        // prototype, with Object.create() or Reflect.construct(), and what is written in C++ takes an instance of dict for a PyDict.
        JSValue prototype = cell->structure()->storedPrototype(asObject(cell));
        if (isType(prototype)) {
            auto* type = asType(prototype);
            if (Structure* structure = type->instanceStructure(); structure && structure->classInfoForCells() == cell->classInfo()) [[likely]]
                return type;
            // It is JavaScript that makes the instances, and nothing written in C++ takes them for more than objects.
            if (type->layout() == PyType::Layout::JavaScript)
                return type;
        }
        // A cell of Python's, so an instance of a class of JavaScript's that is derived from one of Python's.
        if (cell->structure()->typeInfo().overloadsOperators()) [[unlikely]] {
            PyType* type = classForPrototype(globalObject, prototype);
            if (Structure* structure = type->instanceStructure(); structure && structure->classInfoForCells() == cell->classInfo())
                return type;
        }
        if (isListCell(cell))
            return realm->typeList();
        if (cell->type() == Uint8ArrayType)
            return realm->typeByteArray();
        if (cell->type() == JSGeneratorType)
            return realm->typeGenerator();
        if (cell->type() == ErrorInstanceType)
            return typeOfError(realm, uncheckedDowncast<ErrorInstance>(cell));
        // What a class of JavaScript's made.
        PyType* type = classForPrototype(globalObject, prototype);
        return type->layout() == PyType::Layout::JavaScript ? type : realm->typeJSObject();
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

bool isExactly(JSGlobalObject* globalObject, JSValue value, PyType* type)
{
    return typeOf(globalObject, value) == type;
}

bool isExactly(JSGlobalObject* globalObject, JSValue value, BuiltinType type)
{
    return typeOf(globalObject, value) == globalObject->pyRealm()->type(type);
}

String typeNameOfArgument(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_str : typeName(globalObject, value);
}

String typeName(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value)->nameString(globalObject);
}

JSObject* createMemberDescriptor(JSGlobalObject* globalObject, PyType* owner, JSString* name, const Identifier* storage, JSValue initialValue)
{
    VM& vm = globalObject->vm();
    // What is in a slot is a property of the instance under a private name, as a private field of a class of JavaScript's is. Nothing else can name it,
    // so it is in no dict and no list of properties.
    Symbol* key = Symbol::create(vm, static_cast<SymbolImpl&>(*storage->impl()));
    return PyGetSetDescriptor::createForSlot(globalObject, owner, name, key, initialValue);
}

// ---- Exceptions

JSObject* createException(JSGlobalObject* globalObject, PyType* type, JSValue argument)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyException* exception = PyException::create(vm, type);
    exception->putDirect(vm, vm.pythonNames().private_args, argument ? PyTuple::create(globalObject, { argument }) : PyTuple::create(globalObject, 0));
    // What its __init__ would have done with the one argument, for those that do more than keep it.
    if (!argument)
        return exception;
    if (type == realm->typeStopIteration())
        exception->putDirect(vm, vm.pythonNames().field_value, argument);
    else if (type == realm->typeSystemExit())
        exception->putDirect(vm, vm.pythonNames().field_code, argument);
    else if (type->isSubtypeOf(realm->typeSyntaxError()) || type->isSubtypeOf(realm->typeImportError()))
        exception->putDirect(vm, vm.pythonNames().field_message, argument);
    return exception;
}

JSObject* createException(JSGlobalObject* globalObject, PyType* type, const String& message)
{
    // There was no room for what was to be said, which has in it things that are as long as a program makes them.
    if (message.isNull()) [[unlikely]]
        return createException(globalObject, globalObject->pyRealm()->typeMemoryError(), JSValue());
    return createException(globalObject, type, jsString(globalObject->vm(), message));
}

String textOrMemoryError(JSGlobalObject* globalObject, String&& text)
{
    if (text.isNull()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseMemoryError(globalObject, scope);
    }
    return WTF::move(text);
}

JSValue strOrMemoryError(JSGlobalObject* globalObject, const String& text)
{
    VM& vm = globalObject->vm();
    if (text.isNull()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseMemoryError(globalObject, scope);
    }
    return jsString(vm, text);
}

String textOfBytes(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return emptyString();
    std::span<Latin1Character> characters;
    RefPtr text = bytes.size() > String::MaxLength ? nullptr : StringImpl::tryCreateUninitialized(bytes.size(), characters);
    if (!text) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseMemoryError(globalObject, scope);
        return { };
    }
    memcpySpan(characters, byteCast<Latin1Character>(bytes));
    return String(text.releaseNonNull());
}

String TextBuilder::finish(JSGlobalObject* globalObject)
{
    return textOrMemoryError(globalObject, tryFinish());
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
    // If it is already somewhere in the chain of what is being handled, it is taken out, or the chain would go round. A program can have made one that goes round already, so another goes along it at half the pace, and is
    // caught up with if it does: _PyErr_SetObject()
    JSObject* slow = asObject(handled);
    bool movesSlow = false;
    for (JSObject* link = asObject(handled);;) {
        JSValue next = link->getDirect(vm, name);
        if (!next || !next.isObject())
            break;
        if (next == JSValue(exception)) {
            link->putDirect(vm, name, jsUndefined());
            break;
        }
        link = asObject(next);
        if (link == slow)
            break;
        if (movesSlow)
            slow = asObject(slow->getDirect(vm, name));
        movesSlow = !movesSlow;
    }
    exception->putDirect(vm, name, handled);
}

JSValue raise(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type, const String& message)
{
    // As in createException().
    if (message.isNull()) [[unlikely]]
        return raiseMemoryError(globalObject, scope);
    return raise(globalObject, scope, type, jsString(globalObject->vm(), message));
}

JSValue raiseNameError(JSGlobalObject* globalObject, ThrowScope& scope, const String& name)
{
    JSObject* exception = createException(globalObject, globalObject->pyRealm()->typeNameError(), concatenate("name '"_s, name, "' is not defined"_s));
    exception->putDirect(globalObject->vm(), globalObject->vm().pythonNames().field_name, jsString(globalObject->vm(), name));
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
        raise(globalObject, scope, BuiltinType::NameError, concatenate("cannot access free variable '"_s, string, "' where it is not associated with a value in enclosing scope"_s));
    else
        raise(globalObject, scope, BuiltinType::UnboundLocalError, concatenate("cannot access local variable '"_s, string, "' where it is not associated with a value"_s));
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

// ---- Attributes

// A TypeError that JavaScript made is an instance of Python's TypeError, there being one such class. It is none the less an object of JavaScript's.
bool isJavaScriptObject(JSValue value, PyType* type)
{
    if (!value.isObject())
        return false;
    return type->hasFlag(PyType::IsJavaScript) || (value.asCell()->type() == ErrorInstanceType && !value.asCell()->structure()->typeInfo().overloadsOperators());
}

// Where an object's own attributes are, if it can have any. They are the properties of this.
//
// It is the object itself, and its __dict__ is a dict that is backed by it. The exception is an object that has been given, for its __dict__, a dict that
// is some other object's already: two objects with one dict, so that what is set on either is set on both. Then it is that other object.
//
// An object of JavaScript's has none. Its attributes are its properties too, but as JavaScript finds and sets them: there may be a getter, a setter or a Proxy, and it may be
// frozen. This would go around all of that.
JSObject* attributeStorage(JSGlobalObject* globalObject, JSValue value, PyType* type)
{
    if (!type->hasFlag(PyType::HasInstanceDict) || !value.isObject() || isJavaScriptObject(value, type))
        return nullptr;
    JSObject* object = asObject(value);
    // The attributes of a class are seen to separately.
    if (object->type() == PyTypeType)
        return nullptr;
    if (type->hasFlag(PyType::MayHaveForeignDict)) [[unlikely]] {
        if (JSValue foreign = object->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_foreignDict))
            return uncheckedDowncast<PyDict>(foreign.asCell())->ensureBacking(globalObject);
    }
    return object;
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
    JavaScriptFunction, // Becomes a function bound to the instance: where Python would pass `self`, a function of JavaScript's gets `this`.
    JavaScriptAccessor, // What `get x() {}` and `set x(v) {}` in a class of JavaScript's make. It is no value, and no program is given it.
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
            case PyNativeFunction::Kind::StaticMethod:
                return { };
            case PyNativeFunction::Kind::Method:
            case PyNativeFunction::Kind::Wrapper:
                return { DescriptorKind::Function, false, { } };
            case PyNativeFunction::Kind::ClassMethod:
                return { DescriptorKind::NativeClassMethod, false, { } };
            }
        }
        if (auto* function = uncheckedDowncast<JSFunction>(cell); !isPythonFunction(function)) {
            // A class says what something is, and is not something that it does. What is bound is bound.
            if (function->inherits<JSBoundFunction>() || (!function->isHostOrBuiltinFunction() && function->jsExecutable()->isClassConstructorFunction()))
                return { };
            return { DescriptorKind::JavaScriptFunction, false, { } };
        }
        return { DescriptorKind::Function, false, { } };
    case GetterSetterType:
        return { DescriptorKind::JavaScriptAccessor, true, { } };
    case StringType:
    case HeapBigIntType:
    case SymbolType:
    case PyTupleType:
    case PyDictType:
    case PySetType:
    case PyTypeType:
    case PyBoundMethodType:
        return { };
    default:
        break;
    }
    if (!cell->isObject())
        return { };
    if (auto* getSet = dynamicDowncast<PyGetSetDescriptor>(cell))
        return { getSet->storage() ? DescriptorKind::Member : DescriptorKind::GetSet, true, { } };

    PyType* type = typeOf(globalObject, value);
    if (type == realm->typeProperty())
        return { DescriptorKind::Property, true, { } };
    if (type == realm->typeStaticMethod())
        return { DescriptorKind::StaticMethod, false, { } };
    if (type == realm->typeClassMethod())
        return { DescriptorKind::ClassMethod, false, { } };
    if (type == realm->typeJSObject() || type == realm->typeJSFunction() || type == realm->typeList())
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
        return PyBoundMethod::create(globalObject, value, type->object());
    case DescriptorKind::JavaScriptFunction:
        if (!instance)
            return value;
        RELEASE_AND_RETURN(scope, JSBoundFunction::bind(globalObject, vm.topCallFrame, asObject(value), instance, ArgList()));
    case DescriptorKind::JavaScriptAccessor: {
        auto* accessor = uncheckedDowncast<GetterSetter>(value.asCell());
        if (!instance)
            return accessor->isGetterNull() ? jsUndefined() : JSValue(accessor->getter());
        RELEASE_AND_RETURN(scope, accessor->callGetter(globalObject, instance));
    }
    case DescriptorKind::GetSet:
        if (!instance)
            return value;
        RELEASE_AND_RETURN(scope, uncheckedDowncast<PyGetSetDescriptor>(value.asCell())->getter()(globalObject, instance));
    case DescriptorKind::Property: {
        if (!instance)
            return value;
        RELEASE_AND_RETURN(scope, getProperty(globalObject, uncheckedDowncast<PyNativeObject>(value.asCell()), instance));
    }
    case DescriptorKind::StaticMethod:
        return uncheckedDowncast<PyNativeObject>(value.asCell())->field(0);
    case DescriptorKind::ClassMethod: {
        JSValue function = uncheckedDowncast<PyNativeObject>(value.asCell())->field(0);
        return PyBoundMethod::createMethod(globalObject, function, type->object());
    }
    case DescriptorKind::Member: {
        if (!instance)
            return value;
        auto* member = uncheckedDowncast<PyGetSetDescriptor>(value.asCell());
        if (JSValue stored = asObject(instance)->getDirect(vm, member->storage()->privateName()))
            return stored;
        // One of a built-in type has a value from the start.
        if (JSValue initial = member->initialValue())
            return initial;
        return raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', fullyQualifiedTypeName(globalObject, instance), "' object has no attribute '"_s, member->name()->value(globalObject).data, '\''));
    }
    case DescriptorKind::General:
        if (!descriptor.getter)
            return value;
        RELEASE_AND_RETURN(scope, call(globalObject, descriptor.getter, value, instance ? instance : jsUndefined(), type->object()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue nameAsString(VM& vm, PropertyName name)
{
    return jsString(vm, String(name.uid()));
}

// type.__getattribute__. Empty, with nothing raised, if there is no such attribute.
enum class InheritedFunctions : uint8_t { Bind, LeaveAsFound };
static JSValue getJavaScriptProperty(JSGlobalObject*, JSObject*, PropertyName, InheritedFunctions);

} // namespace

JSValue callForInstance(JSGlobalObject* globalObject, JSValue function, JSValue self, JSValue argument)
{
    if (classifyDescriptor(globalObject, function).kind != DescriptorKind::JavaScriptFunction) [[likely]]
        return argument ? call(globalObject, function, self, argument) : call(globalObject, function, self);
    MarkedArgumentBuffer buffer;
    if (argument)
        buffer.append(argument);
    return JSC::call(globalObject, function, JSC::getCallData(function), self, buffer);
}

JSValue getTypeAttribute(JSGlobalObject* globalObject, PyType* type, PropertyName name, ClassIsFunctionToo classIsFunctionToo, PyType* from)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* metatype = type->metatype();
    JSValue metaAttribute = metatype->lookup(vm, name);
    Descriptor metaDescriptor;
    if (metaAttribute) {
        metaDescriptor = classifyDescriptor(globalObject, metaAttribute);
        if (metaDescriptor.isData)
            RELEASE_AND_RETURN(scope, bind(globalObject, metaDescriptor, metaAttribute, type->object(), metatype));
    }
    bool isStatic = false;
    if (JSValue attribute = type->lookupOnClass(vm, name, isStatic, from)) {
        // What is `static` in a class of JavaScript's is the class's and not its instances', so it is the class that an accessor is for.
        Descriptor descriptor = classifyDescriptor(globalObject, attribute);
        bool isForClass = isStatic && descriptor.kind == DescriptorKind::JavaScriptAccessor;
        RELEASE_AND_RETURN(scope, bind(globalObject, descriptor, attribute, isForClass ? JSValue(type->object()) : JSValue(), type));
    }
    if (metaAttribute)
        RELEASE_AND_RETURN(scope, bind(globalObject, metaDescriptor, metaAttribute, type->object(), metatype));
    // A class of JavaScript's is a function of JavaScript's besides.
    if (JSObject* constructor = type->javaScriptConstructor(); constructor && classIsFunctionToo == ClassIsFunctionToo::Yes) {
        JSValue property = getJavaScriptProperty(globalObject, constructor, name, InheritedFunctions::Bind);
        RETURN_IF_EXCEPTION(scope, { });
        if (property)
            return property;
        PyType* functionType = globalObject->pyRealm()->typeJSFunction();
        if (JSValue attribute = functionType->lookup(vm, name))
            RELEASE_AND_RETURN(scope, bind(globalObject, classifyDescriptor(globalObject, attribute), attribute, constructor, functionType));
    }
    return { };
}

namespace {

// An attribute of a JavaScript object is a property of it, as JavaScript finds it. Empty if it has none.
//
// A function that is got from what an object inherits from remembers the object, as one that is got from a class does in Python: it is what
// obj.method.bind(obj) would be. One that the object has of its own is as it is, as one in an instance's __dict__ is in Python. So js.Math.floor and
// js.Array are themselves. Nor does it apply to a class, or to obj.constructor, which say what the object is and are not something that it does.
static JSValue getJavaScriptProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, InheritedFunctions inheritedFunctions)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    bool found = object->getPropertySlot(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found)
        return { };
    JSValue value = slot.getValue(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (inheritedFunctions == InheritedFunctions::LeaveAsFound || !slot.isValue() || name == vm.propertyNames->constructor)
        return value;
    // What the global object has is what globalThis has, which stands for it.
    JSObject* owner = object->type() == GlobalProxyType ? uncheckedDowncast<JSGlobalProxy>(object)->target() : object;
    if (slot.slotBase() == owner)
        return value;
    auto* function = dynamicDowncast<JSFunction>(value);
    if (!function || function->inherits<JSBoundFunction>() || function->inherits<PyNativeFunction>())
        return value;
    if (!function->isHostOrBuiltinFunction() && (function->jsExecutable()->isClassConstructorFunction() || isPythonFunction(function)))
        return value;
    RELEASE_AND_RETURN(scope, JSBoundFunction::bind(globalObject, vm.topCallFrame, function, object, ArgList()));
}

// What a module of JavaScript's exports by that name. Empty if it exports nothing by it, or has not got as far as giving it a value, which is how it is with a module of Python's
// that is in the middle of being imported: the attribute is not there yet.
static JSValue getExport(JSGlobalObject* globalObject, JSModuleNamespaceObject* module, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    bool isThere = module->isInitializedExport(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isThere)
        return { };
    RELEASE_AND_RETURN(scope, module->get(globalObject, name));
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
    if (isJavaScriptObject(value, type)) {
        JSValue property = getJavaScriptProperty(globalObject, asObject(value), name, InheritedFunctions::Bind);
        RETURN_IF_EXCEPTION(scope, { });
        if (property)
            return property;
    } else if (JSObject* storage = attributeStorage(globalObject, value, type)) {
        if (auto* module = dynamicDowncast<JSModuleNamespaceObject>(value)) [[unlikely]] {
            JSValue exported = getExport(globalObject, module, name);
            RETURN_IF_EXCEPTION(scope, { });
            if (exported)
                return exported;
        }
        if (JSValue own = getStoredAttribute(vm, storage, name))
            return own;
    }
    if (attribute)
        RELEASE_AND_RETURN(scope, bind(globalObject, descriptor, attribute, value, type));
    return { };
}

// The end of _Py_module_getattro_impl(): as much help as can be given. Null if it raised.
String messageForNoModuleAttribute(JSGlobalObject* globalObject, JSObject* module, StringView attribute)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue moduleName = getStoredAttribute(vm, module, vm.pythonNames().dunder_name);
    if (!moduleName || !stringIn(moduleName))
        return concatenate("module has no attribute '"_s, attribute, '\'');
    String name = stringIn(moduleName)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String plain = concatenate("module '"_s, name, "' has no attribute '"_s, attribute, '\'');
    JSValue spec = getStoredAttribute(vm, module, vm.pythonNames().dunder_spec);
    if (!spec)
        return plain;
    JSValue origin = fileOriginOfSpec(globalObject, spec);
    RETURN_IF_EXCEPTION(scope, { });
    String originText;
    if (origin) {
        originText = stringIn(origin)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool isShadowing = isPossiblyShadowing(globalObject, origin);
    if (isShadowing) {
        auto isShadowingLibrary = isShadowingStandardLibrary(globalObject, moduleName);
        RETURN_IF_EXCEPTION(scope, { });
        if (*isShadowingLibrary)
            return concatenate(plain, " (consider renaming '"_s, originText, "' since it has the same name as the standard library module named '"_s, name, "' and prevents importing that standard library module)"_s);
    }
    auto isInitializing = isSpecInitializing(globalObject, spec);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isInitializing) {
        // Of what is not the standard library's, it is only said that it may be in the way of something if it has not been run to its end.
        if (isShadowing)
            return concatenate(plain, " (consider renaming '"_s, originText, "' if it has the same name as a library you intended to import)"_s);
        if (origin)
            return concatenate("partially initialized module '"_s, name, "' from '"_s, originText, "' has no attribute '"_s, attribute, "' (most likely due to a circular import)"_s);
        return concatenate("partially initialized module '"_s, name, "' has no attribute '"_s, attribute, "' (most likely due to a circular import)"_s);
    }
    auto isSubmodule = isUninitializedSubmodule(globalObject, spec, jsString(vm, attribute.toString()));
    RETURN_IF_EXCEPTION(scope, { });
    if (*isSubmodule)
        return concatenate("cannot access submodule '"_s, attribute, "' of module '"_s, name, "' (most likely due to a circular import)"_s);
    return plain;
}

JSValue raiseNoAttribute(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    StringView attribute { name.uid() };
    String message;
    if (isClass(value))
        message = concatenate("type object '"_s, asType(value)->nameString(globalObject), "' has no attribute '"_s, attribute, '\'');
    else if (JSObject* module = tryModule(globalObject, value)) {
        message = messageForNoModuleAttribute(globalObject, module, attribute);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (message.isNull())
        message = concatenate('\'', typeName(globalObject, value), "' object has no attribute '"_s, attribute, '\'');
    JSObject* exception = createException(globalObject, globalObject->pyRealm()->typeAttributeError(), message);
    exception->putDirect(vm, vm.pythonNames().field_name, jsString(vm, attribute.toString()));
    exception->putDirect(vm, vm.pythonNames().field_object, value);
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

} // anonymous namespace

bool hasGet(JSGlobalObject* globalObject, JSValue value)
{
    return classifyDescriptor(globalObject, value).kind != DescriptorKind::Plain;
}

JSValue bindDescriptor(JSGlobalObject* globalObject, JSValue descriptor, JSValue instance, PyType* type)
{
    return bind(globalObject, classifyDescriptor(globalObject, descriptor), descriptor, instance, type);
}

// ---- Attributes

JSValue genericGetAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    if (isClass(value))
        return getTypeAttribute(globalObject, asType(value), name);
    return getObjectAttribute(globalObject, value, typeOf(globalObject, value), name);
}

// module.__getattr__(name), for a module that defines such a function (PEP 562), which is asked when the module has no such attribute. Empty, with
// nothing raised, if it defines none. If it says AttributeError, that is what is raised, unless there is someone else to ask.
enum class IfModuleSaysNo : uint8_t { Raise, GoOn };
static JSValue askModuleForAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name, IfModuleSaysNo ifNo)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* module = tryModule(globalObject, value);
    if (!module)
        return { };
    JSValue hook = getStoredAttribute(vm, module, vm.pythonNames().dunder_getattr);
    if (!hook)
        return { };
    JSValue result = call(globalObject, hook, nameAsString(vm, name));
    if (scope.exception()) [[unlikely]] {
        if (ifNo == IfModuleSaysNo::GoOn)
            catchException(globalObject, BuiltinType::AttributeError);
        return { };
    }
    return result;
}

JSValue getAttributeIfPresent(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyType* type = typeOf(globalObject, value);
    unsigned hooks = type->hooks(globalObject);

    JSValue result;
    if (hooks & PyType::HasCustomGetAttribute) [[unlikely]]
        result = callSpecial(globalObject, type, type->lookup(vm, names.dunder_getattribute), value, nameAsString(vm, name));
    else
        result = genericGetAttribute(globalObject, value, name);
    // AttributeError is how it is said that there is no such attribute, whoever says it: the getter of a property may. __getattr__ gets its
    // say then too.
    if (scope.exception()) [[unlikely]] {
        if (!catchException(globalObject, BuiltinType::AttributeError))
            return { };
        result = { };
    }
    if (result)
        return result;
    // A __getattribute__() of the class's own has asked, if it meant it to be.
    if (!(hooks & PyType::HasCustomGetAttribute)) {
        result = askModuleForAttribute(globalObject, value, name, IfModuleSaysNo::GoOn);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (result || !(hooks & PyType::HasGetAttr))
        return result;

    result = callSpecial(globalObject, type, type->lookup(vm, names.dunder_getattr), value, nameAsString(vm, name));
    if (scope.exception()) [[unlikely]] {
        catchException(globalObject, BuiltinType::AttributeError);
        return { };
    }
    return result;
}

// module.__getattribute__()
JSValue getModuleAttribute(JSGlobalObject* globalObject, JSValue module, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = genericGetAttribute(globalObject, module, name);
    if (scope.exception() && !catchException(globalObject, BuiltinType::AttributeError))
        return { };
    if (result)
        return result;
    result = askModuleForAttribute(globalObject, module, name, IfModuleSaysNo::Raise);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    return raiseNoAttribute(globalObject, scope, module, name);
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
        result = callSpecial(globalObject, type, type->lookup(vm, names.dunder_getattribute), value, nameAsString(vm, name));
    else
        result = genericGetAttribute(globalObject, value, name);

    if (scope.exception()) [[unlikely]] {
        if (!(hooks & PyType::HasGetAttr) || !catchException(globalObject, BuiltinType::AttributeError))
            return { };
        result = { };
    }
    if (result) [[likely]]
        return result;
    if (!(hooks & PyType::HasCustomGetAttribute)) {
        result = askModuleForAttribute(globalObject, value, name, hooks & PyType::HasGetAttr ? IfModuleSaysNo::GoOn : IfModuleSaysNo::Raise);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    if (hooks & PyType::HasGetAttr)
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, type, type->lookup(vm, names.dunder_getattr), value, nameAsString(vm, name)));
    return raiseNoAttribute(globalObject, scope, value, name);
}

JSValue getIndexLikeAttribute(VM& vm, JSObject* object, PropertyName name)
{
    JSValue dict = object->getDirect(vm, vm.pythonNames().private_dict);
    return dict ? uncheckedDowncast<PyDict>(dict.asCell())->get(object->globalObject(), nameAsString(vm, name)) : JSValue();
}

void putIndexLikeAttribute(VM& vm, JSObject* object, PropertyName name, JSValue value)
{
    JSGlobalObject* globalObject = object->globalObject();
    PyDict::backedBy(globalObject, object)->set(globalObject, nameAsString(vm, name), value);
}

bool deleteStoredAttribute(JSGlobalObject* globalObject, JSObject* object, PropertyName name)
{
    if (isIndexLike(name)) [[unlikely]] {
        VM& vm = globalObject->vm();
        JSValue dict = object->getDirect(vm, vm.pythonNames().private_dict);
        return dict && uncheckedDowncast<PyDict>(dict.asCell())->remove(globalObject, nameAsString(vm, name));
    }
    DeletePropertySlot slot;
    return JSObject::deleteProperty(object, globalObject, name, slot);
}

bool isDataDescriptor(JSGlobalObject* globalObject, JSValue value)
{
    return classifyDescriptor(globalObject, value).isData;
}

bool classComesBeforeInstance(JSGlobalObject* globalObject, PyType* type, PropertyName name, AttributeAccess access)
{
    unsigned hooks = type->hooks(globalObject);
    if (access == AttributeAccess::Get ? hooks & PyType::HasCustomGetAttribute : (hooks & PyType::HasCustomSetAttr) || !type->hasFlag(PyType::HasInstanceDict))
        return true;
    if (type->hasFlag(PyType::MayHaveForeignDict))
        return true;
    JSValue attribute = type->lookup(globalObject->vm(), name);
    return attribute && isDataDescriptor(globalObject, attribute);
}

// descriptor.__set__(value, newValue), or __delete__ if `newValue` is empty, for a descriptor that has such a thing. False if it has not.
static bool setThroughDescriptor(JSGlobalObject* globalObject, JSValue found, JSValue value, PyType* type, StringView attribute, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    Descriptor descriptor = classifyDescriptor(globalObject, found);
    switch (descriptor.kind) {
    case DescriptorKind::GetSet: {
        auto* getSet = uncheckedDowncast<PyGetSetDescriptor>(found.asCell());
        if (!getSet->setter()) {
            if (getSet->isMember())
                raise(globalObject, scope, BuiltinType::AttributeError, "readonly attribute"_s);
            else
                raise(globalObject, scope, BuiltinType::AttributeError, concatenate("attribute '"_s, attribute, "' of '"_s, getSet->owner()->nameString(globalObject), "' objects is not writable"_s));
            return true;
        }
        scope.release();
        getSet->setter()(globalObject, value, newValue);
        return true;
    }
    case DescriptorKind::Property:
        scope.release();
        setProperty(globalObject, uncheckedDowncast<PyNativeObject>(found.asCell()), value, newValue);
        return true;
    case DescriptorKind::JavaScriptAccessor:
        if (!newValue) {
            raise(globalObject, scope, BuiltinType::AttributeError, concatenate("property '"_s, attribute, "' of '"_s, type->nameString(globalObject), "' object has no deleter"_s));
            return true;
        }
        scope.release();
        uncheckedDowncast<GetterSetter>(found.asCell())->callSetter(globalObject, value, newValue, true);
        return true;
    case DescriptorKind::Member: {
        PropertyName storage = uncheckedDowncast<PyGetSetDescriptor>(found.asCell())->storage()->privateName();
        if (newValue) {
            asObject(value)->putDirect(vm, storage, newValue);
            return true;
        }
        if (!asObject(value)->getDirect(vm, storage)) {
            // Deleting an empty slot says only which. One that is never without a value goes back to the one it began with, and is there already.
            if (!uncheckedDowncast<PyGetSetDescriptor>(found.asCell())->initialValue())
                raise(globalObject, scope, BuiltinType::AttributeError, attribute.toString());
            return true;
        }
        scope.release();
        JSCell::deleteProperty(asObject(value), globalObject, storage);
        return true;
    }
    case DescriptorKind::General:
        if (descriptor.isData) {
            JSValue function = typeOf(globalObject, found)->lookup(vm, newValue ? names.dunder_set : names.dunder_delete);
            if (!function) {
                raise(globalObject, scope, BuiltinType::AttributeError, newValue ? "__set__"_s : "__delete__"_s);
                return true;
            }
            scope.release();
            if (newValue)
                callSpecial(globalObject, typeOf(globalObject, found), function, found, value, newValue);
            else
                callSpecial(globalObject, typeOf(globalObject, found), function, found, value);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void setDescriptor(JSGlobalObject* globalObject, JSValue descriptor, JSValue instance, StringView attribute, JSValue newValue)
{
    setThroughDescriptor(globalObject, descriptor, instance, typeOf(globalObject, instance), attribute, newValue);
}

void raiseCannotSetAttribute(JSGlobalObject* globalObject, JSValue object, PropertyName name, bool isDeleting)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raise(globalObject, scope, BuiltinType::AttributeError, concatenate(isDeleting ? "can't delete attribute '"_s : "can't set attribute '"_s, StringView { name.uid() }, "' of '"_s, typeName(globalObject, object), "' object"_s));
}

// obj.name = newValue, of an object of JavaScript's, and del obj.name if `newValue` is empty. It is for JavaScript to say whether it can be done. What it will not do is
// AttributeError, which is what a dataclass that is frozen raises.
static void setJavaScriptProperty(JSGlobalObject* globalObject, JSObject* object, PyType* type, PropertyName name, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringView attribute { name.uid() };
    // It is asked as code that is not strict asks, so that it says no and does not throw.
    if (newValue) {
        PutPropertySlot slot(object, false);
        bool wasSet = object->methodTable()->put(object, globalObject, name, newValue, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (!wasSet)
            RELEASE_AND_RETURN(scope, raiseCannotSetAttribute(globalObject, object, name, false));
        return;
    }
    // JavaScript is content to delete what is not there.
    bool isThere = object->hasOwnProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, void());
    if (!isThere) {
        raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', type->nameString(globalObject), "' object has no attribute '"_s, attribute, '\''));
        return;
    }
    bool wasDeleted = JSCell::deleteProperty(object, globalObject, name);
    RETURN_IF_EXCEPTION(scope, void());
    if (!wasDeleted)
        RELEASE_AND_RETURN(scope, raiseCannotSetAttribute(globalObject, object, name, true));
}

// object.__setattr__ and object.__delattr__, and type's. `newValue` is empty to delete.
void genericSetAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, value);
    StringView attribute { name.uid() };

    // Nothing of a built-in class can be set, whatever there may be to set it with.
    if (isType(value) && asType(value)->isImmutable()) {
        raiseTypeError(globalObject, scope, concatenate("cannot set '"_s, attribute, "' attribute of immutable type '"_s, asType(value)->nameString(globalObject), '\''));
        return;
    }

    JSValue found = type->lookup(vm, name);
    if (found) {
        bool wasSet = setThroughDescriptor(globalObject, found, value, type, attribute, newValue);
        RETURN_IF_EXCEPTION(scope, void());
        if (wasSet)
            return;
    }

    if (isClass(value)) {
        auto* target = asType(value);
        if (target->isImmutable()) {
            raiseTypeError(globalObject, scope, concatenate("cannot set '"_s, attribute, "' attribute of immutable type '"_s, target->nameString(globalObject), '\''));
            return;
        }
        if (JSObject* constructor = target->javaScriptConstructor()) {
            // An attribute of a class is there for its instances too, and in JavaScript that is a property of their prototype. What is `static` is
            // not, and stays where it is.
            JSObject* holder = constructor->getDirect(vm, name) ? constructor : target->javaScriptPrototype();
            if (newValue) {
                scope.release();
                PutPropertySlot slot(holder, true);
                holder->methodTable()->put(holder, globalObject, name, newValue, slot);
                return;
            }
            if (!holder->getDirect(vm, name)) {
                raise(globalObject, scope, BuiltinType::AttributeError, concatenate("type object '"_s, target->nameString(globalObject), "' has no attribute '"_s, attribute, '\''));
                return;
            }
            scope.release();
            JSCell::deleteProperty(holder, globalObject, name);
            return;
        }
        if (newValue) {
            target->setAttribute(vm, name, newValue);
            return;
        }
        bool deleted = target->deleteAttribute(vm, globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (!deleted)
            raise(globalObject, scope, BuiltinType::AttributeError, concatenate("type object '"_s, target->nameString(globalObject), "' has no attribute '"_s, attribute, '\''));
        return;
    }

    if (JSObject* storage = attributeStorage(globalObject, value, type)) {
        // What a module of JavaScript's exports is for it alone to set.
        if (auto* module = dynamicDowncast<JSModuleNamespaceObject>(value); module && module->hasExport(name)) [[unlikely]]
            RELEASE_AND_RETURN(scope, raiseCannotSetAttribute(globalObject, value, name, !newValue));
        if (newValue) {
            if (!tryPutStoredAttribute(vm, storage, name, newValue)) [[unlikely]]
                RELEASE_AND_RETURN(scope, raiseCannotSetAttribute(globalObject, value, name, false));
            return;
        }
        if (!getStoredAttribute(vm, storage, name)) {
            // Whatever it is: a module says no more of itself than anything else does.
            raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', type->nameString(globalObject), "' object has no attribute '"_s, attribute, '\''));
            return;
        }
        if (!mayDeleteStoredAttribute(vm, storage, name)) [[unlikely]]
            RELEASE_AND_RETURN(scope, raiseCannotSetAttribute(globalObject, value, name, true));
        scope.release();
        deleteStoredAttribute(globalObject, storage, name);
        return;
    }

    if (isJavaScriptObject(value, type))
        RELEASE_AND_RETURN(scope, setJavaScriptProperty(globalObject, asObject(value), type, name, newValue));

    if (found) {
        raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', type->nameString(globalObject), "' object attribute '"_s, attribute, "' is read-only"_s));
        return;
    }
    // Deleting is setting to nothing, and is spoken of as setting.
    raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', type->nameString(globalObject), "' object has no attribute '"_s, attribute, "' and no __dict__ for setting new attributes"_s));
}

void setAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue newValue)
{
    VM& vm = globalObject->vm();
    PyType* type = typeOf(globalObject, value);
    if (type->hooks(globalObject) & PyType::HasCustomSetAttr) [[unlikely]] {
        callSpecial(globalObject, type, type->lookup(vm, vm.pythonNames().dunder_setattr), value, nameAsString(vm, name), newValue);
        return;
    }
    genericSetAttribute(globalObject, value, name, newValue);
}

void deleteAttribute(JSGlobalObject* globalObject, JSValue value, PropertyName name)
{
    VM& vm = globalObject->vm();
    PyType* type = typeOf(globalObject, value);
    if (type->hooks(globalObject) & PyType::HasCustomSetAttr) [[unlikely]] {
        callSpecial(globalObject, type, type->lookup(vm, vm.pythonNames().dunder_delattr), value, nameAsString(vm, name));
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
        if (start && !(isClass(instance) && asType(instance) == asType(start))) {
            JSValue attribute = asType(start)->lookupAfter(vm, asType(object->field(0)), name);
            if (attribute && classifyDescriptor(globalObject, attribute).kind == DescriptorKind::Function) {
                self = instance;
                return attribute;
            }
        }
    }
    if (isClass(base) || (type->hooks(globalObject) & PyType::HasCustomGetAttribute))
        return getAttribute(globalObject, base, name);
    JSValue attribute = type->lookup(vm, name);
    if (isJavaScriptObject(base, type)) {
        // What is about to be called is called with the object as `this` in any case, so there is nothing for it to remember.
        if (!attribute || !classifyDescriptor(globalObject, attribute).isData) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            JSValue property = getJavaScriptProperty(globalObject, asObject(base), name, InheritedFunctions::LeaveAsFound);
            RETURN_IF_EXCEPTION(scope, { });
            if (property)
                return property;
        }
        return getAttribute(globalObject, base, name);
    }
    DescriptorKind kind = attribute ? classifyDescriptor(globalObject, attribute).kind : DescriptorKind::Plain;
    if (kind != DescriptorKind::Function && kind != DescriptorKind::JavaScriptFunction)
        return getAttribute(globalObject, base, name);
    // What the instance itself has by that name comes first.
    if (JSObject* storage = attributeStorage(globalObject, base, type)) {
        if (JSValue own = getStoredAttribute(vm, storage, name))
            return own;
    }
    // What is called without `self` is called with what it was got from as `this`, which is what one of JavaScript's wants.
    if (kind == DescriptorKind::Function)
        self = base;
    return attribute;
}

JSValue bindSpecial(JSGlobalObject* globalObject, PyType* type, JSValue attribute, JSValue value, JSValue& self)
{
    self = { };
    Descriptor descriptor = classifyDescriptor(globalObject, attribute);
    if (descriptor.kind == DescriptorKind::Function) {
        self = value;
        return attribute;
    }
    return bind(globalObject, descriptor, attribute, value, type);
}

JSValue lookupSpecial(JSGlobalObject* globalObject, JSValue value, PropertyName name, JSValue& self)
{
    self = { };
    PyType* type = typeOf(globalObject, value);
    JSValue attribute = type->lookup(globalObject->vm(), name);
    if (!attribute)
        return { };
    return bindSpecial(globalObject, type, attribute, value, self);
}

template<typename... Arguments>
static ALWAYS_INLINE JSValue callSpecialWith(JSGlobalObject* globalObject, PyType* type, JSValue attribute, JSValue value, Arguments... arguments)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue self;
    JSValue function = bindSpecial(globalObject, type, attribute, value, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, function, self, arguments...));
}

JSValue callSpecial(JSGlobalObject* globalObject, PyType* type, JSValue attribute, JSValue value) { return callSpecialWith(globalObject, type, attribute, value); }
JSValue callSpecial(JSGlobalObject* globalObject, PyType* type, JSValue attribute, JSValue value, JSValue a) { return callSpecialWith(globalObject, type, attribute, value, a); }
JSValue callSpecial(JSGlobalObject* globalObject, PyType* type, JSValue attribute, JSValue value, JSValue a, JSValue b) { return callSpecialWith(globalObject, type, attribute, value, a, b); }

// ---- Calls

static const FunctionInfo* pythonInfoOf(JSValue callable)
{
    auto* function = dynamicDowncast<JSFunction>(callable);
    if (!function || function->isHostOrBuiltinFunction())
        return nullptr;
    return function->jsExecutable()->unlinkedExecutable()->pythonInfo();
}

// Python has no way of writing `import express from "express"`. What it writes is `import express`, which gives it the module, and then `express()`. A module of JavaScript's cannot be called, so
// there is nothing else that that can mean: it is what the module exports by default that is called. It is asked only where there is nothing left to do but raise. Empty if there is no such thing.
static JSValue defaultExportToCall(JSGlobalObject* globalObject, JSValue callable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* module = dynamicDowncast<JSModuleNamespaceObject>(callable);
    if (!module)
        return { };
    JSValue exported = getExport(globalObject, module, vm.propertyNames->defaultKeyword);
    RETURN_IF_EXCEPTION(scope, { });
    return exported && exported.isCallable() ? exported : JSValue();
}

bool isCallable(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell())
        return false;
    if (value.asCell()->type() == PyInstanceType)
        return !!typeOf(globalObject, value)->lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_call);
    if (value.asCell()->type() == ModuleNamespaceObjectType) [[unlikely]] {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        bool hasSomethingToCall = !!defaultExportToCall(globalObject, value);
        if (scope.exception()) [[unlikely]] {
            (void)scope.tryClearException();
            return false;
        }
        return hasSomethingToCall;
    }
    if (value.isCallable())
        return true;
    // See callWhatOnlyPythonCalls().
    return value.isObject() && typeOf(globalObject, value)->lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_call);
}

JSObject* createNotCallableError(JSGlobalObject* globalObject, JSValue callable)
{
    return createException(globalObject, globalObject->pyRealm()->typeTypeError(), concatenate('\'', typeName(globalObject, callable), "' object is not callable"_s));
}

static JSValue raiseNotCallable(JSGlobalObject* globalObject, ThrowScope& scope, JSValue callable)
{
    throwException(globalObject, scope, createNotCallableError(globalObject, callable));
    return { };
}

JSC_DEFINE_HOST_FUNCTION(callWhatOnlyPythonCalls, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    NativeArguments given(callFrame);
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywords(globalObject, callFrame->jsCallee(), given.allFrom(0), given.keywordNames())));
}

// f(*values) can be given any number of them, and a call is not to fail, or to leave what is called nothing to run in, because they were put on the stack. Most of what can be called does not need them there: see
// callWithKeywords(). A function that is written in C++ finds them there, and is done with them when it returns, so for that they go there if they take no more of it than they leave.
static bool areBetterKeptOffStack(VM& vm, const ArgList& arguments)
{
    auto* stackPointer = static_cast<uint8_t*>(currentStackPointer());
    auto* limit = static_cast<uint8_t*>(vm.softStackLimit());
    size_t left = stackPointer > limit ? stackPointer - limit : 0;
    return arguments.size() * sizeof(Register) > left / 2;
}

JSValue call(JSGlobalObject* globalObject, JSValue callable, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (areBetterKeptOffStack(vm, arguments)) [[unlikely]]
        RELEASE_AND_RETURN(scope, callWithKeywords(globalObject, callable, arguments, nullptr));
    auto callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]]
        RELEASE_AND_RETURN(scope, callWithKeywords(globalObject, callable, arguments, nullptr));
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
    TextBuilder builder;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i)
            builder.append(i + 1 == names.size() ? (names.size() > 2 ? ", and "_s : " and "_s) : ", "_s);
        builder.append('\'', names[i], '\'');
    }
    return builder.tryFinish();
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
    // Which is wanted only if something is wrong.
    auto functionName = [&] { return nameOfFunction(globalObject, function, true); };

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
        PyTuple* rest = PyTuple::tryCreate(globalObject, given - taken);
        RETURN_IF_EXCEPTION(scope, false);
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
                raiseTypeError(globalObject, scope, concatenate(functionName(), "() got multiple values for argument '"_s, keywordString, '\''));
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
        raiseTypeError(globalObject, scope, concatenate(functionName(), "() got an unexpected keyword argument '"_s, keywordString, '\''));
        return false;
    }
    if (!positionalOnlyGivenByKeyword.isEmpty()) {
        TextBuilder list;
        for (size_t i = 0; i < positionalOnlyGivenByKeyword.size(); ++i)
            list.append(i ? ", "_s : ""_s, positionalOnlyGivenByKeyword[i]);
        raiseTypeError(globalObject, scope, concatenate(functionName(), "() got some positional-only arguments passed as keyword arguments: '"_s, list.tryFinish(), '\''));
        return false;
    }

    JSValue defaultsValue = function->getDirect(vm, names.private_defaults);
    PyTuple* defaults = defaultsValue ? uncheckedDowncast<PyTuple>(defaultsValue.asCell()) : nullptr;
    unsigned defaultCount = defaults ? defaults->length() : 0;

    if (given > positionalCount && !info.hasVariadic) {
        unsigned keywordOnlyGiven = 0;
        for (unsigned i = positionalCount; i < namedCount; ++i)
            keywordOnlyGiven += !!bound[i];
        TextBuilder message;
        message.append(functionName(), "() takes "_s);
        if (defaultCount)
            message.append("from "_s, positionalCount - defaultCount, " to "_s, positionalCount, " positional arguments"_s);
        else
            message.append(positionalCount, " positional argument"_s, positionalCount == 1 ? ""_s : "s"_s);
        message.append(" but "_s, given);
        if (keywordOnlyGiven)
            message.append(" positional argument"_s, given == 1 ? ""_s : "s"_s, " (and "_s, keywordOnlyGiven, " keyword-only argument"_s, keywordOnlyGiven == 1 ? ""_s : "s"_s, ')');
        message.append(given == 1 && !keywordOnlyGiven ? " was given"_s : " were given"_s);
        raiseTypeError(globalObject, scope, message.tryFinish());
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
        raiseTypeError(globalObject, scope, concatenate(functionName(), "() missing "_s, missing.size(), " required positional argument"_s, missing.size() == 1 ? ""_s : "s"_s, ": "_s, joinNames(missing)));
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
            raiseTypeError(globalObject, scope, concatenate(functionName(), "() missing "_s, missing.size(), " required keyword-only argument"_s, missing.size() == 1 ? ""_s : "s"_s, ": "_s, joinNames(missing)));
            return false;
        }
    }
    for (JSValue value : bound)
        result.append(value);
    return true;
}

JSValue callWithKeywords(JSGlobalObject* globalObject, JSValue callable, const ArgList& arguments, KeywordNames* keywordNames, JSValue thisValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]] {
        // See callWhatOnlyPythonCalls().
        JSValue instead = defaultExportToCall(globalObject, callable);
        RETURN_IF_EXCEPTION(scope, { });
        if (instead)
            RELEASE_AND_RETURN(scope, callWithKeywords(globalObject, instead, arguments, keywordNames, thisValue));
        if (!callable.isObject() || callable.asCell()->type() == ModuleNamespaceObjectType)
            return raiseNotCallable(globalObject, scope, callable);
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            raiseRecursionError(globalObject);
            return { };
        }
        RELEASE_AND_RETURN(scope, callInstance(globalObject, asObject(callable), arguments, keywordNames));
    }
    unsigned keywordCount = keywordNames ? keywordNames->length() : 0;

    // A function of Python's can be given a value for each of its parameters, in place of the arguments. What is more than it has names for is on its way to a tuple, or to an exception, and goes straight there.
    // So what goes on the stack is no more than the function has room for in any case, and a function that calls itself with a great many goes as deep as one that does with few.
    const FunctionInfo* info = pythonInfoOf(callable);
    if (info && !keywordCount && arguments.size() <= info->positionalCount) [[likely]]
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, thisValue, arguments));
    if (info) {
        MarkedArgumentBuffer bound;
        bool ok = bindArguments(globalObject, uncheckedDowncast<JSFunction>(callable.asCell()), *info, arguments, keywordNames, bound);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, globalObject->pyRealm()->boundArgumentsMarker(), bound));
    }

    // These pass on what they are given, so there is nothing to put it on the stack for. But a method of a function that is written in C++ is called as that is. What that says when it is given the wrong arguments
    // has in it the class that the method was got by way of, which it finds by looking at what called it: see functionString().
    JSCell* cell = callable.asCell();
    auto* boundMethod = cell->type() == PyBoundMethodType ? uncheckedDowncast<PyBoundMethod>(cell) : nullptr;
    if (boundMethod && boundMethod->function().inherits<PyNativeFunction>())
        boundMethod = nullptr;
    if (boundMethod || cell->type() == PyTypeType || isCallOfInstance(callData)) {
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            raiseRecursionError(globalObject);
            return { };
        }
        if (boundMethod)
            RELEASE_AND_RETURN(scope, boundMethod->call(globalObject, arguments, keywordNames));
        if (cell->type() == PyTypeType)
            RELEASE_AND_RETURN(scope, uncheckedDowncast<PyType>(cell)->call(globalObject, arguments, keywordNames));
        RELEASE_AND_RETURN(scope, callInstance(globalObject, asObject(cell), arguments, keywordNames));
    }

    bool keepsOffStack = areBetterKeptOffStack(vm, arguments);
    if (!keywordCount && !keepsOffStack) [[likely]]
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, thisValue, arguments));

    bool understandsKeywords = cell->inherits<PyNativeFunction>() || cell->type() == PyBoundMethodType;
    if (understandsKeywords && keepsOffStack) [[unlikely]] {
        // See NativeArguments.
        JSCellButterfly* values = JSCellButterfly::tryCreateFromArgList(vm, arguments);
        if (!keywordNames)
            keywordNames = KeywordNames::tryCreate(vm, CopyOnWriteArrayWithContiguous, 0);
        if (!values || !keywordNames)
            return raiseMemoryError(globalObject, scope);
        MarkedArgumentBuffer packed;
        packed.append(values);
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, keywordNames, packed));
    }
    if (understandsKeywords)
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, keywordNames, arguments));
    // A function of JavaScript's has them on the stack, and if there is no room for them it is as it would be in JavaScript.
    if (!keywordCount)
        RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, thisValue, arguments));

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
    RELEASE_AND_RETURN(scope, JSC::call(globalObject, callable, callData, thisValue, converted));
}

JSValue instantiate(JSGlobalObject* globalObject, PyType* type, const ArgList& arguments, KeywordNames* keywordNames)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    // type(x)
    if (type == realm->typeType() && arguments.size() - (keywordNames ? keywordNames->length() : 0) == 1) {
        if (keywordNames)
            return raiseTypeError(globalObject, scope, "type() takes no keyword arguments"_s);
        return typeOf(globalObject, arguments.at(0))->object();
    }
    // Which is said here, of type itself, so that it is not said that it takes exactly three.
    if (type == realm->typeType() && arguments.size() - (keywordNames ? keywordNames->length() : 0) != 3)
        return raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s);

    RELEASE_AND_RETURN(scope, instantiateFrom(globalObject, type, type, arguments, keywordNames));
}

JSValue instantiateFrom(JSGlobalObject* globalObject, PyType* type, PyType* from, const ArgList& arguments, KeywordNames* keywordNames)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    // Whether a constructor of JavaScript's is in the middle of making it, and has come to super(...).
    bool isContinuing = type != from;

    if (type->cannotBeInstantiated(vm))
        return raiseTypeError(globalObject, scope, concatenate("cannot create '"_s, type->nameString(globalObject), "' instances"_s));
    JSValue constructor = type->lookupFrom(vm, from, names.dunder_new);
    ASSERT(constructor);
    // slot_tp_new(): it is what the class has by that name, as `type.__new__` would find it. A function is a static method whether or not it says so, and whatever else has a __get__() is asked.
    constructor = bindDescriptor(globalObject, constructor, JSValue(), type);
    RETURN_IF_EXCEPTION(scope, { });

    auto prepend = [&] (MarkedArgumentBuffer& buffer, JSValue first) {
        buffer.append(first);
        for (unsigned i = 0; i < arguments.size(); ++i)
            buffer.append(arguments.at(i));
    };
    // What object.__new__ and object.__init__ make of arguments goes by whether the class has its own of the one and the other, to tell whose they are.
    // Here they are for whoever wants them, as arguments are in JavaScript.
    MarkedArgumentBuffer withType;
    JSValue instance;
    if (isContinuing && constructor.asCell() == realm->function(PyRealm::WellKnownFunction::ObjectNew))
        instance = call(globalObject, constructor, type->object());
    else {
        prepend(withType, type->object());
        instance = callWithKeywords(globalObject, constructor, withType, keywordNames);
    }
    RETURN_IF_EXCEPTION(scope, { });

    PyType* instanceType = typeOf(globalObject, instance);
    if (!instanceType->isSubtypeOf(type))
        return instance;
    JSValue initializer = isContinuing ? instanceType->lookupFrom(vm, from, names.dunder_init) : instanceType->lookup(vm, names.dunder_init);
    if (!initializer || (isContinuing && initializer.asCell() == realm->function(PyRealm::WellKnownFunction::ObjectInit)))
        return instance;
    // slot_tp_init()
    JSValue self;
    initializer = bindSpecial(globalObject, instanceType, initializer, instance, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result;
    if (self) {
        MarkedArgumentBuffer withInstance;
        prepend(withInstance, instance);
        result = callWithKeywords(globalObject, initializer, withInstance, keywordNames);
    } else
        result = callWithKeywords(globalObject, initializer, arguments, keywordNames);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(result))
        return raiseTypeError(globalObject, scope, concatenate("__init__() should return None, not '"_s, typeName(globalObject, result), '\''));
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
    raiseTypeError(globalObject, scope, concatenate(functionName, "() takes no keyword arguments"_s));
    return false;
}

bool NativeArguments::check(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral functionName, unsigned minimum, unsigned maximum) const
{
    if (!checkNoKeywords(globalObject, scope, functionName))
        return false;
    if (size() >= minimum && size() <= maximum)
        return true;

    // The instance or the class that comes first is not something that whoever called it thinks of having given.
    auto* callee = dynamicDowncast<PyNativeFunction>(m_callFrame->jsCallee());
    bool hasImplicitFirst = callee && callee->hasImplicitFirst() && minimum;
    unsigned given = size() - (hasImplicitFirst && size());
    minimum -= hasImplicitFirst;
    maximum -= hasImplicitFirst;

    // What takes none or one says so under its full name, and the rest under its own, as in CPython, where they are called in different ways.
    StringView name { functionName };
    if (minimum == maximum && minimum <= 1) {
        String qualified = name.toString();
        if (callee && callee->hasImplicitFirst() && callee->kind() != PyNativeFunction::Kind::New && callee->owner() && isType(callee->owner()))
            qualified = concatenate(asType(callee->owner())->nameString(globalObject), '.', name.substring(name.reverseFind('.') + 1));
        raiseTypeError(globalObject, scope, concatenate(qualified, minimum ? "() takes exactly one argument ("_s : "() takes no arguments ("_s, given, " given)"_s));
        return false;
    }
    if (hasImplicitFirst)
        name = name.substring(name.reverseFind('.') + 1);
    if (minimum == maximum)
        raiseTypeError(globalObject, scope, concatenate(name, " expected "_s, minimum, " arguments, got "_s, given));
    else if (given < minimum)
        raiseTypeError(globalObject, scope, concatenate(name, " expected at least "_s, minimum, " argument"_s, minimum == 1 ? ""_s : "s"_s, ", got "_s, given));
    else
        raiseTypeError(globalObject, scope, concatenate(name, " expected at most "_s, maximum, " argument"_s, maximum == 1 ? ""_s : "s"_s, ", got "_s, given));
    return false;
}

} } // namespace JSC::Python
