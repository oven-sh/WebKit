/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyCodecs.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"
#include "PyWeakRef.h"
#include <wtf/HexNumber.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// object, type, and the types of None, NotImplemented and Ellipsis. (Objects/typeobject.c in CPython is the reference.)

// ---- Helpers ----

JSString* attributeName(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(name)) {
        raiseTypeError(globalObject, scope, "attribute name must be string, not '"_s, typeName(globalObject, name), '\'');
        return nullptr;
    }
    JSString* string = strValue(name);
    if (string->isRope()) {
        string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return string;
}

bool rejectKeywords(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args)
{
    if (!args.hasKeywords()) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, function, "() takes no keyword arguments"_s);
    return false;
}

bool rejectKeywordsInNew(JSGlobalObject* globalObject, ASCIILiteral function, PyType* type, PyType* base, const CallArgs& args)
{
    if (!args.hasKeywords()) [[likely]]
        return true;
    if (type != base && type->lookup(Special::Init) != base->lookup(Special::Init))
        return true;
    return rejectKeywords(globalObject, function, args);
}

bool rejectKeywordsInInit(JSGlobalObject* globalObject, ASCIILiteral function, JSValue self, PyType* base, const CallArgs& args)
{
    if (!args.hasKeywords()) [[likely]]
        return true;
    PyType* type = typeOf(globalObject, self);
    if (type != base && type->lookup(Special::New) != base->lookup(Special::New))
        return true;
    return rejectKeywords(globalObject, function, args);
}

bool checkPositional(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, unsigned min, unsigned max)
{
    unsigned count = args.size();
    if (count >= min && count <= max) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned bound = count < min ? min : max;
    raiseTypeError(globalObject, scope, function, " expected "_s, min == max ? ""_s : count < min ? "at least "_s : "at most "_s, bound, " argument"_s, bound == 1 ? ""_s : "s"_s, ", got "_s, count);
    return false;
}

JSValue identityOf(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isString()) {
        // See isIdentical().
        if (const StringImpl* impl = asString(value)->tryGetValueImpl())
            return makeInt(globalObject, static_cast<uint64_t>(std::bit_cast<uintptr_t>(impl)));
    }
    if (value.isCell())
        return makeInt(globalObject, static_cast<uint64_t>(std::bit_cast<uintptr_t>(value.asCell())));
    if (value.isNumber())
        value = jsNumber(value.asNumber());
    else if (value.isNull())
        value = jsUndefined();
    return makeInt(globalObject, static_cast<uint64_t>(JSValue::encode(value)));
}

String addressForRepr(JSGlobalObject* globalObject, JSValue value)
{
    JSValue identity = identityOf(globalObject, value);
    return makeString("0x"_s, intToString(globalObject, identity, 16));
}

static JSValue typeModule(JSGlobalObject* globalObject, PyType* type)
{
    if (!type->isHeapType())
        return type->builtinModule();
    return type->dict()->get(runtime(globalObject).names.dModule());
}

String qualifiedTypeName(JSGlobalObject* globalObject, PyType* type)
{
    JSValue module = typeModule(globalObject, type);
    String qualname = stringOf(type->qualname());
    if (module && module.isString() && !asString(module)->isRope()) {
        String moduleName = stringOf(asString(module));
        if (moduleName != "builtins"_s)
            return makeString(moduleName, '.', qualname);
    }
    return qualname;
}

String typeNameForMessage(JSGlobalObject* globalObject, PyType* type)
{
    String name = qualifiedTypeName(globalObject, type);
    return name.startsWith("__main__."_s) ? name.substring(9) : name;
}

static bool isNativeNew(JSValue method, NativeDef::VariadicWithSelf function)
{
    auto* native = PyNativeFunction::tryFrom(method);
    return native && native->def().signature.isVariadic && native->def().signature.hasSelf && native->def().pointer.variadicWithSelf == function;
}

// The built-in type that decides what kind of cell the instances are.
static PyType* layoutBase(PyType* type)
{
    PyType* base = type->solidBase();
    while (base->isHeapType())
        base = base->base()->solidBase();
    return base;
}

JSValue allocateInstance(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (type == rt.objectType())
        return PyObject::create(vm, rt.structure<PyObject>(), type);
    ASSERT(layoutBase(type) == rt.objectType());
    PyInstance* instance = PyInstance::create(vm, type);
    if (type->lookup(Special::Del)) [[unlikely]]
        rt.registerFinalizable(instance);
    return instance;
}

// ---- object ----

static bool hasExcessArguments(const CallArgs& args) { return args.size() || args.keywordCount(); }

JSValue objectNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);

    if (hasExcessArguments(args)) {
        if (!isNativeNew(type->lookup(Special::New), objectNew))
            return raiseTypeError(globalObject, scope, "object.__new__() takes exactly one argument (the type to instantiate)"_s);
        if (isNativeNew(type->lookup(Special::Init), objectInit))
            return raiseTypeError(globalObject, scope, type->nameInMessages(), "() takes no arguments"_s);
    }

    if (type->has(TypeFlag::Abstract)) [[unlikely]] {
        JSValue methods = type->dict()->get(rt.names.dAbstractMethods());
        if (!methods)
            return raise(globalObject, scope, rt.AttributeError(), "__abstractmethods__"_s);
        JSArray* sorted = listFromIterable(globalObject, methods);
        RETURN_IF_EXCEPTION(scope, { });
        listSort(globalObject, sorted, { }, false);
        RETURN_IF_EXCEPTION(scope, { });
        JSString* joined = strJoin(globalObject, makeStr(vm, "', '"_s), sorted);
        RETURN_IF_EXCEPTION(scope, { });
        String text = joined->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseTypeError(globalObject, scope, "Can't instantiate abstract class "_s, type->name(), " without an implementation for abstract method"_s, listLength(sorted) > 1 ? "s"_s : ""_s, " '"_s, text, '\'');
    }
    // A class derived from a built-in type that has no __new__ because there is no making its instances from Python.
    if (layoutBase(type) != rt.objectType()) [[unlikely]]
        return raiseTypeError(globalObject, scope, "cannot create '"_s, type->nameInMessages(), "' instances"_s);
    return allocateInstance(globalObject, type);
}

JSValue objectInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    if (!hasExcessArguments(args)) [[likely]]
        return jsUndefined();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, self);
    if (!isNativeNew(type->lookup(Special::Init), objectInit))
        return raiseTypeError(globalObject, scope, "object.__init__() takes exactly one argument (the instance to initialize)"_s);
    if (isNativeNew(type->lookup(Special::New), objectNew))
        return raiseTypeError(globalObject, scope, type->nameInMessages(), ".__init__() takes exactly one argument (the instance to initialize)"_s);
    return jsUndefined();
}

JSValue objectRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(globalObject, makeString('<', qualifiedTypeName(globalObject, typeOf(globalObject, self)), " object at "_s, addressForRepr(globalObject, self), '>'));
}

// Whoever asked for the str checks that this is one.
JSValue objectStr(JSGlobalObject* globalObject, JSValue self)
{
    return callSpecial(globalObject, typeOf(globalObject, self)->lookup(Special::Repr), self);
}

JSValue objectHash(JSGlobalObject* globalObject, JSValue self)
{
    uint64_t bits = self.isCell() ? std::bit_cast<uintptr_t>(self.asCell()) : static_cast<uint64_t>(JSValue::encode(self));
    int64_t hash = static_cast<int64_t>((bits >> 4) | (bits << 60));
    return makeInt(globalObject, hash == -1 ? static_cast<int64_t>(-2) : hash);
}

JSValue objectEq(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return isIdentical(self, other) ? jsBoolean(true) : runtime(globalObject).notImplemented();
}

static JSValue objectNe(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The opposite of whatever __eq__ says, if it says anything.
    JSValue method = rt.typeOf(self)->lookup(Special::Eq);
    JSValue result = callSpecial(globalObject, method, self, other);
    RETURN_IF_EXCEPTION(scope, { });
    if (result == rt.notImplemented())
        return result;
    bool equal = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(!equal);
}

static JSValue objectNotComparable(JSGlobalObject* globalObject, JSValue, JSValue)
{
    return runtime(globalObject).notImplemented();
}

JSValue objectGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    JSString* string = attributeName(globalObject, name);
    if (!string)
        return { };
    return genericGetAttr(globalObject, self, string);
}

// object.__setattr__(int, ...) would get around what type.__setattr__ forbids: a method written in C++ is not for the instances
// of a built-in type that has one of its own.
template<typename IsThisFunction>
static bool checkNotOverridden(JSGlobalObject* globalObject, JSValue self, Special special, ASCIILiteral what, const IsThisFunction& isThisFunction)
{
    PyType* type = typeOf(globalObject, self);
    PyTuple* mro = type->mro();
    for (unsigned i = 0; mro && i < mro->length(); ++i) {
        PyType* base = PyType::from(mro->at(i));
        if (base->isHeapType())
            continue;
        auto* native = PyNativeFunction::tryFrom(base->dict()->get(runtime(globalObject).name(special)));
        if (!native)
            continue;
        if (isThisFunction(native->def()))
            return true;
        break;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "can't apply this "_s, what, " to "_s, type->nameInMessages(), " object"_s);
    return false;
}

JSValue objectSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkNotOverridden(globalObject, self, Special::SetAttr, "__setattr__"_s, [](const NativeDef& def) { return def.pointer.f3 == objectSetAttr; }))
        return { };
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    genericSetAttr(globalObject, self, string, value, name.isString() ? JSValue() : name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

JSValue objectDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkNotOverridden(globalObject, self, Special::DelAttr, "__delattr__"_s, [](const NativeDef& def) { return def.pointer.f2 == objectDelAttr; }))
        return { };
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    genericSetAttr(globalObject, self, string, { });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue objectFormat(JSGlobalObject* globalObject, JSValue self, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(spec))
        return raiseTypeError(globalObject, scope, "__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, spec));
    if (strValue(spec)->length())
        return raiseTypeError(globalObject, scope, "unsupported format string passed to "_s, typeName(globalObject, self), ".__format__"_s);
    RELEASE_AND_RETURN(scope, str(globalObject, self));
}

static JSValue objectSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(self.isCell() ? static_cast<int32_t>(self.asCell()->cellSize()) : 16);
}

static JSValue objectSubclassHook(JSGlobalObject* globalObject, JSValue, const CallArgs&)
{
    return runtime(globalObject).notImplemented();
}

static JSValue objectInitSubclass(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, stringOf(PyType::from(cls)->qualname()), ".__init_subclass__() takes no keyword arguments"_s);
    if (args.size())
        return raiseTypeError(globalObject, scope, stringOf(PyType::from(cls)->qualname()), ".__init_subclass__() takes no arguments ("_s, args.size(), " given)"_s);
    return jsUndefined();
}

// Adds the names in the dicts of the class and of its bases.
static void mergeClassDict(JSGlobalObject* globalObject, PyDict* names, JSValue cls)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue classDict = getAttrIfPresent(globalObject, cls, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, void());
    if (classDict) {
        dictUpdate(globalObject, names, classDict);
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSValue bases = getAttrIfPresent(globalObject, cls, rt.names.dBases());
    RETURN_IF_EXCEPTION(scope, void());
    if (!bases)
        return;
    int64_t count = length(globalObject, bases);
    if (count < 0) {
        takePendingException(globalObject);
        return;
    }
    for (int64_t i = 0; i < count; ++i) {
        JSValue base = getItem(globalObject, bases, jsNumber(static_cast<double>(i)));
        RETURN_IF_EXCEPTION(scope, void());
        mergeClassDict(globalObject, names, base);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static JSValue keysAsList(JSGlobalObject* globalObject, PyDict* dict)
{
    JSArray* result = newList(globalObject, dict->size());
    JSValue key, value;
    for (unsigned position = 0; dict->next(position, key, value);)
        listAppend(globalObject, result, key);
    return result;
}

static JSValue objectDir(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* names;
    JSValue dict = getAttrIfPresent(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (PyDict* own = PyDict::tryFrom(dict))
        names = own->copy(globalObject);
    else
        names = PyDict::create(globalObject);
    JSValue cls = getAttrIfPresent(globalObject, self, rt.names.dClass());
    RETURN_IF_EXCEPTION(scope, { });
    if (cls) {
        mergeClassDict(globalObject, names, cls);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return keysAsList(globalObject, names);
}

static JSValue objectGetClass(JSGlobalObject* globalObject, JSValue self)
{
    return typeOf(globalObject, self);
}

static bool sameSlots(PyType* a, PyType* b)
{
    PyTuple* slotsA = a->slots();
    PyTuple* slotsB = b->slots();
    unsigned countA = slotsA ? slotsA->length() : 0;
    unsigned countB = slotsB ? slotsB->length() : 0;
    if (countA != countB)
        return false;
    for (unsigned i = 0; i < countA; ++i) {
        if (!WTF::equal(asString(slotsA->at(i))->tryGetValueImpl(), asString(slotsB->at(i))->tryGetValueImpl()))
            return false;
    }
    return true;
}

// Whether an instance of one can become an instance of the other. If it is a class that gets a new base in place of an old
// one, the new base may have a __dict__ or a __weakref__ only if the class has.
static bool layoutsAreCompatible(PyType* oldType, PyType* newType, PyType* derived = nullptr)
{
    for (TypeFlag flag : { TypeFlag::HasDict, TypeFlag::HasWeakref }) {
        if (derived ? newType->has(flag) && !derived->has(flag) : oldType->has(flag) != newType->has(flag))
            return false;
    }
    PyType* a = oldType->solidBase();
    PyType* b = newType->solidBase();
    while (a != b) {
        if (!a->isHeapType() || !b->isHeapType() || !sameSlots(a, b))
            return false;
        a = a->base()->solidBase();
        b = b->base()->solidBase();
    }
    return true;
}

static void objectSetClass(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete __class__ attribute"_s);
        return;
    }
    PyType* newType = PyType::tryFrom(value);
    if (!newType) {
        raiseTypeError(globalObject, scope, "__class__ must be set to a class, not '"_s, typeName(globalObject, value), "' object"_s);
        return;
    }
    if (isAudited(globalObject) && !audit(globalObject, "object.__setattr__"_s, self, makeStr(vm, "__class__"_s), value))
        return;
    PyType* oldType = rt.typeOf(self);
    bool areModules = oldType->isSubtypeOf(rt.moduleType()) && newType->isSubtypeOf(rt.moduleType());
    if (!areModules && (!oldType->isHeapType() || !newType->isHeapType())) {
        raiseTypeError(globalObject, scope, "__class__ assignment only supported for mutable types or ModuleType subclasses"_s);
        return;
    }
    if (!layoutsAreCompatible(oldType, newType)) {
        raiseTypeError(globalObject, scope, "__class__ assignment: '"_s, newType->name(), "' object layout differs from '"_s, oldType->name(), '\'');
        return;
    }
    JSCell* cell = self.asCell();
    if (isPyObject(cell)) {
        uncheckedDowncast<PyObject>(cell)->setType(vm, newType);
        // What was remembered about the attributes of a module goes by its dict alone: see describeAttribute().
        if (auto* module = PyModule::tryFrom(self))
            module->dict()->renewWatchedVersion();
    } else if (PyListObject::is(cell))
        uncheckedDowncast<PyListObject>(cell)->setType(vm, newType);
    else
        PyType::from(self)->setType(vm, newType);
}

static constexpr NativeDef objectMethods[] = {
    PY_NEW(objectNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", objectInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", objectRepr, "($self, /)"),
    PY_SLOT("__str__", objectStr, "($self, /)"),
    PY_SLOT("__hash__", objectHash, "($self, /)"),
    PY_SLOT("__eq__", objectEq, "($self, value, /)"),
    PY_SLOT("__ne__", objectNe, "($self, value, /)"),
    PY_SLOT("__lt__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__le__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__gt__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__ge__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", objectSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", objectDelAttr, "($self, name, /)"),
    PY_METHOD("__format__", objectFormat, "($self, format_spec, /)"),
    PY_METHOD("__sizeof__", objectSizeOf, "($self, /)"),
    PY_METHOD("__dir__", objectDir, "($self, /)"),
    PY_METHOD("__reduce__", objectReduce, "($self, /)"),
    PY_METHOD("__reduce_ex__", objectReduceEx, "($self, protocol, /)"),
    PY_METHOD("__getstate__", objectGetState, "($self, /)"),
    PY_CLASSMETHOD("__subclasshook__", objectSubclassHook, "($type, *args, **kwargs)"),
    PY_CLASSMETHOD("__init_subclass__", objectInitSubclass, "($type, *args, **kwargs)"),
};

static constexpr NativeGetSet objectGetSets[] = {
    { "__class__"_s, objectGetClass, objectSetClass },
};

// ---- The __dict__ and __weakref__ of instances ----

static JSValue instanceGetDict(JSGlobalObject* globalObject, JSValue self)
{
    if (PyDict* dict = instanceDict(globalObject, self, true))
        return dict;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "This object has no __dict__"_s);
}

static void instanceSetDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value && !PyDict::is(value)) {
        raiseTypeError(globalObject, scope, "__dict__ must be set to a dictionary, not a '"_s, typeName(globalObject, value), '\'');
        return;
    }
    if (!setInstanceDict(globalObject, self, value ? PyDict::from(value) : PyDict::create(globalObject)))
        raise(globalObject, scope, runtime(globalObject).AttributeError(), "This object has no __dict__"_s);
}

static JSValue instanceGetWeakref(JSGlobalObject* globalObject, JSValue self)
{
    if (typeOf(globalObject, self)->has(TypeFlag::HasWeakref))
        return firstWeakRefTo(globalObject, self);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "This object has no __weakref__"_s);
}

// That of an instance of a built-in type can be replaced, but not deleted.
static void builtinInstanceSetDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (value)
        return instanceSetDict(globalObject, self, value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "cannot delete __dict__"_s);
}

static constexpr NativeGetSet instanceDictGetSet[] = { { "__dict__"_s, instanceGetDict, instanceSetDict, false, "dictionary for instance variables"_s } };
static constexpr NativeGetSet builtinInstanceDictGetSet[] = { { "__dict__"_s, instanceGetDict, builtinInstanceSetDict, false, "dictionary for instance variables"_s } };
static constexpr NativeGetSet instanceWeakrefGetSet[] = { { "__weakref__"_s, instanceGetWeakref, nullptr, false, "list of weak references to the object"_s } };

std::span<const NativeGetSet> dictGetSet() { return builtinInstanceDictGetSet; }

// ---- type: attributes ----

JSValue typeGetAttr(JSGlobalObject* globalObject, PyType* type, JSString* name, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* metatype = type->type();
    JSValue metaAttribute = metatype->lookup(name);
    JSValue metaGetter;
    if (metaAttribute) {
        PyType* attributeType = rt.typeOf(metaAttribute);
        metaGetter = attributeType->lookup(Special::Get);
        if (metaGetter && (attributeType->lookup(Special::Set) || attributeType->lookup(Special::Delete)))
            RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, metaAttribute, type, metatype));
    }
    if (JSValue attribute = type->lookup(name))
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, attribute, { }, type));
    if (metaGetter)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, metaAttribute, type, metatype));
    if (metaAttribute)
        return metaAttribute;
    if (suppress)
        return { };
    return raiseAttributeError(globalObject, scope, type, name, makeString("type object '"_s, type->nameInMessages(), "' has no attribute '"_s, stringOf(name), '\''));
}

JSValue typeGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    JSString* string = attributeName(globalObject, name);
    if (!string)
        return { };
    return typeGetAttr(globalObject, PyType::from(self), string);
}

static void typeSetAttrImpl(JSGlobalObject* globalObject, PyType* type, JSString* name, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!type->isHeapType() && !type->has(TypeFlag::Mutable)) {
        raiseTypeError(globalObject, scope, "cannot set "_s, strRepr(stringOf(name)), " attribute of immutable type '"_s, type->nameInMessages(), '\'');
        return;
    }

    // type.__name__ and the like.
    PyType* metatype = type->type();
    if (JSValue descriptor = metatype->lookup(name)) {
        PyType* descriptorType = rt.typeOf(descriptor);
        if (descriptorType->lookup(Special::Set) || descriptorType->lookup(Special::Delete))
            RELEASE_AND_RETURN(scope, genericSetAttr(globalObject, type, name, value));
    }

    name = internString(globalObject, name);
    RETURN_IF_EXCEPTION(scope, void());
    if (value)
        type->dict()->set(vm, name, value);
    else if (!type->dict()->remove(vm, name)) {
        raiseAttributeError(globalObject, scope, type, name, makeString("type object '"_s, type->nameInMessages(), "' has no attribute '"_s, stringOf(name), '\''));
        return;
    }
    type->didChange(vm);
}

static JSValue typeSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    typeSetAttrImpl(globalObject, PyType::from(self), string, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue typeDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    typeSetAttrImpl(globalObject, PyType::from(self), string, { });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

// ---- type: calling one ----

JSValue typeCall(JSGlobalObject* globalObject, PyType* type, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (type == rt.typeType()) {
        if (args.size() == 1 && !args.hasKeywords())
            return rt.typeOf(args[0]);
        if (args.size() != 3)
            return raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s);
    }

    JSValue newMethod = type->lookup(Special::New);
    if (!newMethod || type->has(TypeFlag::DisallowInstantiation))
        return raiseTypeError(globalObject, scope, "cannot create '"_s, type->nameInMessages(), "' instances"_s);

    JSValue object;
    if (auto* native = PyNativeFunction::tryFrom(newMethod); native && native->kind() == NativeKind::New)
        object = native->invokeWithSelf(globalObject, type, args);
    else {
        JSValue function;
        if (auto* wrapper = dynamicDowncast<PyWrapperObject>(newMethod); wrapper && wrapper->type() == rt.staticmethodType())
            function = wrapper->wrapped();
        else {
            function = bindDescriptor(globalObject, newMethod, { }, type);
            RETURN_IF_EXCEPTION(scope, { });
        }
        object = withFirstArgument(type, args, [&](const CallArgs& all) {
            return call(globalObject, function, all);
        });
    }
    RETURN_IF_EXCEPTION(scope, { });

    // What __new__ made may be something else altogether, which is then left alone.
    PyType* objectType = rt.typeOf(object);
    if (!objectType->isSubtypeOf(type))
        return object;
    if (objectType->lookup(Special::Del) && object.isCell()) [[unlikely]]
        rt.registerFinalizable(object.asCell());
    JSValue init = objectType->lookup(Special::Init);
    if (!init)
        return object;
    JSValue result = callSpecial(globalObject, init, object, args);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(result))
        return raiseTypeError(globalObject, scope, "__init__() should return None, not '"_s, typeName(globalObject, result), '\'');
    return object;
}

static JSValue typeCallMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    return typeCall(globalObject, PyType::from(self), args);
}

JSValue prepareConstruct(JSGlobalObject* globalObject, JSValue callee, JSValue& self)
{
    PyType* type = PyType::tryFrom(callee);
    if (!type || type->has(TypeFlag::Abstract) || type->has(TypeFlag::DisallowInstantiation))
        return { };
    JSValue init = type->lookup(Special::Init);
    if (!PyFunction::is(init) || !isNativeNew(type->lookup(Special::New), objectNew) || !isNativeNew(type->type()->lookup(Special::Call), typeCallMethod) || layoutBase(type) != runtime(globalObject).objectType())
        return { };
    self = allocateInstance(globalObject, type);
    return init;
}

// ---- type: making one ----

PyType* calculateMetaclass(JSGlobalObject* globalObject, PyType* metatype, PyTuple* bases)
{
    Runtime& rt = runtime(globalObject);
    PyType* winner = metatype;
    for (unsigned i = 0; i < bases->length(); ++i) {
        PyType* candidate = rt.typeOf(bases->at(i));
        if (winner->isSubtypeOf(candidate))
            continue;
        if (candidate->isSubtypeOf(winner)) {
            winner = candidate;
            continue;
        }
        VM& vm = rt.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "metaclass conflict: the metaclass of a derived class must be a (non-strict) subclass of the metaclasses of all its bases"_s);
        return nullptr;
    }
    return winner;
}

// The first of the bases on the way to the one that decides the layout of the instances.
static PyType* findBestBase(JSGlobalObject* globalObject, PyTuple* bases)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* best = nullptr;
    PyType* winner = nullptr;
    for (unsigned i = 0; i < bases->length(); ++i) {
        PyType* base = PyType::tryFrom(bases->at(i));
        if (!base) {
            raiseTypeError(globalObject, scope, "bases must be types; got '"_s, typeName(globalObject, bases->at(i)), '\'');
            return nullptr;
        }
        if (!base->has(TypeFlag::Base)) {
            raiseTypeError(globalObject, scope, "type '"_s, base->nameInMessages(), "' is not an acceptable base type"_s);
            return nullptr;
        }
        PyType* candidate = base->solidBase();
        if (!winner) {
            winner = candidate;
            best = base;
        } else if (winner->isSubtypeOf(candidate)) {
        } else if (candidate->isSubtypeOf(winner)) {
            winner = candidate;
            best = base;
        } else {
            raiseTypeError(globalObject, scope, "multiple bases have instance lay-out conflict"_s);
            return nullptr;
        }
    }
    return best;
}

static bool isIdentifier(StringView text)
{
    if (text.isEmpty())
        return false;
    bool first = true;
    for (char32_t character : text.codePoints()) {
        if (first ? !(character == '_' || isXIDStart(character)) : !isXIDContinue(character))
            return false;
        first = false;
    }
    return true;
}

// __name in class C is _C__name.
static String mangle(const String& className, const String& name)
{
    if (!name.startsWith("__"_s) || name.endsWith("__"_s) || name.contains('.'))
        return name;
    unsigned start = 0;
    while (start < className.length() && className[start] == '_')
        ++start;
    if (start == className.length())
        return name;
    return makeString('_', StringView(className).substring(start), name);
}

// Sets the MRO of the type, by the mro() of its metaclass. False: an exception.
static bool resolveMRO(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyTuple* mro;
    if (type->type() == rt.typeType()) {
        mro = computeMRO(globalObject, type);
        RETURN_IF_EXCEPTION(scope, false);
    } else {
        JSValue result = callMethod(globalObject, type, rt.names.mro());
        RETURN_IF_EXCEPTION(scope, false);
        mro = tupleFromIterable(globalObject, result);
        RETURN_IF_EXCEPTION(scope, false);
        if (!mro->length()) {
            raiseTypeError(globalObject, scope, "type MRO must not be empty"_s);
            return false;
        }
        PyType* solid = type->solidBase();
        for (unsigned i = 0; i < mro->length(); ++i) {
            PyType* base = PyType::tryFrom(mro->at(i));
            if (!base) {
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".mro() returned a non-class ('"_s, typeNameForMessage(globalObject, rt.typeOf(mro->at(i))), "')"_s);
                return false;
            }
            if (!solid->isSubtypeOf(base->solidBase())) {
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".mro() returned base with unsuitable layout ('"_s, typeNameForMessage(globalObject, base), "')"_s);
                return false;
            }
        }
    }
    type->setMRO(vm, mro);
    type->didChange(vm);
    return true;
}

// Raises the exception again, with one more note.
static void raiseWithNote(JSGlobalObject* globalObject, JSValue exception, const String& note)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue notes = getAttrIfPresent(globalObject, exception, rt.names.dNotes());
    if (!scope.exception()) {
        if (!notes) {
            notes = newList(globalObject);
            setAttr(globalObject, exception, rt.names.dNotes(), notes);
        }
        if (!scope.exception() && isList(notes))
            listAppend(globalObject, asList(notes), makeStr(vm, note));
    }
    takePendingException(globalObject);
    raiseException(globalObject, scope, exception);
}

// The name and the docstring of a class are kept as UTF-8 in CPython, which not every str has. False: a UnicodeEncodeError.
static bool checkEncodable(JSGlobalObject* globalObject, JSString* string)
{
    if (StringImpl* impl = string->tryGetValueImpl(); impl && impl->is8Bit()) [[likely]]
        return true;
    return !!codecEncodeUTF8(globalObject, string, "strict"_s);
}

static JSValue typeNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* metatype = PyType::from(cls);

    if (args.size() != 3)
        return raiseTypeError(globalObject, scope, "type.__new__() takes exactly 3 arguments ("_s, args.size(), " given)"_s);
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "type.__new__() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]));
    if (!PyTuple::is(args[1]))
        return raiseTypeError(globalObject, scope, "type.__new__() argument 2 must be tuple, not "_s, typeName(globalObject, args[1]));
    if (!PyDict::is(args[2]))
        return raiseTypeError(globalObject, scope, "type.__new__() argument 3 must be dict or frozendict, not "_s, typeName(globalObject, args[2]));
    JSString* name = internString(globalObject, strValue(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* bases = PyTuple::from(args[1]);
    String nameText = stringOf(name);
    if (nameText.contains('\0'))
        return raiseValueError(globalObject, scope, "type name must not contain null characters"_s);
    if (!checkEncodable(globalObject, name))
        return { };

    // ---- The bases ----

    PyType* base;
    if (!bases->length()) {
        base = rt.objectType();
        bases = PyTuple::create(globalObject, { JSValue(base) });
    } else {
        for (unsigned i = 0; i < bases->length(); ++i) {
            if (PyType::is(bases->at(i)))
                continue;
            bool hasEntries = hasAttr(globalObject, bases->at(i), rt.name(Special::MroEntries));
            RETURN_IF_EXCEPTION(scope, { });
            if (hasEntries)
                return raiseTypeError(globalObject, scope, "type() doesn't support MRO entry resolution; use types.new_class()"_s);
        }
        PyType* winner = calculateMetaclass(globalObject, metatype, bases);
        RETURN_IF_EXCEPTION(scope, { });
        if (winner != metatype) {
            JSValue winnerNew = winner->lookup(Special::New);
            if (!isNativeNew(winnerNew, typeNew)) {
                JSValue function = bindDescriptor(globalObject, winnerNew, { }, winner);
                RETURN_IF_EXCEPTION(scope, { });
                if (auto* native = PyNativeFunction::tryFrom(function); native && native->kind() == NativeKind::New)
                    RELEASE_AND_RETURN(scope, invokeNative(globalObject, native->def(), winner, args));
                RELEASE_AND_RETURN(scope, withFirstArgument(winner, args, [&](const CallArgs& all) {
                    return call(globalObject, function, all);
                }));
            }
            metatype = winner;
        }
        base = findBestBase(globalObject, bases);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // In the order in which the namespace iterates, if it is of a class with an order of its own.
    PyDict* dict = PyDict::create(globalObject);
    dictUpdateFromMapping(globalObject, dict, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSValue doc = dict->get(rt.names.dDoc()); doc && doc.isString() && !checkEncodable(globalObject, asString(doc)))
        return { };

    // ---- __slots__ ----

    bool mayAddDict = !base->has(TypeFlag::HasDict);
    bool mayAddWeakref = !base->has(TypeFlag::HasWeakref);
    bool addDict = false;
    bool addWeakref = false;
    PyTuple* slots = nullptr;
    if (JSValue slotsValue = dict->get(rt.names.dSlots())) {
        PyTuple* given;
        if (isStr(slotsValue))
            given = PyTuple::create(globalObject, { slotsValue });
        else {
            given = tupleFromIterable(globalObject, slotsValue);
            RETURN_IF_EXCEPTION(scope, { });
        }
        Vector<String, 8> names;
        for (unsigned i = 0; i < given->length(); ++i) {
            JSValue item = given->at(i);
            if (!isStr(item))
                return raiseTypeError(globalObject, scope, "__slots__ items must be strings, not '"_s, typeName(globalObject, item), '\'');
            String slot = strValue(item)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isIdentifier(slot))
                return raiseTypeError(globalObject, scope, "__slots__ must be identifiers"_s);
            if (slot == "__dict__"_s) {
                if (!mayAddDict || addDict)
                    return raiseTypeError(globalObject, scope, "__dict__ slot disallowed: we already got one"_s);
                addDict = true;
                continue;
            }
            if (slot == "__weakref__"_s) {
                if (!mayAddWeakref || addWeakref)
                    return raiseTypeError(globalObject, scope, "__weakref__ slot disallowed: we already got one"_s);
                addWeakref = true;
                continue;
            }
            slot = mangle(nameText, slot);
            if (dict->get(internString(vm, slot)) && slot != "__qualname__"_s && slot != "__classcell__"_s && slot != "__classdictcell__"_s)
                return raiseValueError(globalObject, scope, strRepr(slot), " in __slots__ conflicts with class variable"_s);
            names.append(slot);
        }
        std::ranges::sort(names, [](const String& a, const String& b) {
            return codePointCompareLessThan(a, b);
        });
        if (!names.isEmpty()) {
            PyType* layout = layoutBase(base);
            if (layout == rt.intType() || layout == rt.bytesType() || layout == rt.typeType())
                return raiseTypeError(globalObject, scope, "arbitrary __slots__ not supported for subtype of '"_s, base->name(), '\'');
            slots = PyTuple::create(globalObject, names.size());
            for (unsigned i = 0; i < names.size(); ++i)
                slots->initializeAt(vm, i, internString(vm, names[i]));
        }
        // The other bases may bring a __dict__ along.
        for (unsigned i = 0; i < bases->length(); ++i) {
            PyType* other = PyType::from(bases->at(i));
            if (other == base)
                continue;
            if (mayAddDict && other->has(TypeFlag::HasDict))
                addDict = true;
            if (mayAddWeakref && other->has(TypeFlag::HasWeakref))
                addWeakref = true;
        }
    } else {
        addDict = mayAddDict;
        addWeakref = mayAddWeakref;
    }

    // ---- The type ----

    PyType* type = PyType::createBare(vm, rt.typeStructure());
    type->setType(vm, metatype);
    type->setNameString(vm, name);
    type->setBases(vm, bases);
    type->setBase(vm, base);
    type->setDict(vm, dict);
    type->setSlots(vm, slots);
    type->setSolidBase(vm, slots ? type : base->solidBase());
    OptionSet<TypeFlag> flags { TypeFlag::Heap, TypeFlag::Base };
    flags.add(base->flags() & inheritedTypeFlags);
    if (addDict)
        flags.add(TypeFlag::HasDict);
    if (addWeakref)
        flags.add(TypeFlag::HasWeakref);
    if (slots)
        flags.add(TypeFlag::OwnLayout);
    type->setFlags(flags);

    if (!dict->get(rt.names.dModule())) {
        if (PyDict* globals = currentGlobals(globalObject)) {
            if (JSValue module = globals->get(rt.names.dName()))
                dict->set(vm, rt.names.dModule(), module);
        }
    }
    if (JSValue qualname = dict->get(rt.names.dQualname())) {
        if (!isStr(qualname))
            return raiseTypeError(globalObject, scope, "type __qualname__ must be a str, not "_s, typeName(globalObject, qualname));
        JSString* string = strValue(qualname);
        string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        type->setQualname(vm, string);
        dict->remove(vm, rt.names.dQualname());
    }

    // A plain function under one of these names is not an ordinary method.
    if (JSValue function = dict->get(rt.name(Special::New)); function && PyFunction::is(function))
        dict->set(vm, rt.name(Special::New), PyWrapperObject::create(globalObject, rt.staticmethodType(), function));
    for (Special special : { Special::InitSubclass, Special::ClassGetItem }) {
        if (JSValue function = dict->get(rt.name(special)); function && PyFunction::is(function))
            dict->set(vm, rt.name(special), PyWrapperObject::create(globalObject, rt.classmethodType(), function));
    }

    if (slots) {
        for (unsigned i = 0; i < slots->length(); ++i) {
            JSString* slot = asString(slots->at(i));
            dict->set(vm, slot, PyMemberDescriptor::create(globalObject, slot, type));
        }
    }
    // One descriptor serves all classes, so it is object's.
    auto commonDescriptor = [&](Runtime::Slot slot, const NativeGetSet& def) {
        JSValue descriptor = rt.slot(slot);
        if (!descriptor) {
            descriptor = PyGetSetDescriptor::create(globalObject, def, rt.objectType());
            rt.setSlot(slot, descriptor);
        }
        return descriptor;
    };
    // What the body of the class bound the names to stays.
    if (addDict && !dict->get(rt.names.dDict()))
        dict->set(vm, rt.names.dDict(), commonDescriptor(Runtime::Slot::InstanceDictDescriptor, instanceDictGetSet[0]));
    if (addWeakref && !dict->get(rt.names.dWeakref()))
        dict->set(vm, rt.names.dWeakref(), commonDescriptor(Runtime::Slot::InstanceWeakrefDescriptor, instanceWeakrefGetSet[0]));
    if (!dict->get(rt.names.dDoc()))
        dict->set(vm, rt.names.dDoc(), jsUndefined());

    if (JSValue cell = dict->get(rt.names.dClassCell())) {
        if (!PyCell::is(cell))
            return raiseTypeError(globalObject, scope, "__classcell__ must be a nonlocal cell, not "_s, reprForMessage(globalObject, rt.typeOf(cell)));
        PyCell::from(cell)->setValue(vm, type);
        dict->remove(vm, rt.names.dClassCell());
    }
    if (JSValue cell = dict->get(rt.names.dClassDictCell())) {
        if (!PyCell::is(cell))
            return raiseTypeError(globalObject, scope, "__classdictcell__ must be a nonlocal cell, not "_s, reprForMessage(globalObject, rt.typeOf(cell)));
        PyCell::from(cell)->setValue(vm, dict);
        dict->remove(vm, rt.names.dClassDictCell());
    }

    // What defines equality without saying how to hash cannot be hashed.
    if (dict->get(rt.name(Special::Eq)) && !dict->get(rt.name(Special::Hash)))
        dict->set(vm, rt.name(Special::Hash), jsUndefined());

    resolveMRO(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });

    // A sequence or a mapping is so for `match` whichever base says it.
    if (!type->has(TypeFlag::Sequence) && !type->has(TypeFlag::Mapping)) {
        PyTuple* mro = type->mro();
        for (unsigned i = 1; i < mro->length(); ++i) {
            PyType* ancestor = PyType::from(mro->at(i));
            if (ancestor->has(TypeFlag::Sequence) || ancestor->has(TypeFlag::Mapping)) {
                type->add(ancestor->has(TypeFlag::Sequence) ? TypeFlag::Sequence : TypeFlag::Mapping);
                break;
            }
        }
    }
    for (unsigned i = 0; i < bases->length(); ++i)
        PyType::from(bases->at(i))->addSubclass(globalObject, type);

    // ---- __set_name__, __init_subclass__ ----

    {
        PyDict* snapshot = dict->copy(globalObject);
        JSValue key, value;
        for (unsigned position = 0; snapshot->next(position, key, value);) {
            PyType* valueType = rt.typeOf(value);
            JSValue setName = valueType->lookup(Special::SetName);
            if (!setName)
                continue;
            callSpecial(globalObject, setName, value, type, key);
            if (scope.exception()) [[unlikely]] {
                // Nothing else can run while it propagates.
                JSValue exception = takePendingException(globalObject);
                raiseWithNote(globalObject, exception, makeString("Error calling __set_name__ on '"_s, valueType->name(), "' instance "_s, reprForMessage(globalObject, key), " in '"_s, nameText, '\''));
                return { };
            }
        }
    }

    {
        // super(type, type).__init_subclass__(**kwargs)
        // (A metaclass's mro() may have put the type anywhere. With nothing after it, super() finds what it has itself: object's.)
        PyTuple* mro = type->mro();
        unsigned position = 0;
        while (position < mro->length() && mro->at(position) != JSValue(type))
            ++position;
        if (position == mro->length()) [[unlikely]]
            return raiseTypeError(globalObject, scope, "super(type, obj): obj (type "_s, type->name(), ") is not an instance or subtype of type ("_s, type->name(), ")."_s);
        JSValue method;
        for (unsigned i = position + 1; i < mro->length() && !method; ++i)
            method = PyType::from(mro->at(i))->dict()->get(rt.name(Special::InitSubclass));
        if (!method)
            method = rt.objectType()->dict()->get(rt.name(Special::InitSubclass));
        // That of object does nothing but refuse arguments.
        auto* native = PyNativeFunction::tryFrom(method);
        if (!native || native->ownerType() != rt.objectType() || args.hasKeywords()) {
            JSValue bound = bindDescriptor(globalObject, method, { }, type);
            RETURN_IF_EXCEPTION(scope, { });
            call(globalObject, bound, CallArgs(args.values() + args.size(), 0, args.keywordNames()));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return type;
}

static JSValue typeInit(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords() && args.size() == 1)
        return raiseTypeError(globalObject, scope, "type.__init__() takes no keyword arguments"_s);
    if (args.size() != 1 && args.size() != 3)
        return raiseTypeError(globalObject, scope, "type.__init__() takes 1 or 3 arguments"_s);
    return jsUndefined();
}

// ---- type: the rest ----

static JSValue typeRepr(JSGlobalObject* globalObject, JSValue self)
{
    PyType* type = PyType::from(self);
    // With no module to show, it is __name__ that CPython shows, not __qualname__.
    JSValue module = typeModule(globalObject, type);
    bool showsModule = module && module.isString() && !asString(module)->isRope() && stringOf(asString(module)) != "builtins"_s;
    return makeStr(globalObject, makeString("<class '"_s, showsModule ? qualifiedTypeName(globalObject, type) : stringOf(type->nameString()), "'>"_s));
}

static JSValue typeMRO(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* mro = computeMRO(globalObject, PyType::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    return newList(globalObject, mro->span());
}

static JSValue typeSubclasses(JSGlobalObject* globalObject, JSValue self)
{
    MarkedArgumentBuffer subclasses;
    PyType::from(self)->appendSubclassesTo(subclasses);
    return newList(globalObject, spanOf(subclasses));
}

static JSValue typePrepare(JSGlobalObject* globalObject, JSValue, const CallArgs&)
{
    return PyDict::create(globalObject);
}

static JSValue typeInstanceCheck(JSGlobalObject* globalObject, JSValue self, JSValue instance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = isInstanceWithoutHook(globalObject, instance, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue typeSubclassCheck(JSGlobalObject* globalObject, JSValue self, JSValue subclass)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = isSubclassWithoutHook(globalObject, subclass, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue typeDir(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* names = PyDict::create(globalObject);
    mergeClassDict(globalObject, names, self);
    RETURN_IF_EXCEPTION(scope, { });
    return keysAsList(globalObject, names);
}

static JSValue typeOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, self, other); }
static JSValue typeROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, other, self); }

static bool checkMutable(JSGlobalObject* globalObject, PyType* type, JSValue value, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!type->isHeapType() && !type->has(TypeFlag::Mutable)) {
        raiseTypeError(globalObject, scope, "cannot set '"_s, name, "' attribute of immutable type '"_s, type->nameInMessages(), '\'');
        return false;
    }
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete '"_s, name, "' attribute of type '"_s, type->name(), '\'');
        return false;
    }
    if (isAudited(globalObject))
        RELEASE_AND_RETURN(scope, audit(globalObject, "object.__setattr__"_s, type, makeStr(vm, name), value));
    return true;
}

static JSValue typeGetName(JSGlobalObject*, JSValue self) { return PyType::from(self)->nameString(); }

static void typeSetName(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__name__"_s))
        return;
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "can only assign string to "_s, type->name(), ".__name__, not '"_s, typeName(globalObject, value), '\'');
        return;
    }
    JSString* name = internString(globalObject, strValue(value));
    RETURN_IF_EXCEPTION(scope, void());
    if (stringOf(name).contains('\0')) {
        raiseValueError(globalObject, scope, "type name must not contain null characters"_s);
        return;
    }
    if (!checkEncodable(globalObject, name))
        return;
    if (!type->qualname() || type->qualname() == type->nameString())
        type->setQualname(vm, type->nameString());
    type->setNameString(vm, name);
}

static JSValue typeGetQualname(JSGlobalObject*, JSValue self) { return PyType::from(self)->qualname(); }

static void typeSetQualname(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__qualname__"_s))
        return;
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "can only assign string to "_s, type->name(), ".__qualname__, not '"_s, typeName(globalObject, value), '\'');
        return;
    }
    JSString* string = strValue(value);
    string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    type->setQualname(vm, string);
}

static JSValue typeGetModule(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue module = typeModule(globalObject, PyType::from(self)))
        return module;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "__module__"_s);
}

static void typeSetModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__module__"_s))
        return;
    Runtime& rt = runtime(globalObject);
    type->dict()->set(rt.vm(), rt.names.dModule(), value);
    type->dict()->remove(rt.vm(), rt.names.dFirstLineNo());
    type->didChange(rt.vm());
}

static JSValue typeGetBases(JSGlobalObject*, JSValue self) { return PyType::from(self)->bases(); }
static JSValue typeGetMRO(JSGlobalObject*, JSValue self)
{
    PyTuple* mro = PyType::from(self)->mro();
    return mro ? JSValue(mro) : jsUndefined();
}
static JSValue typeGetBase(JSGlobalObject*, JSValue self)
{
    PyType* base = PyType::from(self)->base();
    return base ? JSValue(base) : jsUndefined();
}

// Works out the MRO of the type again, and of all that derive from it.
static bool updateMROs(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    resolveMRO(globalObject, type);
    RETURN_IF_EXCEPTION(scope, false);
    MarkedArgumentBuffer subclasses;
    type->appendSubclassesTo(subclasses);
    for (unsigned i = 0; i < subclasses.size(); ++i) {
        updateMROs(globalObject, PyType::from(subclasses.at(i)));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static void typeSetBases(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__bases__"_s))
        return;
    PyTuple* bases = PyTuple::tryFrom(value);
    if (!bases) {
        raiseTypeError(globalObject, scope, "can only assign tuple to "_s, type->name(), ".__bases__, not "_s, typeName(globalObject, value));
        return;
    }
    if (!bases->length()) {
        raiseTypeError(globalObject, scope, "can only assign non-empty tuple to "_s, type->name(), ".__bases__, not ()"_s);
        return;
    }
    for (unsigned i = 0; i < bases->length(); ++i) {
        PyType* base = PyType::tryFrom(bases->at(i));
        if (!base) {
            raiseTypeError(globalObject, scope, type->name(), ".__bases__ must be tuple of classes, not '"_s, typeName(globalObject, bases->at(i)), '\'');
            return;
        }
        if (base->isSubtypeOf(type)) {
            raiseTypeError(globalObject, scope, "a __bases__ item causes an inheritance cycle"_s);
            return;
        }
    }
    PyType* base = findBestBase(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, void());
    if (!layoutsAreCompatible(type->base(), base, type)) {
        raiseTypeError(globalObject, scope, "__bases__ assignment: '"_s, base->name(), "' object layout differs from '"_s, type->base()->name(), '\'');
        return;
    }

    PyTuple* oldBases = type->bases();
    PyType* oldBase = type->base();
    PyTuple* oldMRO = type->mro();
    type->setBases(vm, bases);
    type->setBase(vm, base);
    if (!type->slots())
        type->setSolidBase(vm, base->solidBase());
    updateMROs(globalObject, type);
    if (scope.exception()) {
        type->setBases(vm, oldBases);
        type->setBase(vm, oldBase);
        if (!type->slots())
            type->setSolidBase(vm, oldBase->solidBase());
        type->setMRO(vm, oldMRO);
        type->didChange(vm);
        return;
    }
    for (unsigned i = 0; i < oldBases->length(); ++i)
        PyType::from(oldBases->at(i))->removeSubclass(globalObject, type);
    for (unsigned i = 0; i < bases->length(); ++i)
        PyType::from(bases->at(i))->addSubclass(globalObject, type);
}

static JSValue typeGetDict(JSGlobalObject* globalObject, JSValue self)
{
    return makeMappingProxy(globalObject, PyType::from(self)->dict());
}

static JSValue typeGetDoc(JSGlobalObject* globalObject, JSValue self)
{
    PyType* type = PyType::from(self);
    JSValue doc = type->dict()->get(runtime(globalObject).names.dDoc());
    if (!doc)
        return jsUndefined();
    return bindDescriptor(globalObject, doc, { }, type);
}

static void typeSetDoc(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__doc__"_s))
        return;
    Runtime& rt = runtime(globalObject);
    type->dict()->set(rt.vm(), rt.names.dDoc(), value);
    type->didChange(rt.vm());
}

static JSValue typeGetAbstractMethods(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = PyType::from(self);
    // type itself has the descriptor, not the attribute.
    if (type != rt.typeType()) {
        if (JSValue methods = type->dict()->get(rt.names.dAbstractMethods()))
            return methods;
    }
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, rt.AttributeError(), "__abstractmethods__"_s);
}

static void typeSetAbstractMethods(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    bool isAbstract = false;
    if (value) {
        isAbstract = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        type->dict()->set(vm, rt.names.dAbstractMethods(), value);
    } else if (!type->dict()->remove(vm, rt.names.dAbstractMethods())) {
        raise(globalObject, scope, rt.AttributeError(), "__abstractmethods__"_s);
        return;
    }
    if (isAbstract)
        type->add(TypeFlag::Abstract);
    else
        type->remove(TypeFlag::Abstract);
    type->didChange(vm);
}

static JSValue raiseNoTypeAttribute(JSGlobalObject* globalObject, PyType* type, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString("type object '"_s, type->nameInMessages(), "' has no attribute '"_s, name, '\''));
}

static JSValue typeGetAnnotate(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = PyType::from(self);
    if (!type->isHeapType())
        return raiseNoTypeAttribute(globalObject, type, "__annotate__"_s);
    JSValue annotate = type->dict()->get(rt.names.dAnnotateFunc());
    if (!annotate)
        annotate = type->dict()->get(rt.names.dAnnotate());
    if (annotate)
        return bindDescriptor(globalObject, annotate, { }, type);
    type->dict()->set(vm, rt.names.dAnnotateFunc(), jsUndefined());
    type->didChange(vm);
    return jsUndefined();
}

static void typeSetAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete __annotate__ attribute"_s);
        return;
    }
    if (!type->isHeapType()) {
        raiseTypeError(globalObject, scope, "cannot set '__annotate__' attribute of immutable type '"_s, type->nameInMessages(), '\'');
        return;
    }
    if (!isNone(value) && !isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "__annotate__ must be callable or None"_s);
        return;
    }
    type->dict()->set(vm, rt.names.dAnnotateFunc(), value);
    if (!isNone(value))
        type->dict()->remove(vm, rt.names.dAnnotationsCache());
    type->didChange(vm);
}

static JSValue typeGetAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!type->isHeapType())
        return raiseNoTypeAttribute(globalObject, type, "__annotations__"_s);

    JSValue annotations = type->dict()->get(rt.names.dAnnotations());
    if (!annotations)
        annotations = type->dict()->get(rt.names.dAnnotationsCache());
    if (annotations)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, annotations, { }, type));

    JSValue annotate = typeGetAnnotate(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isCallable(globalObject, annotate)) {
        annotations = call(globalObject, annotate, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        if (!PyDict::is(annotations))
            return raiseTypeError(globalObject, scope, "__annotate__() must return a dict, not "_s, typeName(globalObject, annotations));
    } else
        annotations = PyDict::create(globalObject);
    type->dict()->set(vm, rt.names.dAnnotationsCache(), annotations);
    type->didChange(vm);
    return annotations;
}

static void typeSetAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (!type->isHeapType()) {
        raiseTypeError(globalObject, scope, "cannot set '__annotations__' attribute of immutable type '"_s, type->nameInMessages(), '\'');
        return;
    }
    PyDict* dict = type->dict();
    JSString* key = dict->get(rt.names.dAnnotations()) ? rt.names.dAnnotations() : rt.names.dAnnotationsCache();
    if (value)
        dict->set(vm, key, value);
    else if (!dict->remove(vm, key)) {
        raise(globalObject, scope, rt.AttributeError(), "__annotations__"_s);
        return;
    }
    dict->remove(vm, rt.names.dAnnotateFunc());
    dict->remove(vm, rt.names.dAnnotate());
}

static JSValue typeGetTypeParams(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = PyType::from(self);
    if (type != rt.typeType()) {
        if (JSValue params = type->dict()->get(rt.names.dTypeParams()))
            return params;
    }
    return rt.emptyTuple();
}

static void typeSetTypeParams(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyType* type = PyType::from(self);
    if (!checkMutable(globalObject, type, value, "__type_params__"_s))
        return;
    Runtime& rt = runtime(globalObject);
    type->dict()->set(rt.vm(), rt.names.dTypeParams(), value);
    type->didChange(rt.vm());
}

// How the built-in types are called, for inspect.signature(). The types that are not here have no signature in CPython either.
static constexpr std::pair<ASCIILiteral, ASCIILiteral> typeSignatures[] = {
    { "FrameLocalsProxy"_s, "($frame)"_s },
    { "NoneType"_s, "()"_s },
    { "NotImplementedType"_s, "()"_s },
    { "_blake2.blake2b"_s, "(data=b'', *, digest_size=_blake2.blake2b.MAX_DIGEST_SIZE,\n        key=b'', salt=b'', person=b'', fanout=1, depth=1, leaf_size=0,\n        node_offset=0, node_depth=0, inner_size=0, last_node=False,\n        usedforsecurity=True, string=None)"_s },
    { "_blake2.blake2s"_s, "(data=b'', *, digest_size=_blake2.blake2s.MAX_DIGEST_SIZE,\n        key=b'', salt=b'', person=b'', fanout=1, depth=1, leaf_size=0,\n        node_offset=0, node_depth=0, inner_size=0, last_node=False,\n        usedforsecurity=True, string=None)"_s },
    { "_hashlib.HASH"_s, "(name, string=b'')"_s },
    { "_hashlib.HASHXOF"_s, "(name, string=b'')"_s },
    { "_io.BufferedRWPair"_s, "(reader, writer, buffer_size=DEFAULT_BUFFER_SIZE, /)"_s },
    { "_io.BufferedRandom"_s, "(raw, buffer_size=DEFAULT_BUFFER_SIZE)"_s },
    { "_io.BufferedReader"_s, "(raw, buffer_size=DEFAULT_BUFFER_SIZE)"_s },
    { "_io.BufferedWriter"_s, "(raw, buffer_size=DEFAULT_BUFFER_SIZE)"_s },
    { "_io.BytesIO"_s, "(initial_bytes=b'')"_s },
    { "_io.FileIO"_s, "(file, mode='r', closefd=True, opener=None)"_s },
    { "_io.IncrementalNewlineDecoder"_s, "(decoder, translate, errors='strict')"_s },
    { "_io.StringIO"_s, "(initial_value='', newline='\\n')"_s },
    { "_io.TextIOWrapper"_s, "(buffer, encoding=None, errors=None, newline=None,\n              line_buffering=False, write_through=False)"_s },
    { "_pickle.Pickler"_s, "(file, protocol=None, fix_imports=True, buffer_callback=None)"_s },
    { "_pickle.Unpickler"_s, "(file, *, fix_imports=True, encoding='ASCII', errors='strict',\n          buffers=())"_s },
    { "_queue.SimpleQueue"_s, "()"_s },
    { "_struct.Struct"_s, "(format)"_s },
    { "_thread._local"_s, "()"_s },
    { "_thread.lock"_s, "()"_s },
    { "bool"_s, "(object=False, /)"_s },
    { "cell"_s, "([contents])"_s },
    { "classmethod"_s, "(function, /)"_s },
    { "code"_s, "(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize,\n     flags, codestring, constants, names, varnames, filename, name,\n     qualname, firstlineno, linetable, exceptiontable, freevars=(),\n     cellvars=(), /)"_s },
    { "collections.deque"_s, "([iterable[, maxlen]])"_s },
    { "complex"_s, "(real=0, imag=0)"_s },
    { "compression.zstd.ZstdCompressor"_s, "(level=None, options=None, zstd_dict=None)"_s },
    { "compression.zstd.ZstdDecompressor"_s, "(zstd_dict=None, options=None)"_s },
    { "compression.zstd.ZstdDict"_s, "(dict_content, /, *, is_raw=False)"_s },
    { "ellipsis"_s, "()"_s },
    { "enumerate"_s, "(iterable, start=0)"_s },
    { "filter"_s, "(function, iterable, /)"_s },
    { "float"_s, "(x=0, /)"_s },
    { "frozenset"_s, "(iterable=(), /)"_s },
    { "function"_s, "(code, globals, name=None, argdefs=None, closure=None,\n         kwdefaults=None)"_s },
    { "functools.partial"_s, "(func, /, *args, **keywords)"_s },
    { "itertools._tee"_s, "(iterable, /)"_s },
    { "itertools.accumulate"_s, "(iterable, func=None, *, initial=None)"_s },
    { "itertools.batched"_s, "(iterable, n, *, strict=False)"_s },
    { "itertools.chain"_s, "(*iterables)"_s },
    { "itertools.combinations"_s, "(iterable, r)"_s },
    { "itertools.combinations_with_replacement"_s, "(iterable, r)"_s },
    { "itertools.compress"_s, "(data, selectors)"_s },
    { "itertools.count"_s, "(start=0, step=1)"_s },
    { "itertools.cycle"_s, "(iterable, /)"_s },
    { "itertools.dropwhile"_s, "(predicate, iterable, /)"_s },
    { "itertools.filterfalse"_s, "(function, iterable, /)"_s },
    { "itertools.groupby"_s, "(iterable, key=None)"_s },
    { "itertools.pairwise"_s, "(iterable, /)"_s },
    { "itertools.permutations"_s, "(iterable, r=None)"_s },
    { "itertools.product"_s, "(*iterables, repeat=1)"_s },
    { "itertools.starmap"_s, "(function, iterable, /)"_s },
    { "itertools.takewhile"_s, "(predicate, iterable, /)"_s },
    { "itertools.zip_longest"_s, "(*iterables, fillvalue=None)"_s },
    { "lazy_import"_s, "(builtins, name, fromlist=None, /)"_s },
    { "list"_s, "(iterable=(), /)"_s },
    { "map"_s, "(function, iterable, /, *iterables, strict=False)"_s },
    { "mappingproxy"_s, "(mapping)"_s },
    { "memoryview"_s, "(object)"_s },
    { "method"_s, "(function, instance, /)"_s },
    { "module"_s, "(name, doc=None)"_s },
    { "object"_s, "()"_s },
    { "operator.attrgetter"_s, "(attr, /, *attrs)"_s },
    { "operator.itemgetter"_s, "(item, /, *items)"_s },
    { "operator.methodcaller"_s, "(name, /, *args, **kwargs)"_s },
    { "posix.sched_param"_s, "(sched_priority)"_s },
    { "property"_s, "(fget=None, fset=None, fdel=None, doc=None)"_s },
    { "reversed"_s, "(object, /)"_s },
    { "sentinel"_s, "(name, /, *, repr=None)"_s },
    { "set"_s, "(iterable=(), /)"_s },
    { "staticmethod"_s, "(function, /)"_s },
    { "traceback"_s, "(tb_next, tb_frame, tb_lasti, tb_lineno)"_s },
    { "tuple"_s, "(iterable=(), /)"_s },
    { "types.GenericAlias"_s, "(origin, args, /)"_s },
    { "types.SimpleNamespace"_s, "(mapping_or_iterable=(), /, **kwargs)"_s },
    { "zip"_s, "(*iterables, strict=False)"_s },
    { "zlib._ZlibDecompressor"_s, "(wbits=MAX_WBITS, zdict=b'')"_s },
};

static JSValue typeGetTextSignature(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = PyType::from(self);
    if (!type->isHeapType()) {
        String name = type->nameInMessages();
        for (auto& [typeName, signature] : typeSignatures) {
            if (name == typeName)
                return makeStr(rt.vm(), signature);
        }
        return jsUndefined();
    }
    // A class says it in its docstring: "Name(parameters)\n--\n\n".
    JSValue doc = type->dict()->get(rt.names.dDoc());
    if (!doc || !doc.isString() || asString(doc)->isRope())
        return jsUndefined();
    String docText = stringOf(asString(doc));
    StringView text = docText;
    String name = type->name();
    if (!text.startsWith(name) || text.length() <= name.length() || text[name.length()] != '(')
        return jsUndefined();
    size_t end = text.find(")\n--\n\n"_s, name.length());
    // The parameters may take several lines, but no empty one.
    if (end == notFound || text.left(end).find("\n\n"_s) != notFound)
        return jsUndefined();
    return makeStr(rt.vm(), text.substring(name.length(), end + 1 - name.length()).toString());
}

// In the numbering of CPython, which programs test bits of.
static JSValue typeGetFlags(JSGlobalObject* globalObject, JSValue self)
{
    PyType* type = PyType::from(self);
    static constexpr std::pair<TypeFlag, unsigned> bits[] = {
        { TypeFlag::HasDict, 4 }, { TypeFlag::Sequence, 5 }, { TypeFlag::Mapping, 6 }, { TypeFlag::DisallowInstantiation, 7 },
        { TypeFlag::Heap, 9 }, { TypeFlag::Base, 10 }, { TypeFlag::Abstract, 20 }, { TypeFlag::MatchSelf, 22 },
        { TypeFlag::IntSubclass, 24 }, { TypeFlag::ListSubclass, 25 }, { TypeFlag::TupleSubclass, 26 }, { TypeFlag::BytesSubclass, 27 },
        { TypeFlag::StrSubclass, 28 }, { TypeFlag::DictSubclass, 29 }, { TypeFlag::ExceptionSubclass, 30 }, { TypeFlag::TypeSubclass, 31 },
    };
    uint64_t flags = 1 << 12; // Ready.
    for (auto [flag, bit] : bits) {
        if (type->has(flag))
            flags |= static_cast<uint64_t>(1) << bit;
    }
    if (type->has(TypeFlag::Mutable))
        flags |= 1 << 9;
    else if (!type->isHeapType())
        flags |= 1 << 8; // Immutable.
    // Its instances may refer to other objects. (They are all the collector's here.)
    Runtime& rt = runtime(globalObject);
    bool isAtomic = false;
    for (PyType* atomic : { rt.objectType(), rt.intType(), rt.boolType(), rt.floatType(), rt.complexType(), rt.strType(), rt.bytesType(), rt.bytearrayType(), rt.noneType(), rt.rangeType(), rt.typeOf(rt.notImplemented()), rt.typeOf(rt.ellipsis()) })
        isAtomic |= type == atomic;
    if (!isAtomic)
        flags |= 1 << 14;
    return makeInt(globalObject, flags);
}

static JSValue typeGetBasicSize(JSGlobalObject*, JSValue) { return jsNumber(16); }
static JSValue typeGetItemSize(JSGlobalObject*, JSValue) { return jsNumber(0); }
static JSValue typeGetDictOffset(JSGlobalObject*, JSValue self) { return jsNumber(PyType::from(self)->has(TypeFlag::HasDict) ? -1 : 0); }
static JSValue typeGetWeakrefOffset(JSGlobalObject*, JSValue self) { return jsNumber(PyType::from(self)->has(TypeFlag::HasWeakref) ? -1 : 0); }

static constexpr NativeDef typeMethods[] = {
    PY_NEW(typeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", typeInit, "($self, *args, **kwargs)"),
    PY_SLOT("__call__", typeCallMethod, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", typeRepr, "($self, /)"),
    PY_SLOT("__getattribute__", typeGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", typeSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", typeDelAttr, "($self, name, /)"),
    PY_SLOT("__or__", typeOr, "($self, value, /)"),
    PY_SLOT("__ror__", typeROr, "($self, value, /)"),
    PY_METHOD("mro", typeMRO, "($self, /)"),
    PY_METHOD("__subclasses__", typeSubclasses, "($self, /)"),
    PY_CLASSMETHOD("__prepare__", typePrepare, "($cls, *args, **kwargs)"),
    PY_METHOD("__instancecheck__", typeInstanceCheck, "($self, instance, /)"),
    PY_METHOD("__subclasscheck__", typeSubclassCheck, "($self, subclass, /)"),
    PY_METHOD("__dir__", typeDir, "($self, /)"),
};

static constexpr NativeGetSet typeGetSets[] = {
    { "__name__"_s, typeGetName, typeSetName },
    { "__qualname__"_s, typeGetQualname, typeSetQualname },
    { "__bases__"_s, typeGetBases, typeSetBases },
    { "__mro__"_s, typeGetMRO },
    PY_MEMBER("__base__", typeGetBase),
    { "__module__"_s, typeGetModule, typeSetModule },
    { "__abstractmethods__"_s, typeGetAbstractMethods, typeSetAbstractMethods },
    { "__dict__"_s, typeGetDict },
    { "__doc__"_s, typeGetDoc, typeSetDoc },
    { "__text_signature__"_s, typeGetTextSignature },
    { "__annotations__"_s, typeGetAnnotations, typeSetAnnotations },
    { "__annotate__"_s, typeGetAnnotate, typeSetAnnotate },
    { "__type_params__"_s, typeGetTypeParams, typeSetTypeParams },
    PY_MEMBER("__flags__", typeGetFlags),
    PY_MEMBER("__basicsize__", typeGetBasicSize),
    PY_MEMBER("__itemsize__", typeGetItemSize),
    PY_MEMBER("__dictoffset__", typeGetDictOffset),
    PY_MEMBER("__weakrefoffset__", typeGetWeakrefOffset),
};

void initializeObjectTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.objectType()->initializeBuiltin(globalObject, { "object"_s, { TypeFlag::Base }, objectMethods, objectGetSets, "The base class of the class hierarchy.\n\nWhen called, it accepts no arguments and returns a new featureless\ninstance that has no instance attributes and cannot be given any.\n"_s }, rt.typeType(), nullptr);
    rt.typeType()->initializeBuiltin(globalObject, { "type"_s, { TypeFlag::Base, TypeFlag::TypeSubclass, TypeFlag::OwnLayout, TypeFlag::HasDict, TypeFlag::HasWeakref }, typeMethods, typeGetSets, "type(object) -> the object's type\ntype(name, bases, dict, **kwds) -> a new type"_s }, rt.typeType(), rt.objectType());
}

// ---- __build_class__ ----

// Replaces each base that is not a class by what its __mro_entries__ says.
static PyTuple* updateBases(JSGlobalObject* globalObject, PyTuple* bases)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* replaced = nullptr;
    for (unsigned i = 0; i < bases->length(); ++i) {
        JSValue base = bases->at(i);
        JSValue method;
        if (!PyType::is(base)) {
            method = getAttrIfPresent(globalObject, base, rt.name(Special::MroEntries));
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (!method) {
            if (replaced)
                listAppend(globalObject, replaced, base);
            continue;
        }
        JSValue entries = call(globalObject, method, bases);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!PyTuple::is(entries)) {
            raiseTypeError(globalObject, scope, "__mro_entries__ must return a tuple"_s);
            return nullptr;
        }
        if (!replaced) {
            replaced = newList(globalObject);
            for (unsigned j = 0; j < i; ++j)
                listAppend(globalObject, replaced, bases->at(j));
        }
        for (JSValue entry : PyTuple::from(entries)->span())
            listAppend(globalObject, replaced, entry);
    }
    return replaced ? tupleFromList(globalObject, replaced) : bases;
}

JSValue buildClass(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (args.size() < 2)
        return raiseTypeError(globalObject, scope, "__build_class__: not enough arguments"_s);
    PyFunction* function = PyFunction::tryFrom(args[0]);
    if (!function)
        return raiseTypeError(globalObject, scope, "__build_class__: func must be a function"_s);
    JSValue name = args[1];
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "__build_class__: name is not a string"_s);
    PyTuple* originalBases = PyTuple::create(globalObject, args.positional().subspan(2));
    PyTuple* bases = updateBases(globalObject, originalBases);
    RETURN_IF_EXCEPTION(scope, { });

    // The keywords, but for `metaclass`.
    JSValue meta;
    Vector<JSValue, 8> callValues { name, bases, JSValue() };
    Vector<JSValue, 4> keywordNames;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        if (WTF::equal(args.keywordName(i)->tryGetValueImpl(), rt.names.metaclass()->tryGetValueImpl())) {
            meta = args.keywordValue(i);
            continue;
        }
        keywordNames.append(args.keywordName(i));
        callValues.append(args.keywordValue(i));
    }
    PyTuple* names = nullptr;
    if (!keywordNames.isEmpty()) {
        names = PyTuple::createUninitialized(vm, rt.keywordNamesStructure(), rt.tupleType(), keywordNames.size());
        for (unsigned i = 0; i < keywordNames.size(); ++i)
            names->initializeAt(vm, i, keywordNames[i]);
    }

    bool isClass = true;
    if (meta)
        isClass = PyType::is(meta);
    else
        meta = bases->length() ? rt.typeOf(bases->at(0)) : rt.typeType();
    if (isClass) {
        meta = calculateMetaclass(globalObject, PyType::from(meta), bases);
        RETURN_IF_EXCEPTION(scope, { });
    }

    JSValue ns;
    JSValue prepare = getAttrIfPresent(globalObject, meta, rt.name(Special::Prepare));
    RETURN_IF_EXCEPTION(scope, { });
    if (!prepare)
        ns = PyDict::create(globalObject);
    else {
        // The values of the keywords follow the two positional arguments.
        Vector<JSValue, 8> prepareValues { name, bases };
        prepareValues.append(callValues.subspan(3));
        ns = call(globalObject, prepare, CallArgs(prepareValues.span().data(), 2, names));
        RETURN_IF_EXCEPTION(scope, { });
        if (!PyDict::is(ns) && !rt.typeOf(ns)->lookup(Special::GetItem))
            return raiseTypeError(globalObject, scope, isClass ? PyType::from(meta)->name() : "<metaclass>"_str, ".__prepare__() must return a mapping, not "_s, typeName(globalObject, ns));
    }

    JSValue cell = call(globalObject, function, ns);
    RETURN_IF_EXCEPTION(scope, { });
    if (bases != originalBases) {
        setItem(globalObject, ns, rt.names.dOrigBases(), originalBases);
        RETURN_IF_EXCEPTION(scope, { });
    }
    callValues[2] = ns;
    JSValue cls = call(globalObject, meta, CallArgs(callValues.span().data(), 3, names));
    RETURN_IF_EXCEPTION(scope, { });

    if (PyType::is(cls) && PyCell::is(cell)) {
        JSValue cellValue = PyCell::from(cell)->value();
        if (cellValue != cls) {
            if (!cellValue)
                return raiseRuntimeError(globalObject, scope, "__class__ not set defining "_s, reprForMessage(globalObject, name), " as "_s, reprForMessage(globalObject, cls), ". Was __classcell__ propagated to type.__new__?"_s);
            return raiseTypeError(globalObject, scope, "__class__ set to "_s, reprForMessage(globalObject, cellValue), " defining "_s, reprForMessage(globalObject, name), " as "_s, reprForMessage(globalObject, cls));
        }
    }
    return cls;
}

// ---- None, NotImplemented, Ellipsis ----

static JSValue noneNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasExcessArguments(args))
        return raiseTypeError(globalObject, scope, "NoneType takes no arguments"_s);
    return jsUndefined();
}

static JSValue noneRepr(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "None"_s); }
static JSValue noneBool(JSGlobalObject*, JSValue) { return jsBoolean(false); }
static JSValue noneHash(JSGlobalObject*, JSValue) { return jsNumber(static_cast<double>(0xFCA86420)); }

static constexpr NativeDef noneMethods[] = {
    PY_NEW(noneNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", noneRepr, "($self, /)"),
    PY_SLOT("__bool__", noneBool, "($self, /)"),
    PY_SLOT("__hash__", noneHash, "($self, /)"),
};

static JSValue notImplementedNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasExcessArguments(args))
        return raiseTypeError(globalObject, scope, "NotImplementedType takes no arguments"_s);
    return runtime(globalObject).notImplemented();
}

static JSValue notImplementedRepr(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "NotImplemented"_s); }

static JSValue notImplementedBool(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "NotImplemented should not be used in a boolean context"_s);
}

static constexpr NativeDef notImplementedMethods[] = {
    PY_NEW(notImplementedNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", notImplementedRepr, "($self, /)"),
    PY_SLOT("__bool__", notImplementedBool, "($self, /)"),
    PY_METHOD("__reduce__", notImplementedRepr, "($self, /)"),
};

static JSValue ellipsisNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasExcessArguments(args))
        return raiseTypeError(globalObject, scope, "EllipsisType takes no arguments"_s);
    return runtime(globalObject).ellipsis();
}

static JSValue ellipsisRepr(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "Ellipsis"_s); }

static constexpr NativeDef ellipsisMethods[] = {
    PY_NEW(ellipsisNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", ellipsisRepr, "($self, /)"),
    PY_METHOD("__reduce__", ellipsisRepr, "($self, /)"),
};

void initializeSingletonTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.noneType()->initializeBuiltin(globalObject, { "NoneType"_s, { }, noneMethods, { }, "The type of the None singleton."_s }, rt.typeType(), rt.objectType());
    rt.notImplementedType()->initializeBuiltin(globalObject, { "NotImplementedType"_s, { }, notImplementedMethods, { }, "The type of the NotImplemented singleton."_s }, rt.typeType(), rt.objectType());
    rt.ellipsisType()->initializeBuiltin(globalObject, { "ellipsis"_s, { }, ellipsisMethods, { }, "The type of the Ellipsis singleton."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
