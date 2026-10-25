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

static JSC_DECLARE_HOST_FUNCTION(validationPlainObjectFor);
static JSC_DECLARE_HOST_FUNCTION(validationHasPlainElements);
static JSC_DECLARE_HOST_FUNCTION(validationPlainArrayLength);
static JSC_DECLARE_HOST_FUNCTION(validationPlainArrayFor);
static JSC_DECLARE_HOST_FUNCTION(validationHolesOf);
static JSC_DECLARE_HOST_FUNCTION(validationCopyPlainObject);
static JSC_DECLARE_HOST_FUNCTION(validationCopyPlainArray);
static JSC_DECLARE_HOST_FUNCTION(validationHasOnlyTheseKeys);
static JSC_DECLARE_HOST_FUNCTION(validationHasOwnKey);
static JSC_DECLARE_HOST_FUNCTION(validationOwnValue);
static JSC_DECLARE_HOST_FUNCTION(validationMatchesPattern);
static JSC_DECLARE_HOST_FUNCTION(validationMatchesFormat);
static JSC_DECLARE_HOST_FUNCTION(validationIsUrl);
static JSC_DECLARE_HOST_FUNCTION(validationMatchesTemplate);
static JSC_DECLARE_HOST_FUNCTION(validationCodePointLength);
static JSC_DECLARE_HOST_FUNCTION(validationParseText);
static JSC_DECLARE_HOST_FUNCTION(validationStartRemembering);
static JSC_DECLARE_HOST_FUNCTION(validationRecall);
static JSC_DECLARE_HOST_FUNCTION(validationRemember);
static JSC_DECLARE_HOST_FUNCTION(validationThrowTooManyVisits);
static JSC_DECLARE_HOST_FUNCTION(validationHasUniqueItems);
static JSC_DECLARE_HOST_FUNCTION(validationIsMultipleInDecimals);
static JSC_DECLARE_HOST_FUNCTION(validationSaysOnlyJSON);

static ValidationFormatChecker formatChecker;

// One call looks at so many objects and arrays, and takes so many holes for undefined.
AOT_THRESHOLD(int32_t, mostVisits, 1 << 24);
AOT_THRESHOLD(int32_t, mostHoles, 65536);

// What the functions of one validator share while a call runs. It is set anew when a call begins.
enum StateIndex : unsigned { VisitsInState, MemoryInState, HolesInState, LookedThroughInState, LengthOfState };

static SymbolImpl::StaticSymbolImpl endOfCheckName { "validationEndsHere", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl failureName { "validationFailed", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl rememberedCodeName { "validatorsByPlan", SymbolImpl::s_flagIsPrivate };
static SymbolImpl::StaticSymbolImpl keptAliveName { "validatorKeepsAlive", SymbolImpl::s_flagIsPrivate };

static PropertyName nameOf(SymbolImpl::StaticSymbolImpl& name) { return PropertyName(&static_cast<SymbolImpl&>(name)); }

// Programs never see either. The first says that only running code of the value could answer, so that the whole check ends; the second that a node has refused its value.
static Symbol* symbolOf(VM& vm, SymbolImpl::StaticSymbolImpl& name) { return Symbol::create(vm, static_cast<SymbolImpl&>(name)); }

static bool isAnyArray(JSValue value) { return value.isCell() && (value.asCell()->type() == ArrayType || value.asCell()->type() == DerivedArrayType); }

// (value, mark): for an object that is not plain, a plain object that holds what { ...value } would read under a string. Undefined for what is no object in the sense of JSON; the mark where reading would run code.
JSC_DEFINE_HOST_FUNCTION(validationPlainObjectFor, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue mark = callFrame->uncheckedArgument(1);
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
            return JSValue::encode(mark);
        JSValue held = slot.getValue(globalObject, name);
        RETURN_IF_EXCEPTION(scope, {});
        if (auto index = parseIndex(name))
            copy->putDirectIndex(globalObject, *index, held);
        else
            copy->putDirect(vm, name, held);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(copy);
}

// Of a final object: whether reading it by any of its own keys runs nothing.
JSC_DEFINE_HOST_FUNCTION(validationHasPlainElements, (JSGlobalObject*, CallFrame* callFrame))
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

static unsigned holesOf(JSArray* array)
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

JSC_DEFINE_HOST_FUNCTION(validationPlainArrayLength, (JSGlobalObject*, CallFrame* callFrame))
{
    JSValue value = callFrame->uncheckedArgument(0);
    if (!isJSArray(value) || !isPlainArray(asArray(value)))
        return JSValue::encode(jsNumber(-1));
    return JSValue::encode(jsNumber(asArray(value)->length()));
}

// (value, mark): for an array that is not plain, one that has every element of its own: what the value has, and undefined for a hole. Undefined for what is no array; the mark where an element is no value, and where more is missing than any call takes.
JSC_DEFINE_HOST_FUNCTION(validationPlainArrayFor, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue mark = callFrame->uncheckedArgument(1);
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
        if (isPresent ? !slot.isValue() : ++holes > static_cast<unsigned>(mostHoles))
            return JSValue::encode(mark);
        JSValue held = isPresent ? slot.getValue(globalObject, Identifier::from(vm, i)) : jsUndefined();
        RETURN_IF_EXCEPTION(scope, {});
        copy->putDirectIndex(globalObject, i, held);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(copy);
}

// How many holes an array has. One more than any call takes stands for all greater numbers.
JSC_DEFINE_HOST_FUNCTION(validationHolesOf, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    if (!isAnyArray(value))
        return JSValue::encode(jsNumber(0));
    JSArray* array = asArray(value);
    if (!hasAnyArrayStorage(array->indexingType()) && array->type() == ArrayType)
        return JSValue::encode(jsNumber(holesOf(array)));
    unsigned length = array->length();
    unsigned holes = 0;
    for (unsigned i = 0; i < length && holes <= static_cast<unsigned>(mostHoles); ++i) {
        PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
        holes += !array->methodTable()->getOwnPropertySlotByIndex(array, globalObject, i, slot);
        RETURN_IF_EXCEPTION(scope, {});
    }
    return JSValue::encode(jsNumber(holes));
}

// Of a final object: what { ...value } makes, where making it runs nothing and every key is a string. Undefined elsewhere.
JSC_DEFINE_HOST_FUNCTION(validationCopyPlainObject, (JSGlobalObject * globalObject, CallFrame* callFrame))
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
    // What becomes of a key that is a symbol depends on the node.
    if (hasSymbol)
        return JSValue::encode(jsUndefined());
    for (unsigned i = 0; i < names.size(); ++i)
        copy->putDirect(vm, names[i], values.at(i));
    ensureStillAliveHere(structure);
    return JSValue::encode(copy);
}

// (array, fewest, most, mark): a copy, where the array is plain and has no hole. Undefined where its length is not within the two; the mark elsewhere.
JSC_DEFINE_HOST_FUNCTION(validationCopyPlainArray, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* array = asArray(callFrame->uncheckedArgument(0));
    JSValue mark = callFrame->uncheckedArgument(3);
    if (!isPlainArray(array))
        return JSValue::encode(mark);
    double length = array->length();
    if (length < callFrame->uncheckedArgument(1).asNumber() || length > callFrame->uncheckedArgument(2).asNumber())
        return JSValue::encode(jsUndefined());
    if (holesOf(array))
        return JSValue::encode(mark);
    JSArray* copy = tryCloneArrayFromFast<ArrayFillMode::Empty>(globalObject, array);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(copy && isPlainArray(copy) ? JSValue(copy) : mark);
}

JSC_DEFINE_HOST_FUNCTION(validationHasOnlyTheseKeys, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

static bool findOwnWithoutRunningCode(JSGlobalObject* globalObject, JSObject* object, JSString* key, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = key->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot));
}

// (plain object, key): whether the key is its own and is enumerated.
JSC_DEFINE_HOST_FUNCTION(validationHasOwnKey, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(callFrame->uncheckedArgument(0));
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isPresent = findOwnWithoutRunningCode(globalObject, object, asString(callFrame->uncheckedArgument(1)), slot);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsBoolean(isPresent && !(slot.attributes() & PropertyAttribute::DontEnum)));
}

// (plain object, key, mark): what it holds of its own under a key that is an index.
JSC_DEFINE_HOST_FUNCTION(validationOwnValue, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = asObject(callFrame->uncheckedArgument(0));
    JSString* key = asString(callFrame->uncheckedArgument(1));
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isPresent = findOwnWithoutRunningCode(globalObject, object, key, slot);
    RETURN_IF_EXCEPTION(scope, {});
    if (!isPresent || (slot.attributes() & PropertyAttribute::DontEnum))
        return JSValue::encode(jsUndefined());
    if (!slot.isValue())
        return JSValue::encode(callFrame->uncheckedArgument(2));
    auto name = key->toIdentifier(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    RELEASE_AND_RETURN(scope, JSValue::encode(slot.getValue(globalObject, name)));
}

// Nothing where the search was given up: that is no answer, and "no match" under a negation would be an accept. An engine that cannot tell says "no match".
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

JSC_DEFINE_HOST_FUNCTION(validationMatchesPattern, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

JSC_DEFINE_HOST_FUNCTION(validationMatchesFormat, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = asString(callFrame->uncheckedArgument(1))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    return JSValue::encode(jsBoolean(formatChecker && formatChecker(vm, callFrame->uncheckedArgument(0).asInt32(), view)));
}

JSC_DEFINE_HOST_FUNCTION(validationIsUrl, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

// (text, the texts around the placeholders, a letter for each placeholder)
JSC_DEFINE_HOST_FUNCTION(validationMatchesTemplate, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto held = asString(callFrame->uncheckedArgument(0))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    StringView value = held;
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

JSC_DEFINE_HOST_FUNCTION(validationCodePointLength, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

JSC_DEFINE_HOST_FUNCTION(validationParseText, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = asString(callFrame->uncheckedArgument(0))->view(globalObject);
    RETURN_IF_EXCEPTION(scope, {});
    RELEASE_AND_RETURN(scope, JSValue::encode(JSONParseWithException(globalObject, view)));
}

// From here to the end of the call, what a node that reaches itself has said of an object is not worked out again.
JSC_DEFINE_HOST_FUNCTION(validationStartRemembering, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    JSArray* state = asArray(callFrame->uncheckedArgument(0));
    if (state->getIndexQuickly(MemoryInState).isUndefined())
        state->setIndexQuickly(vm, MemoryInState, JSMap::create(vm, globalObject->mapStructure()));
    return JSValue::encode(jsUndefined());
}

// (memory, node, value): what was said, or undefined.
JSC_DEFINE_HOST_FUNCTION(validationRecall, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

// (memory, node, value, verdict)
JSC_DEFINE_HOST_FUNCTION(validationRemember, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

JSC_DEFINE_HOST_FUNCTION(validationThrowTooManyVisits, (JSGlobalObject * globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMRangeError(globalObject, scope, "The value holds more than 16777216 objects and arrays"_s);
}

// Two values are equal exactly if they are written alike here. A key is written as the address of its one and only string, which is kept for as long as the writing.
class CanonicalWriter {
public:
    explicit CanonicalWriter(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    enum class End : uint8_t { Written, NeedsCode, Threw };

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
                if (isPresent ? !slot.isValue() : ++holes > static_cast<unsigned>(mostHoles))
                    return End::NeedsCode;
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
                return End::NeedsCode;
            JSValue held = slot.getValue(m_globalObject, key);
            RETURN_IF_EXCEPTION(scope, End::Threw);
            if (held.isUndefined())
                continue;
            ++count;
            word(Text, reinterpret_cast<uintptr_t>(key));
            End end = write(held);
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

// (an array that has every element of its own, mark): whether no two elements are equal. The mark where only running code could say.
JSC_DEFINE_HOST_FUNCTION(validationHasUniqueItems, (JSGlobalObject * globalObject, CallFrame* callFrame))
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

JSC_DEFINE_HOST_FUNCTION(validationIsMultipleInDecimals, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(isMultipleInDecimals(callFrame->uncheckedArgument(0).asNumber(), callFrame->uncheckedArgument(1).asNumber())));
}

// Whether a value, with all that it holds under its enumerated keys and its indices, is one that a JSON text says. A member that holds undefined is absent.
class JSONLook {
public:
    // What one call has looked through and found good is not looked through again.
    JSONLook(JSGlobalObject* globalObject, JSMap* lookedThrough)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_lookedThrough(lookedThrough)
    {
    }

    ValidationLook look(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (value.isNumber())
            return std::isfinite(value.asNumber()) ? ValidationLook::OnlyJSON : ValidationLook::HoldsOther;
        if (value.isString() || value.isBoolean() || value.isNull())
            return ValidationLook::OnlyJSON;
        if (!value.isObject() || isFunctionType(value.asCell()->type()))
            return ValidationLook::HoldsOther;
        JSObject* object = asObject(value);
        if (object->type() == ProxyObjectType)
            return ValidationLook::NeedsCode;
        if (m_seen.contains(object) || (m_lookedThrough && m_lookedThrough->has(m_globalObject, object)))
            return ValidationLook::OnlyJSON;
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return ValidationLook::NeedsCode;
        }
        if (isAnyArray(value)) {
            unsigned length = asArray(object)->length();
            for (unsigned i = 0; i < length; ++i) {
                JSValue element = object->tryGetIndexQuickly(i);
                if (!element) {
                    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                    bool isPresent = object->methodTable()->getOwnPropertySlotByIndex(object, m_globalObject, i, slot);
                    RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
                    if (!isPresent)
                        return ValidationLook::HoldsOther;
                    if (!slot.isValue())
                        return ValidationLook::NeedsCode;
                    element = slot.getValue(m_globalObject, Identifier::from(m_vm, i));
                    RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
                }
                ValidationLook answer = look(element);
                if (answer != ValidationLook::OnlyJSON)
                    return answer;
            }
        } else {
            PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            object->methodTable()->getOwnPropertyNames(object, m_globalObject, names, DontEnumPropertiesMode::Exclude);
            RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
            for (auto& name : names) {
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                bool isPresent = object->methodTable()->getOwnPropertySlot(object, m_globalObject, name, slot);
                RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
                if (!isPresent)
                    continue;
                if (!slot.isValue())
                    return ValidationLook::NeedsCode;
                JSValue held = slot.getValue(m_globalObject, name);
                RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
                if (held.isUndefined())
                    continue;
                ValidationLook answer = look(held);
                if (answer != ValidationLook::OnlyJSON)
                    return answer;
            }
        }
        if (m_lookedThrough) {
            m_lookedThrough->set(m_globalObject, object, jsBoolean(true));
            RETURN_IF_EXCEPTION(scope, ValidationLook::NeedsCode);
        } else
            m_seen.add(object);
        return ValidationLook::OnlyJSON;
    }

private:
    JSGlobalObject* m_globalObject;
    VM& m_vm;
    JSMap* m_lookedThrough;
    UncheckedKeyHashSet<JSObject*> m_seen;
};

// (value, mark, the state of the call or undefined): a boolean, or the mark where only running code could say.
JSC_DEFINE_HOST_FUNCTION(validationSaysOnlyJSON, (JSGlobalObject * globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = callFrame->uncheckedArgument(0);
    // Nothing is read from a function, so nothing can be said of it.
    if (value.isCell() && isFunctionType(value.asCell()->type()))
        return JSValue::encode(callFrame->uncheckedArgument(1));
    JSMap* lookedThrough = nullptr;
    if (value.isObject() && callFrame->uncheckedArgument(2).isCell()) {
        JSArray* state = asArray(callFrame->uncheckedArgument(2));
        if (state->getIndexQuickly(LookedThroughInState).isUndefined())
            state->setIndexQuickly(vm, LookedThroughInState, JSMap::create(vm, globalObject->mapStructure()));
        lookedThrough = uncheckedDowncast<JSMap>(state->getIndexQuickly(LookedThroughInState).asCell());
    }
    ValidationLook answer = JSONLook(globalObject, lookedThrough).look(value);
    RETURN_IF_EXCEPTION(scope, {});
    if (answer == ValidationLook::NeedsCode)
        return JSValue::encode(callFrame->uncheckedArgument(1));
    return JSValue::encode(jsBoolean(answer == ValidationLook::OnlyJSON));
}

namespace {

enum class Mode : uint8_t { Check, Copy };

static bool isContainer(Shape shape) { return shape >= Shape::Array && shape <= Shape::Object; }

// A node that says nothing but the name of another is that other.
static uint32_t aliasOf(const Plan::Node& node)
{
    bool isAlias = node.shape == Shape::Unknown && node.defaultKind == Plan::Literal::None && node.rules.size() == 1 && node.rules[0].kind == RuleKind::Every && node.rules[0].nodes.size() == 1;
    return isAlias ? node.rules[0].nodes[0] : Plan::none;
}

struct Analysis : ThreadSafeRefCounted<Analysis> {
    // Functions of their own: the nodes that reach themselves, and large ones that several places name.
    Vector<bool> isFunction;
    Vector<bool> reachesItself;
    Vector<bool> reachesFunction;
    // The node, or an arm of it, is an object or an array. So a copy of the value is another value.
    Vector<bool> describesContainers;
    // The node, or an arm of it, puts something in the place of undefined.
    Vector<bool> appliesDefault;
    Vector<bool> acceptsUndefined;
    // An object or an array that can hold objects or arrays.
    Vector<bool> holdsContainers;
    bool hasFunctions { false };
    bool hasLoops { false };
    bool hasDefaults { false };
    bool takesHoles { false };
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
        analysis.isFunction.fill(false, size);
        analysis.reachesItself.fill(false, size);
        analysis.reachesFunction.fill(false, size);
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
            if (m_uses[i] > 1 && m_weight[i] > largestRepeatedNode)
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
        m_reaches.fill(Reaches::NotAsked, size);
        for (unsigned i = 0; i < size; ++i) {
            if (m_state[i] != State::Done)
                continue;
            m_analysis.reachesFunction[i] = reachesFunction(i);
            const Plan::Node& node = m_plan.nodes[i];
            if (!isContainer(node.shape))
                continue;
            bool holds = false;
            bool takesHoles = false;
            forEachChildOfShape(node, [&](uint32_t child) {
                holds |= m_analysis.describesContainers[child];
                takesHoles |= m_analysis.acceptsUndefined[child];
            });
            m_analysis.holdsContainers[i] = holds;
            if (node.shape == Shape::Array || node.shape == Shape::Tuple)
                m_analysis.takesHoles |= takesHoles;
            m_analysis.hasLoops |= node.shape != Shape::Object || node.undeclared == Plan::Undeclared::Validated;
            for (auto& rule : node.rules) {
                m_analysis.hasLoops |= rule.kind == RuleKind::PatternMembers;
                // What a rule looks at below the value is looked at as what the shape says is.
                for (uint32_t child : rule.nodes)
                    m_analysis.holdsContainers[i] = m_analysis.holdsContainers[i] || m_analysis.describesContainers[child];
            }
        }
    }

private:
    AOT_CLASS_THRESHOLD(unsigned, largestRepeatedNode, 12);
    AOT_CLASS_THRESHOLD(unsigned, deepestPlan, 400);
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

    void forEachAlternative(uint32_t index, NOESCAPE const Invocable<void(const Plan::Node&)> auto& visit) const
    {
        Vector<uint32_t, 8> alternatives { index };
        for (unsigned i = 0; i < alternatives.size(); ++i) {
            const Plan::Node& node = m_plan.nodes[alternatives[i]];
            if (uint32_t other = aliasOf(node); other != Plan::none) {
                if (!alternatives.contains(other))
                    alternatives.append(other);
                continue;
            }
            visit(node);
            if (node.shape != Shape::Union)
                continue;
            for (uint32_t arm : node.children) {
                if (!alternatives.contains(arm))
                    alternatives.append(arm);
            }
        }
    }

    bool reachesFunction(uint32_t index)
    {
        if (m_analysis.isFunction[index])
            return true;
        if (m_reaches[index] != Reaches::NotAsked)
            return m_reaches[index] == Reaches::Yes;
        const Plan::Node& node = m_plan.nodes[index];
        bool reaches = false;
        forEachChildOfShape(node, [&](uint32_t child) {
            reaches = reaches || reachesFunction(child);
        });
        for (auto& rule : node.rules) {
            for (uint32_t child : rule.nodes)
                reaches = reaches || reachesFunction(child);
        }
        m_reaches[index] = reaches ? Reaches::Yes : Reaches::No;
        return reaches;
    }

    // Each rule speaks of one kind of value.
    static bool suits(RuleKind kind, Shape shape)
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

    static unsigned nodesOf(RuleKind kind)
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
        if (index >= m_plan.nodes.size() || m_depth > deepestPlan) {
            m_analysis.isSupported = false;
            return 0;
        }
        SetForScope depth(m_depth, m_depth + 1);
        ++m_uses[index];
        if (m_state[index] == State::Open) {
            m_analysis.isFunction[index] = true;
            m_analysis.reachesItself[index] = true;
            return 1;
        }
        if (m_state[index] == State::Done)
            return m_weight[index];
        m_state[index] = State::Open;
        const Plan::Node& node = m_plan.nodes[index];
        unsigned weight = 1 + node.rules.size();
        bool needsChild = node.shape == Shape::Array || node.shape == Shape::Dictionary || (node.shape == Shape::Object && node.undeclared == Plan::Undeclared::Validated);
        if (needsChild && node.child == Plan::none)
            m_analysis.isSupported = false;
        if (node.shape == Shape::Tuple && node.required > node.children.size())
            m_analysis.isSupported = false;
        forEachChildOfShape(node, [&](uint32_t child) {
            weight += visit(child);
        });
        for (auto& rule : node.rules) {
            if (!suits(rule.kind, node.shape) || (nodesOf(rule.kind) && rule.nodes.size() != nodesOf(rule.kind)))
                m_analysis.isSupported = false;
            if (rule.kind == RuleKind::PatternMembers && rule.texts.size() != rule.nodes.size())
                m_analysis.isSupported = false;
            if (rule.kind == RuleKind::Template && (rule.texts.size() < 2 || rule.text.length() + 1 != rule.texts.size()))
                m_analysis.isSupported = false;
            for (uint32_t child : rule.nodes)
                weight += visit(child);
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

    // A text shares nothing, so what is made of one has as many objects as the text says.
    bool limitsVisits() const { return kind != ValidatorKind::Parse && (analysis->hasFunctions || analysis->hasLoops); }
    bool countsVisits() const { return analysis->hasFunctions || limitsVisits(); }
    bool countsHoles() const { return kind != ValidatorKind::Parse && analysis->takesHoles; }
    // The functions of a validator have the counts in one place.
    bool hasState() const { return analysis->hasFunctions; }
    // Nobody else has what a text was made into, so only a default changes it, and then it is copied as any value is.
    Mode mode() const { return kind == ValidatorKind::Validate || (kind == ValidatorKind::Parse && analysis->hasDefaults) ? Mode::Copy : Mode::Check; }

    const Ref<Plan> plan;
    const Ref<Analysis> analysis;
    JSGlobalObject* const globalObject;
    const ValidatorKind kind;
};

// What one writing of a validator's functions has in common. The first function holds all of it as constants.
struct Functions {
    JSFunction* checks { nullptr };
    JSFunction* copies { nullptr };
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
        m_mark = a.constant(symbolOf(m_vm, endOfCheckName));
        m_failure = a.constant(symbolOf(m_vm, failureName));
        if (functions.state)
            m_state = a.constant(functions.state);
    }

    void emitCheck()
    {
        Target fail = a.newLabel();
        m_refuse = fail;
        m_tooMany = fail;
        emitBeginning();
        emitUse(m_plan.root, a.argument(0), fail, false);
        a.returnValue(a.constant(jsBoolean(true)));
        a.bind(fail);
        a.returnValue(a.constant(jsBoolean(false)));
        emitDeferred();
    }

    void emitValidate(JSObject* onRefused, JSObject* onNotLookedAt)
    {
        Target fail = a.newLabel();
        m_refuse = a.newLabel();
        m_tooMany = a.newLabel();
        Slot value = a.newRegister();
        emitBeginning();
        a.move(value, a.argument(0));
        emitUse(m_plan.root, value, fail, false);
        a.returnValue(value);
        Slot given[] = { a.argument(0) };
        a.bind(fail);
        a.call(value, onRefused, given);
        a.returnValue(value);
        a.bind(m_refuse);
        a.call(value, onNotLookedAt, given);
        a.returnValue(value);
        emitTooMany();
        emitDeferred();
    }

    void emitParse(JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments)
    {
        Target fail = a.newLabel();
        Target isOther = a.newLabel();
        m_refuse = a.newLabel();
        m_tooMany = m_refuse;
        Slot parsed = a.newRegister();
        Slot value = a.newRegister();
        Slot count = a.newRegister();
        Slot one = a.constant(jsNumber(1));
        a.readArgumentCount(count);
        a.jumpIfStrictlyEqual(count, one, false, isOther);
        a.jumpIfCellType(a.argument(0), StringType, false, isOther);
        callHelper(parsed, m_parseText, validationParseText, { a.argument(0) });
        emitBeginning();
        a.move(value, parsed);
        emitUse(m_plan.root, value, fail, false);
        a.returnValue(m_mode == Mode::Copy ? value : parsed);
        Slot made[] = { parsed };
        a.bind(fail);
        a.call(value, onRefused, made);
        a.returnValue(value);
        a.bind(m_refuse);
        a.call(value, onNotLookedAt, made);
        a.returnValue(value);
        // It is handed as many arguments as came.
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

    // function (value, node). Where it checks: true, false, or undefined where the whole check ends in a refusal. Where it copies: the copy, or one of the two symbols.
    void emitFunctionOfNodes()
    {
        m_refuse = a.newLabel();
        m_tooMany = m_shared.kind == ValidatorKind::Validate ? a.newLabel() : m_refuse;
        bool copies = m_mode == Mode::Copy;
        Vector<std::pair<uint32_t, Target>> entries;
        for (unsigned i = 0; i < m_plan.nodes.size(); ++i) {
            if (!m_analysis.isFunction[i])
                continue;
            entries.append({ i, a.newLabel() });
            a.jumpIfStrictlyEqual(a.argument(1), a.constant(jsNumber(i)), true, entries.last().second);
        }
        a.jump(m_refuse);
        for (auto [node, label] : entries) {
            SlotScope scope(a);
            a.bind(label);
            Target fail = a.newLabel();
            Slot value = a.newRegister();
            a.move(value, a.argument(0));
            // A copy is made anew at every place, so only what a check has said is kept.
            bool remembers = !copies && m_analysis.reachesItself[node];
            Slot memory = remembers ? a.newRegister() : nullptr;
            Slot verdict = remembers ? a.newRegister() : nullptr;
            Target isNotKnown = a.newLabel();
            Target finish = a.newLabel();
            if (remembers) {
                a.readElement(memory, m_state, a.constant(jsNumber(MemoryInState)));
                a.jumpIfStrictlyEqual(memory, m_undefined, true, isNotKnown);
                callHelper(verdict, m_recall, validationRecall, { memory, a.argument(1), a.argument(0) });
                a.jumpIfStrictlyEqual(verdict, m_undefined, true, isNotKnown);
                a.returnValue(verdict);
                a.bind(isNotKnown);
            }
            emitNode(node, value, fail, true);
            if (!remembers) {
                a.returnValue(copies ? value : a.constant(jsBoolean(true)));
                a.bind(fail);
                a.returnValue(copies ? m_failure : a.constant(jsBoolean(false)));
                continue;
            }
            a.move(verdict, a.constant(jsBoolean(true)));
            a.jump(finish);
            a.bind(fail);
            a.move(verdict, a.constant(jsBoolean(false)));
            a.bind(finish);
            Target isDone = a.newLabel();
            a.readElement(memory, m_state, a.constant(jsNumber(MemoryInState)));
            a.jumpIfStrictlyEqual(memory, m_undefined, true, isDone);
            callHelper(value, m_remember, validationRemember, { memory, a.argument(1), a.argument(0), verdict });
            a.bind(isDone);
            a.returnValue(verdict);
        }
        a.bind(m_refuse);
        a.returnValue(copies ? m_mark : m_undefined);
        if (m_tooMany != m_refuse)
            emitTooMany();
        emitDeferred();
    }

private:
    class SlotScope {
    public:
        explicit SlotScope(Writer& assembler)
            : m_assembler(assembler)
            , m_count(assembler.registersInUse())
        {
        }
        ~SlotScope() { m_assembler.releaseRegistersFrom(m_count); }

    private:
        Writer& m_assembler;
        unsigned m_count;
    };

    const Plan::Node& nodeAt(uint32_t index) const { return m_plan.nodes[index]; }
    bool copies() const { return m_mode == Mode::Copy; }
    // Whether what the node hands on can be another value than it was given.
    bool changes(uint32_t index) const { return copies() && (m_analysis.describesContainers[index] || m_analysis.appliesDefault[index]); }
    bool saysNothing(uint32_t index) const { return nodeAt(index).shape == Shape::Unknown && nodeAt(index).rules.isEmpty() && nodeAt(index).defaultKind == Plan::Literal::None; }

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
                a.keepUntilTheEnd(one);
        }
        a.keepRegistersInUse();
        m_deferred.append(WTF::move(block));
    }

    void emitDeferred()
    {
        for (unsigned i = 0; i < m_deferred.size(); ++i)
            m_deferred[i]();
    }

    void emitBeginning()
    {
        if (m_state) {
            a.writeOwnElement(m_state, a.constant(jsNumber(VisitsInState)), m_zero);
            a.writeOwnElement(m_state, a.constant(jsNumber(MemoryInState)), m_undefined);
            a.writeOwnElement(m_state, a.constant(jsNumber(LookedThroughInState)), m_undefined);
            if (m_shared.countsHoles())
                a.writeOwnElement(m_state, a.constant(jsNumber(HolesInState)), a.constant(jsNumber(mostHoles)));
            return;
        }
        if (m_shared.countsVisits()) {
            m_visits = a.newRegister();
            a.move(m_visits, m_zero);
        }
        if (m_shared.countsHoles()) {
            m_holes = a.newRegister();
            a.move(m_holes, a.constant(jsNumber(mostHoles)));
        }
    }

    void emitTooMany()
    {
        a.bind(m_tooMany);
        Slot nothing = a.newRegister();
        callHelper(nothing, m_throwTooManyVisits, validationThrowTooManyVisits, { });
        a.returnValue(nothing);
    }

    void readVisits(Slot destination)
    {
        if (m_state)
            a.readElement(destination, m_state, a.constant(jsNumber(VisitsInState)));
        else
            a.move(destination, m_visits);
    }

    // A value may hold one object at many places, and each place is looked at.
    void emitCountVisit(const Plan::Node& node)
    {
        if (!m_shared.countsVisits() || !m_analysis.holdsContainers[&node - m_plan.nodes.span().data()])
            return;
        SlotScope scope(a);
        Slot visits = m_visits;
        if (m_state) {
            visits = a.newRegister();
            readVisits(visits);
        }
        a.increment(visits);
        if (m_state)
            a.writeOwnElement(m_state, a.constant(jsNumber(VisitsInState)), visits);
        if (m_shared.limitsVisits())
            a.jumpIfCompares(Comparison::Greater, visits, a.constant(jsNumber(mostVisits)), true, m_tooMany);
    }

    // An arm that has looked below its value and then failed: the next arm looks there again, and so at every level.
    void emitAfterFailedArm(Slot visitsBefore)
    {
        SlotScope scope(a);
        Slot visits = a.newRegister();
        Target isCheap = a.newLabel();
        readVisits(visits);
        a.compute(Arithmetic::Subtract, visits, visits, visitsBefore);
        a.jumpIfCompares(Comparison::Greater, visits, a.constant(jsNumber(1)), false, isCheap);
        if (copies())
            a.jump(m_refuse);
        else
            callHelper(visits, m_startRemembering, validationStartRemembering, { m_state });
        a.bind(isCheap);
    }

    void emitStartRemembering(std::span<const uint32_t> nodes)
    {
        bool reaches = false;
        for (uint32_t node : nodes)
            reaches |= m_analysis.reachesFunction[node];
        if (!reaches)
            return;
        SlotScope scope(a);
        Slot memory = a.newRegister();
        Target remembers = a.newLabel();
        a.readElement(memory, m_state, a.constant(jsNumber(MemoryInState)));
        a.jumpIfStrictlyEqual(memory, m_undefined, false, remembers);
        callHelper(memory, m_startRemembering, validationStartRemembering, { m_state });
        a.bind(remembers);
    }

    // What the node makes of the value is left in `value`. isSoft: a failure here does not end the check.
    void emitUse(uint32_t index, Slot value, Target fail, bool isSoft)
    {
        if (!m_analysis.isFunction[index]) {
            emitNode(index, value, fail, isSoft);
            return;
        }
        SlotScope scope(a);
        Slot result = a.newRegister();
        Slot arguments[] = { value, a.constant(jsNumber(index)) };
        if (copies()) {
            a.call(result, m_functions.copies, arguments);
            a.jumpIfStrictlyEqual(result, m_failure, true, fail);
            a.jumpIfStrictlyEqual(result, m_mark, true, m_refuse);
            a.move(value, result);
            return;
        }
        a.call(result, m_functions.checks, arguments);
        Target passed = a.newLabel();
        a.jumpIfTruthy(result, true, passed);
        a.jumpIfStrictlyEqual(result, m_undefined, true, m_refuse);
        a.jump(fail);
        a.bind(passed);
    }

    // A rule asks whether a node takes the value. Nothing is made.
    void emitTest(uint32_t index, Slot value, Target fail, bool isSoft)
    {
        SetForScope mode(m_mode, Mode::Check);
        if (!m_analysis.describesContainers[index] && !m_analysis.appliesDefault[index]) {
            emitUse(index, value, fail, isSoft);
            return;
        }
        SlotScope scope(a);
        Slot tested = a.newRegister();
        a.move(tested, value);
        emitUse(index, tested, fail, isSoft);
    }

    uint32_t indexOf(const Plan::Node& node) const { return &node - m_plan.nodes.span().data(); }

    // A rule under which a node that refuses lets the value pass first looks through all of its value, at any depth, below what no node describes too:
    // it fails if the value holds what no JSON text says (a number that is not finite, undefined or a hole among elements, a bigint, a symbol, a function).
    // The outermost such rule fails first, so nothing turns that round.
    void emitLookThrough(const Plan::Node& node, Slot value, Target fail)
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
        SlotScope scope(a);
        Slot answer = a.newRegister();
        callHelper(answer, m_saysOnlyJSON, validationSaysOnlyJSON, { value, m_mark, m_state ? m_state : m_undefined });
        a.jumpIfStrictlyEqual(answer, m_mark, true, m_refuse);
        a.jumpIfTruthy(answer, false, fail);
    }

    JSValue defaultOf(const Plan::Node& node)
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
        const Plan::Node& node = nodeAt(index);
        if (JSValue standIn = defaultOf(node)) {
            Target isThere = a.newLabel();
            a.jumpIfStrictlyEqual(value, m_undefined, false, isThere);
            a.move(value, a.constant(standIn));
            a.bind(isThere);
        }
        Target misfit = fail;
        switch (node.shape) {
        case Shape::Unknown:
            break;
        case Shape::Never:
            a.jump(misfit);
            return;
        case Shape::Undefined:
            a.jumpIfStrictlyEqual(value, m_undefined, false, misfit);
            break;
        case Shape::Null:
            a.jumpIfStrictlyEqual(value, a.constant(jsNull()), false, misfit);
            break;
        case Shape::AnyObject:
            a.jumpIfObject(value, false, misfit);
            break;
        case Shape::Boolean:
            a.jumpIfBoolean(value, false, misfit);
            break;
        case Shape::BigInt:
            a.jumpIfCellType(value, HeapBigIntType, false, misfit);
            break;
        case Shape::BooleanLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsBoolean(node.boolean)), false, misfit);
            break;
        case Shape::NumberLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsNumber(node.number)), false, misfit);
            break;
        case Shape::StringLiteral:
            a.jumpIfStrictlyEqual(value, a.constant(jsString(m_vm, node.string)), false, misfit);
            break;
        case Shape::Number:
            emitNumber(node, value, misfit);
            break;
        case Shape::String:
            emitString(node, value, misfit, fail);
            break;
        case Shape::Array:
        case Shape::Tuple:
            emitArray(node, value, misfit, fail, isSoft);
            break;
        case Shape::Dictionary:
        case Shape::Object:
            emitObjectGuard(readsByKey(node), value, misfit);
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
                emitLookThrough(node, value, fail);
                emitStartRemembering(rule.nodes.span());
                emitTest(rule.nodes[0], value, refused, true);
                a.jump(fail);
                a.bind(refused);
                break;
            }
            case RuleKind::If: {
                Target otherwise = a.newLabel();
                Target done = a.newLabel();
                emitLookThrough(node, value, fail);
                emitStartRemembering(rule.nodes.span());
                emitTest(rule.nodes[0], value, otherwise, true);
                emitTest(rule.nodes[1], value, fail, isSoft);
                a.jump(done);
                a.bind(otherwise);
                emitTest(rule.nodes[2], value, fail, isSoft);
                a.bind(done);
                break;
            }
            case RuleKind::ExactlyOne: {
                SlotScope scope(a);
                Slot hasPassed = a.newRegister();
                emitLookThrough(node, value, fail);
                emitStartRemembering(rule.nodes.span());
                a.move(hasPassed, a.constant(jsBoolean(false)));
                for (uint32_t arm : rule.nodes) {
                    Target missed = a.newLabel();
                    emitTest(arm, value, missed, true);
                    a.jumpIfTruthy(hasPassed, true, fail);
                    a.move(hasPassed, a.constant(jsBoolean(true)));
                    a.bind(missed);
                }
                a.jumpIfTruthy(hasPassed, false, fail);
                break;
            }
            case RuleKind::Every:
                // A node that says nothing but the name of another is that other.
                if (aliasOf(node) != Plan::none) {
                    emitUse(rule.nodes[0], value, fail, isSoft);
                    break;
                }
                if (rule.nodes.size() > 1)
                    emitStartRemembering(rule.nodes.span());
                for (uint32_t part : rule.nodes)
                    emitTest(part, value, fail, isSoft);
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
        SlotScope scope(a);
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
                if ((rule.flags & Plan::byDecimals) && rule.number != std::trunc(rule.number)) {
                    callHelper(scratch, m_isMultipleInDecimals, validationIsMultipleInDecimals, { value, bound });
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

    JSArray* arrayOfStrings(const auto& strings, NOESCAPE const auto& stringOf)
    {
        JSArray* array = constructEmptyArray(m_shared.globalObject, nullptr);
        unsigned position = 0;
        for (auto& one : strings)
            array->putDirectIndex(m_shared.globalObject, position++, jsOwnedString(m_vm, stringOf(one)));
        return array;
    }

    void emitString(const Plan::Node& node, Slot value, Target misfit, Target fail)
    {
        SlotScope scope(a);
        a.jumpIfCellType(value, StringType, false, misfit);
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
                callHelper(scratch, m_codePointLength, validationCodePointLength, { value });
                a.jumpIfCompares(Comparison::Less, scratch, a.constant(jsNumber(rule.count)), true, fail);
                a.bind(isLongEnough);
                break;
            }
            case RuleKind::MaximumLength: {
                needUnits();
                Target isShortEnough = a.newLabel();
                a.jumpIfCompares(Comparison::LessOrEqual, units, a.constant(jsNumber(rule.count)), true, isShortEnough);
                a.jumpIfCompares(Comparison::Greater, units, a.constant(jsNumber(2.0 * rule.count)), true, fail);
                callHelper(scratch, m_codePointLength, validationCodePointLength, { value });
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
                callHelper(scratch, m_matchesPattern, validationMatchesPattern, { a.constant(RegExp::create(m_vm, rule.text, OptionSet<Yarr::Flags>::fromRaw(rule.flags))), value });
                break;
            case RuleKind::Format:
                callHelper(scratch, m_matchesFormat, validationMatchesFormat, { a.constant(jsNumber(rule.count)), value });
                break;
            case RuleKind::Url:
                callHelper(scratch, m_isUrl, validationIsUrl, { value });
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
                callHelper(scratch, m_matchesTemplate, validationMatchesTemplate, { value, a.constant(arrayOfStrings(rule.texts, [](const String& text) { return text; })), a.constant(jsOwnedString(m_vm, rule.text)) });
                break;
            }
            a.jumpIfTruthy(scratch, false, fail);
        }
    }

    bool readsByKey(const Plan::Node& node) const
    {
        if (findRule(node, RuleKind::PatternMembers))
            return true;
        if (node.shape == Shape::Dictionary)
            return !saysNothing(node.child);
        return node.undeclared == Plan::Undeclared::Validated && !saysNothing(node.child);
    }

    // Behind it `value` is an object all of whose properties are values that are enumerated, and that nobody else has where a copy is made.
    void emitObjectGuard(bool readsByKey, Slot value, Target fail)
    {
        if (copies()) {
            Target isOther = a.newLabel();
            Slot copy = a.newRegister();
            a.jumpIfCellType(value, FinalObjectType, false, isOther);
            callHelper(copy, m_copyPlainObject, validationCopyPlainObject, { value });
            a.jumpIfStrictlyEqual(copy, m_undefined, true, m_refuse);
            a.move(value, copy);
            defer({ value }, [=, this] {
                a.bind(isOther);
                a.jumpIfObject(value, false, fail);
                a.jumpIfCellType(value, ArrayType, true, fail);
                a.jumpIfCellType(value, DerivedArrayType, true, fail);
                a.jump(m_refuse);
            });
            return;
        }
        Target isOdd = a.newLabel();
        Target isPlain = a.newLabel();
        Slot copy = a.newRegister();
        a.jumpIfCellType(value, FinalObjectType, false, isOdd);
        a.jumpIfStructureFlags(value, Structure::s_hasAnyKindOfGetterSetterPropertiesBits | Structure::s_hasNonEnumerablePropertiesBits, true, isOdd);
        if (readsByKey) {
            callHelper(copy, m_hasPlainElements, validationHasPlainElements, { value });
            a.jumpIfTruthy(copy, false, isOdd);
        }
        a.bind(isPlain);
        defer({ value, copy }, [=, this] {
            a.bind(isOdd);
            callHelper(copy, m_plainObjectFor, validationPlainObjectFor, { value, m_mark });
            a.jumpIfStrictlyEqual(copy, m_undefined, true, fail);
            a.jumpIfStrictlyEqual(copy, m_mark, true, m_refuse);
            a.move(value, copy);
            a.jump(isPlain);
        });
    }

    unsigned costOf(uint32_t index) const
    {
        const Plan::Node& node = nodeAt(index);
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
                if (nodeAt(arm).shape != Shape::StringLiteral)
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

    // What is said of every key of an object, and of what it holds under the keys that are not declared.
    bool needsKeysLoop(const Plan::Node& node) const
    {
        return readsByKey(node) || findRule(node, RuleKind::MemberNames);
    }

    void emitKeysLoop(const Plan::Node& node, Slot object, Target fail, bool isSoft)
    {
        SlotScope scope(a);
        const Plan::Rule* names = findRule(node, RuleKind::MemberNames);
        const Plan::Rule* patterns = findRule(node, RuleKind::PatternMembers);
        bool rejects = node.shape == Shape::Object && node.undeclared == Plan::Undeclared::Rejected;
        uint32_t valueNode = node.shape == Shape::Dictionary || node.undeclared == Plan::Undeclared::Validated ? node.child : Plan::none;
        if (valueNode != Plan::none && saysNothing(valueNode))
            valueNode = Plan::none;
        Slot keys = a.newRegister();
        Slot count = a.newRegister();
        Slot index = a.newRegister();
        Slot key = a.newRegister();
        Slot held = a.newRegister();
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
            emitTest(names->nodes[0], key, fail, isSoft);
        if (patterns) {
            a.move(isCovered, a.constant(jsBoolean(false)));
            for (unsigned i = 0; i < patterns->nodes.size(); ++i) {
                Target isNoMatch = a.newLabel();
                callHelper(held, m_matchesPattern, validationMatchesPattern, { a.constant(RegExp::create(m_vm, patterns->texts[i], OptionSet<Yarr::Flags>::fromRaw(patterns->flags))), key });
                a.jumpIfTruthy(held, false, isNoMatch);
                a.move(isCovered, a.constant(jsBoolean(true)));
                if (!saysNothing(patterns->nodes[i])) {
                    a.readElement(held, object, key);
                    emitTest(patterns->nodes[i], held, fail, isSoft);
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
                a.readElement(held, object, key);
                emitUse(valueNode, held, fail, isSoft);
                if (changes(valueNode))
                    a.writeOwnElement(object, key, held);
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
        callHelper(destination, m_ownValue, validationOwnValue, { object, a.constant(jsOwnedString(m_vm, name.string())), m_mark });
        a.jumpIfStrictlyEqual(destination, m_mark, true, m_refuse);
    }

    void emitRulesOfObjects(const Plan::Node& node, Slot value, Target fail, bool isSoft)
    {
        for (auto& rule : node.rules) {
            SlotScope scope(a);
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
                Slot held = a.newRegister();
                Target isAbsent = a.newLabel();
                readOwn(held, value, rule.text);
                a.jumpIfStrictlyEqual(held, m_undefined, true, isAbsent);
                for (auto& name : rule.texts) {
                    readOwn(held, value, name);
                    a.jumpIfStrictlyEqual(held, m_undefined, true, fail);
                }
                if (rule.kind == RuleKind::DependentNode) {
                    emitLookThrough(node, value, fail);
                    emitStartRemembering(rule.nodes.span());
                    emitTest(rule.nodes[0], value, fail, isSoft);
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
        callHelper(destination, m_ownValue, validationOwnValue, { object, a.constant(jsOwnedString(m_vm, member.key.string())), m_mark });
        a.jumpIfStrictlyEqual(destination, m_mark, true, m_refuse);
    }

    void writeMember(Slot object, const Plan::Member& member, Slot value)
    {
        if (auto index = parseIndex(*member.key.impl()))
            a.writeOwnElement(object, a.constant(jsNumber(*index)), value);
        else
            a.writeOwnProperty(object, member.key.impl(), value);
    }

    // The value is a plain object. The member that chose this arm of a union has been looked at.
    void emitObject(const Plan::Node& node, Slot value, Target fail, bool isSoft, const UniquedStringImpl* chosenBy)
    {
        SlotScope scope(a);
        emitCountVisit(node);
        if (node.shape == Shape::Dictionary) {
            emitRulesOfObjects(node, value, fail, isSoft);
            if (needsKeysLoop(node))
                emitKeysLoop(node, value, fail, isSoft);
            return;
        }
        // A key that a pattern takes is declared, which only a look at each key shows.
        bool isClosed = node.undeclared == Plan::Undeclared::Rejected && !findRule(node, RuleKind::PatternMembers);
        // Whether such a member is among the keys shows only when the code runs.
        auto mayBeAbsent = [&](const Plan::Member& member) {
            return member.isOptional || m_analysis.appliesDefault[member.node];
        };
        unsigned alwaysPresent = 0;
        bool someMayBeAbsent = false;
        for (auto& member : node.members) {
            alwaysPresent += !mayBeAbsent(member);
            someMayBeAbsent |= mayBeAbsent(member);
        }
        Slot held = a.newRegister();
        Slot present = nullptr;
        if (isClosed && someMayBeAbsent) {
            present = a.newRegister();
            a.move(present, a.constant(jsNumber(alwaysPresent)));
        }
        // A default that is filled in becomes the last key, so where a copy is made such members keep their order.
        bool fillsDefaults = false;
        for (auto& member : node.members)
            fillsDefaults |= copies() && m_analysis.appliesDefault[member.node];
        Vector<const Plan::Member*, 16> cheapestFirst;
        for (unsigned cost = 0; cost < 3; ++cost) {
            for (auto& member : node.members) {
                if (fillsDefaults ? !cost : costOf(member.node) == cost)
                    cheapestFirst.append(&member);
            }
        }
        for (auto* next : cheapestFirst) {
            auto& member = *next;
            if (member.key.impl() == chosenBy)
                continue;
            if (member.isOptional && saysNothing(member.node) && !isClosed)
                continue;
            bool appliesDefault = m_analysis.appliesDefault[member.node];
            Target isAbsent = a.newLabel();
            readMember(held, value, member);
            if (member.isOptional && !appliesDefault)
                a.jumpIfStrictlyEqual(held, m_undefined, true, isAbsent);
            else if (!member.isOptional && !member.mustBeOwn && !appliesDefault && m_analysis.acceptsUndefined[member.node])
                a.jumpIfStrictlyEqual(held, m_undefined, true, fail);
            // A default is written into a copy, and is then one of its keys.
            if (present && mayBeAbsent(member) && !copies()) {
                Target isCounted = a.newLabel();
                a.jumpIfStrictlyEqual(held, m_undefined, true, isCounted);
                a.increment(present);
                a.bind(isCounted);
            }
            emitUse(member.node, held, fail, isSoft);
            if (member.mustBeOwn) {
                Target isOwn = a.newLabel();
                Slot answer = a.newRegister();
                a.jumpIfStrictlyEqual(held, m_undefined, false, isOwn);
                callHelper(answer, m_hasOwnKey, validationHasOwnKey, { value, a.constant(jsOwnedString(m_vm, member.key.string())) });
                a.jumpIfTruthy(answer, false, fail);
                a.bind(isOwn);
            }
            if (changes(member.node)) {
                if (m_analysis.acceptsUndefined[member.node]) {
                    Target isNothing = a.newLabel();
                    a.jumpIfStrictlyEqual(held, m_undefined, true, isNothing);
                    writeMember(value, member, held);
                    a.bind(isNothing);
                } else
                    writeMember(value, member, held);
            }
            if (present && mayBeAbsent(member) && copies()) {
                Target isCounted = a.newLabel();
                a.jumpIfStrictlyEqual(held, m_undefined, true, isCounted);
                a.increment(present);
                a.bind(isCounted);
            }
            a.bind(isAbsent);
        }
        if (isClosed) {
            Target askByName = a.newLabel();
            Target done = a.newLabel();
            callHelper(held, m_objectKeys, objectConstructorKeys, { value }, ObjectKeysIntrinsic);
            a.readLength(held, held);
            a.jumpIfStrictlyEqual(held, present ? present : a.constant(jsNumber(alwaysPresent)), false, askByName);
            a.bind(done);
            const Plan::Node* declaredBy = &node;
            defer({ value, held }, [=, this] {
                a.bind(askByName);
                JSArray* names = arrayOfStrings(declaredBy->members, [](const Plan::Member& member) { return member.key.string(); });
                callHelper(held, m_hasOnlyTheseKeys, validationHasOnlyTheseKeys, { value, a.constant(names) });
                a.jumpIfTruthy(held, false, fail);
                a.jump(done);
            });
        }
        emitRulesOfObjects(node, value, fail, isSoft);
        if (needsKeysLoop(node))
            emitKeysLoop(node, value, fail, isSoft);
    }

    void emitArray(const Plan::Node& node, Slot value, Target misfit, Target fail, bool isSoft)
    {
        SlotScope scope(a);
        Slot length = a.newRegister();
        unsigned first = node.shape == Shape::Tuple ? node.children.size() : 0;
        double fewest = node.shape == Shape::Tuple ? node.required : 0;
        double most = node.shape == Shape::Tuple && node.child == Plan::none ? first : std::numeric_limits<uint32_t>::max();
        for (auto& rule : node.rules) {
            if (rule.kind == RuleKind::MinimumLength)
                fewest = std::max<double>(fewest, rule.count);
            else if (rule.kind == RuleKind::MaximumLength)
                most = std::min<double>(most, rule.count);
        }
        bool takesHoles = node.child != Plan::none && m_analysis.acceptsUndefined[node.child];
        for (uint32_t child : node.children)
            takesHoles |= m_analysis.acceptsUndefined[child];

        if (copies()) {
            Target isOther = a.newLabel();
            Target isOdd = a.newLabel();
            a.jumpIfCellType(value, ArrayType, false, isOther);
            callHelper(length, m_copyPlainArray, validationCopyPlainArray, { value, a.constant(jsNumber(fewest)), a.constant(jsNumber(most)), m_mark });
            a.jumpIfCellType(length, ArrayType, false, isOdd);
            a.move(value, length);
            a.readLength(length, value);
            defer({ value, length }, [=, this] {
                a.bind(isOther);
                a.jumpIfCellType(value, DerivedArrayType, true, m_refuse);
                a.jumpIfCellType(value, ProxyObjectType, true, m_refuse);
                a.jump(misfit);
                a.bind(isOdd);
                a.jumpIfStrictlyEqual(length, m_undefined, true, fail);
                a.jump(m_refuse);
            });
        } else {
            Target isOdd = a.newLabel();
            Target isPlain = a.newLabel();
            Target hasHoles = a.newLabel();
            bool countsHoles = takesHoles && m_shared.countsHoles();
            Slot holes = countsHoles ? a.newRegister() : nullptr;
            a.jumpIfCellType(value, ArrayType, false, isOdd);
            callHelper(length, m_plainArrayLength, validationPlainArrayLength, { value });
            a.jumpIfCompares(Comparison::Less, length, m_zero, true, isOdd);
            if (countsHoles) {
                callHelper(holes, m_holesOf, validationHolesOf, { value });
                a.jumpIfTruthy(holes, true, hasHoles);
            }
            a.bind(isPlain);
            defer({ value, length, holes }, [=, this] {
                a.bind(isOdd);
                if (countsHoles)
                    callHelper(holes, m_holesOf, validationHolesOf, { value });
                callHelper(length, m_plainArrayFor, validationPlainArrayFor, { value, m_mark });
                a.jumpIfStrictlyEqual(length, m_undefined, true, misfit);
                a.jumpIfStrictlyEqual(length, m_mark, true, m_refuse);
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
                    a.readElement(left, m_state, a.constant(jsNumber(HolesInState)));
                }
                a.compute(Arithmetic::Subtract, left, left, holes);
                if (m_state)
                    a.writeOwnElement(m_state, a.constant(jsNumber(HolesInState)), left);
                a.jumpIfCompares(Comparison::Less, left, m_zero, true, m_refuse);
                a.jump(isPlain);
            });
            if (fewest)
                a.jumpIfCompares(Comparison::Less, length, a.constant(jsNumber(fewest)), true, fail);
            if (most < std::numeric_limits<uint32_t>::max())
                a.jumpIfCompares(Comparison::Greater, length, a.constant(jsNumber(most)), true, fail);
        }
        emitCountVisit(node);

        Slot held = a.newRegister();
        Target done = a.newLabel();
        for (unsigned i = 0; i < first; ++i) {
            Slot position = a.constant(jsNumber(i));
            if (i >= node.required)
                a.jumpIfCompares(Comparison::Greater, length, position, false, done);
            if (saysNothing(node.children[i]))
                continue;
            a.readElement(held, value, position);
            emitUse(node.children[i], held, fail, isSoft);
            if (changes(node.children[i]))
                a.writeOwnElement(value, position, held);
        }
        if (node.child != Plan::none && !saysNothing(node.child)) {
            Slot index = a.newRegister();
            a.move(index, a.constant(jsNumber(first)));
            Target head = a.newLabel();
            a.bind(head);
            a.loopHint();
            a.jumpIfCompares(Comparison::Less, index, length, false, done);
            a.readElement(held, value, index);
            emitUse(node.child, held, fail, isSoft);
            if (changes(node.child))
                a.writeOwnElement(value, index, held);
            a.increment(index);
            a.jump(head);
        }
        a.bind(done);

        for (auto& rule : node.rules) {
            if (rule.kind == RuleKind::UniqueItems) {
                callHelper(held, m_hasUniqueItems, validationHasUniqueItems, { value, m_mark });
                a.jumpIfStrictlyEqual(held, m_mark, true, m_refuse);
                a.jumpIfTruthy(held, false, fail);
            }
            if (rule.kind != RuleKind::Contains)
                continue;
            SlotScope ruleScope(a);
            Slot index = a.newRegister();
            Slot found = a.newRegister();
            Target head = a.newLabel();
            Target missed = a.newLabel();
            Target exit = a.newLabel();
            Target enough = a.newLabel();
            if (rule.most != Plan::none)
                emitLookThrough(node, value, fail);
            emitStartRemembering(rule.nodes.span());
            a.move(index, m_zero);
            a.move(found, m_zero);
            a.bind(head);
            a.loopHint();
            a.jumpIfCompares(Comparison::Less, index, length, false, exit);
            a.readElement(held, value, index);
            emitTest(rule.nodes[0], held, missed, true);
            a.increment(found);
            if (rule.most == Plan::none)
                a.jumpIfCompares(Comparison::GreaterOrEqual, found, a.constant(jsNumber(rule.count)), true, enough);
            else
                a.jumpIfCompares(Comparison::Greater, found, a.constant(jsNumber(rule.most)), true, fail);
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
        const Plan::Node& node = nodeAt(index);
        if (node.shape == Shape::Union && node.rules.isEmpty() && node.defaultKind == Plan::Literal::None && !m_analysis.isFunction[index]) {
            for (uint32_t arm : node.children)
                appendLiterals(arm, literals, areAllStrings);
            return;
        }
        areAllStrings &= node.shape == Shape::StringLiteral && node.rules.isEmpty() && node.defaultKind == Plan::Literal::None;
        literals.append(&node);
    }

    // A required member that every arm holds to strings no other arm allows.
    const UniquedStringImpl* discriminatorOf(const Plan::Node& node, Vector<std::pair<StringImpl*, unsigned>, 16>& tags) const
    {
        if (node.children.size() < 2)
            return nullptr;
        for (uint32_t arm : node.children) {
            if (nodeAt(arm).shape != Shape::Object || nodeAt(arm).defaultKind != Plan::Literal::None || m_analysis.isFunction[arm])
                return nullptr;
        }
        for (auto& candidate : nodeAt(node.children[0]).members) {
            if (parseIndex(*candidate.key.impl()))
                continue;
            tags.clear();
            bool holds = true;
            for (unsigned i = 0; i < node.children.size() && holds; ++i) {
                const Plan::Member* found = nullptr;
                for (auto& member : nodeAt(node.children[i]).members) {
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
        SlotScope scope(a);
        Target done = a.newLabel();
        Target misfit = fail;

        Vector<const Plan::Node*, 8> literals;
        bool areAllStrings = true;
        for (uint32_t arm : node.children)
            appendLiterals(arm, literals, areAllStrings);
        if (areAllStrings && literals.size() > 1) {
            Target missed = a.newLabel();
            a.beginStringSwitch(value);
            a.bind(missed);
            a.jump(misfit);
            a.bind(done);
            Vector<Writer::StringCase, 16> cases;
            for (auto* literal : literals)
                cases.append({ literal->string.impl(), done });
            a.endStringSwitch(cases.span(), missed);
            return;
        }

        Vector<std::pair<StringImpl*, unsigned>, 16> tags;
        if (auto* key = discriminatorOf(node, tags)) {
            bool readsByKey = false;
            for (uint32_t arm : node.children)
                readsByKey |= this->readsByKey(nodeAt(arm));
            emitObjectGuard(readsByKey, value, misfit);
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
                emitObject(nodeAt(arm), value, fail, isSoft, key);
                emitRulesOfAnyShape(nodeAt(arm), value, fail, isSoft);
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
                emitUse(arm, value, fail, isSoft);
                break;
            }
            SlotScope armScope(a);
            Target next = a.newLabel();
            Slot attempt = value;
            if (m_analysis.appliesDefault[arm] || (copies() && m_analysis.describesContainers[arm])) {
                attempt = a.newRegister();
                a.move(attempt, value);
            }
            Slot visitsBefore = nullptr;
            if (m_analysis.reachesFunction[arm]) {
                visitsBefore = a.newRegister();
                readVisits(visitsBefore);
            }
            emitUse(arm, attempt, next, true);
            if (attempt != value)
                a.move(value, attempt);
            a.jump(done);
            a.bind(next);
            if (visitsBefore)
                emitAfterFailedArm(visitsBefore);
        }
        if (node.children.isEmpty())
            a.jump(misfit);
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
    Slot m_mark;
    Slot m_failure;
    Slot m_state { nullptr };
    Slot m_visits { nullptr };
    Slot m_holes { nullptr };
    Target m_refuse { 0 };
    Target m_tooMany { 0 };
    Vector<Function<void()>> m_deferred;
    JSValue m_plainObjectFor;
    JSValue m_hasPlainElements;
    JSValue m_plainArrayLength;
    JSValue m_plainArrayFor;
    JSValue m_holesOf;
    JSValue m_copyPlainObject;
    JSValue m_copyPlainArray;
    JSValue m_hasOnlyTheseKeys;
    JSValue m_hasOwnKey;
    JSValue m_ownValue;
    JSValue m_matchesPattern;
    JSValue m_matchesFormat;
    JSValue m_isUrl;
    JSValue m_matchesTemplate;
    JSValue m_codePointLength;
    JSValue m_objectKeys;
    JSValue m_parseText;
    JSValue m_startRemembering;
    JSValue m_recall;
    JSValue m_remember;
    JSValue m_throwTooManyVisits;
    JSValue m_hasUniqueItems;
    JSValue m_isMultipleInDecimals;
    JSValue m_saysOnlyJSON;
};

class FunctionOfNodesBody final : public GeneratedFunctionBody {
public:
    FunctionOfNodesBody(Ref<Shared>&& shared, Mode mode)
        : m_shared(WTF::move(shared))
        , m_mode(mode)
    {
    }

    void emit(Writer& assembler) final { Emitter(assembler, m_shared.get(), functions, m_mode).emitFunctionOfNodes(); }

    // Each of the functions holds all of them as constants, or under a name of its own until it is written.
    Functions functions;

private:
    const Ref<Shared> m_shared;
    const Mode m_mode;
};

class ValidatorBody final : public GeneratedFunctionBody {
public:
    explicit ValidatorBody(Ref<Shared>&& shared, JSObject* onRefused = nullptr, JSObject* onNotLookedAt = nullptr, JSObject* onOtherArguments = nullptr)
        : m_shared(WTF::move(shared))
        , m_onRefused(onRefused)
        , m_onNotLookedAt(onNotLookedAt)
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
            for (unsigned i = 0; i < LengthOfState; ++i)
                functions.state->putDirectIndex(globalObject, i, i == MemoryInState || i == LookedThroughInState ? jsUndefined() : jsNumber(0));
            auto checks = adoptRef(*new FunctionOfNodesBody(m_shared.copyRef(), Mode::Check));
            functions.checks = createGeneratedFunction(vm, globalObject, "check"_s, 2, checks.copyRef());
            RefPtr<FunctionOfNodesBody> copies;
            if (mode == Mode::Copy) {
                copies = adoptRef(*new FunctionOfNodesBody(m_shared.copyRef(), Mode::Copy));
                functions.copies = createGeneratedFunction(vm, globalObject, "validate"_s, 2, *copies);
            }
            JSArray* kept = constructEmptyArray(globalObject, nullptr);
            kept->putDirectIndex(globalObject, 0, functions.state);
            kept->putDirectIndex(globalObject, 1, functions.checks);
            if (functions.copies)
                kept->putDirectIndex(globalObject, 2, functions.copies);
            functions.checks->putDirect(vm, nameOf(keptAliveName), kept);
            checks->functions = functions;
            if (copies) {
                functions.copies->putDirect(vm, nameOf(keptAliveName), kept);
                copies->functions = functions;
            }
        }
        Emitter emitter(assembler, m_shared.get(), functions, mode);
        if (functions.checks)
            assembler.constant(functions.checks);
        switch (m_shared->kind) {
        case ValidatorKind::Check:
            emitter.emitCheck();
            break;
        case ValidatorKind::Validate:
            emitter.emitValidate(m_onRefused, m_onNotLookedAt);
            break;
        case ValidatorKind::Parse:
            emitter.emitParse(m_onRefused, m_onNotLookedAt, m_onOtherArguments);
            break;
        }
    }

private:
    const Ref<Shared> m_shared;
    JSObject* const m_onRefused;
    JSObject* const m_onNotLookedAt;
    JSObject* const m_onOtherArguments;
};

// Says everything that a plan says, so two plans with one text are one plan.
class PlanText {
public:
    static String of(const Plan& plan)
    {
        PlanText text;
        text.number(plan.root);
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
                text.number(rule.most);
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

AOT_THRESHOLD(unsigned, mostRememberedPlans, 256);

// The code of a plan, for the next validator with the same plan: what the code cache does for two functions with one text.
static JSMap* rememberedCode(VM& vm, JSGlobalObject* globalObject)
{
    if (JSValue found = globalObject->getDirect(vm, nameOf(rememberedCodeName)))
        return uncheckedDowncast<JSMap>(found.asCell());
    JSMap* map = JSMap::create(vm, globalObject->mapStructure());
    globalObject->putDirect(vm, nameOf(rememberedCodeName), map, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    return map;
}

static RefPtr<Shared> analyze(Ref<Plan>&& plan, JSGlobalObject* globalObject, ValidatorKind kind)
{
#if ENABLE(AOT)
    // A program that was compiled ahead of time runs no code that was made later.
    if (globalObject->aotInstance())
        return nullptr;
#endif
    auto shared = adoptRef(*new Shared(WTF::move(plan), globalObject, kind));
    Analyzer(shared->plan.get(), shared->analysis.get()).run();
    if (!shared->analysis->isSupported)
        return nullptr;
    return shared;
}

static JSFunction* createWithHandlers(VM& vm, JSGlobalObject* globalObject, Ref<Plan>&& plan, ValidatorKind kind, ASCIILiteral name, unsigned parameterCount, JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments)
{
    auto shared = analyze(WTF::move(plan), globalObject, kind);
    if (!shared)
        return nullptr;
    JSFunction* function = createGeneratedFunction(vm, globalObject, name, parameterCount, adoptRef(*new ValidatorBody(shared.releaseNonNull(), onRefused, onNotLookedAt, onOtherArguments)));
    JSArray* kept = constructEmptyArray(globalObject, nullptr);
    kept->putDirectIndex(globalObject, 0, onRefused);
    kept->putDirectIndex(globalObject, 1, onNotLookedAt);
    if (onOtherArguments)
        kept->putDirectIndex(globalObject, 2, onOtherArguments);
    function->putDirect(vm, nameOf(keptAliveName), kept);
    return function;
}

// The text that AOTValidationPlanText.cpp writes.
class PlanReader {
public:
    explicit PlanReader(JSGlobalObject* globalObject)
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
        if (!count || count > mostNodes)
            return nullptr;
        m_count = count;
        m_plan->nodes.grow(count);
        for (unsigned i = 0; i < count && m_isRight; ++i)
            readNode(m_plan->nodes[i], elementOf(nodes, i));
        return m_isRight ? RefPtr<Plan>(m_plan.ptr()) : nullptr;
    }

private:
    AOT_CLASS_THRESHOLD(unsigned, mostNodes, 1 << 16);

    JSValue giveUp()
    {
        m_isRight = false;
        return { };
    }

    JSValue elementOf(JSArray* array, unsigned index) { return array->getDirectIndex(m_globalObject, index); }

    JSValue find(JSObject* object, ASCIILiteral name) { return object->getDirect(m_vm, Identifier::fromString(m_vm, name)); }

    JSObject* objectOf(JSValue value)
    {
        if (value && value.isObject() && !isJSArray(value))
            return asObject(value);
        giveUp();
        return nullptr;
    }

    String stringOf(JSValue value)
    {
        if (value && value.isString())
            return asString(value)->value(m_globalObject);
        giveUp();
        return String();
    }

    double numberOf(JSValue value)
    {
        if (value && value.isNumber())
            return value.asNumber();
        giveUp();
        return 0;
    }

    uint32_t countOf(JSValue value)
    {
        double number = numberOf(value);
        if (number >= 0 && number < Plan::none && number == std::trunc(number))
            return static_cast<uint32_t>(number);
        giveUp();
        return 0;
    }

    uint32_t nodeOf(JSValue value)
    {
        uint32_t node = countOf(value);
        if (node >= m_count)
            giveUp();
        return node;
    }

    bool booleanOf(JSValue value)
    {
        if (value && value.isBoolean())
            return value.asBoolean();
        giveUp();
        return false;
    }

    void forEachElement(JSValue list, NOESCAPE const Invocable<void(JSValue)> auto& visit)
    {
        if (!list || !isJSArray(list)) {
            giveUp();
            return;
        }
        JSArray* array = asArray(list);
        for (unsigned i = 0; i < array->length() && m_isRight; ++i)
            visit(elementOf(array, i));
    }

    Vector<uint32_t> nodesOf(JSValue list)
    {
        Vector<uint32_t> nodes;
        forEachElement(list, [&](JSValue one) {
            nodes.append(nodeOf(one));
        });
        return nodes;
    }

    Vector<String> stringsOf(JSValue list)
    {
        Vector<String> strings;
        forEachElement(list, [&](JSValue one) {
            strings.append(stringOf(one));
        });
        return strings;
    }

    void readDefault(Plan::Node& node, JSValue value)
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
            node.defaultString = stringOf(value);
        }
    }

    void readRule(Plan::Node& node, JSObject* object)
    {
        String name = stringOf(find(object, "rule"_s));
        Plan::Rule rule;
        auto is = [&](ASCIILiteral word, RuleKind kind) {
            if (name != word)
                return false;
            rule.kind = kind;
            return true;
        };
        if (is("Integer"_s, RuleKind::Integer)) {
        } else if (is("Minimum"_s, RuleKind::Minimum) || is("Maximum"_s, RuleKind::Maximum) || is("ExclusiveMinimum"_s, RuleKind::ExclusiveMinimum) || is("ExclusiveMaximum"_s, RuleKind::ExclusiveMaximum))
            rule.number = numberOf(find(object, "bound"_s));
        else if (is("MultipleOf"_s, RuleKind::MultipleOf))
            rule.number = numberOf(find(object, "divisor"_s));
        else if (is("MinimumLength"_s, RuleKind::MinimumLength) || is("MaximumLength"_s, RuleKind::MaximumLength))
            rule.count = countOf(find(object, "length"_s));
        else if (is("Pattern"_s, RuleKind::Pattern)) {
            rule.text = stringOf(find(object, "source"_s));
            if (JSValue isUnicode = find(object, "unicode"_s); isUnicode && booleanOf(isUnicode))
                rule.flags = OptionSet<Yarr::Flags> { Yarr::Flags::Unicode }.toRaw();
        } else if (is("Format"_s, RuleKind::Pattern)) {
            // An expression, or none for what URL.canParse() answers.
            rule.text = stringOf(find(object, "source"_s));
            if (rule.text.isEmpty())
                rule.kind = RuleKind::Url;
        } else if (is("Template"_s, RuleKind::Template)) {
            rule.texts = stringsOf(find(object, "texts"_s));
            rule.text = stringOf(find(object, "placeholders"_s));
        } else if (is("Not"_s, RuleKind::Not))
            rule.nodes.append(nodeOf(find(object, "node"_s)));
        else if (is("If"_s, RuleKind::If)) {
            rule.nodes.append(nodeOf(find(object, "condition"_s)));
            rule.nodes.append(nodeOf(find(object, "then"_s)));
            rule.nodes.append(nodeOf(find(object, "otherwise"_s)));
        } else if (is("ExactlyOne"_s, RuleKind::ExactlyOne) || is("Every"_s, RuleKind::Every))
            rule.nodes = nodesOf(find(object, "nodes"_s));
        else if (is("UniqueItems"_s, RuleKind::UniqueItems)) {
        } else if (is("Contains"_s, RuleKind::Contains)) {
            rule.nodes.append(nodeOf(find(object, "node"_s)));
            rule.count = countOf(find(object, "minimum"_s));
            if (JSValue most = find(object, "maximum"_s))
                rule.most = countOf(most);
        } else if (is("PatternMembers"_s, RuleKind::PatternMembers)) {
            // The patterns of a JSON Schema.
            rule.flags = OptionSet<Yarr::Flags> { Yarr::Flags::Unicode }.toRaw();
            rule.texts = stringsOf(find(object, "sources"_s));
            rule.nodes = nodesOf(find(object, "nodes"_s));
            for (auto& source : rule.texts) {
                if (m_isRight && !RegExp::create(m_vm, source, OptionSet<Yarr::Flags>::fromRaw(rule.flags))->isValid())
                    giveUp();
            }
        } else if (is("MemberNames"_s, RuleKind::MemberNames))
            rule.nodes.append(nodeOf(find(object, "node"_s)));
        else if (is("MinimumMembers"_s, RuleKind::MinimumMembers) || is("MaximumMembers"_s, RuleKind::MaximumMembers))
            rule.count = countOf(find(object, "count"_s));
        else if (is("DependentMembers"_s, RuleKind::DependentMembers)) {
            rule.text = stringOf(find(object, "key"_s));
            rule.texts = stringsOf(find(object, "names"_s));
        } else if (is("DependentNode"_s, RuleKind::DependentNode)) {
            rule.text = stringOf(find(object, "key"_s));
            rule.nodes.append(nodeOf(find(object, "node"_s)));
        } else {
            giveUp();
            return;
        }
        if (rule.kind == RuleKind::Pattern && m_isRight && !RegExp::create(m_vm, rule.text, OptionSet<Yarr::Flags>::fromRaw(rule.flags))->isValid())
            giveUp();
        node.rules.append(WTF::move(rule));
    }

    void readNode(Plan::Node& node, JSValue value)
    {
        JSObject* object = objectOf(value);
        if (!object)
            return;
        String shape = stringOf(find(object, "shape"_s));
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
            String primitive = stringOf(find(object, "primitive"_s));
            if (primitive == "string"_s)
                node.shape = Shape::String;
            else if (primitive == "number"_s)
                node.shape = Shape::Number;
            else if (primitive == "boolean"_s)
                node.shape = Shape::Boolean;
            else if (primitive == "bigint"_s)
                node.shape = Shape::BigInt;
            else
                giveUp();
        } else if (shape == "BooleanLiteral"_s) {
            node.shape = Shape::BooleanLiteral;
            node.boolean = booleanOf(find(object, "value"_s));
        } else if (shape == "NumberLiteral"_s) {
            // A number that JSON cannot say is written as null, and equals nothing.
            JSValue number = find(object, "value"_s);
            node.shape = Shape::NumberLiteral;
            node.number = number && number.isNull() ? PNaN : numberOf(number);
        } else if (shape == "StringLiteral"_s) {
            node.shape = Shape::StringLiteral;
            node.string = stringOf(find(object, "value"_s));
        } else if (shape == "Array"_s || shape == "Dictionary"_s) {
            node.shape = shape == "Array"_s ? Shape::Array : Shape::Dictionary;
            node.child = nodeOf(find(object, "element"_s));
        } else if (shape == "Tuple"_s) {
            node.shape = Shape::Tuple;
            if (JSValue elements = find(object, "elements"_s))
                node.children = nodesOf(elements);
            JSValue required = find(object, "required"_s);
            node.required = required ? countOf(required) : node.children.size();
            if (JSValue rest = find(object, "rest"_s))
                node.child = nodeOf(rest);
        } else if (shape == "Object"_s) {
            node.shape = Shape::Object;
            if (JSValue members = find(object, "members"_s)) {
                forEachElement(members, [&](JSValue one) {
                    JSObject* member = objectOf(one);
                    if (!member)
                        return;
                    String key = stringOf(find(member, "key"_s));
                    if (!m_isRight)
                        return;
                    node.members.append(Plan::Member { AtomString(key), nodeOf(find(member, "node"_s)), booleanOf(find(member, "isOptional"_s)), booleanOf(find(member, "mustBeOwn"_s)) });
                });
            }
            String undeclared = stringOf(find(object, "undeclared"_s));
            if (undeclared == "Allowed"_s)
                node.undeclared = Plan::Undeclared::Allowed;
            else if (undeclared == "Rejected"_s)
                node.undeclared = Plan::Undeclared::Rejected;
            else if (undeclared == "Validated"_s) {
                node.undeclared = Plan::Undeclared::Validated;
                node.child = nodeOf(find(object, "undeclaredValue"_s));
            } else
                giveUp();
        } else if (shape == "Union"_s) {
            node.shape = Shape::Union;
            if (JSValue arms = find(object, "arms"_s))
                node.children = nodesOf(arms);
        } else
            giveUp();

        if (JSValue rules = find(object, "rules"_s)) {
            forEachElement(rules, [&](JSValue one) {
                if (JSObject* rule = objectOf(one))
                    readRule(node, rule);
            });
        }
        if (JSValue standIn = find(object, "default"_s))
            readDefault(node, standIn);
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Ref<Plan> m_plan;
    unsigned m_count { 0 };
    bool m_isRight { true };
};

} // namespace

} // namespace ValidatorBytecode

namespace {

// digits x 10^exponent
struct Decimals {
    uint64_t digits { 0 };
    int exponent { 0 };
};

Decimals decimalsOf(double positive)
{
    using Converter = WTF::double_conversion::DoubleToStringConverter;
    std::array<char, Converter::kBase10MaximalLength + 1> buffer;
    bool isNegative = false;
    int length = 0;
    int point = 0;
    Converter::DoubleToAscii(positive, Converter::SHORTEST, 0, std::span<char> { buffer }, isNegative, length, point);
    Decimals result;
    for (int i = 0; i < length; ++i)
        result.digits = result.digits * 10 + (buffer[i] - '0');
    result.exponent = point - length;
    return result;
}

} // namespace

bool isMultipleInDecimals(double number, double divisor)
{
    using Wide = UInt128;
    if (!std::isfinite(number) || !std::isfinite(divisor) || !(divisor > 0))
        return false;
    if (!number)
        return true;
    Decimals dividend = decimalsOf(std::abs(number));
    Decimals unit = decimalsOf(divisor);
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
    // The digits of a double are below 10^17.
    AOT_THRESHOLD(int, mostDigits, 17);
    if (-shift > mostDigits)
        return false;
    Wide whole = unit.digits;
    for (int i = 0; i < -shift; ++i)
        whole *= 10;
    return !(dividend.digits % whole);
}

ValidationLook lookThroughForJSON(JSGlobalObject* globalObject, JSValue value)
{
    return ValidatorBytecode::JSONLook(globalObject, nullptr).look(value);
}

void setValidationFormatChecker(ValidationFormatChecker checker) { ValidatorBytecode::formatChecker = checker; }

RefPtr<ValidationPlan> readValidationPlan(JSGlobalObject* globalObject, const String& text)
{
    return ValidatorBytecode::PlanReader(globalObject).read(text);
}

JSFunction* createCheckFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan)
{
    using namespace ValidatorBytecode;
#if ENABLE(AOT)
    if (globalObject->aotInstance())
        return nullptr;
#endif
    JSMap* remembered = rememberedCode(vm, globalObject);
    JSString* text = jsString(vm, PlanText::of(plan.get()));
    JSValue code = remembered->get(globalObject, text);
    if (code.isCell())
        return JSFunction::create(vm, globalObject, uncheckedDowncast<FunctionExecutable>(code.asCell()), globalObject);

    auto shared = analyze(WTF::move(plan), globalObject, ValidatorKind::Check);
    if (!shared)
        return nullptr;
    JSFunction* function = createGeneratedFunction(vm, globalObject, "check"_s, 1, adoptRef(*new ValidatorBody(shared.releaseNonNull())));
    prepareGeneratedFunction(vm, function);
    if (remembered->size() >= mostRememberedPlans)
        remembered->clear(globalObject);
    remembered->set(globalObject, text, function->jsExecutable());
    return function;
}

JSFunction* createValidateFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan, JSObject* onRefused, JSObject* onNotLookedAt)
{
    return ValidatorBytecode::createWithHandlers(vm, globalObject, WTF::move(plan), ValidatorKind::Validate, "validate"_s, 1, onRefused, onNotLookedAt, nullptr);
}

JSFunction* createParseFunction(VM& vm, JSGlobalObject* globalObject, Ref<ValidationPlan>&& plan, JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments)
{
    return ValidatorBytecode::createWithHandlers(vm, globalObject, WTF::move(plan), ValidatorKind::Parse, "parse"_s, 2, onRefused, onNotLookedAt, onOtherArguments);
}

JSFunction* createValidatorOfPlanText(JSGlobalObject* globalObject, ValidatorKind kind, const String& planText, JSObject* onRefused, JSObject* onNotLookedAt, JSObject* onOtherArguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RefPtr<ValidationPlan> plan = readValidationPlan(globalObject, planText);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!plan) {
        throwTypeError(globalObject, scope, "The text is no validation plan"_s);
        return nullptr;
    }
    switch (kind) {
    case ValidatorKind::Check:
        RELEASE_AND_RETURN(scope, createCheckFunction(vm, globalObject, plan.releaseNonNull()));
    case ValidatorKind::Validate:
        RELEASE_AND_RETURN(scope, createValidateFunction(vm, globalObject, plan.releaseNonNull(), onRefused, onNotLookedAt));
    case ValidatorKind::Parse:
        RELEASE_AND_RETURN(scope, createParseFunction(vm, globalObject, plan.releaseNonNull(), onRefused, onNotLookedAt, onOtherArguments));
    }
    return nullptr;
}

} // namespace JSC
