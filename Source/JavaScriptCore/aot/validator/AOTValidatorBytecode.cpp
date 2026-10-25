/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBytecode.h"

#include "AOTGeneratedFunction.h"
#include "AOTThreshold.h"
#include "FunctionExecutable.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObjectFunctions.h"
#include "JSGlobalObjectInlines.h"
#include "JSMapInlines.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "PropertyNameArray.h"
#include "RegExpInlines.h"
#include "Symbol.h"
#include "YarrFlags.h"
#include <wtf/Function.h>
#include <wtf/Int128.h>
#include <wtf/SetForScope.h>
#include <wtf/URL.h>
#include <wtf/dtoa/double-conversion.h>

namespace JSC {

namespace ValidatorBytecode {

using Plan = ValidationPlan;
using Shape = Plan::Shape;
using RuleKind = Plan::RuleKind;
using Writer = GeneratedFunctionAssembler;
using Slot = Writer::Register;
using Target = Writer::Label;
using Comparison = Writer::Comparison;
using Arithmetic = Writer::Arithmetic;

static JSC_DECLARE_HOST_FUNCTION(copyExoticObject);
static JSC_DECLARE_HOST_FUNCTION(hasPlainElements);
static JSC_DECLARE_HOST_FUNCTION(getPlainArrayLength);
static JSC_DECLARE_HOST_FUNCTION(copyExoticArray);
static JSC_DECLARE_HOST_FUNCTION(countHoles);
static JSC_DECLARE_HOST_FUNCTION(clonePlainObject);
static JSC_DECLARE_HOST_FUNCTION(clonePlainArray);
static JSC_DECLARE_HOST_FUNCTION(hasOnlyDeclaredKeys);
static JSC_DECLARE_HOST_FUNCTION(hasOwnEnumerableKey);
static JSC_DECLARE_HOST_FUNCTION(getOwnValue);
static JSC_DECLARE_HOST_FUNCTION(matchesPattern);
static JSC_DECLARE_HOST_FUNCTION(matchesFormat);
static JSC_DECLARE_HOST_FUNCTION(isValidURL);
static JSC_DECLARE_HOST_FUNCTION(matchesTemplate);
static JSC_DECLARE_HOST_FUNCTION(countCodePoints);
static JSC_DECLARE_HOST_FUNCTION(parseJSONText);
static JSC_DECLARE_HOST_FUNCTION(startMemoizing);
static JSC_DECLARE_HOST_FUNCTION(findVerdict);
static JSC_DECLARE_HOST_FUNCTION(storeVerdict);
static JSC_DECLARE_HOST_FUNCTION(throwTooManyObjects);
static JSC_DECLARE_HOST_FUNCTION(hasUniqueItems);
static JSC_DECLARE_HOST_FUNCTION(isDecimalMultiple);
static JSC_DECLARE_HOST_FUNCTION(containsOnlyJSON);

static ValidationFormatChecker formatChecker;

AOT_THRESHOLD(int32_t, maxVisitedObjects, 1 << 24);
AOT_THRESHOLD(int32_t, maxHoles, 65536);

// Shared by the functions of one validator and reset at the start of each call.
enum StateIndex : unsigned { StateVisitCount, StateMemo, StateHolesLeft, StateScannedObjects, StateLength };

static SymbolImpl::StaticSymbolImpl bailoutMarkerName { "validatorBailout", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl failureMarkerName { "validatorFailure", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl codeCacheName { "validatorCodeCache", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl rootsName { "validatorRoots", SymbolImpl::s_flagIsPrivate };

static PropertyName toPropertyName(SymbolImpl::StaticSymbolImpl& name) { return PropertyName(&static_cast<SymbolImpl&>(name)); }

static Symbol* createMarker(VM& vm, SymbolImpl::StaticSymbolImpl& name) { return Symbol::create(vm, static_cast<SymbolImpl&>(name)); }

static bool isAnyArray(JSValue value) { return value.isCell() && (value.asCell()->type() == ArrayType || value.asCell()->type() == DerivedArrayType); }

JSC_DEFINE_HOST_FUNCTION(copyExoticObject, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue bailoutMarker = callFrame->uncheckedArgument(1);
    if (!value.isObject() || isAnyArray(value))
        return JSValue::encode(jsUndefined());
    JSObject* object = asObject(value);
    JSType type = object->type();
    if (type == ProxyObjectType || isFunctionType(type))
        return JSValue::encode(jsUndefined());

    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, names, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, {});
    JSObject* copy = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    for (auto& name : names) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isPresent = object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, {});
        if (!isPresent)
            continue;
        if (!slot.isValue())
            return JSValue::encode(bailoutMarker);
        JSValue current = slot.getValue(globalObject, name);
        RETURN_IF_EXCEPTION(scope, {});
        if (auto index = parseIndex(name))
            copy->putDirectIndex(globalObject, *index, current);
        else
            copy->putDirect(vm, name, current);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(copy);
}

JSC_DEFINE_HOST_FUNCTION(hasPlainElements, (JSGlobalObject*, CallFrame* callFrame))
{
    IndexingType indexing = asObject(callFrame->uncheckedArgument(0))->indexingType();
    return JSValue::encode(jsBoolean(!hasAnyArrayStorage(indexing) && !(indexing & MayHaveIndexedAccessors)));
}

static bool isPlainArray(JSArray* array)
{
    Structure* structure = array->structure();
    JSGlobalObject* owner = structure->globalObject();
    return !hasAnyArrayStorage(structure->indexingType()) && owner->isOriginalArrayStructure(structure) && owner->arrayPrototypeChainIsSane();
}

static unsigned countHolesInPlainArray(JSArray* array)
{
    unsigned length = array->length();
    unsigned holes = 0;
    switch (array->indexingType() & IndexingShapeMask) {
    case Int32Shape:
    case ContiguousShape: {
        auto elements = array->butterfly()->contiguous();
        for (unsigned i = 0; i < length; ++i)
            holes += !elements.at(array, i);
        return holes;
    }
    case DoubleShape: {
        auto elements = array->butterfly()->contiguousDouble();
        for (unsigned i = 0; i < length; ++i) {
            double element = elements.at(array, i);
            holes += element != element;
        }
        return holes;
    }
    default:
        return length;
    }
}

JSC_DEFINE_HOST_FUNCTION(getPlainArrayLength, (JSGlobalObject*, CallFrame* callFrame))
{
    JSValue value = callFrame->uncheckedArgument(0);
    if (!isJSArray(value) || !isPlainArray(asArray(value)))
        return JSValue::encode(jsNumber(-1));
    return JSValue::encode(jsNumber(asArray(value)->length()));
}

JSC_DEFINE_HOST_FUNCTION(copyExoticArray, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue bailoutMarker = callFrame->uncheckedArgument(1);
    if (!isAnyArray(value))
        return JSValue::encode(jsUndefined());
    JSArray* array = asArray(value);
    unsigned length = array->length();
    JSArray* copy = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, {});
    unsigned holes = 0;
    for (unsigned i = 0; i < length; ++i) {
        PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isPresent = array->methodTable()->getOwnPropertySlotByIndex(array, globalObject, i, slot);
        RETURN_IF_EXCEPTION(scope, {});
        if (isPresent ? !slot.isValue() : ++holes > static_cast<unsigned>(maxHoles))
            return JSValue::encode(bailoutMarker);
        JSValue current = isPresent ? slot.getValue(globalObject, Identifier::from(vm, i)) : jsUndefined();
        RETURN_IF_EXCEPTION(scope, {});
        copy->putDirectIndex(globalObject, i, current);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(copy);
}

JSC_DEFINE_HOST_FUNCTION(countHoles, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    if (!isAnyArray(value))
        return JSValue::encode(jsNumber(0));
    JSArray* array = asArray(value);
    if (!hasAnyArrayStorage(array->indexingType()) && array->type() == ArrayType)
        return JSValue::encode(jsNumber(countHolesInPlainArray(array)));
    unsigned length = array->length();
    unsigned holes = 0;
    for (unsigned i = 0; i < length && holes <= static_cast<unsigned>(maxHoles); ++i) {
        PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
        holes += !array->methodTable()->getOwnPropertySlotByIndex(array, globalObject, i, slot);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(jsNumber(holes));
}

JSC_DEFINE_HOST_FUNCTION(clonePlainObject, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* source = asObject(callFrame->uncheckedArgument(0));
    Structure* structure = source->structure();
    if (structure->hasAnyKindOfGetterSetterProperties() || structure->isUncacheableDictionary())
        return JSValue::encode(jsUndefined());
    IndexingType indexing = structure->indexingType();
    if (hasAnyArrayStorage(indexing) || (indexing & MayHaveIndexedAccessors))
        return JSValue::encode(jsUndefined());
    if (structure->canPerformFastPropertyEnumerationCommon()) [[likely]] {
        if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, source))
            return JSValue::encode(copy);
    }

    JSObject* copy = constructEmptyObject(globalObject);
    if (hasIndexedProperties(indexing)) {
        unsigned length = source->butterfly()->publicLength();
        for (unsigned i = 0; i < length; ++i) {
            if (!source->canGetIndexQuickly(i))
                continue;
            copy->putDirectIndex(globalObject, i, source->getIndexQuickly(i));
            RETURN_IF_EXCEPTION(scope, {});
        }
    }
    Vector<UniquedStringImpl*, 16> names;
    MarkedArgumentBuffer values;
    bool hasSymbol = false;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) ALWAYS_INLINE_LAMBDA {
        if (PropertyName(entry.key()).isPrivateName() || (entry.attributes() & PropertyAttribute::DontEnum))
            return true;
        hasSymbol |= entry.key()->isSymbol();
        names.append(entry.key());
        values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
        return true;
    });
    if (hasSymbol)
        return JSValue::encode(jsUndefined());
    for (unsigned i = 0; i < names.size(); ++i)
        copy->putDirect(vm, names[i], values.at(i));
    ensureStillAliveHere(structure);
    return JSValue::encode(copy);
}

JSC_DEFINE_HOST_FUNCTION(clonePlainArray, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* array = asArray(callFrame->uncheckedArgument(0));
    JSValue bailoutMarker = callFrame->uncheckedArgument(3);
    if (!isPlainArray(array))
        return JSValue::encode(bailoutMarker);
    double length = array->length();
    if (length < callFrame->uncheckedArgument(1).asNumber() || length > callFrame->uncheckedArgument(2).asNumber())
        return JSValue::encode(jsUndefined());
    if (countHolesInPlainArray(array))
        return JSValue::encode(bailoutMarker);
    JSArray* copy = tryCloneArrayFromFast<ArrayFillMode::Empty>(globalObject, array);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(copy && isPlainArray(copy) ? JSValue(copy) : bailoutMarker);
}

JSC_DEFINE_HOST_FUNCTION(hasOnlyDeclaredKeys, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(callFrame->uncheckedArgument(0));
    JSArray* declared = asArray(callFrame->uncheckedArgument(1));
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, names, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, {});
    unsigned count = declared->length();
    for (auto& name : names) {
        bool isDeclared = false;
        for (unsigned i = 0; i < count && !isDeclared; ++i)
            isDeclared = asString(declared->getIndexQuickly(i))->tryGetValueImpl() == name.impl();
        if (!isDeclared)
            return JSValue::encode(jsBoolean(false));
    }
    return JSValue::encode(jsBoolean(true));
}

static bool getOwnSlotWithoutSideEffects(JSGlobalObject* globalObject, JSObject* object, JSString* key, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = key->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot));
}

JSC_DEFINE_HOST_FUNCTION(hasOwnEnumerableKey, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(callFrame->uncheckedArgument(0));
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isPresent = getOwnSlotWithoutSideEffects(globalObject, object, asString(callFrame->uncheckedArgument(1)), slot);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsBoolean(isPresent && !(slot.attributes() & PropertyAttribute::DontEnum)));
}

JSC_DEFINE_HOST_FUNCTION(getOwnValue, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(callFrame->uncheckedArgument(0));
    JSString* key = asString(callFrame->uncheckedArgument(1));
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isPresent = getOwnSlotWithoutSideEffects(globalObject, object, key, slot);
    RETURN_IF_EXCEPTION(scope, {});
    if (!isPresent || (slot.attributes() & PropertyAttribute::DontEnum))
        return JSValue::encode(jsUndefined());
    if (!slot.isValue())
        return JSValue::encode(callFrame->uncheckedArgument(2));
    auto name = key->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    RELEASE_AND_RETURN(scope, JSValue::encode(slot.getValue(globalObject, name)));
}

template<typename Expression>
static std::optional<bool> searchFromStart(JSGlobalObject* globalObject, VM& vm, Expression* regExp, StringView text)
{
    if constexpr (requires { regExp->searchFromStart(globalObject, text); }) {
        auto answer = regExp->searchFromStart(globalObject, text);
        if (answer == Expression::SearchAnswer::GaveUp)
            return std::nullopt;
        return answer == Expression::SearchAnswer::Matches;
    } else
        return !!regExp->matchInline(globalObject, vm, text, 0);
}

JSC_DEFINE_HOST_FUNCTION(matchesPattern, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* regExp = uncheckedDowncast<RegExp>(callFrame->uncheckedArgument(0).asCell());
    auto view = asString(callFrame->uncheckedArgument(1))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    std::optional<bool> answer = searchFromStart(globalObject, vm, regExp, view);
    RETURN_IF_EXCEPTION(scope, {});
    if (!answer) [[unlikely]]
        return throwVMRangeError(globalObject, scope, "The search for a pattern took too many steps"_s);
    return JSValue::encode(jsBoolean(*answer));
}

JSC_DEFINE_HOST_FUNCTION(matchesFormat, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = asString(callFrame->uncheckedArgument(1))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsBoolean(formatChecker && formatChecker(vm, callFrame->uncheckedArgument(0).asInt32(), view)));
}

JSC_DEFINE_HOST_FUNCTION(isValidURL, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = asString(callFrame->uncheckedArgument(0))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsBoolean(URL(text).isValid()));
}

static bool isIntegerText(StringView part)
{
    unsigned position = part.length() && part[0] == '-';
    unsigned length = part.length();
    if (position == length)
        return false;
    if (part[position] != '0') {
        for (; position < length; ++position) {
            if (!isASCIIDigit(part[position]))
                return false;
        }
        return true;
    }
    if (position + 1 == length)
        return true;
    char16_t radix = toASCIILower(part[position + 1]);
    position += 2;
    if (position == length || (radix != 'x' && radix != 'o' && radix != 'b'))
        return false;
    for (; position < length; ++position) {
        char16_t digit = part[position];
        if (radix == 'x' ? !isASCIIHexDigit(digit) : radix == 'o' ? !isASCIIOctalDigit(digit) : !isASCIIBinaryDigit(digit))
            return false;
    }
    return true;
}

static bool matchesPlaceholder(char16_t kind, StringView part)
{
    if (kind == 's')
        return true;
    if (kind == 'n')
        return !part.isEmpty() && std::isfinite(jsToNumber(part));
    return isIntegerText(part);
}

JSC_DEFINE_HOST_FUNCTION(matchesTemplate, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto current = asString(callFrame->uncheckedArgument(0))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    StringView value = current;
    JSArray* texts = asArray(callFrame->uncheckedArgument(1));
    StringView placeholders = asString(callFrame->uncheckedArgument(2))->tryGetValueImpl();
    auto textAt = [&](unsigned index) -> StringView { return asString(texts->getIndexQuickly(index))->tryGetValueImpl(); };
    unsigned last = texts->length() - 1;
    StringView head = textAt(0);
    StringView tail = textAt(last);
    if (value.length() < head.length() + tail.length() || !value.startsWith(head) || !value.endsWith(tail))
        return JSValue::encode(jsBoolean(false));
    unsigned end = value.length() - tail.length();
    unsigned position = head.length();
    for (unsigned i = 1; i < last; ++i) {
        StringView text = textAt(i);
        size_t partEnd = text.isEmpty() ? position + 1 : value.find(text, position);
        if (partEnd == notFound || partEnd + text.length() > end)
            return JSValue::encode(jsBoolean(false));
        if (!matchesPlaceholder(placeholders[i - 1], value.substring(position, partEnd - position)))
            return JSValue::encode(jsBoolean(false));
        position = partEnd + text.length();
    }
    return JSValue::encode(jsBoolean(matchesPlaceholder(placeholders[last - 1], value.substring(position, end - position))));
}

JSC_DEFINE_HOST_FUNCTION(countCodePoints, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = asString(callFrame->uncheckedArgument(0))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    if (view->is8Bit())
        return JSValue::encode(jsNumber(view->length()));
    auto characters = view->span16();
    unsigned count = 0;
    for (size_t i = 0; i < characters.size(); ++i, ++count) {
        if (U16_IS_LEAD(characters[i]) && i + 1 < characters.size() && U16_IS_TRAIL(characters[i + 1]))
            ++i;
    }
    return JSValue::encode(jsNumber(count));
}

JSC_DEFINE_HOST_FUNCTION(parseJSONText, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = asString(callFrame->uncheckedArgument(0))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    RELEASE_AND_RETURN(scope, JSValue::encode(JSONParseWithException(globalObject, view)));
}

JSC_DEFINE_HOST_FUNCTION(startMemoizing, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    JSArray* state = asArray(callFrame->uncheckedArgument(0));
    if (state->getIndexQuickly(StateMemo).isUndefined())
        state->setIndexQuickly(vm, StateMemo, JSMap::create(vm, globalObject->mapStructure()));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(findVerdict, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    JSValue value = callFrame->uncheckedArgument(2);
    if (!value.isObject())
        return JSValue::encode(jsUndefined());
    JSValue known = uncheckedDowncast<JSMap>(callFrame->uncheckedArgument(0).asCell())->get(globalObject, value);
    if (!known.isCell())
        return JSValue::encode(jsUndefined());
    JSArray* verdicts = asArray(known);
    JSValue node = callFrame->uncheckedArgument(1);
    for (unsigned i = 0; i + 1 < verdicts->length(); i += 2) {
        if (verdicts->getIndexQuickly(i) == node)
            return JSValue::encode(verdicts->getIndexQuickly(i + 1));
    }
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(storeVerdict, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(2);
    if (!value.isObject())
        return JSValue::encode(jsUndefined());
    auto* memory = uncheckedDowncast<JSMap>(callFrame->uncheckedArgument(0).asCell());
    JSValue known = memory->get(globalObject, value);
    JSArray* verdicts = known.isCell() ? asArray(known) : nullptr;
    if (!verdicts) {
        verdicts = constructEmptyArray(globalObject, nullptr);
        RETURN_IF_EXCEPTION(scope, {});
        memory->set(globalObject, value, verdicts);
        RETURN_IF_EXCEPTION(scope, {});
    }
    unsigned length = verdicts->length();
    verdicts->putDirectIndex(globalObject, length, callFrame->uncheckedArgument(1));
    RETURN_IF_EXCEPTION(scope, {});
    verdicts->putDirectIndex(globalObject, length + 1, callFrame->uncheckedArgument(3));
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(throwTooManyObjects, (JSGlobalObject * globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMRangeError(globalObject, scope, "The value holds more than 16777216 objects and arrays"_s);
}

class CanonicalWriter {
public:
    explicit CanonicalWriter(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    enum class End : uint8_t { Written, NeedsUserCode, Threw };

    End write(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (value.isNull())
            return tag(Null);
        if (value.isBoolean())
            return tag(value.asBoolean() ? True : False);
        if (value.isNumber() && std::isfinite(value.asNumber()))
            return word(Number, std::bit_cast<uint64_t>(value.asNumber() + 0.0));
        if (value.isString()) {
            auto view = asString(value)->view(m_globalObject);
            RETURN_IF_EXCEPTION(scope, End::Threw);
            word(Text, view->length());
            for (unsigned i = 0; i < view->length(); ++i) {
                char16_t unit = view.data[i];
                bytes.append(static_cast<uint8_t>(unit));
                bytes.append(static_cast<uint8_t>(unit >> 8));
            }
            return End::Written;
        }
        if (!value.isObject() || value.asCell()->type() == ProxyObjectType || isFunctionType(value.asCell()->type()))
            return word(Other, JSValue::encode(value));
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return End::Threw;
        }
        JSObject* object = asObject(value);
        if (isAnyArray(value)) {
            unsigned length = asArray(object)->length();
            word(List, length);
            unsigned holes = 0;
            for (unsigned i = 0; i < length; ++i) {
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                bool isPresent = object->methodTable()->getOwnPropertySlotByIndex(object, m_globalObject, i, slot);
                RETURN_IF_EXCEPTION(scope, End::Threw);
                if (isPresent ? !slot.isValue() : ++holes > static_cast<unsigned>(maxHoles))
                    return End::NeedsUserCode;
                JSValue element = isPresent ? slot.getValue(m_globalObject, Identifier::from(m_vm, i)) : jsUndefined();
                RETURN_IF_EXCEPTION(scope, End::Threw);
                End end = write(element);
                if (end != End::Written)
                    return end;
            }
            return End::Written;
        }
        PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        object->methodTable()->getOwnPropertyNames(object, m_globalObject, names, DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, End::Threw);
        Vector<UniquedStringImpl*, 16> sorted;
        for (auto& name : names) {
            sorted.append(name.impl());
            m_keys.append(name.impl());
        }
        std::sort(sorted.begin(), sorted.end());
        size_t placeOfCount = bytes.size() + 1;
        word(Record, 0);
        uint64_t count = 0;
        for (UniquedStringImpl* key : sorted) {
            PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
            bool isPresent = object->methodTable()->getOwnPropertySlot(object, m_globalObject, key, slot);
            RETURN_IF_EXCEPTION(scope, End::Threw);
            if (!isPresent)
                continue;
            if (!slot.isValue())
                return End::NeedsUserCode;
            JSValue current = slot.getValue(m_globalObject, key);
            RETURN_IF_EXCEPTION(scope, End::Threw);
            if (current.isUndefined())
                continue;
            ++count;
            word(Text, reinterpret_cast<uintptr_t>(key));
            End end = write(current);
            if (end != End::Written)
                return end;
        }
        memcpy(bytes.mutableSpan().subspan(placeOfCount).data(), &count, sizeof(count));
        return End::Written;
    }

    Vector<uint8_t, 1024> bytes;

private:
    enum Tag : uint8_t { Null, False, True, Number, Text, List, Record, Other };

    End tag(Tag tag)
    {
        bytes.append(tag);
        return End::Written;
    }

    End word(Tag tag, uint64_t word)
    {
        bytes.append(tag);
        bytes.append(std::span { reinterpret_cast<const uint8_t*>(&word), sizeof(word) });
        return End::Written;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Vector<RefPtr<UniquedStringImpl>, 32> m_keys;
};

JSC_DEFINE_HOST_FUNCTION(hasUniqueItems, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct Item {
        size_t start;
        size_t end;
    };
    JSArray* array = asArray(callFrame->uncheckedArgument(0));
    unsigned length = array->length();
    CanonicalWriter writer(globalObject);
    Vector<Item, 32> items;
    for (unsigned i = 0; i < length; ++i) {
        size_t start = writer.bytes.size();
        JSValue element = array->canGetIndexQuickly(i) ? array->getIndexQuickly(i) : jsUndefined();
        auto end = writer.write(element);
        RETURN_IF_EXCEPTION(scope, {});
        if (end != CanonicalWriter::End::Written)
            return JSValue::encode(callFrame->uncheckedArgument(1));
        items.append(Item { start, writer.bytes.size() });
    }
    auto compare = [&](const Item& a, const Item& b) {
        if (a.end - a.start != b.end - b.start)
            return a.end - a.start < b.end - b.start ? -1 : 1;
        return memcmp(writer.bytes.span().subspan(a.start).data(), writer.bytes.span().subspan(b.start).data(), a.end - a.start);
    };
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        return compare(a, b) < 0;
    });
    for (size_t i = 1; i < items.size(); ++i) {
        if (!compare(items[i - 1], items[i]))
            return JSValue::encode(jsBoolean(false));
    }
    return JSValue::encode(jsBoolean(true));
}

JSC_DEFINE_HOST_FUNCTION(isDecimalMultiple, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(isDecimalMultipleOf(callFrame->uncheckedArgument(0).asNumber(), callFrame->uncheckedArgument(1).asNumber())));
}

class JSONScanner {
public:
    JSONScanner(JSGlobalObject* globalObject, JSMap* scannedObjects)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_scannedObjects(scannedObjects)
    {
    }

    JSONScanResult look(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (value.isNumber())
            return std::isfinite(value.asNumber()) ? JSONScanResult::OnlyJSON : JSONScanResult::HasNonJSON;
        if (value.isString() || value.isBoolean() || value.isNull())
            return JSONScanResult::OnlyJSON;
        if (!value.isObject() || isFunctionType(value.asCell()->type()))
            return JSONScanResult::HasNonJSON;
        JSObject* object = asObject(value);
        if (object->type() == ProxyObjectType)
            return JSONScanResult::NeedsUserCode;
        if (m_seen.contains(object) || (m_scannedObjects && m_scannedObjects->has(m_globalObject, object)))
            return JSONScanResult::OnlyJSON;
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return JSONScanResult::NeedsUserCode;
        }
        if (isAnyArray(value)) {
            unsigned length = asArray(object)->length();
            for (unsigned i = 0; i < length; ++i) {
                JSValue element = object->tryGetIndexQuickly(i);
                if (!element) {
                    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                    bool isPresent = object->methodTable()->getOwnPropertySlotByIndex(object, m_globalObject, i, slot);
                    RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
                    if (!isPresent)
                        return JSONScanResult::HasNonJSON;
                    if (!slot.isValue())
                        return JSONScanResult::NeedsUserCode;
                    element = slot.getValue(m_globalObject, Identifier::from(m_vm, i));
                    RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
                }
                JSONScanResult answer = look(element);
                if (answer != JSONScanResult::OnlyJSON)
                    return answer;
            }
        } else {
            PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            object->methodTable()->getOwnPropertyNames(object, m_globalObject, names, DontEnumPropertiesMode::Exclude);
            RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
            for (auto& name : names) {
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                bool isPresent = object->methodTable()->getOwnPropertySlot(object, m_globalObject, name, slot);
                RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
                if (!isPresent)
                    continue;
                if (!slot.isValue())
                    return JSONScanResult::NeedsUserCode;
                JSValue current = slot.getValue(m_globalObject, name);
                RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
                if (current.isUndefined())
                    continue;
                JSONScanResult answer = look(current);
                if (answer != JSONScanResult::OnlyJSON)
                    return answer;
            }
        }
        if (m_scannedObjects) {
            m_scannedObjects->set(m_globalObject, object, jsBoolean(true));
            RETURN_IF_EXCEPTION(scope, JSONScanResult::NeedsUserCode);
        } else
            m_seen.add(object);
        return JSONScanResult::OnlyJSON;
    }

private:
    JSGlobalObject* m_globalObject;
    VM& m_vm;
    JSMap* m_scannedObjects;
    UncheckedKeyHashSet<JSObject*> m_seen;
};

JSC_DEFINE_HOST_FUNCTION(containsOnlyJSON, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    if (value.isCell() && isFunctionType(value.asCell()->type()))
        return JSValue::encode(callFrame->uncheckedArgument(1));
    JSMap* scannedObjects = nullptr;
    if (value.isObject() && callFrame->uncheckedArgument(2).isCell()) {
        JSArray* state = asArray(callFrame->uncheckedArgument(2));
        if (state->getIndexQuickly(StateScannedObjects).isUndefined())
            state->setIndexQuickly(vm, StateScannedObjects, JSMap::create(vm, globalObject->mapStructure()));
        scannedObjects = uncheckedDowncast<JSMap>(state->getIndexQuickly(StateScannedObjects).asCell());
    }
    JSONScanResult answer = JSONScanner(globalObject, scannedObjects).look(value);
    RETURN_IF_EXCEPTION(scope, {});
    if (answer == JSONScanResult::NeedsUserCode)
        return JSValue::encode(callFrame->uncheckedArgument(1));
    return JSValue::encode(jsBoolean(answer == JSONScanResult::OnlyJSON));
}

namespace {

enum class Mode : uint8_t { Check, Copy };

static bool isContainer(Shape shape) { return shape >= Shape::Array && shape <= Shape::Object; }

static uint32_t findAlias(const Plan::Node& node)
{
    bool isAlias = node.shape == Shape::Unknown && node.defaultKind == Plan::Literal::None && node.rules.size() == 1 && node.rules[0].kind == RuleKind::Every && node.rules[0].nodes.size() == 1;
    return isAlias ? node.rules[0].nodes[0] : Plan::none;
}

struct Analysis : ThreadSafeRefCounted<Analysis> {
    Vector<bool> isFunction;
    Vector<bool> isRecursive;
    Vector<bool> callsSharedFunction;
    Vector<bool> describesContainers;
    Vector<bool> appliesDefault;
    Vector<bool> acceptsUndefined;
    Vector<bool> holdsContainers;
    bool hasFunctions { false };
    bool hasLoops { false };
    bool hasDefaults { false };
    bool acceptsHoles { false };
    bool isSupported { true };
};

class Analyzer {
public:
    Analyzer(const Plan& plan, Analysis& analysis)
        : m_plan(plan)
        , m_analysis(analysis)
    {
        size_t size = plan.nodes.size();
        m_state.fill(State::New, size);
        m_weight.fill(0, size);
        m_uses.fill(0, size);
        m_lastWalk.fill(0, size);
        m_inlinedSize.fill(0, size);
        analysis.isFunction.fill(false, size);
        analysis.isRecursive.fill(false, size);
        analysis.callsSharedFunction.fill(false, size);
        analysis.describesContainers.fill(false, size);
        analysis.appliesDefault.fill(false, size);
        analysis.acceptsUndefined.fill(false, size);
        analysis.holdsContainers.fill(false, size);
    }

    void run()
    {
        visit(m_plan.root);
        if (!m_analysis.isSupported)
            return;
        size_t size = m_plan.nodes.size();
        for (unsigned i = 0; i < size; ++i) {
            if (m_uses[i] > 1 && m_weight[i] > maxDuplicatedWeight)
                m_analysis.isFunction[i] = true;
            m_analysis.hasFunctions |= m_analysis.isFunction[i];
        }
        for (unsigned i = 0; i < size; ++i) {
            if (m_state[i] != State::Done)
                continue;
            forEachAlternative(i, [&](const Plan::Node& alternative) {
                m_analysis.describesContainers[i] = m_analysis.describesContainers[i] || isContainer(alternative.shape);
                m_analysis.appliesDefault[i] = m_analysis.appliesDefault[i] || alternative.defaultKind != Plan::Literal::None;
                m_analysis.acceptsUndefined[i] = m_analysis.acceptsUndefined[i] || alternative.shape == Shape::Unknown || alternative.shape == Shape::Undefined || alternative.defaultKind != Plan::Literal::None;
            });
            m_analysis.hasDefaults |= m_analysis.appliesDefault[i];
        }
        unsigned written = computeInlinedSize(m_plan.root);
        for (unsigned i = 0; i < size; ++i) {
            if (m_analysis.isFunction[i])
                written = std::min(maxWeight, written + computeInlinedSize(i));
        }
        if (written > maxInlinedNodes)
            m_analysis.isSupported = false;
        if (!m_analysis.isSupported)
            return;
        m_reaches.fill(Reaches::NotAsked, size);
        for (unsigned i = 0; i < size; ++i) {
            if (m_state[i] != State::Done)
                continue;
            m_analysis.callsSharedFunction[i] = callsSharedFunction(i);
            const Plan::Node& node = m_plan.nodes[i];
            if (!isContainer(node.shape))
                continue;
            bool holds = false;
            bool acceptsHoles = false;
            forEachChildOfShape(node, [&](uint32_t child) {
                holds |= m_analysis.describesContainers[child];
                acceptsHoles |= m_analysis.acceptsUndefined[child];
            });
            m_analysis.holdsContainers[i] = holds;
            if (node.shape == Shape::Array || node.shape == Shape::Tuple)
                m_analysis.acceptsHoles |= acceptsHoles;
            m_analysis.hasLoops |= node.shape != Shape::Object || node.undeclared == Plan::Undeclared::Validated;
            for (auto& rule : node.rules) {
                m_analysis.hasLoops |= rule.kind == RuleKind::PatternMembers;
                for (uint32_t child : rule.nodes)
                    m_analysis.holdsContainers[i] = m_analysis.holdsContainers[i] || m_analysis.describesContainers[child];
            }
        }
    }

private:
    AOT_CLASS_THRESHOLD(unsigned, maxDuplicatedWeight, 12);
    AOT_CLASS_THRESHOLD(unsigned, maxPlanDepth, 400);
    AOT_CLASS_THRESHOLD(unsigned, maxWeight, 1 << 20);
    AOT_CLASS_THRESHOLD(unsigned, maxInlinedNodes, 1 << 17);
    AOT_CLASS_THRESHOLD(unsigned, maxAnalysisSteps, 1 << 22);
    enum class State : uint8_t { New, Open, Done };
    enum class Reaches : uint8_t { NotAsked, No, Yes };

    static void forEachChildOfShape(const Plan::Node& node, NOESCAPE const Invocable<void(uint32_t)> auto& visit)
    {
        if (node.child != Plan::none)
            visit(node.child);
        for (uint32_t child : node.children)
            visit(child);
        for (auto& member : node.members)
            visit(member.node);
    }

    void forEachAlternative(uint32_t index, NOESCAPE const Invocable<void(const Plan::Node&)> auto& visit)
    {
        ++m_walk;
        Vector<uint32_t, 8> alternatives;
        auto add = [&](uint32_t alternative) {
            if (std::exchange(m_lastWalk[alternative], m_walk) != m_walk)
                alternatives.append(alternative);
        };
        add(index);
        for (unsigned i = 0; i < alternatives.size(); ++i) {
            const Plan::Node& node = m_plan.nodes[alternatives[i]];
            if (++m_steps > maxAnalysisSteps) {
                m_analysis.isSupported = false;
                return;
            }
            if (uint32_t other = findAlias(node); other != Plan::none) {
                add(other);
                continue;
            }
            visit(node);
            if (node.shape != Shape::Union)
                continue;
            for (uint32_t arm : node.children)
                add(arm);
        }
    }

    unsigned computeInlinedSize(uint32_t index)
    {
        if (m_inlinedSize[index])
            return m_inlinedSize[index];
        const Plan::Node& node = m_plan.nodes[index];
        unsigned size = 1 + node.rules.size();
        auto add = [&](uint32_t child) {
            size = std::min(maxWeight, size + (m_analysis.isFunction[child] ? 1 : computeInlinedSize(child)));
        };
        forEachChildOfShape(node, add);
        for (auto& rule : node.rules) {
            for (uint32_t child : rule.nodes)
                add(child);
        }
        m_inlinedSize[index] = size;
        return size;
    }

    bool callsSharedFunction(uint32_t index)
    {
        if (m_analysis.isFunction[index])
            return true;
        if (m_reaches[index] != Reaches::NotAsked)
            return m_reaches[index] == Reaches::Yes;
        const Plan::Node& node = m_plan.nodes[index];
        bool reaches = false;
        forEachChildOfShape(node, [&](uint32_t child) {
            reaches = reaches || callsSharedFunction(child);
        });
        for (auto& rule : node.rules) {
            for (uint32_t child : rule.nodes)
                reaches = reaches || callsSharedFunction(child);
        }
        m_reaches[index] = reaches ? Reaches::Yes : Reaches::No;
        return reaches;
    }

    static bool appliesToShape(RuleKind kind, Shape shape)
    {
        switch (kind) {
        case RuleKind::Integer:
        case RuleKind::Minimum:
        case RuleKind::Maximum:
        case RuleKind::ExclusiveMinimum:
        case RuleKind::ExclusiveMaximum:
        case RuleKind::MultipleOf:
            return shape == Shape::Number;
        case RuleKind::MinimumLength:
        case RuleKind::MaximumLength:
            return shape == Shape::String || shape == Shape::Array || shape == Shape::Tuple;
        case RuleKind::Pattern:
        case RuleKind::Format:
        case RuleKind::Url:
        case RuleKind::Template:
            return shape == Shape::String;
        case RuleKind::Not:
        case RuleKind::If:
        case RuleKind::ExactlyOne:
        case RuleKind::Every:
            return true;
        case RuleKind::UniqueItems:
        case RuleKind::Contains:
            return shape == Shape::Array || shape == Shape::Tuple;
        case RuleKind::MinimumMembers:
        case RuleKind::MaximumMembers:
        case RuleKind::PatternMembers:
        case RuleKind::MemberNames:
        case RuleKind::DependentMembers:
        case RuleKind::DependentNode:
            return shape == Shape::Object || shape == Shape::Dictionary;
        }
        return false;
    }

    static unsigned toNodeIndexes(RuleKind kind)
    {
        switch (kind) {
        case RuleKind::Not:
        case RuleKind::Contains:
        case RuleKind::MemberNames:
        case RuleKind::DependentNode:
            return 1;
        case RuleKind::If:
            return 3;
        case RuleKind::Integer:
        case RuleKind::Minimum:
        case RuleKind::Maximum:
        case RuleKind::ExclusiveMinimum:
        case RuleKind::ExclusiveMaximum:
        case RuleKind::MultipleOf:
        case RuleKind::MinimumLength:
        case RuleKind::MaximumLength:
        case RuleKind::MinimumMembers:
        case RuleKind::MaximumMembers:
        case RuleKind::Pattern:
        case RuleKind::Format:
        case RuleKind::Url:
        case RuleKind::Template:
        case RuleKind::UniqueItems:
        case RuleKind::PatternMembers:
        case RuleKind::DependentMembers:
        case RuleKind::ExactlyOne:
        case RuleKind::Every:
            return 0;
        }
    }

    unsigned visit(uint32_t index)
    {
        if (index >= m_plan.nodes.size() || m_depth > maxPlanDepth) {
            m_analysis.isSupported = false;
            return 0;
        }
        SetForScope depth(m_depth, m_depth + 1);
        ++m_uses[index];
        if (m_state[index] == State::Open) {
            m_analysis.isFunction[index] = true;
            m_analysis.isRecursive[index] = true;
            return 1;
        }
        if (m_state[index] == State::Done)
            return m_weight[index];
        m_state[index] = State::Open;
        const Plan::Node& node = m_plan.nodes[index];
        unsigned weight = 1 + node.rules.size();
        auto add = [&](unsigned more) {
            weight = std::min(maxWeight, weight + std::min(maxWeight, more));
        };
        bool needsChild = node.shape == Shape::Array || node.shape == Shape::Dictionary || (node.shape == Shape::Object && node.undeclared == Plan::Undeclared::Validated);
        if (needsChild && node.child == Plan::none)
            m_analysis.isSupported = false;
        if (node.shape == Shape::Tuple && node.required > node.children.size())
            m_analysis.isSupported = false;
        forEachChildOfShape(node, [&](uint32_t child) {
            add(visit(child));
        });
        for (auto& rule : node.rules) {
            if (!appliesToShape(rule.kind, node.shape) || (toNodeIndexes(rule.kind) && rule.nodes.size() != toNodeIndexes(rule.kind)))
                m_analysis.isSupported = false;
            if (rule.kind == RuleKind::PatternMembers && rule.texts.size() != rule.nodes.size())
                m_analysis.isSupported = false;
            if (rule.kind == RuleKind::Template && (rule.texts.size() < 2 || rule.text.length() + 1 != rule.texts.size()))
                m_analysis.isSupported = false;
            for (uint32_t child : rule.nodes)
                add(visit(child));
        }
        m_state[index] = State::Done;
        m_weight[index] = weight;
        return weight;
    }

    const Plan& m_plan;
    Analysis& m_analysis;
    Vector<State> m_state;
    Vector<Reaches> m_reaches;
    Vector<unsigned> m_weight;
    Vector<unsigned> m_uses;
    Vector<unsigned> m_lastWalk;
    Vector<unsigned> m_inlinedSize;
    unsigned m_walk { 0 };
    unsigned m_steps { 0 };
    unsigned m_depth { 0 };
};

struct Shared : ThreadSafeRefCounted<Shared> {
    Shared(Ref<Plan>&& plan, JSGlobalObject* globalObject, ValidatorKind kind)
        : plan(WTF::move(plan))
        , analysis(adoptRef(*new Analysis))
        , globalObject(globalObject)
        , kind(kind)
    {
    }

    bool limitsVisits() const { return kind != ValidatorKind::Parse && (analysis->hasFunctions || analysis->hasLoops); }
    bool countsVisits() const { return analysis->hasFunctions || limitsVisits(); }
    bool countsHoles() const { return kind != ValidatorKind::Parse && analysis->acceptsHoles; }
    bool hasState() const { return analysis->hasFunctions; }
    Mode mode() const { return kind == ValidatorKind::Validate || (kind == ValidatorKind::Parse && analysis->hasDefaults) ? Mode::Copy : Mode::Check; }

    const Ref<Plan> plan;
    const Ref<Analysis> analysis;
    JSGlobalObject* const globalObject;
    const ValidatorKind kind;
};

struct Functions {
    JSFunction* checkFunction { nullptr };
    JSFunction* copyFunction { nullptr };
    JSArray* state { nullptr };
};

class Emitter {
public:
    Emitter(Writer& assembler, Shared& shared, const Functions& functions, Mode mode)
        : a(assembler)
        , m_shared(shared)
        , m_plan(shared.plan.get())
        , m_analysis(shared.analysis.get())
        , m_vm(assembler.vm())
        , m_functions(functions)
        , m_mode(mode)
    {
        m_undefined = a.constant(jsUndefined());
        m_zero = a.constant(jsNumber(0));
        m_bailoutMarker = a.constant(createMarker(m_vm, bailoutMarkerName));
        m_failureMarker = a.constant(createMarker(m_vm, failureMarkerName));
        if (functions.state)
            m_state = a.constant(functions.state);
    }

    void emitCheck()
    {
        Target fail = a.newLabel();
        m_bailout = fail;
        m_tooManyObjects = fail;
        emitResetState();
        emitNodeOrCall(m_plan.root, a.argument(0), fail, false);
        a.returnValue(a.constant(jsBoolean(true)));
        a.bind(fail);
        a.returnValue(a.constant(jsBoolean(false)));
        emitDeferred();
    }

    void emitValidate(JSObject* onRefused, JSObject* onBailout)
    {
        Target fail = a.newLabel();
        m_bailout = a.newLabel();
        m_tooManyObjects = a.newLabel();
        Slot value = a.newRegister();
        emitResetState();
        a.move(value, a.argument(0));
        emitNodeOrCall(m_plan.root, value, fail, false);
        a.returnValue(value);
        Slot given[] = { a.argument(0) };
        a.bind(fail);
        a.call(value, onRefused, given);
        a.returnValue(value);
        a.bind(m_bailout);
        a.call(value, onBailout, given);
        a.returnValue(value);
        emitThrowTooManyObjects();
        emitDeferred();
    }

    void emitParse(JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments)
    {
        Target fail = a.newLabel();
        Target isOther = a.newLabel();
        m_bailout = a.newLabel();
        m_tooManyObjects = m_bailout;
        Slot parsed = a.newRegister();
        Slot value = a.newRegister();
        Slot count = a.newRegister();
        Slot one = a.constant(jsNumber(1));
        a.readArgumentCount(count);
        a.jumpIfStrictlyEqual(count, one, false, isOther);
        a.jumpIfCellType(a.argument(0), StringType, false, isOther);
        callHelper(parsed, m_parseJSONText, parseJSONText, { a.argument(0) });
        emitResetState();
        a.move(value, parsed);
        emitNodeOrCall(m_plan.root, value, fail, false);
        a.returnValue(m_mode == Mode::Copy ? value : parsed);
        Slot made[] = { parsed };
        a.bind(fail);
        a.call(value, onRefused, made);
        a.returnValue(value);
        a.bind(m_bailout);
        a.call(value, onBailout, made);
        a.returnValue(value);
        a.bind(isOther);
        Target hasNone = a.newLabel();
        Target hasOne = a.newLabel();
        Slot given[] = { a.argument(0), a.argument(1) };
        a.jumpIfCompares(Comparison::Less, count, one, true, hasNone);
        a.jumpIfStrictlyEqual(count, one, true, hasOne);
        a.call(value, onOtherArguments, given);
        a.returnValue(value);
        a.bind(hasOne);
        a.call(value, onOtherArguments, std::span<const Slot>(given, 1));
        a.returnValue(value);
        a.bind(hasNone);
        a.call(value, onOtherArguments, { });
        a.returnValue(value);
        emitDeferred();
    }

    void emitSharedNodesFunction()
    {
        m_bailout = a.newLabel();
        m_tooManyObjects = m_shared.kind == ValidatorKind::Validate ? a.newLabel() : m_bailout;
        bool isCopying = m_mode == Mode::Copy;
        Vector<std::pair<uint32_t, Target>> entries;
        for (unsigned i = 0; i < m_plan.nodes.size(); ++i) {
            if (!m_analysis.isFunction[i])
                continue;
            entries.append({ i, a.newLabel() });
            a.jumpIfStrictlyEqual(a.argument(1), a.constant(jsNumber(i)), true, entries.last().second);
        }
        a.jump(m_bailout);
        for (auto [node, label] : entries) {
            RegisterScope scope(a);
            a.bind(label);
            Target fail = a.newLabel();
            Slot value = a.newRegister();
            a.move(value, a.argument(0));
            bool memoizes = !isCopying && m_analysis.isRecursive[node];
            Slot memory = memoizes ? a.newRegister() : nullptr;
            Slot verdict = memoizes ? a.newRegister() : nullptr;
            Target isNotKnown = a.newLabel();
            Target finish = a.newLabel();
            if (memoizes) {
                a.readElement(memory, m_state, a.constant(jsNumber(StateMemo)));
                a.jumpIfStrictlyEqual(memory, m_undefined, true, isNotKnown);
                callHelper(verdict, m_findVerdict, findVerdict, { memory, a.argument(1), a.argument(0) });
                a.jumpIfStrictlyEqual(verdict, m_undefined, true, isNotKnown);
                a.returnValue(verdict);
                a.bind(isNotKnown);
            }
            emitNode(node, value, fail, true);
            if (!memoizes) {
                a.returnValue(isCopying ? value : a.constant(jsBoolean(true)));
                a.bind(fail);
                a.returnValue(isCopying ? m_failureMarker : a.constant(jsBoolean(false)));
                continue;
            }
            a.move(verdict, a.constant(jsBoolean(true)));
            a.jump(finish);
            a.bind(fail);
            a.move(verdict, a.constant(jsBoolean(false)));
            a.bind(finish);
            Target isDone = a.newLabel();
            a.readElement(memory, m_state, a.constant(jsNumber(StateMemo)));
            a.jumpIfStrictlyEqual(memory, m_undefined, true, isDone);
            callHelper(value, m_storeVerdict, storeVerdict, { memory, a.argument(1), a.argument(0), verdict });
            a.bind(isDone);
            a.returnValue(verdict);
        }
        a.bind(m_bailout);
        a.returnValue(isCopying ? m_bailoutMarker : m_undefined);
        if (m_tooManyObjects != m_bailout)
            emitThrowTooManyObjects();
        emitDeferred();
    }

private:
    class RegisterScope {
    public:
        explicit RegisterScope(Writer& assembler)
            : m_assembler(assembler)
            , m_count(assembler.liveRegisterCount())
        {
        }
        ~RegisterScope() { m_assembler.releaseRegistersAbove(m_count); }

    private:
        Writer& m_assembler;
        unsigned m_count;
    };

    const Plan::Node& getNode(uint32_t index) const { return m_plan.nodes[index]; }
    bool isCopying() const { return m_mode == Mode::Copy; }
    bool changes(uint32_t index) const { return isCopying() && (m_analysis.describesContainers[index] || m_analysis.appliesDefault[index]); }
    bool acceptsAnything(uint32_t index) const { return getNode(index).shape == Shape::Unknown && getNode(index).rules.isEmpty() && getNode(index).defaultKind == Plan::Literal::None; }

    JSValue helper(NativeFunction function, unsigned length, Intrinsic intrinsic = NoIntrinsic)
    {
        return JSFunction::create(m_vm, m_shared.globalObject, length, String(), function, ImplementationVisibility::Private, intrinsic);
    }

    void callHelper(Slot destination, JSValue& cached, NativeFunction function, std::initializer_list<Slot> arguments, Intrinsic intrinsic = NoIntrinsic)
    {
        if (!cached)
            cached = helper(function, arguments.size(), intrinsic);
        a.call(destination, cached, std::span<const Slot>(arguments.begin(), arguments.size()));
    }

    void defer(std::initializer_list<Slot> used, Function<void()>&& block)
    {
        for (Slot one : used) {
            if (one)
                a.pinRegister(one);
        }
        a.pinLiveRegisters();
        m_deferred.append(WTF::move(block));
    }

    void emitDeferred()
    {
        for (unsigned i = 0; i < m_deferred.size(); ++i)
            m_deferred[i]();
    }

    void emitResetState()
    {
        if (m_state) {
            a.writeOwnElement(m_state, a.constant(jsNumber(StateVisitCount)), m_zero);
            a.writeOwnElement(m_state, a.constant(jsNumber(StateMemo)), m_undefined);
            a.writeOwnElement(m_state, a.constant(jsNumber(StateScannedObjects)), m_undefined);
            if (m_shared.countsHoles())
                a.writeOwnElement(m_state, a.constant(jsNumber(StateHolesLeft)), a.constant(jsNumber(maxHoles)));
            return;
        }
        if (m_shared.countsVisits()) {
            m_visits = a.newRegister();
            a.move(m_visits, m_zero);
        }
        if (m_shared.countsHoles()) {
            m_holes = a.newRegister();
            a.move(m_holes, a.constant(jsNumber(maxHoles)));
        }
    }

    void emitThrowTooManyObjects()
    {
        a.bind(m_tooManyObjects);
        Slot nothing = a.newRegister();
        callHelper(nothing, m_throwTooManyObjects, throwTooManyObjects, { });
        a.returnValue(nothing);
    }

    void readVisits(Slot destination)
    {
        if (m_state)
            a.readElement(destination, m_state, a.constant(jsNumber(StateVisitCount)));
        else
            a.move(destination, m_visits);
    }

    void emitCountVisit(const Plan::Node& node)
    {
        if (!m_shared.countsVisits() || !m_analysis.holdsContainers[&node - m_plan.nodes.span().data()])
            return;
        RegisterScope scope(a);
        Slot visits = m_visits;
        if (m_state) {
            visits = a.newRegister();
            readVisits(visits);
        }
        a.increment(visits);
        if (m_state)
            a.writeOwnElement(m_state, a.constant(jsNumber(StateVisitCount)), visits);
        if (m_shared.limitsVisits())
            a.jumpIfCompares(Comparison::Greater, visits, a.constant(jsNumber(maxVisitedObjects)), true, m_tooManyObjects);
    }

    // Without memoizing, arms that each recurse into the same value double the work at every level.
    void emitMemoizeAfterDeepFailure(Slot visitsBefore)
    {
        RegisterScope scope(a);
        Slot visits = a.newRegister();
        Target isCheap = a.newLabel();
        readVisits(visits);
        a.compute(Arithmetic::Subtract, visits, visits, visitsBefore);
        a.jumpIfCompares(Comparison::Greater, visits, a.constant(jsNumber(1)), false, isCheap);
        callHelper(visits, m_startMemoizing, startMemoizing, { m_state });
        a.bind(isCheap);
    }

    void emitStartMemoizing(std::span<const uint32_t> nodes)
    {
        bool reaches = false;
        for (uint32_t node : nodes)
            reaches |= m_analysis.callsSharedFunction[node];
        if (!reaches)
            return;
        RegisterScope scope(a);
        Slot memory = a.newRegister();
        Target memoizes = a.newLabel();
        a.readElement(memory, m_state, a.constant(jsNumber(StateMemo)));
        a.jumpIfStrictlyEqual(memory, m_undefined, false, memoizes);
        callHelper(memory, m_startMemoizing, startMemoizing, { m_state });
        a.bind(memoizes);
    }

    void emitNodeOrCall(uint32_t index, Slot value, Target fail, bool isSoft)
    {
        if (!m_analysis.isFunction[index]) {
            emitNode(index, value, fail, isSoft);
            return;
        }
        RegisterScope scope(a);
        Slot result = a.newRegister();
        Slot arguments[] = { value, a.constant(jsNumber(index)) };
        if (isCopying()) {
            a.call(result, m_functions.copyFunction, arguments);
            a.jumpIfStrictlyEqual(result, m_failureMarker, true, fail);
            a.jumpIfStrictlyEqual(result, m_bailoutMarker, true, m_bailout);
            a.move(value, result);
            return;
        }
        a.call(result, m_functions.checkFunction, arguments);
        Target passed = a.newLabel();
        a.jumpIfTruthy(result, true, passed);
        a.jumpIfStrictlyEqual(result, m_undefined, true, m_bailout);
        a.jump(fail);
        a.bind(passed);
    }

    void emitCheckOnly(uint32_t index, Slot value, Target fail, bool isSoft)
    {
        SetForScope mode(m_mode, Mode::Check);
        if (!m_analysis.describesContainers[index] && !m_analysis.appliesDefault[index]) {
            emitNodeOrCall(index, value, fail, isSoft);
            return;
        }
        RegisterScope scope(a);
        Slot tested = a.newRegister();
        a.move(tested, value);
        emitNodeOrCall(index, tested, fail, isSoft);
    }

    uint32_t getIndex(const Plan::Node& node) const { return &node - m_plan.nodes.span().data(); }

    // A rule that passes when a node fails must not pass because of a value JSON cannot hold.
    void emitRequireOnlyJSON(const Plan::Node& node, Slot value, Target fail)
    {
        switch (node.shape) {
        case Shape::Null:
        case Shape::String:
        case Shape::Number:
        case Shape::Boolean:
        case Shape::BooleanLiteral:
        case Shape::StringLiteral:
            return;
        case Shape::Unknown:
        case Shape::Never:
        case Shape::Undefined:
        case Shape::AnyObject:
        case Shape::BigInt:
        case Shape::NumberLiteral:
        case Shape::Array:
        case Shape::Tuple:
        case Shape::Dictionary:
        case Shape::Object:
        case Shape::Union:
            break;
        }
        RegisterScope scope(a);
        Slot answer = a.newRegister();
        callHelper(answer, m_containsOnlyJSON, containsOnlyJSON, { value, m_bailoutMarker, m_state ? m_state : m_undefined });
        a.jumpIfStrictlyEqual(answer, m_bailoutMarker, true, m_bailout);
        a.jumpIfTruthy(answer, false, fail);
    }

    JSValue getDefaultValue(const Plan::Node& node)
    {
        switch (node.defaultKind) {
        case Plan::Literal::None:
            break;
        case Plan::Literal::Null:
            return jsNull();
        case Plan::Literal::Boolean:
            return jsBoolean(node.defaultBoolean);
        case Plan::Literal::Number:
            return jsNumber(node.defaultNumber);
        case Plan::Literal::String:
            return jsString(m_vm, node.defaultString);
        }
        return { };
    }

    void emitNode(uint32_t index, Slot value, Target fail, bool isSoft)
    {
        const Plan::Node& node = getNode(index);
        if (JSValue defaultValue = getDefaultValue(node)) {
            Target hasValue = a.newLabel();
            a.jumpIfStrictlyEqual(value, m_undefined, false, hasValue);
            a.move(value, a.constant(defaultValue));
            a.bind(hasValue);
        }
        Target wrongShape = fail;
        switch (node.shape) {
        case Shape::Unknown:
            break;
        case Shape::Never:
            a.jump(wrongShape);
            return;
        case Shape::Undefined:
            a.jumpIfStrictlyEqual(value, m_undefined, false, wrongShape);
            break;
        case Shape::Null:
            a.jumpIfStrictlyEqual(value, a.constant(jsNull()), false, wrongShape);
            break;
        case Shape::AnyObject:
            a.jumpIfObject(value, false, wrongShape);
            break;
        case Shape::Boolean:
            a.jumpIfBoolean(value, false, wrongShape);
            break;
        case Shape::BigInt:
            a.jumpIfCellType(value, HeapBigIntType, false, wrongShape);
            break;
        case Shape::BooleanLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsBoolean(node.boolean)), false, wrongShape);
            break;
        case Shape::NumberLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsNumber(node.number)), false, wrongShape);
            break;
        case Shape::StringLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsString(m_vm, node.string)), false, wrongShape);
            break;
        case Shape::Number:
            emitNumber(node, value, wrongShape);
            break;
        case Shape::String:
            emitString(node, value, wrongShape, fail);
            break;
        case Shape::Array:
        case Shape::Tuple:
            emitArray(node, value, wrongShape, fail, isSoft);
            break;
        case Shape::Dictionary:
        case Shape::Object:
            emitObjectGuard(readsValuesByKey(node), value, wrongShape);
            emitObject(node, value, fail, isSoft, nullptr);
            break;
        case Shape::Union:
            emitUnion(node, value, fail, isSoft);
            break;
        }
        emitRulesOfAnyShape(node, value, fail, isSoft);
    }

    void emitRulesOfAnyShape(const Plan::Node& node, Slot value, Target fail, bool isSoft)
    {
        for (auto& rule : node.rules) {
            switch (rule.kind) {
            case RuleKind::Not: {
                Target refused = a.newLabel();
                emitRequireOnlyJSON(node, value, fail);
                emitStartMemoizing(rule.nodes.span());
                emitCheckOnly(rule.nodes[0], value, refused, true);
                a.jump(fail);
                a.bind(refused);
                break;
            }
            case RuleKind::If: {
                Target otherwise = a.newLabel();
                Target done = a.newLabel();
                emitRequireOnlyJSON(node, value, fail);
                emitStartMemoizing(rule.nodes.span());
                emitCheckOnly(rule.nodes[0], value, otherwise, true);
                emitCheckOnly(rule.nodes[1], value, fail, isSoft);
                a.jump(done);
                a.bind(otherwise);
                emitCheckOnly(rule.nodes[2], value, fail, isSoft);
                a.bind(done);
                break;
            }
            case RuleKind::ExactlyOne: {
                RegisterScope scope(a);
                Slot hasPassed = a.newRegister();
                emitRequireOnlyJSON(node, value, fail);
                emitStartMemoizing(rule.nodes.span());
                a.move(hasPassed, a.constant(jsBoolean(false)));
                for (uint32_t arm : rule.nodes) {
                    Target missed = a.newLabel();
                    emitCheckOnly(arm, value, missed, true);
                    a.jumpIfTruthy(hasPassed, true, fail);
                    a.move(hasPassed, a.constant(jsBoolean(true)));
                    a.bind(missed);
                }
                a.jumpIfTruthy(hasPassed, false, fail);
                break;
            }
            case RuleKind::Every:
                if (findAlias(node) != Plan::none) {
                    emitNodeOrCall(rule.nodes[0], value, fail, isSoft);
                    break;
                }
                if (rule.nodes.size() > 1)
                    emitStartMemoizing(rule.nodes.span());
                for (uint32_t part : rule.nodes)
                    emitCheckOnly(part, value, fail, isSoft);
                break;
            case RuleKind::Integer:
            case RuleKind::Minimum:
            case RuleKind::Maximum:
            case RuleKind::ExclusiveMinimum:
            case RuleKind::ExclusiveMaximum:
            case RuleKind::MultipleOf:
            case RuleKind::MinimumLength:
            case RuleKind::MaximumLength:
            case RuleKind::MinimumMembers:
            case RuleKind::MaximumMembers:
            case RuleKind::Pattern:
            case RuleKind::Format:
            case RuleKind::Url:
            case RuleKind::Template:
            case RuleKind::UniqueItems:
            case RuleKind::Contains:
            case RuleKind::PatternMembers:
            case RuleKind::MemberNames:
            case RuleKind::DependentMembers:
            case RuleKind::DependentNode:
                break;
            }
        }
    }

    void emitNumber(const Plan::Node& node, Slot value, Target fail)
    {
        RegisterScope scope(a);
        a.jumpIfNumber(value, false, fail);
        bool isInteger = false;
        bool hasLower = false;
        bool hasUpper = false;
        for (auto& rule : node.rules) {
            isInteger |= rule.kind == RuleKind::Integer;
            hasLower |= (rule.kind == RuleKind::Minimum || rule.kind == RuleKind::ExclusiveMinimum) && std::isfinite(rule.number);
            hasUpper |= (rule.kind == RuleKind::Maximum || rule.kind == RuleKind::ExclusiveMaximum) && std::isfinite(rule.number);
        }
        Slot scratch = a.newRegister();
        if (isInteger) {
            Target isWhole = a.newLabel();
            a.compute(Arithmetic::BitOr, scratch, value, m_zero);
            a.jumpIfStrictlyEqual(scratch, value, true, isWhole);
            a.compute(Arithmetic::Remainder, scratch, value, a.constant(jsNumber(1)));
            a.jumpIfStrictlyEqual(scratch, m_zero, false, fail);
            a.bind(isWhole);
        } else if (!hasLower || !hasUpper) {
            a.compute(Arithmetic::Subtract, scratch, value, value);
            a.jumpIfStrictlyEqual(scratch, m_zero, false, fail);
        }
        for (auto& rule : node.rules) {
            Slot bound = a.constant(jsNumber(rule.number));
            switch (rule.kind) {
            case RuleKind::Minimum:
                a.jumpIfCompares(Comparison::GreaterOrEqual, value, bound, false, fail);
                break;
            case RuleKind::Maximum:
                a.jumpIfCompares(Comparison::LessOrEqual, value, bound, false, fail);
                break;
            case RuleKind::ExclusiveMinimum:
                a.jumpIfCompares(Comparison::Greater, value, bound, false, fail);
                break;
            case RuleKind::ExclusiveMaximum:
                a.jumpIfCompares(Comparison::Less, value, bound, false, fail);
                break;
            case RuleKind::MultipleOf:
                if ((rule.flags & Plan::compareAsDecimals) && rule.number != std::trunc(rule.number)) {
                    callHelper(scratch, m_isDecimalMultiple, isDecimalMultiple, { value, bound });
                    a.jumpIfTruthy(scratch, false, fail);
                    break;
                }
                a.compute(Arithmetic::Remainder, scratch, value, bound);
                a.jumpIfStrictlyEqual(scratch, m_zero, false, fail);
                break;
            case RuleKind::Integer:
            case RuleKind::MinimumLength:
            case RuleKind::MaximumLength:
            case RuleKind::MinimumMembers:
            case RuleKind::MaximumMembers:
            case RuleKind::Pattern:
            case RuleKind::Format:
            case RuleKind::Url:
            case RuleKind::Template:
            case RuleKind::UniqueItems:
            case RuleKind::Contains:
            case RuleKind::PatternMembers:
            case RuleKind::MemberNames:
            case RuleKind::DependentMembers:
            case RuleKind::DependentNode:
            case RuleKind::Not:
            case RuleKind::If:
            case RuleKind::ExactlyOne:
            case RuleKind::Every:
                break;
            }
        }
    }

    JSArray* createStringArray(const auto& strings, NOESCAPE const auto& toString)
    {
        JSArray* array = constructEmptyArray(m_shared.globalObject, nullptr);
        unsigned position = 0;
        for (auto& one : strings)
            array->putDirectIndex(m_shared.globalObject, position++, jsOwnedString(m_vm, toString(one)));
        return array;
    }

    void emitString(const Plan::Node& node, Slot value, Target wrongShape, Target fail)
    {
        RegisterScope scope(a);
        a.jumpIfCellType(value, StringType, false, wrongShape);
        Slot units = nullptr;
        Slot scratch = nullptr;
        auto needUnits = [&] {
            if (units)
                return;
            units = a.newRegister();
            scratch = a.newRegister();
            a.readLength(units, value);
        };
        for (auto& rule : node.rules) {
            switch (rule.kind) {
            case RuleKind::MinimumLength: {
                if (!rule.count)
                    break;
                needUnits();
                a.jumpIfCompares(Comparison::Less, units, a.constant(jsNumber(rule.count)), true, fail);
                if (rule.count == 1)
                    break;
                Target isLongEnough = a.newLabel();
                a.jumpIfCompares(Comparison::GreaterOrEqual, units, a.constant(jsNumber(2.0 * rule.count - 1)), true, isLongEnough);
                callHelper(scratch, m_countCodePoints, countCodePoints, { value });
                a.jumpIfCompares(Comparison::Less, scratch, a.constant(jsNumber(rule.count)), true, fail);
                a.bind(isLongEnough);
                break;
            }
            case RuleKind::MaximumLength: {
                needUnits();
                Target isShortEnough = a.newLabel();
                a.jumpIfCompares(Comparison::LessOrEqual, units, a.constant(jsNumber(rule.count)), true, isShortEnough);
                a.jumpIfCompares(Comparison::Greater, units, a.constant(jsNumber(2.0 * rule.count)), true, fail);
                callHelper(scratch, m_countCodePoints, countCodePoints, { value });
                a.jumpIfCompares(Comparison::Greater, scratch, a.constant(jsNumber(rule.count)), true, fail);
                a.bind(isShortEnough);
                break;
            }
            case RuleKind::Integer:
            case RuleKind::Minimum:
            case RuleKind::Maximum:
            case RuleKind::ExclusiveMinimum:
            case RuleKind::ExclusiveMaximum:
            case RuleKind::MultipleOf:
            case RuleKind::MinimumMembers:
            case RuleKind::MaximumMembers:
            case RuleKind::Pattern:
            case RuleKind::Format:
            case RuleKind::Url:
            case RuleKind::Template:
            case RuleKind::UniqueItems:
            case RuleKind::Contains:
            case RuleKind::PatternMembers:
            case RuleKind::MemberNames:
            case RuleKind::DependentMembers:
            case RuleKind::DependentNode:
            case RuleKind::Not:
            case RuleKind::If:
            case RuleKind::ExactlyOne:
            case RuleKind::Every:
                break;
            }
        }
        for (auto& rule : node.rules) {
            if (rule.kind < RuleKind::Pattern || rule.kind > RuleKind::Template)
                continue;
            if (!scratch)
                scratch = a.newRegister();
            switch (rule.kind) {
            case RuleKind::Pattern:
                callHelper(scratch, m_matchesPattern, matchesPattern, { a.constant(RegExp::create(m_vm, rule.text, OptionSet<Yarr::Flags>::fromRaw(rule.flags))), value });
                break;
            case RuleKind::Format:
                callHelper(scratch, m_matchesFormat, matchesFormat, { a.constant(jsNumber(rule.count)), value });
                break;
            case RuleKind::Url:
                callHelper(scratch, m_isValidURL, isValidURL, { value });
                break;
            case RuleKind::Integer:
            case RuleKind::Minimum:
            case RuleKind::Maximum:
            case RuleKind::ExclusiveMinimum:
            case RuleKind::ExclusiveMaximum:
            case RuleKind::MultipleOf:
            case RuleKind::MinimumLength:
            case RuleKind::MaximumLength:
            case RuleKind::MinimumMembers:
            case RuleKind::MaximumMembers:
            case RuleKind::Template:
            case RuleKind::UniqueItems:
            case RuleKind::Contains:
            case RuleKind::PatternMembers:
            case RuleKind::MemberNames:
            case RuleKind::DependentMembers:
            case RuleKind::DependentNode:
            case RuleKind::Not:
            case RuleKind::If:
            case RuleKind::ExactlyOne:
            case RuleKind::Every:
                callHelper(scratch, m_matchesTemplate, matchesTemplate, { value, a.constant(createStringArray(rule.texts, [](const String& text) { return text; })), a.constant(jsOwnedString(m_vm, rule.text)) });
                break;
            }
            a.jumpIfTruthy(scratch, false, fail);
        }
    }

    bool readsValuesByKey(const Plan::Node& node) const
    {
        if (findRule(node, RuleKind::PatternMembers))
            return true;
        if (node.shape == Shape::Dictionary)
            return !acceptsAnything(node.child);
        return node.undeclared == Plan::Undeclared::Validated && !acceptsAnything(node.child);
    }

    void emitObjectGuard(bool readsValuesByKey, Slot value, Target fail)
    {
        if (isCopying()) {
            Target isOther = a.newLabel();
            Slot copy = a.newRegister();
            a.jumpIfCellType(value, FinalObjectType, false, isOther);
            callHelper(copy, m_clonePlainObject, clonePlainObject, { value });
            a.jumpIfStrictlyEqual(copy, m_undefined, true, m_bailout);
            a.move(value, copy);
            defer({ value }, [=, this] {
                a.bind(isOther);
                a.jumpIfObject(value, false, fail);
                a.jumpIfCellType(value, ArrayType, true, fail);
                a.jumpIfCellType(value, DerivedArrayType, true, fail);
                a.jump(m_bailout);
            });
            return;
        }
        Target isExotic = a.newLabel();
        Target isPlain = a.newLabel();
        Slot copy = a.newRegister();
        a.jumpIfCellType(value, FinalObjectType, false, isExotic);
        a.jumpIfStructureFlags(value, Structure::s_hasAnyKindOfGetterSetterPropertiesBits | Structure::s_hasNonEnumerablePropertiesBits, true, isExotic);
        if (readsValuesByKey) {
            callHelper(copy, m_hasPlainElements, hasPlainElements, { value });
            a.jumpIfTruthy(copy, false, isExotic);
        }
        a.bind(isPlain);
        defer({ value, copy }, [=, this] {
            a.bind(isExotic);
            callHelper(copy, m_copyExoticObject, copyExoticObject, { value, m_bailoutMarker });
            a.jumpIfStrictlyEqual(copy, m_undefined, true, fail);
            a.jumpIfStrictlyEqual(copy, m_bailoutMarker, true, m_bailout);
            a.move(value, copy);
            a.jump(isPlain);
        });
    }

    unsigned getCost(uint32_t index) const
    {
        const Plan::Node& node = getNode(index);
        if (m_analysis.isFunction[index])
            return 2;
        switch (node.shape) {
        case Shape::Array:
        case Shape::Tuple:
        case Shape::Dictionary:
        case Shape::Object:
            return 2;
        case Shape::Union:
            for (uint32_t arm : node.children) {
                if (getNode(arm).shape != Shape::StringLiteral)
                    return 1;
            }
            break;
        case Shape::Unknown:
        case Shape::Never:
        case Shape::Undefined:
        case Shape::Null:
        case Shape::AnyObject:
        case Shape::String:
        case Shape::Number:
        case Shape::Boolean:
        case Shape::BigInt:
        case Shape::BooleanLiteral:
        case Shape::NumberLiteral:
        case Shape::StringLiteral:
            break;
        }
        for (auto& rule : node.rules) {
            if (rule.kind >= RuleKind::Pattern)
                return 1;
        }
        return 0;
    }

    static const Plan::Rule* findRule(const Plan::Node& node, RuleKind kind)
    {
        for (auto& rule : node.rules) {
            if (rule.kind == kind)
                return &rule;
        }
        return nullptr;
    }

    bool needsKeysLoop(const Plan::Node& node) const
    {
        return readsValuesByKey(node) || findRule(node, RuleKind::MemberNames);
    }

    void emitKeysLoop(const Plan::Node& node, Slot object, Target fail, bool isSoft)
    {
        RegisterScope scope(a);
        const Plan::Rule* names = findRule(node, RuleKind::MemberNames);
        const Plan::Rule* patterns = findRule(node, RuleKind::PatternMembers);
        bool rejects = node.shape == Shape::Object && node.undeclared == Plan::Undeclared::Rejected;
        uint32_t valueNode = node.shape == Shape::Dictionary || node.undeclared == Plan::Undeclared::Validated ? node.child : Plan::none;
        if (valueNode != Plan::none && acceptsAnything(valueNode))
            valueNode = Plan::none;
        Slot keys = a.newRegister();
        Slot count = a.newRegister();
        Slot index = a.newRegister();
        Slot key = a.newRegister();
        Slot current = a.newRegister();
        Slot isCovered = patterns ? a.newRegister() : nullptr;
        callHelper(keys, m_objectKeys, objectConstructorKeys, { object }, ObjectKeysIntrinsic);
        a.readLength(count, keys);
        a.move(index, m_zero);
        Target head = a.newLabel();
        Target next = a.newLabel();
        Target exit = a.newLabel();
        a.bind(head);
        a.loopHint();
        a.jumpIfCompares(Comparison::Less, index, count, false, exit);
        a.readElement(key, keys, index);
        if (names)
            emitCheckOnly(names->nodes[0], key, fail, isSoft);
        if (patterns) {
            a.move(isCovered, a.constant(jsBoolean(false)));
            for (unsigned i = 0; i < patterns->nodes.size(); ++i) {
                Target isNoMatch = a.newLabel();
                callHelper(current, m_matchesPattern, matchesPattern, { a.constant(RegExp::create(m_vm, patterns->texts[i], OptionSet<Yarr::Flags>::fromRaw(patterns->flags))), key });
                a.jumpIfTruthy(current, false, isNoMatch);
                a.move(isCovered, a.constant(jsBoolean(true)));
                if (!acceptsAnything(patterns->nodes[i])) {
                    a.readElement(current, object, key);
                    emitCheckOnly(patterns->nodes[i], current, fail, isSoft);
                }
                a.bind(isNoMatch);
            }
        }
        bool looksAtUndeclared = valueNode != Plan::none || (rejects && patterns);
        bool skipsDeclared = looksAtUndeclared && !node.members.isEmpty();
        Target undeclared = a.newLabel();
        if (skipsDeclared)
            a.beginStringSwitch(key);
        a.bind(undeclared);
        if (looksAtUndeclared) {
            if (patterns)
                a.jumpIfTruthy(isCovered, true, next);
            if (valueNode == Plan::none)
                a.jump(fail);
            else {
                a.readElement(current, object, key);
                emitNodeOrCall(valueNode, current, fail, isSoft);
                if (changes(valueNode))
                    a.writeOwnElement(object, key, current);
            }
        }
        a.bind(next);
        if (skipsDeclared) {
            Vector<Writer::StringCase, 16> cases;
            for (auto& member : node.members)
                cases.append({ member.key.impl(), next });
            a.endStringSwitch(cases.span(), undeclared);
        }
        a.increment(index);
        a.jump(head);
        a.bind(exit);
    }

    void readOwn(Slot destination, Slot object, const String& key)
    {
        Identifier name = Identifier::fromString(m_vm, key);
        if (!parseIndex(name)) {
            a.readOwnProperty(destination, object, name.impl());
            return;
        }
        callHelper(destination, m_getOwnValue, getOwnValue, { object, a.constant(jsOwnedString(m_vm, name.string())), m_bailoutMarker });
        a.jumpIfStrictlyEqual(destination, m_bailoutMarker, true, m_bailout);
    }

    void emitRulesOfObjects(const Plan::Node& node, Slot value, Target fail, bool isSoft)
    {
        for (auto& rule : node.rules) {
            RegisterScope scope(a);
            switch (rule.kind) {
            case RuleKind::MinimumMembers:
            case RuleKind::MaximumMembers: {
                Slot count = a.newRegister();
                callHelper(count, m_objectKeys, objectConstructorKeys, { value }, ObjectKeysIntrinsic);
                a.readLength(count, count);
                a.jumpIfCompares(rule.kind == RuleKind::MinimumMembers ? Comparison::Less : Comparison::Greater, count, a.constant(jsNumber(rule.count)), true, fail);
                break;
            }
            case RuleKind::DependentMembers:
            case RuleKind::DependentNode: {
                Slot current = a.newRegister();
                Target isAbsent = a.newLabel();
                readOwn(current, value, rule.text);
                a.jumpIfStrictlyEqual(current, m_undefined, true, isAbsent);
                for (auto& name : rule.texts) {
                    readOwn(current, value, name);
                    a.jumpIfStrictlyEqual(current, m_undefined, true, fail);
                }
                if (rule.kind == RuleKind::DependentNode) {
                    emitRequireOnlyJSON(node, value, fail);
                    emitStartMemoizing(rule.nodes.span());
                    emitCheckOnly(rule.nodes[0], value, fail, isSoft);
                }
                a.bind(isAbsent);
                break;
            }
            case RuleKind::Integer:
            case RuleKind::Minimum:
            case RuleKind::Maximum:
            case RuleKind::ExclusiveMinimum:
            case RuleKind::ExclusiveMaximum:
            case RuleKind::MultipleOf:
            case RuleKind::MinimumLength:
            case RuleKind::MaximumLength:
            case RuleKind::Pattern:
            case RuleKind::Format:
            case RuleKind::Url:
            case RuleKind::Template:
            case RuleKind::UniqueItems:
            case RuleKind::Contains:
            case RuleKind::PatternMembers:
            case RuleKind::MemberNames:
            case RuleKind::Not:
            case RuleKind::If:
            case RuleKind::ExactlyOne:
            case RuleKind::Every:
                break;
            }
        }
    }

    void readMember(Slot destination, Slot object, const Plan::Member& member)
    {
        if (!parseIndex(*member.key.impl())) {
            a.readOwnProperty(destination, object, member.key.impl());
            return;
        }
        callHelper(destination, m_getOwnValue, getOwnValue, { object, a.constant(jsOwnedString(m_vm, member.key.string())), m_bailoutMarker });
        a.jumpIfStrictlyEqual(destination, m_bailoutMarker, true, m_bailout);
    }

    void writeMember(Slot object, const Plan::Member& member, Slot value)
    {
        if (auto index = parseIndex(*member.key.impl()))
            a.writeOwnElement(object, a.constant(jsNumber(*index)), value);
        else
            a.writeOwnProperty(object, member.key.impl(), value);
    }

    void emitObject(const Plan::Node& node, Slot value, Target fail, bool isSoft, const UniquedStringImpl* chosenBy)
    {
        RegisterScope scope(a);
        emitCountVisit(node);
        if (node.shape == Shape::Dictionary) {
            emitRulesOfObjects(node, value, fail, isSoft);
            if (needsKeysLoop(node))
                emitKeysLoop(node, value, fail, isSoft);
            return;
        }
        bool isClosed = node.undeclared == Plan::Undeclared::Rejected && !findRule(node, RuleKind::PatternMembers);
        auto canBeAbsent = [&](const Plan::Member& member) {
            return member.isOptional || m_analysis.appliesDefault[member.node];
        };
        unsigned requiredKeyCount = 0;
        bool hasOptionalKeys = false;
        for (auto& member : node.members) {
            requiredKeyCount += !canBeAbsent(member);
            hasOptionalKeys |= canBeAbsent(member);
        }
        Slot current = a.newRegister();
        Slot presentKeyCount = nullptr;
        if (isClosed && hasOptionalKeys) {
            presentKeyCount = a.newRegister();
            a.move(presentKeyCount, a.constant(jsNumber(requiredKeyCount)));
        }
        bool writesDefaults = false;
        for (auto& member : node.members)
            writesDefaults |= isCopying() && m_analysis.appliesDefault[member.node];
        Vector<const Plan::Member*, 16> orderedMembers;
        for (unsigned cost = 0; cost < 3; ++cost) {
            for (auto& member : node.members) {
                if (writesDefaults ? !cost : getCost(member.node) == cost)
                    orderedMembers.append(&member);
            }
        }
        for (auto* next : orderedMembers) {
            auto& member = *next;
            if (member.key.impl() == chosenBy)
                continue;
            if (member.isOptional && acceptsAnything(member.node) && !isClosed)
                continue;
            bool appliesDefault = m_analysis.appliesDefault[member.node];
            Target isAbsent = a.newLabel();
            readMember(current, value, member);
            if (member.isOptional && !appliesDefault)
                a.jumpIfStrictlyEqual(current, m_undefined, true, isAbsent);
            else if (m_plan.treatsUndefinedAsMissing && !member.isOptional && !member.mustBeOwn && !appliesDefault && m_analysis.acceptsUndefined[member.node])
                a.jumpIfStrictlyEqual(current, m_undefined, true, fail);
            if (presentKeyCount && canBeAbsent(member) && !isCopying()) {
                Target isCounted = a.newLabel();
                a.jumpIfStrictlyEqual(current, m_undefined, true, isCounted);
                a.increment(presentKeyCount);
                a.bind(isCounted);
            }
            emitNodeOrCall(member.node, current, fail, isSoft);
            if (member.mustBeOwn) {
                Target isOwn = a.newLabel();
                Slot answer = a.newRegister();
                a.jumpIfStrictlyEqual(current, m_undefined, false, isOwn);
                callHelper(answer, m_hasOwnEnumerableKey, hasOwnEnumerableKey, { value, a.constant(jsOwnedString(m_vm, member.key.string())) });
                a.jumpIfTruthy(answer, false, fail);
                a.bind(isOwn);
            }
            if (changes(member.node)) {
                if (m_analysis.acceptsUndefined[member.node]) {
                    Target isNothing = a.newLabel();
                    a.jumpIfStrictlyEqual(current, m_undefined, true, isNothing);
                    writeMember(value, member, current);
                    a.bind(isNothing);
                } else
                    writeMember(value, member, current);
            }
            if (presentKeyCount && canBeAbsent(member) && isCopying()) {
                Target isCounted = a.newLabel();
                a.jumpIfStrictlyEqual(current, m_undefined, true, isCounted);
                a.increment(presentKeyCount);
                a.bind(isCounted);
            }
            a.bind(isAbsent);
        }
        if (isClosed) {
            Target checkEachKey = a.newLabel();
            Target done = a.newLabel();
            callHelper(current, m_objectKeys, objectConstructorKeys, { value }, ObjectKeysIntrinsic);
            a.readLength(current, current);
            a.jumpIfStrictlyEqual(current, presentKeyCount ? presentKeyCount : a.constant(jsNumber(requiredKeyCount)), false, checkEachKey);
            a.bind(done);
            const Plan::Node* declaredBy = &node;
            defer({ value, current }, [=, this] {
                a.bind(checkEachKey);
                JSArray* names = createStringArray(declaredBy->members, [](const Plan::Member& member) { return member.key.string(); });
                callHelper(current, m_hasOnlyDeclaredKeys, hasOnlyDeclaredKeys, { value, a.constant(names) });
                a.jumpIfTruthy(current, false, fail);
                a.jump(done);
            });
        }
        emitRulesOfObjects(node, value, fail, isSoft);
        if (needsKeysLoop(node))
            emitKeysLoop(node, value, fail, isSoft);
    }

    void emitArray(const Plan::Node& node, Slot value, Target wrongShape, Target fail, bool isSoft)
    {
        RegisterScope scope(a);
        Slot length = a.newRegister();
        unsigned first = node.shape == Shape::Tuple ? node.children.size() : 0;
        double minLength = node.shape == Shape::Tuple ? node.required : 0;
        double maxLength = node.shape == Shape::Tuple && node.child == Plan::none ? first : std::numeric_limits<uint32_t>::max();
        for (auto& rule : node.rules) {
            if (rule.kind == RuleKind::MinimumLength)
                minLength = std::max<double>(minLength, rule.count);
            else if (rule.kind == RuleKind::MaximumLength)
                maxLength = std::min<double>(maxLength, rule.count);
        }
        bool acceptsHoles = node.child != Plan::none && m_analysis.acceptsUndefined[node.child];
        for (uint32_t child : node.children)
            acceptsHoles |= m_analysis.acceptsUndefined[child];

        if (isCopying()) {
            Target isOther = a.newLabel();
            Target isExotic = a.newLabel();
            a.jumpIfCellType(value, ArrayType, false, isOther);
            callHelper(length, m_clonePlainArray, clonePlainArray, { value, a.constant(jsNumber(minLength)), a.constant(jsNumber(maxLength)), m_bailoutMarker });
            a.jumpIfCellType(length, ArrayType, false, isExotic);
            a.move(value, length);
            a.readLength(length, value);
            defer({ value, length }, [=, this] {
                a.bind(isOther);
                a.jumpIfCellType(value, DerivedArrayType, true, m_bailout);
                a.jumpIfCellType(value, ProxyObjectType, true, m_bailout);
                a.jump(wrongShape);
                a.bind(isExotic);
                a.jumpIfStrictlyEqual(length, m_undefined, true, fail);
                a.jump(m_bailout);
            });
        } else {
            Target isExotic = a.newLabel();
            Target isPlain = a.newLabel();
            Target hasHoles = a.newLabel();
            bool countsHoles = acceptsHoles && m_shared.countsHoles();
            Slot holes = countsHoles ? a.newRegister() : nullptr;
            a.jumpIfCellType(value, ArrayType, false, isExotic);
            callHelper(length, m_getPlainArrayLength, getPlainArrayLength, { value });
            a.jumpIfCompares(Comparison::Less, length, m_zero, true, isExotic);
            if (countsHoles) {
                callHelper(holes, m_countHoles, countHoles, { value });
                a.jumpIfTruthy(holes, true, hasHoles);
            }
            a.bind(isPlain);
            defer({ value, length, holes }, [=, this] {
                a.bind(isExotic);
                if (countsHoles)
                    callHelper(holes, m_countHoles, countHoles, { value });
                callHelper(length, m_copyExoticArray, copyExoticArray, { value, m_bailoutMarker });
                a.jumpIfStrictlyEqual(length, m_undefined, true, wrongShape);
                a.jumpIfStrictlyEqual(length, m_bailoutMarker, true, m_bailout);
                a.move(value, length);
                a.readLength(length, value);
                if (!countsHoles) {
                    a.jump(isPlain);
                    return;
                }
                a.bind(hasHoles);
                Slot left = m_holes;
                if (m_state) {
                    left = a.newRegister();
                    a.readElement(left, m_state, a.constant(jsNumber(StateHolesLeft)));
                }
                a.compute(Arithmetic::Subtract, left, left, holes);
                if (m_state)
                    a.writeOwnElement(m_state, a.constant(jsNumber(StateHolesLeft)), left);
                a.jumpIfCompares(Comparison::Less, left, m_zero, true, m_bailout);
                a.jump(isPlain);
            });
            if (minLength)
                a.jumpIfCompares(Comparison::Less, length, a.constant(jsNumber(minLength)), true, fail);
            if (maxLength < std::numeric_limits<uint32_t>::max())
                a.jumpIfCompares(Comparison::Greater, length, a.constant(jsNumber(maxLength)), true, fail);
        }
        emitCountVisit(node);

        Slot current = a.newRegister();
        Target done = a.newLabel();
        for (unsigned i = 0; i < first; ++i) {
            Slot position = a.constant(jsNumber(i));
            if (i >= node.required)
                a.jumpIfCompares(Comparison::Greater, length, position, false, done);
            if (acceptsAnything(node.children[i]))
                continue;
            a.readElement(current, value, position);
            emitNodeOrCall(node.children[i], current, fail, isSoft);
            if (changes(node.children[i]))
                a.writeOwnElement(value, position, current);
        }
        if (node.child != Plan::none && !acceptsAnything(node.child)) {
            Slot index = a.newRegister();
            a.move(index, a.constant(jsNumber(first)));
            Target head = a.newLabel();
            a.bind(head);
            a.loopHint();
            a.jumpIfCompares(Comparison::Less, index, length, false, done);
            a.readElement(current, value, index);
            emitNodeOrCall(node.child, current, fail, isSoft);
            if (changes(node.child))
                a.writeOwnElement(value, index, current);
            a.increment(index);
            a.jump(head);
        }
        a.bind(done);

        for (auto& rule : node.rules) {
            if (rule.kind == RuleKind::UniqueItems) {
                callHelper(current, m_hasUniqueItems, hasUniqueItems, { value, m_bailoutMarker });
                a.jumpIfStrictlyEqual(current, m_bailoutMarker, true, m_bailout);
                a.jumpIfTruthy(current, false, fail);
            }
            if (rule.kind != RuleKind::Contains)
                continue;
            RegisterScope ruleScope(a);
            Slot index = a.newRegister();
            Slot found = a.newRegister();
            Target head = a.newLabel();
            Target missed = a.newLabel();
            Target exit = a.newLabel();
            Target enough = a.newLabel();
            if (rule.maxCount != Plan::none)
                emitRequireOnlyJSON(node, value, fail);
            emitStartMemoizing(rule.nodes.span());
            a.move(index, m_zero);
            a.move(found, m_zero);
            a.bind(head);
            a.loopHint();
            a.jumpIfCompares(Comparison::Less, index, length, false, exit);
            a.readElement(current, value, index);
            emitCheckOnly(rule.nodes[0], current, missed, true);
            a.increment(found);
            if (rule.maxCount == Plan::none)
                a.jumpIfCompares(Comparison::GreaterOrEqual, found, a.constant(jsNumber(rule.count)), true, enough);
            else
                a.jumpIfCompares(Comparison::Greater, found, a.constant(jsNumber(rule.maxCount)), true, fail);
            a.bind(missed);
            a.increment(index);
            a.jump(head);
            a.bind(exit);
            a.jumpIfCompares(Comparison::Less, found, a.constant(jsNumber(rule.count)), true, fail);
            a.bind(enough);
        }
    }

    void appendLiterals(uint32_t index, Vector<const Plan::Node*, 8>& literals, bool& areAllStrings) const
    {
        const Plan::Node& node = getNode(index);
        if (node.shape == Shape::Union && node.rules.isEmpty() && node.defaultKind == Plan::Literal::None && !m_analysis.isFunction[index]) {
            for (uint32_t arm : node.children)
                appendLiterals(arm, literals, areAllStrings);
            return;
        }
        areAllStrings &= node.shape == Shape::StringLiteral && node.rules.isEmpty() && node.defaultKind == Plan::Literal::None;
        literals.append(&node);
    }

    const UniquedStringImpl* findDiscriminator(const Plan::Node& node, Vector<std::pair<StringImpl*, unsigned>, 16>& tags) const
    {
        if (node.children.size() < 2)
            return nullptr;
        for (uint32_t arm : node.children) {
            if (getNode(arm).shape != Shape::Object || getNode(arm).defaultKind != Plan::Literal::None || m_analysis.isFunction[arm])
                return nullptr;
        }
        for (auto& candidate : getNode(node.children[0]).members) {
            if (parseIndex(*candidate.key.impl()))
                continue;
            tags.clear();
            bool holds = true;
            for (unsigned i = 0; i < node.children.size() && holds; ++i) {
                const Plan::Member* found = nullptr;
                for (auto& member : getNode(node.children[i]).members) {
                    if (member.key == candidate.key)
                        found = &member;
                }
                if (!found || found->isOptional) {
                    holds = false;
                    break;
                }
                Vector<const Plan::Node*, 8> literals;
                appendLiterals(found->node, literals, holds);
                for (auto* literal : literals) {
                    if (!holds)
                        break;
                    for (auto& tag : tags)
                        holds &= !equal(tag.first, literal->string.impl());
                    tags.append({ literal->string.impl(), i });
                }
            }
            if (holds)
                return candidate.key.impl();
        }
        return nullptr;
    }

    void emitUnion(const Plan::Node& node, Slot value, Target fail, bool isSoft)
    {
        RegisterScope scope(a);
        Target done = a.newLabel();
        Target wrongShape = fail;

        Vector<const Plan::Node*, 8> literals;
        bool areAllStrings = true;
        for (uint32_t arm : node.children)
            appendLiterals(arm, literals, areAllStrings);
        if (areAllStrings && literals.size() > 1) {
            Target missed = a.newLabel();
            a.beginStringSwitch(value);
            a.bind(missed);
            a.jump(wrongShape);
            a.bind(done);
            Vector<Writer::StringCase, 16> cases;
            for (auto* literal : literals)
                cases.append({ literal->string.impl(), done });
            a.endStringSwitch(cases.span(), missed);
            return;
        }

        Vector<std::pair<StringImpl*, unsigned>, 16> tags;
        if (auto* key = findDiscriminator(node, tags)) {
            bool readsValuesByKey = false;
            for (uint32_t arm : node.children)
                readsValuesByKey |= this->readsValuesByKey(getNode(arm));
            emitObjectGuard(readsValuesByKey, value, wrongShape);
            Slot tag = a.newRegister();
            a.readOwnProperty(tag, value, const_cast<UniquedStringImpl*>(key));
            Target missed = a.newLabel();
            Vector<Target, 8> arms;
            a.beginStringSwitch(tag);
            a.bind(missed);
            a.jump(fail);
            for (uint32_t arm : node.children) {
                arms.append(a.newLabel());
                a.bind(arms.last());
                emitObject(getNode(arm), value, fail, isSoft, key);
                emitRulesOfAnyShape(getNode(arm), value, fail, isSoft);
                a.jump(done);
            }
            Vector<Writer::StringCase, 16> cases;
            for (auto& one : tags)
                cases.append({ one.first, arms[one.second] });
            a.endStringSwitch(cases.span(), missed);
            a.bind(done);
            return;
        }

        for (unsigned i = 0; i < node.children.size(); ++i) {
            uint32_t arm = node.children[i];
            if (i + 1 == node.children.size()) {
                emitNodeOrCall(arm, value, fail, isSoft);
                break;
            }
            RegisterScope armScope(a);
            Target next = a.newLabel();
            if (isCopying() && m_analysis.callsSharedFunction[arm]) {
                uint32_t asked[] = { arm };
                emitStartMemoizing(asked);
                emitCheckOnly(arm, value, next, true);
                emitNodeOrCall(arm, value, m_bailout, isSoft);
                a.jump(done);
                a.bind(next);
                continue;
            }
            Slot attempt = value;
            if (m_analysis.appliesDefault[arm] || (isCopying() && m_analysis.describesContainers[arm])) {
                attempt = a.newRegister();
                a.move(attempt, value);
            }
            Slot visitsBefore = nullptr;
            if (m_analysis.callsSharedFunction[arm]) {
                visitsBefore = a.newRegister();
                readVisits(visitsBefore);
            }
            emitNodeOrCall(arm, attempt, next, true);
            if (attempt != value)
                a.move(value, attempt);
            a.jump(done);
            a.bind(next);
            if (visitsBefore)
                emitMemoizeAfterDeepFailure(visitsBefore);
        }
        if (node.children.isEmpty())
            a.jump(wrongShape);
        a.bind(done);
    }

    Writer& a;
    Shared& m_shared;
    const Plan& m_plan;
    const Analysis& m_analysis;
    VM& m_vm;
    const Functions m_functions;
    Mode m_mode;
    Slot m_undefined;
    Slot m_zero;
    Slot m_bailoutMarker;
    Slot m_failureMarker;
    Slot m_state { nullptr };
    Slot m_visits { nullptr };
    Slot m_holes { nullptr };
    Target m_bailout { 0 };
    Target m_tooManyObjects { 0 };
    Vector<Function<void()>> m_deferred;
    JSValue m_copyExoticObject;
    JSValue m_hasPlainElements;
    JSValue m_getPlainArrayLength;
    JSValue m_copyExoticArray;
    JSValue m_countHoles;
    JSValue m_clonePlainObject;
    JSValue m_clonePlainArray;
    JSValue m_hasOnlyDeclaredKeys;
    JSValue m_hasOwnEnumerableKey;
    JSValue m_getOwnValue;
    JSValue m_matchesPattern;
    JSValue m_matchesFormat;
    JSValue m_isValidURL;
    JSValue m_matchesTemplate;
    JSValue m_countCodePoints;
    JSValue m_objectKeys;
    JSValue m_parseJSONText;
    JSValue m_startMemoizing;
    JSValue m_findVerdict;
    JSValue m_storeVerdict;
    JSValue m_throwTooManyObjects;
    JSValue m_hasUniqueItems;
    JSValue m_isDecimalMultiple;
    JSValue m_containsOnlyJSON;
};

class SharedNodesBody final : public GeneratedFunctionBody {
public:
    SharedNodesBody(Ref<Shared>&& shared, Mode mode)
        : m_shared(WTF::move(shared))
        , m_mode(mode)
    {
    }

    void emit(Writer& assembler) final { Emitter(assembler, m_shared.get(), functions, m_mode).emitSharedNodesFunction(); }

    Functions functions;

private:
    const Ref<Shared> m_shared;
    const Mode m_mode;
};

class ValidatorBody final : public GeneratedFunctionBody {
public:
    explicit ValidatorBody(Ref<Shared>&& shared, JSObject* onRefused = nullptr, JSObject* onBailout = nullptr, JSObject* onOtherArguments = nullptr)
        : m_shared(WTF::move(shared))
        , m_onRefused(onRefused)
        , m_onBailout(onBailout)
        , m_onOtherArguments(onOtherArguments)
    {
    }

    void emit(Writer& assembler) final
    {
        VM& vm = assembler.vm();
        JSGlobalObject* globalObject = m_shared->globalObject;
        Mode mode = m_shared->mode();
        Functions functions;
        if (m_shared->hasState()) {
            functions.state = constructEmptyArray(globalObject, nullptr);
            for (unsigned i = 0; i < StateLength; ++i)
                functions.state->putDirectIndex(globalObject, i, i == StateMemo || i == StateScannedObjects ? jsUndefined() : jsNumber(0));
            auto checkFunction = adoptRef(*new SharedNodesBody(m_shared.copyRef(), Mode::Check));
            functions.checkFunction = createGeneratedFunction(vm, globalObject, "check"_s, 2, checkFunction.copyRef());
            RefPtr<SharedNodesBody> copyFunction;
            if (mode == Mode::Copy) {
                copyFunction = adoptRef(*new SharedNodesBody(m_shared.copyRef(), Mode::Copy));
                functions.copyFunction = createGeneratedFunction(vm, globalObject, "validate"_s, 2, *copyFunction);
            }
            JSArray* roots = constructEmptyArray(globalObject, nullptr);
            roots->putDirectIndex(globalObject, 0, functions.state);
            roots->putDirectIndex(globalObject, 1, functions.checkFunction);
            if (functions.copyFunction)
                roots->putDirectIndex(globalObject, 2, functions.copyFunction);
            functions.checkFunction->putDirect(vm, toPropertyName(rootsName), roots);
            checkFunction->functions = functions;
            if (copyFunction) {
                functions.copyFunction->putDirect(vm, toPropertyName(rootsName), roots);
                copyFunction->functions = functions;
            }
        }
        Emitter emitter(assembler, m_shared.get(), functions, mode);
        if (functions.checkFunction)
            assembler.constant(functions.checkFunction);
        switch (m_shared->kind) {
        case ValidatorKind::Check:
            emitter.emitCheck();
            break;
        case ValidatorKind::Validate:
            emitter.emitValidate(m_onRefused, m_onBailout);
            break;
        case ValidatorKind::Parse:
            emitter.emitParse(m_onRefused, m_onBailout, m_onOtherArguments);
            break;
        }
    }

private:
    const Ref<Shared> m_shared;
    JSObject* const m_onRefused;
    JSObject* const m_onBailout;
    JSObject* const m_onOtherArguments;
};

class PlanCacheKey {
public:
    static String of(const Plan& plan)
    {
        PlanCacheKey text;
        text.number(plan.root);
        text.number(plan.treatsUndefinedAsMissing);
        text.number(plan.nodes.size());
        for (auto& node : plan.nodes) {
            text.number(static_cast<uint32_t>(node.shape) | static_cast<uint32_t>(node.undeclared) << 8 | static_cast<uint32_t>(node.boolean) << 16 | static_cast<uint32_t>(node.defaultKind) << 17 | static_cast<uint32_t>(node.defaultBoolean) << 25);
            text.number(std::bit_cast<uint64_t>(node.number));
            text.number(std::bit_cast<uint64_t>(node.defaultNumber));
            text.string(node.string);
            text.string(node.defaultString);
            text.number(node.child);
            text.number(node.required);
            text.list(node.children);
            text.number(node.members.size());
            for (auto& member : node.members) {
                text.string(member.key.string());
                text.number(member.node);
                text.number(member.isOptional | member.mustBeOwn << 1);
            }
            text.number(node.rules.size());
            for (auto& rule : node.rules) {
                text.number(static_cast<uint32_t>(rule.kind) | static_cast<uint32_t>(rule.flags) << 8);
                text.number(rule.count);
                text.number(rule.maxCount);
                text.number(std::bit_cast<uint64_t>(rule.number));
                text.string(rule.text);
                text.number(rule.texts.size());
                for (auto& one : rule.texts)
                    text.string(one);
                text.list(rule.nodes);
            }
        }
        return String(text.m_bytes.span());
    }

private:
    void number(uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i)
            m_bytes.append(static_cast<Latin1Character>(value >> (8 * i)));
    }

    void list(const Vector<uint32_t>& values)
    {
        number(values.size());
        for (uint32_t value : values)
            number(value);
    }

    void string(const String& value)
    {
        number(value.isNull() ? UINT64_MAX : value.length());
        for (unsigned i = 0; i < value.length(); ++i) {
            m_bytes.append(static_cast<Latin1Character>(value[i]));
            m_bytes.append(static_cast<Latin1Character>(value[i] >> 8));
        }
    }

    Vector<Latin1Character, 1024> m_bytes;
};

AOT_THRESHOLD(unsigned, maxCachedPlans, 256);

static JSMap* getCodeCache(VM& vm, JSGlobalObject* globalObject)
{
    if (JSValue found = globalObject->getDirect(vm, toPropertyName(codeCacheName)))
        return uncheckedDowncast<JSMap>(found.asCell());
    JSMap* map = JSMap::create(vm, globalObject->mapStructure());
    globalObject->putDirect(vm, toPropertyName(codeCacheName), map, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    return map;
}

static RefPtr<Shared> analyze(Ref<Plan>&& plan, JSGlobalObject* globalObject, ValidatorKind kind)
{
#if ENABLE(AOT)
    if (globalObject->aotInstance())
        return nullptr;
#endif
    auto shared = adoptRef(*new Shared(WTF::move(plan), globalObject, kind));
    Analyzer(shared->plan.get(), shared->analysis.get()).run();
    if (!shared->analysis->isSupported)
        return nullptr;
    return shared;
}

static JSFunction* createWithHandlers(VM& vm, JSGlobalObject* globalObject, Ref<Plan>&& plan, ValidatorKind kind, ASCIILiteral name, unsigned parameterCount, JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments)
{
    auto shared = analyze(WTF::move(plan), globalObject, kind);
    if (!shared)
        return nullptr;
    JSFunction* function = createGeneratedFunction(vm, globalObject, name, parameterCount, adoptRef(*new ValidatorBody(shared.releaseNonNull(), onRefused, onBailout, onOtherArguments)));
    JSArray* roots = constructEmptyArray(globalObject, nullptr);
    roots->putDirectIndex(globalObject, 0, onRefused);
    roots->putDirectIndex(globalObject, 1, onBailout);
    if (onOtherArguments)
        roots->putDirectIndex(globalObject, 2, onOtherArguments);
    function->putDirect(vm, toPropertyName(rootsName), roots);
    return function;
}

class PlanParser {
public:
    explicit PlanParser(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_plan(Plan::create())
    {
    }

    RefPtr<Plan> read(const String& text)
    {
        JSValue parsed = JSONParse(m_globalObject, text);
        if (!parsed || !isJSArray(parsed))
            return nullptr;
        JSArray* nodes = asArray(parsed);
        unsigned count = nodes->length();
        if (!count || count > maxPlanNodes)
            return nullptr;
        m_count = count;
        m_plan->nodes.grow(count);
        for (unsigned i = 0; i < count && m_isValid; ++i)
            parseNode(m_plan->nodes[i], getElement(nodes, i));
        for (auto& [node, discriminant] : m_discriminants) {
            if (m_isValid)
                parseDiscriminant(m_plan->nodes[node], discriminant);
        }
        return m_isValid ? RefPtr<Plan>(m_plan.ptr()) : nullptr;
    }

private:
    AOT_CLASS_THRESHOLD(unsigned, maxPlanNodes, 1 << 16);

    JSValue fail()
    {
        m_isValid = false;
        return { };
    }

    JSValue getElement(JSArray* array, unsigned index) { return array->getDirectIndex(m_globalObject, index); }

    JSValue find(JSObject* object, ASCIILiteral name) { return object->getDirect(m_vm, Identifier::fromString(m_vm, name)); }

    void requireOnlyKeys(JSObject* object, std::span<const ASCIILiteral> allowed)
    {
        PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        object->methodTable()->getOwnPropertyNames(object, m_globalObject, names, DontEnumPropertiesMode::Include);
        for (auto& name : names) {
            bool isAllowed = false;
            for (ASCIILiteral one : allowed)
                isAllowed |= name.string() == one;
            if (!isAllowed)
                fail();
        }
    }

    JSObject* toObject(JSValue value)
    {
        if (value && value.isObject() && !isJSArray(value))
            return asObject(value);
        fail();
        return nullptr;
    }

    String toString(JSValue value)
    {
        if (value && value.isString())
            return asString(value)->value(m_globalObject);
        fail();
        return String();
    }

    double toNumber(JSValue value)
    {
        if (value && value.isNumber())
            return value.asNumber();
        fail();
        return 0;
    }

    uint32_t toCount(JSValue value)
    {
        double number = toNumber(value);
        if (number >= 0 && number < Plan::none && number == std::trunc(number))
            return static_cast<uint32_t>(number);
        fail();
        return 0;
    }

    uint32_t toNodeIndex(JSValue value)
    {
        uint32_t node = toCount(value);
        if (node >= m_count)
            fail();
        return node;
    }

    bool toBoolean(JSValue value)
    {
        if (value && value.isBoolean())
            return value.asBoolean();
        fail();
        return false;
    }

    void forEachElement(JSValue list, NOESCAPE const Invocable<void(JSValue)> auto& visit)
    {
        if (!list || !isJSArray(list)) {
            fail();
            return;
        }
        JSArray* array = asArray(list);
        for (unsigned i = 0; i < array->length() && m_isValid; ++i)
            visit(getElement(array, i));
    }

    Vector<uint32_t> toNodeIndexes(JSValue list)
    {
        Vector<uint32_t> nodes;
        forEachElement(list, [&](JSValue one) {
            nodes.append(toNodeIndex(one));
        });
        return nodes;
    }

    Vector<String> toStrings(JSValue list)
    {
        Vector<String> strings;
        forEachElement(list, [&](JSValue one) {
            strings.append(toString(one));
        });
        return strings;
    }

    void parseDefault(Plan::Node& node, JSValue value)
    {
        if (value.isNull())
            node.defaultKind = Plan::Literal::Null;
        else if (value.isBoolean()) {
            node.defaultKind = Plan::Literal::Boolean;
            node.defaultBoolean = value.asBoolean();
        } else if (value.isNumber()) {
            node.defaultKind = Plan::Literal::Number;
            node.defaultNumber = value.asNumber();
        } else {
            node.defaultKind = Plan::Literal::String;
            node.defaultString = toString(value);
        }
    }

    void parseRule(Plan::Node& node, JSObject* object)
    {
        String name = toString(find(object, "rule"_s));
        Plan::Rule rule;
        auto is = [&](ASCIILiteral word, RuleKind kind) {
            if (name != word)
                return false;
            rule.kind = kind;
            return true;
        };
        Vector<ASCIILiteral, 2> allowed;
        if (is("Integer"_s, RuleKind::Integer)) {
        } else if (is("Minimum"_s, RuleKind::Minimum) || is("Maximum"_s, RuleKind::Maximum) || is("ExclusiveMinimum"_s, RuleKind::ExclusiveMinimum) || is("ExclusiveMaximum"_s, RuleKind::ExclusiveMaximum)) {
            rule.number = toNumber(find(object, "bound"_s));
            allowed = { "bound"_s };
        } else if (is("MultipleOf"_s, RuleKind::MultipleOf)) {
            rule.number = toNumber(find(object, "divisor"_s));
            allowed = { "divisor"_s };
        } else if (is("MinimumLength"_s, RuleKind::MinimumLength) || is("MaximumLength"_s, RuleKind::MaximumLength)) {
            rule.count = toCount(find(object, "length"_s));
            allowed = { "length"_s };
        } else if (is("Pattern"_s, RuleKind::Pattern)) {
            rule.text = toString(find(object, "source"_s));
            allowed = { "source"_s };
        } else if (is("Format"_s, RuleKind::Pattern)) {
            rule.text = toString(find(object, "source"_s));
            String format = toString(find(object, "format"_s));
            allowed = { "source"_s, "format"_s };
            if (rule.text.isEmpty()) {
                rule.kind = RuleKind::Url;
                if (format != "url"_s)
                    fail();
            }
        } else if (is("Template"_s, RuleKind::Template)) {
            rule.texts = toStrings(find(object, "texts"_s));
            rule.text = toString(find(object, "placeholders"_s));
            allowed = { "texts"_s, "placeholders"_s };
        } else {
            fail();
            return;
        }
        allowed.append("rule"_s);
        requireOnlyKeys(object, allowed.span());
        if (rule.kind == RuleKind::Pattern && m_isValid && !RegExp::create(m_vm, rule.text, OptionSet<Yarr::Flags>::fromRaw(rule.flags))->isValid())
            fail();
        node.rules.append(WTF::move(rule));
    }

    bool isLiteral(const Plan::Node& node, JSValue tag)
    {
        if (!node.rules.isEmpty() || node.defaultKind != Plan::Literal::None)
            return false;
        switch (node.shape) {
        case Shape::Null:
            return tag.isNull();
        case Shape::BooleanLiteral:
            return tag.isBoolean() && tag.asBoolean() == node.boolean;
        case Shape::NumberLiteral:
            return tag.isNumber() && tag.asNumber() == node.number;
        case Shape::StringLiteral:
            return tag.isString() && asString(tag)->value(m_globalObject) == node.string;
        case Shape::Unknown:
        case Shape::Never:
        case Shape::Undefined:
        case Shape::AnyObject:
        case Shape::String:
        case Shape::Number:
        case Shape::Boolean:
        case Shape::BigInt:
        case Shape::Array:
        case Shape::Tuple:
        case Shape::Dictionary:
        case Shape::Object:
        case Shape::Union:
            return false;
        }
        return false;
    }

    // Puts the tagged arms first. That keeps the order of attempts only if each tagged arm requires its own tag.
    void parseDiscriminant(Plan::Node& node, JSValue value)
    {
        JSObject* object = toObject(value);
        if (!object)
            return;
        static constexpr ASCIILiteral words[] = { "key"_s, "tags"_s, "tagged"_s, "untagged"_s, "expected"_s };
        requireOnlyKeys(object, words);
        AtomString key(toString(find(object, "key"_s)));
        Vector<uint32_t> tagged = toNodeIndexes(find(object, "tagged"_s));
        Vector<uint32_t> untagged = toNodeIndexes(find(object, "untagged"_s));
        Vector<JSValue> tags;
        forEachElement(find(object, "tags"_s), [&](JSValue tag) {
            tags.append(tag);
        });
        if (!m_isValid || tags.size() != tagged.size())
            return void(fail());
        for (unsigned i = 0; i < tagged.size(); ++i) {
            const Plan::Node& arm = m_plan->nodes[tagged[i]];
            bool requiresTag = false;
            for (auto& member : arm.members)
                requiresTag |= arm.shape == Shape::Object && member.key == key && !member.isOptional && isLiteral(m_plan->nodes[member.node], tags[i]);
            if (!requiresTag)
                return void(fail());
        }
        Vector<uint32_t> ordered = tagged;
        ordered.appendVector(untagged);
        Vector<uint32_t> sorted = ordered;
        Vector<uint32_t> arms = node.children;
        std::sort(sorted.begin(), sorted.end());
        std::sort(arms.begin(), arms.end());
        if (sorted != arms)
            return void(fail());
        node.children = WTF::move(ordered);
    }

    void parseNode(Plan::Node& node, JSValue value)
    {
        JSObject* object = toObject(value);
        if (!object)
            return;
        String shape = toString(find(object, "shape"_s));
        if (shape == "Unknown"_s)
            node.shape = Shape::Unknown;
        else if (shape == "Never"_s)
            node.shape = Shape::Never;
        else if (shape == "Undefined"_s)
            node.shape = Shape::Undefined;
        else if (shape == "Null"_s)
            node.shape = Shape::Null;
        else if (shape == "AnyObject"_s)
            node.shape = Shape::AnyObject;
        else if (shape == "Primitive"_s) {
            String primitive = toString(find(object, "primitive"_s));
            if (primitive == "string"_s)
                node.shape = Shape::String;
            else if (primitive == "number"_s)
                node.shape = Shape::Number;
            else if (primitive == "boolean"_s)
                node.shape = Shape::Boolean;
            else if (primitive == "bigint"_s)
                node.shape = Shape::BigInt;
            else
                fail();
        } else if (shape == "BooleanLiteral"_s) {
            node.shape = Shape::BooleanLiteral;
            node.boolean = toBoolean(find(object, "value"_s));
        } else if (shape == "NumberLiteral"_s) {
            node.shape = Shape::NumberLiteral;
            node.number = toNumber(find(object, "value"_s));
        } else if (shape == "StringLiteral"_s) {
            node.shape = Shape::StringLiteral;
            node.string = toString(find(object, "value"_s));
        } else if (shape == "Array"_s || shape == "Dictionary"_s) {
            node.shape = shape == "Array"_s ? Shape::Array : Shape::Dictionary;
            node.child = toNodeIndex(find(object, "element"_s));
        } else if (shape == "Tuple"_s) {
            node.shape = Shape::Tuple;
            if (JSValue elements = find(object, "elements"_s))
                node.children = toNodeIndexes(elements);
            node.required = node.children.size();
        } else if (shape == "Object"_s) {
            node.shape = Shape::Object;
            if (JSValue members = find(object, "members"_s)) {
                forEachElement(members, [&](JSValue one) {
                    JSObject* member = toObject(one);
                    if (!member)
                        return;
                    String key = toString(find(member, "key"_s));
                    if (!m_isValid)
                        return;
                    static constexpr ASCIILiteral memberKeys[] = { "key"_s, "node"_s, "isOptional"_s, "mustBeOwn"_s };
                    requireOnlyKeys(member, memberKeys);
                    AtomString name(key);
                    for (auto& other : node.members) {
                        if (other.key == name)
                            fail();
                    }
                    node.members.append(Plan::Member { name, toNodeIndex(find(member, "node"_s)), toBoolean(find(member, "isOptional"_s)), toBoolean(find(member, "mustBeOwn"_s)) });
                });
            }
            String undeclared = toString(find(object, "undeclared"_s));
            if (undeclared == "Allowed"_s)
                node.undeclared = Plan::Undeclared::Allowed;
            else if (undeclared == "Rejected"_s)
                node.undeclared = Plan::Undeclared::Rejected;
            else if (undeclared == "Validated"_s) {
                node.undeclared = Plan::Undeclared::Validated;
                node.child = toNodeIndex(find(object, "undeclaredValue"_s));
            } else
                fail();
        } else if (shape == "Union"_s) {
            node.shape = Shape::Union;
            if (JSValue arms = find(object, "arms"_s))
                node.children = toNodeIndexes(arms);
            if (JSValue discriminant = find(object, "discriminant"_s))
                m_discriminants.append({ static_cast<uint32_t>(&node - m_plan->nodes.span().data()), discriminant });
        } else
            fail();

        static constexpr ASCIILiteral nodeKeys[] = { "shape"_s, "primitive"_s, "value"_s, "element"_s, "elements"_s, "arms"_s, "members"_s, "undeclared"_s, "undeclaredValue"_s, "discriminant"_s, "remembers"_s, "rules"_s, "message"_s, "default"_s, "text"_s };
        requireOnlyKeys(object, nodeKeys);
        if (JSValue rules = find(object, "rules"_s)) {
            forEachElement(rules, [&](JSValue one) {
                if (JSObject* rule = toObject(one))
                    parseRule(node, rule);
            });
        }
        if (JSValue defaultValue = find(object, "default"_s))
            parseDefault(node, defaultValue);
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Ref<Plan> m_plan;
    Vector<std::pair<uint32_t, JSValue>> m_discriminants;
    unsigned m_count { 0 };
    bool m_isValid { true };
};

} // namespace

} // namespace ValidatorBytecode

namespace {

struct Decimal {
    uint64_t digits { 0 };
    int exponent { 0 };
};

Decimal toDecimal(double positive)
{
    using Converter = WTF::double_conversion::DoubleToStringConverter;
    std::array<char, Converter::kBase10MaximalLength + 1> buffer;
    bool isNegative = false;
    int length = 0;
    int point = 0;
    Converter::DoubleToAscii(positive, Converter::SHORTEST, 0, std::span<char> { buffer }, isNegative, length, point);
    Decimal result;
    for (int i = 0; i < length; ++i)
        result.digits = result.digits * 10 + (buffer[i] - '0');
    result.exponent = point - length;
    return result;
}

} // namespace

bool isDecimalMultipleOf(double number, double divisor)
{
    using Wide = UInt128;
    if (!std::isfinite(number) || !std::isfinite(divisor) || !(divisor > 0))
        return false;
    if (!number)
        return true;
    Decimal dividend = toDecimal(std::abs(number));
    Decimal unit = toDecimal(divisor);
    int shift = dividend.exponent - unit.exponent;
    if (shift >= 0) {
        Wide remainder = dividend.digits % unit.digits;
        Wide power = 10 % unit.digits;
        for (unsigned left = shift; left; left >>= 1) {
            if (left & 1)
                remainder = remainder * power % unit.digits;
            power = power * power % unit.digits;
        }
        return !remainder;
    }
    AOT_THRESHOLD(int, maxDecimalDigits, 17);
    if (-shift > maxDecimalDigits)
        return false;
    Wide whole = unit.digits;
    for (int i = 0; i < -shift; ++i)
        whole *= 10;
    return !(dividend.digits % whole);
}

JSONScanResult scanForNonJSON(JSGlobalObject* globalObject, JSValue value)
{
    return ValidatorBytecode::JSONScanner(globalObject, nullptr).look(value);
}

void setValidationFormatChecker(ValidationFormatChecker checker) { ValidatorBytecode::formatChecker = checker; }

RefPtr<ValidationPlan> parseValidationPlan(JSGlobalObject* globalObject, const String& text)
{
    return ValidatorBytecode::PlanParser(globalObject).read(text);
}

JSFunction* createCheckFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan)
{
    using namespace ValidatorBytecode;
#if ENABLE(AOT)
    if (globalObject->aotInstance())
        return nullptr;
#endif
    JSMap* codeCache = getCodeCache(vm, globalObject);
    JSString* text = jsString(vm, PlanCacheKey::of(plan.get()));
    JSValue code = codeCache->get(globalObject, text);
    if (code.isCell())
        return JSFunction::create(vm, globalObject, uncheckedDowncast<FunctionExecutable>(code.asCell()), globalObject);

    auto shared = analyze(WTF::move(plan), globalObject, ValidatorKind::Check);
    if (!shared)
        return nullptr;
    JSFunction* function = createGeneratedFunction(vm, globalObject, "check"_s, 1, adoptRef(*new ValidatorBody(shared.releaseNonNull())));
    compileGeneratedFunction(vm, function);
    if (codeCache->size() >= maxCachedPlans)
        codeCache->clear(globalObject);
    codeCache->set(globalObject, text, function->jsExecutable());
    return function;
}

JSFunction* createValidateFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan, JSObject* onRefused, JSObject* onBailout)
{
    return ValidatorBytecode::createWithHandlers(vm, globalObject, WTF::move(plan), ValidatorKind::Validate, "validate"_s, 1, onRefused, onBailout, nullptr);
}

JSFunction* createParseFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan, JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments)
{
    return ValidatorBytecode::createWithHandlers(vm, globalObject, WTF::move(plan), ValidatorKind::Parse, "parse"_s, 2, onRefused, onBailout, onOtherArguments);
}

JSFunction* createValidatorFromPlanText(JSGlobalObject* globalObject, ValidatorKind kind, const String& planText, JSObject* onRefused, JSObject* onBailout, JSObject* onOtherArguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RefPtr<ValidationPlan> plan = parseValidationPlan(globalObject, planText);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!plan) {
        throwTypeError(globalObject, scope, "Invalid validation plan"_s);
        return nullptr;
    }
    switch (kind) {
    case ValidatorKind::Check:
        RELEASE_AND_RETURN(scope, createCheckFunction(vm, globalObject, plan.releaseNonNull()));
    case ValidatorKind::Validate:
        RELEASE_AND_RETURN(scope, createValidateFunction(vm, globalObject, plan.releaseNonNull(), onRefused, onBailout));
    case ValidatorKind::Parse:
        RELEASE_AND_RETURN(scope, createParseFunction(vm, globalObject, plan.releaseNonNull(), onRefused, onBailout, onOtherArguments));
    }
    return nullptr;
}

} // namespace JSC
