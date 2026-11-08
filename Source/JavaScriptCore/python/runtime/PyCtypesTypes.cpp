/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "JSCInlines.h"
#include "PyList.h"
#include "PyWeakRef.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

#if !OS(WINDOWS)
#include <dlfcn.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

std::span<const NativeGetSet> charArrayGetSets(); // PyCtypesData.cpp
std::span<const NativeGetSet> wcharArrayGetSets();
PyCData* dataInView(JSGlobalObject*, PyType*, PyMemoryView*, int64_t offset);

// ---- What every metaclass has ----

static JSValue raiseAbstractClass(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "abstract class"_s);
}

static JSValue metaRepeat(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 0)
        return raiseValueError(globalObject, scope, "Array length must be >= 0, not "_s, count);
    RELEASE_AND_RETURN(scope, arrayTypeFor(globalObject, self, count));
}

static JSValue metaSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue size = callSpecial(globalObject, rt.typeType()->lookup(Special::SizeOf), self);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t extra = 0;
    if (PyCTypeInfo* info = infoOf(globalObject, self)) {
        if (info->format)
            extra += info->format->length() + 1;
        if (info->fields)
            extra += (info->length + 1) * sizeof(void*);
        if (info->shape)
            extra += info->shape->length() * sizeof(int64_t);
    }
    RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Add, size, makeInt(globalObject, extra)));
}

static PyCTypeInfo* infoForPointerType(JSGlobalObject* globalObject, JSValue self)
{
    PyCTypeInfo* info = infoOf(globalObject, self);
    if (info)
        return info;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = reprForMessage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, nullptr);
    raiseTypeError(globalObject, scope, text, " must have storage info"_s);
    return nullptr;
}

static JSValue metaGetPointerType(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoForPointerType(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSValue pointerType = info->pointerType.get())
        return pointerType;
    String text = reprForMessage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString(text, " has no attribute '__pointer_type__'"_s));
}

static void metaSetPointerType(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoForPointerType(globalObject, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (value)
        info->pointerType.set(vm, info, value);
    else
        info->pointerType.clear();
}

static constexpr NativeDef metaMethods[] = {
    PY_SEQUENCE_SLOT("__mul__", metaRepeat, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", metaRepeat, "($self, value, /)"),
    PY_METHOD("__sizeof__", metaSizeOf, "($self, /)"),
};

static constexpr NativeGetSet metaGetSets[] = {
    { "__pointer_type__"_s, metaGetPointerType, metaSetPointerType },
};

const TypeDef metaTypeDef { "_ctypes.CType_Type"_s, { TypeFlag::Base, TypeFlag::Immutable }, metaMethods, metaGetSets, { } };

// ---- from_address, from_buffer, from_buffer_copy, in_dll ----

static JSValue typeFromAddress(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(value))
        return raiseTypeError(globalObject, scope, "integer expected"_s);
    uintptr_t address;
    if (failed(scope, pointerFromInt(globalObject, value, address)))
        return { };
    RELEASE_AND_RETURN(scope, dataAtAddress(globalObject, PyType::from(self), std::bit_cast<void*>(address)));
}

static bool offsetArgument(JSGlobalObject* globalObject, JSValue value, int64_t& offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    offset = 0;
    if (!value)
        return true;
    if (failed(scope, ssizeArgument(globalObject, value, offset)))
        return false;
    if (offset < 0) {
        raiseValueError(globalObject, scope, "offset cannot be negative"_s);
        return false;
    }
    return true;
}

static JSValue typeFromBuffer(JSGlobalObject* globalObject, JSValue self, JSValue object, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t offset;
    if (failed(scope, offsetArgument(globalObject, offsetValue, offset)))
        return { };
    PyCTypeInfo* info = infoOf(globalObject, self);
    if (!info)
        RELEASE_AND_RETURN(scope, raiseAbstractClass(globalObject));
    JSValue viewValue = makeMemoryView(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    auto* view = PyMemoryView::from(viewValue);
    view->isInternal = true; // Until it is in _objects. The audit hooks are code of the program.
    auto fail = [&](JSValue) -> JSValue {
        ExceptionSetAside suspended(globalObject);
        view->release(globalObject);
        return { };
    };
    if (view->isReadOnly)
        return fail(raiseTypeError(globalObject, scope, "underlying buffer is not writable"_s));
    if (!view->isCContiguous)
        return fail(raiseTypeError(globalObject, scope, "underlying buffer is not C contiguous"_s));
    if (failed(scope, checkRoomFor(globalObject, info, view->length, offset)))
        return fail({ });
    uint8_t* memory = view->data(globalObject);
    if (scope.exception())
        return fail({ });
    audit(globalObject, "ctypes.cdata/buffer"_s, intFromPointer(globalObject, memory), makeInt(globalObject, view->length), makeInt(globalObject, offset));
    if (scope.exception())
        return fail({ });
    PyCData* data = dataInView(globalObject, PyType::from(self), view, offset); // It asks again, behind its hook.
    if (scope.exception())
        return fail({ });
    view->isInternal = false;
    keepAlive(globalObject, data, -1, view, Kept::ForWhole);
    RETURN_IF_EXCEPTION(scope, { });
    // What is made of the instance from now on, pointers and the arguments of foreign functions, are addresses of the memory.
    view->pinMemoryOfJS(globalObject);
    return data;
}

static JSValue typeFromBufferCopy(JSGlobalObject* globalObject, JSValue self, JSValue object, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    bool ok = holder.get(object);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    int64_t offset;
    if (failed(scope, offsetArgument(globalObject, offsetValue, offset)))
        return { };
    PyCTypeInfo* info = infoOf(globalObject, self);
    if (!info)
        RELEASE_AND_RETURN(scope, raiseAbstractClass(globalObject));
    Buffer& buffer = holder.buffer;
    if (failed(scope, checkRoomFor(globalObject, info, buffer.length, offset)))
        return { };
    audit(globalObject, "ctypes.cdata/buffer"_s, intFromPointer(globalObject, buffer.data), makeInt(globalObject, buffer.length), makeInt(globalObject, offset));
    RETURN_IF_EXCEPTION(scope, { });
    PyCData* data = newData(globalObject, PyType::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    // An audit hook may have run anything, and so may the making of the instance: the class may have been given its fields, the memory may have moved.
    if (failed(scope, refreshBuffer(globalObject, buffer)))
        return { };
    if (data->size > buffer.length - offset)
        return raiseValueError(globalObject, scope, "Buffer size too small ("_s, buffer.length, " instead of at least "_s, data->size + offset, " bytes)"_s);
    auto memory = Place { data }.resolve(globalObject, data->size);
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(memory.data(), buffer.data + offset, memory.size());
    return data;
}

static JSValue typeInDll(JSGlobalObject* globalObject, JSValue self, JSValue dll, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "in_dll() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, nameValue));
    JSString* name = strValue(nameValue);
    Vector<uint8_t> utf8;
    if (failed(scope, encodeCString(globalObject, name, utf8)))
        return { };
    audit(globalObject, "ctypes.dlsym"_s, dll, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue handleValue = getAttr(globalObject, dll, state.handleName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInt(handleValue))
        return raiseTypeError(globalObject, scope, "the _handle attribute of the second argument must be an integer"_s);
    uintptr_t handle;
    pointerFromInt(globalObject, handleValue, handle);
    if (scope.exception()) {
        takePendingException(globalObject);
        return raiseValueError(globalObject, scope, "could not convert the _handle attribute to a pointer"_s);
    }
#if OS(WINDOWS)
    UNUSED_VARIABLE(handle);
    void* address = nullptr;
#else
    dlerror();
    void* address = dlsym(std::bit_cast<void*>(handle), byteCast<char>(utf8.span()).data());
    if (!address) {
        if (const char* message = dlerror())
            return raiseValueError(globalObject, scope, decodeFilesystem(byteCast<uint8_t>(unsafeSpan(message))));
    }
#endif
    if (!address)
        return raiseValueError(globalObject, scope, "symbol '"_s, stringOf(name), "' not found"_s);
    RELEASE_AND_RETURN(scope, dataAtAddress(globalObject, PyType::from(self), address));
}

// ---- from_param ----

JSValue dataTypeFromParam(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);
    bool isOfType = isInstance(globalObject, value, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType)
        return value;
    if (auto* argument = PyCArg::tryFrom(value)) {
        // A reference: to an instance of what this type points to?
        JSValue object = argument->object.get();
        PyCTypeInfo* info = infoOf(globalObject, self);
        if (info && object && info->proto) {
            bool matches = isInstance(globalObject, object, info->proto.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (matches)
                return value;
        }
        return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance instead of pointer to "_s, object ? typeName(globalObject, object) : "???"_str);
    }
    JSValue asParameter = getAttrIfPresent(globalObject, value, state.asParameterName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (asParameter) {
        AsParameterScope nesting;
        if (failed(scope, !nesting.isTooDeep(globalObject)))
            return { };
        RELEASE_AND_RETURN(scope, dataTypeFromParam(globalObject, self, asParameter));
    }
    return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance instead of "_s, typeName(globalObject, value));
}

#define CTYPES_COMMON_TYPE_METHODS \
    PY_METHOD("from_address", typeFromAddress, "($self, value, /)"), \
    PY_METHOD("from_buffer", typeFromBuffer, "($self, obj, offset=0, /)"), \
    PY_METHOD("from_buffer_copy", typeFromBufferCopy, "($self, buffer, offset=0, /)"), \
    PY_METHOD("in_dll", typeInDll, "($self, dll, name, /)")

// ---- Structure and Union ----

static bool makeFieldsOfAnonymous(JSGlobalObject* globalObject, State& state, PyType* type, PyCField* descriptor, int64_t index, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
        return false;
    }
    JSValue fields = getAttr(globalObject, descriptor->proto.get(), state.fieldsName.get());
    RETURN_IF_EXCEPTION(scope, false);
    if (!PyTuple::is(fields) && !isList(fields)) {
        fields = listFromIterable(globalObject, fields);
        if (scope.exception()) {
            if (pendingExceptionMatches(globalObject, runtime(globalObject).TypeError())) {
                takePendingException(globalObject);
                raiseTypeError(globalObject, scope, "_fields_ must be a sequence"_s);
            }
            return false;
        }
    }
    int64_t count = length(globalObject, fields);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue pair = getItem(globalObject, fields, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        auto* tuple = PyTuple::tryFrom(pair);
        if (!tuple || tuple->length() < 2 || tuple->length() > 3) {
            raiseTypeError(globalObject, scope, "function takes at least 2 arguments"_s);
            return false;
        }
        JSString* name = attributeName(globalObject, tuple->at(0));
        RETURN_IF_EXCEPTION(scope, false);
        JSValue inner = getAttr(globalObject, descriptor->proto.get(), name);
        RETURN_IF_EXCEPTION(scope, false);
        auto* field = PyCField::tryFrom(inner);
        if (!field || field->type() != state.fieldType.get()) {
            raiseTypeError(globalObject, scope, "unexpected type"_s);
            return false;
        }
        if (field->isAnonymous) {
            if (failed(scope, makeFieldsOfAnonymous(globalObject, state, type, field, index + field->index, offset + field->byteOffset)))
                return false;
            continue;
        }
        PyCField* copy = PyCField::create(vm, state.fieldStructure.get(), state.fieldType.get());
        copy->byteSize = field->byteSize;
        copy->byteOffset = field->byteOffset + offset;
        copy->bits = field->bits;
        copy->index = field->index + index;
        copy->proto.set(vm, copy, field->proto.get());
        copy->accessor = field->accessor;
        copy->name.set(vm, copy, field->name.get());
        setAttr(globalObject, type, name, copy);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static bool makeAnonymousFields(JSGlobalObject* globalObject, State& state, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue anonymous = getAttrIfPresent(globalObject, type, state.anonymousName.get());
    RETURN_IF_EXCEPTION(scope, false);
    if (!anonymous)
        return true;
    JSArray* names = listFromIterable(globalObject, anonymous);
    if (scope.exception()) {
        if (pendingExceptionMatches(globalObject, runtime(globalObject).TypeError())) {
            takePendingException(globalObject);
            raiseTypeError(globalObject, scope, "_anonymous_ must be a sequence"_s);
        }
        return false;
    }
    for (unsigned i = 0; i < listLength(names); ++i) {
        JSString* name = attributeName(globalObject, listGet(names, i));
        RETURN_IF_EXCEPTION(scope, false);
        JSValue descriptor = getAttr(globalObject, type, name);
        RETURN_IF_EXCEPTION(scope, false);
        auto* field = PyCField::tryFrom(descriptor);
        if (!field || field->type() != state.fieldType.get()) {
            raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString('\'', stringOf(name), "' is specified in _anonymous_ but not in _fields_"_s));
            return false;
        }
        field->isAnonymous = true;
        if (failed(scope, makeFieldsOfAnonymous(globalObject, state, type, field, field->index, field->byteOffset)))
            return false;
    }
    return true;
}

// _fields_ is being set: ctypes._layout works out where the fields are.
static bool updateStructInfo(JSGlobalObject* globalObject, PyType* type, JSValue fields, bool isStruct)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (!fields)
        return true;
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info) {
        raiseTypeError(globalObject, scope, "ctypes state is not initialized"_s);
        return false;
    }
    PyCTypeInfo* baseInfo = type->base() ? infoOf(globalObject, type->base()) : nullptr;
    if (info->isFinal) {
        raise(globalObject, scope, rt.AttributeError(), "_fields_ is final"_s);
        return false;
    }
    // A metaclass derived from this one and another has the methods of both, for classes that only one of them knows about.
    if (info->kind != TypeKind::Structure && info->kind != TypeKind::Union) {
        raiseTypeError(globalObject, scope, '\'', type->name(), "' is no structure and no union"_s);
        return false;
    }

    JSValue module = importModule(globalObject, "ctypes._layout"_s);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue getLayout = getAttr(globalObject, module, state.getLayoutName.get());
    RETURN_IF_EXCEPTION(scope, false);
    PyDict* keywords = PyDict::create(globalObject);
    keywords->set(vm, state.isStructName.get(), jsBoolean(isStruct));
    keywords->set(vm, state.baseName.get(), baseInfo ? JSValue(type->base()) : none());
    JSValue layout = callWithTupleAndDict(globalObject, getLayout, PyTuple::create(globalObject, { type, fields }), keywords);
    RETURN_IF_EXCEPTION(scope, false);

    auto nonNegative = [&](JSString* name, ASCIILiteral message, int64_t& result) {
        JSValue value = getAttr(globalObject, layout, name);
        RETURN_IF_EXCEPTION(scope, false);
        result = toSsize(globalObject, value, rt.OverflowError());
        RETURN_IF_EXCEPTION(scope, false);
        if (result < 0) {
            raiseValueError(globalObject, scope, message);
            return false;
        }
        return true;
    };
    int64_t totalAlignment;
    int64_t totalSize;
    if (!nonNegative(state.alignName.get(), "align must be a non-negative integer"_s, totalAlignment) || !nonNegative(state.sizeName.get(), "size must be a non-negative integer"_s, totalSize))
        return false;
    JSValue format = getAttr(globalObject, layout, state.formatSpecName.get());
    RETURN_IF_EXCEPTION(scope, false);
    if (!isStr(format)) {
        raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
        return false;
    }
    StrView formatView(globalObject, strValue(format));
    RETURN_IF_EXCEPTION(scope, false);
    info->format.set(vm, info, makeStr(vm, formatView.string()));

    JSValue layoutFields = getAttr(globalObject, layout, state.fieldsOfLayoutName.get());
    RETURN_IF_EXCEPTION(scope, false);
    PyTuple* newFields = tupleFromIterable(globalObject, layoutFields);
    RETURN_IF_EXCEPTION(scope, false);

    unsigned inherited = baseInfo && baseInfo->fields ? baseInfo->fields->length() : 0;
    PyTuple* allFields = PyTuple::create(globalObject, inherited + newFields->length());
    for (unsigned i = 0; i < inherited; ++i)
        allFields->initializeAt(vm, i, baseInfo->fields->at(i));
    for (unsigned i = 0; i < newFields->length(); ++i) {
        auto* field = PyCField::tryFrom(newFields->at(i));
        if (!field) {
            raiseTypeError(globalObject, scope, "fields must be of type CField, got "_s, typeNameForMessage(globalObject, typeOf(globalObject, newFields->at(i))));
            return false;
        }
        if (field->index != i) {
            String name = reprForMessage(globalObject, field->name.get());
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(globalObject, scope, "field "_s, name, " index mismatch (expected "_s, i, ", got "_s, field->index, ')');
            return false;
        }
        allFields->initializeAt(vm, inherited + i, field);
        PyCTypeInfo* fieldInfo = infoOf(globalObject, field->proto.get());
        ASSERT(fieldInfo);
        if (fieldInfo->flags & (IsPointer | HasPointer))
            info->flags |= HasPointer;
        if (fieldInfo->flags & (IsAddress | HasAddress))
            info->flags |= HasAddress;
        if (fieldInfo->kind == TypeKind::Union || (fieldInfo->flags & HasUnion))
            info->flags |= HasUnion;
        if (field->bits.size || (fieldInfo->flags & HasBitField))
            info->flags |= HasBitField;
        fieldInfo->isFinal = true;
        setAttr(globalObject, type, field->name.get(), field);
        RETURN_IF_EXCEPTION(scope, false);
    }
    // (CPython's flags go by the new fields alone.)
    if (baseInfo && baseInfo->flags & HasAddress)
        info->flags |= HasAddress;
    info->fields.set(vm, info, allFields);
    info->size = totalSize;
    info->alignment = totalAlignment;
    info->length = (baseInfo ? baseInfo->length : 0) + newFields->length();

    if (info->isFinal) {
        raise(globalObject, scope, rt.AttributeError(), "Structure or union cannot contain itself"_s);
        return false;
    }
    info->isFinal = true;
    RELEASE_AND_RETURN(scope, makeAnonymousFields(globalObject, state, type));
}

template<bool isStruct>
static JSValue structMetaInit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);
    if (type->dict()->get(state.abstractName.get()))
        return none();
    PyCTypeInfo* info = createInfo(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    info->kind = isStruct ? TypeKind::Structure : TypeKind::Union;
    info->foreignKind = ForeignKind::Aggregate;
    info->format.set(vm, info, makeStr(vm, "B"_s));
    if (JSValue fields = type->dict()->get(state.fieldsName.get())) {
        setAttr(globalObject, self, state.fieldsName.get(), fields);
        RETURN_IF_EXCEPTION(scope, { });
        return none();
    }
    PyCTypeInfo* baseInfo = type->base() ? infoOf(globalObject, type->base()) : nullptr;
    if (baseInfo) {
        info->copyFrom(vm, *baseInfo);
        baseInfo->isFinal = true;
    }
    return none();
}

template<bool isStruct>
static JSValue structMetaSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(key)) {
        StrView view(globalObject, strValue(key));
        RETURN_IF_EXCEPTION(scope, { });
        if (view.view() == "_fields_"_s && failed(scope, updateStructInfo(globalObject, PyType::from(self), value, isStruct)))
            return { };
    }
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, rt.typeType()->lookup(Special::SetAttr), self, key, value));
}

static JSValue structMetaDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    return callSpecial(globalObject, runtime(globalObject).typeType()->lookup(Special::DelAttr), self, key);
}

static constexpr NativeDef structMetaMethods[] = {
    PY_SLOT("__setattr__", structMetaSetAttr<true>, "($self, name, value, /)"),
    PY_SLOT("__delattr__", structMetaDelAttr, "($self, name, /)"),
    PY_SLOT("__init__", structMetaInit<true>, "($self, /, *args, **kwargs)"),
    PY_METHOD("from_param", dataTypeFromParam, "($self, value, /)"),
    CTYPES_COMMON_TYPE_METHODS,
};

static constexpr NativeDef unionMetaMethods[] = {
    PY_SLOT("__setattr__", structMetaSetAttr<false>, "($self, name, value, /)"),
    PY_SLOT("__delattr__", structMetaDelAttr, "($self, name, /)"),
    PY_SLOT("__init__", structMetaInit<false>, "($self, /, *args, **kwargs)"),
    PY_METHOD("from_param", dataTypeFromParam, "($self, value, /)"),
    CTYPES_COMMON_TYPE_METHODS,
};

const TypeDef structMetaDef { "_ctypes.PyCStructType"_s, { TypeFlag::Base, TypeFlag::Immutable }, structMetaMethods, { }, "metatype for the CData Objects"_s };
const TypeDef unionMetaDef { "_ctypes.UnionType"_s, { TypeFlag::Base, TypeFlag::Immutable }, unionMetaMethods, { }, "metatype for the Union Objects"_s };

// ---- Pointers ----

static bool setPointerProto(JSGlobalObject* globalObject, PyType* self, PyCTypeInfo* info, JSValue proto)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyType::is(proto)) {
        raiseTypeError(globalObject, scope, "_type_ must be a type"_s);
        return false;
    }
    PyCTypeInfo* target = infoForPointerType(globalObject, proto);
    RETURN_IF_EXCEPTION(scope, false);
    info->proto.set(vm, info, proto);
    if (!target->pointerType)
        target->pointerType.set(vm, target, self);
    StringBuilder format;
    format.append('&');
    if (PyTuple* shape = target->shape.get(); shape && shape->length()) {
        format.append('(');
        for (unsigned i = 0; i < shape->length(); ++i) {
            if (i)
                format.append(',');
            format.append(intToString(globalObject, shape->at(i)));
            RETURN_IF_EXCEPTION(scope, false);
        }
        format.append(')');
    }
    format.append(target->format ? stringOf(target->format.get()) : "B"_str);
    info->format.set(vm, info, makeStr(vm, format.toString()));
    return true;
}

static JSValue pointerMetaInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);
    if (args.size() < 3)
        return raiseIndexError(globalObject, scope, "tuple index out of range"_s);
    PyCTypeInfo* info = createInfo(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    info->kind = TypeKind::Pointer;
    info->foreignKind = ForeignKind::Pointer;
    info->size = sizeof(void*);
    info->alignment = alignof(void*);
    info->length = 1;
    info->flags |= IsPointer | IsAddress;
    if (JSValue proto = type->dict()->get(state.typeName.get())) {
        if (failed(scope, setPointerProto(globalObject, type, info, proto)))
            return { };
    }
    return none();
}

static JSValue pointerMetaSetType(JSGlobalObject* globalObject, JSValue self, JSValue proto)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOf(globalObject, self);
    // Where a pointer has the type it points to, a simple type has its code and an array the type of its items.
    if (!info || info->kind == TypeKind::Simple || info->kind == TypeKind::Array)
        RELEASE_AND_RETURN(scope, raiseAbstractClass(globalObject));
    if (failed(scope, setPointerProto(globalObject, PyType::from(self), info, proto)))
        return { };
    setAttr(globalObject, self, stateOf(globalObject).typeName.get(), proto);
    RETURN_IF_EXCEPTION(scope, { });
    return none();
}

static JSValue pointerMetaFromParam(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (isNone(value))
        return value;
    PyCTypeInfo* info = infoOf(globalObject, self);
    if (!info)
        RELEASE_AND_RETURN(scope, raiseAbstractClass(globalObject));
    if (!info->proto)
        return raiseTypeError(globalObject, scope, "cannot convert argument: POINTER _type_ type is not set"_s);
    // An instance of what it points to is passed by reference.
    bool isTarget = isInstance(globalObject, value, info->proto.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (isTarget && PyCData::is(value))
        RELEASE_AND_RETURN(scope, makeReference(globalObject, value, 0));
    if (isInstanceOfData(value, state.pointerType.get()) || isInstanceOfData(value, state.arrayType.get())) {
        PyCTypeInfo* valueInfo = infoOfInstance(globalObject, value);
        if (valueInfo && valueInfo->proto) {
            bool matches = isSubclass(globalObject, valueInfo->proto.get(), info->proto.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (matches)
                return value;
        }
    }
    RELEASE_AND_RETURN(scope, dataTypeFromParam(globalObject, self, value));
}

static constexpr NativeDef pointerMetaMethods[] = {
    PY_SLOT("__init__", pointerMetaInit, "($self, /, *args, **kwargs)"),
    CTYPES_COMMON_TYPE_METHODS,
    PY_METHOD("from_param", pointerMetaFromParam, "($self, value, /)"),
    PY_METHOD("set_type", pointerMetaSetType, "($self, type, /)"),
};

const TypeDef pointerMetaDef { "_ctypes.PyCPointerType"_s, { TypeFlag::Base, TypeFlag::Immutable }, pointerMetaMethods, { }, "metatype for the Pointer Objects"_s };

// ---- Arrays ----

static JSValue arrayMetaInit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);

    JSValue lengthValue = getAttrIfPresent(globalObject, self, state.lengthName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!lengthValue)
        return raise(globalObject, scope, rt.AttributeError(), "class must define a '_length_' attribute"_s);
    if (!isInt(lengthValue))
        return raiseTypeError(globalObject, scope, "The '_length_' attribute must be an integer"_s);
    JSValue exactLength = intValue(lengthValue);
    if (intSign(exactLength) < 0)
        return raiseValueError(globalObject, scope, "The '_length_' attribute must not be negative"_s);
    auto length = tryInt64(exactLength);
    if (!length)
        return raiseOverflowError(globalObject, scope, "The '_length_' attribute is too large"_s);
    JSValue itemType = getAttrIfPresent(globalObject, self, state.typeName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!itemType)
        return raise(globalObject, scope, rt.AttributeError(), "class must define a '_type_' attribute"_s);

    PyCTypeInfo* info = createInfo(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    PyCTypeInfo* itemInfo = infoOf(globalObject, itemType);
    if (!itemInfo)
        return raiseTypeError(globalObject, scope, "_type_ must have storage info"_s);
    info->foreignKind = ForeignKind::Pointer;
    info->format.setMayBeNull(vm, info, itemInfo->format.get());
    unsigned itemDimensions = itemInfo->shape ? itemInfo->shape->length() : 0;
    PyTuple* shape = PyTuple::create(globalObject, itemDimensions + 1);
    shape->initializeAt(vm, 0, exactLength);
    for (unsigned i = 0; i < itemDimensions; ++i)
        shape->initializeAt(vm, i + 1, itemInfo->shape->at(i));
    info->shape.set(vm, info, shape);
    if (itemInfo->size && *length > std::numeric_limits<int64_t>::max() / itemInfo->size)
        return raiseOverflowError(globalObject, scope, "array too large"_s);
    if (itemInfo->flags & (IsPointer | HasPointer))
        info->flags |= HasPointer;
    if (itemInfo->flags & (IsAddress | HasAddress))
        info->flags |= HasAddress;
    info->size = itemInfo->size * *length;
    info->alignment = itemInfo->alignment;
    info->length = *length;
    info->proto.set(vm, info, itemType);
    info->kind = TypeKind::Array;
    // Arrays of characters are strings as well. (The metaclass can be given to a class that is not derived from Array, nor from any class of ctypes: its instances
    // are not what these take them for.)
    if (!hasDataForInstances(state, type))
        return none();
    if (itemInfo->accessor == Accessor { Code::Char })
        type->addGetSets(globalObject, charArrayGetSets());
    else if (itemInfo->accessor == Accessor { Code::WChar })
        type->addGetSets(globalObject, wcharArrayGetSets());
    return none();
}

static constexpr NativeDef arrayMetaMethods[] = {
    PY_SLOT("__init__", arrayMetaInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("from_param", dataTypeFromParam, "($self, value, /)"),
    CTYPES_COMMON_TYPE_METHODS,
};

const TypeDef arrayMetaDef { "_ctypes.PyCArrayType"_s, { TypeFlag::Base, TypeFlag::Immutable }, arrayMetaMethods, { }, "metatype for the Array Objects"_s };

// Null: it is no weak reference, or what it referred to is gone.
static PyType* typeReferredToBy(JSValue cached)
{
    return PyWeakRef::is(cached) ? PyType::tryFrom(weakRefGet(cached)) : nullptr;
}

JSValue arrayTypeFor(JSGlobalObject* globalObject, JSValue itemType, int64_t length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyDict* cache = state.arrayTypes.get();
    JSValue lengthValue = makeInt(globalObject, length);
    JSValue key = PyTuple::create(globalObject, { itemType, lengthValue });
    JSValue cached = cache->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (PyType* type = cached ? typeReferredToBy(cached) : nullptr) {
        if (PyCTypeInfo* info = infoOf(globalObject, type); info && info->kind == TypeKind::Array && info->length == length && info->proto.get() == itemType)
            return type;
    }
    PyType* item = PyType::tryFrom(itemType);
    if (!item)
        return raiseTypeError(globalObject, scope, "Expected a type object"_s);
    PyDict* members = PyDict::create(globalObject);
    members->set(vm, state.lengthName.get(), lengthValue);
    members->set(vm, state.typeName.get(), itemType);
    JSValue name = makeStr(globalObject, makeString(item->nameInMessages().left(200), "_Array_"_s, length));
    JSValue result = call(globalObject, state.arrayMeta.get(), name, PyTuple::create(globalObject, { state.arrayType.get() }), members);
    RETURN_IF_EXCEPTION(scope, { });

    // The types are not kept alive by this. The entries of those that are gone are removed now and then.
    if (cache->size() >= state.arrayTypesSweepAt) {
        MarkedArgumentBuffer dead;
        JSValue entryKey;
        JSValue entryValue;
        for (unsigned position = 0; cache->next(position, entryKey, entryValue);) {
            if (!typeReferredToBy(entryValue))
                dead.append(entryKey);
        }
        for (unsigned i = 0; i < dead.size(); ++i) {
            cache->remove(globalObject, dead.at(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        state.arrayTypesSweepAt = std::max(64u, cache->size() * 2);
    }
    PyWeakRef* weak = makeWeakRef(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    weak->setIsInternal();
    cache->set(globalObject, key, weak);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// ---- Simple types ----

static PyCArg* packedArgument(JSGlobalObject* globalObject, Code code, char tag, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Packed packed;
    bool ok = pack(globalObject, { code }, value, sizeof(void*), packed);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(ok, ok);
    PyCArg* argument = PyCArg::create(globalObject);
    argument->foreignKind = ForeignKind::Pointer;
    argument->tag = tag;
    memcpy(argument->value, packed.bytes, sizeof(argument->value));
    argument->object.set(vm, argument, packed.keep);
    if (packed.owner)
        argument->keep.set(vm, argument, packed.owner);
    return argument;
}

static PyCArg* pointerArgument(JSGlobalObject* globalObject, char tag, PyCData* holder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto memory = Place { holder }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCArg* argument = PyCArg::create(globalObject);
    argument->foreignKind = ForeignKind::Pointer;
    argument->tag = tag;
    memcpy(argument->value, memory.data(), sizeof(void*));
    argument->object.set(vm, argument, holder);
    keepWhatIsPointedTo(globalObject, argument, holder);
    return argument;
}

// c_char_p and c_wchar_p.
template<Code pointerCode, Code characterCode, char tag>
static JSValue stringPointerFromParam(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (isNone(value))
        return value;
    if (pointerCode == Code::CharPointer ? PyBytes::is(value) : isStr(value))
        RELEASE_AND_RETURN(scope, packedArgument(globalObject, pointerCode, tag, value));
    bool isOfType = isInstance(globalObject, value, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType)
        return value;
    auto isCharacterType = [&](PyCTypeInfo* info) { return info && info->accessor == Accessor { characterCode }; };
    if (isInstanceOfData(value, state.arrayType.get()) || isInstanceOfData(value, state.pointerType.get())) {
        PyCTypeInfo* info = infoOfInstance(globalObject, value);
        if (info && info->proto && isCharacterType(infoOf(globalObject, info->proto.get())))
            return value;
    }
    if (auto* argument = PyCArg::tryFrom(value)) {
        if (isCharacterType(infoOfInstance(globalObject, argument->object.get())))
            return value;
    }
    JSValue asParameter = getAttrIfPresent(globalObject, value, state.asParameterName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (asParameter) {
        AsParameterScope nesting;
        if (failed(scope, !nesting.isTooDeep(globalObject)))
            return { };
        RELEASE_AND_RETURN(scope, (stringPointerFromParam<pointerCode, characterCode, tag>(globalObject, self, asParameter)));
    }
    return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, value), "' object cannot be interpreted as ctypes."_s, pointerCode == Code::CharPointer ? "c_char_p"_s : "c_wchar_p"_s);
}

static JSValue voidPointerFromParam(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (isNone(value))
        return value;
    if (isInt(value))
        RELEASE_AND_RETURN(scope, packedArgument(globalObject, Code::VoidPointer, 'P', value));
    if (PyBytes::is(value))
        RELEASE_AND_RETURN(scope, packedArgument(globalObject, Code::CharPointer, 'z', value));
    if (isStr(value))
        RELEASE_AND_RETURN(scope, packedArgument(globalObject, Code::WCharPointer, 'Z', value));
    bool isOfType = isInstance(globalObject, value, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType)
        return value;
    if (isInstanceOfData(value, state.arrayType.get()) || isInstanceOfData(value, state.pointerType.get()))
        return value;
    if (auto* argument = PyCArg::tryFrom(value); argument && argument->tag == 'P' && !argument->tag2)
        return value;
    if (auto* function = PyCFuncPtr::tryFrom(value))
        RELEASE_AND_RETURN(scope, pointerArgument(globalObject, 'P', function));
    if (auto* data = PyCData::tryFrom(value)) {
        PyCTypeInfo* info = infoOfInstance(globalObject, value);
        if (info && (info->accessor.code == Code::CharPointer || info->accessor.code == Code::WCharPointer))
            RELEASE_AND_RETURN(scope, pointerArgument(globalObject, 'Z', data));
    }
    JSValue asParameter = getAttrIfPresent(globalObject, value, state.asParameterName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (asParameter) {
        AsParameterScope nesting;
        if (failed(scope, !nesting.isTooDeep(globalObject)))
            return { };
        RELEASE_AND_RETURN(scope, voidPointerFromParam(globalObject, self, asParameter));
    }
    return raiseTypeError(globalObject, scope, '\'', typeName(globalObject, value), "' object cannot be interpreted as ctypes.c_void_p"_s);
}

static constexpr NativeDef charPointerFromParamDef = PY_CLASSMETHOD("from_param", (stringPointerFromParam<Code::CharPointer, Code::Char, 'z'>), "($type, value, /)");
static constexpr NativeDef wcharPointerFromParamDef = PY_CLASSMETHOD("from_param", (stringPointerFromParam<Code::WCharPointer, Code::WChar, 'Z'>), "($type, value, /)");
static constexpr NativeDef voidPointerFromParamDef = PY_CLASSMETHOD("from_param", voidPointerFromParam, "($type, value, /)");

static void fillSimpleInfo(VM& vm, PyCTypeInfo* info, const CodeDescription& description, JSValue proto, bool isSwapped)
{
    info->kind = TypeKind::Simple;
    info->foreignKind = description.foreignKind;
    info->letter = description.text[0];
    // The swapped c_bool is, in CPython, an integer of that size.
    info->accessor = { isSwapped && description.code == Code::Bool ? Code::UnsignedInt : description.code, isSwapped && description.size > 1 };
    info->alignment = description.alignment;
    info->length = 0;
    info->size = description.size;
    info->proto.set(vm, info, proto);
    info->format.set(vm, info, makeStr(vm, makeString(isSwapped ? '>' : '<', description.format)));
}

static JSValue simpleMetaInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);

    callSpecial(globalObject, rt.typeType()->lookup(Special::Init), self, args);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue proto = getAttrIfPresent(globalObject, self, state.typeName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!proto)
        return raise(globalObject, scope, rt.AttributeError(), "class must define a '_type_' attribute"_s);
    if (!isStr(proto))
        return raiseTypeError(globalObject, scope, "class must define a '_type_' string attribute"_s);
    StrView text(globalObject, strValue(proto));
    RETURN_IF_EXCEPTION(scope, { });
    const CodeDescription* description = describeCode(text.view());
    if (!description) {
        return raise(globalObject, scope, rt.AttributeError(), "class must define a '_type_' attribute which must be\n"
            "one of these characters: 'cbBhHiIlLdfuzZqQPOv?g',\n"
            "or one of these strings: 'Zf', 'Zd', 'Zg'."_s);
    }
    PyCTypeInfo* info = createInfo(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    fillSimpleInfo(vm, info, *description, proto, false);
    if (isAddress(description->code))
        info->flags |= IsAddress;

    // The pointer types themselves, not what is derived from them, take more kinds of arguments.
    if (type->base() == state.simpleType.get()) {
        const NativeDef* fromParam = nullptr;
        switch (description->code) {
        case Code::CharPointer:
            fromParam = &charPointerFromParamDef;
            break;
        case Code::WCharPointer:
            fromParam = &wcharPointerFromParamDef;
            break;
        case Code::VoidPointer:
            fromParam = &voidPointerFromParamDef;
            break;
        default:
            break;
        }
        if (fromParam || description->code == Code::Object || description->code == Code::CharArray)
            info->flags |= IsPointer;
        if (fromParam) {
            type->dict()->set(vm, state.fromParamName.get(), PyNativeFunction::create(globalObject, *fromParam, NativeKind::ClassMethod, { }, type));
            type->didChange(vm);
        }
    }

    if (type->type() == state.simpleMeta.get() && description->canBeSwapped && args.size() >= 1 && isStr(args[0])) {
        StrView name(globalObject, strValue(args[0]));
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer swappedArguments;
        swappedArguments.append(makeStr(globalObject, makeString(name.string(), "_be"_s)));
        for (unsigned i = 1; i < args.totalCount(); ++i)
            swappedArguments.append(args.values()[i]);
        Vector<JSValue, 8> values;
        for (unsigned i = 0; i < swappedArguments.size(); ++i)
            values.append(swappedArguments.at(i));
        JSValue swapped = PyNativeFunction::makeInstance(globalObject, rt.typeType(), type->type(), CallArgs(values.span().data(), args.size(), args.keywordNames()));
        RETURN_IF_EXCEPTION(scope, { });
        // (A base may have been given another metaclass since this class was made, and that has the last word.)
        if (!PyType::is(swapped))
            return raiseTypeError(globalObject, scope, "type.__new__() should have returned a class, not '"_s, typeName(globalObject, swapped), '\'');
        PyCTypeInfo* swappedInfo = createInfo(globalObject, PyType::from(swapped));
        RETURN_IF_EXCEPTION(scope, { });
        fillSimpleInfo(vm, swappedInfo, *description, proto, true);
        setAttr(globalObject, self, state.ctypeBEName.get(), swapped);
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, self, state.ctypeLEName.get(), self);
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, swapped, state.ctypeLEName.get(), self);
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, swapped, state.ctypeBEName.get(), swapped);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue simpleMetaFromParam(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    bool isOfType = isInstance(globalObject, value, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType)
        return value;
    PyCTypeInfo* info = infoOf(globalObject, self);
    if (!info || info->kind != TypeKind::Simple)
        RELEASE_AND_RETURN(scope, raiseAbstractClass(globalObject));
    const CodeDescription* description = describeCode(stringOf(strValue(info->proto.get())));
    ASSERT(description);
    Packed packed;
    pack(globalObject, { description->code }, value, info->size, packed);
    if (!scope.exception()) [[likely]] {
        PyCArg* argument = PyCArg::create(globalObject);
        argument->foreignKind = description->foreignKind;
        argument->tag = description->text[0];
        argument->tag2 = description->text.length() > 1 ? description->text[1] : 0;
        memcpy(argument->value, packed.bytes, sizeof(argument->value));
        argument->object.set(vm, argument, packed.keep);
        return argument;
    }
    JSValue exception = takePendingException(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue asParameter = getAttrIfPresent(globalObject, value, state.asParameterName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (asParameter) {
        AsParameterScope nesting;
        if (failed(scope, !nesting.isTooDeep(globalObject)))
            return { };
        RELEASE_AND_RETURN(scope, simpleMetaFromParam(globalObject, self, asParameter));
    }
    return raiseException(globalObject, scope, exception);
}

static constexpr NativeDef simpleMetaMethods[] = {
    PY_SLOT("__init__", simpleMetaInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("from_param", simpleMetaFromParam, "($self, value, /)"),
    CTYPES_COMMON_TYPE_METHODS,
};

const TypeDef simpleMetaDef { "_ctypes.PyCSimpleType"_s, { TypeFlag::Base, TypeFlag::Immutable }, simpleMetaMethods, { }, "metatype for the PyCSimpleType Objects"_s };

// ---- Function pointers ----

JSValue convertersFor(JSGlobalObject* globalObject, JSValue argTypes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyTuple* types = tupleFromIterable(globalObject, argTypes);
    if (scope.exception()) {
        takePendingException(globalObject);
        return raiseTypeError(globalObject, scope, "_argtypes_ must be a sequence of types"_s);
    }
    if (types->length() > 1024)
        return raise(globalObject, scope, state.argumentError.get(), makeString("_argtypes_ has too many arguments ("_s, types->length(), "), maximum is 1024"_s));
    PyTuple* converters = PyTuple::create(globalObject, types->length());
    for (unsigned i = 0; i < types->length(); ++i) {
        JSValue converter = getAttrIfPresent(globalObject, types->at(i), state.fromParamName.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (!converter)
            return raiseTypeError(globalObject, scope, "item "_s, i + 1, " in _argtypes_ has no from_param method"_s);
        converters->initializeAt(vm, i, converter);
    }
    return converters;
}

static JSValue functionMetaInit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(self);
    PyCTypeInfo* info = createInfo(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    info->kind = TypeKind::Function;
    info->foreignKind = ForeignKind::Pointer;
    info->format.set(vm, info, makeStr(vm, "X{}"_s));
    info->alignment = alignof(void*);
    info->length = 1;
    info->size = sizeof(void*);

    PyDict* dict = type->dict();
    JSValue flags = dict->get(state.flagsName.get());
    if (!flags || !isInt(flags))
        return raiseTypeError(globalObject, scope, "class must define _flags_ which must be an integer"_s);
    info->flags = static_cast<unsigned>(lowBitsOfInt(intValue(flags))) | IsPointer | IsAddress;
    if (JSValue argTypes = dict->get(state.argtypesName.get())) {
        JSValue converters = convertersFor(globalObject, argTypes);
        RETURN_IF_EXCEPTION(scope, { });
        info->argTypes.set(vm, info, argTypes);
        info->converters.set(vm, info, converters);
    }
    if (JSValue resultType = dict->get(state.restypeName.get())) {
        if (!isNone(resultType) && !infoOf(globalObject, resultType) && !isCallable(globalObject, resultType))
            return raiseTypeError(globalObject, scope, "_restype_ must be a type, a callable, or None"_s);
        info->resultType.set(vm, info, resultType);
        JSValue checker = getAttrIfPresent(globalObject, resultType, state.checkRetvalName.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (checker)
            info->checker.set(vm, info, checker);
    }
    return none();
}

static constexpr NativeDef functionMetaMethods[] = {
    PY_SLOT("__init__", functionMetaInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("from_param", dataTypeFromParam, "($self, value, /)"),
    CTYPES_COMMON_TYPE_METHODS,
};

const TypeDef functionMetaDef { "_ctypes.PyCFuncPtrType"_s, { TypeFlag::Base, TypeFlag::Immutable }, functionMetaMethods, { }, "metatype for C function pointers"_s };

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
