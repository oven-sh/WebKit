/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "CApiHooks.h"
#include "JSCInlines.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyPosix.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

#if !OS(WINDOWS)
#include <dlfcn.h>
#endif
#if OS(LINUX)
#include <link.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// ---- The cells ----

PY_DEFINE_CELL(PyCTypeInfo);
PY_DEFINE_CELL(PyCData);
PY_DEFINE_CELL(PyCFuncPtr);
PY_DEFINE_CELL(PyCField);
PY_DEFINE_CELL(PyCArg);
PY_DEFINE_CELL(PyCThunk);
PY_DEFINE_CELL(State);

PyCTypeInfo::PyCTypeInfo(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCTypeInfo* PyCTypeInfo::create(VM& vm, State& state)
{
    auto* info = new (NotNull, allocateCell<PyCTypeInfo>(vm)) PyCTypeInfo(vm, state.infoStructure.get(), state.infoType.get());
    info->finishCreation(vm);
    info->state.set(vm, info, &state);
    return info;
}

void PyCTypeInfo::copyFrom(VM& vm, const PyCTypeInfo& other)
{
    kind = other.kind;
    accessor = other.accessor;
    foreignKind = other.foreignKind;
    letter = other.letter;
    flags = other.flags;
    size = other.size;
    alignment = other.alignment;
    length = other.length;
    proto.setWithoutWriteBarrier(other.proto.get());
    argTypes.setWithoutWriteBarrier(other.argTypes.get());
    converters.setWithoutWriteBarrier(other.converters.get());
    resultType.setWithoutWriteBarrier(other.resultType.get());
    checker.setWithoutWriteBarrier(other.checker.get());
    format.setMayBeNull(vm, this, other.format.get());
    shape.setMayBeNull(vm, this, other.shape.get());
    fields.setMayBeNull(vm, this, other.fields.get());
    vm.writeBarrier(this);
}

template<typename Visitor>
void PyCTypeInfo::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCTypeInfo>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->proto);
    visitor.append(thisObject->argTypes);
    visitor.append(thisObject->converters);
    visitor.append(thisObject->resultType);
    visitor.append(thisObject->checker);
    visitor.append(thisObject->pointerType);
    visitor.append(thisObject->format);
    visitor.append(thisObject->shape);
    visitor.append(thisObject->fields);
    visitor.append(thisObject->state);
}

DEFINE_VISIT_CHILDREN(PyCTypeInfo);

PyCData::PyCData(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCData* PyCData::create(VM& vm, Structure* structure, PyType* type)
{
    auto* data = new (NotNull, allocateCell<PyCData>(vm)) PyCData(vm, structure, type);
    data->finishCreation(vm);
    return data;
}

template<typename Visitor>
void PyCData::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCData>(cell);
    Base::visitChildren(thisObject, visitor);
    // The order is that in which an instance lets go of things when it dies (PyFinalizationOrder.cpp). The memory first, and with it what is written down beside it: so what _objects holds dies when _objects
    // lets go of it, in the order of that dict, as in CPython, which has nothing else.
    visitor.append(thisObject->m_owner);
    visitor.append(thisObject->m_past);
    visitor.append(thisObject->base);
    visitor.append(thisObject->objects);
}

DEFINE_VISIT_CHILDREN(PyCData);

PyCFuncPtr::PyCFuncPtr(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCFuncPtr* PyCFuncPtr::create(VM& vm, Structure* structure, PyType* type)
{
    auto* function = new (NotNull, allocateCell<PyCFuncPtr>(vm)) PyCFuncPtr(vm, structure, type);
    function->finishCreation(vm);
    return function;
}

template<typename Visitor>
void PyCFuncPtr::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCFuncPtr>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->callable);
    visitor.append(thisObject->thunk);
    visitor.append(thisObject->converters);
    visitor.append(thisObject->argTypes);
    visitor.append(thisObject->resultType);
    visitor.append(thisObject->checker);
    visitor.append(thisObject->errorCheck);
    visitor.append(thisObject->paramFlags);
}

DEFINE_VISIT_CHILDREN(PyCFuncPtr);

PyCField::PyCField(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCField* PyCField::create(VM& vm, Structure* structure, PyType* type)
{
    auto* field = new (NotNull, allocateCell<PyCField>(vm)) PyCField(vm, structure, type);
    field->finishCreation(vm);
    return field;
}

template<typename Visitor>
void PyCField::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCField>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->name);
    visitor.append(thisObject->proto);
}

DEFINE_VISIT_CHILDREN(PyCField);

PyCArg::PyCArg(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCArg* PyCArg::create(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    State& state = stateOf(globalObject);
    auto* argument = new (NotNull, allocateCell<PyCArg>(vm)) PyCArg(vm, state.argStructure.get(), state.argType.get());
    argument->finishCreation(vm);
    return argument;
}

template<typename Visitor>
void PyCArg::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCArg>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->object);
    visitor.append(thisObject->keep);
}

DEFINE_VISIT_CHILDREN(PyCArg);

PyCThunk::PyCThunk(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCThunk* PyCThunk::create(VM& vm, Structure* structure, PyType* type)
{
    auto* thunk = new (NotNull, allocateCell<PyCThunk>(vm)) PyCThunk(vm, structure, type);
    thunk->finishCreation(vm);
    return thunk;
}

template<typename Visitor>
void PyCThunk::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCThunk>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->callable);
    visitor.append(thisObject->argTypes);
    visitor.append(thisObject->resultType);
}

DEFINE_VISIT_CHILDREN(PyCThunk);

State::State(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

State* State::create(VM& vm, Structure* structure, PyType* type)
{
    auto* state = new (NotNull, allocateCell<State>(vm)) State(vm, structure, type);
    state->finishCreation(vm);
    return state;
}

template<typename Visitor>
void State::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<State>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->metaType);
    visitor.append(thisObject->simpleMeta);
    visitor.append(thisObject->structMeta);
    visitor.append(thisObject->unionMeta);
    visitor.append(thisObject->arrayMeta);
    visitor.append(thisObject->pointerMeta);
    visitor.append(thisObject->functionMeta);
    visitor.append(thisObject->dataType);
    visitor.append(thisObject->simpleType);
    visitor.append(thisObject->structType);
    visitor.append(thisObject->unionType);
    visitor.append(thisObject->arrayType);
    visitor.append(thisObject->pointerType);
    visitor.append(thisObject->functionType);
    visitor.append(thisObject->fieldType);
    visitor.append(thisObject->argType);
    visitor.append(thisObject->infoType);
    visitor.append(thisObject->thunkType);
    visitor.append(thisObject->argumentError);
    visitor.append(thisObject->dataStructure);
    visitor.append(thisObject->functionStructure);
    visitor.append(thisObject->fieldStructure);
    visitor.append(thisObject->argStructure);
    visitor.append(thisObject->infoStructure);
    visitor.append(thisObject->thunkStructure);
    visitor.append(thisObject->storageStructure);
    visitor.append(thisObject->tableStructure);
    visitor.append(thisObject->records);
    visitor.append(thisObject->arrayTypes);
    visitor.append(thisObject->heldObjects);
    visitor.append(thisObject->unpickle);
#define VISIT_NAME(id, text) visitor.append(thisObject->id##Name);
    FOR_EACH_CTYPES_NAME(VISIT_NAME)
#undef VISIT_NAME
}

DEFINE_VISIT_CHILDREN(State);

// ---- The state ----

// Never seen from Python.
static constexpr TypeDef stateTypeDef { "_ctypes._state"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };
static constexpr TypeDef infoTypeDef { "_ctypes._info"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

static State* createState(JSGlobalObject* globalObject, PyType* holder)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    State* state = State::create(vm, State::createStructure(vm, globalObject), holder);
    setHiddenField(globalObject, holder, 0, state);

    auto addType = [&](WriteBarrier<PyType>& field, const TypeDef& def, PyType* base, PyType* meta = nullptr) {
        PyType* type = rt.typeFor(def, base);
        if (meta)
            type->setType(vm, meta);
        field.set(vm, state, type);
    };
    addType(state->metaType, metaTypeDef, rt.typeType());
    addType(state->simpleMeta, simpleMetaDef, state->metaType.get());
    addType(state->structMeta, structMetaDef, state->metaType.get());
    addType(state->unionMeta, unionMetaDef, state->metaType.get());
    addType(state->arrayMeta, arrayMetaDef, state->metaType.get());
    addType(state->pointerMeta, pointerMetaDef, state->metaType.get());
    addType(state->functionMeta, functionMetaDef, state->metaType.get());

    addType(state->dataType, dataTypeDef, nullptr);
    addType(state->simpleType, simpleTypeDef, state->dataType.get(), state->simpleMeta.get());
    addType(state->structType, structTypeDef, state->dataType.get(), state->structMeta.get());
    addType(state->unionType, unionTypeDef, state->dataType.get(), state->unionMeta.get());
    addType(state->arrayType, arrayTypeDef, state->dataType.get(), state->arrayMeta.get());
    addType(state->pointerType, pointerTypeDef, state->dataType.get(), state->pointerMeta.get());
    addType(state->functionType, functionTypeDef, state->dataType.get(), state->functionMeta.get());
    addType(state->fieldType, fieldTypeDef, nullptr);
    addType(state->argType, argTypeDef, nullptr);
    addType(state->thunkType, thunkTypeDef, nullptr);
    addType(state->infoType, infoTypeDef, nullptr);

    state->dataStructure.set(vm, state, PyCData::createStructure(vm, globalObject));
    state->functionStructure.set(vm, state, PyCFuncPtr::createStructure(vm, globalObject));
    state->fieldStructure.set(vm, state, PyCField::createStructure(vm, globalObject));
    state->argStructure.set(vm, state, PyCArg::createStructure(vm, globalObject));
    state->infoStructure.set(vm, state, PyCTypeInfo::createStructure(vm, globalObject));
    state->thunkStructure.set(vm, state, PyCThunk::createStructure(vm, globalObject));
    state->storageStructure.set(vm, state, PyCStorage::createStructure(vm, globalObject));
    state->tableStructure.set(vm, state, PyCTable::createStructure(vm, globalObject));

    state->arrayTypes.set(vm, state, PyDict::create(globalObject));
    state->heldObjects.set(vm, state, PyDict::create(globalObject));

#define INTERN_NAME(id, text) state->id##Name.set(vm, state, internString(vm, text ""_s));
    FOR_EACH_CTYPES_NAME(INTERN_NAME)
#undef INTERN_NAME
    return state;
}

State& stateOf(JSGlobalObject* globalObject)
{
    PyType* holder = runtime(globalObject).typeFor(stateTypeDef);
    if (JSValue state = hiddenField(globalObject, holder, 0)) [[likely]]
        return *State::from(state);
    return *createState(globalObject, holder);
}

PyCTypeInfo* infoOf(JSGlobalObject* globalObject, JSValue type)
{
    if (!PyType::is(type))
        return nullptr;
    PyCTypeInfo* info = PyCTypeInfo::tryFrom(hiddenField(globalObject, type, 0));
    return info && info->kind != TypeKind::None ? info : nullptr;
}

PyCTypeInfo* infoOfInstance(JSGlobalObject* globalObject, JSValue object)
{
    PyCData* data = PyCData::tryFrom(object);
    return data ? infoOf(globalObject, data->type()) : nullptr;
}

PyCTypeInfo* createInfo(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The hidden field is theirs in the classes of these metaclasses. Other types have other things there.
    if (!type->type()->isSubtypeOf(stateOf(globalObject).metaType.get())) {
        raiseSystemError(globalObject, scope, '\'', type->nameInMessages(), "' is not a ctypes class."_s);
        return nullptr;
    }
    if (PyCTypeInfo::is(hiddenField(globalObject, type, 0))) {
        raiseSystemError(globalObject, scope, "StgInfo of '"_s, type->nameInMessages(), "' is already initialized."_s);
        return nullptr;
    }
    PyCTypeInfo* info = PyCTypeInfo::create(vm, stateOf(globalObject));
    setHiddenField(globalObject, type, 0, info);
    return info;
}

// ---- The functions of the module ----

static JSValue moduleSizeOf(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyCTypeInfo* info = infoOf(globalObject, object))
        return makeInt(globalObject, info->size);
    if (PyCData* data = PyCData::tryFrom(object))
        return makeInt(globalObject, data->size);
    return raiseTypeError(globalObject, scope, "this type has no size"_s);
}

static PyCTypeInfo* infoOfAny(JSGlobalObject* globalObject, JSValue object)
{
    return PyType::is(object) ? infoOf(globalObject, object) : infoOfInstance(globalObject, object);
}

static JSValue moduleAlignment(JSGlobalObject* globalObject, JSValue, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyCTypeInfo* info = infoOfAny(globalObject, object))
        return makeInt(globalObject, info->alignment);
    return raiseTypeError(globalObject, scope, "no alignment info"_s);
}

static JSValue moduleByRef(JSGlobalObject* globalObject, JSValue object, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyCData::is(object))
        return raiseTypeError(globalObject, scope, "byref() argument 1 must be _ctypes._CData, not "_s, typeNameOfArgument(globalObject, object));
    int64_t offset = 0;
    if (offsetValue && failed(scope, ssizeArgument(globalObject, offsetValue, offset)))
        return { };
    RELEASE_AND_RETURN(scope, makeReference(globalObject, object, offset));
}

static JSValue moduleAddressOf(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::tryFrom(object);
    if (!data)
        return raiseTypeError(globalObject, scope, "addressof() argument must be _ctypes._CData, not "_s, typeNameOfArgument(globalObject, object));
    audit(globalObject, "ctypes.addressof"_s, object);
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t* memory = data->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return intFromPointer(globalObject, memory);
}

static JSValue moduleResize(JSGlobalObject* globalObject, JSValue object, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::tryFrom(object);
    if (!data)
        return raiseTypeError(globalObject, scope, "resize() argument 1 must be _ctypes._CData, not "_s, typeNameOfArgument(globalObject, object));
    int64_t size;
    if (failed(scope, ssizeArgument(globalObject, sizeValue, size)))
        return { };
    PyCTypeInfo* info = infoOfInstance(globalObject, object);
    if (!info)
        return raiseTypeError(globalObject, scope, "expected ctypes instance"_s);
    if (size < info->size)
        return raiseValueError(globalObject, scope, "minimum size is "_s, info->size);
    if (!data->ownsMemory)
        return raiseValueError(globalObject, scope, "Memory cannot be resized because this object doesn't own it"_s);
    // Whatever was made from parts of the instance, or points into it, goes on using the memory it had.
    auto* oldStorage = uncheckedDowncast<PyByteStorage>(data->owner());
    int64_t oldSize = data->size;
    // All of it was zero to begin with, and only the program has written to it since.
    if (size <= static_cast<int64_t>(oldStorage->capacity())) {
        data->size = size;
        return none();
    }
    // What it had is kept. So that an instance that grows step by step does not come to hold the square of what it needs, it is given half as much again at least.
    size_t oldCapacity = oldStorage->capacity();
    if (failed(scope, allocateMemory(globalObject, data, info, size, static_cast<int64_t>(oldCapacity + oldCapacity / 2))))
        return { };
    memcpy(data->memory(globalObject), oldStorage->data(), static_cast<size_t>(oldSize));
    data->keepFormerStorage(vm, oldStorage);
    return none();
}

static JSValue moduleBufferInfo(JSGlobalObject* globalObject, JSValue, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfAny(globalObject, object);
    if (!info)
        return raiseTypeError(globalObject, scope, "not a ctypes type or object"_s);
    PyTuple* shape = info->shape ? info->shape.get() : PyTuple::empty(globalObject);
    return PyTuple::create(globalObject, { info->format ? JSValue(info->format.get()) : none(), jsNumber(shape->length()), shape });
}

static JSValue moduleUnpickle(JSGlobalObject* globalObject, JSValue type, JSValue stateValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* state = PyTuple::tryFrom(stateValue);
    if (!state)
        return raiseTypeError(globalObject, scope, "argument 2 must be tuple, not "_s, typeNameOfArgument(globalObject, stateValue));
    JSValue object = callMethod(globalObject, type, rt.name(Special::New), type);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue method = getAttr(globalObject, object, stateOf(globalObject).setStateName.get());
    RETURN_IF_EXCEPTION(scope, { });
    callWithTupleAndDict(globalObject, method, state, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    return object;
}

static JSValue moduleGetErrno(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    audit(globalObject, "ctypes.get_errno"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(privateErrno());
}

static JSValue moduleSetErrno(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    audit(globalObject, "ctypes.set_errno"_s, value);
    RETURN_IF_EXCEPTION(scope, { });
    int number = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(std::exchange(privateErrno(), number));
}

// An address that is not 0. False: an exception.
static bool addressArgument(JSGlobalObject* globalObject, JSValue value, void*& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uintptr_t address;
    if (failed(scope, pointerFromInt(globalObject, value, address)))
        return false;
    if (!address) {
        raiseSystemError(globalObject, scope, "a function returned NULL without setting an exception"_s);
        return false;
    }
    result = std::bit_cast<void*>(address);
    return true;
}

static JSValue moduleObjectFromPointer(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    void* address;
    if (failed(scope, addressArgument(globalObject, value, address)))
        return { };
    JSValue object = objectFromBits(globalObject, std::bit_cast<uintptr_t>(address));
    RETURN_IF_EXCEPTION(scope, { });
    audit(globalObject, "ctypes.PyObj_FromPtr"_s, object);
    RETURN_IF_EXCEPTION(scope, { });
    return object;
}

// There are no reference counts. What a count above what the program can see means is that the object stays alive.
bool holdObject(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!object.isCell())
        return true;
    PyDict* held = stateOf(globalObject).heldObjects.get();
    JSValue key = makeInt(globalObject, static_cast<uint64_t>(JSValue::encode(object)));
    JSValue entry = held->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    int64_t count = entry ? smallIntValue(PyTuple::from(entry)->at(1)) : 0;
    held->set(globalObject, key, PyTuple::create(globalObject, { object, makeInt(globalObject, count + 1) }));
    return !scope.exception();
}

// These are references as C owns them, and are counted with those: what is taken here may be given back by C. Only an interpreter that the C API is not for keeps count here.
static JSValue moduleIncRef(JSGlobalObject* globalObject, JSValue, JSValue object)
{
    if (!object.isCell())
        return object;
    if (CApi::tryEnsure(globalObject)) {
        CApi::giveReference(identityBits(object));
        return object;
    }
    return holdObject(globalObject, object) ? object : JSValue();
}

static JSValue moduleDecRef(JSGlobalObject* globalObject, JSValue, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!object.isCell())
        return object;
    if (Runtime& rt = runtime(globalObject); CApi::isFor(rt)) {
        if (CApi::isSetUpFor(rt))
            CApi::giveBackReference(identityBits(object));
        return object;
    }
    PyDict* held = stateOf(globalObject).heldObjects.get();
    JSValue key = makeInt(globalObject, static_cast<uint64_t>(JSValue::encode(object)));
    JSValue entry = held->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!entry)
        return object;
    int64_t count = smallIntValue(PyTuple::from(entry)->at(1));
    if (count <= 1)
        held->remove(globalObject, key);
    else
        held->set(globalObject, key, PyTuple::create(globalObject, { object, makeInt(globalObject, count - 1) }));
    RETURN_IF_EXCEPTION(scope, { });
    return object;
}

template<unsigned flags>
static JSValue moduleCallFunction(JSGlobalObject* globalObject, JSValue addressValue, JSValue argumentsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    void* address;
    if (failed(scope, addressArgument(globalObject, addressValue, address)))
        return { };
    auto* arguments = PyTuple::tryFrom(argumentsValue);
    if (!arguments)
        return raiseTypeError(globalObject, scope, "argument 2 must be tuple, not "_s, typeNameOfArgument(globalObject, argumentsValue));
    RELEASE_AND_RETURN(scope, callFunction(globalObject, address, arguments, flags, { }, { }, { }));
}

static JSValue moduleGetAttr(JSGlobalObject* globalObject, JSValue name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(name)) {
        StrView view(globalObject, strValue(name));
        RETURN_IF_EXCEPTION(scope, { });
        if (view.view() == "__version__"_s) {
            warn(globalObject, rt.DeprecationWarning(), "'__version__' is deprecated and slated for removal in Python 3.20"_s);
            RETURN_IF_EXCEPTION(scope, { });
            return makeStr(vm, "1.1.0"_s);
        }
    }
    String text = reprForMessage(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return raise(globalObject, scope, rt.AttributeError(), makeString("module '_ctypes' has no attribute "_s, text));
}

#if !OS(WINDOWS)

static JSValue raiseLoaderError(JSGlobalObject* globalObject, ASCIILiteral fallback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    const char* message = dlerror();
    return raise(globalObject, scope, runtime(globalObject).OSError(), message ? decodeFilesystem(byteCast<uint8_t>(unsafeSpan(message))) : String(fallback));
}

// What a library does as it is loaded, and as it goes, may call the C API, and raise. `who` is the repr() of the function that has run it. False: an exception.
static bool lookAtWhatLibraryRaised(JSGlobalObject* globalObject, bool hasSucceeded, ASCIILiteral who)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!CApi::isSetUpFor(runtime(globalObject)))
        return !scope.exception();
    if (scope.exception() || !hasSucceeded) [[unlikely]] {
        CApi::forgetWhatCRaised();
        return !scope.exception();
    }
    RELEASE_AND_RETURN(scope, CApi::throwWhatCRaisedBesideResult(globalObject, who));
}

static JSValue moduleDlopen(JSGlobalObject* globalObject, JSValue name, JSValue modeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int mode = RTLD_NOW | RTLD_LOCAL;
    if (modeValue) {
        mode = toCInt(globalObject, modeValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    mode |= RTLD_NOW;
    PosixBytes path;
    if (!isNone(name) && failed(scope, posixToFSBytes(globalObject, name, path)))
        return { };
    audit(globalObject, "ctypes.dlopen"_s, name);
    RETURN_IF_EXCEPTION(scope, { });
    CApi::tryEnsure(globalObject);
    void* handle;
    {
        CApi::CRunsFor running(runtime(globalObject));
        handle = dlopen(isNone(name) ? nullptr : path.characters(), mode);
    }
    if (failed(scope, lookAtWhatLibraryRaised(globalObject, handle, "<built-in function dlopen>"_s)))
        return { };
    if (!handle)
        RELEASE_AND_RETURN(scope, raiseLoaderError(globalObject, "dlopen() error"_s));
    return intFromPointer(globalObject, handle);
}

static JSValue moduleDlclose(JSGlobalObject* globalObject, JSValue handleValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    void* handle;
    if (failed(scope, addressArgument(globalObject, handleValue, handle)))
        return { };
    bool hasSucceeded;
    {
        CApi::CRunsFor running(runtime(globalObject));
        hasSucceeded = !dlclose(handle);
    }
    if (failed(scope, lookAtWhatLibraryRaised(globalObject, hasSucceeded, "<built-in function dlclose>"_s)))
        return { };
    if (!hasSucceeded)
        RELEASE_AND_RETURN(scope, raiseLoaderError(globalObject, "dlclose() error"_s));
    return none();
}

static JSValue moduleDlsym(JSGlobalObject* globalObject, JSValue handleValue, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    void* handle;
    if (failed(scope, addressArgument(globalObject, handleValue, handle)))
        return { };
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "dlsym() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, nameValue));
    Vector<uint8_t> name;
    if (failed(scope, encodeCString(globalObject, strValue(nameValue), name)))
        return { };
    audit(globalObject, "ctypes.dlsym/handle"_s, handleValue, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    dlerror();
    if (void* address = dlsym(handle, byteCast<char>(name.span()).data()))
        return intFromPointer(globalObject, address);
    if (const char* message = dlerror())
        return raise(globalObject, scope, runtime(globalObject).OSError(), decodeFilesystem(byteCast<uint8_t>(unsafeSpan(message))));
    return raise(globalObject, scope, runtime(globalObject).OSError(), makeString("symbol '"_s, stringOf(strValue(nameValue)), "' not found"_s));
}

#endif // !OS(WINDOWS)

#if OS(LINUX)

static JSValue moduleDllist(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The loader holds a lock while it goes through the list: nothing of the VM is touched until it is done.
    Vector<Vector<char>> names;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) {
        static_cast<Vector<Vector<char>>*>(data)->append(Vector<char>(unsafeSpan(info->dlpi_name)));
        return 0;
    }, &names);
    JSArray* result = newList(globalObject, names.size());
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& name : names) {
        listAppend(globalObject, result, makeStr(globalObject, decodeFilesystem(byteCast<uint8_t>(name.span()))));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

#endif

// A function that CPython declares as taking a tuple of arguments, which it picks apart itself: it has no signature to show, and
// words its complaints its own way. `name` is null where CPython's format does not name the function.
template<auto function, unsigned min, unsigned max>
static JSValue takingTuple(JSGlobalObject* globalObject, ASCIILiteral name, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shown = name.isNull() ? "function"_str : makeString(name, "()"_s);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, shown, " takes no keyword arguments"_s);
    if (args.size() < min || args.size() > max) {
        unsigned limit = args.size() < min ? min : max;
        return raiseTypeError(globalObject, scope, shown, " takes "_s, min == max ? "exactly"_s : args.size() < min ? "at least"_s : "at most"_s, ' ', limit, " argument"_s, limit == 1 ? ""_s : "s"_s, " ("_s, args.size(), " given)"_s);
    }
    if constexpr (max == 1)
        RELEASE_AND_RETURN(scope, function(globalObject, args.get(0)));
    else
        RELEASE_AND_RETURN(scope, function(globalObject, args.get(0), args.get(1)));
}

#define CTYPES_TUPLE_FUNCTION(id, function, name, min, max) \
    static JSValue id(JSGlobalObject* globalObject, const CallArgs& args) { return takingTuple<function, min, max>(globalObject, name, args); }
CTYPES_TUPLE_FUNCTION(tupleSetErrno, moduleSetErrno, { }, 1, 1)
CTYPES_TUPLE_FUNCTION(tupleUnpickle, moduleUnpickle, { }, 2, 2)
CTYPES_TUPLE_FUNCTION(tupleCallFunction, moduleCallFunction<0>, { }, 2, 2)
CTYPES_TUPLE_FUNCTION(tupleCallCdeclFunction, moduleCallFunction<FunctionCdecl>, { }, 2, 2)
CTYPES_TUPLE_FUNCTION(tupleObjectFromPointer, moduleObjectFromPointer, "PyObj_FromPtr"_s, 1, 1)
#if !OS(WINDOWS)
CTYPES_TUPLE_FUNCTION(tupleDlopen, moduleDlopen, "dlopen"_s, 1, 2)
CTYPES_TUPLE_FUNCTION(tupleDlclose, moduleDlclose, "dlclose"_s, 1, 1)
CTYPES_TUPLE_FUNCTION(tupleDlsym, moduleDlsym, "dlsym"_s, 2, 2)
#endif

static JSValue tupleGetAttr(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "__getattr__"_s, args)) || failed(scope, checkPositional(globalObject, "__getattr__"_s, args, 1, 1)))
        return { };
    RELEASE_AND_RETURN(scope, moduleGetAttr(globalObject, args[0]));
}

static constexpr NativeDef unpickleDef = PY_FUNCTION("_unpickle", tupleUnpickle, "(*args, **kwargs)");

static constexpr NativeDef moduleFunctions[] = {
    PY_FUNCTION("get_errno", moduleGetErrno, "($self, /)"),
    PY_FUNCTION("set_errno", tupleSetErrno, "(*args, **kwargs)"),
    PY_FUNCTION("buffer_info", moduleBufferInfo, "($self, object, /)"),
    PY_FUNCTION("resize", moduleResize, "(obj, size, /)"),
#if !OS(WINDOWS)
    PY_FUNCTION("dlopen", tupleDlopen, "(*args, **kwargs)"),
    PY_FUNCTION("dlclose", tupleDlclose, "(*args, **kwargs)"),
    PY_FUNCTION("dlsym", tupleDlsym, "(*args, **kwargs)"),
#endif
#if OS(LINUX)
    PY_FUNCTION("dllist", moduleDllist, "($self, /)"),
#endif
    PY_FUNCTION("alignment", moduleAlignment, "($self, object, /)"),
    PY_FUNCTION("sizeof", moduleSizeOf, "(obj, /)"),
    PY_FUNCTION("byref", moduleByRef, "(obj, offset=0, /)"),
    PY_FUNCTION("addressof", moduleAddressOf, "(obj, /)"),
    PY_FUNCTION("call_function", tupleCallFunction, "(*args, **kwargs)"),
    PY_FUNCTION("call_cdeclfunction", tupleCallCdeclFunction, "(*args, **kwargs)"),
    PY_FUNCTION("PyObj_FromPtr", tupleObjectFromPointer, "(*args, **kwargs)"),
    PY_FUNCTION("Py_INCREF", moduleIncRef, "($self, object, /)"),
    PY_FUNCTION("Py_DECREF", moduleDecRef, "($self, object, /)"),
    PY_FUNCTION("__getattr__", tupleGetAttr, "(*args, **kwargs)"),
};

} // namespace Ctypes

void initializeCtypesModule(JSGlobalObject* globalObject, PyModule* module)
{
    using namespace Ctypes;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);

    module->add(globalObject, "__doc__"_s, makeStr(vm, "Create and manipulate C compatible data types in Python."_s));
    module->addFunctions(globalObject, moduleFunctions);
    if (!state.unpickle)
        state.unpickle.set(vm, &state, PyNativeFunction::create(globalObject, unpickleDef, NativeKind::Function, { }, makeStr(vm, "_ctypes"_s)));
    module->add(globalObject, "_unpickle"_s, state.unpickle.get());

    if (!state.argumentError) {
        // class ArgumentError(Exception), of the module ctypes.
        PyDict* members = PyDict::create(globalObject);
        members->set(vm, rt.names.dModule(), makeStr(vm, "ctypes"_s));
        JSValue type = call(globalObject, rt.typeType(), makeStr(vm, "ArgumentError"_s), PyTuple::create(globalObject, { rt.Exception() }), members);
        RETURN_IF_EXCEPTION(scope, void());
        state.argumentError.set(vm, &state, PyType::from(type));
    }
    module->add(globalObject, "ArgumentError"_s, state.argumentError.get());

    module->add(globalObject, "_SimpleCData"_s, state.simpleType.get());
    module->add(globalObject, "Structure"_s, state.structType.get());
    module->add(globalObject, "Union"_s, state.unionType.get());
    module->add(globalObject, "Array"_s, state.arrayType.get());
    module->add(globalObject, "_Pointer"_s, state.pointerType.get());
    module->add(globalObject, "CFuncPtr"_s, state.functionType.get());
    module->add(globalObject, "CField"_s, state.fieldType.get());

    module->add(globalObject, "FUNCFLAG_CDECL"_s, jsNumber(FunctionCdecl));
    module->add(globalObject, "FUNCFLAG_USE_ERRNO"_s, jsNumber(FunctionUseErrno));
    module->add(globalObject, "FUNCFLAG_USE_LASTERROR"_s, jsNumber(FunctionUseLastError));
    module->add(globalObject, "FUNCFLAG_PYTHONAPI"_s, jsNumber(FunctionPythonAPI));
    module->add(globalObject, "CTYPES_MAX_ARGCOUNT"_s, jsNumber(1024));
    module->add(globalObject, "SIZEOF_TIME_T"_s, jsNumber(static_cast<int>(sizeof(time_t))));
#if !OS(WINDOWS)
    module->add(globalObject, "RTLD_LOCAL"_s, jsNumber(RTLD_LOCAL));
    module->add(globalObject, "RTLD_GLOBAL"_s, jsNumber(RTLD_GLOBAL));
#endif
    module->add(globalObject, "_memmove_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfMemmove())));
    module->add(globalObject, "_memset_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfMemset())));
    module->add(globalObject, "_string_at_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfStringAt())));
    module->add(globalObject, "_wstring_at_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfWStringAt())));
    module->add(globalObject, "_memoryview_at_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfMemoryViewAt())));
    module->add(globalObject, "_cast_addr"_s, makeInt(globalObject, static_cast<uint64_t>(addressOfCast())));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
