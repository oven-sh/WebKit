/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSStruct.h"

#include "ArgList.h"
#include "BuiltinNames.h"
#include "CallData.h"
#include "FunctionExecutable.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSSharedStruct.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

namespace JSC {

// A struct declaration (see StructDefinitionEvaluation in the proposal) is compiled as a class whose elements the
// class machinery evaluates, and whose constructor is a native function made here. It carries, in private
// properties that user code cannot name:
//   @structShared       whether this is a shared struct; every struct constructor has it, nothing else does
//   @structFields       a null-prototype object (see createFieldList) with the property keys of the public fields that
//                       this struct declares, in order, once each
//   @structParent       whether the struct has a heritage (its constructor kind is derived); the parent is read from the
//                       prototype of the constructor when an instance is made, as the proposal says
//   @structInitializer  the constructor method that the struct body declares, called with the instance as this value
//                       after the fields are set up (undefined if the body has none)
//   @structSetup        the function that installs the brand of the private methods of this struct and defines its
//                       private fields as undefined (absent if there is nothing to set up)
//   @instanceFieldInitializer  the function that runs the field initializers of this struct, made by the class
//                       machinery (absent if there is nothing to run)
//   @structSource       the source text of the declaration, for Function.prototype.toString
// The prototype has @structParent: the constructor. Every instance of an unshared struct has @structBrand: its
// constructor, or a null-prototype list of the constructors of its chain (see createFieldList) if it has more than one.
// A method that a struct body declares is wrapped in a function that carries the original in @structMethod and the
// constructor of its struct in @structBrand, and checks the receiver.
//
// structDefine(operation, ...) is called by the code of the class declaration (ClassExprNode::emitBytecode):
//   0 create(superclass, hasHeritage, shared, name, sourceText): the constructor function
//   1 link(constructor, prototype)
//   2 field(constructor, key): a public instance field; a repeated key is one field
//   3 finish(constructor, prototype, initializer): the instance side is complete, and its methods are branded
//   4 seal(constructor, prototype)
//   5 superConstructor(homeObject): the prototype of the constructor method, which super(...) calls the constructor
//     method of
//   6 wrapPrivateMethod(prototype, method): a private method of a struct, branded like the others
//   7 superThunk(superConstructor): the function that super(...) calls; when it is called (the arguments are evaluated by
//     then) it checks the super constructor and calls its constructor method
static JSC_DECLARE_HOST_FUNCTION(structCallWithoutNew);
static JSC_DECLARE_HOST_FUNCTION(structConstruct);
static JSC_DECLARE_HOST_FUNCTION(structBrandedCall);
static JSC_DECLARE_HOST_FUNCTION(structSuperCall);

static bool isSharedStructConstructor(VM& vm, JSObject* constructor)
{
    return constructor->getDirect(vm, vm.propertyNames->builtinNames().structSharedPrivateName()).isTrue();
}

// The field list of a struct is kept in a plain object with a null prototype that only this file can reach (an
// array would let Array.prototype and its setters observe or change it). Its elements are indexed own properties,
// read and written without consulting any prototype, and `length` is an own data property.
static JSObject* createFieldList(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* list = JSFinalObject::create(vm, JSFinalObject::createStructure(vm, globalObject, jsNull(), JSFinalObject::defaultInlineCapacity));
    list->putDirect(vm, vm.propertyNames->length, jsNumber(0), static_cast<unsigned>(PropertyAttribute::DontEnum));
    return list;
}

static unsigned fieldListLength(VM& vm, JSObject* list)
{
    JSValue length = list->getDirect(vm, vm.propertyNames->length);
    return length.isNumber() ? static_cast<unsigned>(length.asNumber()) : 0;
}

static JSObject* fieldListOf(VM& vm, JSObject* constructor)
{
    JSValue list = constructor->getDirect(vm, vm.propertyNames->builtinNames().structFieldsPrivateName());
    RELEASE_ASSERT(list && list.isObject());
    return asObject(list);
}

static JSValue fieldListGet(JSGlobalObject* globalObject, JSObject* list, unsigned index)
{
    return list->getDirectIndex(globalObject, index);
}

static void fieldListAppend(JSGlobalObject* globalObject, JSObject* list, JSValue key)
{
    VM& vm = globalObject->vm();
    unsigned length = fieldListLength(vm, list);
    list->putDirectIndex(globalObject, length, key);
    list->putDirect(vm, vm.propertyNames->length, jsNumber(length + 1), static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// Every instance of an unshared struct has a @structBrand property that holds its most derived constructor. A
// receiver is an instance of a struct if its brand is that struct's constructor or has it among its @structParent
// chain, so instances of subclasses pass, an object that merely inherits from the prototype does not.
static bool structHasBrand(JSGlobalObject* globalObject, JSValue receiver, JSObject* structConstructor)
{
    VM& vm = globalObject->vm();
    JSObject* object = receiver.getObject();
    if (!object)
        return false;
    JSValue brand = object->getDirect(vm, vm.propertyNames->builtinNames().structBrandPrivateName());
    if (!brand || !brand.isObject())
        return false;
    JSObject* brandObject = asObject(brand);
    if (brandObject == structConstructor)
        return true;
    if (brandObject->inherits<JSFunction>())
        return false;
    unsigned count = fieldListLength(vm, brandObject);
    for (unsigned i = 0; i < count; ++i) {
        if (fieldListGet(globalObject, brandObject, i) == JSValue(structConstructor))
            return true;
    }
    return false;
}

JSC_DEFINE_HOST_FUNCTION(structBrandedCall, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* callee = callFrame->jsCallee();
    JSValue original = callee->getDirect(vm, builtinNames.structMethodPrivateName());
    JSValue structConstructor = callee->getDirect(vm, builtinNames.structBrandPrivateName());
    RELEASE_ASSERT(original && original.isObject() && structConstructor && structConstructor.isObject());

    if (!structHasBrand(globalObject, callFrame->thisValue(), asObject(structConstructor)))
        return throwVMTypeError(globalObject, scope, "A struct method was called on an object that is not an instance of the struct"_s);

    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    auto callData = JSC::getCallData(original);
    RELEASE_AND_RETURN(scope, JSValue::encode(JSC::call(globalObject, original, callData, callFrame->thisValue(), arguments)));
}

// A function with the name of `original` that checks the receiver first. It has no length of its own.
static JSFunction* wrapStructMethod(JSGlobalObject* globalObject, JSObject* original, JSObject* structConstructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSValue nameValue = original->get(globalObject, vm.propertyNames->name);
    RETURN_IF_EXCEPTION(scope, nullptr);
    String name;
    if (nameValue.isString()) {
        name = asString(nameValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    JSFunction* wrapper = JSFunction::create(vm, globalObject, 0, name, structBrandedCall, ImplementationVisibility::Public);
    DeletePropertySlot deleteSlot;
    wrapper->methodTable()->deleteProperty(wrapper, globalObject, vm.propertyNames->length, deleteSlot);
    RETURN_IF_EXCEPTION(scope, nullptr);
    wrapper->putDirect(vm, builtinNames.structMethodPrivateName(), original, 0);
    wrapper->putDirect(vm, builtinNames.structBrandPrivateName(), structConstructor, 0);
    return wrapper;
}

// Only a method that is declared as `name(parameters) { body }` is wrapped; accessors, generators and async methods
// are ordinary functions (DefineMethod applies StructMethodCreate to that production only).
static bool isPlainMethod(JSValue value)
{
    JSFunction* function = dynamicDowncast<JSFunction>(value);
    if (!function || function->isHostOrBuiltinFunction())
        return false;
    return function->jsExecutable()->parseMode() == SourceParseMode::MethodMode;
}

// Replaces every plain method of a struct prototype by a wrapper that throws a TypeError for a receiver that is not
// an instance of the struct, then calls the original with the same receiver and arguments.
static void brandStructMethods(JSGlobalObject* globalObject, JSObject* prototype, JSObject* structConstructor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    prototype->methodTable()->getOwnPropertyNames(prototype, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (size_t i = 0; i < names.size(); ++i) {
        Identifier name = names[i];
        if (name == vm.propertyNames->constructor)
            continue;
        PropertyDescriptor descriptor;
        bool found = prototype->getOwnPropertyDescriptor(globalObject, name, descriptor);
        RETURN_IF_EXCEPTION(scope, void());
        if (!found || descriptor.isAccessorDescriptor() || !isPlainMethod(descriptor.value()))
            continue;

        JSFunction* wrapper = wrapStructMethod(globalObject, asObject(descriptor.value()), structConstructor);
        RETURN_IF_EXCEPTION(scope, void());
        PropertyDescriptor replacement;
        replacement.setValue(wrapper);
        prototype->methodTable()->defineOwnProperty(prototype, globalObject, name, replacement, true);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSC_DEFINE_HOST_FUNCTION(structCallWithoutNew, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Struct constructors must be called with new"_s);
}

// The [[Construct]] of a struct constructor function: makes the instance with all its fields (undefined), makes it
// sealed, runs the field initializers of the struct chain (the parent first) and then the constructor method of
// the struct. The return value of that method is ignored.
JSC_DEFINE_HOST_FUNCTION(structConstruct, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* constructor = callFrame->jsCallee();
    if (callFrame->newTarget() != JSValue(constructor)) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "A struct constructor can only be constructed as the new.target"_s);

    bool shared = isSharedStructConstructor(vm, constructor);

    // The struct chain, the constructor first: the parent of a derived struct is the prototype of its constructor at
    // this moment, as long as it is a struct of the same kind.
    Vector<JSObject*, 4> chain;
    chain.append(constructor);
    for (JSObject* current = constructor; current->getDirect(vm, builtinNames.structParentPrivateName()).isTrue();) {
        JSValue parent = current->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        JSObject* parentObject = parent.getObject();
        if (!parentObject)
            break;
        JSValue parentShared = parentObject->getDirect(vm, builtinNames.structSharedPrivateName());
        if (!parentShared || parentShared.isTrue() != shared)
            break;
        chain.append(parentObject);
        current = parentObject;
    }

    // The public fields of the chain, the parent's first, each once (a field that is declared again is one field).
    Vector<JSValue, 16> keys;
    for (size_t i = chain.size(); i--;) {
        JSObject* own = fieldListOf(vm, chain[i]);
        unsigned count = fieldListLength(vm, own);
        for (unsigned j = 0; j < count; ++j) {
            JSValue key = fieldListGet(globalObject, own, j);
            if (!key || !(key.isString() || key.isSymbol()))
                return throwVMTypeError(globalObject, scope, "A struct has a field that is not named by a string or a symbol"_s);
            bool seen = false;
            if (chain.size() > 1) {
                for (JSValue existing : keys) {
                    if (JSValue::strictEqual(globalObject, existing, key)) {
                        seen = true;
                        break;
                    }
                }
            }
            if (!seen)
                keys.append(key);
        }
    }
    unsigned fieldCount = keys.size();

    JSObject* object = nullptr;
    if (shared) {
        // A shared struct is an exotic object over a new thread-safe block with every field undefined. Its shape
        // lists the fields as 's', the length and the name of a string, or 'y' and the address of the SharedSymbol of
        // a symbol (see SharedStructShape).
        StringBuilder names;
        for (unsigned i = 0; i < fieldCount; ++i) {
            JSValue key = keys[i];
            if (key.isString()) {
                String name = asString(key)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                names.append("s"_s, name.length(), ":"_s, name);
            } else {
                Ref<SharedSymbol> sharedSymbol = sharedSymbolOf(globalObject, asSymbol(key));
                String address = String::number(reinterpret_cast<uintptr_t>(sharedSymbol.ptr()));
                names.append("y"_s, address.length(), ":"_s, address);
            }
        }
        auto block = SharedStructBlock::create(SharedStructShape::intern(names.toString()));
        object = wrapSharedStructBlock(globalObject, WTF::move(block));
    } else {
        JSValue prototypeValue = constructor->getDirect(vm, vm.propertyNames->prototype);
        JSObject* prototype = prototypeValue.isObject() ? asObject(prototypeValue) : globalObject->objectPrototype();
        unsigned inlineCapacity = std::min<unsigned>(fieldCount + 1, JSFinalObject::maxInlineCapacity);
        object = constructEmptyObject(globalObject, prototype, inlineCapacity);

        for (unsigned i = 0; i < fieldCount; ++i) {
            Identifier identifier = keys[i].toPropertyKey(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (auto index = parseIndex(identifier))
                object->putDirectIndex(globalObject, index.value(), jsUndefined(), static_cast<unsigned>(PropertyAttribute::DontDelete), PutDirectIndexShouldThrow);
            else
                object->putDirect(vm, identifier, jsUndefined(), static_cast<unsigned>(PropertyAttribute::DontDelete));
            RETURN_IF_EXCEPTION(scope, { });
        }

        // The brands that the methods of the struct check: the constructors of the chain. They are not an own property
        // that any key can name.
        JSValue brand = constructor;
        if (chain.size() > 1) {
            JSObject* brands = createFieldList(globalObject);
            for (size_t i = 0; i < chain.size(); ++i)
                fieldListAppend(globalObject, brands, chain[i]);
            brand = brands;
        }
        object->putDirect(vm, builtinNames.structBrandPrivateName(), brand, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));

        // The private methods and fields of the chain are set up with the fields, before the instance is sealed and
        // before any initializer runs.
        for (size_t i = chain.size(); i--;) {
            JSValue setup = chain[i]->getDirect(vm, builtinNames.structSetupPrivateName());
            if (!setup || !setup.isObject())
                continue;
            MarkedArgumentBuffer noArguments;
            auto callData = JSC::getCallData(setup);
            JSC::call(globalObject, setup, callData, object, noArguments);
            RETURN_IF_EXCEPTION(scope, { });
        }

        bool prevented = JSObject::preventExtensions(object, globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!prevented)
            return throwVMTypeError(globalObject, scope, "Cannot seal the struct instance"_s);
    }

    // The field initializers of the chain, the parent first.
    for (size_t i = chain.size(); i--;) {
        JSValue fieldInitializer = chain[i]->getDirect(vm, builtinNames.instanceFieldInitializerPrivateName());
        if (!fieldInitializer || !fieldInitializer.isObject())
            continue;
        MarkedArgumentBuffer noArguments;
        auto callData = JSC::getCallData(fieldInitializer);
        JSC::call(globalObject, fieldInitializer, callData, object, noArguments);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // The constructor method is called, not constructed: new.target is undefined in it and its result is dropped.
    JSValue initializer = constructor->getDirect(vm, builtinNames.structInitializerPrivateName());
    if (initializer && initializer.isObject()) {
        MarkedArgumentBuffer arguments;
        for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
            arguments.append(callFrame->uncheckedArgument(i));
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return { };
        }
        auto callData = JSC::getCallData(initializer);
        JSC::call(globalObject, initializer, callData, object, arguments);
        RETURN_IF_EXCEPTION(scope, { });
    }

    return JSValue::encode(object);
}

static EncodedJSValue structCreate(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSValue superclass = callFrame->argument(1);
    bool hasHeritage = callFrame->argument(2).asBoolean();
    bool shared = callFrame->argument(3).asBoolean();
    String name = asString(callFrame->argument(4))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue source = callFrame->argument(5);

    // `extends null` has no parent: the constructor stays a child of Function.prototype.
    JSObject* parent = nullptr;
    if (hasHeritage && !superclass.isNull()) {
        JSObject* superObject = superclass.getObject();
        JSValue parentShared = superObject ? superObject->getDirect(vm, builtinNames.structSharedPrivateName()) : JSValue();
        if (!parentShared || parentShared.isTrue() != shared)
            return throwVMTypeError(globalObject, scope, shared ? "A shared struct can only extend another shared struct"_s : "A struct can only extend another struct"_s);
        parent = superObject;
    }

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, name, structCallWithoutNew, ImplementationVisibility::Public, NoIntrinsic, structConstruct);
    if (parent)
        constructor->setPrototypeDirect(vm, parent);

    JSObject* fields = createFieldList(globalObject);
    constructor->putDirect(vm, builtinNames.structSharedPrivateName(), jsBoolean(shared), 0);
    constructor->putDirect(vm, builtinNames.structFieldsPrivateName(), fields, 0);
    constructor->putDirect(vm, builtinNames.structParentPrivateName(), jsBoolean(hasHeritage), 0);
    constructor->putDirect(vm, builtinNames.structInitializerPrivateName(), jsUndefined(), 0);
    // Function.prototype.toString answers the source text of the declaration.
    if (source.isString())
        constructor->putDirect(vm, builtinNames.structSourcePrivateName(), source, 0);
    return JSValue::encode(constructor);
}

static EncodedJSValue structLink(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* constructor = asObject(callFrame->argument(1));
    JSObject* prototype = asObject(callFrame->argument(2));
    bool shared = isSharedStructConstructor(vm, constructor);

    // A shared struct has no prototype: the prototype of its constructor is null and instances have none.
    constructor->putDirect(vm, vm.propertyNames->prototype, shared ? jsNull() : JSValue(prototype), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    // MakeConstructor(F, false, proto) does not give the prototype a "constructor" property.
    // The prototype knows its constructor (for super()) under a name that is not the brand of instances.
    prototype->putDirect(vm, builtinNames.structParentPrivateName(), constructor, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    return JSValue::encode(jsUndefined());
}

static EncodedJSValue structAddField(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* constructor = asObject(callFrame->argument(1));
    JSValue key = callFrame->argument(2);
    JSObject* fields = fieldListOf(vm, constructor);
    if (!(key.isString() || key.isSymbol()))
        return throwVMTypeError(globalObject, scope, "A struct field name must be a string or a symbol"_s);

    // A field that is declared twice is one field: defining it again changes nothing.
    unsigned count = fieldListLength(vm, fields);
    for (unsigned i = 0; i < count; ++i) {
        JSValue existing = fieldListGet(globalObject, fields, i);
        if (existing && JSValue::strictEqual(globalObject, existing, key))
            return JSValue::encode(jsUndefined());
    }
    fieldListAppend(globalObject, fields, key);
    return JSValue::encode(jsUndefined());
}

static EncodedJSValue structFinish(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* constructor = asObject(callFrame->argument(1));
    JSObject* prototype = asObject(callFrame->argument(2));
    JSValue initializer = callFrame->argument(3);
    constructor->putDirect(vm, builtinNames.structInitializerPrivateName(), initializer, 0);
    // The constructor method is a function whose prototype is the parent of the struct constructor; that is where
    // super() looks for the constructor to call.
    if (initializer.isObject())
        asObject(initializer)->setPrototypeDirect(vm, constructor->getPrototypeDirect());

    // Methods of an unshared struct are not generic: they only accept instances of the struct.
    // (A shared struct has no instance methods.)
    if (!isSharedStructConstructor(vm, constructor)) {
        brandStructMethods(globalObject, prototype, constructor);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(jsUndefined());
}

static EncodedJSValue structSeal(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();

    JSObject* constructor = asObject(callFrame->argument(1));
    JSObject* prototype = asObject(callFrame->argument(2));
    // The prototype of an unshared struct is sealed; the constructor of a shared struct is (it has no prototype).
    if (isSharedStructConstructor(vm, constructor))
        constructor->seal(vm);
    else
        prototype->seal(vm);
    return JSValue::encode(jsUndefined());
}

// super(...) in the constructor method of a struct: GetSuperConstructor() is the prototype of the constructor method.
// The home object of that method is the prototype of its struct, which knows the struct constructor.
static EncodedJSValue structSuperConstructor(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSObject* homeObject = callFrame->argument(1).getObject();
    JSValue constructorValue = homeObject ? homeObject->getDirect(vm, builtinNames.structParentPrivateName()) : JSValue();
    JSValue initializer = constructorValue.isObject() ? asObject(constructorValue)->getDirect(vm, builtinNames.structInitializerPrivateName()) : JSValue();
    if (!initializer || !initializer.isObject())
        return throwVMTypeError(globalObject, scope, "super() is only valid in the constructor of a struct"_s);

    JSValue superConstructor = asObject(initializer)->getPrototype(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(superConstructor);
}

// What super(...) calls: it is called with the arguments of super(...) and the this value of the constructor method,
// checks the super constructor (which it holds in @structMethod) and calls the constructor method of the parent
// struct with them, or does nothing if the parent has none.
JSC_DEFINE_HOST_FUNCTION(structSuperCall, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const BuiltinNames& builtinNames = vm.propertyNames->builtinNames();

    JSValue parent = callFrame->jsCallee()->getDirect(vm, builtinNames.structMethodPrivateName());
    if (!parent.isConstructor())
        return throwVMTypeError(globalObject, scope, "The super constructor of a struct is not a constructor"_s);
    JSObject* parentObject = asObject(parent);
    if (!parentObject->getDirect(vm, builtinNames.structSharedPrivateName()))
        return throwVMTypeError(globalObject, scope, "The super constructor of a struct is not a struct"_s);

    JSValue initializer = parentObject->getDirect(vm, builtinNames.structInitializerPrivateName());
    if (!initializer || !initializer.isObject())
        return JSValue::encode(jsUndefined());
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    auto callData = JSC::getCallData(initializer);
    RELEASE_AND_RETURN(scope, JSValue::encode(JSC::call(globalObject, initializer, callData, callFrame->thisValue(), arguments)));
}

static EncodedJSValue structSuperThunk(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    JSFunction* thunk = JSFunction::create(vm, globalObject, 0, String(), structSuperCall, ImplementationVisibility::Private);
    thunk->putDirect(vm, vm.propertyNames->builtinNames().structMethodPrivateName(), callFrame->argument(1), 0);
    return JSValue::encode(thunk);
}

// A private method of an unshared struct, branded like the methods of its prototype.
static EncodedJSValue structWrapPrivateMethod(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* prototype = asObject(callFrame->argument(1));
    JSObject* method = asObject(callFrame->argument(2));
    JSValue constructor = prototype->getDirect(vm, vm.propertyNames->builtinNames().structParentPrivateName());
    RELEASE_ASSERT(constructor && constructor.isObject());
    JSFunction* wrapper = wrapStructMethod(globalObject, method, asObject(constructor));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(wrapper);
}

// structDefine(operation, ...): see the comment at the top.
JSC_DEFINE_HOST_FUNCTION(structDefine, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    switch (static_cast<int>(callFrame->argument(0).asNumber())) {
    case 0:
        return structCreate(globalObject, callFrame);
    case 1:
        return structLink(globalObject, callFrame);
    case 2:
        return structAddField(globalObject, callFrame);
    case 3:
        return structFinish(globalObject, callFrame);
    case 4:
        return structSeal(globalObject, callFrame);
    case 5:
        return structSuperConstructor(globalObject, callFrame);
    case 6:
        return structWrapPrivateMethod(globalObject, callFrame);
    case 7:
        return structSuperThunk(globalObject, callFrame);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return JSValue::encode(jsUndefined());
}

} // namespace JSC
