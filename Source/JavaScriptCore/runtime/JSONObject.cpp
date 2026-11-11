/*
 * Copyright (C) 2009-2024 Apple Inc. All rights reserved.
 * Copyright (C) 2020 Alexey Shvayka <shvaikalesh@gmail.com>.
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
#include "JSONObject.h"

#include "ArrayConstructor.h"
#include "BigIntObject.h"
#include "BooleanObject.h"
#include "GetterSetter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSRawJSONObject.h"
#include "LiteralParser.h"
#include "NumberObject.h"
#include "ObjectConstructorInlines.h"
#include "PropertyNameArray.h"
#include "VMInlines.h"
#include <charconv>
#include <wtf/MathExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/dtoa.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/ParsingUtilities.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringBuilderJSON.h>
#include <wtf/text/StringCommon.h>

#if ENABLE(AOT)
#include "AOTJSONHooks.h"
#include "AOTJSONTextWriter.h"
#include "AOTJSONHooks.h"
#include "AOTPropertyListCache.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeReporter.h"
#include "AOTWatchedJSONLevels.h"
#include "DateInstance.h"
#include "DateTimeWriter.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include <variant>
#include <wtf/HashSet.h>
#endif

// Turn this on to log information about fastStringify usage, with a focus on why it failed.
#define FAST_STRINGIFY_LOG_USAGE 0

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSONObject);

static JSC_DECLARE_HOST_FUNCTION(jsonProtoFuncParse);
JSC_DECLARE_HOST_FUNCTION(jsonProtoFuncStringify);
static JSC_DECLARE_HOST_FUNCTION(jsonProtoFuncIsRawJSON);
static JSC_DECLARE_HOST_FUNCTION(jsonProtoFuncRawJSON);

}

#include "JSONObject.lut.h"

namespace JSC {

JSONObject::JSONObject(VM& vm, Structure* structure)
    : JSNonFinalObject(vm, structure)
{
}

void JSONObject::finishCreation(VM& vm, JSGlobalObject* globalObject)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    JSC_TO_STRING_TAG_WITHOUT_TRANSITION();
    if (Options::useJSONSourceTextAccess()) {
        JSC_NATIVE_FUNCTION_WITHOUT_TRANSITION(vm.propertyNames->isRawJSON, jsonProtoFuncIsRawJSON, static_cast<unsigned>(PropertyAttribute::DontEnum), 1, ImplementationVisibility::Public);
        JSC_NATIVE_FUNCTION_WITHOUT_TRANSITION(vm.propertyNames->rawJSON, jsonProtoFuncRawJSON, static_cast<unsigned>(PropertyAttribute::DontEnum), 1, ImplementationVisibility::Public);
    }
}

// PropertyNameForFunctionCall objects must be on the stack, since the JSValue that they create is not marked.
class PropertyNameForFunctionCall {
public:
    PropertyNameForFunctionCall(PropertyName);
    PropertyNameForFunctionCall(unsigned);

    JSValue value(VM&) const;

private:
    PropertyName m_propertyName;
    unsigned m_number;
    mutable JSValue m_value;
};

class Stringifier {
    WTF_MAKE_NONCOPYABLE(Stringifier);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    static String stringify(JSGlobalObject&, JSValue, JSValue replacer, JSValue space);

private:
    class Holder {
    public:
        enum RootHolderTag { RootHolder };
        Holder(JSGlobalObject*, JSObject*, Structure*);
        Holder(RootHolderTag, JSObject*);

        NODELETE JSObject* object() const { return m_object; }
        NODELETE bool isArray() const { return m_isArray; }
        NODELETE bool hasFastObjectProperties() const { return m_hasFastObjectProperties; }

        bool appendNextProperty(Stringifier&, StringBuilder&);

    private:
        JSObject* m_object { nullptr };
        Structure* m_structure { nullptr };
        const bool m_isJSArray { false };
        const bool m_isArray { false };
        bool m_hasFastObjectProperties { false };
        unsigned m_index { 0 };
        unsigned m_size { 0 };
        RefPtr<PropertyNameArray> m_propertyNames;
        Vector<std::tuple<PropertyName, unsigned>, 8> m_propertiesAndOffsets;
#if ENABLE(AOT)
        // A compiled object's own properties at the start of the walk, if they were all fields. The list lives as long as the program.
        std::span<const AOT::FieldAsProperty> m_initialFields;
#endif
    };

    friend class Holder;

    Stringifier(JSGlobalObject*, JSValue replacer, JSValue space);

    JSValue toJSON(JSValue, const PropertyNameForFunctionCall&);

    enum StringifyResult { StringifyFailed, StringifySucceeded, StringifyFailedDueToUndefinedOrSymbolValue };
    StringifyResult appendStringifiedValue(StringBuilder&, JSValue, const Holder&, const PropertyNameForFunctionCall&);

    bool willIndent() const;
    void indent();
    void unindent();
    void startNewLine(StringBuilder&) const;
    NODELETE bool isCallableReplacer() const { return m_replacerCallData.type != CallData::Type::None; }

    JSGlobalObject* const m_globalObject;
    JSValue m_replacer;
    bool m_usingArrayReplacer { false };
    PropertyNameArrayBuilder m_arrayReplacerPropertyNames;
    CallData m_replacerCallData;
    String m_gap;

    MarkedArgumentBufferWithSize<16> m_objectStack;
    Vector<Holder, 16, UnsafeVectorOverflow> m_holderStack;
    String m_repeatedGap;
    StringView m_indent;
};

// ------------------------------ helper functions --------------------------------

static inline JSValue unwrapBoxedPrimitive(JSGlobalObject* globalObject, JSObject* object)
{
    if (object->inherits<NumberObject>())
        return jsNumber(object->toNumber(globalObject));
    if (object->inherits<StringObject>())
        return object->toString(globalObject);
    if (object->inherits<BooleanObject>() || object->inherits<BigIntObject>())
        return uncheckedDowncast<JSWrapperObject>(object)->internalValue();

    // Do not unwrap SymbolObject to Symbol. It is not performed in the spec.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-serializejsonproperty

    return object;
}

static inline JSValue unwrapBoxedPrimitive(JSGlobalObject* globalObject, JSValue value)
{
    return value.isObject() ? unwrapBoxedPrimitive(globalObject, asObject(value)) : value;
}

static constexpr unsigned maxGapLength = 10;

static inline String gap(JSGlobalObject* globalObject, JSValue space)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    space = unwrapBoxedPrimitive(globalObject, space);
    RETURN_IF_EXCEPTION(scope, { });

    // If the space value is a number, create a gap string with that number of spaces.
    if (space.isNumber()) {
        unsigned count = clampTo<unsigned>(space.asNumber(), 0, maxGapLength);
        constexpr auto spaces = "          "_s;
        static_assert(spaces.length() == maxGapLength);
        return String(spaces.span8().first(count));
    }

    // If the space value is a string, use it as the gap string, otherwise use no gap string.
    String spaces = space.getString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (spaces.length() <= maxGapLength)
        return spaces;
    return spaces.substringSharingImpl(0, maxGapLength);
}

// ------------------------------ PropertyNameForFunctionCall --------------------------------

inline PropertyNameForFunctionCall::PropertyNameForFunctionCall(PropertyName propertyName)
    : m_propertyName(propertyName)
{
}

inline PropertyNameForFunctionCall::PropertyNameForFunctionCall(unsigned number)
    : m_number(number)
{
}

JSValue PropertyNameForFunctionCall::value(VM& vm) const
{
    if (!m_value) {
        if (!m_propertyName.isNull())
            m_value = jsString(vm, String { m_propertyName.uid() });
        else {
            if (m_number <= 9)
                return vm.smallStrings.singleCharacterString(m_number + '0');
            m_value = jsNontrivialString(vm, vm.numericStrings.add(m_number));
        }
    }
    return m_value;
}

// ------------------------------ Stringifier --------------------------------

Stringifier::Stringifier(JSGlobalObject* globalObject, JSValue replacer, JSValue space)
    : m_globalObject(globalObject)
    , m_replacer(replacer)
    , m_arrayReplacerPropertyNames(globalObject->vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (m_replacer.isObject()) {
        JSObject* replacerObject = asObject(m_replacer);

        m_replacerCallData = JSC::getCallData(replacerObject);
        if (m_replacerCallData.type == CallData::Type::None) {
            bool isArrayReplacer = JSC::isArray(globalObject, replacerObject);
            RETURN_IF_EXCEPTION(scope, );
            if (isArrayReplacer) {
                m_usingArrayReplacer = true;
                forEachInArrayLike(globalObject, replacerObject, [&] (JSValue name) -> bool {
                    if (name.isObject()) {
                        auto* nameObject = uncheckedDowncast<JSObject>(name);
                        if (!nameObject->inherits<NumberObject>() && !nameObject->inherits<StringObject>())
                            return true;
                    } else if (!name.isNumber() && !name.isString())
                        return true;

                    JSString* propertyNameString = name.toString(globalObject);
                    RETURN_IF_EXCEPTION(scope, false);
                    auto propertyName = propertyNameString->toIdentifier(globalObject);
                    RETURN_IF_EXCEPTION(scope, false);
                    m_arrayReplacerPropertyNames.add(WTF::move(propertyName));
                    return true;
                });
                RETURN_IF_EXCEPTION(scope, );
            }
        }
    }

    scope.release();
    m_gap = gap(globalObject, space);
}

String Stringifier::stringify(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space)
{
    VM& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Stringifier stringifier(&globalObject, replacer, space);
    RETURN_IF_EXCEPTION(scope, { });

    PropertyNameForFunctionCall emptyPropertyName(vm.propertyNames->emptyIdentifier.impl());

    // If the replacer is not callable, root object wrapper is non-user-observable.
    // We can skip creating this wrapper object.
    JSObject* object = nullptr;
    if (stringifier.isCallableReplacer()) {
        object = constructEmptyObject(&globalObject);
        object->putDirect(vm, vm.propertyNames->emptyIdentifier, value);
    }

    StringBuilder result(OverflowPolicy::RecordOverflow);
    Holder root(Holder::RootHolder, object);
    auto stringifyResult = stringifier.appendStringifiedValue(result, value, root, emptyPropertyName);
    RETURN_IF_EXCEPTION(scope, { });
    if (result.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(&globalObject, scope);
        return { };
    }
    if (stringifyResult != StringifySucceeded) [[unlikely]]
        RELEASE_AND_RETURN(scope, { });
    RELEASE_AND_RETURN(scope, WTF::move(result).toString());
}

ALWAYS_INLINE JSValue Stringifier::toJSON(JSValue baseValue, const PropertyNameForFunctionCall& propertyName)
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    scope.assertNoException();

    JSValue toJSONFunction;
    if (baseValue.isObject())
        toJSONFunction = asObject(baseValue)->structure()->cachedSpecialProperty(CachedSpecialPropertyKey::ToJSON);

    if (!toJSONFunction) {
        PropertySlot slot(baseValue, PropertySlot::InternalMethodType::Get);
        bool hasProperty = baseValue.getPropertySlot(m_globalObject, vm.propertyNames->toJSON, slot);
        RETURN_IF_EXCEPTION(scope, { });
        toJSONFunction = hasProperty ? slot.getValue(m_globalObject, vm.propertyNames->toJSON) : jsUndefined();
        RETURN_IF_EXCEPTION(scope, { });

        if (baseValue.isObject())
            asObject(baseValue)->structure()->cacheSpecialProperty(m_globalObject, vm, toJSONFunction, CachedSpecialPropertyKey::ToJSON, slot);
    }

    auto callData = JSC::getCallData(toJSONFunction);
    if (callData.type == CallData::Type::None)
        return baseValue;

    auto args = WTF::toArray<EncodedJSValue>({
        JSValue::encode(propertyName.value(vm)),
    });
    RELEASE_AND_RETURN(scope, call(m_globalObject, asObject(toJSONFunction), callData, baseValue, ArgList { args.data(), args.size() }));
}

// We clamp recursion well beyond anything reasonable.
constexpr unsigned maximumSideStackRecursion = 40000;
Stringifier::StringifyResult Stringifier::appendStringifiedValue(StringBuilder& builder, JSValue value, const Holder& holder, const PropertyNameForFunctionCall& propertyName)
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Recursion is avoided by !holderStackWasEmpty check and do/while loop at the end of this method.
    // We're having this recursion check here as a fail safe in case the code
    // below get modified such that recursion is no longer avoided.
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(m_globalObject, scope);
        return StringifyFailed;
    }

    // Call the toJSON function.
    if (value.isObject() || value.isBigInt()) {
        value = toJSON(value, propertyName);
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
    }

    // Call the replacer function.
    if (isCallableReplacer()) {
        auto args = WTF::toArray<EncodedJSValue>({
            JSValue::encode(propertyName.value(vm)),
            JSValue::encode(value),
        });
        ASSERT(holder.object());
        value = call(m_globalObject, m_replacer, m_replacerCallData, holder.object(), ArgList { args.data(), args.size() });
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
    }

    if ((value.isUndefined() || value.isSymbol()) && !holder.isArray())
        return StringifyFailedDueToUndefinedOrSymbolValue;

    if (value.isObject()) {
        JSObject* object = asObject(value);
        if (object->inherits<JSRawJSONObject>()) {
            String string = uncheckedDowncast<JSRawJSONObject>(object)->rawJSON(vm)->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, StringifyFailed);
            builder.append(WTF::move(string));
            return StringifySucceeded;

        }
        value = unwrapBoxedPrimitive(m_globalObject, object);
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
    }

    if (value.isNull()) {
        builder.append("null"_s);
        return StringifySucceeded;
    }

    if (value.isBoolean()) {
        if (value.isTrue())
            builder.append("true"_s);
        else
            builder.append("false"_s);
        return StringifySucceeded;
    }

    if (value.isString()) {
        auto string = asString(value)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
        builder.appendQuotedJSONString(string);
        return StringifySucceeded;
    }

    if (value.isNumber()) {
        if (value.isInt32())
            builder.append(value.asInt32());
        else {
            double number = value.asNumber();
            if (!std::isfinite(number))
                builder.append("null"_s);
#if USE(BUN_JSC_ADDITIONS)
            else if (auto int32Value = tryConvertToStrictInt32(number))
                builder.append(*int32Value);
#endif
            else
                builder.append(number);
        }
        return StringifySucceeded;
    }

    if (value.isBigInt()) {
        throwTypeError(m_globalObject, scope, "JSON.stringify cannot serialize BigInt."_s);
        return StringifyFailed;
    }

    if (!value.isObject())
        return StringifyFailed;

    JSObject* object = asObject(value);
    if (object->isCallable()) {
        if (holder.isArray()) {
            builder.append("null"_s);
            return StringifySucceeded;
        }
        return StringifyFailedDueToUndefinedOrSymbolValue;
    }

    if (builder.hasOverflowed()) [[unlikely]]
        return StringifyFailed;

    // Handle cycle detection, and put the holder on the stack.
    for (unsigned i = 0; i < m_holderStack.size(); i++) {
        if (m_holderStack[i].object() == object) {
            throwTypeError(m_globalObject, scope, "JSON.stringify cannot serialize cyclic structures."_s);
            return StringifyFailed;
        }
    }

    if (m_holderStack.size() >= maximumSideStackRecursion) [[unlikely]] {
        throwStackOverflowError(m_globalObject, scope);
        return StringifyFailed;
    }

    bool holderStackWasEmpty = m_holderStack.isEmpty();
    Structure* structure = object->structure();
    m_holderStack.append(Holder(m_globalObject, object, structure));
    m_objectStack.appendWithCrashOnOverflow(object);
    m_objectStack.appendWithCrashOnOverflow(structure);
    RETURN_IF_EXCEPTION(scope, StringifyFailed);
    if (!holderStackWasEmpty)
        return StringifySucceeded;

    do {
        while (m_holderStack.last().appendNextProperty(*this, builder))
            RETURN_IF_EXCEPTION(scope, StringifyFailed);
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
        if (builder.hasOverflowed()) [[unlikely]]
            return StringifyFailed;
        m_holderStack.removeLast();
        m_objectStack.removeLast();
        m_objectStack.removeLast();
    } while (!m_holderStack.isEmpty());
    return StringifySucceeded;
}

NODELETE inline bool Stringifier::willIndent() const
{
    return !m_gap.isEmpty();
}

inline void Stringifier::indent()
{
    // Use a single shared string, m_repeatedGap, so we don't keep allocating new ones as we indent and unindent.
    unsigned newSize = m_indent.length() + m_gap.length();
    if (newSize > m_repeatedGap.length())
        m_repeatedGap = makeString(m_repeatedGap, m_gap);
    ASSERT(newSize <= m_repeatedGap.length());
    m_indent = StringView { m_repeatedGap }.left(newSize);
}

NODELETE inline void Stringifier::unindent()
{
    ASSERT(m_indent.length() >= m_gap.length());
    m_indent = StringView { m_repeatedGap }.left(m_indent.length() - m_gap.length());
}

inline void Stringifier::startNewLine(StringBuilder& builder) const
{
    if (willIndent())
        builder.append('\n', m_indent);
}

#if ENABLE(AOT)
// The fields of a compiled object that has no other own properties, if each field has its key precomputed in JSON form.
static std::optional<std::span<const AOT::FieldAsProperty>> fieldsWithJSONKeys(JSObject* object)
{
    if (object->type() != CompiledObjectType)
        return std::nullopt;
    auto fields = AOT::PropertyWalk::fieldsInKeyOrder(object);
    if (!fields || fields->empty())
        return std::nullopt;
    for (auto& field : *fields) {
        if (field.jsonKey.empty())
            return std::nullopt;
    }
    return fields;
}
#endif

inline Stringifier::Holder::Holder(JSGlobalObject* globalObject, JSObject* object, Structure* structure)
    : m_object(object)
    , m_structure(structure)
    , m_isJSArray(isJSArray(object))
    , m_isArray(JSC::isArray(globalObject, object))
{
}

inline Stringifier::Holder::Holder(RootHolderTag, JSObject* object)
    : m_object(object)
{
}

bool Stringifier::Holder::appendNextProperty(Stringifier& stringifier, StringBuilder& builder)
{
    ASSERT(m_index <= m_size);

    JSGlobalObject* globalObject = stringifier.m_globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // First time through, initialize.
    if (!m_index) {
        if (m_isArray) {
            uint64_t length = toLength(globalObject, m_object);
            RETURN_IF_EXCEPTION(scope, false);
            if (length > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return false;
            }
            m_size = static_cast<uint32_t>(length);
            RETURN_IF_EXCEPTION(scope, false);
            builder.append('[');
        } else {
            if (stringifier.m_usingArrayReplacer) {
                m_propertyNames = stringifier.m_arrayReplacerPropertyNames.data();
                m_size = m_propertyNames->propertyNameVector().size();
            } else if (m_object->structure() == m_structure && canPerformFastPropertyNameEnumerationForJSONStringifyWithSideEffect(m_structure)) {
                m_hasFastObjectProperties = m_structure->canPerformFastPropertyEnumeration();
                m_structure->forEachProperty(vm, [&](const auto& entry) -> bool {
                    if (entry.attributes() & PropertyAttribute::DontEnum)
                        return true;

                    PropertyName propertyName(entry.key());
                    if (propertyName.isSymbol())
                        return true;
                    m_propertiesAndOffsets.constructAndAppend(propertyName, entry.offset());
                    return true;
                });
                m_size = m_propertiesAndOffsets.size();
#if ENABLE(AOT)
            } else if (auto fields = fieldsWithJSONKeys(m_object)) {
                m_initialFields = *fields;
                m_size = m_initialFields.size();
#endif
            } else {
                PropertyNameArrayBuilder objectPropertyNames(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                m_object->methodTable()->getOwnPropertyNames(m_object, globalObject, objectPropertyNames, DontEnumPropertiesMode::Exclude);
                RETURN_IF_EXCEPTION(scope, false);
                m_propertyNames = objectPropertyNames.releaseData();
                m_size = m_propertyNames->propertyNameVector().size();
            }

            builder.append('{');
        }
        stringifier.indent();
    }
    if (builder.hasOverflowed()) [[unlikely]]
        return false;

    // Last time through, finish up and return false.
    if (m_index == m_size) {
        stringifier.unindent();
        if (m_size && builder[builder.length() - 1] != '{')
            stringifier.startNewLine(builder);
        builder.append(m_isArray ? ']' : '}');
        return false;
    }

    // Handle a single element of the array or object.
    unsigned index = m_index++;
    unsigned rollBackPoint = 0;
    StringifyResult stringifyResult;
    if (m_isArray) {
        // Get the value.
        JSValue value;
        if (m_isJSArray && m_object->canGetIndexQuickly(index))
            value = m_object->getIndexQuickly(index);
        else {
            value = m_object->get(globalObject, index);
            RETURN_IF_EXCEPTION(scope, false);
        }

        // Append the separator string.
        if (index)
            builder.append(',');
        stringifier.startNewLine(builder);

        // Append the stringified value.
        stringifyResult = stringifier.appendStringifiedValue(builder, value, *this, index);
        ASSERT(stringifyResult != StringifyFailedDueToUndefinedOrSymbolValue);
    } else {
        PropertyName propertyName;
        JSValue value;
#if ENABLE(AOT)
        std::span<const Latin1Character> keyAsWritten;
        if (!m_initialFields.empty()) {
            const AOT::FieldAsProperty& field = m_initialFields[index];
            propertyName = field.name;
            keyAsWritten = field.jsonKey.subspan(1);
            // Code that ran for an earlier member may have changed the object.
            auto currentFields = AOT::PropertyWalk::fieldsInKeyOrder(m_object);
            if (currentFields && currentFields->data() == m_initialFields.data()) [[likely]]
                value = AOT::PropertyWalk::readListedField(m_object, field);
            if (!value) [[unlikely]] {
                value = m_object->get(globalObject, propertyName);
                RETURN_IF_EXCEPTION(scope, false);
            }
        } else
#endif
        if (m_hasFastObjectProperties) {
            propertyName = std::get<0>(m_propertiesAndOffsets[index]);
            if (m_object->structureID() == m_structure->id()) {
                unsigned offset = std::get<1>(m_propertiesAndOffsets[index]);
                value = m_object->getDirect(offset);
            } else {
                value = m_object->get(globalObject, propertyName);
                RETURN_IF_EXCEPTION(scope, false);
            }
        } else {
            if (m_propertyNames) {
                propertyName = m_propertyNames->propertyNameVector()[index];
                value = m_object->get(globalObject, propertyName);
                RETURN_IF_EXCEPTION(scope, false);
            } else {
                propertyName = std::get<0>(m_propertiesAndOffsets[index]);
                if (m_object->structureID() == m_structure->id()) {
                    unsigned offset = std::get<1>(m_propertiesAndOffsets[index]);
                    value = m_object->getDirect(offset);
                    if (value.isGetterSetter()) {
                        value = uncheckedDowncast<GetterSetter>(value)->callGetter(globalObject, m_object);
                        RETURN_IF_EXCEPTION(scope, false);
                    } else if (value.isCustomGetterSetter()) {
                        value = m_object->get(globalObject, propertyName);
                        RETURN_IF_EXCEPTION(scope, false);
                    }
                } else {
                    value = m_object->get(globalObject, propertyName);
                    RETURN_IF_EXCEPTION(scope, false);
                }
            }
        }

        rollBackPoint = builder.length();

        // Append the separator string.
        if (builder[rollBackPoint - 1] != '{')
            builder.append(',');
        stringifier.startNewLine(builder);

        // Append the property name, colon, and space.
#if ENABLE(AOT)
        if (!keyAsWritten.empty())
            builder.append(keyAsWritten);
        else
#endif
        {
            builder.appendQuotedJSONString(*propertyName.uid());
            builder.append(':');
        }
        if (stringifier.willIndent())
            builder.append(' ');

        // Append the stringified value.
        stringifyResult = stringifier.appendStringifiedValue(builder, value, *this, propertyName);
    }
    RETURN_IF_EXCEPTION(scope, false);

    // From this point on, no access to the this pointer or to any members, because the
    // Holder object may have moved if the call to stringify pushed a new Holder onto
    // m_holderStack.

    switch (stringifyResult) {
        case StringifyFailed:
            builder.append("null"_s);
            break;
        case StringifySucceeded:
            break;
        case StringifyFailedDueToUndefinedOrSymbolValue:
            // This only occurs when we get an undefined value or a symbol value for
            // an object property. In this case we don't want the separator and
            // property name that we already appended, so roll back.
            builder.shrink(rollBackPoint);
            break;
    }

    return true;
}

// ------------------------------ FastStringifier --------------------------------

// FastStringifier does a no-side-effects stringify of the most common types of
// objects and arrays. It bails out if the serialization is any longer than a
// fixed buffer and handles only the simplest cases, including only 8-bit character
// strings. Instead of explicit checks to prevent excessive recursion and cycles,
// it counts on hitting the buffer size limit to catch those things. If it fails,
// since there is no side effect, the full general purpose Stringifier can be used
// and the only cost of the fast stringifying attempt is the time wasted.

enum class BufferMode : uint8_t {
    StaticBuffer,
    DynamicBuffer,
};

enum class HasGap : bool { No, Yes };

enum class FailureReason : uint8_t {
    BufferFull,
    Found16BitEarly,
    Found16BitLate,
    StackOverflow,
    Unknown,
};

// A program that makes one result is likely to make another much like it. lengthOfBigResult and isWide are of the last result that outgrew the static buffer, isWideInStaticBuffer of the last made with 16-bit characters.
struct StringifyHints {
    uint32_t lengthOfBigResult;
    bool isWide;
    bool isWideInStaticBuffer;
};
thread_local static StringifyHints stringifyHints;

#if ENABLE(AOT)
using AOT::WatchedJSONLevels;
#endif

// The lengths in characters of the last two results produced by a dynamic-buffer FastStringifier of each character
// type are kept in the VM, the newer in the low 32 bits.
template<typename CharType>
static uint64_t& dynamicBufferLengths(VM& vm)
{
    if constexpr (std::is_same_v<CharType, Latin1Character>)
        return vm.recentLatin1JSONStringifyLengths();
    else
        return vm.recentUTF16JSONStringifyLengths();
}

// Bounds how much address space a stale hint can reserve up front.
static constexpr unsigned maxDynamicBufferLengthHint = 32 * MB;
static constexpr unsigned dynamicBufferLengthHintSlack = 256;

template<typename CharType, BufferMode bufferMode>
class FastStringifier {
public:
    // Returns null string if the fast case fails. `levelsAround`: the objects and lists that somebody else is writing around the value.
    static String stringify(JSGlobalObject&, JSValue, JSValue replacer, JSValue space, std::optional<FailureReason>&, unsigned levelsAround = 0);

    static constexpr bool growsBuffer = bufferMode != BufferMode::StaticBuffer;
    static constexpr unsigned staticBufferSize = bufferMode == BufferMode::StaticBuffer ? 8192 : 8;
    static constexpr unsigned dynamicBufferInlineCapacity = bufferMode == BufferMode::DynamicBuffer ? 1024 : 0;

    static constexpr bool useShortCopyTier = growsBuffer;

    // A string that is at least so long is gone through once, escaping as it goes, so much at a time. (A shorter one is copied in the hope that there is nothing to escape.)
    static constexpr unsigned lengthOfLongString = 64;
    static constexpr unsigned pieceOfLongString = bufferMode == BufferMode::StaticBuffer ? 256 : 4096;
    static constexpr unsigned lengthOfBigResult = 128 * 1024;

    static constexpr unsigned preparedStructureTableSizeLog2 = bufferMode == BufferMode::StaticBuffer ? 4 : 8;
    static constexpr unsigned preparedStructureTableSize = 1u << preparedStructureTableSizeLog2;
    static constexpr unsigned maxPreparedProperties = bufferMode == BufferMode::StaticBuffer ? 128 : 1024;
    static constexpr unsigned maxPropertiesPerPreparedStructure = 32;
    static constexpr unsigned maxPreparedPropertyScratch = 2 * maxPropertiesPerPreparedStructure;

    struct PreparedStructureRange {
        uint16_t firstPropertyIndex;
        uint16_t propertyCount;
    };

    struct PreparedProperty {
        uint32_t keyOffset;
        uint16_t keyLength;
        int16_t storageIndex;
    };
    static_assert(sizeof(PreparedProperty) == 8);

private:
    explicit FastStringifier(JSGlobalObject&);
    template<HasGap hasGap> void append(JSValue);
    void appendInt32(int32_t);
    template<HasGap hasGap> void appendInt32Array(JSArray&);
#if ENABLE(AOT)
    void appendDate(DateInstance&);
    bool datesHaveBuiltinToJSON(JSObject& datePrototype);
    bool readOwnMember(JSObject&, PropertyName);
    template<HasGap hasGap> void appendCompiledObject(JSObject&);
    bool tryAppendObjectWithExtraProperties(JSObject&);
    bool mayCompiledObjectHaveToJSON(JSObject&);
    bool mayHaveOwnOrInheritedToJSON(JSObject&);
    // The position of the next character and the end of the buffer. While a cursor is in use, m_length is stale.
    struct Cursor {
        CharType* next { nullptr };
        CharType* limit { nullptr };
    };
    Cursor cursorAtEnd() { return { buffer() + m_length, buffer() + m_capacity }; }
    void publishLength(const Cursor& cursor) { m_length = cursor.next - buffer(); }
    bool reserve(Cursor&, size_t);
    static std::optional<unsigned> reservationFor(std::span<const AOT::FieldAsProperty>);
    template<HasGap hasGap> bool writeListedFields(Cursor&, JSObject&, std::span<const AOT::FieldAsProperty>, unsigned reservation);
    template<HasGap hasGap> void appendListedFields(JSObject&, std::span<const AOT::FieldAsProperty>);
    bool isMemberWrittenByItsFields(JSObject&);
    bool writeListedFieldsOfMember(Cursor&, JSObject&, std::span<const AOT::FieldAsProperty>, unsigned reservation);
    void appendElementsInCompiledProgram(JSArray&);
    enum class ArrayResult : uint8_t { No, Yes, Failed };
    ArrayResult writePrimitiveArray(Cursor&, JSArray&);
    bool isCompiledStructureKnown(const JSObject& object) const { return (m_compiledStructuresKnown & knowsCompiledStructure) && object.structureID().bits() == m_compiledStructureWithoutToJSON; }
#endif
    template<HasGap hasGap> bool appendPreparedObjectKey(const PreparedProperty&, bool& wroteProperty, unsigned newLineAndIndent, unsigned valueCapacity, CharType*& out, unsigned& length, unsigned& capacity);
    template<HasGap hasGap> void appendPreparedObject(JSObject&, unsigned structureSlot);
    template<HasGap hasGap> void appendUnpreparedObject(JSObject&, StructureID);
    String result();

    static ALWAYS_INLINE unsigned preparedStructureSlot(StructureID structureID)
    {
        return (structureID.bits() * 0x9e3779b9u) >> (32 - preparedStructureTableSizeLog2);
    }
    Structure* preparedStructure(unsigned slot) const { return m_preparedStructureCount ? m_preparedStructures[slot] : nullptr; }

    static constexpr unsigned maxInt32StringLength = ("-2147483648"_s).length();

    // FIXME These should probably just take an ASCIILiteral.
    void append(char, char, char, char);
    void append(char, char, char, char, char);
    bool setGap(JSValue space);
    unsigned newLineAndIndentSize() const;
    void appendNewLineAndIndentUnchecked();
    template<typename StringCharType> bool appendContentOfLongString(std::span<const StringCharType>);
    void recordWideString();
    template<typename T> void recordFailure(FailureReason, T&& reason);
    template<typename T> NODELETE void recordFailure(T&& reason)
    {
        recordFailure(FailureReason::Unknown, std::forward<T>(reason));
    }
    void recordBufferFull();
    String firstGetterSetterPropertyName(JSObject&) const;
    void recordFastPropertyEnumerationFailure(JSObject&);
    bool haveFailure() const;
    bool startBelow(unsigned levelsAround);
    template<HasGap hasGap> bool enterHolder();
    template<HasGap hasGap> void leaveHolder();
    bool hasRemainingCapacity(unsigned size = 1);
    bool hasRemainingCapacitySlow(unsigned size);
#if ENABLE(AOT)
    bool hasStackSpaceAtWatchedLevel(JSValue, void* stackPointer);
#endif
    bool mayHaveToJSON(JSObject&) const;

    static void logOutcome(ASCIILiteral);
    static void logOutcome(String&&);

    static unsigned usableBufferSize(unsigned availableBufferSize);

    CharType* buffer();
    const CharType* buffer() const;
    std::span<CharType> bufferSpan();

    JSGlobalObject& m_globalObject;
    VM& m_vm;
    unsigned m_length { 0 }; // length of content already filled into m_buffer.
    unsigned m_capacity { 0 };
    unsigned m_depth { 0 };
    unsigned m_gapLength { 0 };
    std::array<Latin1Character, maxGapLength> m_gap;
    bool m_checkedObjectPrototype { false };
    bool m_checkedArrayPrototype { false };
    bool m_hasSeenWideString { false };
#if ENABLE(AOT)
    // These two fill existing padding, so the members after them keep the offsets that they have without ENABLE(AOT).
    static constexpr uint8_t knowsCompiledStructure = 1;
    static constexpr uint8_t knowsCompiledStructureOfMember = 2;
    uint8_t m_compiledStructuresKnown { 0 };
    TriState m_datesHaveBuiltinToJSON { TriState::Indeterminate };
#endif
    uint16_t m_preparedStructureCount { 0 };
    uint16_t m_preparedPropertyCount { 0 };
    uint16_t m_preparedPropertyScratchCount { 0 };
    std::optional<FailureReason> m_failureReason;
#if USE(BUN_JSC_ADDITIONS)
    // This table and the next are cleared where the first object is written without a record, so a value without one pays for neither.
    std::array<Structure*, preparedStructureTableSize> m_preparedStructures;
    // Upstream prepares the Structure of every object it writes. The record costs more than it saves until another object of
    // that Structure comes, and most values have one object of each. So a Structure is prepared where another one is likely:
    // for an element of a list of two or more, and for an object whose Structure appendUnpreparedObject() meets again.
    // The bits of the StructureID that it last met in each slot. No StructureID is zero.
    std::array<uint32_t, preparedStructureTableSize> m_structuresMetOnce;
    bool m_hasClearedStructureTables { false };
    // Set around the append() of such an element. appendUnpreparedObject() takes it, so what lies in the element does not see it.
    bool m_isElementOfList { false };
#else
    std::array<Structure*, preparedStructureTableSize> m_preparedStructures { };
#endif
    Vector<CharType, dynamicBufferInlineCapacity, CrashOnOverflow, 16, WTF::StringImplMalloc> m_dynamicBuffer;
    uint8_t* m_stackLimit { nullptr };
#if ENABLE(AOT)
    uint32_t m_compiledStructureWithoutToJSON; // The bits of a StructureID. Written before its bit in m_compiledStructuresKnown is set.
    uint32_t m_compiledStructureOfMemberWithoutToJSON; // The same for an object that is a member of another.
    // While it is watching, m_stackLimit is above every frame, so every append() takes the slow check.
    NO_UNIQUE_ADDRESS std::conditional_t<growsBuffer, WatchedJSONLevels, std::monostate> m_watchedLevels;
#endif
    std::array<PreparedStructureRange, preparedStructureTableSize> m_preparedStructureRanges;
    std::array<PreparedProperty, maxPreparedProperties> m_preparedProperties;
    std::array<PreparedProperty, maxPreparedPropertyScratch> m_preparedPropertyScratch;

    CharType m_buffer[staticBufferSize];
};

#if !FAST_STRINGIFY_LOG_USAGE

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::logOutcome(ASCIILiteral)
{
}

#else

static void logOutcomeImpl(String&& outcome)
{
    static NeverDestroyed<HashCountedSet<String>> set;
    static std::atomic<unsigned> count;
    set->add(outcome);
    if (!(++count % 100)) {
        Vector<KeyValuePair<String, unsigned>> vector;
        for (auto& pair : set.get())
            vector.append(pair);
        std::ranges::sort(vector, [](auto& a, auto &b) {
            return a.value != b.value ? a.value > b.value : codePointCompareLessThan(a.key, b.key);
        });
        dataLogLn("fastStringify outcomes");
        for (auto& pair : vector) {
            dataLogF("%5u", pair.value);
            dataLogLn(": ", pair.key);
        }
    }
}

template<typename CharType, BufferMode bufferMode>
void FastStringifier<CharType, bufferMode>::logOutcome(ASCIILiteral outcome)
{
    logOutcomeImpl(String { outcome });
}

template<typename CharType, BufferMode bufferMode>
void FastStringifier<CharType, bufferMode>::logOutcome(String&& outcome)
{
    logOutcomeImpl(WTF::move(outcome));
}

#endif

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE CharType* FastStringifier<CharType, bufferMode>::buffer()
{
    if constexpr (bufferMode == BufferMode::StaticBuffer)
        return m_buffer;
    else
        return m_dynamicBuffer.mutableSpan().data();
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE const CharType* FastStringifier<CharType, bufferMode>::buffer() const
{
    if constexpr (bufferMode == BufferMode::StaticBuffer)
        return m_buffer;
    else
        return m_dynamicBuffer.span().data();
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE std::span<CharType> FastStringifier<CharType, bufferMode>::bufferSpan()
{
    if constexpr (bufferMode == BufferMode::StaticBuffer)
        return std::span<CharType> { m_buffer };
    else
        return m_dynamicBuffer.mutableSpan();
}

template<typename CharType, BufferMode bufferMode>
inline unsigned FastStringifier<CharType, bufferMode>::usableBufferSize(unsigned availableBufferSize)
{
    // FastStringifier relies on m_capacity (i.e. the remaining usable capacity) in m_buffer
    // to limit recursion. Hence, we need to compute an appropriate m_capacity value.
    //
    // To do this, we empirically measured the worst case stack usage incurred by 1 recursion
    // of any of the append methods. Assuming each call to append() only consumes 1 Latin1Character in
    // m_buffer, the amount of buffer size that FastStringifier is allowed to run with can be
    // estimated as:
    //
    //      stackCapacityForRecursion = remainingStackCapacity - maxLeafFunctionStackUsage
    //      maxAllowedBufferSize = stackCapacityForRecursion / maxRecursionFrameSize
    //      usableBufferSize = min(maxAllowedBufferSize, sizeof(m_buffer))
    //
    // 1. A leaf function is any function that append() calls which does not recurse.
    //    At peak recursion, there needs to be enough room left on the stack to execute any
    //    of these leaf functions i.e. maxLeafFunctionStackUsage.
    //
    //    We estimate maxLeafFunctionStackUsage to be StackBounds::DefaultReservedZone.
    //    stack.recursionLimit() already adds DefaultReservedZone to the bottom of the stack.
    //    Hence, using stack.recursionLimit() to compute stackCapacityForRecursion will leave
    //    us with the needed stack space for leaf functions to execute.
    //
    // 2. We can compute m_capacity as:
    //
    //      m_capacity = m_length + usableBufferSize
    //
    //    where m_length is the position of the next usable character for emission in m_buffer.
    //
    // 3. This calculation of m_capacity is a best effort estimate. If we're not
    //    conservative enough and get it wrong, the worst that can happen is that we'll
    //    crash when recursion causes us to step on the stack guard page at the bottom of
    //    the stack. The goal of trying to estimate a good m_capacity value is to avoid
    //    this stack overflow crash.
    //
    //    Note that for a Release build, maxRecursionFrameSize is measured to be less than
    //    384 bytes. This is well below stack guard page sizes which are between 4 and 16K
    //    depending on the OS. Hence, recursing too deeply with FastStringifier::append()
    //    is guaranteed to crash in the stack guard page.
    //
    // 4. If we're too conservative, we might fail out of FastStringifier too eagerly.
    //    In this case, we'll just fall back to the slow path Stringifier. The only down
    //    side here is potential loss of some performance opportunity when we encounter
    //    a workload that recurses deeply. We expect such workloads to be rare.

    auto& stack = Thread::currentSingleton().stack();
    uint8_t* stackPointer = std::bit_cast<uint8_t*>(currentStackPointer());
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(stack.recursionLimit());
    size_t stackCapacityForRecursion = stackPointer - stackLimit;

#if ASAN_ENABLED
    // Measured to be ~4608 for a Debug ASAN build on arm64E, rounding up to 5K for margin.
    constexpr size_t maxRecursionFrameSize = 5 * KB;
#elif !defined(NDEBUG)
    // Measured to be ~912 for a Debug build on arm64E, rounding up to 1280 for margin.
    constexpr size_t maxRecursionFrameSize = 1280;
#else
    // Measured to be ~224 for a Release build on arm64E, rounding up to 384 for margin.
    constexpr size_t maxRecursionFrameSize = 384;
#endif
    ASSERT(static_cast<unsigned>(stackCapacityForRecursion) == stackCapacityForRecursion);
    unsigned allowedBufferSize = stackCapacityForRecursion / maxRecursionFrameSize;
    unsigned usableBufferSize = std::min(allowedBufferSize, availableBufferSize);
    return usableBufferSize;
}

template<typename CharType, BufferMode bufferMode>
inline FastStringifier<CharType, bufferMode>::FastStringifier(JSGlobalObject& globalObject)
    : m_globalObject(globalObject)
    , m_vm(globalObject.vm())
{
    if constexpr (bufferMode == BufferMode::StaticBuffer)
        m_capacity = m_length + usableBufferSize(staticBufferSize);
    else {
        uint64_t recentLengths = dynamicBufferLengths<CharType>(m_vm);
        unsigned lengthHint = std::min({ static_cast<unsigned>(recentLengths), static_cast<unsigned>(recentLengths >> 32), maxDynamicBufferLengthHint });
#if USE(BUN_JSC_ADDITIONS)
        // appendContentOfLongString() asks for six times a piece before it writes one, which upstream's slack does not hold: a result with
        // a long string near its end grew to twice its length, was copied there, and was copied again when result() gave the room back.
        // The room of hasRemainingCapacitySlow() for a result like the last, and no more than the length, so that result() keeps the buffer.
        unsigned slack = std::max<unsigned>(dynamicBufferLengthHintSlack, std::min<unsigned>(lengthHint / 8 + pieceOfLongString * 6, lengthHint));
        unsigned initialSize = lengthHint > dynamicBufferInlineCapacity ? lengthHint + slack : dynamicBufferInlineCapacity;
#else
        unsigned initialSize = lengthHint > dynamicBufferInlineCapacity ? lengthHint + dynamicBufferLengthHintSlack : dynamicBufferInlineCapacity;
#endif
        if (initialSize == dynamicBufferInlineCapacity || !m_dynamicBuffer.tryGrow(initialSize))
            m_dynamicBuffer.grow(dynamicBufferInlineCapacity);
        m_capacity = m_dynamicBuffer.size();
        m_stackLimit = std::bit_cast<uint8_t*>(m_vm.softStackLimit());
    }
}

// The general way refuses a value that is nested deeper than maximumSideStackRecursion, on every machine. Here only the stack would say stop, and how deep that
// is depends on the thread and on the build. So what is nested deeper is left to the general way, which throws. False with a failure.
// A static buffer is full long before: every level writes a character.
template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::enterHolder()
{
    static_assert(growsBuffer || staticBufferSize < maximumSideStackRecursion);
    if constexpr (growsBuffer) {
        if (m_depth >= maximumSideStackRecursion) [[unlikely]] {
            recordFailure(FailureReason::StackOverflow, "nested too deep"_s);
            return false;
        }
    }
    if constexpr (hasGap == HasGap::Yes || growsBuffer)
        ++m_depth;
    return true;
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
ALWAYS_INLINE void FastStringifier<CharType, bufferMode>::leaveHolder()
{
    if constexpr (hasGap == HasGap::Yes || growsBuffer)
        --m_depth;
}

template<typename CharType, BufferMode bufferMode>
inline bool FastStringifier<CharType, bufferMode>::haveFailure() const
{
    return !!m_failureReason;
}

template<typename CharType, BufferMode bufferMode>
inline String FastStringifier<CharType, bufferMode>::result()
{
    if (haveFailure())
        return { };
#if FAST_STRINGIFY_LOG_USAGE
    static std::atomic<unsigned> maxSizeSeen;
    if (m_length > maxSizeSeen) {
        maxSizeSeen = m_length;
        dataLogLn("max fastStringify buffer size used: ", m_length);
    }
    logOutcome("success"_s);
#endif
    if constexpr (bufferMode == BufferMode::DynamicBuffer) {
        stringifyHints.isWide = sizeof(CharType) == 2 && m_hasSeenWideString;
        stringifyHints.isWideInStaticBuffer = stringifyHints.isWide;
        if (m_dynamicBuffer.size() > lengthOfBigResult)
            stringifyHints.lengthOfBigResult = m_length;
        uint64_t& recentLengths = dynamicBufferLengths<CharType>(m_vm);
        recentLengths = (recentLengths << 32) | m_length;
        if constexpr (sizeof(CharType) == 2) {
            if (!m_hasSeenWideString)
                return String::make8Bit(std::span { static_cast<const FastStringifier*>(this)->buffer(), m_length });
        }
        m_dynamicBuffer.shrink(m_length);
        if (m_dynamicBuffer.capacity() / 2 > m_length)
            m_dynamicBuffer.shrinkToFit();
        return StringImpl::adopt(WTF::move(m_dynamicBuffer));
    }
    if constexpr (sizeof(CharType) == 2) {
        stringifyHints.isWideInStaticBuffer = m_hasSeenWideString;
        if (!m_hasSeenWideString)
            return String::make8Bit(std::span { static_cast<const FastStringifier*>(this)->buffer(), m_length });
    }
    return std::span { static_cast<const FastStringifier*>(this)->buffer(), m_length };
}

template<typename CharType, BufferMode bufferMode>
template<typename T> inline void FastStringifier<CharType, bufferMode>::recordFailure(FailureReason failureReason, T&& reason)
{
    if (!haveFailure())
        logOutcome(std::forward<T>(reason));
    m_failureReason = failureReason;
}

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::recordBufferFull()
{
    recordFailure(FailureReason::BufferFull, "buffer full"_s);
}

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::recordWideString()
{
    if constexpr (growsBuffer)
        recordFailure(FailureReason::Found16BitLate, "16-bit string, "_s);
    else
        recordFailure(m_length < (m_capacity / 2) ? FailureReason::Found16BitEarly : FailureReason::Found16BitLate, "16-bit string, "_s);
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::hasRemainingCapacity(unsigned size)
{
    ASSERT(!haveFailure());
    ASSERT(size > 0);
    unsigned remainingCapacity = m_capacity - m_length;
    if (size <= remainingCapacity)
        return true;
    return hasRemainingCapacitySlow(size);
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::hasRemainingCapacitySlow(unsigned size)
{
    ASSERT(!haveFailure());
    if constexpr (bufferMode == BufferMode::StaticBuffer) {
        unsigned unusedBufferSize = staticBufferSize - m_length;
        unsigned usableSize = usableBufferSize(unusedBufferSize);
        if (usableSize < size)
            return false;

        m_capacity = m_length + usableSize;
        ASSERT(m_capacity - m_length >= size);
        return true;
    } else {
        size_t newSize = std::max<size_t>(m_dynamicBuffer.size() * 2, m_dynamicBuffer.size() + size);
#if ENABLE(AOT)
        if (m_watchedLevels.isWatchingAfterGrowthTo(newSize))
            m_stackLimit = std::bit_cast<uint8_t*>(std::numeric_limits<uintptr_t>::max());
#endif
        if (newSize > lengthOfBigResult) {
            size_t lengthOfLast = stringifyHints.lengthOfBigResult;
            size_t sizeForResultLikeLast = lengthOfLast + lengthOfLast / 8 + pieceOfLongString * 6;
            if (sizeForResultLikeLast > newSize) {
                if (StringImpl::isValidLength<CharType>(sizeForResultLikeLast) && m_dynamicBuffer.tryGrow(sizeForResultLikeLast)) [[likely]] {
                    m_capacity = m_dynamicBuffer.size();
                    ASSERT(m_capacity - m_length >= size);
                    return true;
                }
                // This result may be far smaller than the last, and what it needs may still be there.
                stringifyHints.lengthOfBigResult = 0;
            }
        }
        if (!StringImpl::isValidLength<CharType>(newSize)) [[unlikely]]
            return false;

        if (!m_dynamicBuffer.tryGrow(newSize)) [[unlikely]]
            return false;

        m_capacity = m_dynamicBuffer.size();
        ASSERT(m_capacity - m_length >= size);
        return true;
    }
}

#if ENABLE(AOT)
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE bool FastStringifier<CharType, bufferMode>::hasStackSpaceAtWatchedLevel(JSValue value, void* stackPointer)
{
    if constexpr (growsBuffer) {
        uint8_t* stackLimit = std::bit_cast<uint8_t*>(m_vm.softStackLimit());
        if (std::bit_cast<uint8_t*>(stackPointer) < stackLimit) {
            recordFailure(FailureReason::StackOverflow, "stack overflow"_s);
            return false;
        }
        bool shouldContinue = !m_watchedLevels.isWatching() || !value.isObject() || m_watchedLevels.enter(JSValue::encode(value), m_depth);
        if (!m_watchedLevels.isWatching())
            m_stackLimit = stackLimit;
        if (!shouldContinue)
            recordFailure("object inside itself"_s);
        return shouldContinue;
    } else {
        UNUSED_PARAM(value);
        UNUSED_PARAM(stackPointer);
        return true;
    }
}
#endif

template<typename CharType, BufferMode bufferMode>
template<typename StringCharType>
bool FastStringifier<CharType, bufferMode>::appendContentOfLongString(std::span<const StringCharType> rest)
{
    while (!rest.empty()) {
        auto piece = rest.first(std::min<size_t>(rest.size(), pieceOfLongString));
        if constexpr (sizeof(StringCharType) == 2) {
            if (piece.size() < rest.size() && U16_IS_LEAD(piece.back()))
                piece = piece.first(piece.size() - 1);
        }
        if (!hasRemainingCapacity(piece.size() * 6 + 1)) [[unlikely]] {
            recordBufferFull();
            return false;
        }
        auto output = bufferSpan().subspan(m_length);
        if (!WTF::appendEscapedJSONStringContent(output, piece)) [[unlikely]] {
            recordWideString();
            return false;
        }
        m_length = output.data() - buffer();
        rest = rest.subspan(piece.size());
    }
    return true;
}

#if !FAST_STRINGIFY_LOG_USAGE

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::recordFastPropertyEnumerationFailure(JSObject&)
{
    recordFailure("!canPerformFastPropertyEnumerationForJSONStringify"_s);
}

#else

template<typename CharType, BufferMode bufferMode>
String FastStringifier<CharType, bufferMode>::firstGetterSetterPropertyName(JSObject& object) const
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    PropertyNameArray names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Include);
    JSObject::getOwnPropertyNames(&object, &m_globalObject, names, DontEnumPropertiesMode::Include);
    CLEAR_AND_RETURN_IF_EXCEPTION(scope, "getOwnPropertyNames exception occurred"_s);
    for (auto& name : names) {
        PropertySlot slot(&object, PropertySlot::InternalMethodType::Get);
        JSObject::getOwnPropertySlot(&object, &m_globalObject, name, slot);
        CLEAR_AND_RETURN_IF_EXCEPTION(scope, "getOwnPropertySlot exception occurred"_s);
        if (slot.isAccessor())
            RELEASE_AND_RETURN(scope, name.string());
    }
    RELEASE_AND_RETURN(scope, "not found"_s);
}

template<typename CharType, BufferMode bufferMode>
void FastStringifier<CharType, bufferMode>::recordFastPropertyEnumerationFailure(JSObject& object)
{
    auto& structure = *object.structure();
    if (structure.typeInfo().overridesGetOwnPropertySlot())
        recordFailure("overridesGetOwnPropertySlot"_s);
    else if (structure.typeInfo().overridesAnyFormOfGetOwnPropertyNames())
        recordFailure("overridesAnyFormOfGetOwnPropertyNames"_s);
    else if (hasIndexedProperties(structure.indexingType()))
        recordFailure("hasIndexedProperties"_s);
    else if (structure.hasAnyKindOfGetterSetterProperties())
        recordFailure("getter/setter: "_s + firstGetterSetterPropertyName(object));
    else if (structure.hasReadOnlyOrGetterSetterPropertiesExcludingProto())
        recordFailure("hasReadOnlyOrGetterSetterPropertiesExcludingProto"_s);
    else if (structure.isUncacheableDictionary())
        recordFailure("isUncacheableDictionary"_s);
    else if (structure.hasUnderscoreProtoPropertyExcludingOriginalProto())
        recordFailure("hasUnderscoreProtoPropertyExcludingOriginalProto"_s);
    else
        recordFailure("!canPerformFastPropertyEnumerationForJSONStringify mystery"_s);
}

#endif

template<typename CharType, BufferMode bufferMode>
inline bool FastStringifier<CharType, bufferMode>::mayHaveToJSON(JSObject& object) const
{
    if (auto function = object.structure()->cachedSpecialProperty(CachedSpecialPropertyKey::ToJSON))
        return !function.isUndefined();
    if (object.noSideEffectMayHaveNonIndexProperty(m_vm, m_vm.propertyNames->toJSON)) [[unlikely]] {
        // Getting the property value so we can cache it could cause side effects; instead return true without caching anything.
        return true;
    }
    // Cache this so we can answer false next time without redoing the noSideEffectMayHaveNonIndexProperty work.
    PropertySlot slot { &object, PropertySlot::InternalMethodType::Get };
    object.structure()->cacheSpecialProperty(&m_globalObject, m_vm, jsUndefined(), CachedSpecialPropertyKey::ToJSON, slot);
    return false;
}

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::append(char a, char b, char c, char d)
{
    if (!hasRemainingCapacity(4)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    buffer()[m_length] = a;
    buffer()[m_length + 1] = b;
    buffer()[m_length + 2] = c;
    buffer()[m_length + 3] = d;
    m_length += 4;
}

template<typename CharType, BufferMode bufferMode>
inline void FastStringifier<CharType, bufferMode>::append(char a, char b, char c, char d, char e)
{
    if (!hasRemainingCapacity(5)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    buffer()[m_length] = a;
    buffer()[m_length + 1] = b;
    buffer()[m_length + 2] = c;
    buffer()[m_length + 3] = d;
    buffer()[m_length + 4] = e;
    m_length += 5;
}

// Resolves the space argument into a gap string, mirroring the gap() helper above.
// Returns false for the cases left to the general Stringifier: boxed primitives
// (observable unwrap), 16-bit gap strings, and other cells.
template<typename CharType, BufferMode bufferMode>
inline bool FastStringifier<CharType, bufferMode>::setGap(JSValue space)
{
    ASSERT(!space.isUndefined());
    if (space.isNumber()) {
        m_gapLength = clampTo<unsigned>(space.asNumber(), 0, maxGapLength);
        m_gap.fill(' ');
        return true;
    }
    if (space.isString()) {
        auto string = asString(space)->tryGetValue();
        if (string.data.isNull() || !string.data.is8Bit()) [[unlikely]]
            return false;
        auto span = string.data.span8();
        unsigned count = std::min<unsigned>(span.size(), maxGapLength);
        memcpySpan(std::span { m_gap }.first(count), span.first(count));
        m_gapLength = count;
        return true;
    }
    if (space.isCell())
        return false;
    // Other primitives (booleans, null) mean no gap.
    m_gapLength = 0;
    return true;
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE unsigned FastStringifier<CharType, bufferMode>::newLineAndIndentSize() const
{
    ASSERT(m_gapLength);
    return 1 + m_depth * m_gapLength;
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE void FastStringifier<CharType, bufferMode>::appendNewLineAndIndentUnchecked()
{
    ASSERT(m_gapLength);
    auto* cursor = buffer() + m_length;
    *cursor++ = '\n';
    for (unsigned i = 0; i < m_depth; ++i) {
        for (unsigned j = 0; j < m_gapLength; ++j)
            *cursor++ = m_gap[j];
    }
    m_length += 1 + m_depth * m_gapLength;
}

template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE void FastStringifier<CharType, bufferMode>::appendInt32(int32_t number)
{
    if constexpr (sizeof(CharType) == 1) {
#if ENABLE(AOT)
        m_length = AOT::appendJSONInteger(buffer() + m_length, number) - buffer();
#else
        char* cursor = std::bit_cast<char*>(buffer()) + m_length;
        auto result = std::to_chars(cursor, cursor + maxInt32StringLength, number);
        ASSERT(result.ec != std::errc::value_too_large);
        m_length += result.ptr - cursor;
#endif
    } else {
        std::array<char, maxInt32StringLength> temporary;
        auto result = std::to_chars(temporary.data(), temporary.data() + maxInt32StringLength, number);
        ASSERT(result.ec != std::errc::value_too_large);
        unsigned lengthToCopy = result.ptr - temporary.data();
        WTF::copyElements(spanReinterpretCast<uint16_t>(bufferSpan().subspan(m_length)), spanReinterpretCast<const uint8_t>(std::span { temporary }).first(lengthToCopy));
        m_length += lengthToCopy;
    }
}

static ALWAYS_INLINE bool eightByteRangeNeedsJSONEscape(uint64_t word)
{
    bool hasControl = hasZeroByte(word & 0xE0E0E0E0E0E0E0E0ULL); // any byte in [0x00,0x1F]?
    bool hasQuote = hasZeroByte(word ^ 0x2222222222222222ULL); // any byte == '"'?
    bool hasBackslash = hasZeroByte(word ^ 0x5C5C5C5C5C5C5C5CULL); // any byte == '\\'?
    return hasControl || hasQuote || hasBackslash;
}

template<typename CharType>
static ALWAYS_INLINE uint64_t copyEightBytesAndLoad(const CharType* source, CharType* destination)
{
    uint64_t word = WTF::unalignedLoad<uint64_t>(source);
    WTF::unalignedStore<uint64_t>(destination, word);
    return word;
}

static ALWAYS_INLINE uint32_t copyFourBytesAndLoad(const Latin1Character* source, Latin1Character* destination)
{
    uint32_t word = WTF::unalignedLoad<uint32_t>(source);
    WTF::unalignedStore<uint32_t>(destination, word);
    return word;
}

static ALWAYS_INLINE uint64_t upconvertEightBytesAndLoad(const Latin1Character* source, char16_t* destination)
{
    uint64_t word = WTF::unalignedLoad<uint64_t>(source);
    uint64_t low = zeroExtendBytesToHalfwords(static_cast<uint32_t>(word));
    uint64_t high = zeroExtendBytesToHalfwords(static_cast<uint32_t>(word >> 32));
    WTF::unalignedStore<uint64_t>(destination, low);
    WTF::unalignedStore<uint64_t>(destination + 4, high);
    return word;
}

#if (CPU(ARM64) || CPU(X86_64)) && COMPILER(CLANG)
#define JSON_STRING_COPY_HAS_SIMD 1
#else
#define JSON_STRING_COPY_HAS_SIMD 0
#endif

static constexpr size_t narrowStride = sizeof(uint64_t);

template<typename CharType, bool useShortCopyTier = false>
static ALWAYS_INLINE bool stringCopySameType(std::span<const CharType> span, CharType* cursor)
{
#if JSON_STRING_COPY_HAS_SIMD
    constexpr size_t stride = SIMD::stride<CharType>;
    if (span.size() >= stride) {
        using UnsignedType = SameSizeUnsignedInteger<CharType>;
        using BulkType = decltype(SIMD::load(static_cast<const UnsignedType*>(nullptr)));
        constexpr auto quoteMask = SIMD::splat<UnsignedType>('"');
        constexpr auto escapeMask = SIMD::splat<UnsignedType>('\\');
        constexpr auto controlMask = SIMD::splat<UnsignedType>(' ');
        const auto* ptr = span.data();
        const auto* end = ptr + span.size();
        auto* cursorEnd = cursor + span.size();
        BulkType accumulated { };
        BulkType accumulatedSurrogates { };
        for (; ptr + stride <= end; ptr += stride, cursor += stride) {
            auto input = SIMD::load(std::bit_cast<const UnsignedType*>(ptr));
            SIMD::store(input, std::bit_cast<UnsignedType*>(cursor));
            auto quotes = SIMD::equal(input, quoteMask);
            auto escapes = SIMD::equal(input, escapeMask);
            auto controls = SIMD::lessThan(input, controlMask);
            accumulated = SIMD::bitOr(accumulated, quotes, escapes, controls);
            if constexpr (sizeof(CharType) != 1) {
                constexpr auto surrogateMask = SIMD::splat<UnsignedType>(0xf800);
                constexpr auto surrogateCheckMask = SIMD::splat<UnsignedType>(0xd800);
                accumulatedSurrogates = SIMD::bitOr(accumulatedSurrogates, SIMD::equal(SIMD::bitAnd(input, surrogateMask), surrogateCheckMask));
            }
        }
        if (ptr < end) {
            auto input = SIMD::load(std::bit_cast<const UnsignedType*>(end - stride));
            SIMD::store(input, std::bit_cast<UnsignedType*>(cursorEnd - stride));
            auto quotes = SIMD::equal(input, quoteMask);
            auto escapes = SIMD::equal(input, escapeMask);
            auto controls = SIMD::lessThan(input, controlMask);
            accumulated = SIMD::bitOr(accumulated, quotes, escapes, controls);
            if constexpr (sizeof(CharType) != 1) {
                constexpr auto surrogateMask = SIMD::splat<UnsignedType>(0xf800);
                constexpr auto surrogateCheckMask = SIMD::splat<UnsignedType>(0xd800);
                accumulatedSurrogates = SIMD::bitOr(accumulatedSurrogates, SIMD::equal(SIMD::bitAnd(input, surrogateMask), surrogateCheckMask));
            }
        }
        if (SIMD::isNonZero(accumulated))
            return true;
        if constexpr (sizeof(CharType) != 1) {
            if (SIMD::isNonZero(accumulatedSurrogates))
                return !WTF::isWellFormedUTF16(span);
        }
        return false;
    }
#endif
    if constexpr (sizeof(CharType) == 1) {
        if (span.size() >= narrowStride) {
            const auto* ptr = span.data();
            const auto* end = ptr + span.size();
            auto* cursorEnd = cursor + span.size();
            uint64_t accumulated = 0;
#if JSON_STRING_COPY_HAS_SIMD
            static_assert(stride == 2 * narrowStride);
            ASSERT(span.size() < 2 * narrowStride);
            // The first and the last eight bytes in one vector. They overlap, which does no harm.
            auto input = simde_vcombine_u8(simde_vld1_u8(std::bit_cast<const uint8_t*>(ptr)), simde_vld1_u8(std::bit_cast<const uint8_t*>(end - narrowStride)));
            simde_vst1_u8(std::bit_cast<uint8_t*>(cursor), simde_vget_low_u8(input));
            simde_vst1_u8(std::bit_cast<uint8_t*>(cursorEnd - narrowStride), simde_vget_high_u8(input));
            UNUSED_VARIABLE(accumulated);
            return SIMD::isNonZero(SIMD::bitOr(SIMD::equal(input, SIMD::splat<uint8_t>('"')), SIMD::equal(input, SIMD::splat<uint8_t>('\\')), SIMD::lessThan(input, SIMD::splat<uint8_t>(' '))));
#else
            for (; ptr + narrowStride <= end; ptr += narrowStride, cursor += narrowStride)
                accumulated |= eightByteRangeNeedsJSONEscape(copyEightBytesAndLoad(ptr, cursor));
            if (ptr < end)
                accumulated |= eightByteRangeNeedsJSONEscape(copyEightBytesAndLoad(end - narrowStride, cursorEnd - narrowStride));
            return accumulated;
#endif
        }
        if constexpr (useShortCopyTier) {
            if (span.size() >= 4) {
                const auto* ptr = span.data();
                const auto* end = ptr + span.size();
                auto* cursorEnd = cursor + span.size();
                uint64_t low = copyFourBytesAndLoad(ptr, cursor);
                uint64_t high = copyFourBytesAndLoad(end - 4, cursorEnd - 4);
                return eightByteRangeNeedsJSONEscape(low | (high << 32));
            }
        }
    }
    for (auto character : span) {
        if constexpr (sizeof(CharType) != 1) {
            if (U16_IS_SURROGATE(character)) [[unlikely]]
                return true;
        }
        if (character <= 0xff && WTF::escapedFormsForJSON[character]) [[unlikely]]
            return true;
        *cursor++ = character;
    }
    return false;
}

static ALWAYS_INLINE bool stringCopyUpconvert(std::span<const Latin1Character> span, char16_t* cursor)
{
#if JSON_STRING_COPY_HAS_SIMD
    constexpr size_t stride = SIMD::stride<Latin1Character>;
    if (span.size() >= stride) {
        using UnsignedType = uint8_t;
        using BulkType = decltype(SIMD::load(static_cast<const UnsignedType*>(nullptr)));
        constexpr auto quoteMask = SIMD::splat<UnsignedType>('"');
        constexpr auto escapeMask = SIMD::splat<UnsignedType>('\\');
        constexpr auto controlMask = SIMD::splat<UnsignedType>(' ');
        constexpr auto zeros = SIMD::splat<UnsignedType>(0);
        const auto* ptr = span.data();
        const auto* end = ptr + span.size();
        auto* cursorEnd = cursor + span.size();
        BulkType accumulated { };
        for (; ptr + stride <= end; ptr += stride, cursor += stride) {
            auto input = SIMD::load(std::bit_cast<const UnsignedType*>(ptr));
            simde_vst2q_u8(std::bit_cast<UnsignedType*>(cursor), (simde_uint8x16x2_t { input, zeros }));
            auto quotes = SIMD::equal(input, quoteMask);
            auto escapes = SIMD::equal(input, escapeMask);
            auto controls = SIMD::lessThan(input, controlMask);
            accumulated = SIMD::bitOr(accumulated, quotes, escapes, controls);
        }
        if (ptr < end) {
            auto input = SIMD::load(std::bit_cast<const UnsignedType*>(end - stride));
            simde_vst2q_u8(std::bit_cast<UnsignedType*>(cursorEnd - stride), (simde_uint8x16x2_t { input, zeros }));
            auto quotes = SIMD::equal(input, quoteMask);
            auto escapes = SIMD::equal(input, escapeMask);
            auto controls = SIMD::lessThan(input, controlMask);
            accumulated = SIMD::bitOr(accumulated, quotes, escapes, controls);
        }
        return SIMD::isNonZero(accumulated);
    }
#endif
    if (span.size() >= narrowStride) {
        const auto* ptr = span.data();
        const auto* end = ptr + span.size();
        auto* cursorEnd = cursor + span.size();
        uint64_t accumulated = 0;
#if JSON_STRING_COPY_HAS_SIMD
        static_assert(stride <= 2 * narrowStride);
        ASSERT(span.size() < 2 * narrowStride);
        accumulated = eightByteRangeNeedsJSONEscape(upconvertEightBytesAndLoad(ptr, cursor));
        accumulated |= eightByteRangeNeedsJSONEscape(upconvertEightBytesAndLoad(end - narrowStride, cursorEnd - narrowStride));
#else
        for (; ptr + narrowStride <= end; ptr += narrowStride, cursor += narrowStride)
            accumulated |= eightByteRangeNeedsJSONEscape(upconvertEightBytesAndLoad(ptr, cursor));
        if (ptr < end)
            accumulated |= eightByteRangeNeedsJSONEscape(upconvertEightBytesAndLoad(end - narrowStride, cursorEnd - narrowStride));
#endif
        return accumulated;
    }
    for (auto character : span) {
        if (WTF::escapedFormsForJSON[character]) [[unlikely]]
            return true;
        *cursor++ = character;
    }
    return false;
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::appendPreparedObjectKey(const PreparedProperty& property, bool& wroteProperty, unsigned newLineAndIndent, unsigned valueCapacity, CharType*& out, unsigned& length, unsigned& capacity)
{
    unsigned keyLength = property.keyLength;
#if JSON_STRING_COPY_HAS_SIMD
    static constexpr unsigned copyGranularity = SIMD::stride<CharType>;
#else
    static constexpr unsigned copyGranularity = 1;
#endif
    unsigned requiredCapacity = std::max<unsigned>(keyLength + valueCapacity, copyGranularity);
    if constexpr (hasGap == HasGap::Yes)
        requiredCapacity += wroteProperty + newLineAndIndent;
    if (capacity - length < requiredCapacity) [[unlikely]] {
        m_length = length;
        if (!hasRemainingCapacitySlow(requiredCapacity)) [[unlikely]] {
            recordBufferFull();
            return false;
        }
        out = buffer();
        capacity = m_capacity;
    }

    if constexpr (hasGap == HasGap::Yes) {
        if (wroteProperty)
            out[length++] = ',';
        wroteProperty = true;
        m_length = length;
        appendNewLineAndIndentUnchecked();
        out = buffer();
        length = m_length;
    }

    const CharType* keyPtr = out + property.keyOffset;
#if JSON_STRING_COPY_HAS_SIMD
    using UnsignedType = SameSizeUnsignedInteger<CharType>;
    if (keyLength <= copyGranularity) [[likely]]
        SIMD::store(SIMD::load(std::bit_cast<const UnsignedType*>(keyPtr)), std::bit_cast<UnsignedType*>(out + length));
    else {
        unsigned offset = 0;
        for (; offset + copyGranularity < keyLength; offset += copyGranularity)
            SIMD::store(SIMD::load(std::bit_cast<const UnsignedType*>(keyPtr + offset)), std::bit_cast<UnsignedType*>(out + length + offset));
        SIMD::store(SIMD::load(std::bit_cast<const UnsignedType*>(keyPtr + keyLength - copyGranularity)), std::bit_cast<UnsignedType*>(out + length + keyLength - copyGranularity));
    }
#else
    memcpySpan(std::span { out + length, keyLength }, std::span { keyPtr, keyLength });
#endif

    length += keyLength;
    return true;
}

#undef JSON_STRING_COPY_HAS_SIMD

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendPreparedObject(JSObject& object, unsigned structureSlot)
{
    auto range = m_preparedStructureRanges[structureSlot];
    if (!range.propertyCount) [[unlikely]] {
        if (!enterHolder<hasGap>()) [[unlikely]]
            return;
        leaveHolder<hasGap>();
        if (!hasRemainingCapacity(2)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        CharType* out = buffer() + m_length;
        out[0] = '{';
        out[1] = '}';
        m_length += 2;
        return;
    }

    if constexpr (hasGap == HasGap::Yes) {
        if (!hasRemainingCapacity()) [[unlikely]] {
            recordBufferFull();
            return;
        }
        buffer()[m_length++] = '{';
    }
    if (!enterHolder<hasGap>()) [[unlikely]]
        return;

    const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
    const auto* objectStorage = std::bit_cast<const WriteBarrierBase<Unknown>*>(&object);
    const auto* butterflyStorage = *std::bit_cast<const WriteBarrierBase<Unknown>* const*>(std::bit_cast<const char*>(&object) + JSObject::butterflyOffset());
    std::span<PreparedProperty> properties = std::span(m_preparedProperties).subspan(range.firstPropertyIndex, range.propertyCount);

    CharType* out = buffer();
    unsigned length = m_length;
    unsigned capacity = m_capacity;
    unsigned objectStart = length;
    bool wroteProperty = false;

    for (const PreparedProperty& property : properties) {
        const auto* storageBase = property.storageIndex >= 0 ? objectStorage : butterflyStorage;
        JSValue value = storageBase[property.storageIndex].get();
        if (value.isUndefined()) [[unlikely]]
            continue;

        if (value.isInt32()) {
            if (!appendPreparedObjectKey<hasGap>(property, wroteProperty, newLineAndIndent, maxInt32StringLength, out, length, capacity)) [[unlikely]]
                return;
            if constexpr (sizeof(CharType) == 1) {
                char* cursor = std::bit_cast<char*>(out) + length;
                auto result = std::to_chars(cursor, cursor + maxInt32StringLength, value.asInt32());
                ASSERT(result.ec != std::errc::value_too_large);
                length += result.ptr - cursor;
            } else {
                m_length = length;
                appendInt32(value.asInt32());
                length = m_length;
            }
            continue;
        }

        if constexpr (std::same_as<CharType, Latin1Character>) {
            if (value.isCell() && value.asCell()->type() == StringType) [[likely]] {
                auto valueString = asString(value.asCell())->tryGetView();
                if (!valueString.data.isNull() && valueString.data.is8Bit()) [[likely]] {
                    unsigned valueLength = valueString.data.length();
                    if (!appendPreparedObjectKey<hasGap>(property, wroteProperty, newLineAndIndent, 1 + valueLength + 1, out, length, capacity)) [[unlikely]]
                        return;
                    out[length] = '"';
                    if (!stringCopySameType<CharType, useShortCopyTier>(valueString.data.span8(), out + length + 1)) [[likely]] {
                        out[length + 1 + valueLength] = '"';
                        length += 1 + valueLength + 1;
                        continue;
                    }
                    m_length = length;
                    append<hasGap>(value);
                    if (haveFailure()) [[unlikely]]
                        return;
                    out = buffer();
                    length = m_length;
                    capacity = m_capacity;
                    continue;
                }
            }
        }

        if (value.isDouble()) {
            double number = value.asDouble();
            if (std::isfinite(number)) [[likely]] {
                if (!appendPreparedObjectKey<hasGap>(property, wroteProperty, newLineAndIndent, numberToShortestStringBufferSize, out, length, capacity)) [[unlikely]]
                    return;
                if constexpr (sizeof(CharType) == 1)
                    length += WTF::numberToShortestString(number, std::span<char, numberToShortestStringBufferSize> { std::bit_cast<char*>(out) + length, numberToShortestStringBufferSize });
                else {
                    std::array<char, numberToShortestStringBufferSize> temporary;
                    size_t formattedLength = WTF::numberToShortestString(number, temporary);
                    WTF::copyElements(std::span { reinterpret_cast<uint16_t*>(out + length), formattedLength }, byteCast<uint8_t>(std::span { temporary }).first(formattedLength));
                    length += formattedLength;
                }
                continue;
            }
        }

        if (!appendPreparedObjectKey<hasGap>(property, wroteProperty, newLineAndIndent, 0, out, length, capacity)) [[unlikely]]
            return;
        m_length = length;
        append<hasGap>(value);
        if (haveFailure()) [[unlikely]]
            return;
        out = buffer();
        length = m_length;
        capacity = m_capacity;
    }

    leaveHolder<hasGap>();
    if constexpr (hasGap == HasGap::No) {
        if (length == objectStart) [[unlikely]] {
            if (capacity - length < 2) [[unlikely]] {
                m_length = length;
                if (!hasRemainingCapacitySlow(2)) [[unlikely]] {
                    recordBufferFull();
                    return;
                }
                out = buffer();
            }
            out[length] = '{';
            out[length + 1] = '}';
            m_length = length + 2;
            return;
        }
        if (capacity == length) [[unlikely]] {
            m_length = length;
            if (!hasRemainingCapacitySlow(1)) [[unlikely]] {
                recordBufferFull();
                return;
            }
            out = buffer();
        }
        // If property 0 was undefined, the first written property started with ','; overwrite it with '{'.
        out[objectStart] = '{';
        out[length] = '}';
        m_length = length + 1;
    } else {
        m_length = length;
        bool needNewLine = wroteProperty;
        if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        if (needNewLine)
            appendNewLineAndIndentUnchecked();
        buffer()[m_length++] = '}';
    }
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendUnpreparedObject(JSObject& object, StructureID structureID)
{
    if (object.isCallable()) [[unlikely]] {
        recordFailure("callable object"_s);
        return;
    }
    auto& structure = *object.structure();
    if (structure.hasPolyProto()) [[unlikely]] {
        recordFailure("hasPolyProto"_s);
        return;
    }
    if (structure.storedPrototype() != m_globalObject.objectPrototype()) {
        if (object.type() != FinalObjectType) [[unlikely]] {
            recordFailure("non-final object with non-standard prototype"_s);
            return;
        }
        if (mayHaveToJSON(object)) [[unlikely]] {
            recordFailure("object may have toJSON"_s);
            return;
        }
    } else if (!m_checkedObjectPrototype) {
        if (mayHaveToJSON(*m_globalObject.objectPrototype())) [[unlikely]] {
            recordFailure("object prototype may have toJSON"_s);
            return;
        }
        m_checkedObjectPrototype = true;
    }
    if (!hasRemainingCapacity()) [[unlikely]] {
        recordBufferFull();
        return;
    }
    buffer()[m_length++] = '{';
    if (!structure.canPerformFastPropertyEnumeration()) [[unlikely]] {
        recordFastPropertyEnumerationFailure(object);
        return;
    }
    // A non-reified static table can hold a toJSON that is not in the property table yet.
    if (structure.hasNonReifiedStaticProperties()) [[unlikely]] {
        recordFailure("object has non-reified static properties"_s);
        return;
    }
    if (!enterHolder<hasGap>()) [[unlikely]]
        return;
    const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
    // The Structure tracks whether any property is non-enumerable or keyed by a symbol, so the common case can
    // skip reading each entry's attributes and testing its key.
    const bool mayHaveNonEnumerableOrSymbolProperties = !structure.isQuickPropertyAccessAllowedForEnumeration();
    const unsigned preparedPropertyScratchBase = m_preparedPropertyScratchCount;
#if USE(BUN_JSC_ADDITIONS)
    if (!m_hasClearedStructureTables) {
        m_preparedStructures.fill(nullptr);
        m_structuresMetOnce.fill(0);
        m_hasClearedStructureTables = true;
    }
    uint32_t& metOnce = m_structuresMetOnce[preparedStructureSlot(structureID)];
    bool canCacheStructure = m_isElementOfList || metOnce == structureID.bits();
    m_isElementOfList = false;
    metOnce = structureID.bits();
#else
    bool canCacheStructure = true;
#endif
    bool wroteProperty = false;
    structure.forEachProperty(m_vm, [&](const auto& entry) -> bool {
        if (mayHaveNonEnumerableOrSymbolProperties) [[unlikely]] {
            if (entry.attributes() & PropertyAttribute::DontEnum) {
                // https://tc39.es/ecma262/#sec-serializejsonproperty
                // Step 2.a's GetV finds an own toJSON regardless of enumerability, so a
                // non-enumerable one still applies even though this loop skips the entry.
                // An enumerable toJSON needs no check.
                // 1. If it is callable, appending its value below fails the fast path.
                // 2. If it is not callable the spec serializes it as an ordinary property.
                if (entry.key() == m_vm.propertyNames->toJSON) [[unlikely]] {
                    recordFailure("object has non-enumerable toJSON"_s);
                    return false;
                }
                return true;
            }
            if (entry.key()->isSymbol()) {
                recordFailure("symbol"_s);
                return false;
            }
        }
        auto& name = *entry.key();

        // Right now, we do not support 16-bit name here since name in 16-bit is significantly more rare than 16-bit string.
        // FIXME: We should just extract the `StringType` case into a helper and use that.
        if (!name.is8Bit()) [[unlikely]] {
            recordFailure("16-bit property name"_s);
            return false;
        }
        auto span = name.span8();

        // The structure cannot transition mid-iteration on FastStringifier's case.
        // canPerformFastPropertyEnumeration ruled out getters/setters and
        // mayHaveToJSON ruled out a toJSON / getter / Proxy on the prototype chain,
        // so there is no JS observable to mutate the structure.
        ASSERT(object.structure() == &structure);

        JSValue value = object.getDirect(entry.offset());
        if (value.isUndefined()) {
            canCacheStructure = false;
            return true;
        }

        if (!hasRemainingCapacity(wroteProperty + newLineAndIndent + 1 + span.size() + 2 + (hasGap == HasGap::Yes))) [[unlikely]] {
            recordBufferFull();
            return false;
        }

        {
            // m_dynamicBuffer has inline capacity, so buffer() can point inside this object
            // next to m_length: stores through it may-alias the members, forcing the compiler
            // to reload both after every store. Cache them instead. Scoped because the pointer
            // dies at the next hasRemainingCapacity(), which can reallocate.
            CharType* out = buffer();
            unsigned length = m_length;

            if (wroteProperty)
                out[length++] = ',';

            if constexpr (hasGap == HasGap::Yes) {
                // The callee reads m_length and advances it.
                m_length = length;
                appendNewLineAndIndentUnchecked();
                out = buffer();
                length = m_length;
            }

            out[length] = '"';

            // Publish m_length before every bail-out.
            if constexpr (std::same_as<CharType, char16_t>) {
                if (stringCopyUpconvert(span, out + length + 1)) [[unlikely]] {
                    m_length = length;
                    recordFailure("property name character needs escaping"_s);
                    return false;
                }
            } else {
                if (stringCopySameType<CharType, useShortCopyTier>(span, out + length + 1)) [[unlikely]] {
                    m_length = length;
                    recordFailure("property name character needs escaping"_s);
                    return false;
                }
            }

            out[length + 1 + span.size()] = '"';
            out[length + 1 + span.size() + 1] = ':';
            length += 1 + span.size() + 2;
            if constexpr (hasGap == HasGap::Yes)
                out[length++] = ' ';

            if (canCacheStructure) {
                // Without a gap, 4 is the number of characters in `{"":` and `,"":`.
                // With a gap, 4 is the number of characters in `"": ` and `{`/`,` aren't part of the slice.
                unsigned keyLength = span.size() + 4;
                int storageIndex = indexRelativeToBase(entry.offset());
                if (m_preparedPropertyScratchCount - preparedPropertyScratchBase < maxPropertiesPerPreparedStructure
                    && m_preparedPropertyScratchCount < maxPreparedPropertyScratch
                    && keyLength <= std::numeric_limits<uint16_t>::max()
                    && storageIndex >= std::numeric_limits<int16_t>::min()
                    && storageIndex <= std::numeric_limits<int16_t>::max()) [[likely]] {
                    unsigned keyOffset = length - keyLength;
                    m_preparedPropertyScratch[m_preparedPropertyScratchCount++] = {
                        keyOffset,
                        static_cast<uint16_t>(keyLength),
                        static_cast<int16_t>(storageIndex)
                    };
                } else
                    canCacheStructure = false;
            }

            wroteProperty = true;
            // Mandatory, not bookkeeping: the next hasRemainingCapacity() sizes itself from
            // m_length, so leaving a stale value here would under-count the space needed.
            m_length = length;
        }

        if constexpr (std::same_as<CharType, Latin1Character>) {
            // Inlining String case here since it is too common.
            if (value.isCell() && value.asCell()->type() == StringType) [[likely]] {
                auto valueString = asString(value.asCell())->tryGetView();
                if (!valueString.data.isNull() && valueString.data.is8Bit()) [[likely]] {
                    unsigned valueLength = valueString.data.length();
                    if (!hasRemainingCapacity(1 + valueLength + 1)) [[unlikely]] {
                        recordBufferFull();
                        return false;
                    }
                    buffer()[m_length] = '"';
                    if (!stringCopySameType<CharType, useShortCopyTier>(valueString.data.span8(), buffer() + m_length + 1)) [[likely]] {
                        buffer()[m_length + 1 + valueLength] = '"';
                        m_length += 1 + valueLength + 1;
                        return true;
                    }
                }
            }
        }

        // Also Int32 case is too common.
        if (value.isInt32()) {
            if (!hasRemainingCapacity(maxInt32StringLength)) [[unlikely]] {
                recordBufferFull();
                return false;
            }
            appendInt32(value.asInt32());
            return true;
        }

        append<hasGap>(value);
        return !haveFailure();
    });
    if (haveFailure()) [[unlikely]]
        return;
    // Because nothing is pushed between here and the copy below, the popped entries stay intact until then.
    const unsigned preparedPropertyCount = m_preparedPropertyScratchCount - preparedPropertyScratchBase;
    m_preparedPropertyScratchCount = preparedPropertyScratchBase;
    leaveHolder<hasGap>();
    bool needNewLine = hasGap == HasGap::Yes && wroteProperty;
    if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    if (needNewLine)
        appendNewLineAndIndentUnchecked();
    buffer()[m_length++] = '}';

    if (canCacheStructure && m_preparedStructureCount < preparedStructureTableSize / 2 && m_preparedPropertyCount + preparedPropertyCount <= maxPreparedProperties) {
        unsigned slot = preparedStructureSlot(structureID);
        while (Structure* cachedStructure = m_preparedStructures[slot]) {
            if (cachedStructure->id() == structureID)
                return;
            slot = (slot + 1) & (preparedStructureTableSize - 1);
        }
        uint16_t firstPropertyIndex = m_preparedPropertyCount;
        for (unsigned i = 0; i < preparedPropertyCount; ++i)
            m_preparedProperties[firstPropertyIndex + i] = m_preparedPropertyScratch[preparedPropertyScratchBase + i];
        m_preparedPropertyCount += preparedPropertyCount;
        ++m_preparedStructureCount;
        m_preparedStructureRanges[slot] = { firstPropertyIndex, static_cast<uint16_t>(preparedPropertyCount) };
        m_preparedStructures[slot] = structureID.decode();
    }
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
void FastStringifier<CharType, bufferMode>::append(JSValue value)
{
    if constexpr (growsBuffer) {
        if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
#if ENABLE(AOT)
            if (!hasStackSpaceAtWatchedLevel(value, currentStackPointer()))
                return;
#else
            recordFailure(FailureReason::StackOverflow, "stack overflow"_s);
            return;
#endif
        }
    }

    if (value.isNull()) {
        append('n', 'u', 'l', 'l');
        return;
    }

    if (value.isTrue()) {
        append('t', 'r', 'u', 'e');
        return;
    }

    if (value.isFalse()) {
        append('f', 'a', 'l', 's', 'e');
        return;
    }

    if (value.isInt32()) {
        if (!hasRemainingCapacity(maxInt32StringLength)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        appendInt32(value.asInt32());
        return;
    }

    if (value.isDouble()) {
        auto number = value.asDouble();
        if (!std::isfinite(number)) {
            append('n', 'u', 'l', 'l');
            return;
        }
        if (!hasRemainingCapacity(numberToShortestStringBufferSize)) [[unlikely]] {
            recordBufferFull();
            return;
        }
#if USE(BUN_JSC_ADDITIONS)
        static_assert(maxInt32StringLength <= numberToShortestStringBufferSize);
        if (auto int32Value = tryConvertToStrictInt32(number)) {
            appendInt32(*int32Value);
            return;
        }
#endif
        if constexpr (sizeof(CharType) == 1)
            m_length += WTF::numberToShortestString(number, byteCast<char>(bufferSpan().subspan(m_length)).template first<numberToShortestStringBufferSize>());
        else {
            std::array<char, numberToShortestStringBufferSize> temporary;
            size_t length = WTF::numberToShortestString(number, temporary);
            WTF::copyElements(spanReinterpretCast<uint16_t>(bufferSpan().subspan(m_length)), byteCast<uint8_t>(std::span { temporary }).first(length));
            m_length += length;
        }
        return;
    }

    if (!value.isCell()) [[unlikely]] {
        recordFailure("value type"_s);
        return;
    }
    auto& cell = *value.asCell();

    switch (cell.type()) {
    case StringType: {
        auto string = asString(&cell)->tryGetView();
        if (string.data.isNull()) [[unlikely]] {
            recordFailure("String::tryGetValue"_s);
            return;
        }
        auto stringLength = string.data.length();
        if (!hasRemainingCapacity(1 + stringLength + 1)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        buffer()[m_length] = '"';
        if constexpr (sizeof(CharType) == 2) {
            if (!m_hasSeenWideString && !string.data.is8Bit())
                m_hasSeenWideString = !WTF::charactersAreAllLatin1(string.data.span16());
        }
        if (stringLength >= lengthOfLongString) {
            ++m_length;
            if (string.data.is8Bit() ? appendContentOfLongString(string.data.span8()) : appendContentOfLongString(string.data.span16())) [[likely]]
                buffer()[m_length++] = '"';
            return;
        }

        if constexpr (std::same_as<CharType, Latin1Character>) {
            if (string.data.is8Bit()) [[likely]] {
                if (!stringCopySameType<CharType, useShortCopyTier>(string.data.span8(), buffer() + m_length + 1)) [[likely]] {
                    buffer()[m_length + 1 + stringLength] = '"';
                    m_length += 1 + stringLength + 1;
                    return;
                }
            }
        } else {
            static_assert(std::same_as<CharType, char16_t>);
            if (string.data.is8Bit()) {
                if (!stringCopyUpconvert(string.data.span8(), buffer() + m_length + 1)) [[likely]] {
                    buffer()[m_length + 1 + stringLength] = '"';
                    m_length += 1 + stringLength + 1;
                    return;
                }
            } else {
                if (!stringCopySameType(string.data.span16(), buffer() + m_length + 1)) [[likely]] {
                    buffer()[m_length + 1 + stringLength] = '"';
                    m_length += 1 + stringLength + 1;
                    return;
                }
            }
        }

        auto escapedLength = 1 + CheckedUint32 { stringLength } * 6 + 1;
        if (escapedLength.hasOverflowed()) [[unlikely]] {
            recordBufferFull();
            return;
        }
        if (!hasRemainingCapacity(escapedLength.value())) [[unlikely]] {
            recordBufferFull();
            return;
        }

        ASSERT(buffer()[m_length] == '"');
        auto output = bufferSpan().subspan(m_length + 1);
        if constexpr (std::same_as<CharType, char16_t>) {
            if (string.data.is8Bit())
                WTF::appendEscapedJSONStringContent(output, string.data.span8());
            else
                WTF::appendEscapedJSONStringContent(output, string.data.span16());
        } else {
            if (string.data.is8Bit())
                WTF::appendEscapedJSONStringContent(output, string.data.span8());
            else {
                // It's possible the UTF-16 string is actually Latin-1. We try to avoid that but bailing out here is _way_ more expensive if it does sometimes happen.
                bool success = WTF::appendEscapedJSONStringContent(output, string.data.span16());
                if (!success) [[unlikely]] {
                    recordWideString();
                    return;
                }
            }
        }

        consume(output) = '"';
        m_length = output.data() - buffer();
        return;
    }

    case ObjectType:
    case FinalObjectType: {
        auto& object = *asObject(&cell);
        StructureID structureID = object.structureID();
        unsigned slot = preparedStructureSlot(structureID);
        while (Structure* cachedStructure = preparedStructure(slot)) {
            if (cachedStructure->id() == structureID) [[likely]] {
                appendPreparedObject<hasGap>(object, slot);
                return;
            }
            slot = (slot + 1) & (preparedStructureTableSize - 1);
        }
        appendUnpreparedObject<hasGap>(object, structureID);
        return;
    }

    case ArrayType: {
        auto& array = *asArray(&cell);
        if (!m_checkedArrayPrototype) {
            if (mayHaveToJSON(*m_globalObject.arrayPrototype())) [[unlikely]] {
                recordFailure("array prototype may have toJSON"_s);
                return;
            }
            m_checkedArrayPrototype = true;
        }
        auto& structure = *array.structure();
        if (!m_globalObject.isOriginalArrayStructure(&structure)) [[unlikely]] {
            if (structure.hasPolyProto()) [[unlikely]] {
                recordFailure("hasPolyProto"_s);
                return;
            }
            if (structure.storedPrototype() != m_globalObject.arrayPrototype()) [[unlikely]] {
                recordFailure("non-standard array prototype"_s);
                return;
            }
            structure.forEachProperty(m_vm, [&](const PropertyTableEntry& entry) -> bool {
                if (entry.key() == m_vm.propertyNames->toJSON) [[unlikely]] {
                    recordFailure("array has toJSON"_s);
                    return false;
                }
                return true;
            });
            if (haveFailure())
                return;
        }
        if (!hasRemainingCapacity()) [[unlikely]] {
            recordBufferFull();
            return;
        }
        buffer()[m_length++] = '[';
        if (!enterHolder<hasGap>()) [[unlikely]]
            return;

        auto closeArray = [&] {
            leaveHolder<hasGap>();
            bool needNewLine = hasGap == HasGap::Yes && buffer()[m_length - 1] != '[';
            if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
                recordBufferFull();
                return;
            }
            if (needNewLine)
                appendNewLineAndIndentUnchecked();
            buffer()[m_length++] = ']';
        };

        IndexingType indexingType = array.indexingType();
        if (hasInt32(indexingType)) {
            appendInt32Array<hasGap>(array);
            if (haveFailure()) [[unlikely]]
                return;
            closeArray();
            return;
        }

        if (hasContiguous(indexingType)) {
            auto* butterfly = array.butterfly();
            unsigned length = butterfly->publicLength();
            if (length > butterfly->vectorLength()) [[unlikely]] {
                recordFailure("!lengthBeyondVector"_s);
                return;
            }
#if ENABLE(AOT)
            if constexpr (hasGap == HasGap::No) {
                JSValue first = length ? butterfly->contiguous().at(&array, 0).get() : JSValue();
                if (first && first.isCell() && first.asCell()->type() == CompiledObjectType) [[unlikely]] {
                    appendElementsInCompiledProgram(array);
                    if (haveFailure()) [[unlikely]]
                        return;
                    closeArray();
                    return;
                }
            }
#endif
            auto data = butterfly->contiguous();
            const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
            Structure* lastStructure { };
            unsigned lastStructureSlot = 0;
            for (unsigned i = 0; i < length; ++i) {
                if (i || hasGap == HasGap::Yes) {
                    if (!hasRemainingCapacity(!!i + newLineAndIndent)) [[unlikely]] {
                        recordBufferFull();
                        return;
                    }
                    if (i)
                        buffer()[m_length++] = ',';
                    if constexpr (hasGap == HasGap::Yes)
                        appendNewLineAndIndentUnchecked();
                }
                JSValue element = data.at(&array, i).get();
                if (!element) [[unlikely]] {
                    recordFailure("!canGetIndexQuickly"_s);
                    return;
                }
                if (m_preparedStructureCount && element.isCell()) {
                    JSCell* elementCell = element.asCell();
                    StructureID structureID = elementCell->structureID();
                    if (structureID.decode() == lastStructure) [[likely]] {
                        appendPreparedObject<hasGap>(*asObject(elementCell), lastStructureSlot);
                        if (haveFailure()) [[unlikely]]
                            return;
                        continue;
                    }
                    unsigned slot = preparedStructureSlot(structureID);
                    while (Structure* cachedStructure = m_preparedStructures[slot]) {
                        if (cachedStructure->id() == structureID) [[likely]] {
                            lastStructure = structureID.decode();
                            lastStructureSlot = slot;
                            appendPreparedObject<hasGap>(*asObject(elementCell), slot);
                            if (haveFailure()) [[unlikely]]
                                return;
                            goto nextContiguousElement;
                        }
                        slot = (slot + 1) & (preparedStructureTableSize - 1);
                    }
                }
#if USE(BUN_JSC_ADDITIONS)
                m_isElementOfList = length > 1;
                append<hasGap>(element.isUndefined() ? jsNull() : element);
                m_isElementOfList = false;
#else
                append<hasGap>(element.isUndefined() ? jsNull() : element);
#endif
                if (haveFailure()) [[unlikely]]
                    return;
            nextContiguousElement:
            }
            closeArray();
            return;
        }
        const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
        for (unsigned i = 0, length = array.length(); i < length; ++i) {
            if (i || hasGap == HasGap::Yes) {
                if (!hasRemainingCapacity(!!i + newLineAndIndent)) [[unlikely]] {
                    recordBufferFull();
                    return;
                }
                if (i)
                    buffer()[m_length++] = ',';
                if constexpr (hasGap == HasGap::Yes)
                    appendNewLineAndIndentUnchecked();
            }
            if (!array.canGetIndexQuickly(i)) [[unlikely]] {
                recordFailure("!canGetIndexQuickly"_s);
                return;
            }
            JSValue element = array.getIndexQuickly(i);
            append<hasGap>(element.isUndefined() ? jsNull() : element);
            if (haveFailure()) [[unlikely]]
                return;
        }
        closeArray();
        return;
    }

    case JSFunctionType:
        recordFailure("function"_s);
        return;

    default:
#if ENABLE(AOT)
        // These are not separate cases, so the switch compiles as it does without them, and a process without compiled code checks only after the switch has failed.
        if (cell.type() == CompiledObjectType) [[unlikely]] {
            appendCompiledObject<hasGap>(*asObject(&cell));
            return;
        }
        if (cell.type() == JSDateType && m_vm.hasCompiledCode()) [[unlikely]] {
            appendDate(*uncheckedDowncast<DateInstance>(&cell));
            return;
        }
#endif
        recordFailure("object type"_s);
    }
}

#if ENABLE(AOT)
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendDate(DateInstance& date)
{
    Structure* structure = date.structure();
    if (structure->realm() != &m_globalObject || structure != m_globalObject.dateStructure() || !datesHaveBuiltinToJSON(*asObject(structure->storedPrototype()))) [[unlikely]] {
        recordFailure("date that may have its own toJSON"_s);
        return;
    }
    if (!hasRemainingCapacity(1 + maxISODateTimeLength + 1)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    double time = date.internalNumber();
    if (!std::isfinite(time)) {
        append('n', 'u', 'l', 'l');
        return;
    }
    if (m_vm.aotState().realmWhoseDatesAreWrittenInPlace != &m_globalObject) [[unlikely]] {
        if (!readOwnMember(*asObject(structure->storedPrototype()), m_vm.propertyNames->toISOString)) {
            recordFailure("date that may have its own toJSON"_s);
            return;
        }
        m_vm.aotState().realmWhoseDatesAreWrittenInPlace = &m_globalObject;
    }
    auto* cursor = buffer() + m_length;
    *cursor++ = '"';
    cursor = writeISODateTime(cursor, time);
    *cursor++ = '"';
    m_length = cursor - buffer();
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::datesHaveBuiltinToJSON(JSObject& datePrototype)
{
    // This realm is recorded only after one of its dates passed all of the checks below.
    if (m_vm.aotState().realmWhoseDatesAreWrittenInPlace == &m_globalObject) [[likely]]
        return true;
    if (m_datesHaveBuiltinToJSON != TriState::Indeterminate)
        return m_datesHaveBuiltinToJSON == TriState::True;
    bool result = true;
    auto& names = *m_vm.propertyNames;
    for (auto* name : { names.toJSON.impl(), names.toISOString.impl(), names.valueOf.impl(), names.toPrimitiveSymbol.impl() })
        result &= datePrototype.structure()->isImmutablePropertyForReader(name);
    // The prototype reifies a member on first read. The generic path reads these two for every date, in this order, and toISOString for a valid one.
    if (result && m_vm.aotState().realmWhoseDatesAreWrittenInPlace != &m_globalObject)
        result = readOwnMember(datePrototype, names.toJSON) && readOwnMember(datePrototype, names.valueOf);
    m_datesHaveBuiltinToJSON = triState(result);
    return result;
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::readOwnMember(JSObject& object, PropertyName name)
{
    PropertySlot slot(&object, PropertySlot::InternalMethodType::GetOwnProperty);
    return JSObject::getOwnPropertySlot(&object, &m_globalObject, name, slot);
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendCompiledObject(JSObject& object)
{
    if constexpr (hasGap == HasGap::No && std::same_as<CharType, Latin1Character>) {
        if (AOT::PropertyWalk::hasExtraProperties(&object) && tryAppendObjectWithExtraProperties(object))
            return;
    }
    // All of these checks depend only on the Structure. The remembered Structure has already passed them.
    if (!isCompiledStructureKnown(object)) {
        if (object.isCallable()) [[unlikely]] {
            recordFailure("callable object"_s);
            return;
        }
        auto& structure = *object.structure();
        if (structure.hasPolyProto()) [[unlikely]] {
            recordFailure("hasPolyProto"_s);
            return;
        }
        if (structure.hasNonReifiedStaticProperties()) [[unlikely]] {
            recordFailure("object has non-reified static properties"_s);
            return;
        }
        if (mayCompiledObjectHaveToJSON(object)) [[unlikely]] {
            recordFailure("compiled object may have toJSON"_s);
            return;
        }
    }
    if (auto fields = AOT::PropertyWalk::fieldsInKeyOrder(&object)) [[likely]] {
        appendListedFields<hasGap>(object, *fields);
        return;
    }
    if (!hasRemainingCapacity()) [[unlikely]] {
        recordBufferFull();
        return;
    }
    buffer()[m_length++] = '{';
    if (!enterHolder<hasGap>()) [[unlikely]]
        return;
    const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
    auto appendProperty = [&](UniquedStringImpl& name, JSValue value) ALWAYS_INLINE_LAMBDA -> bool {
        if (name.isSymbol()) [[unlikely]] {
            recordFailure("symbol"_s);
            return false;
        }

        if (!name.is8Bit()) [[unlikely]] {
            recordFailure("16-bit property name"_s);
            return false;
        }
        auto span = name.span8();

        if (value.isUndefined())
            return true;

        bool needComma = buffer()[m_length - 1] != '{';
        if (!hasRemainingCapacity(needComma + newLineAndIndent + 1 + span.size() + 2 + (hasGap == HasGap::Yes))) [[unlikely]] {
            recordBufferFull();
            return false;
        }

        {
            CharType* out = buffer();
            unsigned length = m_length;

            if (needComma)
                out[length++] = ',';

            if constexpr (hasGap == HasGap::Yes) {
                m_length = length;
                appendNewLineAndIndentUnchecked();
                out = buffer();
                length = m_length;
            }

            out[length] = '"';

            if constexpr (std::same_as<CharType, char16_t>) {
                if (stringCopyUpconvert(span, out + length + 1)) [[unlikely]] {
                    m_length = length;
                    recordFailure("property name character needs escaping"_s);
                    return false;
                }
            } else {
                if (stringCopySameType<CharType, useShortCopyTier>(span, out + length + 1)) [[unlikely]] {
                    m_length = length;
                    recordFailure("property name character needs escaping"_s);
                    return false;
                }
            }

            out[length + 1 + span.size()] = '"';
            out[length + 1 + span.size() + 1] = ':';
            length += 1 + span.size() + 2;
            if constexpr (hasGap == HasGap::Yes)
                out[length++] = ' ';

            m_length = length;
        }

        if constexpr (std::same_as<CharType, Latin1Character>) {
            if (value.isCell() && value.asCell()->type() == StringType) [[likely]] {
                auto valueString = asString(value.asCell())->tryGetView();
                if (!valueString.data.isNull() && valueString.data.is8Bit()) [[likely]] {
                    unsigned valueLength = valueString.data.length();
                    if (!hasRemainingCapacity(1 + valueLength + 1)) [[unlikely]] {
                        recordBufferFull();
                        return false;
                    }
                    buffer()[m_length] = '"';
                    if (!stringCopySameType<CharType, useShortCopyTier>(valueString.data.span8(), buffer() + m_length + 1)) [[likely]] {
                        buffer()[m_length + 1 + valueLength] = '"';
                        m_length += 1 + valueLength + 1;
                        return true;
                    }
                }
            }
        }

        if (value.isInt32()) {
            if (!hasRemainingCapacity(maxInt32StringLength)) [[unlikely]] {
                recordBufferFull();
                return false;
            }
            appendInt32(value.asInt32());
            return true;
        }

        append<hasGap>(value);
        return !haveFailure();
    };
    auto result = AOT::PropertyWalk::forEachOwnProperty(m_vm, &object, AOT::PropertyWalk::Filter::EnumerableStrings, [&](const AOT::PropertyWalk::OwnProperty& property) {
        return appendProperty(*property.name, property.value) ? IterationStatus::Continue : IterationStatus::Done;
    });
    if (result == AOT::PropertyWalk::Result::NeedsGenericPath)
        recordFailure("compiled object has an accessor or an index"_s);
    if (haveFailure()) [[unlikely]]
        return;
    leaveHolder<hasGap>();
    bool needNewLine = hasGap == HasGap::Yes && buffer()[m_length - 1] != '{';
    if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    if (needNewLine)
        appendNewLineAndIndentUnchecked();
    buffer()[m_length++] = '}';
}

struct ListedPropertiesWritten {
    enum class Status : uint8_t { All, NeedsRoom, FoundOtherValue, FoundAbsentField };

    Status status;
    unsigned index;
    Latin1Character* next;
    size_t roomNeeded { 0 };
    JSValue otherValue { };
};

static const AOT::CachedPropertyList* listOfPropertiesForJSON(VM& vm, JSObject& object, JSObject*& extraProperties)
{
    Structure& structure = *object.structure();
    if (object.isCallable() || structure.hasPolyProto() || structure.hasNonReifiedStaticProperties()) [[unlikely]]
        return nullptr;
    const AOT::CachedPropertyList* list = AOT::PropertyWalk::tryGetOwnPropertyList(vm, &object, extraProperties);
    return list && !list->hasToJSON && !list->endsOfJSONKeys.isEmpty() ? list : nullptr;
}

// Writes the listed properties starting at `index`, each preceded by a comma, and stops at the first one that needs more than its key, the space up to the limit, and a number, boolean, null or flat 8-bit string.
// If the value is left to the caller, the key has been written. If the property needs more space, or the field is absent, nothing has been written.
static NEVER_INLINE ListedPropertiesWritten writeListedProperties(const JSObject& object, JSObject* extraProperties, const AOT::CachedPropertyList& list, unsigned index, Latin1Character* next, const Latin1Character* limit)
{
    using Status = ListedPropertiesWritten::Status;
    constexpr size_t blockSize = AOT::FieldAsProperty::jsonKeyBlockSize;
    const AOT::PropertyListEntry* properties = list.properties.span().data();
    const uint32_t* endsOfKeys = list.endsOfJSONKeys.span().data();
    const Latin1Character* keys = list.jsonKeys.span().data();
    unsigned count = list.properties.size();
    for (; index < count; ++index) {
        JSValue value = AOT::PropertyWalk::readListedProperty(&object, properties[index], extraProperties);
        if (!value) [[unlikely]]
            return { Status::FoundAbsentField, index, next };
        if (value.isUndefined()) [[unlikely]]
            continue;
        uint32_t startOfKey = index ? endsOfKeys[index - 1] : 0;
        const Latin1Character* key = keys + startOfKey;
        size_t keySize = endsOfKeys[index] - startOfKey;
        size_t room = limit - next;
        size_t roomForScalar = keySize + blockSize + AOT::maxJSONNumberLength;
        if (room < roomForScalar) [[unlikely]]
            return { Status::NeedsRoom, index, next, roomForScalar };
        memcpy(next, key, blockSize);
        if (keySize > blockSize) [[unlikely]] {
            for (size_t offset = blockSize; offset < keySize; offset += blockSize)
                memcpy(next + offset, key + offset, blockSize);
        }
        Latin1Character* afterKey = next + keySize;
        if (value.isInt32())
            next = AOT::appendJSONInteger(afterKey, value.asInt32());
        else if (value.isCell()) {
            const StringImpl* string = value.asCell()->type() == StringType ? asString(value.asCell())->tryGetValueImpl() : nullptr;
            if (!string || !string->is8Bit()) [[unlikely]]
                return { Status::FoundOtherValue, index, afterKey, 0, value };
            size_t length = string->length();
            if (room < keySize + 1 + length + 1) [[unlikely]]
                return { Status::NeedsRoom, index, next, keySize + 1 + length + 1 };
            *afterKey = '"';
            if (stringCopySameType<Latin1Character, true>(string->span8(), afterKey + 1)) [[unlikely]] {
                // Reads from `value` again, so that nothing else has to stay in a register across the copy.
                auto characters = asString(value.asCell())->tryGetValueImpl()->span8();
                if (static_cast<size_t>(limit - afterKey) < AOT::maxEscapedJSONStringLength(characters.size()))
                    return { Status::FoundOtherValue, index, afterKey, 0, value };
                next = AOT::appendQuotedAndEscapedJSONString(afterKey, characters);
                continue;
            }
            afterKey[1 + length] = '"';
            next = afterKey + 1 + length + 1;
        } else if (value.isDouble())
            next = AOT::appendJSONNumber(afterKey, value.asDouble());
        else if (value.isBoolean())
            next = AOT::appendJSONBoolean(afterKey, value.isTrue());
        else if (value.isNull())
            next = AOT::appendJSONLiteral(afterKey, "null"_s);
        else
            return { Status::FoundOtherValue, index, afterKey, 0, value };
    }
    return { Status::All, index, next };
}

// Returns false without writing anything if the object is left to the generic path for compiled objects. Returns true if the object was written or a failure was recorded.
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE bool FastStringifier<CharType, bufferMode>::tryAppendObjectWithExtraProperties(JSObject& object)
{
    if constexpr (std::same_as<CharType, Latin1Character>) {
        using Status = ListedPropertiesWritten::Status;
        // A garbage collection drops the list.
        DeferGC deferGC(m_vm);
        JSObject* extraProperties;
        const AOT::CachedPropertyList* list = listOfPropertiesForJSON(m_vm, object, extraProperties);
        if (!list) [[unlikely]]
            return false;
        JSValue prototype = object.structure()->storedPrototype();
        if (prototype.isObject() && mayHaveToJSON(*asObject(prototype))) [[unlikely]]
            return false;
        if (!enterHolder<HasGap::No>()) [[unlikely]]
            return true;

        unsigned count = list->properties.size();
        // Every key starts with a comma. The first comma lands where the opening brace belongs, and the brace overwrites it at the end.
        unsigned brace = m_length;
        bool hasPublished = false;
        Cursor cursor = cursorAtEnd();
        for (unsigned index = 0;;) {
            if (!reserve(cursor, 2)) [[unlikely]]
                return true;
            ListedPropertiesWritten written = writeListedProperties(object, extraProperties, *list, index, cursor.next, cursor.limit - 2);
            cursor.next = written.next;
            index = written.index;
            if (written.status == Status::All) [[likely]]
                break;
            if (written.status == Status::FoundAbsentField) {
                if (!hasPublished) {
                    leaveHolder<HasGap::No>();
                    return false;
                }
                recordFailure("a listed field of a compiled object is absent"_s);
                return true;
            }
            hasPublished = true;
            if (written.status == Status::NeedsRoom) {
                if (!reserve(cursor, written.roomNeeded + 2)) [[unlikely]]
                    return true;
                continue;
            }
            publishLength(cursor);
            append<HasGap::No>(written.otherValue);
            if (haveFailure()) [[unlikely]]
                return true;
            // The list of a nested object may have replaced it.
            list = listOfPropertiesForJSON(m_vm, object, extraProperties);
            if (!list || list->properties.size() != count) [[unlikely]] {
                recordFailure("the list of a compiled object's properties was replaced"_s);
                return true;
            }
            cursor = cursorAtEnd();
            ++index;
        }
        leaveHolder<HasGap::No>();
        cursor.next += static_cast<size_t>(cursor.next - buffer()) == brace;
        buffer()[brace] = '{';
        *cursor.next++ = '}';
        publishLength(cursor);
        return true;
    } else {
        UNUSED_PARAM(object);
        return false;
    }
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::mayCompiledObjectHaveToJSON(JSObject& object)
{
    Structure* structure = object.structure();
    JSValue cached = structure->cachedSpecialProperty(CachedSpecialPropertyKey::ToJSON);
    if (!cached) {
        // For a compiled Structure this caches a miss that the Structure and its chain prove, and nothing else.
        PropertySlot unsetSlot { &object, PropertySlot::InternalMethodType::Get };
        structure->cacheSpecialProperty(&m_globalObject, m_vm, jsUndefined(), CachedSpecialPropertyKey::ToJSON, unsetSlot);
        cached = structure->cachedSpecialProperty(CachedSpecialPropertyKey::ToJSON);
    }
    if (cached && cached.isUndefined()) {
        m_compiledStructureWithoutToJSON = object.structureID().bits();
        m_compiledStructuresKnown |= knowsCompiledStructure;
        return false;
    }
    return mayHaveOwnOrInheritedToJSON(object);
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::mayHaveOwnOrInheritedToJSON(JSObject& object)
{
    if (AOT::PropertyWalk::hasOwnProperty(&object, m_vm.propertyNames->toJSON.impl()) != AOT::PropertyWalk::Presence::Absent)
        return true;
    JSValue prototype = object.structure()->storedPrototype();
    return prototype.isObject() && mayHaveToJSON(*asObject(prototype));
}

struct FieldsWrittenBySlot {
    Latin1Character* next;
    const AOT::FieldAsProperty* firstFieldNotWritten;
};

// Writes the members of a compiled object that is not in a stack frame, and stops at the first one that needs more than its slot, its key and the reserved space.
static ALWAYS_INLINE FieldsWrittenBySlot writeFieldsBySlotInline(Latin1Character* next, const Latin1Character* limit, const JSObject& object, const AOT::FieldAsProperty* field, const AOT::FieldAsProperty* end, size_t reservation)
{
    constexpr size_t blockSize = AOT::FieldAsProperty::jsonKeyBlockSize;
    for (; field != end; ++field) {
        const Latin1Character* key = field->jsonKey.data();
        const size_t keySize = field->jsonKey.size();
        // Keys are copied in whole blocks, so the copy may read past the end of a key.
        memcpy(next, key, blockSize);
        if (keySize > blockSize) [[unlikely]] {
            for (size_t offset = blockSize; offset < keySize; offset += blockSize)
                memcpy(next + offset, key + offset, blockSize);
        }
        Latin1Character* afterKey = next + keySize;
        const uint8_t* slot = std::bit_cast<const uint8_t*>(&object) + field->offsetForJSON;
        JSCell* cell;
        switch (field->representationForJSON) {
        case AOT::SlotRepresentation::Int32:
            next = AOT::appendJSONInteger(afterKey, WTF::unalignedLoad<int32_t>(slot));
            continue;
        case AOT::SlotRepresentation::OptionalInt32: {
            int32_t word = WTF::unalignedLoad<int32_t>(slot);
            if (word < AOT::minInlineInt32) [[unlikely]]
                return { next, field };
            next = AOT::appendJSONInteger(afterKey, word);
            continue;
        }
        case AOT::SlotRepresentation::Double:
            next = AOT::appendJSONNumber(afterKey, WTF::unalignedLoad<double>(slot));
            continue;
        case AOT::SlotRepresentation::OptionalDouble: {
            // The range check also catches a few other NaNs besides the three sentinels (undefined, null, absent).
            uint64_t bits = WTF::unalignedLoad<uint64_t>(slot);
            if ((bits >> 48) - (AOT::undefinedDouble >> 48) <= (AOT::absentDouble - AOT::undefinedDouble) >> 48) [[unlikely]]
                return { next, field };
            next = AOT::appendJSONNumber(afterKey, std::bit_cast<double>(bits));
            continue;
        }
        case AOT::SlotRepresentation::Int64:
        case AOT::SlotRepresentation::OptionalInt64: {
            uint64_t integer = WTF::unalignedLoad<uint64_t>(slot);
            if (integer >= 100000000) [[unlikely]]
                return { next, field };
            next = AOT::appendJSONInteger(afterKey, static_cast<int64_t>(integer));
            continue;
        }
        case AOT::SlotRepresentation::Boolean:
            next = AOT::appendJSONBoolean(afterKey, *slot);
            continue;
        case AOT::SlotRepresentation::OptionalBoolean: {
            static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
            uint8_t word = *slot;
            if ((word | 1) != JSValue::ValueTrue) [[unlikely]]
                return { next, field };
            next = AOT::appendJSONBoolean(afterKey, word & 1);
            continue;
        }
        case AOT::SlotRepresentation::Cell32:
        case AOT::SlotRepresentation::OptionalCell32: {
            uint32_t word = WTF::unalignedLoad<uint32_t>(slot);
            if (word <= AOT::undefinedCell32) {
                if (word == AOT::nullCell32)
                    next = AOT::appendJSONLiteral(afterKey, "null"_s);
                else if (word != AOT::undefinedCell32 && word != AOT::absentCell32) [[unlikely]]
                    return { next, field };
                continue;
            }
            cell = AOT::PropertyWalk::cellInNarrowSlot(&object, word);
            break;
        }
        case AOT::SlotRepresentation::Cell:
        case AOT::SlotRepresentation::JSValue: {
            JSValue held = JSValue::decode(WTF::unalignedLoad<EncodedJSValue>(slot));
            if (held.isInt32()) {
                next = AOT::appendJSONInteger(afterKey, held.asInt32());
                continue;
            }
            if (!held || held.isUndefined())
                continue;
            if (!held.isCell()) {
                if (held.isDouble())
                    next = AOT::appendJSONNumber(afterKey, held.asDouble());
                else if (held.isBoolean())
                    next = AOT::appendJSONBoolean(afterKey, held.isTrue());
                else if (held.isNull())
                    next = AOT::appendJSONLiteral(afterKey, "null"_s);
                else
                    return { next, field };
                continue;
            }
            cell = held.asCell();
            break;
        }
        case AOT::SlotRepresentation::Uint8:
        case AOT::SlotRepresentation::Int8:
        case AOT::SlotRepresentation::Uint16:
        case AOT::SlotRepresentation::Int16:
        case AOT::SlotRepresentation::Uint32:
        case AOT::SlotRepresentation::Float:
        case AOT::SlotRepresentation::DeferredClosure:
            return { next, field };
        }

        if (cell->type() != StringType)
            break;
        const StringImpl* string = asString(cell)->tryGetValueImpl();
        if (!string || !string->is8Bit()) [[unlikely]]
            break;
        const size_t length = string->length();
        if (static_cast<size_t>(limit - afterKey) < 1 + length + 1 + reservation) [[unlikely]]
            break;
        *afterKey = '"';
        if (stringCopySameType<Latin1Character, true>(string->span8(), afterKey + 1)) [[unlikely]]
            break;
        afterKey[1 + length] = '"';
        next = afterKey + 1 + length + 1;
    }
    return { next, field };
}

static bool areFieldsPlain(std::span<const AOT::FieldAsProperty> fields)
{
    return std::ranges::all_of(fields, [](const AOT::FieldAsProperty& field) {
        bool holdsAnyValue = field.representationForJSON == AOT::SlotRepresentation::JSValue || field.representationForJSON == AOT::SlotRepresentation::Cell;
        return holdsAnyValue && field.jsonKey.size() <= AOT::FieldAsProperty::jsonKeyBlockSize;
    });
}

// writeFieldsBySlotInline() for plain fields: it does not check a field's representation or the length of its key.
static ALWAYS_INLINE FieldsWrittenBySlot writePlainFieldsInline(Latin1Character* next, const Latin1Character* limit, const JSObject& object, const AOT::FieldAsProperty* field, const AOT::FieldAsProperty* end, size_t reservation)
{
    for (; field != end; ++field) {
        memcpy(next, field->jsonKey.data(), AOT::FieldAsProperty::jsonKeyBlockSize);
        Latin1Character* afterKey = next + field->jsonKey.size();
        JSValue held = JSValue::decode(WTF::unalignedLoad<EncodedJSValue>(std::bit_cast<const uint8_t*>(&object) + field->offsetForJSON));
        if (held.isInt32()) {
            next = AOT::appendJSONInteger(afterKey, held.asInt32());
            continue;
        }
        if (!held.isCell()) {
            if (held.isBoolean())
                next = AOT::appendJSONBoolean(afterKey, held.isTrue());
            else if (held.isDouble())
                next = AOT::appendJSONNumber(afterKey, held.asDouble());
            else if (held.isNull())
                next = AOT::appendJSONLiteral(afterKey, "null"_s);
            else if (!held.isUndefined()) [[unlikely]]
                return { next, field };
            continue;
        }
        if (!held) [[unlikely]]
            continue;
        JSCell* cell = held.asCell();
        if (cell->type() != StringType)
            return { next, field };
        const StringImpl* string = asString(cell)->tryGetValueImpl();
        if (!string || !string->is8Bit()) [[unlikely]]
            return { next, field };
        const size_t length = string->length();
        if (static_cast<size_t>(limit - afterKey) < 1 + length + 1 + reservation) [[unlikely]]
            return { next, field };
        *afterKey = '"';
        if (stringCopySameType<Latin1Character, true>(string->span8(), afterKey + 1)) [[unlikely]]
            return { next, field };
        afterKey[1 + length] = '"';
        next = afterKey + 1 + length + 1;
    }
    return { next, field };
}

static NEVER_INLINE FieldsWrittenBySlot writeFieldsBySlot(Latin1Character* next, const Latin1Character* limit, const JSObject& object, const AOT::FieldAsProperty* field, const AOT::FieldAsProperty* end, size_t reservation)
{
    return writeFieldsBySlotInline(next, limit, object, field, end, reservation);
}

struct ElementsWrittenBySlot {
    Latin1Character* next;
    unsigned firstElementNotWritten;
    bool stoppedAtField;
};

// Writes the elements after the first one, each preceded by a comma, as long as they have the same fields and key order as the known object. Nothing is written for the element at which it stops.
template<bool fieldsArePlain>
static NEVER_INLINE ElementsWrittenBySlot writeElementsBySlot(Latin1Character* next, const Latin1Character* limit, const WriteBarrier<Unknown>* elements, unsigned index, unsigned length, const JSObject& known, std::span<const AOT::FieldAsProperty> fields, size_t reservation)
{
    const AOT::FieldAsProperty* const first = fields.data();
    const AOT::FieldAsProperty* const end = first + fields.size();
    const StructureID structureOfKnown = known.structureID();
    const uint32_t orderOfKnown = AOT::PropertyWalk::orderWordWithoutIntegrity(&known);
    for (; index < length; ++index) {
        JSValue element = elements[index].get();
        if (!element || !element.isCell()) [[unlikely]]
            break;
        // It is treated as an object only after its Structure shows that it is one.
        const JSObject& object = *static_cast<const JSObject*>(element.asCell());
        if (object.structureID() != structureOfKnown || AOT::PropertyWalk::orderWordWithoutIntegrity(&object) != orderOfKnown || AOT::PropertyWalk::isInFrame(&object)) [[unlikely]]
            break;
        if (static_cast<size_t>(limit - next) < 1 + reservation) [[unlikely]]
            break;
        *next = ',';
        // Every key starts with a comma. The first comma lands where the opening brace belongs.
        Latin1Character* brace = next + 1;
        FieldsWrittenBySlot written;
        if constexpr (fieldsArePlain)
            written = writePlainFieldsInline(brace, limit, object, first, end, reservation);
        else
            written = writeFieldsBySlotInline(brace, limit, object, first, end, reservation);
        if (written.firstFieldNotWritten != end) [[unlikely]]
            return { next, index, true };
        next = written.next;
        next += next == brace;
        *brace = '{';
        *next++ = '}';
    }
    return { next, index, false };
}

// Space for this many characters after the cursor.
template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::reserve(Cursor& cursor, size_t size)
{
    if (static_cast<size_t>(cursor.limit - cursor.next) >= size) [[likely]]
        return true;
    publishLength(cursor);
    if (size > std::numeric_limits<unsigned>::max() || !hasRemainingCapacity(size)) [[unlikely]] {
        recordBufferFull();
        return false;
    }
    cursor = cursorAtEnd();
    return true;
}

// Space for a comma, the braces and every key, number, boolean and null of an object with these fields. Only a string or a nested value needs more.
template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE std::optional<unsigned> FastStringifier<CharType, bufferMode>::reservationFor(std::span<const AOT::FieldAsProperty> fields)
{
    constexpr size_t maxShortValueLength = std::max<size_t>(AOT::maxJSONNumberLength, maxInt32StringLength);
    constexpr size_t maxReservation = 1 << 30;
    size_t size = 3 + AOT::jsonKeysSize(fields) + fields.size() * maxShortValueLength + AOT::FieldAsProperty::jsonKeyBlockSize;
    if (size > maxReservation) [[unlikely]]
        return std::nullopt;
    return size;
}

// Writes a compiled object whose own properties are these fields. Returns false on failure.
template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::writeListedFields(Cursor& cursor, JSObject& object, std::span<const AOT::FieldAsProperty> fields, unsigned reservation)
{
    constexpr bool writesLatin1 = std::same_as<CharType, Latin1Character>;
    constexpr size_t blockSize = AOT::FieldAsProperty::jsonKeyBlockSize;

    auto appendNewLineAndIndent = [&]() ALWAYS_INLINE_LAMBDA {
        *cursor.next++ = '\n';
        for (unsigned level = 0; level < m_depth; ++level) {
            // All of the gap's array is copied, and the next copy starts where the gap ends.
            for (unsigned i = 0; i < maxGapLength; ++i)
                cursor.next[i] = m_gap[i];
            cursor.next += m_gapLength;
        }
    };
    if (!enterHolder<hasGap>()) [[unlikely]]
        return false;
    if constexpr (hasGap == HasGap::Yes) {
        // A newline and the indent before each member, a space after its colon, and a newline before the closing brace.
        size_t size = reservation + (fields.size() + 1) * static_cast<size_t>(newLineAndIndentSize() + 1) + maxGapLength;
        if (size > std::numeric_limits<unsigned>::max()) [[unlikely]] {
            recordBufferFull();
            return false;
        }
        reservation = size;
    }

    if (!reserve(cursor, reservation)) [[unlikely]]
        return false;
    // Every key starts with a comma. The first comma lands where the opening brace belongs, and the brace overwrites it at the end.
    const size_t positionOfBrace = cursor.next - buffer();
    const bool readsSlots = writesLatin1 && hasGap == HasGap::No && !AOT::PropertyWalk::isInFrame(&object);

    for (const AOT::FieldAsProperty* current = fields.data(), *end = current + fields.size(); ; ++current) {
        if constexpr (writesLatin1 && hasGap == HasGap::No) {
            if (readsSlots && current != end) [[likely]] {
                FieldsWrittenBySlot written = writeFieldsBySlot(cursor.next, cursor.limit, object, current, end, reservation);
                cursor.next = written.next;
                current = written.firstFieldNotWritten;
            }
        }
        if (current == end)
            break;
        auto& field = *current;
        auto key = field.jsonKey;
        if (key.empty()) [[unlikely]] {
            recordFailure("16-bit property name"_s);
            return false;
        }
        // A position, not a pointer: making room for a string may move the buffer.
        const size_t positionOfMember = cursor.next - buffer();
        auto appendKey = [&]() ALWAYS_INLINE_LAMBDA {
            if constexpr (hasGap == HasGap::Yes) {
                // The newline goes between the comma and the quote.
                *cursor.next++ = ',';
                appendNewLineAndIndent();
                key = key.subspan(1);
            }
            if constexpr (writesLatin1) {
                memcpy(cursor.next, key.data(), blockSize);
                if (key.size() > blockSize) [[unlikely]] {
                    for (size_t offset = blockSize; offset < key.size(); offset += blockSize)
                        memcpy(cursor.next + offset, key.data() + offset, blockSize);
                }
            } else {
                static_assert(narrowStride <= blockSize);
                for (size_t offset = 0; offset < key.size(); offset += narrowStride)
                    upconvertEightBytesAndLoad(key.data() + offset, cursor.next + offset);
            }
            cursor.next += key.size();
            if constexpr (hasGap == HasGap::Yes)
                *cursor.next++ = ' ';
        };

        JSValue value;
        if constexpr (writesLatin1) {
            AOT::PropertyWalk::readListedFieldUnboxed(&object, field, [&](auto unboxed) ALWAYS_INLINE_LAMBDA {
                using Unboxed = decltype(unboxed);
                if constexpr (std::same_as<Unboxed, JSValue>)
                    value = unboxed;
                else {
                    appendKey();
                    if constexpr (std::same_as<Unboxed, int32_t>)
                        cursor.next = AOT::appendJSONInteger(cursor.next, unboxed);
                    else if constexpr (std::same_as<Unboxed, double>)
                        cursor.next = AOT::appendJSONNumber(cursor.next, unboxed);
                    else {
                        static_assert(std::same_as<Unboxed, bool>);
                        cursor.next = unboxed ? AOT::appendJSONLiteral(cursor.next, "true"_s) : AOT::appendJSONLiteral(cursor.next, "false"_s);
                    }
                }
            });
        } else
            value = AOT::PropertyWalk::readListedField(&object, field);
        if (!value || value.isUndefined())
            continue;

        if (value.isCell()) {
            JSCell& cell = *value.asCell();
            JSType type = cell.type();
            if (type == StringType) {
                if constexpr (writesLatin1) {
                    // A rope is left to the generic path.
                    const StringImpl* string = asString(&cell)->tryGetValueImpl();
                    if (string && string->is8Bit()) [[likely]] {
                        appendKey();
                        size_t length = string->length();
                        if (!reserve(cursor, 1 + length + 1 + reservation)) [[unlikely]]
                            return false;
                        *cursor.next = '"';
                        if (!stringCopySameType<CharType, true>(string->span8(), cursor.next + 1)) [[likely]] {
                            cursor.next[1 + length] = '"';
                            cursor.next += 1 + length + 1;
                            continue;
                        }
                        cursor.next = buffer() + positionOfMember;
                        key = field.jsonKey;
                    }
                }
            } else if (type == SymbolType)
                continue;
            else if (isFunctionType(type)) {
                if (mayHaveOwnOrInheritedToJSON(*asObject(&cell))) [[unlikely]] {
                    recordFailure("function may have toJSON"_s);
                    return false;
                }
                continue;
            } else if constexpr (writesLatin1 && hasGap == HasGap::No) {
                if (type == ArrayType) {
                    appendKey();
                    switch (writePrimitiveArray(cursor, *asArray(&cell))) {
                    case ArrayResult::Yes:
                        if (!reserve(cursor, reservation)) [[unlikely]]
                            return false;
                        continue;
                    case ArrayResult::Failed:
                        return false;
                    case ArrayResult::No:
                        cursor.next = buffer() + positionOfMember;
                        break;
                    }
                }
                // A compiled object that is fully known continues with this cursor.
                if (type == CompiledObjectType && !AOT::PropertyWalk::hasExtraProperties(asObject(&cell))) {
                    if (!isMemberWrittenByItsFields(*asObject(&cell))) [[unlikely]] {
                        recordFailure("compiled object that is callable, has a prototype of its own or static properties that are not made yet, or may have toJSON"_s);
                        return false;
                    }
                    auto fieldsOfMember = AOT::PropertyWalk::fieldsInKeyOrder(asObject(&cell));
                    auto reservationOfMember = fieldsOfMember ? reservationFor(*fieldsOfMember) : std::nullopt;
                    if (reservationOfMember) [[likely]] {
                        appendKey();
                        if (!writeListedFieldsOfMember(cursor, *asObject(&cell), *fieldsOfMember, *reservationOfMember) || !reserve(cursor, reservation)) [[unlikely]]
                            return false;
                        continue;
                    }
                }
            }
        } else if constexpr (writesLatin1) {
            appendKey();
            if (value.isInt32())
                cursor.next = AOT::appendJSONInteger(cursor.next, value.asInt32());
            else if (value.isTrue())
                cursor.next = AOT::appendJSONLiteral(cursor.next, "true"_s);
            else if (value.isFalse())
                cursor.next = AOT::appendJSONLiteral(cursor.next, "false"_s);
            else if (value.isNull())
                cursor.next = AOT::appendJSONLiteral(cursor.next, "null"_s);
            else {
                ASSERT(value.isDouble());
                cursor.next = AOT::appendJSONNumber(cursor.next, value.asDouble());
            }
            continue;
        }

        appendKey();
        publishLength(cursor);
        append<hasGap>(value);
        if (haveFailure()) [[unlikely]]
            return false;
        cursor = cursorAtEnd();
        if (!reserve(cursor, reservation)) [[unlikely]]
            return false;
    }

    CharType* brace = buffer() + positionOfBrace;
    leaveHolder<hasGap>();
    if constexpr (hasGap == HasGap::Yes) {
        if (cursor.next != brace)
            appendNewLineAndIndent();
    }
    cursor.next += cursor.next == brace;
    *brace = '{';
    *cursor.next++ = '}';
    return true;
}

// Writes an array of strings, numbers, booleans, null and undefined. Writes nothing if there are other elements, or strings that cannot be copied verbatim.
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE auto FastStringifier<CharType, bufferMode>::writePrimitiveArray(Cursor& cursor, JSArray& array) -> ArrayResult
{
    if constexpr (std::same_as<CharType, Latin1Character>) {
        // append() checks the prototype of arrays at the first array.
        if (!m_checkedArrayPrototype || !m_globalObject.isOriginalArrayStructure(array.structure()))
            return ArrayResult::No;
        IndexingType indexingType = array.indexingType();
        if (!hasInt32(indexingType) && !hasContiguous(indexingType))
            return ArrayResult::No;
        auto* butterfly = array.butterfly();
        const unsigned length = butterfly->publicLength();
        if (length > butterfly->vectorLength()) [[unlikely]]
            return ArrayResult::No;

        // A comma, a non-string value and the closing bracket.
        constexpr size_t maxShortElementLength = 1 + std::max<size_t>(AOT::maxJSONNumberLength, maxInt32StringLength) + 1;
        auto elements = butterfly->contiguous();
        const size_t arrayOffset = cursor.next - buffer();
        if (!enterHolder<HasGap::No>() || !reserve(cursor, 2)) [[unlikely]]
            return ArrayResult::Failed;
        auto rollBack = [&] {
            leaveHolder<HasGap::No>();
            cursor.next = buffer() + arrayOffset;
            return ArrayResult::No;
        };
        *cursor.next++ = '[';
        for (unsigned i = 0; i < length; ++i) {
            if (!reserve(cursor, maxShortElementLength)) [[unlikely]]
                return ArrayResult::Failed;
            if (i)
                *cursor.next++ = ',';
            JSValue element = elements.at(&array, i).get();
            if (element.isInt32()) {
                cursor.next = AOT::appendJSONInteger(cursor.next, element.asInt32());
                continue;
            }
            if (!element) [[unlikely]]
                return rollBack();
            if (element.isCell()) {
                if (element.asCell()->type() != StringType)
                    return rollBack();
                const StringImpl* string = asString(element.asCell())->tryGetValueImpl();
                if (!string || !string->is8Bit()) [[unlikely]]
                    return rollBack();
                const size_t stringLength = string->length();
                if (!reserve(cursor, 1 + stringLength + 1 + 1)) [[unlikely]]
                    return ArrayResult::Failed;
                *cursor.next = '"';
                if (stringCopySameType<CharType, true>(string->span8(), cursor.next + 1)) [[unlikely]]
                    return rollBack();
                cursor.next[1 + stringLength] = '"';
                cursor.next += 1 + stringLength + 1;
                continue;
            }
            if (element.isDouble())
                cursor.next = AOT::appendJSONNumber(cursor.next, element.asDouble());
            else if (element.isBoolean())
                cursor.next = AOT::appendJSONBoolean(cursor.next, element.isTrue());
            else if (element.isUndefinedOrNull())
                cursor.next = AOT::appendJSONLiteral(cursor.next, "null"_s);
            else
                return rollBack();
        }
        *cursor.next++ = ']';
        leaveHolder<HasGap::No>();
        return ArrayResult::Yes;
    } else {
        UNUSED_PARAM(cursor);
        UNUSED_PARAM(array);
        return ArrayResult::No;
    }
}

// The checks that appendCompiledObject() makes on an object without extra properties before it writes the fields.
template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::isMemberWrittenByItsFields(JSObject& object)
{
    uint32_t bits = object.structureID().bits();
    if (((m_compiledStructuresKnown & knowsCompiledStructureOfMember) && bits == m_compiledStructureOfMemberWithoutToJSON) || isCompiledStructureKnown(object)) [[likely]]
        return true;
    auto& structure = *object.structure();
    uint32_t known = m_compiledStructureWithoutToJSON;
    uint8_t knownBefore = m_compiledStructuresKnown;
    if (object.isCallable() || structure.hasPolyProto() || structure.hasNonReifiedStaticProperties() || mayCompiledObjectHaveToJSON(object)) [[unlikely]]
        return false;
    // Only a Structure that passed the checks is remembered. If only this object passed them, the next object with its Structure is checked again.
    if (isCompiledStructureKnown(object)) {
        m_compiledStructureOfMemberWithoutToJSON = bits;
        m_compiledStructureWithoutToJSON = known;
        m_compiledStructuresKnown = knownBefore | knowsCompiledStructureOfMember;
    }
    return true;
}

template<typename CharType, BufferMode bufferMode>
NEVER_INLINE bool FastStringifier<CharType, bufferMode>::writeListedFieldsOfMember(Cursor& cursor, JSObject& object, std::span<const AOT::FieldAsProperty> fields, unsigned reservation)
{
    if constexpr (growsBuffer) {
        if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
            if (!hasStackSpaceAtWatchedLevel(&object, currentStackPointer()))
                return false;
        }
    }
    return writeListedFields<HasGap::No>(cursor, object, fields, reservation);
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendListedFields(JSObject& object, std::span<const AOT::FieldAsProperty> fields)
{
    std::optional<unsigned> reservation = reservationFor(fields);
    if (!reservation) [[unlikely]] {
        recordFailure("long property names"_s);
        return;
    }
    Cursor cursor = cursorAtEnd();
    if (writeListedFields<hasGap>(cursor, object, fields, *reservation)) [[likely]]
        publishLength(cursor);
}

// An array's elements are mostly objects with the same Structure and key order, so what is known about one element holds for the next.
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendElementsInCompiledProgram(JSArray& array)
{
    auto* butterfly = array.butterfly();
    unsigned length = butterfly->publicLength();
    auto data = butterfly->contiguous();
    if constexpr (growsBuffer) {
        if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
            // append() has already reported the array.
            if (!hasStackSpaceAtWatchedLevel(jsUndefined(), currentStackPointer()))
                return;
        }
    }

    const JSObject* known = nullptr;
    std::span<const AOT::FieldAsProperty> fields;
    unsigned reservation = 0;
    bool fieldsArePlain = false;
    Cursor cursor = cursorAtEnd();
    bool writesElementsBySlot = std::same_as<CharType, Latin1Character>;
    for (unsigned i = 0; i < length; ++i) {
        if constexpr (std::same_as<CharType, Latin1Character>) {
            if (known && writesElementsBySlot) {
                ElementsWrittenBySlot written = fieldsArePlain ? writeElementsBySlot<true>(cursor.next, cursor.limit, data.data(), i, length, *known, fields, reservation)
                    : writeElementsBySlot<false>(cursor.next, cursor.limit, data.data(), i, length, *known, fields, reservation);
                cursor.next = written.next;
                i = written.firstElementNotWritten;
                writesElementsBySlot = !written.stoppedAtField;
                if (i == length)
                    break;
            }
        }
        JSValue element = data.at(&array, i).get();
        if (!element) [[unlikely]] {
            recordFailure("!canGetIndexQuickly"_s);
            return;
        }
        if (known && element.isCell() && element.asCell()->type() == CompiledObjectType && AOT::PropertyWalk::haveSameFieldsInKeyOrder(known, asObject(element.asCell()))) [[likely]] {
            if constexpr (growsBuffer) {
                if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
                    if (!hasStackSpaceAtWatchedLevel(element, currentStackPointer()))
                        return;
                }
            }
            // The previous element's reservation has space for the comma.
            *cursor.next++ = ',';
            if (!writeListedFields<HasGap::No>(cursor, *asObject(element.asCell()), fields, reservation)) [[unlikely]]
                return;
            continue;
        }

        if (!reserve(cursor, 1)) [[unlikely]]
            return;
        if (i)
            *cursor.next++ = ',';
        publishLength(cursor);
        append<HasGap::No>(element.isUndefined() ? jsNull() : element);
        if (haveFailure()) [[unlikely]]
            return;
        cursor = cursorAtEnd();
        if (!reserve(cursor, 1)) [[unlikely]]
            return;

        known = nullptr;
        if (element.isCell() && element.asCell()->type() == CompiledObjectType && isCompiledStructureKnown(*asObject(element.asCell()))) {
            if (auto listed = AOT::PropertyWalk::fieldsInKeyOrder(asObject(element.asCell()))) {
                if (auto size = reservationFor(*listed)) {
                    known = asObject(element.asCell());
                    fields = *listed;
                    reservation = *size;
                    if constexpr (std::same_as<CharType, Latin1Character>)
                        fieldsArePlain = areFieldsPlain(fields);
                }
            }
        }
    }
    publishLength(cursor);
}
#endif

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendInt32Array(JSArray& array)
{
    auto* butterfly = array.butterfly();
    unsigned length = butterfly->publicLength();
    if (length > butterfly->vectorLength()) [[unlikely]] {
        recordFailure("!lengthBeyondVector"_s);
        return;
    }
    auto data = butterfly->contiguousInt32();
    const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
    for (unsigned i = 0; i < length; ++i) {
        JSValue element = data.at(&array, i).get();
        if (!element) [[unlikely]] {
            recordFailure("!canGetIndexQuickly"_s);
            return;
        }
        if (!hasRemainingCapacity(1 + newLineAndIndent + maxInt32StringLength)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        if (i)
            buffer()[m_length++] = ',';
        if constexpr (hasGap == HasGap::Yes)
            appendNewLineAndIndentUnchecked();
        appendInt32(element.asInt32());
    }
}

// The levels around the value count as those inside it. False with a failure: a static buffer counts none, so it takes no value that could pass the limit before the buffer is full.
template<typename CharType, BufferMode bufferMode>
ALWAYS_INLINE bool FastStringifier<CharType, bufferMode>::startBelow(unsigned levelsAround)
{
    if constexpr (growsBuffer)
        m_depth = levelsAround;
    else if (levelsAround > maximumSideStackRecursion - staticBufferSize) [[unlikely]] {
        recordBufferFull();
        return false;
    }
    return true;
}

template<typename CharType, BufferMode bufferMode>
NEVER_INLINE String FastStringifier<CharType, bufferMode>::stringify(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space, std::optional<FailureReason>& failureReason, unsigned levelsAround)
{
    if (replacer.isObject()) {
        logOutcome("replacer"_s);
        return { };
    }
    FastStringifier stringifier(globalObject);
    if (!space.isUndefined()) {
        if (!stringifier.setGap(space)) [[unlikely]] {
            logOutcome("space"_s);
            return { };
        }
    }
    if (!stringifier.startBelow(levelsAround)) [[unlikely]] {
        failureReason = stringifier.m_failureReason;
        return { };
    }
    if (stringifier.m_gapLength)
        stringifier.append<HasGap::Yes>(value);
    else
        stringifier.append<HasGap::No>(value);
    failureReason = stringifier.m_failureReason;
    return stringifier.result();
}

// A fast case that fails has run no code, so the next one starts over. Null if all of them fail.
// `triedLatin1DynamicBuffer`: that case has failed for this value already.
static String stringifyAfterFirstFastCaseFailed(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space, std::optional<FailureReason> failureReason, unsigned levelsAround = 0, bool triedLatin1DynamicBuffer = false)
{
    if (failureReason == FailureReason::Found16BitEarly) {
        failureReason = std::nullopt;
        if (String result = FastStringifier<char16_t, BufferMode::StaticBuffer>::stringify(globalObject, value, replacer, space, failureReason, levelsAround); !result.isNull())
            return result;

        if (failureReason == FailureReason::BufferFull) {
            failureReason = std::nullopt;
            if (String result = FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason, levelsAround); !result.isNull())
                return result;
        }
    } else if (failureReason == FailureReason::BufferFull) {
        failureReason = std::nullopt;
        // Wrongly guessing 16-bit costs a conversion to 8-bit at the end. Wrongly guessing 8-bit costs all of the work done before the first 16-bit character.
        if (stringifyHints.isWide) {
            if (String result = FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason, levelsAround); !result.isNull())
                return result;
        } else if (!triedLatin1DynamicBuffer) {
            if (String result = FastStringifier<Latin1Character, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason, levelsAround); !result.isNull())
                return result;
        }
    }
    // However much work is thrown away, retrying with 16-bit characters is still much faster than the generic path.
    if (failureReason == FailureReason::Found16BitLate)
        return FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason, levelsAround);
    return { };
}

#if ENABLE(AOT)
// A fast path only succeeds when no user code has to run, so the general path runs none either and must produce the same string.
static NEVER_INLINE void checkAgainstGeneralStringifier(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space, const String& written)
{
    VM& vm = globalObject.vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AOT::countEvent(vm, AOT::RuntimeCounterID::FastPathResultsVerified);
    String general = Stringifier::stringify(globalObject, value, replacer, space);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return;
    }
    if (general != written)
        AOT::crashOnJSONOutputMismatch(written, general);
}
#endif

// Whether the last two Latin-1 results needed a dynamic buffer, i.e. were longer than the static one can hold.
static bool recentResultsOverflowedStaticBuffer(VM& vm)
{
    uint64_t lengths = dynamicBufferLengths<Latin1Character>(vm);
    unsigned smallerLength = std::min(static_cast<unsigned>(lengths), static_cast<unsigned>(lengths >> 32));
    return smallerLength > FastStringifier<Latin1Character, BufferMode::StaticBuffer>::staticBufferSize;
}

static NEVER_INLINE String stringify(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space)
{
    // Nothing is written for undefined, and without an object among the other arguments no code runs that could tell.
    if (value.isUndefined() && !replacer.isObject() && !space.isObject())
        return { };
    VM& vm = globalObject.vm();
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimit());
    if (std::bit_cast<uint8_t*>(currentStackPointer()) >= stackLimit) [[likely]] {
        std::optional<FailureReason> failureReason;
        bool triedLatin1DynamicBuffer = false;
        if (recentResultsOverflowedStaticBuffer(vm)) [[unlikely]] {
            triedLatin1DynamicBuffer = true;
            if (String result = FastStringifier<Latin1Character, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull()) {
#if ENABLE(AOT)
                if (vm.aotState().verifiesFastPathAnswers) [[unlikely]]
                    checkAgainstGeneralStringifier(globalObject, value, replacer, space, result);
#endif
                return result;
            }
            dynamicBufferLengths<Latin1Character>(vm) = 0;
        }
        // A 16-bit string that the dynamic buffer met late is past what the static buffer holds: the cases behind the first go on with 16-bit characters.
        if (failureReason != FailureReason::Found16BitLate) {
            failureReason = std::nullopt;
            if (stringifyHints.isWideInStaticBuffer)
                failureReason = FailureReason::Found16BitEarly;
            else if (String result = FastStringifier<Latin1Character, BufferMode::StaticBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull()) {
#if ENABLE(AOT)
                if (vm.aotState().verifiesFastPathAnswers) [[unlikely]]
                    checkAgainstGeneralStringifier(globalObject, value, replacer, space, result);
#endif
                return result;
            }
        }
        if (String result = stringifyAfterFirstFastCaseFailed(globalObject, value, replacer, space, failureReason, 0, triedLatin1DynamicBuffer); !result.isNull()) {
#if ENABLE(AOT)
            if (vm.aotState().verifiesFastPathAnswers) [[unlikely]]
                checkAgainstGeneralStringifier(globalObject, value, replacer, space, result);
#endif
            return result;
        }
    }
#if ENABLE(AOT)
    if (vm.hasCompiledCode()) [[unlikely]]
        AOT::countEvent(vm, AOT::RuntimeCounterID::JSONStringifySlowPaths);
#endif
    String result = Stringifier::stringify(globalObject, value, replacer, space);
#if FAST_STRINGIFY_LOG_USAGE
    if (!result.isNull())
        dataLogLn("Not fastStringify: ", result);
#endif
    return result;
}

// ------------------------------ JSONObject --------------------------------

const ClassInfo JSONObject::s_info = { "JSON"_s, &JSNonFinalObject::s_info, &jsonTable, nullptr, CREATE_METHOD_TABLE(JSONObject) };

/* Source for JSONObject.lut.h
@begin jsonTable
  parse         jsonProtoFuncParse             DontEnum|Function 2
  stringify     jsonProtoFuncStringify         DontEnum|Function 3
@end
*/

// ECMA 15.8

class Walker {
    WTF_MAKE_NONCOPYABLE(Walker);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    Walker(JSGlobalObject* globalObject, JSString* source, JSObject* function, CallData callData, const JSONRanges* sourceRanges)
        : m_globalObject(globalObject)
        , m_source(source)
        , m_function(function)
        , m_callData(callData)
        , m_sourceRanges(sourceRanges)
    {
    }
    JSValue walk(JSValue unfiltered);
private:
    JSValue callReviver(JSObject* thisObj, JSValue property, JSValue unfiltered, const JSONRanges::Entry* range)
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);

        JSObject* context = nullptr;
        if (m_sourceRanges) {
            context = constructEmptyObject(m_globalObject);
            if (range && !unfiltered.isObject()) {
                JSString* substring = jsSubstring(m_globalObject, m_source, range->range.begin(), range->range.distance());
                RETURN_IF_EXCEPTION(scope, { });
                context->putDirect(vm, vm.propertyNames->source, substring);
            }
        }

        auto args = WTF::toArray<EncodedJSValue>({
            JSValue::encode(property),
            JSValue::encode(unfiltered),
            JSValue::encode(context),
        });
        RELEASE_AND_RETURN(scope, call(m_globalObject, m_function, m_callData, thisObj, ArgList { args.data(), context ? 3u : 2u }));
    }

    friend class Holder;

    JSGlobalObject* m_globalObject;
    JSString* m_source;
    JSObject* m_function;
    CallData m_callData;
    const JSONRanges* m_sourceRanges;
};

enum WalkerState { StateUnknown, ArrayStartState, ArrayStartVisitMember, ArrayEndVisitMember, 
                                 ObjectStartState, ObjectStartVisitMember, ObjectEndVisitMember };
NEVER_INLINE JSValue Walker::walk(JSValue unfiltered)
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Vector<PropertyNameArrayBuilder, 16, UnsafeVectorOverflow> propertyStack;
    Vector<uint32_t, 16, UnsafeVectorOverflow> indexStack;
    MarkedArgumentBuffer markedStack;
    Vector<const JSONRanges::Entry*, 16> entryStack;
    Vector<unsigned, 16, UnsafeVectorOverflow> arrayLengthStack;
    
    Vector<WalkerState, 16, UnsafeVectorOverflow> stateStack;
    WalkerState state = StateUnknown;
    JSValue inValue = unfiltered;
    const JSONRanges::Entry* inValueRange = m_sourceRanges ? &m_sourceRanges->root() : nullptr;
    const JSONRanges::Entry* outValueRange = m_sourceRanges ? &m_sourceRanges->root() : nullptr;
    JSValue outValue = jsNull();
    
    while (1) {
        switch (state) {
            arrayStartState:
            case ArrayStartState: {
                ASSERT(inValue.isObject());
                ASSERT(isArray(m_globalObject, inValue));
                EXCEPTION_ASSERT(!scope.exception());

                if (markedStack.size() >= maximumSideStackRecursion) [[unlikely]]
                    return throwStackOverflowError(m_globalObject, scope);

                JSObject* array = asObject(inValue);
                markedStack.appendWithCrashOnOverflow(array);
                if (m_sourceRanges) {
                    if (inValueRange) {
                        if (!std::holds_alternative<JSONRanges::Array>(inValueRange->properties))
                            inValueRange = nullptr;
                    }
                    entryStack.append(inValueRange);
                }
                uint64_t length = toLength(m_globalObject, array);
                RETURN_IF_EXCEPTION(scope, { });
                if (length > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
                    throwOutOfMemoryError(m_globalObject, scope);
                    return { };
                }
                RETURN_IF_EXCEPTION(scope, { });
                arrayLengthStack.append(static_cast<uint32_t>(length));
                indexStack.append(0);
            }
            arrayStartVisitMember:
            [[fallthrough]];
            case ArrayStartVisitMember: {
                JSObject* array = asObject(markedStack.last());
                uint32_t index = indexStack.last();
                unsigned arrayLength = arrayLengthStack.last();
                if (index == arrayLength) {
                    outValue = array;
                    if (m_sourceRanges)
                        outValueRange = entryStack.takeLast();
                    markedStack.removeLast();
                    arrayLengthStack.removeLast();
                    indexStack.removeLast();
                    break;
                }

                if (isJSArray(array) && array->canGetIndexQuickly(index))
                    inValue = array->getIndexQuickly(index);
                else {
                    inValue = array->get(m_globalObject, index);
                    RETURN_IF_EXCEPTION(scope, { });
                }

                if (m_sourceRanges) {
                    if (auto* last = entryStack.last()) {
                        auto& arrayRangeVector = std::get<JSONRanges::Array>(last->properties);
                        if (index >= arrayRangeVector.size())
                            inValueRange = nullptr;
                        else {
                            inValueRange = &arrayRangeVector[index];
                            bool isSameValue = sameValue(m_globalObject, inValueRange->value, inValue);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (!isSameValue)
                                inValueRange = nullptr;
                        }
                    } else
                        inValueRange = nullptr;
                }

                if (inValue.isObject()) {
                    stateStack.append(ArrayEndVisitMember);
                    goto stateUnknown;
                } else {
                    outValue = inValue;
                    outValueRange = inValueRange;
                }
                [[fallthrough]];
            }
            case ArrayEndVisitMember: {
                JSObject* array = asObject(markedStack.last());
                JSValue filteredValue = callReviver(array, jsString(vm, String::number(indexStack.last())), outValue, outValueRange);
                RETURN_IF_EXCEPTION(scope, { });
                if (filteredValue.isUndefined())
                    array->methodTable()->deletePropertyByIndex(array, m_globalObject, indexStack.last());
                else
                    array->putDirectIndex(m_globalObject, indexStack.last(), filteredValue, 0, PutDirectIndexShouldNotThrow);
                RETURN_IF_EXCEPTION(scope, { });
                indexStack.last()++;
                goto arrayStartVisitMember;
            }
            objectStartState:
            case ObjectStartState: {
                ASSERT(inValue.isObject());
                ASSERT(!isJSArray(inValue));
                if (markedStack.size() >= maximumSideStackRecursion) [[unlikely]]
                    return throwStackOverflowError(m_globalObject, scope);

                JSObject* object = asObject(inValue);
                markedStack.appendWithCrashOnOverflow(object);
                if (m_sourceRanges) {
                    if (inValueRange) {
                        if (!std::holds_alternative<JSONRanges::Object>(inValueRange->properties))
                            inValueRange = nullptr;
                    }
                    entryStack.append(inValueRange);
                }
                indexStack.append(0);
                propertyStack.append(PropertyNameArrayBuilder(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude));
                object->methodTable()->getOwnPropertyNames(object, m_globalObject, propertyStack.last(), DontEnumPropertiesMode::Exclude);
                RETURN_IF_EXCEPTION(scope, { });
            }
            objectStartVisitMember:
            [[fallthrough]];
            case ObjectStartVisitMember: {
                JSObject* object = uncheckedDowncast<JSObject>(markedStack.last());
                uint32_t index = indexStack.last();
                PropertyNameArrayBuilder& properties = propertyStack.last();
                if (index == properties.size()) {
                    outValue = object;
                    if (m_sourceRanges)
                        outValueRange = entryStack.takeLast();
                    markedStack.removeLast();
                    indexStack.removeLast();
                    propertyStack.removeLast();
                    break;
                }
                inValue = object->get(m_globalObject, properties[index]);
                // The holder may be modified by the reviver function so any lookup may throw
                RETURN_IF_EXCEPTION(scope, { });

                if (m_sourceRanges) {
                    if (auto* last = entryStack.last()) {
                        auto iterator = std::get<JSONRanges::Object>(last->properties).find(properties[index].impl());
                        if (iterator != std::get<JSONRanges::Object>(last->properties).end()) {
                            inValueRange = &iterator->value;
                            bool isSameValue = sameValue(m_globalObject, inValueRange->value, inValue);
                            RETURN_IF_EXCEPTION(scope, { });
                            if (!isSameValue)
                                inValueRange = nullptr;
                        } else
                            inValueRange = nullptr;
                    } else
                        inValueRange = nullptr;
                }

                if (inValue.isObject()) {
                    stateStack.append(ObjectEndVisitMember);
                    goto stateUnknown;
                } else {
                    outValue = inValue;
                    outValueRange = inValueRange;
                }
                [[fallthrough]];
            }
            case ObjectEndVisitMember: {
                JSObject* object = uncheckedDowncast<JSObject>(markedStack.last());
                Identifier prop = propertyStack.last()[indexStack.last()];
                JSValue filteredValue = callReviver(object, jsString(vm, prop.string()), outValue, outValueRange);
                RETURN_IF_EXCEPTION(scope, { });
                if (filteredValue.isUndefined())
                    JSCell::deleteProperty(object, m_globalObject, prop);
                else {
                    unsigned attributes;
                    PropertyOffset offset = object->getDirectOffset(vm, prop, attributes);
                    // The reviver was handed this holder and may have made its properties immutable, or laid it above other objects: only the checked path may write then.
                    if (offset != invalidOffset && attributes == static_cast<unsigned>(PropertyAttribute::None) && !object->structure()->mayHaveImmutableProperties() && !(object->structure()->mayBePrototype() && vm.hasCompiledCode())) [[likely]] {
                        object->putDirectOffset(vm, offset, filteredValue);
                        object->structure()->didReplaceProperty(offset);
                    } else {
                        bool shouldThrow = false;
                        object->createDataProperty(m_globalObject, prop, filteredValue, shouldThrow);
                    }
                }
                RETURN_IF_EXCEPTION(scope, { });
                indexStack.last()++;
                goto objectStartVisitMember;
            }
            stateUnknown:
            case StateUnknown: {
                if (inValueRange) {
                    bool isSameValue = sameValue(m_globalObject, inValueRange->value, inValue);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (!isSameValue)
                        inValueRange = nullptr;
                }

                if (!inValue.isObject()) {
                    outValue = inValue;
                    outValueRange = inValueRange;
                    break;
                }
                bool valueIsArray = isArray(m_globalObject, inValue);
                RETURN_IF_EXCEPTION(scope, { });
                if (valueIsArray)
                    goto arrayStartState;
                goto objectStartState;
            }
        }
        if (stateStack.isEmpty())
            break;

        state = stateStack.last();
        stateStack.removeLast();
    }
    JSObject* finalHolder = constructEmptyObject(m_globalObject);
    finalHolder->putDirect(vm, vm.propertyNames->emptyIdentifier, outValue);
    RELEASE_AND_RETURN(scope, callReviver(finalHolder, jsEmptyString(vm), outValue, outValueRange));
}

static NEVER_INLINE JSValue jsonParseSlow(JSGlobalObject* globalObject, JSString* string, StringView view, CallData callData, JSObject* function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSONRanges ranges;
    JSValue unfiltered;
    if (view.is8Bit()) {
        LiteralParser<Latin1Character, JSONReviverMode::Enabled> jsonParser(globalObject, view.span8(), StrictJSON);
        unfiltered = jsonParser.tryLiteralParse(Options::useJSONSourceTextAccess() ? &ranges : nullptr);
        EXCEPTION_ASSERT(!scope.exception() || !unfiltered);
        if (!unfiltered) {
            RETURN_IF_EXCEPTION(scope, { });
            throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
            return { };
        }
    } else {
        LiteralParser<char16_t, JSONReviverMode::Enabled> jsonParser(globalObject, view.span16(), StrictJSON);
        unfiltered = jsonParser.tryLiteralParse(Options::useJSONSourceTextAccess() ? &ranges : nullptr);
        EXCEPTION_ASSERT(!scope.exception() || !unfiltered);
        if (!unfiltered) {
            RETURN_IF_EXCEPTION(scope, { });
            throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
            return { };
        }
    }

    scope.release();
    Walker walker(globalObject, string, function, callData, Options::useJSONSourceTextAccess() ? &ranges : nullptr);
    return walker.walk(unfiltered);
}

// ECMA-262 v5 15.12.2
JSC_DEFINE_HOST_FUNCTION(jsonProtoFuncParse, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* string = callFrame->argument(0).toString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    if (callFrame->argumentCount() >= 2) {
        JSValue function = callFrame->uncheckedArgument(1);
        CallData callData = JSC::getCallData(function);
        if (callData.type != CallData::Type::None)
            RELEASE_AND_RETURN(scope, JSValue::encode(jsonParseSlow(globalObject, string, view, WTF::move(callData), asObject(function))));
    }

#if ENABLE(AOT)
    if (globalObject->aotInstance()) [[unlikely]] {
        if (JSValue parsed = AOT::tryParseJSON(globalObject, view))
            return JSValue::encode(parsed);
    }
#endif

    if (view->is8Bit()) {
        LiteralParser<Latin1Character, JSONReviverMode::Disabled> jsonParser(globalObject, view->span8(), StrictJSON);
        JSValue unfiltered = jsonParser.tryLiteralParse();
        EXCEPTION_ASSERT(!scope.exception() || !unfiltered);
        if (!unfiltered) {
            RETURN_IF_EXCEPTION(scope, { });
            return throwVMError(globalObject, scope, createSyntaxError(globalObject, jsonParser.getErrorMessage()));
        }
        return JSValue::encode(unfiltered);
    }

    LiteralParser<char16_t, JSONReviverMode::Disabled> jsonParser(globalObject, view->span16(), StrictJSON);
    JSValue unfiltered = jsonParser.tryLiteralParse();
    EXCEPTION_ASSERT(!scope.exception() || !unfiltered);
    if (!unfiltered) {
        RETURN_IF_EXCEPTION(scope, { });
        return throwVMError(globalObject, scope, createSyntaxError(globalObject, jsonParser.getErrorMessage()));
    }
    return JSValue::encode(unfiltered);
}

// ECMA-262 v5 15.12.3
JSC_DEFINE_HOST_FUNCTION(jsonProtoFuncStringify, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    String result = stringify(*globalObject, callFrame->argument(0), callFrame->argument(1), callFrame->argument(2));
    return result.isNull() ? encodedJSUndefined() : JSValue::encode(jsString(globalObject->vm(), WTF::move(result)));
}

JSValue JSONParse(JSGlobalObject* globalObject, StringView json)
{
    if (json.isNull())
        return JSValue();

#if ENABLE(AOT)
    if (globalObject->aotInstance()) [[unlikely]] {
        if (JSValue parsed = AOT::tryParseJSON(globalObject, json))
            return parsed;
    }
#endif

    if (json.is8Bit()) {
        LiteralParser<Latin1Character, JSONReviverMode::Disabled> jsonParser(globalObject, json.span8(), StrictJSON);
        return jsonParser.tryLiteralParse();
    }

    LiteralParser<char16_t, JSONReviverMode::Disabled> jsonParser(globalObject, json.span16(), StrictJSON);
    return jsonParser.tryLiteralParse();
}

#if USE(BUN_JSC_ADDITIONS)
StreamingJSONParseResult streamingJSONParse(JSGlobalObject* globalObject, StringView input, MarkedArgumentBuffer& results)
{
    if (input.isNull())
        return { 0, StreamingJSONParseResult::Status::Complete };

    if (input.is8Bit()) {
        LiteralParser<Latin1Character, JSONReviverMode::Disabled> parser(globalObject, input.span8(), StrictJSON);
        return parser.tryStreamingParse(results);
    }

    LiteralParser<char16_t, JSONReviverMode::Disabled> parser(globalObject, input.span16(), StrictJSON);
    return parser.tryStreamingParse(results);
}
#endif // USE(BUN_JSC_ADDITIONS)

JSValue JSONParseWithException(JSGlobalObject* globalObject, StringView json)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (json.isNull())
        return JSValue();

#if ENABLE(AOT)
    if (globalObject->aotInstance()) [[unlikely]] {
        if (JSValue parsed = AOT::tryParseJSON(globalObject, json))
            return parsed;
    }
#endif

    if (json.is8Bit()) {
        LiteralParser<Latin1Character, JSONReviverMode::Disabled> jsonParser(globalObject, json.span8(), StrictJSON);
        JSValue result = jsonParser.tryLiteralParse();
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
        return result;
    }

    LiteralParser<char16_t, JSONReviverMode::Disabled> jsonParser(globalObject, json.span16(), StrictJSON);
    JSValue result = jsonParser.tryLiteralParse();
    RETURN_IF_EXCEPTION(scope, { });
    if (!result)
        throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
    return result;
}

String JSONStringify(JSGlobalObject* globalObject, JSValue value, JSValue space)
{
    return stringify(*globalObject, value, jsNull(), space);
}

String JSONStringify(JSGlobalObject* globalObject, JSValue value, unsigned indent)
{
    return stringify(*globalObject, value, jsNull(), jsNumber(indent));
}

JSC_DEFINE_HOST_FUNCTION(jsonProtoFuncIsRawJSON, (JSGlobalObject*, CallFrame* callFrame))
{
    // https://tc39.es/proposal-json-parse-with-source/#sec-json.israwjson
    return JSValue::encode(jsBoolean(callFrame->argument(0).inherits<JSRawJSONObject>()));
}

JSC_DEFINE_HOST_FUNCTION(jsonProtoFuncRawJSON, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    // https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSString* jsString = callFrame->argument(0).toString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    auto isJSONWhitespace = [](char16_t character) {
        return character == 0x0009 || character == 0x000A || character == 0x000D || character == 0x0020;
    };

    auto view = jsString->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (view->isEmpty()) [[unlikely]] {
        throwSyntaxError(globalObject, scope, "JSON.rawJSON cannot accept empty string"_s);
        return { };
    }

    char16_t firstCharacter = view->codeUnitAt(0);
    if (isJSONWhitespace(firstCharacter)) [[unlikely]] {
        throwSyntaxError(globalObject, scope, makeString("JSON.rawJSON cannot accept string starting with '"_s, firstCharacter, "'"_s));
        return { };
    }

    char16_t lastCharacter = view->codeUnitAt(view->length() - 1);
    if (isJSONWhitespace(lastCharacter)) [[unlikely]] {
        throwSyntaxError(globalObject, scope, makeString("JSON.rawJSON cannot accept string ending with '"_s, lastCharacter, "'"_s));
        return { };
    }

    {
        JSValue result;
        if (view->is8Bit()) {
            LiteralParser<Latin1Character, JSONReviverMode::Disabled> jsonParser(globalObject, view->span8(), StrictJSON);
            result = jsonParser.tryLiteralParsePrimitiveValue();
            RETURN_IF_EXCEPTION(scope, { });
            if (!result) [[unlikely]] {
                throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
                return { };
            }
        } else {
            LiteralParser<char16_t, JSONReviverMode::Disabled> jsonParser(globalObject, view->span16(), StrictJSON);
            result = jsonParser.tryLiteralParsePrimitiveValue();
            RETURN_IF_EXCEPTION(scope, { });
            if (!result) [[unlikely]] {
                throwSyntaxError(globalObject, scope, jsonParser.getErrorMessage());
                return { };
            }
        }
    }
    auto* object = JSRawJSONObject::tryCreate(vm, globalObject->rawJSONObjectStructure(), jsString);
    if (!object) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }

    return JSValue::encode(object);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
