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
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/ParsingUtilities.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringBuilderJSON.h>
#include <wtf/text/StringCommon.h>

#if ENABLE(AOT)
#include "AOTJSONHooks.h"
#include "AOTJSONTextWriter.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeCounters.h"
#include "DateInstance.h"
#include "DateTimeWriter.h"
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
#if ENABLE(AOT)
    static String stringifyProperty(JSGlobalObject&, JSValue, const PropertyNameForFunctionCall&);
#endif

private:
    class Holder {
    public:
        enum RootHolderTag { RootHolder };
        Holder(JSGlobalObject*, JSObject*, Structure*);
        Holder(RootHolderTag, JSObject*);

        JSObject* LEGACY_NODELETE object() const { return m_object; }
        bool LEGACY_NODELETE isArray() const { return m_isArray; }
        bool LEGACY_NODELETE hasFastObjectProperties() const { return m_hasFastObjectProperties; }

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
        // The own properties of a compiled object when the walk began, where they were its fields. The list lives as long as the program.
        std::span<const AOT::FieldAsProperty> m_listedFields;
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
    bool LEGACY_NODELETE isCallableReplacer() const { return m_replacerCallData.type != CallData::Type::None; }

    JSGlobalObject* const m_globalObject;
    JSValue m_replacer;
    bool m_usingArrayReplacer { false };
    PropertyNameArrayBuilder m_arrayReplacerPropertyNames;
    CallData m_replacerCallData;
    String m_gap;

    MarkedArgumentBufferWithSize<16> m_objectStack;
    Vector<Holder, 16, UnsafeVectorOverflow> m_holderStack;
    // The objects of m_holderStack, while the stack is deeper than holderScanLimit.
    UncheckedKeyHashSet<JSObject*> m_holderSet;
    bool m_holderSetActive { false };
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
    RELEASE_AND_RETURN(scope, result.toString());
}

#if ENABLE(AOT)
String Stringifier::stringifyProperty(JSGlobalObject& globalObject, JSValue value, const PropertyNameForFunctionCall& propertyName)
{
    VM& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Stringifier stringifier(&globalObject, jsUndefined(), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });

    StringBuilder result(OverflowPolicy::RecordOverflow);
    Holder root(Holder::RootHolder, nullptr);
    auto stringifyResult = stringifier.appendStringifiedValue(result, value, root, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    if (result.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(&globalObject, scope);
        return { };
    }
    if (stringifyResult != StringifySucceeded) [[unlikely]]
        RELEASE_AND_RETURN(scope, { });
    RELEASE_AND_RETURN(scope, result.toString());
}
#endif

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
// A holder with fewer than this many ancestors finds a cycle by a scan of m_holderStack. Deeper holders use
// m_holderSet, so that a chain of depth N costs O(N) and not O(N^2). The limit is the measured point where
// the scan and the hash lookup cost the same, and is well past the depth of any ordinary document. The set
// is let go once the stack is back at half the limit, so that a document with one deep pocket pays for the
// set only while it is in that pocket, and one that crosses the limit again and again pays one copy of the
// stack per crossing, which costs about one scan at that depth.
constexpr unsigned holderScanLimit = 256;
static_assert(holderScanLimit < maximumSideStackRecursion);
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
    if (!m_holderSetActive) [[likely]] {
        for (unsigned i = 0; i < m_holderStack.size(); i++) {
            if (m_holderStack[i].object() == object) {
                throwTypeError(m_globalObject, scope, "JSON.stringify cannot serialize cyclic structures."_s);
                return StringifyFailed;
            }
        }
        if (m_holderStack.size() >= holderScanLimit) [[unlikely]] {
            // From here on the set stands in for the scan. It holds every object on the stack, in lock-step with it.
            m_holderSet.reserveInitialCapacity(holderScanLimit + 1);
            for (const Holder& holder : m_holderStack)
                m_holderSet.add(holder.object());
            m_holderSet.add(object);
            m_holderSetActive = true;
        }
    } else {
        if (!m_holderSet.add(object).isNewEntry) {
            throwTypeError(m_globalObject, scope, "JSON.stringify cannot serialize cyclic structures."_s);
            return StringifyFailed;
        }
        if (m_holderStack.size() >= maximumSideStackRecursion) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return StringifyFailed;
        }
    }

    bool holderStackWasEmpty = m_holderStack.isEmpty();
    Structure* structure = object->structure();
    m_holderStack.append(Holder(m_globalObject, object, structure));
    m_objectStack.appendWithCrashOnOverflow(object);
    m_objectStack.appendWithCrashOnOverflow(structure);
    ASSERT(!m_holderSetActive || m_holderSet.size() == m_holderStack.size());
    RETURN_IF_EXCEPTION(scope, StringifyFailed);
    if (!holderStackWasEmpty)
        return StringifySucceeded;

    do {
        while (m_holderStack.last().appendNextProperty(*this, builder))
            RETURN_IF_EXCEPTION(scope, StringifyFailed);
        RETURN_IF_EXCEPTION(scope, StringifyFailed);
        if (builder.hasOverflowed()) [[unlikely]]
            return StringifyFailed;
        if (m_holderSetActive) [[unlikely]] {
            m_holderSet.remove(m_holderStack.last().object());
            if (m_holderStack.size() <= holderScanLimit / 2) {
                m_holderSet.clear();
                m_holderSetActive = false;
            }
        }
        m_holderStack.removeLast();
        m_objectStack.removeLast();
        m_objectStack.removeLast();
        ASSERT(!m_holderSetActive || m_holderSet.size() == m_holderStack.size());
    } while (!m_holderStack.isEmpty());
    return StringifySucceeded;
}

inline bool LEGACY_NODELETE Stringifier::willIndent() const
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

inline void LEGACY_NODELETE Stringifier::unindent()
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
// The fields of a compiled object that has no other own property, where each has its key at hand as JSON writes it.
static std::optional<std::span<const AOT::FieldAsProperty>> listedFieldsWithJSONKeys(JSObject* object)
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
            } else if (auto fields = listedFieldsWithJSONKeys(m_object)) {
                m_listedFields = *fields;
                m_size = m_listedFields.size();
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
        if (!m_listedFields.empty()) {
            const AOT::FieldAsProperty& field = m_listedFields[index];
            propertyName = field.name;
            keyAsWritten = field.jsonKey.subspan(1);
            // Code that ran for a member in front may have changed the object.
            auto fieldsNow = AOT::PropertyWalk::fieldsInKeyOrder(m_object);
            if (fieldsNow && fieldsNow->data() == m_listedFields.data()) [[likely]]
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
thread_local static ASCIILiteral reasonOfLastFailure;
#endif

template<typename CharType, BufferMode bufferMode>
class FastStringifier {
public:
    // Returns null string if the fast case fails.
    static String stringify(JSGlobalObject&, JSValue, JSValue replacer, JSValue space, std::optional<FailureReason>&);
#if ENABLE(AOT)
    // False if the fast case fails, and with an exception.
    static bool tryAppendTo(AOT::StringBuilderStorage&, JSGlobalObject&, JSValue, std::optional<FailureReason>&);
#endif

    static constexpr unsigned staticBufferSize = bufferMode == BufferMode::StaticBuffer ? 8192 : 8;
    static constexpr unsigned dynamicBufferInlineCapacity = bufferMode == BufferMode::StaticBuffer ? 0 : 1024;

    static constexpr bool useShortCopyTier = bufferMode == BufferMode::DynamicBuffer;

    // A string that is at least so long is gone through once, escaping as it goes, so much at a time. (A shorter one is copied in the hope that there is nothing to escape.)
    static constexpr unsigned lengthOfLongString = 64;
    static constexpr unsigned pieceOfLongString = bufferMode == BufferMode::StaticBuffer ? 256 : 4096;
    static constexpr unsigned lengthOfBigResult = 128 * 1024;

private:
    explicit FastStringifier(JSGlobalObject&);
    template<HasGap hasGap> void append(JSValue);
    void appendInt32(int32_t);
    template<HasGap hasGap> void appendInt32Array(JSArray&);
#if ENABLE(AOT)
    void appendDate(DateInstance&);
    bool datesHaveBuiltinToJSON(JSObject& datePrototype);
    template<HasGap hasGap> void appendCompiledObject(JSObject&);
    bool mayCompiledObjectHaveToJSON(JSObject&);
    bool mayHaveOwnOrInheritedToJSON(JSObject&);
    // Where the next character goes, and where the buffer ends. While one is in use m_length is out of date.
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
    void appendElementsInCompiledProgram(JSArray&);
    bool isCompiledStructureKnown(const JSObject& object) const { return m_hasCompiledStructureWithoutToJSON && object.structureID().bits() == m_compiledStructureWithoutToJSON; }
#endif
    String result();

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
    template<typename T> void LEGACY_NODELETE recordFailure(T&& reason)
    {
        recordFailure(FailureReason::Unknown, std::forward<T>(reason));
    }
    void recordBufferFull();
    String firstGetterSetterPropertyName(JSObject&) const;
    void recordFastPropertyEnumerationFailure(JSObject&);
    bool haveFailure() const;
    bool hasRemainingCapacity(unsigned size = 1);
    bool hasRemainingCapacitySlow(unsigned size);
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
    // These two take the place of padding: the members behind them are where they are without compiled code.
    bool m_hasCompiledStructureWithoutToJSON { false };
    TriState m_datesHaveBuiltinToJSON { TriState::Indeterminate };
#endif
    std::optional<FailureReason> m_failureReason;
    Vector<CharType, dynamicBufferInlineCapacity, CrashOnOverflow, 16, WTF::StringImplMalloc> m_dynamicBuffer;
    uint8_t* m_stackLimit { nullptr };
#if ENABLE(AOT)
    uint32_t m_compiledStructureWithoutToJSON; // The bits of a StructureID. Written before m_hasCompiledStructureWithoutToJSON is set.
#endif

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
        m_dynamicBuffer.grow(dynamicBufferInlineCapacity);
        m_capacity = dynamicBufferInlineCapacity;
        m_stackLimit = std::bit_cast<uint8_t*>(m_vm.softStackLimit());
    }
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
#if ENABLE(AOT)
    if constexpr (std::same_as<std::remove_cvref_t<T>, ASCIILiteral>) {
        if (!haveFailure())
            reasonOfLastFailure = reason;
    }
#endif
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
    if constexpr (bufferMode == BufferMode::DynamicBuffer)
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
                accumulated = SIMD::bitOr(accumulated, SIMD::equal(SIMD::bitAnd(input, surrogateMask), surrogateCheckMask));
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
                accumulated = SIMD::bitOr(accumulated, SIMD::equal(SIMD::bitAnd(input, surrogateMask), surrogateCheckMask));
            }
        }
        return SIMD::isNonZero(accumulated);
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

#undef JSON_STRING_COPY_HAS_SIMD

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
void FastStringifier<CharType, bufferMode>::append(JSValue value)
{
    if constexpr (bufferMode == BufferMode::DynamicBuffer) {
        if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
            recordFailure(FailureReason::StackOverflow, "stack overflow"_s);
            return;
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
        if (!string.data.is8Bit())
            m_hasSeenWideString = true;
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
            if (cell.type() != FinalObjectType) [[unlikely]] {
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
        if constexpr (hasGap == HasGap::Yes)
            ++m_depth;
        const unsigned newLineAndIndent = hasGap == HasGap::Yes ? newLineAndIndentSize() : 0;
        structure.forEachProperty(m_vm, [&](const auto& entry) -> bool {
            if (entry.attributes() & PropertyAttribute::DontEnum) [[unlikely]] {
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
            auto& name = *entry.key();
            if (name.isSymbol()) [[unlikely]] {
                recordFailure("symbol"_s);
                return false;
            }

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
            if (value.isUndefined())
                return true;

            bool needComma = buffer()[m_length - 1] != '{';
            if (!hasRemainingCapacity(needComma + newLineAndIndent + 1 + span.size() + 2 + (hasGap == HasGap::Yes))) [[unlikely]] {
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

                if (needComma)
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
        if constexpr (hasGap == HasGap::Yes)
            --m_depth;
        bool needNewLine = hasGap == HasGap::Yes && buffer()[m_length - 1] != '{';
        if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
            recordBufferFull();
            return;
        }
        if (needNewLine)
            appendNewLineAndIndentUnchecked();
        buffer()[m_length++] = '}';
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
        if constexpr (hasGap == HasGap::Yes)
            ++m_depth;

        auto closeArray = [&] {
            if constexpr (hasGap == HasGap::Yes)
                --m_depth;
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
                append<hasGap>(element.isUndefined() ? jsNull() : element);
                if (haveFailure()) [[unlikely]]
                    return;
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
        // Not cases of their own: the switch keeps the form it has without them, and a process without compiled code asks only where it has failed.
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
    auto* cursor = buffer() + m_length;
    *cursor++ = '"';
    cursor = writeISODateTime(cursor, time);
    *cursor++ = '"';
    m_length = cursor - buffer();
}

template<typename CharType, BufferMode bufferMode>
bool FastStringifier<CharType, bufferMode>::datesHaveBuiltinToJSON(JSObject& datePrototype)
{
    if (m_datesHaveBuiltinToJSON != TriState::Indeterminate)
        return m_datesHaveBuiltinToJSON == TriState::True;
    bool result = true;
    auto& names = *m_vm.propertyNames;
    for (auto* name : { names.toJSON.impl(), names.toISOString.impl(), names.valueOf.impl(), names.toPrimitiveSymbol.impl() })
        result &= datePrototype.structure()->isImmutablePropertyForReader(name);
    m_datesHaveBuiltinToJSON = triState(result);
    return result;
}

template<typename CharType, BufferMode bufferMode>
template<HasGap hasGap>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendCompiledObject(JSObject& object)
{
    // All of this is asked of the Structure. The one that is remembered has answered.
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
    if constexpr (hasGap == HasGap::Yes)
        ++m_depth;
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
    if constexpr (hasGap == HasGap::Yes)
        --m_depth;
    bool needNewLine = hasGap == HasGap::Yes && buffer()[m_length - 1] != '{';
    if (!hasRemainingCapacity(needNewLine ? 1 + newLineAndIndentSize() : 1)) [[unlikely]] {
        recordBufferFull();
        return;
    }
    if (needNewLine)
        appendNewLineAndIndentUnchecked();
    buffer()[m_length++] = '}';
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
        m_hasCompiledStructureWithoutToJSON = true;
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

// Writes the members of a compiled object that is not in a frame, up to the first one that takes more than its slot, its key and the room that is reserved.
static ALWAYS_INLINE FieldsWrittenBySlot writeFieldsBySlotInline(Latin1Character* next, const Latin1Character* limit, const JSObject& object, const AOT::FieldAsProperty* field, const AOT::FieldAsProperty* end, size_t reservation)
{
    constexpr size_t blockSize = AOT::FieldAsProperty::jsonKeyBlockSize;
    for (; field != end; ++field) {
        const Latin1Character* key = field->jsonKey.data();
        const size_t keySize = field->jsonKey.size();
        // The key is written at once and counts only if a value follows it. Where there is none this copies what lies behind the keys of the list.
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
            // A few other NaNs go with the three words that are no numbers.
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
        case AOT::SlotRepresentation::LiteralString:
        case AOT::SlotRepresentation::LiteralString32:
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

static NEVER_INLINE FieldsWrittenBySlot writeFieldsBySlot(Latin1Character* next, const Latin1Character* limit, const JSObject& object, const AOT::FieldAsProperty* field, const AOT::FieldAsProperty* end, size_t reservation)
{
    return writeFieldsBySlotInline(next, limit, object, field, end, reservation);
}

struct ElementsWrittenBySlot {
    Latin1Character* next;
    unsigned firstElementNotWritten;
    bool stoppedAtField;
};

// Writes the elements behind the first one, each with a comma in front, while they have the fields and the order of keys of the known object. Of the element at which it stops nothing is written.
static NEVER_INLINE ElementsWrittenBySlot writeElementsBySlot(Latin1Character* next, const Latin1Character* limit, const WriteBarrier<Unknown>* elements, unsigned index, unsigned length, const JSObject& known, std::span<const AOT::FieldAsProperty> fields, size_t reservation)
{
    const AOT::FieldAsProperty* end = fields.data() + fields.size();
    for (; index < length; ++index) {
        JSValue element = elements[index].get();
        if (!element || !element.isCell()) [[unlikely]]
            break;
        // Looked at as an object only once it has the Structure of one.
        const JSObject& object = *static_cast<const JSObject*>(element.asCell());
        if (object.structureID() != known.structureID() || !AOT::PropertyWalk::haveSameFieldsInKeyOrder(&known, &object) || AOT::PropertyWalk::isInFrame(&object)) [[unlikely]]
            break;
        if (static_cast<size_t>(limit - next) < 1 + reservation) [[unlikely]]
            break;
        *next = ',';
        // Every key starts with a comma. The first one lands where the brace belongs.
        Latin1Character* brace = next + 1;
        FieldsWrittenBySlot written = writeFieldsBySlotInline(brace, limit, object, fields.data(), end, reservation);
        if (written.firstFieldNotWritten != end) [[unlikely]]
            return { next, index, true };
        next = written.next;
        next += next == brace;
        *brace = '{';
        *next++ = '}';
    }
    return { next, index, false };
}

// Room for so many characters behind the cursor.
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

// Room for a comma, the braces and every key, number, boolean and null of an object with these fields: only a string and a nested value ask for more.
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

// Writes a compiled object whose own properties are these fields. False with a failure.
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
    if constexpr (hasGap == HasGap::Yes) {
        // In front of each member a new line and its indent, behind its colon a space; a new line in front of the closing brace.
        ++m_depth;
        size_t size = reservation + (fields.size() + 1) * static_cast<size_t>(newLineAndIndentSize() + 1) + maxGapLength;
        if (size > std::numeric_limits<unsigned>::max()) [[unlikely]] {
            recordBufferFull();
            return false;
        }
        reservation = size;
    }

    if (!reserve(cursor, reservation)) [[unlikely]]
        return false;
    // Every key starts with a comma. The first one lands where the brace belongs, which is written at the end.
    const size_t positionOfBrace = cursor.next - buffer();
    const bool readsSlots = writesLatin1 && hasGap == HasGap::No && !AOT::PropertyWalk::isInFrame(&object);

    for (const AOT::FieldAsProperty* current = fields.data(), *end = current + fields.size(); ; ++current) {
        if constexpr (writesLatin1 && hasGap == HasGap::No) {
            if (readsSlots) [[likely]] {
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
                // The new line stands between the comma and the quote.
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
                    // A rope is left to the general case.
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
    if constexpr (hasGap == HasGap::Yes) {
        --m_depth;
        if (cursor.next != brace)
            appendNewLineAndIndent();
    }
    cursor.next += cursor.next == brace;
    *brace = '{';
    *cursor.next++ = '}';
    return true;
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

// The elements of an array are mostly objects with one Structure and one order of keys: what is known of one element holds for the next.
template<typename CharType, BufferMode bufferMode>
NEVER_INLINE void FastStringifier<CharType, bufferMode>::appendElementsInCompiledProgram(JSArray& array)
{
    auto* butterfly = array.butterfly();
    unsigned length = butterfly->publicLength();
    auto data = butterfly->contiguous();
    if constexpr (bufferMode == BufferMode::DynamicBuffer) {
        if (std::bit_cast<uint8_t*>(currentStackPointer()) < m_stackLimit) [[unlikely]] {
            recordFailure(FailureReason::StackOverflow, "stack overflow"_s);
            return;
        }
    }

    const JSObject* known = nullptr;
    std::span<const AOT::FieldAsProperty> fields;
    unsigned reservation = 0;
    Cursor cursor = cursorAtEnd();
    bool writesElementsBySlot = std::same_as<CharType, Latin1Character>;
    for (unsigned i = 0; i < length; ++i) {
        if constexpr (std::same_as<CharType, Latin1Character>) {
            if (known && writesElementsBySlot) {
                ElementsWrittenBySlot written = writeElementsBySlot(cursor.next, cursor.limit, data.data(), i, length, *known, fields, reservation);
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
            // The reservation of the element in front has room for the comma.
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

template<typename CharType, BufferMode bufferMode>
inline String FastStringifier<CharType, bufferMode>::stringify(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space, std::optional<FailureReason>& failureReason)
{
    if (replacer.isObject()) {
        logOutcome("replacer"_s);
#if ENABLE(AOT)
        reasonOfLastFailure = "replacer"_s;
#endif
        return { };
    }
    FastStringifier stringifier(globalObject);
    if (!space.isUndefined()) {
        if (!stringifier.setGap(space)) [[unlikely]] {
            logOutcome("space"_s);
#if ENABLE(AOT)
            reasonOfLastFailure = "space"_s;
#endif
            return { };
        }
    }
    if (stringifier.m_gapLength)
        stringifier.append<HasGap::Yes>(value);
    else
        stringifier.append<HasGap::No>(value);
    failureReason = stringifier.m_failureReason;
    return stringifier.result();
}

#if ENABLE(AOT)
template<typename CharType, BufferMode bufferMode>
inline bool FastStringifier<CharType, bufferMode>::tryAppendTo(AOT::StringBuilderStorage& builder, JSGlobalObject& globalObject, JSValue value, std::optional<FailureReason>& failureReason)
{
    FastStringifier stringifier(globalObject);
    stringifier.template append<HasGap::No>(value);
    failureReason = stringifier.m_failureReason;
    if (stringifier.haveFailure())
        return false;
    std::span<const CharType> text { static_cast<const FastStringifier&>(stringifier).buffer(), stringifier.m_length };
    return builder.append(&globalObject, text) == AOT::StringBuilderStorage::AppendResult::Appended;
}
#endif

// A fast case that fails has run no code, so the next one starts over. Null if all of them fail.
static String stringifyAfterFirstFastCaseFailed(JSGlobalObject& globalObject, JSValue value, JSValue replacer, JSValue space, std::optional<FailureReason> failureReason)
{
    if (failureReason == FailureReason::Found16BitEarly) {
        failureReason = std::nullopt;
        if (String result = FastStringifier<char16_t, BufferMode::StaticBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull())
            return result;

        if (failureReason == FailureReason::BufferFull) {
            failureReason = std::nullopt;
            if (String result = FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull())
                return result;
        }
    } else if (failureReason == FailureReason::BufferFull) {
        failureReason = std::nullopt;
        // Wrongly guessing 16-bit costs a conversion to 8-bit at the end. Wrongly guessing 8-bit costs all of the work done before the first 16-bit character.
        if (stringifyHints.isWide) {
            if (String result = FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull())
                return result;
        } else if (String result = FastStringifier<Latin1Character, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull())
            return result;
    }
    // However much work is thrown away, retrying with 16-bit characters is still much faster than the generic path.
    if (failureReason == FailureReason::Found16BitLate)
        return FastStringifier<char16_t, BufferMode::DynamicBuffer>::stringify(globalObject, value, replacer, space, failureReason);
    return { };
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
        if (stringifyHints.isWideInStaticBuffer)
            failureReason = FailureReason::Found16BitEarly;
        else if (String result = FastStringifier<Latin1Character, BufferMode::StaticBuffer>::stringify(globalObject, value, replacer, space, failureReason); !result.isNull())
            return result;
        if (String result = stringifyAfterFirstFastCaseFailed(globalObject, value, replacer, space, failureReason); !result.isNull())
            return result;
    }
#if ENABLE(AOT)
    if (vm.hasCompiledCode()) [[unlikely]] {
        AOT::count(vm, AOT::RuntimeCounterID::JSONStringifySlowPaths);
        AOT::countReasonForSlowStringify(vm, std::bit_cast<uint8_t*>(currentStackPointer()) >= stackLimit ? reasonOfLastFailure : "little stack left"_s);
    }
#endif
    String result = Stringifier::stringify(globalObject, value, replacer, space);
#if FAST_STRINGIFY_LOG_USAGE
    if (!result.isNull())
        dataLogLn("Not fastStringify: ", result);
#endif
    return result;
}

#if ENABLE(AOT)
static String stringifyProperty(JSGlobalObject& globalObject, JSValue value, JSValue property)
{
    VM& vm = globalObject.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (property.isNumber())
        RELEASE_AND_RETURN(scope, Stringifier::stringifyProperty(globalObject, value, PropertyNameForFunctionCall(static_cast<unsigned>(property.asNumber()))));
    Identifier name = asString(property)->toIdentifier(&globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, Stringifier::stringifyProperty(globalObject, value, PropertyNameForFunctionCall(name.impl())));
}

bool AOT::appendJSONTextOfProperty(JSGlobalObject* globalObject, StringBuilderStorage& builder, JSValue value, JSValue property)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<FailureReason> failureReason;
    if (std::bit_cast<uint8_t*>(currentStackPointer()) >= std::bit_cast<uint8_t*>(vm.softStackLimit())) [[likely]] {
        bool isAppended = FastStringifier<Latin1Character, BufferMode::StaticBuffer>::tryAppendTo(builder, *globalObject, value, failureReason);
        RETURN_IF_EXCEPTION(scope, false);
        if (isAppended)
            return true;
    }

    // Only the general serializer calls toJSON, which is told the key.
    String text = stringifyAfterFirstFastCaseFailed(*globalObject, value, jsUndefined(), jsUndefined(), failureReason);
    if (text.isNull())
        text = stringifyProperty(*globalObject, value, property);
    RETURN_IF_EXCEPTION(scope, false);
    if (text.isNull())
        return false;
    auto result = text.is8Bit() ? builder.append(globalObject, text.span8()) : builder.append(globalObject, text.span16());
    RELEASE_AND_RETURN(scope, result == StringBuilderStorage::AppendResult::Appended);
}
#endif

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
                    // The reviver was handed this holder and may have made its properties immutable: only the checked path may write then.
                    if (offset != invalidOffset && attributes == static_cast<unsigned>(PropertyAttribute::None) && !object->structure()->mayHaveImmutableProperties()) [[likely]] {
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

#if ENABLE(AOT)
static ALWAYS_INLINE JSValue tryParseWithCompiledParser(JSGlobalObject* globalObject, StringView text)
{
    if (!globalObject->aotInstance()) [[likely]]
        return { };
    return AOT::tryParseJSON(globalObject, text);
}
#endif

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
    if (JSValue parsed = tryParseWithCompiledParser(globalObject, view))
        return JSValue::encode(parsed);
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
    if (JSValue parsed = tryParseWithCompiledParser(globalObject, json))
        return parsed;
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

JSValue JSONParseWithException(JSGlobalObject* globalObject, StringView json, CompiledJSONParser compiledParser)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (json.isNull())
        return JSValue();

#if ENABLE(AOT)
    if (globalObject->aotInstance() && compiledParser == CompiledJSONParser::Try) [[unlikely]] {
        if (JSValue parsed = AOT::tryParseJSON(globalObject, json))
            return parsed;
    }
#else
    UNUSED_PARAM(compiledParser);
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
