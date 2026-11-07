/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyException.h"

#include "DeferTermination.h"
#include "DisallowVMEntry.h"
#include "ErrorInstance.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSWeakMapInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyType.h"
#include "PyTypes.h"
#include "TopExceptionScope.h"
#include <errno.h>
#include <unicode/uchar.h>
#include <wtf/HexNumber.h>
#include <wtf/unicode/CharacterNames.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The built-in exceptions. (Objects/exceptions.c in CPython is the reference.)

PY_DEFINE_CELL(PyException);

PyException::PyException(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyException* PyException::create(VM& vm, Structure* structure, PyType* type)
{
    auto* exception = new (NotNull, allocateCell<PyException>(vm)) PyException(vm, structure, type);
    exception->finishCreation(vm);
    return exception;
}

static JSObject* prototypeForInstancesOfExceptionClass(JSGlobalObject*, PyType*);

JSValue PyException::getPrototype(JSObject* object, JSGlobalObject*)
{
    auto* exception = uncheckedDowncast<PyException>(object);
    Runtime& rt = runtime(exception->realm());
    if (exception->type()->isSubtypeOf(rt.JSError())) [[unlikely]] {
        if (JSObject* prototype = prototypeForInstancesOfExceptionClass(exception->realm(), exception->type()))
            return prototype;
    }
    if (exception->type()->isSubtypeOf(rt.TypeError()))
        return rt.jsViewObject(JSViewObject::TypeErrorPrototype);
    return exception->getPrototypeDirect();
}

template<typename Visitor>
void PyException::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyException>(cell);
    Base::visitChildren(thisObject, visitor);
    // In the order in which CPython's lets go of them: a derived type first clears what is its own, then BaseException the rest. See DeadGraph.
    visitor.appendValues(thisObject->m_fields.data(), numberOfFields);
    visitor.append(thisObject->m_dict);
    visitor.append(thisObject->m_args);
    visitor.append(thisObject->m_traceback);
    visitor.append(thisObject->m_cause);
    visitor.append(thisObject->m_context);
}

DEFINE_VISIT_CHILDREN(PyException);

// ---- Making them from C++ ----

static JSValue baseExceptionNew(JSGlobalObject*, JSValue cls, const CallArgs&);
static JSValue baseExceptionInit(JSGlobalObject*, JSValue object, const CallArgs&);

static ALWAYS_INLINE bool isNative(JSValue method, JSValue (*function)(JSGlobalObject*, JSValue, const CallArgs&))
{
    auto* native = PyNativeFunction::tryFrom(method);
    return native && native->isVariadicWithSelf(function);
}

// (Not any function written in C++ that is found under the name: BaseException.add_note reads self.__notes__, which may be a property.)
static ALWAYS_INLINE bool isInitOfBuiltinType(PyNativeFunction* native)
{
    return native && native->kind() == NativeKind::SlotWrapper && native->def().name == "__init__"_s;
}

static ALWAYS_INLINE bool isInitializedByBaseException(PyType* type)
{
    JSValue init = type->lookup(Special::Init);
    if (isNative(init, baseExceptionInit)) [[likely]]
        return true;
    // A type that cannot be written to has an __init__ written in C++ that applies to it. Anything else was put into a dict behind the back of a type, which CPython
    // never sees. Nor must this: it would be refused with an exception, which is made here, and refused in its turn.
    if (type->isHeapType() || !type->isImmutable())
        return false;
    auto* native = PyNativeFunction::tryFrom(init);
    if (!isInitOfBuiltinType(native))
        return true;
    // (object's is none of theirs either: BaseException has one.)
    PyType* owner = native->ownerType();
    return !owner->base() || !type->isSubtypeOf(owner);
}

PyException* newException(JSGlobalObject* globalObject, PyType* type, std::span<const JSValue> args)
{
    // Most types of exceptions have the __new__ and the __init__ of BaseException. Then this is what comes of calling the type, and
    // nothing of the program's would be run. (One of them is RecursionError, which is made where the stack is as good as used up: a call would
    // find that out in its turn, and raise another.)
    Runtime& rt = runtime(globalObject);
    if (type->type() == rt.typeType() && type->isMadeBy(baseExceptionNew) && isInitializedByBaseException(type)) [[likely]] {
        PyException* exception = PyException::create(rt.vm(), rt.structure<PyException>(), type);
        exception->setArgs(rt.vm(), PyTuple::create(globalObject, args));
        return exception;
    }
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    JSValue result = call(globalObject, type, CallArgs(args));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (auto* exception = PyException::tryFrom(result)) [[likely]]
        return exception;
    raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, type), " should have returned an instance of BaseException, not "_s, typeName(globalObject, result));
    return nullptr;
}

PyException* newException(JSGlobalObject* globalObject, PyType* type, const String& message)
{
    JSValue argument = makeStr(globalObject, message);
    return newException(globalObject, type, std::span<const JSValue> { &argument, 1 });
}

// Of an object of JS. (0 is taken: see PyJSView.cpp.)
static constexpr unsigned jsErrorHiddenField = 1;
// The frames of Python it has passed through before there was a JSError to have the traceback.
static constexpr unsigned jsErrorTracebackHiddenField = 2;

// An object of JS keeps its exception. (Not one of Python's: the hidden fields of those belong to their types.)
static bool keepsItsException(Runtime& rt, JSValue thrown)
{
    PyType* type = rt.typeOf(thrown);
    return type == rt.jsObjectType() || type == rt.jsPlainObjectType() || type == rt.jsFunctionType();
}

// `object instanceof` the class whose prototype it is, without running code.
static bool inheritsFromPrototypeOfJS(JSObject* object, JSObject* wanted)
{
    for (JSValue prototype = object->getPrototypeDirect(); prototype.isObject(); prototype = asObject(prototype)->getPrototypeDirect()) {
        if (asObject(prototype) == wanted)
            return true;
    }
    return false;
}

// Whether it is Error.prototype, of whichever realm.
static bool isErrorPrototypeOfJS(JSObject* object)
{
    return object == object->realm()->errorPrototype();
}

// `value instanceof Error`, of whichever realm, without running code.
static bool isErrorOfJS(JSValue value)
{
    if (!value.isObject() || isPyObject(value))
        return false;
    for (JSValue prototype = asObject(value)->getPrototypeDirect(); prototype.isObject(); prototype = asObject(prototype)->getPrototypeDirect()) {
        if (isErrorPrototypeOfJS(asObject(prototype)))
            return true;
    }
    return false;
}

// ---- The classes of what JS threw ----

// The hidden fields of a class that stands for a prototype of JS's.
static constexpr unsigned prototypeOfJSField = 0; // The prototype. null: JSThrownValue, which stands for none.
static constexpr unsigned prototypeForInstancesField = 1; // See prototypeForInstancesOfExceptionClass().
static constexpr unsigned osErrorClassesField = 2; // dict: see osErrorClassFor().

static bool pendingExceptionIsForLackOfMeans(JSGlobalObject*, TopExceptionScope&);
static bool pendingExceptionIsForLackOfStack(JSGlobalObject*, TopExceptionScope&);

// Whether the host has asked for what runs on the main thread to be ended, and this is that.
static bool isBeingEnded(Runtime& rt)
{
    VM& vm = rt.vm();
    return rt.threads().isMainThread() && (vm.hasTerminationRequest() || vm.traps().needHandling(VMTraps::NeedTermination));
}

// JS lets anything be the `name` of a function. Not this long, since the class is made where nothing can be ended.
static constexpr unsigned longestNameOfMirror = 1000;

// What type() takes for the name of a class without asking anybody, and a traceback can show on its line behind "JS", with something in it that is seen. One that has no UTF-8 goes to the
// codec's handler of errors, and a program can put its own in the place of 'strict'.
static bool canNameClass(StringView name)
{
    if (name.length() > longestNameOfMirror)
        return false;
    bool hasWhatIsSeen = false;
    bool isJoiner = false;
    for (unsigned i = 0; i < name.length(); ++i) {
        bool isFirst = !i;
        char16_t unit = name[i];
        // Control characters, and what ends a line.
        if (unit < 0x20 || (unit >= 0x7f && unit < 0xa0) || unit == 0x2028 || unit == 0x2029)
            return false;
        char32_t character = unit;
        if (U16_IS_SURROGATE(unit)) {
            if (!U16_IS_SURROGATE_LEAD(unit) || i + 1 == name.length() || !U16_IS_TRAIL(name[i + 1]))
                return false;
            character = U16_GET_SUPPLEMENTARY(unit, name[++i]);
        }
        // What is not seen and changes what is: the marks that turn the direction of the text, those of no width. But for the two joiners, which
        // the grammar of JS takes within a name: Persian and the scripts of India are written with them.
        isJoiner = character == zeroWidthNonJoiner || character == zeroWidthJoiner;
        if (character >= 0x80 && u_charType(character) == U_FORMAT_CHAR && !isJoiner)
            return false;
        // A mark is seen on what it follows, and at the start that is the "S".
        bool isMark = U_GET_GC_MASK(character) & U_GC_M_MASK;
        if (isFirst && (isMark || isJoiner))
            return false;
        // (Nothing is assigned, or no more than room: blanks, the fillers of Hangul, the pattern of Braille without a dot.)
        hasWhatIsSeen |= u_isgraph(character) && !isMark && !u_hasBinaryProperty(character, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) && character != 0x2800;
    }
    return hasWhatIsSeen && !isJoiner;
}

// class name(*bases), in _js, which stands for the prototype. Null: it cannot be made. There may be next to no stack left, or memory. If it is
// not that, `canNeverBeMade` is set and nothing is raised: who gives it has something to put in its place, for good.
// To make it runs nothing of the program's: nobody can set anything in the bases, as in JSError itself (__init_subclass__), the name is one
// that canNameClass(), and what is in the namespace is of built-in types. The VM holds it to that, as it holds who looks at a property with
// VMInquiry. So there is nothing here for a termination to end: it waits, and no class is left half made.
static PyType* makeClassOfJSErrors(JSGlobalObject* globalObject, const String& name, std::initializer_list<JSValue> bases, JSValue prototypeOfJS, IfClassCannotBeMade otherwise, bool* canNeverBeMade = nullptr)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    ASSERT(canNameClass(StringView(name).substring(2)));
    DeferTerminationForAWhile deferred(vm);
    DisallowVMEntry noCodeOfTheProgram(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyDict* ns = PyDict::create(globalObject);
    ns->set(vm, rt.names.dModule(), internString(vm, "_js"_s));
    JSValue type = call(globalObject, rt.typeType(), makeStr(vm, name), PyTuple::create(globalObject, bases), ns);
    if (scope.exception()) [[unlikely]] {
        if (canNeverBeMade)
            *canNeverBeMade = !pendingExceptionIsForLackOfMeans(globalObject, scope);
        if (otherwise == IfClassCannotBeMade::NearestStandsIn || (canNeverBeMade && *canNeverBeMade))
            tryDropPendingException(scope);
        return nullptr;
    }
    PyType::from(type)->add(TypeFlag::Immutable);
    setHiddenField(globalObject, type, prototypeOfJSField, prototypeOfJS);
    return PyType::from(type);
}

// One of the classes that there is one of, made when it is first asked for. Where it cannot be made: JSError, or null.
static PyType* classOfJSErrorsKeptAs(JSGlobalObject* globalObject, JSString* key, StringView nameWithoutJS, std::initializer_list<JSValue> bases, JSValue prototypeOfJS, IfClassCannotBeMade otherwise)
{
    Runtime& rt = runtime(globalObject);
    if (!rt.slot(Runtime::Slot::JSErrorClasses))
        rt.setSlot(Runtime::Slot::JSErrorClasses, PyDict::create(globalObject));
    PyDict* classes = PyDict::from(rt.slot(Runtime::Slot::JSErrorClasses));
    if (JSValue type = classes->get(key))
        return PyType::from(type);
    PyType* type = makeClassOfJSErrors(globalObject, makeString("JS"_s, nameWithoutJS), bases, prototypeOfJS, otherwise);
    if (!type)
        return otherwise == IfClassCannotBeMade::Raise ? nullptr : rt.JSError();
    classes->set(rt.vm(), key, type);
    return type;
}

static JSObject* typeErrorPrototypeOfJS(JSGlobalObject* realm)
{
    return realm->errorStructure(ErrorType::TypeError)->storedPrototypeObject();
}

// class JSTypeError(JSError, TypeError): what stands for a TypeError of JS, so that `except TypeError` catches it. So for OSError
// and the classes derived from it.
PyType* jsErrorTypeFor(JSGlobalObject* globalObject, PyType* base, IfClassCannotBeMade otherwise)
{
    Runtime& rt = runtime(globalObject);
    return classOfJSErrorsKeptAs(globalObject, base->nameString(), base->name(), { rt.JSError(), base }, base == rt.TypeError() ? typeErrorPrototypeOfJS(globalObject) : globalObject->errorPrototype(), otherwise);
}

PyType* jsTypeErrorType(JSGlobalObject* globalObject, IfClassCannotBeMade otherwise)
{
    return jsErrorTypeFor(globalObject, runtime(globalObject).TypeError(), otherwise);
}

// class JSThrownValue(JSError): what stands for something that JS threw and that is no Error, `throw 1`. `except JSError` catches all that JS
// throws, and JSError itself stands for Error.
PyType* jsThrownValueType(JSGlobalObject* globalObject, IfClassCannotBeMade otherwise)
{
    Runtime& rt = runtime(globalObject);
    // (The others are kept under the name of their other base. This one has none.)
    return classOfJSErrorsKeptAs(globalObject, rt.JSError()->nameString(), "ThrownValue"_s, { rt.JSError() }, jsNull(), otherwise);
}

// object.name, if the object has it itself and it is a value. It runs no code of the program's. (A function makes its name and its prototype
// when they are first asked for, and the host computes DOMException.prototype.constructor.) So there is nothing to end, and a termination
// waits: a function looks what is asked of the VM while it makes them. What the host makes or computes, and a getter too, takes the main
// thread: `needsMainThread` is set where another cannot tell, and `lackedMeans` where there was no stack or memory for what the host makes.
struct CouldNotTell {
    bool needsMainThread { false };
    bool lackedMeans { false };
    bool lackedStack { false }; // With lackedMeans: that is what there was none of.
};
static void raiseForLackOfMeans(JSGlobalObject* globalObject, bool lackedStack)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    if (lackedStack)
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
    else
        raiseMemoryError(globalObject, scope, String());
}

static JSValue ownValueOfJS(JSGlobalObject* globalObject, JSObject* object, PropertyName name, CouldNotTell& couldNotTell)
{
    if (!runtime(globalObject).threads().isMainThread()) [[unlikely]]
        return ownValueOfJSForAnyThread(globalObject, object, name, couldNotTell.needsMainThread);
    VM& vm = getVM(globalObject);
    DeferTerminationForAWhile deferred(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool found = object->methodTable()->getOwnPropertySlot(object, object->realm(), name, slot);
    JSValue value;
    if (!scope.exception() && found && (slot.isValue() || slot.isCustom()))
        value = slot.getValue(object->realm(), name);
    if (scope.exception()) [[unlikely]] {
        // (An accessor of the host's throws where the prototype is not what it is for: then there is no value.)
        couldNotTell.lackedMeans |= pendingExceptionIsForLackOfMeans(globalObject, scope);
        couldNotTell.lackedStack |= pendingExceptionIsForLackOfStack(globalObject, scope);
        tryDropPendingException(scope);
        return { };
    }
    return value;
}

// The prefix and the name of the class whose prototype it is, or what it says itself (`Mine.prototype = new Error; Mine.prototype.name = "Mine"`).
// Null: it has none, or none that a class can have.
static String nameOfClassForPrototype(JSGlobalObject* globalObject, JSObject* prototype, ASCIILiteral prefix, CouldNotTell& couldNotTell)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String text;
    // (type() lets anything be the name of a class of Python's, too.)
    if (auto* view = dynamicDowncast<PyPrototypeView>(prototype))
        text = view->type()->name();
    else {
        JSValue name;
        JSValue constructor = ownValueOfJS(globalObject, prototype, vm.propertyNames->constructor, couldNotTell);
        if (constructor && constructor.isObject() && ownValueOfJS(globalObject, asObject(constructor), vm.propertyNames->prototype, couldNotTell) == JSValue(prototype))
            name = ownValueOfJS(globalObject, asObject(constructor), vm.propertyNames->name, couldNotTell);
        if (!name || !name.isString() || !asString(name)->length())
            name = ownValueOfJS(globalObject, prototype, vm.propertyNames->name, couldNotTell);
        if (!name || !name.isString() || !asString(name)->length() || asString(name)->length() > longestNameOfMirror)
            return { };
        // (It may be a rope.)
        text = asString(name)->value(globalObject);
        if (scope.exception()) [[unlikely]] {
            tryDropPendingException(scope);
            couldNotTell.lackedMeans = true;
            return { };
        }
    }
    // (The limit is for what comes behind "JS".)
    if (!canNameClass(text) || text.length() + prefix.length() - 2 > longestNameOfMirror)
        return { };
    String result = tryMakeString(prefix, text);
    couldNotTell.lackedMeans |= result.isNull();
    return result;
}

// What makeClassOfJSErrors() has put there. Empty: nothing, the class is the program's. The hidden fields of an object are its type's to use, so those
// of a class its metaclass's. The classes made here are made by `type`, and a program can put one with another metaclass under JSError: those of
// ctypes keep what they know of a type in the same field.
static JSValue prototypeOfJSIn(JSGlobalObject* globalObject, PyType* type)
{
    return type->type() == runtime(globalObject).typeType() ? hiddenField(globalObject, type, prototypeOfJSField) : JSValue();
}

// Of JSError or a class under it: whether it is of Errors, and not JSThrownValue or under that.
static bool standsForError(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    PyTuple* mro = type->mro();
    for (unsigned i = 0; i < mro->length(); ++i) {
        PyType* next = PyType::from(mro->at(i));
        if (!next->isSubtypeOf(rt.JSError()))
            continue;
        if (JSValue prototype = prototypeOfJSIn(globalObject, next); prototype && prototype.isNull())
            return false;
    }
    return true;
}

// The class of the exceptions that stand for the objects with this prototype. The classes
// mirror the chain of prototypes, that is `extends`: class JSRangeError(JSError), and class JSValidationError(JSRangeError) for
// `class ValidationError extends RangeError`. So what goes by type(e) sees what was thrown: contextlib.suppress(), assertRaises(),
// pytest.raises(), a traceback. They are made when something of the kind is first thrown or asked for, without running code of the
// program's, and live as long as the prototype.
// JS throws anything. Where no Error is on the chain it is mirrored to its end, under JSThrownValue: class JSThrownObject(JSThrownValue) for
// Object.prototype, class JSThrownResponse(JSThrownObject) for `throw new Response()`.
// Every class has an MRO of its own, so what a chain of n prototypes costs grows with the square of n, where to JS it is n objects
// (Object.create() in a loop). So a chain is mirrored this far down from its upper end, which is further than anybody writes `extends`. The
// prototypes below have the class of the last that is mirrored.
static constexpr unsigned longestMROOfMirror = 256;

// `lackedMeans` is set where another stands in because there was no stack, memory or time: a moment later it may go well.
static PyType* exceptionClassForPrototypeOfJS(JSGlobalObject* globalObject, JSObject* prototype, IfClassCannotBeMade otherwise, bool& lackedMeans)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (prototype == globalObject->errorPrototype())
        return rt.JSError();
    auto forLackOfMeans = [&](PyType* nearest) {
        lackedMeans = true;
        return otherwise == IfClassCannotBeMade::Raise ? nullptr : nearest;
    };
    JSWeakMap* classes = weakMapInSlot(globalObject, Runtime::Slot::ExceptionClassesOfJSPrototypes);
    if (JSValue known = classes->get(prototype); known.isCell()) [[likely]]
        return PyType::from(known);

    // See makeClassOfJSErrors().
    DeferTerminationForAWhile deferred(vm);
    DisallowVMEntry noCodeOfTheProgram(vm);
    // From the first on the chain that has a class, down.
    MarkedArgumentBuffer withoutClass;
    PyType* type = rt.JSError();
    for (JSObject* object = prototype; object != globalObject->errorPrototype();) {
        if (JSValue known = classes->get(object); known.isCell()) {
            type = PyType::from(known);
            break;
        }
        // (_js.JSTypeError is there for who has caught none.)
        if (object == typeErrorPrototypeOfJS(globalObject)) {
            type = jsTypeErrorType(globalObject, otherwise);
            if (!type || type == rt.JSError())
                return forLackOfMeans(type);
            classes->set(vm, object, type);
            break;
        }
        withoutClass.append(object);
        // The Error of another realm is an error, and no Error of this one.
        if (isErrorPrototypeOfJS(object))
            break;
        JSValue parent = object->getPrototypeDirect();
        if (!parent.isObject()) {
            // No Error is on the chain.
            type = jsThrownValueType(globalObject, otherwise);
            if (!type || type == rt.JSError())
                return forLackOfMeans(type);
            break;
        }
        object = asObject(parent);
    }
    // (_js.JSObject, _js.JSMap and _js.JSPromise are the types of the objects.)
    ASCIILiteral prefix = standsForError(globalObject, type) ? "JS"_s : "JSThrown"_s;
    auto forLackOfMemoryOrStack = [&](bool lackedStack) {
        if (otherwise == IfClassCannotBeMade::Raise)
            raiseForLackOfMeans(globalObject, lackedStack);
        return forLackOfMeans(type);
    };
    if (withoutClass.hasOverflowed()) [[unlikely]]
        return forLackOfMemoryOrStack(false);
    while (!withoutClass.isEmpty()) {
        if (type->mro()->length() >= longestMROOfMirror) [[unlikely]] {
            classes->set(vm, prototype, type);
            return type;
        }
        // The chain is as long as the program has made it, so what is being ended makes no more of them: it will not see what it gets. (It is
        // what runs on the main thread that a termination ends. Another thread that is to stop makes them all. So does who asks for the class
        // itself, who is given null only with an exception: there are no more than longestMROOfMirror to make.)
        if (otherwise == IfClassCannotBeMade::NearestStandsIn && isBeingEnded(rt)) [[unlikely]]
            return forLackOfMeans(type);
        JSObject* object = asObject(withoutClass.takeLast());
        CouldNotTell couldNotTell;
        String name = nameOfClassForPrototype(globalObject, object, prefix, couldNotTell);
        // A class is named once.
        if (couldNotTell.lackedMeans) [[unlikely]]
            return forLackOfMemoryOrStack(couldNotTell.lackedStack);
        if (couldNotTell.needsMainThread) [[unlikely]] {
            if (otherwise == IfClassCannotBeMade::NearestStandsIn)
                return type;
            ensureMainThreadForJS(globalObject);
            return nullptr;
        }
        PyType* typeError = nullptr;
        if (object == typeErrorPrototypeOfJS(object->realm())) {
            // The TypeError of another realm is no TypeError to this one's JS. To Python it is a TypeError that JS threw, as this one's.
            typeError = jsTypeErrorType(globalObject, otherwise);
            if (!typeError || typeError == rt.JSError())
                return forLackOfMeans(type);
            // (Somebody may have put it under this one's TypeError, or under its Error.)
            if (type->isSubtypeOf(typeError))
                typeError = nullptr;
            else if (typeError->isSubtypeOf(type))
                type = std::exchange(typeError, nullptr);
        }
        if (name.isNull())
            name = type->name();
        bool canNeverBeMade = false;
        PyType* made = typeError ? makeClassOfJSErrors(globalObject, name, { type, typeError }, object, otherwise, &canNeverBeMade) : makeClassOfJSErrors(globalObject, name, { type }, object, otherwise, &canNeverBeMade);
        if (!made) {
            if (!canNeverBeMade)
                return forLackOfMeans(type);
            made = type;
        }
        classes->set(vm, object, made);
        type = made;
    }
    return type;
}

static PyType* exceptionClassForPrototypeOfJS(JSGlobalObject* globalObject, JSObject* prototype, IfClassCannotBeMade otherwise)
{
    bool lackedMeans = false;
    return exceptionClassForPrototypeOfJS(globalObject, prototype, otherwise, lackedMeans);
}

// The first class in the MRO that stands for a prototype says what the instances are. Empty: none does. null: it is JSThrownValue.
static JSValue prototypeOfJSFor(JSGlobalObject* globalObject, PyType* type, PyType** standsFor = nullptr)
{
    Runtime& rt = runtime(globalObject);
    PyTuple* mro = type->mro();
    for (unsigned i = 0; mro && i < mro->length(); ++i) {
        PyType* next = PyType::from(mro->at(i));
        if (!next->isSubtypeOf(rt.JSError()))
            continue;
        JSValue prototype = next == rt.JSError() ? JSValue(globalObject->errorPrototype()) : prototypeOfJSIn(globalObject, next);
        if (!prototype)
            continue;
        if (standsFor)
            *standsFor = next;
        return prototype;
    }
    return { };
}

static bool exceptionClassStandsForPrototypeOfJS(JSGlobalObject* globalObject, PyType* type, JSObject* wanted)
{
    JSValue prototype = prototypeOfJSFor(globalObject, type);
    return prototype && prototype.isObject() && (asObject(prototype) == wanted || inheritsFromPrototypeOfJS(asObject(prototype), wanted));
}

// function.prototype, without running code. Null: it has none, or none that can be seen.
static JSObject* prototypeOfClassOfJSToCatch(JSGlobalObject* globalObject, JSObject* function, CouldNotTell& couldNotTell)
{
    while (auto* bound = dynamicDowncast<JSBoundFunction>(function))
        function = bound->targetFunction();
    JSValue prototype = ownValueOfJS(globalObject, function, getVM(globalObject).propertyNames->prototype, couldNotTell);
    return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
}

static JSObject* prototypeOfClassOfJSToCatch(JSGlobalObject* globalObject, JSObject* function)
{
    CouldNotTell couldNotTell;
    return prototypeOfClassOfJSToCatch(globalObject, function, couldNotTell);
}

// For who asked for a class and is to be told why there is none. False: something is raised.
static bool raiseIfCouldNotTell(JSGlobalObject* globalObject, const CouldNotTell& couldNotTell, IfClassCannotBeMade otherwise)
{
    if (otherwise != IfClassCannotBeMade::Raise)
        return true;
    if (couldNotTell.lackedMeans) {
        raiseForLackOfMeans(globalObject, couldNotTell.lackedStack);
        return false;
    }
    return !couldNotTell.needsMainThread || ensureMainThreadForJS(globalObject);
}

bool exceptionClassStandsForClassOfJS(JSGlobalObject* globalObject, PyType* type, JSObject* function)
{
    JSObject* prototype = prototypeOfClassOfJSToCatch(globalObject, function);
    return prototype && exceptionClassStandsForPrototypeOfJS(globalObject, type, prototype);
}

// JSRangeError('x'), made in Python, stands for nothing that JS has. To JS it is an exception of Python's as any other, and a RangeError:
// its prototype is an object that inherits from RangeError.prototype, as that of any other inherits from Error.prototype. Null: as any other.
static JSObject* prototypeForInstancesOfExceptionClass(JSGlobalObject* globalObject, PyType* type)
{
    PyType* standsFor = nullptr;
    JSValue prototype = prototypeOfJSFor(globalObject, type, &standsFor);
    if (!prototype || !prototype.isObject() || asObject(prototype) == globalObject->errorPrototype() || asObject(prototype) == typeErrorPrototypeOfJS(globalObject))
        return nullptr;
    if (JSValue known = hiddenField(globalObject, standsFor, prototypeForInstancesField))
        return asObject(known);
    // (JSThrownResponse('x') is no Response: an Error, as JSThrownValue('x').)
    if (!standsForError(globalObject, standsFor))
        return nullptr;
    JSObject* made = makePrototypeOfExceptionsForJS(globalObject, asObject(prototype));
    setHiddenField(globalObject, standsFor, prototypeForInstancesField, made);
    return made;
}

// The class for a class: js.RangeError, or a class of Python's that derives from it. Null: it is no class, or, if only those are wanted, it is not
// Error and does not extend it.
PyType* exceptionClassForClassOfJS(JSGlobalObject* globalObject, JSObject* cls, IfClassCannotBeMade otherwise, bool onlyOfErrors)
{
    JSObject* prototype;
    CouldNotTell couldNotTell;
    if (PyType* type = PyType::tryFrom(cls)) {
        // (_js.JSObject and _js.JSMap are the types of the objects: they stand for no class, and nothing has their `prototype`. A class that is being made has no MRO yet, and the
        // mro() of its metaclass may ask.)
        PyTuple* mro = type->mro();
        bool derivesFromClassOfJS = false;
        for (unsigned i = 0; mro && type->has(TypeFlag::AttributesAreProperties) && i < mro->length() && !derivesFromClassOfJS; ++i)
            derivesFromClassOfJS = prototypeOfClassOfJS(PyType::from(mro->at(i)));
        prototype = derivesFromClassOfJS ? prototypeForJS(globalObject, type) : nullptr;
    } else
        prototype = !isPyObject(cls) && cls->isCallable() ? prototypeOfClassOfJSToCatch(globalObject, cls, couldNotTell) : nullptr;
    if (!prototype && !raiseIfCouldNotTell(globalObject, couldNotTell, otherwise))
        return nullptr;
    if (!prototype || (onlyOfErrors && !isErrorPrototypeOfJS(prototype) && !isErrorOfJS(prototype)))
        return nullptr;
    return exceptionClassForPrototypeOfJS(globalObject, prototype, otherwise);
}

// TypeError, OSError and what is under it, told when this is compiled: of the 71 names a name is compared with these.
static constexpr bool goesWithClassOfJS(std::string_view name)
{
    constexpr std::pair<std::string_view, std::string_view> bases[] = {
#define PY_NAME_AND_BASE(id, base) { #id, #base },
        FOR_EACH_PY_EXCEPTION(PY_NAME_AND_BASE)
#undef PY_NAME_AND_BASE
    };
    if (name == "TypeError")
        return true;
    while (name != "OSError") {
        auto* found = std::ranges::find(bases, name, &std::pair<std::string_view, std::string_view>::first);
        if (found == std::end(bases))
            return false;
        name = found->second;
    }
    return true;
}
#define PY_COUNT_EXCEPTION(id, unused) + goesWithClassOfJS(#id)
static_assert(0 FOR_EACH_PY_EXCEPTION(PY_COUNT_EXCEPTION) == 17);
#undef PY_COUNT_EXCEPTION
static_assert(goesWithClassOfJS("OSError") && goesWithClassOfJS("ConnectionResetError") && !goesWithClassOfJS("ValueError") && !goesWithClassOfJS("BaseException"));

// What `_js.name` is in every process and in every thread, whatever the program has done to the globals: JSFileNotFoundError, and JSRangeError
// for the RangeError that the engine throws. (The classes that go with those of Python's come first.) Null: there is none.
static PyType* exceptionClassNamedEverywhere(JSGlobalObject* globalObject, const String& name, IfClassCannotBeMade otherwise)
{
    Runtime& rt = runtime(globalObject);
    if (!name.startsWith("JS"_s))
        return nullptr;
    StringView rest = StringView(name).substring(2);
    if (rest == "Error"_s)
        return rt.JSError();
    if (rest == "ThrownValue"_s)
        return jsThrownValueType(globalObject, otherwise);
#define PY_FIND_EXCEPTION(id, unused) \
    if constexpr (goesWithClassOfJS(#id)) { \
        if (rest == #id ""_s) \
            return jsErrorTypeFor(globalObject, rt.id(), otherwise); \
    }
    FOR_EACH_PY_EXCEPTION(PY_FIND_EXCEPTION)
#undef PY_FIND_EXCEPTION
    for (unsigned i = 0; i < NumberOfErrorType; ++i) {
        if (rest == errorTypeName(static_cast<ErrorType>(i)))
            return exceptionClassForPrototypeOfJS(globalObject, globalObject->errorStructure(static_cast<ErrorType>(i))->storedPrototypeObject(), otherwise);
    }
    return nullptr;
}

// What `_js.name` is, for a class that is made when it is first asked for: those, and JSValidationError for the ValidationError that is a global
// of JS's. Null: there is none. It runs no code of the program's.
PyType* exceptionClassNamed(JSGlobalObject* globalObject, const String& name, IfClassCannotBeMade otherwise)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = exceptionClassNamedEverywhere(globalObject, name, otherwise);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (type || !name.startsWith("JS"_s))
        return type;
    CouldNotTell couldNotTell;
    JSValue global = ownValueOfJS(globalObject, globalObject, Identifier::fromString(vm, name.substring(2)), couldNotTell);
    if (!global && failed(scope, raiseIfCouldNotTell(globalObject, couldNotTell, otherwise)))
        return nullptr;
    if (!global || !global.isObject() || isPyObject(global))
        return nullptr;
    RELEASE_AND_RETURN(scope, exceptionClassForClassOfJS(globalObject, asObject(global), otherwise, true));
}

// What stands for an error of the system whose class extends Error: class JSFileNotFoundError(JSDiskError, FileNotFoundError). JSError: it
// cannot be made.
static PyType* osErrorClassFor(JSGlobalObject* globalObject, PyType* standsFor, PyType* base)
{
    Runtime& rt = runtime(globalObject);
    if (standsFor == rt.JSError())
        return jsErrorTypeFor(globalObject, base);
    JSValue classes = hiddenField(globalObject, standsFor, osErrorClassesField);
    if (!classes) {
        classes = PyDict::create(globalObject);
        setHiddenField(globalObject, standsFor, osErrorClassesField, classes);
    }
    if (JSValue type = PyDict::from(classes)->get(base->nameString()))
        return PyType::from(type);
    PyType* type = makeClassOfJSErrors(globalObject, makeString("JS"_s, base->name()), { standsFor, base }, prototypeOfJSIn(globalObject, standsFor), IfClassCannotBeMade::NearestStandsIn);
    if (!type)
        return rt.JSError();
    PyDict::from(classes)->set(rt.vm(), base->nameString(), type);
    return type;
}

// pickle names a class by its module and its name, for another process to find. Of these classes that has only those of the errors of the
// engine. For any other it is the nearest base that is one of them, as it was JSError for all.
static PyType* classToPickleAs(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    // (A class of the program's is the program's to find.)
    if (type == rt.JSError() || !prototypeOfJSIn(globalObject, type))
        return type;
    PyTuple* mro = type->mro();
    bool isOSError = type->isSubtypeOf(rt.OSError());
    for (unsigned i = 0; i < mro->length(); ++i) {
        PyType* next = PyType::from(mro->at(i));
        if (isOSError) {
            if (!next->isHeapType() && next->isSubtypeOf(rt.OSError()))
                return jsErrorTypeFor(globalObject, next);
            continue;
        }
        if (next->isSubtypeOf(rt.JSError()) && exceptionClassNamedEverywhere(globalObject, next->name(), IfClassCannotBeMade::NearestStandsIn) == next)
            return next;
    }
    return rt.JSError();
}

static PyType* osErrorSubclassFor(Runtime&, int errorNumber);

// Whether what is pending says where the program was, not what it does: there was no stack left, no memory, or no time. The same a moment later
// may go well.
static bool pendingExceptionIsForLackOfMeans(JSGlobalObject* globalObject, TopExceptionScope& scope)
{
    Runtime& rt = runtime(globalObject);
    Exception* pending = scope.exception();
    if (rt.vm().isTerminationException(pending))
        return true;
    if (auto* error = dynamicDowncast<ErrorInstance>(pending->value()))
        return error->isStackOverflowError() || error->isOutOfMemoryError();
    auto* exception = PyException::tryFrom(pending->value());
    return exception && (exception->type()->isSubtypeOf(rt.RecursionError()) || exception->type()->isSubtypeOf(rt.MemoryError()));
}

static bool pendingExceptionIsForLackOfStack(JSGlobalObject* globalObject, TopExceptionScope& scope)
{
    JSValue pending = scope.exception()->value();
    if (auto* error = dynamicDowncast<ErrorInstance>(pending))
        return error->isStackOverflowError();
    auto* exception = PyException::tryFrom(pending);
    return exception && exception->type()->isSubtypeOf(runtime(globalObject).RecursionError());
}

// An error of the system, as Node and Bun throw them, has the name of the error number as its `code` (and the number, negated or libuv's
// own, as its `errno`). In Python that is an OSError, of the class that OSError(errno, ...) makes. `standsFor`: the class of the Error.
// Null: it is no such error, or, with `lackedMeans`, there is no telling now.
static PyException* osErrorFromThrownValue(JSGlobalObject* globalObject, JSObject* thrown, PyType* standsFor, bool& lackedMeans)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto property = [&](ASCIILiteral name) {
        if (scope.exception()) [[unlikely]]
            return JSValue();
        // (A thread other than the main one runs nothing of JS's: it sees what is there to be seen.)
        JSValue value = rt.threads().isMainThread() ? thrown->get(globalObject, Identifier::fromString(vm, name)) : valueOfJSForAnyThread(globalObject, thrown, Identifier::fromString(vm, name));
        if (scope.exception()) [[unlikely]] {
            lackedMeans |= pendingExceptionIsForLackOfMeans(globalObject, scope);
            tryDropPendingException(scope);
            return JSValue();
        }
        return value;
    };
    JSValue number = property("errno"_s);
    JSValue code = number && number.isNumber() ? property("code"_s) : JSValue();
    if (!code || !code.isString())
        return nullptr;
    auto errorNumber = errnoForName(asString(code)->view(globalObject));
    if (!errorNumber)
        return nullptr;
    JSValue path = property("path"_s);
    JSValue destination = property("dest"_s);
    // The host has ended the look.
    if (scope.exception()) [[unlikely]]
        return nullptr;
    PyType* base = osErrorSubclassFor(rt, *errorNumber);
    bool hasPath = path && path.isString();
    bool hasDestination = hasPath && destination && destination.isString();
    JSValue arguments[] = { jsNumber(*errorNumber), makeStr(vm, String::fromUTF8(strerror(*errorNumber))), path, jsUndefined(), destination };
    PyType* type = osErrorClassFor(globalObject, standsFor, base ? base : rt.OSError());
    if (type == rt.JSError()) {
        lackedMeans = true;
        return nullptr;
    }
    PyException* exception = newException(globalObject, type, std::span<const JSValue> { arguments }.first(hasDestination ? 5 : hasPath ? 3 : 2));
    if (scope.exception()) [[unlikely]] {
        lackedMeans = true;
        tryDropPendingException(scope);
    }
    return exception;
}

// What Error.prototype.toString() makes of the `name` and the `message` that are there to be seen, for a thread other than the main one, which
// runs no toString().
static String messageOfErrorForAnyThread(JSGlobalObject* globalObject, JSObject* thrown)
{
    VM& vm = getVM(globalObject);
    auto text = [&](const Identifier& key) -> String {
        JSValue value = valueOfJSForAnyThread(globalObject, thrown, key);
        return value && value.isString() ? asString(value)->tryGetValue() : String();
    };
    String name = text(vm.propertyNames->name);
    String message = text(vm.propertyNames->message);
    if (name.isEmpty() && message.isEmpty())
        return "<exception>"_s;
    if (name.isEmpty() || message.isEmpty())
        return name.isEmpty() ? message : name;
    String result = tryMakeString(name, ": "_s, message);
    return result.isNull() ? "<exception>"_s : result;
}

// What stands for the value, without its cause. `isForKeeps`: whoever asks again may get the same, if the value is one that keeps its exception.
// `isNew`: it is such a value, and this has been made now: it has no cause yet and has not been kept.
static JSValue exceptionWithoutCauseFromThrownValue(JSGlobalObject* globalObject, JSValue thrown, bool& isForKeeps, bool& isNew)
{
    isForKeeps = true;
    isNew = false;
    if (PyException::is(thrown))
        return thrown;

    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool remembers = keepsItsException(rt, thrown);
    if (remembers) {
        if (JSValue existing = hiddenField(globalObject, thrown, jsErrorHiddenField))
            return existing;
    }
    PyException* exception;
    auto* error = dynamicDowncast<ErrorInstance>(thrown);
    if (error && error->isStackOverflowError())
        exception = newException(globalObject, rt.RecursionError(), "maximum recursion depth exceeded"_s);
    else if (error && error->isOutOfMemoryError())
        exception = newException(globalObject, rt.MemoryError(), std::span<const JSValue> { });
    else {
        // What it is, is asked before anything of the program's runs, which may change it. (To ask runs nothing that would have to be ended.)
        bool isError = isErrorOfJS(thrown);
        // (A Proxy has none but what its handler says.)
        JSValue prototype = thrown.isObject() ? asObject(thrown)->getPrototypeDirect() : jsNull();
        // Where there is no stack, memory or time to make it what it should be, here or below, this is an exception for now. Who asks later gets another.
        // (Not so where a thread cannot see what the class is called: it never will, and what it catches, changes and raises again is to stay what it is.)
        bool lackedMeans = false;
        PyType* type = prototype.isObject() ? exceptionClassForPrototypeOfJS(globalObject, asObject(prototype), IfClassCannotBeMade::NearestStandsIn, lackedMeans) : jsThrownValueType(globalObject);
        lackedMeans |= !isError && type == rt.JSError();
        String message;
        {
            // Its properties and its toString() are code of the program, which the unwinder may have come to: a generator asks what is leaving it.
            TerminationNotDeferred canBeEnded(globalObject);
            exception = isError ? osErrorFromThrownValue(globalObject, asObject(thrown), type, lackedMeans) : nullptr;
            if (!exception && !scope.exception())
                message = rt.threads().isMainThread() || !thrown.isObject() ? thrown.toWTFString(globalObject) : messageOfErrorForAnyThread(globalObject, asObject(thrown));
            if (scope.exception()) {
                lackedMeans |= pendingExceptionIsForLackOfMeans(globalObject, scope);
                dropPendingException(globalObject, scope);
                exception = nullptr;
                message = "<exception>"_s;
            }
        }
        if (!exception) {
            // A termination that was dropped has been asked for again. It is not this that it is to end: whoever wants to know what was thrown is told.
            DeferTerminationForAWhile deferred(vm);
            exception = newException(globalObject, type, message);
        }
        if (!failed(scope, exception))
            exception->setField(vm, ExceptionField::jsErrorValue, thrown);
        isForKeeps = !lackedMeans;
    }
    if (failed(scope, exception)) [[unlikely]]
        return { };
    if (remembers) {
        if (JSValue traceback = hiddenField(globalObject, thrown, jsErrorTracebackHiddenField))
            exception->setTraceback(vm, traceback);
        isNew = true;
    }
    return exception;
}

// From now on the exception is what stands for the value. Unless the value has got one meanwhile: its toString() may have had it caught, or have
// let another thread catch it. Then that is it.
static JSValue keepExceptionFor(JSGlobalObject* globalObject, JSValue thrown, JSValue exception)
{
    if (JSValue existing = hiddenField(globalObject, thrown, jsErrorHiddenField)) [[unlikely]]
        return existing;
    setHiddenField(globalObject, thrown, jsErrorHiddenField, exception);
    return exception;
}

// new Error(message, { cause }) is `raise ... from cause`, if the cause is something that can be raised. A chain of causes is as long as the
// program has made it, so it is gone down in a loop: how far is not to depend on how much stack there is.
// One that is kept has the cause that is kept. So nothing is kept before all of the chain has been made: what is made of one link runs code of the
// program's (toString()), which may catch another link, or something that has one for its cause, and would build on what is dropped again if a
// link further down is not for keeps.
JSValue exceptionFromThrownValue(JSGlobalObject* globalObject, JSValue thrown)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    bool isForKeeps;
    bool isNew;
    JSValue result = exceptionWithoutCauseFromThrownValue(globalObject, thrown, isForKeeps, isNew);
    if (!result || !isNew || !isForKeeps) [[unlikely]]
        return result;
    auto causeOf = [&](JSValue value) {
        JSValue cause = asObject(value)->getDirect(vm, vm.propertyNames->cause);
        return cause && (PyException::is(cause) || isErrorOfJS(cause)) ? cause : JSValue();
    };
    JSValue cause = causeOf(thrown);
    if (!cause) [[likely]]
        return keepExceptionFor(globalObject, thrown, result);

    // What was thrown and its causes, as far as they are new, and what stands for each.
    MarkedArgumentBuffer values;
    MarkedArgumentBuffer exceptions;
    // (Causes may go in a circle.)
    UncheckedKeyHashMap<JSCell*, unsigned> positions;
    JSValue value = thrown;
    JSValue exception = result;
    while (true) {
        positions.add(value.asCell(), values.size());
        values.append(value);
        exceptions.append(exception);
        if (values.hasOverflowed() || exceptions.hasOverflowed()) [[unlikely]]
            return result;
        if (!cause)
            break;
        JSValue causeInPython;
        bool causeIsForKeeps = true;
        isNew = false;
        if (auto position = positions.find(cause.asCell()); position != positions.end())
            causeInPython = exceptions.at(position->value);
        else {
            // (As in exceptionClassForPrototypeOfJS(): what is being ended will not see what it gets.)
            if (!isBeingEnded(rt)) [[likely]]
                causeInPython = exceptionWithoutCauseFromThrownValue(globalObject, cause, causeIsForKeeps, isNew);
        }
        if (causeInPython) {
            PyException::from(exception)->setCause(vm, causeInPython);
            PyException::from(exception)->setSuppressContext(true);
        }
        if (!causeInPython || !causeIsForKeeps) [[unlikely]]
            return result;
        // (What is not new is an exception of Python's, or one that is kept, with its causes, or one of these here.)
        if (!isNew)
            break;
        value = cause;
        exception = causeInPython;
        cause = causeOf(value);
    }
    for (unsigned i = 0; i < values.size(); ++i) {
        JSValue kept = keepExceptionFor(globalObject, values.at(i), exceptions.at(i));
        if (kept == exceptions.at(i)) [[likely]]
            continue;
        // What it has got meanwhile has its causes, and they are kept.
        if (!i)
            return kept;
        PyException::from(exceptions.at(i - 1))->setCause(vm, kept);
        break;
    }
    return result;
}

bool isClassOfJSToCatch(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    return value.isObject() && rt.typeOf(value) == rt.jsFunctionType() && isClassOfJS(value.asCell());
}

bool standsForInstanceOfJS(JSGlobalObject* globalObject, JSValue value, JSValue classOfJS)
{
    Runtime& rt = runtime(globalObject);
    auto* exception = PyException::tryFrom(value);
    if (!exception || !exception->type()->isSubtypeOf(rt.JSError()))
        return false;
    JSValue thrown = exception->field(ExceptionField::jsErrorValue);
    if (thrown && !thrown.isObject())
        return false;
    JSObject* prototype = prototypeOfClassOfJSToCatch(globalObject, asObject(classOfJS));
    if (!prototype)
        return false;
    // One that was made in Python stands for nothing: its class says what it is.
    if (!thrown)
        return exceptionClassStandsForPrototypeOfJS(globalObject, exception->type(), prototype);
    return inheritsFromPrototypeOfJS(asObject(thrown), prototype);
}

// `raise js.RangeError("x")`, `raise js.RangeError`: what stands for the error, which is what is thrown. Empty: it is no Error of JS and
// no class of them, or an exception.
static JSValue exceptionForRaisedErrorOfJS(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isClassOfJSToCatch(globalObject, value)) {
        JSObject* prototype = prototypeOfClassOfJSToCatch(globalObject, asObject(value));
        if (!prototype || (!isErrorPrototypeOfJS(prototype) && !isErrorOfJS(prototype)))
            return { };
        value = call(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isErrorOfJS(value))
        return { };
    RELEASE_AND_RETURN(scope, exceptionFromThrownValue(globalObject, value));
}

bool addTracebackEntryToThrownObject(JSGlobalObject* globalObject, JSValue thrown, const ScopedLambda<JSValue(JSValue next)>& makeEntry)
{
    if (!thrown.isObject())
        return false;
    Runtime& rt = runtime(globalObject);
    if (!keepsItsException(rt, thrown))
        return false;
    if (JSValue entry = makeEntry(hiddenField(globalObject, thrown, jsErrorTracebackHiddenField)))
        setHiddenField(globalObject, thrown, jsErrorTracebackHiddenField, entry);
    return true;
}

// The same for a frame of JS, of which there is an entry only if the value is an exception of Python's, stands for one, or has been through
// code of Python's. False: it is none of these.
bool addTracebackEntryForJSToThrownValue(JSGlobalObject* globalObject, JSValue thrown, const ScopedLambda<JSValue(JSValue next)>& makeEntry)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* exception = PyException::tryFrom(thrown);
    if (!exception) {
        if (!thrown.isObject() || !keepsItsException(rt, thrown))
            return false;
        exception = PyException::tryFrom(hiddenField(globalObject, thrown, jsErrorHiddenField));
    }
    if (exception) {
        DeferGC deferGC(vm);
        exception->setTraceback(vm, makeEntry(exception->traceback()));
        return true;
    }
    JSValue next = hiddenField(globalObject, thrown, jsErrorTracebackHiddenField);
    if (!next)
        return false;
    DeferGC deferGC(vm);
    setHiddenField(globalObject, thrown, jsErrorTracebackHiddenField, makeEntry(next));
    return true;
}

PyException* exceptionOfThrownValueIfKnown(JSGlobalObject* globalObject, JSValue thrown)
{
    if (auto* exception = PyException::tryFrom(thrown))
        return exception;
    if (!thrown.isObject())
        return nullptr;
    Runtime& rt = runtime(globalObject);
    if (!keepsItsException(rt, thrown))
        return nullptr;
    return PyException::tryFrom(hiddenField(globalObject, thrown, jsErrorHiddenField));
}

// ---- The raise statement ----

bool isMadeWithoutCodeOfTheProgram(JSGlobalObject* globalObject, PyType* type)
{
    if (!type->has(TypeFlag::Heap) && !type->has(TypeFlag::Mutable)) [[likely]]
        return true;
    return type->type() == runtime(globalObject).typeType() && type->maker() && isInitOfBuiltinType(PyNativeFunction::tryFrom(type->lookup(Special::Init))) && !type->lookup(Special::Del);
}

void raiseStatement(JSGlobalObject* globalObject, JSValue exception, JSValue cause, JSValue callee)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!exception) {
        // Again, what is being handled.
        JSValue handled = rt.handledException();
        if (!handled) {
            raiseRuntimeError(globalObject, scope, "No active exception to reraise"_s);
            return;
        }
        throwPythonException(globalObject, scope, handled);
        return;
    }

    auto isExceptionClass = [](JSValue value) {
        PyType* type = PyType::tryFrom(value);
        return type && type->has(TypeFlag::ExceptionSubclass);
    };

    JSValue value;
    if (isExceptionClass(exception)) {
        value = call(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, void());
        if (!PyException::is(value)) {
            raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, exception), " should have returned an instance of BaseException, not "_s, reprForMessage(globalObject, rt.typeOf(value)));
            return;
        }
        callee = exception;
    } else if (PyException::is(exception))
        value = exception;
    else {
        value = exceptionForRaisedErrorOfJS(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value) {
            raiseTypeError(globalObject, scope, "exceptions must derive from BaseException"_s);
            return;
        }
    }

    if (cause) {
        JSValue fixedCause;
        if (isExceptionClass(cause)) {
            fixedCause = call(globalObject, cause);
            RETURN_IF_EXCEPTION(scope, void());
            if (!PyException::is(fixedCause)) {
                raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, cause), " should have returned an instance of BaseException, not "_s, reprForMessage(globalObject, rt.typeOf(fixedCause)));
                return;
            }
        } else if (PyException::is(cause))
            fixedCause = cause;
        else if (!isNone(cause)) {
            fixedCause = exceptionForRaisedErrorOfJS(globalObject, cause);
            RETURN_IF_EXCEPTION(scope, void());
            if (!fixedCause) {
                raiseTypeError(globalObject, scope, "exception causes must derive from BaseException"_s);
                return;
            }
        }
        PyException::from(value)->setCause(vm, fixedCause);
        PyException::from(value)->setSuppressContext(true);
        callee = { };
    }
    // The class was called, and this is what its own __new__ made: nothing else has it.
    if (callee && callee == JSValue(PyException::from(value)->type()))
        raiseNewException(globalObject, scope, PyException::from(value));
    else
        raiseException(globalObject, scope, value);
}

// ---- BaseException ----

static PyException* self(JSValue value) { return PyException::from(value); }

static JSValue noneIfEmpty(JSValue value) { return value ? value : jsUndefined(); }

static JSValue baseExceptionNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), PyType::from(cls));
    exception->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    return exception;
}

static bool rejectKeywords(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!args.hasKeywords())
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, typeName(globalObject, object), "() takes no keyword arguments"_s);
    return false;
}

static JSValue baseExceptionInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, object, args)))
        return { };
    // (__new__ has just made a tuple of the same.)
    auto positional = args.positional();
    if (PyTuple* existing = self(object)->args(); existing && std::ranges::equal(existing->span(), positional))
        return jsUndefined();
    self(object)->setArgs(vm, PyTuple::create(globalObject, positional));
    return jsUndefined();
}

static JSValue baseExceptionStr(JSGlobalObject* globalObject, JSValue object)
{
    PyTuple* args = self(object)->args();
    switch (args->length()) {
    case 0:
        return runtime(globalObject).names.empty();
    case 1:
        return strObject(globalObject, args->at(0));
    default:
        return str(globalObject, args);
    }
}

static JSValue baseExceptionRepr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* args = self(object)->args();
    // Without the module, even where messages have it: "Error('x')" for a binascii.Error.
    String name = typeOf(globalObject, object)->name();
    if (args->length() == 1) {
        JSString* argument = repr(globalObject, args->at(0));
        RETURN_IF_EXCEPTION(scope, { });
        String text = argument->value(globalObject);
        return makeStr(vm, makeString(name, '(', text, ')'));
    }
    JSString* arguments = repr(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    String text = arguments->value(globalObject);
    return makeStr(vm, makeString(name, text));
}

static JSValue baseExceptionReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    if (exception->dict())
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), JSValue(exception->dict()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()) });
}

static JSValue baseExceptionSetState(JSGlobalObject* globalObject, JSValue object, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(state))
        return jsUndefined();
    auto* dict = PyDict::tryFrom(state);
    if (!dict)
        return raiseTypeError(globalObject, scope, "state is not a dictionary"_s);
    JSValue key, value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        if (!isStr(key))
            return raiseTypeError(globalObject, scope, "attribute name must be string, not '"_s, typeName(globalObject, key), '\'');
        setAttr(globalObject, object, strValue(key), value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static void setTraceback(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "__traceback__ may not be deleted"_s);
        return;
    }
    if (isNone(value)) {
        self(object)->setTraceback(vm, { });
        return;
    }
    if (!PyTraceback::is(value)) {
        raiseTypeError(globalObject, scope, "__traceback__ must be a traceback or None"_s);
        return;
    }
    self(object)->setTraceback(vm, value);
}

static JSValue baseExceptionWithTraceback(JSGlobalObject* globalObject, JSValue object, JSValue traceback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    setTraceback(globalObject, object, traceback);
    RETURN_IF_EXCEPTION(scope, { });
    return object;
}

static JSValue baseExceptionAddNote(JSGlobalObject* globalObject, JSValue object, JSValue note)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(note))
        return raiseTypeError(globalObject, scope, "add_note() argument must be str, not "_s, isNone(note) ? String("None"_s) : typeName(globalObject, note));
    JSValue notes = getAttrIfPresent(globalObject, object, rt.names.dNotes());
    RETURN_IF_EXCEPTION(scope, { });
    if (!notes) {
        notes = newList(globalObject);
        setAttr(globalObject, object, rt.names.dNotes(), notes);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (!isList(notes))
        return raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s);
    listAppend(globalObject, asList(notes), note);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue getArgs(JSGlobalObject*, JSValue object) { return self(object)->args(); }

static void setArgs(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "args may not be deleted"_s);
        return;
    }
    PyTuple* tuple = tupleFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    self(object)->setArgs(vm, tuple);
}

static JSValue getTraceback(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->traceback()); }
static JSValue getContext(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->context()); }
static JSValue getCause(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->cause()); }
static JSValue getSuppressContext(JSGlobalObject*, JSValue object) { return jsBoolean(self(object)->suppressContext()); }

static bool checkExceptionOrNone(JSGlobalObject* globalObject, JSValue& value, ASCIILiteral what, ASCIILiteral description)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, what, " may not be deleted"_s);
        return false;
    }
    if (isNone(value)) {
        value = JSValue();
        return true;
    }
    if (!PyException::is(value)) {
        raiseTypeError(globalObject, scope, "exception "_s, description, " must be None or derive from BaseException"_s);
        return false;
    }
    return true;
}

static void setContext(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    if (checkExceptionOrNone(globalObject, value, "__context__"_s, "context"_s))
        self(object)->setContext(getVM(globalObject), value);
}

static void setCause(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    if (checkExceptionOrNone(globalObject, value, "__cause__"_s, "cause"_s)) {
        self(object)->setCause(getVM(globalObject), value);
        self(object)->setSuppressContext(true);
    }
}

static void setSuppressContext(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isBoolean()) {
        raiseTypeError(globalObject, scope, value ? "attribute value type must be bool"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    self(object)->setSuppressContext(value.asBoolean());
}

// The attributes that are kept in the fields.
template<unsigned index>
static JSValue getField(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->field(index)); }

template<unsigned index>
static void setField(JSGlobalObject* globalObject, JSValue object, JSValue value) { self(object)->setField(getVM(globalObject), index, value); }

#define PY_EXCEPTION_FIELD(name, index) NativeGetSet { name ""_s, getField<ExceptionField::index>, setField<ExceptionField::index>, true }

static constexpr NativeDef baseExceptionMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", baseExceptionInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", baseExceptionStr, "($self, /)"),
    PY_SLOT("__repr__", baseExceptionRepr, "($self, /)"),
    PY_METHOD("__reduce__", baseExceptionReduce, "($self, /)"),
    PY_METHOD("__setstate__", baseExceptionSetState, "($self, state, /)"),
    PY_METHOD("with_traceback", baseExceptionWithTraceback, "($self, tb, /)"),
    PY_METHOD("add_note", baseExceptionAddNote, "($self, note, /)"),
};

static constexpr NativeGetSet baseExceptionGetSets[] = {
    { "args"_s, getArgs, setArgs },
    { "__traceback__"_s, getTraceback, setTraceback },
    { "__context__"_s, getContext, setContext },
    { "__cause__"_s, getCause, setCause },
    PY_WRITABLE_MEMBER("__suppress_context__", getSuppressContext, setSuppressContext),
};

// ---- StopIteration, SystemExit ----

static JSValue stopIterationInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!baseExceptionInit(globalObject, object, args))
        return { };
    self(object)->setField(getVM(globalObject), ExceptionField::stopIterationValue, args.size() ? args[0] : jsUndefined());
    return jsUndefined();
}

static constexpr NativeDef stopIterationMethods[] = {
    PY_SLOT("__init__", stopIterationInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet stopIterationGetSets[] = {
    PY_EXCEPTION_FIELD("value", stopIterationValue),
};

static JSValue systemExitInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!baseExceptionInit(globalObject, object, args))
        return { };
    JSValue code = !args.size() ? jsUndefined() : args.size() == 1 ? args[0] : JSValue(self(object)->args());
    self(object)->setField(getVM(globalObject), ExceptionField::systemExitCode, code);
    return jsUndefined();
}

static constexpr NativeDef systemExitMethods[] = {
    PY_SLOT("__init__", systemExitInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet systemExitGetSets[] = {
    PY_EXCEPTION_FIELD("code", systemExitCode),
};

// ---- Those that take keyword arguments ----

// False: an exception. Each keyword goes in the field that goes with its name.
static bool initWithKeywords(JSGlobalObject* globalObject, ASCIILiteral className, JSValue object, const CallArgs& args, std::initializer_list<std::pair<ASCIILiteral, unsigned>> keywords)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    if (args.keywordCount() > keywords.size()) {
        raiseTypeError(globalObject, scope, className, "() takes at most "_s, keywords.size(), " keyword argument"_s, keywords.size() == 1 ? ""_s : "s"_s, " ("_s, args.keywordCount(), " given)"_s);
        return false;
    }
    exception->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    for (auto& keyword : keywords)
        exception->setField(vm, keyword.second, { });
    if (args.keywordCount() > keywords.size()) {
        raiseTypeError(globalObject, scope, className, "() takes at most "_s, static_cast<unsigned>(keywords.size()), " keyword argument"_s, keywords.size() == 1 ? ""_s : "s"_s, " ("_s, args.keywordCount(), " given)"_s);
        return false;
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        StringImpl* name = args.keywordName(i)->tryGetValueImpl();
        bool found = false;
        for (auto& keyword : keywords) {
            if (WTF::equal(name, keyword.first.span8())) {
                exception->setField(vm, keyword.second, args.keywordValue(i));
                found = true;
                break;
            }
        }
        if (!found) {
            Vector<ASCIILiteral, 4> names;
            for (auto& keyword : keywords)
                names.append(keyword.first);
            scope.release();
            raiseUnexpectedKeyword(globalObject, className, String(name), names.span());
            return false;
        }
    }
    return true;
}

static JSValue importErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "ImportError"_s, object, args, { { "name"_s, ExceptionField::importErrorName }, { "path"_s, ExceptionField::importErrorPath }, { "name_from"_s, ExceptionField::importErrorNameFrom } })))
        return { };
    self(object)->setField(getVM(globalObject), ExceptionField::importErrorMsg, args.size() == 1 ? args[0] : jsUndefined());
    return jsUndefined();
}

static JSValue importErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    JSValue message = self(object)->field(ExceptionField::importErrorMsg);
    if (message && isStr(message))
        return strValue(message);
    return baseExceptionStr(globalObject, object);
}

// ImportError('message', name='module', path='file')
static JSValue importErrorRepr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue base = baseExceptionRepr(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = exception->field(ExceptionField::importErrorName);
    JSValue path = exception->field(ExceptionField::importErrorPath);
    if (!name && !path)
        return base;
    String text = asString(base)->value(globalObject);
    StrBuilder builder;
    builder.append(StringView(text).left(text.length() - 1));
    bool needsComma = !!exception->args()->length();
    auto append = [&](ASCIILiteral keyword, JSValue value) {
        if (!value)
            return true;
        JSString* string = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        builder.append(needsComma ? ", "_s : ""_s, keyword, '=', String(string->value(globalObject)));
        needsComma = true;
        return true;
    };
    if (!append("name"_s, name) || !append("path"_s, path))
        return { };
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// The __dict__ and, under their names, the fields that are set. None: there is neither.
static JSValue stateWithFields(JSGlobalObject* globalObject, PyException* exception, std::initializer_list<std::pair<ASCIILiteral, JSValue>> fields)
{
    VM& vm = getVM(globalObject);
    PyDict* state = nullptr;
    for (auto& field : fields) {
        if (!field.second)
            continue;
        if (!state)
            state = exception->dict() ? exception->dict()->copy(globalObject) : PyDict::create(globalObject);
        state->set(vm, internString(vm, field.first), field.second);
    }
    if (state)
        return state;
    return exception->dict() ? JSValue(exception->dict()) : jsUndefined();
}

static JSValue importErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    JSValue state = stateWithFields(globalObject, exception, { { "name"_s, exception->field(ExceptionField::importErrorName) }, { "path"_s, exception->field(ExceptionField::importErrorPath) }, { "name_from"_s, exception->field(ExceptionField::importErrorNameFrom) } });
    if (isNone(state))
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), state });
}

static constexpr NativeDef importErrorMethods[] = {
    PY_SLOT("__init__", importErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", importErrorRepr, "($self, /)"),
    PY_SLOT("__str__", importErrorStr, "($self, /)"),
    PY_METHOD("__reduce__", importErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet importErrorGetSets[] = {
    PY_EXCEPTION_FIELD("msg", importErrorMsg),
    PY_EXCEPTION_FIELD("name", importErrorName),
    PY_EXCEPTION_FIELD("path", importErrorPath),
    PY_EXCEPTION_FIELD("name_from", importErrorNameFrom),
};

static JSValue nameErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "NameError"_s, object, args, { { "name"_s, ExceptionField::nameErrorName } })))
        return { };
    return jsUndefined();
}

static constexpr NativeDef nameErrorMethods[] = {
    PY_SLOT("__init__", nameErrorInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet nameErrorGetSets[] = {
    PY_EXCEPTION_FIELD("name", nameErrorName),
};

static JSValue attributeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "AttributeError"_s, object, args, { { "name"_s, ExceptionField::attributeErrorName }, { "obj"_s, ExceptionField::attributeErrorObj } })))
        return { };
    return jsUndefined();
}

// Without obj, which often cannot be pickled.
static JSValue attributeErrorGetState(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    return stateWithFields(globalObject, exception, { { "name"_s, exception->field(ExceptionField::attributeErrorName) }, { "args"_s, exception->args() } });
}

static JSValue attributeErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), attributeErrorGetState(globalObject, object) });
}

static constexpr NativeDef attributeErrorMethods[] = {
    PY_SLOT("__init__", attributeErrorInit, "($self, *args, **kwargs)"),
    PY_METHOD("__getstate__", attributeErrorGetState, "($self, /)"),
    PY_METHOD("__reduce__", attributeErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet attributeErrorGetSets[] = {
    PY_EXCEPTION_FIELD("name", attributeErrorName),
    PY_EXCEPTION_FIELD("obj", attributeErrorObj),
};

// ---- KeyError ----

static JSValue keyErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    // KeyError('') would otherwise show as nothing at all.
    PyTuple* args = self(object)->args();
    if (args->length() == 1)
        return reprObject(globalObject, args->at(0));
    return baseExceptionStr(globalObject, object);
}

static constexpr NativeDef keyErrorMethods[] = {
    PY_SLOT("__str__", keyErrorStr, "($self, /)"),
};

// ---- OSError ----

static PyType* osErrorSubclassFor(Runtime& rt, int errorNumber)
{
    switch (errorNumber) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EALREADY:
    case EINPROGRESS:
        return rt.BlockingIOError();
    case EPIPE:
    case ESHUTDOWN:
        return rt.BrokenPipeError();
    case ECHILD:
        return rt.ChildProcessError();
    case ECONNABORTED:
        return rt.ConnectionAbortedError();
    case ECONNREFUSED:
        return rt.ConnectionRefusedError();
    case ECONNRESET:
        return rt.ConnectionResetError();
    case EEXIST:
        return rt.FileExistsError();
    case ENOENT:
        return rt.FileNotFoundError();
    case EISDIR:
        return rt.IsADirectoryError();
    case ENOTDIR:
        return rt.NotADirectoryError();
    case EINTR:
        return rt.InterruptedError();
    case EACCES:
    case EPERM:
        return rt.PermissionError();
    case ESRCH:
        return rt.ProcessLookupError();
    case ETIMEDOUT:
        return rt.TimeoutError();
    default:
        return nullptr;
    }
}

// False: an exception.
static bool osErrorInitialize(JSGlobalObject* globalObject, PyException* exception, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = args.size();
    std::span<const JSValue> kept = args.positional();
    if (count >= 2 && count <= 5) {
        exception->setField(vm, ExceptionField::osErrorErrno, args[0]);
        exception->setField(vm, ExceptionField::osErrorStrerror, args[1]);
        if (count >= 3) {
            JSValue filename = args[2];
            PyType* filenameType = rt.typeOf(filename);
            bool isNumber = filenameType->lookup(Special::Index) || filenameType->lookup(Special::Int) || filenameType->lookup(Special::Float);
            if (exception->type() == rt.BlockingIOError() && isNumber) {
                // In place of a file name, how many characters were written. Nothing is then taken out of the args.
                int64_t written = toSsize(globalObject, filename, rt.ValueError());
                RETURN_IF_EXCEPTION(scope, false);
                // -1 is CPython's mark for "not set".
                if (written != -1)
                    exception->setField(vm, ExceptionField::osErrorCharactersWritten, makeInt(globalObject, written));
            } else if (!isNone(filename)) {
                exception->setField(vm, ExceptionField::osErrorFilename, filename);
                if (count == 5 && !isNone(args[4]))
                    exception->setField(vm, ExceptionField::osErrorFilename2, args[4]);
                // The file names are not among the args, for compatibility.
                kept = kept.first(2);
            }
        }
    }
    exception->setArgs(vm, PyTuple::create(globalObject, kept));
    return true;
}

// A subclass that has an __init__ of its own, and not a __new__, gets to see the arguments first.
static bool osErrorUsesInit(JSGlobalObject* globalObject, PyType* type)
{
    PyType* base = runtime(globalObject).OSError();
    return type != base && type->lookup(Special::Init) != base->lookup(Special::Init) && type->isMadeAs(base);
}

static JSValue osErrorNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    bool usesInit = osErrorUsesInit(globalObject, type);
    if (!usesInit) {
        if (args.hasKeywords())
            return raiseTypeError(globalObject, scope, type->name(), "() takes no keyword arguments"_s);
        if (type == rt.OSError() && args.size() >= 2 && args.size() <= 5 && isInt(args[0])) {
            auto number = tryInt64(intValue(args[0]));
            if (PyType* subclass = number && *number == static_cast<int>(*number) ? osErrorSubclassFor(rt, static_cast<int>(*number)) : nullptr)
                type = subclass;
        }
    }
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), type);
    if (usesInit)
        exception->setArgs(vm, rt.emptyTuple());
    else if (failed(scope, osErrorInitialize(globalObject, exception, args)))
        return { };
    return exception;
}

static JSValue osErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!osErrorUsesInit(globalObject, self(object)->type()))
        return jsUndefined();
    if (failed(scope, rejectKeywords(globalObject, object, args)))
        return { };
    if (failed(scope, osErrorInitialize(globalObject, self(object), args)))
        return { };
    return jsUndefined();
}

static JSValue osErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue errorNumber = exception->field(ExceptionField::osErrorErrno);
    JSValue message = exception->field(ExceptionField::osErrorStrerror);
    JSValue filename = exception->field(ExceptionField::osErrorFilename);
    JSValue filename2 = exception->field(ExceptionField::osErrorFilename2);

    auto text = [&](JSValue value, bool useRepr) -> String {
        JSString* string = useRepr ? repr(globalObject, noneIfEmpty(value)) : str(globalObject, noneIfEmpty(value));
        if (!string)
            return { };
        return string->value(globalObject);
    };
    if (filename) {
        String first = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String second = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        String third = text(filename, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (filename2) {
            String fourth = text(filename2, true);
            RETURN_IF_EXCEPTION(scope, { });
            return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second, ": "_s, third, " -> "_s, fourth));
        }
        return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second, ": "_s, third));
    }
    if (errorNumber && message) {
        String first = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String second = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second));
    }
    RELEASE_AND_RETURN(scope, baseExceptionStr(globalObject, object));
}

static JSValue osErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    PyTuple* args = exception->args();
    JSValue filename = exception->field(ExceptionField::osErrorFilename);
    if (args->length() == 2 && filename) {
        JSValue filename2 = exception->field(ExceptionField::osErrorFilename2);
        if (filename2)
            args = PyTuple::create(globalObject, { args->at(0), args->at(1), filename, jsUndefined(), filename2 });
        else
            args = PyTuple::create(globalObject, { args->at(0), args->at(1), filename });
    }
    if (exception->dict())
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(args), JSValue(exception->dict()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(args) });
}

static JSValue getCharactersWritten(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = self(object)->field(ExceptionField::osErrorCharactersWritten);
    if (!value)
        return raise(globalObject, scope, runtime(globalObject).AttributeError(), "characters_written"_s);
    return value;
}

static void setCharactersWritten(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    if (!value) {
        if (!exception->field(ExceptionField::osErrorCharactersWritten)) {
            raise(globalObject, scope, rt.AttributeError(), "characters_written"_s);
            return;
        }
        exception->setField(vm, ExceptionField::osErrorCharactersWritten, { });
        return;
    }
    int64_t count = toSsize(globalObject, value, rt.ValueError());
    RETURN_IF_EXCEPTION(scope, void());
    // -1 is how CPython says that there is none.
    exception->setField(vm, ExceptionField::osErrorCharactersWritten, count == -1 ? JSValue() : makeInt(globalObject, count));
}

static constexpr NativeDef osErrorMethods[] = {
    PY_NEW(osErrorNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", osErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", osErrorStr, "($self, /)"),
    PY_METHOD("__reduce__", osErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet osErrorGetSets[] = {
    PY_EXCEPTION_FIELD("errno", osErrorErrno),
    PY_EXCEPTION_FIELD("strerror", osErrorStrerror),
    PY_EXCEPTION_FIELD("filename", osErrorFilename),
    PY_EXCEPTION_FIELD("filename2", osErrorFilename2),
    { "characters_written"_s, getCharactersWritten, setCharactersWritten },
};

// ---- SyntaxError ----

static JSValue syntaxErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, baseExceptionInit(globalObject, object, args)))
        return { };
    PyException* exception = self(object);
    if (args.size() >= 1)
        exception->setField(vm, ExceptionField::syntaxErrorMsg, args[0]);
    if (args.size() == 2) {
        PyTuple* info = tupleFromIterable(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned length = info->length();
        if (length < 4 || length > 7)
            return raiseTypeError(globalObject, scope, "function takes "_s, length < 4 ? "at least 4"_s : "at most 7"_s, " arguments ("_s, length, " given)"_s);
        if (length == 5)
            return raiseTypeError(globalObject, scope, "end_offset must be provided when end_lineno is provided"_s);
        exception->setField(vm, ExceptionField::syntaxErrorFilename, info->at(0));
        exception->setField(vm, ExceptionField::syntaxErrorLineno, info->at(1));
        exception->setField(vm, ExceptionField::syntaxErrorOffset, info->at(2));
        exception->setField(vm, ExceptionField::syntaxErrorText, info->at(3));
        exception->setField(vm, ExceptionField::syntaxErrorEndLineno, length >= 6 ? info->at(4) : JSValue());
        exception->setField(vm, ExceptionField::syntaxErrorEndOffset, length >= 6 ? info->at(5) : JSValue());
        exception->setField(vm, ExceptionField::syntaxErrorMetadata, length == 7 ? info->at(6) : JSValue());
    }
    return jsUndefined();
}

static JSValue syntaxErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSString* message = str(globalObject, noneIfEmpty(exception->field(ExceptionField::syntaxErrorMsg)));
    RETURN_IF_EXCEPTION(scope, { });

    JSValue filename = exception->field(ExceptionField::syntaxErrorFilename);
    JSValue lineNumber = exception->field(ExceptionField::syntaxErrorLineno);
    bool hasFilename = filename && isStr(filename);
    bool hasLineNumber = lineNumber && isExactInt(lineNumber);
    if (!hasFilename && !hasLineNumber)
        return message;

    String text = message->value(globalObject);
    String baseName;
    if (hasFilename) {
        baseName = strValue(filename)->value(globalObject);
        size_t slash = baseName.reverseFind('/');
        if (slash != notFound)
            baseName = baseName.substring(slash + 1);
    }
    // The line is shown as a number of C, which is -1 if it does not fit.
    String line = hasLineNumber ? (tryInt64(lineNumber) ? intToString(globalObject, lineNumber) : String("-1"_s)) : String();
    if (hasFilename && hasLineNumber)
        return makeStr(vm, makeString(text, " ("_s, baseName, ", line "_s, line, ')'));
    if (hasFilename)
        return makeStr(vm, makeString(text, " ("_s, baseName, ')'));
    return makeStr(vm, makeString(text, " (line "_s, line, ')'));
}

static constexpr NativeDef syntaxErrorMethods[] = {
    PY_SLOT("__init__", syntaxErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", syntaxErrorStr, "($self, /)"),
};
static constexpr NativeGetSet syntaxErrorGetSets[] = {
    PY_EXCEPTION_FIELD("msg", syntaxErrorMsg),
    PY_EXCEPTION_FIELD("filename", syntaxErrorFilename),
    PY_EXCEPTION_FIELD("lineno", syntaxErrorLineno),
    PY_EXCEPTION_FIELD("offset", syntaxErrorOffset),
    PY_EXCEPTION_FIELD("text", syntaxErrorText),
    PY_EXCEPTION_FIELD("end_lineno", syntaxErrorEndLineno),
    PY_EXCEPTION_FIELD("end_offset", syntaxErrorEndOffset),
    PY_EXCEPTION_FIELD("print_file_and_line", syntaxErrorPrintFileAndLine),
    PY_EXCEPTION_FIELD("_metadata", syntaxErrorMetadata),
};

// ---- UnicodeError ----

static JSValue unicodeErrorInitCommon(JSGlobalObject* globalObject, JSValue object, const CallArgs& args, bool hasEncoding, bool isDecoding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, baseExceptionInit(globalObject, object, args)))
        return { };
    unsigned expected = hasEncoding ? 5 : 4;
    if (args.size() != expected)
        return raiseTypeError(globalObject, scope, "function takes exactly "_s, expected, " arguments ("_s, args.size(), " given)"_s);
    unsigned next = 0;
    auto takeStr = [&]() -> JSValue {
        JSValue value = args[next++];
        if (!isStr(value))
            return raiseTypeError(globalObject, scope, "argument "_s, next, " must be str, not "_s, isNone(value) ? String("None"_s) : typeName(globalObject, value));
        return value;
    };
    auto takeIndex = [&]() -> JSValue {
        int64_t value;
        if (failed(scope, ssizeArgument(globalObject, args[next++], value)))
            return { };
        return makeInt(globalObject, value);
    };
    JSValue encoding;
    if (hasEncoding) {
        encoding = takeStr();
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue target = isDecoding ? args[next++] : takeStr();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue start = takeIndex();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue end = takeIndex();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue reason = takeStr();
    RETURN_IF_EXCEPTION(scope, { });
    if (isDecoding && !PyBytes::is(target)) {
        // What could change is copied.
        std::span<const uint8_t> bytes;
        if (failed(scope, getBytesLike(globalObject, target, bytes)))
            return { };
        target = PyBytes::create(globalObject, bytes);
    }
    PyException* exception = self(object);
    exception->setField(vm, ExceptionField::unicodeErrorEncoding, encoding);
    exception->setField(vm, ExceptionField::unicodeErrorObject, target);
    exception->setField(vm, ExceptionField::unicodeErrorStart, start);
    exception->setField(vm, ExceptionField::unicodeErrorEnd, end);
    exception->setField(vm, ExceptionField::unicodeErrorReason, reason);
    return jsUndefined();
}

static JSValue unicodeEncodeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, true, false); }
static JSValue unicodeDecodeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, true, true); }
static JSValue unicodeTranslateErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, false, false); }

static String escapedCharacter(char32_t ch)
{
    if (ch <= 0xff)
        return makeString("\\x"_s, hex(ch, 2, Lowercase));
    if (ch <= 0xffff)
        return makeString("\\u"_s, hex(ch, 4, Lowercase));
    return makeString("\\U"_s, hex(ch, 8, Lowercase));
}

static JSValue unicodeErrorStrCommon(JSGlobalObject* globalObject, JSValue object, int kind)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue target = exception->field(ExceptionField::unicodeErrorObject);
    if (!target)
        RELEASE_AND_RETURN(scope, runtime(globalObject).names.empty());

    // What __init__ has not set shows as it does in CPython: null pointers and zeros.
    auto textOf = [&](unsigned field) -> String {
        JSValue value = exception->field(field);
        if (!value)
            return "<NULL>"_s;
        JSString* string = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        return string->value(globalObject);
    };
    auto positionOf = [&](unsigned field) -> int64_t {
        JSValue value = exception->field(field);
        return value ? toSsize(globalObject, value, nullptr) : 0;
    };
    String reason = textOf(ExceptionField::unicodeErrorReason);
    RETURN_IF_EXCEPTION(scope, { });
    String encoding;
    if (kind != 2) {
        encoding = textOf(ExceptionField::unicodeErrorEncoding);
        RETURN_IF_EXCEPTION(scope, { });
    }
    int64_t start = positionOf(ExceptionField::unicodeErrorStart);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t end = positionOf(ExceptionField::unicodeErrorEnd);
    RETURN_IF_EXCEPTION(scope, { });

    // A __str__ called above may have done anything to the exception.
    target = exception->field(ExceptionField::unicodeErrorObject);
    if (!target)
        return raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute is not set"_s);
    if (kind == 1 ? !PyBytes::is(target) : !isStr(target))
        return raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute must be a "_s, kind == 1 ? "bytes"_s : "string"_s);

    if (kind == 1) {
        auto bytes = PyBytes::from(target)->span();
        if (start >= 0 && start < static_cast<int64_t>(bytes.size()) && end == start + 1)
            return makeStr(vm, makeString('\'', encoding, "' codec can't decode byte 0x"_s, hex(bytes[start], 2, Lowercase), " in position "_s, start, ": "_s, reason));
        return makeStr(vm, makeString('\'', encoding, "' codec can't decode bytes in position "_s, start, '-', end - 1, ": "_s, reason));
    }

    ASCIILiteral verb = kind ? "translate"_s : "encode"_s;
    String prefix = kind ? String("can't "_s) : makeString('\'', encoding, "' codec can't "_s);
    if (isStr(target)) {
        StrView view(globalObject, strValue(target));
        RETURN_IF_EXCEPTION(scope, { });
        if (start >= 0 && start < view.length() && end == start + 1)
            return makeStr(vm, makeString(prefix, verb, " character '"_s, escapedCharacter(view.at(static_cast<unsigned>(start))), "' in position "_s, start, ": "_s, reason));
    }
    return makeStr(vm, makeString(prefix, verb, " characters in position "_s, start, '-', end - 1, ": "_s, reason));
}

static JSValue unicodeEncodeErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 0); }
static JSValue unicodeDecodeErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 1); }
static JSValue unicodeTranslateErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 2); }

static constexpr NativeDef unicodeEncodeErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeEncodeErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeEncodeErrorStr, "($self, /)"),
};
static constexpr NativeDef unicodeDecodeErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeDecodeErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeDecodeErrorStr, "($self, /)"),
};
static constexpr NativeDef unicodeTranslateErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeTranslateErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeTranslateErrorStr, "($self, /)"),
};
// start and end are numbers of C in CPython.
template<unsigned index>
static void setPositionField(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isInt(value)) {
        raiseTypeError(globalObject, scope, value ? "an integer is required"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, value, position)))
        return;
    self(object)->setField(vm, index, makeInt(globalObject, position));
}

static constexpr NativeGetSet unicodeErrorGetSets[] = {
    PY_EXCEPTION_FIELD("encoding", unicodeErrorEncoding),
    PY_EXCEPTION_FIELD("object", unicodeErrorObject),
    PY_WRITABLE_MEMBER("start", getField<ExceptionField::unicodeErrorStart>, setPositionField<ExceptionField::unicodeErrorStart>),
    PY_WRITABLE_MEMBER("end", getField<ExceptionField::unicodeErrorEnd>, setPositionField<ExceptionField::unicodeErrorEnd>),
    PY_EXCEPTION_FIELD("reason", unicodeErrorReason),
};

// ---- JSError: what JS code threw ----

static constexpr NativeGetSet jsErrorGetSets[] = {
    PY_EXCEPTION_FIELD("value", jsErrorValue),
};

// What the exception has not, the object that was thrown may have: error.code, error.message, error.cause.
static JSValue jsErrorGetAttr(JSGlobalObject* globalObject, JSValue object, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue thrown = self(object)->field(ExceptionField::jsErrorValue);
    if (thrown && thrown.isObject() && typeOf(globalObject, thrown)->isSubtypeOf(runtime(globalObject).jsObjectType())) {
        JSValue property = attributeFromPropertyOfJS(globalObject, asObject(thrown), name);
        RETURN_IF_EXCEPTION(scope, { });
        if (property)
            return property;
    }
    return raiseAttributeError(globalObject, scope, object, name, makeString('\'', typeName(globalObject, object), "' object has no attribute '"_s, stringOf(name), '\''));
}

// That is for Python. JS has what was thrown itself, error.value, and is shown the exception as any other: its class, its message.
JSValue attributeOfExceptionForJS(JSGlobalObject* globalObject, PyException* exception, JSString* name)
{
    PyType* type = exception->type();
    if (isNative(type->lookup(Special::GetAttr), jsErrorGetAttr) && isNative(type->lookup(Special::GetAttribute), objectGetAttribute))
        return genericGetAttr(globalObject, exception, name, true);
    return getAttrIfPresent(globalObject, exception, name);
}

// super().__reduce__(): that of OSError or ImportError, if it is one of these too.
static JSValue reduceAsNextClassDoes(JSGlobalObject* globalObject, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = loadSuperAttribute(globalObject, rt.superType(), rt.JSError(), object, rt.names.dReduce(), false, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, method));
}

// That, but for the class: see classToPickleAs().
static JSValue jsErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = self(object)->type();
    JSValue reduced = reduceAsNextClassDoes(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* named = classToPickleAs(globalObject, type);
    if (named == type || !PyTuple::is(reduced) || !PyTuple::from(reduced)->length())
        return reduced;
    PyTuple* tuple = PyTuple::from(reduced);
    PyTuple* result = PyTuple::create(globalObject, tuple->length());
    result->initializeAt(vm, 0, named);
    for (unsigned i = 1; i < tuple->length(); ++i)
        result->initializeAt(vm, i, tuple->at(i));
    return result;
}

// A copy stays in the process, where the class is at hand: what copy.copy() and copy.deepcopy() do where there is no __copy__(), step by step, with
// the class as it is. `memo`: what deepcopy() has given, or None for copy().
static JSValue copyOfJSError(JSGlobalObject* globalObject, JSValue object, bool isDeep, JSValue memo)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = self(object)->type();
    // (e.__deepcopy__(None): _reconstruct() takes a memo that is None for "not deep".)
    if (isDeep && isNone(memo))
        memo = PyDict::create(globalObject);
    JSValue module = importModule(globalObject, "copy"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue table = getAttr(globalObject, module, internString(vm, "dispatch_table"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue reductor = callMethod(globalObject, table, rt.names.get(), type);
    RETURN_IF_EXCEPTION(scope, { });
    bool isRegistered = !isNone(reductor);
    if (isRegistered && isDeep) {
        isRegistered = isTrue(globalObject, reductor);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue reduced;
    if (isRegistered)
        reduced = call(globalObject, reductor, object);
    else {
        reductor = getAttrIfPresent(globalObject, object, rt.names.dReduceEx());
        RETURN_IF_EXCEPTION(scope, { });
        if (reductor && !isNone(reductor))
            reduced = call(globalObject, reductor, jsNumber(4));
        else {
            reductor = getAttrIfPresent(globalObject, object, rt.names.dReduce());
            RETURN_IF_EXCEPTION(scope, { });
            bool canReduce = reductor && isTrue(globalObject, reductor);
            RETURN_IF_EXCEPTION(scope, { });
            if (!canReduce) {
                JSValue error = getAttr(globalObject, module, internString(vm, "Error"_s));
                RETURN_IF_EXCEPTION(scope, { });
                JSString* name = str(globalObject, type);
                RETURN_IF_EXCEPTION(scope, { });
                JSValue exception = call(globalObject, error, makeStr(vm, makeString(isDeep ? "un(deep)copyable object of type "_s : "un(shallow)copyable object of type "_s, stringOf(name))));
                RETURN_IF_EXCEPTION(scope, { });
                return raiseException(globalObject, scope, exception);
            }
            reduced = call(globalObject, reductor);
        }
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (isStr(reduced))
        return object;
    JSValue reconstruct = getAttr(globalObject, module, internString(vm, "_reconstruct"_s));
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(object);
    arguments.append(memo);
    // The class, where jsErrorReduce() has put another in its place. What the program has registered for the class, or has given the instance for a
    // __reduce_ex__() or a __reduce__(), says what it means. (Nothing can be set in a class that is named by another.)
    JSValue named;
    auto hasItsOwn = [&](JSString* name) {
        PyDict* dict = self(object)->dict();
        JSValue own = dict ? dict->get(name) : JSValue();
        return own && !isNone(own);
    };
    if (!isRegistered && !hasItsOwn(rt.names.dReduceEx()) && !hasItsOwn(rt.names.dReduce()))
        named = classToPickleAs(globalObject, type);
    forEach(globalObject, reduced, [&](JSValue part) {
        arguments.append(arguments.size() == 2 && part == named ? JSValue(type) : part);
        return !arguments.hasOverflowed();
    });
    if (scope.exception()) [[unlikely]] {
        // _reconstruct(x, memo, *reduced)
        PyType* typeOfReduced = rt.typeOf(reduced);
        if (pendingExceptionMatches(globalObject, rt.TypeError()) && !typeOfReduced->lookup(Special::Iter) && !typeOfReduced->lookup(Special::GetItem)) {
            takePendingException(globalObject);
            raiseTypeError(globalObject, scope, "Value after * must be an iterable, not "_s, typeOfReduced->name());
        }
        return { };
    }
    if (arguments.hasOverflowed()) [[unlikely]]
        return raiseMemoryError(globalObject, scope, String());
    RELEASE_AND_RETURN(scope, call(globalObject, reconstruct, CallArgs(valuesOf(arguments), arguments.size())));
}

static JSValue jsErrorCopy(JSGlobalObject* globalObject, JSValue object) { return copyOfJSError(globalObject, object, false, jsUndefined()); }
static JSValue jsErrorDeepCopy(JSGlobalObject* globalObject, JSValue object, JSValue memo) { return copyOfJSError(globalObject, object, true, memo); }

static constexpr NativeDef jsErrorMethods[] = {
    PY_SLOT("__getattr__", jsErrorGetAttr, "($self, name, /)"),
    PY_METHOD("__reduce__", jsErrorReduce, "($self, /)"),
    PY_METHOD("__copy__", jsErrorCopy, "($self, /)"),
    PY_METHOD("__deepcopy__", jsErrorDeepCopy, "($self, memo, /)"),
};

// ---- The types ----

// In CPython every type whose instances are laid out as those of BaseException has a __new__ of its own.
static constexpr NativeDef plainExceptionMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
};

void initializeExceptionTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);

    static constexpr OptionSet<TypeFlag> flags { TypeFlag::Base, TypeFlag::HasDict };
    static constexpr OptionSet<TypeFlag> flagsWithFields { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::OwnLayout };

    auto define = [&](PyType* type, ASCIILiteral name, PyType* base) {
        TypeDef def { name, flags, { }, { }, { } };
        if (type == rt.BaseException()) {
            def.flags = flagsWithFields;
            def.flags.add(TypeFlag::ExceptionSubclass);
            def.methods = baseExceptionMethods;
            def.getsets = baseExceptionGetSets;
        } else if (type == rt.StopIteration()) {
            def.flags = flagsWithFields;
            def.methods = stopIterationMethods;
            def.getsets = stopIterationGetSets;
        } else if (type == rt.SystemExit()) {
            def.flags = flagsWithFields;
            def.methods = systemExitMethods;
            def.getsets = systemExitGetSets;
        } else if (type == rt.ImportError()) {
            def.flags = flagsWithFields;
            def.methods = importErrorMethods;
            def.getsets = importErrorGetSets;
        } else if (type == rt.NameError()) {
            def.flags = flagsWithFields;
            def.methods = nameErrorMethods;
            def.getsets = nameErrorGetSets;
        } else if (type == rt.AttributeError()) {
            def.flags = flagsWithFields;
            def.methods = attributeErrorMethods;
            def.getsets = attributeErrorGetSets;
        } else if (type == rt.KeyError())
            def.methods = keyErrorMethods;
        else if (type == rt.OSError()) {
            def.flags = flagsWithFields;
            def.methods = osErrorMethods;
            def.getsets = osErrorGetSets;
        } else if (type == rt.SyntaxError()) {
            def.flags = flagsWithFields;
            def.methods = syntaxErrorMethods;
            def.getsets = syntaxErrorGetSets;
        } else if (type == rt.UnicodeEncodeError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeEncodeErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.UnicodeDecodeError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeDecodeErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.UnicodeTranslateError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeTranslateErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.JSError()) {
            def.name = "_js.JSError"_s;
            def.methods = jsErrorMethods;
            def.getsets = jsErrorGetSets;
        } else if (type == rt.IncompleteInputError())
            def.name = "_IncompleteInputError"_s;
        else if (base->solidBase() == rt.BaseException() && type != rt.BaseExceptionGroup() && type != rt.ExceptionGroup())
            def.methods = plainExceptionMethods;
        type->initializeBuiltin(globalObject, def, rt.typeType(), base);
    };

#define PY_INITIALIZE_EXCEPTION(id, base) define(rt.id(), #id ""_s, exceptionBase_##base(rt));
#define PY_DEFINE_BASE_GETTER(id, base) [[maybe_unused]] auto exceptionBase_##id = [](Runtime& rt) { return rt.id(); };
    [[maybe_unused]] auto exceptionBase_object = [](Runtime& rt) { return rt.objectType(); };
    FOR_EACH_PY_EXCEPTION(PY_DEFINE_BASE_GETTER)
    FOR_EACH_PY_EXCEPTION(PY_INITIALIZE_EXCEPTION)
#undef PY_DEFINE_BASE_GETTER
#undef PY_INITIALIZE_EXCEPTION

    initializeExceptionGroupTypes(globalObject);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
