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
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PythonOperations.h"

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
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyInstanceType, StructureFlags), info(), NonArray, inlineCapacity);
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

CallData PyInstance::getCallData(JSCell*)
{
    // Whether its class has __call__ is found out when it is called. FIXME: typeof says "function" of every instance.
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callInstance;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
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

PyNativeFunction* PyNativeFunction::create(VM& vm, JSGlobalObject* globalObject, unsigned length, const String& name, NativeFunction nativeFunction, Kind kind, JSObject* owner, unsigned data)
{
    NativeExecutable* executable = vm.getHostFunction(nativeFunction, ImplementationVisibility::Public, NoIntrinsic, callHostFunctionAsConstructor, nullptr, length, name);
    auto* function = new (NotNull, allocateCell<PyNativeFunction>(vm)) PyNativeFunction(vm, executable, globalObject, globalObject->pyRealm()->nativeFunctionStructure(), kind, owner, data);
    function->finishCreation(vm);
    return function;
}

Structure* PyNativeFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags), info());
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
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyTupleType, StructureFlags), info());
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
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
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
        return Structure::create(vm, globalObject, prototype, TypeInfo(jsType, StructureFlags), info()); \
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
    auto* method = new (NotNull, allocateCell<PyBoundMethod>(vm)) PyBoundMethod(vm, globalObject->pyRealm()->structureFor(BuiltinType::Method), function, self);
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
    Base::visitChildren(cell, visitor);
}

DEFINE_PYTHON_CELL(PyRange, "range", PyRangeType)

PyRange::PyRange(VM& vm, Structure* structure, int64_t start, int64_t stop, int64_t step)
    : Base(vm, structure)
    , m_start(start)
    , m_stop(stop)
    , m_step(step)
{
    if (step > 0)
        m_length = start < stop ? static_cast<int64_t>((static_cast<uint64_t>(stop) - static_cast<uint64_t>(start) - 1) / static_cast<uint64_t>(step)) + 1 : 0;
    else
        m_length = start > stop ? static_cast<int64_t>((static_cast<uint64_t>(start) - static_cast<uint64_t>(stop) - 1) / (0 - static_cast<uint64_t>(step))) + 1 : 0;
}

PyRange* PyRange::create(JSGlobalObject* globalObject, int64_t start, int64_t stop, int64_t step)
{
    VM& vm = globalObject->vm();
    auto* range = new (NotNull, allocateCell<PyRange>(vm)) PyRange(vm, globalObject->pyRealm()->structureFor(BuiltinType::Range), start, stop, step);
    range->finishCreation(vm);
    return range;
}

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

std::optional<PySlice::Indices> PySlice::indices(JSGlobalObject* globalObject, int64_t length) const
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

    Indices result;
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
    auto clamp = [&] (JSValue value, int64_t whenAbsent) -> std::optional<int64_t> {
        if (Python::isNone(value))
            return whenAbsent;
        auto index = evaluate(value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        int64_t i = *index;
        if (i < 0) {
            i += length;
            if (i < 0)
                i = isBackwards ? -1 : 0;
        } else if (i >= length)
            i = isBackwards ? length - 1 : length;
        return i;
    };

    auto first = clamp(start(), isBackwards ? length - 1 : 0);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto last = clamp(stop(), isBackwards ? -1 : length);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    result.start = *first;
    result.stop = *last;
    if (isBackwards)
        result.length = result.stop < result.start ? (result.start - result.stop - 1) / (-result.step) + 1 : 0;
    else
        result.length = result.start < result.stop ? (result.stop - result.start - 1) / result.step + 1 : 0;
    return result;
}

template<typename Visitor>
void PyNativeObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyNativeObject>(cell);
    Base::visitChildren(thisObject, visitor);
    for (auto& field : thisObject->m_fields)
        visitor.append(field);
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
void PyGetSetDescriptor::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyGetSetDescriptor>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_owner);
    visitor.append(thisObject->m_name);
}

DEFINE_PYTHON_CELL(PyGetSetDescriptor, "getset_descriptor", ObjectType)

PyGetSetDescriptor* PyGetSetDescriptor::create(JSGlobalObject* globalObject, PyType* owner, const String& name, Getter getter, Setter setter)
{
    VM& vm = globalObject->vm();
    auto* descriptor = new (NotNull, allocateCell<PyGetSetDescriptor>(vm)) PyGetSetDescriptor(vm, globalObject->pyRealm()->structureFor(BuiltinType::GetSetDescriptor), getter, setter);
    descriptor->finishCreation(vm);
    descriptor->m_owner.set(vm, descriptor, owner);
    descriptor->m_name.set(vm, descriptor, jsString(vm, name));
    return descriptor;
}

template<typename Visitor>
void PyNamespace::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
}

DEFINE_PYTHON_CELL(PyNamespace, "namespace", PyNamespaceType)

PyNamespace* PyNamespace::create(VM& vm, Structure* structure)
{
    auto* object = new (NotNull, allocateCell<PyNamespace>(vm)) PyNamespace(vm, structure);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PyNameErrorRaiser::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
}

DEFINE_PYTHON_CELL(PyNameErrorRaiser, "namespace", ObjectType)

PyNameErrorRaiser* PyNameErrorRaiser::create(VM& vm, JSGlobalObject* globalObject)
{
    auto* object = new (NotNull, allocateCell<PyNameErrorRaiser>(vm)) PyNameErrorRaiser(vm, createStructure(vm, globalObject, jsNull()));
    object->finishCreation(vm);
    return object;
}

bool PyNameErrorRaiser::getOwnPropertySlot(JSObject*, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Only for what is after the value. `name in namespace` is a question, and the answer is no.
    if (slot.internalMethodType() != PropertySlot::InternalMethodType::Get || propertyName.isSymbol())
        return false;
    Python::raise(globalObject, scope, BuiltinType::NameError, makeString("name '"_s, StringView(propertyName.uid()), "' is not defined"_s));
    return false;
}

template<typename Visitor>
void PyModule::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyModule>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_namespace);
}

DEFINE_PYTHON_CELL(PyModule, "module", PyModuleType)

PyModule* PyModule::create(JSGlobalObject* globalObject, const String& name)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    PyNamespace* namespaceObject = PyNamespace::create(vm, realm->namespaceStructure());
    auto* module = new (NotNull, allocateCell<PyModule>(vm)) PyModule(vm, realm->structureFor(BuiltinType::Module), namespaceObject);
    module->finishCreation(vm);
    namespaceObject->putDirect(vm, names.dunder_name, jsString(vm, name));
    namespaceObject->putDirect(vm, names.dunder_doc, jsUndefined());
    namespaceObject->putDirect(vm, names.dunder_package, jsUndefined());
    namespaceObject->putDirect(vm, names.dunder_loader, jsUndefined());
    namespaceObject->putDirect(vm, names.dunder_spec, jsUndefined());
    return module;
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
    case Kind::Str:
        return BuiltinType::StrIterator;
    case Kind::Bytes:
        return BuiltinType::BytesIterator;
    case Kind::DictKeys:
        return BuiltinType::DictKeyIterator;
    case Kind::DictValues:
        return BuiltinType::DictValueIterator;
    case Kind::DictItems:
        return BuiltinType::DictItemIterator;
    case Kind::DictReverseKeys:
        return BuiltinType::DictReverseKeyIterator;
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyIterator* PyIterator::create(JSGlobalObject* globalObject, Kind kind, JSValue a, JSValue b, int64_t index, int64_t stop, int64_t step)
{
    VM& vm = globalObject->vm();
    auto* iterator = new (NotNull, allocateCell<PyIterator>(vm)) PyIterator(vm, globalObject->pyRealm()->structureFor(typeFor(kind)), kind, a, b, index, stop, step);
    iterator->finishCreation(vm);
    return iterator;
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
        if (m_index >= list->length()) {
            finish();
            return { };
        }
        RELEASE_AND_RETURN(scope, list->getIndex(globalObject, static_cast<unsigned>(m_index++)));
    }
    case Kind::ListReverse: {
        auto* list = uncheckedDowncast<JSArray>(m_a.get().asCell());
        if (m_index < 0 || m_index >= list->length()) {
            finish();
            return { };
        }
        RELEASE_AND_RETURN(scope, list->getIndex(globalObject, static_cast<unsigned>(m_index--)));
    }
    case Kind::Tuple: {
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
    case Kind::DictKeys:
    case Kind::DictValues:
    case Kind::DictItems:
    case Kind::Set: {
        auto* table = uncheckedDowncast<PyHashTable>(m_a.get().asCell());
        if (table->size() != static_cast<uint64_t>(m_stop)) {
            m_stop = -1; // It goes on saying so.
            return Python::raise(globalObject, scope, BuiltinType::RuntimeError, m_kind == Kind::Set ? "Set changed size during iteration"_s : "dictionary changed size during iteration"_s);
        }
        while (m_index < table->entryCount()) {
            unsigned entry = m_index++;
            JSValue key = table->keyAt(entry);
            if (!key)
                continue;
            if (m_kind == Kind::DictKeys || m_kind == Kind::Set)
                return key;
            if (m_kind == Kind::DictValues)
                return table->valueAt(entry);
            return PyTuple::create(globalObject, { key, table->valueAt(entry) });
        }
        finish();
        return { };
    }
    case Kind::DictReverseKeys: {
        auto* table = uncheckedDowncast<PyHashTable>(m_a.get().asCell());
        if (table->size() != static_cast<uint64_t>(m_stop)) {
            m_stop = -1;
            return Python::raise(globalObject, scope, BuiltinType::RuntimeError, "dictionary changed size during iteration"_s);
        }
        while (m_index > 0) {
            JSValue key = table->keyAt(--m_index);
            if (key)
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
        RETURN_IF_EXCEPTION(scope, { });
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
        JSValue index = Python::intFromInt64(globalObject, m_index++);
        return PyTuple::create(globalObject, { index, value });
    }
    case Kind::Zip: {
        auto* iterators = uncheckedDowncast<PyTuple>(m_a.get().asCell());
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
                return Python::raiseValueError(globalObject, scope, makeString("zip() argument "_s, i + 1, " is shorter than argument"_s, i == 1 ? " 1"_s : "s 1-"_s, i == 1 ? String() : String::number(i)));
            for (unsigned j = 1; j < count; ++j) {
                JSValue other = Python::iteratorNext(globalObject, iterators->at(j));
                RETURN_IF_EXCEPTION(scope, { });
                if (other)
                    return Python::raiseValueError(globalObject, scope, makeString("zip() argument "_s, j + 1, " is longer than argument"_s, j == 1 ? " 1"_s : "s 1-"_s, j == 1 ? String() : String::number(j)));
            }
            return { };
        }
        return PyTuple::createFromArguments(globalObject, values);
    }
    case Kind::Map: {
        auto* iterators = uncheckedDowncast<PyTuple>(m_b.get().asCell());
        MarkedArgumentBuffer values;
        for (unsigned i = 0; i < iterators->length(); ++i) {
            JSValue value = Python::iteratorNext(globalObject, iterators->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (!value)
                return { };
            values.append(value);
        }
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
    case Kind::Bytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC
