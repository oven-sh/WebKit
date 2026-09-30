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

#pragma once

#include "ArgList.h"
#include "CallFrame.h"
#include "JSCJSValue.h"
#include "JSCellButterfly.h"
#include "PyRealm.h"
#include "PythonCommonNames.h"
#include "PythonFunctionInfo.h"
#include "PythonOperators.h"
#include "PythonText.h"
#include <wtf/HexNumber.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class BytecodeIndex;
class CodeBlock;
class JSModuleNamespaceObject;
class PyDict;
class PyNativeObject;
class PyRange;
class PyFrame;
class SourceCode;

namespace Python {

// To JavaScript a number is a number. Here what is an int32 is an int and what is a double is a float, so JSC::jsNumber() makes a float of an integer that does not fit in an int32, and an int of a double that has nothing after
// the point. This hides it from everything in this namespace, and takes only what is sure to fit. Anything wider is for intFromInt64() or intFromUInt64(), and a double is for floatFromDouble(). What is wanted as a number of
// JavaScript's, whichever it comes to, is asked for by its whole name.
template<typename T>
concept IntegerThatFitsInInt32 = std::is_integral_v<T> && (sizeof(T) < sizeof(int32_t) || (sizeof(T) == sizeof(int32_t) && std::is_signed_v<T>));
template<typename T>
concept EnumThatFitsInInt32 = std::is_enum_v<T> && IntegerThatFitsInInt32<std::underlying_type_t<T>>;
template<typename T>
requires IntegerThatFitsInInt32<T> || EnumThatFitsInInt32<T>
ALWAYS_INLINE JSValue jsNumber(T value) { return JSC::jsNumber(static_cast<int32_t>(value)); }

// What the language does. Each of these is what an opcode or a built-in function comes down to, for any values at all. They
// raise Python exceptions, in the way that JavaScriptCore throws: the caller checks its scope.

// The names of the keywords in a call, which are strings. It belongs to no global object, so it can be a constant of compiled code.
using KeywordNames = JSCellButterfly;
inline bool isKeywordNames(JSValue value) { return value.isCell() && value.asCell()->type() == JSCellButterflyType; }

// ---- Kinds of value

inline bool isNone(JSValue value) { return value.isUndefinedOrNull(); }
// int, and not bool.
bool isInt(JSValue);
bool isFloat(JSValue);
inline bool isStr(JSValue value) { return value.isString(); }
// A list, or an instance of a class derived from list: an Array, or what JavaScriptCore calls a derived array. isJSArray() is only the first.
inline bool isListCell(JSCell* cell) { return cell->type() == ArrayType || cell->type() == DerivedArrayType; }
bool isList(JSValue);

PyType* typeOf(JSGlobalObject*, JSValue);
bool isInstance(JSGlobalObject*, JSValue, PyType*); // By its type alone: not what __instancecheck__ or __class__ say.
bool isExactly(JSGlobalObject*, JSValue, PyType*); // And not of a class derived from it.
bool isExactly(JSGlobalObject*, JSValue, BuiltinType);
String typeName(JSGlobalObject*, JSValue);
String typeNameOfArgument(JSGlobalObject*, JSValue); // As _PyArg_BadArgument() puts it: None is "None".

// ---- Exceptions

JSObject* createException(JSGlobalObject*, PyType*, const String& message);
JSObject* createNotCallableError(JSGlobalObject*, JSValue callee);
// For an object that JavaScript cannot call, which is the callee of the frame, when it is Python that calls it. A module of JavaScript's is what it exports by default. Anything else is up to its class: what a constructor of
// JavaScript's made is a Map or a Date, say, and is not asked whether it can be called, but its class may be a program's and have __call__(). It raises the above if there is nothing to call.
JSC_DECLARE_HOST_FUNCTION(callWhatOnlyPythonCalls);
// NameError: name 'x' is not defined
JSValue raiseNameError(JSGlobalObject*, ThrowScope&, const String& name);
// Makes what is being handled now the __context__ of an exception that is about to be raised.
void setContext(JSGlobalObject*, JSObject* exception);
String nameOfFunction(JSGlobalObject*, JSFunction*, bool qualified);
JSObject* createException(JSGlobalObject*, PyType*, JSValue argument);
// These throw, and return an empty value for the caller to return.
JSValue raise(JSGlobalObject*, ThrowScope&, BuiltinType, const String& message);
JSValue raise(JSGlobalObject*, ThrowScope&, BuiltinType, JSValue argument);
JSValue raise(JSGlobalObject*, ThrowScope&, PyType*, const String& message); // One of a module's own
inline JSValue raiseTypeError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, BuiltinType::TypeError, message); }
inline JSValue raiseValueError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, BuiltinType::ValueError, message); }
inline JSValue raiseMemoryError(JSGlobalObject* globalObject, ThrowScope& scope) { return raise(globalObject, scope, BuiltinType::MemoryError, JSValue()); }

// ---- Text that there may be no room for: see PythonText.h
JSValue strOrMemoryError(JSGlobalObject*, const String&); // A str of the text. Empty if it raised.
String textOrMemoryError(JSGlobalObject*, String&&); // The text.
String textOfBytes(JSGlobalObject*, std::span<const uint8_t>); // A character for each byte. There can be more bytes than a string has room for. Null if it raised.

// The characters of a string, one to an element, for what has to go back and forth among them. False, with MemoryError raised, if there is no room.
template<size_t inlineCapacity>
bool charactersOf(JSGlobalObject* globalObject, StringView text, Vector<char32_t, inlineCapacity>& characters)
{
    if (!characters.tryReserveCapacity(text.length())) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseMemoryError(globalObject, scope);
        return false;
    }
    for (char32_t character : text.codePoints())
        characters.append(character);
    return true;
}

void throwUnboundVariable(JSGlobalObject*, CodeBlock*, JSString* name);
// Whether what has been thrown is an instance of the type. If so it is caught, and no longer thrown.
bool catchException(JSGlobalObject*, BuiltinType);
// _PyErr_FormatNote(): adds a note to what has been thrown, which goes on being thrown. It is what exception.add_note() does.
void addNoteToRaised(JSGlobalObject*, const String& note);
// The same, where working out what to say may run something. It is worked out with nothing thrown.
void addNoteToRaised(JSGlobalObject*, const ScopedLambda<String()>& note);
// The Python exception for whatever was thrown, which may have been thrown by JavaScript.
JSValue exceptionValue(JSGlobalObject*, JSValue thrown);

// ---- Attributes

// What an object keeps for itself, under a name that no program can name. If it is nothing, the object keeps it no longer: there is no such thing as a property that has nothing.
void putDirectOrRemove(JSGlobalObject*, JSObject*, PropertyName, JSValue);

JS_EXPORT_PRIVATE JSValue getAttribute(JSGlobalObject*, JSValue, PropertyName);
// Empty, and nothing raised, if there is no such attribute.
JSValue getAttributeIfPresent(JSGlobalObject*, JSValue, PropertyName);
JSValue getModuleAttribute(JSGlobalObject*, JSValue module, PropertyName); // module.__getattribute__()
void setAttribute(JSGlobalObject*, JSValue, PropertyName, JSValue);
void deleteAttribute(JSGlobalObject*, JSValue, PropertyName);
// What object and type do, whatever the class says: object.__getattribute__, which gives an empty value if there is none, and
// object.__setattr__, which deletes if the value is empty.
JSValue genericGetAttribute(JSGlobalObject*, JSValue, PropertyName);
void genericSetAttribute(JSGlobalObject*, JSValue, PropertyName, JSValue);
JSValue loadMethod(JSGlobalObject*, JSValue base, PropertyName, JSValue& self);
// A special method, which is looked for in the type and not in the instance. Empty if the type has none. `self` as for loadMethod.
JSValue lookupSpecial(JSGlobalObject*, JSValue, PropertyName, JSValue& self);
// The second half of that, for whoever has already found what the class of `value` has by the name: lookup_maybe_method() of CPython's Objects/typeobject.c. A function is what is to be called, with the
// value to begin with, and `self` is the value. Anything else is asked by __get__() what it is for the value, and `self` is empty.
JSValue bindSpecial(JSGlobalObject*, PyType*, JSValue attribute, JSValue value, JSValue& self);
// And that, called. It is how anything that a class has by a special name is to be called, since it need not be a function: it can be a static method, or whatever has a __get__().
JSValue callSpecial(JSGlobalObject*, PyType*, JSValue attribute, JSValue value);
JSValue callSpecial(JSGlobalObject*, PyType*, JSValue attribute, JSValue value, JSValue);
JSValue callSpecial(JSGlobalObject*, PyType*, JSValue attribute, JSValue value, JSValue, JSValue);
// What `descriptor`, found in a class, gives when got from `instance` (empty for the class itself) of `type`.
JSValue bindDescriptor(JSGlobalObject*, JSValue descriptor, JSValue instance, PyType*);
bool hasGet(JSGlobalObject*, JSValue); // Whether its class has a __get__(), so that it is not simply what it is when it is found in a class.
// descriptor.__set__(instance, newValue), or __delete__ if `newValue` is empty. `attribute` is what to call it if it cannot be done.
void setDescriptor(JSGlobalObject*, JSValue descriptor, JSValue instance, StringView attribute, JSValue newValue);

// What is in a slot: one of __slots__, or what in CPython is a field of the C struct of a built-in type. It is a property of the instance under
// a name that no attribute can have, and that __dict__ does not show. Empty if nothing is in it.
JSObject* createMemberDescriptor(JSGlobalObject*, PyType* owner, JSString* name, const Identifier* storage, JSValue initialValue = JSValue());

// The property that JavaScript sees on something of Python's, other than what it has of its own: getattr(), and a few names that mean to
// JavaScript what others mean to Python, like toString and Symbol.iterator. Empty if there is none.
JSValue getPropertyForJavaScript(JSGlobalObject*, JSValue receiver, PropertyName, PyType* from);
// Where an object's own attributes are, if it can have any: they are the properties of this. Null if it cannot.
JSObject* attributeStorage(JSGlobalObject*, JSValue, PyType*);
// Whether it is an object that JavaScript made, so that its attributes are its properties as JavaScript finds, sets and deletes them. `type` is its class.
bool isJavaScriptObject(JSValue, PyType* type);
// An attribute that something has of its own is a property of it that is enumerable, and is a value. What is not enumerable is JavaScript's business: the name and
// length of a function, the stack of an Error. So is an accessor. Python does not see either. Empty if there is no such attribute.
//
// A cell of Python's does not let JavaScript make a property of it anything else (definePropertyFromJavaScript()), or freeze it. A function is a JSFunction, which does.
//
// A module of JavaScript's is its namespace object. What that exports are its attributes, and are not kept here. What Python sets on it besides is, as __spec__: JavaScript finds
// nothing in a namespace object but what is exported and what is keyed by a symbol, so it does not see them, and has nothing to say about them.
//
// A name that JavaScript would take for an index, as "0", cannot be that of a property like the rest: to the engine that is an element of an array. It
// can only come from getattr() and the like or by way of a __dict__, not being something that can be written after a dot. An attribute of such a name is
// kept where a key of a __dict__ that is not a string is: in the dict, which is made if there was none.
inline bool isIndexLike(PropertyName name)
{
    UniquedStringImpl* uid = name.uid();
    return uid && uid->length() && isASCIIDigit((*uid)[0]) && !uid->isSymbol() && parseIndex(name);
}
JSValue getIndexLikeAttribute(VM&, JSObject*, PropertyName);
void putIndexLikeAttribute(VM&, JSObject*, PropertyName, JSValue);

inline JSValue getStoredAttribute(VM& vm, JSObject* object, PropertyName name)
{
    if (isIndexLike(name)) [[unlikely]]
        return getIndexLikeAttribute(vm, object, name);
    unsigned attributes;
    JSValue value = object->getDirect(vm, name, attributes);
    return value && !(attributes & (PropertyAttribute::DontEnum | PropertyAttribute::AccessorOrCustomAccessorOrValue)) ? value : JSValue();
}
// Whether JavaScript has left it so that the attribute can be deleted. It is to be asked first.
inline bool mayDeleteStoredAttribute(VM& vm, JSObject* object, PropertyName name)
{
    if (object->structure()->typeInfo().overloadsOperators() || isIndexLike(name) || object->type() == ModuleNamespaceObjectType) [[likely]]
        return true;
    unsigned attributes;
    return !object->getDirect(vm, name, attributes) || !(attributes & PropertyAttribute::DontDelete);
}
// What is raised if not. `isDeleting` says which was meant.
void raiseCannotSetAttribute(JSGlobalObject*, JSValue object, PropertyName, bool isDeleting);
inline void putStoredAttribute(VM& vm, JSObject* object, PropertyName name, JSValue value)
{
    if (isIndexLike(name)) [[unlikely]]
        return putIndexLikeAttribute(vm, object, name, value);
    object->putDirect(vm, name, value, static_cast<unsigned>(PropertyAttribute::None));
}
// The same, if JavaScript has left it so that it can be. False if it has not.
inline bool tryPutStoredAttribute(VM& vm, JSObject* object, PropertyName name, JSValue value)
{
    if (object->structure()->typeInfo().overloadsOperators() || isIndexLike(name) || object->type() == ModuleNamespaceObjectType) [[likely]] {
        putStoredAttribute(vm, object, name, value);
        return true;
    }
    unsigned attributes;
    if (!object->getDirect(vm, name, attributes)) {
        if (!object->isStructureExtensible())
            return false;
        attributes = 0;
    }
    // What is hidden is to be seen from now on, which what is not configurable cannot come to be.
    constexpr unsigned hiddenForGood = PropertyAttribute::DontEnum | PropertyAttribute::DontDelete;
    if ((attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue) || (attributes & hiddenForGood) == hiddenForGood)
        return false;
    object->putDirect(vm, name, value, attributes & PropertyAttribute::DontDelete);
    return true;
}
// Takes away a property that holds an attribute. This is what delattr() comes down to in the end, so it is not to go by way of what JavaScript's
// `delete` does to something of Python's, which is delattr().
bool deleteStoredAttribute(JSGlobalObject*, JSObject*, PropertyName);
// Whether its code is Python.
bool isPythonFunction(JSFunction*);
// isJavaScriptClass(): whether it is a class that JavaScript made, which is to say a constructor: what `new` can be used with.
// classFor(): what there is to know about one. Both are declared in PyType.h.
// The class of what has this for its prototype: that of the constructor that it is the prototype of, or failing that of the next one along that is
// some constructor's.
PyType* classForPrototype(JSGlobalObject*, JSValue prototype);
// The class of a class that JavaScript made. This does not take finding out all about it.
PyType* metatypeOfJavaScriptClass(JSGlobalObject*, JSObject* constructor);
// An attribute of a class. Empty if it has none. A class of JavaScript's is a function besides, and has what a function has.
enum class ClassIsFunctionToo : bool { No, Yes };
// If `from` is given, what the class itself defines is looked for beginning with that one in the order of resolution.
JSValue getTypeAttribute(JSGlobalObject*, PyType*, PropertyName, ClassIsFunctionToo = ClassIsFunctionToo::Yes, PyType* from = nullptr);
// For a class whose instances are the first to have a __dict__, or to be weakly referred to: the descriptors for those.
void addInstanceDescriptors(JSGlobalObject*, PyType*, bool addsDict, bool addsWeakReferences);
// The class that a value is, whichever language made it. Null if it is not one.
PyType* tryClass(JSGlobalObject*, JSValue);

// Whether an object that JavaScript is working on is Python's: a class, or an instance of the class that is its prototype. Something of JavaScript's can
// have a class for a prototype, or further up, if it was made to.
bool isPythonObject(JSGlobalObject*, JSValue);
// Getting a property of something of Python's, or asking whether it has one, as far as that is up to what it has of its own. `ordinary` is what
// finds that.
bool getOwnPropertySlotFromJavaScript(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&, bool (*ordinary)(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&));
// receiver.name = value, delete receiver.name and Object.defineProperty(receiver, name, descriptor) in JavaScript, of something of Python's.
bool setPropertyFromJavaScript(JSGlobalObject*, JSValue receiver, PropertyName, JSValue, PutPropertySlot&);
bool deletePropertyFromJavaScript(JSGlobalObject*, JSValue receiver, PropertyName);
bool definePropertyFromJavaScript(JSGlobalObject*, JSObject* receiver, PropertyName, const PropertyDescriptor&, bool shouldThrow);
// Whether a class has something to say about an attribute of its instances that comes before what the instance itself has: see
// PyType::instanceAccessIsAsFound().
enum class AttributeAccess : uint8_t { Get, Set };
bool classComesBeforeInstance(JSGlobalObject*, PyType*, PropertyName, AttributeAccess);
// Whether it has __set__ or __delete__.
bool isDataDescriptor(JSGlobalObject*, JSValue);
// Whether two things that a class may have by some name come to an instance in the same way: as they are, or as a method of it, and so on.
bool isGotFromInstanceInTheSameWay(JSGlobalObject*, JSValue, JSValue);

// Where getattr() finds an attribute, for what is compiled to find it there again without looking. It goes for whatever has the structure that the object has, for as long as type->instanceAccessIsAsFound() holds. If it is in
// a class, it goes for as long besides as the class has it there and those that come before it have nothing by the name: generateConditionsForPythonClassAttribute().
struct AttributeLocation {
    enum class Kind : uint8_t {
        Unknown, // Nothing that can be relied on.
        Own, // A property of the object.
        InClass, // A property of `holder`, that is to an instance what it is.
        Method, // A property of `holder`, that is a function which takes the instance for its first argument.
    };
    Kind kind { Kind::Unknown };
    PropertyOffset offset { invalidOffset };
    PyType* type { nullptr }; // What the object is an instance of.
    PyType* holder { nullptr };
};
AttributeLocation locateAttribute(JSGlobalObject*, JSValue, PropertyName);
// The class of an object for which setattr() by that name comes to setting a property of the object that anyone can see and set, and to nothing else. It goes for as long as the same. Null if it is not so.
PyType* classIfAttributeIsSetAsProperty(JSGlobalObject*, JSValue, PropertyName);

// Whether it is Python that made the call that a frame is for: code in Python, or something of Python's that is written in C++ and calls what it is given.
JS_EXPORT_PRIVATE bool isCalledByPython(VM&, CallFrame*);

// ---- Global variables

// Where a global variable was found, for whoever wants to remember: it is there for as long as the objects have these structures. If it was in
// `globals`, there is no `builtinsStructure`. If it is not something that can be relied on, there is no `globalsStructure` either.
struct GlobalLocation {
    Structure* globalsStructure { nullptr };
    Structure* builtinsStructure { nullptr };
    PropertyOffset offset { 0 };
};
// The value of a global variable: what `globals` has by that name, or failing that `builtins`. Otherwise it raises NameError.
// LOAD_GLOBAL asks a dict of a derived class as it would any mapping. LOAD_NAME, which has asked the locals so, looks in the globals for itself.
enum class GlobalsAre : bool { LookedIn, AskedAsAMapping };
JSValue loadGlobal(JSGlobalObject*, JSObject* globals, JSObject* builtins, PropertyName, GlobalLocation&, GlobalsAre = GlobalsAre::AskedAsAMapping);

// ---- Calls

JSValue call(JSGlobalObject*, JSValue callable, const ArgList&);
JSValue call(JSGlobalObject*, JSValue callable);
JSValue call(JSGlobalObject*, JSValue callable, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue, JSValue);
// The values of the keywords are the last of the arguments. `keywordNames` may be null.
// `thisValue` is for a function of JavaScript's: what it was got from, in base.function(...). Python's own make nothing of it.
JSValue callWithKeywords(JSGlobalObject*, JSValue callable, const ArgList&, KeywordNames* keywordNames, JSValue thisValue = jsUndefined());
// instance(...), of an instance of a class that has __call__().
JSValue callInstance(JSGlobalObject*, JSObject* instance, const ArgList&, KeywordNames*);
// Whether that is what calling it comes to: it is an instance of a class that has __call__(), whatever kind of cell it is.
bool isCallOfInstance(const CallData&);
// callable(*arguments, **keywords). The values of the keywords are added to the arguments. `keywords` may be null.
JSValue callWithKeywordDict(JSGlobalObject*, JSValue callable, MarkedArgumentBuffer& arguments, PyDict* keywords);
// The value of each parameter of a function written in Python. False if it raised.
bool bindArguments(JSGlobalObject*, JSFunction*, const FunctionInfo&, const ArgList&, KeywordNames*, MarkedArgumentBuffer& bound);
// What loadMethod or lookupSpecial gave.
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, const ArgList&);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, JSValue);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, JSValue, JSValue);
bool isCallable(JSGlobalObject*, JSValue);
// type(...): __new__, and then __init__. The first argument is not the type.
JSValue instantiate(JSGlobalObject*, PyType*, const ArgList&, KeywordNames* keywordNames);
// The part that `from`, and what comes after it in the order of resolution, have in making an instance of the class.
JSValue instantiateFrom(JSGlobalObject*, PyType*, PyType* from, const ArgList&, KeywordNames* keywordNames);

// ---- Classes

// What a class statement does. `body` is a function that fills in the namespace it is given.
JSValue buildClass(JSGlobalObject*, JSValue body, JSString* name, PyTuple* bases, PyDict* keywords);
// _PyType_CalculateMetaclass(): the most derived of the metaclasses of the bases and the one that was asked for. Null if it raised.
PyType* calculateMetaclass(JSGlobalObject*, PyType* metatype, PyTuple* bases);
// type.__new__(metatype, name, bases, namespace, **keywords)
JSValue newType(JSGlobalObject*, PyType* metatype, JSString* name, PyTuple* bases, PyDict* namespaceDict, PyDict* keywords);
// PyErr_NewException("module.name", base, NULL): a class made as a class statement in that module would make it. Null if it raised.
PyType* newException(JSGlobalObject*, ASCIILiteral module, ASCIILiteral name, PyType* base);
// isinstance() and issubclass(), which a class can have its own idea of.
bool isInstanceOf(JSGlobalObject*, JSValue, JSValue classInfo);

// function(self) or function(self, argument), for a function that is called on behalf of an instance. One of JavaScript's gets the instance as `this`.
JSValue callForInstance(JSGlobalObject*, JSValue function, JSValue self, JSValue argument = JSValue());

// property
JSValue getProperty(JSGlobalObject*, PyNativeObject*, JSValue instance);
void setProperty(JSGlobalObject*, PyNativeObject*, JSValue instance, JSValue); // An empty value deletes.
bool isAbstract(JSGlobalObject*, JSValue); // Whether it says that it is: __isabstractmethod__.

// list[int] and int | str
bool isGenericAlias(JSGlobalObject*, JSValue);
bool isUnion(JSGlobalObject*, JSValue);
PyTuple* argumentsOfUnion(JSValue);
JSValue newGenericAlias(JSGlobalObject*, JSValue origin, JSValue arguments, bool starred = false);
JSValue unionOf(JSGlobalObject*, JSValue, JSValue); // NotImplemented if either is not something that there can be a union of.
JSValue unionFrom(JSGlobalObject*, JSValue arguments);
PyTuple* makeParameters(JSGlobalObject*, JSValue arguments);
PyTuple* substituteParameters(JSGlobalObject*, JSValue self, JSValue arguments, PyTuple* parameters, JSValue item);
void appendTypeRepr(JSGlobalObject*, StringBuilder&, JSValue);

// What `def f[T]`, `class C[T]` and `type A = ...` make
bool isTypeAlias(JSGlobalObject*, JSValue);
JSValue newTypeVar(JSGlobalObject*, JSString* name, JSValue evaluator, bool isConstraints);
JSValue newParamSpec(JSGlobalObject*, JSString* name);
JSValue newTypeVarTuple(JSGlobalObject*, JSString* name);
void setTypeParameterDefault(JSGlobalObject*, JSValue parameter, JSValue evaluator);
JSValue newTypeAlias(JSGlobalObject*, JSString* name, JSValue typeParameters, JSValue computeValue);
JSValue subscriptGeneric(JSGlobalObject*, PyTuple* typeParameters);
JSObject* createTypingModule(JSGlobalObject*); // _typing

// t"..."
JSValue newInterpolation(JSGlobalObject*, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpecification);
JSValue newTemplate(JSGlobalObject*, JSValue strings, JSValue interpolations);
bool isSubclassOf(JSGlobalObject*, JSValue, JSValue classInfo);
PyTuple* defaultOrder(JSGlobalObject*, PyType*); // What type.mro() gives.
PyType* superCheck(JSGlobalObject*, PyType*, JSValue object); // The class whose order super(type, object) searches. Null, having raised, if it makes no sense.
void setBases(JSGlobalObject*, PyType*, JSValue); // C.__bases__ = ...
bool areLaidOutAlike(JSGlobalObject*, PyType* oldType, PyType* newType); // Whether an instance of the one could be made an instance of the other.
// An attribute got through super().
JSValue getSuperAttribute(JSGlobalObject*, JSValue superObject, PropertyName);

// ---- Modules

// A module is an instance of the class `module`, or of one derived from it, like any other instance. Its attributes, which are its properties, are
// the global variables of the code in it.
JS_EXPORT_PRIVATE JSObject* newModule(JSGlobalObject*, const String& name, PyType* = nullptr);
// One that is written in C++. It has the __doc__ that CPython's has, if CPython has it.
JS_EXPORT_PRIVATE JSObject* newBuiltinModule(JSGlobalObject*, ASCIILiteral name);
// The value, if it is a module. Otherwise null.
JSObject* tryModule(JSGlobalObject*, JSValue);
// A list of the names of what a module of JavaScript's exports, less what it has not got as far as giving a value.
JSArray* namesOfExports(JSGlobalObject*, JSModuleNamespaceObject*);

// import name, as the statement does it. `fromList` is None or a tuple of names.
void registerModule(JSGlobalObject*, const String& name, JSValue module);
// The source in a file. Null, with nothing raised, if it cannot be read.
SourceCode readSourceIfPresent(JSGlobalObject*, const String& path);
// The module that a file is, which is run if it has not been. For a module that is asked for by where it is: JavaScript's `import`.
JS_EXPORT_PRIVATE JSValue importModuleFromSource(JSGlobalObject*, const SourceCode&);
// The same, as what a module of JavaScript's can import: each of its global variables by name, and itself as the default.
void exportModule(JSGlobalObject*, const SourceCode&, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues);
// That, of a module that there is already. It is for a host that has some other way of saying which is meant.
JS_EXPORT_PRIVATE void exportModule(JSGlobalObject*, JSValue module, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues);
// exception.args
PyTuple* exceptionArguments(JSGlobalObject*, JSValue exception);
// OSError(errno, strerror(errno)[, filename]), or the class derived from it that is for that error.
JS_EXPORT_PRIVATE JSValue raiseOSError(JSGlobalObject*, ThrowScope&, int errorNumber, JSValue filename = JSValue(), PyType* derivedFromOSError = nullptr);
// What is given for the name of a file, as the system wants it: a str, bytes, or what has __fspath__. Nothing if it raised.
// It cannot have a zero in it, and what is said if it has depends on who is asking: PyUnicode_FSDecoder() unless it is given, and "embedded null byte" is PyUnicode_FSConverter().
JSValue fileSystemPathOf(JSGlobalObject*, JSValue); // PyOS_FSPath(). Empty if it raised.
JS_EXPORT_PRIVATE std::optional<CString> toFileSystemPath(JSGlobalObject*, JSValue, ASCIILiteral ifItHasZero = { });
// An attribute of the module sys, as it is now. Empty if it has been deleted.
JSValue sysAttribute(JSGlobalObject*, ASCIILiteral name);

// ---- Looking into code that is running

// The namespace of the module that the code of a frame is in. Null if it is not Python's.
JSObject* globalsOfFrame(JSGlobalObject*, CallFrame*);
// What locals() gives there: the namespace itself for a module or the body of a class, and for a function a new dict of its variables.
JSValue localsOfFrame(JSGlobalObject*, CallFrame*);
JSValue localsOfFrame(JSGlobalObject*, PyFrame*);
// The frame of the Python code that called a function written in C++.
CallFrame* callerOf(CallFrame*);
// _PyThreadState_GetFrame(): the innermost frame that is Python's, whatever is being run. Null if there is none.
CallFrame* innermostPythonFrame(VM&);
// What is in a cell, which is empty if nothing is.
JSValue contentsOfCell(JSValue cell);
// The cell for __class__, from what the body of a class returned. None if there is none.
JSValue cellForClass(JSGlobalObject*, JSValue returnedByBody);
// Whether it is a generator whose code has CO_ITERABLE_COROUTINE, and so can be awaited.
bool isIterableCoroutine(JSGlobalObject*, JSValue);
void setContentsOfCell(VM&, JSValue cell, JSValue);
WriteBarrierBase<Unknown>* variableOfCell(JSValue cell, JSCell*& owner); // Where that is, and the cell that a write to it is a write to.

// Notes that an exception has come to a frame of Python code, by being raised in it or by coming out of what it called. The unwinder calls it.
void addTracebackEntry(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex);
// _PyTraceback_Add(): the same of what has been raised, for something written in C++ that it has come to, which is to be found in the traceback as if it were a function of that name in that file. pyexpat does it
// for what calls a handler.
JS_EXPORT_PRIVATE void addTracebackEntry(JSGlobalObject*, ASCIILiteral functionName, ASCIILiteral filename, int line);
// What it has for a traceback, as it is kept. Empty if it has none.
JSValue tracebackOf(VM&, JSValue exception);
// The frames of a traceback as JavaScript has a stack, which is from where it was raised outwards, for `owner` to keep.
Vector<StackFrame> stackOfTraceback(VM&, JSCell* owner, JSValue traceback);
// A frame of Python code is about to be no more. If it has a frame object, that outlives it. The unwinder calls it, and op_py_ret.
void leaveFrame(VM&, CallFrame*, BytecodeIndex);
// The slow path of op_py_enter: either it is too deep, and RecursionError is raised, or something is to be told that a frame has begun or been resumed.
void raiseRecursionError(JSGlobalObject*);
void enterFrame(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isResume);
// Those of op_py_line, op_py_ret and op_py_leave, which are only come to if something is to be told.
// What is not to be done when it comes up, since anything might be going on, and is put off until Python code is next between one thing and another: the callbacks of weak references, and KeyboardInterrupt. It is
// _Py_HandlePending() of CPython's Python/ceval_gil.c. See VM::hasPythonWork().
void doPendingWork(JSGlobalObject*);
enum class LineKind : uint8_t {
    Line,
    WhereItCanBeGoneOnFrom, // The same, and nothing but the variables and what CodeDetails::jumpBlocks tells of is made use of by what comes after: it is the beginning of a statement. frame.f_lineno = n can go to it.
    AfterBackwardJump, // Going round a loop again, which sys.settrace() is told of though it be all on one line.
    OfHandledException, // Nothing is told. The frame is back on the line that what is being handled was raised on, as it is when `with` lets an exception go on its way.
};
std::optional<BytecodeIndex> frameIsAtLine(JSGlobalObject*, CallFrame*, BytecodeIndex, LineKind); // What to run next, if that is not what comes next.
void setTracesOpcodes(JSGlobalObject*, PyFrame*, bool); // Whether sys.settrace() is told of each instruction of the frame's code.
int lineOfTracebackFor(JSGlobalObject*, JSValue exception, PyFrame*); // The line that an exception came to a frame on, or -1.
enum class ToldArgument : uint8_t { None, First, ListOfPositional };
void frameIsCalling(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue callable, JSValue argument, ToldArgument);
void frameIsBranching(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isTaken);
void frameIsBranchingInLoop(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isExhausted); // At op_py_iter_next.
void frameIsJumping(JSGlobalObject*, CallFrame*, BytecodeIndex);
void frameIsReturning(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue);
void frameIsYielding(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue);
// A generator that the frame was going through, or waiting on, has returned. To be called if VM::isPythonWatched().
void generatorHasReturnedTo(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue returned);
void generatorHasReturnedOnThrowTo(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue returned);
// What is no generator says that there is no more by raising StopIteration, which is caught at once by what asked. It was raised all the same, and the frame that asked is told
// of as one that it came to. The first is for what catches it, the second for what began the asking, before and after. All are for if VM::isPythonWatched().
void noteCaughtStopIteration(JSGlobalObject*, JSValue exception);
void forgetCaughtStopIteration(JSGlobalObject*);
void tellOfCaughtStopIteration(JSGlobalObject*, CallFrame*, BytecodeIndex);
// The unwinder has done with a frame of Python code, which is counted no more.
void unwindFrame(VM&, CallFrame*, BytecodeIndex);
// What becomes of an exception in a frame of Python code, which the unwinder tells of if VM::isPythonWatched(). What is told may raise something itself, which is then what is
// being thrown, and is returned.
enum class ExceptionProgress : uint8_t {
    CameToFrame, // By being raised in it, or by coming out of what it called.
    WasRaisedAgain, // By what had caught it, in the same frame.
    IsHandled,
    LeavesFrame,
};
Exception* tellOfException(VM&, CallFrame*, BytecodeIndex, JSValue thrown, ExceptionProgress);
// What Python prints when an exception gets away: the traceback, and those of what led to it.
String formatException(JSGlobalObject*, JSValue exception);
bool isSequence(JSGlobalObject*, JSValue); // PySequence_Check()
bool isSequenceSlot(JSGlobalObject*, JSValue method); // Whether it is what a built-in sequence has for + or *, which has its turn after what numbers have. See binaryOperation().
// Warnings from what is written in C++: PyErr_WarnEx() and PyErr_WarnExplicitObject(). False if something has been raised, as it is if the program has asked for such warnings to be errors.
bool warn(JSGlobalObject*, BuiltinType category, const String& message, int64_t stackLevel = 1, JSValue source = { });
bool warnExplicit(JSGlobalObject*, BuiltinType category, const String& message, const String& filename, int64_t line);
// For what __int__() and the like return, which is to be of the class itself. What is of a class derived from it is let by, with a warning: `before`, the name of its class, and the rest.
bool warnIfOfStrictSubclass(JSGlobalObject*, JSValue result, BuiltinType, const String& before, ASCIILiteral className);
// For `except*`.
JSValue matchExceptionGroup(JSGlobalObject*, CallFrame*, JSValue exception, JSValue pattern);
JSValue prepareReraiseStar(JSGlobalObject*, JSValue original, JSArray* exceptions);
void forEachTracebackEntry(JSGlobalObject*, JSValue traceback, const ScopedLambda<void(PyFrame*, unsigned bytecodeOffset, unsigned line)>&); // From the outermost frame in.
String appendSyntaxErrorLocation(JSGlobalObject*, StringBuilder&, JSValue exception);
bool isSpace(char32_t); // str.isspace()
String formatTraceback(JSGlobalObject*, JSValue traceback); // PyTraceBack_Print(): "Traceback (most recent call last):" and what follows, or nothing if it is not a traceback.
// _Py_DisplaySourceLine(): a line of a file, without what it is indented by. Null if there is no such line, or no such file.
String sourceLineForDisplay(JSGlobalObject*, const String& filename, int64_t line);
// What has been raised has nowhere to go: sys.unraisablehook is told of it, and nothing is raised any longer. PyErr_FormatUnraisable() and PyErr_WriteUnraisable().
void reportUnraisable(JSGlobalObject*, const String& message, JSValue object = JSValue());
// PyErr_FormatUnraisable("message %R", shown). If there is no showing it, nothing is said of what was being done either.
void reportUnraisableShowing(JSGlobalObject*, ASCIILiteral message, JSValue shown);
// sys.audit(event, *arguments). False if a hook raised. It costs nothing to speak of while there are no hooks.
bool auditSlow(JSGlobalObject*, ASCIILiteral event, const ArgList& arguments);
// What has been raised and not caught by a program that is being run: sys.excepthook is given it. PyErr_Print(), but for SystemExit.
void reportUncaughtException(JSGlobalObject*, JSValue exception);
// PySys_WriteStderr(): to sys.stderr. What goes wrong with that is lost.
void writeToStandardError(JSGlobalObject*, const String&);

// ---- Operators

JSValue binaryOperation(JSGlobalObject*, BinaryOperator, bool inPlace, JSValue, JSValue);
JSValue unaryOperation(JSGlobalObject*, UnaryOperator, JSValue);
JSValue absolute(JSGlobalObject*, JSValue); // abs()
JSValue sequenceConcatenate(JSGlobalObject*, bool inPlace, JSValue, JSValue); // PySequence_Concat() and PySequence_InPlaceConcat()
int64_t sequenceSize(JSGlobalObject*, JSValue); // PySequence_Size()
JSValue sequenceItem(JSGlobalObject*, JSValue, int64_t index); // PySequence_GetItem()
void checkIsIndexable(JSGlobalObject*, JSValue); // What that looks at before it gets anything. It may raise.
JSValue compare(JSGlobalObject*, ComparisonOperator, JSValue, JSValue);
bool isTrue(JSGlobalObject*, JSValue);
bool isEqual(JSGlobalObject*, JSValue, JSValue); // x is y or x == y, which is what containers ask.
bool isIdentical(JSValue, JSValue);
bool contains(JSGlobalObject*, JSValue container, JSValue);
JSValue power(JSGlobalObject*, JSValue, JSValue, JSValue modulus);
JSValue powerOfInts(JSGlobalObject*, JSValue base, JSValue exponent, JSValue modulus); // All three are ints, and there is a modulus.
JSValue divmod(JSGlobalObject*, JSValue, JSValue);

// ---- Subscripts

JSValue getItem(JSGlobalObject*, JSValue, JSValue key);
void setItem(JSGlobalObject*, JSValue, JSValue key, JSValue);
void deleteItem(JSGlobalObject*, JSValue, JSValue key);
// len(). Negative if it raised.
int64_t length(JSGlobalObject*, JSValue);

// Whether what is in it can be taken straight out of it: it is a dict, of a class that may be derived from dict but goes through it as dict does. As in dict_merge(), a
// __getitem__() or a keys() of its own is not asked.
bool isGoneThroughAsDict(JSGlobalObject*, JSValue);
void updateDictFrom(JSGlobalObject*, PyDict*, JSValue mappingOrPairs);

// ---- Iteration

// A list is a JavaScript array, and this is as many elements as one of those keeps side by side. With more it keeps them in a table, one by one, which is no way to keep a list.
static constexpr size_t maxListLength = MAX_STORAGE_VECTOR_LENGTH;

JSValue getIterator(JSGlobalObject*, JSValue);
// Whether its class has what it takes to be gone through, whatever comes of trying: __iter__(), or __getitem__() if it is not a dict. Where CPython puts a TypeError from iter() in words of its own, it is
// only for what has neither, so that what a program's own __iter__() raises is not lost.
bool hasWhatItTakesToBeIterated(JSGlobalObject*, JSValue);
// Empty when there is no more, with nothing raised. If that is because a generator has returned, what it returned is given too.
JSValue iteratorNext(JSGlobalObject*, JSValue iterator, JSValue* returnedByGenerator = nullptr);
// The same, but that if it is by raising StopIteration that it said there is no more, that is still raised: tp_iternext.
JSValue iteratorNextKeepingStopIteration(JSGlobalObject*, JSValue iterator, JSValue* returnedByGenerator = nullptr);
// (*Py_TYPE(iterator)->tp_iternext)(iterator), which is what an iterator that goes through another one asks it: empty when there is no more, and then StopIteration has been raised if there is anything for it to say,
// as there is when a generator returns something. So what returns as soon as this is empty says whatever the other one said.
JSValue iteratorStep(JSGlobalObject*, JSValue iterator);
// Calls the function with each. It returns false to stop. Returns false if something was raised.
bool forEach(JSGlobalObject*, JSValue iterable, const ScopedLambda<bool(JSValue)>&);
bool collect(JSGlobalObject*, JSValue iterable, MarkedArgumentBuffer&);
// PyObject_LengthHint(): how many it says there are in it, by __len__() or __length_hint__(), or the default if it does not say. Nothing if it raised.
std::optional<int64_t> lengthHint(JSGlobalObject*, JSValue, int64_t defaultValue);
// What CPython has ways of its own to put in a list, without asking anything of it: _list_extend() of its Objects/listobject.c.
bool isPutInListWithoutAsking(JSGlobalObject*, JSValue iterable);
// PySequence_List(): as list(iterable) gathers them, which is to ask first how many there will be. It raises MemoryError if that is more than a list that already has `alreadyThere` has room for.
bool collectAsList(JSGlobalObject*, JSValue iterable, MarkedArgumentBuffer&, size_t alreadyThere = 0);
// PySequence_Fast(): the same, but that it is the iterator that is asked how many, and that what cannot be gone through at all is complained of in the words given.
bool collectFast(JSGlobalObject*, JSValue iterable, MarkedArgumentBuffer&, ASCIILiteral complaint);
void unpackSequence(JSGlobalObject*, JSValue iterable, unsigned count, int starIndex, Register* first);
// The same, into a tuple, which is what was given if that will do. Null if it raised.
PyTuple* unpackSequenceIntoTuple(JSGlobalObject*, JSValue iterable, unsigned count, int starIndex);
JSValue newTuple(JSGlobalObject*, Register* first, unsigned count);

// ---- What every object can be asked

String repr(JSGlobalObject*, JSValue);
String str(JSGlobalObject*, JSValue);
// PyObject_Repr() and PyObject_Str(): what __repr__() or __str__() returned, which is a str or an instance of a class derived from str, and is what repr() and str() give.
JSValue reprObject(JSGlobalObject*, JSValue);
JSValue strObject(JSGlobalObject*, JSValue);
String addressOf(const void*); // 0x..., as in <object object at 0x...>
String fullyQualifiedTypeName(JSGlobalObject*, JSValue); // What CPython's %T writes: the class of something, with where it is from unless that is builtins or __main__.
JSValue format(JSGlobalObject*, JSValue, const String& specification);
// Never -1, unless it raised.
int64_t hash(JSGlobalObject*, JSValue);
int64_t hashOfString(const String&);
// object.__hash__
int64_t hashOfPointer(const void*);

// ---- Numbers

// An int, from what may not fit an int32.
// a * b + c, as it comes out in CPython where that is written as one expression. Where there is an instruction that does both, the compilers that CPython is built with use it, and it rounds once where two would
// round twice, which shows in the last digit. Nothing here is compiled that way, since that is not what JavaScript's arithmetic is, so where CPython has such an expression it is said.
inline double multiplyAdd(double a, double b, double c)
{
#if CPU(ARM64)
    return std::fma(a, b, c);
#else
    return a * b + c;
#endif
}

JSValue intFromInt64(JSGlobalObject*, int64_t);
JSValue intFromUInt64(JSGlobalObject*, uint64_t);
bool isNumber(JSGlobalObject*, JSValue); // PyNumber_Check()
// _PyUnicode_TransformDecimalAndSpaceToASCII(), of what is not all ASCII: a digit of any script is that digit, and a space of any kind is a space. It ends with a question mark at the first thing that is neither. Null if it raised.
String decimalsAndSpacesInASCII(JSGlobalObject*, StringView);
std::optional<int64_t> toSsizeOfInt(JSGlobalObject*, JSValue); // PyLong_AsSsize_t(), which takes an int and nothing that could be made one. Nothing if it raised.
// The "i" of PyArg_ParseTuple(). Nothing if it raised.
std::optional<int> toCIntOfFormat(JSGlobalObject*, JSValue);
// PyLong_AsUnsignedLongLongMask(): what an int has in its low 64 bits, whatever else it has.
uint64_t lowBitsOfInt(JSValue);
// UNSIGNED_INT_CONVERTER() of CPython's Objects/longobject.c, which is _PyLong_UInt16_Converter() and the like: an int, or what has __index__(), that is not negative and is no more than `maximum`. `typeName` is what C calls
// what it is to fit in. Nothing if it raised.
std::optional<uint64_t> toUnsignedNoMoreThan(JSGlobalObject*, JSValue, uint64_t maximum, ASCIILiteral typeName);
template<typename Type>
std::optional<Type> toUnsigned(JSGlobalObject* globalObject, JSValue value, ASCIILiteral typeName)
{
    auto converted = toUnsignedNoMoreThan(globalObject, value, std::numeric_limits<Type>::max(), typeName);
    if (!converted)
        return std::nullopt;
    return static_cast<Type>(*converted);
}
JSValue parseInt(JSGlobalObject*, StringView, unsigned base); // The int that a string spells, in the base. Empty if it spells none.
JSValue intFromDouble(JSGlobalObject*, double); // Truncated.
JSValue floatFromDouble(double);
// ---- range

// len(range). It raises OverflowError, and this is -1, if that is more than fits.
int64_t rangeLength(JSGlobalObject*, PyRange*);
// range[key]
JSValue rangeGetItem(JSGlobalObject*, PyRange*, JSValue key);
// value in range
bool rangeContains(JSGlobalObject*, PyRange*, JSValue);
bool rangesAreEqual(PyRange*, PyRange*);
JSValue rangeIterator(JSGlobalObject*, PyRange*);
// The item of a range that is at an index, which is an int that is not negative. Empty if that is past the end.
JSValue nextOfLongRange(JSGlobalObject*, PyRange*, JSValue index);

// ---- Numbers, from anything

// operator.index(value): an int and nothing else, from an int, a bool, an instance of a class derived from int, or what has __index__.
JSValue toInt(JSGlobalObject*, JSValue);
// int(value): PyNumber_Long(). By __int__(), by __index__(), or from what is written in a str or in bytes.
JSValue numberLong(JSGlobalObject*, JSValue);
// The value of an int, if it fits.
std::optional<int64_t> tryInt64(JSValue);
int compareInts(JSValue, JSValue); // Negative, zero or positive. Both are ints, of any size.
// What __index__ gives, as something to index with: clamped to the range of an int64 if `clamp`, or else it raises IndexError.
std::optional<int64_t> toIndex(JSGlobalObject*, JSValue, bool clamp = false);
std::optional<int64_t> toIndexOrOverflow(JSGlobalObject*, JSValue); // PyNumber_AsSsize_t(value, PyExc_OverflowError)
// The same, for an argument that in CPython is a Py_ssize_t, a long or an int of C's. It raises OverflowError if it does not fit in one, and says which.
std::optional<int64_t> toSsize(JSGlobalObject*, JSValue);
std::optional<int64_t> toCLong(JSGlobalObject*, JSValue);
std::optional<long long> toCLongLong(JSGlobalObject*, JSValue);
std::optional<int> toCInt(JSGlobalObject*, JSValue);
// `unsigned_int(bitwise=True)` of Argument Clinic: PyLong_AsUnsignedLongMask(). Nothing if it raised.
std::optional<uint32_t> toUnsignedIntMask(JSGlobalObject*, JSValue);
// _Py_convert_optional_to_ssize_t(): an argument that is an int or None, or was not given.
std::optional<int64_t> toOptionalSsize(JSGlobalObject*, JSValue, int64_t defaultValue);
// The `str` and `str(accept={str, NoneType})` of Argument Clinic: text with no zero in it. A null String is None, where that will do. Nothing if it raised.
// `argument` is what to call it: "argument", "argument 1" or "argument 'mode'".
std::optional<String> toTextArgument(JSGlobalObject*, JSValue, ASCIILiteral function, ASCIILiteral argument, bool mayBeNone = false);
// _PyEval_SliceIndex(), and _PyEval_SliceIndexNotNone(): where something begins or ends, clamped. Whether it is None is for the caller to have seen to: `mayBeNone` is only for what is said.
std::optional<int64_t> toSliceIndex(JSGlobalObject*, JSValue, bool mayBeNone);
// The value of an int, a bool or a float, or of what has __float__ or __index__.
std::optional<double> toDouble(JSGlobalObject*, JSValue);
String reprOfDouble(double);
String reprOfComplex(double real, double imaginary);

// ---- The arguments of a function written in C++

// They are on the stack, those that were given by position and then those that were given by name, whose names are `this`. If there are so many that they are better kept off the stack, they are in something
// like what the names are in, which is all that is on the stack, and `this` is the names even if there are none: see callWithKeywords(). Neither is anything that a program can get hold of.
class NativeArguments {
public:
    explicit NativeArguments(CallFrame* callFrame)
        : m_callFrame(callFrame)
        , m_values(std::bit_cast<EncodedJSValue*>(callFrame->addressOfArgumentsStart()))
        , m_positionalCount(callFrame->argumentCount())
    {
        if (isKeywordNames(callFrame->thisValue())) [[unlikely]]
            takeNames(uncheckedDowncast<KeywordNames>(callFrame->thisValue().asCell()));
    }

    // How many were given by position.
    unsigned size() const { return m_positionalCount; }
    // The argument that is at a position in the signature, whether it was given by position or by name. Empty if it was not given.
    JSValue at(unsigned index) const
    {
        if (index < m_positionalCount) [[likely]]
            return JSValue::decode(m_values[index]);
        return m_keywordNames ? givenByName(index) : JSValue();
    }
    // One that there has to be, so that it is known to have been given, by position or by name.
    JSValue operator[](unsigned index) const
    {
        JSValue value = at(index);
        ASSERT(value);
        return value;
    }
    // All of them but the first so many, to pass on: what was given by position, and then what was given by name.
    ArgList allFrom(unsigned index) const
    {
        ASSERT(index <= m_positionalCount);
        return ArgList(m_values + index, m_positionalCount + keywordCount() - index);
    }

    unsigned keywordCount() const { return m_keywordNames ? m_keywordNames->length() : 0; }
    JSString* keywordName(unsigned index) const { return asString(m_keywordNames->get(index)); }
    JSValue keywordValue(unsigned index) const { return JSValue::decode(m_values[m_positionalCount + index]); }
    // Empty if it was not given.
    JSValue keyword(JSGlobalObject*, ASCIILiteral name) const;
    KeywordNames* keywordNames() const { return m_keywordNames; }

    // Raises TypeError, in the words CPython uses, and returns false, if there are not between these many or there are keywords.
    bool check(JSGlobalObject*, ThrowScope&, ASCIILiteral functionName, unsigned minimum, unsigned maximum) const;
    bool checkNoKeywords(JSGlobalObject*, ThrowScope&, ASCIILiteral functionName) const;

    CallFrame* callFrame() const { return m_callFrame; }

private:
    JSValue givenByName(unsigned index) const;

    void takeNames(KeywordNames* names)
    {
        if (m_positionalCount == 1 && isKeywordNames(JSValue::decode(m_values[0]))) {
            auto* values = uncheckedDowncast<JSCellButterfly>(JSValue::decode(m_values[0]).asCell());
            m_values = std::bit_cast<EncodedJSValue*>(values->toButterfly()->contiguous().data());
            m_positionalCount = values->length();
        }
        m_positionalCount -= names->length();
        if (names->length())
            m_keywordNames = names;
    }

    CallFrame* m_callFrame;
    EncodedJSValue* m_values;
    KeywordNames* m_keywordNames { nullptr };
    unsigned m_positionalCount;
};

} } // namespace JSC::Python
