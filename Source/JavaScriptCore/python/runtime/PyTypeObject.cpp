/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"
#include "PyJSClass.h"

#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
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

JSString* attributeNameSlow(JSGlobalObject* globalObject, JSValue name)
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
    if (type != base && !type->isMadeAs(base))
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
    return makeInt(globalObject, identityBits(value));
}

String addressForRepr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue identity = identityOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeString("0x"_s, intToString(globalObject, identity, 16)));
}

static JSValue typeModule(JSGlobalObject* globalObject, PyType* type)
{
    if (!type->isHeapType() && !type->has(TypeFlag::Mutable))
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
    return native && native->isVariadicWithSelf(function);
}

// __itemsize__: not 0 for a type whose instances are of variable size in CPython, on which some rules for classes depend.
static unsigned itemSize(PyType* type)
{
    if (type->has(TypeFlag::IntSubclass))
        return 4;
    if (type->has(TypeFlag::TupleSubclass))
        return 8;
    if (type->has(TypeFlag::BytesSubclass))
        return 1;
    if (type->has(TypeFlag::TypeSubclass))
        return 40;
    return 0;
}

// The built-in type that decides what kind of cell the instances are.
static PyType* layoutBase(PyType* type)
{
    PyType* base = type->solidBase();
    while (base->isHeapType())
        base = base->base()->solidBase();
    return base;
}

JSValue allocateInstance(JSGlobalObject* globalObject, PyType* type, PyType* layout)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (type == rt.objectType())
        return PyObject::create(vm, rt.structure<PyObject>(), type);
    ASSERT_UNUSED(layout, layoutBase(type) == (layout ? layout : rt.objectType()));
    return PyInstance::create(vm, type);
}

void didMakeObject(JSGlobalObject* globalObject, PyType* type, JSValue object)
{
    // What is of another class was not made for this call, as far as anybody can tell: str(x) gives what x.__str__() returned.
    if (type->lookup(Special::Del) && object.isCell() && typeOf(globalObject, object) == type) [[unlikely]]
        runtime(globalObject).registerFinalizable(object.asCell());
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
        if (!type->isMadeBy(objectNew))
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
    RELEASE_AND_RETURN(scope, allocateInstance(globalObject, type));
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
    if (type->isMadeBy(objectNew))
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

int64_t identityHash(JSGlobalObject*, JSValue self)
{
    // It goes by what id() goes by, as in CPython. (But for a str, which has a hash of its own: this is asked for one that is the self of a bound method.)
    uint64_t bits = identityBits(self);
    int64_t hash = static_cast<int64_t>((bits >> 4) | (bits << 60));
    return hash == -1 ? -2 : hash;
}

JSValue objectHash(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, identityHash(globalObject, self)); }

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
    JSValue result = callOperatorMethod(globalObject, method, self, other);
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!string)
        return { };
    RELEASE_AND_RETURN(scope, genericGetAttrAsGiven(globalObject, self, string, name));
}

// object.__setattr__(int, ...) would get around what type.__setattr__ forbids. Classes are all that CPython refuses (tried with 25 kinds of object): for a weak proxy, a threading.local, a
// decimal.Context, a partial, which have such methods of their own too, this is the search that any object has.
static bool checkIsNoClass(JSGlobalObject* globalObject, JSValue self, ASCIILiteral what)
{
    if (!PyType::tryFrom(self)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "can't apply this "_s, what, " to "_s, typeOf(globalObject, self)->nameInMessages(), " object"_s);
    return false;
}

JSValue objectSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkIsNoClass(globalObject, self, "__setattr__"_s)))
        return { };
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    genericSetAttrAsGiven(globalObject, self, string, name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

JSValue objectDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkIsNoClass(globalObject, self, "__delattr__"_s)))
        return { };
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    genericSetAttrAsGiven(globalObject, self, string, name, { });
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
    RELEASE_AND_RETURN(scope, strObject(globalObject, self));
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
    EXCEPTION_ASSERT(!!scope.exception() == (count < 0));
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
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSArray* result = newList(globalObject, dict->size());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue key, value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        listAppend(globalObject, result, key);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue instanceGetDict(JSGlobalObject*, JSValue self);

// Whether to read the attribute of an instance of the type is to call this getter of a built-in type, and nothing else. (It is a data
// descriptor: nothing that the object has comes before it.)
static bool readsThroughGetter(PyType* type, JSString* name, JSValue (*getter)(JSGlobalObject*, JSValue))
{
    if (!isNative(type->lookup(Special::GetAttribute), objectGetAttribute))
        return false;
    auto* descriptor = PyGetSetDescriptor::tryFrom(type->lookup(name));
    if (!descriptor || descriptor->def().getter != getter)
        return false;
    // It applies to the instances. (object, whose getters these mostly are, is the last in line unless the mro() of a metaclass has left it out.)
    PyType* owner = descriptor->owner();
    PyTuple* mro = type->mro();
    return (mro && mro->length() && mro->at(mro->length() - 1).asCell() == owner) || appliesToInstancesOf(owner, type);
}

// Whether instance.__dict__ would move the attributes to a dict now (PyInstance::ensureDict()), after which no instruction remembers
// where they are. Who only reads them has forEachAttribute().
static bool dictWouldBeMadeOfAttributes(Runtime& rt, PyInstance* instance)
{
    return !instance->dict() && readsThroughGetter(instance->type(), rt.names.dDict(), instanceGetDict);
}

static JSValue objectDir(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* names;
    if (auto* instance = PyInstance::tryFrom(self); instance && dictWouldBeMadeOfAttributes(rt, instance)) {
        names = PyDict::create(globalObject);
        instance->forEachAttribute(globalObject, [&](JSString* name, JSValue value) {
            names->set(vm, name, value);
            return true;
        });
    } else {
        JSValue dict = getAttrIfPresent(globalObject, self, rt.names.dDict());
        RETURN_IF_EXCEPTION(scope, { });
        if (PyDict* own = PyDict::tryFrom(dict))
            names = own->copy(globalObject);
        else
            names = PyDict::create(globalObject);
    }
    JSValue cls = getAttrIfPresent(globalObject, self, rt.names.dClass());
    RETURN_IF_EXCEPTION(scope, { });
    if (cls) {
        mergeClassDict(globalObject, names, cls);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, keysAsList(globalObject, names));
}

static JSValue objectGetClass(JSGlobalObject* globalObject, JSValue self)
{
    return typeOf(globalObject, self);
}

bool instancesMayClaimAnotherClass(Runtime& rt, PyType* type)
{
    return !readsThroughGetter(type, rt.names.dClass(), objectGetClass);
}

// Objects register for finalization when they are made, if their class has a __del__ then. This is for a class that gets one
// later, by assignment to the attribute or to __bases__: it finds the instances there are, of it and of what derives from it.
// (Among them those that nothing refers to any more but no collection has found yet. An object dies when a collection finds it.)
static void registerInstancesForFinalization(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    MarkedArgumentBuffer instances;
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return IterationStatus::Continue;
            auto* cell = static_cast<JSCell*>(heapCell);
            if (!cell->structureID() || !cell->isObject() || cell->structure()->realm() != globalObject)
                return IterationStatus::Continue;
            PyType* cellType = nullptr;
            if (isPyObject(cell))
                cellType = uncheckedDowncast<PyObject>(cell)->type();
            else if (PyListObject::is(cell))
                cellType = uncheckedDowncast<PyListObject>(cell)->type();
            else if (PyType::is(cell))
                cellType = uncheckedDowncast<PyType>(cell)->type();
            // One that is being made has no type yet, and a type that is being made no MRO.
            if (cellType && cellType->isHeapType() && cellType->mro() && cellType->isSubtypeOf(type))
                instances.append(cell);
            return IterationStatus::Continue;
        });
    }
    rt.registerFinalizableLate(instances);
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
    // The classes that last added something to the layout: the same one, or two that added the same to the same base.
    PyType* a = oldType->solidBase();
    PyType* b = newType->solidBase();
    return a == b || (a->base() == b->base() && a->isHeapType() && b->isHeapType() && sameSlots(a, b));
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
    if (isAudited(globalObject) && failed(scope, audit(globalObject, "object.__setattr__"_s, self, makeStr(vm, "__class__"_s), value)))
        return;
    PyType* oldType = rt.typeOf(self);
    bool areModules = oldType->isSubtypeOf(rt.moduleType()) && newType->isSubtypeOf(rt.moduleType());
    // (What an object of JS's is an instance of, its prototype says.)
    if (!areModules && (!oldType->isHeapType() || !newType->isHeapType() || oldType->has(TypeFlag::AttributesAreProperties))) {
        raiseTypeError(globalObject, scope, "__class__ assignment only supported for mutable types or ModuleType subclasses"_s);
        return;
    }
    // (A class without an order of its bases is still being made: this is the mro() of its metaclass.)
    if (!newType->mro() || !layoutsAreCompatible(oldType, newType)) {
        raiseTypeError(globalObject, scope, "__class__ assignment: '"_s, newType->nameInMessages(), "' object layout differs from '"_s, oldType->nameInMessages(), '\'');
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
    if (newType->lookup(Special::Del) && !oldType->lookup(Special::Del)) [[unlikely]] {
        MarkedArgumentBuffer objects;
        objects.append(cell);
        rt.registerFinalizableLate(objects);
    }
    // See mayBecomeAnotherKindOfDescriptor().
    if (oldType->hasInstancesAmongAttributes()) [[unlikely]]
        oldType->didChange(vm);
}

static constexpr NativeDef defaultComparisonMethods[] = {
    PY_SLOT("__eq__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__ne__", objectNe, "($self, value, /)"),
    PY_SLOT("__lt__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__le__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__gt__", objectNotComparable, "($self, value, /)"),
    PY_SLOT("__ge__", objectNotComparable, "($self, value, /)"),
};

std::span<const NativeDef> defaultComparisons() { return defaultComparisonMethods; }

static constexpr NativeDef repeatableObjectMethods[] = {
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", objectSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", objectDelAttr, "($self, name, /)"),
    PY_HASH(identityHash),
    PY_SLOT("__eq__", objectEq, "($self, value, /)"),
};

std::span<const NativeDef> repeatableSlotsOfObject() { return repeatableObjectMethods; }

static constexpr NativeDef objectMethods[] = {
    PY_NEW(objectNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", objectInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", objectRepr, "($self, /)"),
    PY_SLOT("__str__", objectStr, "($self, /)"),
    PY_HASH(identityHash),
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
    PY_CLASSMETHOD("__subclasshook__", objectSubclassHook, "!($type, object, /)"),
    PY_CLASSMETHOD("__init_subclass__", objectInitSubclass, "!($type, /)"),
};

static constexpr NativeGetSet objectGetSets[] = {
    { "__class__"_s, objectGetClass, objectSetClass },
};

// ---- The __dict__ and __weakref__ of instances ----

JSValue builtinInstanceGetDict(JSGlobalObject* globalObject, JSValue self)
{
    if (PyDict* dict = instanceDict(globalObject, self, true))
        return dict;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "This object has no __dict__"_s);
}

// class Meta(Plain, type): the __dict__ that is found first for an instance of Meta, a class, is that of Plain, which is for
// ordinary instances. What the __dict__ of a class (a module, a function) is, the descriptor of the built-in type knows.
// Empty and no exception: there is no such type among the bases.
static JSValue dictDescriptorOfBuiltinBase(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(self);
    while (type->base() && (type->isHeapType() || !type->has(TypeFlag::HasDict)))
        type = type->base();
    if (!type->base())
        return { };
    JSValue descriptor = type->lookup(rt.names.dDict());
    if (descriptor && rt.typeOf(descriptor)->lookup(Special::Set))
        return descriptor;
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    return raiseTypeError(globalObject, scope, "this __dict__ descriptor does not support '"_s, typeName(globalObject, self), "' objects"_s);
}

static JSValue instanceGetDict(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue descriptor = dictDescriptorOfBuiltinBase(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (descriptor) [[unlikely]]
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, descriptor, self, typeOf(globalObject, self)));
    RELEASE_AND_RETURN(scope, builtinInstanceGetDict(globalObject, self));
}

static void setDictOfInstance(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // class Both(Plain, js.Map) has this from Plain. As for any object of JS's: no other mapping can take the place of the properties.
    if (typeOf(globalObject, self)->has(TypeFlag::AttributesAreProperties)) [[unlikely]] {
        raiseAttributeError(globalObject, scope, self, runtime(globalObject).names.dDict(), makeString("attribute '__dict__' of '"_s, typeName(globalObject, self), "' objects is not writable"_s));
        return;
    }
    if (value && !isDict(value)) {
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

static void instanceSetDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSValue descriptor = dictDescriptorOfBuiltinBase(globalObject, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (!descriptor) [[likely]]
        RELEASE_AND_RETURN(scope, setDictOfInstance(globalObject, self, value));
    if (value)
        RELEASE_AND_RETURN(scope, void(callSpecial(globalObject, rt.typeOf(descriptor)->lookup(Special::Set), descriptor, self, value)));
    JSValue deleter = rt.typeOf(descriptor)->lookup(Special::Delete);
    if (!deleter) {
        raise(globalObject, scope, rt.AttributeError(), "__delete__"_s);
        return;
    }
    RELEASE_AND_RETURN(scope, void(callSpecial(globalObject, deleter, descriptor, self)));
}

// That of an instance of a built-in type can be replaced, but not deleted.
static void builtinInstanceSetDict(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (value)
        return setDictOfInstance(globalObject, self, value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "cannot delete __dict__"_s);
}

static constexpr NativeGetSet instanceDictGetSet[] = { { "__dict__"_s, instanceGetDict, instanceSetDict, false, "dictionary for instance variables"_s } };
static constexpr NativeGetSet builtinInstanceDictGetSet[] = { { "__dict__"_s, builtinInstanceGetDict, builtinInstanceSetDict } };
static constexpr NativeGetSet instanceWeakrefGetSet[] = { { "__weakref__"_s, instanceGetWeakref, nullptr, false, "list of weak references to the object"_s } };

std::span<const NativeGetSet> dictGetSet() { return builtinInstanceDictGetSet; }
bool isDictDescriptorOfInstances(JSValue descriptor)
{
    auto* getSet = descriptor.isCell() ? dynamicDowncast<PyGetSetDescriptor>(descriptor.asCell()) : nullptr;
    return getSet && (getSet->def().getter == instanceGetDict || getSet->def().getter == builtinInstanceGetDict);
}
std::span<const NativeGetSet> weakrefGetSet() { return instanceWeakrefGetSet; }

// ---- type: attributes ----

// `nameIsKey`: as for genericGetAttr().
template<bool nameIsKey>
static ALWAYS_INLINE JSValue typeGetAttrImpl(JSGlobalObject* globalObject, PyType* type, JSString* name, JSValue key, bool suppress)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyType* metatype = type->type();
    JSValue metaAttribute = nameIsKey ? metatype->lookupKey(globalObject, key) : metatype->lookup(name);
    if constexpr (nameIsKey)
        RETURN_IF_EXCEPTION(scope, { });
    JSValue metaGetter;
    if (metaAttribute) {
        PyType* attributeType = rt.typeOf(metaAttribute);
        metaGetter = attributeType->lookup(Special::Get);
        if (metaGetter && (attributeType->lookup(Special::Set) || attributeType->lookup(Special::Delete)))
            RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, metaAttribute, type, metatype));
    }
    JSValue attribute = nameIsKey ? type->lookupKey(globalObject, key) : type->lookup(name);
    if constexpr (nameIsKey)
        RETURN_IF_EXCEPTION(scope, { });
    if (attribute)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, attribute, { }, type));
    if (metaGetter)
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, metaAttribute, type, metatype));
    if (metaAttribute)
        return metaAttribute;
    if (type->isSubtypeOf(rt.jsObjectType())) [[unlikely]] {
        JSValue inherited = staticMemberOfClassOfJS(globalObject, type, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (inherited)
            return inherited;
    }
    if (suppress)
        return { };
    // Which attribute of which object it was is for whoever asked to say: getAttr().
    return raise(globalObject, scope, rt.AttributeError(), makeString("type object '"_s, type->nameInMessages(), "' has no attribute '"_s, stringOf(name), '\''));
}

JSValue typeGetAttr(JSGlobalObject* globalObject, PyType* type, JSString* name, bool suppress)
{
    return typeGetAttrImpl<false>(globalObject, type, name, { }, suppress);
}

JSValue typeGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!string)
        return { };
    if (!name.isString()) [[unlikely]]
        RELEASE_AND_RETURN(scope, typeGetAttrImpl<true>(globalObject, PyType::from(self), string, name, false));
    RELEASE_AND_RETURN(scope, typeGetAttr(globalObject, PyType::from(self), string));
}

static void typeSetAttrImpl(JSGlobalObject* globalObject, PyType* type, JSString* name, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (type->isImmutable()) {
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
    bool getsFinalizer = value && isIdentical(name, rt.name(Special::Del)) && !type->lookup(Special::Del);
    // (As any key is stored and removed: type("X", (), { key: 1 }) may have a key that says it is equal to the name, and that is where a lookup finds it.)
    if (value) {
        type->dict()->set(globalObject, name, value);
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        bool removed = type->dict()->remove(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (!removed) {
            raiseAttributeError(globalObject, scope, type, name, makeString("type object '"_s, type->nameInMessages(), "' has no attribute '"_s, stringOf(name), '\''));
            return;
        }
    }
    StringImpl* impl = name->tryGetValueImpl();
    bool isHook = impl == rt.name(Special::GetAttr)->tryGetValueImpl() || impl == rt.name(Special::GetAttribute)->tryGetValueImpl();
    type->didChange(vm, isHook ? PyType::Change::HowAttributesAreRead : PyType::Change::Any);
    if (impl == rt.name(Special::New)->tryGetValueImpl()) [[unlikely]]
        type->updateMakers();
    if (getsFinalizer) [[unlikely]]
        registerInstancesForFinalization(globalObject, type);
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

// found(cls, ...), for the __new__ that was found in `type` or behind it.
static ALWAYS_INLINE JSValue callNew(JSGlobalObject* globalObject, JSValue found, PyType* type, PyType* cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSValue function;
    if (auto* wrapper = dynamicDowncast<PyWrapperObject>(found); wrapper && wrapper->type() == rt.staticmethodType())
        function = wrapper->wrapped();
    else {
        function = bindDescriptor(globalObject, found, { }, type);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, withFirstArgument(cls, args, [&](const CallArgs& all) {
        return call(globalObject, function, all);
    }));
}

// Whether getattr(cls, '__new__') is what is found in cls, for the classes of this metaclass that have one.
static ALWAYS_INLINE bool givesNewOfClassAsFound(Runtime& rt, PyType* metatype)
{
    if (metatype == rt.typeType()) [[likely]]
        return true;
    if (!isNative(metatype->lookup(Special::GetAttribute), typeGetAttribute) || metatype->lookup(Special::GetAttr))
        return false;
    // The __new__ of the metaclass itself comes first if it is a data descriptor.
    JSValue ofMetatype = metatype->lookup(Special::New);
    if (!ofMetatype)
        return true;
    PyType* kind = rt.typeOf(ofMetatype);
    return !kind->lookup(Special::Set) && !kind->lookup(Special::Delete);
}

// The first half: tp_new(type, ...) in CPython.
static ALWAYS_INLINE JSValue makeInstanceOf(JSGlobalObject* globalObject, PyType* type, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyNativeFunction* maker = type->maker()) {
        JSValue object = maker->invokeWithSelf(globalObject, type, args);
        RETURN_IF_EXCEPTION(scope, { });
        didMakeObject(globalObject, type, object);
        return object;
    }
    if (!type->looksUpNew())
        return raiseTypeError(globalObject, scope, "cannot create '"_s, type->nameInMessages(), "' instances"_s);
    // type.__new__, as an attribute of the class.
    JSValue found = givesNewOfClassAsFound(rt, type->type()) ? type->lookup(Special::New) : JSValue();
    if (found) [[likely]]
        RELEASE_AND_RETURN(scope, callNew(globalObject, found, type, type, args));
    JSValue function = getAttr(globalObject, type, rt.name(Special::New));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, withFirstArgument(type, args, [&](const CallArgs& all) {
        return call(globalObject, function, all);
    }));
}

static NEVER_INLINE PyType::Construction findConstruction(Runtime& rt, PyType* type)
{
    auto takesAllArguments = [&](PyNativeFunction* native, NativeKind kind) {
        return native && native->kind() == kind && native->def().signature.isVariadic && native->def().signature.hasSelf && !native->def().signature.isMadeAtRunTime && !(native->def().flags & (OnWipedStack | MayWait));
    };
    auto construction = PyType::Construction::General;
    uint64_t timesLookupRanCode = PyType::timesLookupRanCode();
    auto* init = PyNativeFunction::tryFrom(type->lookup(Special::Init));
    if (type->isImmutable() && !type->isHeapType() && type != rt.typeType() && !type->lookup(Special::Del) && takesAllArguments(type->maker(), NativeKind::New) && !type->isMadeBy(objectNew)) {
        // object.__init__ has nothing to say about the arguments of a type with a __new__ of its own.
        if (init && init->isVariadicWithSelf(objectInit))
            construction = PyType::Construction::NativeNew;
        // (The C++ function is called as it is, so it must be the type's own or inherited, and still be found when this is.)
        else if (takesAllArguments(init, NativeKind::SlotWrapper) && type->isSubtypeOf(init->ownerType()) && type->remembers(Special::Init))
            construction = PyType::Construction::NativeNewAndInit;
    }
    // A key that is no str was asked, and may have changed the type.
    if (timesLookupRanCode != PyType::timesLookupRanCode()) [[unlikely]]
        return PyType::Construction::General;
    type->setConstruction(construction);
    return construction;
}

static JSValue typeCallWithoutNotes(JSGlobalObject*, PyType*, const CallArgs&, PyType* derived, bool afterType);

// list(islice(g(), 3)), enumerate(g()): see "Generators" in PyTemporaries.cpp.
static NEVER_INLINE JSValue typeCallWithNewGenerator(JSGlobalObject* globalObject, PyType* type, const CallArgs& args, PyType* derived, bool afterType)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!derived && type->has(TypeFlag::GoesThroughArgument)) {
        GeneratorArgumentScope argument(globalObject, args.size() ? args[0] : JSValue());
        RELEASE_AND_RETURN(scope, typeCallWithoutNotes(globalObject, type, args, derived, afterType));
    }
    if (derived || !type->has(TypeFlag::HasIteratorsToItself))
        RELEASE_AND_RETURN(scope, typeCallWithoutNotes(globalObject, type, args, derived, afterType));
    Runtime::CallOfInstruction call = rt.callInProgress();
    JSValue iterator = typeCallWithoutNotes(globalObject, type, args, derived, afterType);
    RETURN_IF_EXCEPTION(scope, { });
    rt.iteratorWasMade(call, rt.typeOf(iterator) == type ? iterator.asCell() : nullptr);
    return iterator;
}

JSValue typeCall(JSGlobalObject* globalObject, PyType* type, const CallArgs& args, PyType* derived, bool afterType)
{
    if (runtime(globalObject).hasNewGenerator()) [[unlikely]]
        return typeCallWithNewGenerator(globalObject, type, args, derived, afterType);
    return typeCallWithoutNotes(globalObject, type, args, derived, afterType);
}

static JSValue typeCallWithoutNotes(JSGlobalObject* globalObject, PyType* type, const CallArgs& allArgs, PyType* derived, bool afterType)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallArgs args = allArgs;
    PyType* cls = derived ? derived : type;
    // For a derived class: what the type has, or what follows it, in the MRO of that class.
    auto lookup = [&](Special special) -> JSValue {
        PyTuple* mro = derived ? derived->mro() : nullptr;
        if (!mro)
            return type->lookup(special);
        unsigned i = 0;
        while (i < mro->length() && mro->at(i).asCell() != type)
            ++i;
        for (i += afterType ? 1 : 0; i < mro->length(); ++i) {
            if (JSValue found = PyType::from(mro->at(i))->dict()->get(rt.name(special)))
                return found;
        }
        return { };
    };

    if (type == rt.typeType() && !derived) {
        if (args.size() == 1 && !args.hasKeywords())
            return rt.typeOf(args[0]);
        if (args.size() == 1)
            return raiseTypeError(globalObject, scope, "type() takes no keyword arguments"_s);
        if (args.size() != 3)
            return raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s);
    }

    // The second half, for what __new__ made.
    auto initialize = [&](JSValue object) -> JSValue {
        // It may be something else altogether, which is then left alone.
        PyType* objectType = rt.typeOf(object);
        if (!objectType->isSubtypeOf(cls))
            return object;
        // A class written in JS that has an __init__ besides its constructor calls those of its bases from there.
        if (afterType && !constructorInitializes(type))
            return object;
        JSValue init = derived ? lookup(Special::Init) : objectType->lookup(Special::Init);
        if (!init || (derived && isNativeNew(init, objectInit)))
            return object;
        JSValue result = callSpecial(globalObject, init, object, allArgs);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(result))
            return raiseTypeError(globalObject, scope, "__init__() should return None, not '"_s, typeName(globalObject, result), '\'');
        return object;
    };

    if (auto construction = type->construction() == PyType::Construction::Unknown ? findConstruction(rt, type) : type->construction();
        construction != PyType::Construction::General && !derived && vm.isSafeToRecurseSoft()) {
        NativeDef::VariadicWithSelf make = type->maker()->def().pointer.variadicWithSelf;
        if (construction == PyType::Construction::NativeNew) {
            JSValue object = make(globalObject, type, args);
            RETURN_IF_EXCEPTION(scope, { });
            if (rt.typeOf(object) != type) [[unlikely]]
                RELEASE_AND_RETURN(scope, initialize(object));
            return object;
        }
        // (Now: __new__ may run code of the program's, after which nothing may be remembered of the type. Its __init__ is still this one: it cannot be written to.)
        NativeDef::VariadicWithSelf init = uncheckedDowncast<PyNativeFunction>(type->lookup(Special::Init).asCell())->def().pointer.variadicWithSelf;
        JSValue object = make(globalObject, type, args);
        RETURN_IF_EXCEPTION(scope, { });
        // What OSError(2, "") makes is of a class derived from it.
        if (rt.typeOf(object) != type) [[unlikely]]
            RELEASE_AND_RETURN(scope, initialize(object));
        init(globalObject, object, args);
        RETURN_IF_EXCEPTION(scope, { });
        return object;
    }

    if (!derived) {
        JSValue object = makeInstanceOf(globalObject, type, args);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, initialize(object));
    }

    // The constructor of a class of JS has called super(...): what the type, or what follows it, has for a __new__ makes an instance of that class.
    JSValue found = lookup(Special::New);
    if (!found || (type->has(TypeFlag::DisallowInstantiation) && !afterType))
        return raiseTypeError(globalObject, scope, "cannot create '"_s, type->nameInMessages(), "' instances"_s);
    JSValue object;
    if (auto* native = PyNativeFunction::tryFrom(found); native && native->kind() == NativeKind::New) {
        // As where a class is made, the __new__ of a built-in type, whichever, stands for what has come along __base__: class V(ValueError, OSError) finds
        // ValueError's and is made by OSError's. After super(...) that is what has come to the class of the instance: that may be class P(Mixin, dict),
        // where Mixin is the class of JS. The classes that look __new__ up and find a constructor of JS are passed over: they are made as if it had
        // none. Not so one that finds anything else: "look it up" is then what has come to the class.
        PyType* inherited = afterType ? derived : nullptr;
        while (inherited && inherited->looksUpNew())
            inherited = isNewOfConstructor(inherited->lookup(Special::New)) ? inherited->base() : nullptr;
        bool isInherited = inherited;
        if (!isInherited) {
            // If the type looks it up, here it is. (The other case in which it is the owner is for the message.)
            PyType* owner = native->ownerType();
            inherited = !type->maker() || (type->isSubtypeOf(owner) && type->isMadeAs(owner)) ? owner : type;
        }
        // object.__new__ and object.__init__ tell by the class of the instance whether the arguments are meant for the other of the two.
        // Here that is for the type to say, whose methods these are.
        if (inherited->isMadeBy(objectNew)) {
            if (hasExcessArguments(args) && isNativeNew(lookup(Special::Init), objectInit) && !hasInitOfItsOwn(derived))
                return raiseTypeError(globalObject, scope, derived->nameInMessages(), "() takes no arguments"_s);
            args = CallArgs();
        }
        object = PyNativeFunction::makeInstance(globalObject, isInherited ? nullptr : inherited, derived, args);
    } else
        object = callNew(globalObject, found, type, derived, args);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, initialize(object));
}

static JSValue typeCallMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    // In CPython only a call of type itself has words of its own for this.
    if (self == JSValue(runtime(globalObject).typeType()) && args.size() == 1 && args.hasKeywords()) {
        auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
        return raiseTypeError(globalObject, scope, "type() takes 1 or 3 arguments"_s);
    }
    return typeCall(globalObject, PyType::from(self), args);
}

JSValue prepareConstruct(JSGlobalObject* globalObject, JSValue callee, JSValue& self)
{
    PyType* type = PyType::tryFrom(callee);
    if (!type || type->has(TypeFlag::Abstract))
        return { };
    JSValue init = type->lookup(Special::Init);
    if (!PyFunction::is(init) || !type->isMadeBy(objectNew) || !isNativeNew(type->type()->lookup(Special::Call), typeCallMethod))
        return { };
    self = allocateInstance(globalObject, type);
    didMakeObject(globalObject, type, self);
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

enum class MROResult : uint8_t {
    Raised,
    Superseded, // While mro() ran, the type got an MRO from somebody who knew its bases as they are now. That one stays.
    Set,
};

// Sets the MRO of the type, by the mro() of its metaclass.
static MROResult resolveMRO(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyTuple* before = type->mro();
    PyTuple* mro;
    if (type->type() == rt.typeType()) {
        mro = computeMRO(globalObject, type);
        RETURN_IF_EXCEPTION(scope, MROResult::Raised);
    } else {
        // Looked up as a special method is: the __getattribute__ of the metaclass is not asked.
        JSValue method = type->type()->lookup(rt.names.mro());
        if (!method) {
            raise(globalObject, scope, rt.AttributeError(), "mro"_s);
            return MROResult::Raised;
        }
        JSValue result = callSpecial(globalObject, method, type);
        RETURN_IF_EXCEPTION(scope, MROResult::Raised);
        mro = tupleFromIterable(globalObject, result);
        RETURN_IF_EXCEPTION(scope, MROResult::Raised);
        if (!mro->length()) {
            raiseTypeError(globalObject, scope, "type MRO must not be empty"_s);
            return MROResult::Raised;
        }
        PyType* solid = type->solidBase();
        for (unsigned i = 0; i < mro->length(); ++i) {
            PyType* base = PyType::tryFrom(mro->at(i));
            if (!base) {
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".mro() returned a non-class ('"_s, typeNameForMessage(globalObject, rt.typeOf(mro->at(i))), "')"_s);
                return MROResult::Raised;
            }
            if (!solid->isSubtypeOf(base->solidBase())) {
                raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".mro() returned base with unsuitable layout ('"_s, typeNameForMessage(globalObject, base), "')"_s);
                return MROResult::Raised;
            }
        }
    }
    if (type->mro() != before)
        return MROResult::Superseded;
    type->setMRO(vm, mro);
    type->didChange(vm, PyType::Change::HowAttributesAreRead);
    // An mro() may name a class that the type does not derive from. What is remembered of the type's attributes depends on that
    // class all the same.
    if (type->type() != rt.typeType()) {
        PyTuple* bases = type->bases();
        for (unsigned i = 0; i < mro->length(); ++i) {
            PyType* entry = PyType::from(mro->at(i));
            bool isAncestor = entry == type;
            for (unsigned j = 0; !isAncestor && j < bases->length(); ++j)
                isAncestor = PyType::from(bases->at(j))->isSubtypeOf(entry);
            if (!isAncestor) {
                entry->addDependent(globalObject, type);
                RETURN_IF_EXCEPTION(scope, MROResult::Raised);
            }
        }
    }
    return MROResult::Set;
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (StringImpl* impl = string->tryGetValueImpl(); impl && impl->is8Bit()) [[likely]]
        return true;
    RELEASE_AND_RETURN(scope, !!codecEncodeUTF8(globalObject, string, "strict"_s));
}

static JSValue typeNewWithKeywordsAsGiven(JSGlobalObject*, JSValue cls, const CallArgs&);

static JSValue typeNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    if (!args.hasKeywords() || cls != JSValue(rt.typeType()) || !keywordKeys(globalObject, args.keywordNames())) [[likely]]
        return typeNewWithKeywordsAsGiven(globalObject, cls, args);
    // type(name, bases, dict, **m), or a class statement that comes to that. In CPython a call of type itself puts the keywords into a dict on the way here, so names that are no plain
    // strs are asked for their hashes once more.
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    PyDict* keywords = PyDict::create(globalObject);
    addKeywordsTo(globalObject, keywords, args);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* names = keywordNamesOf(globalObject, keywords);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer values;
    for (JSValue value : args.positional())
        values.append(value);
    JSValue key, value;
    for (unsigned position = 0; keywords->next(position, key, value);)
        values.append(value);
    RELEASE_AND_RETURN(scope, typeNewWithKeywordsAsGiven(globalObject, cls, CallArgs(valuesOf(values), args.size(), names)));
}

static JSValue typeNewWithKeywordsAsGiven(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
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
        return raiseTypeError(globalObject, scope, "type.__new__() argument 2 must be tuple, not "_s, typeNameOfArgument(globalObject, args[1]));
    if (!PyDict::is(args[2]))
        return raiseTypeError(globalObject, scope, "type.__new__() argument 3 must be dict or frozendict, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, args[2])));
    JSString* name = internString(globalObject, strValue(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* bases = PyTuple::from(args[1]);
    String nameText = stringOf(name);
    if (nameText.contains('\0'))
        return raiseValueError(globalObject, scope, "type name must not contain null characters"_s);
    if (failed(scope, checkEncodable(globalObject, name)))
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
            if (!winner->isMadeBy(typeNew))
                RELEASE_AND_RETURN(scope, makeInstanceOf(globalObject, winner, args));
            metatype = winner;
        }
        base = findBestBase(globalObject, bases);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // In the order in which the namespace iterates, if it is of a class with an order of its own.
    PyDict* dict = PyDict::create(globalObject);
    dictUpdateFromMapping(globalObject, dict, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSValue doc = dict->get(rt.names.dDoc()); doc && doc.isString() && failed(scope, checkEncodable(globalObject, asString(doc))))
        return { };

    // ---- __slots__ ----

    bool mayAddDict = !base->has(TypeFlag::HasDict) && !base->has(TypeFlag::AttributesAreProperties);
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
        // Not for what is of variable size in CPython, unless __slots__ asks for it.
        addWeakref = mayAddWeakref && !itemSize(base);
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
    flags.remove({ TypeFlag::Sequence, TypeFlag::Mapping }); // These go by the MRO: see below.
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
        } else {
            // No code of Python's is running: it is JS that makes the class, as with `class extends`.
            dict->set(vm, rt.names.dModule(), internString(vm, "js"_s));
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
    if (JSValue function = dict->get(rt.name(Special::New)); function && (PyFunction::is(function) || isFunctionOfJS(rt, function))) {
        JSValue method = makeStaticMethod(globalObject, function);
        RETURN_IF_EXCEPTION(scope, { });
        dict->set(vm, rt.name(Special::New), method);
    }
    for (Special special : { Special::InitSubclass, Special::ClassGetItem }) {
        if (JSValue function = dict->get(rt.name(special)); function && (PyFunction::is(function) || isFunctionOfJS(rt, function))) {
            JSValue method = makeClassMethod(globalObject, function);
            RETURN_IF_EXCEPTION(scope, { });
            dict->set(vm, rt.name(special), method);
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

    // After those: __slots__ may name them.
    if (slots) {
        for (unsigned i = 0; i < slots->length(); ++i) {
            JSString* slot = asString(slots->at(i));
            dict->set(vm, slot, PyMemberDescriptor::create(globalObject, slot, type));
        }
    }

    // What defines equality without saying how to hash cannot be hashed.
    if (dict->get(rt.name(Special::Eq)) && !dict->get(rt.name(Special::Hash)))
        dict->set(vm, rt.name(Special::Hash), jsUndefined());

    resolveMRO(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    type->takeMakerOfBase(vm);
    type->updateMaker();

    // Nor can what has inherited no way to hash: the mro() of a metaclass has left out object, and every class that has object in its own.
    if (PyTuple* mro = type->mro(); (!mro->length() || mro->at(mro->length() - 1) != JSValue(rt.objectType())) && !type->lookup(Special::Hash) && !dict->get(rt.name(Special::Hash))) [[unlikely]] {
        bool inherits = false;
        for (unsigned i = 1; i < mro->length() && !inherits; ++i)
            inherits = !!PyType::from(mro->at(i))->lookup(Special::Hash);
        if (!inherits) {
            dict->set(vm, rt.name(Special::Hash), jsUndefined());
            type->didChange(vm);
        }
    }

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
    for (unsigned i = 0; i < bases->length(); ++i) {
        PyType::from(bases->at(i))->addSubclass(globalObject, type);
        RETURN_IF_EXCEPTION(scope, { });
    }

    if (!dict->hasOnlyStringKeys()) [[unlikely]] {
        bool allAreStrs = true;
        JSValue key, value;
        for (unsigned position = 0; allAreStrs && dict->next(position, key, value);)
            allAreStrs = isStr(key);
        if (!allAreStrs && failed(scope, warn(globalObject, rt.RuntimeWarning(), makeString("non-string key in the __dict__ of class "_s, nameText))))
            return { };
    }

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
                if (!exception) // A termination.
                    return { };
                scope.release();
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
    RELEASE_AND_RETURN(scope, newList(globalObject, mro->span()));
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
    RELEASE_AND_RETURN(scope, keysAsList(globalObject, names));
}

static JSValue typeOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, self, other); }
static JSValue typeROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, other, self); }

static bool checkMutable(JSGlobalObject* globalObject, PyType* type, JSValue value, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "cannot delete '"_s, name, "' attribute of type '"_s, type->nameInMessages(), '\'');
        return false;
    }
    if (type->isImmutable()) {
        raiseTypeError(globalObject, scope, "cannot set '"_s, name, "' attribute of immutable type '"_s, type->nameInMessages(), '\'');
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
    if (failed(scope, checkMutable(globalObject, type, value, "__name__"_s)))
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
    if (failed(scope, checkEncodable(globalObject, name)))
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
    if (failed(scope, checkMutable(globalObject, type, value, "__qualname__"_s)))
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (failed(scope, checkMutable(globalObject, type, value, "__module__"_s)))
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

// Works out the MRO of the type again, and of all that derive from it. Appends to `changes`, for each type that got a new
// one: the type, the new MRO, the old one. False: an exception.
static bool updateMROs(JSGlobalObject* globalObject, PyType* type, MarkedArgumentBuffer& changes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* oldMRO = type->mro();
    MROResult result = resolveMRO(globalObject, type);
    EXCEPTION_ASSERT_UNUSED(scope, !!scope.exception() == (result == MROResult::Raised));
    if (result != MROResult::Set)
        return result == MROResult::Superseded;
    changes.append(type);
    changes.append(type->mro());
    changes.append(oldMRO ? JSValue(oldMRO) : jsUndefined());
    // A copy: an mro() may change who derives from whom.
    MarkedArgumentBuffer subclasses;
    type->appendSubclassesTo(subclasses);
    for (unsigned i = 0; i < subclasses.size(); ++i) {
        if (failed(scope, updateMROs(globalObject, PyType::from(subclasses.at(i)), changes)))
            return false;
    }
    return true;
}

// After the base of the type has changed: which class decides the layout, for it and for all that derive from it.
static void updateSolidBases(VM& vm, PyType* type)
{
    if (!type->slots() && !type->has(TypeFlag::OwnLayout))
        type->setSolidBase(vm, type->base()->solidBase());
    MarkedArgumentBuffer subclasses;
    type->appendSubclassesTo(subclasses);
    for (unsigned i = 0; i < subclasses.size(); ++i)
        updateSolidBases(vm, PyType::from(subclasses.at(i)));
}

static void typeSetBases(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (failed(scope, checkMutable(globalObject, type, value, "__bases__"_s)))
        return;
    // Its prototype, which inherits from that of the class of JS's and says what its instances are, is made once: see PyPrototypeView.
    if (type->has(TypeFlag::AttributesAreProperties)) {
        raiseTypeError(globalObject, scope, "__bases__ assignment: the bases of '"_s, type->nameInMessages(), "', a class of objects of JavaScript, cannot be changed"_s);
        return;
    }
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
        // The MRO of the base may be out of date, if this is called by the mro() of a metaclass that another assignment to
        // __bases__ is calling. Its base is not.
        bool derives = base->isSubtypeOf(type);
        for (PyType* ancestor = base; !derives && ancestor; ancestor = ancestor->base())
            derives = ancestor == type;
        if (derives) {
            raiseTypeError(globalObject, scope, "a __bases__ item causes an inheritance cycle"_s);
            return;
        }
    }
    PyType* base = findBestBase(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, void());
    if (!layoutsAreCompatible(type->base(), base, type)) {
        raiseTypeError(globalObject, scope, "__bases__ assignment: '"_s, base->nameInMessages(), "' object layout differs from '"_s, type->base()->nameInMessages(), '\'');
        return;
    }

    PyTuple* oldBases = type->bases();
    PyType* oldBase = type->base();
    bool hadFinalizer = !!type->lookup(Special::Del);
    type->setBases(vm, bases);
    type->setBase(vm, base);
    updateSolidBases(vm, type);
    MarkedArgumentBuffer changes;
    bool succeeded = updateMROs(globalObject, type, changes);
    // An mro() that ran may itself have assigned to __bases__. Then what it assigned stays, and has been seen to.
    bool isCurrent = type->bases() == bases;
    if (failed(scope, succeeded)) {
        for (unsigned i = changes.size(); i; i -= 3) {
            PyType* changed = PyType::from(changes.at(i - 3));
            if (changed->mro() != changes.at(i - 2).asCell())
                continue;
            changed->setMRO(vm, PyTuple::tryFrom(changes.at(i - 1)));
            changed->didChange(vm);
        }
        if (isCurrent) {
            type->setBases(vm, oldBases);
            type->setBase(vm, oldBase);
            updateSolidBases(vm, type);
            type->didChange(vm);
        }
        return;
    }
    if (!isCurrent)
        return;
    for (unsigned i = 0; i < oldBases->length(); ++i) {
        PyType::from(oldBases->at(i))->removeSubclass(globalObject, type);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < bases->length(); ++i) {
        PyType::from(bases->at(i))->addSubclass(globalObject, type);
        RETURN_IF_EXCEPTION(scope, void());
    }
    type->didChange(vm, PyType::Change::HowAttributesAreRead);
    type->updateMakers();
    if (!hadFinalizer && type->lookup(Special::Del)) [[unlikely]]
        registerInstancesForFinalization(globalObject, type);
}

static JSValue typeGetDict(JSGlobalObject* globalObject, JSValue self)
{
    return makeMappingProxy(globalObject, PyType::from(self)->dict());
}


static JSValue typeGetDoc(JSGlobalObject* globalObject, JSValue self)
{
    PyType* type = PyType::from(self);
    // What its own namespace has under the name is this very descriptor.
    if (type == runtime(globalObject).typeType())
        return docstringFor(globalObject, "type type"_s);
    JSValue doc = type->dict()->get(runtime(globalObject).names.dDoc());
    if (!doc)
        return jsUndefined();
    // So it is for property, whose instances have a __doc__ each.
    if (!type->isHeapType() && !isStr(doc) && !isNone(doc)) {
        if (JSValue text = docstringFor(globalObject, makeString("type "_s, type->nameInMessages())); !isNone(text))
            return text;
    }
    return bindDescriptor(globalObject, doc, { }, type);
}

static void typeSetDoc(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (failed(scope, checkMutable(globalObject, type, value, "__doc__"_s)))
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
    // What the program wrote itself comes before what the compiler made.
    JSValue annotate = type->dict()->get(rt.names.dAnnotate());
    if (!annotate)
        annotate = type->dict()->get(rt.names.dAnnotateFunc());
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
    if (!type->isHeapType() || type->has(TypeFlag::Immutable)) {
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
        RELEASE_AND_RETURN(scope, raiseNoTypeAttribute(globalObject, type, "__annotations__"_s));

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
    if (!type->isHeapType() || type->has(TypeFlag::Immutable)) {
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
    type->didChange(vm);
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(self);
    if (failed(scope, checkMutable(globalObject, type, value, "__type_params__"_s)))
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
    { "_asyncio.Future"_s, "(*, loop=None)"_s },
    { "_asyncio.Task"_s, "(coro, *, loop=None, name=None, context=None, eager_start=False)"_s },
    { "_blake2.blake2b"_s, "(data=b'', *, digest_size=_blake2.blake2b.MAX_DIGEST_SIZE,\n        key=b'', salt=b'', person=b'', fanout=1, depth=1, leaf_size=0,\n        node_offset=0, node_depth=0, inner_size=0, last_node=False,\n        usedforsecurity=True, string=None)"_s },
    { "_blake2.blake2s"_s, "(data=b'', *, digest_size=_blake2.blake2s.MAX_DIGEST_SIZE,\n        key=b'', salt=b'', person=b'', fanout=1, depth=1, leaf_size=0,\n        node_offset=0, node_depth=0, inner_size=0, last_node=False,\n        usedforsecurity=True, string=None)"_s },
    { "_bz2.BZ2Compressor"_s, "(compresslevel=9, /)"_s },
    { "_bz2.BZ2Decompressor"_s, "()"_s },
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
    { "_lzma.LZMADecompressor"_s, "(format=FORMAT_AUTO, memlimit=None, filters=None)"_s },
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
    { "datetime.date"_s, "(year, month, day)"_s },
    { "datetime.datetime"_s, "(year, month, day, hour=0, minute=0, second=0, microsecond=0,\n         tzinfo=None, *, fold=0)"_s },
    { "datetime.time"_s, "(hour=0, minute=0, second=0, microsecond=0, tzinfo=None, *, fold=0)"_s },
    { "datetime.timedelta"_s, "(days=0, seconds=0, microseconds=0, milliseconds=0, minutes=0,\n          hours=0, weeks=0)"_s },
    { "datetime.timezone"_s, "(offset, name=<unrepresentable>)"_s },
    { "decimal.Context"_s, "(prec=None, rounding=None, Emin=None, Emax=None, capitals=None,\n        clamp=None, flags=None, traps=None)"_s },
    { "decimal.Decimal"_s, "(value='0', context=None)"_s },
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

bool isMadeAtRunTimeInCPython(PyType* type)
{
    if (type->isHeapType())
        return false;
    JSValue module = type->dict()->get(Runtime::from(type->realm()).names.dModule());
    return module && module.isString();
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
    if (type->isImmutable())
        flags |= 1 << 8;
    if (!type->isHeapType()) {
        if (isMadeAtRunTimeInCPython(type))
            flags |= 1 << 9;
    }
    // Its instances may refer to other objects. (They are all the collector's here.)
    Runtime& rt = runtime(globalObject);
    bool isAtomic = false;
    for (PyType* atomic : { rt.objectType(), rt.intType(), rt.boolType(), rt.floatType(), rt.complexType(), rt.strType(), rt.bytesType(), rt.bytearrayType(), rt.noneType(), rt.rangeType(), rt.typeOf(rt.notImplemented()), rt.typeOf(rt.ellipsis()) })
        isAtomic |= type == atomic;
    if (!isAtomic)
        flags |= 1 << 14;
    return makeInt(globalObject, flags);
}

// As in CPython, but for the table of the names of the instances' attributes, which grows with them there.
static JSValue typeSizeOf(JSGlobalObject*, JSValue self)
{
    PyType* type = PyType::from(self);
    if (!type->isHeapType() && !type->has(TypeFlag::Mutable))
        return jsNumber(424);
    return jsNumber(type->has(TypeFlag::HasDict) && !itemSize(type) ? 1712 : 944);
}

// The sizes are CPython's, for what is asked about most. They say nothing about the cells here.
static JSValue typeGetBasicSize(JSGlobalObject*, JSValue self)
{
    static constexpr std::pair<ASCIILiteral, unsigned> sizes[] = {
        { "int"_s, 24 }, { "float"_s, 24 }, { "complex"_s, 32 }, { "str"_s, 64 }, { "bytes"_s, 33 }, { "bytearray"_s, 64 }, { "tuple"_s, 32 },
        { "list"_s, 40 }, { "dict"_s, 48 }, { "set"_s, 200 }, { "frozenset"_s, 200 }, { "type"_s, 944 }, { "BaseException"_s, 72 },
    };
    unsigned slotCount = 0;
    for (PyType* type = PyType::from(self); type; type = type->base()) {
        if (type->isHeapType()) {
            if (PyTuple* slots = type->slots())
                slotCount += slots->length();
            continue;
        }
        for (auto [name, size] : sizes) {
            if (type->nameInMessages() == name)
                return jsNumber(size + 8 * slotCount);
        }
    }
    return jsNumber(16 + 8 * slotCount);
}
static JSValue typeGetItemSize(JSGlobalObject*, JSValue self) { return jsNumber(itemSize(PyType::from(self))); }
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
    PY_CLASSMETHOD("__prepare__", typePrepare, "!($cls, name, bases, /, **kwds)"),
    PY_METHOD("__instancecheck__", typeInstanceCheck, "($self, instance, /)"),
    PY_METHOD("__subclasscheck__", typeSubclassCheck, "($self, subclass, /)"),
    PY_METHOD("__dir__", typeDir, "($self, /)"),
    PY_METHOD("__sizeof__", typeSizeOf, "($self, /)"),
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
            if (replaced) {
                listAppend(globalObject, replaced, base);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
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
            for (unsigned j = 0; j < i; ++j) {
                listAppend(globalObject, replaced, bases->at(j));
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
        for (JSValue entry : PyTuple::from(entries)->span()) {
            listAppend(globalObject, replaced, entry);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    RELEASE_AND_RETURN(scope, replaced ? tupleFromList(globalObject, replaced) : bases);
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
    PyTuple* bases = PyTuple::create(globalObject, args.positional().subspan(2));
    RELEASE_AND_RETURN(scope, buildClass(globalObject, name, bases, args, [&](JSValue ns) {
        return call(globalObject, function, ns);
    }));
}

JSValue buildClass(JSGlobalObject* globalObject, JSValue name, PyTuple* originalBases, const CallArgs& args, const ScopedLambda<JSValue(JSValue)>& body)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* bases = updateBases(globalObject, originalBases);
    RETURN_IF_EXCEPTION(scope, { });

    // The keywords, but for `metaclass`.
    JSValue meta;
    Vector<JSValue, 8> callValues { name, bases, JSValue() };
    Vector<JSValue, 4> keywordNames;
    PyTuple* names = nullptr;
    if (args.keywordCount() && keywordKeys(globalObject, args.keywordNames())) [[unlikely]] {
        // Some are not plain strs. CPython puts the keywords into a dict and takes 'metaclass' out of that.
        PyDict* keywords = PyDict::create(globalObject);
        addKeywordsTo(globalObject, keywords, args);
        RETURN_IF_EXCEPTION(scope, { });
        meta = keywords->take(globalObject, rt.names.metaclass());
        RETURN_IF_EXCEPTION(scope, { });
        if (!keywords->isEmpty()) {
            names = keywordNamesOf(globalObject, keywords);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue key, value;
            for (unsigned position = 0; keywords->next(position, key, value);)
                callValues.append(value);
        }
    } else {
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            if (WTF::equal(args.keywordName(i)->tryGetValueImpl(), rt.names.metaclass()->tryGetValueImpl())) {
                meta = args.keywordValue(i);
                continue;
            }
            keywordNames.append(args.keywordName(i));
            callValues.append(args.keywordValue(i));
        }
    }
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
        if (!PyDict::is(ns) && !isSubscriptedByKeys(globalObject, ns))
            return raiseTypeError(globalObject, scope, isClass ? PyType::from(meta)->name() : "<metaclass>"_str, ".__prepare__() must return a mapping, not "_s, typeName(globalObject, ns));
    }

    JSValue cell = body(ns);
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
static int64_t noneHash(JSGlobalObject*, JSValue) { return 0xFCA86420; }

static constexpr NativeDef noneMethods[] = {
    PY_NEW(noneNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", noneRepr, "($self, /)"),
    PY_SLOT("__bool__", noneBool, "($self, /)"),
    PY_HASH(noneHash),
};

static JSValue notImplementedNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasExcessArguments(args))
        return raiseTypeError(globalObject, scope, "NotImplementedType takes no arguments"_s);
    RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
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
    RELEASE_AND_RETURN(scope, runtime(globalObject).ellipsis());
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
    rt.noneType()->initializeBuiltin(globalObject, { "NoneType"_s, { TypeFlag::NothingOfItsOwn }, noneMethods, { }, "The type of the None singleton."_s }, rt.typeType(), rt.objectType());
    rt.notImplementedType()->initializeBuiltin(globalObject, { "NotImplementedType"_s, { TypeFlag::NothingOfItsOwn }, notImplementedMethods, { }, "The type of the NotImplemented singleton."_s }, rt.typeType(), rt.objectType());
    rt.ellipsisType()->initializeBuiltin(globalObject, { "ellipsis"_s, { TypeFlag::NothingOfItsOwn }, ellipsisMethods, { }, "The type of the Ellipsis singleton."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
