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
#include "ImmutableIntrinsics.h"

#include "AOTFunction.h"
#include "BuiltinExecutables.h"
#include "BuiltinNames.h"
#include "CustomGetterSetter.h"
#include "DeletePropertySlot.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSGlobalObjectFunctions.h"
#include "Lookup.h"
#include "PropertyDescriptor.h"
#include "StackVisitor.h"
#include "TopExceptionScope.h"
#include <wtf/ASCIICType.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmutableIntrinsics);

static std::atomic<const ImmutableIntrinsics*> s_shared;

const ImmutableIntrinsics* ImmutableIntrinsics::shared()
{
    return s_shared.load(std::memory_order_acquire);
}

void ImmutableIntrinsics::ensureShared(VM& vm)
{
    if (!vm.useImmutableIntrinsics || shared())
        return;
    JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    RELEASE_ASSERT(shared());
}

// The build describes the realm of a VM that has no image: there a builtin that is implemented in JavaScript is no host function.
static bool isCompiledAheadOfTime(JSFunction* function)
{
#if ENABLE(AOT)
    return AOT::Typed::declaredAs(function) != AOT::Typed::DeclaredAs::NotCompiled;
#else
    UNUSED_PARAM(function);
    return false;
#endif
}

static uint64_t keyFor(unsigned holder, const StringImpl& name)
{
    return static_cast<uint64_t>(holder + 1) << 32 | name.hash();
}

unsigned ImmutableIntrinsics::find(unsigned holder, const StringImpl& name) const
{
    if (name.isSymbol())
        return 0;
    auto it = m_numbers.find(keyFor(holder, name));
    if (it == m_numbers.end() || !WTF::equal(m_entries[it->value].name.impl(), &name))
        return 0;
    return it->value;
}

Vector<EncodedJSValue> ImmutableIntrinsics::describe(JSGlobalObject* globalObject, std::span<const ASCIILiteral> variableNames, unsigned numberOfVariablesWithNumberedProperties)
{
    VM& vm = globalObject->vm();
    auto description = std::unique_ptr<ImmutableIntrinsics>(new ImmutableIntrinsics);
    Vector<EncodedJSValue> values;
    UncheckedKeyHashMap<JSCell*, uint16_t> numberOfCell;

    auto add = [&](unsigned holder, const StringImpl& name, JSValue value) {
        RELEASE_ASSERT(values.size() < maximumCount);
        uint16_t number = values.size();
        Entry entry;
        entry.name = String(name.isolatedCopy());
        entry.holder = holder;
        entry.canonical = number;
        if (value.isCell()) {
            RELEASE_ASSERT(!value.asCell()->structure()->typeInfo().masqueradesAsUndefined());
            entry.isCell = true;
            entry.type = value.asCell()->type();
            entry.canonical = numberOfCell.add(value.asCell(), number).iterator->value;
            if (auto* function = dynamicDowncast<JSFunction>(value.asCell()); function && function->isBuiltinFunction()) {
                if (auto index = vm.builtinExecutables()->indexOf(function->jsExecutable()->unlinkedExecutable()))
                    entry.builtinCode = *index + 1;
            } else if (function)
                entry.isHostFunction = function->isHostFunction() && !isCompiledAheadOfTime(function);
            entry.inheritsFromFunctionPrototype = number && isFunctionOfIntrinsic(entry) && asObject(value)->getPrototypeDirect() == globalObject->functionPrototype();
        } else
            entry.primitive = JSValue::encode(value);
        if (number && !description->m_numbers.add(keyFor(holder, name), number).isNewEntry)
            description->m_numbers.set(keyFor(holder, name), 0);
        description->m_hash = WTF::pairIntHash(description->m_hash, WTF::pairIntHash(name.hash(), holder << 16 | entry.canonical)) + entry.type + entry.isCell + entry.isHostFunction + entry.inheritsFromFunctionPrototype;
        description->m_entries.append(WTF::move(entry));
        values.append(JSValue::encode(value));
    };

    add(0, *StringImpl::empty(), globalObject);
    for (ASCIILiteral name : variableNames) {
        Identifier identifier = Identifier::fromString(vm, name);
        add(ImmutableIntrinsics::globalObject, *identifier.impl(), globalObject->getDirect(vm, identifier));
    }
    for (unsigned number = 1; number < values.size(); ++number) {
        JSObject* object = JSValue::decode(values[number]).getObject();
        if (!object || description->m_entries[number].canonical != number || !object->structure()->inheritorsMayOverrideReadOnlyProperties())
            continue;
        if (description->m_entries[number].type == JSFunctionType || (number > numberOfVariablesWithNumberedProperties && number <= variableNames.size()))
            continue;
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& property) {
            constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
            if (!property.key()->isSymbol() && (property.attributes() & fixed) == fixed && !(property.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                add(number, *property.key(), object->getDirect(property.offset()));
            return true;
        });
    }

    static Lock lock;
    Locker locker { lock };
    if (const ImmutableIntrinsics* first = shared())
        RELEASE_ASSERT(first->count() == description->count() && first->hash() == description->hash());
    else
        s_shared.store(description.release(), std::memory_order_release);
    return values;
}

namespace {

struct PlaceInProgram {
    String functionName;
    String sourceURL;
    LineColumn position;
    bool isStrict { true };
    bool isBelowFunctionOfEngine { false };
};

struct PropertyAsStored {
    bool exists { false };
    bool wouldBeFixed { false };
    bool hasSetter { false };
    unsigned attributes { 0 };
};

}

static PlaceInProgram placeInProgram(VM& vm)
{
    PlaceInProgram place;
    if (!vm.topCallFrame)
        return place;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (!visitor->hasCode() || visitor->isBuiltinFunction()) {
            place.isBelowFunctionOfEngine = true;
            return IterationStatus::Continue;
        }
        place.functionName = visitor->functionName();
        place.sourceURL = visitor->sourceURL();
        place.position = visitor->computeLineAndColumn();
        if (auto* executable = visitor->ownerExecutable())
            place.isStrict = executable->isInStrictContext();
        return IterationStatus::Done;
    });
    return place;
}

static PropertyAsStored propertyAsStored(VM& vm, JSObject* object, PropertyName name)
{
    Structure* structure = object->structure();
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, name, attributes);
    if (isValidOffset(offset)) {
        bool hasSetter = false;
        if (attributes & PropertyAttribute::Accessor)
            hasSetter = !uncheckedDowncast<GetterSetter>(object->getDirect(offset))->isSetterNull();
        else if (attributes & PropertyAttribute::CustomAccessorOrValue)
            hasSetter = !!uncheckedDowncast<CustomGetterSetter>(object->getDirect(offset))->setter();
        return { true, !!(attributes & PropertyAttribute::WouldBeFixed), hasSetter, attributes };
    }
    if (!structure->hasNonReifiedStaticProperties())
        return { };
    auto entry = structure->findPropertyHashEntry(name);
    if (!entry)
        return { };
    attributes = entry->value->attributes();
    if (!(attributes & (PropertyAttribute::CustomAccessor | PropertyAttribute::BuiltinOrFunctionOrAccessorOrLazyPropertyOrConstant)))
        attributes |= PropertyAttribute::CustomValue;
    bool hasSetter = (attributes & PropertyAttribute::CustomAccessorOrValue) && entry->value->propertyPutter();
    return { true, isStaticPropertyOfIntrinsic(vm, *object, *entry->value), hasSetter, attributes };
}

static bool wouldInheritImmutableProperty(VM& vm, JSObject* object, PropertyName name)
{
    for (JSValue prototype = object->getPrototypeDirect(); prototype.isObject(); prototype = asObject(prototype)->getPrototypeDirect()) {
        Structure* structure = asObject(prototype)->structure();
        if (!structure->typeInfo().wouldHaveFixedProperties())
            continue;
        unsigned attributes;
        if (isValidOffset(structure->get(vm, name, attributes))) {
            if (attributes & (PropertyAttribute::DontDelete | PropertyAttribute::WouldBeFixed))
                return true;
        } else if (structure->hasNonReifiedStaticProperties() && structure->findPropertyHashEntry(name))
            return true;
    }
    return false;
}

static bool isPlainName(StringView text, bool allowsSpaces = false)
{
    if (text.isEmpty() || text.length() > 64)
        return false;
    for (auto character : text.codeUnits()) {
        if (!isASCIIAlphanumeric(character) && character != '_' && character != '$' && !(allowsSpaces && character == ' '))
            return false;
    }
    return true;
}

static bool isWellKnownSymbol(VM& vm, PropertyName name)
{
#define JSC_RETURN_TRUE_IF_WELL_KNOWN_SYMBOL(symbol) \
    if (name == vm.propertyNames->symbol##Symbol) \
        return true;
    JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(JSC_RETURN_TRUE_IF_WELL_KNOWN_SYMBOL)
#undef JSC_RETURN_TRUE_IF_WELL_KNOWN_SYMBOL
    return false;
}

static String nameOfFunction(VM& vm, JSObject* object)
{
    String name;
    if (auto* function = dynamicDowncast<JSFunction>(object))
        name = function->isHostOrBuiltinFunction() ? function->name(vm) : function->jsExecutable()->ecmaName().string();
    else
        name = uncheckedDowncast<InternalFunction>(object)->name();
    if (!isPlainName(name, true))
        return "function"_s;
    return name;
}

static JSObject* generatorFunctionOfPrototype(VM& vm, JSObject* object)
{
    JSValue function = object->getDirect(vm, vm.propertyNames->builtinNames().generatorPrivateName());
    return function ? function.getObject() : nullptr;
}

static String describeIntrinsic(VM& vm, JSObject* object, unsigned depth = 0)
{
    if (object->isGlobalObject())
        return "globalThis"_s;
    JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor);
    JSObject* constructorObject = constructor ? constructor.getObject() : nullptr;
    if (constructorObject && constructorObject != object && depth < 2 && constructorObject->getDirect(vm, vm.propertyNames->prototype) == JSValue(object))
        return makeString(describeIntrinsic(vm, constructorObject, depth + 1), ".prototype"_s);
    if (is<JSFunction>(object) || is<InternalFunction>(object))
        return nameOfFunction(vm, object);
    bool isPrototype = object->structure()->typeInfo().wouldHaveImmutableProperties();
    JSValue tag = object->getDirect(vm, vm.propertyNames->toStringTagSymbol);
    String text;
    if (tag && tag.isString())
        text = asString(tag)->tryGetValue();
    if (isPlainName(text, true)) {
        if (!isPrototype)
            return text;
        StringBuilder builder;
        builder.append('%');
        for (auto character : StringView(text).codeUnits()) {
            if (character != ' ')
                builder.append(character);
        }
        builder.append("Prototype%"_s);
        return builder.toString();
    }
    if (JSObject* function = isPrototype && depth < 2 ? generatorFunctionOfPrototype(vm, object) : nullptr)
        return makeString(describeIntrinsic(vm, function, depth + 1), ".prototype"_s);
    return object->classInfo()->className;
}

static String describeKey(VM& vm, PropertyName name)
{
    if (name.isSymbol())
        return isWellKnownSymbol(vm, name) ? makeString('[', StringView(name.uid()), ']') : "[symbol]"_str;
    if (parseIndex(name))
        return "[index]"_s;
    if (!isPlainName(StringView(name.uid())))
        return "[name]"_s;
    return makeString('.', StringView(name.uid()));
}

static String describeKeyAsArgument(VM& vm, PropertyName name)
{
    if (name.isSymbol())
        return isWellKnownSymbol(vm, name) ? String(name.uid()) : "symbol"_str;
    if (parseIndex(name))
        return "index"_s;
    if (!isPlainName(StringView(name.uid())))
        return "name"_s;
    return makeString('"', StringView(name.uid()), '"');
}

static ASCIILiteral kindOfWrite(VM& vm, JSObject* object)
{
    if (object->isGlobalObject())
        return "global-binding-write"_s;
    if (!object->structure()->typeInfo().wouldHaveImmutableProperties())
        return "builtin-member-replace"_s;
    return generatorFunctionOfPrototype(vm, object) ? "generator-prototype-write"_s : "builtin-prototype-write"_s;
}

static void reportRefusal(JSGlobalObject* globalObject, ASCIILiteral kind, ASCIILiteral functionOfReflect, JSObject* object, PropertyName name, const String& error, std::optional<bool> shouldThrow)
{
    VM& vm = globalObject->vm();
    PlaceInProgram place = placeInProgram(vm);
    String objectText = describeIntrinsic(vm, object);
    String touched = name.isNull() ? objectText : makeString(objectText, describeKey(vm, name));
    String message;
    if (shouldThrow.value_or(place.isStrict && !place.isBelowFunctionOfEngine))
        message = makeString(touched, ": "_s, error);
    else if (place.isBelowFunctionOfEngine) {
        if (name.isNull())
            message = makeString("Reflect."_s, functionOfReflect, '(', objectText, "): answers false"_s);
        else
            message = makeString("Reflect."_s, functionOfReflect, '(', objectText, ", "_s, describeKeyAsArgument(vm, name), "): answers false"_s);
    } else if (!shouldThrow)
        message = makeString("delete "_s, touched, ": answers false"_s);
    else
        message = makeString(touched, ": ignored ("_s, error, ')');
    {
        static Lock lock;
        static NeverDestroyed<UncheckedKeyHashSet<String>> reported;
        Locker locker { lock };
        if (!reported->add(makeString(place.sourceURL, ':', place.position.line, ':', place.position.column, ' ', message)).isNewEntry)
            return;
    }
    vm.immutableIntrinsicsAuditReporter()(globalObject, kind, message, place.functionName, place.sourceURL, place.position.line, place.position.column);
}

void ImmutableIntrinsicsAudit::willPut(JSGlobalObject* globalObject, JSObject* receiver, PropertyName name, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    if (name.isPrivateName())
        return;
    TypeInfo typeInfo = receiver->structure()->typeInfo();
    bool isImmutable = typeInfo.wouldHaveImmutableProperties();
    bool storesOnReceiver = true;
    bool isOwn = false;
    bool isFixed = false;
    for (JSObject* object = receiver; object; object = object->getPrototypeDirect().getObject()) {
        if (object != receiver && object->structure()->typeInfo().overridesPut())
            return;
        PropertyAsStored property = propertyAsStored(vm, object, name);
        if (!property.exists) {
            if (object == receiver && isImmutable && TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags())) {
                PropertySlot ownSlot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
                if (object->methodTable()->getOwnPropertySlot(object, globalObject, name, ownSlot) && (ownSlot.attributes() & PropertyAttribute::ReadOnly))
                    return;
            }
            continue;
        }
        if (property.attributes & PropertyAttribute::ReadOnly)
            return;
        if (property.attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor)) {
            if (!property.hasSetter)
                return;
            storesOnReceiver = false;
        } else if (object == receiver) {
            isOwn = true;
            isFixed = property.wouldBeFixed && !(property.attributes & PropertyAttribute::CustomValue);
        }
        break;
    }
    bool isRefused = isImmutable;
    if (!isRefused && storesOnReceiver)
        isRefused = isOwn ? isFixed : typeInfo.wouldHaveFixedProperties() && wouldInheritImmutableProperty(vm, receiver, name);
    if (!isRefused)
        return;
    if (storesOnReceiver && !isOwn && !receiver->isStructureExtensible())
        return;
    slot.disableCaching();
    reportRefusal(globalObject, kindOfWrite(vm, receiver), "set"_s, receiver, name, ReadonlyPropertyWriteError, slot.isStrictMode());
}

void ImmutableIntrinsicsAudit::willPutByIndex(JSGlobalObject* globalObject, JSObject* receiver, unsigned index, bool shouldThrow)
{
    if (!receiver->isStructureExtensible())
        return;
    VM& vm = globalObject->vm();
    Identifier name = Identifier::from(vm, index);
    reportRefusal(globalObject, kindOfWrite(vm, receiver), "set"_s, receiver, name, ReadonlyPropertyWriteError, shouldThrow);
}

void ImmutableIntrinsicsAudit::willDefineOwnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, const PropertyDescriptor& descriptor, bool isCurrentDefined, const PropertyDescriptor& current, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    if (name.isPrivateName())
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool isAccepted = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, object->isStructureExtensible(), descriptor, isCurrentDefined, current, false);
    if (scope.exception()) [[unlikely]]
        return;
    if (!isAccepted)
        return;
    TypeInfo typeInfo = object->structure()->typeInfo();
    bool isImmutable = typeInfo.wouldHaveImmutableProperties();
    String error;
    if (!isCurrentDefined || !(current.attributes() & PropertyAttribute::WouldBeFixed)) {
        if (isImmutable)
            error = NonExtensibleObjectPropertyDefineError;
        else if (!isCurrentDefined && typeInfo.wouldHaveFixedProperties() && name != vm.propertyNames->name && name != vm.propertyNames->length && wouldInheritImmutableProperty(vm, object, name))
            error = ReadonlyPropertyWriteError;
        else
            return;
    } else {
        PropertyDescriptor fixed = current;
        fixed.setConfigurable(false);
        if (fixed.isDataDescriptor() && !(propertyAsStored(vm, object, name).attributes & PropertyAttribute::CustomValue))
            fixed.setWritable(false);
        if (isImmutable) {
            if (descriptor.isEmpty())
                return;
            bool isUnchanged = fixed.equalTo(globalObject, descriptor) || isPropertyUnchangedByDescriptor(globalObject, fixed, descriptor);
            if (scope.exception()) [[unlikely]]
                return;
            if (isUnchanged)
                return;
            error = ImmutablePropertyDefineError;
        } else {
            validateAndApplyPropertyDescriptor(globalObject, nullptr, name, true, descriptor, true, fixed, true);
            Exception* exception = scope.exception();
            if (!exception)
                return;
            if (vm.isTerminationException(exception)) [[unlikely]]
                return;
            JSObject* thrown = exception->value().getObject();
            JSValue text = thrown ? thrown->getDirect(vm, vm.propertyNames->message) : JSValue();
            scope.clearException();
            if (!text || !text.isString())
                return;
            error = asString(text)->tryGetValue();
        }
    }
    reportRefusal(globalObject, kindOfWrite(vm, object), "defineProperty"_s, object, name, error, shouldThrow);
}

void ImmutableIntrinsicsAudit::willDefineOwnIndexedProperty(JSGlobalObject* globalObject, JSObject* object, unsigned index, const PropertyDescriptor&, bool shouldThrow)
{
    if (!object->isStructureExtensible())
        return;
    VM& vm = globalObject->vm();
    Identifier name = Identifier::from(vm, index);
    reportRefusal(globalObject, kindOfWrite(vm, object), "defineProperty"_s, object, name, NonExtensibleObjectPropertyDefineError, shouldThrow);
}

void ImmutableIntrinsicsAudit::willDeleteProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, DeletePropertySlot& slot)
{
    slot.disableCaching();
    reportRefusal(globalObject, kindOfWrite(globalObject->vm(), object), "deleteProperty"_s, object, name, UnableToDeletePropertyError, std::nullopt);
}

void ImmutableIntrinsicsAudit::willSetPrototype(JSGlobalObject* globalObject, JSObject* object, JSValue prototype, bool shouldThrow)
{
    if (object->structure()->isImmutablePrototypeExoticObject() || object->getPrototypeDirect() == prototype || !object->isStructureExtensible())
        return;
    reportRefusal(globalObject, kindOfWrite(globalObject->vm(), object), "setPrototypeOf"_s, object, PropertyName(static_cast<UniquedStringImpl*>(nullptr)), "Cannot set prototype of immutable prototype object"_s, shouldThrow);
}

void ImmutableIntrinsicsAudit::willReplacePrototypeProperty(JSGlobalObject* globalObject, JSFunction* function, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    if (function->isHostOrBuiltinFunction())
        return;
    SourceParseMode parseMode = function->jsExecutable()->parseMode();
    if (!isGeneratorWrapperParseMode(parseMode) && !isAsyncGeneratorWrapperParseMode(parseMode))
        return;
    if (function->getDirect(vm, vm.propertyNames->prototype) == value)
        return;
    reportRefusal(globalObject, "generator-prototype-write"_s, "set"_s, function, vm.propertyNames->prototype, makeString("Cannot replace 'prototype' of generator function '"_s, nameOfFunction(vm, function), "': loops call the generator's body directly"_s), shouldThrow);
    if (JSObject* object = value.getObject(); object && object->type() == FinalObjectType && object->isStructureExtensible()) {
        object->putDirect(vm, vm.propertyNames->builtinNames().generatorPrivateName(), function, static_cast<unsigned>(PropertyAttribute::DontEnum));
        object->fixProperties(vm, JSObject::FixedObject::BecomesNonExtensible);
    }
}

void ImmutableIntrinsicsAudit::didAccessCallerOrArguments(JSGlobalObject* globalObject, JSFunction* function, PropertyName name)
{
    reportRefusal(globalObject, "function-caller-arguments"_s, "get"_s, function, name, RestrictedPropertyAccessError, true);
}

} // namespace JSC
