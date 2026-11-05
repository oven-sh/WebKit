/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyBytecode.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyNative.h"
#include "PyRuntime.h"
#include "PyTuple.h"
#include "PyType.h"

namespace JSC {
namespace Python {

// What can be done with any object. Whatever runs Python code may throw: a JSValue that comes back empty says so, unless
// the comment says what else it may mean; after anything else, ask the VM.

inline PyType* typeOf(JSGlobalObject* globalObject, JSValue value) { return runtime(globalObject).typeOf(value); }

// ---- Attributes ----

JSValue getAttr(JSGlobalObject*, JSValue object, JSString* name);
// What getAttr() does for an AttributeError that comes out of the lookup without saying which attribute it was.
void completeAttributeError(JSGlobalObject*, JSValue thrown, JSValue object, JSValue name);
// Empty, with no exception, if there is no such attribute.
JSValue getAttrIfPresent(JSGlobalObject*, JSValue object, JSString* name);
void setAttr(JSGlobalObject*, JSValue object, JSString* name, JSValue value);
void delAttr(JSGlobalObject*, JSValue object, JSString* name);
// What these raise for an instance of a class that has no __setattr__, or no __delattr__. The name is a str or of a class derived from it.
void raiseCannotSetAttr(JSGlobalObject*, JSValue object, JSValue name, bool isDeletion);
bool hasAttr(JSGlobalObject*, JSValue object, JSString* name);

// object.__getattribute__, object.__setattr__ (an empty value deletes).
// With `suppress`, a missing attribute is an empty value and no exception.
JSValue genericGetAttr(JSGlobalObject*, JSValue object, JSString* name, bool suppress = false);
// For a name that is an instance of a class derived from str, `key`, whose text is `name`: looked up as a dict looks up any key.
JSValue genericGetAttrByKey(JSGlobalObject*, JSValue object, JSString* name, JSValue key, bool suppress = false);
void genericSetAttrByKey(JSGlobalObject*, JSValue object, JSString* name, JSValue key, JSValue value); // No value: delete.
JSValue getAttrByKey(JSGlobalObject*, JSValue object, JSValue key, bool suppress); // getattr(object, key)
void genericSetAttr(JSGlobalObject*, JSValue object, JSString* name, JSValue value);
// For a __getattribute__, __setattr__ or __delattr__ written in C++: `given` is the name as it got it, `name` the text of that
// (attributeName()).
inline JSValue genericGetAttrAsGiven(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue given)
{
    if (given.isString()) [[likely]]
        return genericGetAttr(globalObject, object, name);
    return genericGetAttrByKey(globalObject, object, name, given);
}
inline void genericSetAttrAsGiven(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue given, JSValue value)
{
    if (given.isString()) [[likely]]
        return genericSetAttr(globalObject, object, name, value);
    genericSetAttrByKey(globalObject, object, name, given, value);
}
// And for one whose object has its attributes in a dict that is not its __dict__: to object's own methods they are not there.
JSValue genericGetAttrWithDict(JSGlobalObject*, JSValue object, JSString* name, JSValue given, PyDict*);
void genericSetAttrWithDict(JSGlobalObject*, JSValue object, JSString* name, JSValue given, JSValue value, PyDict*); // No value: delete.
// And for one that passes the question on to another object: getattr(object, given), setattr(object, given, value),
// delattr(object, given).
void setAttrByKey(JSGlobalObject*, JSValue object, JSValue key, JSValue value); // No value: delete.
inline JSValue getAttrAsGiven(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue given)
{
    if (given.isString()) [[likely]]
        return getAttr(globalObject, object, name);
    return getAttrByKey(globalObject, object, given, false);
}
inline void setAttrAsGiven(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue given, JSValue value)
{
    if (given.isString()) [[likely]]
        return setAttr(globalObject, object, name, value);
    setAttrByKey(globalObject, object, given, value);
}
inline void delAttrAsGiven(JSGlobalObject* globalObject, JSValue object, JSString* name, JSValue given)
{
    if (given.isString()) [[likely]]
        return delAttr(globalObject, object, name);
    setAttrByKey(globalObject, object, given, { });
}

// For object.name(...): the function and, in `self`, what to pass it first; or the attribute, and an empty `self`.
JSValue getMethod(JSGlobalObject*, JSValue object, JSString* name, JSValue& self);
// What is found in a class is a function that an instance binds as its first argument, so that it can be called with that and no bound method.
bool isCalledWithInstanceFirst(JSValue descriptor);
// getattr(super(type, object), name), where `super` is whatever the global of that name is. If it is the built-in, no object is made of it.
// `isWrittenWithoutArguments`: super(). `self`, unless null: as for getMethod().
JSValue loadSuperAttribute(JSGlobalObject*, JSValue super, JSValue type, JSValue object, JSString* name, bool isWrittenWithoutArguments, JSValue* self);
// The same for a method that the language itself calls, which is looked for in the class alone. Empty, and no exception: there is none.
JSValue getSpecialMethod(JSGlobalObject*, JSValue object, Special, JSValue& self);

// The __dict__ of the object. Null: it has none, or, unless `create`, has had no use for one yet.
PyDict* instanceDict(JSGlobalObject*, JSValue object, bool create);
// What op_py_get_attr and op_py_get_method remember of the last attribute they got (their metadata has these fields), so as to
// get it again without a lookup from an object that is alike. The code that does so is in LowLevelInterpreter64.asm.
struct AttributeCache {
    // `offset`, if it is not that of a property of the object, which holds the attribute.
    static constexpr int32_t classValue = -1; // `payload` is the attribute. In a call, it is a function that takes the object as its first argument.
    static constexpr int32_t unboundClassValue = -2; // op_py_get_method only: `payload` is the attribute.
    static constexpr int32_t moduleEntry = -3; // The attribute is the value of entry `typeVersion` of the dict of the module, while PyDict::watchedVersion() of that is `payload`.
    static constexpr int32_t staticMethod = -4; // `payload` is a staticmethod: the attribute is what that wraps now.
    static constexpr int32_t classMethod = -5; // op_py_get_method only: `payload` is a classmethod. What that wraps now takes the class of the object as its first argument.
    static constexpr int32_t tupleItem = -6; // op_py_get_attr only: the attribute is item `payload` of the object, a tuple.
    static constexpr int32_t propertyGetter = -7; // op_py_get_attr only: `payload` is a property: the attribute is what the getter it has now returns for the object.
    // The object is a class, and the attribute was found in it or in one of its bases. As classValue, staticMethod, classMethod.
    static constexpr int32_t valueOfClass = -8;
    static constexpr int32_t staticMethodOfClass = -9;
    static constexpr int32_t classMethodOfClass = -10; // The first argument is the object.
    // Whether `payload` is a value. The versions vouch for it: while they hold, the class has it. The collector does not go by
    // that, since a dict can be written to behind the back of its class (gc.get_referents(C.__dict__)): what is remembered about
    // a value that is gone is forgotten, as it is for a Structure. See CodeBlock::reconcileLLIntInlineCachesAtGCEnd().
    static constexpr bool payloadIsValue(int32_t offset, uint32_t jsType) { return jsType || (offset < 0 && offset != moduleEntry && offset != tupleItem); }

    uint64_t payload { 0 };
    // The object is alike if it has this Structure and
    // - its type has this version;
    // - if it is a class, it has this version itself, and its type has `metatypeVersion`;
    // - if it is a module, as said above...
    StructureID structureID;
    uint32_t typeVersion { 0 };
    int32_t offset { 0 };
    // ... or, if this is not 0, if it has this JSType: a str, a list. Then `payload` is as for classValue.
    uint32_t jsType { 0 };
    uint32_t metatypeVersion { 0 };
};
// Finds the attribute and says where it is in the terms of AttributeCache. No code of the program has run when it says anything but None,
// and whatever getAttr(), or with `forCall` getMethod(), would check or raise before it returns the attribute has been checked.
enum class AttributeDescription : uint8_t {
    None, // Not to be said in those terms: that is for getAttr().
    Absent, // Neither the class nor the object has it: that is for getAttrOfAbsent().
    OfThisObjectNow, // Not to be remembered: the object keeps its attributes in a dict, its class has no version left...
    OfAllAlike,
};
AttributeDescription describeAttribute(JSGlobalObject*, JSValue object, JSString* name, bool forCall, AttributeCache&);
// What is left of getAttr() then: __getattr__, or the AttributeError.
JSValue getAttrOfAbsent(JSGlobalObject*, JSValue object, JSString* name);
// For getSpecialMethod().
bool describeSpecialMethod(JSGlobalObject*, JSValue object, Special, AttributeCache&);

// The same for op_py_set_attr: where `object.name = value` will put the value, under the conditions of describeAttribute(), setAttr()
// being the lookup.
struct SetAttributeDescription {
    enum class Kind : uint8_t {
        None, // Not to be said in these terms: that is for setAttr().
        InDict, // In `dict`, under the name.
        Replace, // The object has the attribute, at `offset`.
        Add, // It will be a new property, with `key`.
    };
    Kind kind { Kind::None };
    // Replace, Add: of every object that has the Structure, while `type` has the version it has now.
    bool holdsForAllAlike { false };
    PropertyOffset offset { invalidOffset };
    PyDict* dict { nullptr };
    UniquedStringImpl* key { nullptr };
    StructureID structureID;
    PyType* type { nullptr };
};
SetAttributeDescription describeSetAttribute(JSGlobalObject*, JSValue object, JSString* name);

// What object.__dict__[name] is, without asking for the dict, which costs an instance of a class its layout: see PyInstance.
// Empty: there is no such item, or no dict; or an exception, which only the __eq__ of a key that is not a str can raise.
JSValue instanceAttribute(JSGlobalObject*, JSValue object, JSString* name);
bool setInstanceDict(JSGlobalObject*, JSValue object, PyDict*);

// descriptor.__get__(object, type), if it has one. `object` is empty for an access through the type.
JSValue bindDescriptor(JSGlobalObject*, JSValue descriptor, JSValue object, PyType*);

// ---- Special methods: looked up on the type, never on the instance ----

// Bound to the object. Empty, with no exception, if the type has none.
JSValue lookupSpecial(JSGlobalObject*, JSValue object, Special);
// `method` is what type(self).lookup() found.
JSValue callSpecial(JSGlobalObject*, JSValue method, JSValue self, const CallArgs&);
// The same for a method that an operator calls if there is one (__eq__, __add__). What is found under the name may be a descriptor
// whose __get__ raises AttributeError: that counts as no method, and the result is NotImplemented.
JSValue callOperatorMethod(JSGlobalObject*, JSValue method, JSValue self, const CallArgs&);
inline JSValue callOperatorMethod(JSGlobalObject* globalObject, JSValue method, JSValue self, JSValue argument) { return callOperatorMethod(globalObject, method, self, CallArgs(&argument, 1)); }
inline JSValue callOperatorMethod(JSGlobalObject* globalObject, JSValue method, JSValue self, JSValue first, JSValue second)
{
    JSValue arguments[] = { first, second };
    return callOperatorMethod(globalObject, method, self, CallArgs(arguments, 2));
}
inline JSValue callSpecial(JSGlobalObject* globalObject, JSValue method, JSValue self) { return callSpecial(globalObject, method, self, CallArgs()); }
inline JSValue callSpecial(JSGlobalObject* globalObject, JSValue method, JSValue self, JSValue argument) { return callSpecial(globalObject, method, self, CallArgs(&argument, 1)); }
inline JSValue callSpecial(JSGlobalObject* globalObject, JSValue method, JSValue self, JSValue first, JSValue second)
{
    JSValue arguments[] = { first, second };
    return callSpecial(globalObject, method, self, CallArgs(arguments, 2));
}

// The same two, given where `method` was found: it is what type->lookup(special) gave, and `self` is an instance of `type`. What the type
// knows to be a C++ function that takes just these values (PyType::isNativeSpecialTaking()) is called as that: these are the slots
// of CPython's types.
template<typename... Arguments>
ALWAYS_INLINE bool canCallNativeSpecial(JSGlobalObject* globalObject, PyType* type, Special special, JSValue method)
{
    // As PyNativeFunction::invoke(): whatever recursion there is among functions written in C++ comes through here.
    return type->isNativeSpecialTaking(special, method, sizeof...(Arguments)) && getVM(globalObject).isSafeToRecurseSoft();
}
template<typename... Arguments>
ALWAYS_INLINE JSValue callNativeSpecial(JSGlobalObject* globalObject, JSValue method, JSValue self, Arguments... arguments)
{
    return uncheckedDowncast<PyNativeFunction>(method.asCell())->def().template functionOfArity<1 + sizeof...(Arguments)>()(globalObject, self, arguments...);
}
template<typename... Arguments>
ALWAYS_INLINE JSValue callSpecial(JSGlobalObject* globalObject, PyType* type, Special special, JSValue method, JSValue self, Arguments... arguments)
{
    if (canCallNativeSpecial<Arguments...>(globalObject, type, special, method)) [[likely]]
        return callNativeSpecial(globalObject, method, self, arguments...);
    return callSpecial(globalObject, method, self, arguments...);
}
template<typename... Arguments>
ALWAYS_INLINE JSValue callOperatorMethod(JSGlobalObject* globalObject, PyType* type, Special special, JSValue method, JSValue self, Arguments... arguments)
{
    if (canCallNativeSpecial<Arguments...>(globalObject, type, special, method)) [[likely]]
        return callNativeSpecial(globalObject, method, self, arguments...);
    return callOperatorMethod(globalObject, method, self, arguments...);
}

// ---- Calls ----

JSValue call(JSGlobalObject*, JSValue callable, const CallArgs&);
inline JSValue call(JSGlobalObject* globalObject, JSValue callable) { return call(globalObject, callable, CallArgs()); }
template<typename... Arguments>
requires(sizeof...(Arguments) > 0 && (std::is_convertible_v<Arguments, JSValue> && ...))
JSValue call(JSGlobalObject* globalObject, JSValue callable, Arguments... arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue values[] = { JSValue(arguments)... };
    RELEASE_AND_RETURN(scope, call(globalObject, callable, CallArgs(values, sizeof...(Arguments))));
}
// callable(*args, **kwargs). `kwargs` may be null.
JSValue callWithTupleAndDict(JSGlobalObject*, JSValue callable, PyTuple* args, PyDict* kwargs);
template<typename... Arguments>
JSValue callMethod(JSGlobalObject* globalObject, JSValue object, JSString* name, Arguments... arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue values[] = { JSValue(), JSValue(arguments)... };
    JSValue method = getMethod(globalObject, object, name, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (values[0])
        RELEASE_AND_RETURN(scope, call(globalObject, method, CallArgs(values, sizeof...(Arguments) + 1)));
    RELEASE_AND_RETURN(scope, call(globalObject, method, CallArgs(values + 1, sizeof...(Arguments))));
}
bool isCallable(JSGlobalObject*, JSValue);

// ---- Text ----

JSString* repr(JSGlobalObject*, JSValue);
// What __repr__ or __str__ returned, which may be an instance of a class derived from str.
JSValue reprObject(JSGlobalObject*, JSValue);
JSValue strObject(JSGlobalObject*, JSValue);
JSString* str(JSGlobalObject*, JSValue);
JSString* ascii(JSGlobalObject*, JSValue);
JSValue asciiObject(JSGlobalObject*, JSValue); // What __repr__ returned, if there is nothing to escape in it.
JSString* format(JSGlobalObject*, JSValue, JSString* spec); // A null spec is an empty one.
JSValue formatObject(JSGlobalObject*, JSValue, JSString* spec); // The same, as the object __format__() returned: it may be of a class derived from str.
// For messages: the text, or "<?>" and no exception if that throws... never throws.
String reprForMessage(JSGlobalObject*, JSValue);
// repr(value), cut short as "%.<limit>R" does it.
String shortRepr(JSGlobalObject*, JSValue, unsigned limit);

// ---- Comparison ----

// `a is b`
bool isIdentical(JSValue, JSValue);
int64_t hash(JSGlobalObject*, JSValue); // -1: an exception.
// The same, for a key: "cannot use 'list' as a dict key (unhashable type: 'list')". `what`: "a dict key".
int64_t hashForContainer(JSGlobalObject*, JSValue, ASCIILiteral what);
bool isTrue(JSGlobalObject*, JSValue);

// Operators, for the interpreter, which calls the methods that are written in Python itself. begin...() returns the result. If it returns
// nothing and has raised nothing, Runtime::methodToCall is the function to be called, with the operands. What that returns is the
// result, unless it is NotImplemented: then continue...(), given `progress`, says how it goes on, in the same way, or calls
// whatever there is to call.
enum class MethodsWrittenInPython : bool { AreCalled, AreLeftToCaller };

// One of Eq, NotEq, Lt, LtE, Gt, GtE.
JSValue richCompare(JSGlobalObject*, JSValue, JSValue, CompareOperator);
JSValue beginComparison(JSGlobalObject*, JSValue, JSValue, CompareOperator);
JSValue continueComparison(JSGlobalObject*, JSValue, JSValue, CompareOperator, unsigned progress, MethodsWrittenInPython);
// The same as a bool; an object is taken to equal itself.
bool richCompareBool(JSGlobalObject*, JSValue, JSValue, CompareOperator);
bool haveSameText(JSGlobalObject*, JSString*, JSString*);
ALWAYS_INLINE bool isEqual(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (a == b)
        return true;
    if (a.isInt32() && b.isInt32())
        return false;
    if (a.isString() && b.isString())
        return asString(a)->length() == asString(b)->length() && haveSameText(globalObject, asString(a), asString(b));
    return richCompareBool(globalObject, a, b, CompareOperator::Eq);
}
// Any of the operators.
JSValue compare(JSGlobalObject*, JSValue, JSValue, CompareOperator);

// ---- Containers ----

int64_t length(JSGlobalObject*, JSValue); // -1: an exception.
int64_t lengthHint(JSGlobalObject*, JSValue, int64_t defaultValue); // -1: an exception.
JSValue getItem(JSGlobalObject*, JSValue object, JSValue key);
void setItem(JSGlobalObject*, JSValue object, JSValue key, JSValue value);
void delItem(JSGlobalObject*, JSValue object, JSValue key);
bool contains(JSGlobalObject*, JSValue container, JSValue item);
// As PyMapping_Check(): it has a __getitem__ that takes keys. A list has; that of a deque takes indices and nothing else.
bool isSubscriptedByKeys(JSGlobalObject*, JSValue);
// What these do with an object that they have no case for (PyAbstractInlines.h).
JSValue getItemByMethod(JSGlobalObject*, JSValue object, JSValue key);
void setItemByMethod(JSGlobalObject*, JSValue object, JSValue key, JSValue value);
bool containsByMethod(JSGlobalObject*, JSValue container, JSValue item);

JSValue getIter(JSGlobalObject*, JSValue);
// Empty, with no exception, when there are no more.
JSValue iterNext(JSGlobalObject*, JSValue iterator);
bool isIterator(JSGlobalObject*, JSValue);

// For C++ that goes through an iterable: iter() and next(). A list, a tuple or a str itself is gone through as its iterator goes through
// it, by index, the length of the list being read each time, but without the iterator, which nobody would get to see.
class Items {
    WTF_MAKE_NONCOPYABLE(Items);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // May throw: what is not iterable.
    ALWAYS_INLINE Items(JSGlobalObject* globalObject, JSValue iterable)
        : m_globalObject(globalObject)
    {
        if (isExactList(iterable)) {
            m_kind = Kind::List;
            m_cell = iterable.asCell();
        } else if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == runtime(globalObject).tupleType()) {
            m_kind = Kind::Tuple;
            m_cell = tuple;
        } else if (iterable.isString())
            initializeWithStr(asString(iterable));
        else
            initializeWithIterator(iterable);
    }

    // Empty: there are no more, or an exception.
    ALWAYS_INLINE JSValue next()
    {
        switch (m_kind) {
        case Kind::List:
            if (auto* list = uncheckedDowncast<JSArray>(m_cell); m_index < list->length()) [[likely]]
                return listGet(list, m_index++);
            m_kind = Kind::Exhausted;
            return { };
        case Kind::Tuple:
            if (auto* tuple = uncheckedDowncast<PyTuple>(m_cell); m_index < tuple->length()) [[likely]]
                return tuple->at(m_index++);
            m_kind = Kind::Exhausted;
            return { };
        case Kind::Latin1Str:
            if (auto characters = uncheckedDowncast<JSString>(m_cell)->tryGetValueImpl()->span8(); m_index < characters.size()) [[likely]]
                return getVM(m_globalObject).smallStrings.singleCharacterString(characters[m_index++]);
            m_kind = Kind::Exhausted;
            return { };
        case Kind::Range:
            return uncheckedDowncast<PyIterator>(m_cell)->nextOfRange(m_globalObject);
        case Kind::BuiltinIterator:
            return uncheckedDowncast<PyIterator>(m_cell)->next(m_globalObject);
        case Kind::Iterator:
            return iterNext(m_globalObject, m_cell);
        case Kind::Exhausted:
            break;
        }
        return { };
    }

private:
    enum class Kind : uint8_t { List, Tuple, Latin1Str, Range, BuiltinIterator, Iterator, Exhausted };
    void initializeWithStr(JSString*);
    void initializeWithIterator(JSValue iterable);

    JSGlobalObject* m_globalObject;
    JSCell* m_cell { nullptr };
    unsigned m_index { 0 };
    Kind m_kind { Kind::Exhausted };
};

// Calls the function with each item until it returns false. False: it did, or an exception.
template<typename Function>
bool forEach(JSGlobalObject*, JSValue iterable, const Function&);

// ---- Numbers ----

JSValue binaryOperation(JSGlobalObject*, BinaryOperator, JSValue, JSValue);
JSValue beginBinaryOperation(JSGlobalObject*, BinaryOperator, JSValue, JSValue);
JSValue continueBinaryOperation(JSGlobalObject*, BinaryOperator, JSValue, JSValue, unsigned progress, MethodsWrittenInPython);
JSValue unaryOperation(JSGlobalObject*, UnaryOperator, JSValue);
JSValue power(JSGlobalObject*, JSValue base, JSValue exponent, JSValue modulus);
JSValue divmod(JSGlobalObject*, JSValue, JSValue);
JSValue absolute(JSGlobalObject*, JSValue);

// operator.index(): an exact int.
JSValue toIndex(JSGlobalObject*, JSValue);
bool hasIndex(JSGlobalObject*, JSValue);
// The same as an int64_t. What does not fit throws an exception of the given type; with none, it is clipped.
int64_t toSsize(JSGlobalObject*, JSValue, PyType* overflowError);
// As toDouble(), but an instance of a class derived from float is asked through its __float__, as float(value) asks it.
double toDoubleAsFloatDoes(JSGlobalObject*, JSValue);
// For an argument that must be an int that fits: OverflowError otherwise.
int64_t toInt64(JSGlobalObject*, JSValue);
int toCInt(JSGlobalObject*, JSValue);
// float(): for an argument that must be a real number.
double toDouble(JSGlobalObject*, JSValue);
// int(): through __int__, __index__ or __trunc__.
JSValue toInt(JSGlobalObject*, JSValue);

// ---- Types ----

bool isInstance(JSGlobalObject*, JSValue object, JSValue classOrTuple);
bool isSubclass(JSGlobalObject*, JSValue derived, JSValue classOrTuple);
// Without __instancecheck__.
inline bool isInstanceOf(JSGlobalObject* globalObject, JSValue object, PyType* type) { return typeOf(globalObject, object)->isSubtypeOf(type); }

// ---- Recursion in repr() ----

// True: the object is already being shown further up, and the caller shows "..." in its place.
class ReprGuard {
public:
    ReprGuard(JSGlobalObject*, JSValue);
    ~ReprGuard();
    bool isRecursive() const { return m_isRecursive; }

private:
    JSGlobalObject* m_globalObject;
    JSCell* m_cell;
    bool m_isRecursive;
};

template<typename Function>
bool forEach(JSGlobalObject* globalObject, JSValue iterable, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Items items(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, false);
    while (true) {
        JSValue item = items.next();
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        bool goOn = function(item);
        RETURN_IF_EXCEPTION(scope, false);
        if (!goOn)
            return false;
    }
}

} // namespace Python
} // namespace JSC
