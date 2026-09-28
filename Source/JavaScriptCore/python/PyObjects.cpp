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
#include "PyObjects.h"

#include "JSCInlines.h"
#include "JSInternalFieldObjectImplInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"

namespace JSC {

// ---- PyInstance

const ClassInfo PyInstance::s_info = { "object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyInstance) };

template<typename Visitor>
void PyInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);
    thisObject->visitButterflyAndInlineStorage(visitor);
}

DEFINE_VISIT_CHILDREN(PyInstance);

PyInstance* PyInstance::create(VM& vm, Structure* structure)
{
    size_t inlineCapacity = structure->inlineCapacity();
    auto* instance = new (NotNull, allocateCell<PyInstance>(vm, allocationSize(inlineCapacity))) PyInstance(vm, structure, inlineCapacity);
    instance->finishCreation(vm);
    return instance;
}

Structure* PyInstance::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, unsigned inlineCapacity)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyInstanceType, StructureFlags | pythonCellFlags), info(), NonArray, inlineCapacity);
}

static JSC_DECLARE_HOST_FUNCTION(callInstance);

// instance(...) is type(instance).__call__(instance, ...)
JSC_DEFINE_HOST_FUNCTION(callInstance, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue instance = callFrame->jsCallee();
    JSValue self;
    JSValue function = Python::lookupSpecial(globalObject, instance, vm.pythonNames().dunder_call, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!function)
        return JSValue::encode(Python::raiseTypeError(globalObject, scope, makeString('\'', Python::typeName(globalObject, instance), "' object is not callable"_s)));
    MarkedArgumentBuffer arguments;
    if (self)
        arguments.append(self);
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    Python::NativeArguments given(callFrame);
    RELEASE_AND_RETURN(scope, JSValue::encode(Python::callWithKeywords(globalObject, function, arguments, given.keywordNames())));
}

static bool nothingIsOrdinary(VM&, PropertyName) { return false; }
static bool indicesAreOrdinary(VM&, PropertyName name) { return !!parseIndex(name); }
static bool indicesAndLengthAreOrdinary(VM& vm, PropertyName name) { return name == vm.propertyNames->length || parseIndex(name); }

PYTHON_DEFINE_EXOTIC_METHODS(PyInstance, nothingIsOrdinary)
PYTHON_DEFINE_EXOTIC_INDEX_METHODS(PyInstance)
PYTHON_DEFINE_EXOTIC_METHODS(PyBoxedValue, nothingIsOrdinary)
PYTHON_DEFINE_EXOTIC_METHODS(PyDict, nothingIsOrdinary)
PYTHON_DEFINE_EXOTIC_METHODS(PySet, nothingIsOrdinary)
PYTHON_DEFINE_EXOTIC_METHODS(PyDerivedList, indicesAndLengthAreOrdinary)
PYTHON_DEFINE_EXOTIC_METHODS(PyDerivedBytes, indicesAreOrdinary)

// ---- PyException

const ClassInfo PyException::s_info = { "Error"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyException) };

Structure* PyException::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ErrorInstanceType, StructureFlags | pythonCellFlags), info());
}

PyException* PyException::create(VM& vm, PyType* type)
{
    auto* exception = new (NotNull, allocateCell<PyException>(vm)) PyException(vm, type->instanceStructure(), type->errorType());
    exception->finishCreationForEmbedderError(vm);
    return exception;
}

// What ErrorInstance sees to by itself.
static bool isErrorInfo(VM& vm, PropertyName name)
{
    return name == vm.propertyNames->stack || name == vm.propertyNames->line || name == vm.propertyNames->column || name == vm.propertyNames->sourceURL;
}

// What is JavaScript's about an Error besides.
static bool isErrorProperty(VM& vm, PropertyName name)
{
    return name == vm.propertyNames->name || name == vm.propertyNames->message || name == vm.propertyNames->cause;
}

bool PyException::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (name.isSymbol() || isErrorInfo(vm, name))
        RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, name, slot));
    if (!isErrorProperty(vm, name))
        RELEASE_AND_RETURN(scope, Python::getOwnPropertySlotFromJavaScript(object, globalObject, name, slot, Base::getOwnPropertySlot));

    // What JavaScript has set it to, if it has.
    unsigned attributes;
    if (JSValue set = object->getDirect(vm, name, attributes); set && (attributes & PropertyAttribute::DontEnum)) {
        slot.setValue(object, attributes, set);
        return true;
    }
    JSValue value;
    if (name == vm.propertyNames->name)
        value = Python::typeOf(globalObject, object)->name();
    else if (slot.isVMInquiry())
        return false; // The rest may run something.
    else if (name == vm.propertyNames->message) {
        String text = Python::str(globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        value = jsString(vm, text);
    } else {
        value = object->getDirect(vm, vm.pythonNames().private_cause);
        if (!value || value.isUndefinedOrNull())
            return false;
    }
    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), value);
    return true;
}

bool PyException::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    if (name.isSymbol() || isErrorInfo(vm, name) || slot.thisValue() != JSValue(cell))
        return Base::put(cell, globalObject, name, value, slot);
    if (isErrorProperty(vm, name)) {
        slot.disableCaching();
        asObject(cell)->putDirect(vm, name, value, static_cast<unsigned>(PropertyAttribute::DontEnum));
        return true;
    }
    return Python::setPropertyFromJavaScript(globalObject, cell, name, value, slot);
}

bool PyException::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    if (name.isSymbol() || isErrorInfo(vm, name) || isErrorProperty(vm, name))
        return Base::deleteProperty(cell, globalObject, name, slot);
    return Python::deletePropertyFromJavaScript(globalObject, cell, name);
}

bool PyException::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    if (name.isSymbol() || isErrorInfo(vm, name))
        return Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    return Python::definePropertyFromJavaScript(globalObject, object, name, descriptor, shouldThrow);
}

bool PyException::preventExtensions(JSObject*, JSGlobalObject*)
{
    return false;
}

// ---- What is JavaScript's cell, and an instance of a class of Python's

const ClassInfo PyDerivedList::s_info = { "list"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyDerivedList) };
const ClassInfo PyDerivedBytes::s_info = { "bytes"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyDerivedBytes) };

Structure* PyDerivedList::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(DerivedArrayType, StructureFlags | pythonCellFlags), info(), ArrayWithUndecided);
}

Structure* PyDerivedBytes::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(Uint8ArrayType, StructureFlags | pythonCellFlags), info(), NonArray);
}

static CallData callDataOfInstance(JSCell* cell)
{
    CallData callData;
    JSObject* object = asObject(cell);
    VM& vm = cell->vm();
    if (Python::typeOf(object->globalObject(), object)->lookup(vm, vm.pythonNames().dunder_call)) {
        callData.type = CallData::Type::Native;
        callData.native.function = callInstance;
        callData.native.isBoundFunction = false;
        callData.native.isWasm = false;
    }
    return callData;
}

CallData PyInstance::getCallData(JSCell* cell)
{
    return callDataOfInstance(cell);
}

CallData PyNativeObject::getCallData(JSCell* cell)
{
    return callDataOfInstance(cell);
}

// ---- PyNativeFunction

const ClassInfo PyNativeFunction::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyNativeFunction) };

template<typename Visitor>
void PyNativeFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyNativeFunction>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_owner);
}

DEFINE_VISIT_CHILDREN(PyNativeFunction);

static BuiltinType classOfKind(PyNativeFunction::Kind kind)
{
    switch (kind) {
    case PyNativeFunction::Kind::Function:
    case PyNativeFunction::Kind::New:
    case PyNativeFunction::Kind::StaticMethod:
        return BuiltinType::BuiltinFunction;
    case PyNativeFunction::Kind::Method:
        return BuiltinType::MethodDescriptor;
    case PyNativeFunction::Kind::Wrapper:
        return BuiltinType::WrapperDescriptor;
    case PyNativeFunction::Kind::ClassMethod:
        return BuiltinType::ClassMethodDescriptor;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyNativeFunction* PyNativeFunction::create(VM& vm, JSGlobalObject* globalObject, unsigned length, const String& name, NativeFunction nativeFunction, Kind kind, JSObject* owner, unsigned data, ImplementationVisibility visibility, ASCIILiteral signature, Arguments arguments)
{
    using Python::BuiltinDescription;
    const BuiltinDescription* description = nullptr;
    if (owner && isType(owner)) {
        // It may be that CPython has it in a class that this one is derived from, and has no need of another.
        for (auto& ancestor : asType(owner)->mro()->span()) {
            description = Python::findAttributeDescription(asType(ancestor.get())->nameWithoutModule(globalObject), name);
            if (description)
                break;
        }
        if (description && description->kind == BuiltinDescription::Kind::WrapperDescriptor && kind == Kind::Method)
            kind = Kind::Wrapper;
        if (kind == Kind::Function)
            kind = Kind::StaticMethod;
    } else if (owner) {
        JSValue moduleName = owner->getDirect(vm, vm.pythonNames().dunder_name);
        if (moduleName && moduleName.isString())
            description = Python::findFunctionDescription(asString(moduleName)->value(globalObject).data, name);
    }
    if (arguments == Arguments::AreThoseOfTheClass || arguments == Arguments::AreThoseOfTheClassButNotChecked) {
        auto* typeDescription = Python::findTypeDescription(asType(owner)->nameWithoutModule(globalObject));
        if (signature.isNull() && typeDescription)
            signature = typeDescription->signature;
        // One that does not say is left to see to them itself.
        if (signature.isNull())
            arguments = Arguments::AreNotChecked;
    }
    if (signature.isNull() && description)
        signature = description->signature;
    // Only what nothing but compiled code can call goes without.
    RELEASE_ASSERT_WITH_MESSAGE(!signature.isNull() || visibility == ImplementationVisibility::Private, "A function of Python's that is written in C++ has a signature");

    NativeExecutable* executable = vm.getHostFunction(nativeFunction, visibility, NoIntrinsic, callHostFunctionAsConstructor, nullptr, length, name);
    auto* function = new (NotNull, allocateCell<PyNativeFunction>(vm)) PyNativeFunction(vm, executable, globalObject, globalObject->pyRealm()->nativeFunctionStructure(classOfKind(kind)), kind, owner, data);
    function->finishCreation(vm);
    function->m_description = description;
    if (!signature.isNull())
        function->setSignature(vm.pythonNames().signatureFor(signature), arguments);
    return function;
}

void PyNativeFunction::setSignature(const Python::NativeSignature* signature, Arguments arguments)
{
    m_signature = signature;
    m_takesArgumentsOfTheClass = arguments == Arguments::AreThoseOfTheClass || arguments == Arguments::AreThoseOfTheClassButNotChecked;
    // Any number will do for one that sees to them itself.
    if (signature->family() == Python::NativeSignature::Family::Unchecked || arguments == Arguments::AreNotChecked || arguments == Arguments::AreThoseOfTheClassButNotChecked)
        return;
    m_checksArguments = true;
    unsigned implicit = hasImplicitFirst();
    m_minimumArguments = signature->requiredPositionalCount() + implicit;
    m_maximumArguments = signature->hasVarPositional() ? std::numeric_limits<unsigned>::max() : signature->positionalCount() + implicit;
    // There is no number of them that will do without any being given by name.
    if (signature->requiredKeywordOnlyCount())
        m_minimumArguments = std::numeric_limits<unsigned>::max();
}

Structure* PyNativeFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags | IsImmutablePrototypeExoticObject), info());
}

// ---- PyTuple

const ClassInfo PyTuple::s_info = { "tuple"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyTuple) };

template<typename Visitor>
void PyTuple::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyTuple>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.appendValues(thisObject->values(), thisObject->m_length);
}

DEFINE_VISIT_CHILDREN(PyTuple);

PyTuple* PyTuple::create(VM& vm, Structure* structure, unsigned length)
{
    auto* tuple = new (NotNull, allocateCell<PyTuple>(vm, allocationSize(length))) PyTuple(vm, structure, length);
    for (unsigned i = 0; i < length; ++i)
        tuple->values()[i].setStartingValue(jsUndefined());
    tuple->finishCreation(vm);
    return tuple;
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, unsigned length)
{
    PyRealm* realm = globalObject->pyRealm();
    if (!length && realm->emptyTuple())
        return realm->emptyTuple();
    return create(globalObject->vm(), realm->tupleStructure(), length);
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, std::span<const JSValue> values)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = create(globalObject, values.size());
    for (unsigned i = 0; i < values.size(); ++i)
        tuple->initializeAt(vm, i, values[i]);
    return tuple;
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, std::initializer_list<JSValue> values)
{
    return create(globalObject, std::span<const JSValue> { values.begin(), values.size() });
}

PyTuple* PyTuple::createFromArguments(JSGlobalObject* globalObject, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = create(globalObject, arguments.size());
    for (unsigned i = 0; i < arguments.size(); ++i)
        tuple->initializeAt(vm, i, arguments.at(i));
    return tuple;
}

Structure* PyTuple::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyTupleType, StructureFlags | pythonCellFlags), info());
}

bool PyTuple::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto* thisObject = uncheckedDowncast<PyTuple>(object);
    if (propertyName == vm.propertyNames->length) {
        slot.setValue(thisObject, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, jsNumber(thisObject->length()));
        return true;
    }
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return getOwnPropertySlotByIndex(object, globalObject, *index, slot);
    if (propertyName.isSymbol())
        return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
    return Python::getOwnPropertySlotFromJavaScript(object, globalObject, propertyName, slot, Base::getOwnPropertySlot);
}

bool PyTuple::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    if (name.isSymbol() || slot.thisValue() != JSValue(cell))
        return Base::put(cell, globalObject, name, value, slot);
    // Its items and its length are not to be set, which Python says in its own way.
    return Python::setPropertyFromJavaScript(globalObject, cell, name, value, slot);
}

bool PyTuple::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    if (name.isSymbol())
        return Base::deleteProperty(cell, globalObject, name, slot);
    if (indicesAndLengthAreOrdinary(globalObject->vm(), name))
        return false;
    return Python::deletePropertyFromJavaScript(globalObject, cell, name);
}

bool PyTuple::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    if (name.isSymbol())
        return Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    return Python::definePropertyFromJavaScript(globalObject, object, name, descriptor, shouldThrow);
}

bool PyTuple::preventExtensions(JSObject*, JSGlobalObject*)
{
    return false;
}

bool PyTuple::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject*, unsigned index, PropertySlot& slot)
{
    auto* thisObject = uncheckedDowncast<PyTuple>(object);
    if (index >= thisObject->length())
        return false;
    slot.setValue(thisObject, PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, thisObject->at(index));
    return true;
}

void PyTuple::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto* thisObject = uncheckedDowncast<PyTuple>(object);
    for (unsigned i = 0; i < thisObject->length(); ++i)
        propertyNames.add(Identifier::from(vm, i));
    if (mode == DontEnumPropertiesMode::Include)
        propertyNames.add(vm.propertyNames->length);
    thisObject->getOwnNonIndexPropertyNames(globalObject, propertyNames, mode);
}

// ---- The small ones

#define DEFINE_PYTHON_CELL(ClassName, pythonName, jsType) \
    const ClassInfo ClassName::s_info = { pythonName ""_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ClassName) }; \
    Structure* ClassName::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype) \
    { \
        return Structure::create(vm, globalObject, prototype, TypeInfo(jsType, StructureFlags | pythonCellFlags), info()); \
    } \
    DEFINE_VISIT_CHILDREN(ClassName);

template<typename Visitor>
void PyBoundMethod::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyBoundMethod>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_function);
    visitor.append(thisObject->m_self);
}

DEFINE_PYTHON_CELL(PyBoundMethod, "method", PyBoundMethodType)

PyBoundMethod* PyBoundMethod::create(JSGlobalObject* globalObject, JSValue function, JSValue self)
{
    VM& vm = globalObject->vm();
    // What Python calls it depends on what it is a method from.
    BuiltinType type = BuiltinType::Method;
    if (auto* native = dynamicDowncast<PyNativeFunction>(function))
        type = native->kind() == PyNativeFunction::Kind::Wrapper ? BuiltinType::MethodWrapper : BuiltinType::BuiltinFunction;
    auto* method = new (NotNull, allocateCell<PyBoundMethod>(vm)) PyBoundMethod(vm, globalObject->pyRealm()->structureFor(type), function, self);
    method->finishCreation(vm);
    return method;
}

static JSC_DECLARE_HOST_FUNCTION(callBoundMethod);

JSC_DEFINE_HOST_FUNCTION(callBoundMethod, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* method = uncheckedDowncast<PyBoundMethod>(callFrame->jsCallee());
    MarkedArgumentBuffer arguments;
    arguments.append(method->self());
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    Python::NativeArguments given(callFrame);
    return JSValue::encode(Python::callWithKeywords(globalObject, method->function(), arguments, given.keywordNames()));
}

CallData PyBoundMethod::getCallData(JSCell*)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callBoundMethod;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

template<typename Visitor>
void PyRange::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyRange>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_start);
    visitor.append(thisObject->m_stop);
    visitor.append(thisObject->m_step);
    visitor.append(thisObject->m_length);
}

DEFINE_PYTHON_CELL(PyRange, "range", PyRangeType)

template<typename Visitor>
void PySlice::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySlice>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_start);
    visitor.append(thisObject->m_stop);
    visitor.append(thisObject->m_step);
}

DEFINE_PYTHON_CELL(PySlice, "slice", PySliceType)

PySlice* PySlice::create(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step)
{
    VM& vm = globalObject->vm();
    auto* slice = new (NotNull, allocateCell<PySlice>(vm)) PySlice(vm, globalObject->pyRealm()->structureFor(BuiltinType::Slice), start, stop, step);
    slice->finishCreation(vm);
    return slice;
}

std::optional<PySlice::Bounds> PySlice::unpack(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto evaluate = [&] (JSValue value) -> std::optional<int64_t> {
        JSValue self;
        if (!Python::isInt(value) && !value.isBoolean()) {
            JSValue index = Python::lookupSpecial(globalObject, value, vm.pythonNames().dunder_index, self);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (!index) {
                Python::raiseTypeError(globalObject, scope, "slice indices must be integers or None or have an __index__ method"_s);
                return std::nullopt;
            }
        }
        RELEASE_AND_RETURN(scope, Python::toIndex(globalObject, value, true));
    };

    Bounds result;
    result.step = 1;
    if (!Python::isNone(step())) {
        auto value = evaluate(step());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        result.step = *value;
        if (!result.step) {
            Python::raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
            return std::nullopt;
        }
        // So that it can be negated.
        result.step = std::max(result.step, -std::numeric_limits<int64_t>::max());
    }
    bool isBackwards = result.step < 0;
    result.start = isBackwards ? std::numeric_limits<int64_t>::max() : 0;
    if (!Python::isNone(start())) {
        auto value = evaluate(start());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        result.start = *value;
    }
    result.stop = isBackwards ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    if (!Python::isNone(stop())) {
        auto value = evaluate(stop());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        result.stop = *value;
    }
    return result;
}

PySlice::Indices PySlice::adjust(Bounds bounds, int64_t length)
{
    bool isBackwards = bounds.step < 0;
    auto clamp = [&] (int64_t i) {
        if (i < 0) {
            i += length;
            return i < 0 ? (isBackwards ? -1 : 0) : i;
        }
        return i >= length ? (isBackwards ? length - 1 : length) : i;
    };
    Indices result;
    result.step = bounds.step;
    result.start = clamp(bounds.start);
    result.stop = clamp(bounds.stop);
    if (isBackwards)
        result.length = result.stop < result.start ? (result.start - result.stop - 1) / (-result.step) + 1 : 0;
    else
        result.length = result.start < result.stop ? (result.stop - result.start - 1) / result.step + 1 : 0;
    return result;
}

template<typename Visitor>
void PyNativeObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    // The fields are the base's to visit.
    Base::visitChildren(cell, visitor);
}

DEFINE_PYTHON_CELL(PyNativeObject, "object", PyNativeObjectType)

PyNativeObject* PyNativeObject::create(VM& vm, Structure* structure)
{
    auto* object = new (NotNull, allocateCell<PyNativeObject>(vm)) PyNativeObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

PyNativeObject* PyNativeObject::create(JSGlobalObject* globalObject, BuiltinType type, JSValue a, JSValue b, JSValue c, JSValue d)
{
    VM& vm = globalObject->vm();
    PyNativeObject* object = create(vm, globalObject->pyRealm()->structureFor(type));
    object->setField(vm, 0, a);
    object->setField(vm, 1, b);
    object->setField(vm, 2, c);
    object->setField(vm, 3, d);
    return object;
}

template<typename Visitor>
void PyTypingObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
}

DEFINE_PYTHON_CELL(PyTypingObject, "object", ObjectType)

PyTypingObject* PyTypingObject::create(VM& vm, Structure* structure)
{
    auto* object = new (NotNull, allocateCell<PyTypingObject>(vm)) PyTypingObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PyGetSetDescriptor::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyGetSetDescriptor>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_owner);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_storage);
    visitor.append(thisObject->m_initialValue);
}

DEFINE_PYTHON_CELL(PyGetSetDescriptor, "getset_descriptor", ObjectType)

PyGetSetDescriptor* PyGetSetDescriptor::create(JSGlobalObject* globalObject, PyType* owner, const String& name, Getter getter, Setter setter, bool isMember, ASCIILiteral doc)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    // Which of the two it is called is as CPython has it, if it has it.
    // A class that is written in Python may be called what a built-in one is.
    const Python::BuiltinDescription* description = owner->hasFlag(PyType::IsHeapType) ? nullptr : Python::findAttributeDescription(owner->nameWithoutModule(globalObject), name);
    if (description) {
        isMember = description->kind == Python::BuiltinDescription::Kind::MemberDescriptor;
        doc = description->doc;
    }
    auto* descriptor = new (NotNull, allocateCell<PyGetSetDescriptor>(vm)) PyGetSetDescriptor(vm, realm->structureFor(isMember ? BuiltinType::MemberDescriptor : BuiltinType::GetSetDescriptor), getter, setter);
    descriptor->finishCreation(vm);
    descriptor->m_doc = doc;
    descriptor->m_isMember = isMember;
    descriptor->m_owner.set(vm, descriptor, owner);
    descriptor->m_name.set(vm, descriptor, jsString(vm, name));
    return descriptor;
}

PyGetSetDescriptor* PyGetSetDescriptor::createForSlot(JSGlobalObject* globalObject, PyType* owner, JSString* name, Symbol* storage, JSValue initialValue)
{
    VM& vm = globalObject->vm();
    auto* descriptor = new (NotNull, allocateCell<PyGetSetDescriptor>(vm)) PyGetSetDescriptor(vm, globalObject->pyRealm()->structureFor(BuiltinType::MemberDescriptor), nullptr, nullptr);
    descriptor->finishCreation(vm);
    descriptor->m_isMember = true;
    descriptor->m_owner.set(vm, descriptor, owner);
    descriptor->m_name.set(vm, descriptor, name);
    descriptor->m_storage.set(vm, descriptor, storage);
    if (initialValue)
        descriptor->m_initialValue.set(vm, descriptor, initialValue);
    return descriptor;
}

template<typename Visitor>
void PyBoxedValue::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyBoxedValue>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_value);
}

DEFINE_PYTHON_CELL(PyBoxedValue, "object", PyBoxedValueType)

PyBoxedValue* PyBoxedValue::create(VM& vm, Structure* structure, JSValue value)
{
    auto* object = new (NotNull, allocateCell<PyBoxedValue>(vm)) PyBoxedValue(vm, structure, value);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PyIterator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyIterator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_a);
    visitor.append(thisObject->m_b);
}

DEFINE_PYTHON_CELL(PyIterator, "iterator", PyIteratorType)

PyIterator* PyIterator::create(JSGlobalObject* globalObject, Kind kind, PyDict* dict)
{
    PyTuple* backingKeys = dict->backing() ? dict->backingKeys(globalObject) : nullptr;
    bool isReverse = kind == Kind::DictReverseKeys || kind == Kind::DictReverseValues || kind == Kind::DictReverseItems;
    int64_t start = isReverse ? (backingKeys ? backingKeys->length() : 0) + dict->ownTable().entryCount() : 0;
    return create(globalObject, kind, dict, backingKeys ? JSValue(backingKeys) : JSValue(), start, dict->size());
}

BuiltinType PyIterator::typeFor(Kind kind)
{
    switch (kind) {
    case Kind::List:
        return BuiltinType::ListIterator;
    case Kind::ListReverse:
        return BuiltinType::ListReverseIterator;
    case Kind::Tuple:
        return BuiltinType::TupleIterator;
    case Kind::Range:
        return BuiltinType::RangeIterator;
    case Kind::LongRange:
        return BuiltinType::LongRangeIterator;
    case Kind::AsciiStr:
        return BuiltinType::StrAsciiIterator;
    case Kind::Str:
        return BuiltinType::StrIterator;
    case Kind::Bytes:
        return BuiltinType::BytesIterator;
    case Kind::ByteArray:
        return BuiltinType::ByteArrayIterator;
    case Kind::Memory:
        return BuiltinType::MemoryIterator;
    case Kind::DictKeys:
        return BuiltinType::DictKeyIterator;
    case Kind::DictValues:
        return BuiltinType::DictValueIterator;
    case Kind::DictItems:
        return BuiltinType::DictItemIterator;
    case Kind::DictReverseKeys:
        return BuiltinType::DictReverseKeyIterator;
    case Kind::DictReverseValues:
        return BuiltinType::DictReverseValueIterator;
    case Kind::DictReverseItems:
        return BuiltinType::DictReverseItemIterator;
    case Kind::Set:
        return BuiltinType::SetIterator;
    case Kind::Sequence:
    case Kind::JavaScript:
        return BuiltinType::SequenceIterator;
    case Kind::Callable:
        return BuiltinType::CallableIterator;
    case Kind::Enumerate:
        return BuiltinType::Enumerate;
    case Kind::Zip:
        return BuiltinType::Zip;
    case Kind::Map:
        return BuiltinType::Map;
    case Kind::Filter:
        return BuiltinType::Filter;
    case Kind::Reversed:
        return BuiltinType::Reversed;
    case Kind::CodeLines:
        return BuiltinType::LineIterator;
    case Kind::CodePositions:
        return BuiltinType::PositionsIterator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyIterator* PyIterator::create(JSGlobalObject* globalObject, Structure* structure, Kind kind, JSValue a, JSValue b, int64_t index, int64_t stop, int64_t step)
{
    VM& vm = globalObject->vm();
    auto* iterator = new (NotNull, allocateCell<PyIterator>(vm)) PyIterator(vm, structure, kind, a, b, index, stop, step);
    iterator->finishCreation(vm);
    iterator->m_isOfDerivedClass = structure != globalObject->pyRealm()->structureFor(typeFor(kind));
    return iterator;
}

PyIterator* PyIterator::create(JSGlobalObject* globalObject, Kind kind, JSValue a, JSValue b, int64_t index, int64_t stop, int64_t step)
{
    return create(globalObject, globalObject->pyRealm()->structureFor(typeFor(kind)), kind, a, b, index, stop, step);
}

PyIterator* PyIterator::copy(JSGlobalObject* globalObject) const
{
    return create(globalObject, structure(), m_kind, m_a.get(), m_b.get(), m_index, m_stop, m_step);
}

JSValue PyIterator::next(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Once it has run out it stays that way.
    if (!m_a && m_kind != Kind::Range)
        return { };

    switch (m_kind) {
    case Kind::List: {
        auto* list = uncheckedDowncast<JSArray>(m_a.get().asCell());
        if (m_index < 0 || m_index >= list->length()) {
            finish();
            return { };
        }
        RELEASE_AND_RETURN(scope, list->getIndex(globalObject, static_cast<unsigned>(m_index++)));
    }
    case Kind::ListReverse: {
        auto* list = uncheckedDowncast<JSArray>(m_a.get().asCell());
        if (m_index < 0 || m_index >= list->length()) {
            // It keeps the list, and __setstate__() can set it going again.
            m_index = -1;
            return { };
        }
        RELEASE_AND_RETURN(scope, list->getIndex(globalObject, static_cast<unsigned>(m_index--)));
    }
    case Kind::Tuple:
    case Kind::CodeLines:
    case Kind::CodePositions: {
        auto* tuple = uncheckedDowncast<PyTuple>(m_a.get().asCell());
        if (m_index >= tuple->length()) {
            finish();
            return { };
        }
        return tuple->at(m_index++);
    }
    case Kind::Range: {
        if (m_stop <= 0)
            return { };
        --m_stop;
        int64_t value = m_index;
        m_index = static_cast<int64_t>(static_cast<uint64_t>(m_index) + static_cast<uint64_t>(m_step));
        RELEASE_AND_RETURN(scope, Python::intFromInt64(globalObject, value));
    }
    case Kind::LongRange: {
        auto* range = uncheckedDowncast<PyRange>(m_a.get().asCell());
        JSValue value = Python::nextOfLongRange(globalObject, range, m_b.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            return { };
        JSValue following = Python::numberBinaryOperation(globalObject, Python::BinaryOperator::Add, m_b.get(), jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        m_b.set(vm, this, following);
        return value;
    }
    case Kind::AsciiStr:
    case Kind::Str: {
        JSString* string = asString(m_a.get());
        if (m_index >= string->length()) {
            finish();
            return { };
        }
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned index = m_index;
        // One character, which is two code units if it is outside the basic plane.
        unsigned size = index + 1 < view->length() && U16_IS_LEAD(view[index]) && U16_IS_TRAIL(view[index + 1]) ? 2 : 1;
        m_index += size;
        if (size == 1)
            return jsSingleCharacterString(vm, view[index]);
        return jsSubstring(globalObject, string, index, size);
    }
    case Kind::Set: {
        auto* set = uncheckedDowncast<PySet>(m_a.get().asCell());
        if (set->size() != static_cast<uint64_t>(m_stop)) {
            m_stop = -1; // It goes on saying so.
            return Python::raise(globalObject, scope, BuiltinType::RuntimeError, "Set changed size during iteration"_s);
        }
        while (m_index < set->entryCount()) {
            if (JSValue key = set->keyAt(m_index++)) {
                ++m_step;
                return key;
            }
        }
        finish();
        return { };
    }
    case Kind::DictKeys:
    case Kind::DictValues:
    case Kind::DictItems:
    case Kind::DictReverseKeys:
    case Kind::DictReverseValues:
    case Kind::DictReverseItems: {
        PyDict* dict = asDict(m_a.get());
        if (dict->size() != static_cast<uint64_t>(m_stop)) {
            m_stop = -1;
            return Python::raise(globalObject, scope, BuiltinType::RuntimeError, "dictionary changed size during iteration"_s);
        }
        // First the keys that were in the object that backs it, if one does, and then its own table.
        auto* backingKeys = m_b ? uncheckedDowncast<PyTuple>(m_b.get().asCell()) : nullptr;
        int64_t backingCount = backingKeys ? backingKeys->length() : 0;
        PyHashTable& table = dict->ownTable();
        bool isReverse = this->isReverse();
        while (isReverse ? m_index > 0 : m_index < backingCount + table.entryCount()) {
            int64_t position = isReverse ? --m_index : m_index++;
            JSValue key;
            JSValue value;
            if (position < backingCount) {
                key = backingKeys->at(position);
                value = dict->get(globalObject, key);
            } else {
                key = table.keyAt(position - backingCount);
                value = key ? table.valueAt(position - backingCount) : JSValue();
            }
            if (!value)
                continue;
            ++m_step;
            if (m_kind == Kind::DictValues || m_kind == Kind::DictReverseValues)
                return value;
            if (m_kind == Kind::DictItems || m_kind == Kind::DictReverseItems)
                return PyTuple::create(globalObject, { key, value });
            return key;
        }
        finish();
        return { };
    }
    case Kind::Sequence: {
        JSValue value = Python::getItem(globalObject, m_a.get(), Python::intFromInt64(globalObject, m_index));
        if (scope.exception()) [[unlikely]] {
            if (Python::catchException(globalObject, BuiltinType::IndexError) || Python::catchException(globalObject, BuiltinType::StopIteration))
                finish();
            return { };
        }
        ++m_index;
        return value;
    }
    case Kind::Reversed: {
        if (m_index < 0) {
            finish();
            return { };
        }
        JSValue value = Python::getItem(globalObject, m_a.get(), Python::intFromInt64(globalObject, m_index));
        if (scope.exception()) [[unlikely]] {
            if (Python::catchException(globalObject, BuiltinType::IndexError) || Python::catchException(globalObject, BuiltinType::StopIteration))
                finish();
            return { };
        }
        --m_index;
        return value;
    }
    case Kind::Callable: {
        JSValue value = Python::call(globalObject, m_a.get());
        if (scope.exception()) [[unlikely]] {
            if (Python::catchException(globalObject, BuiltinType::StopIteration))
                finish();
            return { };
        }
        bool isSentinel = Python::isEqual(globalObject, value, m_b.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (isSentinel) {
            finish();
            return { };
        }
        return value;
    }
    case Kind::Enumerate: {
        JSValue value = Python::iteratorNext(globalObject, m_a.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            return { };
        JSValue index = m_b ? m_b.get() : Python::intFromInt64(globalObject, m_index);
        if (!m_b && m_index < std::numeric_limits<int64_t>::max())
            ++m_index;
        else {
            JSValue following = Python::numberBinaryOperation(globalObject, Python::BinaryOperator::Add, index, jsNumber(1));
            RETURN_IF_EXCEPTION(scope, { });
            m_b.set(vm, this, following);
        }
        return PyTuple::create(globalObject, { index, value });
    }
    case Kind::Zip:
    case Kind::Map: {
        bool isZip = m_kind == Kind::Zip;
        auto* iterators = uncheckedDowncast<PyTuple>((isZip ? m_a : m_b).get().asCell());
        ASCIILiteral name = isZip ? "zip"_s : "map"_s;
        unsigned count = iterators->length();
        if (!count)
            return { };
        MarkedArgumentBuffer values;
        for (unsigned i = 0; i < count; ++i) {
            JSValue value = Python::iteratorNext(globalObject, iterators->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (value) {
                values.append(value);
                continue;
            }
            if (!m_index)
                return { };
            // strict=True: they all have to run out together.
            if (i)
                return Python::raiseValueError(globalObject, scope, makeString(name, "() argument "_s, i + 1, " is shorter than argument"_s, i == 1 ? " "_s : "s 1-"_s, i));
            for (unsigned j = 1; j < count; ++j) {
                JSValue other = Python::iteratorNext(globalObject, iterators->at(j));
                RETURN_IF_EXCEPTION(scope, { });
                if (other)
                    return Python::raiseValueError(globalObject, scope, makeString(name, "() argument "_s, j + 1, " is longer than argument"_s, j == 1 ? " "_s : "s 1-"_s, j));
            }
            return { };
        }
        if (isZip)
            return PyTuple::createFromArguments(globalObject, values);
        RELEASE_AND_RETURN(scope, Python::call(globalObject, m_a.get(), values));
    }
    case Kind::Filter: {
        while (true) {
            JSValue value = Python::iteratorNext(globalObject, m_b.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (!value)
                return { };
            JSValue verdict = value;
            if (!Python::isNone(m_a.get())) {
                verdict = Python::call(globalObject, m_a.get(), value);
                RETURN_IF_EXCEPTION(scope, { });
            }
            bool keep = Python::isTrue(globalObject, verdict);
            RETURN_IF_EXCEPTION(scope, { });
            if (keep)
                return value;
        }
    }
    case Kind::JavaScript: {
        JSValue result = JSC::call(globalObject, m_b.get(), m_a.get(), ArgList(), "next is not a function"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result.isObject())
            return Python::raiseTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        JSValue done = asObject(result)->get(globalObject, vm.propertyNames->done);
        RETURN_IF_EXCEPTION(scope, { });
        if (done.toBoolean(globalObject)) {
            finish();
            return { };
        }
        RELEASE_AND_RETURN(scope, asObject(result)->get(globalObject, vm.propertyNames->value));
    }
    case Kind::Memory: {
        if (m_index >= m_stop) {
            finish();
            return { };
        }
        RELEASE_AND_RETURN(scope, Python::getItem(globalObject, m_a.get(), Python::intFromInt64(globalObject, m_index++)));
    }
    case Kind::Bytes:
    case Kind::ByteArray: {
        auto* view = uncheckedDowncast<JSUint8Array>(m_a.get().asCell());
        if (m_index < 0 || view->isDetached() || static_cast<uint64_t>(m_index) >= view->length()) {
            finish();
            return { };
        }
        return jsNumber(view->typedVector()[m_index++]);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC
